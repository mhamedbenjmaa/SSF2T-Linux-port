// Display, input and timer through SDL3, loaded at runtime (no development
// files needed to build). Renders VGA mode 13h stretched to 4:3, optionally
// through the xBR 4x filter (filter.c).
#include "platform.h"
#include "controls.h"
#include "filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#define LIBSDL "SDL3.dll"
static void *lib_open(const char *n) { return (void *)LoadLibraryA(n); }
static void *lib_sym(void *l, const char *s) { return (void *)GetProcAddress((HMODULE)l, s); }
#else
#include <dlfcn.h>
#include <unistd.h>
#define LIBSDL "libSDL3.so.0"
static void *lib_open(const char *n) { return dlopen(n, RTLD_NOW | RTLD_GLOBAL); }
static void *lib_sym(void *l, const char *s) { return dlsym(l, s); }
#endif

#ifdef _WIN32
static CRITICAL_SECTION mtx;
static void mtx_init(void) { InitializeCriticalSection(&mtx); }
static void lock(void) { EnterCriticalSection(&mtx); }
static void unlock(void) { LeaveCriticalSection(&mtx); }
#else
#include <pthread.h>
static pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
static void mtx_init(void) {}
static void lock(void) { pthread_mutex_lock(&mtx); }
static void unlock(void) { pthread_mutex_unlock(&mtx); }
#endif

uint8_t g_vga_pal[768];
int g_video_mode = 3;
void *g_sdl;

uint64_t now_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
#endif
}
void sleep_ns(uint64_t ns) {
#ifdef _WIN32
    Sleep((DWORD)(ns / 1000000 ? ns / 1000000 : 1));
#else
    struct timespec ts = { (time_t)(ns / 1000000000ull), (long)(ns % 1000000000ull) };
    nanosleep(&ts, NULL);
#endif
}

bool sdl_load(void **fns, const char **names, int n) {
    for (int i = 0; i < n; i++) {
        fns[i] = lib_sym(g_sdl, names[i]);
        if (!fns[i]) { fprintf(stderr, "SDL3: missing %s\n", names[i]); return false; }
    }
    return true;
}

typedef struct SDL_Window SDL_Window;
typedef struct SDL_Renderer SDL_Renderer;
typedef struct SDL_Texture SDL_Texture;
typedef union { uint32_t type; uint8_t pad[128]; } SDL_Event;
typedef uint32_t (*SDL_TimerCallback)(void *, uint32_t, uint32_t);

static struct {
    bool (*Init)(uint32_t);
    SDL_Window *(*CreateWindow)(const char *, int, int, uint64_t);
    SDL_Renderer *(*CreateRenderer)(SDL_Window *, const char *);
    SDL_Texture *(*CreateTexture)(SDL_Renderer *, uint32_t, int, int, int);
    bool (*SetTextureScaleMode)(SDL_Texture *, int);
    bool (*UpdateTexture)(SDL_Texture *, const void *, const void *, int);
    bool (*RenderClear)(SDL_Renderer *);
    bool (*RenderTexture)(SDL_Renderer *, SDL_Texture *, const void *, const void *);
    bool (*RenderPresent)(SDL_Renderer *);
    bool (*PollEvent)(SDL_Event *);
    bool (*SetRenderLogicalPresentation)(SDL_Renderer *, int, int, int);
    bool (*SetWindowFullscreen)(SDL_Window *, bool);
    bool (*HideCursor)(void);
    const char *(*GetError)(void);
    uint32_t (*AddTimer)(uint32_t, SDL_TimerCallback, void *);
    void *(*OpenGamepad)(uint32_t);
    void (*CloseGamepad)(void *);
    const char *(*GetGamepadName)(void *);
} S;
static const char *S_names[] = { "SDL_Init", "SDL_CreateWindow", "SDL_CreateRenderer", "SDL_CreateTexture",
    "SDL_SetTextureScaleMode", "SDL_UpdateTexture", "SDL_RenderClear", "SDL_RenderTexture", "SDL_RenderPresent",
    "SDL_PollEvent", "SDL_SetRenderLogicalPresentation", "SDL_SetWindowFullscreen", "SDL_HideCursor",
    "SDL_GetError", "SDL_AddTimer", "SDL_OpenGamepad", "SDL_CloseGamepad", "SDL_GetGamepadName" };

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex, *tex_big;   // 320x200 as is, 1280x800 filtered
static bool quit, fullscreen;
static uint32_t frame[320 * 200];

