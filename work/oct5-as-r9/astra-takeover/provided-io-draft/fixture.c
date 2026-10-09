#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_provided.h>
#include <thylacine/loom_service_decode.h>
#include <thylacine/loom.h>
#include <thylacine/loom_service_fids.h>
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
static struct loom_service_ref scope,child,dest;
static struct Burrow backing,ring;
static u8 storage[4096],cq_storage[1024];
void poll_waiter_list_wake(struct poll_waiter_list *w){CHECK(!l.lock.value && w==&l.cq_waiters,"CQ wake outside Ll");}
static int roots=1,pins,charged,allocations,notifications,wire_effects;
static bool allocation_fault,close_during_alloc;
static struct loom_service_operation *drop_watch;
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
 CHECK(x==&l && s.incarnation==scope.incarnation && p==&protocol && pins==1 && !charged && !allocations && !l.lock.value,"entry pin through refund");--pins;return 0;
}
bool addrspace_charge_pages(struct AddrSpace *a,u32 n,bool exempt) {
 CHECK(a==&as && a->lock.value && n==1 && !exempt && pins==1,"charge before fixed resources");++charged;return true;
}
void addrspace_uncharge_pages(struct AddrSpace *a,u32 n) {
 CHECK(a==&as && a->lock.value && n==1 && charged==1 && !allocations,"refund after actual storage free");--charged;
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
 --allocations;free(ptr);
}
int p9_client_submit_async(struct p9_client *c,struct p9_rpc *r,p9_session_build_fn build,void *ctx) {
 (void)c;(void)r;(void)build;(void)ctx;++wire_effects;extinction("preparation sent wire");return -P9_E_IO;
}
/* ACTUAL_POOL */
/* ACTUAL_FIXED */
/* ACTUAL_FID_ALLOCATOR */
/* ACTUAL_CONSTRUCTOR */
/* ACTUAL_RPC */
/* ACTUAL_ADMISSION */
/* ACTUAL_IO */
/* ACTUAL_PROVIDED */
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
 l.magic=LOOM_MAGIC;l.service_as=&as;l.service_creator=99;l.cq_entries=4;
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
int main(void) {
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
 CHECK(loom_admission_cancel_locked(o,-T_E_CANCELED,true,5)==-T_E_BUSY,"cancellation cannot overtake pending MORE");
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
 CHECK(!loom_provided_deliver(o) && loom_rpc_rearm_read_locked(o,5)==-T_E_CANCELED,"abort preserves MORE but bars new shot");
 CHECK(!loom_admission_cancel_locked(o,-T_E_CANCELED,true,5),"terminal follows committed MORE");free_provided(o);
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
