// Bounded charged ring index supplies the entry lifetime required by CONNECT.
// Ll protects lookup+pin, installation, detach and removal. No allocation,
// protocol step, native wake, teardown or final drop is performed under Ll.
#include <thylacine/loom_service_engine.h>
#include <thylacine/loom_service_connect.h>
#include <thylacine/loom.h>
#include <thylacine/errno.h>
#include <thylacine/srvconn.h>
#include <thylacine/extinction.h>

static struct loom_engine_entry *engine_exact(struct Loom *l, struct loom_service_ref ref) {
    if (ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT) return NULL;
    struct loom_engine_entry *e=&l->service_engine->entries[ref.slot];
    return e->phase != LENGINE_EMPTY && e->incarnation == ref.incarnation ? e : NULL;
}
static void engine_changed(struct Loom *l) {
    loom_private_close_changed(l); // publishes epoch + wakes only after Ll
}
static struct loom_service_request *engine_request(struct Loom *l, struct loom_service_protocol *p) {
    if (p->request.index >= l->service_requests.capacity) return NULL;
    struct loom_service_request *r=&l->service_requests.records[p->request.index];
    return r->phase != LREQ_FREE && r->identity == p->request.identity ? r : NULL;
}
int loom_engine_adopt(struct loom_service_protocol *p, u64 now) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l=p->owner;
    spin_lock(&l->lock);
    struct loom_service_ref ref=p->scope;
    if (!p->connect_bound || p->indexed || p->admission.published || p->users || p->driving ||
        ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT) {
        spin_unlock(&l->lock); return -T_E_INVAL;
    }
    struct loom_engine_entry *e=&l->service_engine->entries[ref.slot];
    if (l->service_closing || e->phase != LENGINE_EMPTY) {
        int rc=l->service_closing ? -T_E_CANCELED : -T_E_BUSY;
        spin_unlock(&l->lock); return rc;
    }
    *e=(struct loom_engine_entry){.protocol=p,.incarnation=ref.incarnation,
                                 .pins=1,.phase=LENGINE_INSTALLING};
    p->indexed=true;
    ++l->service_engine->count;
    spin_unlock(&l->lock);
#ifdef KERNEL_TESTS
    loom_private_close_boundary_for_test(l,1); // unlocked INSTALLING owner
