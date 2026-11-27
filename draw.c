/*
 * draw.c - SDL2 graphics for Lua.
 *
 * Build with CMake:
 *   cmake -S . -B build
 *   cmake --build build
 *
 * Or manually (Lua 5.4, Linux):
 *   gcc -O2 -Wall -Wextra -fPIC -shared -o draw.so draw.c \
 *       $(pkg-config --cflags --libs lua5.4 sdl2 SDL2_image) -lm
 */

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <SDL_image.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PALETTE_NAME_MAX 64
#define PALETTE_INDEX_MAX 1023
#define PALETTE_INITIAL_CAP 64
#define PALETTE_MAP_INITIAL 128
#define PALETTE_MAP_EMPTY (-1)
#define PALETTE_MAP_TOMB (-2)
#define SPRITE_MAX 1024
#define SPRITE_NAME_MAX 64
#define SPRITE_SCALE_MAX 1000000
#define MELODY_MAX 1024
#define MELODY_NAME_MAX 64
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_SAMPLE_COUNT 512
#define AUDIO_VOICES_MAX 8
#define WAV_MAX_BYTES (64 * 1024 * 1024)
#define MIDI_MAX_BYTES (16 * 1024 * 1024)
#define IMAGE_MAX_DIM 4096
#define IMAGE_MAX_PIXELS (4 * 1024 * 1024)
#define FONT_MAX 1024
#define FONT_NAME_MAX 64
#define FONT_CHARS_MAX 256
#define AUDIO_WAVE_SINE 0
#define AUDIO_WAVE_SQUARE 1
#define AUDIO_WAVE_TRIANGLE 2
#define AUDIO_WAVE_SAW 3
#define AUDIO_KIND_MELODY 0
#define AUDIO_KIND_WAV 1
#define ANIM_MAX 1024
#define ANIM_NAME_MAX 64
#define ANIM_FRAME_MAX 1024
#define TRIANGLE_COORD_LIMIT 500000000LL

typedef struct {
    int used;
    char name[PALETTE_NAME_MAX];
    int r, g, b, a;
} PaletteEntry;

typedef struct {
    int used;
    char name[SPRITE_NAME_MAX];
    int width;
    int height;
    int *pixels;             /* palette indices, -1 = transparent */
} SpriteEntry;

typedef struct {
    int rest;
    double freq;
    double beats;
} MelodyNote;

typedef struct {
    int used;
    char name[MELODY_NAME_MAX];
    int kind;
    int count;
    MelodyNote *notes;
    int16_t *samples;
    int sample_count;
    int sample_rate;
} MelodyEntry;

typedef struct {
    int playing;
    int kind;
    MelodyNote *notes;
    int count;
    int index;
    int note_sample;
    int note_samples;
    double phase;
    int16_t *samples;
    int sample_count;
    double sample_pos;
    int sample_rate;
    int volume;
    double speed;
    int wave;
} AudioVoice;

typedef struct {
    SDL_AudioDeviceID device;
    SDL_mutex *mutex;
    int sample_rate;
    AudioVoice voices[AUDIO_VOICES_MAX];
} AudioState;

typedef struct {
    int used;
    char name[ANIM_NAME_MAX];
    int count;
    char **frames;
    double fps;
    Uint32 started;
} AnimEntry;

typedef struct {
    int used;
    char name[FONT_NAME_MAX];
    int first;
    int count;
    int cell_w;
    int cell_h;
    int stride;
    int scale;
    unsigned char *masks;
} FontEntry;

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
    Uint64 prev_frame;
    int has_prev_frame;
    double frame_delta;
    int alpha;
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
static AnimEntry g_anims[ANIM_MAX];
static FontEntry g_fonts[FONT_MAX];
static MelodyEntry g_melodies[MELODY_MAX];
static AudioState g_audio;

static void audio_update_finished(void);

typedef struct {
    int down[SDL_NUM_SCANCODES];
    int pressed[SDL_NUM_SCANCODES];
    int last_key;
    int mouse_down[3];
    int mouse_pressed[3];
    int mouse_button;
    int mouse_x;
    int mouse_y;
    int mouse_dx;
    int mouse_dy;
    int frame_mouse_dx;
    int frame_mouse_dy;
    int wheel_dx;
    int wheel_dy;
    int frame_wheel_dx;
    int frame_wheel_dy;
} InputState;

typedef struct {
    const char *name;
    SDL_Scancode code;
} KeyName;

static InputState g_input;

static const KeyName g_key_names[] = {
    {"a", SDL_SCANCODE_A}, {"b", SDL_SCANCODE_B}, {"c", SDL_SCANCODE_C},
    {"d", SDL_SCANCODE_D}, {"e", SDL_SCANCODE_E}, {"f", SDL_SCANCODE_F},
    {"g", SDL_SCANCODE_G}, {"h", SDL_SCANCODE_H}, {"i", SDL_SCANCODE_I},
    {"j", SDL_SCANCODE_J}, {"k", SDL_SCANCODE_K}, {"l", SDL_SCANCODE_L},
    {"m", SDL_SCANCODE_M}, {"n", SDL_SCANCODE_N}, {"o", SDL_SCANCODE_O},
    {"p", SDL_SCANCODE_P}, {"q", SDL_SCANCODE_Q}, {"r", SDL_SCANCODE_R},
    {"s", SDL_SCANCODE_S}, {"t", SDL_SCANCODE_T}, {"u", SDL_SCANCODE_U},
    {"v", SDL_SCANCODE_V}, {"w", SDL_SCANCODE_W}, {"x", SDL_SCANCODE_X},
    {"y", SDL_SCANCODE_Y}, {"z", SDL_SCANCODE_Z},
    {"0", SDL_SCANCODE_0}, {"1", SDL_SCANCODE_1}, {"2", SDL_SCANCODE_2},
    {"3", SDL_SCANCODE_3}, {"4", SDL_SCANCODE_4}, {"5", SDL_SCANCODE_5},
    {"6", SDL_SCANCODE_6}, {"7", SDL_SCANCODE_7}, {"8", SDL_SCANCODE_8},
    {"9", SDL_SCANCODE_9},
    {"left", SDL_SCANCODE_LEFT}, {"right", SDL_SCANCODE_RIGHT},
    {"up", SDL_SCANCODE_UP}, {"down", SDL_SCANCODE_DOWN},
    {"space", SDL_SCANCODE_SPACE}, {"enter", SDL_SCANCODE_RETURN},
    {"escape", SDL_SCANCODE_ESCAPE}, {"esc", SDL_SCANCODE_ESCAPE},
    {"tab", SDL_SCANCODE_TAB}, {"backspace", SDL_SCANCODE_BACKSPACE},
    {"delete", SDL_SCANCODE_DELETE},
    {"shift", SDL_SCANCODE_LSHIFT}, {"rshift", SDL_SCANCODE_RSHIFT},
    {"ctrl", SDL_SCANCODE_LCTRL}, {"rctrl", SDL_SCANCODE_RCTRL},
    {"alt", SDL_SCANCODE_LALT}, {"ralt", SDL_SCANCODE_RALT},
    {"f1", SDL_SCANCODE_F1}, {"f2", SDL_SCANCODE_F2},
    {"f3", SDL_SCANCODE_F3}, {"f4", SDL_SCANCODE_F4},
    {"f5", SDL_SCANCODE_F5}, {"f6", SDL_SCANCODE_F6},
    {"f7", SDL_SCANCODE_F7}, {"f8", SDL_SCANCODE_F8},
    {"f9", SDL_SCANCODE_F9}, {"f10", SDL_SCANCODE_F10},
    {"f11", SDL_SCANCODE_F11}, {"f12", SDL_SCANCODE_F12}
};

static const size_t g_key_name_count =
    sizeof(g_key_names) / sizeof(g_key_names[0]);

static const char *g_mouse_button_names[3] = {"left", "middle", "right"};

#define GAMEPAD_SLOTS 4
#define GAMEPAD_NAME_MAX 128

typedef struct {
    SDL_GameController *controller;
    SDL_JoystickID instance;
    char name[GAMEPAD_NAME_MAX];
    int connected;
    int down[SDL_CONTROLLER_BUTTON_MAX];
    int pressed[SDL_CONTROLLER_BUTTON_MAX];
    int last_button;
    double axis[SDL_CONTROLLER_AXIS_MAX];
} GamepadState;

typedef struct {
    const char *name;
    SDL_GameControllerButton button;
} GamepadButtonName;

typedef struct {
    const char *name;
    SDL_GameControllerAxis axis;
} GamepadAxisName;

static GamepadState g_gamepads[GAMEPAD_SLOTS];
static int g_gamecontroller_owned;

