#include "platform/Log.h"
#include "NetworkManager.h"

#include <chrono>
#include <iostream>
#include <stdexcept>

#ifdef WII_PLATFORM
#include <unistd.h>
#elif defined(PS2_PLATFORM)
#include <delaythread.h>
#include "ps2/system/Ps2ThreadPriority.h"
#elif defined(CTR_PLATFORM)
#include <3ds.h>
#include "3ds/DsBootstrap.h"
#include "Packet51MapChunk.h"
#include "Packet0KeepAlive.h"
#endif

#include "NetHandler.h"
#include "Packet.h"
#include "java/JavaNetwork.h"
#include "java/Arithmetic.h"
#include "java/System.h"
#include "platform/PlatformTuning.h"

int_t NetworkManager::field_28145_d[256];
int_t NetworkManager::field_28144_e[256];
std::atomic<int_t> NetworkManager::numReadThreads{0};
std::atomic<int_t> NetworkManager::numWriteThreads{0};
std::mutex NetworkManager::threadSyncObject;

#if defined(CTR_PLATFORM)
namespace
{
// The read queue's entity lane (see NetworkManager.h): the packets that
// create, move, animate, equip, mount, sleep or destroy OTHER entities. Ids
// 20..42 are the same block the PS2's received-entity counter already treats
// as the entity domain (36 and 37 are not registered); 17 (sleep) and 18
// (swing animation) are entity display state too, exactly like 38/40/41/42.
// Everything that must keep its wire order against chunk data (50..54, 60,
// 61, 130..132) or against the login/respawn reset (1, 5, 8, 9, 43, 100..107,
// ...) stays in the main lane.
inline bool isEntityLanePacketId(int_t packetId)
{
	return (packetId >= 20 && packetId <= 42) || packetId == 17 || packetId == 18;
}

// The 3DS split of the read queue's byte budget. The chunk lane keeps the
// historical 4 MB ceiling (MAX_READ_QUEUE_BYTES) for the one traffic class
// that can actually fill it: Packet51 map-chunk floods. Every other packet
// is small enough to be admitted against kMaxSmallQueueBytes instead, so a
// flood that exhausts the chunk budget stops back-pressuring keepalive,
// entity and block-change traffic -- the reader keeps decoding them (and the
// dispatch lanes keep delivering them) while chunk packets hold behind the
// game thread's import cap, exactly as TCP backpressure intends for the
// bulk class alone. 512 KB is generous against real small traffic (the
// entity and keepalive lanes drain fully every tick, and a whole ring of
// Packet50 pre-chunks is a few kilobytes) while still bounding a malicious
// small-packet flood on its own.
constexpr std::size_t kMaxSmallQueueBytes = 512 * 1024;

// Packets this small take the small lane's budget. Chunk-data packets always
// take the chunk lane's regardless of size -- the small lane is reserved for
// the latency-sensitive classes and must not be eroded by cheap sections of
// a chunk flood.
constexpr std::size_t kSmallPacketAdmitBytes = 2048;

// Same classification on both ends of the queue: admission and dispatch must
// agree on which byte budget a packet pays into, or the counters drift.
inline bool isSmallReadQueuePacket(const Packet *packet, std::size_t packetBytes)
{
	return !packet->isChunkDataPacket && packetBytes <= kSmallPacketAdmitBytes;
}
}
#endif

NetworkManager::NetworkManager(const std::string &host, int_t port, const std::string &s, NetHandler *nethandler)
	: networkSocket(JavaNetwork::createSocket())
	, running(true)
	, serverTerminating(false)
	, terminating(false)
	, terminationReason("")
	, netHandler(nethandler)
	, serverHandler(nethandler != nullptr && nethandler->isServerHandler())
	, chunkDataSendCounter(0)
	, timeSinceLastRead(0)
	, sendQueueByteLength(0)
	, readQueueByteLength(0)
	, field_20100_w(50)
{
	if (networkSocket == nullptr || !networkSocket->connect(host, port))
		throw std::runtime_error("Connection refused: " + host + ":" + std::to_string(port));

	remoteSocketAddress = networkSocket->getRemoteSocketAddress();
	socketInputStream = JavaNetwork::createInputStream(*networkSocket);
	socketOutputStream = JavaNetwork::createOutputStream(*networkSocket);
	if (socketInputStream == nullptr || socketOutputStream == nullptr)
		throw std::runtime_error("Could not create network streams");
	socketOutputStream->exceptions(std::ios::badbit | std::ios::failbit);
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
#ifdef PS2_PLATFORM
	constexpr int kNetworkThreadPriority = Ps2ThreadPriority::kNetwork;
#else
	constexpr int kNetworkThreadPriority = 64;
#endif
#if defined(CTR_PLATFORM)
	// The 3DS pins its read/write workers to the second core when the OS
	// granted it (main_3ds.cpp asks at boot): in multiplayer nothing else
	// wants that ARM11 -- the async chunk generator is singleplayer-only --
	// so the socket pair, the 512-byte recv churn and every Packet51 zlib
	// inflate move off the game's core. That is what un-starves the writer:
	// digs and swings had been leaving the console late enough for the
	// server to reject them, which read as "blocks come back and mobs
	// ignore me". Loaders that refuse the CPU-time request fall back to the
	// default core (Thread.cpp keeps that path correct).
	constexpr std::uintptr_t kNetworkThreadAffinity = PLATFORM_NETWORK_THREAD_AFFINITY_MASK;
#else
	constexpr std::uintptr_t kNetworkThreadAffinity = 0;
#endif
	if (!platformReadThread.start(&NetworkManager::platformReadThreadEntry, this, 32 * 1024, kNetworkThreadPriority, kNetworkThreadAffinity))
	{
		networkSocket->close();
		throw std::runtime_error("Could not create network read thread");
	}
	if (!platformWriteThread.start(&NetworkManager::platformWriteThreadEntry, this, 32 * 1024, kNetworkThreadPriority, kNetworkThreadAffinity))
	{
		running = false;
		networkSocket->close();
		platformReadThread.join();
		throw std::runtime_error("Could not create network write thread");
	}
#else
	try
	{
		readThread = std::thread(&NetworkManager::readThreadRun, this);
		writeThread = std::thread(&NetworkManager::writeThreadRun, this);
	}
	catch (...)
	{
		running = false;
		if (networkSocket != nullptr)
			networkSocket->close();
		wakeThreads();
		if (readThread.joinable())
			readThread.join();
		throw;
	}
#endif
	(void)s;
}

