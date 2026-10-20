# kgfx — RSP geometry microcode

Source: `src/rsp/kgfx.S` (assembled by `tools/k64rsp.py`, linked into libk64).
C side: `include/k64/kgfx.h`, `src/gfx/kgfx.c`. Self-test: `demos/04_rsp`. Demos: `demos/05_kgfx_cube`, `demos/06_kgfx_lit`, `demos/07_kgfx_envmap`, `demos/08_kgfx_room` (camera inside a textured lit room: the near plane cuts walls and pillars every frame; ~120 tris, rsp ~1.5 ms, rdp ~470k clk, soft and prdp clean).

## Status

| Stage | Content | State |
|-------|---------|-------|
| A | command fetch, MTX, VIEWPORT, VTX (transform, 1/w, screen), END | done — 04_rsp PASS |
| B | TRI/GEOMMODE: RSP triangle setup -> RDP commands (shade/tex/z), culling, RDP ring handoff | done — 04_rsp PASS |
| C | cube/mesh demos on kgfx, CPU vs RSP timing | 05_kgfx_cube (15 cubes) — mesh demos next |
| D | clip flags, near/far + guard-band clipping on the RSP | done — 04_rsp PASS |
| E | directional lighting (4 lights + ambient, packed normals) | done — 04_rsp PASS, 06_kgfx_lit |
| E2 | texture coordinate generation (spherical environment map) | done — 04_rsp PASS, 07_kgfx_envmap |

## Execution model

The CPU records 8-byte commands into an RDRAM buffer (`k64_kgfx_*` inline builders) and
calls `k64_kgfx_run()`: the list is written back from D$, its physical address goes to
DMEM `CMDPTR`, the RSP starts at `START`. The microcode DMAs the list in 256-byte chunks,
dispatches through a 16-entry jump table (opcodes 0x10..0x1F) and halts with `break` on
END (`k64_kgfx_wait()` polls SP halt, returns DMEM `STATUS`).

Triangles go straight to the RDP. `k64_kgfx_run()` exports the CPU's RDP ring state
(`k64_rdp_ring_export`: 4 block addresses, current block, cursor, end, DPC_START issued) into
DMEM `RING`. The RSP stages RDP commands in one half of DMEM `OUTBUF` (2 × 704 B). When the
next triangle might not fit (and at END) it starts the DMA of that half to the ring cursor
and goes on in the other half; the next flush (or END) waits for the DMA and advances
`DPC_END` — the RDP starts rasterizing while the RSP is still on later triangles. When a ring block is full the RSP waits
for `DPC_STATUS.START_VALID` to clear and moves to the next block (same protocol as the CPU
side, `src/gfx/rdp.c`). `k64_kgfx_wait()` imports the ring back (`k64_rdp_ring_import`) so CPU
RDP commands continue after the RSP's; the D$ line at the new cursor is invalidated since the
RSP wrote it behind the cache.

The microcode is loaded once (`k64_rsp_loaded` tracks the resident IMEM image); vector
registers holding the matrix/viewport are rebuilt from DMEM at each start, so another
microcode may run in between as long as it reloads kgfx.

## Commands

Opcode in bits 63..56. Addresses are physical RDRAM (`K64_PHYS`), 8-byte aligned.

| Op | Name | Encoding | Effect |
|----|------|----------|--------|
| 0x10 | PARTICLES | lo = addr of a `k64_particles_t` block | one RDP rectangle per particle (overlay, [PARTICLES.md](PARTICLES.md), `k64_kgfx_particles`) |
| 0x11 | MTX | hi bit 0 = `K64_KGFX_MTX_LIGHTS`; lo = addr | load 64-byte s15.16 matrix (MVP); with the flag, also the 160-byte light block right after it (`k64_kgfx_mtxlights_t`, `k64_kgfx_mtx_lights`) |
| 0x12 | VTX | hi bits 23..16 = dest, 15..8 = count, bit 0 = COLIDX; lo = addr | transform `count` vertices into cache slots `dest..`; with COLIDX each vertex rgba is replaced by the colour table entry its `a` byte points to (`k64_kgfx_vtx_colidx`). Count 0 = COL: DMA 264 B of colours from the CALL colour base + lo into the colour table (`k64_kgfx_col`) |
| 0x13 | TRI | hi bits 23..0 = vertex refs a, b, c (0x20 + slot * 5 = DMEM address / 8); lo bit 24 set -> second triangle in lo bits 23..0 | set up 1 or 2 triangles (`k64_kgfx_tri`, `k64_kgfx_tri2`) |
| 0x14 | VIEWPORT | lo = addr | load 32-byte viewport |
| 0x15 | GEOMMODE | hi bits 2..0 = tile, bits 5..3 = mip levels - 1 (RDP triangle `level`); lo bits 2..0 = `K64_TRI_*` (Z 1, TEX 2, SHADE 4), lo bits 9..8 = `K64_KGFX_CULL_BACK/FRONT`, lo bit 10 = `K64_KGFX_LIGHTING`, lo bit 11 = `K64_KGFX_TEXGEN` | triangle type for following TRIs; lighting / texgen for following VTXs |
| 0x16 | LIGHT | lo = addr | load 160-byte `k64_kgfx_lights_t` |
| 0x17 | TEXGEN | lo = addr | load 112-byte `k64_kgfx_texgen_t` |
| 0x18 | CULL | hi bits 23..0 = commands to skip; lo = mask of vertex cache slots | skip those commands when every slot in the mask is outside one and the same frustum plane (`k64_kgfx_cull` / `k64_kgfx_cull_end`) |
| 0x19 | CALL | hi bits 23..0 = colour base (RDRAM); lo = RDRAM address of a command list | run it until RET, then continue (one level; prebaked model lists, `k64_kgfx_call`, `k64_kgfx_call_col`) |
| 0x1A | RET | | return from CALL |
| 0x1D | FOG | hi bits 23..16 = 255 − max (0xFF: off); hi bits 15..0 = Kf; lo = start << 16 \| Ki | following VTXs write a fog factor into the vertex alpha (`k64_kgfx_fog` / `k64_kgfx_fog_max` / `k64_kgfx_fog_off`) |
| 0x1E | RDP | lo = RDRAM address of raw RDP commands; hi bits 7..0 = bytes - 1 (<= 256, multiple of 8) | flush the pending triangles, then DMA the commands into OUTBUF: modes, texture loads and rectangles in list order (`k64_kgfx_rdp`) |
| 0x1F | END | | halt |

