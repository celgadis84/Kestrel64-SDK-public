// Demo 01 — boot, VI, double buffer, CPU-drawn gradient + moving bar, debug print.
// Proves: IPL3 handoff, crt0, BSS, cache maintenance, VI timing, ISViewer.
#include <k64/k64.h>

int main(void) {
  k64_printf("[01_hello] kestrel64-sdk up. tv=%d rdram=%uKB\n", k64_tv_type(), k64_mem_total() >> 10);
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_mem_dump();

  for (u32 frame = 0;; frame++) {
    const k64_surface_t *fb = k64_display_acquire();
    u16 *px = fb->pixels;
    u32 t0 = k64_count();
    for (u32 y = 0; y < fb->height; y++) {
      u16 row = k64_rgba16(y, 64, 255 - y, 1);
      u16 *line = px + y * fb->width;
      for (u32 x = 0; x < fb->width; x++) line[x] = row ^ (u16)((x ^ y) & 0x18 ? 0 : 0x0842);
    }
    u32 bar = (frame * 3) % fb->width;
    for (u32 y = 100; y < 140; y++) for (u32 x = bar; x < bar + 16 && x < fb->width; x++)
      px[y * fb->width + x] = 0xFFFF;
    k64_dcache_writeback(px, fb->stride * fb->height);   // CPU wrote -> VI reads
    u32 dt = k64_count() - t0;
    k64_display_show(fb);
    if ((frame & 63) == 0) k64_printf("[01_hello] frame %u cpu-fill %u us\n", frame, K64_US(dt));
  }
}
