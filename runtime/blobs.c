/* blobs.c - the HolyC compiler's output, matched to its lifted form.
 *
 * Every function, statement and asm block TempleOS compiles at run time lands
 * in a heap block (MAlloc on the code heap). When lifted code first reaches an
 * address in such a block, the block is found from TempleOS's own heap header
 * (CMemUsed: the heap control it came from, then its size), hashed, and looked
 * up by (address, hash) among the blocks lifted so far. A match activates the
 * block's lifted entry points; no match captures the block to work/jit/ and
 * stops the run (tools/gen.py lifts it). When the guest frees a block, its
 * entries are dropped, so different code later at the same address can never
 * reach stale lifted code.
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

typedef struct { uint64_t a; lifted_fn fn; } BlobEnt;
typedef struct { uint64_t addr, size, fnv; const BlobEnt *ents; size_t nents; } Blob;
extern const Blob g_blobs[]; extern const size_t g_nblobs;

#define OVB 16
static struct { uint64_t a; lifted_fn fn; } ov[1 << OVB];
static uint64_t act_lo = ~0ull, act_hi;

static uint64_t fnv_mem(uint64_t a, uint64_t n)
{
    uint64_t h = 1469598103934665603ull; const uint8_t *p = gptr(a);
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}
static size_t slot(uint64_t a) { return (size_t)((a * 0x9E3779B97F4A7C15ull) >> (64 - OVB)); }
static void ov_put(uint64_t a, lifted_fn fn)
{
    for (size_t i = slot(a);; i = (i + 1) & ((1 << OVB) - 1))
        if (!ov[i].a || ov[i].a == a) { ov[i].a = a; ov[i].fn = fn; return; }
}
lifted_fn blob_lookup(uint64_t a)
{
    if (a < act_lo || a >= act_hi) return NULL;
    for (size_t i = slot(a);; i = (i + 1) & ((1 << OVB) - 1)) {
        if (!ov[i].a) return NULL;
        if (ov[i].a == a) return ov[i].fn;
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

/* Called on a dispatch miss outside the kernel and the captured modules. */
lifted_fn blob_resolve(CPU *c, uint64_t a)
{
    uint64_t st, sz;
    if (!heap_block(a, &st, &sz)) return NULL;
    uint64_t h = fnv_mem(st, sz);
    for (size_t i = 0; i < g_nblobs; i++)
        if (g_blobs[i].addr == st && g_blobs[i].fnv == h) {
            for (size_t k = 0; k < g_blobs[i].nents; k++) ov_put(g_blobs[i].ents[k].a, g_blobs[i].ents[k].fn);
            if (st < act_lo) act_lo = st;
            if (st + sz > act_hi) act_hi = st + sz;
            lifted_fn f = blob_lookup(a);
            if (f) return f;
            return NULL;                                   /* a new entry into a known block */
        }
    /* New compiler output: write it down for lifting. */
    char dir[1024], p[1200];
    snprintf(dir, sizeof dir, "%s/jit", g_work); mkdir_(dir);
    snprintf(p, sizeof p, "%s/blob_%llX_%016llX.bin", dir, (unsigned long long)st, (unsigned long long)h);
    FILE *f = fopen(p, "wb");
    if (f) { fwrite(gptr(st), 1, sz, f); fclose(f); }
    snprintf(p, sizeof p, "%s/index.txt", dir);
    f = fopen(p, "a");
    if (f) { fprintf(f, "blob 0x%llX 0x%llX 0x%016llX\n", (unsigned long long)st, (unsigned long long)sz, (unsigned long long)h); fclose(f); }
    fprintf(stderr, "captured compiler output at %#llx (%llu bytes); run tools/gen.py again\n",
            (unsigned long long)st, (unsigned long long)sz);
    (void)c;
    fflush(stderr);
    _Exit(22);
}

/* _FREE, wrapped: forget the lifted entries of a block that goes away. */
void hook_free(CPU *c)
{
    extern lifted_fn g_body__FREE;
    uint64_t p = rd64(c->rsp + 8);
    if (p >= act_lo && p < act_hi) {
        uint64_t sz = rd64(p - 8) - 16;
        for (size_t i = 0; i < (1u << OVB); i++)
            if (ov[i].a >= p && ov[i].a < p + sz) ov[i].fn = NULL;   /* keep the probe chain */
    }
    g_body__FREE(c);
}
