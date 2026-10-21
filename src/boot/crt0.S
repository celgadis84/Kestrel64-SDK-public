# kestrel64-sdk — CPU entry point.
#
# Contract with IPL3 (libdragon ipl3_compat, same as the retail IPL3): the first 1 MiB of
# ROM after offset 0x1000 has been copied to the header entry point (0x80000400), caches
# are initialised, RDRAM is up, and boot info sits at 0x80000300.
#
# We: set up the stack and COP0/COP1 state, pull in anything that did not fit in the 1 MiB
# boot copy, clear BSS, install the exception vectors and jump to main().

    .set noreorder
    .set noat
    .set mips3

    .section .text.boot, "ax", @progbits
    .global _start
    .weak   k64_logo_play
    .type   _start, @function
_start:
    lui     $sp, %hi(__stack_top)
    addiu   $sp, $sp, %lo(__stack_top)

    # Status: CU1 (bit 29) + FR (bit 26, n32 wants 32 x 64-bit FPRs). IE=0, EXL=0, ERL=0,
    # BEV=0 (vectors in RDRAM), 32-bit kernel addressing (KX=0).
    lui     $t0, 0x2400
    mtc0    $t0, $12
    nop
    # FCSR: FS (flush denormals to zero -> no unimplemented-operation traps), RN, no enables.
    lui     $t0, 0x0100
    ctc1    $t0, $31

    # ---- load the part of the image beyond the 1 MiB boot copy (if any) -------------
    lui     $t0, %hi(__boot_copy_end)       # RAM address where the IPL3 copy stopped
    addiu   $t0, $t0, %lo(__boot_copy_end)
    lui     $t1, %hi(__load_end)            # RAM end of everything that must be resident
    addiu   $t1, $t1, %lo(__load_end)
    sltu    $t2, $t0, $t1
    beqz    $t2, 2f
    nop
    # PI DMA: cart (__boot_copy_end - 0x80000400 + 0x10001000) -> RDRAM phys
    lui     $t3, 0xA460                     # PI base
1:  lw      $t2, 0x10($t3)                  # wait PI idle
    andi    $t2, $t2, 3
    bnez    $t2, 1b
    nop
    lui     $t2, 0x1FFF
    ori     $t2, $t2, 0xFFFF
    and     $t2, $t0, $t2                   # phys dst
    sw      $t2, 0x00($t3)                  # PI_DRAM_ADDR
    lui     $t2, %hi(0x10001000 - 0x80000400)
    addiu   $t2, $t2, %lo(0x10001000 - 0x80000400)
    addu    $t2, $t2, $t0
    sw      $t2, 0x04($t3)                  # PI_CART_ADDR
    subu    $t2, $t1, $t0
    addiu   $t2, $t2, -1
    sw      $t2, 0x0C($t3)                  # PI_WR_LEN (cart -> RDRAM)
1:  lw      $t2, 0x10($t3)
    andi    $t2, $t2, 3
    bnez    $t2, 1b
    nop
    # Lines of that range may be cached from the IPL3 copy loop: drop them.
    move    $t2, $t0
1:  cache   0x11, 0($t2)                    # D hit invalidate
    cache   0x10, 0($t2)                    # I hit invalidate
    addiu   $t2, $t2, 16
    sltu    $at, $t2, $t1
    bnez    $at, 1b
    nop
2:
    # ---- clear BSS (8-byte aligned by the linker script) -----------------------------
    lui     $t0, %hi(__bss_start)
    addiu   $t0, $t0, %lo(__bss_start)
    lui     $t1, %hi(__bss_end)
    addiu   $t1, $t1, %lo(__bss_end)
    beq     $t0, $t1, 2f
    nop
1:  sd      $zero, 0($t0)
    addiu   $t0, $t0, 8
    sltu    $at, $t0, $t1
    bnez    $at, 1b
    nop
2:
    jal     k64_runtime_init
    nop
    lui     $t0, %hi(k64_logo_play)          # boot logo, when linked (k64_add_rom)
    addiu   $t0, $t0, %lo(k64_logo_play)
    beqz    $t0, 4f
    nop
    jalr    $t0
    nop
