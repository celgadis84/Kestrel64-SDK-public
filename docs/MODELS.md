# Models, animation, billboards — and a tiny3d compatibility layer

Plan for getting 3D content into kestrel64-sdk. Everything heavy happens on the host
(cross-platform Python baker); the N64 side only DMAs blobs and patches offsets.

## 1. Static models: bake, don't parse

Host: `tools/k64bake.py` reads glTF/GLB and writes a `.k64m` blob.

| Part | On cart | Why |
|------|---------|-----|
| Header + offset table | relocatable, patched once after the PI DMA | no parsing on the CPU |
| Chunks | ≤ `K64_KGFX_VCACHE` (32) vertices + the triangles that use only them | one `VTX` + n `TRI` per chunk, the vertex cache never thrashes |
| Vertex data | s16 position (mesh quantized to its bounding box), packed normal, RGBA8, s10.5 ST | 16–32 B/vertex, DMA-ready, 8-byte aligned |
| Command lists | prebaked kgfx commands per chunk, in RDRAM | the RSP DMAs them straight; the CPU only patches the matrix/material header |
| Bounding sphere per chunk + per object | CPU frustum/near-plane test | skip invisible chunks; flag chunks that need the clip overlay ([RSP-RESIDENCY](RSP-RESIDENCY.md)) |
| Materials | sorted: texture → combiner → render mode | fewest TMEM loads and mode changes |
| Textures | TMEM-ready formats (CI4/CI8/IA/RGBA16), pre-swizzled, mip chain | load with one `LOAD_BLOCK`, no CPU conversion |
| LODs | 2–3 per object | distance-based choice on the CPU |

The baker also does the vertex-cache ordering (greedy chunking by shared vertices), so the
RSP reads each vertex from RDRAM once per chunk.

## 2. Animation

### Implemented: skinned models (`k64bake.py model`, demo 10_trex)

`k64bake.py model in.glb out.k64m --tris N [--tex 64x64:ci4] [--fps 30] [--preview p]`
(code in `tools/k64model.py`; CMake: `k64_add_rom(... MODELS name=file.glb:"options")`,
embedded as `u8 name[]`). Runtime: `include/k64/model.h`, `src/gfx/model.c`.

1. **Load**: every mode-4 primitive of the glTF is merged; the largest texture goes into the
   top of one atlas, other textures and flat base colors get 8x8 cells in the bottom strip.
2. **Decimate**: quadric edge collapse (half-edge, onto existing vertices) with border planes,
   link condition, normal-flip check, and a **wedge rule** for UV seams: a collapse is legal
   only if every wedge (pos, uv) of the removed vertex maps onto a wedge of the kept one.
   Collapses across virtual-bone groups wait for the floor (below). T-Rex: 10902 → 2400 tris
   in ~12 s.
3. **Virtual bones**: each vertex keeps its two heaviest joints, weight in quarters
   (q/4 of joint a). Each distinct (a, b, q) is one matrix: smooth joints for the price of
   rigid skinning on the RSP. T-Rex: 118 virtual bones over 41 joints.
4. **Chunk**: greedy meshlets of ≤ 32 vertices (vertex cache), scored to add few new
   vertices and few new virtual-bone groups.
5. **Commands**: per chunk, per virtual-bone group: `MTX` with `K64_KGFX_MTX_LIGHTS` (matrix +
   light block in one command, `k64_kgfx_mtxlights_t`; when the group changes), `VTX`, then `TRI`/`TRI2`; the list ends with `RET` and is `CALL`ed. Chunks are ordered
   greedily so each one starts with the group the previous one ended with, and ends with the
   group most remaining chunks contain (T-Rex: 347 -> 310 switches; changing the chunk score
   weights moved nothing, mesh connectivity decides the chunks). The low words hold
   indices/offsets that `k64_model_load` patches into addresses once.
6. **Animation**: sampled at a fixed rate, per joint quaternion (Q15, hemisphere-continuous)
   + translation (s16 × power-of-two scale). Scale channels are dropped (T-Rex: ≤ 0.0002).
7. **Precision**: model space is normalized to ±4096 over every frame (the constant
   non-joint root chain is folded into the root matrix), so the s15.16 MVP keeps its range.
   Floats are stored without denormals: the VR4300 traps on denormal operands even with
   FCSR.FS set.