Unknown opcodes in 0x10..0x1F (or outside the range) stop the list with
`STATUS = 0x100 | opcode`.

## Formats

**Matrix** (64 B): s15.16, column-major `m[col][row]`; 16 × s16 integer halves, then
16 × u16 fraction halves (`k64_mtxfx_t`, `k64_mtx_to_fixed`). Vertex positions are s16 model
units, so fold the model scale into the matrix (e.g. ±4096 units = ±1.0 with a 1/4096 scale).

**Input vertex** (16 B, `k64_vtx_t`): s16 x, y, z; u16 norm (packed normal, used only with
lighting: x bits 15..11, y 10..5, z 4..0, two's complement, `k64_kgfx_pack_normal`, tiny3d's
encoding); u8 r, g, b, a; s16 s, t (s10.5 texels).

**Viewport** (32 B, `k64_viewport_t`): s16 scale[8], trans[8] as (x, y, z, 0) twice.
screen = trans + NDC × scale. x/y in s13.2, z in 0..0x7FFE. `k64_viewport_set(w, h)`.

**Vertex cache entry** (40 B, `k64_kgfx_vcache_t`), 32 entries:

| Offset | Content |
|--------|---------|
| 0x00 | s16 clip x, y, z, w (integer) |
| 0x08 | u16 clip x, y, z, w (fraction) |
| 0x10 | s16 screen x, y (s13.2), z, 1/w integer |
| 0x18 | u16 1/w fraction, u16 clip flags (`K64_KGFX_CLIP_*`), u32 pad |
| 0x20 | u8 r, g, b, a; s16 s, t (copied from input) |

## Math

Two vertices per pass (A in vector elements 0..3, B in 4..7); matrix columns are duplicated
in both halves and inputs broadcast with `[Nh]`.

- clip = M × (x, y, z, 1): `vmudn/vmadh` pairs, exact (48-bit accumulator, no rounding).
- 1/w: `vrcph/vrcpl/vrcph` on the 32-bit w gives 2^31/W (= 0.5/w in s15.16), ×2, then one
  Newton-Raphson step x1 = x0 (2 − w x0). Measured error ≤ 1 lsb of s15.16 (04_rsp).
- NDC = clip × 1/w (32×32 → s15.16 via `vmudl/vmadm/vmadn/vmadh`), screen = trans + NDC × scale.

- screen z is clamped to ≥ 0 (near-plane vertices round to −1, −2; the triangle setup needs
  z deltas inside s16).
- clip flags from `vch`/`vcl` against ±W (low byte: C ≤ −W, high byte: C ≥ W) and ±4W
  (guard band, 4W saturated to s32):

| Bit | Name | Condition |
|-----|------|-----------|
| 0x01 / 0x10 | CLIP_NX / CLIP_PX | x ≤ −w / x ≥ w |
| 0x02 / 0x20 | CLIP_NY / CLIP_PY | y ≤ −w / y ≥ w |
| 0x04 / 0x40 | CLIP_NEAR / CLIP_FAR | z ≤ −w / z ≥ w |
| 0x100 / 0x1000 | GUARD_NX / GUARD_PX | x ≤ −4w / x ≥ 4w |
| 0x200 / 0x2000 | GUARD_NY / GUARD_PY | y ≤ −4w / y ≥ 4w |

Measured in 04_rsp (24 random sets incl. 8 with near/far crossings, 768 vertices vs CPU
reference): clip exact, flags exact, 1/w ≤ 2 lsb, screen x/y ≤ 1 s13.2 unit (0.25 px), z ≤ 2.

### Triangle setup (TRI)

Same output as the CPU reference `k64_rdp_tri` (`src/gfx/tri.c`); method after libdragon's
`rsp_rdpq_tri.inc`.

- Sort the 3 vertices by screen y (tracking the swap parity), edges H (1→3), M (1→2), L (2→3)
  in s13.2. NZ = MX·HY − HX·MY (32-bit); NZ = 0 → dropped. Major-side flag `lft` = NZ > 0;
  front/back = `lft` xor parity, then GEOMMODE culling.
- Reciprocals of HY, MY, LY and NZ with `vrcp*` + one Newton-Raphson step, kept at half scale
  (2^31/x): 2^32/x does not fit 32 bits for x = 1, 2 (edges 1/4 or 1/2 px tall). Products are
  doubled afterwards.
- NZ is normalized first: NZ' = NZ >> k with |NZ'| < 2^16 (k = bit length of |NZ| >> 16,
  scalar binary search), and the attribute numerators are shifted by the same k
  (× 0x10000 >> k, skipped for k = 0). Without it 2^31/NZ keeps only ~7 significant bits on
  guard-band triangles (NZ ~ 2^24); with it 15–16 bits (~4e-5 relative) for any size.
