# RDP modes: FILL, COPY, 1-cycle, 2-cycle

What each cycle type can draw, what it costs, and what that means for "canvas" rendering and
for cheaper triangles. Bench: `demos/09_rdp_bench` (results in the log and on screen, so the
same ROM gives hardware numbers from a flashcart).

## What each mode can draw

| mode | rate (pipeline) | primitives | per pixel |
|------|-----------------|------------|-----------|
| FILL | 64 bits/clk: 4 px at 16 bpp, 2 at 32 bpp | fill rectangles **and triangles** (edges only) | the 32-bit FILL color register; at 16 bpp it holds two pixels, so odd/even columns can differ (dither/stripes for free). No Z, blend, dither, AA, shade, texture |
| COPY | 64 bits/clk: 4 texels at 16 bpp | texture rectangles only | TMEM texels copied as is; TLUT lookup works (CI4/CI8 → RGBA16 at copy speed); alpha compare (drop alpha 0) is the only per-pixel test. 16-bpp color image only (32 bpp hangs the RDP, libdragon) |
| 1-cycle | 1 px/clk | everything | combiner + blender once, Z, AA, bilinear, perspective, LOD |
| 2-cycle | 1 px / 2 clk | everything | two combiner/blender passes (fog, trilinear, multitexture) |

Notes:

- **FILL triangles work**: any triangle command (no shade/texture/Z coefficients) rasterizes
  with the fill color; verified in kestrel64 SoftRDP and parallel-rdp (bench final frame:
  star of FILL triangles). Sub-pixel edges are honored. Good for flat geometry, shadows
  under objects, UI, polygon "flat shading" when the color changes rarely.
- **Changing the FILL color needs SYNC_PIPE** (libdragon autosync) — 50 GCLK each. PRIM color
  in 1-cycle is latched per primitive and needs none. So per-primitive colors favour 1-cycle.