NetworkManager::~NetworkManager()
{
	networkShutdown("disconnect.closed", std::vector<std::string>());
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	if (platformReadThread.joinable() && !platformReadThread.isCurrent()) platformReadThread.join();
	if (platformWriteThread.joinable() && !platformWriteThread.isCurrent()) platformWriteThread.join();
#else
	if (closeThread.joinable() && closeThread.get_id() != std::this_thread::get_id())
		closeThread.join();
	if (readThread.joinable() && readThread.get_id() != std::this_thread::get_id())
		readThread.join();
	if (writeThread.joinable() && writeThread.get_id() != std::this_thread::get_id())
		writeThread.join();
#endif
}

void NetworkManager::addToSendQueue(Packet *packet)
{
	std::unique_ptr<Packet> ownedPacket(packet);
	if (ownedPacket == nullptr || serverTerminating || terminating || !running)
		return;

	std::lock_guard<PlatformMutex> guard(sendQueueLock);
	sendQueueByteLength += ownedPacket->getPacketSize() + 1;
	if (ownedPacket->isChunkDataPacket)
		chunkDataPackets.emplace_back(std::move(ownedPacket));
	else
		dataPackets.emplace_back(std::move(ownedPacket));
	wakeThreads();
}

bool NetworkManager::sendPacket()
{
	bool flag = false;
	try
	{
		if (socketOutputStream == nullptr)
			return false;

		std::unique_ptr<Packet> packet;
		{
			std::lock_guard<PlatformMutex> guard(sendQueueLock);
			if (!dataPackets.empty() && (chunkDataSendCounter == 0 || JavaArithmetic::longSub(System::currentTimeMillis(), dataPackets[0]->creationTimeMillis) >= chunkDataSendCounter))
			{
				packet = std::move(dataPackets.front());
				dataPackets.pop_front();
				sendQueueByteLength -= packet->getPacketSize() + 1;
			}
		}
		if (packet != nullptr)
		{
			Packet::writePacket(packet.get(), *socketOutputStream);
			if (!socketOutputStream->good())
				throw std::runtime_error("Failed to write network packet");
			field_28144_e[packet->getPacketId()] += packet->getPacketSize() + 1;
			flag = true;
		}

		std::unique_ptr<Packet> packet1;
		{
			std::lock_guard<PlatformMutex> guard(sendQueueLock);
			if (field_20100_w-- <= 0 && !chunkDataPackets.empty() && (chunkDataSendCounter == 0 || JavaArithmetic::longSub(System::currentTimeMillis(), chunkDataPackets[0]->creationTimeMillis) >= chunkDataSendCounter))
			{
				packet1 = std::move(chunkDataPackets.front());
				chunkDataPackets.pop_front();
				sendQueueByteLength -= packet1->getPacketSize() + 1;
			}
		}
		if (packet1 != nullptr)
		{
			Packet::writePacket(packet1.get(), *socketOutputStream);
			if (!socketOutputStream->good())
				throw std::runtime_error("Failed to write chunk packet");
			field_28144_e[packet1->getPacketId()] += packet1->getPacketSize() + 1;
			field_20100_w = 0;
			flag = true;
		}
	}
	catch (std::exception &exception)
	{
		if (!terminating)
			onNetworkError(exception);
		return false;
	}
	return flag;
}

void NetworkManager::wakeThreads()
{
#if !defined(WII_PLATFORM) && !defined(PS2_PLATFORM) && !defined(CTR_PLATFORM)
	threadSleepCondition.notify_all();
#endif
}

