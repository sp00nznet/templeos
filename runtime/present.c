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
    /* GrUpdateLine64 writes only the qwords that changed, so the bytes it
     * skips must still hold the newly selected plane: swap that plane in. */
    for (int p = 0; p < 4; p++)
        if (new_mask & (1 << p)) { memcpy((uint8_t *)v, planes[p], PLANE); break; }
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

/* --sound: the PC speaker as a square wave. Off by default; tests stay muted. */
extern volatile uint32_t g_speaker_hz;
static void audio_cb(void *u, Uint8 *out, int n)
{
    static double ph;
    int16_t *o = (int16_t *)out;
    uint32_t hz = g_speaker_hz;
    (void)u;
    for (int i = 0; i < n / 2; i++) {
        o[i] = (hz && hz < 20000) ? (ph < 0.5 ? 3000 : -3000) : 0;
        ph += hz / 44100.0;
        ph -= (int)ph;
    }
}

void sound_init(void)
{
    SDL_AudioSpec want = {0};
    want.freq = 44100; want.format = AUDIO_S16SYS; want.channels = 1; want.samples = 512;
    want.callback = audio_cb;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { fprintf(stderr, "audio: %s\n", SDL_GetError()); return; }
    SDL_AudioDeviceID d = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (d) SDL_PauseAudioDevice(d, 0);
    else fprintf(stderr, "audio: %s\n", SDL_GetError());
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

/* SDL scancode -> PC scancode set 1 (what the i8042 delivers with
 * translation on). 0xE0xx are the extended keys. */
static uint16_t set1(SDL_Scancode s)
{
    static const uint8_t az[26] = {0x1E,0x30,0x2E,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,
                                   0x31,0x18,0x19,0x10,0x13,0x1F,0x14,0x16,0x2F,0x11,0x2D,0x15,0x2C};
    if (s >= SDL_SCANCODE_A && s <= SDL_SCANCODE_Z) return az[s - SDL_SCANCODE_A];
    if (s >= SDL_SCANCODE_1 && s <= SDL_SCANCODE_0) return (uint16_t)(0x02 + (s - SDL_SCANCODE_1));
    if (s >= SDL_SCANCODE_F1 && s <= SDL_SCANCODE_F10) return (uint16_t)(0x3B + (s - SDL_SCANCODE_F1));
    switch (s) {
    case SDL_SCANCODE_RETURN: return 0x1C;      case SDL_SCANCODE_ESCAPE: return 0x01;
    case SDL_SCANCODE_BACKSPACE: return 0x0E;   case SDL_SCANCODE_TAB: return 0x0F;
    case SDL_SCANCODE_SPACE: return 0x39;       case SDL_SCANCODE_MINUS: return 0x0C;
    case SDL_SCANCODE_EQUALS: return 0x0D;      case SDL_SCANCODE_LEFTBRACKET: return 0x1A;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1B; case SDL_SCANCODE_BACKSLASH: return 0x2B;
    case SDL_SCANCODE_SEMICOLON: return 0x27;   case SDL_SCANCODE_APOSTROPHE: return 0x28;
    case SDL_SCANCODE_GRAVE: return 0x29;       case SDL_SCANCODE_COMMA: return 0x33;
    case SDL_SCANCODE_PERIOD: return 0x34;      case SDL_SCANCODE_SLASH: return 0x35;
    case SDL_SCANCODE_CAPSLOCK: return 0x3A;    case SDL_SCANCODE_F11: return 0x57;
    case SDL_SCANCODE_F12: return 0x58;         case SDL_SCANCODE_LSHIFT: return 0x2A;
    case SDL_SCANCODE_RSHIFT: return 0x36;      case SDL_SCANCODE_LCTRL: return 0x1D;
    case SDL_SCANCODE_LALT: return 0x38;        case SDL_SCANCODE_RCTRL: return 0xE01D;
    case SDL_SCANCODE_RALT: return 0xE038;      case SDL_SCANCODE_UP: return 0xE048;
    case SDL_SCANCODE_DOWN: return 0xE050;      case SDL_SCANCODE_LEFT: return 0xE04B;
    case SDL_SCANCODE_RIGHT: return 0xE04D;     case SDL_SCANCODE_HOME: return 0xE047;
    case SDL_SCANCODE_END: return 0xE04F;       case SDL_SCANCODE_PAGEUP: return 0xE049;
    case SDL_SCANCODE_PAGEDOWN: return 0xE051;  case SDL_SCANCODE_INSERT: return 0xE052;
    case SDL_SCANCODE_DELETE: return 0xE053;    case SDL_SCANCODE_LGUI: return 0xE05B;
    default: return 0;
    }
}

void input_bytes(const uint8_t *b, int n, int aux);   /* devices.c */

static void key(SDL_Scancode s, int up)
{
    uint16_t k = set1(s);
    if (!k) return;
    uint8_t b[2]; int n = 0;
    if (k >> 8) b[n++] = 0xE0;
    b[n++] = (uint8_t)((k & 0x7F) | (up ? 0x80 : 0));
    input_bytes(b, n, 0);
}

/* A PS/2 mouse packet: buttons and signed 9-bit deltas, y up. */
static void mouse(int dx, int dy, int dz, uint32_t buttons)
{
    while (dx || dy || dz) {
        int x = dx > 255 ? 255 : dx < -255 ? -255 : dx, y = dy > 255 ? 255 : dy < -255 ? -255 : dy;
        int z = dz > 7 ? 7 : dz < -8 ? -8 : dz;
        dx -= x; dy -= y; dz -= z;
        y = -y;
        uint8_t p[4] = {(uint8_t)(0x08 | (buttons & SDL_BUTTON_LMASK ? 1 : 0) | (buttons & SDL_BUTTON_RMASK ? 2 : 0)
                                   | (buttons & SDL_BUTTON_MMASK ? 4 : 0) | (x < 0 ? 0x10 : 0) | (y < 0 ? 0x20 : 0)),
                        (uint8_t)x, (uint8_t)y, (uint8_t)(z & 0x0F)};
        input_bytes(p, 4, 1);
    }
}

void present_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_QUIT: g_quit = 1; break;
        case SDL_KEYDOWN: case SDL_KEYUP: key(e.key.keysym.scancode, e.type == SDL_KEYUP); break;
        case SDL_MOUSEMOTION: mouse(e.motion.xrel, e.motion.yrel, 0, e.motion.state); break;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: { uint32_t b = SDL_GetMouseState(NULL, NULL); uint8_t p[4] = {(uint8_t)(0x08 | (b & 1 ? 1 : 0) | (b & 4 ? 2 : 0) | (b & 2 ? 4 : 0)), 0, 0, 0}; input_bytes(p, 4, 1); break; }
        case SDL_MOUSEWHEEL: mouse(0, 0, -e.wheel.y, SDL_GetMouseState(NULL, NULL)); break;
        }
    }
}

