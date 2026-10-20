// Demo 09 — RDP mode bench: what FILL and COPY can draw and what each mode costs.
//
// Every test draws into the back buffer, framed by full syncs, and is timed with DPC_CLOCK
// (GCLK, 62.5 MHz). Results go to the log ([09] lines) and stay on screen, so the same ROM
// gives hardware numbers on a flashcart. The last test frame (FILL triangles + COPY sprites +
// a 1-cycle upscaled low-res canvas) is left visible as a visual check.
#include <k64/k64.h>

#define W 320
#define H 240
#define CW 80                               // low-res canvas
#define CH 60

static u16 tex_px[32 * 32] K64_ALIGN(16);
static u16 canvas[CW * CH] K64_ALIGN(16);
static k64_tex_t tex;
static const k64_surface_t *fb;
static u16 *zbuf;

static void target(void) { k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, W, H, fb->pixels, zbuf); }

// wall: DPC_CLOCK over the test (includes waiting for the CPU to write commands);
// busy: DPC_PIPEBUSY, cycles the pipeline actually worked (what Thar0's RDP-Timing-Tests use);
// cpu: CPU time spent writing the commands (us).
typedef struct { u32 wall, busy, cpu_us; } cost_t;
static cost_t base_cost;
static cost_t measure(void (*fn)(void)) {
  k64_rdp_wait_idle();
  u32 c0 = k64_rdp_clock(), p0 = DPC_PIPEBUSY & 0xFFFFFF, t0 = k64_count();
  fn();
  u32 t1 = k64_count();
  k64_rdp_wait_idle();
  return (cost_t){ (k64_rdp_clock() - c0) & 0xFFFFFF, ((DPC_PIPEBUSY & 0xFFFFFF) - p0) & 0xFFFFFF, K64_US(t1 - t0) };
}

static void vtx(k64_svtx_t *v, float x, float y) { *v = (k64_svtx_t){ .x = x, .y = y, .inv_w = 1, .a = 255 }; }
static void screen_tris(u32 flags) {
  k64_svtx_t a, b, c, d;
  vtx(&a, 0, 0); vtx(&b, W, 0); vtx(&c, W, H); vtx(&d, 0, H);
  k64_rdp_tri(flags, 0, &a, &b, &c);
  k64_rdp_tri(flags, 0, &a, &c, &d);
}
static void set_fill(void) {
  k64_rdp_set_other_modes(K64_OM_FILL);
  k64_rdp_set_fill_color(k64_rgba16(40, 80, 160, 255) * 0x10001u);
}
static void set_1c(u64 cyc) {
  k64_rdp_set_other_modes((K64_OM_1C_OPAQUE & ~K64_OM_CYCLE_2) | cyc);
  k64_rdp_set_combine(K64_CC_PRIM);
  k64_rdp_set_prim_color(0x50A0C0FF);
}

