// DsCirclePadPro.cpp -- Circle Pad Pro accessory driver for Old 3DS/XL.
//
// WHY ir:USER AND NOT ir:rst
// --------------------------
// The first CPP attempt in this tree forced libctru's hidShouldUseIrrst()
// hook on, betting the accessory would stream through the ir:rst shared
// memory the way the New 3DS internal C-stick does. On real hardware it
// does not: that path is the New model's alone. Every hardware-validated
// implementation of Old-3DS CPP support -- retail compatible cartridges,
// and the open homebrew proven on real consoles (Red Viper's
// source/3ds/cpp.c, which this flow is cross-checked against) -- drives
// the accessory directly over the raw ir:USER protocol instead: the title
// itself becomes the IR master, requests the connection, reads the
// accessory's calibration block and polls "read input" requests. ir:rst is
// not involved at all, which also keeps two services from fighting over
// the console's single IR transceiver.
//
// WHAT THE WORKER DOES
// --------------------
// A single background thread owns one IRNOP session loop:
//
//   initialize -> connect (4 x 14 ms tries, then a 1 s rest)
//              -> calibrate (CRC-8 validated candidates, with the 0x400+
//                 fallback location some accessories store them at)
//              -> stream (read-input requests at the accessory's own
//                 32 ms period while the shared header reports the link)
//
// and on a failure tears the session down and tries the next round -- but
// the retry is BOUNDED. No retail title cycles the IRNOP machinery forever
// (they carry ir:USER only while establishing or holding a link), and the
// 2026-10 Old 2DS crash dump -- a data abort inside the ir system module
// on lid close, parsed from Luma's crash_dump_00000000.dmp -- was taken
// with this worker's endless initialize/connect/teardown loop churning a
// CPP-less console. kProbeRoundsMax rounds without a calibrated link end
// the episode: the session is torn down and the worker parks on the exit
// event (no IRNOP traffic at all) until shutdown() reaps it, with the
// next probe riding the aptStateHook wake/restore re-init or a fresh
// boot. A clip-on while budget remains still links immediately; one
// after it waits for the next wake. Every wait includes the shutdown
// event, so stop is prompt from any state, parked included.
//
// OWNERSHIP / SHUTDOWN
// --------------------
// init() creates the session resources (ir:USER handle, shared block,
// exit event, worker thread); the worker only borrows them and releases
// the two per-cycle events itself. shutdown() signals, joins, then
// releases everything -- called from the normal shutdown path
// (ClientPlatformPolicy_3DS::shutdownFinalize) and from the atexit safety
// net (stopSurvivingWorkerThreads), both idempotent. A join that times
// out leaks the session deliberately: closing handles or freeing memory
// under a still-live thread is the very crash that avoids.
//
// The published state (see the header) is relaxed atomics; dsInputPoll
// folds it into the HID snapshot beside it each frame.
#ifdef CTR_PLATFORM

#include "3ds/input/DsCirclePadPro.h"

#include <3ds.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <malloc.h>

#include "platform/Log.h"

