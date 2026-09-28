#pragma once

// -----------------------------------------------------------------------------
// 3DS world tuning
// -----------------------------------------------------------------------------
// The 3DS takes the desktop branch of PlatformGameTuning.h on purpose (it
// shares the desktop WorldRenderer/RenderGlobal path) and then overrides the
// handful of desktop *assumptions* the ARM11 cannot honour. Same shape as the
// Wii's WiiWorldTuning.h/WiiFrameTuning.h pair, kept in one file because every
// knob here traces to the same 2026-09-25 debug.log: a world entry that
// started at 53 fps, spent its whole mesh budget on a 2312-slot renderer grid,
// and slid to 1 fps within seconds:
//
//   [3ds.perf] frames=534 fps=53 ...                 (menu / loading)
//   [3ds.perf] frames=481 fps=48 ... 458 fps=45 ... 330 fps=33
//   [3ds.perf] frames=212 fps=21 frame=65/9400ms ...
//   [3ds.perf] frames=15  fps=1  frame=708/1444ms tick=238/347ms
//   [MC][W][render] 3ds: draw with 21096 vertices exceeds one arena ...
//
// The desktop defaults it inherited: render distance FAR with
// ofRenderDistanceFine=128 -- RenderGlobal derives its grid from the fine
// distance, so that is a 17 x 8 x 17 grid (2312 sections, the very figure the
// Wii's tuning header calls unaffordable at 729 MHz) -- plus
// PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME=99 with no wall-clock mesh budget,
// and a 128-block synchronous preload (289 chunk columns before the first
// frame). None of that is a deliberate choice for this hardware; it is simply
// the only branch of the table the 3DS had not claimed yet.

// TINY, the PS2's own default (Ps2CoreTuning.h). GameSettingsBackend_3DS pins
// both Cycle and Clamp to this value exactly like the PS2 backend, so this is
// the whole render-distance surface: what boots is what plays.
#undef  PLATFORM_DEFAULT_RENDER_DISTANCE
#define PLATFORM_DEFAULT_RENDER_DISTANCE          3

// The lever the renderer grid actually reads on this path. RenderGlobal sizes
// its grid from Config::getRenderDistanceFine(), GameSettings clamps that
// fine value to PLATFORM_VISIBLE_CHUNK_RADIUS * 16 (GameSettingsBackend_3DS),
// and EntityRenderer draws the fog edge at the same figure -- so radius 2
// pins grid, fog and the fine slider to 32 blocks, 5 columns wide
// (2*32/16+1). Coherent with TINY above: the coarse table maps TINY to
// 32 << (3 - 3) = 32 blocks.
#undef  PLATFORM_VISIBLE_CHUNK_RADIUS
#define PLATFORM_VISIBLE_CHUNK_RADIUS              2

// Moving vertical renderer window, the PS2's 5x3x5 idea at TINY proportions.
// The desktop branch of RenderGlobal::markRenderersForNewPosition() centers
// this window on the player generically (the guarded-edge variant is the
// PS2/WII one), so 5 sections = 80 blocks tall against TINY's 32-block
// horizontal reach and the grid drops from 5 x 8 x 5 = 200 slots to 125.
#undef  PLATFORM_VERTICAL_CHUNK_COUNT
#define PLATFORM_VERTICAL_CHUNK_COUNT              5
#undef  PLATFORM_CENTER_VERTICAL_RENDERERS
#define PLATFORM_CENTER_VERTICAL_RENDERERS         1

// Preload radius, in blocks. The desktop's 128 steps 16 blocks at a time over
// [-128, 128] on both axes -- 17 x 17 = 289 chunk columns generated
// SYNCHRONOUSLY on the loading screen (the Wii tuning header's own analysis
// of the same default). 32 keeps startup to 5 x 5 columns and streams the
// rest behind the mesh budget below.
#undef  PLATFORM_PRELOAD_RADIUS_BLOCKS
#define PLATFORM_PRELOAD_RADIUS_BLOCKS             32

// Mesh governor: one updateRenderers() step per frame under a wall-clock
// budget, instead of the vanilla retry loop that re-calls updateRenderers
// until the same total work is done -- the loop is the 9400 ms frame in the
// log once the ceiling above it is 99 updates with no clock. Both halves have
// to switch together; see the PLATFORM_MESH_BUDGET comment in
// PlatformGameTuning.h.
#undef  PLATFORM_MESH_BUDGET
#define PLATFORM_MESH_BUDGET                       1
// The attempt ceiling is the PS2's 10: platformGameSettingsDefaultChunkUpdates()
// returns this value, so it is both the ofChunkUpdates default and the
// load-time floor -- PS2's options.txt carries ofChunkUpdates:10 and the 3DS
// now writes the same number. Real work stays bounded by the
// PLATFORM_CHUNK_BUILD_BUDGET_MS clock below (MeshBudget::exhausted() stops
// at 5 ms regardless); this only caps how many renderer entries one call may
// try before the clock says stop.
#undef  PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME
#define PLATFORM_MAX_RENDERER_UPDATES_PER_FRAME    10
#undef  PLATFORM_CHUNK_BUILD_BUDGET_MS
#define PLATFORM_CHUNK_BUILD_BUDGET_MS             5

