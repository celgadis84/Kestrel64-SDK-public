# Getting started

From an empty folder to a ROM that runs, then where each part of the SDK lives.

## 1. What you need

| Tool | Why |
|------|-----|
| LLVM: `clang`, `ld.lld`, `llvm-objcopy` (any recent release) | compiles C and CPU assembly for the VR4300 (`mips64-none-elf`, n32 ABI). No GCC cross toolchain is used. |
| CMake >= 3.20 and Ninja | build |
| Python 3 | host tools: RSP assembler (`tools/k64rsp.py`), ROM builder, asset bakers |
| Pillow (`pip install pillow`) | textures, fonts, the boot logo |
| numpy (`pip install numpy`) | models, impostors, video (only when a ROM bakes them) |

The same files build on Windows (MSYS2 `clang64` works), Linux and macOS. If the LLVM tools
are not on `PATH`, point `K64_LLVM_DIR` (CMake variable or environment) at their `bin`
folder.

## 2. Build the SDK and its demos

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

Every demo lands in `build/demos/<name>/<name>.z64`. `demos/01_hello` is the smallest;
the README lists what each one shows.

## 3. Your own game

`template/` is a complete project outside the SDK tree: copy it anywhere and point it at
the SDK.

```sh
cp -r kestrel64-sdk/template mygame
cmake -S mygame -B mygame/build -G Ninja -DK64_SDK=/path/to/kestrel64-sdk
ninja -C mygame/build
```

Its `CMakeLists.txt` sets the SDK toolchain before `project()`, pulls the SDK in with
`add_subdirectory` (demos off) and declares the ROM:

```cmake
k64_add_rom(NAME mygame SOURCES main.c TITLE "MY GAME")
```

`k64_add_rom` also takes the save type (`SAVE eeprom4k|eeprom16k|sram256k|sram768k|sram1m|flash1m`),
extra microcode (`UCODE`), assets (`TEXTURES`, `SOUNDS`, `FONTS`, `MODELS`, `MUSIC`, `FILES`,
`ROMFS` ...) and `NO_LOGO`. The full list, with what each asset kind becomes in C, is the
comment at the top of `cmake/k64.cmake`.

## 4. Running it

- **Emulator**: the SDK is developed against kestrel64; `tools/run_kestrel.sh rom.z64 out.png`
  runs a ROM headless, prints its debug log and saves the last frame. Any accurate emulator
  that boots homebrew should run the ROMs.
- **Hardware**: copy the `.z64` to a flashcart (64Drive, EverDrive-64, SummerCart64). Debug
  output (`k64_printf`) goes to the ISViewer port, which those carts and emulators forward to
  a host console.

## 5. What happens before `main`

`crt0` copies nothing (IPL3 loaded the image), clears BSS and calls `k64_runtime_init`:
exception vectors (a crash prints a register dump and backtrace on screen), RDRAM detection
and zones (`k64/mem.h`), every RCP interrupt masked. The boot logo plays next unless the ROM
was built with `NO_LOGO`. Then `main()` runs. Returning from it calls `k64_exit`.

The core runtime has **no threads and no interrupts**: everything is polled from your loop.
Threads, message queues and RCP events with libultra semantics are opt-in (`k64/thread.h`,
`docs/SCHEDULER.md`).

## 6. The frame loop (template/main.c)

```c
k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
k64_rdp_init();                 // RDP command ring
k64_text_init();                // built-in debug font
for (;;) {
  k64_pad_poll_start();         // controller read runs on the SI while we draw
  const k64_surface_t *fb = k64_display_acquire();   // a buffer the VI is not showing
  k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
  k64_rdp_clear(color * 0x10001u, false);
  ...draw...
  k64_pad_poll_end();           // k64_pads[] updated
  k64_rdp_wait_idle();          // RDP done with fb
  k64_display_show(fb);         // swapped in at the next vblank
}
```

Three rules that every N64 program has to follow, and that the SDK does not hide:

1. **Cache.** The CPU caches RDRAM, the RCP does not see the cache. Before the RSP, RDP, VI or
   a DMA reads data the CPU wrote: `k64_dcache_writeback(ptr, len)`. Before the CPU reads
   what they wrote: `k64_dcache_invalidate(ptr, len)`. Keep such buffers 16-byte aligned and
   padded to 16 (`K64_ALIGN(16)`), see `k64/cpu.h`.
2. **Addresses.** The RCP takes physical addresses. SDK functions accept KSEG0 pointers and
   convert; raw command encoders (`k64_rdpc_*`) do not.
