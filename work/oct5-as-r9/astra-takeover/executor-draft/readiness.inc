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
        // A concurrent close/wait sweep may have parked on this temporary
        // pin. Observe that dependency in the SAME Ll interval as its return;
        // sampling before or after an independent put can miss the last wake.
        spin_lock(&l->lock);
        if (e->phase!=LENGINE_LIVE || e->protocol!=p || !e->pins)
            extinction("ready visit lost entry pin");
        --e->pins;
        bool dependent=l->service_closing || l->service_engine->wait_active;
        spin_unlock(&l->lock);
        if (dependent) engine_changed(l);
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