- Attribute lanes: r g b a s t w z. Per attribute: DX = (MA·HY − HA·MY)/NZ,
  DY = (HA·MX − MA·HX)/NZ, DE = DY + DX·ISH, base = A1 + DE·FY (FY = sub-scanline offset).
- Perspective texture: s, t are pre-multiplied by n = 1/w · min(w) (n ≤ 1, clamped to
  0x7FFF) so the RDP divides by the normalized w lane.
- Before setup: AND of the three vertices' flags & `CLIP_REJECT` (0x77) → dropped (all
  outside one frustum plane); OR & `CLIP_CUT` (0x3344: near, far, guard band) → clipped (below);
  otherwise set up directly — triangles crossing only the ±w x/y planes are left to the RDP
  scissor, inside the 4× guard band.

Measured in 04_rsp (8 seeds, 468 triangles with all attributes vs the float CPU path, s15.16
lsb): headers/culling/sort exact; edges ≤ 97, slopes ≤ 164 (< 1/400 px); shade base ≤ 317,
gradients ≤ 1265 (< 1/50 level); z ≤ 938 / 2284. Texture base ≤ 2.8 s10.5 units: the RSP
rounds s·n, t·n to integer s10.5 units per vertex (≤ 1/32 texel each) — accepted.
"Slivers" (any gradient ≥ 4096/px) are counted but not compared: both paths saturate there.

### Clipping

`tri_clip`: Sutherland–Hodgman in clip space (s15.16) against the planes the triangle crosses,
in the order near, far, guard −x, +x, −y, +y.

- Plane distance: exact 32-bit W ± C, or W ± C/4 for the guard planes (the 4W plane scaled by
  1/4: same crossing point, no overflow). Clip coordinates must keep |W| + |C| < 32768.
- A new vertex is always interpolated from the inside vertex towards the outside one, so an
  edge shared by two triangles is cut at the same point in both (no cracks). t = d_in /
  (d_in − d_out) by a restoring division (both halved when the difference reaches 2^31);
  clip x, y, z, w, color and s, t interpolated, then projected like `vtx_loop`.
- New vertices go to a pool of 12 cache entries (32..43, over INBUF, dead after VTX); pool
  exhausted → the triangle is dropped (a plane creates at most 2 new vertices, so 6 planes
  fit; the check guards degenerate input). The two polygon lists (vertex references, DMEM address / 8) live at 0x7E0 / 0x7F0.
- The polygon is drawn as a fan (v0, vi, vi+1) through the normal setup; the winding is kept,
  so culling still works. `k64_kgfx_read_vcache` can read the pool (`K64_KGFX_CLIP_POOL`).

Measured in 04_rsp stage C (16 seeds × 24 triangles crossing near/far/guard planes, each run
alone against a float Sutherland–Hodgman on the CPU with the same plane order; 217 cut, 91
rejected → 496 output triangles): new vertices clip ≤ 14 lsb, screen ≤ 1 s13.2 unit, z ≤ 4,
color ≤ 2, s/t ≤ 2; triangle headers and counts exact; X error at the end of an edge ≤ 2050
(1/32 px; guard-band edges are long); shade ≤ 36 / 50; z base ≤ 4269 (1/16 step), z error at
the far side of the polygon ≤ 74 steps — near the near plane z runs at ~2000 steps per pixel
over 600 px of guard band, where the 4e-5 of 1/NZ' shows.

### Cost

Measured on kestrel64 (04_rsp, 64 random triangles shade+tex+z, all on screen): RSP
~340 µs, CPU float path ~2100 µs. Demo 05 (15 cubes, 180 triangles submitted, ~80 drawn):
CPU time per frame 412 µs on the RSP path (matrices + command list) vs 2212 µs on the CPU path;
the RSP finishes ~670 µs after the list is submitted, with the RDP already rasterizing
(~800 µs since stage D: clip flags per vertex, reject/cut test and NZ normalization per
triangle; 05 has no clipped triangles).

kestrel64 runs the RSP at one instruction per cycle, so its timings above count instructions
only. On hardware the vector unit has 4-cycle result latency and a scalar + vector pair can
issue together, so instruction *order* matters as much as count. Static estimate
(`tools/rsp_cycles.py`, see below), fall-through path:

| Path | Instructions | Cycles before | Cycles after scheduling |
|------|--------------|---------------|-------------------------|
| one triangle, shade + tex + z | 305 | 380 | 299 (IPC 1.02) |
| two vertices (`vtx_loop`) | 73 | ~110 (incl. 2× `vtx_flags` call) | 80 |

### Scheduling and tools

The hot blocks (`tri_s3`, `tri_w2`, `vtx_loop`) are hand-scheduled for SU/VU dual issue:
scalar work that does not depend on the vector chain (header stores, FY/X1, W loads, flags,
DMEM stores) is interleaved between dependent vector ops, and delay slots carry useful work.
Rules that keep the reordering correct:

- every vector computational op writes ACC_LO (including `vmov`, `vrcp*`, `vadd`, `veq`,
  `vmrg`), so no vector op may sit inside a `vmud*`/`vmad*`/`vsar` chain;
- `vaddc`/`vsubc` → `vadd`/`vsub` pass VCO, `veq` → `vmrg` passes VCC, and
  `vrcph`/`vrcpl`/`vrcph` pass the divider state: keep each pair free of ops that change it;
- a partial-lane load (`lsv`) into a register goes after the last full write of it;
- stores in a delay slot that may be skipped must land past the current command end.

