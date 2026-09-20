/* BIOS -> AMD64 bootstrap. See docs/migration/BOOT64.md for the fixed ABI.
 * E820:5000..55ff, handoff:7000..704f, stack:6f00, stage2:8000..9fff,
 * paging:10000..15fff, disk staging:20000..7ffff. Never switch boot disks. */
.code16
.section .text
.globl _start
_start:
    cli
    xorw %ax,%ax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movw $0x6f00,%sp
    cld
    movw $3,%ax
    int $0x10
    xorw %ax,%ax
    movw %ax,%ds
    movw %ax,%es
    movw $0x3fb,%dx
    movb $0x80,%al
    outb %al,%dx
    movw $0x3f8,%dx
    movb $1,%al
    outb %al,%dx
    movw $0x3f9,%dx
    xorb %al,%al
    outb %al,%dx
    movw $0x3fb,%dx
    movb $3,%al
    outb %al,%dx
    leaw hello,%si
    call print
    /* Check CPUID existence before executing it. */
    pushfl
    popl %eax
    movl %eax,%ecx
    xorl $0x200000,%eax
    pushl %eax
    popfl
    pushfl
    popl %eax
    pushl %ecx
    popfl
    xorl %ecx,%eax
    testl $0x200000,%eax
    jz cpu_bad
    movl $1,%eax
    cpuid
    andl $0x60,%edx
    cmpl $0x60,%edx
    jne cpu_bad
    movl $0x80000000,%eax
    cpuid
    cmpl $0x80000001,%eax
    jb cpu_bad
    movl $0x80000001,%eax
    cpuid
    andl $0x20100000,%edx
    cmpl $0x20100000,%edx
    jne cpu_bad

    movw $0x7000,%di
    movw $40,%cx
    xorw %ax,%ax
    rep stosw
    movl $0x4f424353,0x7000
    movl $0x3436544f,0x7004
    movl $1,0x7008
    movl $80,0x700c
    movl $0x5000,0x7010
    movl $24,0x701c
    movl $0x100000,0x7020
    movl kernel_memory_end,%eax
    movl %eax,0x7028
    movl kernel_file_bytes,%eax
    addl $0x100000,%eax
    movl %eax,0x7030
    movl $0x10000,0x7038
    movzbl 0x7b00,%eax
    movl %eax,0x7040
    movl $1,0x7044
    xorl %ebx,%ebx
    movw $0x5000,%di
map_next:
    cmpl $64,0x7018
    jae map_bad
    movl $1,20(%di)
    movl $0xe820,%eax
    movl $0x534d4150,%edx
    movl $24,%ecx
    pushw %ds
    pushw %es
    pushw %di
    pushw %si
    pushw %bp
    int $0x15
    popw %bp
    popw %si
    popw %di
    popw %es
    popw %ds
    jc map_end
    cmpl $0x534d4150,%eax
    jne map_bad
    cmpl $20,%ecx
    jb map_bad
    testl $1,20(%di)
    jz map_record
    movl (%di),%eax
    movl 4(%di),%edx
    addl 8(%di),%eax
    adcl 12(%di),%edx
    jc map_bad
map_record:
    incl 0x7018
    addw $24,%di
    testl %ebx,%ebx
    jnz map_next
map_end:
    cmpl $0,0x7018
    je map_bad
    /* Validate staging, tables and complete kernel/BSS span BEFORE writing. */
    cmpl $0,kernel_file_bytes
    je map_bad
    movl kernel_sector_count,%eax
    testl %eax,%eax
    jz map_bad
    cmpl $768,%eax
    ja map_bad
    shll $9,%eax
    cmpl kernel_file_bytes,%eax
    jb map_bad
    movl $0x20000,%esi
    leal 0x20000(%eax),%edi
    call ram_range
    jc map_bad
    movl $0x10000,%esi
    movl $0x16000,%edi
    call ram_range
    jc map_bad
    movl $0x100000,%esi
    movl kernel_memory_end,%edi
    cmpl $0x100000,%edi
    jbe map_bad
    cmpl $0x1000000,%edi
    ja map_bad
    cmpl 0x7030,%edi
    jb map_bad
    call ram_range
    jc map_bad
    leaw map_ok,%si
    call print
    call a20_test
    jnc a20_ok
    inb $0x92,%al
    orb $2,%al
    andb $0xfe,%al
    outb %al,$0x92
    call a20_test
    jnc a20_ok
    call kbd_wait
    movb $0xd1,%al
    outb %al,$0x64
    call kbd_wait
    movb $0xdf,%al
    outb %al,$0x60
    call kbd_wait
    call a20_test
    jc a20_bad
