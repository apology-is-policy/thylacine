#include <thylacine/loom_service_action.h>
#include <thylacine/loom_service_connect.h>
#include <thylacine/poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_retirement.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/loom.h>
#include <thylacine/loom_service_fids.h>
#include <thylacine/loom_service_engine.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/burrow.h>
#include <thylacine/errno.h>
// Physical-address translation, allocator, Proc snapshot and final backing-drop
// boundaries are host doubles. All exported data structures use real headers.
#define page_to_pa(p) ((paddr_t)(p))
#define pa_to_kva(p) ((void *)(p))
static unsigned checks;
#define CHECK(x,m) do{if(!(x)){fprintf(stderr,"FAIL %s\n",m);exit(1);}++checks;}while(0)
static struct Loom l;
static struct Proc creator;
static struct AddrSpace as,other_as;
static struct loom_service_protocol protocol;
static struct loom_service_request records[4];
static struct loom_slot_table slots;
static struct loom_service_node_table nodes;
static struct loom_service_pool_table pools;
static struct loom_service_engine engine;
static struct loom_service_ref scope,child,dest;
static struct Burrow backing,ring;
static u8 storage[4096],cq_storage[1024];
void poll_waiter_list_wake(struct poll_waiter_list *w){CHECK(!l.lock.value && w==&l.cq_waiters,"CQ wake outside Ll");}
static int driver_pins;
static u64 clock_now=3,drop_time;
static u64 timer_now_ns(void){return clock_now;}
static int roots=1,pins,charged,allocations,notifications,wire_effects;
static bool allocation_fault,close_during_alloc;
static bool watch_free;
static struct loom_request_ref freeing_request;
static struct loom_service_operation *drop_watch;
int wakeup(struct Rendez *r){(void)r;return 0;}
void srvconn_teardown(struct SrvConn *c){CHECK(c==(struct SrvConn *)&protocol && !l.lock.value,"native teardown outside Ll");}
static void extinction(const char *s){fprintf(stderr,"INVARIANT %s\n",s);exit(86);}
bool loom_is_private(const struct Loom *x){return x==&l;}
void loom_ref(struct Loom *x){CHECK(x==&l,"retained Loom root");++roots;}
void loom_unref(struct Loom *x){CHECK(x==&l && !l.lock.value && roots>1,"Loom drop outside lock");--roots;}
void loom_private_close_changed(struct Loom *x){CHECK(x==&l&&!l.lock.value,"notification outside Ll");++notifications;}
void burrow_ref(struct Burrow *b){CHECK(l.lock.value && b==&backing && b->handle_count>0,"backing pin while table locked");++b->handle_count;}
static void loom_private_drop_buffer(struct AddrSpace *a,struct Burrow *b) {
 CHECK(!l.lock.value && a==&as && b==&backing && b->handle_count>0,"physical backing drop outside Ll");
 if(drop_watch){
  CHECK(drop_watch->fixed_dropping && !drop_watch->fixed.owner && l.service_fixed_settling==1,"unlink is not physical settlement");
  CHECK(loom_admission_cancel_locked(drop_watch,-T_E_CANCELED,true,5)==-T_E_BUSY,"terminal blocked during physical drop");
  CHECK(loom_private_io_drop_fixed(drop_watch)==-T_E_BUSY,"second physical drop refused");
 }
 if(drop_time)clock_now=drop_time;
 --b->handle_count;
}
bool proc_service_snapshot_by_stripes(u64 stripes,const struct AddrSpace *a,struct ProcServiceIdentity *out) {
 CHECK(!l.lock.value && stripes==99 && a==&as,"fresh creator snapshot boundary");
 memset(out,0,sizeof(*out));out->access.principal_id=42;out->access.primary_gid=7;return true;
}
int loom_engine_get(struct Loom *x,struct loom_service_ref s,struct loom_service_protocol **out) {
 CHECK(x==&l&&!l.lock.value,"engine get unlocked");
 if(s.slot!=scope.slot || s.incarnation!=scope.incarnation)return -T_E_NOENT;
 ++pins;*out=&protocol;return 0;
}
int loom_engine_put(struct Loom *x,struct loom_service_ref s,struct loom_service_protocol *p) {
 CHECK(x==&l && s.incarnation==scope.incarnation && p==&protocol && pins==1+driver_pins && !charged && !allocations && !l.lock.value,"entry pin through refund");--pins;return 0;
}
bool addrspace_charge_pages(struct AddrSpace *a,u32 n,bool exempt) {
 CHECK(a==&as && a->lock.value && n==1 && !exempt && pins==1,"charge before fixed resources");++charged;return true;
}
void addrspace_uncharge_pages(struct AddrSpace *a,u32 n) {
 CHECK(a==&as && a->lock.value && n==1 && charged==1 && !allocations,"refund after actual storage free");
 if(watch_free)CHECK(records[freeing_request.index].owners==1 && records[freeing_request.index].identity==freeing_request.identity,"physical owner retained through refund");
 --charged;
}
static void *kmalloc(size_t n,u32 flags) {
 CHECK(n==PAGE_SIZE && !l.lock.value && charged==1,"charged page allocation");(void)flags;
 if(allocation_fault)return NULL;
 if(close_during_alloc){l.service_closing=true;slots.closing=true;}
 ++allocations;return calloc(1,n);
}
static void kfree(void *ptr) {
 struct loom_service_operation *o=ptr;
 CHECK(allocations==1 && charged==1 && !l.lock.value && !o->fixed.owner && !o->fixed_dropping && !o->pool_stream && !o->source_borrow && !o->fid_claim,"storage free after physical holds");
 if(watch_free){
  freeing_request=o->request;
  CHECK(o->phase==LRPC_RETIRED && records[o->request.index].identity==o->request.identity && records[o->request.index].owners==1 && records[o->request.index].phase==LREQ_DELIVERED,"physical owner retained through kfree");
 }
 --allocations;free(ptr);
}
/* ACTUAL_NODES */
/* ACTUAL_FIDS */
/* ACTUAL_WIRE */
/* ACTUAL_POOL */
/* ACTUAL_FIXED */
/* ACTUAL_FID_ALLOCATOR */
/* ACTUAL_CONSTRUCTOR */
/* ACTUAL_RPC */
/* ACTUAL_ADMISSION */
/* ACTUAL_IO */
/* ACTUAL_PROVIDED */
/* ACTUAL_RETIREMENT */
/* ACTUAL_TERMINAL */
/* ACTUAL_ABORT */
/* ACTUAL_POOL_DESTROY */
static void client_send_progress_signal(struct p9_client *c){(void)c;}
/* ACTUAL_SUBMIT */
/* ACTUAL_SCOPE_ABORT */
/* ACTUAL_READY */
/* ACTUAL_ACTION */
static struct loom_sqe sqe(unsigned opcode) {
 struct loom_sqe s={.opcode=opcode,.handle_idx=child.slot,.user_data=77};
 s._resv1[1]=child.incarnation;s._resv1[3]=100;
 if(opcode==LOOM_OP_READ || opcode==LOOM_OP_WRITE){s.len=4;s.offset=512;s._resv1[0]=32;}
 if(opcode==LOOM_OP_WALK){s.offset=dest.slot;s._resv1[2]=dest.incarnation;}
 return s;
}
static void reset(void) {
 CHECK(roots==1 && !pins && !charged && !allocations,"previous roots balanced");
 memset(&l,0,sizeof(l));memset(&creator,0,sizeof(creator));memset(&protocol,0,sizeof(protocol));
 memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));memset(&pools,0,sizeof(pools));memset(records,0,sizeof(records));
 memset(&backing,0,sizeof(backing));memset(&ring,0,sizeof(ring));memset(storage,0,sizeof(storage));
 backing.type=BURROW_TYPE_ANON;backing.pages=(struct page *)storage;backing.size=sizeof(storage);backing.handle_count=1;
 l.magic=LOOM_MAGIC;l.refcount=1;memset(&engine,0,sizeof(engine));l.service_engine=&engine;l.service_as=&as;l.service_creator=99;l.cq_entries=4;
 l.service_slots=&slots;l.service_nodes=&nodes;l.service_pools=&pools;l.ring=&ring;
 l.service_requests=(struct loom_request_table){.records=records,.capacity=4};
 l.n_reg_buf=1;l.reg_buf[0]=(struct loom_reg_buffer){.burrow=&backing,.kva=storage+128,.len=256};
 creator.magic=PROC_MAGIC;creator.as=&as;creator.stripes=99;
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope) && !loom_service_node_publish(&slots,&nodes,scope),"scope setup");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;CHECK(!loom_slot_pin(&slots,scope,LOOM_SERVICE_SCOPE),"engine scope pin");
 CHECK(!loom_fid_bind_root(&slots,&nodes,scope,41),"root map");
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&child) && !loom_service_node_publish(&slots,&nodes,child),"child setup");
 CHECK(!loom_fid_claim_walk(&slots,&nodes,child,scope,987) && !loom_fid_commit_walk(&slots,&nodes,child,scope,987,73) && !loom_fid_release_claim(&slots,&nodes,child,987),"child map");
 CHECK(!loom_fid_claim_open(&slots,&nodes,child,scope,988) && !loom_fid_commit_open(&slots,&nodes,child,scope,988,2) && !loom_fid_release_claim(&slots,&nodes,child,988),"child read/write");
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&dest) && !loom_service_node_publish(&slots,&nodes,dest),"destination setup");
 protocol.owner=&l;protocol.scope=scope;protocol.indexed=protocol.connect_bound=true;protocol.client.magic=P9_CLIENT_MAGIC;protocol.client.next_fid=100;
 allocation_fault=close_during_alloc=false;drop_watch=NULL;notifications=wire_effects=0;
}
// Admitted destruction is an explicit fixture boundary; the real READY/close
// destructor remains pending. Its prescribed physical-free-before-refund order
// is checked by the allocator/image/entry doubles above.
static void destroy_terminal(struct loom_service_operation *o) {
 struct loom_request_ref r=o->request;
 CHECK(!loom_request_deliver(&l.service_requests,r) && !loom_rpc_unlink_locked(o),"terminal delivery/unlink");
 kfree(o);spin_lock(&as.lock);addrspace_uncharge_pages(&as,1);spin_unlock(&as.lock);
 CHECK(!loom_request_drop_owner(&l.service_requests,r) && !loom_request_reap(&l.service_requests,&slots,r),"physical owner ends");
 CHECK(!loom_engine_put(&l,scope,&protocol),"last entry pin");loom_unref(&l);
}
static int fixed_cases(void) {
 reset();struct loom_sqe s=sqe(LOOM_OP_LOPEN);struct loom_service_operation *o=(void *)1;
 creator.as=&other_as;CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES && o==(void *)1 && !pins,"exact creator image refusal");creator.as=&as;
 creator.stripes++;CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES,"exact creator incarnation refusal");creator.stripes--;s=sqe(LOOM_OP_READ);
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && o->program.wire.payload==storage+160 && o->fixed.extent.offset==160 && backing.handle_count==2 && l.service_fixed_count==1 && roots==3,"prepared canonical slice owns independent pin");
 CHECK(!wire_effects && !protocol.op_count && records[0].phase==LREQ_PREPARED,"prepare has no wire or queue effect");
 s.len=200;s.offset=900;s._resv1[0]=0;
 CHECK(o->program.wire.count==4 && o->program.wire.offset==512 && o->program.wire.payload_capacity==4,"program detached from later SQE edits");
 CHECK(loom_private_operation_release_unused(o)==-T_E_BUSY,"raw unused free refuses fixed hold");
 CHECK(loom_private_io_publish(o,100)==-T_E_TIMEDOUT && o->fixed.owner==&l && !protocol.op_count,"fresh publication deadline retains provisional hold");
 CHECK(!loom_private_io_release_prepared(o) && roots==1 && backing.handle_count==1 && !l.service_fixed_count,"unpublished cleanup frees both roots");

 reset();s=sqe(LOOM_OP_WRITE);CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o),"write preparation");
 // Simulate replacement dropping the old registered table pin. The accepted
 // extent owns its own pin and never consults the new table for its pointer.
 --backing.handle_count;l.reg_buf[0].burrow=NULL;l.n_reg_buf=0;
 CHECK(o->program.wire.payload==storage+160 && backing.handle_count==1,"table replacement preserves accepted extent");
 CHECK(!loom_private_io_publish(o,2) && protocol.op_head==o && roots==3,"publish transfers owner and balances wake root");
 CHECK(loom_private_io_release_prepared(o)==-T_E_BUSY && loom_private_io_drop_fixed(o)==-T_E_BUSY,"queued ownership cannot release payload");
 protocol.driving=true;CHECK(!loom_rpc_settle_locked(o),"queued cancellation settles");
 CHECK(loom_admission_cancel_locked(o,-T_E_CANCELED,true,4)==-T_E_BUSY,"terminal waits for physical fixed release");
 drop_watch=o;CHECK(!loom_private_io_drop_fixed(o) && !o->program.wire.payload && !o->program.wire.payload_capacity && backing.handle_count==0 && roots==2,"physical payload release clears pointer");drop_watch=NULL;
 CHECK(!loom_admission_cancel_locked(o,-T_E_CANCELED,true,4),"terminal follows actual buffer drop");destroy_terminal(o);

 reset();s=sqe(LOOM_OP_WALK);s.len=3;s._resv1[0]=32;memcpy(storage+160,"abc",3);
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !memcmp(o->program.wire.component,"abc",3) && !o->fixed.owner && backing.handle_count==1 && protocol.client.next_fid==101,"walk owns copied component and burns wire fid");
 memset(storage+160,'z',3);CHECK(!memcmp(o->program.wire.component,"abc",3),"walk never rereads mutable backing");
 CHECK(!loom_private_io_release_prepared(o) && protocol.client.next_fid==101,"rollback never rewinds wire fid");
 reset();s=sqe(LOOM_OP_WALK);s.len=2;memcpy(storage+128,"..",2);o=(void *)1;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && o==(void *)1 && backing.handle_count==1 && roots==1 && !l.service_fixed_count && protocol.client.next_fid==100,"invalid component unwinds before fid allocation");
 reset();s=sqe(LOOM_OP_WALK);protocol.client.next_fid=P9_NOFID;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_NOSPC && roots==1 && !charged,"exhausted monotone allocator unwinds");
 reset();s=sqe(LOOM_OP_READ);s.len=0;s._resv1[0]=256;
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !o->fixed.owner && !o->program.wire.payload && !o->program.wire.payload_capacity && backing.handle_count==1,"empty extent validates end offset without pin");
 CHECK(!loom_private_io_release_prepared(o),"empty prepared cleanup");s._resv1[0]=257;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && roots==1,"empty extent still bounds offset");
 s._resv1[0]=0;s.buf_idx_or_off=1;l.reg_buf[1]=l.reg_buf[0]; // stale metadata must not authorize an unregistered slot
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL,"empty extent still validates registration");
 reset();s=sqe(LOOM_OP_READ);allocation_fault=true;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_NOMEM && roots==1 && !pins && !charged,"allocation failure no fixed side effect");
 reset();s=sqe(LOOM_OP_READ);close_during_alloc=true;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_CANCELED && roots==1 && !pins && !charged && backing.handle_count==1,"close during allocation unwinds before fixed pin");
 reset();s=sqe(LOOM_OP_READ);s.len=300;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && !l.service_requests.occupied && roots==1,"invalid fixed slice unwinds storage");
 reset();s=sqe(LOOM_OP_READ);s.flags=LOOM_SQE_BUFFER_SELECT;s._resv1[0]=0;s._resv1[2]=1;
 CHECK(loom_private_io_prepare(&l,&creator,&s,1,&o)==-T_E_OPNOTSUPP && !pins,"pooled frontend not silently fixed");
 reset();s=sqe(LOOM_OP_LOPEN);s.offset=2;
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && o->program.phase==LPROGRAM_STAT && o->program.wire.mode==2,"open preserves mode and staged DAC");
 CHECK(!loom_private_io_release_prepared(o),"open prepared cleanup");
 printf("PASS fixed SQE preparation: %u checks\n",checks);return 0;
}
static struct loom_service_ref pool_ref;
static void make_pool(u32 count) {
 struct loom_pool_extent ext[2]={{&backing,512,64},{&backing,640,64}};
 CHECK(!loom_service_pool_prepare(&slots,&pools,ext,count,&pool_ref) &&
       !loom_service_pool_publish(&slots,&pools,pool_ref),"pool setup ledger");
 // Registration already pinned canonical backing; this fixture's physical
 // registration boundary holds those pins independently of operation storage.
 backing.handle_count+=count;l.receipt_size=32;
 memset(cq_storage,0,sizeof(cq_storage));l.ring_kva=cq_storage;l.hdr_off=0;l.cqe_off=128;l.receipt_off=256;
}
static struct loom_sqe pool_sqe(bool multi) {
 struct loom_sqe s=sqe(LOOM_OP_READ);s.flags=LOOM_SQE_BUFFER_SELECT|(multi?LOOM_SQE_MULTISHOT:0);
 s._resv1[0]=0;s.buf_idx_or_off=pool_ref.slot;s._resv1[2]=pool_ref.incarnation;return s;
}
static void completed_read(struct loom_service_operation *o,u32 count) {
 // Parsed completion boundary: synthetic valid dispatch record, actual owner
 // capture + program consumption. No real wire/progress proof in this fixture.
 o->program.wire.built=true;o->program.wire.expect=(struct loom_service_reply_expect){.kind=P9_TREAD,.fid=73,.new_fid=73,.count=4};
 o->submitted=true;struct p9_dispatch_result d={.kind=P9_TREAD,.fid=73,.new_fid=73,.read_count=count,.read_data=(const u8 *)"abc"};
 loom_rpc_capture(&o->rpc,0,&d);enum loom_program_action action;
 CHECK(!loom_rpc_advance(o,&action) && action==LPROGRAM_TERMINAL && !loom_rpc_settle_locked(o),"parsed read capture settles actual program");
}
static void free_provided(struct loom_service_operation *o) {
 struct loom_request_ref r=o->request;
 if(records[r.index].phase==LREQ_TERMINAL)CHECK(!loom_request_deliver(&l.service_requests,r),"fixture plain terminal delivery");
 CHECK(!loom_rpc_unlink_locked(o) && !loom_provided_detach_locked(o),"retired owner returns only stream ref");
 kfree(o);spin_lock(&as.lock);addrspace_uncharge_pages(&as,1);spin_unlock(&as.lock);
 CHECK(!loom_request_drop_owner(&l.service_requests,r) && !loom_request_reap(&l.service_requests,&slots,r),"provided physical owner ends");
 CHECK(!loom_engine_put(&l,scope,&protocol),"provided final engine pin");loom_unref(&l);
}
static int provided_cases(void) {
 CHECK(!fixed_cases(),"fixed regressions");
 struct loom_service_operation *o;struct loom_sqe s;
 reset();make_pool(2);s=pool_sqe(false);o=(void *)1;
 creator.as=&other_as;CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES && o==(void *)1 && !pins,"provided creator image exact");creator.as=&as;
 creator.stripes++;CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_ACCES && !pins,"provided creator incarnation exact");creator.stripes--;
 s.len=65;
 CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && o==(void *)1 && !pools.pools[pool_ref.slot].streams && roots==1,"all member bounds precede stream ownership");
 s.len=4;pools.bank.cells[1].extent.length=2;
 CHECK(loom_provided_prepare(&l,&creator,&s,1,&o)==-T_E_INVAL && roots==1,"later short member also rejects whole request");pools.bank.cells[1].extent.length=64;
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && o->pool_stream && !o->pool_busy && !pools.bank.next_nonce && !wire_effects,"prepare retains stream without consuming member");
 CHECK(loom_private_operation_release_unused(o)==-T_E_BUSY,"raw free refuses pool stream");
 CHECK(loom_private_io_drop_fixed(o)==-T_E_BUSY,"fixed drop cannot clear pooled ownership");
 CHECK(loom_service_pool_reap(&slots,&pools,pool_ref)==-T_E_BUSY,"prepared stream prevents pool reap");
 CHECK(!loom_provided_release_prepared(o) && !pools.pools[pool_ref.slot].streams,"prepared rollback returns stream");

 reset();make_pool(2);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"single published");protocol.driving=true;
 CHECK(loom_rpc_begin_locked(o,3)==-T_E_BUSY && o->phase==LRPC_QUEUED,"raw begin refuses unclaimed pool");
 CHECK(!loom_provided_begin_locked(o,3) && o->pool_busy && o->receipt.lease==1 && o->program.wire.payload==storage+512,"selected canonical member before read");
 CHECK(loom_provided_release_busy_locked(o)==-T_E_BUSY,"inflight member cannot return");
 completed_read(o,3);CHECK(!memcmp(storage+512,"abc",3),"captured reply in selected member");
 CHECK(loom_admission_commit_locked(o,4)==-T_E_BUSY,"ordinary finish cannot bypass pool commit");
 CHECK(!loom_provided_commit_locked(o,4) && o->pool_pending && o->pool_terminal && records[o->request.index].phase==LREQ_TERMINAL && !o->program.wire.payload,"single positive commits terminal and pending payload");
 CHECK(loom_provided_detach_locked(o)==-T_E_BUSY,"terminal bookkeeping retains stream");
 // Isolate the pending guard from the independent phase guard, modeling a
 // destructor ordering bug that unlinked before actual payload publication.
 o->phase=LRPC_RETIRED;
 CHECK(loom_provided_detach_locked(o)==-T_E_BUSY,"terminal bookkeeping retains stream");o->phase=LRPC_TERMINAL;
 struct loom_service_buffer_receipt receipt=o->receipt;
 CHECK(loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt)==-T_E_NOENT,"pending payload cannot return");
 l.cq_tail=4;CHECK(loom_provided_deliver(o)==-T_E_AGAIN && o->pool_pending,"full CQ preserves committed payload");
 protocol.abort_reason=-T_E_CANCELED;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 ((struct loom_ring_hdr *)cq_storage)->cq_head=4;
 CHECK(!loom_provided_deliver(o) && !o->pool_pending && records[o->request.index].phase==LREQ_DELIVERED,"committed single survives scope abort and delivers once");
 struct loom_cqe *cq=(void *)(cq_storage+l.cqe_off);struct loom_service_buffer_receipt *receipts=(void *)(cq_storage+l.receipt_off);
 CHECK(cq[0].result==3 && cq[0].user_data==77 && cq[0].flags==LOOM_CQE_SERVICE_BUFFER && receipts[0].lease==receipt.lease && receipts[0].pool.slot==receipt.pool.slot && receipts[0].pool.incarnation==receipt.pool.incarnation && receipts[0].member==receipt.member && !receipts[0].reserved && ((struct loom_ring_hdr *)cq_storage)->cq_tail==5,"single CQE and receipt share publication");
 CHECK(loom_provided_deliver(o)==-T_E_NOENT,"single delivery never duplicates terminal");free_provided(o);
 CHECK(loom_service_pool_reap(&slots,&pools,pool_ref)==-T_E_BUSY,"scope retirement never returns lease");
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt) && loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt)==-T_E_NOENT,"late exact return only once");

 reset();make_pool(1);s=pool_sqe(true);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"multi published");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"multi claim");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4) && o->phase==LRPC_SETTLING && records[o->request.index].phase==LREQ_ACTIVE,"MORE preserves active terminal obligation");
 CHECK(loom_retirement_post_terminal(o)==-T_E_BUSY,"terminal delivery cannot overtake pending MORE");
 CHECK(loom_rpc_rearm_read_locked(o,5)==-T_E_BUSY,"undelivered MORE prevents next shot");
 l.service_requests.detached=true;
 CHECK(loom_provided_deliver(o)==-T_E_CANCELED && o->pool_pending,"detached consumer cannot publish");l.service_requests.detached=false;
 CHECK(!loom_provided_deliver(o) && cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER),"MORE delivery has explicit receipt");receipt=o->receipt;
 CHECK(loom_rpc_rearm_read_locked(o,100)==-T_E_TIMEDOUT,"rearm checks absolute deadline");
 CHECK(!loom_rpc_rearm_read_locked(o,5) && o->program.wire.offset==512,"rearm retains original offset");
 CHECK(loom_provided_begin_locked(o,6)==-T_E_AGAIN && o->phase==LRPC_QUEUED && !o->pool_busy && pools.bank.next_nonce==1,"empty pool waits without shot or nonce");
 CHECK(loom_provided_begin_locked(o,100)==-T_E_TIMEDOUT,"empty pool deadline remains absolute");
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt) && !loom_provided_begin_locked(o,7) && o->receipt.lease==2,"return enables fresh monotone lease");
 CHECK(loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt)==-T_E_NOENT,"old lease never returns next shot");
 completed_read(o,0);CHECK(!loom_provided_commit_locked(o,8) && !o->pool_busy && !o->pool_pending && !o->pool_terminal && records[o->request.index].result==0,"EOF returns busy without payload lease");free_provided(o);

 reset();make_pool(1);s=pool_sqe(true);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"abort multi publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"abort multi claim");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4),"success before abort");receipt=o->receipt;
 protocol.abort_reason=-T_E_CANCELED;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 CHECK(!loom_admission_cancel_locked(o,-T_E_CANCELED,true,5) && o->pool_pending && !records[o->request.index].borrows && !o->source_borrow,"abort commits terminal without CQ-dependent local borrow");
 CHECK(loom_retirement_post_terminal(o)==-T_E_BUSY && !l.cq_tail,"terminal waits behind committed MORE");
 CHECK(!loom_provided_deliver(o) && loom_rpc_rearm_read_locked(o,5)==-T_E_BUSY,"settled abort delivers MORE but cannot rearm");
 CHECK(!loom_retirement_post_terminal(o) && cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER) && cq[1].result==-T_E_CANCELED && !cq[1].flags && !receipts[1].lease,"MORE precedes zero-receipt negative terminal");free_provided(o);
 CHECK(loom_service_pool_reap(&slots,&pools,pool_ref)==-T_E_BUSY && !loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt),"lease outlives request and scope");

 reset();make_pool(1);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"late abort publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"late abort claim");completed_read(o,3);
 protocol.abort_reason=-T_E_CANCELED;
 CHECK(loom_provided_commit_locked(o,4)==-T_E_CANCELED && o->pool_busy && !o->pool_pending,"abort before commit preserves only busy cleanup");
 CHECK(!loom_provided_release_busy_locked(o) && !o->program.wire.payload && !o->program.wire.payload_capacity && !loom_admission_cancel_locked(o,-T_E_CANCELED,true,5),"quiescent abort releases unpublished busy");free_provided(o);

 reset();make_pool(1);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"exhaust publish");protocol.driving=true;pools.bank.next_nonce=UINT64_MAX;
 CHECK(loom_provided_begin_locked(o,3)==-T_E_NOSPC && !o->pool_busy,"lease exhaustion admits no read");CHECK(!loom_rpc_settle_locked(o) && !loom_admission_cancel_locked(o,-T_E_NOSPC,true,4),"exhaust terminal");free_provided(o);
 reset();make_pool(1);s=pool_sqe(false);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"retry publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"retry claim");receipt=o->receipt;
 // Model only a proven local no-wire submit refusal. The actual adapter's
 // distinction from remote EAGAIN is qualified in wire-draft controls.
 o->submitted=true;o->program.wire.local_again=true;loom_rpc_capture(&o->rpc,-T_E_AGAIN,NULL);
 enum loom_program_action action;
 CHECK(!loom_rpc_advance(o,&action) && action==LPROGRAM_NEXT && !loom_rpc_requeue_locked(o,4),"local no-wire refusal requeues");
 CHECK(!loom_provided_begin_locked(o,5) && o->receipt.lease==receipt.lease && pools.bank.next_nonce==1,"local retry retains the same busy member");
 o->submitted=true;o->program.wire.local_again=false;loom_rpc_capture(&o->rpc,-T_E_AGAIN,NULL);
 CHECK(!loom_rpc_advance(o,&action) && action==LPROGRAM_TERMINAL && !loom_rpc_settle_locked(o),"remote AGAIN is terminal");
 CHECK(!loom_provided_release_busy_locked(o) && !loom_admission_cancel_locked(o,-T_E_AGAIN,true,6),"negative completion returns only unpublished busy");free_provided(o);
 reset();make_pool(1);s=pool_sqe(true);CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"undelivered positive publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"undelivered positive claim");completed_read(o,3);
 CHECK(!loom_provided_release_busy_locked(o),"positive bytes discarded before cancellation");
 CHECK(loom_rpc_rearm_read_locked(o,4)==-T_E_BUSY,"new shot requires delivered MORE proof");
 CHECK(!loom_admission_cancel_locked(o,-T_E_CANCELED,true,5),"uncommitted positive can only terminate");free_provided(o);
 printf("PASS provided ownership: %u checks\n",checks);return 0;
}
static unsigned abort_calls,abort_completions;
static void backend_abort(void *ctx) {
 CHECK(ctx==&protocol && protocol.client.lock.value && !l.lock.value &&
       !protocol.rpc_quiesced && !protocol.progress.io.tx,"transport abort precedes quiescence");++abort_calls;
}
static void abort_callback(struct p9_rpc *rpc,int status,struct p9_dispatch_result *r) {
 CHECK(!rpc->sending && !protocol.progress.sending && !protocol.progress.io.tx &&
       !protocol.rpc_quiesced && !r && status==-P9_E_IO,"TX owner ends before abort callback");++abort_completions;
}
static void setup_abort(void) {
 protocol.client.progress=&protocol.progress;protocol.admission.conn=(struct SrvConn *)&protocol;
 protocol.progress.io.transport=&protocol.client.transport;
 protocol.progress.io.ops.abort=backend_abort;protocol.progress.io.ops.ctx=&protocol;
 protocol.progress.io.tx=storage;protocol.progress.io.tx_len=10;
 abort_calls=abort_completions=0;
}
static void close_latch(void) {
 l.service_closing=true;loom_slots_close(&slots);protocol.abort_reason=-T_E_CANCELED;
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;protocol.driving=true;
}
static int retirement_cases(void) {
 reset();setup_abort();struct p9_rpc rpc={.sending=true,.on_complete=abort_callback};
 protocol.progress.sending=&rpc;protocol.client.inflight[0]=&rpc;
 p9_client_progress_abort(&protocol.client);
 CHECK(abort_calls==1 && abort_completions==1 && !rpc.sending && !protocol.client.inflight[0],"actual progress abort releases in-flight TX owner");
 // A reply can already have removed the tag while TX still borrows its owner.
 reset();setup_abort();rpc=(struct p9_rpc){.sending=true};protocol.progress.sending=&rpc;
 p9_client_progress_abort(&protocol.client);
 CHECK(!rpc.sending && !abort_completions,"abort releases early-replied TX owner too");
 CHECK(!provided_cases(),"provided regressions");
 struct loom_service_operation *o;struct loom_sqe s;
 reset();setup_abort();s=sqe(LOOM_OP_WRITE);
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !loom_private_io_publish(o,2),"fixed close publish");protocol.driving=true;
 CHECK(!loom_admission_begin_locked(o,3),"fixed close begin");
 o->submitted=true;o->rpc.sending=true;protocol.progress.sending=&o->rpc;protocol.client.inflight[0]=&o->rpc;
 close_latch();
 CHECK(loom_retirement_close_operation(o,4)==-T_E_BUSY,"close requires actual quiescence");
 CHECK(!loom_retirement_quiesce(&protocol) && protocol.rpc_quiesced && o->callback_seen && !o->rpc.sending && o->rpc.on_complete!=NULL,"actual abort returns before owner settling");
 CHECK(!loom_retirement_quiesce(&protocol) && abort_calls==1,"quiescence is idempotent");
 drop_watch=o;watch_free=true;
 CHECK(!loom_retirement_close_operation(o,5) && roots==1 && !pins && !charged && !protocol.op_count && backing.handle_count==1 && !l.service_requests.occupied,"fixed close consumes after actual buffer and storage return");watch_free=false;drop_watch=NULL;

 reset();setup_abort();make_pool(1);s=pool_sqe(true);
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"pending close publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"pending close begin");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4),"pending close commit");
 protocol.abort_reason=-T_E_CANCELED;CHECK(!loom_retirement_quiesce(&protocol),"scope abort actually quiesces");
 CHECK(loom_provided_discard_closed_locked(o)==-T_E_BUSY && o->pool_pending,"scope abort cannot discard pending success");
 close_latch();CHECK(loom_requests_detach(&l.service_requests,&slots)==-T_E_BUSY,"global detach blocked by active pending borrow");
 // Another constructor/owner remains independently visible. Closing this
 // operation must not detach the whole table or consume that reservation.
 struct loom_request_ref other;slots.closing=false;nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;
 CHECK(!loom_request_prepare(&l.service_requests,&slots,&nodes,scope,88,0,0,&other),"other reservation setup");
 CHECK(!loom_request_hold_owner(&l.service_requests,other),"other physical reservation");slots.closing=true;nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 watch_free=true;CHECK(!loom_retirement_close_operation(o,5) && !l.service_requests.detached && records[other.index].phase==LREQ_PREPARED && records[other.index].owners==1 && l.service_requests.occupied==1 && pools.bank.cells[0].phase==LMEMBER_AVAILABLE && !pools.pools[pool_ref.slot].streams && !l.cq_tail,"pending close precedes global detach without losing other owner");watch_free=false;
 CHECK(!loom_request_drop_owner(&l.service_requests,other) && !loom_request_rollback(&l.service_requests,&slots,other),"other constructor owns its own rollback");

 reset();setup_abort();make_pool(1);s=pool_sqe(false);
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"single pending close publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"single pending close begin");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4),"single pending close commit");close_latch();CHECK(!loom_retirement_quiesce(&protocol),"single pending quiesce");
 watch_free=true;CHECK(!loom_retirement_close_operation(o,5) && !l.cq_tail && pools.bank.cells[0].phase==LMEMBER_AVAILABLE && !l.service_requests.occupied,"single committed terminal discarded once on whole close");watch_free=false;

 reset();setup_abort();make_pool(1);s=pool_sqe(false);
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"leased close publish");protocol.driving=true;
 CHECK(!loom_provided_begin_locked(o,3),"leased close begin");completed_read(o,3);CHECK(!loom_provided_commit_locked(o,4) && !loom_provided_deliver(o),"leased close deliver");
 struct loom_service_buffer_receipt receipt=o->receipt;close_latch();CHECK(!loom_retirement_quiesce(&protocol),"leased close quiesce");
 watch_free=true;CHECK(!loom_retirement_close_operation(o,5) && pools.bank.cells[0].phase==LMEMBER_LEASED && !pools.pools[pool_ref.slot].streams,"operation destruction does not auto-return lease");watch_free=false;
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt),"exact lease still returnable before final ring destruction");

 reset();setup_abort();s=sqe(LOOM_OP_LOPEN);CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o),"unpublished owner setup");close_latch();CHECK(!loom_retirement_quiesce(&protocol),"unpublished scope quiesce");
 CHECK(loom_retirement_close_operation(o,5)==-T_E_BUSY && allocations==1 && records[o->request.index].phase==LREQ_PREPARED,"close never steals unpublished constructor");
 CHECK(!loom_private_io_release_prepared(o),"constructor retains exclusive cleanup");
 reset();setup_abort();s=sqe(LOOM_OP_READ);s.len=0;
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !loom_private_io_publish(o,2),"queued empty close setup");close_latch();
 CHECK(loom_retirement_close_operation(o,3)==-T_E_BUSY && allocations==1,"queued close independently requires client quiescence");
 CHECK(!loom_retirement_quiesce(&protocol),"queued quiescence");
 l.service_closing=false;CHECK(loom_retirement_close_operation(o,4)==-T_E_BUSY,"ring close latch required");l.service_closing=true;
 slots.closing=false;CHECK(loom_retirement_close_operation(o,4)==-T_E_BUSY && o->phase==LRPC_QUEUED,"slot close latch required");slots.closing=true;
 CHECK(!loom_rpc_settle_locked(o) && !loom_admission_cancel_locked(o,-T_E_CANCELED,true,4),"queued terminal setup");
 CHECK(loom_private_operation_release_retired(o)==-T_E_BUSY && allocations==1,"terminal storage survives undelivered request");
 watch_free=true;CHECK(!loom_retirement_close_operation(o,5),"queued terminal closes after exact discard");watch_free=false;
 reset();make_pool(2);close_latch();
 CHECK(loom_private_pools_destroy(&l)==-T_E_BUSY && backing.handle_count==3,"live ring cannot discard pool leases");
 CHECK(!loom_pool_stream_get(&pools.pools[pool_ref.slot]),"final pool stream setup");
 struct loom_service_buffer_receipt lease;CHECK(!loom_pool_claim(&pools.bank,&pools.pools[pool_ref.slot],4,&lease) && !loom_pool_commit(&pools.bank,&pools.pools[pool_ref.slot],&lease,3,77,false) && !loom_pool_deliver(&pools.bank,&pools.pools[pool_ref.slot],&lease),"final leased pool setup");
 close_latch();l.refcount=0;
 CHECK(loom_private_pools_destroy(&l)==-T_E_BUSY && backing.handle_count==3,"final pool stream blocks reclamation");
 CHECK(!loom_pool_stream_put(&pools.pools[pool_ref.slot]),"final stream return");
 pools.bank.cells[1].phase=LMEMBER_BUSY;
 CHECK(loom_private_pools_destroy(&l)==-T_E_BUSY && pools.bank.cells[0].phase==LMEMBER_LEASED && backing.handle_count==3,"validate whole bank before any lease invalidation");pools.bank.cells[1].phase=LMEMBER_AVAILABLE;
 CHECK(!loom_private_pools_destroy(&l) && backing.handle_count==1 && loom_service_pools_empty(&pools) && slots.slots[pool_ref.slot].phase==LSLOT_FREE,"final exclusive destruction physically returns every member pin");
 printf("PASS retirement ownership: %u checks\n",checks);return 0;
}

