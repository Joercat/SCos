/* Entry and the ONLY way out of CPL3: the SYSCALL instruction. */
.section .text.user.entry,"ax"
.code64
.globl user_start
user_start:
    cld
    movq %rdi, %r12              /* shared page pointer */
    xorl %ebp, %ebp
    /* 64 KiB stack is mapped just below USER_STACK_TOP by the kernel. */
    movq $0x600000290000, %rsp
    andq $-16, %rsp
    movq %r12, %rdi
    call user_main
    /* user_main should never return; exit anyway. */
    movl $1, %eax                /* SC_NR_EXIT */
    xorl %edi, %edi
    xorl %esi, %esi
    xorl %edx, %edx
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    xorl %r9d, %r9d
    syscall
1:  jmp 1b

/* long sc(long nr, long a1, long a2, long a3, long a4, long a5)
 * Six SysV register arguments; SYSCALL uses r10 instead of rcx for arg 4 and
 * clobbers rcx/r11, so the stub reshuffles before executing SYSCALL. */
.section .text
.globl sc
.type sc,@function
sc:
    movq %rdi, %rax              /* nr */
    movq %rsi, %rdi              /* a1 */
    movq %rdx, %rsi              /* a2 */
    movq %rcx, %rdx              /* a3 */
    movq %r8,  %r10              /* a4 -> SYSCALL's r10 */
    movq %r9,  %r8               /* a5 */
    movq $0,   %r9
    syscall
    ret

.section .note.GNU-stack,"",@progbits
