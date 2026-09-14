/*
 * SCos native - stage 2 (loaded at 0x8000)
 *
 * Real-mode work: serial console, VESA/VBE linear framebuffer mode setup,
 * memory size probe (int 15h E801), A20 gate, kernel load from disk (LBA with
 * CHS fallback), then switch to 32-bit protected mode and jump to the kernel
 * entry point at 0x100000.
 *
 * Boot parameters are left at physical 0x7000 (struct boot_info in types.h).
 */
    .code16
    .section .text
    .globl _start
_start:
    xorw %ax, %ax
    movw %ax, %ds
    movw %ax, %es
    movw $0x6f00, %sp

    /* ---- serial COM1 init: 115200 8N1 ---- */
    movw $0x3fb, %dx
    movb $0x80, %al
    outb %al, %dx
    movw $0x3f8, %dx
    movb $0x01, %al
    outb %al, %dx
    movw $0x3f9, %dx
    xorb %al, %al
    outb %al, %dx
    movw $0x3fb, %dx
    movb $0x03, %al
    outb %al, %dx
    movw $0x3fa, %dx
    movb $0xc7, %al
    outb %al, %dx

    leaw s_boot, %si
    call ser16

    /* ---- VBE controller info -> 0:0x6000 ---- */
    xorw %ax, %ax
    movw %ax, %es
    movw $0x6000, %di
    movl $0x32454256, %es:(%di)      /* "VBE2" */
    movw $0x4f00, %ax
    int $0x10
    cmpw $0x004f, %ax
    je vbe_ok
    leaw s_novbe, %si
    call ser16
    jmp halt16
vbe_ok:
    movw 0x600e, %bx                 /* mode list offset */
    movw 0x6010, %cx                 /* mode list segment */
    movw %cx, %es

/*
 * Mode selection: pass 1 looks for exactly 1024x768x32 LFB, pass 2 accepts
 * any >=800x600 32bpp LFB mode.
 */
    xorw %bp, %bp                    /* bp = pass */
mode_loop:
    movw %es:(%bx), %dx
    cmpw $0xffff, %dx
    je next_pass
    pushw %bx
    pushw %dx
    movw $0x6800, %di
    movw %dx, %cx
    movw $0x4f01, %ax
    int $0x10
    popw %dx
    popw %bx
    cmpw $0x004f, %ax
    jne next_mode
    movw 0x6800, %ax                 /* attributes */
    andw $0x0081, %ax                /* supported + LFB */
    cmpw $0x0081, %ax
    jne next_mode
    cmpb $32, 0x6819                 /* bpp */
    jne next_mode
    movw 0x6812, %ax                 /* width */
    movw 0x6814, %cx                 /* height */
    testw %bp, %bp
    jnz accept_any
    cmpw $1024, %ax
    jne next_mode
    cmpw $768, %cx
    jne next_mode
    jmp mode_found
accept_any:
    cmpw $800, %ax
    jb next_mode
    cmpw $600, %cx
    jb next_mode
mode_found:
    /* save chosen mode info */
    xorw %ax, %ax
    movw %ax, %es
    movl $0x50534353, 0x7000         /* magic "SCSP" */
    movl 0x6828, %eax                /* physical LFB base */
    movl %eax, 0x7004
    movw 0x6810, %ax                 /* bytes per scanline */
    testw %ax, %ax
    jnz pitch_ok
    movw 0x6812, %ax
    shlw $2, %ax
pitch_ok:
    movw %ax, 0x7008
    movw 0x6812, %ax
    movw %ax, 0x700a                 /* width */
    movw 0x6814, %ax
    movw %ax, 0x700c                 /* height */
    movb $32, 0x700e
    leaw s_modeok, %si
    call ser16
    movw %dx, %bx                    /* mode number (from loop) */
    orw $0x4000, %bx                 /* enable LFB */
    movw $0x4f02, %ax
    int $0x10
    cmpw $0x004f, %ax
    je set_ok
    leaw s_setfail, %si
    call ser16
    jmp halt16
set_ok:
    jmp mem_probe

next_mode:
    incw %bx
    incw %bx
    jmp mode_loop
next_pass:
    testw %bp, %bp
    jnz mode_fail
    movw $1, %bp
    movw 0x600e, %bx
    movw 0x6010, %cx
    movw %cx, %es
    jmp mode_loop
mode_fail:
    leaw s_nomode, %si
    call ser16
halt16:
    cli
    hlt
    jmp halt16

/* ---- memory size via int 15h AX=E801 ---- */
mem_probe:
    xorw %ax, %ax
    movw %ax, %es
    movw $0xe801, %ax
    int $0x15
    jc mem_default
    /* AX = KB between 1M and 16M, BX = 64K blocks above 16M (or CX/DX) */
    testw %ax, %ax
    jnz e801_have
    movw %cx, %ax
    movw %dx, %bx
e801_have:
    testw %ax, %ax
    jz mem_default2
    /* total KB = 1024 + AX + BX*64 */
    movzwl %ax, %eax
    movzwl %bx, %ecx
    shll $6, %ecx
    addl %ecx, %eax
    addl $1024, %eax
    movl %eax, 0x7010
    jmp mem_done
