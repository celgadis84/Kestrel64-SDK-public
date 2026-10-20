// kestrel64-sdk — kgfx: geometry microcode (RSP).
//
// Reads a command list from RDRAM (8-byte commands, opcode in the top byte), transforms
// vertices into a DMEM vertex cache and (stage B+) emits RDP triangle commands.
// Layout, formats and command encoding: docs/KGFX.md. C side: include/k64/kgfx.h.
//
// Fixed-point conventions
//   matrix   s15.16, column-major m[col][row], stored as 16 x s16 integer halves followed by
//            16 x u16 fraction halves (64 bytes)
//   vertex   s16 model coordinates (scale them through the matrix)
//   clip     s15.16 (int/frac split), screen x/y s13.2, z 0..0x7FFE, 1/w s15.16
//
// Two vertices are processed per pass: vertex A in vector elements 0-3, B in 4-7, the
// matrix columns are duplicated in both halves and inputs are broadcast with [Nh].

// ---- DMEM ------------------------------------------------------------------------------------
    .equ VCACHE_STRIDE, 40
    .equ VCACHE_COUNT,  32
    .equ INBUF_SIZE,    VCACHE_COUNT * 16
    .equ CMDBUF_SIZE,   256

    .data
    .org 0x000
vconst_data:                    // $v1: e0=0 e1=1 e2=2 e3=0x7FFF e4=4 e5=0x200 e6=0x8000 e7=-1
    .half 0, 1, 2, 0x7FFF, 4, 0x200, 0x8000, 0xFFFF

    .org 0x010
    .global VIEWPORT
VIEWPORT:                       // s16 scale[8] then s16 trans[8] (x,y,z,K twice: W normalize
                                // K = scale[3] + trans[3] / 65536)
    .half 640, -480, 0x3FFF, 1, 640, -480, 0x3FFF, 1
    .half 640,  480, 0x3FFF, 0, 640,  480, 0x3FFF, 0

    .org 0x030
    .global CMDPTR
CMDPTR:     .word 0             // RDRAM address of the command list (written by the CPU)
    .global STATUS
STATUS:     .word 0             // 0 = ok, else 0x100 | opcode of a bad command
    .global VTXCOUNT
VTXCOUNT:   .word 0             // vertices transformed (stats)
    .global TRICOUNT
TRICOUNT:   .word 0             // triangles emitted to the RDP (stats)

    .org 0x040
    .global MTX
MTX:                            // current matrix (identity by default)
    .half 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1
    .half 0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0,  0, 0, 0, 0

    .org 0x080
cmd_table:                      // opcodes 0x10..0x1F
    .half cmd_part_ovl, cmd_mtx, cmd_vtx, cmd_tri, cmd_viewport, cmd_geommode_ovl, cmd_light, cmd_texgen
    .half cmd_cull, cmd_call, cmd_ret, cmd_audio_ovl, cmd_adpcm_ovl, cmd_fog, cmd_rdp, cmd_end

    // RDP output ring, mirrored from the CPU stream (src/gfx/rdp.c) by k64_kgfx_run and
    // handed back by k64_kgfx_wait. Physical addresses.
    .global RING
RING:       .word 0, 0, 0, 0    // block base addresses
RING_IDX:   .word 0             // current block
RING_CUR:   .word 0             // write cursor (everything before it is queued to the RDP)
RING_END:   .word 0             // end of the current block
RING_STARTED: .word 0           // DPC_START issued for the current block

    .global GEOMMODE
GEOMMODE:   .word 0x08000000    // triangle command template: op << 24 | level << 19 | tile << 16
CULLMODE:   .word 0             // bit 31 = cull back, bit 0 = cull front (see tri)
TRIOFS:     .byte 112, 112, 160 // shade, texture, z block offsets in the command (a disabled
TRISIZE:    .byte 32            // block goes to scratch past the enabled ones, < TRI_MAX) and
                                // the bytes per emitted triangle for the current mode
LIGHTMODE:  .word vtx_loop << 16 // VTX passes: first << 16 | return (cmd_geommode)

    .global OVL_TABLE
OVL_TABLE:                      // per overlay: (length - 1) >> 3 << 24 | RDRAM address (set
    .word 0, 0, 0, 0, 0         // by the CPU; all load at OVL_SLOT)
OVL_RESIDENT: .word 0           // OVL_* in the slot (the IMEM image holds OVL_VTX)
    .equ OVL_VTX, 0
    .equ OVL_CLIP, 4
    .equ OVL_AUDIO, 8
    .equ OVL_ADPCM, 12
    .equ OVL_PART, 16

CLIP_PLANE_TABLE:               // flag bit, byte offset of the coordinate, mode (1 = -C, 2 = 4W)
    .half 0x0004                // near: W + Z
    .byte 4, 0
    .half 0x0040                // far: W - Z
    .byte 4, 1
    .half 0x0100                // guard -x: 4W + X
    .byte 0, 2
    .half 0x1000                // guard +x: 4W - X
    .byte 0, 3
    .half 0x0200                // guard -y: 4W + Y
    .byte 2, 2
    .half 0x2000                // guard +y: 4W - Y
    .byte 2, 3
DATA_END:
    // End of the DMEM image: everything below is initialized and DMAed by k64_rsp_load.
    // The work areas above it are scratch (never read before written), so they are not part
    // of the image and cost no RDRAM reads when the microcode is (re)loaded.

    .global VCACHE
    .equ VCACHE,     0x100      // vertex cache, VCACHE_COUNT x VCACHE_STRIDE (0x100..0x5FF)
    .assert DATA_END <= VCACHE, "DMEM image overlaps the vertex cache"
    // Vertex references (TRI, clip lists) are DMEM address / 8: VREF_BASE + slot * 5.
    .equ VREF_BASE,  VCACHE / 8
    .assert VCACHE % 8 == 0 && VREF_BASE == 0x20, "VREF_BASE must match K64_KGFX_VREF"
    .global INBUF
    .equ INBUF,      0x600      // input vertices (0x600..0x7FF)
    .equ CMDBUF,     0x800      // command list chunk (0x800..0x8FF)
    .equ RING_BLOCK_BYTES, 2048 * 8
    .equ OUTBUF,     0x900      // RDP command staging: two halves of OUTBUF_HALF bytes, one
    .equ OUTBUF_HALF, 0x2B8     // filled while the other is DMAed to the ring (0x900..0xE6F)
    .equ FOGMUL,     0xE70      // u16 fog factor scale 2 * max + 2 (set by FOG; with fog off
                                // K = 0, so its value does not matter until the first FOG)
    .equ COLBASE,    0xE74      // colour base: w0 of the last CALL (bits 23..0), 0 at START
    .equ COLTAB,     0x4F8      // colour table loaded by COL, over cache slots 25..31
    .equ COLTAB_SIZE, 264       // 64 colours + up to 8 bytes of DMA misalignment
    .assert COLTAB + COLTAB_SIZE == INBUF && COLTAB % 8 == 0, "COLTAB must end at INBUF"
    .equ TRI_MAX,    176        // largest triangle command (shade + tex + z)
    .equ CLIP_POOL_COUNT, 12    // scratch cache entries 32..43 for clipped vertices (over INBUF)
    .equ CLIP_LISTS, 0x7E0      // two clip polygon lists (cache indices), 16 B each
    .equ CLIP_REJECT, 0x77      // -x -y -z +x +y +z (K64_KGFX_CLIP_*)
    .equ CLIP_PLANES_MASK, 0x3344   // near, far, guard band
    .equ LIGHTS,     0xE80      // k64_kgfx_lights_t (set by LIGHT, not part of the image)
    .equ LIGHTS_SIZE, 0xA0
    .equ LIGHTTMP,   0xF20      // 8 bytes: lit colors of a vertex pair
    .global FOGDATA
    .equ FOGDATA,    0xF28      // s16 start, Ki, u16 Kf, mask (set by FOG; the CPU writes the
                                // disabled state 0, 0, 0, 0xFFFF before each task)
    .equ TEXGEN,     0xF30      // k64_kgfx_texgen_t (set by TEXGEN, not part of the image)
    .equ TEXGEN_SIZE, 0x70
    .equ CLIPFLAGS,  0xFA0      // u16 clip flags per cache slot, 32 x 2 B (written by VTX)
    .equ RETADDR,    0xFE0      // RDRAM return address of CALL (one level)
    .equ RING_PUB,   0xFE4      // ring position up to which DPC_END has been written
    .equ OUTLIMIT,   0xFE8      // OUTBUF half being filled: start + OUTBUF_HALF - TRI_MAX (the
                                // last outptr a triangle may start at)
    .equ NZLUT,      0xFEC      // 16 B: bit length of 0..15 (written by START)
    .global PARTCOUNT
    .equ PARTCOUNT,  0xFFC      // rectangles emitted by PARTICLES (stats, zeroed by START)

// ---- registers -------------------------------------------------------------------------------
    .def vzero  $v0
    .def vconst $v1
    .def vscale $v2
    .def vtrans $v3
    .def vin    $v4             // model xyz (A | B)
    .def vattr  $v5             // s, t, rgba (A | B), copied through
    .def vci    $v6             // clip, integer
    .def vcf    $v7             // clip, fraction
    .def vrci   $v8             // 1/w integer (elements 3 and 7)
    .def vrcf   $v9             // 1/w fraction
    .def vni    $v10            // NDC integer
    .def vnf    $v11            // NDC fraction
    .def vscr   $v12            // screen x, y, z, 1/w int
    .def vtmp   $v13
    .def vwnf   $v14            // -w (vtx_iter's Newton-Raphson step)
    .def vwni   $v15
    .def v4wi   $v24            // 4W (guard band compare)
    .def v4wf   $v25
    .def vzc    $v26            // screen z clamped to >= 0
    .def vfog   $v27            // FOGDATA twice: start, Ki, Kf, mask
    .def vfd    $v28            // fog factor (lanes 3, 7)
    .def vfb    $v29            // b << 8 | alpha after fog (lanes 3, 7), stored a pair later
    .def vfa    $v30            // b, a & mask
    .def vfmul  $v31            // FOGMUL (element 0)
    .def vmi0   $v16            // matrix columns, integer
    .def vmi1   $v17
    .def vmi2   $v18
    .def vmi3   $v19
    .def vmf0   $v20            // matrix columns, fraction
    .def vmf1   $v21
    .def vmf2   $v22
    .def vmf3   $v23

    .def cmdptr $s0             // RDRAM address of the next command chunk
    .def cmdcur $s1             // DMEM read pointer in CMDBUF
    .def cmdend $s2             // DMEM end of the valid chunk
    .def w0     $a0             // current command, high word
    .def w1     $a1             // current command, low word
    .def dmem   $a2             // dma_read: DMEM address
    .def dram   $a3             // dma_read: RDRAM address
    .def dlen   $t9             // dma_read: length - 1
    .def outptr $s3             // OUTBUF write pointer (RDP commands not yet sent)
    .def ntri   $v0             // triangles emitted (stored to TRICOUNT at END)

// ---- code ------------------------------------------------------------------------------------
    .text
    .global START
START:
    vxor    vzero, vzero, vzero
    lqv     vconst[0], vconst_data($zero)
    jal     load_viewport
    nop
    jal     load_matrix
    li      $at, 0                      // matrix registers match MTX
    lw      cmdptr, CMDPTR($zero)
    sw      $zero, STATUS($zero)
    sw      $zero, VTXCOUNT($zero)
    sw      $zero, PARTCOUNT($zero)
    sw      $zero, COLBASE($zero)
    li      ntri, 0
    li      outptr, OUTBUF
    li      $t0, OUTBUF + OUTBUF_HALF - TRI_MAX
    sw      $t0, OUTLIMIT($zero)
    lw      $t0, RING_CUR($zero)
    sw      $t0, RING_PUB($zero)
    li      $t0, 0x00010202
    sw      $t0, NZLUT+0($zero)
    li      $t0, 0x03030303
    sw      $t0, NZLUT+4($zero)
    li      $t0, 0x04040404
    sw      $t0, NZLUT+8($zero)
    sw      $t0, NZLUT+12($zero)
    li      cmdcur, CMDBUF
    li      cmdend, CMDBUF

cmd_next:
    lw      w0, 0(cmdcur)               // both reloaded at cmd_have after a refill
    bne     cmdcur, cmdend, cmd_have_w1
    lw      w1, 4(cmdcur)               // (3-cycle load latency: w0 is ready for the srl)
    li      dmem, CMDBUF
    move    dram, cmdptr
    jal     dma_read
    li      dlen, CMDBUF_SIZE - 1
    addiu   cmdptr, cmdptr, CMDBUF_SIZE
    li      cmdcur, CMDBUF
    li      cmdend, CMDBUF + CMDBUF_SIZE
cmd_have:
    lw      w0, 0(cmdcur)
    lw      w1, 4(cmdcur)
cmd_have_w1:
    srl     $t0, w0, 23
    andi    $t0, $t0, 0x1FE             // opcode * 2
    lhu     $t2, cmd_table - 0x20($t0)  // read before the range check (any DMEM address is fine)
    addiu   $t0, $t0, -0x20
    sltiu   $t1, $t0, 32
    beqz    $t1, cmd_bad
    addiu   cmdcur, cmdcur, 8
    jr      $t2
    nop

cmd_bad:
    srl     $t0, w0, 24
    ori     $t0, $t0, 0x100
    sw      $t0, STATUS($zero)
cmd_end:
    jal     flush_out
    sw      ntri, TRICOUNT($zero)
    jal     flush_publish               // also waits for a trailing MTX / LIGHT / TEXGEN DMA
    nop
    break
    nop

// CULL: w1 = mask of vertex cache slots, w0 bits 23..0 = commands to skip. When every slot in
// the mask is outside one and the same frustum plane, the next commands are skipped (what
// they draw lies inside the hull of those vertices). Stops at the first slot that leaves no
// common plane.
// The flags come from CLIPFLAGS, 8 slots per vector: slots outside the mask are replaced by
// all ones (vmrg under VCC = mask byte), then the 32 lanes are ANDed together.
cmd_cull:
    beqz    w1, cmd_next                // empty mask: nothing to test
    li      $t5, CLIPFLAGS
    lqv     $v4[0], 0x00($t5)
    srl     $t0, w1, 8
    lqv     $v5[0], 0x10($t5)
    srl     $t1, w1, 16
    lqv     $v12[0], 0x20($t5)
    srl     $t2, w1, 24
    lqv     $v13[0], 0x30($t5)
    andi    $t3, w1, 0xFF
    ctc2    $t3, vcc
    andi    $t0, $t0, 0xFF
    vmrg    $v4, $v4, vconst[7]
    ctc2    $t0, vcc
    andi    $t1, $t1, 0xFF
    vmrg    $v5, $v5, vconst[7]
    ctc2    $t1, vcc
    vmrg    $v12, $v12, vconst[7]
    ctc2    $t2, vcc
    vmrg    $v13, $v13, vconst[7]
    vand    $v4, $v4, $v5
    sll     $t4, w0, 8
    vand    $v12, $v12, $v13
    srl     $t4, $t4, 5                 // bytes to skip
    // The AND tree is one long dependency chain: its stalls fetch and decode the next
    // command (speculatively: past the buffered chunk the words are garbage and unused)
    lw      w0, 0(cmdcur)
    lw      w1, 4(cmdcur)
    vand    $v4, $v4, $v12
    srl     $t0, w0, 23
    andi    $t0, $t0, 0x1FE             // opcode * 2
    lhu     $t2, cmd_table - 0x20($t0)
    vand    $v4, $v4, $v4[1q]
    addiu   $t0, $t0, -0x20
    sltiu   $t1, $t0, 32
    xor     $t3, cmdcur, cmdend
    vand    $v4, $v4, $v4[2h]
    sltu    $t3, $zero, $t3             // a command is buffered
    and     $t1, $t1, $t3               // ... and its opcode is valid
    addu    $t4, cmdcur, $t4            // skip target
    vand    $v4, $v4, $v4[4]
    mfc2    $t5, $v4[0]
    andi    $t5, $t5, CLIP_REJECT
    beqz    $t1, cull_redo              // refill or bad opcode
    sltu    $t3, cmdend, $t4
    bnez    $t5, cull_skip
    addiu   cmdcur, cmdcur, 8           // (cull_skip restarts from $t4)
    jr      $t2                         // no common plane: dispatch the next command
    nop
