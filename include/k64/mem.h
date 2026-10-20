// kestrel64-sdk — RDRAM planner.
//
// RDRAM is 4 MiB (8 MiB with Expansion Pak), in 1 MiB banks. The RDP reads textures and
// reads/writes color+Z every span; the VI streams the front buffer continuously. Placing
// these streams in distinct banks avoids row/bank thrash. The planner hands out memory
// per "zone" so the layout is decided once, at init, and never fragments:
//
//   zone            default bank        user
//   K64_ZONE_CODE   0 (linker)          code, data, bss, stack
//   K64_ZONE_FB     1 (+2 for 32bpp/3x) color buffers
//   K64_ZONE_Z      last 4MB bank (3)   Z buffer, start of bank
//   K64_ZONE_TEX    2                   texture/vertex/ucode-data (RSP/RDP read-mostly)
//   K64_ZONE_HEAP   whatever is left    game data (bump allocator + frame arenas)
#pragma once
#include "hw.h"

typedef enum { K64_ZONE_FB, K64_ZONE_Z, K64_ZONE_TEX, K64_ZONE_HEAP, K64_ZONE_COUNT } k64_zone_t;

// Detects 4 / 8 MiB and resets every zone to empty. The runtime calls it before main (and
// the boot logo again when it returns): an app calls it only to drop all its allocations.
void  k64_mem_init(void);
// NULL when the zone is full. align is raised to 16 and size rounded up to 16, so no two
// allocations share a D$ line (safe to invalidate one for an RCP write).
void *k64_mem_alloc(k64_zone_t zone, u32 size, u32 align);   // never freed (level-lifetime)
u32   k64_mem_free_bytes(k64_zone_t zone);
u32   k64_mem_largest_free(k64_zone_t zone);                 // biggest single allocation possible
u32   k64_mem_total(void);                                   // RDRAM size in bytes
void  k64_mem_dump(void);                                    // free KB per zone, to k64_printf

// Fills [dst, dst + len) with the big-endian 32-bit pattern `value` (byte i of the range
// gets byte (address & 3) of value), at any alignment. Whole D$ lines in the range are
// filled through the MI repeat mode (n64brew "MIPS Interface", MI_MODE): one uncached
// store writes up to 128 bytes of RDRAM; those lines are invalidated, not written back, so
// the fill leaves the D$ contents alone (a memset of the same size evicts all of it). Not
// faster than memset + writeback in kestrel64 (150 KB: ~1.0 ms vs ~0.6 ms, demo 31);
// unmeasured on hardware.
// The partial lines at both ends are stored through the cache. Interrupts are masked
// around each arm + store pair (any other RDRAM write in between would consume the arm).
void  k64_mem_fill(void *dst, u32 value, u32 len);

// Frame arena: linear scratch reset each frame (display lists, transient vertices).
typedef struct { u8 *base, *cur, *end; } k64_arena_t;
// alloc returns NULL when the arena is full; align must be a power of two. Nothing is
// cleared or written back: memory from it that the RCP reads still needs
// k64_dcache_writeback.
void k64_arena_init(k64_arena_t *a, void *mem, u32 size);
K64_INLINE void k64_arena_reset(k64_arena_t *a) { a->cur = a->base; }
K64_INLINE void *k64_arena_alloc(k64_arena_t *a, u32 size, u32 align) {
  u8 *p = (u8*)(((u32)a->cur + align - 1) & ~(align - 1));
  if (K64_UNLIKELY(p + size > a->end)) return 0;
  a->cur = p + size; return p;
}