bool NetworkManager::readPacket()
{
	#if defined(PS2_PLATFORM)
	// The PS2 has 32 MB total RAM shared with the rest of the client. Bound a
	// bursty server before queued packets can consume the heap used by chunks.
	constexpr std::size_t MAX_READ_QUEUE_BYTES = 2 * 1024 * 1024;
	constexpr std::size_t MAX_READ_QUEUE_PACKETS = 1024;
	#elif defined(WII_PLATFORM) || defined(CTR_PLATFORM)
	// Wii and 3DS: twice the PS2's slack -- both pack 64 MB of application
	// RAM shared with the rest of the client, so a bursty server may hold a
	// little more but must still stay away from the heap the chunks need.
	constexpr std::size_t MAX_READ_QUEUE_BYTES = 4 * 1024 * 1024;
	constexpr std::size_t MAX_READ_QUEUE_PACKETS = 2048;
	#else
	constexpr std::size_t MAX_READ_QUEUE_BYTES = 32 * 1024 * 1024;
	constexpr std::size_t MAX_READ_QUEUE_PACKETS = 8192;
	#endif
	bool flag = false;
	try
	{
		if (socketInputStream == nullptr)
			return false;

		std::unique_ptr<Packet> packet = Packet::readPacket(*socketInputStream, serverHandler);
		if (packet != nullptr)
		{
			const int_t packetBytesSigned = packet->getPacketSize() + 1;
			if (packetBytesSigned <= 0)
				throw std::runtime_error("Invalid incoming packet size");
			const std::size_t packetBytes = static_cast<std::size_t>(packetBytesSigned);
			field_28145_d[packet->getPacketId()] += packetBytesSigned;
#if PLATFORM_PS2
			if (packet->getPacketId() >= 20 && packet->getPacketId() <= 42)
				receivedEntityPackets.fetch_add(1, std::memory_order_relaxed);
#endif
			if (packetBytes > MAX_READ_QUEUE_BYTES)
				throw std::runtime_error("Incoming packet exceeds queue limit");

#if defined(CTR_PLATFORM)
			// The client's whole keepalive story: the Tab-list ping the player
			// sees is the round trip of THIS packet, and the old path -- queue
			// into the keepalive lane, wait for the game thread's dispatch (a
			// whole tick: 50 ms quiet, 1.7 s under a flood), then the send
			// queue -- added up to seconds whenever the 2048-packet queue cap
			// was pinned or the tick was stretched (the 2026-10-05 session:
			// keepalives dispatched ageMs=1861-2356 with the backlog pinned at
			// 2047 tiny packets). handleKeepAlive does nothing but echo the id
			// back (NetClientHandler.cpp), so the reader can do exactly that
			// right here, at decode time: no lane, no tick, no admission --
			// addToSendQueue only takes the send-queue lock, and the writer
			// thread polls that queue every 2 ms. The game thread never sees a
			// Packet0 on this console. Server-side keepalives (if this console
			// ever hosts) still take the dispatch path below; the guard keeps
			// them untouched.
			lastDecodeTimeMillis.store(System::currentTimeMillis(), std::memory_order_release);
			if (packet->getPacketId() == 0 && !serverHandler)
			{
				Packet0KeepAlive *keepAlive = static_cast<Packet0KeepAlive *>(packet.get());
				addToSendQueue(new Packet0KeepAlive(keepAlive->randomId));
				return true;
			}
#endif

			for (;;)
			{
				{
					std::lock_guard<PlatformMutex> guard(readQueueLock);
#if defined(CTR_PLATFORM)
					// The byte budget is split by packet class (see
					// kMaxSmallQueueBytes): chunk-lane bytes against
					// MAX_READ_QUEUE_BYTES, small packets against their own
					// ceiling, so a chunk flood holding its lane cannot also
					// hold the keepalive/entity traffic behind it. All lanes
					// share the packet-count budget -- the split is a
					// byte-budget split, not extra packets.
					const int_t decodedPacketId = packet->getPacketId();
					const bool smallPacket = isSmallReadQueuePacket(packet.get(), packetBytes);
					const std::size_t laneBytes = smallPacket ? readQueueSmallBytes : readQueueChunkBytes;
					const std::size_t laneBudget = smallPacket ? kMaxSmallQueueBytes : MAX_READ_QUEUE_BYTES;
					if (readPackets.size() + entityPackets.size() + keepalivePackets.size() < MAX_READ_QUEUE_PACKETS &&
					    laneBytes + packetBytes <= laneBudget)
					{
						readQueueByteLength += packetBytes;
						if (smallPacket)
							readQueueSmallBytes += packetBytes;
						else
							readQueueChunkBytes += packetBytes;
						// A login (1) or respawn (9) (re)establishes the world
						// every packet behind it belongs to. Raise the barrier
						// while one is undischarged: entity packets decoded in
						// that window stay in the main lane so they cannot be
						// dispatched ahead of it against the previous (or a
						// null) world. The dispatch loop lowers the barrier
						// once the establishing packet has been processed.
						if (decodedPacketId == 1 || decodedPacketId == 9)
							worldResetPacketsPending++;
						// Packet0 needs none of that protection: it is a pure
						// RTT echo reading no world state, so it may overtake
						// chunk data and the login/respawn barrier alike.
						if (decodedPacketId == 0)
							keepalivePackets.emplace_back(std::move(packet));
						else if (worldResetPacketsPending == 0 &&
						         isEntityLanePacketId(decodedPacketId))
							entityPackets.emplace_back(std::move(packet));
						else
							readPackets.emplace_back(std::move(packet));
						flag = true;
						break;
					}
#else
					if (readPackets.size() < MAX_READ_QUEUE_PACKETS &&
					    readQueueByteLength <= MAX_READ_QUEUE_BYTES &&
					    packetBytes <= MAX_READ_QUEUE_BYTES - readQueueByteLength)
					{
						readQueueByteLength += packetBytes;
						readPackets.emplace_back(std::move(packet));
						flag = true;
						break;
					}
#endif
				}

#if defined(PS2_PLATFORM) || defined(WII_PLATFORM) || defined(CTR_PLATFORM)
				// Do not turn a normal server chunk burst into a disconnect. Holding
				// this one already-decoded packet while the game thread drains the
				// bounded queue applies TCP backpressure and caps the peak at the
				// queue budget plus one protocol-sized packet. The 3DS game thread
				// drains at PS2-like rates, so it takes the PS2's policy too.
#if defined(CTR_PLATFORM)
				// The admission wait is this thread's only idle time while the
				// queue is full -- spend it inflating the oldest still-compressed
				// Packet51 at the queue front (the wedge the decode-time cap
				// left behind) instead of only sleeping: the game thread's
				// dispatch would otherwise pay that same inflate inline inside
				// its tick.
				preInflateFrontQueuedChunk();
#endif
				if (!running || serverTerminating)
					return false;
				sleepThread();
#else
				throw std::runtime_error("Incoming packet queue overflow");
#endif
			}
#if defined(CTR_PLATFORM)
			// After queueing a decode, the front gets first refusal on any
			// free live-inflate slot ahead of the next decode: Packet51s
			// decoded past the cap sit compressed ahead of everything the
			// reader decodes later, so keeping that front inflated is what
			// keeps the game thread's imports cheap (see
			// preInflateFrontQueuedChunk).
			preInflateFrontQueuedChunk();
#endif
		}
		else if (!serverTerminating)
		{
			networkShutdown("disconnect.endOfStream", std::vector<std::string>());
		}
	}
	catch (std::exception &exception)
	{
		if (!terminating)
			onNetworkError(exception);
		return false;
	}
	return flag;
}

