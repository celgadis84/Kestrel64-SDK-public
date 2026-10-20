// kestrel64-sdk — cartridge save memory.
//
// One byte-addressed API over the save device. The type comes from the ROM header (set
// with k64_add_rom(... SAVE <type>), Advanced Homebrew ROM Header at 0x3F, which
// flashcarts and emulators read too); EEPROM is also probed on the joybus.
//
// Costs (blocking, polling runtime):
//  * EEPROM: 8-byte blocks, ~0.5 ms per joybus transaction; a block write keeps the chip
//    busy for up to ~15 ms on hardware (waited for on the next access). Writes compare
//    first and skip blocks that already hold the data.
//  * SRAM: PI DMA over domain 2, a few us per KB. Battery-backed, no wear.
//  * FlashRAM: 128-byte pages, 16 KiB erase sectors. Writes skip unchanged pages, program
//    still-erased pages in place, and otherwise rewrite the sector (an erase takes up to
//    ~0.5 s on hardware; a 16 KiB scratch buffer comes from the heap zone on first use).
#pragma once
#include "hw.h"

typedef enum {
  K64_SAVE_NONE,
  K64_SAVE_EEPROM4K,      // 512 bytes
  K64_SAVE_EEPROM16K,     // 2 KB
  K64_SAVE_SRAM256K,      // 32 KB
  K64_SAVE_SRAM1M,        // 128 KB
  K64_SAVE_FLASH1M,       // 128 KB FlashRAM
  K64_SAVE_SRAM768K,      // 96 KB: three 32 KB banks (address bits 18-19), one linear range here
} k64_save_type_t;

// Detects the device; call once before the others. Returns its type.
k64_save_type_t k64_save_init(void);
k64_save_type_t k64_save_type(void);
u32 k64_save_size(void);

// Any offset / length / alignment. False if out of range or the device did not answer.
bool k64_save_read(u32 offset, void *dst, u32 len);
bool k64_save_write(u32 offset, const void *src, u32 len);