namespace DsCirclePadPro
{

namespace
{

// ---------------------------------------------------------------------
// Protocol constants (3dbrew "Circle Pad Pro" / IRUSER interface; the
// timing cadence matches the hardware-proven homebrew flow cited above).
// ---------------------------------------------------------------------

// IR device ID 1 is the Circle Pad Pro itself.
constexpr std::uint8_t kDeviceIdCirclePadPro = 1;

// ir:USER IRNOP session geometry. The IPC recv parameter carries data plus
// the 8-byte PacketInfo slots (2000 + 0xa0*8 = 3280), the send side
// mirrors it. The one-page shared block lays out exactly as: header 16 +
// recv BufferInfo 16 + recv slots 1280 + recv data 2000 = 3312, then the
// send region (info 16 + slots 256 + data 512 = 784) closing the page at
// 0x1000 -- the arithmetic is why the recv ring boundary below is 2000.
constexpr std::size_t kSharedMemSize = 0x1000;
constexpr std::size_t kRecvDataSize = 2000;
constexpr unsigned kRecvPacketCapacity = 0xa0;
constexpr std::size_t kSendDataSize = 0x200;
constexpr unsigned kSendPacketCapacity = 0x20;
constexpr std::uint8_t kBaudRate = 4; // 3dbrew: the value yields 96000 bps
constexpr std::size_t kIpcRecvBuffSize = kRecvDataSize + kRecvPacketCapacity * 8;
constexpr std::size_t kIpcSendBuffSize = kSendDataSize + kSendPacketCapacity * 8;

// Input request: {id, response period in ms, unknown}. The accessory then
// answers every 32 ms; a 20 ms receive window either catches that answer
// or the attempt times out (four attempts per exchange).
constexpr std::uint8_t kInputRequestId = 0x01;
constexpr std::uint8_t kInputResponsePeriodMs = 0x20;
constexpr std::uint8_t kInputRequestUnknown = 0x87;
constexpr std::uint8_t kInputResponseHead = 0x10;
constexpr u64 kResponseWaitNs = 20ull * 1000000ull;
constexpr int kExchangeAttempts = 4;

// Connection: 14 ms for the accessory to raise its status event, four
// tries in quick succession, then a full second's rest -- hammering the
// IR bus while the CPP is asleep or clipped off serves nothing.
constexpr u64 kConnectWaitNs = 14ull * 1000000ull;
constexpr int kConnectAttempts = 4;
constexpr u64 kRetryRestNs = 1000ull * 1000000ull;

// Probe budget: rounds without a calibrated link before the worker parks
// (see workerMain). A present, awake accessory answers round one, so this
// window only exists for the clip-on-at-boot and after-wake cases; no
// retail title cycles IRNOP indefinitely, and the endless retry this
// replaces was the prime suspect of the ir-module crash (file header).
constexpr int kProbeRoundsMax = 15;

// shared-memory header.connection_status while the link carries input.
constexpr std::uint8_t kConnectionStatusStreaming = 2;

// Input response (6 bytes): id, two contiguous little-endian 12-bit axes
// in three bytes, then a flag byte -- battery in bits 0-4 and the three
// button bits which read SET while the button is NOT pressed.
constexpr int kInputResponseSize = 6;
constexpr std::uint8_t kBatteryMask = 0x1F;
constexpr std::uint8_t kFlagZlReleased = 1u << 5;
constexpr std::uint8_t kFlagZrReleased = 1u << 6;
constexpr std::uint8_t kFlagRReleased = 1u << 7;

// Calibration: {id, expected response time in ms, offset, size}. One read
// of 0x40 bytes covers four 16-byte candidate blocks; the 0x400+ region is
// the fallback location some accessories store theirs at.
constexpr std::uint8_t kCalibrationRequestId = 0x02;
constexpr std::uint8_t kCalibrationResponseHead = 0x11;
constexpr std::uint8_t kCalibrationResponseTimeMs = 100;
constexpr std::uint16_t kCalibrationReadSize = 0x40;
constexpr std::uint16_t kCalibrationFallbackBase = 0x400;
constexpr int kCalibrationFallbackSpan = 0x40;
constexpr int kCalibrationCandidates = 4;
// id(1) + offset(2) + size(2) + candidates(4 x 16).
constexpr int kCalibrationResponseSize = 1 + 2 + 2 + kCalibrationCandidates * 16;

// The right pad reports in the left circle pad's range once calibrated
// (subtract centre, scale, divide by 8 -- 3dbrew), so DsInput's
// kCirclePadMax normalization applies unchanged downstream.
constexpr int kPadScaleDivisor = 8;

// Worker thread shape. Priority 0x38 matches the reference implementation
// (s4ammy's retail-binary patch, which creates the same worker through the
// game SDK's thread call): deliberately BELOW the app's own threads, which
// sit around 0x30 -- on the Old 3DS/2DS everything runs on one core, and an
// accessory poller that outranks the game is exactly the scheduling
// pressure the 2026-10 network regression taught this port to fear. The
// waits are event-driven, so a lower priority costs nothing but a little
// input latency the frame loop already absorbs. Core 0 like every other
// input path, an 8 KiB stack -- the reference manages with 4 KiB, but this
// worker also formats log lines.
constexpr int kWorkerPriority = 0x38;
constexpr int kWorkerCore = 0;
constexpr std::size_t kWorkerStackSize = 0x2000;
// Generous: the worst-case exit path is one 1 s rest plus a handful of
// 20 ms waits. A timeout means an IPC never returned and we leak instead.
constexpr u64 kJoinTimeoutNs = 5000ull * 1000000ull;

// ---------------------------------------------------------------------
// Session state: created by init(), released by shutdown() once the
// worker is joined. The worker borrows all of it.
// ---------------------------------------------------------------------

Handle g_irUser = 0;
Handle g_memBlock = 0;
Handle g_exitEvent = 0;
void* g_sharedMem = nullptr; // one page, memalign'd
Thread g_thread = nullptr;
bool g_started = false;      // init() completed; guards init()/shutdown()

// The oldest unconsumed PacketInfo slot. Reset whenever the ring is
// (re)initialized; advanced only when the IR module acknowledges a
// release. Starts at zero so a reconnect never walks stale slots the way
// a persistent cursor would.
unsigned g_recvCursor = 0;

// Probe budget and per-episode link diagnostics. g_probeRounds counts
// consecutive rounds without a CALIBRATED link -- nothing answered, or a
// device answered whose calibration never validated -- because either way
// the IRNOP machinery is the thing cycling. init() resets it for a fresh
// worker, a calibrated link resets it mid-session, and reaching
// kProbeRoundsMax parks the worker (workerMain). The calibration WARN
// separates the two failure classes in a user's debug.log (the 2026-10-07
// report): "linked but calibration failed" is a protocol or device
// problem, not a clip-off / battery-dead / asleep console. It logs once
// per episode; level-0 builds compile it out entirely -- the cppe overlay
// marker covers that case (startError, above).
int g_probeRounds = 0;
bool g_calibrationFailedLogged = false;

// Published state (worker -> game thread). The position packs into one
// word so a sample can never tear axes from two updates, and the
// connected flag carries a release so readers that see it also see the
// data written before it.
std::atomic<std::uint32_t> g_packedPos{0}; // dx:16 | dy:16
std::atomic<std::uint32_t> g_held{0};
std::atomic<std::uint8_t> g_battery{0};
std::atomic<bool> g_connected{false};

// Last start-attempt outcome, for the startAttempted()/startError()
// diagnostic surface (see the header): written by init() on its own thread
// context (the main thread, from dsInputInit/dsInputPoll) and read from the
// debug-line formatter, so both are atomic. Deliberately NOT cleared by
// shutdown(): a worker that never started must keep saying why.
std::atomic<bool> g_startAttempted{false};
std::atomic<std::uint32_t> g_startError{0};

void publishLive(std::int16_t dx, std::int16_t dy, std::uint32_t held, std::uint8_t battery)
{
	const std::uint32_t packed =
	    (static_cast<std::uint32_t>(static_cast<std::uint16_t>(dx))) |
	    (static_cast<std::uint32_t>(static_cast<std::uint16_t>(dy)) << 16);
	g_packedPos.store(packed, std::memory_order_relaxed);
	g_held.store(held, std::memory_order_relaxed);
	g_battery.store(battery, std::memory_order_relaxed);
	g_connected.store(true, std::memory_order_release);
}

void publishIdle()
{
	// Data first, flag last -- mirrors publishLive: a sample() landing
	// between the stores must never see connected==false with stale held
	// bits (DsInput folds held unconditionally, which would phantom-press
	// ZL/ZR/R for one frame on link loss).
	g_packedPos.store(0, std::memory_order_relaxed);
	g_held.store(0, std::memory_order_relaxed);
	g_battery.store(0, std::memory_order_relaxed);
	g_connected.store(false, std::memory_order_release);
}

// ---------------------------------------------------------------------
// Shared-memory layout (3dbrew IRUSER): a header the IR module owns,
// then the recv ring (BufferInfo, PacketInfo slots, packet data), then
// the send ring. Read-only from here.
// ---------------------------------------------------------------------

struct SharedHeader
{
	std::uint32_t latestRecvError;
	std::uint32_t latestSendError;
	std::uint8_t connectionStatus;
	std::uint8_t tryingToConnect;
	std::uint8_t connectionRole;
	std::uint8_t machineId;
	std::uint8_t connected;
	std::uint8_t networkId;
	std::uint8_t initialized;
	std::uint8_t _pad; // keeps the struct 4-aligned behind the u32s
};

struct BufferInfo
{
	std::uint32_t beginIndex;
	std::uint32_t endIndex;
	std::uint32_t packetCount;
	std::uint32_t unknown;
};

struct PacketInfo
{
	std::uint32_t offset;
	std::uint32_t size;
};

struct SharedMemory
{
	SharedHeader header;                      // 0x000
	BufferInfo recvInfo;                      // 0x010
	PacketInfo recvPackets[kRecvPacketCapacity]; // 0x020 .. 0x520
};                                            // packet data follows

static_assert(sizeof(SharedHeader) == 16, "IRNOP header is two u32s and eight u8s");
static_assert(sizeof(SharedMemory) == 32 + kRecvPacketCapacity * 8,
              "recv metadata must end exactly where the packet data begins");

// Request payloads, sized exactly as they go on the wire.
struct InputRequest
{
	std::uint8_t id;
	std::uint8_t responsePeriod;
	std::uint8_t unknown;
};

struct CalibrationRequest
{
	std::uint8_t id;
	std::uint8_t responseTime;
	std::uint16_t offset;
	std::uint16_t size;
};

static_assert(sizeof(InputRequest) == 3, "input request is three bytes");
static_assert(sizeof(CalibrationRequest) == 6, "u8 + u8 + u16 + u16 packs to six bytes");

constexpr InputRequest kInputRequest{kInputRequestId, kInputResponsePeriodMs,
                                     kInputRequestUnknown};

// ---------------------------------------------------------------------
// CRC-8 (poly 0x07, init 0): the IR module appends each framed packet's
// CRC as its last byte, so the CRC over the whole frame must be zero.
// The standard table, generated at compile time rather than pasted.
// ---------------------------------------------------------------------

struct Crc8Table
{
	std::uint8_t entry[256];
	constexpr Crc8Table() : entry{}
	{
		for (unsigned i = 0; i < 256; ++i)
		{
			std::uint8_t r = static_cast<std::uint8_t>(i);
			for (int bit = 0; bit < 8; ++bit)
				r = static_cast<std::uint8_t>((r & 0x80) ? ((r << 1) ^ 0x07) : (r << 1));
			entry[i] = r;
		}
	}
};

constexpr Crc8Table kCrc8{};

std::uint8_t crc8(const std::uint8_t* data, std::size_t len)
{
	std::uint8_t crc = 0;
	for (std::size_t i = 0; i < len; ++i)
		crc = kCrc8.entry[crc ^ data[i]];
	return crc;
}

// Same checksum walked over a ring buffer: the segment may wrap past the
// end of the packet data back to its start.
std::uint8_t crc8Ring(const std::uint8_t* ring, std::size_t ringSize,
                      const std::uint8_t* pos, std::size_t len)
{
	std::uint8_t crc = 0;
	const std::uint8_t* cursor = pos;
	const std::uint8_t* const end = ring + ringSize;
	for (std::size_t i = 0; i < len; ++i)
	{
		crc = kCrc8.entry[crc ^ *cursor++];
		if (cursor == end)
			cursor = ring;
	}
	return crc;
}

// ---------------------------------------------------------------------
// ir:USER IPC (command IDs per 3dbrew's IRUSER interface).
// ---------------------------------------------------------------------

Result initializeIrnop()
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0x18, 6, 2); // 0x180182
	c[1] = static_cast<u32>(kSharedMemSize);
	c[2] = static_cast<u32>(kIpcRecvBuffSize);
	c[3] = kRecvPacketCapacity;
	c[4] = static_cast<u32>(kIpcSendBuffSize);
	c[5] = kSendPacketCapacity;
	c[6] = kBaudRate;
	c[7] = 0; // static-buffer descriptor: the handle below
	c[8] = g_memBlock;
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

Result finalizeIrnop()
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0x2, 0, 0); // 0x20000
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

