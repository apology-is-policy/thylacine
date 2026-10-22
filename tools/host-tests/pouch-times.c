#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
/* The AT_ and UTIME_ values are the musl tree's, from the driver: the host's differ
 * (Darwin's UTIME_NOW is -1, which would hide a negative nanosecond). */
#undef AT_FDCWD
#undef AT_SYMLINK_NOFOLLOW
#undef AT_EMPTY_PATH
#undef UTIME_NOW
#undef UTIME_OMIT
#define AT_FDCWD MUSL_AT_FDCWD
#define AT_SYMLINK_NOFOLLOW MUSL_AT_SYMLINK_NOFOLLOW
#define AT_EMPTY_PATH MUSL_AT_EMPTY_PATH
#define UTIME_NOW MUSL_UTIME_NOW
#define UTIME_OMIT MUSL_UTIME_OMIT
/* A flag outside every set the code under test accepts. */
#define AT_BOGUS 0x40000
_Static_assert(!(AT_BOGUS & (MUSL_AT_SYMLINK_NOFOLLOW | MUSL_AT_EMPTY_PATH)), "AT_BOGUS collides");
#define hidden
#include "fs-under-test.h"
#define NCALL 8
static long nums[NCALL], args[NCALL][7];
static char paths[NCALL][POUCH_PATH_MAX + 8];
static struct timespec sent[NCALL][2];
static int calls, failures, checks, fstat_calls, fstat_errno;
static long r_open, r_wstat;
static unsigned fstat_mode;
/* The kernel's side of each call the code under test makes; x6 is read as the kernel does. */
static long sys(long n, long a, long b, long c, long d, long e, long f, long g) {
 int i = calls++, times_in = n == SYS_thyla_wstat && (b & (POUCH_WSTAT_ATIME_SET | POUCH_WSTAT_MTIME_SET));
 if (i < NCALL) {
  nums[i] = n; args[i][0] = a; args[i][1] = b; args[i][2] = c; args[i][3] = d; args[i][4] = e; args[i][5] = f; args[i][6] = g;
  if (n == SYS_thyla_open && c >= 0 && c < (long)sizeof paths[0]) { memcpy(paths[i], (const void *)b, (size_t)c); paths[i][c] = 0; }
  if (times_in && g) memcpy(sent[i], (const void *)g, sizeof sent[i]);
 }
 /* The kernel's copy-in of a null x6 faults. */
 if (times_in && !g) return -EFAULT;
 if (n == SYS_thyla_open) return r_open;
 if (n == SYS_thyla_wstat) return r_wstat;
 return 0;
}
/* musl's own argument-count dispatch: every argument is widened to long, up to x6. */
#define __scc(X) ((long)(X))
#define NARGS_X(a,b,c,d,e,f,g,h,n,...) n
#define NARGS(...) NARGS_X(__VA_ARGS__,7,6,5,4,3,2,1,0,)
#define CAT_X(a,b) a##b
#define CAT(a,b) CAT_X(a,b)
#define __syscall(...) CAT(sc,NARGS(__VA_ARGS__))(__VA_ARGS__)
#define sc1(n,a) sys(n,__scc(a),0,0,0,0,0,0)
#define sc4(n,a,b,c,d) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),0,0,0)
#define sc6(n,a,b,c,d,e,f) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),__scc(f),0)
#define sc7(n,a,b,c,d,e,f,g) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),__scc(f),__scc(g))
/* The series' own decoder (src/internal/syscall_ret.c): a -1 reads EIO, as on the device. */
#include "syscall-ret-under-test.c"
#define syscall(...) __syscall_ret(__syscall(__VA_ARGS__))
/* The host's mode_t is promotable; the guest's is not. */
#define mode_t unsigned
struct pouch_stat { mode_t st_mode; };
#define stat pouch_stat
static int mock_fstat(int fd, struct pouch_stat *st) {
 (void)fd; fstat_calls++;
 if (fstat_errno) { errno = fstat_errno; return -1; }
 st->st_mode = fstat_mode; return 0;
}
#define fstat mock_fstat
#define utimensat tested_utimensat
#include "utimensat-under-test.c"
#undef utimensat
#define fchmod tested_fchmod
#include "fchmod-under-test.c"
#undef fchmod
#define fchmodat tested_fchmodat
#include "fchmodat-under-test.c"
#undef fchmodat
static int at(long n) { for (int i = 0; i < calls && i < NCALL; i++) if (nums[i] == n) return i; return -1; }
static int count(long n) { int k = 0; for (int i = 0; i < calls && i < NCALL; i++) k += nums[i] == n; return k; }
static int closed(long fd) { int i = at(SYS_close); return count(SYS_close) == 1 && args[i][0] == fd; }
/* The one wstat the call made: its fd, its mask, and what x6 carried. */
static int wstat_is(long fd, unsigned valid, const void *x6) {
 int i = at(SYS_thyla_wstat);
 return count(SYS_thyla_wstat) == 1 && args[i][0] == fd && args[i][1] == (long)valid && args[i][2] == 0 && args[i][3] == 0 &&
        args[i][4] == 0 && args[i][5] == 0 && args[i][6] == (long)x6;
}
static int sent_is(int which, long sec, long nsec) { int i = at(SYS_thyla_wstat); return i >= 0 && sent[i][which].tv_sec == sec && sent[i][which].tv_nsec == nsec; }
static int refused(int r, int e) { return r == -1 && errno == e && calls == 0; }
#define CASE(name, expr) do { ++checks; int ok = (expr); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
static void reset(void) {
 calls = fstat_calls = fstat_errno = 0; fstat_mode = 0; r_open = 7; r_wstat = 0;
 memset(nums, 0, sizeof nums); memset(args, 0, sizeof args); memset(paths, 0, sizeof paths); memset(sent, 0, sizeof sent); errno = EDOM;
}
#define A POUCH_WSTAT_ATIME
#define M POUCH_WSTAT_MTIME
#define AS POUCH_WSTAT_ATIME_SET
#define MS POUCH_WSTAT_MTIME_SET
int main(void) {
 static char full[POUCH_PATH_MAX + 1], over[POUCH_PATH_MAX + 2];
 memset(full, 'a', POUCH_PATH_MAX); memset(over, 'a', POUCH_PATH_MAX + 1);
 struct timespec tv[2];
#define TV(s0, n0, s1, n1) (tv[0].tv_sec = (s0), tv[0].tv_nsec = (n0), tv[1].tv_sec = (s1), tv[1].tv_nsec = (n1), tv)
 /* Pouch's numbers are the kernel's own. */
 CASE("number-wstat", SYS_thyla_wstat == KERNEL_SYS_WSTAT);
 CASE("number-mode", POUCH_WSTAT_MODE == KERNEL_T_WSTAT_MODE);
 CASE("number-atime", A == KERNEL_T_WSTAT_ATIME);
 CASE("number-mtime", M == KERNEL_T_WSTAT_MTIME);
 CASE("number-atime-set", AS == KERNEL_T_WSTAT_ATIME_SET);
 CASE("number-mtime-set", MS == KERNEL_T_WSTAT_MTIME_SET);
 CASE("number-opath", POUCH_OPATH == KERNEL_WALK_OPEN_OPATH);
 CASE("number-nofollow", POUCH_ONOFOLLOW == KERNEL_WALK_OPEN_NOFOLLOW);
 CASE("number-path-max", POUCH_PATH_MAX == KERNEL_OPEN_PATH_MAX);

 /* Null times is the touch: both bits, no _SET, nothing read from x6. */
 reset(); CASE("touch-null", tested_utimensat(AT_FDCWD, "/f", 0, 0) == 0 && nums[0] == SYS_thyla_open && args[0][0] == POUCH_FROM_ROOT &&
                strcmp(paths[0], "/f") == 0 && args[0][2] == 2 && args[0][3] == (long)POUCH_OPATH && wstat_is(7, A | M, 0) && closed(7));
 reset(); CASE("touch-both-now", tested_utimensat(AT_FDCWD, "/f", TV(9, UTIME_NOW, 9, UTIME_NOW), 0) == 0 && wstat_is(7, A | M, tv));
 /* A value is its bit and its _SET bit, the pair handed to the kernel at x6. */
 reset(); CASE("explicit-both", tested_utimensat(AT_FDCWD, "/f", TV(1, 2, 3, 4), 0) == 0 && wstat_is(7, A | M | AS | MS, tv) && sent_is(0, 1, 2) && sent_is(1, 3, 4) && closed(7));
 reset(); CASE("explicit-nsec-max", tested_utimensat(AT_FDCWD, "/f", TV(1, 999999999, 0, 0), 0) == 0 && wstat_is(7, A | M | AS | MS, tv) && sent_is(0, 1, 999999999));
 /* UTIME_OMIT leaves its time out of the mask; UTIME_NOW is the bit alone. */
 reset(); CASE("omit-atime", tested_utimensat(AT_FDCWD, "/f", TV(1, UTIME_OMIT, 5, 6), 0) == 0 && wstat_is(7, M | MS, tv) && sent_is(1, 5, 6));
 reset(); CASE("omit-mtime-now-atime", tested_utimensat(AT_FDCWD, "/f", TV(1, UTIME_NOW, 2, UTIME_OMIT), 0) == 0 && wstat_is(7, A, tv));
 reset(); CASE("now-mtime-value-atime", tested_utimensat(AT_FDCWD, "/f", TV(7, 8, 0, UTIME_NOW), 0) == 0 && wstat_is(7, A | AS | M, tv) && sent_is(0, 7, 8));
 /* Both UTIME_OMIT does nothing: no path, descriptor or flag is looked at (ASan would see a null path read). */
 reset(); CASE("both-omit", tested_utimensat(AT_FDCWD, "/f", TV(1, UTIME_OMIT, 2, UTIME_OMIT), 0) == 0 && calls == 0);
 reset(); CASE("both-omit-reads-nothing", tested_utimensat(-1, 0, TV(1, UTIME_OMIT, 2, UTIME_OMIT), AT_BOGUS) == 0 && calls == 0);
 /* A nanosecond that is neither a sentinel nor in [0, 999999999] is EINVAL, before anything traps. */
 reset(); CASE("nsec-negative", refused(tested_utimensat(AT_FDCWD, "/f", TV(1, -1, 2, UTIME_OMIT), 0), EINVAL));
 reset(); CASE("nsec-billion", refused(tested_utimensat(AT_FDCWD, "/f", TV(1, UTIME_NOW, 2, 1000000000), 0), EINVAL));
 reset(); CASE("nsec-between-sentinels", refused(tested_utimensat(AT_FDCWD, "/f", TV(1, UTIME_OMIT - 1, 2, 0), 0), EINVAL));
 /* The seconds cross by value: the kernel judges them (a time before 1970 is its EINVAL). */
 reset(); r_wstat = -EINVAL; CASE("sec-negative-kernel", tested_utimensat(AT_FDCWD, "/f", TV(-1, 0, 0, UTIME_OMIT), 0) == -1 && errno == EINVAL && sent_is(0, -1, 0) && closed(7));

 /* The path form's flags: AT_SYMLINK_NOFOLLOW opens the link itself; any other flag is EINVAL. */
 reset(); CASE("nofollow", tested_utimensat(AT_FDCWD, "/l", 0, AT_SYMLINK_NOFOLLOW) == 0 && args[0][3] == (long)(POUCH_OPATH | POUCH_ONOFOLLOW) && wstat_is(7, A | M, 0) && fstat_calls == 0);
 reset(); CASE("bad-flag", refused(tested_utimensat(AT_FDCWD, "/f", 0, AT_BOGUS), EINVAL));
 reset(); CASE("bad-flag-beside-nofollow", refused(tested_utimensat(AT_FDCWD, "/f", 0, AT_BOGUS | AT_SYMLINK_NOFOLLOW), EINVAL));
 reset(); CASE("bad-flag-before-efault", refused(tested_utimensat(AT_FDCWD, 0, 0, AT_BOGUS), EINVAL));
 reset(); CASE("cwd-null-path-efault", refused(tested_utimensat(AT_FDCWD, 0, 0, 0), EFAULT));
 reset(); CASE("dirfd-enotsup", refused(tested_utimensat(3, "f", 0, 0), ENOTSUP));
 reset(); CASE("empty-path", refused(tested_utimensat(AT_FDCWD, "", 0, 0), ENOENT));
 reset(); CASE("too-long", refused(tested_utimensat(AT_FDCWD, over, 0, 0), ENAMETOOLONG));
 reset(); CASE("longest-path", tested_utimensat(AT_FDCWD, full, 0, 0) == 0 && args[0][2] == (long)POUCH_PATH_MAX);
 /* The open's answer is the call's, and nothing is set or closed. */
 /* AT_EMPTY_PATH with "" is the descriptor itself, or the working directory for AT_FDCWD; with a name it changes nothing. */
 reset(); CASE("empty-path-fd", tested_utimensat(5, "", TV(1, 2, 3, 4), AT_EMPTY_PATH) == 0 && calls == 1 && wstat_is(5, A | M | AS | MS, tv) && sent_is(0, 1, 2));
 reset(); CASE("empty-path-fd-nofollow", tested_utimensat(5, "", 0, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) == 0 && calls == 1 && wstat_is(5, A | M, 0));
 reset(); r_wstat = -EACCES; CASE("empty-path-fd-eperm", tested_utimensat(5, "", TV(1, 2, 3, 4), AT_EMPTY_PATH) == -1 && errno == EPERM && calls == 1);
 reset(); CASE("empty-path-cwd", tested_utimensat(AT_FDCWD, "", 0, AT_EMPTY_PATH) == 0 && nums[0] == SYS_thyla_open && strcmp(paths[0], ".") == 0 &&
                args[0][2] == 1 && args[0][3] == (long)POUCH_OPATH && wstat_is(7, A | M, 0) && closed(7));
 reset(); CASE("empty-flag-cwd-name", tested_utimensat(AT_FDCWD, "/f", 0, AT_EMPTY_PATH) == 0 && strcmp(paths[0], "/f") == 0 && wstat_is(7, A | M, 0));
 reset(); CASE("empty-flag-dirfd-name", refused(tested_utimensat(3, "f", 0, AT_EMPTY_PATH), ENOTSUP));
 reset(); r_open = -ENOENT; CASE("absent", tested_utimensat(AT_FDCWD, "/nope", 0, 0) == -1 && errno == ENOENT && count(SYS_thyla_wstat) == 0 && count(SYS_close) == 0);

 /* The descriptor form: a null path with a real descriptor, no open, flags 0 only. */
 reset(); CASE("fd-touch", tested_utimensat(5, 0, 0, 0) == 0 && calls == 1 && wstat_is(5, A | M, 0));
 reset(); CASE("fd-explicit", tested_utimensat(5, 0, TV(1, 2, 3, UTIME_OMIT), 0) == 0 && calls == 1 && wstat_is(5, A | AS, tv) && sent_is(0, 1, 2));
 reset(); CASE("fd-nofollow-einval", refused(tested_utimensat(5, 0, 0, AT_SYMLINK_NOFOLLOW), EINVAL));
 reset(); CASE("fd-empty-path-einval", refused(tested_utimensat(5, 0, 0, AT_EMPTY_PATH), EINVAL));
 reset(); r_wstat = -EBADF; CASE("fd-ebadf", tested_utimensat(-1, 0, 0, 0) == -1 && errno == EBADF && calls == 1);

 /* Denials: the kernel's EACCES is EPERM for every request but the touch. */
 reset(); r_wstat = -EACCES; CASE("explicit-eperm", tested_utimensat(AT_FDCWD, "/f", TV(1, 2, 3, 4), 0) == -1 && errno == EPERM && closed(7));
 reset(); r_wstat = -EACCES; CASE("one-now-eperm", tested_utimensat(AT_FDCWD, "/f", TV(1, UTIME_NOW, 2, UTIME_OMIT), 0) == -1 && errno == EPERM && closed(7));
 reset(); r_wstat = -EACCES; CASE("touch-eacces", tested_utimensat(AT_FDCWD, "/f", 0, 0) == -1 && errno == EACCES && closed(7));
 reset(); r_wstat = -EACCES; CASE("touch-now-now-eacces", tested_utimensat(AT_FDCWD, "/f", TV(1, UTIME_NOW, 2, UTIME_NOW), 0) == -1 && errno == EACCES);
 reset(); r_wstat = -EACCES; CASE("fd-explicit-eperm", tested_utimensat(5, 0, TV(1, 2, 3, 4), 0) == -1 && errno == EPERM);
 reset(); r_wstat = -EACCES; CASE("fd-touch-eacces", tested_utimensat(5, 0, 0, 0) == -1 && errno == EACCES);
 /* A component's search denied is the open's EACCES, never an ownership EPERM. */
 reset(); r_open = -EACCES; CASE("open-eacces-stays", tested_utimensat(AT_FDCWD, "/d/f", TV(1, 2, 3, 4), 0) == -1 && errno == EACCES && count(SYS_thyla_wstat) == 0);
 /* Every other answer crosses as it is. */
 reset(); r_wstat = -EROFS; CASE("erofs", tested_utimensat(AT_FDCWD, "/f", TV(1, 2, 3, 4), 0) == -1 && errno == EROFS && closed(7));
 reset(); r_wstat = -EOPNOTSUPP; CASE("eopnotsupp", tested_utimensat(AT_FDCWD, "/f", 0, 0) == -1 && errno == EOPNOTSUPP);

 /* chmod's only refusal is ownership: the wstat's EACCES is EPERM. */
 reset(); CASE("fchmod", tested_fchmod(4, 0640) == 0 && wstat_is(4, POUCH_WSTAT_MODE, 0) == 0 && args[0][0] == 4 && args[0][1] == POUCH_WSTAT_MODE && args[0][2] == 0640);
 reset(); CASE("fchmod-masks", tested_fchmod(4, 04755) == 0 && args[0][2] == 0755);
 reset(); r_wstat = -EACCES; CASE("fchmod-eperm", tested_fchmod(4, 0640) == -1 && errno == EPERM);
 reset(); r_wstat = -EBADF; CASE("fchmod-ebadf", tested_fchmod(4, 0640) == -1 && errno == EBADF);
 reset(); r_wstat = -EACCES; CASE("fchmodat-eperm", tested_fchmodat(AT_FDCWD, "/f", 0640, 0) == -1 && errno == EPERM && closed(7));
 reset(); r_wstat = -EACCES; fstat_mode = S_IFREG | 0600; CASE("lchmod-eperm", tested_fchmodat(AT_FDCWD, "/f", 0640, AT_SYMLINK_NOFOLLOW) == -1 && errno == EPERM && fstat_calls == 1 && closed(7));
 reset(); r_open = -EACCES; CASE("fchmodat-open-eacces", tested_fchmodat(AT_FDCWD, "/d/f", 0640, 0) == -1 && errno == EACCES && count(SYS_thyla_wstat) == 0);
 reset(); fstat_errno = EACCES; CASE("lchmod-fstat-eacces", tested_fchmodat(AT_FDCWD, "/f", 0640, AT_SYMLINK_NOFOLLOW) == -1 && errno == EACCES && count(SYS_thyla_wstat) == 0 && closed(7));
 reset(); r_wstat = -EROFS; CASE("fchmodat-erofs", tested_fchmodat(AT_FDCWD, "/f", 0640, 0) == -1 && errno == EROFS && closed(7));
 printf("tests=%d failures=%d\n", checks, failures); return !!failures;
}