// Edit-latency lane (Wii values): without it a block break queues behind the
// streaming budget above and the edited section only re-meshes once the
// player moves.
#undef  PLATFORM_URGENT_MESH_DISTANCE_SQ
#define PLATFORM_URGENT_MESH_DISTANCE_SQ           1024.0f
#undef  PLATFORM_URGENT_MESH_BUDGET_MS
#define PLATFORM_URGENT_MESH_BUDGET_MS             10

// Resident chunk cache and eviction: the PS2's memory profile
// (Ps2CoreTuning.h). The cache now matches TINY's 5-column window exactly
// (radius 2) instead of the visible+1 rule this header used to quote, the
// unload ring stays one chunk beyond as hysteresis so walking a boundary
// does not churn, and the map reserve follows -- 25 mandatory / 49 maximum
// resident columns instead of 49/81, ~2 MB less world held.
//
// The cost is the PS2's own documented trade: EntityRenderer's prefetch
// clamps its radius to this cache, so a chunk now enters the generation
// queue when it becomes VISIBLE, and crossing a border at speed can show a
// hole for a tick or two until the streamer catches up -- terrain that
// disappears at the streaming edge rather than a kept-alive outer ring,
// which is the resource saving this profile is buying.
// PLATFORM_CULL_MISSING_CHUNK_BOUNDARY_FACES (worldgen profile below) keeps
// that hole reading as open air instead of a wall of dirt along the absent
// column. Rollback is 3/4, the hole-free visible+1 rule.
//
// Simulation radii are NOT tied to this (the PS2's rule): the random-tick
// and mob radii stay pinned to the visible radius above, so shrinking the
// streaming window does not grow per-tick simulation cost.
#undef  PLATFORM_CHUNK_CACHE_RADIUS
#define PLATFORM_CHUNK_CACHE_RADIUS                2
#undef  PLATFORM_CHUNK_UNLOAD_RADIUS
#define PLATFORM_CHUNK_UNLOAD_RADIUS               3
#undef  PLATFORM_CHUNK_MAP_RESERVE
#define PLATFORM_CHUNK_MAP_RESERVE                 64

// Eviction rate, the PS2's: a chunk leaves only once it is BOTH outside the
// unload ring and untouched for 60 world ticks, and the per-tick budgets
// bound how many are actually freed -- unloadChunk can write the chunk out,
// and an SD write is exactly the kind of multi-chunk free+save burst that
// arrives mid-walk as a tirone. The desktop 4/16/200 would allow a
// four-chunk spike inside one tick.
#undef  PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK
#define PLATFORM_MAX_CHUNK_UNLOADS_PER_TICK         1
#undef  PLATFORM_EMERGENCY_CHUNK_UNLOADS_PER_TICK
#define PLATFORM_EMERGENCY_CHUNK_UNLOADS_PER_TICK   2
#undef  PLATFORM_MIN_UNUSED_TICKS_BEFORE_UNLOAD
#define PLATFORM_MIN_UNUSED_TICKS_BEFORE_UNLOAD    60

// Save policy, the PS2's. In a NEW world every streamed column is dirty the
// moment it is born (heightmap fill, structures and populate all mark it), so
// the desktop policy taxes a walk twice over: every runtime autosave (40
// ticks = 2 s) rewrites level.dat and serializes up to 24 dirty chunks in one
// game-thread burst, and every eviction then serializes its chunk again on
// the way out. A saved tutorial never pays any of it because its chunks
// arrive decoded and clean -- exactly the new-world-vs-tutorial asymmetry
// behind the "0-fps tirones while moving" report. The 3DS takes the PS2's
// answers instead:
//   * runtime autosave is off entirely: generated terrain is deterministic
//     from the seed, and everything goes out with the "Saving chunks" pass
//     (or the pause menu's explicit save) rather than mid-walk;
//   * unload writes only chunks the player actually edited
//     (PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_UNLOAD, whose Chunk/World
//     marking code is flag-gated, so enabling it here is the whole change),
//     so walking out of a generated column frees it instead of serializing
//     it to SD on the game thread;
//   * should a runtime save ever be re-enabled, it skips level.dat and
//     drips at most 2 chunks per pass -- the desktop 24-chunk,
//     level.dat-every-time burst is what the autosave was.
#undef  PLATFORM_DISABLE_RUNTIME_AUTOSAVE
#define PLATFORM_DISABLE_RUNTIME_AUTOSAVE             1
#undef  PLATFORM_RUNTIME_AUTOSAVE_LEVEL_DATA
#define PLATFORM_RUNTIME_AUTOSAVE_LEVEL_DATA          0
#undef  PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT
#define PLATFORM_INCREMENTAL_CHUNK_SAVE_LIMIT        2
#undef  PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_UNLOAD
#define PLATFORM_SAVE_RUNTIME_CHUNK_EDITS_ON_UNLOAD   1

