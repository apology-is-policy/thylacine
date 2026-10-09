
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_close_queue.h>
#define LOOM_MAGIC 123
#define CHECK(c,s) do {if(!(c)){puts(s);exit(11);}} while(0)
typedef struct {int held;} spin_lock_t;
struct Rendez {int unused;};
struct loom_slot_table {bool closing;};
struct Loom {u32 magic,refcount;void *service_as;bool service_closing;spin_lock_t lock;
 struct loom_slot_table *service_slots;struct loom_close_work service_close_work;
 struct Loom *service_retire_next;int cq_waiters;struct Rendez sqpoll_park;int progress;bool event;};
static spin_lock_t service_retire_lock;
static struct Rendez service_retire_wait;
static struct Loom *service_retire_head;
static struct Loom **service_retire_tail;
static bool service_retire_ready;
static struct loom_close_queue service_close_queue;
static int locks,wakes,polls,visits,destroys;
static void extinction(const char *s) {CHECK(false,s);}
static void spin_lock_init(spin_lock_t *s){s->held=0;}
static void spin_lock(spin_lock_t *s){CHECK(!locks && !s->held,"no queue/Loom lock nesting");s->held=1;locks++;}
static void spin_unlock(spin_lock_t *s){CHECK(locks==1 && s->held,"matched lock release");locks--;s->held=0;}
static void rendez_init(struct Rendez *r){r->unused=0;}
static void wakeup(struct Rendez *r){(void)r;CHECK(!locks,"scheduler wake outside locks");wakes++;}
static void poll_waiter_list_wake(int *p){(void)p;CHECK(!locks,"poll wake outside locks");polls++;}
static bool loom_is_private(const struct Loom *l){return l && l->service_as;}
static void loom_slots_close(struct loom_slot_table *t){CHECK(locks==1,"close latch serialized");t->closing=true;}
static void loom_free(struct Loom *l){(void)l;CHECK(false,"private owner entered legacy free");}
void loom_private_close_changed(struct Loom *);
static void loom_private_destroy(struct Loom *l){CHECK(!locks && l->refcount==0 &&
 (l->service_close_work.state==LCLOSE_NEW || l->service_close_work.state==LCLOSE_DONE),"final free only after queue root ended");l->magic=0;destroys++;}
static int loom_engine_close_visit(struct Loom *l){CHECK(!locks && l->refcount && l->service_closing,"driver uses live root outside Q");visits++;
 if(l->event){l->event=false;loom_private_close_changed(l);}return l->progress;}
