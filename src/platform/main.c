// sf2t: Super Street Fighter II Turbo (DOS, 1995) ported to Windows and Linux.
// The game code is the recompiled original (src/recomp), with hand-written
// replacements (src/game); this file sets up the machine it expects.
#include "platform.h"
#include "controls.h"
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif
#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

int g_verbose;

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "sf2t: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

// Load the original executable's sections at their addresses: the code is
// recompiled, but the data (tables, strings, initial values) comes from it.
static void load_exe(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)sz);
    if (!d || fread(d, 1, (size_t)sz, f) != (size_t)sz) die("cannot read %s", path);
    fclose(f);
    uint32_t pe;
    memcpy(&pe, d + 0x3C, 4);
    uint16_t nsec, optsz;
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&optsz, d + pe + 20, 2);
    uint32_t base, hdrsize;
    memcpy(&base, d + pe + 24 + 28, 4);
    memcpy(&hdrsize, d + pe + 24 + 60, 4);
    memcpy(G(base), d, hdrsize);
    uint8_t *sec = d + pe + 24 + optsz;
    for (int i = 0; i < nsec; i++, sec += 40) {
        uint32_t vsize, va, rawsz, rawptr;
        memcpy(&vsize, sec + 8, 4); memcpy(&va, sec + 12, 4);
        memcpy(&rawsz, sec + 16, 4); memcpy(&rawptr, sec + 20, 4);
        uint32_t n = (rawsz < vsize || vsize == 0) ? rawsz : vsize;
        if (rawptr + n > (uint32_t)sz) n = (uint32_t)sz > rawptr ? (uint32_t)sz - rawptr : 0;
        memcpy(G(base + va), d + rawptr, n);
    }
    free(d);
}

void video_main_loop(void);
void video_threads_init(void);
#ifdef _WIN32
#include <windows.h>
static DWORD WINAPI game_thread(LPVOID p) { (void)p; F_110000(); exit(0); return 0; }
static void start_game_thread(void) {
    video_threads_init();
    HANDLE h = CreateThread(NULL, 1 << 20, game_thread, NULL, 0, NULL);
    if (!h) die("cannot start the game thread");
}
#else
#include <pthread.h>
static void *game_thread(void *p) { (void)p; F_110000(); exit(0); return NULL; }
static void start_game_thread(void) {
    video_threads_init();
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8 << 20);
    if (pthread_create(&t, &a, game_thread, NULL) != 0) die("cannot start the game thread");
}
#endif

static void usage(void) {
    fprintf(stderr,
        "usage: sf2t [--game DIR] [--cd FILE.cue] [--hires|--lowres] [-v]\n"
        "  --game DIR    installed game folder (the one with SF2TURBO.EXE)\n"
        "  --cd FILE     CD image .cue, for the music\n"
        "                Both are remembered in the settings file once given.\n"
        "                Also: SSF2T_GAME / SSF2T_CD environment variables.\n"
        "  --hires       smooth the picture (xBR 4x filter; Alt+F switches in game)\n"
        "  --lowres      original pixels (default)\n");
    exit(1);
}

static bool file_exists(const char *p) {
    FILE *f = fopen(p, "rb");
    if (f) fclose(f);
    return f != NULL;
}

// Absolute form of a path, so the saved settings work from any folder.
static void absolute(char *p, size_t n) {
#ifdef _WIN32
    char tmp[PATH_MAX];
    if (_fullpath(tmp, p, sizeof tmp)) snprintf(p, n, "%s", tmp);
#else
    char tmp[PATH_MAX];
    if (realpath(p, tmp)) snprintf(p, n, "%s", tmp);
#endif
}

