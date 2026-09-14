/*
 * SCos native - stage 1 (MBR, sector 0)
 * Loads stage 2 (16 sectors starting at sector 1) to 0x8000 and jumps to it.
 */
    .code16
    .section .text
    .globl _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %ss
    movw $0x7c00, %sp
    sti

    /* drive number may be in DL from BIOS; save it for stage2 at 0x7c00-2 */
    movb %dl, 0x7bfe

    /* ES:BX = 0x800:0x0000 -> linear 0x8000 */
    movw $0x800, %ax
    movw %ax, %es
    xorw %bx, %bx
    movw $0x0210, %ax        /* AH=02 read sectors, AL=16 */
    movw $0x0002, %cx        /* cylinder 0, sector 2 (1-based) */
    movb $0x00, %dh          /* head 0 */
    movb 0x7bfe, %dl         /* drive */
    int $0x13
    jc disk_fail
    /* far jump to stage2 */
    ljmpl $0x800, $0x0000

disk_fail:
    /* retry once with drive 0x80 */
    movw $0x800, %ax
    movw %ax, %es
    xorw %bx, %bx
    movw $0x0210, %ax
    movw $0x0002, %cx
    movb $0x00, %dh
    movb $0x80, %dl
    int $0x13
    jc fail_msg_loop
    ljmpl $0x800, $0x0000

fail_msg_loop:
    movb $0x0e, %ah
    movw $0x0007, %bx
    leaw fail_str, %si
fl:
    lodsb
    testb %al, %al
    jz fh
    int $0x10
    jmp fl
fh:
    cli
    hlt
    jmp fh

fail_str: .asciz "\r\nSCos: disk read error"

    .org 510
    .byte 0x55, 0xAA
