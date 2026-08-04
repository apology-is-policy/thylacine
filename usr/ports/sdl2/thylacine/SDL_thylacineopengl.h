/* SDL_thylacine — the OpenGL context path (CL-7 step 2 / #138).
 *
 * SDL's GL layer is entirely function-pointer driven: every gl* call inside
 * SDL_video.c goes through SDL_GL_GetProcAddress, so SDL itself never names a
 * GL symbol. This backend supplies the nine driver hooks that turn that layer
 * into a working context, backed by Mesa's gallium OSMesa frontend running on
 * llvmpipe — which JIT-compiles shaders through the I-42 dual-mapped code
 * Burrow (CL-7b).
 *
 * WHY OSMesa AND NOT EGL: OSMesa renders into a caller-supplied memory buffer.
 * That buffer is slot 0 of the mapped weave, so a GL frame lands in the
 * compositor's pixels with no blit at all — see the format note in
 * SDL_thylacineopengl.c. EGL is structurally impossible here anyway (its
 * loader dlopen()s the driver; Thylacine has no dynamic loader).
 *
 * WHY THE OSMesa SYMBOLS ARE WEAK: libSDL2.a is linked by every SDL program,
 * GL or not, and libOSMesa.a + its 73 LLVM archives are ~365 MB of link input.
 * A hard reference would force every SDL program to carry the whole rasteriser
 * — pouch-hello-sdl would go from ~1 MB to ~70 MB, and a program with no
 * interest in GL could not link at all. Weak undefined references let the same
 * libSDL2.a serve both: a program that links libOSMesa.a gets a real context,
 * and one that does not gets a clean SDL_SetError from GL_LoadLibrary. Both
 * directions are exercised in the build (see build_sdl2's two probes).
 */
#ifndef SDL_thylacineopengl_h_
#define SDL_thylacineopengl_h_

#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_THYLACINE

#include "../SDL_sysvideo.h"

extern int THYLACINE_GL_LoadLibrary(_THIS, const char *path);
extern void *THYLACINE_GL_GetProcAddress(_THIS, const char *proc);
extern void THYLACINE_GL_UnloadLibrary(_THIS);
extern SDL_GLContext THYLACINE_GL_CreateContext(_THIS, SDL_Window *window);
extern int THYLACINE_GL_MakeCurrent(_THIS, SDL_Window *window,
                                    SDL_GLContext context);
extern int THYLACINE_GL_SetSwapInterval(_THIS, int interval);
extern int THYLACINE_GL_GetSwapInterval(_THIS);
extern int THYLACINE_GL_SwapWindow(_THIS, SDL_Window *window);
extern void THYLACINE_GL_DeleteContext(_THIS, SDL_GLContext context);

/* True when the OSMesa rasteriser was linked into this program. The whole
 * GL path is inert without it, and GL_LoadLibrary is where that becomes a
 * legible error rather than a null-call fault. */
extern SDL_bool THYLACINE_GL_Available(void);

#endif /* SDL_VIDEO_DRIVER_THYLACINE */

#endif /* SDL_thylacineopengl_h_ */
