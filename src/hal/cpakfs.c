// kestrel64-sdk — Controller Pak notes in the libultra file system layout (docs/PAK.md).
//
// 256-byte pages, 32 KiB banks (bank select: a write to 0x8000), everything big-endian.
//   page 0                 ID block x4 (blocks 1, 3, 4, 6), label (block 0)
//   pages 1 .. B           page table of each bank: 128 two-byte entries {bank, page}
//   pages 1+B .. 2B        backup copy of the page tables
//   pages 1+2B, 2+2B       note table: 16 x 32 bytes
//   pages 3+2B ..          data (bank 0); pages 1..127 in the other banks
// Page table entry: next page of the note, 0x0001 = last page, 0x0003 = free. Entry 0 holds
// the table checksum in its low byte: the byte sum of the entries of the bank's data pages.
#include <k64/pak.h>
#include <k64/cpu.h>

#define ID_SIZE    32
#define END_PAGE   0x0001
#define FREE_PAGE  0x0003
#define STATUS_USED 0x02
#define BANK_SELECT 0x8000

// Big-endian loads and stores (the pak's byte order).
static u16 be16(const u8 *p) { return (u16)(p[0] << 8 | p[1]); }
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

// Page layout helpers: page table and backup of each bank, note table, first data page.
static u32 table_page(const k64_cpak_fs_t *fs, u32 bank) { return 1 + bank; }
static u32 backup_page(const k64_cpak_fs_t *fs, u32 bank) { return 1 + fs->banks + bank; }
static u32 dir_page(const k64_cpak_fs_t *fs) { return 1 + 2u * fs->banks; }
static u32 first_data(const k64_cpak_fs_t *fs, u32 bank) { return bank ? 1 : fs->first_page; }

// Makes `bank` current: writes its number, 32 times, to the bank-select address. Skipped if
// it is already current.
static bool select_bank(k64_cpak_fs_t *fs, u32 bank) {
  if (fs->cur_bank == bank) return true;
  u8 b[32];
  for (int i = 0; i < 32; i++) b[i] = (u8)bank;
  if (!k64_pak_write32(fs->port, BANK_SELECT, b)) return false;
  fs->cur_bank = (u8)bank;
  return true;
}

// Bytes at a page of bank 0's system area or of any bank's data.
static bool rd(k64_cpak_fs_t *fs, u32 bank, u32 off, void *dst, u32 len) {
  return select_bank(fs, bank) && k64_cpak_read(fs->port, off, dst, len);
}
static bool wr(k64_cpak_fs_t *fs, u32 bank, u32 off, const void *src, u32 len) {
  return select_bank(fs, bank) && k64_cpak_write(fs->port, off, src, len);
}

// ---- ID block ----

// Sum and complement-sum of the 14 big-endian words in bytes 0..27 of an ID block.
static void id_sums(const u8 *id, u16 *sum, u16 *inv) {
  *sum = *inv = 0;
  for (int j = 0; j < 28; j += 2) {
    u16 d = be16(id + j);
    *sum = (u16)(*sum + d);
    *inv = (u16)(*inv + (u16)~d);
  }
}

// Valid ID block: both checksums (28, 30) match, device word (0x18) bit 0 set, bank count set.
static bool id_ok(const u8 *id) {
  u16 s, i;
  id_sums(id, &s, &i);
  return be16(id + 28) == s && be16(id + 30) == i && (be16(id + 0x18) & 1) && id[0x1A];
}

// Blocks (32 bytes each, in page 0) holding the four ID block copies.
static const u8 id_blocks[4] = { 1, 3, 4, 6 };

// ---- page tables ----

// Byte sum of the bank's data-page entries; stored in the low byte of entry 0 by write_table().
static u8 table_sum(const k64_cpak_fs_t *fs, u32 bank) {
  u32 s = 0;
  for (u32 i = first_data(fs, bank) * 2; i < 256; i++) s += fs->table[bank][i];
  return (u8)s;
}

// Page table entry of {bank, page} in the in-RAM copy. set_entry() changes the copy only;
// write_table() flushes it.
static u16 entry(const k64_cpak_fs_t *fs, u32 bank, u32 page) { return be16(&fs->table[bank][page * 2]); }
static void set_entry(k64_cpak_fs_t *fs, u32 bank, u32 page, u16 v) { put16(&fs->table[bank][page * 2], v); }

// A data page {bank, page}?
static bool data_page(const k64_cpak_fs_t *fs, u16 e) {
  u32 bank = e >> 8, page = e & 0xFF;
  return bank < fs->banks && page < 128 && page >= first_data(fs, bank);
}

