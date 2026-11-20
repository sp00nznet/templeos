/* blobs.c - the HolyC compiler's output, matched to its lifted form.
 *
 * Every function, statement and constant expression TempleOS compiles at run
 * time lands in a heap block (MAlloc on the code heap). The compiler often
 * makes the same code again at another address (a constant expression is
 * compiled, called once and freed), so lifted blocks are keyed by their bytes
 * alone: tools/gen.py lifts each one at a nominal base with its own addresses
 * written as B + offset, and the dispatcher supplies B, where this copy lives.
 *
 * When lifted code first reaches an address in a block, the block is found
 * (from the COCCompile hook's record, else from TempleOS's own heap header,
 * CMemUsed: heap control, size), hashed, and looked up. A match activates the
 * block's entry points at this address; no match writes the block (and every
 * other block compiled so far that is not lifted yet) to work/jit/ and stops
 * the run. When the guest frees a block, its entries are dropped, so other
 * code later at the same address never reaches stale lifted code.
 */
#include "temple.h"
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define mkdir_(p) _mkdir(p)
#else
#define mkdir_(p) mkdir(p, 0777)
#endif

typedef struct { uint64_t off; lifted_fn fn; } BlobEnt;
typedef struct { uint64_t size, key; const BlobEnt *ents; size_t nents; const uint32_t *mask; size_t nmask; } Blob;
extern const Blob g_blobs[]; extern const size_t g_nblobs;
uint64_t g_blob_base;                           /* B for the lifted block being entered */

#define OVB 16
static struct { uint64_t a, base; lifted_fn fn; } ov[1 << OVB];
static uint64_t act_lo = ~0ull, act_hi;

static uint64_t fnv_mem(uint64_t a, uint64_t n)
{
    uint64_t h = 1469598103934665603ull; const uint8_t *p = gptr(a);
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}
static size_t slot(uint64_t a) { return (size_t)((a * 0x9E3779B97F4A7C15ull) >> (64 - OVB)); }
static void ov_put(uint64_t a, uint64_t base, lifted_fn fn)
{
    for (size_t i = slot(a);; i = (i + 1) & ((1 << OVB) - 1))
        if (!ov[i].a || ov[i].a == a) { ov[i].a = a; ov[i].base = base; ov[i].fn = fn; return; }
}
lifted_fn blob_lookup(uint64_t a)
{
    if (a < act_lo || a >= act_hi) return NULL;
    for (size_t i = slot(a);; i = (i + 1) & ((1 << OVB) - 1)) {
        if (!ov[i].a) return NULL;
        if (ov[i].a == a) { g_blob_base = ov[i].base; return ov[i].fn; }
    }
}
int blob_known(uint64_t a) { return a >= act_lo && a < act_hi; }

/* The heap block holding a: [start, start+size), from CMemUsed {hc, size}. */
static int heap_block(uint64_t a, uint64_t *start, uint64_t *size)
{
    for (uint64_t p = a & ~7ull; p + (1u << 20) > a && p > 0x1000; p -= 8) {
        uint64_t hc = rd64(p - 16), sz = rd64(p - 8);
        if (hc < 0x1000 || hc >= GUEST_RAM - 16) continue;
        if (rd32(hc + 8) != 0x56536348u) continue;          /* HEAP_CTRL_SIGNATURE_VAL */
        if (sz < 16 || sz > (64u << 20) || p - 16 + sz <= a) continue;
        *start = p; *size = sz - 16;
        return 1;
    }
    return 0;
}

/* Every block COCCompile returned, with its exact code size (the heap block
 * is rounded up, and the slack holds whatever was there before). */
static struct { uint64_t a, n; int live; } made[1 << 16];
static int nmade;

static int code_size(uint64_t st, uint64_t heap_sz)
{
    for (int i = nmade - 1; i >= 0; i--) if (made[i].a == st && made[i].live) return (int)made[i].n;
    return (int)heap_sz;
}

/* A lifted block whose bytes match these, apart from the fields its lifted
 * code reads at run time (immediates, displacements, call targets). */
