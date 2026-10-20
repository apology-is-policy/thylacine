/* Pouch fstat(), ioctl() and isatty() over a mocked kernel SYS_fstat: a dead
 * descriptor answers EBADF, a live one a request does not fit answers ENOTTY,
 * and the kernel's other errnos cross fstat() unchanged. */
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

/* The kernel speaks Linux errno values; the musl sources compare against the
 * host's. The ones these cases use must agree. */
_Static_assert(EPERM == 1 && EIO == 5 && EBADF == 9 && EACCES == 13 &&
               EFAULT == 14 && EINVAL == 22 && ENOTTY == 25,
               "host errno values differ from Linux's");

#define hidden __attribute__((__visibility__("hidden")))
#define weak_alias(old, new)
#include "linux-stat.h"
#include "linux-tty.h"
#include "sysnums.h"
#include "dispatch-under-test.h"
#include "_pouch_socket.h"

/* The kernel's handle table: each listed fd answers `reply` to SYS_fstat and,
 * when that is 0, the record given; any other fd names nothing (-EBADF). */
struct kfd { int fd; long reply; unsigned mode; unsigned long qid; };
static struct kfd g_kfds[16];
static int g_nkfds, g_fstat_calls, g_fstat_tagged, g_other_calls;

static long harness_sys(long n, long a, long b, long c) {
    (void)c;
    if (n != SYS_fstat) { ++g_other_calls; return -EINVAL; }
    ++g_fstat_calls;
    if (a >= 0 && (a & POUCH_SOCK_TAG)) ++g_fstat_tagged;
    for (int i = 0; i < g_nkfds; i++) {
        if (g_kfds[i].fd != a) continue;
        if (g_kfds[i].reply == 0) {
            unsigned char *t = (unsigned char *)b;
            unsigned long size = 42;
            memset(t, 0, 88);
            memcpy(t + 0, &size, 8);
            memcpy(t + 8, &g_kfds[i].qid, 8);
            memcpy(t + 40, &g_kfds[i].mode, 4);
        }
        return g_kfds[i].reply;
    }
    return -EBADF;
}
#define __syscall1(n, a) harness_sys((n), (long)(a), 0, 0)
#define __syscall2(n, a, b) harness_sys((n), (long)(a), (long)(b), 0)
#define __syscall3(n, a, b, c) harness_sys((n), (long)(a), (long)(b), (long)(c))
long __syscall_ret(unsigned long r);

/* libc's socket table: two live slots, everything else no slot. */
static struct pouch_sock_slot g_unix_slot, g_inet_slot;
static int g_resolves;
#define TAG_UNIX (POUCH_SOCK_TAG | 1)
#define TAG_INET (POUCH_SOCK_TAG | 2)
#define TAG_DEAD (POUCH_SOCK_TAG | 3)
struct pouch_sock_slot *pouch_sock_resolve(int fd) {
    ++g_resolves;
    if (fd == TAG_UNIX) return &g_unix_slot;
    if (fd == TAG_INET) return &g_inet_slot;
    return 0;
}
int pouch_sock_readable(int fd) { (void)fd; return 0; }

