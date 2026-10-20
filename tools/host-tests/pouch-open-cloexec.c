#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef O_PATH
#define O_PATH 010000000
#endif
#ifndef O_TMPFILE
#define O_TMPFILE (020000000 | O_DIRECTORY)
#endif
#define hidden
#define POUCH_SOCK_TAG 0x40000000
#define SYS_close 57
#define SYS_lseek 62
#define SYS_thyla_handle_flags_set 130
#include "fs-under-test.h"
static long nums[8], x0s[8], x1s[8];
static int calls, failures, checks, parent_calls, fstat_calls;
static long r_open, r_create, r_set;
static void *pouch_sock_resolve(int fd) { (void)fd; return 0; }
static long sys(long n, long a, long b) {
 if (calls < 8) { nums[calls] = n; x0s[calls] = a; x1s[calls] = b; }
 calls++;
 if (n == SYS_thyla_open) return r_open;
 if (n == SYS_thyla_walk_create) return r_create;
 if (n == SYS_thyla_handle_flags_set) return r_set;
 return 0;
}
/* musl's own argument-count dispatch: every argument is widened to long. */
#define __scc(X) ((long)(X))
#define NARGS_X(a,b,c,d,e,f,g,h,n,...) n
#define NARGS(...) NARGS_X(__VA_ARGS__,7,6,5,4,3,2,1,0,)
#define CAT_X(a,b) a##b
#define CAT(a,b) CAT_X(a,b)
#define __syscall(...) CAT(sc,NARGS(__VA_ARGS__))(__VA_ARGS__)
#define sc1(n,a) sys(n,__scc(a),0)
#define sc2(n,a,b) sys(n,__scc(a),__scc(b))
#define sc3(n,a,b,c) ((void)__scc(c),sys(n,__scc(a),__scc(b)))
#define sc4(n,a,b,c,d) ((void)__scc(c),(void)__scc(d),sys(n,__scc(a),__scc(b)))
#define sc5(n,a,b,c,d,e) ((void)__scc(c),(void)__scc(d),(void)__scc(e),sys(n,__scc(a),__scc(b)))
#define __syscall2(n,a,b) sys(n,__scc(a),__scc(b))
static long __syscall_ret(unsigned long r) { if (r > -4096UL) { errno = -(long)r; return -1; } return (long)r; }
int __pouch_open_parent(int atfd, const char *path, const char **leaf, size_t *llen, int *dirreq) {
 (void)atfd; parent_calls++; *leaf = path; *llen = strlen(path); *dirreq = 0; return 3;
}
/* The host's mode_t is promotable; the guest's is not. */
#define mode_t unsigned
/* Only an O_DIRECTORY open asks what it opened; no case here passes one. */
static int mock_fstat(int fd, struct stat *st) { (void)fd; fstat_calls++; memset(st, 0, sizeof *st); return 0; }
#define fstat mock_fstat
#define openat tested_openat
#include "descriptor-under-test.h"
#include "openat-under-test.c"
#undef openat
static int at(long n) { for (int i = 0; i < calls && i < 8; i++) if (nums[i] == n) return i; return -1; }
static int count(long n) { int k = 0; for (int i = 0; i < calls && i < 8; i++) k += nums[i] == n; return k; }
#define CASE(name, expr) do { ++checks; int ok = (expr); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
#define RESET(o, c, s) do { r_open = (o); r_create = (c); r_set = (s); calls = parent_calls = 0; errno = EDOM; } while (0)
int main(void) {
 int set, seek;
 /* Every successful exit flags the descriptor it returns, and only that one. */
 RESET(7, 0, 0); CASE("plain-cloexec", tested_openat(AT_FDCWD, "f", O_RDONLY | O_CLOEXEC) == 7 && calls == 2 && nums[0] == SYS_thyla_open && (set = at(130)) == 1 && x0s[set] == 7 && x1s[set] == 1);
 RESET(7, 0, 0); CASE("plain-no-cloexec", tested_openat(AT_FDCWD, "f", O_RDONLY) == 7 && calls == 1 && at(130) < 0);
 RESET(0, 7, 0); CASE("create-cloexec", tested_openat(AT_FDCWD, "f", O_WRONLY | O_CREAT | O_CLOEXEC, 0644) == 7 && nums[0] == SYS_thyla_walk_create && x0s[0] == 3 && (set = at(130)) >= 0 && x0s[set] == 7 && count(130) == 1);
 RESET(8, -EEXIST, 0); CASE("eexist-open-cloexec", tested_openat(AT_FDCWD, "f", O_WRONLY | O_CREAT | O_CLOEXEC, 0644) == 8 && nums[0] == SYS_thyla_walk_create && nums[1] == SYS_thyla_open && x0s[1] == 3 && (set = at(130)) >= 0 && x0s[set] == 8 && count(130) == 1);
 RESET(8, -EEXIST, 0); CASE("excl-eexist-no-set", tested_openat(AT_FDCWD, "f", O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644) == -1 && errno == EEXIST && at(130) < 0);
 /* A set answering EBADF: a peer closed the descriptor, so it is not closed again. */
 RESET(7, 0, -EBADF); CASE("set-gone-not-closed", tested_openat(AT_FDCWD, "f", O_RDONLY | O_CLOEXEC) == -1 && errno == EBADF && count(SYS_close) == 0);
 RESET(8, -EEXIST, -EBADF); CASE("eexist-set-gone", tested_openat(AT_FDCWD, "f", O_RDWR | O_CREAT | O_CLOEXEC, 0644) == -1 && errno == EBADF && count(SYS_close) == 1 && x0s[at(SYS_close)] == 3);
 /* Any other failed set closes the descriptor and fails the open with its errno. */
 RESET(7, 0, -1); CASE("set-old-kernel-closes", tested_openat(AT_FDCWD, "f", O_RDONLY | O_CLOEXEC) == -1 && errno == ENOSYS && count(SYS_close) == 1 && x0s[at(SYS_close)] == 7);
 /* The append seek follows the set, and a failed set leaves nothing to seek. */
 RESET(7, 0, 0); CASE("append-set-before-seek", tested_openat(AT_FDCWD, "f", O_WRONLY | O_APPEND | O_CLOEXEC) == 7 && (set = at(130)) >= 0 && (seek = at(SYS_lseek)) > set && x0s[seek] == 7);
 RESET(7, 0, -1); CASE("append-failed-set-no-seek", tested_openat(AT_FDCWD, "f", O_WRONLY | O_APPEND | O_CLOEXEC) == -1 && at(SYS_lseek) < 0);
 RESET(7, 0, 0); CASE("append-no-cloexec-seeks", tested_openat(AT_FDCWD, "f", O_WRONLY | O_APPEND) == 7 && at(130) < 0 && at(SYS_lseek) == 1);
 CASE("no-fstat-without-o-directory", fstat_calls == 0);
 printf("tests=%d failures=%d\n", checks, failures); return !!failures;
}
