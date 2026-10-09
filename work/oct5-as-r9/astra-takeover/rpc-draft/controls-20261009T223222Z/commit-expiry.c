
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/loom_service_reply.h>
#include <thylacine/errno.h>
#define PAGE_SIZE 4096
struct p9_rpc {void (*on_complete)(struct p9_rpc *,int,struct p9_dispatch_result *);bool sending;};
struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;struct loom_request_table service_requests;bool service_closing;};
struct loom_service_operation;
struct loom_service_protocol {struct Loom *owner;struct loom_service_ref scope;bool driving,connect_bound,indexed,locally_retired;s32 abort_reason;struct loom_service_operation *op_head,*op_tail;u32 op_count;};
static void extinction(const char *m){fprintf(stderr,"INVARIANT %s\n",m);exit(86);}
enum loom_rpc_phase { LRPC_NEW, LRPC_QUEUED, LRPC_INFLIGHT, LRPC_SETTLING, LRPC_TERMINAL, LRPC_RETIRED };
struct loom_service_operation {
    struct Loom *owner;
    struct loom_service_protocol *protocol; // exact engine entry pin
    struct loom_request_ref request;
    struct loom_service_ref scope;
    struct p9_rpc rpc;
    struct loom_service_reply_expect expect;
    struct loom_service_reply reply;
    u8 component[256];
    // Linked under Ll; sole protocol driver owns callback capture while driving.
    struct loom_service_operation *next, *prev;
    u32 phase;
    bool callback_seen;
    s32 capture_error;
    u8 *payload; // resolved retained kernel extent, never a user pointer
    u32 payload_capacity;
};
// Internal owner transitions, not user admission or protocol authority. All
// except callback hold Ll. Caller owns the actual operation and exact engine
// entry/Loom pins. Before enqueue it must resolve and retain FID/destination,
// payload and creator authority; do not infer those facts from this list.
int loom_rpc_enqueue_locked(struct loom_service_operation *,u64 now);
// Sole p->driving owner only: begin, requeue and settle surround actual client
// calls. Begin occurs before submit_async (which may callback synchronously).
// A successful begin MUST be followed by submission or explicit local error
// capture if client validation refused without taking ownership.
int loom_rpc_begin_locked(struct loom_service_operation *,u64 now);
void loom_rpc_capture(struct p9_rpc *,int,struct p9_dispatch_result *);
// Only after the client call has returned; no callback/parser reference escapes.
// Use requeue for a next stat/open stage or a retryable no-wire AGAIN; the driver
// must prove the protocol disposition, never reissue an ambiguous wire effect.
int loom_rpc_requeue_locked(struct loom_service_operation *);
int loom_rpc_settle_locked(struct loom_service_operation *);
// Settlement bars resubmission while real FID/buffer drops run unlocked. Caller
// proves all physical parser/buffer users ended before finish. Success requires
// a validated captured success and fresh absolute deadline/abort check. For FID
// publication call check_finish before publishing, then finish, in ONE Ll hold.
// A timeout/cancellation refusal leaves state unchanged for scope abort followed
// by a negative terminal. The caller must abort the scope on admitted expiry.
int loom_rpc_check_finish_locked(struct loom_service_operation *,s32 result,u64 now);
int loom_rpc_finish_locked(struct loom_service_operation *,s32 result,u64 now);
// DELIVERED includes explicit closed-consumer discard. Unlink before actual
// physical free/refund, then drop request owner/reap and protocol/Loom roots.
// A terminal alone never lets storage disappear behind a blocked CQ consumer.
int loom_rpc_unlink_locked(struct loom_service_operation *);
_Static_assert(sizeof(struct loom_service_operation)<=PAGE_SIZE,"RPC owner buddy page");
static bool rpc_same(struct loom_service_ref a,struct loom_service_ref b) {
    return a.slot==b.slot && a.incarnation==b.incarnation && a.reserved==b.reserved;
}
static struct loom_service_request *rpc_record(struct loom_service_operation *o) {
    if(!o || !o->owner || !o->protocol || o->protocol->owner!=o->owner ||
       !rpc_same(o->scope,o->protocol->scope) || o->request.reserved ||
       !o->request.identity)return NULL;
    struct loom_request_table *t=&o->owner->service_requests;
    if(!t->records || o->request.index>=t->capacity)return NULL;
    struct loom_service_request *r=&t->records[o->request.index];
    return r->identity==o->request.identity && rpc_same(r->scope,o->scope) &&
           r->owners==1 ? r : NULL;
}
static int rpc_ready(struct loom_service_operation *o) {
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    struct loom_service_ref ref=o->scope;
    if(ref.reserved || !ref.incarnation || ref.slot>=LOOM_SERVICE_SLOT_COUNT)return -T_E_NOENT;
    struct loom_service_slot *s=&l->service_slots->slots[ref.slot];
    struct loom_service_node *n=&l->service_nodes->nodes[ref.slot];
    if(s->incarnation!=ref.incarnation || s->kind!=LOOM_SERVICE_SCOPE ||
       s->phase!=LSLOT_LIVE)return -T_E_NOENT;
    if(l->service_closing || l->service_slots->closing || l->service_requests.detached ||
       n->state!=LOOM_SERVICE_READY || n->reason || p->abort_reason ||
       p->locally_retired || !p->connect_bound || !p->indexed)return -T_E_CANCELED;
    return 0;
}
int loom_rpc_enqueue_locked(struct loom_service_operation *o,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    if(o->phase!=LRPC_NEW || o->next || o->prev || o->callback_seen ||
       o->capture_error || o->rpc.on_complete || o->rpc.sending || r->phase!=LREQ_PREPARED || r->borrows)
        return -T_E_BUSY;
    if(p->op_count>=l->service_requests.capacity)return -T_E_NOSPC;
    int rc=rpc_ready(o);
    if(rc)return rc;
    rc=loom_request_admit(&l->service_requests,l->service_slots,l->service_nodes,o->request,now);
    if(rc)return rc;
    if(loom_request_borrow(&l->service_requests,l->service_slots,l->service_nodes,o->request))
        extinction("RPC lost fresh request borrow");
    o->prev=p->op_tail;
    if(p->op_tail)p->op_tail->next=o;else p->op_head=o;
    p->op_tail=o;++p->op_count;o->phase=LRPC_QUEUED;
    return 0;
}
int loom_rpc_begin_locked(struct loom_service_operation *o,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_QUEUED ||
       r->phase!=LREQ_ACTIVE || r->borrows!=1 || o->rpc.sending)return -T_E_BUSY;
    int rc=rpc_ready(o);if(rc)return rc;
    if(o->capture_error || o->callback_seen)return -T_E_IO;
    if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;
    o->callback_seen=false;o->capture_error=0;
    o->rpc.on_complete=loom_rpc_capture;o->phase=LRPC_INFLIGHT;
    return 0;
}
void loom_rpc_capture(struct p9_rpc *rpc,int status,struct p9_dispatch_result *reply) {
    // Actual retained owner; callback runs only inside the sole driver. Never
    // touch Ll, CQ, allocator, client APIs or the list from this boundary.
    struct loom_service_operation *o=(void *)((u8 *)rpc-
                                  __builtin_offsetof(struct loom_service_operation,rpc));
    if(o->phase!=LRPC_INFLIGHT || o->callback_seen) {
        o->capture_error=-T_E_IO;return;
    }
    o->capture_error=loom_private_reply_capture(&o->expect,status,reply,
                                      o->payload,o->payload_capacity,&o->reply);
    o->callback_seen=true;
}
int loom_rpc_requeue_locked(struct loom_service_operation *o) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_INFLIGHT || !o->callback_seen ||
       o->capture_error || o->rpc.sending || r->phase!=LREQ_ACTIVE || r->borrows!=1)
        return -T_E_BUSY;
    int rc=rpc_ready(o);if(rc)return rc;
    o->rpc.on_complete=NULL;o->callback_seen=false;o->phase=LRPC_QUEUED;
    return 0;
}
int loom_rpc_settle_locked(struct loom_service_operation *o) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || r->phase!=LREQ_ACTIVE || r->borrows!=1 ||
       o->rpc.sending || (o->phase!=LRPC_QUEUED &&
         (o->phase!=LRPC_INFLIGHT || !o->callback_seen)))return -T_E_BUSY;
    // Sole driver has returned from client code. There can be no later callback
    // before either requeue or destruction; payload/FID release may now settle.
    o->rpc.on_complete=NULL;o->phase=LRPC_SETTLING;
    return 0;
}
int loom_rpc_check_finish_locked(struct loom_service_operation *o,s32 result,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_SETTLING ||
       r->phase!=LREQ_ACTIVE || r->borrows!=1 || o->rpc.on_complete || o->rpc.sending)
        return -T_E_BUSY;
    // Malformed or duplicate completion poisons the private scope. Do not
    // turn it into an isolated operation error while leaving the scope alive.
    if(result<0 && o->capture_error && !o->protocol->abort_reason)return -T_E_BUSY;
    if(result>=0) {
        if(!o->callback_seen || o->capture_error || o->reply.result<0)return -T_E_IO;
        int rc=rpc_ready(o);if(rc)return rc;
        (void)now;
    }
    return 0;
}
int loom_rpc_finish_locked(struct loom_service_operation *o,s32 result,u64 now) {
    int rc=loom_rpc_check_finish_locked(o,result,now);if(rc)return rc;
    struct Loom *l=o->owner;
    if(loom_request_put(&l->service_requests,o->request) ||
       loom_request_finish(&l->service_requests,l->service_slots,l->service_nodes,o->request,result))
        extinction("RPC lost settled terminal obligation");
    o->phase=LRPC_TERMINAL;
    return 0;
}
int loom_rpc_unlink_locked(struct loom_service_operation *o) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    struct loom_service_protocol *p=o->protocol;
    if(o->phase!=LRPC_TERMINAL || r->phase!=LREQ_DELIVERED || r->borrows ||
       o->rpc.on_complete || o->rpc.sending || !p->op_count)return -T_E_BUSY;
    if((o->prev ? o->prev->next!=o : p->op_head!=o) ||
       (o->next ? o->next->prev!=o : p->op_tail!=o))return -T_E_INVAL;
    if(o->prev)o->prev->next=o->next;else p->op_head=o->next;
    if(o->next)o->next->prev=o->prev;else p->op_tail=o->prev;
    --p->op_count;o->next=NULL;o->prev=NULL;o->phase=LRPC_RETIRED;
    return 0;
}
static struct Loom l;
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_request records[4];
static struct loom_service_protocol protocol;
static struct loom_service_ref scope;
static struct loom_service_operation a,b,c,d;
static unsigned checks;
#define C(x,m) do{if(!(x)){fprintf(stderr,"FAIL %s\n",m);return 1;}++checks;}while(0)
static void reset(void) {
 memset(&l,0,sizeof(l));memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));
 memset(records,0,sizeof(records));memset(&protocol,0,sizeof(protocol));
 memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));memset(&c,0,sizeof(c));memset(&d,0,sizeof(d));
 l.service_slots=&slots;l.service_nodes=&nodes;
 l.service_requests=(struct loom_request_table){.records=records,.capacity=4};
 if(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope) ||
    loom_service_node_publish(&slots,&nodes,scope))abort();
 nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 protocol.owner=&l;protocol.scope=scope;protocol.connect_bound=protocol.indexed=true;
}
static void prepare(struct loom_service_operation *o) {
 o->owner=&l;o->protocol=&protocol;o->scope=scope;
 if(loom_request_prepare(&l.service_requests,&slots,&nodes,scope,77,100,1,&o->request) ||
    loom_request_hold_owner(&l.service_requests,o->request))abort();
 o->expect.kind=P9_TREAD;o->expect.fid=7;o->expect.count=4;
}
static void capture(struct loom_service_operation *o) {
 u8 bytes[4]={1,2,3,4};
 struct p9_dispatch_result reply={.kind=P9_TREAD,.fid=7,.read_count=4,.read_data=bytes};
 loom_rpc_capture(&o->rpc,0,&reply);memset(bytes,0,sizeof(bytes));
}
int main(void) {
 reset();prepare(&a);prepare(&b);prepare(&c);
 u8 buf[4]={0};a.payload=buf;a.payload_capacity=4;
 struct loom_request_ref original=a.request;
 a.request.identity+=((u64)1<<32);
 C(!rpc_record(&a) && loom_rpc_enqueue_locked(&a,2)==-T_E_NOENT && !protocol.op_count,"full request identity");a.request=original;
 records[a.request.index].owners=0;
 C(loom_rpc_enqueue_locked(&a,2)==-T_E_NOENT,"physical owner required");records[a.request.index].owners=1;
 C(loom_rpc_enqueue_locked(&a,100)==-T_E_TIMEDOUT && !protocol.op_count && records[0].phase==LREQ_PREPARED,"expired enqueue unchanged");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ATTACH;
 C(loom_rpc_enqueue_locked(&a,2)==-T_E_CANCELED,"admission requires READY");nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 C(!loom_rpc_enqueue_locked(&a,2) && records[0].phase==LREQ_ACTIVE && records[0].borrows==1 && protocol.op_head==&a && protocol.op_tail==&a,"enqueue owns active borrow and list");
 C(loom_rpc_enqueue_locked(&a,2)==-T_E_BUSY && protocol.op_count==1,"duplicate enqueue denied");
 C(!loom_rpc_enqueue_locked(&b,2) && !loom_rpc_enqueue_locked(&c,2) && protocol.op_head==&a && protocol.op_tail==&c && a.next==&b && b.prev==&a && b.next==&c && c.prev==&b && protocol.op_count==3,"bounded doubly linked admission");
 prepare(&d);protocol.op_count=4;
 C(loom_rpc_enqueue_locked(&d,2)==-T_E_NOSPC && records[3].phase==LREQ_PREPARED,"queue capacity refusal");protocol.op_count=3;
 C(loom_rpc_begin_locked(&a,3)==-T_E_BUSY,"sole driver required");protocol.driving=true;
 C(loom_rpc_begin_locked(&a,100)==-T_E_TIMEDOUT && a.phase==LRPC_QUEUED,"expiry before wire");
 C(!loom_rpc_begin_locked(&a,3) && a.rpc.on_complete==loom_rpc_capture && a.phase==LRPC_INFLIGHT,"callback armed before submit");
 C(loom_rpc_begin_locked(&a,3)==-T_E_BUSY,"no duplicate submission");
 C(loom_rpc_settle_locked(&a)==-T_E_BUSY,"inflight callback retains owner");
 capture(&a);
 C(a.phase==LRPC_INFLIGHT && a.callback_seen && !a.capture_error && a.reply.result==4 && buf[0]==1 && buf[3]==4 && records[0].phase==LREQ_ACTIVE && records[0].borrows==1,"callback copies bytes without terminal");
 a.rpc.sending=true;
 C(loom_rpc_settle_locked(&a)==-T_E_BUSY,"pending sender blocks settlement");a.rpc.sending=false;
 C(!loom_rpc_settle_locked(&a) && !a.rpc.on_complete && a.phase==LRPC_SETTLING,"client returned before settling");
 C(loom_rpc_begin_locked(&a,3)==-T_E_BUSY,"settling cannot resubmit");
 C(!loom_request_borrow(&l.service_requests,&slots,&nodes,a.request),"extra physical borrower");
 C(loom_rpc_check_finish_locked(&a,4,4)==-T_E_BUSY && loom_rpc_finish_locked(&a,4,4)==-T_E_BUSY,"all physical borrowers must end");
 C(!loom_request_put(&l.service_requests,a.request),"extra borrow returns");
 C(loom_rpc_finish_locked(&a,4,100)==-T_E_TIMEDOUT && records[0].phase==LREQ_ACTIVE && records[0].borrows==1,"deadline beats captured success");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;nodes.nodes[scope.slot].reason=-T_E_TIMEDOUT;
 C(loom_rpc_finish_locked(&a,4,5)==-T_E_CANCELED && records[0].phase==LREQ_ACTIVE,"abort beats captured success");
 C(!loom_rpc_finish_locked(&a,-T_E_TIMEDOUT,100) && a.phase==LRPC_TERMINAL && records[0].result==-T_E_TIMEDOUT && !records[0].borrows && records[0].owners==1,"negative terminal ends borrow retains storage");
 C(loom_rpc_finish_locked(&a,4,5)==-T_E_BUSY && records[0].result==-T_E_TIMEDOUT,"terminal immutable");
 C(loom_rpc_unlink_locked(&a)==-T_E_BUSY && protocol.op_head==&a && protocol.op_count==3,"CQ backpressure retains discoverable owner");
 C(!loom_rpc_settle_locked(&b) && loom_rpc_finish_locked(&b,0,4)==-T_E_IO,"queued cancellation cannot fabricate success");
 C(!loom_rpc_finish_locked(&b,-T_E_CANCELED,4),"queued cancellation terminal");
 C(!loom_request_deliver(&l.service_requests,b.request),"middle terminal delivered");
 C(!loom_rpc_unlink_locked(&b) && a.next==&c && c.prev==&a && protocol.op_count==2 && b.phase==LRPC_RETIRED,"middle unlink retains neighbors");
 C(loom_request_reap(&l.service_requests,&slots,b.request)==-T_E_BUSY,"delivery and unlink do not free physical owner");
 C(!loom_request_drop_owner(&l.service_requests,b.request) && !loom_request_reap(&l.service_requests,&slots,b.request),"physical destruction permits record reuse");
 C(loom_rpc_unlink_locked(&b)==-T_E_NOENT,"stale unlinked request denied");
 C(!loom_request_deliver(&l.service_requests,a.request) && !loom_rpc_unlink_locked(&a) && protocol.op_head==&c && !c.prev && protocol.op_tail==&c,"head unlink");
 C(!loom_rpc_settle_locked(&c) && !loom_rpc_finish_locked(&c,-T_E_CANCELED,5),"last queued cancel");
 slots.closing=true;l.service_closing=true;
 C(!loom_requests_detach(&l.service_requests,&slots) && !loom_request_discard(&l.service_requests,c.request),"closed consumer discard");
 C(!loom_rpc_unlink_locked(&c) && !protocol.op_head && !protocol.op_tail && !protocol.op_count,"last unlink empties list");
 reset();prepare(&a);a.payload=buf;a.payload_capacity=4;
 C(!loom_rpc_enqueue_locked(&a,2),"retry admission");protocol.driving=true;
 C(!loom_rpc_begin_locked(&a,3),"synchronous error arm");
 loom_rpc_capture(&a.rpc,-T_E_AGAIN,(void *)1);
 C(a.callback_seen && a.reply.result==-T_E_AGAIN && !a.capture_error,"synchronous error captures without parser");
 C(!loom_rpc_requeue_locked(&a) && !a.callback_seen && !a.rpc.on_complete && records[0].borrows==1 && protocol.op_count==1,"retry preserves one obligation and borrow");
 C(!loom_rpc_begin_locked(&a,4),"retry begins once");capture(&a);
 capture(&a);
 C(a.capture_error==-T_E_IO && a.reply.result==4,"duplicate callback fails closed");
 C(loom_rpc_requeue_locked(&a)==-T_E_BUSY,"invalid capture cannot requeue");
 C(!loom_rpc_settle_locked(&a) && loom_rpc_finish_locked(&a,4,5)==-T_E_IO,"invalid capture cannot commit success");
 C(loom_rpc_finish_locked(&a,-T_E_IO,5)==-T_E_BUSY,"malformed capture requires scope abort");
 protocol.abort_reason=-T_E_IO;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 C(!loom_rpc_finish_locked(&a,-T_E_IO,5),"invalid capture cancellation");
 reset();prepare(&a);a.payload=buf;a.payload_capacity=4;
 C(!loom_rpc_enqueue_locked(&a,2),"success admission");protocol.driving=true;
 C(!loom_rpc_begin_locked(&a,3),"success arm");capture(&a);
 C(!loom_rpc_settle_locked(&a) && !loom_rpc_check_finish_locked(&a,4,4) && !loom_rpc_finish_locked(&a,4,4) && records[0].result==4,"validated captured success commits");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 C(loom_rpc_finish_locked(&a,-T_E_CANCELED,5)==-T_E_BUSY && records[0].result==4,"earlier success survives abort");
 printf("PASS RPC owner: %u checks\n",checks);return 0;
}
