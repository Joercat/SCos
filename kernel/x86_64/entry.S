.section .text.entry,"ax"
.code64
.globl _start
/* RDI is the fixed low-memory boot handoff. No inherited stack is trusted.
 * Keep that pointer in a register while clearing BSS, including our new stacks.
 * A 16-byte aligned RSP BEFORE call gives SysV's (RSP + 8) alignment in C.
 * No FPU/SIMD or TLS state is consumed by this ring-0-only build. */
_start:
    cli
    cld
    movq %rdi,%r12
    leaq _bss_start(%rip),%rdi
    leaq _bss_end(%rip),%rcx
    subq %rdi,%rcx
    xorl %eax,%eax
    rep stosb
    leaq stack_top(%rip),%rsp
    andq $-16,%rsp
    xorq %rbp,%rbp
    movq %r12,%rdi
    call kernel_main
1:  cli
    hlt
    jmp 1b
.globl load_gdt
/* Reload CS through a far return; LGDT alone does not replace cached segment
 * descriptors. Data selectors are then refreshed before TSS/IDT installation. */
load_gdt:
    lgdt (%rdi)
    pushq $8
    leaq 1f(%rip),%rax
    pushq %rax
    lretq
1:  movw $16,%ax
    movw %ax,%ds
    movw %ax,%es
    movw %ax,%ss
    movw %ax,%fs
    movw %ax,%gs
    ret
.section .bss.stacks,"aw",@nobits
.balign 4096
.globl stack_guard,stack_top,df_guard,df_top,nmi_guard,nmi_top,mc_guard,mc_top
stack_guard: .skip 4096
    .skip 65536
stack_top:
df_guard: .skip 4096
    .skip 16384
df_top:
nmi_guard: .skip 4096
    .skip 16384
nmi_top:
mc_guard: .skip 4096
    .skip 16384
mc_top:
.section .note.GNU-stack,"",@progbits
