// /pouch-hello-mem -- the B-1b prover of the Pouch memory seam (patches 0044,
// 0045, 0046; ARCHITECTURE.md sec 6.5 "The permission ceiling" / "Capacity";
// POUCH-DESIGN.md sec 8.1). Twelve legs. Every leg runs even after an earlier
// one failed, so one boot reports them all; the census line (what joey
// matches) prints only when all twelve passed.
//
// The bar is the operator's: a program is never refused memory while free
// memory exists, and memory it relinquishes returns to the system so its
// footprint shrinks at runtime -- on the Pouch substrate as on the native one.
// The footprint is read from the kernel's own census, the data view of
// /proc/<pid>/status (pages: minus tables: minus file:), never inferred from
// what libc believes it did.
//
// Output stays well inside joey's 2048-byte capture window; stdout is line
// buffered so a leg that dies mid-way leaves its predecessors' lines behind.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pouch-census.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PG 4096ul

// A VA no mapping of this process reaches: the top GiB of the kernel's user
// burrow window [0x100000000, 0x400000000000) (kernel/include/thylacine/
// exec.h), which the first-fit allocator (vma_find_gap) hands out last. Every
// leg that expects ENOMEM here first passes hole_is_a_hole(), the control that
// /proc/<pid>/maps shows no row covering it.
#define HOLE ((void *)0x3fffc0000000ul)

// The mallocng-trim floors, pinned under the measurement. N blocks of SZ bytes
// are ~10 pages each, so the class's groups hold ~4000 pages; freeing every
// other block leaves every group retained, and only mallocng's MADV_FREE of
// the whole pages inside each freed slot (free.c, USE_MADV_FREE) can make the
// count fall -- ~9 pages per slot. MEASURED 2026-09-23 (identical at -smp 4
// and -smp 1): before=11 full=4263 half-freed=2339 all-freed=11 -- a fall of
// 1924 pages at the half, and everything back at the end. On a libc built
// with USE_MADV_FREE 0 the half-freed figure stays within ~100 of the full
// one (the early single-slot groups), far under the 1200 floor.
#define TRIM_N            400
#define TRIM_SZ           40000
#define TRIM_RISE_FLOOR   3000
#define TRIM_HALF_FLOOR   1200
#define TRIM_LO_SLACK     64

static int g_fails;

static void ok(const char *leg) {
    printf("pouch-hello-mem: %s ok\n", leg);
}

static void bad(const char *leg, const char *what) {
    printf("pouch-hello-mem: %s FAIL: %s (errno %d)\n", leg, what, errno);
    g_fails++;
}

#define CHECK(cond, what) do { if (!(cond)) { bad(LEG, what); return; } } while (0)

static int field_after(const char *buf, const char *key, unsigned long *out) {
    const char *p = strstr(buf, key);
    if (!p) return -1;
    p += strlen(key);
    while (*p == ' ') p++;
    if (*p < '0' || *p > '9') return -1;
    unsigned long v = 0;
    while (*p >= '0' && *p <= '9') v = v * 10ul + (unsigned long)(*p++ - '0');
    *out = v;
    return 0;
}

// The data view of the kernel's holder count for this address space: pages:
// minus the page tables and the FILE pages charged beside them (kernel/
// devproc.c format_status). -1 on any miss, never a guess.
static long data_pages(void) {
    char path[64], buf[2048];
    snprintf(path, sizeof path, "/proc/%d/status", (int)getpid());
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    unsigned long pages = 0, tables = 0, file = 0;
    if (field_after(buf, "\npages:", &pages) != 0 ||
        field_after(buf, "\ntables:", &tables) != 0 ||
        field_after(buf, "\nfile:", &file) != 0)
        return -1;
    return (long)pages - (long)tables - (long)file;
}

static int hole_is_a_hole(void) {
    char path[64], line[256];
    snprintf(path, sizeof path, "/proc/%d/maps", (int)getpid());
    FILE *mf = fopen(path, "r");
    if (!mf) return 0;
    int rows = 0, covered = 0;
    while (fgets(line, sizeof line, mf)) {
        unsigned long a = 0, b = 0;
        if (sscanf(line, "%lx-%lx", &a, &b) != 2) continue;
        rows++;
        if (a <= (unsigned long)HOLE && (unsigned long)HOLE < b) covered = 1;
    }
    fclose(mf);
    return rows > 0 && !covered;
}