a20_ok:
    movw $0x55aa,%bx
    movb $0x41,%ah
    movb 0x7b00,%dl
    pushw %ds
    pushw %es
    stc
    int $0x13
    popw %es
    popw %ds
    jc disk_bad
    cmpw $0xaa55,%bx
    jne disk_bad
    testw $1,%cx
    jz disk_bad
    movl kernel_start_lba,%eax
    movl %eax,dap+8
    movw $0x2000,dap+6
    movl kernel_sector_count,%eax
    movl %eax,remaining
read_next:
    movw $1,dap+2 /* one sector: no DMA 64-KiB boundary crossing */
    movw $dap,%si
    movb 0x7b00,%dl
    movb $0x42,%ah
    pushal
    pushw %ds
    pushw %es
    stc
    int $0x13
    popw %es
    popw %ds
    popal
    jc disk_bad
    incl dap+8
    addw $32,dap+6
    decl remaining
    jnz read_next
    leaw loaded,%si
    call print
    cli
    movb $0x80,%al /* mask NMI across mode/table transition */
    outb %al,$0x70
    lgdt gdt_ptr
    movl %cr0,%eax
    orl $1,%eax
    movl %eax,%cr0
    ljmpl $8,$protected
cpu_bad: leaw cpu_error,%si
    jmp fatal
map_bad: leaw map_error,%si
    jmp fatal
disk_bad: leaw disk_error,%si
    jmp fatal
a20_bad: leaw a20_error,%si
fatal:
    call print
1:  cli
    hlt
    jmp 1b

/* [ESI,EDI) must fit usable RAM and intersect no enabled reserved entry. */
ram_range:
    pushal
    movb $0,found
    movl $0x5000,%ecx
    movl 0x7018,%ebp
range_next:
    testl $1,20(%ecx)
    jz range_skip
    cmpl $0,4(%ecx)
    jne range_skip
    movl (%ecx),%eax
    movl 8(%ecx),%edx
    cmpl $0,12(%ecx)
    jne range_large
    testl %edx,%edx
    jz range_skip
    addl %eax,%edx
    jnc range_test
range_large:
    movl $0xffffffff,%edx
range_test:
    cmpl %esi,%edx
    jbe range_skip
    cmpl %edi,%eax
    jae range_skip
    cmpl $1,16(%ecx)
    jne range_fail
    cmpl %esi,%eax
    ja range_skip
    cmpl %edi,%edx
    jb range_skip
    movb $1,found
range_skip:
    addl $24,%ecx
    decl %ebp
    jnz range_next
    cmpb $1,found
    jne range_fail
    popal
    clc
    ret
range_fail:
    popal
    stc
    ret

a20_test:
    pushw %es
    movw $0xffff,%ax
    movw %ax,%es
    movb 0x6c00,%al
    movb %es:0x6c10,%ah
    pushw %ax
    movb $0,0x6c00
    movb $0xff,%es:0x6c10
    cmpb $0xff,0x6c00
    popw %ax
    movb %ah,%es:0x6c10
    movb %al,0x6c00
    popw %es
    je 1f
    clc
    ret
1:  stc
    ret
kbd_wait:
    movw $0xffff,%cx
1:  inb $0x64,%al
    testb $2,%al
    jz 2f
    loop 1b
2:  ret
print:
    cld
    lodsb
    testb %al,%al
    jz 3f
    pushal
    pushw %ds
    pushw %es
    movb %al,%bl
    movw $0x3fd,%dx
    movw $0xffff,%cx
1:  inb %dx,%al
    testb $0x20,%al
    jnz 2f
    loop 1b
    jmp 4f
2:  movb %bl,%al
    movw $0x3f8,%dx
    outb %al,%dx
