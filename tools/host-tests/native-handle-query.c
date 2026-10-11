#include <stdint.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t u64; typedef int64_t s64; typedef int hidx_t;
#define T_E_INVAL 22
#define T_E_BADF 9
#define PROC_HANDLE_MAX 1024
#define SYS_HANDLE_CLOSE_ON_EXEC 1u
struct Proc { int unused; }; struct Thread { struct Proc *proc; };
static struct Proc proc; static struct Thread thread = {&proc};
static struct Thread *current = &thread; static int lookups;
static struct Thread *current_thread(void) { return current; }
static int handle_get_cloexec(struct Proc *p, hidx_t fd) {
 assert(p == &proc && fd >= 0 && fd < PROC_HANDLE_MAX); ++lookups;
 return fd == 3 ? 0 : fd == 4 ? 1 : -1;
}
#include "handler.c"
int main(void) {
 assert(sys_handle_flags_get_handler(3)==0);
 assert(sys_handle_flags_get_handler(4)==1);
 assert(sys_handle_flags_get_handler(5)==-9);
 int before=lookups;
 assert(sys_handle_flags_get_handler(1024)==-9);
 assert(sys_handle_flags_get_handler(UINT64_MAX)==-9);
 assert(sys_handle_flags_get_handler((1ull<<32)+3)==-9);
 assert(lookups==before);
 current=0; assert(sys_handle_flags_get_handler(3)==-22);
 current=&thread; thread.proc=0; assert(sys_handle_flags_get_handler(3)==-22);
 assert(lookups==before); puts("PASS native handler boundary: 8 cases, no narrowed-alias lookup");
}
