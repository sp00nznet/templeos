/* devices.c - the PC around TempleOS, modelled in C.
 *
 * Only what TempleOS's own drivers touch: the PIT and PIC (the 1 kHz jiffy
 * interrupt), the i8042 keyboard/mouse controller, the CMOS clock, VGA's
 * sequencer and DAC, PCI configuration space (empty), an ATAPI CD-ROM on the
 * primary IDE channel serving the user's ISO read-only, the HPET and a quiet
 * local APIC. Each is a register file; none runs guest code.
 */
#include "temple.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

/* ---------------- PIC ---------------- */
static struct { uint8_t irr, isr, imr, vec, icw; } pic[2] = {{0, 0, 0xFF, 0x08, 0}, {0, 0, 0xFF, 0x70, 0}};

void irq_raise(int irq) { pic[irq >> 3].irr |= 1 << (irq & 7); if (irq >= 8) pic[0].irr |= 4; }

static int pic_pick(int deliver)
{
    for (int i = 0; i < 8; i++) {
        uint8_t m = 1 << i;
        if (pic[0].isr & m) return -1;        /* something of higher priority is in service */
        if ((pic[0].irr & m) && !(pic[0].imr & m)) {
            if (i == 2) {
                for (int j = 0; j < 8; j++) {
                    uint8_t n = 1 << j;
                    if (pic[1].isr & n) return -1;
                    if ((pic[1].irr & n) && !(pic[1].imr & n)) {
                        if (deliver) {
                            pic[1].irr &= ~n; pic[1].isr |= n; pic[0].isr |= m;
                            if (!pic[1].irr) pic[0].irr &= ~m;
                        }
                        return pic[1].vec + j;
                    }
                }
                continue;
            }
            if (deliver) { pic[0].irr &= ~m; pic[0].isr |= m; }
            return pic[0].vec + i;
        }
    }
    return -1;
}
int pic_next(void) { return pic_pick(1); }
int pic_any(void) { return pic_pick(0) >= 0; }

static void pic_out(int n, int port, uint8_t v)
{
    if (port == 0) {
        if (v & 0x10) { pic[n].icw = 1; pic[n].imr = 0; pic[n].isr = 0; pic[n].irr = 0; return; }
        if ((v & 0x18) == 0 && (v & 0x20)) {          /* EOI: clear the highest in service */
            for (int i = 0; i < 8; i++) if (pic[n].isr & (1 << i)) { pic[n].isr &= ~(1 << i); break; }
        }
        return;
    }
    if (pic[n].icw == 1) { pic[n].vec = v & 0xF8; pic[n].icw = 2; return; }
    if (pic[n].icw == 2) { pic[n].icw = 3; return; }
    if (pic[n].icw == 3) { pic[n].icw = 0; return; }
    pic[n].imr = v;
}

/* ---------------- PIT ---------------- */
static uint32_t pit_reload = 65536, pit_latch_lo, pit_access;
static uint64_t pit_t0, pit_ticks;
static uint8_t port61;

static void pit_tick(void)
{
    uint64_t now = host_ns();
    if (!pit_t0) pit_t0 = now;
    double hz = 1193182.0 / (pit_reload ? pit_reload : 65536);
    uint64_t due = (uint64_t)((double)(now - pit_t0) * hz / 1e9);
    if (due > pit_ticks) { pit_ticks = due; irq_raise(0); }
}

/* ---------------- i8042 ---------------- */
#define KQ 256
static uint8_t kq[KQ], kqa[KQ]; static int kqh, kqt;
static uint8_t kbc_cmd = 0x47, kbc_next, mouse_next, mouse_rates[3], mouse_id;
static SDL_mutex *in_lock;
static uint8_t hostq[1024], hostqa[1024]; static int hqh, hqt;

