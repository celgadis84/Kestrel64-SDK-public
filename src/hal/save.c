// kestrel64-sdk — cartridge save memory (EEPROM over joybus, SRAM and FlashRAM over PI
// domain 2).
#include <k64/save.h>
#include <k64/joybus.h>
#include <k64/pi.h>
#include <k64/cpu.h>
#include <k64/debug.h>
#include <k64/mem.h>

#define EEP_CHANNEL 4
#define EEP_BUSY    0x80          // identify status bit: write cycle in progress

static k64_save_type_t type;
static bool eep_busy;             // a write was issued: poll before the next command

// Bytes per k64_save_type_t value (index = enum value).
static const u32 sizes[] = { 0, 512, 2048, 32 * 1024, 128 * 1024, 128 * 1024, 96 * 1024 };

k64_save_type_t k64_save_type(void) { return type; }
u32 k64_save_size(void) { return sizes[type]; }

// ---- EEPROM ----
// Joybus identify: device ID in rx[0..1], status in rx[2]. Returns 0 when there is no reply.
static u16 eep_identify(u8 *status) {
  u8 tx = 0x00, rx[3];
  if (!k64_joybus_cmd(EEP_CHANNEL, &tx, 1, rx, 3)) return 0;
  if (status) *status = rx[2];
  return (u16)(rx[0] << 8 | rx[1]);
}

// Polls identify until the EEPROM's internal write cycle clears its busy bit.
static void eep_wait(void) {
  if (!eep_busy) return;
  u8 st;
  while (eep_identify(&st) && (st & EEP_BUSY)) {}
  eep_busy = false;
}

// One 8-byte block over the joybus (command 0x04). Waits out a pending write first.
static bool eep_read_block(u32 block, u8 out[8]) {
  eep_wait();
  u8 tx[2] = { 0x04, (u8)block };
  return k64_joybus_cmd(EEP_CHANNEL, tx, 2, out, 8);
}

// Command 0x05. The chip then runs an internal write cycle, so eep_busy makes the next access wait.
static bool eep_write_block(u32 block, const u8 in[8]) {
  eep_wait();
  u8 tx[10] = { 0x05, (u8)block }, st;
  for (int i = 0; i < 8; i++) tx[2 + i] = in[i];
  if (!k64_joybus_cmd(EEP_CHANNEL, tx, 10, &st, 1)) return false;
  eep_busy = true;
  return true;
}

// Byte-granular access over 8-byte blocks.
static bool eep_read(u32 off, u8 *dst, u32 len) {
  while (len) {
    u8 b[8];
    u32 o = off & 7, n = 8 - o < len ? 8 - o : len;
    if (!eep_read_block(off >> 3, b)) return false;
    for (u32 i = 0; i < n; i++) dst[i] = b[o + i];
    off += n; dst += n; len -= n;
  }
  return true;
}

// Partial blocks are read-modify-written; a block is only written when a byte in it changed.
static bool eep_write(u32 off, const u8 *src, u32 len) {
  while (len) {
    u8 b[8];
    u32 o = off & 7, n = 8 - o < len ? 8 - o : len;
    if (!eep_read_block(off >> 3, b)) return false;
    bool same = true;
    for (u32 i = 0; i < n; i++) { same &= b[o + i] == src[i]; b[o + i] = src[i]; }
    if (!same && !eep_write_block(off >> 3, b)) return false;
    off += n; src += n; len -= n;
  }
  return true;
}

// ---- SRAM ----
// DMA through an aligned bounce buffer: any caller alignment / length.
static u8 bounce[512] __attribute__((aligned(16)));

// RDRAM -> cart by PI DMA (PI_RD_LEN is the RDRAM-to-cart direction), from the bounce buffer.
static void sram_dma_to_cart(u32 addr, u32 len) {
  k64_dcache_writeback(bounce, len);
  k64_pi_wait();
  PI_DRAM_ADDR = K64_PHYS(bounce);
  PI_CART_ADDR = addr;
  PI_RD_LEN = len - 1;
  k64_pi_wait();
  PI_STATUS = PI_WR_CLR_INTR;
}

