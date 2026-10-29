#include <thylacine/loom_service_reply.h>
#include <thylacine/errno.h>

// Freestanding kernel has no libc memcpy symbol. Large structure assignment
// may emit one even with -fno-builtin; keep the copy explicit like 9p_client.
static void reply_copy(void *to,const void *from,size_t count) {
    u8 *dst=to;const u8 *src=from;
    for(size_t i=0;i<count;++i)dst[i]=src[i];
}

int loom_private_reply_capture(const struct loom_service_reply_expect *e, int status,
    const struct p9_dispatch_result *d, u8 *payload, u32 capacity,
    struct loom_service_reply *out) {
    if (!e || !out) return -T_E_INVAL;
    struct loom_service_reply r={0};
    // Error paths may pass no parsed frame, or an incompletely filled result.
    if (status<0) { r.result=status; reply_copy(out,&r,sizeof(r)); return 0; }
    if (status || !d || d->is_error || d->kind!=e->kind || d->fid!=e->fid ||
        d->new_fid!=e->new_fid) return -T_E_IO;
    switch (e->kind) {
    case P9_TWALK:
        if (e->walk_names>1 || d->nwqid!=e->walk_names ||
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
        reply_copy(&r.attr,&d->attr,sizeof(r.attr));
        break;
    case P9_TCLUNK: break;
    default: return -T_E_OPNOTSUPP;
    }
    reply_copy(out,&r,sizeof(r));
    return 0;
}
