/* gl-sdl-prove — the CL-7 step 2 proving binary (#138).
 *
 * The GL twin of sdl-probe. Where sdl-probe reaches the weave through
 * SDL_GetWindowSurface + SDL_UpdateWindowSurface, this one reaches it through
 * SDL_GL_CreateContext + SDL_GL_SwapWindow: OSMesa binds llvmpipe's output
 * directly to weave slot 0, the JIT compiles the rasteriser pipeline to
 * AArch64 through the I-42 dual-mapped code Burrow, and the present is the
 * same synchronous tpresent.
 *
 * It draws sdl-probe's EXACT quadrant pattern (TL red / TR green / BL blue /
 * BR white) on purpose: the screendump family already knows how to check that
 * pattern, so pointing the established checker at this binary makes the
 * rendering path the only variable between the two. A GL-specific pattern
 * would have needed a GL-specific checker, and then a disagreement between
 * them would prove nothing about either.
 *
 * The exit code is the verdict, not the output. Every station returns a
 * distinct non-zero code, so a gate can treat rc == 0 as a strong assertion
 * and parse nothing — the CL-7b lesson (an osmesa-prove that only PRINTED its
 * failures let a boot with no CAP_JIT run on and report the generic
 * NULL-context error, indistinguishable from three other faults).
 *
 * Output contract (greppable, the probe convention):
 *   "gl-sdl-prove: PASS renderer=<...> WxH frames=N"  on success
 *   "gl-sdl-prove: FAIL <stage>: <detail>"            on any failure
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL.h>
#include <SDL_opengl.h>

/* llvmpipe JITs inside THIS process, and CAP_JIT is elevation-only -- it is
 * stripped at every fork, so nothing can hand it to us. A GL program on
 * Thylacine has to walk the corvus clearance path for itself before it touches
 * a context. Skipping that is not a subtle failure: SYS_JIT_CREATE returns
 * EACCES and llvmpipe reports "Failed to materialize symbols", which reads
 * like a Mesa fault rather than a missing capability. */
#include "thyla_capjit.h"

#define PROBE_W 640
#define PROBE_H 400
#define PROBE_FRAMES 8

/* Station codes -- the verdict. Distinct so a failure names itself without
 * the gate reading stdout. */
enum {
    RC_OK = 0,
    RC_CAPJIT = 1,
    RC_INIT = 2,
    RC_DRIVER = 3,
    RC_WINDOW = 4,
    RC_CONTEXT = 5,
    RC_PROCADDR = 6,
    RC_RENDERER = 7,
    RC_SWAP = 8,
    RC_PIXELS = 9
};

static int fail(const char *stage, const char *detail, int rc)
{
    printf("gl-sdl-prove: FAIL %s: %s\n", stage,
           detail ? detail : SDL_GetError());
    return rc;
}

/* GL's origin is bottom-left; the weave (like every framebuffer) is top-down,
 * and the backend's OSMESA_Y_UP=0 is what reconciles them at storage level. GL
 * coordinates stay self-consistent either way -- so a quadrant that must be
 * DISPLAYED at the top is drawn at the TOP in GL coordinates too, and read
 * back from there. This helper keeps that mapping in exactly one place. */
static void quadrant(int top, int left, float r, float g, float b)
{
    glScissor(left ? 0 : PROBE_W / 2, top ? PROBE_H / 2 : 0,
              PROBE_W / 2, PROBE_H / 2);
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
}

/* Read one pixel in GL coordinates and compare against an expected RGB.
 * Tolerance 8/255 absorbs any format conversion on the way through; a wrong
 * quadrant is off by ~255, so this cannot pass a mis-drawn frame. */
