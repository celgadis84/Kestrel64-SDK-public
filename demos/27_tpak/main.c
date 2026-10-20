// Demo 27 — Transfer Pak: Game Boy cartridges on all four ports (k64/tpak.h).
// Expects the synthetic carts from mkgbrom.py (port 1 MBC1, 2 MBC3+RTC, 3 MBC5, 4 MBC2).
// Self-test: header decode, ROM reads across bank edges (incl. the MBC1 0x20 bank
// banks), RAM write/read in every RAM bank, MBC2 4-bit RAM, MBC3 clock set/latch/read.
#include <k64/k64.h>

static u32 fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; k64_printf("[27_tpak] FAIL " __VA_ARGS__); k64_printf("\n"); } } while (0)

static u8 rom_byte(u32 i) { return (u8)(i * 31 ^ (i >> 14) * 0x5B ^ (i >> 8)); }

typedef struct { const char *title; k64_gb_mbc_t mbc; u32 rom, ram; bool rtc; u8 cgb; } expect_t;
static const expect_t want[4] = {
  { "K64 MBC1", K64_GB_MBC1, 1u << 20,  8u << 10, false, 0x00 },
  { "K64 MBC3", K64_GB_MBC3, 128u << 10, 32u << 10, true, 0x00 },
  { "K64 MBC5", K64_GB_MBC5, 512u << 10, 128u << 10, false, 0x80 },
  { "K64 MBC2", K64_GB_MBC2, 256u << 10, 512, false, 0x00 },
};

static bool same_str(const char *a, const char *b) {
  while (*a && *a == *b) a++, b++;
  return *a == *b;
}

// 40 bytes straddling the start and the end of `bank`, compared with the generator formula.
static void check_bank(k64_gbcart_t *c, u32 bank) {
  u8 buf[48];
  u32 at[2] = { bank * 0x4000 + (bank ? 0 : 0x150), bank * 0x4000 + 0x4000 - 40 };
  for (int k = 0; k < 2; k++) {
    bool ok = k64_gbcart_read_rom(c, at[k] + 3, buf + 1, 37);
    for (u32 i = 0; ok && i < 37; i++) ok = buf[1 + i] == rom_byte(at[k] + 3 + i);
    CHECK(ok, "port %d rom bank %u at %05x", c->port + 1, bank, at[k]);
  }
}

static void test_port(int port, const expect_t *w) {
  k64_gbcart_t c;
  k64_gb_err_t e = k64_gbcart_open(&c, port);
  if (e != K64_GB_OK) { CHECK(0, "port %d open err %d status %02x", port + 1, e, k64_tpak_status(port)); return; }
  k64_printf("[27_tpak] port %d '%s' type %02x mbc %d rom %uK ram %u%s\n", port + 1, c.title, c.type,
             c.mbc, c.rom_size >> 10, c.ram_size, c.rtc ? " rtc" : "");
  CHECK(same_str(c.title, w->title) && c.mbc == w->mbc && c.rom_size == w->rom && c.ram_size == w->ram &&
        c.rtc == w->rtc && c.cgb == w->cgb && c.battery, "port %d header", port + 1);

  u32 banks = c.rom_size >> 14;
  check_bank(&c, 0);
  check_bank(&c, 1);
  check_bank(&c, banks - 1);
  check_bank(&c, banks / 2 + 1);
  if (c.mbc == K64_GB_MBC1) { check_bank(&c, 0x20); check_bank(&c, 0x21); check_bank(&c, 0x3F); }
  check_bank(&c, 0);                                     // back to 0 after the mode-1 banks

  // RAM: odd-offset write straddling every bank edge, then read back.
  u8 pat[64], back[64];
  u32 rbanks = c.ram_size > 0x2000 ? c.ram_size >> 13 : 1, salt = k64_count();
  for (u32 b = 0; b < rbanks; b++) {
    u32 off = c.ram_size > 0x2000 ? b * 0x2000 + 0x2000 - 21 : 0x77;
    u32 n = off + 45 <= c.ram_size ? 45 : c.ram_size - off;
    for (u32 i = 0; i < n; i++) pat[i] = (u8)(salt + b * 17 + i * 3);
    if (c.mbc == K64_GB_MBC2) for (u32 i = 0; i < n; i++) pat[i] &= 0x0F;
    bool ok = k64_gbcart_write_ram(&c, off, pat, n) && k64_gbcart_read_ram(&c, off, back, n);
    for (u32 i = 0; ok && i < n; i++) ok = (c.mbc == K64_GB_MBC2 ? back[i] & 0x0F : back[i]) == pat[i];
    CHECK(ok, "port %d ram bank %u", port + 1, b);
  }
  CHECK(!k64_gbcart_read_ram(&c, c.ram_size - 4, back, 8), "port %d ram bounds", port + 1);
  CHECK(!k64_gbcart_read_rom(&c, c.rom_size - 4, back, 8), "port %d rom bounds", port + 1);

  if (c.rtc) {
    u8 set[5] = { 42, 13, 7, 0x2C, 0x41 }, got[5];       // halted, day 0x12C
    bool ok = k64_gbcart_rtc_write(&c, set) && k64_gbcart_rtc_read(&c, got);
    for (int i = 0; ok && i < 5; i++) ok = got[i] == set[i];
    CHECK(ok, "rtc halted %u:%u:%u day %u", got[2], got[1], got[0], got[3] | (got[4] & 1) << 8);
    CHECK(k64_gbcart_rtc_read(&c, got) && got[0] == 42, "rtc stays halted");
  }
  k64_tpak_stop(port);
  CHECK(!(k64_tpak_status(port) & K64_TPAK_ACCESS), "port %d stop", port + 1);
}

int main(void) {
  for (int p = 0; p < 4; p++) {
    k64_pak_type_t t = k64_pak_probe(p);
    CHECK(t == K64_PAK_TRANSFER, "port %d probe %d", p + 1, t);
    u32 t0 = k64_count();
    test_port(p, &want[p]);
    k64_printf("[27_tpak] port %d done in %u ms\n", p + 1, K64_US(k64_count() - t0) / 1000);
  }
  k64_printf("[27_tpak] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);
  for (;;) {}
}