// Cart address of save offset `off` and the bytes left in its bank. The 768K SRAM is three
// 32 KiB chips selected by address bits 18-19 (0x08000000, 0x08040000, 0x08080000).
static u32 sram_addr(u32 off, u32 *room) {
  if (type != K64_SAVE_SRAM768K) { *room = 0xFFFFFFFFu; return PI_DOM2_ADDR2 + off; }
  *room = 0x8000 - (off & 0x7FFF);
  return PI_DOM2_ADDR2 + ((off >> 15) << 18) + (off & 0x7FFF);
}

// Reads through the bounce buffer, one bank-limited chunk per DMA.
static bool sram_read(u32 off, u8 *dst, u32 len) {
  while (len) {
    u32 room, cart = sram_addr(off, &room);
    u32 a = cart & ~1u, lead = cart - a;       // the PI wants an even cart address
    u32 n = len < sizeof bounce - 2 ? len : sizeof bounce - 2;
    if (n > room) n = room;
    k64_pi_read(bounce, a, (lead + n + 1) & ~1u);
    for (u32 i = 0; i < n; i++) dst[i] = bounce[lead + i];
    off += n; dst += n; len -= n;
  }
  return true;
}

// Writes through the bounce buffer, one bank-limited chunk per DMA.
static bool sram_write(u32 off, const u8 *src, u32 len) {
  while (len) {
    u32 room, cart = sram_addr(off, &room);
    u32 a = cart & ~1u, lead = cart - a;
    u32 n = len < sizeof bounce - 2 ? len : sizeof bounce - 2;
    if (n > room) n = room;
    u32 dl = (lead + n + 1) & ~1u;
    if (lead || dl != n) {                     // odd edges: keep the neighbouring bytes
      k64_pi_read(bounce, a, dl);
    }
    for (u32 i = 0; i < n; i++) bounce[lead + i] = src[i];
    sram_dma_to_cart(a, dl);
    off += n; src += n; len -= n;
  }
  return true;
}

// ---- FlashRAM ----
// 1024 pages of 128 bytes, erased in 16 KiB sectors (128 pages). Commands go to the
// register at 0x08010000; reads at 0x08000000 return status or array data depending on the
// mode. Protocol and chip quirks follow libultra (osFlash*).
#define FL_CMD        (0xA0000000u | (PI_DOM2_ADDR2 + 0x10000))
#define FL_DATA       (0xA0000000u | PI_DOM2_ADDR2)
#define FL_PAGE       128u
#define FL_SECTOR     (128u * FL_PAGE)
#define FL_ERASE_SECT 0x4B000000u
#define FL_ERASE_EXEC 0x78000000u
#define FL_PROGRAM    0xA5000000u
#define FL_LOAD_PAGE  0xB4000000u
#define FL_STATUS     0xD2000000u
#define FL_ID         0xE1000000u
#define FL_READ       0xF0000000u

static bool fl_old;               // early Macronix chips: array address = page * 64
static u8 *fl_sector;             // 16 KiB scratch for a sector rewrite (allocated on first use)
static u8 fl_page[FL_PAGE] __attribute__((aligned(16)));

// Waits for PI to go idle, then writes one command word to the FlashRAM command register.
static void fl_cmd(u32 c) { k64_pi_wait(); *(volatile u32 *)FL_CMD = c; }
// One word from the data port: status or array data, depending on the last command.
static u32 fl_read_io(void) { k64_pi_wait(); return *(volatile u32 *)FL_DATA; }
// Resets the status register: status command, then a zero write to the data port.
static void fl_clear_status(void) { fl_cmd(FL_STATUS); k64_pi_wait(); *(volatile u32 *)FL_DATA = 0; }
// Cart address of a page; old Macronix chips address 64-byte halves (page * 64).
static u32 fl_addr(u32 page) { return PI_DOM2_ADDR2 + page * (fl_old ? FL_PAGE / 2 : FL_PAGE); }

// Reads whole pages; a DMA never crosses a 256-page boundary (as libultra).
static void fl_read_pages(u32 page, u8 *dst, u32 n) {
  fl_cmd(FL_READ);
  (void)fl_read_io();
  while (n) {
    u32 k = 256 - (page & 255);
    if (k > n) k = n;
    k64_pi_read(dst, fl_addr(page), k * FL_PAGE);
    page += k; dst += k * FL_PAGE; n -= k;
  }
}

