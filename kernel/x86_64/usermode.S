/* SCos CPL3 boundary.  One user-mode episode at a time (cooperative kernel).
 *
 * user_enter(entry, ursp, arg) pushes the kernel continuation on the current
 * kernel stack, records RSP, and SYSRETQs into CPL3.  Every kernel service is
 * reached with SYSCALL.  SC_NR_EXIT (or a contained hardware fault) returns to
 * user_enter's C caller, so the WM sees an ordinary function call.
 *
 * SYSCALL does not switch RSP and clobbers RCX/R11; we save all three.  It also
 * does not switch CR3 - that is deliberate: the user PML4 keeps every kernel
 * mapping but marks them supervisor-only, so CPL0 code can still reach kernel
 * data while CPL3 code cannot.
 */
.section .text
.code64

.globl sc_user_active
.globl sc_user_krsp
.globl sc_user_ursp
.globl sc_user_urip
.globl sc_user_urflags
.globl sc_user_snr
.section .data
.balign 8
sc_user_active:  .quad 0
sc_user_krsp:    .quad 0
sc_user_ursp:    .quad 0
sc_user_urip:    .quad 0
sc_user_urflags: .quad 0
sc_user_snr:     .quad 0
sc_user_kflags:  .quad 0
.section .text

/* LSTAR target.  CPL0 here, still on the user RSP, user CR3 still loaded. */
.globl sc_syscall_entry
sc_syscall_entry:
    movq %rsp, sc_user_ursp(%rip)
    movq %rcx, sc_user_urip(%rip)
    movq %r11, sc_user_urflags(%rip)
    movq %rax, sc_user_snr(%rip)
    movq sc_user_krsp(%rip), %rsp
    /* SysV args: rdi, rsi, rdx, rcx, r8, r9.  SYSCALL puts arg4 in r10. */
    movq %r10, %rcx
    call user_syscall_dispatch
    /* SC_NR_EXIT sets sc_user_active to 2; return to user_enter's caller. */
    cmpl $2, sc_user_active(%rip)
    je   sc_user_exit_return
    /* Normal service: restore CPL3 context and go back. */
    movq sc_user_ursp(%rip), %rsp
    movq sc_user_urip(%rip), %rcx
    movq sc_user_urflags(%rip), %r11
    movl $1, sc_user_active(%rip)
    sysretq

/* RSP is still sc_user_krsp here: the six callee-saved registers that
 * user_enter pushed are on top of it. */
.globl sc_user_exit_return
sc_user_exit_return:
    movl $0, sc_user_active(%rip)
    pushq sc_user_kflags(%rip)
    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    ret

/* Exception while in CPL3: interrupt_dispatch rewrote the frame so IRETQ
 * lands here at CPL0.  Adopt the saved kernel stack and return an error. */
.globl sc_user_fault_return
sc_user_fault_return:
    movq sc_user_krsp(%rip), %rsp
    movl $0, sc_user_active(%rip)
    pushq sc_user_kflags(%rip)
    popfq
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbp
    popq %rbx
    movq $0xffffffffffffffff, %rax
    ret

/* uint64_t user_enter(uint64_t entry, uint64_t user_rsp, uint64_t arg) */
.globl user_enter
user_enter:
    pushq %rbx
    pushq %rbp
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    pushfq
    popq sc_user_kflags(%rip)    /* exact kernel RFLAGS to restore on return */
    movq %rsp, sc_user_krsp(%rip)
    movl $1, sc_user_active(%rip)
    movq %rdi, %rcx              /* user RIP */
    movq %rsi, %rsp              /* user RSP */
    movq %rdx, %rdi              /* arg */
    movq $0x202, %r11            /* user RFLAGS: IF=1, bit1 reserved */
    xorl %eax, %eax
    sysretq

.section .note.GNU-stack,"",@progbits

/* Dedicated RSP0 for interrupts that arrive while CPL3 is running.  Ring-0
 * interrupts never switch stacks, so repointing RSP0 is safe. */
.section .bss.stacks,"aw",@nobits
.balign 4096
.globl user_irq_stack_guard, user_irq_stack_top
user_irq_stack_guard: .skip 4096
    .skip 65536
user_irq_stack_top:
