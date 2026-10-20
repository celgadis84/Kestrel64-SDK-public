# Particles and billboards

Three kinds of camera-facing things, each on its cheapest path (design: MODELS.md §3).
Demo 21_particles shows the first two and self-tests them against a CPU projection; demo
23_impostor does the third.

| Kind | API | Path |
|------|-----|------|
| Screen-aligned sprites (sparks, dust, smoke) | `k64_kgfx_particles` (kgfx op 0x10) | RSP overlay: one point through the matrix, one RDP rectangle |
| Y-axis billboards (trees, lamps) | `k64_kgfx_billboard_y` (CPU helper) | 4 vertices facing the eye around Y, then the normal triangle path |
| Impostors (far LOD of big objects) | `k64_impostor_draw` (`.k64i`, `IMPOSTORS`) | host renders N views; pick the closest per instance, draw it as a Y billboard |

## PARTICLES command

A block (`k64_particles_t`, 16-byte header + 16 bytes per particle) in RDRAM:

```c
typedef struct { s16 x, y, z; u16 size; u8 r, g, b, a; s16 s, t; } k64_particle_t;
blk->scale = k64_kgfx_particles_scale(&mvp, &vp);   // after k64_viewport_wnorm
blk->count = n; blk->flags = K64_PART_TEX | K64_PART_Z | K64_PART_COLOR;
blk->tile = 0; blk->tex_shift = 4;                  // 16x16 texel square per particle
k64_dcache_writeback(blk, 16 + n * 16);
k64_kgfx_particles(&l, blk);
```

Per particle the microcode:

1. transforms the center with the current matrix (two particles per vector pass, lanes 0-3
   and 4-7), W-normalizes like `VTX`, takes 1/w with one Newton-Raphson step;
