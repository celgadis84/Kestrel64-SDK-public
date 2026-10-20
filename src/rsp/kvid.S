// kestrel64-sdk — kvid: K64V reconstruct microcode (RSP). docs/VIDEO.md, C side src/video/.
//
// One run reconstructs one macroblock row: it streams the row's work list (k64/video.h)
// from RDRAM, and for each strip of up to 4 macroblocks DMAs the previous frame's Y/U/V
// pixels in (strided), overwrites the prediction of motion macroblocks with the displaced
// source block, rebuilds the coded blocks (IDCT + prediction + clamp), fills uncoded intra
// blocks with 128 and DMAs the strip out to the current frame's planes. Bit-exact with
// k64_video_reconstruct_cpu.
//
// Motion: the source block starts at any byte, DMA wants 8-byte alignment. The aligned
// window around it (16 rows x 24 bytes luma, 8 x 16 per chroma plane) is DMAed to RGBBUF
// (free until the colour conversion) and each row moved into the strip with lqv + lrv (a
// 16-byte load from any address) and sqv / sdv.
//
// IDCT, no transposes. With Mc_u the vector (M[0][u] .. M[7][u]) and X_v the coefficient
// row v (lanes u):
//   pass 1  T_i = sum_v X_v * Mc_v[i]        (lanes u, element i of Mc_v broadcast)
//   pass 2  D_i = sum_u Mc_u * T_i[u]        (lanes j = pixel row i)
// each as vmulf + 7 x vmacf: sat16((2 * sum + 0x8000) >> 16), the products of the
// reference in the same accumulator. Then d = vmulf by 0x1000 (round /8), and the pixel
// works in the luv/suv domain (<< 7): sat16(d * 128) + pred << 7, clamped to 0 .. 255 << 7.
// That is exact: saturation only happens where the final clamp would decide anyway.
//
// Then, if FB is set, the strip is converted to RGBA5551 and DMAed to the framebuffer, two
// luma rows (one chroma row) at a time. Full-range BT.601 in the << 7 domain (y7 = Y << 7,
// e7 / d7 = (U / V - 128) << 7), per 8 chroma samples:
//   rc = vmulf(d7, 0.402) + d7    gc = vmulf(e7, -0.344) + vmacf(d7, -0.714)    bc = vmulf(e7, 0.772) + e7
// each chroma value is applied to two luma pixels by accumulating it through 0/1 lane masks
// (vmudh y7 * 1, then vmadh mask_k * c[k]), the channel clamped to 0 .. 255 << 7 and its top
// five bits packed (R >> 10) << 11 | (G >> 10) << 6 | (B >> 10) << 1 | 1. k64_video_blit is
// the same computation on the CPU.

// ---- DMEM ------------------------------------------------------------------------------------
    .equ COEF,   0x100          // 8x8 s16 coefficients of the current block
    .equ WBUF,   0x200          // work list window
    .equ WCHUNK, 0x400
    .equ WNEED,  260            // largest item: count + 64 x {index, value}
    .equ YBUF,   0x600          // strip: 16 rows x 16n bytes
    .equ UBUF,   0xA00          // 8 rows x 8n
    .equ VBUF,   0xB00
    .equ RGBBUF, 0xC00          // 4 x 256: two output rows of 16n pixels each
    .equ MCY,    0xC00          // motion windows (in RGBBUF): 16 x 24 luma,
    .equ MCU,    0xD80          // 8 x 16 U,
    .equ MCV,    0xE00          // 8 x 16 V
    .equ STRIP,  4              // macroblocks per strip

    .data
    .org 0x000
mcol:                           // Mc_u, u = 0..7 (the transpose of the Q15 IDCT matrix)
    .half  11585,  11585,  11585,  11585,  11585,  11585,  11585,  11585
    .half  16069,  13623,   9102,   3196,  -3196,  -9102, -13623, -16069
    .half  15137,   6270,  -6270, -15137, -15137,  -6270,   6270,  15137
    .half  13623,  -3196, -16069,  -9102,   9102,  16069,   3196, -13623
    .half  11585, -11585, -11585,  11585,  11585, -11585, -11585,  11585
    .half   9102, -16069,   3196,  13623, -13623,  -3196,  16069,  -9102
    .half   6270, -15137,  15137,  -6270,  -6270,  15137, -15137,   6270
    .half   3196,  -9102,  13623, -16069,  16069, -13623,   9102,  -3196
    .org 0x080