static void t_empty(void) {}
static void t_fill_rect(void) { set_fill(); k64_rdp_fill_rect(0, 0, W, H); }
static void t_fill_tri(void) { set_fill(); screen_tris(0); }
static void t_1c_rect(void) { set_1c(K64_OM_CYCLE_1); k64_rdp_fill_rect(0, 0, W, H); }
static void t_1c_tri(void) { set_1c(K64_OM_CYCLE_1); screen_tris(0); }
static void t_2c_rect(void) { set_1c(K64_OM_CYCLE_2); k64_rdp_fill_rect(0, 0, W, H); }
static void t_copy_tiles(void) {
  k64_rdp_set_other_modes(K64_OM_COPY);
  k64_rdp_load_tex(0, 0, &tex, K64_TEX_WRAP, K64_TEX_WRAP);
  for (int y = 0; y < H; y += 32)
    for (int x = 0; x < W; x += 32) k64_rdp_sprite(0, x, y, 32, y + 32 <= H ? 32 : H - y);
}
static void t_copy_wrap(void) {
  k64_rdp_set_other_modes(K64_OM_COPY);
  k64_rdp_load_tex(0, 0, &tex, K64_TEX_WRAP, K64_TEX_WRAP);
  k64_rdp_sprite(0, 0, 0, W, H);
}
// "Canvas": the screen as NxN blocks, one color per block.
static int blk;
static u32 blk_color(int x, int y) { return k64_rgba16((u8)(x * 3), (u8)(y * 5), (u8)((x ^ y) * 7), 255); }
static void t_canvas_fill(void) {           // FILL: a color change needs a pipe sync
  k64_rdp_set_other_modes(K64_OM_FILL);
  for (int y = 0; y < H; y += blk)
    for (int x = 0; x < W; x += blk) {
      k64_rdp_set_fill_color(blk_color(x, y) * 0x10001u);
      k64_rdp_fill_rect(x, y, x + blk, y + blk);
    }
}
static void t_canvas_prim(void) {           // 1-cycle: PRIM color is latched, no sync
  set_1c(K64_OM_CYCLE_1);
  for (int y = 0; y < H; y += blk)
    for (int x = 0; x < W; x += blk) {
      u16 c = (u16)blk_color(x, y);
      k64_rdp_set_prim_color((u32)(c >> 11) << 27 | (u32)((c >> 6) & 31) << 19 | (u32)((c >> 1) & 31) << 11 | 0xFF);
      k64_rdp_fill_rect(x, y, x + blk, y + blk);
    }
}
// Canvas drawn by the CPU/RSP at 80x60, upscaled x4 by 1-cycle texture rectangles in strips
// that fit TMEM (bilinear). COPY cannot scale horizontally, so this is the cheapest upscale.
static void t_canvas_upscale(void) {
  k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_SAMPLE_BILINEAR);
  k64_rdp_set_combine(K64_CC_TEX);
  for (int y = 0; y < CH; y += 20) {
    k64_tex_t s = { .width = CW, .height = 20, .fmt = K64_FMT_RGBA, .siz = K64_SIZ_16,
                    .line_bytes = CW * 2, .tmem_line = CW * 2 / 8, .pixels = &canvas[y * CW] };
    k64_rdp_load_tex(0, 0, &s, K64_TEX_CLAMP, K64_TEX_CLAMP);
    k64_rdp_tex_rect(0, 0, y * 4, W, 80, 0, 0, 256, 256);
  }
}
// Many small triangles: the screen as 2 triangles per NxN cell, 1-cycle flat.
static void t_small_tris(void) {
  set_1c(K64_OM_CYCLE_1);
  k64_svtx_t a, b, c, d;
  for (int y = 0; y < H; y += blk)
    for (int x = 0; x < W; x += blk) {
      vtx(&a, x, y); vtx(&b, x + blk, y); vtx(&c, x + blk, y + blk); vtx(&d, x, y + blk);
      k64_rdp_tri(0, 0, &a, &b, &c);
      k64_rdp_tri(0, 0, &a, &c, &d);
    }
}

typedef struct { const char *name; void (*fn)(void); int blk; u32 px; } test_t;
static const test_t tests[] = {
  { "fill rect",        t_fill_rect,  0, W * H },
  { "fill 2 tris",      t_fill_tri,   0, W * H },
  { "1cyc rect",        t_1c_rect,    0, W * H },
  { "1cyc 2 tris",      t_1c_tri,     0, W * H },
  { "2cyc rect",        t_2c_rect,    0, W * H },
  { "copy 32x32 x80",   t_copy_tiles, 0, W * H },
  { "copy 1 wrap rect", t_copy_wrap,  0, W * H },
  { "fill canvas 16",   t_canvas_fill, 16, W * H },
  { "fill canvas 8",    t_canvas_fill, 8, W * H },
  { "fill canvas 4",    t_canvas_fill, 4, W * H },
  { "prim canvas 8",    t_canvas_prim, 8, W * H },
  { "prim canvas 4",    t_canvas_prim, 4, W * H },
  { "prim canvas 2",    t_canvas_prim, 2, W * H },
  { "1cyc tris 16",     t_small_tris, 16, W * H },
  { "1cyc tris 8",      t_small_tris, 8, W * H },
  { "upscale 80x60",    t_canvas_upscale, 0, W * H },
};
#define NT K64_ARRAY_LEN(tests)
static cost_t res[NT];