cull_redo:
    bnez    $t5, cull_skip
    nop
    j       cmd_next                    // the normal path reloads
    nop
cull_skip:
    beqz    $t3, cmd_next               // target inside the buffered chunk
    move    cmdcur, $t4
    subu    $t5, $t4, cmdend
    addu    cmdptr, cmdptr, $t5         // past it: fetch from the target
    j       cmd_next
    move    cmdcur, cmdend

// CALL: w1 = RDRAM address (8-byte aligned) of a command list ending with RET; one level.
// w0 bits 23..0 = colour base for COL. The next command's RDRAM address is the fetch pointer
// minus what is still buffered.
cmd_call:
    sw      w0, COLBASE($zero)          // (DMA addresses ignore bits 31..24)
    subu    $t0, cmdend, cmdcur
    subu    $t0, cmdptr, $t0
    sw      $t0, RETADDR($zero)
    move    cmdptr, w1
    j       cmd_next
    move    cmdcur, cmdend              // refetch from the new pointer

cmd_ret:
    lw      cmdptr, RETADDR($zero)
    j       cmd_next
    move    cmdcur, cmdend

// MTX: w1 = RDRAM address of a 64-byte s15.16 matrix; w0 bit 0 (K64_KGFX_MTX_LIGHTS): a light
// block follows it (k64_kgfx_mtxlights_t), loaded as LIGHT would.
// MTX, LIGHT and TEXGEN only start their DMA: the data is first read by the next VTX, whose
// own DMA wait covers them (the DMA queue completes in order). $at != 0 tells VTX to load the
// new matrix into registers.
// The DMA start is inlined (here and in LIGHT / TEXGEN): back to cmd_next from the last mtc0.
cmd_mtx:
    mfc0    $t0, SP_DMA_FULL
    li      $t1, MTX
    bnez    $t0, cmd_mtx
    li      $t2, 63
    mtc0    $t1, SP_MEM_ADDR
    mtc0    w1, SP_DRAM_ADDR
    mtc0    $t2, SP_RD_LEN
    andi    $t3, w0, 1
    li      $at, 1
    beqz    $t3, cmd_next
    addiu   w1, w1, 64                  // the light block (LIGHT below)

// LIGHT: w1 = RDRAM address of a k64_kgfx_lights_t (LIGHTS_SIZE bytes, docs/KGFX.md).
cmd_light:
    mfc0    $t0, SP_DMA_FULL
    li      $t1, LIGHTS
    bnez    $t0, cmd_light
    li      $t2, LIGHTS_SIZE - 1
    mtc0    $t1, SP_MEM_ADDR
    mtc0    w1, SP_DRAM_ADDR
    j       cmd_next
    mtc0    $t2, SP_RD_LEN

// VIEWPORT: w1 = RDRAM address of 32 bytes (scale[8], trans[8]).
cmd_viewport:
    li      dmem, VIEWPORT
    move    dram, w1
    jal     dma_read
    li      dlen, 31
    j       load_viewport
    li      $ra, cmd_next

// GEOMMODE: w0 bits 2..0 = render tile, 5..3 = mip levels - 1; w1 bits 2..0 = K64_TRI_* (Z 1, TEX 2, SHADE 4),
// bits 9..8 = cull (1 back, 2 front). Cold (a few per frame): its body lives in the clip
// overlay, so a GEOMMODE costs a clip overlay load and the next VTX reloads VTX.
cmd_geommode_ovl:
    jal     ovl_load
    li      $t0, OVL_CLIP
    j       cmd_geommode
    nop

// FOG: w0 bits 23..16 = 255 - max (0xFF disables), bits 15..0 = Kf; w1 = start << 16 | Ki.
// VTX replaces the vertex alpha with clamp((w - start) * (Ki + Kf / 65536), 0, 0x7FFF)
// * (2 * max + 2) >> 16 (0..max; >> 7 for max 255), w the integer clip w (eye distance),
// through the mask 0xFF00; disabled, the mask 0xFFFF and K = 0 keep it.
cmd_fog:
    sw      w1, FOGDATA($zero)
    srl     $t0, w0, 16
    andi    $t0, $t0, 0xFF              // 255 - max
    addiu   $t1, $t0, 1
    srl     $t1, $t1, 8                 // 1: disabled
    subu    $t1, $zero, $t1
    andi    $t1, $t1, 0xFF
    ori     $t1, $t1, 0xFF00            // mask
    sll     $t0, $t0, 1
    li      $t2, 0x200
    subu    $t0, $t2, $t0               // 2 * max + 2
    sh      $t0, FOGMUL($zero)
    sh      w0, FOGDATA+4($zero)
    j       cmd_next
    sh      $t1, FOGDATA+6($zero)

// TEXGEN: w1 = RDRAM address of a k64_kgfx_texgen_t (TEXGEN_SIZE bytes, docs/KGFX.md).
cmd_texgen:
    mfc0    $t0, SP_DMA_FULL
    li      $t1, TEXGEN
    bnez    $t0, cmd_texgen
    li      $t2, TEXGEN_SIZE - 1
    mtc0    $t1, SP_MEM_ADDR
    mtc0    w1, SP_DRAM_ADDR
    j       cmd_next
    mtc0    $t2, SP_RD_LEN

// COL (a VTX with count 0): loads COLTAB_SIZE bytes from colour base + w1, rounded down to 8
// bytes (DMA), into COLTAB for VTX_COLIDX. Only started: the next VTX waits for it.
cmd_col:
    lw      $t1, COLBASE($zero)
    li      $t2, COLTAB
    li      $t4, COLTAB_SIZE - 1
    addu    $t1, $t1, w1
col_dma:
    mfc0    $t0, SP_DMA_FULL
    bnez    $t0, col_dma
    nop
    mtc0    $t2, SP_MEM_ADDR
    mtc0    $t1, SP_DRAM_ADDR
    j       cmd_next
    mtc0    $t4, SP_RD_LEN

// VTX_COLIDX pass (resident: VTX jumps here after its DMA): each input vertex's rgba becomes
// the COLTAB word at byte offset a. $t2 = INBUF, $t3 = count.
vtx_colidx:
    sll     $t5, $t3, 4
    addu    $t5, $t5, $t2
ci_loop:
    lbu     $t6, 11($t2)
    addiu   $t2, $t2, 16
    lw      $t6, COLTAB($t6)
    bne     $t2, $t5, ci_loop
    sw      $t6, 8 - 16($t2)
    j       vtx_ci_done
    nop

// RDP: w1 = RDRAM address (8-byte aligned) of raw RDP commands, w0 bits 7..0 = bytes - 1
// (up to 256, a multiple of 8). The pending triangles go out first, so the commands land at
// the start of an OUTBUF half and keep their order in the ring.
cmd_rdp:
    jal     flush_out
    andi    dlen, w0, 0xFF
    move    dmem, outptr
    jal     dma_read
    move    dram, w1
    addu    outptr, outptr, dlen
    j       cmd_next
    addiu   outptr, outptr, 1

// ---- overlay slot ----------------------------------------------------------------------------
// IMEM from OVL_SLOT to OVL_VTX_END is shared: its default occupant is the VTX code (vertex
// transform, lighting, texgen, overlay OVL_VTX, part of the IMEM image); a triangle that needs
// clipping loads the clip overlay over it (OVL_CLIP) and the next VTX loads VTX back. Both
// go through ovl_load, which skips the DMA when the overlay is already resident. The VTX
// entry of cmd_table points at cmd_vtx while VTX is resident and at cmd_vtx_ovl otherwise
// (ovl_load patches it), so a VTX pays no residency check.
cmd_vtx_ovl:
    jal     ovl_load
    li      $t0, OVL_VTX
    j       OVL_SLOT
    nop
    .balign 8
    .global OVL_SLOT
OVL_SLOT:

// VTX: w0 = op | dest << 16 | count << 8 | colidx, w1 = RDRAM address of count x 16-byte
// vertices (count 0: COL).
//   in:  s16 x, y, z, pad; u8 r, g, b, a; s16 s, t (colidx: a = COLTAB byte offset of rgba)
//   out: VCACHE[dest..dest+count) (VCACHE_STRIDE bytes each, see docs/KGFX.md)
cmd_vtx:
    srl     $t3, w0, 8
    andi    $t3, $t3, 0xFF              // count
    beqz    $t3, cmd_col
    srl     $t4, w0, 16
    // The vertex DMA is started inline; the setup below runs while it is in flight.
vtx_dma:
    mfc0    $t0, SP_DMA_FULL
    sll     $t2, $t3, 4
    bnez    $t0, vtx_dma
    addiu   $t2, $t2, -1
    li      $t1, INBUF
    mtc0    $t1, SP_MEM_ADDR
    mtc0    w1, SP_DRAM_ADDR
    mtc0    $t2, SP_RD_LEN
    andi    $t4, $t4, 0xFF              // dest
    lw      $t0, VTXCOUNT($zero)
    sll     $t1, $t4, 3
    sll     $gp, $t4, 1
    addu    $t0, $t0, $t3
    sw      $t0, VTXCOUNT($zero)
    sll     $t0, $t4, 5
    addu    $a1, $t0, $t1
    lw      $t0, LIGHTMODE($zero)       // passes: first << 16 | return
    addiu   $a1, $a1, VCACHE            // out = VCACHE + dest * 40
    addiu   $gp, $gp, CLIPFLAGS         // CLIPFLAGS + dest * 2
    li      $a2, INBUF                  // in
    srl     $t1, $t0, 16
    li      $s4, 0x03030707             // clip flag nibble masks (vtx_iter)
    li      $s5, 0x00FF00FF
vtx_dma_wait:                           // also covers MTX / LIGHT / TEXGEN / COL (in-order queue)
    mfc0    $t5, SP_DMA_BUSY
    bnez    $t5, vtx_dma_wait
    andi    $t5, w0, 1
    bnez    $t5, vtx_colidx
    li      $t2, INBUF
vtx_ci_done:
    beqz    $at, vtx_mtx_ok
    nop
    jal     load_matrix
    li      $at, 0
vtx_mtx_ok:
    jr      $t1                         // vtx_loop, or light_verts / texgen_verts first
    andi    $ra, $t0, 0xFFFF
vtx_texgen:                             // lighting, then texgen
    j       texgen_verts
    li      $ra, vtx_loop

vtx_loop:
    // Software-pipelined over vertex pairs: each iteration transforms pair k (clip, 1/w, clip
    // flags) and finishes pair k - 1 (NDC, screen, all stores to $t9 = vertex A, $t8 = B).
    // Pair -1 is junk stored over slot `dest` (pair 0 overwrites it); vtx_last finishes the
    // last pair. When a pair has no vertex B, $t8 = $t9 and every B store comes before the
    // matching A store.
    li      $k0, FOGMUL
    move    $t9, $a1
    move    $t8, $a1
    ldv     vfog[0], FOGDATA - 0x1000($zero) // (DMEM wraps; v27.. are free after light / texgen)
    ldv     vfog[8], FOGDATA - 0x1000($zero)
    lsv     vfmul[0], 0($k0)
    move    $k0, $gp                    // flags of the previous pair: A, B
    move    $k1, $gp
vtx_iter:
    ldv     vin[0], 0($a2)
    vmudl   vtmp, vcf, vrcf[3h]         // previous pair: NDC = clip * 1/w (32x32 -> s15.16)
    ldv     vin[8], 16($a2)
    vmadm   vtmp, vci, vrcf[3h]
    slti    $t5, $t3, 2                 // this pair has no vertex B: its B stores go to A
    vmadn   vnf, vcf, vrci[3h]
    xori    $t5, $t5, 1                 // (no store two cycles after a load)
    vmadh   vni, vci, vrci[3h]
    sdv     vattr[8], 0x20($t8)
    vmudh   vtmp, vtrans, vconst[1]     // screen = trans + NDC * scale
    sdv     vattr[0], 0x20($t9)
    vor     vfb, vfa, vfd               // previous pair's b << 8 | fog alpha
    sdv     vci[8], 0x00($t8)
    vmadn   vtmp, vnf, vscale
    sdv     vci[0], 0x00($t9)
    vmadh   vscr, vni, vscale
    sdv     vcf[8], 0x08($t8)
    sdv     vcf[0], 0x08($t9)
    ldv     vattr[0], 8($a2)

    vmudn   vtmp, vmf3, vconst[1]       // clip = M * (x, y, z, 1)
    ldv     vattr[8], 24($a2)
    vmadh   vtmp, vmi3, vconst[1]
    addiu   $a2, $a2, 32
    vmadn   vtmp, vmf0, vin[0h]
    sll     $a0, $t5, 1
    vmadh   vtmp, vmi0, vin[0h]
    ssv     vrcf[14], 0x18($t8)
    vmadn   vtmp, vmf1, vin[1h]
    ssv     vrcf[6], 0x18($t9)
    vmadh   vtmp, vmi1, vin[1h]
    sh      $t7, 0x1A($t8)
    vmadn   vcf, vmf2, vin[2h]
    sh      $t2, 0x1A($t9)
    vmadh   vci, vmi2, vin[2h]
    // previous pair: z >= 0 (rounding puts near-plane vertices at -1, -2, and the triangle
    // setup needs z deltas inside s16), 1/w integer in lanes 3, 7 before it is replaced
    vge     vzc, vscr, vzero
    sh      $t7, 0($k1)                 // flags of the previous pair
    vmov    vscr[3], vrci[3]
    sh      $t2, 0($k0)
    vmov    vscr[7], vrci[7]
    move    $k0, $gp
    vsub    vfd, vci, vfog[0]           // fog of this pair: w - start (lanes 3, 7)
    // W normalize: clip *= K (16.16: integer in viewport scale lane 3, fraction in trans lane
    // 3). A uniform scale keeps NDC, the clip compares and the guard band; it brings W near 1
    // so that the reciprocal below (2^31 / W as s15.16) keeps its bits. Fog used W unscaled.
    vmudl   vtmp, vcf, vtrans[3h]
    vmadm   vtmp, vci, vtrans[3h]
    vmadn   vcf, vcf, vscale[3h]
    vmadh   vci, vci, vscale[3h]
    // Clip flags (offset 0x1A, K64_KGFX_CLIP_*): 32-bit compares of x, y, z against +-W and
    // of x, y against +-4W (guard band). VCC low byte = C <= -W, high byte = C >= W, one bit
    // per lane (A in lanes 0..3, B in 4..7). Both compares come first, so that the flag
    // decode pairs with the reciprocal.
    vmudn   vtmp, vcf, vconst[4]        // 4W
    addu    $k1, $gp, $a0
    vmadh   v4wi, vci, vconst[4]
    addiu   $gp, $gp, 4
    vmadn   v4wf, vzero, vzero[0]
    sll     $a3, $t5, 5
    vch     vtmp, vci, vci[3h]
    sll     $t5, $t5, 3
    vcl     vtmp, vcf, vcf[3h]
    addu    $a3, $a3, $t5
    vmudm   vtmp, vfd, vfog[2]          // * (Ki + Kf / 65536), clamped to s16
    ssv     vfb[14], 0x22($t8)          // previous pair: b << 8 | fog alpha (B first)
    vmadh   vfd, vfd, vfog[1]
    ssv     vfb[6], 0x22($t9)
    cfc2    $t0, vcc
    vch     vtmp, vci, v4wi[3h]
    vcl     vtmp, vcf, v4wf[3h]
    // 1/w (32-bit reciprocal of the s15.16 w: result = 2^31 / W, i.e. 0.5/w in s15.16)
    vrcph   vrci[3], vci[3]
    cfc2    $t1, vcc
    vrcpl   vrcf[3], vcf[3]
    andi    $t0, $t0, 0xFFFF            // (cfc2 sign-extends)
    // Flags: VCC (+-W) in the low half of $t1, VCC (+-4W) in the high half; each byte's low
    // nibble holds A's bits, its high nibble B's. Packing the four nibbles gives
    // -x -y -z . +x +y +z . -4x -4y . . +4x +4y (K64_KGFX_CLIP_*).
    vrcph   vrci[3], vzero[0]
    addiu   $t3, $t3, -2
    vrcph   vrci[7], vci[7]
    sll     $t1, $t1, 16
    vge     vfd, vfd, vzero             // (VCC is free after the cfc2)
    or      $t1, $t1, $t0
    vrcpl   vrcf[7], vcf[7]
    and     $t2, $t1, $s4               // A: 0x03030707
    vrcph   vrci[7], vzero[0]
    srl     $t7, $t1, 4
    vsubc   vwnf, vzero, vcf            // -w for the Newton-Raphson step
    and     $t7, $t7, $s4               // B
    vsub    vwni, vzero, vci
    srl     $t4, $t2, 4
    vmudn   vrcf, vrcf, vconst[2]       // x2 -> 1/w
    or      $t2, $t2, $t4
    vmadh   vrci, vrci, vconst[2]
    srl     $t5, $t7, 4
    vmadn   vrcf, vzero, vzero[0]
    or      $t7, $t7, $t5
    vmov    vscr[2], vzc[2]             // z >= 0
    and     $t2, $t2, $s5               // 0x00FF00FF
    vmov    vscr[6], vzc[6]
    and     $t7, $t7, $s5
    vmudl   vfd, vfd, vfmul[0]          // * (2 * max + 2) >> 16: 0..max
    // one Newton-Raphson step: x1 = x0 * (2 - w * x0)  (table result is ~11 bits)
    vmudl   vtmp, vwnf, vrcf
    srl     $t4, $t2, 8
    vmadm   vtmp, vwni, vrcf
    or      $t2, $t2, $t4               // (bits 16.. are junk: stored with sh)
    vmadn   vnf, vwnf, vrci
    srl     $t5, $t7, 8
    vmadh   vni, vwni, vrci
    or      $t7, $t7, $t5
    vmudn   vtmp, vrcf, vconst[2]       // x0 * (2 - w * x0) = 2 * x0 + x0 * (-w * x0)
    sdv     vscr[8], 0x10($t8)
    vmadh   vtmp, vrci, vconst[2]
    sdv     vscr[0], 0x10($t9)
    vmadl   vtmp, vrcf, vnf
    vmadm   vtmp, vrci, vnf
    move    $t9, $a1
    vmadn   vrcf, vrcf, vni
    addu    $t8, $a1, $a3
    vmadh   vrci, vrci, vni
    addiu   $a1, $a1, 2 * VCACHE_STRIDE
    bgtz    $t3, vtx_iter
    vand    vfa, vfog, vattr[1h]        // b, a & mask (lanes 3, 7)

vtx_last:                               // stage B of vtx_iter for the last pair
    vmudl   vtmp, vcf, vrcf[3h]
    sdv     vattr[8], 0x20($t8)
    vmadm   vtmp, vci, vrcf[3h]
    sdv     vattr[0], 0x20($t9)
    vmadn   vnf, vcf, vrci[3h]
    sdv     vci[8], 0x00($t8)
    vmadh   vni, vci, vrci[3h]
    sdv     vci[0], 0x00($t9)
    vmudh   vtmp, vtrans, vconst[1]
    sdv     vcf[8], 0x08($t8)
    vmadn   vtmp, vnf, vscale
    sdv     vcf[0], 0x08($t9)
    vmadh   vscr, vni, vscale
    ssv     vrcf[14], 0x18($t8)
    vor     vfb, vfa, vfd
    ssv     vrcf[6], 0x18($t9)
    sh      $t7, 0x1A($t8)
    sh      $t2, 0x1A($t9)
    vge     vtmp, vscr, vzero
    sh      $t7, 0($k1)
    vmov    vscr[3], vrci[3]
    sh      $t2, 0($k0)
    vmov    vscr[7], vrci[7]
    vmov    vscr[2], vtmp[2]
    vmov    vscr[6], vtmp[6]
    sdv     vscr[8], 0x10($t8)
    sdv     vscr[0], 0x10($t9)
    ssv     vfb[14], 0x22($t8)
    j       cmd_next
    ssv     vfb[6], 0x22($t9)


// Lighting pass over INBUF ($t3 vertices, in pairs; keeps $a1, $a2, $t3): replaces each
// vertex color with color * (ambient + sum of max(N . L, 0) * light color), clamped to 2.0.
// The normal is the packed norm field (5.6.5 as tiny3d: x bits 15..11, y 10..5, z 4..0).
//   N . L     normal Q15 (x, z scaled 31/32, y 63/64: the light directions undo it) times
//             the model-space light directions (Q14, one light per lane): 3 MACs, as a
//             matrix transform, with the 4 lights in lanes 0..3 (vertex A) and 4..7 (B)
//   light     Q14 per channel: ambient (Q14) + the 4 light colors (Q15) weighted by N . L; the
//             alpha lane is 1.0 (ambient lane 3), so alpha passes through exactly
//   color     (light * rgba << 7) >> 15, x2: exact for light 1.0
    .def vldx   $v24            // light directions: x of lights 0..3 (twice), Q14
    .def vldy   $v25
    .def vldz   $v26
    .def vlc0   $v27            // light colors r, g, b, 0 (twice), Q15
    .def vlc1   $v28
    .def vlc2   $v29
    .def vlc3   $v30
    .def vlamb  $v31            // ambient r, g, b, 1.0
    .def vnsh   $v14            // normal decode: 1, 32, 2048 (shift), then mask
    .def vnmsk  $v15
    .def vlit   $v8             // light per channel of the previous pair
    .def vcol   $v9             // vertex colors of the previous pair (A lanes 0..3, B 4..7)
    .def vnn    $v5             // normal decode scratch (vattr is free here)
// Software-pipelined over four pairs: each iteration decodes the next pair's normals,
// finishes the previous pair (color * light) and stores the one before it in the latency gaps
// of the current pair's N . L and light chains. The loop runs pairs + 1 times; the second
// iteration stores junk over pair 0's colors after loading them, the last one computes a junk
// pair past the input and decodes / loads the two after it (reads only).
light_verts:
    li      $t8, LIGHTS
    lqv     vldx[0], 0x00($t8)
    lqv     vldy[0], 0x10($t8)
    lqv     vldz[0], 0x20($t8)
    lqv     vlc0[0], 0x30($t8)
    lqv     vlc1[0], 0x40($t8)
    lqv     vlc2[0], 0x50($t8)
    lqv     vlc3[0], 0x60($t8)
    lqv     vlamb[0], 0x70($t8)
    lqv     vnsh[0], 0x80($t8)
    lqv     vnmsk[0], 0x90($t8)
    move    $t7, $t8
    li      $t8, INBUF                  // current pair
    ldv     vin[8], 16($t8)
    ldv     vin[0], 0($t8)
    li      $t9, INBUF                  // pair stored this iteration
    move    $k0, $t9                    // previous pair
    lw      $t1, 8($t9)                 // first store: pair 0's own colors (no-op)
    lw      $t2, 24($t9)
    addiu   $t5, $t3, 3
    srl     $t5, $t5, 1                 // pairs + 1 (an odd count lights one junk slot of INBUF)
    li      $t6, 0xF0                   // VCC: lanes 4..7
    vmudn   vtmp, vnsh, vin[3h]         // pad << 0, 5, 11 in the accumulator low half
    ldv     vin[8], 48($t8)             // pair 1
    vsar    vni, vni, vni[2]
    ldv     vin[0], 32($t8)
    vand    vni, vni, vnmsk             // pair 0 normal x, y, z (Q15)
// Each iteration stores the colors read back at the end of the one before (two pairs back),
// so the suv -> lw -> sw round trip stays off the vector chain; the last one is stored on exit.
light_loop:
    vmulf   vscr, vlit, vcol            // previous pair: light * color
    sw      $t1, 8($t9)
    vmulf   vtmp, vldx, vni[0h]
    sw      $t2, 24($t9)
    vmacf   vtmp, vldy, vni[1h]
    addiu   $t4, $t8, 4
    vmacf   vnf, vldz, vni[2h]          // N . L per light (Q14)
    luv     vci[0], 8($t8)              // A r g b a (u8 << 7)
    vmudn   vtmp, vnsh, vin[3h]         // next pair's normals (vni is consumed above)
    luv     vcf[0], 16($t4)             // B r g b a in lanes 4..7 (bytes 20..27)
    vsar    vnn, vnn, vnn[2]
    ldv     vin[8], 80($t8)             // the pair after
    vadd    vscr, vscr, vscr
    ldv     vin[0], 64($t8)
    vge     vnf, vnf, vzero
    move    $t9, $k0
    vand    vni, vnn, vnmsk
    ctc2    $t6, vcc
    vmrg    vcol, vcf, vci              // vertex colors: A lanes 0..3, B 4..7
    suv     vscr[0], LIGHTTMP - LIGHTS($t7)
    vmudh   vtmp, vlamb, vconst[1]
    move    $k0, $t8
    vmacf   vtmp, vlc0, vnf[0h]
    addiu   $t5, $t5, -1
    vmacf   vtmp, vlc1, vnf[1h]
    lw      $t1, LIGHTTMP + 0($zero)
    vmacf   vtmp, vlc2, vnf[2h]
    lw      $t2, LIGHTTMP + 4($zero)
    vmacf   vlit, vlc3, vnf[3h]         // light per channel (Q14, saturates at 2.0)
    bnez    $t5, light_loop
    addiu   $t8, $t8, 32
    sw      $t1, 8($t9)
    jr      $ra
    sw      $t2, 24($t9)
    .undef vlit
    .undef vcol
    .undef vnn

// Texture coordinate pass over INBUF ($t3 vertices, in pairs; keeps $a1, $a2, $t3): s, t =
// off + scale * (N . axis) / 2, with the two model-space view axes (Q14, lanes 0, 1 for
// vertex A, 4, 5 for B) against the decoded normal as in light_verts.
    .def vnn    $v5             // normal decode scratch (vattr is free here)
// Software-pipelined like light_verts: each iteration stores the previous pair's s, t and
// decodes the next pair's normals in the latency gaps of the current pair. The loop runs
// pairs + 1 times; the first iteration stores junk over pair 0's s, t, the last one computes
// a junk pair past the input (reads only).
texgen_verts:
    li      $t8, TEXGEN
    lqv     vldx[0], 0x00($t8)
    lqv     vldy[0], 0x10($t8)
    lqv     vldz[0], 0x20($t8)
    lqv     vlc0[0], 0x30($t8)          // scale
    lqv     vlc1[0], 0x40($t8)          // offset
    lqv     vnsh[0], 0x50($t8)
    lqv     vnmsk[0], 0x60($t8)
    li      $t8, INBUF                  // current pair
    ldv     vin[0], 0($t8)
    ldv     vin[8], 16($t8)
    move    $t9, $t8                    // previous pair (stores)
    addiu   $t5, $t3, 3
    srl     $t5, $t5, 1                 // pairs + 1
    vmudn   vtmp, vnsh, vin[3h]
    ldv     vin[0], 32($t8)             // pair 1
    vsar    vni, vni, vni[2]
    ldv     vin[8], 48($t8)
    vand    vni, vni, vnmsk             // pair 0 normal x, y, z (Q15)
texgen_loop:
    vmulf   vtmp, vldx, vni[0h]
    slv     vnf[0], 12($t9)
    vmacf   vtmp, vldy, vni[1h]
    slv     vnf[8], 28($t9)
    vmacf   vnf, vldz, vni[2h]          // N . axis (Q14)
    move    $t9, $t8
    vmudn   vtmp, vnsh, vin[3h]         // next pair's normals
    ldv     vin[0], 64($t8)             // the pair after
    vsar    vnn, vnn, vnn[2]
    ldv     vin[8], 80($t8)
    vmudh   vtmp, vlc1, vconst[1]
    addiu   $t5, $t5, -1
    vmacf   vnf, vlc0, vnf              // s, t (s10.5)
    vand    vni, vnn, vnmsk
    bnez    $t5, texgen_loop
    addiu   $t8, $t8, 32
    jr      $ra
    nop
    .undef vnn

    .balign 8
    .global OVL_VTX_END
OVL_VTX_END:
    .assert OVL_CLIP_END - OVL_SLOT <= OVL_VTX_END - OVL_SLOT, "clip overlay larger than the slot"
    .assert OVL_AUDIO_END - OVL_SLOT <= OVL_VTX_END - OVL_SLOT, "audio overlay larger than the slot"
    .assert OVL_ADPCM_END - OVL_SLOT <= OVL_VTX_END - OVL_SLOT, "adpcm overlay larger than the slot"
    .assert OVL_PART_END - OVL_SLOT <= OVL_VTX_END - OVL_SLOT, "particle overlay larger than the slot"

// TRI: w0 bits 23..0 = vertex cache slots a, b, c; w1 = a second triangle in the same form,
// present when its bits 31..24 are non-zero.
cmd_tri:
    ori     $s7, $zero, cmd_tri_2       // tri returns to $s7
    j       tri
    move    $s4, w1
cmd_tri_2:
    srl     $t0, $s4, 24
    beqz    $t0, cmd_next
    move    w0, $s4                     // reloaded at cmd_next
    j       tri                         // tail call: returns straight to cmd_next
    ori     $s7, $zero, cmd_next

// ---- triangle setup --------------------------------------------------------------------------
// Same math and output as k64_rdp_tri (src/gfx/tri.c), in fixed point. The method follows
// libdragon's rsp_rdpq_tri.inc (Giovanni Bajo, Unlicense):
//   - sort by y; edges H (1->3), M (1->2), L (2->3) in s13.2
//   - NZ = MX*HY - HX*MY (32-bit, .4); slopes and 1/NZ by vrcp + one Newton-Raphson step
//   - attribute plane: DX = (MA*HY - HA*MY)/NZ, DY = (HA*MX - MA*HX)/NZ, DE = DY + DX*ISH,
//     base = A1 + DE*FY with FY = floor(y1) - y1
//   - perspective: 1/w scaled by the smallest W (largest 1/w ~ 1.0 in 0.15), S/T times it
//     as 16.16 (the fraction matters where vn is small)
// Limits: S/T deltas inside a triangle must fit s16; 1/NZ saturates on sub-pixel slivers.
    .def vhml   $v4             // HX HY MX MY LX LY
    .def vmm    $v5             // MX MY (lanes 0, 1)
    .def vneg   $v6             // -vhml
    .def vnzi   $v7
    .def vnzf   $v8
    .def vsx    $v8             // slope numerators: vhml, 0 for horizontal edges (after NZ)
    .def vri    $v9             // reciprocals: lanes 0/2/4 = 1/HY 1/MY 1/LY, lane 7 = 1/NZ
    .def vrf    $v10
    .def vxi    $v11            // reciprocal inputs
    .def vxf    $v12
    .def vti    $v13
    .def vtf    $v14
    .def vjunk  $v15
    .def vsli   $v24            // slopes: lane 0 ISH, 2 ISM, 4 ISL (s15.16)
    .def vslf   $v25
    .def vedi   $v26            // edges: lane 0 XH, 2 XM
    .def vedf   $v27
    .def vfy    $v28            // lanes 0/1 = FY int/frac, 2/3 = X1 int/frac
    .def va1    $v29            // per-vertex attributes: r g b a s t w z
    .def va2    $v30
    .def va3    $v31

    .def p1     $a1             // sorted vertices (VCACHE addresses)
    .def p2     $a2
    .def p3     $a3
    .def lft    $s5             // major edge on the left

// Triangle p1, p2, p3 (VCACHE addresses of projected vertices) -> RDP command; returns to $s7.
// tri itself (w0 = slots) is entered with $s7 set by the caller.
// Entry for the clipper: the face-culling product, then the common path.
tri_go:
    llv     vhml[0], 0x10(p2)
    llv     vtf[0], 0x10(p1)
    llv     vmm[0], 0x10(p3)
    vsub    vneg, vtf, vhml
    vsub    vhml, vhml, vtf
    vsub    vmm, vmm, vtf
    vmudh   vjunk, vhml, vmm[1]
    vmadh   vnzi, vmm, vneg[1]
    j       tri_check
    lw      $t5, OUTLIMIT($zero)

tri:
    // vertex references are DMEM addresses / 8 (VREF_BASE + slot * 5): address = ref * 8
    srl     $t1, w0, 5
    andi    p2, $t1, 0x7F8
    srl     $t0, w0, 13
    andi    p1, $t0, 0x7F8

    // Face culling, computed before the sort and the reciprocals and interleaved with the
    // scalar trivial-reject / clip test below: back <=> the submitted winding is clockwise
    // on the y-down screen, cross(p2 - p1, p3 - p1) > 0. Only the high half is tested:
    // front <=> negative; zero area counts as back (and is dropped below anyway).
    // All three vertices outside one frustum plane: invisible. Any vertex past the near or
    // far plane or the guard band: clip. Otherwise the RDP scissor handles the screen edges.
    // Both need a flag in the OR of the three, so the fast path tests only that; triangles
    // with a vertex off screen take tri_rejclip for the AND (and come back if visible).
    llv     vhml[0], 0x10(p2)           // p1, p2 first: their loads overlap the p3 decode
    llv     vtf[0], 0x10(p1)
    sll     $t2, w0, 3
    andi    p3, $t2, 0x7F8
    llv     vmm[0], 0x10(p3)
    lhu     $t0, 0x1A(p1)
    vsub    vneg, vtf, vhml             // p1 - p2
    lhu     $t1, 0x1A(p2)
    vsub    vhml, vhml, vtf             // e1 = p2 - p1 (lanes 0 x, 1 y)
    lhu     $t2, 0x1A(p3)
    lw      $t7, CULLMODE($zero)
    vsub    vmm, vmm, vtf               // e2 = p3 - p1
    or      $t4, $t0, $t1
    or      $t4, $t4, $t2
    andi    $t4, $t4, CLIP_REJECT | CLIP_PLANES_MASK
    vmudh   vjunk, vhml, vmm[1]         // e1x*e2y + e2x*(p1y - p2y); the clamped high half
    vmadh   vnzi, vmm, vneg[1]          // has the sign of the sum
    bnez    $t4, tri_rejclip            // off screen: rejected, clipped or back to tri_vis
    lw      $t5, OUTLIMIT($zero)
tri_vis:
    lh      $t0, 0x12(p1)
    lh      $t1, 0x12(p2)
    mfc2    $t6, vnzi[0]
    lh      $t2, 0x12(p3)
    // cull first: a culled triangle ends here. sra gives shift 0 (back) or 31 (front), which
    // brings CULLMODE bit 31 or bit 0 to the sign.
    sra     $t6, $t6, 31
    sllv    $t4, $t7, $t6
    bltz    $t4, tri_done
    sltu    $t5, $t5, outptr            // no room for TRI_MAX bytes in this OUTBUF half
    bnez    $t5, tri_flush
    // Sort by screen y (same comparisons as k64_rdp_tri).
    slt     $t3, $t1, $t0
tri_sort:
    beqz    $t3, tri_s1
    move    $t4, p1                    // $t4 is dead at tri_s1
    move    p1, p2
    move    p2, $t4
    move    $t4, $t0
    move    $t0, $t1
    move    $t1, $t4
tri_s1:
    slt     $t3, $t2, $t1
    beqz    $t3, tri_s2
    move    $t4, p2                    // $t4 is dead at tri_s2
    move    p2, p3
    move    p3, $t4
    move    $t4, $t1
    move    $t1, $t2
    move    $t2, $t4
tri_s2:
    slt     $t3, $t1, $t0
    beqz    $t3, tri_s3
    move    $t4, p1                    // $t4 is dead at tri_s3
    move    p1, p2
    move    p2, $t4
    move    $t4, $t0
    move    $t0, $t1
    move    $t1, $t4
tri_s3:
    // From here on the code is scheduled for SU/VU dual issue (tools/rsp_cycles.py): scalar
    // work that does not depend on the vector chain is interleaved to fill vector latencies.
    // Every vector computational op writes the accumulator, so no other vector op may sit
    // inside a multiply-accumulate chain (vmud* .. vmad* .. vsar).
    llv     vhml[0], 0x10(p3)           // edge vectors
    vxor    vxi, vzero, vzero
    llv     vhml[4], 0x10(p2)
    vxor    vxf, vzero, vzero
    llv     vhml[8], 0x10(p3)
    llv     vtf[0], 0x10(p1)
    llv     vtf[4], 0x10(p1)
    llv     vtf[8], 0x10(p2)
    llv     vmm[0], 0x10(p2)
    andi    $t5, $t2, 0x3FFF            // header y fields (s11.2): YL = y3, YM = y2, YH = y1
    andi    $t6, $t1, 0x3FFF
    andi    $t7, $t0, 0x3FFF
    vsub    vneg, vtf, vhml             // -(edge vectors), without waiting for them
    sh      $t5, 2(outptr)
    vsub    vhml, vhml, vtf
    sh      $t6, 4(outptr)
    vsub    vmm, vmm, vtf
    andi    $t3, $t0, 3                 // FY = floor(y1) - y1 = -frac(y1) and X1, both s15.16
    sll     $t3, $t3, 14
    // NZ first: the scalar NZ tests at the end wait on it, the reciprocals are not needed
    // before tri_nz.
    // Scalar memory ops are ordered so no store (sh, ssv, mtc2) issues two cycles after a
    // load (lh, lw, mtc2): that costs a stall cycle.
    subu    $t4, $zero, $t3
    vmudh   vjunk, vmm, vhml[1]         // NZ = MX*HY - HX*MY
    mtc2    $t4, vfy[2]
    vmadh   vjunk, vhml, vneg[3]
    sltu    $t5, $zero, $t3
    vsar    vnzi, vjunk, vjunk[0]
    subu    $t5, $zero, $t5
    vsar    vnzf, vjunk, vjunk[1]
    // Reciprocals of HY, MY, LY (16-bit) and NZ (32-bit), kept at half scale (2^31/x): the
    // full 2^32/x does not fit 32 bits for x = 1, 2 (edges a quarter / half pixel tall).
    mtc2    $t5, vfy[0]
    vrcp    vrf[0], vhml[1]
    sh      $t7, 6(outptr)
    vrcph   vri[0], vhml[1]
    lh      $t4, 0x10(p1)
    vrcp    vrf[2], vhml[3]
    ssv     vnzi[0], 12(outptr)         // NZ as 32 bits, through the (later) ISL slot
    vrcph   vri[2], vhml[3]
    lh      $t3, 0x10(p2)               // XL = X2
    vrcp    vrf[4], vhml[5]
    ssv     vnzf[0], 14(outptr)
    vrcph   vri[4], vhml[5]
    lw      $t2, 12(outptr)             // NZ
    vmov    vxf[0], vhml[1]             // reciprocal inputs
    sra     $t6, $t4, 2
    vmov    vxf[2], vhml[3]
    andi    $t4, $t4, 3
    vmov    vxf[4], vhml[5]
    sra     $t1, $t2, 31
    sll     $t4, $t4, 14
    mtc2    $t6, vfy[4]
    mtc2    $t4, vfy[6]
    veq     vjunk, vxf, vzero           // horizontal edge (HY = 0): slope 0
    beqz    $t2, tri_done               // degenerate
    slt     lft, $zero, $t2             // NZ > 0
    // NZ normalized to NZ' = NZ >> k with |NZ'| < 2^16: 2^31/NZ' then keeps 15-16 significant
    // bits whatever the size (guard-band triangles reach NZ ~ 2^24, where 2^31/NZ has 7).
    // The attribute numerators are shifted by k too (x 0x10000>>k, k stays in $t6): their
    // truncation costs ~1/|NZ'| of a step per pixel, below the s15.16 lsb.
    xor     $t1, $t2, $t1               // ~NZ when negative: same bit length as |NZ| - 1
    vmrg    vsx, vzero, vhml
    lw      $t9, GEOMMODE($zero)        // op << 24 | level << 19 | tile << 16
    vaddc   vtf, vxf, vxf               // -2x for the Newton-Raphson step (lane 7 in tri_nz)
    sra     $t6, $t3, 2
    vadd    vti, vxi, vxi
    andi    $t3, $t3, 3
    vor     vedi, vzero, vfy[2]         // X1 in every lane: XH, XM start from it
    srl     $t5, $t1, 16
    vor     vedf, vzero, vfy[3]
    sll     $t3, $t3, 14
    vsubc   vtf, vzero, vtf
    sh      $t6, 8(outptr)
    vsub    vti, vzero, vti
    sh      $t3, 10(outptr)
    bnez    $t5, tri_nz_k               // |NZ| >= 2^16 (rare: over ~2048 pixels)
    li      $t6, 0                      // k
tri_nz:                                 // $t2 = NZ' (k = 0 falls through, tri_nz_k jumps here)
    sra     $t1, $t2, 16
    mtc2    $t1, vxi[14]                // NZ' in lane 7 of the reciprocal inputs
    mtc2    $t2, vxf[14]
    sll     $t7, $t2, 1
    subu    $t7, $zero, $t7
    sra     $t5, $t7, 16
    vrcph   vri[7], vxi[7]
    mtc2    $t7, vtf[14]                // -2 NZ'
    vrcpl   vrf[7], vxf[7]
    mtc2    $t5, vti[14]
    vrcph   vri[7], vzero[0]
    srl     $t0, $t9, 16                // command header
    sll     $t1, lft, 7
    or      $t0, $t0, $t1

    // Newton-Raphson: r1 = r0 * (2 - 2 * x * r0)
    vmudl   vjunk, vtf, vrf
    sh      $t0, 0(outptr)              // (before the loads: no store two cycles after one)
    vmadm   vjunk, vti, vrf
    lh      $t2, 0x06(p2)               // W of the three vertices (s15.16), for the attributes
    vmadn   vtf, vtf, vri
    lhu     $t4, 0x0E(p2)
    vmadh   vti, vti, vri
    lh      $t3, 0x06(p3)
    vmudn   vjunk, vrf, vconst[2]       // r0 * (2 + t) = 2 * r0 + r0 * t
    lhu     $t5, 0x0E(p3)
    vmadh   vjunk, vri, vconst[2]
    lh      $t0, 0x06(p1)
    vmadl   vjunk, vrf, vtf
    lhu     $t1, 0x0E(p1)
    vmadm   vjunk, vri, vtf
    sll     $t2, $t2, 16
    vmadn   vrf, vrf, vti
    or      $t2, $t2, $t4
    vmadh   vri, vri, vti
    lsv     $v7[0], 0x16(p1)            // vinvi: 1/w of the vertices (s15.16), for the attributes
    addiu   $t8, outptr, 32             // (both in the wait for vrf / vri)
    sll     $t3, $t3, 16

    // slopes = (HX, MX, LX) / (HY, MY, LY) in s15.16 (0 for a horizontal edge); the product
    // is accumulated twice to undo the half-scale reciprocal
    vmudn   vjunk, vrf, vsx
    or      $t3, $t3, $t5
    vmadh   vjunk, vri, vsx
    sll     $t0, $t0, 16
    vmadn   vjunk, vrf, vsx
    or      $t0, $t0, $t1
    vmadh   vjunk, vri, vsx
    srl     $t9, $t9, 24
    vsar    vslf, vjunk, vjunk[1]
    vsar    vsli, vjunk, vjunk[0]
    andi    $t9, $t9, 7                 // K64_TRI_* flags
    // XH, XM = X1 + slope * FY, X1 accumulated first (in the slope latency). The scalar side
    // starts the smallest W for the attributes, branch-free (W > 0, so W2 - W1 cannot
    // overflow): t0 = min(W1, W2).
    vmudn   vjunk, vedf, vconst[1]
    subu    $t5, $t2, $t0
    vmadh   vjunk, vedi, vconst[1]
    sra     $t4, $t5, 31
    vmadl   vjunk, vslf, vfy[1]
    and     $t5, $t5, $t4
    vmadm   vjunk, vsli, vfy[1]
    addu    $t0, $t0, $t5
    vmadn   vedf, vslf, vfy[0]
    ssv     vsli[8], 12(outptr)         // ISL
    vmadh   vedi, vsli, vfy[0]
    ssv     vslf[8], 14(outptr)
    ssv     vsli[0], 20(outptr)         // ISH
    // 1/NZ x8 (x2 for the half scale, x4 puts DX/DY below in s15.16)
    vmudn   vjunk, vrf, vconst[4]
    ssv     vslf[0], 22(outptr)
    vmadh   vjunk, vri, vconst[4]
    ssv     vsli[4], 28(outptr)         // ISM
    vmadn   vjunk, vrf, vconst[4]
    ssv     vslf[4], 30(outptr)
    vmadh   vri, vri, vconst[4]
    ssv     vedi[0], 16(outptr)         // XH
    vmadn   vrf, vzero, vzero[0]
    ssv     vedf[0], 18(outptr)
    ssv     vedi[4], 24(outptr)         // XM
    ssv     vedf[4], 26(outptr)
    beqz    $t9, tri_emit
    subu    $t5, $t3, $t0

    // ---- attributes ----
    .def vw     $v5             // smallest W: lane 0 int, 1 frac
    .def vinvi  $v7
    .def vinvf  $v8
    .def vn     $v13            // 1/w * minW in 0.15, lanes 0..2 = vertices 1..3
    .def vt1    $v14
    .def vt2    $v26
    .def vt3    $v27
    .def vf1    $v11            // s/w, t/w fractions (lanes 4, 5; 0 elsewhere): vertex 1
    .def vf2    $v12            // vertex 2
    .def vf3    $v14            // vertex 3 (vt1 after its merge)
