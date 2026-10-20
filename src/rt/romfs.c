// kestrel64-sdk — ROM file system (see include/k64/romfs.h, tools/k64fs.py for the image).
#include <k64/romfs.h>
#include <k64/pi.h>
#include <k64/pack.h>
#include <k64/mem.h>
#include <k64/debug.h>

#define MAGIC   0x4B363453u   // 'K64S'
#define PACKED  0x80000000u

typedef struct { u32 hash, name, data, size; } entry_t;

// The image, linked by k64_add_rom(... ROMFS ...): its address is the PI address.
extern const u8 k64_romfs[] __attribute__((weak));

static const u8 *dir;           // header + entries + names, in RAM
static u32 count;
static u64 bounce[64];          // 512 bytes: unaligned reads

static bool mount(void) {
  if (dir) return true;
  if (!k64_romfs) return false;
  u32 rom = (u32)k64_romfs;
  u32 hdr[4] K64_ALIGN(16);
  k64_pi_read(hdr, rom, 16);
  if (hdr[0] != MAGIC) return false;
  u8 *d = k64_mem_alloc(K64_ZONE_HEAP, hdr[2], 16);
  k64_pi_read(d, rom, hdr[2]);
  count = hdr[1];
  dir = d;
  return true;
}

static const entry_t *entries(void) { return (const entry_t *)(dir + 16); }

static const entry_t *find(const char *path) {
  if (!mount()) return NULL;
  while (*path == '/') path++;
  u32 h = 0x811C9DC5u;
  for (const u8 *p = (const u8 *)path; *p; p++) h = (h ^ *p) * 0x01000193u;
  const entry_t *e = entries();
  u32 lo = 0, hi = count;
  while (lo < hi) {                                     // first entry with hash >= h
    u32 mid = (lo + hi) / 2;
    if (e[mid].hash < h) lo = mid + 1; else hi = mid;
  }
  for (; lo < count && e[lo].hash == h; lo++)
    if (!__builtin_strcmp((const char *)dir + e[lo].name, path)) return &e[lo];
  return NULL;
}

bool k64_romfs_open(k64_file_t *f, const char *path) {
  const entry_t *e = find(path);
  if (!e) return false;
  *f = (k64_file_t){ .rom = (u32)k64_romfs + e->data, .size = e->size & ~PACKED,
                     .packed = (e->size & PACKED) != 0 };
  return true;
}

u32 k64_romfs_read(k64_file_t *f, void *dst, u32 len) {
  if (len > f->size - f->pos) len = f->size - f->pos;
  u8 *d = dst;
  for (u32 left = len; left;) {
    u32 rom = f->rom + f->pos, n;
    if (((u32)d & 7) == 0 && (rom & 1) == 0 && left >= 2) {
      n = left & ~1u;                                   // PI DMA lengths are even
      k64_pi_read(d, rom, n);
    } else {
      u32 skew = rom & 1;
      n = left < sizeof bounce - 2 ? left : sizeof bounce - 2;
      k64_pi_read(bounce, rom - skew, (skew + n + 1) & ~1u);   // the file data is padded to 16
      __builtin_memcpy(d, (u8 *)bounce + skew, n);
    }
    d += n; f->pos += n; left -= n;
  }
  return len;
}

u32 k64_romfs_seek(k64_file_t *f, u32 pos) {
  f->pos = pos < f->size ? pos : f->size;
  return f->pos;
}

void *k64_romfs_load(const char *path, u32 *size) {
  k64_file_t f;
  if (!k64_romfs_open(&f, path)) return NULL;
  if (f.packed) return k64_unpack_rom_alloc(f.rom, size);
  void *p = k64_mem_alloc(K64_ZONE_HEAP, (f.size + 15) & ~15u, 16);
  if (f.size) k64_pi_read(p, f.rom, (f.size + 1) & ~1u);
  if (size) *size = f.size;
  return p;
}

u32 k64_romfs_count(void) { return mount() ? count : 0; }

const char *k64_romfs_name(u32 i) {
  return i < k64_romfs_count() ? (const char *)dir + entries()[i].name : NULL;
}