// 1. A mapping minted PROT_NONE is raised, lowered and raised again: the prot
//    asked for is the prot minted (0003 minted RW whatever was asked), and a
//    byte written under RW survives the lowering to R.
static void leg_prot_ladder(void) {
#define LEG "prot-ladder"
    char *p = mmap(0, 3 * PG, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED, "mmap PROT_NONE");
    volatile char *v = (volatile char *)p + PG;
    CHECK(mprotect(p + PG, PG, PROT_READ | PROT_WRITE) == 0, "raise to RW");
    v[0] = 0x5a;
    CHECK(v[0] == 0x5a, "write then read under RW");
    CHECK(mprotect(p + PG, PG, PROT_READ) == 0, "lower to R");
    CHECK(v[0] == 0x5a, "the byte survives the lowering");
    CHECK(mprotect(p + PG, PG, PROT_READ | PROT_WRITE) == 0, "raise to RW again");
    v[0] = 0x3c;
    CHECK(v[0] == 0x3c, "write after the second raise");
    CHECK(munmap(p, 3 * PG) == 0, "munmap");
    ok(LEG);
#undef LEG
}

// 2. Executable anonymous memory is refused at the mint and at the raise, with
//    EACCES, never degraded to RW (I-12; the seam passes the kernel's answer).
static void leg_x_refused(void) {
#define LEG "x-refused"
    char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED, "mmap RW");
    errno = 0;
    CHECK(mprotect(p, PG, PROT_READ | PROT_EXEC) == -1 && errno == EACCES, "mprotect RX is EACCES");
    errno = 0;
    void *q = mmap(0, PG, PROT_READ | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(q == MAP_FAILED && errno == EACCES, "mmap RX is EACCES");
    CHECK(munmap(p, PG) == 0, "munmap");
    ok(LEG);
#undef LEG
}

// 3. mprotect's Linux answers: a zero length is 0 before any call, a hole is
//    ENOMEM, a prot bit the seam does not know is EINVAL.
static void leg_prot_errnos(void) {
#define LEG "prot-errnos"
    char *p = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED, "mmap RW");
    CHECK(mprotect(p, 0, PROT_READ) == 0, "a length of 0 is 0");
    errno = 0;
    CHECK(mprotect(HOLE, PG, PROT_READ) == -1 && errno == ENOMEM, "a hole is ENOMEM");
    errno = 0;
    CHECK(mprotect(p, PG, 0x10) == -1 && errno == EINVAL, "an unknown prot bit is EINVAL");
    CHECK(munmap(p, PG) == 0, "munmap");
    ok(LEG);
#undef LEG
}

// 4/5. The bar itself: 4 MiB touched page by page raises the data view by the
//    pages touched; madvise(DONTNEED / FREE) returns them and the view falls
//    back; the mapping stays and a released page re-faults zero.
static void leg_madvise_release(const char *leg, int advice) {
#define LEG leg
    long p0 = data_pages();
    CHECK(p0 >= 0, "status read");
    size_t len = 4ul << 20;
    char *m = mmap(0, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED, "mmap 4 MiB");
    volatile char *v = (volatile char *)m;
    for (size_t i = 0; i < len; i += PG) v[i] = 1;
    long p1 = data_pages();
    CHECK(p1 - p0 >= (long)(len / PG), "the data view rose by the pages touched");
    CHECK(madvise(m, len, advice) == 0, "madvise");
    long p2 = data_pages();
    printf("pouch-hello-mem: %s pages before=%ld touched=%ld released=%ld\n", leg, p0, p1, p2);
    CHECK(p2 <= p0 + 8, "the data view fell back");
    CHECK(v[0] == 0, "a released page re-faults zero");
    CHECK(munmap(m, len) == 0, "munmap");
    ok(leg);
#undef LEG
}

// 6. The pure hints answer 0 (Thylacine has no readahead / THP / dump policy
//    for them to steer), through madvise and posix_madvise alike.
static void leg_madvise_hint(void) {
#define LEG "madvise-hint"
    char *m = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED, "mmap");
    CHECK(madvise(m, PG, MADV_WILLNEED) == 0, "WILLNEED is 0");
    CHECK(madvise(m, PG, MADV_SEQUENTIAL) == 0, "SEQUENTIAL is 0");
    CHECK(posix_madvise(m, PG, POSIX_MADV_WILLNEED) == 0, "posix_madvise WILLNEED is 0");
    CHECK(munmap(m, PG) == 0, "munmap");
    ok(LEG);
