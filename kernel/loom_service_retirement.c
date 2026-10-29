#include <thylacine/loom_service_retirement.h>
#include <thylacine/loom.h>
#include <thylacine/srvconn.h>
#include <thylacine/errno.h>
#include <thylacine/extinction.h>
int loom_retirement_quiesce(struct loom_service_protocol *p) {
    if(!p || !p->owner)return -T_E_INVAL;
    struct Loom *l=p->owner;spin_lock(&l->lock);
    if(!p->driving || !p->indexed || !p->connect_bound || p->abort_reason>=0 ||
       p->client.magic!=P9_CLIENT_MAGIC || p->client.progress!=&p->progress ||
       !p->admission.conn) {spin_unlock(&l->lock);return -T_E_BUSY;}
    bool done=p->rpc_quiesced;spin_unlock(&l->lock);if(done)return 0;
    srvconn_teardown(p->admission.conn);
    p9_client_progress_abort(&p->client);
    // Sole driver has returned from all callbacks. An abort flag by itself
    // does not certify these cursors or RPC container users have ended.
    spin_lock(&l->lock);
    if(p->progress.sending || p->progress.io.tx)extinction("abort retained TX cursor");
    struct p9_outstanding *e;
    for(u32 tag=0;(e=p9_session_next_active(&p->client.session,&tag))!=NULL;++tag)
        if(e->owner)extinction("abort retained RPC callback");
    p->rpc_quiesced=true;spin_unlock(&l->lock);return 0;
}
int loom_retirement_close_operation(struct loom_service_operation *o,u64 now) {
    if(!o || !o->owner || !o->protocol)return -T_E_INVAL;
    struct Loom *l=o->owner;struct loom_service_protocol *p=o->protocol;
    spin_lock(&l->lock);
    if(!l->service_closing || !l->service_slots->closing || !p->driving ||
       !p->rpc_quiesced || p->abort_reason>=0 || o->phase==LRPC_NEW) {
        spin_unlock(&l->lock);return -T_E_BUSY;
    }
    int rc=0;
    if(o->phase==LRPC_QUEUED || o->phase==LRPC_INFLIGHT)rc=loom_rpc_settle_locked(o);
    if(!rc && o->pool_pending)rc=loom_provided_discard_closed_locked(o);
    if(!rc && o->pool_busy)rc=loom_provided_release_busy_locked(o);
    spin_unlock(&l->lock);if(rc)return rc;
    if(o->fixed.owner || o->fixed_dropping){rc=loom_private_io_drop_fixed(o);if(rc)return rc;}
    spin_lock(&l->lock);
    if(o->phase==LRPC_SETTLING)rc=loom_admission_cancel_locked(o,p->abort_reason,p->rpc_quiesced,now);
    if(!rc && o->phase!=LRPC_TERMINAL)rc=-T_E_BUSY;
    if(!rc) {
        struct loom_service_request *r=&l->service_requests.records[o->request.index];
        if(r->phase!=LREQ_DELIVERED)
            rc=loom_request_discard_closed_owner(&l->service_requests,l->service_slots,o->request);
    }
    spin_unlock(&l->lock);if(rc)return rc;
    return loom_private_operation_release_retired(o);
}

int loom_retirement_post_terminal(struct loom_service_operation *o) {
    if(!o || !o->owner || !o->protocol)return -T_E_INVAL;
    struct Loom *l=o->owner;spin_lock(&l->lock);
    if(!o->protocol->driving || o->phase!=LRPC_TERMINAL || o->pool_pending ||
       o->pool_busy || o->fixed.owner || o->fixed_dropping) {
        spin_unlock(&l->lock);return -T_E_BUSY;
    }
    // The actual combined publisher already delivered a single-shot lease.
    bool combined=o->pool_terminal;spin_unlock(&l->lock);
    return combined?0:loom_private_post_terminal(l,o->request);
}