Result clearReceiveBuffer()
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0x3, 0, 0); // 0x30000
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

Result clearSendBuffer()
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0x4, 0, 0); // 0x40000
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

Result requireConnection(std::uint8_t deviceId)
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0x6, 1, 0); // 0x60040
	c[1] = deviceId;
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

Result disconnect()
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0x9, 0, 0); // 0x90000
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

Result getReceiveEvent(Handle* out)
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0xA, 0, 0); // 0xA0000
	const Result r = svcSendSyncRequest(g_irUser);
	if (R_FAILED(r))
		return r;
	*out = static_cast<Handle>(c[3]);
	return static_cast<Result>(c[1]);
}

Result getConnectionStatusEvent(Handle* out)
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0xC, 0, 0); // 0xC0000
	const Result r = svcSendSyncRequest(g_irUser);
	if (R_FAILED(r))
		return r;
	*out = static_cast<Handle>(c[3]);
	return static_cast<Result>(c[1]);
}

Result sendIrnop(const void* data, std::uint32_t size)
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0xD, 1, 2); // 0xD0042
	c[1] = size;
	c[2] = (size << 14) | 2;
	c[3] = static_cast<u32>(reinterpret_cast<std::uintptr_t>(data));
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

Result releaseSharedData(std::uint32_t count)
{
	u32* c = getThreadCommandBuffer();
	c[0] = IPC_MakeHeader(0x19, 1, 0); // 0x190040
	c[1] = count;
	const Result r = svcSendSyncRequest(g_irUser);
	return R_FAILED(r) ? r : static_cast<Result>(c[1]);
}

