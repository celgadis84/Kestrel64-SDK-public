// Demo 29 — K64V video playback (k64/video.h): a procedural 320x240 clip with VADPCM audio,
// encoded at build time by tools/k64vid.py. CPU parse, RSP reconstruct + colour conversion
// straight into the framebuffer, RSP audio decode (kgfx ADPCM overlay).
// Self-test (first pass): every frame's Y/U/V planes and every packet's audio samples hash
// to the CRCs the encoder stored (bit-exact with the encoder), and every 8th frame the RSP's
// RGBA5551 output equals the CPU reference (k64_video_blit). Second pass: the same CRCs with
// the CPU reconstruct and audio decode (k64_video_reconstruct_cpu, k64_adpcm_run_cpu).
// Prints per-frame timings, then loops the clip.
#include <k64/k64.h>

extern const u8 clip[];

static u32 fails;
#define CHECK(c, ...)                             \
  do {                                            \
    if (!(c)) {                                   \
      fails++;                                    \
      k64_printf("[29_video] FAIL " __VA_ARGS__); \
      k64_printf("\n");                           \
    }                                             \
  } while (0)

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  static k64_video_t v;
  bool ok = k64_video_open(&v, (u32)(uintptr_t)clip);
  CHECK(ok, "open");
  if (!ok) for (;;) {}
  k64_printf("[29_video] %ux%u %u frames %u.%02u fps audio %u Hz\n", v.width, v.height, v.frames,
             v.fps >> 16, ((v.fps & 0xFFFF) * 100) >> 16, v.audio_rate);
  if (v.audio_rate) k64_audio_init(v.audio_rate, (v.audio_rate / 60 + 4) & ~3u);
  k64_surface_t ref = { .width = v.width, .height = v.height, .stride = v.width * 2, .bpp = K64_BPP16 };
  ref.pixels = k64_mem_alloc(K64_ZONE_HEAP, ref.stride * ref.height, 16);

  for (u32 pass = 0;; pass++) {
    k64_video_open(&v, (u32)(uintptr_t)clip);
    v.use_cpu = pass == 1;
    u32 t_start = k64_count(), sum_p = 0, sum_r = 0, sum_a = 0, late = 0, rgb_checked = 0, apk = 0;
    for (u32 f = 0;; f++) {
      // Frame f is due at t_start + f / fps.
      u32 due = (u32)(((u64)f << 16) * K64_COUNT_HZ / v.fps);
      if (k64_count() - t_start > due) late++;
      while (k64_count() - t_start < due) k64_video_audio(&v);
      const k64_surface_t *fb = k64_display_acquire();
      int x = (fb->width - v.width) / 2, y = (fb->height - v.height) / 2;
      if (!k64_video_decode(&v, fb, x, y)) break;
      sum_p += v.t_parse; sum_r += v.t_recon; sum_a += v.t_audio;
      if (pass < 2) {
        u32 crc = k64_video_plane_crc(&v);
        CHECK(crc == v.crc, "%s frame %u crc %08x want %08x", pass ? "cpu" : "rsp", f, crc, v.crc);
        if (v.a_count) {
          u32 acrc = k64_video_audio_crc(&v);
          CHECK(acrc == v.acrc, "%s frame %u audio crc %08x want %08x", pass ? "cpu" : "rsp", f, acrc, v.acrc);
          apk++;
        }
        if (pass == 0 && f % 8 == 0) {
          k64_video_blit(&v, &ref, 0, 0);
          k64_dcache_invalidate(fb->pixels, fb->stride * fb->height);
          u32 bad = 0;
          for (u32 r = 0; r < v.height; r++) {
            const u16 *a = (const u16 *)((u8 *)fb->pixels + (y + r) * fb->stride) + x;
            const u16 *b = (const u16 *)((u8 *)ref.pixels + r * ref.stride);
            for (u32 c = 0; c < v.width; c++) bad += a[c] != b[c];
          }
          CHECK(!bad, "frame %u: %u RGB pixels differ from the CPU reference", f, bad);
          rgb_checked++;
        }
      }
      k64_display_show(fb);
      k64_video_audio(&v);
      if (pass == 0 && (f % 16 == 0 || f + 1 == v.frames))
        k64_printf("[29_video] frame %u parse %u us rest %u us audio %u us\n", f, K64_US(v.t_parse),
                   K64_US(v.t_recon), K64_US(v.t_audio));
    }
    if (pass < 2) {
      CHECK(v.frame == v.frames, "decoded %u of %u frames", v.frame, v.frames);
      CHECK(apk == v.frames, "%u of %u packets decoded audio", apk, v.frames);
      k64_printf("[29_video] %s avg per frame: parse %u us rest %u us audio %u us, %u late, %u RGB frames checked\n",
                 pass ? "cpu" : "rsp", K64_US(sum_p) / v.frames, K64_US(sum_r) / v.frames,
                 K64_US(sum_a) / v.frames, late, rgb_checked);
    }
    if (pass == 1) k64_printf("[29_video] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);
  }
}