// Explicit fixture boundary: native progress/readiness/demux is controlled.
// Actual submit still builds real frames/tags and retains the TX cursor. The
// fixture completes that physical hold before supplying parsed replies below.
static int progress_result,progress_calls;
int p9_client_progress_step(struct p9_client *c,u64 now) {
 CHECK(c==&protocol.client && protocol.driving && !l.lock.value && driver_pins==1 && now==clock_now,"IO under independent driver lifetime");
 ++progress_calls;return progress_result;
}
static u8 out_frame[8192];
static void action_setup(void) {
 reset();setup_abort();protocol.progress.io.tx=NULL;protocol.progress.io.tx_len=0;
 CHECK(!p9_session_init(&protocol.client.session,41,8192),"action session setup");
 protocol.client.session.state=P9_SESS_OPEN;protocol.client.session.negotiated_msize=4096;
 protocol.client.session.bound_fids[0]=41;protocol.client.session.bound_fids[1]=73;protocol.client.session.n_bound_fids=2;
 protocol.client.out_buf=out_frame;protocol.client.out_buf_cap=sizeof(out_frame);
 protocol.progress.io.frame_limit=4096;protocol.client.transport.state=P9_TRANS_PROGRESS;protocol.client.transport.magic=P9_TRANSPORT_MAGIC;
 memset(cq_storage,0,sizeof(cq_storage));l.ring_kva=cq_storage;l.cqe_off=128;l.receipt_off=256;
 clock_now=3;drop_time=0;progress_result=progress_calls=0;
}
static int visit(u64 stale_now,bool io) {
 // Engine integration is not installed. Simulate its separately retained
 // entry/root; unlike the operation's pin these survive consuming FREE/DISCARD.
 ++pins;++roots;driver_pins=1;
 int before=notifications;int rc=loom_ready_step(&protocol,stale_now,io);
 CHECK(!protocol.driving && pins>=1 && roots>=2,"driver pin survives consuming action");
 CHECK(rc<=0 || notifications>before,"transition return wakes executor");
 --pins;--roots;driver_pins=0;return rc;
}
static struct loom_service_operation *action_fixed(unsigned op) {
 struct loom_sqe s=sqe(op);struct loom_service_operation *o;
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !loom_private_io_publish(o,2),"action fixed publish");return o;
}
static struct loom_service_operation *action_pool(bool multi) {
 make_pool(1);struct loom_sqe s=pool_sqe(multi);struct loom_service_operation *o;
 CHECK(!loom_provided_prepare(&l,&creator,&s,1,&o) && !loom_provided_publish(o,2),"action pool publish");return o;
}
static void parsed_reply(struct loom_service_operation *o,s32 result) {
 CHECK(o->phase==LRPC_INFLIGHT && o->submitted && !o->callback_seen,"one submitted callback");
 // These cursor/tag changes stand in for a completed transport frame/demux.
 // They do not qualify actual transport or remote success semantics.
 protocol.progress.sending=NULL;protocol.progress.io.tx=NULL;o->rpc.sending=false;
 protocol.client.inflight[o->rpc.tag]=NULL;
 p9_session_retract_unsent(&protocol.client.session,o->rpc.tag);
 struct p9_dispatch_result r={.kind=o->program.wire.kind,.fid=o->program.wire.fid,.new_fid=o->program.wire.expect.new_fid,
   .read_count=result>0?(u32)result:0,.read_data=(const u8 *)"abc",.write_count=result>0?(u32)result:0};
 loom_rpc_capture(&o->rpc,result<0?result:0,result<0?NULL:&r);
}
static void delivered_free(void) {
 watch_free=true;CHECK(visit(3,false)==1 && !protocol.op_count && roots==1 && !pins && !charged && !allocations,"executor physical free under separate root");watch_free=false;
}
static void finish_aborted(void) {
 CHECK(visit(3,false)==1 && protocol.rpc_quiesced,"executor actually quiesces");
 CHECK(visit(3,false)==1,"executor negative commit");
 CHECK(visit(3,false)==1,"executor negative post");delivered_free();
}
static int action_cases(void) {
 CHECK(!retirement_cases(),"retirement regressions");
 struct loom_service_operation *o;struct loom_cqe *cq=(void *)(cq_storage+128);
 action_setup();o=action_fixed(LOOM_OP_WRITE);
 protocol.driving=true;CHECK(!loom_ready_step(&protocol,3,false) && protocol.driving,"contending executor does not release another driver");protocol.driving=false;
 l.cq_tail=4;CHECK(!visit(3,false) && !o->submitted && o->phase==LRPC_QUEUED,"full CQ blocks fresh submission");l.cq_tail=0;
 CHECK(visit(3,false)==1 && o->phase==LRPC_INFLIGHT && o->submitted && o->rpc.sending && protocol.progress.io.tx==out_frame,"executor submits actual frame");
 CHECK(!visit(3,false) && !o->callback_seen,"inflight waits for actual callback");
 parsed_reply(o,3);CHECK(visit(3,false)==1 && o->phase==LRPC_SETTLING && o->advanced,"executor consumes reply once then settles");
 drop_watch=o;CHECK(visit(3,false)==1 && o->phase==LRPC_TERMINAL && !o->fixed.owner && !o->source_borrow && backing.handle_count==1,"executor commit follows physical fixed drop");drop_watch=NULL;
 CHECK(visit(3,false)==1 && l.cq_tail==1 && cq[0].result==3,"executor posts positive terminal");delivered_free();

 action_setup();o=action_fixed(LOOM_OP_WRITE);clock_now=100;
 CHECK(visit(3,false)==1 && protocol.abort_reason==-T_E_TIMEDOUT && o->phase==LRPC_QUEUED && !o->submitted,"fresh begin deadline refuses wire");finish_aborted();

 action_setup();o=action_fixed(LOOM_OP_WRITE);CHECK(visit(3,false)==1,"drop deadline submit");parsed_reply(o,3);CHECK(visit(3,false)==1,"drop deadline advance");
 drop_time=100;CHECK(visit(3,false)==1 && protocol.abort_reason==-T_E_TIMEDOUT && o->phase==LRPC_SETTLING && !o->fixed.owner,"fresh commit deadline after physical drop");drop_time=0;finish_aborted();

 action_setup();o=action_fixed(LOOM_OP_WRITE);protocol.driving=true;
 CHECK(!loom_admission_begin_locked(o,3),"local retry begin");
 // Actual submit sees occupied cursor before invoking builder, so AGAIN is
 // proven no-wire and may retry. Ready selection normally parks this shortage.
 protocol.progress.io.tx=storage;CHECK(loom_rpc_submit(o)==-T_E_AGAIN && o->callback_seen && !o->program.wire.built && o->program.wire.local_again,"actual local no-wire refusal");protocol.progress.io.tx=NULL;protocol.driving=false;
 clock_now=100;CHECK(visit(3,false)==1 && o->advanced && o->phase==LRPC_SETTLING && protocol.abort_reason==-T_E_TIMEDOUT,"failed fresh requeue settles consumed attempt");finish_aborted();

 action_setup();o=action_pool(true);CHECK(visit(3,false)==1 && o->pool_busy,"executor claims provided member before submit");parsed_reply(o,3);
 CHECK(visit(3,false)==1 && visit(3,false)==1 && o->pool_pending && o->phase==LRPC_SETTLING,"executor commits MORE without terminal");
 struct loom_service_buffer_receipt receipt=o->receipt;
 l.cq_tail=4;CHECK(!loom_private_connect_abort(&protocol,-T_E_CANCELED),"scope abort while full CQ");
 CHECK(visit(3,false)==1 && protocol.rpc_quiesced,"full CQ cannot block quiescence");
 CHECK(visit(3,false)==1 && o->phase==LRPC_TERMINAL && o->pool_pending && !o->source_borrow && !records[o->request.index].borrows,"negative commit releases borrow while MORE waits");
 CHECK(!visit(3,false) && l.cq_tail==4,"full CQ preserves committed MORE");
 ((struct loom_ring_hdr *)cq_storage)->cq_head=4;
 CHECK(visit(3,false)==1 && cq[0].flags==(LOOM_CQE_MORE|LOOM_CQE_SERVICE_BUFFER) && cq[0].result==3,"executor MORE delivered before negative terminal");
 CHECK(visit(3,false)==1 && cq[1].result==-T_E_CANCELED && !cq[1].flags,"executor terminal after MORE");delivered_free();
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt),"leased payload survives executor destruction");

 action_setup();o=action_pool(true);CHECK(visit(3,false)==1,"rearm expiry submit");parsed_reply(o,3);
 CHECK(visit(3,false)==1 && visit(3,false)==1 && visit(3,false)==1 && o->pool_more_delivered,"MORE licenses rearm");clock_now=100;
 CHECK(visit(3,false)==1 && protocol.abort_reason==-T_E_TIMEDOUT && o->phase==LRPC_SETTLING,"fresh rearm deadline bars new shot");finish_aborted();

 action_setup();o=action_pool(true);CHECK(visit(3,false)==1,"normal rearm submit");parsed_reply(o,3);
 CHECK(visit(3,false)==1 && visit(3,false)==1 && visit(3,false)==1,"normal MORE delivery");receipt=o->receipt;
 CHECK(visit(3,false)==1 && o->phase==LRPC_QUEUED && !o->pool_more_delivered,"executor consumes rearm proof");
 CHECK(!visit(3,false) && !o->pool_busy,"empty pool parks without new nonce");
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt) && visit(3,false)==1 && o->receipt.lease!=receipt.lease,"returned member enables next bounded submission");
 parsed_reply(o,0);CHECK(visit(3,false)==1 && visit(3,false)==1 && !o->pool_pending && o->phase==LRPC_TERMINAL,"EOF releases member and terminates");CHECK(visit(3,false)==1,"EOF terminal");delivered_free();

 action_setup();o=action_pool(false);pools.bank.next_nonce=UINT64_MAX;
 CHECK(visit(3,false)==1 && o->phase==LRPC_TERMINAL && records[o->request.index].result==-T_E_NOSPC && !o->submitted && !protocol.abort_reason,"nonce exhaustion is isolated terminal without wire");CHECK(visit(3,false)==1,"exhaust terminal post");delivered_free();

 action_setup();o=action_pool(false);CHECK(visit(3,false)==1,"single submit");parsed_reply(o,3);CHECK(visit(3,false)==1 && visit(3,false)==1 && o->pool_terminal,"single combined commit");
 CHECK(visit(3,false)==1 && l.cq_tail==1 && cq[0].result==3,"single combined delivery");delivered_free();CHECK(l.cq_tail==1,"single terminal never duplicates");

 action_setup();o=action_pool(true);CHECK(visit(3,false)==1,"whole close submit");parsed_reply(o,3);CHECK(visit(3,false)==1 && visit(3,false)==1 && o->pool_pending,"whole close pending");
 l.cq_tail=4;l.service_closing=true;loom_slots_close(&slots);
 CHECK(visit(3,false)==1 && protocol.rpc_quiesced,"whole close first ends writers");watch_free=true;
 CHECK(visit(3,false)==1 && !protocol.op_count && !l.service_requests.occupied && l.cq_tail==4 && pools.bank.cells[0].phase==LMEMBER_AVAILABLE,"whole close discards pending and consumes owner despite full CQ");watch_free=false;

 action_setup();o=action_fixed(LOOM_OP_CLUNK);CHECK(visit(3,false)==1,"CLUNK submit");parsed_reply(o,-T_E_IO);
 CHECK(visit(3,false)==1 && o->phase==LRPC_SETTLING,"CLUNK error advance");
 CHECK(visit(3,false)==1 && protocol.abort_reason==-T_E_IO && o->fid_claim==P9_TCLUNK && !protocol.rpc_quiesced,"CLUNK failure cannot retire before scope quiescence");finish_aborted();

 action_setup();o=action_pool(false);CHECK(visit(3,false)==1,"remote AGAIN submit");parsed_reply(o,-T_E_AGAIN);
 CHECK(visit(3,false)==1 && o->action==LPROGRAM_TERMINAL,"remote AGAIN never retried");
 CHECK(visit(3,false)==1 && o->phase==LRPC_TERMINAL && !protocol.abort_reason,"remote AGAIN isolated terminal");CHECK(visit(3,false)==1,"remote AGAIN post");delivered_free();

 action_setup();o=action_fixed(LOOM_OP_WRITE);CHECK(visit(3,false)==1,"malformed submit");parsed_reply(o,3);o->capture_error=-T_E_IO;
 CHECK(visit(3,false)==1 && o->phase==LRPC_SETTLING && protocol.abort_reason==-T_E_IO,"malformed reply settles and aborts scope");finish_aborted();

 action_setup();o=action_pool(false);CHECK(visit(3,false)==1 && o->rpc.sending && o->pool_busy,"inflight busy cancellation submit");
 CHECK(!loom_private_connect_abort(&protocol,-T_E_CANCELED),"inflight abort latch");
 CHECK(visit(3,false)==1 && !o->rpc.sending && o->callback_seen && protocol.rpc_quiesced,"actual abort ends in-flight callback and TX hold");
 CHECK(visit(3,false)==1 && o->phase==LRPC_TERMINAL && !o->pool_busy && pools.bank.cells[0].phase==LMEMBER_AVAILABLE,"cancellation releases BUSY after actual writer end");
 CHECK(visit(3,false)==1,"inflight cancel post");delivered_free();

 action_setup();CHECK(!visit(3,false) && !progress_calls,"no readiness no speculative IO");
 CHECK(!visit(3,true) && progress_calls==1,"one nonblocking IO quantum can park");progress_result=1;
 CHECK(visit(3,true)==1 && progress_calls==2,"positive IO progress normalized");progress_result=-T_E_IO;
 CHECK(visit(3,true)==1 && protocol.abort_reason==-T_E_IO && !protocol.rpc_quiesced,"IO failure latches scope before writer proof");
 CHECK(visit(3,false)==1 && protocol.rpc_quiesced && !visit(3,true),"quiescent dead scope never reenters IO");
 printf("PASS action executor: %u checks\n",checks);return 0;
}
static struct loom_service_operation *root_op(unsigned op) {
 struct loom_sqe s=sqe(op);s.handle_idx=scope.slot;s._resv1[1]=scope.incarnation;
 if(op==LOOM_OP_LOPEN)s.offset=2;
 struct loom_service_operation *o;
 CHECK(!loom_private_io_prepare(&l,&creator,&s,1,&o) && !loom_private_io_publish(o,2),"root operation admission");return o;
}
static bool open_expire;
static void root_open(u32 unit) {
 struct loom_service_operation *o=root_op(LOOM_OP_LOPEN);
 CHECK(visit(3,false)==1 && o->program.wire.kind==P9_TGETATTR,"open begins with actual stat submission");
 protocol.progress.sending=NULL;protocol.progress.io.tx=NULL;o->rpc.sending=false;
 protocol.client.inflight[o->rpc.tag]=NULL;p9_session_retract_unsent(&protocol.client.session,o->rpc.tag);
 struct p9_dispatch_result r={.kind=P9_TGETATTR,.fid=41,.new_fid=41};
 r.attr.valid=P9_GETATTR_MODE|P9_GETATTR_UID|P9_GETATTR_GID;r.attr.mode=0666;r.attr.uid=42;r.attr.gid=7;
 loom_rpc_capture(&o->rpc,0,&r);
 CHECK(visit(3,false)==1 && o->phase==LRPC_QUEUED && o->program.wire.kind==P9_TLOPEN,"authorized stat advances to LOPEN");
 CHECK(visit(3,false)==1,"actual LOPEN submission");
 protocol.progress.sending=NULL;protocol.progress.io.tx=NULL;o->rpc.sending=false;
 protocol.client.inflight[o->rpc.tag]=NULL;p9_session_retract_unsent(&protocol.client.session,o->rpc.tag);
 r=(struct p9_dispatch_result){.kind=P9_TLOPEN,.fid=41,.new_fid=41,.open_iounit=unit};loom_rpc_capture(&o->rpc,0,&r);
 CHECK(visit(3,false)==1 && o->reply.iounit==unit,"Rlopen captured iounit survives parser");
 if(open_expire) {
  clock_now=100;CHECK(visit(3,false)==1 && protocol.abort_reason==-T_E_TIMEDOUT && !nodes.nodes[scope.slot].fid_iounit && !(nodes.nodes[scope.slot].fid_flags&LFID_OPEN),"expired open publishes neither access nor iounit");finish_aborted();return;
 }
 CHECK(visit(3,false)==1 && nodes.nodes[scope.slot].fid_iounit==unit && (nodes.nodes[scope.slot].fid_flags&LFID_OPEN),"successful open atomically retains negotiated iounit");
 CHECK(visit(3,false)==1,"open terminal");delivered_free();
 ((struct loom_ring_hdr *)cq_storage)->cq_head=l.cq_tail;
}
int main(void) {
 CHECK(!action_cases(),"action regressions");
 u32 units[]={2,0,UINT32_MAX};
 for(unsigned i=0;i<3;++i) {
  action_setup();root_open(units[i]);
  struct loom_service_operation *o=root_op(LOOM_OP_WRITE);
  CHECK(o->program.wire.iounit==units[i],"write admission snapshots negotiated iounit");
  // Metadata remains protected by the borrow in production. This mutation is
  // a hostile fixture boundary proving no later wire lookup changes the copy.
  nodes.nodes[scope.slot].fid_iounit=1;
  CHECK(visit(3,false)==1 && o->program.wire.expect.count==(units[i]==2?2u:4u),"write wire respects negotiated limit and zero semantics");
  parsed_reply(o,(s32)o->program.wire.expect.count);CHECK(visit(3,false)==1 && visit(3,false)==1 && visit(3,false)==1,"limited write completion");delivered_free();
  ((struct loom_ring_hdr *)cq_storage)->cq_head=l.cq_tail;nodes.nodes[scope.slot].fid_iounit=units[i];
  o=root_op(LOOM_OP_READ);CHECK(o->program.wire.iounit==units[i],"read admission snapshots negotiated iounit");
  CHECK(visit(3,false)==1 && o->program.wire.expect.count==(units[i]==2?2u:4u),"read wire respects negotiated limit and zero semantics");
  parsed_reply(o,2);CHECK(visit(3,false)==1 && visit(3,false)==1 && visit(3,false)==1,"limited read completion");delivered_free();
  ((struct loom_ring_hdr *)cq_storage)->cq_head=l.cq_tail;
  protocol.client.session.negotiated_msize=24;o=root_op(LOOM_OP_WRITE);
  CHECK(visit(3,false)==1 && o->program.wire.expect.count==1,"iounit never widens negotiated frame budget");parsed_reply(o,1);
  CHECK(visit(3,false)==1 && visit(3,false)==1 && visit(3,false)==1,"frame limited write completion");delivered_free();
 }
 action_setup();open_expire=true;root_open(99);open_expire=false;
 action_setup();nodes.nodes[child.slot].fid_iounit=2;struct loom_service_operation *pooled=action_pool(true);
 CHECK(visit(3,false)==1 && pooled->program.wire.iounit==2 && pooled->program.wire.expect.count==2,"provided read inherits source negotiated iounit");parsed_reply(pooled,2);
 CHECK(visit(3,false)==1 && visit(3,false)==1 && visit(3,false)==1,"limited provided MORE");
 struct loom_service_buffer_receipt receipt=pooled->receipt;CHECK(visit(3,false)==1,"limited provided rearm");
 CHECK(!loom_pool_return(&pools.bank,&pools.pools[pool_ref.slot],&receipt),"limited provided return");
 CHECK(visit(3,false)==1 && pooled->program.wire.expect.count==2,"rearmed provided read retains negotiated iounit");
 parsed_reply(pooled,0);CHECK(visit(3,false)==1 && visit(3,false)==1 && visit(3,false)==1,"limited provided EOF");delivered_free();
 action_setup();nodes.nodes[child.slot].fid_iounit=123;
 nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;
 CHECK(!loom_fid_retire_aborted(&slots,&nodes,child) && !loom_fid_reap(&slots,&nodes,child) && !nodes.nodes[child.slot].fid_iounit,"reap clears negotiated metadata before slot reuse");
 nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;struct loom_service_ref fresh;
 CHECK(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_FID,scope,&fresh) && fresh.slot==child.slot && fresh.incarnation!=child.incarnation && !nodes.nodes[fresh.slot].fid_iounit,"reused incarnation starts with zero iounit");
 CHECK(!loom_service_node_rollback(&slots,&nodes,fresh),"iounit reuse cleanup");
 printf("PASS negotiated iounit: %u checks\n",checks);return 0;
}
