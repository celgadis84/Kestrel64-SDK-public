// kestrel64-sdk — flashcart SD card + FAT file system.
//
// Cart drivers are libcart (third_party/libcart, public domain): 64Drive, EverDrive-64 X
// series, EverDrive-64 V1/V2/V2.5/V3/ED64+, SummerCart64. The file system is FatFs
// (third_party/fatfs, ChaN's BSD-style license): FAT12/16/32 and exFAT, long names, read
// and write. After k64_sd_init the card is FatFs volume "" (or "0:"): any f_* call of ff.h
// works on it, e.g. f_open(&fil, "save/slot1.bin", FA_WRITE | FA_CREATE_ALWAYS). Paths
// without a leading '/' are relative to the current folder (k64_sd_chdir).
//
// Reads go card -> cart SDRAM (the cart's own DMA) -> RDRAM (PI DMA), through a 32 KiB
// scratch area at the top of the SDRAM libcart reports (cart_size: 64 MiB, less the save
// area V1/V2 EverDrives and some SummerCart64 setups keep there). The ROM must end below it;
// keeping it under 63 MiB is enough on every supported cart but a SummerCart64 that reports
// less (then cart_size - 32 KiB).
#pragma once
#include "hw.h"
#include "../../third_party/fatfs/ff.h"

typedef enum {
  K64_SD_NONE = -1, K64_SD_64DRIVE = 0, K64_SD_EVERDRIVE_X = 1, K64_SD_EVERDRIVE = 2, K64_SD_SUMMERCART64 = 3,
} k64_sd_cart_t;

// Detects the cart, starts the card and mounts it. False when there is no supported cart,
// no card or no FAT/exFAT volume; k64_sd_error() says which. Safe to call again.
bool k64_sd_init(void);
const char *k64_sd_error(void);
k64_sd_cart_t k64_sd_cart(void);
const char *k64_sd_cart_name(void);

// Folders. A flashcart does not tell the ROM where on the card it was started from, so a
// game keeps its files in its own folder (next to its ROM, anywhere on the card) and finds
// that folder at boot: k64_sd_find tries `hint` first, then walks the card (folders
// `K64_SD_DEPTH` deep, dot folders skipped) for a folder that holds a file `name` for which
// `ok(path_to_file, ctx)` is true (pass NULL to take the first). The folder's path goes to
// `dir` ("GAMES/DISC1", no leading slash; "" is the root). After k64_sd_chdir(dir), relative
// paths ("STREAM.BIN", "saves/card1.mcd", "../other") resolve inside it, as do f_* calls.
#define K64_SD_DEPTH 4
typedef bool (*k64_sd_match_t)(const char *path, void *ctx);
bool k64_sd_find(const char *hint, const char *name, k64_sd_match_t ok, void *ctx, char *dir, u32 dirlen);
bool k64_sd_chdir(const char *dir);

// Raw 512-byte sectors, any alignment.
bool k64_sd_read_sectors(u32 lba, u32 n, void *dst);
bool k64_sd_write_sectors(u32 lba, u32 n, const void *src);

// Read-only file with a fast-seek map (random access without walking the cluster chain).
#define K64_SD_MAP 64            // DWORDs: (fragments + 1) * 2; a fresh copy has 1 fragment
typedef struct {
  FIL fil;
  DWORD map[K64_SD_MAP];
} k64_sdfile_t;

// false if the card is not mounted or the file is missing. A file in more fragments than
// the map holds still opens, with slow (chain-walking) seeks.
bool k64_sd_open(k64_sdfile_t *f, const char *path);
K64_INLINE u32 k64_sd_size(const k64_sdfile_t *f) { return (u32)f_size(&f->fil); }
// len bytes at off (clamped to the file), returns the bytes read. Any alignment; whole
// sectors into an 8-byte aligned dst go straight from the cart by PI DMA.
u32  k64_sd_pread(k64_sdfile_t *f, void *dst, u32 off, u32 len);
void k64_sd_close(k64_sdfile_t *f);
