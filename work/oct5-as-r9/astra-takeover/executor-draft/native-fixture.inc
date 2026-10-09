// Actual existing SQPOLL thread, native poll hooks and creator ticket. The
// responder injects a partial first response and complete second handshake in
// the registered-before-sleep window; no explicit engine visit drives either.
static struct {
    struct Loom *l;
    struct SrvConn *server[2];
    u64 parks;
    bool injected, done;
    const char *error;
} executor_probe;
static void loom_executor_boundary(struct Loom *l,u32 stage) {
    if (stage!=5 || l!=executor_probe.l) return;
    __atomic_fetch_add(&executor_probe.parks,1,__ATOMIC_RELEASE);
    if (__atomic_exchange_n(&executor_probe.injected,true,__ATOMIC_ACQ_REL)) return;
    u8 partial=19;
    if (srvconn_server_send(executor_probe.server[0],&partial,1)!=1 ||
        lc_reply(executor_probe.server[1],P9_TVERSION,P9_NOTAG) ||
        lc_reply(executor_probe.server[1],P9_TATTACH,0))
        executor_probe.error="executor real peer injection failed";
    __atomic_store_n(&executor_probe.done,true,__ATOMIC_RELEASE);
}
static bool loom_executor_wait_node(struct Loom *l,struct loom_service_ref ref,u32 state) {
    u64 end=timer_now_ns()+5000000000ull;
    do {
        spin_lock(&l->lock);
        bool found=l->service_nodes->nodes[ref.slot].state==state;
        spin_unlock(&l->lock);
        if(found)return true;
        sched();
    } while(timer_now_ns()<end);
    return false;
}
static const char *loom_executor_case(struct Loom **owner,struct Proc *creator,
    struct Proc *poster,struct SrvRegistry *reg,struct loom_service_ref targets[2],
    u64 goal,u32 baseline) {
    const char *error=NULL;
    struct Loom *l=*owner;
    struct loom_service_ref scope[2]={{0},{0}};
    struct loom_service_protocol *prepared=NULL,*held[2]={NULL,NULL};
    struct SrvConn *server[2]={NULL,NULL};
    struct loom_service_reserve reserve={.size=sizeof(reserve),.version=1,.kind=LOOM_SERVICE_SCOPE};
    struct Handle observer={0};int fd=-1;
    int workers=creator->loom_sqpoll_count;
    struct loom_ring_hdr *h=(void *)(l->ring_kva+l->hdr_off);
#define EX(c,m) do {if(!(c)){error=m;goto done;}}while(0)
    for (u32 i=0;i<2;++i) {
        EX(!loom_private_node_prepare(l,creator,&reserve,&scope[i]) &&
           !loom_private_node_publish(l,scope[i]),"executor reserves exact scopes");
        u64 now=timer_now_ns();
        EX(!loom_private_connect_prepare(l,scope[i],targets[i],77+i,
           i?0:now+2000000000ull,now,&prepared),"executor charged protocol preparation");
        EX(!loom_engine_adopt(prepared,now),"executor indexed adoption");prepared=NULL;
        EX(!loom_engine_get(l,scope[i],&held[i]),"executor fixture retains observation pin");
        server[i]=srv_accept_blocking(srv_lookup_in(reg,i?"large":"small",5),poster->stripes);
        EX(server[i]==held[i]->admission.conn,"executor retained real peer");
    }
    EX(!loom_post_cqe(l,900,0,0) && !loom_post_cqe(l,901,0,0),"executor starts behind full CQ");
    fd=handle_alloc(creator,KOBJ_LOOM,RIGHT_READ|RIGHT_WRITE,l);
    EX(fd>=0,"executor handle adopts ring root");*owner=NULL;
    EX(!handle_get(creator,fd,&observer),"executor observer lifetime");
    executor_probe.l=l;executor_probe.server[0]=server[0];executor_probe.server[1]=server[1];
    executor_probe.parks=0;executor_probe.injected=false;executor_probe.done=false;executor_probe.error=NULL;
    loom_private_close_hook_for_test(loom_executor_boundary);
    EX(loom_start_sqpoll(l)==-T_E_OPNOTSUPP,"ordinary private worker activation remains refused");
    EX(!loom_private_start_executor(l) && creator->loom_sqpoll_count==workers+1 &&
       l->service_engine->worker_ticket.stripes==creator->stripes,"existing creator budget charges actual private worker");
    EX(loom_private_start_executor(l)==-T_E_BUSY,"second private executor refused");
    EX(loom_executor_wait_node(l,scope[1],LOOM_SERVICE_READY),"registered peer wake drives second scope without explicit progress");
    EX(__atomic_load_n(&executor_probe.done,__ATOMIC_ACQUIRE) && !executor_probe.error,
       executor_probe.error?executor_probe.error:"executor injected real registration-window replies");
    spin_lock(&l->lock);
    bool fair=l->service_nodes->nodes[scope[0].slot].state!=LOOM_SERVICE_RETIRED &&
        l->service_requests.records[held[1]->request.index].phase==LREQ_TERMINAL;
    spin_unlock(&l->lock);
    EX(fair,"partial first peer cannot starve second and full CQ retains success");
    u64 parks=__atomic_load_n(&executor_probe.parks,__ATOMIC_ACQUIRE);
    for(u32 i=0;i<64;++i)sched();
    EX(__atomic_load_n(&executor_probe.parks,__ATOMIC_ACQUIRE)<=parks+1,
       "READY and full CQ park without repeated self wake");
    __atomic_store_n(&h->cq_head,1,__ATOMIC_RELEASE);
    wakeup(&l->sqpoll_park); // internal equivalent of the future ENTER wake
    u64 end=timer_now_ns()+5000000000ull;bool delivered=false;
    do {
        spin_lock(&l->lock);
        delivered=l->service_requests.records[held[1]->request.index].phase==LREQ_DELIVERED;
        spin_unlock(&l->lock);
        if(delivered)break;sched();
    }while(timer_now_ns()<end);
    EX(delivered,"consumer drain wakes retained terminal delivery");
    EX(loom_executor_wait_node(l,scope[0],LOOM_SERVICE_RETIRED),"absolute timeout retires partial peer without a native edge");
    spin_lock(&l->lock);
    bool timed=l->service_requests.records[held[0]->request.index].result==-T_E_TIMEDOUT &&
        l->service_requests.records[held[0]->request.index].phase==LREQ_TERMINAL;
    spin_unlock(&l->lock);
    EX(timed,"timeout terminal survives refilled CQ");
    EX(!handle_close(creator,fd),"actual handle close starts local retirement");fd=-1;
    for(u32 i=0;i<2;++i) {EX(!loom_engine_put(l,scope[i],held[i]),"fixture returns exact entry pins");held[i]=NULL;}
    EX(loom_close_wait_state(l,LCLOSE_DONE) && __atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE),
       "close completion includes actual private executor exit");
    EX(!l->service_engine->wait_active && !l->service_engine->wait_mask &&
       loom_engine_empty(l->service_engine),"close removes all wait hooks and protocol owners");
    loom_private_close_hook_for_test(NULL);executor_probe.l=NULL;
    u64 freed=srvconn_total_freed();
    handle_put(&observer);l=NULL;
    EX(lp_wait(goal) && creator->loom_sqpoll_count==workers && creator->as->page_count==baseline &&
       !creator->as->private_rings,"actual worker join precedes exact ticket and image refund");
    EX(srvconn_total_freed()==freed,"peer endpoints survive worker and local ring reclamation");
done:
    loom_private_close_hook_for_test(NULL);
    if(l) {
        if(prepared)(void)loom_private_connect_rollback(prepared);
        for(u32 i=0;i<2;++i)if(held[i])(void)loom_engine_put(l,scope[i],held[i]);
        if(fd>=0)(void)handle_close(creator,fd);
        else if(*owner)loom_handle_closed(l);
        // An already executing boundary callback may outlive clearing the hook.
        // Retain its peer refs and globals until the actual executor exits.
        if(l->sqpoll) {
            u64 stop=timer_now_ns()+5000000000ull;
            while(!__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE) && timer_now_ns()<stop)sched();
            if(!__atomic_load_n(&l->sqpoll_exited,__ATOMIC_ACQUIRE))
                extinction("executor fixture could not quiesce its callback");
        }
        if(observer.obj)handle_put(&observer);
    }
    for(u32 i=0;i<2;++i)if(server[i])srvconn_unref(server[i]);
    executor_probe.l=NULL;
#undef EX
    return error;
}
