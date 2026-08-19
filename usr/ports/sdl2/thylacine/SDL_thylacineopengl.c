/* SDL_thylacine — the OpenGL context path (CL-7 step 2 / #138).
 * Design notes in SDL_thylacineopengl.h.
 */
#include "../../SDL_internal.h"

#ifdef SDL_VIDEO_DRIVER_THYLACINE

#include "SDL_video.h"
#include "SDL_cpuinfo.h"   /* SDL_GetCPUCount, for the LP_NUM_THREADS default */
#include "../SDL_sysvideo.h"

#include "SDL_thylacinevideo.h"
#include "SDL_thylacineopengl.h"

/* GL/osmesa.h pulls in GL/gl.h, which shares the __gl_h_ include guard with
 * SDL_opengl.h — the two are mutually exclusive by construction. This TU
 * takes the Mesa side and never includes SDL_opengl.h; the SDL core and the
 * application take the SDL side. That is the same arrangement TyrQuake's
 * vid_sgl.c uses, and it is why nothing here can also speak SDL's GL enums. */
#include <GL/osmesa.h>

/* The corvus clearance walk (usr/lib/thylajit). Header-only and self-contained
 * — it reaches corvus with raw `svc` for the two syscalls musl does not carry,
 * so it pulls in no library SDL does not already have. */
#include "thyla_capjit.h"

/* The rasteriser is optional at link time (see the header). A weak undefined
 * reference resolves to 0 when libOSMesa.a is absent, which is what lets one
 * libSDL2.a serve both GL and non-GL programs.
 *
 * Redeclaring after the header is what applies the attribute — osmesa.h
 * declares these normally, and a weak redeclaration of an already-declared
 * function is honoured by clang. */
extern __attribute__((weak)) OSMesaContext
OSMesaCreateContextExt(GLenum format, GLint depthBits, GLint stencilBits,
                       GLint accumBits, OSMesaContext sharelist);
extern __attribute__((weak)) void OSMesaDestroyContext(OSMesaContext ctx);
extern __attribute__((weak)) GLboolean
OSMesaMakeCurrent(OSMesaContext ctx, void *buffer, GLenum type,
                  GLsizei width, GLsizei height);
extern __attribute__((weak)) void OSMesaPixelStore(GLint pname, GLint value);
extern __attribute__((weak)) OSMESAproc OSMesaGetProcAddress(const char *fn);
extern __attribute__((weak)) void glFinish(void);
extern __attribute__((weak)) void glFlush(void);

/* Warp-4 (fork patch 0007): negotiate/withdraw the direct present. Weak
 * like the rest -- absent on a pre-0007 libOSMesa.a, and the negotiation
 * simply never happens (the readback path is the shipping behavior). */
extern __attribute__((weak)) int
OSMesaThylacineDirect(OSMesaContext ctx, unsigned surface_id,
                      unsigned *ctx_pub_out);
extern __attribute__((weak)) void OSMesaThylacineDirectOff(OSMesaContext ctx);

typedef struct THYLACINE_GLContext
{
    OSMesaContext osmesa;
    /* What OSMesaMakeCurrent was last handed. A TEV_CONFIGURE reweave swaps
     * the weave generation and MOVES map_va (TAPESTRY.md §18.3), so a context
     * bound to the old mapping would render into freed pages. Recording the
     * binding here — rather than reacting to the resize event — means the
     * re-bind is driven by the state that actually changed, so it is correct
     * no matter which layer noticed the resize first. */
    uint64_t bound_va;
    uint32_t bound_w, bound_h;
    SDL_Window *window;
    /* Warp-4: the direct present is active -- the compositor displays
     * the GL color resource itself (fullscreen: SET_SCANOUT of the 3D
     * resource; windowed: server-side readback + compose), the OSMesa
     * readback into the weave is suppressed, and the swap needs only a
     * glFlush (ordering is the controlq's FIFO), not a glFinish. */
    SDL_bool direct;
} THYLACINE_GLContext;

SDL_bool THYLACINE_GL_Available(void)
{
    /* Every entry point this backend calls, checked as one: a partial link is
     * not a configuration anyone should get a half-working context out of. */
    return (OSMesaCreateContextExt && OSMesaDestroyContext &&
            OSMesaMakeCurrent && OSMesaPixelStore && OSMesaGetProcAddress &&
            glFinish)
               ? SDL_TRUE
               : SDL_FALSE;
}

