// kestrel64-sdk — SI joybus controller reads.
#include <k64/pad.h>
#include <k64/cpu.h>

k64_pad_t k64_pads[4];

// PIF RAM image: per channel {tx=1, rx=4, cmd=0x01 (read buttons), 4 reply bytes, 0xFF pad},
// 0xFE terminates, byte 63 = 1 tells the PIF to run the joybus program. The pad byte must be
// 0xFF (ignored): a 0x00 there would make the PIF skip the next channel.
static u64 pif_cmd[8] __attribute__((aligned(16)));
static u64 pif_rep[8] __attribute__((aligned(16)));
static bool pending;                         // poll_start ran; poll_end has not collected it yet

// Fills pif_cmd: four channel records, the 0xFE terminator and the run flag in byte 63.
static void build_cmd(void) {
  u8 *b = (u8*)pif_cmd;
  for (int i = 0; i < 64; i++) b[i] = 0;
  for (int c = 0; c < 4; c++) {
    u8 *p = b + c * 8;
    p[0] = 0x01; p[1] = 0x04; p[2] = 0x01; p[3] = p[4] = p[5] = p[6] = p[7] = 0xFF;
  }
  b[32] = 0xFE;
  b[63] = 0x01;
}

// Spins until the SI DMA and the SI I/O are both idle.
static void si_wait(void) { while (SI_STATUS & (SI_ST_DMA_BUSY | SI_ST_IO_BUSY)) {} }

void k64_pad_poll_start(void) {
  if (!pif_cmd[7]) build_cmd();                // byte 63 is set by build_cmd(), so nonzero = built
  si_wait();
  k64_dcache_writeback(pif_cmd, 64);           // the SI DMA reads RDRAM: flush the command image
  SI_DRAM_ADDR = K64_PHYS(pif_cmd);
  SI_PIF_AD_WR64B = PIF_RAM_PHYS;              // RDRAM -> PIF RAM; the PIF runs the program
  si_wait();
  k64_dcache_invalidate(pif_rep, 64);          // reply lands behind the cache: drop stale lines
  SI_DRAM_ADDR = K64_PHYS(pif_rep);
  SI_PIF_AD_RD64B = PIF_RAM_PHYS;              // PIF RAM -> RDRAM, collected by poll_end
  pending = true;
}

void k64_pad_poll_end(void) {
  if (!pending) return;
  si_wait();                                   // the reply must be in RDRAM before it is read
  SI_STATUS = 0;                               // any write acks the SI interrupt
  pending = false;
  const u8 *b = (const u8*)pif_rep;            // lines invalidated before the DMA: cached reads are fresh
  for (int c = 0; c < 4; c++) {
    const u8 *p = b + c * 8;
    k64_pad_t *pad = &k64_pads[c];
    bool ok = (p[1] & 0xC0) == 0;                // rx error bits: no device / overrun
    // 0xFF3F drops bit 7 (the L+R+Start stick-reset flag) and bit 6 (unused).
    u16 held = ok ? (u16)((p[3] << 8) | p[4]) & 0xFF3F : 0;
    pad->present = ok;
    pad->pressed = held & ~pad->held;          // edges against the previous poll
    pad->released = pad->held & ~held;
    pad->held = held;
    pad->x = ok ? (s8)p[5] : 0;                // analog stick, signed
    pad->y = ok ? (s8)p[6] : 0;
  }
}

void k64_pad_poll(void) { k64_pad_poll_start(); k64_pad_poll_end(); }
