#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <string>
#include <thread>
#include <vector>
#include <atomic>
#include <iosfwd>

#include "platform/Thread.h"
#include "platform/Mutex.h"

#include "java/Type.h"

namespace JavaNetwork
{
class Socket;
}

class NetHandler;
class Packet;

// net.minecraft.src.NetworkManager
class NetworkManager
{
public:
	NetworkManager(const std::string &host, int_t port, const std::string &s, NetHandler *nethandler);
	~NetworkManager();

	void addToSendQueue(Packet *packet);
	void processReadPackets();
	void wakeThreads();
	void networkShutdown(const std::string &s, const std::vector<std::string> &aobj);
	void serverShutdown() { closeConnection(); }
	void closeConnection();
	void flushQueue() { closeConnection(); }

	unsigned int getReceivedEntityPacketCount() const { return receivedEntityPackets.load(std::memory_order_relaxed); }
	std::size_t getReadQueuePacketCount();
	std::size_t getReadQueueByteLength();
	std::size_t getSocketReceivedByteCount() const;
	std::size_t getSocketSentByteCount() const;
	bool isReadThreadActive() const;
	bool isWriteThreadActive() const;

	static bool isRunning(NetworkManager *networkmanager);
	static bool isServerTerminating(NetworkManager *networkmanager);
	static bool readNetworkPacket(NetworkManager *networkmanager);
	static bool sendNetworkPacket(NetworkManager *networkmanager);
	static bool isTerminating(NetworkManager *networkmanager);
	static void handleNetworkException(NetworkManager *networkmanager, std::exception &exception);
	static std::thread *getReadThread(NetworkManager *networkmanager);
	static std::thread *getWriteThread(NetworkManager *networkmanager);

	static std::mutex threadSyncObject;
	static std::ostream *getSocketOutputStream(NetworkManager *networkmanager);

	static int_t field_28145_d[256];
	static int_t field_28144_e[256];
	static std::atomic<int_t> numReadThreads;
	static std::atomic<int_t> numWriteThreads;

	int_t chunkDataSendCounter;

private:
	bool readPacket();
#if defined(CTR_PLATFORM)
	// Reader/writer-side "wedge" service for queued map chunks. Packet51s
	// decoded while the live-inflated cap (Packet51MapChunk.cpp) was full
	// stay compressed in readPackets, ahead of everything decoded later; the
	// dispatch's inline inflate of that compressed front was what stretched
	// the game tick to hundreds of milliseconds through every chunk stream.
	// This pops the oldest still-compressed Packet51 off the queue front,
	// inflates it on the calling network thread (never the game thread), and
	// puts it back at the front: order, lanes and byte accounting unchanged,
	// no-op when the front is not a compressed Packet51 or the cap is full.
	bool preInflateFrontQueuedChunk();
#endif
	bool sendPacket();
	void onNetworkError(std::exception &exception);
	void readThreadRun();
	void writeThreadRun();
	void sleepThread();
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	static void *platformReadThreadEntry(void *argument);
	static void *platformWriteThreadEntry(void *argument);
#endif

