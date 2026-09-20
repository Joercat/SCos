/*
 * SCos native - stage 1 (MBR, sector 0)
 * Loads stage 2 (16 sectors starting at LBA 1) to 0x8000 and jumps to it.
 *
 * Real-hardware notes:
 *  - the BIOS drive number (DL) is trusted first but every read is retried
 *    with LBA extensions (AH=42h) and then classic CHS (AH=02h), on the SAME BIOS boot drive. Never fall back to an unrelated disk.
 *  - our code leaves interrupts disabled; firmware calls manage their own
 *    interrupt requirements. The loader makes no timing guarantee.
 *  - scratch: 0x7b00 = BIOS drive, 0x7b10 = disk address packet.
 */
    .code16
    .section .text
    .globl _start
_start:
    cli
    xorw %ax, %ax
    movw %ax, %ds
    xorw %ax, %ax
    movw %ax, %ss
    movw $0x7c00, %sp
    cld

    movb %dl, 0x7b00            /* remember the drive the BIOS booted from */

    call rd_lba                 /* BIOS drive, LBA extensions */
    jnc  go
    call rd_chs                 /* BIOS drive, classic CHS */
    jnc  go
    jmp fail

fail:
    cld
    movb $0x0e, %ah
    movw $0x0007, %bx
    leaw fail_str, %si
fl:
    cld
    lodsb
    testb %al, %al
    jz   fh
    pushal
    pushw %ds
    pushw %es
    int  $0x10
    popw %es
    popw %ds
    popal
    jmp  fl
fh:
    cli
    hlt
    jmp  fh

go:
    ljmpl $0x800, $0x0000

/* read 16 sectors from LBA 1 to 0x0800:0, drive in 0x7b00 */
rd_lba:
    movw $0x7b10, %si
    movw $0x0010, 0(%si)        /* packet size 16 */
    movw $0x0010, 2(%si)        /* sector count 16 */
    movw $0x0000, 4(%si)        /* offset */
    movw $0x0800, 6(%si)        /* segment */
    movl $0x00000001, 8(%si)    /* LBA low */
    movl $0x00000000, 12(%si)   /* LBA high */
    movb 0x7b00, %dl
    movw $0x4200, %ax
    pushw %ds
    pushw %es
    stc                        /* failure unless firmware reports success */
    int  $0x13
    popw %es
    popw %ds
    ret

/* read 16 sectors from CHS (0,0,2) to 0x0800:0, drive in 0x7b00 */
rd_chs:
    movw $0x800, %ax
    movw %ax, %es
    xorw %bx, %bx
    movw $0x0210, %ax           /* AH=02, 16 sectors */
    movw $0x0002, %cx           /* cylinder 0, sector 2 */
    movb $0x00, %dh             /* head 0 */
    movb 0x7b00, %dl
    pushw %ds
    pushw %es
    stc                        /* failure unless firmware reports success */
    int  $0x13
    popw %es
    popw %ds
    ret

fail_str: .asciz "\r\nSCos: disk read error (no bootable stage2)\r\n"

    .org 510
    .byte 0x55, 0xAA
