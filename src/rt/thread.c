// kestrel64-sdk — threads with libultra semantics (see include/k64/thread.h).
//
// Every switch goes through the exception path in crt0.S: interrupts arrive there, and a
// thread that has to reschedule (it blocks, wakes a higher-priority thread, yields...)
// enters it with `syscall`. k64_thread_dispatch runs with interrupts off (EXL set) on the
// kernel stack and returns the thread to resume. Thread-side code edits the queues with
// interrupts disabled.
#include <k64/thread.h>
#include <k64/cpu.h>
#include <k64/debug.h>

// reset.c: Count at the reset button press.
extern volatile u32 k64_reset_count;

// vi.c: interlaced modes need the VI interrupt every field.
extern bool k64_vi_interlaced;
void k64_vi_field_irq(void);

#define SR_IE       0x0001u
#define SR_EXL      0x0002u
#define SR_IM_RCP   0x0400u     // IP2: RCP (MI) interrupt line
#define SR_IM_PRENMI 0x1000u    // IP4: reset button
#define SR_IM_TIMER 0x8000u     // IP7: Count == Compare
#define SR_BASE     0x24000000u // CU1 + FR, as crt0 sets it

k64_thread_t *k64_thread_cur;          // read by crt0.S
u32 k64_thread_im;                     // Status IM bits every thread runs with (crt0.S)
u64 k64_thread_kstack[256] K64_ALIGN(16);

void k64_thread_syscall(void);         // crt0.S

static k64_thread_t *runq;
static k64_thread_t main_thread, idle_thread;
static u64 idle_stack[64] K64_ALIGN(16);
static struct { k64_mq_t *q; k64_msg_t m; } events[K64_EVENT_COUNT];
static bool yield_req;
static u32 switches;

// ---- queues: sorted by priority, FIFO among equals ----------------------------------------
// Inserts t after every thread of equal or higher priority (so equals stay FIFO) and records
// which list holds it, for q_remove.
static void q_insert(k64_thread_t **head, k64_thread_t *t) {
  k64_thread_t **p = head;
  while (*p && (*p)->pri >= t->pri) p = &(*p)->next;
  t->next = *p;
  *p = t;
  t->queue = head;
}

// Unlinks t from whatever list holds it; no-op if it is on none (running or stopped).
static void q_remove(k64_thread_t *t) {
  if (!t->queue) return;
  for (k64_thread_t **p = t->queue; *p; p = &(*p)->next)
    if (*p == t) { *p = t->next; break; }
  t->next = NULL;
  t->queue = NULL;
}

// Takes the head (highest priority) off a non-empty list.
static k64_thread_t *q_pop(k64_thread_t **head) {
  k64_thread_t *t = *head;
  *head = t->next;
  t->next = NULL;
  t->queue = NULL;
  return t;
}

static void make_ready(k64_thread_t *t) {
  t->state = K64_THREAD_READY;
  q_insert(&runq, t);
}

// Thread side, interrupts off: switch if a higher-priority thread is ready.
static void resched(void) {
  if (runq && runq->pri > k64_thread_cur->pri) k64_thread_syscall();
}

// Thread side, interrupts off: parks the current thread on a wait list and switches away.
// Returns once someone moved it back to the run queue; callers re-check their condition.
static void block_on(k64_thread_t **list) {
  k64_thread_cur->state = K64_THREAD_WAITING;
  q_insert(list, k64_thread_cur);
  k64_thread_syscall();
}

// ---- message queues ---------------------------------------------------------------------------
void k64_mq_init(k64_mq_t *q, k64_msg_t *msgs, u32 count) {
  *q = (k64_mq_t){ .msgs = msgs, .count = count };
}

// Puts a message and wakes the first receiver. Interrupts off; caller has checked room.
static void mq_put(k64_mq_t *q, k64_msg_t m, bool jam) {
  if (jam) {                              // jam: in front of the oldest message
    q->first = (q->first ? q->first : q->count) - 1;
    q->msgs[q->first] = m;
  } else {
    u32 i = q->first + q->valid;
    q->msgs[i >= q->count ? i - q->count : i] = m;
  }
  q->valid++;
  if (q->recv_wait) make_ready(q_pop(&q->recv_wait));
}

