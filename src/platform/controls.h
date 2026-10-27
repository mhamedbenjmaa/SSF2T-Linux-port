// Settings file sf2t.ini: game paths, video, player controls (controls.c).
#pragma once
#include <stdint.h>

enum { A_UP, A_DOWN, A_LEFT, A_RIGHT, A_LP, A_MP, A_HP, A_LK, A_MK, A_HK, A_START, A_BACK, A_COUNT };
enum { P_LT = 15, P_RT = 16, P_COUNT = 17 };   // pad inputs: SDL buttons 0..14, then triggers
#define CTL_SDL_KEYS 232

extern const uint16_t ctl_game_key[2][A_COUNT];  // game scancode per player action
extern uint16_t ctl_key[CTL_SDL_KEYS];           // SDL scancode -> game scancode (0 = none)
extern uint16_t ctl_pad[2][P_COUNT];             // pad input -> mask of actions
extern int ctl_deadzone, ctl_trigger, ctl_first_pad;            // raw axis values; deadzone -1 = stick off

#include <stdbool.h>
extern bool ctl_filter;                          // xBR upscaling on

extern char cfg_game[1024], cfg_cd[1024];        // [paths] (empty = not set)

void controls_load(const char *save_dir);          // reads sf2t.ini (creates it if missing)
void config_save_paths(const char *game, const char *cd);   // rewrites its [paths] section
