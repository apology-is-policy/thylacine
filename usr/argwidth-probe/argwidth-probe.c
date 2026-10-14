// argwidth-probe -- the EL0 leg of ERRORS.md principle 6: a syscall argument
// that does not fit its declared width is refused, never truncated. The kernel
// test syscall.argwidth_probe spawns it and requires PASS.
//
// Each case makes the same call twice, one variable apart: once with the
// argument as its binding passes it, once with a bit set above that width. The
// wide call must answer the refusal; the narrow one must answer anything else,
// which is what a kernel that cast the high bits away would have answered for
// the wide call too.

#include <stdint.h>
#include <stddef.h>
#include <thyla/syscall.h>
#include <thyla/poll.h>

#define PASS 72

// The kernel's errno registry (kernel/include/thylacine/errno.h).
#define E_BADF       9
#define E_INVAL     22
#define E_TIMEDOUT 110

// No libt wrapper; the kernel test pins these numbers to the kernel's.
#define SYS_POSTNOTE_NR        47
#define SYS_BURROW_MAP_FILE_NR 126

#define W        (1ul << 32)
#define NO_PID   0x7ffffff0l
#define WAIT_NOHANG 1ul

static void put_dec(long v) {
    char b[24];
    int n = 0;
    unsigned long u = v < 0 ? (unsigned long)-v : (unsigned long)v;
    do { b[n++] = (char)('0' + u % 10); u /= 10; } while (u && n < 22);
    char o[24];
    int k = 0;
    if (v < 0) o[k++] = '-';
    while (n) o[k++] = b[--n];
    o[k] = 0;
    t_putstr(o);
}

__attribute__((noreturn))
static void fail(int line, const char *why, long got) {
    t_putstr("argwidth-probe: FAIL line ");
    put_dec(line);
    t_putstr(": ");
    t_putstr(why);
    t_putstr(" (got ");
    put_dec(got);
    t_putstr(")\n");
    t_exits(1);
}
#define CHECK(cond, why, got) do { if (!(cond)) fail(__LINE__, (why), (long)(got)); } while (0)

// The narrow call answers something other than the refusal; the wide one
// answers exactly the refusal.
#define PAIR(narrow, wide, refusal, what) do {                                  \
        long n_ = (narrow);                                                      \
        CHECK(n_ != (refusal), what ": the narrow call reaches past the check", n_); \
        long w_ = (wide);                                                        \
        CHECK(w_ == (refusal), what ": the wide call is refused", w_);           \
    } while (0)

// A raw call, so a word a libt wrapper declares narrower (int, unsigned int)
// still reaches the kernel whole.
static long sc(long nr, unsigned long a0, unsigned long a1, unsigned long a2,
               unsigned long a3, unsigned long a4, unsigned long a5) {
    register unsigned long x0 __asm__("x0") = a0;
    register unsigned long x1 __asm__("x1") = a1;
    register unsigned long x2 __asm__("x2") = a2;
    register unsigned long x3 __asm__("x3") = a3;
    register unsigned long x4 __asm__("x4") = a4;
    register unsigned long x5 __asm__("x5") = a5;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile ("svc #0"
                      : "+r"(x0)
                      : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                      : "memory", "cc");
    return (long)x0;
}

static uint8_t g_buf[256];
static struct t_stat g_st;
static volatile uint32_t g_word;

