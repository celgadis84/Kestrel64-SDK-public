// kestrel64-sdk — cartridge (PI) DMA.
// ROM-resident data: declare with K64_ROM_DATA / link via k64_add_assets(... ROM ...); the
// symbol's address IS the PI bus address (0x10000000 + rom offset).
#pragma once
#include "hw.h"

#define K64_ROM_DATA __attribute__((section(".rom")))

// Synchronous: dst 8-byte aligned, rom address 2-byte aligned, len multiple of 2.
// Invalidates dst in D$ (any dirty data there is discarded).
void k64_pi_read(void *dst, u32 rom_addr, u32 len);
// Async: start and poll. Caller must invalidate dst before reading it.
void k64_pi_read_async(void *dst, u32 rom_addr, u32 len);
K64_INLINE bool k64_pi_busy(void) { return (PI_STATUS & (PI_ST_DMA_BUSY | PI_ST_IO_BUSY)) != 0; }
K64_INLINE void k64_pi_wait(void) { while (k64_pi_busy()) {} }
