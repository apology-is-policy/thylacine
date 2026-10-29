
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held,wakes;
static void boundary_unlocked(void);
static void spin_lock(spin_lock_t *l) {CHECK(!l->held && !held,"lock reentry");l->held=1;held++;}
static void spin_unlock(spin_lock_t *l) {CHECK(l->held && held==1,"lock release");l->held=0;held--;boundary_unlocked();}
struct loom_ring_hdr {u32 cq_head,cq_tail,cq_mask;};
struct loom_cqe {u64 user_data;s32 result;u32 flags;};

// Internal bounded owner index. No private syscall/worker activation.
#ifndef THYLACINE_LOOM_SERVICE_ENGINE_H
#define THYLACINE_LOOM_SERVICE_ENGINE_H
#include <thylacine/loom_service_abi.h>
struct Loom;
struct loom_service_protocol;
enum loom_engine_phase { LENGINE_EMPTY, LENGINE_INSTALLING, LENGINE_LIVE };
struct loom_engine_entry {
    struct loom_service_protocol *protocol;
    u64 incarnation;
    u32 pins, phase;
};
struct loom_service_engine {
    struct loom_engine_entry entries[LOOM_SERVICE_SLOT_COUNT];
    u32 count, cursor;
};
// A separately charged buddy page in a private Loom, zeroed at creation. All fields below are serialized by the SAME Loom lock.
// Caller retains l for every call, including final collection. Adopt transfers
// p only on return 0. Once indexed, even failed publication retains a terminal
// obligation; its negative result is driven/posted through the ordinary path.
// Install owns a pin through native publication AND unlocked accept wake.
int loom_engine_adopt(struct loom_service_protocol *, u64 now);
// Get/put retain protocol storage, not authority to issue general RPCs. A get
// against installing storage returns AGAIN. Abort handles that state internally.
// No pointer may outlive its put; exactly one put per successful get.
int loom_engine_get(struct Loom *, struct loom_service_ref, struct loom_service_protocol **);
int loom_engine_put(struct Loom *, struct loom_service_ref, struct loom_service_protocol *);
int loom_engine_abort(struct Loom *, struct loom_service_ref, s32 reason);
// One round-robin scope visit (one bounded client step, at most one CQ post).
// Returns 1 if a scope was visited, 0 if none eligible. Not a sleep/readiness
// predicate: the future executor must register native poll interest/recheck.
int loom_engine_visit(struct Loom *, u64 now);
// Removes only locally retired owners with delivered/discarded terminals and
// zero entry pins. Final drops outside Ll. Leaves scope metadata for REAP.
int loom_engine_collect(struct Loom *, struct loom_service_ref);
// Whole-ring-close consumer detach requires every indexed protocol locally
// retired, no entry/install/parser/fixed borrows. Then discard retained terminals
// and collect their storage, independent of CQ room. No native peer wait.
int loom_engine_discard_closed(struct Loom *);
// One close quantum: 0 blocked on external local holders, 1 immediate work,
// 2 complete (index/request/fixed ownership empty after consumer detach).
// Retirer owns a queue root; its internal puts never kick its own RUNNING work.
int loom_engine_close_visit(struct Loom *);
bool loom_engine_empty(const struct loom_service_engine *);
#endif

struct Loom {u64 service_creator;struct loom_service_engine *service_engine;u32 service_fixed_count,service_fixed_settling;int sqpoll_park;struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;u32 magic;spin_lock_t lock;void *service_as;bool service_closing;
 struct loom_request_table service_requests;u8 *ring_kva;u32 hdr_off,cqe_off,receipt_off,receipt_size,cq_tail,cq_entries;int cq_waiters;};
static bool loom_is_private(struct Loom *l) {return l && l->service_as;}
static void poll_waiter_list_wake(int *p) {(void)p;CHECK(!held,"wake outside owner lock");wakes++;}

