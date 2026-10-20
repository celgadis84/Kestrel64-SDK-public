// kestrel64-sdk — RDP command layer.
//
// Raw, zero-abstraction encoders for every RDP command, writing 64-bit words into a
// command stream that the RDP fetches straight from RDRAM (DPC "RDRAM mode"). The stream is
// a ring of fixed blocks; k64_rdp_flush() writes the new range back from D$ and extends
// DPC_END so the RDP starts on it while the CPU keeps producing (CPU/RDP overlap).
//
// Costs worth knowing (n64brew, RDP timing): SYNC_PIPE ~50 GCLK, SYNC_TILE ~33, SYNC_LOAD
// ~25 — fixed, even when nothing is in flight. The state helpers below track what is in
// flight and only emit the syncs that are needed (k64_rdp_* "smart" API). The raw k64_rdpc_*
// encoders never sync behind your back.
#pragma once
#include "hw.h"
#include "cpu.h"

// ---- formats -------------------------------------------------------------------
enum { K64_FMT_RGBA = 0, K64_FMT_YUV = 1, K64_FMT_CI = 2, K64_FMT_IA = 3, K64_FMT_I = 4 };
enum { K64_SIZ_4 = 0, K64_SIZ_8 = 1, K64_SIZ_16 = 2, K64_SIZ_32 = 3 };

// ---- other modes (SET_OTHER_MODES), 64-bit image of the command minus opcode -----
#define K64_OM_ATOMIC_PRIM     (1ull << 55)
#define K64_OM_CYCLE_1         (0ull << 52)
#define K64_OM_CYCLE_2         (1ull << 52)
#define K64_OM_CYCLE_COPY      (2ull << 52)
#define K64_OM_CYCLE_FILL      (3ull << 52)
#define K64_OM_PERSP_TEX       (1ull << 51)
#define K64_OM_DETAIL_TEX      (1ull << 50)
#define K64_OM_SHARPEN_TEX     (1ull << 49)
#define K64_OM_TEX_LOD         (1ull << 48)
#define K64_OM_TLUT            (1ull << 47)
#define K64_OM_TLUT_IA         (1ull << 46)
#define K64_OM_SAMPLE_BILINEAR (1ull << 45)   // 0 = point
#define K64_OM_MID_TEXEL       (1ull << 44)
#define K64_OM_BILERP0         (1ull << 43)
#define K64_OM_BILERP1         (1ull << 42)
#define K64_OM_CONVERT_ONE     (1ull << 41)
#define K64_OM_KEY             (1ull << 40)
#define K64_OM_RGB_DITHER(n)   ((u64)(n) << 38) // 0 magic sq,1 bayer,2 noise,3 none
#define K64_OM_ALPHA_DITHER(n) ((u64)(n) << 36) // 0 pattern,1 ~pattern,2 noise,3 none
#define K64_OM_BLEND(m1a0, m1b0, m2a0, m2b0, m1a1, m1b1, m2a1, m2b1) \
  ( ((u64)(m1a0) << 30) | ((u64)(m1a1) << 28) | ((u64)(m1b0) << 26) | ((u64)(m1b1) << 24) \
  | ((u64)(m2a0) << 22) | ((u64)(m2a1) << 20) | ((u64)(m2b0) << 18) | ((u64)(m2b1) << 16) )
#define K64_OM_FORCE_BLEND     (1ull << 14)
#define K64_OM_ALPHA_CVG_SEL   (1ull << 13)
#define K64_OM_CVG_X_ALPHA     (1ull << 12)
#define K64_OM_Z_MODE(n)       ((u64)(n) << 10) // 0 opaque,1 interpen,2 transparent,3 decal
#define K64_OM_CVG_DEST(n)     ((u64)(n) << 8)  // 0 clamp,1 wrap,2 zap,3 save
#define K64_OM_COLOR_ON_CVG    (1ull << 7)
#define K64_OM_IMAGE_READ      (1ull << 6)
#define K64_OM_Z_UPDATE        (1ull << 5)
#define K64_OM_Z_COMPARE       (1ull << 4)
#define K64_OM_ANTIALIAS       (1ull << 3)
#define K64_OM_Z_SOURCE_PRIM   (1ull << 2)
#define K64_OM_DITHER_ALPHA    (1ull << 1)
#define K64_OM_ALPHA_COMPARE   (1ull << 0)

