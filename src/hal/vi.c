// kestrel64-sdk — VI timing tables + framebuffer chain (poll-driven, v1).
// Timing values: libdragon n64sys.c / n64brew VI page (standard 320x240 progressive).
#include <k64/vi.h>
#include <k64/mem.h>
#include <k64/cpu.h>
#include <k64/debug.h>
#include <k64/timer.h>

// VI regs 5..11: burst, v_sync, h_sync, h_sync_leap, h_video, v_video, v_burst
static const u32 vi_timing[3][7] = {
  [K64_TV_PAL]  = { 0x0404233a, 0x00000271, 0x00150c69, 0x0c6f0c6e, 0x00800300, 0x005f0239, 0x0009026b },
  [K64_TV_NTSC] = { 0x03e52239, 0x0000020d, 0x00000c15, 0x0c150c15, 0x006c02ec, 0x002501ff, 0x000e0204 },
  [K64_TV_MPAL] = { 0x04651e39, 0x0000020d, 0x00040c11, 0x0c190c1a, 0x006c02ec, 0x002501ff, 0x000e0204 },
};

#define VI_TYPE_16       2u
#define VI_TYPE_32       3u
#define VI_GAMMA_DITHER  (1u << 2)
#define VI_DIVOT         (1u << 4)
#define VI_SERRATE       (1u << 6)
#define SR_IM_RCP        0x0400u    // IP2: RCP (MI) interrupt line
#define VI_AA_FULL       (0u << 8)   // AA + resample, always fetch extra line
#define VI_AA_RESAMPLE   (2u << 8)   // resample only
#define VI_DEDITHER      (1u << 16)
#define VI_PIXEL_ADV     (3u << 12)

// Per field (V_CURRENT bit 0 at the vblank interrupt): VI_ORIGIN, VI_V_BURST. Read by
// crt0.S, layout fixed: [field * 2 + 0] = origin, [field * 2 + 1] = v_burst.
u32 k64_vi_field[4] K64_ALIGN(16);
bool k64_vi_interlaced;

// MPAL: old VI revisions (NUS-CPU-02) garble the top unless V_BURST changes every field.
// Values from libdragon vi.c (found on hardware by its authors; Unlicense).
static const u32 mpal_vburst[2] = { 0x000B0202, 0x000E0204 };

static k64_surface_t surf[3];
static u16 own_width, own_height;  // scaler setup of the own buffers
static bool scanning;              // k64_display_scan window active
static u8  nbuf, front;
static u32 frames;
static u32 vi_ctrl;

k64_tv_t k64_tv_type(void) {
  u32 t = K64_BOOT_TVTYPE;
  return t <= 2 ? (k64_tv_t)t : K64_TV_NTSC;
}

void k64_vi_wait_vblank(void) {
  // V_CURRENT counts half-lines; line 2 is the start of the vertical blank interval as
  // programmed by the tables above (V_INTR convention). Wait for a fresh arrival.
  while ((VI_V_CURRENT & ~1u) == 2) {}
  while ((VI_V_CURRENT & ~1u) != 2) {}
  frames++;
}

u32 k64_vi_frame_count(void) { return frames; }
void k64_vi_set_origin(u32 phys) { VI_ORIGIN = phys; }

// Thread-module side of k64_vi_isr: called from its interrupt handler before the ack.
void k64_vi_field_irq(void) {
  u32 f = VI_V_CURRENT & 1;
  VI_ORIGIN = k64_vi_field[f * 2];
  VI_V_BURST = k64_vi_field[f * 2 + 1];
}

static void set_field_origins(u32 phys, u32 stride) {
  k64_vi_field[0] = phys + stride;
  k64_vi_field[2] = phys;
}