// Entity simulation normally requires every chunk in a 32-block radius (5x5
// columns); the resident cache above is 5x5 (radius 2), so the halved range
// keeps one chunk of margin inside it.
#undef  PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS
#define PLATFORM_PLAYER_UPDATE_CHUNK_RANGE_BLOCKS  16

// Touch-look scale, +10% over vanilla's mouse curve (owner call, 2026-09-28):
// the bottom panel is this console's only camera control, so a slightly
// hotter cube beats dragging long arcs across the 240-px panel height. The
// alias slot is PlatformInputTuning.h (8.0f vanilla), next to the PS2's
// direct-camera scale.
#undef  PLATFORM_MOUSE_CAMERA_SCALE
#define PLATFORM_MOUSE_CAMERA_SCALE              8.8f

// Lighting, the Wii values: the desktop queue is effectively unbounded
// (1,000,000 jobs, 5-entry merge scan) and the flood fill is what makes a
// streamed chunk publish expensive. 3 ms of a ~33 ms frame with a 256-job
// backstop; the wide merge scan and hard cap keep the backlog from growing
// for as long as the player keeps walking.
#undef  PLATFORM_LIGHTING_UPDATES_PER_FRAME
#define PLATFORM_LIGHTING_UPDATES_PER_FRAME        256
#undef  PLATFORM_LIGHTING_INTERACTIVE_QUEUE_MAX
#define PLATFORM_LIGHTING_INTERACTIVE_QUEUE_MAX    256
#undef  PLATFORM_LIGHTING_INTERACTIVE_BURST
#define PLATFORM_LIGHTING_INTERACTIVE_BURST        2048
#undef  PLATFORM_LIGHTING_BUDGET_US
#define PLATFORM_LIGHTING_BUDGET_US                3000
#undef  PLATFORM_LIGHTING_MERGE_SCAN
#define PLATFORM_LIGHTING_MERGE_SCAN               96
#undef  PLATFORM_LIGHTING_QUEUE_HARD_CAP
#define PLATFORM_LIGHTING_QUEUE_HARD_CAP           8192

// Random ticks: 25 columns at radius 2 instead of the desktop's 81-chunk
// sweep, a quarter of the per-chunk probe rate, round-robin across 6 columns
// per world tick.
#undef  PLATFORM_RANDOM_TICK_CHUNK_RADIUS
#define PLATFORM_RANDOM_TICK_CHUNK_RADIUS          2
#undef  PLATFORM_RANDOM_BLOCK_TICKS_PER_CHUNK
#define PLATFORM_RANDOM_BLOCK_TICKS_PER_CHUNK      10
#undef  PLATFORM_RANDOM_TICK_CHUNKS_PER_TICK
#define PLATFORM_RANDOM_TICK_CHUNKS_PER_TICK       6

// Entity CPU guardrails (Wii values, radii scaled to TINY's 32-block reach):
// entities beyond the terrain distance stand on ground that is not drawn, so
// skip their model submission and their push scans; cap live mobs and A* work
// so a spawn pass cannot eat the tick.
#undef  PLATFORM_LIMIT_ENTITY_RENDER_DISTANCE
#define PLATFORM_LIMIT_ENTITY_RENDER_DISTANCE      1
#undef  PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS
#define PLATFORM_ENTITY_RENDER_RADIUS_BLOCKS       32.0f
#undef  PLATFORM_LIMIT_ENTITY_PUSH_COLLISIONS
#define PLATFORM_LIMIT_ENTITY_PUSH_COLLISIONS      1
#undef  PLATFORM_ENTITY_PUSH_COLLISION_RADIUS_BLOCKS
#define PLATFORM_ENTITY_PUSH_COLLISION_RADIUS_BLOCKS 32.0f
#undef  PLATFORM_MAX_LIVE_MOBS
#define PLATFORM_MAX_LIVE_MOBS                     8
#undef  PLATFORM_PATHFIND_BUDGET_PER_TICK
#define PLATFORM_PATHFIND_BUDGET_PER_TICK          2
#undef  PLATFORM_PATHFIND_MAX_NODES
#define PLATFORM_PATHFIND_MAX_NODES                160
#undef  PLATFORM_MOB_SPAWN_INTERVAL_TICKS
#define PLATFORM_MOB_SPAWN_INTERVAL_TICKS          8
#undef  PLATFORM_MOB_SPAWN_Y_BAND
#define PLATFORM_MOB_SPAWN_Y_BAND                  24

