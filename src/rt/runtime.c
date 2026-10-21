// kestrel64-sdk — runtime init + crash screen.
#include <k64/debug.h>
#include <k64/cpu.h>
#include <k64/mem.h>
#include <k64/vi.h>

extern const u8 k64_vector_stub[], k64_vector_stub_end[];

k64_exc_frame_t k64_exc_frame K64_ALIGN(16);
u64 k64_crash_stack[512] K64_ALIGN(16);   // crt0 switches to its top on exceptions

void k64_runtime_init(void) {
  // Install the exception vectors (TLB refill 0x000, XTLB 0x080, cache error 0x100,
  // general 0x180). Write through KSEG0, then make I$ coherent with what we wrote.
  static const u32 vec[] = { 0x80000000, 0x80000080, 0x80000100, 0x80000180 };
  u32 n = (u32)(k64_vector_stub_end - k64_vector_stub);
  for (u32 i = 0; i < K64_ARRAY_LEN(vec); i++) {
    u32 *dst = (u32*)vec[i]; const u32 *src = (const u32*)k64_vector_stub;
    for (u32 w = 0; w < n / 4; w++) dst[w] = src[w];
    // The stub was written through the D$: write it back so the I$ refill reads it from RDRAM.
    k64_dcache_writeback((void*)vec[i], n);
    k64_icache_invalidate((void*)vec[i], n);
  }
  // Mask every RCP interrupt line; the core runtime polls.
  MI_MASK = MI_MASK_CLR(0) | MI_MASK_CLR(1) | MI_MASK_CLR(2) | MI_MASK_CLR(3) | MI_MASK_CLR(4) | MI_MASK_CLR(5);
  // Tell the PIF boot is over (bit 3 of the PIF RAM control byte). Without it the PIF
  // halts the CPU ~5 s after power-on; libultra does the same in osInitialize.
  while (SI_STATUS & (SI_ST_DMA_BUSY | SI_ST_IO_BUSY)) {}
  // PIF RAM is reached through SI: wait for it to be idle around each access.
  u32 ctl = K64_REG(K64_KSEG1(PIF_RAM_PHYS + 0x3C));
  while (SI_STATUS & (SI_ST_DMA_BUSY | SI_ST_IO_BUSY)) {}
  K64_REG(K64_KSEG1(PIF_RAM_PHYS + 0x3C)) = ctl | 0x08;
  while (SI_STATUS & (SI_ST_DMA_BUSY | SI_ST_IO_BUSY)) {}
  k64_mem_init();
}
