// /pouch-hello-fd -- the fd-surface prover (B-3a; patch 0051).
//
// Runs POST-PIVOT (spawned as /bin/pouch-hello-fd) so the file legs land on
// the persistent Stratum FS, where fsync is a real Tsync. Legs, each printed +
// boot-fatal via the joey expect string:
//
//   fsync         : fsync and fdatasync of a written file answer 0 (0001 parked
//                   both at the ENOSYS sentinel); a pipe, whose Dev has nothing
//                   to synchronize, answers EINVAL (so a stub answering 0 is
//                   red); fsync(-1) is EBADF.
//   fsync-sock    : a socket answers EINVAL (nothing to synchronize); a closed
//                   socket's number EBADF.
//   writev        : three pieces written by writev read back, by pread, as one
//                   run at the file offset writev advanced.
//   writev-atomic : on a nonblocking pipe with two bytes free, a four-byte
//                   writev of two two-byte pieces answers EAGAIN and writes
//                   NOTHING -- one gathered write, which POSIX makes atomic at
//                   or under PIPE_BUF. A write per piece would land "ab".
//   readv         : ten bytes in a pipe scatter across three iovecs in order.
//                   A byte source cannot tell one gathered read from a read
//                   per iovec; the read half of "one transfer" has no witness
//                   here (a record source -- a datagram -- would be one).
//   heap          : a 3 MiB two-piece pwritev, resumed after each short count
//                   (one call moves at most one buffer, and the kernel and the
//                   9P client cut a transfer shorter still), reads back
//                   byte-exact through preadv, resumed the same way.
//   iov-errnos    : a negative count, a count past IOV_MAX, lengths summing
//                   past SSIZE_MAX and a negative preadv offset are EINVAL.
//   v2            : preadv2 and pwritev2 without flags are preadv and pwritev:
//                   offset -1 writes at the cursor and advances it, offset 0
//                   reads without moving it, offset -2 is EINVAL; a flag
//                   (RWF_HIPRI) is ENOSYS, as the kernel has none.
//   sock-seek     : lseek, pread and pwrite on a socket are ESPIPE; a negative
//                   pread or pwrite offset on a socket EINVAL, as Linux checks
//                   the offset before the descriptor.
//   sock-fstat    : fstat of a socket is S_ISSOCK on st_dev all bits set (a
//                   devno the kernel never issues: static Devs report 0 and
//                   its counter starts at 1), two sockets differ in st_ino,
//                   and a closed socket's number is EBADF.
//
// Every leg runs, and each opens the probe file itself, so a red run names
// each failing leg and only those; the census line prints only when all pass.
// fd 1 is a pipe write-end joey relays to the boot log, line-buffered so a leg
// that crashes is preceded by every earlier leg's line.
// Cross-compiled with tools/pouch-clang against the pouch sysroot.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pouch-census.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <unistd.h>

#define PROBE "/pouch-fd-probe.bin"

// SYS_SET_NONBLOCK (kernel/include/thylacine/syscall.h): fcntl(F_SETFL) does
// not reach it from pouch yet, so the atomic leg asks the kernel directly.
#define THYLA_SYS_SET_NONBLOCK 123

static int fail(const char *leg) {
    printf("pouch-hello-fd: FAIL %s (errno %d)\n", leg, errno);
    return 1;
}

// errno after a call that must answer -1 with `want`.
static int refused(long rc, int want) {
    return rc == -1 && errno == want;
}

