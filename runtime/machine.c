/* machine.c - the CPU's system side: ports, MSRs, descriptor tables,
 * interrupts and the guest memory map.
 *
 * Every function here is called from lifted code (cpu64.h, RECOMP64_SYSTEM).
 * Nothing executes guest bytes: an interrupt is delivered by building the
 * frame the hardware would push and calling the lifted handler that the IDT
 * names, at a safe point the lifter marked with SYS_POLL.
 */
#include "temple.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/mman.h>
#endif

CPU g_cpu;
uint8_t *g_mem_base;
uint64_t g_mmio_lo = GUEST_RAM;
int64_t g_image_delta = 0;
volatile int g_irq_pending;
int g_trace;

static uint64_t idt_base, gdt_base;
static uint64_t msr_efer, msr_star, msr_tsc_aux;

uint64_t mem_init(void)
{
    /* One reservation for all of guest physical memory, committed lazily by
     * the host OS. The guest never sees a host address: g_mem_base + va. */
#if defined(_WIN32)
    g_mem_base = VirtualAlloc(NULL, GUEST_RAM + 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    g_mem_base = mmap(NULL, GUEST_RAM + 0x10000, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (g_mem_base == MAP_FAILED) g_mem_base = NULL;
#endif
    if (!g_mem_base) { fprintf(stderr, "cannot reserve guest memory\n"); exit(1); }
    return GUEST_RAM;
}

/* Addresses at or above g_mmio_lo: the kernel's uncached alias of the low
 * 4 GB (dev.uncached_alias), through which it reaches VGA memory, the HPET
 * and the local APIC. RAM through the alias is the same RAM. */
uintptr_t mmio_addr(uint64_t a, int size, int write)
{
    if (a >= ALIAS_BASE && a < ALIAS_END) {
        uint64_t p = a - ALIAS_BASE;
        if (p + size <= GUEST_RAM) return (uintptr_t)(g_mem_base + p);
        return dev_mmio(p, size, write);
    }
    fprintf(stderr, "guest access outside its memory: %#llx (rip %#llx)\n",
            (unsigned long long)a, (unsigned long long)g_cpu.rip);
    exit(4);
}

uint64_t host_ns(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER f; LARGE_INTEGER t;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}

/* ---- ports and system instructions ---- */
uint64_t sys_in(CPU *c, uint32_t port, int size) { (void)c; return dev_in(port, size); }
void sys_out(CPU *c, uint32_t port, uint64_t v, int size) { (void)c; dev_out(port, v, size); }

uint64_t sys_rd(CPU *c, int reg)
{
    (void)c;
    switch (reg) {
    case 0: return 0x80000033;     /* PG | NE | ET | MP | PE */
    case 3: return 0;
    case 4: return 0xB0;
    case 100: return host_ns() * 6364136223846793005ull + 1442695040888963407ull;
    }
    return 0;
}

static void fx_save(CPU *c, uint64_t a)
{
    memset(gptr(a), 0, 512);
    wr16(a, (uint16_t)c->fpu_cw);
    wr16(a + 2, fpu_sw_get(c));
    wr32(a + 24, c->mxcsr ? c->mxcsr : 0x1F80);
    for (int i = 0; i < 8; i++) wrf80(a + 32 + 16 * i, *fst(c, i));
    memcpy(gptr(a + 160), c->xmm, 256);
}
static void fx_restore(CPU *c, uint64_t a)
{
    c->fpu_cw = rd16(a);
    c->fpu_sw = rd16(a + 2);
    c->fpu_top = (c->fpu_sw >> 11) & 7;
    c->mxcsr = rd32(a + 24);
    for (int i = 0; i < 8; i++) *fst(c, i) = rdf80(a + 32 + 16 * i);
    memcpy(c->xmm, gptr(a + 160), 256);
}

void sys_op(CPU *c, int op, uint64_t a, uint64_t b)
{
    switch (op) {
    case SYSOP_LGDT: gdt_base = rd64(a + 2); break;
    case SYSOP_LIDT: idt_base = rd64(a + 2); break;
    case SYSOP_LTR: case SYSOP_MOVCR: case SYSOP_INVLPG: case SYSOP_WBINVD: break;
    case SYSOP_FXSAVE: fx_save(c, a); break;
    case SYSOP_FXRSTOR: fx_restore(c, a); break;
    case SYSOP_RDTSC: {
        uint64_t t = host_ns() * 3;          /* a 3 GHz TSC, from the host clock */
        SET32(c->rax, (uint32_t)t); SET32(c->rdx, (uint32_t)(t >> 32));
        break;
    }
    case SYSOP_WRMSR: {
        uint32_t n = (uint32_t)c->rcx;
        uint64_t v = ((uint64_t)(uint32_t)c->rdx << 32) | (uint32_t)c->rax;
        if (n == 0xC0000100) c->fs_base = v;
        else if (n == 0xC0000101) c->gs_base = v;
        else if (n == 0xC0000080) msr_efer = v;
        else if (n == 0xC0000081) msr_star = v;
        else if (n == 0xC0000103) msr_tsc_aux = v;
        break;
    }
    case SYSOP_RDMSR: {
        uint32_t n = (uint32_t)c->rcx; uint64_t v = 0;
        if (n == 0xC0000100) v = c->fs_base;
        else if (n == 0xC0000101) v = c->gs_base;
        else if (n == 0xC0000080) v = msr_efer | 0x500;
        else if (n == 0x1B) v = 0xFEE00900;          /* APIC base, BSP, enabled */
        SET32(c->rax, (uint32_t)v); SET32(c->rdx, (uint32_t)(v >> 32));
        break;
    }
    case SYSOP_UD2:
        fprintf(stderr, "ud2 at %#llx\n", (unsigned long long)a);
        exit(5);
    default:
        fprintf(stderr, "sys_op %d unhandled at %#llx\n", op, (unsigned long long)c->rip);
        exit(5);
    }
    (void)b;
}

/* ---- interrupts ---- */
static uint64_t idt_target(int vec)
{
    uint64_t g = idt_base + 16ull * vec;
    return (uint64_t)rd16(g) | ((uint64_t)rd16(g + 6) << 16) | ((uint64_t)rd32(g + 8) << 32);
}

static void deliver_vec(CPU *c, uint64_t ret_rip, int vec)
{
    uint64_t rsp = c->rsp, fl = rflags_get(c);
    c->rsp = (c->rsp - 0) & ~0xFull;        /* 64-bit interrupt frames are 16-aligned */
    push64(c, 0x10);                        /* ss */
    push64(c, rsp);
    push64(c, fl);
    push64(c, 0x30);                        /* cs64 */
    push64(c, ret_rip);
    c->ifl = 0;
    uint64_t h = idt_target(vec);
    if (g_trace) fprintf(stderr, "[irq] vec %#x -> %#llx\n", vec, (unsigned long long)h);
    dispatch(c, h);
    fprintf(stderr, "interrupt handler %#llx returned without iret\n", (unsigned long long)h);
    exit(6);
}

int pic_next(void);                          /* devices.c */
int pic_any(void);
static int s_vec;
static void deliver_pic(CPU *c, uint64_t ret_rip) { deliver_vec(c, ret_rip, s_vec); }

void sys_irq_poll(CPU *c)
{
    dev_tick();
    int v = pic_next();
    g_irq_pending = pic_any();
    if (v < 0 || !c->ifl) return;
    s_vec = v;
    resume_point_irq(c, deliver_pic);
}

void sys_hlt(CPU *c)
{
    if (!c->ifl) { fprintf(stderr, "hlt with interrupts off at %#llx\n", (unsigned long long)c->rip); exit(7); }
    for (;;) {
        dev_tick();
        if (pic_any()) break;
        uint64_t t = host_ns();
        while (!g_irq_pending && host_ns() - t < 1000000ull) { /* until the next tick */
#if defined(_WIN32)
            Sleep(0);
#else
            struct timespec ts = {0, 100000}; nanosleep(&ts, NULL);
#endif
        }
        if (g_quit) exit(0);
    }
    sys_irq_poll(c);
}

void sys_iret(CPU *c)
{
    uint64_t rip = pop64(c);
    pop64(c);                                /* cs */
    uint64_t fl = pop64(c);
    uint64_t rsp = pop64(c);
    pop64(c);                                /* ss */
    rflags_set(c, fl);
    c->rsp = rsp;
    tasks_iret(c, rip);
}

void sys_int(CPU *c, int vec, uint64_t ret)
{
    fprintf(stderr, "int %#x at %#llx (software interrupt)\n", vec, (unsigned long long)c->rip);
    (void)ret;
    exit(8);
}

void sys_ret_mismatch(CPU *c, uint64_t expected)
{
    uint64_t o1, o2;
    const char *a = sym_near(expected, &o1), *b = sym_near(c->last_ret, &o2);
    guest_backtrace(c);
    fprintf(stderr, "return mismatch: call site expected %#llx (%s+%#llx), ret went to %#llx (%s+%#llx)\n",
            (unsigned long long)expected, a, (unsigned long long)o1,
            (unsigned long long)c->last_ret, b, (unsigned long long)o2);
    exit(9);
}

void recomp_todo(uint64_t va, const char *what)
{
    fprintf(stderr, "lifter gap at %#llx: %s\n", (unsigned long long)va, what);
    exit(10);
}

void machine_init(void)
{
    memset(&g_cpu, 0, sizeof g_cpu);
    g_cpu.fpu_cw = 0x037F;
    g_cpu.mxcsr = 0x1F80;
}
