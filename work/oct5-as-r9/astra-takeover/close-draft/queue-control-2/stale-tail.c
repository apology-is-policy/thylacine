#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x,m) do { if (!(x)) { fprintf(stderr,"FAIL: %s\n",m); exit(73); } } while (0)
_Noreturn static void extinction(const char *s) { fprintf(stderr,"EXTINCTION: %s\n",s); exit(74); }
// Intrusive queue state only: caller serializes every operation with Q lock.
#ifndef THYLACINE_LOOM_SERVICE_CLOSE_QUEUE_H
#define THYLACINE_LOOM_SERVICE_CLOSE_QUEUE_H
enum loom_close_state { LCLOSE_NEW, LCLOSE_READY, LCLOSE_RUNNING, LCLOSE_PARKED, LCLOSE_DONE };
struct loom_close_work {
    struct loom_close_work *next;
    enum loom_close_state state;
    bool dirty;
};
struct loom_close_queue {
    struct loom_close_work *head;
    struct loom_close_work **tail;
};
enum loom_close_finish { LCLOSE_WAIT, LCLOSE_AGAIN, LCLOSE_DROP };
void loom_closeq_init(struct loom_close_queue *);
// Zero node starts once. Caller holds a live Loom ref; when this returns true,
// take exactly one queue reference BEFORE unlocking. Atomic ref increment may
// occur under Q, but final ref drops, Ll acquisition and wakes never do.
bool loom_closeq_start(struct loom_close_queue *, struct loom_close_work *);
// External prerequisite changed AFTER finishing its physical pointer accesses.
// RUNNING records dirty; PARKED enqueues once. Return true means wake after Q.
// Internal driver puts MUST NOT call this: they would cause perpetual requeue.
bool loom_closeq_kick(struct loom_close_queue *, struct loom_close_work *);
struct loom_close_work *loom_closeq_take(struct loom_close_queue *);
// Worker retained its queue root across unlocked work. complete proves no
// remaining local protocol ownership; more means useful immediate work, not
// merely a nonempty owner index. DROP transfers the queue ref to the caller,
// who must release it exactly once AFTER unlocking Q. DONE cannot be restarted.
enum loom_close_finish loom_closeq_finish(struct loom_close_queue *, struct loom_close_work *, bool complete, bool more);
#endif

void loom_closeq_init(struct loom_close_queue *q) {
    q->head=NULL; q->tail=&q->head;
}
static void closeq_append(struct loom_close_queue *q, struct loom_close_work *w) {
    w->next=NULL;
    *q->tail=w; q->tail=&w->next;
    w->state=LCLOSE_READY;
}
bool loom_closeq_start(struct loom_close_queue *q, struct loom_close_work *w) {
    if (w->state != LCLOSE_NEW) return false;
    w->dirty=false;
    closeq_append(q,w);
    return true;
}
bool loom_closeq_kick(struct loom_close_queue *q, struct loom_close_work *w) {
    if (w->state == LCLOSE_RUNNING) w->dirty=true;
    if (w->state != LCLOSE_PARKED) return false;
    closeq_append(q,w);
    return true;
}
struct loom_close_work *loom_closeq_take(struct loom_close_queue *q) {
    struct loom_close_work *w=q->head;
    if (!w) return NULL;
    if (w->state != LCLOSE_READY) extinction("close queue lost ready membership");
    q->head=w->next;
    if (false) q->tail=&q->head;
    w->next=NULL; w->state=LCLOSE_RUNNING; w->dirty=false;
    return w;
}
enum loom_close_finish loom_closeq_finish(struct loom_close_queue *q, struct loom_close_work *w, bool complete, bool more) {
    if (w->state != LCLOSE_RUNNING) extinction("close queue lost running root");
    if (complete) {
        w->state=LCLOSE_DONE; w->dirty=false;
        return LCLOSE_DROP;
    }
    if (more || w->dirty) {
        closeq_append(q,w);
        return LCLOSE_AGAIN;
    }
    w->state=LCLOSE_PARKED;
    return LCLOSE_WAIT;
}

