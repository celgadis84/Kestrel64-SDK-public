# K64V video

K64V is the SDK's own video codec, designed around the N64: the RSP does the per-pixel work
(IDCT, prediction, clamping, colour conversion) and the CPU only parses the bitstream, in
parallel with it. The building blocks are textbook techniques (8x8 DCT, zigzag scan,
Exp-Golomb codes, skip/inter/intra/motion-compensated macroblocks); the format, the transform arithmetic and
the code are written for this SDK, not taken from another decoder.

Tools: `tools/k64vid.py` encodes PNG frames + a 16-bit mono WAV, and `k64vid.py --decode`
is an independent Python decoder that checks every frame CRC. Runtime: `k64/video.h`.
Demo: `29_video`.

## Format

See the docstring of `tools/k64vid.py` for the byte layout. In short:

- YUV 4:2:0, 16x16 macroblocks: Y0 Y1 Y2 Y3 U V blocks of 8x8.
- I frames: every macroblock intra (block + 128). P frames: per macroblock `ue(mode)`:
  0 skip (previous frame), 1 inter (previous frame + residual), 2 intra, 3 motion
  (previous frame displaced by a whole-pixel vector + residual).
- Motion vectors: `se(dx - px) se(dy - py)`, predicted from the macroblock to the left when
  it is a motion macroblock (else 0). Chroma moves by `(dx >> 1, dy >> 1)`. The 16x16 luma
  source lies inside the frame (the encoder only searches there, the parse rejects the rest),
  so no edge extension is needed. The encoder does a full search (+-12 px by default,
  `--search`) on luma SAD and lets rate-distortion pick the mode.
- 6-bit coded block pattern; per coded block `ue(n-1)` then n tokens
  `ue(run) ue(|level|-1) sign` in zigzag order. Coefficient = `sat16(level * q * (8+u+v))`.
- Each packet carries the FNV-1a of the decoded Y/U/V planes and of the decoded audio,
  and the frame's audio as VADPCM frames (9 bytes per 16 samples, one codebook for the clip
  in the file header, `--npred` predictors, coded by `tools/k64adpcm.py`): the audio frames
  that end by sample `round((f + 1) * rate / fps)`, the decoder state carried over from
  packet to packet. Demo clip: 172 KB of PCM -> 48 KB, SNR 42.3 dB.

## The transform is defined by the RSP

The IDCT is not "a DCT, rounded somehow": it is the exact integer computation the
microcode performs, so encoder, CPU reference and RSP agree bit for bit and P frames never
drift. With `M` the 8x8 orthonormal DCT basis in Q15 (`video_ref.c`):

    pass 1 (columns, 8 lanes = u):  t[i][u] = sat16((2 * sum_v M[i][v] * x[v][u] + 0x8000) >> 16)
    pass 2 (rows,    8 lanes = i):  p[j][i] = sat16((2 * sum_u M[j][u] * t[i][u] + 0x8000) >> 16)
    pixel delta = (p * 0x2000 + 0x8000) >> 16

which is `vmulf` followed by 7 `vmacf` per output row (the accumulator keeps the full sum,
the rounding constant comes from `vmulf`), and `vmulf` by 0x1000 for the final /8.
Coefficients are in units of 8x the orthonormal DCT of the pixels, so the DC of a block
stays in s16 with room for residuals.

## Stages

    CPU   parse row r          -> work list (mode | cbp, [motion offsets], {count, {index, value}...})
    RSP   reconstruct row r-1  -> Y/U/V planes, RGBA5551 framebuffer

Two sets of planes alternate: each frame reads its prediction from the previous one and
writes the other. The RSP DMAs each strip in from the previous frame and out to the
current one, so skip and inter cost nothing extra. A motion macroblock's source starts at
any byte: the RSP has no scalar multiply and DMA wants 8-byte alignment, so the CPU parse
puts the source's byte offsets in the work list and the RSP DMAs the aligned window around
it (16 x 24 luma, 8 x 16 per chroma plane) and moves each row with `lqv` + `lrv`.
`k64_video_reconstruct_cpu` is the reference for the RSP stage. The CPU path measured in
kestrel64 (320x240, demo clip): parse 11 ms/frame, reconstruct 92 ms, YUV->RGB 87 ms: the
CPU alone gives ~5 fps, which is why the last two stages belong on the RSP.