- **COPY cannot scale horizontally**: it emits 4 consecutive texels per clock (dsdx must be
  4.0 in the command's terms). Vertical scaling (dtdy) and wrap/mirror work. Triangles in COPY
  mode are not supported.
- Fill/copy rectangles take inclusive bottom-right coordinates (`k64_rdp_fill_rect` adjusts).
- **2-cycle presets** (rdp.h): `K64_OM_2C_FOG_SHADE` (cycle 0 = FOG_COLOR × shade alpha +
  pixel × (1 − a), cycle 1 = pass-through) with `K64_CC_2C_TEX_SHADE` / `K64_CC_2C_SHADE`
  (cycle 0 = the equation, alpha without the shade since it holds the fog factor; cycle 1 =
  COMB). The shade alpha comes from kgfx FOG (docs/KGFX.md). The cycle-0 blend runs without
  FORCE_BLEND on hardware and prdp; kestrel64 SoftRDP skips it (reported).

## Measured (kestrel64, model) and caveats

320×240 RGBA16, VI on, framebuffer and Z in separate banks. `busy` = DPC_PIPEBUSY,
`wall` = DPC_CLOCK including time spent waiting for the CPU to write commands.
kestrel64 cdfb818 RDP model (2026-10-02).

| test | busy clk | clk/px | busy clk/prim | CPU µs to build |
|------|---------:|-------:|--------------:|----------------:|
| fill rect | 21 150 | 0.27 | | 1 |
| fill 2 tris | 21 670 | 0.28 | | 18 |
| 1-cycle rect | 92 457 | 1.20 | | 2 |
| 1-cycle 2 tris | 93 017 | 1.21 | | 18 |
| 2-cycle rect | 169 257 | 2.20 | | 1 |
| copy 80 × 32×32 sprites | 22 670 | 0.29 | | 128 |
| copy one wrapped rect | 21 062 | 0.27 | | 4 |
| fill canvas 16×16 (color change + SYNC_PIPE per block) | 41 050 | 0.53 | 136 | 344 |
| fill canvas 8×8 | 91 150 | 1.18 | 75 | 1 407 |
| fill canvas 4×4 | 278 350 | 3.62 | 57 | 5 732 |
| prim canvas 8×8 (1-cycle, PRIM per block) | 167 185 | 2.17 | 139 | 1 121 |
| prim canvas 4×4 | 186 560 | 2.42 | 38 | 4 469 |
| prim canvas 2×2 | 440 320 | 5.73 | 22 | 17 924 |
| 1-cycle 2 tris per 16×16 cell (CPU setup) | 108 600 | 1.41 | 362 | 5 385 |
| 1-cycle 2 tris per 8×8 cell | 140 400 | 1.82 | 117 | 21 535 |
| 80×60 canvas upscaled ×4 (1-cycle bilinear texrects) | 92 673 | 1.20 | | 18 |

What the model now contains (kestrel64 docs/RDP-TIMING.md, "FILL/COPY path and per-line
cost"), all from documented behaviour, no fitted constants:

- FILL/COPY bypass the span buffer and write 64 bits per GCLK straight to RDRAM: ~4.4× the
  1-cycle rate, in line with libdragon's "approximately 4 times faster".
- Every line of every primitive pays 1 dead pipeline GCLK (n64brew).
- 1/2-cycle span-buffer chunks (8 px) are rounded per line: a 2-px-wide line pays a chunk.

Caveats:

- The 1/2-cycle constants are calibrated on Thar0's RDP-Timing-Tests; FILL/COPY follow
  documented rates but no hardware measurement of them exists in the sources.
- **No per-primitive setup cost** (edge walker, command fetch) is modelled — no source gives
  a number. Small primitives are still somewhat cheaper here than on hardware. Running 09 on
  a flashcart would be the first hardware data point for both.

## 3D geometry in FILL mode (demo 13_fillgeo)

Can flat-shaded 3D (Star Fox style: one lit color per face, no texture) use the cheap modes?
COPY cannot (rectangles only); FILL can: kgfx with GEOMMODE 0 (no shade / texture / Z
coefficients) emits edge-only triangles, which the RDP fills with the FILL color. Without Z,
correct order comes from the painter's algorithm: convex, non-intersecting objects, back-face
culled, sorted back to front on the CPU (one sort per frame over objects, not triangles).

Scene: floor + 30 spinning cubes, ~177 triangles drawn, ~89 face colors (a face color change
is `SYNC_PIPE` + `SET_FILL_COLOR` in FILL, `SET_PRIM_COLOR` in 1-cycle, sent as kgfx RDP
commands between the triangles). kestrel64 cdfb818 model, 320×240 RGBA16, average of 64
frames, color + Z clear included:

| mode | RDP busy clk | RDP ms | vs Z1C | RSP µs |
|------|-------------:|-------:|-------:|-------:|
| Z1C: 1-cycle + Z, PRIM color, front to back | 395 934 | 6.33 | 1× | 1 387 |
| P1C: 1-cycle, no Z, PRIM color, painter | 140 858 | 2.25 | 2.8× | 1 061 |
| FILL: no Z, SYNC_PIPE + FILL color per face, painter | 58 024 | 0.93 | 6.8× | 1 068 |
| clear only | 21 150 | 0.34 | | |

Reading:

- **Z is the expensive part**, not the cycle type: dropping Z (Z clear + per-pixel Z
  read/write) is 2.8×, with the same 1-cycle pixels. Any scene that can be ordered on the
  CPU (objects that do not interpenetrate, or a BSP / portal order) gains this first.
- **FILL on top is another 2.4×** for the triangles: 89 pipe syncs (~4.5k clk) are noise
  against the fill rate. Consecutive faces with the same color skip the sync.
- **The RSP becomes the bottleneck**: in FILL the RDP is busy 58k clk of a 127k clk wall;
  it waits for kgfx triangle setup (1.07 ms, about 67k GCLK). Without Z, kgfx also skips the
  Z coefficients (RSP 1.39 → 1.07 ms). More FILL geometry therefore costs RSP setup, not fill.
- Mixing works: FILL for the flat, unlit, far or background parts (terrain, buildings,
  shadows as flat polygons drawn before the objects), 1-cycle for what needs Gouraud,
  texture or blending. A mode switch is SYNC_PIPE + SET_OTHER_MODES (~50 clk).

Limits of FILL geometry: one color per primitive, no Z, no shading, texture, blending, dither
or anti-aliasing (hard, aliased edges); 16 bpp lets odd/even columns differ (two-pixel fill
color) for a dither-like pattern. Same caveat as above: no hardware measurement yet, and no
per-primitive setup cost in the model (this favours many small FILL triangles).

Demo detail: raw RDP commands sent through kgfx are invisible to the CPU's RDP state tracking
(`k64_rdps`), so after `k64_kgfx_wait` the demo invalidates the tracked fill / prim colors;
otherwise the next frame's clear skips its SET_FILL_COLOR and fills with the last face color.

## "Canvas" rendering: a 3D game from many small squares?

What the bench shows, independent of the exact constants:

1. **The squares' colors have to be computed somewhere** — that is the renderer itself (ray
   casting, voxel terrain, SDF, a software rasterizer). The RDP only blits the result.
2. One primitive per square costs a command (8 B rect + 8 B color) the CPU or RSP must write,
   plus per-line RDP overhead. At 4×4 blocks (80×60 effective) that is 4800 primitives
   and ~77 KB of commands per frame; the CPU alone needed 4.5 ms just to write them. FILL
   blocks are cheap to rasterize but every color change adds a 50-GCLK pipe sync: FILL wins
   at 8×8 and above (1.18 vs 2.17 clk/px), PRIM-colored 1-cycle rects win at 4×4 (2.42 vs
   3.62). Both are far from the 0.27 clk/px of one big FILL or COPY.
3. **The fast path is a texture**: compute the canvas at low resolution into RDRAM (16-bit, or
   8-bit indices to halve the bandwidth) and let the RDP upscale it:
   - 1-cycle bilinear texture rectangles in TMEM-sized strips: ~1.2 clk/px, ~1.5 ms full screen,
     smooth upscale (measured above, 3 loads + 3 rects).
   - COPY with CI8 + TLUT and dtdy = 0.5 (vertical ×2), horizontal ×2 by writing each index
     twice (or in a 2-pixel FILL-color trick): the fastest blit, ~0.3 clk/px, no filtering —
     4× cheaper than the bilinear upscale, so ~1.1 ms of RDP time per frame are freed.
4. So the canvas approach is a real option for *non-polygonal* renderers whose per-pixel work
   runs on the RSP: budget at 30 fps is ~2 M RSP cycles × 8 lanes per frame — ~3400 lane-ops
   per pixel at 80×60, ~850 at 160×120. Candidates: the SDF ray marcher (ROADMAP), a
   Comanche-style voxel terrain (column ray casting, ~10–20 ops per sample), Wolfenstein/Doom
   style column renderers. For polygon games, triangles at 1 px/clk are still the best use of
   the RDP.

## Cheaper triangles

An RDP triangle command is self-contained: edges (32 B) + shade (64 B) + texture (64 B) + Z
(16 B) = up to 176 B, no strips or fans, nothing carried over from the previous triangle. The
rasterization cost per pixel depends only on the mode. What can be saved:

- **RSP setup is the real cost.** kgfx profile with `KESTREL_RSP_TIMING=1` (06_kgfx_lit,
  `tools/prof_rsp.py --timing`): triangle setup (`tri*`) ≈ 75% of RSP cycles, vertex
  transform ≈ 13%, lighting ≈ 2%; IPC 0.95.
- **Coplanar quads share gradients.** Across a plane, s/w, t/w, 1/w, z and flat or linearly
  lit colors are linear in screen space, so both triangles of a quad have *identical*
  DxDx/DxDy coefficients. A QUAD command (or TRI2 detecting a shared plane) computes the
  gradient block once and only redoes edges and start values: roughly half the setup for
  walls, floors, sprites, terrain cells.
- **Shared edges in strips/fans**: the inverse slope of an edge is reused by the next
  triangle; the RSP keeps the last edge's 1/dy. (kgfx: ~0 gain, the reciprocals already pair
  with scalar work; KGFX.md.)
- **Drop unused coefficient blocks** (kgfx already does, per GEOMMODE triangle type):
  flat-shaded geometry uses PRIM color (no 64 B shade block), untextured skips the texture
  block, no-Z skips 16 B. Fewer bytes to DMA and write.
- **Rectangles where possible**: screen-aligned quads (particles, unrotated billboards, HUD,
  text) as texture rectangles: 16 B and no setup at all.
- **FILL triangles** for flat, unlit, unblended geometry: the 4 px/clk path (on hardware).
- **Don't draw it**: back-face, frustum and occlusion culling, and front-to-back order
  with Z (failing pixels skip the color/Z writes — Thar0's tests show the gain).

Follow-ups (ROADMAP): FILL / no-Z geometry path in kgfx (demo 13 measured it); QUAD/planar gradient reuse and edge reuse in kgfx; CI8 COPY canvas
test; voxel-terrain canvas demo; hardware run of 09.
