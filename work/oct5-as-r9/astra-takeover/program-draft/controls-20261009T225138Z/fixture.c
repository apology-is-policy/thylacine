#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_program.h>
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


static struct p9_client c;
static struct p9_client_progress progress;
static struct loom_service_program program;
#define w program.wire
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
    memset(&c,0,sizeof(c));memset(&progress,0,sizeof(progress));memset(&program,0,sizeof(program));memset(&rpc,0,sizeof(rpc));memset(&reply,0,sizeof(reply));
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
static int wire_baseline(void) {
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
static struct ProcAccessIdentity admitted;
static void start_program(u8 kind,u32 mode,u16 component_len) {
    setup(kind);admitted=(struct ProcAccessIdentity){.principal_id=101,.primary_gid=202};
    admitted.supp_gid_count=1;admitted.supp_gids[0]=303;
    w.mode=mode;w.component_len=component_len;
    if(component_len)memcpy(w.component,"abc",3);
    CHECK(!loom_private_program_start(&program,&admitted,kind),"program starts");
}
static void stat_reply(u32 mode,u32 uid,u32 gid,u8 type) {
    header(160,P9_RGETATTR,rpc.tag);put32(frame+7,P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID);
    frame[15]=type;put32(frame+28,mode);put32(frame+32,uid);put32(frame+36,gid);delivered(160);
}
static enum loom_program_action advance(void){return loom_private_program_reply(&program,&reply,capture_error);}
static void synthetic_stat(u32 mode,u32 uid,u32 gid,u8 type) {
    w.built=true;w.expect.kind=P9_TGETATTR;
    reply=(struct loom_service_reply){0};reply.attr.valid=P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID;
    reply.attr.mode=mode;reply.attr.uid=uid;reply.attr.gid=gid;reply.attr.qid.type=type;
}
int main(void) {
    wire_baseline();
    start_program(P9_TLOPEN,0,0);
    CHECK(program.phase==LPROGRAM_STAT&&w.kind==P9_TGETATTR,"open starts with stat");
    CHECK(!submit(),"open stat sent");stat_reply(0400,101,202,0);
    admitted.principal_id=999;admitted.caps=CAP_DAC_OVERRIDE;admitted.supp_gids[0]=404;
    CHECK(program.identity.principal_id==101&&!program.identity.caps&&program.identity.supp_gids[0]==303,"admission snapshot detached");
    CHECK(advance()==LPROGRAM_NEXT&&w.kind==P9_TLOPEN&&program.phase==LPROGRAM_EFFECT,"authorized stat advances open");
    CHECK(!submit(),"authorized open sent");header(24,P9_RLOPEN,rpc.tag);delivered(24);
    CHECK(advance()==LPROGRAM_TERMINAL&&!program.result&&program.phase==LPROGRAM_DONE,"open terminal remains staged");
    CHECK(advance()==LPROGRAM_ABORT,"duplicate program result rejected");
    start_program(P9_TWALK,0,3);CHECK(w.kind==P9_TGETATTR,"component walk starts with stat");
    CHECK(!submit(),"walk stat sent");stat_reply(0100,101,202,P9_QTDIR);
    CHECK(advance()==LPROGRAM_NEXT&&w.kind==P9_TWALK,"search permits walk stage");
    CHECK(!submit(),"authorized walk sent");header(22,P9_RWALK,rpc.tag);frame[7]=1;delivered(22);
    CHECK(advance()==LPROGRAM_TERMINAL&&!program.result,"walk success needs owner publication");
    start_program(P9_TWALK,0,0);CHECK(w.kind==P9_TWALK&&program.phase==LPROGRAM_EFFECT,"clone has no search traversal");
    CHECK(!submit(),"clone sent");header(9,P9_RWALK,rpc.tag);delivered(9);CHECK(advance()==LPROGRAM_TERMINAL,"clone completed");
    start_program(P9_TLOPEN,1,0);CHECK(!submit(),"denied open stat sent");stat_reply(0400,101,202,0);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES&&send_calls==1,"denied open sends no effect");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,999,999,P9_QTSYMLINK);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_LOOP,"symlink open refused before DAC");
    start_program(P9_TLOPEN,0,0);program.identity.caps=CAP_DAC_OVERRIDE;synthetic_stat(0777,999,999,P9_QTSYMLINK);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_LOOP,"override cannot bypass symlink");
    start_program(P9_TWALK,0,3);synthetic_stat(0,999,999,0);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_NOTDIR,"directory type before search");
    start_program(P9_TWALK,0,3);synthetic_stat(0,101,202,P9_QTDIR);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES,"search requires execute");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,101,202,0);reply.attr.valid&=~P9_GETATTR_GID;
    CHECK(advance()==LPROGRAM_ABORT&&program.result==-T_E_IO,"missing stat field poisons scope");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,101,202,0);capture_error=-T_E_IO;
    CHECK(advance()==LPROGRAM_ABORT,"malformed capture poisons scope");
    start_program(P9_TLOPEN,0,0);reply.result=-T_E_ACCES;
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES,"remote stat error terminal");
    start_program(P9_TWRITE,0,0);w.count=3;send_rc=P9_TRANSPORT_EAGAIN;
    CHECK(submit()==-P9_E_AGAIN&&advance()==LPROGRAM_NEXT&&program.phase==LPROGRAM_EFFECT,"local shortage keeps effect phase");
    send_rc=0;CHECK(!submit(),"retried write accepted");header(11,P9_RLERROR,rpc.tag);put32(frame+7,T_E_AGAIN);delivered(11);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_AGAIN,"remote shortage terminal no replay");
    start_program(P9_TREAD,0,0);w.count=3;CHECK(!submit(),"read direct effect");header(13,P9_RREAD,rpc.tag);put32(frame+7,2);memcpy(frame+11,"xy",2);delivered(13);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==2,"short read preserved");
    start_program(P9_TREAD,0,0);reply.result=0;w.expect.kind=P9_TREAD;
    CHECK(advance()==LPROGRAM_ABORT,"uncaptured success rejected");
    start_program(P9_TREAD,0,0);w.built=true;w.expect.kind=P9_TWRITE;
    CHECK(advance()==LPROGRAM_ABORT,"wrong frame expectation rejected");
    start_program(P9_TLOPEN,0,0);synthetic_stat(0777,101,202,0);reply.result=1;
    CHECK(advance()==LPROGRAM_ABORT,"positive stat result rejected");
    setup(0);admitted.principal_id=0;CHECK(loom_private_program_start(&program,&admitted,P9_TREAD)==-T_E_INVAL,"invalid principal refused");
    admitted.principal_id=101;admitted.supp_gid_count=PROC_SUPP_GIDS_MAX+1;CHECK(loom_private_program_start(&program,&admitted,P9_TREAD)==-T_E_INVAL,"invalid group snapshot refused");
    admitted.supp_gid_count=0;CHECK(loom_private_program_start(&program,&admitted,P9_TGETATTR)==-T_E_OPNOTSUPP,"user stat stage not an operation");
    w.mode=3;CHECK(loom_private_program_start(&program,&admitted,P9_TLOPEN)==-T_E_INVAL,"unallocated open mode refused");
    start_program(P9_TREAD,0,0);CHECK(loom_private_program_start(&program,&admitted,P9_TREAD)==-T_E_INVAL,"cannot recapture admitted identity");
    // Independent POSIX bit oracle, all 512 modes and owner/group/other axes.
    // Owner deliberately also matches group: owner-first must not fall through.
    for(unsigned mode=0;mode<512;++mode)for(unsigned axis=0;axis<3;++axis)for(unsigned open=0;open<3;++open) {
        start_program(P9_TLOPEN,open,0);
        u32 uid=axis==0?101:999,gid=axis<2?202:999;
        synthetic_stat(mode,uid,gid,0);
        unsigned bits=(mode>>(axis==0?6:axis==1?3:0))&7;
        unsigned need=open==0?4:open==1?2:6;
        bool allowed=(bits&need)==need;
        enum loom_program_action a=advance();
        CHECK(allowed?(a==LPROGRAM_NEXT):(a==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES),"POSIX open permission matrix");
    }
    start_program(P9_TLOPEN,0,0);synthetic_stat(0040,999,303,0);CHECK(advance()==LPROGRAM_NEXT,"captured supplementary group");
    start_program(P9_TLOPEN,0,0);program.identity.principal_id=PRINCIPAL_SYSTEM;synthetic_stat(0,999,999,0);
    CHECK(advance()==LPROGRAM_TERMINAL&&program.result==-T_E_ACCES,"system principal has no DAC bypass");
    u64 caps[]={CAP_DAC_OVERRIDE,CAP_HOSTOWNER};
    for(unsigned i=0;i<2;++i){start_program(P9_TLOPEN,2,0);program.identity.caps=caps[i];synthetic_stat(0,999,999,0);CHECK(advance()==LPROGRAM_NEXT,"captured capability override");}
    printf("PASS staged program: %d total checks\n",checks);return 0;
}
