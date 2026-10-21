// kestrel64-sdk — compressed assets (.k64z: LZ4 block stream, tools/k64pack.py).
//
// Link packed assets with k64_add_assets(... PACKED name=file ...): ROM-resident, the
// symbol is the PI address of the .k64z header. k64_unpack_rom decodes in place: the
// stream is DMA'd in chunks to the end of the destination buffer while the decoder runs
// behind the DMA, so loading costs about max(PI time of the packed size, decode time)
// instead of the PI time of the raw size. No scratch buffer.
#pragma once
#include "hw.h"

typedef struct {
  u32 fourcc;              // 'K64Z'
  u32 raw_size;           // decoded bytes
  u32 packed_size;        // LZ4 stream bytes
  u32 buf_size;           // in-place buffer: raw_size + margin, multiple of 16
} k64_pack_hdr_t;

#define K64_PACK_FOURCC 0x4B36345Au

// Header of a ROM-resident .k64z (one 16-byte PI read).
k64_pack_hdr_t k64_pack_info(u32 rom_addr);
// Decodes a ROM-resident .k64z into dst (16-byte aligned, buf_size bytes: the tail past
// raw_size is scratch). Returns raw_size. The decoded data is in D$ (not written back).
u32 k64_unpack_rom(void *dst, u32 rom_addr);
// Same, into a new K64_ZONE_HEAP allocation (16-byte aligned). Returns it; *size = raw_size.
void *k64_unpack_rom_alloc(u32 rom_addr, u32 *size);
// Decodes a .k64z already in RAM into dst (raw_size bytes, any alignment, must not
// overlap src). Returns raw_size.
u32 k64_unpack(void *dst, const void *src);
