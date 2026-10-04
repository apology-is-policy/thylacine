#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <limits.h>
typedef uint64_t u64;
#define PROC_STATE_ALIVE 1
#define PROC_THREAD_MAX 8
#define CHECK(x,msg) do { if (!(x)) { fprintf(stderr,"FAIL %s\n",msg); exit(1); } } while(0)
struct AddrSpace { int value; };
struct Proc { u64 stripes; struct AddrSpace *as; int state,thread_count,loom_sqpoll_count; const char *group_exit_msg; bool exempt; };
/* ACTUAL_DECL */
typedef pthread_mutex_t spin_lock_t;
typedef int irq_state_t;
static spin_lock_t g_proc_table_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool locked;
static irq_state_t spin_lock_irqsave(spin_lock_t *l) { pthread_mutex_lock(l); locked=true; return 0; }
static void spin_unlock_irqrestore(spin_lock_t *l,irq_state_t s) { (void)s; locked=false; pthread_mutex_unlock(l); }
static struct Proc *table[4];
static struct Proc *kproc(void) { return table[0]; }
static int proc_for_each_walk(struct Proc *root,int (*cb)(struct Proc *,void *),void *arg) {
    (void)root; CHECK(locked,"walk holds lifetime lock");
    for (unsigned i=0;i<4;i++) if(table[i]) { int r=cb(table[i],arg); if(r) return r; }
    return 0;
}
static bool proc_resource_exempt(struct Proc *p) { CHECK(locked,"budget under lifetime lock"); return p->exempt; }
/* ACTUAL_BODY */
static struct AddrSpace old_image,new_image;
static struct Proc creator,other;
static void reset(void) {
    creator=(struct Proc){.stripes=17,.as=&old_image,.state=1,.thread_count=7};
    other=(struct Proc){.stripes=18,.as=&old_image,.state=1,.loom_sqpoll_count=3};
    table[0]=&other;table[1]=&creator;table[2]=table[3]=NULL;
}
static void cases(void) {
    reset(); struct ProcSqpollTicket t={0},t2={0};
    CHECK(!proc_sqpoll_ticket_charge(0,&old_image,&t)&&!proc_sqpoll_ticket_charge(17,NULL,&t)&&!proc_sqpoll_ticket_charge(17,&old_image,NULL),"invalid input fails closed");
    CHECK(!proc_sqpoll_ticket_charge(999,&old_image,&t),"missing creator refused");
    CHECK(!proc_sqpoll_ticket_charge(17,&new_image,&t),"wrong image refused");
    creator.state=2; CHECK(!proc_sqpoll_ticket_charge(17,&old_image,&t),"zombie refused");creator.state=1;
    creator.group_exit_msg="exit";CHECK(!proc_sqpoll_ticket_charge(17,&old_image,&t),"terminating refused");creator.group_exit_msg=NULL;
    CHECK(proc_sqpoll_ticket_charge(17,&old_image,&t)&&t.stripes==17&&creator.loom_sqpoll_count==1,"exact creator charged");
    CHECK(!proc_sqpoll_ticket_charge(17,&old_image,&t2)&&!t2.stripes,"shared cap enforced");
    creator.exempt=true;
    CHECK(!proc_sqpoll_ticket_charge(17,&old_image,&t),"occupied ticket refused");
    CHECK(proc_sqpoll_ticket_charge(17,&old_image,&t2)&&creator.loom_sqpoll_count==2,"exempt charge still counted");
    creator.as=&new_image;
    proc_sqpoll_ticket_release(&t);CHECK(!t.stripes&&creator.loom_sqpoll_count==1,"refund survives exec");
    proc_sqpoll_ticket_release(&t);CHECK(creator.loom_sqpoll_count==1,"duplicate refund harmless");
    creator.state=2;proc_sqpoll_ticket_release(&t2);CHECK(!creator.loom_sqpoll_count,"zombie refund");
    creator.state=1;creator.as=&old_image;
    CHECK(proc_sqpoll_ticket_charge(17,&old_image,&t),"reap fixture charge");
    table[1]=NULL;proc_sqpoll_ticket_release(&t);
    CHECK(!t.stripes&&other.loom_sqpoll_count==3,"reaped creator cannot refund replacement");
    table[1]=&creator;creator.stripes=19;creator.loom_sqpoll_count=5;t.stripes=17;
    proc_sqpoll_ticket_release(&t);CHECK(creator.loom_sqpoll_count==5,"same address new incarnation untouched");
    creator.loom_sqpoll_count=INT_MAX;
    CHECK(!proc_sqpoll_ticket_charge(19,&old_image,&t),"exempt overflow refused");
}
struct race { struct ProcSqpollTicket *t; bool got; };
static void *charge_thread(void *arg) { struct race *r=arg;r->got=proc_sqpoll_ticket_charge(17,&old_image,r->t);return NULL; }
static void *release_thread(void *arg) {proc_sqpoll_ticket_release(arg);return NULL;}
static void races(void) {
    for(unsigned n=0;n<100;n++) {
        reset();pthread_t a,b;struct ProcSqpollTicket t={0},u={0};
        struct race ra={&t,false},rb={n&1?&u:&t,false};
        CHECK(!pthread_create(&a,NULL,charge_thread,&ra)&&!pthread_create(&b,NULL,charge_thread,&rb),"threads start");
        pthread_join(a,NULL);pthread_join(b,NULL);
        CHECK(ra.got!=rb.got&&creator.loom_sqpoll_count==1,"concurrent admission one charge");
        struct ProcSqpollTicket *winner=ra.got?ra.t:rb.t;
        // Another real charge must survive duplicate retirement of winner.
        creator.exempt=true;struct ProcSqpollTicket retained={0};
        CHECK(proc_sqpoll_ticket_charge(17,&old_image,&retained),"retained race charge");
        CHECK(!pthread_create(&a,NULL,release_thread,winner)&&!pthread_create(&b,NULL,release_thread,winner),"release threads start");
        pthread_join(a,NULL);pthread_join(b,NULL);
        CHECK(creator.loom_sqpoll_count==1&&!winner->stripes,"concurrent release once");
        proc_sqpoll_ticket_release(&retained);CHECK(!creator.loom_sqpoll_count,"race balance");
    }
}
int main(void) {cases();races();puts("PASS actual ticket admission, exec/reap/ABA and 100 admission/release races");}