#if defined(CTR_PLATFORM)
bool NetworkManager::preInflateFrontQueuedChunk()
{
	// Take the front packet out of the queue only if it is a Packet51 that
	// still holds just its compressed payload. The dispatch may pop whatever
	// lands at the front meanwhile -- an import or two moving ahead of this
	// column is harmless, chunks are position-keyed -- and nothing else can
	// reach the packet while this thread owns it.
	std::unique_ptr<Packet> front;
	{
		std::lock_guard<PlatformMutex> guard(readQueueLock);
		if (readPackets.empty())
			return false;
		Packet *candidate = readPackets.front().get();
		if (candidate == nullptr || candidate->getPacketId() != 51)
			return false;
		Packet51MapChunk *mapChunk = static_cast<Packet51MapChunk *>(candidate);
		if (!mapChunk->needsInflation())
			return false;
		front = std::move(readPackets.front());
		readPackets.pop_front();
	}

	// Inflate outside the lock: this thread owns the packet exclusively here,
	// and the zlib pass is exactly the work being moved off the game thread
	// -- holding readQueueLock through it would just move the stall onto
	// every dispatch pop. preInflate() honours the same live-inflated cap as
	// the decode-time pre-inflate, so the inflated buffers behind the queue
	// stay bounded whichever thread fills them.
	const bool inflated = static_cast<Packet51MapChunk *>(front.get())->preInflate();

	// Back at the front. Byte accounting is untouched on purpose: the queue
	// counts the compressed size (getPacketSize), which inflation does not
	// change. When the cap was full this is a no-op round trip and the next
	// caller retries.
	{
		std::lock_guard<PlatformMutex> guard(readQueueLock);
		readPackets.push_front(std::move(front));
	}
	return inflated;
}
#endif

void NetworkManager::onNetworkError(std::exception &exception)
{
	MC_LOG_ERROR("game", "%s\n", exception.what());
	networkShutdown("disconnect.genericReason", std::vector<std::string>{std::string("Internal exception: ") + exception.what()});
}

void NetworkManager::networkShutdown(const std::string &s, const std::vector<std::string> &aobj)
{
	std::lock_guard<PlatformMutex> shutdownGuard(shutdownLock);
	if (!running)
		return;

	terminationReason = s;
	field_20101_t = aobj;
	terminating = true;
	running = false;
	wakeThreads();

	// Only close the socket here so a blocked recv() in the read thread returns
	// and both threads observe running == false and exit. Do NOT free the stream
	// or socket objects: another thread may still be inside is.get()/flush().
	// They are released in the destructor, after both threads have been joined.
	if (networkSocket != nullptr)
		networkSocket->close();
}

