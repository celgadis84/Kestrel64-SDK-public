// kestrel64-sdk — time base and deadline timers.
//
// Time is a 64-bit tick count at K64_COUNT_HZ (46.875 MHz), built by extending COP0 Count,
// which wraps every ~91.6 s. The extension only sees a wrap if k64_time() runs at least once
// per wrap period; k64_display_show() and k64_timer_poll() call it, so any game that shows
// frames or polls timers is covered.
//
// Timers are polled, not interrupt driven (the runtime runs with interrupts masked): the
// main loop calls k64_timer_poll(), which runs the callbacks whose deadline has passed, in
// deadline order. A callback may start or stop any timer, itself included; a timer it arms
// runs on a later poll (each poll works on one time snapshot, so it always terminates, even
// with zero delays). Lateness is
// bounded by how often the loop polls (once per frame = up to a frame late).
//
// Periodic timers keep their phase: the next deadline is the previous one plus the period,
// never "now + period", so they do not drift. If the loop falls more than a period behind,
// the missed periods are skipped (counted in `overruns`) instead of firing in a burst.
#pragma once
#include "hw.h"

#define K64_TICKS_US(us)  ((u64)(us) * K64_COUNT_HZ / 1000000u)
#define K64_TICKS_MS(ms)  ((u64)(ms) * (K64_COUNT_HZ / 1000u))

// Monotonic ticks since boot (64-bit, does not wrap in practice).
u64 k64_time(void);
K64_INLINE u64 k64_time_us(void) { return k64_time() * 1000000u / K64_COUNT_HZ; }
K64_INLINE u64 k64_ticks_to_us(u64 t) { return t * 1000000u / K64_COUNT_HZ; }

typedef struct k64_timer k64_timer_t;
typedef void (*k64_timer_fn)(k64_timer_t *t, void *arg);

struct k64_timer {
  u64 deadline;           // absolute, in k64_time() ticks
  u64 period;             // 0 = one-shot
  k64_timer_fn fn;
  void *arg;
  u32 overruns;           // periods skipped because the loop polled too late
  bool active;
  k64_timer_t *next;      // internal: pending list, sorted by deadline
};

// (Re)starts `t`: first call after `delay` ticks, then every `period` ticks (0 = once).
void k64_timer_start(k64_timer_t *t, u64 delay, u64 period, k64_timer_fn fn, void *arg);
void k64_timer_stop(k64_timer_t *t);
// Runs every due callback. Returns how many ran.
u32 k64_timer_poll(void);
// Ticks until the earliest deadline (0 if one is due, ~0 if no timer is running).
u64 k64_timer_next(void);