kconst:                         // /8 rounding, << 7, 128 << 7, 255 << 7
    .half 0x1000, 128, 16384, 32640, 0, 0, 0, 0

    .org 0x090                  // parameters, written by the CPU before each run (physical)
    .global WORK
WORK:   .word 0                 // work list of the row (8-byte aligned)
    .global YROW
YROW:   .word 0                 // first byte of the row in each plane
    .global UROW
UROW:   .word 0
    .global VROW
VROW:   .word 0
    .global WIDTH
WIDTH:  .word 0                 // luma width in bytes (multiple of 16)
    .global FB
FB:     .word 0                 // RGBA5551 destination of the row's first pixel, 0 = none
    .global FBSTRIDE
FBSTRIDE: .word 0               // framebuffer bytes per line

    .org 0x0B0
kc0:                            // 0.402 -0.344 -0.714 0.772 (Q15), 128 << 7, 255 << 7, -, 1
    .half 13173, -11277, -23401, 25297, 16384, 32640, 0, 1
kc1:                            // G >> 5, B >> 10 (vmudm), R / G masks, alpha, << 1 (vmudn)
    .half 2048, 64, 0x7C00, 0x03E0, 1, 2, 0, 0

    .org 0x0D0                  // more parameters: the previous frame (physical)
    .global RYROW
RYROW:  .word 0                 // first byte of the row in each plane (same layout as YROW)
    .global RUROW
RUROW:  .word 0
    .global RVROW
RVROW:  .word 0
    .global RYBASE
RYBASE: .word 0                 // first byte of each plane (motion offsets are from these)
    .global RUBASE
RUBASE: .word 0
    .global RVBASE
RVBASE: .word 0

    .org 0x180                  // after COEF
smask:                          // chroma k of a half goes to lanes 2k, 2k + 1
    .half 1, 1, 0, 0, 0, 0, 0, 0
    .half 0, 0, 1, 1, 0, 0, 0, 0
    .half 0, 0, 0, 0, 1, 1, 0, 0
    .half 0, 0, 0, 0, 0, 0, 1, 1

// ---- registers -------------------------------------------------------------------------------
    .def vzero  $v0
    .def vk     $v25            // kconst
    .def vfill  $v26            // 128 << 7 in every lane
    .def vpred  $v27
    // $v1-$v8 X_v / D_i, $v9-$v16 T_i, $v17-$v24 Mc_u

    .def cur    $s0             // work list cursor (DMEM)
    .def wend   $s1             // end of the valid window
    .def wbase  $s2             // RDRAM address of WBUF
    .def mx     $s3             // first macroblock of the strip
    .def nmb    $s4             // macroblocks in the row
    .def n      $s5             // macroblocks in the strip
    .def m      $s6             // macroblock in the strip
    .def ystr   $s7             // strip strides: 16n, 8n
    .def cstr   $t8
    .def ylen   $a3             // SP_RD/WR_LEN words of the strip DMAs
    .def clen   $fp
    .def mode   $k0
    .def cbp    $k1
    .def blk    $gp             // block 0..5 (Y0 Y1 Y2 Y3 U V)
    .def dst    $t6             // block pixels, row stride
    .def stride $t7

.macro PASS1 d, i
    vmulf   \d, $v1, $v17[\i]
    vmacf   \d, $v2, $v18[\i]
    vmacf   \d, $v3, $v19[\i]
    vmacf   \d, $v4, $v20[\i]
    vmacf   \d, $v5, $v21[\i]
    vmacf   \d, $v6, $v22[\i]
    vmacf   \d, $v7, $v23[\i]
    vmacf   \d, $v8, $v24[\i]
.endm

.macro PASS2 d, t
    vmulf   \d, $v17, \t[0]
    vmacf   \d, $v18, \t[1]
    vmacf   \d, $v19, \t[2]
    vmacf   \d, $v20, \t[3]
    vmacf   \d, $v21, \t[4]
    vmacf   \d, $v22, \t[5]
    vmacf   \d, $v23, \t[6]
    vmacf   \d, $v24, \t[7]
.endm

.macro CHAN d, c, e0, e1, e2, e3
    vmudh   \d, $v4, $v9[7]            // y7
    vmadh   \d, $v11, \c[\e0]
    vmadh   \d, $v12, \c[\e1]
    vmadh   \d, $v13, \c[\e2]
    vmadh   \d, $v14, \c[\e3]
    vge     \d, \d, vzero
    vlt     \d, \d, $v9[5]
