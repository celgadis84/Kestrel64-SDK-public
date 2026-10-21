// kestrel64-sdk — IS-Viewer output, tiny printf, panic/crash reporting.
#include <k64/debug.h>
#include <k64/cpu.h>

// IS-Viewer 64 register block: write length at +0x14, text buffer at +0x20.
#define ISV_BASE   0xB3FF0000u
#define ISV_WRITE  K64_REG(ISV_BASE + 0x14)
#define ISV_BUF    (ISV_BASE + 0x20)
#define ISV_CHUNK  512u

// The IS-Viewer sits on the PI bus: wait until no DMA or IO access is in flight.
K64_INLINE void pi_io_wait(void) { while (PI_STATUS & (PI_ST_DMA_BUSY | PI_ST_IO_BUSY)) {} }

// Sends in chunks of up to 512 bytes: fill the buffer, then write the chunk length.
void k64_debug_write(const char *s, u32 len) {
  while (len) {
    u32 n = len < ISV_CHUNK ? len : ISV_CHUNK;
    for (u32 i = 0; i < n; i += 4) {
      u32 w = 0;
      // Four chars per word, big-endian, zero-padded past the chunk end.
      for (u32 b = 0; b < 4; b++) w = (w << 8) | (i + b < n ? (u8)s[i + b] : 0);
      pi_io_wait();
      K64_REG(ISV_BUF + i) = w;
    }
    pi_io_wait();
    ISV_WRITE = n;
    s += n; len -= n;
  }
}

// ---- formatter: %[-0][width][.prec][l|ll]{d,i,u,x,X,p,s,c,f,%} -----------------------------
typedef struct { char *p, *end; int n; } out_t;
// Counts every character, stores only while room is left (one byte is kept for the NUL).
static void put(out_t *o, char c) { if (o->p < o->end) *o->p++ = c; o->n++; }

// Digits are built least significant first. Space padding goes before the sign, zero padding after.
static void put_num(out_t *o, u64 v, bool neg, u32 base, bool upper, int width, char pad, bool left) {
  char digits[24]; int k = 0;
  const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
  do { digits[k++] = dig[v % base]; v /= base; } while (v);
  int len = k + (neg ? 1 : 0);
  if (!left && pad == ' ') for (; width > len; width--) put(o, ' ');
  if (neg) put(o, '-');
  if (!left && pad == '0') for (; width > len; width--) put(o, '0');
  while (k) put(o, digits[--k]);
  if (left) for (; width > len; width--) put(o, ' ');
}

int k64_vsnprintf(char *buf, u32 size, const char *fmt, va_list ap) {
  out_t o = { buf, buf + (size ? size - 1 : 0), 0 };
  for (; *fmt; fmt++) {
    if (*fmt != '%') { put(&o, *fmt); continue; }
    fmt++;
    bool left = false; char pad = ' '; int width = 0, lng = 0;
    for (;; fmt++) { if (*fmt == '-') left = true; else if (*fmt == '0') pad = '0'; else break; }
    while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
    int prec = -1;
    if (*fmt == '.') { fmt++; prec = 0; while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0'); }
    while (*fmt == 'l') { lng++; fmt++; }
    switch (*fmt) {
      case 'd': case 'i': {
        s64 v = lng >= 2 ? va_arg(ap, s64) : lng ? va_arg(ap, long) : va_arg(ap, int);
        put_num(&o, v < 0 ? (u64)-v : (u64)v, v < 0, 10, false, width, pad, left); break; }
      case 'u': case 'x': case 'X': {
        u64 v = lng >= 2 ? va_arg(ap, u64) : lng ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
        put_num(&o, v, false, *fmt == 'u' ? 10 : 16, *fmt == 'X', width, pad, left); break; }
      case 'p': put_num(&o, (u32)va_arg(ap, void*), false, 16, false, 8, '0', false); break;
      case 'c': put(&o, (char)va_arg(ap, int)); break;
      case 'f': case 'F': {
        // Fixed-point only (no exponent form); precision capped at 9 digits.
        double v = va_arg(ap, double);
        // NaN prints as "nan" (no sign).
        if (v != v) { put(&o, 'n'); put(&o, 'a'); put(&o, 'n'); break; }
        bool neg = v < 0; if (neg) v = -v;
        if (prec < 0) prec = 6;
        if (prec > 9) prec = 9;
        u64 scale = 1; for (int i = 0; i < prec; i++) scale *= 10;
        // Too large for the u64 scaled value: print inf.
        if (v > 1.8e19 / (double)scale) { if (neg) put(&o, '-'); put(&o, 'i'); put(&o, 'n'); put(&o, 'f'); break; }
        // Round half up, then split the scaled value into integer and fraction digits.
        u64 fx = (u64)(v * (double)scale + 0.5);
        u64 ip = fx / scale, fp = fx % scale;
        put_num(&o, ip, neg, 10, false, width > prec + 1 ? width - (prec ? prec + 1 : 0) : 0, pad, left && !prec);
        if (prec) { put(&o, '.'); put_num(&o, fp, false, 10, false, prec, '0', false); }
        break; }
      case 's': {
        const char *s = va_arg(ap, const char*); if (!s) s = "(null)";
        int len = 0; while (s[len]) len++;
        if (!left) for (; width > len; width--) put(&o, ' ');
        while (*s) put(&o, *s++);
        if (left) for (; width > len; width--) put(&o, ' ');
        break; }
      case '%': put(&o, '%'); break;
      // Format ends right after '%': step back so the loop's increment lands on the NUL.
      case 0: fmt--; break;
      default: put(&o, '%'); put(&o, *fmt); break;
    }
  }
  if (size) *o.p = 0;
  return o.n;
}

int k64_snprintf(char *buf, u32 size, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); int n = k64_vsnprintf(buf, size, fmt, ap); va_end(ap); return n;
}

void k64_printf(const char *fmt, ...) {
  char buf[256];
  va_list ap; va_start(ap, fmt); int n = k64_vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  // Output past the 255-char buffer is dropped; vsnprintf already returned the full length.
  if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
  k64_debug_write(buf, (u32)n);
}

void k64_crash_panic(const char *msg) __attribute__((noreturn));   // crash.c

void k64_panic(const char *fmt, ...) {
  char buf[160];
  va_list ap; va_start(ap, fmt); k64_vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  k64_crash_panic(buf);
}

// Nothing to return to: report the code and halt.
void k64_exit(int code) {
  k64_printf("\n[k64] main returned %d\n", code);
  for (;;) {}
}
