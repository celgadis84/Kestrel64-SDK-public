# Roadmap

Work queue, top first. Done items move to the stage tables in the topic docs (KGFX.md, …).

## Now — SDK v1 completeness

Order (each item: API in `include/k64/`, a demo, checked in kestrel64):

1. **Saves**: done for EEPROM 4K/16K, SRAM 256K/768K/1M and FlashRAM 1M (`k64/save.h`,
   `k64/joybus.h`, demo 14_save; `k64_add_rom(... SAVE <type>)` writes the Advanced Homebrew
   ROM Header). FlashRAM follows libultra: chip ID picks the old (page * 64) or new array
   addressing, writes program erased bytes in place and rewrite the 16 KiB sector otherwise.
   Banked 768K SRAM: three 32 KiB chips at 0x08000000/0x08040000/0x08080000 (address bits
   18-19), one linear 96 KiB range in the API, transfers split at the bank edges; demo
   14_save_sram768 also writes across each 32 KiB edge and one marker per bank (PASS in
   kestrel64 5c5b83a, persists across runs).
2. **Controller accessories**: done (`k64/pak.h`, demo 15_pak): probe (Controller / Rumble /
   Transfer Pak), raw 32-byte transfers with address and data CRC, Controller Pak bytes,
   rumble motor. Notes (docs/PAK.md): the libultra file system layout (mount with repair of
   lost pages / damaged copies, format, create / delete / find, note read / write), multi-bank
   paks up to 4 banks (bank count detected at format; only 1-bank paks tested, kestrel64 has
   no bank select).
3. **Timers** (done): 64-bit time base extending COP0 Count (`k64_time`), deadline/periodic
   timers polled from the main loop (`k64_timer_*`, phase-locked, overruns skipped and
   counted), demo 17_timers with a self-test.
4. **Threads / scheduler** (done, docs/SCHEDULER.md): core stays single-threaded and
   polled; opt-in `k64/thread.h` with libultra semantics (priorities, preemption on
   interrupts/messages, message queues, RCP + Count/Compare events) for code written for libultra threads.
   Demo 18_threads self-tests it.
5. **Asset compression** (done, docs/PACK.md): `.k64z` = LZ4 block with an optimal-parse
   packer (`tools/k64pack.py`), `PACKED` keyword in the CMake helpers (models/textures baked
   first), `k64_unpack_rom` decodes in place behind the PI DMA (no scratch buffer; T-rex
   loads 2.5x faster than raw). Demo 19_pack self-tests it.
6. **Text / UI** (done, docs/FONTS.md): `.k64f` proportional fonts baked from TTF/OTF
   (`tools/k64font.py`, own GPOS/kern reader), I4 sheets = one TMEM load, kerning,
   UTF-8, color codes, shadow, word-wrapped boxes. `FONTS` CMake keyword. Demo 20_fonts.
7. **Particles / billboards** (done, docs/PARTICLES.md): kgfx `PARTICLES` overlay (point
   through the matrix -> one TEXRECT/FILL_RECT sized by 1/w, prim z, prim color, edge cuts),
   `k64_kgfx_billboard_y` for trees through the triangle path. Demo 21_particles self-tests
   both. Particle loop software pipelined (~72 RSP cycles per particle, was
   ~139). Impostors (`k64bake.py impostor` -> `.k64i`, `k64_impostor_draw`, demo
   23_impostor).
8. **Music sequencer** (done, docs/MUSIC.md): MIDI -> `.k64q` (`tools/k64music.py`, `MUSIC`
   keyword; notes carry durations, loop markers, tempo map), player stepped per 128-frame
   mixer block through `k64_mixer_build_ex` (events on 4 ms boundaries), instruments =
   VADPCM or s16 sample + root + ADSR on the volume ramps, drum map, bend, sustain pedal,
   voice stealing. Demo 22_music self-tests the note timing and RSP == CPU.

9. **High resolution** (done): `k64_display_init` takes 320/640 x 240/480; 480 lines are
   interlaced (VI serrate, V_SYNC one half-line shorter). The vblank interrupt loads the
   per-field VI_ORIGIN (field 0 one line down) and, on MPAL, V_BURST: a k0/k1-only ISR in
   crt0 without threads, a hook in the thread module's handler with it. Two 640x480x16
   buffers spill from the FB bank into the texture bank. Demo 24_hires self-tests both ISR
   paths. Field 1 is untested until kestrel64 models the field bit of VI_V_CURRENT
   (requested, PD64_pending.md P9).

Still missing from the SDK itself (top first):

10. **Reset** (done): `k64/reset.h` enables the pre-NMI
    interrupt; the crt0 ISR (or the thread module, which also posts `K64_EVENT_PRENMI`)
    records the press and masks IP4. `k64_reset_warm` / `k64_reset_buffer` give the boot
    type and the 64-byte NMI buffer. Demo 25_reset, verified in kestrel64 with its RESET
    button (Ctrl+R or MCP `reset.soft`): cold boot, then warm boots counting 1, 2 with the
    pre-NMI seen ~484 ms before the NMI.