.endm

// 8 pixels: luma at yoff(yb), chroma elements e0..e3 of $v1-$v3, RGBA5551 to ooff(ob).
.macro RGB8 yoff, yb, ooff, ob, e0, e1, e2, e3
    luv     $v4[0], \yoff(\yb)
    CHAN    $v5, $v1, \e0, \e1, \e2, \e3
    CHAN    $v6, $v2, \e0, \e1, \e2, \e3
    CHAN    $v7, $v3, \e0, \e1, \e2, \e3
    vand    $v5, $v5, $v10[2]           // R >> 10 << 10
    vmudm   $v6, $v6, $v10[0]
    vand    $v6, $v6, $v10[3]           // G >> 10 << 5
    vmudm   $v7, $v7, $v10[1]           // B >> 10
    vor     $v5, $v5, $v6
    vor     $v5, $v5, $v7
    vmudn   $v5, $v5, $v10[5]           // << 1 (unsigned: up to 0xFFFE)
    vor     $v5, $v5, $v10[4]
    sqv     $v5[0], \ooff(\ob)
.endm

.macro PIXROW d
    vmulf   \d, \d, vk[0]               // round(p / 8)
    luv     vpred[0], 0(dst)
    vmudh   \d, \d, vk[1]               // sat16(d << 7)
    vadd    \d, \d, vpred               // (VCO is clear: no carry-in)
    vge     \d, \d, vzero
    vlt     \d, \d, vk[3]
    suv     \d[0], 0(dst)
    addu    dst, dst, stride
.endm

// ---- code ------------------------------------------------------------------------------------
    .text
    .global START
START:
    vxor    vzero, vzero, vzero
    ctc2    $zero, vco
    lqv     $v17[0], 0x00($zero)
    lqv     $v18[0], 0x10($zero)
    lqv     $v19[0], 0x20($zero)
    lqv     $v20[0], 0x30($zero)
    lqv     $v21[0], 0x40($zero)
    lqv     $v22[0], 0x50($zero)
    lqv     $v23[0], 0x60($zero)
    lqv     $v24[0], 0x70($zero)
    lqv     vk[0], kconst($zero)
    vor     vfill, vzero, vk[2]
    lw      wbase, WORK($zero)
    li      cur, WBUF + WCHUNK          // empty window just before WORK: the first
    li      wend, WBUF + WCHUNK         // ensure loads it from WORK
    addiu   wbase, wbase, -WCHUNK
    lw      $t0, WIDTH($zero)
    srl     nmb, $t0, 4
    li      mx, 0

strip_loop:
    subu    n, nmb, mx
    slti    $t0, n, STRIP + 1
    bnez    $t0, strip_n
    nop
    li      n, STRIP
strip_n:
    sll     ystr, n, 4
    sll     cstr, n, 3
    lw      $t0, WIDTH($zero)
    addiu   ylen, ystr, -1              // 16 rows of 16n, skipping the rest of the line
    ori     ylen, ylen, 15 << 12
    subu    $t1, $t0, ystr
    sll     $t1, $t1, 20
    or      ylen, ylen, $t1
    srl     $t0, $t0, 1
    addiu   clen, cstr, -1              // 8 rows of 8n
    ori     clen, clen, 7 << 12
    subu    $t1, $t0, cstr
    sll     $t1, $t1, 20
    or      clen, clen, $t1

    jal     strip_addr
    li      $sp, RYROW                  // in: the previous frame
    jal     dma_rd
    li      $t3, YBUF
    jal     dma_rd
    li      $t3, UBUF
    jal     dma_rd
    li      $t3, VBUF
    jal     dma_wait
    li      m, 0

mb_loop:
    jal     ensure
    nop
    lhu     mode, 0(cur)
    addiu   cur, cur, 2
    srl     cbp, mode, 2
    andi    mode, mode, 3
    beqz    mode, mb_next               // skip: the previous frame stays
    li      blk, 0
    xori    $t0, mode, 3
    bnez    $t0, blk_loop
    nop
    jal     motion                      // then it is inter on the moved pixels
    li      mode, 1

blk_loop:
    slti    $t0, blk, 4
    beqz    $t0, blk_chroma
    sll     $t1, m, 4
    srl     $t2, blk, 1                 // luma: YBUF + (blk >> 1) * 8 rows + m * 16 + (blk & 1) * 8
    subu    $t2, $zero, $t2
    sll     $t3, ystr, 3
    and     $t3, $t3, $t2
    andi    $t4, blk, 1
    sll     $t4, $t4, 3
    addu    dst, $t1, $t3
    addu    dst, dst, $t4
    addiu   dst, dst, YBUF
    b       blk_have
    move    stride, ystr