// blender inputs: P/M = 0 pixel(in),1 memory,2 blend color,3 fog color
//                 A   = 0 combined alpha,1 fog alpha,2 shade alpha,3 zero
//                 B   = 0 1-A,1 memory alpha,2 one,3 zero
enum { K64_BL_IN = 0, K64_BL_MEM = 1, K64_BL_BLEND = 2, K64_BL_FOG = 3 };
enum { K64_BL_A_IN = 0, K64_BL_A_FOG = 1, K64_BL_A_SHADE = 2, K64_BL_A_ZERO = 3 };
enum { K64_BL_B_1MA = 0, K64_BL_B_MEM = 1, K64_BL_B_ONE = 2, K64_BL_B_ZERO = 3 };

// Ready-made mode words.
// BILERP0/1 = texture filter outputs RGB (both clear = YUV->RGB convert using K0..K5).
#define K64_OM_BASE        (K64_OM_RGB_DITHER(3) | K64_OM_ALPHA_DITHER(3) | K64_OM_BILERP0 | K64_OM_BILERP1)
#define K64_OM_FILL        (K64_OM_CYCLE_FILL)
#define K64_OM_COPY        (K64_OM_CYCLE_COPY)
#define K64_OM_COPY_ALPHA  (K64_OM_CYCLE_COPY | K64_OM_ALPHA_COMPARE)   // drop alpha=0 texels
// 1-cycle opaque, pass-through blender (pixel*1 + mem*0)
#define K64_OM_1C_OPAQUE \
  (K64_OM_BASE | K64_OM_CYCLE_1 | K64_OM_BLEND(K64_BL_IN, K64_BL_A_IN, K64_BL_IN, K64_BL_B_1MA, 0, 0, 0, 0))
// 1-cycle alpha blend: pixel*a + mem*(1-a)
#define K64_OM_1C_XLU      (K64_OM_BASE | K64_OM_CYCLE_1 | K64_OM_FORCE_BLEND | K64_OM_IMAGE_READ | \
                            K64_OM_BLEND(K64_BL_IN, K64_BL_A_IN, K64_BL_MEM, K64_BL_B_1MA, 0,0,0,0))
#define K64_OM_ZBUF        (K64_OM_Z_COMPARE | K64_OM_Z_UPDATE)
// 2-cycle fog by shade alpha (kgfx FOG writes the fog factor into the vertex alpha): cycle 0
// mixes FOG_COLOR in, fog * a + pixel * (1 - a); cycle 1 passes it through, opaque. Pair it
// with a 2-cycle combine whose cycle 1 is COMB (K64_CC_2C_*).
#define K64_OM_2C_FOG_SHADE (K64_OM_BASE | K64_OM_CYCLE_2 | \
                             K64_OM_BLEND(K64_BL_FOG, K64_BL_A_SHADE, K64_BL_IN, K64_BL_B_1MA, \
                                          K64_BL_IN, K64_BL_A_IN, K64_BL_IN, K64_BL_B_1MA))

// ---- color combiner ----------------------------------------------------------------
// RGB  A: 0 comb,1 tex0,2 tex1,3 prim,4 shade,5 env,6 one,7 noise,8 zero
//      B: 0 comb,1 tex0,2 tex1,3 prim,4 shade,5 env,6 keycenter,7 k4,8 zero
//      C: 0 comb,1 tex0,2 tex1,3 prim,4 shade,5 env,6 keyscale,7 comb_a,8 tex0_a,9 tex1_a,
//         10 prim_a,11 shade_a,12 env_a,13 lod_frac,14 prim_lod_frac,15 k5,16 zero
//      D: 0 comb,1 tex0,2 tex1,3 prim,4 shade,5 env,6 one,7 zero
// A    A/B/D: 0 comb,1 tex0,2 tex1,3 prim,4 shade,5 env,6 one,7 zero
//      C: 0 lod_frac,1 tex0,2 tex1,3 prim,4 shade,5 env,6 prim_lod_frac,7 zero
// Output = (A - B) * C + D
enum { CC_COMB = 0, CC_TEX0 = 1, CC_TEX1 = 2, CC_PRIM = 3, CC_SHADE = 4, CC_ENV = 5,
       CC_ONE = 6, CC_NOISE = 7, CC_ZERO = 8, CC_KEYCENTER = 6, CC_K4 = 7, CC_KEYSCALE = 6,
       CC_COMB_A = 7, CC_TEX0_A = 8, CC_TEX1_A = 9, CC_PRIM_A = 10, CC_SHADE_A = 11,
       CC_ENV_A = 12, CC_LOD_FRAC = 13, CC_PRIM_LOD_FRAC = 14, CC_K5 = 15, CC_C_ZERO = 16,
       CC_D_ZERO = 7 };
