#pragma once

#ifdef PS2_OPTIMIZATION_VALIDATION

namespace Ps2OptimizationValidation
{
void reportAndReset();

void remoteLivingPhysics(bool fullPhysics);
void remoteInterpolationQuery(bool executed);
void multiplayerItemPhysics(bool skippedSettledPhysics);
void skeletonCullDraw();
void particleFastMove();
void particleLayerCapEviction();

void terrainFogCull(int pass);
void terrainClusters(int tested, int rejected, int clipSafe, int guardRisk);
void terrainVu1Submit(int vertices);
void terrainVu1Retry(bool fatal);
void terrainVu0Submit(int vertices);
void translucentQuadRejected();

void enclosedWaterSkip();
void flatWaterFastPath();
void waterMerge(int input, int eligible, int removed);

void lightingDrain(int jobs, bool interactive, bool countExit, bool budgetExit, int queueStart);

void weatherFrame(float rainStrength, int candidates, int rainColumns, int snowColumns,
                  int textureSwitches);

void meshBuildStarted();
void meshBuildPublished(bool dirtyDuringBuild);
void meshDirtyCoalesced();
void meshDirtyRestarted();
void meshUrgentMarked();
void meshUrgentFinished(bool published);
}

#else

namespace Ps2OptimizationValidation
{
inline void reportAndReset() {}
inline void remoteLivingPhysics(bool) {}
inline void remoteInterpolationQuery(bool) {}
inline void multiplayerItemPhysics(bool) {}
inline void skeletonCullDraw() {}
inline void particleFastMove() {}
inline void particleLayerCapEviction() {}
inline void terrainFogCull(int) {}
inline void terrainClusters(int, int, int, int) {}
inline void terrainVu1Submit(int) {}
inline void terrainVu1Retry(bool) {}
inline void terrainVu0Submit(int) {}
inline void translucentQuadRejected() {}
inline void enclosedWaterSkip() {}
inline void flatWaterFastPath() {}
inline void waterMerge(int, int, int) {}
inline void lightingDrain(int, bool, bool, bool, int) {}
inline void weatherFrame(float, int, int, int, int) {}
inline void meshBuildStarted() {}
inline void meshBuildPublished(bool) {}
inline void meshDirtyCoalesced() {}
inline void meshDirtyRestarted() {}
inline void meshUrgentMarked() {}
inline void meshUrgentFinished(bool) {}
}

#endif