static const GamepadButtonName g_gamepad_buttons[] = {
    {"a", SDL_CONTROLLER_BUTTON_A},
    {"b", SDL_CONTROLLER_BUTTON_B},
    {"x", SDL_CONTROLLER_BUTTON_X},
    {"y", SDL_CONTROLLER_BUTTON_Y},
    {"back", SDL_CONTROLLER_BUTTON_BACK},
    {"guide", SDL_CONTROLLER_BUTTON_GUIDE},
    {"start", SDL_CONTROLLER_BUTTON_START},
    {"leftstick", SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {"rightstick", SDL_CONTROLLER_BUTTON_RIGHTSTICK},
    {"leftshoulder", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"lb", SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {"rightshoulder", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"rb", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {"dpadup", SDL_CONTROLLER_BUTTON_DPAD_UP},
    {"dpaddown", SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {"dpadleft", SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {"dpadright", SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {"share", SDL_CONTROLLER_BUTTON_MISC1},
    {"misc1", SDL_CONTROLLER_BUTTON_MISC1},
    {"paddle1", SDL_CONTROLLER_BUTTON_PADDLE1},
    {"paddle2", SDL_CONTROLLER_BUTTON_PADDLE2},
    {"paddle3", SDL_CONTROLLER_BUTTON_PADDLE3},
    {"paddle4", SDL_CONTROLLER_BUTTON_PADDLE4},
    {"touchpad", SDL_CONTROLLER_BUTTON_TOUCHPAD}
};

static const size_t g_gamepad_button_count =
    sizeof(g_gamepad_buttons) / sizeof(g_gamepad_buttons[0]);

static const GamepadAxisName g_gamepad_axes[] = {
    {"leftx", SDL_CONTROLLER_AXIS_LEFTX},
    {"lx", SDL_CONTROLLER_AXIS_LEFTX},
    {"lefty", SDL_CONTROLLER_AXIS_LEFTY},
    {"ly", SDL_CONTROLLER_AXIS_LEFTY},
    {"rightx", SDL_CONTROLLER_AXIS_RIGHTX},
    {"rx", SDL_CONTROLLER_AXIS_RIGHTX},
    {"righty", SDL_CONTROLLER_AXIS_RIGHTY},
    {"ry", SDL_CONTROLLER_AXIS_RIGHTY},
    {"triggerleft", SDL_CONTROLLER_AXIS_TRIGGERLEFT},
    {"lt", SDL_CONTROLLER_AXIS_TRIGGERLEFT},
    {"triggerright", SDL_CONTROLLER_AXIS_TRIGGERRIGHT},
    {"rt", SDL_CONTROLLER_AXIS_TRIGGERRIGHT}
};

static const size_t g_gamepad_axis_count =
    sizeof(g_gamepad_axes) / sizeof(g_gamepad_axes[0]);

static void gamepad_reset_states(void);
static void gamepad_shutdown(void);
static void gamepad_init(void);
static void gamepad_handle_event(const SDL_Event *e);

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

static int clamp_color_number(lua_Number v)
{
    if (!isfinite(v) || v <= 0.0)
        return 0;
    if (v >= 255.0)
        return 255;
    return (int)(v + 0.5);
}

static int clamp_alpha_number(lua_Number v)
{
    if (!isfinite(v) || v <= 0.0)
        return 0;
    if (v <= 1.0)
        return (int)(v * 255.0 + 0.5);
    if (v >= 255.0)
        return 255;
    return (int)(v + 0.5);
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

    if (g_palette_count >= PALETTE_INDEX_MAX + 1)
        return -1;
    if (palette_grow(g_palette_count + 1) != 0)
        return -1;

    i = g_palette_count++;
    g_palette_free_hint = 0;
    return i;
}

static int auto_palette_add(const char *prefix, int n, int r, int g, int b,
                            int a)
{
    char name[PALETTE_NAME_MAX];
    size_t len;
    int suffix;
    int index;
    int written;

    written = snprintf(name, sizeof(name), "%s_c%d", prefix, n);
    if (written < 0 || (size_t)written >= sizeof(name))
        return -1;
    for (suffix = 2; find_palette_name(name) >= 0; ++suffix) {
        written = snprintf(name, sizeof(name), "%s_c%d_%d", prefix, n,
                           suffix);
        if (written < 0 || (size_t)written >= sizeof(name) || suffix > 10000)
            return -1;
    }

    index = find_free_palette();
    if (index < 0)
        return -1;
    len = strlen(name);
    memcpy(g_palette[index].name, name, len + 1);
    g_palette[index].used = 1;
    if (palette_map_set_name(name, index) != 0) {
        g_palette[index].used = 0;
        g_palette[index].name[0] = '\0';
        return -1;
    }
    g_palette[index].r = clamp_color(r);
    g_palette[index].g = clamp_color(g);
    g_palette[index].b = clamp_color(b);
    g_palette[index].a = clamp_color(a);
    return index;
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

static int read_color(lua_State *L, int first, int *r, int *g, int *b, int *a)
{
    int i;
    int values[4];

    *r = 255;
    *g = 255;
    *b = 255;
    *a = 255;
    if (lua_gettop(L) < first)
        return -1;

    if (lua_type(L, first) == LUA_TTABLE) {
        for (i = 0; i < 3; ++i) {
            lua_pushinteger(L, i + 1);
            lua_rawget(L, first);
            if (!lua_isnumber(L, -1))
                return luaL_error(L,
                    "color must be a palette name or {r, g, b, [a]}");
            values[i] = clamp_color_number(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_pushinteger(L, 4);
        lua_rawget(L, first);
        if (lua_isnil(L, -1))
            values[3] = 255;
        else if (lua_isnumber(L, -1))
            values[3] = clamp_alpha_number(lua_tonumber(L, -1));
        else
            return luaL_error(L,
                "color must be a palette name or {r, g, b, [a]}");
        lua_pop(L, 1);
        *r = values[0];
        *g = values[1];
        *b = values[2];
        *a = values[3];
        return 0;
    }

    if (lua_type(L, first) == LUA_TSTRING) {
        const char *name = lua_tostring(L, first);
        int index = find_palette_name(name);

        if (index < 0)
            return luaL_error(L, "palette color '%s' is not defined", name);

        *r = g_palette[index].r;
        *g = g_palette[index].g;
        *b = g_palette[index].b;
        *a = g_palette[index].a;
        return 0;
    }

    return luaL_error(L,
        "color must be a palette name or {r, g, b, [a]}");
}

static int read_fill(lua_State *L, int color_first, const char *name)
{
    lua_Integer value;

    if (lua_gettop(L) < color_first + 1)
        return 0;

    value = luaL_optinteger(L, color_first + 1, 0);
    if (value < -1)
        return luaL_error(L, "%s: fill must be -1, 0, or positive", name);
    if (value > INT_MAX)
        return luaL_error(L, "%s: fill is too large", name);

    return (int)value;
}

static uint32_t make_color(int r, int g, int b, int a)
{
    int alpha = (clamp_color(a) * g_draw.alpha) / 255;

    return ((uint32_t)alpha << 24) |
           ((uint32_t)r << 16) |
           ((uint32_t)g << 8) |
           (uint32_t)b;
}

static uint32_t blend_color(uint32_t dst, uint32_t src)
{
    unsigned sa = (src >> 24) & 0xFF;
    unsigned da;
    unsigned out_a;
    unsigned out_r, out_g, out_b;

    if (sa == 255)
        return src;
    if (sa == 0)
        return dst;

    da = (dst >> 24) & 0xFF;
    if (da == 0)
        return src;

    out_a = sa + (da * (255u - sa)) / 255u;
    if (out_a == 0)
        return 0;

    out_r = (((src >> 16) & 0xFF) * sa +
             (((dst >> 16) & 0xFF) * da * (255u - sa)) / 255u +
             out_a / 2u) / out_a;
    out_g = (((src >> 8) & 0xFF) * sa +
             (((dst >> 8) & 0xFF) * da * (255u - sa)) / 255u +
             out_a / 2u) / out_a;
    out_b = ((src & 0xFF) * sa +
             ((dst & 0xFF) * da * (255u - sa)) / 255u +
             out_a / 2u) / out_a;

    return (out_a << 24) |
           ((out_r & 0xFF) << 16) |
           ((out_g & 0xFF) << 8) |
           (out_b & 0xFF);
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

static int find_anim_name(const char *name)
{
    int i;

    for (i = 0; i < ANIM_MAX; ++i) {
        if (g_anims[i].used && strcmp(g_anims[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int first_free_anim(void)
{
    int i;

    for (i = 0; i < ANIM_MAX; ++i) {
        if (!g_anims[i].used)
            return i;
    }
    return -1;
}

static void free_anim(AnimEntry *anim)
{
    int i;

    for (i = 0; i < anim->count; ++i)
        free(anim->frames[i]);
    free(anim->frames);
    anim->frames = NULL;
    anim->count = 0;
}

static void free_anims(void)
{
    int i;

    for (i = 0; i < ANIM_MAX; ++i) {
        free_anim(&g_anims[i]);
        g_anims[i].used = 0;
        g_anims[i].name[0] = '\0';
    }
}

static int find_font_name(const char *name)
{
    int i;

    for (i = 0; i < FONT_MAX; ++i) {
        if (g_fonts[i].used && strcmp(g_fonts[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int first_free_font(void)
{
    int i;

    for (i = 0; i < FONT_MAX; ++i) {
        if (!g_fonts[i].used)
            return i;
    }
    return -1;
}

static void free_font(FontEntry *font)
{
    free(font->masks);
    memset(font, 0, sizeof(*font));
}

static void free_fonts(void)
{
    int i;

    for (i = 0; i < FONT_MAX; ++i)
        free_font(&g_fonts[i]);
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

static int find_melody_name(const char *name)
{
    int i;

    for (i = 0; i < MELODY_MAX; ++i) {
        if (g_melodies[i].used &&
            strcmp(g_melodies[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int first_free_melody(void)
{
    int i;

    for (i = 0; i < MELODY_MAX; ++i) {
        if (!g_melodies[i].used)
            return i;
    }
    return -1;
}

static void free_melody(MelodyEntry *melody)
{
    free(melody->notes);
    free(melody->samples);
    melody->notes = NULL;
    melody->samples = NULL;
    melody->count = 0;
    melody->sample_count = 0;
    melody->sample_rate = 0;
    melody->kind = AUDIO_KIND_MELODY;
}

static void free_melodies(void)
{
    int i;

    for (i = 0; i < MELODY_MAX; ++i) {
        free_melody(&g_melodies[i]);
        g_melodies[i].used = 0;
        g_melodies[i].name[0] = '\0';
    }
}

static double note_name_to_freq(const char *s)
{
    static const int semitones[7] = {0, 2, 4, 5, 7, 9, 11};
    const char *p = s;
    char letter;
    int semi;
    int octave = 4;
    int midi;

    if (!s || !*s)
        return -1.0;

    letter = (char)tolower((unsigned char)*p++);
    if (letter < 'a' || letter > 'g')
        return -1.0;
    semi = semitones[letter - 'a'];

    if (*p == '#' || *p == 'b') {
        if (*p == '#')
            semi += 1;
        else
            semi -= 1;
        ++p;
    }

    if (*p) {
        char *end;
        long raw = strtol(p, &end, 10);

        if (end == p || *end != '\0' || raw < 0 || raw > 8)
            return -1.0;
        octave = (int)raw;
    }

    midi = (octave + 1) * 12 + semi;
    return 440.0 * pow(2.0, (midi - 69) / 12.0);
}

static int parse_note_scalar(lua_State *L, int index, MelodyNote *note)
{
    const char *s;
    double n;
    double freq;

    note->rest = 0;
    note->freq = 0.0;
    note->beats = 1.0;

    if (lua_isnil(L, index))
        return -1;

    if (lua_type(L, index) == LUA_TNUMBER) {
        n = luaL_checknumber(L, index);
        if (n == 0.0) {
            note->rest = 1;
            return 0;
        }
        if (n < 0.0 || n > 20000.0)
            return -1;
        note->freq = n;
        return 0;
    }

    if (lua_type(L, index) == LUA_TSTRING) {
        s = lua_tostring(L, index);
        if (strcmp(s, "-") == 0 || strcmp(s, "r") == 0 ||
            strcmp(s, "R") == 0 || strcmp(s, "rest") == 0 ||
            strcmp(s, "REST") == 0) {
            note->rest = 1;
            return 0;
        }
        freq = note_name_to_freq(s);
        if (freq < 0.0)
            return -1;
        note->freq = freq;
        return 0;
    }

    return -1;
}

static double parse_beats(lua_State *L, int index)
{
    double n;

    if (lua_isnoneornil(L, index))
        return 1.0;
    if (lua_type(L, index) != LUA_TNUMBER)
        return -1.0;

    n = luaL_checknumber(L, index);
    if (n <= 0.0 || n > 64.0)
        return -1.0;
    return n;
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

static int parse_angle_named(lua_State *L, int index, const char *name)
{
    lua_Number n;
    lua_Integer angle;

    if (lua_isnoneornil(L, index))
        return 0;

    n = luaL_checknumber(L, index);
    if (!isfinite(n) || n < -1000000.0 || n > 1000000.0)
        return luaL_error(L, "%s: angle is out of range", name);

    angle = (lua_Integer)n;
    if ((lua_Number)angle != n)
        return luaL_error(L, "%s: angle must be an integer", name);

    angle %= 360;
    if (angle < 0)
        angle += 360;

    return (int)angle;
}

static int parse_angle(lua_State *L, int index)
{
    return parse_angle_named(L, index, "draw.draw");
}

static int parse_flip(lua_State *L, int index, const char *option)
{
    if (lua_isnoneornil(L, index))
        return 0;
    if (!lua_isboolean(L, index))
        return luaL_error(L, "draw.draw: %s must be true or false", option);
    return lua_toboolean(L, index);
}

static int read_flip_field(lua_State *L, int index, int field,
                           const char *option)
{
    int value;

    if (lua_isnoneornil(L, index))
        return 0;
    if (lua_type(L, index) != LUA_TTABLE)
        return luaL_error(L, "draw.draw: %s must be a table", option);

    lua_pushinteger(L, field);
    lua_rawget(L, index);
    value = parse_flip(L, lua_gettop(L), option);
    lua_pop(L, 1);
    return value;
}

static double read_pivot_field(lua_State *L, int index, int field,
                               double default_value)
{
    double value;

    lua_pushinteger(L, field);
    lua_rawget(L, index);
    value = luaL_optnumber(L, lua_gettop(L), (lua_Number)default_value);
    lua_pop(L, 1);
    return value;
}

static int parse_tint(lua_State *L, int index, int *r, int *g, int *b, int *a)
{
    int i;
    int values[4];
    const char *name;

    *r = 255;
    *g = 255;
    *b = 255;
    *a = 255;

    if (lua_isnoneornil(L, index))
        return 0;

    if (lua_istable(L, index)) {
        for (i = 0; i < 3; ++i) {
            lua_pushinteger(L, i + 1);
            lua_rawget(L, index);
            if (!lua_isnumber(L, -1))
                return luaL_error(L, "draw.draw: tint must be {r, g, b, [a]}");
            values[i] = clamp_color_number(lua_tonumber(L, -1));
            lua_pop(L, 1);
        }
        lua_pushinteger(L, 4);
        lua_rawget(L, index);
        if (lua_isnil(L, -1))
            values[3] = 255;
        else if (lua_isnumber(L, -1))
            values[3] = clamp_alpha_number(lua_tonumber(L, -1));
        else
            return luaL_error(L, "draw.draw: tint must be {r, g, b, [a]}");
        lua_pop(L, 1);
        *r = values[0];
        *g = values[1];
        *b = values[2];
        *a = values[3];
        return 0;
    }

    if (lua_type(L, index) == LUA_TSTRING) {
        name = lua_tostring(L, index);
        i = find_palette_name(name);
        if (i < 0)
            return luaL_error(L,
                "draw.draw: tint palette color '%s' is not defined", name);
        *r = g_palette[i].r;
        *g = g_palette[i].g;
        *b = g_palette[i].b;
        *a = g_palette[i].a;
        return 0;
    }

    return luaL_error(L,
        "draw.draw: tint must be a palette name or {r, g, b, [a]}");
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
        row[x] = blend_color(row[x], color);

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

static void fill_rotated_rect(double x, double y, double w, double h,
                              int angle, int fill, uint32_t color)
{
    double cx, cy, hw, hh, rad, c, s;
    double minx, maxx, miny, maxy;
    long long y0, y1, row;
    int t;

    if (w <= 0.0 || h <= 0.0)
        return;

    cx = x + w / 2.0;
    cy = y + h / 2.0;
    hw = w / 2.0;
    hh = h / 2.0;
    rad = (double)angle * 0.017453292519943295769236907684886;
    c = cos(rad);
    s = sin(rad);
    if (fabs(c) < 1e-12)
        c = 0.0;
    if (fabs(s) < 1e-12)
        s = 0.0;

    minx = maxx = -hw * c - (-hh) * s;
    miny = maxy = -hw * s + (-hh) * c;
    {
        double lx, ly, rx, ry;
        int k;

        for (k = 0; k < 4; ++k) {
            lx = (k & 1) ? hw : -hw;
            ly = (k & 2) ? hh : -hh;
            rx = lx * c - ly * s;
            ry = lx * s + ly * c;
            if (k == 0) {
                minx = maxx = rx;
                miny = maxy = ry;
            } else {
                if (rx < minx) minx = rx;
                if (rx > maxx) maxx = rx;
                if (ry < miny) miny = ry;
                if (ry > maxy) maxy = ry;
            }
        }
    }

    y0 = (long long)floor(cy + miny) - 1;
    y1 = (long long)ceil(cy + maxy) + 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= g_draw.height) y1 = g_draw.height - 1;
    if (y0 > y1)
        return;

    t = 1;
    if (fill != 0) {
        t = fill < 0 ? 1 : fill;
        if ((double)t > (w + 1.0) / 2.0)
            t = (int)((w + 1.0) / 2.0);
        if ((double)t > (h + 1.0) / 2.0)
            t = (int)((h + 1.0) / 2.0);
        if (t <= 0)
            return;
    }

    for (row = y0; row <= y1; ++row) {
        double dy = (double)row + 0.5 - cy;
        double lo, hi, a, b;
        long long x0, x1, col;

        if (fabs(c) > 1e-12) {
            a = (-hw - dy * s) / c;
            b = (hw - dy * s) / c;
            lo = a < b ? a : b;
            hi = a < b ? b : a;
        } else {
            if (fabs(dy * s) > hw)
                continue;
            lo = -1e100;
            hi = 1e100;
        }

        if (fabs(s) > 1e-12) {
            double a0 = (dy * c - hh) / s;
            double b0 = (dy * c + hh) / s;

            a = a0 < b0 ? a0 : b0;
            b = a0 < b0 ? b0 : a0;
            if (a > lo) lo = a;
            if (b < hi) hi = b;
        } else {
            if (fabs(dy * c) > hh)
                continue;
        }
        if (lo > hi)
            continue;

        x0 = (long long)ceil(cx + lo - 0.5 - 1e-9);
        x1 = (long long)floor(cx + hi - 0.5 + 1e-9);
        if (x0 < 0) x0 = 0;
        if (x1 >= g_draw.width) x1 = g_draw.width - 1;
        if (x0 > x1)
            continue;

        if (fill == 0) {
            fill_span(x0, x1, row, color);
            continue;
        }

        for (col = x0; col <= x1; ++col) {
            double dx = (double)col + 0.5 - cx;
            double lx = dx * c + dy * s;
            double ly = -dx * s + dy * c;

            if (hw - lx <= (double)t || hw + lx <= (double)t ||
                hh - ly <= (double)t || hh + ly <= (double)t)
                fill_span(col, col, row, color);
        }
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

static void draw_line(long long x1, long long y1,
                      long long x2, long long y2,
                      int width, uint32_t color)
{
    typedef __int128 line_i128;

    long long dx = x2 - x1;
    long long dy = y2 - y1;
    long long len2 = dx * dx + dy * dy;
    long long len = isqrt_ll(len2);
    long long radius = width / 2 + 1;
    line_i128 cross_limit = (line_i128)width * len;
    line_i128 dot_limit = (line_i128)len2;
    line_i128 end_limit = (line_i128)width * width;

    if (len2 == 0) {
        fill_rect(x1 - width / 2, y1 - width / 2,
                  width, width, 0, color);
        return;
    }

    long long minx, maxx, miny, maxy;
    long long x, y;

    minx = x1 < x2 ? x1 : x2;
    maxx = x1 > x2 ? x1 : x2;
    miny = y1 < y2 ? y1 : y2;
    maxy = y1 > y2 ? y1 : y2;

    minx -= radius;
    maxx += radius;
    miny -= radius;
    maxy += radius;

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
        long long start = -1;

        for (x = minx; x <= maxx; ++x) {
            line_i128 vx = (line_i128)x - x1;
            line_i128 vy = (line_i128)y - y1;
            line_i128 wx = (line_i128)x - x2;
            line_i128 wy = (line_i128)y - y2;
            line_i128 cross = (line_i128)dx * vy - (line_i128)dy * vx;
            line_i128 dot = (line_i128)dx * vx + (line_i128)dy * vy;
            line_i128 dist_a = vx * vx + vy * vy;
            line_i128 dist_b = wx * wx + wy * wy;
            int hit;

            if (cross < 0)
                cross = -cross;

            hit = (dot >= 0 && dot <= dot_limit &&
                   2 * cross <= cross_limit) ||
                  4 * dist_a <= end_limit ||
                  4 * dist_b <= end_limit;

            if (hit) {
                if (start < 0)
                    start = x;
            } else if (start >= 0) {
                fill_span(start, x - 1, y, color);
                start = -1;
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

static void reset_input(void)
{
    memset(&g_input, 0, sizeof(g_input));
    g_input.last_key = -1;
    g_input.mouse_button = -1;
    gamepad_reset_states();
}

static SDL_Scancode find_key_code(lua_State *L, int arg)
{
    const char *name = luaL_checkstring(L, arg);
    size_t i;

    for (i = 0; i < g_key_name_count; ++i) {
        if (strcmp(g_key_names[i].name, name) == 0)
            return g_key_names[i].code;
    }

    return (SDL_Scancode)luaL_error(L, "unknown key name '%s'", name);
}

static const char *key_name_for_code(int code)
{
    size_t i;

    for (i = 0; i < g_key_name_count; ++i) {
        if ((int)g_key_names[i].code == code)
            return g_key_names[i].name;
    }

    return NULL;
}

static int find_mouse_button(lua_State *L, int arg)
{
    const char *name = luaL_checkstring(L, arg);
    int i;

    for (i = 0; i < 3; ++i) {
        if (strcmp(g_mouse_button_names[i], name) == 0)
            return i;
    }

    return (int)luaL_error(L, "unknown mouse button '%s'", name);
}

static int event_button_index(Uint8 button)
{
    if (button == SDL_BUTTON_LEFT)
        return 0;
    if (button == SDL_BUTTON_MIDDLE)
        return 1;
    if (button == SDL_BUTTON_RIGHT)
        return 2;
    return -1;
}

static void gamepad_clear(GamepadState *pad)
{
    if (!pad)
        return;
    if (pad->controller) {
        SDL_GameControllerClose(pad->controller);
        pad->controller = NULL;
    }
    memset(pad, 0, sizeof(*pad));
    pad->instance = -1;
    pad->last_button = -1;
}

static void gamepad_reset_states(void)
{
    int i;

    for (i = 0; i < GAMEPAD_SLOTS; ++i) {
        memset(g_gamepads[i].down, 0, sizeof(g_gamepads[i].down));
        memset(g_gamepads[i].pressed, 0, sizeof(g_gamepads[i].pressed));
        memset(g_gamepads[i].axis, 0, sizeof(g_gamepads[i].axis));
        g_gamepads[i].last_button = -1;
    }
}

static int gamepad_slot(lua_State *L, int arg, const char *cmd)
{
    lua_Integer index = luaL_optinteger(L, arg, 1);

    if (index < 1 || index > GAMEPAD_SLOTS)
        return (int)luaL_error(L, "draw.%s: index must be 1..%d",
                               cmd, GAMEPAD_SLOTS);
    return (int)index - 1;
}

static int gamepad_find_button(lua_State *L, int arg, const char *cmd)
{
    const char *name = luaL_checkstring(L, arg);
    size_t i;

    for (i = 0; i < g_gamepad_button_count; ++i) {
        if (strcmp(g_gamepad_buttons[i].name, name) == 0)
            return (int)g_gamepad_buttons[i].button;
    }
    return (int)luaL_error(L, "draw.%s: unknown button '%s'", cmd, name);
}

static int gamepad_find_axis(lua_State *L, int arg, const char *cmd)
{
    const char *name = luaL_checkstring(L, arg);
    size_t i;

    for (i = 0; i < g_gamepad_axis_count; ++i) {
        if (strcmp(g_gamepad_axes[i].name, name) == 0)
            return (int)g_gamepad_axes[i].axis;
    }
    return (int)luaL_error(L, "draw.%s: unknown stick '%s'", cmd, name);
}

static int gamepad_slot_by_instance(SDL_JoystickID instance)
{
    int i;

    for (i = 0; i < GAMEPAD_SLOTS; ++i) {
        if (!g_gamepads[i].connected || !g_gamepads[i].controller)
            continue;
        if (SDL_JoystickInstanceID(
                SDL_GameControllerGetJoystick(g_gamepads[i].controller)) == instance)
            return i;
    }
    return -1;
}

static void gamepad_open_device(int device_index)
{
    SDL_GameController *controller;
    SDL_JoystickID instance;
    const char *name;
    int duplicate;
    int slot;
    int i;

    if (device_index < 0 || !SDL_IsGameController(device_index))
        return;
    controller = SDL_GameControllerOpen(device_index);
    if (!controller)
        return;
    instance = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controller));
    if (instance < 0) {
        SDL_GameControllerClose(controller);
        return;
    }

    duplicate = gamepad_slot_by_instance(instance);
    if (duplicate >= 0) {
        SDL_GameControllerClose(controller);
        return;
    }

    slot = -1;
    for (i = 0; i < GAMEPAD_SLOTS; ++i) {
        if (!g_gamepads[i].connected) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        SDL_GameControllerClose(controller);
        return;
    }

    gamepad_clear(&g_gamepads[slot]);
    g_gamepads[slot].controller = controller;
    g_gamepads[slot].instance = instance;
    g_gamepads[slot].connected = 1;
    g_gamepads[slot].last_button = -1;
    name = SDL_GameControllerName(controller);
    if (name)
        snprintf(g_gamepads[slot].name, GAMEPAD_NAME_MAX, "%s", name);
}

static void gamepad_remove_instance(SDL_JoystickID instance)
{
    int slot = gamepad_slot_by_instance(instance);

    if (slot >= 0)
        gamepad_clear(&g_gamepads[slot]);
}

static double gamepad_normalize_axis(Sint16 value, int trigger)
{
    double normalized;

    if (trigger) {
        if (value <= 0)
            return 0.0;
        normalized = (double)value / 32767.0;
        return normalized < 0.05 ? 0.0 : normalized;
    }

    normalized = (double)value / (value < 0 ? 32768.0 : 32767.0);
    return fabs(normalized) < 0.15 ? 0.0 : normalized;
}

static void gamepad_set_button(GamepadState *pad, int button, int down)
{
    if (!pad || button < 0 || button >= SDL_CONTROLLER_BUTTON_MAX)
        return;
    if (down) {
        pad->down[button] = 1;
        pad->pressed[button] = 1;
        pad->last_button = button;
    } else {
        pad->down[button] = 0;
    }
}

static void gamepad_scan_connected(void)
{
    int count = SDL_NumJoysticks();
    int i;

    for (i = 0; i < count; ++i)
        gamepad_open_device(i);
}

static void gamepad_init(void)
{
    if (!SDL_WasInit(SDL_INIT_GAMECONTROLLER)) {
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
            return;
        g_gamecontroller_owned = 1;
    }
    SDL_GameControllerEventState(SDL_ENABLE);
    gamepad_scan_connected();
}

static void gamepad_shutdown(void)
{
    int i;

    for (i = 0; i < GAMEPAD_SLOTS; ++i)
        gamepad_clear(&g_gamepads[i]);
    if (g_gamecontroller_owned) {
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        g_gamecontroller_owned = 0;
    }
}

static void gamepad_handle_event(const SDL_Event *e)
{
    int slot;
    int index;

    if (e->type == SDL_CONTROLLERDEVICEADDED) {
        gamepad_open_device((int)e->cdevice.which);
        return;
    }
    if (e->type == SDL_CONTROLLERDEVICEREMOVED) {
        gamepad_remove_instance(e->cdevice.which);
        return;
    }
    if (e->type == SDL_CONTROLLERBUTTONDOWN || e->type == SDL_CONTROLLERBUTTONUP) {
        slot = gamepad_slot_by_instance(e->cbutton.which);
        if (slot >= 0)
            gamepad_set_button(&g_gamepads[slot], (int)e->cbutton.button,
                               e->type == SDL_CONTROLLERBUTTONDOWN);
        return;
    }
    if (e->type == SDL_CONTROLLERAXISMOTION) {
        slot = gamepad_slot_by_instance(e->caxis.which);
        index = (int)e->caxis.axis;
        if (slot >= 0 && index >= 0 && index < SDL_CONTROLLER_AXIS_MAX) {
            g_gamepads[slot].axis[index] = gamepad_normalize_axis(
                e->caxis.value,
                index == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
                index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        }
    }
}

static void event_position_to_logical(int win_x, int win_y,
                                      int *out_x, int *out_y)
{
#if SDL_VERSION_ATLEAST(2, 0, 18)
    float logical_x, logical_y;

    if (g_draw.renderer) {
        SDL_RenderWindowToLogical(g_draw.renderer, win_x, win_y,
                                  &logical_x, &logical_y);
        *out_x = (int)logical_x;
        *out_y = (int)logical_y;
        return;
    }
#endif

    {
        int win_w = 1;
        int win_h = 1;

        SDL_GetWindowSize(g_draw.window, &win_w, &win_h);
        if (win_w <= 0)
            win_w = 1;
        if (win_h <= 0)
            win_h = 1;

        *out_x = (int)((long long)win_x * g_draw.width / win_w);
        *out_y = (int)((long long)win_y * g_draw.height / win_h);
    }
}

static void event_delta_to_logical(int win_dx, int win_dy,
                                   int *out_dx, int *out_dy)
{
    int win_w = 1;
    int win_h = 1;

    SDL_GetWindowSize(g_draw.window, &win_w, &win_h);
    if (win_w <= 0)
        win_w = 1;
    if (win_h <= 0)
        win_h = 1;

    *out_dx = (int)((long long)win_dx * g_draw.width / win_w);
    *out_dy = (int)((long long)win_dy * g_draw.height / win_h);
}

static void update_frame_delta(void)
{
    Uint64 now = SDL_GetPerformanceCounter();
    Uint64 freq = SDL_GetPerformanceFrequency();

    if (g_draw.has_prev_frame && freq != 0 && now >= g_draw.prev_frame)
        g_draw.frame_delta =
            (double)(now - g_draw.prev_frame) / (double)freq;
    else
        g_draw.frame_delta = 0.0;

    g_draw.prev_frame = now;
    g_draw.has_prev_frame = 1;
}

static void snapshot_frame_input(void)
{
    g_input.frame_mouse_dx = g_input.mouse_dx;
    g_input.frame_mouse_dy = g_input.mouse_dy;
    g_input.mouse_dx = 0;
    g_input.mouse_dy = 0;

    g_input.frame_wheel_dx = g_input.wheel_dx;
    g_input.frame_wheel_dy = g_input.wheel_dy;
    g_input.wheel_dx = 0;
    g_input.wheel_dy = 0;
}

static void handle_event(const SDL_Event *e)
{
    gamepad_handle_event(e);

    if (e->type == SDL_QUIT) {
        g_draw.open = 0;
        return;
    }

    if (e->type == SDL_WINDOWEVENT) {
        if (e->window.event == SDL_WINDOWEVENT_CLOSE)
            g_draw.open = 0;
        else if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            reset_input();
        return;
    }

    if (e->type == SDL_KEYDOWN) {
        int code = (int)e->key.keysym.scancode;

        if (e->key.repeat)
            return;
        if (code >= 0 && code < SDL_NUM_SCANCODES) {
            g_input.down[code] = 1;
            g_input.pressed[code] = 1;
            g_input.last_key = code;
        }
        return;
    }

    if (e->type == SDL_KEYUP) {
        int code = (int)e->key.keysym.scancode;

        if (code >= 0 && code < SDL_NUM_SCANCODES)
            g_input.down[code] = 0;
        return;
    }

    if (e->type == SDL_MOUSEMOTION) {
        int dx, dy;

        event_position_to_logical(e->motion.x, e->motion.y,
                                  &g_input.mouse_x, &g_input.mouse_y);
        event_delta_to_logical(e->motion.xrel, e->motion.yrel, &dx, &dy);
        g_input.mouse_dx += dx;
        g_input.mouse_dy += dy;
        return;
    }

    if (e->type == SDL_MOUSEBUTTONDOWN || e->type == SDL_MOUSEBUTTONUP) {
        int index = event_button_index(e->button.button);

        if (e->type == SDL_MOUSEBUTTONDOWN) {
            event_position_to_logical(e->button.x, e->button.y,
                                      &g_input.mouse_x, &g_input.mouse_y);
        }
        if (index >= 0) {
            if (e->type == SDL_MOUSEBUTTONDOWN) {
                g_input.mouse_down[index] = 1;
                g_input.mouse_pressed[index] = 1;
                g_input.mouse_button = index;
            } else {
                g_input.mouse_down[index] = 0;
            }
        }
        return;
    }

    if (e->type == SDL_MOUSEWHEEL) {
        g_input.wheel_dx += e->wheel.x;
        g_input.wheel_dy += e->wheel.y;
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

static int l_present(lua_State *L)
{
    if (!g_draw.initialized || !g_draw.open) {
        lua_pushboolean(L, 0);
        return 1;
    }

    update_frame_delta();
    audio_update_finished();

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

    snapshot_frame_input();
    present();
    lua_pushboolean(L, 1);
    return 1;
}

static void put_pixel(long long x, long long y, uint32_t color)
{
    if (x < 0 || x >= (long long)g_draw.width ||
        y < 0 || y >= (long long)g_draw.height)
        return;

    g_draw.pixels[(size_t)y * (size_t)g_draw.width + (size_t)x] =
        blend_color(g_draw.pixels[(size_t)y * (size_t)g_draw.width + (size_t)x],
                    color);
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

static void audio_clear_voice(AudioVoice *voice)
{
    free(voice->notes);
    free(voice->samples);
    memset(voice, 0, sizeof(*voice));
}

static void audio_stop(void)
{
    int i;

    if (g_audio.device)
        SDL_PauseAudioDevice(g_audio.device, 1);

    if (g_audio.mutex) {
        SDL_LockMutex(g_audio.mutex);
        for (i = 0; i < AUDIO_VOICES_MAX; ++i)
            audio_clear_voice(&g_audio.voices[i]);
        SDL_UnlockMutex(g_audio.mutex);
    }
}

static void audio_close(void)
{
    audio_stop();

    if (g_audio.device) {
        SDL_CloseAudioDevice(g_audio.device);
        g_audio.device = 0;
    }
    if (g_audio.mutex) {
        SDL_DestroyMutex(g_audio.mutex);
        g_audio.mutex = NULL;
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

static int note_sample_count(const MelodyNote *note, double speed, int rate)
{
    double samples = note->beats / speed * (double)rate;

    if (samples < 1.0)
        samples = 1.0;
    if (samples > 2147483647.0)
        samples = 2147483647.0;
    return (int)samples;
}

static void audio_set_note(AudioVoice *voice, int index, int rate)
{
    voice->index = index;
    voice->note_sample = 0;
    voice->phase = 0.0;

    if (index >= voice->count) {
        voice->note_samples = 0;
        return;
    }
    voice->note_samples =
        note_sample_count(&voice->notes[index], voice->speed, rate);
}

static double wave_sample(int wave, double phase)
{
    if (wave == AUDIO_WAVE_SQUARE)
        return phase < 0.5 ? 1.0 : -1.0;
    if (wave == AUDIO_WAVE_TRIANGLE)
        return 4.0 * fabs(phase - floor(phase + 0.5)) - 1.0;
    if (wave == AUDIO_WAVE_SAW)
        return 2.0 * phase - 1.0;
    return sin(2.0 * 3.14159265358979323846 * phase);
}

static int parse_wave(lua_State *L, int index)
{
    const char *s;
    lua_Integer n;

    if (lua_isnoneornil(L, index))
        return AUDIO_WAVE_SINE;
    if (lua_type(L, index) == LUA_TNUMBER) {
        n = luaL_checkinteger(L, index);
        if (n < AUDIO_WAVE_SINE || n > AUDIO_WAVE_SAW)
            return -1;
        return (int)n;
    }
    if (lua_type(L, index) != LUA_TSTRING)
        return -1;
    s = lua_tostring(L, index);
    if (strcmp(s, "sine") == 0)
        return AUDIO_WAVE_SINE;
    if (strcmp(s, "square") == 0 || strcmp(s, "sqr") == 0)
        return AUDIO_WAVE_SQUARE;
    if (strcmp(s, "triangle") == 0 || strcmp(s, "tri") == 0)
        return AUDIO_WAVE_TRIANGLE;
    if (strcmp(s, "saw") == 0 || strcmp(s, "sawtooth") == 0)
        return AUDIO_WAVE_SAW;
    return -1;
}

static uint16_t read_le16(const unsigned char *p)
{
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}

static uint32_t read_le32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int load_wav(const char *path, MelodyEntry *out)
{
    FILE *f;
    unsigned char head[12];
    unsigned char chunk[8];
    unsigned char fmt[16];
    unsigned char *raw = NULL;
    int16_t *mono = NULL;
    int have_fmt = 0;
    int channels = 0;
    int bits = 0;
    int rate = 0;
    uint32_t chunk_size;
    size_t frames;
    int frame;
    int ch;
    long skip;
    int rc = -1;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fread(head, 1, sizeof(head), f) != sizeof(head) ||
        memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0)
        goto done;

    while (fread(chunk, 1, sizeof(chunk), f) == sizeof(chunk)) {
        chunk_size = read_le32(chunk + 4);

        if (memcmp(chunk, "fmt ", 4) == 0) {
            if (chunk_size < 16 || fread(fmt, 1, 16, f) != 16)
                goto done;
            if (read_le16(fmt) != 1)
                goto done;
            channels = (int)read_le16(fmt + 2);
            rate = (int)read_le32(fmt + 4);
            bits = (int)read_le16(fmt + 14);
            if (channels < 1 || channels > 2 || rate <= 0 ||
                (bits != 8 && bits != 16))
                goto done;
            have_fmt = 1;
            skip = (long)(chunk_size - 16) + (long)(chunk_size & 1u);
            if (fseek(f, skip, SEEK_CUR) != 0)
                goto done;
        } else if (memcmp(chunk, "data", 4) == 0) {
            if (!have_fmt || chunk_size == 0 || chunk_size > WAV_MAX_BYTES)
                goto done;
            raw = (unsigned char *)malloc(chunk_size);
            if (!raw) {
                rc = -2;
                goto done;
            }
            if (fread(raw, 1, chunk_size, f) != chunk_size)
                goto done;
            frames = bits == 16 ? chunk_size / (size_t)(2 * channels)
                                : chunk_size / (size_t)channels;
            if (frames == 0)
                goto done;
            mono = (int16_t *)malloc(frames * sizeof(int16_t));
            if (!mono) {
                rc = -2;
                goto done;
            }
            for (frame = 0; frame < (int)frames; ++frame) {
                int32_t sum = 0;
                for (ch = 0; ch < channels; ++ch) {
                    if (bits == 16) {
                        size_t pos = ((size_t)frame * channels + ch) * 2;
                        sum += (int16_t)read_le16(raw + pos);
                    } else {
                        size_t pos = (size_t)frame * channels + ch;
                        sum += ((int)raw[pos] - 128) * 256;
                    }
                }
                mono[frame] = (int16_t)(sum / channels);
            }
            out->kind = AUDIO_KIND_WAV;
            out->notes = NULL;
            out->count = 0;
            out->samples = mono;
            out->sample_count = (int)frames;
            out->sample_rate = rate;
            mono = NULL;
            rc = 0;
            break;
        } else {
            skip = (long)chunk_size + (long)(chunk_size & 1u);
            if (fseek(f, skip, SEEK_CUR) != 0)
                goto done;
        }
    }

done:
    free(raw);
    free(mono);
    fclose(f);
    return rc;
}

typedef struct {
    long long tick;
    int off;
    int note;
    int velocity;
    int channel;
} MidiEvent;

typedef struct {
    long long start;
    long long end;
    int note;
    int velocity;
} MidiNote;

static int midi_read_vlq(const unsigned char **p, const unsigned char *end,
                         unsigned long *out)
{
    unsigned long value = 0;
    int i;

    for (i = 0; i < 4; ++i) {
        unsigned char byte;

        if (*p >= end)
            return -1;
        byte = *(*p)++;
        value = (value << 7) | (byte & 0x7f);
        if (!(byte & 0x80)) {
            *out = value;
            return 0;
        }
    }
    return -1;
}

static uint16_t midi_be16(const unsigned char *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static uint32_t midi_be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static double midi_freq(int note)
{
    return 440.0 * pow(2.0, ((double)note - 69.0) / 12.0);
}

static int midi_event_cmp(const void *a, const void *b)
{
    const MidiEvent *ea = (const MidiEvent *)a;
    const MidiEvent *eb = (const MidiEvent *)b;

    if (ea->tick != eb->tick)
        return ea->tick < eb->tick ? -1 : 1;
    if (ea->off != eb->off)
        return ea->off < eb->off ? -1 : 1;
    if (ea->note != eb->note)
        return ea->note < eb->note ? -1 : 1;
    return ea->channel - eb->channel;
}

static int midi_note_cmp(const void *a, const void *b)
{
    const MidiNote *na = (const MidiNote *)a;
    const MidiNote *nb = (const MidiNote *)b;

    if (na->start != nb->start)
        return na->start < nb->start ? -1 : 1;
    if (na->note != nb->note)
        return na->note > nb->note ? -1 : 1;
    return na->end - nb->end < 0 ? -1 : (na->end != nb->end);
}

static int midi_append_event(MidiEvent **events, int *count, int *cap,
                             MidiEvent event)
{
    if (*count == *cap) {
        int new_cap = *cap ? *cap * 2 : 32;
        MidiEvent *tmp = (MidiEvent *)realloc(*events,
            (size_t)new_cap * sizeof(MidiEvent));

        if (!tmp)
            return -2;
        *events = tmp;
        *cap = new_cap;
    }
    (*events)[(*count)++] = event;
    return 0;
}

static int midi_append_note(MidiNote **notes, int *count, int *cap,
                            MidiNote note)
{
    if (*count == *cap) {
        int new_cap = *cap ? *cap * 2 : 32;
        MidiNote *tmp = (MidiNote *)realloc(*notes,
            (size_t)new_cap * sizeof(MidiNote));

        if (!tmp)
            return -2;
        *notes = tmp;
        *cap = new_cap;
    }
    (*notes)[(*count)++] = note;
    return 0;
}

static int midi_parse_track(const unsigned char *data, size_t size, int ppq,
                            MidiNote **out_notes, int *out_count,
                            unsigned long *out_tempo)
{
    MidiEvent *events = NULL;
    MidiNote *notes = NULL;
    int event_count = 0;
    int event_cap = 0;
    int note_count = 0;
    int note_cap = 0;
    int open[16][128];
    const unsigned char *p = data;
    const unsigned char *end = data + size;
    unsigned long tick = 0;
    unsigned long max_tick = 0;
    int running = 0;
    int i;
    int rc = -1;

    for (i = 0; i < 16; ++i) {
        for (int j = 0; j < 128; ++j)
            open[i][j] = -1;
    }

    while (p < end) {
        unsigned long delta;
        unsigned long len;
        int status;
        int data_len;
        int d1;
        int d2;
        MidiEvent event;

        if (midi_read_vlq(&p, end, &delta) != 0)
            goto done;
        tick += delta;
        if (p >= end)
            break;
        if (tick > 0x7fffffffUL)
            goto done;
        max_tick = tick;

        status = *p;
        if (status & 0x80) {
            running = status;
            ++p;
        } else if (running == 0) {
            goto done;
        } else {
            status = running;
        }

        if (status == 0xff) {
            unsigned char type;

            if (p >= end)
                goto done;
            type = *p++;
            if (midi_read_vlq(&p, end, &len) != 0 ||
                (size_t)(end - p) < len)
                goto done;
            if (type == 0x2f)
                break;
            if (type == 0x51 && len >= 3) {
                unsigned long tempo = ((unsigned long)p[0] << 16) |
                                      ((unsigned long)p[1] << 8) |
                                      (unsigned long)p[2];
                if (tempo > 0)
                    *out_tempo = tempo;
            }
            p += len;
            continue;
        }

        if (status == 0xf0 || status == 0xf7) {
            if (midi_read_vlq(&p, end, &len) != 0 ||
                (size_t)(end - p) < len)
                goto done;
            p += len;
            continue;
        }

        if (status >= 0xf1 && status <= 0xfe)
            continue;
        if (status < 0x80 || status > 0xef)
            goto done;

        data_len = (status >> 4) == 0xc || (status >> 4) == 0xd ? 1 : 2;
        if ((size_t)(end - p) < (size_t)data_len)
            goto done;
        d1 = p[0];
        d2 = data_len > 1 ? p[1] : 0;
        p += data_len;

        event.tick = (long long)tick;
        event.note = d1;
        event.velocity = d2;
        event.channel = status & 0x0f;
        if ((status >> 4) == 0x9 && d2 > 0) {
            event.off = 0;
            if (midi_append_event(&events, &event_count, &event_cap,
                                  event) != 0) {
                rc = -2;
                goto done;
            }
        } else if ((status >> 4) == 0x8 ||
                   ((status >> 4) == 0x9 && d2 == 0)) {
            event.off = 1;
            if (midi_append_event(&events, &event_count, &event_cap,
                                  event) != 0) {
                rc = -2;
                goto done;
            }
        }
    }

    qsort(events, (size_t)event_count, sizeof(MidiEvent), midi_event_cmp);
    for (i = 0; i < event_count; ++i) {
        MidiEvent *event = &events[i];

        if (!event->off) {
            open[event->channel][event->note] = i;
            continue;
        }
        if (open[event->channel][event->note] >= 0) {
            MidiEvent *on = &events[open[event->channel][event->note]];
            MidiNote note;

            if (event->tick > on->tick) {
                note.start = on->tick;
                note.end = event->tick;
                note.note = on->note;
                note.velocity = on->velocity;
                if (midi_append_note(&notes, &note_count, &note_cap,
                                     note) != 0) {
                    rc = -2;
                    goto done;
                }
            }
            open[event->channel][event->note] = -1;
        }
    }

    for (int ch = 0; ch < 16; ++ch) {
        for (int note = 0; note < 128; ++note) {
            if (open[ch][note] >= 0) {
                MidiEvent *on = &events[open[ch][note]];
                MidiNote item;

                item.start = on->tick;
                item.end = (long long)max_tick > item.start
                               ? (long long)max_tick
                               : item.start + ppq;
                item.note = on->note;
                item.velocity = on->velocity;
                if (midi_append_note(&notes, &note_count, &note_cap,
                                     item) != 0) {
                    rc = -2;
                    goto done;
                }
            }
        }
    }

    qsort(notes, (size_t)note_count, sizeof(MidiNote), midi_note_cmp);
    for (i = 1; i < note_count; ++i) {
        if (notes[i].start < notes[i - 1].end)
            notes[i - 1].end = notes[i].start;
        if (notes[i - 1].end <= notes[i - 1].start)
            notes[i - 1].end = notes[i - 1].start;
    }

    *out_notes = notes;
    *out_count = note_count;
    notes = NULL;
    rc = 0;

done:
    free(events);
    free(notes);
    return rc;
}

static int midi_append_melody_note(MelodyNote **notes, int *count, int *cap,
                                   MelodyNote note)
{
    if (*count == *cap) {
        int new_cap = *cap ? *cap * 2 : 32;
        MelodyNote *tmp = (MelodyNote *)realloc(*notes,
            (size_t)new_cap * sizeof(MelodyNote));

        if (!tmp)
            return -2;
        *notes = tmp;
        *cap = new_cap;
    }
    (*notes)[(*count)++] = note;
    return 0;
}

static int midi_to_melody(MidiNote *notes, int count, int ppq,
                          MelodyNote **out_notes, int *out_count)
{
    MelodyNote *result = NULL;
    int result_count = 0;
    int result_cap = 0;
    long long cursor = 0;
    int i;

    qsort(notes, (size_t)count, sizeof(MidiNote), midi_note_cmp);
    for (i = 0; i < count; ++i) {
        MelodyNote note;

        if (notes[i].end <= notes[i].start)
            continue;
        if (notes[i].start > cursor) {
            double rest = (double)(notes[i].start - cursor) / (double)ppq;

            while (rest > 64.0) {
                note.rest = 1;
                note.freq = 0.0;
                note.beats = 64.0;
                if (midi_append_melody_note(&result, &result_count,
                                            &result_cap, note) != 0)
                    goto oom;
                rest -= 64.0;
            }
            if (rest > 0.000001) {
                note.rest = 1;
                note.freq = 0.0;
                note.beats = rest;
                if (midi_append_melody_note(&result, &result_count,
                                            &result_cap, note) != 0)
                    goto oom;
            }
            cursor = notes[i].start;
        }

        note.rest = 0;
        note.freq = midi_freq(notes[i].note);
        note.beats = (double)(notes[i].end - notes[i].start) / (double)ppq;
        while (note.beats > 64.0) {
            MelodyNote part = note;

            part.beats = 64.0;
            if (midi_append_melody_note(&result, &result_count, &result_cap,
                                        part) != 0)
                goto oom;
            note.beats -= 64.0;
        }
        if (note.beats > 0.000001) {
            if (midi_append_melody_note(&result, &result_count, &result_cap,
                                        note) != 0)
                goto oom;
        }
        if (notes[i].end > cursor)
            cursor = notes[i].end;
    }

    *out_notes = result;
    *out_count = result_count;
    return 0;

oom:
    free(result);
    return -2;
}

static int read_file_bytes(const char *path, unsigned char **out,
                           size_t *out_size)
{
    FILE *f;
    long size;
    unsigned char *buffer;

    f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    size = ftell(f);
    if (size < 0 || size > MIDI_MAX_BYTES ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return -1;
    }
    buffer = (unsigned char *)malloc((size_t)size ? (size_t)size : 1);
    if (!buffer) {
        fclose(f);
        return -2;
    }
    if (size > 0 && fread(buffer, 1, (size_t)size, f) != (size_t)size) {
        free(buffer);
        fclose(f);
        return -1;
    }
    fclose(f);
    *out = buffer;
    *out_size = (size_t)size;
    return 0;
}

static void audio_callback(void *userdata, Uint8 *stream, int len)
{
    int16_t *out = (int16_t *)stream;
    int samples = len / (int)sizeof(int16_t);
    int i;
    int v;

    (void)userdata;
    SDL_memset(stream, 0, (size_t)len);
    if (!g_audio.mutex)
        return;

    SDL_LockMutex(g_audio.mutex);
    for (i = 0; i < samples; ++i) {
        double mix = 0.0;

        for (v = 0; v < AUDIO_VOICES_MAX; ++v) {
            AudioVoice *voice = &g_audio.voices[v];
            double sample = 0.0;

            if (!voice->playing)
                continue;

            if (voice->kind == AUDIO_KIND_MELODY) {
                const MelodyNote *note;

                if (voice->index >= voice->count) {
                    voice->playing = 0;
                    continue;
                }
                if (voice->note_sample >= voice->note_samples) {
                    audio_set_note(voice, voice->index + 1,
                                   g_audio.sample_rate);
                    if (voice->index >= voice->count) {
                        voice->playing = 0;
                        continue;
                    }
                }
                note = &voice->notes[voice->index];
                if (!note->rest && note->freq > 0.0) {
                    double attack = 0.005 * (double)g_audio.sample_rate;
                    double release = 0.010 * (double)g_audio.sample_rate;
                    double left = (double)(voice->note_samples -
                                           voice->note_sample);
                    double envelope = 1.0;

                    if (attack > 0.0 &&
                        (double)voice->note_sample < attack)
                        envelope = (double)voice->note_sample / attack;
                    if (release > 0.0 && left < release)
                        envelope *= left / release;
                    if (envelope < 0.0)
                        envelope = 0.0;
                    if (envelope > 1.0)
                        envelope = 1.0;

                    sample = wave_sample(voice->wave, voice->phase) *
                             envelope;
                    voice->phase += note->freq /
                                    (double)g_audio.sample_rate;
                    if (voice->phase >= 1.0)
                        voice->phase -= floor(voice->phase);
                }
                ++voice->note_sample;
            } else {
                double step = voice->speed * (double)voice->sample_rate /
                              (double)g_audio.sample_rate;
                int pos;
                double frac;
                double a;
                double b;

                voice->sample_pos += step;
                if (voice->sample_pos >= (double)voice->sample_count) {
                    voice->playing = 0;
                    continue;
                }
                pos = (int)voice->sample_pos;
                frac = voice->sample_pos - (double)pos;
                a = (double)voice->samples[pos];
                b = pos + 1 < voice->sample_count
                        ? (double)voice->samples[pos + 1] : a;
                sample = (a * (1.0 - frac) + b * frac) / 32768.0;
            }

            mix += sample * ((double)voice->volume / 255.0);
        }

        if (mix > 1.0)
            mix = 1.0;
        if (mix < -1.0)
            mix = -1.0;
        out[i] = (int16_t)(mix * 12000.0);
    }
    SDL_UnlockMutex(g_audio.mutex);
}

static int ensure_audio_device(lua_State *L)
{
    SDL_AudioSpec want;
    SDL_AudioSpec have;

    if (g_audio.device)
        return 0;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return luaL_error(L, "draw.play: SDL_InitSubSystem failed: %s",
                          SDL_GetError());

    g_audio.mutex = SDL_CreateMutex();
    if (!g_audio.mutex)
        return luaL_error(L, "draw.play: cannot create audio mutex");

    SDL_zero(want);
    want.freq = AUDIO_SAMPLE_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = AUDIO_SAMPLE_COUNT;
    want.callback = audio_callback;

    g_audio.device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!g_audio.device) {
        SDL_DestroyMutex(g_audio.mutex);
        g_audio.mutex = NULL;
        return luaL_error(L, "draw.play: cannot open audio device: %s",
                          SDL_GetError());
    }
    g_audio.sample_rate = have.freq > 0 ? have.freq : AUDIO_SAMPLE_RATE;
    return 0;
}

static void audio_update_finished(void)
{
    int playing = 0;
    int i;

    if (!g_audio.device || !g_audio.mutex)
        return;

    SDL_LockMutex(g_audio.mutex);
    for (i = 0; i < AUDIO_VOICES_MAX; ++i) {
        if (g_audio.voices[i].playing) {
            playing = 1;
            break;
        }
    }
    SDL_UnlockMutex(g_audio.mutex);

    if (!playing)
        SDL_PauseAudioDevice(g_audio.device, 1);
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
    g_draw.prev_frame = 0;
    g_draw.has_prev_frame = 0;
    g_draw.frame_delta = 0.0;
    g_draw.alpha = 255;

    gamepad_shutdown();
    audio_close();

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
    reset_input();

    if (SDL_WasInit(SDL_INIT_VIDEO) == 0) {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) != 0)
            return luaL_error(L, "draw.open: SDL_Init failed: %s",
                              SDL_GetError());
        g_draw.sdl_owned = 1;
    }

    gamepad_init();

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
    SDL_SetTextureBlendMode(g_draw.texture, SDL_BLENDMODE_BLEND);

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
    g_draw.prev_frame = g_draw.last_tick;
    g_draw.has_prev_frame = 0;
    g_draw.frame_delta = 0.0;
    g_draw.alpha = 255;
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
    int r, g, b, a;
    const char *name;
    size_t len;

    r = clamp_color(luaL_checkinteger(L, 2));
    g = clamp_color(luaL_checkinteger(L, 3));
    b = clamp_color(luaL_checkinteger(L, 4));
    a = clamp_color(luaL_optinteger(L, 5, 255));

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
    g_palette[index].a = a;

    lua_pushinteger(L, index);
    return 1;
}

static int sprite_palette_index(lua_State *L, int index)
{
    const char *name;
    int slot;
    int r, g, b, a;

    if (lua_type(L, index) == LUA_TSTRING) {
        name = lua_tostring(L, index);
        slot = find_palette_name(name);
        if (slot < 0)
            return luaL_error(L,
                "draw.sprite: palette color '%s' is not defined", name);
        return slot;
    }

    if (lua_type(L, index) == LUA_TTABLE) {
        if (read_color(L, index, &r, &g, &b, &a) < 0)
            return luaL_error(L,
                "draw.sprite: palette entry must be a color name or {r, g, b, [a]}");
        slot = find_free_palette();
        if (slot < 0)
            return luaL_error(L, "draw.sprite: out of memory");
        g_palette[slot].name[0] = '\0';
        g_palette[slot].used = 1;
        g_palette[slot].r = r;
        g_palette[slot].g = g;
        g_palette[slot].b = b;
        g_palette[slot].a = a;
        return slot;
    }

    return luaL_error(L,
        "draw.sprite: palette entry must be a color name or {r, g, b, [a]}");
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
    int *palette_map = NULL;
    int palette_count = 0;
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

    if (!lua_isnoneornil(L, 3)) {
        luaL_checktype(L, 3, LUA_TTABLE);
        palette_count = (int)lua_rawlen(L, 3);
        if (palette_count <= 0)
            return luaL_error(L, "draw.sprite: palette is empty");
        palette_map = (int *)malloc((size_t)palette_count * sizeof(int));
        if (!palette_map)
            return luaL_error(L, "draw.sprite: out of memory");
        for (i = 0; i < (size_t)palette_count; ++i) {
            lua_rawgeti(L, 3, (int)i + 1);
            palette_map[i] = sprite_palette_index(L, lua_gettop(L));
            lua_pop(L, 1);
        }
    }

    pixels = (int *)malloc((size_t)width * (size_t)height * sizeof(int));
    if (!pixels) {
        free(palette_map);
        return luaL_error(L, "draw.sprite: out of memory");
    }

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

                    int limit = palette_map ? palette_count - 1
                                            : PALETTE_INDEX_MAX;

                    if (raw < -1 || raw > limit) {
                        lua_pop(L, 2);
                        free(pixels);
                        free(palette_map);
                        return luaL_error(L,
                            "draw.sprite: palette index is out of range in row %d",
                            row + 1);
                    }
                    value = raw < 0 ? -1
                                    : (palette_map ? palette_map[(int)raw]
                                                   : (int)raw);
                } else {
                    lua_pop(L, 2);
                    free(pixels);
                    free(palette_map);
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
                free(palette_map);
                if (rc == -2)
                    return luaL_error(L, "draw.sprite: out of memory");
                return luaL_error(L,
                    "draw.sprite: invalid number in row %d", row + 1);
            }

            for (col = 0; col < tmp_len; ++col) {
                int value = tmp[col];
                int limit = palette_map ? palette_count - 1
                                        : PALETTE_INDEX_MAX;

                if (value < -1 || value > limit) {
                    free(tmp);
                    lua_pop(L, 1);
                    free(pixels);
                    free(palette_map);
                    return luaL_error(L,
                        "draw.sprite: palette index %d in row %d is out of range",
                        value, row + 1);
                }

                pixels[(size_t)row * width + col] =
                    value < 0 ? -1 : (palette_map ? palette_map[value] : value);
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
            free(palette_map);
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
    free(palette_map);

    lua_pushinteger(L, index);
    return 1;
}

static int l_animation(lua_State *L)
{
    const char *name;
    size_t name_len;
    size_t count;
    size_t i;
    double fps;
    char **frames = NULL;
    int index;

    name = luaL_checkstring(L, 1);
    name_len = strlen(name);
    if (name_len == 0)
        return luaL_error(L, "draw.animation: name must not be empty");
    if (name_len >= ANIM_NAME_MAX)
        return luaL_error(L, "draw.animation: name is too long");

    luaL_checktype(L, 2, LUA_TTABLE);
    count = (size_t)lua_rawlen(L, 2);
    if (count == 0)
        return luaL_error(L, "draw.animation: animation is empty");
    if (count > ANIM_FRAME_MAX)
        return luaL_error(L, "draw.animation: too many frames (max %d)",
                          ANIM_FRAME_MAX);

    fps = luaL_optnumber(L, 3, 8.0);
    if (fps < 0.1 || fps > 1000.0)
        return luaL_error(L, "draw.animation: fps must be 0.1..1000");

    frames = (char **)calloc(count, sizeof(char *));
    if (!frames)
        return luaL_error(L, "draw.animation: out of memory");

    for (i = 0; i < count; ++i) {
        const char *frame;
        size_t len;

        lua_rawgeti(L, 2, (int)i + 1);
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            for (i = 0; i < count; ++i)
                free(frames[i]);
            free(frames);
            return luaL_error(L, "draw.animation: frame %d must be a string",
                              (int)i + 1);
        }
        frame = lua_tostring(L, -1);
        len = strlen(frame);
        if (len == 0 || len >= SPRITE_NAME_MAX) {
            lua_pop(L, 1);
            for (i = 0; i < count; ++i)
                free(frames[i]);
            free(frames);
            return luaL_error(L, "draw.animation: invalid sprite name in frame %d",
                              (int)i + 1);
        }
        frames[i] = (char *)malloc(len + 1);
        if (!frames[i]) {
            lua_pop(L, 1);
            for (i = 0; i < count; ++i)
                free(frames[i]);
            free(frames);
            return luaL_error(L, "draw.animation: out of memory");
        }
        memcpy(frames[i], frame, len + 1);
        lua_pop(L, 1);
    }

    index = find_anim_name(name);
    if (index < 0) {
        index = first_free_anim();
        if (index < 0) {
            for (i = 0; i < count; ++i)
                free(frames[i]);
            free(frames);
            return luaL_error(L, "draw.animation: too many animations (max %d)",
                              ANIM_MAX);
        }
        memcpy(g_anims[index].name, name, name_len + 1);
        g_anims[index].used = 1;
    } else {
        free_anim(&g_anims[index]);
    }

    g_anims[index].frames = frames;
    g_anims[index].count = (int)count;
    g_anims[index].fps = fps;
    g_anims[index].started = SDL_GetTicks();

    lua_pushinteger(L, index + 1);
    return 1;
}

typedef struct {
    uint32_t key;
    int count;
} ImageColor;

static ImageColor *g_image_colors;

typedef struct {
    int lo;
    int hi;
    int rmin, rmax;
    int gmin, gmax;
    int bmin, bmax;
    int pixels;
} ImageBox;

static int g_image_sort_channel;

static int image_key(uint32_t key)
{
    return (int)(key & 0xffu);
}

static int image_key_r(uint32_t key)
{
    return (int)((key >> 16) & 0xffu);
}

static int image_key_g(uint32_t key)
{
    return (int)((key >> 8) & 0xffu);
}

static uint32_t image_make_key(int r, int g, int b)
{
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static int image_u32_cmp(const void *a, const void *b)
{
    uint32_t ka = *(const uint32_t *)a;
    uint32_t kb = *(const uint32_t *)b;

    return ka < kb ? -1 : (ka > kb ? 1 : 0);
}

static int image_order_cmp(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    int va;
    int vb;

    if (g_image_sort_channel == 0)
        return g_image_colors[ia].key < g_image_colors[ib].key ? -1 :
               (g_image_colors[ia].key > g_image_colors[ib].key ? 1 : 0);
    if (g_image_sort_channel == 1) {
        va = image_key_r(g_image_colors[ia].key);
        vb = image_key_r(g_image_colors[ib].key);
    } else if (g_image_sort_channel == 2) {
        va = image_key_g(g_image_colors[ia].key);
        vb = image_key_g(g_image_colors[ib].key);
    } else {
        va = image_key(g_image_colors[ia].key);
        vb = image_key(g_image_colors[ib].key);
    }
    return va < vb ? -1 : (va > vb ? 1 : 0);
}

static void image_box_init(ImageBox *box, const ImageColor *colors, int lo,
                           int hi)
{
    int i;

    box->lo = lo;
    box->hi = hi;
    box->rmin = box->gmin = box->bmin = 255;
    box->rmax = box->gmax = box->bmax = 0;
    box->pixels = 0;
    for (i = lo; i <= hi; ++i) {
        int r = image_key_r(colors[i].key);
        int g = image_key_g(colors[i].key);
        int b = image_key(colors[i].key);

        if (r < box->rmin) box->rmin = r;
        if (r > box->rmax) box->rmax = r;
        if (g < box->gmin) box->gmin = g;
        if (g > box->gmax) box->gmax = g;
        if (b < box->bmin) box->bmin = b;
        if (b > box->bmax) box->bmax = b;
        box->pixels += colors[i].count;
    }
}

static int image_nearest_palette(uint32_t key, const uint32_t *palette,
                                 int count)
{
    int best = 0;
    long long best_dist = LLONG_MAX;
    int i;

    for (i = 0; i < count; ++i) {
        int dr = image_key_r(key) - image_key_r(palette[i]);
        int dg = image_key_g(key) - image_key_g(palette[i]);
        int db = image_key(key) - image_key(palette[i]);
        long long dist = (long long)dr * dr + (long long)dg * dg +
                         (long long)db * db;

        if (dist < best_dist) {
            best_dist = dist;
            best = i;
        }
    }
    return best;
}

static int image_quantize(const unsigned char *pixels, int w, int h,
                          int channels, int alpha_threshold, int maxcolors,
                          int **out_indices, int *out_colors,
                          uint32_t *out_palette)
{
    uint32_t *keys = NULL;
    ImageColor *colors = NULL;
    ImageBox *boxes = NULL;
    int *order = NULL;
    int *mapping = NULL;
    int *result = NULL;
    int key_count = 0;
    int unique = 0;
    int color_inputs = 0;
    int box_count = 0;
    int i;
    int rc = -1;

    for (i = 0; i < w * h; ++i) {
        if (pixels[i * channels + 3] >= alpha_threshold)
            ++key_count;
    }
    if (key_count == 0)
        return -1;

    keys = (uint32_t *)malloc((size_t)key_count * sizeof(uint32_t));
    if (!keys)
        return -2;
    key_count = 0;
    for (i = 0; i < w * h; ++i) {
        unsigned char a = pixels[i * channels + 3];

        if (a >= alpha_threshold) {
            keys[key_count++] = image_make_key(pixels[i * channels],
                                               pixels[i * channels + 1],
                                               pixels[i * channels + 2]);
        }
    }
    qsort(keys, (size_t)key_count, sizeof(uint32_t), image_u32_cmp);

    colors = (ImageColor *)malloc((size_t)key_count * sizeof(ImageColor));
    if (!colors) {
        rc = -2;
        goto done;
    }
    for (i = 0; i < key_count; ++i) {
        if (i == 0 || keys[i] != keys[i - 1]) {
            colors[unique].key = keys[i];
            colors[unique].count = 1;
            ++unique;
        } else {
            --unique;
            ++colors[unique].count;
            ++unique;
        }
    }
    color_inputs = unique;

    if (unique <= maxcolors) {
        for (i = 0; i < unique; ++i)
            out_palette[i] = colors[i].key;
        mapping = NULL;
    } else {
        boxes = (ImageBox *)malloc((size_t)maxcolors * sizeof(ImageBox));
        order = (int *)malloc((size_t)color_inputs * sizeof(int));
        mapping = (int *)malloc((size_t)color_inputs * sizeof(int));
        if (!boxes || !order || !mapping) {
            rc = -2;
            goto done;
        }
        for (i = 0; i < color_inputs; ++i)
            order[i] = i;
        g_image_colors = colors;
        image_box_init(&boxes[0], colors, 0, color_inputs - 1);
        box_count = 1;

        while (box_count < maxcolors) {
            int best = -1;
            long long best_score = -1;
            int channel = 0;
            int len;
            int half;

            for (i = 0; i < box_count; ++i) {
                int rr = boxes[i].rmax - boxes[i].rmin;
                int gg = boxes[i].gmax - boxes[i].gmin;
                int bb = boxes[i].bmax - boxes[i].bmin;
                int range = rr;
                long long score;

                if (gg > range) range = gg;
                if (bb > range) range = bb;
                score = ((long long)range + 1) * boxes[i].pixels;
                if (boxes[i].hi > boxes[i].lo && score > best_score) {
                    best_score = score;
                    best = i;
                    channel = rr >= gg && rr >= bb ? 1
                            : (gg >= bb ? 2 : 3);
                }
            }
            if (best < 0)
                break;

            g_image_sort_channel = channel;
            len = boxes[best].hi - boxes[best].lo + 1;
            qsort(order + boxes[best].lo, (size_t)len, sizeof(int),
                  image_order_cmp);
            half = len / 2;
            if (half < 1) half = 1;
            if (half >= len) half = len - 1;

            image_box_init(&boxes[box_count], colors,
                           boxes[best].lo + half, boxes[best].hi);
            image_box_init(&boxes[best], colors,
                           boxes[best].lo, boxes[best].lo + half - 1);
            ++box_count;
        }

        for (i = 0; i < box_count; ++i) {
            ImageBox *box = &boxes[i];
            long long sr = 0, sg = 0, sb = 0, total = 0;
            int j;

            for (j = box->lo; j <= box->hi; ++j) {
                int count = colors[j].count;

                sr += (long long)image_key_r(colors[j].key) * count;
                sg += (long long)image_key_g(colors[j].key) * count;
                sb += (long long)image_key(colors[j].key) * count;
                total += count;
            }
            out_palette[i] = image_make_key((int)(sr / total),
                                            (int)(sg / total),
                                            (int)(sb / total));
        }
        for (i = 0; i < color_inputs; ++i)
            mapping[i] = image_nearest_palette(colors[i].key, out_palette,
                                               box_count);
        unique = box_count;
    }

    result = (int *)malloc((size_t)w * h * sizeof(int));
    if (!result) {
        rc = -2;
        goto done;
    }
    for (i = 0; i < w * h; ++i) {
        int pos = -1;
        int lo = 0;
        int hi = color_inputs - 1;
        uint32_t key;

        if (pixels[i * channels + 3] < alpha_threshold) {
            result[i] = -1;
            continue;
        }
        key = image_make_key(pixels[i * channels],
                             pixels[i * channels + 1],
                             pixels[i * channels + 2]);
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t mid_key = colors[mid].key;

            if (mid_key == key) {
                pos = mid;
                break;
            }
            if (mid_key < key)
                lo = mid + 1;
            else
                hi = mid - 1;
        }
        if (pos < 0) {
            rc = -1;
            goto done;
        }
        result[i] = mapping ? mapping[pos] : pos;
    }

    *out_indices = result;
    *out_colors = unique;
    result = NULL;
    rc = 0;

done:
    g_image_colors = NULL;
    free(keys);
    free(colors);
    free(boxes);
    free(order);
    free(mapping);
    free(result);
    return rc;
}

static unsigned char *load_rgba_file(const char *path, int *w, int *h,
                                     char *err, size_t err_size)
{
    SDL_Surface *src;
    SDL_Surface *rgba;
    unsigned char *data;
    size_t row;
    size_t total;

    src = IMG_Load(path);
    if (!src) {
        snprintf(err, err_size, "%s", IMG_GetError());
        return NULL;
    }
    rgba = SDL_ConvertSurfaceFormat(src, SDL_PIXELFORMAT_RGBA32, 0);
    SDL_FreeSurface(src);
    if (!rgba) {
        snprintf(err, err_size, "%s", SDL_GetError());
        return NULL;
    }
    if (rgba->w <= 0 || rgba->h <= 0 || rgba->w > IMAGE_MAX_DIM ||
        rgba->h > IMAGE_MAX_DIM || (size_t)rgba->w > SIZE_MAX / 4 ||
        (size_t)rgba->h > SIZE_MAX / ((size_t)rgba->w * 4)) {
        SDL_FreeSurface(rgba);
        snprintf(err, err_size, "image is too large");
        return NULL;
    }
    total = (size_t)rgba->w * (size_t)rgba->h * 4;
    data = (unsigned char *)malloc(total);
    if (!data) {
        SDL_FreeSurface(rgba);
        snprintf(err, err_size, "out of memory");
        return NULL;
    }
    for (row = 0; row < (size_t)rgba->h; ++row) {
        memcpy(data + row * (size_t)rgba->w * 4,
               (unsigned char *)rgba->pixels + row * (size_t)rgba->pitch,
               (size_t)rgba->w * 4);
    }
    *w = rgba->w;
    *h = rgba->h;
    SDL_FreeSurface(rgba);
    return data;
}

static int l_font(lua_State *L)
{
    const char *name;
    const char *path;
    size_t name_len;
    int cell_w;
    int cell_h;
    int first;
    int scale;
    int w, h;
    unsigned char *data;
    char err[256];
    int cols, rows, count, stride;
    int has_alpha = 0;
    unsigned char *masks = NULL;
    FontEntry *font;
    int index;
    int gx, gy, px, py;
    size_t i;
    size_t total;

    name = luaL_checkstring(L, 1);
    path = luaL_checkstring(L, 2);
    name_len = strlen(name);
    if (name_len == 0)
        return luaL_error(L, "draw.font: name must not be empty");
    if (name_len >= FONT_NAME_MAX)
        return luaL_error(L, "draw.font: name is too long");

    cell_w = (int)luaL_optinteger(L, 3, 8);
    cell_h = (int)luaL_optinteger(L, 4, 8);
    first = (int)luaL_optinteger(L, 5, 32);
    scale = (int)luaL_optinteger(L, 6, 1);
    if (cell_w < 1 || cell_w > 64 || cell_h < 1 || cell_h > 64)
        return luaL_error(L, "draw.font: cell size must be 1..64");
    if (first < 0 || first >= FONT_CHARS_MAX)
        return luaL_error(L, "draw.font: first code must be 0..255");
    if (scale < 1 || scale > 16)
        return luaL_error(L, "draw.font: scale must be 1..16");

    data = load_rgba_file(path, &w, &h, err, sizeof(err));
    if (!data)
        return luaL_error(L, "draw.font: cannot load '%s': %s", path, err);
    cols = w / cell_w;
    rows = h / cell_h;
    if (cols <= 0 || rows <= 0 || cols > FONT_CHARS_MAX ||
        rows > FONT_CHARS_MAX || cols > INT_MAX / rows) {
        free(data);
        return luaL_error(L, "draw.font: invalid glyph grid");
    }
    count = cols * rows;
    if (count > FONT_CHARS_MAX - first) {
        free(data);
        return luaL_error(L, "draw.font: glyph codes are out of range");
    }
    stride = (cell_w + 7) / 8;
    total = (size_t)count * (size_t)cell_h * (size_t)stride;
    masks = (unsigned char *)calloc(total, 1);
    if (!masks) {
        free(data);
        return luaL_error(L, "draw.font: out of memory");
    }

    for (i = 0; i < (size_t)w * h * 4; i += 4) {
        if (data[i + 3] != 255) {
            has_alpha = 1;
            break;
        }
    }

    for (gy = 0; gy < rows; ++gy) {
        for (gx = 0; gx < cols; ++gx) {
            unsigned char *glyph = masks +
                (size_t)(gy * cols + gx) * cell_h * stride;

            for (py = 0; py < cell_h; ++py) {
                for (px = 0; px < cell_w; ++px) {
                    size_t pos = ((size_t)(gy * cell_h + py) * w +
                                  (size_t)(gx * cell_w + px)) * 4;
                    int set;

                    if (has_alpha) {
                        set = data[pos + 3] >= 128;
                    } else {
                        int luma = (data[pos] * 299 + data[pos + 1] * 587 +
                                    data[pos + 2] * 114) / 1000;

                        set = luma < 128;
                    }
                    if (set)
                        glyph[py * stride + px / 8] |=
                            (unsigned char)(0x80 >> (px % 8));
                }
            }
        }
    }
    free(data);

    index = find_font_name(name);
    if (index < 0) {
        index = first_free_font();
        if (index < 0) {
            free(masks);
            return luaL_error(L, "draw.font: too many fonts (max %d)",
                              FONT_MAX);
        }
        memcpy(g_fonts[index].name, name, name_len + 1);
        g_fonts[index].used = 1;
    } else {
        free_font(&g_fonts[index]);
    }

    font = &g_fonts[index];
    font->first = first;
    font->count = count;
    font->cell_w = cell_w;
    font->cell_h = cell_h;
    font->stride = stride;
    font->scale = scale;
    font->masks = masks;

    lua_pushinteger(L, index + 1);
    return 1;
}

static int l_spriteimage(lua_State *L)
{
    const char *name;
    const char *path;
    size_t name_len;
    int maxcolors;
    int alpha_threshold;
    int w, h;
    unsigned char *data;
    char err[256];
    int *indices = NULL;
    uint32_t palette[PALETTE_INDEX_MAX + 1];
    int color_count = 0;
    int first_palette = 0;
    int *sprite_pixels = NULL;
    int width, height;
    int index;
    int qrc;
    size_t total;

    name = luaL_checkstring(L, 1);
    path = luaL_checkstring(L, 2);
    name_len = strlen(name);
    if (name_len == 0)
        return luaL_error(L, "draw.spriteimage: name must not be empty");
    if (name_len >= SPRITE_NAME_MAX)
        return luaL_error(L, "draw.spriteimage: name is too long");

    maxcolors = (int)luaL_optinteger(L, 3, 256);
    alpha_threshold = clamp_color((int)luaL_optinteger(L, 4, 128));
    if (maxcolors < 2 || maxcolors > PALETTE_INDEX_MAX + 1)
        return luaL_error(L, "draw.spriteimage: maxcolors must be 2..1024");

    data = load_rgba_file(path, &w, &h, err, sizeof(err));
    if (!data)
        return luaL_error(L, "draw.spriteimage: cannot load '%s': %s", path,
                          err);
    total = (size_t)w * (size_t)h;
    if (total > IMAGE_MAX_PIXELS) {
        free(data);
        return luaL_error(L, "draw.spriteimage: image is too large");
    }

    qrc = image_quantize(data, w, h, 4, alpha_threshold, maxcolors,
                         &indices, &color_count, palette);
    if (qrc != 0) {
        free(data);
        free(indices);
        if (qrc == -2)
            return luaL_error(L, "draw.spriteimage: out of memory");
        return luaL_error(L, "draw.spriteimage: image has no visible pixels");
    }

    {
        int available = 0;
        int i;

        for (i = 0; i < g_palette_count; ++i) {
            if (!g_palette[i].used)
                ++available;
        }
        int needed = color_count - available;

        if (needed < 0)
            needed = 0;
        if (g_palette_count + needed > PALETTE_INDEX_MAX + 1) {
            free(data);
            free(indices);
            return luaL_error(L, "draw.spriteimage: not enough palette slots");
        }
    }

    for (int i = 0; i < color_count; ++i) {
        int r = image_key_r(palette[i]);
        int g = image_key_g(palette[i]);
        int b = image_key(palette[i]);
        int pi = auto_palette_add(name, i, r, g, b, 255);

        if (pi < 0) {
            free(data);
            free(indices);
            return luaL_error(L, "draw.spriteimage: cannot create palette");
        }
        if (i == 0)
            first_palette = pi;
        for (size_t j = 0; j < total; ++j) {
            if (indices[j] == i)
                indices[j] = pi;
        }
    }

    width = w;
    height = h;
    sprite_pixels = (int *)malloc(total * sizeof(int));
    if (!sprite_pixels) {
        free(data);
        free(indices);
        return luaL_error(L, "draw.spriteimage: out of memory");
    }
    memcpy(sprite_pixels, indices, total * sizeof(int));
    free(data);
    free(indices);

    index = find_sprite_name(name);
    if (index < 0) {
        index = first_free_sprite();
        if (index < 0) {
            free(sprite_pixels);
            return luaL_error(L, "draw.spriteimage: too many sprites (max %d)",
                              SPRITE_MAX);
        }
        memcpy(g_sprites[index].name, name, name_len + 1);
        g_sprites[index].used = 1;
    } else {
        free(g_sprites[index].pixels);
    }
    g_sprites[index].pixels = sprite_pixels;
    g_sprites[index].width = width;
    g_sprites[index].height = height;

    lua_pushinteger(L, index);
    lua_pushinteger(L, first_palette);
    lua_pushinteger(L, color_count);
    return 3;
}

static int l_draw_sprite(lua_State *L)
{
    lua_Integer x, y;
    const char *name;
    int scale;
    int angle;
    int flipx;
    int flipy;
    int tr, tg, tb, ta;
    int index;
    int sx, sy;
    int sx0, sx1, sy0, sy1;
    int col, row;
    int value;
    uint32_t color;
    size_t i;
    size_t count;
    SpriteEntry *sprite;
    AnimEntry *anim = NULL;
    double W, H, bw, bh;
    double xd, yd;
    double pivot_x, pivot_y;
    double px, py;
    double min_x = 0.0, max_x = 0.0, min_y = 0.0, max_y = 0.0;
    double c, s, rad;
    double dx, dy, rot_x, rot_y, src_cx, src_cy, p, q;
    int k;
    int pivot_given;

    if (!ensure_open(L, "draw.draw"))
        return 0;

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    name = luaL_checkstring(L, 3);
    scale = parse_scale(L, 4);
    angle = parse_angle(L, 5);
    flipx = read_flip_field(L, 6, 1, "flip");
    flipy = read_flip_field(L, 6, 2, "flip");
    if (parse_tint(L, 7, &tr, &tg, &tb, &ta) != 0)
        return 0;

    index = find_sprite_name(name);
    if (index < 0) {
        int ai = find_anim_name(name);

        if (ai >= 0) {
            anim = &g_anims[ai];
            if (anim->count > 0) {
                Uint32 now = SDL_GetTicks();
                double elapsed = (double)(now - anim->started) / 1000.0;
                int frame = (int)(elapsed * anim->fps) % anim->count;

                name = anim->frames[frame];
                index = find_sprite_name(name);
            }
        }
    }
    if (index < 0)
        return luaL_error(L,
            "draw.draw: sprite or animation '%s' is not defined", name);

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

    W = (double)sprite->width * (double)scale;
    H = (double)sprite->height * (double)scale;
    rad = (double)angle * 0.017453292519943295769236907684886;
    c = cos(rad);
    s = sin(rad);
    if (fabs(c) < 1e-12)
        c = 0.0;
    if (fabs(s) < 1e-12)
        s = 0.0;

    pivot_given = !lua_isnoneornil(L, 8);
    if (pivot_given) {
        if (lua_type(L, 8) != LUA_TTABLE)
            return luaL_error(L, "draw.draw: pivot must be a table");
        pivot_x = read_pivot_field(L, 8, 1,
                                   (double)sprite->width / 2.0);
        pivot_y = read_pivot_field(L, 8, 2,
                                   (double)sprite->height / 2.0);
    } else {
        pivot_x = (double)sprite->width / 2.0;
        pivot_y = (double)sprite->height / 2.0;
    }
    if (!isfinite(pivot_x) || !isfinite(pivot_y) || pivot_x < -1000000.0 ||
        pivot_x > 1000000.0 || pivot_y < -1000000.0 ||
        pivot_y > 1000000.0)
        return luaL_error(L,
            "draw.draw: pivot must be finite and within +/-1000000");
    if (flipx)
        pivot_x = (double)sprite->width - pivot_x;
    if (flipy)
        pivot_y = (double)sprite->height - pivot_y;
    px = pivot_x * (double)scale;
    py = pivot_y * (double)scale;

    for (k = 0; k < 4; ++k) {
        double vx = (k & 1) ? W - px : -px;
        double vy = (k & 2) ? H - py : -py;
        double rx = vx * c - vy * s;
        double ry = vx * s + vy * c;

        if (k == 0) {
            min_x = max_x = rx;
            min_y = max_y = ry;
        } else {
            if (rx < min_x) min_x = rx;
            if (rx > max_x) max_x = rx;
            if (ry < min_y) min_y = ry;
            if (ry > max_y) max_y = ry;
        }
    }
    bw = max_x - min_x;
    bh = max_y - min_y;

    xd = (double)x;
    yd = (double)y;

    if (pivot_given) {
        if (!visible_range(xd + min_x, bw, g_draw.width, &sx0, &sx1) ||
            !visible_range(yd + min_y, bh, g_draw.height, &sy0, &sy1))
            return 0;
    } else {
        if (!visible_range(xd, bw, g_draw.width, &sx0, &sx1) ||
            !visible_range(yd, bh, g_draw.height, &sy0, &sy1))
            return 0;
    }

    for (sy = sy0; sy <= sy1; ++sy) {
        dy = (double)sy + 0.5 - yd;
        if (!pivot_given)
            dy += min_y;

        for (sx = sx0; sx <= sx1; ++sx) {
            dx = (double)sx + 0.5 - xd;
            if (!pivot_given)
                dx += min_x;
            rot_x = dx;
            rot_y = dy;
            src_cx = rot_x * c + rot_y * s;
            src_cy = -rot_x * s + rot_y * c;
            p = src_cx + px;
            q = src_cy + py;

            if (p < 0.0 || p >= W || q < 0.0 || q >= H)
                continue;

            col = (int)floor(p / scale + 1e-9);
            row = (int)floor(q / scale + 1e-9);
            if (flipx)
                col = sprite->width - 1 - col;
            if (flipy)
                row = sprite->height - 1 - row;
            if (col < 0 || col >= sprite->width ||
                row < 0 || row >= sprite->height)
                continue;

            value = sprite->pixels[(size_t)row * sprite->width + col];
            if (value < 0)
                continue;

            color = make_color(
                (g_palette[value].r * tr) / 255,
                (g_palette[value].g * tg) / 255,
                (g_palette[value].b * tb) / 255,
                (g_palette[value].a * ta) / 255);
            g_draw.pixels[(size_t)sy * g_draw.width + sx] =
                blend_color(g_draw.pixels[(size_t)sy * g_draw.width + sx],
                            color);
        }
    }

    mark_dirty_rect(sx0, sy0, sx1 - sx0 + 1, sy1 - sy0 + 1);
    return 0;
}

static int l_clear(lua_State *L)
{
    int r, g, b, a;
    uint32_t color;
    size_t count;
    size_t i;

    if (!ensure_open(L, "draw.clear"))
        return 0;

    if (read_color(L, 1, &r, &g, &b, &a) < 0) {
        r = 0;
        g = 0;
        b = 0;
    }
    color = make_color(r, g, b, a);

    count = (size_t)g_draw.width * (size_t)g_draw.height;
    for (i = 0; i < count; ++i)
        g_draw.pixels[i] = color;

    mark_dirty_rect(0, 0, g_draw.width, g_draw.height);
    return 0;
}

static int l_pixel(lua_State *L)
{
    lua_Integer x, y;
    int r, g, b, a;

    if (!ensure_open(L, "draw.pixel"))
        return 0;

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    if (read_color(L, 3, &r, &g, &b, &a) < 0)
        return luaL_error(L, "draw.pixel: expected a color");

    put_pixel((long long)x, (long long)y, make_color(r, g, b, a));
    return 0;
}

static int l_getpixel(lua_State *L)
{
    long long x, y;
    uint32_t color;

    if (!g_draw.initialized || !g_draw.open) {
        lua_pushnil(L);
        return 1;
    }

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    if (x < 0 || y < 0 || x >= g_draw.width || y >= g_draw.height) {
        lua_pushnil(L);
        return 1;
    }

    color = g_draw.pixels[(size_t)y * (size_t)g_draw.width + (size_t)x];
    lua_pushinteger(L, (int)((color >> 16) & 0xFF));
    lua_pushinteger(L, (int)((color >> 8) & 0xFF));
    lua_pushinteger(L, (int)(color & 0xFF));
    lua_pushinteger(L, (int)((color >> 24) & 0xFF));
    return 4;
}

static int l_width(lua_State *L)
{
    lua_pushinteger(L, g_draw.width);
    return 1;
}

static int l_height(lua_State *L)
{
    lua_pushinteger(L, g_draw.height);
    return 1;
}

static int l_title(lua_State *L)
{
    const char *title;

    if (lua_isnoneornil(L, 1)) {
        if (g_draw.window)
            lua_pushstring(L, SDL_GetWindowTitle(g_draw.window));
        else
            lua_pushnil(L);
        return 1;
    }

    title = luaL_checkstring(L, 1);
    if (g_draw.window)
        SDL_SetWindowTitle(g_draw.window, title);
    return 0;
}

static int l_delta(lua_State *L)
{
    lua_pushnumber(L, (lua_Number)g_draw.frame_delta);
    return 1;
}

static int l_alpha(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, g_draw.alpha);
        return 1;
    }

    g_draw.alpha = clamp_color(luaL_checkinteger(L, 1));
    return 0;
}

static int l_mouse(lua_State *L)
{
    lua_pushinteger(L, g_input.frame_mouse_dx);
    lua_pushinteger(L, g_input.frame_mouse_dy);
    return 2;
}

static int l_mouseposition(lua_State *L)
{
    const char *axis;

    if (lua_isnoneornil(L, 1)) {
        lua_pushinteger(L, g_input.mouse_x);
        lua_pushinteger(L, g_input.mouse_y);
        return 2;
    }

    axis = luaL_checkstring(L, 1);
    if (strcmp(axis, "x") == 0) {
        lua_pushinteger(L, g_input.mouse_x);
        return 1;
    }
    if (strcmp(axis, "y") == 0) {
        lua_pushinteger(L, g_input.mouse_y);
        return 1;
    }

    return luaL_error(L, "draw.mouseposition: expected 'x' or 'y'");
}

static int l_mousebutton(lua_State *L)
{
    if (g_input.mouse_button < 0) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, g_mouse_button_names[g_input.mouse_button]);
    return 1;
}

static int l_lastkey(lua_State *L)
{
    const char *name = key_name_for_code(g_input.last_key);

    if (name == NULL) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushstring(L, name);
    return 1;
}

static int l_key(lua_State *L)
{
    SDL_Scancode code = find_key_code(L, 1);

    lua_pushboolean(L, (int)code >= 0 && (int)code < SDL_NUM_SCANCODES &&
                        g_input.down[(int)code]);
    return 1;
}

static int l_keypress(lua_State *L)
{
    SDL_Scancode code = find_key_code(L, 1);

    if ((int)code >= 0 && (int)code < SDL_NUM_SCANCODES &&
        g_input.pressed[(int)code]) {
        g_input.pressed[(int)code] = 0;
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int l_mousepress(lua_State *L)
{
    int index = find_mouse_button(L, 1);

    if (g_input.mouse_pressed[index]) {
        g_input.mouse_pressed[index] = 0;
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int l_wheel(lua_State *L)
{
    lua_pushinteger(L, g_input.frame_wheel_dx);
    lua_pushinteger(L, g_input.frame_wheel_dy);
    return 2;
}

static int l_gamepadcount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < GAMEPAD_SLOTS; ++i) {
        if (g_gamepads[i].connected)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_gamepadname(lua_State *L)
{
    GamepadState *pad = &g_gamepads[gamepad_slot(L, 1, "gamepadname")];

    if (!pad->connected || pad->name[0] == '\0') {
        lua_pushnil(L);
        return 1;
    }
    lua_pushstring(L, pad->name);
    return 1;
}

static int l_gamepadbutton(lua_State *L)
{
    int button = gamepad_find_button(L, 1, "gamepadbutton");
    GamepadState *pad = &g_gamepads[gamepad_slot(L, 2, "gamepadbutton")];

    lua_pushboolean(L, pad->connected && button >= 0 &&
                       button < SDL_CONTROLLER_BUTTON_MAX &&
                       pad->down[button]);
    return 1;
}

static int l_gamepadpress(lua_State *L)
{
    int button = gamepad_find_button(L, 1, "gamepadpress");
    GamepadState *pad = &g_gamepads[gamepad_slot(L, 2, "gamepadpress")];

    if (pad->connected && button >= 0 && button < SDL_CONTROLLER_BUTTON_MAX &&
        pad->pressed[button]) {
        pad->pressed[button] = 0;
        lua_pushboolean(L, 1);
    } else {
        lua_pushboolean(L, 0);
    }
    return 1;
}

static int l_gamepadstick(lua_State *L)
{
    int axis = gamepad_find_axis(L, 1, "gamepadstick");
    GamepadState *pad = &g_gamepads[gamepad_slot(L, 2, "gamepadstick")];
    double value = 0.0;

    if (pad->connected && pad->controller && axis >= 0 &&
        axis < SDL_CONTROLLER_AXIS_MAX) {
        value = gamepad_normalize_axis(
            SDL_GameControllerGetAxis(pad->controller,
                                      (SDL_GameControllerAxis)axis),
            axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
            axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    }
    lua_pushnumber(L, value);
    return 1;
}

static int l_gamepadrumble(lua_State *L)
{
    lua_Number left = luaL_checknumber(L, 1);
    lua_Number right = luaL_checknumber(L, 2);
    lua_Integer duration = luaL_optinteger(L, 3, 200);
    GamepadState *pad = &g_gamepads[gamepad_slot(L, 4, "gamepadrumble")];

    if (!isfinite(left) || left < 0.0 || left > 1.0)
        return luaL_error(L, "draw.gamepadrumble: left must be 0..1");
    if (!isfinite(right) || right < 0.0 || right > 1.0)
        return luaL_error(L, "draw.gamepadrumble: right must be 0..1");
    if (duration < 0 || duration > 5000)
        return luaL_error(L, "draw.gamepadrumble: duration must be 0..5000");

    if (!pad->connected || !pad->controller) {
        lua_pushboolean(L, 0);
        return 1;
    }

    lua_pushboolean(L, SDL_GameControllerRumble(
        pad->controller,
        (Uint16)(left * 65535.0 + 0.5),
        (Uint16)(right * 65535.0 + 0.5),
        (Uint32)duration) == 0);
    return 1;
}

static int l_line(lua_State *L)
{
    lua_Integer x1, y1, x2, y2;
    int r, g, b, a;
    int fill;
    int width;

    if (!ensure_open(L, "draw.line"))
        return 0;

    x1 = luaL_checkinteger(L, 1);
    y1 = luaL_checkinteger(L, 2);
    x2 = luaL_checkinteger(L, 3);
    y2 = luaL_checkinteger(L, 4);

    if (x1 < -1000000000LL || x1 > 1000000000LL ||
        y1 < -1000000000LL || y1 > 1000000000LL ||
        x2 < -1000000000LL || x2 > 1000000000LL ||
        y2 < -1000000000LL || y2 > 1000000000LL)
        return luaL_error(L, "draw.line: coordinates are too large");

    if (read_color(L, 5, &r, &g, &b, &a) < 0)
        return luaL_error(L, "draw.line: expected a color");
    fill = read_fill(L, 5, "draw.line");
    width = fill <= 0 ? 1 : fill;

    draw_line((long long)x1, (long long)y1,
              (long long)x2, (long long)y2,
              width, make_color(r, g, b, a));
    return 0;
}

static int l_circle(lua_State *L)
{
    lua_Integer x, y, radius;
    int r, g, b, a;
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

    if (read_color(L, 4, &r, &g, &b, &a) < 0)
        return luaL_error(L, "draw.circle: expected a color");
    fill = read_fill(L, 4, "draw.circle");

    fill_circle((long long)x, (long long)y, (long long)radius,
                fill, make_color(r, g, b, a));
    return 0;
}

static int l_square(lua_State *L)
{
    lua_Integer x, y, w, h;
    int r, g, b, a;
    int fill;
    int angle;

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

    if (read_color(L, 5, &r, &g, &b, &a) < 0)
        return luaL_error(L, "draw.square: expected a color");
    fill = read_fill(L, 5, "draw.square");
    angle = parse_angle_named(L, 7, "draw.square");

    if (angle == 0) {
        fill_rect((long long)x, (long long)y, (long long)w, (long long)h,
                  fill, make_color(r, g, b, a));
        return 0;
    }

    if (w < 0) {
        x += w;
        w = -w;
    }
    if (h < 0) {
        y += h;
        h = -h;
    }
    if (w > 0 && h > 0)
        fill_rotated_rect((double)x, (double)y, (double)w, (double)h,
                          angle, fill, make_color(r, g, b, a));
    return 0;
}

static int l_triangle(lua_State *L)
{
    lua_Integer x1, y1, x2, y2, x3, y3;
    int r, g, b, a;
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

    if (read_color(L, 7, &r, &g, &b, &a) < 0)
        return luaL_error(L, "draw.triangle: expected a color");
    fill = read_fill(L, 7, "draw.triangle");

    fill_triangle((long long)x1, (long long)y1,
                  (long long)x2, (long long)y2,
                  (long long)x3, (long long)y3,
                  fill, make_color(r, g, b, a));
    return 0;
}

static int l_text(lua_State *L)
{
    int top;
    long long x, y;
    const char *text;
    size_t len;
    int font;
    int r, g, b, a;
    int size = 8;
    long long start_x;
    size_t i;
    uint32_t color;
    FontEntry *custom = NULL;
    int advance_x;
    int advance_y;
    static const int sizes[4] = {4, 8, 16, 24};

    if (!ensure_open(L, "draw.text"))
        return 0;

    top = lua_gettop(L);
    if (top < 3)
        return luaL_error(L, "draw.text: expected at least x, y, text");

    x = (long long)luaL_checkinteger(L, 1);
    y = (long long)luaL_checkinteger(L, 2);
    text = luaL_checklstring(L, 3, &len);

    if (lua_type(L, 4) == LUA_TSTRING) {
        int fi = find_font_name(lua_tostring(L, 4));

        if (fi < 0)
            return luaL_error(L, "draw.text: font '%s' is not defined",
                              lua_tostring(L, 4));
        custom = &g_fonts[fi];
    } else {
        font = (int)luaL_optinteger(L, 4, 1);
        if (font < 0)
            font = 0;
        if (font > 3)
            font = 3;
        size = sizes[font];
    }

    if (read_color(L, 5, &r, &g, &b, &a) < 0) {
        r = 255;
        g = 255;
        b = 255;
    }

    if (custom) {
        advance_x = custom->cell_w * custom->scale + 1;
        advance_y = custom->cell_h * custom->scale + 1;
    } else {
        advance_x = size + 1;
        advance_y = size + 1;
    }
    color = make_color(r, g, b, a);
    start_x = x;

    for (i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)text[i];
        const unsigned char *glyph;

        if (ch == '\n') {
            x = start_x;
            y += advance_y;
            continue;
        }
        if (ch == '\r')
            continue;

        if (custom) {
            int code = (int)ch - custom->first;
            int dx, dy;

            if (code < 0 || code >= custom->count)
                continue;
            glyph = custom->masks +
                    (size_t)code * custom->cell_h * custom->stride;
            for (dy = 0; dy < custom->cell_h * custom->scale; ++dy) {
                int sy = dy / custom->scale;

                for (dx = 0; dx < custom->cell_w * custom->scale; ++dx) {
                    int sx = dx / custom->scale;
                    int bit = (glyph[sy * custom->stride + sx / 8] >>
                               (7 - (sx % 8))) & 1;

                    if (bit)
                        put_pixel(x + dx, y + dy, color);
                }
            }
            x += advance_x;
            continue;
        }

        if (ch < 32 || ch > 127)
            continue;
        glyph = font8x8[ch];
        {
            int dx, dy;

            for (dy = 0; dy < size; ++dy) {
                for (dx = 0; dx < size; ++dx) {
                    if (glyph_pixel(glyph, dx, dy, size))
                        put_pixel(x + dx, y + dy, color);
                }
            }
        }
        x += advance_x;
    }

    return 0;
}

static int l_close(lua_State *L)
{
    (void)L;
    destroy_resources();
    reset_input();
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

static int l_melody(lua_State *L)
{
    const char *name;
    size_t len;
    MelodyNote *notes = NULL;
    MelodyEntry *melody;
    int count = 0;
    int cap = 0;
    int index;
    int i;

    if (!lua_istable(L, 2))
        return luaL_error(L, "draw.melody: expected a table of notes");

    name = luaL_checkstring(L, 1);
    len = strlen(name);
    if (len == 0)
        return luaL_error(L, "draw.melody: name must not be empty");
    if (len >= MELODY_NAME_MAX)
        return luaL_error(L, "draw.melody: name is too long");

    index = find_melody_name(name);
    if (index < 0) {
        index = first_free_melody();
        if (index < 0)
            return luaL_error(L, "draw.melody: too many melodies (max %d)",
                              MELODY_MAX);
    }

    for (i = 1;; ++i) {
        MelodyNote note;

        lua_pushinteger(L, i);
        lua_rawget(L, 2);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }

        if (lua_istable(L, -1)) {
            lua_pushinteger(L, 1);
            lua_rawget(L, -2);
            if (parse_note_scalar(L, -1, &note) != 0) {
                free(notes);
                return luaL_error(L,
                    "draw.melody: note %d must be a name, frequency or rest", i);
            }
            lua_pop(L, 1);

            lua_pushinteger(L, 2);
            lua_rawget(L, -2);
            note.beats = parse_beats(L, -1);
            if (note.beats < 0.0) {
                free(notes);
                return luaL_error(L,
                    "draw.melody: note %d duration must be 0..64 beats", i);
            }
            lua_pop(L, 1);
        } else if (parse_note_scalar(L, -1, &note) != 0) {
            free(notes);
            return luaL_error(L,
                "draw.melody: note %d must be a name, frequency or rest", i);
        }

        if (count == cap) {
            int new_cap = cap ? cap * 2 : 16;
            MelodyNote *tmp = (MelodyNote *)realloc(notes,
                (size_t)new_cap * sizeof(MelodyNote));

            if (!tmp) {
                free(notes);
                return luaL_error(L, "draw.melody: out of memory");
            }
            notes = tmp;
            cap = new_cap;
        }
        notes[count++] = note;
        lua_pop(L, 1);
    }

    melody = &g_melodies[index];
    free_melody(melody);
    memcpy(melody->name, name, len + 1);
    melody->used = 1;
    melody->kind = AUDIO_KIND_MELODY;
    melody->notes = notes;
    melody->count = count;

    lua_pushinteger(L, index + 1);
    return 1;
}

static int l_midi(lua_State *L)
{
    const char *name;
    const char *path;
    size_t name_len;
    lua_Integer track_opt;
    unsigned char *data = NULL;
    size_t size = 0;
    const unsigned char *p;
    const unsigned char *end;
    uint32_t header_len;
    uint16_t track_count;
    uint16_t ppq;
    MidiNote *chosen_midi = NULL;
    MelodyNote *notes = NULL;
    int chosen_count = 0;
    int note_count = 0;
    unsigned long chosen_tempo = 0;
    int chosen = 0;
    int index;
    int t;
    int rc;
    const char *msg = NULL;
    char msgbuf[128];
    double speed;

    name = luaL_checkstring(L, 1);
    path = luaL_checkstring(L, 2);
    track_opt = luaL_optinteger(L, 3, 0);
    name_len = strlen(name);
    if (name_len == 0) {
        msg = "draw.midi: name must not be empty";
        goto fail;
    }
    if (name_len >= MELODY_NAME_MAX) {
        msg = "draw.midi: name is too long";
        goto fail;
    }
    if (track_opt < 0) {
        msg = "draw.midi: track must be positive or zero";
        goto fail;
    }

    rc = read_file_bytes(path, &data, &size);
    if (rc == -2) {
        msg = "draw.midi: out of memory";
        goto fail;
    }
    if (rc != 0) {
        snprintf(msgbuf, sizeof(msgbuf),
                 "draw.midi: cannot read '%s'", path);
        msg = msgbuf;
        goto fail;
    }
    if (size < 14 || memcmp(data, "MThd", 4) != 0) {
        msg = "draw.midi: invalid SMF header";
        goto fail;
    }
    header_len = midi_be32(data + 4);
    if (header_len < 6 || header_len > size - 8) {
        msg = "draw.midi: invalid SMF header length";
        goto fail;
    }
    track_count = midi_be16(data + 10);
    ppq = midi_be16(data + 12);
    if (ppq == 0 || ppq >= 0x8000) {
        msg = "draw.midi: only PPQN division is supported";
        goto fail;
    }
    if (track_opt > track_count) {
        msg = "draw.midi: track is out of range";
        goto fail;
    }

    p = data + 8 + header_len;
    end = data + size;
    for (t = 1; t <= track_count; ++t) {
        uint32_t chunk_size;
        MidiNote *track_notes = NULL;
        int track_count_notes = 0;
        unsigned long track_tempo = 0;

        if ((size_t)(end - p) < 8) {
            msg = "draw.midi: truncated track";
            goto fail;
        }
        chunk_size = midi_be32(p + 4);
        if (memcmp(p, "MTrk", 4) != 0 || chunk_size > (size_t)(end - p - 8)) {
            msg = "draw.midi: invalid track chunk";
            goto fail;
        }
        p += 8;

        rc = midi_parse_track(p, chunk_size, ppq, &track_notes,
                              &track_count_notes, &track_tempo);
        if (rc == -2) {
            msg = "draw.midi: out of memory";
            goto fail;
        }
        if (rc != 0) {
            msg = "draw.midi: cannot parse track";
            goto fail;
        }
        p += chunk_size;

        if (track_opt > 0 && t == track_opt) {
            chosen_midi = track_notes;
            chosen_count = track_count_notes;
            chosen_tempo = track_tempo;
            chosen = 1;
            break;
        }
        if (track_opt == 0 && !chosen && track_count_notes > 0) {
            chosen_midi = track_notes;
            chosen_count = track_count_notes;
            chosen_tempo = track_tempo;
            chosen = 1;
        } else {
            free(track_notes);
        }
    }

    if (!chosen || chosen_count == 0) {
        msg = "draw.midi: no notes found";
        goto fail;
    }

    rc = midi_to_melody(chosen_midi, chosen_count, ppq, &notes, &note_count);
    if (rc == -2) {
        msg = "draw.midi: out of memory";
        goto fail;
    }
    if (note_count == 0) {
        msg = "draw.midi: no notes found";
        goto fail;
    }

    index = find_melody_name(name);
    if (index < 0) {
        index = first_free_melody();
        if (index < 0) {
            msg = "draw.midi: too many melodies (max 1024)";
            goto fail;
        }
        memcpy(g_melodies[index].name, name, name_len + 1);
        g_melodies[index].used = 1;
    }

    {
        MelodyEntry *entry = &g_melodies[index];

        free_melody(entry);
        entry->kind = AUDIO_KIND_MELODY;
        entry->notes = notes;
        entry->count = note_count;
        notes = NULL;
    }

    speed = chosen_tempo ? 60000000.0 / (double)chosen_tempo : 4.0;
    if (speed < 0.1)
        speed = 0.1;
    if (speed > 64.0)
        speed = 64.0;

    free(chosen_midi);
    free(notes);
    free(data);
    lua_pushinteger(L, index + 1);
    lua_pushnumber(L, speed);
    return 2;

fail:
    free(chosen_midi);
    free(notes);
    free(data);
    if (msg)
        return luaL_error(L, "%s", msg);
    return luaL_error(L, "draw.midi: failed");
}

static int l_sound(lua_State *L)
{
    const char *name;
    const char *path;
    size_t len;
    MelodyEntry loaded;
    MelodyEntry *entry;
    int index;
    int rc;

    name = luaL_checkstring(L, 1);
    path = luaL_checkstring(L, 2);
    len = strlen(name);
    if (len == 0)
        return luaL_error(L, "draw.sound: name must not be empty");
    if (len >= MELODY_NAME_MAX)
        return luaL_error(L, "draw.sound: name is too long");

    memset(&loaded, 0, sizeof(loaded));
    rc = load_wav(path, &loaded);
    if (rc == -2)
        return luaL_error(L, "draw.sound: out of memory");
    if (rc != 0) {
        free_melody(&loaded);
        return luaL_error(L, "draw.sound: cannot load '%s'", path);
    }

    index = find_melody_name(name);
    if (index < 0) {
        index = first_free_melody();
        if (index < 0) {
            free_melody(&loaded);
            return luaL_error(L, "draw.sound: too many sounds (max %d)",
                              MELODY_MAX);
        }
        memcpy(g_melodies[index].name, name, len + 1);
        g_melodies[index].used = 1;
    }

    entry = &g_melodies[index];
    free_melody(entry);
    entry->kind = loaded.kind;
    entry->samples = loaded.samples;
    entry->sample_count = loaded.sample_count;
    entry->sample_rate = loaded.sample_rate;

    lua_pushinteger(L, index + 1);
    return 1;
}

static int l_play(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int index = find_melody_name(name);
    MelodyEntry *entry;
    AudioVoice *voice = NULL;
    MelodyNote *notes = NULL;
    int16_t *samples = NULL;
    size_t bytes;
    int volume;
    double speed;
    int wave;
    int i;

    if (index < 0)
        return luaL_error(L, "draw.play: sound '%s' is not defined", name);
    entry = &g_melodies[index];

    volume = clamp_color((int)luaL_optinteger(L, 2, 128));
    speed = luaL_optnumber(L, 3,
        entry->kind == AUDIO_KIND_WAV ? 1.0 : 4.0);
    wave = parse_wave(L, 4);
    if (wave < 0)
        return luaL_error(L, "draw.play: unknown wave");
    if (speed < 0.01 || speed > 64.0)
        return luaL_error(L, "draw.play: speed must be 0.01..64");
    if (ensure_audio_device(L) != 0)
        return 0;

    if (entry->kind == AUDIO_KIND_MELODY) {
        bytes = (size_t)entry->count * sizeof(MelodyNote);
        notes = (MelodyNote *)malloc(bytes ? bytes : 1);
        if (!notes)
            return luaL_error(L, "draw.play: out of memory");
        if (entry->count > 0)
            memcpy(notes, entry->notes, bytes);
    } else {
        bytes = (size_t)entry->sample_count * sizeof(int16_t);
        samples = (int16_t *)malloc(bytes ? bytes : 1);
        if (!samples) {
            free(notes);
            return luaL_error(L, "draw.play: out of memory");
        }
        if (entry->sample_count > 0)
            memcpy(samples, entry->samples, bytes);
    }

    SDL_LockMutex(g_audio.mutex);
    for (i = 0; i < AUDIO_VOICES_MAX; ++i) {
        if (!g_audio.voices[i].playing) {
            voice = &g_audio.voices[i];
            break;
        }
    }
    if (!voice)
        voice = &g_audio.voices[0];
    audio_clear_voice(voice);

    voice->kind = entry->kind;
    voice->notes = notes;
    voice->count = entry->count;
    voice->samples = samples;
    voice->sample_count = entry->sample_count;
    voice->sample_rate = entry->sample_rate;
    voice->volume = volume;
    voice->speed = speed;
    voice->wave = wave;
    voice->playing = entry->kind == AUDIO_KIND_WAV
                         ? entry->sample_count > 0
                         : entry->count > 0;
    if (entry->kind == AUDIO_KIND_MELODY)
        audio_set_note(voice, 0, g_audio.sample_rate);
    SDL_UnlockMutex(g_audio.mutex);

    SDL_PauseAudioDevice(g_audio.device, 0);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stop(lua_State *L)
{
    (void)L;
    audio_stop();
    return 0;
}

#define COLLISION_COORD_LIMIT 1000000000LL
#define COLLISION_RADIUS_LIMIT 3037000499LL
#define COLLISION_POINT_LIMIT 1000000
#define COLLISION_AREA_LIMIT 4000000LL

typedef enum {
    COLLISION_POINT = 0,
    COLLISION_CIRCLE,
    COLLISION_SQUARE,
    COLLISION_TRIANGLE,
    COLLISION_SPRITE
} CollisionKind;

typedef struct {
    CollisionKind kind;
    long long x, y;
    long long x2, y2;
    long long x3, y3;
    long long radius;
    long long width, height;
    long long area2;
    int sprite_index;
    int scale, angle;
    int flipx, flipy;
    int tr, tg, tb, ta;
    double pivot_x, pivot_y;
    int pivot_given;
    double rot_min_x, rot_max_x, rot_min_y, rot_max_y;
    double px, py, c, s;
} CollisionShape;

typedef long long CollisionPoint;

typedef struct {
    CollisionPoint *data;
    size_t count;
    size_t cap;
} PointBuf;

static int collision_abs(int top, int index)
{
    return index > 0 ? index : top + index + 1;
}

static int collision_get_field(lua_State *L, int table, const char *key)
{
    lua_pushstring(L, key);
    lua_rawget(L, table);
    return lua_gettop(L);
}

static int collision_table_field(lua_State *L, int table, const char *key)
{
    int index = collision_get_field(L, table, key);

    if (!lua_istable(L, index)) {
        lua_pop(L, 1);
        return 0;
    }
    return collision_abs(lua_gettop(L), index);
}

static long long collision_int_value(lua_State *L, lua_Number n,
                                     const char *path, long long min,
                                     long long max)
{
    long long value = (long long)n;

    if (!isfinite(n) || (lua_Number)value != n || value < min || value > max)
        luaL_error(L, "draw.collision: %s must be an integer within range",
                   path);
    return value;
}

static long long collision_req_int(lua_State *L, int table, const char *key,
                                   const char *path, long long min,
                                   long long max)
{
    int index = collision_get_field(L, table, key);
    long long value;

    if (!lua_isnumber(L, index))
        luaL_error(L, "draw.collision: %s must be a number", path);
    value = collision_int_value(L, lua_tonumber(L, index), path, min, max);
    lua_pop(L, 1);
    return value;
}

static long long collision_opt_int(lua_State *L, int table, const char *key,
                                   long long default_value, const char *path,
                                   long long min, long long max)
{
    int index = collision_get_field(L, table, key);
    long long value = default_value;

    if (!lua_isnil(L, index))
        value = collision_int_value(L, lua_tonumber(L, index), path, min, max);
    lua_pop(L, 1);
    return value;
}

static int collision_opt_bool(lua_State *L, int table, const char *key,
                              const char *path)
{
    int index = collision_get_field(L, table, key);
    int value = 0;

    if (!lua_isnil(L, index)) {
        if (!lua_isboolean(L, index))
            luaL_error(L, "draw.collision: %s must be true or false", path);
        value = lua_toboolean(L, index);
    }
    lua_pop(L, 1);
    return value;
}

static int collision_opt_index_bool(lua_State *L, int table, int key,
                                    const char *path)
{
    lua_pushinteger(L, key);
    lua_rawget(L, table);
    {
        int index = lua_gettop(L);
        int value = 0;

        if (!lua_isnil(L, index)) {
            if (!lua_isboolean(L, index))
                luaL_error(L, "draw.collision: %s must be true or false", path);
            value = lua_toboolean(L, index);
        }
        lua_pop(L, 1);
        return value;
    }
}

static double collision_opt_index_double(lua_State *L, int table, int key,
                                         double default_value,
                                         const char *path)
{
    lua_pushinteger(L, key);
    lua_rawget(L, table);
    {
        int index = lua_gettop(L);
        double value = default_value;

        if (!lua_isnil(L, index)) {
            if (!lua_isnumber(L, index))
                luaL_error(L, "draw.collision: %s must be a number", path);
            value = (double)lua_tonumber(L, index);
            if (!isfinite(value))
                luaL_error(L, "draw.collision: %s must be finite", path);
        }
        lua_pop(L, 1);
        return value;
    }
}

static const char *collision_req_string(lua_State *L, int table,
                                        const char *key, const char *path)
{
    int index = collision_get_field(L, table, key);
    const char *value;

    if (lua_type(L, index) != LUA_TSTRING)
        luaL_error(L, "draw.collision: %s must be a string", path);
    value = lua_tostring(L, index);
    lua_pop(L, 1);
    return value;
}

static int collision_sprite_index(lua_State *L, const char *name)
{
    int index = find_sprite_name(name);

    if (index < 0) {
        int ai = find_anim_name(name);

        if (ai >= 0 && g_anims[ai].count > 0) {
            Uint32 now = SDL_GetTicks();
            double elapsed = (double)(now - g_anims[ai].started) / 1000.0;
            int frame = (int)(elapsed * g_anims[ai].fps) % g_anims[ai].count;

            name = g_anims[ai].frames[frame];
            index = find_sprite_name(name);
        }
    }
    if (index < 0)
        luaL_error(L, "draw.collision: sprite or animation '%s' is not defined",
                   name);
    return index;
}

static void collision_read_tint(lua_State *L, int table, int *r, int *g,
                                int *b, int *a)
{
    int index = collision_get_field(L, table, "tint");

    if (lua_isnil(L, index)) {
        *r = 255;
        *g = 255;
        *b = 255;
        *a = 255;
        lua_pop(L, 1);
        return;
    }
    read_color(L, index, r, g, b, a);
    lua_pop(L, 1);
}

static void collision_sprite_geometry(CollisionShape *out)
{
    const SpriteEntry *sprite = &g_sprites[out->sprite_index];
    double W, H, rad;
    int k;

    out->px = out->pivot_x * (double)out->scale;
    out->py = out->pivot_y * (double)out->scale;
    W = (double)sprite->width * (double)out->scale;
    H = (double)sprite->height * (double)out->scale;
    rad = (double)out->angle * 0.017453292519943295769236907684886;
    out->c = cos(rad);
    out->s = sin(rad);
    if (fabs(out->c) < 1e-12)
        out->c = 0.0;
    if (fabs(out->s) < 1e-12)
        out->s = 0.0;

    for (k = 0; k < 4; ++k) {
        double vx = (k & 1) ? W - out->px : -out->px;
        double vy = (k & 2) ? H - out->py : -out->py;
        double rx = vx * out->c - vy * out->s;
        double ry = vx * out->s + vy * out->c;

        if (k == 0) {
            out->rot_min_x = out->rot_max_x = rx;
            out->rot_min_y = out->rot_max_y = ry;
        } else {
            if (rx < out->rot_min_x) out->rot_min_x = rx;
            if (rx > out->rot_max_x) out->rot_max_x = rx;
            if (ry < out->rot_min_y) out->rot_min_y = ry;
            if (ry > out->rot_max_y) out->rot_max_y = ry;
        }
    }
}

static void collision_parse_sprite(lua_State *L, int table, CollisionShape *out)
{
    const char *name;
    SpriteEntry *sprite;
    int pivot_table;
    int flip_table;
    int angle;
    int scale;
    size_t i;
    size_t count;
    double pivot_x, pivot_y;

    name = collision_req_string(L, table, "sprite", "a.sprite.name");
    out->x = collision_req_int(L, table, "x", "a.sprite.x",
                               -COLLISION_COORD_LIMIT, COLLISION_COORD_LIMIT);
    out->y = collision_req_int(L, table, "y", "a.sprite.y",
                               -COLLISION_COORD_LIMIT, COLLISION_COORD_LIMIT);
    scale = (int)collision_opt_int(L, table, "scale", 1, "a.sprite.scale",
                                   1, SPRITE_SCALE_MAX);
    angle = (int)collision_opt_int(L, table, "angle", 0, "a.sprite.angle",
                                   -1000000, 1000000);
    angle %= 360;
    if (angle < 0)
        angle += 360;
    out->scale = scale;
    out->angle = angle;

    flip_table = collision_table_field(L, table, "flip");
    if (flip_table) {
        out->flipx = collision_opt_index_bool(L, flip_table, 1,
                                              "a.sprite.flip[1]");
        out->flipy = collision_opt_index_bool(L, flip_table, 2,
                                              "a.sprite.flip[2]");
        lua_pop(L, 1);
    } else {
        out->flipx = collision_opt_bool(L, table, "flipx", "a.sprite.flipx");
        out->flipy = collision_opt_bool(L, table, "flipy", "a.sprite.flipy");
    }
    collision_read_tint(L, table, &out->tr, &out->tg, &out->tb, &out->ta);

    out->sprite_index = collision_sprite_index(L, name);
    sprite = &g_sprites[out->sprite_index];
    count = (size_t)sprite->width * (size_t)sprite->height;
    for (i = 0; i < count; ++i) {
        int value = sprite->pixels[i];

        if (value >= 0 &&
            (value >= g_palette_count || !g_palette[value].used))
            luaL_error(L, "draw.collision: palette index %d is not defined",
                       value);
    }

    pivot_table = collision_table_field(L, table, "pivot");
    out->pivot_given = pivot_table != 0;
    pivot_x = (double)sprite->width / 2.0;
    pivot_y = (double)sprite->height / 2.0;
    if (pivot_table) {
        pivot_x = collision_opt_index_double(L, pivot_table, 1, pivot_x,
                                             "a.sprite.pivot[1]");
        pivot_y = collision_opt_index_double(L, pivot_table, 2, pivot_y,
                                             "a.sprite.pivot[2]");
        lua_pop(L, 1);
    }
    if (!isfinite(pivot_x) || !isfinite(pivot_y) || pivot_x < -1000000.0 ||
        pivot_x > 1000000.0 || pivot_y < -1000000.0 || pivot_y > 1000000.0)
        luaL_error(L,
                   "draw.collision: pivot must be finite and within +/-1000000");
    if (out->flipx)
        pivot_x = (double)sprite->width - pivot_x;
    if (out->flipy)
        pivot_y = (double)sprite->height - pivot_y;
    out->pivot_x = pivot_x;
    out->pivot_y = pivot_y;
    collision_sprite_geometry(out);
}

static int collision_parse_shape(lua_State *L, int index, CollisionShape *out)
{
    int table;
    int sub;

    memset(out, 0, sizeof(*out));
    table = collision_abs(lua_gettop(L), index);
    if (!lua_istable(L, table))
        return luaL_error(L, "draw.collision: arguments must be tables");

    sub = collision_table_field(L, table, "sprite");
    if (lua_type(L, collision_get_field(L, table, "sprite")) == LUA_TSTRING) {
        lua_pop(L, 1);
        out->kind = COLLISION_SPRITE;
        collision_parse_sprite(L, table, out);
        return 0;
    }
    lua_pop(L, 1);
    if (sub) {
        lua_pop(L, 1);
        out->kind = COLLISION_SPRITE;
        collision_parse_sprite(L, sub, out);
        return 0;
    }

    sub = collision_table_field(L, table, "point");
    if (sub) {
        out->kind = COLLISION_POINT;
        out->x = collision_req_int(L, sub, "x", "a.point.x",
                                   -COLLISION_COORD_LIMIT,
                                   COLLISION_COORD_LIMIT);
        out->y = collision_req_int(L, sub, "y", "a.point.y",
                                   -COLLISION_COORD_LIMIT,
                                   COLLISION_COORD_LIMIT);
        lua_pop(L, 1);
        return 0;
    }

    sub = collision_table_field(L, table, "circle");
    if (sub) {
        out->kind = COLLISION_CIRCLE;
        out->x = collision_req_int(L, sub, "x", "a.circle.x",
                                   -COLLISION_COORD_LIMIT,
                                   COLLISION_COORD_LIMIT);
        out->y = collision_req_int(L, sub, "y", "a.circle.y",
                                   -COLLISION_COORD_LIMIT,
                                   COLLISION_COORD_LIMIT);
        out->radius = collision_req_int(L, sub, "radius", "a.circle.radius",
                                        0, COLLISION_RADIUS_LIMIT);
        lua_pop(L, 1);
        return 0;
    }

    sub = collision_table_field(L, table, "square");
    if (sub) {
        long long raw_angle;
        double rad;

        out->kind = COLLISION_SQUARE;
        out->x = collision_req_int(L, sub, "x", "a.square.x",
                                   -COLLISION_COORD_LIMIT,
                                   COLLISION_COORD_LIMIT);
        out->y = collision_req_int(L, sub, "y", "a.square.y",
                                   -COLLISION_COORD_LIMIT,
                                   COLLISION_COORD_LIMIT);
        out->width = collision_req_int(L, sub, "width", "a.square.width",
                                       0, COLLISION_COORD_LIMIT);
        out->height = collision_req_int(L, sub, "height", "a.square.height",
                                        0, COLLISION_COORD_LIMIT);
        raw_angle = collision_opt_int(L, sub, "angle", 0, "a.square.angle",
                                      -1000000, 1000000);
        raw_angle %= 360;
        if (raw_angle < 0)
            raw_angle += 360;
        out->angle = (int)raw_angle;
        rad = (double)out->angle * 0.017453292519943295769236907684886;
        out->c = cos(rad);
        out->s = sin(rad);
        if (fabs(out->c) < 1e-12)
            out->c = 0.0;
        if (fabs(out->s) < 1e-12)
            out->s = 0.0;
        lua_pop(L, 1);
        return 0;
    }

    sub = collision_table_field(L, table, "triangle");
    if (sub) {
        out->kind = COLLISION_TRIANGLE;
        out->x = collision_req_int(L, sub, "x1", "a.triangle.x1",
                                   -TRIANGLE_COORD_LIMIT, TRIANGLE_COORD_LIMIT);
        out->y = collision_req_int(L, sub, "y1", "a.triangle.y1",
                                   -TRIANGLE_COORD_LIMIT, TRIANGLE_COORD_LIMIT);
        out->x2 = collision_req_int(L, sub, "x2", "a.triangle.x2",
                                    -TRIANGLE_COORD_LIMIT, TRIANGLE_COORD_LIMIT);
        out->y2 = collision_req_int(L, sub, "y2", "a.triangle.y2",
                                    -TRIANGLE_COORD_LIMIT, TRIANGLE_COORD_LIMIT);
        out->x3 = collision_req_int(L, sub, "x3", "a.triangle.x3",
                                    -TRIANGLE_COORD_LIMIT, TRIANGLE_COORD_LIMIT);
        out->y3 = collision_req_int(L, sub, "y3", "a.triangle.y3",
                                    -TRIANGLE_COORD_LIMIT, TRIANGLE_COORD_LIMIT);
        out->area2 = (out->x2 - out->x) * (out->y3 - out->y) -
                     (out->y2 - out->y) * (out->x3 - out->x);
        lua_pop(L, 1);
        return 0;
    }

    return luaL_error(L,
        "draw.collision: expected sprite, point, circle, square, or triangle");
}

static void collision_bbox(const CollisionShape *shape, long long *x0,
                           long long *y0, long long *x1, long long *y1)
{
    long long minx, miny, maxx, maxy;

    switch (shape->kind) {
    case COLLISION_POINT:
        minx = maxx = shape->x;
        miny = maxy = shape->y;
        break;
    case COLLISION_CIRCLE:
        minx = shape->x - shape->radius;
        maxx = shape->x + shape->radius;
        miny = shape->y - shape->radius;
        maxy = shape->y + shape->radius;
        break;
    case COLLISION_SQUARE:
        if (shape->angle == 0) {
            minx = shape->x;
            maxx = shape->x + shape->width - 1;
            miny = shape->y;
            maxy = shape->y + shape->height - 1;
        } else {
            double cx = (double)shape->x + (double)shape->width / 2.0;
            double cy = (double)shape->y + (double)shape->height / 2.0;
            double hw = (double)shape->width / 2.0;
            double hh = (double)shape->height / 2.0;
            double lox, loy, rx, ry;
            int k;

            minx = maxx = 0.0;
            miny = maxy = 0.0;
            for (k = 0; k < 4; ++k) {
                lox = (k & 1) ? hw : -hw;
                loy = (k & 2) ? hh : -hh;
                rx = lox * shape->c - loy * shape->s;
                ry = lox * shape->s + loy * shape->c;
                if (k == 0) {
                    minx = maxx = rx;
                    miny = maxy = ry;
                } else {
                    if (rx < minx) minx = rx;
                    if (rx > maxx) maxx = rx;
                    if (ry < miny) miny = ry;
                    if (ry > maxy) maxy = ry;
                }
            }
            minx = (long long)floor(cx + minx) - 1;
            maxx = (long long)ceil(cx + maxx) + 1;
            miny = (long long)floor(cy + miny) - 1;
            maxy = (long long)ceil(cy + maxy) + 1;
        }
        break;
    case COLLISION_TRIANGLE:
        minx = shape->x;
        maxx = shape->x;
        if (shape->x2 < minx) minx = shape->x2;
        if (shape->x2 > maxx) maxx = shape->x2;
        if (shape->x3 < minx) minx = shape->x3;
        if (shape->x3 > maxx) maxx = shape->x3;
        miny = shape->y;
        maxy = shape->y;
        if (shape->y2 < miny) miny = shape->y2;
        if (shape->y2 > maxy) maxy = shape->y2;
        if (shape->y3 < miny) miny = shape->y3;
        if (shape->y3 > maxy) maxy = shape->y3;
        break;
    default:
        minx = (long long)floor((double)shape->x +
                                (shape->pivot_given ? shape->rot_min_x : 0.0)) - 1;
        maxx = (long long)ceil((double)shape->x +
                               (shape->pivot_given
                                    ? shape->rot_max_x
                                    : shape->rot_max_x - shape->rot_min_x)) + 1;
        miny = (long long)floor((double)shape->y +
                                (shape->pivot_given ? shape->rot_min_y : 0.0)) - 1;
        maxy = (long long)ceil((double)shape->y +
                               (shape->pivot_given
                                    ? shape->rot_max_y
                                    : shape->rot_max_y - shape->rot_min_y)) + 1;
        break;
    }

    *x0 = minx;
    *y0 = miny;
    *x1 = maxx;
    *y1 = maxy;
}

static int collision_shape_contains(const CollisionShape *shape,
                                    long long x, long long y)
{
    long long px2, py2, e1, e2, e3;

    switch (shape->kind) {
    case COLLISION_POINT:
        return x == shape->x && y == shape->y;
    case COLLISION_CIRCLE: {
        __int128 dx = (__int128)x - (__int128)shape->x;
        __int128 dy = (__int128)y - (__int128)shape->y;
        __int128 r = (__int128)shape->radius;
        return dx * dx + dy * dy <= r * r;
    }
    case COLLISION_SQUARE:
        if (shape->angle == 0)
            return x >= shape->x && x < shape->x + shape->width &&
                   y >= shape->y && y < shape->y + shape->height;
        else {
            double cx = (double)shape->x + (double)shape->width / 2.0;
            double cy = (double)shape->y + (double)shape->height / 2.0;
            double hw = (double)shape->width / 2.0;
            double hh = (double)shape->height / 2.0;
            double dx = (double)x + 0.5 - cx;
            double dy = (double)y + 0.5 - cy;
            double lx = dx * shape->c + dy * shape->s;
            double ly = -dx * shape->s + dy * shape->c;

            return fabs(lx) <= hw && fabs(ly) <= hh;
        }
    case COLLISION_TRIANGLE:
        if (shape->area2 == 0)
            return 0;
        px2 = 2 * x + 1;
        py2 = 2 * y + 1;
        e1 = triangle_edge(shape->x, shape->y, shape->x2, shape->y2, px2, py2);
        e2 = triangle_edge(shape->x2, shape->y2, shape->x3, shape->y3, px2, py2);
        e3 = triangle_edge(shape->x3, shape->y3, shape->x, shape->y, px2, py2);
        return shape->area2 > 0
                   ? (e1 >= 0 && e2 >= 0 && e3 >= 0)
                   : (e1 <= 0 && e2 <= 0 && e3 <= 0);
    default:
        return 0;
    }
}

static int collision_area_ok(lua_State *L, long long width, long long height)
{
    if (width <= 0 || height <= 0)
        return 0;
    if (width > COLLISION_AREA_LIMIT || height > COLLISION_AREA_LIMIT ||
        width * height > COLLISION_AREA_LIMIT)
        return luaL_error(L, "draw.collision: intersection area is too large");
    return 1;
}

static int collision_intersection(lua_State *L, const CollisionShape *a,
                                  const CollisionShape *b, long long *x0,
                                  long long *y0, long long *x1, long long *y1)
{
    long long ax0, ay0, ax1, ay1;
    long long bx0, by0, bx1, by1;

    collision_bbox(a, &ax0, &ay0, &ax1, &ay1);
    collision_bbox(b, &bx0, &by0, &bx1, &by1);
    *x0 = ax0 > bx0 ? ax0 : bx0;
    *y0 = ay0 > by0 ? ay0 : by0;
    *x1 = ax1 < bx1 ? ax1 : bx1;
    *y1 = ay1 < by1 ? ay1 : by1;
    if (!collision_area_ok(L, *x1 - *x0 + 1, *y1 - *y0 + 1))
        return 0;
    return 1;
}

static CollisionPoint collision_point_key(long long x, long long y)
{
    return (long long)(((uint64_t)(uint32_t)(int32_t)x << 32) |
                       (uint32_t)(int32_t)y);
}

static long long collision_key_x(CollisionPoint key)
{
    return (long long)(int32_t)(uint32_t)((uint64_t)key >> 32);
}

static long long collision_key_y(CollisionPoint key)
{
    return (long long)(int32_t)(uint32_t)key;
}

static int collision_point_cmp(long long ax, long long ay, long long bx,
                               long long by)
{
    if (ax < bx) return -1;
    if (ax > bx) return 1;
    if (ay < by) return -1;
    if (ay > by) return 1;
    return 0;
}

static int collision_point_qcmp(const void *pa, const void *pb)
{
    CollisionPoint a = *(const CollisionPoint *)pa;
    CollisionPoint b = *(const CollisionPoint *)pb;

    return collision_point_cmp(collision_key_x(a), collision_key_y(a),
                               collision_key_x(b), collision_key_y(b));
}

static int collision_point_lookup(const PointBuf *buf, long long x, long long y)
{
    size_t lo = 0;
    size_t hi = buf->count;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = collision_point_cmp(collision_key_x(buf->data[mid]),
                                      collision_key_y(buf->data[mid]), x, y);

        if (cmp == 0)
            return 1;
        if (cmp < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

static int collision_buf_init(PointBuf *buf)
{
    buf->data = (CollisionPoint *)malloc(1024 * sizeof(CollisionPoint));
    buf->count = 0;
    buf->cap = buf->data ? 1024 : 0;
    return buf->data ? 0 : -1;
}

static int collision_buf_push(PointBuf *buf, long long x, long long y)
{
    CollisionPoint *data;

    if (buf->count >= COLLISION_POINT_LIMIT)
        return -1;
    if (buf->count == buf->cap) {
        size_t cap = buf->cap ? buf->cap * 2 : 1024;

        if (cap > COLLISION_POINT_LIMIT)
            cap = COLLISION_POINT_LIMIT;
        data = (CollisionPoint *)realloc(buf->data,
                                         cap * sizeof(CollisionPoint));
        if (!data)
            return -2;
        buf->data = data;
        buf->cap = cap;
    }
    buf->data[buf->count++] = collision_point_key(x, y);
    return 0;
}

static int collision_rasterize_sprite(const CollisionShape *shape,
                                      long long clip_x0, long long clip_y0,
                                      long long clip_x1, long long clip_y1,
                                      PointBuf *buf)
{
    const SpriteEntry *sprite = &g_sprites[shape->sprite_index];
    long long sx0, sx1, sy0, sy1, sx, sy;
    double W, H;
    double base_x, base_y;
    double span_x, span_y;
    int global_alpha = g_draw.initialized ? g_draw.alpha : 255;

    W = (double)sprite->width * (double)shape->scale;
    H = (double)sprite->height * (double)shape->scale;
    base_x = (double)shape->x +
             (shape->pivot_given ? shape->rot_min_x : 0.0);
    base_y = (double)shape->y +
             (shape->pivot_given ? shape->rot_min_y : 0.0);
    span_x = shape->rot_max_x - shape->rot_min_x;
    span_y = shape->rot_max_y - shape->rot_min_y;
    sx0 = (long long)ceil(base_x);
    sx1 = (long long)floor(base_x + span_x - 1.0);
    sy0 = (long long)ceil(base_y);
    sy1 = (long long)floor(base_y + span_y - 1.0);
    if (sx0 < clip_x0) sx0 = clip_x0;
    if (sy0 < clip_y0) sy0 = clip_y0;
    if (sx1 > clip_x1) sx1 = clip_x1;
    if (sy1 > clip_y1) sy1 = clip_y1;
    if (sx0 > sx1 || sy0 > sy1)
        return 0;

    for (sy = sy0; sy <= sy1; ++sy) {
        double dy = (double)sy + 0.5 - (double)shape->y;

        if (!shape->pivot_given)
            dy += shape->rot_min_y;
        for (sx = sx0; sx <= sx1; ++sx) {
            double dx = (double)sx + 0.5 - (double)shape->x;
            double src_cx, src_cy, p, q;
            int col, row, value, alpha;
            int rc;

            if (!shape->pivot_given)
                dx += shape->rot_min_x;
            src_cx = dx * shape->c + dy * shape->s;
            src_cy = -dx * shape->s + dy * shape->c;
            p = src_cx + shape->px;
            q = src_cy + shape->py;
            if (p < 0.0 || p >= W || q < 0.0 || q >= H)
                continue;

            col = (int)floor(p / (double)shape->scale + 1e-9);
            row = (int)floor(q / (double)shape->scale + 1e-9);
            if (shape->flipx)
                col = sprite->width - 1 - col;
            if (shape->flipy)
                row = sprite->height - 1 - row;
            if (col < 0 || col >= sprite->width || row < 0 ||
                row >= sprite->height)
                continue;

            value = sprite->pixels[(size_t)row * sprite->width + col];
            if (value < 0)
                continue;
            alpha = (g_palette[value].a * shape->ta) / 255;
            alpha = (alpha * global_alpha) / 255;
            if (alpha <= 0)
                continue;

            rc = collision_buf_push(buf, sx, sy);
            if (rc != 0)
                return rc;
        }
    }
    return 0;
}

static int collision_raster_error(lua_State *L, int code)
{
    if (code == -2)
        return luaL_error(L, "draw.collision: out of memory");
    return luaL_error(L, "draw.collision: too many opaque pixels");
}

static int collision_prepare_sprite_points(lua_State *L,
                                           const CollisionShape *sprite,
                                           const CollisionShape *other,
                                           PointBuf *buf)
{
    long long x0, y0, x1, y1;
    int rc;

    buf->data = NULL;
    buf->count = 0;
    buf->cap = 0;
    if (!collision_intersection(L, sprite, other, &x0, &y0, &x1, &y1))
        return -3;
    if (collision_buf_init(buf) != 0)
        return -2;
    rc = collision_rasterize_sprite(sprite, x0, y0, x1, y1, buf);
    if (rc != 0) {
        free(buf->data);
        buf->data = NULL;
        buf->count = 0;
        buf->cap = 0;
        return rc;
    }
    return 0;
}

static int collision_shapes_overlap(lua_State *L, const CollisionShape *a,
                                    const CollisionShape *b)
{
    long long x0, y0, x1, y1, x, y;

    if (!collision_intersection(L, a, b, &x0, &y0, &x1, &y1))
        return 0;
    for (y = y0; y <= y1; ++y) {
        for (x = x0; x <= x1; ++x) {
            if (collision_shape_contains(a, x, y) &&
                collision_shape_contains(b, x, y))
                return 1;
        }
    }
    return 0;
}

static void collision_buf_free(PointBuf *buf)
{
    free(buf->data);
    buf->data = NULL;
    buf->count = 0;
    buf->cap = 0;
}

static int collision_common_points(lua_State *L, const CollisionShape *a,
                                   const CollisionShape *b, PointBuf *out)
{
    long long x0, y0, x1, y1, x, y;
    PointBuf tmp;
    size_t i;
    int rc;

    out->data = NULL;
    out->count = 0;
    out->cap = 0;

    if (a->kind == COLLISION_SPRITE && b->kind == COLLISION_SPRITE) {
        PointBuf ba;
        PointBuf bb;

        rc = collision_prepare_sprite_points(L, a, b, &ba);
        if (rc == -3)
            return -3;
        if (rc != 0)
            return rc;
        rc = collision_prepare_sprite_points(L, b, a, &bb);
        if (rc == -3) {
            collision_buf_free(&ba);
            return -3;
        }
        if (rc != 0) {
            collision_buf_free(&ba);
            return rc;
        }

        rc = collision_buf_init(out);
        if (rc != 0) {
            collision_buf_free(&ba);
            collision_buf_free(&bb);
            return -2;
        }
        qsort(bb.data, bb.count, sizeof(CollisionPoint), collision_point_qcmp);
        for (i = 0; i < ba.count; ++i) {
            if (collision_point_lookup(&bb, collision_key_x(ba.data[i]),
                                       collision_key_y(ba.data[i]))) {
                rc = collision_buf_push(out, collision_key_x(ba.data[i]),
                                        collision_key_y(ba.data[i]));
                if (rc != 0) {
                    collision_buf_free(&ba);
                    collision_buf_free(&bb);
                    collision_buf_free(out);
                    return rc;
                }
            }
        }
        collision_buf_free(&ba);
        collision_buf_free(&bb);
        return 0;
    }

    if (a->kind == COLLISION_SPRITE || b->kind == COLLISION_SPRITE) {
        const CollisionShape *sprite = a->kind == COLLISION_SPRITE ? a : b;
        const CollisionShape *shape = a->kind == COLLISION_SPRITE ? b : a;

        rc = collision_prepare_sprite_points(L, sprite, shape, &tmp);
        if (rc == -3)
            return -3;
        if (rc != 0)
            return rc;
        rc = collision_buf_init(out);
        if (rc != 0) {
            collision_buf_free(&tmp);
            return -2;
        }
        for (i = 0; i < tmp.count; ++i) {
            x = collision_key_x(tmp.data[i]);
            y = collision_key_y(tmp.data[i]);
            if (collision_shape_contains(shape, x, y)) {
                rc = collision_buf_push(out, x, y);
                if (rc != 0) {
                    collision_buf_free(&tmp);
                    collision_buf_free(out);
                    return rc;
                }
            }
        }
        collision_buf_free(&tmp);
        return 0;
    }

    if (!collision_intersection(L, a, b, &x0, &y0, &x1, &y1))
        return -3;
    rc = collision_buf_init(out);
    if (rc != 0)
        return -2;
    for (y = y0; y <= y1; ++y) {
        for (x = x0; x <= x1; ++x) {
            if (collision_shape_contains(a, x, y) &&
                collision_shape_contains(b, x, y)) {
                rc = collision_buf_push(out, x, y);
                if (rc != 0) {
                    collision_buf_free(out);
                    return rc;
                }
            }
        }
    }
    return 0;
}

static long long collision_round_ll(double v)
{
    return v >= 0.0 ? (long long)floor(v + 0.5) : (long long)ceil(v - 0.5);
}

static int collision_lerp_angle(int from, int to, double t)
{
    double delta = (double)to - (double)from;
    long long value;

    if (delta > 180.0)
        delta -= 360.0;
    if (delta < -180.0)
        delta += 360.0;
    value = collision_round_ll((double)from + delta * t) % 360;
    if (value < 0)
        value += 360;
    return (int)value;
}

static void collision_interpolate(const CollisionShape *from,
                                  const CollisionShape *to, double t,
                                  CollisionShape *out)
{
    double rad;

    *out = *to;
    out->x = collision_round_ll((double)from->x +
                                ((double)to->x - (double)from->x) * t);
    out->y = collision_round_ll((double)from->y +
                                ((double)to->y - (double)from->y) * t);

    switch (to->kind) {
    case COLLISION_CIRCLE:
        out->radius = collision_round_ll(
            (double)from->radius + ((double)to->radius - (double)from->radius) * t);
        break;
    case COLLISION_SQUARE:
        out->width = collision_round_ll(
            (double)from->width + ((double)to->width - (double)from->width) * t);
        out->height = collision_round_ll(
            (double)from->height + ((double)to->height - (double)from->height) * t);
        out->angle = collision_lerp_angle(from->angle, to->angle, t);
        rad = (double)out->angle * 0.017453292519943295769236907684886;
        out->c = cos(rad);
        out->s = sin(rad);
        if (fabs(out->c) < 1e-12)
            out->c = 0.0;
        if (fabs(out->s) < 1e-12)
            out->s = 0.0;
        break;
    case COLLISION_TRIANGLE:
        out->x2 = collision_round_ll((double)from->x2 +
                                     ((double)to->x2 - (double)from->x2) * t);
        out->y2 = collision_round_ll((double)from->y2 +
                                     ((double)to->y2 - (double)from->y2) * t);
        out->x3 = collision_round_ll((double)from->x3 +
                                     ((double)to->x3 - (double)from->x3) * t);
        out->y3 = collision_round_ll((double)from->y3 +
                                     ((double)to->y3 - (double)from->y3) * t);
        out->area2 = (out->x2 - out->x) * (out->y3 - out->y) -
                     (out->y2 - out->y) * (out->x3 - out->x);
        break;
    case COLLISION_SPRITE:
        out->angle = collision_lerp_angle(from->angle, to->angle, t);
        out->pivot_x = from->pivot_x + (to->pivot_x - from->pivot_x) * t;
        out->pivot_y = from->pivot_y + (to->pivot_y - from->pivot_y) * t;
        collision_sprite_geometry(out);
        break;
    default:
        break;
    }
}

static int collision_same_kind(const CollisionShape *a,
                               const CollisionShape *b)
{
    if (a->kind != b->kind)
        return 0;
    if (a->kind == COLLISION_SPRITE && a->sprite_index != b->sprite_index)
        return 0;
    return 1;
}

static int collision_overlap_at(lua_State *L, const CollisionShape *a,
                                const CollisionShape *b)
{
    PointBuf buf;
    int rc;
    int hit;

    rc = collision_common_points(L, a, b, &buf);
    if (rc == -3)
        return 0;
    if (rc != 0) {
        collision_buf_free(&buf);
        collision_raster_error(L, rc);
        return 0;
    }
    hit = buf.count > 0;
    collision_buf_free(&buf);
    return hit;
}

static void collision_centroid(const PointBuf *buf, double *cx, double *cy)
{
    double sx = 0.0;
    double sy = 0.0;
    size_t i;

    for (i = 0; i < buf->count; ++i) {
        sx += (double)collision_key_x(buf->data[i]);
        sy += (double)collision_key_y(buf->data[i]);
    }
    *cx = buf->count ? sx / (double)buf->count : 0.0;
    *cy = buf->count ? sy / (double)buf->count : 0.0;
}

static void collision_shape_center(const CollisionShape *shape,
                                   double *cx, double *cy)
{
    switch (shape->kind) {
    case COLLISION_POINT:
    case COLLISION_CIRCLE:
        *cx = (double)shape->x;
        *cy = (double)shape->y;
        break;
    case COLLISION_SQUARE:
        *cx = (double)shape->x + (double)shape->width / 2.0;
        *cy = (double)shape->y + (double)shape->height / 2.0;
        break;
    case COLLISION_TRIANGLE:
        *cx = (double)(shape->x + shape->x2 + shape->x3) / 3.0;
        *cy = (double)(shape->y + shape->y2 + shape->y3) / 3.0;
        break;
    default:
        *cx = (double)shape->x +
              (shape->rot_min_x + shape->rot_max_x) / 2.0;
        *cy = (double)shape->y +
              (shape->rot_min_y + shape->rot_max_y) / 2.0;
        break;
    }
}

static int collision_angle_from_vector(double dx, double dy, int *angle)
{
    double deg;

    if (!isfinite(dx) || !isfinite(dy) || dx * dx + dy * dy < 1e-18)
        return 0;
    deg = atan2(dy, dx) * 57.295779513082320876798154814105;
    if (deg < 0.0)
        deg += 360.0;
    if (deg >= 360.0)
        deg -= 360.0;
    *angle = (int)collision_round_ll(deg) % 360;
    return 1;
}

static int collision_triangle_normal(const CollisionShape *shape,
                                     double cx, double cy,
                                     double bx, double by, int *angle)
{
    double px[3], py[3];
    double gx, gy;
    double best = 0.0;
    double nx = 0.0, ny = 0.0;
    int found = 0;
    int i;

    px[0] = (double)shape->x;
    py[0] = (double)shape->y;
    px[1] = (double)shape->x2;
    py[1] = (double)shape->y2;
    px[2] = (double)shape->x3;
    py[2] = (double)shape->y3;
    gx = (px[0] + px[1] + px[2]) / 3.0;
    gy = (py[0] + py[1] + py[2]) / 3.0;

    for (i = 0; i < 3; ++i) {
        int j = (i + 1) % 3;
        int k = (i + 2) % 3;
        double ex = px[j] - px[i];
        double ey = py[j] - py[i];
        double len2 = ex * ex + ey * ey;
        double t = 0.0;
        double qx, qy, d2, mx, my;

        if (len2 <= 0.0)
            continue;
        t = ((cx - px[i]) * ex + (cy - py[i]) * ey) / len2;
        if (t < 0.0) t = 0.0;
        if (t > 1.0) t = 1.0;
        qx = px[i] + ex * t;
        qy = py[i] + ey * t;
        d2 = (cx - qx) * (cx - qx) + (cy - qy) * (cy - qy);
        if (!found || d2 < best) {
            best = d2;
            mx = ey;
            my = -ex;
            if (mx * (cx - px[k]) + my * (cy - py[k]) > 0.0) {
                nx = mx;
                ny = my;
            } else {
                nx = -mx;
                ny = -my;
            }
            if (fabs(nx * (cx - gx) + ny * (cy - gy)) < 1e-12) {
                mx = bx - gx;
                my = by - gy;
                if (nx * mx + ny * my < 0.0) {
                    nx = -nx;
                    ny = -ny;
                }
            }
            found = 1;
        }
    }
    return found ? collision_angle_from_vector(nx, ny, angle) : 0;
}

static int collision_surface_angle(const CollisionShape *a,
                                   double cx, double cy,
                                   double bx, double by, int *angle)
{
    double ax, ay, dx, dy;

    switch (a->kind) {
    case COLLISION_POINT:
        return collision_angle_from_vector(bx - a->x, by - a->y, angle);
    case COLLISION_CIRCLE:
        dx = cx - (double)a->x;
        dy = cy - (double)a->y;
        if (dx * dx + dy * dy < 1e-12) {
            dx = bx - (double)a->x;
            dy = by - (double)a->y;
        }
        return collision_angle_from_vector(dx, dy, angle);
    case COLLISION_SQUARE: {
        double center_x = (double)a->x + (double)a->width / 2.0;
        double center_y = (double)a->y + (double)a->height / 2.0;
        double hw = (double)a->width / 2.0;
        double hh = (double)a->height / 2.0;
        double lx, ly, dl, dr, dt, db, nlx, nly, nx, ny;

        if (hw <= 0.0 || hh <= 0.0)
            return collision_angle_from_vector(cx - center_x, cy - center_y,
                                               angle);
        lx = (cx - center_x) * a->c + (cy - center_y) * a->s;
        ly = -(cx - center_x) * a->s + (cy - center_y) * a->c;
        dl = hw + lx;
        dr = hw - lx;
        dt = hh + ly;
        db = hh - ly;
        if (dl <= dr && dl <= dt && dl <= db) {
            nlx = -1.0;
            nly = 0.0;
        } else if (dr <= dl && dr <= dt && dr <= db) {
            nlx = 1.0;
            nly = 0.0;
        } else if (dt <= dl && dt <= dr && dt <= db) {
            nlx = 0.0;
            nly = -1.0;
        } else {
            nlx = 0.0;
            nly = 1.0;
        }
        nx = nlx * a->c - nly * a->s;
        ny = nlx * a->s + nly * a->c;
        return collision_angle_from_vector(nx, ny, angle);
    }
    case COLLISION_TRIANGLE:
        return collision_triangle_normal(a, cx, cy, bx, by, angle);
    default:
        collision_shape_center(a, &ax, &ay);
        dx = cx - ax;
        dy = cy - ay;
        if (dx * dx + dy * dy < 1e-12) {
            dx = bx - ax;
            dy = by - ay;
        }
        return collision_angle_from_vector(dx, dy, angle);
    }
}

static int l_contactangle(lua_State *L)
{
    enum { CONTACT_SAMPLES = 16, CONTACT_STEPS = 16 };
    CollisionShape a, b, old_b, current;
    PointBuf buf;
    double cx, cy, bx, by;
    double lo = 0.0, hi = 1.0, t = 0.0;
    int angle;
    int rc;
    int found = 0;
    int i;

    if (collision_parse_shape(L, 1, &a) != 0)
        return 0;
    if (collision_parse_shape(L, 2, &b) != 0)
        return 0;
    if (collision_parse_shape(L, 3, &old_b) != 0)
        return 0;
    if (!collision_same_kind(&b, &old_b))
        return luaL_error(L,
            "draw.contactangle: b and old_b must describe the same shape");

    if (!collision_overlap_at(L, &a, &old_b)) {
        for (i = 0; i <= CONTACT_SAMPLES; ++i) {
            double sample = (double)i / (double)CONTACT_SAMPLES;

            collision_interpolate(&old_b, &b, sample, &current);
            if (!collision_overlap_at(L, &a, &current))
                continue;
            lo = i > 0 ? (double)(i - 1) / (double)CONTACT_SAMPLES : 0.0;
            hi = sample;
            found = 1;
            break;
        }
        if (!found) {
            lua_pushnil(L);
            return 1;
        }
        for (i = 0; i < CONTACT_STEPS; ++i) {
            double mid = (lo + hi) / 2.0;

            collision_interpolate(&old_b, &b, mid, &current);
            if (collision_overlap_at(L, &a, &current))
                hi = mid;
            else
                lo = mid;
        }
        t = hi;
    }

    collision_interpolate(&old_b, &b, t, &current);
    rc = collision_common_points(L, &a, &current, &buf);
    if (rc == -3) {
        lua_pushnil(L);
        return 1;
    }
    if (rc != 0) {
        collision_buf_free(&buf);
        return collision_raster_error(L, rc);
    }
    if (buf.count == 0) {
        collision_buf_free(&buf);
        lua_pushnil(L);
        return 1;
    }

    collision_centroid(&buf, &cx, &cy);
    collision_buf_free(&buf);
    collision_shape_center(&current, &bx, &by);
    if (!collision_surface_angle(&a, cx, cy, bx, by, &angle)) {
        lua_pushnil(L);
        return 1;
    }

    lua_pushinteger(L, angle);
    lua_pushinteger(L, collision_round_ll(cx));
    lua_pushinteger(L, collision_round_ll(cy));
    return 3;
}

static int l_collision(lua_State *L)
{
    CollisionShape a;
    CollisionShape b;

    if (collision_parse_shape(L, 1, &a) != 0)
        return 0;
    if (collision_parse_shape(L, 2, &b) != 0)
        return 0;

    if (a.kind == COLLISION_SPRITE && b.kind == COLLISION_SPRITE) {
        PointBuf ba;
        PointBuf bb;
        size_t i;
        int rc;
        int result = 0;

        rc = collision_prepare_sprite_points(L, &a, &b, &ba);
        if (rc == -3) {
            lua_pushboolean(L, 0);
            return 1;
        }
        if (rc != 0)
            return collision_raster_error(L, rc);
        rc = collision_prepare_sprite_points(L, &b, &a, &bb);
        if (rc == -3) {
            free(ba.data);
            lua_pushboolean(L, 0);
            return 1;
        }
        if (rc != 0) {
            free(ba.data);
            return collision_raster_error(L, rc);
        }

        qsort(bb.data, bb.count, sizeof(CollisionPoint), collision_point_qcmp);
        for (i = 0; i < ba.count; ++i) {
            if (collision_point_lookup(&bb, collision_key_x(ba.data[i]),
                                       collision_key_y(ba.data[i]))) {
                result = 1;
                break;
            }
        }
        free(ba.data);
        free(bb.data);
        lua_pushboolean(L, result);
        return 1;
    }

    if (a.kind == COLLISION_SPRITE || b.kind == COLLISION_SPRITE) {
        const CollisionShape *sprite = a.kind == COLLISION_SPRITE ? &a : &b;
        const CollisionShape *shape = a.kind == COLLISION_SPRITE ? &b : &a;
        PointBuf buf;
        size_t i;
        int rc;
        int result = 0;

        rc = collision_prepare_sprite_points(L, sprite, shape, &buf);
        if (rc == -3) {
            lua_pushboolean(L, 0);
            return 1;
        }
        if (rc != 0)
            return collision_raster_error(L, rc);
        for (i = 0; i < buf.count; ++i) {
            if (collision_shape_contains(shape, collision_key_x(buf.data[i]),
                                         collision_key_y(buf.data[i]))) {
                result = 1;
                break;
            }
        }
        free(buf.data);
        lua_pushboolean(L, result);
        return 1;
    }

    lua_pushboolean(L, collision_shapes_overlap(L, &a, &b));
    return 1;
}

typedef struct {
    const char *name;
    const char *usage;
    const char *description;
} HelpEntry;

static const HelpEntry g_help_entries[] = {
    {"open", "draw.open(width, height, [fps])",
     "Открывает окно; fps по умолчанию 60."},
    {"present", "draw.present() -> boolean",
     "Обновляет экран и события; false, если окно закрыто."},
    {"close", "draw.close()",
     "Закрывает окно."},
    {"clear", "draw.clear([color])",
     "Заливает экран цветом: имя палитры или {r, g, b, [a]}."},
    {"pixel", "draw.pixel(x, y, color)",
     "Рисует пиксель цветом: имя палитры или {r, g, b, [a]}."},
    {"getpixel", "draw.getpixel(x, y) -> r, g, b, a | nil",
     "Возвращает цвет пикселя."},
    {"width", "draw.width() -> number",
     "Возвращает ширину окна."},
    {"height", "draw.height() -> number",
     "Возвращает высоту окна."},
    {"title", "draw.title([text]) -> string | nil",
     "Возвращает или устанавливает заголовок окна."},
    {"delta", "draw.delta() -> number",
     "Возвращает время последнего кадра в секундах."},
    {"alpha", "draw.alpha([a]) -> number | nil",
     "Возвращает или устанавливает альфу по умолчанию."},
    {"mouse", "draw.mouse() -> dx, dy",
     "Возвращает движение мыши за кадр."},
    {"mouseposition", "draw.mouseposition([axis]) -> x, y | number",
     "Возвращает координаты курсора."},
    {"mousebutton", "draw.mousebutton() -> string | nil",
     "Возвращает последнюю нажатую кнопку мыши."},
    {"lastkey", "draw.lastkey() -> string | nil",
     "Возвращает последнюю нажатую клавишу."},
    {"key", "draw.key(name) -> boolean",
     "Проверяет, удерживается ли клавиша."},
    {"keypress", "draw.keypress(name) -> boolean",
     "Проверяет одноразовое нажатие клавиши."},
    {"mousepress", "draw.mousepress(button) -> boolean",
     "Проверяет одноразовое нажатие кнопки мыши."},
    {"wheel", "draw.wheel() -> dx, dy",
     "Возвращает прокрутку колеса за кадр."},
    {"gamepadcount", "draw.gamepadcount() -> number",
     "Возвращает число подключённых геймпадов."},
    {"gamepadname", "draw.gamepadname([index]) -> string | nil",
     "Возвращает имя геймпада; index по умолчанию 1."},
    {"gamepadbutton", "draw.gamepadbutton(name, [index]) -> boolean",
     "Проверяет, удерживается ли кнопка геймпада."},
    {"gamepadpress", "draw.gamepadpress(name, [index]) -> boolean",
     "Одноразовое нажатие кнопки геймпада."},
    {"gamepadstick", "draw.gamepadstick(name, [index]) -> number",
     "Возвращает ось геймпада: -1..1 или 0..1 для триггеров."},
    {"gamepadrumble", "draw.gamepadrumble(left, right, [ms], [index]) -> boolean",
     "Вибрация геймпада; сила 0..1, ms по умолчанию 200."},
    {"line", "draw.line(x1, y1, x2, y2, color, [width])",
     "Рисует линию; цвет — имя палитры или {r, g, b, [a]}."},
    {"circle", "draw.circle(x, y, radius, color, [fill])",
     "Рисует окружность; цвет — имя палитры или {r, g, b, [a]}."},
    {"square", "draw.square(x, y, w, h, color, [fill], [angle])",
     "Рисует прямоугольник с поворотом вокруг центра; углы в градусах."},
    {"triangle", "draw.triangle(x1, y1, x2, y2, x3, y3, color, [fill])",
     "Рисует треугольник; цвет — имя палитры или {r, g, b, [a]}."},
    {"text", "draw.text(x, y, text, [font], [color])",
     "Рисует текст шрифтом по умолчанию или растровым."},
    {"palette", "draw.palette(name_or_index, r, g, b, [a]) -> index",
     "Создаёт или заменяет цвет палитры."},
    {"sprite", "draw.sprite(name, rows, [palette]) -> index",
     "Создаёт спрайт; palette — список цветов для индексов строк."},
    {"spriteimage", "draw.spriteimage(name, path, [maxcolors], [alpha]) -> index, start, count",
     "Загружает изображение как спрайт и палитру."},
    {"font", "draw.font(name, path, [cellw], [cellh], [first], [scale]) -> index",
     "Загружает растровый шрифт."},
    {"animation", "draw.animation(name, frames, [fps]) -> index",
     "Создаёт анимацию из кадров."},
    {"draw", "draw.draw(x, y, name, [scale], [angle], [flip], [tint], [pivot])",
     "Рисует спрайт; flip={flipx,flipy}, pivot={pivotx,pivoty}, nil допустим."},
    {"collision", "draw.collision(a, b) -> boolean",
     "Проверяет пересечение спрайтов и фигур попиксельно."},
    {"contactangle", "draw.contactangle(a, b, old_b) -> angle, x, y | nil",
     "Угол первого касания: куда оттолкнуть b; 0° вправо, 90° вниз."},
    {"melody", "draw.melody(name, notes) -> index",
     "Регистрирует мелодию из частот."},
    {"midi", "draw.midi(name, path, [track]) -> index, speed",
     "Загружает MIDI как мелодию."},
    {"sound", "draw.sound(name, path) -> index",
     "Загружает WAV-звук."},
    {"play", "draw.play(name, [volume], [speed], [wave]) -> boolean",
     "Воспроизводит мелодию или звук."},
    {"stop", "draw.stop()",
     "Останавливает воспроизведение."},
    {"help", "draw.help([command])",
     "Печатает список команд или справку по команде."}
};

static const HelpEntry *find_help_entry(const char *name)
{
    size_t i;
    for (i = 0; i < sizeof(g_help_entries) / sizeof(g_help_entries[0]); ++i) {
        if (strcmp(g_help_entries[i].name, name) == 0)
            return &g_help_entries[i];
    }
    return NULL;
}

static int l_help(lua_State *L)
{
    const HelpEntry *entry;
    size_t i;

    if (lua_isnoneornil(L, 1)) {
        fputs("draw commands:\n", stdout);
        for (i = 0; i < sizeof(g_help_entries) / sizeof(g_help_entries[0]); ++i) {
            entry = &g_help_entries[i];
            printf("  %-14s %s\n", entry->name, entry->usage);
        }
        fflush(stdout);
        return 0;
    }

    entry = find_help_entry(luaL_checkstring(L, 1));
    if (!entry)
        return luaL_error(L, "draw.help: unknown command");
    printf("%s\n  %s\n", entry->usage, entry->description);
    fflush(stdout);
    return 0;
}

static void auto_close(void)
{
    if (g_draw.initialized)
        destroy_resources();
    audio_close();
    free_melodies();
    free_anims();
    free_fonts();
    free_sprites();
    free_palette();
    IMG_Quit();
}

static const luaL_Reg draw_lib[] = {
    {"open",    l_open},
    {"present", l_present},
    {"clear",   l_clear},
    {"pixel",   l_pixel},
    {"getpixel", l_getpixel},
    {"width",   l_width},
    {"height",  l_height},
    {"title",   l_title},
    {"delta",   l_delta},
    {"alpha",   l_alpha},
    {"mouse",   l_mouse},
    {"mouseposition", l_mouseposition},
    {"mousebutton", l_mousebutton},
    {"lastkey", l_lastkey},
    {"key",     l_key},
    {"keypress", l_keypress},
    {"mousepress", l_mousepress},
    {"wheel",   l_wheel},
    {"gamepadcount", l_gamepadcount},
    {"gamepadname", l_gamepadname},
    {"gamepadbutton", l_gamepadbutton},
    {"gamepadpress", l_gamepadpress},
    {"gamepadstick", l_gamepadstick},
    {"gamepadrumble", l_gamepadrumble},
    {"line",    l_line},
    {"circle",  l_circle},
    {"square",  l_square},
    {"triangle", l_triangle},
    {"text",    l_text},
    {"palette", l_palette},
    {"sprite",  l_sprite},
    {"spriteimage", l_spriteimage},
    {"font",    l_font},
    {"animation", l_animation},
    {"draw",    l_draw_sprite},
    {"collision", l_collision},
    {"contactangle", l_contactangle},
    {"melody",  l_melody},
    {"midi",    l_midi},
    {"sound",   l_sound},
    {"play",    l_play},
    {"stop",    l_stop},
    {"help",    l_help},
    {"close",   l_close},
    {NULL, NULL}
};

int luaopen_draw(lua_State *L)
{
    (void)IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG);
    atexit(auto_close);

#if LUA_VERSION_NUM >= 502
    luaL_newlib(L, draw_lib);
#else
    lua_newtable(L);
    luaL_register(L, NULL, draw_lib);
#endif
    return 1;
}
