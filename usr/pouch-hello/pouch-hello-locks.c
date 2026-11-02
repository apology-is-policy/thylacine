// /pouch-hello-locks -- the file-lock prover (B-3h4; patch 0065, operator
// votes 34 and 35: SYS_FILE_LOCK and its two owners, ARCH 9.6.11).
//
// Runs POST-PIVOT (spawned as /bin/pouch-hello-locks) in its own directory at
// the pool root. Every conflict is between two Procs, or two open files, over
// Stratum's lock table: the parent holds, and a child it spawns (itself, with
// a mode in argv[1]) opens the same file and asks. Each leg releases what it
// took, so a lock left behind by a broken release fails the next leg. Legs,
// each printed + boot-fatal via the joey expect string:
//
//   own         : a Proc's lock never conflicts with itself (F_GETLK answers
//                 F_UNLCK), and another Proc's take of it is EAGAIN; F_GETLK
//                 there reports its type and range, l_pid -1.
//   split       : unlocking the middle of a lock leaves both ends held.
//   posix-close : closing ANY descriptor of the file drops all the Proc's
//                 F_SETLK locks on it (POSIX's descriptor-table owner). A
//                 navigation-only descriptor is no open of the file: O_PATH's
//                 close, and the one inside chmod(path), keep them.
//   ofd         : an F_OFD_ lock belongs to its open file: a second open of
//                 the file in the same Proc conflicts with it, and closing
//                 that second open drops nothing.
//   ofd-vs-proc : an OFD lock and a Proc lock conflict within one Proc.
//   flock       : flock is a whole-file lock of the open file, and conflicts
//                 with fcntl locks (Linux's NFS client does the same).
//   wait        : F_SETLKW waits while another Proc holds, and takes the lock
//                 when that Proc releases it.
//   sqlite      : SQLite's unix VFS sequence over its lock bytes: SHARED,
//                 RESERVED, EXCLUSIVE, then the downgrade to SHARED with one
//                 two-byte unlock of PENDING and RESERVED. After it, a reader
//                 is admitted and a writer is not.
//   cached-ro   : a read-only descriptor of a small file the kernel holds whole
//                 (a cached open, whose kernel legs pin the mechanism): it takes
//                 a read lock, flock, fchmod and futimens. Before the lock it
//                 reads what it saw at open, and once locked what the last
//                 holder wrote.
//   downgrade   : a write lock converted to a read lock lands the holder's
//                 staged append first, so the reader it admits reads it.
//   errors      : a write lock through a read-only descriptor and flock
//                 LOCK_EX on one are EBADF, an OFD request with a pid and a
//                 bad whence are EINVAL, a range past LLONG_MAX is EOVERFLOW,
//                 and a pipe, whose Dev serves no locks, is ENOLCK.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "pouch-census.h"

#define SELF "/bin/pouch-hello-locks"
#define WORK "/pouch-locks"
#define FILE_A WORK "/db"

// SQLite's lock bytes (os_unix.c): one byte each for PENDING and RESERVED,
// then the 510-byte SHARED range.
#define PENDING_BYTE  0x40000000LL
#define RESERVED_BYTE (PENDING_BYTE + 1)
#define SHARED_FIRST  (PENDING_BYTE + 2)
#define SHARED_SIZE   510

extern char **environ;

static int fail(const char *what) {
    printf("pouch-hello-locks: FAIL %s (errno %d)\n", what, errno);
    return 1;
}

static short type_of(char c) {
    return c == 'r' ? F_RDLCK : c == 'w' ? F_WRLCK : F_UNLCK;
}

static int lk(int fd, int cmd, char type, long long start, long long len) {
    struct flock fl = { .l_type = type_of(type), .l_whence = SEEK_SET,
                        .l_start = start, .l_len = len, .l_pid = 0 };
    return fcntl(fd, cmd, &fl);
}

// Spawn self with argv[1..]; `out` (or -1) becomes the child's stdout. The
// child's pid, or -1.
static pid_t spawn_child(char *const av[], int out) {
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (out >= 0) posix_spawn_file_actions_adddup2(&fa, out, 1);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, SELF, &fa, NULL, av, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) { errno = rc; return -1; }
    return pid;
}

static int reap(pid_t pid) {
    int st = 0;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st)) return -1;
    return WEXITSTATUS(st);
}

