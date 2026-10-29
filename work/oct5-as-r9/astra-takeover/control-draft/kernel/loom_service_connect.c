// CONNECT owns a charged protocol object plus precharged terminal obligation.
// Ll serializes exact scope identity, admission and terminal success with abort.
// Native publication occurs inside that interval; protocol work, teardown,
// waiter wakes, allocation and final drops occur outside it. The sole driver
// holds a request borrow while running the client. RETIRED requires actual
// cursor abort and no concurrent users, never merely an acknowledged abort.
//
// Internal executor primitive only. The caller supplies entry/storage lifetime
// and prevents general RPCs escaping this handshake owner. The indexed close
// queue uses quiet retirement variants; readiness scheduling, ring dispatch
// and user-facing activation remain separate integration.
#include <thylacine/loom_service_connect.h>
#include <thylacine/loom.h>
#include <thylacine/srvconn.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>

static bool connect_same(struct loom_service_ref a, struct loom_service_ref b) {
    return a.slot == b.slot && a.incarnation == b.incarnation && a.reserved == b.reserved;
}
static struct loom_service_node *connect_scope(struct Loom *l, struct loom_service_ref ref) {
    if (ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT) return NULL;
    struct loom_service_slot *s = &l->service_slots->slots[ref.slot];
    return s->kind == LOOM_SERVICE_SCOPE && s->phase == LSLOT_LIVE &&
           s->incarnation == ref.incarnation ? &l->service_nodes->nodes[ref.slot] : NULL;
}
static struct loom_service_request *connect_request(struct loom_service_protocol *p) {
    struct loom_request_table *t = &p->owner->service_requests;
    if (p->request.reserved || p->request.index >= t->capacity) return NULL;
    struct loom_service_request *r = &t->records[p->request.index];
    return r->phase != LREQ_FREE && r->identity == p->request.identity &&
           connect_same(r->scope,p->scope) ? r : NULL;
}
static void connect_rollback_locked(struct Loom *l, struct loom_service_ref scope,
                                    struct loom_request_ref request) {
    if (loom_request_rollback(&l->service_requests,l->service_slots,request) ||
        loom_slot_unpin(l->service_slots,scope))
        extinction("CONNECT lost prepared ownership");
    struct loom_service_node *n=connect_scope(l,scope);
    if (n && n->state==LOOM_SERVICE_ABORTING && !l->service_slots->slots[scope.slot].pins) {
        n->state=LOOM_SERVICE_RETIRED;
        n->status_flags|=LOOM_SERVICE_LOCAL_RETIRED;
    }
}
int loom_private_connect_prepare(struct Loom *l, struct loom_service_ref scope,
    struct loom_service_ref target, u64 user_data, u64 deadline, u64 now,
    struct loom_service_protocol **out) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l) || !out) return -T_E_INVAL;
    struct loom_request_ref request;
    spin_lock(&l->lock);
    struct loom_service_node *n = connect_scope(l,scope);
    int rc = l->service_closing ? -T_E_CANCELED : !n ? -T_E_NOENT :
             n->state != LOOM_SERVICE_RESERVED ? -T_E_BUSY : 0;
    // The first PREPARED request claims a RESERVED destination across allocation.
    // Other operation types are not admitted to a RESERVED scope by the engine.
    for (u32 i=0; !rc && i<l->service_requests.capacity; ++i) {
        struct loom_service_request *r = &l->service_requests.records[i];
        if (r->phase != LREQ_FREE && connect_same(r->scope,scope)) rc = -T_E_BUSY;
    }
    if (!rc) rc = loom_request_prepare(&l->service_requests,l->service_slots,
                                        l->service_nodes,scope,user_data,deadline,now,&request);
    if (!rc) {
        rc = loom_slot_pin(l->service_slots,scope,LOOM_SERVICE_SCOPE);
        if (rc) (void)loom_request_rollback(&l->service_requests,l->service_slots,request);
    }
    spin_unlock(&l->lock);
    if (rc) return rc;
    struct loom_service_protocol *p;
    rc = loom_private_protocol_prepare(l,target,deadline,now,&p);
    if (rc) {
        spin_lock(&l->lock);
        connect_rollback_locked(l,scope,request);
        spin_unlock(&l->lock);
        loom_private_close_changed(l);
        return rc;
    }
    p->scope = scope; p->request = request; p->connect_deadline = deadline;
    p->connect_bound = true;
    *out = p;
    return 0;
}
static int connect_rollback_consume(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_request *r = connect_request(p);
    if (!p->connect_bound || p->indexed || p->admission.published || p->users || p->driving ||
        !r || r->phase != LREQ_PREPARED || r->borrows) {
        spin_unlock(&l->lock); return -T_E_BUSY;
    }
    struct loom_service_ref scope=p->scope;
    struct loom_request_ref request=p->request;
    p->connect_bound = false;
    spin_unlock(&l->lock);
    // Retain the PREPARED obligation and scope pins while real cursor/storage
    // destruction runs unlocked. A closing worker must still see this owner.
    // The public wrapper retains l across the consuming final protocol drop.
    if (loom_private_protocol_release_unused(p)) extinction("CONNECT lost unused protocol ownership");
    spin_lock(&l->lock);
    connect_rollback_locked(l,scope,request);
    spin_unlock(&l->lock);
    return 0;
}
int loom_private_connect_rollback(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l=p->owner;
    loom_ref(l);
    int rc=connect_rollback_consume(p);
    if (!rc) loom_private_close_changed(l);
    loom_unref(l);
    return rc;
}
int loom_private_connect_publish(struct loom_service_protocol *p, u64 now) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n = connect_scope(l,p->scope);
    int rc = !p->connect_bound || p->admission.published || p->users || p->driving ?
        -T_E_BUSY : p->abort_reason ? p->abort_reason : l->service_closing ? -T_E_CANCELED :
        n && n->state==LOOM_SERVICE_ABORTING && n->reason<0 ? n->reason :
        !n || n->state != LOOM_SERVICE_RESERVED ? -T_E_NOENT : 0;
    if (!rc) rc = loom_request_check_admit(&l->service_requests,l->service_slots,
                                           l->service_nodes,p->request,now);
    if (!rc) rc = devsrv_connect_publish(&p->admission);
    if (!rc) {
        // No request/node predicate changed under Ll. Native publication does
        // not call back into Loom, allocate or wait. Admission is infallible.
        if (loom_request_admit(&l->service_requests,l->service_slots,
                                 l->service_nodes,p->request,now))
            extinction("CONNECT publication lost terminal reservation");
        n->state = LOOM_SERVICE_ADMITTED;
    }
    spin_unlock(&l->lock);
    if (!rc) devsrv_connect_wake(&p->admission);
    return rc;
}
static void connect_latch(struct loom_service_protocol *p, struct loom_service_node *n,
                           s32 reason) {
    if (!p->abort_reason) p->abort_reason = n->reason<0 ? n->reason : reason;
    if (!n->reason) n->reason=p->abort_reason;
    if (n->state != LOOM_SERVICE_RETIRED) n->state = LOOM_SERVICE_ABORTING;
}
int loom_private_connect_abort(struct loom_service_protocol *p, s32 reason) {
    if (!p || !p->owner || reason >= 0) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n = connect_scope(l,p->scope);
    if (!p->connect_bound || !n) {
        spin_unlock(&l->lock); return -T_E_NOENT;
    }
    if (p->locally_retired || p->abort_reason) { spin_unlock(&l->lock); return 1; }
    if (p->users == UINT32_MAX) { spin_unlock(&l->lock); return -T_E_BUSY; }
    ++p->users;
    connect_latch(p,n,reason);
    spin_unlock(&l->lock);
#ifdef KERNEL_TESTS
    loom_private_close_boundary_for_test(l,3); // teardown user owns storage
#endif
    srvconn_teardown(p->admission.conn);
    spin_lock(&l->lock);
    --p->users;
    spin_unlock(&l->lock);
    loom_private_close_changed(l);
    return 0;
}
static int connect_step(struct loom_service_protocol *p, u64 now, bool notify) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n = connect_scope(l,p->scope);
    struct loom_service_request *r = connect_request(p);
    if (n && n->state==LOOM_SERVICE_ABORTING && n->reason<0) connect_latch(p,n,n->reason);
    if (!p->connect_bound || (!p->admission.published && !p->abort_reason) || !n || !r) {
        spin_unlock(&l->lock); return -T_E_NOENT;
    }
    if (p->driving) { spin_unlock(&l->lock); return -T_E_BUSY; }
    if (p->locally_retired) { s32 why=p->abort_reason; spin_unlock(&l->lock); return why; }
    if (l->service_closing) connect_latch(p,n,-T_E_CANCELED);
    // CONNECT deadline ends once its success is committed; READY is not a
    // permanent lease with the old handshake deadline attached to it.
    if (r->phase == LREQ_ACTIVE && p->connect_deadline && p->connect_deadline <= now)
        connect_latch(p,n,-T_E_TIMEDOUT);
    if (!p->abort_reason && n->state == LOOM_SERVICE_READY) {
        spin_unlock(&l->lock); return 1;
    }
    bool borrowed = false;
    if (!p->abort_reason) {
        int rc=loom_request_borrow(&l->service_requests,l->service_slots,l->service_nodes,p->request);
        if (rc) { spin_unlock(&l->lock); return rc; }
        borrowed=true;
    }
    p->driving=true;
    bool aborting=p->abort_reason != 0;
    u32 attempted_phase=p->progress.handshake.phase;
    spin_unlock(&l->lock);
