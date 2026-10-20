# Emulator notes

Behaviour differences found while validating the SDK. Hardware-accurate reference is
parallel-rdp (and real hardware when available); every demo is checked in both kestrel64
renderers (`tools/run_kestrel.sh rom out.png [maxinsn] [prdp]`).

| Date | Symptom | Root cause | Where |
|------|---------|------------|-------|
| 2026-10-01 | CI4 background grey (blue channel replicated) in parallel-rdp, correct in SoftRDP | Other-modes had BILERP0/BILERP1 clear, so the texture filter ran its YUV→RGB convert path with K0..K5. SDK bug (fixed: `K64_OM_BASE` sets both). kestrel64 SoftRDP ignores the convert path → hides the bug. | SDK fixed; kestrel64 SoftRDP inaccuracy reported |
| 2026-10-01 | Demos 03, 05: dark 1-px seams along shared triangle edges in SoftRDP only (05: 567 px differ vs prdp, both RSP and CPU setup paths) | Edge-walk/coverage rounding in kestrel64 SoftRDP (parallel-rdp renders the same command stream seamless). Emulator-side. | kestrel64 SoftRDP. **Fixed** in kestrel64 846aa18 (integer edge walk as parallel-rdp); 06 verified seamless 2026-10-02 |
| 2026-10-01 | RSP microcode scheduling changes (dual issue, latency hiding) do not change kestrel64 RSP timings | kestrel64 executes one RSP instruction per cycle with no stall or SU/VU dual-issue model; only instruction count is visible. Use `tools/rsp_cycles.py` (static) or real hardware for scheduling work. | **Resolved** in kestrel64 4f6b5bd: `KESTREL_RSP_TIMING=1` (stalls, dual issue, DMA wait estimate; see docs/KGFX.md) |
| 2026-10-02 | 09_rdp_bench: FILL/COPY only ~20% faster than 1-cycle (0.97 vs 1.20 clk/px); 1200 small triangles cost the same busy cycles as 2 big ones | RDP cost model calibrated on Thar0's 1/2-cycle fills only: FILL/COPY extrapolated with the same per-chunk memory cost; no per-primitive / per-span overhead. See docs/RDP-MODES.md | kestrel64 RDP timing, reported to the kestrel64 session 2026-10-02. **Modelled** in kestrel64 cdfb818: FILL/COPY write 64 bits/GCLK past the span buffer, 1 dead GCLK per primitive line, 1/2-cycle span chunks rounded per line (FILL/COPY now ~4.4x 1-cycle). Per-primitive setup still unmodelled (no source gives a number) |
| 2026-10-04 | libdragon ROMs (tiny3d A side) die at boot: "rspq_syncpoint_wait timed out"; RSP halted+broke at PC 0x18 with SIG_MORE set | rspq wake race: `RSPQCmd_WaitNewInput` runs its `break` after both CPU wake-up writes to SP_STATUS (SET_SIG_MORE, CLEAR_HALT), which libdragon assumes cannot happen on hardware. A temporary re-kick patch in the A-side libdragon copy confirmed it | kestrel64 threaded RSP. **Fixed** in kestrel64 b2aec21: `break`'s HALT\|BROKE is published at a deadline behind the instruction; a CLEAR_HALT stamped after the `break` now relaunches the RSP (`KESTREL_SPRELAUNCH=0` restores the old behaviour). Same commit fixes VI_V_CURRENT drift (spurious libdragon "VI WARNING") |

## SRAM 768K bank stride (2026-10-04)

kestrel64 `Memory::saveOffset` decodes the banked 768K SRAM (header save type 4) as banks
64 KiB apart (`off >> 16`). Hardware selects the three 32 KiB chips with address bits 18-19:
0x08000000, 0x08040000, 0x08080000 (Dezaemon 3D's boot check reads 0x08040000; n64js PR #84).
Fixed in kestrel64 5c5b83a; demo 14_save_sram768 passes there.