// Decision-AI throttle (PLATFORM_THROTTLE_ENTITY_AI), the Wii's tick-rate
// policy over player distance scaled to TINY's reach: inside NEAR the mob
// thinks every tick as on desktop; between NEAR and FAR every 2nd; past FAR --
// at and beyond the 32-block fog edge, where the terrain the mob stands on is
// not drawn -- every 4th. The entity-round-robin keeps the spread even (the
// (ticksExisted + entityId) & (divisor-1) phase), so no visible mob ever
// freezes: everything inside NEAR runs at full rate.
#undef  PLATFORM_ENTITY_AI_NEAR_RADIUS_BLOCKS
#define PLATFORM_ENTITY_AI_NEAR_RADIUS_BLOCKS      16.0f
#undef  PLATFORM_ENTITY_AI_FAR_RADIUS_BLOCKS
#define PLATFORM_ENTITY_AI_FAR_RADIUS_BLOCKS       32.0f
#undef  PLATFORM_ENTITY_AI_MID_TICK_DIVISOR
#define PLATFORM_ENTITY_AI_MID_TICK_DIVISOR        2
#undef  PLATFORM_ENTITY_AI_FAR_TICK_DIVISOR
#define PLATFORM_ENTITY_AI_FAR_TICK_DIVISOR        4

// Async chunk generation (PLATFORM_ASYNC_CHUNK_GENERATION), the Wii's
// streaming shape on the 3DS's own cores. PlatformAsyncTuning.h maps these
// onto the shared scheduler knobs; the rationale mirrors
// wii/tuning/WiiStreamingTuning.h:
//
//   QUEUE 16      bounds the unpublished backlog (results are not counted
//                 against the resident cache), ~1.3 MB worst case as on the
//                 Wii -- against 64 MB of heap that is the right order.
//   REQUESTS 16/tick, 4/frame   feeding the queue is a handful of hash
//                 lookups; the per-frame pass keeps a fast-walking player from
//                 serialising one dependency per frame.
//   PUBLISH 1/frame, 0/tick     the publish (structures + decoration + Chunk
//                 construction + skylight, on the game thread) is the real
//                 per-column cost. Frames outnumber ticks, so one per frame
//                 is the whole budget: ~30 columns/s against the 3-5 a
//                 walking player exposes, and never two publishes in one
//                 33 ms frame.
//   PRIORITY 32   strictly below the main thread in the shared scale
//                 (Thread.cpp maps it to the lowest 3DS userland priority),
//                 the Wii's "worker gets the gap the frame already contains"
//                 rule for the fallback core. On the dedicated core the
//                 priority is moot -- nothing else runs there.
//   AFFINITY core 1   the ARM11 the OS only grants after
//                 APT_SetAppCpuTimeLimit() (see main_3ds.cpp): real
//                 parallelism rather than time-slicing core 0. If the APT
//                 call fails, Thread.cpp falls back to the default core and
//                 the publish budget alone still keeps the frame whole.
//   ISOLATED BIOME SOURCE 1   the worker samples biomes through its own
//                 WorldChunkManager; the world's BiomeCache is game-thread
//                 state (same reasoning as the Wii).
//   CHUNK DECODE 1   saved worlds pay their NBT parse, block/light arrays
//                 and heightmap on the worker, so walking into explored
//                 terrain publishes without the decode hitch.
//   NEAREST FIRST 1   the queue is 16 entries; taking the column closest to
//                 the player first makes the visible hole fill before the
//                 far edge.
#define PLATFORM_3DS_ASYNC_GENERATION_QUEUE_LIMIT        16
#define PLATFORM_3DS_ASYNC_GENERATION_REQUESTS_PER_TICK   16
#define PLATFORM_3DS_ASYNC_GENERATION_PUBLISH_PER_TICK     0
#define PLATFORM_3DS_ASYNC_GENERATION_REQUESTS_PER_FRAME    4
#define PLATFORM_3DS_ASYNC_GENERATION_PUBLISH_PER_FRAME    1
#define PLATFORM_3DS_ASYNC_GENERATION_THREAD_PRIORITY     32
#define PLATFORM_3DS_ASYNC_GENERATION_AFFINITY_MASK      (1u << 1)
#define PLATFORM_3DS_ASYNC_ISOLATED_BIOME_SOURCE           1
#define PLATFORM_3DS_ASYNC_CHUNK_DECODE                    1
#define PLATFORM_3DS_ASYNC_NEAREST_FIRST                   1

