#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <stdbool.h>
typedef uint64_t u64;typedef int64_t s64;typedef int hidx_t;
#define T_E_INVAL 22
#define T_E_BADF 9
#define T_E_ACCES 13
#define T_E_MFILE 24
#define PROC_HANDLE_MAX 1024
#define SYS_HANDLE_CLOSE_ON_EXEC 1u
#define SYS_HANDLE_DUP_LOWEST 2u
struct Proc {int unused;};struct Thread {struct Proc *proc;};
static struct Proc proc;static struct Thread thread={&proc},*current=&thread;
static int calls,flags,alias_error,checks,lowest;
static struct Thread *current_thread(void){return current;}
static int handle_set_cloexec(struct Proc *p,int fd,int f){assert(p==&proc && fd>=0 && fd<1024);calls++;if(fd!=3)return -1;flags=f;return 0;}
static int handle_dup_to_checked(struct Proc *p,int src,int dst,int f){assert(p==&proc && src>=0 && src<1024 && dst>=0 && dst<1024);calls++;if(src!=3)return -9;if(alias_error)return -13;flags=f;return dst;}
/* Slots 0-4 occupied, every other index free; a floor above 1000 finds none.
 * The core decides a closed source before a floor out of range, and the
 * handler hands it an out-of-range floor clamped to the table size. */
static int handle_dup_posix_checked(struct Proc *p,int src,int min,bool f){assert(p==&proc && src>=0 && src<1024 && min>=0 && min<=1024);calls++;lowest++;if(src!=3)return -9;if(min>=1024)return -22;if(alias_error)return -13;if(min>1000)return -24;flags=f;return min<5?5:min;}
#include "handlers.c"
#define C(x) do{checks++;assert(x);}while(0)
int main(void){
 C(sys_handle_flags_set_handler(3,1)==0 && flags==1);
 C(sys_handle_flags_set_handler(3,0)==0 && flags==0);
 C(sys_handle_flags_set_handler(4,1)==-9);
 int before=calls;
 C(sys_handle_flags_set_handler(3,2)==-22);
 C(sys_handle_flags_set_handler(3,1ull<<32)==-22);
 C(sys_handle_flags_set_handler(1024,0)==-9);
 C(sys_handle_flags_set_handler(UINT64_MAX,0)==-9);
 C(sys_handle_flags_set_handler((1ull<<32)+3,0)==-9);
 C(calls==before);
 C(sys_handle_dup_to_handler(3,40,1)==40 && flags==1);
 C(sys_handle_dup_to_handler(3,40,0)==40 && flags==0);
 C(sys_handle_dup_to_handler(4,40,0)==-9);
 alias_error=1;C(sys_handle_dup_to_handler(3,40,0)==-13);alias_error=0;
 /* SYS_HANDLE_DUP_LOWEST: x1 is a floor; the exact-target core is never asked. */
 int exact=calls-lowest;
 C(sys_handle_dup_to_handler(3,0,2)==5 && flags==0);
 C(sys_handle_dup_to_handler(3,10,3)==10 && flags==1);
 C(sys_handle_dup_to_handler(3,3,2)==5);
 C(sys_handle_dup_to_handler(3,1001,2)==-24);
 C(sys_handle_dup_to_handler(4,0,2)==-9);
 alias_error=1;C(sys_handle_dup_to_handler(3,0,2)==-13);alias_error=0;
 C(sys_handle_dup_to_handler(3,1024,2)==-22);
 C(sys_handle_dup_to_handler(3,UINT64_MAX,3)==-22);
 C(sys_handle_dup_to_handler(3,(1ull<<32)+10,2)==-22);
 C(sys_handle_dup_to_handler(4,1024,2)==-9);
 C(calls-lowest==exact && lowest==10);
 before=calls;
 C(sys_handle_dup_to_handler(3,3,1)==-22);
 C(sys_handle_dup_to_handler(3,40,4)==-22);
 C(sys_handle_dup_to_handler(3,40,7)==-22);
 C(sys_handle_dup_to_handler(UINT64_MAX,0,2)==-9);
 C(sys_handle_dup_to_handler((1ull<<32)+3,0,2)==-9);
 C(sys_handle_dup_to_handler(3,(1ull<<32)+40,0)==-9);
 C(sys_handle_dup_to_handler((1ull<<32)+3,40,0)==-9);
 C(sys_handle_dup_to_handler(UINT64_MAX,40,0)==-9);
 C(sys_handle_dup_to_handler(3,1024,0)==-9);
 current=0;C(sys_handle_flags_set_handler(3,0)==-22);C(sys_handle_dup_to_handler(3,40,0)==-22);
 current=&thread;thread.proc=0;C(sys_handle_flags_set_handler(3,0)==-22);C(sys_handle_dup_to_handler(3,40,0)==-22);
 C(calls==before);printf("PASS native mutation handlers tests=%d\n",checks);
}
