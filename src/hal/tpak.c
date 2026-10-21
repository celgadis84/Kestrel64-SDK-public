// kestrel64-sdk — Transfer Pak + Game Boy cartridge (see k64/tpak.h).
// Pak registers from n64brew "Transfer Pak"; cartridge header and MBC registers from Pan Docs.
#include <k64/tpak.h>
#include <k64/pak.h>

// Transfer Pak registers (n64brew): power, bank select, access control, GB window.
#define TP_POWER  0x8000
#define TP_BANK   0xA000
#define TP_ACCESS 0xB000
#define TP_WINDOW 0xC000
#define POWER_ON  0x84
#define POWER_OFF 0xFE

static u8 tp_bank[4] = { 0xFF, 0xFF, 0xFF, 0xFF };   // bank last written per port (0xFF = unknown)

// Writes one 32-byte block filled with v: the pak takes the register value from the block.
static bool fill(int port, u16 addr, u8 v) {
  u8 b[32];
  for (int i = 0; i < 32; i++) b[i] = v;
  return k64_pak_write32(port, addr, b);
}

u8 k64_tpak_status(int port) {
  u8 b[32];
  // Access register byte 0 holds the K64_TPAK_* status bits; 0 when the read fails.
  return k64_pak_read32(port, TP_ACCESS, b) ? b[0] : 0;
}

bool k64_tpak_start(int port) {
  if ((unsigned)port > 3) return false;
  u8 b[32];
  tp_bank[port] = 0xFF;
  if (!fill(port, TP_POWER, POWER_ON) || !k64_pak_read32(port, TP_POWER, b) || b[31] != POWER_ON) return false;
  if (!fill(port, TP_ACCESS, 1)) return false;
  // The first status read after power-up also reports the reset: read until it clears.
  u8 st = 0;
  for (int i = 0; i < 4; i++) {
    st = k64_tpak_status(port);
    if (!(st & K64_TPAK_RESET)) break;
  }
  return (st & (K64_TPAK_PRESENT | K64_TPAK_ACCESS)) == (K64_TPAK_PRESENT | K64_TPAK_ACCESS);
}

void k64_tpak_stop(int port) {
  if ((unsigned)port > 3) return;
  fill(port, TP_ACCESS, 0);
  fill(port, TP_POWER, POWER_OFF);
  tp_bank[port] = 0xFF;
}

// Window address for GB address `a`, switching the pak bank when needed.
static bool window(int port, u16 a, u16 *wa) {
  u8 bank = a >> 14;
  if (tp_bank[port] != bank) {                 // only switch when the cached bank differs
    if (!fill(port, TP_BANK, bank)) { tp_bank[port] = 0xFF; return false; }
    tp_bank[port] = bank;
  }
  *wa = TP_WINDOW + (a & 0x3FFF);
  return true;
}

bool k64_tpak_read(int port, u16 gbaddr, void *dst, u32 len) {
  if ((unsigned)port > 3 || gbaddr + len > 0x10000) return false;
  u8 *d = dst, b[32];
  u32 a = gbaddr;
  while (len) {
    u16 wa;
    u32 skew = a & 31, n = 32 - skew < len ? 32 - skew : len;
    if (!window(port, (u16)(a - skew), &wa) || !k64_pak_read32(port, wa, b)) return false;
    for (u32 i = 0; i < n; i++) d[i] = b[skew + i];
    d += n; a += n; len -= n;
  }
  return true;
}

bool k64_tpak_write(int port, u16 gbaddr, const void *src, u32 len) {
  if ((unsigned)port > 3 || (gbaddr | len) & 31 || gbaddr + len > 0x10000) return false;
  const u8 *s = src;
  for (u32 a = gbaddr; a < gbaddr + len; a += 32, s += 32) {
    u16 wa;
    if (!window(port, (u16)a, &wa) || !k64_pak_write32(port, wa, s)) return false;
  }
  return true;
}

bool k64_tpak_poke(int port, u16 gbaddr, u8 v) {
  u8 b[32];
  for (int i = 0; i < 32; i++) b[i] = v;
  return k64_tpak_write(port, gbaddr & ~31u, b, 32);
}

// ---- Cartridge -----------------------------------------------------------------------------

