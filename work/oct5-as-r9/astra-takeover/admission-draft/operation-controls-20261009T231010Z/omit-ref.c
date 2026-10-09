
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <thylacine/loom_service_requests.h>
#include <thylacine/errno.h>
#define PAGE_SIZE 4096
#define KP_ZERO 1
#define LOOM_MAGIC 7
#define PRINCIPAL_SYSTEM 0
#define C(x,m) do{if(!(x)){puts(m);exit(11);}}while(0)
typedef struct {int held;} Lock;
static int locks,refs=1,pins,pages,live,fault,freed,refunds,entry_gets;
static void spin_lock(Lock*l){C(!locks && !l->held,"lock nesting");l->held=1;locks++;}
static void spin_unlock(Lock*l){C(locks && l->held,"unlock");l->held=0;locks--;}
struct AddrSpace {Lock lock;};
struct Loom {unsigned magic;bool service_closing;Lock lock;u64 service_creator;struct AddrSpace*service_as;struct loom_slot_table*service_slots;struct loom_service_node_table*service_nodes;struct loom_request_table service_requests;};
struct TestAccess {u64 caps;u32 principal_id,primary_gid,supp_gids[16];u8 supp_gid_count;};
struct ProcServiceIdentity {struct TestAccess access;};
static struct TestAccess fresh;
enum {LRPC_NEW,LRPC_QUEUED};
struct loom_service_protocol {bool connect_bound,locally_retired;s32 abort_reason;};
struct loom_service_operation {struct Loom*owner;struct loom_service_protocol*protocol;struct loom_request_ref request;struct loom_service_ref scope;struct {void*on_complete;bool sending;}rpc;struct {struct TestAccess identity;}program;unsigned phase;void *next,*prev;bool submitted;u8 fid_claim;bool source_borrow;};
static struct Loom l;static struct AddrSpace as;static struct loom_slot_table slots;static struct loom_service_node_table nodes;static struct loom_service_request records[2];static struct loom_service_protocol protocol;static struct loom_service_ref scope;
static bool loom_is_private(struct Loom*x){return x==&l;}
static bool proc_service_snapshot_by_stripes(u64 s,struct AddrSpace*a,struct ProcServiceIdentity*i){C(!locks&&s==123&&a==&as,"creator image");i->access=fresh;return fault!=1;}
static int loom_engine_get(struct Loom*x,struct loom_service_ref s,struct loom_service_protocol**p){C(x==&l&&!locks,"get boundary");entry_gets++;if(fault==2 || s.incarnation!=scope.incarnation)return -T_E_NOENT;pins++;*p=&protocol;return 0;}
static int loom_engine_put(struct Loom*x,struct loom_service_ref s,struct loom_service_protocol*p){C(x==&l&&s.incarnation==scope.incarnation&&p==&protocol&&!locks&&pins==1&&!live&&!pages,"protocol retained through storage refund");pins--;return 0;}
static void loom_ref(struct Loom*x){C(x==&l&&!locks,"ref boundary");refs++;}
static void loom_unref(struct Loom*x){C(x==&l&&!locks&&!live&&!pages&&!pins&&!l.service_requests.occupied,"last Loom drop ordering");refs--;}
static void loom_private_close_changed(struct Loom*x){C(x==&l&&!locks,"wake boundary");}
static void extinction(const char*s){puts(s);exit(12);}
static bool addrspace_charge_pages(struct AddrSpace*a,u32 n,bool exempt){C(a==&as&&as.lock.held&&n==1&&!exempt&&refs==2&&pins==1&&records[0].owners==1,"reserve before charge");if(fault==3)return false;pages++;return true;}
static void addrspace_uncharge_pages(struct AddrSpace*a,u32 n){C(a==&as&&as.lock.held&&n==1&&pages==1&&!live&&records[0].owners==1,"refund after physical free before owner drop");pages--;refunds++;}
static void* kmalloc(size_t n,int flags){C(!locks&&n==PAGE_SIZE&&flags==KP_ZERO&&pages==1&&records[0].owners==1,"one charged buddy page");if(fault==4)return NULL;if(fault==5){l.service_closing=true;slots.closing=true;}if(fault==6){nodes.nodes[scope.slot].state=LOOM_SERVICE_ABORTING;nodes.nodes[scope.slot].reason=-T_E_TIMEDOUT;}live++;return calloc(1,n);}
static void kfree(void*p){C(!locks&&live==1&&pages==1&&pins==1&&refs==2&&records[0].owners==1&&l.service_requests.occupied==1,"physical free retains owner and roots");C(records[0].phase==LREQ_PREPARED,"physical free refuses active request");C(!((struct loom_service_operation*)p)->rpc.on_complete,"physical free refuses callback");C(records[0].identity==((struct loom_service_operation*)p)->request.identity,"physical free exact request");C(loom_request_rollback(&l.service_requests,&slots,((struct loom_service_operation*)p)->request)==-T_E_BUSY,"free boundary blocks request reuse");live--;freed++;free(p);}
// All constructor resources precede any wire effect. Ll never nests an image,
// allocator or client lock. The physical request owner bars reuse even if a
// close/abort driver runs during allocation or destruction outside Ll.

