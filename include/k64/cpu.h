// kestrel64-sdk — VR4300 primitives: COP0, cache maintenance, timing.
//
// The CPU caches are NOT coherent with the RCP. Rule of thumb, enforced by API names:
//   CPU wrote, RCP will read  -> k64_dcache_writeback(ptr, len)
//   RCP wrote, CPU will read  -> k64_dcache_invalidate(ptr, len)
// D$ = 8 KB direct-mapped, 16 B lines. I$ = 16 KB direct-mapped, 32 B lines.
#pragma once
#include "hw.h"

#define K64_DCACHE_SIZE  8192
#define K64_DCACHE_LINE  16
#define K64_ICACHE_SIZE  16384
#define K64_ICACHE_LINE  32

// ---- COP0 ------------------------------------------------------------------
#define K64_C0_READ(reg) ({ u32 _v; __asm__ volatile("mfc0 %0, $" #reg : "=r"(_v)); _v; })
#define K64_C0_WRITE(reg, v) __asm__ volatile("mtc0 %0, $" #reg "\n\tnop" :: "r"((u32)(v)))

K64_INLINE u32  k64_count(void)          { return K64_C0_READ(9); }
K64_INLINE u32  k64_status(void)         { return K64_C0_READ(12); }
K64_INLINE void k64_set_status(u32 v)    { K64_C0_WRITE(12, v); }

// Count runs at 46.875 MHz: 1 us = 46.875 ticks.
#define K64_TICKS_PER_US   47u
#define K64_US(t)          ((u32)(((u64)(t) * 1000000u) / K64_COUNT_HZ))
K64_INLINE void k64_wait_ticks(u32 t) { u32 s = k64_count(); while ((u32)(k64_count() - s) < t) {} }
K64_INLINE void k64_wait_us(u32 us)   { k64_wait_ticks(us * K64_TICKS_PER_US); }

// ---- cache ops ---------------------------------------------------------------
// op codes: (op<<2)|cache. cache: 0=I, 1=D.
#define K64_CACHE_OP(op, addr) __asm__ volatile("cache %0, 0(%1)" :: "i"(op), "r"(addr) : "memory")
enum {
  K64_I_INDEX_INVALIDATE   = 0x00,
  K64_D_INDEX_WB_INV       = 0x01,
  K64_I_INDEX_STORE_TAG    = 0x08,
  K64_D_INDEX_STORE_TAG    = 0x09,
  K64_D_CREATE_DIRTY       = 0x0D,  // allocate line without fetching RDRAM (full-line overwrite only!)
  K64_I_HIT_INVALIDATE     = 0x10,
  K64_D_HIT_INVALIDATE     = 0x11,
  K64_D_HIT_WB_INV         = 0x15,
  K64_D_HIT_WRITEBACK      = 0x19,
};

void k64_dcache_writeback(const void *p, u32 len);
void k64_dcache_invalidate(void *p, u32 len);       // discards dirty data in range!
void k64_dcache_writeback_invalidate(const void *p, u32 len);
void k64_dcache_writeback_all(void);
void k64_icache_invalidate(const void *p, u32 len);
void k64_icache_invalidate_all(void);

// Interrupt enable state (IE bit). Core runtime is polling-based; these exist for
// code that opts into interrupts.
K64_INLINE u32 k64_irq_disable(void) { u32 s = k64_status(); k64_set_status(s & ~1u); return s & 1u; }
K64_INLINE void k64_irq_restore(u32 ie) { k64_set_status((k64_status() & ~1u) | ie); }

K64_INLINE void k64_memory_barrier(void) { __asm__ volatile("sync" ::: "memory"); }
