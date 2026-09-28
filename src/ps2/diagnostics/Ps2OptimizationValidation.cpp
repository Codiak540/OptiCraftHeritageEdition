#include "ps2/diagnostics/Ps2OptimizationValidation.h"

#ifdef PS2_OPTIMIZATION_VALIDATION

#include <algorithm>
#include <cstdint>

#include "platform/Log.h"
#include "platform/PlatformTuning.h"

namespace
{
struct Counters
{
    std::uint32_t remoteFull = 0;
    std::uint32_t remoteLite = 0;
    std::uint32_t remoteQuery = 0;
    std::uint32_t remoteQuerySkipped = 0;
    std::uint32_t itemFull = 0;
    std::uint32_t itemSettledSkip = 0;
    std::uint32_t skeletonCullDraws = 0;
    std::uint32_t particleFastMoves = 0;
    std::uint32_t particleCapEvictions = 0;

    std::uint32_t fogCullOpaque = 0;
    std::uint32_t fogCullTranslucent = 0;
    std::uint32_t clustersTested = 0;
    std::uint32_t clustersRejected = 0;
    std::uint32_t clustersClipSafe = 0;
    std::uint32_t clustersGuardRisk = 0;
    std::uint32_t vu1Submits = 0;
    std::uint32_t vu1Vertices = 0;
    std::uint32_t vu1Retries = 0;
    std::uint32_t vu1Fatal = 0;
    std::uint32_t vu0Submits = 0;
    std::uint32_t vu0Vertices = 0;
    std::uint32_t translucentQuadRejects = 0;

    std::uint32_t enclosedWaterSkip = 0;
    std::uint32_t flatWaterFast = 0;
    std::uint32_t waterMergeCalls = 0;
    std::uint32_t waterMergeInput = 0;
    std::uint32_t waterMergeEligible = 0;
    std::uint32_t waterMergeRemoved = 0;

    std::uint32_t lightingDrains = 0;
    std::uint32_t lightingInteractive = 0;
    std::uint32_t lightingJobs = 0;
    std::uint32_t lightingCountExit = 0;
    std::uint32_t lightingBudgetExit = 0;
    std::uint32_t lightingQueuePeak = 0;

    std::uint32_t weatherFrames = 0;
    std::uint32_t weatherActiveFrames = 0;
    std::uint32_t weatherCandidates = 0;
    std::uint32_t weatherRainColumns = 0;
    std::uint32_t weatherSnowColumns = 0;
    std::uint32_t weatherTextureSwitches = 0;

    std::uint32_t meshBuildStarted = 0;
    std::uint32_t meshBuildPublished = 0;
    std::uint32_t meshFollowupPublished = 0;
    std::uint32_t meshDirtyCoalesced = 0;
    std::uint32_t meshDirtyRestarted = 0;
    std::uint32_t meshUrgentMarked = 0;
    std::uint32_t meshUrgentPublished = 0;
    std::uint32_t meshUrgentYielded = 0;
};

Counters g_counters;
bool g_configReported = false;

constexpr int enabledFlag(bool enabled)
{
    return enabled ? 1 : 0;
}

void reportConfig()
{
    if (g_configReported)
        return;
    g_configReported = true;

#if defined(PS2_ENABLE_VU1_TERRAIN)
    constexpr bool vu1 = true;
#else
    constexpr bool vu1 = false;
#endif
#if defined(PS2_ENABLE_VU0_MESH_FINALIZE)
    constexpr bool vu0Finalize = true;
#else
    constexpr bool vu0Finalize = false;
#endif
#if defined(PS2_MERGE_WATER_TOPS)
    constexpr bool waterMerge = true;
#else
    constexpr bool waterMerge = false;
#endif
#if defined(PS2_ENABLE_PERSPECTIVE_TEXTURES)
    constexpr bool perspective = true;
#else
    constexpr bool perspective = false;
#endif
#if defined(PS2_ENABLE_PSMT8)
    constexpr bool psmt8 = true;
#else
    constexpr bool psmt8 = false;
#endif
#if defined(PS2_RENDER_STATS)
    constexpr bool renderStats = true;
#else
    constexpr bool renderStats = false;
#endif

    MC_LOG_INFO("ps2.validate",
        "config vu1=%d vu0Finalize=%d waterMerge=%d perspective=%d psmt8=%d renderStats=%d"
        " remotePhysicsDiv=%d meshCoalesce=%d chunkBuild=%dms urgentMesh=%dms"
        " lighting=%d/%dus interactive=%d/%d/%dus\n",
        enabledFlag(vu1), enabledFlag(vu0Finalize), enabledFlag(waterMerge),
        enabledFlag(perspective), enabledFlag(psmt8), enabledFlag(renderStats),
        (int)PLATFORM_MULTIPLAYER_REMOTE_LIVING_PHYSICS_TICK_DIVISOR,
        (int)PLATFORM_COALESCE_MESH_REBUILDS, (int)PLATFORM_CHUNK_BUILD_BUDGET_MS,
        (int)PLATFORM_URGENT_MESH_BUDGET_MS,
        (int)PLATFORM_LIGHTING_UPDATES_PER_FRAME, (int)PLATFORM_LIGHTING_BUDGET_US,
        (int)PLATFORM_LIGHTING_INTERACTIVE_QUEUE_MAX,
        (int)PLATFORM_LIGHTING_INTERACTIVE_BURST,
        (int)PLATFORM_LIGHTING_INTERACTIVE_BUDGET_US);

    MC_LOG_INFO("ps2.validate",
        "config particles fast=%d maxLayer=%d brightness=%d destroyGrid=%d randomDisplay=%d cache=%d reuseRng=%d"
        " rainSplash=%d rainSnow=%d fireLayers=%d greedySlices=%d\n",
        (int)PLATFORM_FAST_PARTICLE_PHYSICS, (int)PLATFORM_MAX_PARTICLES_PER_LAYER,
        (int)PLATFORM_PARTICLE_BRIGHTNESS_INTERVAL, (int)PLATFORM_BLOCK_DESTROY_PARTICLE_GRID,
        (int)PLATFORM_RANDOM_DISPLAY_PROBES, (int)PLATFORM_CACHE_RANDOM_DISPLAY_CHUNKS,
        (int)PLATFORM_REUSE_RANDOM_DISPLAY_RNG, (int)PS2_RAIN_SPLASH_PARTICLES_PER_TICK,
        (int)(!PLATFORM_SKIP_RAIN_SNOW), (int)PS2_ENTITY_FIRE_MAX_LAYERS,
        (int)PS2_GREEDY_SLICES_PER_STEP);
}
}

