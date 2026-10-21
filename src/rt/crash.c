// kestrel64-sdk — crash screen: what k64_panic and an unhandled CPU exception show.
//
// One screen, drawn straight into the scanned framebuffer through KSEG1:
//   the message, PC with its function name, a backtrace, the app's own lines
//   (k64_crash_set_hook: a port's guest state), RSP/RDP registers, the CPU registers.
// Function names come from the symbol table k64rom.py appends to the ROM (--elf); its ROM
// offset is in header word 0x34. Without it the screen shows bare addresses.
// The backtrace reads each frame's prologue (addiu sp,sp,-N / sw ra,off(sp)) from the
// function start the symbol table gives, or the previous `jr ra` without one.
#include <k64/debug.h>
#include <k64/cpu.h>
#include <k64/vi.h>

extern const u8 k64_font8x8[96][8];
extern const u8 __text_start[], __text_end[];

// ---- symbols (ROM, read through the PI) -------------------------------------------------
#define SYM_MAGIC 0x4B363453u   // "K64S"
// Table at sym_base: magic, count, then count entries of {addr, size, name offset} (12 bytes,
// sorted by addr), names NUL-terminated at sym_base + name offset.
static u32 sym_base, sym_n;

// One ROM word at off through the PI's direct (uncached KSEG1) window; the PI must be idle, as a
// direct read during a DMA returns garbage.
static u32 rom_rd(u32 off) {
  while (PI_STATUS & (PI_ST_DMA_BUSY | PI_ST_IO_BUSY)) {}
  return K64_REG(0xB0000000u + off);
}

// Finds the table from header word 0x34; any implausible offset, magic or count leaves it off.
static void sym_init(void) {
  u32 o = rom_rd(0x34);
  if ((o & 3) || o < 0x1000 || o >= 0x4000000 || rom_rd(o) != SYM_MAGIC) return;
  sym_base = o;
  sym_n = rom_rd(o + 4);
  if (sym_n > 0x40000) sym_n = 0;
}

// Function holding pc: its start (0 if none) and, if name, its name.
static u32 sym_find(u32 pc, char *name, u32 size) {
  if (name) name[0] = 0;
  if (!sym_n) return 0;
  u32 lo = 0, hi = sym_n;
  while (hi - lo > 1) {                 // last entry with addr <= pc
    u32 m = (lo + hi) / 2;
    if (rom_rd(sym_base + 8 + m * 12) <= pc) lo = m; else hi = m;
  }
  u32 e = sym_base + 8 + lo * 12, a = rom_rd(e), sz = rom_rd(e + 4);
  if (a > pc || (sz && pc >= a + sz)) return 0;
  if (name) {
    u32 s = sym_base + rom_rd(e + 8), i = 0;
    for (; i + 1 < size; i++) {
      u32 b = s + i;
      char c = (char)(rom_rd(b & ~3u) >> (24 - 8 * (b & 3)));   // PI reads are words, big-endian
      if (!c) break;
      name[i] = c;
    }
    name[i] = 0;
  }
  return a;
}

// "name+0x1c", or "?" when the table has nothing for pc.
static void sym_fmt(u32 pc, char *out, u32 size) {
  char name[48];
  u32 a = sym_find(pc, name, sizeof name);
  if (a) k64_snprintf(out, size, "%s+0x%x", name, pc - a);
  else k64_snprintf(out, size, "?");
}

// ---- backtrace --------------------------------------------------------------------------
// Plausibility checks for walked values: word-aligned code inside the image, stack in RDRAM.
static bool in_text(u32 a) { return !(a & 3) && a >= (u32)__text_start && a < (u32)__text_end; }
static bool in_ram(u32 a) { return !(a & 3) && a >= 0x80000000u && a < 0x80800000u; }