tri_w2:
    sra     $t4, $t5, 31                // t0 = min(t0, W3)
    vxor    vt1, vzero, vzero           // s, t land in lanes 4, 5: the s/w, t/w fractions
    and     $t5, $t5, $t4               // must come out 0 in the other lanes
    vxor    vt2, vzero, vzero
    addu    $t0, $t0, $t5
    vxor    vt3, vzero, vzero
    lsv     vinvi[2], 0x16(p2)
    addiu   $t0, $t0, -1                // (minW - lsb) / 2: every 1/w * that is < 0.5, i.e.
    srl     $t0, $t0, 1                 // directly 0.15 below 1.0 (no clamp, no shift)
    srl     $t1, $t0, 16
    mtc2    $t1, vw[0]
    mtc2    $t0, vw[2]
    lsv     vinvi[4], 0x16(p3)
    lsv     vinvf[0], 0x18(p1)
    lsv     vinvf[2], 0x18(p2)
    lsv     vinvf[4], 0x18(p3)
    luv     va1[0], 0x20(p1)            // r g b a (as u8 << 7)
    luv     va2[0], 0x20(p2)
    luv     va3[0], 0x20(p3)
    vmudl   vjunk, vinvf, vw[1]
    llv     vt1[8], 0x24(p1)            // s t -> lanes 4, 5
    vmadm   vjunk, vinvi, vw[1]
    llv     vt2[8], 0x24(p2)
    vmadn   vn, vinvf, vw[0]            // the integer x integer term is 0 (W >= 1 or minW < 2)
    li      $t0, 0xCF                   // (ctc2 two cycles after a load would stall)
    vmudl   va1, va1, vconst[5]         // >> 7
    llv     vt3[8], 0x24(p3)
    vmudl   va2, va2, vconst[5]
    ctc2    $t0, vcc
    vmudl   va3, va3, vconst[5]
    // s/w, t/w = s * vn as 16.16 (integer s10.5 + 16 fraction bits): far from the smallest W
    // vn is small (W ratio 400: vn ~ 41) and a rounded integer product would be off by up
    // to half a unit, tens of percent of the coordinate. The RDP divides S/W by W per pixel,
    // so with both made from the same vn its own rounding cancels out.
    // vn is 0.15 up to 0x7FFF (no room to double it): accumulate s * vn twice.
    vmudm   vf1, vt1, vn[0]
    vmadm   vt1, vt1, vn[0]
    lsv     va1[14], 0x14(p1)           // z
    vsar    vf1, vf1, vf1[2]
    lsv     va2[14], 0x14(p2)
    vmudm   vf2, vt2, vn[1]
    lsv     va3[14], 0x14(p3)
    vmadm   vt2, vt2, vn[1]
    vsar    vf2, vf2, vf2[2]
    vmrg    va1, va1, vt1               // (vmrg writes the accumulator: after the vsar)
    vmov    va1[6], vn[0]               // W
    vmudm   vf3, vt3, vn[2]
    vmadm   vt3, vt3, vn[2]
    vsar    vf3, vf3, vf3[2]
    vmrg    va2, va2, vt2
    vmov    va2[6], vn[1]
    vmrg    va3, va3, vt3
    vmov    va3[6], vn[2]
    .undef vw
    .undef vinvi
    .undef vinvf
    .undef vn
    .undef vt1
    .undef vt2
    .undef vt3

    .def vdxi   $v7
    .def vdxf   $v8
    .def vdyi   $v5
    .def vdyf   $v14
    .def vdei   $v26
    .def vdef   $v27
    .def vfini  $v13
    .def vfinf  $v30
    // DX = MA*HY - HA*MY = A1*(MY - HY) + A2*HY - A3*MY with MY - HY = -LY, and
    // DY = HA*MX - MA*HX = A1*(HX - MX) + A3*MX - A2*HX with HX - MX = LX: no A2 - A1,
    // A3 - A1 differences (nor their latency, nor their saturation).
    vmudn   vjunk, vf1, vneg[5]         // the s/w, t/w fractions (accumulator bit 0) first:
    lbu     $t0, TRIOFS($zero)          // block addresses: shade $t0, texture $t1, z $t2
    vmadn   vjunk, vf2, vhml[1]         // va3 is written last
    lbu     $t1, TRIOFS+1($zero)
    vmadn   vjunk, vf3, vneg[3]
    lbu     $t2, TRIOFS+2($zero)
    vmadh   vjunk, va1, vneg[5]
    vmadh   vjunk, va2, vhml[1]
    vmadh   vjunk, va3, vneg[3]
    vsar    vdxf, vjunk, vjunk[1]
    lbu     $t3, TRISIZE($zero)
    vsar    vdxi, vjunk, vjunk[0]
    addu    $t0, $t0, outptr
    vmudh   vjunk, va1, vhml[4]
    addu    $t1, $t1, outptr
    vmadh   vjunk, va3, vhml[2]
    addu    $t2, $t2, outptr
    vmadh   vjunk, va2, vneg[0]
    addu    outptr, outptr, $t3
    vmadn   vjunk, vf1, vhml[4]
    vmadn   vjunk, vf3, vhml[2]
    vmadn   vjunk, vf2, vneg[0]
    vsar    vdyf, vjunk, vjunk[1]
    vsar    vdyi, vjunk, vjunk[0]
    beqz    $t6, tri_nok
    vmudl   vjunk, vdxf, vfy[4]         // DX, DY >> k (see the NZ normalization); tri_nok
                                        // starts a new accumulator chain
    vmadm   vjunk, vdxi, vfy[4]
    vmadh   vdxi, vzero, vzero[0]
    vmadn   vdxf, vzero, vzero[0]
    vmudl   vjunk, vdyf, vfy[4]
    vmadm   vjunk, vdyi, vfy[4]
    vmadh   vdyi, vzero, vzero[0]
    vmadn   vdyf, vzero, vzero[0]
tri_nok:
    // Every block is stored unconditionally (a disabled one to scratch, see TRIOFS), the
    // stores paired with the chains.
    vmudl   vjunk, vdxf, vrf[7]         // / NZ'
    vmadm   vjunk, vdxi, vrf[7]
    vmadn   vdxf, vdxf, vri[7]
    vmadh   vdxi, vdxi, vri[7]
    vmudl   vjunk, vdyf, vrf[7]
    vmadm   vjunk, vdyi, vrf[7]
    vmadn   vdyf, vdyf, vri[7]
    sdv     vdxf[0], 0x18($t0)
    vmadh   vdyi, vdyi, vri[7]
    sdv     vdxi[0], 0x08($t0)
    vmadl   vjunk, vdxf, vslf[0]        // DE = DY + DX * ISH (accumulator still holds DY)
    sdv     vdxf[8], 0x18($t1)
    vmadm   vjunk, vdxi, vslf[0]
    sdv     vdxi[8], 0x08($t1)
    vmadn   vdef, vdxf, vsli[0]
    ssv     vdxf[14], 0x06($t2)
    vmadh   vdei, vdxi, vsli[0]
    ssv     vdxi[14], 0x04($t2)
    vmudh   vjunk, va1, vconst[1]       // base = A1 + DE * FY
    vmadn   vjunk, vf1, vconst[1]
    sdv     vdyf[0], 0x38($t0)
    vmadl   vjunk, vdef, vfy[1]
    sdv     vdyi[0], 0x28($t0)
    vmadm   vjunk, vdei, vfy[1]
    sdv     vdyf[8], 0x38($t1)
    vmadn   vfinf, vdef, vfy[0]
    sdv     vdyi[8], 0x28($t1)
    vmadh   vfini, vdei, vfy[0]
    ssv     vdyf[14], 0x0E($t2)
    ssv     vdyi[14], 0x0C($t2)
    addiu   ntri, ntri, 1
    sdv     vdef[0], 0x30($t0)
    sdv     vdei[0], 0x20($t0)
    sdv     vdef[8], 0x30($t1)
    sdv     vdei[8], 0x20($t1)
    ssv     vdef[14], 0x0A($t2)
    ssv     vdei[14], 0x08($t2)
    sdv     vfinf[0], 0x10($t0)
    sdv     vfini[0], 0x00($t0)
    sdv     vfinf[8], 0x10($t1)
    sdv     vfini[8], 0x00($t1)
    ssv     vfinf[14], 0x02($t2)
    jr      $s7
    ssv     vfini[14], 0x00($t2)
    .undef vdxi
    .undef vdxf
    .undef vdyi
    .undef vdyf
    .undef vdei
    .undef vdef
    .undef vfini
    .undef vfinf
    .undef vf1
    .undef vf2
    .undef vf3
tri_emit:
    move    outptr, $t8
    jr      $s7
    addiu   ntri, ntri, 1
tri_done:
    jr      $s7
    nop

// Out of line: tri_go (clip fan) and the OUTBUF flush enter the buffer test and the cull here.
tri_flush:
    jal     flush_out                   // keeps a1..a3 and the vector registers
    nop
    j       tri_room
    nop
tri_check:                              // $t5 = OUTLIMIT
    sltu    $t0, $t5, outptr
    beqz    $t0, tri_room_nz
    mfc2    $t3, vnzi[0]
    jal     flush_out
    nop
tri_room:
    // With culling off the test costs the same and passes. The sort's y loads fill the
    // mfc2 latency.
    mfc2    $t3, vnzi[0]
tri_room_nz:
    lw      $t7, CULLMODE($zero)
    lh      $t0, 0x12(p1)
    lh      $t1, 0x12(p2)
    sra     $t3, $t3, 31
    lh      $t2, 0x12(p3)
    sllv    $t4, $t7, $t3
    bltz    $t4, tri_done
    slt     $t3, $t1, $t0
    j       tri_sort
    nop

tri_nz_k:                               // k = bit length of |NZ| >> 16 (<= 15), branch-free
    srl     $t1, $t5, 8
    sltu    $t1, $zero, $t1
    sll     $t6, $t1, 3                 // 8 if >= 0x100
    srlv    $t5, $t5, $t6
    srl     $t1, $t5, 4
    sltu    $t1, $zero, $t1
    sll     $t1, $t1, 2                 // 4 if >= 0x10
    srlv    $t5, $t5, $t1
    lbu     $t5, NZLUT($t5)             // bit length of 0..15
    addu    $t6, $t6, $t1
    addu    $t6, $t6, $t5
    li      $t5, 0x10000
    srlv    $t5, $t5, $t6
    mtc2    $t5, vfy[8]                 // lane 4: numerator scale (only read when k != 0)
    j       tri_nz
    srav    $t2, $t2, $t6

// ---- clipping --------------------------------------------------------------------------------
// Entered from tri with $t4 = the planes the triangle crosses, $s7 = return address.
// Sutherland-Hodgman in clip space (s15.16) against those planes, in CLIP_PLANE_TABLE order;
// new vertices go to scratch cache entries 32..43 (over INBUF, dead after VTX) and are
// projected like vtx_loop does. The polygon is then drawn as a fan (v0, vi, vi+1) through
// tri_go: the winding is kept, so culling still works.
// Plane distances are exact 32-bit (clip coordinates must keep |W| + |C| < 32768); a new
// vertex is always interpolated from the inside vertex towards the outside one, so an edge
// shared by two triangles is cut at the same point in both.
// Kept across the fan: k0 list, k1 count, gp index, fp return, s4 (cmd_tri's second triangle).
    .overlay clip, OVL_SLOT
tri_clip:
    move    $fp, $s7
    move    $s5, $t4
    li      $k0, CLIP_LISTS
    srl     $t0, w0, 16
    sb      $t0, 0($k0)
    srl     $t0, w0, 8
    sb      $t0, 1($k0)
    sb      w0, 2($k0)
    li      $k1, 3                      // vertices in the current list
    li      $gp, VREF_BASE + VCACHE_COUNT * 5   // next scratch entry (references: address / 8)
    li      $s6, CLIP_PLANE_TABLE
clip_plane:
    lhu     $t0, 0($s6)
    and     $t0, $t0, $s5
    beqz    $t0, clip_plane_next
    nop
    lbu     $a0, 2($s6)                 // byte offset of the coordinate (x 0, y 2, z 4)
    lbu     $s7, 3($s6)                 // mode: 1 = W - C, 2 = 4W
    xori    $t9, $k0, 0x10              // output list
    li      $t8, 0                      // output count
    addu    $t0, $k0, $k1
    lbu     $a2, -1($t0)                // previous vertex = the last one
    jal     clip_dist
    move    $t0, $a2
    move    $a3, $t1                    // its distance
    li      $a1, 0
clip_edge:
    addu    $t0, $k0, $a1
    lbu     $t6, 0($t0)                 // current vertex
    jal     clip_dist
    move    $t0, $t6
    move    $t7, $t1
    slt     $t2, $a3, $zero             // previous outside
    slt     $t3, $t7, $zero             // current outside
    beq     $t2, $t3, clip_keep
    nop
    bnez    $t2, clip_enter
    nop
    move    $t0, $a2                    // leaving: inside = previous
    move    $t1, $t6
    move    $t2, $a3
    jal     clip_isect
    move    $t3, $t7
    j       clip_keep
    nop
clip_enter:
    move    $t0, $t6                    // entering: inside = current
    move    $t1, $a2
    move    $t2, $t7
    jal     clip_isect
    move    $t3, $a3
clip_keep:
    bltz    $t7, clip_edge_next
    addu    $t0, $t9, $t8
    sb      $t6, 0($t0)
    addiu   $t8, $t8, 1
clip_edge_next:
    move    $a2, $t6
    move    $a3, $t7
    addiu   $a1, $a1, 1
    bne     $a1, $k1, clip_edge
    nop
    move    $k0, $t9
    move    $k1, $t8
    sltiu   $t0, $k1, 3
    bnez    $t0, clip_out               // nothing left
    nop
clip_plane_next:
    addiu   $s6, $s6, 4
    li      $t0, CLIP_PLANE_TABLE + 24
    bne     $s6, $t0, clip_plane
    nop

    li      $gp, 1
clip_fan:
    lbu     $t0, 0($k0)
    addu    $t1, $k0, $gp
    lbu     $t2, 0($t1)
    lbu     $t3, 1($t1)
    sll     p1, $t0, 3
    sll     p2, $t2, 3
    jal     tri_sub
    sll     p3, $t3, 3
    addiu   $gp, $gp, 1
    addiu   $t0, $k1, -1
    bne     $gp, $t0, clip_fan
    nop
clip_out:
    jr      $fp
    nop

tri_sub:
    j       tri_go
    move    $s7, $ra

// $t0 = vertex reference -> $t1 = plane distance, s15.16: W +- C, or W +- C/4 for the guard
// band (the 4W plane scaled by 1/4: same crossing point, no overflow). Clobbers t0, t2, t3.
clip_dist:
    sll     $t0, $t0, 3
    addu    $t2, $t0, $a0
    lh      $t1, 0x00($t2)              // C
    lhu     $t3, 0x08($t2)
    sll     $t1, $t1, 16
    or      $t1, $t1, $t3
    andi    $t3, $s7, 2
    beqz    $t3, clip_dist_sign
    andi    $t3, $s7, 1
    sra     $t1, $t1, 2
clip_dist_sign:
    beqz    $t3, clip_dist_w
    lh      $t2, 0x06($t0)              // W
    subu    $t1, $zero, $t1
clip_dist_w:
    lhu     $t3, 0x0E($t0)
    sll     $t2, $t2, 16
    or      $t2, $t2, $t3
    jr      $ra
    addu    $t1, $t1, $t2

// New vertex where the edge in -> out crosses the plane, appended to the output list.
// $t0 = inside reference, $t1 = outside reference, $t2 = inside distance (>= 0), $t3 =
// outside distance (< 0). Clobbers t0-t5 and v4-v13.
clip_isect:
    sltiu   $t4, $gp, VREF_BASE + (VCACHE_COUNT + CLIP_POOL_COUNT) * 5
    beqz    $t4, clip_out               // pool exhausted (degenerate input): drop
    sll     $t0, $t0, 3
    sll     $t1, $t1, 3
    ldv     vci[0], 0x00($t0)           // inside: clip int/frac, r g b a (<< 7), s t
    ldv     vcf[0], 0x08($t0)
    luv     vattr[0], 0x20($t0)
    llv     vattr[8], 0x24($t0)
    ldv     vni[0], 0x00($t1)           // outside
    ldv     vnf[0], 0x08($t1)
    luv     vin[0], 0x20($t1)
    llv     vin[8], 0x24($t1)
    // t = d_in / (d_in - d_out) in 0.16, restoring division (needs d_in - d_out < 2^31)
    subu    $t3, $t2, $t3
    bgez    $t3, clip_div_ok
    li      $t4, 16
    srl     $t3, $t3, 1
    srl     $t2, $t2, 1
clip_div_ok:
    li      $t5, 0
clip_div:
    sll     $t2, $t2, 1
    sltu    $t0, $t2, $t3
    xori    $t0, $t0, 1
    sll     $t5, $t5, 1
    or      $t5, $t5, $t0
    beqz    $t0, clip_div_next
    addiu   $t4, $t4, -1
    subu    $t2, $t2, $t3
