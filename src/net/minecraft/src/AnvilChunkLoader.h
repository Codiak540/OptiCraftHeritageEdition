#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "ChunkCoordIntPair.h"
#include "IChunkLoader.h"
#include "IThreadedFileIO.h"

class AnvilChunkLoaderPending;
class NBTTagCompound;

// net.minecraft.src.AnvilChunkLoader
class AnvilChunkLoader : public IChunkLoader, public IThreadedFileIO
{
public:
    explicit AnvilChunkLoader(const std::string &worldDir, bool readOnly = false);
    ~AnvilChunkLoader() override;

    Chunk *loadChunk(World *world, int_t x, int_t z,
                     ChunkLoadStatus *status = nullptr) override;
    // Whether loadChunk() would find data for this chunk: queued for writing
    // or already in its region file. Lets the streaming provider hand only
    // genuinely new terrain to the generation worker, which has no loader of
    // its own and would otherwise generate over a saved chunk.
    bool isChunkSaved(int_t x, int_t z);
    // Worker-side split of loadChunk(), mirroring McRegionChunkLoader: the
    // chunk-generation worker reads the raw bytes (thread-safe: the pending
    // queue copies under its mutex and RegionFile/RegionFileCache serialize
    // internally), decodes blocks/light/heightmap into a Chunk there, and
    // hands the parsed root back so publish can still construct the chunk's
    // entities on the game thread. A decode failure returns null and the raw
    // data travels on for the game-thread fallback, preserving the region
    // data exactly like the McRegion contract.
    bool readChunkData(int_t x, int_t z, std::vector<byte_t> &data,
                       ChunkLoadStatus *status = nullptr);
    Chunk *decodeChunkBlocksFromData(World *world, int_t x, int_t z,
                                     std::vector<byte_t> &data,
                                     std::unique_ptr<NBTTagCompound> &rootOut,
                                     ChunkLoadStatus *status = nullptr);
    // Game-thread completion of a worker-decoded chunk: entities and tile
    // entities through the shared ChunkLoader helper, plus the Anvil-only
    // scheduled-tick replay (world->scheduleBlockUpdateFromLoad) that the
    // worker never runs because it writes World state.
    static void attachChunkEntities(World *world, Chunk *chunk, NBTTagCompound *root);
    // Game-thread fallback when the worker could not decode a column:
    // decode + entity attach over the raw data, same shape as
    // McRegionChunkLoader::loadChunkFromData.
    Chunk *loadChunkFromData(World *world, int_t x, int_t z,
                             std::vector<byte_t> &data, ChunkLoadStatus *status = nullptr);
    void saveChunk(World *world, Chunk *chunk) override;
    void saveExtraChunkData(World *world, Chunk *chunk) override;
    void addRandomArmor() override;
    void chunkTick() override;
    void saveExtraData() override;
    bool writeNextIO() override;

private:
    Chunk *loadChunkFromCompound(World *world, int_t expectedX, int_t expectedZ,
                                 NBTTagCompound *root, ChunkLoadStatus *status);
    Chunk *readChunkFromLevel(World *world, NBTTagCompound *level);
    // Blocks/biomes/heightmap/skylight portion of readChunkFromLevel, shared
    // by the game-thread load and the worker-side decode; the entity,
    // tile-entity and scheduled-tick tails stay with readChunkFromLevel
    // because they touch World.
    Chunk *readChunkBlocksFromLevel(World *world, NBTTagCompound *level);
    void writeChunkToLevel(Chunk *chunk, World *world, NBTTagCompound *level);
    void queueChunkToSave(const ChunkCoordIntPair &position, std::vector<byte_t> serialized);
    void writePendingChunk(AnvilChunkLoaderPending *pending);
    bool copyPendingChunkData(const ChunkCoordIntPair &position, std::vector<byte_t> &out);

    std::string worldDir;
    bool storageDisabled;
    bool readOnly;
    std::vector<byte_t> readScratch;
    std::vector<byte_t> writeScratch;

    std::vector<AnvilChunkLoaderPending *> pendingSaves;
    std::unordered_set<ChunkCoordIntPair, ChunkCoordIntPairValueHash, ChunkCoordIntPairValueEqual> pendingCoordinates;
    // Sum of every pendingSaves entry's serializedData size. Read/written under
    // pendingMutex, exactly like the queue it measures, so the backpressure
    // flush in queueChunkToSave() sees the same accounting the pops do.
    std::size_t pendingBytes = 0;
    std::mutex pendingMutex;
};
