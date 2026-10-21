// kestrel64-sdk — raw joybus transactions.
#include <k64/joybus.h>
#include <k64/cpu.h>
#include <k64/debug.h>
#include <k64/pad.h>

// 64-byte PIF images for the SI DMA, 16-byte aligned for the cache calls below.
static u64 jb_in[8] __attribute__((aligned(16)));
static u64 jb_out[8] __attribute__((aligned(16)));

// Spins until the SI DMA and the SI I/O are both idle.
static void si_wait(void) { while (SI_STATUS & (SI_ST_DMA_BUSY | SI_ST_IO_BUSY)) {} }

void k64_joybus_exec(const u8 in[64], u8 out[64]) {
  k64_pad_poll_end();                          // collect a pending pad poll first
  u8 *b = (u8 *)jb_in;
  for (int i = 0; i < 64; i++) b[i] = in[i];
  si_wait();
  k64_dcache_writeback(jb_in, 64);             // the SI DMA reads RDRAM: flush the input image
  SI_DRAM_ADDR = K64_PHYS(jb_in);
  SI_PIF_AD_WR64B = PIF_RAM_PHYS;
  si_wait();
  k64_dcache_invalidate(jb_out, 64);           // the reply DMA writes RDRAM behind the cache
  SI_DRAM_ADDR = K64_PHYS(jb_out);
  SI_PIF_AD_RD64B = PIF_RAM_PHYS;
  si_wait();
  SI_STATUS = 0;                               // ack the SI interrupt (masked anyway)
  const u8 *r = (const u8 *)jb_out;
  for (int i = 0; i < 64; i++) out[i] = r[i];
}

bool k64_joybus_cmd(int channel, const u8 *tx, u32 ntx, u8 *rx, u32 nrx) {
  K64_ASSERT(channel >= 0 && channel <= 4 && ntx && ntx + nrx + channel + 3 <= 63);
  u8 in[64] = { 0 }, out[64];
  u32 i = channel;                             // 0x00 bytes skip the channels before it
  // Channel record: tx length, rx length, tx bytes, rx slots; 0xFE after the record ends the list.
  in[i] = ntx; in[i + 1] = nrx;
  for (u32 k = 0; k < ntx; k++) in[i + 2 + k] = tx[k];
  for (u32 k = 0; k < nrx; k++) in[i + 2 + ntx + k] = 0xFF;
  in[i + 2 + ntx + nrx] = 0xFE;
  in[63] = 0x01;
  k64_joybus_exec(in, out);
  if (out[i + 1] & 0xC0) return false;         // the reply's rx-length byte carries the error bits
  for (u32 k = 0; k < nrx; k++) rx[k] = out[i + 2 + ntx + k];
  return true;
}

k64_joy_device_t k64_joybus_identify(int channel, u16 *id, u8 *status) {
  u8 tx = 0x00, rx[3];
  // No reply: treat as an empty ID, which maps to K64_JOY_NONE.
  if (!k64_joybus_cmd(channel, &tx, 1, rx, 3)) rx[0] = rx[1] = rx[2] = 0;
  u16 t = (u16)(rx[0] << 8 | rx[1]);
  if (id) *id = t;
  if (status) *status = rx[2];
  switch (t) {
  case 0x0000: return K64_JOY_NONE;
  case 0x0500: return K64_JOY_CONTROLLER;
  case 0x0200: return K64_JOY_MOUSE;
  case 0x0001: return K64_JOY_VRU;
  case 0x0002: return K64_JOY_KEYBOARD;
  case 0x0080: return K64_JOY_EEPROM_4K;
  case 0x00C0: return K64_JOY_EEPROM_16K;
  default:     return K64_JOY_OTHER;
  }
}
