/* Warp-C P2, host-side: does a blit on one context observe the FINISHED work of
 * another context, with no sync between them?
 *
 * THE HAZARD (GPU-DESIGN.md section 4.5.4). virglrenderer maps each guest
 * context to its own host GL context. In-order controlq dequeue orders the
 * COMMANDS but not GL execution across two host contexts sharing an object.
 * Today that is masked, and masked by an accident: the present path ends in
 * `transfer_from_3d_sync`, which must produce bytes and therefore forces the
 * sync as a side effect. A BLIT HAS NO SUCH SIDE EFFECT, so the hazard goes
 * live exactly when Warp-C removes the readback. The failure mode is a torn or
 * stale frame -- an I-40 tearing-freedom violation, which is a soundness
 * question, not a perf detail.
 *
 * WHAT THIS CAN AND CANNOT CONCLUDE, stated up front because it bounds the
 * verdict. A PASS here is evidence that the ordering holds on THIS stack
 * (virglrenderer 1.9.0 + Mesa/V3D on the Pi) for THIS access pattern -- it is
 * not a proof, because a race that does not reproduce is not a race that cannot
 * happen. A FAIL is decisive in the other direction: it means the hazard is
 * real and C-1's spec work has a counterexample to model. So this probe is
 * built to make a failure LIKELY if one is possible (large surfaces, deep
 * unsynced queues, many trials), and to report its own sensitivity rather than
 * claim a clean bill of health.
 *
 * INSTRUMENT DISCIPLINE, inherited from P1a/P1b:
 *   1. A SYNCED arm runs first as the positive control. If the ordering test
 *      fails even WITH an explicit sync between the two contexts, the probe is
 *      broken and no conclusion about the unsynced case is available.
 *   2. Colours CYCLE through three distinguishable values, so a stale read
 *      names WHICH frame it saw (the previous one) instead of merely "not the
 *      expected one". Presence-vs-absence cannot tell stale from corrupt.
 *   3. A 0xDEADBEEF sentinel pre-fills every readback, so "the transfer never
 *      landed" is its own value.
 *   4. Opcodes and offsets come from the project's own headers.
 *
 * Build/run via `tools/warp-host.sh p2`.
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

#define PIPE_TEXTURE_2D     2
#define PIPE_CLEAR_COLOR0   (1u << 2)
#define PIPE_MASK_RGBA      0xf

/* Big on purpose: the wider the surface, the longer ctx A's clears stay in
 * flight, and the more room there is for B's blit to run early if nothing
 * orders them. A 64x64 probe would be almost guaranteed to pass vacuously. */
#define W 1024
#define H 1024

#define CTX_A 1
#define CTX_B 2
#define RES_SRC 1080        /* ctx A renders here */
#define RES_DST 1081        /* ctx B blits into here */
#define SURF_A  2000

#define SENTINEL 0xDEADBEEFu
#define C_GREEN  0xFF00FF00u
#define C_RED    0xFFFF0000u
#define C_BLUE   0xFF0000FFu

/* How many clears ctx A submits per trial before B blits. Deeper queue = more
 * unfinished GPU work outstanding when the blit is issued. */
static int DEPTH  = 24;
static int TRIALS = 200;

static uint32_t cbuf[4096];
static int cdw;
static void c_reset(void) { cdw = 0; }
static void c_push(uint32_t v) { cbuf[cdw++] = v; }
static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static int c_submit(int ctx) { int r = virgl_renderer_submit_cmd(cbuf, ctx, cdw); c_reset(); return r; }

