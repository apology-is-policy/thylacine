#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define hidden
#include "fs-under-test.h"
/* musl aarch64 bits/signal.h; the host has no _NSIG. */
#ifndef _NSIG
#define _NSIG 65
#endif
#include "signal-under-test.h"
static long nums[4], x0s[4], x1s[4], x2s[4], ret;
static int calls, failures, checks;
static long sys(long n, long a, long b, long c) {
 if (calls < 4) { nums[calls] = n; x0s[calls] = a; x1s[calls] = b; x2s[calls] = c; }
 calls++;
 return ret;
}
/* musl's own argument-count dispatch: every argument is widened to long. */
#define __scc(X) ((long)(X))
#define NARGS_X(a,b,c,d,e,f,g,h,n,...) n
#define NARGS(...) NARGS_X(__VA_ARGS__,7,6,5,4,3,2,1,0,)
#define CAT_X(a,b) a##b
#define CAT(a,b) CAT_X(a,b)
#define __syscall(...) CAT(sc,NARGS(__VA_ARGS__))(__VA_ARGS__)
#define sc0(n) sys(n,0,0,0)
#define sc3(n,a,b,c) sys(n,__scc(a),__scc(b),__scc(c))
/* The series' own decoder (src/internal/syscall_ret.c): a -1 reads EIO, as on the device. */
#include "syscall-ret-under-test.c"
#define syscall(...) __syscall_ret(__syscall(__VA_ARGS__))
int __pouch_sig_to_note(int sig, const char **out) {
 if (sig == SIGTERM) { *out = "term"; return 0; }
 if (sig == SIGINT) { *out = "interrupt"; return 0; }
 return -1;
}
const char *__pouch_self_snare(int sig) { return sig == SIGSEGV ? "snare:segv" : 0; }
static int aborts;
static void mock_abort(void) { aborts++; }
static pid_t mock_getpid(void) { return 42; }
#define abort mock_abort
#define getpid mock_getpid
#define kill tested_kill
#define getppid tested_getppid
#include "kill-under-test.c"
#include "getppid-under-test.c"
#undef kill
#undef getppid
#define CASE(name, expr) do { ++checks; int ok = (expr); printf("%s %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; } while (0)
#define RESET(r) do { ret = (r); calls = aborts = 0; errno = EDOM; } while (0)
int main(void) {
 /* Pouch's number for the post is the kernel's; the derived values come from the driver. */
 CASE("postnote-number", SYS_postnote == KERNEL_SYS_POSTNOTE);
 /* kill(pid, 0) is the kernel's probe: an empty name, x1 and x2 both 0. */
 RESET(0); CASE("probe-self", tested_kill(42, 0) == 0 && calls == 1 && nums[0] == KERNEL_SYS_POSTNOTE && x0s[0] == 42 && x1s[0] == 0 && x2s[0] == 0);
 RESET(0); CASE("probe-sentinel", tested_kill(0, 0) == 0 && calls == 1 && x0s[0] == 0 && x2s[0] == 0);
 RESET(0); CASE("probe-large-pid", tested_kill(INT_MAX, 0) == 0 && x0s[0] == INT_MAX && x2s[0] == 0);
 RESET(-EACCES); CASE("probe-refused-eperm", tested_kill(7, 0) == -1 && errno == EPERM && calls == 1);
 RESET(-ESRCH); CASE("probe-missing-esrch", tested_kill(7, 0) == -1 && errno == ESRCH && calls == 1);
 /* A negative pid is a group, or every process: refused, never posted. */
 RESET(0); CASE("probe-group-refused", tested_kill(-5, 0) == -1 && errno == EPERM && calls == 0);
 RESET(0); CASE("probe-all-refused", tested_kill(-1, 0) == -1 && errno == EPERM && calls == 0);
 RESET(0); CASE("probe-int-min-refused", tested_kill(INT_MIN, 0) == -1 && errno == EPERM && calls == 0);
 /* A real signal: the note's name and length, and the same group refusal. */
 RESET(0); CASE("signal-post", tested_kill(7, SIGTERM) == 0 && calls == 1 && x0s[0] == 7 && x1s[0] != 0 && x2s[0] == 4 && !strcmp((const char *)x1s[0], "term"));
 RESET(-EACCES); CASE("signal-refused-eperm", tested_kill(7, SIGTERM) == -1 && errno == EPERM);
 RESET(0); CASE("signal-group-refused", tested_kill(-5, SIGTERM) == -1 && errno == EPERM && calls == 0);
 RESET(0); CASE("signal-unknown-before-group", tested_kill(-5, 1000) == -1 && errno == EINVAL && calls == 0);
 /* getppid: the kernel's number, its value returned as the pid, never an errno. */
 RESET(17); CASE("getppid", tested_getppid() == 17 && calls == 1 && nums[0] == KERNEL_SYS_GETPPID);
 RESET(0); CASE("getppid-zero", tested_getppid() == 0 && errno == EDOM);
 printf("tests=%d failures=%d aborts=%d\n", checks, failures, aborts); return !!failures || aborts;
}