blk_chroma:
    sll     $t1, m, 3                   // chroma: UBUF / VBUF + m * 8
    addiu   $t2, blk, -4
    sll     $t2, $t2, 8
    addu    dst, $t1, $t2
    addiu   dst, dst, UBUF
    move    stride, cstr
blk_have:
    li      $t0, 0x20
    srlv    $t0, $t0, blk
    and     $t0, $t0, cbp
    bnez    $t0, blk_coded
    addiu   $t1, mode, -2               // 0 = intra
    bnez    $t1, blk_next               // inter, not coded: unchanged
    nop
    jal     fill128
    nop
    b       blk_next
    nop
blk_coded:
    bnez    $t1, blk_idct               // inter predicts from the pixels in place,
    nop
    jal     fill128                     // intra from 128
    nop
blk_idct:
    jal     ensure
    nop
    sqv     vzero[0], COEF + 0x00($zero)
    sqv     vzero[0], COEF + 0x10($zero)
    sqv     vzero[0], COEF + 0x20($zero)
    sqv     vzero[0], COEF + 0x30($zero)
    sqv     vzero[0], COEF + 0x40($zero)
    sqv     vzero[0], COEF + 0x50($zero)
    sqv     vzero[0], COEF + 0x60($zero)
    sqv     vzero[0], COEF + 0x70($zero)
    lhu     $t0, 0(cur)                 // count (1..64), then {index, value}
    addiu   cur, cur, 2
scatter:
    lhu     $t1, 0(cur)
    lh      $t2, 2(cur)
    addiu   cur, cur, 4
    sll     $t1, $t1, 1
    addiu   $t0, $t0, -1
    bnez    $t0, scatter
    sh      $t2, COEF($t1)
    lqv     $v1[0], COEF + 0x00($zero)
    lqv     $v2[0], COEF + 0x10($zero)
    lqv     $v3[0], COEF + 0x20($zero)
    lqv     $v4[0], COEF + 0x30($zero)
    lqv     $v5[0], COEF + 0x40($zero)
    lqv     $v6[0], COEF + 0x50($zero)
    lqv     $v7[0], COEF + 0x60($zero)
    lqv     $v8[0], COEF + 0x70($zero)
    PASS1   $v9, 0
    PASS1   $v10, 1
    PASS1   $v11, 2
    PASS1   $v12, 3
    PASS1   $v13, 4
    PASS1   $v14, 5
    PASS1   $v15, 6
    PASS1   $v16, 7
    PASS2   $v1, $v9
    PASS2   $v2, $v10
    PASS2   $v3, $v11
    PASS2   $v4, $v12
    PASS2   $v5, $v13
    PASS2   $v6, $v14
    PASS2   $v7, $v15
    PASS2   $v8, $v16
    PIXROW  $v1
    PIXROW  $v2
    PIXROW  $v3
    PIXROW  $v4
    PIXROW  $v5
    PIXROW  $v6
    PIXROW  $v7
    PIXROW  $v8
blk_next:
    addiu   blk, blk, 1
    slti    $t0, blk, 6
    bnez    $t0, blk_loop
    nop
mb_next:
    addiu   m, m, 1
    bne     m, n, mb_loop
    nop

    jal     strip_addr
    li      $sp, YROW                   // out: the current frame
    jal     dma_wr
    li      $t3, YBUF
    jal     dma_wr
    li      $t3, UBUF
    jal     dma_wr
    li      $t3, VBUF

    // ---- colour conversion of the strip ----
    lw      $fp, FB($zero)
    beqz    $fp, strip_done
    lw      $t1, FBSTRIDE($zero)
    lqv     $v9[0], kc0($zero)
    lqv     $v10[0], kc1($zero)
    lqv     $v11[0], smask + 0x00($zero)
    lqv     $v12[0], smask + 0x10($zero)
    lqv     $v13[0], smask + 0x20($zero)
    lqv     $v14[0], smask + 0x30($zero)
    sll     $t2, n, 5                   // 32n bytes per output row; DMA 2 rows
    addiu   ylen, $t2, -1
    ori     ylen, ylen, 1 << 12
    subu    $t3, $t1, $t2
    sll     $t3, $t3, 20
    or      ylen, ylen, $t3
    sll     $a1, $t1, 1
    sll     $t3, mx, 5
    addu    $fp, $fp, $t3
    li      mode, 8                     // chroma rows left
    li      cbp, RGBBUF
    li      $a0, YBUF
    li      $a2, UBUF
