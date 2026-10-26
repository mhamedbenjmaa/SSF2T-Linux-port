// Settings file sf2t.ini in the save folder (written with the defaults on
// first run): where the game is installed, video options, and the players'
// keyboard and gamepad bindings.
//
// Every binding ends up as one of the game's own default keys (README):
// the game still reads a PC keyboard, so a bound key or pad button simply
// presses that player's default key for the action.
#include "platform.h"
#include "controls.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

// Game scancodes of each player's default keys; 0x100 = E0 prefix.
// P1 W A S D + T Y U / G H J, P2 P ; L ' + Insert Home PgUp / Delete End PgDn.
const uint16_t ctl_game_key[2][A_COUNT] = {
    { 0x11, 0x1F, 0x1E, 0x20, 0x14, 0x15, 0x16, 0x22, 0x23, 0x24, 0x1C, 0x01 },
    { 0x19, 0x27, 0x26, 0x28, 0x152, 0x147, 0x149, 0x153, 0x14F, 0x151, 0x1C, 0x01 },
};

uint16_t ctl_key[CTL_SDL_KEYS];
uint16_t ctl_pad[2][P_COUNT];
int ctl_deadzone = 16000, ctl_trigger = 12000;
bool ctl_filter = false;
char cfg_game[1024], cfg_cd[1024];
static char ini_path[1100];

static const char *action_names[A_COUNT] = {
    "up", "down", "left", "right", "light_punch", "medium_punch", "heavy_punch",
    "light_kick", "medium_kick", "heavy_kick", "start", "back" };

// SDL3 gamepad buttons 0..14, then the two triggers
static const char *pad_names[P_COUNT] = {
    "a", "b", "x", "y", "back", "guide", "start", "l3", "r3", "lb", "rb",
    "up", "down", "left", "right", "lt", "rt" };

static const struct { const char *name; uint8_t sc; } key_names[] = {
    { "enter", 40 }, { "escape", 41 }, { "backspace", 42 }, { "tab", 43 }, { "space", 44 },
    { "minus", 45 }, { "equals", 46 }, { "leftbracket", 47 }, { "rightbracket", 48 },
    { "backslash", 49 }, { "semicolon", 51 }, { "apostrophe", 52 }, { "grave", 53 },
    { "comma", 54 }, { "period", 55 }, { "slash", 56 },
    { "insert", 73 }, { "home", 74 }, { "pageup", 75 }, { "delete", 76 }, { "end", 77 },
    { "pagedown", 78 }, { "right", 79 }, { "left", 80 }, { "down", 81 }, { "up", 82 },
    { "kp_divide", 84 }, { "kp_multiply", 85 }, { "kp_minus", 86 }, { "kp_plus", 87 },
    { "kp_enter", 88 }, { "kp_period", 99 },
    { "lctrl", 224 }, { "lshift", 225 }, { "lalt", 226 }, { "rctrl", 228 }, { "rshift", 229 },
    { "ralt", 230 },
};

static int key_from_name(const char *s) {
    size_t n = strlen(s);
    if (n == 1 && s[0] >= 'a' && s[0] <= 'z') return 4 + s[0] - 'a';
    if (n == 1 && s[0] >= '1' && s[0] <= '9') return 30 + s[0] - '1';
    if (n == 1 && s[0] == '0') return 39;
    if (s[0] == 'f' && n >= 2 && n <= 3) { int f = atoi(s + 1); if (f >= 1 && f <= 12) return 57 + f; }
    if (!strncmp(s, "kp", 2) && n == 3 && isdigit((unsigned char)s[2])) return s[2] == '0' ? 98 : 88 + s[2] - '0';
    for (size_t i = 0; i < sizeof key_names / sizeof *key_names; i++)
        if (!strcmp(s, key_names[i].name)) return key_names[i].sc;
    return -1;
}

static int pad_from_name(const char *s) {
    for (int i = 0; i < P_COUNT; i++) if (!strcmp(s, pad_names[i])) return i;
    return -1;
}