// 1 kHz heartbeat: the game code checks it at loop back-edges
static uint32_t heartbeat(void *u, uint32_t id, uint32_t interval) {
    (void)u; (void)id;
    g_irq_tick++;
    return interval;
}

bool video_init(void) {
    g_sdl = lib_open(LIBSDL);
    if (!g_sdl) { fprintf(stderr, "cannot load %s\n", LIBSDL); return false; }
    if (!sdl_load((void **)&S, S_names, sizeof S_names / sizeof *S_names)) return false;
    if (!S.Init(0x20u | 0x10u | 0x2000u)) { fprintf(stderr, "SDL_Init: %s\n", S.GetError()); return false; }
    win = S.CreateWindow("Super Street Fighter II Turbo", 960, 720, 0x20ull);
    if (!win) return false;
    ren = S.CreateRenderer(win, NULL);
    if (!ren) return false;
    S.SetRenderLogicalPresentation(ren, 640, 480, 2);
    tex = S.CreateTexture(ren, 0x16161804u, 1, 320, 200);
    S.SetTextureScaleMode(tex, 0);
    tex_big = S.CreateTexture(ren, 0x16161804u, 1, 1280, 800);
    if (!tex_big) ctl_filter = false;
    else S.SetTextureScaleMode(tex_big, 1);    // linear: smooth 1280x800 -> window size
    S.HideCursor();
    S.AddTimer(1, heartbeat, NULL);
    return true;
}

void video_set_mode(int mode) {
    LOG("video mode %02x\n", mode);
    g_video_mode = mode;
    wr8(0x449, (uint8_t)mode);
    if (mode == 0x13) memset(G(0xA0000), 0, 65536);
}

// Shared between the game thread (writer) and the main thread (display):
// the 256-colour picture and its palette.
static uint8_t shared_idx[320 * 200];
static uint32_t shared_pal[256];
static volatile int frame_seq;

// Game thread, ~70 times per second: copy VGA memory and palette.
void video_present(void) {
    if (!ren) return;
    lock();
    if (g_video_mode == 0x13) {
        for (int i = 0; i < 256; i++) {
            uint32_t r = g_vga_pal[i * 3], g = g_vga_pal[i * 3 + 1], b = g_vga_pal[i * 3 + 2];
            shared_pal[i] = ((r << 2) | (r >> 4)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 2) | (b >> 4));
        }
        memcpy(shared_idx, G(0xA0000), sizeof shared_idx);
    } else {
        memset(shared_idx, 0, sizeof shared_idx);
        memset(shared_pal, 0, sizeof shared_pal);
    }
    frame_seq++;
    unlock();
}