int THYLACINE_GL_LoadLibrary(_THIS, const char *path)
{
    (void)_this;
    /* There is no dynamic loader on Thylacine, so a named library can only
     * ever be a caller mistake — refuse it rather than silently ignoring it
     * and handing back a context from some other implementation. */
    if (path) {
        return SDL_SetError("thylacine: no dynamic GL loading (OSMesa is "
                            "statically linked); pass NULL");
    }
    if (!THYLACINE_GL_Available()) {
        return SDL_SetError("thylacine: no OSMesa in this program -- link "
                            "libOSMesa.a (see usr/ports/mesa/README.md)");
    }
    return 0;
}

void *THYLACINE_GL_GetProcAddress(_THIS, const char *proc)
{
    (void)_this;
    if (!OSMesaGetProcAddress) {
        SDL_SetError("thylacine: no OSMesa in this program");
        return NULL;
    }
    return (void *)OSMesaGetProcAddress(proc);
}

void THYLACINE_GL_UnloadLibrary(_THIS)
{
    /* Statically linked: nothing to unload. */
    (void)_this;
}

/* Bind ctx to the window's CURRENT weave slot. Split out because both
 * MakeCurrent and the swap path's reweave recovery need exactly this. */
static int gl_bind(THYLACINE_GLContext *ctx, SDL_WindowData *wd)
{
    if (!OSMesaMakeCurrent(ctx->osmesa, thyla_tap_pixels(&wd->tap),
                           GL_UNSIGNED_BYTE, (GLsizei)wd->tap.w,
                           (GLsizei)wd->tap.h)) {
        return SDL_SetError("thylacine: OSMesaMakeCurrent failed");
    }
    /* Top-down raster. OSMesa defaults to Y_UP=1 (GL's bottom-left origin);
     * the weave — like every framebuffer — is top-down, so without this every
     * frame is presented vertically mirrored. It is context state, so it must
     * be set AFTER the context is current, and again after any re-bind. */
    OSMesaPixelStore(OSMESA_Y_UP, 0);

    ctx->bound_va = wd->tap.map_va;
    ctx->bound_w = wd->tap.w;
    ctx->bound_h = wd->tap.h;

    /* Warp-4: negotiate the direct present, on EVERY bind -- a reweave
     * reallocates the framebuffer, and the standing consent names one
     * BO, so it must be re-issued for the new one (both handshake
     * halves are idempotent server-side). The fork's export refuses on
     * llvmpipe (driver-name gate), so this costs a plain llvmpipe bind
     * one cheap failed call. BOTH halves must land; a half-negotiated
     * state suppresses the readback with no display source, which is a
     * frozen pane -- so any failure explicitly restores the readback. */
    ctx->direct = SDL_FALSE;
    if (OSMesaThylacineDirect) {
        unsigned ctx_pub = 0;
        if (OSMesaThylacineDirect(ctx->osmesa, wd->tap.id, &ctx_pub) == 0) {
            if (thyla_tap_glsrc(&wd->tap, ctx_pub) == 0) {
                ctx->direct = SDL_TRUE;
            } else {
                if (OSMesaThylacineDirectOff) {
                    OSMesaThylacineDirectOff(ctx->osmesa);
                }
                (void)thyla_tap_glsrc(&wd->tap, 0);
            }
        }
    }
    return 0;
}

