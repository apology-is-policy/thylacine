
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

// Internal bounded owner index and readiness. Private user dispatch stays disabled.
#ifndef THYLACINE_LOOM_SERVICE_ENGINE_H
#define THYLACINE_LOOM_SERVICE_ENGINE_H
#include <thylacine/loom_service_abi.h>
struct poll_waiter {
    u32                       magic;   // POLL_WAITER_MAGIC
    bool                      ready;   // set under list->lock by a producer
    int            *rendez;  // back-ref to the poller's private Rendez
    struct poll_waiter_list  *list;    // non-NULL while listed; NULL otherwise
    struct poll_waiter       *next;    // singly-linked; valid only while listed
};


#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16

struct ProcSqpollTicket { u64 stripes; };
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
    struct ProcSqpollTicket worker_ticket;
    bool worker_starting; // Ll serializes unpublished internal start attempts
    // Single wait owner, retained entry pins and embedded native hooks fit the
    // already charged index page. Only that owner touches the wait fields.
    // change is atomic: the Rendez predicate NEVER takes Ll (lock inversion).
    u64 change, wait_change, wait_mask, wait_deadline;
    u32 wait_cq_head;
    bool wait_active, wait_work;
    struct poll_waiter waiters[LOOM_SERVICE_SLOT_COUNT];
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
// Readiness-aware bounded visit; no peer waits or allocation. Caller retains l.
// Scans at most64 owners, visits at most one actionable owner round-robin.
int loom_engine_ready_visit(struct Loom *, u64 now);
// Internal wait transaction. A nonnegative arm result owns the sweep even when
// it reports immediate work (1); always disarm it. BUSY owns nothing. One owner
// may arm/sleep/disarm; concurrent explicit ready visits remain permitted.
// Caller retains l through disarm. tsleep uses the returned absolute deadline.
int loom_engine_wait_arm(struct Loom *, u64 now, u64 *deadline);
// ONLY a sleep/tsleep predicate: caller holds the existing sqpoll_park Rendez.
// No Ll, device, allocator, timer or protocol lock is acquired here.
int loom_engine_wait_changed(void *);
void loom_engine_wait_disarm(struct Loom *);
#endif

struct Loom {struct loom_service_engine *service_engine;u32 service_fixed_count,service_fixed_settling;bool sqpoll_stopping,sqpoll_exited;void *sqpoll;int sqpoll_park;struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;u32 magic;spin_lock_t lock;void *service_as;bool service_closing;
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
 for(u32 j=0;j<64;++j) CHECK(!p->owner->service_engine->waiters[j].list,"no hook at physical storage free");releases++;free(p);active=NULL;return 0;
}
static int loom_private_protocol_release_retired(struct loom_service_protocol *p) {
 CHECK(p!=externally_pinned,"external entry prevents storage release");
 CHECK(!held && p->locally_retired && !p->indexed && !p->connect_bound && !p->users && !p->driving &&
       !p->owner->service_slots->slots[p->scope.slot].pins,"indexed lifetime ends after pins and terminal");
 for(u32 j=0;j<64;++j) CHECK(!p->owner->service_engine->waiters[j].list,"no hook at physical storage free");releases++;free(p);active=NULL;return 0;
}
static int devsrv_connect_publish(struct SrvConnectAdmission *a) {
 CHECK(held && active->indexed,"native publication has indexed lifetime");
 struct loom_engine_entry *e=&active->owner->service_engine->entries[active->scope.slot];
 CHECK(e->phase==LENGINE_INSTALLING && e->pins==1,"installation pin covers native publication");
 if(mode==3)return -T_E_NOENT;
 a->published=true;publishes++;return 0;
}
static void devsrv_connect_wake(struct SrvConnectAdmission *a) {
 CHECK(!held && a->published,"native accept wake outside lock");accept_wakes++;
 struct Loom *l=active->owner;struct loom_service_protocol *p=NULL;
 CHECK(loom_engine_get(l,active->scope,&p)==-T_E_AGAIN && !p &&
       loom_engine_collect(l,active->scope)==-T_E_BUSY,"accept wake retains installing lifetime");
 if(mode==2)CHECK(!loom_engine_abort(l,active->scope,-T_E_CANCELED),"abort during accept wake");
 if(mode==5)close_owner(l);
}
static void srvconn_teardown(struct SrvConn *conn) {
 CHECK(!held,"teardown outside lock");conn->dead=true;teardowns++;
}
static int p9_client_progress_step(struct p9_client *client,u64 now) {
 (void)now;active=(void *)((u8 *)client-__builtin_offsetof(struct loom_service_protocol,client));
 CHECK(!held && active->driving && active->owner->service_requests.records[active->request.index].borrows==1,
       "entry owns parser borrow during client step");
 steps++;
 if(mode==6 && active->scope.slot==0)return 0;
 ++active->client.dummy;
 active->progress.handshake.phase=active->client.dummy==1?P9_HS_ATTACH_RECV:P9_HS_READY;
 return 1;
}
static void p9_client_progress_abort(struct p9_client *client) {
 active=(void *)((u8 *)client-__builtin_offsetof(struct loom_service_protocol,client));
 CHECK(!held && active->driving && !active->owner->service_requests.records[active->request.index].borrows,"cursor abort outside lock after parser borrow");aborts++;
}

