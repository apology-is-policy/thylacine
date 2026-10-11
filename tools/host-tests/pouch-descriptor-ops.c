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
static long result;
static int calls, live_socket=1, failures, checks;
static long number, a0, a1, a2;
static void *pouch_sock_resolve(int fd) { return fd==POUCH_SOCK_TAG && live_socket ? &live_socket : 0; }
static long mock(long n,long a,long b,long c) { calls++;number=n;a0=a;a1=b;a2=c;return result; }
#define __syscall1(n,a) mock(n,a,0,0)
#define __syscall2(n,a,b) mock(n,a,b,0)
#define __syscall3(n,a,b,c) mock(n,a,b,c)
#define weak_alias(a,b)
#define dup2 tested_dup2
#define __dup3 tested_dup3
#define fcntl tested_fcntl
#include "descriptor-under-test.h"
#include "fcntl-under-test.c"
#include "dup2-under-test.c"
#include "dup3-under-test.c"
#undef fcntl
#undef dup2
#define CASE(name, expr) do { ++checks;int ok=(expr); printf("%s %s\n",ok?"PASS":"FAIL",name); failures+=!ok; } while(0)
#define RESET(ret) do { result=(ret);calls=0;errno=EDOM; } while(0)
int main(void) {
 RESET(1);CASE("get-no-vararg",tested_fcntl(8,F_GETFD)==FD_CLOEXEC && calls==1 && number==129 && a0==8);
 RESET(0);CASE("set-translates",tested_fcntl(8,F_SETFD,FD_CLOEXEC)==0 && calls==1 && number==130 && a1==1);
 RESET(0);CASE("clear",tested_fcntl(8,F_SETFD,0)==0 && a1==0);
 RESET(0);CASE("unknown-flags",tested_fcntl(8,F_SETFD,~FD_CLOEXEC)==-1 && errno==EINVAL && calls==0);
 RESET(0);CASE("unknown-command-no-vararg",tested_fcntl(8,-999)==-1 && errno==ENOSYS && calls==0);
 RESET(-1);CASE("set-old-kernel",tested_fcntl(8,F_SETFD,1)==-1 && errno==ENOSYS);
 RESET(-EBADF);CASE("get-absent",tested_fcntl(9,F_GETFD)==-1 && errno==EBADF);
 RESET(0);CASE("negative",tested_fcntl(-1,F_GETFD)==-1 && errno==EBADF && calls==0);
 RESET(9);CASE("dup2-target",tested_dup2(8,9)==9 && number==131 && a0==8 && a1==9 && a2==0);
 RESET(9);CASE("dup3-set",tested_dup3(8,9,O_CLOEXEC)==9 && a2==1);
 RESET(9);CASE("dup3-clear",tested_dup3(8,9,0)==9 && a2==0);
 RESET(9);CASE("dup3-self",tested_dup3(8,8,0)==-1 && errno==EINVAL && calls==0);
 RESET(9);CASE("dup3-invalid-flags",tested_dup3(8,9,~O_CLOEXEC)==-1 && errno==EINVAL && calls==0);
 RESET(-1);CASE("dup-old-kernel",tested_dup2(8,9)==-1 && errno==ENOSYS);
 RESET(-EACCES);CASE("alias-policy",tested_dup3(8,9,0)==-1 && errno==EACCES);
 RESET(1);CASE("self-preserves",tested_dup2(8,8)==8 && number==129 && calls==1);
 RESET(0);CASE("socket-get",tested_fcntl(POUCH_SOCK_TAG,F_GETFD)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-set",tested_fcntl(POUCH_SOCK_TAG,F_SETFD,1)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-source",tested_dup2(POUCH_SOCK_TAG,9)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-target",tested_dup2(8,POUCH_SOCK_TAG)==-1 && errno==ENOSYS && calls==0);
 RESET(0);CASE("socket-self",tested_dup2(POUCH_SOCK_TAG,POUCH_SOCK_TAG)==POUCH_SOCK_TAG && calls==0);
 live_socket=0;
 RESET(0);CASE("closed-socket",tested_fcntl(POUCH_SOCK_TAG,F_GETFD)==-1 && errno==EBADF && calls==0);
 RESET(0);CASE("malformed-socket",tested_dup3(8,INT_MAX,0)==-1 && errno==EBADF && calls==0);
 RESET(-ENOSYS);CASE("explicit-enosys",tested_dup3(8,9,0)==-1 && errno==ENOSYS);
 printf("tests=%d failures=%d\n",checks,failures);return !!failures;
}