3. **Memory banks.** Framebuffers, Z buffer and textures each get their own 1 MiB region
   (`K64_ZONE_FB`, `K64_ZONE_Z`, heap), so the RDP's reads and writes do not fight over one
   RDRAM bank. `k64_mem_alloc(zone, size, align)` places them.

## 7. 3D: the kgfx microcode

kgfx is the SDK's RSP microcode: it transforms, lights, clips and culls vertices and writes
RDP triangles straight into the RDP ring. The CPU builds a short command list per frame
(`demos/05_kgfx_cube`):

```c
k64_kgfx_list_t l;
k64_kgfx_begin(&l, list_buf, K64_ARRAY_LEN(list_buf));
k64_kgfx_viewport(&l, &vp);
k64_kgfx_geommode(&l, K64_TRI_SHADE | K64_TRI_Z, 0, K64_KGFX_CULL_BACK);
k64_mtx_to_fixed(&mtx, &mvp);              // whole model-view-projection, s15.16
k64_kgfx_mtx(&l, &mtx);
k64_kgfx_vtx(&l, verts, 8, 0);             // into the vertex cache, slot 0
k64_kgfx_tri2(&l, 0, 2, 1, 0, 3, 2);       // two triangles by cache slot
k64_kgfx_end(&l);
k64_dcache_writeback(&mtx, sizeof mtx);    // rule 1: the RSP reads it
k64_kgfx_run(&l);                          // starts the RSP, returns at once
...CPU work...
k64_kgfx_wait();
```

There is no matrix stack: each `mtx` is the full transform. Commands, limits and the vertex
format are in `docs/KGFX.md`; render modes in `docs/RDP-MODES.md`.

## 8. Where things are

| Header | Covers | Docs / demo |
|--------|--------|-------------|
| `k64/k64.h` | umbrella: includes everything below | |
| `vi.h` | display init, framebuffer chain, vblank | 01, 24 |
| `rdp.h`, `tex.h`, `tri.h` | RDP command ring, modes, textures + TMEM, CPU triangle setup | RDP-MODES.md, 02, 03, 09, 30 |
| `kgfx.h` | kgfx microcode: command lists, lights, texgen, particles | KGFX.md, PARTICLES.md, 05-08, 21 |
| `model.h`, `impostor.h` | baked glTF models (skinned, LODs), impostor billboards | MODELS.md, 10, 12, 23 |
| `font.h` | proportional fonts, UTF-8, word wrap | FONTS.md, 20 |
| `audio.h`, `music.h` | AI output, voice mixer, VADPCM, MIDI sequencer | AUDIO.md, MUSIC.md, 11, 22 |
| `video.h` | K64V video playback (RSP decode) | VIDEO.md, 29 |
| `pad.h`, `joybus.h`, `pak.h`, `tpak.h`, `rtc.h` | controllers, raw joybus, Controller/Rumble/Transfer Pak, cartridge clock | PAK.md, 15, 27, 28 |
| `save.h` | EEPROM / SRAM / FlashRAM | 14 |
| `pi.h`, `pack.h`, `romfs.h`, `sd.h` | cartridge DMA, LZ4 assets, ROM file system, flashcart SD + FAT | PACK.md, 19, 26 |
| `mem.h` | RDRAM zones and arenas | 31 |
| `cpu.h`, `timer.h`, `fmath.h` | COP0, cache, Count timing, deadline timers, float math without libm | 17 |
| `thread.h`, `reset.h` | opt-in threads + message queues, reset button / NMI | SCHEDULER.md, 18, 25 |
| `rsp.h`, `hw.h` | load and run your own microcode, raw register map | RSP-RESIDENCY.md, 04 |
| `debug.h` | `k64_printf`, asserts, panic, crash screen | 32 |

Every public function has a comment above its prototype stating units, ranges, what it
allocates and how it fails; the headers are the reference.

## 9. Tests

- Demos marked *self-test* in the README check themselves against a CPU model or known data
  and print `self-test PASS` / `FAIL` to the debug log (and on screen).
  `tools/test/run_selftests.sh build` runs all 14 of them headless in kestrel64 (build first;
  about 6 minutes in all, 4-70 s per ROM; 27_tpak gets synthetic Game Boy carts from
  `demos/27_tpak/mkgbrom.py`). A ROM far slower than that is hung, not slow.
- `tools/test/check_k64rsp.py` (RSP assembler vs armips golden output) and
  `tools/test/check_k64pack.py` (LZ4 packer round trip) cover the host tools.