// pc/sp/ra of the faulting point; out gets frame pcs (the first is pc, the others call sites).
static int backtrace(u32 pc, u32 sp, u32 ra, u32 *out, int max) {
  int n = 0;
  bool first = true;
  if (!in_text(pc) && max > 0) {      // jumped into the weeds: the caller is in ra
    out[n++] = pc;
    pc = ra - 8;
  }
  while (n < max && in_text(pc) && in_ram(sp)) {
    out[n++] = pc;
    u32 start = sym_find(pc, 0, 0);
    if (!start) {                     // no symbols: back to the previous function's jr ra
      start = pc;
      for (u32 i = 0; i < 8192 && in_text(start - 8); i++, start -= 4)
        if (*(const u32 *)(start - 8) == 0x03E00008u) break;
    }
    s32 frame = 0, raoff = -1;
    for (u32 a = start; a < pc && a - start < 0x4000; a += 4) {
      u32 w = *(const u32 *)a;
      u32 hi = w >> 16;
      if ((hi == 0x27BD || hi == 0x67BD) && (s16)w < 0) frame = -(s16)w;   // (d)addiu sp,sp,-N
      else if (hi == 0xAFBF) raoff = (s16)w;                               // sw ra,off(sp)
      else if (hi == 0xFFBF) raoff = (s16)w + 4;                           // sd ra: low word
    }
    u32 next;
    if (raoff >= 0 && in_ram(sp + raoff)) next = *(const u32 *)(sp + raoff);
    else if (first && raoff < 0) next = ra;                                  // leaf, ra live
    else break;
    if (!first && frame == 0) break;
    sp += frame;
    if (!in_text(next - 8)) break;
    pc = next - 8;                                                          // the jal
    first = false;
  }
  return n;
}

// ---- drawing ----------------------------------------------------------------------------
static const k64_surface_t *fb;
static u32 cy, cols;
static u32 col_bg, col_fg, col_hi, col_dim;

// 8x8 glyphs, written through KSEG1 so they reach RDRAM (and the VI) with no cache flush.
static void put(u32 x, u32 y, u32 color, const char *s) {
  for (; *s; s++, x += 8) {
    u8 c = (u8)*s;
    if (c < 32 || c > 127) c = '?';
    const u8 *g = k64_font8x8[c - 32];
    for (u32 r = 0; r < 8; r++) for (u32 b = 0; b < 8; b++) {
      u32 px = x + b, py = y + r;
      if (px >= fb->width || py >= fb->height) continue;
      u32 v = (g[r] >> b) & 1 ? color : col_bg;
      if (fb->bpp == K64_BPP16) ((u16 *)K64_KSEG1(fb->pixels))[py * (fb->stride / 2) + px] = (u16)v;
      else ((u32 *)K64_KSEG1(fb->pixels))[py * (fb->stride / 4) + px] = v;
    }
  }
}

// Text in lines: wraps at the screen width and at '\n'.
static void line(u32 color, const char *s) {
  char buf[96];
  while (*s) {
    u32 n = 0;
    while (s[n] && s[n] != '\n' && n < cols && n < sizeof buf - 1) n++;
    for (u32 i = 0; i < n; i++) buf[i] = s[i];
    buf[n] = 0;
    if (cy + 8 <= fb->height - 8) put(16, cy, color, buf);
    cy += 9;
    s += n;
    if (*s == '\n') s++;
  }
}

// Clears the buffer the VI shows (bringing up a 320x240 display if none exists) and picks the
// colours for its depth. False if no display could be had: the report then only goes to the log.
static bool screen_open(void) {
  fb = k64_display_front();
  if (!fb->pixels) {
    k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
    fb = k64_display_front();
    if (!fb->pixels) return false;
  }
  if (fb->bpp == K64_BPP16) { col_bg = 0x0015; col_fg = 0xFFFF; col_hi = 0xFFC1; col_dim = 0xA529; }
  else { col_bg = 0x000050FF; col_fg = 0xFFFFFFFF; col_hi = 0xFFF000FF; col_dim = 0xA0A0A0FF; }
  k64_dcache_writeback_all();           // no dirty line may land on the screen later
  u32 words = (u32)fb->stride * fb->height / 4;
  u32 fill = fb->bpp == K64_BPP16 ? col_bg << 16 | col_bg : col_bg;
  for (u32 i = 0; i < words; i++) ((u32 *)K64_KSEG1(fb->pixels))[i] = fill;
  // A port may scan its own buffer (k64_display_scan): show this one.
  if (fb->height <= 240 && fb->width <= 640)
    k64_display_scan(K64_PHYS(fb->pixels), fb->width, fb->height, (u16)(fb->stride / fb->bpp));
  cols = (fb->width - 32) / 8;
  cy = 12;
  return true;
}

// ---- report -----------------------------------------------------------------------------
static void (*hook)(char *buf, u32 size);
void k64_crash_set_hook(void (*fn)(char *buf, u32 size)) { hook = fn; }

