
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held,wakes;
static void spin_lock(spin_lock_t *l) {CHECK(!l->held && !held,"lock reentry");l->held=1;held++;}
static void spin_unlock(spin_lock_t *l) {CHECK(l->held && held==1,"lock release");l->held=0;held--;}
struct loom_ring_hdr {u32 cq_head,cq_tail,cq_mask;};
struct loom_cqe {u64 user_data;s32 result;u32 flags;};
struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;u32 magic;spin_lock_t lock;void *service_as;bool service_closing;
 struct loom_request_table service_requests;u8 *ring_kva;u32 hdr_off,cqe_off,receipt_off,receipt_size,cq_tail,cq_entries;int cq_waiters;};
static bool loom_is_private(struct Loom *l) {return l && l->service_as;}
static void poll_waiter_list_wake(int *p) {(void)p;CHECK(!held,"wake outside owner lock");wakes++;}

enum {P9_HS_VERSION_SEND,P9_HS_VERSION_RECV,P9_HS_ATTACH_SEND,P9_HS_ATTACH_RECV,P9_HS_READY};
struct SrvConn {bool dead;};
struct SrvConnectAdmission {struct SrvConn *conn;bool published;};
struct p9_client {int dummy;};
struct loom_service_protocol {
 struct Loom *owner;struct SrvConnectAdmission admission;struct p9_client client;
 struct {struct {u32 phase;} handshake;} progress;
 struct loom_service_ref scope;struct loom_request_ref request;u64 connect_deadline;
 s32 abort_reason;u32 users;bool connect_bound,driving,locally_retired;
};
static struct loom_service_protocol *active;
static struct SrvConn cn;
static int mode,allocations,releases,steps,aborts,teardowns,publishes,accept_wakes;
static void extinction(const char *s) {CHECK(false,s);}
int loom_private_connect_step(struct loom_service_protocol *,u64);
int loom_private_connect_abort(struct loom_service_protocol *,s32);
int loom_private_connect_release(struct loom_service_protocol *);
static void close_owner(struct Loom *l) {l->service_closing=true;l->service_slots->closing=true;}
static int loom_private_protocol_prepare(struct Loom *l,struct loom_service_ref target,u64 deadline,u64 now,struct loom_service_protocol **out) {
 (void)target;(void)deadline;(void)now;
 CHECK(!held && l->service_requests.occupied==1,"terminal reserved before allocation");
 struct loom_service_request *r=&l->service_requests.records[0];
 CHECK(r->phase==LREQ_PREPARED && l->service_slots->slots[r->scope.slot].pins==2,"two exact scope pins before allocation");
 if(mode==10)return -T_E_NOMEM;
 allocations++;active=calloc(1,sizeof(*active));CHECK(active,"host allocation");
 active->owner=l;active->admission.conn=&cn;*out=active;return 0;
}
static int loom_private_protocol_release_unused(struct loom_service_protocol *p) {
 CHECK(!held && !p->connect_bound && !p->admission.published && !p->owner->service_requests.occupied &&
       !p->owner->service_slots->slots[p->scope.slot].pins,"rollback ledger before native release");
 releases++;free(p);active=NULL;return 0;
}
static int loom_private_protocol_release_retired(struct loom_service_protocol *p) {
 CHECK(!held && p->locally_retired && !p->connect_bound && !p->users && !p->driving && aborts &&
       !p->owner->service_requests.occupied && !p->owner->service_slots->slots[p->scope.slot].pins,
       "retired cleanup after ledger pins and protocol users");
 releases++;free(p);active=NULL;return 0;
}
static int devsrv_connect_publish(struct SrvConnectAdmission *a) {
 CHECK(held && active->owner->service_requests.records[0].phase==LREQ_PREPARED,"publication holds terminal reservation and lock");
 if(mode==9)return -T_E_NOENT;
 a->published=true;publishes++;return 0;
}
static void devsrv_connect_wake(struct SrvConnectAdmission *a) {
 CHECK(!held && a->published && active->owner->service_requests.records[0].phase==LREQ_ACTIVE,"accept wake after atomic admission outside lock");accept_wakes++;
}
static void srvconn_teardown(struct SrvConn *c) {
 CHECK(!held && c==&cn,"teardown outside lock");c->dead=true;teardowns++;
 if(mode==6 && teardowns==1) {
  CHECK(active->users==1,"abort pins transport during teardown");
  CHECK(loom_private_connect_step(active,4)==-T_E_CANCELED && !active->locally_retired &&
        active->owner->service_nodes->nodes[active->scope.slot].state==LOOM_SERVICE_ABORTING,
        "live abort user delays local retirement");
  CHECK(loom_private_connect_release(active)==-T_E_BUSY,"live abort user prevents free");
 }
}
static int p9_client_progress_step(struct p9_client *c,u64 now) {
 (void)c;(void)now;steps++;
 CHECK(!held && active->driving && active->owner->service_requests.records[0].borrows==1,"driver owns request borrow outside lock");
 CHECK(loom_private_connect_step(active,4)==-T_E_BUSY,"second driver refused");
 if(mode==2) CHECK(!loom_private_connect_abort(active,-T_E_CANCELED),"abort during client step");
 if(mode==4)close_owner(active->owner);
 if(mode==5)return -T_E_INVAL;
 active->progress.handshake.phase=steps==1?P9_HS_ATTACH_RECV:P9_HS_READY;
 return 1; // IO progress is not itself handshake completion.
}
static void p9_client_progress_abort(struct p9_client *c) {
 (void)c;CHECK(!held && active->driving && !active->owner->service_requests.records[0].borrows,"local abort after parser borrow outside lock");aborts++;
}
// CONNECT owns a charged protocol object plus precharged terminal obligation.
// Ll serializes exact scope identity, admission and terminal success with abort.
// Native publication occurs inside that interval; protocol work, teardown,
// waiter wakes, allocation and final drops occur outside it. The sole driver
// holds a request borrow while running the client. RETIRED requires actual
// cursor abort and no concurrent users, never merely an acknowledged abort.
//
// Internal executor primitive only. The caller supplies entry/storage lifetime
// and prevents general RPCs escaping this handshake owner. Ring dispatch, fair
// scheduling, close queues and user-facing activation are separate integration.

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
        return rc;
    }
    p->scope = scope; p->request = request; p->connect_deadline = deadline;
    p->connect_bound = true;
    *out = p;
    return 0;
}
int loom_private_connect_rollback(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_request *r = connect_request(p);
    if (!p->connect_bound || p->admission.published || p->users || p->driving ||
        !r || r->phase != LREQ_PREPARED || r->borrows) {
        spin_unlock(&l->lock); return -T_E_BUSY;
    }
    connect_rollback_locked(l,p->scope,p->request);
    p->connect_bound = false;
    spin_unlock(&l->lock);
    return loom_private_protocol_release_unused(p);
}
int loom_private_connect_publish(struct loom_service_protocol *p, u64 now) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n = connect_scope(l,p->scope);
    int rc = !p->connect_bound || p->admission.published || p->users || p->driving ?
        -T_E_BUSY : l->service_closing ? -T_E_CANCELED :
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
    if (!p->abort_reason) p->abort_reason = reason;
    if (n->state != LOOM_SERVICE_RETIRED) n->state = LOOM_SERVICE_ABORTING;
}
int loom_private_connect_abort(struct loom_service_protocol *p, s32 reason) {
    if (!p || !p->owner || reason >= 0) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n = connect_scope(l,p->scope);
    if (!p->connect_bound || !p->admission.published || !n) {
        spin_unlock(&l->lock); return -T_E_NOENT;
    }
    if (p->locally_retired || p->abort_reason) { spin_unlock(&l->lock); return 1; }
    if (p->users == UINT32_MAX) { spin_unlock(&l->lock); return -T_E_BUSY; }
    ++p->users;
    connect_latch(p,n,reason);
    spin_unlock(&l->lock);
    srvconn_teardown(p->admission.conn);
    spin_lock(&l->lock);
    --p->users;
    spin_unlock(&l->lock);
    return 0;
}
int loom_private_connect_step(struct loom_service_protocol *p, u64 now) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l = p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n = connect_scope(l,p->scope);
    struct loom_service_request *r = connect_request(p);
    if (!p->connect_bound || !p->admission.published || !n || !r) {
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
    spin_unlock(&l->lock);
    int rc=aborting ? 0 : p9_client_progress_step(&p->client,now);
    spin_lock(&l->lock);
    if (borrowed && loom_request_put(&l->service_requests,p->request))
        extinction("CONNECT lost parser borrow");
    if (l->service_closing) connect_latch(p,n,-T_E_CANCELED);
    if (rc < 0) connect_latch(p,n,rc == -T_E_TIMEDOUT ? rc : -T_E_IO);
    if (!p->abort_reason) {
        u32 phase=p->progress.handshake.phase;
        if (phase >= P9_HS_ATTACH_RECV) {
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
        return rc;
    }
    s32 reason=p->abort_reason;
    spin_unlock(&l->lock);
    // Sole driver, no escaped RPCs: abort returns with local RX/TX cursors and
    // callbacks detached. Teardown alone could not establish this fact.
    srvconn_teardown(p->admission.conn);
    p9_client_progress_abort(&p->client);
    spin_lock(&l->lock);
    if (r->phase == LREQ_ACTIVE &&
        loom_request_finish(&l->service_requests,l->service_slots,l->service_nodes,p->request,reason))
        extinction("CONNECT lost cancellation obligation");
    // A concurrent abort caller may still hold the transport during teardown.
    // Keep ABORTING until a later visit observes it gone; never free under it.
    if (!p->users) { p->locally_retired=true; n->state=LOOM_SERVICE_RETIRED; }
    p->driving=false;
    spin_unlock(&l->lock);
    return reason;
}
int loom_private_connect_release(struct loom_service_protocol *p) {
    if (!p || !p->owner) return -T_E_INVAL;
    struct Loom *l=p->owner;
    spin_lock(&l->lock);
    struct loom_service_node *n=connect_scope(l,p->scope);
    struct loom_service_request *r=connect_request(p);
    if (!p->connect_bound || !p->locally_retired || p->driving || p->users ||
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
int main(int argc,char **argv) {
 mode=argc>1?atoi(argv[1]):0;
 struct loom_slot_table slots={0};struct loom_service_node_table nodes={0};
 struct loom_service_request records[2]={0};
 struct {struct loom_ring_hdr hdr;struct loom_cqe cq[2];struct loom_service_buffer_receipt receipts[2];} ring={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&ring,.service_slots=&slots,.service_nodes=&nodes,
 .ring_kva=(u8 *)&ring,.cq_entries=2,.cq_tail=2,
 .cqe_off=(u32)((u8 *)ring.cq-(u8 *)&ring),.receipt_off=(u32)((u8 *)ring.receipts-(u8 *)&ring),.receipt_size=sizeof(ring.receipts),
 .service_requests={.records=records,.capacity=2}};
 struct loom_service_ref scope,target={.slot=9,.incarnation=5};
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope) &&
       !loom_service_node_publish(&slots,&nodes,scope),"destination control");
 struct loom_service_protocol *p=NULL,*other=NULL;
 int rc=loom_private_connect_prepare(&l,scope,target,77,100,1,&p);
 if(mode==10) {CHECK(rc==-T_E_NOMEM && !p && !l.service_requests.occupied && !slots.slots[scope.slot].pins && !allocations,"allocation refusal unwinds request and pins");puts("PASS CONNECT control");return 0;}
 CHECK(!rc && p && allocations==1,"prepared control");
 CHECK(loom_private_connect_prepare(&l,scope,target,88,100,1,&other)==-T_E_BUSY && !other && allocations==1,"duplicate CONNECT refused before native allocation");
 if(mode==8)close_owner(&l);
 rc=loom_private_connect_publish(p,mode==7?100:2);
 if(mode>=7 && mode<=9) {
  CHECK(rc==(mode==7?-T_E_TIMEDOUT:mode==8?-T_E_CANCELED:-T_E_NOENT) && !publishes && !accept_wakes && !steps && records[0].phase==LREQ_PREPARED,"failed publication has no effects");
  CHECK(!loom_private_connect_rollback(p) && releases==1,"prepared rollback");puts("PASS CONNECT control");return 0;
 }
 CHECK(!rc && publishes==1 && accept_wakes==1 && records[0].phase==LREQ_ACTIVE && nodes.nodes[scope.slot].state==LOOM_SERVICE_ADMITTED,"admitted control");
 CHECK(loom_private_connect_rollback(p)==-T_E_BUSY && loom_private_connect_release(p)==-T_E_BUSY,"published owner cannot free early");
 if(mode==1 || mode==6) {
  CHECK(!loom_private_connect_abort(p,-T_E_CANCELED) && !p->locally_retired && nodes.nodes[scope.slot].state==LOOM_SERVICE_ABORTING,"acknowledgement not retirement");
  CHECK(loom_private_connect_abort(p,-T_E_IO)==1 && p->abort_reason==-T_E_CANCELED,"first reason wins");
 }
 rc=loom_private_connect_step(p,mode==3?100:3);
 s32 result=mode==0?(s32)scope.slot:mode==3?-T_E_TIMEDOUT:mode==5?-T_E_IO:-T_E_CANCELED;
 if(mode==0) {
  CHECK(!rc && records[0].phase==LREQ_ACTIVE && nodes.nodes[scope.slot].state==LOOM_SERVICE_ATTACH,"IO progress is not READY");
  CHECK(loom_private_connect_step(p,4)==1 && records[0].result==result && nodes.nodes[scope.slot].state==LOOM_SERVICE_READY,"handshake READY commits success");
  CHECK(loom_private_connect_step(p,101)==1 && !p->abort_reason && steps==2,"deadline ends on success");
  CHECK(!loom_private_connect_abort(p,-T_E_CANCELED),"abort after success");
  rc=loom_private_connect_step(p,5);
 }
 CHECK(rc==(mode==0?-T_E_CANCELED:result) && p->locally_retired && aborts && nodes.nodes[scope.slot].state==LOOM_SERVICE_RETIRED,"actual driver abort retires");
 CHECK(records[0].phase==LREQ_TERMINAL && records[0].result==result && !records[0].borrows,"immutable terminal result");
 if(mode==1 || mode==3 || mode==6) CHECK(!steps,"preexisting abort avoids client progress");
 ring.cq[0].user_data=999;
 CHECK(loom_private_post_terminal(&l,p->request)==-T_E_AGAIN && ring.cq[0].user_data==999 &&
       records[0].phase==LREQ_TERMINAL && loom_private_connect_release(p)==-T_E_BUSY,"CQ full retains protocol and terminal");
 ring.hdr.cq_head=1;
 CHECK(!loom_private_post_terminal(&l,p->request) && ring.cq[0].result==result && ring.cq[0].user_data==77,"exact CQ delivery");
 CHECK(loom_private_connect_reap(&l,scope)==-T_E_BUSY,"protocol pin blocks metadata reap");
 CHECK(!loom_private_connect_release(p) && releases==1 && !l.service_requests.occupied,"retired release");
 CHECK(!loom_private_connect_reap(&l,scope) && loom_private_connect_reap(&l,scope)==-T_E_NOENT,"scope reaps once");
 CHECK(!held,"balanced lock");puts("PASS CONNECT control");return 0;
}
