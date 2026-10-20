// kestrel64-sdk — RDP triangle setup on the CPU.
//
// This is the reference implementation (float, VR4300 FPU). The kgfx RSP microcode does the
// same job in fixed point and is validated against it; the CPU path also serves as a
// fallback for low triangle counts or debugging.
#pragma once
#include "rdp.h"

enum { K64_TRI_Z = 1, K64_TRI_TEX = 2, K64_TRI_SHADE = 4 };

// Screen-space vertex.
typedef struct {
  float x, y;          // pixels (sub-pixel precise)
  float z;             // depth 0..1 (0 = near)
  float inv_w;         // 1/w (perspective-correct texturing); any positive value if no TEX
  float r, g, b, a;    // 0..255
  float s, t;          // texels
} k64_svtx_t;

// Emits one RDP triangle command (8..22 words). `flags` = K64_TRI_* mask, `tile` = render tile.
void k64_rdp_tri(u32 flags, u32 tile, const k64_svtx_t *v1, const k64_svtx_t *v2, const k64_svtx_t *v3);