// Run a child to completion: 0 when it saw what it was told to expect.
static int child(const char *a1, const char *a2, const char *a3, const char *a4,
                 const char *a5, const char *a6) {
    char *av[] = { SELF, (char *)a1, (char *)a2, (char *)a3, (char *)a4,
                   (char *)a5, (char *)a6, NULL };
    pid_t pid = spawn_child(av, -1);
    return pid < 0 ? -1 : reap(pid);
}

// A child: open the file read-write and make one request.
//   set  <s|o> <r|w|u> <start> <len> <errno>   F_SETLK / F_OFD_SETLK
//   get  <r|w> <start> <len> <type> <start'>   F_GETLK; len' rides argv[7]
//   flk  <ex|sh|un> <errno>                     flock with LOCK_NB
//   setw <r|w> <start> <len>                    F_SETLKW, after one byte on fd 1
//   wbyte                                       write 'x' at 0 under F_SETLKW
//   size <n>                                    fstat reports size n
//   rbyte <off> <c>                             under a read lock on byte 0, byte off is c
static int child_main(int argc, char **argv) {
    int fd = open(FILE_A, O_RDWR);
    if (fd < 0) return 90;
    const char *m = argv[1];
    if (!strcmp(m, "set") && argc >= 7) {
        int want = atoi(argv[6]);
        int cmd = argv[2][0] == 'o' ? F_OFD_SETLK : F_SETLK;
        int rc = lk(fd, cmd, argv[3][0], strtoll(argv[4], 0, 0), strtoll(argv[5], 0, 0));
        if (want == 0) return rc == 0 ? 0 : 91;
        return rc == -1 && errno == want ? 0 : 92;
    }
    if (!strcmp(m, "get") && argc >= 8) {
        struct flock fl = { .l_type = type_of(argv[2][0]), .l_whence = SEEK_SET,
                            .l_start = strtoll(argv[3], 0, 0),
                            .l_len = strtoll(argv[4], 0, 0) };
        if (fcntl(fd, F_GETLK, &fl) != 0) return 93;
        if (fl.l_type != type_of(argv[5][0])) return 94;
        if (fl.l_type == F_UNLCK) return 0;
        if (fl.l_whence != SEEK_SET || fl.l_start != strtoll(argv[6], 0, 0)
            || fl.l_len != strtoll(argv[7], 0, 0) || fl.l_pid != -1) return 95;
        return 0;
    }
    if (!strcmp(m, "flk") && argc >= 4) {
        int op = !strcmp(argv[2], "ex") ? LOCK_EX : !strcmp(argv[2], "sh") ? LOCK_SH : LOCK_UN;
        int want = atoi(argv[3]);
        int rc = flock(fd, op | LOCK_NB);
        if (want == 0) return rc == 0 ? 0 : 96;
        return rc == -1 && errno == want ? 0 : 97;
    }
    if (!strcmp(m, "setw") && argc >= 5) {
        if (write(1, "W", 1) != 1) return 98;
        int rc = lk(fd, F_SETLKW, argv[2][0], strtoll(argv[3], 0, 0), strtoll(argv[4], 0, 0));
        return rc == 0 ? 0 : 99;
    }
    if (!strcmp(m, "wbyte")) {
        if (lk(fd, F_SETLKW, 'w', 0, 1) != 0) return 88;
        if (pwrite(fd, "x", 1, 0) != 1) return 87;
        return lk(fd, F_SETLK, 'u', 0, 1) == 0 ? 0 : 86;
    }
    if (!strcmp(m, "size") && argc >= 3) {
        struct stat st;
        if (fstat(fd, &st) != 0) return 85;
        return st.st_size == strtoll(argv[2], 0, 0) ? 0 : 84;
    }
    if (!strcmp(m, "rbyte") && argc >= 4) {
        if (lk(fd, F_SETLK, 'r', 0, 1) != 0) return 83;
        char b = 0;
        if (pread(fd, &b, 1, strtoll(argv[2], 0, 0)) != 1) return 82;
        return b == argv[3][0] ? 0 : 81;
    }
    return 89;
}

static char nbuf[8][24];
static const char *num(int slot, long long v) {
    snprintf(nbuf[slot], sizeof nbuf[slot], "%lld", v);
    return nbuf[slot];
}

// Another Proc's F_SETLK (s) or F_OFD_SETLK (o) of [start, start+len) answers
// `want` (0 or an errno).
static int other_set(char cmd, char type, long long start, long long len, int want) {
    char c[2] = { cmd, 0 }, t[2] = { type, 0 };
    return child("set", c, t, num(0, start), num(1, len), num(2, want));
}

