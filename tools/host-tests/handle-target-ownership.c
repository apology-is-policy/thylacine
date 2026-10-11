/* Actual kernel replacement body, pthread lock and bounded reference owners.
 * This checks publication/release ordering, not the real KObj release hooks. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
typedef uint64_t u64;typedef int hidx_t;typedef unsigned rights_t;
#define PROC_HANDLE_MAX 1024
#define HANDLE_MAGIC 77
#define T_E_BADF 9
#define T_E_INVAL 22
#define T_E_ACCES 13
enum kobj_kind {KOBJ_INVALID,KOBJ_PROCESS,KOBJ_SRV};
struct Object {atomic_int refs;};
struct Handle {int magic;enum kobj_kind kind;rights_t rights;void *obj;};
struct HandleTable {pthread_mutex_t lock;struct Handle slots[1024];bool flags[1024];};
struct Proc {struct HandleTable table;};
static struct Proc proc;static struct Object objects[2];
static u64 g_handle_allocated,g_handle_freed;
static _Thread_local int locked;
static atomic_int stop;
static void spin_lock(pthread_mutex_t *m){assert(!locked);assert(!pthread_mutex_lock(m));locked=1;}
static void spin_unlock(pthread_mutex_t *m){assert(locked);locked=0;assert(!pthread_mutex_unlock(m));}
static struct HandleTable *proc_handles_or_extinct(struct Proc *p){assert(p);return &p->table;}
static bool handle_slot_may_alias(struct Handle *s){assert(locked);return s->kind==KOBJ_PROCESS;}
static void handle_acquire_obj(enum kobj_kind k,void *o){assert(locked && k==KOBJ_PROCESS);atomic_fetch_add(&((struct Object*)o)->refs,1);}
static void handle_release_obj(enum kobj_kind k,void *o){assert(!locked && k==KOBJ_PROCESS);assert(atomic_fetch_sub(&((struct Object*)o)->refs,1)>0);}
static void cloexec_set_locked(struct HandleTable *t,int fd,bool v){assert(locked);t->flags[fd]=v;}
#include "target-under-test.c"
static void install(int fd,int object){spin_lock(&proc.table.lock);assert(!proc.table.slots[fd].magic);proc.table.slots[fd]=(struct Handle){HANDLE_MAGIC,KOBJ_PROCESS,4,&objects[object]};handle_acquire_obj(KOBJ_PROCESS,&objects[object]);spin_unlock(&proc.table.lock);}
static void remove_slot(int fd){spin_lock(&proc.table.lock);struct Handle h=proc.table.slots[fd];proc.table.slots[fd].magic=0;spin_unlock(&proc.table.lock);if(h.magic)handle_release_obj(h.kind,h.obj);}
static void *reuse(void *unused){(void)unused;for(int i=0;i<20000;i++){remove_slot(1);install(1,i&1);}atomic_store(&stop,1);return 0;}
static void *duplicate(void *unused){(void)unused;do{int v=handle_dup_to_checked(&proc,1,2,true);assert(v==2 || v==-9);spin_lock(&proc.table.lock);assert(proc.table.slots[2].magic==HANDLE_MAGIC && proc.table.slots[2].rights==4);spin_unlock(&proc.table.lock);}while(!atomic_load(&stop));return 0;}
int main(void){
 assert(!pthread_mutex_init(&proc.table.lock,0));install(1,0);install(2,1);
 assert(handle_dup_to_checked(&proc,1,2,true)==2 && proc.table.flags[2]);
 assert(atomic_load(&objects[0].refs)==2 && atomic_load(&objects[1].refs)==0);
 assert(g_handle_allocated==1 && g_handle_freed==1);
 assert(handle_dup_to_checked(&proc,1,2,false)==2 && !proc.table.flags[2]);
 assert(atomic_load(&objects[0].refs)==2); // same object replacement balanced
 assert(handle_dup_to_checked(&proc,1,3,true)==3 && proc.table.flags[3]);
 assert(g_handle_allocated-g_handle_freed==1);
 assert(handle_dup_to_checked(&proc,9,2,true)==-9 && !proc.table.flags[2]);
 assert(handle_dup_to_checked(&proc,1,1,true)==-22);
 assert(handle_dup_to_checked(&proc,-1,2,0)==-9);
 assert(handle_dup_to_checked(&proc,1,1024,0)==-9);
 spin_lock(&proc.table.lock);proc.table.slots[1].kind=KOBJ_SRV;spin_unlock(&proc.table.lock);
 assert(handle_dup_to_checked(&proc,1,2,true)==-13 && !proc.table.flags[2]);
 spin_lock(&proc.table.lock);proc.table.slots[1].kind=KOBJ_PROCESS;spin_unlock(&proc.table.lock);
 assert(handle_dup_to(&proc,9,2,true)==-1);
 pthread_t a,b;assert(!pthread_create(&a,0,reuse,0));assert(!pthread_create(&b,0,duplicate,0));assert(!pthread_join(a,0));assert(!pthread_join(b,0));
 for(int i=0;i<1024;i++) if(!proc.table.slots[i].magic) install(i,0);
 assert(handle_dup_to_checked(&proc,1,2,true)==2); // no free slots required
 for(int i=0;i<1024;i++)remove_slot(i);
 assert(atomic_load(&objects[0].refs)==0 && atomic_load(&objects[1].refs)==0);
 puts("PASS actual target replacement: rights, flags, errors, references, full table, 20000 concurrent close/reuse iterations");
}
