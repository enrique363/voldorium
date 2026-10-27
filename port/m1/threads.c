/* SDK threads on host stacks.
 *
 * The Katana thread library switches threads with a setjmp/longjmp pair:
 *   8C102E84 save(r4 = buffer): stores r15, r14, PR, r0-r13, MACH/L, GBR, FPSCR, FPUL, FR0-15
 *            and returns 0;
 *   8C102E10 restore(r4 = buffer, r5 = value): reloads them and "returns" from the matching
 *            save with r0 = value (1 if value is 0), on the saved stack.
 * The switch routines call save and then, later in the same function, restore another
 * thread's buffer. A context therefore resumes in the middle of the function that saved it,
 * so the host side must use real setjmp/longjmp in that function's C frame: the translator
 * emits `setjmp(*sh4_ctx_slot(c, buffer))` around every call to the save routine, and
 * restore longjmps to the slot of its buffer. Each SH-4 thread runs on its own host stack;
 * a buffer that was never saved (the library prepared it by hand) starts a new thread there,
 * at the buffer's PR.
 *
 * Linux: threads are ucontext stacks and a restore longjmps straight onto the saved stack.
 * Windows: threads are fibers. The stack limits live in the TEB and only SwitchToFiber updates
 * them, so a restore whose context was saved on another fiber switches to that fiber, which
 * then longjmps to the context itself (it was suspended in this same function).
 */
/* longjmp between host stacks is the point here; glibc's fortified longjmp rejects it */
#undef _FORTIFY_SOURCE
#define _FORTIFY_SOURCE 0
#include "m1.h"
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <ucontext.h>
#endif

void (*m1_translated(u32 a))(SH4 *);

#define MAX_CTX 64
#define HOST_STACK (1u << 20)

struct slot
{
	u32 buf;
	int valid;		/* a setjmp in a live frame holds this buffer's context */
	int in_irq;		/* interrupt state of that thread when it saved */
	sh4_jmp_buf jb;
#ifdef _WIN32
	void *fiber;	/* fiber the context was saved on */
	void *own;		/* fiber of a thread started here */
#else
	void *stack;	/* host stack, for threads started here */
	ucontext_t uc;
#endif
};
static struct slot slots[MAX_CTX];
static int nslots;
static u64 switches, threads_started;

int m1_get_irq(void);
void m1_set_irq(int in_irq);

static struct slot *find(u32 buf)
{
	for (int i = 0; i < nslots; i++)
		if (slots[i].buf == buf)
			return &slots[i];
	if (nslots == MAX_CTX)
		m1_stop("too many thread contexts");
	memset(&slots[nslots], 0, sizeof(slots[0]));
	slots[nslots].buf = buf;
	return &slots[nslots++];
}

#ifdef _WIN32
static struct slot *pending;	/* context to resume once the fiber switched to runs */
static void *cur_fiber(void)
{
	static void *main_fiber;
	if (!main_fiber)
		main_fiber = ConvertThreadToFiber(0);
	return GetCurrentFiber();
}
static void resume_pending(void)
{
	struct slot *p = pending;
	pending = 0;
	if (p)
	{
		m1_set_irq(p->in_irq);
		sh4_longjmp(p->jb);
	}
}

void *m1_fiber_current(void) { return cur_fiber(); }	/* netplay.c */
#endif

sh4_jmp_buf *sh4_ctx_slot(SH4 *c, u32 buf)
{
	(void)c;
	struct slot *s = find(buf);
	s->valid = 1;
	s->in_irq = m1_get_irq();
#ifdef _WIN32
	s->fiber = cur_fiber();
#endif
	return &s->jb;
}

void sh4_ctx_resumed(SH4 *c)
{
	sh4_set_host_fpmode(c->fpscr);
}

#ifdef _WIN32
static void CALLBACK thread_entry(void *arg)
#else
static void thread_entry(void)
#endif
{
#ifdef _WIN32
	(void)arg;
#endif
	SH4 *c = m1_cpu;
	threads_started++;
	printf("thread start at %08x, stack %08x\n", c->pr, c->r[15]);
	m1_set_irq(0);
	sh4_set_host_fpmode(c->fpscr);
	/* The thread starts by "returning" to the saved PR. Each time translated code returns to
	   this base frame, execution continues at the PR it left behind, as rts does on hardware. */
	u32 target = c->pr;
	for (;;)
	{
		c->pr = 0xfffffffeu;
		sh4_call(c, target);
		target = c->pr;
		if (target == 0xfffffffeu)
			m1_stop("a thread's entry function returned to its base");
	}
}

int thread_hle(SH4 *c, u32 addr)
{
	if (addr != 0x8c102e10u)
		return 0;	/* the save routine runs as translated code (its caller did the setjmp) */
	const u32 buf = c->r[4];
	struct slot *s = find(buf);
	m1_translated(0x8c102e10u)(c);	/* loads r0-r15, PR, MAC, GBR, FPSCR, FPUL, FR; r0 = return value */
	switches++;
#ifdef _WIN32
	if (s->valid)
	{
		pending = s;
		if (s->fiber != cur_fiber())
			SwitchToFiber(s->fiber);	/* that fiber resumes below and makes the jump */
		resume_pending();
	}
	/* never saved: a new thread starting at the buffer's PR, on a fiber of its own */
	if (s->own == cur_fiber())
		m1_stop("a thread restarts itself from an unsaved context");
	if (s->own)
		DeleteFiber(s->own);
	s->own = CreateFiberEx(64 << 10, 8u << 20, 0, thread_entry, 0);
	if (!s->own)
		m1_stop("CreateFiberEx failed");
	SwitchToFiber(s->own);
	resume_pending();	/* back on this fiber: another thread restored one of its contexts */
	m1_stop("fiber resumed without a context");
	return 1;
#else
	if (s->valid)
	{
		m1_set_irq(s->in_irq);
		sh4_longjmp(s->jb);
	}
	/* never saved: a new thread starting at the buffer's PR on the buffer's stack */
	if (!s->stack)
		s->stack = malloc(HOST_STACK);
	getcontext(&s->uc);
	s->uc.uc_stack.ss_sp = s->stack;
	s->uc.uc_stack.ss_size = HOST_STACK;
	s->uc.uc_link = 0;
	makecontext(&s->uc, thread_entry, 0);
	setcontext(&s->uc);
	return 1;
#endif
}

void thread_report(FILE *f)
{
	fprintf(f, "%llu thread switches, %llu threads started, %d contexts\n",
			(unsigned long long)switches, (unsigned long long)threads_started, nslots);
}