static const uint16_t sc_map[232] = {
    [4] = 0x1E, [5] = 0x30, [6] = 0x2E, [7] = 0x20, [8] = 0x12, [9] = 0x21, [10] = 0x22,
    [11] = 0x23, [12] = 0x17, [13] = 0x24, [14] = 0x25, [15] = 0x26, [16] = 0x32, [17] = 0x31,
    [18] = 0x18, [19] = 0x19, [20] = 0x10, [21] = 0x13, [22] = 0x1F, [23] = 0x14, [24] = 0x16,
    [25] = 0x2F, [26] = 0x11, [27] = 0x2D, [28] = 0x15, [29] = 0x2C,
    [30] = 0x02, [31] = 0x03, [32] = 0x04, [33] = 0x05, [34] = 0x06, [35] = 0x07, [36] = 0x08,
    [37] = 0x09, [38] = 0x0A, [39] = 0x0B,
    [40] = 0x1C, [41] = 0x01, [42] = 0x0E, [43] = 0x0F, [44] = 0x39, [45] = 0x0C, [46] = 0x0D,
    [47] = 0x1A, [48] = 0x1B, [49] = 0x2B, [51] = 0x27, [52] = 0x28, [53] = 0x29, [54] = 0x33,
    [55] = 0x34, [56] = 0x35, [57] = 0x3A,
    [58] = 0x3B, [59] = 0x3C, [60] = 0x3D, [61] = 0x3E, [62] = 0x3F, [63] = 0x40, [64] = 0x41,
    [65] = 0x42, [66] = 0x43, [67] = 0x44, [68] = 0x57, [69] = 0x58,
    [71] = 0x46, [73] = 0x152, [74] = 0x147, [75] = 0x149, [76] = 0x153, [77] = 0x14F,
    [78] = 0x151, [79] = 0x14D, [80] = 0x14B, [81] = 0x150, [82] = 0x148, [83] = 0x45,
    [84] = 0x135, [85] = 0x37, [86] = 0x4A, [87] = 0x4E, [88] = 0x11C, [89] = 0x4F, [90] = 0x50,
    [91] = 0x51, [92] = 0x4B, [93] = 0x4C, [94] = 0x4D, [95] = 0x47, [96] = 0x48, [97] = 0x49,
    [98] = 0x52, [99] = 0x53,
    [224] = 0x1D, [225] = 0x2A, [226] = 0x38, [228] = 0x11D, [229] = 0x36, [230] = 0x138,
};
static bool alt_down;
static uint8_t ev_ring[512];
static int ev_head, ev_tail;
static volatile bool quit_flag;

static void ev_push(uint8_t sc) {           // main thread, under lock
    int n = (ev_tail + 1) & 511;
    if (n != ev_head) { ev_ring[ev_tail] = sc; ev_tail = n; }
}

// ---- gamepads: each pad drives one player's keys (bindings: controls.c) ----
typedef struct {
    uint32_t id;
    void *pad;
    bool held[A_COUNT];        // key state sent to the game
    bool in[P_COUNT];          // buttons and triggers
    int16_t lx, ly;            // left stick
} Pad;
static Pad pads[2];

static void key_send(uint16_t code, bool down) {   // main thread, takes the lock
    lock();
    if (code & 0x100) ev_push(0xE0);
    ev_push((uint8_t)((code & 0x7F) | (down ? 0 : 0x80)));
    unlock();
}

static void pad_update(int p) {
    Pad *d = &pads[p];
    uint16_t want = 0;
    for (int i = 0; i < P_COUNT; i++) if (d->in[i]) want |= ctl_pad[p][i];
    int dead = ctl_deadzone;
    if (dead >= 0) {
        if (d->lx < -dead) want |= 1u << A_LEFT;
        if (d->lx > dead) want |= 1u << A_RIGHT;
        if (d->ly < -dead) want |= 1u << A_UP;
        if (d->ly > dead) want |= 1u << A_DOWN;
    }
    for (int k = 0; k < A_COUNT; k++) {
        bool w = (want >> k) & 1;
        if (w != d->held[k]) { d->held[k] = w; key_send(ctl_game_key[p][k], w); }
    }
}

static int pad_slot(uint32_t id) {
    for (int p = 0; p < 2; p++) if (pads[p].pad && pads[p].id == id) return p;
    return -1;
}

static void pad_event(const SDL_Event *ev) {
    uint32_t id;
    memcpy(&id, ev->pad + 16, 4);
    if (ev->type == 0x653) {                       // gamepad added
        if (pad_slot(id) >= 0) return;
        for (int i = 0; i < 2; i++) {
            int p = i ^ ctl_first_pad;             // first_pad = 2: player 2 first
            if (!pads[p].pad) {
                pads[p] = (Pad){ .id = id, .pad = S.OpenGamepad(id) };
                if (pads[p].pad) fprintf(stderr, "gamepad %d: %s\n", p + 1, S.GetGamepadName(pads[p].pad));
                return;
            }
        }
        return;
    }
    int p = pad_slot(id);
    if (p < 0) return;
    Pad *d = &pads[p];
    if (ev->type == 0x654) {                       // removed: release its keys
        memset(d->in, 0, sizeof d->in);
        d->lx = d->ly = 0;
        pad_update(p);
        S.CloseGamepad(d->pad);
        d->pad = NULL;
        return;
    }
    if (ev->type == 0x651 || ev->type == 0x652) {  // button down / up
        uint8_t b = ev->pad[20];
        if (b < 15) { d->in[b] = ev->type == 0x651; pad_update(p); }
        return;
    }
    if (ev->type == 0x650) {                       // axis motion
        int16_t v;
        memcpy(&v, ev->pad + 24, 2);
        switch (ev->pad[20]) {
        case 0: d->lx = v; break;
        case 1: d->ly = v; break;
        case 4: d->in[P_LT] = v > ctl_trigger; break;
        case 5: d->in[P_RT] = v > ctl_trigger; break;
        default: return;
        }
        pad_update(p);
    }
}

