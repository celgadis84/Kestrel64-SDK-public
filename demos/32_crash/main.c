// Demo 32 — the crash screen. A: bad load (CPU exception), B: k64_panic, Z: K64_ASSERT,
// each a few calls deep so the backtrace has frames to show. With no button for three
// seconds it does the bad load by itself.
#include <k64/k64.h>

static void crash_hook(char *buf, u32 size) { k64_snprintf(buf, size, "app line: 32_crash demo"); }

__attribute__((noinline)) static u32 level3(volatile u32 *p, int how) {
  if (how == 1) k64_panic("32_crash: k64_panic from level3 (%d)", how);
  K64_ASSERT(how != 2);
  return *p;                              // how 0: p is 1, an address error
}
__attribute__((noinline)) static u32 level2(volatile u32 *p, int how) { return level3(p, how) + 1; }
__attribute__((noinline)) static u32 level1(volatile u32 *p, int how) { return level2(p, how) * 3; }

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_crash_set_hook(crash_hook);
  for (u32 frame = 0;; frame++) {
    const k64_surface_t *fb = k64_display_acquire();
    u16 *px = fb->pixels;
    for (u32 i = 0; i < (u32)fb->width * fb->height; i++) px[i] = k64_rgba16(20, 60, 20, 1);
    k64_dcache_writeback(px, fb->stride * fb->height);
    k64_display_show(fb);
    k64_pad_poll();
    u16 b = k64_pads[0].pressed;
    int how = b & K64_PAD_B ? 1 : b & K64_PAD_Z ? 2 : b & K64_PAD_A || frame == 180 ? 0 : -1;
    if (how >= 0) k64_printf("[32_crash] %u\n", level1((volatile u32 *)1, how));
  }
}