clip_div_next:
    bnez    $t4, clip_div
    nop
    mtc2    $t5, vtmp[0]
    // new = in + (out - in) * t: clip in 32 bits, attributes in 16 (S/T deltas must fit s16)
    vsubc   vnf, vnf, vcf
    vsub    vni, vni, vci
    vsub    vin, vin, vattr
    vmudl   vscr, vnf, vtmp[0]
    vmadm   vscr, vni, vtmp[0]
    vmadn   vscr, vcf, vconst[1]
    vmadh   vci, vci, vconst[1]
    vmadn   vcf, vzero, vzero[0]
    vmudm   vin, vin, vtmp[0]
    vadd    vattr, vattr, vin
    sll     $t0, $gp, 3
    sdv     vci[0], 0x00($t0)
    sdv     vcf[0], 0x08($t0)
    suv     vattr[0], 0x20($t0)
    slv     vattr[8], 0x24($t0)
    sh      $zero, 0x1A($t0)
    addu    $t1, $t9, $t8
    sb      $gp, 0($t1)
    addiu   $t8, $t8, 1
    addiu   $gp, $gp, 5
    mfc2    $t1, vci[6]                 // W <= 0 cannot be projected (degenerate): drop
    mfc2    $t2, vcf[6]
    bltz    $t1, clip_out
    or      $t2, $t2, $t1
    beqz    $t2, clip_out
    nop
    // projection, as in vtx_loop (lane 3 = W)
    vrcph   vrci[3], vci[3]
    vrcpl   vrcf[3], vcf[3]
    vrcph   vrci[3], vzero[0]
    vmudn   vrcf, vrcf, vconst[2]
    vmadh   vrci, vrci, vconst[2]
    vmadn   vrcf, vzero, vzero[0]
    vmudl   vtmp, vcf, vrcf
    vmadm   vtmp, vci, vrcf
    vmadn   vnf, vcf, vrci
    vmadh   vni, vci, vrci
    vsubc   vnf, vzero, vnf
    vsub    vni, vzero, vni
    vadd    vni, vni, vconst[2]
    vmudl   vtmp, vrcf, vnf
    vmadm   vtmp, vrci, vnf
    vmadn   vrcf, vrcf, vni
    vmadh   vrci, vrci, vni
    vmudl   vtmp, vcf, vrcf[3h]
    vmadm   vtmp, vci, vrcf[3h]
    vmadn   vnf, vcf, vrci[3h]
    vmadh   vni, vci, vrci[3h]
    vmudh   vtmp, vtrans, vconst[1]
    vmadn   vtmp, vnf, vscale
    vmadh   vscr, vni, vscale
    vge     vtmp, vscr, vzero           // z >= 0, as in the vertex transform
    vmov    vscr[2], vtmp[2]
    vmov    vscr[3], vrci[3]
    sdv     vscr[0], 0x10($t0)
    jr      $ra
    ssv     vrcf[6], 0x18($t0)
// GEOMMODE (entered through cmd_geommode_ovl).
cmd_geommode:
    andi    $t0, w1, 7
    ori     $t1, $t0, 0x08
    sll     $t1, $t1, 24
    andi    $t2, w0, 0x3F               // tile 2..0, mip levels 5..3 (RDP bits 21..16)
    sll     $t2, $t2, 16
    or      $t1, $t1, $t2
    sw      $t1, GEOMMODE($zero)
    srl     $t2, w1, 9                  // front (0x200) -> bit 0
    andi    $t2, $t2, 1
    sll     $t3, w1, 23                 // back (0x100) -> bit 31 (bits 30..23 unused)
    or      $t2, $t2, $t3
    sw      $t2, CULLMODE($zero)
    andi    $t2, w1, 0xC00              // VTX passes: lighting 0x400, texgen 0x800
    lui     $t4, vtx_loop               // (lui/ori: the labels are forward references)
    beqz    $t2, gm_pass
    li      $t5, 0x400
    lui     $t4, light_verts
    beq     $t2, $t5, gm_pass
    ori     $t4, $t4, vtx_loop
    li      $t5, 0x800
    lui     $t4, texgen_verts
    beq     $t2, $t5, gm_pass
    ori     $t4, $t4, vtx_loop
    lui     $t4, light_verts
    ori     $t4, $t4, vtx_texgen
gm_pass:
    sw      $t4, LIGHTMODE($zero)
    li      $t3, 32                     // size = 32 + 64 shade + 64 tex + 16 z
    andi    $t2, $t0, 4
    sll     $t2, $t2, 4
    addu    $t3, $t3, $t2
    andi    $t2, $t0, 2
    sll     $t2, $t2, 5
    addu    $t3, $t3, $t2
    andi    $t2, $t0, 1
    sll     $t2, $t2, 4
    addu    $t3, $t3, $t2
    sb      $t3, TRISIZE($zero)
    andi    $t1, $t0, 4                 // TRIOFS: enabled blocks packed from 32, disabled
    li      $t2, 112                    // ones at 112 (shade, texture) / 160 (z)
    beqz    $t1, gm_ofs_s
    li      $t3, 32
    move    $t2, $t3
    addiu   $t3, $t3, 64
gm_ofs_s:
    sb      $t2, TRIOFS($zero)
    andi    $t1, $t0, 2
    beqz    $t1, gm_ofs_t
    li      $t2, 112
    move    $t2, $t3
    addiu   $t3, $t3, 64
gm_ofs_t:
    sb      $t2, TRIOFS+1($zero)
    andi    $t1, $t0, 1
    bnez    $t1, gm_ofs_z
    nop
    li      $t3, 160
gm_ofs_z:
    j       cmd_next
    sb      $t3, TRIOFS+2($zero)
    .balign 8
OVL_CLIP_END:
    .text

// ---- audio mixer ------------------------------------------------------------------------------
// AUDIO: w1 = RDRAM address of a k64_mixer record list (include/k64/mixer.h, docs/AUDIO.md).
// Runs the records into OUTB (one block of up to 128 stereo frames, s16 L R interleaved),
// writes each block to RDRAM at its BLOCK record, ends at the END record. Clobbers DMEM
// 0x100..0x7FF (the vertex cache and the input buffer: VTX again before any TRI) and the
// vector registers above $v1; the matrix and the viewport are reloaded on exit.
// A segment is one voice over frames start..start+count-1 of the block: frame f reads the
// samples at pos = frac + (f - start) * step (16.16, relative to the record's source address)
// and adds clamp(lerp * vol) to OUTB with saturation, as k64_mixer_run_cpu does. A RAMP record
// (src = 3, +4 dL << 16 | dR) gives the next voice record a per-frame volume step: frame f
// uses vol + (f - start) * d. The first group's lanes come from offsets k - (start & 3) (the
// lanes before the start may saturate: masked), the later groups from a second vector with
// offsets k - (start & 3) + 4, advanced by 4 * d per group (every lane a frame of the record
// except past its end, so no saturated lane is carried forward).
// Lanes hold 4 frames as L R L R ...: llv puts samples i, i+1 in a lane pair, [0q]/[1q]
// duplicate them into both channels of the frame.
    .equ OUTB,      0x100           // mix block: 128 frames x 4 B
    .equ SRCBUF,    0x300           // source samples of one segment (up to 0x760)
    .equ AUDTMP,    0x760           // 16 B frac lanes, 16 B lane indices
    .equ SEGBUF,    0x780           // record list chunk
    .equ SEGBUF_SIZE, 128
    .def avA    $v16                // gathered samples: lanes 2k, 2k+1 = s[i], s[i+1] of frame k
    .def avF    $v17                // position fractions (u16), duplicated per frame
    .def avNF   $v18                // ~avF
    .def avI    $v19                // interpolated, then scaled
    .def avO    $v20                // OUTB group
    .def avVol  $v21                // L R L R ... (Q15)
    .def avMask $v22                // lanes inside the segment
    .def avHi   $v23                // lanes up to the last frame (last group)
    .def avInc  $v24                // lane 0: 4 * step (low 16 bits)
    .def avOne  $v25                // 1 in every lane
    .def avOnes $v26                // 0xFFFF in every lane
    .def avLane $v27                // 0 0 1 1 2 2 3 3
    .def avK    $v28
    .def avJ    $v29
    .def avD    $v10                // per-frame volume step L R L R ... (RAMP), else 0
    .def avD4   $v11                // 4 * avD
    .def avVn   $v12                // volume of the next group
    .overlay audio, OVL_SLOT
audio_main:
    move    $s4, w1                     // next record chunk in RDRAM
    li      $s5, SEGBUF + SEGBUF_SIZE   // read pointer = end: fetch first
    vor     avOne, vzero, vconst[1]
    vor     avOnes, vzero, vconst[7]
    vxor    avD, avD, avD
    li      $t0, 0x00010001
    sw      $zero, AUDTMP + 16($zero)
    sw      $t0, AUDTMP + 20($zero)
    sll     $t1, $t0, 1
    sw      $t1, AUDTMP + 24($zero)
    addu    $t1, $t1, $t0
    sw      $t1, AUDTMP + 28($zero)
    li      $t0, AUDTMP
    lqv     avLane[0], 16($t0)
    jal     aud_clear
    li      $t7, -2

aud_next:
    li      $t0, SEGBUF + SEGBUF_SIZE
    bne     $s5, $t0, aud_have
    li      dmem, SEGBUF
    move    dram, $s4
    jal     dma_read
    li      dlen, SEGBUF_SIZE - 1
    addiu   $s4, $s4, SEGBUF_SIZE
    li      $s5, SEGBUF
aud_have:
    lw      $t0, 0($s5)                 // dma8 << 24 | source, or 0 = BLOCK, 1 = END
    lw      $gp, 4($s5)                 // step (BLOCK: RDRAM address of the block)
    lhu     $t1, 8($s5)                 // frac (BLOCK: frames)
    lbu     $t2, 10($s5)                // start
    lbu     $t3, 11($s5)                // count - 1
    sltiu   $t4, $t0, 4                 // 0 BLOCK, 1 END, 3 RAMP
    bnez    $t4, aud_ctl
    addiu   $s5, $s5, 16
    llv     avVol[0], -4($s5)
    llv     avVol[4], -4($s5)
    llv     avVol[8], -4($s5)
    llv     avVol[12], -4($s5)
    // source: DMA from the 8-byte aligned address below it
    srl     dlen, $t0, 24
    sll     dlen, dlen, 3
    addiu   dlen, dlen, 7
    lui     $t4, 0x00FF
    ori     $t4, $t4, 0xFFF8
    and     dram, $t0, $t4
    andi    $k0, $t0, 7
    jal     dma_read                    // clobbers t0
    li      dmem, SRCBUF
    addiu   $k0, $k0, SRCBUF            // sample 0 of the segment
    // position of the first lane of the start group: frac - (start & 3) * step
    andi    $t4, $t2, 3
    move    $k1, $t1
    beqz    $t4, aud_pos_ok
    move    $t5, $t4
aud_pos:
    addiu   $t5, $t5, -1
    bnez    $t5, aud_pos
    subu    $k1, $k1, $gp
aud_pos_ok:
    sh      $k1, AUDTMP + 0($zero)
    sh      $k1, AUDTMP + 2($zero)
    addu    $t5, $k1, $gp
    sh      $t5, AUDTMP + 4($zero)
    sh      $t5, AUDTMP + 6($zero)
    addu    $t5, $t5, $gp
    sh      $t5, AUDTMP + 8($zero)
    sh      $t5, AUDTMP + 10($zero)
    addu    $t5, $t5, $gp
    sh      $t5, AUDTMP + 12($zero)
    sh      $t5, AUDTMP + 14($zero)
    li      $t0, AUDTMP
    lqv     avF[0], 0($t0)
    sll     $t5, $gp, 2
    mtc2    $t5, avInc[0]
    // lane masks: first group lanes >= start & 3, last group lanes <= (start + count - 1) & 3
    ctc2    $zero, vco
    mtc2    $t4, avK[0]
    vge     avJ, avLane, avK[0]
    vmrg    avMask, avOnes, vzero
    addu    $t6, $t2, $t3               // last frame
    andi    $t5, $t6, 3
    addiu   $t5, $t5, 1
    mtc2    $t5, avK[0]
    vlt     avJ, avLane, avK[0]
    vmrg    avHi, avOnes, vzero
    // volume per lane: vol + (k - (start & 3)) * d, next group + 4 * d
    vsubc   avJ, vzero, vzero           // clear VCO for vsub/vadd
    mtc2    $t4, avK[0]
    li      $t5, 4
    mtc2    $t5, avK[2]
    vsub    avJ, avLane, avK[0]
    vmudh   avI, avD, avJ
    vadd    avJ, avJ, avK[1]
    vmudh   avVn, avD, avJ
    vmudh   avD4, avD, avK[1]
    vadd    avVn, avVn, avVol
    vadd    avVol, avVol, avI
    vxor    avD, avD, avD               // the step applies to this record only
    srl     $t6, $t6, 2
    srl     $t5, $t2, 2
    subu    $t8, $t6, $t5               // groups - 1
    sll     $t5, $t5, 4
    addiu   $fp, $t5, OUTB
aud_group:
    bnez    $t8, aud_gather
    nop
    vand    avMask, avMask, avHi
aud_gather:
    srl     $t0, $k1, 15
    and     $t0, $t0, $t7
    addu    $t0, $t0, $k0
    llv     avA[0], 0($t0)
    addu    $k1, $k1, $gp
    srl     $t0, $k1, 15
    and     $t0, $t0, $t7
    addu    $t0, $t0, $k0
    llv     avA[4], 0($t0)
    addu    $k1, $k1, $gp
    srl     $t0, $k1, 15
    and     $t0, $t0, $t7
    addu    $t0, $t0, $k0
    llv     avA[8], 0($t0)
    addu    $k1, $k1, $gp
    srl     $t0, $k1, 15
    and     $t0, $t0, $t7
    addu    $t0, $t0, $k0
    llv     avA[12], 0($t0)
    addu    $k1, $k1, $gp
    lqv     avO[0], 0($fp)
    // s0 * ~f + s1 * f + s0 = s0 * 65536 + (s1 - s0) * f
    vxor    avNF, avF, avOnes
    vmudn   avJ, avNF, avA[0q]
    vmadn   avJ, avF, avA[1q]
    vmadn   avJ, avOne, avA[0q]
    vmadm   avI, vzero, vzero[0]
    vmulf   avI, avI, avVol
    vand    avI, avI, avMask
    vadd    avO, avO, avI
    vaddc   avF, avF, avInc[0]
    vsubc   avJ, vzero, vzero           // clear VCO for the next vadd
    vor     avMask, avOnes, vzero
    vor     avVol, avVn, vzero
    vadd    avVn, avVn, avD4
    sqv     avO[0], 0($fp)
    addiu   $fp, $fp, 16
    bnez    $t8, aud_group
    addiu   $t8, $t8, -1
    j       aud_next
    nop

aud_ctl:
    addiu   $t5, $t0, -3
    beqz    $t5, aud_ramp
    sll     $t1, $t1, 2                 // BLOCK: frames * 4 bytes
    bnez    $t0, aud_end
    nop
aud_wr_full:
    mfc0    $t0, SP_DMA_FULL
    bnez    $t0, aud_wr_full
    li      $t0, OUTB
    mtc0    $t0, SP_MEM_ADDR
    mtc0    $gp, SP_DRAM_ADDR
    addiu   $t1, $t1, -1
    mtc0    $t1, SP_WR_LEN
    jal     dma_wait                    // OUTB is cleared next
    nop
    jal     aud_clear
    nop
    j       aud_next
    nop

aud_ramp:
    llv     avD[0], -12($s5)
    llv     avD[4], -12($s5)
    llv     avD[8], -12($s5)
    j       aud_next
    llv     avD[12], -12($s5)

aud_end:
    jal     load_viewport
    li      $at, 1                      // matrix registers no longer match MTX
    j       cmd_next
    nop


aud_clear:
    li      $t0, OUTB
    li      $t1, OUTB + 0x200
aud_clear_loop:
    sqv     vzero[0], 0x00($t0)
    sqv     vzero[0], 0x10($t0)
    sqv     vzero[0], 0x20($t0)
    addiu   $t0, $t0, 0x40
    bne     $t0, $t1, aud_clear_loop
    sqv     vzero[0], -0x10($t0)
    jr      $ra
    nop
    .balign 8
OVL_AUDIO_END:
    .text

