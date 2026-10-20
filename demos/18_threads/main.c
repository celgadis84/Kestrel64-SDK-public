// Demo 18 — opt-in threads (k64/thread.h, docs/SCHEDULER.md): self-test of the libultra
// semantics, then a frame loop with a VI-woken thread.
#include <k64/k64.h>
#include <k64/thread.h>

static u32 fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; k64_printf("[18_threads] FAIL " __VA_ARGS__); k64_printf("\n"); } } while (0)

#define STACK(name) static u64 name[512] K64_ALIGN(16)
#define TOP(s) ((void *)((s) + K64_ARRAY_LEN(s)))
#define MSG(i) ((k64_msg_t)(uintptr_t)(i))
#define VAL(m) ((u32)(uintptr_t)(m))

// ---- ping-pong: switch cost -----------------------------------------------------------------
static k64_mq_t ping_q, pong_q;
static k64_msg_t ping_buf[1], pong_buf[1];
static k64_thread_t pong_t;
STACK(pong_stack);
static void pong_fn(void *arg) {
  (void)arg;
  for (;;) { k64_msg_t m; k64_mq_recv(&ping_q, &m, true); k64_mq_send(&pong_q, m, true); }
}

// ---- VI-woken high-priority thread that also trashes FPRs, temporaries and HI/LO ----------
static k64_mq_t vi_q;
static k64_msg_t vi_buf[8];
static k64_thread_t vi_t;
STACK(vi_stack);
static volatile u32 vi_count;

#define F4(a, b, c, d) "dmtc1 %0,$f" #a "\n dmtc1 %0,$f" #b "\n dmtc1 %0,$f" #c "\n dmtc1 %0,$f" #d "\n"
__attribute__((noinline)) static void trash(u32 seed) {
  u64 g = seed * 0x9E3779B97F4A7C15ull;
  __asm__ volatile(
    F4(0, 1, 2, 3) F4(4, 5, 6, 7) F4(8, 9, 10, 11) F4(12, 13, 14, 15)
    F4(16, 17, 18, 19) F4(20, 21, 22, 23) F4(24, 25, 26, 27) F4(28, 29, 30, 31)
    "move $8,%0\n move $9,%0\n move $10,%0\n move $11,%0\n move $12,%0\n move $13,%0\n"
    "move $14,%0\n move $15,%0\n move $24,%0\n move $25,%0\n mthi %0\n mtlo %0\n"
    "move $16,%0\n move $17,%0\n move $18,%0\n move $19,%0\n move $20,%0\n move $21,%0\n"
    "move $22,%0\n move $23,%0\n"
    :: "r"(g) : "$f0","$f1","$f2","$f3","$f4","$f5","$f6","$f7","$f8","$f9","$f10","$f11","$f12",
       "$f13","$f14","$f15","$f16","$f17","$f18","$f19","$f20","$f21","$f22","$f23","$f24","$f25",
       "$f26","$f27","$f28","$f29","$f30","$f31","$8","$9","$10","$11","$12","$13","$14","$15",
       "$24","$25","$16","$17","$18","$19","$20","$21","$22","$23","hi","lo");
}
static void vi_fn(void *arg) {
  (void)arg;
  for (;;) { k64_mq_recv(&vi_q, NULL, true); vi_count++; trash(vi_count); }
}

// Long mixed FP/integer computation: same result however often it is preempted.
__attribute__((noinline)) static u64 crunch(u32 n) {
  double a = 1.0, b = 0.5, c = 0.0;
  u64 h = 1469598103934665603ull;
  for (u32 i = 0; i < n; i++) {
    a = a * 1.0000001 + b * 0.25;
    b = b * 0.9999999 - c * 0.0001;
    c += a * b;
    h = (h ^ i) * 1099511628211ull + (u64)i * i;
    if (c > 1e6) c -= 1e6;
  }
  u64 bits;
  __builtin_memcpy(&bits, &c, 8);
  return h ^ bits;
}

