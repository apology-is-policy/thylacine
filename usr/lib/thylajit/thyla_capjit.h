/* thyla_capjit.h -- acquire CAP_JIT by walking the corvus clearance path.
 *
 * `CAP_JIT` is ELEVATION-ONLY: it is stripped at every fork, so no parent can
 * hand it to a child at spawn and no amount of privilege in joey helps. The
 * only way any Proc ever holds it is to walk the corvus clearance path *itself*
 * and redeem the grant. That is the concrete sense in which a GL program on
 * Thylacine is a capability client rather than an ordinary binary: llvmpipe
 * JITs inside the calling process, so every GL program must do this before it
 * touches a context.
 *
 * Header-only, freestanding-ish (raw `svc` for the two syscalls musl does not
 * carry), so a pouch C program can `#include` it and call
 * `thyla_acquire_cap_jit()` once at startup.
 *
 * THIS IS A SECOND COPY. The first lives inside `osmesa_prove.c` in the Mesa
 * fork (usr/ports/mesa/patches/0005), and it has to: that tree is built on the
 * GCP builder with no include path into this repo, so it cannot share a header
 * with us. The duplication is deliberate and bounded -- if the corvus wire
 * protocol or the clearance name changes, BOTH copies move. Grep for
 * `CLEARANCE_ACTIVATE` to find them.
 *
 * #139 did NOT move the fork's copy, and that is correct rather than an
 * oversight: verb 18 is an ADDITION, verb 15 is byte-unchanged, and
 * osmesa-prove runs at boot as PRINCIPAL_SYSTEM where only the bearer form can
 * work anyway. The fork's copy keeps taking the path it always took. It should
 * be brought forward when it is next rebuilt for some other reason -- not
 * sooner, since a builder round costs real money and buys nothing here.
 */
#ifndef THYLA_CAPJIT_H
#define THYLA_CAPJIT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define THYLA_CAP_JIT     (1ull << 11)
#define THYLA_SYS_CAP_USE 33
#define THYLA_SYS_OPEN    65
#define THYLA_FROM_ROOT   (-1L)
#define THYLA_OREAD       0
#define THYLA_ORDWR       2

/* Reaching corvus needs SYS_OPEN (65), NOT SYS_WALK_OPEN (34): 34 walks
 * exactly one path component, so it cannot resolve "/srv/corvus". Measured,
 * not assumed -- with 34 the open failed every retry while jit-prover
 * connected fine in the same boot. */
static long thyla_capjit_open(long dirfd, const char *name, unsigned long len,
                              unsigned long omode)
{
    register long x0 __asm__("x0") = dirfd;
    register long x1 __asm__("x1") = (long)(uintptr_t)name;
    register long x2 __asm__("x2") = (long)len;
    register long x3 __asm__("x3") = (long)omode;
    register long x8 __asm__("x8") = THYLA_SYS_OPEN;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3), "r"(x8)
                     : "memory", "cc");
    return x0;
}

static long thyla_capjit_cap_use(unsigned long long mask)
{
    register long x0 __asm__("x0") = (long)mask;
    register long x8 __asm__("x8") = THYLA_SYS_CAP_USE;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory", "cc");
    return x0;
}

static int thyla_capjit_rd(int fd, void *p, size_t n)
{
    unsigned char *b = (unsigned char *)p;
    size_t off = 0;
    while (off < n) {
        ssize_t r = read(fd, b + off, n - off);
        if (r <= 0) {
            return -1;
        }
        off += (size_t)r;
    }
    return 0;
}

