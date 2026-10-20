// Demo 17 — time base and polled deadline timers: a self-test (Count wrap, one-shot,
// periodic phase, overruns, ordering, callbacks that stop/start timers), then three
// periodic timers driving on-screen counters from the main loop.
#include <k64/k64.h>

static u32 fails;
#define CHECK(c, ...)                              \
  do {                                             \
    if (!(c)) {                                    \
      fails++;                                     \
      k64_printf("[17_timers] FAIL " __VA_ARGS__); \
      k64_printf("\n");                            \
    }                                              \
  } while (0)

static void count_cb(k64_timer_t *t, void *arg) { (void)t; (*(u32 *)arg)++; }

static u32 order[4], norder;
static void order_cb(k64_timer_t *t, void *arg) { (void)t; order[norder++] = (u32)(uintptr_t)arg; }

static k64_timer_t chained;
static u32 chained_hits;
static void chained_cb(k64_timer_t *t, void *arg) { (void)t; (void)arg; chained_hits++; }
static void self_stop_cb(k64_timer_t *t, void *arg) {
  (*(u32 *)arg)++;
  k64_timer_stop(t);                                        // periodic timer stops itself
  k64_timer_start(&chained, 0, 0, chained_cb, NULL);        // and arms another, due now
}

static void poll_until(u64 ticks) {
  u64 end = k64_time() + ticks;
  while (k64_time() < end) k64_timer_poll();
}

static void self_test(void) {
  // Count wrap: move Count just below 2^32 and cross it.
  u64 before = k64_time();
  K64_C0_WRITE(9, 0xFFFFFFFFu - K64_TICKS_US(200));
  u64 a = k64_time();                                       // Count went "back": counted as a wrap
  k64_wait_us(400);
  u64 b = k64_time();
  CHECK(a > before && b > a && (b >> 32) == (a >> 32) + 1, "wrap a=%llx b=%llx", a, b);
  CHECK(b - a >= K64_TICKS_US(390) && b - a < K64_TICKS_US(600), "wrap delta %llu", b - a);

  // One-shot: not before its deadline, exactly once after.
  u32 n = 0;
  k64_timer_t once = {0};
  k64_timer_start(&once, K64_TICKS_MS(5), 0, count_cb, &n);
  poll_until(K64_TICKS_MS(4));
  CHECK(n == 0 && once.active, "one-shot early n=%u", n);
  poll_until(K64_TICKS_MS(3));
  CHECK(n == 1 && !once.active, "one-shot n=%u", n);

  // Periodic: 1 ms for 20.5 ms = 20 calls, phase locked to the start (no drift).
  n = 0;
  k64_timer_t per = {0};
  u64 t0 = k64_time();
  k64_timer_start(&per, K64_TICKS_MS(1), K64_TICKS_MS(1), count_cb, &n);
  u64 first = per.deadline;
  poll_until(K64_TICKS_US(20500));
  CHECK(n == 20, "periodic n=%u", n);
  CHECK(per.deadline == first + 20 * K64_TICKS_MS(1) && first - t0 - K64_TICKS_MS(1) < K64_TICKS_US(50),
        "periodic phase");
  CHECK(per.overruns == 0, "periodic overruns %u", per.overruns);

  // Overrun: stall 5.5 periods, one call, 5 skipped, phase kept.
  u64 d = per.deadline;
  k64_wait_us(5500);
  u32 ran = k64_timer_poll();
  CHECK(ran == 1 && n == 21 && per.overruns == 5, "overrun ran=%u n=%u ov=%u", ran, n, per.overruns);
  CHECK(per.deadline == d + 6 * K64_TICKS_MS(1), "overrun phase");
  k64_timer_stop(&per);
  CHECK(!per.active && k64_timer_next() == ~0ull, "stop");

  // Order: by deadline, FIFO among equal deadlines; restart moves a timer.
  k64_timer_t o[4] = {0};
  norder = 0;
  k64_timer_start(&o[0], K64_TICKS_US(300), 0, order_cb, (void *)0);
  k64_timer_start(&o[1], K64_TICKS_US(100), 0, order_cb, (void *)1);
  k64_timer_start(&o[2], K64_TICKS_US(300), 0, order_cb, (void *)2);
  k64_timer_start(&o[3], K64_TICKS_US(200), 0, order_cb, (void *)3);
  o[2].deadline = o[0].deadline;                            // force an exact tie
  k64_timer_stop(&o[2]);
  k64_timer_start(&o[2], 0, 0, order_cb, (void *)2);        // restarted: now first
  k64_wait_us(400);
  k64_timer_poll();
  CHECK(norder == 4 && order[0] == 2 && order[1] == 1 && order[2] == 3 && order[3] == 0,
        "order %u: %u %u %u %u", norder, order[0], order[1], order[2], order[3]);

  // A callback stops its own periodic timer and arms another one due "now": that one is
  // past the poll's time snapshot, so it runs on the next poll.
  n = 0;
  k64_timer_t ss = {0};
  k64_timer_start(&ss, 0, K64_TICKS_MS(1), self_stop_cb, &n);
  ran = k64_timer_poll();
  CHECK(ran == 1 && n == 1 && chained_hits == 0 && !ss.active && chained.active,
        "callback stop/start ran=%u n=%u ch=%u", ran, n, chained_hits);
  ran = k64_timer_poll();
  CHECK(ran == 1 && chained_hits == 1 && !chained.active, "chained ran=%u ch=%u", ran, chained_hits);
}

