// kestrel64-sdk — cartridge real-time clock (joybus RTC on the cartridge channel).
//
// From n64brew "Joybus Protocol": RTC Info 0x06 (id 0x0010 + status), Read block 0x07,
// Write block 0x08; 8-byte blocks:
//   block 0  control: byte 0 bits 0-1 write protect (RTC, NVRAM), byte 1 bits 1-2 stop
//   block 1  8 bytes of battery-backed SRAM
//   block 2  date/time, packed BCD: sec, min, hour (| 0x80 = 24 h), day, weekday (0-6),
//            month, year % 100, centuries since 1900
// Status bit 7 = stopped (block 2 is safe to write), bit 1 crystal fail, bit 0 battery fail.
// Carts with the clock: Animal Forest, Dezaemon 3D; flash carts often emulate it.
#pragma once
#include "hw.h"

#define K64_RTC_STOPPED      0x80u
#define K64_RTC_CRYSTAL_FAIL 0x02u
#define K64_RTC_BATTERY_FAIL 0x01u

typedef struct {
  u16 year;      // 1900-2099
  u8 month;      // 1-12
  u8 day;        // 1-31
  u8 weekday;    // 0-6 (0 = Sunday)
  u8 hour, min, sec;
} k64_datetime_t;

// true when the cartridge answers RTC Info with the clock identifier.
bool k64_rtc_present(void);
// Status byte (K64_RTC_*), 0xFF when there is no clock.
u8 k64_rtc_status(void);
bool k64_rtc_get(k64_datetime_t *t);
// Stops the clock, writes the time, restarts it and protects it again. false: no clock, an
// out-of-range field, or the clock never reported stopped.
bool k64_rtc_set(const k64_datetime_t *t);
// The 8 SRAM bytes of block 1.
bool k64_rtc_sram_read(u8 out[8]);
bool k64_rtc_sram_write(const u8 in[8]);
// Day of week (0 = Sunday) for a Gregorian date.
u8 k64_weekday(u16 year, u8 month, u8 day);
