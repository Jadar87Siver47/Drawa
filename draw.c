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
#include <SDL_opengl.h>
#include <SDL_opengl_glext.h>

#include <SDL_image.h>
#include <SDL_ttf.h>
#include <SDL_mixer.h>

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
#define CANVAS_MAX 64
#define CANVAS_DIM_MAX 4096
#define CANVAS_AREA_MAX 4194304
#define CANVAS_TOTAL_MAX 16777216
#define TILEMAP_MAX 32
#define TILEMAP_ROW_MAX 256
#define TILEMAP_COL_MAX 256
#define TILEMAP_TILE_MAX 64
#define TILEMAP_BODY_MAX 512
#define TTF_FONT_MAX 32
#define TTF_CACHE_MAX 256
#define MIXER_ASSET_MAX 64
#define MIXER_CHANNEL_MAX 64
#define SHADER_MAX 32
#define SHADER_NAME_MAX 64
#define SHADER_SOURCE_MAX (64 * 1024)
#define PARTICLE_MAX 2048
#define TWEEN_MAX 128
#define AUX_PI 3.14159265358979323846

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
    int used;
    int width;
    int height;
    uint32_t *pixels;
} CanvasEntry;

typedef struct {
    int used;
    char name[PALETTE_NAME_MAX];
    int rows;
    int cols;
    int size;
    char tiles[TILEMAP_TILE_MAX][SPRITE_NAME_MAX];
    signed char cells[TILEMAP_ROW_MAX * TILEMAP_COL_MAX];
    int body_count;
    int body_ids[TILEMAP_BODY_MAX];
} TilemapEntry;

typedef struct {
    int used;
    char name[PALETTE_NAME_MAX];
    TTF_Font *font;
    int ptsize;
} TtfEntry;

typedef struct {
    int used;
    int font_id;
    char *text;
    SDL_Surface *surface;
    unsigned long stamp;
} TtfCacheEntry;

typedef struct {
    int used;
    int kind;
    char name[PALETTE_NAME_MAX];
    Mix_Chunk *chunk;
    Mix_Music *music;
} MixerEntry;

typedef struct {
    int used;
    char name[SHADER_NAME_MAX];
    GLuint program;
} ShaderEntry;

typedef struct {
    int used;
    double x, y;
    double vx, vy;
    double life;
    double max_life;
    double gravity;
    double drag;
    int size;
    int r, g, b, a;
} ParticleEntry;

typedef struct {
    int used;
    double from;
    double to;
    double delay;
    double duration;
    double elapsed;
    int easing;
} TweenEntry;

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
    SDL_GLContext gl_context;
    GLuint gl_texture;
    GLuint gl_program;
    GLuint gl_vbo;
    int gl_ready;
    int viewport_x, viewport_y, viewport_w, viewport_h;
    Uint32 start_tick;
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
static CanvasEntry g_canvases[CANVAS_MAX];
static TilemapEntry g_tilemaps[TILEMAP_MAX];
static TtfEntry g_ttf_fonts[TTF_FONT_MAX];
static TtfCacheEntry g_ttf_cache[TTF_CACHE_MAX];
static unsigned long g_ttf_stamp;
static MixerEntry g_mixer_assets[MIXER_ASSET_MAX];
static int g_mixer_ready;
static ShaderEntry g_shaders[SHADER_MAX];
static int g_active_shader;
static char g_shader_error[512];
static ParticleEntry g_particles[PARTICLE_MAX];
static TweenEntry g_tweens[TWEEN_MAX];

static PFNGLCREATESHADERPROC glCreateShader = NULL;
static PFNGLSHADERSOURCEPROC glShaderSource = NULL;
static PFNGLCOMPILESHADERPROC glCompileShader = NULL;
static PFNGLGETSHADERIVPROC glGetShaderiv = NULL;
static PFNGLGETSHADERINFOLOGPROC glGetShaderInfoLog = NULL;
static PFNGLDELETESHADERPROC glDeleteShader = NULL;
static PFNGLCREATEPROGRAMPROC glCreateProgram = NULL;
static PFNGLATTACHSHADERPROC glAttachShader = NULL;
static PFNGLLINKPROGRAMPROC glLinkProgram = NULL;
static PFNGLGETPROGRAMIVPROC glGetProgramiv = NULL;
static PFNGLGETPROGRAMINFOLOGPROC glGetProgramInfoLog = NULL;
static PFNGLDELETEPROGRAMPROC glDeleteProgram = NULL;
static PFNGLGENBUFFERSPROC glGenBuffers = NULL;
static PFNGLBINDBUFFERPROC glBindBuffer = NULL;
static PFNGLBUFFERDATAPROC glBufferData = NULL;
static PFNGLDELETEBUFFERSPROC glDeleteBuffers = NULL;
static PFNGLUSEPROGRAMPROC glUseProgram = NULL;
static PFNGLUNIFORM1IPROC glUniform1i = NULL;
static PFNGLUNIFORM1FPROC glUniform1f = NULL;
static PFNGLUNIFORM2FPROC glUniform2f = NULL;
static PFNGLUNIFORM3FPROC glUniform3f = NULL;
static PFNGLUNIFORM4FPROC glUniform4f = NULL;
static PFNGLGETUNIFORMLOCATIONPROC glGetUniformLocation = NULL;
static PFNGLGETATTRIBLOCATIONPROC glGetAttribLocation = NULL;
static PFNGLENABLEVERTEXATTRIBARRAYPROC glEnableVertexAttribArray = NULL;
static PFNGLVERTEXATTRIBPOINTERPROC glVertexAttribPointer = NULL;
static PFNGLDISABLEVERTEXATTRIBARRAYPROC glDisableVertexAttribArray = NULL;
static int g_channel_left[MIXER_CHANNEL_MAX];
static int g_channel_right[MIXER_CHANNEL_MAX];
static double g_listener_x;
static double g_listener_y;
static double g_listener_spread = 320.0;
static double g_camera_x;
static double g_camera_y;
static uint32_t *g_main_pixels;
static int g_main_width;
static int g_main_height;
static int g_canvas_target;

static void audio_update_finished(void);
static void physics_clear_state(void);
static void canvas_restore_main(void);
static void free_canvases(void);
static void tilemap_clear_bodyrefs(void);
static void free_tilemaps(void);
static void shader_release_resources(void);

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

static void canvas_restore_main(void)
{
    if (g_canvas_target == 0)
        return;
    g_draw.pixels = g_main_pixels;
    g_draw.width = g_main_width;
    g_draw.height = g_main_height;
    g_canvas_target = 0;
}

static void free_canvases(void)
{
    int i;

    for (i = 0; i < CANVAS_MAX; ++i) {
        free(g_canvases[i].pixels);
        memset(&g_canvases[i], 0, sizeof(g_canvases[i]));
    }
}

static void tilemap_clear_bodyrefs(void)
{
    int i;

    for (i = 0; i < TILEMAP_MAX; ++i) {
        g_tilemaps[i].body_count = 0;
        memset(g_tilemaps[i].body_ids, 0, sizeof(g_tilemaps[i].body_ids));
    }
}

static void free_tilemaps(void)
{
    tilemap_clear_bodyrefs();
    memset(g_tilemaps, 0, sizeof(g_tilemaps));
}

static void mark_dirty(int x, int y)
{
    int x1, y1;

    if (g_canvas_target != 0)
        return;
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

    if (g_canvas_target != 0)
        return;
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

static const char *g_shader_default_vertex =
    "attribute vec2 a_position;\n"
    "attribute vec2 a_uv;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    v_texcoord = a_uv;\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "}\n";

static const char *g_shader_default_fragment =
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_texture, v_texcoord);\n"
    "    gl_FragColor = vec4(c.rgb * c.a, 1.0);\n"
    "}\n";

static const char *g_shader_soft_fragment =
    "varying vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_softness;\n"
    "void main() {\n"
    "    vec2 px = 1.0 / max(u_resolution, vec2(1.0));\n"
    "    vec4 c = texture2D(u_texture, v_texcoord);\n"
    "    vec4 blur = c * 4.0;\n"
    "    blur += texture2D(u_texture, v_texcoord + vec2(px.x, 0.0)) * 2.0;\n"
    "    blur += texture2D(u_texture, v_texcoord - vec2(px.x, 0.0)) * 2.0;\n"
    "    blur += texture2D(u_texture, v_texcoord + vec2(0.0, px.y)) * 2.0;\n"
    "    blur += texture2D(u_texture, v_texcoord - vec2(0.0, px.y)) * 2.0;\n"
    "    blur += texture2D(u_texture, v_texcoord + px);\n"
    "    blur += texture2D(u_texture, v_texcoord - px);\n"
    "    blur += texture2D(u_texture, v_texcoord + vec2(px.x, -px.y));\n"
    "    blur += texture2D(u_texture, v_texcoord + vec2(-px.x, px.y));\n"
    "    blur /= 16.0;\n"
    "    vec3 rgb = mix(c.rgb, blur.rgb, clamp(u_softness, 0.0, 1.0));\n"
    "    gl_FragColor = vec4(rgb * c.a, 1.0);\n"
    "}\n";

static int shader_load_functions(void)
{
#define LOAD_GL(name, type) do { \
        name = (type)SDL_GL_GetProcAddress(#name); \
        if (!name) \
            return -1; \
    } while (0)

    LOAD_GL(glCreateShader, PFNGLCREATESHADERPROC);
    LOAD_GL(glShaderSource, PFNGLSHADERSOURCEPROC);
    LOAD_GL(glCompileShader, PFNGLCOMPILESHADERPROC);
    LOAD_GL(glGetShaderiv, PFNGLGETSHADERIVPROC);
    LOAD_GL(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC);
    LOAD_GL(glDeleteShader, PFNGLDELETESHADERPROC);
    LOAD_GL(glCreateProgram, PFNGLCREATEPROGRAMPROC);
    LOAD_GL(glAttachShader, PFNGLATTACHSHADERPROC);
    LOAD_GL(glLinkProgram, PFNGLLINKPROGRAMPROC);
    LOAD_GL(glGetProgramiv, PFNGLGETPROGRAMIVPROC);
    LOAD_GL(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC);
    LOAD_GL(glDeleteProgram, PFNGLDELETEPROGRAMPROC);
    LOAD_GL(glGenBuffers, PFNGLGENBUFFERSPROC);
    LOAD_GL(glBindBuffer, PFNGLBINDBUFFERPROC);
    LOAD_GL(glBufferData, PFNGLBUFFERDATAPROC);
    LOAD_GL(glDeleteBuffers, PFNGLDELETEBUFFERSPROC);
    LOAD_GL(glUseProgram, PFNGLUSEPROGRAMPROC);
    LOAD_GL(glUniform1i, PFNGLUNIFORM1IPROC);
    LOAD_GL(glUniform1f, PFNGLUNIFORM1FPROC);
    LOAD_GL(glUniform2f, PFNGLUNIFORM2FPROC);
    LOAD_GL(glUniform3f, PFNGLUNIFORM3FPROC);
    LOAD_GL(glUniform4f, PFNGLUNIFORM4FPROC);
    LOAD_GL(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
    LOAD_GL(glGetAttribLocation, PFNGLGETATTRIBLOCATIONPROC);
    LOAD_GL(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC);
    LOAD_GL(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC);
    LOAD_GL(glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC);
#undef LOAD_GL
    return 0;
}

static GLuint shader_compile_stage(GLenum kind, const char *source, char *log,
                                   size_t log_size)
{
    GLuint shader = glCreateShader(kind);
    GLint ok = 0;

    if (!shader)
        return 0;
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        if (log && log_size > 0) {
            glGetShaderInfoLog(shader, (GLsizei)log_size, NULL, log);
            log[log_size - 1] = '\0';
        }
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint shader_link_program(const char *vertex, const char *fragment,
                                  char *log, size_t log_size)
{
    GLuint vs;
    GLuint fs;
    GLuint program;
    GLint ok = 0;

    if (log && log_size > 0)
        log[0] = '\0';
    vs = shader_compile_stage(GL_VERTEX_SHADER, vertex, log, log_size);
    if (!vs)
        return 0;
    fs = shader_compile_stage(GL_FRAGMENT_SHADER, fragment, log, log_size);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }
    program = glCreateProgram();
    if (!program) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok && log && log_size > 0) {
        glGetProgramInfoLog(program, (GLsizei)log_size, NULL, log);
        log[log_size - 1] = '\0';
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

static ShaderEntry *shader_by_id(lua_Integer id)
{
    if (id < 1 || id > SHADER_MAX)
        return NULL;
    if (!g_shaders[id - 1].used)
        return NULL;
    return &g_shaders[id - 1];
}

static int find_shader_name(const char *name)
{
    int i;

    for (i = 0; i < SHADER_MAX; ++i) {
        if (g_shaders[i].used && strcmp(g_shaders[i].name, name) == 0)
            return i + 1;
    }
    return 0;
}

static GLuint shader_active_program(void)
{
    if (g_active_shader > 0 && g_shaders[g_active_shader - 1].used)
        return g_shaders[g_active_shader - 1].program;
    return g_draw.gl_program;
}

static int shader_init_default(void)
{
    static const float quad[] = {
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f
    };

    g_draw.gl_program = shader_link_program(
        g_shader_default_vertex, g_shader_default_fragment,
        g_shader_error, sizeof(g_shader_error));
    if (!g_draw.gl_program)
        return -1;

    memset(&g_shaders[0], 0, sizeof(g_shaders[0]));
    g_shaders[0].used = 1;
    strcpy(g_shaders[0].name, "soft");
    g_shaders[0].program = shader_link_program(
        g_shader_default_vertex, g_shader_soft_fragment,
        g_shader_error, sizeof(g_shader_error));
    if (!g_shaders[0].program)
        return -1;

    glGenTextures(1, &g_draw.gl_texture);
    glBindTexture(GL_TEXTURE_2D, g_draw.gl_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, g_draw.width, g_draw.height, 0,
                 GL_BGRA, GL_UNSIGNED_BYTE, NULL);

    glGenBuffers(1, &g_draw.gl_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_draw.gl_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    return 0;
}

static void shader_release_resources(void)
{
    int i;

    for (i = 0; i < SHADER_MAX; ++i) {
        if (g_shaders[i].used && g_shaders[i].program) {
            glDeleteProgram(g_shaders[i].program);
            g_shaders[i].program = 0;
        }
        g_shaders[i].used = 0;
        g_shaders[i].name[0] = '\0';
    }
    g_active_shader = 0;
    if (g_draw.gl_program) {
        glDeleteProgram(g_draw.gl_program);
        g_draw.gl_program = 0;
    }
    if (g_draw.gl_texture) {
        glDeleteTextures(1, &g_draw.gl_texture);
        g_draw.gl_texture = 0;
    }
    if (g_draw.gl_vbo) {
        glDeleteBuffers(1, &g_draw.gl_vbo);
        g_draw.gl_vbo = 0;
    }
    if (g_draw.gl_context) {
        SDL_GL_DeleteContext(g_draw.gl_context);
        g_draw.gl_context = NULL;
    }
    g_draw.gl_ready = 0;
}

static void shader_compute_viewport(int *vx, int *vy, int *vw, int *vh)
{
    int win_w = 1;
    int win_h = 1;
    double scale;

    SDL_GetWindowSize(g_draw.window, &win_w, &win_h);
    if (win_w <= 0)
        win_w = 1;
    if (win_h <= 0)
        win_h = 1;
    if (g_main_width <= 0 || g_main_height <= 0) {
        *vx = 0;
        *vy = 0;
        *vw = win_w;
        *vh = win_h;
        return;
    }
    scale = (double)win_w / (double)g_main_width;
    if ((double)win_h / (double)g_main_height < scale)
        scale = (double)win_h / (double)g_main_height;
    if (scale <= 0.0)
        scale = 1.0;
    *vw = (int)(g_main_width * scale);
    *vh = (int)(g_main_height * scale);
    if (*vw < 1)
        *vw = 1;
    if (*vh < 1)
        *vh = 1;
    *vx = (win_w - *vw) / 2;
    *vy = (win_h - *vh) / 2;
    if (*vx < 0)
        *vx = 0;
    if (*vy < 0)
        *vy = 0;
}

static void shader_present(void)
{
    GLuint program;
    GLint pos;
    GLint uv;
    float elapsed;

    shader_compute_viewport(&g_draw.viewport_x, &g_draw.viewport_y,
                            &g_draw.viewport_w, &g_draw.viewport_h);
    glViewport(g_draw.viewport_x, g_draw.viewport_y,
               g_draw.viewport_w, g_draw.viewport_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_draw.gl_texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                    g_main_width, g_main_height,
                    GL_BGRA, GL_UNSIGNED_BYTE, g_main_pixels);

    program = shader_active_program();
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "u_texture"), 0);
    glUniform2f(glGetUniformLocation(program, "u_resolution"),
                (float)g_main_width, (float)g_main_height);
    elapsed = (float)(SDL_GetTicks() - g_draw.start_tick) / 1000.0f;
    glUniform1f(glGetUniformLocation(program, "u_time"), elapsed);
    if (program == g_shaders[0].program && g_shaders[0].used)
        glUniform1f(glGetUniformLocation(program, "u_softness"), 0.35f);

    glBindBuffer(GL_ARRAY_BUFFER, g_draw.gl_vbo);
    pos = glGetAttribLocation(program, "a_position");
    uv = glGetAttribLocation(program, "a_uv");
    if (pos >= 0) {
        glEnableVertexAttribArray((GLuint)pos);
        glVertexAttribPointer((GLuint)pos, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float), (const void *)0);
    }
    if (uv >= 0) {
        glEnableVertexAttribArray((GLuint)uv);
        glVertexAttribPointer((GLuint)uv, 2, GL_FLOAT, GL_FALSE,
                              4 * sizeof(float),
                              (const void *)(2 * sizeof(float)));
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (pos >= 0)
        glDisableVertexAttribArray((GLuint)pos);
    if (uv >= 0)
        glDisableVertexAttribArray((GLuint)uv);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    SDL_GL_SwapWindow(g_draw.window);
}

static void present(void)
{
    if (!g_draw.initialized || !g_draw.open)
        return;
    if (g_draw.gl_ready) {
        shader_present();
        g_draw.has_dirty = 0;
        return;
    }

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
    if (g_draw.gl_ready) {
        int vx, vy, vw, vh;
        double scale_x;
        double scale_y;

        shader_compute_viewport(&vx, &vy, &vw, &vh);
        scale_x = (double)g_main_width / (double)(vw > 0 ? vw : 1);
        scale_y = (double)g_main_height / (double)(vh > 0 ? vh : 1);
        *out_x = (int)((win_x - vx) * scale_x);
        *out_y = (int)((win_y - vy) * scale_y);
        if (*out_x < 0)
            *out_x = 0;
        if (*out_y < 0)
            *out_y = 0;
        if (*out_x >= g_main_width)
            *out_x = g_main_width - 1;
        if (*out_y >= g_main_height)
            *out_y = g_main_height - 1;
        return;
    }
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

        *out_x = (int)((long long)win_x * g_main_width / win_w);
        *out_y = (int)((long long)win_y * g_main_height / win_h);
    }
}

