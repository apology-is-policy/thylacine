/* SDL_thylacine — the Tapestry video backend (G-7a). Design notes in
 * SDL_thylacinevideo.h; the wire client in thyla_tap.c.
 *
 * Single-slot present discipline: the framebuffer SDL hands the app IS
 * slot 0 of the mapped weave (zero-copy draw), and UpdateWindowFramebuffer
 * is one blocking tpresent write — the compositor consumes the pixels
 * inside that dispatch (the stage-0 synchronous engine), so the app never
 * draws a slot the compositor is still reading. No SDL-side shadow
 * surface, no copy.
 *
 * Resize: a size-changing TEV_CONFIGURE acks + reweaves (the §18.3
 * generation swap) in PumpEvents, then reports SDL_WINDOWEVENT_RESIZED;
 * SDL core invalidates the window surface and the app's next
 * SDL_GetWindowSurface returns the NEW slot-0 pointer. An app that keeps
 * presenting a stale surface pointer after a resize event faults its own
 * mapping (snare:segv, self-contained) — the standard SDL re-query
 * contract, stated here honestly.
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_THYLACINE

#include "SDL_video.h"
#include "../SDL_sysvideo.h"
#include "../SDL_pixels_c.h"
#include "../../events/SDL_events_c.h"
#include "../../events/SDL_mouse_c.h"

#include <time.h>

#include "SDL_thylacinevideo.h"
#include "SDL_thylacineevents_c.h"

#define THYLACINEVID_DRIVER_NAME "thylacine"

static int THYLACINE_VideoInit(_THIS);
static void THYLACINE_VideoQuit(_THIS);
static int THYLACINE_CreateWindow(_THIS, SDL_Window *window);
static void THYLACINE_DestroyWindow(_THIS, SDL_Window *window);
static int THYLACINE_CreateWindowFramebuffer(_THIS, SDL_Window *window,
                                             Uint32 *format, void **pixels,
                                             int *pitch);
static int THYLACINE_UpdateWindowFramebuffer(_THIS, SDL_Window *window,
                                             const SDL_Rect *rects,
                                             int numrects);
static void THYLACINE_DestroyWindowFramebuffer(_THIS, SDL_Window *window);

static void THYLACINE_DeleteDevice(SDL_VideoDevice *device)
{
    SDL_free(device->driverdata);
    SDL_free(device);
}

static SDL_VideoDevice *THYLACINE_CreateDevice(void)
{
    SDL_VideoDevice *device;
    SDL_VideoData *data;

    device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
    if (!device) {
        SDL_OutOfMemory();
        return NULL;
    }
    data = (SDL_VideoData *)SDL_calloc(1, sizeof(SDL_VideoData));
    if (!data) {
        SDL_free(device);
        SDL_OutOfMemory();
        return NULL;
    }
    device->driverdata = data;

    device->VideoInit = THYLACINE_VideoInit;
    device->VideoQuit = THYLACINE_VideoQuit;
    device->PumpEvents = THYLACINE_PumpEvents;
    device->CreateSDLWindow = THYLACINE_CreateWindow;
    device->DestroyWindow = THYLACINE_DestroyWindow;
    device->CreateWindowFramebuffer = THYLACINE_CreateWindowFramebuffer;
    device->UpdateWindowFramebuffer = THYLACINE_UpdateWindowFramebuffer;
    device->DestroyWindowFramebuffer = THYLACINE_DestroyWindowFramebuffer;

    device->free = THYLACINE_DeleteDevice;

    return device;
}

VideoBootStrap THYLACINE_bootstrap = {
    THYLACINEVID_DRIVER_NAME, "Thylacine tapestry video driver",
    THYLACINE_CreateDevice,
    NULL /* no ShowMessageBox implementation */
};

static int THYLACINE_SetRelativeMouseMode(SDL_bool enabled)
{
    /* Accepting relative mode keeps SDL core off its warp emulation (no
     * warpable cursor exists here); the event pump then delivers RELATIVE
     * motions computed from successive tablet positions. */
    (void)enabled;
    return 0;
}

static int THYLACINE_VideoInit(_THIS)
{
    SDL_DisplayMode mode;
    uint32_t w = 0, h = 0;

    SDL_GetMouse()->SetRelativeMouseMode = THYLACINE_SetRelativeMouseMode;

    /* The display dims come from the compositor's global ctl; failing to
     * reach /srv/tapestry fails the whole driver (SDL falls through to
     * the next bootstrap or errors out honestly). */
    if (thyla_tap_display_size(&w, &h) != 0 || w == 0 || h == 0) {
        return SDL_SetError("thylacine: /srv/tapestry unreachable");
    }

    SDL_zero(mode);
    /* The weave is little-endian 0xAARRGGBB words = SDL ARGB8888. */
    mode.format = SDL_PIXELFORMAT_ARGB8888;
    mode.w = (int)w;
    mode.h = (int)h;
    mode.refresh_rate = 60;
    mode.driverdata = NULL;
    if (SDL_AddBasicVideoDisplay(&mode) < 0) {
        return -1;
    }
    SDL_AddDisplayMode(&_this->displays[0], &mode);
    return 0;
}

static void THYLACINE_VideoQuit(_THIS)
{
}

