#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_io.h>
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
static u8 storage[4096];
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
 CHECK(allocations==1 && charged==1 && !l.lock.value && !o->fixed.owner && !o->fixed_dropping && !o->source_borrow && !o->fid_claim,"storage free after physical holds");
 --allocations;free(ptr);
}
int p9_client_submit_async(struct p9_client *c,struct p9_rpc *r,p9_session_build_fn build,void *ctx) {
 (void)c;(void)r;(void)build;(void)ctx;++wire_effects;extinction("preparation sent wire");return -P9_E_IO;
}
/* ACTUAL_FIXED */
/* ACTUAL_FID_ALLOCATOR */
/* ACTUAL_CONSTRUCTOR */
/* ACTUAL_RPC */
/* ACTUAL_ADMISSION */
/* ACTUAL_IO */
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
int main(void) {
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