4:
    jal     main
    nop
    jal     k64_exit
    move    $a0, $v0
3:  b       3b
    nop
    .size _start, . - _start

# ---- exception vector stub (copied to 0x80000000/080/100/180 by k64_runtime_init) ----
    .section .text.vec, "ax", @progbits
    .global k64_vector_stub
    .global k64_vector_stub_end
# Each vector slot is 0x80 bytes and branches cannot reach the handler, so the stub loads its
# full address into k0 (the register the ABI leaves to exception code) and jumps.
k64_vector_stub:
    lui     $k0, %hi(k64_exception_entry)
    addiu   $k0, $k0, %lo(k64_exception_entry)
    jr      $k0
    nop
k64_vector_stub_end:

# ---- general exception entry ------------------------------------------------------------
# Interrupts go to the thread switch when the thread module runs (k64/thread.h), else to
# k64_vi_isr (the only interrupt enabled without threads). Syscalls only happen with the
# thread module. Anything else is a fault: the context is frozen into k64_exc_frame and
# k64_crash() never returns.
    .section .text.exc, "ax", @progbits
    .global k64_exception_entry
k64_exception_entry:
    mfc0    $k0, $13            # Cause
    andi    $k0, $k0, 0x7C      # ExcCode << 2
    beqz    $k0, k64_irq_entry  # 0: interrupt
    addiu   $k0, $k0, -(8 << 2)
    beqz    $k0, k64_thread_sys # 8: syscall
    nop
    # k64_exc_frame_t layout: gpr[n] at n * 8 (zero, k0, k1 skipped), then hi, lo, EPC, Status,
    # Cause, BadVAddr, FCSR. t0 is reused as scratch only after its own value is stored.
    lui     $k0, %hi(k64_exc_frame)
    addiu   $k0, $k0, %lo(k64_exc_frame)
    sd      $1,   8($k0)
    sd      $2,  16($k0)
    sd      $3,  24($k0)
    sd      $4,  32($k0)
    sd      $5,  40($k0)
    sd      $6,  48($k0)
    sd      $7,  56($k0)
    sd      $8,  64($k0)
    sd      $9,  72($k0)
    sd      $10, 80($k0)
    sd      $11, 88($k0)
    sd      $12, 96($k0)
    sd      $13,104($k0)
    sd      $14,112($k0)
    sd      $15,120($k0)
    sd      $16,128($k0)
    sd      $17,136($k0)
    sd      $18,144($k0)
    sd      $19,152($k0)
    sd      $20,160($k0)
    sd      $21,168($k0)
    sd      $22,176($k0)
    sd      $23,184($k0)
    sd      $24,192($k0)
    sd      $25,200($k0)
    sd      $28,224($k0)
    sd      $29,232($k0)
    sd      $30,240($k0)
    sd      $31,248($k0)
    mfhi    $t0
    sd      $t0,256($k0)
    mflo    $t0
    sd      $t0,264($k0)
    mfc0    $t0, $14            # EPC
    sw      $t0,272($k0)
    mfc0    $t0, $12            # Status
    sw      $t0,276($k0)
    mfc0    $t0, $13            # Cause
    sw      $t0,280($k0)
    mfc0    $t0, $8             # BadVAddr
    sw      $t0,284($k0)
    cfc1    $t0, $31            # FCSR
    sw      $t0,288($k0)
    # leave exception level so C code runs normally; stay with interrupts off
    lui     $t0, 0x2400
    mtc0    $t0, $12
    nop
    # Own stack: the faulting sp may be the cause of the fault.
    lui     $sp, %hi(k64_crash_stack + 4096)
    addiu   $sp, $sp, %lo(k64_crash_stack + 4096)
    jal     k64_crash
    move    $a0, $k0
1:  b       1b
    nop

# ---- interrupts without the thread module ----------------------------------------------
# Two sources, k0/k1 only so no context save:
# - pre-NMI (reset button, k64_reset_watch): record Count in k64_reset_count (reset.c) and
#   mask IP4, which stays asserted until the NMI.
# - VI vblank, enabled by k64_display_init for interlaced modes: load the per-field
#   VI_ORIGIN / VI_V_BURST from k64_vi_field (vi.c).
    .weak   k64_vi_field
    .weak   k64_reset_count