#endif
    int rc=loom_private_connect_publish(p,now);
    if (rc) (void)loom_private_connect_abort(p,rc);
    spin_lock(&l->lock);
    // Other controls may pin this installing entry; installer drops only its
    // own pin, never their pins, and holds storage through native accept wake.
    if (e->phase != LENGINE_INSTALLING || e->protocol != p || !e->pins)
        extinction("engine lost installation lifetime");
    e->phase=LENGINE_LIVE;
    --e->pins;
    spin_unlock(&l->lock);
    engine_changed(l);
    return 0;
}
static int engine_pin_locked(struct Loom *l, struct loom_service_ref ref,
                              struct loom_service_protocol **out, bool installing) {
    struct loom_engine_entry *e=engine_exact(l,ref);
    if (!e) return -T_E_NOENT;
    if (!installing && e->phase != LENGINE_LIVE) return -T_E_AGAIN;
    if (e->pins == UINT32_MAX) return -T_E_BUSY;
    ++e->pins;
    *out=e->protocol;
    return 0;
}
int loom_engine_get(struct Loom *l, struct loom_service_ref ref, struct loom_service_protocol **out) {
    if (!l || !loom_is_private(l) || !out) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc=engine_pin_locked(l,ref,out,false);
    spin_unlock(&l->lock);
    return rc;
}
static int engine_put(struct Loom *l, struct loom_service_ref ref, struct loom_service_protocol *p, bool notify) {
    if (!l || !loom_is_private(l) || !p) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct loom_engine_entry *e=engine_exact(l,ref);
    int rc=!e || e->protocol != p ? -T_E_NOENT : !e->pins ? -T_E_INVAL : 0;
    if (!rc) --e->pins;
    spin_unlock(&l->lock);
    if (!rc && notify) engine_changed(l);
    return rc;
}
int loom_engine_put(struct Loom *l, struct loom_service_ref ref, struct loom_service_protocol *p) {
    return engine_put(l,ref,p,true);
}
int loom_engine_abort(struct Loom *l, struct loom_service_ref ref, s32 reason) {
    if (!l || !loom_is_private(l) || reason >= 0) return -T_E_INVAL;
    struct loom_service_protocol *p;
    spin_lock(&l->lock);
    int rc=engine_pin_locked(l,ref,&p,true);
    spin_unlock(&l->lock);
    if (rc) return rc;
    rc=loom_private_connect_abort(p,reason);
    if (loom_engine_put(l,ref,p)) extinction("engine lost abort entry pin");
    return rc;
}
static int engine_collect(struct Loom *l, struct loom_service_ref ref, bool notify) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct loom_engine_entry *e=engine_exact(l,ref);
    if (!e) { spin_unlock(&l->lock); return -T_E_NOENT; }
    struct loom_service_protocol *p=e->protocol;
    struct loom_service_request *r=engine_request(l,p);
    if (e->phase != LENGINE_LIVE || e->pins || !p->locally_retired || p->users || p->driving ||
        !r || r->phase != LREQ_DELIVERED || r->borrows) {
        spin_unlock(&l->lock); return -T_E_BUSY;
    }
    *e=(struct loom_engine_entry){0};
    --l->service_engine->count;
    p->indexed=false;
    spin_unlock(&l->lock);
    if (loom_private_connect_release_retiring(p)) extinction("engine lost final CONNECT ownership");
    if (notify) engine_changed(l);
    return 0;
}
int loom_engine_collect(struct Loom *l, struct loom_service_ref ref) {
    return engine_collect(l,ref,true);
}
int loom_engine_visit(struct Loom *l, u64 now) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    struct loom_service_protocol *p=NULL;
    struct loom_service_ref ref={0};
    spin_lock(&l->lock);
    for (u32 n=0;n<LOOM_SERVICE_SLOT_COUNT;++n) {
        u32 at=l->service_engine->cursor;
        l->service_engine->cursor=(at+1u)%LOOM_SERVICE_SLOT_COUNT;
        struct loom_engine_entry *e=&l->service_engine->entries[at];
        if (e->phase != LENGINE_LIVE || e->pins == UINT32_MAX || e->protocol->driving) continue;
        ref=(struct loom_service_ref){.slot=at,.incarnation=e->incarnation};
        ++e->pins;p=e->protocol;break;
    }
    spin_unlock(&l->lock);
    if (!p) return 0;
    (void)loom_private_connect_step(p,now);
    (void)loom_private_post_terminal(l,p->request);
    if (loom_engine_put(l,ref,p)) extinction("engine lost progress entry pin");
    (void)loom_engine_collect(l,ref);
    return 1;
}
static int engine_discard_closed(struct Loom *l, bool notify) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    struct loom_service_ref refs[LOOM_SERVICE_SLOT_COUNT];
    u32 count=0;
    spin_lock(&l->lock);
    if (!l->service_closing || l->service_fixed_count || l->service_fixed_settling) { spin_unlock(&l->lock); return -T_E_BUSY; }
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_engine_entry *e=&l->service_engine->entries[i];
        if (e->phase == LENGINE_EMPTY) continue;
        struct loom_service_protocol *p=e->protocol;
        if (e->phase != LENGINE_LIVE || e->pins || !p->locally_retired || p->users || p->driving) {
            spin_unlock(&l->lock); return -T_E_BUSY;
        }
        refs[count++]=p->scope;
    }
    // A concurrent constructor not yet indexed still owns a request/scope pin.
    // Likewise a future non-CONNECT operation requires its own retirement proof;
    // zero parser borrows alone cannot authorize discarding its obligation.
    for (u32 i=0;i<l->service_requests.capacity;++i) {
        struct loom_service_request *r=&l->service_requests.records[i];
        if (r->phase == LREQ_FREE) continue;
        struct loom_engine_entry *e=engine_exact(l,r->scope);
        if (!e || e->protocol->request.index != i ||
            e->protocol->request.identity != r->identity ||
            (r->phase != LREQ_TERMINAL && r->phase != LREQ_DELIVERED)) {
            spin_unlock(&l->lock); return -T_E_BUSY;
        }
    }
    int rc=loom_requests_detach(&l->service_requests,l->service_slots);
    if (!rc) for (u32 i=0;i<count;++i) {
        struct loom_service_protocol *p=l->service_engine->entries[refs[i].slot].protocol;
        struct loom_service_request *r=engine_request(l,p);
        if (!r || (r->phase != LREQ_DELIVERED &&
            loom_request_discard(&l->service_requests,p->request)))
            extinction("engine lost detached terminal obligation");
    }
    spin_unlock(&l->lock);
    if (rc) return rc;
    // Lookup pins may race now, delaying collection but never invalidating
    // quiescence. No new protocol operation may be admitted after close.
    for (u32 i=0;i<count;++i) (void)engine_collect(l,refs[i],notify);
    spin_lock(&l->lock);
    bool empty=loom_engine_empty(l->service_engine);
    spin_unlock(&l->lock);
    return empty ? 0 : -T_E_BUSY;
}
bool loom_engine_empty(const struct loom_service_engine *t) {
    if (!t || t->count || t->wait_active || t->wait_mask) return false;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        const struct loom_engine_entry *e=&t->entries[i];
        if (e->phase || e->protocol || e->pins || e->incarnation) return false;
    }
    return true;
}