#undef LEG
}

// 7. madvise's Linux answers: an unaligned start and an unknown advice are
//    EINVAL, a release on a hole is ENOMEM, a zero length is 0.
static void leg_madvise_errnos(void) {
#define LEG "madvise-errnos"
    char *m = mmap(0, PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED, "mmap");
    errno = 0;
    CHECK(madvise(m + 1, PG, MADV_DONTNEED) == -1 && errno == EINVAL, "an unaligned start is EINVAL");
    errno = 0;
    CHECK(madvise(m, PG, 99) == -1 && errno == EINVAL, "an unknown advice is EINVAL");
    errno = 0;
    CHECK(madvise(HOLE, PG, MADV_DONTNEED) == -1 && errno == ENOMEM, "a release on a hole is ENOMEM");
    CHECK(madvise(m, 0, MADV_DONTNEED) == 0, "a length of 0 is 0");
    CHECK(munmap(m, PG) == 0, "munmap");
    ok(LEG);
#undef LEG
}

// 8. The range detach (B-1a'): the middle two of four pages go, the end pages
//    keep their bytes, the middle is a hole (mprotect says ENOMEM) and a second
//    unmap of it answers 0.
static void leg_partial_munmap(void) {
#define LEG "partial-munmap"
    char *m = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED, "mmap 4 pages");
    volatile char *v = (volatile char *)m;
    for (int i = 0; i < 4; i++) v[i * PG] = (char)('a' + i);
    CHECK(munmap(m + PG, 2 * PG) == 0, "unmap the middle two");
    CHECK(v[0] == 'a' && v[3 * PG] == 'd', "the end pages keep their bytes");
    errno = 0;
    CHECK(mprotect(m + PG, 2 * PG, PROT_READ) == -1 && errno == ENOMEM, "the middle is a hole");
    CHECK(munmap(m + PG, 2 * PG) == 0, "unmapping the hole answers 0");
    CHECK(munmap(m, PG) == 0 && munmap(m + 3 * PG, PG) == 0, "unmap the ends");
    ok(LEG);
#undef LEG
}