// Send/jam body: waits for room when blocking (re-checked on every wake, another sender may
// have taken it), then hands the message over and switches if that woke a higher priority.
static bool mq_send(k64_mq_t *q, k64_msg_t m, bool block, bool jam) {
  u32 ie = k64_irq_disable();
  while (q->valid >= q->count) {
    if (!block) { k64_irq_restore(ie); return false; }
    block_on(&q->send_wait);
  }
  mq_put(q, m, jam);
  resched();
  k64_irq_restore(ie);
  return true;
}

bool k64_mq_send(k64_mq_t *q, k64_msg_t m, bool block) { return mq_send(q, m, block, false); }
bool k64_mq_jam(k64_mq_t *q, k64_msg_t m, bool block)  { return mq_send(q, m, block, true); }

bool k64_mq_recv(k64_mq_t *q, k64_msg_t *m, bool block) {
  u32 ie = k64_irq_disable();
  while (q->valid == 0) {
    if (!block) { k64_irq_restore(ie); return false; }
    block_on(&q->recv_wait);
  }
  if (m) *m = q->msgs[q->first];
  if (++q->first == q->count) q->first = 0;
  q->valid--;
  if (q->send_wait) make_ready(q_pop(&q->send_wait));
  resched();
  k64_irq_restore(ie);
  return true;
}

// ---- threads ----------------------------------------------------------------------------------
// First code of every created thread (its EPC): a thread whose function returns stops itself.
static void thread_entry(void (*fn)(void *), void *arg) {
  fn(arg);
  k64_thread_stop(NULL);
}

// Priority -1, always ready, so the run queue is never empty when everything else waits.
static void idle_fn(void *arg) { (void)arg; for (;;) {} }

// Context registers are 64 bits: a 32-bit KSEG0 address must be sign-extended, or a 64-bit
// restore would load a non-canonical address.
#define SEXT(p) ((u64)(s64)(s32)(u32)(p))

void k64_thread_create(k64_thread_t *t, u32 id, void (*fn)(void *), void *arg, void *stack_top, s32 pri) {
  *t = (k64_thread_t){ .pri = pri, .id = id, .state = K64_THREAD_STOPPED };
  u32 gp;
  __asm__("move %0, $gp" : "=r"(gp));
  t->ctx.gpr[4] = SEXT(fn);                        // a0, a1: thread_entry's arguments
  t->ctx.gpr[5] = SEXT(arg);
  t->ctx.gpr[28] = SEXT(gp);
  t->ctx.gpr[29] = SEXT((u32)stack_top - 16);     // sp, 16 bytes of argument home space
  t->ctx.epc = (u32)thread_entry;
  t->ctx.sr = SR_BASE | SR_EXL | SR_IE;            // partial = 0: full restore
  u32 fcsr;
  __asm__ volatile("cfc1 %0, $31" : "=r"(fcsr));
  t->ctx.fcsr = fcsr & ~0x0003F000u;     // no pending FP exception causes
}

void k64_thread_start(k64_thread_t *t) {
  u32 ie = k64_irq_disable();
  if (t->state == K64_THREAD_STOPPED) {
    make_ready(t);
    resched();
  }
  k64_irq_restore(ie);
}

void k64_thread_stop(k64_thread_t *t) {
  u32 ie = k64_irq_disable();
  if (!t) t = k64_thread_cur;
  if (t == k64_thread_cur) {
    t->state = K64_THREAD_STOPPED;
    k64_thread_syscall();                 // returns when started again
  } else {
    q_remove(t);
    t->state = K64_THREAD_STOPPED;
  }
  k64_irq_restore(ie);
}

void k64_thread_set_pri(k64_thread_t *t, s32 pri) {
  u32 ie = k64_irq_disable();
  if (!t) t = k64_thread_cur;
  if (t->pri != pri) {
    t->pri = pri;
    if (t->queue) {                       // keep its list sorted
      k64_thread_t **head = t->queue;
      q_remove(t);
      q_insert(head, t);
    }
    resched();
  }
  k64_irq_restore(ie);
}

s32 k64_thread_get_pri(k64_thread_t *t) { return (t ? t : k64_thread_cur)->pri; }
k64_thread_t *k64_thread_self(void) { return k64_thread_cur; }

void k64_thread_yield(void) {
  u32 ie = k64_irq_disable();
  if (runq && runq->pri >= k64_thread_cur->pri) {
    yield_req = true;
    k64_thread_syscall();
  }
  k64_irq_restore(ie);
}

u32 k64_thread_switches(void) { return switches; }

// ---- events -----------------------------------------------------------------------------------
// Applies k64_thread_im to the live Status register (the IM field, bits 15..8).
static void status_im(void) {
  k64_set_status((k64_status() & ~0xFF00u) | k64_thread_im);
}

