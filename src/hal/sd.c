// kestrel64-sdk — flashcart SD card (see k64/sd.h): FatFs disk I/O over libcart.
#include <k64/sd.h>
#include <k64/pi.h>
#include <k64/debug.h>
#include "../../third_party/fatfs/diskio.h"
#include "../../third_party/libcart/cart.h"

#define SCRATCH 0x8000u                    // landing area in cart SDRAM, 2 KiB aligned

static const char *err = "not initialised";
static bool ready;
static u32 scratch;                        // PI address
static FATFS fs;
// Aligned sector buffer for unaligned caller buffers (PI DMA needs 8-byte alignment).
static u8 bounce[512] K64_ALIGN(16);

// card -> SDRAM by the cart, SDRAM -> RDRAM by PI DMA: much faster than the CPU reading the
// card's data port byte by byte (which is what cart_card_rd_dram does on the EverDrive V1-V3)
bool k64_sd_read_sectors(u32 lba, u32 n, void *dst) {
  if (!scratch) return false;
  u8 *d = dst;
  while (n) {
    u32 k = n < SCRATCH / 512 ? n : SCRATCH / 512;
    if ((u32)d & 7) k = 1;                     // unaligned destination: one sector per pass
    if (cart_card_rd_cart(scratch, lba, k)) return false;
    if ((u32)d & 7) {
      k64_pi_read(bounce, scratch, 512);
      for (u32 i = 0; i < 512; i++) d[i] = bounce[i];
    } else {
      k64_pi_read(d, scratch, k * 512);
    }
    d += k * 512; lba += k; n -= k;
  }
  return true;
}

bool k64_sd_write_sectors(u32 lba, u32 n, const void *src) {
  if (!scratch) return false;
  const u8 *s = src;
  for (; n; n--, s += 512, lba++) {
    const u8 *p = s;
    // Unaligned source: copy into the aligned bounce buffer first.
    if ((u32)s & 7) { for (u32 i = 0; i < 512; i++) bounce[i] = s[i]; p = bounce; }
    if (cart_card_wr_dram(p, lba, 1)) return false;
  }
  return true;
}

// ---- FatFs disk interface (one drive) ----
// Drive 0 is the flashcart SD card; it is ready once k64_sd_init() has set the scratch area.
DSTATUS disk_initialize(BYTE pdrv) { return pdrv || !scratch ? STA_NOINIT : 0; }
DSTATUS disk_status(BYTE pdrv) { return pdrv || !scratch ? STA_NOINIT : 0; }
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv || sector >> 32) return RES_PARERR;
  return k64_sd_read_sectors((u32)sector, count, buff) ? RES_OK : RES_ERROR;
}
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
  if (pdrv || sector >> 32) return RES_PARERR;
  return k64_sd_write_sectors((u32)sector, count, buff) ? RES_OK : RES_ERROR;
}
// Only CTRL_SYNC is needed: writes complete before they return.
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
  return cmd == CTRL_SYNC ? RES_OK : RES_PARERR;
}

bool k64_sd_init(void) {
  if (ready) return true;
  if (!scratch) {
    if (cart_init() < 0) { err = "no supported flashcart"; return false; }
    if (cart_card_init()) { err = "no SD card"; return false; }
    // Scratch area at the top of cart SDRAM, 2 KiB aligned.
    scratch = (0x10000000u + cart_size - SCRATCH) & ~0x7FFu;
  }
  if (f_mount(&fs, "", 1) != FR_OK) { err = "no FAT/exFAT volume"; return false; }
  ready = true;
  err = "ok";
  return true;
}

const char *k64_sd_error(void) { return err; }
k64_sd_cart_t k64_sd_cart(void) { return (k64_sd_cart_t)cart_type; }
const char *k64_sd_cart_name(void) {
  // Indexed by cart_type.
  static const char *name[] = { "64Drive", "EverDrive-64 X", "EverDrive-64", "SummerCart64" };
  return cart_type >= 0 && cart_type < CART_MAX ? name[cart_type] : "none";
}