static int thyla_capjit_wr(int fd, const void *p, size_t n)
{
    const unsigned char *b = (const unsigned char *)p;
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w <= 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/* One corvus request/response. Request: verb, version, len16 LE, payload.
 * Response: status, len16 LE, payload. */
static int thyla_capjit_rpc(int fd, unsigned char verb, const unsigned char *pl,
                            size_t pl_len, unsigned char *rx, size_t rx_cap,
                            size_t *rx_len, unsigned char *status)
{
    unsigned char hdr[4], rh[3];
    size_t n;

    hdr[0] = verb;
    hdr[1] = 1; /* protocol version */
    hdr[2] = (unsigned char)(pl_len & 0xff);
    hdr[3] = (unsigned char)((pl_len >> 8) & 0xff);
    if (thyla_capjit_wr(fd, hdr, sizeof hdr) != 0) {
        return -1;
    }
    if (pl_len && thyla_capjit_wr(fd, pl, pl_len) != 0) {
        return -1;
    }
    if (thyla_capjit_rd(fd, rh, sizeof rh) != 0) {
        return -1;
    }
    n = (size_t)rh[1] | ((size_t)rh[2] << 8);
    if (n > rx_cap) {
        return -1;
    }
    if (n && thyla_capjit_rd(fd, rx, n) != 0) {
        return -1;
    }
    *rx_len = n;
    *status = rh[0];
    return 0;
}

/* Returns 0 when this Proc holds CAP_JIT, else a DISTINCT non-zero per
 * station (THYLA_CAPJIT_E*). The station must be readable from the return
 * value and not only from stdout: the console truncates short lines
 * nondeterministically (task #95 -- observed losing 2, 0 and 4 bytes off the
 * end of this very function's AUTH message across three identical runs), so a
 * diagnosis that lives only in printf output is a diagnosis that sometimes
 * does not arrive. The status byte corvus returned is printed too, because
 * "AUTH failed" without it names the step but not the reason. */
#define THYLA_CAPJIT_OK        0
#define THYLA_CAPJIT_ENOSRV    11 /* /srv/corvus unreachable        */
#define THYLA_CAPJIT_ENOCTL    12 /* corvus ctl would not open      */
#define THYLA_CAPJIT_EAUTH     13 /* AUTH refused                   */
#define THYLA_CAPJIT_ECLEAR    14 /* CLEARANCE_ACTIVATE(jit) refused*/
#define THYLA_CAPJIT_EGRANT    15 /* granted mask was not CAP_JIT   */
#define THYLA_CAPJIT_EREDEEM   16 /* SYS_CAP_USE refused the grant  */

/* One CLEARANCE_ACTIVATE, either form. `token` NULL selects the #139 SELF form
 * (verb 18, no bearer secret -- corvus authorizes the connection's own
 * kernel-stamped principal); non-NULL selects the bearer form (verb 15).
 * Returns 0 and fills *granted on OK. */
static int thyla_capjit_activate(int ctl, const unsigned char *token,
                                 unsigned long long *granted, unsigned char *st)
{
    unsigned char pl[128], rx[256];
    size_t o = 0, rl;
    int i, rc;

    if (token) {
        memcpy(pl, token, 33);
        o = 33;
    }
    pl[o++] = 3;
    memcpy(pl + o, "jit", 3);
    o += 3;
    memset(pl + o, 0, 16); /* self_restrict, valid_until_req */
    o += 16;

    rc = thyla_capjit_rpc(ctl, token ? 15 : 18, pl, o, rx, sizeof rx, &rl, st);
    if (rc != 0 || *st != 0 || rl != 12) {
        return -1;
    }
    *granted = 0;
    for (i = 0; i < 8; i++) {
        *granted |= (unsigned long long)rx[4 + i] << (8 * i);
    }
    return 0;
}

/* Held-once, PROCESS-wide -- and deliberately a weak GLOBAL rather than a
 * function-static.
 *
 * Since GLQuake this header is included by more than one TU of the same
 * program: libSDL2.a's GL backend acquires the capability so that STOCK
 * SDL-GL programs need no Thylacine-specific code at all (LLVM-DESIGN section
 * 9 step 2), while gl-sdl-prove also calls it directly, because its gate
 * asserts WHICH form won and only the direct call prints that. A
 * function-static would be per-TU and let the walk run twice in that binary --
 * a second corvus round-trip whose only two possible outcomes are "the same
 * answer again" and "a NEW failure on a path that had already succeeded".
 *
 * A weak definition merges to ONE object across TUs, including when one of
 * them arrives from an archive (verified by compiling both topologies and
 * comparing the address, not assumed -- a weak UNDEFINED reference behaves
 * nothing like a weak definition, and that distinction already cost #138 a
 * link that succeeded with an inert GL path). So the two callers compose in
 * either order: whichever runs first walks, the other sees it done.
 *
 * Not thread-safe and does not need to be: both call sites are startup, and
 * SDL_GL_CreateContext is documented main-thread-only. */
__attribute__((weak)) int thyla_capjit_held;

/* Acquire CAP_JIT, trying the SELF form before the bearer form.
 *
 * Both are needed, and which one works is a property of HOW THE CALLER WAS
 * SPAWNED, not of the caller's code:
 *
 *   - Post-login (a program the user ran): the kernel stamped this Proc with
 *     the user's principal at spawn (login uses SPAWN_IDENTITY_SET), so the
 *     SELF form authorizes it directly. No secret is quoted, sent, or held.
 *   - At boot (jit-prover, osmesa-prove, and anything else joey spawns with a
 *     plain t_spawn): the Proc runs as PRINCIPAL_SYSTEM, which is not a corvus
 *     user and has no eligibility. The SELF form correctly refuses it; the
 *     bearer form is the bootstrap path, and its demo credentials are exactly
 *     that -- a prover's credentials, not a login.
 *
 * The fallback cannot mask a real failure, which is why it is safe to have:
 * both forms end in the SAME eligibility + auth_required checks against the
 * same level, so anything that refuses on policy grounds refuses BOTH. The
 * only failures the fallback converts are the two identity mismatches -- "you
 * are not the session user" and "no session is bound" -- and in exactly those
 * cases the bearer form is the correct next question to ask. On a double
 * failure both statuses are reported; one alone would name the wrong door. */
static int thyla_acquire_cap_jit(const char *who)
{
    static const char PASS[] = "correct-horse-battery-staple-v1";
    unsigned char pl[128], rx[256], token[33];
    unsigned long long granted = 0;
    unsigned char st_self = 0, st = 0;
    size_t o, rl, pw;
    long root = -1, ctl;
    int i, rc, self_ok;

    if (thyla_capjit_held) {
        return THYLA_CAPJIT_OK;
    }

    for (i = 0; i < 16 && root < 0; i++) {
        root = thyla_capjit_open(THYLA_FROM_ROOT, "/srv/corvus", 11,
                                 THYLA_OREAD);
    }
    if (root < 0) {
        printf("%s: FAIL cannot reach /srv/corvus\n", who);
        return THYLA_CAPJIT_ENOSRV;
    }
    ctl = thyla_capjit_open(root, "ctl", 3, THYLA_ORDWR);
    close((int)root);
    if (ctl < 0) {
        printf("%s: FAIL cannot open corvus ctl\n", who);
        return THYLA_CAPJIT_ENOCTL;
    }

    /* The SELF form first -- the one that quotes nothing. */
    self_ok = (thyla_capjit_activate((int)ctl, NULL, &granted, &st_self) == 0);

    if (!self_ok) {
        /* AUTH, then the bearer form. CLEARANCE_LIST is skipped on purpose --
         * ACTIVATE fails identically if the principal is not eligible, and
         * /jit-prover exercises the listing machinery exhaustively in the same
         * boot. */
        o = 0;
        pl[o++] = 7;
        memcpy(pl + o, "michael", 7);
        o += 7;
        pw = sizeof(PASS) - 1;
        pl[o++] = (unsigned char)(pw & 0xff);
        pl[o++] = (unsigned char)((pw >> 8) & 0xff);
        memcpy(pl + o, PASS, pw);
        o += pw;
        /* The numbers come FIRST. #95 truncates line TAILS, and on the very
         * run this was written for it ate the tail of this exact message --
         * "FAIL corvus AU" -- destroying the only evidence that distinguished
         * "SELF was refused, here is why" from "the transport broke". A
         * diagnostic whose discriminator sits after the prose is a diagnostic
         * that sometimes is not there. */
        rc = thyla_capjit_rpc((int)ctl, 1, pl, o, rx, sizeof rx, &rl, &st);
        if (rc != 0 || st != 0 || rl != sizeof token) {
            printf("%s: FAIL self=%u auth=%u rc=%d rl=%u corvus AUTH\n", who,
                   (unsigned)st_self, (unsigned)st, rc, (unsigned)rl);
            return THYLA_CAPJIT_EAUTH;
        }
        memcpy(token, rx, sizeof token);

        if (thyla_capjit_activate((int)ctl, token, &granted, &st) != 0) {
            printf("%s: FAIL self=%u clear=%u corvus CLEARANCE\n", who,
                   (unsigned)st_self, (unsigned)st);
            return THYLA_CAPJIT_ECLEAR;
        }
    }

    if (granted != THYLA_CAP_JIT) {
        printf("%s: FAIL clearance granted 0x%llx, want CAP_JIT\n", who,
               granted);
        return THYLA_CAPJIT_EGRANT;
    }
    if (thyla_capjit_cap_use(granted) != 0) {
        printf("%s: FAIL redeeming the CAP_JIT grant\n", who);
        return THYLA_CAPJIT_EREDEEM;
    }
    /* ctl is deliberately left open for the life of the process, as
     * /jit-prover does: the capability is already ours (a legate scope is torn
     * down by the holder's own death, not by a session close) and the jit level
     * carries time_bound_ns 0, so nothing expires underneath us.
     *
     * WHICH form won is reported, and reported EARLY in the line: a gate that
     * only sees "CAP_JIT acquired" cannot tell the #139 path from the boot path
     * that always worked, and #95 eats line TAILS -- a discriminator parked at
     * the end is a discriminator that sometimes is not there. */
    thyla_capjit_held = 1;
    printf("%s: CAP_JIT acquired (%s) via the corvus jit clearance\n", who,
           self_ok ? "SELF" : "bearer");
    fflush(stdout);
    return THYLA_CAPJIT_OK;
}

#endif /* THYLA_CAPJIT_H */