static int pixel_is(int x, int y, int r, int g, int b, const char *what)
{
    unsigned char px[4] = { 0, 0, 0, 0 };

    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (abs((int)px[0] - r) > 8 || abs((int)px[1] - g) > 8 ||
        abs((int)px[2] - b) > 8) {
        printf("gl-sdl-prove: pixel %s at (%d,%d) = %u,%u,%u want %d,%d,%d\n",
               what, x, y, px[0], px[1], px[2], r, g, b);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    /* Optional hold: `gl-sdl-prove N` keeps the final frame presented for N
     * extra seconds (1 Hz re-presents) -- the screendump window for the
     * interactive scenario, same contract as sdl-probe. */
    int hold_s = (argc > 1) ? atoi(argv[1]) : 0;
    const char *driver, *renderer, *version;
    SDL_Window *win;
    SDL_GLContext ctx;
    int frames = 0, i;

    /* BEFORE SDL_Init, and before anything touches GL. Without it llvmpipe's
     * first JIT gets EACCES from SYS_JIT_CREATE and surfaces as "Failed to
     * materialize symbols" -- a Mesa-shaped message for a capability-shaped
     * problem, three layers from its cause. Its own station code so the gate
     * can tell a missing clearance from a broken context. */
    if (thyla_acquire_cap_jit("gl-sdl-prove") != 0) {
        return RC_CAPJIT;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        return fail("init", NULL, RC_INIT);
    }
    driver = SDL_GetCurrentVideoDriver();
    if (!driver || strcmp(driver, "thylacine") != 0) {
        SDL_Quit();
        return fail("driver", driver ? driver : "(none)", RC_DRIVER);
    }

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, 32);

    win = SDL_CreateWindow("gl-sdl-prove", SDL_WINDOWPOS_UNDEFINED,
                           SDL_WINDOWPOS_UNDEFINED, PROBE_W, PROBE_H,
                           SDL_WINDOW_OPENGL);
    if (!win) {
        SDL_Quit();
        return fail("window", NULL, RC_WINDOW);
    }

    ctx = SDL_GL_CreateContext(win);
    if (!ctx) {
        SDL_DestroyWindow(win);
        SDL_Quit();
        return fail("context", NULL, RC_CONTEXT);
    }

    /* The SDL entry-point path, exercised the way a stock GL program that
     * loads extensions would. Distinct from calling glClear directly below --
     * that resolves at link time, this at run time through the driver hook. */
    if (!SDL_GL_GetProcAddress("glClear")) {
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return fail("procaddr", "SDL_GL_GetProcAddress(glClear) == NULL",
                    RC_PROCADDR);
    }

    renderer = (const char *)glGetString(GL_RENDERER);
    version = (const char *)glGetString(GL_VERSION);
    if (!renderer || !strstr(renderer, "llvmpipe")) {
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        /* Not cosmetic: a context that came up on some stub rasteriser would
         * pass every other station here while proving nothing about CL-7. */
        return fail("renderer", renderer ? renderer : "(none)", RC_RENDERER);
    }
    printf("gl-sdl-prove: renderer=%s version=%s\n", renderer,
           version ? version : "(none)");

    glViewport(0, 0, PROBE_W, PROBE_H);
    glEnable(GL_SCISSOR_TEST);

    for (; frames < PROBE_FRAMES; frames++) {
        SDL_Event ev;

        /* sdl-probe's pattern, drawn by the rasteriser instead of memset. */
        quadrant(1, 1, 1.0f, 0.0f, 0.0f); /* TL red   */
        quadrant(1, 0, 0.0f, 1.0f, 0.0f); /* TR green */
        quadrant(0, 1, 0.0f, 0.0f, 1.0f); /* BL blue  */
        quadrant(0, 0, 1.0f, 1.0f, 1.0f); /* BR white */

        /* A real primitive through the full pipeline. The scissored clears
         * above can take a fast path that never touches a JIT-compiled
         * fragment pipeline; a filled triangle cannot -- so without this the
         * probe could pass on a rasteriser that never actually compiled
         * anything, which is the CL-7 claim. Drawn in the dead centre, in a
         * fifth colour, and read back below. */
        glDisable(GL_SCISSOR_TEST);
        glColor3f(1.0f, 0.0f, 1.0f);
        glBegin(GL_TRIANGLES);
        glVertex2f(-0.5f, -0.5f);
        glVertex2f(0.5f, -0.5f);
        glVertex2f(0.0f, 0.5f);
        glEnd();
        glEnable(GL_SCISSOR_TEST);

        /* The public SDL_GL_SwapWindow returns void -- the int-returning
         * SDL_GL_SwapWindowWithResult is internal to SDL. So the failure
         * channel is the error string, cleared first so a stale error from
         * some earlier call cannot be read as this swap's. */
        SDL_ClearError();
        SDL_GL_SwapWindow(win);
        if (SDL_GetError()[0] != '\0') {
            SDL_GL_DeleteContext(ctx);
            SDL_DestroyWindow(win);
            SDL_Quit();
            return fail("swap", NULL, RC_SWAP);
        }
        while (SDL_PollEvent(&ev)) {
            /* Drained, not acted on: the point is that the pump keeps
             * running alongside a GL context (it is a separate thread
             * touching the same surface). */
        }
    }

    /* Verify what the rasteriser actually produced. glReadPixels goes through
     * the same buffer the compositor read, so agreement here is agreement
     * about the bytes that were presented -- not about what we asked for.
     *
     * The quadrant samples sit at 1/8 and 7/8, NOT at the quadrant centres,
     * because the triangle is drawn UNSCISSORED and therefore overwrites the
     * middle of all four. Its NDC span [-0.5,0.5] x [-0.5,0.5] maps to screen
     * x in [160,480], y in [100,300] at 640x400 -- so the obvious centre
     * samples (W/4,H/4) and (3W/4,H/4) land EXACTLY on its two bottom
     * vertices. That is what the first real run of these checks found: BL read
     * magenta, the triangle's colour, and the readback was right while the
     * assertion was wrong.
     *
     * These lines had never executed before #139: every prior in-guest run
     * died at the CAP_JIT gate, so this station was unreachable and its bug
     * could not surface. Keep the samples in the outer corners, clear of the
     * triangle by ~80px in x and ~50px in y; anything that grows the triangle
     * must move them again. */
    glDisable(GL_SCISSOR_TEST);
    if (!pixel_is(PROBE_W / 8, PROBE_H * 7 / 8, 255, 0, 0, "TL") ||
        !pixel_is(PROBE_W * 7 / 8, PROBE_H * 7 / 8, 0, 255, 0, "TR") ||
        !pixel_is(PROBE_W / 8, PROBE_H / 8, 0, 0, 255, "BL") ||
        !pixel_is(PROBE_W * 7 / 8, PROBE_H / 8, 255, 255, 255, "BR") ||
        !pixel_is(PROBE_W / 2, PROBE_H / 2 - 8, 255, 0, 255, "triangle")) {
        SDL_GL_DeleteContext(ctx);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return fail("pixels", "readback mismatch (see above)", RC_PIXELS);
    }

    printf("gl-sdl-prove: PASS renderer=%s %dx%d frames=%d\n", renderer,
           PROBE_W, PROBE_H, frames);
    fflush(stdout);

    for (i = 0; i < hold_s; i++) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
        }
        SDL_GL_SwapWindow(win);
        SDL_Delay(1000);
    }

    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return RC_OK;
}