// Loads one page into the chip buffer by PI DMA, then programs it. Polls until the busy bit clears.
static bool fl_program(u32 page, const u8 *src) {
  fl_cmd(FL_LOAD_PAGE);
  k64_dcache_writeback(src, FL_PAGE);
  k64_pi_wait();
  PI_DRAM_ADDR = K64_PHYS(src);
  PI_CART_ADDR = PI_DOM2_ADDR2;
  PI_RD_LEN = FL_PAGE - 1;
  k64_pi_wait();
  PI_STATUS = PI_WR_CLR_INTR;
  if (!fl_old) fl_cmd(FL_LOAD_PAGE);
  fl_cmd(FL_PROGRAM | page);
  u32 st;
  while ((st = fl_read_io()) & 1) {}
  fl_clear_status();
  return (st & 4) != 0;
}

// Erases the 16 KiB sector that holds page, then polls until the busy bit clears.
static bool fl_erase(u32 page) {
  fl_cmd(FL_ERASE_SECT | page);
  fl_cmd(FL_ERASE_EXEC);
  u32 st;
  while ((st = fl_read_io()) & 2) {}
  fl_clear_status();
  return (st & 8) != 0;
}

// Reads through the bounce buffer, whole pages at a time.
static bool fl_read(u32 off, u8 *dst, u32 len) {
  while (len) {
    u32 page = off / FL_PAGE, o = off % FL_PAGE;
    u32 pages = (o + len + FL_PAGE - 1) / FL_PAGE;
    if (pages > sizeof bounce / FL_PAGE) pages = sizeof bounce / FL_PAGE;
    u32 n = pages * FL_PAGE - o;
    if (n > len) n = len;
    fl_read_pages(page, bounce, pages);
    for (u32 i = 0; i < n; i++) dst[i] = bounce[o + i];
    off += n; dst += n; len -= n;
  }
  return true;
}

// Merges src[so..so+n) (sector-relative) into page p of the sector held in pg. Returns
// whether the page changes and, through *erase, whether a changed byte is not erased.
static bool fl_merge(u8 *pg, u32 p, u32 so, u32 n, const u8 *src, bool *erase) {
  bool dirty = false;
  for (u32 i = 0; i < FL_PAGE; i++) {
    u32 x = p * FL_PAGE + i;
    if (x < so || x >= so + n) continue;
    u8 want = src[x - so];
    if (pg[i] != want) { dirty = true; if (pg[i] != 0xFF) *erase = true; }
    pg[i] = want;
  }
  return dirty;
}

// Per touched sector: unchanged pages are left alone; changes that land on still-erased
// bytes are programmed in place, anything else reads, erases and reprograms the sector.
static bool fl_write(u32 off, const u8 *src, u32 len) {
  while (len) {
    u32 sect = off / FL_SECTOR, so = off % FL_SECTOR, base = sect * 128;
    u32 n = FL_SECTOR - so < len ? FL_SECTOR - so : len;
    u32 first = so / FL_PAGE, last = (so + n - 1) / FL_PAGE;
    bool erase = false;
    for (u32 p = first; p <= last && !erase; p++) {
      fl_read_pages(base + p, fl_page, 1);
      if (fl_merge(fl_page, p, so, n, src, &erase) && !erase && !fl_program(base + p, fl_page))
        return false;
    }
    if (erase) {
      if (!fl_sector) fl_sector = k64_mem_alloc(K64_ZONE_HEAP, FL_SECTOR, 16);
      if (!fl_sector) return false;
      // Slow path: keep a copy of the whole sector, patch it, erase, then reprogram the
      // pages that are not blank.
      fl_read_pages(base, fl_sector, 128);
      for (u32 i = 0; i < n; i++) fl_sector[so + i] = src[i];
      if (!fl_erase(base)) return false;
      for (u32 p = 0; p < 128; p++) {
        const u8 *pg = fl_sector + p * FL_PAGE;
        bool blank = true;
        for (u32 i = 0; blank && i < FL_PAGE; i++) blank = pg[i] == 0xFF;
        if (!blank && !fl_program(base + p, pg)) return false;
      }
    }
    off += n; src += n; len -= n;
  }
  return true;
}