static struct loom_close_queue q;
static struct loom_close_work a,b;
static unsigned roots;
static void init(void) { memset(&a,0,sizeof a);memset(&b,0,sizeof b);roots=0;loom_closeq_init(&q); }
static bool start(struct loom_close_work *w) { bool added=loom_closeq_start(&q,w);if(added)roots++;return added; }
static enum loom_close_finish finish(struct loom_close_work *w,bool done,bool more) {
 enum loom_close_finish r=loom_closeq_finish(&q,w,done,more);
 if(r==LCLOSE_DROP){CHECK(roots,"root release underflow");roots--;}
 return r;
}
int main(void) {
 init();
 CHECK(!loom_closeq_take(&q),"empty queue has no work");
 CHECK(!loom_closeq_kick(&q,&a) && a.state==LCLOSE_NEW,"kick cannot start a root");
 CHECK(start(&a) && roots==1,"first start owns one root");
 CHECK(!start(&a) && roots==1,"duplicate start retains exactly one root");
 CHECK(!loom_closeq_kick(&q,&a) && a.next==NULL,"ready kick cannot duplicate link");
 CHECK(start(&b) && roots==2,"second ring owns separate root");
 CHECK(loom_closeq_take(&q)==&a && a.state==LCLOSE_RUNNING && !a.next,"take transfers membership without dropping root");
 CHECK(finish(&a,false,true)==LCLOSE_AGAIN,"immediately useful work requeues");
 CHECK(loom_closeq_take(&q)==&b,"fair FIFO gives other ring a turn");
 CHECK(finish(&b,true,false)==LCLOSE_DROP && roots==1,"complete drops exactly one root");
 CHECK(!start(&b) && !loom_closeq_kick(&q,&b) && b.state==LCLOSE_DONE,"completed ring cannot restart");
 CHECK(loom_closeq_take(&q)==&a && !q.head && q.tail==&q.head,"tail returns to queue head after last take");
 CHECK(finish(&a,false,false)==LCLOSE_WAIT && a.state==LCLOSE_PARKED && roots==1 && !q.head,"blocked ring parks without self wake");
 CHECK(!loom_closeq_take(&q),"parked ring consumes no runnable turn");
 CHECK(loom_closeq_kick(&q,&a) && a.state==LCLOSE_READY && roots==1,"external put wakes parked ring using same root");
 CHECK(!loom_closeq_kick(&q,&a) && !a.next,"multiple parked kicks append once");
 CHECK(loom_closeq_take(&q)==&a && !a.dirty,"new turn consumes earlier kick");
 CHECK(!loom_closeq_kick(&q,&a) && a.dirty,"event between observation and park recorded");
 CHECK(finish(&a,false,false)==LCLOSE_AGAIN && q.head==&a,"concurrent external event prevents missed wake");
 CHECK(loom_closeq_take(&q)==&a && !a.dirty,"old dirty bit does not survive next take");
 CHECK(finish(&a,false,false)==LCLOSE_WAIT && !q.head,"dirty event grants one recheck not endless spin");
 CHECK(loom_closeq_kick(&q,&a) && loom_closeq_take(&q)==&a,"second external event starts next turn");
 CHECK(!loom_closeq_kick(&q,&a) && a.dirty,"completion can race harmless late kick");
 CHECK(finish(&a,true,true)==LCLOSE_DROP && roots==0 && a.state==LCLOSE_DONE && !q.head,"complete outranks stale kick and more flag");
 CHECK(!loom_closeq_kick(&q,&a) && !start(&a),"late kicks cannot resurrect completed root");
 puts("PASS queue control: FIFO, exact root actions, duplicate start, park quiet, before/after park kicks, completion, no restart");
 return 0;
}