static void event_delta_to_logical(int win_dx, int win_dy,
                                   int *out_dx, int *out_dy)
{
    if (g_draw.gl_ready) {
        int vx, vy, vw, vh;
        double scale_x;
        double scale_y;

        shader_compute_viewport(&vx, &vy, &vw, &vh);
        scale_x = (double)g_main_width / (double)(vw > 0 ? vw : 1);
        scale_y = (double)g_main_height / (double)(vh > 0 ? vh : 1);
        *out_dx = (int)(win_dx * scale_x);
        *out_dy = (int)(win_dy * scale_y);
        return;
    }

    {
        int win_w = 1;
        int win_h = 1;

        SDL_GetWindowSize(g_draw.window, &win_w, &win_h);
        if (win_w <= 0)
            win_w = 1;
        if (win_h <= 0)
            win_h = 1;

        *out_dx = (int)((long long)win_dx * g_main_width / win_w);
        *out_dy = (int)((long long)win_dy * g_main_height / win_h);
    }
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
    canvas_restore_main();
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

enum {
    EASE_LINEAR,
    EASE_QUAD_IN,
    EASE_QUAD_OUT,
    EASE_QUAD_INOUT,
    EASE_CUBIC_OUT,
    EASE_SINE_INOUT,
    EASE_BACK_OUT,
    EASE_ELASTIC_OUT
};

static void particle_clear_state(void)
{
    memset(g_particles, 0, sizeof(g_particles));
}

static void tween_clear_state(void)
{
    memset(g_tweens, 0, sizeof(g_tweens));
}

static double aux_step(lua_State *L, int index, const char *where)
{
    double dt = luaL_optnumber(L, index, g_draw.frame_delta);

    if (!isfinite(dt) || dt < 0.0 || dt > 1.0)
        return luaL_error(L, "%s: dt must be finite and between 0 and 1",
                          where);
    return dt;
}

static void particle_update(double dt)
{
    int i;

    for (i = 0; i < PARTICLE_MAX; ++i) {
        ParticleEntry *p = &g_particles[i];
        double damping;

        if (!p->used)
            continue;
        p->vy += p->gravity * dt;
        damping = pow(p->drag, dt);
        p->vx *= damping;
        p->vy *= damping;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->life -= dt;
        if (p->life <= 0.0)
            memset(p, 0, sizeof(*p));
    }
}

static void particle_draw_all(void)
{
    int i;

    for (i = 0; i < PARTICLE_MAX; ++i) {
        ParticleEntry *p = &g_particles[i];
        double life = 1.0;
        uint32_t color;
        long long px;
        long long py;

        if (!p->used)
            continue;
        if (p->max_life > 0.0) {
            life = p->life / p->max_life;
            if (life < 0.0)
                life = 0.0;
            if (life > 1.0)
                life = 1.0;
        }
        color = make_color(p->r, p->g, p->b, (int)(p->a * life + 0.5));
        px = (long long)(p->x + 0.5);
        py = (long long)(p->y + 0.5);
        if (p->size <= 2)
            fill_rect(px, py, 1, 1, 0, color);
        else
            fill_circle(px, py, p->size / 2, 0, color);
    }
}

static int parse_easing(lua_State *L, int index, const char *where)
{
    const char *name = luaL_optstring(L, index, "quadout");

    if (strcmp(name, "linear") == 0)
        return EASE_LINEAR;
    if (strcmp(name, "quadin") == 0)
        return EASE_QUAD_IN;
    if (strcmp(name, "quadout") == 0)
        return EASE_QUAD_OUT;
    if (strcmp(name, "quadinout") == 0)
        return EASE_QUAD_INOUT;
    if (strcmp(name, "cubicout") == 0)
        return EASE_CUBIC_OUT;
    if (strcmp(name, "sineinout") == 0)
        return EASE_SINE_INOUT;
    if (strcmp(name, "backout") == 0)
        return EASE_BACK_OUT;
    if (strcmp(name, "elasticout") == 0)
        return EASE_ELASTIC_OUT;
    return luaL_error(L, "%s: unknown easing '%s'", where, name);
}

static double easing_value(int easing, double t)
{
    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;
    if (easing == EASE_LINEAR)
        return t;
    if (easing == EASE_QUAD_IN)
        return t * t;
    if (easing == EASE_QUAD_OUT)
        return 1.0 - (1.0 - t) * (1.0 - t);
    if (easing == EASE_QUAD_INOUT)
        return t < 0.5 ? 2.0 * t * t : 1.0 - 2.0 * (1.0 - t) * (1.0 - t);
    if (easing == EASE_CUBIC_OUT)
        return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
    if (easing == EASE_SINE_INOUT)
        return 0.5 - 0.5 * cos(AUX_PI * t);
    if (easing == EASE_BACK_OUT) {
        double c = 1.70158;
        double u = t - 1.0;

        return 1.0 + (c + 1.0) * u * u * u + c * u * u;
    }
    if (easing == EASE_ELASTIC_OUT) {
        double u = t - 1.0;

        return pow(2.0, -10.0 * t) * sin((u * 6.0 - 0.75) *
               (2.0 * AUX_PI / 3.0)) + 1.0;
    }
    return t;
}

static double tween_value(const TweenEntry *tw)
{
    double t;

    if (tw->elapsed <= tw->delay)
        return tw->from;
    t = (tw->elapsed - tw->delay) / tw->duration;
    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    return tw->from + (tw->to - tw->from) * easing_value(tw->easing, t);
}

static int tween_finished(const TweenEntry *tw)
{
    return tw->elapsed >= tw->delay + tw->duration;
}

static void tween_update(double dt)
{
    int i;

    for (i = 0; i < TWEEN_MAX; ++i) {
        if (g_tweens[i].used)
            g_tweens[i].elapsed += dt;
    }
}

static void destroy_resources(void)
{
    canvas_restore_main();
    shader_release_resources();
    particle_clear_state();
    tween_clear_state();
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
    g_main_pixels = NULL;
    g_main_width = 0;
    g_main_height = 0;
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
    g_draw.viewport_x = 0;
    g_draw.viewport_y = 0;
    g_draw.viewport_w = 0;
    g_draw.viewport_h = 0;
    g_draw.start_tick = 0;

    gamepad_shutdown();
    audio_close();
    physics_clear_state();

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
    srand((unsigned int)SDL_GetTicks());
    if (TTF_Init() != 0)
        return luaL_error(L, "draw.open: TTF_Init failed: %s", TTF_GetError());

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_draw.window = SDL_CreateWindow(
        "Lua Draw",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        (int)w,
        (int)h,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (!g_draw.window) {
        SDL_ClearError();
        g_draw.window = SDL_CreateWindow(
            "Lua Draw",
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            (int)w,
            (int)h,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    }
    if (!g_draw.window) {
        destroy_resources();
        return luaL_error(L, "draw.open: cannot create window: %s",
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
    g_main_pixels = pixels;
    g_main_width = (int)w;
    g_main_height = (int)h;
    g_canvas_target = 0;
    g_camera_x = 0.0;
    g_camera_y = 0.0;

    g_draw.vsync = 0;
    g_draw.gl_context = SDL_GL_CreateContext(g_draw.window);
    if (g_draw.gl_context) {
        if (SDL_GL_MakeCurrent(g_draw.window, g_draw.gl_context) != 0) {
            SDL_GL_DeleteContext(g_draw.gl_context);
            g_draw.gl_context = NULL;
        } else if (shader_load_functions() == 0 && shader_init_default() == 0) {
            g_draw.gl_ready = 1;
            if (SDL_GL_SetSwapInterval(1) < 0) {
                SDL_GL_SetSwapInterval(0);
            } else {
                g_draw.vsync = 1;
            }
        } else {
            shader_release_resources();
        }
    }
    if (!g_draw.gl_ready) {
        SDL_ClearError();
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
    }

    g_draw.fps = (int)fps;
    g_draw.initialized = 1;
    g_draw.open = 1;
    g_draw.has_dirty = 0;
    g_draw.start_tick = SDL_GetTicks();

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
    lua_pushinteger(L, g_main_width);
    return 1;
}

static int l_height(lua_State *L)
{
    lua_pushinteger(L, g_main_height);
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

enum {
    GRADIENT_VERTICAL,
    GRADIENT_HORIZONTAL,
    GRADIENT_DIAGONAL,
    GRADIENT_RADIAL
};

static int parse_gradient_mode(lua_State *L, int index, const char *where)
{
    const char *mode = luaL_optstring(L, index, "vertical");

    if (strcmp(mode, "vertical") == 0 || strcmp(mode, "v") == 0)
        return GRADIENT_VERTICAL;
    if (strcmp(mode, "horizontal") == 0 || strcmp(mode, "h") == 0)
        return GRADIENT_HORIZONTAL;
    if (strcmp(mode, "diagonal") == 0 || strcmp(mode, "d") == 0)
        return GRADIENT_DIAGONAL;
    if (strcmp(mode, "radial") == 0 || strcmp(mode, "r") == 0)
        return GRADIENT_RADIAL;
    return luaL_error(L, "%s: unknown gradient mode '%s'", where, mode);
}

static uint32_t gradient_mix(int r0, int g0, int b0, int a0,
                             int r1, int g1, int b1, int a1, double t)
{
    double smooth;

    if (t < 0.0)
        t = 0.0;
    if (t > 1.0)
        t = 1.0;
    smooth = t * t * (3.0 - 2.0 * t);
    return make_color(
        (int)(r0 + (r1 - r0) * smooth + 0.5),
        (int)(g0 + (g1 - g0) * smooth + 0.5),
        (int)(b0 + (b1 - b0) * smooth + 0.5),
        (int)(a0 + (a1 - a0) * smooth + 0.5));
}

static void gradient_rect(long long x, long long y, long long w, long long h,
                          int mode, int r0, int g0, int b0, int a0,
                          int r1, int g1, int b1, int a1)
{
    long long x0, y0, x1, y1;
    long long row;
    double inv_w;
    double inv_h;
    double cx;
    double cy;
    double rx;
    double ry;
    double inv_radius;

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

    x0 = x;
    y0 = y;
    x1 = x + w;
    y1 = y + h;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > g_draw.width)
        x1 = g_draw.width;
    if (y1 > g_draw.height)
        y1 = g_draw.height;
    if (x0 >= x1 || y0 >= y1)
        return;

    inv_w = 1.0 / (double)w;
    inv_h = 1.0 / (double)h;
    cx = (double)x + (double)w * 0.5;
    cy = (double)y + (double)h * 0.5;
    rx = (double)w * 0.5;
    ry = (double)h * 0.5;
    if (rx < 1.0)
        rx = 1.0;
    if (ry < 1.0)
        ry = 1.0;
    inv_radius = 1.0 / sqrt(2.0);

    for (row = y0; row < y1; ++row) {
        uint32_t *dst = g_draw.pixels + (size_t)row * (size_t)g_draw.width;
        long long col;

        for (col = x0; col < x1; ++col) {
            double t;

            if (mode == GRADIENT_HORIZONTAL) {
                t = ((double)col + 0.5 - (double)x) * inv_w;
            } else if (mode == GRADIENT_DIAGONAL) {
                double tx = ((double)col + 0.5 - (double)x) * inv_w;
                double ty = ((double)row + 0.5 - (double)y) * inv_h;

                t = (tx + ty) * 0.5;
            } else if (mode == GRADIENT_RADIAL) {
                double dx = ((double)col + 0.5 - cx) / rx;
                double dy = ((double)row + 0.5 - cy) / ry;

                t = sqrt(dx * dx + dy * dy) * inv_radius;
            } else {
                t = ((double)row + 0.5 - (double)y) * inv_h;
            }
            dst[col] = blend_color(
                dst[col],
                gradient_mix(r0, g0, b0, a0, r1, g1, b1, a1, t));
        }
    }
    mark_dirty_rect((int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0));
}

static int l_gradient(lua_State *L)
{
    lua_Integer x, y, w, h;
    int r0, g0, b0, a0;
    int r1, g1, b1, a1;
    int mode;

    if (!ensure_open(L, "draw.gradient"))
        return 0;

    x = luaL_checkinteger(L, 1);
    y = luaL_checkinteger(L, 2);
    w = luaL_checkinteger(L, 3);
    h = luaL_checkinteger(L, 4);
    if (x < -1000000000LL || x > 1000000000LL ||
        y < -1000000000LL || y > 1000000000LL ||
        w < -1000000000LL || w > 1000000000LL ||
        h < -1000000000LL || h > 1000000000LL)
        return luaL_error(L, "draw.gradient: coordinates are too large");
    if (read_color(L, 5, &r0, &g0, &b0, &a0) < 0)
        return luaL_error(L, "draw.gradient: expected the first color");
    if (read_color(L, 6, &r1, &g1, &b1, &a1) < 0)
        return luaL_error(L, "draw.gradient: expected the second color");
    mode = parse_gradient_mode(L, 7, "draw.gradient");

    gradient_rect((long long)x, (long long)y, (long long)w, (long long)h,
                  mode, r0, g0, b0, a0, r1, g1, b1, a1);
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

#define PHYSICS_BODY_MAX 512
#define PHYSICS_JOINT_MAX 256
#define PHYSICS_CONTACT_MAX 1024
#define PHYSICS_ITERATIONS 6
#define PHYSICS_SLOP 0.01
#define PHYSICS_CORRECTION 0.8
#define PHYSICS_DENSITY 0.001
#define PHYSICS_COORD_LIMIT 1000000000.0
#define PHYSICS_SPEED_LIMIT 1000000.0
#define PHYSICS_ANGULAR_LIMIT 12.56637061435917295385
#define PHYSICS_REST_THRESHOLD 1.0

typedef enum {
    PHYSICS_CIRCLE = 0,
    PHYSICS_BOX,
    PHYSICS_TRIANGLE
} PhysicsShapeKind;

typedef enum {
    PHYSICS_DYNAMIC = 0,
    PHYSICS_STATIC,
    PHYSICS_KINEMATIC
} PhysicsBodyType;

typedef struct {
    double x, y;
} PhysicsVec;

typedef struct {
    int used;
    int id;
    PhysicsShapeKind shape;
    PhysicsBodyType type;
    double x, y, angle;
    double vx, vy, omega;
    double fx, fy, torque;
    double mass, inv_mass, inertia, inv_inertia;
    double restitution, friction;
    double linear_damping, angular_damping;
    uint32_t category, mask;
    int sensor;
    double radius;
    double half_w, half_h;
    PhysicsVec verts[3];
    char sprite[SPRITE_NAME_MAX];
    double sprite_scale;
    int has_sprite;
    char color[PALETTE_NAME_MAX];
    int has_color;
} PhysicsBody;

typedef struct {
    int used;
    int id;
    int kind;
    int a, b;
    double rest, stiffness, damping;
    PhysicsVec local_a, local_b;
} PhysicsJoint;

typedef struct {
    int a, b;
    double nx, ny, px, py, penetration;
    double impulse, tangent_impulse, bias;
    int sensor;
} PhysicsContact;

static PhysicsBody g_physics_bodies[PHYSICS_BODY_MAX];
static PhysicsJoint g_physics_joints[PHYSICS_JOINT_MAX];
static PhysicsContact g_physics_contacts[PHYSICS_CONTACT_MAX];
static int g_physics_contact_count;
static PhysicsVec g_physics_gravity;
static int g_physics_callback = LUA_NOREF;

static void physics_clear_state(void)
{
    memset(g_physics_bodies, 0, sizeof(g_physics_bodies));
    memset(g_physics_joints, 0, sizeof(g_physics_joints));
    memset(g_physics_contacts, 0, sizeof(g_physics_contacts));
    g_physics_contact_count = 0;
    g_physics_gravity.x = 0.0;
    g_physics_gravity.y = 0.0;
    tilemap_clear_bodyrefs();
}

static PhysicsVec physics_vec(double x, double y)
{
    PhysicsVec v;
    v.x = x;
    v.y = y;
    return v;
}

static double physics_dot(PhysicsVec a, PhysicsVec b)
{
    return a.x * b.x + a.y * b.y;
}

static double physics_cross(PhysicsVec a, PhysicsVec b)
{
    return a.x * b.y - a.y * b.x;
}

static double physics_length(PhysicsVec v)
{
    return sqrt(physics_dot(v, v));
}

static int physics_normalize(PhysicsVec *v)
{
    double len = physics_length(*v);

    if (!(len > 1e-12) || !isfinite(len))
        return 0;
    v->x /= len;
    v->y /= len;
    return 1;
}

static PhysicsVec physics_rotate(PhysicsVec v, double c, double s)
{
    return physics_vec(v.x * c - v.y * s, v.x * s + v.y * c);
}

static double physics_deg_to_rad(double deg)
{
    return deg * 0.017453292519943295769236907684886;
}

static int physics_rad_to_deg(double rad)
{
    double deg = rad * 57.295779513082320876798154814105;
    if (deg >= 0.0)
        return (int)(deg + 0.5);
    return (int)(deg - 0.5);
}

static double physics_round(double v)
{
    if (v >= 0.0)
        return floor(v + 0.5);
    return ceil(v - 0.5);
}

static int physics_read_number_field(lua_State *L, int table,
                                     const char *field, double fallback,
                                     double *out, const char *where)
{
    lua_getfield(L, table, field);
    if (lua_isnoneornil(L, -1)) {
        lua_pop(L, 1);
        *out = fallback;
        return 0;
    }
    if (lua_type(L, -1) != LUA_TNUMBER) {
        lua_pop(L, 1);
        return luaL_error(L, "%s: %s must be a number", where, field);
    }
    *out = (double)lua_tonumber(L, -1);
    lua_pop(L, 1);
    if (!isfinite(*out))
        return luaL_error(L, "%s: %s must be finite", where, field);
    return 0;
}

static int physics_read_bool_field(lua_State *L, int table,
                                   const char *field, int fallback,
                                   int *out, const char *where)
{
    lua_getfield(L, table, field);
    if (lua_isnoneornil(L, -1)) {
        lua_pop(L, 1);
        *out = fallback;
        return 0;
    }
    if (lua_type(L, -1) != LUA_TBOOLEAN) {
        lua_pop(L, 1);
        return luaL_error(L, "%s: %s must be a boolean", where, field);
    }
    *out = lua_toboolean(L, -1);
    lua_pop(L, 1);
    return 0;
}

static uint32_t physics_parse_u32_value(lua_State *L, int index,
                                        uint32_t fallback,
                                        const char *where, const char *field)
{
    lua_Integer value;

    if (lua_isnoneornil(L, index))
        return fallback;
    if (lua_type(L, index) != LUA_TNUMBER)
        return (uint32_t)luaL_error(L, "%s: %s must be an integer",
                                    where, field);
    value = lua_tointeger(L, index);
    if (value < 0 || (unsigned long long)value > 0xffffffffULL)
        return (uint32_t)luaL_error(L, "%s: %s must be 0..4294967295",
                                    where, field);
    return (uint32_t)value;
}

static int physics_read_u32_field(lua_State *L, int table,
                                  const char *field, uint32_t fallback,
                                  uint32_t *out, const char *where)
{
    lua_getfield(L, table, field);
    *out = physics_parse_u32_value(L, -1, fallback, where, field);
    lua_pop(L, 1);
    return 0;
}

static PhysicsBody *physics_body_by_id(int id, const char *where)
{
    PhysicsBody *body;

    if (id <= 0 || id > PHYSICS_BODY_MAX)
        return NULL;
    body = &g_physics_bodies[id - 1];
    if (!body->used || body->id != id)
        return NULL;
    (void)where;
    return body;
}

static PhysicsJoint *physics_joint_by_id(int id, const char *where)
{
    PhysicsJoint *joint;

    if (id <= 0 || id > PHYSICS_JOINT_MAX)
        return NULL;
    joint = &g_physics_joints[id - 1];
    if (!joint->used || joint->id != id)
        return NULL;
    (void)where;
    return joint;
}

static void physics_body_vertices(PhysicsBody *body, PhysicsVec out[4],
                                  int *count)
{
    double c = cos(body->angle);
    double s = sin(body->angle);
    int i;

    if (body->shape == PHYSICS_BOX) {
        PhysicsVec local[4];
        local[0] = physics_vec(-body->half_w, -body->half_h);
        local[1] = physics_vec(body->half_w, -body->half_h);
        local[2] = physics_vec(body->half_w, body->half_h);
        local[3] = physics_vec(-body->half_w, body->half_h);
        for (i = 0; i < 4; ++i) {
            PhysicsVec r = physics_rotate(local[i], c, s);
            out[i] = physics_vec(body->x + r.x, body->y + r.y);
        }
        *count = 4;
        return;
    }
    for (i = 0; i < 3; ++i) {
        PhysicsVec r = physics_rotate(body->verts[i], c, s);
        out[i] = physics_vec(body->x + r.x, body->y + r.y);
    }
    *count = 3;
}

static void physics_body_bounds(PhysicsBody *body,
                                double *min_x, double *min_y,
                                double *max_x, double *max_y)
{
    PhysicsVec verts[4];
    int count;
    int i;

    if (body->shape == PHYSICS_CIRCLE) {
        *min_x = body->x - body->radius;
        *max_x = body->x + body->radius;
        *min_y = body->y - body->radius;
        *max_y = body->y + body->radius;
        return;
    }
    physics_body_vertices(body, verts, &count);
    *min_x = *max_x = verts[0].x;
    *min_y = *max_y = verts[0].y;
    for (i = 1; i < count; ++i) {
        if (verts[i].x < *min_x) *min_x = verts[i].x;
        if (verts[i].x > *max_x) *max_x = verts[i].x;
        if (verts[i].y < *min_y) *min_y = verts[i].y;
        if (verts[i].y > *max_y) *max_y = verts[i].y;
    }
}

static double physics_shape_area(PhysicsBody *body)
{
    if (body->shape == PHYSICS_CIRCLE)
        return 3.14159265358979323846 * body->radius * body->radius;
    if (body->shape == PHYSICS_BOX)
        return body->half_w * 2.0 * body->half_h * 2.0;
    return fabs(physics_cross(
        physics_vec(body->verts[1].x - body->verts[0].x,
                    body->verts[1].y - body->verts[0].y),
        physics_vec(body->verts[2].x - body->verts[0].x,
                    body->verts[2].y - body->verts[0].y))) * 0.5;
}

static PhysicsVec physics_closest_on_segment(PhysicsVec p,
                                             PhysicsVec a, PhysicsVec b)
{
    PhysicsVec ab = physics_vec(b.x - a.x, b.y - a.y);
    double len2 = physics_dot(ab, ab);
    double t;

    if (len2 <= 1e-12)
        return a;
    t = physics_dot(physics_vec(p.x - a.x, p.y - a.y), ab) / len2;
    if (t < 0.0) t = 0.0;
    if (t > 1.0) t = 1.0;
    return physics_vec(a.x + ab.x * t, a.y + ab.y * t);
}

static int physics_point_in_poly(PhysicsVec p, PhysicsVec *verts, int count)
{
    int inside = 0;
    int i;

    for (i = 0; i < count; ++i) {
        PhysicsVec a = verts[i];
        PhysicsVec b = verts[(i + 1) % count];
        if (((a.y > p.y) != (b.y > p.y)) &&
            (p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x))
            inside = !inside;
    }
    return inside;
}

static PhysicsVec physics_poly_support(PhysicsVec *verts, int count,
                                       PhysicsVec dir)
{
    PhysicsVec best = verts[0];
    double best_dot = physics_dot(best, dir);
    int i;

    for (i = 1; i < count; ++i) {
        double value = physics_dot(verts[i], dir);
        if (value > best_dot) {
            best_dot = value;
            best = verts[i];
        }
    }
    return best;
}

static int physics_collide_circles(PhysicsBody *a, PhysicsBody *b,
                                   PhysicsContact *out)
{
    PhysicsVec delta = physics_vec(b->x - a->x, b->y - a->y);
    double dist = physics_length(delta);
    double radius = a->radius + b->radius;
    double penetration;
    PhysicsVec normal;

    if (dist >= radius)
        return 0;
    if (!physics_normalize(&delta))
        delta = physics_vec(1.0, 0.0);
    normal = delta;
    penetration = radius - dist;
    out->nx = normal.x;
    out->ny = normal.y;
    out->px = a->x + normal.x * (a->radius - penetration * 0.5);
    out->py = a->y + normal.y * (a->radius - penetration * 0.5);
    out->penetration = penetration;
    return 1;
}

static int physics_collide_polys(PhysicsBody *a, PhysicsBody *b,
                                 PhysicsContact *out)
{
    PhysicsVec av[4], bv[4];
    int an, bn;
    double best = INFINITY;
    PhysicsVec best_axis = physics_vec(0.0, 0.0);
    int i;

    physics_body_vertices(a, av, &an);
    physics_body_vertices(b, bv, &bn);
    for (i = 0; i < an + bn; ++i) {
        PhysicsVec *verts = i < an ? av : bv;
        int count = i < an ? an : bn;
        int j = i < an ? i : i - an;
        PhysicsVec edge = physics_vec(verts[(j + 1) % count].x - verts[j].x,
                                      verts[(j + 1) % count].y - verts[j].y);
        PhysicsVec axis = physics_vec(-edge.y, edge.x);
        double min_a, max_a, min_b, max_b, overlap;
        int k;

        if (!physics_normalize(&axis))
            continue;
        min_a = max_a = physics_dot(av[0], axis);
        min_b = max_b = physics_dot(bv[0], axis);
        for (k = 1; k < an; ++k) {
            double value = physics_dot(av[k], axis);
            if (value < min_a) min_a = value;
            if (value > max_a) max_a = value;
        }
        for (k = 1; k < bn; ++k) {
            double value = physics_dot(bv[k], axis);
            if (value < min_b) min_b = value;
            if (value > max_b) max_b = value;
        }
        overlap = (max_a < max_b ? max_a : max_b) -
                  (min_a > min_b ? min_a : min_b);
        if (overlap <= 0.0)
            return 0;
        if (overlap < best) {
            PhysicsVec centers = physics_vec(b->x - a->x, b->y - a->y);
            best = overlap;
            best_axis = axis;
            if (physics_dot(centers, axis) < 0.0) {
                best_axis.x = -axis.x;
                best_axis.y = -axis.y;
            }
        }
    }
    if (!isfinite(best) || !physics_normalize(&best_axis))
        return 0;
    {
        PhysicsVec tangent = physics_vec(-best_axis.y, best_axis.x);
        PhysicsVec ca = physics_vec(a->x, a->y);
        PhysicsVec cb = physics_vec(b->x, b->y);
        PhysicsVec pa = physics_poly_support(av, an, best_axis);
        PhysicsVec pb = physics_poly_support(bv, bn,
                                              physics_vec(-best_axis.x,
                                                          -best_axis.y));
        double npos = (physics_dot(pa, best_axis) +
                       physics_dot(pb, best_axis)) * 0.5;
        double tpos = (physics_dot(ca, tangent) +
                       physics_dot(cb, tangent)) * 0.5;
        double amin = physics_dot(av[0], tangent);
        double amax = amin;
        double bmin = physics_dot(bv[0], tangent);
        double bmax = bmin;
        double lo, hi;
        int k;

        for (k = 1; k < an; ++k) {
            double value = physics_dot(av[k], tangent);
            if (value < amin) amin = value;
            if (value > amax) amax = value;
        }
        for (k = 1; k < bn; ++k) {
            double value = physics_dot(bv[k], tangent);
            if (value < bmin) bmin = value;
            if (value > bmax) bmax = value;
        }
        lo = amin > bmin ? amin : bmin;
        hi = amax < bmax ? amax : bmax;
        if (tpos < lo) tpos = lo;
        if (tpos > hi) tpos = hi;
        out->px = best_axis.x * npos + tangent.x * tpos;
        out->py = best_axis.y * npos + tangent.y * tpos;
    }
    out->nx = best_axis.x;

    out->ny = best_axis.y;
    out->penetration = best;
    return 1;
}

static int physics_collide_circle_poly(PhysicsBody *circle, PhysicsBody *poly,
                                       PhysicsContact *out)
{
    PhysicsVec verts[4];
    PhysicsVec closest;
    PhysicsVec delta;
    int count;
    double best = INFINITY;
    double dist;
    int inside;
    int i;

    physics_body_vertices(poly, verts, &count);
    closest = physics_closest_on_segment(
        physics_vec(circle->x, circle->y), verts[0], verts[1]);
    for (i = 0; i < count; ++i) {
        PhysicsVec point = physics_closest_on_segment(
            physics_vec(circle->x, circle->y), verts[i],
            verts[(i + 1) % count]);
        PhysicsVec d = physics_vec(point.x - circle->x,
                                   point.y - circle->y);
        double value = physics_dot(d, d);
        if (value < best) {
            best = value;
            closest = point;
        }
    }
    dist = sqrt(best);
    inside = physics_point_in_poly(physics_vec(circle->x, circle->y),
                                   verts, count);
    if (!inside) {
        if (dist >= circle->radius)
            return 0;
        delta = physics_vec(closest.x - circle->x, closest.y - circle->y);
        out->penetration = circle->radius - dist;
    } else {
        delta = physics_vec(circle->x - closest.x, circle->y - closest.y);
        out->penetration = circle->radius + dist;
    }
    if (!physics_normalize(&delta))
        delta = physics_vec(1.0, 0.0);
    out->nx = delta.x;
    out->ny = delta.y;
    out->px = closest.x;
    out->py = closest.y;
    return 1;
}

static int physics_collide_pair(PhysicsBody *a, PhysicsBody *b,
                                PhysicsContact *out)
{
    int result;

    memset(out, 0, sizeof(*out));
    out->a = a->id;
    out->b = b->id;
    out->sensor = a->sensor || b->sensor;
    if (a->shape == PHYSICS_CIRCLE && b->shape == PHYSICS_CIRCLE)
        result = physics_collide_circles(a, b, out);
    else if (a->shape != PHYSICS_CIRCLE && b->shape != PHYSICS_CIRCLE)
        result = physics_collide_polys(a, b, out);
    else if (a->shape == PHYSICS_CIRCLE)
        result = physics_collide_circle_poly(a, b, out);
    else {
        result = physics_collide_circle_poly(b, a, out);
        out->nx = -out->nx;
        out->ny = -out->ny;
    }
    return result;
}

static int physics_can_collide(PhysicsBody *a, PhysicsBody *b)
{
    if (!a->sensor && !b->sensor && a->inv_mass == 0.0 &&
        b->inv_mass == 0.0)
        return 0;
    return ((a->category & b->mask) != 0 && (b->category & a->mask) != 0);
}

static int physics_store_contact(PhysicsContact *contact)
{
    if (g_physics_contact_count >= PHYSICS_CONTACT_MAX)
        return 0;
    g_physics_contacts[g_physics_contact_count++] = *contact;
    return 1;
}

static void physics_detect_contacts(void)
{
    int i, j;

    for (i = 0; i < PHYSICS_BODY_MAX; ++i) {
        PhysicsBody *a = &g_physics_bodies[i];
        double ax0, ay0, ax1, ay1;

        if (!a->used)
            continue;
        physics_body_bounds(a, &ax0, &ay0, &ax1, &ay1);
        for (j = i + 1; j < PHYSICS_BODY_MAX; ++j) {
            PhysicsBody *b = &g_physics_bodies[j];
            double bx0, by0, bx1, by1;
            PhysicsContact contact;

            if (!b->used || !physics_can_collide(a, b))
                continue;
            physics_body_bounds(b, &bx0, &by0, &bx1, &by1);
            if (ax1 < bx0 || bx1 < ax0 || ay1 < by0 || by1 < ay0)
                continue;
            if (physics_collide_pair(a, b, &contact))
                physics_store_contact(&contact);
        }
    }
}

static void physics_apply_impulse(PhysicsBody *body, PhysicsVec impulse,
                                  PhysicsVec r)
{
    body->vx += impulse.x * body->inv_mass;
    body->vy += impulse.y * body->inv_mass;
    body->omega += physics_cross(r, impulse) * body->inv_inertia;
}

static void physics_prepare_contact(PhysicsContact *contact)
{
    PhysicsBody *a = physics_body_by_id(contact->a, "draw.physicsstep");
    PhysicsBody *b = physics_body_by_id(contact->b, "draw.physicsstep");
    PhysicsVec n, ra, rb, rv;
    double vel, restitution;

    contact->impulse = 0.0;
    contact->tangent_impulse = 0.0;
    contact->bias = 0.0;
    if (!a || !b || contact->sensor)
        return;
    n = physics_vec(contact->nx, contact->ny);
    ra = physics_vec(contact->px - a->x, contact->py - a->y);
    rb = physics_vec(contact->px - b->x, contact->py - b->y);
    rv = physics_vec(
        b->vx - b->omega * rb.y - a->vx + a->omega * ra.y,
        b->vy + b->omega * rb.x - a->vy - a->omega * ra.x);
    vel = physics_dot(rv, n);
    restitution = a->restitution > b->restitution
                      ? a->restitution : b->restitution;
    if (vel < -PHYSICS_REST_THRESHOLD)
        contact->bias = -restitution * vel;
}

static void physics_solve_contact(PhysicsContact *contact)
{
    PhysicsBody *a = physics_body_by_id(contact->a, "draw.physicsstep");
    PhysicsBody *b = physics_body_by_id(contact->b, "draw.physicsstep");
    PhysicsVec n, ra, rb, rv, tangent;
    double ra_cross_n, rb_cross_n;
    double inv_mass, denom;
    double vel, impulse, delta_impulse;
    double mu, limit;

    if (!a || !b || contact->sensor)
        return;
    n = physics_vec(contact->nx, contact->ny);
    ra = physics_vec(contact->px - a->x, contact->py - a->y);
    rb = physics_vec(contact->px - b->x, contact->py - b->y);
    inv_mass = a->inv_mass + b->inv_mass;
    if (inv_mass <= 0.0)
        return;

    rv = physics_vec(
        b->vx - b->omega * rb.y - a->vx + a->omega * ra.y,
        b->vy + b->omega * rb.x - a->vy - a->omega * ra.x);
    vel = physics_dot(rv, n);
    ra_cross_n = physics_cross(ra, n);
    rb_cross_n = physics_cross(rb, n);
    denom = inv_mass +
            ra_cross_n * ra_cross_n * a->inv_inertia +
            rb_cross_n * rb_cross_n * b->inv_inertia;
    if (denom <= 0.0)
        return;
    impulse = contact->impulse + (-vel + contact->bias) / denom;
    if (impulse < 0.0)
        impulse = 0.0;
    delta_impulse = impulse - contact->impulse;
    contact->impulse = impulse;
    physics_apply_impulse(a, physics_vec(-n.x * delta_impulse,
                                         -n.y * delta_impulse), ra);
    physics_apply_impulse(b, physics_vec(n.x * delta_impulse,
                                         n.y * delta_impulse), rb);

    rv = physics_vec(
        b->vx - b->omega * rb.y - a->vx + a->omega * ra.y,
        b->vy + b->omega * rb.x - a->vy - a->omega * ra.x);
    tangent = physics_vec(rv.x - n.x * physics_dot(rv, n),
                          rv.y - n.y * physics_dot(rv, n));
    if (!physics_normalize(&tangent))
        return;
    ra_cross_n = physics_cross(ra, tangent);
    rb_cross_n = physics_cross(rb, tangent);
    denom = inv_mass +
            ra_cross_n * ra_cross_n * a->inv_inertia +
            rb_cross_n * rb_cross_n * b->inv_inertia;
    if (denom <= 0.0)
        return;
    impulse = contact->tangent_impulse - physics_dot(rv, tangent) / denom;
    mu = sqrt(a->friction * b->friction);
    limit = mu * contact->impulse;
    if (impulse > limit)
        impulse = limit;
    if (impulse < -limit)
        impulse = -limit;
    delta_impulse = impulse - contact->tangent_impulse;
    contact->tangent_impulse = impulse;
    physics_apply_impulse(a, physics_vec(-tangent.x * delta_impulse,
                                         -tangent.y * delta_impulse), ra);
    physics_apply_impulse(b, physics_vec(tangent.x * delta_impulse,
                                         tangent.y * delta_impulse), rb);
}

static void physics_correct_contacts(int start)
{
    int i;

    for (i = start; i < g_physics_contact_count; ++i) {
        PhysicsContact *contact = &g_physics_contacts[i];
        PhysicsBody *a = physics_body_by_id(contact->a, "draw.physicsstep");
        PhysicsBody *b = physics_body_by_id(contact->b, "draw.physicsstep");
        PhysicsVec n;
        double inv_mass, separation, correction;

        if (!a || !b || contact->sensor)
            continue;
        n = physics_vec(contact->nx, contact->ny);
        inv_mass = a->inv_mass + b->inv_mass;
        if (inv_mass <= 0.0)
            continue;
        separation = contact->penetration - PHYSICS_SLOP;
        if (separation <= 0.0)
            continue;
        correction = separation * PHYSICS_CORRECTION / inv_mass;
        a->x -= n.x * correction * a->inv_mass;
        a->y -= n.y * correction * a->inv_mass;
        b->x += n.x * correction * b->inv_mass;
        b->y += n.y * correction * b->inv_mass;
    }
}

static PhysicsVec physics_local_to_world(PhysicsBody *body, PhysicsVec local)
{
    double c = cos(body->angle);
    double s = sin(body->angle);
    PhysicsVec r = physics_rotate(local, c, s);
    return physics_vec(body->x + r.x, body->y + r.y);
}

static void physics_solve_joint(PhysicsJoint *joint)
{
    PhysicsBody *a = physics_body_by_id(joint->a, "draw.physicsstep");
    PhysicsBody *b = physics_body_by_id(joint->b, "draw.physicsstep");
    PhysicsVec pa, pb, delta, normal, rel;
    double dist, diff, inv_mass, force, correction;

    if (!a || !b)
        return;
    if (joint->kind == 2) {
        PhysicsVec wa = physics_local_to_world(a, joint->local_a);
        PhysicsVec wb = physics_local_to_world(b, joint->local_b);
        PhysicsVec correction = physics_vec((wb.x - wa.x) * joint->stiffness,
                                            (wb.y - wa.y) * joint->stiffness);
        double sum = a->inv_mass + b->inv_mass + a->inv_inertia +
                     b->inv_inertia;
        double la = physics_cross(joint->local_a, correction);
        double lb = physics_cross(joint->local_b, correction);

        if (sum <= 0.0)
            return;
        a->x += correction.x * a->inv_mass / sum;
        a->y += correction.y * a->inv_mass / sum;
        b->x -= correction.x * b->inv_mass / sum;
        b->y -= correction.y * b->inv_mass / sum;
        a->angle += la * a->inv_inertia / sum;
        b->angle -= lb * b->inv_inertia / sum;
        rel = physics_vec(
            b->vx - b->omega * (wb.y - b->y) - a->vx +
                a->omega * (wa.y - a->y),
            b->vy + b->omega * (wb.x - b->x) - a->vy -
                a->omega * (wa.x - a->x));
        a->vx += rel.x * joint->damping * a->inv_mass;
        a->vy += rel.y * joint->damping * a->inv_mass;
        b->vx -= rel.x * joint->damping * b->inv_mass;
        b->vy -= rel.y * joint->damping * b->inv_mass;
        return;
    }

    pa = physics_vec(a->x, a->y);
    pb = physics_vec(b->x, b->y);
    delta = physics_vec(pb.x - pa.x, pb.y - pa.y);
    dist = physics_length(delta);
    if (!physics_normalize(&delta))
        delta = physics_vec(1.0, 0.0);
    normal = delta;
    diff = dist - joint->rest;
    rel = physics_vec(b->vx - a->vx, b->vy - a->vy);

    if (joint->kind == 1) {
        force = joint->stiffness * diff +
                joint->damping * physics_dot(rel, normal);
        a->fx += force * normal.x;
        a->fy += force * normal.y;
        b->fx -= force * normal.x;
        b->fy -= force * normal.y;
        return;
    }

    inv_mass = a->inv_mass + b->inv_mass;
    if (inv_mass <= 0.0)
        return;
    correction = diff * joint->stiffness / inv_mass;
    a->x += normal.x * correction * a->inv_mass;
    a->y += normal.y * correction * a->inv_mass;
    b->x -= normal.x * correction * b->inv_mass;
    b->y -= normal.y * correction * b->inv_mass;
    force = physics_dot(rel, normal) * joint->damping / inv_mass;
    a->vx += normal.x * force * a->inv_mass;
    a->vy += normal.y * force * a->inv_mass;
    b->vx -= normal.x * force * b->inv_mass;
    b->vy -= normal.y * force * b->inv_mass;
}

static void physics_clamp_body(PhysicsBody *body)
{
    if (!isfinite(body->x) || !isfinite(body->y)) {
        body->x = 0.0;
        body->y = 0.0;
        body->vx = 0.0;
        body->vy = 0.0;
        body->omega = 0.0;
    }
    if (!isfinite(body->vx)) body->vx = 0.0;
    if (!isfinite(body->vy)) body->vy = 0.0;
    if (!isfinite(body->omega)) body->omega = 0.0;
    if (!isfinite(body->angle)) body->angle = 0.0;
    if (body->vx > PHYSICS_SPEED_LIMIT) body->vx = PHYSICS_SPEED_LIMIT;
    if (body->vx < -PHYSICS_SPEED_LIMIT) body->vx = -PHYSICS_SPEED_LIMIT;
    if (body->vy > PHYSICS_SPEED_LIMIT) body->vy = PHYSICS_SPEED_LIMIT;
    if (body->vy < -PHYSICS_SPEED_LIMIT) body->vy = -PHYSICS_SPEED_LIMIT;
    if (body->omega > PHYSICS_ANGULAR_LIMIT)
        body->omega = PHYSICS_ANGULAR_LIMIT;
    if (body->omega < -PHYSICS_ANGULAR_LIMIT)
        body->omega = -PHYSICS_ANGULAR_LIMIT;
}

static void physics_integrate(double dt)
{
    int i;

    for (i = 0; i < PHYSICS_BODY_MAX; ++i) {
        PhysicsBody *body = &g_physics_bodies[i];
        double linear_scale;
        double angular_scale;

        if (!body->used || body->inv_mass == 0.0)
            continue;
        body->vx += (g_physics_gravity.x + body->fx * body->inv_mass) * dt;
        body->vy += (g_physics_gravity.y + body->fy * body->inv_mass) * dt;
        body->omega += body->torque * body->inv_inertia * dt;
        linear_scale = 1.0 / (1.0 + body->linear_damping * dt);
        angular_scale = 1.0 / (1.0 + body->angular_damping * dt);
        body->vx *= linear_scale;
        body->vy *= linear_scale;
        body->omega *= angular_scale;
        physics_clamp_body(body);
        body->x += body->vx * dt;
        body->y += body->vy * dt;
        body->angle += body->omega * dt;
        body->fx = 0.0;
        body->fy = 0.0;
        body->torque = 0.0;
    }
}

static int physics_parse_body(lua_State *L, int index, PhysicsBody *body)
{
    const char *where = "draw.physicsbody";
    const char *type_name;
    const char *sprite_name;
    double density;
    double mass;
    double scale;
    int sprite_index;
    size_t name_len;
    int shape_index;
    int i;

    if (lua_type(L, index) != LUA_TTABLE)
        return luaL_error(L, "%s: expected a table", where);
    memset(body, 0, sizeof(*body));
    body->type = PHYSICS_DYNAMIC;
    body->restitution = 0.0;
    body->friction = 0.5;
    body->category = 0xffffffffU;
    body->mask = 0xffffffffU;
    body->sprite_scale = 1.0;

    lua_getfield(L, index, "type");
    if (!lua_isnoneornil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            return luaL_error(L, "%s: type must be a string", where);
        }
        type_name = lua_tostring(L, -1);
        if (strcmp(type_name, "dynamic") == 0)
            body->type = PHYSICS_DYNAMIC;
        else if (strcmp(type_name, "static") == 0)
            body->type = PHYSICS_STATIC;
        else if (strcmp(type_name, "kinematic") == 0)
            body->type = PHYSICS_KINEMATIC;
        else {
            lua_pop(L, 1);
            return luaL_error(L,
                "%s: type must be dynamic, static, or kinematic", where);
        }
    }
    lua_pop(L, 1);

    if (physics_read_number_field(L, index, "x", 0.0, &body->x, where) ||
        physics_read_number_field(L, index, "y", 0.0, &body->y, where) ||
        physics_read_number_field(L, index, "angle", 0.0, &body->angle, where) ||
        physics_read_number_field(L, index, "vx", 0.0, &body->vx, where) ||
        physics_read_number_field(L, index, "vy", 0.0, &body->vy, where) ||
        physics_read_number_field(L, index, "omega", 0.0, &body->omega, where) ||
        physics_read_number_field(L, index, "restitution", 0.0,
                                  &body->restitution, where) ||
        physics_read_number_field(L, index, "friction", 0.5,
                                  &body->friction, where) ||
        physics_read_number_field(L, index, "lineardamping", 0.0,
                                  &body->linear_damping, where) ||
        physics_read_number_field(L, index, "angulardamping", 0.0,
                                  &body->angular_damping, where) ||
        physics_read_number_field(L, index, "density", PHYSICS_DENSITY,
                                  &density, where) ||
        physics_read_number_field(L, index, "mass", -1.0, &mass, where) ||
        physics_read_number_field(L, index, "scale", 1.0, &scale, where))
        return luaL_error(L, "%s: invalid value", where);
    if (fabs(body->x) > PHYSICS_COORD_LIMIT ||
        fabs(body->y) > PHYSICS_COORD_LIMIT ||
        fabs(body->vx) > PHYSICS_SPEED_LIMIT ||
        fabs(body->vy) > PHYSICS_SPEED_LIMIT ||
        fabs(body->omega) > 100000.0)
        return luaL_error(L, "%s: position or velocity is too large", where);
    if (body->restitution < 0.0 || body->restitution > 1.0)
        return luaL_error(L, "%s: restitution must be 0..1", where);
    if (body->friction < 0.0 || body->friction > 1000.0)
        return luaL_error(L, "%s: friction must be 0..1000", where);
    if (body->linear_damping < 0.0 || body->angular_damping < 0.0 ||
        density <= 0.0 || scale < 1.0 || scale > 1000000.0)
        return luaL_error(L, "%s: invalid body parameters", where);
    if (physics_read_bool_field(L, index, "sensor", 0, &body->sensor, where) ||
        physics_read_u32_field(L, index, "category", 0xffffffffU,
                               &body->category, where) ||
        physics_read_u32_field(L, index, "mask", 0xffffffffU,
                               &body->mask, where))
        return luaL_error(L, "%s: invalid filter", where);
    lua_getfield(L, index, "color");
    if (!lua_isnoneornil(L, -1)) {
        if (lua_type(L, -1) != LUA_TSTRING) {
            lua_pop(L, 1);
            return luaL_error(L, "%s: color must be a palette name", where);
        }
        name_len = strlen(lua_tostring(L, -1));
        if (name_len >= sizeof(body->color)) {
            lua_pop(L, 1);
            return luaL_error(L, "%s: color name is too long", where);
        }
        strcpy(body->color, lua_tostring(L, -1));
        body->has_color = 1;
    }
    lua_pop(L, 1);

    lua_getfield(L, index, "circle");
    if (!lua_isnoneornil(L, -1)) {
        shape_index = lua_gettop(L);
        if (lua_type(L, shape_index) != LUA_TTABLE) {
            lua_pop(L, 1);
            return luaL_error(L, "%s: circle must be a table", where);
        }
        if (physics_read_number_field(L, shape_index, "radius", -1.0,
                                      &body->radius, where))
            return luaL_error(L, "%s: invalid radius", where);
        lua_pop(L, 1);
        if (body->radius <= 0.0 || body->radius > PHYSICS_COORD_LIMIT)
            return luaL_error(L, "%s: radius must be positive", where);
        body->shape = PHYSICS_CIRCLE;
    } else {
        lua_pop(L, 1);
        lua_getfield(L, index, "box");
        if (!lua_isnoneornil(L, -1)) {
            double width, height;

            shape_index = lua_gettop(L);
            if (lua_type(L, shape_index) != LUA_TTABLE) {
                lua_pop(L, 1);
                return luaL_error(L, "%s: box must be a table", where);
            }
            if (physics_read_number_field(L, shape_index, "width", -1.0,
                                          &width, where) ||
                physics_read_number_field(L, shape_index, "height", -1.0,
                                          &height, where)) {
                lua_pop(L, 1);
                return luaL_error(L, "%s: invalid box size", where);
            }
            lua_pop(L, 1);
            if (width <= 0.0 || height <= 0.0 ||
                width > PHYSICS_COORD_LIMIT || height > PHYSICS_COORD_LIMIT)
                return luaL_error(L, "%s: box size must be positive", where);
            body->half_w = width * 0.5;
            body->half_h = height * 0.5;
            body->shape = PHYSICS_BOX;
        } else {
            lua_pop(L, 1);
            lua_getfield(L, index, "sprite");
            if (!lua_isnoneornil(L, -1)) {
                SpriteEntry *sprite;

                if (lua_type(L, -1) != LUA_TSTRING) {
                    lua_pop(L, 1);
                    return luaL_error(L, "%s: sprite must be a name", where);
                }
                name_len = strlen(lua_tostring(L, -1));
                if (name_len == 0 || name_len >= sizeof(body->sprite)) {
                    lua_pop(L, 1);
                    return luaL_error(L, "%s: sprite name is invalid", where);
                }
                sprite_name = lua_tostring(L, -1);
                sprite_index = find_sprite_name(sprite_name);
                if (sprite_index < 0) {
                    lua_pop(L, 1);
                    return luaL_error(L,
                        "%s: sprite '%s' is not defined", where,
                        sprite_name);
                }
                strcpy(body->sprite, sprite_name);
                lua_pop(L, 1);
                sprite = &g_sprites[sprite_index];
                body->half_w = (double)sprite->width * scale * 0.5;
                body->half_h = (double)sprite->height * scale * 0.5;
                body->sprite_scale = scale;
                body->has_sprite = 1;
                body->shape = PHYSICS_BOX;
            } else {
                lua_pop(L, 1);
                lua_getfield(L, index, "triangle");
                if (lua_isnoneornil(L, -1)) {
                    lua_pop(L, 1);
                    return luaL_error(L,
                        "%s: expected circle, box, sprite, or triangle",
                        where);
                }
                shape_index = lua_gettop(L);
                if (lua_type(L, shape_index) != LUA_TTABLE) {
                    lua_pop(L, 1);
                    return luaL_error(L, "%s: triangle must be a table",
                                      where);
                }
                lua_rawgeti(L, shape_index, 1);
                if (lua_isnumber(L, -1)) {
                    for (i = 0; i < 6; ++i) {
                        lua_Number value;

                        lua_rawgeti(L, shape_index, i + 1);
                        if (lua_type(L, -1) != LUA_TNUMBER) {
                            lua_pop(L, 2);
                            return luaL_error(L,
                                "%s: triangle needs 6 numbers", where);
                        }
                        value = lua_tonumber(L, -1);
                        lua_pop(L, 1);
                        if (!isfinite(value) ||
                            fabs(value) > PHYSICS_COORD_LIMIT)
                            return luaL_error(L,
                                "%s: triangle coordinate is invalid", where);
                        if (i % 2 == 0)
                            body->verts[i / 2].x = (double)value;
                        else
                            body->verts[i / 2].y = (double)value;
                    }
                    lua_pop(L, 1);
                } else {
                    const char *fields[6] = {"x1", "y1", "x2", "y2",
                                             "x3", "y3"};
                    double values[6];

                    lua_pop(L, 1);
                    for (i = 0; i < 6; ++i) {
                        if (physics_read_number_field(L, shape_index,
                            fields[i], 0.0, &values[i], where))
                            return luaL_error(L,
                                "%s: triangle coordinate is invalid", where);
                        if (fabs(values[i]) > PHYSICS_COORD_LIMIT)
                            return luaL_error(L,
                                "%s: triangle coordinate is invalid", where);
                    }
                    for (i = 0; i < 3; ++i)
                        body->verts[i] = physics_vec(values[i * 2],
                                                     values[i * 2 + 1]);
                }
                lua_pop(L, 1);
                if (physics_shape_area(body) <= 1e-9)
                    return luaL_error(L, "%s: triangle is degenerate", where);
                body->shape = PHYSICS_TRIANGLE;
            }
        }
    }

    body->angle = physics_deg_to_rad(body->angle);
    body->omega = physics_deg_to_rad(body->omega);
    if (mass < 0.0)
        mass = physics_shape_area(body) * density;
    if (!isfinite(mass) || mass <= 0.0)
        mass = 0.001;
    body->mass = mass;
    if (body->type == PHYSICS_DYNAMIC) {
        body->inv_mass = 1.0 / mass;
        if (body->shape == PHYSICS_CIRCLE)
            body->inertia = 0.5 * mass * body->radius * body->radius;
        else if (body->shape == PHYSICS_BOX)
            body->inertia = mass *
                ((2.0 * body->half_w) * (2.0 * body->half_w) +
                 (2.0 * body->half_h) * (2.0 * body->half_h)) / 12.0;
        else {
            double max_d2 = 0.0;
            for (i = 0; i < 3; ++i) {
                PhysicsVec v = body->verts[i];
                double d2 = physics_dot(v, v);
                if (d2 > max_d2)
                    max_d2 = d2;
            }
            body->inertia = mass * fmax(max_d2, 1.0);
        }
        if (body->inertia > 0.0)
            body->inv_inertia = 1.0 / body->inertia;
    }
    return 0;
}

static int physics_alloc_body(void)
{
    int i;

    for (i = 0; i < PHYSICS_BODY_MAX; ++i) {
        if (!g_physics_bodies[i].used) {
            g_physics_bodies[i].id = i + 1;
            return i + 1;
        }
    }
    return 0;
}

static int physics_alloc_joint(void)
{
    int i;

    for (i = 0; i < PHYSICS_JOINT_MAX; ++i) {
        if (!g_physics_joints[i].used) {
            g_physics_joints[i].id = i + 1;
            return i + 1;
        }
    }
    return 0;
}

static PhysicsBody *physics_check_body(lua_State *L, int index)
{
    int id = (int)luaL_checkinteger(L, index);
    PhysicsBody *body = physics_body_by_id(id, "draw.physics");

    if (!body)
        luaL_error(L, "draw.physics: body %d is not defined", id);
    return body;
}

static int physics_call_callback(lua_State *L, PhysicsContact *contact)
{
    if (g_physics_callback == LUA_NOREF)
        return 0;
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_physics_callback);
    lua_pushinteger(L, contact->a);
    lua_pushinteger(L, contact->b);
    lua_pushnumber(L, contact->px);
    lua_pushnumber(L, contact->py);
    lua_pushnumber(L, contact->nx);
    lua_pushnumber(L, contact->ny);
    lua_pushboolean(L, contact->sensor);
    if (lua_pcall(L, 7, 0, 0) != 0)
        return luaL_error(L, "draw.physicsstep: callback error: %s",
                          lua_tostring(L, -1));
    return 0;
}

static int l_physicsworld(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        double x = luaL_checknumber(L, 1);
        double y = luaL_checknumber(L, 2);

        if (!isfinite(x) || !isfinite(y) || fabs(x) > PHYSICS_SPEED_LIMIT ||
            fabs(y) > PHYSICS_SPEED_LIMIT)
            return luaL_error(L, "draw.physicsworld: invalid gravity");
        g_physics_gravity.x = x;
        g_physics_gravity.y = y;
    }
    lua_pushnumber(L, g_physics_gravity.x);
    lua_pushnumber(L, g_physics_gravity.y);
    return 2;
}

static int l_physicsclear(lua_State *L)
{
    physics_clear_state();
    lua_pushboolean(L, 1);
    return 1;
}

static int l_physicsbody(lua_State *L)
{
    PhysicsBody tmp;
    PhysicsBody *body;
    int id;

    if (physics_parse_body(L, 1, &tmp) != 0)
        return luaL_error(L, "draw.physicsbody: invalid body");
    id = physics_alloc_body();
    if (id == 0)
        return luaL_error(L, "draw.physicsbody: too many bodies (max %d)",
                          PHYSICS_BODY_MAX);
    body = &g_physics_bodies[id - 1];
    *body = tmp;
    body->id = id;
    body->used = 1;
    lua_pushinteger(L, id);
    return 1;
}

static int l_physicsstep(lua_State *L)
{
    double dt = luaL_optnumber(L, 1, -1.0);
    int substep;
    int i;

    if (dt < 0.0)
        dt = g_draw.frame_delta;
    if (!isfinite(dt))
        return luaL_error(L, "draw.physicsstep: dt must be finite");
    if (dt <= 0.0) {
        lua_pushinteger(L, 0);
        return 1;
    }
    if (dt > 1.0 / 30.0)
        dt = 1.0 / 30.0;
    g_physics_contact_count = 0;
    for (substep = 0; substep < 2; ++substep) {
        double h = dt * 0.5;
        int base = g_physics_contact_count;
        int iter;

        physics_integrate(h);
        physics_detect_contacts();
        for (i = base; i < g_physics_contact_count; ++i)
            physics_prepare_contact(&g_physics_contacts[i]);
        for (iter = 0; iter < PHYSICS_ITERATIONS; ++iter) {
            for (i = 0; i < PHYSICS_JOINT_MAX; ++i) {
                if (g_physics_joints[i].used)
                    physics_solve_joint(&g_physics_joints[i]);
            }
            for (i = base; i < g_physics_contact_count; ++i)
                physics_solve_contact(&g_physics_contacts[i]);
        }
        physics_correct_contacts(base);
        for (i = 0; i < PHYSICS_BODY_MAX; ++i) {
            if (g_physics_bodies[i].used)
                physics_clamp_body(&g_physics_bodies[i]);
        }
    }
    for (i = 0; i < g_physics_contact_count; ++i) {
        if (physics_call_callback(L, &g_physics_contacts[i]) != 0)
            return 0;
    }
    lua_pushinteger(L, g_physics_contact_count);
    return 1;
}

static int l_physicsposition(lua_State *L)
{
    PhysicsBody *body = physics_check_body(L, 1);

    lua_pushnumber(L, body->x);
    lua_pushnumber(L, body->y);
    lua_pushnumber(L, body->angle * 57.295779513082320876798154814105);
    return 3;
}

static int l_physicsvelocity(lua_State *L)
{
    PhysicsBody *body = physics_check_body(L, 1);

    lua_pushnumber(L, body->vx);
    lua_pushnumber(L, body->vy);
    lua_pushnumber(L, body->omega * 57.295779513082320876798154814105);
    return 3;
}

static int l_physicssetposition(lua_State *L)
{
    PhysicsBody *body = physics_check_body(L, 1);
    double x = luaL_checknumber(L, 2);
    double y = luaL_checknumber(L, 3);
    double angle = luaL_optnumber(L, 4,
        body->angle * 57.295779513082320876798154814105);

    if (!isfinite(x) || !isfinite(y) || !isfinite(angle) ||
        fabs(x) > PHYSICS_COORD_LIMIT || fabs(y) > PHYSICS_COORD_LIMIT)
        return luaL_error(L, "draw.physicssetposition: invalid value");
    body->x = x;
    body->y = y;
    body->angle = physics_deg_to_rad(angle);
    return 0;
}

static int l_physicssetvelocity(lua_State *L)
{
    PhysicsBody *body = physics_check_body(L, 1);
    double vx = luaL_checknumber(L, 2);
    double vy = luaL_checknumber(L, 3);
    double omega = luaL_optnumber(L, 4,
        body->omega * 57.295779513082320876798154814105);

    if (!isfinite(vx) || !isfinite(vy) || !isfinite(omega) ||
        fabs(vx) > PHYSICS_SPEED_LIMIT || fabs(vy) > PHYSICS_SPEED_LIMIT ||
        fabs(omega) > 100000.0)
        return luaL_error(L, "draw.physicssetvelocity: invalid value");
    body->vx = vx;
    body->vy = vy;
    body->omega = physics_deg_to_rad(omega);
    return 0;
}

static int l_physicsapplyforce(lua_State *L)
{
    PhysicsBody *body = physics_check_body(L, 1);
    double fx = luaL_checknumber(L, 2);
    double fy = luaL_checknumber(L, 3);

    if (!isfinite(fx) || !isfinite(fy))
        return luaL_error(L, "draw.physicsapplyforce: invalid force");
    body->fx += fx;
    body->fy += fy;
    return 0;
}

static int l_physicsapplyimpulse(lua_State *L)
{
    PhysicsBody *body = physics_check_body(L, 1);
    double ix = luaL_checknumber(L, 2);
    double iy = luaL_checknumber(L, 3);

    if (!isfinite(ix) || !isfinite(iy))
        return luaL_error(L, "draw.physicsapplyimpulse: invalid impulse");
    body->vx += ix * body->inv_mass;
    body->vy += iy * body->inv_mass;
    return 0;
}

static int l_physicsfilter(lua_State *L)
{
    PhysicsBody *body = physics_check_body(L, 1);

    body->category = physics_parse_u32_value(L, 2, body->category,
                                             "draw.physicsfilter",
                                             "category");
    body->mask = physics_parse_u32_value(L, 3, body->mask,
                                         "draw.physicsfilter", "mask");
    lua_pushinteger(L, (lua_Integer)body->category);
    lua_pushinteger(L, (lua_Integer)body->mask);
    return 2;
}

static int l_physicsremove(lua_State *L)
{
    int id = (int)luaL_checkinteger(L, 1);
    PhysicsBody *body = physics_body_by_id(id, "draw.physicsremove");
    int i;

    if (!body)
        return luaL_error(L, "draw.physicsremove: body %d is not defined",
                          id);
    memset(body, 0, sizeof(*body));
    for (i = 0; i < PHYSICS_JOINT_MAX; ++i) {
        if (g_physics_joints[i].used &&
            (g_physics_joints[i].a == id || g_physics_joints[i].b == id))
            memset(&g_physics_joints[i], 0, sizeof(g_physics_joints[i]));
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_physicscontacts(lua_State *L)
{
    int i;

    lua_createtable(L, g_physics_contact_count, 0);
    for (i = 0; i < g_physics_contact_count; ++i) {
        PhysicsContact *contact = &g_physics_contacts[i];

        lua_createtable(L, 0, 7);
        lua_pushinteger(L, contact->a);
        lua_setfield(L, -2, "a");
        lua_pushinteger(L, contact->b);
        lua_setfield(L, -2, "b");
        lua_pushnumber(L, contact->px);
        lua_setfield(L, -2, "x");
        lua_pushnumber(L, contact->py);
        lua_setfield(L, -2, "y");
        lua_pushnumber(L, contact->nx);
        lua_setfield(L, -2, "nx");
        lua_pushnumber(L, contact->ny);
        lua_setfield(L, -2, "ny");
        lua_pushboolean(L, contact->sensor);
        lua_setfield(L, -2, "sensor");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int l_physicscallback(lua_State *L)
{
    if (lua_isnoneornil(L, 1)) {
        if (g_physics_callback != LUA_NOREF) {
            luaL_unref(L, LUA_REGISTRYINDEX, g_physics_callback);
            g_physics_callback = LUA_NOREF;
        }
    } else {
        luaL_checktype(L, 1, LUA_TFUNCTION);
        if (g_physics_callback != LUA_NOREF)
            luaL_unref(L, LUA_REGISTRYINDEX, g_physics_callback);
        lua_pushvalue(L, 1);
        g_physics_callback = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    lua_pushboolean(L, g_physics_callback != LUA_NOREF);
    return 1;
}

static int l_physicsjoint(lua_State *L)
{
    const char *kind = luaL_checkstring(L, 1);
    PhysicsBody *a = physics_check_body(L, 2);
    PhysicsBody *b = physics_check_body(L, 3);
    PhysicsJoint tmp;
    PhysicsJoint *joint;
    double dist;
    int id;

    memset(&tmp, 0, sizeof(tmp));
    tmp.a = a->id;
    tmp.b = b->id;
    dist = sqrt((b->x - a->x) * (b->x - a->x) +
                (b->y - a->y) * (b->y - a->y));
    if (strcmp(kind, "distance") == 0)
        tmp.kind = 0;
    else if (strcmp(kind, "spring") == 0)
        tmp.kind = 1;
    else if (strcmp(kind, "pin") == 0)
        tmp.kind = 2;
    else
        return luaL_error(L, "draw.physicsjoint: unknown joint type '%s'",
                          kind);
    tmp.rest = dist;
    tmp.stiffness = tmp.kind == 1 ? 100.0 : 1.0;
    tmp.damping = tmp.kind == 1 ? 2.0 : 0.0;
    if (!lua_isnoneornil(L, 4)) {
        int table;

        if (lua_type(L, 4) != LUA_TTABLE)
            return luaL_error(L, "draw.physicsjoint: options must be a table");
        table = 4;
        if (physics_read_number_field(L, table, "length", tmp.rest,
                                      &tmp.rest, "draw.physicsjoint") ||
            physics_read_number_field(L, table, "rest", tmp.rest,
                                      &tmp.rest, "draw.physicsjoint") ||
            physics_read_number_field(L, table, "stiffness", tmp.stiffness,
                                      &tmp.stiffness, "draw.physicsjoint") ||
            physics_read_number_field(L, table, "damping", tmp.damping,
                                      &tmp.damping, "draw.physicsjoint") ||
            physics_read_number_field(L, table, "ax", 0.0,
                                      &tmp.local_a.x, "draw.physicsjoint") ||
            physics_read_number_field(L, table, "ay", 0.0,
                                      &tmp.local_a.y, "draw.physicsjoint") ||
            physics_read_number_field(L, table, "bx", 0.0,
                                      &tmp.local_b.x, "draw.physicsjoint") ||
            physics_read_number_field(L, table, "by", 0.0,
                                      &tmp.local_b.y, "draw.physicsjoint"))
            return luaL_error(L, "draw.physicsjoint: invalid value");
    }
    if (!isfinite(tmp.rest) || tmp.rest < 0.0 ||
        !isfinite(tmp.stiffness) || tmp.stiffness < 0.0 ||
        !isfinite(tmp.damping) || tmp.damping < 0.0 ||
        !isfinite(tmp.local_a.x) || !isfinite(tmp.local_a.y) ||
        !isfinite(tmp.local_b.x) || !isfinite(tmp.local_b.y))
        return luaL_error(L, "draw.physicsjoint: invalid joint parameters");
    if (tmp.kind != 1 && tmp.stiffness > 1.0)
        return luaL_error(L, "draw.physicsjoint: stiffness must be 0..1");
    if (tmp.kind == 1 && tmp.stiffness > 10000.0)
        return luaL_error(L, "draw.physicsjoint: stiffness is too large");
    id = physics_alloc_joint();
    if (id == 0)
        return luaL_error(L, "draw.physicsjoint: too many joints (max %d)",
                          PHYSICS_JOINT_MAX);
    joint = &g_physics_joints[id - 1];
    *joint = tmp;
    joint->id = id;
    joint->used = 1;
    lua_pushinteger(L, id);
    return 1;
}

static int l_physicsjointremove(lua_State *L)
{
    int id = (int)luaL_checkinteger(L, 1);
    PhysicsJoint *joint = physics_joint_by_id(id, "draw.physicsjointremove");

    if (!joint)
        return luaL_error(L, "draw.physicsjointremove: joint %d is not defined",
                          id);
    memset(joint, 0, sizeof(*joint));
    lua_pushboolean(L, 1);
    return 1;
}

static int physics_ray_segment(PhysicsVec origin, PhysicsVec dir,
                               PhysicsVec a, PhysicsVec b,
                               double *t, PhysicsVec *normal)
{
    PhysicsVec edge = physics_vec(b.x - a.x, b.y - a.y);
    PhysicsVec q = physics_vec(a.x - origin.x, a.y - origin.y);
    double denom = physics_cross(dir, edge);
    double u, value;

    if (fabs(denom) < 1e-12)
        return 0;
    value = physics_cross(q, edge) / denom;
    u = physics_cross(q, dir) / denom;
    if (value < 0.0 || u < 0.0 || u > 1.0)
        return 0;
    *normal = physics_vec(-edge.y, edge.x);
    if (!physics_normalize(normal))
        return 0;
    if (physics_dot(*normal, dir) > 0.0) {
        normal->x = -normal->x;
        normal->y = -normal->y;
    }
    *t = value;
    return 1;
}

static int physics_raycast_body(PhysicsBody *body, PhysicsVec origin,
                                PhysicsVec dir, double length,
                                uint32_t category, uint32_t mask,
                                double *best_t, PhysicsVec *best_normal)
{
    double t = INFINITY;
    PhysicsVec normal = physics_vec(0.0, 0.0);
    int found = 0;

    if ((body->category & mask) == 0 || (category & body->mask) == 0)
        return 0;
    if (body->shape == PHYSICS_CIRCLE) {
        PhysicsVec to_center = physics_vec(origin.x - body->x,
                                           origin.y - body->y);
        double b = 2.0 * physics_dot(to_center, dir);
        double c = physics_dot(to_center, to_center) -
                   body->radius * body->radius;
        double disc = b * b - 4.0 * c;
        double root;

        if (disc < 0.0)
            return 0;
        root = sqrt(disc);
        t = (-b - root) * 0.5;
        if (t < 0.0)
            t = (-b + root) * 0.5;
        if (t < 0.0 || t > length)
            return 0;
        normal = physics_vec(origin.x + dir.x * t - body->x,
                             origin.y + dir.y * t - body->y);
        if (!physics_normalize(&normal))
            normal = physics_vec(-dir.x, -dir.y);
        found = 1;
    } else {
        PhysicsVec verts[4];
        int count;
        int i;

        physics_body_vertices(body, verts, &count);
        for (i = 0; i < count; ++i) {
            double value;
            PhysicsVec candidate;

            if (!physics_ray_segment(origin, dir, verts[i],
                                     verts[(i + 1) % count], &value,
                                     &candidate))
                continue;
            if (value >= 0.0 && value <= length && value < t) {
                t = value;
                normal = candidate;
                found = 1;
            }
        }
    }
    if (!found || t >= *best_t)
        return 0;
    *best_t = t;
    *best_normal = normal;
    return 1;
}

static int l_physicsraycast(lua_State *L)
{
    double x1 = luaL_checknumber(L, 1);
    double y1 = luaL_checknumber(L, 2);
    double x2 = luaL_checknumber(L, 3);
    double y2 = luaL_checknumber(L, 4);
    uint32_t category = physics_parse_u32_value(L, 5, 0xffffffffU,
                                                "draw.physicsraycast",
                                                "category");
    uint32_t mask = physics_parse_u32_value(L, 6, 0xffffffffU,
                                            "draw.physicsraycast", "mask");
    PhysicsVec origin = physics_vec(x1, y1);
    PhysicsVec delta = physics_vec(x2 - x1, y2 - y1);
    double length = physics_length(delta);
    double best_t = INFINITY;
    PhysicsVec best_normal = physics_vec(0.0, 0.0);
    PhysicsBody *hit = NULL;
    int i;

    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) ||
        !physics_normalize(&delta))
        return luaL_error(L, "draw.physicsraycast: invalid ray");
    for (i = 0; i < PHYSICS_BODY_MAX; ++i) {
        if (g_physics_bodies[i].used &&
            physics_raycast_body(&g_physics_bodies[i], origin, delta, length,
                                 category, mask, &best_t, &best_normal))
            hit = &g_physics_bodies[i];
    }
    if (!hit) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushinteger(L, hit->id);
    lua_pushnumber(L, origin.x + delta.x * best_t);
    lua_pushnumber(L, origin.y + delta.y * best_t);
    lua_pushnumber(L, best_normal.x);
    lua_pushnumber(L, best_normal.y);
    lua_pushnumber(L, best_t / length);
    return 6;
}

static int l_physicsdraw(lua_State *L)
{
    int r = 255, g = 255, b = 255, a = 255;
    int i;

    if (!ensure_open(L, "draw.physicsdraw"))
        return 0;
    if (read_color(L, 1, &r, &g, &b, &a) < 0) {
        r = 255;
        g = 255;
        b = 255;
        a = 255;
    }
    lua_settop(L, 0);
    for (i = 0; i < PHYSICS_BODY_MAX; ++i) {
        PhysicsBody *body = &g_physics_bodies[i];
        int br = r, bg = g, bb = b, ba = a;

        if (!body->used)
            continue;
        if (body->has_sprite) {
            int sprite_index = find_sprite_name(body->sprite);
            SpriteEntry *sprite;

            if (sprite_index < 0)
                continue;
            sprite = &g_sprites[sprite_index];
            lua_pushinteger(L, (lua_Integer)physics_round(body->x));
            lua_pushinteger(L, (lua_Integer)physics_round(body->y));
            lua_pushstring(L, body->sprite);
            lua_pushinteger(L, (lua_Integer)body->sprite_scale);
            lua_pushinteger(L, physics_rad_to_deg(body->angle));
            lua_pushnil(L);
            lua_pushnil(L);
            lua_pushnil(L);
            lua_createtable(L, 0, 2);
            lua_pushnumber(L, (double)sprite->width * 0.5);
            lua_setfield(L, -2, "pivotx");
            lua_pushnumber(L, (double)sprite->height * 0.5);
            lua_setfield(L, -2, "pivoty");
            l_draw_sprite(L);
            lua_settop(L, 0);
            continue;
        }
        if (body->has_color) {
            lua_pushstring(L, body->color);
            if (read_color(L, -1, &br, &bg, &bb, &ba) >= 0) {
                r = br;
                g = bg;
                b = bb;
                a = ba;
            }
            lua_pop(L, 1);
        }
        if (body->shape == PHYSICS_CIRCLE) {
            fill_circle((long long)physics_round(body->x),
                        (long long)physics_round(body->y),
                        (long long)physics_round(body->radius),
                        1, make_color(r, g, b, a));
        } else if (body->shape == PHYSICS_BOX) {
            fill_rotated_rect(body->x - body->half_w,
                              body->y - body->half_h,
                              body->half_w * 2.0, body->half_h * 2.0,
                              physics_rad_to_deg(body->angle), 1,
                              make_color(r, g, b, a));
        } else {
            PhysicsVec verts[4];
            int count;

            physics_body_vertices(body, verts, &count);
            fill_triangle((long long)physics_round(verts[0].x),
                          (long long)physics_round(verts[0].y),
                          (long long)physics_round(verts[1].x),
                          (long long)physics_round(verts[1].y),
                          (long long)physics_round(verts[2].x),
                          (long long)physics_round(verts[2].y),
                          1, make_color(r, g, b, a));
        }
    }
    return 0;
}

static int l_physicsbodycount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < PHYSICS_BODY_MAX; ++i) {
        if (g_physics_bodies[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_physicsjointcount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < PHYSICS_JOINT_MAX; ++i) {
        if (g_physics_joints[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static CanvasEntry *canvas_by_id(lua_Integer id)
{
    if (id < 1 || id > CANVAS_MAX)
        return NULL;
    if (!g_canvases[id - 1].used)
        return NULL;
    return &g_canvases[id - 1];
}

static size_t canvas_total_pixels(void)
{
    size_t total = 0;
    int i;

    for (i = 0; i < CANVAS_MAX; ++i) {
        if (g_canvases[i].used)
            total += (size_t)g_canvases[i].width * (size_t)g_canvases[i].height;
    }
    return total;
}

static void canvas_fill(CanvasEntry *canvas, uint32_t color)
{
    size_t count = (size_t)canvas->width * (size_t)canvas->height;
    size_t i;

    for (i = 0; i < count; ++i)
        canvas->pixels[i] = color;
}

static int l_canvas(lua_State *L)
{
    lua_Integer wi = luaL_checkinteger(L, 1);
    lua_Integer hi = luaL_checkinteger(L, 2);
    int w, h;
    uint32_t color = 0;
    CanvasEntry *canvas;
    int i;
    size_t total;
    int r, g, b, a;

    if (wi < 1 || hi < 1 || wi > CANVAS_DIM_MAX || hi > CANVAS_DIM_MAX)
        return luaL_error(L, "draw.canvas: size must be 1..%d", CANVAS_DIM_MAX);
    w = (int)wi;
    h = (int)hi;
    if ((long long)w * (long long)h > CANVAS_AREA_MAX)
        return luaL_error(L, "draw.canvas: area must not exceed %d",
                          CANVAS_AREA_MAX);

    if (lua_gettop(L) >= 3 && !lua_isnoneornil(L, 3)) {
        if (read_color(L, 3, &r, &g, &b, &a) != 0)
            return 0;
        color = make_color(r, g, b, a);
    }

    canvas = NULL;
    for (i = 0; i < CANVAS_MAX; ++i) {
        if (!g_canvases[i].used) {
            canvas = &g_canvases[i];
            break;
        }
    }
    if (!canvas)
        return luaL_error(L, "draw.canvas: limit of %d canvases reached",
                          CANVAS_MAX);

    total = canvas_total_pixels() + (size_t)w * (size_t)h;
    if (total > CANVAS_TOTAL_MAX)
        return luaL_error(L, "draw.canvas: total canvas area exceeds %d",
                          CANVAS_TOTAL_MAX);

    canvas->pixels = (uint32_t *)calloc((size_t)w * (size_t)h,
                                        sizeof(uint32_t));
    if (!canvas->pixels)
        return luaL_error(L, "draw.canvas: out of memory");
    canvas->used = 1;
    canvas->width = w;
    canvas->height = h;
    if (color != 0)
        canvas_fill(canvas, color);

    lua_pushinteger(L, (lua_Integer)(canvas - g_canvases + 1));
    return 1;
}

static int l_setcanvas(lua_State *L)
{
    int previous = g_canvas_target;
    CanvasEntry *canvas;

    if (lua_isnoneornil(L, 1) || lua_tointeger(L, 1) == 0) {
        canvas_restore_main();
        lua_pushinteger(L, previous);
        return 1;
    }

    canvas = canvas_by_id(luaL_checkinteger(L, 1));
    if (!canvas)
        return luaL_error(L, "draw.setcanvas: canvas is not defined");

    canvas_restore_main();
    g_draw.pixels = canvas->pixels;
    g_draw.width = canvas->width;
    g_draw.height = canvas->height;
    g_canvas_target = (int)(canvas - g_canvases + 1);
    lua_pushinteger(L, previous);
    return 1;
}

static int l_canvasclear(lua_State *L)
{
    int r = 0, g = 0, b = 0, a = 0;
    uint32_t color = 0;
    size_t count;
    size_t i;

    if (g_canvas_target == 0 && !ensure_open(L, "draw.canvasclear"))
        return 0;
    if (!g_draw.pixels)
        return 0;

    if (lua_gettop(L) >= 1 && !lua_isnoneornil(L, 1)) {
        if (read_color(L, 1, &r, &g, &b, &a) != 0)
            return 0;
        color = make_color(r, g, b, a);
    }

    count = (size_t)g_draw.width * (size_t)g_draw.height;
    for (i = 0; i < count; ++i)
        g_draw.pixels[i] = color;
    mark_dirty_rect(0, 0, g_draw.width, g_draw.height);
    return 0;
}

static int l_canvassize(lua_State *L)
{
    CanvasEntry *canvas = canvas_by_id(luaL_checkinteger(L, 1));

    if (!canvas)
        return luaL_error(L, "draw.canvassize: canvas is not defined");
    lua_pushinteger(L, canvas->width);
    lua_pushinteger(L, canvas->height);
    return 2;
}

static int l_canvasremove(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    CanvasEntry *canvas = canvas_by_id(id);

    if (!canvas) {
        lua_pushboolean(L, 0);
        return 1;
    }
    if (g_canvas_target == id)
        canvas_restore_main();
    free(canvas->pixels);
    memset(canvas, 0, sizeof(*canvas));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_canvascount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < CANVAS_MAX; ++i) {
        if (g_canvases[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_canvasdraw(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    CanvasEntry *canvas = canvas_by_id(id);
    lua_Integer xi, yi;
    int x, y;
    int scale;
    int angle;
    int flipx, flipy;
    int tr, tg, tb, ta;
    int extra_alpha;
    double W, H, bw, bh;
    double xd, yd;
    double pivot_x, pivot_y;
    double px, py;
    double min_x = 0.0, max_x = 0.0, min_y = 0.0, max_y = 0.0;
    double c, s, rad;
    double dx, dy, rot_x, rot_y, src_cx, src_cy, p, q;
    double alpha_factor;
    int k;
    int sx0, sx1, sy0, sy1;
    int sx, sy;
    int col, row;

    if (!canvas)
        return luaL_error(L, "draw.canvasdraw: canvas is not defined");
    if (!ensure_open(L, "draw.canvasdraw"))
        return 0;
    if (g_canvas_target == id)
        return luaL_error(L, "draw.canvasdraw: cannot draw canvas into itself");

    xi = luaL_checkinteger(L, 2);
    yi = luaL_checkinteger(L, 3);
    if (xi < -1000000 || xi > 1000000 || yi < -1000000 || yi > 1000000)
        return luaL_error(L, "draw.canvasdraw: position out of range");
    x = (int)xi;
    y = (int)yi;
    scale = parse_scale(L, 4);
    angle = parse_angle(L, 5);
    flipx = read_flip_field(L, 6, 1, "flip");
    flipy = read_flip_field(L, 6, 2, "flip");
    if (parse_tint(L, 7, &tr, &tg, &tb, &ta) != 0)
        return 0;
    extra_alpha = clamp_alpha_number(luaL_optnumber(L, 9, 1.0));
    if (lua_isnoneornil(L, 8)) {
        pivot_x = (double)canvas->width / 2.0;
        pivot_y = (double)canvas->height / 2.0;
    } else {
        if (lua_type(L, 8) != LUA_TTABLE)
            return luaL_error(L, "draw.canvasdraw: pivot must be a table");
        pivot_x = read_pivot_field(L, 8, 1,
                                   (double)canvas->width / 2.0);
        pivot_y = read_pivot_field(L, 8, 2,
                                   (double)canvas->height / 2.0);
    }
    if (!isfinite(pivot_x) || !isfinite(pivot_y) || pivot_x < -1000000.0 ||
        pivot_x > 1000000.0 || pivot_y < -1000000.0 ||
        pivot_y > 1000000.0)
        return luaL_error(L,
            "draw.canvasdraw: pivot must be finite and within +/-1000000");
    if (flipx)
        pivot_x = (double)canvas->width - pivot_x;
    if (flipy)
        pivot_y = (double)canvas->height - pivot_y;
    px = pivot_x * (double)scale;
    py = pivot_y * (double)scale;

    W = (double)canvas->width * (double)scale;
    H = (double)canvas->height * (double)scale;
    rad = (double)angle * 0.017453292519943295769236907684886;
    c = cos(rad);
    s = sin(rad);
    if (fabs(c) < 1e-12)
        c = 0.0;
    if (fabs(s) < 1e-12)
        s = 0.0;

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

    if (!lua_isnoneornil(L, 8)) {
        if (!visible_range(xd + min_x, bw, g_draw.width, &sx0, &sx1) ||
            !visible_range(yd + min_y, bh, g_draw.height, &sy0, &sy1))
            return 0;
    } else {
        if (!visible_range(xd, bw, g_draw.width, &sx0, &sx1) ||
            !visible_range(yd, bh, g_draw.height, &sy0, &sy1))
            return 0;
    }

    alpha_factor = (double)extra_alpha * (double)g_draw.alpha / 65025.0;

    for (sy = sy0; sy <= sy1; ++sy) {
        dy = (double)sy + 0.5 - yd;
        if (lua_isnoneornil(L, 8))
            dy += min_y;

        for (sx = sx0; sx <= sx1; ++sx) {
            dx = (double)sx + 0.5 - xd;
            if (lua_isnoneornil(L, 8))
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
                col = canvas->width - 1 - col;
            if (flipy)
                row = canvas->height - 1 - row;
            if (col < 0 || col >= canvas->width ||
                row < 0 || row >= canvas->height)
                continue;

            {
                uint32_t src = canvas->pixels[(size_t)row * canvas->width +
                                              (size_t)col];
                int sa = (int)((src >> 24) & 0xFF);
                int sr = (int)((src >> 16) & 0xFF);
                int sg = (int)((src >> 8) & 0xFF);
                int sb = (int)(src & 0xFF);
                uint32_t color;

                sa = (int)(sa * ta / 255.0 * alpha_factor + 0.5);
                sr = sr * tr / 255;
                sg = sg * tg / 255;
                sb = sb * tb / 255;
                color = ((uint32_t)sa << 24) |
                        ((uint32_t)sr << 16) |
                        ((uint32_t)sg << 8) |
                        (uint32_t)sb;
                g_draw.pixels[(size_t)sy * g_draw.width + sx] =
                    blend_color(
                        g_draw.pixels[(size_t)sy * g_draw.width + sx],
                        color);
            }
        }
    }

    mark_dirty_rect(sx0, sy0, sx1 - sx0 + 1, sy1 - sy0 + 1);
    return 0;
}

static int find_tilemap_name(const char *name)
{
    int i;

    for (i = 0; i < TILEMAP_MAX; ++i) {
        if (g_tilemaps[i].used && strcmp(g_tilemaps[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int tile_from_char(char ch, int *out)
{
    int value;

    if (ch == '.' || ch == ' ') {
        *out = -1;
        return 0;
    }
    if (ch >= '0' && ch <= '9')
        value = ch - '0';
    else if (ch >= 'a' && ch <= 'z')
        value = 10 + (ch - 'a');
    else if (ch >= 'A' && ch <= 'Z')
        value = 36 + (ch - 'A');
    else
        return -1;
    if (value >= TILEMAP_TILE_MAX)
        return -1;
    *out = value;
    return 0;
}

static void tilemap_remove_bodies(TilemapEntry *map)
{
    int i;

    for (i = 0; i < map->body_count; ++i) {
        int id = map->body_ids[i];

        if (id > 0 && id <= PHYSICS_BODY_MAX)
            memset(&g_physics_bodies[id - 1], 0, sizeof(PhysicsBody));
    }
    map->body_count = 0;
    memset(map->body_ids, 0, sizeof(map->body_ids));
}

static int l_tilemap(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    size_t name_len = strlen(name);
    lua_Integer size_value = luaL_checkinteger(L, 4);
    TilemapEntry *map;
    signed char cells[TILEMAP_ROW_MAX * TILEMAP_COL_MAX];
    int used_tiles[TILEMAP_TILE_MAX];
    int solid_set[TILEMAP_TILE_MAX];
    int solid_all = 0;
    int rows;
    int cols = 0;
    int size;
    int i, j;
    int free_index = -1;
    int value;

    if (name_len == 0 || name_len >= sizeof(map->name))
        return luaL_error(L, "draw.tilemap: invalid name");
    if (lua_type(L, 2) != LUA_TTABLE)
        return luaL_error(L, "draw.tilemap: rows must be a table");
    if (lua_type(L, 3) != LUA_TTABLE)
        return luaL_error(L, "draw.tilemap: tiles must be a table");
    if (size_value < 1 || size_value > CANVAS_DIM_MAX)
        return luaL_error(L, "draw.tilemap: tile size must be 1..%d",
                          CANVAS_DIM_MAX);
    size = (int)size_value;

    if (find_tilemap_name(name) >= 0)
        return luaL_error(L, "draw.tilemap: '%s' is already defined", name);
    for (i = 0; i < TILEMAP_MAX; ++i) {
        if (!g_tilemaps[i].used) {
            free_index = i;
            break;
        }
    }
    if (free_index < 0)
        return luaL_error(L, "draw.tilemap: limit of %d maps reached",
                          TILEMAP_MAX);

    rows = (int)lua_rawlen(L, 2);
    if (rows < 1 || rows > TILEMAP_ROW_MAX)
        return luaL_error(L, "draw.tilemap: rows must be 1..%d",
                          TILEMAP_ROW_MAX);
    memset(cells, -1, sizeof(cells));
    memset(used_tiles, 0, sizeof(used_tiles));
    memset(solid_set, 0, sizeof(solid_set));

    for (i = 0; i < rows; ++i) {
        lua_rawgeti(L, 2, i + 1);
        if (lua_type(L, -1) == LUA_TSTRING) {
            const char *text = lua_tostring(L, -1);
            size_t len = strlen(text);

            if (len > TILEMAP_COL_MAX) {
                lua_pop(L, 1);
                return luaL_error(L, "draw.tilemap: row %d is too long", i + 1);
            }
            for (j = 0; j < (int)len; ++j) {
                if (tile_from_char(text[j], &value) != 0) {
                    lua_pop(L, 1);
                    return luaL_error(L,
                        "draw.tilemap: invalid tile character in row %d",
                        i + 1);
                }
                if (value >= 0) {
                    cells[i * TILEMAP_COL_MAX + j] = (signed char)value;
                    used_tiles[value] = 1;
                }
            }
            if ((int)len > cols)
                cols = (int)len;
        } else if (lua_type(L, -1) == LUA_TTABLE) {
            size_t len = lua_rawlen(L, -1);

            if (len > TILEMAP_COL_MAX) {
                lua_pop(L, 1);
                return luaL_error(L, "draw.tilemap: row %d is too long", i + 1);
            }
            for (j = 0; j < (int)len; ++j) {
                lua_rawgeti(L, -1, j + 1);
                if (lua_isnil(L, -1)) {
                    value = -1;
                } else if (lua_isnumber(L, -1)) {
                    lua_Integer v = lua_tointeger(L, -1);

                    if (v < -1 || v >= TILEMAP_TILE_MAX) {
                        lua_pop(L, 2);
                        return luaL_error(L,
                            "draw.tilemap: tile index out of range in row %d",
                            i + 1);
                    }
                    value = (int)v;
                } else {
                    lua_pop(L, 2);
                    return luaL_error(L,
                        "draw.tilemap: tile must be a number in row %d",
                        i + 1);
                }
                lua_pop(L, 1);
                if (value >= 0) {
                    cells[i * TILEMAP_COL_MAX + j] = (signed char)value;
                    used_tiles[value] = 1;
                }
            }
            if ((int)len > cols)
                cols = (int)len;
        } else {
            lua_pop(L, 1);
            return luaL_error(L, "draw.tilemap: row %d must be a string or table",
                              i + 1);
        }
        lua_pop(L, 1);
    }

    if (cols < 1)
        return luaL_error(L, "draw.tilemap: map is empty");

    if (!lua_isnoneornil(L, 5)) {
        if (lua_type(L, 5) == LUA_TBOOLEAN) {
            solid_all = lua_toboolean(L, 5) ? 1 : 0;
        } else if (lua_type(L, 5) == LUA_TTABLE) {
            size_t count = lua_rawlen(L, 5);

            for (i = 1; i <= (int)count; ++i) {
                lua_rawgeti(L, 5, i);
                if (!lua_isnumber(L, -1)) {
                    lua_pop(L, 1);
                    return luaL_error(L,
                        "draw.tilemap: solid entries must be tile indices");
                }
                value = (int)lua_tointeger(L, -1);
                lua_pop(L, 1);
                if (value < 0 || value >= TILEMAP_TILE_MAX)
                    return luaL_error(L,
                        "draw.tilemap: solid tile index out of range");
                solid_set[value] = 1;
            }
        } else {
            return luaL_error(L,
                "draw.tilemap: solid must be a boolean or table");
        }
    }

    map = &g_tilemaps[free_index];
    memset(map, 0, sizeof(*map));
    map->used = 1;
    strcpy(map->name, name);
    map->rows = rows;
    map->cols = cols;
    map->size = size;
    memcpy(map->cells, cells, sizeof(cells));

    for (i = 0; i < TILEMAP_TILE_MAX; ++i) {
        const char *sprite_name;

        if (!used_tiles[i])
            continue;
        lua_rawgeti(L, 3, i + 1);
        sprite_name = lua_tostring(L, -1);
        if (!sprite_name || find_sprite_name(sprite_name) < 0) {
            lua_pop(L, 1);
            tilemap_remove_bodies(map);
            memset(map, 0, sizeof(*map));
            return luaL_error(L,
                "draw.tilemap: sprite for tile %d is not defined", i);
        }
        strcpy(map->tiles[i], sprite_name);
        lua_pop(L, 1);
    }

    for (i = 0; i < rows; ++i) {
        for (j = 0; j < cols; ++j) {
            PhysicsBody tmp;
            PhysicsBody *body;
            int id;
            int tile = map->cells[i * TILEMAP_COL_MAX + j];

            if (tile < 0 || !(solid_all || solid_set[tile]))
                continue;
            if (map->body_count >= TILEMAP_BODY_MAX) {
                tilemap_remove_bodies(map);
                memset(map, 0, sizeof(*map));
                return luaL_error(L,
                    "draw.tilemap: too many solid tiles (max %d)",
                    TILEMAP_BODY_MAX);
            }
            memset(&tmp, 0, sizeof(tmp));
            tmp.type = PHYSICS_STATIC;
            tmp.shape = PHYSICS_BOX;
            tmp.x = ((double)j + 0.5) * (double)size;
            tmp.y = ((double)i + 0.5) * (double)size;
            tmp.half_w = (double)size * 0.5;
            tmp.half_h = (double)size * 0.5;
            tmp.mass = 1.0;
            tmp.restitution = 0.0;
            tmp.friction = 0.5;
            tmp.category = 0xffffffffU;
            tmp.mask = 0xffffffffU;
            id = physics_alloc_body();
            if (id == 0) {
                tilemap_remove_bodies(map);
                memset(map, 0, sizeof(*map));
                return luaL_error(L,
                    "draw.tilemap: too many physics bodies (max %d)",
                    PHYSICS_BODY_MAX);
            }
            body = &g_physics_bodies[id - 1];
            *body = tmp;
            body->id = id;
            body->used = 1;
            map->body_ids[map->body_count++] = id;
        }
    }

    lua_pushinteger(L, free_index + 1);
    return 1;
}

static int l_drawtilemap(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int index = find_tilemap_name(name);
    TilemapEntry *map;
    double camx, camy;
    int col0, col1, row0, row1;
    int row, col;
    double size;

    if (index < 0)
        return luaL_error(L, "draw.drawtilemap: '%s' is not defined", name);
    if (!ensure_open(L, "draw.drawtilemap"))
        return 0;
    map = &g_tilemaps[index];
    camx = luaL_optnumber(L, 2, g_camera_x);
    camy = luaL_optnumber(L, 3, g_camera_y);
    if (!isfinite(camx) || !isfinite(camy) || fabs(camx) > 1000000000.0 ||
        fabs(camy) > 1000000000.0)
        return luaL_error(L, "draw.drawtilemap: camera is invalid");
    size = (double)map->size;

    col0 = (int)floor(camx / size);
    row0 = (int)floor(camy / size);
    col1 = (int)floor((camx + (double)g_draw.width - 1.0) / size);
    row1 = (int)floor((camy + (double)g_draw.height - 1.0) / size);
    if (col0 < 0) col0 = 0;
    if (row0 < 0) row0 = 0;
    if (col1 >= map->cols) col1 = map->cols - 1;
    if (row1 >= map->rows) row1 = map->rows - 1;
    if (col0 > col1 || row0 > row1)
        return 0;

    for (row = row0; row <= row1; ++row) {
        for (col = col0; col <= col1; ++col) {
            int tile = map->cells[row * TILEMAP_COL_MAX + col];
            SpriteEntry *sprite;
            double left, top;
            int sx0, sx1, sy0, sy1;
            int sx, sy;

            if (tile < 0)
                continue;
            sprite = &g_sprites[find_sprite_name(map->tiles[tile])];
            left = (double)col * size - camx;
            top = (double)row * size - camy;
            sx0 = (int)floor(left);
            sy0 = (int)floor(top);
            sx1 = (int)ceil(left + size) - 1;
            sy1 = (int)ceil(top + size) - 1;
            if (sx0 < 0) sx0 = 0;
            if (sy0 < 0) sy0 = 0;
            if (sx1 >= g_draw.width) sx1 = g_draw.width - 1;
            if (sy1 >= g_draw.height) sy1 = g_draw.height - 1;
            if (sx0 > sx1 || sy0 > sy1)
                continue;

            for (sy = sy0; sy <= sy1; ++sy) {
                int src_y = (int)(((double)sy - top) * sprite->height / size);

                if (src_y < 0) src_y = 0;
                if (src_y >= sprite->height) src_y = sprite->height - 1;

                for (sx = sx0; sx <= sx1; ++sx) {
                    int src_x = (int)(((double)sx - left) * sprite->width /
                                      size);
                    int value;

                    if (src_x < 0) src_x = 0;
                    if (src_x >= sprite->width) src_x = sprite->width - 1;
                    value = sprite->pixels[(size_t)src_y * sprite->width +
                                           (size_t)src_x];
                    if (value < 0 || value >= g_palette_count ||
                        !g_palette[value].used)
                        continue;
                    {
                        uint32_t color = make_color(g_palette[value].r,
                                                    g_palette[value].g,
                                                    g_palette[value].b,
                                                    g_palette[value].a);
                        size_t dst = (size_t)sy * g_draw.width + (size_t)sx;

                        g_draw.pixels[dst] =
                            blend_color(g_draw.pixels[dst], color);
                    }
                }
            }
        }
    }

    mark_dirty_rect((int)floor(-camx), (int)floor(-camy),
                    map->cols * map->size, map->rows * map->size);
    return 0;
}

static int l_camera(lua_State *L)
{
    if (lua_gettop(L) == 0) {
        lua_pushnumber(L, g_camera_x);
        lua_pushnumber(L, g_camera_y);
        return 2;
    }

    {
        double x = luaL_optnumber(L, 1, g_camera_x);
        double y = luaL_optnumber(L, 2, g_camera_y);

        if (!isfinite(x) || !isfinite(y) || fabs(x) > 1000000000.0 ||
            fabs(y) > 1000000000.0)
            return luaL_error(L, "draw.camera: invalid position");
        g_camera_x = x;
        g_camera_y = y;
    }
    lua_pushnumber(L, g_camera_x);
    lua_pushnumber(L, g_camera_y);
    return 2;
}

static int l_screentoworld(lua_State *L)
{
    double x = luaL_checknumber(L, 1);
    double y = luaL_checknumber(L, 2);

    if (!isfinite(x) || !isfinite(y))
        return luaL_error(L, "draw.screentoworld: invalid position");
    lua_pushnumber(L, x + g_camera_x);
    lua_pushnumber(L, y + g_camera_y);
    return 2;
}

static int l_tilemapremove(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    int index = find_tilemap_name(name);

    if (index < 0) {
        lua_pushboolean(L, 0);
        return 1;
    }
    tilemap_remove_bodies(&g_tilemaps[index]);
    memset(&g_tilemaps[index], 0, sizeof(g_tilemaps[index]));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_tilemapcount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < TILEMAP_MAX; ++i) {
        if (g_tilemaps[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int ttf_find_font(lua_State *L, int index)
{
    if (lua_type(L, index) == LUA_TNUMBER) {
        lua_Integer id = luaL_checkinteger(L, index);

        if (id < 1 || id > TTF_FONT_MAX || !g_ttf_fonts[id - 1].used) {
            luaL_error(L, "draw.ttf: font is not defined");
            return -1;
        }
        return (int)id - 1;
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        const char *name = lua_tostring(L, index);
        int i;

        for (i = 0; i < TTF_FONT_MAX; ++i) {
            if (g_ttf_fonts[i].used && strcmp(g_ttf_fonts[i].name, name) == 0)
                return i;
        }
    }
    luaL_error(L, "draw.ttf: font is not defined");
    return -1;
}

static void ttf_cache_free(TtfCacheEntry *entry)
{
    if (entry->surface)
        SDL_FreeSurface(entry->surface);
    free(entry->text);
    memset(entry, 0, sizeof(*entry));
}

static void ttf_cache_clear_font(int font_id)
{
    int i;

    for (i = 0; i < TTF_CACHE_MAX; ++i) {
        if (g_ttf_cache[i].used && g_ttf_cache[i].font_id == font_id)
            ttf_cache_free(&g_ttf_cache[i]);
    }
}

static SDL_Surface *ttf_render_text(lua_State *L, int font_id, const char *text)
{
    TtfEntry *font = &g_ttf_fonts[font_id];
    TtfCacheEntry *entry = NULL;
    TtfCacheEntry *victim = NULL;
    SDL_Color white = {255, 255, 255, 255};
    SDL_Surface *rendered;
    SDL_Surface *converted;
    size_t len;
    int i;

    for (i = 0; i < TTF_CACHE_MAX; ++i) {
        if (g_ttf_cache[i].used && g_ttf_cache[i].font_id == font_id &&
            strcmp(g_ttf_cache[i].text, text) == 0) {
            entry = &g_ttf_cache[i];
            break;
        }
    }
    if (entry) {
        entry->stamp = ++g_ttf_stamp;
        return entry->surface;
    }

    rendered = TTF_RenderUTF8_Blended(font->font, text, white);
    if (!rendered) {
        luaL_error(L, "draw.ttf: cannot render text: %s", TTF_GetError());
        return NULL;
    }
    converted = SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(rendered);
    if (!converted) {
        luaL_error(L, "draw.ttf: cannot convert text: %s", SDL_GetError());
        return NULL;
    }

    len = strlen(text) + 1;
    for (i = 0; i < TTF_CACHE_MAX; ++i) {
        if (!g_ttf_cache[i].used) {
            entry = &g_ttf_cache[i];
            break;
        }
        if (!victim || g_ttf_cache[i].stamp < victim->stamp)
            victim = &g_ttf_cache[i];
    }
    if (!entry)
        entry = victim;
    ttf_cache_free(entry);
    entry->text = (char *)malloc(len);
    if (!entry->text) {
        SDL_FreeSurface(converted);
        luaL_error(L, "draw.ttf: out of memory");
        return NULL;
    }
    memcpy(entry->text, text, len);
    entry->surface = converted;
    entry->font_id = font_id;
    entry->stamp = ++g_ttf_stamp;
    entry->used = 1;
    return converted;
}

static void ttf_blit(SDL_Surface *surface, int x, int y,
                     int r, int g, int b, int a)
{
    int width = surface->w;
    int height = surface->h;
    int sx, sy;

    for (sy = 0; sy < height; ++sy) {
        uint32_t *row = (uint32_t *)((uint8_t *)surface->pixels +
                                     (size_t)sy * surface->pitch);

        for (sx = 0; sx < width; ++sx) {
            uint32_t src = row[sx];
            int sa = (int)((src >> 24) & 0xFF);
            int sr, sg, sb;
            uint32_t color;
            int dx = x + sx;
            int dy = y + sy;
            size_t dst;

            if (dx < 0 || dy < 0 || dx >= g_draw.width || dy >= g_draw.height)
                continue;
            sa = sa * a * g_draw.alpha / 65025;
            if (sa <= 0)
                continue;
            sr = (int)(((src >> 16) & 0xFF) * r / 255);
            sg = (int)(((src >> 8) & 0xFF) * g / 255);
            sb = (int)((src & 0xFF) * b / 255);
            color = ((uint32_t)sa << 24) |
                    ((uint32_t)sr << 16) |
                    ((uint32_t)sg << 8) |
                    (uint32_t)sb;
            dst = (size_t)dy * g_draw.width + (size_t)dx;
            g_draw.pixels[dst] = blend_color(g_draw.pixels[dst], color);
        }
    }
    mark_dirty_rect(x, y, width, height);
}

static int ttf_parse_style(lua_State *L, int index)
{
    int style = TTF_STYLE_NORMAL;

    if (lua_isnoneornil(L, index))
        return style;
    if (lua_type(L, index) == LUA_TNUMBER) {
        lua_Integer value = luaL_checkinteger(L, index);

        if (value < 0 || value > 0xFFFF)
            return luaL_error(L, "draw.ttf: style is out of range");
        return (int)value;
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        const char *name = lua_tostring(L, index);

        if (strcmp(name, "normal") == 0)
            return TTF_STYLE_NORMAL;
        if (strcmp(name, "bold") == 0)
            return TTF_STYLE_BOLD;
        if (strcmp(name, "italic") == 0)
            return TTF_STYLE_ITALIC;
        if (strcmp(name, "underline") == 0)
            return TTF_STYLE_UNDERLINE;
        if (strcmp(name, "strikethrough") == 0)
            return TTF_STYLE_STRIKETHROUGH;
    }
    return luaL_error(L,
        "draw.ttf: style must be normal, bold, italic, underline, or "
        "strikethrough");
}

static int l_ttf(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *path = luaL_checkstring(L, 2);
    lua_Integer ptsize = luaL_checkinteger(L, 3);
    int style;
    lua_Integer outline;
    size_t name_len = strlen(name);
    TtfEntry *font = NULL;
    TTF_Font *loaded;
    int i;

    if (name_len == 0 || name_len >= sizeof(font->name))
        return luaL_error(L, "draw.ttf: invalid name");
    if (ptsize < 1 || ptsize > 256)
        return luaL_error(L, "draw.ttf: ptsize must be 1..256");
    style = ttf_parse_style(L, 4);
    if (lua_isnoneornil(L, 4))
        style = TTF_STYLE_NORMAL;
    outline = luaL_optinteger(L, 5, 0);
    if (outline < 0 || outline > 8)
        return luaL_error(L, "draw.ttf: outline must be 0..8");
    if (TTF_Init() != 0)
        return luaL_error(L, "draw.ttf: TTF_Init failed: %s", TTF_GetError());

    for (i = 0; i < TTF_FONT_MAX; ++i) {
        if (g_ttf_fonts[i].used && strcmp(g_ttf_fonts[i].name, name) == 0)
            return luaL_error(L, "draw.ttf: '%s' is already defined", name);
        if (!g_ttf_fonts[i].used && !font)
            font = &g_ttf_fonts[i];
    }
    if (!font)
        return luaL_error(L, "draw.ttf: limit of %d fonts reached",
                          TTF_FONT_MAX);

    loaded = TTF_OpenFont(path, (int)ptsize);
    if (!loaded)
        return luaL_error(L, "draw.ttf: cannot open '%s': %s", path,
                          TTF_GetError());
    TTF_SetFontStyle(loaded, style);
    TTF_SetFontOutline(loaded, (int)outline);
    memset(font, 0, sizeof(*font));
    font->used = 1;
    strcpy(font->name, name);
    font->font = loaded;
    font->ptsize = (int)ptsize;
    lua_pushinteger(L, (lua_Integer)(font - g_ttf_fonts + 1));
    return 1;
}

static int l_ttftext(lua_State *L)
{
    lua_Integer xi = luaL_checkinteger(L, 1);
    lua_Integer yi = luaL_checkinteger(L, 2);
    const char *text = luaL_checkstring(L, 4);
    int font_id;
    SDL_Surface *surface;
    int r = 255, g = 255, b = 255, a = 255;
    int extra_alpha;

    if (!ensure_open(L, "draw.ttftext"))
        return 0;
    if (xi < -1000000 || xi > 1000000 || yi < -1000000 || yi > 1000000)
        return luaL_error(L, "draw.ttftext: position out of range");
    if (strlen(text) > 8192)
        return luaL_error(L, "draw.ttftext: text is too long");
    font_id = ttf_find_font(L, 3);
    extra_alpha = clamp_alpha_number(luaL_optnumber(L, 6, 1.0));
    if (lua_gettop(L) >= 5 && !lua_isnoneornil(L, 5)) {
        if (read_color(L, 5, &r, &g, &b, &a) != 0)
            return 0;
    }
    a = a * extra_alpha / 255;
    surface = ttf_render_text(L, font_id, text);
    ttf_blit(surface, (int)xi, (int)yi, r, g, b, a);
    return 0;
}

static int l_ttfsize(lua_State *L)
{
    int font_id = ttf_find_font(L, 1);
    const char *text = luaL_checkstring(L, 2);
    SDL_Surface *surface;

    if (strlen(text) > 8192)
        return luaL_error(L, "draw.ttfsize: text is too long");
    surface = ttf_render_text(L, font_id, text);
    lua_pushinteger(L, surface->w);
    lua_pushinteger(L, surface->h);
    return 2;
}

static int l_ttfremove(lua_State *L)
{
    int font_id = ttf_find_font(L, 1);

    ttf_cache_clear_font(font_id);
    TTF_CloseFont(g_ttf_fonts[font_id].font);
    memset(&g_ttf_fonts[font_id], 0, sizeof(g_ttf_fonts[font_id]));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_ttfcount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < TTF_FONT_MAX; ++i) {
        if (g_ttf_fonts[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int mixer_init(lua_State *L)
{
    int flags = MIX_INIT_OGG | MIX_INIT_MP3 | MIX_INIT_MOD | MIX_INIT_FLAC;
    int spec_count;

    if (g_mixer_ready)
        return 0;
    Mix_Init(flags);
    spec_count = Mix_QuerySpec(NULL, NULL, NULL);
    if (spec_count == 0 && Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2,
                                         2048) != 0) {
        luaL_error(L, "draw.mixer: cannot open audio: %s", Mix_GetError());
        return -1;
    }
    g_mixer_ready = 1;
    for (int i = 0; i < MIXER_CHANNEL_MAX; ++i) {
        g_channel_left[i] = 127;
        g_channel_right[i] = 127;
    }
    Mix_Volume(-1, MIX_MAX_VOLUME);
    Mix_VolumeMusic(MIX_MAX_VOLUME);
    return 0;
}

static MixerEntry *mixer_find(lua_State *L, int index, int kind)
{
    if (lua_type(L, index) == LUA_TNUMBER) {
        lua_Integer id = luaL_checkinteger(L, index);

        if (id < 1 || id > MIXER_ASSET_MAX || !g_mixer_assets[id - 1].used ||
            g_mixer_assets[id - 1].kind != kind) {
            luaL_error(L, "draw.mixer: asset is not defined");
            return NULL;
        }
        return &g_mixer_assets[id - 1];
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        const char *name = lua_tostring(L, index);
        int i;

        for (i = 0; i < MIXER_ASSET_MAX; ++i) {
            if (g_mixer_assets[i].used && g_mixer_assets[i].kind == kind &&
                strcmp(g_mixer_assets[i].name, name) == 0)
                return &g_mixer_assets[i];
        }
    }
    luaL_error(L, "draw.mixer: asset is not defined");
    return NULL;
}

static int mixer_volume(lua_State *L, int index, int fallback)
{
    if (lua_isnoneornil(L, index))
        return fallback;
    return clamp_alpha_number(luaL_optnumber(L, index, 1.0));
}

static MixerEntry *mixer_alloc(lua_State *L, int kind, const char *name)
{
    MixerEntry *entry = NULL;
    int i;

    for (i = 0; i < MIXER_ASSET_MAX; ++i) {
        if (g_mixer_assets[i].used &&
            strcmp(g_mixer_assets[i].name, name) == 0) {
            luaL_error(L, "draw.mixer: '%s' is already defined", name);
            return NULL;
        }
        if (!g_mixer_assets[i].used && !entry)
            entry = &g_mixer_assets[i];
    }
    if (!entry) {
        luaL_error(L, "draw.mixer: limit of %d assets reached",
                   MIXER_ASSET_MAX);
        return NULL;
    }
    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    entry->kind = kind;
    strcpy(entry->name, name);
    return entry;
}

static int l_sample(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *path = luaL_checkstring(L, 2);
    MixerEntry *entry;
    Mix_Chunk *chunk;

    if (strlen(name) == 0 || strlen(name) >= sizeof(entry->name))
        return luaL_error(L, "draw.sample: invalid name");
    if (mixer_init(L) != 0)
        return 0;
    entry = mixer_alloc(L, 1, name);
    if (!entry)
        return 0;
    chunk = Mix_LoadWAV(path);
    if (!chunk) {
        memset(entry, 0, sizeof(*entry));
        return luaL_error(L, "draw.sample: cannot open '%s': %s", path,
                          Mix_GetError());
    }
    entry->chunk = chunk;
    lua_pushinteger(L, (lua_Integer)(entry - g_mixer_assets + 1));
    return 1;
}

static int l_music(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *path = luaL_checkstring(L, 2);
    MixerEntry *entry;
    Mix_Music *music;

    if (strlen(name) == 0 || strlen(name) >= sizeof(entry->name))
        return luaL_error(L, "draw.music: invalid name");
    if (mixer_init(L) != 0)
        return 0;
    entry = mixer_alloc(L, 2, name);
    if (!entry)
        return 0;
    music = Mix_LoadMUS(path);
    if (!music) {
        memset(entry, 0, sizeof(*entry));
        return luaL_error(L, "draw.music: cannot open '%s': %s", path,
                          Mix_GetError());
    }
    entry->music = music;
    lua_pushinteger(L, (lua_Integer)(entry - g_mixer_assets + 1));
    return 1;
}

static int l_playsample(lua_State *L)
{
    MixerEntry *entry = mixer_find(L, 1, 1);
    int volume = mixer_volume(L, 2, 255);
    lua_Integer loops = luaL_optinteger(L, 3, 0);
    int channel;
    double pan;
    int has_x = !lua_isnoneornil(L, 4);
    int has_y = !lua_isnoneornil(L, 5);
    int has_pan = !lua_isnoneornil(L, 6);

    if (!entry)
        return 0;
    if (loops < -1 || loops > 1000000)
        return luaL_error(L, "draw.playsample: loops must be -1 or non-negative");
    if (has_x || has_y) {
        double x = luaL_optnumber(L, 4, g_listener_x);
        double y = luaL_optnumber(L, 5, g_listener_y);
        double dx = x - g_listener_x;
        double dy = y - g_listener_y;
        double dist = sqrt(dx * dx + dy * dy);

        if (!isfinite(x) || !isfinite(y))
            return luaL_error(L, "draw.playsample: invalid position");
        volume = (int)(volume *
                       fmax(0.0, 1.0 - dist / g_listener_spread) + 0.5);
        if (!has_pan && g_listener_spread > 0.0)
            pan = fmax(-1.0, fmin(1.0, dx / g_listener_spread));
        else
            pan = 0.0;
    } else {
        pan = has_pan ? luaL_checknumber(L, 6) : 0.0;
    }
    if (!isfinite(pan) || pan < -1.0 || pan > 1.0)
        return luaL_error(L, "draw.playsample: pan must be -1..1");

    channel = Mix_PlayChannelTimed(-1, entry->chunk, (int)loops, -1);
    if (channel < 0)
        return luaL_error(L, "draw.playsample: no free channel: %s",
                          Mix_GetError());
    Mix_Volume(channel, volume);
    if (has_pan || has_x || has_y) {
        int left = (int)(255.0 * (1.0 - pan) * 0.5 + 0.5);
        int right = (int)(255.0 * (1.0 + pan) * 0.5 + 0.5);

        Mix_SetPanning(channel, left, right);
        if (channel < MIXER_CHANNEL_MAX) {
            g_channel_left[channel] = left;
            g_channel_right[channel] = right;
        }
    }
    lua_pushinteger(L, channel);
    return 1;
}

static int l_playmusic(lua_State *L)
{
    MixerEntry *entry = mixer_find(L, 1, 2);
    lua_Integer loops = luaL_optinteger(L, 2, 0);
    int volume = mixer_volume(L, 3, 255);

    if (!entry)
        return 0;
    if (loops < -1 || loops > 1000000)
        return luaL_error(L, "draw.playmusic: loops must be -1 or non-negative");
    Mix_VolumeMusic(volume);
    if (Mix_PlayMusic(entry->music, (int)loops) != 0)
        return luaL_error(L, "draw.playmusic: %s", Mix_GetError());
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stopmusic(lua_State *L)
{
    Mix_HaltMusic();
    lua_pushboolean(L, 1);
    return 1;
}

static int l_musicpause(lua_State *L)
{
    int pause = lua_isnoneornil(L, 1) ? !Mix_PausedMusic() :
                (lua_toboolean(L, 1) ? 1 : 0);

    if (pause)
        Mix_PauseMusic();
    else
        Mix_ResumeMusic();
    lua_pushboolean(L, Mix_PausedMusic());
    return 1;
}

static int l_musicplaying(lua_State *L)
{
    lua_pushboolean(L, Mix_PlayingMusic() && !Mix_PausedMusic());
    return 1;
}

static int l_musicvolume(lua_State *L)
{
    if (!lua_isnoneornil(L, 1))
        Mix_VolumeMusic(mixer_volume(L, 1, 255));
    lua_pushinteger(L, Mix_VolumeMusic(-1));
    return 1;
}

static int l_mastervolume(lua_State *L)
{
    if (!lua_isnoneornil(L, 1))
        Mix_Volume(-1, mixer_volume(L, 1, 255));
    lua_pushinteger(L, Mix_Volume(-1, -1));
    return 1;
}

static int l_channelvolume(lua_State *L)
{
    int channel = (int)luaL_checkinteger(L, 1);

    if (channel < 0)
        return luaL_error(L, "draw.channelvolume: invalid channel");
    if (!lua_isnoneornil(L, 2))
        Mix_Volume(channel, mixer_volume(L, 2, 255));
    lua_pushinteger(L, Mix_Volume(channel, -1));
    return 1;
}

static int l_channelpan(lua_State *L)
{
    int channel = (int)luaL_checkinteger(L, 1);
    int left, right;

    if (channel < 0)
        return luaL_error(L, "draw.channelpan: invalid channel");
    if (lua_isnoneornil(L, 2)) {
        if (channel >= MIXER_CHANNEL_MAX)
            return luaL_error(L, "draw.channelpan: invalid channel");
        lua_pushnumber(L, (double)(g_channel_right[channel] -
                                   g_channel_left[channel]) / 255.0);
        return 1;
    }

    {
        double pan = luaL_checknumber(L, 2);

        if (!isfinite(pan) || pan < -1.0 || pan > 1.0)
            return luaL_error(L, "draw.channelpan: pan must be -1..1");
        left = (int)(255.0 * (1.0 - pan) * 0.5 + 0.5);
        right = (int)(255.0 * (1.0 + pan) * 0.5 + 0.5);
        if (Mix_SetPanning(channel, left, right) != 0)
            return luaL_error(L, "draw.channelpan: %s", Mix_GetError());
        if (channel < MIXER_CHANNEL_MAX) {
            g_channel_left[channel] = left;
            g_channel_right[channel] = right;
        }
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_stopchannel(lua_State *L)
{
    int channel = (int)luaL_checkinteger(L, 1);

    if (channel < 0)
        return luaL_error(L, "draw.stopchannel: invalid channel");
    Mix_HaltChannel(channel);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_listener(lua_State *L)
{
    if (!lua_isnoneornil(L, 1)) {
        double x = luaL_checknumber(L, 1);
        double y = luaL_checknumber(L, 2);
        double spread = luaL_optnumber(L, 3, g_listener_spread);

        if (!isfinite(x) || !isfinite(y) || !isfinite(spread) || spread <= 0.0)
            return luaL_error(L, "draw.listener: invalid position or spread");
        g_listener_x = x;
        g_listener_y = y;
        g_listener_spread = spread;
    }
    lua_pushnumber(L, g_listener_x);
    lua_pushnumber(L, g_listener_y);
    lua_pushnumber(L, g_listener_spread);
    return 3;
}

static int l_mixerremove(lua_State *L)
{
    MixerEntry *entry = mixer_find(L, 1, lua_isnoneornil(L, 2) ? 1 :
                                   (int)luaL_checkinteger(L, 2));

    if (!entry)
        return 0;
    if (entry->chunk)
        Mix_FreeChunk(entry->chunk);
    if (entry->music)
        Mix_FreeMusic(entry->music);
    memset(entry, 0, sizeof(*entry));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_mixercount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < MIXER_ASSET_MAX; ++i) {
        if (g_mixer_assets[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static ShaderEntry *shader_find_arg(lua_State *L, int index)
{
    if (lua_type(L, index) == LUA_TNUMBER) {
        lua_Integer id = luaL_checkinteger(L, index);

        return shader_by_id(id);
    }
    if (lua_type(L, index) == LUA_TSTRING) {
        int id = find_shader_name(lua_tostring(L, index));

        return id > 0 ? &g_shaders[id - 1] : NULL;
    }
    return NULL;
}

static int l_shader(lua_State *L)
{
    const char *name = luaL_checkstring(L, 1);
    const char *fragment = luaL_checkstring(L, 2);
    const char *vertex = luaL_optstring(L, 3, g_shader_default_vertex);
    char log[1024];
    ShaderEntry *entry = NULL;
    GLuint program;
    size_t name_len = strlen(name);
    int i;

    if (!ensure_open(L, "draw.shader") || !g_draw.gl_ready)
        return luaL_error(L, "draw.shader: OpenGL shaders are unavailable");
    if (name_len == 0 || name_len >= SHADER_NAME_MAX)
        return luaL_error(L, "draw.shader: invalid name");
    if (strlen(fragment) == 0 || strlen(fragment) >= SHADER_SOURCE_MAX)
        return luaL_error(L, "draw.shader: invalid fragment source");
    if (strlen(vertex) == 0 || strlen(vertex) >= SHADER_SOURCE_MAX)
        return luaL_error(L, "draw.shader: invalid vertex source");

    for (i = 0; i < SHADER_MAX; ++i) {
        if (g_shaders[i].used && strcmp(g_shaders[i].name, name) == 0)
            return luaL_error(L, "draw.shader: '%s' is already defined", name);
        if (!g_shaders[i].used && !entry)
            entry = &g_shaders[i];
    }
    if (!entry)
        return luaL_error(L, "draw.shader: limit of %d shaders reached",
                          SHADER_MAX);

    program = shader_link_program(vertex, fragment, log, sizeof(log));
    if (!program)
        return luaL_error(L, "draw.shader: %s",
                          log[0] != '\0' ? log : "shader compilation failed");

    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    strcpy(entry->name, name);
    entry->program = program;
    lua_pushinteger(L, (lua_Integer)(entry - g_shaders + 1));
    return 1;
}

static int l_setshader(lua_State *L)
{
    int previous = g_active_shader;
    ShaderEntry *entry;

    if (!ensure_open(L, "draw.setshader") || !g_draw.gl_ready)
        return luaL_error(L, "draw.setshader: OpenGL shaders are unavailable");
    if (lua_isnoneornil(L, 1) ||
        (lua_type(L, 1) == LUA_TNUMBER && lua_tointeger(L, 1) == 0)) {
        g_active_shader = 0;
        lua_pushinteger(L, previous);
        return 1;
    }
    entry = shader_find_arg(L, 1);
    if (!entry)
        return luaL_error(L, "draw.setshader: shader is not defined");
    g_active_shader = (int)(entry - g_shaders + 1);
    lua_pushinteger(L, previous);
    return 1;
}

static int l_shaderparam(lua_State *L)
{
    ShaderEntry *entry;
    const char *name;
    GLint location;
    double values[4];
    int count = lua_gettop(L);
    int i;

    if (!ensure_open(L, "draw.shaderparam") || !g_draw.gl_ready)
        return luaL_error(L, "draw.shaderparam: OpenGL shaders are unavailable");
    entry = shader_find_arg(L, 1);
    if (!entry)
        return luaL_error(L, "draw.shaderparam: shader is not defined");
    name = luaL_checkstring(L, 2);
    if (name[0] == '\0' || count < 3 || count > 6)
        return luaL_error(L, "draw.shaderparam: expected name and 1..4 values");
    for (i = 0; i < count - 2; ++i) {
        values[i] = luaL_checknumber(L, 3 + i);
        if (!isfinite(values[i]))
            return luaL_error(L, "draw.shaderparam: values must be finite");
    }

    glUseProgram(entry->program);
    location = glGetUniformLocation(entry->program, name);
    if (location >= 0) {
        if (count == 3)
            glUniform1f(location, (float)values[0]);
        else if (count == 4)
            glUniform2f(location, (float)values[0], (float)values[1]);
        else if (count == 5)
            glUniform3f(location, (float)values[0], (float)values[1],
                        (float)values[2]);
        else
            glUniform4f(location, (float)values[0], (float)values[1],
                        (float)values[2], (float)values[3]);
    }
    glUseProgram(shader_active_program());
    lua_pushboolean(L, location >= 0);
    return 1;
}

static int l_shaderremove(lua_State *L)
{
    ShaderEntry *entry;

    if (!ensure_open(L, "draw.shaderremove") || !g_draw.gl_ready)
        return luaL_error(L, "draw.shaderremove: OpenGL shaders are unavailable");
    entry = shader_find_arg(L, 1);
    if (!entry)
        return luaL_error(L, "draw.shaderremove: shader is not defined");
    if (entry->program) {
        glDeleteProgram(entry->program);
        entry->program = 0;
    }
    if (g_active_shader == (int)(entry - g_shaders + 1))
        g_active_shader = 0;
    memset(entry, 0, sizeof(*entry));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_shadercount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < SHADER_MAX; ++i) {
        if (g_shaders[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_particle(lua_State *L)
{
    lua_Number x = luaL_checknumber(L, 1);
    lua_Number y = luaL_checknumber(L, 2);
    lua_Number vx = luaL_optnumber(L, 3, 0.0);
    lua_Number vy = luaL_optnumber(L, 4, 0.0);
    lua_Number life = luaL_checknumber(L, 5);
    lua_Integer size = luaL_optinteger(L, 7, 4);
    lua_Number gravity = luaL_optnumber(L, 8, 0.0);
    lua_Number drag = luaL_optnumber(L, 9, 1.0);
    ParticleEntry *entry = NULL;
    int r, g, b, a;
    int i;

    if (!isfinite(x) || !isfinite(y) || !isfinite(vx) || !isfinite(vy) ||
        !isfinite(life) || !isfinite(gravity) || !isfinite(drag) ||
        fabs(x) > 1000000000.0 || fabs(y) > 1000000000.0 ||
        fabs(vx) > 1000000000.0 || fabs(vy) > 1000000000.0)
        return luaL_error(L, "draw.particle: values are out of range");
    if (life <= 0.0 || life > 60.0)
        return luaL_error(L, "draw.particle: life must be greater than 0 and at most 60");
    if (size < 1 || size > 64)
        return luaL_error(L, "draw.particle: size must be 1..64");
    if (drag <= 0.0 || drag > 1.0)
        return luaL_error(L, "draw.particle: drag must be greater than 0 and at most 1");
    if (read_color(L, 6, &r, &g, &b, &a) < 0)
        return luaL_error(L, "draw.particle: expected a color");

    for (i = 0; i < PARTICLE_MAX; ++i) {
        if (!g_particles[i].used) {
            entry = &g_particles[i];
            break;
        }
    }
    if (!entry)
        return luaL_error(L, "draw.particle: limit of %d particles reached",
                          PARTICLE_MAX);

    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    entry->x = (double)x;
    entry->y = (double)y;
    entry->vx = (double)vx;
    entry->vy = (double)vy;
    entry->life = (double)life;
    entry->max_life = (double)life;
    entry->gravity = (double)gravity;
    entry->drag = (double)drag;
    entry->size = (int)size;
    entry->r = r;
    entry->g = g;
    entry->b = b;
    entry->a = a;
    lua_pushinteger(L, (lua_Integer)(entry - g_particles + 1));
    return 1;
}

static int l_emit(lua_State *L)
{
    lua_Number x = luaL_checknumber(L, 1);
    lua_Number y = luaL_checknumber(L, 2);
    lua_Integer count = luaL_checkinteger(L, 3);
    lua_Number speed = luaL_checknumber(L, 4);
    lua_Number life = luaL_checknumber(L, 5);
    lua_Integer size = luaL_optinteger(L, 7, 4);
    lua_Number angle = luaL_optnumber(L, 8, 0.0);
    lua_Number spread = luaL_optnumber(L, 9, 360.0);
    lua_Number gravity = luaL_optnumber(L, 10, 0.0);
    lua_Number drag = luaL_optnumber(L, 11, 1.0);
    double base = (double)angle * AUX_PI / 180.0;
    double range = fabs((double)spread) * AUX_PI / 360.0;
    int r, g, b, a;
    int free_count = 0;
    int i;

    if (!isfinite(x) || !isfinite(y) || !isfinite(speed) || !isfinite(life) ||
        !isfinite(angle) || !isfinite(spread) || !isfinite(gravity) ||
        !isfinite(drag) || fabs(x) > 1000000000.0 ||
        fabs(y) > 1000000000.0 || fabs(speed) > 1000000000.0 ||
        fabs(angle) > 1000000000.0 || fabs(spread) > 1000000000.0)
        return luaL_error(L, "draw.emit: values are out of range");
    if (count < 1 || count > 256)
        return luaL_error(L, "draw.emit: count must be 1..256");
    if (speed < 0.0 || speed > 1000000000.0)
        return luaL_error(L, "draw.emit: speed must be between 0 and 1000000000");
    if (life <= 0.0 || life > 60.0)
        return luaL_error(L, "draw.emit: life must be greater than 0 and at most 60");
    if (size < 1 || size > 64)
        return luaL_error(L, "draw.emit: size must be 1..64");
    if (drag <= 0.0 || drag > 1.0)
        return luaL_error(L, "draw.emit: drag must be greater than 0 and at most 1");
    if (read_color(L, 6, &r, &g, &b, &a) < 0)
        return luaL_error(L, "draw.emit: expected a color");
    for (i = 0; i < PARTICLE_MAX; ++i) {
        if (!g_particles[i].used)
            ++free_count;
    }
    if (free_count < count)
        return luaL_error(L, "draw.emit: not enough free particles");

    for (i = 0; i < (int)count; ++i) {
        ParticleEntry *entry = NULL;
        double dir = base + range * ((double)rand() / (double)RAND_MAX * 2.0 - 1.0);
        int j;

        for (j = 0; j < PARTICLE_MAX; ++j) {
            if (!g_particles[j].used) {
                entry = &g_particles[j];
                break;
            }
        }
        memset(entry, 0, sizeof(*entry));
        entry->used = 1;
        entry->x = (double)x;
        entry->y = (double)y;
        entry->vx = cos(dir) * (double)speed;
        entry->vy = sin(dir) * (double)speed;
        entry->life = (double)life * (0.75 + 0.25 *
            ((double)rand() / (double)RAND_MAX));
        entry->max_life = (double)life;
        entry->gravity = (double)gravity;
        entry->drag = (double)drag;
        entry->size = (int)size;
        entry->r = r;
        entry->g = g;
        entry->b = b;
        entry->a = a;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_particlecount(lua_State *L);

static int l_particleupdate(lua_State *L)
{
    double dt = aux_step(L, 1, "draw.particleupdate");

    particle_update(dt);
    lua_pushinteger(L, l_particlecount(L));
    return 1;
}

static int l_particledraw(lua_State *L)
{
    if (!ensure_open(L, "draw.particledraw"))
        return 0;
    particle_draw_all();
    return 0;
}

static int l_particleclear(lua_State *L)
{
    particle_clear_state();
    lua_pushboolean(L, 1);
    return 1;
}

static int l_particlecount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < PARTICLE_MAX; ++i) {
        if (g_particles[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

static int l_tween(lua_State *L)
{
    lua_Number from = luaL_checknumber(L, 1);
    lua_Number to = luaL_checknumber(L, 2);
    lua_Number duration = luaL_checknumber(L, 3);
    lua_Number delay = luaL_optnumber(L, 5, 0.0);
    TweenEntry *entry = NULL;
    int easing;
    int i;

    if (!isfinite(from) || !isfinite(to) || !isfinite(duration) ||
        !isfinite(delay) || duration <= 0.0 || duration > 3600.0 ||
        delay < 0.0 || delay > 3600.0)
        return luaL_error(L, "draw.tween: invalid duration or delay");
    easing = parse_easing(L, 4, "draw.tween");
    for (i = 0; i < TWEEN_MAX; ++i) {
        if (!g_tweens[i].used) {
            entry = &g_tweens[i];
            break;
        }
    }
    if (!entry)
        return luaL_error(L, "draw.tween: limit of %d tweens reached",
                          TWEEN_MAX);

    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    entry->from = (double)from;
    entry->to = (double)to;
    entry->delay = (double)delay;
    entry->duration = (double)duration;
    entry->easing = easing;
    lua_pushinteger(L, (lua_Integer)(entry - g_tweens + 1));
    return 1;
}

static int l_tweenvalue(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);
    const TweenEntry *entry;

    if (id < 1 || id > TWEEN_MAX || !g_tweens[id - 1].used)
        return luaL_error(L, "draw.tweenvalue: tween is not defined");
    entry = &g_tweens[id - 1];
    lua_pushnumber(L, (lua_Number)tween_value(entry));
    lua_pushboolean(L, tween_finished(entry));
    return 2;
}

static int l_tweenupdate(lua_State *L)
{
    double dt = aux_step(L, 1, "draw.tweenupdate");
    int active = 0;
    int i;

    tween_update(dt);
    for (i = 0; i < TWEEN_MAX; ++i) {
        if (g_tweens[i].used && !tween_finished(&g_tweens[i]))
            ++active;
    }
    lua_pushinteger(L, active);
    return 1;
}

static int l_tweenremove(lua_State *L)
{
    lua_Integer id = luaL_checkinteger(L, 1);

    if (id < 1 || id > TWEEN_MAX || !g_tweens[id - 1].used)
        return luaL_error(L, "draw.tweenremove: tween is not defined");
    memset(&g_tweens[id - 1], 0, sizeof(g_tweens[id - 1]));
    lua_pushboolean(L, 1);
    return 1;
}

static int l_tweenclear(lua_State *L)
{
    tween_clear_state();
    lua_pushboolean(L, 1);
    return 1;
}

static int l_tweencount(lua_State *L)
{
    int count = 0;
    int i;

    for (i = 0; i < TWEEN_MAX; ++i) {
        if (g_tweens[i].used)
            ++count;
    }
    lua_pushinteger(L, count);
    return 1;
}

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
    {"gradient", "draw.gradient(x, y, w, h, color1, color2, [mode])",
     "Заливает прямоугольник градиентом; mode: vertical, horizontal, diagonal, radial."},
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
    {"canvas", "draw.canvas(width, height, [color]) -> id",
     "Создаёт прозрачный буфер; color или {r,g,b,[a]} заливает его."},
    {"setcanvas", "draw.setcanvas([id]) -> previous",
     "Перенаправляет рисование в буфер; без id — обратно в окно."},
    {"canvasclear", "draw.canvasclear([color])",
     "Заливает текущий буфер прозрачным или указанным цветом."},
    {"canvassize", "draw.canvassize(id) -> width, height",
     "Возвращает размер буфера."},
    {"canvasremove", "draw.canvasremove(id) -> boolean",
     "Удаляет буфер; active буфер сбрасывается в окно."},
    {"canvascount", "draw.canvascount() -> count",
     "Возвращает количество буферов."},
    {"canvasdraw", "draw.canvasdraw(id, x, y, [scale], [angle], [flip], [tint], [pivot], [alpha])",
     "Рисует буфер; без pivot (x, y) — левый верхний угол."},
    {"shader", "draw.shader(name, fragment_source, [vertex_source]) -> id",
     "Компилирует GLSL-шейдер для постобработки кадра."},
    {"setshader", "draw.setshader([id or name]) -> previous",
     "Включает шейдер; без аргумента — обычный вывод."},
    {"shaderparam", "draw.shaderparam(id or name, uniform, x[, y[, z[, w]]]) -> boolean",
     "Передаёт float/vec2/vec3/vec4 в шейдер."},
    {"shaderremove", "draw.shaderremove(id or name) -> boolean",
     "Удаляет шейдер."},
    {"shadercount", "draw.shadercount() -> count",
     "Возвращает количество шейдеров."},
    {"particle", "draw.particle(x, y, vx, vy, life, color, [size], [gravity], [drag]) -> id",
     "Создаёт одну частицу."},
    {"emit", "draw.emit(x, y, count, speed, life, color, [size], [angle], [spread], [gravity], [drag]) -> count",
     "Создаёт вспышку частиц; angle и spread в градусах."},
    {"particleupdate", "draw.particleupdate([dt]) -> count",
     "Двигает частицы; по умолчанию dt = draw.delta()."},
    {"particledraw", "draw.particledraw()",
     "Рисует частицы в текущую цель."},
    {"particleclear", "draw.particleclear() -> boolean",
     "Удаляет все частицы."},
    {"particlecount", "draw.particlecount() -> count",
     "Возвращает количество частиц."},
    {"tween", "draw.tween(from, to, duration, [easing], [delay]) -> id",
     "Создаёт твин; easing: linear, quadin, quadout, quadinout, cubicout, sineinout, backout, elasticout."},
    {"tweenvalue", "draw.tweenvalue(id) -> value, done",
     "Возвращает текущее значение твина и признак завершения."},
    {"tweenupdate", "draw.tweenupdate([dt]) -> active",
     "Продвигает твины; по умолчанию dt = draw.delta()."},
    {"tweenremove", "draw.tweenremove(id) -> boolean",
     "Удаляет твин."},
    {"tweenclear", "draw.tweenclear() -> boolean",
     "Удаляет все твины."},
    {"tweencount", "draw.tweencount() -> count",
     "Возвращает количество твинов."},
    {"sample", "draw.sample(name, path) -> id",
     "Загружает SDL_mixer sample: WAV, OGG или другой поддерживаемый формат."},
    {"music", "draw.music(name, path) -> id",
     "Загружает потоковую музыку: OGG, MP3, MOD и другие форматы."},
    {"playsample", "draw.playsample(name, [volume], [loops], [x], [y], [pan]) -> channel",
     "Играет sample; x/y включают пространственное затухание и pan."},
    {"playmusic", "draw.playmusic(name, [loops], [volume]) -> boolean",
     "Играет музыку; loops = -1 — бесконечно."},
    {"stopmusic", "draw.stopmusic() -> boolean",
     "Останавливает музыку."},
    {"musicpause", "draw.musicpause([pause]) -> boolean",
     "Ставит на паузу или продолжает музыку."},
    {"musicplaying", "draw.musicplaying() -> boolean",
     "Возвращает true, если музыка играет не на паузе."},
    {"musicvolume", "draw.musicvolume([volume]) -> volume",
     "Задаёт или возвращает громкость музыки."},
    {"mastervolume", "draw.mastervolume([volume]) -> volume",
     "Задаёт или возвращает общую громкость sample."},
    {"channelvolume", "draw.channelvolume(channel, [volume]) -> volume",
     "Задаёт или возвращает громкость канала."},
    {"channelpan", "draw.channelpan(channel, [pan]) -> pan | boolean",
     "Возвращает или задаёт панораму: -1 слева, 0 центр, 1 справа."},
    {"stopchannel", "draw.stopchannel(channel) -> boolean",
     "Останавливает канал."},
    {"listener", "draw.listener([x], [y], [spread]) -> x, y, spread",
     "Задаёт или возвращает слушателя для пространственного звука."},
    {"mixerremove", "draw.mixerremove(name, [kind]) -> boolean",
     "Удаляет sample или музыку; kind: 1 sample, 2 music."},
    {"mixercount", "draw.mixercount() -> count",
     "Возвращает количество загруженных sample и music."},
    {"ttf", "draw.ttf(name, path, ptsize, [style], [outline]) -> id",
     "Загружает TTF; style: normal, bold, italic, underline, strikethrough."},
    {"ttftext", "draw.ttftext(x, y, font, text, [color], [alpha])",
     "Рисует UTF-8 текст TTF-шрифтом; alpha — 0..1 или 2..255."},
    {"ttfsize", "draw.ttfsize(font, text) -> width, height",
     "Возвращает размер отрендеренной строки."},
    {"ttfremove", "draw.ttfremove(font) -> boolean",
     "Удаляет шрифт и кэш его строк."},
    {"ttfcount", "draw.ttfcount() -> count",
     "Возвращает количество TTF-шрифтов."},
    {"tilemap", "draw.tilemap(name, rows, tiles, size, [solid]) -> id",
     "Создаёт карту из строк или таблиц; solid — true или список индексов."},
    {"drawtilemap", "draw.drawtilemap(name, [camx], [camy])",
     "Рисует карту с камерой; cam по умолчанию draw.camera()."},
    {"camera", "draw.camera([x], [y]) -> x, y",
     "Возвращает или задаёт верхний левый угол камеры в мире."},
    {"screentoworld", "draw.screentoworld(x, y) -> worldx, worldy",
     "Переводит координаты экрана в координаты мира через камеру."},
    {"tilemapremove", "draw.tilemapremove(name) -> boolean",
     "Удаляет карту и созданные ею статические тела."},
    {"tilemapcount", "draw.tilemapcount() -> count",
     "Возвращает количество карт."},
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
    {"physicsworld", "draw.physicsworld([gx], [gy]) -> gx, gy",
     "Задаёт или возвращает гравитацию физического мира."},
    {"physicsclear", "draw.physicsclear() -> boolean",
     "Удаляет все тела и соединения."},
    {"physicsbody", "draw.physicsbody(shape) -> id",
     "Тело: circle, box, sprite или triangle; type dynamic/static/kinematic."},
    {"physicsstep", "draw.physicsstep([dt]) -> contacts",
     "Шагает физика; dt по умолчанию draw.delta()."},
    {"physicsdraw", "draw.physicsdraw([color])",
     "Рисует тела: спрайты или фигуры."},
    {"physicsposition", "draw.physicsposition(id) -> x, y, angle",
     "Возвращает положение и угол тела."},
    {"physicsvelocity", "draw.physicsvelocity(id) -> vx, vy, omega",
     "Возвращает скорость тела."},
    {"physicssetposition", "draw.physicssetposition(id, x, y, [angle])",
     "Устанавливает положение тела."},
    {"physicssetvelocity", "draw.physicssetvelocity(id, vx, vy, [omega])",
     "Устанавливает скорость тела."},
    {"physicsapplyforce", "draw.physicsapplyforce(id, fx, fy)",
     "Добавляет силу к телу."},
    {"physicsapplyimpulse", "draw.physicsapplyimpulse(id, ix, iy)",
     "Применяет импульс к телу."},
    {"physicsfilter", "draw.physicsfilter(id, [category], [mask]) -> category, mask",
     "Проверяет или задаёт биты фильтрации столкновений."},
    {"physicsremove", "draw.physicsremove(id) -> boolean",
     "Удаляет тело и связанные соединения."},
    {"physicscontacts", "draw.physicscontacts() -> table",
     "Контакты последнего шага."},
    {"physicscallback", "draw.physicscallback([fn]) -> boolean",
     "Устанавливает или снимает функцию столкновений."},
    {"physicsjoint", "draw.physicsjoint(kind, a, b, [options]) -> id",
     "Соединение distance, spring или pin."},
    {"physicsjointremove", "draw.physicsjointremove(id) -> boolean",
     "Удаляет соединение."},
    {"physicsraycast", "draw.physicsraycast(x1, y1, x2, y2, [cat], [mask]) -> hit | nil",
     "Ближайшее попадание луча: id, x, y, nx, ny, t."},
    {"physicsbodycount", "draw.physicsbodycount() -> number",
     "Возвращает число тел."},
    {"physicsjointcount", "draw.physicsjointcount() -> number",
     "Возвращает число соединений."},
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
    physics_clear_state();
    free_canvases();
    free_tilemaps();
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
    {"gradient", l_gradient},
    {"triangle", l_triangle},
    {"text",    l_text},
    {"palette", l_palette},
    {"sprite",  l_sprite},
    {"spriteimage", l_spriteimage},
    {"font",    l_font},
    {"animation", l_animation},
    {"draw",    l_draw_sprite},
    {"canvas",  l_canvas},
    {"setcanvas", l_setcanvas},
    {"canvasclear", l_canvasclear},
    {"canvassize", l_canvassize},
    {"canvasremove", l_canvasremove},
    {"canvascount", l_canvascount},
    {"canvasdraw", l_canvasdraw},
    {"shader", l_shader},
    {"setshader", l_setshader},
    {"shaderparam", l_shaderparam},
    {"shaderremove", l_shaderremove},
    {"shadercount", l_shadercount},
    {"particle", l_particle},
    {"emit", l_emit},
    {"particleupdate", l_particleupdate},
    {"particledraw", l_particledraw},
    {"particleclear", l_particleclear},
    {"particlecount", l_particlecount},
    {"tween", l_tween},
    {"tweenvalue", l_tweenvalue},
    {"tweenupdate", l_tweenupdate},
    {"tweenremove", l_tweenremove},
    {"tweenclear", l_tweenclear},
    {"tweencount", l_tweencount},
    {"sample", l_sample},
    {"music", l_music},
    {"playsample", l_playsample},
    {"playmusic", l_playmusic},
    {"stopmusic", l_stopmusic},
    {"musicpause", l_musicpause},
    {"musicplaying", l_musicplaying},
    {"musicvolume", l_musicvolume},
    {"mastervolume", l_mastervolume},
    {"channelvolume", l_channelvolume},
    {"channelpan", l_channelpan},
    {"stopchannel", l_stopchannel},
    {"listener", l_listener},
    {"mixerremove", l_mixerremove},
    {"mixercount", l_mixercount},
    {"ttf", l_ttf},
    {"ttftext", l_ttftext},
    {"ttfsize", l_ttfsize},
    {"ttfremove", l_ttfremove},
    {"ttfcount", l_ttfcount},
    {"tilemap", l_tilemap},
    {"drawtilemap", l_drawtilemap},
    {"camera", l_camera},
    {"screentoworld", l_screentoworld},
    {"tilemapremove", l_tilemapremove},
    {"tilemapcount", l_tilemapcount},
    {"collision", l_collision},
    {"contactangle", l_contactangle},
    {"physicsworld", l_physicsworld},
    {"physicsclear", l_physicsclear},
    {"physicsbody", l_physicsbody},
    {"physicsstep", l_physicsstep},
    {"physicsdraw", l_physicsdraw},
    {"physicsposition", l_physicsposition},
    {"physicsvelocity", l_physicsvelocity},
    {"physicssetposition", l_physicssetposition},
    {"physicssetvelocity", l_physicssetvelocity},
    {"physicsapplyforce", l_physicsapplyforce},
    {"physicsapplyimpulse", l_physicsapplyimpulse},
    {"physicsfilter", l_physicsfilter},
    {"physicsremove", l_physicsremove},
    {"physicscontacts", l_physicscontacts},
    {"physicscallback", l_physicscallback},
    {"physicsjoint", l_physicsjoint},
    {"physicsjointremove", l_physicsjointremove},
    {"physicsraycast", l_physicsraycast},
    {"physicsbodycount", l_physicsbodycount},
    {"physicsjointcount", l_physicsjointcount},
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
