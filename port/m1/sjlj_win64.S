/* setjmp/longjmp for SDK thread contexts on Windows x64 (see sh4rt.h): saves the callee-saved
   registers of the Microsoft x64 ABI (rbx, rbp, rdi, rsi, r12-r15, xmm6-xmm15), MXCSR and the
   x87 control word, the stack pointer and the return address. longjmp restores them and
   returns 1 from the matching setjmp; no unwinding, no stack checks. Buffer: 256 bytes,
   16-byte aligned. */
	.text
	.globl sh4_setjmp
	.def sh4_setjmp; .scl 2; .type 32; .endef
sh4_setjmp:
	movq %rbx, 0(%rcx)
	movq %rbp, 8(%rcx)
	movq %rdi, 16(%rcx)
	movq %rsi, 24(%rcx)
	movq %r12, 32(%rcx)
	movq %r13, 40(%rcx)
	movq %r14, 48(%rcx)
	movq %r15, 56(%rcx)
	leaq 8(%rsp), %rdx
	movq %rdx, 64(%rcx)
	movq (%rsp), %rdx
	movq %rdx, 72(%rcx)
	stmxcsr 80(%rcx)
	fnstcw 84(%rcx)
	movdqa %xmm6, 96(%rcx)
	movdqa %xmm7, 112(%rcx)
	movdqa %xmm8, 128(%rcx)
	movdqa %xmm9, 144(%rcx)
	movdqa %xmm10, 160(%rcx)
	movdqa %xmm11, 176(%rcx)
	movdqa %xmm12, 192(%rcx)
	movdqa %xmm13, 208(%rcx)
	movdqa %xmm14, 224(%rcx)
	movdqa %xmm15, 240(%rcx)
	xorl %eax, %eax
	ret

	.globl sh4_longjmp
	.def sh4_longjmp; .scl 2; .type 32; .endef
sh4_longjmp:
	movq 0(%rcx), %rbx
	movq 8(%rcx), %rbp
	movq 16(%rcx), %rdi
	movq 24(%rcx), %rsi
	movq 32(%rcx), %r12
	movq 40(%rcx), %r13
	movq 48(%rcx), %r14
	movq 56(%rcx), %r15
	ldmxcsr 80(%rcx)
	fldcw 84(%rcx)
	movdqa 96(%rcx), %xmm6
	movdqa 112(%rcx), %xmm7
	movdqa 128(%rcx), %xmm8
	movdqa 144(%rcx), %xmm9
	movdqa 160(%rcx), %xmm10
	movdqa 176(%rcx), %xmm11
	movdqa 192(%rcx), %xmm12
	movdqa 208(%rcx), %xmm13
	movdqa 224(%rcx), %xmm14
	movdqa 240(%rcx), %xmm15
	movq 64(%rcx), %rsp
	movl $1, %eax
	jmpq *72(%rcx)
