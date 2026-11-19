/* tasks.c - host contexts for TempleOS task switches.
 *
 * A lifted call is a C call, so a guest task's call chain lives on a host
 * stack. TempleOS switches tasks by loading another task's registers and
 * IRETQ-ing to its saved RIP/RSP (Sched.HC, _TASK_CONTEXT_RESTORE), which a
 * C call chain cannot do by itself. So each place a task can be suspended
 * registers a *resume point* - (guest rip, guest rsp) plus a jmp_buf on the
 * host stack that is executing it:
 *
 *   - Yield (_YIELD saves rip=_RET and the rsp at its entry), and
 *   - every interrupt this layer delivers (the frame's rip is a token unique
 *     to that delivery; IRQ_TIMER saves it as the task's rip).
 *
 * An IRETQ then looks its (rip, rsp) up. Found on this host stack: longjmp to
 * it. Found on another: hand that context the CPU and wait. Not found: the
 * task has never run, so a new context starts it with dispatch(rip). Contexts
 * are host threads passing one baton (only one ever runs), so the CPU state is
 * a single shared struct. A context whose points are all consumed can never be
 * resumed again and exits.
 */
#include "temple.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

typedef struct Ctx Ctx;
typedef struct { uint64_t rip, rsp; Ctx *ctx; jmp_buf *jb; uint64_t seq; int used; } Point;
struct Ctx { SDL_Thread *th; SDL_sem *sem; jmp_buf base; int pending; uint64_t start; int id; int npoints; };

#define MAXP 4096
static Point pts[MAXP];
static uint64_t seq;
static Ctx *cur;
static int nctx, live;
static uint64_t irq_token = 0xFFFF800000000000ull;
extern uint64_t K_RET;                        /* _RET, where Yield resumes */

static int add_point(uint64_t rip, uint64_t rsp, jmp_buf *jb)
{
    for (int i = 0; i < MAXP; i++)
        if (!pts[i].used) {
            pts[i] = (Point){rip, rsp, cur, jb, ++seq, 1};
            cur->npoints++;
            return i;
        }
    fprintf(stderr, "out of resume points\n");
    exit(11);
}

static void drop_point(int i) { if (pts[i].used) { pts[i].used = 0; pts[i].ctx->npoints--; } }

/* Consume point i: it and every newer point of its context, which live higher
 * on that host stack and are discarded by the longjmp. */
static void consume(int i)
{
    Ctx *x = pts[i].ctx; uint64_t s = pts[i].seq;
    for (int j = 0; j < MAXP; j++)
        if (pts[j].used && pts[j].ctx == x && pts[j].seq >= s) drop_point(j);
}

static int find_point(uint64_t rip, uint64_t rsp)
{
    for (int i = 0; i < MAXP; i++)
        if (pts[i].used && pts[i].rip == rip && pts[i].rsp == rsp) return i;
    return -1;
}

static int ctx_main(void *arg);

static Ctx *ctx_new(uint64_t rip)
{
    Ctx *x = calloc(1, sizeof *x);
    x->sem = SDL_CreateSemaphore(0);
    x->start = rip;
    x->pending = -1;
    x->id = nctx++;
    live++;
    char name[32]; snprintf(name, sizeof name, "guest%d", x->id);
    x->th = SDL_CreateThreadWithStackSize(ctx_main, name, 256u << 20, x);
    if (!x->th) { fprintf(stderr, "cannot create context: %s\n", SDL_GetError()); exit(11); }
    SDL_DetachThread(x->th);
    return x;
}

/* Give the CPU to x and stop running here. Returns only if this context is
 * resumed later, by longjmp to one of its points (never by a plain return). */
static void switch_to(Ctx *x)
{
    Ctx *me = cur;
    int dead = me->npoints == 0;               /* nothing can resume me: exit */
    cur = x;
    SDL_SemPost(x->sem);
    if (dead) longjmp(me->base, 1);
    SDL_SemWait(me->sem);
    int p = me->pending; me->pending = -1;
    jmp_buf *jb = pts[p].jb;
    consume(p);
    longjmp(*jb, 1);
}

static int ctx_main(void *arg)
{
    Ctx *x = arg;
    SDL_SemWait(x->sem);
    if (setjmp(x->base) == 0) {
        uint64_t rip = x->start;
        if (g_trace) fprintf(stderr, "[task] context %d starts at %#llx\n", x->id, (unsigned long long)rip);
        for (;;) {                  /* a function that returns has popped where to go next */
            dispatch(&g_cpu, rip);
            rip = g_cpu.last_ret;
        }
    }
    if (g_trace) fprintf(stderr, "[task] context %d ends\n", x->id);
    SDL_DestroySemaphore(x->sem);
    live--;
    free(x);
    return 0;
}

void tasks_iret(CPU *c, uint64_t rip)
{
    int p = find_point(rip, c->rsp);
    if (p >= 0) {
        Point pt = pts[p];
        if (pt.ctx == cur) { consume(p); longjmp(*pt.jb, 1); }
        pt.ctx->pending = p;
        switch_to(pt.ctx);
    }
    switch_to(ctx_new(rip));               /* a task that has never run */
}

/* _YIELD, wrapped: the lifted body saves this task and IRETQs to the next.
 * When this task is chosen again it IRETQs to rip=_RET with the rsp we saw at
 * entry, which lands here; _RET is a bare RET, done by hand. */
void hook_yield(CPU *c)
{
    extern void L_YIELD_BODY(CPU *);
    jmp_buf jb;
    int p = add_point(K_RET, c->rsp, &jb);
    if (setjmp(jb) == 0) {
        L_YIELD_BODY(c);                   /* single-user mode returns normally */
        if (pts[p].used && pts[p].jb == &jb) drop_point(p);
        return;
    }
    c->last_ret = rd64(c->rsp);
    c->rsp += 8;
}

void resume_point_irq(CPU *c, void (*deliver)(CPU *, uint64_t))
{
    jmp_buf jb;
    uint64_t tok = irq_token++;
    add_point(tok, c->rsp, &jb);
    if (setjmp(jb) == 0) deliver(c, tok);
}

void tasks_start(CPU *c, uint64_t rip)
{
    (void)c;
    Ctx boot = {0};
    boot.sem = SDL_CreateSemaphore(0);
    cur = &boot;
    Ctx *x = ctx_new(rip);
    cur = x;
    SDL_SemPost(x->sem);
    /* The host thread keeps the window: it never takes the baton. */
}