enum { CA_COMB = 0, CA_TEX0 = 1, CA_TEX1 = 2, CA_PRIM = 3, CA_SHADE = 4, CA_ENV = 5,
       CA_ONE = 6, CA_ZERO = 7, CA_LOD_FRAC = 0, CA_PRIM_LOD_FRAC = 6 };

// Full 2-cycle combine. In 1-cycle mode the hardware uses cycle-1 fields: pass the same
// equation twice (K64_CC_1CYC does it).
#define K64_CC(a0,b0,c0,d0, aa0,ab0,ac0,ad0, a1,b1,c1,d1, aa1,ab1,ac1,ad1) \
  ( ((u64)((a0)&15) << 52) | ((u64)((c0)&31) << 47) | ((u64)((aa0)&7) << 44) | ((u64)((ac0)&7) << 41) \
  | ((u64)((a1)&15) << 37) | ((u64)((c1)&31) << 32) | ((u64)((b0)&15) << 28) | ((u64)((b1)&15) << 24) \
  | ((u64)((aa1)&7) << 21) | ((u64)((ac1)&7) << 18) | ((u64)((d0)&7) << 15) | ((u64)((ab0)&7) << 12) \
  | ((u64)((ad0)&7) << 9)  | ((u64)((d1)&7) << 6)   | ((u64)((ab1)&7) << 3)  | ((u64)((ad1)&7)) )
#define K64_CC_1CYC(a,b,c,d, aa,ab,ac,ad) K64_CC(a,b,c,d, aa,ab,ac,ad, a,b,c,d, aa,ab,ac,ad)

// Common equations
#define K64_CC_SHADE        K64_CC_1CYC(CC_ZERO,CC_ZERO,CC_C_ZERO,CC_SHADE, CA_ZERO,CA_ZERO,CA_ZERO,CA_SHADE)
#define K64_CC_PRIM         K64_CC_1CYC(CC_ZERO,CC_ZERO,CC_C_ZERO,CC_PRIM,  CA_ZERO,CA_ZERO,CA_ZERO,CA_PRIM)
#define K64_CC_TEX          K64_CC_1CYC(CC_ZERO,CC_ZERO,CC_C_ZERO,CC_TEX0,  CA_ZERO,CA_ZERO,CA_ZERO,CA_TEX0)
#define K64_CC_TEX_SHADE    K64_CC_1CYC(CC_TEX0,CC_ZERO,CC_SHADE,CC_D_ZERO, CA_TEX0,CA_ZERO,CA_SHADE,CA_ZERO)
#define K64_CC_TEX_PRIM     K64_CC_1CYC(CC_TEX0,CC_ZERO,CC_PRIM,CC_D_ZERO,  CA_TEX0,CA_ZERO,CA_PRIM,CA_ZERO)
// 2-cycle: cycle 0 = the equation, cycle 1 = COMB. Alpha leaves the shade out (with fog it
// holds the fog factor): texel alpha, or one.
#define K64_CC_2C_SHADE     K64_CC(CC_ZERO,CC_ZERO,CC_C_ZERO,CC_SHADE, CA_ZERO,CA_ZERO,CA_ZERO,CA_ONE, \
                                   CC_ZERO,CC_ZERO,CC_C_ZERO,CC_COMB,  CA_ZERO,CA_ZERO,CA_ZERO,CA_COMB)
