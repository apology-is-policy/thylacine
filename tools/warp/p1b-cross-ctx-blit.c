/* Warp-C P1b, host-side: may a virgl context blit FROM a resource created by
 * ANOTHER context, given an explicit virgl_renderer_ctx_attach_resource?
 *
 * WHY THIS RUNS OUTSIDE THYLACINE AT ALL. P1b must answer before anything
 * structural lands, but inside the guest it needs a cross-attach verb that does
 * not exist: CTX_ATTACH_RESOURCE lives only inside tapestryd and both call
 * sites attach to the resource's OWN dev_ctx, while the client-facing ctl verbs
 * are verify / present-to / submit. Building the verb is C-2, and P1b gates
 * C-2. That circle is cut by asking virglrenderer directly -- no guest change,
 * no I-45 authority decision, no scripture change. It either kills the design
 * cheaply or de-risks C-2 completely.
 *
 * WHAT MAKES THE ANSWER TRUSTWORTHY (the P1a instrument discipline):
 *
 *  1. A SAME-CONTEXT blit runs FIRST as a positive control. Without it, a
 *     mis-encoded blit produces an unchanged destination, which reads exactly
 *     like "the renderer refused" -- the wrong answer, and the comforting one.
 *     If the control does not move the pixels, this aborts and reports an
 *     INSTRUMENT failure rather than a result.
 *  2. THREE distinguishable values, never presence-vs-absence. The destination
 *     is RED, the cross-context source is GREEN, the control source is BLUE, so
 *     the readback names what happened instead of merely whether it changed:
 *     GREEN = the cross-context blit ran, RED = refused/no-op, BLUE = the reset
 *     between phases did not take (a bug in this probe, not a finding).
 *  3. A 0xDEADBEEF sentinel pre-fills every readback buffer, so "the transfer
 *     never landed" cannot be misread as any of the three.
 *  4. Opcodes and field offsets come from the project's own headers, not from
 *     recall. An earlier run lost time to opcode 21 (GET_QUERY_RESULT) standing
 *     in for BLIT (16); including the header removes that whole class.
 *
 * Deliberately built against the FETCHED 1.9.0 headers, not Debian's
 * libvirglrenderer-dev (1.1.0), and linked -l:libvirglrenderer.so.1 -- a header
 * from one ABI over a runtime from another is exactly the setup that yields a
 * confident wrong answer.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>

#include "virglrenderer.h"
#include "virgl_hw.h"
#include "virgl_protocol.h"

/* Mesa's, not virgl's: these live in pipe headers this deliberately does not
 * pull in. Pinned by mesa p_defines.h. */
#define PIPE_TEXTURE_2D     2
#define PIPE_CLEAR_COLOR0   (1u << 2)
#define PIPE_MASK_RGBA      0xf

#define W 64
#define H 64
#define FMT VIRGL_FORMAT_B8G8R8A8_UNORM

#define CTX_A 1
#define CTX_B 2

#define RES_GREEN 1080      /* created and cleared by ctx A -- the foreign one */
#define RES_DST   1081      /* ctx B's own; the blit destination */
#define RES_BLUE  1082      /* ctx B's own; the control source */

#define SURF_BASE 2000
#define SENTINEL  0xDEADBEEFu

/* B8G8R8A8 in memory order, read back as a little-endian u32.
 * Derived, then CHECKED against a real readback in phase 1 -- if the convention
 * here is wrong, phase 1 fails loudly instead of poisoning every later compare. */
#define C_GREEN 0xFF00FF00u
#define C_RED   0xFFFF0000u
#define C_BLUE  0xFF0000FFu

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

/* ---- command stream ---------------------------------------------------- */
static uint32_t cbuf[512];
static int cdw;
static void c_reset(void) { cdw = 0; }
static void c_push(uint32_t v) { cbuf[cdw++] = v; }

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

static int c_submit(int ctx)
{
    int r = virgl_renderer_submit_cmd(cbuf, ctx, cdw);
    c_reset();
    return r;
}

static void cmd_surface(uint32_t surf, uint32_t res)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE,
                      VIRGL_OBJ_SURFACE_SIZE));
    c_push(surf);
    c_push(res);
    c_push(FMT);
    c_push(0);                  /* texture level  */
    c_push(0);                  /* texture layers */
}

