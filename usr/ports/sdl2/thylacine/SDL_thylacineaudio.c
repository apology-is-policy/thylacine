/* SDL_thylacineaudio -- the Nocturne audio backend (N-2a-2, docs/NOCTURNE.md
 * section 6.5). SDL's audio thread hands us one converted period at a time;
 * we stream it to a private Nocturne voice over plain blocking file ops --
 * the byte-copy path (the Weft ring is N-2b), the analog of thyla_tap.c on
 * the video side.
 *
 * The connection IS the voice's lifetime. OpenDevice opens /srv/nocturne
 * directly (a fresh per-client connection, NOT joey's shared /dev/nocturne
 * mount) and mints a voice on it via nodes/new; nocturned owns that voice to
 * this connection, so CloseDevice -- or the app simply exiting -- tears the
 * connection down and reaps the voice (server.rs drop_conn_voices). No
 * explicit "remove" needed on the happy path.
 *
 * Pacing rides the sink, no timer. A write to a voice's audio file blocks
 * (nocturned parks the Twrite until the mixer drains room -- Plan 9's
 * blocking audio(3) write), so once the voice FIFO (64 KiB, ~340 ms) fills,
 * each PlayDevice returns at exactly the device drain rate. WaitDevice is
 * therefore the SDL no-op default; adding a delay on top would underrun.
 * The ~340 ms FIFO depth is the byte-copy path's latency ceiling; N-2b's
 * ring trims it.
 *
 * Nocturne dictates one format (S16LE stereo 48000 Hz, manual/41-audio.md);
 * OpenDevice forces it and lets SDL's core build the conversion stream from
 * whatever the app asked for. Init probes /srv/nocturne and declines the
 * driver when it is absent, so a soundless machine falls through to DUMMY.
 */
#include "../../SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_THYLACINE

#include "SDL_audio.h"
#include "../SDL_audio_c.h"
#include "SDL_thylacineaudio.h"

#include <thyla/syscall.h>

#define THYLACINEAUDIO_DRIVER_NAME "thylacine"

#define NOC_SRV     "/srv/nocturne"
#define NOC_SRV_LEN 13

/* nocturne's fixed device format (manual/41-audio.md). */
#define NOC_FREQ     48000
#define NOC_CHANNELS 2
#define NOC_FORMAT   AUDIO_S16LSB

/* Keep every wire write well under nocturned's 32 KiB msize; the audio
 * write is accepted in full (blocking), so this only bounds one Twrite. */
#define NOC_WRITE_CHUNK 8192u

/* Decimal parse of the voice id nodes/new hands back. Returns >=1, or -1. */
static long noc_parse_dec(const char *s, long n)
{
    long v = 0;
    int any = 0;
    const char *end = s + n;
    while (s < end && (*s == ' ' || *s == '\n' || *s == '\t')) {
        s++;
    }
    while (s < end && *s >= '0' && *s <= '9') {
        v = v * 10 + (long)(*s - '0');
        if (v > 0x7fffffffL) {
            return -1;
        }
        s++;
        any = 1;
    }
    return any ? v : -1;
}

/* Append `v` as decimal at `p`; returns the new write cursor. */
static char *noc_fmt_u32(char *p, unsigned v)
{
    char tmp[10];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *p++ = tmp[--n];
    }
    return p;
}

static Uint8 *THYLACINEAUDIO_GetDeviceBuf(_THIS)
{
    return _this->hidden->mixbuf;
}

static void THYLACINEAUDIO_PlayDevice(_THIS)
{
    struct SDL_PrivateAudioData *h = _this->hidden;
    const Uint8 *p = h->mixbuf;
    Uint32 left = h->mixlen;

    while (left > 0) {
        Uint32 chunk = left < NOC_WRITE_CHUNK ? left : NOC_WRITE_CHUNK;
        long w = t_write(h->audio_fd, p, chunk);
        if (w <= 0) {
            /* the voice/connection is gone (nocturned crash, forced teardown);
             * report it so SDL retires the device rather than spinning. */
            SDL_OpenedAudioDeviceDisconnected(_this);
            return;
        }
        p += w;
        left -= (Uint32)w;
    }
}