// ---------------------------------------------------------------------
// Recv ring reader.
// ---------------------------------------------------------------------

void clearPacket()
{
	// Advance the cursor only on acknowledgement: a failed release keeps
	// the packet queued, and re-reading the same slot beats skipping one
	// the module still counts as pending.
	if (R_FAILED(releaseSharedData(1)))
		return;
	if (++g_recvCursor >= kRecvPacketCapacity)
		g_recvCursor = 0;
}

// Read the framed packet at the cursor into out. Returns the frame's full
// content length (0 = nothing usable); the packet is always consumed, so a
// bad or oversized frame costs one poll, never a stuck ring.
std::uint32_t readPacket(void* out, std::uint32_t outLen)
{
	auto* mem = static_cast<SharedMemory*>(g_sharedMem);
	if (mem->recvInfo.packetCount == 0)
		return 0;

	const PacketInfo info = mem->recvPackets[g_recvCursor];
	// A desynced cursor (ring cleared under us, stale slot) must cost a
	// dropped packet, never an out-of-bounds walk.
	if (info.size == 0 || info.size > kRecvDataSize || info.offset >= kRecvDataSize)
	{
		clearPacket();
		return 0;
	}

	const auto* const ring = reinterpret_cast<const std::uint8_t*>(mem) + sizeof(SharedMemory);
	const std::uint8_t* const ringEnd = ring + kRecvDataSize;
	const std::uint8_t* cursor = ring + info.offset;

	if (crc8Ring(ring, kRecvDataSize, cursor, info.size) != 0)
	{
		clearPacket();
		return 0;
	}

	// Frame header: four bytes, then the content. Length lives in header
	// byte 2 (0x3F mask); with its 0x40 bit set byte 3 extends it to 16
	// bits, otherwise byte 3 was already the first content byte.
	std::uint8_t header[4];
	for (int i = 0; i < 4; ++i)
	{
		header[i] = *cursor++;
		if (cursor == ringEnd)
			cursor = ring;
	}

	int contentLength = header[2] & 0x3F;
	if ((header[2] & 0x40) != 0)
	{
		contentLength = (contentLength << 8) | header[3];
	}
	else
	{
		if (cursor == ring)
			cursor = ringEnd;
		--cursor;
	}

	const int limit = static_cast<int>(outLen);
	const int copied = contentLength < limit ? contentLength : limit;
	auto* bytes = static_cast<std::uint8_t*>(out);
	for (int i = 0; i < copied; ++i)
	{
		bytes[i] = *cursor++;
		if (cursor == ringEnd)
			cursor = ring;
	}

	// Consume unconditionally. A frame whose content did not fit is
	// unusable by definition (every expected response has a buffer sized
	// for it); leaving it queued would wedge the cursor on it forever,
	// with every future read re-reporting the same mismatch until the
	// link drops.
	clearPacket();
	return static_cast<std::uint32_t>(contentLength);
}

