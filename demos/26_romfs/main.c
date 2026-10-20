// Demo 26 — ROM file system (k64/romfs.h): the data/ tree is packed by tools/k64fs.py
// (*.txt compressed). Self-test: lookups, listing, reads at every RAM/ROM alignment and
// odd lengths, seeks, packed loads.
#include <k64/k64.h>

static u32 fails;
#define CHECK(c, ...)                             \
  do {                                            \
    if (!(c)) {                                   \
      fails++;                                    \
      k64_printf("[26_romfs] FAIL " __VA_ARGS__); \
      k64_printf("\n");                           \
    }                                             \
  } while (0)

static u8 odd_byte(u32 i) { return (u8)(i * 7 + 3); }

int main(void) {
  k64_printf("[26_romfs] %u files\n", k64_romfs_count());
  for (u32 i = 0; i < k64_romfs_count(); i++) k64_printf("[26_romfs]   %s\n", k64_romfs_name(i));
  CHECK(k64_romfs_count() == 6, "count %u", k64_romfs_count());

  k64_file_t f;
  CHECK(!k64_romfs_open(&f, "missing.txt") && !k64_romfs_open(&f, "levels") && !k64_romfs_open(&f, "README.TXT"),
        "missing");
  // Tiny *.txt files do not shrink, so k64fs stores them raw.
  CHECK(k64_romfs_open(&f, "/levels/level2.txt") && !f.packed && f.size == 17, "leading slash, raw small file");

  // Unpacked file, every dst alignment x file offset x length mix.
  CHECK(k64_romfs_open(&f, "bin/odd.bin") && !f.packed && f.size == 1237, "odd.bin size %u", f.size);
  static u8 buf[1300] K64_ALIGN(16);
  u32 bad = 0;
  for (u32 da = 0; da < 8; da++)
    for (u32 off = 0; off < 4; off++)
      for (u32 len = 1; len < 1200; len = len * 3 + 1) {
        __builtin_memset(buf, 0xEE, sizeof buf);
        k64_romfs_seek(&f, off);
        u32 n = k64_romfs_read(&f, buf + da, len);
        for (u32 i = 0; i < n; i++) if (buf[da + i] != odd_byte(off + i)) { bad++; break; }
        if (n != len || buf[da + len] != 0xEE || (da && buf[da - 1] != 0xEE) || f.pos != off + len) bad++;
      }
  CHECK(!bad, "aligned/unaligned reads: %u bad", bad);
  k64_romfs_seek(&f, 1230);
  CHECK(k64_romfs_read(&f, buf, 100) == 7 && buf[6] == odd_byte(1236), "read clamps at eof");
  CHECK(k64_romfs_seek(&f, 5000) == 1237 && k64_romfs_read(&f, buf, 4) == 0, "seek clamps");

  // Sequential chunked reads equal the whole-file load.
  u32 size;
  u8 *whole = k64_romfs_load("bin/pattern.dat", &size);
  CHECK(whole && size == 6000, "pattern load %u", size);
  k64_romfs_open(&f, "bin/pattern.dat");
  u32 ok = 1, at = 0;
  for (u32 n; (n = k64_romfs_read(&f, buf + 3, 333)) != 0; at += n) ok &= !__builtin_memcmp(buf + 3, whole + at, n);
  CHECK(ok && at == 6000, "chunked reads");
  for (u32 i = 0; i < size; i++) if (whole[i] != (u8)((i * 13) ^ (i >> 8))) { CHECK(0, "pattern byte %u", i); break; }

  // Packed text: decoded by load, stored size smaller.
  char *txt = k64_romfs_load("big.txt", &size);
  k64_romfs_open(&f, "big.txt");
  CHECK(txt && size == 400 * 38 && f.packed && f.size < size, "big.txt %u stored %u", size, f.size);
  CHECK(txt && !__builtin_memcmp(txt, "line 0000 of", 12) && !__builtin_memcmp(txt + 399 * 38, "line 0399 of", 12),
        "big.txt content");
  char *r = k64_romfs_load("readme.txt", &size);
  CHECK(r && size == 30 && !__builtin_memcmp(r, "kestrel64-sdk ROM file system\n", 30), "readme");

  u32 t0 = k64_count();
  for (int i = 0; i < 100; i++) k64_romfs_open(&f, "levels/level1.txt");
  k64_printf("[26_romfs] open %u ns\n", K64_US(k64_count() - t0) * 10);
  k64_printf("[26_romfs] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);
  for (;;) {}
}
