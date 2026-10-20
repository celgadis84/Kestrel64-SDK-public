// Demo 10 — a skinned, animated T-Rex on kgfx.
//
// Model: "Tyrannosaurus Rex Lowpoly" by pikabobalex, CC BY 4.0
// (https://sketchfab.com/3d-models/tyrannosaurus-rex-lowpoly-516dbe9717554e74b0bfcbb77a431b17),
// redistributed via LibSaturn — assets/trex/LICENSE.txt.
//
// The baker (k64bake.py model) decimates the 10.9k-triangle glTF to 2.4k, packs its textures
// into one 64x64 CI4 atlas, splits the skin into "virtual bones" (two joints, weight in
// quarters) and prebakes the whole draw as a kgfx command list. Per frame the CPU poses 41
// joints and builds one matrix + light block per virtual bone; the list is CALLed. The model's
// buffers are double-buffered, so the CPU poses and prepares frame N+1 while the RSP draws
// frame N.
//
// Levels of detail (--lods 1600,1100,600): the camera distance picks LOD 0 / 1 / 2 / 3
// (thresholds 14000 / 21000 / 25500), all four drawn from the same prepare.
//
// A pauses the animation, B stops the turntable, C-up / C-down move the camera, R cycles the
// LOD choice (auto, 0, 1, 2, 3).
#include <k64/k64.h>
#include <k64/model.h>

extern u8 trex[];

static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[64] K64_ALIGN(16);

static const k64_kgfx_light_t lt[2] = {
  { { 0.4f, 0.9f, 0.5f }, 240, 225, 200 },
  { { -0.6f, 0.1f, -0.7f }, 70, 90, 130 },
};

