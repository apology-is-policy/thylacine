// /pouch-hello-malloc — the heap-exercising pouch hello (Phase 6 sub-chunk 7b).
//
// First POSIX C program Thylacine runs that exercises dynamic memory. It
// drives mallocng (musl's allocator) through every code path that the
// 0003-pouch-mman boundary-line patch routes onto SYS_BURROW_ATTACH /
// SYS_BURROW_DETACH:
//
//   - small malloc/free                       (slot allocation; the
//                                              metadata-area mmap path
//                                              triggers on first call)
//   - calloc                                  (zero-init via the freshly-
//                                              demand-zero attach; mallocng
//                                              relies on the kernel zeroing)
//   - realloc-grow within a slot              (no remap; same slot)
//   - large malloc/free (> MMAP_THRESHOLD)    (mallocng's individually-
//                                              mmapped path — a direct
//                                              attach for the whole region)
//   - large realloc-grow                      (mremap returns ENOSYS via
//                                              the 0xFFFF sentinel guard;
//                                              mallocng falls through to
//                                              malloc + memcpy + free)
//
// Every write reads back as written, so this is also the first userspace
// confirmation that the kernel's demand-page user-fault path (page-tables
// installed on first touch over an eagerly-allocated Burrow's pages —
// docs/reference/79-sys-burrow.md) is sound.
//
// MMAP_THRESHOLD in mallocng is 131052 bytes (src/malloc/mallocng/meta.h);
// we pick 256 KiB for the large size to land well past the threshold.
//
// On any byte-level mismatch the program returns non-zero — joey treats
// that as a boot regression. fd 1 is a pipe write-end joey relays to the
// boot-log UART and content-checks for the "exit 0" marker.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LARGE_BYTES   (256u * 1024u)   // > MMAP_THRESHOLD (131052)
#define LARGE_REGROWN (LARGE_BYTES + 65536u)
#define SMALL_BYTES   64u

static int fail(const char *what) {
    printf("pouch-hello-malloc: FAIL %s\n", what);
    return 1;
}

// fill p[0..n) with a deterministic byte sequence keyed by `seed`. Cheap
// arithmetic on unsigned, well-defined wrap on the byte cast.
static void fill(unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)((seed * 0x9eu + i) & 0xffu);
}

// confirm p[0..n) matches the sequence fill() wrote with the same seed.
static int check(const unsigned char *p, size_t n, unsigned seed) {
    for (size_t i = 0; i < n; i++)
        if (p[i] != (unsigned char)((seed * 0x9eu + i) & 0xffu))
            return 0;
    return 1;
}

int main(void) {
    printf("pouch-hello-malloc: heap exercise (mallocng over SYS_BURROW_ATTACH)\n");

    // 1. small malloc/free — first call also triggers mallocng's first
    //    metadata-area mmap (the brk fallback path; see 0003 patch
    //    preamble's mallocng-correctness notes).
    unsigned char *a = malloc(SMALL_BYTES);
    if (!a) return fail("malloc(SMALL)");
    fill(a, SMALL_BYTES, 1);
    if (!check(a, SMALL_BYTES, 1)) return fail("small read-back");
    free(a);

    // 2. calloc — pages must be zero. mallocng relies on the kernel
    //    handing out demand-zero pages (which our SYS_BURROW_ATTACH does;
    //    burrow_create_anon zeros the pages it allocates).
    unsigned char *b = calloc(SMALL_BYTES, 16);
    if (!b) return fail("calloc");
    for (size_t i = 0; i < (size_t)SMALL_BYTES * 16; i++)
        if (b[i] != 0) return fail("calloc not zero-initialized");
    free(b);

    // 3. realloc-grow within a slot — mallocng fits the new size into the
    //    same slot and returns the same pointer (no underlying remap).
    unsigned char *c = malloc(SMALL_BYTES);
    if (!c) return fail("malloc(SMALL) for realloc");
    fill(c, SMALL_BYTES, 2);
    unsigned char *c2 = realloc(c, SMALL_BYTES * 4);
    if (!c2) return fail("realloc grow small");
    if (!check(c2, SMALL_BYTES, 2)) return fail("realloc-small lost data");
    free(c2);

    // 4. large malloc/free — > MMAP_THRESHOLD pushes mallocng down the
    //    individually-mmapped allocation path: one SYS_BURROW_ATTACH for
    //    the whole region, one SYS_BURROW_DETACH on free. Touch every
    //    page (LARGE > one page).
    unsigned char *d = malloc(LARGE_BYTES);
    if (!d) return fail("malloc(LARGE)");
    fill(d, LARGE_BYTES, 3);
    if (!check(d, LARGE_BYTES, 3)) return fail("large read-back");
    free(d);

    // 5. large realloc-grow — mallocng tries mremap, gets MAP_FAILED +
    //    ENOSYS (the sentinel guard), falls through to
    //    malloc + memcpy + free at realloc.c:46. Data must survive.
    unsigned char *e = malloc(LARGE_BYTES);
    if (!e) return fail("malloc(LARGE) for realloc");
    fill(e, LARGE_BYTES, 4);
    unsigned char *e2 = realloc(e, LARGE_REGROWN);
    if (!e2) return fail("realloc grow large");
    if (!check(e2, LARGE_BYTES, 4)) return fail("realloc-large lost data");
    free(e2);

    printf("pouch-hello-malloc: small malloc/free ok\n");
    printf("pouch-hello-malloc: calloc zeroing ok\n");
    printf("pouch-hello-malloc: realloc-grow (small slot) ok\n");
    printf("pouch-hello-malloc: large malloc/free ok (> MMAP_THRESHOLD)\n");
    printf("pouch-hello-malloc: realloc-grow large ok (mremap ENOSYS -> malloc+memcpy+free)\n");
    printf("pouch-hello-malloc: exit 0\n");
    return 0;
}