// Main copy first, then the backup: a reset between the two leaves one of them whole.
static bool write_table(k64_cpak_fs_t *fs, u32 bank) {
  fs->table[bank][0] = 0;
  fs->table[bank][1] = table_sum(fs, bank);
  return wr(fs, 0, table_page(fs, bank) * K64_CPAK_PAGE, fs->table[bank], 256) &&
         wr(fs, 0, backup_page(fs, bank) * K64_CPAK_PAGE, fs->table[bank], 256);
}

// Writes note table entry i (32 bytes) to bank 0.
static bool write_dir(k64_cpak_fs_t *fs, int i) {
  return wr(fs, 0, dir_page(fs) * K64_CPAK_PAGE + (u32)i * 32, fs->dir[i], 32);
}

// Note entry (32 bytes): game code 0..3, company code 4..5, first page 6..7, status 8,
// extension 0x0C..0x0F, name 0x10..0x1F. Slot is used when game and company codes are nonzero.
static bool used(const k64_cpak_fs_t *fs, int i) {
  return be32(fs->dir[i]) != 0 && be16(fs->dir[i] + 4) != 0;
}

// ---- names: the pak font ----

// Character to pak font code, or -1. Pak font: space 0x0F, digits 0x10..0x19, A-Z 0x1A..0x33,
// punctuation 0x34..0x41. Lower case is folded to upper case.
static int enc(char c) {
  static const char punct[] = "!\"#'*+,-./:=?@";
  if (c == ' ') return 0x0F;
  if (c >= '0' && c <= '9') return 0x10 + (c - '0');
  if (c >= 'a' && c <= 'z') c = (char)(c - 32);
  if (c >= 'A' && c <= 'Z') return 0x1A + (c - 'A');
  for (int i = 0; punct[i]; i++) if (punct[i] == c) return 0x34 + i;
  return -1;
}

// Pak font code to character (inverse of enc()); unknown codes give '~'.
static char dec(u8 v) {
  static const char punct[] = "!\"#'*+,-./:=?@";
  if (v == 0x0F) return ' ';
  if (v >= 0x10 && v < 0x1A) return (char)('0' + v - 0x10);
  if (v >= 0x1A && v < 0x34) return (char)('A' + v - 0x1A);
  if (v >= 0x34 && v < 0x42) return punct[v - 0x34];
  return '~';
}

// Encodes s into n font bytes, zero-padded; NULL is empty. False if s is too long or has
// a character the font lacks.
static bool encode(const char *s, u8 *out, u32 n) {
  u32 i = 0;
  for (; s && s[i]; i++) {
    int c = enc(s[i]);
    if (i >= n || c < 0) return false;
    out[i] = (u8)c;
  }
  for (; i < n; i++) out[i] = 0;
  return true;
}

// ---- mount / check ----

// Pages in note i's chain, or -1 if it is broken: a page out of range, free, or (with
// `seen`, a bitmap the chain's pages are added to) already owned.
static int walk(const k64_cpak_fs_t *fs, int i, u8 *seen) {
  u16 p = be16(fs->dir[i] + 6);
  int n = 0;
  for (;;) {
    if (!data_page(fs, p) || n >= K64_CPAK_MAX_BANKS * 128) return -1;
    u32 k = (p >> 8) * 128 + (p & 0xFF);
    if (seen && (seen[k >> 3] >> (k & 7) & 1)) return -1;
    if (seen) seen[k >> 3] |= (u8)(1 << (k & 7));
    u16 next = entry(fs, p >> 8, p & 0xFF);
    n++;
    if (next == END_PAGE) return n;
    if (next == FREE_PAGE) return -1;
    p = next;
  }
}