11. **ROM file system** (done): `ROMFS dir [ROMFS_PACK glob ...]` in `k64_add_rom` packs a
    tree with `tools/k64fs.py` (FNV-1a hashed, sorted directory; files 16-aligned; glob
    matches stored as .k64z when that shrinks them). `k64/romfs.h`: open by path, read at
    any RAM/ROM alignment (straight PI DMA when aligned, 512-byte bounce otherwise), seek,
    whole-file load (decoded when packed), listing. Demo 26_romfs self-tests it.
12. **Joybus accessories** (partly done): `k64_joybus_identify` (controller / mouse / VRU /
    keyboard / EEPROM), cartridge RTC `k64/rtc.h` (date/time get/set with stop handshake,
    8-byte SRAM), mouse through `k64_pads[]` (x/y = relative motion). Demo 28_joybus,
    verified in kestrel64 with `KESTREL_RTC=1 KESTREL_PADTYPE=0200` (clock read/set/SRAM,
    mouse identified on port 2). **Blocked**: Randnet keyboard and VRU drivers — n64brew documents only
    their identifiers and command numbers, not the packet layouts; not reimplemented from
    other projects' code (provenance rule).
13. **Transfer Pak** (done): `k64/tpak.h` — pak power/access/status, raw GB bus, and a
    cartridge layer (header + checksum, MBC1/MBC2/MBC3+RTC/MBC5 banking) for linear ROM/RAM
    access and the MBC3 clock. Demo 27_tpak self-tests four synthetic carts (docs/PAK.md).
14. **Video playback** (done, docs/VIDEO.md): own K64V codec (YUV 4:2:0, 8x8
    IDCT defined by the RSP arithmetic, Exp-Golomb run/level, skip/inter/intra/motion), encoder
    `tools/k64vid.py`, `k64/video.h` with a CPU reference back end bit-exact with the
    encoder (demo 29_video). RSP back end done (microcode `kvid`): IDCT + prediction +
    YUV -> RGBA5551 straight to the framebuffer, overlapped with the CPU parse of the next
    macroblock row, bit-exact with the CPU reference. Whole-pixel motion vectors (RSP
    fetches the displaced block; -22% bits on the demo clip). Table-driven parse
    (8.7 -> 3.6 ms/frame in kestrel64; the RSP stage is now the longer one). VADPCM audio
    decoded by the mixer's RSP overlay (~0.11 ms/frame, 3.6x smaller than PCM). Open:
    half-pixel motion.

Collision/physics stay with the game.

## Near term — kgfx