void k64_display_init(const k64_display_cfg_t *cfg) {
  K64_ASSERT(cfg->buffers >= 1 && cfg->buffers <= 3);
  K64_ASSERT((cfg->width == 320 || cfg->width == 640) && (cfg->height == 240 || cfg->height == 480));
  bool ilace = cfg->height == 480;
  nbuf = cfg->buffers;
  own_width = cfg->width; own_height = cfg->height; scanning = false;
  u32 stride = cfg->width * cfg->bpp;
  u32 size = stride * cfg->height;
  for (int i = 0; i < nbuf; i++) {
    // 64-byte align: RDP color image must be 64B aligned for best bursts (8B minimum).
    void *p = k64_mem_alloc(K64_ZONE_FB, size, 64);
    surf[i] = (k64_surface_t){ p, cfg->width, cfg->height, (u16)stride, cfg->bpp, (u8)i };
    // Clear through KSEG1: no point dragging 150-300 KB through the 8 KB D$.
    u64 *q = (u64*)K64_KSEG1(p);
    for (u32 n = 0; n < size / 8; n++) q[n] = 0;
  }
  front = 0;

  vi_ctrl = (cfg->bpp == K64_BPP32 ? VI_TYPE_32 : VI_TYPE_16) | VI_PIXEL_ADV;
  vi_ctrl |= cfg->antialias ? (VI_AA_FULL | VI_DIVOT) : VI_AA_RESAMPLE;
  if (cfg->bpp == K64_BPP16) {
    vi_ctrl |= VI_GAMMA_DITHER;
    if (cfg->dither_filter) vi_ctrl |= VI_DEDITHER;
  }

  if (ilace) vi_ctrl |= VI_SERRATE;

  k64_tv_t tv = k64_tv_type();
  const u32 *t = vi_timing[tv];
  u32 ie = k64_irq_disable();
  if (k64_vi_interlaced && !ilace) MI_MASK = MI_MASK_CLR(3);
  k64_vi_interlaced = ilace;
  VI_CTRL = 0;                                  // blank while reprogramming
  VI_ORIGIN = K64_PHYS(surf[0].pixels);
  VI_WIDTH = cfg->width;
  VI_V_INTR = 2;
  VI_V_CURRENT = 0;                             // write clears VI interrupt
  for (int i = 0; i < 7; i++) VI_REGS[5 + i] = t[i];
  if (ilace) VI_V_SYNC = t[1] & ~1u;            // V_SYNC is lines - 1: interlace is odd
  VI_X_SCALE = (cfg->width * 1024u) / 640u;     // 320 -> 0x200, 640 -> 0x400
  VI_Y_SCALE = (cfg->height * 1024u) / 240u;    // 240 -> 0x400, 480 -> 0x800
  VI_CTRL = vi_ctrl;
  if (ilace) {
    set_field_origins(K64_PHYS(surf[0].pixels), stride);
    for (int f = 0; f < 2; f++) k64_vi_field[f * 2 + 1] = tv == K64_TV_MPAL ? mpal_vburst[f] : t[6];
    MI_MASK = MI_MASK_SET(3);
    k64_set_status(k64_status() | SR_IM_RCP);   // IE comes back with the restore below
    ie = 1;
  }
  k64_irq_restore(ie);
}

const k64_surface_t *k64_display_acquire(void) {
  // Double buffering: show() swaps at vblank synchronously, so the non-front buffer is free.
  // Triple: show() allows one swap per field, so front + 1 left the screen a field ago.
  return &surf[(front + 1) % nbuf];
}

const k64_surface_t *k64_display_surface(u32 i) { return &surf[i % nbuf]; }

// Triple buffering does not wait for the vblank: the VI latches VI_ORIGIN at the start of
// each field, so a write at any time is shown from the next field on, without tearing. The
// only rule is one swap per field, otherwise the buffer replaced last time could still be
// on screen when it is handed out again. A field start has passed when V_CURRENT went back
// down, or when more than a field period went by.
static u32 show_line, show_count;

void k64_display_show(const k64_surface_t *s) {
  k64_time();                       // keeps the 64-bit time base ahead of Count wraps
  if (nbuf < 3) {
    k64_vi_wait_vblank();
  } else {
    u32 field = k64_tv_type() == K64_TV_PAL ? K64_COUNT_HZ / 50 : K64_COUNT_HZ / 59;
    if ((VI_V_CURRENT & ~1u) >= show_line && k64_count() - show_count < field)
      k64_vi_wait_vblank();
    else
      frames++;
    show_line = VI_V_CURRENT & ~1u;
    show_count = k64_count();
  }
  if (k64_vi_interlaced) {
    // The vblank interrupt may already have run for this field: apply its origin here too.
    u32 ie = k64_irq_disable();
    set_field_origins(K64_PHYS(s->pixels), s->stride);
    VI_ORIGIN = k64_vi_field[(VI_V_CURRENT & 1) * 2];
    k64_irq_restore(ie);
  } else {
    if (scanning) {
      VI_WIDTH = own_width;
      VI_X_SCALE = (own_width * 1024u) / 640u;
      VI_Y_SCALE = (own_height * 1024u) / 240u;
      scanning = false;
    }
    VI_ORIGIN = K64_PHYS(s->pixels);
  }
  front = s->index;
}

void k64_display_scan(u32 phys, u16 width, u16 height, u16 stride_px) {
  K64_ASSERT(!k64_vi_interlaced && width && width <= 640 && height && height <= 240 && stride_px >= width);
  // The registers latch at the field start like VI_ORIGIN: written together outside the
  // active area nothing tears.
  k64_vi_wait_vblank();
  VI_WIDTH = stride_px;
  VI_X_SCALE = (width * 1024u) / 640u;          // input pixels per output pixel, 2.10
  VI_Y_SCALE = (height * 1024u) / 240u;
  VI_ORIGIN = phys;
  scanning = true;
}

const k64_surface_t *k64_display_front(void) { return &surf[front]; }
