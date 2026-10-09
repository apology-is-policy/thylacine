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
/* ACTUAL_SUBMIT */
struct Loom {struct loom_slot_table *service_slots;struct loom_service_node_table *service_nodes;struct loom_request_table service_requests;bool service_closing;};
static struct Loom l;
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_request records[4];
static struct loom_service_protocol protocol;
static struct loom_service_ref scope;
static struct loom_service_operation owner;
static void extinction(const char *m){fprintf(stderr,"INVARIANT %s\n",m);exit(86);}
/* ACTUAL_RPC */
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

    setup(P9_TREAD);w.count=1;u8 first[1]={0},second[1]={0};w.payload=first;w.payload_capacity=1;
    start(P9_TREAD);CHECK(!submit(),"first interleaved submit");
    struct loom_service_operation other={0};other.owner=&l;other.protocol=&protocol;other.scope=scope;
    other.program.wire.fid=73;other.program.wire.count=1;other.program.wire.payload=second;other.program.wire.payload_capacity=1;
    struct ProcAccessIdentity id={.principal_id=51};
    CHECK(!loom_private_program_start(&other.program,&id,P9_TREAD),"second program");
    CHECK(!loom_request_prepare(&l.service_requests,&slots,&nodes,scope,78,100,1,&other.request) && !loom_request_hold_owner(&l.service_requests,other.request),"second request");
    CHECK(!loom_rpc_enqueue_locked(&other,2) && !loom_rpc_begin_locked(&other,3) && !loom_rpc_submit(&other),"second interleaved submit");
    // Real session dispatch routes the second reply first; retained containers
    // must not confuse either tag, expectation, payload or request ledger.
    #undef rpc
    struct p9_rpc *other_rpc=&other.rpc;
    #define rpc owner.rpc
    header(12,P9_RREAD,other_rpc->tag);put32(frame+7,1);frame[11]='B';
    struct p9_dispatch_result dr;
    CHECK(!p9_session_dispatch_rmsg(&c.session,frame,12,&dr),"second reply dispatch");
    c.inflight[other_rpc->tag]=NULL;loom_rpc_capture(other_rpc,0,&dr);
    CHECK(second[0]=='B' && first[0]==0 && !owner.callback_seen && protocol.op_count==2,"interleaved capture keeps exact owner");
    CHECK(!loom_rpc_advance(&other,&action) && action==LPROGRAM_TERMINAL && !loom_rpc_settle_locked(&other) && !loom_rpc_finish_locked(&other,1,4),"second operation completes first");
    CHECK(!loom_request_deliver(&l.service_requests,other.request) && !loom_rpc_unlink_locked(&other) && protocol.op_head==&owner && protocol.op_tail==&owner,"interleaved tail unlink");
    header(12,P9_RREAD,rpc.tag);put32(frame+7,1);frame[11]='A';delivered(12);
    CHECK(first[0]=='A' && second[0]=='B' && advance()==LPROGRAM_TERMINAL,"first reply preserves second payload");
    CHECK(!loom_rpc_settle_locked(&owner) && !loom_rpc_finish_locked(&owner,1,5),"first operation completes second");
    printf("PASS unified owner/program: %d checks\n",checks);return 0;
}
