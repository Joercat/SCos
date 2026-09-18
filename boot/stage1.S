/*
 * SCos native - stage 1 (MBR, sector 0)
 * Loads stage 2 (16 sectors starting at LBA 1) to 0x8000 and jumps to it.
 *
 * Real-hardware notes:
 *  - the BIOS drive number (DL) is trusted first but every read is retried
 *    with LBA extensions (AH=42h) and then classic CHS (AH=02h), and then
 *    against drives 0x80 and 0x00, because USB sticks under a real CSM BIOS
 *    may appear as 0x80, 0x81 or 0x00 depending on firmware and port.
 *  - interrupts stay masked for the whole stage: we run for milliseconds
 *    and cannot afford a BIOS timer tick pushing over our scratch area.
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
    cmpb $0x80, 0x7b00          /* already tried 0x80? */
    je   try_floppy
    movb $0x80, 0x7b00          /* first hard disk */
    call rd_lba
    jnc  go
    call rd_chs
    jnc  go
try_floppy:
    movb $0x00, 0x7b00          /* floppy / USB floppy emulation */
    call rd_lba
    jnc  go
    call rd_chs
    jnc  go

fail:
    movb $0x0e, %ah
    movw $0x0007, %bx
    leaw fail_str, %si
fl:
    lodsb
    testb %al, %al
    jz   fh
    int  $0x10
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
    int  $0x13
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
    int  $0x13
    ret

fail_str: .asciz "\r\nSCos: disk read error (no bootable stage2)\r\n"

    .org 510
    .byte 0x55, 0xAA
