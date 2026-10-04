// Actual transport + wire/session sources linked by test-9p-progress.py.
// Fake byte pipe controls every partial boundary. It cannot qualify SMP locks.
#include <thylacine/9p_transport.h>
#include <thylacine/9p_session.h>
#include <thylacine/errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x, why) do { if (!(x)) { fputs(why "\n", stderr); exit(1); } } while (0)
struct pipe {
    u8 input[1024], output[1024];
    size_t available, read, wrote, chunk;
    unsigned sends, recvs, aborts, closes;
    bool again, eof, oversize;
};
static int try_send(void *ctx, const u8 *buf, size_t len) {
    struct pipe *p=ctx;p->sends++;
    if (p->again) return P9_TRANSPORT_EAGAIN;
    if (p->eof) return 0;
    if (p->oversize) return (int)len+1;
    if (len>p->chunk) len=p->chunk;
    CHECK(p->wrote+len<=sizeof(p->output), "fixture output bound");
    memcpy(p->output+p->wrote,buf,len);p->wrote+=len;return (int)len;
}
static int try_recv(void *ctx,u8 *buf,size_t len) {
    struct pipe *p=ctx;p->recvs++;
    if(p->again)return P9_TRANSPORT_EAGAIN;
    if(p->oversize)return (int)len+1;
    if(p->read==p->available)return p->eof?0:P9_TRANSPORT_EAGAIN;
    if(len>p->chunk)len=p->chunk;
    if(len>p->available-p->read)len=p->available-p->read;
    memcpy(buf,p->input+p->read,len);p->read+=len;return (int)len;
}
static void abort_pipe(void *ctx) { ((struct pipe*)ctx)->aborts++; }
static int legacy_send(void *ctx,const u8 *buf,size_t n) {
    (void)ctx;(void)buf;(void)n;CHECK(false,"blocking send invoked");return -1;
}
static int legacy_recv(void *ctx,u8 *buf,size_t n) {
    (void)ctx;(void)buf;(void)n;CHECK(false,"blocking recv invoked");return -1;
}
static int close_pipe(void *ctx) { ((struct pipe*)ctx)->closes++;return 0; }
struct fixture { struct pipe pipe; struct p9_transport t; struct p9_transport_progress p;u8 buf[512]; };
static void init(struct fixture *f) {
    memset(f,0,sizeof(*f));f->pipe.chunk=512;
    struct p9_transport_ops ops={.send=legacy_send,.recv=legacy_recv,.close=close_pipe,.ctx=&f->pipe};
    struct p9_transport_try_ops nonblock={.send=try_send,.recv=try_recv,.abort=abort_pipe,.ctx=&f->pipe};
    CHECK(p9_transport_init(&f->t,ops,f->buf,sizeof(f->buf))==0,"transport init");
    CHECK(p9_transport_progress_init(&f->p,&f->t,nonblock,256)==0,"progress init");
}
static void frame(u8 *b,unsigned len) {
    for(unsigned i=0;i<len;i++)b[i]=(u8)(i*37+11);
    b[0]=(u8)len;b[1]=(u8)(len>>8);b[2]=b[3]=0;b[4]=P9_RREAD;b[5]=b[6]=0;
}
static void check_dead(struct fixture *f) {
    unsigned s=f->pipe.sends,r=f->pipe.recvs;
    p9_transport_progress_abort(&f->p);p9_transport_progress_abort(&f->p);
    CHECK(f->pipe.aborts==1,"abort must be exactly once");
    CHECK(p9_transport_progress_send(&f->p)<0 && p9_transport_progress_recv(&f->p)<0,"terminal must reject progress");
    CHECK(f->pipe.sends==s && f->pipe.recvs==r,"no I/O after terminal");
    CHECK(f->t.total_errors==1,"terminal error counted once");
    CHECK(p9_transport_close(&f->t)==0 && p9_transport_close(&f->t)==0 && f->pipe.closes==1,"close after abort exactly once");
}
static void send_boundaries(void) {
    u8 msg[67];frame(msg,sizeof(msg));
    for(size_t at=0;at<=sizeof(msg);at++) {
        struct fixture f;init(&f);
        CHECK(p9_transport_progress_queue(&f.p,msg,sizeof(msg))==0,"queue valid");
        CHECK(p9_transport_progress_queue(&f.p,msg,sizeof(msg))<0,"do not replace queued borrow");
        f.pipe.chunk=1;
        for(size_t i=0;i<at;i++) {
            unsigned before=f.pipe.sends;
            int rc=p9_transport_progress_send(&f.p);
            CHECK(f.pipe.sends==before+1,"one send callback per step");
            CHECK(rc==(i+1==sizeof(msg)?1:0),"send completeness");
        }
        if(at<sizeof(msg)) {
            f.pipe.again=true;
            CHECK(p9_transport_progress_send(&f.p)==0,"EAGAIN retains send");
            CHECK(f.p.tx_sent==at && f.pipe.wrote==at,"EAGAIN preserves send cursor");
            f.pipe.again=false;f.pipe.chunk=512;
            CHECK(p9_transport_progress_send(&f.p)==1,"send resumes suffix");
        }
        CHECK(f.pipe.wrote==sizeof(msg)&&memcmp(f.pipe.output,msg,sizeof(msg))==0,"send bytes exactly once");
        CHECK(f.p.bytes_sent && !f.p.tx && f.t.total_sent==1,"send borrow released");
        p9_transport_progress_abort(&f.p);check_dead(&f);
    }
    for(size_t at=0;at<sizeof(msg);at++) {
        struct fixture f;init(&f);f.pipe.chunk=1;
        CHECK(p9_transport_progress_queue(&f.p,msg,sizeof(msg))==0,"queue cancel");
        for(size_t i=0;i<at;i++)CHECK(p9_transport_progress_send(&f.p)==0,"prefix pending");
        p9_transport_progress_abort(&f.p);
        CHECK(f.p.bytes_sent==(at>0),"escaped-byte diagnostic");check_dead(&f);
    }
}
static void receive_boundaries(void) {
    for(size_t at=0;at<=67;at++) {
        struct fixture f;init(&f);frame(f.pipe.input,67);frame(f.pipe.input+67,7);
        f.pipe.available=at;f.pipe.chunk=1;
        for(size_t i=0;i<at;i++) {
            unsigned before=f.pipe.recvs;
            int rc=p9_transport_progress_recv(&f.p);
            CHECK(f.pipe.recvs==before+1,"one recv callback per step");
            CHECK(rc==(i+1==67?67:0),"recv completeness");
        }
        if(at<67) {
            size_t cursor=f.p.rx_have;
            CHECK(p9_transport_progress_recv(&f.p)==0 && f.p.rx_have==cursor,"EAGAIN preserves receive cursor");
            f.pipe.available=74;f.pipe.chunk=512;
            int rc=0;
            for(unsigned step=0;step<2 && !rc;step++)rc=p9_transport_progress_recv(&f.p);
            CHECK(rc==67,"receive resumes suffix");
        }
        CHECK(f.pipe.read==67&&memcmp(f.buf,f.pipe.input,67)==0,"one frame exact bytes");
        f.pipe.available=74;f.pipe.chunk=512;
        CHECK(p9_transport_progress_recv(&f.p)==7 && f.pipe.read==74,"coalesced next header stays separate");
        CHECK(f.t.total_recvd==2,"received frame count");
        p9_transport_progress_abort(&f.p);check_dead(&f);
    }
    for(size_t at=0;at<67;at++) {
        struct fixture f;init(&f);frame(f.pipe.input,67);f.pipe.available=at;f.pipe.chunk=1;
        for(size_t i=0;i<at;i++)CHECK(p9_transport_progress_recv(&f.p)==0,"partial receive");
        f.pipe.eof=true;
        CHECK(p9_transport_progress_recv(&f.p)<0,"EOF at every byte is terminal");check_dead(&f);
        init(&f);frame(f.pipe.input,67);f.pipe.available=at;f.pipe.chunk=1;
        for(size_t i=0;i<at;i++)CHECK(p9_transport_progress_recv(&f.p)==0,"partial cancel receive");
        p9_transport_progress_abort(&f.p);check_dead(&f);
    }
}
static void malformed(void) {
    const unsigned sizes[]={0,1,6,257,0xffffffffu};
    for(unsigned i=0;i<sizeof(sizes)/sizeof(sizes[0]);i++) {
        struct fixture f;init(&f);frame(f.pipe.input,7);u32 n=sizes[i];
        for(unsigned j=0;j<4;j++)f.pipe.input[j]=(u8)(n>>(8*j));f.pipe.available=7;
        CHECK(p9_transport_progress_recv(&f.p)<0,"reject frame outside negotiated bound");check_dead(&f);
    }
    struct fixture f;u8 msg[67];frame(msg,sizeof(msg));init(&f);f.pipe.oversize=true;
    CHECK(p9_transport_progress_recv(&f.p)<0,"reject oversize backend recv");check_dead(&f);
    init(&f);f.pipe.oversize=true;CHECK(!p9_transport_progress_queue(&f.p,msg,sizeof(msg)),"oversize send queue");
    CHECK(p9_transport_progress_send(&f.p)<0,"reject oversize backend send");check_dead(&f);
    init(&f);f.pipe.eof=true;CHECK(!p9_transport_progress_queue(&f.p,msg,sizeof(msg)),"EOF send queue");
    CHECK(p9_transport_progress_send(&f.p)<0,"send EOF is terminal");check_dead(&f);
    init(&f);CHECK(p9_transport_send(&f.t,msg,sizeof(msg))<0&&p9_transport_recv(&f.t)<0,"exclusive mode rejects legacy I/O");
    CHECK(p9_transport_close(&f.t)<0 && f.pipe.closes==0,"legacy close cannot bypass abort");
    CHECK(p9_transport_progress_limit(&f.p,257)<0,"limit cannot grow");
    CHECK(p9_transport_progress_limit(&f.p,7)==0,"limit can shrink at boundary");
    CHECK(p9_transport_progress_queue(&f.p,msg,sizeof(msg))<0,"send respects negotiated limit");
    p9_transport_progress_abort(&f.p);check_dead(&f);
}
static void version_refusal(void) {
    const char *dialects[]={"unknown","9P2000","9P2000.u","9P2000.Lx","9P2000.M",""};
    struct p9_session s;struct p9_dispatch_result r;u8 b[128];
    for(unsigned i=0;i<sizeof(dialects)/sizeof(dialects[0]);i++) {
        CHECK(p9_session_init(&s,0,256)==0,"session init");
        int n=p9_build_tversion(b,sizeof(b),P9_NOTAG,256,(const u8*)dialects[i],strlen(dialects[i]));b[4]=P9_RVERSION;
        CHECK(p9_session_dispatch_rmsg(&s,b,n,&r)<0,"unsupported dialect refused");
        CHECK(s.state==P9_SESS_INIT && s.negotiated_msize==0,"refusal cannot publish VERSIONED");
    }
    for(unsigned size=0;size<7;size++) {
        p9_session_init(&s,0,256);
        int n=p9_build_tversion(b,sizeof(b),P9_NOTAG,size,(const u8*)"9P2000.L",8);b[4]=P9_RVERSION;
        CHECK(p9_session_dispatch_rmsg(&s,b,n,&r)<0,"framing-impossible msize refused");
        CHECK(s.state==P9_SESS_INIT,"bad size cannot publish VERSIONED");
    }
    p9_session_init(&s,0,256);
    int n=p9_build_tversion(b,sizeof(b),P9_NOTAG,128,(const u8*)"9P2000.L",8);b[4]=P9_RVERSION;
    CHECK(!p9_session_dispatch_rmsg(&s,b,n,&r) && s.negotiated_msize==128,"supported negotiation preserved");
}

