// kestrel64-sdk — Controller Pak / Rumble Pak over the joybus.
//
// Accessory read 0x02 {addr hi, addr lo} -> 32 data bytes + data CRC; write 0x03 {addr hi,
// addr lo, 32 bytes} -> data CRC. The low 5 bits of the address carry a checksum of the
// upper 11. With an empty slot the controller answers with the CRC inverted.
#include <k64/pak.h>
#include <k64/joybus.h>

#define CMD_READ  0x02
#define CMD_WRITE 0x03
#define ADDR_PROBE  0x8000      // accessory identification register
#define ADDR_MOTOR  0xC000
#define PROBE_RUMBLE    0x80
#define PROBE_TPAK_ON   0x84
#define PROBE_TPAK_OFF  0xFE

static u16 addr_crc(u16 addr) {
  static const u8 x[16] = { 0, 0, 0, 0, 0, 0x15, 0x1F, 0x0B, 0x16, 0x19, 0x07, 0x0E, 0x1C, 0x0D, 0x1A, 0x01 };
  addr &= 0xFFE0;
  u16 c = 0;
  for (int i = 15; i >= 5; i--) if (addr >> i & 1) c ^= x[i];
  return addr | (c & 0x1F);
}

static u8 data_crc(const u8 *d) {
  u32 crc = 0;
  for (int i = 0; i <= 32; i++) {
    for (int b = 7; b >= 0; b--) {
      u32 bit = crc & 0x80 ? 0x85 : 0;
      crc <<= 1;
      if (i < 32 && d[i] >> b & 1) crc |= 1;
      crc = (crc ^ bit) & 0xFF;
    }
  }
  return (u8)crc;
}

bool k64_pak_read32(int port, u16 addr, u8 out[32]) {
  u16 a = addr_crc(addr);
  u8 tx[3] = { CMD_READ, (u8)(a >> 8), (u8)a }, rx[33];
  if (!k64_joybus_cmd(port, tx, 3, rx, 33)) return false;
  for (int i = 0; i < 32; i++) out[i] = rx[i];
  return rx[32] == data_crc(rx);
}

bool k64_pak_write32(int port, u16 addr, const u8 in[32]) {
  u16 a = addr_crc(addr);
  u8 tx[35] = { CMD_WRITE, (u8)(a >> 8), (u8)a }, rx[1];
  for (int i = 0; i < 32; i++) tx[3 + i] = in[i];
  if (!k64_joybus_cmd(port, tx, 35, rx, 1)) return false;
  return rx[0] == data_crc(in);
}

static bool fill_write(int port, u16 addr, u8 v) {
  u8 b[32];
  for (int i = 0; i < 32; i++) b[i] = v;
  return k64_pak_write32(port, addr, b);
}

static bool reads_back(int port, u16 addr, u8 v) {
  u8 b[32];
  return k64_pak_read32(port, addr, b) && b[31] == v;
}

k64_pak_type_t k64_pak_probe(int port) {
  // Controller status: bit 0 of the third byte = something in the slot.
  u8 tx = 0x00, st[3];
  if (!k64_joybus_cmd(port, &tx, 1, st, 3) || !(st[2] & 1)) return K64_PAK_NONE;

  // Controller Pak: plain RAM at 0x0000. Write a pattern, read it back, restore.
  u8 save[32], pat[32], back[32];
  if (k64_pak_read32(port, 0, save)) {
    for (int i = 0; i < 32; i++) pat[i] = (u8)(save[i] ^ 0xA5 ^ i);
    bool ram = k64_pak_write32(port, 0, pat) && k64_pak_read32(port, 0, back);
    for (int i = 0; ram && i < 32; i++) ram = back[i] == pat[i];
    if (ram) { k64_pak_write32(port, 0, save); return K64_PAK_CONTROLLER; }
  }
  // Rumble Pak answers 0x80 at the probe register once 0x80 is written there.
  if (fill_write(port, ADDR_PROBE, PROBE_RUMBLE) && reads_back(port, ADDR_PROBE, PROBE_RUMBLE))
    return K64_PAK_RUMBLE;
  // Transfer Pak: power it on, check, power it off again.
  if (fill_write(port, ADDR_PROBE, PROBE_TPAK_ON) && reads_back(port, ADDR_PROBE, PROBE_TPAK_ON)) {
    fill_write(port, ADDR_PROBE, PROBE_TPAK_OFF);
    return K64_PAK_TRANSFER;
  }
  return K64_PAK_UNKNOWN;
}

bool k64_rumble(int port, bool on) { return fill_write(port, ADDR_MOTOR, on ? 1 : 0); }

bool k64_cpak_read(int port, u32 offset, void *dst, u32 len) {
  if (offset > K64_CPAK_SIZE || len > K64_CPAK_SIZE - offset) return false;
  u8 *d = dst, blk[32];
  while (len) {
    u32 base = offset & ~31u, o = offset - base, n = 32 - o < len ? 32 - o : len;
    if (!k64_pak_read32(port, (u16)base, blk)) return false;
    for (u32 i = 0; i < n; i++) d[i] = blk[o + i];
    d += n; offset += n; len -= n;
  }
  return true;
}

bool k64_cpak_write(int port, u32 offset, const void *src, u32 len) {
  if (offset > K64_CPAK_SIZE || len > K64_CPAK_SIZE - offset) return false;
  const u8 *s = src;
  u8 blk[32];
  while (len) {
    u32 base = offset & ~31u, o = offset - base, n = 32 - o < len ? 32 - o : len;
    if (n == 32) {                             // whole block: no read needed
      if (!k64_pak_write32(port, (u16)base, s)) return false;
      s += n; offset += n; len -= n;
      continue;
    }
    if (!k64_pak_read32(port, (u16)base, blk)) return false;
    bool same = true;
    for (u32 i = 0; i < n; i++) { same = same && blk[o + i] == s[i]; blk[o + i] = s[i]; }
    if (!same && !k64_pak_write32(port, (u16)base, blk)) return false;
    s += n; offset += n; len -= n;
  }
  return true;
}
