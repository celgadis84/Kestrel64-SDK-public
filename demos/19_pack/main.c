// Demo 19 — compressed assets: ROM-resident .k64z decoded in place while the PI DMA streams
// it (k64_unpack_rom), checked against a resident copy, timed against a plain PI read of
// the same raw size. Also the RAM-to-RAM decoder (k64_unpack).
#include <k64/k64.h>


extern u8 trex_ref[], trex_ref_end[];
extern u8 trex_z[], shrine_z[], bricks_z[], patterns_z[];

// Same bytes as assets/gen_patterns.py.
static u32 gen_patterns(u8 *out) {
  u32 x = 1, n = 0;
#define R() (x = x * 1103515245u + 12345u)
  for (u32 seg = 0; seg < 300; seg++) {
    u32 v = R();
    if (seg % 3 == 2) {
      for (u32 k = 1 + (v >> 12) % 40; k; k--) out[n++] = R() >> 24;
    } else {
      u32 p = 1 + (v >> 8) % 9, len = 1 + (v >> 12) % 200;
      u8 pat[9];
      for (u32 i = 0; i < p; i++) pat[i] = R() >> 24;
      for (u32 i = 0; i < len; i++) out[n++] = pat[i % p];
    }
  }
#undef R
  return n;
}

static u32 fails;
#define CHECK(c, ...)                            \
  do {                                           \
    if (!(c)) {                                  \
      fails++;                                   \
      k64_printf("[19_pack] FAIL " __VA_ARGS__); \
      k64_printf("\n");                          \
    }                                            \
  } while (0)

typedef struct { const char *name; u32 raw, packed, us_pi, us_pz, us_unpack, us_ram; } row_t;
static row_t rows[4];

static u32 us_since(u64 t0) { return (u32)k64_ticks_to_us(k64_time() - t0); }

static void measure(row_t *r, const char *name, u8 *rom, const u8 *ref) {
  u32 a = (u32)rom;
  k64_pack_hdr_t h = k64_pack_info(a);
  u8 *buf = k64_mem_alloc(K64_ZONE_HEAP, h.buf_size, 16);
  r->name = name;
  r->raw = h.raw_size;
  r->packed = h.packed_size + 16;
  // Baseline: the raw size straight from ROM (what an uncompressed asset costs).
  u64 t0 = k64_time();
  k64_pi_read(buf, a, (h.raw_size + 1) & ~1u);
  r->us_pi = us_since(t0);
  __builtin_memset(buf, 0x5A, h.buf_size);
  t0 = k64_time();
  u32 n = k64_unpack_rom(buf, a);
  r->us_unpack = us_since(t0);
  CHECK(n == h.raw_size, "%s size %u", name, n);
  if (ref) CHECK(__builtin_memcmp(buf, ref, n) == 0, "%s differs from the resident copy", name);

  // RAM-to-RAM: fetch the .k64z itself, decode it again, compare with the in-place result.
  u32 zlen = (r->packed + 7) & ~7u;
  u8 *z = k64_mem_alloc(K64_ZONE_HEAP, zlen, 16);
  u8 *out = k64_mem_alloc(K64_ZONE_HEAP, n + 8, 16);
  t0 = k64_time();
  k64_pi_read(z, a, zlen);
  r->us_pz = us_since(t0);
  out[n + 1] = 0xA5;
  t0 = k64_time();
  CHECK(k64_unpack(out + 1, z) == n, "%s ram size", name);           // odd destination
  r->us_ram = us_since(t0);
  CHECK(__builtin_memcmp(out + 1, buf, n) == 0 && out[n + 1] == 0xA5, "%s ram decode differs", name);
  k64_printf("[19_pack] %-7s %6u -> %6u (%2u%%)  pi raw %5u us  pi k64z %5u us  unpack %5u us (ram %5u us)\n", name,
             r->raw, r->packed, r->packed * 100 / r->raw, r->us_pi, r->us_pz, r->us_unpack, r->us_ram);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  measure(&rows[0], "trex", trex_z, trex_ref);
  CHECK(rows[0].raw == (u32)(trex_ref_end - trex_ref), "trex raw %u vs %u", rows[0].raw,
        (u32)(trex_ref_end - trex_ref));
  measure(&rows[1], "shrine", shrine_z, NULL);
  measure(&rows[2], "bricks", bricks_z, NULL);
  static u8 pat_ref[32768];
  u32 pn = gen_patterns(pat_ref);
  measure(&rows[3], "pattern", patterns_z, pat_ref);
  CHECK(rows[3].raw == pn, "patterns raw %u vs %u", rows[3].raw, pn);
  k64_printf("[19_pack] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);

  for (;;) {
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, false);
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  19 packed assets");
    k64_text_printf(8, 24, fails ? 0xFF4040FF : 0x40FF40FF, "self-test %s", fails ? "FAIL" : "PASS");
    k64_text_printf(8, 48, 0xFFFF80FF, "asset     raw    k64z   pi raw  unpack");
    for (int i = 0; i < 4; i++) {
      row_t *r = &rows[i];
      k64_text_printf(8, 64 + i * 12, 0xC0C0C0FF, "%-7s %6u %6u %5u us %5u us", r->name, r->raw, r->packed, r->us_pi,
                      r->us_unpack);
    }
    k64_text_printf(8, 124, 0x8080A0FF, "LZ4 decoded in place behind the PI DMA:");
    k64_text_printf(8, 136, 0x8080A0FF, "no scratch buffer, load ~ packed size");
    k64_rdp_wait_idle();
    k64_display_show(fb);
  }
}