static void cmd_framebuffer(uint32_t surf)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0,
                      VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)));
    c_push(1);                  /* nr_cbufs    */
    c_push(0);                  /* zsurf handle */
    c_push(surf);
}

static void cmd_clear(float r, float g, float b)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE));
    c_push(PIPE_CLEAR_COLOR0);
    c_push(f2u(r)); c_push(f2u(g)); c_push(f2u(b)); c_push(f2u(1.0f));
    c_push(0); c_push(0);       /* depth (double) */
    c_push(0);                  /* stencil        */
}

static void cmd_blit(uint32_t dst, uint32_t src)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE));
    c_push(VIRGL_CMD_BLIT_S0_MASK(PIPE_MASK_RGBA) | VIRGL_CMD_BLIT_S0_FILTER(0));
    c_push(0);                  /* scissor minx/miny */
    c_push(0);                  /* scissor maxx/maxy */
    c_push(dst); c_push(0); c_push(FMT);
    c_push(0); c_push(0); c_push(0);
    c_push(W); c_push(H); c_push(1);
    c_push(src); c_push(0); c_push(FMT);
    c_push(0); c_push(0); c_push(0);
    c_push(W); c_push(H); c_push(1);
}

/* Clear `res` through `ctx`. Each phase builds its own surface handle so a
 * stale object can never silently satisfy a later phase. */
static int paint(int ctx, uint32_t res, uint32_t surf, float r, float g, float b)
{
    c_reset();
    cmd_surface(surf, res);
    cmd_framebuffer(surf);
    cmd_clear(r, g, b);
    return c_submit(ctx);
}

/* ---- readback ---------------------------------------------------------- */
static uint32_t px[W * H];

static uint32_t readback(uint32_t res, int ctx)
{
    struct virgl_box box = { .x = 0, .y = 0, .z = 0, .w = W, .h = H, .d = 1 };
    struct iovec iov = { .iov_base = px, .iov_len = sizeof(px) };

    for (int i = 0; i < W * H; i++)
        px[i] = SENTINEL;       /* so "never landed" is its own value */

    int r = virgl_renderer_transfer_read_iov(res, ctx, 0, 0, 0, &box, 0, &iov, 1);
    if (r) {
        printf("  transfer_read_iov(res=%u ctx=%d) failed rc=%d\n", res, ctx, r);
        return SENTINEL;
    }
    return px[0];
}

static const char *name_of(uint32_t v)
{
    if (v == C_GREEN)  return "GREEN";
    if (v == C_RED)    return "RED";
    if (v == C_BLUE)   return "BLUE";
    if (v == SENTINEL) return "SENTINEL(readback never landed)";
    return "UNKNOWN";
}

/* ---- virglrenderer bring-up -------------------------------------------- */
static void cb_write_fence(void *cookie, uint32_t fence) { (void)cookie; (void)fence; }
static int  cb_get_drm_fd(void *cookie)
{
    (void)cookie;
    const char *node = getenv("P1B_NODE");
    if (!node) node = "/dev/dri/renderD128";
    int fd = open(node, O_RDWR | O_CLOEXEC);
    printf("  [cb] get_drm_fd(%s) -> %d%s\n", node, fd,
           fd < 0 ? " (FAILED)" : "");
    fflush(stdout);
    return fd;
}

static struct virgl_renderer_callbacks cbs = {
    .version     = 2,           /* v2 is what get_drm_fd needs; claim no more */
    .write_fence = cb_write_fence,
    .get_drm_fd  = cb_get_drm_fd,
};

/* At version 1 the struct predates get_drm_fd, so virglrenderer must find a
 * device on its own. Sweeping this separates "my fd is unacceptable" from
 * "this box cannot bring up a renderer headless at all" -- two failures that
 * look identical from a bare rc=-1. */
static struct virgl_renderer_callbacks cbs_v1 = {
    .version     = 1,
    .write_fence = cb_write_fence,
};

