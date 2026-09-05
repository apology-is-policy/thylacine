/* sdl-audio-probe -- the N-2a-2 witness (docs/NOCTURNE.md section 6.5).
 *
 * The first SDL AUDIO program on Thylacine: SDL_Init(AUDIO) resolves the
 * thylacine bootstrap (SDL_thylacineaudio.c), SDL_OpenAudioDevice mints a
 * private Nocturne voice over a fresh /srv/nocturne connection, and the SDL
 * audio thread streams the callback's PCM to it -- the WHOLE audio path
 * through stock SDL API, no driver internals touched. The callback fills a
 * 1 kHz + 2 kHz chord (both tones in one stereo buffer), so a wav capture
 * carries both frequencies at once: the mixing-witness verdict
 * (audio-verdict.py --chord) proving the SDL byte path delivers complex PCM.
 *
 * Run instead of /nocturne-probe under the `thylacine.sdlaudio` boot arg
 * (joey), so the wav is a clean SDL capture -- the two probes never share
 * one capture (the chord verdict's silent-tail check forbids a second tone).
 *
 * Output contract (greppable, the probe convention):
 *   "sdl-audio-probe: PASS driver=thylacine freq=F ch=C"   on success
 *   "sdl-audio-probe: FAIL <stage>: <SDL error>"           on any failure
 */
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#define RATE     48000
#define CHANNELS 2
#define SAMPLES  1024   /* the app's requested period (frames) */
#define PLAY_MS  1200   /* tone duration */
#define TAIL_MS  800    /* pause + drain: > the 64 KiB voice FIFO (~340 ms) */

/* One 1 kHz cycle at 48 kHz (48 samples), amplitude 0.25 full-scale. The
 * 1 kHz tone steps by 1, the 2 kHz tone by 2; their sum peaks at ~0.5, so
 * the chord never clips. */
static const Sint16 SINE48[48] = {
         0,   1069,   2120,   3135,   4096,   4987,   5793,   6499,
      7094,   7568,   7913,   8122,   8192,   8122,   7913,   7568,
      7094,   6499,   5793,   4987,   4096,   3135,   2120,   1069,
         0,  -1069,  -2120,  -3135,  -4096,  -4987,  -5793,  -6499,
     -7094,  -7568,  -7913,  -8122,  -8192,  -8122,  -7913,  -7568,
     -7094,  -6499,  -5793,  -4987,  -4096,  -3135,  -2120,  -1069,
};

static int idx1; /* 1 kHz phase (step 1) */
static int idx2; /* 2 kHz phase (step 2) */

static void SDLCALL fill(void *ud, Uint8 *stream, int len)
{
    Sint16 *s = (Sint16 *)stream;
    int frames = len / (CHANNELS * (int)sizeof(Sint16));
    (void)ud;
    for (int i = 0; i < frames; i++) {
        Sint16 v = (Sint16)(SINE48[idx1] + SINE48[idx2]);
        idx1 = (idx1 + 1) % 48;
        idx2 = (idx2 + 2) % 48;
        s[2 * i] = v;     /* L */
        s[2 * i + 1] = v; /* R */
    }
}

static int fail(const char *stage)
{
    printf("sdl-audio-probe: FAIL %s: %s\n", stage, SDL_GetError());
    return 1;
}

int main(int argc, char **argv)
{
    SDL_AudioSpec want, have;
    SDL_AudioDeviceID dev;
    const char *driver;

    (void)argc;
    (void)argv;

    if (SDL_Init(SDL_INIT_AUDIO) != 0) {
        return fail("init");
    }
    driver = SDL_GetCurrentAudioDriver();
    if (!driver || strcmp(driver, "thylacine") != 0) {
        printf("sdl-audio-probe: FAIL driver: got %s\n", driver ? driver : "(none)");
        SDL_Quit();
        return 1;
    }

    SDL_memset(&want, 0, sizeof(want));
    want.freq = RATE;
    want.format = AUDIO_S16LSB;
    want.channels = CHANNELS;
    want.samples = SAMPLES;
    want.callback = fill;

    /* allowed_changes = 0: SDL delivers our requested spec, converting if the
     * device differs. The thylacine backend forces 48 kHz S16 stereo -- what
     * we ask for here -- so this exercises the direct path. */
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (dev == 0) {
        SDL_Quit();
        return fail("open");
    }

    SDL_PauseAudioDevice(dev, 0); /* start playing */
    SDL_Delay(PLAY_MS);
    SDL_PauseAudioDevice(dev, 1); /* stop: the thread now feeds silence */
    SDL_Delay(TAIL_MS);           /* let the FIFO drain, leaving a silent tail */
    SDL_CloseAudioDevice(dev);    /* closes the conn -> the voice is reaped */

    printf("sdl-audio-probe: PASS driver=thylacine freq=%d ch=%d\n",
           have.freq, have.channels);
    SDL_Quit();
    return 0;
}