static const char *exc_name(u32 code) {
  static const char *n[32] = { "Interrupt", "TLB modification", "TLB miss (load)", "TLB miss (store)",
    "Address error (load)", "Address error (store)", "Bus error (fetch)", "Bus error (data)",
    "Syscall", "Breakpoint", "Reserved instruction", "Coprocessor unusable", "Overflow",
    "Trap", "?", "Floating point", [23] = "Watch" };
  return n[code & 31] ? n[code & 31] : "?";
}

static __attribute__((noreturn)) void report(const char *title, const k64_exc_frame_t *f, u32 pc, u32 sp, u32 ra,
                                             int skip) {
  static int depth;
  if (depth++) for (;;) {}              // crashed while reporting
  k64_set_status(K64_C0_READ(12) & ~1u);   // IE off: no thread switch or handler from here on
  sym_init();
  char buf[160], s[64];
  u32 bt[12];
  int nbt = backtrace(pc, sp, ra, bt, K64_ARRAY_LEN(bt));

  k64_printf("\n*** CRASH: %s\n", title);
  for (int i = skip; i < nbt; i++) {
    sym_fmt(bt[i], s, sizeof s);
    k64_printf("  #%d %08x %s\n", i - skip, bt[i], s);
  }
  char extra[160];
  extra[0] = 0;
  if (hook) { hook(extra, sizeof extra); k64_printf("%s\n", extra); }

  if (!screen_open()) for (;;) {}
  line(col_hi, f ? "KESTREL64-SDK CRASH: EXCEPTION" : "KESTREL64-SDK CRASH: PANIC");
  line(col_fg, title);
  cy += 3;
  if (f) {
    sym_fmt(f->epc, s, sizeof s);
    k64_snprintf(buf, sizeof buf, "PC  %08x %s", f->epc, s); line(col_fg, buf);
    k64_snprintf(buf, sizeof buf, "BAD %08x CA %08x SR %08x", f->badvaddr, f->cause, f->status);
    line(col_dim, buf);
  }
  for (int i = skip; i < nbt && i < skip + 6; i++) {
    sym_fmt(bt[i], s, sizeof s);
    k64_snprintf(buf, sizeof buf, "#%d %08x %s", i - skip, bt[i], s);
    line(col_fg, buf);
  }
  if (extra[0]) { cy += 3; line(col_hi, extra); }
  cy += 3;
  k64_snprintf(buf, sizeof buf, "SP %04x pc %03x  MI %02x  DP %04x", (unsigned)(SP_STATUS & 0x7FFF),
               (unsigned)(SP_PC & 0xFFF), (unsigned)(MI_INTR & 0x3F), (unsigned)(DPC_STATUS & 0x7FF));
  line(col_dim, buf);
  k64_snprintf(buf, sizeof buf, "DPC %06x %06x %06x", (unsigned)(DPC_START & 0xFFFFFF),
               (unsigned)(DPC_END & 0xFFFFFF), (unsigned)(DPC_CURRENT & 0xFFFFFF));
  line(col_dim, buf);
  if (f) {
    static const char rn[32][3] = { "zr","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3",
      "t4","t5","t6","t7","s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra" };
    static const u8 order[] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,28,29,30,31 };
    cy += 3;
    for (u32 i = 0; i < sizeof order; i += 3) {
      u32 n = 0;
      for (u32 j = i; j < i + 3 && j < sizeof order; j++)
        n += k64_snprintf(buf + n, sizeof buf - n, "%s %08x ", rn[order[j]], (u32)f->gpr[order[j]]);
      line(col_fg, buf);
    }
  }
  for (;;) {}
}

void k64_crash(k64_exc_frame_t *f) {
  char title[48];
  k64_snprintf(title, sizeof title, "%s", exc_name((f->cause >> 2) & 31));
  report(title, f, f->epc, (u32)f->gpr[29], (u32)f->gpr[31], 0);
}

// Kept for code that drew its own title (fault probes): same screen, under the caller's title.
void k64_crash_screen(const char *title, const k64_exc_frame_t *f) {
  report(title, f, f ? f->epc : 0, f ? (u32)f->gpr[29] : 0, f ? (u32)f->gpr[31] : 0, 0);
}

// k64_panic's body: the backtrace starts here, the two first frames (this and k64_panic) are
// left out of the list.
__attribute__((noinline, noreturn)) void k64_crash_panic(const char *msg) {
  u32 sp, ra;
  __asm__ volatile("move %0, $sp" : "=r"(sp));
  __asm__ volatile("move %0, $ra" : "=r"(ra));
here:
  report(msg, 0, (u32)&&here, sp, ra, 2);
}