Tools: `k64rsp.py --listing` (built as `build/ucode/<name>.lst`: PC, word, source line);
`tools/prof_rsp.py rom lst` samples a running ROM per IMEM slot in kestrel64 (instruction
counts per label); `tools/rsp_cycles.py lst [--from L --to L] [--detail] [--joined]`
estimates stalls and dual issue per labelled segment. The pipeline model is a port of
RSPL's cost evaluator (`src/lib/optimizer/eval/evalCost.js`, Max Bebök, Apache-2.0,
https://github.com/HailToDodongo/rspl), itself based on
https://n64brew.dev/wiki/Reality_Signal_Processor/CPU_Pipeline. It does not model
accumulator hazards (the hardware forwards ACC) and treats every branch as taken.

kestrel64 runs the same pipeline model at run time with `KESTREL_RSP_TIMING=1` (kestrel64
4f6b5bd; forces the RSP interpreter, emulation results unchanged). Over MCP,
`profile_rsp(metric="cycles"|"stalls")` gives cycles, stalls and dual-issue pairs per IMEM
slot, and `rsp_registers().timing` gives totals and per-task cycles (by OSTask type), with
an estimated SP DMA wait (10 + 8 B/cycle per row). Measured on 05_kgfx_cube: `tri_s3` 108,
`vtx_loop` 78, `tri_w2` 78 cycles per pass (static model: 112 / 80 / 80, it counts every
branch as taken), kgfx IPC 0.98, ~44k cycles per task.

Comparing runs: per-task averages follow the camera, so compare a routine's cycles against
another routine with a fixed workload in the same run (e.g. `tri`), or per iteration. The
per-routine `insns` column counts issue slots (a dual-issued pair is one).

Early face culling (2026-10-02): the back/front test used to come after the y sort, the
edge reciprocals and the NZ normalization, so a culled triangle paid most of the setup. It
now runs first on the submitted vertices: back <=> cross(p2 - p1, p3 - p1) > 0, the high
half of one `vmudh`/`vmadh` pair (the sort's swap parity is gone). 10_trex (2400 tris, ~55%
back-facing): RSP 13.0 -> 11.0 ms per frame; 08 (inside a room, few back faces) unchanged;
identical output.

Batch culling (2026-10-02): in 08, 84% of TRI triangles were rejected at entry (all three
vertices outside one plane), and dispatching and rejecting them took ~20% of the RSP time.
CULL tests a hull once and skips the commands it covers: 08 culls each grid by its 4
corners (before its VTXs, so lighting and transform are skipped too) and each band by its
two vertex rows. RSP time per frame -18% on average (up to -49%), identical output; the
CPU writes ~45 µs more of commands.
The first CULL walked the mask bit by bit and itself took ~11.5% of the RSP time (~20 slots
per call). VTX now also writes each slot's flags to a packed array (CLIPFLAGS), and CULL
loads the 32 slots as 4 vectors, replaces the slots outside the mask with all ones (`vmrg`
with VCC = one mask byte per vector) and AND-reduces them: ~50 cycles per call whatever the
mask, +6 cycles per vertex pair in VTX. 08 RSP time a further -6%.

Software pipelining (2026-10-02): `light_loop` and `vtx_iter` each finish the previous
vertex pair (color * light and its stores; NDC, screen and all stores) inside the latency
gaps of the current pair's chain. `light_loop` runs pairs + 1 times (the extra pass reads
past the input); the vertex loop instead ends with `vtx_last` (stage B only, 25 cycles),
because VTX batches are short — in 08 the average is ~2.5 pairs, so a whole extra pass
(~71 cycles) cost more than the pipelining saved. Static model: light 56 → 39 and vertex
94 → 71 cycles per pair.

Measured scheduling pass (2026-10-02, `prof_rsp.py --timing`, 10_trex modelled cycles per
task, pixel-identical unless noted):