1. **Cheaper triangles** (docs/RDP-MODES.md): triangle setup is ~75% of kgfx RSP cycles.
   Coplanar QUAD gradient reuse measured as a net loss, edge 1/dy reuse saves ~0 (the
   reciprocals are dual-issued with scalar work), both in KGFX.md.
   (Done: s/w, t/w carry 16 fraction bits; they were rounded to whole s10.5 units, off by
   tens of percent at the far vertex of a high W ratio triangle. The 08 "purple wall"
   streaks are the texture's own diagonal noise, magnified.)
2. **RSP cycle budget** (`tools/prof_rsp.py --timing`, `--fields N` for a deterministic
   window): done so far — asynchronous MTX/LIGHT/TEXGEN DMA, double-buffered OUTBUF flush,
   face cull overlapped with the reject tests, pipelined `light_loop` and `texgen_loop`,
   branch-free NZ bit length, early horizontal-edge mask, dispatch without load-use stall
   VTX prologue (overlay check folded into the dispatch table, precomputed pass jump,
   loop constants set during the DMA wait; ~135 -> ~120 cycles per `VTX`), triangle setup
   scheduled around the store-after-load stall, branch-free smallest W, face cull right
   after the NZ sign (one branch for reject + clip), vertex references as DMEM addresses
   with an OR-only flag test (the AND only out of line)
   (T-Rex 801k -> 538.8k cycles, envmap 225.7k -> 166.0k, both with the 16 fraction bit
   s/w, t/w). Left: the `tri_nok` store tail is scalar-bound (packing the Z block into one
   vector breaks the DY accumulator chain, ~0.5%); overlapping or sharing the cull product
   across the two triangles of a `TRI` does not pay (KGFX.md). The `VTX` loop runs at IPC
   1.74, scalar-bound by the clip flag packing; the rest of its fixed cost is mostly the input DMA wait.
3. **Audio** (docs/AUDIO.md): mixer overlay done (RSP-RESIDENCY step 4), VADPCM decode
   overlay + encoder + CMake SOUNDS done, volume ramps done. RSP-RESIDENCY step 2 (region
   tags) waits for a second microcode.
4. **Model path cost** (docs/MODELS.md, demo 10_trex): fewer `VTX` (chunk group order
   done: 347 -> 310 switches; chunker fill: 420 -> 374 `VTX`), static-mesh variant of the baker
   (done: demo 12_static), 2-cycle presets (done: kgfx FOG + `K64_OM_2C_FOG_SHADE`,
   `K64_CC_2C_*`, demo 12), LODs (done: `--lods`, `k64_model_draw_lod`, demo 10 with
   four; the decimator's floor went from ~1070 to ~370 T-Rex triangles with cross-bone
   collapses after the floor, the texture seams are what stops it now).
   CPU prepare under the RSP (double-buffered model buffers) is done.
5. **tiny3d adaptation layer** for A/B comparisons (tiny3d on libdragon vs the same scene here).
   Done for 00_quad, 01_model, 03_objects (docs/TINY3D-AB.md): 60 fps on both sides; under
   stress the compat is CPU-bound (rspq interpreted on the CPU) and its RSP part ~18% slower
   per triangle. **Paused (2026-10-04)** until the SDK is complete: block memoization
   (rspq block runs replayed as kgfx sub-lists) is parked on branch `wip/compat-memo`
   (03_objects matches, 01_model breaks after a partial recording).

## Medium term

### Canvas renderers (docs/RDP-MODES.md)

Low-res canvas computed on the RSP, upscaled by the RDP (1-cycle bilinear strips, or COPY of
CI8 + TLUT). Demos: Comanche-style voxel terrain, then the SDF ray marcher below. Run
09_rdp_bench on hardware to calibrate FILL/COPY and per-primitive costs.

### SDF microcode + demo

Signed distance fields on the RSP. Budget: 62.5 MHz, 8 lanes. Full-screen per-pixel ray
marching is out of reach (320×240 × ~32 steps × ~20 ops / 8 lanes ≈ 6 M cycles/frame), but:

- **3D ray marching at low resolution**: 80×60 (4800 px → 600 lane groups × 32 steps × ~20
  ops ≈ 0.4 M cycles ≈ 6 ms) or 160×120 at ~4× that; the RSP writes an RGBA16/I8 texture,
  the RDP upscales it with bilinear texture rectangles (and can composite polygons over it
  with z). Scene SDFs as a small instruction list (sphere, box, torus, smooth union) the
  ucode interprets per lane group; normals by central differences or analytic per primitive.
- **2D SDFs**: SDF glyphs/shapes rendered by the RDP itself (I8 distance texture + alpha
  compare / combiner threshold) — crisp scalable text and UI at no RSP cost; the ucode only
  needed for generating/animating fields.
- Demo: animated raymarched blob scene (smooth union of spheres) at 80×60 → 320×240, plus
  SDF text overlay. Measure fps on kestrel64 with RSP timing.

### SDL2 adaptation layer (`k64sdl`) for quick PC ports

A subset of SDL2 over the SDK so 2D / software-rendered PC games (OpenTyrian, NXEngine,
Wolf3D, Doom-likes) port with little code change. Noted 2026-10-05. Mapping:

- `SDL_Surface` / `SDL_UpdateWindowSurface` → framebuffer + VI (CPU-drawn, DMA'd to the
  scanned buffer).
- `SDL_Renderer` (textures, `RenderCopy(Ex)`, alpha, colour mod) → RDP texrect / triangles;
  textures over 4 KiB of TMEM drawn in tiles.
- Input (`SDL_GameController`, keyboard events) → pad, keys mapped to buttons.
- Audio (`SDL_AudioSpec` callback) → SDK audio thread feeding the mixer / AI.
- `SDL_GetTicks`, `SDL_Delay`, timers → SDK timers; `SDL_Thread`, mutex, cond → `k64/thread.h`.
- `SDL_RWops` + a stdio shim (`fopen`/`fread`) → read-only file system in ROM.

Order: surface, input, audio, time, threads, RWops first; `SDL_Renderer` on the RDP next.
SDL + OpenGL (most 3D PC games) needs a GL 1.x fixed-function layer on kgfx: a separate,
larger project, done only if a port justifies it (run-time API translation costs CPU, as the
tiny3d A/B showed). Limits that apply to every port: PC code and data assume little-endian
(convert data at build time or swap on load), 8 MiB RAM, 64 MiB ROM, 93 MHz CPU without SIMD.

### Deferred hardware ideas (measure on a console before building)

- **XBUS (RDP commands from DMEM).** Saves only the command bytes on RDRAM (RSP write +
  RDP read); costs the RSP/RDP decoupling: with a small DMEM buffer the RSP waits for the
  RDP and does nothing else meanwhile. For 2D (8x8 tex rects: ~16-24 command bytes vs 128
  written + texels) commands are ~10-15% of the traffic, so the best case is small and only
  when RDRAM bandwidth is the limit. Candidate: an RSP-built tilemap/particle path with an
  idle RSP. kestrel64 does not model RDRAM contention finely enough to judge it.
- **RDRAM 9th bit for video**: see docs/VIDEO.md (nothing beyond Z/alpha masking).

