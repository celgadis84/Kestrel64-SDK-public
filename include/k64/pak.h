// kestrel64-sdk — controller accessories (the pak slot of each controller).
//
// Every call is one or more blocking joybus transactions, ~0.4 ms each in PIF time: probe
// at boot or when a controller reports a pak change, not every frame. A pending
// k64_pad_poll_start() is collected first, so these may be called at any point.
//
// The Controller Pak is exposed raw (32 KiB, byte-addressed) and as notes (files) in the
// libultra file system layout, so notes are shared with commercial games (docs/PAK.md).
#pragma once
#include "hw.h"

typedef enum {
  K64_PAK_NONE,         // empty slot or no controller
  K64_PAK_CONTROLLER,   // Controller Pak (32 KiB SRAM)
  K64_PAK_RUMBLE,       // Rumble Pak
  K64_PAK_TRANSFER,     // Transfer Pak (detected only)
  K64_PAK_UNKNOWN,      // something answers but is none of the above
} k64_pak_type_t;

#define K64_CPAK_SIZE 0x8000u

// Identifies the pak in `port` (0..3). Non-destructive: the Controller Pak test writes a
// pattern over the first 32 bytes and restores them.
k64_pak_type_t k64_pak_probe(int port);

// Raw 32-byte accessory transfers at `addr` (multiple of 32). Both check the data CRC the
// pak returns; false = no controller, no pak, or a corrupted transfer.
bool k64_pak_read32(int port, u16 addr, u8 out[32]);
bool k64_pak_write32(int port, u16 addr, const u8 in[32]);

// Controller Pak bytes (bank 0). Partial 32-byte blocks are read first, and left alone when
// unchanged; whole blocks are written directly.
bool k64_cpak_read(int port, u32 offset, void *dst, u32 len);
bool k64_cpak_write(int port, u32 offset, const void *src, u32 len);

// Rumble Pak motor on/off. Call k64_pak_probe first: it arms the pak.
bool k64_rumble(int port, bool on);

// ---- Controller Pak notes ------------------------------------------------------------------
// The pak is split in 256-byte pages: page 0 holds the ID block, then the page table (one per
// 32 KiB bank) and its backup copy, then the 16-entry note table (2 pages); the rest are data
// pages (123 on a plain 32 KiB pak). A note is a chain of pages named by game code, company
// code, a 16-character name and a 4-character extension.

#define K64_CPAK_NOTES     16
#define K64_CPAK_PAGE      256u
#define K64_CPAK_MAX_BANKS 4          // 128 KiB third-party paks

typedef enum {
  K64_CPAK_OK,
  K64_CPAK_ERR_IO,          // no pak, or a transfer failed
  K64_CPAK_ERR_NOFS,        // ID block unreadable: not formatted (k64_cpak_format)
  K64_CPAK_ERR_CORRUPT,     // page table and its backup both damaged
  K64_CPAK_ERR_FULL,        // not enough free pages
  K64_CPAK_ERR_DIRFULL,     // all 16 notes used
  K64_CPAK_ERR_EXISTS,      // a note with this game, company, name and extension exists
  K64_CPAK_ERR_NOTFOUND,    // no such note
  K64_CPAK_ERR_NAME,        // a character the pak font cannot store
  K64_CPAK_ERR_RANGE,       // offset / length past the note
} k64_cpak_err_t;

typedef struct {
  int port;
  u8 banks, first_page, cur_bank;   // first_page: first data page of bank 0
  u8 fixed;                         // mount repaired something (backup table, lost pages)
  u8 id[32];
  u8 table[K64_CPAK_MAX_BANKS][256];
  u8 dir[K64_CPAK_NOTES][32];
} k64_cpak_fs_t;

typedef struct {
  u32 game_code;
  u16 company_code;
  char name[17], ext[5];            // ASCII (characters outside it read as '~')
  u32 pages;
} k64_cpak_note_t;

// Reads the ID block (any of its 4 copies), the page table (the backup if the main copy is
// damaged) and the note table, then checks every note's chain: pages marked used but owned by
// no note are freed. ~60 joybus transfers on a 32 KiB pak.
k64_cpak_err_t k64_cpak_mount(k64_cpak_fs_t *fs, int port);
// Erases every note: new ID block (bank count detected), empty page and note tables.
k64_cpak_err_t k64_cpak_format(int port);

// Unused data pages (256 bytes each) on the mounted pak.
u32 k64_cpak_free_pages(const k64_cpak_fs_t *fs);
// false: entry `i` (0..15) unused.
bool k64_cpak_note(const k64_cpak_fs_t *fs, int i, k64_cpak_note_t *out);
// Index of the note, or -1. name / ext: ASCII, upper or lower case, NUL-terminated.
int k64_cpak_find(const k64_cpak_fs_t *fs, u32 game, u16 company, const char *name, const char *ext);
// New note of `bytes` (rounded up to whole pages, contents left as found) in the first free
// entry; *index gets its entry.
k64_cpak_err_t k64_cpak_create(k64_cpak_fs_t *fs, u32 game, u16 company, const char *name,
                               const char *ext, u32 bytes, int *index);
k64_cpak_err_t k64_cpak_delete(k64_cpak_fs_t *fs, int index);
k64_cpak_err_t k64_cpak_note_read(k64_cpak_fs_t *fs, int index, u32 offset, void *dst, u32 len);
k64_cpak_err_t k64_cpak_note_write(k64_cpak_fs_t *fs, int index, u32 offset, const void *src, u32 len);