#ifdef KERNEL_TESTS
    if (notify) loom_private_close_boundary_for_test(l,2); // sole external driver
#endif
    int rc=aborting ? 0 : p9_client_progress_step(&p->client,now);
    spin_lock(&l->lock);
    // Sole driver has returned: only now read its unlocked progress cursor.
    // Query conservatively reports possible bytes while driving is true.
    if (p->progress.io.bytes_sent) n->status_flags|=LOOM_SERVICE_BYTES_SENT;
    if (n->state==LOOM_SERVICE_ABORTING && n->reason<0) connect_latch(p,n,n->reason);
    if (borrowed && loom_request_put(&l->service_requests,p->request))
        extinction("CONNECT lost parser borrow");
    if (l->service_closing) connect_latch(p,n,-T_E_CANCELED);
    if (rc < 0) {
        p->failure_phase=attempted_phase; n->failure_phase=attempted_phase;
        connect_latch(p,n,rc == -T_E_TIMEDOUT ? rc : -T_E_IO);
    }
    if (!p->abort_reason) {
        u32 phase=p->progress.handshake.phase;
        if (phase == P9_HS_READY) {
            if (loom_request_finish(&l->service_requests,l->service_slots,
                                       l->service_nodes,p->request,(s32)p->scope.slot))
                extinction("CONNECT lost success obligation");
            n->state=LOOM_SERVICE_READY;
            rc=1;
        } else {
            n->state=phase <= P9_HS_VERSION_RECV ? LOOM_SERVICE_VERSION : LOOM_SERVICE_ATTACH;
            rc=0;
        }
        p->driving=false;
        spin_unlock(&l->lock);
        if (notify) loom_private_close_changed(l);
        return rc;
    }
    s32 reason=p->abort_reason;
    spin_unlock(&l->lock);
    // Sole driver, no escaped RPCs: abort returns with local RX/TX cursors and
    // callbacks detached. Teardown alone could not establish this fact.
    srvconn_teardown(p->admission.conn);
    p9_client_progress_abort(&p->client);
    spin_lock(&l->lock);
    if (r->phase == LREQ_PREPARED &&
        loom_request_reject(&l->service_requests,l->service_slots,p->request,reason))
        extinction("CONNECT lost rejected admission obligation");
    if (r->phase == LREQ_ACTIVE &&
        loom_request_finish(&l->service_requests,l->service_slots,l->service_nodes,p->request,reason))
        extinction("CONNECT lost cancellation obligation");
    // A concurrent abort caller may still hold the transport during teardown.
    // Keep ABORTING until a later visit observes it gone; never free under it.
    if (!p->users) {
        p->locally_retired=true; n->state=LOOM_SERVICE_RETIRED;
        n->status_flags|=LOOM_SERVICE_LOCAL_RETIRED;
    }
    p->driving=false;
    spin_unlock(&l->lock);
    if (notify) loom_private_close_changed(l);
    return reason;
}
int loom_private_connect_step(struct loom_service_protocol *p, u64 now) {
    return connect_step(p,now,true);
}
int loom_private_connect_step_retiring(struct loom_service_protocol *p) {
    return connect_step(p,0,false);
}
int loom_private_connect_release_retiring(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l=p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n=connect_scope(l,p->scope);
    struct loom_service_request *r=connect_request(p);
    if (!p->connect_bound || p->indexed || !p->locally_retired || p->driving || p->users ||
        !n || n->state != LOOM_SERVICE_RETIRED || !r || r->phase != LREQ_DELIVERED || r->borrows) {
        spin_unlock(&l->lock); return -T_E_BUSY;
    }
    if (loom_request_reap(&l->service_requests,l->service_slots,p->request) ||
        loom_slot_unpin(l->service_slots,p->scope))
        extinction("CONNECT lost retired ownership");
    p->connect_bound=false;
    spin_unlock(&l->lock);
    return loom_private_protocol_release_retired(p);
}
int loom_private_connect_release(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l=p->owner;
    loom_ref(l);
    int rc=loom_private_connect_release_retiring(p);
    if (!rc) loom_private_close_changed(l);
    loom_unref(l);
    return rc;
}
int loom_private_connect_reap(struct Loom *l, struct loom_service_ref scope) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct loom_service_node *n=connect_scope(l,scope);
    int rc=!n ? -T_E_NOENT : n->state != LOOM_SERVICE_RETIRED ||
        l->service_slots->slots[scope.slot].pins ? -T_E_BUSY : 0;
    if (!rc) {
        *n=(struct loom_service_node){0};
        if (loom_slot_retire(l->service_slots,scope) || loom_slot_reap(l->service_slots,scope))
            extinction("CONNECT lost retired scope");
    }
    spin_unlock(&l->lock);
    return rc;
}
