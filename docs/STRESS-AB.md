# Geometry stress A/B: tiny3d on libdragon vs kestrel64-sdk

Same scene written natively on each side, no compat layer involved:

- **A**: `bench/t3d_stress` — tiny3d on libdragon (preview, prebuilt toolchain), tiny3d's
  own microcode. Sphere recorded once as an rspq block; per copy `t3d_matrix_set` +
  `rspq_block_run`. Two rows (32 vertices) per `t3d_vert_load`, every vertex loaded once.
- **B**: `demos/16_stress` — kestrel64-sdk, kgfx microcode. Sphere recorded once as a kgfx
  sub-list; per copy `k64_kgfx_mtx_lights` + `k64_kgfx_call`. One row (16 vertices) per
  VTX, every vertex loaded once.

Common: sphere of 16 rows x 16 vertices (450 triangles), copies on the same grid, same
camera, ambient + 2 directional lights, z-buffer, back-face culling, opaque 1-cycle
shade, 320x240x16, three framebuffers. The copies spin with a different rotation order on
each side (does not change the cost of a sphere).

Method: kestrel64 b2aec21+ build-static, `tools/stress_ab.py` (press D-Up to the copy count,
settle, `rdp.stats` over 120 VI fields). Emulator figures: kestrel64's RSP/RDP timing is
not calibrated against hardware.

## Results

| copies | sent tris | A ms/flip | B ms/flip | A RSP busy | B RSP busy | A tris drawn | B tris drawn |
|---|---|---|---|---|---|---|---|
| 8  | 3600  | 16.68 | 16.68 | 87.4% | 69.4% | 1571 | 1413 |
| 16 | 7200  | 28.60 | 25.34 | 100%  | 94.4% | 3246 | 2977 |
| 32 | 14400 | 55.61 | 51.33 | 100%  | 96.1% | 6561 | 6203 |

- Per sent triangle (same geometry on both sides) the frame costs A 3.97 / 3.86 µs and
  B 3.52 / 3.56 µs at 16 / 32 copies: **B is 8–11% faster**.
- A reports ~10% more triangles drawn for the same spheres: the pole rows are degenerate
  (zero-area) triangles, 30 per sphere, which kgfx culls and tiny3d sends to the RDP.
- With two framebuffers B lost: `k64_display_show` waits for the vblank, so a 24 ms frame
  became 33.4 ms and the RSP sat idle 28% of the time. 16_stress now uses three.

## Sustained rate (this scene, emulator)

~280k sent / ~120k drawn triangles per second on B, i.e. ~2000 drawn lit triangles per
frame at 60 fps, ~4000 at 30 fps. The RSP is the limit; the RDP is 10–25% busy.
