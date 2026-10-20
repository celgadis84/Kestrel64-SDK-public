/* kestrel64-sdk: the few libdragon calls libcart makes, over the SDK's PI and cache code.
   Replaces libdragon's n64types.h / n64sys.h / dma.h includes in cart.c. */
#pragma once
#include <stdint.h>
#include <k64/pi.h>
#include <k64/cpu.h>
#include <stddef.h>

int memcmp(const void *a, const void *b, size_t n);   /* src/rt/string.c */

typedef uint64_t u_uint64_t __attribute__((aligned(1)));

static inline uint32_t io_read(uint32_t pi) { k64_pi_wait(); return K64_REG(0xA0000000u | pi); }
static inline void io_write(uint32_t pi, uint32_t v) { k64_pi_wait(); K64_REG(0xA0000000u | pi) = v; }

static inline void data_cache_hit_writeback(void *p, unsigned long n) { k64_dcache_writeback(p, n); }
static inline void data_cache_hit_writeback_invalidate(void *p, unsigned long n) { k64_dcache_writeback_invalidate(p, n); }

/* cart -> RDRAM */
static inline void dma_read_raw_async(void *dram, uint32_t pi, unsigned long len) {
  k64_pi_wait();
  PI_DRAM_ADDR = K64_PHYS(dram);
  PI_CART_ADDR = K64_PHYS(pi);
  PI_WR_LEN = len - 1;
}
/* RDRAM -> cart */
static inline void dma_write_raw_async(const void *dram, uint32_t pi, unsigned long len) {
  k64_pi_wait();
  PI_DRAM_ADDR = K64_PHYS(dram);
  PI_CART_ADDR = K64_PHYS(pi);
  PI_RD_LEN = len - 1;
}
static inline void dma_wait(void) { k64_pi_wait(); PI_STATUS = PI_WR_CLR_INTR; }
