#pragma once

#include <cstdint>
#include <vector>

#include "java/Type.h"

class World;
class Entity;

// EntityCulling-style occlusion culling for the entity render pass -- a
// clean-room port of the technique behind tr7zw/EntityCulling, adapted to the
// beta renderer's single-threaded frame structure.
//
// The frustum test in RenderGlobal::renderEntities only rejects entities
// outside the view volume; anything inside it is submitted for drawing even
// when a wall of terrain fully hides it -- and on the consoles the animated-
// model submission is one of the most expensive things the frame does. This
// cache answers "is this entity hidden behind opaque blocks?" with a voxel
// DDA ray cast from the camera to the entity's bounding-box centre, and
// remembers the answer for a few frames so the cost is amortised.
//
// The same cache also answers for tile entities (the moreculling extension of
// the technique): chests, signs and every other special-rendered block entity
// pay the same per-frame transform+draw the mobs do, and a chest behind a wall
// is just as invisible. Tile entities are keyed by packed block position
// instead of entity id; the two key families never collide (see
// tileEntityKey).
//
// Differences from the upstream mod, on purpose:
//  * No async ray-cast thread. The mod walks the entity list on a side
//    thread against a world snapshot; here the World is mutated by the game
//    thread and a second reader would race chunk teardown. Instead the
//    casts run on the render thread under a per-frame budget
//    (PLATFORM_ENTITY_OCCLUSION_RAYCASTS_PER_FRAME), so the worst case is
//    bounded regardless of entity count.
//  * A stale entry is reused when the budget is exhausted, so a burst of
//    new entities degrades to slightly older answers, never to unbounded
//    work. An entity with no entry at all renders this frame (visible by
//    default: a wrong cull is a pop-in, a wrong render is only vanilla).
//  * Casts abort as "visible" when they step into an unloaded chunk: the
//    missing terrain can never prove occlusion, and probing it must not
//    force chunk generation (World::getBlockId generates through
//    getChunkFromChunkCoords).
//
// Never culled: the render view entity itself and frustum-exempt entities
// (both filtered by the caller before asking), anything ridden (a culled
// boat would leave its passenger hovering over nothing), anything closer
// than kMinCullDistanceSq to the camera (an entity peeking around a corner
// must not pop), and bounding boxes wider than kMaxCullAabbSize on any axis
// (the dragon and other multi-block bodies intersect too much geometry for a
// centre ray to be a meaningful visibility answer). The caller additionally
// exempts piston tile entities: their moving arm is drawn outside the base
// block, so a centre-of-block ray can report an occluded core while the arm
// pokes through the very wall that blocked the cast.
class EntityOcclusionCache
{
public:
	// Advances the frame clock and refills the per-frame ray-cast budget.
	// Call once per rendered frame, before the first isEntityOccluded().
	void beginFrame();

	// True when the entity should be skipped this frame: the cached (or
	// freshly cast) answer says opaque terrain fully hides it between the
	// camera position and the entity's bounding-box centre.
	bool isEntityOccluded(World *world, Entity *entity,
	                      double cameraX, double cameraY, double cameraZ);

	// Tile-entity variant: the ray targets the centre of the block the tile
	// entity lives in. Keyed by packed position, sharing the entity budget.
	bool isTileEntityOccluded(World *world, int_t blockX, int_t blockY, int_t blockZ,
	                          double cameraX, double cameraY, double cameraZ);

	// Drops all cached answers. Entity ids are per-world, so this belongs
	// on every world change; the frame clock keeps counting.
	void clear();

private:
	struct Entry
	{
		std::uint64_t key = 0;
		bool occluded = false;
		std::uint32_t lastRayFrame = 0;
		std::uint32_t lastSeenFrame = 0;
	};

	// Cache flow shared by both askers: fresh answer, budgeted recast,
	// stale reuse. The caller owns the safety guards and the target point.
	bool occludedWithCache(World *world, std::uint64_t key,
	                       double cameraX, double cameraY, double cameraZ,
	                       double targetX, double targetY, double targetZ);

	// Position-keyed tile entity entries carry the top bit set; entity ids
	// are positive int_t and can never reach it.
	static std::uint64_t tileEntityKey(int_t blockX, int_t blockY, int_t blockZ);

	Entry *findEntry(std::uint64_t key);
	const Entry *findEntry(std::uint64_t key) const;

	std::vector<Entry> entries;
	std::uint32_t frame = 0;
	int raycastsLeft = 0;
};