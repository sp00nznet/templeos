/* main.c - boot TempleOS from the user's ISO, as a host program.
 *
 * The firmware, the DVD boot loader and the kernel's 16- and 32-bit start-up
 * (KStart16.HC, KStart32.HC, Mem/PageTables.HC) are the part of a TempleOS
 * boot that talks to the BIOS; their effect on memory is reproduced here in C.
 * Everything from KStart64 on is the kernel's own code, lifted.
 */
#define SDL_MAIN_HANDLED
#include "temple.h"
#include "ksyms.h"
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

void devices_init(void);
uint64_t K_RET = K__RET;
const char *g_work = "work";

static uint8_t *read_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); if (fread(b, 1, *n, f) != *n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}
static void w16(uint64_t a, uint16_t v) { memcpy(gptr(a), &v, 2); }
static void w32(uint64_t a, uint32_t v) { memcpy(gptr(a), &v, 4); }
static void w64(uint64_t a, uint64_t v) { memcpy(gptr(a), &v, 8); }
static uint32_t r32(uint64_t a) { uint32_t v; memcpy(&v, gptr(a), 4); return v; }
static uint64_t r64(uint64_t a) { uint64_t v; memcpy(&v, gptr(a), 8); return v; }

static uint64_t fnv(const uint8_t *p, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    while (n--) { h ^= *p++; h *= 1099511628211ull; }
    return h;
}

/* BootDVD + KStart16 + KStart32: load the kernel, patch it, describe memory. */
static int boot(const uint8_t *iso, size_t iso_n)
{
    /* The El Torito image is BootDVD.HC; the boot installer patched in where the
     * kernel is (BDVD_BLK_LO/HI, _CNT, _SHIFT_BLKS) just after its messages. */
    const uint8_t *bl = iso + 21 * 2048;
    const char *msg = "64-bit capable processor.\n\r";
    const uint8_t *p = NULL;
    for (int i = 0; i < 2048 - 40; i++) if (!memcmp(bl + i, msg, strlen(msg) + 1)) { p = bl + i + strlen(msg) + 1; break; }
    if (!p) { fprintf(stderr, "not a TempleOS ISO (no DVD boot loader)\n"); return 0; }
    uint32_t blk = p[0] | p[1] << 8 | (uint32_t)(p[2] | p[3] << 8) << 16;
    uint32_t shift = p[6] | p[7] << 8;
    const uint8_t *k = iso + (uint64_t)blk * 2048 + shift * 512;
    if (memcmp(k + 4, "TOSB", 4)) { fprintf(stderr, "no kernel at block %u\n", blk); return 0; }
    uint64_t ksize; memcpy(&ksize, k + 24, 8);
    if (fnv(k, ksize) != K_IMAGE_FNV) {
        fprintf(stderr, "this ISO's kernel is not the one this build was lifted from (TempleOS V5.03 expected)\n");
        return 0;
    }

    /* BootDVD loads the file at BOOT_RAM_BASE; the module starts after the CBinFile header. */
    memcpy(gptr(0x7C00), k, ksize);
    uint64_t mb = 0x7C20;
    /* KStart32: patch the absolute addresses (the patch table's first entry). */
    uint64_t pt; memcpy(&pt, k + 16, 8);
    uint32_t n = r32(0x7C00 + pt + 1);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t at = mb + r32(0x7C00 + pt + 6 + 4ull * i);
        w32(at, r32(at) + (uint32_t)mb);
    }

    /* KStart16: what the BIOS told it. 1 GB of RAM. */
    w32(K_SYS_RUN_LEVEL, 1 | 2 | 4 | 8);            /* 16BIT, VGA, 32BIT, PATCHED */
    w32(K_SYS_BOOT_SRC, 4 | shift << 16);           /* BOOT_SRC_DVD, kernel's offset in its block */
    w32(K_SYS_BOOT_BLK, blk);
    w32(K_MEM_BOOT_BASE, (uint32_t)mb);
    w32(K_SYS_BOOT_PATCH_TABLE_BASE, (uint32_t)(mb + pt - 32));   /* LoadKernel reads the exports here */
    w16(K_MEM_E801, 15 * 1024);                      /* KB between 1 and 16 MB */
    w16(K_MEM_E801 + 2, (uint16_t)((GUEST_RAM - 0x1000000) >> 16));
    uint64_t e = K_MEM_E820;                         /* CMemE820: base, len, type */
    w64(e, 0); w64(e + 8, 0x9F000); w32(e + 16, 1); e += 24;
    w64(e, 0x100000); w64(e + 8, GUEST_RAM - 0x100000); w32(e + 16, 1); e += 24;
    w64(e, 0); w64(e + 8, 0); w32(e + 16, 0);
    w64(K_MEM_PHYSICAL_SPACE, GUEST_RAM);
    w16(K_SYS_PCI_BUSES, 256);
    w32(K_SYS_PCIBIOS_SERVICE_DIR, 0);
    w32(K_SYS_PCI_SERVICES, 0);

    /* SYS_INIT_PAGE_TABLES: 2 MB pages, identity, plus 4 GB for the uncached alias. */
    uint64_t mapped = GUEST_RAM + (4ull << 30);
    uint64_t n2 = (mapped + 0x1FFFFF) >> 21, n1 = (n2 + 0x1FF) >> 9, n512 = (n1 + 0x1FF) >> 9;
    w64(K_MEM_PAGE_SIZE, 0x200000);
    w64(K_MEM_MAPPED_SPACE, mapped);
    w64(K_MEM_2MEG_NUM, n2); w64(K_MEM_1GIG_NUM, n1); w64(K_MEM_512GIG_NUM, n512);
    memset(gptr(0x100000), 0, K_SIZEOF_SYS_FIXED_AREA);
    uint64_t pml2 = 0x100000 + K_SIZEOF_SYS_FIXED_AREA;
    uint64_t pml3 = pml2 + ((n2 + 0x1FF) & ~0x1FFull) * 8;
    uint64_t pml4 = pml3 + ((n1 + 0x1FF) & ~0x1FFull) * 8;
    uint64_t heap = pml4 + ((n512 + 0x1FF) & ~0x1FFull) * 8;
    w64(K_MEM_PML2, pml2); w64(K_MEM_PML3, pml3); w64(K_MEM_PML4, pml4); w64(K_MEM_HEAP_BASE, heap);
    memset(gptr(pml2), 0, heap - pml2);
    for (uint64_t i = 0; i < n2; i++) w64(pml2 + 8 * i, 0x87 + (i << 21));
    for (uint64_t i = 0; i < n1; i++) w64(pml3 + 8 * i, (pml2 | 7) + (i << 12));
    for (uint64_t i = 0; i < n512; i++) w64(pml4 + 8 * i, (pml3 | 7) + (i << 12));

    /* SYS_INIT_16MEG_SYS_CODE_BP: the first block pool, up to 16 MB. */
    uint64_t bp = K_SYS_FIXED_AREA_SYS_CODE_BP;
    w64(K_SYS_CODE_BP, bp); w64(K_SYS_DATA_BP, 0);
    uint64_t sz = (0x100000 + (15ull * 1024 << 10)) - heap;
    uint64_t pags = sz >> 9;
    w64(heap, r64(bp + 0x18));                        /* CMemBlk.next = free list */
    w64(bp + 0x18, heap);                             /* CBlkPool.mem_free_lst */
    w32(heap + 0x10, 0x6E55424D);                     /* MBS_UNUSED_SIGNATURE_VAL */
    w32(heap + 0x14, (uint32_t)pags);
    w32(bp + 8, r32(bp + 8) + (uint32_t)(pags << 9));
    w32(K_SYS_RUN_LEVEL, r32(K_SYS_RUN_LEVEL) | 16); /* RLf_16MEG_SYS_CODE_BP */

    /* SYS_ENTER_LONG_MODE returns into KStart64 with the temporary stack. */
    g_cpu.rsp = 0x97000;
    return 1;
}

