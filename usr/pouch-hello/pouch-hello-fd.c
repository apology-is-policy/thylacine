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
//   writev-atomic : on a pipe made nonblocking through the F_GETFL / F_SETFL
//                   idiom, with two bytes free, a four-byte writev of two
//                   two-byte pieces answers EAGAIN and writes NOTHING -- one
//                   gathered write, which POSIX makes atomic at or under
//                   PIPE_BUF. A write per piece would land "ab".
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
//   dup           : dup takes the LOWEST free number (a hole made below
//                   two fresh opens), names the SAME open file (a write
//                   through the copy moves the original's offset), never
//                   carries close-on-exec, and a closed or negative fd is
//                   EBADF.
//   dupfd         : F_DUPFD lands a copy of its source (a write through it
//                   moves the source's offset) at its floor when free and at
//                   the next free number above it when not; F_DUPFD_CLOEXEC
//                   sets the flag; the last slot is taken once, then EMFILE;
//                   a floor past the table or negative is EINVAL, but a
//                   closed fd is EBADF whatever the floor; dup2 and dup3 land
//                   on their target.
//   getfl         : F_GETFL reports exactly the access mode, O_APPEND and
//                   O_NONBLOCK; restating O_APPEND in F_SETFL is accepted,
//                   flipping it is EINVAL.
//   setfl         : F_SETFL O_NONBLOCK reads back on that end only, makes an
//                   empty pipe's read EAGAIN, and clearing it reads back
//                   clear; the flag belongs to the open file, so a dup sees
//                   it.
//   cloexec       : open O_CLOEXEC, fopen "e" and fdopen "e" set the flag and
//                   a plain open does not; freopen without a name keeps its
//                   descriptor (its F_SETFL used to answer ENOSYS and close
//                   the stream).
//   fdopendir     : fdopendir over an open directory reads its entries (its
//                   O_PATH test is F_GETFL, which used to answer -1).
//   umask         : umask answers the mask it replaces, 022 from boot (patch
//                   0058; it answered -1), and the kernel applies it: a create
//                   asking 0666 is the mode less the mask, bounded by the
//                   directory's (M8), each derived from the pool root's mode.
//                   A bit above 0777 is cut, never refused.
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
#include <sys/uio.h>
#include <dirent.h>
#include <unistd.h>

#define PROBE "/pouch-fd-probe.bin"
#define PROBE_NEW "/pouch-fd-probe-new.bin"

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
    if (fcntl(p[1], F_SETFL, fcntl(p[1], F_GETFL) | O_NONBLOCK) != 0)
        return fail("atomic nonblock");
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

// True when `fd` is not open -- each exact-number leg proves its slot free first.
static int is_free(int fd) {
    errno = 0;
    return refused(fcntl(fd, F_GETFD), EBADF);
}