static void final_frame(void) {
  // FILL triangles (a star), COPY sprites, upscaled canvas strip
  k64_rdp_set_other_modes(K64_OM_FILL);
  k64_rdp_set_fill_color(k64_rgba16(10, 10, 30, 255) * 0x10001u);
  k64_rdp_fill_rect(0, 0, W, H);
  t_canvas_upscale();
  k64_rdp_set_other_modes(K64_OM_FILL);
  k64_svtx_t a, b, c;
  for (int i = 0; i < 5; i++) {
    float a0 = i * 2 * K64_PI / 5, a1 = a0 + 2 * K64_PI / 5 * 2;
    vtx(&a, 250, 160); vtx(&b, 250 + 50 * k64_cosf(a0), 160 + 50 * k64_sinf(a0));
    vtx(&c, 250 + 50 * k64_cosf(a1), 160 + 50 * k64_sinf(a1));
    k64_rdp_set_fill_color(k64_rgba16(255, (u8)(60 * i), 40, 255) * 0x10001u);
    k64_rdp_tri(0, 0, &a, &b, &c);
    k64_rdp_tri(0, 0, &a, &c, &b);       // either winding
  }
  k64_rdp_set_other_modes(K64_OM_COPY_ALPHA);
  k64_rdp_load_tex(0, 0, &tex, K64_TEX_WRAP, K64_TEX_WRAP);
  for (int i = 0; i < 4; i++) k64_rdp_sprite(0, 20 + i * 40, 120, 32, 32);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = W, .height = H, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  zbuf = k64_mem_alloc(K64_ZONE_Z, W * H * 2, 64);
  for (int y = 0; y < 32; y++)
    for (int x = 0; x < 32; x++) {
      int dx = x - 16, dy = y - 16;
      bool in = dx * dx + dy * dy < 15 * 15;
      tex_px[y * 32 + x] = in ? k64_rgba16((u8)(x * 8), (u8)(y * 8), 200, 255) : 0;
    }
  for (int y = 0; y < CH; y++)
    for (int x = 0; x < CW; x++) {
      int dx = x - 40, dy = y - 30;
      canvas[y * CW + x] = k64_rgba16((u8)(128 + dx * 3), (u8)(dx * dx + dy * dy), (u8)(y * 4), 255);
    }
  k64_dcache_writeback(tex_px, sizeof tex_px);
  k64_dcache_writeback(canvas, sizeof canvas);
  tex = (k64_tex_t){ .width = 32, .height = 32, .fmt = K64_FMT_RGBA, .siz = K64_SIZ_16,
                     .line_bytes = 64, .tmem_line = 8, .pixels = tex_px };

  fb = k64_display_acquire();
  target();
  base_cost = measure(t_empty);
  k64_printf("[09] empty (sync full) wall %u busy %u clk\n", base_cost.wall, base_cost.busy);
  k64_printf("[09] test               wall clk  busy clk  busy/px   wall us  cpu us\n");
  for (u32 i = 0; i < NT; i++) {
    blk = tests[i].blk;
    cost_t c = measure(tests[i].fn);
    c.wall = c.wall > base_cost.wall ? c.wall - base_cost.wall : 0;
    c.busy = c.busy > base_cost.busy ? c.busy - base_cost.busy : 0;
    res[i] = c;
    u32 cpp100 = (u32)((u64)c.busy * 100 / tests[i].px);
    u32 prims = tests[i].blk ? (W / tests[i].blk) * (H / tests[i].blk) : 0;
    k64_printf("[09] %-17s %9u %9u  %3u.%02u %9u %7u%s", tests[i].name, c.wall, c.busy, cpp100 / 100, cpp100 % 100,
               c.wall * 16 / 1000, c.cpu_us, prims ? "" : "\n");
    if (prims) k64_printf("  %u prims, wall %u busy %u clk/prim\n", prims, c.wall / prims, c.busy / prims);
  }
  for (u32 frame = 0;; frame++) {
    fb = k64_display_acquire();
    target();
    final_frame();
    for (u32 i = 0; i < NT; i++)
      k64_text_printf(4, 6 + i * 9, 0xFFFFFFFF, "%-16s%8u%8u%6u", tests[i].name, res[i].wall, res[i].busy,
                      res[i].cpu_us);
    k64_rdp_wait_idle();
    k64_display_show(fb);
    if (frame == 2) k64_printf("[09] done\n");
  }
}
