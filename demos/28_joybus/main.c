// Demo 28 — joybus devices: identify every channel, read the cartridge clock when there is
// one, show mouse motion when a mouse is plugged. Self-test: identification of the
// controller in port 1, k64_weekday; with a clock: read, set/read-back/restore, SRAM round
// trip; without one: the clock API answering "no clock" cleanly.
#include <k64/k64.h>

static u32 fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; k64_printf("[28_joybus] FAIL " __VA_ARGS__); k64_printf("\n"); } } while (0)

static const char *names[] = { "none", "controller", "mouse", "VRU", "keyboard", "eeprom 4k", "eeprom 16k", "other" };

int main(void) {
  k64_joy_device_t dev[5];
  for (int ch = 0; ch < 5; ch++) {
    u16 id; u8 st;
    dev[ch] = k64_joybus_identify(ch, &id, &st);
    k64_printf("[28_joybus] channel %d: %s (id %04x status %02x)\n", ch, names[dev[ch]], id, st);
  }
  CHECK(dev[0] == K64_JOY_CONTROLLER, "port 1 is %s", names[dev[0]]);

  CHECK(k64_weekday(2000, 1, 1) == 6 && k64_weekday(2026, 10, 6) == 2 && k64_weekday(1999, 12, 31) == 5 &&
        k64_weekday(2024, 2, 29) == 4, "weekday");

  k64_datetime_t t;
  if (k64_rtc_present()) {
    u8 st = k64_rtc_status();
    bool ok = k64_rtc_get(&t);
    k64_printf("[28_joybus] rtc status %02x: %04u-%02u-%02u %02u:%02u:%02u weekday %u\n", st, t.year, t.month,
               t.day, t.hour, t.min, t.sec, t.weekday);
    CHECK(ok && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31 && t.hour < 24, "rtc read");

    // Set a known time, read it back, then put the original back (a few ms are lost).
    k64_datetime_t set = { 2031, 12, 31, 0, 23, 59, 58 }, got;
    set.weekday = k64_weekday(set.year, set.month, set.day);
    ok = k64_rtc_set(&set) && k64_rtc_get(&got);
    k64_printf("[28_joybus] rtc set -> %04u-%02u-%02u %02u:%02u:%02u\n", got.year, got.month, got.day,
               got.hour, got.min, got.sec);
    CHECK(ok && got.year == 2031 && got.month == 12 && got.day == 31 && got.hour == 23 && got.min == 59 &&
          got.sec >= 58 && got.weekday == set.weekday, "rtc set/get");
    CHECK(k64_rtc_set(&t) && !(k64_rtc_status() & K64_RTC_STOPPED), "rtc restore / running");

    u8 sram[8], pat[8] = { 'K', '6', '4', 'R', 'T', 'C', (u8)k64_count(), 0x5A }, back[8];
    ok = k64_rtc_sram_read(sram) && k64_rtc_sram_write(pat) && k64_rtc_sram_read(back);
    for (int i = 0; ok && i < 8; i++) ok = back[i] == pat[i];
    CHECK(ok && k64_rtc_sram_write(sram), "rtc sram");
  } else {
    k64_printf("[28_joybus] no cartridge clock\n");
    CHECK(!k64_rtc_get(&t) && !k64_rtc_set(&(k64_datetime_t){ 2026, 10, 6, 2, 12, 0, 0 }) &&
          k64_rtc_status() == 0xFF, "no-clock path");
  }
  k64_printf("[28_joybus] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);

  for (;;) {                                // mouse: accumulate relative motion
    static s32 mx, my;
    k64_vi_wait_vblank();
    k64_pad_poll();
    for (int p = 0; p < 4; p++)
      if (dev[p] == K64_JOY_MOUSE && (k64_pads[p].x || k64_pads[p].y)) {
        mx += k64_pads[p].x; my += k64_pads[p].y;
        k64_printf("[28_joybus] mouse %d at %d,%d buttons %04x\n", p + 1, mx, my, k64_pads[p].held);
      }
  }
}
