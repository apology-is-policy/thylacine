from pathlib import Path
import re,json,subprocess,hashlib,datetime
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
O=D/('operation-controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));O.mkdir()
src=(D/'kernel/loom_service_operation.c').read_text()
resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
import os
st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
# Actual constructor/ledger code with explicit allocator/image/engine boundaries.
pre=r'''
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
'''
body=re.sub(r'^#include[^\n]*\n','',src,flags=re.M)
post=r'''
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
'''
mutants={
'fid-claim':('!op->fid_claim','true','claimed FID blocks unused release'),
'fid-borrow':('!op->source_borrow','true','source hold blocks unused release'),
'identity':('dst[i]=src[i];','dst[i]=src[i]&0;','fresh creator identity retained byte-for-byte'),
'phase':('op->phase==LRPC_NEW','true','queued owner release refused'),
'linked':('!op->next && !op->prev','true','linked owner release refused'),
'submitted':('!op->submitted','true','submitted owner release refused'),
'early-owner-drop':('    kfree(op);\n    operation_refund(l);\n    operation_unreserve(l,request);','    operation_unreserve(l,request);\n    kfree(op);\n    operation_refund(l);','physical free retains owner and roots'),
'early-refund':('    kfree(op);\n    operation_refund(l);','    operation_refund(l);\n    kfree(op);','refund after physical free before owner drop'),
'slab-allocation':('kmalloc(PAGE_SIZE,KP_ZERO)','kmalloc(sizeof(*op),KP_ZERO)','one charged buddy page'),
'omit-ref':('    loom_ref(l);','    /* missing root */','reserve before charge'),
'omit-owner':('if (!rc && loom_request_hold_owner(&l->service_requests,request))','if (false)','reserve before charge'),
'expiry':('if (deadline && deadline<=now)','if (false)','expired output untouched'),
'no-postcheck':('    rc=operation_scope_ready(l,scope,p);\n    spin_unlock(&l->lock);','    rc=0;\n    spin_unlock(&l->lock);','failed constructor leaves output'),
'active-release':('r->phase==LREQ_PREPARED','true','physical free refuses active request'),
'callback-release':('!op->rpc.on_complete','true','physical free refuses callback'),
'stale-release':('r->identity==request.identity','true','physical free exact request'),
}
sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
for name,edit in [('clean',None),*mutants.items()]:
 s=body
 if edit:assert edit[0] in s,name;s=s.replace(edit[0],edit[1],1)
 c=O/(name+'.c');c.write_text(pre+s+post);exe=O/name
 cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-isysroot',sdk,'-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-g','-I'+str(D/'kernel/include'),'-I'+str(R/'kernel/include'),str(c),str(R/'kernel/loom_service_requests.c'),str(R/'kernel/loom_service_slots.c'),str(R/'kernel/loom_service_nodes.c'),'-o',str(exe)]
 with (O/(name+'-compile.log')).open('wb') as f:r=subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT)
 assert r.returncode==0,(name,'compile')
 with (O/(name+'.log')).open('wb') as f:r=subprocess.run([str(exe)],stdout=f,stderr=subprocess.STDOUT)
 assert (r.returncode==0)==(name=='clean'),(name,r.returncode)
 if edit:assert edit[2] in (O/(name+'.log')).read_text(),(name,'wrong witness')
 (O/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
 results.append(dict(case=name,rc=r.returncode,source_sha256=hashlib.sha256(c.read_bytes()).hexdigest(),binary_sha256=hashlib.sha256(exe.read_bytes()).hexdigest()));print('PASS',name,flush=True)
(O/'result.json').write_text(json.dumps(dict(source_sha256=hashlib.sha256(src.encode()).hexdigest(),inputs={str(p.relative_to(R)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [D/'check-operation.py',D/'kernel/loom_service_operation.c',R/'kernel/loom_service_requests.c',R/'kernel/loom_service_slots.c',R/'kernel/loom_service_nodes.c']},cases=results,limits='Actual constructor+ledger code with explicit host allocator/image/engine/layout doubles; no native physical accounting, scheduler or RPC integration proof.'),indent=2)+'\n');print(O)