static void kpush(uint8_t b, int aux) { kq[kqt] = b; kqa[kqt] = aux; kqt = (kqt + 1) % KQ; }
static void mouse_cmd(uint8_t v)
{
    if (mouse_next) {
        if (mouse_next == 0xF3) { mouse_rates[0] = mouse_rates[1]; mouse_rates[1] = mouse_rates[2]; mouse_rates[2] = v;
            if (mouse_rates[0] == 200 && mouse_rates[1] == 100 && mouse_rates[2] == 80) mouse_id = 3; }
        mouse_next = 0; kpush(0xFA, 1); return;
    }
    switch (v) {
    case 0xFF: kpush(0xFA, 1); kpush(0xAA, 1); kpush(0x00, 1); mouse_id = 0; break;
    case 0xF2: kpush(0xFA, 1); kpush(mouse_id, 1); break;
    case 0xF3: case 0xE8: mouse_next = v; kpush(0xFA, 1); break;
    case 0xE9: kpush(0xFA, 1); kpush(0, 1); kpush(2, 1); kpush(100, 1); break;
    default: kpush(0xFA, 1);
    }
}
static void kbd_cmd(uint8_t v)
{
    if (kbc_next == 0x60) { kbc_cmd = v; kbc_next = 0; return; }
    if (kbc_next == 0xD4) { kbc_next = 0; mouse_cmd(v); return; }
    if (kbc_next == 0xD3) { kbc_next = 0; kpush(v, 1); return; }
    if (kbc_next == 0xD2) { kbc_next = 0; kpush(v, 0); return; }
    if (kbc_next == 1) { kbc_next = 0; kpush(0xFA, 0); return; }   /* argument of ED/F3/F0 */
    switch (v) {
    case 0xFF: kpush(0xFA, 0); kpush(0xAA, 0); break;
    case 0xF2: kpush(0xFA, 0); kpush(0xAB, 0); kpush(0x83, 0); break;
    case 0xED: case 0xF3: case 0xF0: kbc_next = 1; kpush(0xFA, 0); break;
    case 0xEE: kpush(0xEE, 0); break;
    default: kpush(0xFA, 0);
    }
}
static void kbc_ctl(uint8_t v)
{
    switch (v) {
    case 0x20: kpush(kbc_cmd, 0); break;
    case 0x60: case 0xD4: case 0xD3: case 0xD2: kbc_next = v; break;
    case 0xA9: case 0xAB: kpush(0x00, 0); break;
    case 0xAA: kpush(0x55, 0); break;
    case 0xA7: kbc_cmd |= 0x20; break;
    case 0xA8: kbc_cmd &= ~0x20; break;
    case 0xAD: kbc_cmd |= 0x10; break;
    case 0xAE: kbc_cmd &= ~0x10; break;
    }
}
static void kbc_irq(void)
{
    if (kqh == kqt) return;
    if (kqa[kqh]) { if (kbc_cmd & 2) irq_raise(12); }
    else if (kbc_cmd & 1) irq_raise(1);
}
/* Host side (main thread): queue input bytes for the guest. */
void input_bytes(const uint8_t *b, int n, int aux)
{
    SDL_LockMutex(in_lock);
    for (int i = 0; i < n; i++) { hostq[hqt] = b[i]; hostqa[hqt] = aux; hqt = (hqt + 1) % 1024; }
    SDL_UnlockMutex(in_lock);
}
static void input_drain(void)
{
    if (kqh != kqt) return;                    /* one device byte at a time, like the wire */
    SDL_LockMutex(in_lock);
    if (hqh != hqt) { kpush(hostq[hqh], hostqa[hqh]); hqh = (hqh + 1) % 1024; }
    SDL_UnlockMutex(in_lock);
}

/* ---------------- CMOS ---------------- */
static uint8_t cmos_idx;
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static uint8_t cmos_read(void)
{
    time_t t = time(NULL); struct tm *tm = localtime(&t);
    switch (cmos_idx & 0x7F) {
    case 0x00: return bcd(tm->tm_sec);
    case 0x02: return bcd(tm->tm_min);
    case 0x04: return bcd(tm->tm_hour);
    case 0x06: return bcd(tm->tm_wday + 1);
    case 0x07: return bcd(tm->tm_mday);
    case 0x08: return bcd(tm->tm_mon + 1);
    case 0x09: return bcd(tm->tm_year % 100);
    case 0x32: return bcd(19 + tm->tm_year / 100);
    case 0x0A: return 0x26;
    case 0x0B: return 0x02;
    case 0x0D: return 0x80;
    }
    return 0;
}

