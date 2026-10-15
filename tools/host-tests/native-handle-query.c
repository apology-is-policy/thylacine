#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
typedef uint64_t u64; typedef int64_t s64; typedef uint32_t u32; typedef int hidx_t;
#define T_E_INVAL 22
#define T_E_BADF 9
#define PROC_HANDLE_MAX 1024
#define SYS_HANDLE_CLOSE_ON_EXEC 1u
#define SYS_HANDLE_STATUS_OMODE_SHIFT 8u
#define SYS_HANDLE_STATUS_NONBLOCK (1u << 10)
#define SYS_HANDLE_STATUS_APPEND (1u << 11)
#define SYS_WALK_OPEN_OAPPEND 0x40u
struct handle_status { bool cloexec; u32 omode; bool nonblock; };
struct Proc { int unused; }; struct Thread { struct Proc *proc; };
static struct Proc proc; static struct Thread thread = {&proc};
static struct Thread *current = &thread; static int lookups;
static struct Thread *current_thread(void) { return current; }
/* 3: plain; 4: close-on-exec; 6: ORDWR|OAPPEND, nonblock; 7: OEXEC|OTRUNC. */
static int handle_get_status(struct Proc *p, hidx_t fd, struct handle_status *st) {
 assert(p == &proc && fd >= 0 && fd < PROC_HANDLE_MAX); ++lookups;
 st->cloexec = false; st->omode = 0; st->nonblock = false;
 if (fd == 3) return 0;
 if (fd == 4) { st->cloexec = true; return 0; }
 if (fd == 6) { st->omode = 0x42; st->nonblock = true; return 0; }
 if (fd == 7) { st->omode = 0x13; return 0; }
 return -1;
}
#include "handler.c"
int main(void) {
 assert(sys_handle_flags_get_handler(3)==0);
 assert(sys_handle_flags_get_handler(4)==1);
 assert(sys_handle_flags_get_handler(5)==-9);
 assert(sys_handle_flags_get_handler(6)==0xE00);
 assert(sys_handle_flags_get_handler(7)==0x300);
 int before=lookups;
 assert(sys_handle_flags_get_handler(1024)==-9);
 assert(sys_handle_flags_get_handler(UINT64_MAX)==-9);
 assert(sys_handle_flags_get_handler((1ull<<32)+3)==-9);
 assert(lookups==before);
 current=0; assert(sys_handle_flags_get_handler(3)==-22);
 current=&thread; thread.proc=0; assert(sys_handle_flags_get_handler(3)==-22);
 assert(lookups==before); puts("PASS native handler boundary: 10 cases, no narrowed-alias lookup");
}