// ---------------------------------------------------------------------
// Request/response exchange: send, wait on {recv event, exit event},
// parse the answer. Four attempts inside; the outcome says what to do
// with the link afterwards.
// ---------------------------------------------------------------------

enum class Exchange
{
	Ok,
	Timeout,   // link trouble -> full reconnect
	ReadError, // garbled frame -> keep the link, poll again
	Exit,      // shutdown requested
};

Exchange exchange(Handle recvEvent, const void* request, std::uint32_t requestSize,
                  void* response, std::uint32_t responseSize, std::uint8_t expectedHead)
{
	Handle waitHandles[2] = {recvEvent, g_exitEvent};
	s32 which = -1;
	Exchange outcome = Exchange::Timeout;
	for (int attempt = 0; attempt < kExchangeAttempts; ++attempt)
	{
		if (R_FAILED(sendIrnop(request, requestSize)))
		{
			outcome = Exchange::ReadError;
			continue;
		}
		const Result waited =
		    svcWaitSynchronizationN(&which, waitHandles, 2, false, kResponseWaitNs);
		if (R_FAILED(waited))
		{
			// Timeout is the expected miss; any other wait failure is
			// treated the same -- the reconnect path recovers either.
			outcome = Exchange::Timeout;
			continue;
		}
		if (which == 1)
			return Exchange::Exit;
		const std::uint32_t got = readPacket(response, responseSize);
		if (got == responseSize &&
		    static_cast<const std::uint8_t*>(response)[0] == expectedHead)
			return Exchange::Ok;
		outcome = Exchange::ReadError;
	}
	return outcome;
}

// ---------------------------------------------------------------------
// Calibration: four 16-byte candidate blocks, each self-validating over
// its own CRC-8. Byte 0 is unknown; bytes 1-3 carry two contiguous
// little-endian 12-bit fields (x centre low, y centre high), then two
// f32 scales, three pad bytes and the CRC.
// ---------------------------------------------------------------------

struct Calibration
{
	std::uint16_t xCentre = 0;
	std::uint16_t yCentre = 0;
	float xScale = 1.0f;
	float yScale = 1.0f;
};

bool tryCandidate(const std::uint8_t* cand, Calibration* out)
{
	if (crc8(cand, 15) != cand[15])
		return false;
	Calibration cal;
	cal.xCentre = static_cast<std::uint16_t>(
	    (cand[1] | ((cand[2] & 0x0F) << 8)) & 0x0FFF);
	cal.yCentre = static_cast<std::uint16_t>(
	    (((cand[2] >> 4) & 0x0F) | (cand[3] << 4)) & 0x0FFF);
	std::memcpy(&cal.xScale, &cand[4], sizeof(float));
	std::memcpy(&cal.yScale, &cand[8], sizeof(float));
	// CRC-valid garbage still must not feed an UB float->int cast below.
	if (!(cal.xScale > 0.0f) || !(cal.yScale > 0.0f) ||
	    !std::isfinite(cal.xScale) || !std::isfinite(cal.yScale))
		return false;
	*out = cal;
	return true;
}

bool decodeCandidates(const std::uint8_t* response, Calibration* out)
{
	const std::uint8_t* const candidates = response + 5; // id + offset + size
	for (int i = 0; i < kCalibrationCandidates; ++i)
		if (tryCandidate(candidates + i * 16, out))
			return true;
	return false;
}

enum class Io
{
	Ok,
	Fail, // link trouble or nothing valid -> reconnect
	Exit,
};

Io readCalibration(Handle recvEvent, Calibration* out)
{
	CalibrationRequest request{};
	request.id = kCalibrationRequestId;
	request.responseTime = kCalibrationResponseTimeMs;
	request.size = kCalibrationReadSize;
	request.offset = 0;
	std::uint8_t response[kCalibrationResponseSize];

	const Exchange first =
	    exchange(recvEvent, &request, sizeof(request), response, sizeof(response),
	             kCalibrationResponseHead);
	if (first == Exchange::Exit)
		return Io::Exit;
	if (first != Exchange::Ok)
		return Io::Fail;
	if (decodeCandidates(response, out))
		return Io::Ok;

	// Some accessories keep their block in the 0x400+ region instead.
	for (int i = 0; i < kCalibrationFallbackSpan; ++i)
	{
		request.offset = static_cast<std::uint16_t>(kCalibrationFallbackBase + i);
		const Exchange x =
		    exchange(recvEvent, &request, sizeof(request), response, sizeof(response),
		             kCalibrationResponseHead);
		if (x == Exchange::Exit)
			return Io::Exit;
		if (x != Exchange::Ok)
			return Io::Fail;
		if (decodeCandidates(response, out))
			return Io::Ok;
	}
	return Io::Fail;
}

