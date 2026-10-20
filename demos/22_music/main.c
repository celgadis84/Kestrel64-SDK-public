// Demo 22 — music: a MIDI song (make_music.py -> song.mid, baked to .k64q by the MUSIC asset
// keyword) played by the k64 music sequencer on 12 mixer voices: lead and bass (looped
// VADPCM), pad (looped s16, sustain pedal, expression swell), drums on channel 10 (VADPCM
// one-shots); pitch bend, pan, a tempo change and a loop.
//
// Self-test first (no audio): the sequencer runs ~20 s of song through k64_mixer_build_ex
// and the note count after every 128-frame block is checked against note times computed here
// from the .k64q (tempo map + loop) — notes start on the first block at or after their time.
// Then it plays: every buffer is mixed on the RSP and again on the CPU and compared sample for
// sample (as demo 11); the screen shows the voices, the song position and the stats.
#include <k64/k64.h>

#define RATE    32000
#define FRAMES  544
#define NVOICES 12
#define MAXREC  512

extern const u8 song[], lead[], bass[], kick[], snare[], hat[], pad[], pad_end[];

static k64_adpcm_t lead_a, bass_a, kick_a, snare_a, hat_a;
static k64_adpcm_voice_t dec[NVOICES];
static k64_mixer_t mixer;
static k64_music_t mus;
static k64_bank_t bank;
static k64_instrument_t i_lead, i_bass, i_pad, i_kick, i_snare, i_hat;
static k64_mixrec_t recs[MAXREC] K64_ALIGN(16);
static s16 rsp_out[FRAMES * 2] K64_ALIGN(16);
static s16 scratch[FRAMES * 2] K64_ALIGN(16);
static u64 list_buf[16] K64_ALIGN(16);

static void make_bank(void) {
  K64_ASSERT(k64_adpcm_load(&lead_a, lead) && k64_adpcm_load(&bass_a, bass));
  K64_ASSERT(k64_adpcm_load(&kick_a, kick) && k64_adpcm_load(&snare_a, snare) && k64_adpcm_load(&hat_a, hat));
  i_lead = (k64_instrument_t){ .adpcm = &lead_a, .root = 69, .volume = 100, .attack = 8,
                               .decay = 300, .sustain = 80, .release = 120 };
  i_bass = (k64_instrument_t){ .adpcm = &bass_a, .root = 45, .volume = 120, .attack = 2,
                               .decay = 200, .sustain = 70, .release = 40 };
  i_pad = (k64_instrument_t){ .pcm = (const s16 *)pad, .len = (u32)(pad_end - pad) / 2 - 1,
                              .loop_start = 0, .rate = 28160, .root = 57, .volume = 90,
                              .attack = 250, .decay = 0, .sustain = 127, .release = 600 };
  i_kick = (k64_instrument_t){ .adpcm = &kick_a, .root = 60, .volume = 127, .sustain = 127, .release = 30 };
  i_snare = (k64_instrument_t){ .adpcm = &snare_a, .root = 60, .volume = 110, .pan = 8, .sustain = 127,
                                .release = 60 };
  i_hat = (k64_instrument_t){ .adpcm = &hat_a, .root = 60, .volume = 80, .pan = -20, .sustain = 127,
                              .release = 20 };
  bank.program[0] = &i_lead;
  bank.program[1] = &i_bass;
  bank.program[2] = &i_pad;
  bank.drum[36] = &i_kick;
  bank.drum[38] = &i_snare;
  bank.drum[42] = &i_hat;
}

// ---- self-test: note start times from the .k64q --------------------------------------------

#define TEST_SECONDS 20
static float note_frame[1024];
static u32 nnotes;

static u32 var(const u8 **p) {
  u32 v = 0;
  u8 b;
  do { b = *(*p)++; v = v << 7 | (b & 0x7F); } while (b & 0x80);
  return v;
}