_Static_assert(sizeof(struct loom_service_operation)<=PAGE_SIZE,
               "operation metadata must fit its charged buddy page");

#ifdef KERNEL_TESTS
static u32 operation_fault;
void loom_private_operation_fault_arm_for_test(void) {
    __atomic_store_n(&operation_fault,1,__ATOMIC_RELAXED);
}
bool loom_private_operation_fault_pending_for_test(void) {
    return __atomic_load_n(&operation_fault,__ATOMIC_RELAXED)!=0;
}
static bool operation_fault_take(void) {
    return __atomic_exchange_n(&operation_fault,0,__ATOMIC_RELAXED)!=0;
}
#else
static bool operation_fault_take(void) { return false; }
#endif

static int operation_scope_ready(struct Loom *l,struct loom_service_ref s,
                                  struct loom_service_protocol *p) {
    if (l->service_closing || l->service_slots->closing) return -T_E_CANCELED;
    if (s.reserved || !s.incarnation || s.slot>=LOOM_SERVICE_SLOT_COUNT) return -T_E_NOENT;
    struct loom_service_slot *slot=&l->service_slots->slots[s.slot];
    struct loom_service_node *node=&l->service_nodes->nodes[s.slot];
    if (slot->incarnation!=s.incarnation || slot->kind!=LOOM_SERVICE_SCOPE ||
        slot->phase!=LSLOT_LIVE) return -T_E_NOENT;
    if (node->reason<0) return node->reason;
    if (node->state!=LOOM_SERVICE_READY || !p->connect_bound ||
        p->locally_retired || p->abort_reason) return -T_E_CANCELED;
    return 0;
}
static void operation_refund(struct Loom *l) {
    spin_lock(&l->service_as->lock);
    addrspace_uncharge_pages(l->service_as,1);
    spin_unlock(&l->service_as->lock);
}
static void operation_unreserve(struct Loom *l,struct loom_request_ref r) {
    spin_lock(&l->lock);
    if (loom_request_drop_owner(&l->service_requests,r) ||
        loom_request_rollback(&l->service_requests,l->service_slots,r))
        extinction("operation lost prepared physical owner");
    spin_unlock(&l->lock);
}
static void operation_put(struct Loom *l,struct loom_service_ref s,
                           struct loom_service_protocol *p) {
    if (loom_engine_put(l,s,p)) extinction("operation lost protocol entry pin");
    loom_private_close_changed(l);
    loom_unref(l); // retained creator image and Loom are last
}
int loom_private_operation_prepare(struct Loom *l,struct loom_service_ref scope,
    u64 user_data,u64 deadline,u64 now,struct loom_service_operation **out) {
    bool fault=operation_fault_take();
    if (!l || l->magic!=LOOM_MAGIC || !loom_is_private(l) || !out) return -T_E_INVAL;
    if (deadline && deadline<=now) return -T_E_TIMEDOUT;
    struct ProcServiceIdentity identity;
    if (!proc_service_snapshot_by_stripes(l->service_creator,l->service_as,&identity))
        return -T_E_ACCES;
    struct loom_service_protocol *p;
    int rc=loom_engine_get(l,scope,&p);
    if (rc) return rc;
    /* missing root */
    struct loom_request_ref request;
    spin_lock(&l->lock);
    rc=operation_scope_ready(l,scope,p);
    if (!rc) rc=loom_request_prepare(&l->service_requests,l->service_slots,
              l->service_nodes,scope,user_data,deadline,now,&request);
    if (!rc && loom_request_hold_owner(&l->service_requests,request))
        extinction("operation cannot hold fresh prepared owner");
    spin_unlock(&l->lock);
    if (rc) { operation_put(l,scope,p); return rc; }
    spin_lock(&l->service_as->lock);
    bool charged=addrspace_charge_pages(l->service_as,1,
                       identity.access.principal_id==PRINCIPAL_SYSTEM);
    spin_unlock(&l->service_as->lock);
    if (!charged) { rc=-T_E_NOMEM; goto fail_reservation; }
    // Request a full page, not sizeof(*op): exact buddy storage charge even
    // when operation metadata would otherwise fit a shared slab class.
    struct loom_service_operation *op=fault ? NULL : kmalloc(PAGE_SIZE,KP_ZERO);
    if (!op) { rc=-T_E_NOMEM; goto fail_charge; }
    op->owner=l; op->protocol=p; op->request=request; op->scope=scope;
    // Keep the fresh creator/image snapshot, never CONNECT or worker authority.
    const u8 *src=(const u8 *)&identity.access;
    u8 *dst=(u8 *)&op->program.identity;
    for(size_t i=0;i<sizeof(identity.access);++i)dst[i]=src[i];
    spin_lock(&l->lock);
    rc=operation_scope_ready(l,scope,p);
    spin_unlock(&l->lock);
    if (rc) { kfree(op); goto fail_charge; }
    *out=op;
    return 0;
fail_charge:
    operation_refund(l);
fail_reservation:
    operation_unreserve(l,request);
    operation_put(l,scope,p);
    return rc;
}
int loom_private_operation_release_unused(struct loom_service_operation *op) {
    if (!op || !op->owner || !op->protocol) return -T_E_INVAL;
    struct Loom *l=op->owner;
    struct loom_service_protocol *p=op->protocol;
    struct loom_service_ref scope=op->scope;
    struct loom_request_ref request=op->request;
    spin_lock(&l->lock);
    struct loom_service_request *r=request.index<l->service_requests.capacity ?
                              &l->service_requests.records[request.index] : NULL;
    bool unused=r && !request.reserved && request.identity &&
        r->identity==request.identity && r->phase==LREQ_PREPARED &&
        r->owners==1 && !r->borrows && !op->rpc.on_complete && !op->rpc.sending &&
        op->phase==LRPC_NEW && !op->next && !op->prev && !op->submitted && !op->fid_claim && !op->source_borrow;
    spin_unlock(&l->lock);
    if (!unused) return -T_E_BUSY;
    kfree(op);
    operation_refund(l);
    operation_unreserve(l,request);
    operation_put(l,scope,p);
    return 0;
}