static void THYLACINEAUDIO_CloseDevice(_THIS)
{
    struct SDL_PrivateAudioData *h = _this->hidden;
    if (!h) {
        return;
    }
    if (h->audio_fd >= 0) {
        t_close(h->audio_fd);
    }
    if (h->root >= 0) {
        t_close(h->root); /* conn teardown reaps the minted voice */
    }
    SDL_free(h->mixbuf);
    SDL_free(h);
    _this->hidden = NULL;
}

static int THYLACINEAUDIO_OpenDevice(_THIS, const char *devname)
{
    struct SDL_PrivateAudioData *h;
    long root, mint_fd, audio_fd, id, n;
    char idbuf[16];
    char path[40];
    char *pp;

    /* nocturne dictates the format; force it and let SDL convert. */
    _this->spec.format = NOC_FORMAT;
    _this->spec.channels = NOC_CHANNELS;
    _this->spec.freq = NOC_FREQ;
    SDL_CalculateAudioSpec(&_this->spec);

    h = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*h));
    if (!h) {
        return SDL_OutOfMemory();
    }
    h->root = h->audio_fd = -1;

    /* A fresh per-client connection -- the voice reaps when we close it. */
    root = t_open(T_WALK_OPEN_FROM_ROOT, NOC_SRV, NOC_SRV_LEN, T_OREAD);
    if (root < 0) {
        SDL_free(h);
        return SDL_SetError("nocturne: could not connect /srv/nocturne");
    }

    /* Mint a voice: opening nodes/new mints, its read yields the decimal id. */
    mint_fd = t_open(root, "nodes/new", 9, T_OREAD);
    if (mint_fd < 0) {
        t_close(root);
        SDL_free(h);
        return SDL_SetError("nocturne: could not mint a voice");
    }
    n = t_read(mint_fd, idbuf, sizeof(idbuf));
    t_close(mint_fd);
    if (n <= 0 || (id = noc_parse_dec(idbuf, n)) <= 0) {
        t_close(root);
        SDL_free(h);
        return SDL_SetError("nocturne: bad voice id");
    }

    /* nodes/<id>/audio, resolved by walk from the session root fid. */
    pp = path;
    SDL_memcpy(pp, "nodes/", 6);
    pp += 6;
    pp = noc_fmt_u32(pp, (unsigned)id);
    SDL_memcpy(pp, "/audio", 6);
    pp += 6;
    audio_fd = t_open(root, path, (size_t)(pp - path), T_OWRITE);
    if (audio_fd < 0) {
        t_close(root);
        SDL_free(h);
        return SDL_SetError("nocturne: could not open the voice audio file");
    }

    h->mixlen = _this->spec.size;
    h->mixbuf = (Uint8 *)SDL_malloc(h->mixlen);
    if (!h->mixbuf) {
        t_close(audio_fd);
        t_close(root);
        SDL_free(h);
        return SDL_OutOfMemory();
    }
    SDL_memset(h->mixbuf, _this->spec.silence, h->mixlen);

    h->root = root;
    h->audio_fd = audio_fd;
    _this->hidden = h;
    return 0;
}

static SDL_bool THYLACINEAUDIO_Init(SDL_AudioDriverImpl *impl)
{
    /* Offer this driver only when nocturne is actually present, so a
     * soundless machine (THYLACINE_NO_AUDIO / no virtio-sound function)
     * falls through to the DUMMY driver rather than failing every open. */
    long probe = t_open(T_WALK_OPEN_FROM_ROOT, NOC_SRV, NOC_SRV_LEN, T_OREAD);
    if (probe < 0) {
        return SDL_FALSE;
    }
    t_close(probe);

    impl->OpenDevice = THYLACINEAUDIO_OpenDevice;
    impl->PlayDevice = THYLACINEAUDIO_PlayDevice;
    impl->GetDeviceBuf = THYLACINEAUDIO_GetDeviceBuf;
    impl->CloseDevice = THYLACINEAUDIO_CloseDevice;

    impl->OnlyHasDefaultOutputDevice = SDL_TRUE;
    /* Playback only at N-2a; capture ("ears") is Nocturne N-3. */

    return SDL_TRUE;
}

AudioBootStrap THYLACINEAUDIO_bootstrap = {
    THYLACINEAUDIO_DRIVER_NAME, "Thylacine Nocturne audio",
    THYLACINEAUDIO_Init, SDL_FALSE
};

#endif /* SDL_AUDIO_DRIVER_THYLACINE */