void NetworkManager::processReadPackets()
{
	#ifdef PS2_PLATFORM
	constexpr int_t MAX_SEND_QUEUE_BYTES = 512 * 1024;
	constexpr int_t MAX_PACKETS_PER_TICK = 128;
	#elif defined(CTR_PLATFORM)
	// The PS2's budget, for the same reason it exists there: a server burst
	// (the chunk fan-in right after the spawn teleport) stays queued for the
	// next ticks instead of monopolizing this 268 MHz core into a visible
	// multi-second stall. The reader thread -- now on the second core --
	// keeps decoding ahead regardless, and the smaller send-queue ceiling
	// turns a wedged writer into a prompt disconnect.overflow instead of a
	// many-minute zombie where the player's blocks silently come back.
	constexpr int_t MAX_SEND_QUEUE_BYTES = 512 * 1024;
	constexpr int_t MAX_PACKETS_PER_TICK = 128;
	// Map-chunk imports are the one dispatch with a real per-packet heap
	// cost (a whole resident Chunk column per Packet51). A login flood
	// imported up to 128 columns inside one tick -- tens of MB of transient
	// heap on a 64 MB console, which was the release-session std::bad_alloc
	// (debug.log, 2026-09-28). A handful per tick lets the client-side trim
	// evict behind the flood as it lands; the decoded queue (4 MB) holds
	// the rest, and TCP backpressure stops the server running away.
	//
	// The cap is model-profiled at RUNTIME (this binary runs on both): the
	// New 3DS's 804 MHz ARM11 imports a column in well under half the time
	// the Old model's 268 MHz core needs, and it carries twice the RAM for
	// the transient column, so it can drain a server's chunk stream at
	// twice the rate without turning the flood back into the burst the cap
	// exists to prevent. Nothing else in the 3DS networking stack differs
	// between the models -- soc:U is the same service with the same
	// behaviour on both.
	constexpr int_t CHUNK_PACKETS_PER_TICK_OLD3DS = 6;
	constexpr int_t CHUNK_PACKETS_PER_TICK_NEW3DS = 12;
	const int_t MAX_CHUNK_PACKETS_PER_TICK =
	    dsIsNew3DS() ? CHUNK_PACKETS_PER_TICK_NEW3DS : CHUNK_PACKETS_PER_TICK_OLD3DS;
	// The wall-clock half of the import cap. The counts bound how many columns
	// a tick may import, but the per-column cost is what actually collapses the
	// tick on the Old model -- six ~192 KB section imports (zlib plus the copy
	// into fresh storages, plus the heightmap pass) are far more than a 50 ms
	// tick can carry at 268 MHz, so the count alone let the tick stretch until
	// movement packets went out late and the server read the client as laggy.
	// Once chunk imports alone have spent this budget, the dispatch stops at
	// the front Packet51 with the same re-queue shape as the count cap; at
	// least one import always runs, and a New 3DS still drains several inside
	// it. The reader thread keeps decoding ahead regardless, and TCP
	// backpressure holds the rest server-side.
	constexpr long_t MAX_CHUNK_IMPORT_BUDGET_NS = 10LL * 1000LL * 1000LL;
	#else
	constexpr int_t MAX_SEND_QUEUE_BYTES = 0x100000;
	constexpr int_t MAX_PACKETS_PER_TICK = 1000;
	#endif

	bool sendQueueOverflow;
	{
		std::lock_guard<PlatformMutex> guard(sendQueueLock);
		sendQueueOverflow = sendQueueByteLength > MAX_SEND_QUEUE_BYTES;
	}
	if (sendQueueOverflow)
		networkShutdown("disconnect.overflow", std::vector<std::string>());

	bool empty;
	{
		std::lock_guard<PlatformMutex> guard(readQueueLock);
#if defined(CTR_PLATFORM)
		// Packet0s never queue on this console (echoed at decode, see
		// readPacket), so queue emptiness alone no longer proves the server
		// went silent: an AFK player on a quiet server gets one keepalive a
		// second and nothing else, and a queue-emptiness watchdog would
		// false-fire the 60 s disconnect.timeout on exactly that. Feed the
		// watchdog from the reader's decode activity instead -- any traffic
		// in the last second means the connection is alive even when the
		// lanes sit empty between keepalives. A dead socket stops decoding,
		// the lanes drain, and the 1200-tick watchdog fires as before.
		empty = readPackets.empty() && entityPackets.empty() && keepalivePackets.empty() &&
		    System::currentTimeMillis() - lastDecodeTimeMillis.load(std::memory_order_acquire) >= 1000;
#else
		empty = readPackets.empty();
#endif
	}

	if (empty)
	{
		if (timeSinceLastRead++ == 1200)
			networkShutdown("disconnect.timeout", std::vector<std::string>());
	}
	else
	{
		timeSinceLastRead = 0;
	}

	// Limit packet dispatch work per game tick on PS2. A large burst remains
	// queued for subsequent ticks instead of monopolizing the EE and causing a
	// visible frame hitch.
	int_t chunkImportsThisTick = 0;
#if defined(CTR_PLATFORM)
	// Clock for MAX_CHUNK_IMPORT_BUDGET_NS: starts at the dispatch loop, not
	// at the top of the tick, so the keepalive/overflow bookkeeping above
	// never eats into the import allowance.
	const long_t chunkDispatchStartNs = System::nanoTime();
	// Entity-lane drain cap: a mob-dense area floods the lane with
	// thousands of tiny packets a second, and an unbounded drain eats every
	// one of the 128 dispatch iterations for as long as the flood lasts --
	// Packet51 imports starve for tens of ticks and the world streams in one
	// burst at the end (the "tirones" while chunks load near the farm,
	// 2026-10-05 session). Capping the lane leaves the main lane at least 32
	// iterations every tick so chunks keep landing; the flood itself drains
	// a few ticks slower, which WorldClient::entitySpawnQueue absorbs --
	// entities park until their chunk is resident anyway.
	int_t entityLaneDrainedThisTick = 0;
#endif
	for (int_t i = MAX_PACKETS_PER_TICK; i-- > 0;)
	{
		std::unique_ptr<Packet> packet;
		int_t packetBytes = 0;
		{
			std::lock_guard<PlatformMutex> guard(readQueueLock);
#if defined(CTR_PLATFORM)
			// Keepalive lane first, whole lane per tick: Packet0 is a pure RTT
			// echo (no world state read or written), so it may always run
			// ahead of everything else. Buried in the main lane it waited
			// behind the six-per-tick Packet51 import cap for the length of
			// a chunk flood -- tens of ticks on the Old 3DS -- which the
			// server's ping measurement echoed back and the player list
			// showed as multi-second ping while walking.
			//
			// Entity lane next, whole lane per tick: those packets are a few
			// dozen bytes each and only touch the entity maps, so draining
			// them here costs microseconds and keeps movement, equipment and
			// despawns flowing every tick no matter how deep the capped
			// Packet51 backlog behind them is. On the Old 3DS one chunk
			// import can consume most of the import budget, and without this
			// lane a chunk flood froze every entity for the length of the
			// flood and then snapped them ahead -- the "multiplayer stutters
			// on hardware, perfect in emulator" gap: the emulator drains the
			// same flood in a couple of ticks. Entities that arrive before
			// their chunk data park in WorldClient::entitySpawnQueue, which
			// the protocol already has to tolerate.
			if (!keepalivePackets.empty())
			{
				packet = std::move(keepalivePackets.front());
				keepalivePackets.pop_front();
			}
			else if (!entityPackets.empty() && entityLaneDrainedThisTick < 96)
			{
				++entityLaneDrainedThisTick;
				packet = std::move(entityPackets.front());
				entityPackets.pop_front();
			}
			else
			{
				if (readPackets.empty())
					break;
				// The chunk-import cap: re-queue a Packet51 at the front and
				// stop the dispatch there, so the import cost is spread over
				// ticks and the smaller packets behind it are not starved by a
				// flood that would never yield (see MAX_CHUNK_PACKETS_PER_TICK
				// and the wall-clock MAX_CHUNK_IMPORT_BUDGET_NS).
				if (readPackets.front() != nullptr &&
				    readPackets.front()->getPacketId() == 51 &&
				    (chunkImportsThisTick >= MAX_CHUNK_PACKETS_PER_TICK ||
				     System::nanoTime() - chunkDispatchStartNs >= MAX_CHUNK_IMPORT_BUDGET_NS))
					break;
				packet = std::move(readPackets.front());
				readPackets.pop_front();
			}
#else
			if (readPackets.empty())
				break;
			packet = std::move(readPackets.front());
			readPackets.pop_front();
#endif
			packetBytes = packet != nullptr ? packet->getPacketSize() + 1 : 0;
			if (packetBytes > 0 && static_cast<std::size_t>(packetBytes) <= readQueueByteLength)
				readQueueByteLength -= static_cast<std::size_t>(packetBytes);
			else if (packetBytes > 0)
				readQueueByteLength = 0;
#if defined(CTR_PLATFORM)
			// The split byte budget's lane counters (see readPacket's
			// admission): the same classification the packet was admitted
			// under, so neither counter can drift below zero.
			if (packetBytes > 0)
			{
				const bool smallPacket = isSmallReadQueuePacket(packet.get(), static_cast<std::size_t>(packetBytes));
				std::size_t &laneBytes = smallPacket ? readQueueSmallBytes : readQueueChunkBytes;
				if (static_cast<std::size_t>(packetBytes) <= laneBytes)
					laneBytes -= static_cast<std::size_t>(packetBytes);
				else
					laneBytes = 0;
			}
#endif
		}
#if defined(CTR_PLATFORM)
		if (packet != nullptr && packet->getPacketId() == 51)
			++chunkImportsThisTick;
#endif
		if (packet != nullptr && netHandler != nullptr)
		{
			try
			{
#if PLATFORM_PS2
				if (packet->getPacketId() >= 20 && packet->getPacketId() <= 42)
					MC_LOG_TRACE("net.entity", "dispatch packet=%d received-total=%u ageMs=%lld\n",
						packet->getPacketId(), getReceivedEntityPacketCount(),
						static_cast<long long>(System::currentTimeMillis() - packet->creationTimeMillis));
#endif
#if defined(CTR_PLATFORM)
				const int_t dispatchedPacketId = packet->getPacketId();
#endif
				packet->processPacket(*netHandler);
#if defined(CTR_PLATFORM)
				// The login/respawn has now (re)established the world the
				// packets behind it belong to; entity packets decoded from
				// here on may take the fast lane again. Lowered after
				// processPacket returns so entities decoded while the
				// establishing handler is still running stay conservative
				// (main lane) -- the handler may not have finished wiring the
				// new world.
				if (dispatchedPacketId == 1 || dispatchedPacketId == 9)
				{
					std::lock_guard<PlatformMutex> guard(readQueueLock);
					if (worldResetPacketsPending > 0)
						worldResetPacketsPending--;
				}
#endif
			}
			catch (std::exception &exception)
			{
				onNetworkError(exception);
				std::lock_guard<PlatformMutex> guard(readQueueLock);
				readPackets.clear();
#if defined(CTR_PLATFORM)
				entityPackets.clear();
				keepalivePackets.clear();
				worldResetPacketsPending = 0;
				readQueueSmallBytes = 0;
				readQueueChunkBytes = 0;
#endif
				readQueueByteLength = 0;
				break;
			}
			catch (...)
			{
				std::runtime_error exception("Unhandled exception while processing network packet");
				onNetworkError(exception);
				std::lock_guard<PlatformMutex> guard(readQueueLock);
				readPackets.clear();
#if defined(CTR_PLATFORM)
				entityPackets.clear();
				keepalivePackets.clear();
				worldResetPacketsPending = 0;
				readQueueSmallBytes = 0;
				readQueueChunkBytes = 0;
#endif
				readQueueByteLength = 0;
				break;
			}
		}
	}

	wakeThreads();

	{
		std::lock_guard<PlatformMutex> guard(readQueueLock);
#if defined(CTR_PLATFORM)
		empty = readPackets.empty() && entityPackets.empty() && keepalivePackets.empty();
#else
		empty = readPackets.empty();
#endif
	}
	if (terminating && empty && netHandler != nullptr)
		netHandler->handleErrorMessage(terminationReason, field_20101_t);
}

