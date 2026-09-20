/*
 * SCos native kernel - 32-bit protected mode entry point and interrupt stubs.
 * Entered by stage2 with a flat GDT (0x08 code / 0x10 data), interrupts off,
 * boot parameters at physical 0x7000.
 */
    .code32
    .section .text
    .globl _start
_start:
    cli
    movl $0x10, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs

    /* zero .bss */
    movl $_bss_start, %edi
    movl $_bss_end, %ecx
    subl %edi, %ecx
    shrl $2, %ecx
    xorl %eax, %eax
    rep stosl

    /* kernel stack */
    movl $stack_top, %esp

    /* boot info pointer */
    movl $0x7000, %eax
    pushl %eax
    call kmain
    addl $4, %esp

    /* kmain should never return */
1:  cli
    hlt
    jmp 1b

/* ---------------- interrupt stubs ---------------- */
.macro ISR_NOERR n
isr_\n:
    pushl $0
    pushl $\n
    jmp isr_common
.endm
.macro ISR_ERR n
isr_\n:
    pushl $\n
    jmp isr_common
.endm

    ISR_NOERR 0
    ISR_NOERR 1
    ISR_NOERR 2
    ISR_NOERR 3
    ISR_NOERR 4
    ISR_NOERR 5
    ISR_NOERR 6
    ISR_NOERR 7
    ISR_ERR   8
    ISR_NOERR 9
    ISR_ERR   10
    ISR_ERR   11
    ISR_ERR   12
    ISR_ERR   13
    ISR_ERR   14
    ISR_NOERR 15
    ISR_NOERR 16
    ISR_ERR   17
    ISR_NOERR 18
    ISR_NOERR 19
    ISR_NOERR 20
    ISR_NOERR 21
    ISR_NOERR 22
    ISR_NOERR 23
    ISR_NOERR 24
    ISR_NOERR 25
    ISR_NOERR 26
    ISR_NOERR 27
    ISR_NOERR 28
    ISR_NOERR 29
    ISR_ERR   30
    ISR_NOERR 31
    /* IRQs 0-15 mapped to 32-47 */
    ISR_NOERR 32
    ISR_NOERR 33
    ISR_NOERR 34
    ISR_NOERR 35
    ISR_NOERR 36
    ISR_NOERR 37
    ISR_NOERR 38
    ISR_NOERR 39
    ISR_NOERR 40
    ISR_NOERR 41
    ISR_NOERR 42
    ISR_NOERR 43
    ISR_NOERR 44
    ISR_NOERR 45
    ISR_NOERR 46
    ISR_NOERR 47

isr_common:
    pushal
    movl %esp, %eax
    pushl %eax
    call isr_handler
    addl $4, %esp
    popal
    addl $8, %esp          /* discard int number + error code */
    iret

/* address of stub table for idt.c */
    .globl isr_stub_table
isr_stub_table:
    .long isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
    .long isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
    .long isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
    .long isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
    .long isr_32, isr_33, isr_34, isr_35, isr_36, isr_37, isr_38, isr_39
    .long isr_40, isr_41, isr_42, isr_43, isr_44, isr_45, isr_46, isr_47

/* small asm helpers used from C */
    .globl cpu_hlt
cpu_hlt:
    hlt
    ret

    .globl irq_enable
irq_enable:
    sti
    ret

    .globl irq_disable
irq_disable:
    cli
    ret

    .globl cpu_reboot_8042
cpu_reboot_8042:
    movw $0x64, %dx
    movb $0xFE, %al
    outb %al, %dx
1:  hlt
    jmp 1b

    .section .bss
    .align 16
    .space 65536
stack_top:
