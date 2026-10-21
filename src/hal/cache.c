// kestrel64-sdk — cache maintenance. Hit ops touch only lines in range; when the range is
// larger than the cache, index ops over the whole cache are cheaper.
#include <k64/cpu.h>

// Applies a hit cache op to every line overlapping [p, p + len). The start is rounded down to
// a line, so a partially covered first or last line is included.
#define LINE_LOOP(op, p, len, line)                                         \
  do {                                                                      \
    u32 a_ = (u32)(p) & ~((line) - 1);                                      \
    u32 e_ = (u32)(p) + (len);                                              \
    for (; a_ < e_; a_ += (line)) K64_CACHE_OP(op, a_);                     \
  } while (0)

void k64_dcache_writeback(const void *p, u32 len) {
  if (len >= K64_DCACHE_SIZE) { k64_dcache_writeback_all(); return; }
  LINE_LOOP(K64_D_HIT_WRITEBACK, p, len, K64_DCACHE_LINE);
}

// No whole-cache shortcut here: an index invalidate would also discard dirty lines outside
// the range, losing CPU writes that never reached RDRAM.
void k64_dcache_invalidate(void *p, u32 len) {
  LINE_LOOP(K64_D_HIT_INVALIDATE, p, len, K64_DCACHE_LINE);
}

void k64_dcache_writeback_invalidate(const void *p, u32 len) {
  if (len >= K64_DCACHE_SIZE) { k64_dcache_writeback_all(); return; }
  LINE_LOOP(K64_D_HIT_WB_INV, p, len, K64_DCACHE_LINE);
}

// Index ops address the cache by line index; any KSEG0 address with the right index bits works,
// so walking 0x80000000 up to the cache size covers every line once.
void k64_dcache_writeback_all(void) {
  for (u32 a = 0x80000000; a < 0x80000000 + K64_DCACHE_SIZE; a += K64_DCACHE_LINE)
    K64_CACHE_OP(K64_D_INDEX_WB_INV, a);
}

void k64_icache_invalidate(const void *p, u32 len) {
  if (len >= K64_ICACHE_SIZE) { k64_icache_invalidate_all(); return; }
  LINE_LOOP(K64_I_HIT_INVALIDATE, p, len, K64_ICACHE_LINE);
}

void k64_icache_invalidate_all(void) {
  for (u32 a = 0x80000000; a < 0x80000000 + K64_ICACHE_SIZE; a += K64_ICACHE_LINE)
    K64_CACHE_OP(K64_I_INDEX_INVALIDATE, a);
}
