// Sound effects mixer: replaces the HMI SOS digital driver the game used.
// Samples are 8-bit unsigned mono PCM in game memory, played at 16 kHz (the
// rate the game initialised the HMI mixer with). Mixing happens on the main
// thread and is pushed to an SDL3 audio stream, keeping ~40 ms queued.
#include "platform.h"
#include <string.h>

#define VOICES 32
#define RATE 16000

typedef struct { int format, channels, freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
static struct {
    SDL_AudioStream *(*OpenAudioDeviceStream)(uint32_t, const SDL_AudioSpec *, void *, void *);
    bool (*ResumeAudioStreamDevice)(SDL_AudioStream *);
    bool (*PutAudioStreamData)(SDL_AudioStream *, const void *, int);
    int (*GetAudioStreamQueued)(SDL_AudioStream *);
} A;
static const char *A_names[] = { "SDL_OpenAudioDeviceStream", "SDL_ResumeAudioStreamDevice",
    "SDL_PutAudioStreamData", "SDL_GetAudioStreamQueued" };
static SDL_AudioStream *stream;

// pos and step are 16.16 fixed point; step = sample rate / mixer rate
typedef struct { bool on; uint32_t addr, len; uint64_t pos; uint32_t step; int vol; } Voice;   // vol 0..32767
static Voice voices[VOICES];
static uint32_t next_voice;

void sfx_init(void) {
    if (stream || !g_sdl || !sdl_load((void **)&A, A_names, 4)) return;
    SDL_AudioSpec spec = { 0x8010 /* S16 */, 1, RATE };
    stream = A.OpenAudioDeviceStream(0xFFFFFFFFu, &spec, NULL, NULL);
    if (stream) A.ResumeAudioStreamDevice(stream);
}

// Returns a handle (voice index). Like the HMI mixer, a new sample takes a
// free voice, or the oldest one if all are busy.
int sfx_start(uint32_t addr, uint32_t len, uint32_t rate, int vol) {
    int v = -1;
    for (int i = 0; i < VOICES; i++) if (!voices[i].on) { v = i; break; }
    if (v < 0) v = (int)(next_voice++ % VOICES);
    // the game set the HMI pitch to rate * 4.096 (= rate / 16000 in 16.16)
    uint32_t step = (uint32_t)((uint64_t)(rate ? rate : RATE) * 65536 / RATE);
    voices[v] = (Voice){ true, addr, len, 0, step, vol };
    return v;
}

bool sfx_playing(int h) { return h >= 0 && h < VOICES && voices[h].on; }
void sfx_stop(int h) { if (h >= 0 && h < VOICES) voices[h].on = false; }
void sfx_stop_all(void) { for (int i = 0; i < VOICES; i++) voices[i].on = false; }
void sfx_volume(int h, int vol) { if (h >= 0 && h < VOICES) voices[h].vol = vol; }

static void mix(int16_t *out, int n) {
    for (int k = 0; k < n; k++) {
        int32_t acc = 0;
        for (int i = 0; i < VOICES; i++) {
            Voice *v = &voices[i];
            if (!v->on) continue;
            uint32_t i0 = (uint32_t)(v->pos >> 16), fr = (uint32_t)(v->pos & 0xFFFF);
            int a = (int)rd8(v->addr + i0) - 128;
            int b = i0 + 1 < v->len ? (int)rd8(v->addr + i0 + 1) - 128 : a;
            int s = a + (int)(((int64_t)(b - a) * fr) >> 16);   // linear interpolation
            acc += s * v->vol >> 7;            // 8-bit sample * 15-bit volume -> ~16-bit
            v->pos += v->step;
            if ((v->pos >> 16) >= v->len) v->on = false;
        }
        if (acc > 32767) acc = 32767;
        if (acc < -32768) acc = -32768;
        out[k] = (int16_t)acc;
    }
}

// Called from the main timer path (io_tick).
void sfx_tick(void) {
    if (!stream) {
        // no audio device: advance voices in real time so "sample done" works
        static uint64_t last;
        uint64_t now = now_ns();
        if (last) {
            uint32_t n = (uint32_t)((now - last) * RATE / 1000000000ull);
            for (int i = 0; i < VOICES; i++)
                if (voices[i].on && ((voices[i].pos += (uint64_t)n * voices[i].step) >> 16) >= voices[i].len) voices[i].on = false;
        }
        last = now;
        return;
    }
    int16_t buf[256];
    int target = RATE * 2 / 25;   // 40 ms of 16-bit mono
    while (A.GetAudioStreamQueued(stream) < target) {
        mix(buf, 256);
        A.PutAudioStreamData(stream, buf, sizeof buf);
    }
}