static int other_get(char type, long long start, long long len, char wtype,
                     long long wstart, long long wlen) {
    char t[2] = { type, 0 }, w[2] = { wtype, 0 };
    char *av[] = { SELF, "get", t, (char *)num(0, start), (char *)num(1, len), w,
                   (char *)num(2, wstart), (char *)num(3, wlen), NULL };
    pid_t pid = spawn_child(av, -1);
    return pid < 0 ? -1 : reap(pid);
}

// Release everything this Proc and this open file hold on the file.
static int release_all(int fd) {
    if (lk(fd, F_SETLK, 'u', 0, 0) != 0) return -1;
    if (lk(fd, F_OFD_SETLK, 'u', 0, 0) != 0) return -1;
    return flock(fd, LOCK_UN);
}

static int leg_own(int fd) {
    if (lk(fd, F_SETLK, 'w', 0, 10) != 0) return fail("own: F_SETLK WRLCK [0,10)");
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 10 };
    if (fcntl(fd, F_GETLK, &fl) != 0 || fl.l_type != F_UNLCK)
        return fail("own: the holder's F_GETLK is not F_UNLCK");
    if (other_set('s', 'w', 0, 10, EAGAIN) != 0) return fail("own: another Proc's take is not EAGAIN");
    if (other_set('s', 'r', 9, 1, EAGAIN) != 0) return fail("own: another Proc's read of the last byte is not EAGAIN");
    if (other_set('s', 'w', 10, 1, 0) != 0) return fail("own: the byte past the lock is not free");
    if (other_get('r', 5, 1, 'w', 0, 10) != 0) return fail("own: F_GETLK does not report WRLCK [0,10) pid -1");
    printf("pouch-hello-locks: own ok\n");
    return 0;
}

static int leg_split(int fd) {
    if (lk(fd, F_SETLK, 'u', 3, 4) != 0) return fail("split: F_UNLCK [3,7)");
    if (other_set('s', 'w', 3, 4, 0) != 0) return fail("split: the unlocked middle is not free");
    if (other_set('s', 'w', 0, 1, EAGAIN) != 0) return fail("split: the head was released");
    if (other_set('s', 'w', 9, 1, EAGAIN) != 0) return fail("split: the tail was released");
    printf("pouch-hello-locks: split ok\n");
    return 0;
}

static int leg_posix_close(int fd) {
    if (lk(fd, F_SETLK, 'w', 0, 10) != 0) return fail("posix-close: F_SETLK WRLCK [0,10)");
    if (other_set('s', 'w', 0, 10, EAGAIN) != 0) return fail("posix-close: the lock is not held");
    int op = open(FILE_A, O_PATH);
    if (op < 0) return fail("posix-close: an O_PATH open");
    if (close(op) != 0) return fail("posix-close: its close");
    if (other_set('s', 'w', 0, 10, EAGAIN) != 0) return fail("posix-close: an O_PATH close dropped the Proc's locks");
    if (chmod(FILE_A, 0644) != 0) return fail("posix-close: chmod");
    if (other_set('s', 'w', 0, 10, EAGAIN) != 0) return fail("posix-close: chmod(path) dropped the Proc's locks");
    int fd2 = open(FILE_A, O_RDWR);
    if (fd2 < 0) return fail("posix-close: second open");
    if (close(fd2) != 0) return fail("posix-close: close");
    if (other_set('s', 'w', 0, 10, 0) != 0) return fail("posix-close: a close of another descriptor kept the Proc's locks");
    printf("pouch-hello-locks: posix-close ok\n");
    return 0;
}

