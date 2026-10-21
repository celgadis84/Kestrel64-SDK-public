// kestrel64-sdk — cartridge joybus RTC (see k64/rtc.h).
#include <k64/rtc.h>
#include <k64/joybus.h>
#include <k64/cpu.h>

#define RTC_CHANNEL 4                // cartridge slot on the joybus
#define CMD_INFO    0x06
#define CMD_READ    0x07
#define CMD_WRITE   0x08
#define RTC_ID      0x0010

// INFO command: device ID in rx[0..1], status bits in rx[2] (K64_RTC_STOPPED and friends).
static bool info(u8 *status) {
  u8 tx = CMD_INFO, rx[3];
  if (!k64_joybus_cmd(RTC_CHANNEL, &tx, 1, rx, 3) || (rx[0] << 8 | rx[1]) != RTC_ID) return false;
  *status = rx[2];
  return true;
}

bool k64_rtc_present(void) { u8 s; return info(&s); }
u8 k64_rtc_status(void) { u8 s; return info(&s) ? s : 0xFF; }

// Reads one 8-byte block; the reply is 9 bytes, the last one is not copied out.
static bool block_read(u8 block, u8 out[8]) {
  u8 tx[2] = { CMD_READ, block }, rx[9];
  if (!k64_joybus_cmd(RTC_CHANNEL, tx, 2, rx, 9)) return false;
  for (int i = 0; i < 8; i++) out[i] = rx[i];
  return true;
}

// Writes one 8-byte block (block number, then the data); the reply is one status byte.
static bool block_write(u8 block, const u8 in[8]) {
  u8 tx[10] = { CMD_WRITE, block }, st;
  for (int i = 0; i < 8; i++) tx[2 + i] = in[i];
  return k64_joybus_cmd(RTC_CHANNEL, tx, 10, &st, 1);
}

// BCD: two decimal digits per byte, tens in the high nibble.
static u8 bcd(u8 v) { return (u8)((v / 10) << 4 | v % 10); }
static u8 unbcd(u8 v) { return (u8)((v >> 4) * 10 + (v & 15)); }

bool k64_rtc_get(k64_datetime_t *t) {
  u8 b[8];
  if (!k64_rtc_present() || !block_read(2, b)) return false;
  t->sec = unbcd(b[0] & 0x7F);
  t->min = unbcd(b[1] & 0x7F);
  t->hour = unbcd(b[2] & 0x3F);
  t->day = unbcd(b[3] & 0x3F);
  t->weekday = b[4] & 7;
  t->month = unbcd(b[5] & 0x1F);
  // b[7] is the century offset from 1900, b[6] the two-digit year.
  t->year = (u16)(1900 + 100 * unbcd(b[7]) + unbcd(b[6]));
  return true;
}

// Block 0 is the control block: byte 0 = protect (0x00 unlocked, 0x03 both locked), byte 1 = stop
// (0x04 halts the clock).
static bool control(u8 protect, u8 stop) {
  u8 c[8] = { protect, stop };
  return block_write(0, c);
}

bool k64_rtc_set(const k64_datetime_t *t) {
  if (t->year < 1900 || t->year > 2099 || t->month < 1 || t->month > 12 || t->day < 1 || t->day > 31 ||
      t->hour > 23 || t->min > 59 || t->sec > 59 || t->weekday > 6 || !k64_rtc_present())
    return false;
  // Unprotect + stop, and wait for the status to confirm before touching block 2.
  if (!control(0x00, 0x04)) return false;
  u32 t0 = k64_count();
  while (!(k64_rtc_status() & K64_RTC_STOPPED))
    if (K64_US(k64_count() - t0) > 50000) { control(0x03, 0x00); return false; }  // 50 ms, re-lock
  u16 y = t->year - 1900;
  u8 b[8] = { bcd(t->sec), bcd(t->min), (u8)(bcd(t->hour) | 0x80), bcd(t->day), t->weekday,
              bcd(t->month), bcd((u8)(y % 100)), bcd((u8)(y / 100)) };
  bool ok = block_write(2, b);
  return control(0x03, 0x00) && ok;       // run, protect both
}

bool k64_rtc_sram_read(u8 out[8]) { return k64_rtc_present() && block_read(1, out); }

bool k64_rtc_sram_write(const u8 in[8]) {
  if (!k64_rtc_present() || !control(0x00, 0x00)) return false;   // NVRAM protect off
  bool ok = block_write(1, in);
  return control(0x03, 0x00) && ok;
}

u8 k64_weekday(u16 year, u8 month, u8 day) {
  // Sakamoto's method; 0 = Sunday.
  static const u8 k[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
  if (month < 3) year--;
  return (u8)((year + year / 4 - year / 100 + year / 400 + k[(month - 1) % 12] + day) % 7);
}
