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
    // IDCT
    .def vrow0  $v1            // X_v coefficient row v, then D_i pixel row i
    .def vrow1  $v2
    .def vrow2  $v3
    .def vrow3  $v4
    .def vrow4  $v5
    .def vrow5  $v6
    .def vrow6  $v7
    .def vrow7  $v8
    .def vtr0   $v9            // T_i, pass 1 result
    .def vtr1   $v10
    .def vtr2   $v11
    .def vtr3   $v12
    .def vtr4   $v13
    .def vtr5   $v14
    .def vtr6   $v15
    .def vtr7   $v16
    .def vmc0   $v17            // Mc_u, u = 0..7, loaded once at START
    .def vmc1   $v18
    .def vmc2   $v19
    .def vmc3   $v20
    .def vmc4   $v21
    .def vmc5   $v22
    .def vmc6   $v23
    .def vmc7   $v24
    // colour conversion (reuses the IDCT registers, which are free by then)
    .def vrc    $v1             // chroma terms of R, G, B for 8 chroma samples
    .def vgc    $v2
    .def vbc    $v3
    .def vy7    $v4             // 8 luma samples << 7
    .def vr     $v5             // channels of 8 pixels, then the packed RGBA5551
    .def vg     $v6
    .def vb     $v7
    .def ve7    $v8             // (U - 128) << 7
    .def vkc0   $v9             // kc0: conversion factors, 128 << 7, 255 << 7, 1
    .def vkc1   $v10            // kc1: packing shifts and masks
    .def vsm0   $v11            // smask rows: chroma k -> lanes 2k, 2k + 1
    .def vsm1   $v12
    .def vsm2   $v13
    .def vsm3   $v14
    .def vd7    $v15            // (V - 128) << 7

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
    .def dmastat $t9            // SP_DMA_FULL / SP_DMA_BUSY polls
    // strip DMA convention (strip_addr, dma_rd, dma_wr)
    .def rowtab $sp             // in: YROW or RYROW, the three plane row addresses
    .def dmem   $t3             // in: DMEM side of the next DMA
    .def dram0  $t4             // RDRAM side of the next DMA, then the two after it
    .def dram1  $t5
    .def dram2  $a2
    .def dlen   $a1             // length word of the next DMA
    // $t0-$t5 are scratch unless a routine's header says otherwise

// T_i = sum_v X_v * Mc_v[i]: one pass-1 row into \d.
.macro PASS1 d, i
    vmulf   \d, vrow0, vmc0[\i]
    vmacf   \d, vrow1, vmc1[\i]
    vmacf   \d, vrow2, vmc2[\i]
    vmacf   \d, vrow3, vmc3[\i]
    vmacf   \d, vrow4, vmc4[\i]
    vmacf   \d, vrow5, vmc5[\i]
    vmacf   \d, vrow6, vmc6[\i]
    vmacf   \d, vrow7, vmc7[\i]
.endm

// D_i = sum_u Mc_u * T_i[u]: pixel row i into \d from T_i in \t.
.macro PASS2 d, t
    vmulf   \d, vmc0, \t[0]
    vmacf   \d, vmc1, \t[1]
    vmacf   \d, vmc2, \t[2]
    vmacf   \d, vmc3, \t[3]
    vmacf   \d, vmc4, \t[4]
    vmacf   \d, vmc5, \t[5]
    vmacf   \d, vmc6, \t[6]
    vmacf   \d, vmc7, \t[7]
.endm

// One channel of 8 pixels into \d: y7 plus chroma term \c[e_k] on the lanes of sample k,
// clamped to 0 .. 255 << 7.
.macro CHAN d, c, e0, e1, e2, e3
    vmudh   \d, vy7, vkc0[7]            // y7 * 1
    vmadh   \d, vsm0, \c[\e0]
    vmadh   \d, vsm1, \c[\e1]
    vmadh   \d, vsm2, \c[\e2]
    vmadh   \d, vsm3, \c[\e3]
    vge     \d, \d, vzero
    vlt     \d, \d, vkc0[5]
.endm

// 8 pixels: luma at yoff(yb), chroma elements e0..e3 of vrc / vgc / vbc, RGBA5551 to ooff(ob).
.macro RGB8 yoff, yb, ooff, ob, e0, e1, e2, e3
    luv     vy7[0], \yoff(\yb)
    CHAN    vr, vrc, \e0, \e1, \e2, \e3
    CHAN    vg, vgc, \e0, \e1, \e2, \e3
    CHAN    vb, vbc, \e0, \e1, \e2, \e3
    vand    vr, vr, vkc1[2]             // R >> 10 << 10
    vmudm   vg, vg, vkc1[0]
    vand    vg, vg, vkc1[3]             // G >> 10 << 5
    vmudm   vb, vb, vkc1[1]             // B >> 10
    vor     vr, vr, vg
    vor     vr, vr, vb
    vmudn   vr, vr, vkc1[5]             // << 1 (unsigned: up to 0xFFFE)
    vor     vr, vr, vkc1[4]             // alpha
    sqv     vr[0], \ooff(\ob)
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
    lqv     vmc0[0], mcol + 0x00($zero)
    lqv     vmc1[0], mcol + 0x10($zero)
    lqv     vmc2[0], mcol + 0x20($zero)
    lqv     vmc3[0], mcol + 0x30($zero)
    lqv     vmc4[0], mcol + 0x40($zero)
    lqv     vmc5[0], mcol + 0x50($zero)
    lqv     vmc6[0], mcol + 0x60($zero)
    lqv     vmc7[0], mcol + 0x70($zero)
    lqv     vk[0], kconst($zero)
    vor     vfill, vzero, vk[2]
    lw      wbase, WORK($zero)
    li      cur, WBUF + WCHUNK          // empty window just before WORK: the first
    li      wend, WBUF + WCHUNK         // ensure loads it from WORK
    addiu   wbase, wbase, -WCHUNK
    lw      $t0, WIDTH($zero)
    srl     nmb, $t0, 4
    li      mx, 0

    .def width  $t0             // strip setup: luma, then chroma width in bytes
    .def skip   $t1             // bytes skipped per DMA line, at the SKIP field (<< 20)
strip_loop:
    subu    n, nmb, mx
    slti    $t0, n, STRIP + 1
    bnez    $t0, strip_n
    nop
    li      n, STRIP
strip_n:
    sll     ystr, n, 4
    sll     cstr, n, 3
    lw      width, WIDTH($zero)
    addiu   ylen, ystr, -1              // 16 rows of 16n, skipping the rest of the line
    ori     ylen, ylen, 15 << 12
    subu    skip, width, ystr
    sll     skip, skip, 20
    or      ylen, ylen, skip
    srl     width, width, 1
    addiu   clen, cstr, -1              // 8 rows of 8n
    ori     clen, clen, 7 << 12
    subu    skip, width, cstr
    sll     skip, skip, 20
    or      clen, clen, skip
    .undef width
    .undef skip

    jal     strip_addr
    li      rowtab, RYROW               // in: the previous frame
    jal     dma_rd
    li      dmem, YBUF
    jal     dma_rd
    li      dmem, UBUF
    jal     dma_rd
    li      dmem, VBUF
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

    .def colofs $t1             // block address terms: column (m), row half, half column
    .def rowmsk $t2
    .def rowofs $t3
    .def hcol   $t4
    .def plane  $t2             // chroma: 0 = U, 1 = V, then its offset
    .def inter  $t1             // blk_have on: mode - 2, nonzero for inter
blk_loop:
    slti    $t0, blk, 4
    beqz    $t0, blk_chroma
    sll     colofs, m, 4
    srl     rowmsk, blk, 1              // luma: YBUF + (blk >> 1) * 8 rows + m * 16 + (blk & 1) * 8
    subu    rowmsk, $zero, rowmsk       // 0 or all ones
    sll     rowofs, ystr, 3
    and     rowofs, rowofs, rowmsk
    andi    hcol, blk, 1
    sll     hcol, hcol, 3
    addu    dst, colofs, rowofs
    addu    dst, dst, hcol
    addiu   dst, dst, YBUF
    b       blk_have
    move    stride, ystr
