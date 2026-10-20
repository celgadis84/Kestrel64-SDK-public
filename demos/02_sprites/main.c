// Demo 02 — 2D on the RDP: scrolling tiled background (1-cycle, CI4 + TLUT, wrap),
// N bouncing sprites in COPY mode (4 px/clk, alpha-compare), HUD text from an I4 font.
// Shows: RDP command stream, TMEM loads + residency cache, sync elision, baked textures.
#include <k64/k64.h>

extern const u8 ball[], bricks[];

#define NBALLS 256
typedef struct { s32 x, y, vx, vy; } obj_t;   // 16.16
static obj_t objs[NBALLS];

static u32 rng = 0x12345678;
static u32 rnd(void) { rng = rng * 1664525u + 1013904223u; return rng >> 8; }

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  k64_tex_t tball, tbricks;
  K64_ASSERT(k64_tex_from_blob(&tball, ball));
  K64_ASSERT(k64_tex_from_blob(&tbricks, bricks));

  for (int i = 0; i < NBALLS; i++)
    objs[i] = (obj_t){ (s32)(rnd() % 288) << 16, (s32)(rnd() % 208) << 16,
                       (s32)(rnd() % 0x30000) - 0x18000, (s32)(rnd() % 0x30000) - 0x18000 };

  u32 cpu_us = 0, rdp_clk = 0;
  int nballs = 96, scx = 0, scy = 0;
  for (u32 frame = 0;; frame++) {
    u32 t0 = k64_count();
    k64_pad_poll_start();                      // SI runs while we build the frame
    const k64_surface_t *fb = k64_display_acquire();
    u32 c0 = k64_rdp_clock();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, 0);

    // background: one 320x240 rect, texture wraps (mask 5 = 32 texels)
    k64_rdp_set_other_modes(K64_OM_BASE | K64_OM_CYCLE_1 | K64_OM_TLUT |
                            K64_OM_BLEND(K64_BL_IN, K64_BL_A_IN, K64_BL_IN, K64_BL_B_1MA, 0,0,0,0));
    k64_rdp_set_combine(K64_CC_TEX);
    k64_rdp_load_tex(0, 0, &tbricks, K64_TEX_WRAP, K64_TEX_WRAP);
    k64_rdp_tex_rect(0, 0, 0, fb->width, fb->height, scx & 31, scy & 31, 1 << 10, 1 << 10);

    // sprites: copy mode, alpha bit drops the transparent corners
    k64_rdp_set_other_modes(K64_OM_COPY_ALPHA);
    k64_rdp_set_blend_color(0x00000001);
    k64_rdp_load_tex(1, 0, &tball, K64_TEX_CLAMP, K64_TEX_CLAMP);
    for (int i = 0; i < nballs; i++) {
      obj_t *o = &objs[i];
      o->x += o->vx; o->y += o->vy;
      if (o->x < 0 || o->x > (288 << 16)) { o->vx = -o->vx; o->x += o->vx; }
      if (o->y < 0 || o->y > (208 << 16)) { o->vy = -o->vy; o->y += o->vy; }
      k64_rdp_sprite(1, o->x >> 16, o->y >> 16, 32, 32);
    }

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  02 sprites  x%d", nballs);
    k64_text_printf(8, 220, 0xFFE040FF, "cpu %4u us  rdp %6u clk  syncs p%u t%u l%u",
                    cpu_us, rdp_clk, k64_rdps.stat_syncs_pipe, k64_rdps.stat_syncs_tile, k64_rdps.stat_syncs_load);
    cpu_us = K64_US(k64_count() - t0);
    k64_pad_poll_end();
    const k64_pad_t *p = &k64_pads[0];
    if (p->pressed & K64_PAD_A) nballs = nballs + 32 > NBALLS ? NBALLS : nballs + 32;
    if (p->pressed & K64_PAD_B) nballs = nballs - 32 < 0 ? 0 : nballs - 32;
    scx += p->present && (p->x > 16 || p->x < -16) ? p->x / 16 : 1;
    scy -= p->present ? p->y / 16 : 0;
    if (!p->present) scy++;
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_rdps.stat_syncs_pipe = k64_rdps.stat_syncs_tile = k64_rdps.stat_syncs_load = 0;
    k64_display_show(fb);
    if ((frame & 63) == 0) k64_printf("[02] frame %u cpu %u us rdp %u clk pad%d %04x\n", frame, cpu_us, rdp_clk,
                                      k64_pads[0].present, k64_pads[0].held);
  }
}