std::size_t NetworkManager::getReadQueuePacketCount()
{
	std::lock_guard<PlatformMutex> guard(readQueueLock);
#if defined(CTR_PLATFORM)
	return readPackets.size() + entityPackets.size() + keepalivePackets.size();
#else
	return readPackets.size();
#endif
}

std::size_t NetworkManager::getReadQueueByteLength()
{
	std::lock_guard<PlatformMutex> guard(readQueueLock);
	return readQueueByteLength;
}

std::size_t NetworkManager::getSocketReceivedByteCount() const
{
	return networkSocket != nullptr ? networkSocket->getReceivedByteCount() : 0;
}

std::size_t NetworkManager::getSocketSentByteCount() const
{
	return networkSocket != nullptr ? networkSocket->getSentByteCount() : 0;
}

bool NetworkManager::isReadThreadActive() const
{
	return numReadThreads.load(std::memory_order_relaxed) > 0;
}

bool NetworkManager::isWriteThreadActive() const
{
	return numWriteThreads.load(std::memory_order_relaxed) > 0;
}

void NetworkManager::closeConnection()
{
	wakeThreads();
	serverTerminating = true;
	if (networkSocket != nullptr)
		networkSocket->interruptRead();

#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	// The writer closes the connection after the queued disconnect packet has
	// been flushed. interruptRead() only shuts down the receive side here.
#else
	// Java can detach this helper safely because the NetworkManager remains GC-reachable.
	// In C++, keep the delayed closer owned by the manager so it cannot outlive `this`.
	if (!closeThread.joinable())
	{
		closeThread = std::thread([this]()
		{
			std::unique_lock<std::mutex> lock(threadSleepLock);
			threadSleepCondition.wait_for(lock, std::chrono::milliseconds(2000), [this]()
			{
				return !running.load();
			});
			lock.unlock();
			if (running.load())
				networkShutdown("disconnect.closed", std::vector<std::string>());
		});
	}
#endif
}

