// Demo 25 — reset button: counts resets in the NMI buffer (kept across a warm boot),
// background blue after power-on, green after a reset. Pressing reset turns the screen red
// and shows how long the pre-NMI warning lasted, until the NMI restarts the ROM.
#include <k64/k64.h>

typedef struct { u32 magic, resets, last_warning_ms; } nmi_state_t;
#define MAGIC 0x4B363452u   // "K64R"

int main(void) {
  nmi_state_t *st = (nmi_state_t *)k64_reset_buffer();
  bool warm = k64_reset_warm();
  if (!warm || st->magic != MAGIC) *st = (nmi_state_t){ .magic = MAGIC };
  else st->resets++;
  k64_printf("[25_reset] %s boot, resets %u, last warning %u ms\n", warm ? "warm" : "cold",
             st->resets, st->last_warning_ms);

  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_reset_watch();
  u16 bg = warm ? k64_rgba16(30, 140, 60, 1) : k64_rgba16(30, 60, 160, 1);
  for (u32 frame = 0;; frame++) {
    const k64_surface_t *fb = k64_display_acquire();
    bool pending = k64_reset_pending();
    if (pending) st->last_warning_ms = K64_US(k64_reset_elapsed()) / 1000;
    u16 c = pending ? k64_rgba16(200, 30, 30, 1) : bg, *px = fb->pixels;
    for (u32 i = 0; i < (u32)fb->width * fb->height; i++) px[i] = c;
    for (u32 n = 0; n < st->resets && n < 30; n++)          // one tick per reset so far
      for (u32 y = 20; y < 40; y++) for (u32 x = 0; x < 6; x++) px[y * fb->width + 10 + n * 10 + x] = 0xFFFF;
    k64_dcache_writeback(px, fb->stride * fb->height);
    k64_display_show(fb);
    if ((frame & 63) == 0) k64_printf("[25_reset] frame %u pending %d\n", frame, pending);
  }
}
