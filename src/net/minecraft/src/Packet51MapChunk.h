#pragma once

#include "Packet.h"
#include <vector>

class NetHandler;

// net.minecraft.src.Packet51MapChunk (Minecraft 1.2.5)
class Packet51MapChunk : public Packet
{
public:
    Packet51MapChunk();
    // Releases the 3DS reader-side pre-inflate slot when the packet holds
    // inflated data (see Packet51MapChunk.cpp).
    ~Packet51MapChunk() override;

    void readPacketData(std::istream &is) override;
    void writePacketData(std::ostream &os) override;
    void processPacket(NetHandler &nethandler) override;
    int_t getPacketSize() override;

    bool ensureDecompressed();
    // True while the packet holds only its compressed payload -- the state
    // the 3DS reader-side wedge service looks for at the read-queue front
    // (see NetworkManager::preInflateFrontQueuedChunk).
    bool needsInflation() const;
    // Inflate under the same live-inflated cap as the reader's decode-time
    // pre-inflate; false = the cap is full (or the payload cannot inflate)
    // and the caller should retry later. The wedge service runs only in the
    // 3DS direct-import profile; the other profiles return false unchanged.
    bool preInflate();
    std::vector<byte_t> takeCompressedData();

    int_t xCh = 0;
    int_t zCh = 0;
    int_t yChMin = 0;
    int_t yChMax = 0;
    bool includeInitialize = false;
    std::vector<byte_t> chunkData;

private:
    std::size_t expectedInflatedSize() const;

    int_t tempLength = 0;
    int_t field_48178_h = 0;
    std::vector<byte_t> compressedChunk;
};