// ---------------------------------------------------------------------
// Worker cycle.
// ---------------------------------------------------------------------

// Volatile read: the IR module rewrites this field behind our back, and
// the streaming loop must re-evaluate it every pass.
bool connectionStreaming()
{
	volatile const SharedHeader* const header =
	    &static_cast<SharedMemory*>(g_sharedMem)->header;
	return header->connectionStatus == kConnectionStatusStreaming;
}

// Wait out a rest period; true when the shutdown event fired instead.
bool restThenContinue(u64 timeoutNs)
{
	return R_SUCCEEDED(svcWaitSynchronization(g_exitEvent, timeoutNs));
}

void teardownIrnop()
{
	// Session-down sequence: drop the link, empty both buffers, end the
	// IRNOP session. Failures don't matter -- the next initialize starts
	// from whatever the module has left, and the module is the only one
	// keeping score.
	disconnect();
	clearReceiveBuffer();
	clearSendBuffer();
	finalizeIrnop();
}

void closeEvents(Handle connEvent, Handle recvEvent)
{
	if (connEvent)
		svcCloseHandle(connEvent);
	if (recvEvent)
		svcCloseHandle(recvEvent);
}

enum class CycleOutcome
{
	Exit,
	Retry,
	Park, // probe budget spent with no calibrated link; await shutdown
};

