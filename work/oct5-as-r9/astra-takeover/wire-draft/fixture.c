#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_wire.h>
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
static struct p9_client c;
static struct p9_client_progress progress;
static struct loom_service_wire w;
static struct p9_rpc rpc;
static struct loom_service_reply reply;
static u8 output[8192],receive[8192],payload[8192],frame[512];
static int send_rc,send_calls,callbacks,capture_error;
static int send_fn(void *ctx,const u8 *b,size_t n){(void)ctx;(void)b;++send_calls;return send_rc?send_rc:(int)n;}
static int recv_fn(void *ctx,u8 *b,size_t n){(void)ctx;(void)b;(void)n;return -1;}
static int close_fn(void *ctx){(void)ctx;return 0;}
static void abort_fn(void *ctx){(void)ctx;}
static void capture(struct p9_rpc *r,int status,struct p9_dispatch_result *d) {
    CHECK(r==&rpc,"exact callback owner");++callbacks;
    capture_error=loom_private_reply_capture(&w.expect,status,d,payload,sizeof(payload),&reply);
}
static u32 le32(const u8 *b){return (u32)b[0]|(u32)b[1]<<8|(u32)b[2]<<16|(u32)b[3]<<24;}
static u64 le64(const u8 *b){return (u64)le32(b)|((u64)le32(b+4)<<32);}
static void put32(u8 *b,u32 v){for(unsigned i=0;i<4;++i)b[i]=(u8)(v>>(8*i));}
static void header(unsigned size,unsigned kind,unsigned tag){memset(frame,0,sizeof(frame));put32(frame,size);frame[4]=kind;frame[5]=tag;frame[6]=tag>>8;}
static void setup(unsigned kind) {
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
    rpc.on_complete=capture;
}
static void enable_progress(void) {
    struct p9_transport_try_ops ops={.send=send_fn,.recv=recv_fn,.abort=abort_fn};
    CHECK(!p9_transport_progress_init(&progress.io,&c.transport,ops,4096),"real progress init");
    c.progress=&progress;
}
static int submit(void){return loom_private_wire_submit(&c,&rpc,&w);}
static void delivered(unsigned length) {
    struct p9_dispatch_result d;
    CHECK(!p9_session_dispatch_rmsg(&c.session,frame,length,&d),"actual reply dispatch");
    c.inflight[rpc.tag]=NULL;
    capture(&rpc,d.is_error?-(int)d.ecode:0,&d);
}
int main(void) {
    setup(P9_TREAD);w.count=7000;w.offset=0x123456789abcdULL;
    CHECK(!submit() && !callbacks,"accepted read waits for reply");
    CHECK(w.expect.count==4085 && le32(output+19)==4085,"read negotiated bound");
    CHECK(le32(output+7)==73 && le64(output+11)==w.offset,"exact fid offset");
    CHECK(w.expect.new_fid==73 && w.expect.fid==73,"nonwalk expectation identity");
    CHECK(c.inflight[rpc.tag]==&rpc && !w.local_again,"actual accepted owner");
    header(14,P9_RREAD,rpc.tag);put32(frame+7,3);memcpy(frame+11,"abc",3);delivered(14);
    CHECK(!capture_error && reply.result==3 && !memcmp(payload,"abc",3),"read captured matching expectation");
    CHECK(!loom_private_wire_retry(&w,0,&reply),"success cannot retry");
    setup(P9_TWRITE);memset(payload,0x5a,sizeof(payload));w.count=5000;w.offset=123;
    CHECK(!submit(),"write submit");
    CHECK(w.expect.count==4073 && le32(output)==4096 && le32(output+19)==4073,"write negotiated bound");
    CHECK(output[23]==0x5a && output[4095]==0x5a,"write copied payload");
    header(11,P9_RWRITE,rpc.tag);put32(frame+7,4000);delivered(11);
    CHECK(!capture_error && reply.result==4000,"short write capture");
    setup(P9_TWRITE);c.out_buf_cap=100;w.count=500;
    CHECK(!submit() && w.expect.count==77,"write output capacity clamp");
    setup(P9_TREAD);w.count=600;w.iounit=91;
    CHECK(!submit() && w.expect.count==91,"iounit bound");
    setup(P9_TREAD);w.count=0;w.payload=NULL;w.payload_capacity=0;
    CHECK(!submit() && w.expect.count==0,"zero length no pointer");
    setup(P9_TREAD);w.count=12;w.payload_capacity=11;
    CHECK(submit()==-P9_E_IO && callbacks==1 && !w.built && !send_calls,"capacity refusal before effect");
    setup(P9_TWRITE);w.count=1;w.payload=NULL;
    CHECK(submit()==-P9_E_IO && !send_calls,"null payload refusal");
    setup(P9_TREAD);w.count=1;w.offset=UINT64_MAX;
    CHECK(submit()==-P9_E_IO && !send_calls,"offset overflow refusal");
    setup(P9_TWRITE);w.count=1;c.out_buf_cap=23;
    CHECK(submit()==-P9_E_IO && !send_calls,"nonzero degenerate bound refusal");
    setup(P9_TWALK);w.component_len=3;memcpy(w.component,"abc",3);
    CHECK(!submit() && output[15]==1 && output[17]==3 && !memcmp(output+19,"abc",3),"copied component frame");
    CHECK(w.expect.new_fid==99 && w.expect.walk_names==1 && c.session.n_reserved_slots==1,"walk exact reservation");
    header(22,P9_RWALK,rpc.tag);frame[7]=1;frame[9]=0x80;delivered(22);
    CHECK(!capture_error && p9_session_fid_bound(&c.session,99),"walk real bind and capture");
    setup(P9_TWALK);
    CHECK(!submit() && w.expect.walk_names==0 && output[15]==0,"clone zero names");
    header(9,P9_RWALK,rpc.tag);delivered(9);
    CHECK(!capture_error && p9_session_fid_bound(&c.session,99),"clone real bind");
    const char *bad[]={".","..","a/b"};
    for(unsigned i=0;i<3;++i){setup(P9_TWALK);w.component_len=strlen(bad[i]);memcpy(w.component,bad[i],w.component_len);CHECK(submit()==-P9_E_IO&&!send_calls,"invalid component refused");}
    setup(P9_TWALK);w.component_len=256;CHECK(submit()==-P9_E_IO&&!send_calls,"long component refused");
    setup(P9_TWALK);w.component_len=2;w.component[0]='a';CHECK(submit()==-P9_E_IO&&!send_calls,"nul component refused");
    setup(P9_TLOPEN);w.mode=2;CHECK(!submit()&&le32(output+11)==2,"open mode exact");
    header(24,P9_RLOPEN,rpc.tag);put32(frame+20,100);delivered(24);
    CHECK(!capture_error && reply.iounit==100,"open capture");
    setup(P9_TLOPEN);w.mode=3;CHECK(submit()==-P9_E_IO&&!send_calls,"open mode refusal");
    setup(P9_TGETATTR);CHECK(!submit(),"getattr submit");
    CHECK(w.expect.attr_mask==(P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID)&&le64(output+11)==w.expect.attr_mask,"complete DAC attribute mask");
    header(160,P9_RGETATTR,rpc.tag);put32(frame+7,(u32)w.expect.attr_mask);put32(frame+28,0750);put32(frame+32,42);put32(frame+36,7);delivered(160);
    CHECK(!capture_error && reply.attr.mode==0750 && reply.attr.uid==42 && reply.attr.gid==7,"DAC attributes captured");
    setup(P9_TGETATTR);CHECK(!submit(),"getattr missing mask submit");header(160,P9_RGETATTR,rpc.tag);put32(frame+7,P9_GETATTR_MODE);delivered(160);
    CHECK(capture_error==-T_E_IO,"incomplete DAC attributes malformed");
    setup(P9_TCLUNK);CHECK(!submit()&&!p9_session_fid_bound(&c.session,73),"clunk actual unbind");
    header(7,P9_RCLUNK,rpc.tag);delivered(7);CHECK(!capture_error,"clunk capture");
    setup(P9_TCLUNK);w.fid=41;CHECK(submit()==-P9_E_IO&&!send_calls,"root clunk refused");
    setup(0);CHECK(submit()==-P9_E_IO&&!send_calls,"unknown program step refused");
    setup(P9_TWRITE);w.count=3;send_rc=P9_TRANSPORT_EAGAIN;
    CHECK(submit()==-P9_E_AGAIN && callbacks==1 && w.local_again,"synchronous local shortage");
    CHECK(!p9_session_inflight(&c.session)&&!c.inflight[rpc.tag],"unsent request retracted");
    CHECK(loom_private_wire_retry(&w,capture_error,&reply),"local no wire retry proof");
    CHECK(!loom_private_wire_retry(&w,-T_E_IO,&reply),"malformed capture bars retry");
    send_rc=0;CHECK(!submit()&&!w.local_again,"new attempt clears retry proof");
    header(11,P9_RLERROR,rpc.tag);put32(frame+7,T_E_AGAIN);delivered(11);
    CHECK(reply.result==-T_E_AGAIN&&!loom_private_wire_retry(&w,capture_error,&reply),"remote EAGAIN never replayed");
    setup(P9_TCLUNK);send_rc=P9_TRANSPORT_EAGAIN;
    CHECK(submit()==-P9_E_AGAIN&&p9_session_fid_bound(&c.session,73)&&!c.session.n_reserved_slots,"clunk retraction restores binding");
    setup(P9_TWRITE);w.count=3;enable_progress();progress.io.tx=output;
    CHECK(submit()==-P9_E_AGAIN&&!w.built&&!send_calls&&loom_private_wire_retry(&w,capture_error,&reply),"pending tx local retry");
    setup(P9_TWRITE);w.count=3;
    for(unsigned i=0;i<P9_SESSION_MAX_OUTSTANDING;++i)c.session.outstanding[i].active=true;
    CHECK(submit()==-P9_E_AGAIN&&!w.built&&!send_calls,"full tag local retry");
    setup(P9_TWRITE);w.count=3;enable_progress();
    CHECK(!submit()&&rpc.sending&&progress.sending==&rpc&&progress.io.tx==output&&!send_calls,"progress submission retains frame");
    setup(P9_TREAD);rpc.on_complete=NULL;w.local_again=true;
    CHECK(submit()==-P9_E_INVAL&&!callbacks&&!w.local_again,"invalid callback not accepted");
    setup(P9_TREAD);c.magic=0;CHECK(submit()==-P9_E_INVAL&&!callbacks,"invalid client not accepted");
    setup(P9_TREAD);c.dead=true;CHECK(submit()==-P9_E_IO&&callbacks==1&&!w.local_again,"dead client immediate error");
    setup(P9_TWRITE);w.count=3;send_rc=-1;CHECK(submit()==-P9_E_IO&&callbacks==1&&c.dead&&!w.local_again,"broken send not retry");
    printf("PASS wire adapter: %d checks\n",checks);return 0;
}