4:  movb %bl,%al
    movw $7,%bx
    movb $0x0e,%ah
    int $0x10
    popw %es
    popw %ds
    popal
    cld
    jmp print
3:  ret
.p2align 3
dap: .word 16,1,0,0x2000
    .quad 17
remaining: .long 0
found: .byte 0
hello: .asciz "SCos: BIOS -> x86-64 startup\r\n"
map_ok: .asciz "E820 map and boot ranges verified\r\n"
loaded: .asciz "Kernel loaded; enabling long mode\r\n"
cpu_error: .asciz "HALTED: CPU requires CPUID, MSR, PAE, long mode and NX\r\n"
map_error: .asciz "HALTED: invalid/incomplete E820 or unsafe boot range\r\n"
disk_error: .asciz "HALTED: EDD read failed on BIOS boot drive\r\n"
a20_error: .asciz "HALTED: A20 unavailable\r\n"
.p2align 3
gdt_ptr: .word gdt_end-gdt-1
    .long gdt
gdt:
    .quad 0,0x00cf9a000000ffff,0x00cf92000000ffff,0x00af9a000000ffff
gdt_end:
.code32
protected:
    movw $16,%ax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movl $0x6f00,%esp
    cld
    /* Validate disk payload before executing it or changing the destination. */
    movl $0x20000,%esi
    movl kernel_file_bytes,%ecx
    movl $0xffffffff,%edx
crc_byte:
    movzbl (%esi),%eax
    xorl %eax,%edx
    movl $8,%ebx
crc_bit:
    movl %edx,%eax
    andl $1,%eax
    negl %eax
    andl $0xedb88320,%eax
    shrl $1,%edx
    xorl %eax,%edx
    decl %ebx
    jnz crc_bit
    incl %esi
    decl %ecx
    jnz crc_byte
    notl %edx
    cmpl kernel_crc32,%edx
    jne checksum_bad
    movl $0x20000,%esi
    movl $0x100000,%edi
    movl kernel_file_bytes,%ecx
    rep movsb
    /* Temporary 0..4-GiB identity map, NX outside first 2 MiB. Replaced by
     * sparse kernel-owned 4-KiB W^X mappings before enabling interrupts. */
    movl $0x10000,%edi
    xorl %eax,%eax
    movl $0x6000/4,%ecx
    rep stosl
    movl $0x11003,0x10000
    movl $0x12003,0x11000
    movl $0x13003,0x11008
    movl $0x14003,0x11010
    movl $0x15003,0x11018
    movl $0x12000,%edi
    movl $0x83,%eax
    movl $2048,%ecx
1:  movl %eax,(%edi)
    movl $0x80000000,4(%edi)
    addl $0x200000,%eax
    addl $8,%edi
    loop 1b
    movl $0,0x12004
    movl %cr4,%eax
    orl $0x20,%eax
    movl %eax,%cr4
    movl $0x10000,%eax
    movl %eax,%cr3
    movl $0xc0000080,%ecx
    rdmsr
    orl $0x900,%eax
    wrmsr
    movl %cr0,%eax
    orl $0x80010000,%eax
    movl %eax,%cr0
    ljmpl $24,$long_mode
checksum_bad:
    movl $checksum_error,%esi
    movl $0xb8000,%edi
1:  lodsb
    testb %al,%al
    jz 4f
    movb %al,%bl
    movb $0x4f,%ah
    stosw
    movl $65536,%ecx
    movw $0x3fd,%dx
2:  inb %dx,%al
    testb $0x20,%al
    jnz 3f
    loop 2b
    jmp 1b
3:  movb %bl,%al
    movw $0x3f8,%dx
    outb %al,%dx
    jmp 1b
4:  cli
    hlt
    jmp 4b
checksum_error: .asciz "HALTED: kernel checksum mismatch"
.code64
long_mode:
    movw $16,%ax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movl $0x7000,%edi
    movl $0x100000,%eax
    jmp *%rax
.p2align 3
.globl kernel_start_lba,kernel_sector_count,kernel_file_bytes,kernel_memory_end,kernel_crc32
kernel_start_lba: .long 17
kernel_sector_count: .long 0
kernel_file_bytes: .long 0
kernel_memory_end: .long 0
kernel_crc32: .long 0
.section .note.GNU-stack,"",@progbits