static int leg_fsync(void) {
    int fd = open(PROBE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("fsync open");
    if (write(fd, "durable\n", 8) != 8) return fail("fsync write");
    if (fsync(fd) != 0) return fail("fsync");
    if (fdatasync(fd) != 0) return fail("fdatasync");
    close(fd);
    int p[2];
    if (pipe(p) != 0) return fail("fsync pipe");
    errno = 0;
    if (!refused(fsync(p[1]), EINVAL)) return fail("fsync(pipe) not EINVAL");
    close(p[0]);
    close(p[1]);
    errno = 0;
    if (!refused(fsync(-1), EBADF)) return fail("fsync(-1) not EBADF");
    return 0;
}

static int leg_fsync_sock(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return fail("fsync-sock socket");
    errno = 0;
    if (!refused(fsync(s), EINVAL)) return fail("fsync(sock) not EINVAL");
    errno = 0;
    if (!refused(fdatasync(s), EINVAL)) return fail("fdatasync(sock) not EINVAL");
    close(s);
    errno = 0;
    if (!refused(fsync(s), EBADF)) return fail("fsync(closed sock) not EBADF");
    return 0;
}

static int leg_writev(void) {
    int fd = open(PROBE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("writev open");
    if (write(fd, "<<", 2) != 2) return fail("writev lead");
    struct iovec v[3] = {
        { (void *)"alpha-", 6 }, { (void *)"beta-", 5 }, { (void *)"gamma", 5 },
    };
    if (writev(fd, v, 3) != 16) return fail("writev count");
    if (lseek(fd, 0, SEEK_CUR) != 18) return fail("writev offset");
    char got[32] = {0};
    if (pread(fd, got, sizeof got, 0) != 18) return fail("writev pread");
    if (memcmp(got, "<<alpha-beta-gamma", 18) != 0) return fail("writev bytes");
    close(fd);
    return 0;
}

static int leg_writev_atomic(void) {
    int p[2];
    if (pipe(p) != 0) return fail("atomic pipe");
    if (syscall(THYLA_SYS_SET_NONBLOCK, p[1], 1) != 0) return fail("atomic nonblock");
    static char fill[4094];
    memset(fill, 'x', sizeof fill);
    if (write(p[1], fill, sizeof fill) != (ssize_t)sizeof fill) return fail("atomic fill");
    struct iovec v[2] = { { (void *)"ab", 2 }, { (void *)"cd", 2 } };
    errno = 0;
    ssize_t w = writev(p[1], v, 2);
    if (w == 2) return fail("atomic: writev split into one write per piece");
    if (!refused(w, EAGAIN)) return fail("atomic writev not EAGAIN");
    close(p[1]);
    static char drain[8192];
    size_t total = 0;
    ssize_t r;
    while ((r = read(p[0], drain + total, sizeof drain - total)) > 0) total += (size_t)r;
    if (total != sizeof fill) return fail("atomic: bytes beyond the fill");
    close(p[0]);
    return 0;
}

static int leg_readv(void) {
    int p[2];
    if (pipe(p) != 0) return fail("readv pipe");
    if (write(p[1], "0123456789", 10) != 10) return fail("readv fill");
    char a[3], b[3], c[8];
    struct iovec v[3] = { { a, 3 }, { b, 3 }, { c, 8 } };
    if (readv(p[0], v, 3) != 10) return fail("readv count");
    if (memcmp(a, "012", 3) || memcmp(b, "345", 3) || memcmp(c, "6789", 4))
        return fail("readv scatter");
    close(p[0]);
    close(p[1]);
    return 0;
}

// Points v[0..cnt) past the first `skip` bytes; answers the first element
// still holding any.
static int iov_skip(struct iovec *v, int cnt, size_t skip) {
    int i = 0;
    while (i < cnt && skip >= v[i].iov_len) skip -= v[i++].iov_len;
    if (i < cnt) {
        v[i].iov_base = (char *)v[i].iov_base + skip;
        v[i].iov_len -= skip;
    }
    return i;
}

static int leg_heap(void) {
    const size_t half = 3u << 19;   // 1.5 MiB: two pieces, 3 MiB in all
    unsigned char *a = malloc(half), *b = malloc(half);
    unsigned char *ra = malloc(half), *rb = malloc(half);
    if (!a || !b || !ra || !rb) return fail("heap malloc");
    for (size_t i = 0; i < half; i++) {
        a[i] = (unsigned char)(i * 7u + 1u);
        b[i] = (unsigned char)(i * 13u + 5u);
    }
    int fd = open(PROBE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("heap open");
    size_t put = 0;
    while (put < 2 * half) {
        struct iovec w[2] = { { a, half }, { b, half } };
        int i = iov_skip(w, 2, put);
        ssize_t n = pwritev(fd, w + i, 2 - i, 4096 + (off_t)put);
        if (n <= 0) return fail("heap pwritev");
        put += (size_t)n;
    }
    size_t got = 0;
    while (got < 2 * half) {
        struct iovec r[2] = { { ra, half }, { rb, half } };
        int i = iov_skip(r, 2, got);
        ssize_t n = preadv(fd, r + i, 2 - i, 4096 + (off_t)got);
        if (n <= 0) return fail("heap preadv");
        got += (size_t)n;
    }
    if (memcmp(a, ra, half) || memcmp(b, rb, half)) return fail("heap bytes");
    close(fd);
    free(a); free(b); free(ra); free(rb);
    return 0;
}

static int leg_iov_errnos(void) {
    int fd = open(PROBE, O_RDWR | O_CREAT, 0644);
    if (fd < 0) return fail("errnos open");
    char x;
    struct iovec one = { &x, 1 };
    errno = 0;
    if (!refused(readv(fd, &one, -1), EINVAL)) return fail("readv count -1");
    errno = 0;
    if (!refused(writev(fd, &one, IOV_MAX + 1), EINVAL)) return fail("writev count IOV_MAX+1");
    struct iovec huge[2] = { { &x, (size_t)SSIZE_MAX }, { &x, 2 } };
    errno = 0;
    if (!refused(writev(fd, huge, 2), EINVAL)) return fail("writev sum past SSIZE_MAX");
    errno = 0;
    if (!refused(preadv(fd, &one, 1, -1), EINVAL)) return fail("preadv offset -1");
    close(fd);
    return 0;
}

static int leg_v2(void) {
    int fd = open(PROBE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("v2 open");
    if (write(fd, "<<", 2) != 2) return fail("v2 lead");
    struct iovec w[2] = { { (void *)"left-", 5 }, { (void *)"right", 5 } };
    if (pwritev2(fd, w, 2, -1, 0) != 10) return fail("pwritev2 at the cursor");
    if (lseek(fd, 0, SEEK_CUR) != 12) return fail("pwritev2(-1) left the cursor");
    char a[4], b[8];
    struct iovec r[2] = { { a, 4 }, { b, 8 } };
    if (preadv2(fd, r, 2, 0, 0) != 12) return fail("preadv2 at 0");
    if (memcmp(a, "<<le", 4) || memcmp(b, "ft-right", 8)) return fail("preadv2 bytes");
    if (lseek(fd, 0, SEEK_CUR) != 12) return fail("preadv2(0) moved the cursor");
    errno = 0;
    if (!refused(preadv2(fd, r, 2, -2, 0), EINVAL)) return fail("preadv2 offset -2 not EINVAL");
    errno = 0;
    if (!refused(pwritev2(fd, w, 2, -2, 0), EINVAL)) return fail("pwritev2 offset -2 not EINVAL");
    errno = 0;
    if (!refused(preadv2(fd, r, 2, 0, RWF_HIPRI), ENOSYS)) return fail("preadv2 RWF_HIPRI not ENOSYS");
    close(fd);
    return 0;
}

static int leg_sock_seek(void) {
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return fail("seek socket");
    char x = 0;
    errno = 0;
    if (!refused(lseek(s, 0, SEEK_SET), ESPIPE)) return fail("lseek(sock) not ESPIPE");
    errno = 0;
    if (!refused(pread(s, &x, 1, 0), ESPIPE)) return fail("pread(sock) not ESPIPE");
    errno = 0;
    if (!refused(pwrite(s, &x, 1, 0), ESPIPE)) return fail("pwrite(sock) not ESPIPE");
    errno = 0;
    if (!refused(pread(s, &x, 1, -1), EINVAL)) return fail("pread(sock, -1) not EINVAL");
    errno = 0;
    if (!refused(pwrite(s, &x, 1, -1), EINVAL)) return fail("pwrite(sock, -1) not EINVAL");
    close(s);
    return 0;
}

static int leg_sock_fstat(void) {
    int s1 = socket(AF_UNIX, SOCK_STREAM, 0);
    int s2 = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s1 < 0 || s2 < 0) return fail("fstat sockets");
    struct stat a, b;
    if (fstat(s1, &a) != 0 || fstat(s2, &b) != 0) return fail("fstat(sock)");
    if (!S_ISSOCK(a.st_mode) || !S_ISSOCK(b.st_mode)) return fail("fstat not S_ISSOCK");
    if (a.st_dev != (dev_t)-1 || b.st_dev != (dev_t)-1)
        return fail("fstat socket st_dev not all bits set");
    if (a.st_ino == b.st_ino) return fail("fstat sockets share st_ino");
    close(s1);
    close(s2);
    errno = 0;
    if (!refused(fstat(s1, &a), EBADF)) return fail("fstat(closed sock) not EBADF");
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    static const struct { const char *name; int (*run)(void); } legs[] = {
        { "fsync", leg_fsync },           { "fsync-sock", leg_fsync_sock },
        { "writev", leg_writev },         { "writev-atomic", leg_writev_atomic },
        { "readv", leg_readv },           { "heap", leg_heap },
        { "iov-errnos", leg_iov_errnos }, { "v2", leg_v2 },
        { "sock-seek", leg_sock_seek },   { "sock-fstat", leg_sock_fstat },
    };
    int failed = 0;
    for (size_t i = 0; i < sizeof legs / sizeof legs[0]; i++) {
        if (legs[i].run()) failed++;
        else printf("pouch-hello-fd: %s OK\n", legs[i].name);
    }
    unlink(PROBE);
    if (failed) {
        printf("pouch-hello-fd: %d leg(s) FAILED\n", failed);
        return 1;
    }
    puts(POUCH_CENSUS_FD);
    return 0;
}
