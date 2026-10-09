// Terminal obligations survive CQ backpressure and abort. PREPARED reserves a
// bounded cell and exact parent pin before publication; ACTIVE owns one future
// terminal; TERMINAL is immutable and locally quiescent; DELIVERED can be reaped
// only after the owner ends its remaining record references. Identity never
// rewinds, including failed admission. All transitions are under the owner lock.
// This implements the request/terminal subset of loom_service.tla. It does not
// establish native admission, usercopy, caller authority, CQ capacity or actual
// protocol retirement; those facts are supplied by the owning engine.
#include <thylacine/loom_service_requests.h>
#include <thylacine/errno.h>

static struct loom_service_request *request_exact(const struct loom_request_table *t,
                                                   struct loom_request_ref ref) {
    if (!t || !t->records || ref.reserved || !ref.identity || ref.index >= t->capacity)
        return NULL;
    struct loom_service_request *r = &t->records[ref.index];
    return r->phase != LREQ_FREE && r->identity == ref.identity ? r : NULL;
}
static bool request_scope_held(const struct loom_slot_table *s, struct loom_service_ref ref) {
    if (!s || ref.reserved || !ref.incarnation || ref.slot >= LOOM_SERVICE_SLOT_COUNT)
        return false;
    const struct loom_service_slot *p = &s->slots[ref.slot];
    return p->incarnation == ref.incarnation && p->kind == LOOM_SERVICE_SCOPE &&
           (p->phase == LSLOT_LIVE || p->phase == LSLOT_RETIRING) && p->pins;
}
static bool request_scope_open(const struct loom_slot_table *s,
                               const struct loom_service_node_table *n,
                               struct loom_service_ref ref) {
    if (!s || !n || s->closing || ref.reserved || !ref.incarnation ||
        ref.slot >= LOOM_SERVICE_SLOT_COUNT) return false;
    const struct loom_service_slot *p = &s->slots[ref.slot];
    u32 state = n->nodes[ref.slot].state;
    return p->incarnation == ref.incarnation && p->kind == LOOM_SERVICE_SCOPE &&
           p->phase == LSLOT_LIVE && state >= LOOM_SERVICE_RESERVED &&
           state <= LOOM_SERVICE_READY;
}
int loom_request_prepare(struct loom_request_table *t, struct loom_slot_table *s,
                         struct loom_service_node_table *n, struct loom_service_ref scope,
                         u64 user_data, u64 deadline, u64 now, struct loom_request_ref *out) {
    if (!t || !t->records || !t->capacity || !s || !n || !out) return -T_E_INVAL;
    if (t->detached || s->closing) return -T_E_CANCELED;
    if (deadline && deadline <= now) return -T_E_TIMEDOUT;
    if (!request_scope_open(s,n,scope)) return -T_E_NOENT;
    if (t->occupied >= t->capacity || t->next_identity == UINT64_MAX) return -T_E_NOSPC;
    for (u32 i=0;i<t->capacity;++i) {
        struct loom_service_request *r = &t->records[i];
        if (r->phase != LREQ_FREE) continue;
        int rc = loom_slot_pin(s,scope,LOOM_SERVICE_SCOPE);
        if (rc) return rc;
        *r = (struct loom_service_request){ .scope=scope, .identity=++t->next_identity,
            .user_data=user_data, .deadline=deadline, .phase=LREQ_PREPARED };
        ++t->occupied;
        *out = (struct loom_request_ref){ .index=i, .identity=r->identity };
        return 0;
    }
    return -T_E_NOSPC;
}
int loom_request_hold_owner(struct loom_request_table *t, struct loom_request_ref ref) {
    struct loom_service_request *r=request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->phase!=LREQ_PREPARED || r->owners || r->borrows) return -T_E_BUSY;
    r->owners=1;
    return 0;
}
int loom_request_drop_owner(struct loom_request_table *t, struct loom_request_ref ref) {
    struct loom_service_request *r=request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->owners!=1 || r->borrows || r->phase==LREQ_ACTIVE) return -T_E_BUSY;
    r->owners=0;
    return 0;
}
int loom_request_check_admit(struct loom_request_table *t, struct loom_slot_table *s,
                       struct loom_service_node_table *n, struct loom_request_ref ref, u64 now) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->phase != LREQ_PREPARED) return -T_E_BUSY;
    if (t->detached || !request_scope_open(s,n,r->scope)) return -T_E_CANCELED;
    if (r->deadline && r->deadline <= now) return -T_E_TIMEDOUT;
    return 0;
}
int loom_request_admit(struct loom_request_table *t, struct loom_slot_table *s,
                       struct loom_service_node_table *n, struct loom_request_ref ref, u64 now) {
    int rc = loom_request_check_admit(t,s,n,ref,now);
    if (!rc) request_exact(t,ref)->phase = LREQ_ACTIVE;
    return rc;
}
int loom_request_reject(struct loom_request_table *t, struct loom_slot_table *s,
                        struct loom_request_ref ref, s32 reason) {
    struct loom_service_request *r=request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (reason >= 0) return -T_E_INVAL;
    if (r->phase != LREQ_PREPARED || r->borrows) return -T_E_BUSY;
    if (!request_scope_held(s,r->scope)) return -T_E_INVAL;
    r->result=reason;
    r->phase=LREQ_TERMINAL;
    return 0;
}
static int request_remove(struct loom_request_table *t, struct loom_slot_table *s,
                           struct loom_request_ref ref, u32 phase) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->phase != phase || r->borrows || r->owners) return -T_E_BUSY;
    if (!t->occupied || !request_scope_held(s,r->scope)) return -T_E_INVAL;
    struct loom_service_ref scope = r->scope;
    *r = (struct loom_service_request){0};
    --t->occupied;
    (void)loom_slot_unpin(s,scope);
    return 0;
}
int loom_request_rollback(struct loom_request_table *t, struct loom_slot_table *s,
                          struct loom_request_ref ref) {
    return request_remove(t,s,ref,LREQ_PREPARED);
}
int loom_request_borrow(struct loom_request_table *t, struct loom_slot_table *s,
                        struct loom_service_node_table *n, struct loom_request_ref ref) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->phase != LREQ_ACTIVE) return -T_E_BUSY;
    if (t->detached || !request_scope_open(s,n,r->scope)) return -T_E_CANCELED;
    if (r->borrows == UINT32_MAX) return -T_E_NOSPC;
    ++r->borrows;
    return 0;
}
int loom_request_put(struct loom_request_table *t, struct loom_request_ref ref) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->phase != LREQ_ACTIVE || !r->borrows) return -T_E_INVAL;
    --r->borrows;
    return 0;
}
int loom_request_finish(struct loom_request_table *t, struct loom_slot_table *s,
                        struct loom_service_node_table *n, struct loom_request_ref ref, s32 result) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->phase != LREQ_ACTIVE || r->borrows) return -T_E_BUSY;
    if (!request_scope_held(s,r->scope)) return -T_E_INVAL;
    if (result >= 0 && (t->detached || !request_scope_open(s,n,r->scope))) return -T_E_CANCELED;
    r->result = result;
    r->phase = LREQ_TERMINAL;
    return 0;
}
int loom_request_peek(const struct loom_request_table *t, struct loom_request_ref ref,
                      u64 *user_data, s32 *result) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (!user_data || !result) return -T_E_INVAL;
    if (r->phase != LREQ_TERMINAL || r->borrows) return -T_E_BUSY;
    *user_data = r->user_data; *result = r->result;
    return 0;
}
int loom_request_deliver(struct loom_request_table *t, struct loom_request_ref ref) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (r->phase != LREQ_TERMINAL || r->borrows) return -T_E_BUSY;
    if (t->detached) return -T_E_CANCELED;
    r->phase = LREQ_DELIVERED;
    return 0;
}
int loom_requests_detach(struct loom_request_table *t, const struct loom_slot_table *s) {
    if (!t || !t->records || !s) return -T_E_INVAL;
    if (!s->closing) return -T_E_BUSY;
    for (u32 i=0;i<t->capacity;++i) if (t->records[i].borrows) return -T_E_BUSY;
    t->detached = true;
    return 0;
}
int loom_request_discard(struct loom_request_table *t, struct loom_request_ref ref) {
    struct loom_service_request *r = request_exact(t,ref);
    if (!r) return -T_E_NOENT;
    if (!t->detached || r->phase != LREQ_TERMINAL || r->borrows) return -T_E_BUSY;
    r->phase = LREQ_DELIVERED;
    return 0;
}
int loom_request_reap(struct loom_request_table *t, struct loom_slot_table *s,
                      struct loom_request_ref ref) {
    return request_remove(t,s,ref,LREQ_DELIVERED);
}
bool loom_requests_empty(const struct loom_request_table *t) {
    if (!t || !t->records || t->occupied) return false;
    for (u32 i=0;i<t->capacity;++i) {
        const struct loom_service_request *r=&t->records[i];
        if (r->phase != LREQ_FREE || r->identity || r->scope.incarnation || r->scope.slot ||
            r->scope.reserved || r->borrows || r->user_data || r->deadline || r->result || r->reserved)
            return false;
    }
    return true;
}

