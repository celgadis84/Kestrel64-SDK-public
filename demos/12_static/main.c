// Demo 12 — a static (unskinned) glTF scene on kgfx.
//
// shrine.glb comes from make_scene.py (committed, regenerate with it): a node hierarchy with
// rotation, non-uniform and mirrored scales, a textured material and flat colours, a mesh
// without normals. The baker (k64bake.py model) bakes every node transform into the vertices:
// one identity joint, one virtual bone, so the command list holds a single MTX + LIGHT and
// then only VTX / TRI. Per frame the CPU builds that one matrix and light block (the camera
// orbits); the list is CALLed.
//
// Fog (kgfx FOG + 2-cycle blender, K64_OM_2C_FOG_SHADE) fades the far side into the clear
// colour.
//
// A stops the orbit, B toggles the fog.
#include <k64/k64.h>
#include <k64/model.h>

extern u8 shrine[];

static k64_viewport_t vp K64_ALIGN(16);
static u64 list_buf[64] K64_ALIGN(16);

static const k64_kgfx_light_t lt[2] = {
  { { 0.4f, 0.9f, 0.5f }, 235, 225, 205 },
  { { -0.6f, 0.2f, -0.7f }, 60, 80, 120 },
};

static void prepare(k64_model_t *m, const k64_mat4_t *proj, float orbit, u32 *prep_us) {
  const k64_model_hdr_t *h = m->hdr;
  float cy = (h->lo[1] + h->hi[1]) * 0.5f;
  k64_mat4_t view, viewproj, world;
  float r = 7000.f;
  k64_mat4_lookat(&view, (k64_vec3_t){ r * k64_sinf(orbit), cy + 1500.f, r * k64_cosf(orbit) },
                  (k64_vec3_t){ 0, cy - 300.f, 0 }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, proj, &view);
  k64_mat4_identity(&world);
  u32 t = k64_count();
  k64_model_prepare(m, &viewproj, &world, lt, 2, 55, 58, 68);
  *prep_us = K64_US(k64_count() - t);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_viewport_wnorm(&vp, 1500.f, 20000.f);       // the projection's near / far
  k64_dcache_writeback(&vp, sizeof vp);

  k64_model_t m;
  K64_ASSERT(k64_model_load(&m, shrine));
  const k64_model_hdr_t *h = m.hdr;
  k64_printf("[12] model: %u tris, %u verts, %u joints, %u virtual bones, %u frames, %u commands\n",
             h->ntris, h->nverts, h->njoints, h->nvb, h->nframes, h->ncmds);
  k64_model_pose(&m, 0);                          // static: the one pose, once
  k64_mat4_t proj;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1500.f, 20000.f);

  bool turn = true, fog = true;
  float orbit = 0.6f;
  u32 cpu_us = 0, rsp_us = 0, rdp_clk = 0, tris = 0, prep_us = 0, frame_us = 0;
  u32 tf = k64_count();
  prepare(&m, &proj, orbit, &prep_us);
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    u32 t0 = k64_count(), c0 = k64_rdp_clock();
    frame_us = K64_US(t0 - tf);
    tf = t0;

    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(60, 80, 110, 255) * 0x10001u, true);
    const u64 tex_om = K64_OM_ZBUF | K64_OM_PERSP_TEX | K64_OM_SAMPLE_BILINEAR | K64_OM_TLUT;
    if (fog) {
      k64_rdp_set_fog_color(0x3C506EFFu);         // the clear colour
      k64_rdp_set_other_modes(K64_OM_2C_FOG_SHADE | tex_om);
      k64_rdp_set_combine(K64_CC_2C_TEX_SHADE);
    } else {
      k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | tex_om);
      k64_rdp_set_combine(K64_CC_TEX_SHADE);
    }
    k64_rdp_load_tex(0, 0, &m.tex, K64_TEX_CLAMP, K64_TEX_CLAMP);

    k64_kgfx_list_t l;
    k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
    k64_kgfx_viewport(&l, &vp);
    k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z | K64_TRI_TEX, 0, K64_KGFX_CULL_BACK | K64_KGFX_LIGHTING);
    if (fog) k64_kgfx_fog(&l, 6000, 12000);       // eye distance, the projection's units
    k64_model_draw(&l, &m);
    k64_kgfx_end(&l);
    k64_kgfx_run(&l);

    u32 t1 = k64_count();
    if (turn) orbit += 0.008f;
    prepare(&m, &proj, orbit, &prep_us);
    u32 t2 = k64_count();
    u32 st = k64_kgfx_wait();
    rsp_us = K64_US(k64_count() - t1);
    cpu_us = K64_US(t2 - t1);
    tris = k64_kgfx_tri_count();
    if (st) k64_printf("[12] kgfx status %x\n", st);

    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  12 static");
    k64_text_printf(8, 20, 0xFFE040FF, "tris %u  cpu %4u us  rsp %5u us", tris, cpu_us, rsp_us);
    k64_text_printf(8, 32, 0xFFE040FF, "rdp %6u clk  frame %5u us  fog %s", rdp_clk, frame_us, fog ? "on" : "off");
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    rdp_clk = (k64_rdp_clock() - c0) & 0xFFFFFF;
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[12] frame %u tris %u cpu %u us (prepare %u) rsp %u us rdp %u clk frame %u us\n",
                 frame, tris, cpu_us, prep_us, rsp_us, rdp_clk, frame_us);
    if (k64_pads[0].pressed & K64_PAD_A) turn = !turn;
    if (k64_pads[0].pressed & K64_PAD_B) fog = !fog;
  }
}
