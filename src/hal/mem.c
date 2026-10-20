// kestrel64-sdk — RDRAM zone planner (see include/k64/mem.h for the bank plan).
#include <k64/mem.h>
#include <k64/debug.h>

extern u8 __heap0_start[], __stack_bottom[];

#define B  0x80000000u
#define MB 0x100000u

static u32 rdram_size;
static u32 fb_lo, fb_hi;        // bank 1
static u32 tex_lo, tex_hi;      // bank 2
static u32 b3_lo, b3_hi;        // bank 3: Z grows up from b3_lo, heap grows down from b3_hi
static u32 xp_lo, xp_hi;        // expansion pak banks 4..7 (heap, first choice)
static u32 b0_lo, b0_hi;        // leftover of bank 0 between bss and stack (heap, last choice)

static const char *zone_name[K64_ZONE_COUNT] = { "fb", "z", "tex", "heap" };

void k64_mem_init(void) {
  rdram_size = K64_BOOT_MEMSIZE == 0x800000 ? 0x800000 : 0x400000;
  fb_lo = B + 1*MB;  fb_hi = B + 2*MB;
  tex_lo = B + 2*MB; tex_hi = B + 3*MB;
  b3_lo = B + 3*MB;  b3_hi = B + 4*MB;
  xp_lo = xp_hi = 0;
  if (rdram_size == 0x800000) { xp_lo = B + 4*MB; xp_hi = B + 8*MB; }
  b0_lo = (u32)__heap0_start; b0_hi = (u32)__stack_bottom;
}

u32 k64_mem_total(void) { return rdram_size; }

static void *up(u32 *lo, u32 hi, u32 size, u32 align) {
  u32 p = (*lo + align - 1) & ~(align - 1);
  if (p < *lo || p + size > hi) return 0;
  *lo = p + size; return (void*)p;
}
static void *down(u32 lo, u32 *hi, u32 size, u32 align) {
  if (*hi - lo < size) return 0;
  u32 p = (*hi - size) & ~(align - 1);
  if (p < lo) return 0;
  *hi = p; return (void*)p;
}

void *k64_mem_alloc(k64_zone_t zone, u32 size, u32 align) {
  if (align < 16) align = 16;                  // never share a D$ line between owners
  size = (size + 15) & ~15u;
  void *p = 0;
  switch (zone) {
    case K64_ZONE_FB:  p = up(&fb_lo, fb_hi, size, align);
                       if (!p) p = up(&tex_lo, tex_hi, size, align); break;   // 640x480 spill
    case K64_ZONE_TEX: p = up(&tex_lo, tex_hi, size, align);
                       if (!p && xp_hi) p = up(&xp_lo, xp_hi, size, align); break;
    case K64_ZONE_Z:   p = up(&b3_lo, b3_hi, size, align); break;
    case K64_ZONE_HEAP:
      if (xp_hi) p = up(&xp_lo, xp_hi, size, align);
      if (!p) p = down(b3_lo, &b3_hi, size, align);
      if (!p) p = up(&b0_lo, b0_hi, size, align);
      break;
    default: break;
  }
  if (!p) k64_panic("k64_mem_alloc: zone %s out of memory (%u bytes, caller %08x)", zone_name[zone], size,
                    (unsigned)(uintptr_t)__builtin_return_address(0));
  return p;
}

u32 k64_mem_free_bytes(k64_zone_t zone) {
  switch (zone) {
    case K64_ZONE_FB:   return fb_hi - fb_lo;
    case K64_ZONE_TEX:  return tex_hi - tex_lo;
    case K64_ZONE_Z:    return b3_hi - b3_lo;
    case K64_ZONE_HEAP: return (xp_hi - xp_lo) + (b3_hi - b3_lo) + (b0_hi - b0_lo);
    default: return 0;
  }
}

u32 k64_mem_largest_free(k64_zone_t zone) {
  u32 m = 0, r[3];
  switch (zone) {
    case K64_ZONE_FB:   r[0] = fb_hi - fb_lo; r[1] = tex_hi - tex_lo; r[2] = 0; break;
    case K64_ZONE_TEX:  r[0] = tex_hi - tex_lo; r[1] = xp_hi - xp_lo; r[2] = 0; break;
    case K64_ZONE_Z:    r[0] = b3_hi - b3_lo; r[1] = r[2] = 0; break;
    case K64_ZONE_HEAP: r[0] = xp_hi - xp_lo; r[1] = b3_hi - b3_lo; r[2] = b0_hi - b0_lo; break;
    default: return 0;
  }
  for (int i = 0; i < 3; i++) if (r[i] > m) m = r[i];
  return m & ~15u;
}

void k64_mem_dump(void) {
  k64_printf("[k64] rdram %u KB\n", rdram_size >> 10);
  for (int z = 0; z < K64_ZONE_COUNT; z++)
    k64_printf("[k64]   %-4s free %u KB\n", zone_name[z], k64_mem_free_bytes((k64_zone_t)z) >> 10);
}

void k64_arena_init(k64_arena_t *a, void *mem, u32 size) {
  a->base = a->cur = (u8*)mem; a->end = (u8*)mem + size;
}

// ---- MI repeat fill -----------------------------------------------------------------------
#include <k64/cpu.h>

static void fill_cached(u8 *p, u32 value, u32 len) {
  for (; len; len--, p++) *p = (u8)(value >> (24 - 8 * ((u32)p & 3)));
}

void k64_mem_fill(void *dst, u32 value, u32 len) {
  u8 *p = dst, *end = p + len;
  u8 *a = (u8 *)(((u32)p + 15) & ~15u), *b = (u8 *)((u32)end & ~15u);
  if (b <= a) { fill_cached(p, value, len); return; }
  fill_cached(p, value, (u32)(a - p));
  fill_cached(b, value, (u32)(end - b));
  // The whole lines are about to be overwritten in RDRAM: drop them from D$ (nothing of them
  // is kept, so no write-back), then one aligned uncached store per <= 128 bytes. Aligned to
  // 16 so no transfer hits the RI's unaligned-carry bug.
  k64_dcache_invalidate(a, (u32)(b - a));
  volatile u32 *u = K64_KSEG1(a);
  for (u32 left = (u32)(b - a); left;) {
    u32 ie = k64_irq_disable();                   // 16 transfers (2 KB) per masked stretch
    for (int k = 0; k < 16 && left; k++) {
      u32 n = left > 128 ? 128 : left;
      MI_MODE = MI_MODE_SET_REPEAT | (n - 1);
      *u = value;
      u += n / 4;
      left -= n;
    }
    k64_irq_restore(ie);
  }
}
