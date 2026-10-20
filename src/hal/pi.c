// kestrel64-sdk — PI DMA.
#include <k64/pi.h>
#include <k64/cpu.h>
#include <k64/debug.h>

void k64_pi_read_async(void *dst, u32 rom_addr, u32 len) {
  K64_ASSERT(((u32)dst & 7) == 0 && (rom_addr & 1) == 0 && len && (len & 1) == 0);
  k64_pi_wait();
  PI_DRAM_ADDR = K64_PHYS(dst);
  PI_CART_ADDR = K64_PHYS(rom_addr);
  PI_WR_LEN = len - 1;
}

void k64_pi_read(void *dst, u32 rom_addr, u32 len) {
  // Invalidate first: a dirty line written back during the DMA would clobber fresh data.
  // Partial lines at the edges are written back (they may hold unrelated live data).
  u32 a = (u32)dst, e = a + len;
  if (a & 15) K64_CACHE_OP(K64_D_HIT_WB_INV, a & ~15u);
  if (e & 15) K64_CACHE_OP(K64_D_HIT_WB_INV, e & ~15u);
  k64_dcache_invalidate(dst, len);
  k64_pi_read_async(dst, rom_addr, len);
  k64_pi_wait();
  PI_STATUS = PI_WR_CLR_INTR;
}
