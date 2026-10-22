// /pouch-hello-times -- the file-times prover (G12; patch 0063, operator votes
// 2026-10-22: SYS_WSTAT's time bits).
//
// Runs POST-PIVOT (spawned as /bin/pouch-hello-times) in its own directory at
// the pool root, which the boot-chain identity owns, so every request here is
// the owner's. The refusals of authority -- EPERM for a non-owner, EACCES for a
// touch without write permission -- are the kernel tests' (perm.wstat_times,
// dev9p.wstat_times_permission) and the host test's (test-pouch-times.py) to
// prove. Each leg sets an old time first, so a call that did nothing cannot
// pass. Legs, each printed + boot-fatal via the joey expect string:
//
//   explicit   : utimensat sets both times; stat reads them back, in seconds
//                (the kernel's stat carries no nanoseconds).
//   omit       : UTIME_OMIT leaves one time and sets the other.
//   both-omit  : two UTIME_OMIT succeed without looking the path up, even for
//                a path that does not exist, and change nothing.
//   now        : null times, and two UTIME_NOW, take the file server's clock:
//                within a few seconds of the guest's.
//   futimens   : the descriptor form. Every descriptor leg opens for writing:
//                a read-only open of a small file the kernel holds (this
//                one, just written) is the fidless cached open, which takes
//                no wstat (FID-LIFECYCLE-DESIGN 3.3; its EOPNOTSUPP is
//                pinned by the dev9p kernel tests).
//   utimes     : utimes, futimes, utime and lutimes reach the same call.
//   empty-path : AT_EMPTY_PATH with "" names the descriptor, and with
//                AT_FDCWD the working directory; a directory takes times.
//   nofollow   : AT_SYMLINK_NOFOLLOW sets a dangling link's own times (lstat
//                reads them) where following it is ENOENT.
//   errors     : a nanosecond count out of range and an unknown flag are
//                EINVAL, a null path EFAULT, a closed descriptor EBADF, a
//                missing file ENOENT, a real dirfd with a path ENOTSUP (P-3).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include "pouch-census.h"

#define WORK "/pouch-times"
#define FILE_A WORK "/a"
#define DEAD "/lib/pouch-links/dead"

// Two times well before any boot: a leg that changed nothing reads them back.
#define T_OLD 1000000000L
#define T_NEW 1100000000L

static int fail(const char *what) {
    printf("pouch-hello-times: FAIL %s (errno %d)\n", what, errno);
    return 1;
}

static int set_both(const char *path, long at, long mt) {
    struct timespec ts[2] = { { at, 0 }, { mt, 0 } };
    return utimensat(AT_FDCWD, path, ts, 0);
}

// 0 iff path's times read back as (at, mt).
static int times_are(const char *path, long at, long mt) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (st.st_atime == at && st.st_mtime == mt) ? 0 : 1;
}

static int leg_explicit(void) {
    struct timespec ts[2] = { { T_OLD, 123 }, { T_NEW, 456 } };
    if (utimensat(AT_FDCWD, FILE_A, ts, 0) != 0) return fail("utimensat explicit");
    if (times_are(FILE_A, T_OLD, T_NEW) != 0) return fail("explicit times read back");
    if (set_both(FILE_A, T_NEW, T_OLD) != 0) return fail("utimensat explicit, swapped");
    if (times_are(FILE_A, T_NEW, T_OLD) != 0) return fail("swapped times read back");
    return 0;
}

static int leg_omit(void) {
    if (set_both(FILE_A, T_OLD, T_OLD) != 0) return fail("reset");
    struct timespec keep_a[2] = { { 0, UTIME_OMIT }, { T_NEW, 0 } };
    if (utimensat(AT_FDCWD, FILE_A, keep_a, 0) != 0) return fail("atime OMIT");
    if (times_are(FILE_A, T_OLD, T_NEW) != 0) return fail("atime OMIT moved atime or not mtime");
    struct timespec keep_m[2] = { { T_NEW, 0 }, { 0, UTIME_OMIT } };
    if (utimensat(AT_FDCWD, FILE_A, keep_m, 0) != 0) return fail("mtime OMIT");
    if (times_are(FILE_A, T_NEW, T_NEW) != 0) return fail("mtime OMIT moved mtime or not atime");
    return 0;
}

