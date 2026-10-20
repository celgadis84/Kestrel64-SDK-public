// kestrel64-sdk — reset button (pre-NMI) and warm-boot state.
//
// Pressing reset makes the PIF raise the CPU's pre-NMI interrupt (Cause IP4) about half a
// second before it resets the CPU through NMI. A game uses that time to stop audio and
// RCP work so the reset does not land in the middle of a DMA. After the NMI, IPL3 runs
// again and reports a warm boot; RDRAM is kept, including the 64-byte NMI buffer below.
#pragma once
#include "hw.h"

// Enables the pre-NMI interrupt (and CPU interrupts). Works with or without k64/thread.h;
// with threads, K64_EVENT_PRENMI is posted as well.
void k64_reset_watch(void);
// True once the reset button was pressed (the NMI follows within ~0.5 s).
bool k64_reset_pending(void);
// Count ticks since the press (0 when none).
u32  k64_reset_elapsed(void);
// True when this boot came from the reset button instead of power-on.
bool k64_reset_warm(void);
// 64 bytes of RDRAM the boot code leaves alone across a reset (libultra osAppNMIBuffer).
// Zero after power-on.
u8  *k64_reset_buffer(void);
