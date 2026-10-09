#include <thylacine/loom_service_close_queue.h>
#include <thylacine/extinction.h>

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
