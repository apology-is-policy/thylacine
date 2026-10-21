#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#define POUCH_SOCK_TAG 0x40000000
#define SYS_thyla_handle_flags_get 129
#define SYS_thyla_handle_flags_set 130
#define SYS_thyla_handle_dup_to 131
#define SYS_thyla_set_nonblock 123
/* Linux status bits the host libc may lack; the adapter only tests them. */
#ifndef O_DIRECT
#define O_DIRECT 0200000
#endif
#ifndef O_NOATIME
#define O_NOATIME 01000000
#endif
static long results[4];
static int nresults, calls, live_socket=1, failures, checks;
static long number, a0, a1, a2, numbers[4], args0[4], args1[4];
static void *pouch_sock_resolve(int fd) { return fd==POUCH_SOCK_TAG && live_socket ? &live_socket : 0; }
static long mock(long n,long a,long b,long c) {
 if (calls<4) { numbers[calls]=n; args0[calls]=a; args1[calls]=b; }
 calls++;number=n;a0=a;a1=b;a2=c;
 return results[calls-1<nresults ? calls-1 : nresults-1];
}
/* Every recorded call's x0 is `fd`: no adapter may aim its call elsewhere. */
static int all_x0(long fd) { for (int i=0;i<calls && i<4;i++) if (args0[i]!=fd) return 0; return calls>0; }
static int pipe_calls, pipe_fail, close_calls, closed[2], cancellable_closes;
static int __pouch_pipe(int fd[2]) { pipe_calls++; if (pipe_fail) { errno=EMFILE; return -1; } fd[0]=5; fd[1]=6; return 0; }
/* The raw close is the only variadic __syscall the adapters make. */
#define SYS_close 57
static long mock_raw_close(long n,long fd) { assert(n==SYS_close); if (close_calls<2) closed[close_calls]=(int)fd; close_calls++; return 0; }
/* close() is a cancellation point; an unwind must not reach it. */
__attribute__((unused)) static int cancellable_close(int fd) { cancellable_closes++; return (int)mock_raw_close(SYS_close,fd); }
#define __syscall(n,a) mock_raw_close(n,a)
#define __syscall1(n,a) mock(n,a,0,0)
#define __syscall2(n,a,b) mock(n,a,b,0)
#define __syscall3(n,a,b,c) mock(n,a,b,c)
#define weak_alias(a,b)
#define dup2 tested_dup2
#define __dup3 tested_dup3
#define fcntl tested_fcntl
#define dup tested_dup
#define pipe2 tested_pipe2
#include "descriptor-under-test.h"
#include "fcntl-under-test.c"
#include "dup-under-test.c"
#include "dup2-under-test.c"
#include "dup3-under-test.c"
#define close cancellable_close
#include "pipe2-under-test.c"
#undef close
#undef pipe2
#undef fcntl
#undef dup
#undef dup2
#define CASE(name, expr) do { ++checks;int ok=(expr); printf("%s %s\n",ok?"PASS":"FAIL",name); failures+=!ok; } while(0)
#define RESET(ret) do { results[0]=(ret);nresults=1;calls=0;errno=EDOM;pipe_calls=pipe_fail=close_calls=cancellable_closes=0; } while(0)
#define RESET2(r1,r2) do { results[0]=(r1);results[1]=(r2);nresults=2;calls=0;errno=EDOM;pipe_calls=pipe_fail=close_calls=cancellable_closes=0; } while(0)
#define OM(m) ((long)(m)<<8)
int main(void) {
 RESET(1);CASE("get-no-vararg",tested_fcntl(8,F_GETFD)==FD_CLOEXEC && calls==1 && number==129 && a0==8 && all_x0(8));
 RESET(0);CASE("set-translates",tested_fcntl(8,F_SETFD,FD_CLOEXEC)==0 && calls==1 && number==130 && a1==1 && all_x0(8));
 RESET(0);CASE("clear",tested_fcntl(8,F_SETFD,0)==0 && a1==0 && all_x0(8));
 RESET(0);CASE("unknown-flags",tested_fcntl(8,F_SETFD,~FD_CLOEXEC)==-1 && errno==EINVAL && calls==0);
 RESET(0);CASE("unknown-command-no-vararg",tested_fcntl(8,-999)==-1 && errno==ENOSYS && calls==0);
 RESET(-1);CASE("set-old-kernel",tested_fcntl(8,F_SETFD,1)==-1 && errno==ENOSYS && all_x0(8));
 RESET(-EBADF);CASE("get-absent",tested_fcntl(9,F_GETFD)==-1 && errno==EBADF && all_x0(9));
 RESET(0);CASE("negative",tested_fcntl(-1,F_GETFD)==-1 && errno==EBADF && calls==0);
 RESET(9);CASE("dup2-target",tested_dup2(8,9)==9 && number==131 && a0==8 && a1==9 && a2==0 && all_x0(8));
 RESET(9);CASE("dup3-set",tested_dup3(8,9,O_CLOEXEC)==9 && a2==1 && all_x0(8));
 RESET(9);CASE("dup3-clear",tested_dup3(8,9,0)==9 && a2==0 && all_x0(8));
 RESET(9);CASE("dup3-self",tested_dup3(8,8,0)==-1 && errno==EINVAL && calls==0);
 RESET(9);CASE("dup3-invalid-flags",tested_dup3(8,9,~O_CLOEXEC)==-1 && errno==EINVAL && calls==0);
 RESET(-1);CASE("dup-old-kernel",tested_dup2(8,9)==-1 && errno==ENOSYS && all_x0(8));
 RESET(-EACCES);CASE("alias-policy",tested_dup3(8,9,0)==-1 && errno==EACCES && all_x0(8));
 RESET(1);CASE("self-preserves",tested_dup2(8,8)==8 && number==129 && calls==1 && all_x0(8));
 RESET(0);CASE("socket-get",tested_fcntl(POUCH_SOCK_TAG,F_GETFD)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-set",tested_fcntl(POUCH_SOCK_TAG,F_SETFD,1)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-source",tested_dup2(POUCH_SOCK_TAG,9)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-target",tested_dup2(8,POUCH_SOCK_TAG)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-self",tested_dup2(POUCH_SOCK_TAG,POUCH_SOCK_TAG)==POUCH_SOCK_TAG && calls==0);
 live_socket=0;
 RESET(0);CASE("closed-socket",tested_fcntl(POUCH_SOCK_TAG,F_GETFD)==-1 && errno==EBADF && calls==0);
 RESET(0);CASE("malformed-socket",tested_dup3(8,INT_MAX,0)==-1 && errno==EBADF && calls==0);
 RESET(-ENOSYS);CASE("explicit-enosys",tested_dup3(8,9,0)==-1 && errno==ENOSYS && all_x0(8));
 live_socket=1;
 /* dup: the lowest free slot is DUP_TO with the LOWEST flag and floor 0. */
 RESET(5);CASE("dup-lowest",tested_dup(8)==5 && calls==1 && number==131 && a0==8 && a1==0 && a2==2);
 RESET(5);CASE("dup-negative",tested_dup(-1)==-1 && errno==EBADF && calls==0);
 RESET(5);CASE("dup-socket",tested_dup(POUCH_SOCK_TAG)==-1 && errno==ENOSYS && calls==0);
 RESET(-1);CASE("dup-lowest-old-kernel",tested_dup(8)==-1 && errno==ENOSYS);
 RESET(-EMFILE);CASE("dup-full",tested_dup(8)==-1 && errno==EMFILE);
 /* F_DUPFD: the floor rides x1; a negative one is the kernel's to refuse. */
 RESET(12);CASE("dupfd-floor",tested_fcntl(8,F_DUPFD,10)==12 && calls==1 && number==131 && a1==10 && a2==2 && all_x0(8));
 RESET(12);CASE("dupfd-cloexec",tested_fcntl(8,F_DUPFD_CLOEXEC,10)==12 && a2==3 && all_x0(8));
 RESET(-EINVAL);CASE("dupfd-negative-floor",tested_fcntl(8,F_DUPFD,-1)==-1 && errno==EINVAL && calls==1 && a1==-1L && all_x0(8));
 RESET(-EBADF);CASE("dupfd-closed",tested_fcntl(8,F_DUPFD,-1)==-1 && errno==EBADF && all_x0(8));
 RESET(12);CASE("dupfd-socket",tested_fcntl(POUCH_SOCK_TAG,F_DUPFD,0)==-1 && errno==ENOSYS && calls==0);
 /* F_GETFL: the status word's fields, and nothing from the descriptor flag. */
 RESET(0);CASE("getfl-rdonly",tested_fcntl(8,F_GETFL)==O_RDONLY && calls==1 && number==129 && all_x0(8));
 RESET(OM(1)|0x400);CASE("getfl-wronly-nonblock",tested_fcntl(8,F_GETFL)==(O_WRONLY|O_NONBLOCK) && all_x0(8));
 RESET(OM(2)|0x800|1);CASE("getfl-rdwr-append-no-cloexec",tested_fcntl(8,F_GETFL)==(O_RDWR|O_APPEND) && all_x0(8));
 RESET(OM(3));CASE("getfl-oexec",tested_fcntl(8,F_GETFL)==O_RDONLY && all_x0(8));
 RESET(-EBADF);CASE("getfl-absent",tested_fcntl(8,F_GETFL)==-1 && errno==EBADF && all_x0(8));
 RESET(0);CASE("getfl-socket",tested_fcntl(POUCH_SOCK_TAG,F_GETFL)==-1 && errno==ENOSYS && calls==0);
 /* F_SETFL: read the status, refuse what cannot change, set nonblock. */
 RESET2(0,0);CASE("setfl-nonblock",tested_fcntl(8,F_SETFL,O_NONBLOCK)==0 && calls==2 && numbers[0]==129 && numbers[1]==123 && args1[1]==1 && all_x0(8));
 RESET2(0x400,0);CASE("setfl-clear",tested_fcntl(8,F_SETFL,0)==0 && calls==2 && args1[1]==0 && all_x0(8));
 RESET2(OM(1)|0x800,0);CASE("setfl-restate-append",tested_fcntl(8,F_SETFL,O_WRONLY|O_APPEND|O_NONBLOCK)==0 && calls==2 && args1[1]==1 && all_x0(8));
 RESET2(OM(1),0);CASE("setfl-append-on",tested_fcntl(8,F_SETFL,O_APPEND)==-1 && errno==EINVAL && calls==1 && all_x0(8));
 RESET2(OM(1)|0x800,0);CASE("setfl-append-off",tested_fcntl(8,F_SETFL,O_NONBLOCK)==-1 && errno==EINVAL && calls==1 && all_x0(8));
 RESET2(0,0);CASE("setfl-direct",tested_fcntl(8,F_SETFL,O_DIRECT)==-1 && errno==EINVAL && calls==1 && all_x0(8));
 RESET2(0,0);CASE("setfl-async",tested_fcntl(8,F_SETFL,O_ASYNC)==-1 && errno==EINVAL && calls==1 && all_x0(8));
 RESET2(0,0);CASE("setfl-noatime",tested_fcntl(8,F_SETFL,O_NOATIME)==-1 && errno==EINVAL && calls==1 && all_x0(8));
 RESET2(0,0);CASE("setfl-ignored-bits",tested_fcntl(8,F_SETFL,O_CREAT|O_TRUNC|O_RDWR|O_NONBLOCK)==0 && calls==2 && args1[1]==1 && all_x0(8));
 RESET(-EBADF);CASE("setfl-absent",tested_fcntl(8,F_SETFL,O_NONBLOCK)==-1 && errno==EBADF && calls==1 && all_x0(8));
 RESET2(0,-EBADF);CASE("setfl-closed-between",tested_fcntl(8,F_SETFL,O_NONBLOCK)==-1 && errno==EBADF && calls==2 && all_x0(8));
 RESET2(0,-1);CASE("setfl-old-kernel",tested_fcntl(8,F_SETFL,O_NONBLOCK)==-1 && errno==ENOSYS && all_x0(8));
 RESET(0);CASE("setfl-socket",tested_fcntl(POUCH_SOCK_TAG,F_SETFL,O_NONBLOCK)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("unknown-command-getown",tested_fcntl(8,F_GETOWN)==-1 && errno==ENOSYS && calls==0);
 /* pipe2: both ends flagged after the pipe exists; a failed set closes both. */
 int pf[2]={-7,-7};
 RESET(0);CASE("pipe2-plain",tested_pipe2(pf,0)==0 && pf[0]==5 && pf[1]==6 && calls==0);
 RESET(0);CASE("pipe2-cloexec",tested_pipe2(pf,O_CLOEXEC)==0 && calls==2 && numbers[0]==130 && numbers[1]==130 && args1[0]==1 && args1[1]==1 && args0[0]==5 && args0[1]==6);
 RESET(0);CASE("pipe2-nonblock",tested_pipe2(pf,O_NONBLOCK)==0 && calls==2 && numbers[0]==123 && numbers[1]==123 && args1[0]==1 && args1[1]==1 && args0[0]==5 && args0[1]==6);
 RESET(0);CASE("pipe2-both",tested_pipe2(pf,O_CLOEXEC|O_NONBLOCK)==0 && calls==4 && numbers[0]==130 && numbers[1]==123 && numbers[2]==130 && numbers[3]==123 && args0[0]==5 && args0[1]==5 && args0[2]==6 && args0[3]==6);
 RESET(0);CASE("pipe2-bad-flag",tested_pipe2(pf,O_APPEND)==-1 && errno==EINVAL && pipe_calls==0 && calls==0);
 pf[0]=pf[1]=-7;
 /* EBADF means a peer closed that end: its number may be another thread's,
  * so only the other end is closed. Any other failure closes both. */
 RESET2(0,-EBADF);CASE("pipe2-gone-end-not-closed",tested_pipe2(pf,O_CLOEXEC|O_NONBLOCK)==-1 && errno==EBADF && calls==2 && close_calls==1 && closed[0]==6 && cancellable_closes==0 && args0[0]==5 && args0[1]==5 && pf[0]==-7 && pf[1]==-7);
 RESET2(0,-EBADF);CASE("pipe2-gone-second-end",tested_pipe2(pf,O_CLOEXEC)==-1 && errno==EBADF && calls==2 && close_calls==1 && closed[0]==5 && args0[1]==6 && pf[0]==-7);
 RESET2(0,-EINVAL);CASE("pipe2-set-fails-closes-both",tested_pipe2(pf,O_CLOEXEC|O_NONBLOCK)==-1 && errno==EINVAL && close_calls==2 && closed[0]==5 && closed[1]==6 && cancellable_closes==0);
 RESET2(-1,0);CASE("pipe2-old-kernel",tested_pipe2(pf,O_CLOEXEC)==-1 && errno==ENOSYS && close_calls==2 && closed[0]==5 && closed[1]==6 && cancellable_closes==0 && args0[0]==5);
 /* A refused close-on-exec set fails the call even when nonblock was asked
  * too: the nonblock set never runs, so it cannot answer for it. */
 RESET2(-1,0);CASE("pipe2-both-old-kernel",tested_pipe2(pf,O_CLOEXEC|O_NONBLOCK)==-1 && errno==ENOSYS && calls==1 && numbers[0]==130 && close_calls==2 && closed[0]==5 && closed[1]==6 && cancellable_closes==0 && pf[0]==-7);
 RESET2(-EINVAL,0);CASE("pipe2-both-cloexec-refused",tested_pipe2(pf,O_CLOEXEC|O_NONBLOCK)==-1 && errno==EINVAL && calls==1 && numbers[0]==130 && close_calls==2 && closed[0]==5 && closed[1]==6 && pf[0]==-7);
 RESET(0);pipe_fail=1;CASE("pipe2-pipe-fails",tested_pipe2(pf,O_CLOEXEC)==-1 && errno==EMFILE && calls==0 && close_calls==0);
 /* pouch_set_cloexec (open, fdopen "e", freopen "e"): a tagged socket never
  * reaches the native call. */
 RESET(0);CASE("cloexec-native",pouch_set_cloexec(8)==0 && calls==1 && number==130 && a1==1 && all_x0(8));
 RESET(-1);CASE("cloexec-old-kernel",pouch_set_cloexec(8)==-ENOSYS && all_x0(8));
 RESET(0);CASE("cloexec-socket",pouch_set_cloexec(POUCH_SOCK_TAG)==-ENOSYS && calls==0);
 live_socket=0;
 RESET(0);CASE("cloexec-closed-socket",pouch_set_cloexec(POUCH_SOCK_TAG)==-EBADF && calls==0);
 live_socket=1;
 printf("tests=%d failures=%d\n",checks,failures);return !!failures;
}