blk_chroma:
    sll     colofs, m, 3                // chroma: UBUF / VBUF + m * 8
    addiu   plane, blk, -4              // 0 = U, 1 = V (VBUF = UBUF + 0x100)
    sll     plane, plane, 8
    addu    dst, colofs, plane
    addiu   dst, dst, UBUF
    move    stride, cstr
blk_have:
    li      $t0, 0x20                   // cbp bit of the block: 0x20 >> blk
    srlv    $t0, $t0, blk
    and     $t0, $t0, cbp
    bnez    $t0, blk_coded
    addiu   inter, mode, -2             // 0 = intra
    bnez    inter, blk_next             // inter, not coded: unchanged
    nop
    jal     fill128
    nop
    b       blk_next
    nop
blk_coded:
    bnez    inter, blk_idct             // inter predicts from the pixels in place,
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
    .undef colofs
    .undef rowmsk
    .undef rowofs
    .undef hcol
    .undef plane
    .undef inter
    .def ncoef  $t0
    .def cidx   $t1             // coefficient index, then its byte offset in COEF
    .def cval   $t2
    lhu     ncoef, 0(cur)               // count (1..64), then {index, value}
    addiu   cur, cur, 2
scatter:
    lhu     cidx, 0(cur)
    lh      cval, 2(cur)
    addiu   cur, cur, 4
    sll     cidx, cidx, 1
    addiu   ncoef, ncoef, -1
    bnez    ncoef, scatter
    sh      cval, COEF(cidx)
    .undef ncoef
    .undef cidx
    .undef cval
    lqv     vrow0[0], COEF + 0x00($zero)
    lqv     vrow1[0], COEF + 0x10($zero)
    lqv     vrow2[0], COEF + 0x20($zero)
    lqv     vrow3[0], COEF + 0x30($zero)
    lqv     vrow4[0], COEF + 0x40($zero)
    lqv     vrow5[0], COEF + 0x50($zero)
    lqv     vrow6[0], COEF + 0x60($zero)
    lqv     vrow7[0], COEF + 0x70($zero)
    PASS1   vtr0, 0
    PASS1   vtr1, 1
    PASS1   vtr2, 2
    PASS1   vtr3, 3
    PASS1   vtr4, 4
    PASS1   vtr5, 5
    PASS1   vtr6, 6
    PASS1   vtr7, 7
    PASS2   vrow0, vtr0
    PASS2   vrow1, vtr1
    PASS2   vrow2, vtr2
    PASS2   vrow3, vtr3
    PASS2   vrow4, vtr4
    PASS2   vrow5, vtr5
    PASS2   vrow6, vtr6
    PASS2   vrow7, vtr7
    PIXROW  vrow0
    PIXROW  vrow1
    PIXROW  vrow2
    PIXROW  vrow3
    PIXROW  vrow4
    PIXROW  vrow5
    PIXROW  vrow6
    PIXROW  vrow7
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
    li      rowtab, YROW                // out: the current frame
    jal     dma_wr
    li      dmem, YBUF
    jal     dma_wr
    li      dmem, UBUF
    jal     dma_wr
    li      dmem, VBUF

    // ---- colour conversion of the strip ----
    // The strip's work registers are free here: clen, mode, cbp, m and the DMA convention
    // registers take the roles below until strip_done.
    .def fbptr  $fp             // RDRAM: framebuffer address of the next output row pair
    .def fbstr  $t1             // FBSTRIDE (setup only)
    .def orow   $t2             // 32n: bytes of one output row (setup only)
    .def fbstep $a1             // 2 * FBSTRIDE
    .def fblen  $a3             // SP_WR_LEN word: 2 rows of 32n, skipping the rest of the line
    .def crows  $k0             // chroma rows left
    .def obuf   $k1             // RGBBUF slot being filled (4 x 0x100, round robin)
    .def yrow   $a0             // DMEM: luma row pair, chroma row of the group loop
    .def urow   $a2
    .def uptr   $t0             // group loop: U, V, luma, output of the row pair's top row
    .def vptr   $t1
    .def yptr   $t6
    .def optr   $t7
    .def y2ptr  $t2             // bottom row: luma, output
    .def o2ptr  $t3
    .def ngrp   $s6             // groups (8 chroma samples) left in the row
    lw      fbptr, FB($zero)
    beqz    fbptr, strip_done
    lw      fbstr, FBSTRIDE($zero)
    lqv     vkc0[0], kc0($zero)
    lqv     vkc1[0], kc1($zero)
    lqv     vsm0[0], smask + 0x00($zero)
    lqv     vsm1[0], smask + 0x10($zero)
    lqv     vsm2[0], smask + 0x20($zero)
    lqv     vsm3[0], smask + 0x30($zero)
    sll     orow, n, 5                  // 32n bytes per output row; DMA 2 rows
    addiu   fblen, orow, -1
    ori     fblen, fblen, 1 << 12
    subu    $t3, fbstr, orow
    sll     $t3, $t3, 20
    or      fblen, fblen, $t3
    sll     fbstep, fbstr, 1
    sll     $t3, mx, 5
    addu    fbptr, fbptr, $t3
    li      crows, 8
    li      obuf, RGBBUF
    li      yrow, YBUF
    li      urow, UBUF