static int leg_both_omit(void) {
    if (set_both(FILE_A, T_OLD, T_NEW) != 0) return fail("reset");
    struct timespec none[2] = { { 0, UTIME_OMIT }, { 0, UTIME_OMIT } };
    if (utimensat(AT_FDCWD, FILE_A, none, 0) != 0) return fail("both OMIT");
    if (utimensat(AT_FDCWD, WORK "/nosuch", none, 0) != 0) return fail("both OMIT, missing path");
    if (times_are(FILE_A, T_OLD, T_NEW) != 0) return fail("both OMIT changed a time");
    return 0;
}

// Both times within slack of the guest's clock around the call.
static int near_now(const char *path, time_t t0, const char *what) {
    struct stat st;
    if (stat(path, &st) != 0) return fail(what);
    time_t t1 = time(NULL);
    if (st.st_atime < t0 - 2 || st.st_atime > t1 + 2 || st.st_mtime < t0 - 2 || st.st_mtime > t1 + 2) {
        printf("pouch-hello-times: %s: atime %lld mtime %lld, clock %lld..%lld\n", what,
               (long long)st.st_atime, (long long)st.st_mtime, (long long)t0, (long long)t1);
        errno = 0;
        return fail(what);
    }
    return 0;
}

static int leg_now(void) {
    if (set_both(FILE_A, T_OLD, T_OLD) != 0) return fail("reset");
    time_t t0 = time(NULL);
    if (utimensat(AT_FDCWD, FILE_A, NULL, 0) != 0) return fail("null times");
    if (near_now(FILE_A, t0, "null times is not now")) return 1;
    if (set_both(FILE_A, T_OLD, T_OLD) != 0) return fail("reset");
    struct timespec now[2] = { { 0, UTIME_NOW }, { 0, UTIME_NOW } };
    t0 = time(NULL);
    if (utimensat(AT_FDCWD, FILE_A, now, 0) != 0) return fail("two UTIME_NOW");
    return near_now(FILE_A, t0, "two UTIME_NOW is not now");
}

static int leg_futimens(void) {
    int fd = open(FILE_A, O_RDWR);
    if (fd < 0) return fail("open for writing");
    struct timespec ts[2] = { { T_NEW, 0 }, { T_OLD, 0 } };
    int r = futimens(fd, ts);
    int e = errno;
    struct stat st;
    int sr = fstat(fd, &st);
    close(fd);
    errno = e;
    if (r != 0) return fail("futimens");
    if (sr != 0 || st.st_atime != T_NEW || st.st_mtime != T_OLD) return fail("futimens times read back");
    return 0;
}

static int leg_utimes(void) {
    struct timeval tv[2] = { { T_OLD, 0 }, { T_NEW, 0 } };
    if (utimes(FILE_A, tv) != 0) return fail("utimes");
    if (times_are(FILE_A, T_OLD, T_NEW) != 0) return fail("utimes times read back");
    int fd = open(FILE_A, O_RDWR);
    if (fd < 0) return fail("open");
    struct timeval tv2[2] = { { T_NEW, 0 }, { T_OLD, 0 } };
    int r = futimes(fd, tv2);
    close(fd);
    if (r != 0) return fail("futimes");
    if (times_are(FILE_A, T_NEW, T_OLD) != 0) return fail("futimes times read back");
    struct utimbuf ub = { .actime = T_OLD, .modtime = T_OLD };
    if (utime(FILE_A, &ub) != 0) return fail("utime");
    if (times_are(FILE_A, T_OLD, T_OLD) != 0) return fail("utime times read back");
    if (lutimes(FILE_A, tv) != 0) return fail("lutimes of a file");
    if (times_are(FILE_A, T_OLD, T_NEW) != 0) return fail("lutimes times read back");
    return 0;
}

static int leg_empty_path(void) {
    int fd = open(FILE_A, O_RDWR);
    if (fd < 0) return fail("open");
    struct timespec ts[2] = { { T_NEW, 0 }, { T_NEW, 0 } };
    int r = utimensat(fd, "", ts, AT_EMPTY_PATH);
    close(fd);
    if (r != 0) return fail("AT_EMPTY_PATH on a descriptor");
    if (times_are(FILE_A, T_NEW, T_NEW) != 0) return fail("AT_EMPTY_PATH times read back");
    struct timespec dts[2] = { { T_OLD, 0 }, { T_NEW, 0 } };
    if (utimensat(AT_FDCWD, "", dts, AT_EMPTY_PATH) != 0) return fail("AT_EMPTY_PATH on the working directory");
    if (times_are(WORK, T_OLD, T_NEW) != 0) return fail("the working directory's times read back");
    return 0;
}

