// kestrel64-sdk — Transfer Pak: a Game Boy / Game Boy Color cartridge on a controller port.
//
// The Transfer Pak bridges the cartridge's 16-bit bus to the accessory window of the pak
// slot (n64brew "Transfer Pak"):
//   0x8000  pak power: write 0x84 = on, 0xFE = off; reads 0x84 while on
//   0xA000  bank: which 16 KiB quarter of the GB address space 0xC000-0xFFFF shows
//   0xB000  cartridge access: write 1 = on, 0 = off; reads the status byte below
//   0xC000  the selected GB bus quarter (reads and writes go straight to the cartridge)
// Every transfer is 32 bytes, ~0.4 ms of joybus time: whole ROM dumps take minutes.
//
// The cartridge layer reads the header (Pan Docs "The Cartridge Header") and drives the
// memory bank controller so ROM / RAM are addressed linearly: no MBC, MBC1, MBC2, MBC3
// (+ RTC) and MBC5 — the controllers of the games that talk to the Transfer Pak.
#pragma once
#include "hw.h"

// Status byte (0xB000). PRESENT: a cartridge is seated. ACCESS: cartridge access is on.
// RESET: the GB reset line was seen (first status read after power-up). PULLED: the
// cartridge was removed since the last read.
#define K64_TPAK_ACCESS  0x01u
#define K64_TPAK_RESET   0x04u
#define K64_TPAK_PULLED  0x40u
#define K64_TPAK_PRESENT 0x80u

// Powers the pak and turns cartridge access on. false: no Transfer Pak, or no cartridge.
bool k64_tpak_start(int port);
// Cartridge access off, pak power off (do it before the player may swap cartridges).
void k64_tpak_stop(int port);
// Status byte, 0 when the transfer failed.
u8 k64_tpak_status(int port);

// Raw GB bus. Reads take any address/length. Writes go to whole 32-byte blocks (address and
// length multiples of 32). k64_tpak_poke fills one block with `v`: how MBC registers are
// written, since a register answers to every address of its range.
bool k64_tpak_read(int port, u16 gbaddr, void *dst, u32 len);
bool k64_tpak_write(int port, u16 gbaddr, const void *src, u32 len);
bool k64_tpak_poke(int port, u16 gbaddr, u8 v);

// ---- Cartridge -----------------------------------------------------------------------------

typedef enum { K64_GB_NOMBC, K64_GB_MBC1, K64_GB_MBC2, K64_GB_MBC3, K64_GB_MBC5, K64_GB_UNKNOWN } k64_gb_mbc_t;

typedef struct {
  int port;
  char title[17];       // NUL-terminated, trailing NULs/spaces dropped
  u8 type;              // header 0x147 (cartridge type)
  u8 cgb;               // header 0x143: 0x80 = CGB enhanced, 0xC0 = CGB only
  k64_gb_mbc_t mbc;
  bool battery, rtc;
  u32 rom_size, ram_size;
  u8 header[0x50];      // raw 0x100-0x14F
} k64_gbcart_t;

typedef enum {
  K64_GB_OK,
  K64_GB_ERR_IO,        // no Transfer Pak / transfer failed
  K64_GB_ERR_NOCART,    // pak present, slot empty
  K64_GB_ERR_HEADER,    // header checksum (0x14D) does not match: dirty contacts?
} k64_gb_err_t;

// Starts the pak and reads the header.
k64_gb_err_t k64_gbcart_open(k64_gbcart_t *c, int port);
// ROM bytes at a linear offset (bank * 16 KiB + offset in bank).
bool k64_gbcart_read_rom(k64_gbcart_t *c, u32 offset, void *dst, u32 len);
// External cartridge RAM (the save), linear across RAM banks. MBC2 RAM is 512 4-bit cells:
// only the low nibble of each byte is stored.
bool k64_gbcart_read_ram(k64_gbcart_t *c, u32 offset, void *dst, u32 len);
// Any offset/length: partial 32-byte blocks are read first and merged.
bool k64_gbcart_write_ram(k64_gbcart_t *c, u32 offset, const void *src, u32 len);
// MBC3 clock: latches, then reads seconds, minutes, hours, day low, day high (bit 0 = day
// bit 8, bit 6 = halted, bit 7 = day counter overflow).
bool k64_gbcart_rtc_read(k64_gbcart_t *c, u8 out[5]);
// Sets the five MBC3 clock registers (same layout). Set day-high bit 6 to stop the clock
// while setting it, then write again with the bit clear.
bool k64_gbcart_rtc_write(k64_gbcart_t *c, const u8 in[5]);
