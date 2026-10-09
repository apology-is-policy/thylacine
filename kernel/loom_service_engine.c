// Bounded charged ring index supplies the entry lifetime required by CONNECT.
// Ll protects lookup+pin, installation, detach and removal. No allocation,
// protocol step, native wake, teardown or final drop is performed under Ll.
#include <thylacine/loom_service_engine.h>
#include <thylacine/loom_service_connect.h>
#include <thylacine/loom.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>

static struct loom_engine_entry *engine_exact(struct Loom *l, struct loom_service_ref ref) {
    if (ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT) return NULL;
    struct loom_engine_entry *e=&l->service_engine->entries[ref.slot];
    return e->phase != LENGINE_EMPTY && e->incarnation == ref.incarnation ? e : NULL;
}
static void engine_changed(struct Loom *l) {
    // Wake after changing the locked predicate. The existing SQPOLL Rendez is
    // only a wake target here; active private SQPOLL is not enabled. Close work
    // separately wakes the existing reference-owning retirement queue.
    wakeup(&l->sqpoll_park);
    loom_private_close_changed(l);
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
    if (!t || t->count) return false;
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
    if (!engine_discard_closed(l,false)) return 2;
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
