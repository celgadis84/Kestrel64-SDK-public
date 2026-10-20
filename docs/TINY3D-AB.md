# tiny3d A/B: libdragon vs kestrel64-sdk

Same tiny3d example sources, built twice:

- **A**: tiny3d on libdragon (preview branch, commit 43c67dc), official prebuilt
  toolchain (GCC 16.2, libdragon `.deb`), tiny3d's own RSP microcode.
- **B**: kestrel64-sdk with the compat layer (`compat/`, clang): libdragon's rspq/rdpq and
  tiny3d's API run on the CPU and drive the kgfx microcode (see MODELS.md §4).

Scenes: `00_quad`, `01_model`, `03_objects`. Same resolution (320×240, 16 bpp), same
filters, same assets (B loads the `.t3dm` / `.sprite` files unconverted).

## Patches

- Example sources: **none**, on either side.
- libdragon and tiny3d: **none**. Before kestrel64 b2aec21 every A ROM died at boot
  ("rspq_syncpoint_wait timed out"). The cause was the emulator: in threaded mode a
  CLEAR_HALT that landed between the RSP's `break` and its delayed halt was lost (see
  EMULATOR-NOTES.md). A temporary re-kick patch in libdragon confirmed the cause; with
  b2aec21 the unpatched ROMs give the same numbers.
- A prints one "VI WARNING: __vblank_interrupt outside of vblank period: 374" at boot (a VI
  interrupt still pending from before libdragon sets V_INTR). Harmless.

## Method

kestrel64 b2aec21 (build-static), `tools/ab_stats.py`: 3 s warm-up, then
`rdp.stats` over 120 fields. `msPerFlip` is guest time between framebuffer flips; RSP and
RDP busy are percentages of guest time. With `--timing` (`KESTREL_RSP_TIMING=1`) the
default scenes gave identical numbers (not investigated).
These are emulator figures: kestrel64's RSP and RDP timing models are not calibrated
against hardware yet.

## Results

Default scenes (vsync-limited):

| Scene | Side | ms/flip | RDP busy | RSP busy | tris/flip | RDP gclk/flip |
|---|---|---|---|---|---|---|
| 00_quad | A | 16.68 | 4.95% | 3.14% | 2 | 51.6k |
| | B | 16.68 | 6.97% | 0.22% | 2 | 72.7k |
| 01_model | A | 16.68 | 32.1% | 44.5% | 523 | 335k |
| | B | 16.68 | 33.9% | 34.6% | 520 | 354k |
| 03_objects (50 actors) | A | 16.68 | 19.3% | 64.4% | 889 | 202k |
| | B | 16.68 | 21.5% | 49.5% | 880 | 224k |

Stress: `03_objects` with C-right held (250 actors, `--pad cright --warmup 5`):

| Side | ms/flip | RDP busy | RSP busy | tris/flip | RSP time/flip |
|---|---|---|---|---|---|
| A | 50.05 | 26.2% | 100% | 4458 | 50.1 ms (11.2 µs/tri) |
| B | 69.03 | 19.8% | 60.3% | 4443 | 41.6 ms (9.4 µs/tri) |

Both render the same images (`build/a_*.png` vs `build/b_*.png`).

## Reading

- At the default load both sides hold 60 fps. B now spends less RSP than tiny3d's own
  microcode (01: 34.6% vs 44.5%, 03: 49.5% vs 64.4%) and a bit more RDP.
- Under stress A is RSP-bound (100%). B is CPU-bound: the compat interprets the rspq queue
  on the CPU (vertex unpacking, kgfx cache allocation, list building), ~15.5 µs of CPU per
  triangle (69 ms / 4443), and the RSP waits on it. CPU profile: triangle windows 56%, t3d
  command decode 16%, rdpq overlay 8%, render-mode updates 4%. The compat is built
  `-ffreestanding`, so `<string.h>` maps constant-size `mem*` calls to the compiler builtins
  (inlined word copies): that alone took the stress frame from 77 to 69 ms.
  Per triangle B's RSP time is 9.4 µs against tiny3d's 11.2 µs.
- The RDP side is close. B's `00_quad` frame costs 72.7k gclk vs 51.6k; both clear with
  fill rectangles, so the gap is elsewhere in the command stream (not investigated).

## Vertex loading in the compat

tiny3d loads up to 70 vertices into its own cache, kgfx keeps 32. The first compat made a
tiny3d vertex resident when a triangle missed it and extended the load only over adjacent,
free slots: in `03_objects` (50 actors) that was 1.9 vertices per VTX command (debug
counters: 136.8k VTX for 264k vertices over 120 fields), and each VTX has a fixed RSP cost.
Now triangles are queued per tiny3d
draw command and drawn in windows: a window takes triangles while their vertices fit in the
kgfx cache, loads every missing vertex at once (gathered from the tiny3d buffer, into
consecutive slots from a wrapping cursor, skipping the slots the window uses), then emits
the triangles in pairs. The images are pixel-identical to before; RSP time per frame
dropped 27..36%.

## What the compat needed to reach 60 fps

Before, B ran `01_model` and `03_objects` at 33.4 ms/flip: each frame was interpreted, then
the CPU waited for the RSP, then for the vblank. Now (commit 03d6c54):

- kgfx submissions are double-buffered and asynchronous; raw RDP always goes into a list;
- `rdpq_detach_show` (SYNC_FULL + callback) and rspq callbacks are deferred until their
  submission is done; `rspq_flush` does not wait when the RSP is busy;
- three framebuffers: `k64_display_show` writes `VI_ORIGIN` at once (one swap per field);
- one list-space check per batch of triangles in the t3d interpreter.

## Caveats

- kestrel64 sometimes dies mid-run while being driven over telemetry (about 1 in 5 runs,
  both sides, not reproducible with `--run`); `ab_stats.py` retries.
- No hardware numbers yet.