| Change | Cycles |
|--------|--------|
| start (after early culling) | 801k |
| asynchronous MTX/LIGHT/TEXGEN DMA, double-buffered OUTBUF (DMEM map below) | 761k |
| face cull product interleaved with the reject / clip-flag tests at `tri` entry | 736k |
| `light_loop` decodes the next pair's normals during the current pair (3-deep pipeline) | 722k |
| NZ bit length branch-free (two halving steps + 16-entry table `NZLUT`) | 717k |
| NZ' written straight to lane 7 of the reciprocal inputs (`mtc2`, no `vmov`) | 711k |
| horizontal-edge mask on the slope numerators, in the scalar NZ code | 701k |
| command dispatch: table read before the range check | 700k |
| y-sort and buffer-room branch delay slots filled (deterministic `--fields 240`: 704.2k → 700.5k; 06 −0.6%, 08 −0.3%) | 700.5k |
| command fetch, `TRI` second-triangle tail call, DX/DY >>k and triangle-count delay slots (06 −1.2%, 08 −0.9%) | 693.6k |
| triangle count in a register (`$v0`, stored at END); OUTSTART loaded in the cull product's latency (06 −1.7%) | 684.3k |
| Newton-Raphson on a precomputed −2x (built in the vrcp latency), W loads in its stalls — one truncation instead of two: edge pixels / ±1 RGB555 step differ, 04 error bounds unchanged | 683.2k |
| CULL: the AND tree stalls fetch and decode the next command, dispatched straight from the test (08 room 78.47k -> 78.14k) | 683.2k |
| vertex 1/w Newton-Raphson on a precomputed −w (no negation in the chain; 06 −0.6%, 08 77.6k): ±1 RGB555 / edge pixels differ, 04 bounds unchanged | 679.2k |
| `vtx_iter` rescheduled: clip compares before the reciprocal (flag decode pairs with it), 4W and the z clamp in own registers, no store two cycles after a load (77 -> 69 cycles per pair; 06 68.06k, 08 76.73k) | 672.6k |
| Clip flags packed as nibbles (both VCC words in one GPR, two masks + shifts: 22 -> 13 SU ops), both compares ahead of the reciprocal, the Newton-Raphson `+2` folded into the accumulator as `2 * x0` (bit-identical, no `vadd` latency) (69 -> 57 cycles per pair; 06 66.49k, 08 74.86k) | 658.3k |
| Triangle 1/NZ Newton-Raphson: `-2x` precomputed in `tri_s3`'s scalar-only stretch (lane 7 from the scalar NZ'), the `+2` folded into the accumulator, `vrcp` issued between the `mtc2`s (`tri_nz` 67 -> 60 cycles; 06 65.71k, 08 74.28k) | 651.5k |
| NZ read as one word (`ssv` x2 + `lw`), bit length only when `|NZ| >= 2^16` (out-of-line `tri_nz_k`), XL and the `-2x` vector ops interleaved into the stall slots (`tri_s3` 62 -> ~50 cycles; 06 63.89k, 08 73.30k) | 635.6k |
| Attribute setup: `vw = (minW - lsb) / 2` makes every `1/w * vw` < 0.5, i.e. directly 0.15 (no `>= 1.0` clamp, no shift; texel rounding moves on a few 08 pixels), and DX/DY as `A1*(-LY) + A2*HY - A3*MY` / `A1*LX + A3*MX - A2*HX` without the `A2 - A1`, `A3 - A1` subtractions (`tri_w2` 59 -> 51 cycles; 06 62.84k, 08 72.52k) | 626.6k |
| `light_loop`: the previous pair's `light * color` first, so the color store round trip (`suv` -> `lw` -> `sw`) runs a few cycles earlier; `tri`: p2/p1 decoded first so their loads overlap the p3 decode (06 62.40k, 08 71.99k) | 622.0k |
| XH/XM: X1 broadcast into the edge registers (`vor` in `tri_s3`'s scalar stretch) and accumulated first (`vmudn`/`vmadh` x 1) in the slope `vsar` latency, replacing the `vaddc`/`vadd` after the chain (`tri_nz` 60 -> 56; 06 62.00k, 08 71.70k) | 618.6k |
| Attribute blocks stored without branches: block addresses from `TRIOFS` (per-mode offsets set by GEOMMODE, a disabled block goes to scratch past the enabled ones inside `TRI_MAX`), the 24 stores paired with the `tri_nok` chains (`tri_nok` + emit 63 -> 35 cycles; 06 60.07k, 08 69.57k) | 593.8k |
| Face cull right after the reject / clip tests in `tri` (a culled triangle exits at cycle ~35 instead of ~47); the OUTBUF room test follows it, `tri_check` stays out of line for the clip fan and the flush (06 58.50k, 08 69.69k) | 586.0k |
| `tri_nz`: the k = 0 path skips the NZ shift and the numerator scale (moved to `tri_nz_k`); the NZ reciprocal inputs are written first so `vrcp` starts 4 cycles after them (06 58.11k, 08 69.53k) | 582.6k |
| `tri_s3`: the NZ product ahead of the edge reciprocals (`vneg` from the loaded vertices, not from the edge vectors), so the scalar degenerate / bit-length tests no longer wait on it (06 57.85k, 08 69.34k) | 580.3k |
| `MTX`, `LIGHT`, `TEXGEN` start their DMA inline and jump back to `cmd_next` from the last `mtc0` (no `jal dma_start` / `j cmd_next` / `nop`; envmap 172.6k) | 574.5k |
| Baker: chunk order chains the virtual-bone group across chunks (347 -> 310 `MTX`+`LIGHT` per frame, MODELS.md) | 571.6k |
| `light_loop` stores each pair's colors one iteration later, so the `suv` -> `lw` -> `sw` round trip leaves the vector chain: 23 -> 19 cycles per pair (06 57.45k, 08 68.62k) | 566.3k |
| `VTX` starts its DMA inline and does its setup (stats, output pointers, light mode) while it is in flight (06 57.11k, 08 67.20k, envmap 170.9k) | 556.9k |
| `MTX` flag `K64_KGFX_MTX_LIGHTS`: the light block follows the matrix in RDRAM and one command loads both (model path: 2265 -> 1955 commands) | 552.6k |
| (s/w, t/w with 16 fraction bits: 545.1k.) Vertex references are DMEM address / 8 (`0x20 + slot * 5`; the decode loses its three `addiu VCACHE`, model blob `K6M2`); `tri` tests only the OR of the three clip flags (rejection needs a flag in it too; the AND runs out of line in `tri_rejclip`, which jumps back when only the screen edges are crossed); OUTBUF room against a precomputed `OUTLIMIT` word (`tri` 39 -> 31 instructions; 08 87.56k -> 87.15k, envmap 168.4k -> 166.0k) | 538.8k |

`texgen_loop` got the same pipelining as `light_loop` (07: 225.7k → 222.5k per task).
What is left in the triangle path is mostly dependency latency with no independent vector
work to fill it: the reciprocal Newton-Raphson chain, the 1/w → S/T chain in `tri_w2`, the
cull product (3 cycles; two triangles of a `TRI` could share one product in two lanes).
Merging the exits of `tri` (reject, clip, cull, OUTBUF full) into one branch was tried: the
scalar chain is the same length, and culled triangles (half of T-Rex) exit later; T-Rex
552.6k -> 575.6k with all four merged, no gain with only the rare three merged. Dropped.

**QUAD tried and dropped** (2026-10-02). A `QUAD` command (a, b, c, d → (a, b, c), (a, c, d))
whose second triangle reused the attribute DX/DY of the first (both scaled with the smallest W
of the four vertices; shared trivial reject; second triangle skipped when the first is
back-facing) saves ~10 cycles per reused triangle out of ~250 — DX/DY is only 8 vector ops plus
the /NZ scale, and the reuse path still costs 4 `lqv` + putting DY back in the accumulator. The
`TRI` path pays for the mode test and the fourth-W min (~0.7 cycles per triangle). Deterministic
240-field windows (`prof_rsp.py --fields 240`), cycles per task, HEAD vs QUAD ucode:
08 room (quads used) 80.0k → 79.3k (−0.8%), 06 lit 71.8k → 72.3k (+0.8%), 10 T-Rex 704.2k →
706.5k (+0.3%). A net loss for models (all `TRI`), so not merged; the source is kept outside
the tree. Gradient reuse also rounds ±1 lsb differently from a fresh setup.

**Edge 1/dy reuse not done** (2026-10-03, by inspection of the scheduled code). The three edge
reciprocals are 6 `vrcp`/`vrcph` ops in `tri_s3`, each dual-issued with a scalar op of the
header/NZ work, and the Newton-Raphson refinement runs once for all lanes. Skipping them for a
shared edge leaves the scalar ops alone in the same cycles: ~0 saved, while finding the shared
edge after the Y sort (and keeping its slope across commands) costs scalar compares and a branch.

**Second-triangle front end not overlapped** (2026-10-03, by inspection + T-Rex profile). The
cull chain (`llv` → `vsub` → `vmudh`/`vmadh` → `mfc2` → sign) stalls 4 cycles per triangle
(1 at `vmudh`, 2 at `mfc2`, 1 at `sra`), ~1.8% of T-Rex. Hiding it means computing it for the
second triangle of a `TRI` while the first one runs, and neither place works:
- in `tri_w2` / `tri_nok` the scalar slots are free but no vector register is: v2/v3 hold the
  viewport and v16..v23 the matrix across commands (`$at` = 0), the rest is live in the
  attribute chains. The three `llv` would land in the `tri_nok` store tail, which is
  scalar-bound, and only triangles whose first one was drawn would gain;
- in the first triangle's prologue (both products in one vector, lanes 0 and 2 through the
  `[1q]` element) the second triangle's decode and loads cost ~10 scalar ops of which ~4 fill
  stalls: +6 cycles there for ~6 saved later, and single-triangle `TRI`s only pay.
