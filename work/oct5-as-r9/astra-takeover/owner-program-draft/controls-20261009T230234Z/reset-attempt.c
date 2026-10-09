#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_rpc.h>
#include <thylacine/errno.h>
static int checks;
#define CHECK(x,m) do {if(!(x)){fprintf(stderr,"FAIL %s\n",m);exit(1);}++checks;}while(0)
// Explicit host boundary: lock assertions are sequential, wake signal is inert,
// and death completion is a stand-in. Production submit function is extracted
// verbatim below; session/codec/transport/capture are separately linked unchanged.
static void client_send_progress_signal(struct p9_client *c){(void)c;}
static void client_mark_dead_locked(struct p9_client *c,bool gone) {
    (void)gone;c->dead=true;
    for(unsigned i=0;i<P9_SESSION_MAX_OUTSTANDING;++i)if(c->inflight[i]) {
        struct p9_rpc *r=c->inflight[i];c->inflight[i]=NULL;r->sending=false;
        r->on_complete(r,-P9_E_IO,NULL);
    }
}
int p9_client_submit_async(struct p9_client *c, struct p9_rpc *rpc,
                           p9_session_build_fn build, void *build_ctx) {
    // Reject WITHOUT firing on_complete: nothing was taken over yet, and an
    // absent on_complete is precisely what we'd need to complete the op.
    if (!c || c->magic != P9_CLIENT_MAGIC)     return -P9_E_INVAL;
    if (!rpc || !rpc->on_complete || !build)   return -P9_E_INVAL;

    spin_lock(&c->lock);
    if (c->dead || !p9_session_is_open(&c->session)) {
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_IO, NULL);   // own it: complete + bail
        return -P9_E_IO;
    }
    if (c->progress && c->progress->io.tx) {
        // The shared out_buf is immutable until the partial frame is sent.
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_AGAIN, NULL);
        return -P9_E_AGAIN;
    }
    // A full tag pool is a shortage, not a failure of this op: the sync path
    // drains a tag (client_drain_until_free_tag), but an async submitter must
    // not block, so the op completes with the retryable -P9_E_AGAIN before
    // anything is built -- a build failure would read as -EIO.
    if (!p9_session_has_free_tag(&c->session)) {
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_AGAIN, NULL);
        return -P9_E_AGAIN;
    }
    // Build the Tmsg into the shared out_buf under the lock (allocating the tag
    // + marking session.outstanding[tag]); a >0 return is a well-formed frame.
    int built = build(&c->session, c->out_buf, c->out_buf_cap, build_ctx);
    if (built <= 0) {
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_IO, NULL);
        return -P9_E_IO;
    }
    u32 size; u8 type; u16 tag;
    if (p9_peek_header(c->out_buf, (size_t)built, &size, &type, &tag) < 0 ||
        tag == P9_NOTAG || tag >= P9_SESSION_MAX_OUTSTANDING) {
        // Unreachable for a conforming builder (a session_send_* writes a valid
        // tagged frame). If it ever fires, `build` already marked an outstanding
        // tag we cannot track (the header is unparseable / NOTAG / out of range),
        // so latch the session dead -- fail closed; the orphaned outstanding tag
        // is then moot (the session is unusable) -- and complete this op. `rpc`
        // is not yet registered, so mark_dead does not double-fire it.
        client_mark_dead_locked(c, false);
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_IO, NULL);
        return -P9_E_IO;
    }
    rpc->tag       = (u16)tag;
    rpc->done      = false;
    rpc->dead      = false;
    rpc->be_reader = false;
    rpc->reply_len = 0;
    rpc->reply_buf = NULL;          // async: demux dispatches from recv_buf
    rpc->owner     = NULL;          // 8c-3 (#89): async ops are skipped in the
                                    // handoff (on_complete != NULL first), so
                                    // owner is never read -- keep it non-garbage
    rpc->noted     = false;         // flush(5) state is sync-only: kept inert
    rpc->flushing  = false;
    rpc->honoured  = false;
    rpc->flushed   = false;
    rpc->honour_rc = 0;
    rpc->flush_out = NULL;
    rpc->sending   = false;
    rendez_init(&rpc->rendez);      // unused for async, but kept inert
    c->inflight[tag] = rpc;

    if (c->progress) {
        rpc->sending = true;
        c->progress->sending = rpc;
        if (p9_transport_progress_queue(&c->progress->io, c->out_buf,
                                        (size_t)built) < 0) {
            client_mark_dead_locked(c, false);
            spin_unlock(&c->lock);
            return -P9_E_IO;
        }
        c->total_ops++;
        spin_unlock(&c->lock);
        return 0;
    }

    int src = p9_transport_send(&c->transport, c->out_buf, (size_t)built);
    if (src == P9_TRANSPORT_EAGAIN) {
        // A full send ring is back-pressure, not a break, as on the sync path
        // (client_send_flow) and the abandon Tflush. The transport pushed zero
        // bytes, so the server never saw this op: take it back whole -- the
        // tag (never-sent, so I-10-safe) and a Tclunk's fid -- and leave the
        // SHARED session live. The submitter cannot wait the ring out, so the
        // op completes with the retryable -P9_E_AGAIN.
        c->inflight[tag] = NULL;
        p9_session_retract_unsent(&c->session, tag);
        client_send_progress_signal(c);   // a tag drain parks for a freed tag
        spin_unlock(&c->lock);
        rpc->on_complete(rpc, -P9_E_AGAIN, NULL);
        return -P9_E_AGAIN;
    }
    if (src < 0) {
        // The byte stream is broken: latch dead + complete every in-flight op.
        // `rpc` is registered, so mark_dead fires its on_complete (error CQE).
        client_mark_dead_locked(c, false);
        spin_unlock(&c->lock);
        return -P9_E_IO;
    }
    c->total_ops++;
    spin_unlock(&c->lock);
    return 0;
}


struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;struct loom_request_table service_requests;bool service_closing;};
static struct Loom l;
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_request records[4];
static struct loom_service_protocol protocol;
static struct loom_service_ref scope;
static struct loom_service_operation owner;
static void extinction(const char *m){fprintf(stderr,"INVARIANT %s\n",m);exit(86);}
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
    o->submitted=false;
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
    o->capture_error=loom_private_reply_capture(&o->program.wire.expect,status,reply,
                                      o->program.wire.payload,o->program.wire.payload_capacity,&o->reply);
    o->callback_seen=true;
}
int loom_rpc_requeue_locked(struct loom_service_operation *o,u64 now) {
    struct loom_service_request *r=rpc_record(o);
    if(!r)return -T_E_NOENT;
    if(!o->protocol->driving || o->phase!=LRPC_INFLIGHT || !o->callback_seen ||
       o->capture_error || o->rpc.sending || !o->advanced || o->action!=LPROGRAM_NEXT ||
       r->phase!=LREQ_ACTIVE || r->borrows!=1)
        return -T_E_BUSY;
    int rc=rpc_ready(o);if(rc)return rc;
    if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;
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
    if(result<0 && (o->capture_error || (o->advanced && o->action==LPROGRAM_ABORT)) &&
       !o->protocol->abort_reason)return -T_E_BUSY;
    if(result>=0) {
        if(!o->callback_seen || o->capture_error || o->reply.result<0 ||
           !o->advanced || o->action!=LPROGRAM_TERMINAL ||
           o->program.phase!=LPROGRAM_DONE || o->program.result<0)return -T_E_IO;
        int rc=rpc_ready(o);if(rc)return rc;
        if(r->deadline && r->deadline<=now)return -T_E_TIMEDOUT;
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

// These two edges run outside Ll, under the exclusive p->driving claim. They
// may not release storage. A callback may occur synchronously inside submit.
int loom_rpc_submit(struct loom_service_operation *o) {
    if(!o || !o->protocol || !o->protocol->driving || o->phase!=LRPC_INFLIGHT ||
       o->rpc.on_complete!=loom_rpc_capture || o->submitted || o->callback_seen ||
       o->rpc.sending)return -T_E_BUSY;
    o->submitted=true;
    int rc=loom_private_wire_submit(&o->protocol->client,&o->rpc,&o->program.wire);
    // Invalid client arguments are the one refusal that does not callback.
    // Accepted negative completions already captured: never duplicate them.
    if(rc<0 && !o->callback_seen)loom_rpc_capture(&o->rpc,rc,NULL);
    return rc;
}
int loom_rpc_advance(struct loom_service_operation *o,enum loom_program_action *out) {
    if(!o || !out || !o->protocol || !o->protocol->driving ||
       o->phase!=LRPC_INFLIGHT || !o->submitted || !o->callback_seen ||
       o->rpc.sending || o->advanced)return -T_E_BUSY;
    o->action=loom_private_program_reply(&o->program,&o->reply,o->capture_error);
    o->advanced=true;*out=o->action;return 0;
}

#define c protocol.client
#define w owner.program.wire
#define rpc owner.rpc
#define reply owner.reply
#define capture_error owner.capture_error

static struct p9_client_progress progress;

static u8 output[8192],receive[8192],payload[8192],frame[512];
static int send_rc,send_calls,callbacks;
static int send_fn(void *ctx,const u8 *b,size_t n){(void)ctx;(void)b;++send_calls;return send_rc?send_rc:(int)n;}
static int recv_fn(void *ctx,u8 *b,size_t n){(void)ctx;(void)b;(void)n;return -1;}
static int close_fn(void *ctx){(void)ctx;return 0;}
static void abort_fn(void *ctx){(void)ctx;}
static void capture(struct p9_rpc *r,int status,struct p9_dispatch_result *d) {
    CHECK(r==&rpc,"exact callback owner");++callbacks;
    loom_rpc_capture(r,status,d);
}
static void put32(u8 *b,u32 v){for(unsigned i=0;i<4;++i)b[i]=(u8)(v>>(8*i));}
static void header(unsigned size,unsigned kind,unsigned tag){memset(frame,0,sizeof(frame));put32(frame,size);frame[4]=kind;frame[5]=tag;frame[6]=tag>>8;}
static void setup(unsigned kind) {
    memset(&protocol,0,sizeof(protocol));memset(&owner,0,sizeof(owner));
    memset(&l,0,sizeof(l));memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));memset(records,0,sizeof(records));
    l.service_slots=&slots;l.service_nodes=&nodes;
    l.service_requests=(struct loom_request_table){.records=records,.capacity=4};
    CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope) && !loom_service_node_publish(&slots,&nodes,scope),"scope setup");
    nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
    protocol.owner=&l;protocol.scope=scope;protocol.connect_bound=protocol.indexed=protocol.driving=true;
    owner.owner=&l;owner.protocol=&protocol;owner.scope=scope;
    CHECK(!loom_request_prepare(&l.service_requests,&slots,&nodes,scope,77,100,1,&owner.request) && !loom_request_hold_owner(&l.service_requests,owner.request),"request setup");
    memset(&c,0,sizeof(c));memset(&progress,0,sizeof(progress));memset(&w,0,sizeof(w));memset(&rpc,0,sizeof(rpc));memset(&reply,0,sizeof(reply));
    send_rc=send_calls=callbacks=capture_error=0;
    c.magic=P9_CLIENT_MAGIC;c.out_buf=output;c.out_buf_cap=sizeof(output);
    CHECK(!p9_session_init(&c.session,41,8192),"session init");
    // Explicit already-attached fixture; no handshake qualification claim.
    c.session.state=P9_SESS_OPEN;c.session.negotiated_msize=4096;
    c.session.bound_fids[0]=41;c.session.bound_fids[1]=73;c.session.n_bound_fids=2;
    struct p9_transport_ops ops={.send=send_fn,.recv=recv_fn,.close=close_fn};
    CHECK(!p9_transport_init(&c.transport,ops,receive,sizeof(receive)),"transport init");
    w.kind=kind;w.fid=73;w.new_fid=99;w.payload=payload;w.payload_capacity=sizeof(payload);

}
static void enable_progress(void) {
    struct p9_transport_try_ops ops={.send=send_fn,.recv=recv_fn,.abort=abort_fn};
    CHECK(!p9_transport_progress_init(&progress.io,&c.transport,ops,4096),"real progress init");
    c.progress=&progress;
}
static void start(unsigned kind) {
    struct ProcAccessIdentity id={.principal_id=42,.primary_gid=7};
    CHECK(!loom_private_program_start(&owner.program,&id,kind),"program start");
    CHECK(!loom_rpc_enqueue_locked(&owner,2),"owner admission");
    CHECK(!loom_rpc_begin_locked(&owner,3),"owner begin");
}
static int submit(void){return loom_rpc_submit(&owner);}
static enum loom_program_action advance(void) {
    enum loom_program_action action;
    CHECK(!loom_rpc_advance(&owner,&action),"advance completed attempt");return action;
}
static void delivered(unsigned length) {
    struct p9_dispatch_result d;
    CHECK(!p9_session_dispatch_rmsg(&c.session,frame,length,&d),"actual reply dispatch");
    c.inflight[rpc.tag]=NULL;
    capture(&rpc,d.is_error?-(int)d.ecode:0,&d);
}
int main(void) {
    setup(P9_TREAD);w.count=4;u8 retained[4]={0};w.payload=retained;w.payload_capacity=4;
    start(P9_TREAD);enum loom_program_action action=LPROGRAM_ABORT;
    CHECK(loom_rpc_advance(&owner,&action)==-T_E_BUSY && action==LPROGRAM_ABORT,"no advance before submission");
    CHECK(!submit() && c.inflight[rpc.tag]==&rpc && !owner.callback_seen,"actual retained submit");
    CHECK(submit()==-T_E_BUSY && send_calls==1,"one submission per attempt");
    CHECK(loom_rpc_advance(&owner,&action)==-T_E_BUSY,"no advance before callback");
    CHECK(loom_rpc_settle_locked(&owner)==-T_E_BUSY,"no early settlement");
    header(15,P9_RREAD,rpc.tag);put32(frame+7,4);memcpy(frame+11,"abcd",4);delivered(15);
    CHECK(!capture_error && reply.result==4 && !memcmp(retained,"abcd",4) && records[0].phase==LREQ_ACTIVE && records[0].borrows==1,"builder and capture share retained payload");
    CHECK(loom_rpc_requeue_locked(&owner,4)==-T_E_BUSY,"no unadvanced requeue");
    CHECK(advance()==LPROGRAM_TERMINAL,"read terminal program");
    CHECK(loom_rpc_advance(&owner,&action)==-T_E_BUSY && owner.program.result==4,"one advance per callback");
    CHECK(loom_rpc_requeue_locked(&owner,4)==-T_E_BUSY,"terminal program never requeues");
    CHECK(!loom_rpc_settle_locked(&owner),"settle after parser return");
    CHECK(loom_rpc_finish_locked(&owner,4,100)==-T_E_TIMEDOUT,"deadline beats success");
    CHECK(!loom_rpc_finish_locked(&owner,4,4) && records[0].result==4 && records[0].owners==1 && !records[0].borrows,"terminal retains physical owner");
    CHECK(loom_rpc_unlink_locked(&owner)==-T_E_BUSY,"CQ backpressure retains owner");
    CHECK(!loom_request_deliver(&l.service_requests,owner.request) && !loom_rpc_unlink_locked(&owner) && !protocol.op_count,"delivered owner unlinks");
    CHECK(loom_request_reap(&l.service_requests,&slots,owner.request)==-T_E_BUSY,"physical owner bars reuse");

    setup(P9_TLOPEN);w.mode=0;start(P9_TLOPEN);CHECK(!submit(),"open stat submit");
    header(160,P9_RGETATTR,rpc.tag);put32(frame+7,(u32)w.expect.attr_mask);put32(frame+28,0400);put32(frame+32,42);put32(frame+36,7);delivered(160);
    CHECK(advance()==LPROGRAM_NEXT && owner.program.phase==LPROGRAM_EFFECT,"stat advances without terminal");
    CHECK(loom_rpc_requeue_locked(&owner,100)==-T_E_TIMEDOUT && owner.phase==LRPC_INFLIGHT,"deadline bars next stage");
    CHECK(!loom_rpc_requeue_locked(&owner,4) && records[0].borrows==1,"same borrow through DAC stage");
    CHECK(!loom_rpc_begin_locked(&owner,5) && !owner.submitted && !owner.advanced,"new attempt resets consumed edges");
    CHECK(!submit() && output[4]==P9_TLOPEN && send_calls==2,"authorized effect once");
    header(24,P9_RLOPEN,rpc.tag);put32(frame+20,91);delivered(24);
    CHECK(advance()==LPROGRAM_TERMINAL && reply.iounit==91,"open captures same owner");
    CHECK(!loom_rpc_settle_locked(&owner) && !loom_rpc_finish_locked(&owner,0,6),"open terminal after staged effect");

    setup(P9_TLOPEN);start(P9_TLOPEN);CHECK(!submit(),"denied stat submit");
    header(160,P9_RGETATTR,rpc.tag);put32(frame+7,(u32)w.expect.attr_mask);put32(frame+28,0000);put32(frame+32,42);put32(frame+36,7);delivered(160);
    CHECK(advance()==LPROGRAM_TERMINAL && owner.program.result==-T_E_ACCES && send_calls==1,"denied DAC has no effect");
    CHECK(!loom_rpc_settle_locked(&owner) && loom_rpc_finish_locked(&owner,0,6)==-T_E_IO,"denied program cannot publish success");
    CHECK(!loom_rpc_finish_locked(&owner,-T_E_ACCES,6),"denied DAC terminal");

    setup(P9_TWRITE);w.count=3;start(P9_TWRITE);send_rc=P9_TRANSPORT_EAGAIN;
    CHECK(submit()==-T_E_AGAIN && owner.callback_seen && !capture_error && w.local_again,"synchronous refusal captured once");
    CHECK(advance()==LPROGRAM_NEXT && !loom_rpc_requeue_locked(&owner,4),"local no-wire retry");
    CHECK(!loom_rpc_begin_locked(&owner,5),"retry begin");send_rc=0;CHECK(!submit() && !w.local_again,"retry clears local proof");
    header(11,P9_RLERROR,rpc.tag);put32(frame+7,T_E_AGAIN);delivered(11);
    CHECK(advance()==LPROGRAM_TERMINAL && owner.program.result==-T_E_AGAIN && loom_rpc_requeue_locked(&owner,6)==-T_E_BUSY,"remote error never replays");

    setup(P9_TREAD);w.count=1;start(P9_TREAD);c.magic=0;
    CHECK(submit()==-T_E_INVAL && owner.callback_seen && !capture_error && reply.result==-T_E_INVAL && !send_calls,"invalid client refusal captured locally");
    CHECK(advance()==LPROGRAM_TERMINAL && !loom_rpc_settle_locked(&owner) && !loom_rpc_finish_locked(&owner,-T_E_INVAL,4),"local refusal settles without phantom owner");
    setup(P9_TREAD);start(P9_TREAD);c.dead=true;
    CHECK(submit()==-T_E_IO && owner.callback_seen && !capture_error,"accepted error not double captured");

    setup(P9_TREAD);w.count=1;start(P9_TREAD);enable_progress();
    CHECK(!submit() && rpc.sending && !owner.callback_seen,"progress sender retains owner");
    loom_rpc_capture(&rpc,-T_E_IO,NULL);
    CHECK(loom_rpc_advance(&owner,&action)==-T_E_BUSY && loom_rpc_settle_locked(&owner)==-T_E_BUSY,"sender blocks advance and settlement");
    rpc.sending=false;c.inflight[rpc.tag]=NULL;progress.sending=NULL;progress.io.tx=NULL;
    CHECK(advance()==LPROGRAM_TERMINAL,"quiesced error advances");

    setup(P9_TREAD);w.count=1;start(P9_TREAD);CHECK(!submit(),"malformed read submit");
    struct p9_dispatch_result bad={.kind=P9_TREAD,.fid=999,.new_fid=999};
    loom_rpc_capture(&rpc,0,&bad);c.inflight[rpc.tag]=NULL;
    CHECK(advance()==LPROGRAM_ABORT && !loom_rpc_settle_locked(&owner),"malformed capture demands abort");
    CHECK(loom_rpc_finish_locked(&owner,-T_E_IO,4)==-T_E_BUSY,"poison cannot finish without scope abort");
    protocol.abort_reason=-T_E_IO;CHECK(!loom_rpc_finish_locked(&owner,-T_E_IO,4),"abort permits negative terminal");

    setup(P9_TREAD);start(P9_TREAD);CHECK(!submit(),"duplicate callback submit");
    loom_rpc_capture(&rpc,-T_E_IO,NULL);loom_rpc_capture(&rpc,-T_E_IO,NULL);c.inflight[rpc.tag]=NULL;
    CHECK(capture_error==-T_E_IO && advance()==LPROGRAM_ABORT,"duplicate callback poisons scope");

    setup(P9_TLOPEN);start(P9_TLOPEN);CHECK(!submit(),"unadvanced stat submit");
    header(160,P9_RGETATTR,rpc.tag);put32(frame+7,(u32)w.expect.attr_mask);put32(frame+28,0400);put32(frame+32,42);put32(frame+36,7);delivered(160);
    CHECK(!loom_rpc_settle_locked(&owner) && loom_rpc_finish_locked(&owner,0,4)==-T_E_IO,"stat cannot bypass program advance");
    printf("PASS unified owner/program: %d checks\n",checks);return 0;
}
