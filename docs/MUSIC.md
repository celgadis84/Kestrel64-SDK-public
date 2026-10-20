# Music

`k64/music.h`: a MIDI-driven sequencer that plays songs on a range of mixer voices
(docs/AUDIO.md). Demo 22_music plays a 4-part song (lead, bass, pad, drums) and self-tests the
note timing; every buffer is also mixed on the CPU and compared with the RSP output.

## Pipeline

```cmake
k64_add_rom(NAME game SOURCES main.c
            MUSIC song=song.mid                       # or "song=song.mid:--loop"
            SOUNDS "lead=lead.wav:--loop 1024" kick=kick.wav
            FILES pad=pad.pcm)                        # raw big-endian s16 + guard sample
```

`tools/k64music.py` (`k64bake.py music`) turns a Standard MIDI File (type 0 or 1) into a
`.k64q` song. All tracks are merged into one stream in tick order; a note is one event that
carries its duration, so the player keeps no note-off state.

Kept: notes, program change, volume (CC 7), pan (CC 10), expression (CC 11), sustain pedal
(CC 64), pitch bend, bend range (RPN 0 via CC 101/100/6), tempo. Dropped: other controllers,
sysex, aftertouch, text. At equal ticks, controls come before notes (a program change on the
same tick applies to the note).

Loop: meta markers `loopStart` / `loopEnd` (meta 0x06 or text), or `--loop` (whole song, the
end rounded up to a bar, or `--loop-end` ticks). Notes still sounding at the loop end are cut
there. When the tempo changes inside the loop, the loop start's tempo is written after the
loop start so the jump restores it; other channel state is not restored (set it before the
loop start, or again inside the loop).

### .k64q (big-endian)

| Offset | |
|--------|---|
| 0  | `K64Q` |
| 4  | u16 ticks per quarter, u16 version (1) |
| 8  | u32 tempo at tick 0 (µs per quarter) |
| 12 | u32 event bytes |
| 16 | events: varint delta ticks, op byte, operands |

| Op | Operands |
|----|----------|
| `0x0c` note | key, velocity, varint duration (>= 1 tick) |
| `0x1c` program | p |
| `0x2c` volume / `0x3c` pan / `0x5c` expression | 0..127 |
| `0x4c` bend | s16, -8192..8191 |
| `0x6c` sustain pedal | 0 / 1 |
| `0x7c` bend range | semitones |
| `0xF0` tempo | u24 µs per quarter |
| `0xF1` loop start / `0xF2` loop end (jump) / `0xFF` end | |

(`c` = channel 0..15.) Demo 22's song: 228 notes, 9 bars, 1456 bytes.

## Player

```c
static k64_adpcm_voice_t dec[12];                  // one per voice when the bank has VADPCM
k64_music_init(&mus, &mixer, 0, 12, dec, &bank, k64_audio_rate());
k64_music_play(&mus, song);                        // song = embedded .k64q
...
s16 *buf = k64_audio_next();
if (buf) {
  u32 n = k64_music_build(&mus, recs, MAXREC, buf, FRAMES);   // = k64_mixer_build_ex + tick
  k64_kgfx_audio(&l, recs);                                   // the usual mixer list
  ...
  k64_audio_submit(buf);
}
```

`k64_mixer_build_ex(m, recs, cap, out, frames, tick, ctx)` calls `tick(ctx, frame, n)` before
building the records of each 128-frame block; `k64_music_tick` plays the events that are due
there. Events therefore land on 128-frame boundaries (4 ms at 32 kHz) whatever the buffer size
and the video rate. Voices started, stopped or changed in a block are mixed from that block;
other code may use the voices outside the player's range in the same list.

Timing: song position is a 32.32 tick counter advanced by `ticks per frame = ppq x 10^6 /
(tempo x rate)` (x `speed / 256`, `k64_music_speed`) per block. A note is started at the
first block start at or after its time; a tempo change takes effect from the block where it is
due (a sub-block shift of the later events, a few frames).

### Instruments

```c
typedef struct {
  const k64_adpcm_t *adpcm;        // VADPCM sample, or NULL and an s16 sample:
  const s16 *pcm; u32 len, loop_start, rate;
  u8 root; s8 fine;                // key at which the sample plays at its rate, cents
  u8 volume; s8 pan;               // 0..127; pan offset (drum kits)
  u16 attack, decay, release;      // ms
  u8 sustain;                      // 0..127
} k64_instrument_t;
```

A `k64_bank_t` maps the 128 programs (missing: program 0, else silent) and, for channel 10,
the 128 drum keys (each played at its root). Tones loop; drums are usually one-shots.

Per note:

- step = `rate / out_rate x 2^((key - root + fine / 100 + bend) / 12)` (16.16, clamped below
  4.0); bend = `bend x range / 8192` semitones, updated on the channel's voices on every bend
  event (2^x by a 5th-order polynomial, < 0.03 cent error).
- gain = `(velocity x volume x expression / 127^3)^2 x instrument volume x master / 127^2 x 20000` (Q15)
  (squared: the usual MIDI loudness curve), panned with sin/cos (center = -3 dB per side).
- ADSR on the mixer's volume ramps, no per-sample CPU work: attack ramps 0 -> gain, decay ramps
  to `sustain x gain`, release ramps to 0 (at least 64 frames, no clicks) and frees the voice.
  Stages advance at block starts. A one-shot sample that ends frees its voice too. Volume,
  pan, expression and master changes re-ramp the playing notes (>= 64 frames).
- Sustain pedal: notes that end while it is down are held until it is released.

Voice allocation: a free voice, else the quietest releasing one, else the oldest note
(`stats.steals`). VADPCM restart rule (AUDIO.md): a voice whose decoder already mixed in this
list cannot restart a VADPCM sample before the next list; such a note waits for a voice
(`stats.deferred`, played at the start of the next list, up to one buffer late; dropped if it
would have ended by then). With enough voices it does not happen (demo 22: 0 in 20 s).

`k64_music_stop` releases every note; `k64_music_volume` sets the master volume (0..127,
default 100). `mus.now >> 32` is the song tick, `mus.v[]` the per-voice state (channel, key,
stage), `mus.stats` the counters.

## Measured (kestrel64, demo 22, 544-frame buffers at 32006 Hz, 12 voices)

| | per buffer (17 ms of audio) |
|---|---|
| `k64_music_build` (events + envelopes + records) | 90-270 µs CPU |
| RSP (ADPCM decode + mix, one kgfx task) | 0.3-1.25 ms (40-80 records) |
| CPU reference mix of the same records | 2-11 ms |

The RSP does the per-sample work; the CPU cost is per note and per block, not per sample.

## Self-test (demo 22)

Before playback the player runs ~19 s of song (5585 blocks, the loop and the tempo change)
through `k64_mixer_build_ex` with a tick that checks, after every block, the number of notes
started against note times computed independently from the `.k64q` with a tempo map in
double precision (16 frames slack for tempo changes taking effect at block starts).
Deferred or dropped notes or no loop also fail it. Then each played buffer is compared RSP
vs CPU sample for sample.