namespace Ps2OptimizationValidation
{
void reportAndReset()
{
    reportConfig();

    MC_LOG_INFO("ps2.validate",
        "entity remoteFull=%u remoteLite=%u interpQuery=%u interpSkip=%u itemFull=%u itemSettledSkip=%u"
        " skeletonCull=%u particleFastMove=%u particleCapEvict=%u\n",
        g_counters.remoteFull, g_counters.remoteLite,
        g_counters.remoteQuery, g_counters.remoteQuerySkipped,
        g_counters.itemFull, g_counters.itemSettledSkip, g_counters.skeletonCullDraws,
        g_counters.particleFastMoves, g_counters.particleCapEvictions);

    MC_LOG_INFO("ps2.validate",
        "terrain fogCull0=%u fogCull1=%u clusters=%u rejected=%u clipSafe=%u guardRisk=%u"
        " vu1Submit=%u vu1Verts=%u vu1Retry=%u vu1Fatal=%u vu0Submit=%u vu0Verts=%u quadReject=%u\n",
        g_counters.fogCullOpaque, g_counters.fogCullTranslucent,
        g_counters.clustersTested, g_counters.clustersRejected,
        g_counters.clustersClipSafe, g_counters.clustersGuardRisk,
        g_counters.vu1Submits, g_counters.vu1Vertices,
        g_counters.vu1Retries, g_counters.vu1Fatal,
        g_counters.vu0Submits, g_counters.vu0Vertices,
        g_counters.translucentQuadRejects);

    MC_LOG_INFO("ps2.validate",
        "water enclosedSkip=%u flatFast=%u mergeCalls=%u mergeInput=%u mergeEligible=%u mergeRemoved=%u\n",
        g_counters.enclosedWaterSkip, g_counters.flatWaterFast,
        g_counters.waterMergeCalls, g_counters.waterMergeInput,
        g_counters.waterMergeEligible, g_counters.waterMergeRemoved);

    MC_LOG_INFO("ps2.validate",
        "lighting drains=%u interactive=%u jobs=%u countExit=%u budgetExit=%u queuePeak=%u\n",
        g_counters.lightingDrains, g_counters.lightingInteractive,
        g_counters.lightingJobs, g_counters.lightingCountExit,
        g_counters.lightingBudgetExit, g_counters.lightingQueuePeak);

    MC_LOG_INFO("ps2.validate",
        "weather frames=%u active=%u candidates=%u rain=%u snow=%u texSwitch=%u\n",
        g_counters.weatherFrames, g_counters.weatherActiveFrames,
        g_counters.weatherCandidates, g_counters.weatherRainColumns,
        g_counters.weatherSnowColumns, g_counters.weatherTextureSwitches);

    MC_LOG_INFO("ps2.validate",
        "mesh start=%u publish=%u followup=%u coalesce=%u restart=%u urgentMark=%u urgentPublish=%u urgentYield=%u\n",
        g_counters.meshBuildStarted, g_counters.meshBuildPublished,
        g_counters.meshFollowupPublished, g_counters.meshDirtyCoalesced,
        g_counters.meshDirtyRestarted, g_counters.meshUrgentMarked,
        g_counters.meshUrgentPublished, g_counters.meshUrgentYielded);

    g_counters = Counters{};
}

void remoteLivingPhysics(bool fullPhysics)
{
    fullPhysics ? ++g_counters.remoteFull : ++g_counters.remoteLite;
}

void remoteInterpolationQuery(bool executed)
{
    executed ? ++g_counters.remoteQuery : ++g_counters.remoteQuerySkipped;
}

void multiplayerItemPhysics(bool skippedSettledPhysics)
{
    skippedSettledPhysics ? ++g_counters.itemSettledSkip : ++g_counters.itemFull;
}

void skeletonCullDraw()
{
    ++g_counters.skeletonCullDraws;
}

void particleFastMove()
{
    ++g_counters.particleFastMoves;
}

void particleLayerCapEviction()
{
    ++g_counters.particleCapEvictions;
}

void terrainFogCull(int pass)
{
    if (pass == 0)
        ++g_counters.fogCullOpaque;
    else if (pass == 1)
        ++g_counters.fogCullTranslucent;
}

void terrainClusters(int tested, int rejected, int clipSafe, int guardRisk)
{
    if (tested > 0) g_counters.clustersTested += (std::uint32_t)tested;
    if (rejected > 0) g_counters.clustersRejected += (std::uint32_t)rejected;
    if (clipSafe > 0) g_counters.clustersClipSafe += (std::uint32_t)clipSafe;
    if (guardRisk > 0) g_counters.clustersGuardRisk += (std::uint32_t)guardRisk;
}

void terrainVu1Submit(int vertices)
{
    ++g_counters.vu1Submits;
    if (vertices > 0) g_counters.vu1Vertices += (std::uint32_t)vertices;
}

void terrainVu1Retry(bool fatal)
{
    ++g_counters.vu1Retries;
    if (fatal) ++g_counters.vu1Fatal;
}

void terrainVu0Submit(int vertices)
{
    ++g_counters.vu0Submits;
    if (vertices > 0) g_counters.vu0Vertices += (std::uint32_t)vertices;
}

void translucentQuadRejected()
{
    ++g_counters.translucentQuadRejects;
}

void enclosedWaterSkip()
{
    ++g_counters.enclosedWaterSkip;
}

void flatWaterFastPath()
{
    ++g_counters.flatWaterFast;
}

void waterMerge(int input, int eligible, int removed)
{
    ++g_counters.waterMergeCalls;
    if (input > 0) g_counters.waterMergeInput += (std::uint32_t)input;
    if (eligible > 0) g_counters.waterMergeEligible += (std::uint32_t)eligible;
    if (removed > 0) g_counters.waterMergeRemoved += (std::uint32_t)removed;
}

void lightingDrain(int jobs, bool interactive, bool countExit, bool budgetExit, int queueStart)
{
    if (jobs <= 0 && queueStart <= 0)
        return;
    ++g_counters.lightingDrains;
    if (interactive) ++g_counters.lightingInteractive;
    if (jobs > 0) g_counters.lightingJobs += (std::uint32_t)jobs;
    if (countExit) ++g_counters.lightingCountExit;
    if (budgetExit) ++g_counters.lightingBudgetExit;
    if (queueStart > 0)
        g_counters.lightingQueuePeak = std::max(g_counters.lightingQueuePeak, (std::uint32_t)queueStart);
}

void weatherFrame(float rainStrength, int candidates, int rainColumns, int snowColumns,
                  int textureSwitches)
{
    ++g_counters.weatherFrames;
    if (rainStrength > 0.0f) ++g_counters.weatherActiveFrames;
    if (candidates > 0) g_counters.weatherCandidates += (std::uint32_t)candidates;
    if (rainColumns > 0) g_counters.weatherRainColumns += (std::uint32_t)rainColumns;
    if (snowColumns > 0) g_counters.weatherSnowColumns += (std::uint32_t)snowColumns;
    if (textureSwitches > 0) g_counters.weatherTextureSwitches += (std::uint32_t)textureSwitches;
}

void meshBuildStarted()
{
    ++g_counters.meshBuildStarted;
}

void meshBuildPublished(bool dirtyDuringBuild)
{
    ++g_counters.meshBuildPublished;
    if (dirtyDuringBuild) ++g_counters.meshFollowupPublished;
}

void meshDirtyCoalesced()
{
    ++g_counters.meshDirtyCoalesced;
}

void meshDirtyRestarted()
{
    ++g_counters.meshDirtyRestarted;
}

void meshUrgentMarked()
{
    ++g_counters.meshUrgentMarked;
}

void meshUrgentFinished(bool published)
{
    published ? ++g_counters.meshUrgentPublished : ++g_counters.meshUrgentYielded;
}
}

#endif