static void descriptors(void) {
    long rd = -1, wr = -1;
    CHECK(t_pipe(&rd, &wr) == 0, "pipe", rd);
    long file = t_open(T_WALK_OPEN_FROM_ROOT, "/bin/hello", 10, T_OREAD);
    CHECK(file >= 0, "open /bin/hello", file);
    long dir = t_open(T_WALK_OPEN_FROM_ROOT, "/bin", 4, T_OREAD);
    CHECK(dir >= 0, "open /bin", dir);

    g_buf[0] = 'w';
    PAIR(t_write(wr, g_buf, 1), t_write(wr | W, g_buf, 1), -E_BADF, "write");
    PAIR(t_read(rd, g_buf, 1), t_read(rd | W, g_buf, 1), -E_BADF, "read");
    PAIR(t_pread(file, g_buf, 1, 0), t_pread(file | W, g_buf, 1, 0), -E_BADF, "pread");
    PAIR(t_pwrite(wr, g_buf, 1, 0), t_pwrite(wr | W, g_buf, 1, 0), -E_BADF, "pwrite");
    PAIR(t_lseek(file, 0, T_SEEK_SET), t_lseek(file | W, 0, T_SEEK_SET), -E_BADF, "lseek fd");
    PAIR(t_walk_open(dir, "hello", 5, T_OREAD), t_walk_open(dir | W, "hello", 5, T_OREAD),
         -E_BADF, "walk_open");
    PAIR(t_fd_devclass(file), t_fd_devclass(file | W), -E_BADF, "fd_devclass");
    PAIR(t_fsync(wr, 0), t_fsync(wr | W, 0), -E_BADF, "fsync");
    // These three still answer a dead descriptor with the flat -1 (ERRORS.md ER-3).
    PAIR(t_fstat(file, &g_st), t_fstat(file | W, &g_st), -1, "fstat");
    PAIR(t_fd2path(file, (char *)g_buf, sizeof g_buf), t_fd2path(file | W, (char *)g_buf, sizeof g_buf),
         -1, "fd2path");
    PAIR(t_readdir(dir, g_buf, sizeof g_buf), t_readdir(dir | W, g_buf, sizeof g_buf), -1, "readdir");
    PAIR(t_open(dir, "hello", 5, T_OREAD), t_open(dir | W, "hello", 5, T_OREAD), -1, "open start fd");
    PAIR(t_dup(file, T_RIGHT_READ), t_dup(file | W, T_RIGHT_READ), -1, "dup");
    PAIR(sc(SYS_BURROW_MAP_FILE_NR, file, 0, 4096, T_BURROW_PROT_READ, 0, 0),
         sc(SYS_BURROW_MAP_FILE_NR, file | W, 0, 4096, T_BURROW_PROT_READ, 0, 0),
         -E_BADF, "burrow_map_file");

    // A refused close leaves the descriptor open.
    long victim = t_dup(file, T_RIGHT_READ);
    CHECK(victim >= 0, "dup for close", victim);
    long wide_close = t_close(victim | W);
    CHECK(wide_close == -E_BADF, "close: the wide call is refused", wide_close);
    long still = t_fd_devclass(victim);
    CHECK(still >= 0, "close: the refused close left the descriptor open", still);
    long narrow_close = t_close(victim);
    CHECK(narrow_close == 0, "close: the narrow call closes", narrow_close);

    // A whence that does not fit is refused like one that names no seek.
    PAIR(t_lseek(file, 0, T_SEEK_SET), t_lseek(file, 0, W | T_SEEK_SET), -E_INVAL, "lseek whence");
    long neg = t_lseek(file, 0, -1);
    CHECK(neg == -E_INVAL, "lseek: a negative whence is still refused", neg);

    // wstat: every field that does not fit is refused before the descriptor is
    // looked up. A closed descriptor keeps the narrow call from changing any
    // file: it passes the width check and stops at the lookup's -1.
    long dead = t_dup(file, T_RIGHT_READ);
    CHECK(dead >= 0, "dup for wstat", dead);
    CHECK(t_close(dead) == 0, "close for wstat", dead);
    PAIR(t_wstat(dead, T_WSTAT_MODE, 0644, 0, 0), t_wstat(dead, W | T_WSTAT_MODE, 0644, 0, 0),
         -E_INVAL, "wstat valid");
    PAIR(t_wstat(dead, T_WSTAT_MODE, 0644, 0, 0), t_wstat(dead, T_WSTAT_MODE, W | 0644, 0, 0),
         -E_INVAL, "wstat mode");
    PAIR(t_wstat(dead, T_WSTAT_UID, 0, 5, 0), t_wstat(dead, T_WSTAT_UID, 0, W | 5, 0),
         -E_INVAL, "wstat uid");
    PAIR(t_wstat(dead, T_WSTAT_GID, 0, 0, 5), t_wstat(dead, T_WSTAT_GID, 0, 0, W | 5),
         -E_INVAL, "wstat gid");

    // A directory a mutation names (a path handle is born with write rights):
    // the narrow call reaches the directory and stops at the missing name, so
    // nothing changes; no answer past the lookup is EBADF.
    long pd = t_open(T_WALK_OPEN_FROM_ROOT, "/bin", 4, T_OPATH);
    CHECK(pd >= 0, "open /bin as a path", pd);
    PAIR(t_unlink(pd, "argwidth-none", 13, 0), t_unlink(pd | W, "argwidth-none", 13, 0),
         -E_BADF, "unlink parent fd");
    PAIR(t_rename(pd, "argwidth-none", 13, pd, "argwidth-nil", 12),
         t_rename(pd | W, "argwidth-none", 13, pd, "argwidth-nil", 12), -E_BADF, "rename old dir fd");
    PAIR(t_rename(pd, "argwidth-none", 13, pd, "argwidth-nil", 12),
         t_rename(pd, "argwidth-none", 13, pd | W, "argwidth-nil", 12), -E_BADF, "rename new dir fd");
    t_close(pd);

    t_close(dir);
    t_close(file);
    t_close(rd);
    t_close(wr);
}

