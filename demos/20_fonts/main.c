// Demo 20 — proportional fonts: antialiased + kerned Lato at 16 and 12 px, the 1-bit m5x7
// pixel font, word-wrapped boxes in three alignments, UTF-8 (Latin-1), color codes, shadow.
// Self-test on the layout rules, then the RDP cost of a paragraph.
#include <k64/k64.h>

extern const u8 lato16[], lato12[], pixel[];
static k64_font_t f16, f12, fpx;

static u32 fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; k64_printf("[20_fonts] FAIL " __VA_ARGS__); k64_printf("\n"); } } while (0)

static const char *para =
  "El veloz murci\xC3\xA9lago hind\xC3\xBA com\xC3\xAD""a feliz cardillo y kiwi. "
  "La cig\xC3\xBC""e\xC3\xB1""a tocaba el saxof\xC3\xB3n detr\xC3\xA1s del palenque de paja.";

static void self_test(void) {
  CHECK(k64_font_from_blob(&f16, lato16) && k64_font_from_blob(&f12, lato12) && k64_font_from_blob(&fpx, pixel), "blob");
  CHECK(!f16.mono && fpx.mono, "mono flags %d %d", f16.mono, fpx.mono);
  CHECK(f16.ascii && f12.ascii && fpx.ascii, "ascii fast path");
  CHECK(f12.nsheets == 1, "lato12 sheets %u", f12.nsheets);
  u32 n_tilde = k64_font_glyph(&f16, 0xF1);
  CHECK(f16.codepoints[n_tilde] == 0xF1, "U+00F1 lookup");
  CHECK(k64_font_glyph(&f16, 0x4E00) == f16.fallback && f16.codepoints[f16.fallback] == '?', "fallback");
  CHECK(k64_font_glyph(&f16, 'A') == 'A' - 32, "ascii index");
  // UTF-8: two-byte sequence = one glyph, a truncated one = the fallback glyph.
  CHECK(k64_font_width(&f16, "\xC3\xB1") == (f16.glyphs[n_tilde].advance + 2) / 4, "utf8 width");
  CHECK(k64_font_width(&f16, "\xC3") == k64_font_width(&f16, "?"), "utf8 truncated");
  // Kerning: Lato pulls "AV" and "To" together, the pixel font has no pairs.
  int a = k64_font_width(&f16, "A"), v = k64_font_width(&f16, "V"), av = k64_font_width(&f16, "AV");
  CHECK(av < a + v, "kern AV %d vs %d+%d", av, a, v);
  CHECK(k64_font_width(&f16, "To") < k64_font_width(&f16, "T") + k64_font_width(&f16, "o"), "kern To");
  CHECK(fpx.nkern == 0 && k64_font_width(&fpx, "AV") == k64_font_width(&fpx, "A") + k64_font_width(&fpx, "V"), "pixel no kern");
  CHECK(k64_font_width(&f16, "") == 0 && k64_font_width(&f16, "ab\ncd") == k64_font_width(&f16, "ab"), "first line width");
  CHECK(k64_font_width(&f16, "\x02" "ab\x01") == k64_font_width(&f16, "ab"), "color codes take no space");
  // Boxes: one line when it fits, wrapping at spaces, a word wider than the box split.
  int lh = f16.line_height;
  CHECK(k64_font_box(&f16, 0, 0, 1000, 0, 0, "one line", false) == lh, "box one line");
  CHECK(k64_font_box(&f16, 0, 0, k64_font_width(&f16, "aaa bbb") + 1, 0, 0, "aaa bbb ccc", false) == 2 * lh, "box wrap");
  CHECK(k64_font_box(&f16, 0, 0, k64_font_width(&f16, "aaa") + 1, 0, 0, "aaaaaaaaa", false) == 3 * lh, "box split word");
  CHECK(k64_font_box(&f16, 0, 0, 1000, 0, 0, "a\nb\n\nc", false) == 4 * lh, "box newlines");
  CHECK(k64_font_box(&f16, 0, 0, 1, 0, 0, "abc", false) == 3 * lh, "box narrower than a glyph");
  k64_printf("[20_fonts] lato16: %u glyphs %u kern %u sheets line %u | lato12: %u glyphs %u kern | m5x7: %u glyphs, mono\n",
             f16.nglyphs, f16.nkern, f16.nsheets, f16.line_height, f12.nglyphs, f12.nkern, fpx.nglyphs);
}

static void scene(void) {
  k64_font_set_shadow(1, 1, 0x000000C0);
  k64_font_draw(&f16, 12, 8, 0xFFFFFFFF, "kestrel64-sdk  \x02" "20" "\x01  proportional fonts");
  k64_font_set_shadow(0, 0, 0);
  k64_font_printf(&f12, 12, 30, 0xC0C0C0FF, "self-test %s  \xC2\xB7  AV To Wa Ty  \xC2\xB7  \xC2\xBF" "Ma\xC3\xB1" "ana?", fails ? "FAIL" : "PASS");
  k64_font_box(&f16, 12, 50, 140, K64_ALIGN_LEFT, 0xFFE0A0FF, para, true);
  k64_font_box(&f12, 168, 50, 140, K64_ALIGN_RIGHT, 0xA0E0FFFF, para, true);
  k64_font_box(&f12, 12, 170, 296, K64_ALIGN_CENTER, 0xFFFFFFFF,
               "Centered text, \x02" "colored words\x01 and \x03" "codes\x01, wrapped to a 296 px box.", true);
  k64_font_draw(&fpx, 12, 210, 0x80FF80FF, "m5x7 pixel font: 1-bit, alpha compare, no fb read");
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_font_set_palette(1, 0x60FF60FF);
  k64_font_set_palette(2, 0xFF8040FF);
  self_test();
  k64_printf("[20_fonts] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);

  for (u32 frame = 0;; frame++) {
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(24, 28, 48, 255) * 0x10001u, false);
    if (frame == 4) {
      // Cost of the whole text scene alone: RDP clocks and CPU time to emit it.
      k64_rdp_wait_idle();
      u32 st0 = k64_rdps.stat_prims;
      u64 t0 = k64_time();
      u32 c0 = k64_rdp_clock();
      scene();
      u32 us_cpu = (u32)k64_ticks_to_us(k64_time() - t0);
      k64_rdp_wait_idle();
      u32 gclk = (k64_rdp_clock() - c0) & 0xFFFFFF;
      k64_printf("[20_fonts] scene: %u glyph rects, cpu %u us, rdp %u gclk (%u us)\n",
                 k64_rdps.stat_prims - st0, us_cpu, gclk, gclk / 62);
    } else {
      scene();
    }
    k64_rdp_wait_idle();
    k64_display_show(fb);
  }
}
