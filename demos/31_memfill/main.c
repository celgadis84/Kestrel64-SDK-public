// Demo 31 — k64_mem_fill: RDRAM fill through the MI repeat mode (k64/mem.h).
// Self-test: fills at every start/end alignment 0..15 around a sentinel buffer, through the
// cache and from RDRAM (uncached view), and checks the bytes outside are untouched. Then
// times a 320x240x16bpp clear against memset.
#include <k64/k64.h>


static u32 fails;
#define CHECK(c, ...)                               \
  do {                                              \
    if (!(c)) {                                     \
      fails++;                                      \
      k64_printf("[31_memfill] FAIL " __VA_ARGS__); \
      k64_printf("\n");                             \
    }                                               \
  } while (0)

static u8 buf[1024] K64_ALIGN(16);

static u8 want(u32 addr, u32 value) { return (u8)(value >> (24 - 8 * (addr & 3))); }

int main(void) {
  const u32 v = 0x12345678;
  for (u32 off = 0; off < 16; off++)
    for (u32 len = 0; len < 700; len += (len < 40 ? 1 : 37)) {
      __builtin_memset(buf, 0xEE, sizeof buf);
      k64_mem_fill(buf + 64 + off, v, len);
      u32 bad = 0;
      for (int pass = 0; pass < 2; pass++) {
        if (pass) k64_dcache_writeback_invalidate(buf, sizeof buf);   // now check RDRAM itself
        for (u32 i = 0; i < sizeof buf; i++) {
          u32 a = (u32)&buf[i];
          bool in = i >= 64 + off && i < 64 + off + len;
          bad += buf[i] != (in ? want(a, v) : 0xEE);
        }
      }
      CHECK(!bad, "off %u len %u: %u bytes wrong", off, len, bad);
    }

  u32 size = 320 * 240 * 2;
  u8 *fb = k64_mem_alloc(K64_ZONE_HEAP, size, 16);
  k64_dcache_writeback_invalidate(fb, size);
  u32 t0 = k64_count();
  __builtin_memset(fb, 0, size);
  k64_dcache_writeback(fb, size);           // what a CPU clear costs before the RDP/VI sees it
  u32 t1 = k64_count();
  k64_mem_fill(fb, 0x00010001, size);
  u32 t2 = k64_count();
  u32 bad = 0;
  for (u32 i = 0; i < size; i += 4) bad += *(u32 *)(fb + i) != 0x00010001;
  CHECK(!bad, "clear: %u words wrong", bad);
  k64_printf("[31_memfill] 150 KB clear: memset + writeback %u us, k64_mem_fill %u us\n", K64_US(t1 - t0),
             K64_US(t2 - t1));
  k64_printf("[31_memfill] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);
  for (;;) {}
}
