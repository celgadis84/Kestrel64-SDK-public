// kestrel64-sdk — read-only file system in the cartridge ROM.
//
// k64_add_rom(... ROMFS dir[:--pack GLOB ...]) packs a directory tree (tools/k64fs.py) and
// links it ROM-resident. Paths are relative to that directory, '/' separated, case
// sensitive; a leading '/' is ignored. Lookups hash the path and binary search the
// directory, which is copied to RAM (K64_ZONE_HEAP) on the first call.
//
// Files matching a --pack glob are stored as .k64z: k64_romfs_load decodes them, while
// k64_romfs_read returns the stored (packed) bytes.
#pragma once
#include "hw.h"

typedef struct {
  u32 rom;      // PI address of the data
  u32 size;     // stored bytes
  u32 pos;
  bool packed;
} k64_file_t;

// False when the ROM has no file system or no such file.
bool k64_romfs_open(k64_file_t *f, const char *path);
// Reads up to len bytes at the file position. Any dst alignment and length; reads that
// start 8-byte aligned in RAM and 2-byte aligned in the file go straight through PI DMA,
// the rest through a bounce buffer. Not reentrant (one thread at a time).
u32  k64_romfs_read(k64_file_t *f, void *dst, u32 len);
// Moves the position (clamped to the file), returns it.
u32  k64_romfs_seek(k64_file_t *f, u32 pos);
// Whole file into a new K64_ZONE_HEAP allocation (16-byte aligned), decoded if packed.
// NULL when missing. *size = decoded bytes.
void *k64_romfs_load(const char *path, u32 *size);

// Directory listing, in hash order. Index < k64_romfs_count().
u32  k64_romfs_count(void);
const char *k64_romfs_name(u32 i);
