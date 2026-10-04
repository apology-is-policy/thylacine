// Shared native/host driver fixture. The pipe never sleeps; native compilation
// exercises the real client locks and dispatch, host uses the same assertions.
#define PC_CHECK(x, msg) do { if (!(x)) return msg; } while (0)
struct pc_pipe {
    u8 tx[2048], rx[2048];
    u32 sent, received, available, calls, aborts;
    u32 chunk;
    bool tx_block, rx_block;
};
struct pc_op { struct p9_rpc rpc; int completions, result; u32 bytes; };
struct pc_fixture {
    struct p9_client client;
    struct p9_client_progress progress;
    struct pc_pipe pipe;
    u8 recv[512];
    struct pc_op op[3];
};
static struct pc_fixture pc;
static void pc_zero(void *v, size_t n) { for (size_t i=0;i<n;i++) ((u8 *)v)[i]=0; }
static void pc_u32(u8 *p, u32 n) { for (u32 i=0;i<4;i++) p[i]=(u8)(n>>(8*i)); }
static int pc_send(void *ctx, const u8 *b, size_t n) {
    struct pc_pipe *p=ctx;p->calls++;
    if (p->tx_block) return P9_TRANSPORT_EAGAIN;
    if (n>p->chunk) n=p->chunk;
    if (n>sizeof(p->tx)-p->sent) return -1;
    for(size_t i=0;i<n;i++)p->tx[p->sent++]=b[i];
    return (int)n;
}
static int pc_recv(void *ctx, u8 *b, size_t n) {
    struct pc_pipe *p=ctx;p->calls++;
    if (p->rx_block || p->received==p->available) return P9_TRANSPORT_EAGAIN;
    if (n>p->chunk) n=p->chunk;
    if (n>p->available-p->received) n=p->available-p->received;
    for(size_t i=0;i<n;i++)b[i]=p->rx[p->received++];
    return (int)n;
}
static void pc_abort(void *ctx) { ((struct pc_pipe *)ctx)->aborts++; }
static int pc_bad_send(void *ctx,const u8 *b,size_t n) { (void)ctx;(void)b;(void)n;return -1; }
static int pc_bad_recv(void *ctx,u8 *b,size_t n) { (void)ctx;(void)b;(void)n;return -1; }
static int pc_bad_close(void *ctx) { (void)ctx;return -1; }
static void pc_complete(struct p9_rpc *rpc,int rc,struct p9_dispatch_result *dr) {
    struct pc_op *op=(struct pc_op *)rpc;
    op->completions++;op->result=rc;op->bytes=dr&&rc==0?dr->read_count:0;
}
static int pc_build(struct p9_session *s,u8 *out,size_t cap,void *ctx) {
    (void)ctx;return p9_session_send_read(s,out,cap,0,0,4);
}
static void pc_reply(u16 tag) {
    u8 *b=pc.pipe.rx+pc.pipe.available;pc_zero(b,15);pc_u32(b,15);
    b[4]=P9_RREAD;b[5]=(u8)tag;b[6]=(u8)(tag>>8);pc_u32(b+7,4);
    b[11]='t';b[12]='e';b[13]='s';b[14]='t';pc.pipe.available+=15;
}
static const char *pc_ready(void) {
    if(pc.client.magic==P9_CLIENT_MAGIC)p9_client_destroy(&pc.client);
    pc_zero(&pc,sizeof(pc));pc.pipe.chunk=512;
    struct p9_transport_ops ops={.send=pc_bad_send,.recv=pc_bad_recv,.close=pc_bad_close,.ctx=&pc.pipe};
    struct p9_transport_try_ops nb={.send=pc_send,.recv=pc_recv,.abort=pc_abort,.ctx=&pc.pipe};
    PC_CHECK(p9_client_init(&pc.client,0,256,ops,pc.recv,sizeof(pc.recv))==0,"client init");
    PC_CHECK(p9_client_progress_bind(&pc.client,&pc.progress,nb,1234,0)==0,"private bind/no deadline");
    PC_CHECK(!pc.pipe.calls,"bind must not do I/O");
    PC_CHECK(p9_client_handshake(&pc.client,0,0,0,0,0)<0,"blocking handshake refused");
    PC_CHECK(p9_client_progress_step(&pc.client,~(u64)0)==0,"send version");
    PC_CHECK(pc.pipe.sent==21,"exact version frame");
    int n=p9_build_tversion(pc.pipe.rx,sizeof(pc.pipe.rx),P9_NOTAG,256,(const u8 *)"9P2000.L",8);
    PC_CHECK(n==21,"version reply fixture");pc.pipe.rx[4]=P9_RVERSION;pc.pipe.available=21;
    for(unsigned i=0;i<3;i++) PC_CHECK(p9_client_progress_step(&pc.client,~(u64)0)>=0,"version/attach progress");
    PC_CHECK(pc.pipe.sent==44,"exact attach frame");
    PC_CHECK(pc.pipe.tx[40]==(1234&255)&&pc.pipe.tx[41]==(1234>>8),"creator principal retained");
    u8 *b=pc.pipe.rx+21;pc_zero(b,20);pc_u32(b,20);b[4]=P9_RATTACH;b[5]=pc.pipe.tx[26];b[6]=pc.pipe.tx[27];b[7]=0x80;pc.pipe.available=41;
    for(unsigned i=0;i<3&&pc.progress.handshake.phase!=P9_HS_READY;i++)
        PC_CHECK(p9_client_progress_step(&pc.client,~(u64)0)>=0,"attach progress");
    PC_CHECK(pc.progress.handshake.phase==P9_HS_READY,"exact attach publishes ready");
    for(unsigned i=0;i<3;i++)pc.op[i].rpc.on_complete=pc_complete;
    return 0;
}
static const char *pc_partial_and_duplex(void) {
    const char *err=pc_ready();if(err)return err;
    pc.pipe.chunk=1;
    PC_CHECK(!p9_client_submit_async(&pc.client,&pc.op[0].rpc,pc_build,0),"accept first read");
    PC_CHECK(p9_client_submit_async(&pc.client,&pc.op[1].rpc,pc_build,0)==-P9_E_AGAIN,"pending TX refuses buffer overwrite");
    PC_CHECK(pc.op[1].completions==1&&pc.op[1].result==-P9_E_AGAIN,"rejected request completes once");
    u8 bytes[4];u32 count;
    PC_CHECK(p9_client_read(&pc.client,0,0,4,bytes,&count)<0,"blocking read refused");
    unsigned start=pc.pipe.sent;
    for(unsigned i=0;i<60&&pc.progress.io.tx;i++) {
        u32 before=pc.pipe.calls;
        PC_CHECK(p9_client_progress_step(&pc.client,1)>=0,"partial send progress");
        PC_CHECK(pc.pipe.calls-before<=1,"one backend call per visit");
    }
    PC_CHECK(!pc.progress.io.tx&&pc.pipe.sent==start+23,"partial send exactly one frame");
    PC_CHECK(!pc.op[0].completions&&!pc.op[0].rpc.sending,"sent request awaits reply");
    pc_reply(pc.op[0].rpc.tag);
    PC_CHECK(!p9_client_submit_async(&pc.client,&pc.op[2].rpc,pc_build,0),"second request admitted after TX");
    pc.pipe.tx_block=true;
    for(unsigned i=0;i<50&&!pc.op[0].completions;i++) {
        u32 before=pc.pipe.calls;
        PC_CHECK(p9_client_progress_step(&pc.client,2)>=0,"RX progresses with blocked TX");
        PC_CHECK(pc.pipe.calls-before<=1,"duplex visit bounded");
    }
    PC_CHECK(pc.op[0].completions==1&&pc.op[0].result==0&&pc.op[0].bytes==4,"blocked sender cannot starve reply");
    PC_CHECK(pc.progress.io.tx&&!pc.op[2].completions,"blocked TX stays borrowed");
    p9_client_progress_abort(&pc.client);p9_client_progress_abort(&pc.client);
    PC_CHECK(pc.op[2].completions==1&&pc.op[2].result<0,"abort completes pending TX once");
    PC_CHECK(pc.op[0].completions==1,"abort preserves completed result");
    PC_CHECK(pc.pipe.aborts==1&&!pc.progress.sending&&!pc.progress.io.tx,"abort detaches TX before retirement");
    u32 before=pc.pipe.calls;
    PC_CHECK(p9_client_progress_step(&pc.client,3)<0&&pc.pipe.calls==before,"no I/O after abort");
    return 0;
}
static const char *pc_cancel_and_malformed(void) {
    for(unsigned at=0;at<23;at++) {
        const char *err=pc_ready();if(err)return err;pc.pipe.chunk=1;
        PC_CHECK(!p9_client_submit_async(&pc.client,&pc.op[0].rpc,pc_build,0),"accept cancellation request");
        for(unsigned i=0;pc.progress.io.tx_sent<at&&i<60;i++)
            PC_CHECK(p9_client_progress_step(&pc.client,1)>=0,"send to cancellation boundary");
        PC_CHECK(pc.progress.io.tx_sent==at,"exact TX cancellation boundary");
        p9_client_progress_abort(&pc.client);
        PC_CHECK(pc.op[0].completions==1&&pc.op[0].result<0,"every TX boundary completes terminally");
    }
    for(unsigned at=0;at<15;at++) {
        const char *err=pc_ready();if(err)return err;
        PC_CHECK(!p9_client_submit_async(&pc.client,&pc.op[0].rpc,pc_build,0),"accept receive cancellation request");
        PC_CHECK(p9_client_progress_step(&pc.client,1)>=0&&!pc.progress.io.tx,"send whole request");
        pc_reply(pc.op[0].rpc.tag);pc.pipe.chunk=1;
        for(unsigned i=0;pc.progress.io.rx_have<at&&i<20;i++)
            PC_CHECK(p9_client_progress_step(&pc.client,1)>=0,"receive to cancellation boundary");
        PC_CHECK(pc.progress.io.rx_have==at,"exact RX cancellation boundary");
        p9_client_progress_abort(&pc.client);
        PC_CHECK(pc.op[0].completions==1&&pc.op[0].result<0,"every RX boundary completes terminally");
    }
    for(unsigned kind=0;kind<4;kind++) {
        const char *err=pc_ready();if(err)return err;
        PC_CHECK(!p9_client_submit_async(&pc.client,&pc.op[0].rpc,pc_build,0),"malformed request admitted");
        if(kind!=0)PC_CHECK(p9_client_progress_step(&pc.client,1)>=0,"malformed fixture sent");
        else pc.pipe.tx_block=true;
        pc_reply(pc.op[0].rpc.tag);
        u8 *b=pc.pipe.rx+41;
        if(kind==1)b[5]=123; // unknown owner
        if(kind==2)b[4]=P9_RWRITE; // wrong reply kind
        if(kind==3)pc_u32(b,999); // negotiated frame bound
        for(unsigned i=0;i<8&&!pc.client.dead;i++)(void)p9_client_progress_step(&pc.client,1);
        PC_CHECK(pc.client.dead&&pc.op[0].completions==1&&pc.op[0].result<0,"malformed or premature reply aborts once");
        PC_CHECK(pc.pipe.aborts==1&&!pc.progress.sending,"protocol failure detaches local borrows");
    }
    return 0;
}
static unsigned pc_freed;
static int pc_freed_result;
static void pc_free_complete(struct p9_rpc *rpc,int rc,struct p9_dispatch_result *dr) {
    (void)dr;pc_freed++;pc_freed_result=rc;kfree(rpc);
}
static const char *pc_callback_release(void) {
    for(unsigned cancel=0;cancel<2;cancel++) {
        const char *err=pc_ready();if(err)return err;
        struct p9_rpc *rpc=kmalloc(sizeof(*rpc),0);
        PC_CHECK(rpc!=NULL,"callback lifetime allocation");pc_zero(rpc,sizeof(*rpc));
        rpc->on_complete=pc_free_complete;pc_freed=0;pc_freed_result=123;
        PC_CHECK(p9_client_submit_async(&pc.client,rpc,pc_build,0)==0,"freeing callback submitted");
        if(cancel)p9_client_progress_abort(&pc.client);
        else {
            u16 tag=rpc->tag;
            PC_CHECK(p9_client_progress_step(&pc.client,1)>=0,"send freeing callback request");
            pc_reply(tag);
            for(unsigned i=0;i<4&&!pc_freed;i++)(void)p9_client_progress_step(&pc.client,1);
        }
        PC_CHECK(pc_freed==1 && (cancel?pc_freed_result<0:pc_freed_result==0),"callback may free RPC on success or abort");
        p9_client_progress_abort(&pc.client);
        PC_CHECK(pc_freed==1&&!pc.progress.sending,"no RPC borrow after freeing callback");
    }
    return 0;
}
static const char *private_client_fixture_run(void) {
    const char *err=pc_partial_and_duplex();
    if(!err)err=pc_cancel_and_malformed();
    if(!err)err=pc_callback_release();
    if(pc.client.magic==P9_CLIENT_MAGIC)p9_client_destroy(&pc.client);
    return err;
}
#undef PC_CHECK
