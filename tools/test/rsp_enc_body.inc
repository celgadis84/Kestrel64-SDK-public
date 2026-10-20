start:
    nop
    addiu $t0, $zero, 0x123
    addi  $sp, $sp, -16
    ori   $a0, $a1, 0xFFFF
    andi  $1, $2, 0x8000
    xori  $s0, $s1, 3
    slti  $t1, $t2, -5
    sltiu $t1, $t2, 100
    lui   $t3, 0xA404
    add   $v0, $v1, $a0
    addu  $t0, $t1, $t2
    sub   $t0, $t1, $t2
    subu  $t0, $t1, $t2
    and   $t0, $t1, $t2
    or    $t0, $t1, $t2
    xor   $t0, $t1, $t2
    nor   $t0, $t1, $t2
    slt   $t0, $t1, $t2
    sltu  $t0, $t1, $t2
    sll   $t0, $t1, 3
    srl   $t0, $t1, 31
    sra   $t0, $t1, 16
    sllv  $t0, $t1, $t2
    srlv  $t0, $t1, $t2
    srav  $t0, $t1, $t2
    lb    $t0, 4($s0)
    lbu   $t0, -4($s0)
    lh    $t0, 0x7FFE($s0)
    lhu   $t0, 2($s0)
    lw    $t0, 8($sp)
    sb    $t0, 1($s0)
    sh    $t0, 2($s0)
    sw    $ra, 0($sp)
loop:
    beq   $t0, $t1, loop
    nop
    bne   $t0, $zero, fwd
    nop
    blez  $t0, loop
    nop
    bgtz  $t0, fwd
    nop
    bltz  $t0, loop
    nop
    bgez  $t0, fwd
    nop
    bgezal $t0, fwd
    nop
    j     loop
    nop
    jal   fwd
    nop
    jr    $ra
    nop
    jalr  $t9
    nop
    break
fwd:
    mfc0  $t0, SP_STATUS
    mtc0  $t0, SP_MEM_ADDR
    mtc0  $t1, DPC_END
    mfc0  $t2, SP_SEMAPHORE
    mfc2  $t0, $v1[0]
    mfc2  $t0, $v31[14]
    mtc2  $t1, $v7[6]
    cfc2  $t0, vcc
    ctc2  $t0, vco
    cfc2  $t0, vce
    vmulf $v1, $v2, $v3
    vmulu $v1, $v2, $v3[0q]
    vmulq $v1, $v2, $v3[1q]
    vmudl $v1, $v2, $v3[0h]
    vmudm $v1, $v2, $v3[3h]
    vmudn $v4, $v5, $v6[0]
    vmudh $v4, $v5, $v6[7]
    vmacf $v4, $v5, $v6[3]
    vmacu $v4, $v5, $v6
    vmacq $v4, $v5, $v6
    vmadl $v31, $v30, $v29[2]
    vmadm $v0, $v0, $v0
    vmadn $v1, $v2, $v3[4]
    vmadh $v1, $v2, $v3[5]
    vadd  $v1, $v2, $v3
    vsub  $v1, $v2, $v3[1]
    vabs  $v1, $v2, $v3[2h]
    vaddc $v1, $v2, $v3
    vsubc $v1, $v2, $v3
    vlt   $v1, $v2, $v3
    veq   $v1, $v2, $v3
    vne   $v1, $v2, $v3
    vge   $v1, $v2, $v3[6]
    vcl   $v1, $v2, $v3
    vch   $v1, $v2, $v3
    vcr   $v1, $v2, $v3
    vmrg  $v1, $v2, $v3
    vand  $v1, $v2, $v3
    vnand $v1, $v2, $v3
    vor   $v1, $v2, $v3
    vnor  $v1, $v2, $v3
    vxor  $v1, $v2, $v3
    vnxor $v1, $v2, $v3
    vsar  $v1, $v2, $v3[0]
    vsar  $v1, $v2, $v3[2]
    vrcp  $v1[0], $v2[3]
    vrcph $v1[1], $v2[3]
    vrcpl $v1[2], $v2[7]
    vrsq  $v1[3], $v2[0]
    vrsqh $v1[4], $v2[0]
    vrsql $v1[5], $v2[0]
    vmov  $v1[6], $v2[1]
    vrndp $v1, $v2, $v3
    vrndn $v1, $v2, $v3
    lbv   $v1[0], 3($s0)
    lsv   $v1[2], 6($s0)
    llv   $v1[4], 8($s0)
    ldv   $v1[8], 16($s0)
    lqv   $v1[0], 32($s0)
    lrv   $v1[0], 48($s0)
    lpv   $v1[0], 8($s0)
    luv   $v1[0], 8($s0)
    lhv   $v1[0], 16($s0)
    lfv   $v1[0], 16($s0)
    ltv   $v1[0], 16($s0)
    sbv   $v1[15], -1($s0)
    ssv   $v1[14], -2($s0)
    slv   $v1[4], -4($s0)
    sdv   $v1[8], -8($s0)
    sqv   $v1[0], -16($s0)
    srv   $v1[0], 16($s0)
    spv   $v1[0], 504($s0)
    suv   $v1[0], -512($s0)
    shv   $v1[0], 16($s0)
    sfv   $v1[0], 16($s0)
    swv   $v1[0], 16($s0)
    stv   $v1[0], 16($s0)
