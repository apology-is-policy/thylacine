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
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ulimit.h>

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

// One read of /ctl/memory (kernel/devctl.c format_memory): three "key: N pages"
// lines. 1 on a full parse.
static int read_ctl_memory(unsigned long *total, unsigned long *kfree,
                           unsigned long *reserved) {
    FILE *mf = fopen("/ctl/memory", "r");
    if (!mf) return 0;
    int got = fscanf(mf, " total: %lu pages free: %lu pages reserved: %lu pages",
                     total, kfree, reserved);
    if (got != 3 || *total == 0)
        printf("pouch-hello-malloc: /ctl/memory fscanf=%d total=%lu eof=%d err=%d\n",
               got, *total, feof(mf), ferror(mf));
    fclose(mf);
    return got == 3 && *total != 0;
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

    // sysconf's memory figures must be the KERNEL's, not stack residue.
    // /ctl/memory is re-read HERE with a different parser (stdio), and the
    // total must match exactly -- a range check alone would pass on plausible
    // garbage. `free` moves, so it is BRACKETED: the kernel's figure is read
    // before and after the libc call and libc's must sit between them, give or
    // take a slack that a wrong key (total, reserved) cannot fall inside.
    {
        unsigned long total = 0, f1 = 0, f2 = 0, reserved = 0;
        if (!read_ctl_memory(&total, &f1, &reserved))
            return fail("parse /ctl/memory (first read)");
        long phys = sysconf(_SC_PHYS_PAGES);
        long avail = sysconf(_SC_AVPHYS_PAGES);
        unsigned long t2 = 0, r2 = 0;
        if (!read_ctl_memory(&t2, &f2, &r2))
            return fail("parse /ctl/memory (second read)");
        if (phys < 0 || (unsigned long)phys != total) {
            printf("pouch-hello-malloc: _SC_PHYS_PAGES=%ld but /ctl/memory total=%lu\n",
                   phys, total);
            return fail("sysconf(_SC_PHYS_PAGES) != kernel total");
        }
        unsigned long lo = f1 < f2 ? f1 : f2, hi = f1 < f2 ? f2 : f1;
        unsigned long slack = total / 16;
        if (reserved == 0 || reserved >= total || avail <= 0 ||
            (unsigned long)avail > total - reserved ||
            (unsigned long)avail + slack < lo || (unsigned long)avail > hi + slack) {
            printf("pouch-hello-malloc: _SC_AVPHYS_PAGES=%ld kernel free=%lu..%lu "
                   "total=%lu reserved=%lu\n", avail, lo, hi, total, reserved);
            return fail("sysconf(_SC_AVPHYS_PAGES) is not the kernel's free figure");
        }
        printf("pouch-hello-malloc: sysconf phys=%ld avail=%ld pages == /ctl/memory ok\n",
               phys, avail);
    }

    // The calls whose kernel side does not exist must SAY so. Each of these sat
    // on a wrapper the seam parks at ENOSYS, ignored its result, and returned
    // whatever was on the stack. errno is pre-loaded with a value none of them
    // sets, so "left as the caller had it" is checked, not assumed.
    {
        errno = EXDEV;
        long om = sysconf(_SC_OPEN_MAX);
        int e1 = errno;
        errno = EXDEV;
        long cm = sysconf(_SC_CHILD_MAX);
        int e2 = errno;
        if (om != -1 || cm != -1 || e1 != EXDEV || e2 != EXDEV) {
            printf("pouch-hello-malloc: _SC_OPEN_MAX=%ld errno=%d _SC_CHILD_MAX=%ld errno=%d\n",
                   om, e1, cm, e2);
            return fail("sysconf rlimit arm is not an honest -1");
        }
        double la[3] = { -7.0, -7.0, -7.0 };
        if (getloadavg(la, 3) != -1 || la[0] != -7.0)
            return fail("getloadavg invented a load figure");
        if (ulimit(UL_GETFSIZE) != -1)
            return fail("ulimit(UL_GETFSIZE) invented a limit");
        // errno pinned too: on the unfixed libc this ALSO returned -1 whenever
        // the stack residue held no NUL in 65 bytes -- with EINVAL.
        char dn[65];
        memset(dn, 'x', sizeof dn);
        errno = EXDEV;
        if (getdomainname(dn, sizeof dn) != -1 || errno != ENOSYS)
            return fail("getdomainname invented a name");
        // ualarm() read "the time left on the previous alarm" out of a struct
        // its failed setitimer() never wrote.
        errno = EXDEV;
        if (ualarm(1000, 0) != (useconds_t)-1 || errno != ENOSYS)
            return fail("ualarm invented a remaining time");

        // The CPU count is a real figure (the "cpus:" line of /ctl/sched), and
        // the call does not fail, so it must leave errno alone. Read here with
        // a different parser than libc's.
        {
            long want = 0;
            char key[32];
            FILE *sf = fopen("/ctl/sched", "r");
            if (!sf) return fail("open /ctl/sched");
            while (fscanf(sf, "%31s", key) == 1)
                if (!strcmp(key, "cpus:") && fscanf(sf, "%ld", &want) == 1) break;
            fclose(sf);
            errno = EXDEV;
            long onln = sysconf(_SC_NPROCESSORS_ONLN);
            long conf = sysconf(_SC_NPROCESSORS_CONF);
            if (want < 1 || onln != want || conf != want || errno != EXDEV) {
                printf("pouch-hello-malloc: nprocs onln=%ld conf=%ld errno=%d, /ctl/sched cpus=%ld\n",
                       onln, conf, errno, want);
                return fail("sysconf(_SC_NPROCESSORS_*) != the kernel's cpu count");
            }
        }

        // getdtablesize() has no error channel, so libc states the kernel's
        // handle-table size. Checked against the kernel, not against the same
        // number typed again: open until refused and count.
        int dts = getdtablesize();
        static int held[4096];
        int n = 0;
        while (n < (int)(sizeof held / sizeof held[0])) {
            int fd = open("/ctl/memory", O_RDONLY);
            if (fd < 0) break;
            held[n++] = fd;
        }
        int top = n ? held[n - 1] : -1;
        for (int i = 0; i < n; i++) close(held[i]);
        // The last fd issued is the table's last slot: size == top + 1.
        if (n == 0 || n == (int)(sizeof held / sizeof held[0]) || dts != top + 1) {
            printf("pouch-hello-malloc: getdtablesize=%d but the kernel issued %d fds, last=%d\n",
                   dts, n, top);
            return fail("getdtablesize != the kernel's handle table");
        }
        printf("pouch-hello-malloc: no-kernel-side calls honest; getdtablesize=%d == measured ok\n",
               dts);
    }

    printf("pouch-hello-malloc: small malloc/free ok\n");
    printf("pouch-hello-malloc: calloc zeroing ok\n");
    printf("pouch-hello-malloc: realloc-grow (small slot) ok\n");
    printf("pouch-hello-malloc: large malloc/free ok (> MMAP_THRESHOLD)\n");
    printf("pouch-hello-malloc: realloc-grow large ok (mremap ENOSYS -> malloc+memcpy+free)\n");
    // The census is what joey matches: a stale binary (the bake traps that
    // skip a populate) prints the old marker and must not pass for this one.
    printf("pouch-hello-malloc: legs=heap,physpages,nprocs,sentinel-wrappers,ualarm,dtablesize: exit 0\n");
    return 0;
}
