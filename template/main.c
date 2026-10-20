// Minimal kestrel64-sdk game loop: clear the screen on the RDP, print some text, move a box
// with the stick. Walked through in docs/GETTING-STARTED.md.
#include <k64/k64.h>

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  int x = 152, y = 112;
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();                  // the controller read runs while we draw
    const k64_surface_t *fb = k64_display_acquire();

    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(16, 24, 48, 255) * 0x10001u, false);   // leaves FILL mode set
    k64_rdp_set_fill_color(k64_rgba16(255, 200, 40, 255) * 0x10001u);  // 16bpp: two pixels
    k64_rdp_fill_rect(x, y, x + 16, y + 16);
    k64_text_printf(8, 8, 0xFFFFFFFF, "hello from kestrel64-sdk  frame %u", frame);

    k64_pad_poll_end();
    x += k64_pads[0].x / 16;
    y -= k64_pads[0].y / 16;
    x = x < 0 ? 0 : x > 304 ? 304 : x;
    y = y < 0 ? 0 : y > 224 ? 224 : y;

    k64_rdp_wait_idle();                   // the RDP is done with this framebuffer
    k64_display_show(fb);                  // shown at the next vblank
  }
}
