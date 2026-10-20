# Audio

AI output buffers (`src/audio/audio.c`), a voice mixer whose record lists run on the RSP as
an overlay of the kgfx microcode (`src/rsp/kgfx.S`, `cmd_audio_ovl`) or on the CPU
(`src/audio/mixer.c`), demo `11_audio`. API: `include/k64/audio.h`.

## Output

`k64_audio_init(rate, frames)` sets the DAC divider from the VI clock of the console's TV
type (NTSC 48.681812 MHz, PAL 49.656530, MPAL 48.628316; rounded, so 32000 Hz -> 32006 Hz,
`k64_audio_rate()`), bit rate = divider / 66 capped at 16, as libultra. Three buffers of
`frames` stereo s16 frames rotate through the AI's two-entry DMA queue: while the queue is
not full the AI holds at most the last buffer submitted, so the next one in the rotation is
free. `k64_audio_next()` returns it (NULL when the queue is full), `k64_audio_submit()`
queues it. With one buffer of >= rate/60 frames (32 kHz: 544) per video frame the queue
never starves; the demo fills whatever the queue has room for each frame (kestrel64:
188 buffers in 191 frames, the AI rate).

## Mixer

Voices: s16 mono samples with one guard sample at `data[len]` (the loop start sample for a
loop), position `idx + frac/65536`, step 16.16 below 4.0, Q15 volume per channel.
`k64_mixer_build` cuts the output into blocks of 128 frames and each voice into records
where it loops or ends, and advances the voices: the CPU owns all voice state, the RSP keeps
nothing between lists (no persistent DMEM zone needed).

`k64_mixer_build_ex` adds a callback before each block (`tick(ctx, frame, n)`): a sequencer
changes the voices there and they are mixed from that block (docs/MUSIC.md). The decode
records of all blocks go first, as with `k64_mixer_build`; so a VADPCM decoder that mixed in
an earlier block of the list may not be restarted later in the same list (asserted with a
start counter in the decoder).

Volume ramps: `k64_voice_volume(v, l, r, frames)` moves the volume linearly to (l, r) over
`frames` output frames. The builder cuts the voice's records where the ramp ends and puts a
RAMP record (src = 3, +4 `dL << 16 | dR`, the per-frame step `(target - current) / frames
left`, recomputed per record) before each voice record of the ramp; the last record lands
on the target exactly (the truncation error, < 1 step per record, is absorbed there).
|target - current| <= 32767 so every step product fits a s16.

Record (16 B, big-endian), list ends with END:

| Offset | Voice record | BLOCK (src = 0) | END (src = 1) |
|--------|--------------|-----------------|---------------|
| +0  u32 | `dma8 << 24 \| phys` of sample `idx`; dma8 = bytes from `phys & ~7` / 8 - 1 | 0 | 1 |
| +4  u32 | step (16.16) | phys address of the block's frames | |
| +8  u16 | frac | frames (multiple of 4, <= 128) | |
| +10 u8  | first frame in the block | | |
| +11 u8  | frames - 1 | | |
| +12 s16 x2 | volume L, R (Q15) | | |

Arithmetic, per frame f: p = frac + (f - start) * step, i = p >> 16, x = p & 0xFFFF:

```
v   = (s[i] * 65536 + (s[i+1] - s[i]) * x) >> 16            linear interpolation
vol = vol0 + (f - start) * d                                 d from a RAMP record, else 0
t   = clamp16((v * vol * 2 + 0x8000) >> 16)                  Q15 volume
out = clamp16(out + t)                                       per channel, saturating
```

## RSP overlay

`AUDIO list` (opcode 0x1B) loads the audio overlay into the kgfx overlay slot (616 B, the
slot that holds VTX or the clip overlay) and jumps to it; END reloads the viewport and sets
the matrix-reload flag, then the command stream continues, so audio runs inside the frame's
kgfx task with no ucode switch and no yield (RSP-RESIDENCY.md "Audio"). It clobbers DMEM
0x100-0x7FF (vertex cache, OUTBUF): a `VTX` must come before the next `TRI`.

DMEM: OUTB 0x100 (one block, 512 B), SRCBUF 0x300 (1120 B: up to 128 frames at step < 4 plus
the guard and the alignment), AUDTMP 0x760, SEGBUF 0x780 (records read 8 at a time).

Per voice record: one DMA of the samples, then 4 frames per iteration: four `llv` gather
the sample pairs (s[i], s[i+1]) of the 4 frames into lanes 2k, 2k+1; the fraction is kept
per lane (both lanes of a frame), so

```
vmudn  acc  = ~x * s0          (u16 x s16)
vmadn  acc += x * s1
vmadn  acc += 1 * s0           -> s0 * 65536 + (s1 - s0) * x
vmadm  v    = clamp(acc >> 16)
vmulf  t    = v * vol          (L in even lanes, R in odd)
vadd   out += t                (saturating; VCO cleared by vsubc after the vaddc of x)
```

with lane masks for a record that starts or ends inside a group of 4 frames. The volume
vector of the first group is vol + (k - (start & 3)) * d per lane (`vmudh`, then `vadd`);
the later groups take a second vector built with offsets k - (start & 3) + 4 and advanced
by 4 * d per group, so lanes before the start (which may saturate) are never carried
forward. 2 vector ops per group, also when d = 0 (the loop is scalar-bound). The scalar
side only does the gather addresses (no multiply on the RSP scalar unit: the position walks
by adding `step`).

## Measured (kestrel64, `11_audio`, 5 voices, 544 frames, ~42 records)

