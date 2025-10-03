<<<<<<< HEAD
    .section .text
    .globl _entry
_entry:

    li      t0, 0x10000000       # UART0 base on virt
 

    # 设栈（单核：直接用预留的 stack_top；多核：用 mhartid 切段）
    la      sp, stack_top


    # 仅 hart0 清 BSS
    csrr    t2, mhartid
    bnez    t2, 1f
    la      t3, __bss_start
    la      t4, __bss_end
0:  bgeu    t3, t4, 1f
    sd      x0, 0(t3)
    addi    t3, t3, 8
    j       0b
1:
    # 跳到 C 入口
    la      t0, start
    jr      t0

    .section .bss.stack,"aw",@nobits
    .align 16
    .globl  stack_top
    .equ    STACK_SIZE, 16*1024
stack_area:
    .skip   STACK_SIZE
stack_top:
=======
        # qemu -kernel loads the kernel at 0x80000000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80000000.
.section .text
.global _entry
_entry:
        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0
        li a0, 1024*4
        csrr a1, mhartid
        addi a1, a1, 1
        mul a0, a0, a1
        add sp, sp, a0
        # jump to start() in start.c
        call start
spin:
        j spin
>>>>>>> Initial commit from VM
