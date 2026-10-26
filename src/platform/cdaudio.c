// CD audio: plays the raw audio tracks of the CD image (44.1 kHz, 16-bit
// stereo, 2352 bytes per frame) through an SDL3 audio stream.
#include "platform.h"

typedef struct { int format, channels, freq; } SDL_AudioSpec;
typedef struct SDL_AudioStream SDL_AudioStream;
static struct {
    SDL_AudioStream *(*OpenAudioDeviceStream)(uint32_t, const SDL_AudioSpec *, void *, void *);
    bool (*ResumeAudioStreamDevice)(SDL_AudioStream *);
    bool (*PutAudioStreamData)(SDL_AudioStream *, const void *, int);
    int (*GetAudioStreamQueued)(SDL_AudioStream *);
    bool (*ClearAudioStream)(SDL_AudioStream *);
} A;
static const char *A_names[] = { "SDL_OpenAudioDeviceStream", "SDL_ResumeAudioStreamDevice",
    "SDL_PutAudioStreamData", "SDL_GetAudioStreamQueued", "SDL_ClearAudioStream" };
static SDL_AudioStream *stream;
static bool playing;
static uint32_t fed, end;

void audio_init(void) {
    if (!g_sdl || !sdl_load((void **)&A, A_names, 5)) return;
    SDL_AudioSpec spec = { 0x8010, 2, 44100 };
    stream = A.OpenAudioDeviceStream(0xFFFFFFFFu, &spec, NULL, NULL);
    if (stream) A.ResumeAudioStreamDevice(stream);
}
static uint32_t queued_frames(void) { return stream ? (uint32_t)A.GetAudioStreamQueued(stream) / 2352 : 0; }
void cdaudio_play(uint32_t start, uint32_t frames) {
    if (stream) A.ClearAudioStream(stream);
    fed = start;
    end = start + frames;
    if (end > cd_leadout()) end = cd_leadout();
    playing = fed < end;
}
void cdaudio_stop(void) {
    if (!playing) return;
    fed -= queued_frames();
    if (stream) A.ClearAudioStream(stream);
    playing = false;
}
void cdaudio_resume(void) { if (fed < end) playing = true; }
bool cdaudio_playing(void) { return playing; }
uint32_t cdaudio_position(void) { uint32_t q = queued_frames(); return fed > q ? fed - q : 0; }
void cd_tick(void) {
    if (!playing) return;
    if (!stream) { fed++; if (fed >= end) playing = false; return; }
    uint8_t buf[2352];
    while (fed < end && queued_frames() < 8) {
        if (!cd_read_raw(fed, buf)) { fed = end; break; }
        A.PutAudioStreamData(stream, buf, sizeof buf);
        fed++;
    }
    if (fed >= end && queued_frames() == 0) playing = false;
}
