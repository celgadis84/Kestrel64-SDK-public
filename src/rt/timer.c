// kestrel64-sdk — time base and deadline timers (see include/k64/timer.h).
#include <k64/timer.h>
#include <k64/cpu.h>

// Count at the last k64_time(). A smaller value now means Count wrapped (about every 92 s).
static u32 last_count;
static u64 high;                  // wraps of Count seen so far, << 32

// Interrupts off around the update: with threads (k64/thread.h) a thread preempted between
// reading Count and storing it would store a stale value and count a false wrap later.
u64 k64_time(void) {
  u32 ie = k64_irq_disable();
  u32 c = k64_count();
  if (c < last_count) high += 1ull << 32;
  last_count = c;
  u64 t = high | c;
  k64_irq_restore(ie);
  return t;
}

// Armed timers sorted by deadline; the head fires first.
static k64_timer_t *pending;

// Removes t from the pending list if it is there, and clears its link state.
static void unlink(k64_timer_t *t) {
  for (k64_timer_t **p = &pending; *p; p = &(*p)->next)
    if (*p == t) { *p = t->next; break; }
  t->next = NULL;
  t->active = false;
}

// Sorted insert by deadline.
static void insert(k64_timer_t *t) {
  k64_timer_t **p = &pending;
  while (*p && (*p)->deadline <= t->deadline) p = &(*p)->next;   // FIFO among equal deadlines
  t->next = *p;
  *p = t;
  t->active = true;
}

void k64_timer_start(k64_timer_t *t, u64 delay, u64 period, k64_timer_fn fn, void *arg) {
  if (t->active) unlink(t);
  t->deadline = k64_time() + delay;
  t->period = period;
  t->fn = fn;
  t->arg = arg;
  t->overruns = 0;
  insert(t);
}

void k64_timer_stop(k64_timer_t *t) {
  if (t->active) unlink(t);
}

u32 k64_timer_poll(void) {
  u64 now = k64_time();
  u32 ran = 0;
  while (pending && pending->deadline <= now) {
    k64_timer_t *t = pending;
    pending = t->next;
    t->next = NULL;
    t->active = false;
    if (t->period) {                      // re-arm before the call: the callback may stop it
      u64 next = t->deadline + t->period;
      if (next <= now) {
        u64 missed = (now - next) / t->period + 1;
        t->overruns += (u32)missed;
        next += missed * t->period;
      }
      t->deadline = next;
      insert(t);
    }
    t->fn(t, t->arg);
    ran++;
  }
  return ran;
}

u64 k64_timer_next(void) {
  if (!pending) return ~0ull;
  u64 now = k64_time();
  return pending->deadline > now ? pending->deadline - now : 0;
}
