// Demo 11 — audio: a few voices (looped waveforms at several pitches, a one-shot drum, and two
// VADPCM samples: a plucked string one-shot and a looped organ, decoded by the kgfx ADPCM
// overlay) mixed by the kgfx AUDIO overlay on the RSP into AI buffers. Every buffer is mixed a second
// time by the CPU reference (k64_mixer_run_cpu) and compared sample for sample; the screen
// shows the result, the RSP/CPU mixing times and a scope of the last buffer. The AUDIO command
// shares its kgfx list with a spinning cube drawn after it (the overlay slot goes back to VTX).
#include <k64/k64.h>

#define RATE    32000
#define FRAMES  544                        // >= RATE / 60, multiple of 4
#define SINE_N  256
#define SQR_N   64
#define SAW_N   100
#define DRUM_N  3000
#define ADPCM_RECS 40                      // per ADPCM voice: <= 10 groups, <= 3 parts (organ loop 240)
#define MAXREC  (K64_MIXER_VOICES * ((FRAMES + 127) / 128) * 2 + (FRAMES + 127) / 128 + 1 +                  2 * ADPCM_RECS + 1)

static s16 sine[SINE_N + 1] K64_ALIGN(16);
static s16 sqr[SQR_N + 1] K64_ALIGN(16);
static s16 saw[SAW_N + 1] K64_ALIGN(16);
static s16 drum[DRUM_N + 1] K64_ALIGN(16);
static k64_mixrec_t recs[MAXREC] K64_ALIGN(16);
static s16 rsp_out[FRAMES * 2] K64_ALIGN(16);
static u64 list_buf[24] K64_ALIGN(16);
static k64_mtxfx_t mtx K64_ALIGN(16);
static k64_viewport_t vp K64_ALIGN(16);

#define U 256
static const k64_vtx_t cube_v[8] K64_ALIGN(16) = {
  {-U,-U,-U,0, 255, 40, 40,255, 0,0}, { U,-U,-U,0,  40,255, 40,255, 0,0},
  { U, U,-U,0,  40, 40,255,255, 0,0}, {-U, U,-U,0, 255,255, 40,255, 0,0},
  {-U,-U, U,0, 255, 40,255,255, 0,0}, { U,-U, U,0,  40,255,255,255, 0,0},
  { U, U, U,0, 255,255,255,255, 0,0}, {-U, U, U,0,  60, 60, 60,255, 0,0},
};
static const u8 cube_i[12][3] = {
  {0,2,1},{0,3,2}, {4,5,6},{4,6,7}, {0,1,5},{0,5,4}, {3,6,2},{3,7,6}, {0,4,7},{0,7,3}, {1,2,6},{1,6,5},
};
static k64_mixer_t mixer;
extern const u8 pluck[], organ[];          // SOUNDS (make_sounds.py, k64bake.py adpcm)
static k64_adpcm_t pluck_a, organ_a;
static k64_adpcm_voice_t dec[2];

// C major scale, semitones over C4.
static const u8 melody[16] = { 0, 4, 7, 12, 11, 7, 4, 2, 0, 5, 9, 12, 14, 12, 7, 4 };

static u32 note_step(u32 table_len, float semitones_over_c4) {
  float hz = 261.63f;
  for (float s = semitones_over_c4; s >= 1.f; s -= 1.f) hz *= 1.0594631f;
  return (u32)(hz * table_len / RATE * 65536.f);
}

static void make_samples(void) {
  for (u32 i = 0; i < SINE_N; i++) sine[i] = (s16)(k64_sinf(i * (2.f * K64_PI / SINE_N)) * 30000.f);
  sine[SINE_N] = sine[0];
  for (u32 i = 0; i < SQR_N; i++) sqr[i] = i < SQR_N / 2 ? 20000 : -20000;
  sqr[SQR_N] = sqr[0];
  for (u32 i = 0; i < SAW_N; i++) saw[i] = (s16)(-32768 + (s32)(i * 65535 / (SAW_N - 1)));
  saw[SAW_N] = saw[0];
  u32 seed = 1;
  for (u32 i = 0; i < DRUM_N; i++) {
    seed = seed * 1664525u + 1013904223u;
    s32 env = 32767 - (s32)(i * 32767 / DRUM_N);
    drum[i] = (s16)(((s32)(seed >> 16) - 32768) * env >> 15);
  }
  drum[DRUM_N] = 0;
  k64_dcache_writeback(sine, sizeof sine);
  k64_dcache_writeback(sqr, sizeof sqr);
  k64_dcache_writeback(saw, sizeof saw);
  k64_dcache_writeback(drum, sizeof drum);
}