k64_irq_entry:
    lui     $k0, %hi(k64_thread_cur)
    addiu   $k0, $k0, %lo(k64_thread_cur)
    beqz    $k0, k64_bare_irq   # thread module not linked
    nop
    lw      $k0, 0($k0)
    bnez    $k0, k64_thread_exc # threads running
    nop
k64_bare_irq:
    mfc0    $k0, $13            # Cause
    mfc0    $k1, $12            # Status
    and     $k0, $k0, $k1
    andi    $k0, $k0, 0x1000    # IP4 pending and enabled
    beqz    $k0, k64_vi_isr
    nop
    mfc0    $k0, $9             # Count
    ori     $k0, $k0, 1         # nonzero = pressed
    lui     $k1, %hi(k64_reset_count)
    sw      $k0, %lo(k64_reset_count)($k1)
    mfc0    $k0, $12
    addiu   $k1, $zero, -0x1001 # ~0x1000
    and     $k0, $k0, $k1
    mtc0    $k0, $12
    nop
    eret
k64_vi_isr:
    lui     $k1, 0xA440         # VI
    lw      $k0, 0x10($k1)      # V_CURRENT: bit 0 = field
    sw      $zero, 0x10($k1)    # ack
    andi    $k0, $k0, 1
    sll     $k0, $k0, 3         # field * 8
    lui     $k1, %hi(k64_vi_field)
    addu    $k1, $k1, $k0
    lw      $k0, %lo(k64_vi_field)($k1)
    lui     $k1, 0xA440
    sw      $k0, 0x04($k1)      # VI_ORIGIN
    lw      $k0, 0x10($k1)      # two registers only: look the field up again
    andi    $k0, $k0, 1
    sll     $k0, $k0, 3
    lui     $k1, %hi(k64_vi_field + 4)
    addu    $k1, $k1, $k0
    lw      $k0, %lo(k64_vi_field + 4)($k1)
    lui     $k1, 0xA440
    sw      $k0, 0x2C($k1)      # VI_V_BURST
    eret

# ---- thread switch (k64/thread.h) ----------------------------------------------------------
# Saves the running thread's context into k64_thread_cur (k64_thread_ctx_t at offset 0),
# runs k64_thread_dispatch(cause) on the kernel stack with EXL still set (interrupts off),
# then restores the thread it returns and erets into it. Status is restored with the
# kernel's current interrupt mask (k64_thread_im) in place of the saved one.
#
# An interrupt can land anywhere: full context. A syscall is a call to k64_thread_syscall,
# so only what the n32 ABI preserves across calls is saved (s0-s7, gp, sp, fp, ra,
# f20-f31, FCSR) and ctx.partial tells the restore to load just that.
# Weak: ROMs that never call k64_threads_init do not link the thread module (and never
# take an interrupt or syscall).
    .weak   k64_thread_cur
    .weak   k64_thread_im
    .weak   k64_thread_kstack
    .weak   k64_thread_dispatch
    .section .text.exc, "ax", @progbits
