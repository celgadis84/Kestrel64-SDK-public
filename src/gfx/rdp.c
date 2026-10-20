// kestrel64-sdk — RDP command stream (RDRAM mode, ring of blocks, incremental DPC_END).
#include <k64/rdp.h>
#include <k64/cpu.h>
#include <k64/mem.h>
#include <k64/debug.h>

#define RDP_BLOCKS      4
#define RDP_BLOCK_WORDS 2048          // 16 KB per block
_Static_assert(RDP_BLOCKS * RDP_BLOCK_WORDS * 8 == K64_RDP_RING_BYTES, "ring size");

k64_rdp_stream_t k64_rdp;
k64_rdp_state_t  k64_rdps;
static u64 *blocks[RDP_BLOCKS];
static bool block_started;            // DPC_START issued for the current block

void k64_rdp_init(void) {
  k64_rdp_init_at(k64_mem_alloc(K64_ZONE_HEAP, K64_RDP_RING_BYTES, 64));
}

void k64_rdp_init_at(void *mem) {
  K64_ASSERT(((u32)mem & 7) == 0);
  for (int i = 0; i < RDP_BLOCKS; i++)
    blocks[i] = (u64 *)mem + i * RDP_BLOCK_WORDS;
  k64_rdp.block_idx = 0;
  k64_rdp.block = k64_rdp.cur = k64_rdp.sent = k64_rdp.lim = blocks[0];
  k64_rdp.end = blocks[0] + RDP_BLOCK_WORDS;
  block_started = false;
  DPC_STATUS = DPC_WR_CLR_XBUS | DPC_WR_CLR_FREEZE | DPC_WR_CLR_FLUSH;
  MI_MODE = MI_MODE_CLR_DP;
  k64_rdps = (k64_rdp_state_t){ .other_modes = ~0ull, .combine = ~0ull,
    .fill_color = 0xDEADBEEF, .prim_color = 0xDEADBEEF, .env_color = 0xDEADBEEF,
    .blend_color = 0xDEADBEEF, .fog_color = 0xDEADBEEF };
}

void k64_rdp_flush(void) {
  u64 *a = k64_rdp.sent, *b = k64_rdp.cur;
  if (a == b) return;
  k64_dcache_writeback(a, (u32)((u8*)b - (u8*)a));
  if (!block_started) {
    while (DPC_STATUS & DPC_ST_START_VALID) {}       // previous START not consumed yet
    DPC_START = K64_PHYS(a);
    block_started = true;
  }
  DPC_END = K64_PHYS(b);                             // extends the transfer in flight
  k64_rdp.sent = b;
}

void k64_rdp_overflow(void) {
  k64_rdp_flush();
  // Next block is free once the RDP has accepted the current block's START: from then on it
  // only ever reads the current block (or is idle).
  if (DPC_STATUS & DPC_ST_START_VALID) {
    u32 t = k64_count();
    while (DPC_STATUS & DPC_ST_START_VALID) {}
    k64_rdps.stat_stall += k64_count() - t;
  }
  k64_rdp.block_idx = (k64_rdp.block_idx + 1) % RDP_BLOCKS;
  k64_rdp.block = k64_rdp.cur = k64_rdp.sent = k64_rdp.lim = blocks[k64_rdp.block_idx];
  k64_rdp.end = k64_rdp.block + RDP_BLOCK_WORDS;
  block_started = false;
}

#define RDP_AHEAD_LINES 4             // lines prepared per k64_rdp_more

void k64_rdp_more(u32 n) {
  if (k64_rdp.cur + n > k64_rdp.end) k64_rdp_overflow();
  // Lines from the first one not prepared yet up to past cur + n, a few ahead, within the block
  u32 a = ((u32)(k64_rdp.lim > k64_rdp.cur ? k64_rdp.lim : k64_rdp.cur) + 15) & ~15u;
  u32 to = ((u32)(k64_rdp.cur + n) + RDP_AHEAD_LINES * 16 - 1) & ~15u;
  if (to > (u32)k64_rdp.end) to = (u32)k64_rdp.end;
  for (; a < to; a += 16) K64_CACHE_OP(K64_D_CREATE_DIRTY, a);
  k64_rdp.lim = (u64 *)to;
}

