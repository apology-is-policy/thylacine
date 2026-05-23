// /pouch-hello-getrandom — exercising the libsodium CSPRNG path
// (Phase 6 sub-chunk 11 — pouch-devnodes).
//
// POUCH-DESIGN.md §6.6 names "the getrandom-syscall path libsodium
// needs" as a deliverable of this sub-chunk. SYS_GETRANDOM (= 20) and
// the kern_random_bytes core landed in P5-corvus-syscalls; the pouch
// libc binding works automatically because the 0001 syscall-seam patch
// maps __NR_getrandom = 20 (musl's src/linux/getrandom.c is unchanged).
//
// This proving binary calls musl's getrandom(2) at the libc surface and
// verifies the round-trip. Without CAP_CSPRNG_READ joey-side, the kernel
// gates the call (-1, errno=EIO via syscall_ret's flat-EIO decode), so
// joey spawns this child with the cap explicitly via t_spawn_full —
// proving that the libsodium-grade caller's grant story works.
//
// Three probes:
//   1. getrandom(buf, 32, 0) returns 32           — the basic call works.
//   2. at least one of the 32 bytes is non-zero   — RNDR delivers entropy
//                                                   (P(all zero) = 2^-256).
//   3. two consecutive 16-byte reads differ       — basic CSPRNG sanity
//                                                   (P(equal) = 2^-128).
//
// Output:
//   pouch-hello-getrandom: getrandom(32) -> 32 (ok)
//   pouch-hello-getrandom: nonzero entropy observed (ok)
//   pouch-hello-getrandom: two reads differ (ok)
//   pouch-hello-getrandom: exit 0
//
// Non-zero return on any failed assertion — joey treats as a regression.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>

int main(void) {
    unsigned char buf[32];
    for (int i = 0; i < 32; i++) buf[i] = 0xAB;   // poison

    ssize_t got = getrandom(buf, sizeof(buf), 0);
    if (got != (ssize_t)sizeof(buf)) {
        printf("pouch-hello-getrandom: FAIL getrandom got=%zd errno=%d\n",
               got, errno);
        fflush(stdout);
        return 1;
    }
    printf("pouch-hello-getrandom: getrandom(32) -> 32 (ok)\n");
    fflush(stdout);

    int any_nonzero = 0;
    for (int i = 0; i < 32; i++) if (buf[i] != 0) { any_nonzero = 1; break; }
    if (!any_nonzero) {
        printf("pouch-hello-getrandom: FAIL all-zero 32-byte read\n");
        fflush(stdout);
        return 2;
    }
    printf("pouch-hello-getrandom: nonzero entropy observed (ok)\n");
    fflush(stdout);

    unsigned char a[16], b[16];
    if (getrandom(a, sizeof(a), 0) != (ssize_t)sizeof(a) ||
        getrandom(b, sizeof(b), 0) != (ssize_t)sizeof(b)) {
        printf("pouch-hello-getrandom: FAIL 16-byte refill\n");
        fflush(stdout);
        return 3;
    }
    if (memcmp(a, b, sizeof(a)) == 0) {
        printf("pouch-hello-getrandom: FAIL two reads identical\n");
        fflush(stdout);
        return 4;
    }
    printf("pouch-hello-getrandom: two reads differ (ok)\n");
    fflush(stdout);

    printf("pouch-hello-getrandom: exit 0\n");
    fflush(stdout);
    return 0;
}
