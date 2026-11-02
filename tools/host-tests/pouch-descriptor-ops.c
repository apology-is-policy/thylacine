#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
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
/* Linux's OFD lock commands; a host without them takes Linux's values. */
#ifndef F_OFD_GETLK
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#endif
static long results[4];
static int nresults, calls, live_socket=1, failures, checks;
static long number, a0, a1, a2, numbers[4], args0[4], args1[4];
static void *pouch_sock_resolve(int fd) { return fd==POUCH_SOCK_TAG && live_socket ? &live_socket : 0; }
/* SEEK_CUR reads the cursor through the raw call, which says "no cursor" with
 * the kernel's flat -1; SEEK_END reads the size and must not seek. */
#ifndef SYS_lseek
#define SYS_lseek 62
#endif
static off_t cur_pos=100, file_size=1000;
static int lseek_calls, fstat_calls, lseek_fail, fstat_fail;
static long mock(long n,long a,long b,long c) {
 if (n==SYS_lseek) { (void)a; lseek_calls++; assert(b==0 && c==SEEK_CUR); return lseek_fail==1 ? -EBADF : lseek_fail==2 ? -1 : cur_pos; }
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
#include "lock-under-test.h"
/* The lock call: x3 (len) and x4 (the TEST answer) beside mock's record. A
 * TEST that succeeds writes test_answer where x4 points. */
static long la3, la4;
static int cp_calls, test_writes;
static struct pouch_file_lock test_answer;
static long mock5(long n,long a,long b,long c,long d,long e,int cp) {
 la3=d;la4=e;cp_calls+=cp;
 long r=mock(n,a,b,c);
 if (r==0 && (b & POUCH_FILE_LOCK_TEST) && e) { memcpy((void *)e,&test_answer,sizeof test_answer); test_writes++; }
 return r;
}
#define __syscall5(n,a,b,c,d,e) mock5(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),0)
#define __syscall_cp(n,a,b,c,d,e) mock5(n,(long)(a),(long)(b),(long)(c),(long)(d),(long)(e),1)
static int mock_fstat(int fd,struct stat *st) { (void)fd; fstat_calls++; if (fstat_fail) { errno=EBADF; return -1; } memset(st,0,sizeof *st); st->st_size=file_size; return 0; }
#define __fstat mock_fstat
#include "fcntl-under-test.c"
#define flock tested_flock
#include "flock-under-test.c"
#undef flock
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
#define LRESET(ret) do { RESET(ret);la3=la4=-7;cp_calls=test_writes=lseek_calls=fstat_calls=lseek_fail=fstat_fail=0; } while(0)
#define FL(t,w,s,l,pid) (fl=(struct flock){.l_type=(t),.l_whence=(w),.l_start=(s),.l_len=(l),.l_pid=(pid)},&fl)
#define ANSWER(t,s,l) (test_answer=(struct pouch_file_lock){.type=(t),.start=(s),.len=(l),.pid=-1})
int main(void) {
 struct flock fl;
 /* The ABI the kernel's syscall.h states, parsed by the driver. */
 CASE("abi-number",SYS_thyla_file_lock==K_SYS_FILE_LOCK);
 CASE("abi-op",POUCH_FILE_LOCK_RD==K_RD && POUCH_FILE_LOCK_WR==K_WR && POUCH_FILE_LOCK_UN==K_UN && POUCH_FILE_LOCK_TYPE_MASK==K_TYPE_MASK && POUCH_FILE_LOCK_WAIT==K_WAIT && POUCH_FILE_LOCK_TEST==K_TEST && POUCH_FILE_LOCK_PROC==K_PROC && POUCH_FILE_LOCK_FLAGS_ALL==K_FLAGS_ALL);
 CASE("abi-answer",sizeof(struct pouch_file_lock)==K_SIZE && offsetof(struct pouch_file_lock,type)==K_OFF_type && offsetof(struct pouch_file_lock,start)==K_OFF_start && offsetof(struct pouch_file_lock,len)==K_OFF_len && offsetof(struct pouch_file_lock,pid)==K_OFF_pid);
 /* F_SETLK / F_SETLKW: the Proc owns; SETLKW waits, as a cancellation point. */
 LRESET(0);CASE("setlk-proc",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,10,5,0))==0 && calls==1 && number==137 && a1==(1|0x40) && a2==10 && la3==5 && la4==0 && cp_calls==0 && all_x0(8));
 LRESET(0);CASE("setlkw-waits",tested_fcntl(8,F_SETLKW,FL(F_RDLCK,SEEK_SET,0,1,0))==0 && calls==1 && a1==(0|0x40|0x10) && cp_calls==1 && all_x0(8));
 LRESET(0);CASE("setlkw-unlock-no-wait",tested_fcntl(8,F_SETLKW,FL(F_UNLCK,SEEK_SET,0,0,0))==0 && a1==(2|0x40) && cp_calls==1 && all_x0(8));
 LRESET(0);CASE("setlk-pid-ignored",tested_fcntl(8,F_SETLK,FL(F_UNLCK,SEEK_SET,0,0,99))==0 && calls==1 && a1==(2|0x40));
 /* F_OFD_*: the open file owns, and l_pid must be 0. */
 LRESET(0);CASE("ofd-setlk",tested_fcntl(8,F_OFD_SETLK,FL(F_WRLCK,SEEK_SET,3,4,0))==0 && calls==1 && a1==1 && a2==3 && la3==4 && cp_calls==0 && all_x0(8));
 LRESET(0);CASE("ofd-setlkw",tested_fcntl(8,F_OFD_SETLKW,FL(F_RDLCK,SEEK_SET,0,0,0))==0 && a1==0x10 && cp_calls==1);
 LRESET(0);CASE("ofd-pid",tested_fcntl(8,F_OFD_SETLK,FL(F_RDLCK,SEEK_SET,0,0,7))==-1 && errno==EINVAL && calls==1 && number==129 && all_x0(8));
 LRESET(0);CASE("ofd-pid-mode-first",tested_fcntl(8,F_OFD_SETLK,FL(F_WRLCK,SEEK_SET,0,0,7))==-1 && errno==EBADF && calls==1 && number==129);
 LRESET(OM(1));CASE("ofd-pid-wronly-rd",tested_fcntl(8,F_OFD_SETLK,FL(F_RDLCK,SEEK_SET,0,0,7))==-1 && errno==EBADF && calls==1);
 LRESET(OM(2));CASE("ofd-pid-rdwr",tested_fcntl(8,F_OFD_SETLK,FL(F_WRLCK,SEEK_SET,0,0,7))==-1 && errno==EINVAL && calls==1);
 LRESET(-EBADF);CASE("ofd-pid-closed",tested_fcntl(8,F_OFD_SETLK,FL(F_RDLCK,SEEK_SET,0,0,7))==-1 && errno==EBADF && calls==1);
 LRESET(OM(1));CASE("ofd-getlk-pid",tested_fcntl(8,F_OFD_GETLK,FL(F_RDLCK,SEEK_SET,0,0,7))==-1 && errno==EINVAL && calls==1 && test_writes==0);
 /* The range: Linux's rules, each refusal asking about a closed fd first. */
 LRESET(0);CASE("len-zero-to-eof",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,10,0,0))==0 && a2==10 && la3==0);
 LRESET(0);CASE("neg-len",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,10,-4,0))==0 && a2==6 && la3==4);
 LRESET(0);CASE("neg-len-to-zero",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,3,-3,0))==0 && a2==0 && la3==3);
 LRESET(0);CASE("neg-len-below-zero",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,2,-3,0))==-1 && errno==EINVAL && calls==1 && number==129);
 LRESET(0);CASE("neg-start",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,-1,1,0))==-1 && errno==EINVAL && calls==1 && number==129);
 LRESET(0);cur_pos=100;CASE("whence-cur",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_CUR,-10,5,0))==0 && a2==90 && la3==5 && lseek_calls==1 && fstat_calls==0);
 LRESET(0);lseek_fail=1;CASE("whence-cur-fails",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_CUR,0,1,0))==-1 && errno==EBADF && calls==0);
 LRESET(0);lseek_fail=2;CASE("whence-cur-no-cursor",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_CUR,3,1,0))==0 && calls==1 && a2==3 && la3==1 && lseek_calls==1);
 LRESET(0);file_size=1000;CASE("whence-end",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_END,-1,1,0))==0 && a2==999 && la3==1 && fstat_calls==1 && lseek_calls==0);
 LRESET(0);fstat_fail=1;CASE("whence-end-fails",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_END,0,1,0))==-1 && errno==EBADF && calls==0);
 LRESET(0);CASE("whence-bad",tested_fcntl(8,F_SETLK,FL(F_WRLCK,9,0,1,0))==-1 && errno==EINVAL && calls==1 && number==129);
 LRESET(0);CASE("start-overflow",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_END,LLONG_MAX,1,0))==-1 && errno==EOVERFLOW && calls==1 && number==129);
 LRESET(0);CASE("len-overflow",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,10,LLONG_MAX,0))==-1 && errno==EOVERFLOW && calls==1);
 LRESET(0);CASE("len-max-fits",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,1,LLONG_MAX,0))==0 && a2==1 && la3==LLONG_MAX);
 LRESET(0);CASE("type-after-range",tested_fcntl(8,F_SETLK,FL(99,SEEK_SET,10,LLONG_MAX,0))==-1 && errno==EOVERFLOW);
 LRESET(0);CASE("bad-type",tested_fcntl(8,F_SETLK,FL(99,SEEK_SET,0,1,0))==-1 && errno==EINVAL && calls==1 && number==129);
 LRESET(-EBADF);CASE("refuse-closed",tested_fcntl(8,F_SETLK,FL(99,SEEK_SET,0,1,0))==-1 && errno==EBADF && calls==1);
 /* F_GETLK: a TEST; the answer is translated, and a free range changes only l_type. */
 LRESET(0);CASE("getlk-unlck",tested_fcntl(8,F_GETLK,FL(F_UNLCK,SEEK_CUR,0,1,0))==-1 && errno==EINVAL && calls==1 && number==129 && lseek_calls==0);
 LRESET(0);ANSWER(2,10,5);CASE("getlk-free",tested_fcntl(8,F_GETLK,FL(F_WRLCK,SEEK_CUR,-90,5,4))==0 && fl.l_type==F_UNLCK && fl.l_whence==SEEK_CUR && fl.l_start==-90 && fl.l_len==5 && fl.l_pid==4 && a1==(1|0x20|0x40) && a2==10 && la3==5 && test_writes==1);
 LRESET(0);ANSWER(1,4,8);CASE("getlk-conflict",tested_fcntl(8,F_GETLK,FL(F_RDLCK,SEEK_SET,6,1,0))==0 && fl.l_type==F_WRLCK && fl.l_whence==SEEK_SET && fl.l_start==4 && fl.l_len==8 && fl.l_pid==-1 && a1==(0|0x20|0x40));
 LRESET(0);ANSWER(0,4,0);CASE("getlk-conflict-to-eof",tested_fcntl(8,F_GETLK,FL(F_WRLCK,SEEK_SET,6,1,0))==0 && fl.l_type==F_RDLCK && fl.l_start==4 && fl.l_len==0);
 LRESET(0);ANSWER(1,10,~0ULL-9);CASE("getlk-conflict-past-llmax",tested_fcntl(8,F_GETLK,FL(F_WRLCK,SEEK_SET,6,0,0))==0 && fl.l_start==10 && fl.l_len==0);
 LRESET(0);ANSWER(1,10,(unsigned long long)LLONG_MAX-9);CASE("getlk-conflict-ends-at-llmax",tested_fcntl(8,F_GETLK,FL(F_WRLCK,SEEK_SET,6,0,0))==0 && fl.l_len==0);
 LRESET(0);ANSWER(1,10,(unsigned long long)LLONG_MAX-10);CASE("getlk-conflict-ends-below-llmax",tested_fcntl(8,F_GETLK,FL(F_WRLCK,SEEK_SET,6,0,0))==0 && fl.l_len==LLONG_MAX-10);
 LRESET(0);ANSWER(0,0,0);CASE("getlk-ofd",tested_fcntl(8,F_OFD_GETLK,FL(F_RDLCK,SEEK_SET,0,0,0))==0 && a1==(0|0x20) && cp_calls==0);
 LRESET(0);ANSWER(7,0,0);CASE("getlk-bad-answer",tested_fcntl(8,F_GETLK,FL(F_RDLCK,SEEK_SET,0,0,0))==-1 && errno==EIO && fl.l_type==F_RDLCK);
 LRESET(0);ANSWER(1,(unsigned long long)LLONG_MAX+1,1);CASE("getlk-answer-unrepresentable",tested_fcntl(8,F_GETLK,FL(F_RDLCK,SEEK_SET,0,0,0))==-1 && errno==EOVERFLOW && fl.l_type==F_RDLCK);
 LRESET(-EBADF);CASE("getlk-closed",tested_fcntl(8,F_GETLK,FL(F_RDLCK,SEEK_SET,0,0,0))==-1 && errno==EBADF && test_writes==0);
 /* The kernel's answers pass through; an old kernel's bare -1 is ENOSYS. */
 LRESET(-EAGAIN);CASE("setlk-conflict",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,0,0,0))==-1 && errno==EAGAIN);
 LRESET(-ENOLCK);CASE("setlk-enolck",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,0,0,0))==-1 && errno==ENOLCK);
 LRESET(-EINTR);CASE("setlkw-eintr",tested_fcntl(8,F_SETLKW,FL(F_WRLCK,SEEK_SET,0,0,0))==-1 && errno==EINTR && cp_calls==1);
 LRESET(-1);CASE("setlk-old-kernel",tested_fcntl(8,F_SETLK,FL(F_WRLCK,SEEK_SET,0,0,0))==-1 && errno==ENOSYS);
 LRESET(0);CASE("setlk-socket",tested_fcntl(POUCH_SOCK_TAG,F_SETLK,FL(F_WRLCK,SEEK_SET,0,0,0))==-1 && errno==ENOSYS && calls==0);
 LRESET(0);CASE("setlk-negative",tested_fcntl(-1,F_SETLK,FL(F_WRLCK,SEEK_SET,0,0,0))==-1 && errno==EBADF && calls==0);
 /* flock: the open file owns the whole file; it waits unless LOCK_NB. */
 LRESET(0);CASE("flock-ex",tested_flock(8,LOCK_EX)==0 && calls==1 && number==137 && a1==(1|0x10) && a2==0 && la3==0 && la4==0 && cp_calls==0 && all_x0(8));
 LRESET(0);CASE("flock-ex-nb",tested_flock(8,LOCK_EX|LOCK_NB)==0 && a1==1);
 LRESET(0);CASE("flock-sh",tested_flock(8,LOCK_SH)==0 && a1==0x10);
 LRESET(0);CASE("flock-un",tested_flock(8,LOCK_UN)==0 && a1==2);
 LRESET(0);CASE("flock-un-nb",tested_flock(8,LOCK_UN|LOCK_NB)==0 && a1==2);
 LRESET(0);CASE("flock-bad-op",tested_flock(8,0)==-1 && errno==EINVAL && calls==0);
 LRESET(0);CASE("flock-sh-ex",tested_flock(8,LOCK_SH|LOCK_EX)==-1 && errno==EINVAL && calls==0);
 LRESET(-EBADF);CASE("flock-bad-op-closed",tested_flock(8,0)==-1 && errno==EINVAL && calls==0);
 LRESET(0);CASE("flock-bad-op-negative",tested_flock(-1,0)==-1 && errno==EINVAL && calls==0);
 LRESET(-EAGAIN);CASE("flock-would-block",tested_flock(8,LOCK_EX|LOCK_NB)==-1 && errno==EWOULDBLOCK);
 LRESET(-EBADF);CASE("flock-ex-rdonly",tested_flock(8,LOCK_EX)==-1 && errno==EBADF);
 LRESET(0);CASE("flock-socket",tested_flock(POUCH_SOCK_TAG,LOCK_EX)==-1 && errno==ENOSYS && calls==0);
 LRESET(0);CASE("flock-negative",tested_flock(-1,LOCK_EX)==-1 && errno==EBADF && calls==0);
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
