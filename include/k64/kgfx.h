// kestrel64-sdk — kgfx: RSP geometry microcode, CPU side.
//
// The CPU records a command list (8-byte commands) in RDRAM; k64_kgfx_run() hands it to the
// RSP, which DMAs it in 256-byte chunks, transforms vertices into its DMEM vertex cache
// (32 entries) and emits RDP commands. Command encoding and formats: docs/KGFX.md.
//
// Rules: the list, matrices, viewports and vertex arrays must be 8-byte aligned (16 for
// vertices is best: one D$ line each) and stay untouched until k64_kgfx_wait() returns.
// k64_kgfx_run() writes the list back from D$; the data it points to must already be
// written back (k64_dcache_writeback) — or live in a buffer the CPU never caches.
#pragma once
#include "hw.h"
#include "fmath.h"
#include "debug.h"

#define K64_KGFX_VCACHE 32
#define K64_KGFX_CLIP_POOL 12    // entries after the cache: vertices made by the last clipped TRI

// s15.16 matrix, column-major m[col][row]: integer halves then fraction halves (64 bytes).
typedef struct K64_ALIGN(8) { s16 i[4][4]; u16 f[4][4]; } k64_mtxfx_t;

// Input vertex, 16 bytes. Position in model units (s16: scale through the matrix), color
// 8-bit RGBA, texture coordinates s10.5 texels. `norm` is the packed model-space normal
// (k64_kgfx_pack_normal), used when lighting is on; the color then is the material color.
typedef struct K64_ALIGN(8) { s16 x, y, z; u16 norm; u8 r, g, b, a; s16 s, t; } k64_vtx_t;

// Viewport: screen = trans + NDC * scale, per component (x, y, z) twice. x/y in s13.2
// pixels (so 4x the pixel values), z in 0..0x7FFE. Lane 3 holds the W normalize factor
// K = scale[3] + trans[3] / 65536 (k64_viewport_wnorm).
typedef struct K64_ALIGN(8) { s16 scale[8], trans[8]; } k64_viewport_t;

// Vertex cache entry as the microcode stores it (40 bytes, read back for debugging/tests).
typedef struct {
  s16 clip_i[4]; u16 clip_f[4];     // clip x, y, z, w (s15.16)
  s16 sx, sy, sz, invw_i;           // screen s13.2 / z / 1/w integer
  u16 invw_f, flags; u32 pad;       // 1/w fraction, K64_KGFX_CLIP_*
  u8 r, g, b, a; s16 s, t;
} k64_kgfx_vcache_t;
// Clip flags: outside a frustum plane (x, y, z against +-w) or the guard band (x, y against
// +-4w). A vertex behind the eye (w <= 0) is always outside the near or the far plane.
enum {
  K64_KGFX_CLIP_NX = 0x01, K64_KGFX_CLIP_NY = 0x02, K64_KGFX_CLIP_NEAR = 0x04,
  K64_KGFX_CLIP_PX = 0x10, K64_KGFX_CLIP_PY = 0x20, K64_KGFX_CLIP_FAR = 0x40,
  K64_KGFX_GUARD_NX = 0x100, K64_KGFX_GUARD_NY = 0x200,
  K64_KGFX_GUARD_PX = 0x1000, K64_KGFX_GUARD_PY = 0x2000,
  K64_KGFX_CLIP_REJECT = 0x77,    // all three vertices share one of these: triangle dropped
  K64_KGFX_CLIP_CUT = 0x3344,     // any vertex has one of these: triangle clipped on the RSP
};

// Command list being recorded.
typedef struct { u64 *base, *cur, *end; } k64_kgfx_list_t;

enum {
  K64_KGFX_OP_PARTICLES = 0x10,
  K64_KGFX_OP_MTX = 0x11, K64_KGFX_OP_VTX = 0x12, K64_KGFX_OP_TRI = 0x13,
  K64_KGFX_OP_VIEWPORT = 0x14, K64_KGFX_OP_GEOMMODE = 0x15, K64_KGFX_OP_LIGHT = 0x16,
  K64_KGFX_OP_TEXGEN = 0x17, K64_KGFX_OP_CULL = 0x18,
  K64_KGFX_OP_CALL = 0x19, K64_KGFX_OP_RET = 0x1A, K64_KGFX_OP_AUDIO = 0x1B, K64_KGFX_OP_ADPCM = 0x1C,
  K64_KGFX_OP_FOG = 0x1D,
  K64_KGFX_OP_RDP = 0x1E,
  K64_KGFX_OP_END = 0x1F,
};