Writing the product as cross(p1, p2) + p3 terms to skip the `vsub` of p3 adds two products to
the chain for the same final cycle.

### Lighting

GEOMMODE `K64_KGFX_LIGHTING` makes VTX run `light_verts` over the input buffer before the
transform: each vertex color becomes

    rgb = color × min(ambient + Σ max(N · L_i, 0) × light_i, 2.0),  alpha unchanged

with N the decoded vertex normal and L_i the light directions, both in model space. The CPU
side (`k64_kgfx_lights_set(out, lights, n, amb_r, amb_g, amb_b, &model)`) moves the world-space
directions into model space with the transpose of the model matrix's 3×3 (exact for rotations;
no non-uniform scale) and normalizes them, so the RSP needs no normal matrix. Call it per
object and send the block with `k64_kgfx_lights` before the VTXs; unused lights are zero.

Fixed point (`k64_kgfx_lights_t`, 160 B, 16-byte aligned):

- `dir[3][8]`: x, y, z of lights 0..3 as columns (lights in lanes 0..3, again in 4..7), Q14,
  pre-multiplied by 32/31, 64/63, 32/31 to undo the normal decode scale below.
- `col[4][8]`: light colors r, g, b, 0 twice, Q15 (255 → 0x7FFF). `amb[8]`: r, g, b, 1.0
  twice, Q14 (the 1.0 in lane 3 is what keeps alpha).
- `nshift`, `nmask`: decode constants (1, 32, 2048 / 0xF800, 0xFC00, 0xF800).

Per vertex pair (A in lanes 0..3, B in 4..7): the packed normal is broadcast, shifted per lane
(`vmudn` by 1, 32, 2048) and masked into Q15 x, y, z (x, z = n·31/32, y = n·63/64); N · L for
the 4 lights = 3 `vmulf`/`vmacf` against the direction columns, clamped at 0 (`vge`); light =
ambient (`vmudh` × 1) + 4 `vmacf` of the colors by the dot lanes; the colors are loaded with
`luv` (A rgba in lanes 0..3, B in 4..7, merged with `vmrg`), multiplied (`vmulf`), doubled with
saturation (light range 0..2) and stored back with `suv`. ~30 instructions per pair.

Measured in 04_rsp (8 seeds, 2 or 4 lights, random ambient, rotation, normals and colors,
1024 channels): bit exact against a CPU model of the fixed-point math, and within 1 level of a
float evaluation with the decoded normal. Demo 06 (5 objects, 272 lit vertices): RSP +~65 µs
per frame with lighting (~0.24 µs per vertex on kestrel64), CPU +~15 µs per object for
`k64_kgfx_lights_set` (float).

### Texture coordinate generation

GEOMMODE `K64_KGFX_TEXGEN` makes VTX run `texgen_verts` over the input buffer (after
lighting, before the transform): s, t are replaced by a spherical environment map lookup,

    s = tex_w × (1 + N · X) / 2,  t = tex_h × (1 − N · Y) / 2   (s10.5)

