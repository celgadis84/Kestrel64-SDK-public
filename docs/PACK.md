# Packed assets (.k64z)

`k64/pack.h`, `src/rt/pack.c`, host tool `tools/k64pack.py`, demo 19_pack.

## Format

```
0  'K64Z'
4  u32 raw_size      decoded bytes
8  u32 packed_size   LZ4 block stream bytes
12 u32 buf_size      in-place buffer: raw_size + margin, multiple of 16
16 LZ4 block stream, zero padded to 8 bytes
```

The body is a standard LZ4 block (any LZ4 decoder reads it). Why LZ4 and not a stronger
entropy coder: the N64's PI streams ~5.2 MB/s and the CPU decodes LZ4 at ~17 MB/s, so
loading is bound by PI bytes, and an entropy-coded format would make the CPU the
bottleneck (several times slower per byte for ~10-20% smaller). The ratio gap is closed on
the encoder side instead: `k64pack` does an optimal parse (backward DP on encoded size over
a 256-deep hash chain), which beats greedy/lazy LZ4 encoders by a few percent at no decode
cost.

## In-place streamed decode

`k64_unpack_rom(dst, rom)` needs one buffer of `buf_size`. The stream is DMA'd to its end
(`buf_size - align8(packed)`) in chunks (1 KiB first, doubling to 8 KiB, shrinking again
towards the end) while the decoder runs behind the DMA, writing from the start. It polls the
PI every 512 stream bytes to kick the next chunk, so the PI never sits idle.

`buf_size` is chosen by the packer so the output stays 32 bytes behind the next unread
input byte at every sequence: room for the fast paths' overcopy (up to 24 bytes), and the
decoder never dirties a D$ line still holding stream bytes, nor one a DMA in flight will
fill. The decoder reads only up to the last fully-landed 16-byte line of the DMA'd data.
Typical margin: 30-50 bytes. `tools/test/check_k64pack.py` simulates the in-place decode
and asserts the margin on synthetic data and on any files given.

`k64_unpack(dst, src)`: the plain RAM-to-RAM decoder (no overlap, any alignment).

## Decoder fast paths

- token with < 15 literals: copy 16 bytes with two unaligned 8-byte moves;
- match with offset >= 8 and length <= 24: three 8-byte moves;
- longer matches: 8-byte moves; offset < 8 copies the first 8 bytes one at a time, then
  continues 8 at a time with the source moved back to a multiple of the period >= 8.
- Near the end of the output (last 24 bytes) everything is exact.

Tried and dropped: CACHE Create Dirty Exclusive on output lines before writing them (skips
the write-allocate fill). In kestrel64 it was a 15% loss with the default timing and no gain
with `KESTREL_CACHECOST=40`; the per-sequence bookkeeping eats what it saves.

## Build integration

```cmake
k64_add_rom(NAME game SOURCES main.c
  PACKED "level1=level1.glb:--tris 2400"     # model baked, then packed
         "wall=wall.png:ci4"                  # texture baked, then packed
         "blob=data.bin")                     # any file, packed as is
```

Same keyword in `k64_add_assets`. Packed assets stay in ROM; the symbol is the PI address
of the header.

## Numbers (demo 19_pack, kestrel64)

| asset   |    raw | .k64z  | PI raw    | `k64_unpack_rom` | RAM decode |
|---------|-------:|-------:|----------:|-----------------:|-----------:|
| T-rex model (3 LODs) | 195792 | 70004 (35%) | 37.3 ms | 14.8 ms | 11.1 ms |
| shrine model | 17568 | 7895 (44%) | 3.35 ms | 1.59 ms | 1.15 ms |
| bricks ci4 texture | 576 | 228 (39%) | 112 us | 81 us | 32 us |
| patterns (synthetic) | 22221 | 3945 (17%) | 4.24 ms | 0.98 ms | 0.64 ms |

Loading a packed asset costs about the PI time of its packed size plus the decode of the
last chunk: the T-rex loads 2.5x faster than uncompressed. Tiny assets gain little (one
DMA, fixed costs); pack the big ones.