// Float matrix to the microcode's s15.16 layout; elements beyond +-32768 saturate.
void k64_mtx_to_fixed(k64_mtxfx_t *out, const k64_mat4_t *m);
// Full-screen viewport for a w x h surface, depth mapped to 0..0x7FFE. W normalize K = 1.
void k64_viewport_set(k64_viewport_t *vp, u32 w, u32 h);
// W normalize for a perspective with planes near / far (as libultra's perspNorm): the
// microcode scales the clip-space vertex by K = 2 / (near + far) before the perspective
// divide, so that W sits around 1. Its 1/W is s15.16: for an unscaled W of thousands it
// keeps only a few bits, and every screen x / y / z then carries an error of several percent
// that differs per vertex (polygons cross each other as the camera moves). Any projection
// whose far plane is beyond a few hundred units needs it. NDC, clipping and fog (still
// given in the projection's units) are unchanged.
void k64_viewport_wnorm(k64_viewport_t *vp, float near, float far);

// Records into buf (words u64 commands; 8-byte aligned, KSEG0). k64_kgfx_cmd asserts on
// overflow. Every command builder below appends one command.
K64_INLINE void k64_kgfx_begin(k64_kgfx_list_t *l, u64 *buf, u32 words) {
  l->base = l->cur = buf; l->end = buf + words;
}
K64_INLINE void k64_kgfx_cmd(k64_kgfx_list_t *l, u32 hi, u32 lo) {
  K64_ASSERT(l->cur < l->end);
  *l->cur++ = ((u64)hi << 32) | lo;
}
// Loads the matrix that VTX applies (the whole model-view-projection: no stack).
K64_INLINE void k64_kgfx_mtx(k64_kgfx_list_t *l, const k64_mtxfx_t *m) {
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_MTX << 24, K64_PHYS(m));
}
K64_INLINE void k64_kgfx_viewport(k64_kgfx_list_t *l, const k64_viewport_t *vp) {
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_VIEWPORT << 24, K64_PHYS(vp));
}
// Loads `n` vertices into cache slots dest..dest+n-1.
K64_INLINE void k64_kgfx_vtx(k64_kgfx_list_t *l, const k64_vtx_t *v, u32 n, u32 dest) {
  K64_ASSERT(n && dest + n <= K64_KGFX_VCACHE);
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_VTX << 24) | (dest << 16) | (n << 8), K64_PHYS(v));
}
// Indexed vertex colours: each vertex's rgba is replaced by the word at byte offset `a` of
// the colour table, loaded by k64_kgfx_col from `offset` past the colour base (the base is the
// one of the CALL that runs the list, k64_kgfx_call_col; 0 outside any). The load starts at
// (base + offset) & ~7, so a vertex's `a` is ((base + offset) & 7) + 4 * colour index, up to
// 252. The table lives in cache slots 25..31 (K64_KGFX_COLTAB_SLOT): vertices loaded there
// overwrite it.
#define K64_KGFX_COLTAB_SLOT 25
K64_INLINE void k64_kgfx_vtx_colidx(k64_kgfx_list_t *l, const k64_vtx_t *v, u32 n, u32 dest) {
  K64_ASSERT(n && dest + n <= K64_KGFX_COLTAB_SLOT);
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_VTX << 24) | (dest << 16) | (n << 8) | 1, K64_PHYS(v));
}
// Loads the colour table read by k64_kgfx_vtx_colidx (see above).
K64_INLINE void k64_kgfx_col(k64_kgfx_list_t *l, u32 offset) {
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_VTX << 24, offset);
}
// Triangle mode: K64_TRI_* flags (tri.h), render tile, K64_KGFX_CULL_* (0 = draw both),
// K64_KGFX_LIGHTING. Screen y grows down; a back face has its vertices clockwise on screen.
// With K64_KGFX_LIGHTING, VTX lights the vertices with the last LIGHT block; with
// K64_KGFX_TEXGEN it replaces their s, t from the normal with the last TEXGEN block.
// `tile` may carry K64_KGFX_LEVELS(n) for a mipmapped texture (n tiles from the render tile on).
enum { K64_KGFX_CULL_BACK = 0x100, K64_KGFX_CULL_FRONT = 0x200, K64_KGFX_LIGHTING = 0x400,
       K64_KGFX_TEXGEN = 0x800 };