static int THYLACINE_CreateWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_WindowData *wd;
    uint32_t w, h;

    if (vd->window) {
        return SDL_SetError("thylacine: only one window per process");
    }

    w = (uint32_t)(window->w > 0 ? window->w : 1);
    h = (uint32_t)(window->h > 0 ? window->h : 1);

    wd = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
    if (!wd) {
        return SDL_OutOfMemory();
    }
    if (thyla_tap_open(&wd->tap, w, h) != 0) {
        SDL_free(wd);
        return SDL_SetError("thylacine: surface create failed");
    }
    window->driverdata = wd;
    vd->window = window;

    if (THYLACINE_StartEventPump(window) != 0) {
        vd->window = NULL;
        window->driverdata = NULL;
        thyla_tap_close(&wd->tap);
        SDL_free(wd);
        return SDL_SetError("thylacine: event pump start failed");
    }

    /* The compositor decides real placement (placement-transparent);
     * report fullscreen-ish state honestly enough for games. */
    window->flags |= SDL_WINDOW_SHOWN;
    SDL_SetKeyboardFocus(window);
    SDL_SetMouseFocus(window);
    return 0;
}

static void THYLACINE_DestroyWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;

    if (!wd) {
        return;
    }
    /* Stop the pump FIRST: it RETIRES the surface (ctl "destroy") so the
     * parked event read EOFs and the thread joins (closing event_fd would
     * NOT cancel the parked read — #844; see THYLACINE_StopEventPump); then
     * the remaining fids drop the surface. */
    THYLACINE_StopEventPump(window);
    thyla_tap_close(&wd->tap);
    SDL_free(wd);
    window->driverdata = NULL;
    if (vd->window == window) {
        vd->window = NULL;
    }
}

static int THYLACINE_CreateWindowFramebuffer(_THIS, SDL_Window *window,
                                             Uint32 *format, void **pixels,
                                             int *pitch)
{
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;

    if (!wd) {
        return SDL_SetError("thylacine: no surface for window");
    }
    *format = SDL_PIXELFORMAT_ARGB8888;
    *pixels = thyla_tap_pixels(&wd->tap);
    *pitch = (int)wd->tap.stride;
    return 0;
}

static int THYLACINE_UpdateWindowFramebuffer(_THIS, SDL_Window *window,
                                             const SDL_Rect *rects,
                                             int numrects)
{
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;
    ThylaRect tr[63];
    int n = 0;

    if (!wd) {
        return SDL_SetError("thylacine: no surface for window");
    }
    if (numrects > 0 && numrects <= 63) {
        int i;
        for (i = 0; i < numrects; i++) {
            int x = rects[i].x, y = rects[i].y;
            int rw = rects[i].w, rh = rects[i].h;
            if (x < 0) { rw += x; x = 0; }
            if (y < 0) { rh += y; y = 0; }
            /* Clamp the extent to the surface BEFORE any additive compare —
             * an app-supplied w/h near INT_MAX would overflow `x + rw`
             * (signed UB) and skip the bounds guard (audit F4). The subtract
             * form cannot overflow (x is in [0, w)). */
            if (rw > (int)wd->tap.w - x) rw = (int)wd->tap.w - x;
            if (rh > (int)wd->tap.h - y) rh = (int)wd->tap.h - y;
            if (x >= (int)wd->tap.w || y >= (int)wd->tap.h || rw <= 0 || rh <= 0) {
                continue;
            }
            /* rw/rh already clamped to the surface above (overflow-safe). */
            tr[n].x = (uint32_t)x;
            tr[n].y = (uint32_t)y;
            tr[n].w = (uint32_t)rw;
            tr[n].h = (uint32_t)rh;
            n++;
        }
    }
    /* #51 frame pacing (default ON; SDL_THYLACINE_NOPACE=1 opts out --
     * benchmarks): wait for the compositor's next FRAME tick before the
     * present. A 60 Hz compositor can only SHOW 60 fps -- presents
     * beyond that overwrite un-composed pixels and spin a vCPU (the
     * uncapped timedemo's 600 fps + its 122-600 HVF variance). The pump
     * thread bumps frame_seq per TEV_FRAME (never the main thread --
     * this wait would starve PumpEvents and self-deadlock into
     * timeout-only pacing). ONE timed wait, 50 ms wall-clock bound (the
     * G-5 F1 lesson: never a wake-count bound): a degraded/frozen frame
     * clock (clock-rate ctl, test-mode) or a HIDDEN pane (visible-only
     * FRAME emission) degrades to ~20 fps timeout pacing -- background
     * throttling for free -- and teardown (the pump exits after the
     * retire, no further signals) is bounded by the same 50 ms. A
     * spurious wake presents one tick early: pacing slack, never a
     * correctness issue. */
    if (!wd->nopace && wd->pump_started) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 50 * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_mutex_lock(&wd->lock);
        if (wd->frame_seq == wd->presented_seq) {
            pthread_cond_timedwait(&wd->frame_cv, &wd->lock, &ts);
        }
        wd->presented_seq = wd->frame_seq;
        pthread_mutex_unlock(&wd->lock);
    }

    /* n == 0 (no rects, too many, or all clipped away) = full-surface. */
    if (thyla_tap_present(&wd->tap, tr, n) != 0) {
        return SDL_SetError("thylacine: present failed");
    }
    return 0;
}

static void THYLACINE_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
    /* The framebuffer IS the weave slot — owned by the surface, freed at
     * DestroyWindow. Nothing to do here. */
    (void)_this;
    (void)window;
}

#endif /* SDL_VIDEO_DRIVER_THYLACINE */
