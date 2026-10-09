// Actual private controls with native protocol ownership. Wire responder supplies
// only replies; production code owns authority, state, cancellation and cleanup.
static int control_native_query(struct Loom *l,struct Proc *p,
        struct loom_service_ref ref,struct loom_service_snapshot *out) {
    struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=ref};
    return loom_private_service_query(l,p,&in,out);
}
static struct {
    struct Loom *l;struct Proc *creator;struct loom_service_ref scope;
    bool seen,retained;
} control_boundary;
static void control_destruction_boundary(struct Loom *l,u32 stage) {
    if(stage!=4 || l!=control_boundary.l)return;
    loom_private_close_hook_for_test(NULL);
    struct loom_service_snapshot out={0};
    control_boundary.seen=true;
    int rc=control_native_query(l,control_boundary.creator,control_boundary.scope,&out);
    control_boundary.retained=!rc && out.state==LOOM_SERVICE_ABORTING &&
        out.active_ops==1 && !(out.status_flags&LOOM_SERVICE_LOCAL_RETIRED);
}
static const char *loom_control_cases(struct Loom *l,struct Proc *creator,
        struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref targets[2]) {
    const char *error=NULL;struct loom_service_protocol *p=NULL,*held=NULL;
    struct SrvConn *server=NULL;struct loom_service_ref scope={0},child={0};
    struct loom_service_snapshot out={0};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    struct loom_service_control control={.size=sizeof(control),.version=1};
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
    u32 pages=creator->as->page_count;
#define CT(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    CT(!control_native_query(l,creator,targets[0],&out) && out.kind==LOOM_SERVICE_TARGET &&
       out.state==LOOM_SERVICE_READY && !out.scope.incarnation,"control real target snapshot");
    struct loom_service_snapshot in={.size=sizeof(in),.version=1,.object=targets[0]};
    out.reason=123;
    CT(loom_private_service_query(l,poster,&in,&out)==-T_E_ACCES && out.reason==123,
       "control rejects actual foreign creator without output");
    ++in.object.incarnation;
    CT(loom_private_service_query(l,creator,&in,&out)==-T_E_NOENT && out.reason==123,
       "control stale query preserves output");in.object=targets[0];in.kind=1;
    CT(loom_private_service_query(l,creator,&in,&out)==-T_E_INVAL && out.reason==123,
       "control output-only input rejected");
    for(u32 mode=0;mode<4;++mode) {
        CT(!loom_private_node_prepare(l,creator,&reserve,&scope) && !loom_private_node_publish(l,scope),
           "control actual scope reservation");control.object=scope;
        CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RESERVED &&
           !out.active_ops && !out.pending_terminals && !out.reason && !out.status_flags,
           "control RESERVED starts without diagnostics");
        if(mode==0) {
            CT(!loom_private_service_abort(l,creator,&control) &&
               loom_private_service_abort(l,creator,&control)==1,"control unused abort idempotence");
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RETIRED &&
               out.reason==-T_E_CANCELED && out.status_flags==LOOM_SERVICE_LOCAL_RETIRED,
               "control unused abort locally retires");
        } else if(mode==1) {
            CT(!loom_private_connect_prepare(l,scope,targets[0],81,1000,1,&p),"control raw constructor");
            CT(!control_native_query(l,creator,scope,&out) && out.active_ops==1,
               "control counts PREPARED owner");
            CT(!loom_private_service_abort(l,creator,&control) &&
               loom_private_connect_publish(p,2)==-T_E_CANCELED &&
               loom_private_service_reap(l,creator,&control)==-T_E_BUSY,"control cancels constructing scope");
            control_boundary.l=l;control_boundary.creator=creator;control_boundary.scope=scope;
            control_boundary.seen=false;control_boundary.retained=false;
            loom_private_close_hook_for_test(control_destruction_boundary);
            int rc=loom_private_connect_rollback(p);
            loom_private_close_hook_for_test(NULL);if(!rc)p=NULL;
            CT(!rc && control_boundary.seen && control_boundary.retained,
               "control physical rollback boundary retains diagnostic owner");
            CT(!control_native_query(l,creator,scope,&out) && !out.active_ops &&
               out.state==LOOM_SERVICE_RETIRED && out.reason==-T_E_CANCELED &&
               out.status_flags==LOOM_SERVICE_LOCAL_RETIRED,"control rollback retains first reason");
        } else {
            CT(!loom_private_connect_prepare(l,scope,targets[mode-2],80+mode,1000,1,&p),
               "control normal/bulk protocol prepare");
            CT(!loom_engine_adopt(p,2),"control actual indexed admission");p=NULL;
            CT(!loom_engine_get(l,scope,&held),"control retains real entry for inspection");
            server=srv_accept_blocking(srv_lookup_in(reg,mode==2?"small":"large",5),poster->stripes);
            CT(server==held->admission.conn,"control accepted real endpoint");
            CT(!loom_post_cqe(l,1,0,0) && !loom_post_cqe(l,2,0,0),"control fills CQ before handshake");
            CT(!lc_reply(server,P9_TVERSION,P9_NOTAG) && !lc_reply(server,P9_TATTACH,0),
               "control wire version/attach replies");
            for(u32 n=0;n<64 && l->service_nodes->nodes[scope.slot].state!=LOOM_SERVICE_READY;++n)
                (void)loom_engine_visit(l,3+n);
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_READY &&
               !out.active_ops && out.pending_terminals==1 && out.status_flags==LOOM_SERVICE_BYTES_SENT,
               "control actual READY snapshot retains full-CQ terminal and sent bytes");
            struct loom_service_reserve fid={.size=sizeof(fid),.version=1,.kind=LOOM_SERVICE_FID,.scope=scope};
            CT(!loom_private_node_prepare(l,creator,&fid,&child) && !loom_private_node_publish(l,child),
               "control child retains actual READY parent");
            CT(!control_native_query(l,creator,child,&out) && out.kind==LOOM_SERVICE_FID &&
               out.scope.slot==scope.slot && out.scope.incarnation==scope.incarnation,
               "control child snapshot keeps exact parent");
            CT(!loom_private_service_abort(l,creator,&control) &&
               loom_private_service_abort(l,creator,&control)==1,"control CQ-independent abort and idempotence");
            (void)loom_engine_visit(l,100);
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RETIRED &&
               out.reason==-T_E_CANCELED && out.status_flags==3 && out.pending_terminals==1 &&
               l->service_requests.records[held->request.index].result==(s32)scope.slot,
               "control actual abort preserves committed success and retirement diagnostics");
            CT(loom_private_service_reap(l,creator,&control)==-T_E_BUSY,"control entry/terminal pins block reap");
            h->cq_head=l->cq_tail;
            CT(!loom_private_post_terminal(l,held->request),"control publishes retained success");
            CT(!control_native_query(l,creator,scope,&out) && !out.active_ops && !out.pending_terminals,
               "control delivered storage is not a pending operation");
            CT(!loom_engine_put(l,scope,held),"control returns entry pin");held=NULL;
            u64 freed=srvconn_total_freed();
            CT(!loom_engine_collect(l,scope) && srvconn_total_freed()==freed && creator->as->page_count==pages,
               "control local protocol refund leaves server endpoint alive");
            CT(!control_native_query(l,creator,scope,&out) && out.state==LOOM_SERVICE_RETIRED &&
               out.reason==-T_E_CANCELED && out.status_flags==3,"control diagnostics survive physical collection");
            CT(loom_private_service_reap(l,creator,&control)==-T_E_BUSY,"control child blocks parent reap");
            struct loom_service_control child_control={.size=sizeof(child_control),.version=1,.object=child};
            CT(!loom_private_service_reap(l,creator,&child_control),"control unused child reap after parent abort");
            child=(struct loom_service_ref){0};srvconn_unref(server);server=NULL;
        }
        CT(!loom_private_service_reap(l,creator,&control) &&
           loom_private_service_reap(l,creator,&control)==-T_E_NOENT,"control reap consumes exact identity once");
        scope=(struct loom_service_ref){0};h->cq_head=l->cq_tail;
        CT(creator->as->page_count==pages && !l->service_requests.occupied,
           "control refunds all protocol pages and obligations");
    }
done:
    loom_private_close_hook_for_test(NULL);
    if(held)(void)loom_engine_put(l,scope,held);
    if(p)(void)loom_private_connect_rollback(p);
    if(l->service_engine->count) {
        loom_exec_latch(l);
        for(u32 i=0;i<64;++i)(void)loom_engine_visit(l,10000);
        (void)loom_engine_discard_closed(l);
    }
    if(server)srvconn_unref(server);
#undef CT
    return error;
}