k64_thread_exc:
    lui     $k0, %hi(k64_thread_cur)
    lw      $k0, %lo(k64_thread_cur)($k0)
    # k64_thread_ctx_t: gpr[n] at n * 8, hi 256, lo 264, epc 272, sr 276, fcsr 280,
    # partial 284, fpr[n] at 288 + n * 8.
    sd      $1, 8($k0)
    sd      $2, 16($k0)
    sd      $3, 24($k0)
    sd      $4, 32($k0)
    sd      $5, 40($k0)
    sd      $6, 48($k0)
    sd      $7, 56($k0)
    sd      $8, 64($k0)
    sd      $9, 72($k0)
    sd      $10, 80($k0)
    sd      $11, 88($k0)
    sd      $12, 96($k0)
    sd      $13, 104($k0)
    sd      $14, 112($k0)
    sd      $15, 120($k0)
    sd      $16, 128($k0)
    sd      $17, 136($k0)
    sd      $18, 144($k0)
    sd      $19, 152($k0)
    sd      $20, 160($k0)
    sd      $21, 168($k0)
    sd      $22, 176($k0)
    sd      $23, 184($k0)
    sd      $24, 192($k0)
    sd      $25, 200($k0)
    sd      $28, 224($k0)
    sd      $29, 232($k0)
    sd      $30, 240($k0)
    sd      $31, 248($k0)
    mfhi    $t0
    sd      $t0, 256($k0)
    mflo    $t0
    sd      $t0, 264($k0)
    mfc0    $t0, $14            # EPC
    sw      $t0, 272($k0)
    sdc1    $f0, 288($k0)
    sdc1    $f1, 296($k0)
    sdc1    $f2, 304($k0)
    sdc1    $f3, 312($k0)
    sdc1    $f4, 320($k0)
    sdc1    $f5, 328($k0)
    sdc1    $f6, 336($k0)
    sdc1    $f7, 344($k0)
    sdc1    $f8, 352($k0)
    sdc1    $f9, 360($k0)
    sdc1    $f10, 368($k0)
    sdc1    $f11, 376($k0)
    sdc1    $f12, 384($k0)
    sdc1    $f13, 392($k0)
    sdc1    $f14, 400($k0)
    sdc1    $f15, 408($k0)
    sdc1    $f16, 416($k0)
    sdc1    $f17, 424($k0)
    sdc1    $f18, 432($k0)
    sdc1    $f19, 440($k0)
    sdc1    $f20, 448($k0)
    sdc1    $f21, 456($k0)
    sdc1    $f22, 464($k0)
    sdc1    $f23, 472($k0)
    sdc1    $f24, 480($k0)
    sdc1    $f25, 488($k0)
    sdc1    $f26, 496($k0)
    sdc1    $f27, 504($k0)
    sdc1    $f28, 512($k0)
    sdc1    $f29, 520($k0)
    sdc1    $f30, 528($k0)
    sdc1    $f31, 536($k0)
    b       2f
    sw      $zero, 284($k0)     # partial = 0

k64_thread_sys:
    lui     $k0, %hi(k64_thread_cur)
    lw      $k0, %lo(k64_thread_cur)($k0)
    sd      $16, 128($k0)
    sd      $17, 136($k0)
    sd      $18, 144($k0)
    sd      $19, 152($k0)
    sd      $20, 160($k0)
    sd      $21, 168($k0)
    sd      $22, 176($k0)
    sd      $23, 184($k0)
    sd      $28, 224($k0)
    sd      $29, 232($k0)
    sd      $30, 240($k0)
    sd      $31, 248($k0)
    mfc0    $t0, $14            # EPC: resume after the syscall
    addiu   $t0, $t0, 4
    sw      $t0, 272($k0)
    sdc1    $f20, 448($k0)
    sdc1    $f21, 456($k0)
    sdc1    $f22, 464($k0)
    sdc1    $f23, 472($k0)
    sdc1    $f24, 480($k0)
    sdc1    $f25, 488($k0)
    sdc1    $f26, 496($k0)
    sdc1    $f27, 504($k0)
    sdc1    $f28, 512($k0)
    sdc1    $f29, 520($k0)
    sdc1    $f30, 528($k0)
    sdc1    $f31, 536($k0)
    addiu   $t0, $zero, 1
    sw      $t0, 284($k0)       # partial = 1