// One full session attempt. Every exit path closes the per-cycle events
// and tears the IRNOP session down; the 1 s rest covers the two "nothing
// answered" outcomes (never linked, or calibration never validated --
// the reference retries the latter immediately, we rest so a
// misbehaving accessory cannot hammer the bus at full tilt).
CycleOutcome runCycle()
{
	if (R_FAILED(initializeIrnop()))
	{
		// A rejected InitializeIrnop (another process holds the console's
		// one ir:USER session) is module churn like any other round.
		++g_probeRounds;
		if (g_probeRounds >= kProbeRoundsMax)
			return CycleOutcome::Park;
		if (restThenContinue(kRetryRestNs))
			return CycleOutcome::Exit;
		return CycleOutcome::Retry;
	}
	g_recvCursor = 0;

	Handle connEvent = 0;
	Handle recvEvent = 0;
	if (R_FAILED(getConnectionStatusEvent(&connEvent)) ||
	    R_FAILED(getReceiveEvent(&recvEvent)))
	{
		closeEvents(connEvent, recvEvent);
		teardownIrnop();
		++g_probeRounds;
		if (g_probeRounds >= kProbeRoundsMax)
			return CycleOutcome::Park;
		if (restThenContinue(kRetryRestNs))
			return CycleOutcome::Exit;
		return CycleOutcome::Retry;
	}

	Handle waitHandles[2] = {connEvent, g_exitEvent};
	s32 which = -1;

	// --- connect ----------------------------------------------------
	bool linked = false;
	bool exitRequested = false;
	for (int attempt = 0; attempt < kConnectAttempts; ++attempt)
	{
		requireConnection(kDeviceIdCirclePadPro);
		const Result waited =
		    svcWaitSynchronizationN(&which, waitHandles, 2, false, kConnectWaitNs);
		if (R_SUCCEEDED(waited))
		{
			if (which == 0)
			{
				linked = true;
				break;
			}
			// which == 1: the exit event fired AND its oneshot state is
			// consumed by this wait -- the route below must be Exit, never
			// the rest-then-retry path, which would no longer see it.
			exitRequested = true;
			break;
		}
		disconnect();
	}
	if (exitRequested)
	{
		closeEvents(connEvent, recvEvent);
		teardownIrnop();
		return CycleOutcome::Exit;
	}
	if (!linked)
	{
		++g_probeRounds;
		if (g_probeRounds >= kProbeRoundsMax)
		{
			closeEvents(connEvent, recvEvent);
			teardownIrnop();
			return CycleOutcome::Park;
		}
		closeEvents(connEvent, recvEvent);
		teardownIrnop();
		if (restThenContinue(kRetryRestNs))
			return CycleOutcome::Exit;
		return CycleOutcome::Retry;
	}
	// The accessory answered. The probe budget does NOT reset here -- a
	// device that answers but never calibrates must not reset its own
	// budget every round -- only a validated calibration does, below.

	// --- calibrate --------------------------------------------------
	Calibration cal;
	const Io io = readCalibration(recvEvent, &cal);
	if (io == Io::Exit)
	{
		closeEvents(connEvent, recvEvent);
		teardownIrnop();
		return CycleOutcome::Exit;
	}
	if (io == Io::Fail)
	{
		// A connected accessory whose calibration block never validates is
		// the distinct failure class the connect phase cannot name: the IR
		// link itself is fine, so battery/clip advice would send the user
		// hunting the wrong end. Once per episode.
		if (!g_calibrationFailedLogged)
		{
			MC_LOG_WARN("3ds", "Circle Pad Pro: linked but calibration never validated\n");
			g_calibrationFailedLogged = true;
		}
		++g_probeRounds;
		if (g_probeRounds >= kProbeRoundsMax)
		{
			closeEvents(connEvent, recvEvent);
			teardownIrnop();
			return CycleOutcome::Park;
		}
		closeEvents(connEvent, recvEvent);
		teardownIrnop();
		if (restThenContinue(kRetryRestNs))
			return CycleOutcome::Exit;
		return CycleOutcome::Retry;
	}
	g_probeRounds = 0;
	g_calibrationFailedLogged = false;
	MC_LOG_INFO("3ds", "Circle Pad Pro: linked and calibrated\n");

	// --- stream -----------------------------------------------------
	publishIdle(); // connected stays false until the first framed answer
	while (connectionStreaming())
	{
		std::uint8_t response[kInputResponseSize];
		const Exchange x =
		    exchange(recvEvent, &kInputRequest, sizeof(kInputRequest), response,
		             sizeof(response), kInputResponseHead);
		if (x == Exchange::Exit)
		{
			publishIdle();
			closeEvents(connEvent, recvEvent);
			teardownIrnop();
			return CycleOutcome::Exit;
		}
		if (x == Exchange::Timeout)
			break; // link gone -> reconnect from the top
		if (x == Exchange::ReadError)
			continue; // one garbled frame: the accessory is still streaming

		const std::uint16_t rawX = static_cast<std::uint16_t>(
		    (response[1] | ((response[2] & 0x0F) << 8)) & 0x0FFF);
		const std::uint16_t rawY = static_cast<std::uint16_t>(
		    (((response[2] >> 4) & 0x0F) | (response[3] << 4)) & 0x0FFF);
		const std::uint8_t flags = response[4];

		const std::int16_t dx = static_cast<std::int16_t>(
		    static_cast<std::int16_t>(static_cast<float>(
		        static_cast<std::int16_t>(rawX) - static_cast<std::int16_t>(cal.xCentre)) *
		        cal.xScale) /
		    kPadScaleDivisor);
		const std::int16_t dy = static_cast<std::int16_t>(
		    static_cast<std::int16_t>(static_cast<float>(
		        static_cast<std::int16_t>(rawY) - static_cast<std::int16_t>(cal.yCentre)) *
		        cal.yScale) /
		    kPadScaleDivisor);

		std::uint32_t held = 0;
		if ((flags & kFlagRReleased) == 0)
			held |= KEY_R;
		if ((flags & kFlagZlReleased) == 0)
			held |= KEY_ZL;
		if ((flags & kFlagZrReleased) == 0)
			held |= KEY_ZR;

		publishLive(dx, dy, held, flags & kBatteryMask);
	}
	MC_LOG_INFO("3ds", "Circle Pad Pro: link closed; reconnecting\n");
	publishIdle();
	closeEvents(connEvent, recvEvent);
	teardownIrnop();
	return CycleOutcome::Retry;
}

void workerMain(void*)
{
	for (;;)
	{
		switch (runCycle())
		{
		case CycleOutcome::Retry:
			break;
		case CycleOutcome::Park:
			// Budget spent: the session is torn down and every handle the
			// cycle held is closed by now, so park on the exit event --
			// zero IRNOP traffic, zero ir-module churn, 8 KiB of stack --
			// until shutdown() reaps the thread (sleep/HOME/swkbd teardown
			// or process exit). The aptStateHook init() on wake/restore
			// starts the next probe; dsInputPoll's re-arm sees available()
			// stay true and leaves the parked worker alone.
			//
			// One line here, once per worker, for every park route (no
			// accessory, a link that never calibrates, or a module that
			// kept rejecting the session): the parked state is the normal
			// outcome on a CPP-less console, not a fault -- INFO, not WARN.
			MC_LOG_INFO("3ds", "Circle Pad Pro: no link in %d probe "
			              "rounds; standing down until the next wake\n",
			              g_probeRounds);
			publishIdle();
			svcWaitSynchronization(g_exitEvent, U64_MAX);
			return;
		case CycleOutcome::Exit:
			return;
		}
	}
}

// Close whatever exists -- init() failure rollback and the successful
// shutdown path share it. Only ever called with no worker running (join
// returned), so nothing is pulled out from under a live thread.
void releaseSession()
{
	if (g_thread)
	{
		threadFree(g_thread);
		g_thread = nullptr;
	}
	if (g_exitEvent)
	{
		svcCloseHandle(g_exitEvent);
		g_exitEvent = 0;
	}
	if (g_memBlock)
	{
		svcCloseHandle(g_memBlock);
		g_memBlock = 0;
	}
	if (g_irUser)
	{
		svcCloseHandle(g_irUser);
		g_irUser = 0;
	}
	if (g_sharedMem)
	{
		free(g_sharedMem);
		g_sharedMem = nullptr;
	}
}

} // namespace