Per frame: `k64_model_pose` (nlerp, hierarchy) → `k64_model_prepare` (world × skin, blend per
virtual bone, MVP to fixed point, lights into bone space) → `k64_model_draw`.

Blob `K6M2` (big-endian, sections 16-aligned): header (84 B) · vertices (16 B, kgfx layout) ·
commands · parent (s8) · inverse bind (float 4x4) · root · virtual bones (a, b, q, 0) ·
animation [frame][joint] (qx qy qz qw tx ty tz 0, s16) · texture (`.k64t`). T-Rex: 109 KB.

Linear blend skinning is linear in the matrices, so `prepare` takes each joint to clip space
once (P = viewproj · world · skin) and a virtual bone only blends two of them (16 + 9
multiply-adds for the MVP and the light rotation) instead of a matrix product.

Measured (kestrel64, 320x240, 2400 tris, ~1050 drawn after back-face cull, 1-cycle
bilinear CI4 + Z): pose 0.24 ms + prepare 2.27 ms on the CPU (was 3.35 ms with a product
per virtual bone and a light block built twice), kgfx ≈ 11.5 ms (13 ms before the early
face cull). Matrix/light switches cost ~3.5% of the RSP time and each `VTX` ~120 cycles of fixed
overhead (~9%, was ~135) (`tools/prof_rsp.py --timing`); triangle setup and the vertex/light loops
dominate. CPU split: `tools/prof_cpu.py`.

Double buffering (2026-10-02): `k64_model_load` keeps two sets of matrices and light blocks
and a second copy of the command list patched for the second set (T-Rex: +~25 KB of heap);
each `prepare` fills the set the previous `draw` did not use. Demo 10 poses and prepares
frame N+1 right after `k64_kgfx_run` of frame N, so the 2.5 ms run under the RSP: the busy
part of the frame (from the start of the frame to the RDP going idle) drops from 14.6 to
12.2 ms; both stay locked at 60 fps on kestrel64. The RSP side costs nothing (no segment
lookup per command). With the RSP changes of the same day (asynchronous MTX/LIGHT DMA,
double-buffered OUTBUF, earlier face cull, pipelined light loop) the modelled RSP time per
task (`--timing`) went 801k -> 722k cycles; the later scheduling work (ROADMAP item 2)
brought it to 531k.

### Implemented: static models (same baker, demo 12_static)

A glTF without `skins` takes the static path of the same `k64bake.py model` command, and the
runtime is unchanged: the baker emits one identity joint, one virtual bone (0, 0, 4), one
frame and a single `MTX` at the head of the list (`k64_model_pose(&m, 0)` once, then
`prepare`/`draw` per frame).

- **Node transforms**: every mesh is baked in world space (the product of `node_local` up the
  parent chain). Positions go through the matrix and normals through its inverse transpose.
  A mirrored node (det < 0) flips the triangle winding so back-face culling still holds. A
  mesh used by several nodes is baked once per node.
- **Missing normals**: computed by the baker (area-weighted face normals summed per shared vertex index).
- **Atlas**: one texture cannot repeat inside the atlas. UVs outside 0..1 are wrapped per
  vertex (1.0 stays the far edge), which breaks any triangle that spans a repeat; the baker
  prints how many triangles do (`k64model: warning: N triangles cross a texture repeat`). Fix
  them in the source by splitting the faces or keeping UVs in 0..1.
- **CI palette** (`ci4`/`ci8`): the flat-colour cells get exact palette entries first, and the
  textures are quantized (median cut) into the remaining entries over their own pixels only.
  With one global median cut, a flat colour that covers a large area of the atlas but few
  distinct texels (e.g. the grass cell in demo 12) was merged into the stone colours. When
  the whole atlas fits the palette, `tex_blob` builds an exact palette instead.
- **Chunker fill**: unwelded meshes (flat faces, hard edges) leave the greedy meshlet with no
  adjacent candidate after ~3 triangles. When that happens and the vertex cache has room,
  the chunker takes the remaining triangle with the fewest new vertices (ties: nearest
  centroid). T-Rex: 129 -> 80 chunks, 420 -> 374 `VTX` (RSP ~-31 us); demo 12: 656 tris in 25
  `VTX`.