#define K64_KGFX_LEVELS(n) ((((u32)(n) - 1) & 7) << 3)
K64_INLINE void k64_kgfx_geommode(k64_kgfx_list_t *l, u32 tri_flags, u32 tile, u32 cull) {
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_GEOMMODE << 24) | (tile & 0x3F), (tri_flags & 7) | cull);
}

// ---- lighting ----
// color = vertex color * (ambient + sum over lights of max(N . L, 0) * light color), per
// channel, saturating at 2x the vertex color; alpha unchanged. N is the vertex normal in
// model space, L the light direction moved into model space by k64_kgfx_lights_set.
#define K64_KGFX_LIGHTS 4
typedef struct { k64_vec3_t dir; u8 r, g, b; } k64_kgfx_light_t;   // dir: towards the light, world space
// Microcode layout (160 bytes): light directions as columns (x, y, z of lights 0..3, twice,
// Q14), light colors (r, g, b, 0, Q15, twice), ambient (r, g, b, 1.0, Q14, twice), normal
// decode constants.
typedef struct K64_ALIGN(16) {
  s16 dir[3][8];
  s16 col[K64_KGFX_LIGHTS][8];
  s16 amb[8];
  u16 nshift[8], nmask[8];
} k64_kgfx_lights_t;

// Packs a unit normal as 5.6.5 bits (x 15..11, y 10..5, z 4..0; tiny3d's encoding).
u16 k64_kgfx_pack_normal(float x, float y, float z);
// Builds a light block for one object: `n` lights (<= K64_KGFX_LIGHTS), ambient color,
// and the object's model matrix (rotation + uniform scale; NULL = identity).
void k64_kgfx_lights_set(k64_kgfx_lights_t *out, const k64_kgfx_light_t *lights, u32 n,
                         u8 amb_r, u8 amb_g, u8 amb_b, const k64_mat4_t *model);
// Loads a light block (must stay valid until k64_kgfx_wait; written back like a matrix).
K64_INLINE void k64_kgfx_lights(k64_kgfx_list_t *l, const k64_kgfx_lights_t *lt) {
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_LIGHT << 24, K64_PHYS(lt));
}
// A matrix followed by its light block: one MTX command (flag K64_KGFX_MTX_LIGHTS) loads both,
// as k64_kgfx_mtx + k64_kgfx_lights would, for one dispatch less.
typedef struct K64_ALIGN(16) { k64_mtxfx_t mtx; k64_kgfx_lights_t lights; } k64_kgfx_mtxlights_t;
enum { K64_KGFX_MTX_LIGHTS = 1 };
K64_INLINE void k64_kgfx_mtx_lights(k64_kgfx_list_t *l, const k64_kgfx_mtxlights_t *ml) {
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_MTX << 24) | K64_KGFX_MTX_LIGHTS, K64_PHYS(ml));
}
// ---- texture coordinate generation ----
// Spherical environment mapping: s, t = the vertex normal's view-space x and -y, mapped from
// -1..1 to 0..tex_w / 0..tex_h texels. k64_kgfx_texgen_set takes the modelview matrix
// (view x model, rotation + uniform scale) and stores the view axes in model space, so the
// RSP works on the model-space normal. 112 bytes, 16-byte aligned.
typedef struct K64_ALIGN(16) { s16 dir[3][8]; s16 scale[8], off[8]; u16 nshift[8], nmask[8]; } k64_kgfx_texgen_t;
void k64_kgfx_texgen_set(k64_kgfx_texgen_t *out, const k64_mat4_t *modelview, u32 tex_w, u32 tex_h);
// Loads a texgen block (same lifetime rule as a light block).
K64_INLINE void k64_kgfx_texgen(k64_kgfx_list_t *l, const k64_kgfx_texgen_t *tg) {
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_TEXGEN << 24, K64_PHYS(tg));
}