#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
void *NetworkManager::platformReadThreadEntry(void *argument)
{
	NetworkManager *manager = static_cast<NetworkManager *>(argument);
	try { manager->readThreadRun(); }
	catch (std::exception &exception)
	{
		// Never unwind a C++ exception through the LWP C entry point -- and
		// never let a dead reader look like a laggy one either: surface it
		// as a real disconnect so the player is not left in a zombie world.
		MC_LOG_ERROR("game", "network read thread died: %s\n", exception.what());
		if (!manager->terminating && !manager->serverTerminating)
			manager->onNetworkError(exception);
	}
	catch (...)
	{
		std::runtime_error exception("network read thread crashed");
		MC_LOG_ERROR("game", "%s\n", exception.what());
		if (!manager->terminating && !manager->serverTerminating)
			manager->onNetworkError(exception);
	}
	return nullptr;
}

void *NetworkManager::platformWriteThreadEntry(void *argument)
{
	NetworkManager *manager = static_cast<NetworkManager *>(argument);
	try { manager->writeThreadRun(); }
	catch (std::exception &exception)
	{
		// A dead writer is worse than a dead reader: nothing this client
		// sends ever leaves the console, movement included, while incoming
		// chunks keep the world looking alive. Disconnect visibly instead.
		MC_LOG_ERROR("game", "network write thread died: %s\n", exception.what());
		if (!manager->terminating && !manager->serverTerminating)
			manager->onNetworkError(exception);
	}
	catch (...)
	{
		std::runtime_error exception("network write thread crashed");
		MC_LOG_ERROR("game", "%s\n", exception.what());
		if (!manager->terminating && !manager->serverTerminating)
			manager->onNetworkError(exception);
	}
	return nullptr;
}
#endif

