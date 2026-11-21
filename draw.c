/*
 * draw.c - SDL2 graphics for Lua.
 *
 * Build with CMake:
 *   cmake -S . -B build
 *   cmake --build build
 *
 * Or manually (Lua 5.4, Linux):
 *   gcc -O2 -Wall -Wextra -fPIC -shared -o draw.so draw.c \
 *       $(pkg-config --cflags --libs lua5.4 sdl2) -lm
 */

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PALETTE_NAME_MAX 64
#define PALETTE_INDEX_MAX (INT_MAX - 1)
#define PALETTE_INITIAL_CAP 64
#define PALETTE_MAP_INITIAL 128
#define PALETTE_MAP_EMPTY (-1)
#define PALETTE_MAP_TOMB (-2)
#define SPRITE_MAX 128
#define SPRITE_NAME_MAX 64
#define SPRITE_SCALE_MAX 1000000
#define TRIANGLE_COORD_LIMIT 500000000LL

static const double k_diag = 0.70710678118654752440;
static const double k_cos[8] = {
    1.0, k_diag, 0.0, -k_diag, -1.0, -k_diag, 0.0, k_diag
};
static const double k_sin[8] = {
    0.0, k_diag, 1.0, k_diag, 0.0, -k_diag, -1.0, -k_diag
};

typedef struct {
    int used;
    char name[PALETTE_NAME_MAX];
    int r, g, b;
} PaletteEntry;

typedef struct {
    int used;
    char name[SPRITE_NAME_MAX];
    int width;
    int height;
    int *pixels;             /* palette indices, -1 = transparent */
} SpriteEntry;

typedef struct {
    int width;
    int height;
    uint32_t *pixels;        /* ARGB8888, width * height */
    int initialized;
    int open;
    int sdl_owned;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    SDL_Rect dirty;          /* pending region, valid when has_dirty != 0 */
    int has_dirty;
    int fps;
    int vsync;
    Uint64 last_tick;
} DrawState;

static DrawState g_draw;
static PaletteEntry *g_palette;
static int g_palette_count;
static int g_palette_cap;
static int g_palette_free_hint;
static int *g_palette_map;
static int g_palette_map_size;
static int g_palette_map_used;
static SpriteEntry g_sprites[SPRITE_MAX];

/*
 * 8x8 monochrome bitmap font for ASCII 0..127.
 *
 * Author: Daniel Hepper <daniel@hepper.net>
 * License: Public Domain
 * Based on public-domain IBM VGA fonts by Marcel Sondaar.
 */
