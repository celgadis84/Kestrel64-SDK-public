// kestrel64-sdk — K64V video playback (tools/k64vid.py, docs/VIDEO.md).
//
// K64V is the SDK's own transform codec: YUV 4:2:0, 16x16 macroblocks (skip / inter /
// intra / motion), 8x8 integer IDCT defined by the RSP arithmetic (vmulf + 7 x vmacf per output
// row), Exp-Golomb run/level coefficients, VADPCM audio interleaved per frame.
//
// Decoding is split in two stages joined by a work list:
//   parse (CPU)        bitstream -> per macroblock row: modes, coded block patterns and
//                      dequantised coefficients {index, value}
//   reconstruct        work list -> Y/U/V planes (IDCT, + 128 or + the previous frame,
//                      displaced for motion macroblocks, clamp)
// The reconstruct stage has a CPU reference (k64_video_reconstruct_cpu); the RSP back end
// consumes the same work list and must produce the same planes (frame CRCs in the file).
//
// Usage: k64_video_open on the PI address of a .k64v (ROM asset or k64_romfs file), then
// per frame k64_video_decode into an acquired 16bpp surface, and k64_video_audio once per
// game frame. Audio is decoded per packet by the kgfx ADPCM overlay (the mixer's VADPCM
// decoder, docs/AUDIO.md), on the CPU with use_cpu; kgfx hands its RDP command ring over,
// so k64_video_open calls k64_rdp_init if it was not called.
#pragma once
#include "hw.h"
#include "vi.h"

#define K64V_SKIP  0
#define K64V_INTER 1
#define K64V_INTRA 2
#define K64V_MOTION 3

typedef struct {
  // stream description
  u16 width, height;      // multiples of 16
  u32 frames;
  u32 fps;                // 16.16
  u32 audio_rate;         // 0: no audio
  // decoded frame (KSEG0, planar): Y width x height, U and V width/2 x height/2
  u8 *y, *u, *v;
  u8 *ry, *ru, *rv;       // previous frame (the prediction source); swapped with y/u/v per frame
  u32 frame;              // frames decoded so far
  u32 crc;                // FNV-1a of the Y, U, V planes the encoder reconstructed (last frame)
  u32 acrc;               // FNV-1a of the audio samples the encoder decoded (last packet)
  // timing of the last k64_video_decode, in CPU count ticks: CPU parse, and the rest of the
  // frame (reconstruct, or the RSP's share not hidden behind the parse)
  u32 t_parse, t_recon;
  u32 t_audio;            // audio decode (RSP run, including the microcode switch)
  bool use_cpu;           // reconstruct on the CPU (reference) instead of the RSP
  // internal
  u32 rom, next_size, max_packet;
  u8 *pkt[2];
  u8 cur;
  bool prefetching;
  u16 *work[2];           // macroblock row work lists (parse one while the RSP reads the other)
  const u8 *bits;         // bit reader: bitstream, bit position, length in bits
  u32 pos, nbits;
  bool is_p;
  u16 prow;               // macroblock row parse_row parses next
  u16 step[64];           // dequantiser step per natural-order coefficient
  s16 *pcm;               // audio ring (mono), whole 16-sample frames
  u32 pcm_len, pcm_rd, pcm_wr;
  u32 a_first, a_count;   // samples decoded from the last packet (0: none, or dropped)
  u32 max_aframes;
  u32 *adec;              // VADPCM decoder block (kgfx.S adpcm)
  void *arecs;            // decode records
  u64 *alist;             // kgfx list running them
  u8 aslot;               // state slot holding the state after the last packet
  // Source other than the ROM (set before k64_video_open, kept by it): reads len bytes at
  // byte pos of the stream into dst (KSEG0, 16-byte aligned), e.g. a file on the SD card;
  // rom_addr is then the stream's start in that source. Packets are read when decoded
  // instead of prefetched by PI DMA. NULL: the stream is in the ROM, at a PI address.
  bool (*read)(void *ctx, void *dst, u32 pos, u32 len);
  void *read_ctx;
  // Memory for the buffers (set before k64_video_open, kept by it), 16-byte aligned, e.g.
  // a cache the caller can rebuild after the video; NULL: the heap.
  u8 *arena;
  u32 arena_len, arena_used;
  struct {                // buffers kept by k64_video_open for the next stream of the same v
    void *p[9];
    u32 n[9];
  } mem;
} k64_video_t;

// Reads the header and allocates the buffers (K64_ZONE_HEAP). False: not a K64V v3 stream.
// Opening another stream with the same v (zeroed before its first open) reuses its buffers
// where they are large enough: the heap is never freed, so a player of many streams keeps one
// k64_video_t.
bool k64_video_open(k64_video_t *v, u32 rom_addr);
// Decodes the next frame into v->y/u/v and, if fb is not NULL, draws it in RGBA5551 at
// (x, y) of that 16bpp surface. CPU parse; reconstruct and colour conversion on the RSP
// (microcode kvid, loaded on demand: it replaces whatever runs there), on the CPU with
// use_cpu. The RSP draws straight to RDRAM when the frame fits the surface and x is a
// multiple of 4 (8-byte DMA alignment); otherwise k64_video_blit does it. False at the end
// of the stream or on a corrupt packet.
bool k64_video_decode(k64_video_t *v, const k64_surface_t *fb, int x, int y);
// FNV-1a over the current Y, U, V planes (compare with v->crc).
u32  k64_video_plane_crc(const k64_video_t *v);
// FNV-1a over the audio samples of the last packet (s16 big-endian bytes; compare with
// v->acrc when the packet had audio).
u32  k64_video_audio_crc(const k64_video_t *v);
// Converts the current frame to RGBA5551 at (x, y) of a 16bpp surface on the CPU (the
// reference of the RSP conversion, same result bit for bit) and writes the lines back from D$.
void k64_video_blit(const k64_video_t *v, const k64_surface_t *s, int x, int y);
// Feeds the AI from the decoded audio (k64_audio_init(v->audio_rate, ...) first). Call
// once per game frame; plays silence when the ring runs dry.
void k64_video_audio(k64_video_t *v);

// ---- stages (exposed for tests and for the RSP back end) --------------------------------
// Work list of one macroblock row, u16 words: per macroblock {mode | cbp << 2}; for
// K64V_MOTION then the byte offsets of the source block in the previous frame's planes as
// two u32 (high half first): luma (top-left of the 16x16), chroma (top-left of the 8x8, same
// in U and V); then per coded block (cbp bit 5 = Y0 ... bit 0 = V) {count, count x
// {index v*8+u, value}}.
// Parses one macroblock row of the frame started by k64_video_parse_begin; returns the
// words written, 0 on a corrupt stream.
bool k64_video_parse_begin(k64_video_t *v, const u8 *bits, u32 len, bool *is_p);
u32  k64_video_parse_row(k64_video_t *v, u16 *work);
void k64_video_reconstruct_cpu(k64_video_t *v, const u16 *work, u32 mb_row);