c_row:
    move    uptr, urow
    addiu   vptr, urow, VBUF - UBUF
    move    yptr, yrow
    move    optr, obuf
    move    ngrp, n
c_grp:                                  // 8 chroma samples = 16 x 2 pixels
    luv     ve7[0], 0(uptr)
    luv     vd7[0], 0(vptr)
    vsub    ve7, ve7, vkc0[4]           // e7
    vsub    vd7, vd7, vkc0[4]           // d7
    vmulf   vrc, vd7, vkc0[0]
    vadd    vrc, vrc, vd7               // rc
    vmulf   vgc, ve7, vkc0[1]
    vmacf   vgc, vd7, vkc0[2]           // gc
    vmulf   vbc, ve7, vkc0[3]
    vadd    vbc, vbc, ve7               // bc
    addu    y2ptr, yptr, ystr
    sll     o2ptr, n, 5
    addu    o2ptr, optr, o2ptr
    RGB8    0, yptr, 0, optr, 0, 1, 2, 3
    RGB8    8, yptr, 16, optr, 4, 5, 6, 7
    RGB8    0, y2ptr, 0, o2ptr, 0, 1, 2, 3
    RGB8    8, y2ptr, 16, o2ptr, 4, 5, 6, 7
    addiu   uptr, uptr, 8
    addiu   vptr, vptr, 8
    addiu   yptr, yptr, 16
    addiu   ngrp, ngrp, -1
    bnez    ngrp, c_grp
    addiu   optr, optr, 32
c_dma:                                  // the buffer refilled next was queued 4 DMAs ago:
    mfc0    dmastat, SP_DMA_FULL        // done once the queue has room
    bnez    dmastat, c_dma
    nop
    mtc0    obuf, SP_MEM_ADDR
    mtc0    fbptr, SP_DRAM_ADDR
    mtc0    fblen, SP_WR_LEN
    addu    fbptr, fbptr, fbstep
    addiu   obuf, obuf, 0x100
    li      $t2, RGBBUF + 0x400
    bne     obuf, $t2, c_next
    sll     $t2, ystr, 1                // luma advances two rows
    li      obuf, RGBBUF
c_next:
    addu    yrow, yrow, $t2
    addu    urow, urow, cstr
    addiu   crows, crows, -1
    bnez    crows, c_row
    nop
    .undef fbptr
    .undef fbstr
    .undef orow
    .undef fbstep
    .undef fblen
    .undef crows
    .undef obuf
    .undef yrow
    .undef urow
    .undef uptr
    .undef vptr
    .undef yptr
    .undef optr
    .undef y2ptr
    .undef o2ptr
    .undef ngrp
