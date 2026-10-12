#include "platform/world/EntityOcclusionCache.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

#include "net/minecraft/src/Block.h"
#include "net/minecraft/src/Entity.h"
#include "net/minecraft/src/MathHelper.h"
#include "net/minecraft/src/World.h"
#include "java/Arithmetic.h"
#include "platform/PlatformTuning.h"

namespace
{
// Safety margins shared by every platform. These are correctness guards, not
// tuning knobs: shrinking them trades pop-in for culling coverage.
//
// An entity whose bounding-box centre is closer than this to the camera is
// never culled -- at that range a centre ray can report "blocked" for an
// entity partially visible around a corner. 2.5 blocks.
constexpr double kMinCullDistanceSq = 6.25;
// Bounding boxes larger than this on any axis are never culled: a centre
// ray cannot meaningfully describe the visibility of a multi-block body
// (the dragon), and frustum-exempt entities are already skipped upstream.
constexpr double kMaxCullAabbSize = 4.0;

// Cache housekeeping. The purge only runs once every kPurgeInterval frames
// and only once the table has accumulated strays, so a stable entity set
// never pays a scan; dead entities just age out instead.
constexpr std::size_t kMinEntriesBeforePurge = 128;
constexpr std::uint32_t kPurgeInterval = 600;

// Amanatides & Woo voxel DDA from the camera position towards the entity's
// bounding-box centre. Returns true when an opaque cube is crossed before
// the ray reaches the target cell.
//
// Every abort path returns false ("not occluded"): an unloaded chunk can
// never prove occlusion (and must not be generated just to answer a cull),
// a camera embedded in an opaque block (third person against a wall, sand
// on the head) would block every ray and disable culling for the frame, and
// a ray that reaches the target cell without hitting anything means the
// entity is visible.
bool rayBlockedByOpaqueBlocks(World *world, double ox, double oy, double oz,
                              double tx, double ty, double tz)
{
	const double deltaX = tx - ox;
	const double deltaY = ty - oy;
	const double deltaZ = tz - oz;
	const double distanceSq = deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ;
	if (distanceSq <= 0.0)
		return false;

	const double distance = std::sqrt(distanceSq);

	const int startX = MathHelper::floor_double(ox);
	const int startY = MathHelper::floor_double(oy);
	const int startZ = MathHelper::floor_double(oz);
	const int targetX = MathHelper::floor_double(tx);
	const int targetY = MathHelper::floor_double(ty);
	const int targetZ = MathHelper::floor_double(tz);

	// Camera and target share a cell: nothing can sit between them.
	if (startX == targetX && startY == targetY && startZ == targetZ)
		return false;

	// Chunk residency is checked once per chunk crossing, not per cell.
	// No chunk in the beta coordinate range maps here, so it doubles as
	// the "not yet resolved" sentinel.
	int lastChunkX = std::numeric_limits<int>::max();
	int lastChunkZ = std::numeric_limits<int>::max();
	const auto cellInLoadedChunk = [&](int blockX, int blockZ) -> bool
	{
		const int chunkX = JavaArithmetic::intShr(blockX, 4);
		const int chunkZ = JavaArithmetic::intShr(blockZ, 4);
		if (chunkX == lastChunkX && chunkZ == lastChunkZ)
			return true;
		if (!world->chunkExists(chunkX, chunkZ))
			return false;
		lastChunkX = chunkX;
		lastChunkZ = chunkZ;
		return true;
	};

	// A camera inside an opaque block (or outside loaded terrain) proves
	// nothing: keep every entity visible rather than culling the world.
	if (!cellInLoadedChunk(startX, startZ))
		return false;
	{
		const int_t startBlockId = world->getBlockId(startX, startY, startZ);
		if (startBlockId > 0 && startBlockId < Block::BLOCK_REGISTRY_SIZE
			&& Block::opaqueCubeLookup[startBlockId])
		{
			return false;
		}
	}

	const double dirX = deltaX / distance;
	const double dirY = deltaY / distance;
	const double dirZ = deltaZ / distance;

	const int stepX = dirX > 0.0 ? 1 : (dirX < 0.0 ? -1 : 0);
	const int stepY = dirY > 0.0 ? 1 : (dirY < 0.0 ? -1 : 0);
	const int stepZ = dirZ > 0.0 ? 1 : (dirZ < 0.0 ? -1 : 0);

	const double kNever = std::numeric_limits<double>::infinity();
	const double tDeltaX = stepX != 0 ? std::abs(1.0 / dirX) : kNever;
	const double tDeltaY = stepY != 0 ? std::abs(1.0 / dirY) : kNever;
	const double tDeltaZ = stepZ != 0 ? std::abs(1.0 / dirZ) : kNever;

	// Distance along the ray to the first boundary on each axis. For a
	// positive step that is (nextCell - origin); for a negative step it is
	// (origin - cell), and multiplying by tDelta (1/|dir|) makes both
	// cases the same expression. Mutated by the traversal below.
	double tMaxX = stepX != 0
		? (stepX > 0 ? (startX + 1 - ox) : (ox - startX)) * tDeltaX
		: kNever;
	double tMaxY = stepY != 0
		? (stepY > 0 ? (startY + 1 - oy) : (oy - startY)) * tDeltaY
		: kNever;
	double tMaxZ = stepZ != 0
		? (stepZ > 0 ? (startZ + 1 - oz) : (oz - startZ)) * tDeltaZ
		: kNever;

	int cellX = startX;
	int cellY = startY;
	int cellZ = startZ;

	// The t > distance exit below is the real terminator (a ray crosses at
	// most one boundary per unit of t on each axis); the counter is only a
	// belt against pathological floating point, so it can stay generous.
	int stepsLeft = static_cast<int>(distance * 3.0) + 8;

	while (stepsLeft-- > 0)
	{
		// Advance to the next voxel; the start cell was already tested.
		if (tMaxX <= tMaxY && tMaxX <= tMaxZ)
		{
			if (tMaxX > distance)
				return false;
			cellX += stepX;
			tMaxX += tDeltaX;
		}
		else if (tMaxY <= tMaxZ)
		{
			if (tMaxY > distance)
				return false;
			cellY += stepY;
			tMaxY += tDeltaY;
		}
		else
		{
			if (tMaxZ > distance)
				return false;
			cellZ += stepZ;
			tMaxZ += tDeltaZ;
		}

		if (cellX == targetX && cellY == targetY && cellZ == targetZ)
			return false;

		if (!cellInLoadedChunk(cellX, cellZ))
			return false;

		const int_t blockId = world->getBlockId(cellX, cellY, cellZ);
		if (blockId > 0 && blockId < Block::BLOCK_REGISTRY_SIZE
			&& Block::opaqueCubeLookup[blockId])
		{
			return true;
		}
	}

	return false;
}
} // namespace

