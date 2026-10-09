// Real handle-close/forced-exec owner retirement while accepted servers remain
// stalled and the CQ is full. Observer borrow protects only fixture inspection.
static bool loom_close_wait_state(struct Loom *l, u32 wanted) {
    u64 end=timer_now_ns()+5000000000ull;
    while (timer_now_ns()<end) {
        if (loom_private_close_observe_for_test(l,NULL)==wanted) return true;
        sched();
    }
    return false;
}
static const char *loom_close_cases(struct Loom **owner, struct Proc *creator,
        struct Proc *poster, struct SrvRegistry *reg, struct loom_service_ref target[2],
        u32 kind, u64 goal, u32 baseline) {
    const char *error=NULL;
    struct Loom *l=*owner,*other=NULL;
    struct loom_service_ref scope[2]={{0},{0}},child={0};
    struct loom_service_protocol *prepared[2]={NULL,NULL},*held[2]={NULL,NULL};
    struct SrvConn *server[2]={NULL,NULL};
    struct Handle observer={0};
    struct loom_private_fixed fixed={0};
    hidx_t fd=-1;
    u64 va=0;
    u32 owner_pages=creator->as->page_count;
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
#define CL(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    for (u32 i=0;i<2;++i) {
        CL(!loom_private_node_prepare(l,creator,&reserve,&scope[i]) &&
           !loom_private_node_publish(l,scope[i]),"close native scope reservation");
        CL(!loom_private_connect_prepare(l,scope[i],target[i],77+i,0,0,&prepared[i]),
           "close native preparation");
        if (kind==6 && i==1) continue; // real constructor owns a request but no index
        CL(!loom_engine_adopt(prepared[i],0),"close native adoption");prepared[i]=NULL;
        CL(!loom_engine_get(l,scope[i],&held[i]),"close native held entry");
        struct SrvService *svc=srv_lookup_in(reg,i?"large":"small",5);
        CL(srv_backlog_depth(svc)==1,"close native backlog");
        server[i]=srv_accept_blocking(svc,poster->stripes);
        CL(server[i]==held[i]->admission.conn,"close native accepted endpoint");
    }
    if (kind!=6) {
        CL(!lc_reply(server[1],P9_TVERSION,P9_NOTAG) && !lc_reply(server[1],P9_TATTACH,0),
           "close second peer handshake replies");
        for (u32 i=0;i<64 && l->service_nodes->nodes[scope[1].slot].state!=LOOM_SERVICE_READY;++i)
            (void)loom_engine_visit(l,i);
        CL(l->service_nodes->nodes[scope[1].slot].state==LOOM_SERVICE_READY,
           "close second peer READY while first stalls");
        reserve.kind=LOOM_SERVICE_FID;reserve.scope=scope[1];
        CL(!loom_private_node_prepare(l,creator,&reserve,&child),"close unused child holds retired parent metadata");
        if (kind==4) CL(!loom_private_node_publish(l,child),"exec native child publication");
        CL(!loom_post_cqe(l,123,0,0),"close fills CQ behind committed success");
    } else {
        CL(!loom_post_cqe(l,123,0,0) && !loom_post_cqe(l,124,0,0),"constructor close full CQ");
    }
    if (kind==5) {
        s64 attached=sys_burrow_attach_for_proc(creator,PAGE_SIZE);
        CL(attached>0,"close fixed backing attach");va=(u64)attached;
        struct loom_buf_reg b={va,PAGE_SIZE};
        struct loom_service_pool_member m={0,0,0,64};
        CL(!loom_private_register_buffers(l,creator,&b,1) &&
           !loom_private_fixed_acquire(l,creator,&m,&fixed),"close fixed accepted pin");
        CL(!sys_burrow_detach_for_proc(creator,va,PAGE_SIZE),"close fixed unmaps caller backing");va=0;
    }
    fd=handle_alloc(creator,KOBJ_LOOM,RIGHT_READ|RIGHT_WRITE,l);
    CL(fd>=0,"close actual handle owns original ring reference");*owner=NULL;
    CL(!handle_get(creator,fd,&observer),"close observer borrower retained");
    if (kind==4) {
        CL(!handle_set_cloexec(creator,fd,false),"exec private CLOEXEC deliberately cleared");
        handle_private_exec_latch(creator);
        CL(loom_private_close_observe_for_test(l,NULL)==LCLOSE_NEW,"exec commit latch starts no scheduler work");
        CL(handle_close_on_exec(creator)==1,"exec forces private handle removal");fd=-1;
    } else {CL(!handle_close(creator,fd),"actual final handle close returns");fd=-1;}
    CL(loom_close_wait_state(l,LCLOSE_PARKED),"close parks on retained local owner");
    spin_lock(&l->lock);
    bool retired=held[0]->locally_retired && (!held[1] || held[1]->locally_retired);
    bool attached=!l->service_requests.detached;
    bool immutable=!held[1] || l->service_requests.records[held[1]->request.index].result==(s32)scope[1].slot;
    spin_unlock(&l->lock);
    CL(retired && attached && immutable,"close ends actual cursors but preserves borrow and earlier success");
    u64 turns=0,after=0;loom_private_close_observe_for_test(l,&turns);
    for (u32 i=0;i<64;++i) sched();
    CL(loom_private_close_observe_for_test(l,&after)==LCLOSE_PARKED && after==turns,
       "parked close consumes no extra retirement turns");
    if(kind==3) {
        // A parked ring must not monopolize the one existing retirer thread.
        u32 charged=creator->as->page_count;
        other=loom_create_private(creator,2,2,true);
        CL(other,"close second ring construction");++goal;
        loom_handle_closed(other);
        CL(loom_close_wait_state(other,LCLOSE_DONE),"another ring completes while first parks");
        loom_unref(other);other=NULL;
        CL(lp_wait(goal-1) && creator->as->page_count==charged,
           "another ring final destruction is not starved by parked close");
        CL(loom_private_close_observe_for_test(l,NULL)==LCLOSE_PARKED,
           "first ring still waits for its own retained entry");
    }
    for (u32 i=0;i<2;++i) if(held[i]) {
        CL(!loom_engine_put(l,scope[i],held[i]),"close returns actual external entry");held[i]=NULL;
    }
    if (kind==5 || kind==6) {
        CL(loom_close_wait_state(l,LCLOSE_PARKED),"close waits for fixed or unindexed owner");
        spin_lock(&l->lock);attached=!l->service_requests.detached;spin_unlock(&l->lock);
        CL(attached,"physical borrower or constructor preserves consumer");
        if (kind==5) CL(!loom_private_fixed_release(l,&fixed),"last fixed return resumes real close worker");
        if (kind==6) {CL(!loom_private_connect_rollback(prepared[1]),"unindexed rollback resumes real close worker");prepared[1]=NULL;}
    }
    CL(loom_close_wait_state(l,LCLOSE_DONE),"real close worker completes without peer or CQ progress");
    CL(loom_engine_empty(l->service_engine) && !l->service_requests.occupied &&
       l->service_requests.detached,"close removed every owned protocol and request");
    CL(creator->as->private_rings==1 && creator->as->page_count>=owner_pages,
       "observer retains image guard until its own final return");
    u64 freed=srvconn_total_freed();
    handle_put(&observer);l=NULL;
    CL(lp_wait(goal) && creator->as->page_count==baseline && !creator->as->private_rings,
       "close final free refunds exact creator image and child metadata");
    CL(srvconn_total_freed()==freed,"server endpoints survive complete local ring retirement");
done:
    if(other){loom_handle_closed(other);loom_unref(other);}
    // Every cleanup pointer still has either the original owner or observer root.
    if (l) {
        for (u32 i=0;i<2;++i) if(held[i]) {(void)loom_engine_put(l,scope[i],held[i]);held[i]=NULL;}
        for (u32 i=0;i<2;++i) if(prepared[i]) {(void)loom_private_connect_rollback(prepared[i]);prepared[i]=NULL;}
        if(fixed.owner)(void)loom_private_fixed_release(l,&fixed);
        if(va)(void)sys_burrow_detach_for_proc(creator,va,PAGE_SIZE);
        if(fd>=0){handle_close(creator,fd);fd=-1;}
        else if(*owner)loom_handle_closed(l);
    }
    if(observer.obj)handle_put(&observer);
    for(u32 i=0;i<2;++i)if(server[i])srvconn_unref(server[i]);
#undef CL
    return error;
}