// Repairs what it can (damaged ID copies, page tables from their backup, broken notes, orphaned
// pages) and writes the fixes back. fs->fixed reports whether anything was repaired.
k64_cpak_err_t k64_cpak_mount(k64_cpak_fs_t *fs, int port) {
  fs->port = port;
  fs->cur_bank = 0xFF;
  fs->fixed = 0;
  fs->banks = 1;
  int good = -1;
  for (int c = 0; c < 4 && good < 0; c++) {
    if (!rd(fs, 0, id_blocks[c] * 32u, fs->id, ID_SIZE)) return K64_CPAK_ERR_IO;
    if (id_ok(fs->id)) good = c;
  }
  if (good < 0 || fs->id[0x1A] > K64_CPAK_MAX_BANKS) return K64_CPAK_ERR_NOFS;
  for (int c = 0; c < good; c++) {               // damaged copies before the good one
    if (!wr(fs, 0, id_blocks[c] * 32u, fs->id, ID_SIZE)) return K64_CPAK_ERR_IO;
    fs->fixed = 1;
  }
  fs->banks = fs->id[0x1A];
  // Page 0 ID, pages 1..2B tables and backups, 2 note-table pages, then data.
  fs->first_page = (u8)(3 + 2 * fs->banks);

  for (u32 b = 0; b < fs->banks; b++) {
    if (!rd(fs, 0, table_page(fs, b) * K64_CPAK_PAGE, fs->table[b], 256)) return K64_CPAK_ERR_IO;
    if (fs->table[b][1] == table_sum(fs, b)) continue;
    if (!rd(fs, 0, backup_page(fs, b) * K64_CPAK_PAGE, fs->table[b], 256)) return K64_CPAK_ERR_IO;
    if (fs->table[b][1] != table_sum(fs, b)) return K64_CPAK_ERR_CORRUPT;
    if (!wr(fs, 0, table_page(fs, b) * K64_CPAK_PAGE, fs->table[b], 256)) return K64_CPAK_ERR_IO;
    fs->fixed = 1;
  }
  if (!rd(fs, 0, dir_page(fs) * K64_CPAK_PAGE, fs->dir, sizeof fs->dir)) return K64_CPAK_ERR_IO;

  // Notes with a broken chain are dropped; used pages no note owns are freed.
  u8 seen[K64_CPAK_MAX_BANKS * 16] = { 0 };
  for (int i = 0; i < K64_CPAK_NOTES; i++) {
    if (!used(fs, i)) continue;
    // Trial copy: a note whose chain fails the walk must not claim its pages in `seen`.
    u8 try[sizeof seen];
    for (u32 k = 0; k < sizeof seen; k++) try[k] = seen[k];
    if (walk(fs, i, try) > 0) {
      for (u32 k = 0; k < sizeof seen; k++) seen[k] = try[k];
      continue;
    }
    for (int k = 0; k < 32; k++) fs->dir[i][k] = 0;
    if (!write_dir(fs, i)) return K64_CPAK_ERR_IO;
    fs->fixed = 1;
  }
  for (u32 b = 0; b < fs->banks; b++) {
    bool dirty = false;
    for (u32 p = first_data(fs, b); p < 128; p++) {
      u32 k = b * 128 + p;
      if (!(seen[k >> 3] >> (k & 7) & 1) && entry(fs, b, p) != FREE_PAGE) {
        set_entry(fs, b, p, FREE_PAGE);
        dirty = true;
      }
    }
    if (dirty) {
      if (!write_table(fs, b)) return K64_CPAK_ERR_IO;
      fs->fixed = 1;
    }
  }
  return K64_CPAK_OK;
}

k64_cpak_err_t k64_cpak_format(int port) {
  k64_cpak_fs_t fs_, *fs = &fs_;
  fs->port = port;
  fs->cur_bank = 0xFF;

  // Bank count: a marker in block 0 of each bank; a bank that is not there aliases bank 0
  // (or does not keep the marker). Block 0 is the unused label area.
  u8 mark[32], back[32];
  u32 banks = 1;
  for (int i = 0; i < 32; i++) mark[i] = 0x80;
  if (!wr(fs, 0, 0, mark, 32)) return K64_CPAK_ERR_IO;
  for (; banks < K64_CPAK_MAX_BANKS; banks++) {
    for (int i = 0; i < 32; i++) mark[i] = (u8)(0x80 | banks);
    if (!wr(fs, banks, 0, mark, 32) || !rd(fs, banks, 0, back, 32)) return K64_CPAK_ERR_IO;
    bool kept = true;
    for (int i = 0; i < 32; i++) kept = kept && back[i] == mark[i];
    // Bank 0's marker is still 0x80 only if the bank did not alias onto bank 0.
    if (!rd(fs, 0, 0, back, 32)) return K64_CPAK_ERR_IO;
    if (!kept || back[0] != 0x80) break;
  }

  u8 *id = fs->id;
  // ID bytes from an LCG seeded with CP0 Count. Not random; only needs to differ per format.
  u32 seed = k64_count();
  for (int i = 0; i < 0x18; i++) {
    seed = seed * 1103515245u + 12345u;
    id[i] = (u8)(seed >> 16);
  }
  for (int i = 0; i < 4; i++) id[i] = 0;              // never repaired
  put16(id + 0x18, 1);                                // device: Controller Pak
  id[0x1A] = (u8)banks;
  id[0x1B] = 0;
  u16 s, inv;
  id_sums(id, &s, &inv);
  put16(id + 28, s);
  put16(id + 30, inv);
  // Four copies, so mount can recover from any one that survives.
  for (int c = 0; c < 4; c++)
    if (!wr(fs, 0, id_blocks[c] * 32u, id, ID_SIZE)) return K64_CPAK_ERR_IO;

  fs->banks = (u8)banks;
  fs->first_page = (u8)(3 + 2 * banks);
  // Empty note table.
  for (u32 i = 0; i < sizeof fs->dir; i++) fs->dir[0][i] = 0;
  if (!wr(fs, 0, dir_page(fs) * K64_CPAK_PAGE, fs->dir, sizeof fs->dir)) return K64_CPAK_ERR_IO;
  for (u32 b = 0; b < banks; b++) {
    // Reserved pages (bank 0 system area, page 0 of other banks) get entry 0; the rest are free.
    for (u32 p = 0; p < 128; p++) set_entry(fs, b, p, p < first_data(fs, b) ? 0 : FREE_PAGE);
    if (!write_table(fs, b)) return K64_CPAK_ERR_IO;
  }
  return K64_CPAK_OK;
}

