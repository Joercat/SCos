/* SysV AMD64 non-local return; no libc TLS, signals or pointer mangling.
 * FPU control state is retained as well as all callee-saved registers. */
.text
.globl scos_lua_setjmp
.type scos_lua_setjmp,@function
scos_lua_setjmp:
 movq %rbx,0(%rdi)
 movq %rbp,8(%rdi)
 movq %r12,16(%rdi)
 movq %r13,24(%rdi)
 movq %r14,32(%rdi)
 movq %r15,40(%rdi)
 leaq 8(%rsp),%rax
 movq %rax,48(%rdi)
 movq (%rsp),%rax
 movq %rax,56(%rdi)
 stmxcsr 64(%rdi)
 fnstcw 68(%rdi)
 xorl %eax,%eax
 ret
.globl scos_lua_longjmp
.type scos_lua_longjmp,@function
scos_lua_longjmp:
 movl %esi,%eax
 testl %eax,%eax
 jnz 1f
 incl %eax
1:
 movq 0(%rdi),%rbx
 movq 8(%rdi),%rbp
 movq 16(%rdi),%r12
 movq 24(%rdi),%r13
 movq 32(%rdi),%r14
 movq 40(%rdi),%r15
 ldmxcsr 64(%rdi)
 fldcw 68(%rdi)
 movq 48(%rdi),%rsp
 jmp *56(%rdi)
.section .note.GNU-stack,"",@progbits