EntityOcclusionCache::Entry *EntityOcclusionCache::findEntry(std::uint64_t key)
{
	for (Entry &entry : entries)
	{
		if (entry.key == key)
			return &entry;
	}
	return nullptr;
}

EntityOcclusionCache::Entry const *EntityOcclusionCache::findEntry(std::uint64_t key) const
{
	for (const Entry &entry : entries)
	{
		if (entry.key == key)
			return &entry;
	}
	return nullptr;
}

std::uint64_t EntityOcclusionCache::tileEntityKey(int_t blockX, int_t blockY, int_t blockZ)
{
	// Beta world coordinates fit [-2^25, 2^25) blocks (the +-30M border is
	// well inside), so x and z pack into 26 offset bits each and y into its
	// 7-bit height range -- 59 bits total, plus the family tag in the top
	// bit. Positions outside the range simply alias into the same key space
	// the frustum has already culled everything beyond anyway.
	constexpr std::uint64_t kTileEntityTag = 0x8000000000000000ULL;
	const std::uint64_t packedX =
		(static_cast<std::uint64_t>(static_cast<std::int64_t>(blockX) + (1LL << 25)) & 0x3FFFFFFULL) << 33;
	const std::uint64_t packedZ =
		(static_cast<std::uint64_t>(static_cast<std::int64_t>(blockZ) + (1LL << 25)) & 0x3FFFFFFULL) << 7;
	const std::uint64_t packedY = static_cast<std::uint64_t>(blockY) & 0x7FULL;
	return kTileEntityTag | packedX | packedZ | packedY;
}

void EntityOcclusionCache::beginFrame()
{
	++frame;
	raycastsLeft = PLATFORM_ENTITY_OCCLUSION_RAYCASTS_PER_FRAME;

	if (entries.size() > kMinEntriesBeforePurge && (frame % kPurgeInterval) == 0)
	{
		entries.erase(std::remove_if(entries.begin(), entries.end(),
			[this](const Entry &entry)
			{
				return (frame - entry.lastSeenFrame) >= kPurgeInterval;
			}), entries.end());
	}
}

bool EntityOcclusionCache::isEntityOccluded(World *world, Entity *entity,
                                            double cameraX, double cameraY, double cameraZ)
{
	// An occupied mount stays visible: culling the boat or minecart under a
	// passenger leaves them hovering, and the passenger's own body already
	// proves the cell visible.
	if (entity->riddenByEntity != nullptr)
		return false;

	const AxisAlignedBB &box = *entity->boundingBox;
	const double sizeX = box.maxX - box.minX;
	const double sizeY = box.maxY - box.minY;
	const double sizeZ = box.maxZ - box.minZ;
	if (sizeX > kMaxCullAabbSize || sizeY > kMaxCullAabbSize || sizeZ > kMaxCullAabbSize)
		return false;

	const double centerX = (box.minX + box.maxX) * 0.5;
	const double centerY = (box.minY + box.maxY) * 0.5;
	const double centerZ = (box.minZ + box.maxZ) * 0.5;
	const double dx = centerX - cameraX;
	const double dy = centerY - cameraY;
	const double dz = centerZ - cameraZ;
	const double distanceSq = dx * dx + dy * dy + dz * dz;
	if (distanceSq < kMinCullDistanceSq)
		return false;

	return occludedWithCache(world, static_cast<std::uint64_t>(
		static_cast<std::int64_t>(entity->entityId)),
		cameraX, cameraY, cameraZ, centerX, centerY, centerZ);
}

bool EntityOcclusionCache::isTileEntityOccluded(World *world, int_t blockX, int_t blockY, int_t blockZ,
                                                double cameraX, double cameraY, double cameraZ)
{
	const double centerX = blockX + 0.5;
	const double centerY = blockY + 0.5;
	const double centerZ = blockZ + 0.5;
	const double dx = centerX - cameraX;
	const double dy = centerY - cameraY;
	const double dz = centerZ - cameraZ;
	const double distanceSq = dx * dx + dy * dy + dz * dz;
	if (distanceSq < kMinCullDistanceSq)
		return false;

	return occludedWithCache(world, tileEntityKey(blockX, blockY, blockZ),
	                         cameraX, cameraY, cameraZ, centerX, centerY, centerZ);
}

bool EntityOcclusionCache::occludedWithCache(World *world, std::uint64_t key,
                                             double cameraX, double cameraY, double cameraZ,
                                             double targetX, double targetY, double targetZ)
{
	Entry *entry = findEntry(key);
	if (entry != nullptr)
		entry->lastSeenFrame = frame;

	const bool fresh = entry != nullptr
		&& (frame - entry->lastRayFrame) < static_cast<std::uint32_t>(PLATFORM_ENTITY_OCCLUSION_RECHECK_FRAMES);
	if (fresh)
		return entry->occluded;

	if (raycastsLeft <= 0)
	{
		// Budget exhausted: reuse the stale answer rather than defaulting
		// to visible, so a crowded scene keeps the culling win. A brand-new
		// entity renders this frame and gets its cast next frame.
		return entry != nullptr ? entry->occluded : false;
	}

	--raycastsLeft;
	const bool occluded = rayBlockedByOpaqueBlocks(world, cameraX, cameraY, cameraZ,
	                                              targetX, targetY, targetZ);
	if (entry == nullptr)
	{
		Entry newEntry;
		newEntry.key = key;
		newEntry.occluded = occluded;
		newEntry.lastRayFrame = frame;
		newEntry.lastSeenFrame = frame;
		entries.push_back(newEntry);
	}
	else
	{
		entry->occluded = occluded;
		entry->lastRayFrame = frame;
		entry->lastSeenFrame = frame;
	}
	return occluded;
}

void EntityOcclusionCache::clear()
{
	entries.clear();
	raycastsLeft = 0;
}