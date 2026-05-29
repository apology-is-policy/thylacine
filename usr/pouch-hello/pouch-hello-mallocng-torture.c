// /pouch-hello-mallocng-torture -- reproduce the mallocng 128 KiB corruption
// (the "AEGIS-256 corruption" / STM_EBADTAG) in isolation, the EBADTAG DFS.
//
// The native burrow-torture proved the kernel burrow path CLEAN (single-threaded
// + SMP), so the corruption is musl mallocng-specific. stratumd's btree-node
// decrypt does malloc(STM_BTNODE_SIZE = 131072) -> read into it -> decrypt; the
// corruption surfaces post-fill, in mallocng's slot metadata, on a "second
// 128 KiB alloc/free cycle" (sizeclass-63, just past MMAP_THRESHOLD = 131052 ->
// mallocng's individually-mmapped path over SYS_BURROW_ATTACH).
//
// This binary replicates that pattern with NO Stratum/crypto: aggressive
// malloc(131072)/fill/verify/free cycles, interleaved second-cycle allocs and a
// small alloc to perturb the meta area, plus a pthread variant (stratumd is
// multi-threaded). Detectors: (1) a data verify mismatch -> printf REPRODUCED;
// (2) mallocng's own integrity assert -> _Exit(127) (per 0012-pouch-mallocng-
// crash.patch). When built with POUCH_MALLOCNG_DIAG=1 the assert path also dumps
// sizeclass/idx/maplen/stride + the corrupt p[-8..8] bytes (to fd 2) to pinpoint.
//
// fd 1 is joey's relay pipe -> boot-log UART. "mallocng-torture: ALL OK" = clean.

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BIG    131072u   // = STM_BTNODE_SIZE, just past MMAP_THRESHOLD (131052)
#define SMALL  4096u

static int fail(const char *what) {
    printf("mallocng-torture: FAIL %s\n", what);
    fflush(stdout);
    return 1;
}

static void fillb(unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)((seed * 0x9eu + i) & 0xffu);
}

static int checkb(const unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)((seed * 0x9eu + i) & 0xffu))
            return 0;
    return 1;
}

// stratumd-shaped single-threaded torture: 128K alloc + FULL fill (like the
// btree-node memcpy), then a second-cycle alloc/free + cross-verify, plus a
// small alloc to perturb mallocng's meta area. A verify mismatch OR mallocng's
// own assert (-> _Exit 127) means the corruption reproduced.
static int st_torture(unsigned iters) {
    for (unsigned i = 0; i < iters; i++) {
        unsigned char *a = malloc(BIG);
        if (!a) return fail("malloc a");
        fillb(a, BIG, i * 3u + 1u);
        unsigned char *b = malloc(BIG);
        if (!b) return fail("malloc b");
        fillb(b, BIG, i * 3u + 2u);
        if (!checkb(a, BIG, i * 3u + 1u)) return fail("a corrupted after b alloc");
        free(a);                          // free A -> its mmap'd slot returns
        unsigned char *c = malloc(BIG);   // the "second 128 KiB cycle"
        if (!c) return fail("malloc c");
        fillb(c, BIG, i * 3u + 3u);
        if (!checkb(b, BIG, i * 3u + 2u)) return fail("b corrupted by second-cycle alloc");
        if (!checkb(c, BIG, i * 3u + 3u)) return fail("c corrupted");
        unsigned char *s = malloc(SMALL); // perturb the meta area
        if (s) {
            fillb(s, SMALL, i);
            if (!checkb(s, SMALL, i)) return fail("small corrupted");
            free(s);
        }
        free(b);
        free(c);
    }
    return 0;
}

#define NTHREADS 4u
#define MT_ITERS 24u
static volatile int g_mt_fail = 0;

static void *mt_worker(void *arg) {
    unsigned tid = (unsigned)(uintptr_t)arg;
    for (unsigned i = 0; i < MT_ITERS && !g_mt_fail; i++) {
        unsigned seed = (tid << 20) | (i & 0xFFFFFu); // thread-unique
        unsigned char *p = malloc(BIG);
        if (!p) { g_mt_fail = 1; return NULL; }
        fillb(p, BIG, seed);
        if (!checkb(p, BIG, seed)) { g_mt_fail = 1; free(p); return NULL; }
        free(p);
    }
    return NULL;
}

int main(void) {
    printf("mallocng-torture: start (128 KiB malloc/free corruption repro; sc=63 mmap path)\n");
    fflush(stdout);

    if (st_torture(24) != 0) {
        printf("mallocng-torture: single-threaded REPRODUCED\n");
        fflush(stdout);
        return 1;
    }
    printf("mallocng-torture: single-threaded OK (24 iters)\n");
    fflush(stdout);

    pthread_t th[NTHREADS];
    for (unsigned t = 0; t < NTHREADS; t++) {
        if (pthread_create(&th[t], NULL, mt_worker, (void *)(uintptr_t)t) != 0)
            return fail("pthread_create");
    }
    for (unsigned t = 0; t < NTHREADS; t++)
        pthread_join(th[t], NULL);
    if (g_mt_fail) {
        printf("mallocng-torture: multi-threaded REPRODUCED\n");
        fflush(stdout);
        return 1;
    }
    printf("mallocng-torture: multi-threaded OK (4 threads x 24 iters)\n");
    fflush(stdout);

    printf("mallocng-torture: ALL OK\n");
    fflush(stdout);
    return 0;
}