/* ---------------- VGA ---------------- */
uint8_t g_vga_pal[16][3];
static uint8_t seq_idx, gc_idx, dac_w, dac_c, map_mask = 0xF, retrace;
static void vga_out(uint32_t port, uint8_t v)
{
    switch (port) {
    case 0x3C4: seq_idx = v; break;
    case 0x3C5: if (seq_idx == 2) { vga_planes_latch(v & 0xF); map_mask = v & 0xF; } break;
    case 0x3CE: gc_idx = v; break;
    case 0x3C8: dac_w = v; dac_c = 0; break;
    case 0x3C9: if (dac_w < 16) g_vga_pal[dac_w][dac_c] = (uint8_t)((v & 0x3F) * 255 / 63);
                if (++dac_c == 3) { dac_c = 0; dac_w++; } break;
    }
}

/* ---------------- ATAPI CD-ROM (primary master) ---------------- */
static uint8_t *iso; static uint64_t iso_size;
static struct {
    uint8_t feat, nsect, sect, lcyl, hcyl, sel, stat, err;
    uint8_t pkt[12]; int npkt, in_pkt;
    uint8_t *data; uint32_t dlen, dpos, limit;
} ata = { .stat = 0x40 };

int iso_open(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); iso_size = (uint64_t)ftell(f); fseek(f, 0, SEEK_SET);
    iso = malloc(iso_size);
    if (fread(iso, 1, iso_size, f) != iso_size) { fclose(f); return 0; }
    fclose(f);
    return 1;
}
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void ata_data_phase(uint8_t *buf, uint32_t n)
{
    free(ata.data); ata.data = buf; ata.dlen = n; ata.dpos = 0;
    uint32_t chunk = n < ata.limit || !ata.limit ? n : ata.limit;
    ata.lcyl = (uint8_t)chunk; ata.hcyl = (uint8_t)(chunk >> 8);
    ata.nsect = 2;                                  /* I/O = device to host */
    ata.stat = n ? 0x48 : 0x40;
}
static void atapi_exec(void)
{
    uint8_t *p = ata.pkt;
    ata.err = 0;
    switch (p[0]) {
    case 0x00: case 0x1B: case 0x2B: case 0xBB: case 0x1E:
        ata_data_phase(NULL, 0); return;
    case 0x25: {
        uint8_t *b = calloc(1, 8); uint32_t last = (uint32_t)(iso_size / 2048) - 1;
        b[0] = last >> 24; b[1] = last >> 16; b[2] = last >> 8; b[3] = last; b[6] = 0x08;
        ata_data_phase(b, 8); return;
    }
    case 0x28: case 0xA8: {
        uint32_t lba = be32(p + 2), n = p[0] == 0x28 ? (uint32_t)(p[7] << 8 | p[8]) : be32(p + 6);
        uint8_t *b = calloc(n ? n : 1, 2048);
        for (uint32_t i = 0; i < n; i++)
            if ((uint64_t)(lba + i) * 2048 + 2048 <= iso_size) memcpy(b + 2048ull * i, iso + (uint64_t)(lba + i) * 2048, 2048);
        if (g_trace) fprintf(stderr, "[atapi] read lba %u x%u\n", lba, n);
        ata_data_phase(b, n * 2048); return;
    }
    case 0x52: case 0x43: case 0x51: case 0x46: case 0x5A: case 0x12: case 0x03: {
        uint32_t n = (uint32_t)(p[7] << 8 | p[8]); if (p[0] == 0x12 || p[0] == 0x03) n = p[4];
        ata_data_phase(calloc(1, n ? n : 1), n); return;
    }
    }
    if (g_trace) fprintf(stderr, "[atapi] packet %#x unsupported\n", p[0]);
    ata.err = 0x24; ata.stat = 0x41;
}
static int ata_here(void) { return ((ata.sel >> 4) & 1) == 0 && iso; }
static uint64_t ata_in(uint32_t r, int size)
{
    if (!ata_here() && r != 6) return r == 7 ? 0 : 0;
    switch (r) {
    case 0: {
        uint64_t v = 0;
        for (int i = 0; i < size && ata.dpos < ata.dlen; i++) v |= (uint64_t)ata.data[ata.dpos++] << (8 * i);
        if (ata.dpos >= ata.dlen) ata.stat = 0x40;
        else if (ata.limit && ata.dpos % ata.limit == 0) {
            uint32_t rest = ata.dlen - ata.dpos, chunk = rest < ata.limit ? rest : ata.limit;
            ata.lcyl = (uint8_t)chunk; ata.hcyl = (uint8_t)(chunk >> 8);
        }
        return v;
    }
    case 1: return ata.err;
    case 2: return ata.nsect;
    case 3: return ata.sect;
    case 4: return ata.lcyl;
    case 5: return ata.hcyl;
    case 6: return ata.sel;
    case 7: return ata.stat;
    }
    return 0;
}
static void ata_out(uint32_t r, uint64_t v, int size)
{
    if (r == 6) { ata.sel = (uint8_t)v; return; }
    if (!ata_here()) return;
    switch (r) {
    case 0:
        if (ata.in_pkt) {
            for (int i = 0; i < size && ata.npkt < 12; i++) ata.pkt[ata.npkt++] = (uint8_t)(v >> (8 * i));
            if (ata.npkt == 12) { ata.in_pkt = 0; atapi_exec(); }
        }
        return;
    case 1: ata.feat = (uint8_t)v; return;
    case 2: ata.nsect = (uint8_t)v; return;
    case 3: ata.sect = (uint8_t)v; return;
    case 4: ata.lcyl = (uint8_t)v; return;
    case 5: ata.hcyl = (uint8_t)v; return;
    case 7:
        switch ((uint8_t)v) {
        case 0xA0:                              /* PACKET */
            ata.limit = (uint32_t)(ata.hcyl << 8 | ata.lcyl); if (!ata.limit) ata.limit = 0xFFFE;
            ata.npkt = 0; ata.in_pkt = 1; ata.stat = 0x48; ata.nsect = 1; return;
        case 0xEC:                              /* IDENTIFY: aborted, with the ATAPI signature */
            ata.err = 0x04; ata.stat = 0x41; ata.lcyl = 0x14; ata.hcyl = 0xEB; return;
        case 0xA1: {                            /* IDENTIFY PACKET DEVICE */
            uint8_t *b = calloc(1, 512); b[0] = 0x80; b[1] = 0x85;
            ata.limit = 512; ata_data_phase(b, 512); return;
        }
        default: ata.err = 0; ata.stat = 0x40; return;
        }
    }
}