int main(int argc, char **argv) {
    int hires = -1;              // -1: as set in sf2t.ini
    char game_dir[PATH_MAX] = "", cue[PATH_MAX] = "", save_dir[PATH_MAX], args[256] = "";
    bool game_arg = false, cd_arg = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-v")) g_verbose++;
        else if (!strcmp(argv[i], "--hires")) hires = 1;
        else if (!strcmp(argv[i], "--lowres")) hires = 0;
        else if ((!strcmp(argv[i], "--game") || !strcmp(argv[i], "-g")) && i + 1 < argc) { snprintf(game_dir, sizeof game_dir, "%s", argv[++i]); game_arg = true; }
        else if ((!strcmp(argv[i], "--cd") || !strcmp(argv[i], "-c")) && i + 1 < argc) { snprintf(cue, sizeof cue, "%s", argv[++i]); cd_arg = true; }
        else if (argv[i][0] == '-') usage();
        else { size_t l = strlen(args); snprintf(args + l, sizeof args - l, "%s%s", l ? " " : "", argv[i]); }
    }

    // settings folder: saves, high scores, sf2t.ini
#ifdef _WIN32
    const char *appdata = getenv("APPDATA");
    snprintf(save_dir, sizeof save_dir, "%s/ssf2t", appdata ? appdata : ".");
#else
    const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(save_dir, sizeof save_dir, "%s/ssf2t", xdg);
    else snprintf(save_dir, sizeof save_dir, "%s/.local/share/ssf2t", home ? home : ".");
#endif
    controls_load(save_dir);
    if (hires >= 0) ctl_filter = hires;

    // game paths: command line, then environment, then sf2t.ini
    const char *env;
    if (!game_arg && (env = getenv("SSF2T_GAME")) && *env) snprintf(game_dir, sizeof game_dir, "%s", env);
    if (!*game_dir) snprintf(game_dir, sizeof game_dir, "%s", cfg_game);
    if (!cd_arg && (env = getenv("SSF2T_CD")) && *env) snprintf(cue, sizeof cue, "%s", env);
    if (!*cue && !cd_arg) snprintf(cue, sizeof cue, "%s", cfg_cd);
    if (!*game_dir)
        die("the game folder is not set.\n"
            "  Run once: sf2t --game /path/to/game --cd /path/to/SF2.cue\n"
            "  (the folder with SF2TURBO.EXE; the .cue is the CD image, for the music)");
    char exe[PATH_MAX + 32];
    snprintf(exe, sizeof exe, "%s/SF2TURBO.EXE", game_dir);
    if (!file_exists(exe)) die("no SF2TURBO.EXE in %s\n  Give the installed game folder with --game DIR", game_dir);
    if (*cue && !file_exists(cue)) die("CD image %s not found (--cd FILE.cue)", cue);
    absolute(game_dir, sizeof game_dir);
    if (*cue) absolute(cue, sizeof cue);
    if ((game_arg || cd_arg) && (strcmp(game_dir, cfg_game) || strcmp(cue, cfg_cd))) {
        config_save_paths(game_dir, cue);
        fprintf(stderr, "sf2t: game folder saved in %s/sf2t.ini\n", save_dir);
    }
    if (!*cue) fprintf(stderr, "sf2t: no CD image set (--cd FILE.cue): no music\n");

    mem_init();
    load_exe(exe);
    win32_init(args);
    fs_init(game_dir, cue, save_dir);
    sys_init();
    io_init();
    if (!video_init()) die("cannot open the game window (SDL3 missing?)");
    audio_init();
    fpu_init();

    // stack and thread information block
    uint32_t stack_size = 0x60000, stack = mem_alloc(stack_size, 0x1000), top = stack + stack_size;
    wr32(g_tib + 0x00, 0xFFFFFFFFu);
    wr32(g_tib + 0x04, top);
    wr32(g_tib + 0x08, stack);
    wr32(g_tib + 0x18, g_tib);
    R.esp = top - 16;
    G_FLAGS.cf = 0;
    PUSH32(0);             // return address of the entry point (never used)

    // The game runs on its own thread; this thread shows frames and reads
    // input, so waiting for the monitor never stalls the game.
    start_game_thread();
    video_main_loop();
    return 0;
}