// A pid or a timeout is signed: its binding sign-extends it, so a negative
// value arrives with its high bits set and must still be read as itself.
static void signed_words(void) {
    PAIR(t_getpgid(0), t_getpgid(W | 0), -E_INVAL, "getpgid");
    PAIR(t_getsid(0), t_getsid(W | 0), -E_INVAL, "getsid");
    PAIR(t_setpgid(0, 0), t_setpgid(W | 0, 0), -E_INVAL, "setpgid pid");
    PAIR(t_setpgid(0, 0), t_setpgid(0, W | 0), -E_INVAL, "setpgid pgid");
    long neg = t_getpgid(-1);
    CHECK(neg != -E_INVAL, "getpgid: a sign-extended -1 is a pid, not a width", neg);

    // want_pid -1 (any child), sign-extended as an int binding passes it.
    PAIR(sc(T_SYS_WAIT_PID, (unsigned long)-1l, WAIT_NOHANG, 0, 0, 0, 0),
         sc(T_SYS_WAIT_PID, W | 0xfffffffful, WAIT_NOHANG, 0, 0, 0, 0),
         -E_INVAL, "wait_pid want_pid");

    long rd = -1, wr = -1;
    CHECK(t_pipe(&rd, &wr) == 0, "pipe", rd);
    struct pollfd pfd = { .fd = (int)rd, .events = POLLIN, .revents = 0 };
    PAIR(t_poll(&pfd, 1, 0), t_poll(&pfd, 1, (long)(W | 0)), -E_INVAL, "poll timeout");
    g_buf[0] = 'p';
    CHECK(t_write(wr, g_buf, 1) == 1, "poll: a byte to read", 0);
    long ready = t_poll(&pfd, 1, -1);
    CHECK(ready == 1, "poll: a sign-extended -1 still waits for the ready fd", ready);
    t_close(rd);
    t_close(wr);

    static const char name[] = "interrupt";
    PAIR(sc(SYS_POSTNOTE_NR, (unsigned long)NO_PID, (unsigned long)name, sizeof name - 1, 0, 0, 0),
         sc(SYS_POSTNOTE_NR, W | (unsigned long)NO_PID, (unsigned long)name, sizeof name - 1, 0, 0, 0),
         -E_INVAL, "postnote pid");
}

// torpor compares the word bit for bit: an int binding sign-extends it, an
// unsigned one zero-extends it, and both name the same word.
static void torpor_words(void) {
    g_word = 0x80000001u;
    unsigned long va = (unsigned long)&g_word;
    long zero = sc(T_SYS_TORPOR_WAIT, va, 0x80000001ul, 1000, 0, 0, 0);
    CHECK(zero == -E_TIMEDOUT, "torpor_wait: the zero-extended word matches", zero);
    long sign = sc(T_SYS_TORPOR_WAIT, va, 0xffffffff80000001ul, 1000, 0, 0, 0);
    CHECK(sign == -E_TIMEDOUT, "torpor_wait: the sign-extended word matches", sign);
    long wide = sc(T_SYS_TORPOR_WAIT, va, W | 0x80000001ul, 1000, 0, 0, 0);
    CHECK(wide == -E_INVAL, "torpor_wait: a word with a stray high bit is refused", wide);
    long half = sc(T_SYS_TORPOR_WAIT, va, 0xffffffff00000001ul, 1000, 0, 0, 0);
    CHECK(half == -E_INVAL, "torpor_wait: high ones over a clear bit 31 are refused", half);

    PAIR(sc(T_SYS_TORPOR_WAKE, va, 1, 0, 0, 0, 0), sc(T_SYS_TORPOR_WAKE, va, W | 1, 0, 0, 0, 0),
         -E_INVAL, "torpor_wake count");
}

int main(void) {
    descriptors();
    signed_words();
    torpor_words();
    t_putstr("argwidth-probe: PASS\n");
    t_exits(PASS);
}
