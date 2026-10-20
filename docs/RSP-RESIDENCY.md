# RSP residency — keeping data in IMEM/DMEM across overlays and microcodes

IMEM and DMEM are SRAM inside the RSP: they keep their contents across halts, task switches
and microcode changes. A switch costs only the bytes the other code **overwrote**, not the
size of the microcode. The design goal is to make that set as small as possible, so the
RSP reads RDRAM almost only for real input (commands, vertices, samples).

## Where the bytes go today (kgfx)

| Item | Before | Now |
|------|--------|-----|
| IMEM text | 2256 B | 2256 B |
| DMEM image | 2352 B (2 KB of it zeros: vertex cache, input buffer, command buffer) | **208 B** (constants, viewport, matrix, jump table, ring/mode words) |
| Full reload | 4608 B | **2464 B** |

The scratch areas are now `.equ` addresses outside the image: they are always written
before being read, so loading zeros from RDRAM was wasted bandwidth.

Scale: a 320×240 16-bit frame moves roughly 150 KB of color + 300 KB of Z read/write + 150 KB
of VI scanout, plus textures. One microcode switch is ~0.5–1 % of that. The cost that matters
more is the **serial latency**: while the RSP waits for an IMEM/DMEM DMA it is not feeding
the RDP. So the order of priority is: don't load what is still there → load less → overlap
what must be loaded with useful work.

## DMEM partition (4 KB)

| Range | Owner | Lifetime |
|-------|-------|----------|
| 0x000–0x0FF | kgfx header: constants, viewport, matrix, jump table, ring/mode words | persistent; audio never writes it |
| 0x100–0x8FF | kgfx scratch: vertex cache, input vertices, command chunk | dead after `END` |
| 0x900–0xCFF | kgfx OUTBUF (RDP staging, empty after `END`) | dead after `END` |
| 0xD00–0xE6F | kgfx state set by commands: LIGHTS, lighting scratch, TEXGEN, CLIPFLAGS, RETADDR (see KGFX.md) | LIGHTS/TEXGEN live until changed; scratch/CLIPFLAGS dead after `END` |
| 0xE70–0xFFF | free (400 B) → audio persistent state (voice ADPCM/resampler history, envelope state) | persistent; kgfx never writes it |

Audio uses 0x100–0xCFF (3 KB) as its sample buffers while it runs: that area is dead for
gfx between lists. The two persistent zones mean that a gfx → audio → gfx sequence reloads
**no DMEM image at all**: only the IMEM that the other code replaced.

Implementation: `k64_rsp_load` keeps a residency record per region (IMEM text tag, DMEM
header tag, audio state tag) instead of the single `k64_rsp_loaded` pointer, and skips each
region whose tag still matches. The RSP side of each microcode never writes outside its
regions (checked by `k64rsp.py` against a region map: a store/DMA destination range
assertion in the listing pass).

## IMEM: kernel + overlays

kgfx is 3.3 KB now with stage D clipping (the clipper itself is ~0.8 KB: `tri_clip` ..
`clip_isect`). Lighting and the 2D/sprite path will not fit next to it, nor next to audio,
so IMEM becomes:

| Range | Content |
|-------|---------|
| kernel (~0.6 KB) | command fetch/dispatch, DMA helpers, RDP ring flush, overlay loader |
| slot A (~2 KB) | hot overlay: vertex transform + triangle setup (always resident during gfx) |
| slot B (~1.4 KB) | cold overlays, one at a time: clipping, lighting, sprite/2D, audio mixer |

Rules:

- **Tag check before DMA**: each overlay has an ID; the loader compares it with the resident
  ID of the slot and skips the load when equal. Same for the DMEM data of the overlay.
- **CPU-side batching**: command lists are built grouped by overlay (all clipped triangles
  of an object together, all sprites together), so a frame needs a handful of swaps, not
  one per primitive. The model baker orders batches the same way.
- **Prefetch**: the RSP can DMA into slot B while it executes from slot A. The list
  carries a `PREFETCH overlay` hint ahead of the first command that needs it; the loader
  starts the DMA and only waits on `DMA_BUSY` when it actually jumps into the slot.
- **Clipping as a cold path**: most triangles never clip; the guard band (stage D) keeps it
  that way, so the clip overlay is loaded only for the few objects that cross the near plane
  (the CPU knows from its bounding-sphere test and marks them).

## Audio: same kernel, frame-boundary scheduling, no yield

libultra preempts the gfx task (yield) to run audio on time, which forces gfx to save its
live DMEM (~3 KB yield buffer) to RDRAM and read it back. kestrel64-sdk avoids that:

1. Audio runs at a **frame boundary**, after the gfx list's `END` (OUTBUF flushed, vertex
   cache dead) → nothing to save.
2. The audio output buffer holds ≥ 1 frame + margin (e.g. 2 × 735 samples at 44.1 kHz /
   60 Hz) so audio never needs sub-frame preemption.
3. Audio is an **overlay of the same kernel**, entered through an `AUDIO` command appended
   to the frame's command stream: no CPU halt/restart, no kernel reload, no header reload.
   Per frame the RSP reloads slot B twice at most (~1.4 KB each way).
4. If a game ever needs a mid-frame audio slot, the gfx list contains explicit
   `YIELD_POINT` commands placed by the CPU where the vertex cache is dead (between meshes)
   → the yield saves only the ring cursor and mode words (< 64 B).

Audio state that must survive frames (ADPCM predictors, resampler phase, envelope) lives in
0xE70–0xFFF for the hot voices; the rest round-trips to RDRAM per voice, as in any mixer.

## Steps

1. ~~Drop scratch zeros from the kgfx DMEM image~~ (done: 2352 → 208 B).
2. Region tags in `k64_rsp_load` (IMEM / DMEM header / audio state) + region map check in
   `k64rsp.py`. **Deferred**: with audio as a kgfx overlay (step 4) the SDK runs a single
   microcode, so there is no switch to save. Do it with the first second microcode (canvas /
   SDF): images then need a DMEM load offset (today every image lands at 0x000) and a
   declared write range per ucode, and `k64_rsp_load` reports which regions it reloaded so
   kgfx resets `OVL_RESIDENT` when only IMEM came back.
3. Kernel/overlay split — **done for kgfx** (2026-10-02): one slot, VTX (default occupant,
   part of the image) ↔ clip overlay, tag check in `ovl_load`; `.overlay` sections in
   k64rsp.py (KGFX.md "Overlays"). Still open: `PREFETCH` hint, CPU-side grouping of clipped
   objects.
4. ~~Audio overlay + `AUDIO` command~~ (done 2026-10-02, docs/AUDIO.md): the mixer is a
   616 B overlay of the kgfx slot, entered by `AUDIO`; the CPU keeps all voice state, so no
   persistent DMEM zone was needed (0xE70–0xFFF stays free). The overlay table became
   RDRAM addresses + a length table (header had 4 B left).