static void cmd_surface(uint32_t surf, uint32_t res)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE, VIRGL_OBJ_SURFACE_SIZE));
    c_push(surf); c_push(res); c_push(VIRGL_FORMAT_B8G8R8A8_UNORM); c_push(0); c_push(0);
}
static void cmd_framebuffer(uint32_t surf)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0, VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)));
    c_push(1); c_push(0); c_push(surf);
}
static void cmd_clear(float r, float g, float b)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, VIRGL_OBJ_CLEAR_SIZE));
    c_push(PIPE_CLEAR_COLOR0);
    c_push(f2u(r)); c_push(f2u(g)); c_push(f2u(b)); c_push(f2u(1.0f));
    c_push(0); c_push(0); c_push(0);
}
static void cmd_blit(uint32_t dst, uint32_t src)
{
    c_push(VIRGL_CMD0(VIRGL_CCMD_BLIT, 0, VIRGL_CMD_BLIT_SIZE));
    c_push(VIRGL_CMD_BLIT_S0_MASK(PIPE_MASK_RGBA) | VIRGL_CMD_BLIT_S0_FILTER(0));
    c_push(0); c_push(0);
    c_push(dst); c_push(0); c_push(VIRGL_FORMAT_B8G8R8A8_UNORM);
    c_push(0); c_push(0); c_push(0); c_push(W); c_push(H); c_push(1);
    c_push(src); c_push(0); c_push(VIRGL_FORMAT_B8G8R8A8_UNORM);
    c_push(0); c_push(0); c_push(0); c_push(W); c_push(H); c_push(1);
}

/* 1x1 readback. The blit and the clears are full-surface; only one pixel needs
 * inspecting, and a small box keeps the transfer from dominating the trial. */
static uint32_t px1;
static uint32_t read_px(uint32_t res, int ctx)
{
    struct virgl_box box = { .x = 0, .y = 0, .z = 0, .w = 1, .h = 1, .d = 1 };
    struct iovec iov = { .iov_base = &px1, .iov_len = sizeof(px1) };
    px1 = SENTINEL;
    int r = virgl_renderer_transfer_read_iov(res, ctx, 0, 0, 0, &box, 0, &iov, 1);
    if (r) { printf("  transfer_read_iov failed rc=%d\n", r); return SENTINEL; }
    return px1;
}

static const char *nm(uint32_t v)
{
    if (v == C_GREEN)  return "GREEN";
    if (v == C_RED)    return "RED";
    if (v == C_BLUE)   return "BLUE";
    if (v == SENTINEL) return "SENTINEL";
    return "OTHER";
}

static void cb_write_fence(void *c, uint32_t f) { (void)c; (void)f; }
static int  cb_get_drm_fd(void *c)
{
    (void)c;
    const char *n = getenv("P2_NODE"); if (!n) n = "/dev/dri/renderD128";
    return open(n, O_RDWR | O_CLOEXEC);
}
static struct virgl_renderer_callbacks cbs = {
    .version = 2, .write_fence = cb_write_fence, .get_drm_fd = cb_get_drm_fd,
};

static int mkres(uint32_t h)
{
    struct virgl_renderer_resource_create_args a = {
        .handle = h, .target = PIPE_TEXTURE_2D, .format = VIRGL_FORMAT_B8G8R8A8_UNORM,
        .bind = VIRGL_BIND_RENDER_TARGET | VIRGL_BIND_SAMPLER_VIEW,
        .width = W, .height = H, .depth = 1, .array_size = 1,
        .last_level = 0, .nr_samples = 0, .flags = 0,
    };
    return virgl_renderer_resource_create(&a, NULL, 0);
}

/* One trial. `sync` inserts the ordering the real path gets by accident today
 * (a readback of the SOURCE on A, which forces A's work to finish). The
 * unsynced arm omits exactly that and changes nothing else. */
enum arm { ARM_SYNCED, ARM_UNSYNCED, ARM_INVERTED };

