// Demo 24 — 640x480 interlaced: per-field VI_ORIGIN from the vblank interrupt (no threads).
// Even rows red, odd rows blue: both fields must show, each on its own scanline set.
// A moving 1-pixel-high white line checks rows land where drawn. From frame 4 the thread
// module takes the interrupts: its handler must keep loading the per-field origin.
#include <k64/k64.h>
#include <k64/thread.h>

extern u32 k64_vi_field[4];

int main(void) {
  k64_printf("[24_hires] tv=%d rdram=%uKB\n", k64_tv_type(), k64_mem_total() >> 10);
  k64_display_init(&(k64_display_cfg_t){ .width = 640, .height = 480, .bpp = K64_BPP16, .buffers = 2 });
  k64_mem_dump();
  k64_printf("[24_hires] VI_CTRL %08x V_SYNC %03x Y_SCALE %03x\n", VI_CTRL, VI_V_SYNC, VI_Y_SCALE);

  u16 red = k64_rgba16(200, 40, 40, 1), blue = k64_rgba16(40, 40, 200, 1), white = 0xFFFF;
  u32 seen[2] = { 0, 0 };
  for (u32 frame = 0;; frame++) {
    if (frame == 4) {
      k64_threads_init(10);
      k64_event_set(K64_EVENT_VI, NULL, NULL);   // no listener: the VI line stays on anyway
    }
    const k64_surface_t *fb = k64_display_acquire();
    u16 *px = fb->pixels;
    u32 t0 = k64_count();
    for (u32 y = 0; y < fb->height; y++) {
      u16 c = (y & 1) ? blue : red;
      u16 *line = px + y * fb->width;
      for (u32 x = 0; x < fb->width; x++) line[x] = ((x >> 5) ^ (y >> 5)) & 1 ? c : c & 0xE739;
    }
    u32 ly = (frame * 2 + 1) % fb->height;                  // odd row: field 1 only
    for (u32 x = 0; x < fb->width; x++) px[ly * fb->width + x] = white;
    k64_dcache_writeback(px, fb->stride * fb->height);
    u32 dt = k64_count() - t0;
    k64_display_show(fb);
    seen[VI_V_CURRENT & 1] = VI_ORIGIN;
    if (frame == 2 || frame == 6) {                         // without / with the thread module
      VI_ORIGIN = 0;                                        // only the vblank interrupt restores it
      k64_vi_wait_vblank();
      u32 o = VI_ORIGIN, want = k64_vi_field[(VI_V_CURRENT & 1) * 2];
      k64_printf("[24_hires] isr %s: origin %08x want %08x %s\n", frame == 2 ? "bare" : "threads",
                 o, want, o == want ? "PASS" : "FAIL");
    }
    if (frame < 8 || (frame & 31) == 0)
      k64_printf("[24_hires] frame %u fill %u us fb %08x origin f0 %08x f1 %08x table %08x %08x\n",
                 frame, K64_US(dt), (u32)K64_PHYS(px), seen[0], seen[1], k64_vi_field[0], k64_vi_field[2]);
  }
}