static void put32(u8 *b,u32 n) { for(unsigned i=0;i<4;i++)b[i]=(u8)(n>>(8*i)); }
struct hs_fixture {
    struct fixture f;struct p9_session s;struct p9_handshake_progress h;u8 out[256];
    bool version_reply,attach_reply;
};
static void hs_init_deadline(struct hs_fixture *x, u64 deadline) {
    memset(x,0,sizeof(*x));init(&x->f);x->f.pipe.chunk=1;
    CHECK(!p9_session_init(&x->s,17,256),"handshake session init");
    CHECK(!p9_handshake_progress_init(&x->h,&x->s,&x->f.p,x->out,sizeof(x->out),1007,deadline),"handshake init");
    CHECK(x->f.pipe.sends==0 && x->f.pipe.recvs==0,"handshake init has no I/O");
}
static void hs_init(struct hs_fixture *x) { hs_init_deadline(x,1000); }
static void hs_peer(struct hs_fixture *x) {
    struct pipe *p=&x->f.pipe;
    if(!x->version_reply && x->h.phase==P9_HS_VERSION_RECV) {
        CHECK(p->wrote==21 && p->output[4]==P9_TVERSION,"version wire once");
        int n=p9_build_tversion(p->input,sizeof(p->input),P9_NOTAG,128,(const u8*)"9P2000.L",8);
        CHECK(n==21,"fixture version size");p->input[4]=P9_RVERSION;p->available=n;x->version_reply=true;
    }
    if(!x->attach_reply && x->h.phase==P9_HS_ATTACH_RECV) {
        CHECK(p->wrote==44 && p->output[25]==P9_TATTACH,"attach wire once");
        CHECK(p->output[28]==17 && p->output[32]==255,"attach root and NOFID");
        CHECK(p->output[36]==0 && p->output[37]==0 && p->output[38]==0 && p->output[39]==0,"empty native uname/aname");
        CHECK(p->output[40]==(1007&255) && p->output[41]==(1007>>8),"captured principal on wire");
        u8 *b=p->input+p->available;memset(b,0,20);put32(b,20);b[4]=P9_RATTACH;b[5]=p->output[26];b[6]=p->output[27];
        b[7]=0x80;p->available+=20;x->attach_reply=true;
    }
}
static int hs_step(struct hs_fixture *x,u64 now) {
    unsigned calls=x->f.pipe.sends+x->f.pipe.recvs;
    int r=p9_handshake_progress_step(&x->h,now);
    CHECK(x->f.pipe.sends+x->f.pipe.recvs<=calls+1,"one bounded copy per handshake step");
    return r;
}
static void handshake_boundaries(void) {
    // 21 TX version + 21 RX version + 23 TX attach + 20 RX attach visits.
    for(unsigned cut=0;cut<=85;cut++)for(unsigned timed=0;timed<2;timed++) {
        struct hs_fixture x;hs_init(&x);
        for(unsigned step=0;step<cut;step++) {
            hs_peer(&x);int r=hs_step(&x,1);
            CHECK(r==(step==84?1:0),"READY only after complete Rattach");
        }
        if(cut==85) {
            CHECK(x.h.phase==P9_HS_READY && p9_session_fid_bound(&x.s,17),"root ready after attach");
            CHECK(x.f.p.frame_limit==128,"negotiated bound applied");
            CHECK(hs_step(&x,2000)==1,"completed handshake deadline is finished");
        }
        unsigned calls=x.f.pipe.sends+x.f.pipe.recvs;
        if(timed && cut<85) {
            CHECK(hs_step(&x,1000)==-T_E_TIMEDOUT,"absolute deadline at every handshake byte");
        } else p9_handshake_progress_abort(&x.h);
        int reason=(timed && cut<85)?-T_E_TIMEDOUT:-T_E_CANCELED;
        CHECK(hs_step(&x,1001)==reason,"terminal reason stable");
        p9_handshake_progress_abort(&x.h);
        CHECK(hs_step(&x,1002)==reason,"cancel cannot overwrite terminal reason");
        CHECK(x.f.pipe.sends+x.f.pipe.recvs==calls,"cancel/deadline sends no peer cleanup");
        CHECK(!p9_session_is_open(&x.s),"abort cannot leave session open");
        check_dead(&x.f);
    }
    struct hs_fixture x;hs_init(&x);
    for(unsigned i=0;i<21;i++)CHECK(hs_step(&x,1)==0,"send version prefix");
    for(unsigned i=2;i<1000;i++)CHECK(hs_step(&x,i)==0,"stalled peer yields");
    CHECK(hs_step(&x,1000)==-T_E_TIMEDOUT,"stalled peer cannot extend deadline");
    check_dead(&x.f);
}
static void handshake_errors(void) {
    const u32 errors[]={0,13,4095,0x80000000u,0xffffffffu};
    for(unsigned i=0;i<sizeof(errors)/sizeof(errors[0]);i++) {
        struct hs_fixture x;hs_init(&x);x.f.pipe.chunk=512;
        for(unsigned step=0;step<10 && x.h.phase!=P9_HS_ATTACH_RECV;step++) {
            hs_peer(&x);CHECK(hs_step(&x,1)==0,"drive to attach reply");
        }
        CHECK(x.h.phase==P9_HS_ATTACH_RECV,"attach waiting");
        struct pipe *p=&x.f.pipe;u8 *b=p->input+p->available;
        put32(b,11);b[4]=P9_RLERROR;b[5]=b[6]=0;put32(b+7,errors[i]);p->available+=11;
        int r=0;for(unsigned step=0;step<2 && !r;step++)r=hs_step(&x,1);
        int want=errors[i] && errors[i]<=4095?-(int)errors[i]:-T_E_IO;
        CHECK(r==want,"hostile errno bounded before negation");
        CHECK(x.h.failed_phase==P9_HS_ATTACH_RECV,"failed phase retained");check_dead(&x.f);
    }
    for(unsigned which=0;which<3;which++) {
        struct hs_fixture x;hs_init(&x);x.f.pipe.chunk=512;
        CHECK(hs_step(&x,1)==0,"send version");hs_peer(&x);
        if(which==0)x.f.pipe.input[20]='M';
        if(which==1)put32(x.f.pipe.input+7,0);
        if(which==2)put32(x.f.pipe.input+7,7); // too small for Tattach
        int r=0;for(unsigned step=0;step<3 && !r;step++)r=hs_step(&x,1);
        CHECK(r==-T_E_IO,"invalid negotiation terminally refuses attach");
        CHECK(x.f.pipe.wrote==21,"no attach after invalid negotiation");check_dead(&x.f);
    }
}

static void handshake_no_deadline(void) {
    struct hs_fixture x;hs_init_deadline(&x,0);
    int rc=0;
    for(unsigned n=0;n<200 && !rc;n++) { hs_peer(&x);rc=hs_step(&x,~(u64)0); }
    CHECK(rc==1 && x.h.phase==P9_HS_READY,"zero deadline never expires");
    p9_handshake_progress_abort(&x.h);
}
int main(void) { version_refusal();send_boundaries();receive_boundaries();malformed();handshake_boundaries();handshake_errors();handshake_no_deadline();puts("PASS AS-1 framing: every partial boundary, bounded steps, cancellation, EOF, malformed and coalesced frames"); }