Audio runs before the frame's parse: one kgfx list with the ADPCM command (the mixer's
VADPCM overlay, docs/AUDIO.md) decodes the packet into a one-second ring of samples, in
records of up to 16 frames, split where the ring wraps; the state goes from record to
record through two slots of a small decoder block. The microcode switch kvid -> kgfx -> kvid
is included in its cost: ~0.11 ms per frame in kestrel64 (CPU reference: 1.2 ms).
`k64_video_audio` feeds the AI from the ring. kgfx hands its RDP command ring over, so
`k64_video_open` initialises one when the game has not.

## Status

- [x] format, encoder, Python decoder, CPU reference, demo self-test (C == Python, 96/96)
- [x] RSP reconstruct + colour conversion (microcode kvid), overlapped with the CPU parse; bit-exact with the CPU reference (kestrel64: CPU blit ~87 ms -> RSP share ~3.7 ms per frame)
- [x] whole-pixel motion vectors: demo clip 396 -> 307 KB at the same quality (YUV PSNR
      41.1 vs 41.0 dB); RSP and CPU reference both bit-exact (demo 29 checks both)
- [x] faster parse: bit position + unaligned 8-byte loads (ldl/ldr), table-driven leading
      zeros (the VR4300 has no clz; `__builtin_clzll` was a libgcc call) and a 9-bit table
      that decodes a whole {run, level, sign} token (91% of the demo clip's tokens):
      8.7 -> 3.6 ms per 320x240 frame in kestrel64. The RSP share (~6.8 ms with the colour
      conversion) is now the longer stage.
- [ ] half-pixel motion (bilinear on the RSP; the encoder side is the easy part)
- [x] VADPCM audio through the existing RSP decoder (kgfx ADPCM overlay), CRC-checked per packet

## Deferred: the RDRAM 9th bit

Idea (deferred, may lead nowhere): keep video metadata (overlay mask, chroma key, changed
blocks) in the 9th bit of each RDRAM byte. What is documented (n64brew RDRAM / RDP / VI):

- Only the RDP's colour/Z image accesses and the VI use the bit. In a 16bpp colour image the
  RDP keeps a 3-bit coverage value per pixel in the alpha bit + the two hidden bits; in the
  Z buffer they hold part of dz. TMEM loads, CPU and RSP DMA do not expose it (whether RSP
  DMA preserves or clears it on write is undocumented: hardware test pending).
- So its only consumers are the blender (memory coverage with IMAGE_READ: CVG_DEST
  clamp/wrap/zap/save, COLOR_ON_CVG) and the VI anti-alias / divot filters. The CPU parse and
  the RSP reconstruct cannot read it, so it cannot carry delta-frame or motion data.
- No bandwidth gain: the RDP reads/writes those bits on every colour access anyway, but
  writing metadata there takes an extra RDP pass (fill time) over the whole frame, while the
  video itself is written by RSP DMA (kvid) without going through the RDP.
- Masking video against 2D/3D overlays already works with ordinary means: Z compare (UI
  drawn with near Z, video rect with Z test), alpha compare, or drawing the overlay after the
  video. A coverage-based mask would only reproduce that through COLOR_ON_CVG tricks.
- Real side effect to check: the VI AA filter reads the coverage of RSP-written video pixels
  (whatever DMA leaves in the hidden bits). For video, run the VI with AA off (resample only)
  unless the hardware test shows DMA writes full coverage.

Verdict for now: nothing it offers that Z/alpha masking does not; revisit only if the
hardware test turns up something (e.g. RSP DMA writing the bit).