// 9. The engine idiom (JavaScriptCore's OSAllocator, browser-status F3): reserve
//    size + alignment PROT_NONE, trim both slack ends, commit an aligned piece.
static void leg_aligned_reserve(void) {
#define LEG "aligned-reserve"
    size_t want = 1ul << 20, align = 64ul << 10;
    char *r = mmap(0, want + align, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(r != MAP_FAILED, "reserve 1 MiB + 64 KiB");
    char *in = (char *)(((uintptr_t)r + align - 1) & ~(uintptr_t)(align - 1));
    size_t head = (size_t)(in - r), tail = align - head;
    CHECK(head == 0 || munmap(r, head) == 0, "trim the head slack");
    CHECK(tail == 0 || munmap(in + want, tail) == 0, "trim the tail slack");
    CHECK(mprotect(in, align, PROT_READ | PROT_WRITE) == 0, "commit the first 64 KiB");
    volatile char *v = (volatile char *)in;
    v[0] = 1;
    v[align - 1] = 2;
    CHECK(v[0] == 1 && v[align - 1] == 2, "write the committed piece");
    CHECK(munmap(in, want) == 0, "release the interior");
    ok(LEG);
#undef LEG
}

// 10. MAP_FIXED over one's own anonymous mapping (F5, a guard page placed by
//    address): the pages are discarded and re-minted at the new prot, the
//    neighbours keep their bytes; MAP_FIXED never CREATES a mapping (a hole is
//    ENOMEM) and MAP_FIXED_NOREPLACE is ENOSYS.
static void leg_map_fixed(void) {
#define LEG "map-fixed"
    char *m = mmap(0, 4 * PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(m != MAP_FAILED, "mmap 4 pages");
    volatile char *v = (volatile char *)m;
    for (int i = 0; i < 4; i++) v[i * PG] = (char)('A' + i);
    void *f = mmap(m + PG, 2 * PG, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(f == m + PG, "MAP_FIXED over one's own mapping returns the address");
    CHECK(v[0] == 'A' && v[3 * PG] == 'D', "the pages outside keep their bytes");
    CHECK(mprotect(m + PG, 2 * PG, PROT_READ | PROT_WRITE) == 0, "raise the re-minted pages");
    CHECK(v[PG] == 0 && v[2 * PG] == 0, "the re-minted pages were discarded");
    errno = 0;
    void *h = mmap(HOLE, PG, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(h == MAP_FAILED && errno == ENOMEM, "MAP_FIXED on a hole is ENOMEM");
    errno = 0;
    void *nr = mmap(m, PG, PROT_READ | PROT_WRITE, MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(nr == MAP_FAILED && errno == ENOSYS, "MAP_FIXED_NOREPLACE is ENOSYS");
    CHECK(munmap(m, 4 * PG) == 0, "munmap");
    ok(LEG);
#undef LEG
}

// 11. THE SUBSTRATE WITNESS (ARCH 6.5 "Capacity"): mallocng gives pages back
//    while its groups stay retained. RED on a libc built with USE_MADV_FREE 0.
static void leg_mallocng_trim(void) {
#define LEG "mallocng-trim"
    static void *blk[TRIM_N];
    long p0 = data_pages();
    CHECK(p0 >= 0, "status read");
    for (int i = 0; i < TRIM_N; i++) {
        blk[i] = malloc(TRIM_SZ);
        CHECK(blk[i] != NULL, "malloc");
        memset(blk[i], 0x11, TRIM_SZ);
    }
    long phi = data_pages();
    for (int i = 0; i < TRIM_N; i += 2) { free(blk[i]); blk[i] = NULL; }
    long pmid = data_pages();
    for (int i = 1; i < TRIM_N; i += 2) { free(blk[i]); blk[i] = NULL; }
    long plo = data_pages();
    printf("pouch-hello-mem: mallocng-trim pages before=%ld full=%ld half-freed=%ld all-freed=%ld\n",
           p0, phi, pmid, plo);
    CHECK(phi - p0 >= TRIM_RISE_FLOOR, "the heap's pages were charged");
    CHECK(phi - pmid >= TRIM_HALF_FLOOR, "freed slots inside retained groups returned their pages");
    CHECK(plo <= p0 + TRIM_LO_SLACK, "the emptied groups returned everything");
    ok(LEG);
#undef LEG
}

// 12. A 16 KiB static TLS: larger than musl's builtin_tls, so __init_tls maps
//    it -- through the seam's __mmap since patch 0046, which 0044 requires:
//    with __NR_mmap parked at the sentinel, upstream's raw six-argument call
//    gets ENOSYS and this program dies before main (the RED `notls`).
static __thread volatile char g_big[16384];

static void *tls_worker(void *arg) {
    (void)arg;
    g_big[0] = 7;
    g_big[sizeof g_big - 1] = 9;
    return (void *)(uintptr_t)(g_big[0] + g_big[sizeof g_big - 1]);
}

static void leg_tls(void) {
#define LEG "tls"
    g_big[0] = 1;
    g_big[sizeof g_big - 1] = 2;
    pthread_t t;
    void *r = NULL;
    CHECK(pthread_create(&t, NULL, tls_worker, NULL) == 0, "pthread_create");
    CHECK(pthread_join(t, &r) == 0, "pthread_join");
    CHECK((uintptr_t)r == 16, "the thread's own copy");
    CHECK(g_big[0] == 1 && g_big[sizeof g_big - 1] == 2, "the main thread's copy is untouched");
    ok(LEG);
#undef LEG
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!hole_is_a_hole()) {
        printf("pouch-hello-mem: FAIL: %p is covered by a mapping (or no maps rows)\n", HOLE);
        return 2;
    }
    leg_prot_ladder();
    leg_x_refused();
    leg_prot_errnos();
    leg_madvise_release("madvise-dontneed", MADV_DONTNEED);
    leg_madvise_release("madvise-free", MADV_FREE);
    leg_madvise_hint();
    leg_madvise_errnos();
    leg_partial_munmap();
    leg_aligned_reserve();
    leg_map_fixed();
    leg_mallocng_trim();
    leg_tls();
    if (g_fails) {
        printf("pouch-hello-mem: %d leg(s) FAILED\n", g_fails);
        return 1;
    }
    /* The census is what joey matches: a stale binary prints the old marker. */
    puts(POUCH_CENSUS_MEM);
    return 0;
}