static int leg_ofd(int fd) {
    if (lk(fd, F_OFD_SETLK, 'w', 100, 10) != 0) return fail("ofd: F_OFD_SETLK [100,110)");
    int fd2 = open(FILE_A, O_RDWR);
    if (fd2 < 0) return fail("ofd: second open");
    if (lk(fd2, F_OFD_SETLK, 'w', 105, 1) != -1 || errno != EAGAIN)
        return fail("ofd: a second open file in the same Proc took it");
    struct flock fl = { .l_type = F_RDLCK, .l_whence = SEEK_SET, .l_start = 100, .l_len = 1 };
    if (fcntl(fd2, F_OFD_GETLK, &fl) != 0 || fl.l_type != F_WRLCK || fl.l_start != 100
        || fl.l_len != 10 || fl.l_pid != -1)
        return fail("ofd: F_OFD_GETLK does not report WRLCK [100,110) pid -1");
    if (close(fd2) != 0) return fail("ofd: close");
    if (other_set('s', 'w', 100, 10, EAGAIN) != 0) return fail("ofd: closing another open file dropped the OFD lock");
    if (lk(fd, F_OFD_SETLK, 'u', 100, 10) != 0) return fail("ofd: F_OFD_SETLK F_UNLCK");
    if (other_set('o', 'w', 100, 10, 0) != 0) return fail("ofd: the released range is not free");
    printf("pouch-hello-locks: ofd ok\n");
    return 0;
}

static int leg_ofd_vs_proc(int fd) {
    if (lk(fd, F_SETLK, 'w', 200, 1) != 0) return fail("ofd-vs-proc: F_SETLK [200,201)");
    if (lk(fd, F_OFD_SETLK, 'w', 200, 1) != -1 || errno != EAGAIN)
        return fail("ofd-vs-proc: the open file took the Proc's byte");
    if (lk(fd, F_SETLK, 'u', 200, 1) != 0) return fail("ofd-vs-proc: F_UNLCK");
    if (lk(fd, F_OFD_SETLK, 'w', 200, 1) != 0) return fail("ofd-vs-proc: the released byte");
    if (lk(fd, F_SETLK, 'r', 200, 1) != -1 || errno != EAGAIN)
        return fail("ofd-vs-proc: the Proc took the open file's byte");
    if (lk(fd, F_OFD_SETLK, 'u', 200, 1) != 0) return fail("ofd-vs-proc: F_OFD_SETLK F_UNLCK");
    printf("pouch-hello-locks: ofd-vs-proc ok\n");
    return 0;
}

static int leg_flock(int fd) {
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) return fail("flock: LOCK_EX");
    if (child("flk", "sh", num(0, EWOULDBLOCK), 0, 0, 0) != 0) return fail("flock: another Proc's LOCK_SH is not EWOULDBLOCK");
    if (other_set('s', 'r', 4096, 1, EAGAIN) != 0) return fail("flock: an fcntl lock past EOF got through LOCK_EX");
    if (flock(fd, LOCK_SH | LOCK_NB) != 0) return fail("flock: the downgrade to LOCK_SH");
    if (child("flk", "sh", "0", 0, 0, 0) != 0) return fail("flock: a reader beside LOCK_SH");
    if (flock(fd, LOCK_UN) != 0) return fail("flock: LOCK_UN");
    if (child("flk", "ex", "0", 0, 0, 0) != 0) return fail("flock: LOCK_EX after LOCK_UN");
    printf("pouch-hello-locks: flock ok\n");
    return 0;
}

