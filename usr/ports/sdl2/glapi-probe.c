/* The #109 deliverable, as a compile-and-link assertion.
 *
 * LLVM-DESIGN section 9 step 2 promises "stock SDL-GL programs recompile".
 * That is a claim about the SDL we ship, and the only honest way to check
 * it is to be one: include what a stock SDL-GL program includes, call the
 * SDL_GL_* entry points it calls, and link against libSDL2.a.
 *
 * This never runs -- there is no GL driver behind it yet (SDL_GL_LoadLibrary
 * returns SDL_DllNotSupported until the OSMesa hooks land). It is a
 * TRANSLATION + LINK assertion, which is exactly the half #109 delivers.
 *
 * The call set is taken from the real consumer, TyrQuake's vid_sgl.c, not
 * invented: SetAttribute / CreateContext / MakeCurrent / DeleteContext /
 * GetProcAddress / SetSwapInterval / SwapWindow.
 */
#include "SDL.h"

/* Assert the SWITCH, not just the headers.
 *
 * Learned by probing this probe: with SDL_VIDEO_OPENGL turned back OFF, an
 * earlier version of this file still compiled and linked clean, and the
 * build happily printed "a stock SDL-GL program compiles + links". Two
 * things make that possible -- the install step copies include/*.h
 * unconditionally, so the GL headers are in the sysroot either way, and
 * SDL_video.c defines all 20 SDL_GL_* entry points unconditionally (the
 * !SDL_VIDEO_OPENGL bodies just SDL_SetError and return). So neither the
 * header presence nor the symbol set discriminates the config flip; only
 * the macro does. Without this #error the probe would guard half of #109
 * while claiming to guard all of it. */
#if !defined(SDL_VIDEO_OPENGL)
#error "SDL_VIDEO_OPENGL is not defined -- #109's config flip was reverted. \
See usr/ports/sdl2/SDL_config.h and third_party/SDL2/PRUNE-MANIFEST.md."
#endif

#include "SDL_opengl.h"

/* Touch a GL 1.1 core type + enum (SDL_opengl.h) and an extension token
 * (SDL_opengl_glext.h) -- the two headers #109 un-pruned. Losing either
 * one must break this translation unit, or the probe is not a probe. */
static GLenum g_core_enum = GL_TRIANGLES;
static GLint  g_ext_token = GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE;

int main(void)
{
    SDL_Window *win;
    SDL_GLContext ctx;
    void *proc;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return 1;
    }

    /* The attribute set vid_sgl.c requests. */
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, 32);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

    win = SDL_CreateWindow("glapi-probe", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, 320, 240,
                           SDL_WINDOW_OPENGL);
    if (!win) {
        SDL_Quit();
        return 2;
    }

    ctx = SDL_GL_CreateContext(win);
    if (ctx) {
        SDL_GL_MakeCurrent(win, ctx);
        SDL_GL_SetSwapInterval(0);
        proc = SDL_GL_GetProcAddress("glClear");
        (void)proc;
        SDL_GL_SwapWindow(win);
        SDL_GL_DeleteContext(ctx);
    }

    SDL_DestroyWindow(win);
    SDL_Quit();
    return (int)(g_core_enum + (GLenum)g_ext_token) & 0;
}