static const Blob *find_blob(uint64_t st, uint64_t n)
{
    for (size_t i = 0; i < g_nblobs; i++) {
        const Blob *b = &g_blobs[i];
        if (b->size != n) continue;
        uint64_t h = 1469598103934665603ull; const uint8_t *p = gptr(st);
        size_t m = 0;
        for (uint64_t k = 0; k < n; k++) {
            while (m < b->nmask && k >= b->mask[2 * m] + b->mask[2 * m + 1]) m++;
            uint8_t v = (m < b->nmask && k >= b->mask[2 * m]) ? 0 : p[k];
            h ^= v; h *= 1099511628211ull;
        }
        if (h == b->key) return b;
    }
    return NULL;
}

static void save_block(uint64_t st, uint64_t sz, uint64_t h)
{
    char dir[1024], p[1200];
    snprintf(dir, sizeof dir, "%s/jit", g_work); mkdir_(dir);
    snprintf(p, sizeof p, "%s/blob_%llX_%016llX.bin", dir, (unsigned long long)st, (unsigned long long)h);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(gptr(st), 1, sz, f); fclose(f); }
    snprintf(p, sizeof p, "%s/index.txt", dir);
    f = fopen(p, "a");
    if (f) { fprintf(f, "blob 0x%llX 0x%llX 0x%016llX%c", (unsigned long long)st, (unsigned long long)sz, (unsigned long long)h, 10); fclose(f); }
}

/* COCCompile(cc, _code_size, _dbg, _type), wrapped: note the block it made. */
void hook_coccompile(CPU *c)
{
    extern lifted_fn g_body_COCCompile;
    uint64_t psize = rd64(c->rsp + 16);
    g_body_COCCompile(c);
    uint64_t res = c->rax, st, sz;
    if (res && heap_block(res, &st, &sz) && st == res && nmade < (1 << 16)) {
        uint64_t n = psize ? rd64(psize) : sz;
        made[nmade].a = st; made[nmade].n = n && n <= sz ? n : sz; made[nmade].live = 1;
        nmade++;
    }
}

/* Called on a dispatch miss outside the kernel and the captured modules. */
lifted_fn blob_resolve(CPU *c, uint64_t a)
{
    uint64_t st, hsz;
    if (!heap_block(a, &st, &hsz)) return NULL;
    uint64_t sz = (uint64_t)code_size(st, hsz);
    uint64_t h = fnv_mem(st, sz);
    const Blob *b = find_blob(st, sz);
    if (b) {
        for (size_t k = 0; k < b->nents; k++) ov_put(st + b->ents[k].off, st, b->ents[k].fn);
        if (st < act_lo) act_lo = st;
        if (st + sz > act_hi) act_hi = st + sz;
        return blob_lookup(a);                 /* NULL: a new entry into a known block */
    }
    /* New compiler output: write it down for lifting, with every other block
     * compiled so far that is not lifted yet, so one stop captures a batch. */
    save_block(st, sz, h);
    for (int i = 0; i < nmade; i++)
        if (made[i].live && made[i].a != st) {
            if (!find_blob(made[i].a, made[i].n)) save_block(made[i].a, made[i].n, fnv_mem(made[i].a, made[i].n));
        }
    fprintf(stderr, "captured compiler output at %#llx (%llu bytes)%s; run tools/gen.py again\n",
            (unsigned long long)st, (unsigned long long)sz, nmade ? " and the blocks compiled before it" : "");
    (void)c;
    { extern void stop_shot(void); stop_shot(); }
    fflush(stderr);
    _Exit(22);
}

/* _FREE, wrapped: forget the lifted entries of a block that goes away. */
void hook_free(CPU *c)
{
    extern lifted_fn g_body__FREE;
    uint64_t p = rd64(c->rsp + 8);
    for (int i = nmade - 1; i >= 0; i--) if (made[i].a == p && made[i].live) { made[i].live = 0; break; }
    if (p >= act_lo && p < act_hi) {
        uint64_t sz = rd64(p - 8) - 16;
        for (size_t i = 0; i < (1u << OVB); i++)
            if (ov[i].a >= p && ov[i].a < p + sz) ov[i].fn = NULL;   /* keep the probe chain */
    }
    g_body__FREE(c);
}