static void classify(k64_gbcart_t *c) {
  c->mbc = K64_GB_UNKNOWN;
  c->battery = c->rtc = false;
  switch (c->type) {
  case 0x00: case 0x08: c->mbc = K64_GB_NOMBC; break;
  case 0x09:            c->mbc = K64_GB_NOMBC; c->battery = true; break;
  case 0x01: case 0x02: c->mbc = K64_GB_MBC1; break;
  case 0x03:            c->mbc = K64_GB_MBC1; c->battery = true; break;
  case 0x05:            c->mbc = K64_GB_MBC2; break;
  case 0x06:            c->mbc = K64_GB_MBC2; c->battery = true; break;
  case 0x0F: case 0x10: c->mbc = K64_GB_MBC3; c->battery = c->rtc = true; break;
  case 0x11: case 0x12: c->mbc = K64_GB_MBC3; break;
  case 0x13:            c->mbc = K64_GB_MBC3; c->battery = true; break;
  case 0x19: case 0x1A: case 0x1C: case 0x1D: c->mbc = K64_GB_MBC5; break;
  case 0x1B: case 0x1E: c->mbc = K64_GB_MBC5; c->battery = true; break;
  }
  static const u8 ram_kib[6] = { 0, 0, 8, 32, 128, 64 };   // 0x149 code 1 is unused
  u8 r = c->header[0x49];
  c->ram_size = r < 6 ? (u32)ram_kib[r] << 10 : 0;
  if (c->mbc == K64_GB_MBC2) c->ram_size = 512;
  // Header 0x148: ROM size code, 32 KiB << n for n = 0..8.
  c->rom_size = c->header[0x48] <= 8 ? 0x8000u << c->header[0x48] : 0x8000u;
}

k64_gb_err_t k64_gbcart_open(k64_gbcart_t *c, int port) {
  *c = (k64_gbcart_t){ .port = port };
  if ((unsigned)port > 3) return K64_GB_ERR_IO;
  if (!k64_tpak_start(port)) {
    // Status set with PRESENT clear: the pak is there but has no cartridge.
    u8 st = k64_tpak_status(port);
    return st && !(st & K64_TPAK_PRESENT) ? K64_GB_ERR_NOCART : K64_GB_ERR_IO;
  }
  if (!k64_tpak_read(port, 0x100, c->header, sizeof c->header)) return K64_GB_ERR_IO;
  // Header checksum over 0x134-0x14C (header offsets 0x34-0x4C), stored at 0x14D.
  u8 x = 0;
  for (int i = 0x34; i <= 0x4C; i++) x = (u8)(x - c->header[i] - 1);
  if (x != c->header[0x4D]) return K64_GB_ERR_HEADER;
  c->cgb = c->header[0x43];
  int n = c->cgb & 0x80 ? 15 : 16;                  // CGB titles give up their last byte
  for (int i = 0; i < n; i++) c->title[i] = (char)c->header[0x34 + i];
  c->title[n] = 0;
  while (n && (c->title[n - 1] == 0 || c->title[n - 1] == ' ')) c->title[--n] = 0;
  c->type = c->header[0x47];                   // cartridge type, see classify()
  classify(c);
  return K64_GB_OK;
}

// Maps ROM bank `bank`; *base = the GB address it shows at.
static bool map_rom(k64_gbcart_t *c, u32 bank, u16 *base) {
  int p = c->port;
  *base = 0x4000;
  switch (c->mbc) {
  case K64_GB_NOMBC:
    *base = (u16)(bank * 0x4000);
    return bank < 2;
  case K64_GB_MBC1:
    // 5-bit register at 0x2000 (0 acts as 1) + 2-bit register at 0x4000 = bank bits 5-6.
    // Banks 0x00/0x20/0x40/0x60 show only at 0x0000, in mode 1 (mode 0 forces bank 0 there).
    if (!k64_tpak_poke(p, 0x4000, (u8)(bank >> 5 & 3))) return false;
    if (bank & 0x1F) return k64_tpak_poke(p, 0x6000, 0) && k64_tpak_poke(p, 0x2000, (u8)(bank & 0x1F));
    *base = 0x0000;
    return k64_tpak_poke(p, 0x6000, 1);
  case K64_GB_MBC2:
    // Registers at 0x0000-0x3FFF; address bit 8 set = ROM bank (4 bits, 0 acts as 1).
    if (!bank) { *base = 0; return true; }
    return k64_tpak_poke(p, 0x2100, (u8)(bank & 0x0F));
  case K64_GB_MBC3:
    if (!bank) { *base = 0; return true; }
    return k64_tpak_poke(p, 0x2000, (u8)(bank & 0x7F));
  case K64_GB_MBC5:   // 9-bit bank at 0x2000 (low 8) + 0x3000 (bit 8); bank 0 is selectable
    return k64_tpak_poke(p, 0x2000, (u8)bank) && k64_tpak_poke(p, 0x3000, (u8)(bank >> 8 & 1));
  default:
    return false;
  }
}