static void reset(int f){C(!live&&!pages&&!pins&&refs==1,"previous resources released");memset(&l,0,sizeof(l));memset(&slots,0,sizeof(slots));memset(&nodes,0,sizeof(nodes));memset(records,0,sizeof(records));fault=f;memset(&fresh,0xa5,sizeof(fresh));fresh.principal_id=42;freed=refunds=entry_gets=0;l.magic=LOOM_MAGIC;l.service_creator=123;l.service_as=&as;l.service_slots=&slots;l.service_nodes=&nodes;l.service_requests=(struct loom_request_table){.records=records,.capacity=2};C(!loom_service_node_prepare(&slots,&nodes,LOOM_SERVICE_SCOPE,(struct loom_service_ref){0},&scope),"scope prepare");C(!loom_service_node_publish(&slots,&nodes,scope),"scope publish");nodes.nodes[scope.slot].state=LOOM_SERVICE_READY;protocol=(struct loom_service_protocol){.connect_bound=true};}
int main(void){
 for(int f=0;f<=6;f++){
 reset(f);struct loom_service_operation*op=(void*)1;int rc=loom_private_operation_prepare(&l,scope,23,100,10,&op);
 if(f){C(rc<0&&op==(void*)1,"failed constructor leaves output");C(!live&&!pages&&!pins&&refs==1&&!l.service_requests.occupied,"constructor unwind");if(f>=5)C(freed==1&&refunds==1,"allocation race unwinds physical storage");continue;}
 C(!rc&&op!=(void*)1&&live==1&&pages==1&&pins==1&&refs==2,"success retained storage");
 C(!memcmp(&op->program.identity,&fresh,sizeof(fresh)),"fresh creator identity retained byte-for-byte");
 memset(&fresh,0x37,sizeof(fresh));C(op->program.identity.principal_id==42,"identity detached from source");
 op->phase=LRPC_QUEUED;C(loom_private_operation_release_unused(op)==-T_E_BUSY,"queued owner release refused");op->phase=LRPC_NEW;
 op->next=op;C(loom_private_operation_release_unused(op)==-T_E_BUSY,"linked owner release refused");op->next=NULL;
 op->submitted=true;C(loom_private_operation_release_unused(op)==-T_E_BUSY,"submitted owner release refused");op->submitted=false;
 op->fid_claim=110;C(loom_private_operation_release_unused(op)==-T_E_BUSY,"claimed FID blocks unused release");op->fid_claim=0;
 op->source_borrow=true;C(loom_private_operation_release_unused(op)==-T_E_BUSY,"source hold blocks unused release");op->source_borrow=false;
 struct loom_request_ref r=op->request;
 C(!loom_request_admit(&l.service_requests,&slots,&nodes,r,10),"test admitted");C(loom_private_operation_release_unused(op)==-T_E_BUSY&&live==1,"active release refused");records[r.index].phase=LREQ_PREPARED;
 op->rpc.on_complete=(void*)1;C(loom_private_operation_release_unused(op)==-T_E_BUSY&&live==1,"callback release refused");op->rpc.on_complete=NULL;
 op->rpc.sending=true;C(loom_private_operation_release_unused(op)==-T_E_BUSY,"sending release refused");op->rpc.sending=false;
 op->request.identity++;C(loom_private_operation_release_unused(op)==-T_E_BUSY,"stale owner release refused");op->request=r;
 l.service_closing=true;slots.closing=true;C(!loom_private_operation_release_unused(op),"close permits unused cleanup");C(freed==1&&refunds==1&&refs==1&&!pins&&!pages&&!live&&!l.service_requests.occupied,"final cleanup");
 }
 reset(0);struct loom_service_operation*op=(void*)1;C(loom_private_operation_prepare(&l,scope,0,10,10,&op)==-T_E_TIMEDOUT,"expired admission refused");C(op==(void*)1&&!pins&&!live&&!entry_gets,"expired output untouched");
 reset(0);nodes.nodes[scope.slot].state=LOOM_SERVICE_RESERVED;C(loom_private_operation_prepare(&l,scope,0,0,10,&op)<0&&!live,"nonready scope refused");
 puts("PASS constructor faults, close/abort allocation race, active/callback/stale refusal and physical free ordering");}