void NetworkManager::readThreadRun()
{
	numReadThreads++;
	try
	{
		while (running && !serverTerminating)
		{
			while (running && !serverTerminating && readPacket())
			{
			}
			sleepThread();
		}
	}
	catch (...)
	{
		numReadThreads--;
		throw;
	}
	numReadThreads--;
}

void NetworkManager::writeThreadRun()
{
	numWriteThreads++;
	try
	{
		while (running)
		{
			while (running && sendPacket())
			{
			}
			try
			{
				if (socketOutputStream != nullptr)
					socketOutputStream->flush();
			}
			catch (std::exception &exception)
			{
				if (!terminating)
					onNetworkError(exception);
				MC_LOG_ERROR("game", "%s\n", exception.what());
			}
#if defined(CTR_PLATFORM)
			// The writer is idle whenever the send queue is dry, and while the
			// socket is quiet this is the only network thread awake at all --
			// the reader parks inside the blocking stream read. Post-flood
			// that is exactly when the compressed Packet51 wedge is still
			// draining, so service one front column per idle pass: the game
			// thread's dispatch would otherwise pay each of those inflates
			// inline inside its tick. A send queued mid-inflate leaves within
			// one inflate (~100 ms on the Old model), far inside what a
			// stretched tick delays it by today.
			if (!serverTerminating)
			{
				try
				{
					preInflateFrontQueuedChunk();
				}
				catch (std::exception &exception)
				{
					// Same contract as the flush above: a failed service (a
					// bad_alloc inflating a column) is a network error, not
					// a thread abort.
					if (!terminating)
						onNetworkError(exception);
					MC_LOG_ERROR("game", "%s\n", exception.what());
				}
			}
#endif
			sleepThread();

			if (serverTerminating && running)
			{
				bool queueEmpty;
				{
					std::lock_guard<PlatformMutex> guard(sendQueueLock);
					queueEmpty = dataPackets.empty() && chunkDataPackets.empty();
				}
				if (queueEmpty)
					networkShutdown("disconnect.closed", std::vector<std::string>());
			}
		}
	}
	catch (...)
	{
		numWriteThreads--;
		throw;
	}
	numWriteThreads--;
}

void NetworkManager::sleepThread()
{
#ifdef WII_PLATFORM
	// A bounded sleep keeps shutdown latency low without std::condition_variable.
	usleep(2000);
#elif defined(CTR_PLATFORM)
	// Same contract as the Wii branch through libctru: a yielding sleep counted
	// in nanoseconds, so the read/write workers keep running while the manager
	// waits. (newlib hides usleep under strict -std=c++17, so the Wii's
	// usleep(2000) is not available here.)
	svcSleepThread(2000 * 1000LL);
#elif defined(PS2_PLATFORM)
	// PS2 libstdc++ does not provide a dependable std::thread/condition_variable
	// backend. Use the EE kernel scheduler directly.
	DelayThread(2000);
#else
	std::unique_lock<std::mutex> lock(threadSleepLock);
	threadSleepCondition.wait_for(lock, std::chrono::milliseconds(2));
#endif
}

bool NetworkManager::isRunning(NetworkManager *networkmanager)
{
	return networkmanager != nullptr && networkmanager->running;
}

bool NetworkManager::isServerTerminating(NetworkManager *networkmanager)
{
	return networkmanager != nullptr && networkmanager->serverTerminating;
}

bool NetworkManager::readNetworkPacket(NetworkManager *networkmanager)
{
	return networkmanager != nullptr && networkmanager->readPacket();
}

bool NetworkManager::sendNetworkPacket(NetworkManager *networkmanager)
{
	return networkmanager != nullptr && networkmanager->sendPacket();
}

bool NetworkManager::isTerminating(NetworkManager *networkmanager)
{
	return networkmanager != nullptr && networkmanager->terminating;
}

void NetworkManager::handleNetworkException(NetworkManager *networkmanager, std::exception &exception)
{
	if (networkmanager != nullptr)
		networkmanager->onNetworkError(exception);
}

std::thread *NetworkManager::getReadThread(NetworkManager *networkmanager)
{
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	(void)networkmanager;
	return nullptr;
#else
	return networkmanager != nullptr ? &networkmanager->readThread : nullptr;
#endif
}

std::thread *NetworkManager::getWriteThread(NetworkManager *networkmanager)
{
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	(void)networkmanager;
	return nullptr;
#else
	return networkmanager != nullptr ? &networkmanager->writeThread : nullptr;
#endif
}

std::ostream *NetworkManager::getSocketOutputStream(NetworkManager *networkmanager)
{
	return networkmanager != nullptr ? networkmanager->socketOutputStream.get() : nullptr;
}