bool k64_gbcart_read_rom(k64_gbcart_t *c, u32 offset, void *dst, u32 len) {
  if (offset + len > c->rom_size || offset + len < offset) return false;
  u8 *d = dst;
  while (len) {
    // One 16 KiB ROM bank per step: map the bank, then read the part of it that is needed.
    u32 bank = offset >> 14, in = offset & 0x3FFF, n = 0x4000 - in < len ? 0x4000 - in : len;
    u16 base;
    if (!map_rom(c, bank, &base) || !k64_tpak_read(c->port, (u16)(base + in), d, n)) return false;
    d += n; offset += n; len -= n;
  }
  return true;
}

// Enables RAM and maps RAM bank `bank` (8 KiB at 0xA000).
static bool map_ram(k64_gbcart_t *c, u32 bank) {
  int p = c->port;
  if (!k64_tpak_poke(p, 0x0000, 0x0A)) return false;
  switch (c->mbc) {
  case K64_GB_MBC1: return k64_tpak_poke(p, 0x6000, 1) && k64_tpak_poke(p, 0x4000, (u8)(bank & 3));
  case K64_GB_MBC3: return k64_tpak_poke(p, 0x4000, (u8)(bank & 3));
  case K64_GB_MBC5: return k64_tpak_poke(p, 0x4000, (u8)(bank & 0x0F));
  default:          return bank == 0;
  }
}

// Save RAM in 8 KiB banks: each bank is mapped, then the part inside it is read or written.
static bool ram_io(k64_gbcart_t *c, u32 offset, u8 *buf, u32 len, bool wr) {
  if (offset + len > c->ram_size || offset + len < offset) return false;
  bool ok = true;
  while (ok && len) {
    u32 bank = offset >> 13, in = offset & 0x1FFF, n = 0x2000 - in < len ? 0x2000 - in : len;
    ok = map_ram(c, bank) && (wr ? k64_tpak_write(c->port, (u16)(0xA000 + in), buf, n)
                                 : k64_tpak_read(c->port, (u16)(0xA000 + in), buf, n));
    buf += n; offset += n; len -= n;
  }
  k64_tpak_poke(c->port, 0x0000, 0x00);           // RAM off again: guards the save
  return ok;
}

bool k64_gbcart_read_ram(k64_gbcart_t *c, u32 offset, void *dst, u32 len) {
  return ram_io(c, offset, dst, len, false);
}

bool k64_gbcart_write_ram(k64_gbcart_t *c, u32 offset, const void *src, u32 len) {
  if (offset + len > c->ram_size || offset + len < offset) return false;
  const u8 *s = src;
  u8 blk[32];
  while (len) {
    u32 skew = offset & 31, n = 32 - skew < len ? 32 - skew : len;
    // Partial 32-byte block: read it first so the bytes outside the range survive.
    if (n < 32 && !ram_io(c, offset - skew, blk, 32, false)) return false;
    for (u32 i = 0; i < n; i++) blk[skew + i] = s[i];
    if (!ram_io(c, offset - skew, blk, 32, true)) return false;
    s += n; offset += n; len -= n;
  }
  return true;
}

bool k64_gbcart_rtc_read(k64_gbcart_t *c, u8 out[5]) {
  int p = c->port;
  if (!c->rtc) return false;
  // Latch (write 0 then 1 to 0x6000), then RAM bank 0x08-0x0C maps one clock register.
  bool ok = k64_tpak_poke(p, 0x0000, 0x0A) && k64_tpak_poke(p, 0x6000, 0) && k64_tpak_poke(p, 0x6000, 1);
  for (int i = 0; ok && i < 5; i++)
    ok = k64_tpak_poke(p, 0x4000, (u8)(8 + i)) && k64_tpak_read(p, 0xA000, &out[i], 1);
  k64_tpak_poke(p, 0x0000, 0x00);
  return ok;
}

bool k64_gbcart_rtc_write(k64_gbcart_t *c, const u8 in[5]) {
  int p = c->port;
  if (!c->rtc) return false;
  bool ok = k64_tpak_poke(p, 0x0000, 0x0A);
  for (int i = 0; ok && i < 5; i++)
    ok = k64_tpak_poke(p, 0x4000, (u8)(8 + i)) && k64_tpak_poke(p, 0xA000, in[i]);
  k64_tpak_poke(p, 0x0000, 0x00);
  return ok;
}