enum {P9_HS_VERSION_SEND,P9_HS_VERSION_RECV,P9_HS_ATTACH_SEND,P9_HS_ATTACH_RECV,P9_HS_READY};
struct SrvConn {bool dead;};
struct SrvConnectAdmission {struct SrvConn *conn;bool published;};
struct p9_client {int dummy;};
struct loom_service_protocol {
 struct Loom *owner;struct SrvConnectAdmission admission;struct p9_client client;
 struct {struct {u32 phase;} handshake;struct {bool bytes_sent;} io;} progress;
 struct loom_service_ref scope;struct loom_request_ref request;u64 connect_deadline;
 s32 abort_reason;u32 users,failure_phase;bool connect_bound,driving,locally_retired,indexed;
};
static void progress_boundary(struct loom_service_protocol *);
static void destruction_boundary(struct loom_service_protocol *);
static void installation_boundary(struct loom_service_protocol *);
static u32 expected_child_pins;
static struct loom_service_protocol *active, *externally_pinned;
static struct SrvConn cn;
static int mode,allocations,releases,steps,aborts,teardowns,publishes,accept_wakes;
static void extinction(const char *s) {CHECK(false,s);}
int loom_private_connect_step(struct loom_service_protocol *,u64);
int loom_private_connect_abort(struct loom_service_protocol *,s32);
int loom_private_connect_release(struct loom_service_protocol *);
static void close_owner(struct Loom *l) {l->service_closing=true;l->service_slots->closing=true;}
static int loom_private_protocol_prepare(struct Loom *l,struct loom_service_ref target,u64 deadline,u64 now,struct loom_service_protocol **out) {
 (void)target;(void)deadline;(void)now;
 CHECK(!held && l->service_requests.occupied,"request reservation before protocol allocation");
 allocations++;active=calloc(1,sizeof(*active));CHECK(active,"host allocation");
 active->owner=l;active->admission.conn=&cn;*out=active;return 0;
}
static int loom_private_protocol_release_unused(struct loom_service_protocol *p) {
 CHECK(!held && !p->indexed && !p->connect_bound && !p->admission.published,"unpublished owner exclusive cleanup");
 destruction_boundary(p);
 releases++;free(p);active=NULL;return 0;
}
static int loom_private_protocol_release_retired(struct loom_service_protocol *p) {
 CHECK(p!=externally_pinned,"external entry prevents storage release");
 CHECK(!held && p->locally_retired && !p->indexed && !p->connect_bound && !p->users && !p->driving &&
       p->owner->service_slots->slots[p->scope.slot].pins==expected_child_pins,"indexed lifetime ends after pins and terminal");
 releases++;free(p);active=NULL;return 0;
}
static int devsrv_connect_publish(struct SrvConnectAdmission *a) {
 CHECK(held && (active->indexed || mode==101),"native publication has indexed lifetime");
 struct loom_engine_entry *e=&active->owner->service_engine->entries[active->scope.slot];
 CHECK(mode==101 || (e->phase==LENGINE_INSTALLING && e->pins==1),"installation pin covers native publication");
 if(mode==3)return -T_E_NOENT;
 a->published=true;publishes++;return 0;
}
static void devsrv_connect_wake(struct SrvConnectAdmission *a) {
 CHECK(!held && a->published,"native accept wake outside lock");accept_wakes++;
 if(mode==101)return;
 struct Loom *l=active->owner;struct loom_service_protocol *p=NULL;
 CHECK(loom_engine_get(l,active->scope,&p)==-T_E_AGAIN && !p &&
       loom_engine_collect(l,active->scope)==-T_E_BUSY,"accept wake retains installing lifetime");
 if(mode==2)CHECK(!loom_engine_abort(l,active->scope,-T_E_CANCELED),"abort during accept wake");
 if(mode==5)close_owner(l);
}
static void srvconn_teardown(struct SrvConn *conn) {
 CHECK(!held && conn==&cn,"teardown outside lock");conn->dead=true;teardowns++;
}
static int p9_client_progress_step(struct p9_client *client,u64 now) {
 (void)now;active=(void *)((u8 *)client-__builtin_offsetof(struct loom_service_protocol,client));
 CHECK(!held && active->driving && active->owner->service_requests.records[active->request.index].borrows==1,
       "entry owns parser borrow during client step");
 steps++;active->progress.io.bytes_sent=true;
 progress_boundary(active);
 if(mode==6 && active->scope.slot==0)return 0;
 if(mode==104)return -T_E_INVAL;
 ++active->client.dummy;
 active->progress.handshake.phase=mode!=101 && active->client.dummy==1?P9_HS_ATTACH_RECV:P9_HS_READY;
 return 1;
}
static void p9_client_progress_abort(struct p9_client *client) {
 active=(void *)((u8 *)client-__builtin_offsetof(struct loom_service_protocol,client));
 CHECK(!held && active->driving && !active->owner->service_requests.records[active->request.index].borrows,"cursor abort outside lock after parser borrow");aborts++;
}