// ---- VADPCM decoder ---------------------------------------------------------------------------
// ADPCM: w1 = RDRAM address of a list of decode records ending with END (src = 1), run before
// the AUDIO list that mixes the decoded samples (include/k64/audio.h). Same DMEM and register
// clobbers as AUDIO.
// Decode record (src = 2): 16-sample frames of 9 bytes (control byte: scale << 4 |
// predictor, then 16 4-bit residuals), order-2 predictors, decoded to RDRAM as s16.
//   +4  u32  (frames - 1) << 24 | input (byte address of the first frame), frames <= 16
//   +8  u32  mirror << 31 | out slot << 24 | output (8-aligned): mirror = also write the first
//            frame to the decoder's mirror address; out slot != 0: write the final state there
//   +12 u32  in slot << 24 | decoder block (16-aligned)
// Decoder block: +0 codebook address, +4 codebook bytes - 1, +8 mirror address, +12 the
// block's own address (header and codebook are not reloaded for consecutive records of one
// block); +16 * (1 + k) state slot k (the previous 8 samples). The record reads slot `in` and writes slot `out`.
// Per 8-sample half: out = p0 * s[6] + p1 * s[7] + sum_k r[k] * col_k, as a matrix product:
// col_k = 2048 at lane k and p1 from lane k + 1 on (lanes below k stay as the identity),
// r = sign-extended residual << scale, result = clamp16(acc >> 11) (docs/AUDIO.md).
    .equ ADH,   0x300                   // decoder header
    .equ ADST,  0x310                   // state
    .equ ADCB,  0x320                   // codebook, up to 8 predictors
    .equ ADOUT, 0x420                   // up to 16 frames x 32 B
    .equ ADIN,  0x620                   // up to 16 frames x 9 B + alignment
    .def adP0   $v10                    // predictor: contribution of s[6]
    .def adP1   $v11                    // contribution of s[7]
    .def adK    $v12                    // e0 = 1 << scale, e1 = 16, e2 = 32
    .def adR    $v13                    // residual bytes << 8
    .def adE    $v14                    // even residuals
    .def adO    $v15                    // odd residuals
    .def adS    $v16                    // previous half (state), second half
    .def adT    $v17
    .def adM    $v18
    .def adH    $v19
    .def adX    $v20                    // first half
    .overlay adpcm, OVL_SLOT
adpcm_main:
    move    $s4, w1                     // next record chunk in RDRAM
    li      $s5, SEGBUF + SEGBUF_SIZE
    sw      $zero, ADH + 12($zero)      // no decoder header loaded
    li      $t7, -1                     // RDRAM state slot ADST holds (none)
    // VADPCM matrix: identity x 2048 in $v2-$v9 (the lanes above the diagonal are
    // reloaded per frame, the others stay)
    li      $t0, 0x800
    vxor    $v2, vzero, vzero
    mtc2    $t0, $v2[0]
    vxor    $v3, vzero, vzero
    mtc2    $t0, $v3[2]
    vxor    $v4, vzero, vzero
    mtc2    $t0, $v4[4]
    vxor    $v5, vzero, vzero
    mtc2    $t0, $v5[6]
    vxor    $v6, vzero, vzero
    mtc2    $t0, $v6[8]
    vxor    $v7, vzero, vzero
    mtc2    $t0, $v7[10]
    vxor    $v8, vzero, vzero
    mtc2    $t0, $v8[12]
    vxor    $v9, vzero, vzero
    mtc2    $t0, $v9[14]
    li      $t0, 16
    mtc2    $t0, adK[2]
    li      $t0, 32
    mtc2    $t0, adK[4]
ad_next:
    li      $t0, SEGBUF + SEGBUF_SIZE
    bne     $s5, $t0, ad_have
    li      dmem, SEGBUF
    move    dram, $s4
    jal     dma_read
    li      dlen, SEGBUF_SIZE - 1
    addiu   $s4, $s4, SEGBUF_SIZE
    li      $s5, SEGBUF
ad_have:
    lw      $t0, 0($s5)                 // 2 = decode, 1 = END
    lw      $k0, 4($s5)                 // (frames - 1) << 24 | input
    lw      $k1, 8($s5)                 // mirror, out slot, output
    lw      $gp, 12($s5)                // in slot, decoder block
    addiu   $t0, $t0, -2
    bnez    $t0, ad_end
    addiu   $s5, $s5, 16
    lui     $t4, 0x00FF
    ori     $t4, $t4, 0xFFFF
    lw      $t0, ADH + 12($zero)
    and     $t5, $gp, $t4
    beq     $t0, $t5, ad_samebook
    move    dram, $t5
    li      dmem, ADH
    jal     dma_read
    li      dlen, 15
    lw      dram, ADH + 0($zero)
    lw      dlen, ADH + 4($zero)
    jal     dma_read
    li      dmem, ADCB
ad_samebook:
    srl     $t1, $gp, 20
    andi    $t1, $t1, 0xFF0             // in slot * 16
    addu    dram, $t5, $t1
    addiu   dram, dram, 16
    beq     dram, $t7, ad_havestate     // written by the previous record: still in ADST
    li      dmem, ADST
    jal     dma_read
    li      dlen, 15
ad_havestate:
    srl     $t6, $k0, 24                // frames - 1
    and     $t1, $k0, $t4
    andi    $t2, $t1, 7
    subu    dram, $t1, $t2
    sll     $t3, $t6, 3
    addu    $t3, $t3, $t6
    addu    dlen, $t3, $t2              // (frames - 1) * 9 + misalignment + 8 bytes, - 1
    addiu   dlen, dlen, 8
    jal     dma_read
    li      dmem, ADIN
    addiu   $t8, $t2, ADIN + 1          // residuals of the frame (control byte at -1)
    li      $fp, ADOUT
    lqv     adS[0], ADST($zero)
    li      $t1, 1
ad_frame:
    lbu     $t0, -1($t8)
    lpv     adR[0], 0($t8)
    addiu   $t8, $t8, 9
    srl     $t2, $t0, 4
    sllv    $t2, $t1, $t2
    mtc2    $t2, adK[0]
    andi    $t0, $t0, 15
    sll     $t0, $t0, 5
    addiu   $t0, $t0, ADCB
    lqv     adP0[0], 0($t0)
    lqv     adP1[0], 16($t0)
    lqv     $v2[2], 16($t0)
    lqv     $v3[4], 16($t0)
    lqv     $v4[6], 16($t0)
    lqv     $v5[8], 16($t0)
    lqv     $v6[10], 16($t0)
    lqv     $v7[12], 16($t0)
    lqv     $v8[14], 16($t0)
    vmudm   adE, adR, adK[1]            // high nibbles, sign-extended (>> 12)
    vmudn   adO, adR, adK[1]            // low nibbles to the top (<< 4)
    vmudm   adO, adO, adK[1]
    vmudh   adE, adE, adK[0]            // << scale
    vmudh   adO, adO, adK[0]
    vmudh   adT, $v2, adE[0]
    vmadh   adT, $v3, adO[0]
    vmadh   adT, $v4, adE[1]
    vmadh   adT, $v5, adO[1]
    vmadh   adT, $v6, adE[2]
    vmadh   adT, $v7, adO[2]
    vmadh   adT, $v8, adE[3]
    vmadh   adT, $v9, adO[3]
    vmadh   adT, adP0, adS[6]
    vmadh   adT, adP1, adS[7]
    vsar    adM, vzero, vzero[1]
    vsar    adH, vzero, vzero[0]
    vmudn   adT, adM, adK[2]            // (acc >> 16) * 32 >> 16 = sum >> 11, clamped
    vmadh   adX, adH, adK[2]
    vmudh   adT, $v2, adE[4]
    vmadh   adT, $v3, adO[4]
    vmadh   adT, $v4, adE[5]
    vmadh   adT, $v5, adO[5]
    vmadh   adT, $v6, adE[6]
    vmadh   adT, $v7, adO[6]
    vmadh   adT, $v8, adE[7]
    vmadh   adT, $v9, adO[7]
    vmadh   adT, adP0, adX[6]
    vmadh   adT, adP1, adX[7]
    vsar    adM, vzero, vzero[1]
    vsar    adH, vzero, vzero[0]
    vmudn   adT, adM, adK[2]
    vmadh   adS, adH, adK[2]
    sqv     adX[0], 0($fp)
    sqv     adS[0], 16($fp)
    addiu   $fp, $fp, 32
    bnez    $t6, ad_frame
    addiu   $t6, $t6, -1
    // samples out
    li      dmem, ADOUT
    and     dram, $k1, $t4
    srl     $t6, $k0, 24
    sll     dlen, $t6, 5
    jal     ad_write
    addiu   dlen, dlen, 31
    bgez    $k1, ad_nomirror
    li      dlen, 31
    jal     ad_write
    lw      dram, ADH + 8($zero)
ad_nomirror:
    srl     $t1, $k1, 20
    andi    $t1, $t1, 0x7F0             // out slot * 16
    sqv     adS[0], ADST($zero)
    beqz    $t1, ad_next
    li      $t7, -1
    and     dram, $gp, $t4
    addu    dram, dram, $t1
    addiu   dram, dram, 16
    move    $t7, dram
    li      dmem, ADST
    jal     ad_write
    li      dlen, 15
    j       ad_next
    nop

ad_end:
    jal     dma_wait                    // decoded samples in RDRAM
    nop
    jal     load_viewport
    li      $at, 1                      // matrix registers no longer match MTX
    j       cmd_next
    nop

ad_write:                               // DMEM dmem -> RDRAM dram, dlen + 1 bytes (no wait:
    mfc0    $t0, SP_DMA_FULL            // the next dma_read waits for the queue)
    bnez    $t0, ad_write
    nop
    mtc0    dmem, SP_MEM_ADDR
    mtc0    dram, SP_DRAM_ADDR
    jr      $ra
    mtc0    dlen, SP_WR_LEN
    .balign 8
OVL_ADPCM_END:
    .text

// ---- particles --------------------------------------------------------------------------------
// PARTICLES: w1 = RDRAM address (8-aligned) of a block: 16-byte header, then count x 16-byte
// particles (k64_particles_t, include/k64/kgfx.h; docs/PARTICLES.md).
//   header    s32 scale C (s15.16), u16 count, u8 flags, u8 tile, u8 tex shift, pad
//   particle  s16 x, y, z, u16 size; u8 r, g, b, a; s16 s, t
// Per particle the center goes through the matrix as a VTX vertex; the half extent is
// size * C / w (C = model units -> s13.2 pixels at w = 1, from k64_kgfx_particles_scale) and
// one screen-aligned rectangle is emitted: TEXRECT over the whole 2^shift texel square at
// (s, t) (flag TEX) or FILL_RECT with the current combiner, after an optional PRIM_DEPTH
// (flag Z, the center's z) and PRIM_COLOR (flag COLOR, only when it changes). Particles
// outside the near / far planes or the 1024 x 1024 coordinate range are dropped, rectangles
// that cross the left / top edge are cut (the texture start moves with them).
// Clobbers INBUF (the vertex cache stays) and the VTX overlay.
    .equ PART_TEX,   1
    .equ PART_Z,     2
    .equ PART_COLOR, 4
    .equ PART_CHUNK, 28                 // particles per DMA (INBUF up to PSCR)
    .equ PSCR,       0x7C0              // two 32-byte pair buffers (0x7C0, 0x7E0: CLIP_LISTS),
                                        // per particle (A +0, B +16): RDP word 0, word 1,
                                        // cut x, cut y, z, dsdx; constants staged here first
    .def vds    $v5                     // dsdx (lanes 3, 7)
    .def vpc    $v24                    // C int, frac, 0x000F, 0, 0x0FFF, 0x1000, K int, frac
    .def vpi    $v25                    // size * C
    .def vpf    $v26
    .def vrad   $v27                    // radius, s13.2 (lanes 3, 7)
    .def vrl    $v28                    // 2^31 / radius
    .def vrh    $v29
    .def vhi    $v11                    // x1, y1 (vnf, free once vscr is out)
    .def vlo    $v10                    // x0, y0 (vni)
    .def vcut   $v14                    // vwnf
    .def vH     $v30                    // packed RDP words, high halves
    .def vL     $v15                    // low halves (vwni)
    .def vK     $v31                    // command | tile bits: cmd << 8, 0, tile << 8, 0 (twice)
    .overlay part, OVL_SLOT
part_main:
    li      dmem, INBUF
    move    dram, w1
    jal     dma_read                    // also waits for a MTX DMA in flight
    li      dlen, 15
    beqz    $at, part_mtx_ok
    sw      cmdptr, LIGHTTMP + 0($zero) // s0-s2 hold loop constants until the block ends
    jal     load_matrix
    li      $at, 0
part_mtx_ok:
    sw      cmdcur, LIGHTTMP + 4($zero)
    li      $t8, INBUF
    lw      $t0, 0($t8)                 // C
    lhu     $s4, 4($t8)                 // particles left
    lbu     $t1, 6($t8)                 // flags
    lbu     $t2, 7($t8)                 // tile
    lbu     $t3, 8($t8)                 // texture size = 2^shift texels
    li      $t9, PSCR
    sw      $t0, 0($t9)
    lui     $t4, 0x000F
    sw      $t4, 4($t9)
    lui     $t4, 0x0FFF
    ori     $t4, $t4, 0x1000
    sw      $t4, 8($t9)
    addiu   $t3, $t3, 12                // K = 2^(shift + 12): dsdx = (2^31 / radius) * K >> 32
    li      $t4, 1
    sllv    $t4, $t4, $t3
    sw      $t4, 12($t9)
    andi    $t5, $t1, PART_TEX
    lui     $t6, 0x3600                 // FILL_RECT
    beqz    $t5, part_k
    li      $t7, 0
    lui     $t6, 0x2400                 // TEXRECT
    sll     $t7, $t2, 24                // tile << 24
part_k:
    sw      $t6, 16($t9)
    sw      $t7, 20($t9)
    lqv     vpc[0], 0($t9)
    ldv     vK[0], 16($t9)
    ldv     vK[8], 16($t9)
    sll     $s6, $t5, 3
    addiu   $s6, $s6, 8                 // rectangle bytes: 8 (FILL_RECT) or 16 (TEXRECT)
    andi    $s5, $t1, PART_Z
    sll     $s5, $s5, 2                 // PRIM_DEPTH bytes: 0 or 8
    andi    $s2, $t1, PART_COLOR
    srl     $s2, $s2, 2                 // PRIM_COLOR enabled: 0 or 1
    lui     $s0, 0x2E00                 // PRIM_DEPTH
    lui     $s1, 0x3A00                 // PRIM_COLOR
    lw      $sp, PARTCOUNT($zero)       // rectangles, kept in $sp until the block ends
    addiu   $gp, w1, 16                 // next particles in RDRAM
    li      $v1, 1                      // PRIM_COLOR not sent yet
part_chunk:
    beqz    $s4, part_end
    sltiu   $t0, $s4, PART_CHUNK + 1
    bnez    $t0, part_n
    move    $k0, $s4
    li      $k0, PART_CHUNK
part_n:
    subu    $s4, $s4, $k0
    li      dmem, INBUF
    move    dram, $gp
    sll     dlen, $k0, 4
    jal     dma_read
    addiu   dlen, dlen, -1
    sll     $t0, $k0, 4
    addu    $gp, $gp, $t0
    li      $k1, INBUF
    li      $t8, PSCR                   // this pair's buffer
    li      $t9, PSCR + 32              // the previous pair's
    li      $a1, 0x0011                 // reject bits of the previous pair: none yet

// Software pipelined: each iteration runs the vector pass of pair N ($k1, into $t8) and
// the scalar emit of pair N - 1 ($t9, rgba / st at -24 / -20 / -8 / -4 of $k1, reject bits
// in $a1), interleaved for dual issue; one extra iteration drains the last pair.
part_pair:
    lw      $t5, OUTLIMIT($zero)
    ldv     vin[0], 0($k1)
    sltu    $t0, $t5, outptr
    beqz    $t0, part_room
    ldv     vin[8], 16($k1)
    jal     flush_out                   // a pair takes at most 64 bytes
    nop