// output frame of every note start in the first TEST_SECONDS (tempo map, loop jumps)
static void note_times(u32 rate) {
  const u8 *p = song + 16, *loop = NULL;
  u32 ppq = song[4] << 8 | song[5];
  double tempo = (u32)song[8] << 24 | song[9] << 16 | song[10] << 8 | song[11];
  double f = 0;                          // frames at the current event
  nnotes = 0;
  for (;;) {
    f += var(&p) * tempo * rate / (ppq * 1e6);
    if (f > (double)TEST_SECONDS * rate) return;
    u8 op = *p++;
    if (op < 0x10) { K64_ASSERT(nnotes < K64_ARRAY_LEN(note_frame)); note_frame[nnotes++] = (float)f; p += 2; var(&p); }
    else if (op < 0x40 || (op >= 0x50 && op < 0x80)) p++;
    else if (op < 0x50) p += 2;
    else if (op == 0xF0) { tempo = p[0] << 16 | p[1] << 8 | p[2]; p += 3; }
    else if (op == 0xF1) loop = p;
    else if (op == 0xF2) p = loop;
    else return;
  }
}

// Notes started after each block's tick vs the computed times. Tempo changes take effect at
// the block where they are due, so later notes may shift by a few frames: slack of 16 frames.
static u32 test_base, test_fails, test_blocks;

static void test_tick(void *ctx, u32 frame, u32 n) {
  k64_music_tick(ctx, frame, n);
  float f = (float)(test_base + frame);
  u32 lo = 0, hi = 0;
  for (u32 k = 0; k < nnotes; k++) {
    if (note_frame[k] <= f - 16.f) lo++;
    if (note_frame[k] <= f + 16.f) hi++;
  }
  if ((mus.stats.notes < lo || mus.stats.notes > hi) && test_fails++ < 4)
    k64_printf("[22] block at frame %u: %u notes started, expected %u..%u\n", test_base + frame,
               mus.stats.notes, lo, hi);
  test_blocks++;
}

static bool self_test(u32 rate) {
  note_times(rate);
  K64_ASSERT(k64_music_play(&mus, song));
  u32 lists = (TEST_SECONDS - 1) * rate / FRAMES;
  for (test_base = 0; test_base < lists * FRAMES; test_base += FRAMES)
    k64_mixer_build_ex(&mixer, recs, MAXREC, scratch, FRAMES, test_tick, &mus);
  bool pass = !test_fails && !mus.stats.deferred && !mus.stats.dropped && mus.stats.loops;
  k64_printf("[22] self-test: %u blocks, %u notes (%u in %us), loops %u, deferred %u dropped %u "
             "steals %u peak %u: %s\n", test_blocks, mus.stats.notes, nnotes, TEST_SECONDS, mus.stats.loops,
             mus.stats.deferred, mus.stats.dropped, mus.stats.steals, mus.stats.peak, pass ? "PASS" : "FAIL");
  return pass;
}

// ---- display -------------------------------------------------------------------------------