int main(int argc, char **argv)
{
    const char *iso_path = NULL, *shot = NULL;
    int hidden = 0; double shot_at = 10, quit_at = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hidden")) hidden = 1;
        else if (!strcmp(argv[i], "--trace")) g_trace++;
        else if (!strcmp(argv[i], "--shot") && i + 2 < argc) { shot = argv[++i]; shot_at = atof(argv[++i]); }
        else if (!strcmp(argv[i], "--quit") && i + 1 < argc) quit_at = atof(argv[++i]);
        else if (!strcmp(argv[i], "--work") && i + 1 < argc) g_work = argv[++i];
        else iso_path = argv[i];
    }
    if (!iso_path) { fprintf(stderr, "usage: templeos [--hidden] [--shot out.png SECONDS] [--quit SECONDS] TempleOS.ISO\n"); return 2; }
    size_t iso_n; uint8_t *iso = read_file(iso_path, &iso_n);
    if (!iso || !iso_open(iso_path)) { fprintf(stderr, "cannot read %s\n", iso_path); return 2; }

    { extern uint64_t g_trace_fn; const char *t = getenv("TEMPLE_TRACE_FN"); if (t) g_trace_fn = strtoull(t, NULL, 16); }
    SDL_SetMainReady();
    mem_init();
    machine_init();
    if (!boot(iso, iso_n)) return 3;
    if (!present_init(hidden)) return 4;
    devices_init();
    tasks_start(&g_cpu, K_KSTART64);

    uint64_t t0 = host_ns(), last_frame = 0;
    int shot_done = 0;
    while (!g_quit) {
        uint64_t now = host_ns();
        double secs = (now - t0) / 1e9;
        g_irq_pending = 1;                            /* the timer: guest polls take it from here */
        if (now - last_frame > 16000000ull) { present_poll(); present_frame(); last_frame = now; }
        if (shot && !shot_done && secs >= shot_at) { present_png(shot); shot_done = 1; fprintf(stderr, "[shot] %s at %.1fs\n", shot, secs); }
        if (quit_at && secs >= quit_at) break;
        SDL_Delay(1);
    }
    fflush(stderr);
    _Exit(0);
}