// Network thread placement: the read/write workers pin to the same second
// core the generator uses. The two never overlap -- the generator only
// exists in singleplayer, the socket pair only in multiplayer -- so in a
// network session this is a whole ARM11 for the recv churn and the Packet51
// zlib inflates that the game's core was otherwise sharing (2026-09-28:
// the starved writer sent digs late enough for the server to revert them,
// which read as "breaking blocks does nothing").
#undef  PLATFORM_NETWORK_THREAD_AFFINITY_MASK
#define PLATFORM_NETWORK_THREAD_AFFINITY_MASK      (1u << 1)

// Streaming schedule: when the GAME THREAD itself does worldgen work. The
// values are the PS2's (Ps2WorldTuning.h) because the 3DS shares that
// machine's power class (ARM11 at 268 MHz against the EE's 292 MHz), while
// the model around them is the Wii's async worker above -- and on every knob
// below PS2 and Wii agree anyway. All of them override desktop defaults the
// 3DS was still inheriting; the defaults are the outliers, not these. Fresh
// worlds showed the cost of shipping the table as-is: 0-fps tirones on every
// border crossing while the tutorial (saved, already-decorated chunks) was
// smooth.
//
//   GENERATE_SYNC_RADIUS 0 (desktop 1)
//     The synchronous radius is a full provideChunk() -- base terrain, caves,
//     ravines, structures, lighting -- run inline on the game thread for every
//     column within Chebyshev distance 1 of the player. Worse with
//     deferred decoration off: publishPreparedChunk() then runs up to four
//     complete populate() calls inline as well (the 2x2 group around each
//     fresh corner, ChunkProvider.cpp's populate block after the enqueue
//     site). One border crossing with the worker a step behind = tens of
//     milliseconds in a single frame, which is exactly the stutter seen
//     walking into terrain generated from scratch -- the tutorial never hit
//     it because decoded chunks skip generation and arrive decorated. PS2 and
//     Wii both run 0: the ring is requested through the async queue, and only
//     the exact column under the player stays synchronous (cheb <= 0, and
//     EntityRenderer re-centres curChunkX/Z every rendered frame), so
//     standing still can never step onto air while walking pays the async
//     wait instead of a frame-long stall.
//
//   GENERATE_CHUNKS_PER_TICK 1 (desktop 0)
//     The valve behind that queue: when requestChunkDetailed() reports
//     QueueFull (16 entries pending + in flight + unpublished) or the
//     scheduler is not running, this gate in provideChunk() is what still
//     fills the world one synchronous column per tick instead of returning
//     blankChunk forever -- the desktop 0 returns blank unconditionally.
//     PS2 and Wii: 1.
//
//   DEFERRED_POPULATE 1 (desktop 0), with its four travel companions
//     The decoration queue, and the second half of the fix:
//       * off, publishPreparedChunk() runs the inline populate() burst above,
//         and drainAsyncGeneratedChunks() -- the ASYNC publish path --
//         enqueues no populate at all, so columns the worker produced ahead
//         of the player only ever decorated if some later synchronous
//         publish happened to cover their 2x2 group. Walking ahead of the
//         worker left terrain-complete, undecorated chunks (bare trees/ores).
//       * on, both publish paths enqueue the 2x2 group and the tick drain in
//         unload100OldestChunks() decorates under a wall-clock budget.
//         PC_LEGACY, Wii and PS2 all run 1; the desktop 0 is the parity
//         default.
//     enqueuePopulate() refuses entries when POPULATE_CHUNKS_PER_TICK <= 0
//     and drainPendingPopulate() no-ops on a 0-step budget, so the whole
//     group moves together -- as the PS2's:
//
//   INCREMENTAL_POPULATE 1    one BiomeDecorator feature per step so a budget
//     can stop mid-populate and resume next tick (the stage machine in
//     ChunkProviderGeneratePopulateIncremental.cpp; also 1 on PS2, Wii and
//     PC_LEGACY). Without it each "step" is a whole populate().
//   POPULATE_CHUNKS_PER_TICK 1    one 2x2 group at a time through the
//     canPopulateChunk() neighbour gate (PS2: 1, Wii: 1).
//   POPULATE_STEPS_PER_TICK 8    feature steps per tick, the PS2's share
//     (Wii runs 48 under a 2500 us clock instead). ~160 steps/s against the
//     ~0.3-1 columns a walking player exposes stays ahead of demand.
//   POPULATE_STEPS_AFTER_PUBLISH 0    the PS2's stand-down value, and dead
//     code besides on this platform: publishes happen on the FRAME path
//     (PUBLISH_PER_TICK 0 above), and the tick path's
//     drainAsyncGeneratedChunks(0) returns false at once, so
//     publishedThisTick is always false and the full 8 steps run every tick.
//     (Wii's non-zero 8 exists because its tick path does publish; see the
//     comment at the gate in ChunkProvider.cpp.)
//   POPULATE_BUDGET_US 4000    wall-clock ceiling on that same drain, the
//     PS2's figure, clamped through PlatformStreamingFrameBudget.
//   POPULATE_SNOW_COLUMNS_PER_STEP 32    the desktop 256-column snow pass in
//     a single step is itself a spike; both consoles use 32.
//
//   STREAMING_FRAME_BUDGET_US 8000 (desktop 0 = off)
//     The shared per-frame cap over generation, populate and lighting
//     (platform/world/StreamingFrameBudget.h, begun each frame in
//     Minecraft.cpp). With it off every subsystem spends its INDEPENDENT
//     limit in the same rendered frame -- lighting's 3 ms plus populate's
//     4 ms plus a publish stacked into one 33 ms frame, the exact spike the
//     Wii header warns about at its own 6000. 8000 is the PS2's number and
//     leaves those two budgets headroom inside it.
#undef  PLATFORM_GENERATE_SYNC_RADIUS
#define PLATFORM_GENERATE_SYNC_RADIUS                0
#undef  PLATFORM_GENERATE_CHUNKS_PER_TICK
#define PLATFORM_GENERATE_CHUNKS_PER_TICK            1
#undef  PLATFORM_DEFERRED_POPULATE
#define PLATFORM_DEFERRED_POPULATE                   1
#undef  PLATFORM_INCREMENTAL_POPULATE
#define PLATFORM_INCREMENTAL_POPULATE                1
#undef  PLATFORM_POPULATE_CHUNKS_PER_TICK
#define PLATFORM_POPULATE_CHUNKS_PER_TICK            1
#undef  PLATFORM_POPULATE_STEPS_PER_TICK
#define PLATFORM_POPULATE_STEPS_PER_TICK             8
#undef  PLATFORM_POPULATE_STEPS_AFTER_PUBLISH
#define PLATFORM_POPULATE_STEPS_AFTER_PUBLISH        0
#undef  PLATFORM_POPULATE_BUDGET_US
#define PLATFORM_POPULATE_BUDGET_US                  4000
#undef  PLATFORM_POPULATE_SNOW_COLUMNS_PER_STEP
#define PLATFORM_POPULATE_SNOW_COLUMNS_PER_STEP      32
#undef  PLATFORM_STREAMING_FRAME_BUDGET_US
#define PLATFORM_STREAMING_FRAME_BUDGET_US           8000