// ---- notes ----

// Free data pages over all banks, from the in-RAM page tables.
u32 k64_cpak_free_pages(const k64_cpak_fs_t *fs) {
  u32 n = 0;
  for (u32 b = 0; b < fs->banks; b++)
    for (u32 p = first_data(fs, b); p < 128; p++) n += entry(fs, b, p) == FREE_PAGE;
  return n;
}

bool k64_cpak_note(const k64_cpak_fs_t *fs, int i, k64_cpak_note_t *out) {
  if (i < 0 || i >= K64_CPAK_NOTES || !used(fs, i)) return false;
  const u8 *d = fs->dir[i];
  out->game_code = be32(d);
  out->company_code = be16(d + 4);
  u32 n = 0;
  while (n < 16 && d[0x10 + n]) { out->name[n] = dec(d[0x10 + n]); n++; }
  out->name[n] = 0;
  n = 0;
  while (n < 4 && d[0x0C + n]) { out->ext[n] = dec(d[0x0C + n]); n++; }
  out->ext[n] = 0;
  out->pages = (u32)walk(fs, i, NULL);
  return true;
}

int k64_cpak_find(const k64_cpak_fs_t *fs, u32 game, u16 company, const char *name, const char *ext) {
  u8 n[16], e[4];
  if (!encode(name, n, 16) || !encode(ext, e, 4)) return -1;
  for (int i = 0; i < K64_CPAK_NOTES; i++) {
    const u8 *d = fs->dir[i];
    if (!used(fs, i) || be32(d) != game || be16(d + 4) != company) continue;
    bool same = true;
    for (int k = 0; k < 16; k++) same = same && d[0x10 + k] == n[k];
    for (int k = 0; k < 4; k++) same = same && d[0x0C + k] == e[k];
    if (same) return i;
  }
  return -1;
}

k64_cpak_err_t k64_cpak_create(k64_cpak_fs_t *fs, u32 game, u16 company, const char *name,
                               const char *ext, u32 bytes, int *index) {
  u8 d[32] = { 0 };
  if (!game || !company || !encode(name, d + 0x10, 16) || !encode(ext, d + 0x0C, 4)) return K64_CPAK_ERR_NAME;
  if (k64_cpak_find(fs, game, company, name, ext) >= 0) return K64_CPAK_ERR_EXISTS;
  int slot = -1;
  for (int i = 0; i < K64_CPAK_NOTES && slot < 0; i++) if (!used(fs, i)) slot = i;
  if (slot < 0) return K64_CPAK_ERR_DIRFULL;
  u32 pages = (bytes + K64_CPAK_PAGE - 1) / K64_CPAK_PAGE;
  if (!pages) pages = 1;
  if (pages > k64_cpak_free_pages(fs)) return K64_CPAK_ERR_FULL;

  // Chain the first free pages, last to first, so each entry knows its successor.
  // The lowest free pages, linked last to first so each entry gets its successor.
  u16 next = END_PAGE;
  bool dirty[K64_CPAK_MAX_BANKS] = { false };
  u16 take[K64_CPAK_MAX_BANKS * 128];
  u32 n = 0;
  for (u32 b = 0; b < fs->banks && n < pages; b++)
    for (u32 p = first_data(fs, b); p < 128 && n < pages; p++)
      if (entry(fs, b, p) == FREE_PAGE) take[n++] = (u16)(b << 8 | p);
  for (u32 k = pages; k-- > 0;) {
    set_entry(fs, take[k] >> 8, take[k] & 0xFF, next);
    dirty[take[k] >> 8] = true;
    next = take[k];
  }
  for (u32 b = 0; b < fs->banks; b++)
    if (dirty[b] && !write_table(fs, b)) return K64_CPAK_ERR_IO;

  // The note entry last: a reset before it only leaves pages that the next mount frees.
  // Note entry: game code, company code, first page of the chain (next), status.
  d[0] = (u8)(game >> 24); d[1] = (u8)(game >> 16); d[2] = (u8)(game >> 8); d[3] = (u8)game;
  put16(d + 4, company);
  put16(d + 6, next);
  d[8] = STATUS_USED;
  for (int k = 0; k < 32; k++) fs->dir[slot][k] = d[k];
  if (!write_dir(fs, slot)) return K64_CPAK_ERR_IO;
  if (index) *index = slot;
  return K64_CPAK_OK;
}