// One or two triangles from vertex cache slots. The command carries each vertex as its DMEM
// address / 8 (0x20 + slot * 5: the cache is at 0x100, 40 bytes a slot), which the RSP turns
// into an address with one shift and a mask.
#define K64_KGFX_VREF(slot) (0x20u + (u32)(slot) * 5)
K64_INLINE void k64_kgfx_tri(k64_kgfx_list_t *l, u32 a, u32 b, u32 c) {
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_TRI << 24) | (K64_KGFX_VREF(a) << 16) | (K64_KGFX_VREF(b) << 8) | K64_KGFX_VREF(c),
               0);
}
K64_INLINE void k64_kgfx_tri2(k64_kgfx_list_t *l, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) {
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_TRI << 24) | (K64_KGFX_VREF(a) << 16) | (K64_KGFX_VREF(b) << 8) | K64_KGFX_VREF(c),
               (1u << 24) | (K64_KGFX_VREF(d) << 16) | (K64_KGFX_VREF(e) << 8) | K64_KGFX_VREF(f));
}
// Skip the commands up to k64_kgfx_cull_end when every vertex cache slot in `slots` (bit
// mask, loaded by earlier VTX commands) lies outside one and the same frustum plane. The
// skipped commands must only draw inside the hull of those vertices and change no state that
// later commands rely on. Typical: a bounding box or the corners of a planar patch, or the
// two vertex rows of a band.
K64_INLINE u64 *k64_kgfx_cull(k64_kgfx_list_t *l, u32 slots) {
  u64 *c = l->cur;
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_CULL << 24, slots);
  return c;
}
// Closes the skip range of the CULL command that k64_kgfx_cull returned.
K64_INLINE void k64_kgfx_cull_end(k64_kgfx_list_t *l, u64 *cull) {
  u32 n = (u32)(l->cur - cull - 1);
  K64_ASSERT(n < (1u << 24));
  *cull |= (u64)n << 32;
}
// Run the command list at `sub` (KSEG0, 8-byte aligned, written back, ending with RET), then
// continue here. One level: a called list cannot CALL.
K64_INLINE void k64_kgfx_call(k64_kgfx_list_t *l, const u64 *sub) {
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_CALL << 24, K64_PHYS(sub));
}
// CALL with a colour base for the k64_kgfx_col loads of the called list (KSEG0 or physical).
K64_INLINE void k64_kgfx_call_col(k64_kgfx_list_t *l, const u64 *sub, const void *colbase) {
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_CALL << 24) | (K64_PHYS(colbase) & 0xFFFFFF), K64_PHYS(sub));
}
// Ends a list run by CALL.
K64_INLINE void k64_kgfx_ret(k64_kgfx_list_t *l) { k64_kgfx_cmd(l, (u32)K64_KGFX_OP_RET << 24, 0); }
// Fog: VTX (after this command, until the end of the list) replaces each vertex alpha with
// a fog factor, 0 at eye distance `start` rising linearly to 255 at `end` (the integer part
// of clip w, in the projection's units: 0 <= start < end <= 32767); the blender mixes
// FOG_COLOR in by it (K64_OM_2C_FOG_SHADE, rdp.h). Every list starts with fog off, which
// keeps the vertex alpha. k64_kgfx_fog_max stops the ramp at `max` (1..255) instead: the
// factor reaches it at `end` and stays there.
void k64_kgfx_fog_max(k64_kgfx_list_t *l, s32 start, s32 end, u32 max);
K64_INLINE void k64_kgfx_fog(k64_kgfx_list_t *l, s32 start, s32 end) { k64_kgfx_fog_max(l, start, end, 255); }
K64_INLINE void k64_kgfx_fog_off(k64_kgfx_list_t *l) {
  k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_FOG << 24) | (0xFFu << 16), 0);
}
// Raw RDP commands (KSEG0, 8-byte aligned, written back, untouched until k64_kgfx_wait)
// sent in list order: mode / texture / rectangle commands between triangles. Up to 256 bytes
// per command; longer runs are split. The commands are not seen by the CPU's sync tracking
// (k64_rdps): they carry their own syncs.
K64_INLINE void k64_kgfx_rdp(k64_kgfx_list_t *l, const void *cmds, u32 bytes) {
  u32 a = K64_PHYS(cmds);
  for (; bytes; ) {
    u32 n = bytes > 256 ? 256 : bytes;
    k64_kgfx_cmd(l, ((u32)K64_KGFX_OP_RDP << 24) | (n - 1), a);
    a += n; bytes -= n;
  }
}