#include <stdio.h>
#include <string.h>
#include <assert.h>
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_request records[1];
static struct loom_request_table t;
static struct loom_service_ref scope;
static struct loom_request_ref ref;
#define CHECK(c,n) do{if(!(c)){fprintf(stderr,"%s\n",n);assert(0);}}while(0)
static void init(void) {
 memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));memset(records,0,sizeof(records));
 t=(struct loom_request_table){.records=records,.capacity=1};
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope),"scope prepare");
 CHECK(!loom_service_node_publish(&slots,&nodes,scope),"scope publish");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 CHECK(!loom_request_prepare(&t,&slots,&nodes,scope,11,0,1,&ref),"request prepare");
}
int main(void) {
 init();CHECK(sizeof(records[0])==56,"charge geometry unchanged");
 CHECK(!loom_request_hold_owner(&t,ref)&&records[0].owners==1,"owner retained");
 CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"one physical owner");
 CHECK(loom_request_rollback(&t,&slots,ref)==-T_E_BUSY,"rollback waits physical owner");
 CHECK(t.occupied==1&&slots.slots[scope.slot].pins==1,"rollback retains identity and scope");
 CHECK(!loom_request_drop_owner(&t,ref)&&records[0].owners==0,"owner dropped");
 CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"double owner drop rejected");
 CHECK(!loom_request_rollback(&t,&slots,ref)&&t.occupied==0,"rollback after physical free");
 struct loom_request_ref old=ref;
 CHECK(!loom_request_prepare(&t,&slots,&nodes,scope,12,0,1,&ref)&&ref.identity>old.identity,"identity burned");
 CHECK(loom_request_hold_owner(&t,old)==-T_E_NOENT,"stale hold refused");
 CHECK(!loom_request_hold_owner(&t,ref),"new owner");
 CHECK(loom_request_drop_owner(&t,old)==-T_E_NOENT&&records[0].owners==1,"stale drop cannot release new owner");
 CHECK(!loom_request_admit(&t,&slots,&nodes,ref,1),"admit held owner");
 CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"late attach refused");
 CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"active owner cannot disappear");
 CHECK(!loom_request_borrow(&t,&slots,&nodes,ref),"parser borrow");
 CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"borrow blocks owner drop");
 CHECK(!loom_request_put(&t,ref),"parser returns");
 CHECK(!loom_request_finish(&t,&slots,&nodes,ref,4),"commit while retaining physical owner");
 CHECK(!loom_request_deliver(&t,ref),"delivery independent from physical owner");
 CHECK(loom_request_reap(&t,&slots,ref)==-T_E_BUSY,"reap waits physical owner");
 CHECK(!loom_request_drop_owner(&t,ref),"destroy then drop");
 CHECK(!loom_request_reap(&t,&slots,ref),"reap after physical free");
 init();CHECK(!loom_request_hold_owner(&t,ref),"failed constructor owner");
 loom_slots_close(&slots);nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 CHECK(!loom_request_reject(&t,&slots,ref,-T_E_NOMEM),"failed admission terminal");
 CHECK(!loom_requests_detach(&t,&slots)&&!loom_request_discard(&t,ref),"consumer discard");
 CHECK(loom_request_reap(&t,&slots,ref)==-T_E_BUSY,"discard does not free physical owner");
 CHECK(!loom_request_drop_owner(&t,ref)&&!loom_request_reap(&t,&slots,ref),"close cleanup exact settlement");
 init();records[0].borrows=1;CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"constructor cannot overlap borrowed record");
 records[0].borrows=0;CHECK(!loom_request_hold_owner(&t,ref),"attach after borrow ends");
 CHECK(!loom_request_reject(&t,&slots,ref,-T_E_CANCELED),"rejection");
 records[0].borrows=1;CHECK(loom_request_drop_owner(&t,ref)==-T_E_BUSY,"terminal borrow prevents physical release");
 records[0].borrows=0;CHECK(!loom_request_drop_owner(&t,ref),"terminal release");
 init();CHECK(!loom_request_admit(&t,&slots,&nodes,ref,1),"legacy no-owner admission");
 CHECK(loom_request_hold_owner(&t,ref)==-T_E_BUSY,"attach only before admission");
 CHECK(!loom_request_finish(&t,&slots,&nodes,ref,0)&&!loom_request_deliver(&t,ref)&&!loom_request_reap(&t,&slots,ref),"existing CONNECT owner path unchanged");
 puts("PASS physical-owner lifecycle, stale identity, close/discard and no-owner compatibility");
}