// Voice 0 melody (sine), 1 bass (square), 2 pad (saw, panned right), 3 drum, 4 high sine
// left with a step near 4.0.
static void sequence(u32 tick) {
  k64_voice_t *v = mixer.v;
  if (tick % 12 == 0) {
    u32 n = melody[(tick / 12) & 15];
    k64_voice_play(&v[0], sine, SINE_N, 0, note_step(SINE_N, (float)n) / 2, 0, 0);
    k64_voice_volume(&v[0], 9000, 9000, 150);   // click-free attack
  }
  if (tick % 12 == 8) k64_voice_volume(&v[0], 0, 0, 1700);    // release
  if (tick % 48 == 0) {
    k64_voice_play(&v[1], sqr, SQR_N, 0, note_step(SQR_N, 0.f) / 4, 5000, 5000);
    k64_voice_play(&v[2], saw, SAW_N, 0, note_step(SAW_N, 7.f) / 2, 1500, 4500);
    v[2].frac = 0x1234;                    // a non-zero starting phase
  }
  if (tick % 24 == 6) k64_voice_play(&v[3], drum, DRUM_N, K64_NO_LOOP, 0x13000, 12000, 12000);
  if (tick == 30) k64_voice_play(&v[4], sine, SINE_N, 0, 0x3FF00, 3000, -3000);
  if (tick > 30 && tick % 40 == 0)               // slow pan sweep (and a phase flip)
    k64_voice_volume(&v[4], v[4].tgt_r, v[4].tgt_l, 9001);
  // ADPCM: pluck on the off-beats (one-shot, ends by itself or restarts), organ held, retuned
  if (tick % 12 == 6) {
    u32 n = melody[(tick / 12 + 5) & 15];
    float r = 1.f;
    for (u32 k = 0; k < n; k++) r *= 1.0594631f;
    k64_voice_play_adpcm(&v[5], &dec[0], &pluck_a, (u32)(65536.f * r), 7000, 4000);
  }
  if (tick % 96 == 0) {                          // fade in, fade out before the next one
    k64_voice_play_adpcm(&v[6], &dec[1], &organ_a, tick % 192 ? 0x6000 : 0x8000, 0, 0);
    k64_voice_volume(&v[6], 2500, 2500, 6000);
  }
  if (tick % 96 == 70) k64_voice_volume(&v[6], 0, 0, 12345);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  make_samples();
  K64_ASSERT(k64_adpcm_load(&pluck_a, pluck) && k64_adpcm_load(&organ_a, organ));
  k64_audio_init(RATE, FRAMES);

  u16 *zbuf = k64_mem_alloc(K64_ZONE_Z, 320 * 240 * 2, 64);
  k64_viewport_set(&vp, 320, 240);
  k64_dcache_writeback(&vp, sizeof vp);
  k64_mat4_t proj, view, viewproj;
  k64_mat4_perspective(&proj, 60.f * K64_PI / 180.f, 320.f / 240.f, 1.f, 100.f);
  k64_mat4_lookat(&view, (k64_vec3_t){ 0, 0, 6.f }, (k64_vec3_t){ 0, 0, 0 }, (k64_vec3_t){ 0, 1, 0 });
  k64_mat4_mul(&viewproj, &proj, &view);

  u32 tick = 0, ok = 0, bad = 0, nrec = 0, rsp_us = 0, cpu_us = 0, tris = 0;
  for (u32 frame = 0;; frame++) {
    k64_pad_poll_start();
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, zbuf);
    k64_rdp_clear(k64_rgba16(10, 14, 30, 255) * 0x10001u, true);
    k64_rdp_set_other_modes(K64_OM_1C_OPAQUE | K64_OM_ZBUF);
    k64_rdp_set_combine(K64_CC_SHADE);

    // one kgfx list: AUDIO (when the AI queue has room), then the cube
    k64_kgfx_list_t l;
    k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
    k64_kgfx_viewport(&l, &vp);
    k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z, 0, K64_KGFX_CULL_BACK);
    s16 *buf = k64_audio_next();
    if (buf) {
      sequence(tick++);
      nrec = k64_mixer_build(&mixer, recs, MAXREC, buf, FRAMES);
      k64_kgfx_audio(&l, recs);
    }
    k64_mat4_t ry, rx, model, mvp, sc, m;
    k64_mat4_rotate_y(&ry, frame * 0.03f);
    k64_mat4_rotate_x(&rx, frame * 0.021f);
    k64_mat4_mul(&model, &ry, &rx);
    k64_mat4_scale(&sc, 1.f / U, 1.f / U, 1.f / U);
    k64_mat4_mul(&mvp, &viewproj, &model);
    k64_mat4_mul(&m, &mvp, &sc);
    k64_mtx_to_fixed(&mtx, &m);
    k64_dcache_writeback(&mtx, sizeof mtx);
    k64_kgfx_mtx(&l, &mtx);
    k64_kgfx_vtx(&l, cube_v, 8, 0);
    for (int t = 0; t < 12; t += 2)
      k64_kgfx_tri2(&l, cube_i[t][0], cube_i[t][1], cube_i[t][2], cube_i[t + 1][0], cube_i[t + 1][1], cube_i[t + 1][2]);
    k64_kgfx_end(&l);
    u32 t0 = k64_count();
    k64_kgfx_run(&l);
    u32 st = k64_kgfx_wait();
    u32 task_us = K64_US(k64_count() - t0);
    tris = k64_kgfx_tri_count();
    if (st) k64_printf("[11] kgfx status %x\n", st);

    if (buf) {
      rsp_us = task_us;
      k64_dcache_invalidate(buf, FRAMES * 4);
      for (u32 i = 0; i < FRAMES * 2; i++) rsp_out[i] = buf[i];
      t0 = k64_count();
      k64_mixer_run_cpu(recs);             // same records, same output buffer
      cpu_us = K64_US(k64_count() - t0);
      u32 diff = 0, first = 0;
      for (u32 i = 0; i < FRAMES * 2; i++)
        if (rsp_out[i] != buf[i] && diff++ == 0) first = i;
      if (diff) {
        bad++;
        k64_printf("[11] tick %u MISMATCH %u samples, first %u: rsp %d cpu %d\n", tick, diff,
                   first, rsp_out[first], buf[first]);
      } else ok++;
      k64_audio_submit(buf);
    }

    // scope: the last RSP buffer, one column per 2 frames (L green, R red)
    k64_rdp_set_other_modes(K64_OM_FILL);
    k64_rdp_set_fill_color(k64_rgba16(80, 255, 120, 255) * 0x10001u);
    for (u32 x = 0; x < 272; x++) {
      int y = 120 - rsp_out[x * 4] / 512;
      k64_rdp_fill_rect(24 + x, y, 25 + x, y + 2);
    }
    k64_rdp_set_fill_color(k64_rgba16(255, 120, 80, 255) * 0x10001u);
    for (u32 x = 0; x < 272; x++) {
      int y = 120 - rsp_out[x * 4 + 1] / 512;
      k64_rdp_fill_rect(24 + x, y, 25 + x, y + 1);
    }
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  11 audio  %u Hz", k64_audio_rate());
    k64_text_printf(8, 184, 0xFFE040FF, "kgfx task %4u us (audio + %u tris)", rsp_us, tris);
    k64_text_printf(8, 196, 0xFFE040FF, "cpu mix %4u us  recs %u", cpu_us, nrec);
    k64_text_printf(8, 208, bad ? 0xFF4040FF : 0x40FF40FF, "rsp == cpu: %u ok  %u bad", ok, bad);
    k64_text_printf(8, 220, 0xFFE040FF, "buffers %u", tick);
    k64_pad_poll_end();
    k64_rdp_wait_idle();
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[11] frame %u buffers %u ok %u bad %u task %u us cpu %u us recs %u tris %u\n",
                 frame, tick, ok, bad, rsp_us, cpu_us, nrec, tris);
  }
}