Demo 12 (`demos/12_static`, `make_scene.py` writes `shrine.glb` deterministically): node
hierarchy with rotation and non-uniform scale, a mirrored ramp, a textured material and
three flat ones, a torus without normals. 656 tris, RSP ~3.1 ms, CPU 33 us (prepare 25).

Depth range: with near 300 / far 30000 the 16-bit Z cannot separate the ground disc from the
plinth top (0.5 units apart) at the far side of the orbit, and the disc shows through. The
demo uses near 1500 / far 20000. Keep far/near small (<= ~16) for close-together surfaces.

Fog: demo 12 fades eye distance 6000..12000 into the clear colour (kgfx FOG, 2-cycle
blender; B toggles). Trilinear filtering is not offered for baked models: it needs a mip
chain in TMEM next to the level 0, and a CI atlas already shares TMEM with its TLUT (the
palette takes the upper half), so the atlas would have to shrink.

### Implemented: levels of detail (`--lods`, demo 10)

`k64bake.py model --tris 2400 --lods 1600,1100,600` adds LODs decimated from the full mesh (the
same quadric decimator as `--tris`). They share the skeleton, the virtual bones (the union of
every LOD's), the position quantization and the atlas; each has its own vertex buffer and
command list, listed in the header's LOD table (`k64_model_lod_t`, LOD 0 = the main list,
`nlods` <= `K64_MODEL_LODS`). One `k64_model_prepare` serves all of them, so switching LOD
per frame is free: `k64_model_draw_lod(&l, &m, lod)`.

