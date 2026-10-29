static struct Loom l;
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_request records[4];
static struct loom_service_protocol protocol;
static struct loom_service_ref scope;
static struct loom_service_operation a,b,c,d;
static unsigned checks;
#define C(x,m) do{if(!(x)){fprintf(stderr,"FAIL %s\n",m);return 1;}++checks;}while(0)
static void reset(void) {
 memset(&l,0,sizeof(l));memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));
 memset(records,0,sizeof(records));memset(&protocol,0,sizeof(protocol));
 memset(&a,0,sizeof(a));memset(&b,0,sizeof(b));memset(&c,0,sizeof(c));memset(&d,0,sizeof(d));
 l.service_slots=&slots;l.service_nodes=&nodes;
 l.service_requests=(struct loom_request_table){.records=records,.capacity=4};
 if(loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope) ||
    loom_service_node_publish(&slots,&nodes,scope))abort();
 nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 protocol.owner=&l;protocol.scope=scope;protocol.connect_bound=protocol.indexed=true;
}
static void prepare(struct loom_service_operation *o) {
 o->owner=&l;o->protocol=&protocol;o->scope=scope;
 if(loom_request_prepare(&l.service_requests,&slots,&nodes,scope,77,100,1,&o->request) ||
    loom_request_hold_owner(&l.service_requests,o->request))abort();
 o->expect.kind=P9_TREAD;o->expect.fid=7;o->expect.count=4;
}
static void capture(struct loom_service_operation *o) {
 u8 bytes[4]={1,2,3,4};
 struct p9_dispatch_result reply={.kind=P9_TREAD,.fid=7,.read_count=4,.read_data=bytes};
 loom_rpc_capture(&o->rpc,0,&reply);memset(bytes,0,sizeof(bytes));
}
int main(void) {
 reset();prepare(&a);prepare(&b);prepare(&c);
 u8 buf[4]={0};a.payload=buf;a.payload_capacity=4;
 struct loom_request_ref original=a.request;
 a.request.identity+=((u64)1<<32);
 C(!rpc_record(&a) && loom_rpc_enqueue_locked(&a,2)==-T_E_NOENT && !protocol.op_count,"full request identity");a.request=original;
 records[a.request.index].owners=0;
 C(loom_rpc_enqueue_locked(&a,2)==-T_E_NOENT,"physical owner required");records[a.request.index].owners=1;
 C(loom_rpc_enqueue_locked(&a,100)==-T_E_TIMEDOUT && !protocol.op_count && records[0].phase==LREQ_PREPARED,"expired enqueue unchanged");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ATTACH;
 C(loom_rpc_enqueue_locked(&a,2)==-T_E_CANCELED,"admission requires READY");nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 C(!loom_rpc_enqueue_locked(&a,2) && records[0].phase==LREQ_ACTIVE && records[0].borrows==1 && protocol.op_head==&a && protocol.op_tail==&a,"enqueue owns active borrow and list");
 C(loom_rpc_enqueue_locked(&a,2)==-T_E_BUSY && protocol.op_count==1,"duplicate enqueue denied");
 C(!loom_rpc_enqueue_locked(&b,2) && !loom_rpc_enqueue_locked(&c,2) && protocol.op_head==&a && protocol.op_tail==&c && a.next==&b && b.prev==&a && b.next==&c && c.prev==&b && protocol.op_count==3,"bounded doubly linked admission");
 prepare(&d);protocol.op_count=4;
 C(loom_rpc_enqueue_locked(&d,2)==-T_E_NOSPC && records[3].phase==LREQ_PREPARED,"queue capacity refusal");protocol.op_count=3;
 C(loom_rpc_begin_locked(&a,3)==-T_E_BUSY,"sole driver required");protocol.driving=true;
 C(loom_rpc_begin_locked(&a,100)==-T_E_TIMEDOUT && a.phase==LRPC_QUEUED,"expiry before wire");
 C(!loom_rpc_begin_locked(&a,3) && a.rpc.on_complete==loom_rpc_capture && a.phase==LRPC_INFLIGHT,"callback armed before submit");
 C(loom_rpc_begin_locked(&a,3)==-T_E_BUSY,"no duplicate submission");
 C(loom_rpc_settle_locked(&a)==-T_E_BUSY,"inflight callback retains owner");
 capture(&a);
 C(a.phase==LRPC_INFLIGHT && a.callback_seen && !a.capture_error && a.reply.result==4 && buf[0]==1 && buf[3]==4 && records[0].phase==LREQ_ACTIVE && records[0].borrows==1,"callback copies bytes without terminal");
 a.rpc.sending=true;
 C(loom_rpc_settle_locked(&a)==-T_E_BUSY,"pending sender blocks settlement");a.rpc.sending=false;
 C(!loom_rpc_settle_locked(&a) && !a.rpc.on_complete && a.phase==LRPC_SETTLING,"client returned before settling");
 C(loom_rpc_begin_locked(&a,3)==-T_E_BUSY,"settling cannot resubmit");
 C(!loom_request_borrow(&l.service_requests,&slots,&nodes,a.request),"extra physical borrower");
 C(loom_rpc_check_finish_locked(&a,4,4)==-T_E_BUSY && loom_rpc_finish_locked(&a,4,4)==-T_E_BUSY,"all physical borrowers must end");
 C(!loom_request_put(&l.service_requests,a.request),"extra borrow returns");
 C(loom_rpc_finish_locked(&a,4,100)==-T_E_TIMEDOUT && records[0].phase==LREQ_ACTIVE && records[0].borrows==1,"deadline beats captured success");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;nodes.nodes[scope.slot].reason=-T_E_TIMEDOUT;
 C(loom_rpc_finish_locked(&a,4,5)==-T_E_CANCELED && records[0].phase==LREQ_ACTIVE,"abort beats captured success");
 C(!loom_rpc_finish_locked(&a,-T_E_TIMEDOUT,100) && a.phase==LRPC_TERMINAL && records[0].result==-T_E_TIMEDOUT && !records[0].borrows && records[0].owners==1,"negative terminal ends borrow retains storage");
 C(loom_rpc_finish_locked(&a,4,5)==-T_E_BUSY && records[0].result==-T_E_TIMEDOUT,"terminal immutable");
 C(loom_rpc_unlink_locked(&a)==-T_E_BUSY && protocol.op_head==&a && protocol.op_count==3,"CQ backpressure retains discoverable owner");
 C(!loom_rpc_settle_locked(&b) && loom_rpc_finish_locked(&b,0,4)==-T_E_IO,"queued cancellation cannot fabricate success");
 C(!loom_rpc_finish_locked(&b,-T_E_CANCELED,4),"queued cancellation terminal");
 C(!loom_request_deliver(&l.service_requests,b.request),"middle terminal delivered");
 C(!loom_rpc_unlink_locked(&b) && a.next==&c && c.prev==&a && protocol.op_count==2 && b.phase==LRPC_RETIRED,"middle unlink retains neighbors");
 C(loom_request_reap(&l.service_requests,&slots,b.request)==-T_E_BUSY,"delivery and unlink do not free physical owner");
 C(!loom_request_drop_owner(&l.service_requests,b.request) && !loom_request_reap(&l.service_requests,&slots,b.request),"physical destruction permits record reuse");
 C(loom_rpc_unlink_locked(&b)==-T_E_NOENT,"stale unlinked request denied");
 C(!loom_request_deliver(&l.service_requests,a.request) && !loom_rpc_unlink_locked(&a) && protocol.op_head==&c && !c.prev && protocol.op_tail==&c,"head unlink");
 C(!loom_rpc_settle_locked(&c) && !loom_rpc_finish_locked(&c,-T_E_CANCELED,5),"last queued cancel");
 slots.closing=true;l.service_closing=true;
 C(!loom_requests_detach(&l.service_requests,&slots) && !loom_request_discard(&l.service_requests,c.request),"closed consumer discard");
 C(!loom_rpc_unlink_locked(&c) && !protocol.op_head && !protocol.op_tail && !protocol.op_count,"last unlink empties list");
 reset();prepare(&a);a.payload=buf;a.payload_capacity=4;
 C(!loom_rpc_enqueue_locked(&a,2),"retry admission");protocol.driving=true;
 C(!loom_rpc_begin_locked(&a,3),"synchronous error arm");
 loom_rpc_capture(&a.rpc,-T_E_AGAIN,(void *)1);
 C(a.callback_seen && a.reply.result==-T_E_AGAIN && !a.capture_error,"synchronous error captures without parser");
 C(!loom_rpc_requeue_locked(&a) && !a.callback_seen && !a.rpc.on_complete && records[0].borrows==1 && protocol.op_count==1,"retry preserves one obligation and borrow");
 C(!loom_rpc_begin_locked(&a,4),"retry begins once");capture(&a);
 capture(&a);
 C(a.capture_error==-T_E_IO && a.reply.result==4,"duplicate callback fails closed");
 C(loom_rpc_requeue_locked(&a)==-T_E_BUSY,"invalid capture cannot requeue");
 C(!loom_rpc_settle_locked(&a) && loom_rpc_finish_locked(&a,4,5)==-T_E_IO,"invalid capture cannot commit success");
 C(loom_rpc_finish_locked(&a,-T_E_IO,5)==-T_E_BUSY,"malformed capture requires scope abort");
 protocol.abort_reason=-T_E_IO;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 C(!loom_rpc_finish_locked(&a,-T_E_IO,5),"invalid capture cancellation");
 reset();prepare(&a);a.payload=buf;a.payload_capacity=4;
 C(!loom_rpc_enqueue_locked(&a,2),"success admission");protocol.driving=true;
 C(!loom_rpc_begin_locked(&a,3),"success arm");capture(&a);
 C(!loom_rpc_settle_locked(&a) && !loom_rpc_check_finish_locked(&a,4,4) && !loom_rpc_finish_locked(&a,4,4) && records[0].result==4,"validated captured success commits");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 C(loom_rpc_finish_locked(&a,-T_E_CANCELED,5)==-T_E_BUSY && records[0].result==4,"earlier success survives abort");
 printf("PASS RPC owner: %u checks\n",checks);return 0;
}
