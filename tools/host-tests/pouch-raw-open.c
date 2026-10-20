#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
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
/* aarch64's value; upstream's raw opens add it. */
#ifndef O_LARGEFILE
#define O_LARGEFILE 0400000
#endif
#define SYS_openat 0xFFFF
#define hidden
#define weak_alias(old, new)
#define POUCH_SOCK_TAG 0x40000000
#include "fs-under-test.h"
#define NCALL 16
static long nums[NCALL], args[NCALL][6];
static char paths[NCALL][POUCH_PATH_MAX + 8];
static int calls, failures, checks, parent_calls, fstat_calls, fstat_errno, fstat_fd;
static long r_open, r_create, r_flags, parent_ret;
static unsigned fstat_mode;
static void *pouch_sock_resolve(int fd) { (void)fd; return 0; }
static void note_path(int i, long ptr, long len) {
 if (i < NCALL && len >= 0 && len < (long)sizeof paths[0]) { memcpy(paths[i], (const void *)ptr, (size_t)len); paths[i][len] = 0; }
}
/* The kernel's side of each call the code under test makes. A path is read only
 * from a real start descriptor: upstream's mis-shaped open passes a pointer there. */
static long sys(long n, long a, long b, long c, long d, long e, long f) {
 int i = calls++;
 if (i < NCALL) { nums[i] = n; args[i][0] = a; args[i][1] = b; args[i][2] = c; args[i][3] = d; args[i][4] = e; args[i][5] = f; }
 if (n == SYS_thyla_open) { if (a == POUCH_FROM_ROOT || (a >= 0 && a < 64)) note_path(i, b, c); return r_open; }
 if (n == SYS_thyla_walk_create) { note_path(i, b, c); return r_create; }
 if (n == SYS_thyla_handle_flags_set) return r_flags;
 return 0;
}
/* musl's own argument-count dispatch: every argument is widened to long. */
#define __scc(X) ((long)(X))
#define NARGS_X(a,b,c,d,e,f,g,h,n,...) n
#define NARGS(...) NARGS_X(__VA_ARGS__,7,6,5,4,3,2,1,0,)
#define CAT_X(a,b) a##b
#define CAT(a,b) CAT_X(a,b)
#define __syscall(...) CAT(sc,NARGS(__VA_ARGS__))(__VA_ARGS__)
#define sc1(n,a) sys(n,__scc(a),0,0,0,0,0)
#define sc2(n,a,b) sys(n,__scc(a),__scc(b),0,0,0,0)
#define sc3(n,a,b,c) sys(n,__scc(a),__scc(b),__scc(c),0,0,0)
#define sc4(n,a,b,c,d) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),0,0)
#define sc5(n,a,b,c,d,e) sys(n,__scc(a),__scc(b),__scc(c),__scc(d),__scc(e),0)
#define __syscall2(n,a,b) sc2(n,a,b)
#define __syscall3(n,a,b,c) sc3(n,a,b,c)
#define __syscall4(n,a,b,c,d) sc4(n,a,b,c,d)
#define __syscall_cp2(n,a,b) sc2(n,a,b)
#define __syscall_cp3(n,a,b,c) sc3(n,a,b,c)
#define __syscall_cp4(n,a,b,c,d) sc4(n,a,b,c,d)
static long __syscall_ret(unsigned long r) { if (r > -4096UL) { errno = -(long)r; return -1; } return (long)r; }
int __pouch_open_parent(int atfd, const char *path, const char **leaf, size_t *llen, int *dirreq) {
 size_t n = strlen(path);
 (void)atfd; parent_calls++; *dirreq = n && path[n - 1] == '/'; *leaf = path; *llen = n - (size_t)*dirreq; return (int)parent_ret;
}
/* The host's mode_t is promotable; the guest's is not. */
#define mode_t unsigned
/* musl's aarch64 struct stat field names. */
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
/* libc's own open asks the hidden __fstat: a call to the public name, which a
 * program may define, is counted. */
