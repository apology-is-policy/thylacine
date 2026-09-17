/* SDL_thylacineaudio -- the Nocturne audio backend (N-2a-2). Design notes
 * in SDL_thylacineaudio.c. Staged into src/audio/thylacine/ by build_sdl2.
 */
#include "../../SDL_internal.h"

#ifndef SDL_thylacineaudio_h_
#define SDL_thylacineaudio_h_

#include "../SDL_sysaudio.h"

/* Hidden "this" pointer for the audio functions. SDL_sysaudio.h defines
 * _THIS for its own struct then #undefs it, so every driver header
 * re-defines it here (the SDL driver-header idiom -- cf. SDL_dummyaudio.h). */
#define _THIS SDL_AudioDevice *_this

/* Per-open device state: one direct /srv/nocturne connection + the one
 * voice minted on it. Closing `root` reaps the voice (conn-scoped
 * lifetime), so an SDL app's sound stops the instant it exits. */
struct SDL_PrivateAudioData
{
    long root;     /* the /srv/nocturne session fid (open = a fresh conn) */
    long audio_fd; /* nodes/<id>/audio, opened OWRITE */
    Uint8 *mixbuf; /* the period buffer SDL fills; == spec.size bytes */
    Uint32 mixlen; /* spec.size */
};

#endif /* SDL_thylacineaudio_h_ */