static bool fl_detect(void) {
  // Domain 2 timing of the FlashRAM carts (libultra's flash handle: 5 / 12 / 15 / 2).
  PI_BSD_DOM2_LAT = 0x05; PI_BSD_DOM2_PWD = 0x0C; PI_BSD_DOM2_PGS = 0x0F; PI_BSD_DOM2_RLS = 0x02;
  fl_cmd(FL_STATUS); (void)fl_read_io();
  fl_cmd(FL_ID);
  static u32 id[2] __attribute__((aligned(16)));
  k64_pi_read(id, PI_DOM2_ADDR2, 8);
  u32 maker = id[1];
  fl_old = maker == 0x00C20000 || maker == 0x00C20001 || maker == 0x00C2001E;
  return fl_old || maker == 0x00C2001D || maker == 0x003200F1;
}

// ---- API ----
k64_save_type_t k64_save_init(void) {
  static u8 hdr[8] __attribute__((aligned(16)));
  // ROM header bytes 0x38-0x3F: the 'E','D' marker sits at 0x3C-0x3D, the save type in the high
  // nibble of 0x3F.
  k64_pi_read(hdr, 0x10000038, 8);
  int want = -1;                               // from the header, -1 = not declared
  if (hdr[4] == 'E' && hdr[5] == 'D') {
    // Save type codes: 1 EEPROM 4K, 2 EEPROM 16K, 3 SRAM 256K, 4 SRAM 768K, 5 FlashRAM 1M,
    // 6 SRAM 1M.
    switch (hdr[7] >> 4) {
      case 1: want = K64_SAVE_EEPROM4K; break;
      case 2: want = K64_SAVE_EEPROM16K; break;
      case 3: want = K64_SAVE_SRAM256K; break;
      case 4: want = K64_SAVE_SRAM768K; break;
      case 5: want = K64_SAVE_FLASH1M; break;
      case 6: want = K64_SAVE_SRAM1M; break;
      default: want = K64_SAVE_NONE; break;    // none, or a device not supported yet
    }
  }
  type = K64_SAVE_NONE;
  if (want == K64_SAVE_SRAM256K || want == K64_SAVE_SRAM768K || want == K64_SAVE_SRAM1M) {
    // Domain 2 timing of the SRAM carts (libultra's SRAM handle: 5 / 12 / 13 / 2).
    PI_BSD_DOM2_LAT = 0x05; PI_BSD_DOM2_PWD = 0x0C; PI_BSD_DOM2_PGS = 0x0D; PI_BSD_DOM2_RLS = 0x02;
    type = (k64_save_type_t)want;
  } else if (want == K64_SAVE_FLASH1M) {
    if (fl_detect()) type = K64_SAVE_FLASH1M;
  } else if (want == -1 || want == K64_SAVE_EEPROM4K || want == K64_SAVE_EEPROM16K) {
    // Undeclared or EEPROM: probe the joybus EEPROM (ID 0x0080 = 4K, 0x00C0 = 16K).
    u16 id = eep_identify(NULL);
    if (id == 0x0080) type = K64_SAVE_EEPROM4K;
    else if (id == 0x00C0) type = K64_SAVE_EEPROM16K;
  }
  return type;
}

bool k64_save_read(u32 offset, void *dst, u32 len) {
  if (offset > k64_save_size() || len > k64_save_size() - offset) return false;
  if (!len) return true;
  if (type == K64_SAVE_EEPROM4K || type == K64_SAVE_EEPROM16K) return eep_read(offset, dst, len);
  if (type == K64_SAVE_FLASH1M) return fl_read(offset, dst, len);
  return sram_read(offset, dst, len);
}

bool k64_save_write(u32 offset, const void *src, u32 len) {
  if (offset > k64_save_size() || len > k64_save_size() - offset) return false;
  if (!len) return true;
  if (type == K64_SAVE_EEPROM4K || type == K64_SAVE_EEPROM16K) return eep_write(offset, src, len);
  if (type == K64_SAVE_FLASH1M) return fl_write(offset, src, len);
  return sram_write(offset, src, len);
}