// -----------------------------------------------------------------------------
// World-generation profile: DS_FAST_WORLDGEN
// -----------------------------------------------------------------------------
// The PS2's PS2_FAST_WORLDGEN=1 table (Ps2WorldTuning.h) applied knob for
// knob, taking the Wii's value wherever the two consoles differ and the
// console consensus for the hot paths they both ship. Generation is the
// dominant worker cost, so this is what keeps the async queue above from
// ever filling: a worker that finishes a column ten times faster almost
// never triggers the GENERATE_CHUNKS_PER_TICK synchronous fallback, and the
// rare valve tick is itself cheaper because it runs the same lite
// generator. Set the define to 0 to restore the desktop counts (the
// gated knobs below read it the way PS2_FAST_WORLDGEN/WII_FAST_WORLDGEN
// are read); the unconditional console defaults stay either way, exactly as
// on the PS2.
#ifndef DS_FAST_WORLDGEN
#define DS_FAST_WORLDGEN 1
#endif

// Terrain core: the 2D heightmap generator (ChunkProviderGenerateLite.cpp)
// instead of the vanilla 3D double-precision density field -- both consoles
// take it (PS2_USE_HEIGHTMAP_TERRAIN / WII_USE_HEIGHTMAP_TERRAIN). On a CPU
// without hardware doubles the 16-octave double Perlin sweep is the stall
// that made a border crossing cost seconds on PS2; the heightmap path is
// roughly an order of magnitude cheaper and runs per column in float (the
// ARM11 has no hardware double either). Trade: no overhangs or floating
// islands; caves and everything below the surface still carve as before.
// Heightmap parameters are the consoles' shared 63/64/28/0.35 (the desktop
// fallback branch would use 64/64/24/0.5).
#undef  PLATFORM_USE_HEIGHTMAP_TERRAIN
#define PLATFORM_USE_HEIGHTMAP_TERRAIN             1
#undef  PLATFORM_FAST_SURFACE_PASS
#define PLATFORM_FAST_SURFACE_PASS                 (DS_FAST_WORLDGEN && PLATFORM_USE_HEIGHTMAP_TERRAIN)
#undef  PLATFORM_PRECOMPUTE_INITIAL_HEIGHTMAP
#define PLATFORM_PRECOMPUTE_INITIAL_HEIGHTMAP      DS_FAST_WORLDGEN
#undef  PLATFORM_FLOAT_TERRAIN_NOISE
#define PLATFORM_FLOAT_TERRAIN_NOISE               1
#undef  PLATFORM_HEIGHTMAP_SEA_LEVEL
#define PLATFORM_HEIGHTMAP_SEA_LEVEL               63
#undef  PLATFORM_HEIGHTMAP_BASE_HEIGHT
#define PLATFORM_HEIGHTMAP_BASE_HEIGHT             64
#undef  PLATFORM_HEIGHTMAP_AMPLITUDE
#define PLATFORM_HEIGHTMAP_AMPLITUDE               28
#undef  PLATFORM_HEIGHTMAP_FREEZE_TEMP
#define PLATFORM_HEIGHTMAP_FREEZE_TEMP             0.35f