#define K64_CC_2C_TEX_SHADE K64_CC(CC_TEX0,CC_ZERO,CC_SHADE,CC_D_ZERO, CA_ZERO,CA_ZERO,CA_ZERO,CA_TEX0, \
                                   CC_ZERO,CC_ZERO,CC_C_ZERO,CC_COMB,  CA_ZERO,CA_ZERO,CA_ZERO,CA_COMB)

// ---- raw command encoders (return the 64-bit command words) ------------------------
// Nothing is written: pass the result to k64_rdp_put, or store it in a kgfx RDP block.
// Addresses are physical (K64_PHYS); colors are RGBA8888, except FILL_COLOR, which is the
// raw framebuffer pattern (two RGBA5551 pixels at 16bpp).
#define K64_RDP_OP(op)          ((u64)(op) << 56)
#define K64_FX2(v)              ((u32)(v) & 0xFFF)          // 10.2 coordinate field

// FULL: wait for the whole pipeline and raise the DP interrupt; PIPE / TILE / LOAD: see above.
K64_INLINE u64 k64_rdpc_sync_full(void)  { return K64_RDP_OP(0x29); }
K64_INLINE u64 k64_rdpc_sync_pipe(void)  { return K64_RDP_OP(0x27); }
K64_INLINE u64 k64_rdpc_sync_tile(void)  { return K64_RDP_OP(0x28); }
K64_INLINE u64 k64_rdpc_sync_load(void)  { return K64_RDP_OP(0x26); }
// width in pixels (texels for TEX_IMAGE); color / Z images need 64-byte alignment for best
// bursts (8 minimum).
K64_INLINE u64 k64_rdpc_color_image(u32 fmt, u32 siz, u32 width, u32 phys) {
  return K64_RDP_OP(0x3F) | ((u64)fmt << 53) | ((u64)siz << 51) | ((u64)(width - 1) << 32) | (phys & 0x3FFFFFF);
}
K64_INLINE u64 k64_rdpc_z_image(u32 phys)  { return K64_RDP_OP(0x3E) | (phys & 0x3FFFFFF); }
K64_INLINE u64 k64_rdpc_tex_image(u32 fmt, u32 siz, u32 width, u32 phys) {
  return K64_RDP_OP(0x3D) | ((u64)fmt << 53) | ((u64)siz << 51) | ((u64)(width - 1) << 32) | (phys & 0x3FFFFFF);
}
// Coordinates in 10.2 fixed point (pixels * 4).
K64_INLINE u64 k64_rdpc_scissor(u32 x0, u32 y0, u32 x1, u32 y1) {
  return K64_RDP_OP(0x2D) | ((u64)K64_FX2(x0) << 44) | ((u64)K64_FX2(y0) << 32) | (K64_FX2(x1) << 12) | K64_FX2(y1);
}
K64_INLINE u64 k64_rdpc_other_modes(u64 om) { return K64_RDP_OP(0x2F) | (om & 0x00FFFFFFFFFFFFFFull); }
K64_INLINE u64 k64_rdpc_combine(u64 cc)     { return K64_RDP_OP(0x3C) | (cc & 0x00FFFFFFFFFFFFFFull); }
K64_INLINE u64 k64_rdpc_fill_color(u32 c)   { return K64_RDP_OP(0x37) | c; }
K64_INLINE u64 k64_rdpc_fog_color(u32 c)    { return K64_RDP_OP(0x38) | c; }
K64_INLINE u64 k64_rdpc_blend_color(u32 c)  { return K64_RDP_OP(0x39) | c; }
// minlvl / lodfrac: the LOD clamp and the PRIM_LOD_FRAC combiner input.
K64_INLINE u64 k64_rdpc_prim_color(u32 c, u8 minlvl, u8 lodfrac) {
  return K64_RDP_OP(0x3A) | ((u64)(minlvl & 31) << 40) | ((u64)lodfrac << 32) | c;
}
K64_INLINE u64 k64_rdpc_env_color(u32 c)    { return K64_RDP_OP(0x3B) | c; }
// SET_CONVERT: YUV -> RGB factors k0..k3 of the texture filter, k4/k5 for the combiner
// (9-bit signed each).
K64_INLINE u64 k64_rdpc_set_convert(int k0, int k1, int k2, int k3, int k4, int k5) {
  return K64_RDP_OP(0x2C) | ((u64)(k0 & 0x1FF) << 45) | ((u64)(k1 & 0x1FF) << 36) | ((u64)(k2 & 0x1FF) << 27)
       | ((u64)(k3 & 0x1FF) << 18) | ((u64)(k4 & 0x1FF) << 9) | (u64)(k5 & 0x1FF);
}
// Depth for primitives with K64_OM_Z_SOURCE_PRIM (rectangles): z 0..0x7FFF, dz its slope.
K64_INLINE u64 k64_rdpc_prim_depth(u16 z, u16 dz) { return K64_RDP_OP(0x2E) | ((u32)z << 16) | dz; }
// Rectangle: (x0,y0) top-left, (x1,y1) bottom-right, 10.2. Fill/copy mode treat x1,y1 as
// inclusive pixels; 1/2-cycle modes exclusive.
K64_INLINE u64 k64_rdpc_fill_rect(u32 x0, u32 y0, u32 x1, u32 y1) {
  return K64_RDP_OP(0x36) | ((u64)K64_FX2(x1) << 44) | ((u64)K64_FX2(y1) << 32) | (K64_FX2(x0) << 12) | K64_FX2(y0);
}
// tile descriptor: line/tmem in 64-bit words
K64_INLINE u64 k64_rdpc_set_tile(u32 fmt, u32 siz, u32 line, u32 tmem, u32 tile, u32 pal,
                                 u32 ct, u32 mt, u32 maskt, u32 shiftt, u32 cs, u32 ms, u32 masks, u32 shifts) {
  return K64_RDP_OP(0x35) | ((u64)fmt << 53) | ((u64)siz << 51) | ((u64)(line & 0x1FF) << 41)
         | ((u64)(tmem & 0x1FF) << 32) | ((u64)tile << 24) | ((u64)pal << 20) | ((u64)ct << 19) | ((u64)mt << 18)
         | ((u64)maskt << 14) | ((u64)shiftt << 10) | ((u64)cs << 9) | ((u64)ms << 8) | ((u64)masks << 4) | shifts;
}
// LOAD_TILE / SET_TILE_SIZE / LOAD_TLUT share this layout: s, t in 10.2 texels.
K64_INLINE u64 k64_rdpc_tile_cmd(u32 op, u32 tile, u32 s0, u32 t0, u32 s1, u32 t1) {
  return K64_RDP_OP(op) | ((u64)K64_FX2(s0) << 44) | ((u64)K64_FX2(t0) << 32) | ((u64)tile << 24) | (K64_FX2(s1) << 12)
         | K64_FX2(t1);
}
#define k64_rdpc_load_tile(tile, s0, t0, s1, t1)  k64_rdpc_tile_cmd(0x34, tile, s0, t0, s1, t1)
#define k64_rdpc_tile_size(tile, s0, t0, s1, t1)  k64_rdpc_tile_cmd(0x32, tile, s0, t0, s1, t1)
#define k64_rdpc_load_tlut(tile, i0, i1)          k64_rdpc_tile_cmd(0x30, tile, (i0) << 2, 0, (i1) << 2, 0)
// texels (<= 2048) in one go; dxt = 2048 / (line width in 64-bit words), rounded up, so t
// advances once per line (0: the caller swaps lines itself).
K64_INLINE u64 k64_rdpc_load_block(u32 tile, u32 s0, u32 t0, u32 texels, u32 dxt) {
  return K64_RDP_OP(0x33) | ((u64)s0 << 44) | ((u64)t0 << 32) | ((u64)tile << 24) | ((u64)(texels - 1) << 12)
         | (dxt & 0xFFF);
}
// Texture rectangle: coords 10.2; s,t s10.5; dsdx,dtdy s5.10. Two words.
K64_INLINE u64 k64_rdpc_tex_rect0(u32 tile, u32 x0, u32 y0, u32 x1, u32 y1) {
  return K64_RDP_OP(0x24) | ((u64)K64_FX2(x1) << 44) | ((u64)K64_FX2(y1) << 32) | ((u64)tile << 24)
         | (K64_FX2(x0) << 12) | K64_FX2(y0);
}
K64_INLINE u64 k64_rdpc_tex_rect1(s16 s, s16 t, s16 dsdx, s16 dtdy) {
  return ((u64)(u16)s << 48) | ((u64)(u16)t << 32) | ((u32)(u16)dsdx << 16) | (u16)dtdy;
}

