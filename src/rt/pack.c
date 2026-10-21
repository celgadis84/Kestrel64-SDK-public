// kestrel64-sdk — LZ4 block decoder, RAM and streamed in place from ROM (see k64/pack.h).
//
// In-place layout: dst[0 .. buf_size) with the stream at D = buf_size - align8(packed).
// tools/k64pack.py sizes buf_size so that output + 32 <= unread input at every point:
// room for the fast paths' overcopy, and the decoder never writes a D$ line that still
// holds stream bytes to read, nor one that a DMA in flight will fill. Reads stop at `avail`, 16-byte aligned until the last chunk,
// so no line beyond the DMA'd data is ever cached.
#include <k64/pack.h>
#include <k64/pi.h>
#include <k64/cpu.h>
#include <k64/mem.h>
#include <k64/debug.h>

#define CHUNK_MIN 1024u      // first PI DMA: small, so decoding starts early
#define CHUNK_MAX 8192u      // later ones double up to this
#define POLL      512u        // stream bytes between PI status checks (see set_lim)

// 64-bit access that does not need 8-byte alignment: the fast paths run at any output offset.
typedef u64 u64_un __attribute__((aligned(1), may_alias));

typedef struct {
  const u8 *in, *lim;           // fast path: bytes below lim need no check
  const u8 *avail, *end;        // readable end, stream end
  u8 *dma_next, *dma_end;       // streaming: next DMA destination, end of the padded stream
  u32 rom_next, chunk;
  bool busy;
} src_t;


static void set_lim(src_t *s) {
  // While DMA work remains, come back every POLL bytes to start the next chunk as soon as
  // the PI is free (one PI_STATUS read per POLL bytes of stream).
  u32 left = s->avail - s->in;
  s->lim = s->dma_next == s->dma_end && !s->busy ? s->avail : s->in + (left > POLL ? POLL : left);
}

// Retires the finished DMA, if any, then starts the next chunk. avail moves to what has landed.
static void kick(src_t *s) {
  if (s->busy) {
    if (k64_pi_busy()) return;
    PI_STATUS = PI_WR_CLR_INTR;
    s->busy = false;
    s->avail = s->dma_next == s->dma_end ? s->end : (const u8 *)((u32)s->dma_next & ~15u);
  }
  if (s->dma_next < s->dma_end) {
    u32 n = s->dma_end - s->dma_next;
    // The decoder reads a chunk only once it has fully landed, so it trails the DMA by a
    // chunk: shrink them towards the end to shorten the decode left after the last one.
    u32 c = n / 2 > CHUNK_MIN ? (n / 2) & ~15u : CHUNK_MIN;   // DMA: 8-byte multiples
    if (c > s->chunk) c = s->chunk;
    if (n > c) n = c;
    k64_pi_read_async(s->dma_next, s->rom_next, n);
    if (s->chunk < CHUNK_MAX) s->chunk *= 2;
    s->dma_next += n;
    s->rom_next += n;
    s->busy = true;
  }
}

// Slow path: make in[0 .. n) readable.
static K64_NOINLINE void refill(src_t *s, u32 n) {
  kick(s);
  while (s->in + n > s->avail) {
    K64_ASSERT(s->busy);         // a valid stream never reads past its end
    k64_pi_wait();
    kick(s);
  }
  set_lim(s);
}

#define NEED(s, n) do { if (K64_UNLIKELY((s)->in + (n) > (s)->lim)) refill(s, n); } while (0)

// Forward copy, exact (never reads or writes past n): dst may overlap src when dst < src
// (in-place literals) or src + 8 <= dst (matches with offset >= 8).
static inline void copy_fwd(u8 *d, const u8 *s, u32 n) {
  for (; n >= 8; n -= 8, d += 8, s += 8) *(u64_un *)d = *(const u64_un *)s;
  for (; n; n--) *d++ = *s++;
}

// LZ4 length extension: adds bytes until one is not 255.
static u32 ext_len(src_t *s, u32 len) {
  u32 b;
  do {
    NEED(s, 1);
    b = *s->in++;
    len += b;
  } while (b == 255);
  return len;
}

// Repeat step of a match with offset < 8: off * ceil(8 / off), the smallest multiple of off >= 8.
static const u8 period8[8] = { 0, 8, 8, 9, 8, 10, 12, 14 };

static inline void cpy8(u8 *d, const u8 *s) { *(u64_un *)d = *(const u64_un *)s; }

// out_lim = end - 24: from below it the fast paths may write up to 24 bytes past a
// sequence's end (a match ending at e <= out_lim + 16 may write 7 past e).
// In place that lands on stream bytes already read (k64pack keeps a 32-byte margin).
static void lz4(src_t *s, u8 *out, u8 *out_lim) {
  for (;;) {
    u32 t, lit;
    // Fast literal path: token below 0xF0 (literals under 15), input and output room for the copy.
    if (K64_LIKELY(s->in + 20 <= s->lim && out <= out_lim) && (t = *s->in) < 0xF0) {
      // up to 14 literals: copy 16, keep lit
      lit = t >> 4;
      cpy8(out, s->in + 1);
      cpy8(out + 8, s->in + 9);
      s->in += 1 + lit;
      out += lit;
    } else {
      NEED(s, 1);
      t = *s->in++;
      lit = t >> 4;
      if (lit == 15) lit = ext_len(s, lit);
      while (lit) {
        NEED(s, 1);
        u32 k = s->avail - s->in;
        if (k > lit) k = lit;
        copy_fwd(out, s->in, k);
        out += k;
        s->in += k;
        lit -= k;
      }
    }
    // A sequence with literals only ends the block: no offset follows.
    if (s->in == s->end) return;
    NEED(s, 2);
    // Match offset, little-endian.
    u32 off = s->in[0] | s->in[1] << 8;
    s->in += 2;
    u32 ml = t & 15;
    if (ml == 15) ml = ext_len(s, ml);
    ml += 4;
    const u8 *m = out - off;
    u8 *e = out + ml;
    // Short match at offset >= 8: up to three 8-byte copies, each source already written.
    if (K64_LIKELY(off >= 8 && ml <= 24 && out <= out_lim)) {
      cpy8(out, m);
      cpy8(out + 8, m + 8);
      cpy8(out + 16, m + 16);
    } else if (K64_LIKELY(e <= out_lim + 16)) {
      // wide copy, up to 7 bytes past e. A period under 8 bytes: the first 8 one at a time,
      // then the pattern repeats every off * ceil(8 / off) >= 8 bytes.
      u8 *p = out;
      if (off < 8) {
        for (u32 i = 0; i < 8; i++) p[i] = m[i];
        p += 8;
        m = p - period8[off];
      }
      for (; p < e; p += 8, m += 8) cpy8(p, m);
    } else {
      // Near the end of the output: byte-exact copy, nothing written past e.
      for (u8 *p = out; p < e; ) *p++ = *m++;
    }
    out = e;
  }
}

u32 k64_unpack(void *dst, const void *src) {
  const k64_pack_hdr_t *h = src;
  K64_ASSERT(h->fourcc == K64_PACK_FOURCC);
  src_t s = {0};
  s.in = s.lim = s.avail = (const u8 *)(h + 1);
  s.end = s.in + h->packed_size;
  // Whole stream already in RAM: no DMA, no limits.
  s.lim = s.avail = s.end;
  // 24 bytes of slack at the end for the fast paths' overcopy.
  lz4(&s, dst, (u8 *)dst + h->raw_size - 24);
  return h->raw_size;
}

k64_pack_hdr_t k64_pack_info(u32 rom_addr) {
  k64_pack_hdr_t h K64_ALIGN(16);
  k64_pi_read(&h, rom_addr, sizeof h);
  K64_ASSERT(h.fourcc == K64_PACK_FOURCC);
  return h;
}

// In-place decode of a ROM-resident stream: the stream sits at the top of dst while the output
// grows up from its bottom, and the DMA runs ahead of the decoder.
static u32 unpack_rom(void *dst, u32 rom_addr, const k64_pack_hdr_t *h) {
  K64_ASSERT(((u32)dst & 15) == 0);
  u32 padded = (h->packed_size + 7) & ~7u;
  u8 *d = (u8 *)dst + h->buf_size - padded;
  // The stream area must hold no dirty line a DMA could race with; the first line may
  // start below it (output area): write it back, the rest is overwritten anyway.
  if ((u32)d & 15) K64_CACHE_OP(K64_D_HIT_WB_INV, (u32)d & ~15u);
  k64_dcache_invalidate(d, padded);
  src_t s = {
    .in = d, .avail = d, .end = d + h->packed_size,
    .dma_next = d, .dma_end = d + padded, .rom_next = rom_addr + sizeof *h, .chunk = CHUNK_MIN,
  };
  // Start the first chunk; the decoder waits for it in refill().
  kick(&s);
  set_lim(&s);
  lz4(&s, dst, (u8 *)dst + h->raw_size - 24);
  return h->raw_size;
}

u32 k64_unpack_rom(void *dst, u32 rom_addr) {
  k64_pack_hdr_t h = k64_pack_info(rom_addr);
  return unpack_rom(dst, rom_addr, &h);
}

void *k64_unpack_rom_alloc(u32 rom_addr, u32 *size) {
  k64_pack_hdr_t h = k64_pack_info(rom_addr);
  void *p = k64_mem_alloc(K64_ZONE_HEAP, h.buf_size, 16);
  K64_ASSERT(p);
  u32 n = unpack_rom(p, rom_addr, &h);
  if (size) *size = n;
  return p;
}
