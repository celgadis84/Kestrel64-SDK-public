# Third-party code

Vendored unchanged except where noted; each keeps its license file. Built into `libk64`
(CMakeLists.txt, warnings off for these sources).

| Dir | What | From | License | Local changes |
|-----|------|------|---------|---------------|
| `libcart/` | Flashcart drivers: SD card, SDRAM access (64Drive, EverDrive-64 X, EverDrive-64 V1-V3, SummerCart64) | libdragon `src/libcart` @ 43c67dcf (upstream github.com/devwizard64/libcart) | Unlicense (public domain), `libcart/LICENSE` | libdragon includes replaced by `k64_glue.h` (PI/cache calls over the SDK) |
| `fatfs/` | FatFs R0.15 file system (FAT12/16/32, exFAT) | libdragon `src/fatfs` @ 43c67dcf (upstream elm-chan.org/fsw/ff) | ChaN's 1-clause BSD-style, `fatfs/License.md` | `ffconf.h`: fast seek on, relative paths (f_chdir), 1 volume, no RTC; `k64/string.h` shim for the freestanding build |

Used through `include/k64/sd.h`, glue in `src/hal/sd.c`.