/* Scripted input for hidden test runs: "--keys TEXT@SECONDS" types TEXT. */
void present_type(const char *text)
{
    for (const char *p = text; *p; p++) {
        char ch = *p;
        int shift = (ch >= 'A' && ch <= 'Z') || strchr("!@#$%^&*()_+{}|:\"<>?~", ch) != NULL;
        static const char *lo = "`1234567890-=[]\\;',./", *hi = "~!@#$%^&*()_+{}|:\"<>?";
        const char *q = strchr(hi, ch);
        if (q && ch) ch = lo[q - hi];
        SDL_Scancode s = SDL_SCANCODE_UNKNOWN;
        if (ch >= 'a' && ch <= 'z') s = (SDL_Scancode)(SDL_SCANCODE_A + (ch - 'a'));
        else if (ch >= 'A' && ch <= 'Z') s = (SDL_Scancode)(SDL_SCANCODE_A + (ch - 'A'));
        else if (ch >= '1' && ch <= '9') s = (SDL_Scancode)(SDL_SCANCODE_1 + (ch - '1'));
        else if (ch == '0') s = SDL_SCANCODE_0;
        else if (ch == ' ') s = SDL_SCANCODE_SPACE;
        else if (ch == '\n') s = SDL_SCANCODE_RETURN;
        else if (ch == '\t') s = SDL_SCANCODE_TAB;
        else { static const char *sy = "-=[]\\;',./`"; static const SDL_Scancode sc[] = {SDL_SCANCODE_MINUS, SDL_SCANCODE_EQUALS,
                 SDL_SCANCODE_LEFTBRACKET, SDL_SCANCODE_RIGHTBRACKET, SDL_SCANCODE_BACKSLASH, SDL_SCANCODE_SEMICOLON,
                 SDL_SCANCODE_APOSTROPHE, SDL_SCANCODE_COMMA, SDL_SCANCODE_PERIOD, SDL_SCANCODE_SLASH, SDL_SCANCODE_GRAVE};
               const char *r = strchr(sy, ch); if (r && ch) s = sc[r - sy]; }
        if (s == SDL_SCANCODE_UNKNOWN) continue;
        if (shift) key(SDL_SCANCODE_LSHIFT, 0);
        key(s, 0); key(s, 1);
        if (shift) key(SDL_SCANCODE_LSHIFT, 1);
    }
}