2:  mfc0    $t0, $12            # Status
    sw      $t0, 276($k0)
    cfc1    $t0, $31            # FCSR
    sw      $t0, 280($k0)
    # Kernel stack (k64_thread_kstack, 256 dwords): dispatch never runs on a thread's stack.
    lui     $sp, %hi(k64_thread_kstack + 2048)
    addiu   $sp, $sp, %lo(k64_thread_kstack + 2048)
    jal     k64_thread_dispatch
    mfc0    $a0, $13            # Cause
    move    $k0, $v0                # the thread to resume

    lw      $t0, 280($k0)
    ctc1    $t0, $31
    lw      $t0, 272($k0)
    mtc0    $t0, $14
    lw      $t0, 276($k0)
    lui     $t1, %hi(k64_thread_im)
    lw      $t1, %lo(k64_thread_im)($t1)
    lui     $t2, 0xFFFF             # saved Status with its IM field (bits 15..8) replaced
    ori     $t2, $t2, 0x00FF
    and     $t0, $t0, $t2
    or      $t0, $t0, $t1
    ori     $t0, $t0, 2         # keep EXL until eret
    mtc0    $t0, $12
    lw      $t0, 284($k0)           # partial (syscall) context: callee-saved registers only
    beqz    $t0, 3f
    nop
    ldc1    $f20, 448($k0)
    ldc1    $f21, 456($k0)
    ldc1    $f22, 464($k0)
    ldc1    $f23, 472($k0)
    ldc1    $f24, 480($k0)
    ldc1    $f25, 488($k0)
    ldc1    $f26, 496($k0)
    ldc1    $f27, 504($k0)
    ldc1    $f28, 512($k0)
    ldc1    $f29, 520($k0)
    ldc1    $f30, 528($k0)
    ldc1    $f31, 536($k0)
    ld      $16, 128($k0)
    ld      $17, 136($k0)
    ld      $18, 144($k0)
    ld      $19, 152($k0)
    ld      $20, 160($k0)
    ld      $21, 168($k0)
    ld      $22, 176($k0)
    ld      $23, 184($k0)
    ld      $28, 224($k0)
    ld      $29, 232($k0)
    ld      $30, 240($k0)
    ld      $31, 248($k0)
    nop
    eret

3:  # full context (interrupt, or a thread's first run); t0 last via $8 below
    ldc1    $f0, 288($k0)
    ldc1    $f1, 296($k0)
    ldc1    $f2, 304($k0)
    ldc1    $f3, 312($k0)
    ldc1    $f4, 320($k0)
    ldc1    $f5, 328($k0)
    ldc1    $f6, 336($k0)
    ldc1    $f7, 344($k0)
    ldc1    $f8, 352($k0)
    ldc1    $f9, 360($k0)
    ldc1    $f10, 368($k0)
    ldc1    $f11, 376($k0)
    ldc1    $f12, 384($k0)
    ldc1    $f13, 392($k0)
    ldc1    $f14, 400($k0)
    ldc1    $f15, 408($k0)
    ldc1    $f16, 416($k0)
    ldc1    $f17, 424($k0)
    ldc1    $f18, 432($k0)
    ldc1    $f19, 440($k0)
    ldc1    $f20, 448($k0)
    ldc1    $f21, 456($k0)
    ldc1    $f22, 464($k0)
    ldc1    $f23, 472($k0)
    ldc1    $f24, 480($k0)
    ldc1    $f25, 488($k0)
    ldc1    $f26, 496($k0)
    ldc1    $f27, 504($k0)
    ldc1    $f28, 512($k0)
    ldc1    $f29, 520($k0)
    ldc1    $f30, 528($k0)
    ldc1    $f31, 536($k0)
    ld      $t0, 256($k0)
    mthi    $t0
    ld      $t0, 264($k0)
    mtlo    $t0
    ld      $1, 8($k0)
    ld      $2, 16($k0)
    ld      $3, 24($k0)
    ld      $4, 32($k0)
    ld      $5, 40($k0)
    ld      $6, 48($k0)
    ld      $7, 56($k0)
    ld      $8, 64($k0)
    ld      $9, 72($k0)
    ld      $10, 80($k0)
    ld      $11, 88($k0)
    ld      $12, 96($k0)
    ld      $13, 104($k0)
    ld      $14, 112($k0)
    ld      $15, 120($k0)
    ld      $16, 128($k0)
    ld      $17, 136($k0)
    ld      $18, 144($k0)
    ld      $19, 152($k0)
    ld      $20, 160($k0)
    ld      $21, 168($k0)
    ld      $22, 176($k0)
    ld      $23, 184($k0)
    ld      $24, 192($k0)
    ld      $25, 200($k0)
    ld      $28, 224($k0)
    ld      $29, 232($k0)
    ld      $30, 240($k0)
    ld      $31, 248($k0)
    nop
    eret

# Enters the kernel to reschedule (never placed in a branch delay slot: EPC + 4 resumes).
    .section .text, "ax", @progbits
    .global k64_thread_syscall
k64_thread_syscall:
    syscall
    jr      $ra
    nop
