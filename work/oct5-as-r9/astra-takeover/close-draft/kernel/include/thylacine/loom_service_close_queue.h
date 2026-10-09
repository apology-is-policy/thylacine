// Intrusive queue state only: caller serializes every operation with Q lock.
#ifndef THYLACINE_LOOM_SERVICE_CLOSE_QUEUE_H
#define THYLACINE_LOOM_SERVICE_CLOSE_QUEUE_H
#include <thylacine/types.h>
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
