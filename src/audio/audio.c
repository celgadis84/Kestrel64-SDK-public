// kestrel64-sdk — AI output: three rotating buffers fed through the AI's two-entry DMA queue.
//
// The AI plays one buffer and holds one queued. While its queue is not full it holds at most
// the last buffer submitted, so the one after it (two submissions back) is free to fill.
#include <k64/k64.h>

#define AUDIO_BUFS 3

static s16 *bufs[AUDIO_BUFS];
static u32 nframes, out_rate, next_buf;
static bool filling;

static u32 vi_clock(void) {
  switch (k64_tv_type()) {
  case K64_TV_PAL:  return 49656530;
  case K64_TV_MPAL: return 48628316;
  default:          return 48681812;
  }
}

void k64_audio_init(u32 rate, u32 frames) {
  K64_ASSERT((frames & 3) == 0 && frames > 0);
  u32 clock = vi_clock();
  u32 dac = (2 * clock / rate + 1) / 2;        // rounded divider
  u32 bits = dac / 66;
  if (bits > 16) bits = 16;
  AI_DACRATE = dac - 1;
  AI_BITRATE = bits - 1;
  AI_CONTROL = 1;                              // DMA enable
  out_rate = clock / dac;
  nframes = frames;
  static u32 cap;                              // frames the buffers hold: a new init reuses them
  for (u32 i = 0; i < AUDIO_BUFS; i++) {
    if (frames > cap)                          // AI DMA reads them: any bank will do, so a full heap spills to fb
      bufs[i] = k64_mem_alloc(k64_mem_largest_free(K64_ZONE_HEAP) >= frames * 4 + 16 ? K64_ZONE_HEAP : K64_ZONE_FB,
                              frames * 4, 16);
    for (u32 k = 0; k < frames * 2; k++) bufs[i][k] = 0;
    k64_dcache_writeback(bufs[i], frames * 4);
  }
  if (frames > cap) cap = frames;
  next_buf = 0;
  filling = false;
}

u32 k64_audio_rate(void)   { return out_rate; }
u32 k64_audio_frames(void) { return nframes; }

s16 *k64_audio_next(void) {
  if (filling) return bufs[next_buf];
  if (AI_STATUS & AI_ST_FULL) return NULL;
  filling = true;
  return bufs[next_buf];
}

void k64_audio_submit(s16 *buf) {
  K64_ASSERT(filling && buf == bufs[next_buf]);
  while (AI_STATUS & AI_ST_FULL) {}
  AI_DRAM_ADDR = K64_PHYS(buf);
  AI_LEN = nframes * 4;
  AI_STATUS = 0;                               // acknowledge the AI interrupt (masked anyway)
  filling = false;
  next_buf = next_buf + 1 == AUDIO_BUFS ? 0 : next_buf + 1;
}