void loom_retire_init(void);
void loom_exec_latch(struct Loom *l);
static void loom_private_close_notify(struct Loom *l);
void loom_private_close_changed(struct Loom *l);
void loom_handle_closed(struct Loom *l);
static void loom_private_enqueue(struct Loom *l);
static int loom_private_pending(void *unused);
static void loom_private_retire_one(void);
void loom_ref(struct Loom *l);
void loom_unref(struct Loom *l);
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
    if (!q->head) q->tail=&q->head;
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
void loom_retire_init(void) {
    spin_lock_init(&service_retire_lock);
    rendez_init(&service_retire_wait);
    service_retire_tail = &service_retire_head;
    loom_closeq_init(&service_close_queue);
    service_retire_ready = true;
}
void loom_exec_latch(struct Loom *l) {
    if (!loom_is_private(l)) return;
    spin_lock(&l->lock);
    l->service_closing = true;
    loom_slots_close(l->service_slots);
    spin_unlock(&l->lock);
}
static void loom_private_close_notify(struct Loom *l) {
    loom_exec_latch(l);
    poll_waiter_list_wake(&l->cq_waiters);
    wakeup(&l->sqpoll_park);
}
void loom_private_close_changed(struct Loom *l) {
    if (!loom_is_private(l)) return;
    spin_lock(&service_retire_lock);
    bool wake=loom_closeq_kick(&service_close_queue,&l->service_close_work);
    spin_unlock(&service_retire_lock);
    if (wake) wakeup(&service_retire_wait);
}
void loom_handle_closed(struct Loom *l) {
    if (!loom_is_private(l)) return;
    // Actual handle close retains l, outside process/handle-table locks. Exec's
    // earlier leaf latch deliberately does not enter this scheduler path.
    loom_private_close_notify(l);
    spin_lock(&service_retire_lock);
    bool started=loom_closeq_start(&service_close_queue,&l->service_close_work);
    if (started) loom_ref(l); // caller still holds a live ref; never resurrection
    spin_unlock(&service_retire_lock);
    if (started) wakeup(&service_retire_wait);
}
static void loom_private_enqueue(struct Loom *l) {
    // refcount is already zero. Do not call the root-taking handle-close path.
    loom_private_close_notify(l);
    spin_lock(&service_retire_lock);
    l->service_retire_next = NULL;
    *service_retire_tail = l;
    service_retire_tail = &l->service_retire_next;
    spin_unlock(&service_retire_lock);
    wakeup(&service_retire_wait);
}
static int loom_private_pending(void *unused) {
    (void)unused;
    spin_lock(&service_retire_lock);
    int pending = service_retire_head != NULL || service_close_queue.head != NULL;
    spin_unlock(&service_retire_lock);
    return pending;
}
static void loom_private_retire_one(void) {
        spin_lock(&service_retire_lock);
        struct loom_close_work *work=loom_closeq_take(&service_close_queue);
        struct Loom *l = service_retire_head;
        if (l) {
            service_retire_head = l->service_retire_next;
            if (!service_retire_head) service_retire_tail = &service_retire_head;
        }
        spin_unlock(&service_retire_lock);
        if (work) {
            struct Loom *closing=(void *)((char *)work-__builtin_offsetof(struct Loom,service_close_work));
            int progress=loom_engine_close_visit(closing);
            if (progress<0) extinction("close queue reached invalid private owner");
            spin_lock(&service_retire_lock);
            enum loom_close_finish action=loom_closeq_finish(&service_close_queue,work,progress==2,progress==1);
            spin_unlock(&service_retire_lock);
            if (action!=LCLOSE_AGAIN) loom_unref(closing); // final drop outside Q
        }
        if (l) loom_private_destroy(l);
}
void loom_ref(struct Loom *l) {
    if (!l || l->magic != LOOM_MAGIC) return;
    __atomic_fetch_add(&l->refcount, 1, __ATOMIC_ACQUIRE);
}
void loom_unref(struct Loom *l) {
    if (!l || l->magic != LOOM_MAGIC) return;
    if (__atomic_fetch_sub(&l->refcount, 1, __ATOMIC_RELEASE) == 1) {
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        if (loom_is_private(l)) loom_private_enqueue(l);
        else loom_free(l);
    }
}
int main(void) {
 struct loom_slot_table sa={0},sb={0},sc={0};
 struct Loom a={.magic=LOOM_MAGIC,.refcount=1,.service_as=&sa,.service_slots=&sa},
             b={.magic=LOOM_MAGIC,.refcount=1,.service_as=&sb,.service_slots=&sb},
             c={.magic=LOOM_MAGIC,.refcount=1,.service_as=&sc,.service_slots=&sc};
 loom_retire_init();CHECK(service_retire_ready && !loom_private_pending(NULL),"initial queues empty");
 loom_exec_latch(&a);CHECK(a.service_closing && sa.closing && !wakes && !polls && a.refcount==1 &&
                         a.service_close_work.state==LCLOSE_NEW,"exec latch remains leaf only without queue root");
 loom_handle_closed(&a);CHECK(a.refcount==2 && a.service_close_work.state==LCLOSE_READY,"handle close acquires one independent queue root");
 loom_handle_closed(&a);CHECK(a.refcount==2,"duplicate close does not duplicate root");
 loom_unref(&a);CHECK(a.refcount==1,"queue retains owner after final handle drop");
 loom_private_retire_one();CHECK(a.refcount==1 && a.service_close_work.state==LCLOSE_PARKED && !loom_private_pending(NULL) && visits==1,"blocked close parks with one root");
 int prior=wakes;loom_private_close_changed(&a);CHECK(wakes==prior+1 && loom_private_pending(NULL),"external event wakes parked retirement");
 a.event=true;loom_private_retire_one();CHECK(a.refcount==1 && a.service_close_work.state==LCLOSE_READY && visits==2,"event during worker observation retained");
 loom_private_retire_one();CHECK(a.service_close_work.state==LCLOSE_PARKED && !loom_private_pending(NULL) && visits==3,"event requests only one recheck");
 loom_handle_closed(&b);loom_unref(&b);b.progress=1;
 loom_unref(&c);CHECK(!c.refcount && c.service_close_work.state==LCLOSE_NEW && service_retire_head==&c,"zero ref enqueue never resurrects close root");
 loom_private_retire_one();CHECK(c.magic==0 && destroys==1 && b.service_close_work.state==LCLOSE_READY && b.refcount==1,"busy close cannot starve independent final destruction");
 a.progress=2;b.progress=2;loom_private_close_changed(&a);
 loom_private_retire_one();CHECK(b.refcount==0 && b.service_close_work.state==LCLOSE_DONE && a.refcount==1,"completion transfers one queue root outside lock");
 loom_private_retire_one();CHECK(a.refcount==0 && a.service_close_work.state==LCLOSE_DONE && b.magic==0,"second ring progress and first final free share turn");
 loom_private_retire_one();CHECK(a.magic==0 && destroys==3 && !loom_private_pending(NULL),"all final references physically retire once");
 CHECK(!locks,"balanced wrappers locks");puts("PASS actual close wrappers");return 0;
}
