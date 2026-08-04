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
 * "AUTH failed" without it names the step but not the reason.
 *
 * The demo credentials are the same ones osmesa-prove uses; this is a prover
 * path, not a login path. A real GL application would redeem a clearance its
 * own session already holds. */
#define THYLA_CAPJIT_OK        0
#define THYLA_CAPJIT_ENOSRV    11 /* /srv/corvus unreachable        */
#define THYLA_CAPJIT_ENOCTL    12 /* corvus ctl would not open      */
#define THYLA_CAPJIT_EAUTH     13 /* AUTH refused                   */
#define THYLA_CAPJIT_ECLEAR    14 /* CLEARANCE_ACTIVATE(jit) refused*/
#define THYLA_CAPJIT_EGRANT    15 /* granted mask was not CAP_JIT   */
#define THYLA_CAPJIT_EREDEEM   16 /* SYS_CAP_USE refused the grant  */

static int thyla_acquire_cap_jit(const char *who)
{
    static const char PASS[] = "correct-horse-battery-staple-v1";
    unsigned char pl[128], rx[256], token[33];
    unsigned long long granted = 0;
    size_t o, rl, pw;
    unsigned char st;
    long root = -1, ctl;
    int i, rc;

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

    /* AUTH. */
    o = 0;
    pl[o++] = 7;
    memcpy(pl + o, "michael", 7);
    o += 7;
    pw = sizeof(PASS) - 1;
    pl[o++] = (unsigned char)(pw & 0xff);
    pl[o++] = (unsigned char)((pw >> 8) & 0xff);
    memcpy(pl + o, PASS, pw);
    o += pw;
    rc = thyla_capjit_rpc((int)ctl, 1, pl, o, rx, sizeof rx, &rl, &st);
    if (rc != 0 || st != 0 || rl != sizeof token) {
        printf("%s: FAIL corvus AUTH rc=%d st=%u rl=%u\n", who, rc, (unsigned)st,
               (unsigned)rl);
        return THYLA_CAPJIT_EAUTH;
    }
    memcpy(token, rx, sizeof token);

    /* CLEARANCE_ACTIVATE("jit"). CLEARANCE_LIST is skipped on purpose --
     * ACTIVATE fails identically if the principal is not eligible, and
     * /jit-prover exercises the listing machinery exhaustively in the same
     * boot. */
    o = 0;
    memcpy(pl + o, token, sizeof token);
    o += sizeof token;
    pl[o++] = 3;
    memcpy(pl + o, "jit", 3);
    o += 3;
    memset(pl + o, 0, 16); /* self_restrict, valid_until_req */
    o += 16;
    rc = thyla_capjit_rpc((int)ctl, 15, pl, o, rx, sizeof rx, &rl, &st);
    if (rc != 0 || st != 0 || rl != 12) {
        printf("%s: FAIL corvus CLEARANCE rc=%d st=%u rl=%u\n", who, rc,
               (unsigned)st, (unsigned)rl);
        return THYLA_CAPJIT_ECLEAR;
    }
    for (i = 0; i < 8; i++) {
        granted |= (unsigned long long)rx[4 + i] << (8 * i);
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
     * carries time_bound_ns 0, so nothing expires underneath us. */
    printf("%s: CAP_JIT acquired via the corvus jit clearance\n", who);
    fflush(stdout);
    return THYLA_CAPJIT_OK;
}

#endif /* THYLA_CAPJIT_H */