static int leg_wait(int fd) {
    if (lk(fd, F_SETLK, 'w', 0, 1) != 0) return fail("wait: F_SETLK [0,1)");
    int p[2];
    if (pipe(p) != 0) return fail("wait: pipe");
    char *av[] = { SELF, "setw", "w", "0", "1", NULL };
    pid_t pid = spawn_child(av, p[1]);
    close(p[1]);
    if (pid < 0) return fail("wait: spawn");
    char b = 0;
    // The child writes its byte just before it asks; then give it time to be
    // refused at least once.
    if (read(p[0], &b, 1) != 1 || b != 'W') return fail("wait: the child never reached its request");
    close(p[0]);
    struct timespec ts = { 0, 300 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    int st = 0;
    if (waitpid(pid, &st, WNOHANG) != 0) return fail("wait: F_SETLKW returned while the lock was held");
    if (lk(fd, F_SETLK, 'u', 0, 1) != 0) return fail("wait: F_UNLCK");
    if (reap(pid) != 0) return fail("wait: F_SETLKW did not take the released lock");
    printf("pouch-hello-locks: wait ok\n");
    return 0;
}

static int leg_sqlite(int fd) {
    // SHARED: PENDING read-locked while the SHARED range is taken.
    if (lk(fd, F_SETLK, 'r', PENDING_BYTE, 1) != 0) return fail("sqlite: SHARED's PENDING");
    if (lk(fd, F_SETLK, 'r', SHARED_FIRST, SHARED_SIZE) != 0) return fail("sqlite: SHARED range");
    if (lk(fd, F_SETLK, 'u', PENDING_BYTE, 1) != 0) return fail("sqlite: PENDING released");
    // RESERVED, then EXCLUSIVE (PENDING, then the SHARED range for writing).
    if (lk(fd, F_SETLK, 'w', RESERVED_BYTE, 1) != 0) return fail("sqlite: RESERVED");
    if (lk(fd, F_SETLK, 'w', PENDING_BYTE, 1) != 0) return fail("sqlite: EXCLUSIVE's PENDING");
    if (lk(fd, F_SETLK, 'w', SHARED_FIRST, SHARED_SIZE) != 0) return fail("sqlite: EXCLUSIVE");
    if (other_set('s', 'r', SHARED_FIRST, 1, EAGAIN) != 0) return fail("sqlite: a reader beside EXCLUSIVE");
    // The downgrade: the SHARED range back to a read lock, then one unlock of
    // the two bytes PENDING and RESERVED.
    if (lk(fd, F_SETLK, 'r', SHARED_FIRST, SHARED_SIZE) != 0) return fail("sqlite: the downgrade");
    if (lk(fd, F_SETLK, 'u', PENDING_BYTE, 2) != 0) return fail("sqlite: the two-byte unlock");
    if (other_set('s', 'r', SHARED_FIRST, SHARED_SIZE, 0) != 0) return fail("sqlite: a reader after the downgrade is BUSY");
    if (other_set('s', 'w', RESERVED_BYTE, 1, 0) != 0) return fail("sqlite: RESERVED survived the two-byte unlock");
    if (other_set('s', 'w', PENDING_BYTE, 1, 0) != 0) return fail("sqlite: PENDING survived the two-byte unlock");
    if (other_set('s', 'w', SHARED_FIRST + SHARED_SIZE - 1, 1, EAGAIN) != 0) return fail("sqlite: a writer got the SHARED range");
    if (other_get('w', SHARED_FIRST, 1, 'r', SHARED_FIRST, SHARED_SIZE) != 0) return fail("sqlite: F_GETLK of the SHARED range");
    printf("pouch-hello-locks: sqlite ok\n");
    return 0;
}

static int leg_cached_ro(void) {
    // A whole read fills the kernel's pages, so the next read-only open is
    // served from them.
    static char whole[4096];
    int p0 = open(FILE_A, O_RDONLY);
    if (p0 < 0) return fail("cached-ro: the filling open");
    if (read(p0, whole, sizeof whole) != (ssize_t)sizeof whole) return fail("cached-ro: the filling read");
    close(p0);
    int ro = open(FILE_A, O_RDONLY);
    if (ro < 0) return fail("cached-ro: open");
    if (child("wbyte", 0, 0, 0, 0, 0) != 0) return fail("cached-ro: another Proc's locked write");
    // The legs below prove nothing unless this open is the cached one: until the
    // lock it reads the file as it saw it.
    char b = 0;
    if (pread(ro, &b, 1, 0) != 1 || b != 'd') return fail("cached-ro: the open is not served from its snapshot");
    if (lk(ro, F_SETLK, 'r', 0, 1) != 0) return fail("cached-ro: a read lock");
    b = 0;
    if (pread(ro, &b, 1, 0) != 1 || b != 'x') return fail("cached-ro: the lock does not read the last holder's write");
    if (other_set('s', 'w', 0, 1, EAGAIN) != 0) return fail("cached-ro: the read lock is not held");
    if (flock(ro, LOCK_SH | LOCK_NB) != 0) return fail("cached-ro: flock LOCK_SH");
    if (flock(ro, LOCK_UN) != 0) return fail("cached-ro: flock LOCK_UN");
    if (fchmod(ro, 0640) != 0) return fail("cached-ro: fchmod");
    struct timespec ts[2] = { { 1000000000L, 0 }, { 1100000000L, 0 } };
    if (futimens(ro, ts) != 0) return fail("cached-ro: futimens");
    struct stat st;
    if (fstat(ro, &st) != 0 || (st.st_mode & 0777) != 0640 || st.st_mtim.tv_sec != 1100000000L)
        return fail("cached-ro: fstat does not read the fchmod and futimens back");
    if (close(ro) != 0) return fail("cached-ro: close");
    if (other_set('s', 'w', 0, 1, 0) != 0) return fail("cached-ro: its close kept the Proc's lock");
    printf("pouch-hello-locks: cached-ro ok\n");
    return 0;
}

// fd was born by its create, so its appends stage in the kernel. The child's
// fstat shows the byte has not reached the server until the conversion lands it.
static int leg_downgrade(int fd) {
    off_t end = lseek(fd, 0, SEEK_END);
    if (end < 0) return fail("downgrade: lseek");
    if (lk(fd, F_SETLK, 'w', 0, 1) != 0) return fail("downgrade: F_SETLK WRLCK");
    if (write(fd, "y", 1) != 1) return fail("downgrade: the append");
    if (child("size", num(0, end), 0, 0, 0, 0) != 0) return fail("downgrade: the append was not staged");
    if (lk(fd, F_SETLK, 'r', 0, 1) != 0) return fail("downgrade: the conversion");
    if (child("rbyte", num(0, end), "y", 0, 0, 0) != 0) return fail("downgrade: the reader it admitted does not read the append");
    printf("pouch-hello-locks: downgrade ok\n");
    return 0;
}

static int leg_errors(int fd) {
    int ro = open(FILE_A, O_RDONLY);
    if (ro < 0) return fail("errors: read-only open");
    if (lk(ro, F_SETLK, 'w', 0, 1) != -1 || errno != EBADF) return fail("errors: WRLCK through O_RDONLY");
    if (flock(ro, LOCK_EX | LOCK_NB) != -1 || errno != EBADF) return fail("errors: LOCK_EX through O_RDONLY");
    close(ro);
    struct flock fl = { .l_type = F_WRLCK, .l_whence = SEEK_SET, .l_start = 0, .l_len = 1, .l_pid = 1 };
    if (fcntl(fd, F_OFD_SETLK, &fl) != -1 || errno != EINVAL) return fail("errors: an OFD request with a pid");
    fl.l_pid = 0; fl.l_whence = 7;
    if (fcntl(fd, F_SETLK, &fl) != -1 || errno != EINVAL) return fail("errors: a bad whence");
    fl.l_whence = SEEK_END; fl.l_start = LLONG_MAX;
    if (fcntl(fd, F_SETLK, &fl) != -1 || errno != EOVERFLOW) return fail("errors: a start past LLONG_MAX");
    fl.l_whence = SEEK_SET; fl.l_start = 0; fl.l_type = F_UNLCK;
    if (fcntl(fd, F_GETLK, &fl) != -1 || errno != EINVAL) return fail("errors: F_GETLK of F_UNLCK");
    int p[2];
    if (pipe(p) != 0) return fail("errors: pipe");
    if (lk(p[1], F_SETLK, 'w', 0, 1) != -1 || errno != ENOLCK) return fail("errors: a pipe's lock is not ENOLCK");
    struct flock cur = { .l_type = F_WRLCK, .l_whence = SEEK_CUR, .l_start = 0, .l_len = 1 };
    if (fcntl(p[1], F_SETLK, &cur) != -1 || errno != ENOLCK) return fail("errors: SEEK_CUR on a pipe is not ENOLCK");
    close(p[0]); close(p[1]);
    if (close(fd) != 0 || lk(fd, F_SETLK, 'w', 0, 1) != -1 || errno != EBADF)
        return fail("errors: a closed descriptor");
    printf("pouch-hello-locks: errors ok\n");
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2) return child_main(argc, argv);
    setvbuf(stdout, NULL, _IONBF, 0);
    if (mkdir(WORK, 0755) != 0 && errno != EEXIST) return fail("mkdir " WORK);
    int fd = open(FILE_A, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fail("create " FILE_A);
    static char page[4096];
    memset(page, 'd', sizeof page);
    if (write(fd, page, sizeof page) != (ssize_t)sizeof page) return fail("write");
    if (leg_own(fd) || leg_split(fd) || release_all(fd) != 0
        || leg_posix_close(fd) || release_all(fd) != 0
        || leg_ofd(fd) || release_all(fd) != 0
        || leg_ofd_vs_proc(fd) || release_all(fd) != 0
        || leg_flock(fd) || release_all(fd) != 0
        || leg_wait(fd) || release_all(fd) != 0
        || leg_sqlite(fd) || release_all(fd) != 0
        || leg_cached_ro() || release_all(fd) != 0
        || leg_downgrade(fd) || release_all(fd) != 0
        || leg_errors(fd))
        return 1;
    if (unlink(FILE_A) != 0) return fail("unlink");
    puts(POUCH_CENSUS_LOCKS);
    return 0;
}