void k64_event_set(k64_event_t e, k64_mq_t *q, k64_msg_t m) {
  u32 ie = k64_irq_disable();
  events[e].q = q;
  events[e].m = m;
  if (e < K64_EVENT_COUNTER) {
    if (e == K64_EVENT_VI && q) VI_V_INTR = 2;
    // Interlaced video keeps the VI interrupt on for the per-field origin (vi.c).
    MI_MASK = q || (e == K64_EVENT_VI && k64_vi_interlaced) ? MI_MASK_SET(e) : MI_MASK_CLR(e);
  } else if (e == K64_EVENT_COUNTER && !q) {
    k64_thread_im &= ~SR_IM_TIMER;
    status_im();
  }
  k64_irq_restore(ie);
}

void k64_counter_set(u32 count) {
  u32 ie = k64_irq_disable();
  K64_C0_WRITE(11, count);                // also clears a pending Compare interrupt
  if (events[K64_EVENT_COUNTER].q) {
    k64_thread_im |= SR_IM_TIMER;
    status_im();
  }
  k64_irq_restore(ie);
}

// Interrupt side: delivers an event's message, dropped silently when its queue is full
// (libultra behaviour: an event is a notification, not a counted resource).
static void post(k64_event_t e) {
  k64_mq_t *q = events[e].q;
  if (q && q->valid < q->count) mq_put(q, events[e].m, false);
}

// Interrupt side: acknowledges every pending, enabled source at its device (each one keeps
// its line asserted until acked) and posts its event.
static void handle_irq(u32 cause) {
  u32 ip = cause & k64_thread_im;
  if (ip & SR_IM_RCP) {
    u32 pend = MI_INTR & MI_MASK;
    if (pend & MI_INTR_SP) { SP_STATUS = SP_WR_CLR_INTR; post(K64_EVENT_SP); }
    if (pend & MI_INTR_SI) { SI_STATUS = 0;              post(K64_EVENT_SI); }
    if (pend & MI_INTR_AI) { AI_STATUS = 0;              post(K64_EVENT_AI); }
    if (pend & MI_INTR_VI) {
      if (k64_vi_interlaced) k64_vi_field_irq();
      VI_V_CURRENT = 0;                   // any write acks the VI interrupt
      post(K64_EVENT_VI);
    }
    if (pend & MI_INTR_PI) { PI_STATUS = PI_WR_CLR_INTR; post(K64_EVENT_PI); }
    if (pend & MI_INTR_DP) { MI_MODE = MI_MODE_CLR_DP;   post(K64_EVENT_DP); }
  }
  if (ip & SR_IM_PRENMI) {
    k64_reset_count = k64_count() | 1;
    k64_thread_im &= ~SR_IM_PRENMI;       // stays asserted until the NMI
    post(K64_EVENT_PRENMI);
  }
  if (ip & SR_IM_TIMER) {
    K64_C0_WRITE(11, K64_C0_READ(11));    // ack
    k64_thread_im &= ~SR_IM_TIMER;        // one shot
    post(K64_EVENT_COUNTER);
  }
}

// Called by crt0.S with the current thread's context saved. Returns the thread to resume.
k64_thread_t *k64_thread_dispatch(u32 cause) {
  k64_thread_t *cur = k64_thread_cur;
  if (((cause >> 2) & 31) != 8) handle_irq(cause);  // syscall: crt0 already set EPC + 4
  if (cur->state == K64_THREAD_RUNNING) {
    if (!yield_req && !(runq && runq->pri > cur->pri)) return cur;
    make_ready(cur);
  }
  yield_req = false;
  k64_thread_t *n = q_pop(&runq);                    // never empty: idle is always ready
  n->state = K64_THREAD_RUNNING;
  if (n != cur) switches++;
  k64_thread_cur = n;
  return n;
}

void k64_threads_init(s32 pri) {
  main_thread = (k64_thread_t){ .pri = pri, .id = 1, .state = K64_THREAD_RUNNING };
  k64_thread_cur = &main_thread;
  k64_thread_create(&idle_thread, 0, idle_fn, NULL, idle_stack + K64_ARRAY_LEN(idle_stack), -1);
  make_ready(&idle_thread);
  k64_thread_im = SR_IM_RCP | (k64_status() & SR_IM_PRENMI);   // keep k64_reset_watch
  k64_set_status((k64_status() & ~0xFF00u) | k64_thread_im | SR_IE);
}