// Poses and prepares one frame; returns the pose / prepare times.
static void prepare(k64_model_t *m, const k64_mat4_t *viewproj, float anim, float yaw,
                    u32 *pose_us, u32 *prep_us) {
  const k64_model_hdr_t *h = m->hdr;
  float cx = (h->lo[0] + h->hi[0]) * 0.5f, cy = (h->lo[1] + h->hi[1]) * 0.5f, cz = (h->lo[2] + h->hi[2]) * 0.5f;
  k64_mat4_t rot, ctr, world;
  k64_mat4_rotate_y(&rot, yaw);
  k64_mat4_translate(&ctr, -cx, -cy, -cz);
  k64_mat4_mul(&world, &rot, &ctr);
  u32 tp = k64_count();
  k64_model_pose(m, anim);
  u32 tq = k64_count();
  k64_model_prepare(m, viewproj, &world, lt, 2, 60, 62, 72);
  *pose_us = K64_US(tq - tp);
  *prep_us = K64_US(k64_count() - tq);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_viewport_wnorm(&vp, 300.f, 30000.f);        // the projection's near / far
  k64_dcache_writeback(&vp, sizeof vp);

  k64_model_t m;
  K64_ASSERT(k64_model_load(&m, trex));
  const k64_model_hdr_t *h = m.hdr;
  k64_printf("[10] model: %u tris, %u verts, %u joints, %u virtual bones, %u frames @ %u fps\n",
             h->ntris, h->nverts, h->njoints, h->nvb, h->nframes, h->fps);
  for (u32 i = 1; i < m.nlods; i++) {
    const k64_model_lod_t *d = (const k64_model_lod_t*)((const u8*)h + h->off_lods) + i;
    k64_printf("[10] LOD %u: %u tris, %u verts\n", i, d->ntris, d->nverts);
  }
  k64_mat4_t proj, view, viewproj;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 300.f, 30000.f);
  float dist = 10500.f;
  s32 lod_mode = -1;                                // -1 = by distance

  bool animate = true, turn = true;
  float anim = 0, yaw = 0.9f;
  u32 cpu_us = 0, rsp_us = 0, rdp_clk = 0, tris = 0, pose_us = 0, prep_us = 0, frame_us = 0;
  u32 tf = k64_count();
  k64_mat4_lookat(&view, (k64_vec3_t){0, dist * (900.f / 10500.f), dist}, (k64_vec3_t){0, -300, 0},
                  (k64_vec3_t){0, 1, 0});
  k64_mat4_mul(&viewproj, &proj, &view);
  prepare(&m, &viewproj, anim, yaw, &pose_us, &prep_us);
  u32 lod = 0;
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    u32 t0 = k64_count(), c0 = k64_rdp_clock();
    frame_us = K64_US(t0 - tf);
    tf = t0;

    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(52, 60, 76, 255) * 0x10001u, true);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF | K64_OM_PERSP_TEX | K64_OM_SAMPLE_BILINEAR | K64_OM_TLUT);
    k64_rdp_set_combine(K64_CC_TEX_SHADE);
    k64_rdp_load_tex(0, 0, &m.tex, K64_TEX_CLAMP, K64_TEX_CLAMP);

    k64_kgfx_list_t l;
    k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
    k64_kgfx_viewport(&l, &vp);
    k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z | K64_TRI_TEX, 0, K64_KGFX_CULL_BACK | K64_KGFX_LIGHTING);
    k64_model_draw_lod(&l, &m, lod);
    k64_kgfx_end(&l);
    k64_kgfx_run(&l);

    // Next frame's pose and buffers while the RSP draws this one (the input is one frame old).
    u32 t1 = k64_count();
    if (animate) anim += 1.f;                       // 30 fps animation, one step per frame
    if (turn) yaw += 0.01f;
    u32 held = k64_pads[0].held;
    if (held & K64_PAD_CUP) dist = dist > 6000.f ? dist - 150.f : dist;
    if (held & K64_PAD_CDOWN) dist = dist < 28000.f ? dist + 150.f : dist;
    k64_mat4_lookat(&view, (k64_vec3_t){0, dist * (900.f / 10500.f), dist}, (k64_vec3_t){0, -300, 0},
                    (k64_vec3_t){0, 1, 0});
    k64_mat4_mul(&viewproj, &proj, &view);
    prepare(&m, &viewproj, anim, yaw, &pose_us, &prep_us);
    u32 next_lod = lod_mode >= 0 ? (u32)lod_mode : dist > 25500.f ? 3 : dist > 21000.f ? 2 : dist > 14000.f ? 1 : 0;
    u32 t2 = k64_count();
    u32 st = k64_kgfx_wait();
    rsp_us = K64_US(k64_count() - t1);
    cpu_us = K64_US(t2 - t1);
    tris = k64_kgfx_tri_count();
    if (st) k64_printf("[10] kgfx status %x\n", st);

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  10 t-rex");
    k64_text_printf(8, 20, 0xFFE040FF, "tris %u  cpu %5u us  rsp %5u us", tris, cpu_us, rsp_us);
    k64_text_printf(8, 32, 0xFFE040FF, "rdp %6u clk  frame %5u us", rdp_clk, frame_us);
    k64_text_printf(8, 44, 0xFFE040FF, "lod %u (%s)  dist %5d", lod, lod_mode < 0 ? "auto" : "fixed", (s32)dist);
    k64_text_printf(8, 212, 0xC0C0C0FF, "\"Tyrannosaurus Rex Lowpoly\" by");
    k64_text_printf(8, 222, 0xC0C0C0FF, "pikabobalex - CC BY 4.0 (Sketchfab)");
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    lod = next_lod;
    if ((frame & 63) == 63)
      k64_printf(
          "[10] frame %u tris %u cpu %u us (pose %u prepare %u, under the rsp) rsp %u us rdp %u clk frame %u us\n",
          frame, tris, cpu_us, pose_us, prep_us, rsp_us, rdp_clk, frame_us);
    if (k64_pads[0].pressed & K64_PAD_A) animate = !animate;
    if (k64_pads[0].pressed & K64_PAD_B) turn = !turn;
    if (k64_pads[0].pressed & K64_PAD_R) lod_mode = lod_mode >= 3 ? -1 : lod_mode + 1;
  }
}