mem_default2:
mem_default:
    movl $0x40000, 0x7010            /* assume 256 MB */
mem_done:
    leaw s_mem, %si
    call ser16

    /* ---- fast A20 via port 0x92 ---- */
    inb $0x92, %al
    orb $0x02, %al
    andb $0xfe, %al
    outb %al, $0x92

    /* ---- load kernel: LBA sectors [kernel_start_lba, +count) -> 0x100000 ---- */
    leaw s_load, %si
    call ser16

    movw $0x2000, %ax                /* stage buffer at 0x20000 (below 1 MB) */
    movw %ax, %es
    xorw %di, %di                    /* di = offset within 64K chunk */
    movl kernel_start_lba, %esi      /* current LBA */
load_loop:
    movl kernel_sector_count, %ecx
    subl kernel_loaded, %ecx
    jz load_done
    /* chunk = min(remaining, 127) */
    cmp $127, %ecx
    jbe chunk_ok
    movl $127, %ecx
chunk_ok:
    /* build LBA packet at 0x6f00 */
    movw $0x6e00, %bx
    movw $16, (%bx)
    movw %cx, 2(%bx)
    movw $0x0000, 4(%bx)             /* offset */
    movw %es, 6(%bx)                 /* segment */
    movl %esi, 8(%bx)                /* LBA low */
    movl $0, 12(%bx)                 /* LBA high */
    movb $0x80, %dl
    movw $0x4201, %ax                /* AH=42, packet at DS:SI */
    pushl %esi
    movw $0x6e00, %si
    int $0x13
    popl %esi
    jnc chunk_ok2
    /* retry once */
    pushl %esi
    movw $0x4201, %ax
    movw $0x6e00, %si
    movb $0x80, %dl
    int $0x13
    popl %esi
    jnc chunk_ok2
    leaw s_diskfail, %si
    call ser16
    jmp halt16
chunk_ok2:
    addl %ecx, %esi                  /* advance LBA */
    addl %ecx, kernel_loaded
    /* advance ES by chunk size (ecx sectors * 32 paragraphs) */
    movw %cx, %ax
    shlw $5, %ax
    movw %es, %dx
    addw %dx, %ax
    movw %ax, %es
    jmp load_loop
load_done:
    leaw s_loaddone, %si
    call ser16
    /* remember byte size at 0x7020 for the PM copy */
    movl kernel_sector_count, %eax
    shll $9, %eax
    movw $0, %bx
    movw %bx, %es
    movl %eax, 0x7020

    /* ---- switch to protected mode ---- */
    cli
    .byte 0x66, 0x0f, 0x01, 0x16     /* lgdt with 32-bit base */
    .word gdt_ptr
    movl %cr0, %eax
    orl $1, %eax
    movl %eax, %cr0
    ljmpl $0x08, $pm_stub

/* ---- helpers ---- */
ser16:
    pushw %ax
    pushw %dx
sp_loop:
    lodsb
    testb %al, %al
    jz sp_done
    movb %al, %ah
    movw $0x3fd, %dx
sp_wait:
    inb %dx, %al
    testb $0x20, %al
    jz sp_wait
    movw $0x3f8, %dx
    movb %ah, %al
    outb %al, %dx
    jmp sp_loop
sp_done:
    popw %dx
    popw %ax
    ret

    .p2align 4
gdt_ptr:
    .word gdt_end - gdt_start - 1
    .long gdt_start
gdt_start:
    .quad 0x0000000000000000
    .quad 0x00cf9a000000ffff         /* 0x08: 32-bit code, flat */
    .quad 0x00cf92000000ffff         /* 0x10: 32-bit data, flat */
gdt_end:

s_boot:     .asciz "[s2] stage2 alive\n"
s_novbe:    .asciz "[s2] ERROR: no VBE\n"
s_modeok:   .asciz "[s2] vbe mode set\n"
s_setfail:  .asciz "[s2] ERROR: mode set failed\n"
s_nomode:   .asciz "[s2] ERROR: no suitable video mode\n"
s_mem:      .asciz "[s2] memory probed\n"
s_load:     .asciz "[s2] loading kernel\n"
s_diskfail: .asciz "[s2] ERROR: disk read failed\n"
s_loaddone: .asciz "[s2] kernel loaded, entering protected mode\n"

    .code32
pm_stub:
    movl $0x10, %eax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $0x70000, %esp
    /* copy kernel from 0x20000 to 0x100000 */
    movl $0x20000, %esi
    movl $0x100000, %edi
    movl 0x7020, %ecx
    cld
    rep movsb
    jmp $0x08, $0x100000             /* far jump to kernel entry */

    .p2align 4
    .globl kernel_start_lba
kernel_start_lba:   .long 17         /* patched by tools/makedisk.py */
    .globl kernel_sector_count
kernel_sector_count: .long 4096       /* patched by tools/makedisk.py */
kernel_loaded:      .long 0