void k64_rdp_ring_export(k64_rdp_ring_t *r) {
  k64_rdp_flush();
  // The other producer writes from the cursor on: drop the lines prepared ahead of it, whose
  // garbage would otherwise be written back over its commands.
  u32 a = ((u32)k64_rdp.cur + 15) & ~15u;
  if ((u32)k64_rdp.lim > a) k64_dcache_invalidate((void *)a, (u32)k64_rdp.lim - a);
  k64_rdp.lim = k64_rdp.cur;
  for (int i = 0; i < RDP_BLOCKS; i++) r->block[i] = K64_PHYS(blocks[i]);
  r->idx = k64_rdp.block_idx;
  r->cur = K64_PHYS(k64_rdp.cur);
  r->end = K64_PHYS(k64_rdp.end);
  r->started = block_started;
}

void k64_rdp_ring_import(const k64_rdp_ring_t *r) {
  K64_ASSERT(r->idx < RDP_BLOCKS && (r->cur & 7) == 0);
  k64_rdp.block_idx = r->idx;
  k64_rdp.block = blocks[r->idx];
  k64_rdp.cur = k64_rdp.sent = k64_rdp.lim = (u64 *)K64_KSEG0(r->cur);
  k64_rdp.end = (u64 *)K64_KSEG0(r->end);
  block_started = r->started != 0;
  // The line holding the new cursor may sit stale in D$ (an earlier lap of the ring): the
  // next CPU store would merge into it and write the other producer's bytes back over.
  k64_dcache_invalidate((void *)((u32)k64_rdp.cur & ~15u), 16);
}

bool k64_rdp_busy(void) {
  return (DPC_STATUS & (DPC_ST_START_VALID | DPC_ST_CMD_BUSY | DPC_ST_PIPE_BUSY | DPC_ST_DMA_BUSY)) != 0;
}

void k64_rdp_wait_idle(void) {
  k64_rdp_put(k64_rdpc_sync_full());
  k64_rdps.prim_pending = k64_rdps.tile_pending = k64_rdps.load_pending = false;
  k64_rdp_flush();
  u32 t0 = k64_count();
  while (!(MI_INTR & MI_INTR_DP))
    if (k64_count() - t0 > 2 * K64_COUNT_HZ)   // a frame is milliseconds: this is a hang
      k64_panic("rdp: sync_full never done\nDPC %08x %08x %08x st %04x\nSP %08x PC %03x",
                (unsigned)DPC_START, (unsigned)DPC_END, (unsigned)DPC_CURRENT,
                (unsigned)DPC_STATUS, (unsigned)SP_STATUS, (unsigned)SP_PC);
  MI_MODE = MI_MODE_CLR_DP;
  // Start the next frame on a fresh block boundary: keeps DPC buffers short and lets the
  // ring recycle without ever waiting on START_VALID mid-frame.
  k64_rdp_overflow();
}

u32 k64_rdp_clock(void) { return DPC_CLOCK & 0xFFFFFF; }

void k64_rdp_attach(u32 fmt, u32 siz, u32 width, u32 height, void *color, void *z) {
  k64_rdp_pipe_guard();
  u32 c = K64_PHYS(color);
  if (c != k64_rdps.color_image) { k64_rdp_put(k64_rdpc_color_image(fmt, siz, width, c)); k64_rdps.color_image = c; }
  k64_rdps.att_fmt = fmt; k64_rdps.att_siz = siz; k64_rdps.att_w = width; k64_rdps.att_h = height;
  if (z) {
    u32 zp = K64_PHYS(z);
    if (zp != k64_rdps.z_image) { k64_rdp_put(k64_rdpc_z_image(zp)); k64_rdps.z_image = zp; }
  }
  k64_rdp_put(k64_rdpc_scissor(0, 0, width * 4, height * 4));
}

void k64_rdp_clear(u32 fill, bool clear_z) {
  u32 w = k64_rdps.att_w, h = k64_rdps.att_h;
  k64_rdp_set_other_modes(K64_OM_FILL);
  if (clear_z && k64_rdps.z_image) {
    // Z buffer is a 16-bit image: draw into it as a color image with the max depth value.
    k64_rdp_pipe_guard();
    k64_rdp_put(k64_rdpc_color_image(K64_FMT_RGBA, K64_SIZ_16, w, k64_rdps.z_image));
    k64_rdp_set_fill_color(0xFFFCFFFC);
    k64_rdp_fill_rect(0, 0, w, h);
    k64_rdp_pipe_guard();
    k64_rdp_put(k64_rdpc_color_image(k64_rdps.att_fmt, k64_rdps.att_siz, w, k64_rdps.color_image));
  }
  k64_rdp_set_fill_color(fill);
  k64_rdp_fill_rect(0, 0, w, h);
}
