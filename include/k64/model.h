// kestrel64-sdk — skinned, animated models baked by `k64bake.py model` (tools/k64model.py).
//
// The blob carries a prebaked kgfx command list (MTX / LIGHT / VTX / TRI ... RET), the vertex
// buffer it loads, the skeleton, the sampled animation and a CI atlas texture. Skinning is
// done per "virtual bone": a pair of joints with a quantized weight (q/4 of joint a, the rest
// of joint b). Each frame the CPU poses the skeleton, then builds one matrix and one light
// block per virtual bone; the RSP transforms every vertex with its group's matrix.
//
//   k64_model_load(&m, blob);                 // once: patches the command list in place
//   k64_model_pose(&m, frame);                // animation frame (fractional, wraps)
//   k64_model_prepare(&m, &viewproj, &world, lights, n, ar, ag, ab);
//   k64_rdp_load_tex(0, 0, &m.tex, ...);      // CI: other modes need K64_OM_TLUT
//   k64_model_draw(&list, &m);                // CALL into the baked list
//
// Model space is centred on the skeleton root, Y up, about +-4096 units (hdr->lo / hi give
// the box over every frame); scale it through `world`. The matrices, light blocks and command
// list are double-buffered: each prepare fills the set the previous draw did not use, so the
// CPU can pose and prepare frame N+1 while the RSP draws frame N. It must not run while the
// RSP may still read the draw before that (one prepare/draw in flight at most).
#pragma once
#include "hw.h"
#include "fmath.h"
#include "kgfx.h"
#include "tex.h"

typedef struct {
  char magic[4];                          // 'K6M2'
  u16 njoints, nvb, nframes, fps;
  u32 ncmds, off_cmds, off_verts, nverts;
  u32 off_parent, off_invbind, off_root, off_vb, off_anim;
  float tscale;                           // animation translation = s16 * tscale
  u32 off_tex, ntris;
  float lo[3], hi[3];                     // model-space bounds over all frames
  u32 nlods, off_lods;                    // levels of detail (k64_model_lod_t[nlods], 0 = LOD 0)
} k64_model_hdr_t;

// Level of detail: its own vertices and command list (same skeleton, bones and atlas).
typedef struct { u32 ncmds, off_cmds, off_verts, nverts, ntris; } k64_model_lod_t;

#define K64_MODEL_LODS 4

typedef struct {
  const k64_model_hdr_t *hdr;
  u64 *cmds[K64_MODEL_LODS][2];           // per LOD: the blob's list and a copy, one per buffer set
  u32 nlods;
  const s8 *parent;                       // per joint: parent joint, -1 = root
  const k64_mat4_t *invbind, *root;
  const u8 *vb;                           // per virtual bone: joint a, joint b, q (0..4), 0
  const s16 *anim;                        // [frame][joint]: qx qy qz qw (Q15), tx ty tz, 0
  k64_tex_t tex;
  k64_mat4_t *skin;                       // per joint, model space (k64_model_pose)
  k64_kgfx_mtxlights_t *bones[2];         // per virtual bone (k64_model_prepare)
  k64_mat4_t *tmp;                        // 2 per joint: pose / prepare scratch
  u32 cur;                                // buffer set of the last prepare
} k64_model_t;

// `blob` must be in RAM, 16-byte aligned and stay there (it is patched in place and the RSP
// reads its vertices and commands). Allocates the per-frame buffers and the second command
// list from K64_ZONE_HEAP.
bool k64_model_load(k64_model_t *m, void *blob);
void k64_model_pose(k64_model_t *m, float frame);
void k64_model_prepare(k64_model_t *m, const k64_mat4_t *viewproj, const k64_mat4_t *world,
                       const k64_kgfx_light_t *lights, u32 n, u8 amb_r, u8 amb_g, u8 amb_b);
// Records a CALL to the command list of the last prepare. Uses all vertex cache slots and
// leaves the matrix and light block of its last group loaded.
K64_INLINE void k64_model_draw(k64_kgfx_list_t *l, const k64_model_t *m) { k64_kgfx_call(l, m->cmds[0][m->cur]); }
// Same with level of detail `lod` (0 = full; `k64bake.py model --lods`, m->nlods levels). The
// prepare is shared: every LOD reads the same matrices and light blocks.
K64_INLINE void k64_model_draw_lod(k64_kgfx_list_t *l, const k64_model_t *m, u32 lod) {
  k64_kgfx_call(l, m->cmds[lod < m->nlods ? lod : m->nlods - 1][m->cur]);
}