// ---- Count/Compare event --------------------------------------------------------------------
static k64_mq_t ctr_q;
static k64_msg_t ctr_buf[1];
static k64_thread_t ctr_t;
STACK(ctr_stack);
static volatile u32 ctr_woke;
static void ctr_fn(void *arg) {
  (void)arg;
  for (;;) { k64_mq_recv(&ctr_q, NULL, true); ctr_woke = k64_count(); }
}

// ---- blocking send --------------------------------------------------------------------------
static k64_mq_t one_q;
static k64_msg_t one_buf[1];
static k64_thread_t send_t;
STACK(send_stack);
static void send_fn(void *arg) {
  (void)arg;
  for (u32 i = 1; i <= 3; i++) k64_mq_send(&one_q, MSG(i), true);
}

// ---- yield round robin ----------------------------------------------------------------------
static char rr_log[16];
static u32 rr_n;
static k64_thread_t rr_t[2];
STACK(rr_stack0);
STACK(rr_stack1);
static void rr_fn(void *arg) {
  for (int i = 0; i < 3; i++) { rr_log[rr_n++] = (char)(uintptr_t)arg; k64_thread_yield(); }
}

static volatile u32 crunch_n = 2000000;   // volatile: two calls must not fold into one
static u32 per_switch_ns, ctr_latency_us, preempts, crunch_ms;

