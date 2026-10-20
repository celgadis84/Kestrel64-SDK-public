// kestrel64-sdk — impostors: far models drawn as one textured quad (docs/PARTICLES.md).
//
// tools/k64bake.py impostor renders a glTF model from N directions around its vertical axis
// into N small textures (.k64i, IMPOSTORS in k64_add_rom). At run time each instance is a Y
// billboard (k64_kgfx_billboard_y) through the model's axis, textured with the view baked
// closest to the direction it is seen from: 2 triangles instead of the whole mesh, and the
// texture switches as the camera walks around it. Draw with alpha compare (the cells are
// cut out with alpha 0/1), z buffer and K64_CC_TEX_SHADE (the vertex color tints the baked
// lighting; white leaves it as baked).
#pragma once
#include "kgfx.h"
#include "tex.h"

#define K64_IMPOSTOR_MAX_VIEWS 32

typedef struct {
  u32 views;
  float half_w, height, y0, cx, cz;      // quad of the model at scale 1, model units
  u32 tile, tmem;                        // where the views load
  const u8 *cells;
  struct k64_impostor_view_s *v;         // per view: texture, load commands, direction
} k64_impostor_t;

// One drawn copy: model-to-world = translate(pos) * rotate_y(yaw) * scale(scale).
typedef struct { k64_vec3_t pos; float yaw, scale; } k64_impostor_inst_t;

// Parses a resident .k64i blob (KSEG0, 8-byte aligned, kept) and builds each view's TMEM
// load for render tile `tile` at TMEM byte `tmem` (allocated once from K64_ZONE_TEX).
// Returns false on a bad blob.
bool k64_impostor_init(k64_impostor_t *im, const void *blob, u32 tile, u32 tmem);
// View seen from `eye` (world) for an instance: the baked direction closest to the eye's.
u32 k64_impostor_view(const k64_impostor_t *im, const k64_impostor_inst_t *in, k64_vec3_t eye);
const k64_tex_t *k64_impostor_tex(const k64_impostor_t *im, u32 view);
// Quad of an instance (corners as k64_kgfx_billboard_y, texture = the whole view cell).
void k64_impostor_quad(const k64_impostor_t *im, k64_vtx_t v[4], const k64_impostor_inst_t *in,
                       k64_vec3_t eye, u32 rgba);
// Loads a view into TMEM from inside a kgfx list (raw RDP commands with their syncs).
void k64_impostor_load(k64_kgfx_list_t *l, const k64_impostor_t *im, u32 view);
// Whole batch: picks each instance's view, groups the quads by view into `vbuf` (n * 4
// vertices, KSEG0, 16-byte aligned, untouched until k64_kgfx_wait; written back here) and
// records one load per view in use plus VTX/TRI2 for the quads (vertex cache entries 0..31
// are overwritten). Call k64_tex_tmem_forget() after the list's k64_kgfx_wait(). Returns the
// number of views loaded.
u32 k64_impostor_draw(k64_kgfx_list_t *l, const k64_impostor_t *im, const k64_impostor_inst_t *in, u32 n,
                      k64_vec3_t eye, u32 rgba, k64_vtx_t *vbuf);