int loom_engine_discard_closed(struct Loom *l) {
    return engine_discard_closed(l,true);
}
int loom_engine_close_visit(struct Loom *l) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    struct loom_service_protocol *p=NULL;
    struct loom_service_ref ref={0};
    spin_lock(&l->lock);
    if (!l->service_closing) { spin_unlock(&l->lock); return -T_E_INVAL; }
    for (u32 n=0;n<LOOM_SERVICE_SLOT_COUNT;++n) {
        u32 at=l->service_engine->cursor;
        l->service_engine->cursor=(at+1u)%LOOM_SERVICE_SLOT_COUNT;
        struct loom_engine_entry *e=&l->service_engine->entries[at];
        if (e->phase != LENGINE_LIVE || e->pins==UINT32_MAX ||
            e->protocol->driving || e->protocol->users || e->protocol->locally_retired) continue;
        ++e->pins; p=e->protocol;
        ref=(struct loom_service_ref){.slot=at,.incarnation=e->incarnation};
        break;
    }
    spin_unlock(&l->lock);
    if (p) {
        // service_closing latches cancel before any native client step. No CQ
        // post/readiness wait: this visit ends the actual local cursor instead.
        (void)loom_private_connect_step_retiring(p);
        if (engine_put(l,ref,p,false)) extinction("close driver lost entry pin");
    }
    if (!engine_discard_closed(l,false)) {
        spin_lock(&l->lock);
        bool pending=l->service_engine->worker_starting ||
            (l->sqpoll && !__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE));
        spin_unlock(&l->lock);
        return pending ? 0 : 2; // actual worker exit supplies the final wake
    }
    spin_lock(&l->lock);
    bool more=false;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_engine_entry *e=&l->service_engine->entries[i];
        if (e->phase==LENGINE_LIVE && e->pins<UINT32_MAX &&
            !e->protocol->locally_retired && !e->protocol->driving && !e->protocol->users) more=true;
    }
    spin_unlock(&l->lock);
    // An external return between this observation and Q finish sets dirty;
    // a return after PARKED appends it once. Internal entry returns are quiet.
    return more ? 1 : 0;
}
// Appended to engine.c in this isolated draft. Ll protects every protocol
// observation; p->driving forbids reading its cursor while it is written
// unlocked. The caller's entry pin retains the native poll-list lifetime.
struct engine_interest {
    short events;
    bool step, post, collect;
    u64 deadline;
};
static struct engine_interest engine_interest_locked(struct Loom *l,
    struct loom_engine_entry *e, u64 now, u32 own_pins) {
    struct engine_interest a={0};
    struct loom_service_protocol *p=e->protocol;
    struct loom_service_request *r=engine_request(l,p);
    if (e->phase!=LENGINE_LIVE || !r) return a;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    a.post=!l->service_closing && r->phase==LREQ_TERMINAL &&
           (u32)(l->cq_tail-head)<l->cq_entries;
    a.collect=p->locally_retired && !p->users && !p->driving &&
              r->phase==LREQ_DELIVERED && !r->borrows && e->pins==own_pins;
    // A live driver or teardown user owns its return wake; never spin on it.
    if (p->driving || p->users || p->locally_retired) return a;
    struct loom_service_node *n=&l->service_nodes->nodes[p->scope.slot];
    if (l->service_closing || p->abort_reason || n->state==LOOM_SERVICE_ABORTING) {
        a.step=true; return a;
    }
    // Once CONNECT success committed, its old deadline no longer applies.
    if (r->phase!=LREQ_ACTIVE) return a;
    if (p->connect_deadline && p->connect_deadline<=now) {
        a.step=true; return a;
    }
    a.deadline=p->connect_deadline;
    switch (p->progress.handshake.phase) {
    case P9_HS_VERSION_SEND: case P9_HS_ATTACH_SEND: a.events=POLLOUT; break;
    case P9_HS_VERSION_RECV: case P9_HS_ATTACH_RECV: a.events=POLLIN; break;
    default: a.step=true; break; // commit READY or retire a failed cursor
    }
    return a;
}