| | per buffer |
|-|-----------:|
| RSP (kgfx AUDIO, CPU waits) | ~405 us |
| CPU reference | ~2085 us |

RSP and CPU outputs compared every buffer: 188/188 identical.

## VADPCM

Compressed voices (`k64_adpcm_t`, `k64_voice_play_adpcm`): 16-sample frames of 9 bytes
(control byte `scale << 4 | predictor`, then 16 4-bit residuals, even sample in the high
nibble), order-2 predictors (codebook of `npred` entries, each p0[8], p1[8] in Q11: the
responses to s[-2] and s[-1]), 3.56:1 against s16. Per 8-sample half, with
r = clamp16(sign_extend(nibble) << scale):

```
out[j] = clamp16((p0[j] * s6 + p1[j] * s7 + r[j] * 2048 + sum_{k<j} r[k] * p1[j-k-1]) >> 11)
```

s6, s7 = the last two samples of the previous half. The RSP multiplies by `1 << scale` as a
s16, so scale <= 12 (the encoder never emits more).

### Ring

Each ADPCM voice owns a `k64_adpcm_voice_t` (16-aligned, ~8.5 KB): a ring of 4096 decoded
samples (16 groups of 256 = 16 frames) plus a 16-sample mirror of its start (so the mixer's
guard sample and its 4-frame gathers never wrap), a header for the RSP and 17 state slots
(slot 0: the state at the loop start, slot 1 + g: the state before group g). The voice
mixes from the ring as an ordinary looped PCM sample (loop over the whole ring), so the mix
records are unchanged. `k64_mixer_build` first decodes, for each ADPCM voice, every group
the mix of this buffer will read (up to the guard sample) and has not been decoded yet,
then the mix records:

- decoding stays at most one ring ahead of the read position (`dec <= rd + 4096`) and a
  buffer decodes fewer than 15 groups (a 544-frame buffer at step < 4.0 touches <= 10), so no
  group a list reads is overwritten by the same list;
- a group is cut into decode records where the data ends or loops; each record reads its
  start state from a slot and the last part of a group writes the next slot, so lists are
  idempotent (running the same records twice gives the same ring: the CPU reference runs
  them after the RSP in the demo);
- loop start on a frame boundary and loop length a multiple of 16 (the encoder pads the
  start with silence and repeats the loop body until it is), the loop state stored in the
  .k64a; a one-shot voice decodes one frame past `len` (the guard) and ends via `left`.

### Decode record and overlay

`ADPCM list` (opcode 0x1C) loads the ADPCM overlay into the same slot as `AUDIO` and runs its
records, END then returns to the command stream (DMEM as AUDIO). `k64_kgfx_audio` emits the
ADPCM list (when the record list starts with decode records) and then the AUDIO list after
their END. Record (src = 2):

| Offset | Field |
|--------|-------|
| +4 u32 | `(frames - 1) << 24 \| input` (byte address of the first frame), frames <= 16 |
| +8 u32 | `mirror << 31 \| out slot << 24 \| output` (8-aligned); mirror: the record starts at ring[0], also write its first frame to the mirror |
| +12 u32 | `in slot << 24 \| decoder block` |

Per record: header + codebook DMA only when the block differs from the previous record's;
the in-slot state DMA is skipped when the previous record just wrote that slot (the state is
still in DMEM; this also avoids reading back a slot whose write DMA is still queued); one
DMA of the input frames. Per frame the residuals are unpacked with one `lpv` (bytes << 8)
and two multiplies (high nibbles >> 12, low nibbles << 4 then >> 12), scaled by
`1 << scale`, and each half is a matrix product: 8 columns (2048 at lane k, p1 below it,
reloaded per frame from the predictor with 7 shifted `lqv`) times the 8 residuals, plus
p0 * s6 and p1 * s7, `vsar` of the accumulator and `>> 11` with clamping (`vmudn`/`vmadh`
by 32): 14 multiplies per half.

### Assets

`tools/k64bake.py adpcm src.wav dst.k64a [--loop S] [--npred 4]` (`tools/k64adpcm.py`): codebook
by k-means over the frames (assignment by open-loop prediction error, centroid = least
squares predictor, kept stable), then each frame encoded against an exact simulation of the
decoder (all predictors, scales around the open-loop peak, greedy residuals with clamping).
.k64a, big-endian: 'K6A1', rate, len, loop start (0xFFFFFFFF = one-shot), nframes, npred,
codebook offset (48), frames offset, loop state[8]; `k64_adpcm_load` checks it.
CMake: `k64_add_rom(... SOUNDS name=file.wav[:options])` bakes each sound and links it as
`const u8 name[]`.

Demo 11 sounds (`make_sounds.py`, committed WAVs): a Karplus-Strong pluck (12000 samples,
one-shot, SNR 26.0 dB) and an organ (600-sample attack, 120-sample loop, 42.2 dB).

## Measured (kestrel64, `11_audio`, 544 frames)

| | 5 PCM voices (~42 records) | + 2 ADPCM voices (~60 records) |
|-|-----------:|-----------:|
| RSP (kgfx AUDIO + ADPCM, CPU waits) | ~405 us | ~726 us |
| CPU reference | ~2085 us | ~5745 us |

RSP and CPU outputs compared every buffer: 187/187 identical (the ADPCM ring also checked
against an independent Python decode, loop wrap included).

With ramps in the demo (melody attack/release, organ fade in/out, a pan sweep through a
phase flip; ~66 records): RSP ~764 us, CPU ~6190 us, 187/187 identical.
