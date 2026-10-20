// kestrel64-sdk — audio: AI output buffers and the voice mixer.
//
// Output: k64_audio_init picks the AI rate and allocates three buffers of `frames` stereo
// frames (s16 L R interleaved). Each frame of the game, while k64_audio_next() returns a
// buffer, fill it (RSP mixer or CPU) and k64_audio_submit() it: the AI plays one buffer and
// queues one, the third is being filled. Calling it again (another rate, e.g. a video's)
// reuses the buffers when they hold `frames`.
//
// Mixer: up to K64_MIXER_VOICES voices of s16 mono samples, resampled with linear
// interpolation (step = source samples per output frame, 16.16, below 4.0), panned with a
// Q15 volume per channel (optionally ramped linearly, k64_voice_volume) and summed with
// saturation. k64_mixer_build turns the voices into a
// list of records (one per voice and 128-frame block, split where a voice loops or ends) and
// advances them; the kgfx AUDIO command runs the list on the RSP (an overlay of the same
// microcode, no ucode switch: docs/AUDIO.md), k64_mixer_run_cpu runs it on the CPU with the
// same arithmetic (bit-identical output).
//
// Sample data must be written back from D$ (k64_dcache_writeback) and carry one guard sample
// after the end: data[len] = data[loop_start] for a loop, the last sample (or 0) otherwise.
//
// VADPCM voices (k64_voice_play_adpcm): the list starts with decode records that the kgfx
// ADPCM command runs (another overlay) into a ring of decoded samples per voice; the voice
// then mixes from the ring like a looped s16 sample (docs/AUDIO.md).
#pragma once
#include "hw.h"
#include "kgfx.h"

// ---- AI ---------------------------------------------------------------------------------------
// rate in Hz; frames per buffer, a multiple of 4 (>= rate / 60 so one buffer per video frame
// keeps the queue fed: 32000 Hz -> 544).
void k64_audio_init(u32 rate, u32 frames);
u32  k64_audio_rate(void);              // actual rate (the DAC divider rounds)
u32  k64_audio_frames(void);
// Next buffer to fill when the AI queue has room, else NULL. Fill, then submit it.
s16 *k64_audio_next(void);
// Queues the buffer (RDRAM contents: the CPU must have written back what it wrote).
void k64_audio_submit(s16 *buf);

// ---- mixer ------------------------------------------------------------------------------------
#define K64_MIXER_VOICES 16
#define K64_MIXER_BLOCK  128            // frames per RSP block
#define K64_NO_LOOP      0xFFFFFFFFu

// VADPCM sample: 9-byte frames of 16 samples, order-2 predictors (tools/k64bake.py adpcm).
typedef struct {
  const u8 *frames;
  const s16 *codebook;                  // npred x (p0[8], p1[8]), Q11, 16-byte aligned
  u32 npred, nframes;
  u32 rate;                             // source sample rate
  u32 len;                              // samples; < nframes * 16 for a one-shot (guard)
  u32 loop_start;                       // multiple of 16, loop end = len = nframes * 16;
                                        // K64_NO_LOOP: one-shot
  s16 loop_state[8];                    // decoder state at loop_start
} k64_adpcm_t;
// Fills `a` from a .k64a blob (16-byte aligned, stays in use). False if it is not one.
bool k64_adpcm_load(k64_adpcm_t *a, const void *blob);

#define K64_ADPCM_RING  4096            // decoded samples kept per ADPCM voice (16 groups)
#define K64_ADPCM_GROUP 256             // samples per decode group (16 frames)

// Decoder of one ADPCM voice: the RDRAM block the decode records read and write, the ring of
// decoded samples, and the CPU bookkeeping. Static or heap, 16-byte aligned (~8.5 KB).
typedef struct K64_ALIGN(16) {
  u32 codebook, codebook_len1, mirror, self; // RSP header: codebook, bytes - 1, &ring[RING], &this
  s16 state[1 + K64_ADPCM_RING / K64_ADPCM_GROUP][8]; // [0] loop state, [1 + g] before group g
  s16 ring[K64_ADPCM_RING + 16];        // + a copy of ring[0..15] (interpolation guard)
  const k64_adpcm_t *a;
  u32 dec;                              // stream frames decoded
  u32 src;                              // next input frame
  u32 rd;                               // stream sample at the voice position
  u8 in;                                // state slot for the next decode record
  bool ended;                           // one-shot: all frames decoded
  u32 starts;                           // k64_voice_play_adpcm calls (restart check)
} k64_adpcm_voice_t;

