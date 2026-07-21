/* SDL_thylacine — the Tapestry video backend (G-7a; TAPESTRY.md §9/§18.9).
 *
 * The §9 mapping, realized:
 *   CreateWindowFramebuffer  -> thyla_tap_open (surface create + weave map)
 *   UpdateWindowFramebuffer  -> a synchronous tpresent write (the Rwrite is
 *                               the completion — tear-free by construction)
 *   PumpEvents               -> a pthread pump blocking on the event fid,
 *                               drained + translated on the SDL thread
 *
 * This file sits OUTSIDE the vendored tree (usr/ports/sdl2/thylacine/);
 * build_sdl2() copies it into the build-dir tree at src/video/thylacine/,
 * so the relative includes mirror the dummy driver's.
 */
#ifndef SDL_thylacinevideo_h_
#define SDL_thylacinevideo_h_

#include "../../SDL_internal.h"
#include "../SDL_sysvideo.h"

#include <pthread.h>

#include "thyla_tap.h"

/* The pump ring: bounded, mutex-protected, filled by the pump thread,
 * drained by PumpEvents on the SDL thread. tapestryd bounds its own
 * per-surface queue independently; a full ring here drops the NEWEST
 * record (a bounded-input posture, never a block). */
#define THYLACINE_EVQ_CAP 256

typedef struct SDL_WindowData
{
    ThylaTap tap;
    pthread_t pump;
    int pump_started;
    pthread_mutex_t lock;
    ThylaEvent q[THYLACINE_EVQ_CAP];
    int q_head;
    int q_len;
} SDL_WindowData;

typedef struct SDL_VideoData
{
    /* The single window this backend supports at a time (the compositor
     * gives an SDL app one pane; a second SDL_CreateWindow fails). */
    SDL_Window *window;
} SDL_VideoData;

#endif /* SDL_thylacinevideo_h_ */
