/* capture.c - code that exists only at run time, written down for lifting.
 *
 * When lifted code reaches an address that no lifted function covers, and the
 * address lies in a module TempleOS loaded itself (Load() of a .BIN, such as
 * Compiler.BIN), the module's bytes - as they are in memory, relocated and
 * linked - go to work/jit/ and the run stops. tools/gen.py lifts them, and
 * the next run checks the module is byte-for-byte the same before using the
 * lifted code (mods_verify). Nothing here runs the bytes.
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

typedef struct { uint64_t base, size, fnv; } Mod;
extern const Mod g_mods[]; extern const size_t g_nmods;
static unsigned char verified[4096];
static size_t nverified;
int mods_unverified(void) { return nverified < g_nmods; }

static uint64_t fnv_mem(uint64_t a, uint64_t n)
{
    uint64_t h = 1469598103934665603ull; const uint8_t *p = gptr(a);
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

/* A captured module containing a, or -1. The first time one is entered, its
 * memory must be exactly what was captured; anything else is new code. */
int mods_find(uint64_t a)
{
    for (size_t i = 0; i < g_nmods; i++)
        if (a >= g_mods[i].base && a < g_mods[i].base + g_mods[i].size) {
            if (!verified[i]) {
                if (fnv_mem(g_mods[i].base, g_mods[i].size) != g_mods[i].fnv) {
                    fprintf(stderr, "the module at %#llx is not the one that was captured; "
                            "delete work/jit and src/gen, and capture again\n", (unsigned long long)g_mods[i].base);
                    _Exit(23);
                }
                verified[i] = 1; nverified++;
            }
            return (int)i;
        }
    return -1;
}

/* A TempleOS module (a CBinFile, 'TOSB') loaded in memory around a. */
static int find_module(uint64_t a, uint64_t *base, uint64_t *size)
{
    for (uint64_t x = a & ~3ull; x + 32 > a - (4u << 20) && x >= 0x1000; x--) {
        if (memcmp(gptr(x + 4), "TOSB", 4)) continue;
        uint64_t sz; memcpy(&sz, gptr(x + 24), 8);
        uint64_t pt; memcpy(&pt, gptr(x + 16), 8);
        if (sz < 64 || sz > (64u << 20) || pt >= sz || a >= x + sz) continue;
        *base = x; *size = sz;
        return 1;
    }
    return 0;
}
int module_at(uint64_t a) { uint64_t b, s; return find_module(a, &b, &s); }

int capture_module(uint64_t a)
{
    for (uint64_t x = a & ~3ull; x + 32 > a - (4u << 20) && x >= 0x1000; x--) {
        if (memcmp(gptr(x + 4), "TOSB", 4)) continue;
        uint64_t size; memcpy(&size, gptr(x + 24), 8);
        uint64_t pt; memcpy(&pt, gptr(x + 16), 8);
        if (size < 64 || size > (64u << 20) || pt >= size || a >= x + size) continue;
        char dir[1024], p[1100];
        snprintf(dir, sizeof dir, "%s/jit", g_work);
        mkdir_(dir);
        snprintf(p, sizeof p, "%s/mod_%llX.bin", dir, (unsigned long long)x);
        FILE *f = fopen(p, "wb");
        if (!f) return 0;
        fwrite(gptr(x), 1, size, f); fclose(f);
        snprintf(p, sizeof p, "%s/index.txt", dir);
        f = fopen(p, "a");
        if (f) { fprintf(f, "mod 0x%llX 0x%llX\n", (unsigned long long)x, (unsigned long long)size); fclose(f); }
        fprintf(stderr, "captured the module at %#llx (%llu bytes) to %s/mod_%llX.bin; run tools/gen.py again\n",
                (unsigned long long)x, (unsigned long long)size, dir, (unsigned long long)x);
        return 1;
    }
    return 0;
}