void init()
{
	// Idempotent by contract (Display::create may run again).
	if (g_started)
		return;

	// New 3DS: the internal C-stick and its ZL/ZR arrive through ir:rst
	// (libctru's stock hidShouldUseIrrst gate opens it there), and there
	// is no accessory to drive -- this worker exists for Old hardware.
	bool isNew3ds = false;
	APT_CheckNew3DS(&isNew3ds);
	if (isNew3ds)
		return;

	// A real attempt starts here (past the New-3DS early-out): mark it and
	// clear the previous attempt's error, so a retry that succeeds does not
	// keep reporting the old failure.
	g_startAttempted.store(true, std::memory_order_relaxed);
	g_startError.store(0, std::memory_order_relaxed);

	g_sharedMem = memalign(0x1000, kSharedMemSize);
	if (!g_sharedMem)
	{
		MC_LOG_WARN("3ds", "CPP: shared memory allocation failed\n");
		g_startError.store(1, std::memory_order_relaxed);
		return;
	}

	Result r = srvGetServiceHandle(&g_irUser, "ir:USER");
	if (R_FAILED(r))
	{
		MC_LOG_WARN("3ds", "CPP: ir:USER unavailable (%08lX)\n",
		            static_cast<unsigned long>(static_cast<std::uint32_t>(r)));
		g_startError.store(static_cast<std::uint32_t>(r), std::memory_order_relaxed);
		releaseSession();
		return;
	}

	r = svcCreateMemoryBlock(&g_memBlock,
	                         static_cast<u32>(reinterpret_cast<std::uintptr_t>(g_sharedMem)),
	                         static_cast<u32>(kSharedMemSize), MEMPERM_READ,
	                         MEMPERM_READWRITE);
	if (R_FAILED(r))
	{
		MC_LOG_WARN("3ds", "CPP: shared memory block failed (%08lX)\n",
		            static_cast<unsigned long>(static_cast<std::uint32_t>(r)));
		g_startError.store(static_cast<std::uint32_t>(r), std::memory_order_relaxed);
		releaseSession();
		return;
	}

	r = svcCreateEvent(&g_exitEvent, RESET_ONESHOT);
	if (R_FAILED(r))
	{
		MC_LOG_WARN("3ds", "CPP: exit event failed (%08lX)\n",
		            static_cast<unsigned long>(static_cast<std::uint32_t>(r)));
		g_startError.store(static_cast<std::uint32_t>(r), std::memory_order_relaxed);
		releaseSession();
		return;
	}

	// Fresh probe budget and clean diagnostics for this worker: a parked
	// worker's spent episode must not bleed into the new session.
	g_probeRounds = 0;
	g_calibrationFailedLogged = false;

	// Owned thread (not detached): shutdown's join then threadFree is the
	// clean shape -- a detached worker frees its own Thread struct on exit,
	// racing any later join against freed memory.
	g_thread = threadCreate(workerMain, nullptr, kWorkerStackSize, kWorkerPriority,
	                        kWorkerCore, false);
	if (!g_thread)
	{
		MC_LOG_WARN("3ds", "CPP: worker thread could not start\n");
		g_startError.store(2, std::memory_order_relaxed);
		releaseSession();
		return;
	}

	g_started = true;
	MC_LOG_INFO("3ds", "Circle Pad Pro: ir:USER worker started (Old 3DS)\n");
}

void shutdown()
{
	// Idempotent: the normal shutdown path and the atexit safety net both
	// call this, in either order (the second call finds nothing started).
	if (!g_started)
		return;
	g_started = false;

	svcSignalEvent(g_exitEvent);
	const Result joined = threadJoin(g_thread, kJoinTimeoutNs);
	if (R_FAILED(joined))
	{
		// The worker is stuck in an IPC that never returned. Everything it
		// still uses is leaked on purpose: closing the handles or freeing
		// the shared block under a live thread would trade a clean leak for
		// a crash mid-exit. Process teardown reclaims the lot.
		MC_LOG_WARN("3ds", "CPP: worker did not stop in time; session leaked\n");
		g_thread = nullptr;
		return;
	}
	releaseSession();
}

Sample sample()
{
	Sample s;
	s.connected = g_connected.load(std::memory_order_acquire);
	const std::uint32_t packed = g_packedPos.load(std::memory_order_relaxed);
	s.dx = static_cast<std::int16_t>(packed & 0xFFFFu);
	s.dy = static_cast<std::int16_t>(packed >> 16);
	s.held = g_held.load(std::memory_order_relaxed);
	s.battery = g_battery.load(std::memory_order_relaxed);
	return s;
}

bool available()
{
	return g_started;
}

bool startAttempted()
{
	return g_startAttempted.load(std::memory_order_relaxed);
}

std::uint32_t startError()
{
	return g_startError.load(std::memory_order_relaxed);
}

} // namespace DsCirclePadPro

#endif // CTR_PLATFORM