int loom_engine_ready_visit(struct Loom *l, u64 now) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    for (u32 scanned=0;scanned<LOOM_SERVICE_SLOT_COUNT;++scanned) {
        spin_lock(&l->lock);
        u32 at=l->service_engine->cursor;
        l->service_engine->cursor=(at+1u)%LOOM_SERVICE_SLOT_COUNT;
        struct loom_engine_entry *e=&l->service_engine->entries[at];
        if (e->phase!=LENGINE_LIVE || e->pins==UINT32_MAX) {
            spin_unlock(&l->lock); continue;
        }
        ++e->pins;
        struct loom_service_protocol *p=e->protocol;
        struct loom_service_ref ref=p->scope;
        struct engine_interest a=engine_interest_locked(l,e,now,1);
        spin_unlock(&l->lock);
        // Readiness may change after this sample. The actual step is still
        // nonblocking and revalidates driver/abort/deadline under Ll.
        bool step=a.step || (a.events && srvconn_poll(p->admission.conn,true,a.events,NULL));
        if (step) (void)loom_private_connect_step(p,now);
        if (step || a.post) (void)loom_private_post_terminal(l,p->request);
        if (engine_put(l,ref,p,false)) extinction("ready visit lost entry pin");
        int collected=engine_collect(l,ref,false);
        if (step || a.post || !collected) {
            engine_changed(l);
            return 1;
        }
    }
    return 0;
}

int loom_engine_wait_arm(struct Loom *l, u64 now, u64 *deadline) {
    if (!l || !loom_is_private(l) || !deadline) return -T_E_INVAL;
    struct loom_service_engine *t=l->service_engine;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    spin_lock(&l->lock);
    if (t->wait_active) { spin_unlock(&l->lock); return -T_E_BUSY; }
    t->wait_active=true;
    t->wait_mask=0;
    t->wait_work=l->service_closing;
    t->wait_deadline=0;
    t->wait_change=__atomic_load_n(&t->change,__ATOMIC_ACQUIRE);
    t->wait_cq_head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    spin_unlock(&l->lock);
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        spin_lock(&l->lock);
        struct loom_engine_entry *e=&t->entries[i];
        if (e->phase!=LENGINE_LIVE || e->pins==UINT32_MAX) {
            spin_unlock(&l->lock); continue;
        }
        ++e->pins;
        t->wait_mask|=1ull<<i;
        struct loom_service_protocol *p=e->protocol;
        struct engine_interest a=engine_interest_locked(l,e,now,1);
        spin_unlock(&l->lock);
        struct poll_waiter *pw=&t->waiters[i];
        poll_waiter_init(pw,&l->sqpoll_park);
        // The actual SrvConn poll registers under its channel locks BEFORE
        // returning the readiness sample. Keep the entry pin until unhook.
        if (a.events && srvconn_poll(p->admission.conn,true,a.events,pw)) t->wait_work=true;
        if (a.step || a.post || a.collect) t->wait_work=true;
        if (a.deadline && (!t->wait_deadline || a.deadline<t->wait_deadline))
            t->wait_deadline=a.deadline;
    }
    *deadline=t->wait_deadline;
    // A change during registration causes a fresh sweep even if no native edge
    // happened. The final changed predicate runs again under Rendez at sleep.
    return t->wait_work || t->wait_change!=__atomic_load_n(&t->change,__ATOMIC_ACQUIRE);
}

int loom_engine_wait_changed(void *arg) {
    struct Loom *l=arg;
    struct loom_service_engine *t=l->service_engine;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    if (__atomic_load_n(&l->sqpoll_stopping,__ATOMIC_ACQUIRE) || t->wait_work ||
        t->wait_change!=__atomic_load_n(&t->change,__ATOMIC_ACQUIRE) ||
        t->wait_cq_head!=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE)) return 1;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i)
        if ((t->wait_mask&(1ull<<i)) && t->waiters[i].ready) return 1;
    return 0;
}

void loom_engine_wait_disarm(struct Loom *l) {
    struct loom_service_engine *t=l->service_engine;
    if (!t->wait_active) extinction("disarm without engine wait ownership");
    // Whole unhook sweep first; no native list may survive a consuming put.
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i)
        if (t->wait_mask&(1ull<<i)) poll_waiter_list_unregister(&t->waiters[i]);
    spin_lock(&l->lock);
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        if (!(t->wait_mask&(1ull<<i))) continue;
        struct loom_engine_entry *e=&t->entries[i];
        if (e->phase!=LENGINE_LIVE || !e->pins) extinction("engine wait lost entry pin");
        --e->pins;
    }
    t->wait_mask=0;
    t->wait_active=false;
    spin_unlock(&l->lock);
    engine_changed(l); // close may have parked on the wait owner's pins
}

_Static_assert(sizeof(struct loom_service_engine)<=4096,"wait storage budget");
