// kestrel64-sdk — Video Interface + framebuffer chain.
//
// The display module owns N framebuffers (2 or 3) placed by k64_mem in their own RDRAM
// bank(s), away from the Z-buffer and texture banks. The VI scans one buffer while the
// RDP draws the next. All framebuffer pointers returned are KSEG0 cached addresses; the
// RDP/VI always get physical addresses.
#pragma once
#include "hw.h"

typedef enum { K64_TV_PAL = 0, K64_TV_NTSC = 1, K64_TV_MPAL = 2 } k64_tv_t;
typedef enum { K64_BPP16 = 2, K64_BPP32 = 4 } k64_bpp_t;

typedef struct {
  u16 width, height;   // 320 or 640 x 240 (progressive) or 480 (interlaced)
  k64_bpp_t bpp;
  u8  buffers;         // 2 = double, 3 = triple buffering
  bool antialias;      // VI resample+AA (gamma/dither filter); off = cheapest scanout
  bool dither_filter;  // VI dedither filter (only meaningful at 16bpp)
} k64_display_cfg_t;

typedef struct {
  void *pixels;        // KSEG0 pointer
  u16 width, height;
  u16 stride;          // bytes per line
  k64_bpp_t bpp;
  u8  index;
} k64_surface_t;

k64_tv_t k64_tv_type(void);
void k64_display_init(const k64_display_cfg_t *cfg);
const k64_surface_t *k64_display_acquire(void);     // next back buffer
const k64_surface_t *k64_display_surface(u32 i);    // buffer i of the chain
// Queue for scanout from the next field. Double buffering waits for the vblank; triple
// buffering returns at once unless a swap already happened in this field.
void k64_display_show(const k64_surface_t *s);
const k64_surface_t *k64_display_front(void);       // buffer currently scanned
void k64_vi_wait_vblank(void);
u32  k64_vi_frame_count(void);                      // vblanks seen by k64_display_* polls
void k64_vi_set_origin(u32 phys);
// Scan out a window of a buffer the display does not own (progressive modes only): width x
// height pixels at phys, lines stride_px pixels apart, stretched to the full screen by the VI
// scalers (any width up to 640, e.g. a PS1 VRAM display area: stride 1024, width 256..640).
// Same bpp as k64_display_init. Takes effect from the next field; k64_display_show goes back
// to the display's own buffers.
void k64_display_scan(u32 phys, u16 width, u16 height, u16 stride_px);

K64_INLINE u16 k64_rgba16(u8 r, u8 g, u8 b, u8 a) {
  return (u16)(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a ? 1 : 0));
}
K64_INLINE u32 k64_rgba32(u8 r, u8 g, u8 b, u8 a) {
  return ((u32)r << 24) | ((u32)g << 16) | ((u32)b << 8) | a;
}
