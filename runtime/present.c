/* present.c - the 640x480 16-colour VGA screen in an SDL2 window.
 *
 * TempleOS writes VGA memory at 0xA0000 one plane at a time, choosing the
 * plane with the sequencer's map mask (Adam's GrUpdateVGAGraphics, or the
 * kernel's raw text with all four planes at once). Guest memory has one copy
 * of that window, so each time the map mask changes the bytes written under
 * the old mask are latched into those planes here.
 */
#include "temple.h"
#include <SDL.h>
#include <string.h>
#include <stdlib.h>

#define W 640
#define H 480
#define PLANE (W * H / 8)
static uint8_t planes[4][PLANE];
static int cur_mask = 0xF;
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static uint32_t rgba[W * H];
int g_quit;

void vga_planes_latch(int new_mask)
{
    const uint8_t *v = gptr(0xA0000);
    for (int p = 0; p < 4; p++)
        if (cur_mask & (1 << p)) memcpy(planes[p], v, PLANE);
    cur_mask = new_mask;
}

static void compose(void)
{
    /* Planes under the current mask are still being written: read them live. */
    const uint8_t *v = gptr(0xA0000), *pl[4];
    int m = cur_mask;
    for (int p = 0; p < 4; p++) pl[p] = (m & (1 << p)) ? v : planes[p];
    for (int i = 0; i < PLANE; i++) {
        uint8_t b0 = pl[0][i], b1 = pl[1][i], b2 = pl[2][i], b3 = pl[3][i];
        for (int k = 0; k < 8; k++) {
            int s = 7 - k;
            int ci = ((b0 >> s) & 1) | (((b1 >> s) & 1) << 1) | (((b2 >> s) & 1) << 2) | (((b3 >> s) & 1) << 3);
            const uint8_t *c = g_vga_pal[ci];
            rgba[i * 8 + k] = 0xFF000000u | (uint32_t)c[0] << 16 | (uint32_t)c[1] << 8 | c[2];
        }
    }
}

int present_init(int hidden)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 0;
    }
    win = SDL_CreateWindow("TempleOS", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, W * 2, H * 2,
                           SDL_WINDOW_RESIZABLE | (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); return 0; }
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
    if (ren) {
        SDL_RenderSetLogicalSize(ren, W, H);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, W, H);
    }
    return 1;
}

void present_frame(void)
{
    compose();
    if (!tex) return;
    SDL_UpdateTexture(tex, NULL, rgba, W * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

/* ---- PNG, written by hand: stored deflate blocks, so no zlib is needed ---- */
static uint32_t crc_tab[256];
static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    if (!crc_tab[1]) for (uint32_t i = 0; i < 256; i++) { uint32_t r = i; for (int k = 0; k < 8; k++) r = r & 1 ? 0xEDB88320u ^ (r >> 1) : r >> 1; crc_tab[i] = r; }
    c = ~c; while (n--) c = crc_tab[(c ^ *p++) & 0xFF] ^ (c >> 8); return ~c;
}
static void be(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n)
{
    uint8_t h[8]; be(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); fwrite(d, 1, n, f);
    uint32_t c = crc(crc(0, (const uint8_t *)t, 4), d, n); be(h, c); fwrite(h, 1, 4, f);
}
int present_png(const char *path)
{
    compose();
    size_t row = 1 + W * 3, raw_n = row * H;
    uint8_t *raw = malloc(raw_n);
    for (int y = 0; y < H; y++) {
        raw[y * row] = 0;
        for (int x = 0; x < W; x++) {
            uint32_t p = rgba[y * W + x]; uint8_t *o = raw + y * row + 1 + x * 3;
            o[0] = p >> 16; o[1] = p >> 8; o[2] = p;
        }
    }
    size_t nblk = (raw_n + 65534) / 65535, zn = 2 + raw_n + nblk * 5 + 4;
    uint8_t *z = malloc(zn), *q = z;
    *q++ = 0x78; *q++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < raw_n; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw_n; off += 65535) {
        size_t n = raw_n - off < 65535 ? raw_n - off : 65535;
        *q++ = off + n == raw_n; *q++ = n & 0xFF; *q++ = n >> 8; *q++ = ~n & 0xFF; *q++ = (~n >> 8) & 0xFF;
        memcpy(q, raw + off, n); q += n;
    }
    be(q, (b << 16) | a); q += 4;
    FILE *f = fopen(path, "wb");
    if (!f) { free(raw); free(z); return 0; }
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13]; be(ih, W); be(ih + 4, H); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    chunk(f, "IHDR", ih, 13);
    chunk(f, "IDAT", z, (uint32_t)(q - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw); free(z);
    return 1;
}

void present_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_QUIT) g_quit = 1;
}
