// Runtime_3ds.cpp -- 3DS implementation of java/Runtime.h.
//
// The three figures follow the Wii's definitions (src/wii/java/Runtime_wii.cpp)
// because the questions are the same: how much may be allocated at most, how
// much of the heap has been committed, and how much of that is free.
//
// The ceiling comes from the application memregion -- the budget malloc grows
// into, and the number the DsBringup report prints (the datasheet's 64 MB /
// 124 MB is the chip, not what the process can actually use; see the
// reportMemory comment in src/3ds/tools/DsBringup.cpp).
//
// Committed comes from the KERNEL's account of that region,
// osGetMemRegionUsed() -- one svcGetSystemInfo query returning everything
// this process has taken from it: the malloc arena, the linear pool, the
// main thread's stack and anything else carved out of the region. mallinfo()
// alone sees only the arena half of that: DsRender's vertex arenas and index
// buffer and every citro3d texture live in the linear pool, drawn from the
// same region and invisible to newlib's statistics. Without the kernel
// figure the F3 line reported just the malloc share ("7 MB of 64 MB",
// 2026-10-07 hardware session) while the process held several megabytes
// more, inviting a "plenty of memory left" conclusion the real budget does
// not back.
//
// Free is allocator-level -- newlib's fordblks plus linearSpaceFree() -- so
// Java's used = committed - free reads as "busy malloc + busy linear +
// non-pool committed (stacks)": the bytes actually held, not the allocator
// headroom. VRAM -- the framebuffers' 6 MB chip pool -- is separate from the
// memregion and stays out of all three figures, by design.
#ifdef CTR_PLATFORM

#include "java/Runtime.h"

#include <3ds.h>

#include <malloc.h>

Runtime Runtime::instance;

Runtime &Runtime::getRuntime()
{
	return instance;
}

long_t Runtime::maxMemory()
{
	// The whole application memregion: everything the sbrk heap can still
	// grow into, sampled the same way the bring-up report does.
	const u32 ceiling = osGetMemRegionSize(MEMREGION_APPLICATION);
	return ceiling > 0 ? static_cast<long_t>(ceiling) : 1;
}

long_t Runtime::totalMemory()
{
	// Committed against the application region, straight from the
	// kernel's own ledger (see the file comment): the malloc arena, the
	// linear pool and the stacks in one figure.
	return static_cast<long_t>(osGetMemRegionUsed(MEMREGION_APPLICATION));
}

long_t Runtime::freeMemory()
{
	// Free inside the committed pools: what newlib's malloc still has
	// unclaimed, plus the linear allocator's free space. The final clamp
	// keeps the Java invariant -- freeMemory must never exceed
	// totalMemory -- even if the three statistics are sampled across an
	// unusual allocator transition (the same defensive clamp the Wii
	// carries, folded into one at the end).
	const struct mallinfo mi = mallinfo();
	const std::uint32_t committed = static_cast<std::uint32_t>(mi.arena);
	const std::uint32_t allocatorFree = static_cast<std::uint32_t>(mi.fordblks);
	const std::uint32_t mallocFree = allocatorFree < committed ? allocatorFree : committed;
	const std::uint32_t linearFreeBytes = linearSpaceFree();
	const std::uint32_t poolFree = mallocFree + linearFreeBytes;
	const u32 regionUsed = osGetMemRegionUsed(MEMREGION_APPLICATION);
	return static_cast<long_t>(poolFree <= regionUsed ? poolFree : regionUsed);
}

#endif // CTR_PLATFORM