k64_cpak_err_t k64_cpak_delete(k64_cpak_fs_t *fs, int i) {
  if (i < 0 || i >= K64_CPAK_NOTES || !used(fs, i)) return K64_CPAK_ERR_NOTFOUND;
  u16 p = be16(fs->dir[i] + 6);
  // The note entry first: a reset after it only leaves pages that the next mount frees.
  for (int k = 0; k < 32; k++) fs->dir[i][k] = 0;
  if (!write_dir(fs, i)) return K64_CPAK_ERR_IO;
  bool dirty[K64_CPAK_MAX_BANKS] = { false };
  for (u32 guard = 0; data_page(fs, p) && guard < K64_CPAK_MAX_BANKS * 128u; guard++) {
    u16 next = entry(fs, p >> 8, p & 0xFF);
    set_entry(fs, p >> 8, p & 0xFF, FREE_PAGE);
    dirty[p >> 8] = true;
    if (next == END_PAGE) break;
    p = next;
  }
  for (u32 b = 0; b < fs->banks; b++)
    if (dirty[b] && !write_table(fs, b)) return K64_CPAK_ERR_IO;
  return K64_CPAK_OK;
}

// Reads or writes note bytes [off, off+len) across the note's page chain.
static k64_cpak_err_t note_rw(k64_cpak_fs_t *fs, int i, u32 off, u8 *buf, u32 len, bool write) {
  if (i < 0 || i >= K64_CPAK_NOTES || !used(fs, i)) return K64_CPAK_ERR_NOTFOUND;
  // The range must lie inside the chain; walk() gives its page count, or -1 if it is broken.
  int pages = walk(fs, i, NULL);
  if (pages < 0 || off > (u32)pages * K64_CPAK_PAGE || len > (u32)pages * K64_CPAK_PAGE - off)
    return K64_CPAK_ERR_RANGE;
  u16 p = be16(fs->dir[i] + 6);
  for (u32 skip = off / K64_CPAK_PAGE; skip; skip--) {   // to the page holding `off`
    u16 next = data_page(fs, p) ? entry(fs, p >> 8, p & 0xFF) : END_PAGE;
    if (next == END_PAGE) return K64_CPAK_ERR_RANGE;
    p = next;
  }
  off %= K64_CPAK_PAGE;
  while (len) {
    if (!data_page(fs, p)) return K64_CPAK_ERR_RANGE;
    u32 n = K64_CPAK_PAGE - off < len ? K64_CPAK_PAGE - off : len;
    u32 a = (p & 0xFF) * K64_CPAK_PAGE + off;
    bool ok = write ? wr(fs, p >> 8, a, buf, n) : rd(fs, p >> 8, a, buf, n);
    if (!ok) return K64_CPAK_ERR_IO;
    buf += n; len -= n; off = 0;
    if (len) {
      u16 next = entry(fs, p >> 8, p & 0xFF);
      if (next == END_PAGE) return K64_CPAK_ERR_RANGE;
      p = next;
    }
  }
  return K64_CPAK_OK;
}

k64_cpak_err_t k64_cpak_note_read(k64_cpak_fs_t *fs, int i, u32 off, void *dst, u32 len) {
  return note_rw(fs, i, off, dst, len, false);
}

k64_cpak_err_t k64_cpak_note_write(k64_cpak_fs_t *fs, int i, u32 off, const void *src, u32 len) {
  return note_rw(fs, i, off, (u8 *)src, len, true);
}