static uint32_t trial(int i, enum arm a, uint32_t *want_out)
{
    static const struct { float r, g, b; uint32_t v; } cyc[3] = {
        {0,1,0, C_GREEN}, {1,0,0, C_RED}, {0,0,1, C_BLUE},
    };
    const int k = i % 3;
    *want_out = cyc[k].v;

    /* THE SENSITIVITY ARM. Blit BEFORE the clear, so the destination provably
     * holds the PREVIOUS trial's colour. This is not a variant of the
     * experiment -- it is the control that proves the experiment can SEE a
     * stale read at all. Without it, "0 mismatches" is equally consistent with
     * "ordering holds" and "this probe cannot detect reordering", and those are
     * the two readings the whole run has to separate. */
    if (a == ARM_INVERTED) {
        c_reset();
        cmd_blit(RES_DST, RES_SRC);
        if (c_submit(CTX_B) != 0) { printf("  submit B failed\n"); return SENTINEL; }
        uint32_t got = read_px(RES_DST, CTX_B);
        c_reset();
        cmd_surface(SURF_A + (i & 1), RES_SRC);
        cmd_framebuffer(SURF_A + (i & 1));
        cmd_clear(cyc[k].r, cyc[k].g, cyc[k].b);
        (void)c_submit(CTX_A);
        (void)read_px(RES_SRC, CTX_A);        /* land it before the next trial */
        return got;
    }

    /* ctx A: a deep queue of full-surface clears ending in this trial's colour. */
    c_reset();
    cmd_surface(SURF_A + (i & 1), RES_SRC);
    cmd_framebuffer(SURF_A + (i & 1));
    for (int d = 0; d < DEPTH; d++) {
        const int j = (d == DEPTH - 1) ? k : (d % 3);
        cmd_clear(cyc[j].r, cyc[j].g, cyc[j].b);
    }
    if (c_submit(CTX_A) != 0) { printf("  submit A failed\n"); return SENTINEL; }

    if (a == ARM_SYNCED) (void)read_px(RES_SRC, CTX_A);  /* the accidental sync, made explicit */

    /* ctx B: blit A's surface across, with nothing ordering the two. */
    c_reset();
    cmd_blit(RES_DST, RES_SRC);
    if (c_submit(CTX_B) != 0) { printf("  submit B failed\n"); return SENTINEL; }

    return read_px(RES_DST, CTX_B);
}

static int run_arm(const char *label, enum arm a, int trials)
{
    int bad = 0, sent = 0;
    uint32_t first_want = 0, first_got = 0; int first_i = -1;
    for (int i = 0; i < trials; i++) {
        uint32_t want, got = trial(i, a, &want);
        if (got == SENTINEL) { sent++; continue; }
        if (got != want) {
            if (first_i < 0) { first_i = i; first_want = want; first_got = got; }
            bad++;
        }
    }
    printf("  %-9s trials=%d  mismatches=%d  sentinel=%d\n", label, trials, bad, sent);
    if (first_i >= 0)
        printf("             first at trial %d: wanted %s, got %s%s\n",
               first_i, nm(first_want), nm(first_got),
               first_got == ((first_want == C_GREEN) ? C_BLUE
                           : (first_want == C_RED)   ? C_GREEN : C_RED)
               ? "  <-- exactly the PREVIOUS frame: a stale read, not corruption" : "");
    return bad;
}