strip_done:
    addu    mx, mx, n
    bne     mx, nmb, strip_loop
    nop
    jal     dma_wait
    nop
    break

// RDRAM addresses of the strip in the planes whose row addresses are at rowtab (YROW or
// RYROW): dram0 Y, dram1 U, dram2 V, and dlen = ylen. Then dma_rd / dma_wr take them in
// that order (each call shifts: Y with ylen, then U and V with clen).
// Clobbers $t0.
strip_addr:
    lw      dram0, 0(rowtab)
    sll     $t0, mx, 4
    addu    dram0, dram0, $t0
    lw      dram1, 4(rowtab)
    sll     $t0, mx, 3
    addu    dram1, dram1, $t0
    lw      dram2, 8(rowtab)
    addu    dram2, dram2, $t0
    move    dlen, ylen
    jr      $ra
    nop

// DMEM dmem <-> RDRAM dram0 with length word dlen, then shift
// (dram0 <- dram1 <- dram2, dlen <- clen). Waits only for room in the DMA queue.
dma_rd:
    mfc0    dmastat, SP_DMA_FULL
    bnez    dmastat, dma_rd
    nop
    mtc0    dmem, SP_MEM_ADDR
    mtc0    dram0, SP_DRAM_ADDR
    b       dma_shift
    mtc0    dlen, SP_RD_LEN
dma_wr:
    mfc0    dmastat, SP_DMA_FULL
    bnez    dmastat, dma_wr
    nop
    mtc0    dmem, SP_MEM_ADDR
    mtc0    dram0, SP_DRAM_ADDR
    mtc0    dlen, SP_WR_LEN
dma_shift:
    move    dram0, dram1
    move    dram1, dram2
    jr      $ra
    move    dlen, clen

// Waits until every queued DMA is done.
dma_wait:
    mfc0    dmastat, SP_DMA_BUSY
    bnez    dmastat, dma_wait
    nop
    jr      $ra
    nop

// Guarantees WNEED bytes of work list at cur (reloads the window from cur's RDRAM address,
// rounded down to 8 bytes, and waits for it). Clobbers $t0-$t3.
    .def wrd    $t1             // RDRAM address of cur
    .def walign $t2             // its misalignment, the offset of cur in the new window
ensure:
    addiu   $t0, cur, WNEED
    sltu    $t0, wend, $t0
    beqz    $t0, ensure_ok
    addiu   wrd, cur, -WBUF
    addu    wrd, wrd, wbase
    andi    walign, wrd, 7
    subu    wbase, wrd, walign
    addiu   cur, walign, WBUF
    li      wend, WBUF + WCHUNK
ensure_dma:
    mfc0    dmastat, SP_DMA_FULL
    bnez    dmastat, ensure_dma
    li      $t3, WBUF
    mtc0    $t3, SP_MEM_ADDR
    mtc0    wbase, SP_DRAM_ADDR
    li      $t3, WCHUNK - 1
    mtc0    $t3, SP_RD_LEN
ensure_wait:
    mfc0    dmastat, SP_DMA_BUSY
    bnez    dmastat, ensure_wait
    nop
ensure_ok:
    jr      $ra
    nop
    .undef wrd
    .undef walign

// Motion macroblock m: reads the two source offsets at cur, DMAs the aligned windows and
// copies the 16x16 luma and 8x8 U / V source blocks over the strip's pixels.
// Clobbers $t0-$t4, $a0, $a1, $at, $v1.
    .def mret   $at             // return address (motion calls mc_dma)
    .def msrc   $t0             // mc_dma: RDRAM source, DMEM destination, length word
    .def mdst   $t4
    .def mlen   $t3
    .def coff   $t1             // chroma source offset (from RUBASE / RVBASE)
    .def mwidth $t2             // luma, then chroma width in bytes
    .def ywin   $a0             // offset of the source block in its aligned window: luma,
    .def cwin   $a1             // chroma (the same for U and V)
    .def mrow   $v1