part_room:
    vmudn   vtmp, vmf3, vconst[1]
    vmadh   vtmp, vmi3, vconst[1]
    vmadn   vtmp, vmf0, vin[0h]
    vmadh   vtmp, vmi0, vin[0h]
    vmadn   vtmp, vmf1, vin[1h]
    vmadh   vtmp, vmi1, vin[1h]
    vmadn   vcf, vmf2, vin[2h]
    vmadh   vci, vmi2, vin[2h]
    vmudm   vtmp, vin, vpc[1]
    vmadh   vpi, vin, vpc[0]
    vmadn   vpf, vzero, vzero[0]
    vmudl   vtmp, vcf, vtrans[3h]
    vmadm   vtmp, vci, vtrans[3h]
    vmadn   vcf, vcf, vscale[3h]
    lw      $t0, 0($t9)
    vmadh   vci, vci, vscale[3h]
    lw      $t1, 4($t9)
    vch     vtmp, vci, vci[3h]
    vcl     vtmp, vcf, vcf[3h]
    vrcph   vrci[3], vci[3]
    vrcpl   vrcf[3], vcf[3]
    lw      $t2, 8($t9)
    cfc2    $at, vcc
    vrcph   vrci[3], vzero[0]
    vrcph   vrci[7], vci[7]
    lh      $t4, 14($t9)
    lw      $a2, -20($k1)
    andi    $s7, $at, 0x4444
    vrcpl   vrcf[7], vcf[7]
    vrcph   vrci[7], vzero[0]
    andi    $a3, $a1, 0x040F
    vmudn   vrcf, vrcf, vconst[2]
    vmadh   vrci, vrci, vconst[2]
    vmadn   vrcf, vzero, vzero[0]
    vsubc   vwnf, vzero, vcf
    vsub    vwni, vzero, vci
    subu    $t5, $zero, $t2
    bltzal  $t5, part_cut
    sltiu   $a3, $a3, 1
    vmudl   vtmp, vwnf, vrcf
    lh      $t3, 12($t9)
    vmadm   vtmp, vwni, vrcf
    vmadn   vnf, vwnf, vrci
    lw      $a0, -24($k1)
    vmadh   vni, vwni, vrci
    sw      $s0, 0(outptr)
    vmudn   vtmp, vrcf, vconst[2]
    sll     $t3, $t3, 16
    vmadh   vtmp, vrci, vconst[2]
    sw      $t3, 4(outptr)
    vmadl   vtmp, vrcf, vnf
    addu    $t5, outptr, $s5
    vmadm   vtmp, vrci, vnf
    xor     $t6, $a0, $fp
    vmadn   vrcf, vrcf, vni
    or      $t7, $t6, $v1
    vmadh   vrci, vrci, vni
    sltu    $t7, $zero, $t7
    and     $t7, $t7, $a3
    and     $t7, $t7, $s2
    sw      $s1, 0($t5)
    vmudl   vtmp, vcf, vrcf[3h]
    vmadm   vtmp, vci, vrcf[3h]
    sw      $a0, 4($t5)
    vmadn   vnf, vcf, vrci[3h]
    sll     $t3, $t7, 3
    vmadh   vni, vci, vrci[3h]
    addu    $t5, $t5, $t3
    vmudl   vtmp, vpf, vrcf[3h]
    subu    $t3, $zero, $a3
    vmadm   vtmp, vpi, vrcf[3h]
    and     $t6, $t6, $t3
    vmadn   vtmp, vpf, vrci[3h]
    xor     $fp, $fp, $t6
    vmadh   vrad, vpi, vrci[3h]
    sltu    $v1, $t7, $v1
    vmudh   vtmp, vtrans, vconst[1]
    sll     $t6, $t4, 16
    vmadn   vtmp, vnf, vscale
    or      $t4, $t4, $t6
    vmadh   vscr, vni, vscale
    sw      $t0, 0($t5)
    vrcp    vrl[3], vrad[3]
    sw      $t1, 4($t5)
    vrcph   vrh[3], vzero[0]
    sw      $a2, 8($t5)
    vrcp    vrl[7], vrad[7]
    sw      $t4, 12($t5)
    vrcph   vrh[7], vzero[0]
    addu    $t5, $t5, $s6
    vmov    vscr[3], vrad[3]
    subu    $t5, $t5, outptr
    vmov    vscr[7], vrad[7]
    and     $t5, $t5, $t3
    addu    outptr, outptr, $t5
    addu    $sp, $sp, $a3
    lw      $t0, 16($t9)
    vadd    vhi, vscr, vscr[3h]
    vsub    vlo, vscr, vscr[3h]
    lw      $t1, 20($t9)
    vmudl   vtmp, vrl, vpc[7]
    lw      $t2, 24($t9)
    vmadm   vtmp, vrh, vpc[7]
    lh      $t4, 30($t9)
    vmadn   vtmp, vrl, vpc[6]
    lw      $a2, -4($k1)
    vmadh   vds, vrh, vpc[6]
    andi    $a3, $a1, 0x40F0
    vge     vtmp, vzero, vhi
    subu    $t5, $zero, $t2
    bltzal  $t5, part_cut
    sltiu   $a3, $a3, 1
    cfc2    $at, vcc
    vge     vtmp, vlo, vpc[5]
    lh      $t3, 28($t9)
    lw      $a0, -8($k1)
    andi    $at, $at, 0xBB
    or      $s7, $s7, $at
    cfc2    $at, vcc
    vge     vcut, vlo, vzero
    vlt     vhi, vhi, vpc[4]
    sw      $s0, 0(outptr)
    sll     $t3, $t3, 16
    andi    $at, $at, 0xBB
    sw      $t3, 4(outptr)
    or      $s7, $s7, $at
    vge     vscr, vscr, vzero
    vsub    vlo, vcut, vlo
    addu    $t5, outptr, $s5
    vmov    vhi[2], vcut[0]
    xor     $t6, $a0, $fp
    vmov    vhi[3], vcut[1]
    or      $t7, $t6, $v1
    vmov    vhi[6], vcut[4]
    sltu    $t7, $zero, $t7
    vmov    vhi[7], vcut[5]
    and     $t7, $t7, $a3
    vmov    vlo[2], vscr[2]
    and     $t7, $t7, $s2
    vmov    vlo[6], vscr[6]
    sw      $s1, 0($t5)
    vmov    vlo[3], vds[3]
    sw      $a0, 4($t5)
    vand    vL, vhi, vpc[2]
    sll     $t3, $t7, 3
    vmudl   vH, vhi, vpc[5]
    addu    $t5, $t5, $t3
    vmov    vlo[7], vds[7]
    subu    $t3, $zero, $a3
    and     $t6, $t6, $t3
    xor     $fp, $fp, $t6
    li      $at, 0x55
    sltu    $v1, $t7, $v1
    ctc2    $at, vcc
    vmudn   vL, vL, vpc[5]
    vadd    vH, vH, vK
    sll     $t6, $t4, 16
    or      $t4, $t4, $t6
    sw      $t0, 0($t5)
    sdv     vlo[0], 8($t8)
    sw      $t1, 4($t5)
    sdv     vlo[8], 24($t8)
    vor     vL, vL, vhi[1q]
    sw      $a2, 8($t5)
    sw      $t4, 12($t5)
    addu    $t5, $t5, $s6
    subu    $t5, $t5, outptr
    vmrg    vH, vH, vL[0q]
    and     $t5, $t5, $t3
    addu    outptr, outptr, $t5
    addu    $sp, $sp, $a3
    sdv     vH[0], 0($t8)
    sdv     vH[8], 16($t8)
    sltiu   $at, $k0, 2                 // odd count: B of the last pair is junk
    sll     $at, $at, 14
    or      $a1, $s7, $at
    addiu   $k0, $k0, -2
    slti    $at, $k0, -1
    xori    $t9, $t9, 0x20
    addiu   $k1, $k1, 32
    beqz    $at, part_pair
    xori    $t8, $t8, 0x20
    j       part_chunk
    nop

// Rectangle crossing the left / top edge: st ($a2, s << 16 | t) += cut x * dsdx >> 7 << 16 |
// cut y * dsdx >> 7 ($t2 = cut x << 16 | cut y, $t4 = dsdx). No scalar multiply: shift and
// add. Clobbers at, t2, t3, t5-t7 only (called from the middle of the pipelined pair).
part_cut:
    srl     $t3, $t2, 16
    andi    $t2, $t2, 0xFFFF
    move    $at, $t4
    li      $t6, 0
    li      $t7, 0
part_mul:
    andi    $t5, $at, 1
    beqz    $t5, part_mul1
    srl     $at, $at, 1
    addu    $t6, $t6, $t3
    addu    $t7, $t7, $t2
part_mul1:
    sll     $t3, $t3, 1
    bnez    $at, part_mul
    sll     $t2, $t2, 1
    srl     $t6, $t6, 7
    srl     $t7, $t7, 7
    sll     $t6, $t6, 16
    addu    $t7, $a2, $t7
    andi    $t7, $t7, 0xFFFF            // t
    addu    $a2, $a2, $t6
    andi    $t5, $a2, 0xFFFF
    xor     $a2, $a2, $t5               // s
    jr      $ra
    or      $a2, $a2, $t7
part_end:
    lw      cmdptr, LIGHTTMP + 0($zero)
    lw      cmdcur, LIGHTTMP + 4($zero)
    li      cmdend, CMDBUF + CMDBUF_SIZE
    sw      $sp, PARTCOUNT($zero)
    j       cmd_next
    li      $at, 0                      // matrix loaded
    .balign 8
OVL_PART_END:
    .text

// Overlay $t0 (OVL_*: byte offset in OVL_TABLE) into its IMEM slot, unless resident.
// Synchronous. Clobbers t0-t2.
ovl_load:
    lw      $t1, OVL_RESIDENT($zero)
    beq     $t1, $t0, ovl_done
    lw      $t2, OVL_TABLE + 0($t0)     // length code << 24 | RDRAM address
    sw      $t0, OVL_RESIDENT($zero)
    li      $t1, cmd_vtx_ovl
    bnez    $t0, ovl_tab
    nop
    li      $t1, cmd_vtx
ovl_tab:
    sh      $t1, cmd_table + 4($zero)
ovl_full:
    mfc0    $t1, SP_DMA_FULL
    bnez    $t1, ovl_full
    li      $t1, 0x1000 + OVL_SLOT
    mtc0    $t1, SP_MEM_ADDR
    sll     $t1, $t2, 8
    srl     $t1, $t1, 8
    mtc0    $t1, SP_DRAM_ADDR
    srl     $t1, $t2, 24
    sll     $t1, $t1, 3                 // length - 1 (the DMA rounds up to 8 bytes)
    mtc0    $t1, SP_RD_LEN
ovl_wait:
    mfc0    $t1, SP_DMA_BUSY
    bnez    $t1, ovl_wait
    nop
ovl_done:
    jr      $ra
    nop

cmd_audio_ovl:
    jal     ovl_load
    li      $t0, OVL_AUDIO
    j       audio_main
    nop
cmd_adpcm_ovl:
    jal     ovl_load
    li      $t0, OVL_ADPCM
    j       adpcm_main
    nop
cmd_part_ovl:
    jal     ovl_load
    li      $t0, OVL_PART
    j       part_main
    nop

tri_rejclip:                            // t0-t2 = vertex flags, t4 = their OR (masked)
    and     $t3, $t0, $t1
    and     $t3, $t3, $t2
    andi    $t3, $t3, CLIP_REJECT       // planes all vertices are outside of
    bnez    $t3, tri_done               // rejected
    andi    $t4, $t4, CLIP_PLANES_MASK
    beqz    $t4, tri_vis                // only past the screen edges: the scissor's job
    nop
tri_clip_ovl:                           // keeps t4 (planes), w0, p1-p3, s7
    jal     ovl_load
    li      $t0, OVL_CLIP
    j       tri_clip
    nop

// Sends the filled OUTBUF half to the RDP ring without waiting for the DMA: the transfer
// overlaps the setup of the next triangles in the other half, and DPC_END for it is written
// by the next flush_out (or flush_publish at END), once the DMA is known to be complete.
// Moves to the next ring block when the data does not fit, as k64_rdp_overflow does.
// Clobbers t0-t7.
flush_out:
    lw      $t0, OUTLIMIT($zero)
    addiu   $t0, $t0, TRI_MAX - OUTBUF_HALF     // start of the half
    subu    $t1, outptr, $t0
    beqz    $t1, flush_done
    move    $t7, $ra
    jal     flush_publish               // the previous half is in RDRAM: hand it to the RDP
    lw      $t2, RING_CUR($zero)
    lw      $t3, RING_END($zero)
    addu    $t4, $t2, $t1
    sltu    $t4, $t3, $t4
    beqz    $t4, flush_dma
    nop
flush_next_block:                       // the RDP must have latched the current block's START
    mfc0    $t5, DPC_STATUS
    andi    $t5, $t5, 0x400
    bnez    $t5, flush_next_block
    nop
    lw      $t5, RING_IDX($zero)
    addiu   $t5, $t5, 1
    andi    $t5, $t5, 3
    sw      $t5, RING_IDX($zero)
    sll     $t5, $t5, 2
    lw      $t2, RING($t5)
    sw      $zero, RING_STARTED($zero)
    sw      $t2, RING_PUB($zero)
    li      $t3, RING_BLOCK_BYTES
    addu    $t3, $t2, $t3
    sw      $t3, RING_END($zero)
flush_dma:
    mfc0    $t5, SP_DMA_FULL
    bnez    $t5, flush_dma
    nop
    mtc0    $t0, SP_MEM_ADDR
    mtc0    $t2, SP_DRAM_ADDR
    addiu   $t5, $t1, -1
    mtc0    $t5, SP_WR_LEN
    addu    $t2, $t2, $t1
    sw      $t2, RING_CUR($zero)
    xori    outptr, $t0, OUTBUF ^ (OUTBUF + OUTBUF_HALF)   // other half
    addiu   $t0, outptr, OUTBUF_HALF - TRI_MAX
    jr      $t7
    sw      $t0, OUTLIMIT($zero)
flush_done:
    jr      $ra
    nop

// Waits for the DMAs in flight, then hands the ring up to RING_CUR to the RDP (DPC_START on a
// fresh block, DPC_END). Clobbers t5, t6.
flush_publish:
    mfc0    $t5, SP_DMA_BUSY
    bnez    $t5, flush_publish
    lw      $t6, RING_PUB($zero)
    lw      $t5, RING_CUR($zero)
    beq     $t5, $t6, flush_pub_done
    lw      $t5, RING_STARTED($zero)
    bnez    $t5, flush_pub_end
    nop
flush_pub_start:
    mfc0    $t5, DPC_STATUS
    andi    $t5, $t5, 0x400
    bnez    $t5, flush_pub_start
    li      $t5, 1
    mtc0    $t6, DPC_START
    sw      $t5, RING_STARTED($zero)
flush_pub_end:
    lw      $t6, RING_CUR($zero)
    sw      $t6, RING_PUB($zero)
    mtc0    $t6, DPC_END
flush_pub_done:
    jr      $ra
    nop

// Matrix columns from DMEM into vmi*/vmf*, duplicated in both halves.
load_matrix:
    ldv     vmi0[0], MTX + 0x00($zero)
    ldv     vmi0[8], MTX + 0x00($zero)
    ldv     vmi1[0], MTX + 0x08($zero)
    ldv     vmi1[8], MTX + 0x08($zero)
    ldv     vmi2[0], MTX + 0x10($zero)
    ldv     vmi2[8], MTX + 0x10($zero)
    ldv     vmi3[0], MTX + 0x18($zero)
    ldv     vmi3[8], MTX + 0x18($zero)
    ldv     vmf0[0], MTX + 0x20($zero)
    ldv     vmf0[8], MTX + 0x20($zero)
    ldv     vmf1[0], MTX + 0x28($zero)
    ldv     vmf1[8], MTX + 0x28($zero)
    ldv     vmf2[0], MTX + 0x30($zero)
    ldv     vmf2[8], MTX + 0x30($zero)
    ldv     vmf3[0], MTX + 0x38($zero)
    jr      $ra
    ldv     vmf3[8], MTX + 0x38($zero)

load_viewport:
    lqv     vscale[0], VIEWPORT + 0x00($zero)
    jr      $ra
    lqv     vtrans[0], VIEWPORT + 0x10($zero)

// Synchronous DMA RDRAM -> DMEM: dmem, dram, dlen = length - 1.
dma_read:
    mfc0    $t0, SP_DMA_FULL
    bnez    $t0, dma_read
    nop
    mtc0    dmem, SP_MEM_ADDR
    mtc0    dram, SP_DRAM_ADDR
    mtc0    dlen, SP_RD_LEN
dma_wait:
    mfc0    $t0, SP_DMA_BUSY
    bnez    $t0, dma_wait
    nop
    jr      $ra
    nop