SDL_GLContext THYLACINE_GL_CreateContext(_THIS, SDL_Window *window)
{
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;
    THYLACINE_GLContext *ctx;

    if (!THYLACINE_GL_Available()) {
        SDL_SetError("thylacine: no OSMesa in this program");
        return NULL;
    }
    if (!wd) {
        SDL_SetError("thylacine: no surface for window");
        return NULL;
    }

    /* CAP_JIT, before anything reaches the rasteriser.
     *
     * llvmpipe IS a JIT: it compiles rasteriser variants at draw time and
     * executes them in this process, which on Thylacine requires CAP_JIT
     * (I-42) — an ELEVATION-ONLY capability, stripped at every fork, so no
     * parent can hand it down and a program must walk the corvus clearance
     * path ITSELF.
     *
     * It happens HERE, in the platform layer, rather than in each program,
     * because "stock SDL-GL programs recompile" is the entire delivery shape
     * of LLVM-DESIGN §9 step 2. TyrQuake's vid_sgl.c is ordinary SDL-GL code
     * that has never heard of Thylacine; a rule that every GL app must first
     * speak a capability protocol would make that claim false and put a
     * Thylacine-shaped patch in every future port. The application already
     * declared its intent by asking for a GL context — on this platform that
     * request IS a request to JIT, which is exactly the sort of fact a
     * platform abstraction layer exists to carry.
     *
     * This grants nothing. SDL only ASKS; corvus decides, against the calling
     * principal's own eligibility, and refuses cleanly if the user has no jit
     * clearance. A program that acquired the capability itself (gl-sdl-prove
     * does, to print which form won) finds this a no-op, and vice versa.
     *
     * After the Available() check, so a program linking no rasteriser never
     * opens a corvus connection at all; before the allocation, so a refusal
     * has nothing to unwind. */
    if (thyla_acquire_cap_jit("sdl-gl") != 0) {
        SDL_SetError("thylacine: CAP_JIT refused — llvmpipe cannot JIT in "
                     "this process, so there is no rasteriser to bind");
        return NULL;
    }

    /* llvmpipe sizes its rasteriser worker pool from Mesa's
     * util_get_cpu_caps()->nr_cpus, and on this platform that is a hardcoded
     * 1: the POSIX_LITE tier of u_cpu_detect.c carries no sysconf arm (the
     * shipped u_cpu_detect.c.o has no sysconf reference at all — #150), so
     * the pool never spawned and an -smp 4 guest rasterised on one core.
     * Until the Mesa port grows that arm, seed Mesa's own documented
     * override with the real count — the same platform-fact-carrying this
     * function already does for CAP_JIT, two paragraphs up. The user's own
     * LP_NUM_THREADS always wins (overwrite=0), and a 1-CPU guest is left
     * untouched (unset == llvmpipe's inline rasterisation, the same path).
     * SDL_GetCPUCount is sysconf(_SC_NPROCESSORS_ONLN) on this port
     * (HAVE_SYSCONF; honest since pouch 0032 reads /ctl/sched "cpus: N").
     * Before the context create, because llvmpipe reads the variable once
     * at screen creation inside OSMesaCreateContextExt. Measured on the
     * GLQuake demo1 gate (640x400, -smp 4, HVF): 21.6 -> 29.1 fps. */
    if (!SDL_getenv("LP_NUM_THREADS")) {
        int ncpu = SDL_GetCPUCount();
        if (ncpu > 1) {
            char nbuf[16];
            SDL_snprintf(nbuf, sizeof nbuf, "%d", ncpu);
            SDL_setenv("LP_NUM_THREADS", nbuf, 0);
        }
    }

    ctx = (THYLACINE_GLContext *)SDL_calloc(1, sizeof(*ctx));
    if (!ctx) {
        SDL_OutOfMemory();
        return NULL;
    }

    /* OSMESA_BGRA is the whole reason this backend is zero-copy. A weave slot
     * is w*h little-endian 0xAARRGGBB words, i.e. the bytes B,G,R,A in
     * ascending address order — which is precisely what OSMesa calls BGRA. So
     * llvmpipe rasterises straight into the pixels the compositor will read;
     * §9's "rendered into (or blitted into) the weave" resolves to "into".
     * Changing this constant reintroduces a full-frame conversion. */
    ctx->osmesa = OSMesaCreateContextExt(OSMESA_BGRA,
                                         (GLint)_this->gl_config.depth_size,
                                         (GLint)_this->gl_config.stencil_size,
                                         (GLint)_this->gl_config.accum_red_size,
                                         NULL);
    if (!ctx->osmesa) {
        SDL_free(ctx);
        SDL_SetError("thylacine: OSMesaCreateContextExt failed");
        return NULL;
    }
    ctx->window = window;

    /* SDL_GL_CreateContext documents that "creating a context is assumed to
     * make it current in the SDL driver" and updates its own current-context
     * bookkeeping on that basis without calling MakeCurrent — so binding here
     * is required, not a convenience. */
    if (gl_bind(ctx, wd) != 0) {
        OSMesaDestroyContext(ctx->osmesa);
        SDL_free(ctx);
        return NULL;
    }
    return (SDL_GLContext)ctx;
}

