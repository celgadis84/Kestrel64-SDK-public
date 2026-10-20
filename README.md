# kestrel64-sdk

Bare-metal Nintendo 64 SDK built around its own RSP microcode (**kgfx**) and a direct RDP
command stream. C on the CPU, hand-written RSP vector code, host tools in Python; builds
with clang + CMake/Ninja and runs on hardware and on the kestrel64 emulator.

Development happens in a private repository. This public repository receives the parts
considered mature enough to release, as squashed snapshots, so its history is a list of
releases, not of the work.

## Demos

| ROM | Shows |
|-----|-------|
| 01_hello | boot, VI, double buffer, CPU drawing, debug print |
| 02_sprites | 2D on the RDP: tiled CI4 background, sprites, TLUT |
| 03_cube | first 3D: Gouraud, Z-buffer, transform + setup on the CPU |
| 04_rsp | kgfx self-test (vertex + triangle setup checked against a C model) |
| 05_kgfx_cube | the 03 cubes through kgfx |
| 06_kgfx_lit | RSP lighting (directional lights + ambient, packed normals) |
| 07_kgfx_envmap | texture coordinate generation (sphere map) |
| 08_kgfx_room | camera inside geometry, near-plane clipping, textured + lit |
| 09_rdp_bench | FILL / COPY / 1-cycle / 2-cycle costs |
| 10_trex | skinned, animated model baked from glTF (`k64bake.py model`) |
| 11_audio | mixer voices: looped waveforms at several pitches, one-shot drum, VADPCM |
| 12_static | static (unskinned) glTF scene on kgfx |
| 13_fillgeo | flat-shaded geometry in FILL mode versus 1-cycle |
| 14_save | cartridge saves (EEPROM / SRAM / FlashRAM): boot counter that survives power-off |
| 15_pak | controller accessories: Controller Pak, Rumble Pak, Transfer Pak probe |
| 16_stress | geometry stress test: N lit, z-buffered objects |
| 17_timers | 64-bit time base and polled deadline timers (self-test) |
| 18_threads | opt-in libultra-style threads, message queues, RCP events (self-test) |
| 19_pack | compressed `.k64z` assets decoded behind the PI DMA (self-test) |
| 20_fonts | proportional kerned fonts, UTF-8, color codes, word wrap (self-test) |
| 21_particles | kgfx PARTICLES command and Y billboards (self-test) |
| 22_music | MIDI song through the sequencer (self-test) |
| 23_impostor | T-Rex herd as baked impostor billboards |
| 24_hires | 640x480 interlaced, per-field origin from the vblank interrupt (self-test) |
| 25_reset | reset button: pre-NMI warning, warm-boot counter in the NMI buffer |
| 26_romfs | ROM file system: paths, listing, reads at any alignment, packed files (self-test) |
| 27_tpak | Transfer Pak: Game Boy carts (MBC1/2/3+RTC/5) header, ROM, RAM, clock (self-test) |
| 28_joybus | joybus device identification, cartridge RTC, mouse motion (self-test) |
| 29_video | K64V video + VADPCM audio from ROM, RSP decode into the framebuffer, video/audio CRCs + RGB vs CPU (self-test) |
| 30_texfmt | every baked texture format (RGBA16/32, YUV16, CI4/8 with RGBA16 or IA16 palettes, I4/8, IA4/8/16) drawn by the RDP vs a CPU decode (self-test) |
| 31_memfill | k64_mem_fill (MI repeat mode) at every alignment, RDRAM checked, timing vs memset (self-test) |

## Build

```sh
cmake -S . -B build -G Ninja
ninja -C build
```

ROMs land in `build/demos/<name>/<name>.z64`. Docs: [docs/KGFX.md](docs/KGFX.md) (microcode),
[docs/MODELS.md](docs/MODELS.md) (models/animation), [docs/RDP-MODES.md](docs/RDP-MODES.md),
[docs/RSP-RESIDENCY.md](docs/RSP-RESIDENCY.md), [docs/ROADMAP.md](docs/ROADMAP.md).

## Credits

- **T-Rex model (demo 10_trex)**: "Tyrannosaurus Rex Lowpoly" by **pikabobalex**,
  <https://sketchfab.com/3d-models/tyrannosaurus-rex-lowpoly-516dbe9717554e74b0bfcbb77a431b17>,
  licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). Obtained via the
  LibSaturn project's dino_demo. Converted (decimated, texture atlas, quantized animation) by
  kestrel64-sdk; see [assets/trex/LICENSE.txt](assets/trex/LICENSE.txt). The credit is also
  shown on screen in the demo.
- **[libdragon](https://github.com/DragonMinded/libdragon)** (Unlicense): the kgfx triangle
  setup method comes from `rsp_rdpq_tri.inc` by Giovanni Bajo (`src/rsp/kgfx.S`) and the VI
  timing values from its `vi.c` (`src/hal/vi.c`). Its rdpq and the n64brew docs were a
  reference for RDP sync rules and render modes.
- **[tiny3d](https://github.com/HailToDodongo/tiny3d)** by HailToDodongo: reference for the 3D
  pipeline, the packed 5.6.5 normal encoding, and the scenes used in the A/B comparisons.
- IPL3: libdragon's open-source IPL3, see [third_party/ipl3](third_party/ipl3/LICENSE-libdragon.md).

## License

MIT, see [LICENSE](LICENSE). Third-party code and assets keep their own licenses
(`third_party/`, `assets/`, credits above).
