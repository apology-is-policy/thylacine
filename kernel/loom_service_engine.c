// Bounded charged ring index supplies the entry lifetime required by CONNECT.
// Ll protects lookup+pin, installation, detach and removal. No allocation,
// protocol step, native wake, teardown or final drop is performed under Ll.
#include <thylacine/loom_service_engine.h>
#include <thylacine/loom_service_frontend.h>
#include <thylacine/loom_service_connect.h>
#include <thylacine/loom.h>
#include <thylacine/loom_service_action.h>
#include "../arch/arm64/timer.h"
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
    if (e->phase != LENGINE_LIVE || e->pins || p->op_count || p->op_head || p->op_tail || !p->locally_retired || p->users || p->driving ||
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
int loom_engine_visit(struct Loom *l,u64 now) {
    return loom_engine_ready_visit(l,now);
}
static int engine_discard_closed(struct Loom *l, bool notify) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    struct loom_service_ref refs[LOOM_SERVICE_SLOT_COUNT];
    u32 count=0;
    spin_lock(&l->lock);
    if (!l->service_closing || l->service_engine->submitting || l->service_engine->rejected || l->service_fixed_count || l->service_fixed_settling) { spin_unlock(&l->lock); return -T_E_BUSY; }
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_engine_entry *e=&l->service_engine->entries[i];
        if (e->phase == LENGINE_EMPTY) continue;
        struct loom_service_protocol *p=e->protocol;
        if (e->phase != LENGINE_LIVE || e->pins || p->op_count || p->op_head || p->op_tail || !p->locally_retired || p->users || p->driving) {
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
    if (!t || t->count || t->submitting || t->rejected || t->wait_active || t->wait_mask) return false;
    for (u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        const struct loom_engine_entry *e=&t->entries[i];
        if (e->phase || e->protocol || e->pins || e->incarnation) return false;
    }
    return true;
}

int loom_engine_discard_closed(struct Loom *l) {
    return engine_discard_closed(l,true);
}
// Ll protects every protocol
// observation; p->driving forbids reading its cursor while it is written
// unlocked. The caller's entry pin retains the native poll-list lifetime.
// The CONNECT record identifies handshake ownership. Its successful terminal
// ends handshake deadlines, even while CQ delivery waits; subsequent visits
// belong to the ordinary operation driver.
static bool engine_handshake_locked(struct Loom *l,struct loom_service_protocol *p) {
    struct loom_service_request *r=engine_request(l,p);
    return r && (r->phase==LREQ_PREPARED || r->phase==LREQ_ACTIVE);
}
static bool engine_retirable_locked(struct Loom *l,struct loom_service_protocol *p) {
    if(p->locally_retired || p->driving || p->users || !p->rpc_quiesced || !p->abort_reason)
        return false;
    u32 count=0;
    for(struct loom_service_operation *o=p->op_head;o;o=o->next) {
        if(++count>p->op_count)extinction("retire operation list corrupt");
        if(o->phase!=LRPC_TERMINAL || o->source_borrow || o->fid_claim || o->fixed.owner ||
           o->fixed_dropping || o->pool_busy || o->rpc.on_complete || o->rpc.sending)return false;
    }
    if(count!=p->op_count)extinction("retire operation count corrupt");
    for(u32 i=0;i<l->service_requests.capacity;++i) {
        struct loom_service_request *r=&l->service_requests.records[i];
        if(r->phase!=LREQ_FREE && r->scope.slot==p->scope.slot &&
           r->scope.incarnation==p->scope.incarnation &&
           (r->borrows || r->phase==LREQ_ACTIVE))return false;
    }
    for(u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_service_node *n=&l->service_nodes->nodes[i];
        bool member=i==p->scope.slot || (n->parent.slot==p->scope.slot &&
                    n->parent.incarnation==p->scope.incarnation);
        if(member && (n->fid_borrows || n->fid_claim))return false;
    }
    return true;
}
static bool engine_retire_locked(struct Loom *l,struct loom_service_protocol *p) {
    if(!engine_retirable_locked(l,p))return false;
    struct loom_service_node *n=&l->service_nodes->nodes[p->scope.slot];
    p->locally_retired=true;n->state=LOOM_SERVICE_RETIRED;
    n->status_flags|=LOOM_SERVICE_LOCAL_RETIRED;
    if(p->progress.io.bytes_sent)n->status_flags|=LOOM_SERVICE_BYTES_SENT;
    return true;
}
struct engine_interest {
    short events;
    bool step, post, collect, handshake, retire;
    u64 deadline;
};
static struct engine_interest engine_interest_locked(struct Loom *l,
    struct loom_engine_entry *e,u64 now,u32 own_pins) {
    struct engine_interest a={0};struct loom_service_protocol *p=e->protocol;
    struct loom_service_request *r=engine_request(l,p);
    if(e->phase!=LENGINE_LIVE || !r)return a;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    u32 head=__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    a.post=!l->service_closing && r->phase==LREQ_TERMINAL && (u32)(l->cq_tail-head)<l->cq_entries &&
           loom_request_deliver_room(&l->service_requests);
    a.collect=p->locally_retired && !p->op_count && !p->users && !p->driving &&
              r->phase==LREQ_DELIVERED && !r->borrows && e->pins==own_pins;
    if(p->driving || p->users)return a;
    a.handshake=engine_handshake_locked(l,p);
    if(!a.handshake) {
        struct loom_ready_interest ready=loom_ready_interest_locked(p,now);
        a.step=ready.work;a.events=ready.events;a.deadline=ready.deadline;
        a.retire=engine_retirable_locked(l,p);return a;
    }
    if(p->locally_retired)return a;
    struct loom_service_node *n=&l->service_nodes->nodes[p->scope.slot];
    if(l->service_closing || p->abort_reason || n->state==LOOM_SERVICE_ABORTING) {
        a.step=true;return a;
    }
    if(r->phase!=LREQ_ACTIVE)return a;
    if(p->connect_deadline && p->connect_deadline<=now){a.step=true;return a;}
    a.deadline=p->connect_deadline;
    switch(p->progress.handshake.phase) {
    case P9_HS_VERSION_SEND:case P9_HS_ATTACH_SEND:a.events=POLLOUT;break;
    case P9_HS_VERSION_RECV:case P9_HS_ATTACH_RECV:a.events=POLLIN;break;
    default:a.step=true;break;
    }
    return a;
}

static int engine_ready_visit(struct Loom *l, u64 now) {
    if (!l || !loom_is_private(l)) return -T_E_INVAL;
    for (u32 scanned=0;scanned<=LOOM_SERVICE_SLOT_COUNT;++scanned) {
        spin_lock(&l->lock);
        u32 at=l->service_engine->cursor;
        l->service_engine->cursor=(at+1u)%(LOOM_SERVICE_SLOT_COUNT+1u);
        // The precharged refusal is one round-robin participant, not priority
        // work that could starve every scope under a stream of invalid SQEs.
        if(at==LOOM_SERVICE_SLOT_COUNT) {
            spin_unlock(&l->lock);if(loom_frontend_step(l))return 1;continue;
        }
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
        bool native=a.events && srvconn_poll(p->admission.conn,true,a.events,NULL);
        int progress=0;
        // CONNECT's retained terminal has its own delivery turn. Do not post
        // it plus an operation completion in a single general-driver quantum.
        if(a.post)progress=loom_private_post_terminal(l,p->request)?0:1;
        else if(a.step || native) {
            if(a.handshake) {
                (void)loom_private_connect_step(p,now);
                (void)loom_private_post_terminal(l,p->request);
                progress=1;
            } else progress=loom_ready_step(p,now,native);
        }
        // A concurrent close/wait sweep may have parked on this temporary
        // pin. Observe that dependency in the SAME Ll interval as its return;
        // sampling before or after an independent put can miss the last wake.
        spin_lock(&l->lock);
        if (e->phase!=LENGINE_LIVE || e->protocol!=p || !e->pins)
            extinction("ready visit lost entry pin");
        bool retired=engine_retire_locked(l,p);
        --e->pins;
        bool dependent=l->service_closing || l->service_engine->wait_active;
        spin_unlock(&l->lock);
        if (dependent) engine_changed(l);
        int collected=engine_collect(l,ref,false);
        if(progress<0)extinction("engine action refused valid ownership");
        if (progress || retired || !collected) {
            engine_changed(l);
            return 1;
        }
    }
    return 0;
}

int loom_engine_ready_visit(struct Loom *l,u64 now) {
    if(!l || !loom_is_private(l))return -T_E_INVAL;
    // This root is independent of every operation the visit may consume.
    loom_ref(l);int rc=engine_ready_visit(l,now);loom_unref(l);return rc;
}
static int engine_close_visit(struct Loom *l) {
    struct loom_service_protocol *p=NULL;struct loom_service_ref ref={0};bool handshake=false;
    spin_lock(&l->lock);
    if(!l->service_closing){spin_unlock(&l->lock);return -T_E_INVAL;}
    if(l->service_engine->submitting){spin_unlock(&l->lock);return 0;}
    // No caller can claim after close. Its copied refusal owns no local borrows;
    // operation owners still drain below before global consumer detach.
    l->service_engine->rejected=false;
    l->service_engine->rejected_user_data=0;l->service_engine->rejected_result=0;
    for(u32 n=0;n<LOOM_SERVICE_SLOT_COUNT;++n) {
        u32 at=l->service_engine->cursor%LOOM_SERVICE_SLOT_COUNT;l->service_engine->cursor=(at+1u)%LOOM_SERVICE_SLOT_COUNT;
        struct loom_engine_entry *e=&l->service_engine->entries[at];
        if(e->phase!=LENGINE_LIVE || e->pins==UINT32_MAX)continue;
        struct engine_interest a=engine_interest_locked(l,e,timer_now_ns(),0);
        if(!a.step && !a.retire)continue;
        ++e->pins;p=e->protocol;ref=p->scope;handshake=a.handshake;break;
    }
    spin_unlock(&l->lock);
    int progress=0;
    if(p) {
        if(handshake)(void)loom_private_connect_step_retiring(p);
        else progress=loom_ready_step(p,timer_now_ns(),false);
        spin_lock(&l->lock);(void)engine_retire_locked(l,p);spin_unlock(&l->lock);
        if(engine_put(l,ref,p,false))extinction("close driver lost entry pin");
        if(progress<0)extinction("engine action refused valid ownership");
    }
    if(!engine_discard_closed(l,false)) {
        spin_lock(&l->lock);
        bool pending=l->service_engine->worker_starting ||
            (l->sqpoll && !__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE));
        spin_unlock(&l->lock);return pending?0:2;
    }
    spin_lock(&l->lock);bool more=false;
    for(u32 i=0;i<LOOM_SERVICE_SLOT_COUNT;++i) {
        struct loom_engine_entry *e=&l->service_engine->entries[i];
        if(e->phase!=LENGINE_LIVE || e->pins==UINT32_MAX)continue;
        struct engine_interest a=engine_interest_locked(l,e,timer_now_ns(),0);
        if(a.step || a.retire)more=true;
    }
    spin_unlock(&l->lock);return more?1:0;
}
int loom_engine_close_visit(struct Loom *l) {
    if(!l || !loom_is_private(l))return -T_E_INVAL;
    loom_ref(l);int rc=engine_close_visit(l);loom_unref(l);return rc;
}

int loom_engine_wait_arm(struct Loom *l, u64 now, u64 *deadline) {
    if (!l || !loom_is_private(l) || !deadline) return -T_E_INVAL;
    struct loom_service_engine *t=l->service_engine;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    spin_lock(&l->lock);
    if (t->wait_active) { spin_unlock(&l->lock); return -T_E_BUSY; }
    t->wait_active=true;
    t->wait_mask=0;
    t->wait_work=l->service_closing || (t->rejected &&
        (u32)(l->cq_tail-__atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE))<l->cq_entries);
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
        if (a.step || a.post || a.collect || a.retire) t->wait_work=true;
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