int THYLACINE_GL_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
    THYLACINE_GLContext *ctx = (THYLACINE_GLContext *)context;
    SDL_WindowData *wd;

    (void)_this;
    if (!ctx) {
        /* SDL uses (NULL, NULL) to mean "unbind". OSMesa has no unbind call;
         * leaving the context bound is harmless because the next bind
         * replaces it, and nothing reads GL state without a current context. */
        return 0;
    }
    if (!window) {
        return SDL_SetError("thylacine: MakeCurrent needs a window");
    }
    wd = (SDL_WindowData *)window->driverdata;
    if (!wd) {
        return SDL_SetError("thylacine: no surface for window");
    }
    ctx->window = window;
    return gl_bind(ctx, wd);
}

int THYLACINE_GL_SetSwapInterval(_THIS, int interval)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;

    /* No tearing to guard against and no back buffer to flip: a present is a
     * blocking write the compositor completes inside one dispatch. So a
     * negative (adaptive) interval has nothing to adapt, and any interval > 1
     * would mean dropping frames on purpose. Accept 0 (unthrottled) and 1
     * (wait for the compositor's frame tick) and refuse the rest honestly
     * rather than silently rounding — SDL_GL_SetSwapInterval's return value is
     * how an application learns what it actually got. */
    if (interval != 0 && interval != 1) {
        return SDL_SetError("thylacine: swap interval %d unsupported (0 or 1)",
                            interval);
    }
    vd->gl_swap_interval = interval;
    return 0;
}

int THYLACINE_GL_GetSwapInterval(_THIS)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    return vd->gl_swap_interval;
}

int THYLACINE_GL_SwapWindow(_THIS, SDL_Window *window)
{
    SDL_VideoData *vd = (SDL_VideoData *)_this->driverdata;
    SDL_WindowData *wd = (SDL_WindowData *)window->driverdata;
    THYLACINE_GLContext *ctx;

    if (!wd) {
        return SDL_SetError("thylacine: no surface for window");
    }
    ctx = (THYLACINE_GLContext *)SDL_GL_GetCurrentContext();
    if (!ctx) {
        return SDL_SetError("thylacine: no current GL context");
    }

    /* A reweave since the last bind moved the pixels out from under the
     * context. Re-bind BEFORE glFinish, so the rasteriser flushes into the
     * mapping that is about to be presented rather than the retired one. */
    if (ctx->bound_va != wd->tap.map_va || ctx->bound_w != wd->tap.w ||
        ctx->bound_h != wd->tap.h) {
        if (gl_bind(ctx, wd) != 0) {
            return -1;
        }
    }

    /* llvmpipe rasterises on a thread pool, so the draw calls issued this
     * frame are not necessarily in the buffer yet. glFinish is what makes the
     * present a present rather than a race with the rasteriser — without it
     * the compositor reads partially-drawn tiles. This is the one GL call the
     * backend itself makes.
     *
     * Warp-4 direct: the compositor displays the GL resource itself, and
     * ordering to the display is structural (the winsys submits on flush;
     * the server's SET_SCANOUT/RESOURCE_FLUSH queue behind it on the one
     * controlq, FIFO) — so a glFlush suffices and a glFinish would
     * serialize the whole GPU pipeline per frame for nothing. The winsys's
     * own 2-in-flight fence throttle bounds the run-ahead. */
    if (ctx->direct && glFlush) {
        glFlush();
    } else {
        glFinish();
    }

    if (vd->gl_swap_interval > 0) {
        THYLACINE_PaceFrame(wd);
    }

    /* Full-surface damage: GL gives no damage information, and a GL frame
     * that only touched part of the surface is not something we can know. */
    if (thyla_tap_present(&wd->tap, NULL, 0) != 0) {
        return SDL_SetError("thylacine: present failed");
    }
    return 0;
}

void THYLACINE_GL_DeleteContext(_THIS, SDL_GLContext context)
{
    THYLACINE_GLContext *ctx = (THYLACINE_GLContext *)context;

    (void)_this;
    if (!ctx) {
        return;
    }
    if (ctx->osmesa && OSMesaDestroyContext) {
        /* Withdraw a live direct consent first: the warp ctx (per-process,
         * on the shared screen) outlives this GL context, so the consent
         * would otherwise dangle until the surface's gen pin inerts it. */
        if (ctx->direct && OSMesaThylacineDirectOff) {
            OSMesaThylacineDirectOff(ctx->osmesa);
        }
        OSMesaDestroyContext(ctx->osmesa);
    }
    SDL_free(ctx);
}

#endif /* SDL_VIDEO_DRIVER_THYLACINE */