2. drops it when z is outside the near / far planes (no clipping: a particle is a point);
3. radius = `size * scale / w` in s13.2 pixels, where `scale` folds the matrix's x scale and
   the viewport (a sphere's projected size, so it does not stretch with the aspect ratio);
4. drops it when the square is off the 0..1023 coordinate range or the radius is 0, clamps
   the right / bottom edge, and on the left / top edge cuts the rectangle and moves the
   texture start (s, t) by the cut times the texel step;
5. emits `PRIM_DEPTH` (flag Z, the center's screen z), `PRIM_COLOR` (flag COLOR, only when it
   differs from the previous particle's) and either `TEXTURE_RECTANGLE` of the `2^tex_shift`
   texel square at (s, t) (flag TEX, step = 2^shift / (2 * radius)) or `FILL_RECTANGLE` (the
   1-cycle combiner's color, e.g. `K64_CC_PRIM`).

Modes are the caller's (set before `k64_kgfx_run`, or with a kgfx `RDP` command): for z, use
`K64_OM_Z_SOURCE_PRIM` with `K64_OM_Z_COMPARE` (usually without update, so particles do not
hide each other); for shaped sprites, `K64_OM_ALPHA_COMPARE`. One atlas of 2^shift squares
serves several sprites (demo 21: 4 sprites of 16 texels in a 32x32 RGBA16 atlas, chosen by
s, t). `k64_kgfx_part_count()` after `k64_kgfx_wait` = rectangles emitted.

The code is an IMEM overlay over the VTX slot (928 B, `.overlay part`), so it costs no
resident IMEM; the next `VTX` reloads VTX. Group particle blocks after the geometry.
It reads the particles in chunks of 30 into INBUF and uses the clip-list area (0x7E0) as
scratch: the vertex cache survives, the clip pool does not.

### Cost (kestrel64, `KESTREL_RSP_TIMING=1`, demo 21)

768 particles (TEX + Z + COLOR, color changes on every particle): ~62k RSP cycles per task
(the billboard trees included) = ~72 cycles (~1.15 µs) per particle; the first version took
~107k (~139 per particle). The loop is software pipelined: each iteration runs the vector
pass of pair N (transform, 1/w, radius, 1/radius, clamps, and the RDP words packed in
vector lanes: `cmd << 8 | x >> 4`, `(x & 15) << 12 | y`) and the branch-free scalar emit of
pair N - 1 (PRIM_DEPTH / PRIM_COLOR / rectangle always stored, `outptr` advanced by masks),
interleaved by hand-checked dual issue (~122 modelled cycles per pair,
`tools/rsp_cycles.py --overlay part`). Only rectangles cut at the left / top edge leave the
straight line (`bltzal` to the shift-add st correction). The RDP fill of the 768 small
squares (~5 ms) dominates the frame anyway.

## Y billboards

`k64_kgfx_billboard_y(v, base, half_w, height, eye, rgba, s0, t0, s1, t1)` writes 4
vertices standing on `base`, turned around Y toward `eye` (all in model units). Corners
0..3 = bottom-left, bottom-right, top-right, top-left as seen from the eye, so
`k64_kgfx_tri2(l, 0, 1, 2, 0, 2, 3)` is front-facing (back-face cull keeps it). The normal
points at the eye, so lighting and fog work, and the triangles are perspective-correct and
z-buffered like any mesh. Four billboards fill 16 cache slots (one `VTX`).

Building them on the CPU costs a sqrt and a divide per billboard (~1 µs); an RSP version
would only pay off for hundreds per frame (grass), and then a dedicated overlay like
PARTICLES (center + size in, two triangles out) is the path.

## Impostors

A far copy of a big model drawn as one textured quad. `tools/k64bake.py impostor` renders
the glTF model (bind pose) from N directions around its vertical axis into N small
textures; at run time each instance is a Y billboard through the model's axis, textured
with the view baked closest to the direction it is seen from.

```cmake
k64_add_rom(NAME game SOURCES main.c
  IMPOSTORS "trex_imp=${K64_SDK_DIR}/assets/trex/trex_walking_seamless.glb:--views 8 --cell 64x32")
```

```c
k64_impostor_t im;
k64_impostor_init(&im, trex_imp, 0, 0);                 // tile 0, TMEM 0
...
k64_impostor_draw(&l, &im, herd, n, eye, 0xFFFFFFFF, vbuf);   // vbuf: n * 4 vertices
k64_kgfx_run(&l); k64_kgfx_wait();
k64_tex_tmem_forget();                                  // the list loaded TMEM behind tex.c
```

Modes: z buffer, alpha compare (the cells are cut out with alpha 0/1), bilinear,
`K64_CC_TEX_SHADE` (the vertex color tints the baked lighting, white keeps it), back-face
culling is fine (the quad faces the eye).

### Bake (`tools/k64impostor.py`)

- view k is an orthographic render from the horizon at phi = 2 pi k / N, measured from +Z
  toward +X; its right is (cos phi, 0, -sin phi), the right of a billboard facing that way;
- numpy z-buffer rasterizer, 4x4 supersampled; albedo from the model's material atlas,
  Lambert from a fixed model-space light (`--light`) plus `--ambient`;
- alpha = coverage >= 1/2, and the color of transparent texels is bled in from their opaque
  neighbours so bilinear filtering does not pull a dark fringe in at the cut;
- the quad is the same for every view: half width = the largest XZ distance from the
  bounding-box center (any view fits), height = the Y extent;
- options `--views` (<= 32), `--cell WxH` (one cell must fit TMEM: 4 KB), `--format`
  (rgba16, ci8, ci4), `--preview sheet.png`.

`.k64i` (big-endian): `'K6I1'`, u16 views, u16 0, u32 stride, f32 half_w, height, y0, cx,
cz, then the cells as `.k64t` blobs at `32 + k * stride`. ~20 s for the 10.9k-triangle
T-rex at 8 views.

### Run time (`src/gfx/impostor.c`)

`k64_impostor_init` builds each view's TMEM load once (`k64_tex_load_cmds`: the RDP
commands of `k64_rdp_load_tex`, syncs included). Per instance `k64_impostor_draw` turns the
eye direction back into model space (`rotate_y(-yaw)`), picks the view with the largest
dot product and writes the billboard; the quads are then permuted in place so that they
are grouped by view, and each view in use costs one load plus `VTX` / `TRI2` (8 quads per
`VTX`). The load bypasses tex.c's TMEM cache, hence `k64_tex_tmem_forget` after the list.

Demo 23 (64 T-rexes, 8 views of 64x32 RGBA16, kestrel64): 8 loads and ~120 triangles a
frame instead of 64 x 10.9k, ~250k RDP clocks, ~640 µs CPU in `k64_impostor_draw`
(~10 µs per instance: two sines, the view dot products, the billboard's sqrt and divide).
The self-test checks the view choice, a head-on render against the texel alphas and the
load grouping.

Not done: elevation rows (views from above), cross-fade between views, a distance switch
from the mesh (the game's LOD choice).
