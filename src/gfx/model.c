// kestrel64-sdk — baked skinned models (format: include/k64/model.h, baker: tools/k64model.py).
#include <k64/model.h>
#include <k64/mem.h>
#include <k64/cpu.h>
#include <k64/debug.h>

_Static_assert(sizeof(k64_model_hdr_t) == 92, "model header layout");

// out = a * b for affine matrices (bottom row 0 0 0 1).
static void affine_mul(k64_mat4_t *out, const k64_mat4_t *a, const k64_mat4_t *b) {
  k64_mat4_t t;
  for (int c = 0; c < 4; c++) {
    float b0 = b->m[c][0], b1 = b->m[c][1], b2 = b->m[c][2];
    for (int r = 0; r < 3; r++)
      t.m[c][r] = a->m[0][r] * b0 + a->m[1][r] * b1 + a->m[2][r] * b2 + (c == 3 ? a->m[3][r] : 0.f);
    t.m[c][3] = c == 3 ? 1.f : 0.f;
  }
  *out = t;
}

// out = a * b for a full matrix a and an affine b.
static void mul_affine(k64_mat4_t *out, const k64_mat4_t *a, const k64_mat4_t *b) {
  for (int c = 0; c < 4; c++) {
    float b0 = b->m[c][0], b1 = b->m[c][1], b2 = b->m[c][2];
    for (int r = 0; r < 4; r++)
      out->m[c][r] = a->m[0][r] * b0 + a->m[1][r] * b1 + a->m[2][r] * b2 + (c == 3 ? a->m[3][r] : 0.f);
  }
}

bool k64_model_load(k64_model_t *m, void *blob) {
  u8 *b = blob;
  const k64_model_hdr_t *h = blob;
  if (((u32)b & 15) || h->magic[0] != 'K' || h->magic[1] != '6' || h->magic[2] != 'M' || h->magic[3] != '2')
    return false;
  m->hdr = h;
  m->parent = (const s8*)(b + h->off_parent);
  m->invbind = (const k64_mat4_t*)(b + h->off_invbind);
  m->root = (const k64_mat4_t*)(b + h->off_root);
  m->vb = b + h->off_vb;
  m->anim = (const s16*)(b + h->off_anim);
  if (!k64_tex_from_blob(&m->tex, b + h->off_tex))
    return false;
  m->skin = k64_mem_alloc(K64_ZONE_HEAP, h->njoints * sizeof(k64_mat4_t), 16);
  m->tmp = k64_mem_alloc(K64_ZONE_HEAP, 2 * h->njoints * sizeof(k64_mat4_t), 16);
  const k64_model_lod_t *lod = (const k64_model_lod_t*)(b + h->off_lods);
  m->nlods = h->nlods;
  if (m->nlods < 1 || m->nlods > K64_MODEL_LODS)
    return false;
  for (u32 k = 0; k < 2; k++)
    m->bones[k] = k64_mem_alloc(K64_ZONE_HEAP, h->nvb * sizeof(k64_kgfx_mtxlights_t), 16);
  for (u32 d = 0; d < m->nlods; d++, lod++) {
    u32 n = lod->ncmds;
    m->cmds[d][0] = (u64*)(b + lod->off_cmds);
    m->cmds[d][1] = k64_mem_alloc(K64_ZONE_HEAP, n * sizeof(u64), 16);
    for (u32 i = 0; i < n; i++)
      m->cmds[d][1][i] = m->cmds[d][0][i];
    // The baked commands carry indices / offsets in their low word: turn them into addresses,
    // each list with its own matrices and light blocks.
    u32 verts = K64_PHYS(b + lod->off_verts);
    for (u32 k = 0; k < 2; k++) {
      u32 bones = K64_PHYS(m->bones[k]);
      u32 *w = (u32*)m->cmds[d][k];
      for (u32 i = 0; i < n; i++, w += 2) {
        switch (w[0] >> 24) {
        case K64_KGFX_OP_MTX:   w[1] = bones + w[1] * sizeof(k64_kgfx_mtxlights_t); break;
        case K64_KGFX_OP_LIGHT:
          w[1] = bones + w[1] * sizeof(k64_kgfx_mtxlights_t) + offsetof(k64_kgfx_mtxlights_t, lights);
          break;
        case K64_KGFX_OP_VTX:   w[1] += verts; break;
        }
      }
    }
    k64_dcache_writeback(m->cmds[d][1], n * sizeof(u64));
  }
  m->cur = 1;                             // the first prepare fills set 0
  k64_dcache_writeback(blob, h->off_tex);
  return true;
}

