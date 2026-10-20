// kestrel64-sdk — music sequencer on the voice mixer (docs/MUSIC.md).
//
// Songs are .k64q event streams baked from Standard MIDI Files (tools/k64music.py, CMake
// MUSIC keyword): notes carry their duration (no note-off events), plus program, volume,
// pan, expression, sustain pedal, pitch bend (+ range), tempo and a loop. The player runs
// inside k64_mixer_build_ex: before each 128-frame block it plays the events that are due
// (4 ms at 32 kHz), so timing does not depend on the video frame rate.
//
// Instruments are a sample (VADPCM or s16) with a root key and an ADSR envelope; the
// envelope runs on the mixer's volume ramps (no per-sample CPU work). Channel 10 (index 9)
// takes its instrument per key from the drum table. A note takes a free voice of the
// player's range, else steals the quietest released one, else the oldest.
#pragma once
#include "audio.h"

#define K64_MUSIC_CHANNELS 16
#define K64_MUSIC_PENDING  16

typedef struct {
  const k64_adpcm_t *adpcm;             // VADPCM sample, or NULL and:
  const s16 *pcm;                       //   s16 sample (KSEG0, written back, guard sample)
  u32 len, loop_start, rate;            //   loop_start = K64_NO_LOOP: one-shot
  u8 root;                              // MIDI key at which the sample plays at its rate
  s8 fine;                              // cents
  u8 volume;                            // 0..127
  s8 pan;                               // added to the channel pan (drum kits)
  u16 attack, decay, release;           // ms (0 = at once)
  u8 sustain;                           // level after the decay, 0..127
} k64_instrument_t;

typedef struct {
  const k64_instrument_t *program[128]; // by program number (missing: program 0, else silent)
  const k64_instrument_t *drum[128];    // channel 10, by key (played at their root)
} k64_bank_t;

typedef struct {
  u8 program, volume, pan, expression, bend_range;
  bool sustain;
  s16 bend;                             // -8192..8191
} k64_music_channel_t;

typedef struct {
  u32 release_tick;                     // when the note ends (song ticks)
  u32 left;                             // frames left in the envelope stage
  u32 age;
  float level;                          // envelope level at the end of the stage, 0..1
  float gain_l, gain_r;                 // velocity x channel x instrument x master, panned
  float pitch;                          // step without the bend
  const k64_instrument_t *inst;
  u8 ch, key, vel, stage;               // stage: K64_ENV_*
  bool held;                            // ended while the sustain pedal is down
} k64_music_voice_t;
enum { K64_ENV_OFF, K64_ENV_ATTACK, K64_ENV_DECAY, K64_ENV_SUSTAIN, K64_ENV_RELEASE };

typedef struct { u8 ch, key, vel; u32 end; } k64_music_note_t;

typedef struct {
  k64_mixer_t *mixer;
  const k64_bank_t *bank;
  k64_adpcm_voice_t *dec;               // one per voice (ADPCM instruments), or NULL
  u32 first, nvoices, rate;
  const u8 *song, *pos, *loop;
  bool playing;
  u16 ppq, speed;                       // speed: Q8 (256 = as written)
  u32 tempo;                            // µs per quarter note
  u64 now, tick_rate;                   // song ticks, 32.32; per output frame
  u32 next;                             // tick of the next event (pos: its op byte)
  u8 master;
  u32 played;                           // voices that mixed ADPCM in this list (restart rule)
  u32 age;
  k64_music_channel_t ch[K64_MUSIC_CHANNELS];
  k64_music_voice_t v[K64_MIXER_VOICES];
  k64_music_note_t pending[K64_MUSIC_PENDING]; // notes waiting for a voice (next list)
  u32 npending;
  struct { u32 notes, steals, deferred, dropped, loops, peak; } stats;
} k64_music_t;

// Voices first .. first + nvoices - 1 of `m` belong to the player. `dec`: nvoices decoders
// (16-byte aligned) when the bank has VADPCM instruments, else NULL. `rate` = output rate
// (k64_audio_rate()).
void k64_music_init(k64_music_t *mu, k64_mixer_t *m, u32 first, u32 nvoices, k64_adpcm_voice_t *dec,
                    const k64_bank_t *bank, u32 rate);
// Starts a .k64q song (resident, 4-byte aligned) from the top; it loops if it has a loop.
// False if the blob is not a song.
bool k64_music_play(k64_music_t *mu, const void *song);
// Stops reading events and releases every note (their release times, then silence).
void k64_music_stop(k64_music_t *mu);
// Master volume 0..127 (default 100), applied to new notes and ramped onto playing ones.
void k64_music_volume(k64_music_t *mu, u8 volume);
// Tempo scale, Q8 (256 = as written).
void k64_music_speed(k64_music_t *mu, u16 speed);

// Mixer tick (k64_mixer_tick_fn): plays the events due at this block.
void k64_music_tick(void *mu, u32 frame, u32 n);
// k64_mixer_build_ex with the player's tick.
K64_INLINE u32 k64_music_build(k64_music_t *mu, k64_mixrec_t *recs, u32 cap, s16 *out, u32 frames) {
  return k64_mixer_build_ex(mu->mixer, recs, cap, out, frames, k64_music_tick, mu);
}