static void loom_private_close_changed(struct Loom *l) {
 CHECK(!held,"change wake outside Ll");__atomic_fetch_add(&l->service_engine->change,1,__ATOMIC_RELEASE);
}
static void loom_ref(struct Loom *l){(void)l;}
static void loom_unref(struct Loom *l){(void)l;}

static int hook_armed;
static void boundary_unlocked(void) {
 if(!hook_armed || !active || !active->indexed)return;
 struct loom_engine_entry *e=&active->owner->service_engine->entries[active->scope.slot];
 if(e->phase!=LENGINE_INSTALLING)return;
 hook_armed=0;
 if(mode==1)CHECK(!loom_engine_abort(active->owner,active->scope,-T_E_CANCELED),"abort before publication");
 if(mode==7)close_owner(active->owner);
}

int loom_private_post_terminal(struct Loom *,struct loom_request_ref);

static struct Loom *test_ring;
static short native_ready[64];
static int race,linked,unlinked;
static u64 held_epoch;
static void poll_waiter_init(struct poll_waiter *pw,int *r) { *pw=(struct poll_waiter){.magic=0x504f4c57,.rendez=r}; }
static short srvconn_poll(struct SrvConn *c,bool client,short events,struct poll_waiter *pw) {
 CHECK(!held && client,"native poll outside Ll");
 u32 slot=64;
 for(u32 i=0;i<64;++i)if(test_ring->service_engine->entries[i].protocol && test_ring->service_engine->entries[i].protocol->admission.conn==c){slot=i;break;}
 CHECK(slot<64 && test_ring->service_engine->entries[slot].pins,"poll retains native owner");
 if(!pw && race==3) {
  race=0;
  struct loom_service_protocol *p=test_ring->service_engine->entries[slot].protocol;
  CHECK(!loom_engine_abort(test_ring,p->scope,-T_E_CANCELED),"race abort");
  (void)loom_private_connect_step(p,3);(void)loom_private_post_terminal(test_ring,p->request);
  u64 deadline=0;CHECK(!loom_engine_wait_arm(test_ring,3,&deadline),"collector parks behind transient ready pin");
  held_epoch=test_ring->service_engine->change;
 }
 if(!pw && race==4) {
  race=0;close_owner(test_ring);loom_private_close_changed(test_ring);
  CHECK(loom_engine_close_visit(test_ring)==0,"close parks behind transient ready pin");
  held_epoch=test_ring->service_engine->change;
 }
 if(pw) {
  CHECK(!pw->list,"register exactly once");pw->list=(void *)c;linked++;
  if(race==1){pw->ready=true;race=0;} // ready after level sample, before sleep
  if(race==2){loom_private_close_changed(test_ring);race=0;}
 }
 return native_ready[slot] & (events|POLLERR|POLLHUP);
}
static void poll_waiter_list_unregister(struct poll_waiter *pw) {
 CHECK(!held,"unhook outside Ll");
 if(!pw->list)return;
 for(u32 i=0;i<64;++i) if(test_ring->service_engine->wait_mask&(1ull<<i))
  CHECK(test_ring->service_engine->entries[i].pins,"all pins retained through whole unhook sweep");
 pw->list=NULL;unlinked++;
}
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
        if (a.events && srvconn_poll(p->admission.conn,true,a.events,NULL)) t->wait_work=true;
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
}
static void destination(struct Loom *l,struct loom_service_ref *ref) {
 CHECK(!loom_service_node_prepare(l->service_slots,l->service_nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},ref) &&
       !loom_service_node_publish(l->service_slots,l->service_nodes,*ref),"destination control");
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

 test_ring=&l;
 struct loom_service_ref scope,second={0},target={.slot=9,.incarnation=5};destination(&l,&scope);
 struct loom_service_protocol *p=NULL,*q=NULL;
 CHECK(!loom_private_connect_prepare(&l,scope,target,77,100,1,&p) && !loom_engine_adopt(p,2),"adopt control");
 u64 deadline=999;
 if(which==0 || which==2 || which==3) {
  CHECK(!loom_engine_ready_visit(&l,3) && !steps,"stalled owner does not spin");
  race=which==2?1:which==3?2:0;
  CHECK(loom_engine_wait_arm(&l,3,&deadline)==(which==3?1:0) && deadline==100 && linked==1,"register then sample and absolute deadline");
  CHECK(loom_engine_wait_changed(&l)==(which?1:0),"registration race remains observable");
  CHECK(loom_engine_wait_arm(&l,3,&deadline)==-T_E_BUSY && linked==1,"one wait owner");
  CHECK(index.entries[scope.slot].pins==1 && !loom_engine_empty(&index),"wait retains entry lifetime");
  loom_engine_wait_disarm(&l);CHECK(unlinked==linked && !index.entries[scope.slot].pins,"disarm exact pins");
 }
 if(which==1) {
  native_ready[scope.slot]=POLLOUT;
  CHECK(loom_engine_ready_visit(&l,3)==1 && steps==1,"already ready runs one quantum");
  CHECK(!loom_engine_ready_visit(&l,4) && steps==1,"receive phase ignores write readiness");
  CHECK(!loom_engine_wait_arm(&l,4,&deadline) && deadline==100,"receive interest parks");
  native_ready[scope.slot]=POLLIN;index.waiters[scope.slot].ready=true;
  CHECK(loom_engine_wait_changed(&l),"native delayed response wakes");loom_engine_wait_disarm(&l);
  CHECK(loom_engine_ready_visit(&l,5)==1 && steps==2 && records[p->request.index].phase==LREQ_DELIVERED,"ready commits and delivers");
  CHECK(!loom_engine_ready_visit(&l,101) && !loom_engine_wait_arm(&l,101,&deadline) && !deadline && linked==unlinked,"READY has no timer or native wait");
  loom_engine_wait_disarm(&l);
 }
 if(which==4) {
  CHECK(!loom_engine_wait_arm(&l,3,&deadline),"park before close");
  close_owner(&l);loom_private_close_changed(&l);
  CHECK(loom_engine_wait_changed(&l) && loom_engine_close_visit(&l)==0 && !l.service_requests.detached,"close wakes but pins block detach");
  loom_engine_wait_disarm(&l);
 }
 if(which==5) {
  CHECK(!loom_engine_wait_arm(&l,3,&deadline) && deadline==100,"absolute timeout armed without readiness");loom_engine_wait_disarm(&l);
  l.cq_tail=2;
  CHECK(loom_engine_ready_visit(&l,100)==1 && !steps && p->locally_retired && records[p->request.index].result==-T_E_TIMEDOUT,"deadline progresses without native event or CQ room");
  CHECK(!loom_engine_ready_visit(&l,101),"full CQ retirement does not spin");
 }
 if(which==6) {
  l.cq_tail=2;native_ready[scope.slot]=POLLOUT;
  CHECK(loom_engine_ready_visit(&l,3)==1,"first success step");native_ready[scope.slot]=POLLIN;
  CHECK(loom_engine_ready_visit(&l,4)==1 && records[p->request.index].phase==LREQ_TERMINAL,"full CQ preserves committed success");
  CHECK(!loom_engine_ready_visit(&l,101) && !loom_engine_wait_arm(&l,101,&deadline) && !deadline,"backpressure has no spin or old timer");
  ring.hdr.cq_head=1;
  CHECK(loom_engine_wait_changed(&l),"CQ drain changes wait predicate");loom_engine_wait_disarm(&l);
  CHECK(loom_engine_ready_visit(&l,102)==1 && records[p->request.index].phase==LREQ_DELIVERED && steps==2,"drain posts without duplicate protocol step");
 }
 if(which==7 || which==10) {
  if(which==7)p->driving=true;else p->users=1;
  native_ready[scope.slot]=POLLOUT;
  CHECK(!loom_engine_ready_visit(&l,101) && !loom_engine_wait_arm(&l,101,&deadline) && !deadline && !linked,"busy local holder parks instead of deadline spin");
  p->driving=false;p->users=0;loom_private_close_changed(&l);
  CHECK(loom_engine_wait_changed(&l),"holder return invalidates wait snapshot");loom_engine_wait_disarm(&l);
 }
 if(which==8 || which==9) {
  static struct SrvConn other;
  destination(&l,&second);CHECK(!loom_private_connect_prepare(&l,second,target,88,90,1,&q),"second prepare");q->admission.conn=&other;
  CHECK(!loom_engine_adopt(q,2),"second adopt");
  if(which==8) {
   native_ready[second.slot]=POLLOUT;
   CHECK(loom_engine_ready_visit(&l,3)==1 && !p->client.dummy && q->client.dummy==1,"stalled first owner cannot starve ready second");
   native_ready[second.slot]=POLLIN;
   CHECK(loom_engine_ready_visit(&l,4)==1 && !p->client.dummy && q->client.dummy==2,"round robin second quantum");
  }else{
   CHECK(!loom_engine_wait_arm(&l,3,&deadline) && deadline==90 && linked==2,"nearest deadline across all armed owners");
   close_owner(&l);loom_private_close_changed(&l);
   CHECK(loom_engine_close_visit(&l)==1 && loom_engine_close_visit(&l)==0 && !releases,"wait sweep retains both retired owners");
   loom_engine_wait_disarm(&l);CHECK(unlinked==2,"whole unhook sweep");
  }
 }
 if(which==12 || which==13) {
  race=which==12?3:4;
  CHECK(!loom_engine_ready_visit(&l,3),"transient scan has no direct ready action");
  CHECK(index.change>held_epoch,"transient pin return wakes dependent collector or close");
  if(which==12){CHECK(loom_engine_wait_changed(&l),"collector wait invalidated by final external pin return");loom_engine_wait_disarm(&l);}
 }
 if(which==11) {
  native_ready[scope.slot]=POLLHUP;
  CHECK(loom_engine_ready_visit(&l,3)==1 && steps==1,"hangup prompts nonblocking driver independent of requested direction");
 }
 close_owner(&l);loom_private_close_changed(&l);
 for(int i=0;i<5;++i) (void)loom_engine_close_visit(&l);
 CHECK(loom_engine_empty(&index) && allocations==releases && linked==unlinked && !held,"all owners and wait hooks settle");
 CHECK(sizeof(index)<=4096,"wait storage stays within charged page");
 puts("PASS readiness control");return 0;
}