// Caves and ravines, the PS2's fast sweep. Java scans a source radius of 8
// (17x17 chunks = 289 generateChunk calls) around every target chunk; radius
// 3 is 49, and ravines -- which share that sweep's per-chunk Random
// re-seeds for a 1-in-50 roll -- are skipped entirely (Pocket Edition never
// had them). Caves stay ON (the PS2 keeps them too): they are the cheapest
// of the three and the world still reads as Minecraft. Cave geometry in
// float for the same no-hardware-double reason. The octave counts only
// matter when the heightmap is rolled off; kept at the PS2's 12/6 either
// way (vanilla 16/8).
#undef  PLATFORM_SKIP_CAVE_GENERATION
#define PLATFORM_SKIP_CAVE_GENERATION              0
#undef  PLATFORM_CAVE_SOURCE_RADIUS
#define PLATFORM_CAVE_SOURCE_RADIUS                (DS_FAST_WORLDGEN ? 3 : 8)
#undef  PLATFORM_SKIP_RAVINE_GENERATION
#define PLATFORM_SKIP_RAVINE_GENERATION            (DS_FAST_WORLDGEN ? 1 : 0)
#undef  PLATFORM_FLOAT_CAVE_GENERATION
#define PLATFORM_FLOAT_CAVE_GENERATION             1
#undef  PLATFORM_TERRAIN_DENSITY_OCTAVES
#define PLATFORM_TERRAIN_DENSITY_OCTAVES           12
#undef  PLATFORM_TERRAIN_SELECT_OCTAVES
#define PLATFORM_TERRAIN_SELECT_OCTAVES            6

// Publish-side cost -- the async publish is structures + chunk-local
// decoration + Chunk construction + skylight on the game thread, one per
// frame, and these three shrink exactly that lump (both consoles ship all
// three):
//   BATCH_INITIAL_SKYLIGHT_RENDER_UPDATES   collapse the thousands of
//     per-block renderer invalidations emitted while initial skylight is
//     filled into at most one dirty range per section.
//   BULK_GENERATED_CHUNK_IMPORT   take the block/random-tick counts from
//     the fill loop that imports the generator's 32 KB buffer instead of
//     rescanning every cell of every allocated section afterwards --
//     20k-32k redundant iterations per column. Identical counts, not
//     approximated.
//   SEED_GAP_LIGHTING_ON_RELIGHT 0   skip the 256-column cross-chunk
//     gap-lighting seed on relight. With heightmap terrain there are no
//     overhangs to light sideways, so the vertical pass is already the
//     answer; the visible cost is a cave mouth reading as an abrupt dark
//     edge instead of a short gradient -- the PS2's trade.
#undef  PLATFORM_BATCH_INITIAL_SKYLIGHT_RENDER_UPDATES
#define PLATFORM_BATCH_INITIAL_SKYLIGHT_RENDER_UPDATES 1
#undef  PLATFORM_BULK_GENERATED_CHUNK_IMPORT
#define PLATFORM_BULK_GENERATED_CHUNK_IMPORT       1
#undef  PLATFORM_SEED_GAP_LIGHTING_ON_RELIGHT
#define PLATFORM_SEED_GAP_LIGHTING_ON_RELIGHT      0

// Boundary faces: hide the opaque faces drawn against a column that has not
// streamed in yet (RenderBlocks.cpp, shared renderer path). Pairs with the
// cache radius above: the hole at the streaming edge reads as open air
// rather than a wall of dirt. This is the half of the PS2 mesh-edge policy
// that lives in common code (the other half, MESH_WAIT_FOR_PENDING_SOURCES,
// is read only by WorldRendererPs2 and does not exist on this path).
#undef  PLATFORM_CULL_MISSING_CHUNK_BOUNDARY_FACES
#define PLATFORM_CULL_MISSING_CHUNK_BOUNDARY_FACES 1

