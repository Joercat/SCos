#!/usr/bin/env python3
"""Generate all AMD64 IDT stubs; frame order matches interrupt_frame in kernel.h."""
print('.section .text\n.code64')
for n in range(256):
    print(f'isr_{n}:')
    if n not in (8,10,11,12,13,14,17,21,29,30):
        print('    pushq $0')
    print(f'    pushq ${n}\n    jmp isr_common')
regs='rax rbx rcx rdx rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15'.split()
print('isr_common:\n    cld')
for r in regs: print(f'    pushq %{r}')
print('''    movq %rsp,%rdi
    movq %rsp,%rbx
    andq $-16,%rsp
    call interrupt_dispatch
    movq %rbx,%rsp''')
for r in reversed(regs): print(f'    popq %{r}')
print('    addq $16,%rsp\n    iretq\n.section .rodata\n.balign 8\n.globl isr_stubs\nisr_stubs:')
for n in range(256): print(f'    .quad isr_{n}')
print('.section .note.GNU-stack,"",@progbits')