// ---- command stream -------------------------------------------------------------------
typedef struct {
  u64 *cur, *end;            // write cursor / end of current block (KSEG0)
  u64 *lim;                  // end of the D-cache lines prepared for writing (<= end)
  u64 *sent;                 // start of not-yet-flushed region
  u64 *block;                // start of current block
  u32  block_idx;
} k64_rdp_stream_t;
extern k64_rdp_stream_t k64_rdp;

// Allocates the ring (K64_RDP_RING_BYTES, K64_ZONE_HEAP) and resets the stream and the
// tracked state.
void k64_rdp_init(void);
// Same, with the ring in caller memory (K64_RDP_RING_BYTES, 8-byte aligned; 64 is best).
#define K64_RDP_RING_BYTES 65536
void k64_rdp_init_at(void *mem);
// Stream state in physical addresses, for a producer outside the CPU (the kgfx microcode
// writes triangles straight into the ring). Layout shared with kgfx.S (RING).
typedef struct { u32 block[4]; u32 idx, cur, end, started; } k64_rdp_ring_t;
void k64_rdp_ring_export(k64_rdp_ring_t *r); // flushes, then describes the stream
void k64_rdp_ring_import(const k64_rdp_ring_t *r);  // continues after the other producer
void k64_rdp_overflow(void);                 // current block full: flush + move to next
void k64_rdp_more(u32 n);                    // make room for n words past the cursor
void k64_rdp_flush(void);                    // make everything written so far visible to RDP
bool k64_rdp_busy(void);                     // the RDP has work queued or in its pipeline
void k64_rdp_wait_idle(void);                // emits SYNC_FULL, flushes and waits for DP intr
u32  k64_rdp_clock(void);                    // DPC_CLOCK (GCLK counter, 24-bit)

// The ring is written once, front to back, and flushed (written back) before the RDP reads
// it: the D-cache lines ahead of the cursor are created dirty (k64_rdp_more, a few at a time)
// instead of filled from RDRAM, which would cost a full read per 16 bytes of commands for data
// about to be overwritten. Bytes past the cursor in such lines are garbage until written, and
// the RDP never reads them (DPC_END is the cursor). The writers below only check the cursor
// against lim, which keeps each inlined copy small.
K64_INLINE void k64_rdp_put(u64 w) {
  if (K64_UNLIKELY(k64_rdp.cur >= k64_rdp.lim)) k64_rdp_more(1);
  *k64_rdp.cur++ = w;
}
// Two words (a two-word command, or two commands) with one room check.
K64_INLINE void k64_rdp_put2(u64 a, u64 b) {
  if (K64_UNLIKELY(k64_rdp.cur + 2 > k64_rdp.lim)) k64_rdp_more(2);
  k64_rdp.cur[0] = a; k64_rdp.cur[1] = b; k64_rdp.cur += 2;
}
// Reserve n words for a multi-word primitive written in place (triangles): the caller
// writes all n.
K64_INLINE u64 *k64_rdp_reserve(u32 n) {
  if (K64_UNLIKELY(k64_rdp.cur + n > k64_rdp.lim)) k64_rdp_more(n);
  u64 *p = k64_rdp.cur; k64_rdp.cur += n;
  return p;
}