static int leg_nofollow(void) {
    struct stat st;
    if (lstat(DEAD, &st) != 0 || !S_ISLNK(st.st_mode)) return fail("the fixture's dangling link");
    struct timespec ts[2] = { { T_OLD, 0 }, { T_NEW, 0 } };
    errno = 0;
    if (utimensat(AT_FDCWD, DEAD, ts, 0) != -1 || errno != ENOENT)
        return fail("following a dangling link is not ENOENT");
    if (utimensat(AT_FDCWD, DEAD, ts, AT_SYMLINK_NOFOLLOW) != 0) return fail("AT_SYMLINK_NOFOLLOW on a link");
    if (lstat(DEAD, &st) != 0 || st.st_atime != T_OLD || st.st_mtime != T_NEW)
        return fail("the link's own times read back");
    struct timeval tv[2] = { { T_NEW, 0 }, { T_OLD, 0 } };
    if (lutimes(DEAD, tv) != 0) return fail("lutimes on a link");
    if (lstat(DEAD, &st) != 0 || st.st_atime != T_NEW || st.st_mtime != T_OLD)
        return fail("lutimes' times read back");
    return 0;
}

static int expect_errno(int r, int want, const char *what) {
    if (r != -1 || errno != want) {
        printf("pouch-hello-times: %s: r %d errno %d, want errno %d\n", what, r, errno, want);
        return 1;
    }
    return 0;
}

static int leg_errors(void) {
    if (set_both(FILE_A, T_NEW, T_NEW) != 0) return fail("reset");
    int bad = 0;
    struct timespec big[2] = { { T_OLD, 1000000000L }, { T_OLD, 0 } };
    struct timespec neg[2] = { { T_OLD, 0 }, { T_OLD, -1 } };
    struct timespec ts[2] = { { T_OLD, 0 }, { T_OLD, 0 } };
    errno = 0;
    bad += expect_errno(utimensat(AT_FDCWD, FILE_A, big, 0), EINVAL, "nsec 1e9");
    errno = 0;
    bad += expect_errno(utimensat(AT_FDCWD, FILE_A, neg, 0), EINVAL, "nsec -1");
    errno = 0;
    bad += expect_errno(utimensat(AT_FDCWD, FILE_A, ts, AT_REMOVEDIR), EINVAL, "an unknown flag");
    errno = 0;
    bad += expect_errno(utimensat(AT_FDCWD, NULL, ts, 0), EFAULT, "a null path");
    errno = 0;
    bad += expect_errno(utimensat(AT_FDCWD, WORK "/nosuch", ts, 0), ENOENT, "a missing file");
    int fd = open(FILE_A, O_RDONLY);
    if (fd < 0) return fail("open");
    close(fd);
    errno = 0;
    bad += expect_errno(futimens(fd, ts), EBADF, "a closed descriptor");
    int dfd = open(WORK, O_RDONLY | O_DIRECTORY);
    if (dfd < 0) return fail("open the directory");
    errno = 0;
    int r = utimensat(dfd, "a", ts, 0);
    int e = errno;
    close(dfd);
    errno = e;
    bad += expect_errno(r, ENOTSUP, "a real dirfd with a path");
    if (times_are(FILE_A, T_NEW, T_NEW) != 0) return fail("a refused call changed the times");
    return bad ? 1 : 0;
}

static void clean(void) {
    unlink(FILE_A);
    rmdir(WORK);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    clean();
    if (mkdir(WORK, 0755) != 0) return fail("mkdir " WORK);
    int fd = open(FILE_A, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("create " FILE_A);
    if (write(fd, "times\n", 6) != 6) return fail("write " FILE_A);
    close(fd);
    if (chdir(WORK) != 0) return fail("chdir " WORK);
    static const struct { const char *name; int (*run)(void); } legs[] = {
        { "explicit", leg_explicit },     { "omit", leg_omit },
        { "both-omit", leg_both_omit },   { "now", leg_now },
        { "futimens", leg_futimens },     { "utimes", leg_utimes },
        { "empty-path", leg_empty_path }, { "nofollow", leg_nofollow },
        { "errors", leg_errors },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; i++) {
        if (legs[i].run()) failed++;
        else printf("pouch-hello-times: %s OK\n", legs[i].name);
    }
    if (chdir("/") != 0) failed++;
    clean();
    if (failed) {
        printf("pouch-hello-times: %d leg(s) FAILED\n", failed);
        return 1;
    }
    puts(POUCH_CENSUS_TIMES);
    return 0;
}
