// Deterministic real-owner interleavings: execution pauses outside Ll while the
// independent retirement thread observes INSTALLING, driving or teardown users.
static struct {
    struct Loom *l;
    struct Proc *creator;
    struct loom_service_ref scope;
    hidx_t fd;
    u32 stage;
    bool seen;
    const char *error;
} close_boundary;
static void loom_close_boundary_hook(struct Loom *l,u32 stage) {
    if(l!=close_boundary.l || stage!=close_boundary.stage)return;
    loom_private_close_hook_for_test(NULL); // one exact owner boundary
    close_boundary.seen=true;
    if(handle_close(close_boundary.creator,close_boundary.fd)) {
        close_boundary.error="boundary actual handle close";return;
    }
    close_boundary.fd=-1;
    if(!loom_close_wait_state(l,LCLOSE_PARKED)) {
        close_boundary.error="boundary close parks on real suspended owner";return;
    }
    spin_lock(&l->lock);
    struct loom_engine_entry *e=&l->service_engine->entries[close_boundary.scope.slot];
    bool retained=e->protocol && !e->protocol->locally_retired &&
        !l->service_requests.detached && l->service_requests.occupied==1;
    bool held=stage==1 ? e->phase==LENGINE_INSTALLING && e->pins==1 :
        stage==2 ? e->protocol->driving : e->protocol->users!=0;
    spin_unlock(&l->lock);
    if(!retained || !held) {close_boundary.error="boundary retains actual indexed owner and consumer";return;}
    u64 before=0,after=0;loom_private_close_observe_for_test(l,&before);
    for(u32 i=0;i<64;++i)sched();
    if(loom_private_close_observe_for_test(l,&after)!=LCLOSE_PARKED || before!=after)
        close_boundary.error="boundary parked owner cannot self-reschedule";
}
static const char *loom_close_boundary_case(struct Loom **owner,struct Proc *creator,
        struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref target,
        u32 stage,u64 goal,u32 baseline) {
    const char *error=NULL;struct Loom *l=*owner;
    struct loom_service_ref scope={0};
    struct loom_service_protocol *p=NULL,*held=NULL;
    struct SrvConn *server=NULL;struct Handle observer={0};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    close_boundary.l=l;close_boundary.creator=creator;close_boundary.fd=-1;
    close_boundary.stage=stage;close_boundary.seen=false;close_boundary.error=NULL;
#define BC(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    BC(!loom_private_node_prepare(l,creator,&reserve,&scope) && !loom_private_node_publish(l,scope),
       "boundary scope reservation");close_boundary.scope=scope;
    BC(!loom_private_connect_prepare(l,scope,target,199,0,0,&p),"boundary actual preparation");
    if(stage!=1) {
        BC(!loom_engine_adopt(p,0),"boundary actual adoption");p=NULL;
        BC(!loom_engine_get(l,scope,&held),"boundary external entry pin");
        server=srv_accept_blocking(srv_lookup_in(reg,"small",5),poster->stripes);
        BC(server==held->admission.conn,"boundary accepted stalled endpoint");
    }
    close_boundary.fd=handle_alloc(creator,KOBJ_LOOM,RIGHT_READ|RIGHT_WRITE,l);
    BC(close_boundary.fd>=0,"boundary handle owns original reference");*owner=NULL;
    BC(!handle_get(creator,close_boundary.fd,&observer),"boundary observer root");
    loom_private_close_hook_for_test(loom_close_boundary_hook);
    if(stage==1) {BC(!loom_engine_adopt(p,0),"closing installer completes ownership transfer");p=NULL;}
    else if(stage==2)(void)loom_engine_visit(l,0);
    else BC(!loom_engine_abort(l,scope,-T_E_CANCELED),"real teardown user returns after close");
    loom_private_close_hook_for_test(NULL);
    BC(close_boundary.seen && !close_boundary.error,close_boundary.error?close_boundary.error:"boundary hook reached");
    if(held){BC(!loom_engine_put(l,scope,held),"boundary releases external entry");held=NULL;}
    BC(loom_close_wait_state(l,LCLOSE_DONE),"last real boundary return resumes close to completion");
    BC(loom_engine_empty(l->service_engine) && !l->service_requests.occupied && l->service_requests.detached,
       "boundary close settled every owner");
    u64 freed=srvconn_total_freed();handle_put(&observer);l=NULL;
    BC(lp_wait(goal) && creator->as->page_count==baseline && !creator->as->private_rings,
       "boundary final observer return refunds exact image");
    BC(!server || srvconn_total_freed()==freed,"boundary peer survives local final reclamation");
done:
    loom_private_close_hook_for_test(NULL);
    if(l) {
        if(held)(void)loom_engine_put(l,scope,held);
        if(p)(void)loom_private_connect_rollback(p);
        if(close_boundary.fd>=0){handle_close(creator,close_boundary.fd);close_boundary.fd=-1;}
        else if(*owner)loom_handle_closed(l);
    }
    if(observer.obj)handle_put(&observer);
    if(server)srvconn_unref(server);
    close_boundary.l=NULL;
#undef BC
    return error;
}