bool k64_sd_open(k64_sdfile_t *f, const char *path) {
  if (!ready || f_open(&f->fil, path, FA_READ) != FR_OK) return false;
  // Fragment table for fast seeks (FatFs CREATE_LINKMAP), kept per open file.
  f->map[0] = K64_SD_MAP;
  f->fil.cltbl = f->map;
  if (f_lseek(&f->fil, CREATE_LINKMAP) != FR_OK) {
    k64_printf("[sd] %s: more than %d fragments, no fast seek\n", path, K64_SD_MAP / 2 - 1);
    f->fil.cltbl = 0;
  }
  return true;
}

u32 k64_sd_pread(k64_sdfile_t *f, void *dst, u32 off, u32 len) {
  UINT got = 0;
  if (f_lseek(&f->fil, off) != FR_OK || f_read(&f->fil, dst, len, &got) != FR_OK) return 0;
  return got;
}

void k64_sd_close(k64_sdfile_t *f) { f_close(&f->fil); }

// ---- folders ----
// Working path buffer for k64_sd_find(); find_info is the FatFs stat/readdir buffer.
static char find_path[256];
static FILINFO find_info;

// strlen.
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }

// find_path holds the folder (len chars); true when it holds a matching `name`
static bool find_here(u32 len, const char *name, k64_sd_match_t ok, void *ctx) {
  u32 n = slen(name);
  if (len + 1 + n >= sizeof find_path) return false;
  u32 at = len;
  if (len) find_path[at++] = '/';
  for (u32 i = 0; i <= n; i++) find_path[at + i] = name[i];
  bool hit = f_stat(find_path, &find_info) == FR_OK && !(find_info.fattrib & AM_DIR) && (!ok || ok(find_path, ctx));
  find_path[len] = 0;
  return hit;
}

// Depth-first search below the folder in find_path (len chars), at most depth levels down.
static bool find_walk(u32 len, int depth, const char *name, k64_sd_match_t ok, void *ctx) {
  if (find_here(len, name, ok, ctx)) return true;
  if (!depth) return false;
  DIR d;
  if (f_opendir(&d, len ? find_path : "/") != FR_OK) return false;
  bool hit = false;
  while (!hit && f_readdir(&d, &find_info) == FR_OK && find_info.fname[0]) {
    if (!(find_info.fattrib & AM_DIR) || find_info.fname[0] == '.' || (find_info.fattrib & AM_SYS)) continue;
    u32 n = slen(find_info.fname), at = len;
    if (len + 1 + n >= sizeof find_path) continue;
    if (len) find_path[at++] = '/';
    for (u32 i = 0; i <= n; i++) find_path[at + i] = find_info.fname[i];
    hit = find_walk(at + n, depth - 1, name, ok, ctx);
    if (!hit) find_path[len] = 0;
  }
  f_closedir(&d);
  return hit;
}

bool k64_sd_find(const char *hint, const char *name, k64_sd_match_t ok, void *ctx, char *dir, u32 dirlen) {
  if (!ready) return false;
  bool hit = false;
  // Try the hint folder first; the full walk runs only when that misses.
  if (hint) {
    while (*hint == '/') hint++;
    u32 n = slen(hint);
    if (n < sizeof find_path) {
      for (u32 i = 0; i <= n; i++) find_path[i] = hint[i];
      hit = find_here(n, name, ok, ctx);
    }
  }
  if (!hit) { find_path[0] = 0; hit = find_walk(0, K64_SD_DEPTH, name, ok, ctx); }
  if (!hit) return false;
  u32 n = slen(find_path);
  if (n >= dirlen) return false;
  for (u32 i = 0; i <= n; i++) dir[i] = find_path[i];
  return true;
}

bool k64_sd_chdir(const char *dir) {
  if (!ready) return false;
  // dir is relative to the root, so start there.
  if (f_chdir("/") != FR_OK) return false;
  return !*dir || f_chdir(dir) == FR_OK;
}