// Main thread: SDL events -> scancodes.
static void pump_events(void) {
    SDL_Event ev;
    while (S.PollEvent(&ev)) {
        if (ev.type == 0x100) { quit_flag = true; continue; }
        if (ev.type >= 0x650 && ev.type <= 0x654) { pad_event(&ev); continue; }
        if (ev.type != 0x300 && ev.type != 0x301) continue;
        uint32_t sc;
        memcpy(&sc, ev.pad + 24, 4);
        bool down = ev.pad[36] != 0, repeat = ev.pad[37] != 0;
        if (sc == 226 || sc == 230) alt_down = down;
        if (down && sc == 40 && alt_down) { fullscreen = !fullscreen; S.SetWindowFullscreen(win, fullscreen); continue; }
        if (sc == 9 && alt_down) { if (down && tex_big) { ctl_filter = !ctl_filter; frame_seq++; } continue; }   // Alt+F: filter on/off
        if (repeat || sc >= 232) continue;
        uint16_t code = ctl_key[sc] ? ctl_key[sc] : sc_map[sc];
        if (!code) continue;
        lock();
        if (code & 0x100) ev_push(0xE0);
        ev_push((uint8_t)((code & 0x7F) | (down ? 0 : 0x80)));
        unlock();
    }
}

// Game thread: hand the pending scancodes to the keyboard controller.
void video_poll_events(void) {
    lock();
    while (ev_head != ev_tail) { kbd_push(ev_ring[ev_head]); ev_head = (ev_head + 1) & 511; }
    unlock();
}

bool video_quit_requested(void) { return quit_flag; }

// Main thread: display loop. Presenting may wait for the monitor's vertical
// sync; that only delays this thread, never the game.
// debug: SSF2T_SHOTS=dir saves the displayed picture every 2 seconds
static void save_shot(const uint32_t *px, int w, int h) {
    static const char *shots;
    static int shot_n, shot_tick;
    if (!shots) { shots = getenv("SSF2T_SHOTS"); if (!shots) shots = ""; }
    if (!*shots || ++shot_tick % 140) return;
    char path[512];
    snprintf(path, sizeof path, "%s/shot%03d.ppm", shots, shot_n++);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint8_t c[3] = { (uint8_t)(px[i] >> 16), (uint8_t)(px[i] >> 8), (uint8_t)px[i] }; fwrite(c, 1, 3, f); }
    fclose(f);
}

void video_main_loop(void) {
    static uint8_t idx[320 * 200];
    static uint32_t pal[256], big[1280 * 800];
    int shown = -1;
    while (!quit_flag) {
        pump_events();
        if (frame_seq != shown) {
            lock();
            memcpy(idx, shared_idx, sizeof idx);
            memcpy(pal, shared_pal, sizeof pal);
            shown = frame_seq;
            unlock();
            SDL_Texture *t;
            if (ctl_filter) {
                filter_xbr4(idx, pal, big);
                S.UpdateTexture(tex_big, NULL, big, 1280 * 4);
                save_shot(big, 1280, 800);
                t = tex_big;
            } else {
                for (int i = 0; i < 320 * 200; i++) frame[i] = pal[idx[i]];
                S.UpdateTexture(tex, NULL, frame, 320 * 4);
                save_shot(frame, 320, 200);
                t = tex;
            }
            S.RenderClear(ren);
            S.RenderTexture(ren, t, NULL, NULL);
            S.RenderPresent(ren);
        } else sleep_ns(2000000);
    }
}

void video_threads_init(void) { mtx_init(); }
