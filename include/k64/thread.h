// kestrel64-sdk — opt-in threads with libultra semantics (decision: docs/SCHEDULER.md).
//
// Fixed priorities (higher runs first, FIFO among equals). The running thread keeps the
// CPU until it blocks on a message queue, lowers its priority, yields, or a higher-priority
// thread becomes ready — through a message it sends, or a message an interrupt posts (RCP
// events, Count/Compare). No time slicing.
//
// Interrupts are enabled by k64_threads_init() and stay masked per source until an event
// is registered for it. The kernel's handler acknowledges the sources it owns.
// SDK subsystems are not reentrant: use each from one thread (docs/SCHEDULER.md).
#pragma once
#include "hw.h"

typedef void *k64_msg_t;

// Saved context, layout shared with crt0.S (k64_thread_exc).
typedef struct {
  u64 gpr[32];            // 0   ($0, $k0, $k1 not saved)
  u64 hi, lo;             // 256, 264
  u32 epc, sr;            // 272, 276
  u32 fcsr, partial;      // 280, 284: 1 = saved by a syscall, callee-saved registers only
  u64 fpr[32];            // 288 (Status.FR = 1)
} k64_thread_ctx_t;

typedef enum { K64_THREAD_STOPPED, K64_THREAD_READY, K64_THREAD_RUNNING, K64_THREAD_WAITING } k64_thread_state_t;

typedef struct k64_thread k64_thread_t;
struct k64_thread {
  k64_thread_ctx_t ctx;   // must be first
  k64_thread_t *next;     // in the run queue or a message queue's wait list
  k64_thread_t **queue;   // list it is in (NULL if none)
  s32 pri;
  u32 id;
  k64_thread_state_t state;
};

typedef struct {
  // Field order = libultra's OSMesgQueue (mtqueue, fullqueue, validCount, first, msgCount,
  // msg), so a ported game's queues can be used as k64_mq_t in place.
  k64_thread_t *recv_wait, *send_wait;   // blocked threads, by priority
  u32 valid, first, count;
  k64_msg_t *msgs;
} k64_mq_t;

#define K64_THREAD_PRI_MAX 255
#define K64_PRI_SELF       NULL

// Makes the caller a thread of priority `pri` (and starts an internal idle thread at
// priority -1), then enables interrupts. Call once.
void k64_threads_init(s32 pri);
// `stack_top`: 16-byte aligned end of the thread's stack. The thread returns into a stop.
void k64_thread_create(k64_thread_t *t, u32 id, void (*fn)(void *), void *arg, void *stack_top, s32 pri);
void k64_thread_start(k64_thread_t *t);
void k64_thread_stop(k64_thread_t *t);                // NULL = self
void k64_thread_set_pri(k64_thread_t *t, s32 pri);    // NULL = self
s32  k64_thread_get_pri(k64_thread_t *t);             // NULL = self
k64_thread_t *k64_thread_self(void);
void k64_thread_yield(void);                          // behind the other ready threads of its priority

// FIFO of `count` messages stored in `msgs` (owned by the caller, must outlive the queue).
void k64_mq_init(k64_mq_t *q, k64_msg_t *msgs, u32 count);
// Block = wait for room / a message; otherwise return false at once.
bool k64_mq_send(k64_mq_t *q, k64_msg_t m, bool block);
bool k64_mq_jam(k64_mq_t *q, k64_msg_t m, bool block);   // to the front
bool k64_mq_recv(k64_mq_t *q, k64_msg_t *m, bool block);  // m may be NULL
K64_INLINE u32 k64_mq_valid(const k64_mq_t *q) { return q->valid; }

// Interrupt sources. The MI ones match the MI_INTR bit numbers.
typedef enum {
  K64_EVENT_SP, K64_EVENT_SI, K64_EVENT_AI, K64_EVENT_VI, K64_EVENT_PI, K64_EVENT_DP,
  K64_EVENT_COUNTER,      // Count reached Compare (k64_counter_set)
  K64_EVENT_PRENMI,       // reset button pressed, needs k64_reset_watch (k64/reset.h)
  K64_EVENT_COUNT
} k64_event_t;
// Posts `m` to `q` (non-blocking; dropped if full) each time the source interrupts.
// q = NULL masks the source again. VI interrupts once per field.
void k64_event_set(k64_event_t e, k64_mq_t *q, k64_msg_t m);
// Arms K64_EVENT_COUNTER for when Count reaches `count` (one shot per call).
void k64_counter_set(u32 count);

// Statistics: switches so far (each save/restore round through the kernel).
u32 k64_thread_switches(void);