/* No file opens on the host: the winsize leaf and pts ctl files are absent. */
static int mock_open(const char *path, int flags, ...) { (void)path; (void)flags; return -1; }
static ssize_t mock_read(int fd, void *buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
static ssize_t mock_write(int fd, const void *buf, size_t n) { (void)fd; (void)buf; (void)n; return -1; }
static int mock_close(int fd) { (void)fd; return 0; }
#define open mock_open
#define read mock_read
#define write mock_write
#define close mock_close
#define __fstat tested_fstat
#define ioctl tested_ioctl
#define isatty tested_isatty
#include "syscall-ret-under-test.c"
#include "fstat-under-test.c"
#include "ioctl-under-test.c"
#include "isatty-under-test.c"
#undef open
#undef read
#undef write
#undef close

enum { FD_DEAD = 7, FD_FILE, FD_OLD, FD_MASTER, FD_SLAVE, FD_CONS, FD_EACCES, FD_EFAULT };
#define S_CHR 0020000u
#define S_REG 0100000u

static int failures, tests;

static void verdict(const char *name, int ok, int rc, int err) {
    ++tests;
    failures += !ok;
    printf("%s %s rc=%d errno=%d fstat-calls=%d tagged=%d resolves=%d\n",
           ok ? "PASS" : "FAIL", name, rc, err, g_fstat_calls, g_fstat_tagged, g_resolves);
}

static void reset(void) { g_fstat_calls = g_fstat_tagged = g_resolves = g_other_calls = 0; errno = EDOM; }

static void fstat_case(const char *name, int fd, int want_rc, int want_errno, int want_calls) {
    struct stat st;
    memset(&st, 0xa5, sizeof st);
    reset();
    int rc = tested_fstat(fd, &st);
    int err = errno;
    int ok = rc == want_rc && err == want_errno && g_fstat_calls == want_calls && !g_other_calls;
    if (ok && fd == FD_FILE)
        ok = st.st_size == 42 && (unsigned)st.st_mode == (S_REG | 0644) && st.st_ino == 0x1234;
    if (ok && fd == TAG_UNIX)
        ok = ((unsigned)st.st_mode & 0170000u) == 0140000u && st.st_ino == 2;
    verdict(name, ok, rc, err);
}

static void ioctl_case(const char *name, int fd, int req, int want_rc, int want_errno,
                       int want_calls) {
    union { struct termios tio; struct winsize ws; int n; } arg;
    memset(&arg, 0, sizeof arg);
    reset();
    int rc = tested_ioctl(fd, req, &arg);
    int err = errno;
    int ok = rc == want_rc && err == want_errno && g_fstat_calls == want_calls &&
             !g_fstat_tagged && !g_other_calls;
    if (ok && req == (int)TIOCGPTN && rc == 0) ok = arg.n == 3;
    verdict(name, ok, rc, err);
}

static void isatty_case(const char *name, int fd, int want_rc, int want_errno) {
    reset();
    int rc = tested_isatty(fd);
    int err = errno;
    verdict(name, rc == want_rc && err == want_errno && !g_fstat_tagged && !g_other_calls, rc, err);
}

static void kfd(int fd, long reply, unsigned mode, unsigned long qid) {
    assert(g_nkfds < (int)(sizeof g_kfds / sizeof g_kfds[0]));
    g_kfds[g_nkfds++] = (struct kfd){ fd, reply, mode, qid };
}

int main(void) {
    kfd(FD_FILE, 0, S_REG | 0644, 0x1234);
    kfd(FD_OLD, -1, 0, 0);
    kfd(FD_MASTER, 0, S_CHR | 0620, POUCH_PTS_FLAG | (3u << POUCH_PTS_N_SHIFT) | POUCH_PTS_FK_MASTER);
    kfd(FD_SLAVE, 0, S_CHR | 0620, POUCH_PTS_FLAG | (3u << POUCH_PTS_N_SHIFT) | POUCH_PTS_FK_SLAVE);
    kfd(FD_CONS, 0, S_CHR | 0620, POUCH_CONS_FLAG);
    kfd(FD_EACCES, -EACCES, 0, 0);
    kfd(FD_EFAULT, -EFAULT, 0, 0);
    g_unix_slot.in_use = 1; g_unix_slot.family = POUCH_SOCK_FAM_UNIX; g_unix_slot.ctl_fd = -1;
    g_inet_slot.in_use = 1; g_inet_slot.family = POUCH_SOCK_FAM_INET; g_inet_slot.ctl_fd = -1;

    fstat_case("fstat-live", FD_FILE, 0, EDOM, 1);
    fstat_case("fstat-dead-ebadf", FD_DEAD, -1, EBADF, 1);
    fstat_case("fstat-old-kernel-eio", FD_OLD, -1, EIO, 1);
    fstat_case("fstat-dev-errno", FD_EACCES, -1, EACCES, 1);
    fstat_case("fstat-efault", FD_EFAULT, -1, EFAULT, 1);
    fstat_case("fstat-negative", -1, -1, EBADF, 0);
    fstat_case("fstat-socket-live", TAG_UNIX, 0, EDOM, 0);
    fstat_case("fstat-socket-dead", TAG_DEAD, -1, EBADF, 0);

    isatty_case("isatty-dead-ebadf", FD_DEAD, 0, EBADF);
    isatty_case("isatty-file-enotty", FD_FILE, 0, ENOTTY);
    isatty_case("isatty-old-kernel-enotty", FD_OLD, 0, ENOTTY);
    isatty_case("isatty-negative", -1, 0, EBADF);
    isatty_case("isatty-console", FD_CONS, 1, EDOM);
    isatty_case("isatty-pts", FD_SLAVE, 0, ENOTTY);
    isatty_case("isatty-socket-dead", TAG_DEAD, 0, EBADF);
    isatty_case("isatty-socket-live", TAG_UNIX, 0, ENOTTY);

    ioctl_case("tcgets-dead", FD_DEAD, TCGETS, -1, EBADF, 2);
    ioctl_case("tcgets-file", FD_FILE, TCGETS, -1, ENOTTY, 2);
    ioctl_case("unknown-dead", FD_DEAD, 0x7fff, -1, EBADF, 1);
    ioctl_case("unknown-file", FD_FILE, 0x7fff, -1, ENOTTY, 1);
    ioctl_case("fionbio-dead", FD_DEAD, FIONBIO, -1, EBADF, 1);
    ioctl_case("fionbio-socket-dead", TAG_DEAD, FIONBIO, -1, EBADF, 0);
    ioctl_case("fionbio-socket-unix", TAG_UNIX, FIONBIO, -1, ENOTTY, 0);
    ioctl_case("fionread-socket-unix", TAG_UNIX, FIONREAD, -1, ENOTTY, 0);
    ioctl_case("ptn-master", FD_MASTER, TIOCGPTN, 0, EDOM, 1);
    ioctl_case("ptn-slave", FD_SLAVE, TIOCGPTN, -1, ENOTTY, 2);
    ioctl_case("ptn-dead", FD_DEAD, TIOCGPTN, -1, EBADF, 2);
    ioctl_case("winsz-dead", FD_DEAD, TIOCGWINSZ, -1, EBADF, 3);
    ioctl_case("winsz-int-min", INT_MIN, TIOCGWINSZ, -1, EBADF, 2);
    ioctl_case("swinsz-console-eperm", FD_CONS, TIOCSWINSZ, -1, EPERM, 2);
    ioctl_case("swinsz-dead", FD_DEAD, TIOCSWINSZ, -1, EBADF, 3);
    ioctl_case("sctty-dead", FD_DEAD, TIOCSCTTY, -1, EBADF, 2);

    printf("tests=%d failures=%d\n", tests, failures);
    return !!failures;
}
