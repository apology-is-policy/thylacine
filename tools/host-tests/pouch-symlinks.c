#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef AT_NO_AUTOMOUNT
#define AT_NO_AUTOMOUNT 0x800
#endif
#ifndef AT_STATX_SYNC_TYPE
#define AT_STATX_SYNC_TYPE 0x6000
#define AT_STATX_DONT_SYNC 0x4000
#endif
/* A flag outside every set the code under test accepts. */
#define AT_BOGUS 0x40000
_Static_assert(!(O_PATH & (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_NOFOLLOW | O_DIRECTORY | O_CLOEXEC | (O_TMPFILE & ~O_DIRECTORY))), "host O_PATH collides");
_Static_assert(!(AT_BOGUS & (AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT | AT_EMPTY_PATH | AT_STATX_SYNC_TYPE)), "AT_BOGUS collides");
#define hidden
#define weak_alias(old, new)
#define POUCH_SOCK_TAG 0x40000000
#include "fs-under-test.h"
#define NCALL 16
static long nums[NCALL], args[NCALL][6];
static char paths[NCALL][POUCH_PATH_MAX + 8];
static int calls, failures, checks, parent_calls, fstat_calls, fstat_fd, fstat_errno;
static long r_open, r_create, r_stat, r_wstat, r_readlink;
static unsigned fstat_mode, t_mode;
static unsigned long t_size;
static const char *rl_target;
static void *pouch_sock_resolve(int fd) { (void)fd; return 0; }
static void note_path(int i, long ptr, long len) {
 if (i < NCALL && len >= 0 && len < (long)sizeof paths[0]) { memcpy(paths[i], (const void *)ptr, (size_t)len); paths[i][len] = 0; }
}
/* The kernel's side of each call the code under test makes. */
static long sys(long n, long a, long b, long c, long d, long e, long f) {
 int i = calls++;
 if (i < NCALL) { nums[i] = n; args[i][0] = a; args[i][1] = b; args[i][2] = c; args[i][3] = d; args[i][4] = e; args[i][5] = f; }
 if (n == SYS_thyla_open) { note_path(i, b, c); return r_open; }
 if (n == SYS_thyla_walk_create) return r_create;
 if (n == SYS_thyla_stat || n == SYS_thyla_lstat) {
  note_path(i, a, b);
  /* struct t_stat at the kernel's offsets: size at 0, mode at 40. */
  if (!r_stat) { memcpy((char *)c, &t_size, 8); memcpy((char *)c + 40, &t_mode, 4); }
  return r_stat;
 }
 if (n == SYS_thyla_wstat) return r_wstat;
 if (n == SYS_thyla_readlink) {
  size_t k = strlen(rl_target);
  note_path(i, a, b);
  if (r_readlink) return r_readlink;
  if (k > (size_t)d) k = (size_t)d;
  memcpy((char *)c, rl_target, k);
  return (long)k;
 }
 if (n == SYS_thyla_getpid) return 42;
 if (n == SYS_read) { memcpy((char *)b, "/bin/hello", 10); return 10; }
 return 0;
}
/* musl's own argument-count dispatch: every argument is widened to long. */
#define __scc(X) ((long)(X))
#define NARGS_X(a,b,c,d,e,f,g,h,n,...) n
#define NARGS(...) NARGS_X(__VA_ARGS__,7,6,5,4,3,2,1,0,)
#define CAT_X(a,b) a##b
#define CAT(a,b) CAT_X(a,b)
#define __syscall(...) CAT(sc,NARGS(__VA_ARGS__))(__VA_ARGS__)
#define sc0(n) sys(n,0,0,0,0,0,0)
#define sc1(n,a) sys(n,__scc(a),0,0,0,0,0)
#define sc2(n,a,b) sys(n,__scc(a),__scc(b),0,0,0,0)
#define sc3(n,a,b,c) sys(n,__scc(a),__scc(b),__scc(c),0,0,0)
#define sc4(n,a,b,c,d) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),0,0)
#define sc5(n,a,b,c,d,e) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),0)
#define sc6(n,a,b,c,d,e,f) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),__scc(f))
#define __syscall2(n,a,b) sc2(n,a,b)
static long __syscall_ret(unsigned long r) { if (r > -4096UL) { errno = -(long)r; return -1; } return (long)r; }
int __pouch_open_parent(int atfd, const char *path, const char **leaf, size_t *llen, int *dirreq) {
 (void)atfd; parent_calls++; *leaf = path; *llen = strlen(path); *dirreq = 0; return 3;
}
/* The host's mode_t is promotable; the guest's is not. */
#define mode_t unsigned
/* musl's aarch64 struct stat field names, for the fstatat translation. */
struct pouch_stat {
 dev_t st_dev; ino_t st_ino; mode_t st_mode; nlink_t st_nlink; uid_t st_uid; gid_t st_gid; dev_t st_rdev;
 off_t st_size; blksize_t st_blksize; blkcnt_t st_blocks; struct timespec st_atim, st_mtim, st_ctim;
};
#define stat pouch_stat
static int mock_fstat(int fd, struct pouch_stat *st) {
 fstat_calls++; fstat_fd = fd;
 if (fstat_errno) { errno = fstat_errno; return -1; }
 memset(st, 0, sizeof *st); st->st_mode = fstat_mode; return 0;
}
#define fstat mock_fstat
#include "descriptor-under-test.h"
#define openat tested_openat
#include "openat-under-test.c"
#undef openat
#include "fstatat-under-test.c"
#define fchmodat tested_fchmodat
#include "fchmodat-under-test.c"
#undef fchmodat
#define readlinkat tested_readlinkat
#include "readlinkat-under-test.c"
#undef readlinkat
static int at(long n) { for (int i = 0; i < calls && i < NCALL; i++) if (nums[i] == n) return i; return -1; }
static int count(long n) { int k = 0; for (int i = 0; i < calls && i < NCALL; i++) k += nums[i] == n; return k; }
static int closed(long fd) { int i = at(SYS_close); return count(SYS_close) == 1 && args[i][0] == fd; }
#define CASE(name, expr) do { ++checks; int ok = (expr); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
static void reset(void) {
 calls = parent_calls = fstat_calls = fstat_errno = 0; fstat_fd = -1; fstat_mode = t_mode = 0; t_size = 0;
 r_open = 7; r_create = r_stat = r_wstat = r_readlink = 0; rl_target = ""; memset(paths, 0, sizeof paths); errno = EDOM;
}
int main(void) {
 static char full[POUCH_PATH_MAX + 1], over[POUCH_PATH_MAX + 2];
 struct pouch_stat st;
 char buf[64];
 int i;
 memset(full, 'a', POUCH_PATH_MAX); memset(over, 'a', POUCH_PATH_MAX + 1);
 /* Pouch's numbers are the kernel's own. */
 CASE("number-stat", SYS_thyla_stat == KERNEL_SYS_STAT);
 CASE("number-lstat", SYS_thyla_lstat == KERNEL_SYS_LSTAT);
 CASE("number-readlink", SYS_thyla_readlink == KERNEL_SYS_READLINK);
 CASE("number-nofollow", POUCH_ONOFOLLOW == KERNEL_WALK_OPEN_NOFOLLOW);
 CASE("number-opath", POUCH_OPATH == KERNEL_WALK_OPEN_OPATH);
 CASE("number-path-max", POUCH_PATH_MAX == KERNEL_OPEN_PATH_MAX);

 /* openat: O_NOFOLLOW and O_PATH reach the kernel as omode bits. */
 reset(); CASE("open-nofollow-bit", tested_openat(AT_FDCWD, "l", O_RDONLY | O_NOFOLLOW) == 7 && calls == 1 && nums[0] == SYS_thyla_open && args[0][3] == (long)(POUCH_OREAD | POUCH_ONOFOLLOW));
 reset(); r_open = -ELOOP; CASE("open-nofollow-eloop", tested_openat(AT_FDCWD, "l", O_RDONLY | O_NOFOLLOW) == -1 && errno == ELOOP);
 reset(); CASE("open-opath", tested_openat(AT_FDCWD, "f", O_PATH) == 7 && args[0][3] == (long)POUCH_OPATH);
 reset(); CASE("open-opath-nofollow", tested_openat(AT_FDCWD, "l", O_PATH | O_NOFOLLOW) == 7 && args[0][3] == (long)(POUCH_OPATH | POUCH_ONOFOLLOW));
 /* O_PATH beats every other flag, before any of them is looked at. */
 reset(); CASE("open-opath-beats-creat", tested_openat(AT_FDCWD, "f", O_PATH | O_CREAT | O_TRUNC | O_WRONLY | O_APPEND, 0644) == 7 && parent_calls == 0 && count(SYS_thyla_walk_create) == 0 && args[0][3] == (long)POUCH_OPATH && count(SYS_lseek) == 0);
 reset(); fstat_mode = S_IFDIR; CASE("open-opath-beats-creat-directory", tested_openat(AT_FDCWD, "d", O_PATH | O_CREAT | O_DIRECTORY, 0644) == 7 && args[0][3] == (long)POUCH_OPATH);
 reset(); CASE("open-creat-directory-einval", tested_openat(AT_FDCWD, "d", O_RDWR | O_CREAT | O_DIRECTORY, 0644) == -1 && errno == EINVAL && calls == 0 && parent_calls == 0);
 /* O_DIRECTORY without O_NOFOLLOW: the kernel's trailing-slash gate. */
 reset(); fstat_mode = S_IFDIR; CASE("open-directory-slash", tested_openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY) == 7 && strcmp(paths[0], "d/") == 0 && args[0][2] == 2 && fstat_calls == 0);
 reset(); fstat_mode = S_IFDIR; CASE("open-directory-slash-once", tested_openat(AT_FDCWD, "d/", O_RDONLY | O_DIRECTORY) == 7 && strcmp(paths[0], "d/") == 0 && args[0][2] == 2 && fstat_calls == 0);
 reset(); r_open = -ENOTDIR; CASE("open-directory-gate-errno", tested_openat(AT_FDCWD, "f", O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR && fstat_calls == 0);
 reset(); fstat_mode = S_IFREG; CASE("open-directory-no-room", tested_openat(AT_FDCWD, full, O_RDONLY | O_DIRECTORY) == -1 && errno == ENOTDIR && args[0][2] == (long)POUCH_PATH_MAX && closed(7));
 /* With O_NOFOLLOW no slash is added, and the descriptor is checked after it opens. */
 reset(); fstat_mode = S_IFREG; CASE("open-directory-nofollow-file", tested_openat(AT_FDCWD, "f", O_RDONLY | O_DIRECTORY | O_NOFOLLOW) == -1 && errno == ENOTDIR && strcmp(paths[0], "f") == 0 && fstat_fd == 7 && closed(7));
 reset(); fstat_mode = S_IFDIR; CASE("open-directory-nofollow-dir", tested_openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW) == 7 && fstat_calls == 1 && count(SYS_close) == 0);
 reset(); fstat_mode = S_IFLNK; CASE("open-directory-opath-nofollow-link", tested_openat(AT_FDCWD, "l", O_PATH | O_DIRECTORY | O_NOFOLLOW) == -1 && errno == ENOTDIR && strcmp(paths[0], "l") == 0 && closed(7));
 reset(); fstat_errno = EACCES; CASE("open-directory-fstat-fails", tested_openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW) == -1 && errno == EACCES && closed(7));
 /* EBADF from that fstat: a peer closed the number, which may be another thread's now. */
 reset(); fstat_errno = EBADF; CASE("open-directory-fstat-ebadf", tested_openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW) == -1 && errno == EBADF && count(SYS_close) == 0);
 /* O_TRUNC beside O_DIRECTORY is never sent, so no refusal truncates; a directory is EISDIR. */
 reset(); fstat_mode = S_IFREG; CASE("open-directory-nofollow-trunc-file", tested_openat(AT_FDCWD, "f", O_WRONLY | O_DIRECTORY | O_TRUNC | O_NOFOLLOW) == -1 && errno == ENOTDIR && !(args[0][3] & (long)POUCH_OTRUNC) && closed(7));
 reset(); fstat_mode = S_IFREG; CASE("open-directory-no-room-trunc", tested_openat(AT_FDCWD, full, O_WRONLY | O_DIRECTORY | O_TRUNC) == -1 && errno == ENOTDIR && !(args[0][3] & (long)POUCH_OTRUNC) && closed(7));
 reset(); CASE("open-directory-trunc-eisdir", tested_openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY | O_TRUNC) == -1 && errno == EISDIR && strcmp(paths[0], "d/") == 0 && !(args[0][3] & (long)POUCH_OTRUNC) && fstat_calls == 0 && closed(7));
 reset(); fstat_mode = S_IFDIR; CASE("open-directory-nofollow-trunc-dir", tested_openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY | O_TRUNC | O_NOFOLLOW) == -1 && errno == EISDIR && fstat_calls == 1 && closed(7));
 reset(); r_open = -ENOTDIR; CASE("open-directory-trunc-gate", tested_openat(AT_FDCWD, "f", O_RDWR | O_DIRECTORY | O_TRUNC) == -1 && errno == ENOTDIR && !(args[0][3] & (long)POUCH_OTRUNC) && count(SYS_close) == 0);
 reset(); CASE("open-trunc-still-sent", tested_openat(AT_FDCWD, "f", O_WRONLY | O_TRUNC) == 7 && (args[0][3] & (long)POUCH_OTRUNC) && fstat_calls == 0);
 /* The check precedes close-on-exec: a refused descriptor is never flagged. */
 reset(); fstat_mode = S_IFREG; CASE("open-directory-before-cloexec", tested_openat(AT_FDCWD, "f", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) == -1 && errno == ENOTDIR && count(SYS_thyla_handle_flags_set) == 0);
 reset(); fstat_mode = S_IFDIR; CASE("open-directory-cloexec", tested_openat(AT_FDCWD, "d", O_RDONLY | O_DIRECTORY | O_CLOEXEC) == 7 && count(SYS_thyla_handle_flags_set) == 1);
 /* The create arms carry O_NOFOLLOW too: an existing final link is ELOOP. */
 reset(); r_create = 7; CASE("open-creat-nofollow", tested_openat(AT_FDCWD, "f", O_WRONLY | O_CREAT | O_NOFOLLOW, 0644) == 7 && nums[0] == SYS_thyla_walk_create && args[0][3] == (long)(POUCH_OWRITE | POUCH_ONOFOLLOW));
 reset(); r_create = -EEXIST; r_open = -ELOOP; CASE("open-creat-nofollow-link", tested_openat(AT_FDCWD, "l", O_WRONLY | O_CREAT | O_NOFOLLOW, 0644) == -1 && errno == ELOOP && (i = at(SYS_thyla_open)) >= 0 && (args[i][3] & POUCH_ONOFOLLOW));

 /* fstatat: AT_SYMLINK_NOFOLLOW is SYS_LSTAT, which reports the link itself. */
 reset(); t_mode = S_IFLNK | 0777; t_size = 3; CASE("lstat-nofollow", __fstatat(AT_FDCWD, "/l", &st, AT_SYMLINK_NOFOLLOW) == 0 && calls == 1 && nums[0] == SYS_thyla_lstat && S_ISLNK(st.st_mode) && st.st_size == 3 && strcmp(paths[0], "/l") == 0);
 reset(); t_mode = S_IFREG | 0644; CASE("stat-follows", __fstatat(AT_FDCWD, "/l", &st, 0) == 0 && nums[0] == SYS_thyla_stat && S_ISREG(st.st_mode));
 reset(); r_stat = -ENOENT; CASE("lstat-errno", __fstatat(AT_FDCWD, "/nope", &st, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT);
 reset(); CASE("fstatat-bad-flag", __fstatat(AT_FDCWD, "/l", &st, AT_BOGUS) == -1 && errno == EINVAL && calls == 0);
 reset(); CASE("fstatat-automount", __fstatat(AT_FDCWD, "/l", &st, AT_NO_AUTOMOUNT) == 0 && nums[0] == SYS_thyla_stat);
 reset(); CASE("fstatat-statx-sync", __fstatat(AT_FDCWD, "/l", &st, AT_STATX_DONT_SYNC | AT_SYMLINK_NOFOLLOW) == 0 && nums[0] == SYS_thyla_lstat);
 reset(); CASE("fstatat-too-long", __fstatat(AT_FDCWD, over, &st, 0) == -1 && errno == ENAMETOOLONG && calls == 0);
 reset(); fstat_mode = S_IFDIR; CASE("fstatat-empty-path", __fstatat(5, "", &st, AT_EMPTY_PATH) == 0 && fstat_fd == 5 && calls == 0);

 /* fchmodat: AT_SYMLINK_NOFOLLOW never changes a link's target. */
 reset(); fstat_mode = S_IFLNK | 0777; CASE("lchmod-link", tested_fchmodat(AT_FDCWD, "/l", 0644, AT_SYMLINK_NOFOLLOW) == -1 && errno == EOPNOTSUPP && args[0][3] == (long)(POUCH_OPATH | POUCH_ONOFOLLOW) && fstat_fd == 7 && count(SYS_thyla_wstat) == 0 && closed(7));
 reset(); fstat_mode = S_IFREG | 0600; CASE("lchmod-file", tested_fchmodat(AT_FDCWD, "/f", 0644, AT_SYMLINK_NOFOLLOW) == 0 && (i = at(SYS_thyla_wstat)) >= 0 && args[i][0] == 7 && args[i][1] == POUCH_WSTAT_MODE && args[i][2] == 0644 && closed(7));
 reset(); CASE("chmod-follows", tested_fchmodat(AT_FDCWD, "/l", 0644, 0) == 0 && args[0][3] == (long)POUCH_OPATH && fstat_calls == 0 && count(SYS_thyla_wstat) == 1 && closed(7));
 reset(); CASE("fchmodat-bad-flag", tested_fchmodat(AT_FDCWD, "/l", 0644, AT_BOGUS) == -1 && errno == EINVAL && calls == 0);
 reset(); fstat_errno = EIO; CASE("lchmod-fstat-fails", tested_fchmodat(AT_FDCWD, "/l", 0644, AT_SYMLINK_NOFOLLOW) == -1 && errno == EIO && count(SYS_thyla_wstat) == 0 && closed(7));
 reset(); r_open = -ENOENT; CASE("lchmod-absent", tested_fchmodat(AT_FDCWD, "/nope", 0644, AT_SYMLINK_NOFOLLOW) == -1 && errno == ENOENT && fstat_calls == 0 && count(SYS_close) == 0);

 /* readlinkat: the general arm is SYS_READLINK, its answer verbatim. */
 reset(); rl_target = "a/b"; memset(buf, 'X', sizeof buf); CASE("readlink-native", tested_readlinkat(AT_FDCWD, "/l", buf, sizeof buf) == 3 && memcmp(buf, "a/bX", 4) == 0 && calls == 1 && nums[0] == SYS_thyla_readlink && args[0][1] == 2 && args[0][3] == (long)sizeof buf && strcmp(paths[0], "/l") == 0);
 reset(); rl_target = "abcdef"; CASE("readlink-bufsize-passed", tested_readlinkat(AT_FDCWD, "/l", buf, 3) == 3 && args[0][3] == 3);
 reset(); rl_target = "a/b"; CASE("readlink-bufsize-zero", tested_readlinkat(AT_FDCWD, "/l", buf, 0) == 0 && args[0][3] == 1 && (char *)args[0][2] != buf);
 reset(); r_readlink = -EINVAL; CASE("readlink-bufsize-zero-not-link", tested_readlinkat(AT_FDCWD, "/f", buf, 0) == -1 && errno == EINVAL);
 reset(); r_readlink = -EINVAL; CASE("readlink-not-link", tested_readlinkat(AT_FDCWD, "/f", buf, sizeof buf) == -1 && errno == EINVAL);
 reset(); r_readlink = -ENOENT; CASE("readlink-absent", tested_readlinkat(AT_FDCWD, "/nope", buf, sizeof buf) == -1 && errno == ENOENT);
 reset(); CASE("readlink-too-long", tested_readlinkat(AT_FDCWD, over, buf, sizeof buf) == -1 && errno == ENAMETOOLONG && calls == 0);
 reset(); CASE("readlink-dirfd", tested_readlinkat(3, "l", buf, sizeof buf) == -1 && errno == ENOTSUP && calls == 0);
 /* The /proc whitelist still answers first, from the file's contents. */
 reset(); CASE("readlink-proc-first", tested_readlinkat(AT_FDCWD, "/proc/self/exe", buf, sizeof buf) == 10 && memcmp(buf, "/bin/hello", 10) == 0 && count(SYS_thyla_readlink) == 0 && (i = at(SYS_thyla_open)) >= 0 && strcmp(paths[i], "/proc/42/exe") == 0);
 printf("tests=%d failures=%d\n", checks, failures); return !!failures;
}