// Decoration: vegetation and ores written AT generation into the flat block
// buffer (ChunkProviderGenerateDecorateLocal.cpp) instead of arriving
// through setBlock on a live chunk. This is the PS2/Wii fast-profile shape
// and it is what makes the budgets above hold: the deferred populate queue
// then only places structures, lakes, dungeons, springs and animal groups,
// so it drains faster than a fast worker can fill it -- no decoration
// backlog, no trees popping in with a lighting job and a remesh each. The
// worker never touches this (decoration runs inside finishAsyncChunkData on
// the game thread, consistent with the ChunkProvider.cpp construction
// comment). Trade: worldgen changes -- trees and veins are clipped at the
// chunk border, the early Pocket Edition look, and the decoration RNG no
// longer shares its stream with structures and lakes. Saved chunks are
// unaffected.
//
// The mesh-defer pair is the consoles' conditional: with chunk-local
// decoration the late populate only places structures and liquids, none
// worth holding a section's first mesh for, so 1.0e12 never matches and
// the deferral stays effectively off; rolled back, 1024 (32 blocks) keeps
// a deferred populate from double-meshing its trees.
#undef  PLATFORM_CHUNK_LOCAL_DECORATION
#define PLATFORM_CHUNK_LOCAL_DECORATION            (DS_FAST_WORLDGEN ? 1 : 0)
#undef  PLATFORM_DEFER_MESH_DURING_POPULATE
#define PLATFORM_DEFER_MESH_DURING_POPULATE        1
#undef  PLATFORM_POPULATE_MESH_DEFER_DISTANCE_SQ
#define PLATFORM_POPULATE_MESH_DEFER_DISTANCE_SQ   (PLATFORM_CHUNK_LOCAL_DECORATION ? 1.0e12f : 1024.0f)

// Populate counts, the PS2's fast table over the desktop values in
// parentheses (vanilla where the desktop branch is vanilla). These are what
// the budgeted tick drain and the chunk-local pass both spend per column;
// the spring passes are called out on the PS2 as the worst offenders
// (vanilla scatters 50 water + 20 lava attempts).
#undef  PLATFORM_POPULATE_WATER_SPRINGS
#define PLATFORM_POPULATE_WATER_SPRINGS            (DS_FAST_WORLDGEN ? 4 : 50)
#undef  PLATFORM_POPULATE_LAVA_SPRINGS
#define PLATFORM_POPULATE_LAVA_SPRINGS             (DS_FAST_WORLDGEN ? 2 : 20)
#undef  PLATFORM_POPULATE_DUNGEONS
#define PLATFORM_POPULATE_DUNGEONS                 (DS_FAST_WORLDGEN ? 2 : 8)
#undef  PLATFORM_POPULATE_LAKES
#define PLATFORM_POPULATE_LAKES                    (DS_FAST_WORLDGEN ? 0 : 1)
#undef  PLATFORM_FLOWER_PLACEMENT_ATTEMPTS
#define PLATFORM_FLOWER_PLACEMENT_ATTEMPTS         (DS_FAST_WORLDGEN ? 16 : 64)
#undef  PLATFORM_TALL_GRASS_PLACEMENT_ATTEMPTS
#define PLATFORM_TALL_GRASS_PLACEMENT_ATTEMPTS     (DS_FAST_WORLDGEN ? 32 : 128)
#undef  PLATFORM_POPULATE_TREES_PER_CHUNK_MAX
#define PLATFORM_POPULATE_TREES_PER_CHUNK_MAX      (DS_FAST_WORLDGEN ? 2 : -1)
#undef  PLATFORM_POPULATE_GRASS_PER_CHUNK_MAX
#define PLATFORM_POPULATE_GRASS_PER_CHUNK_MAX      (DS_FAST_WORLDGEN ? 2 : -1)
#undef  PLATFORM_POPULATE_JUNGLE_HUGE_TREES
#define PLATFORM_POPULATE_JUNGLE_HUGE_TREES        (DS_FAST_WORLDGEN ? 0 : 1)
#undef  PLATFORM_POPULATE_JUNGLE_VINES
#define PLATFORM_POPULATE_JUNGLE_VINES             (DS_FAST_WORLDGEN ? 8 : 50)
#undef  PLATFORM_POPULATE_CLAY_VEINS
#define PLATFORM_POPULATE_CLAY_VEINS               (DS_FAST_WORLDGEN ? 2 : 10)
#undef  PLATFORM_POPULATE_DIRT_VEINS
#define PLATFORM_POPULATE_DIRT_VEINS               (DS_FAST_WORLDGEN ? 4 : 20)
#undef  PLATFORM_POPULATE_GRAVEL_VEINS
#define PLATFORM_POPULATE_GRAVEL_VEINS             (DS_FAST_WORLDGEN ? 2 : 10)
#undef  PLATFORM_POPULATE_SNOW_PASS
#define PLATFORM_POPULATE_SNOW_PASS                (DS_FAST_WORLDGEN ? 0 : 1)