// Particles (docs/PARTICLES.md): screen-aligned squares around points sent through the
// current matrix, one RDP rectangle each, sized by perspective. 16 bytes per particle:
// center in model units, half extent `size` in model units (<= 32767), color (sent as
// PRIM_COLOR with K64_PART_COLOR), s, t = top-left texel of its texture square (s10.5).
typedef struct K64_ALIGN(8) { s16 x, y, z; u16 size; u8 r, g, b, a; s16 s, t; } k64_particle_t;
enum {
  K64_PART_TEX = 1,     // TEXRECT of the 2^tex_shift texel square at (s, t) on `tile`;
                        // else FILL_RECT (1-cycle: the combiner's color, e.g. PRIM)
  K64_PART_Z = 2,       // PRIM_DEPTH = the center's z (other modes: K64_OM_Z_SOURCE_PRIM)
  K64_PART_COLOR = 4,   // PRIM_COLOR = the particle's rgba (sent only when it changes)
};
// Block header (16 bytes, 16-aligned) followed by `count` particles.
typedef struct K64_ALIGN(16) {
  s32 scale;            // s15.16 model units -> s13.2 pixels at W = 1 (k64_kgfx_particles_scale)
  u16 count;
  u8 flags, tile, tex_shift, pad[7];
  k64_particle_t p[];
} k64_particles_t;
// `scale` for a model-view-projection matrix and the viewport the list uses (after
// k64_viewport_wnorm): the matrix's x scale times the viewport's.
s32 k64_kgfx_particles_scale(const k64_mat4_t *mvp, const k64_viewport_t *vp);
// Draws a block (KSEG0, written back, untouched until k64_kgfx_wait). Loads an IMEM overlay
// over the VTX code, so group particle blocks after the geometry when possible.
K64_INLINE void k64_kgfx_particles(k64_kgfx_list_t *l, const k64_particles_t *b) {
  k64_kgfx_cmd(l, (u32)K64_KGFX_OP_PARTICLES << 24, K64_PHYS(b));
}
// Y-axis billboard (trees, lamps): fills v[0..3] with a quad standing on `base` (model
// units), `half_w` wide each side and `height` tall, turned around the vertical axis to face
// `eye` (model units). Corners: 0 bottom-left, 1 bottom-right, 2 top-right, 3 top-left as
// seen from `eye`; texture (s0, t0) top-left .. (s1, t1) bottom-right, s10.5. Draw with
// k64_kgfx_vtx + k64_kgfx_tri2(l, 0, 1, 2, 0, 2, 3) (front face counter-clockwise).
// Normal = toward the camera, so lighting works too.
void k64_kgfx_billboard_y(k64_vtx_t v[4], k64_vec3_t base, float half_w, float height, k64_vec3_t eye,
                          u32 rgba, s16 s0, s16 t0, s16 s1, s16 t1);
K64_INLINE void k64_kgfx_end(k64_kgfx_list_t *l) { k64_kgfx_cmd(l, (u32)K64_KGFX_OP_END << 24, 0); }

// Loads the microcode if needed and starts the list (asynchronous). Triangles go straight
// into the RDP command stream (rdp.h): the stream is flushed and handed to the RSP, and the
// CPU must not emit RDP commands until k64_kgfx_wait() returns.
void k64_kgfx_run(const k64_kgfx_list_t *l);
// Same, raising the SP interrupt when the list ends (k64_kgfx_wait still takes the stream back).
void k64_kgfx_run_intr(const k64_kgfx_list_t *l);
// Waits for the end of the list and takes the RDP stream back. Returns the microcode status
// (0 = ok, 0x100|op = bad op).
u32 k64_kgfx_wait(void);
// Stats of the last list.
u32 k64_kgfx_tri_count(void);
u32 k64_kgfx_part_count(void);          // rectangles emitted by PARTICLES
// Copies vertex cache entries [first, first+n) to `out` (debug/tests; RSP must be idle).
// Entries K64_KGFX_VCACHE.. are the clip pool.
void k64_kgfx_read_vcache(k64_kgfx_vcache_t *out, u32 first, u32 n);