static void self_test(void) {
  u32 c0 = k64_count();
  u64 expect = crunch(crunch_n);
  crunch_ms = K64_US(k64_count() - c0) / 1000;

  k64_threads_init(10);
  k64_thread_t *self = k64_thread_self();

  // Ping-pong with a higher-priority thread: each round trip = two switches.
  k64_mq_init(&ping_q, ping_buf, 1);
  k64_mq_init(&pong_q, pong_buf, 1);
  k64_thread_create(&pong_t, 2, pong_fn, NULL, TOP(pong_stack), 11);
  k64_thread_start(&pong_t);
  u32 s0 = k64_thread_switches(), t0 = k64_count();
  for (u32 i = 0; i < 1000; i++) {
    k64_mq_send(&ping_q, MSG(i), true);
    k64_msg_t m = NULL;
    k64_mq_recv(&pong_q, &m, true);
    if (VAL(m) != i) { CHECK(0, "ping-pong %u got %u", i, VAL(m)); break; }
  }
  u32 dt = k64_count() - t0, ds = k64_thread_switches() - s0;
  CHECK(ds == 2000, "ping-pong switches %u", ds);
  per_switch_ns = (u32)((u64)dt * 1000000000u / K64_COUNT_HZ / (ds ? ds : 1));
  k64_thread_stop(&pong_t);

  // VI preemption while main computes, with FPR/GPR trashing in the preempting thread.
  k64_mq_init(&vi_q, vi_buf, 8);
  k64_thread_create(&vi_t, 3, vi_fn, NULL, TOP(vi_stack), 20);
  k64_thread_start(&vi_t);
  k64_event_set(K64_EVENT_VI, &vi_q, NULL);
  u32 v0 = vi_count;
  u64 got = crunch(crunch_n);
  preempts = vi_count - v0;
  CHECK(got == expect, "context: crunch %llx != %llx", got, expect);
  CHECK(preempts >= 3, "VI preemption: %u wakes", preempts);
  v0 = vi_count;
  k64_wait_us(100000);
  CHECK(vi_count - v0 >= 5 && vi_count - v0 <= 7, "VI preemption in 100 ms: %u", vi_count - v0);

  // Priority as a mutex: above the VI thread it cannot run; dropping back lets it run at once.
  k64_thread_set_pri(NULL, 21);
  v0 = vi_count;
  k64_wait_us(50000);
  u32 during = vi_count - v0;
  k64_thread_set_pri(NULL, 10);
  u32 after = vi_count - v0;
  CHECK(during == 0 && after >= 2 && after <= 4, "pri mutex: during %u after %u", during, after);

  // Count/Compare event: wake-up latency of a thread.
  k64_mq_init(&ctr_q, ctr_buf, 1);
  k64_thread_create(&ctr_t, 4, ctr_fn, NULL, TOP(ctr_stack), 25);
  k64_thread_start(&ctr_t);
  k64_event_set(K64_EVENT_COUNTER, &ctr_q, NULL);
  ctr_woke = 0;
  u32 target = k64_count() + K64_TICKS_PER_US * 2000;
  k64_counter_set(target);
  k64_wait_us(3000);
  CHECK(ctr_woke != 0, "counter event did not fire");
  ctr_latency_us = (u32)(ctr_woke - target) / K64_TICKS_PER_US;
  CHECK(ctr_woke && (s32)(ctr_woke - target) >= 0 && ctr_latency_us < 50, "counter latency %u us", ctr_latency_us);

  // Blocking send into a 1-slot queue from a lower-priority thread.
  k64_mq_init(&one_q, one_buf, 1);
  k64_thread_create(&send_t, 5, send_fn, NULL, TOP(send_stack), 5);
  k64_thread_start(&send_t);                  // lower priority: does not run yet
  CHECK(k64_mq_valid(&one_q) == 0, "send ran early");
  k64_msg_t m[3] = {0};
  for (int i = 0; i < 3; i++) k64_mq_recv(&one_q, &m[i], true);   // main blocks, sender refills
  CHECK(VAL(m[0]) == 1 && VAL(m[1]) == 2 && VAL(m[2]) == 3, "blocking send order %u %u %u",
        VAL(m[0]), VAL(m[1]), VAL(m[2]));

  // Jam goes to the front; a full queue refuses a non-blocking send.
  k64_msg_t two[2];
  k64_mq_t jq;
  k64_mq_init(&jq, two, 2);
  k64_mq_send(&jq, MSG(7), false);
  k64_mq_jam(&jq, MSG(9), false);
  CHECK(!k64_mq_send(&jq, MSG(1), false), "full queue accepted");
  k64_mq_recv(&jq, &m[0], false);
  k64_mq_recv(&jq, &m[1], false);
  CHECK(VAL(m[0]) == 9 && VAL(m[1]) == 7 && !k64_mq_recv(&jq, NULL, false), "jam order");

  // Yield among equal priorities: round robin, main included.
  k64_thread_create(&rr_t[0], 6, rr_fn, (void *)'A', TOP(rr_stack0), 10);
  k64_thread_create(&rr_t[1], 7, rr_fn, (void *)'B', TOP(rr_stack1), 10);
  k64_thread_start(&rr_t[0]);
  k64_thread_start(&rr_t[1]);
  rr_n = 0;
  while (rr_n < 6) { rr_log[rr_n++] = 'm'; k64_thread_yield(); }
  rr_log[rr_n] = 0;
  CHECK(__builtin_strcmp(rr_log, "mABmAB") == 0, "round robin %s", rr_log);
  CHECK(k64_thread_self() == self, "self");
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  self_test();
  k64_printf("[18_threads] self-test %s (%u failures): switch %u ns, %u VI preemptions in crunch "
             "(%u ms), counter latency %u us, rr %s\n", fails ? "FAIL" : "PASS", fails, per_switch_ns,
             preempts, crunch_ms, ctr_latency_us, rr_log);

  for (u32 frame = 0;; frame++) {
    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, false);
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  18 threads");
    k64_text_printf(8, 24, fails ? 0xFF4040FF : 0x40FF40FF, "self-test %s", fails ? "FAIL" : "PASS");
    k64_text_printf(8, 48, 0xC0C0C0FF, "context switch  %u ns", per_switch_ns);
    k64_text_printf(8, 60, 0xC0C0C0FF, "counter wake    %u us late", ctr_latency_us);
    k64_text_printf(8, 72, 0xC0C0C0FF, "switches        %u", k64_thread_switches());
    k64_text_printf(8, 84, 0xFFFF80FF, "VI thread wakes %u", vi_count);
    k64_rdp_wait_idle();
    k64_display_show(fb);
    if (frame % 300 == 299)
      k64_printf("[18_threads] frame %u vi %u switches %u\n", frame + 1, vi_count, k64_thread_switches());
  }
}