static int mkres(uint32_t handle)
{
    struct virgl_renderer_resource_create_args a = {
        .handle = handle, .target = PIPE_TEXTURE_2D, .format = FMT,
        .bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
        .width = W, .height = H, .depth = 1, .array_size = 1,
        .last_level = 0, .nr_samples = 0, .flags = 0,
    };
    return virgl_renderer_resource_create(&a, NULL, 0);
}

/* The renderer knows exactly why it refused; without this it returns a bare
 * -1 and the caller is left guessing at flag combinations. */
static void cb_log(enum virgl_log_level_flags lvl, const char *msg, void *ud)
{
    (void)ud;
    static const char *n[] = { "DEBUG", "INFO", "WARN", "ERROR", "SILENT" };
    printf("  [virgl %s] %s", lvl <= VIRGL_LOG_LEVEL_SILENT ? n[lvl] : "?", msg);
    if (!strchr(msg, '\n')) printf("\n");
}

int main(void)
{
    int rc;

    virgl_set_log_callback(cb_log, NULL, NULL);

    /* Swept from the environment rather than recompiled: which winsys flags a
     * headless V3D box accepts is a property of the host, not something to
     * hardcode from a guess. */
    int flags = VIRGL_RENDERER_USE_EGL;
    const char *fenv = getenv("P1B_FLAGS");
    if (fenv) flags = (int)strtol(fenv, NULL, 0);

    struct virgl_renderer_callbacks *use = &cbs;
    if (getenv("P1B_CBV1")) use = &cbs_v1;

    /* The cookie MUST be non-NULL. virgl_renderer_init brings the winsys up
     * first and only then checks `if (!cookie || !cbs)` before initializing
     * vrend -- so passing NULL gets you a successful get_drm_fd callback, a
     * successful winsys, and then a bare rc=-1, which looks exactly like the
     * host refusing to bring up a headless renderer. Four flag sweeps could not
     * distinguish those; reading the source did it in one step. */
    static int cookie_storage;
    void *cookie = &cookie_storage;

    printf("virgl_renderer_init(flags=0x%x, cbs.version=%d)\n", flags, use->version);
    rc = virgl_renderer_init(cookie, flags, use);
    if (rc) { printf("virgl_renderer_init failed rc=%d\n", rc); return 2; }
    printf("virglrenderer up (renderD128)\n\n");

    CHECK(virgl_renderer_context_create(CTX_A, 4, "ctxA") == 0, "ctx A create");
    CHECK(virgl_renderer_context_create(CTX_B, 4, "ctxB") == 0, "ctx B create");
    CHECK(mkres(RES_GREEN) == 0, "res GREEN create");
    CHECK(mkres(RES_DST)   == 0, "res DST create");
    CHECK(mkres(RES_BLUE)  == 0, "res BLUE create");

    /* Each context attaches only what it owns. This is the baseline the real
     * question deviates from. */
    virgl_renderer_ctx_attach_resource(CTX_A, RES_GREEN);
    virgl_renderer_ctx_attach_resource(CTX_B, RES_DST);
    virgl_renderer_ctx_attach_resource(CTX_B, RES_BLUE);

    /* -- phase 1: paint, and prove the instrument reads what was painted --- */
    CHECK(paint(CTX_A, RES_GREEN, SURF_BASE + 0, 0.0f, 1.0f, 0.0f) == 0, "paint GREEN");
    CHECK(paint(CTX_B, RES_DST,   SURF_BASE + 1, 1.0f, 0.0f, 0.0f) == 0, "paint RED");
    CHECK(paint(CTX_B, RES_BLUE,  SURF_BASE + 2, 0.0f, 0.0f, 1.0f) == 0, "paint BLUE");

    uint32_t g = readback(RES_GREEN, CTX_A);
    uint32_t d = readback(RES_DST,   CTX_B);
    printf("phase 1  GREEN=%08x (%s)  DST=%08x (%s)\n", g, name_of(g), d, name_of(d));
    if (g != C_GREEN || d != C_RED) {
        printf("\nINSTRUMENT FAILURE: readback does not match the colours just "
               "painted.\nThe colour convention or the clear encoding is wrong, so "
               "NO conclusion about\ncross-context blitting can be drawn from this "
               "run. Not a result.\n");
        virgl_renderer_cleanup(NULL);
        return 3;
    }

    /* -- phase 2: POSITIVE CONTROL, same-context blit ---------------------- */
    c_reset();
    cmd_blit(RES_DST, RES_BLUE);
    CHECK(c_submit(CTX_B) == 0, "same-context blit submit");
    uint32_t ctl = readback(RES_DST, CTX_B);
    printf("phase 2  control (B: BLUE -> DST) = %08x (%s)\n", ctl, name_of(ctl));
    if (ctl != C_BLUE) {
        printf("\nINSTRUMENT FAILURE: a SAME-CONTEXT blit did not move the "
               "pixels.\nThe blit encoding is wrong. A cross-context refusal and a "
               "broken encoding\nare indistinguishable from here, so this run "
               "cannot answer P1b.\n");
        virgl_renderer_cleanup(NULL);
        return 4;
    }
    printf("         blit encoding CONFIRMED working\n");

    /* -- phase 3: reset the destination so GREEN can only arrive by blit --- */
    CHECK(paint(CTX_B, RES_DST, SURF_BASE + 3, 1.0f, 0.0f, 0.0f) == 0, "repaint RED");
    uint32_t reset = readback(RES_DST, CTX_B);
    printf("phase 3  DST reset = %08x (%s)\n", reset, name_of(reset));
    if (reset != C_RED) {
        printf("\nINSTRUMENT FAILURE: the destination did not reset to RED.\n");
        virgl_renderer_cleanup(NULL);
        return 5;
    }

    /* -- phase 4: THE QUESTION -------------------------------------------- */
    /* THE CONTROL THAT DECIDES WHAT A "WORKS" MEANS. If the blit succeeds with
     * the attach, that alone does not show the attach is what PERMITTED it --
     * only that it did not prevent it. Skipping the attach separates the two
     * readings, and they are opposite for I-45:
     *   refused without / works with  -> the attach IS the authority gate, and
     *                                    C-2's verb is the place authority is
     *                                    conferred.
     *   works without                 -> virglrenderer does not isolate
     *                                    resources between contexts at all, so
     *                                    the guest-exposure half of I-45 cannot
     *                                    rest on the renderer refusing. */
    bool do_attach = getenv("P1B_NO_ATTACH") == NULL;
    printf("\nphase 4  ctx B blits from ctx A's resource (attach=%s)\n",
           do_attach ? "YES" : "NO -- control arm");
    if (do_attach)
        virgl_renderer_ctx_attach_resource(CTX_B, RES_GREEN);  /* returns void */

    c_reset();
    cmd_blit(RES_DST, RES_GREEN);
    int blit_rc = c_submit(CTX_B);
    printf("         submit_cmd rc=%d\n", blit_rc);

    uint32_t out = readback(RES_DST, CTX_B);
    printf("         DST = %08x (%s)\n\n", out, name_of(out));

    printf("=== P1b VERDICT (attach=%s) ===\n", do_attach ? "YES" : "NO");
    if (out == C_GREEN && do_attach) {
        printf("  WORKS: an attached cross-context blit moved the pixels.\n"
               "  C-2 can build the attach verb; the design survives.\n");
    } else if (out == C_RED && do_attach) {
        printf("  REFUSED: the destination is untouched even WITH the attach,\n"
               "  and a same-context blit was proven working in phase 2.\n"
               "  The cross-context-blit design dies here.\n");
    } else if (out == C_RED && !do_attach) {
        printf("  CONTROL AS EXPECTED: without the attach the blit was refused.\n"
               "  Paired with the attached arm, this shows the attach is what\n"
               "  PERMITS the access -- the renderer does isolate resources by\n"
               "  context, so C-2's verb is where authority is conferred.\n");
    } else if (out == C_GREEN && !do_attach) {
        printf("  ISOLATION ABSENT: the blit succeeded with NO attach at all.\n"
               "  virglrenderer does not isolate resources between contexts, so\n"
               "  the guest-exposure half of I-45 cannot rest on the renderer\n"
               "  refusing. This is a finding about the host trust boundary.\n");
    } else {
        printf("  INDETERMINATE (%s) -- neither moved nor cleanly refused.\n",
               name_of(out));
    }

    printf("\nchecks failed: %d\n", failures);
    virgl_renderer_cleanup(NULL);
    return failures ? 1 : 0;
}
