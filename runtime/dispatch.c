/* dispatch.c - from a guest address to its lifted function.
 *
 * The table is generated (src/gen/dispatch.c). An address with no lifted
 * function stops the run and names it: it is recorded in
 * work/seeds-kernel.txt, so the next tools/gen.py run lifts it. Nothing is
 * ever executed or interpreted in its place.
 */
#include "temple.h"
#include "ksyms.h"
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t a; void (*fn)(CPU *); } Ent;
typedef struct { uint64_t a; const char *n; } Sym;
extern const Ent g_lifted[]; extern const size_t g_nlifted;
extern const Sym g_syms[]; extern const size_t g_nsyms;

lifted_fn lookup(uint64_t a)
{
    size_t lo = 0, hi = g_nlifted;
    while (lo < hi) {
        size_t m = (lo + hi) / 2;
        if (g_lifted[m].a == a) return g_lifted[m].fn;
        if (g_lifted[m].a < a) lo = m + 1; else hi = m;
    }
    return NULL;
}

const char *sym_near(uint64_t a, uint64_t *off)
{
    size_t lo = 0, hi = g_nsyms;
    while (lo < hi) { size_t m = (lo + hi) / 2; if (g_syms[m].a <= a) lo = m + 1; else hi = m; }
    if (!lo || a - g_syms[lo - 1].a > 0x10000) { *off = a; return "?"; }
    *off = a - g_syms[lo - 1].a;
    return g_syms[lo - 1].n;
}

/* The guest's own frame chain: HolyC functions keep RBP frames. */
void guest_backtrace(CPU *c)
{
    uint64_t bp = c->rbp, off;
    fprintf(stderr, "  rsp %#llx rbp %#llx fs %#llx\n", (unsigned long long)c->rsp,
            (unsigned long long)bp, (unsigned long long)c->fs_base);
    for (int i = 0; i < 16 && bp > 0x1000 && bp < GUEST_RAM - 16; i++) {
        uint64_t ra = rd64(bp + 8);
        fprintf(stderr, "  #%d %#llx %s+%#llx\n", i, (unsigned long long)ra, sym_near(ra, &off), (unsigned long long)off);
        uint64_t nb = rd64(bp);
        if (nb <= bp) break;
        bp = nb;
    }
}

void stop_shot(void)
{
    char p[1024]; snprintf(p, sizeof p, "%s/stop.png", g_work);
    if (present_png(p)) fprintf(stderr, "[shot] %s\n", p);
}

void dispatch_miss(CPU *c, uint64_t a)
{
    stop_shot();
    guest_backtrace(c);
    uint64_t off; const char *s = sym_near(a, &off);
    fprintf(stderr, "no lifted code for %#llx (%s+%#llx), called from near %#llx\n",
            (unsigned long long)a, s, (unsigned long long)off, (unsigned long long)c->rip);
    if ((a >= 0x7C00 && a < K_IMAGE_END) || mods_find(a) >= 0) {
        char p[1024]; snprintf(p, sizeof p, "%s/seeds.txt", g_work);
        FILE *f = fopen(p, "a");
        if (f) { fprintf(f, "0x%llX  # %s+%#llx\n", (unsigned long long)a, s, (unsigned long long)off); fclose(f); }
        fprintf(stderr, "recorded in %s; run tools/gen.py again\n", p);
        fflush(stderr);
        _Exit(20);
    }
    if (capture_module(a)) { fflush(stderr); _Exit(22); }
    if (blob_known(a)) {                     /* a new entry into a block already lifted */
        char p[1024]; snprintf(p, sizeof p, "%s/seeds.txt", g_work);
        FILE *f = fopen(p, "a");
        if (f) { fprintf(f, "0x%llX  # compiler output\n", (unsigned long long)a); fclose(f); }
        fflush(stderr);
        _Exit(20);
    }
    fprintf(stderr, "(code made at run time that was never captured: new code, not run)\n");
    fflush(stderr);
    _Exit(21);
}

int mods_unverified(void);
uint64_t g_trace_fn = ~0ull;
void dispatch(CPU *c, uint64_t a)
{
    lifted_fn f = lookup(a);
    if (!f) f = blob_lookup(a);
    if (!f && a >= K_IMAGE_END && mods_find(a) < 0 && !module_at(a)) f = blob_resolve(c, a);
    if (!f) dispatch_miss(c, a);
    if (a >= K_IMAGE_END && mods_unverified()) mods_find(a);
    if (a == g_trace_fn) {                    /* TEMPLE_TRACE_FN=<hex>: log calls to one function */
        uint64_t a1 = rd64(c->rsp + 8), off;
        char s[48] = {0};
        for (int i = 0; i < 40 && a1 > 0x1000 && a1 < GUEST_RAM && rd8(a1 + i) >= 32 && rd8(a1 + i) < 127; i++) s[i] = (char)rd8(a1 + i);
        fprintf(stderr, "[call %s] args %#llx %#llx %#llx \"%s\"", sym_near(a, &off), (unsigned long long)a1,
                (unsigned long long)rd64(c->rsp + 16), (unsigned long long)rd64(c->rsp + 24), s);
        f(c);
        fprintf(stderr, " -> %#llx\n", (unsigned long long)c->rax);
        return;
    }
    f(c);
}

void dispatch_jmp(CPU *c, uint64_t a) { dispatch(c, a); }