// ---- state-tracked helpers (emit only the syncs the hardware actually needs) ------------
// Pipe sync is required before changing modes/colors/images when a primitive may still be
// in the pipeline; tile sync before re-describing a tile in use; load sync before
// re-loading TMEM in use. We track "a primitive was emitted since the last sync".
typedef struct {
  bool prim_pending;         // primitive emitted since last SYNC_PIPE
  bool tile_pending;         // primitive using tiles emitted since last SYNC_TILE
  bool load_pending;         // primitive using TMEM since last SYNC_LOAD
  u64  other_modes;
  u64  combine;
  u32  fill_color, prim_color, env_color, blend_color, fog_color;
  u32  color_image, z_image;
  u32  att_fmt, att_siz, att_w, att_h;   // attached color image
  u32  stat_syncs_pipe, stat_syncs_tile, stat_syncs_load, stat_prims;
  u32  stat_stall;            // CPU count ticks spent waiting for the RDP to take a block
} k64_rdp_state_t;
extern k64_rdp_state_t k64_rdps;

// SYNC_PIPE if a primitive went out since the last one. Each setter below calls it first and
// skips the command when the value is already set.
K64_INLINE void k64_rdp_pipe_guard(void) {
  if (k64_rdps.prim_pending) {
    k64_rdp_put(k64_rdpc_sync_pipe());
    k64_rdps.prim_pending = false;
    k64_rdps.stat_syncs_pipe++;
  }
}
K64_INLINE void k64_rdp_set_other_modes(u64 om) {
  if (om == k64_rdps.other_modes) return;
  k64_rdp_pipe_guard(); k64_rdps.other_modes = om; k64_rdp_put(k64_rdpc_other_modes(om));
}
K64_INLINE void k64_rdp_set_combine(u64 cc) {
  if (cc == k64_rdps.combine) return;
  k64_rdp_pipe_guard(); k64_rdps.combine = cc; k64_rdp_put(k64_rdpc_combine(cc));
}
K64_INLINE void k64_rdp_set_convert(int k0, int k1, int k2, int k3, int k4, int k5) {
  k64_rdp_pipe_guard(); k64_rdp_put(k64_rdpc_set_convert(k0, k1, k2, k3, k4, k5));
}
#define K64_RDP_COLOR_SETTER(name, field, enc)                                           \
  K64_INLINE void k64_rdp_set_##name(u32 c) {                                            \
    if (c == k64_rdps.field) return;                                                     \
    k64_rdp_pipe_guard(); k64_rdps.field = c; k64_rdp_put(enc);                          \
  }
K64_RDP_COLOR_SETTER(fill_color, fill_color, k64_rdpc_fill_color(c))
K64_RDP_COLOR_SETTER(env_color, env_color, k64_rdpc_env_color(c))
K64_RDP_COLOR_SETTER(blend_color, blend_color, k64_rdpc_blend_color(c))
K64_RDP_COLOR_SETTER(fog_color, fog_color, k64_rdpc_fog_color(c))
K64_INLINE void k64_rdp_set_prim_color(u32 c) {
  if (c == k64_rdps.prim_color) return;
  // PRIM color is latched per primitive (no pipe sync needed, n64brew/libdragon rdpq).
  k64_rdps.prim_color = c; k64_rdp_put(k64_rdpc_prim_color(c, 0, 0));
}
// Pixels, exclusive end. Fill/copy modes treat the end as inclusive: adjust for them.
K64_INLINE bool k64_rdp_mode_is_copyfill(void) { return (k64_rdps.other_modes & K64_OM_CYCLE_COPY) != 0; }
K64_INLINE void k64_rdp_fill_rect(int x0, int y0, int x1, int y1) {
  int adj = k64_rdp_mode_is_copyfill() ? 4 : 0;
  k64_rdp_put(k64_rdpc_fill_rect(x0 * 4, y0 * 4, x1 * 4 - adj, y1 * 4 - adj));
  k64_rdps.prim_pending = true; k64_rdps.stat_prims++;
}
// Sets the color image (and the Z image unless z is NULL) and a full-surface scissor;
// width x height pixels, KSEG0 or physical pointers.
void k64_rdp_attach(u32 fmt, u32 siz, u32 width, u32 height, void *color, void *z);
// Fill the attached color image with `fill` (16bpp: two packed pixels, e.g. k64_rgba16(c)*0x10001)
// and, if clear_z, the attached Z buffer with max depth. Leaves FILL mode set.
void k64_rdp_clear(u32 fill, bool clear_z);
