#include "Packet51MapChunk.h"

#include <atomic>
#include <stdexcept>
#include <utility>
#include <zlib.h>

#include "NetHandler.h"
#include "platform/PlatformTuning.h"

namespace
{
constexpr int_t kMaxCompressedChunkBytes = 256 * 1024;
constexpr std::size_t kBytesPerPrimarySectionWorstCase = 12288;

#if defined(CTR_PLATFORM) && !PLATFORM_MP_DEFERRED_CHUNKS
// The 3DS runs the direct-import multiplayer profile (PLATFORM_MP_DEFERRED_
// CHUNKS 0, see DsWorldTuning.h), so handleMapChunk inflates every Packet51 on
// the game thread. The zlib pass over a full column is the single most
// expensive step of that import, and at 268 MHz six of them per tick are what
// collapsed the tick rate -- movement packets went out late and the server
// read the client as laggy. The desktop already inflates in readPacketData,
// which on this console runs on the reader thread pinned to the second core,
// so the 3DS takes the same shape: pre-inflate there and handleMapChunk's
// ensureDecompressed() becomes a no-op.
//
// The cap keeps the memory bounded. The read queue's byte budget counts the
// COMPRESSED size (getPacketSize), so without it a burst could park hundreds
// of worst-case ~192 KB inflated buffers in the queue at once. Eight already
// inflated packets ahead of the bounded import loop (six columns per tick on
// the Old model) is enough to keep it fed; packets past the cap stay
// compressed and pay the inline inflate on dispatch, exactly the previous
// behaviour. Counted in ensureDecompressed() -- so a game-thread fallback
// inflate reserves the same room -- and released in the destructor,
// whichever thread ends up owning the packet.
constexpr int kMaxLiveInflatedChunkPackets = 8;
std::atomic<int> liveInflatedChunkPackets{0};
#endif

int_t countSectionBits(int_t mask)
{
    int_t count = 0;
    for (int_t section = 0; section < 16; ++section)
        count += (mask >> section) & 1;
    return count;
}
}

Packet51MapChunk::Packet51MapChunk()
{
    isChunkDataPacket = true;
}

#if defined(CTR_PLATFORM) && !PLATFORM_MP_DEFERRED_CHUNKS
Packet51MapChunk::~Packet51MapChunk()
{
    // Release the pre-inflate slot taken in ensureDecompressed(). Only a
    // packet that actually holds inflated data was counted, so an unsent or
    // decode-failed packet releases nothing.
    if (!chunkData.empty())
        liveInflatedChunkPackets.fetch_sub(1, std::memory_order_acq_rel);
}
#else
Packet51MapChunk::~Packet51MapChunk() = default;
#endif

std::size_t Packet51MapChunk::expectedInflatedSize() const
{
    const int_t sectionCount = countSectionBits(yChMin & 0xffff);
    return kBytesPerPrimarySectionWorstCase * (std::size_t)sectionCount
        + (includeInitialize ? 256u : 0u);
}

