// kestrel64-sdk — reset button (see include/k64/reset.h).
#include <k64/reset.h>
#include <k64/cpu.h>

#define SR_IE         0x0001u
#define SR_IM_PRENMI  0x1000u   // IP4: reset button (pre-NMI)

// Set by the interrupt handler (crt0.S without threads, thread.c with): Count at the press,
// forced nonzero.
volatile u32 k64_reset_count;

// Thread module, when linked: the mask every thread runs with.
extern u32 k64_thread_im __attribute__((weak));

void k64_reset_watch(void) {
  k64_irq_disable();
  if (&k64_thread_im) k64_thread_im |= SR_IM_PRENMI;
  k64_set_status(k64_status() | SR_IM_PRENMI);
  k64_irq_restore(1);
}

bool k64_reset_pending(void) { return k64_reset_count != 0; }

u32 k64_reset_elapsed(void) { return k64_reset_count ? k64_count() - k64_reset_count : 0; }

bool k64_reset_warm(void) { return K64_BOOT_RESET != 0; }

u8 *k64_reset_buffer(void) { return (u8 *)0x8000031C; }