c_row:
    move    $t0, $a2
    addiu   $t1, $a2, VBUF - UBUF
    move    $t6, $a0
    move    $t7, cbp
    move    m, n
c_grp:                                  // 8 chroma samples = 16 x 2 pixels
    luv     $v8[0], 0($t0)
    luv     $v15[0], 0($t1)
    vsub    $v8, $v8, $v9[4]            // e7
    vsub    $v15, $v15, $v9[4]          // d7
    vmulf   $v1, $v15, $v9[0]
    vadd    $v1, $v1, $v15              // rc
    vmulf   $v2, $v8, $v9[1]
    vmacf   $v2, $v15, $v9[2]           // gc
    vmulf   $v3, $v8, $v9[3]
    vadd    $v3, $v3, $v8               // bc
    addu    $t2, $t6, ystr
    sll     $t3, n, 5
    addu    $t3, $t7, $t3
    RGB8    0, $t6, 0, $t7, 0, 1, 2, 3
    RGB8    8, $t6, 16, $t7, 4, 5, 6, 7
    RGB8    0, $t2, 0, $t3, 0, 1, 2, 3
    RGB8    8, $t2, 16, $t3, 4, 5, 6, 7
    addiu   $t0, $t0, 8
    addiu   $t1, $t1, 8
    addiu   $t6, $t6, 16
    addiu   m, m, -1
    bnez    m, c_grp
    addiu   $t7, $t7, 32
c_dma:                                  // the buffer refilled next was queued 4 DMAs ago:
    mfc0    $t9, SP_DMA_FULL            // done once the queue has room
    bnez    $t9, c_dma
    nop
    mtc0    cbp, SP_MEM_ADDR
    mtc0    $fp, SP_DRAM_ADDR
    mtc0    ylen, SP_WR_LEN
    addu    $fp, $fp, $a1
    addiu   cbp, cbp, 0x100
    li      $t2, RGBBUF + 0x400
    bne     cbp, $t2, c_next
    sll     $t2, ystr, 1
    li      cbp, RGBBUF
c_next:
    addu    $a0, $a0, $t2
    addu    $a2, $a2, cstr
    addiu   mode, mode, -1
    bnez    mode, c_row
    nop
strip_done:
    addu    mx, mx, n
    bne     mx, nmb, strip_loop
    nop
    jal     dma_wait
    nop
    break

// RDRAM addresses of the strip in the planes whose row addresses are at $sp (YROW or
// RYROW): $t4 Y, $t5 U, $a2 V. Then dma_rd / dma_wr take them in that order (each call
// shifts: Y with ylen, then U and V with clen).
strip_addr:
    lw      $t4, 0($sp)
    sll     $t0, mx, 4
    addu    $t4, $t4, $t0
    lw      $t5, 4($sp)
    sll     $t0, mx, 3
    addu    $t5, $t5, $t0
    lw      $a2, 8($sp)
    addu    $a2, $a2, $t0
    move    $a1, ylen
    jr      $ra
    nop

// DMEM $t3 <-> RDRAM $t4 with length word $a1, then shift ($t4 <- $t5 <- $a2, $a1 <- clen).
dma_rd:
    mfc0    $t9, SP_DMA_FULL
    bnez    $t9, dma_rd
    nop
    mtc0    $t3, SP_MEM_ADDR
    mtc0    $t4, SP_DRAM_ADDR
    b       dma_shift
    mtc0    $a1, SP_RD_LEN
dma_wr:
    mfc0    $t9, SP_DMA_FULL
    bnez    $t9, dma_wr
    nop
    mtc0    $t3, SP_MEM_ADDR
    mtc0    $t4, SP_DRAM_ADDR
    mtc0    $a1, SP_WR_LEN
dma_shift:
    move    $t4, $t5
    move    $t5, $a2
    jr      $ra
    move    $a1, clen

dma_wait:
    mfc0    $t9, SP_DMA_BUSY
    bnez    $t9, dma_wait
    nop
    jr      $ra
    nop