void k64_model_pose(k64_model_t *m, float frame) {
  const k64_model_hdr_t *h = m->hdr;
  u32 nj = h->njoints, nf = h->nframes;
  s32 fi = k64_floori(frame);
  float u = frame - (float)fi;
  fi %= (s32)nf;
  if (fi < 0) fi += (s32)nf;
  u32 f1 = (u32)fi + 1 == nf ? 0 : (u32)fi + 1;
  k64_mat4_t *scratch = m->tmp;
  const s16 *a0 = m->anim + (u32)fi * nj * 8, *a1 = m->anim + f1 * nj * 8;
  const float qs = 1.f / 32767.f;
  for (u32 j = 0; j < nj; j++, a0 += 8, a1 += 8) {
    // nlerp (the baker keeps consecutive frames on one hemisphere, also across the wrap
    // when the loop is seamless)
    float x = (a0[0] + (a1[0] - a0[0]) * u) * qs, y = (a0[1] + (a1[1] - a0[1]) * u) * qs;
    float z = (a0[2] + (a1[2] - a0[2]) * u) * qs, w = (a0[3] + (a1[3] - a0[3]) * u) * qs;
    if (a0[0] * a1[0] + a0[1] * a1[1] + a0[2] * a1[2] + a0[3] * a1[3] < 0) {   // wrap seam
      x = (a0[0] - (a1[0] + a0[0]) * u) * qs; y = (a0[1] - (a1[1] + a0[1]) * u) * qs;
      z = (a0[2] - (a1[2] + a0[2]) * u) * qs; w = (a0[3] - (a1[3] + a0[3]) * u) * qs;
    }
    float n = x * x + y * y + z * z + w * w;
    float s = n > 0 ? 2.f / n : 0;
    k64_mat4_t l;
    l.m[0][0] = 1 - s * (y * y + z * z); l.m[0][1] = s * (x * y + z * w);     l.m[0][2] = s * (x * z - y * w);
    l.m[1][0] = s * (x * y - z * w);     l.m[1][1] = 1 - s * (x * x + z * z); l.m[1][2] = s * (y * z + x * w);
    l.m[2][0] = s * (x * z + y * w);     l.m[2][1] = s * (y * z - x * w);     l.m[2][2] = 1 - s * (x * x + y * y);
    l.m[0][3] = l.m[1][3] = l.m[2][3] = 0;
    l.m[3][0] = (a0[4] + (a1[4] - a0[4]) * u) * h->tscale;
    l.m[3][1] = (a0[5] + (a1[5] - a0[5]) * u) * h->tscale;
    l.m[3][2] = (a0[6] + (a1[6] - a0[6]) * u) * h->tscale;
    l.m[3][3] = 1;
    s32 p = m->parent[j];
    affine_mul(&scratch[j], p < 0 ? m->root : &scratch[p], &l);
    affine_mul(&m->skin[j], &scratch[j], &m->invbind[j]);
  }
}

void k64_model_prepare(k64_model_t *m, const k64_mat4_t *viewproj, const k64_mat4_t *world,
                       const k64_kgfx_light_t *lights, u32 n, u8 amb_r, u8 amb_g, u8 amb_b) {
  // Linear blend skinning is linear in the matrices, so each joint is taken to clip space
  // once (P = viewproj * world * skin) and the virtual bones only blend: 16 + 9 multiply-adds
  // per virtual bone instead of a matrix product. W (world * skin) gives the light rotation.
  const k64_model_hdr_t *h = m->hdr;
  u32 nj = h->njoints;
  k64_mat4_t *W = m->tmp, *P = m->tmp + nj, vpw;
  m->cur ^= 1;
  k64_kgfx_mtxlights_t *bo = m->bones[m->cur];
  k64_mat4_mul(&vpw, viewproj, world);
  for (u32 j = 0; j < nj; j++) {
    affine_mul(&W[j], world, &m->skin[j]);
    mul_affine(&P[j], &vpw, &m->skin[j]);
  }
  const u8 *v = m->vb;
  for (u32 i = 0; i < h->nvb; i++, v += 4) {
    const k64_mat4_t *pa = &P[v[0]], *wa = &W[v[0]];
    if (v[2] >= 4) {
      k64_kgfx_lights_set(&bo[i].lights, lights, n, amb_r, amb_g, amb_b, wa);
      k64_mtx_to_fixed(&bo[i].mtx, pa);
      continue;
    }
    const k64_mat4_t *pb = &P[v[1]], *wb = &W[v[1]];
    float ka = v[2] * 0.25f, kb = 1.f - ka;
    k64_mat4_t b;
    for (int c = 0; c < 3; c++)
      for (int r = 0; r < 3; r++)
        b.m[c][r] = wa->m[c][r] * ka + wb->m[c][r] * kb;
    k64_kgfx_lights_set(&bo[i].lights, lights, n, amb_r, amb_g, amb_b, &b);
    for (int c = 0; c < 4; c++)
      for (int r = 0; r < 4; r++)
        b.m[c][r] = pa->m[c][r] * ka + pb->m[c][r] * kb;
    k64_mtx_to_fixed(&bo[i].mtx, &b);
  }
  k64_dcache_writeback(bo, h->nvb * sizeof(k64_kgfx_mtxlights_t));
}
