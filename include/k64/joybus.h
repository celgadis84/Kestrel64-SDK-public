// kestrel64-sdk — raw joybus transactions through the PIF.
//
// A transaction is a 64-byte PIF RAM program: per channel {tx, rx, tx bytes, rx bytes},
// a 0x00 byte skips a channel, 0xFE ends the program, byte 63 = 1 runs it. Channels 0-3
// are the controller ports, 4 is the cartridge (EEPROM, RTC). The PIF fills the rx bytes
// in place and sets error bits in each rx count (0x80 no device, 0x40 overrun).
// Blocking, ~0.5 ms of PIF time per transaction. k64_pad_poll() is a ready-made one.
#pragma once
#include "hw.h"

void k64_joybus_exec(const u8 in[64], u8 out[64]);

// One command on one channel: tx bytes in, rx bytes out. Returns false if the device did
// not answer (no device / error bits).
bool k64_joybus_cmd(int channel, const u8 *tx, u32 ntx, u8 *rx, u32 nrx);

// Device identity from the Info command (0x00): 16-bit type + status byte (n64brew "Joybus
// Protocol"). Channels 0-3 are controller ports; channel 4 the cartridge (EEPROM type).
typedef enum {
  K64_JOY_NONE,          // nothing answers
  K64_JOY_CONTROLLER,    // 0x0500 (also dance pad, Densha de Go controller)
  K64_JOY_MOUSE,         // 0x0200: k64_pads[] reads it, x/y = relative motion, A/B only
  K64_JOY_VRU,           // 0x0001: Voice Recognition Unit (no driver)
  K64_JOY_KEYBOARD,      // 0x0002: Randnet keyboard (no driver)
  K64_JOY_EEPROM_4K,     // 0x0080
  K64_JOY_EEPROM_16K,    // 0x00C0
  K64_JOY_OTHER,         // any other identifier
} k64_joy_device_t;

// Identifies the device on `channel`. `id` / `status` (optional) get the raw reply.
k64_joy_device_t k64_joybus_identify(int channel, u16 *id, u8 *status);