static const char default_file[] =
"# Super Street Fighter II Turbo - settings\n"
"#\n"
"# key.<action> = keyboard keys, pad.<action> = gamepad inputs.\n"
"# Several inputs per action: separate them with commas (pad.heavy_kick = rt, r3).\n"
"# Leave a value empty to unbind.\n"
"#\n"
"# Keys: a-z 0-9 f1-f12 kp0-kp9 enter escape space tab backspace up down left right\n"
"#   insert delete home end pageup pagedown lshift rshift lctrl rctrl lalt ralt\n"
"#   minus equals comma period slash semicolon apostrophe grave backslash\n"
"#   leftbracket rightbracket kp_enter kp_plus kp_minus kp_multiply kp_divide kp_period\n"
"# Pad: a b x y lb rb lt rt start back guide l3 r3 up down left right\n"
"#   (Xbox names; on PlayStation a=cross b=circle x=square y=triangle lb=L1 rb=R1)\n"
"#\n"
"# The game's own keys (W A S D, T Y U, G H J...) always keep working, so\n"
"# the keyboard bindings below add keys rather than replace them.\n"
"\n"
"[video]\n"
"# xbr = smooth the picture (xBR 4x filter), none = original pixels.\n"
"# sf2t --hires / --lowres override this; Alt+F switches in game.\n"
"filter = none\n"
"\n"
"[general]\n"
"# left stick: percentage of the travel before a direction counts (0 = stick off)\n"
"stick_deadzone = 50\n"
"# triggers (lt, rt): percentage of the travel before they count as pressed\n"
"trigger_threshold = 40\n"
"\n"
"[player1]\n"
"key.up           = w\n"
"key.down         = s\n"
"key.left         = a\n"
"key.right        = d\n"
"key.light_punch  = t\n"
"key.medium_punch = y\n"
"key.heavy_punch  = u\n"
"key.light_kick   = g\n"
"key.medium_kick  = h\n"
"key.heavy_kick   = j\n"
"pad.up           = up\n"
"pad.down         = down\n"
"pad.left         = left\n"
"pad.right        = right\n"
"pad.light_punch  = x\n"
"pad.medium_punch = y\n"
"pad.heavy_punch  = rb\n"
"pad.light_kick   = a\n"
"pad.medium_kick  = b\n"
"pad.heavy_kick   = rt\n"
"pad.start        = start\n"
"pad.back         = back\n"
"\n"
"[player2]\n"
"key.up           = p\n"
"key.down         = semicolon\n"
"key.left         = l\n"
"key.right        = apostrophe\n"
"key.light_punch  = insert\n"
"key.medium_punch = home\n"
"key.heavy_punch  = pageup\n"
"key.light_kick   = delete\n"
"key.medium_kick  = end\n"
"key.heavy_kick   = pagedown\n"
"pad.up           = up\n"
"pad.down         = down\n"
"pad.left         = left\n"
"pad.right        = right\n"
"pad.light_punch  = x\n"
"pad.medium_punch = y\n"
"pad.heavy_punch  = rb\n"
"pad.light_kick   = a\n"
"pad.medium_kick  = b\n"
"pad.heavy_kick   = rt\n"
"pad.start        = start\n"
"pad.back         = back\n"
"\n"
"[paths]\n"
"# installed game folder (the one with SF2TURBO.EXE), and the CD image .cue\n"
"# for the music. sf2t --game DIR --cd FILE.cue fills them in.\n"
"# game = /path/to/SSF2T\n"
"# cd   = /path/to/SF2.cue\n";

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static void parse(FILE *f, const char *path) {
    char line[512];
    int player = -1, ln = 0;
    bool paths = false;
    while (fgets(line, sizeof line, f)) {
        ln++;
        char *h = strchr(line, '#');
        if (h) *h = 0;
        char *s = trim(line);
        if (!*s) continue;
        if (*s == '[') {
            for (char *c = s; *c; c++) *c = (char)tolower((unsigned char)*c);
            paths = !strcmp(s, "[paths]");
            player = !strcmp(s, "[player1]") ? 0 : !strcmp(s, "[player2]") ? 1 : -1;   // [general], [video]: -1
            continue;
        }
        char *eq = strchr(s, '=');
        if (!eq) { fprintf(stderr, "%s:%d: expected name = value\n", path, ln); continue; }
        *eq = 0;
        char *name = trim(s), *val = trim(eq + 1);
        for (char *c = name; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (paths) {                   // paths keep their case
            if (!strcmp(name, "game")) snprintf(cfg_game, sizeof cfg_game, "%s", val);
            else if (!strcmp(name, "cd")) snprintf(cfg_cd, sizeof cfg_cd, "%s", val);
            else fprintf(stderr, "%s:%d: unknown setting '%s'\n", path, ln, name);
            continue;
        }
        for (char *c = val; *c; c++) *c = (char)tolower((unsigned char)*c);
        if (player < 0) {
            int pct = atoi(val);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
            if (!strcmp(name, "filter")) { ctl_filter = strcmp(val, "none") != 0; continue; }
            if (!strcmp(name, "stick_deadzone")) ctl_deadzone = pct ? pct * 32767 / 100 : -1;
            else if (!strcmp(name, "trigger_threshold")) ctl_trigger = pct * 32767 / 100;
            else fprintf(stderr, "%s:%d: unknown setting '%s'\n", path, ln, name);
            continue;
        }
        bool is_key = !strncmp(name, "key.", 4), is_pad = !strncmp(name, "pad.", 4);
        int a = -1;
        for (int i = 0; i < A_COUNT && (is_key || is_pad); i++) if (!strcmp(name + 4, action_names[i])) a = i;
        if (a < 0) { fprintf(stderr, "%s:%d: unknown action '%s'\n", path, ln, name); continue; }
        if (is_pad)                    // the file replaces the default pad binding
            for (int i = 0; i < P_COUNT; i++) ctl_pad[player][i] &= (uint16_t)~(1u << a);
        for (char *tok = strtok(val, ","); tok; tok = strtok(NULL, ",")) {
            tok = trim(tok);
            if (!*tok) continue;
            if (is_key) {
                int sc = key_from_name(tok);
                if (sc < 0) fprintf(stderr, "%s:%d: unknown key '%s'\n", path, ln, tok);
                else ctl_key[sc] = ctl_game_key[player][a];
            } else {
                int b = pad_from_name(tok);
                if (b < 0) fprintf(stderr, "%s:%d: unknown pad input '%s'\n", path, ln, tok);
                else ctl_pad[player][b] |= (uint16_t)(1u << a);
            }
        }
    }
}

void controls_load(const char *save_dir) {
    // built-in defaults (same as the default file), used if it can't be read
    static const int8_t def_pad[A_COUNT] = { 11, 12, 13, 14, 2, 3, 10, 0, 1, 16, 6, 4 };
    for (int p = 0; p < 2; p++)
        for (int a = 0; a < A_COUNT; a++) ctl_pad[p][def_pad[a]] |= (uint16_t)(1u << a);

    snprintf(ini_path, sizeof ini_path, "%s/sf2t.ini", save_dir);
    mkdirs_for(ini_path);
    FILE *f = fopen(ini_path, "r");
    if (!f) {
        char old[1100];                // earlier name of the file
        snprintf(old, sizeof old, "%s/controls.ini", save_dir);
        if (rename(old, ini_path) == 0) f = fopen(ini_path, "r");
    }
    if (!f) {
        FILE *w = fopen(ini_path, "w");
        if (w) { fputs(default_file, w); fclose(w); fprintf(stderr, "sf2t: wrote %s\n", ini_path); }
        return;
    }
    parse(f, ini_path);
    fclose(f);
}

// Replace the [paths] section (it is kept at the end of the file).
void config_save_paths(const char *game, const char *cd) {
    char *rest = NULL;
    size_t n = 0;
    FILE *f = fopen(ini_path, "r");
    if (f) {
        char line[1200];
        bool skip = false;
        while (fgets(line, sizeof line, f)) {
            char *t = line;
            while (isspace((unsigned char)*t)) t++;
            if (*t == '[') skip = !strncasecmp(t, "[paths]", 7);
            if (skip) continue;
            size_t l = strlen(line);
            char *r = realloc(rest, n + l + 1);
            if (!r) break;
            rest = r;
            memcpy(rest + n, line, l);
            n += l;
        }
        fclose(f);
    }
    while (n && (rest[n - 1] == '\n' || rest[n - 1] == ' ')) n--;   // trailing blank lines
    FILE *w = fopen(ini_path, "w");
    if (!w) { free(rest); return; }
    if (n) { fwrite(rest, 1, n, w); fputs("\n\n", w); }
    fprintf(w, "[paths]\n# installed game folder (with SF2TURBO.EXE) and CD image .cue (music)\n");
    fprintf(w, "game = %s\n", game);
    if (cd && *cd) fprintf(w, "cd   = %s\n", cd); else fputs("# cd = /path/to/SF2.cue\n", w);
    fclose(w);
    free(rest);
}