with X, Y the view-space axes. `k64_kgfx_texgen_set(out, &modelview, tex_w, tex_h)` stores
them in model space (rows of the modelview's 3×3, normalized; Y negated since t grows down),
so the same packed normal as for lighting is used and no normal matrix is needed. Send the
block with `k64_kgfx_texgen` per object. Texture sizes up to 512.

Fixed point (`k64_kgfx_texgen_t`, 112 B): `dir[3][8]` the two axes as columns in lanes 0, 1
(4, 5 for vertex B), Q14 with the normal decode undone; `scale` = size × 32, `off` = size × 16
in lanes 0, 1; the normal decode constants. Per pair: decode as in lighting, 3 MACs for the
two dots, then off (`vmudh` × 1) + scale × dot (`vmacf`), stored with `slv` into the input
vertices' s, t. 14 instructions per pair.

Measured in 04_rsp (8 seeds, random rotations, sizes 16..128, 512 coordinates): bit exact
against a CPU model, within 1 s10.5 unit of a float evaluation. Demo 07 (chrome torus and two
spheres, 32×32 RGBA16 sphere map drawn by the CPU, combined TEX × SHADE with lighting).

### Fog

`k64_kgfx_fog(l, start, end)`: VTX replaces each vertex alpha with

    a = clamp((w − start) × K, 0, 0x7FFF) >> 7,   K = 0x7FFF / (end − start)  (16.16: Ki, Kf)

with w the integer part of clip w, i.e. the eye distance in the projection's units (0 at
`start`, 255 at `end`; 0 <= start < end <= 32767). The blender mixes FOG_COLOR in by shade
alpha: `K64_OM_2C_FOG_SHADE` + a 2-cycle combine whose cycle 1 is COMB (`K64_CC_2C_TEX_SHADE`,
`K64_CC_2C_SHADE`, rdp.h); set FOG_COLOR (`k64_rdp_set_fog_color`) to the clear colour.
`k64_kgfx_fog_max(l, start, end, max)` scales the ramp to 0..`max` (1..255): the final
multiply is by FOGMUL = 2 × max + 2 instead of 0x200, so 0x7FFF gives exactly `max` (and
255 stays `>> 7`). A game whose fog never saturates (levels whose fog stops near 77)
uses it to keep the far fog partial.

FOGDATA (DMEM 0xF28, 8 B): s16 start, s16 Ki, u16 Kf, u16 mask. FOG writes it with mask
0xFF00; `k64_kgfx_run` writes the off state (0, 0, 0, 0xFFFF) before every list, so lists
start with fog off (FOGMUL is left as is: with K = 0 it multiplies 0). The loop always computes `(b << 8 | a) & mask | factor`: with fog off the
mask keeps the alpha and the factor is 0, so there is no branch and no second loop (IMEM has
40 bytes left).

In `vtx_iter` (stage A/B pipeline): `vsub` (w − start) right after the transform, `vmudm` /
`vmadh` by K, `vge` 0, `vmudl` by FOGMUL (`lsv` into $v31 element 0 before the loop), `vand` with the vertex b, a, `vor`; the
result (lanes 3, 7) is stored one pair later with `ssv` to cache offset 0x22, B before A like
the other stores. Each op is placed next to an unpaired scalar where one exists. Cost
(T-Rex, `prof_rsp.py --timing --fields 20`): `vtx_iter` 1526k -> 1608k cycles (55.7 -> 58.7
per pair), whole task +5.6k cycles (+1%); measured RSP time 10783 -> 11010 us. The cost is
the same with fog off.

Demo 12 uses it (start 6000, end 12000, B toggles). kestrel64 SoftRDP does not run the
cycle-0 blender without FORCE_BLEND (hardware does; reported), so check fog with prdp
(`tools/run_kestrel.sh ... prdp`).

After a list with triangles, `k64_kgfx_wait` marks the CPU stream's pipe, tile and load syncs
pending: the RSP's triangles may still be sampling TMEM when the CPU loads the next texture.

## DMEM map

| Range | Content |
|-------|---------|
| 0x000 | vector constants (0, 1, 2, 0x7FFF, 4) |
| 0x010 | VIEWPORT (32 B) |
| 0x030 | CMDPTR, STATUS, VTXCOUNT |
| 0x040 | MTX (64 B) |
| 0x080 | command jump table (32 B) |
| 0x0A0 | RING state (8 words), GEOMMODE, CULLMODE, TRISIZE, LIGHTMODE, clip plane table (to 0x0E7) |
| 0x100 | vertex cache, 32 × 40 B; 0x4F8..0x5FF (slots 25..31) doubles as COLTAB, 264 B loaded by COL |
| 0x600 | input vertex buffer, 32 × 16 B; after VTX: clip pool, 12 × 40 B (0x600..0x7DF) |
| 0x7E0 | clip polygon lists, 2 × 16 B (vertex references: DMEM address / 8) |
| 0x800 | command buffer, 256 B |
| 0x900 | OUTBUF: RDP command staging, 2 × 696 B (not part of the DMEM image) |
| 0xE70 | FOGMUL (u16: fog factor scale 2 × max + 2, set by FOG) |
| 0xE74 | COLBASE: colour base, w0 of the last CALL (bits 23..0), zeroed by START |
| 0xE80 | LIGHTS (160 B, set by LIGHT) |
| 0xF20 | lighting scratch (8 B) |
| 0xF28 | FOGDATA (8 B: start, Ki, Kf, mask; set by FOG, reset by `k64_kgfx_run`) |
| 0xF30 | TEXGEN (112 B, set by TEXGEN) |
| 0xFA0 | CLIPFLAGS: u16 clip flags per cache slot (64 B, written by VTX, read by CULL) |
| 0xFE0 | RETADDR: return address of CALL |
| 0xFE4 | RING_PUB (ring position DPC_END has reached), OUTLIMIT (last triangle start in the half being filled) |
| 0xFEC | NZLUT: bit length of 0..15 (16 B, written by START) |
| 0xFF0 | free |
| 0xFFC | PARTCOUNT: rectangles emitted by PARTICLES (zeroed by START) |