typedef struct {
  const s16 *data;                      // KSEG0, 2-byte aligned, len + 1 samples (guard)
  u32 len, loop_start;                  // loop_start = K64_NO_LOOP: one-shot
  u32 idx, step;                        // position: idx + frac / 65536; step 16.16 (< 4.0)
  u16 frac;
  s16 vol_l, vol_r;                     // Q15, current
  s16 tgt_l, tgt_r;                     // ramp target
  u32 ramp;                             // frames left in the ramp (0: none)
  bool on;
  k64_adpcm_voice_t *adpcm;             // ADPCM voice: data is its ring
  u32 left;                             // ADPCM one-shot: samples left from idx
} k64_voice_t;

typedef struct { k64_voice_t v[K64_MIXER_VOICES]; } k64_mixer_t;

// Mixer list record, 16 bytes (the RSP format, docs/AUDIO.md).
typedef union K64_ALIGN(8) {
  struct {
    u32 src;                            // dma8 << 24 | physical address of the first sample;
                                        // 0 = BLOCK, 1 = END, 2 = VADPCM decode, 3 = RAMP
                                        // (w[1] = dL << 16 | dR: per-frame volume step of
                                        // the next voice record)
    u32 step;                           // BLOCK: physical address of the block's frames
    u16 frac;                           // BLOCK: frames
    u8 start, count1;                   // frames start .. start + count1 of the block
    s16 vol_l, vol_r;
  };
  u32 w[4];
} k64_mixrec_t;

// step for playing a `src_rate` sample (or a pitch: src_rate = freq * period length) at the
// output rate.
K64_INLINE u32 k64_mixer_step(u32 src_rate, u32 out_rate) {
  return (u32)(((u64)src_rate << 16) / out_rate);
}
void k64_voice_play(k64_voice_t *v, const s16 *data, u32 len, u32 loop_start, u32 step,
                    s16 vol_l, s16 vol_r);
// Ramps the volume linearly to (vol_l, vol_r) over `frames` output frames (0: at once).
// |target - current| <= 32767 per channel.
void k64_voice_volume(k64_voice_t *v, s16 vol_l, s16 vol_r, u32 frames);
// Plays a VADPCM sample through the decoder `d` (owned by the voice while it plays).
void k64_voice_play_adpcm(k64_voice_t *v, k64_adpcm_voice_t *d, const k64_adpcm_t *a, u32 step,
                          s16 vol_l, s16 vol_r);
// Records for `frames` (multiple of 4) frames written to `out`; advances the voices. Returns
// the number of records (asserts that `cap` is enough: voices x blocks x 2 + blocks + 1 is
// always enough for loops longer than a block, x 5 instead of x 2 for a ramping voice, plus per ADPCM voice decode records: about
// frames x step / 256 + 2, more with loops shorter than 256 samples, and one END). Writes the
// records back from D$. With ADPCM voices the list is: decode records, END, mix records.
u32 k64_mixer_build(k64_mixer_t *m, k64_mixrec_t *recs, u32 cap, s16 *out, u32 frames);
// Same, calling tick(ctx, frame, n) before the records of each block (frames frame .. frame
// + n of `out`): a sequencer starts, stops and changes voices there, so events land on 128
// frame boundaries. A voice started in a block plays from that block. Rule: an ADPCM voice's
// decoder may not be restarted (k64_voice_play_adpcm) in a list after it played in an earlier
// block of the same list (asserted); start the note in the next list or on another voice.
typedef void (*k64_mixer_tick_fn)(void *ctx, u32 frame, u32 n);
u32 k64_mixer_build_ex(k64_mixer_t *m, k64_mixrec_t *recs, u32 cap, s16 *out, u32 frames,
                       k64_mixer_tick_fn tick, void *ctx);
// Runs a record list on the CPU (writes the blocks back from D$).
void k64_mixer_run_cpu(const k64_mixrec_t *recs);
// Runs decode records (up to their END) on the CPU, the reference of the kgfx ADPCM overlay;
// returns the record after the END. The decoder blocks' state slots must not be stale in D$
// (invalidate them if the RSP wrote them). Writes the output back from D$.
const k64_mixrec_t *k64_adpcm_run_cpu(const k64_mixrec_t *recs);

// kgfx ADPCM (when the list has decode records) + AUDIO: runs the record list on the RSP.
// Clobbers the vertex cache (VTX again before the next TRI). The blocks are in RDRAM when
// k64_kgfx_wait returns.
void k64_kgfx_audio(k64_kgfx_list_t *l, const k64_mixrec_t *recs);
