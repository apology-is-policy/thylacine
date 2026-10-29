#include <thylacine/loom_service_wire.h>
#include <thylacine/errno.h>

int loom_private_wire_build(struct p9_session *s,u8 *out,size_t cap,void *ctx) {
    struct loom_service_wire *w=ctx;
    if(!s || !out || !w || w->built || w->fid==P9_NOFID)return -T_E_INVAL;
    u32 msize=s->negotiated_msize?s->negotiated_msize:s->msize;
    if(cap>msize)cap=msize;
    u32 count=w->count;
    if(w->kind==P9_TREAD || w->kind==P9_TWRITE) {
        if(count>w->payload_capacity || (count && !w->payload))return -T_E_INVAL;
        // Match ordinary client limits: Rread header+count is 11 bytes;
        // Twrite header+fid+offset+count is 23. Output capacity also bounds write.
        u32 bound=w->kind==P9_TWRITE?(u32)cap:msize;
        u32 overhead=w->kind==P9_TWRITE?P9_HDR_LEN+16u:P9_HDR_LEN+4u;
        u32 limit=bound>overhead?bound-overhead:0;
        if(count && !limit)return -T_E_INVAL;
        
        if(limit>INT32_MAX)limit=INT32_MAX;
        if(count>limit)count=limit;
        if(w->offset>UINT64_MAX-count)return -T_E_INVAL;
    }
    int rc;
    u32 new_fid=w->fid;
    u64 attr_mask=0;
    u16 names=0;
    switch(w->kind) {
    case P9_TGETATTR:
        attr_mask=P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID;
        rc=p9_session_send_getattr(s,out,cap,w->fid,attr_mask);break;
    case P9_TWALK: {
        u16 n=w->component_len;
        if(n>255 || (n==1 && w->component[0]=='.') ||
           (n==2 && w->component[0]=='.' && w->component[1]=='.'))return -T_E_INVAL;
        for(u16 i=0;i<n;++i)if(!w->component[i] || w->component[i]=='/')return -T_E_INVAL;
        const u8 *name=w->component;size_t len=n;names=n?1:0;
        new_fid=w->new_fid;
        rc=p9_session_send_walk(s,out,cap,w->fid,new_fid,names,&name,&len);break;
    }
    case P9_TLOPEN:
        if(w->mode>2)return -T_E_INVAL;
        rc=p9_session_send_lopen(s,out,cap,w->fid,w->mode);break;
    case P9_TREAD:
        rc=p9_session_send_read(s,out,cap,w->fid,w->offset,count);break;
    case P9_TWRITE:
        rc=p9_session_send_write(s,out,cap,w->fid,w->offset,count,w->payload);break;
    case P9_TCLUNK:
        rc=p9_session_send_clunk(s,out,cap,w->fid);break;
    default:return -T_E_OPNOTSUPP;
    }
    if(rc<=0)return rc;
    // Session outstanding uses new_fid==fid for every non-WALK request.
    // Publish expectations only after the matching real builder succeeded.
    w->expect.fid=w->fid;w->expect.new_fid=new_fid;w->expect.kind=w->kind;
    w->expect.count=count;w->expect.attr_mask=attr_mask;w->expect.walk_names=names;
    w->built=true;
    return rc;
}
int loom_private_wire_submit(struct p9_client *c,struct p9_rpc *rpc,struct loom_service_wire *w) {
    if(!w)return -T_E_INVAL;
    w->built=false;w->local_again=false;
    int rc=p9_client_submit_async(c,rpc,loom_private_wire_build,w);
    // This is the submit return, not callback status. Accepted submissions
    // return zero even when a later server reply reports EAGAIN.
    w->local_again=(rc==-P9_E_AGAIN);
    return rc;
}
bool loom_private_wire_retry(const struct loom_service_wire *w,int error,
                             const struct loom_service_reply *reply) {
    return w && reply && w->local_again && !error && reply->result==-T_E_AGAIN;
}