The decimator never collapses across a texture seam or a border. Collapses that join
vertices of different virtual bones are held back: once nothing else is legal (the T-Rex
reaches that at ~1070 triangles), every remaining edge is pushed again in one cost order,
where a cross-bone collapse also pays the removed vertex's skinning error: the squared
distance between it skinned by its own virtual bone and by the kept vertex's, worst of 16
frames sampled over the animation, times its triangle area (the quadric's units). The kept
vertex keeps its skin. Above that point the output is unchanged. When the heap runs dry the
edges are also pushed again once, since a collapse refused earlier (link condition, flip)
may have become legal. The T-Rex now goes down to ~370 triangles, where the texture seams
(teeth, claws, the UV islands) stop it; 600 still holds the silhouette, below ~450 the head
flattens. The baker warns when a LOD stays more than 10% above its target.

T-Rex (demo 10, same view, kestrel64 model): LOD 0 2400 tris (1096 drawn) rsp 11.0 ms; LOD 1
1600 (747 drawn) 7.6 ms; LOD 2 1100 (480 drawn) 5.5 ms; LOD 3 600 (228 drawn) 3.1 ms. The
demo picks by camera distance (14000 / 21000 / 25500; C-up/C-down move the camera, R forces
a LOD). The CPU prepare (2.2 ms) does
not shrink: it scales with joints and virtual bones, not triangles.

### Design notes (original plan)

| Kind | How | Cost |
|------|-----|------|
| **Skeletal** (T-Rex) | Rigid skinning: each vertex belongs to one bone; the baker groups vertices by bone, so a chunk uses **one matrix**. Joint vertices are duplicated across chunks (the seam is hidden by matching positions in the bind pose). | 1 `MTX` per chunk, no per-vertex blend on the RSP |
| Keyframes | Per bone: quaternion as smallest-three s16, translation s16, optional scale; uneven keyframe times (baker removes keys within tolerance) | ~6–10 B per key |
| Streaming | Long clips stay in ROM; a small ring in RDRAM refilled by PI DMA ahead of the playhead | constant RDRAM regardless of clip length |
| Playback | CPU: nlerp between keys, build bone matrices (s15.16), hierarchy multiply, write them to an RDRAM matrix palette; `osWritebackDCache` once | the RSP reads each matrix once per frame |
| Blending | Crossfade two clips per bone (nlerp) on the CPU | |
| Morph / vertex anim | Flipbook of chunk vertex data (small props, water) | pointer swap |
| Texture anim | Tile offset scroll (`SET_TILE_SIZE`) or flipbook of texture pointers | free on the RSP |

Later option (if the CPU becomes the bottleneck with many characters): move the
hierarchy multiply to an RSP overlay that reads the local transforms and writes the palette.

## 3. Billboards and particles

Yes, three kinds, each with its own cheapest path:

| Kind | Use | Path |
|------|-----|------|
| **Screen-aligned sprites** | particles, sparks, dust, smoke | `PARTICLES` command: the RSP transforms only the center point, scales the size by 1/w, emits one `TEXTURE_RECTANGLE` (or `FILL_RECTANGLE`) with primitive Z. No triangle setup. Same idea as tiny3d's tpx. |
| **Axis-aligned billboards** | trees, grass, lamps | quad rotated around Y toward the camera, built by the RSP from center + size, then the normal triangle path (perspective-correct, Z-buffered, fog) |
| **Impostors** | distant LOD of big objects | pre-rendered views on the host (N angles) → pick the closest view, draw as an axis-aligned billboard |

Particles are a cold IMEM overlay over the VTX slot, so they cost no IMEM while the mesh path
runs. **Done** (2026-10-04): PARTICLES command, Y billboards and impostors, see [PARTICLES.md](PARTICLES.md).

## 4. tiny3d compatibility layer

**Done** for `00_quad`, `01_model`, `03_objects` (unmodified sources, `compat/`). Results,
patches and method: [TINY3D-AB.md](TINY3D-AB.md). Both sides hold 60 fps at default load;
with 250 actors libdragon is RSP-bound at 50 ms/frame, the compat CPU-bound at 74 ms.
The plan below is the original design.

Goal: build tiny3d's own examples against kestrel64-sdk and compare the same scene,
same assets and same resolution: **tiny3d on libdragon** vs **kestrel64-sdk**.
tiny3d (Max Bebök, MIT) is the reference (a checkout next to the SDK).

API surface used by its 27 examples (measured):

| Group | Distinct functions | Mapping |
|-------|--------------------|---------|
| `t3d_*` (vertices, triangles, matrices, viewport, lights, fog, models, skeleton, anim) | 85 | `k64compat/t3d`: thin wrappers emitting kgfx commands; `rspq_block_t` recording maps to a prebaked kgfx command list |
| `tpx_*` (particles) | 11 | kgfx `PARTICLES` overlay |
| `rdpq_*` (modes, combiner, blender, fill/tex rect, tex upload, text) | ~43 | `k64compat/rdpq`: the combiner/other-mode macros are raw RDP encodings, so they port as-is onto our RDP stream |
| `display_*`, `surface_*`, `joypad_*`, `sprite_*`, `dfs_*`, `asset_*` | ~20 | small shims over the SDK's VI/framebuffer, controller, ROM filesystem |

Asset formats:

- `.t3dm` (format in `src/t3d/t3dmodel.h`) loaded directly by the compat loader. Its
  `T3DVertPacked` (two interleaved vertices in 32 B: s16 pos, 5-6-5 normal, RGBA8, s10.5 ST)
  is close to what kgfx needs; either kgfx accepts that layout as an input mode or the
  loader converts once at load time. Decide when kgfx gains normals (stage D).
- libdragon `.sprite` textures: header parse + TMEM upload.

Fairness rules for the comparison:

- Unmodified example sources where possible; any change goes in a patch file listed in the report.
- Same resolution, color depth, AA/dither modes and frame pacing.
- Report per scene: frame time, RSP busy, RDP busy (kestrel64 MCP; real HW numbers when available).
- Example assets have their own licenses: check each before shipping a ROM.

Candidate scenes, in order: `03_objects` (many draw calls), `17_culling`, `18_particles`,
`07_skeleton`/`08_animation`, `99_testscene` (the heavy one).

Prerequisites in kgfx before starting: lighting + near clipping (stage D), textured
triangles from the CPU API, matrix palette (`MTX` per chunk), particle overlay.

## 5. Order of work

1. Stage D: lighting, near clipping, guard band.
2. Baker v1: static GLB → `.k64m`, textured lit demo.
3. Skinning + animation runtime; T-Rex demo (dino_demo GLB, CC BY 4.0, attribution in the ROM and docs).
4. Billboards + particle overlay.
5. tiny3d compat layer and comparison report.
