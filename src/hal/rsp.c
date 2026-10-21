// kestrel64-sdk — RSP control.
#include <k64/rsp.h>
#include <k64/cpu.h>
#include <k64/debug.h>

const u8 *k64_rsp_loaded;

// The transfer and its one-deep request queue must both be idle before the next DMA is set up.
static void dma_wait(void) { while (SP_STATUS & (SP_ST_DMA_BUSY | SP_ST_DMA_FULL)) {} }

// Hardware limits: 8-byte alignment on both sides, and no transfer may cross a 4 KB DMEM/IMEM edge.
static void dma_check(u32 sp_addr, const void *ram, u32 len) {
  K64_ASSERT(((u32)ram & 7) == 0 && (sp_addr & 7) == 0 && len && len <= 4096);
  K64_ASSERT((sp_addr & 0xFFF) + len <= 4096);
}

void k64_rsp_dma_to(u32 sp_addr, const void *src, u32 len) {
  len = (len + 7) & ~7u;
  dma_check(sp_addr, src, len);
  // Write back first: the DMA reads RDRAM directly.
  k64_dcache_writeback(src, len);
  dma_wait();
  // Length registers take bytes - 1. SP_RD_LEN: RDRAM -> SP. SP_MEM_ADDR bit 12 selects IMEM.
  SP_MEM_ADDR = sp_addr & 0x1FFF;
  SP_DRAM_ADDR = K64_PHYS(src);
  SP_RD_LEN = len - 1;
  dma_wait();
}

void k64_rsp_dma_from(void *dst, u32 sp_addr, u32 len) {
  len = (len + 7) & ~7u;
  dma_check(sp_addr, dst, len);
  // Writeback as well as invalidate: the first and last lines may hold dirty bytes outside the range.
  k64_dcache_writeback_invalidate(dst, len);
  dma_wait();
  SP_MEM_ADDR = sp_addr & 0x1FFF;
  SP_DRAM_ADDR = K64_PHYS(dst);
  SP_WR_LEN = len - 1;
  dma_wait();
  // Drop what the D$ may have fetched while the DMA was in flight.
  k64_dcache_invalidate(dst, len);
}

void k64_rsp_load(const k64_ucode_t *u) {
  // Halt before touching SP memory.
  SP_STATUS = SP_WR_SET_HALT;
  while (!(SP_STATUS & SP_ST_HALT)) {}
  u32 tl = (u32)(u->text_end - u->text), dl = (u32)(u->data_end - u->data);
  K64_ASSERT(tl <= K64_IMEM_SIZE && dl <= K64_DMEM_SIZE);
  // IMEM is SP address 0x1000 (bit 12); DMEM is 0.
  if (tl) k64_rsp_dma_to(0x1000, u->text, tl);
  if (dl) k64_rsp_dma_to(0x0000, u->data, dl);
  k64_rsp_loaded = u->text;
}

// Sets the start PC (IMEM offset, 12 bits), clears break/sstep/signals, and selects whether the
// final break raises the SP interrupt. Clearing halt last is what starts execution.
static void run(u32 pc, u32 intr_break) {
  SP_PC = pc & 0xFFC;
  SP_STATUS = SP_WR_CLR_BROKE | SP_WR_CLR_INTR | SP_WR_CLR_SSTEP | intr_break |
              SP_WR_CLR_SIG(0) | SP_WR_CLR_SIG(1) | SP_WR_CLR_SIG(2) | SP_WR_CLR_SIG(3) |
              SP_WR_CLR_SIG(4) | SP_WR_CLR_SIG(5) | SP_WR_CLR_SIG(6) | SP_WR_CLR_SIG(7);
  SP_STATUS = SP_WR_CLR_HALT;
}

void k64_rsp_run(u32 pc) { run(pc, SP_WR_CLR_INTR_BREAK); }
void k64_rsp_run_intr(u32 pc) { run(pc, SP_WR_SET_INTR_BREAK); }

bool k64_rsp_wait(u32 max_spins) {
  for (u32 i = 0; !max_spins || i < max_spins; i++)
    if (SP_STATUS & SP_ST_HALT) return true;
  return false;
}