static int hook_armed;
static void boundary_unlocked(void) {
 if(!hook_armed || !active || !active->indexed)return;
 struct loom_engine_entry *e=&active->owner->service_engine->entries[active->scope.slot];
 if(e->phase!=LENGINE_INSTALLING)return;
 hook_armed=0;
 if(mode==105)installation_boundary(active);
 if(mode==1)CHECK(!loom_engine_abort(active->owner,active->scope,-T_E_CANCELED),"abort before publication");
 if(mode==7)close_owner(active->owner);
}
static void wakeup(int *r) {(void)r;CHECK(!held,"owner change wakes outside lock");}
int loom_private_post_terminal(struct Loom *,struct loom_request_ref);

static int close_kicks,temporary_roots;
static void loom_private_close_changed(struct Loom *l) {(void)l;CHECK(!held,"close notification outside Ll");close_kicks++;}
static void loom_ref(struct Loom *l) {(void)l;temporary_roots++;}
static void loom_unref(struct Loom *l) {(void)l;CHECK(temporary_roots>0,"temporary root balanced");temporary_roots--;}

#include <string.h>
#define PROC_MAGIC 456
struct Proc {u32 magic;void *as;u64 stripes;};
static struct Proc creator;
static int dispatch_kind;
static int loom_private_target_reap(struct Loom *l,struct loom_service_ref r) {(void)l;(void)r;CHECK(!held,"target drop outside Ll");dispatch_kind=LOOM_SERVICE_TARGET;return 0;}
static int loom_private_pool_reap(struct Loom *l,struct loom_service_ref r) {(void)l;(void)r;CHECK(!held,"pool drop outside Ll");dispatch_kind=LOOM_SERVICE_POOL;return 0;}
int loom_private_node_reap_unused(struct Loom *l, struct loom_service_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    int rc = loom_service_node_reap_unused(l->service_slots, l->service_nodes, ref);
    spin_unlock(&l->lock);
    return rc;
}// CONNECT owns a charged protocol object plus precharged terminal obligation.
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
// Bounded charged ring index supplies the entry lifetime required by CONNECT.
// Ll protects lookup+pin, installation, detach and removal. No allocation,
// protocol step, native wake, teardown or final drop is performed under Ll.

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
int loom_private_post_terminal(struct Loom *l, struct loom_request_ref ref) {
    if (!l || l->magic != LOOM_MAGIC || !loom_is_private(l)) return -T_E_INVAL;
    spin_lock(&l->lock);
    struct loom_request_table *t = &l->service_requests;
    u64 user_data; s32 result;
    int rc = loom_request_peek(t,ref,&user_data,&result);
    if (rc) { spin_unlock(&l->lock); return rc; }
    if (t->detached) { spin_unlock(&l->lock); return -T_E_CANCELED; }
    struct loom_ring_hdr *h = (void *)(l->ring_kva+l->hdr_off);
    u32 tail = l->cq_tail;
    u32 head = __atomic_load_n(&h->cq_head,__ATOMIC_ACQUIRE);
    if ((u32)(tail-head) >= l->cq_entries) {
        spin_unlock(&l->lock);
        return -T_E_AGAIN;
    }
    u32 idx = tail & (l->cq_entries-1u);
    struct loom_cqe *cq = (void *)(l->ring_kva+l->cqe_off);
    cq[idx] = (struct loom_cqe){ .user_data=user_data, .result=result };
    if (l->receipt_size) {
        struct loom_service_buffer_receipt *receipts = (void *)(l->ring_kva+l->receipt_off);
        receipts[idx] = (struct loom_service_buffer_receipt){0};
    }
    // No abort check here: a terminal committed before abort stays truthful.
    // Copy, mark delivered, and publish the tail under one lock interval.
    rc = loom_request_deliver(t,ref);
    if (rc) { spin_unlock(&l->lock); return rc; }
    l->cq_tail = tail+1u;
    __atomic_store_n(&h->cq_tail,l->cq_tail,__ATOMIC_RELEASE);
    spin_unlock(&l->lock);
    poll_waiter_list_wake(&l->cq_waiters);
    return 0;
}// Exact identity, authority and diagnostic snapshots share Ll with admission,
// cancellation and collection. No usercopy, native teardown or final drop under
// Ll. Controls remain independent of SQ/CQ capacity and do not activate a worker.

