// Platform layer of the port: what the game asked from DOS, the DOS extender,
// Windows (kernel32 subset) and the PC hardware, implemented on top of the C
// library and SDL3.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "../recomp/recomp.h"

#define GAME_SIZE  0x10000000u      // game address space (256 MB)
#define THUNK_BASE 0x000F0000u      // strings and tables in the "BIOS" area

extern int g_verbose;
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) do { if (g_verbose) logf_(__VA_ARGS__); } while (0)
void die(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));

static inline uint8_t  rd8(uint32_t a)  { return M8(a); }
static inline uint16_t rd16(uint32_t a) { return M16(a); }
static inline uint32_t rd32(uint32_t a) { return M32(a); }
static inline void wr8(uint32_t a, uint8_t v)   { M8(a) = v; }
static inline void wr16(uint32_t a, uint16_t v) { M16(a) = v; }
static inline void wr32(uint32_t a, uint32_t v) { M32(a) = v; }
static inline void *G(uint32_t a) { return g_mem + a; }

// register helpers for system calls
static inline void set_ax(uint16_t v) { SET16(R.eax, v); }
static inline void set_al(uint8_t v) { SET8L(R.eax, v); }
static inline void set_cf(bool on) { G_FLAGS.cf = on; }

// memory.c
void     mem_init(void);
uint32_t mem_alloc(uint32_t size, uint32_t align);
void     mem_free(uint32_t addr);
uint32_t mem_largest_free(void);
uint32_t dosmem_alloc(uint32_t paragraphs, uint32_t *max_avail);
void     dosmem_free(uint16_t seg);

// sys.c: descriptors, interrupts, DPMI/DOS/BIOS services
extern uint16_t sel_code, sel_data, sel_tib;
extern uint32_t g_tib;
uint16_t ldt_alloc(int count);
void     ldt_free(uint16_t sel);
bool     ldt_set_base(uint16_t sel, uint32_t base);
bool     ldt_set_limit(uint16_t sel, uint32_t limit);
uint32_t ldt_get_base(uint16_t sel);
void     sys_init(void);
void     irq_raise(int irq);
extern bool g_vif;

// io.c: PIC, PIT, keyboard, VGA, joystick
void io_init(void);
void io_tick(void);
void kbd_push(uint8_t sc);
double pit_hz(void);

// video.c (SDL3)
extern uint8_t g_vga_pal[768];
extern int g_video_mode;
extern void *g_sdl;
bool video_init(void);
void video_set_mode(int mode);
void video_present(void);
void video_poll_events(void);
bool video_quit_requested(void);
bool sdl_load(void **fns, const char **names, int n);
uint64_t now_ns(void);
void sleep_ns(uint64_t ns);

// win32api.c
void win32_init(const char *cmdline);

// fs.c
void fs_init(const char *game_dir, const char *cue_path, const char *save_dir);
void mkdirs_for(const char *file);   // create the parent folders of a path
int  fs_open(const char *dospath, int access, int disposition);
int  fs_read(int h, void *buf, uint32_t n);
int  fs_write(int h, const void *buf, uint32_t n);
int64_t fs_seek(int h, int64_t off, int whence);
void fs_close(int h);
bool fs_delete(const char *dospath);
int  fs_attributes(const char *dospath);
typedef struct { char name[260]; uint32_t size; int attr; uint32_t mtime; } FsFind;
int  fs_find_first(const char *pattern, FsFind *out);
bool fs_find_next(int sh, FsFind *out);
void fs_find_close(int sh);
bool fs_cd_present(void);
int  cd_track_count(void);
bool cd_track_info(int track, uint32_t *start_lba, uint32_t *len_frames, bool *audio);
uint32_t cd_leadout(void);
bool cd_read_raw(uint32_t lba, uint8_t *out2352);
bool cd_read_cooked(uint32_t lba, uint8_t *out2048);

// cdaudio.c
void audio_init(void);
void cd_tick(void);
void cdaudio_play(uint32_t start_lba, uint32_t frames);
void cdaudio_stop(void);
void cdaudio_resume(void);
bool cdaudio_playing(void);
uint32_t cdaudio_position(void);
