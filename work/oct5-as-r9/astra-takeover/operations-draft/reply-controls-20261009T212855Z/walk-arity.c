#include <thylacine/loom_service_reply.h>
#include <thylacine/errno.h>

int loom_private_reply_capture(const struct loom_service_reply_expect *e, int status,
    const struct p9_dispatch_result *d, u8 *payload, u32 capacity,
    struct loom_service_reply *out) {
    if (!e || !out) return -T_E_INVAL;
    struct loom_service_reply r={0};
    // Error paths may pass no parsed frame, or an incompletely filled result.
    if (status<0) { r.result=status; *out=r; return 0; }
    if (status || !d || d->is_error || d->kind!=e->kind || d->fid!=e->fid ||
        d->new_fid!=e->new_fid) return -T_E_IO;
    switch (e->kind) {
    case P9_TWALK:
        if (false || d->nwqid!=e->walk_names ||
            d->bound_new_fid!=e->new_fid) return -T_E_IO;
        if (e->walk_names) r.qid=d->qids[0];
        break;
    case P9_TLOPEN:
        r.qid=d->open_qid;r.iounit=d->open_iounit;
        break;
    case P9_TREAD:
        if (d->read_count>e->count || d->read_count>capacity ||
            d->read_count>INT32_MAX || (d->read_count && (!payload || !d->read_data)))
            return -T_E_IO;
        // Copy during the callback: recv storage may be reused on the next
        // quantum. Ownership of the destination outlives this parser borrow.
        for (u32 i=0;i<d->read_count;++i) payload[i]=d->read_data[i];
        r.result=(s32)d->read_count;
        break;
    case P9_TWRITE:
        if (d->write_count>e->count || d->write_count>INT32_MAX) return -T_E_IO;
        r.result=(s32)d->write_count;
        break;
    case P9_TGETATTR:
        if ((d->attr.valid&e->attr_mask)!=e->attr_mask) return -T_E_IO;
        r.attr=d->attr;
        break;
    case P9_TCLUNK: break;
    default: return -T_E_OPNOTSUPP;
    }
    *out=r;
    return 0;
}

#include <assert.h>
#include <string.h>
#include <stdio.h>
static struct loom_service_reply_expect e;
static struct p9_dispatch_result d;
static struct loom_service_reply result;
static u8 src[16],dest[16];
static unsigned cases;
static void reset(u8 kind) {
 memset(&e,0,sizeof(e));memset(&d,0,sizeof(d));memset(&result,0xa5,sizeof(result));
 memset(dest,0xcc,sizeof(dest));memset(src,0x7b,sizeof(src));
 e.kind=d.kind=kind;e.fid=d.fid=7;e.new_fid=d.new_fid=8;
 e.count=4;d.read_count=d.write_count=4;d.read_data=src;
}
static void bad(int status,const struct p9_dispatch_result *p,u8 *buf,u32 cap,const char *name) {
 struct loom_service_reply before=result;u8 bytes[16];memcpy(bytes,dest,16);++cases;
 int rc=loom_private_reply_capture(&e,status,p,buf,cap,&result);
 if(rc>=0 || memcmp(&result,&before,sizeof(result)) || memcmp(bytes,dest,16)) {
  fprintf(stderr,"%s: invalid capture mutated output/payload or succeeded (%d)\n",name,rc);assert(0);
 }
}
static void good(void) {++cases;assert(!loom_private_reply_capture(&e,0,&d,dest,16,&result));}
int main(void) {
 reset(P9_TREAD);good();assert(result.result==4 && !memcmp(src,dest,4) && dest[4]==0xcc);
 memset(src,0,16);assert(dest[0]==0x7b); // no escaped recv-buffer alias
 reset(P9_TREAD);d.read_count=5;bad(0,&d,dest,16,"oversized read request");
 reset(P9_TREAD);bad(0,&d,dest,3,"read capacity");
 reset(P9_TREAD);d.read_data=NULL;bad(0,&d,dest,16,"missing read source");
 reset(P9_TREAD);bad(0,&d,NULL,16,"missing pinned destination");
 reset(P9_TREAD);d.read_count=0;d.read_data=NULL;assert(!loom_private_reply_capture(&e,0,&d,NULL,0,&result)&&result.result==0);
 reset(P9_TREAD);bad(0,NULL,dest,16,"missing parsed success");
 reset(P9_TREAD);bad(1,&d,dest,16,"positive callback status");
 reset(P9_TREAD);d.kind=P9_TWRITE;bad(0,&d,dest,16,"wrong operation");
 reset(P9_TREAD);d.fid=9;bad(0,&d,dest,16,"wrong source fid");
 reset(P9_TREAD);d.new_fid=9;bad(0,&d,dest,16,"wrong destination fid");
 reset(P9_TREAD);d.is_error=true;bad(0,&d,dest,16,"unmapped protocol error");
 reset(P9_TREAD);d.read_count=UINT32_MAX;d.read_data=(void*)1;
 assert(!loom_private_reply_capture(&e,-T_E_CANCELED,&d,dest,16,&result));assert(result.result==-T_E_CANCELED&&dest[0]==0xcc);
 assert(!loom_private_reply_capture(&e,-T_E_IO,NULL,NULL,0,&result));assert(result.result==-T_E_IO);
 reset(P9_TWRITE);good();assert(result.result==4);
 reset(P9_TWRITE);d.write_count=5;bad(0,&d,dest,16,"oversized write count");
 reset(P9_TWRITE);e.count=d.write_count=0x80000000u;bad(0,&d,dest,16,"signed result overflow");
 reset(P9_TWALK);e.walk_names=d.nwqid=1;d.bound_new_fid=8;d.qids[0].path=123;good();assert(result.qid.path==123);
 reset(P9_TWALK);e.walk_names=1;d.nwqid=0;d.bound_new_fid=8;bad(0,&d,dest,16,"short component walk");
 reset(P9_TWALK);e.walk_names=0;d.nwqid=1;d.bound_new_fid=8;bad(0,&d,dest,16,"long clone reply");
 reset(P9_TWALK);d.bound_new_fid=9;bad(0,&d,dest,16,"wrong bound fid");
 reset(P9_TWALK);d.bound_new_fid=8;good();assert(result.qid.path==0);
 reset(P9_TWALK);e.walk_names=d.nwqid=2;d.bound_new_fid=8;bad(0,&d,dest,16,"unsupported walk arity");
 reset(P9_TLOPEN);d.open_qid.path=456;d.open_iounit=4096;good();assert(result.qid.path==456&&result.iounit==4096);
 reset(P9_TGETATTR);e.attr_mask=7;d.attr.valid=7;d.attr.uid=123;good();assert(result.attr.uid==123);d.attr.uid=0;assert(result.attr.uid==123);
 reset(P9_TGETATTR);e.attr_mask=7;d.attr.valid=3;bad(0,&d,dest,16,"missing DAC attribute");
 reset(P9_TCLUNK);good();assert(result.result==0);
 reset(P9_TVERSION);bad(0,&d,dest,16,"unqualified operation");
 printf("PASS %u capture cases plus error/EOF and retained-copy assertions\n",cases);
}