motion:
    move    mret, $ra
    lw      msrc, 0(cur)                // luma offset (cur is only 2-aligned: DMEM allows it)
    lw      coff, 4(cur)                // chroma offset
    addiu   cur, cur, 8
    lw      mwidth, WIDTH($zero)
    lw      mlen, RYBASE($zero)
    addu    msrc, msrc, mlen
    andi    ywin, msrc, 7
    subu    msrc, msrc, ywin
    addiu   mlen, mwidth, -24           // 16 rows of 24, skipping the rest of the line
    sll     mlen, mlen, 20
    ori     mlen, mlen, (15 << 12) + 23
    li      mdst, MCY
    jal     mc_dma
    nop
    lw      mlen, RUBASE($zero)
    addu    msrc, coff, mlen
    andi    cwin, msrc, 7
    subu    msrc, msrc, cwin
    srl     mwidth, mwidth, 1
    addiu   mlen, mwidth, -16           // 8 rows of 16
    sll     mlen, mlen, 20
    ori     mlen, mlen, (7 << 12) + 15
    jal     mc_dma
    li      mdst, MCU
    lw      mdst, RVBASE($zero)
    addu    msrc, coff, mdst
    subu    msrc, msrc, cwin
    jal     mc_dma
    li      mdst, MCV
    .undef mwidth
    .def mout   $t2             // copy loops: strip destination, window source, rows left
    .def min    $t0
    .def mcnt   $t4
mc_wait:
    mfc0    dmastat, SP_DMA_BUSY
    bnez    dmastat, mc_wait
    sll     mout, m, 4
    addiu   mout, mout, YBUF            // luma rows -> YBUF + 16m, stride ystr
    addiu   min, ywin, MCY
    li      mcnt, 16
mc_y:
    lqv     mrow[0], 0(min)             // 16 bytes from any address: lqv + lrv
    lrv     mrow[0], 16(min)
    sqv     mrow[0], 0(mout)
    addiu   min, min, 24
    addiu   mcnt, mcnt, -1
    bnez    mcnt, mc_y
    addu    mout, mout, ystr
    sll     mout, m, 3
    addiu   mout, mout, UBUF            // chroma rows -> UBUF / VBUF + 8m, stride cstr
    addiu   min, cwin, MCU
    li      mcnt, 8
mc_c:
    lqv     mrow[0], 0(min)
    lrv     mrow[0], 16(min)
    sdv     mrow[0], 0(mout)
    lqv     mrow[0], MCV - MCU(min)
    lrv     mrow[0], MCV - MCU + 16(min)
    sdv     mrow[0], VBUF - UBUF(mout)
    addiu   min, min, 16
    addiu   mcnt, mcnt, -1
    bnez    mcnt, mc_c
    addu    mout, mout, cstr
    jr      mret
    nop
    .undef mout
    .undef min
    .undef mcnt

// DMEM mdst <- RDRAM msrc, length word mlen. Waits only for room in the DMA queue.
mc_dma:
    mfc0    dmastat, SP_DMA_FULL
    bnez    dmastat, mc_dma
    nop
    mtc0    mdst, SP_MEM_ADDR
    mtc0    msrc, SP_DRAM_ADDR
    jr      $ra
    mtc0    mlen, SP_RD_LEN
    .undef mret
    .undef msrc
    .undef mdst
    .undef mlen
    .undef coff
    .undef ywin
    .undef cwin
    .undef mrow

// 8 rows of 128 at dst (keeps dst / stride). Clobbers $t4, $t5.
    .def frow   $t5
    .def fcnt   $t4
fill128:
    move    frow, dst
    li      fcnt, 8
fill_row:
    suv     vfill[0], 0(frow)
    addiu   fcnt, fcnt, -1
    bnez    fcnt, fill_row
    addu    frow, frow, stride
    jr      $ra
    nop
    .undef frow
    .undef fcnt