int main(void) {
  k64_display_init(&(k64_display_cfg_t){ .width = 320, .height = 240, .bpp = K64_BPP16, .buffers = 2 });
  k64_rdp_init();
  k64_text_init();

  self_test();
  k64_printf("[17_timers] self-test %s (%u failures)\n", fails ? "FAIL" : "PASS", fails);

  static const u32 period_ms[3] = { 10, 250, 1000 };
  u32 hits[3] = {0};
  k64_timer_t t[3] = {0};
  for (int i = 0; i < 3; i++)
    k64_timer_start(&t[i], K64_TICKS_MS(period_ms[i]), K64_TICKS_MS(period_ms[i]), count_cb, &hits[i]);
  u64 start = k64_time();

  for (u32 frame = 0;; frame++) {
    k64_timer_poll();
    u64 ms = k64_ticks_to_us(k64_time() - start) / 1000;
    if (frame % 300 == 299)
      k64_printf("[17_timers] %llu ms: 10ms %u (ov %u) 250ms %u 1s %u\n", ms, hits[0], t[0].overruns, hits[1], hits[2]);

    const k64_surface_t *fb = k64_display_acquire();
    k64_rdp_attach(K64_FMT_RGBA, K64_SIZ_16, fb->width, fb->height, fb->pixels, NULL);
    k64_rdp_clear(k64_rgba16(16, 20, 40, 255) * 0x10001u, false);
    k64_text_printf(8, 8, 0xFFFFFFFF, "kestrel64-sdk  17 timers");
    k64_text_printf(8, 24, fails ? 0xFF4040FF : 0x40FF40FF, "self-test %s", fails ? "FAIL" : "PASS");
    k64_text_printf(8, 48, 0xFFFF80FF, "uptime   %llu.%03llu s", ms / 1000, ms % 1000);
    k64_text_printf(8, 64, 0xC0C0C0FF, "10 ms    %u  (skipped %u)", hits[0], t[0].overruns);
    k64_text_printf(8, 76, 0xC0C0C0FF, "250 ms   %u", hits[1]);
    k64_text_printf(8, 88, 0xC0C0C0FF, "1 s      %u", hits[2]);
    k64_text_printf(8, 112, 0x8080A0FF, "10 ms timer polled once per frame:");
    k64_text_printf(8, 124, 0x8080A0FF, "it skips periods, never drifts");
    k64_rdp_wait_idle();
    k64_display_show(fb);
  }
}