// Guarantees WNEED bytes of work list at cur (reloads the window from cur's RDRAM address).
ensure:
    addiu   $t0, cur, WNEED
    sltu    $t0, wend, $t0
    beqz    $t0, ensure_ok
    addiu   $t1, cur, -WBUF
    addu    $t1, $t1, wbase
    andi    $t2, $t1, 7
    subu    wbase, $t1, $t2
    addiu   cur, $t2, WBUF
    li      wend, WBUF + WCHUNK
ensure_dma:
    mfc0    $t9, SP_DMA_FULL
    bnez    $t9, ensure_dma
    li      $t3, WBUF
    mtc0    $t3, SP_MEM_ADDR
    mtc0    wbase, SP_DRAM_ADDR
    li      $t3, WCHUNK - 1
    mtc0    $t3, SP_RD_LEN
ensure_wait:
    mfc0    $t9, SP_DMA_BUSY
    bnez    $t9, ensure_wait
    nop
ensure_ok:
    jr      $ra
    nop

// Motion macroblock m: reads the two source offsets at cur, DMAs the aligned windows and
// copies the 16x16 luma and 8x8 U / V source blocks over the strip's pixels.
motion:
    move    $at, $ra
    lw      $t0, 0(cur)                 // luma offset (cur is only 2-aligned: DMEM allows it)
    lw      $t1, 4(cur)                 // chroma offset
    addiu   cur, cur, 8
    lw      $t2, WIDTH($zero)
    lw      $t3, RYBASE($zero)
    addu    $t0, $t0, $t3
    andi    $a0, $t0, 7                 // luma window offset
    subu    $t0, $t0, $a0
    addiu   $t3, $t2, -24               // 16 rows of 24, skipping the rest of the line
    sll     $t3, $t3, 20
    ori     $t3, $t3, (15 << 12) + 23
    li      $t4, MCY
    jal     mc_dma
    nop
    lw      $t3, RUBASE($zero)
    addu    $t0, $t1, $t3
    andi    $a1, $t0, 7                 // chroma window offset (same for U and V)
    subu    $t0, $t0, $a1
    srl     $t2, $t2, 1
    addiu   $t3, $t2, -16               // 8 rows of 16
    sll     $t3, $t3, 20
    ori     $t3, $t3, (7 << 12) + 15
    jal     mc_dma
    li      $t4, MCU
    lw      $t4, RVBASE($zero)
    addu    $t0, $t1, $t4
    subu    $t0, $t0, $a1
    jal     mc_dma
    li      $t4, MCV
mc_wait:
    mfc0    $t9, SP_DMA_BUSY
    bnez    $t9, mc_wait
    sll     $t2, m, 4
    addiu   $t2, $t2, YBUF              // luma rows -> YBUF + 16m, stride ystr
    addiu   $t0, $a0, MCY
    li      $t4, 16
mc_y:
    lqv     $v1[0], 0($t0)
    lrv     $v1[0], 16($t0)
    sqv     $v1[0], 0($t2)
    addiu   $t0, $t0, 24
    addiu   $t4, $t4, -1
    bnez    $t4, mc_y
    addu    $t2, $t2, ystr
    sll     $t2, m, 3
    addiu   $t2, $t2, UBUF              // chroma rows -> UBUF / VBUF + 8m, stride cstr
    addiu   $t0, $a1, MCU
    li      $t4, 8
mc_c:
    lqv     $v1[0], 0($t0)
    lrv     $v1[0], 16($t0)
    sdv     $v1[0], 0($t2)
    lqv     $v1[0], MCV - MCU($t0)
    lrv     $v1[0], MCV - MCU + 16($t0)
    sdv     $v1[0], VBUF - UBUF($t2)
    addiu   $t0, $t0, 16
    addiu   $t4, $t4, -1
    bnez    $t4, mc_c
    addu    $t2, $t2, cstr
    jr      $at
    nop

// DMEM $t4 <- RDRAM $t0, length word $t3.
mc_dma:
    mfc0    $t9, SP_DMA_FULL
    bnez    $t9, mc_dma
    nop
    mtc0    $t4, SP_MEM_ADDR
    mtc0    $t0, SP_DRAM_ADDR
    jr      $ra
    mtc0    $t3, SP_RD_LEN

// 8 rows of 128 at dst (keeps dst / stride).
fill128:
    move    $t5, dst
    li      $t4, 8
fill_row:
    suv     vfill[0], 0($t5)
    addiu   $t4, $t4, -1
    bnez    $t4, fill_row
    addu    $t5, $t5, stride
    jr      $ra
    nop