/* ---------------- MMIO: HPET, local APIC ---------------- */
static uint8_t hpet[1024], lapic[4096], ioapic[256], nothing[4096];
static uint64_t hpet_t0;
uintptr_t dev_mmio(uint64_t p, int size, int write)
{
    (void)size;
    if (p >= 0xFED00000 && p < 0xFED00400) {
        uint64_t cap = (100000000ull << 32) | 0x8086A201ull;  /* 10 MHz, 64-bit counter */
        memcpy(hpet, &cap, 8);
        uint64_t cfg; memcpy(&cfg, hpet + 0x10, 8);
        if (!write && (cfg & 1)) {
            if (!hpet_t0) hpet_t0 = host_ns();
            uint64_t n = (host_ns() - hpet_t0) / 100; memcpy(hpet + 0xF0, &n, 8);
        }
        return (uintptr_t)(hpet + (p - 0xFED00000));
    }
    if (p >= 0xFEE00000 && p < 0xFEE01000) {
        if (!write) { uint32_t ver = 0x50014; memcpy(lapic + 0x30, &ver, 4); }
        return (uintptr_t)(lapic + (p - 0xFEE00000));
    }
    if (p >= 0xFEC00000 && p < 0xFEC00100) return (uintptr_t)(ioapic + (p - 0xFEC00000));
    memset(nothing, write ? 0 : 0xFF, sizeof nothing);
    return (uintptr_t)(nothing + (p & 0xFFF));
}