static int public_fstat_calls;
static int public_fstat(int fd, struct pouch_stat *st) { public_fstat_calls++; return mock_fstat(fd, st); }
#define __fstat mock_fstat
#define fstat public_fstat
/* The macros under test, and the helper they name, mocked: what each raw open asks. */
#include "rawopen-under-test.h"
static int so_calls, so_flags;
static unsigned so_mode;
static long so_ret;
static char so_path[64];
long __pouch_sys_open(const char *path, int flags, unsigned mode) {
 so_calls++; so_flags = flags; so_mode = mode; snprintf(so_path, sizeof so_path, "%s", path); return so_ret;
}
#include "descriptor-under-test.h"
/* The helper itself, under its own name, beside the mock. */
#define __pouch_sys_open tested_pouch_sys_open
#define openat tested_openat
#include "openat-under-test.c"
#undef openat
#undef __pouch_sys_open
#undef __fstat
/* __map_file's own calls. */
static long map_size_out;
static int mm_calls, mm_prot, mm_flags, mm_fd, fst_calls, fst_fail;
static size_t mm_len;
static off_t mm_off;
static void *mm_ret;
static char mapped[1];
static int __fstat(int fd, struct pouch_stat *st) {
 (void)fd; fst_calls++; if (fst_fail) { errno = EIO; return -1; }
 memset(st, 0, sizeof *st); st->st_size = map_size_out; return 0;
}
static void *__mmap(void *start, size_t len, int prot, int flags, int fd, off_t off) {
 (void)start; mm_calls++; mm_len = len; mm_prot = prot; mm_flags = flags; mm_fd = fd; mm_off = off; return mm_ret;
}
#include "map-file-under-test.c"
/* fopen's own calls. */
static int fd_calls, fd_fd;
static FILE *fd_ret;
static int __fmodeflags(const char *mode) { return *mode == 'r' ? O_RDONLY : O_WRONLY | O_CREAT | O_TRUNC; }
static FILE *__fdopen(int fd, const char *mode) { (void)mode; fd_calls++; fd_fd = fd; return fd_ret; }
#define fopen tested_fopen
#include "fopen-under-test.c"
#undef fopen
static int count(long n) { int k = 0; for (int i = 0; i < calls && i < NCALL; i++) k += nums[i] == n; return k; }
static int at(long n) { for (int i = 0; i < calls && i < NCALL; i++) if (nums[i] == n) return i; return -1; }
static int closed(long fd) { int i = at(SYS_close); return count(SYS_close) == 1 && args[i][0] == fd; }
#define CASE(name, expr) do { ++checks; int ok = (expr); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
static void reset(void) {
 calls = parent_calls = fstat_calls = fstat_errno = so_calls = so_flags = public_fstat_calls = 0; so_mode = 0; fstat_fd = -1; fstat_mode = 0;
 r_open = 7; r_create = r_flags = 0; parent_ret = 3; so_ret = 9; memset(paths, 0, sizeof paths); so_path[0] = 0;
 mm_calls = fst_calls = fst_fail = fd_calls = 0; mm_ret = mapped; map_size_out = 166; fd_ret = stdin; fd_fd = -1;
 errno = EDOM;
}
int main(void) {
 static char over[POUCH_PATH_MAX + 2];
 size_t size = 0;
 int i;
 memset(over, 'a', POUCH_PATH_MAX + 1);

 /* Every raw open is the helper, with the caller's flags and mode, and no syscall of its own. */
 reset(); CASE("route-open2", __sys_open("/p", O_RDWR) == 9 && so_calls == 1 && strcmp(so_path, "/p") == 0 && so_flags == O_RDWR && so_mode == 0 && calls == 0 && errno == EDOM);
 reset(); CASE("route-open3", __sys_open("/p", O_WRONLY | O_CREAT, 0640) == 9 && so_calls == 1 && so_flags == (O_WRONLY | O_CREAT) && so_mode == 0640 && calls == 0);
 reset(); CASE("route-open-cp2", __sys_open_cp("/p", O_RDONLY) == 9 && so_calls == 1 && so_flags == O_RDONLY && calls == 0);
 reset(); CASE("route-open-cp3", __sys_open_cp("/p", O_RDWR | O_CREAT | O_EXCL, 0600) == 9 && so_calls == 1 && so_mode == 0600 && calls == 0);
 /* The raw forms answer -errno and leave errno; the sys_ forms set it. */
 reset(); so_ret = -ENOENT; CASE("route-raw-errno", __sys_open("/p", O_RDONLY) == -ENOENT && errno == EDOM);
 reset(); so_ret = -ENOENT; CASE("route-sys-open-errno", sys_open("/p", O_RDONLY) == -1 && errno == ENOENT && so_calls == 1);
 reset(); CASE("route-sys-open-ok", sys_open("/p", O_RDONLY | O_CLOEXEC | O_NONBLOCK) == 9 && so_flags == (O_RDONLY | O_CLOEXEC | O_NONBLOCK) && errno == EDOM);
 reset(); so_ret = -EACCES; CASE("route-sys-open-cp-errno", sys_open_cp("/p", O_WRONLY) == -1 && errno == EACCES && so_calls == 1 && calls == 0);

 /* The helper: a descriptor or -errno, and errno as it found it, on every path. */
 reset(); CASE("raw-ok", tested_pouch_sys_open("/f", O_RDONLY, 0) == 7 && errno == EDOM && calls == 1 && nums[0] == SYS_thyla_open && args[0][0] == POUCH_FROM_ROOT && strcmp(paths[0], "/f") == 0 && args[0][3] == (long)POUCH_OREAD);
 reset(); r_open = -ENOENT; CASE("raw-kernel-errno", tested_pouch_sys_open("/nope", O_RDONLY, 0) == -ENOENT && errno == EDOM);
 reset(); CASE("raw-null", tested_pouch_sys_open(NULL, O_RDONLY, 0) == -EFAULT && errno == EDOM && calls == 0);
 reset(); CASE("raw-accmode", tested_pouch_sys_open("/f", O_ACCMODE, 0) == -EINVAL && errno == EDOM && calls == 0);
 reset(); CASE("raw-creat-directory", tested_pouch_sys_open("/d", O_RDWR | O_CREAT | O_DIRECTORY, 0644) == -EINVAL && errno == EDOM && calls == 0 && parent_calls == 0);
 reset(); CASE("raw-tmpfile", tested_pouch_sys_open("/d", O_RDWR | O_TMPFILE, 0600) == -ENOTSUP && errno == EDOM && calls == 0);
 reset(); CASE("raw-empty", tested_pouch_sys_open("", O_RDONLY, 0) == -ENOENT && errno == EDOM && calls == 0);
 reset(); CASE("raw-too-long", tested_pouch_sys_open(over, O_RDONLY, 0) == -ENAMETOOLONG && errno == EDOM && calls == 0);
 reset(); parent_ret = -ENOENT; CASE("raw-parent-errno", tested_pouch_sys_open("/no/f", O_WRONLY | O_CREAT, 0600) == -ENOENT && errno == EDOM && calls == 0);
 reset(); CASE("raw-dirreq", tested_pouch_sys_open("/d/", O_WRONLY | O_CREAT, 0600) == -EISDIR && errno == EDOM && closed(3) && count(SYS_thyla_walk_create) == 0);
 reset(); r_create = -EACCES; CASE("raw-create-errno", tested_pouch_sys_open("/f", O_WRONLY | O_CREAT | O_EXCL, 0600) == -EACCES && errno == EDOM && closed(3));
 reset(); r_create = 8; CASE("raw-create-ok", tested_pouch_sys_open("/f", O_WRONLY | O_CREAT | O_EXCL, 0640) == 8 && errno == EDOM && (i = at(SYS_thyla_walk_create)) >= 0 && args[i][4] == 0640 && closed(3));
 reset(); fstat_errno = EACCES; CASE("raw-fstat-errno-kept", tested_pouch_sys_open("/d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0) == -EACCES && errno == EDOM && fstat_fd == 7 && closed(7));
 reset(); fstat_mode = S_IFREG; CASE("raw-notdir", tested_pouch_sys_open("/f", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0) == -ENOTDIR && errno == EDOM && closed(7));
 /* The tripwire counts a public fstat (the control), and the helper never makes one. */
 reset(); { struct stat t; CASE("public-fstat-tripwire", fstat(5, &t) == 0 && public_fstat_calls == 1 && fstat_calls == 1); }
 reset(); fstat_mode = S_IFDIR; CASE("raw-odir-hidden-fstat", tested_pouch_sys_open("/d", O_RDONLY | O_DIRECTORY | O_NOFOLLOW, 0) == 7 && errno == EDOM && fstat_calls == 1 && public_fstat_calls == 0 && count(SYS_close) == 0);
 reset(); r_flags = -EIO; CASE("raw-cloexec-errno", tested_pouch_sys_open("/f", O_RDONLY | O_CLOEXEC, 0) == -EIO && errno == EDOM && closed(7));
 reset(); CASE("raw-cloexec-ok", tested_pouch_sys_open("/f", O_RDONLY | O_CLOEXEC, 0) == 7 && errno == EDOM && count(SYS_thyla_handle_flags_set) == 1 && count(SYS_close) == 0);
 /* openat() is the helper behind errno. */
 reset(); r_open = -ENOENT; CASE("wrapper-errno", tested_openat(AT_FDCWD, "/nope", O_RDONLY) == -1 && errno == ENOENT);
 reset(); CASE("wrapper-ok", tested_openat(AT_FDCWD, "/f", O_RDONLY) == 7 && errno == EDOM);
 reset(); CASE("wrapper-dirfd", tested_openat(3, "f", O_RDONLY) == -1 && errno == ENOTSUP && calls == 0);
 reset(); r_create = 8; CASE("wrapper-mode", tested_openat(AT_FDCWD, "/f", O_WRONLY | O_CREAT, 0640) == 8 && (i = at(SYS_thyla_walk_create)) >= 0 && args[i][4] == 0640);

 /* __map_file: one raw open, then a private read-only map of the whole file; the descriptor is closed either way. */
 reset(); CASE("map-ok", __map_file("/zone", &size) == (const unsigned char *)mapped && size == 166 && so_calls == 1 && strcmp(so_path, "/zone") == 0 && so_flags == (O_RDONLY | O_CLOEXEC | O_NONBLOCK) && mm_calls == 1 && mm_len == 166 && mm_prot == PROT_READ && mm_flags == MAP_PRIVATE && mm_fd == 9 && mm_off == 0 && closed(9));
 reset(); so_ret = -ENOENT; CASE("map-open-fails", __map_file("/nope", &size) == 0 && errno == ENOENT && mm_calls == 0 && fst_calls == 0 && count(SYS_close) == 0);
 reset(); fst_fail = 1; CASE("map-fstat-fails", __map_file("/zone", &size) == 0 && mm_calls == 0 && closed(9));
 reset(); mm_ret = MAP_FAILED; CASE("map-mmap-fails", __map_file("/zone", &size) == 0 && mm_calls == 1 && closed(9));

 /* fopen: the raw open with the stream's flags and 0666, then the stream. */
 reset(); CASE("fopen-routes", tested_fopen("/nonexistent-pouch-raw-open", "r") == stdin && so_calls == 1 && strcmp(so_path, "/nonexistent-pouch-raw-open") == 0 && so_flags == O_RDONLY && so_mode == 0666 && fd_calls == 1 && fd_fd == 9 && count(SYS_close) == 0);
 reset(); so_ret = -ENOENT; CASE("fopen-errno", tested_fopen("/nonexistent-pouch-raw-open", "r") == NULL && errno == ENOENT && fd_calls == 0 && count(SYS_close) == 0);
 reset(); fd_ret = NULL; CASE("fopen-fdopen-fails", tested_fopen("/nonexistent-pouch-raw-open", "w") == NULL && so_flags == (O_WRONLY | O_CREAT | O_TRUNC) && closed(9));
 reset(); CASE("fopen-bad-mode", tested_fopen("/nonexistent-pouch-raw-open", "x") == NULL && errno == EINVAL && so_calls == 0);
 printf("tests=%d failures=%d\n", checks, failures); return !!failures;
}