static const unsigned char font8x8[128][8] = {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0000 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0001 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0002 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0003 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0004 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0005 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0006 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0007 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0008 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0009 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+000A */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+000B */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+000C */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+000D */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+000E */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+000F */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0010 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0011 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0012 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0013 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0014 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0015 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0016 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0017 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0018 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0019 */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+001A */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+001B */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+001C */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+001D */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+001E */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+001F */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0020 (space) */
    { 0x18, 0x3C, 0x3C, 0x18, 0x18, 0x00, 0x18, 0x00},   /* U+0021 (!) */
    { 0x36, 0x36, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0022 (") */
    { 0x36, 0x36, 0x7F, 0x36, 0x7F, 0x36, 0x36, 0x00},   /* U+0023 (#) */
    { 0x0C, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x0C, 0x00},   /* U+0024 ($) */
    { 0x00, 0x63, 0x33, 0x18, 0x0C, 0x66, 0x63, 0x00},   /* U+0025 (%) */
    { 0x1C, 0x36, 0x1C, 0x6E, 0x3B, 0x33, 0x6E, 0x00},   /* U+0026 (&) */
    { 0x06, 0x06, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0027 (') */
    { 0x18, 0x0C, 0x06, 0x06, 0x06, 0x0C, 0x18, 0x00},   /* U+0028 (() */
    { 0x06, 0x0C, 0x18, 0x18, 0x18, 0x0C, 0x06, 0x00},   /* U+0029 ()) */
    { 0x00, 0x66, 0x3C, 0xFF, 0x3C, 0x66, 0x00, 0x00},   /* U+002A (*) */
    { 0x00, 0x0C, 0x0C, 0x3F, 0x0C, 0x0C, 0x00, 0x00},   /* U+002B (+) */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x06},   /* U+002C (,) */
    { 0x00, 0x00, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x00},   /* U+002D (-) */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x00},   /* U+002E (.) */
    { 0x60, 0x30, 0x18, 0x0C, 0x06, 0x03, 0x01, 0x00},   /* U+002F (/) */
    { 0x3E, 0x63, 0x73, 0x7B, 0x6F, 0x67, 0x3E, 0x00},   /* U+0030 (0) */
    { 0x0C, 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F, 0x00},   /* U+0031 (1) */
    { 0x1E, 0x33, 0x30, 0x1C, 0x06, 0x33, 0x3F, 0x00},   /* U+0032 (2) */
    { 0x1E, 0x33, 0x30, 0x1C, 0x30, 0x33, 0x1E, 0x00},   /* U+0033 (3) */
    { 0x38, 0x3C, 0x36, 0x33, 0x7F, 0x30, 0x78, 0x00},   /* U+0034 (4) */
    { 0x3F, 0x03, 0x1F, 0x30, 0x30, 0x33, 0x1E, 0x00},   /* U+0035 (5) */
    { 0x1C, 0x06, 0x03, 0x1F, 0x33, 0x33, 0x1E, 0x00},   /* U+0036 (6) */
    { 0x3F, 0x33, 0x30, 0x18, 0x0C, 0x0C, 0x0C, 0x00},   /* U+0037 (7) */
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E, 0x00},   /* U+0038 (8) */
    { 0x1E, 0x33, 0x33, 0x3E, 0x30, 0x18, 0x0E, 0x00},   /* U+0039 (9) */
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x00},   /* U+003A (:) */
    { 0x00, 0x0C, 0x0C, 0x00, 0x00, 0x0C, 0x0C, 0x06},   /* U+003B (;) */
    { 0x18, 0x0C, 0x06, 0x03, 0x06, 0x0C, 0x18, 0x00},   /* U+003C (<) */
    { 0x00, 0x00, 0x3F, 0x00, 0x00, 0x3F, 0x00, 0x00},   /* U+003D (=) */
    { 0x06, 0x0C, 0x18, 0x30, 0x18, 0x0C, 0x06, 0x00},   /* U+003E (>) */
    { 0x1E, 0x33, 0x30, 0x18, 0x0C, 0x00, 0x0C, 0x00},   /* U+003F (?) */
    { 0x3E, 0x63, 0x7B, 0x7B, 0x7B, 0x03, 0x1E, 0x00},   /* U+0040 (@) */
    { 0x0C, 0x1E, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x00},   /* U+0041 (A) */
    { 0x3F, 0x66, 0x66, 0x3E, 0x66, 0x66, 0x3F, 0x00},   /* U+0042 (B) */
    { 0x3C, 0x66, 0x03, 0x03, 0x03, 0x66, 0x3C, 0x00},   /* U+0043 (C) */
    { 0x1F, 0x36, 0x66, 0x66, 0x66, 0x36, 0x1F, 0x00},   /* U+0044 (D) */
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x46, 0x7F, 0x00},   /* U+0045 (E) */
    { 0x7F, 0x46, 0x16, 0x1E, 0x16, 0x06, 0x0F, 0x00},   /* U+0046 (F) */
    { 0x3C, 0x66, 0x03, 0x03, 0x73, 0x66, 0x7C, 0x00},   /* U+0047 (G) */
    { 0x33, 0x33, 0x33, 0x3F, 0x33, 0x33, 0x33, 0x00},   /* U+0048 (H) */
    { 0x1E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* U+0049 (I) */
    { 0x78, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E, 0x00},   /* U+004A (J) */
    { 0x67, 0x66, 0x36, 0x1E, 0x36, 0x66, 0x67, 0x00},   /* U+004B (K) */
    { 0x0F, 0x06, 0x06, 0x06, 0x46, 0x66, 0x7F, 0x00},   /* U+004C (L) */
    { 0x63, 0x77, 0x7F, 0x7F, 0x6B, 0x63, 0x63, 0x00},   /* U+004D (M) */
    { 0x63, 0x67, 0x6F, 0x7B, 0x73, 0x63, 0x63, 0x00},   /* U+004E (N) */
    { 0x1C, 0x36, 0x63, 0x63, 0x63, 0x36, 0x1C, 0x00},   /* U+004F (O) */
    { 0x3F, 0x66, 0x66, 0x3E, 0x06, 0x06, 0x0F, 0x00},   /* U+0050 (P) */
    { 0x1E, 0x33, 0x33, 0x33, 0x3B, 0x1E, 0x38, 0x00},   /* U+0051 (Q) */
    { 0x3F, 0x66, 0x66, 0x3E, 0x36, 0x66, 0x67, 0x00},   /* U+0052 (R) */
    { 0x1E, 0x33, 0x07, 0x0E, 0x38, 0x33, 0x1E, 0x00},   /* U+0053 (S) */
    { 0x3F, 0x2D, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* U+0054 (T) */
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x33, 0x3F, 0x00},   /* U+0055 (U) */
    { 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   /* U+0056 (V) */
    { 0x63, 0x63, 0x63, 0x6B, 0x7F, 0x77, 0x63, 0x00},   /* U+0057 (W) */
    { 0x63, 0x63, 0x36, 0x1C, 0x1C, 0x36, 0x63, 0x00},   /* U+0058 (X) */
    { 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x0C, 0x1E, 0x00},   /* U+0059 (Y) */
    { 0x7F, 0x63, 0x31, 0x18, 0x4C, 0x66, 0x7F, 0x00},   /* U+005A (Z) */
    { 0x1E, 0x06, 0x06, 0x06, 0x06, 0x06, 0x1E, 0x00},   /* U+005B ([) */
    { 0x03, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x40, 0x00},   /* U+005C (\) */
    { 0x1E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x1E, 0x00},   /* U+005D (]) */
    { 0x08, 0x1C, 0x36, 0x63, 0x00, 0x00, 0x00, 0x00},   /* U+005E (^) */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF},   /* U+005F (_) */
    { 0x0C, 0x0C, 0x18, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+0060 (`) */
    { 0x00, 0x00, 0x1E, 0x30, 0x3E, 0x33, 0x6E, 0x00},   /* U+0061 (a) */
    { 0x07, 0x06, 0x06, 0x3E, 0x66, 0x66, 0x3B, 0x00},   /* U+0062 (b) */
    { 0x00, 0x00, 0x1E, 0x33, 0x03, 0x33, 0x1E, 0x00},   /* U+0063 (c) */
    { 0x38, 0x30, 0x30, 0x3E, 0x33, 0x33, 0x6E, 0x00},   /* U+0064 (d) */
    { 0x00, 0x00, 0x1E, 0x33, 0x3F, 0x03, 0x1E, 0x00},   /* U+0065 (e) */
    { 0x1C, 0x36, 0x06, 0x0F, 0x06, 0x06, 0x0F, 0x00},   /* U+0066 (f) */
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x1F},   /* U+0067 (g) */
    { 0x07, 0x06, 0x36, 0x6E, 0x66, 0x66, 0x67, 0x00},   /* U+0068 (h) */
    { 0x0C, 0x00, 0x0E, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* U+0069 (i) */
    { 0x30, 0x00, 0x30, 0x30, 0x30, 0x33, 0x33, 0x1E},   /* U+006A (j) */
    { 0x07, 0x06, 0x66, 0x36, 0x1E, 0x36, 0x67, 0x00},   /* U+006B (k) */
    { 0x0E, 0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0x1E, 0x00},   /* U+006C (l) */
    { 0x00, 0x00, 0x33, 0x7F, 0x7F, 0x6B, 0x63, 0x00},   /* U+006D (m) */
    { 0x00, 0x00, 0x1F, 0x33, 0x33, 0x33, 0x33, 0x00},   /* U+006E (n) */
    { 0x00, 0x00, 0x1E, 0x33, 0x33, 0x33, 0x1E, 0x00},   /* U+006F (o) */
    { 0x00, 0x00, 0x3B, 0x66, 0x66, 0x3E, 0x06, 0x0F},   /* U+0070 (p) */
    { 0x00, 0x00, 0x6E, 0x33, 0x33, 0x3E, 0x30, 0x78},   /* U+0071 (q) */
    { 0x00, 0x00, 0x3B, 0x6E, 0x66, 0x06, 0x0F, 0x00},   /* U+0072 (r) */
    { 0x00, 0x00, 0x3E, 0x03, 0x1E, 0x30, 0x1F, 0x00},   /* U+0073 (s) */
    { 0x08, 0x0C, 0x3E, 0x0C, 0x0C, 0x2C, 0x18, 0x00},   /* U+0074 (t) */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x33, 0x6E, 0x00},   /* U+0075 (u) */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x1E, 0x0C, 0x00},   /* U+0076 (v) */
    { 0x00, 0x00, 0x63, 0x6B, 0x7F, 0x7F, 0x36, 0x00},   /* U+0077 (w) */
    { 0x00, 0x00, 0x63, 0x36, 0x1C, 0x36, 0x63, 0x00},   /* U+0078 (x) */
    { 0x00, 0x00, 0x33, 0x33, 0x33, 0x3E, 0x30, 0x1F},   /* U+0079 (y) */
    { 0x00, 0x00, 0x3F, 0x19, 0x0C, 0x26, 0x3F, 0x00},   /* U+007A (z) */
    { 0x38, 0x0C, 0x0C, 0x07, 0x0C, 0x0C, 0x38, 0x00},   /* U+007B ({) */
    { 0x18, 0x18, 0x18, 0x00, 0x18, 0x18, 0x18, 0x00},   /* U+007C (|) */
    { 0x07, 0x0C, 0x0C, 0x38, 0x0C, 0x0C, 0x07, 0x00},   /* U+007D (}) */
    { 0x6E, 0x3B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},   /* U+007E (~) */
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}    /* U+007F */
};

static int clamp_color(lua_Integer v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (int)v;
}

static size_t palette_hash(const char *name)
{
    size_t h = 2166136261u;

    while (*name) {
        h ^= (unsigned char)*name++;
        h *= 16777619u;
    }

    return h;
}

static int palette_grow(int need)
{
    int new_cap;
    PaletteEntry *tmp;

    if (need <= g_palette_cap)
        return 0;

    new_cap = g_palette_cap ? g_palette_cap : PALETTE_INITIAL_CAP;
    while (new_cap < need) {
        if (new_cap > PALETTE_INDEX_MAX / 2) {
            new_cap = need;
            break;
        }
        new_cap *= 2;
    }

    if ((size_t)new_cap > SIZE_MAX / sizeof(PaletteEntry))
        return -1;

    tmp = (PaletteEntry *)realloc(g_palette,
                                  (size_t)new_cap * sizeof(PaletteEntry));
    if (!tmp)
        return -1;

    memset(tmp + g_palette_cap, 0,
           (size_t)(new_cap - g_palette_cap) * sizeof(PaletteEntry));
    g_palette = tmp;
    g_palette_cap = new_cap;
    return 0;
}

static int palette_map_alloc(int size, int **out_map)
{
    int *map;
    int i;

    if ((size_t)size > SIZE_MAX / sizeof(int))
        return -1;

    map = (int *)malloc((size_t)size * sizeof(int));
    if (!map)
        return -1;

    for (i = 0; i < size; ++i)
        map[i] = PALETTE_MAP_EMPTY;

    *out_map = map;
    return 0;
}

static int palette_map_rehash(int new_size)
{
    int *new_map = NULL;
    int i;
    int active = 0;
    int mask = new_size - 1;

    if (palette_map_alloc(new_size, &new_map) != 0)
        return -1;

    for (i = 0; i < g_palette_map_size; ++i) {
        int index = g_palette_map[i];
        int pos;

        if (index < 0 || index >= g_palette_count)
            continue;
        if (!g_palette[index].used || g_palette[index].name[0] == '\0')
            continue;

        pos = (int)(palette_hash(g_palette[index].name) & (size_t)mask);
        while (new_map[pos] != PALETTE_MAP_EMPTY)
            pos = (pos + 1) & mask;

        new_map[pos] = index;
        ++active;
    }

    free(g_palette_map);
    g_palette_map = new_map;
    g_palette_map_size = new_size;
    g_palette_map_used = active;
    return 0;
}

static int palette_map_ready(void)
{
    if (g_palette_map && g_palette_map_size > 0)
        return 0;
    if (palette_map_alloc(PALETTE_MAP_INITIAL, &g_palette_map) != 0)
        return -1;
    g_palette_map_size = PALETTE_MAP_INITIAL;
    return 0;
}

static int find_palette_name(const char *name)
{
    int pos;
    int probe;
    int mask;

    if (!g_palette_map || g_palette_map_size == 0)
        return -1;

    mask = g_palette_map_size - 1;
    pos = (int)(palette_hash(name) & (size_t)mask);

    for (probe = 0; probe < g_palette_map_size; ++probe) {
        int index = g_palette_map[pos];

        if (index == PALETTE_MAP_EMPTY)
            return -1;
        if (index >= 0 && index < g_palette_count &&
            g_palette[index].used && g_palette[index].name[0] != '\0' &&
            strcmp(g_palette[index].name, name) == 0)
            return index;

        pos = (pos + 1) & mask;
    }

    return -1;
}

static void palette_map_remove_name(const char *name)
{
    int pos;
    int probe;
    int mask;

    if (!g_palette_map || g_palette_map_size == 0)
        return;

    mask = g_palette_map_size - 1;
    pos = (int)(palette_hash(name) & (size_t)mask);

    for (probe = 0; probe < g_palette_map_size; ++probe) {
        int index = g_palette_map[pos];

        if (index == PALETTE_MAP_EMPTY)
            return;
        if (index >= 0 && index < g_palette_count &&
            g_palette[index].used && g_palette[index].name[0] != '\0' &&
            strcmp(g_palette[index].name, name) == 0) {
            g_palette_map[pos] = PALETTE_MAP_TOMB;
            return;
        }

        pos = (pos + 1) & mask;
    }
}

static int palette_map_set_name(const char *name, int index)
{
    int pos;
    int probe;
    int mask;
    int first_tomb = -1;

    if (palette_map_ready() != 0)
        return -1;

    if (((size_t)g_palette_map_used + 1) * 10 >=
        (size_t)g_palette_map_size * 7) {
        if (g_palette_map_size > PALETTE_INDEX_MAX / 2)
            return -1;
        if (palette_map_rehash(g_palette_map_size * 2) != 0)
            return -1;
    }

    mask = g_palette_map_size - 1;
    pos = (int)(palette_hash(name) & (size_t)mask);

    for (probe = 0; probe < g_palette_map_size; ++probe) {
        int current = g_palette_map[pos];

        if (current == PALETTE_MAP_EMPTY)
            break;
        if (current == PALETTE_MAP_TOMB) {
            if (first_tomb < 0)
                first_tomb = pos;
        } else if (current >= 0 && current < g_palette_count &&
                   g_palette[current].used &&
                   g_palette[current].name[0] != '\0' &&
                   strcmp(g_palette[current].name, name) == 0) {
            g_palette_map[pos] = index;
            return 0;
        }

        pos = (pos + 1) & mask;
    }

    if (first_tomb >= 0) {
        pos = first_tomb;
    } else {
        ++g_palette_map_used;
    }
    g_palette_map[pos] = index;
    return 0;
}

static int find_free_palette(void)
{
    int i;

    if (g_palette_count > 0) {
        for (i = 0; i < g_palette_count; ++i) {
            int index = g_palette_free_hint + i;

            if (index >= g_palette_count)
                index -= g_palette_count;
            if (!g_palette[index].used) {
                g_palette_free_hint = index + 1;
                if (g_palette_free_hint >= g_palette_count)
                    g_palette_free_hint = 0;
                return index;
            }
        }
    }

    if (g_palette_count >= PALETTE_INDEX_MAX)
        return -1;
    if (palette_grow(g_palette_count + 1) != 0)
        return -1;

    i = g_palette_count++;
    g_palette_free_hint = 0;
    return i;
}

static void free_palette(void)
{
    free(g_palette);
    g_palette = NULL;
    g_palette_count = 0;
    g_palette_cap = 0;
    g_palette_free_hint = 0;

    free(g_palette_map);
    g_palette_map = NULL;
    g_palette_map_size = 0;
    g_palette_map_used = 0;
}

static int read_color(lua_State *L, int first, int *r, int *g, int *b)
{
    int top = lua_gettop(L);

    if (top < first)
        return -1;

    if (top >= first + 2) {
        *r = clamp_color(luaL_checkinteger(L, first));
        *g = clamp_color(luaL_checkinteger(L, first + 1));
        *b = clamp_color(luaL_checkinteger(L, first + 2));
        return 0;
    }

    if (top == first + 1 &&
        lua_type(L, first) != LUA_TSTRING &&
        lua_type(L, first) != LUA_TNUMBER)
        return luaL_error(L,
            "color must be r, g, b or a palette name/index");

    if (lua_type(L, first) == LUA_TSTRING) {
        const char *name = lua_tostring(L, first);
        int index = find_palette_name(name);

        if (index < 0)
            return luaL_error(L, "palette color '%s' is not defined", name);

        *r = g_palette[index].r;
        *g = g_palette[index].g;
        *b = g_palette[index].b;
        return 0;
    }

    if (lua_type(L, first) == LUA_TNUMBER) {
        lua_Integer raw = luaL_checkinteger(L, first);
        int index;

        if (raw < 0 || raw > PALETTE_INDEX_MAX)
            return luaL_error(L, "palette index is out of range");

        index = (int)raw;
        if (index >= g_palette_count || !g_palette[index].used)
            return luaL_error(L, "palette index %d is not defined", index);

        *r = g_palette[index].r;
        *g = g_palette[index].g;
        *b = g_palette[index].b;
        return 0;
    }

    return luaL_error(L, "invalid color argument");
}

static int read_fill(lua_State *L, int color_first, const char *name)
{
    int top = lua_gettop(L);
    int index;
    lua_Integer value;

    if (top >= color_first + 3)
        index = color_first + 3;
    else if (top == color_first + 1)
        index = color_first + 1;
    else
        return 0;

    value = luaL_optinteger(L, index, 0);
    if (value < -1)
        return luaL_error(L, "%s: fill must be -1, 0, or positive", name);
    if (value > INT_MAX)
        return luaL_error(L, "%s: fill is too large", name);

    return (int)value;
}

static uint32_t make_color(int r, int g, int b)
{
    return 0xFF000000u |
           ((uint32_t)r << 16) |
           ((uint32_t)g << 8) |
           (uint32_t)b;
}

static int find_sprite_name(const char *name)
{
    int i;

    for (i = 0; i < SPRITE_MAX; ++i) {
        if (g_sprites[i].used &&
            strcmp(g_sprites[i].name, name) == 0)
            return i;
    }

    return -1;
}

static int first_free_sprite(void)
{
    int i;

    for (i = 0; i < SPRITE_MAX; ++i) {
        if (!g_sprites[i].used)
            return i;
    }

    return -1;
}

static int parse_sprite_string(const char *s, int **out, int *out_len)
{
    int *values = NULL;
    int len = 0;
    int cap = 0;
    const char *p = s;

    while (*p) {
        char *end;
        long v;

        while (*p == ' ' || *p == '\t' || *p == '\n' ||
               *p == '\r' || *p == ',')
            ++p;
        if (*p == '\0')
            break;

        v = strtol(p, &end, 10);
        if (end == p || v < -1 || v > PALETTE_INDEX_MAX) {
            free(values);
            return -1;
        }

        if (len == cap) {
            int new_cap = cap ? cap * 2 : 16;
            int *tmp = (int *)realloc(values,
                                      (size_t)new_cap * sizeof(int));

            if (!tmp) {
                free(values);
                return -2;
            }

            values = tmp;
            cap = new_cap;
        }

        values[len++] = (int)v;
        p = end;
    }

    *out = values;
    *out_len = len;
    return 0;
}

static int parse_scale(lua_State *L, int index)
{
    lua_Number n;
    lua_Integer scale;

    if (lua_isnoneornil(L, index))
        return 1;

    n = luaL_checknumber(L, index);
    if (n < 1.0)
        return luaL_error(L, "draw.draw: scale must be >= 1");
    if (n > (lua_Number)SPRITE_SCALE_MAX)
        return luaL_error(L, "draw.draw: scale is too large");

    scale = (lua_Integer)n;
    if ((lua_Number)scale != n)
        return luaL_error(L, "draw.draw: scale must be an integer");

    return (int)scale;
}

static int parse_angle(lua_State *L, int index)
{
    lua_Number n;
    lua_Integer angle;

    if (lua_isnoneornil(L, index))
        return 0;

    n = luaL_checknumber(L, index);
    if (n < -1000000.0 || n > 1000000.0)
        return luaL_error(L, "draw.draw: angle is out of range");

    angle = (lua_Integer)n;
    if ((lua_Number)angle != n)
        return luaL_error(L, "draw.draw: angle must be an integer");
    if (angle % 45 != 0)
        return luaL_error(L,
            "draw.draw: angle must be a multiple of 45");

    angle %= 360;
    if (angle < 0)
        angle += 360;

    return (int)angle;
}

static int visible_range(double start, double size, int screen_size,
                         int *out_start, int *out_end)
{
    double s = start;
    double e = start + size - 1.0;

    if (s < 0.0)
        s = 0.0;
    if (e > (double)(screen_size - 1))
        e = (double)(screen_size - 1);
    if (e < s)
        return 0;

    *out_start = (int)ceil(s);
    *out_end = (int)floor(e);
    return *out_start <= *out_end;
}

static void mark_dirty(int x, int y)
{
    int x1, y1;

    if (x < 0 || x >= g_draw.width || y < 0 || y >= g_draw.height)
        return;

    if (!g_draw.has_dirty) {
        g_draw.dirty.x = x;
        g_draw.dirty.y = y;
        g_draw.dirty.w = 1;
        g_draw.dirty.h = 1;
        g_draw.has_dirty = 1;
        return;
    }

    x1 = g_draw.dirty.x + g_draw.dirty.w;
    y1 = g_draw.dirty.y + g_draw.dirty.h;

    if (x < g_draw.dirty.x)
        g_draw.dirty.x = x;
    if (y < g_draw.dirty.y)
        g_draw.dirty.y = y;
    if (x + 1 > x1)
        x1 = x + 1;
    if (y + 1 > y1)
        y1 = y + 1;

    g_draw.dirty.w = x1 - g_draw.dirty.x;
    g_draw.dirty.h = y1 - g_draw.dirty.y;
}

static void mark_dirty_rect(int x, int y, int w, int h)
{
    int x1 = x + w;
    int y1 = y + h;

    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > g_draw.width)
        x1 = g_draw.width;
    if (y1 > g_draw.height)
        y1 = g_draw.height;
    if (x >= x1 || y >= y1)
        return;

    if (!g_draw.has_dirty) {
        g_draw.dirty.x = x;
        g_draw.dirty.y = y;
        g_draw.dirty.w = x1 - x;
        g_draw.dirty.h = y1 - y;
        g_draw.has_dirty = 1;
        return;
    }

    {
        int dx1 = g_draw.dirty.x + g_draw.dirty.w;
        int dy1 = g_draw.dirty.y + g_draw.dirty.h;

        if (x < g_draw.dirty.x)
            g_draw.dirty.x = x;
        if (y < g_draw.dirty.y)
            g_draw.dirty.y = y;
        if (x1 > dx1)
            dx1 = x1;
        if (y1 > dy1)
            dy1 = y1;

        g_draw.dirty.w = dx1 - g_draw.dirty.x;
        g_draw.dirty.h = dy1 - g_draw.dirty.y;
    }
}

static void fill_span(long long x0, long long x1, long long y, uint32_t color)
{
    int xi0, xi1, yi, x;
    uint32_t *row;

    if (y < 0 || y >= g_draw.height)
        return;

    if (x0 > x1) {
        long long t = x0;
        x0 = x1;
        x1 = t;
    }

    if (x1 < 0 || x0 >= g_draw.width)
        return;

    if (x0 < 0)
        x0 = 0;
    if (x1 >= g_draw.width)
        x1 = g_draw.width - 1;

    yi = (int)y;
    xi0 = (int)x0;
    xi1 = (int)x1;

    row = g_draw.pixels + (size_t)yi * (size_t)g_draw.width;
    for (x = xi0; x <= xi1; ++x)
        row[x] = color;

    mark_dirty_rect(xi0, yi, xi1 - xi0 + 1, 1);
}

static void fill_rect(long long x, long long y, long long w, long long h,
                      int fill, uint32_t color)
{
    long long y0, y1, row;
    long long t;
    long long inner_h;

    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (w == 0 || h == 0)
        return;

    if (fill == 0) {
        y0 = y;
        y1 = y + h;

        if (y0 < 0)
            y0 = 0;
        if (y1 > g_draw.height)
            y1 = g_draw.height;

        for (row = y0; row < y1; ++row)
            fill_span(x, x + w - 1, row, color);
        return;
    }

    t = fill < 0 ? 1 : fill;
    if (t > (w + 1) / 2)
        t = (w + 1) / 2;
    if (t > (h + 1) / 2)
        t = (h + 1) / 2;
    if (t <= 0)
        return;

    inner_h = h - 2 * t;

    fill_rect(x, y, w, t, 0, color);
    fill_rect(x, y + h - t, w, t, 0, color);
    if (inner_h > 0) {
        fill_rect(x, y + t, t, inner_h, 0, color);
        fill_rect(x + w - t, y + t, t, inner_h, 0, color);
    }
}

static long long isqrt_ll(long long n)
{
    long long r = 0;
    long long bit = 1LL << 62;

    if (n <= 0)
        return 0;

    while (bit > n)
        bit >>= 2;

    while (bit != 0) {
        if (n >= r + bit) {
            n -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }

    return r;
}

static void fill_circle(long long cx, long long cy, long long radius,
                        int fill, uint32_t color)
{
    long long dy, dy0, dy1, r2;
    long long t;
    long long inner_r;

    if (radius < 0)
        return;

    r2 = radius * radius;

    dy0 = -radius;
    dy1 = radius;

    if (dy0 < -cy)
        dy0 = -cy;
    if (dy1 > (long long)g_draw.height - 1 - cy)
        dy1 = (long long)g_draw.height - 1 - cy;

    if (fill == 0) {
        for (dy = dy0; dy <= dy1; ++dy) {
            long long dx = isqrt_ll(r2 - dy * dy);
            fill_span(cx - dx, cx + dx, cy + dy, color);
        }
        return;
    }

    if (radius == 0) {
        fill_span(cx, cx, cy, color);
        return;
    }

    t = fill < 0 ? 1 : fill;
    if (t > radius + 1)
        t = radius + 1;
    inner_r = radius - t;

    for (dy = dy0; dy <= dy1; ++dy) {
        long long outer = isqrt_ll(r2 - dy * dy);
        long long inner = -1;

        if (inner_r >= 0) {
            long long inner2 = inner_r * inner_r - dy * dy;

            if (inner2 >= 0)
                inner = isqrt_ll(inner2);
        }

        if (inner < 0) {
            fill_span(cx - outer, cx + outer, cy + dy, color);
        } else {
            fill_span(cx - outer, cx - inner - 1, cy + dy, color);
            fill_span(cx + inner + 1, cx + outer, cy + dy, color);
        }
    }
}

static long long triangle_edge(long long ax, long long ay,
                               long long bx, long long by,
                               long long px2, long long py2)
{
    return (bx - ax) * (py2 - 2 * ay) -
           (by - ay) * (px2 - 2 * ax);
}

static void fill_triangle(long long x1, long long y1,
                           long long x2, long long y2,
                           long long x3, long long y3,
                           int fill, uint32_t color)
{
    long long area2;
    long long minx, maxx, miny, maxy;
    long long x, y, start;
    long long lim1 = 0, lim2 = 0, lim3 = 0;

    area2 = (x2 - x1) * (y3 - y1) - (y2 - y1) * (x3 - x1);
    if (area2 == 0)
        return;

    minx = x1;
    if (x2 < minx)
        minx = x2;
    if (x3 < minx)
        minx = x3;
    maxx = x1;
    if (x2 > maxx)
        maxx = x2;
    if (x3 > maxx)
        maxx = x3;

    miny = y1;
    if (y2 < miny)
        miny = y2;
    if (y3 < miny)
        miny = y3;
    maxy = y1;
    if (y2 > maxy)
        maxy = y2;
    if (y3 > maxy)
        maxy = y3;

    if (fill != 0) {
        long long t = fill < 0 ? 1 : fill;
        long long span_x = maxx - minx;
        long long span_y = maxy - miny;
        long long max_span = span_x > span_y ? span_x : span_y;
        long long dx1 = x2 - x1, dy1 = y2 - y1;
        long long dx2 = x3 - x2, dy2 = y3 - y2;
        long long dx3 = x1 - x3, dy3 = y1 - y3;

        if (t > max_span)
            t = max_span;

        lim1 = 2 * t * isqrt_ll(dx1 * dx1 + dy1 * dy1);
        lim2 = 2 * t * isqrt_ll(dx2 * dx2 + dy2 * dy2);
        lim3 = 2 * t * isqrt_ll(dx3 * dx3 + dy3 * dy3);
    }

    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx >= g_draw.width)
        maxx = (long long)g_draw.width - 1;
    if (maxy >= g_draw.height)
        maxy = (long long)g_draw.height - 1;
    if (minx > maxx || miny > maxy)
        return;

    for (y = miny; y <= maxy; ++y) {
        long long py2 = 2 * y + 1;

        start = -1;
        for (x = minx; x <= maxx; ++x) {
            long long px2 = 2 * x + 1;
            long long e1 = triangle_edge(x1, y1, x2, y2, px2, py2);
            long long e2 = triangle_edge(x2, y2, x3, y3, px2, py2);
            long long e3 = triangle_edge(x3, y3, x1, y1, px2, py2);
            int inside;

            if (area2 > 0)
                inside = e1 >= 0 && e2 >= 0 && e3 >= 0;
            else
                inside = e1 <= 0 && e2 <= 0 && e3 <= 0;

            if (!inside) {
                if (start >= 0) {
                    fill_span(start, x - 1, y, color);
                    start = -1;
                }
                continue;
            }

            if (fill == 0) {
                if (start < 0)
                    start = x;
            } else {
                long long a1 = area2 > 0 ? e1 : -e1;
                long long a2 = area2 > 0 ? e2 : -e2;
                long long a3 = area2 > 0 ? e3 : -e3;

                if (a1 <= lim1 || a2 <= lim2 || a3 <= lim3)
                    fill_span(x, x, y, color);
            }
        }
        if (start >= 0)
            fill_span(start, maxx, y, color);
    }
}

static void present(void)
{
    if (!g_draw.initialized || !g_draw.open)
        return;

    if (g_draw.has_dirty) {
        SDL_Rect r = g_draw.dirty;
        const void *pixels = g_draw.pixels +
            (size_t)r.y * (size_t)g_draw.width + (size_t)r.x;

        SDL_UpdateTexture(g_draw.texture, &r, pixels,
                          g_draw.width * (int)sizeof(uint32_t));
        g_draw.has_dirty = 0;
    }

    SDL_RenderClear(g_draw.renderer);
    SDL_RenderCopy(g_draw.renderer, g_draw.texture, NULL, NULL);
    SDL_RenderPresent(g_draw.renderer);
}

static void handle_event(const SDL_Event *e)
{
    if (e->type == SDL_QUIT) {
        g_draw.open = 0;
    } else if (e->type == SDL_WINDOWEVENT &&
               e->window.event == SDL_WINDOWEVENT_CLOSE) {
        g_draw.open = 0;
    }
}

static void pump_events(void)
{
    SDL_Event e;

    if (!g_draw.initialized || !g_draw.open)
        return;

    while (SDL_PollEvent(&e))
        handle_event(&e);
}

static void wait_events_timeout(int ms)
{
    Uint32 start = SDL_GetTicks();

    while (g_draw.open) {
        Uint32 elapsed = SDL_GetTicks() - start;
        int remaining;
        SDL_Event e;

        if (elapsed >= (Uint32)ms)
            break;

        remaining = (int)((Uint32)ms - elapsed);
        if (!SDL_WaitEventTimeout(&e, remaining))
            break;

        handle_event(&e);
    }
}

static void wait_frame_time(void)
{
    Uint64 freq;
    Uint64 target;
    Uint64 now;
    Uint64 next;
    Uint32 wait_ms;

    if (!g_draw.initialized || g_draw.fps <= 0)
        return;

    freq = SDL_GetPerformanceFrequency();
    if (freq == 0)
        return;

    target = freq / (Uint64)g_draw.fps;
    if (target == 0)
        target = 1;

    now = SDL_GetPerformanceCounter();
    if (now < g_draw.last_tick)
        g_draw.last_tick = now;

    next = g_draw.last_tick + target;
    if (now >= next) {
        g_draw.last_tick = now;
        return;
    }

    wait_ms = (Uint32)(((next - now) * 1000 + freq - 1) / freq);
    if (wait_ms > 0)
        wait_events_timeout((int)wait_ms);

    g_draw.last_tick = next;
}

static int ensure_open(lua_State *L, const char *name)
{
    (void)L;
    (void)name;

    if (!g_draw.initialized || !g_draw.open)
        return 0;

    pump_events();
    return g_draw.open;
}

static int l_pres(lua_State *L)
{
    if (!g_draw.initialized || !g_draw.open) {
        lua_pushboolean(L, 0);
        return 1;
    }

    pump_events();
    if (!g_draw.open) {
        lua_pushboolean(L, 0);
        return 1;
    }

    wait_frame_time();
    pump_events();
    if (!g_draw.open) {
        lua_pushboolean(L, 0);
        return 1;
    }

    present();
    lua_pushboolean(L, 1);
    return 1;
}

static void put_pixel(long long x, long long y, uint32_t color)
{
    if (x < 0 || x >= (long long)g_draw.width ||
        y < 0 || y >= (long long)g_draw.height)
        return;

    g_draw.pixels[(size_t)y * (size_t)g_draw.width + (size_t)x] = color;
    mark_dirty((int)x, (int)y);
}

static int glyph_pixel(const unsigned char *glyph, int dx, int dy, int size)
{
    int sx0 = dx * 8 / size;
    int sy0 = dy * 8 / size;

    if (size >= 8) {
        int sx = sx0 > 7 ? 7 : sx0;
        int sy = sy0 > 7 ? 7 : sy0;
        return (glyph[sy] >> sx) & 1;
    } else {
        int sx1 = (dx + 1) * 8 / size;
        int sy1 = (dy + 1) * 8 / size;
        int sx, sy;

        if (sx1 <= sx0)
            sx1 = sx0 + 1;
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        if (sx1 > 8)
            sx1 = 8;
        if (sy1 > 8)
            sy1 = 8;

        for (sy = sy0; sy < sy1; ++sy) {
            for (sx = sx0; sx < sx1; ++sx) {
                if ((glyph[sy] >> sx) & 1)
                    return 1;
            }
        }
        return 0;
    }
}

static void destroy_resources(void)
{
    if (g_draw.texture) {
        SDL_DestroyTexture(g_draw.texture);
        g_draw.texture = NULL;
    }
    if (g_draw.renderer) {
        SDL_DestroyRenderer(g_draw.renderer);
        g_draw.renderer = NULL;
    }
    if (g_draw.window) {
        SDL_DestroyWindow(g_draw.window);
        g_draw.window = NULL;
    }

    free(g_draw.pixels);
    g_draw.pixels = NULL;
    g_draw.width = 0;
    g_draw.height = 0;
    g_draw.initialized = 0;
    g_draw.open = 0;
    g_draw.has_dirty = 0;
    g_draw.fps = 60;
    g_draw.vsync = 0;
    g_draw.last_tick = 0;

    if (g_draw.sdl_owned) {
        SDL_Quit();
        g_draw.sdl_owned = 0;
    }
}

static int l_open(lua_State *L)
{
    lua_Integer w = luaL_checkinteger(L, 1);
    lua_Integer h = luaL_checkinteger(L, 2);
    lua_Integer fps = luaL_optinteger(L, 3, 60);
    size_t count;
    uint32_t *pixels;
    Uint64 freq;
    Uint64 target;

    if (w <= 0 || h <= 0)
        return luaL_error(L, "draw.open: width and height must be positive");
    if (fps <= 0 || fps > 1000)
        return luaL_error(L, "draw.open: fps must be between 1 and 1000");

    if ((size_t)w > SIZE_MAX / (size_t)h / sizeof(uint32_t))
        return luaL_error(L, "draw.open: dimensions are too large");

    destroy_resources();

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            return luaL_error(L, "draw.open: SDL_Init failed: %s",
                              SDL_GetError());
        g_draw.sdl_owned = 1;
    }

    g_draw.window = SDL_CreateWindow(
        "Lua Draw",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        (int)w,
        (int)h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_draw.window) {
        destroy_resources();
        return luaL_error(L, "draw.open: cannot create window: %s",
                          SDL_GetError());
    }

    g_draw.vsync = 0;
    g_draw.renderer = SDL_CreateRenderer(
        g_draw.window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (g_draw.renderer) {
        g_draw.vsync = 1;
    } else {
        g_draw.renderer = SDL_CreateRenderer(
            g_draw.window, -1, SDL_RENDERER_ACCELERATED);
    }
    if (!g_draw.renderer) {
        g_draw.renderer = SDL_CreateRenderer(
            g_draw.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!g_draw.renderer) {
        destroy_resources();
        return luaL_error(L, "draw.open: cannot create renderer: %s",
                          SDL_GetError());
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_RenderSetLogicalSize(g_draw.renderer, (int)w, (int)h);

    g_draw.texture = SDL_CreateTexture(
        g_draw.renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        (int)w,
        (int)h);
    if (!g_draw.texture) {
        destroy_resources();
        return luaL_error(L, "draw.open: cannot create texture: %s",
                          SDL_GetError());
    }

    count = (size_t)w * (size_t)h;
    pixels = (uint32_t *)calloc(count, sizeof(uint32_t));
    if (!pixels) {
        destroy_resources();
        return luaL_error(L, "draw.open: out of memory");
    }

    g_draw.pixels = pixels;
    g_draw.width = (int)w;
    g_draw.height = (int)h;
    g_draw.fps = (int)fps;
    g_draw.initialized = 1;
    g_draw.open = 1;
    g_draw.has_dirty = 0;

    g_draw.last_tick = SDL_GetPerformanceCounter();
    freq = SDL_GetPerformanceFrequency();
    if (!g_draw.vsync && freq != 0) {
        target = freq / (Uint64)g_draw.fps;
        if (target == 0)
            target = 1;
        if (g_draw.last_tick > target)
            g_draw.last_tick -= target;
        else
            g_draw.last_tick = 0;
    }

    mark_dirty_rect(0, 0, (int)w, (int)h);
    return 0;
}

static int l_palette(lua_State *L)
{
    int index;
    int r, g, b;
    const char *name;
    size_t len;

    r = clamp_color(luaL_checkinteger(L, 2));
    g = clamp_color(luaL_checkinteger(L, 3));
    b = clamp_color(luaL_checkinteger(L, 4));

    if (lua_type(L, 1) == LUA_TSTRING) {
        name = luaL_checkstring(L, 1);
        len = strlen(name);
        if (len == 0)
            return luaL_error(L, "draw.palette: name must not be empty");
        if (len >= PALETTE_NAME_MAX)
            return luaL_error(L, "draw.palette: name is too long");

        index = find_palette_name(name);
        if (index < 0) {
            index = find_free_palette();
            if (index < 0)
                return luaL_error(L, "draw.palette: out of memory");

            memcpy(g_palette[index].name, name, len + 1);
            g_palette[index].used = 1;
            if (palette_map_set_name(name, index) != 0) {
                g_palette[index].used = 0;
                g_palette[index].name[0] = '\0';
                return luaL_error(L, "draw.palette: out of memory");
            }
        }
    } else {
        lua_Integer raw = luaL_checkinteger(L, 1);

        if (raw < 0 || raw > PALETTE_INDEX_MAX)
            return luaL_error(L, "draw.palette: index is out of range");

        index = (int)raw;
        if (index >= g_palette_count) {
            if (palette_grow(index + 1) != 0)
                return luaL_error(L, "draw.palette: out of memory");
            g_palette_count = index + 1;
        }
        if (g_palette[index].name[0] != '\0')
            palette_map_remove_name(g_palette[index].name);
        g_palette[index].used = 1;
        g_palette[index].name[0] = '\0';
    }

    g_palette[index].r = r;
    g_palette[index].g = g;
    g_palette[index].b = b;

    lua_pushinteger(L, index);
    return 1;
}

static int l_sprite(lua_State *L)
{
    const char *name;
    size_t name_len;
    int height;
    int width;
    int row;
    int col;
    int *pixels;
    int index;
    size_t i;

    name = luaL_checkstring(L, 1);
    name_len = strlen(name);
    if (name_len == 0)
        return luaL_error(L, "draw.sprite: name must not be empty");
    if (name_len >= SPRITE_NAME_MAX)
        return luaL_error(L, "draw.sprite: name is too long");

    luaL_checktype(L, 2, LUA_TTABLE);
    height = (int)lua_rawlen(L, 2);
    if (height <= 0)
        return luaL_error(L, "draw.sprite: sprite is empty");

    width = 0;
    for (row = 0; row < height; ++row) {
        int row_width;

        lua_rawgeti(L, 2, row + 1);

        if (lua_type(L, -1) == LUA_TTABLE) {
            row_width = (int)lua_rawlen(L, -1);
        } else if (lua_type(L, -1) == LUA_TSTRING) {
            int *tmp = NULL;
            int tmp_len = 0;
            int rc = parse_sprite_string(lua_tostring(L, -1),
                                         &tmp, &tmp_len);

            if (rc != 0) {
                lua_pop(L, 1);
                if (rc == -2)
                    return luaL_error(L, "draw.sprite: out of memory");
                return luaL_error(L,
                    "draw.sprite: invalid number in row %d", row + 1);
            }

            free(tmp);
            row_width = tmp_len;
        } else {
            lua_pop(L, 1);
            return luaL_error(L,
                "draw.sprite: row %d must be a table or string",
                row + 1);
        }

        if (row_width > width)
            width = row_width;

        lua_pop(L, 1);
    }

    if (width <= 0)
        return luaL_error(L, "draw.sprite: sprite is empty");

    pixels = (int *)malloc((size_t)width * (size_t)height * sizeof(int));
    if (!pixels)
        return luaL_error(L, "draw.sprite: out of memory");

    for (i = 0; i < (size_t)width * (size_t)height; ++i)
        pixels[i] = -1;

    for (row = 0; row < height; ++row) {
        lua_rawgeti(L, 2, row + 1);

        if (lua_type(L, -1) == LUA_TTABLE) {
            int row_width = (int)lua_rawlen(L, -1);

            for (col = 0; col < row_width; ++col) {
                int value;

                lua_rawgeti(L, -1, col + 1);

                if (lua_isnil(L, -1)) {
                    value = -1;
                } else if (lua_type(L, -1) == LUA_TNUMBER) {
                    lua_Integer raw = luaL_checkinteger(L, -1);

                    if (raw < -1 || raw > PALETTE_INDEX_MAX) {
                        lua_pop(L, 2);
                        free(pixels);
                        return luaL_error(L,
                            "draw.sprite: palette index is out of range in row %d",
                            row + 1);
                    }
                    value = (int)raw;
                } else {
                    lua_pop(L, 2);
                    free(pixels);
                    return luaL_error(L,
                        "draw.sprite: pixel in row %d must be a number",
                        row + 1);
                }

                pixels[(size_t)row * width + col] = value;
                lua_pop(L, 1);
            }
        } else {
            int *tmp = NULL;
            int tmp_len = 0;
            int rc = parse_sprite_string(lua_tostring(L, -1),
                                         &tmp, &tmp_len);

            if (rc != 0) {
                lua_pop(L, 1);
                free(pixels);
                if (rc == -2)
                    return luaL_error(L, "draw.sprite: out of memory");
                return luaL_error(L,
                    "draw.sprite: invalid number in row %d", row + 1);
            }

            for (col = 0; col < tmp_len; ++col) {
                int value = tmp[col];

                if (value < -1 || value > PALETTE_INDEX_MAX) {
                    free(tmp);
                    lua_pop(L, 1);
                    free(pixels);
                    return luaL_error(L,
                        "draw.sprite: palette index %d in row %d is out of range",
                        value, row + 1);
                }

                pixels[(size_t)row * width + col] = value;
            }

            free(tmp);
        }

        lua_pop(L, 1);
    }

    index = find_sprite_name(name);
    if (index < 0) {
        index = first_free_sprite();
        if (index < 0) {
            free(pixels);
            return luaL_error(L,
                "draw.sprite: sprite storage is full (max %d)",
                SPRITE_MAX);
        }
        memcpy(g_sprites[index].name, name, name_len + 1);
        g_sprites[index].used = 1;
    } else {
        free(g_sprites[index].pixels);
    }

    g_sprites[index].pixels = pixels;
    g_sprites[index].width = width;
    g_sprites[index].height = height;

    lua_pushinteger(L, index);
    return 1;
}

static int l_draw_sprite(lua_State *L)
{
    lua_Integer x, y;
    const char *name;
    int scale;
    int angle;
    int step;
    int index;
    int sx, sy;
    int sx0, sx1, sy0, sy1;
    int col, row;
    int value;
    uint32_t color;
    size_t i;
    size_t count;
    SpriteEntry *sprite;
    double W, H, bw, bh;
    double xd, yd;
    double half_w, half_h, half_bw, half_bh;
    double c, s;
    double dx, dy, src_cx, src_cy, p, q;

    if (!ensure_open(L, "draw.draw"))
        return 0;

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    name = luaL_checkstring(L, 3);
    scale = parse_scale(L, 4);
    angle = parse_angle(L, 5);

    index = find_sprite_name(name);
    if (index < 0)
        return luaL_error(L,
            "draw.draw: sprite '%s' is not defined", name);

    sprite = &g_sprites[index];
    count = (size_t)sprite->width * (size_t)sprite->height;

    for (i = 0; i < count; ++i) {
        value = sprite->pixels[i];

        if (value < 0)
            continue;
        if (value >= g_palette_count || !g_palette[value].used)
            return luaL_error(L,
                "draw.draw: palette index %d is not defined", value);
    }

    step = angle / 45;
    W = (double)sprite->width * (double)scale;
    H = (double)sprite->height * (double)scale;

    if (step == 0 || step == 4) {
        bw = W;
        bh = H;
    } else if (step == 2 || step == 6) {
        bw = H;
        bh = W;
    } else {
        bw = ceil((W + H) * k_diag);
        bh = bw;
    }

    xd = (double)x;
    yd = (double)y;

    if (!visible_range(xd, bw, g_draw.width, &sx0, &sx1) ||
        !visible_range(yd, bh, g_draw.height, &sy0, &sy1))
        return 0;

    half_w = W / 2.0;
    half_h = H / 2.0;
    half_bw = bw / 2.0;
    half_bh = bh / 2.0;
    c = k_cos[step];
    s = k_sin[step];

    for (sy = sy0; sy <= sy1; ++sy) {
        dy = (double)sy + 0.5 - yd - half_bh;

        for (sx = sx0; sx <= sx1; ++sx) {
            dx = (double)sx + 0.5 - xd - half_bw;
            src_cx = dx * c + dy * s;
            src_cy = -dx * s + dy * c;
            p = src_cx + half_w;
            q = src_cy + half_h;

            if (p < 0.0 || p >= W || q < 0.0 || q >= H)
                continue;

            col = (int)floor(p / scale + 1e-9);
            row = (int)floor(q / scale + 1e-9);
            if (col < 0 || col >= sprite->width ||
                row < 0 || row >= sprite->height)
                continue;

            value = sprite->pixels[(size_t)row * sprite->width + col];
            if (value < 0)
                continue;

            color = make_color(g_palette[value].r,
                               g_palette[value].g,
                               g_palette[value].b);
            g_draw.pixels[(size_t)sy * g_draw.width + sx] = color;
        }
    }

    mark_dirty_rect(sx0, sy0, sx1 - sx0 + 1, sy1 - sy0 + 1);
    return 0;
}

static int l_clear(lua_State *L)
{
    int r, g, b;
    uint32_t color;
    size_t count;
    size_t i;

    if (!ensure_open(L, "draw.clear"))
        return 0;

    if (read_color(L, 1, &r, &g, &b) < 0) {
        r = 0;
        g = 0;
        b = 0;
    }
    color = make_color(r, g, b);

    count = (size_t)g_draw.width * (size_t)g_draw.height;
    for (i = 0; i < count; ++i)
        g_draw.pixels[i] = color;

    mark_dirty_rect(0, 0, g_draw.width, g_draw.height);
    return 0;
}

static int l_pixel(lua_State *L)
{
    lua_Integer x, y;
    int r, g, b;

    if (!ensure_open(L, "draw.pixel"))
        return 0;

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    if (read_color(L, 3, &r, &g, &b) < 0)
        return luaL_error(L, "draw.pixel: expected a color");

    put_pixel((long long)x, (long long)y, make_color(r, g, b));
    return 0;
}

static int l_circle(lua_State *L)
{
    lua_Integer x, y, radius;
    int r, g, b;
    int fill;

    if (!ensure_open(L, "draw.circle"))
        return 0;

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    radius = luaL_checkinteger(L, 3);

    if (radius < 0)
        return luaL_error(L, "draw.circle: radius must be non-negative");
    if (radius > 3037000499LL)
        return luaL_error(L, "draw.circle: radius is too large");
    if (x < -1000000000LL || x > 1000000000LL ||
        y < -1000000000LL || y > 1000000000LL)
        return luaL_error(L, "draw.circle: coordinates are too large");

    if (read_color(L, 4, &r, &g, &b) < 0)
        return luaL_error(L, "draw.circle: expected a color");
    fill = read_fill(L, 4, "draw.circle");

    fill_circle((long long)x, (long long)y, (long long)radius,
                fill, make_color(r, g, b));
    return 0;
}

static int l_square(lua_State *L)
{
    lua_Integer x, y, w, h;
    int r, g, b;
    int fill;

    if (!ensure_open(L, "draw.square"))
        return 0;

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    w = luaL_checkinteger(L, 3);
    h = luaL_checkinteger(L, 4);

    if (x < -1000000000LL || x > 1000000000LL ||
        y < -1000000000LL || y > 1000000000LL ||
        w < -1000000000LL || w > 1000000000LL ||
        h < -1000000000LL || h > 1000000000LL)
        return luaL_error(L, "draw.square: coordinates are too large");

    if (read_color(L, 5, &r, &g, &b) < 0)
        return luaL_error(L, "draw.square: expected a color");
    fill = read_fill(L, 5, "draw.square");

    fill_rect((long long)x, (long long)y, (long long)w, (long long)h,
              fill, make_color(r, g, b));
    return 0;
}

static int l_triangle(lua_State *L)
{
    lua_Integer x1, y1, x2, y2, x3, y3;
    int r, g, b;
    int fill;

    if (!ensure_open(L, "draw.triangle"))
        return 0;

    x1 = luaL_checkinteger(L, 1);
    y1 = luaL_checkinteger(L, 2);
    x2 = luaL_checkinteger(L, 3);
    y2 = luaL_checkinteger(L, 4);
    x3 = luaL_checkinteger(L, 5);
    y3 = luaL_checkinteger(L, 6);

    if (x1 < -TRIANGLE_COORD_LIMIT || x1 > TRIANGLE_COORD_LIMIT ||
        y1 < -TRIANGLE_COORD_LIMIT || y1 > TRIANGLE_COORD_LIMIT ||
        x2 < -TRIANGLE_COORD_LIMIT || x2 > TRIANGLE_COORD_LIMIT ||
        y2 < -TRIANGLE_COORD_LIMIT || y2 > TRIANGLE_COORD_LIMIT ||
        x3 < -TRIANGLE_COORD_LIMIT || x3 > TRIANGLE_COORD_LIMIT ||
        y3 < -TRIANGLE_COORD_LIMIT || y3 > TRIANGLE_COORD_LIMIT)
        return luaL_error(L, "draw.triangle: coordinates are too large");

    if (read_color(L, 7, &r, &g, &b) < 0)
        return luaL_error(L, "draw.triangle: expected a color");
    fill = read_fill(L, 7, "draw.triangle");

    fill_triangle((long long)x1, (long long)y1,
                  (long long)x2, (long long)y2,
                  (long long)x3, (long long)y3,
                  fill, make_color(r, g, b));
    return 0;
}

static int l_text(lua_State *L)
{
    int top;
    long long x, y;
    const char *text;
    size_t len;
    int font;
    int r, g, b;
    int size;
    long long start_x;
    size_t i;
    uint32_t color;
    static const int sizes[4] = {4, 8, 16, 24};

    if (!ensure_open(L, "draw.text"))
        return 0;

    top = lua_gettop(L);
    if (top < 3)
        return luaL_error(L, "draw.text: expected at least x, y, text");

    x = (long long)luaL_checkinteger(L, 1);
    y = (long long)luaL_checkinteger(L, 2);
    text = luaL_checklstring(L, 3, &len);

    font = (int)luaL_optinteger(L, 4, 1);
    if (read_color(L, 5, &r, &g, &b) < 0) {
        r = 255;
        g = 255;
        b = 255;
    }

    if (font < 0)
        font = 0;
    if (font > 3)
        font = 3;

    size = sizes[font];
    color = make_color(r, g, b);
    start_x = x;

    for (i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        const unsigned char *glyph;
        int dx, dy;

        if (ch == '\n') {
            x = start_x;
            y += size + 1;
            continue;
        }
        if (ch == '\r')
            continue;
        if (ch < 32 || ch > 127)
            continue;

        glyph = font8x8[ch];

        for (dy = 0; dy < size; ++dy) {
            for (dx = 0; dx < size; ++dx) {
                if (glyph_pixel(glyph, dx, dy, size))
                    put_pixel(x + dx, y + dy, color);
            }
        }

        x += size + 1;
    }

    return 0;
}

static int l_close(lua_State *L)
{
    (void)L;
    destroy_resources();
    return 0;
}

static void free_sprites(void)
{
    int i;

    for (i = 0; i < SPRITE_MAX; ++i) {
        free(g_sprites[i].pixels);
        g_sprites[i].pixels = NULL;
        g_sprites[i].used = 0;
    }
}

static void auto_close(void)
{
    if (g_draw.initialized)
        destroy_resources();
    free_sprites();
    free_palette();
}

static const luaL_Reg draw_lib[] = {
    {"open",    l_open},
    {"pres",    l_pres},
    {"clear",   l_clear},
    {"pixel",   l_pixel},
    {"circle",  l_circle},
    {"square",  l_square},
    {"triangle", l_triangle},
    {"text",    l_text},
    {"palette", l_palette},
    {"sprite",  l_sprite},
    {"draw",    l_draw_sprite},
    {"close",   l_close},
    {NULL, NULL}
};

int luaopen_draw(lua_State *L)
{
    atexit(auto_close);

#if LUA_VERSION_NUM >= 502
    luaL_newlib(L, draw_lib);
#else
    lua_newtable(L);
    luaL_register(L, NULL, draw_lib);
#endif
    return 1;
}