DMEM image: 256 B (the whole 0x000–0x0FF header). IMEM: 4088 of 4096 B used (resident + largest overlay slot).

### Vertex cache size (2026-10-04)

32 slots is the most this layout holds, and a bigger cache does not pay for the DMEM it
would take. Baker chunks of the 2400-triangle T-Rex (1925 distinct vertices), vertices
transformed per draw:

| Slots | Chunks | Vertices loaded |
|-------|--------|-----------------|
| 32 | 80 | 2537 |
| 38 | 66 | 2486 |
| 44 | 55 | 2386 |
| 64 | 35 | 2211 |
| 70 (tiny3d) | 32 | 2166 |

64 slots would save ~326 vertex transforms and ~45 `VTX` prologues, ~18k of the 539k RSP
cycles (~3%). What stops it:

- `TRI` vertex references are 8 bits (DMEM address / 8), so every slot and the clip pool
  must sit below 0x800. That range is full: header 256 B, cache 1280 B, input buffer / clip
  pool 512 B. Moving the header out gives 38 slots (~0.7%).
- Wider references: 3 × 9 bits do not fit next to the 8-bit opcode, and an added base costs
  ~3 scalar ops per triangle on the scalar-bound setup, which eats most of the gain.
- DMEM: 32 more 40-byte entries are 1280 B. The only block that large is OUTBUF (2 × 704 B),
  and halving it doubles the flushes, which costs more than the transforms it saves.

tiny3d reaches 70 with a different split: 36-byte entries (2520 B) and 16-bit triangle
indices, with little DMEM kept for RDP output (rdpq stages it).
Meshes from the baker load each vertex ~1.3 times; hand-made lists should be ordered the same
way (≤ 32 vertices per `VTX`, then their triangles).

### Asynchronous DMA (2026-10-02)

`MTX`, `LIGHT` and `TEXGEN` only start their DMA (inline, no wait). Their data is first read by
the next `VTX`, whose own synchronous read waits for them too, since the DMA queue completes
in order; `$at` tells `VTX` that the matrix registers must be reloaded. The OUTBUF flush is
double-buffered as described above. With `KESTREL_RSP_TIMING=1` (the measure that counts
here: kestrel64's own task time is one cycle per instruction and DMA is nearly free, so it
reads slightly *worse*): T-Rex 801k → 761k cycles per task (−5%), DMA stall 55k → 12k.
Publishing the pending flush right after each `VTX` read was tried and is slower (776k).

### Overlays (2026-10-02)

IMEM is a kernel plus one overlay slot (`OVL_SLOT`, ~1 KB). The slot's default occupant
is the VTX code (transform, lighting, texgen): it is part of the IMEM image, so a fresh load
needs no overlay DMA. A triangle that crosses the near/far plane or the guard band jumps
through `tri_clip_ovl`, which loads the clipper (768 B, `.overlay clip` in kgfx.S, embedded
as `kgfx_ovl`) over it; the next `VTX` loads VTX back. `ovl_load` compares the requested
overlay with `OVL_RESIDENT` (DMEM) and skips the DMA when equal, and `OVL_TABLE` gives each
overlay's RDRAM source and DMA length in one word, `(len - 1) >> 3 << 24 | address`
(written by `k64_kgfx_run`; packing the length freed the DMEM image byte budget the
PARTICLES overlay needed). Overlays: VTX, CLIP, AUDIO, ADPCM, PART.
`ovl_load` also patches the `VTX` entry of `cmd_table`: `cmd_vtx` while VTX is resident,
`cmd_vtx_ovl` (load, then jump into the slot) otherwise, so a `VTX` pays no residency check
(the AUDIO overlay of [AUDIO.md](AUDIO.md) uses the same slot; demo 11 mixes both in one list).

The `VTX` passes come from one word, `LIGHTMODE` = first pass << 16 | return address,
precomputed by `GEOMMODE` (none / light / texgen / light then texgen), so the setup after the
DMA wait is one `jr`.

Cost (before the table patch): one load + compare per `VTX` when nothing clips (T-Rex, 420 `VTX`: +27 µs on 11.5 ms);
08_kgfx_room, which clips in most frames: +9 µs on ~1 ms. The image is unchanged (all demos
pixel-identical). The 688 freed bytes go to the triangle path (ROADMAP item 1).

## Indexed vertex colours

For cached lists whose geometry is static but whose vertex colours change every frame (e.g.
rooms: the colour array is rebuilt per frame, the vertices are not). The sublist is recorded
once with `k64_kgfx_vtx_colidx`; the caller passes the frame's colour array as the CALL
colour base (`k64_kgfx_call_col`). Inside, `k64_kgfx_col(l, offset)` DMAs 264 B from
base + offset into COLTAB. DMA ignores address bits 2..0, so each vertex `a` byte holds
`((base + offset) & 7) + 4 * index` (index < 64, byte <= 252): the misalignment of the base
is baked into the vertex, and a sublist is valid for one value of `base & 7`. After the
vertex DMA, VTX with COLIDX replaces each rgba with `COLTAB[a]` before lighting / transform,
so only cache slots below 25 may be written while a table is in use (asserted). Cost: one
264-byte DMA per COL, 6 cycles per vertex.