static int leg_dup(void) {
    int fd = open(PROBE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("dup open");
    if (write(fd, "dup!", 4) != 4) return fail("dup write");
    int a = open(PROBE, O_RDONLY);
    int b = open(PROBE, O_RDONLY);
    if (a < 0 || b < 0) return fail("dup reopen");
    int hole = a < b ? a : b;
    int keep = a < b ? b : a;
    close(hole);
    int d = dup(fd);
    if (d != hole) return fail("dup did not take the lowest free number");
    if (write(d, "++", 2) != 2) return fail("dup write through the copy");
    if (lseek(fd, 0, SEEK_CUR) != 6) return fail("dup: not the same open file");
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return fail("dup flag the source");
    int d2 = dup(fd);
    if (d2 < 0) return fail("dup of a flagged fd");
    if (fcntl(d2, F_GETFD) != 0) return fail("dup carried close-on-exec");
    close(d2);
    close(d);
    close(keep);
    errno = 0;
    if (!refused(dup(d), EBADF)) return fail("dup(closed) not EBADF");
    errno = 0;
    if (!refused(dup(-1), EBADF)) return fail("dup(-1) not EBADF");
    close(fd);
    return 0;
}

static int leg_dupfd(void) {
    int fd = open(PROBE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("dupfd open");
    if (!is_free(100) || !is_free(101) || !is_free(110) || !is_free(111) ||
        !is_free(1023))
        return fail("dupfd: a probe slot is already open");
    if (fcntl(fd, F_DUPFD, 100) != 100) return fail("F_DUPFD did not land at its free floor");
    if (fcntl(100, F_GETFD) != 0) return fail("F_DUPFD set close-on-exec");
    if (write(100, "dupfd", 5) != 5) return fail("F_DUPFD write through the copy");
    if (lseek(fd, 0, SEEK_CUR) != 5) return fail("F_DUPFD: not a copy of the source");
    if (fcntl(fd, F_DUPFD_CLOEXEC, 100) != 101)
        return fail("F_DUPFD_CLOEXEC did not take the next free number above its floor");
    if (fcntl(101, F_GETFD) != FD_CLOEXEC) return fail("F_DUPFD_CLOEXEC left the flag clear");
    if (fcntl(fd, F_DUPFD, 1023) != 1023) return fail("F_DUPFD at the last slot");
    errno = 0;
    if (!refused(fcntl(fd, F_DUPFD, 1023), EMFILE)) return fail("nothing free at or above 1023 not EMFILE");
    errno = 0;
    if (!refused(fcntl(fd, F_DUPFD, 1024), EINVAL)) return fail("F_DUPFD floor past the table not EINVAL");
    errno = 0;
    if (!refused(fcntl(fd, F_DUPFD, -1), EINVAL)) return fail("F_DUPFD negative floor not EINVAL");
    close(1023);
    close(101);
    close(100);
    errno = 0;
    if (!refused(fcntl(100, F_DUPFD, -1), EBADF)) return fail("F_DUPFD on a closed fd not EBADF");
    if (dup2(fd, 110) != 110 || fcntl(110, F_GETFD) != 0) return fail("dup2 onto a free target");
    if (dup3(fd, 111, O_CLOEXEC) != 111 || fcntl(111, F_GETFD) != FD_CLOEXEC)
        return fail("dup3 O_CLOEXEC onto a free target");
    close(111);
    close(110);
    close(fd);
    return 0;
}

static int leg_getfl(void) {
    int w = open(PROBE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int r = open(PROBE, O_RDONLY);
    int a = open(PROBE, O_RDWR | O_APPEND);
    if (w < 0 || r < 0 || a < 0) return fail("getfl opens");
    if (fcntl(r, F_GETFL) != O_RDONLY) return fail("F_GETFL of O_RDONLY");
    if (fcntl(w, F_GETFL) != O_WRONLY) return fail("F_GETFL of O_WRONLY|O_CREAT|O_TRUNC");
    if (fcntl(a, F_GETFL) != (O_RDWR | O_APPEND)) return fail("F_GETFL of O_RDWR|O_APPEND");
    errno = 0;
    if (!refused(fcntl(r, F_SETFL, O_APPEND), EINVAL)) return fail("F_SETFL turning O_APPEND on not EINVAL");
    errno = 0;
    if (!refused(fcntl(a, F_SETFL, 0), EINVAL)) return fail("F_SETFL turning O_APPEND off not EINVAL");
    if (fcntl(a, F_SETFL, fcntl(a, F_GETFL) | O_NONBLOCK) != 0)
        return fail("F_SETFL restating O_APPEND with O_NONBLOCK");
    if (fcntl(a, F_GETFL) != (O_RDWR | O_APPEND | O_NONBLOCK)) return fail("F_GETFL after F_SETFL");
    close(a);
    close(r);
    close(w);
    return 0;
}

static int leg_setfl(void) {
    int p[2];
    if (pipe(p) != 0) return fail("setfl pipe");
    if (fcntl(p[0], F_GETFL) != O_RDONLY || fcntl(p[1], F_GETFL) != O_WRONLY)
        return fail("F_GETFL of the pipe ends");
    if (fcntl(p[0], F_SETFL, O_NONBLOCK) != 0) return fail("F_SETFL O_NONBLOCK");
    // Read back before the read: a flag set on the wrong descriptor would
    // otherwise block this read forever instead of failing the leg.
    if (fcntl(p[0], F_GETFL) != (O_RDONLY | O_NONBLOCK) || fcntl(p[1], F_GETFL) != O_WRONLY)
        return fail("F_SETFL O_NONBLOCK did not land on its own descriptor");
    char c;
    errno = 0;
    if (!refused(read(p[0], &c, 1), EAGAIN)) return fail("nonblocking read of an empty pipe not EAGAIN");
    int d = dup(p[0]);
    if (d < 0) return fail("setfl dup");
    if (!(fcntl(d, F_GETFL) & O_NONBLOCK)) return fail("O_NONBLOCK not shared by the open file's dup");
    if (fcntl(p[0], F_SETFL, 0) != 0) return fail("F_SETFL 0");
    if (fcntl(d, F_GETFL) != O_RDONLY) return fail("O_NONBLOCK did not clear");
    close(d);
    close(p[0]);
    close(p[1]);
    return 0;
}

static int leg_cloexec(void) {
    int plain = open(PROBE, O_RDWR | O_CREAT | O_TRUNC, 0644);
    int fd = open(PROBE, O_RDONLY | O_CLOEXEC);
    if (plain < 0 || fd < 0) return fail("cloexec opens");
    if (fcntl(plain, F_GETFD) != 0) return fail("open without O_CLOEXEC set the flag");
    if (fcntl(fd, F_GETFD) != FD_CLOEXEC) return fail("open O_CLOEXEC left the flag clear");
    close(fd);
    FILE *f = fopen(PROBE, "re");
    if (!f) return fail("fopen re");
    if (fcntl(fileno(f), F_GETFD) != FD_CLOEXEC) return fail("fopen e left the flag clear");
    fclose(f);
    int d = dup(plain);
    if (d < 0) return fail("cloexec dup");
    f = fdopen(d, "r+e");
    if (!f) return fail("fdopen r+e");
    if (fcntl(d, F_GETFD) != FD_CLOEXEC) return fail("fdopen e left the flag clear");
    f = freopen(NULL, "r", f);
    if (!f) return fail("freopen without a name");
    if (fileno(f) != d) return fail("freopen without a name moved the descriptor");
    fclose(f);
    // openat's create arm and its exists-already arm flag what they return.
    unlink(PROBE_NEW);
    int c = open(PROBE_NEW, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (c < 0 || fcntl(c, F_GETFD) != FD_CLOEXEC) return fail("create O_CLOEXEC left the flag clear");
    int e = open(PROBE_NEW, O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    if (e < 0 || fcntl(e, F_GETFD) != FD_CLOEXEC) return fail("O_CREAT of an existing name left the flag clear");
    close(c);
    close(e);
    unlink(PROBE_NEW);
    // freopen with no name: "e" sets the flag; a mode other than "a" keeps an
    // append descriptor's append rather than refusing to change it; "a" on a
    // descriptor opened without O_APPEND is refused, and the stream closed.
    d = dup(plain);
    if (d < 0 || !(f = fdopen(d, "r+"))) return fail("freopen setup");
    f = freopen(NULL, "r+e", f);
    if (!f || fileno(f) != d || fcntl(d, F_GETFD) != FD_CLOEXEC) return fail("freopen e left the flag clear");
    errno = 0;
    if (freopen(NULL, "a", f) || errno != EINVAL) return fail("freopen a on a descriptor without O_APPEND");
    int ap = open(PROBE, O_RDWR | O_APPEND);
    if (ap < 0 || !(f = fdopen(ap, "a+"))) return fail("freopen append setup");
    f = freopen(NULL, "r+", f);
    if (!f || fileno(f) != ap) return fail("freopen r+ closed an append stream");
    if (!(fcntl(ap, F_GETFL) & O_APPEND)) return fail("freopen r+ lost the descriptor's append");
    fclose(f);
    close(plain);
    return 0;
}

// The expected modes come from the pool root's own mode, and a root whose
// mode could not tell the mask or the bound from their absence fails the leg.
static mode_t umask_create_probe(int *rc) {
    unlink(PROBE_NEW);
    int fd = open(PROBE_NEW, O_RDWR | O_CREAT | O_EXCL, 0666);
    struct stat st;
    *rc = fd >= 0 ? fstat(fd, &st) : -1;
    if (fd >= 0) close(fd);
    unlink(PROBE_NEW);
    return *rc == 0 ? (st.st_mode & 0777) : 0;
}

static int leg_umask(void) {
    struct stat root;
    if (stat("/", &root) != 0) return fail("umask stat /");
    mode_t bound = ~(mode_t)0666 | (root.st_mode & 0666);
    mode_t want_masked = 0666 & ~(mode_t)027 & bound;
    mode_t want_bound = 0666 & bound;
    if (want_masked == want_bound || want_bound == 0666)
        return fail("umask: the pool root's mode cannot show the mask and the bound");
    int rc1 = -1, rc2 = -1;
    mode_t was = umask(027);
    mode_t masked = umask_create_probe(&rc1);
    mode_t back = umask(0);
    mode_t bounded = umask_create_probe(&rc2);
    mode_t cut = umask(01022);         // 01000 is above 0777
    mode_t now = umask(was);
    if (was != 022) return fail("umask did not answer the boot mask 022");
    if (rc1 != 0 || rc2 != 0) return fail("umask create or fstat");
    if (masked != want_masked) return fail("umask: the mask did not apply");
    if (bounded != want_bound) return fail("umask: the directory's bound did not apply");
    if (back != 027 || cut != 0) return fail("umask did not answer the mask it replaced");
    if (now != 022) return fail("umask(01022) was not cut to 022");
    return 0;
}

static int leg_fdopendir(void) {
    int fd = open("/", O_RDONLY | O_DIRECTORY);
    if (fd < 0) return fail("fdopendir open");
    DIR *dir = fdopendir(fd);
    if (!dir) return fail("fdopendir");
    int n = 0;
    while (readdir(dir)) n++;
    closedir(dir);
    if (n == 0) return fail("fdopendir read no entries");
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
        { "dup", leg_dup },               { "dupfd", leg_dupfd },
        { "getfl", leg_getfl },           { "setfl", leg_setfl },
        { "cloexec", leg_cloexec },       { "fdopendir", leg_fdopendir },
        { "umask", leg_umask },
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
