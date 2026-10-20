// kestrel64-sdk — impostors (.k64i views on Y billboards).
#include <k64/impostor.h>
#include <k64/mem.h>
#include <k64/cpu.h>

struct k64_impostor_view_s {
  u64 cmds[K64_TEX_LOAD_CMDS];             // TMEM load of this view (k64_tex_load_cmds)
  u32 ncmds;
  float dx, dz;                            // direction from the model toward the camera
  k64_tex_t tex;
};

static u32 rd32(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static float rdf(const u8 *p) { union { u32 u; float f; } x = { rd32(p) }; return x.f; }

bool k64_impostor_init(k64_impostor_t *im, const void *blob, u32 tile, u32 tmem) {
  const u8 *b = blob;
  u32 views = (u32)((b[4] << 8) | b[5]), stride = rd32(b + 8);
  if (rd32(b) != 0x4B364931 || !views || views > K64_IMPOSTOR_MAX_VIEWS) return false;   // 'K6I1'
  im->views = views;
  im->half_w = rdf(b + 12); im->height = rdf(b + 16); im->y0 = rdf(b + 20);
  im->cx = rdf(b + 24); im->cz = rdf(b + 28);
  im->tile = tile; im->tmem = tmem;
  im->cells = b + 32;
  im->v = k64_mem_alloc(K64_ZONE_TEX, views * sizeof *im->v, 16);
  for (u32 k = 0; k < views; k++) {
    struct k64_impostor_view_s *v = &im->v[k];
    if (!k64_tex_from_blob(&v->tex, im->cells + k * stride)) return false;
    v->ncmds = k64_tex_load_cmds(v->cmds, tile, tmem, &v->tex, K64_TEX_CLAMP, K64_TEX_CLAMP);
    float phi = (2 * K64_PI / views) * k;
    v->dx = k64_sinf(phi); v->dz = k64_cosf(phi);
  }
  k64_dcache_writeback(im->v, views * sizeof *im->v);
  return true;
}

// The instance's axis point (bottom of the quad) in world space.
static void axis(const k64_impostor_t *im, const k64_impostor_inst_t *in, float c, float s, k64_vec3_t *base) {
  float x = im->cx * in->scale, z = im->cz * in->scale;
  base->x = in->pos.x + c * x + s * z;
  base->y = in->pos.y + im->y0 * in->scale;
  base->z = in->pos.z - s * x + c * z;
}

// View whose direction, turned by the instance's yaw, is closest to the eye's: the eye
// direction is turned back into model space (rotate_y(-yaw)) and dotted with each view's.
static u32 pick(const k64_impostor_t *im, float c, float s, k64_vec3_t base, k64_vec3_t eye) {
  float ex = eye.x - base.x, ez = eye.z - base.z;
  float lx = c * ex - s * ez, lz = s * ex + c * ez;       // rotate_y(-yaw)
  u32 best = 0;
  float bd = -1e30f;
  for (u32 k = 0; k < im->views; k++) {
    float d = im->v[k].dx * lx + im->v[k].dz * lz;
    if (d > bd) { bd = d; best = k; }
  }
  return best;
}

u32 k64_impostor_view(const k64_impostor_t *im, const k64_impostor_inst_t *in, k64_vec3_t eye) {
  float c = k64_cosf(in->yaw), s = k64_sinf(in->yaw);
  k64_vec3_t base;
  axis(im, in, c, s, &base);
  return pick(im, c, s, base, eye);
}

const k64_tex_t *k64_impostor_tex(const k64_impostor_t *im, u32 view) { return &im->v[view].tex; }

static void quad(const k64_impostor_t *im, k64_vtx_t v[4], const k64_impostor_inst_t *in, u32 view,
                 k64_vec3_t base, k64_vec3_t eye, u32 rgba) {
  const k64_tex_t *t = &im->v[view].tex;
  k64_kgfx_billboard_y(v, base, im->half_w * in->scale, im->height * in->scale, eye, rgba,
                       0, 0, (s16)(t->width << 5), (s16)(t->height << 5));
}

void k64_impostor_quad(const k64_impostor_t *im, k64_vtx_t v[4], const k64_impostor_inst_t *in,
                       k64_vec3_t eye, u32 rgba) {
  float c = k64_cosf(in->yaw), s = k64_sinf(in->yaw);
  k64_vec3_t base;
  axis(im, in, c, s, &base);
  quad(im, v, in, pick(im, c, s, base, eye), base, eye, rgba);
}

void k64_impostor_load(k64_kgfx_list_t *l, const k64_impostor_t *im, u32 view) {
  k64_kgfx_rdp(l, im->v[view].cmds, im->v[view].ncmds * 8);
}

u32 k64_impostor_draw(k64_kgfx_list_t *l, const k64_impostor_t *im, const k64_impostor_inst_t *in, u32 n,
                      k64_vec3_t eye, u32 rgba, k64_vtx_t *vbuf) {
  u16 count[K64_IMPOSTOR_MAX_VIEWS + 1] = { 0 };
  u8 view[256];
  u32 loads = 0;
  for (u32 done = 0; done < n; ) {
    // in steps of 256 instances (the view scratch): quads in instance order, then moved
    // in place into view order (cells all have the same size, so a quad does not depend
    // on its view)
    u32 m = n - done > 256 ? 256 : n - done;
    for (u32 k = 0; k <= im->views; k++) count[k] = 0;
    k64_vtx_t *vb = vbuf + done * 4;
    for (u32 i = 0; i < m; i++) {
      const k64_impostor_inst_t *p = &in[done + i];
      float c = k64_cosf(p->yaw), s = k64_sinf(p->yaw);
      k64_vec3_t base;
      axis(im, p, c, s, &base);
      view[i] = (u8)pick(im, c, s, base, eye);
      count[view[i] + 1]++;
      quad(im, vb + i * 4, p, view[i], base, eye, rgba);
    }
    for (u32 k = 0; k < im->views; k++) count[k + 1] += count[k];   // count[k] = first slot of view k
    u16 fill[K64_IMPOSTOR_MAX_VIEWS];
    for (u32 k = 0; k < im->views; k++) fill[k] = count[k];
    for (u32 i = 0; i < m; i++) view[i] = (u8)fill[view[i]]++;       // view[i] = slot of quad i
    for (u32 i = 0; i < m; i++)
      while (view[i] != i) {                                         // follow the cycle
        u32 j = view[i];
        k64_vtx_t t[4];
        for (int k = 0; k < 4; k++) { t[k] = vb[j * 4 + k]; vb[j * 4 + k] = vb[i * 4 + k]; vb[i * 4 + k] = t[k]; }
        view[i] = view[j]; view[j] = (u8)j;
      }
    k64_dcache_writeback(vb, m * 4 * sizeof *vb);
    for (u32 k = 0; k < im->views; k++) {
      u32 q0 = count[k], q1 = count[k + 1];
      if (q0 == q1) continue;
      k64_impostor_load(l, im, k);
      loads++;
      for (u32 q = q0; q < q1; q += K64_KGFX_VCACHE / 4) {
        u32 nq = q1 - q > K64_KGFX_VCACHE / 4 ? K64_KGFX_VCACHE / 4 : q1 - q;
        k64_kgfx_vtx(l, vb + q * 4, nq * 4, 0);
        for (u32 j = 0; j < nq * 4; j += 4) k64_kgfx_tri2(l, j, j + 1, j + 2, j, j + 2, j + 3);
      }
    }
    done += m;
  }
  return loads;
}
