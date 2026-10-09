// Actual native target, caller snapshot, weighted connection admission and
// charged preallocated client. No server thread: preparation must perform no
// transport I/O and must leave the backlog empty. This does not exercise CONNECT
// publication, request binding, handshake progress or retirement of active work.
static const char *loom_protocol_fixture(void) {
    const char *error = NULL;
    struct Proc *p = test_proc_make(), *poster = test_proc_make();
    struct SrvRegistry *reg = NULL;
    struct Spoor *root = NULL;
    struct Loom *l = NULL;
    struct loom_service_protocol *prepared = NULL;
    struct loom_service_ref refs[2] = {{0},{0}};
    u64 goal = loom_private_retired();
    u32 base = p ? p->as->page_count : 0;
    u32 old_budget = p ? p->as->page_budget : 0;
    bool owner_dropped = false;
#define PO(c,m) do { if (!(c)) { error=m; goto done; } } while (0)
    PO(p && poster,"protocol fixture processes");
    p->principal_id = 42;
    proc_mark_may_post_service(poster);
    reg = srv_registry_create();
    PO(reg,"protocol fixture registry");
    root = devsrv_attach_registry(reg);
    PO(root,"protocol navigation root");
    root->flag |= CWALKONLY;
    const char *names[2] = {"small","large"};
    for (u32 n=0;n<2;++n)
        PO(devsrv_post_listener(poster,root,names[n],5,SRV_MODE_9P,n==1,false,false)>=0,
           "protocol strict native posts");
    spoor_ref(root);
    int fd = handle_alloc(p,KOBJ_SPOOR,RIGHT_READ,root);
    if (fd < 0) spoor_clunk(root);
    PO(fd>=0,"protocol navigation descriptor");
    l=loom_create_private(p,2,2,true);
    if (l) ++goal;
    PO(l,"protocol private owner");
    for (u32 n=0;n<2;++n) {
        struct loom_service_target in = { .size=sizeof(in), .version=1,
                                          .registry_fd=fd, .name_len=5 };
        for (u32 i=0;i<5;++i) in.name[i]=names[n][i];
        PO(!loom_private_target_prepare(l,p,&in,&refs[n]),"protocol target prepare");
        PO(!loom_private_target_publish(l,refs[n]),"protocol target publish");
    }
    u32 owner_pages = p->as->page_count;
    u64 freed = srvconn_total_freed();
    loom_private_protocol_fault_arm_for_test(1);
    PO(loom_private_protocol_prepare(l,refs[0],10,10,&prepared)==-T_E_TIMEDOUT &&
       !prepared && !loom_private_protocol_fault_pending_for_test(),
       "expired preparation consumes fault without ownership");
    PO(srvconn_total_freed()==freed && p->as->page_count==owner_pages,
       "expired preparation allocates no connection or pages");
    p->state=PROC_STATE_ZOMBIE;
    int gone = loom_private_protocol_prepare(l,refs[0],0,0,&prepared);
    p->state=PROC_STATE_ALIVE;
    PO(gone==-T_E_ACCES && !prepared,"dead creator cannot borrow worker authority");
    for (u32 n=0;n<2;++n) {
        u32 msize = n ? SRVCONN_BULK_MSIZE : SRVCONN_MSIZE;
        u32 expected = (u32)burrow_backing_pages(sizeof(*prepared)) +
                       (u32)burrow_backing_pages(msize)*2u;
        struct SrvService *svc = srv_lookup_in(reg,names[n],5);
        for (u32 stage=1;stage<=3u;++stage) {
            loom_private_protocol_fault_arm_for_test(stage);
            PO(loom_private_protocol_prepare(l,refs[n],1000,1,&prepared)==-T_E_NOMEM &&
               !prepared && !loom_private_protocol_fault_pending_for_test(),
               "protocol allocation refusal leaves output untouched");
            ++freed;
            PO(srvconn_total_freed()==freed,"protocol failure frees native endpoint");
            PO(p->as->page_count==owner_pages && srv_backlog_depth(svc)==0,
               "protocol failure refunds complete charge without admission");
        }
        spin_lock(&p->as->lock);p->as->page_budget=owner_pages;spin_unlock(&p->as->lock);
        int limited=loom_private_protocol_prepare(l,refs[n],0,0,&prepared);
        spin_lock(&p->as->lock);p->as->page_budget=old_budget;spin_unlock(&p->as->lock);
        ++freed;
        PO(srvconn_total_freed()==freed,"budget refusal frees native endpoint");
        PO(limited==-T_E_NOMEM && !prepared && p->as->page_count==owner_pages,
           "captured ordinary principal pays image budget");
        PO(!loom_private_protocol_prepare(l,refs[n],1000,1,&prepared),
           "real preallocated protocol prepare");
        PO(prepared && prepared->identity.stripes==p->stripes &&
           prepared->identity.access.principal_id==42 &&
           prepared->admission.conn->peer_stripes==p->stripes &&
           prepared->admission.conn->peer_pid==p->pid,
           "protocol and native connection capture actual caller");
        PO(prepared->owner==l && !prepared->admission.published &&
           !srv_backlog_depth(svc) && !prepared->admission.conn->c2s.count &&
           !prepared->admission.conn->s2c.count,
           "preparation publishes no backlog or protocol bytes");
        PO(prepared->charged_pages==expected && p->as->page_count==owner_pages+expected,
           "entire owner and msize storage charge precedes allocation");
        PO(prepared->client.progress==&prepared->progress &&
           prepared->client.out_buf_cap==msize && !prepared->client.out_buf_owned &&
           prepared->client.transport.recv_buf==prepared->recv_storage &&
           prepared->client.out_buf==prepared->send_storage &&
           prepared->client.out_buf!=prepared->client.out_buf_inline,
           "exclusive client uses charged provided storage without degradation");
        PO(prepared->client.transport.ops.send(NULL,NULL,0)<0 &&
           prepared->client.transport.ops.recv(NULL,NULL,0)<0 &&
           prepared->client.transport.ops.close(NULL)<0,
           "legacy transport access fails locally");
        // This is a guard test, not an actual publication or retirement witness.
        prepared->admission.published=true;
        int busy=loom_private_protocol_release_unused(prepared);
        prepared->admission.published=false;
        PO(busy==-T_E_BUSY && p->as->page_count==owner_pages+expected,
           "unused cleanup cannot assert admitted protocol quiescence");
        PO(!loom_private_protocol_release_unused(prepared),"unused protocol releases locally");
        prepared=NULL;
        ++freed;
        PO(srvconn_total_freed()==freed,"unused release frees native endpoint");
        PO(p->as->page_count==owner_pages && !srv_backlog_depth(svc),
           "unused protocol refunds exact image after storage release");
    }
    PO(!loom_private_protocol_prepare(l,refs[0],0,0,&prepared),"last owner protocol prepare");
    loom_exec_latch(l);
    struct loom_service_protocol *refused=NULL;
    PO(loom_private_protocol_prepare(l,refs[0],0,0,&refused)<0 && !refused,
       "close refuses new protocol construction");
    loom_unref(l);owner_dropped=true;
    PO(loom_private_retired()<goal && p->as->private_rings,
       "prepared protocol retains image and closing owner");
    PO(!loom_private_protocol_release_unused(prepared),"last protocol drops owner last");
    prepared=NULL;l=NULL;
    PO(lp_wait(goal) && p->as->page_count==base && !p->as->private_rings,
       "final protocol owner retires and refunds ring");
done:
    loom_private_protocol_fault_arm_for_test(0);
    if (p) { spin_lock(&p->as->lock);p->as->page_budget=old_budget;spin_unlock(&p->as->lock); }
    if (prepared) (void)loom_private_protocol_release_unused(prepared);
    if (l && !owner_dropped) loom_unref(l);
    if (!lp_wait(goal) && !error) error="protocol cleanup retirement timeout";
    if (root) spoor_clunk(root);
    test_proc_drop(p);test_proc_drop(poster);
    if (reg) srv_registry_unref(reg);
#undef PO
    return error;
}
void test_loom_private_protocol(void);
void test_loom_private_protocol(void) {
    const char *error=loom_protocol_fixture();
    TEST_ASSERT(!error,error);
}
