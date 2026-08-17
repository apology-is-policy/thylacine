/* SDL_thylacine — the GL backend for a tree with no Mesa headers.
 *
 * SDL_thylacineopengl.c includes <GL/osmesa.h>, which is not vendored: it is
 * fetched from the GCP builder into build/sysroot/include/ alongside the
 * 205 MB libOSMesa.a (usr/ports/mesa/README.md, "Fetching the GL link
 * artifacts"). A fresh checkout has neither, and build_sysroot RECREATES
 * build/sysroot/, so even a machine that once fetched them loses them to any
 * sysroot rebuild. Without this file that is a hard build failure for the
 * whole tree, not a missing GL — libSDL2.a stops being produced at all, and
 * with it every SDL program and the ramfs bake behind them.
 *
 * The weak-symbol arrangement in SDL_thylacineopengl.h already makes the
 * RASTERISER optional at link time. This makes the HEADERS optional at compile
 * time, on exactly the same terms and with exactly the same observable
 * behaviour: every hook reports "no OSMesa in this program" through
 * SDL_SetError, GL_CreateContext refuses, and THYLACINE_GL_Available() is
 * false. A GL-less libSDL2.a built here is byte-for-byte as useful to a non-GL
 * program as one built with the headers present, and it is honest to a GL one.
 *
 * The hooks are still WIRED (SDL_thylacinevideo.c installs them
 * unconditionally) for the reason recorded there: leaving GL_CreateContext
 * NULL makes SDL report a missing video DRIVER, when the truth is a missing
 * library. That reason survives verbatim here.
 *
 * This file is NOT a second implementation to keep in sync. It is selected by
 * build_sdl2 INSTEAD of SDL_thylacineopengl.c, never alongside it, and both
 * include SDL_thylacineopengl.h — so a signature change breaks whichever one
 * is being compiled, immediately.
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_THYLACINE

#include "SDL_video.h"
#include "../SDL_sysvideo.h"

#include "SDL_thylacineopengl.h"

/* The one message every hook here reduces to. Named once so the two builds
 * cannot drift on the wording an application actually prints. */
#define NOGL_WHY                                                    \
    "thylacine: no OSMesa in this program -- this libSDL2.a was "   \
    "built without the Mesa headers (see usr/ports/mesa/README.md)"

SDL_bool THYLACINE_GL_Available(void)
{
    return SDL_FALSE;
}

int THYLACINE_GL_LoadLibrary(_THIS, const char *path)
{
    (void)_this;
    (void)path;
    return SDL_SetError(NOGL_WHY);
}

void *THYLACINE_GL_GetProcAddress(_THIS, const char *proc)
{
    (void)_this;
    (void)proc;
    SDL_SetError(NOGL_WHY);
    return NULL;
}

void THYLACINE_GL_UnloadLibrary(_THIS)
{
    (void)_this;
}

SDL_GLContext THYLACINE_GL_CreateContext(_THIS, SDL_Window *window)
{
    (void)_this;
    (void)window;
    SDL_SetError(NOGL_WHY);
    return NULL;
}

int THYLACINE_GL_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
    (void)_this;
    (void)window;
    /* (NULL, NULL) means "unbind" and is the one call that can legitimately
     * arrive here: SDL issues it while tearing a window down, on a path that
     * does not care whether a context was ever created. Matching the real
     * backend's success return keeps that teardown quiet. */
    if (!context) {
        return 0;
    }
    return SDL_SetError(NOGL_WHY);
}

int THYLACINE_GL_SetSwapInterval(_THIS, int interval)
{
    (void)_this;
    (void)interval;
    return SDL_SetError(NOGL_WHY);
}

int THYLACINE_GL_GetSwapInterval(_THIS)
{
    (void)_this;
    return 0;
}

int THYLACINE_GL_SwapWindow(_THIS, SDL_Window *window)
{
    (void)_this;
    (void)window;
    return SDL_SetError(NOGL_WHY);
}

void THYLACINE_GL_DeleteContext(_THIS, SDL_GLContext context)
{
    (void)_this;
    (void)context;
}

#endif /* SDL_VIDEO_DRIVER_THYLACINE */