void Packet51MapChunk::readPacketData(std::istream &is)
{
    xCh = IOUtil::readInt(is);
    zCh = IOUtil::readInt(is);
    includeInitialize = IOUtil::readUnsignedByte(is) != 0;
    yChMin = (int_t)(ushort_t)IOUtil::readShort(is);
    yChMax = (int_t)(ushort_t)IOUtil::readShort(is);
    tempLength = IOUtil::readInt(is);
    field_48178_h = IOUtil::readInt(is);

    if (tempLength <= 0 || tempLength > kMaxCompressedChunkBytes)
        throw std::runtime_error("Invalid compressed map chunk size: " + std::to_string(tempLength));

    compressedChunk.resize((std::size_t)tempLength);
    is.read(reinterpret_cast<char *>(compressedChunk.data()), tempLength);
    if (!is)
        throw std::runtime_error("Truncated compressed map chunk");

#if !defined(WII_PLATFORM) && !defined(PS2_PLATFORM) && !defined(CTR_PLATFORM)
    // Desktop-only eager inflate. Every console that runs
    // PLATFORM_MP_DEFERRED_CHUNKS (PS2, Wii, 3DS) must keep the compressed
    // payload alive: NetClientHandler::handleMapChunk moves it into
    // WorldClient's deferred cache via takeCompressedData(), and a distant
    // column is only ever materialized from that compressed copy when the
    // player walks into promotion range. Inflating (and freeing) it here
    // parks those columns with an empty base, so promoteDeferredChunk()
    // skips them forever -- on the 3DS that read as chunks that never load
    // on multiplayer servers.
    if (!ensureDecompressed())
        throw std::runtime_error("Invalid compressed map chunk data");
#endif

#if defined(CTR_PLATFORM) && !PLATFORM_MP_DEFERRED_CHUNKS
    // Reader-thread pre-inflate, bounded by the live-inflated cap above; the
    // deferred-pipeline exclusion above does not apply because this profile
    // imports every Packet51 directly. Past the cap the packet stays
    // compressed and pays the inline inflate on the game thread in
    // handleMapChunk, exactly the previous behaviour. The compressed payload
    // is kept either way (the console branch of ensureDecompressed never
    // frees it): WorldClient's trim/rematerialize stash still needs it.
    if (liveInflatedChunkPackets.load(std::memory_order_acquire) < kMaxLiveInflatedChunkPackets &&
        !ensureDecompressed())
        throw std::runtime_error("Invalid compressed map chunk data");
#endif
}

bool Packet51MapChunk::ensureDecompressed()
{
    if (!chunkData.empty())
        return true;
    if (compressedChunk.empty())
        return false;

    const std::size_t expected = expectedInflatedSize();
    if (expected == 0)
        return false;

    chunkData.assign(expected, 0);
    uLongf actual = (uLongf)chunkData.size();
    const int result = uncompress(reinterpret_cast<Bytef *>(chunkData.data()), &actual,
                                  reinterpret_cast<const Bytef *>(compressedChunk.data()),
                                  (uLong)compressedChunk.size());
    if (result != Z_OK)
    {
        chunkData.clear();
        return false;
    }

#if defined(CTR_PLATFORM) && !PLATFORM_MP_DEFERRED_CHUNKS
    // Take a pre-inflate slot for as long as this packet holds inflated data
    // (released in the destructor). Applies to the reader-thread pre-inflate
    // and the game-thread fallback alike -- both put the same worst-case
    // buffer behind the queue.
    liveInflatedChunkPackets.fetch_add(1, std::memory_order_acq_rel);
#endif

    // Java allocates the worst-case 12288 bytes for every primary section and
    // leaves any unused Add-array tail zero-filled. Keep that full allocation;
    // Chunk::func_48494_a consumes only the bytes selected by yChMax.
#if !defined(WII_PLATFORM) && !defined(PS2_PLATFORM) && !defined(CTR_PLATFORM)
    std::vector<byte_t>().swap(compressedChunk);
#endif
    return true;
}

std::vector<byte_t> Packet51MapChunk::takeCompressedData()
{
    return std::move(compressedChunk);
}

void Packet51MapChunk::writePacketData(std::ostream &os)
{
    IOUtil::writeInt(os, xCh);
    IOUtil::writeInt(os, zCh);
    IOUtil::writeByte(os, (byte_t)(includeInitialize ? 1 : 0));
    IOUtil::writeShort(os, (short_t)(yChMin & 0xffff));
    IOUtil::writeShort(os, (short_t)(yChMax & 0xffff));
    IOUtil::writeInt(os, tempLength);
    IOUtil::writeInt(os, field_48178_h);
    if (!compressedChunk.empty())
        os.write(reinterpret_cast<const char *>(compressedChunk.data()), tempLength);
}

void Packet51MapChunk::processPacket(NetHandler &nethandler)
{
    nethandler.func_48487_a(*this);
}

int_t Packet51MapChunk::getPacketSize()
{
    return 17 + tempLength;
}