int main(void)
{
    static int cookie;      /* MUST be non-NULL: init checks it only AFTER the
                             * winsys comes up, so NULL yields a bare rc=-1 that
                             * looks like a host that cannot go headless. */
    if (getenv("P2_DEPTH"))  DEPTH  = atoi(getenv("P2_DEPTH"));
    if (getenv("P2_TRIALS")) TRIALS = atoi(getenv("P2_TRIALS"));

    int rc = virgl_renderer_init(&cookie, VIRGL_RENDERER_USE_EGL, &cbs);
    if (rc) { printf("virgl_renderer_init failed rc=%d\n", rc); return 2; }
    printf("P2 cross-context ordering: %dx%d, depth=%d, trials=%d\n\n", W, H, DEPTH, TRIALS);

    if (virgl_renderer_context_create(CTX_A, 4, "ctxA") ||
        virgl_renderer_context_create(CTX_B, 4, "ctxB") ||
        mkres(RES_SRC) || mkres(RES_DST)) {
        printf("setup failed\n"); return 2;
    }
    virgl_renderer_ctx_attach_resource(CTX_A, RES_SRC);
    virgl_renderer_ctx_attach_resource(CTX_B, RES_DST);
    virgl_renderer_ctx_attach_resource(CTX_B, RES_SRC);   /* P1b: this is what permits the blit */

    /* CONTROL 1 -- can the probe report a CLEAN run? If the synced arm
     * mismatches, something other than ordering is wrong. */
    const int ctln = TRIALS < 40 ? TRIALS : 40;
    int ctl = run_arm("SYNCED", ARM_SYNCED, ctln);
    if (ctl != 0) {
        printf("\nINSTRUMENT FAILURE: the SYNCED arm mismatched %d times.\n"
               "Something other than cross-context ordering is wrong (encoding,\n"
               "surface reuse, readback). No P2 verdict from this run.\n", ctl);
        virgl_renderer_cleanup(&cookie);
        return 3;
    }
    printf("             clean -- encoding, blit and readback all sound\n\n");

    /* CONTROL 2 -- can the probe report a DIRTY run? A clean UNSYNCED result is
     * equally consistent with "ordering holds" and "this probe cannot see a
     * stale read", and separating those is the entire job. The inverted arm
     * blits BEFORE the clear, so staleness is guaranteed by construction and
     * every trial MUST mismatch. */
    /* Seed the SOURCE to a colour trial 0 does not expect. The inverted arm
     * blits first, so what it reads back is whatever RES_SRC held BEFORE the
     * trial's clear -- and the SYNCED arm's last trial left GREEN there, which
     * is exactly what trial 0 expects. It matched by coincidence, scoring
     * 39/40.
     *
     * Seeding the DESTINATION (the first attempt) changed nothing, and the
     * failure was informative: the blit overwrites the destination wholesale,
     * so the destination's prior contents can never be observed. Staleness in
     * this probe is always a property of the SOURCE. Restoring the strict
     * "every trial must mismatch" bar this way is stronger than relaxing it to
     * n-1, which would have papered over a real one-trial blind spot. */
    c_reset();
    cmd_surface(SURF_A + 2, RES_SRC);
    cmd_framebuffer(SURF_A + 2);
    cmd_clear(1, 0, 0);                        /* RED; trial 0 expects GREEN */
    (void)c_submit(CTX_A);
    (void)read_px(RES_SRC, CTX_A);             /* land it before the arm starts */

    int inv = run_arm("INVERTED", ARM_INVERTED, ctln);
    if (inv < ctln) {
        printf("\nINSTRUMENT FAILURE: the INVERTED arm should mismatch on EVERY\n"
               "trial (%d) and mismatched %d. The probe cannot reliably see a\n"
               "stale read, so a clean UNSYNCED result would prove nothing.\n"
               "No P2 verdict from this run.\n", ctln, inv);
        virgl_renderer_cleanup(&cookie);
        return 4;
    }
    printf("             all %d mismatched as required -- the probe CAN see a stale read\n\n", inv);

    int bad = run_arm("UNSYNCED", ARM_UNSYNCED, TRIALS);

    printf("\n=== P2 VERDICT ===\n");
    if (bad == 0) {
        printf("  NO REORDERING OBSERVED in %d unsynced trials at %dx%d depth %d.\n"
               "  Evidence that the ordering holds on THIS stack for THIS pattern --\n"
               "  NOT a proof: a race that did not reproduce is not a race that\n"
               "  cannot happen. C-1 still models the hazard; this bounds how hard\n"
               "  it is to hit, and gives the spec a measured starting point.\n",
               TRIALS, W, H, DEPTH);
    } else {
        printf("  REORDERING OBSERVED: %d/%d unsynced trials read the wrong frame,\n"
               "  while the synced control was clean. The hazard is REAL -- the blit\n"
               "  does NOT observe the client's finished frame, so removing the\n"
               "  readback in C-4 would ship an I-40 tearing violation.\n"
               "  This is C-1's counterexample; model it before building.\n", bad, TRIALS);
    }
    virgl_renderer_cleanup(&cookie);
    return bad ? 1 : 0;
}
