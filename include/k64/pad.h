// kestrel64-sdk — controllers over the SI/PIF joybus.
//
// One SI round trip (write 64B command block + read 64B reply) costs ~0.5 ms of PIF time
// but almost no CPU: k64_pad_poll_start() kicks it, k64_pad_poll_end() collects it.
// k64_pad_poll() does both, blocking. Call once per frame.
#pragma once
#include "hw.h"

enum {
  K64_PAD_A = 0x8000, K64_PAD_B = 0x4000, K64_PAD_Z = 0x2000, K64_PAD_START = 0x1000,
  K64_PAD_UP = 0x0800, K64_PAD_DOWN = 0x0400, K64_PAD_LEFT = 0x0200, K64_PAD_RIGHT = 0x0100,
  K64_PAD_L = 0x0020, K64_PAD_R = 0x0010,
  K64_PAD_CUP = 0x0008, K64_PAD_CDOWN = 0x0004, K64_PAD_CLEFT = 0x0002, K64_PAD_CRIGHT = 0x0001,
};

typedef struct {
  bool present;
  u16  held, pressed, released;   // edge masks vs previous poll
  s8   x, y;                      // analog stick, roughly -80..80
} k64_pad_t;

extern k64_pad_t k64_pads[4];

void k64_pad_poll_start(void);
void k64_pad_poll_end(void);
void k64_pad_poll(void);
