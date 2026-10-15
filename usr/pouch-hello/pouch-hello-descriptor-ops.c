/* Ordinary-user native descriptor witness; tools/interactive/ls-descriptors.exp
 * runs it as uid 1000 (the default mode; the others are its own children). */
#define _GNU_SOURCE
#include <thyla/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/socket.h>
static int checks;
static void check(int ok,const char *name){printf("%s OPS %s\n",ok?"PASS":"FAIL",name);if(!ok)exit(1);checks++;}
static int child(const char *path,const char *mode,posix_spawn_file_actions_t *fa){pid_t pid=-1;int status=-1;char *args[]={(char*)path,(char*)mode,0};int ec=posix_spawn(&pid,path,fa,0,args,0);if(ec)return -ec;return waitpid(pid,&status,0)==pid && WIFEXITED(status) ? WEXITSTATUS(status) : -999;}
static long image_replace(const char *path){
 const char packed[]="descriptor-probe\0exec-check\0";
 register long x0 __asm__("x0")=(long)path;
 register long x1 __asm__("x1")=strlen(path);
 register long x2 __asm__("x2")=(long)packed;
 register long x3 __asm__("x3")=sizeof(packed)-1;
 register long x4 __asm__("x4")=2;
 register long x8 __asm__("x8")=104;
 __asm__ volatile("svc #0":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x8):"memory","cc");return x0;
}
int main(int argc,char **argv){
 if(argc==2 && !strcmp(argv[1],"stderr-closed"))return t_handle_flags_get(2)==-EBADF?0:21;
 if(argc==2 && !strcmp(argv[1],"stderr-open"))return t_handle_flags_get(2)==0?0:22;
 if(argc==2 && !strcmp(argv[1],"exec-check")){
  int ok=t_handle_flags_get(40)==-EBADF && t_handle_flags_get(41)==0;
  if(t_close(41)!=0)ok=0;return ok?0:23;
 }
 if(argc==2 && !strcmp(argv[1],"exec-start")){
  int fd=open("/dev/null",O_RDONLY);
  if(fd<0 || dup3(fd,40,O_CLOEXEC)!=40 || dup2(fd,41)!=41 || close(fd))return 24;
  long ret=image_replace(argv[0]);printf("EXEC-FAIL raw=%ld\n",ret);return 25;
 }
 check(getuid()==1000,"ordinary-user");int fd=open("/dev/null",O_RDONLY);check(fd>=3,"open");
 if(argc==2 && !strcmp(argv[1],"old-kernel")){
  errno=0;check(fcntl(fd,F_SETFD,FD_CLOEXEC)==-1 && errno==ENOSYS,"old-set-refused");
  errno=0;check(dup2(fd,40)==-1 && errno==ENOSYS,"old-target-refused");
  check(t_handle_flags_get(fd)==0 && t_handle_flags_get(40)==-EBADF,"old-state-unchanged");
  check(close(fd)==0,"old-cleanup");printf("OPS-OLD-END tests=%d\n",checks);return 0;
 }
 check(argc==1,"expected-mode");check(fcntl(fd,F_GETFD)==0,"get-zero");
 check(fcntl(fd,F_SETFD,FD_CLOEXEC)==0 && fcntl(fd,F_GETFD)==FD_CLOEXEC,"set-readback");
 check(dup2(fd,fd)==fd && fcntl(fd,F_GETFD)==FD_CLOEXEC,"self-preserves");
 check(dup2(fd,40)==40 && fcntl(40,F_GETFD)==0,"exact-target-clears");
 check(dup3(fd,40,O_CLOEXEC)==40 && fcntl(40,F_GETFD)==FD_CLOEXEC,"exact-target-sets");
 check(dup2(fd,40)==40 && fcntl(40,F_GETFD)==0,"occupied-target-clears");
 errno=0;check(dup3(fd,fd,0)==-1 && errno==EINVAL,"dup3-self-refused");
 errno=0;check(dup3(fd,40,0x40000000)==-1 && errno==EINVAL,"dup3-bad-flags");
 check(t_handle_flags_set((1UL<<32)+fd,1)==-EBADF && t_handle_dup_to(fd,(1UL<<32)+40,0)==-EBADF,"no-narrowing-alias");
 check(t_handle_flags_set(fd,2)==-EINVAL && fcntl(fd,F_GETFD)==FD_CLOEXEC,"native-bad-flags-preserve");
 errno=0;check(dup2(1023,40)==-1 && errno==EBADF && t_handle_flags_get(40)==0,"absent-source-preserves-target");
 long narrow=t_dup(fd,T_RIGHT_TRANSFER);check(narrow>=0 && t_handle_dup_to(narrow,41,1)==41,"narrow-rights-target");char byte;
 check(t_read(41,&byte,1)<0 && t_write(41,&byte,1)<0 && t_handle_flags_get(41)==1,"rights-not-widened");
 check(t_close(narrow)==0 && close(41)==0,"narrow-cleanup");
 long held[1024];int n=0;while(n<1024){long h=t_dup(fd,T_RIGHT_TRANSFER);if(h<0)break;held[n++]=h;}
 int live=0;for(int i=0;i<1024;i++)live+=t_handle_flags_get(i)>=0;
 long a=t_handle_flags_set(40,1),b=t_handle_dup_to(fd,40,0),c=t_handle_flags_get(40);int failures=0;
 for(int i=0;i<n;i++)failures+=t_close(held[i])!=0;
 check(live==1024 && a==0 && b==40 && c==0,"full-table-mutation");check(failures==0,"full-table-cleanup");
 int sock=socket(AF_UNIX,SOCK_STREAM,0);check(sock>=0,"socket-open");errno=0;
 check(fcntl(sock,F_SETFD,FD_CLOEXEC)==-1 && errno==ENOSYS,"socket-flags-refused");errno=0;
 check(dup2(fd,sock)==-1 && errno==ENOSYS && dup2(sock,sock)==sock,"socket-target-intact");check(close(sock)==0,"socket-cleanup");
 check(fcntl(2,F_SETFD,FD_CLOEXEC)==0,"flag-stderr");
 check(child(argv[0],"stderr-closed",0)==0,"spawn-excludes-flagged-stream");
 posix_spawn_file_actions_t fa;check(posix_spawn_file_actions_init(&fa)==0 && posix_spawn_file_actions_adddup2(&fa,2,2)==0,"self-action-setup");
 check(child(argv[0],"stderr-open",&fa)==0,"spawn-self-action-clears-child-flag");posix_spawn_file_actions_destroy(&fa);
 check(fcntl(2,F_GETFD)==FD_CLOEXEC && fcntl(2,F_SETFD,0)==0,"parent-flag-preserved");
 check(fcntl(1,F_SETFD,FD_CLOEXEC)==0,"flag-stdout");
 int sparse=child(argv[0],"stderr-open",0);long clear=fcntl(1,F_SETFD,0);check(sparse==-EINVAL && clear==0,"sparse-spawn-refuses");
 check(posix_spawn_file_actions_init(&fa)==0 && posix_spawn_file_actions_addopen(&fa,2,"/dev/null",O_RDONLY|O_CLOEXEC,0)==0,"open-action-setup");
 check(child(argv[0],"stderr-open",&fa)==-ENOSYS,"open-flags-explicit-refusal");posix_spawn_file_actions_destroy(&fa);
 check(child(argv[0],"exec-start",0)==0,"native-exec-consumes-flags");
 check(close(40)==0 && close(fd)==0,"cleanup");printf("OPS-END tests=%d filled=%d\n",checks,n);return 0;
}