	PlatformMutex sendQueueLock;
	PlatformMutex readQueueLock;
	PlatformMutex shutdownLock;
#if !defined(WII_PLATFORM) && !defined(PS2_PLATFORM) && !defined(CTR_PLATFORM)
	std::mutex threadSleepLock;
	std::condition_variable threadSleepCondition;
#endif
	std::unique_ptr<JavaNetwork::Socket> networkSocket;
	std::unique_ptr<std::istream> socketInputStream;
	std::unique_ptr<std::ostream> socketOutputStream;
	std::string remoteSocketAddress;
	std::atomic_bool running;
	std::atomic_bool serverTerminating;
	std::atomic_bool terminating;
	std::string terminationReason;
	std::vector<std::string> field_20101_t;
	std::deque<std::unique_ptr<Packet>> readPackets;
#if defined(CTR_PLATFORM)
	// The 3DS entity lane of the read queue. The dispatch loop drains this
	// deque before readPackets every tick: entity packets are tiny and touch
	// nothing but the entity maps, so delivering them ahead of the capped
	// Packet51 backlog keeps other players and mobs from freezing for the
	// length of a chunk flood and then snapping (WorldClient::entitySpawnQueue
	// already parks spawns whose chunk is not resident, so entities may
	// legitimately arrive before their chunk data). Everything that must keep
	// its wire order against chunks or against the login/respawn reset stays
	// in readPackets. Guarded by readQueueLock.
	std::deque<std::unique_ptr<Packet>> entityPackets;
	// >0 while a world-(re)establishing packet (id 1 login, id 9 respawn) has
	// been decoded but not yet dispatched. Entity packets decoded in that
	// window must keep their wire order behind it -- an entity spawn that
	// jumps ahead of the establishing packet dispatches against a stale or
	// null world (the "connect to a server" PC=0 data path: a Packet20/23/24
	// spawn reached handleMobSpawn/handleNamedEntitySpawn with
	// NetClientHandler::worldClient still null on first connect). Guarded by
	// readQueueLock.
	int_t worldResetPacketsPending = 0;
	// The 3DS keepalive lane of the read queue, drained in full every tick
	// ahead of the entity lane. Packet0 is a pure RTT echo -- it reads no
	// world state and writes none -- so unlike every other packet it may
	// overtake chunk data (and the login/respawn barrier above: an early
	// echo only improves the server's ping reading, it applies nothing to
	// any world). Without the lane, a keepalive decoded behind a Packet51
	// flood waited in the main lane behind the six-per-tick import cap --
	// tens of ticks, a second and more on the Old 3DS -- and that wait was
	// exactly what the server echoed back as the "ping" of the player list
	// during every chunk stream. Guarded by readQueueLock.
	std::deque<std::unique_ptr<Packet>> keepalivePackets;
	// 3DS split of the read queue's byte budget (see NetworkManager.cpp's
	// kMaxSmallQueueBytes): chunk-lane bytes against the historical
	// MAX_READ_QUEUE_BYTES ceiling, everything small against a much tighter
	// ceiling of its own, so a chunk flood filling its budget cannot make
	// the reader hold keepalive/entity/chat packets that still fit theirs.
	// readQueueByteLength remains the total across both lanes. Guarded by
	// readQueueLock.
	std::size_t readQueueChunkBytes = 0;
	std::size_t readQueueSmallBytes = 0;
	// Liveness basis for disconnect.timeout on this console (see
	// processReadPackets): Packet0s are echoed at decode time and never
	// queue (see readPacket), so queue emptiness alone can no longer prove
	// the server still talks to us -- an AFK player on a quiet server gets
	// one keepalive a second and nothing else, and a queue-emptiness
	// watchdog would false-fire after 60 s of exactly that. This stamps
	// every decode; written from the reader thread, read from the game
	// thread.
	std::atomic<long long> lastDecodeTimeMillis{0};
#endif
	std::deque<std::unique_ptr<Packet>> dataPackets;
	std::deque<std::unique_ptr<Packet>> chunkDataPackets;
	NetHandler *netHandler;
	bool serverHandler;
	std::thread readThread;
	std::thread writeThread;
#if !defined(WII_PLATFORM) && !defined(PS2_PLATFORM) && !defined(CTR_PLATFORM)
	std::thread closeThread;
#endif
#if defined(WII_PLATFORM) || defined(PS2_PLATFORM) || defined(CTR_PLATFORM)
	PlatformThread platformReadThread;
	PlatformThread platformWriteThread;
#endif
	int_t timeSinceLastRead;
	int_t sendQueueByteLength;
	std::size_t readQueueByteLength;
	std::atomic<unsigned int> receivedEntityPackets{0};
	int_t field_20100_w;
};