static const char *key_name(u32 k) {
  static char s[5];
  static const char n[] = "C-C#D-D#E-F-F#G-G#A-A#B-";
  s[0] = n[(k % 12) * 2]; s[1] = n[(k % 12) * 2 + 1]; s[2] = (char)('0' + k / 12 - 1); s[3] = 0;
  return s;
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();
  make_bank();
  k64_audio_init(RATE, FRAMES);
  u32 rate = k64_audio_rate();
  k64_music_init(&mus, &mixer, 0, NVOICES, dec, &bank, rate);
  bool test_ok = self_test(rate);

  // fresh start for playback
  for (u32 i = 0; i < K64_MIXER_VOICES; i++) mixer.v[i].on = false;
  k64_music_init(&mus, &mixer, 0, NVOICES, dec, &bank, rate);
  K64_ASSERT(k64_music_play(&mus, song));

  u32 ok = 0, bad = 0, nrec = 0, rsp_us = 0, cpu_us = 0, build_us = 0, bufs = 0;
  for (u32 frame = 0;; frame++) {
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(12, 10, 28, 255) * 0x10001u, false);

    s16 *buf = k64_audio_next();
    if (buf) {
      u32 t0 = k64_count();
      nrec = k64_music_build(&mus, recs, MAXREC, buf, FRAMES);
      build_us = K64_US(k64_count() - t0);
      k64_kgfx_list_t l;
      k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
      k64_kgfx_audio(&l, recs);
      k64_kgfx_end(&l);
      t0 = k64_count();
      k64_kgfx_run(&l);
      u32 st = k64_kgfx_wait();
      rsp_us = K64_US(k64_count() - t0);
      if (st) k64_printf("[22] kgfx status %x\n", st);
      k64_dcache_invalidate(buf, FRAMES * 4);
      for (u32 i = 0; i < FRAMES * 2; i++) rsp_out[i] = buf[i];
      t0 = k64_count();
      k64_mixer_run_cpu(recs);
      cpu_us = K64_US(k64_count() - t0);
      u32 diff = 0, first = 0;
      for (u32 i = 0; i < FRAMES * 2; i++)
        if (rsp_out[i] != buf[i] && diff++ == 0) first = i;
      if (diff) {
        bad++;
        k64_printf("[22] buffer %u MISMATCH %u samples, first %u: rsp %d cpu %d\n", bufs, diff, first,
                   rsp_out[first], buf[first]);
      } else ok++;
      k64_audio_submit(buf);
      bufs++;
    }

    // voices: one row each, bar = current volume (L + R)
    k64_rdp_set_other_modes(K64_OM_FILL);
    for (u32 i = 0; i < NVOICES; i++) {
      const k64_music_voice_t *v = &mus.v[i];
      const k64_voice_t *m = &mixer.v[i];
      u32 y = 40 + i * 12;
      if (v->stage == K64_ENV_OFF || !m->on) {
        k64_text_printf(8, y, 0x606080FF, "%2u  -", i);
        continue;
      }
      static const u32 col[4] = { 0xFFE040FF, 0x40C0FFFF, 0x80FF80FF, 0xFF8060FF };
      u32 c = v->ch == 9 ? 3 : v->ch < 3 ? v->ch : 0;
      k64_text_printf(8, y, col[c], "%2u ch%-2u %-4s %c", i, v->ch + 1, v->ch == 9 ? "drm" : key_name(v->key),
                      "-ADSR"[v->stage]);
      u32 w = (u32)(m->vol_l + m->vol_r) / 256;
      k64_rdp_set_fill_color(k64_rgba16(col[c] >> 24, col[c] >> 16 & 255, col[c] >> 8 & 255, 255) * 0x10001u);
      if (w) k64_rdp_fill_rect(120, y + 1, 120 + (w > 180 ? 180 : w), y + 7);
    }
    u32 tick = (u32)(mus.now >> 32);
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  22 music  %u Hz", rate);
    k64_text_printf(8, 20, 0xC0C0C0FF, "tick %5u  %3u bpm  loop %u  test %s", tick, 60000000u / mus.tempo,
                    mus.stats.loops, test_ok ? "PASS" : "FAIL");
    k64_text_printf(8, 196, 0xFFE040FF, "us: build %3u  rsp %4u  cpu %5u", build_us, rsp_us, cpu_us);
    k64_text_printf(8, 208, 0xFFE040FF, "recs %3u  notes %u  steals %u  peak %u", nrec, mus.stats.notes,
                    mus.stats.steals, mus.stats.peak);
    k64_text_printf(8, 220, bad ? 0xFF4040FF : 0x40FF40FF, "rsp == cpu: %u ok  %u bad", ok, bad);
    k64_rdp_wait_idle();
    k64_display_show(fb);
    if ((frame & 63) == 63)
      k64_printf("[22] frame %u buffers %u ok %u bad %u tick %u notes %u steals %u deferred %u peak %u "
                 "build %u us rsp %u us cpu %u us recs %u\n", frame, bufs, ok, bad, tick, mus.stats.notes,
                 mus.stats.steals, mus.stats.deferred, mus.stats.peak, build_us, rsp_us, cpu_us, nrec);
  }
}