static int control_owner(struct Loom *l,struct Proc *p) {
    if(!l || l->magic!=LOOM_MAGIC || !p || p->magic!=PROC_MAGIC) return -T_E_INVAL;
    if(!loom_is_private(l)) return -T_E_OPNOTSUPP;
    if(p->as!=l->service_as || p->stripes!=l->service_creator) return -T_E_ACCES;
    return 0;
}
static bool control_zero_ref(struct loom_service_ref r) {
    return !r.slot && !r.reserved && !r.incarnation;
}
static bool control_same(struct loom_service_ref a,struct loom_service_ref b) {
    return a.slot==b.slot && a.incarnation==b.incarnation && a.reserved==b.reserved;
}
static struct loom_service_slot *control_exact(struct Loom *l,struct loom_service_ref ref) {
    if(ref.reserved || !ref.incarnation || ref.slot>=LOOM_SERVICE_SLOT_COUNT)return NULL;
    struct loom_service_slot *s=&l->service_slots->slots[ref.slot];
    return s->phase==LSLOT_LIVE && s->incarnation==ref.incarnation ? s : NULL;
}
static int control_input(struct Loom *l,struct Proc *p,const struct loom_service_control *in) {
    int rc=control_owner(l,p);if(rc)return rc;
    if(!in || in->size!=sizeof(*in) || in->version!=LOOM_SERVICE_ABI_VERSION ||
       in->flags || in->reserved || in->object.reserved)return -T_E_INVAL;
    return 0;
}
int loom_private_service_query(struct Loom *l,struct Proc *p,
        const struct loom_service_snapshot *in,struct loom_service_snapshot *out) {
    int rc=control_owner(l,p);if(rc)return rc;
    if(!in || !out || in->size!=sizeof(*in) || in->version!=LOOM_SERVICE_ABI_VERSION ||
       in->flags || in->object.reserved || in->kind || in->state || !control_zero_ref(in->scope) ||
       in->active_ops || in->pending_terminals || in->reason || in->status_flags)return -T_E_INVAL;
    struct loom_service_snapshot result={.size=sizeof(result),.version=LOOM_SERVICE_ABI_VERSION,.object=in->object};
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)rc=-T_E_NOENT;
    else if(s->kind==LOOM_SERVICE_TARGET) {result.kind=s->kind;result.state=LOOM_SERVICE_READY;}
    else if(s->kind==LOOM_SERVICE_SCOPE || s->kind==LOOM_SERVICE_FID) {
        struct loom_service_node *n=&l->service_nodes->nodes[in->object.slot];
        result.kind=s->kind;result.state=n->state;
        result.scope=s->kind==LOOM_SERVICE_SCOPE ? in->object : n->parent;
        result.reason=n->reason;result.status_flags=n->status_flags;
        if(s->kind==LOOM_SERVICE_SCOPE) {
            struct loom_engine_entry *e=&l->service_engine->entries[in->object.slot];
            // Progress is written outside Ll. A live driver may have sent bytes
            // before reacquiring Ll; never race a raw cursor read here.
            if(e->phase!=LENGINE_EMPTY && e->incarnation==in->object.incarnation &&
               e->protocol->driving)result.status_flags|=LOOM_SERVICE_BYTES_SENT;
            for(u32 i=0;i<l->service_requests.capacity;++i) {
                struct loom_service_request *r=&l->service_requests.records[i];
                if(r->phase==LREQ_FREE || !control_same(r->scope,in->object))continue;
                if(r->phase==LREQ_PREPARED || r->phase==LREQ_ACTIVE)++result.active_ops;
                if(r->phase==LREQ_TERMINAL)++result.pending_terminals;
                if(r->borrows)result.status_flags|=LOOM_SERVICE_BYTES_SENT;
            }
        }
    } else rc=-T_E_INVAL; // POOL uses its dedicated snapshot per buffers ABI.
    if(!rc)*out=result;
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_service_abort(struct Loom *l,struct Proc *p,const struct loom_service_control *in) {
    int rc=control_input(l,p,in);if(rc)return rc;
    bool indexed=false;
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)rc=-T_E_NOENT;
    else if(s->kind!=LOOM_SERVICE_SCOPE)rc=-T_E_INVAL;
    else {
        struct loom_service_node *n=&l->service_nodes->nodes[in->object.slot];
        if(n->state==LOOM_SERVICE_ABORTING || n->state==LOOM_SERVICE_RETIRED)rc=1;
        else {
            struct loom_engine_entry *e=&l->service_engine->entries[in->object.slot];
            indexed=e->phase!=LENGINE_EMPTY && e->incarnation==in->object.incarnation;
            if(!indexed) {
                if(!n->reason)n->reason=-T_E_CANCELED;
                n->state=LOOM_SERVICE_ABORTING;
                // A constructing raw owner already holds scope/request pins.
                // Its publish/progress must observe this latch, and rollback
                // alone may settle unused abort once those pins are gone.
                if(!s->pins) {n->state=LOOM_SERVICE_RETIRED;n->status_flags|=LOOM_SERVICE_LOCAL_RETIRED;}
            }
        }
    }
    spin_unlock(&l->lock);
    if(rc || !indexed)return rc;
    rc=loom_engine_abort(l,in->object,-T_E_CANCELED); // owns entry through teardown
    if(rc!=-T_E_NOENT)return rc;
    // A concurrent driver may retire+collect before index pinning. Preserve
    // idempotence for the same unreaped identity, never for a reused slot.
    spin_lock(&l->lock);s=control_exact(l,in->object);
    if(s && s->kind==LOOM_SERVICE_SCOPE &&
       l->service_nodes->nodes[in->object.slot].state>=LOOM_SERVICE_ABORTING)rc=1;
    spin_unlock(&l->lock);
    return rc;
}
int loom_private_service_reap(struct Loom *l,struct Proc *p,const struct loom_service_control *in) {
    int rc=control_input(l,p,in);if(rc)return rc;
    u32 kind=0,state=0;
    spin_lock(&l->lock);
    struct loom_service_slot *s=control_exact(l,in->object);
    if(!s)rc=-T_E_NOENT;
    else {kind=s->kind;if(kind==LOOM_SERVICE_SCOPE || kind==LOOM_SERVICE_FID)state=l->service_nodes->nodes[in->object.slot].state;}
    spin_unlock(&l->lock);
    if(rc)return rc;
    // Each consuming primitive rechecks the SAME exact identity and its pins
    // under Ll; no naked pointer crosses this dispatch interval. Final drops
    // occur outside Ll while the caller's retained Loom keeps its image alive.
    if(kind==LOOM_SERVICE_TARGET)return loom_private_target_reap(l,in->object);
    if(kind==LOOM_SERVICE_POOL)return l->receipt_size ? loom_private_pool_reap(l,in->object) : -T_E_OPNOTSUPP;
    if(kind==LOOM_SERVICE_SCOPE && state==LOOM_SERVICE_RETIRED)return loom_private_connect_reap(l,in->object);
    if(kind==LOOM_SERVICE_SCOPE || kind==LOOM_SERVICE_FID)return loom_private_node_reap_unused(l,in->object);
    return -T_E_INVAL;
}
static void destination(struct Loom *l,struct loom_service_ref *ref) {
 CHECK(!loom_service_node_prepare(l->service_slots,l->service_nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},ref) &&
       !loom_service_node_publish(l->service_slots,l->service_nodes,*ref),"destination control");
}
static struct loom_service_snapshot query(struct Loom *l,struct loom_service_ref ref) {
 struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=ref},out={0};
 CHECK(!loom_private_service_query(l,&creator,&in,&out),"exact query succeeds");return out;
}
static struct loom_service_control input(struct loom_service_ref ref) {
 return (struct loom_service_control){.size=sizeof(struct loom_service_control),.version=1,.object=ref};
}
static void destruction_boundary(struct loom_service_protocol *p) {
 if(mode!=103)return;
 struct loom_service_snapshot s=query(p->owner,p->scope);
 struct loom_service_control c=input(p->scope);
 CHECK(s.state==LOOM_SERVICE_ABORTING && s.active_ops==1 && !(s.status_flags&LOOM_SERVICE_LOCAL_RETIRED) &&
       p->owner->service_slots->slots[p->scope.slot].pins==2 && loom_private_service_reap(p->owner,&creator,&c)==-T_E_BUSY,
       "destruction boundary retains observable ownership");
}
static void installation_boundary(struct loom_service_protocol *p) {
 struct loom_service_snapshot s=query(p->owner,p->scope);
 CHECK(s.state==LOOM_SERVICE_RESERVED && s.active_ops==1 && !s.status_flags,"INSTALLING query stays reserved before publication");
 struct loom_service_control c=input(p->scope);
 CHECK(!loom_private_service_abort(p->owner,&creator,&c),"INSTALLING abort accepted");
}
static void progress_boundary(struct loom_service_protocol *p) {
 if(mode!=101)return;
 struct loom_service_snapshot s=query(p->owner,p->scope);
 CHECK(s.active_ops==1 && (s.status_flags&LOOM_SERVICE_BYTES_SENT),"unindexed in-flight driver reports possible bytes");
 struct loom_service_control c=input(p->scope);
 CHECK(!loom_private_service_abort(p->owner,&creator,&c),"raw abort while driver outside Ll");
}
int main(int argc,char **argv) {
 int which=argc>1?atoi(argv[1]):0;mode=0;
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};struct loom_service_request records[2]={0};
 struct {struct loom_ring_hdr hdr;struct loom_cqe cq[2];struct loom_service_buffer_receipt receipts[2];} ring={0};
 struct loom_service_engine index={0};
 struct Loom l={.service_engine=&index,.magic=LOOM_MAGIC,.service_as=&ring,.service_slots=&slots,.service_nodes=&nodes,
 .ring_kva=(u8 *)&ring,.cq_entries=2,.cqe_off=(u32)((u8 *)ring.cq-(u8 *)&ring),
 .receipt_off=(u32)((u8 *)ring.receipts-(u8 *)&ring),.receipt_size=sizeof(ring.receipts),
 .service_requests={.records=records,.capacity=2}};

 l.service_creator=77;creator=(struct Proc){.magic=PROC_MAGIC,.as=l.service_as,.stripes=77};
 struct loom_service_ref scope,target={.slot=9,.incarnation=5};destination(&l,&scope);
 struct loom_service_control c=input(scope);struct loom_service_snapshot s=query(&l,scope);
 struct loom_service_protocol *p=NULL,*pin=NULL;
 CHECK(s.kind==LOOM_SERVICE_SCOPE && s.state==LOOM_SERVICE_RESERVED && !s.active_ops &&
       !s.pending_terminals && !s.reason && !s.status_flags && control_same(s.scope,scope),"unused snapshot exact and empty");
 if(which==0) {
  struct Proc stranger=creator;stranger.stripes++;
  struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=scope},out,before;
  memset(&out,0xa5,sizeof(out));before=out;
  CHECK(loom_private_service_query(&l,&stranger,&in,&out)==-T_E_ACCES && !memcmp(&out,&before,sizeof(out)),"foreign creator refuses untouched output");
  stranger=creator;stranger.as=&stranger;
  CHECK(loom_private_service_abort(&l,&stranger,&c)==-T_E_ACCES,"foreign image denied");
  in.kind=1;CHECK(loom_private_service_query(&l,&creator,&in,&out)==-T_E_INVAL && !memcmp(&out,&before,sizeof(out)),"output fields rejected without write");in.kind=0;
  in.object.incarnation++;CHECK(loom_private_service_query(&l,&creator,&in,&out)==-T_E_NOENT && !memcmp(&out,&before,sizeof(out)),"stale query leaves output unchanged");
  c.flags=1;CHECK(loom_private_service_abort(&l,&creator,&c)==-T_E_INVAL,"unknown flags denied");c.flags=0;
  CHECK(!loom_private_service_reap(&l,&creator,&c) && loom_private_service_reap(&l,&creator,&c)==-T_E_NOENT,"unused reap and stale reuse");
 } else if(which==1) {
  CHECK(!loom_private_service_abort(&l,&creator,&c),"unused abort accepted");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==-T_E_CANCELED && s.status_flags==LOOM_SERVICE_LOCAL_RETIRED,"unused abort reports completed local retirement");
  CHECK(loom_private_service_abort(&l,&creator,&c)==1 && !loom_private_service_reap(&l,&creator,&c),"unused abort idempotence and reap");
 } else if(which==2) {
  mode=103;
  CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p),"raw prepare");s=query(&l,scope);
  CHECK(s.active_ops==1 && !s.pending_terminals,"PREPARED owner counted");
  CHECK(!loom_private_service_abort(&l,&creator,&c) && loom_private_connect_publish(p,2)==-T_E_CANCELED,"abort denies raw publication");
  s=query(&l,scope);CHECK(s.state==LOOM_SERVICE_ABORTING && !(s.status_flags&LOOM_SERVICE_LOCAL_RETIRED) && loom_private_service_reap(&l,&creator,&c)==-T_E_BUSY,"constructing abort does not claim retirement");
  CHECK(!loom_private_connect_rollback(p),"raw aborted constructor rolls back");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && !s.active_ops && s.reason==-T_E_CANCELED && s.status_flags==LOOM_SERVICE_LOCAL_RETIRED,"rollback retains diagnostic after physical cleanup");
  CHECK(!loom_private_service_reap(&l,&creator,&c),"aborted constructor reap");
 } else if(which==3 || which==5) {
  CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p) && !loom_engine_adopt(p,2),"indexed preparation");
  l.cq_tail=2;CHECK(loom_engine_visit(&l,3)==1 && loom_engine_visit(&l,4)==1,"drive to READY behind full CQ");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_READY && !s.active_ops && s.pending_terminals==1 && s.status_flags==LOOM_SERVICE_BYTES_SENT,"READY has retained terminal and sticky bytes");
  struct loom_service_ref child={0};
  if(which==5)expected_child_pins=1;
  if(which==5)CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&child) && !loom_service_node_publish(&slots,&nodes,child),"child pins READY parent");
  CHECK(!loom_engine_get(&l,scope,&pin) && !loom_private_service_abort(&l,&creator,&c),"abort indexed READY owner");
  CHECK(loom_engine_visit(&l,5)==1,"actual cursor abort");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==-T_E_CANCELED && s.pending_terminals==1 && s.status_flags==3 && records[p->request.index].result==(s32)scope.slot,"abort retains prior success and diagnostics");
  CHECK(loom_private_service_reap(&l,&creator,&c)==-T_E_BUSY,"retired owner pin blocks reap");
  ring.hdr.cq_head=1;CHECK(!loom_private_post_terminal(&l,p->request),"post retained terminal");s=query(&l,scope);
  CHECK(!s.active_ops && !s.pending_terminals && s.reason==-T_E_CANCELED,"DELIVERED is storage not active obligation");
  CHECK(!loom_engine_put(&l,scope,pin) && !loom_engine_collect(&l,scope),"return pin and collect");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.status_flags==3 && s.reason==-T_E_CANCELED,"collected owner leaves stable diagnostic");
  if(which==5) {
   CHECK(loom_private_service_reap(&l,&creator,&c)==-T_E_BUSY,"child pin prevents parent reap");
   struct loom_service_snapshot f=query(&l,child);CHECK(f.kind==LOOM_SERVICE_FID && control_same(f.scope,scope),"FID query reports exact parent");
   struct loom_service_control fc=input(child);CHECK(!loom_private_service_reap(&l,&creator,&fc),"unused child reap after abort");
  }
  CHECK(!loom_private_service_reap(&l,&creator,&c),"collected retired scope reap");
 } else if(which==4) {
  mode=101;CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p) && !loom_private_connect_publish(p,2),"raw published owner");
  CHECK(loom_private_connect_step(p,3)==-T_E_CANCELED && records[p->request.index].result==-T_E_CANCELED,"raw metadata abort wins before READY commit");
  s=query(&l,scope);CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==-T_E_CANCELED && s.status_flags==3,"raw driver publishes sticky diagnostics");
  CHECK(!loom_private_post_terminal(&l,p->request) && !loom_private_connect_release(p) && !loom_private_service_reap(&l,&creator,&c),"raw cancelled owner fully reaps");
 } else if(which==7 || which==8) {
  mode=which==7?104:105;
  CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p),"error/installation prepare");
  if(which==8)hook_armed=1;
  CHECK(!loom_engine_adopt(p,2),"error/installation adoption");
  if(which==7)p->progress.handshake.phase=P9_HS_ATTACH_SEND;
  l.cq_tail=2;CHECK(loom_engine_visit(&l,3)==1,"error/installation retirement visit");s=query(&l,scope);
  CHECK(s.state==LOOM_SERVICE_RETIRED && s.reason==(which==7?-T_E_IO:-T_E_CANCELED) && s.pending_terminals==1,"error/installation diagnostics");
  if(which==7)CHECK(nodes.nodes[scope.slot].failure_phase==P9_HS_ATTACH_SEND,"retained actual failure phase");
  ring.hdr.cq_head=1;CHECK(!loom_private_post_terminal(&l,p->request) && !loom_engine_collect(&l,scope),"error/installation collect");
  if(which==7)CHECK(nodes.nodes[scope.slot].failure_phase==P9_HS_ATTACH_SEND,"failure phase survives collection");
  CHECK(!loom_private_service_reap(&l,&creator,&c),"error/installation reap");
 } else if(which==6) {
  struct loom_service_ref ref={0};CHECK(!loom_slot_reserve(&slots,LOOM_SERVICE_TARGET,&ref) && !loom_slot_publish(&slots,ref),"target fixture");
  s=query(&l,ref);CHECK(s.kind==LOOM_SERVICE_TARGET && s.state==LOOM_SERVICE_READY && control_zero_ref(s.scope),"target query");
  c=input(ref);CHECK(loom_private_service_abort(&l,&creator,&c)==-T_E_INVAL && !loom_private_service_reap(&l,&creator,&c) && dispatch_kind==LOOM_SERVICE_TARGET,"target controls dispatch exact primitive");
  CHECK(!loom_slot_reserve(&slots,LOOM_SERVICE_POOL,&ref) && !loom_slot_publish(&slots,ref),"pool fixture");c=input(ref);
  struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=ref};
  CHECK(loom_private_service_query(&l,&creator,&in,&s)==-T_E_INVAL,"POOL uses dedicated query");
  l.receipt_size=0;CHECK(loom_private_service_reap(&l,&creator,&c)==-T_E_OPNOTSUPP,"pool mode required");l.receipt_size=32;
  CHECK(!loom_private_service_reap(&l,&creator,&c) && dispatch_kind==LOOM_SERVICE_POOL,"pool reap dispatch");
 }
 CHECK(!held && !temporary_roots,"control locks and temporary roots balance");puts("PASS service control");return 0;
}