/* ---------------- ports ---------------- */
static uint32_t pci_addr;
uint64_t dev_in(uint32_t port, int size)
{
    if (port >= 0x1F0 && port <= 0x1F7) return ata_in(port - 0x1F0, size);
    if (port == 0x3F6) return ata_here() ? ata.stat : 0;
    switch (port) {
    case 0x20: case 0x21: return port & 1 ? pic[0].imr : pic[0].irr;
    case 0xA0: case 0xA1: return port & 1 ? pic[1].imr : pic[1].irr;
    case 0x40: { uint64_t t = host_ns() * 1193182ull / 1000000000ull;
                 uint32_t c = pit_reload - (uint32_t)(t % pit_reload);
                 if (pit_access++ & 1) return (c >> 8) & 0xFF; pit_latch_lo = c & 0xFF; return pit_latch_lo; }
    case 0x60: { input_drain(); uint8_t b = 0; if (kqh != kqt) { b = kq[kqh]; kqh = (kqh + 1) % KQ; } kbc_irq(); return b; }
    case 0x64: input_drain(); return 0x14 | (kqh != kqt ? 1 : 0) | (kqh != kqt && kqa[kqh] ? 0x20 : 0);
    case 0x61: port61 ^= 0x30; return port61;
    case 0x71: return cmos_read();
    case 0x3DA: retrace ^= 9; return retrace;
    case 0x3C5: return seq_idx == 2 ? map_mask : 0;
    case 0xCF8: return pci_addr;
    case 0xCFC: case 0xCFD: case 0xCFE: case 0xCFF: return size == 4 ? 0xFFFFFFFFu : size == 2 ? 0xFFFF : 0xFF;
    }
    if (g_trace > 1) fprintf(stderr, "[port] in %#x\n", port);
    return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
}

void dev_out(uint32_t port, uint64_t v, int size)
{
    if (port >= 0x1F0 && port <= 0x1F7) { ata_out(port - 0x1F0, v, size); return; }
    switch (port) {
    case 0x20: case 0x21: pic_out(0, port & 1, (uint8_t)v); return;
    case 0xA0: case 0xA1: pic_out(1, port & 1, (uint8_t)v); return;
    case 0x43: pit_access = 0; return;
    case 0x40: if (pit_access++ & 1) { pit_reload = (pit_reload & 0xFF) | ((uint32_t)(v & 0xFF) << 8); if (!pit_reload) pit_reload = 65536; pit_t0 = 0; pit_ticks = 0; }
               else pit_reload = (uint32_t)(v & 0xFF); return;
    case 0x60: kbd_cmd((uint8_t)v); kbc_irq(); return;
    case 0x64: kbc_ctl((uint8_t)v); kbc_irq(); return;
    case 0x61: port61 = (uint8_t)v; return;                /* PC speaker gate: muted */
    case 0x70: cmos_idx = (uint8_t)v; return;
    case 0xCF8: pci_addr = (uint32_t)v; return;
    case 0x3C4: case 0x3C5: case 0x3CE: case 0x3CF: case 0x3C8: case 0x3C9:
        vga_out(port, (uint8_t)v);
        if (size == 2) vga_out(port + 1, (uint8_t)(v >> 8));
        return;
    }
    if (g_trace > 1) fprintf(stderr, "[port] out %#x = %#llx\n", port, (unsigned long long)v);
}

/* Called by the guest context at interrupt safe points and in HLT. */
void dev_tick(void)
{
    if (!in_lock) in_lock = SDL_CreateMutex();
    pit_tick();
    input_drain();
    kbc_irq();
}

void devices_init(void)
{
    in_lock = SDL_CreateMutex();
    static const uint8_t ega[16][3] = {
        {0,0,0},{0,0,170},{0,170,0},{0,170,170},{170,0,0},{170,0,170},{170,85,0},{170,170,170},
        {85,85,85},{85,85,255},{85,255,85},{85,255,255},{255,85,85},{255,85,255},{255,255,85},{255,255,255}};
    memcpy(g_vga_pal, ega, sizeof ega);
}
