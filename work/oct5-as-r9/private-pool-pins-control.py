#!/usr/bin/env python3
"""Actual internal pool owner functions and real ledgers with checked host doubles.

Sequential ASan/UBSan control and exact mutants. Models image/lock/Burrow and
post-unlock table replacement; native fixture covers actual backing pins. This
is not protocol admission, concurrent execution or guest allocator proof.
"""
from pathlib import Path
import datetime,difflib,hashlib,importlib.util,json,os,re,subprocess
ROOT=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('extract',Path(__file__).with_name('final-reclaim-control.py'))
extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
PRE=r'''
#include <stdio.h>
#include <stdlib.h>
#include <thylacine/loom_service_pool_table.h>
#include <thylacine/errno.h>
#define LOOM_MAGIC 123
#define PROC_MAGIC 456
#define BURROW_TYPE_ANON 1
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held;
static void (*on_unlock)(void);
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"lock reentry");l->held=1;held++; }
static void spin_unlock(spin_lock_t *l) {
 CHECK(l->held,"lock release");l->held=0;held--;
 if(on_unlock) {void(*f)(void)=on_unlock;on_unlock=NULL;f();}
}
struct AddrSpace { spin_lock_t lock;u32 pages; };
struct Burrow { u32 type;u8 *pages;u64 size;struct AddrSpace *payer;int refs; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct loom_reg_buffer { struct Burrow *burrow;u8 *kva;u32 len,_pad; };
struct Loom {
 u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_service_pool_table *service_pools;
 struct loom_slot_table *service_slots;u32 receipt_size;
 struct Burrow *ring;struct loom_reg_buffer reg_buf[2];u32 n_reg_buf;
 void *inflight_ops,*chain;u32 async_inflight,admitting,chain_len,rearm_pending;
};
static struct AddrSpace a={.pages=10},other={.pages=10};
static u8 storage[4096];
static struct Burrow buffer={.type=BURROW_TYPE_ANON,.pages=storage,.size=sizeof(storage),.payer=&a,.refs=2};
static struct Burrow ring={.type=BURROW_TYPE_ANON,.pages=storage,.size=sizeof(storage),.payer=&a,.refs=2};
static bool loom_is_private(struct Loom *l) { return l && l->service_as; }
static u64 page_to_pa(u8 *p) {return (u64)p;}
static u8 *pa_to_kva(u64 p) {return (u8 *)p;}
static void burrow_ref(struct Burrow *b) {CHECK(held==1 && b->refs>0,"pin while table locked and alive");b->refs++;}
static bool burrow_unref_settled_in(struct Burrow *b,struct AddrSpace *as,u32 *refund) {
 CHECK(!held && b->refs>0,"drop outside locks with owned pin");b->refs--;
 *refund=(!b->refs && b->payer==as)?1:0;return !b->refs;
}
static void addrspace_uncharge_pages(struct AddrSpace *as,u32 n) {
 CHECK(as->lock.held && n<=as->pages,"exact-image refund under lock");as->pages-=n;
}
static void replace_table_after_unlock(void) {
 CHECK(!held && buffer.refs==4,"replacement sees independent pool pins");buffer.refs-=2;
}
'''
MAIN=r'''
int main(void) {
 struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_service_pool_table pools={0};struct loom_slot_table slots={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&a,.service_creator=99,.service_pools=&pools,
   .service_slots=&slots,.receipt_size=64,.ring=&ring,.n_reg_buf=2,
   .reg_buf={{&buffer,storage+128,1024,0},{&buffer,storage+192,512,0}}};
 struct loom_service_pool_create in={.size=sizeof(in),.version=1,.count=2,
   .members={{0,0,64,64},{1,0,64,64}}};
 struct loom_service_ref ref={0},second={0},stale;
 p.as=&other;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"wrong image rejected");p.as=&a;
 p.stripes=100;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_ACCES,"wrong creator rejected");p.stripes=99;
 l.receipt_size=0;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_OPNOTSUPP,"receipt geometry required");l.receipt_size=64;
 in.result.incarnation=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"output field rejected");in.result.incarnation=0;
 in.members[63].length=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"unused tail rejected");in.members[63].length=0;
 in.members[0].reserved=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"member reserved rejected");in.members[0].reserved=0;
 in.members[0].offset=~(u64)0;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"wrapping range rejected");in.members[0].offset=64;
 in.members[0].length=1024;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"member end outside registration rejected");in.members[0].length=64;
 l.reg_buf[0].burrow=&ring;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"ring backing rejected");l.reg_buf[0].burrow=&buffer;
 l.reg_buf[0].kva=storage+4000;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL,"registration outside backing rejected");l.reg_buf[0].kva=storage+128;
 l.admitting=1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_BUSY,"unresolved fixed admission rejected");l.admitting=0;
 l.chain=(void*)1;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_BUSY,"held fixed chain rejected");l.chain=NULL;
 in.members[1].offset=0;
 CHECK(loom_private_pool_prepare(&l,&p,&in,&ref)==-T_E_INVAL && !ref.incarnation && buffer.refs==2,
       "canonical alias overlap rejected");in.members[1].offset=64;
 CHECK(!loom_private_pool_prepare(&l,&p,&in,&ref) && buffer.refs==4,"member pins acquired exactly");
 CHECK(pools.bank.cells[0].extent.offset==192 && pools.bank.cells[1].extent.offset==256,
       "canonical offsets retain registered slice displacement");
 CHECK(loom_private_pool_prepare(&l,&p,&in,&second)==-T_E_BUSY && !second.incarnation && buffer.refs==4,
       "provisional overlap cannot leak pins");
 stale=ref;
 CHECK(!loom_private_pool_rollback(&l,ref) && buffer.refs==2,"rollback drops exactly member pins");
 CHECK(!loom_private_pool_prepare(&l,&p,&in,&ref) && buffer.refs==4 && ref.incarnation!=stale.incarnation,"identity burned");
 CHECK(loom_private_pool_rollback(&l,stale)==-T_E_NOENT && buffer.refs==4,"stale removal retains pins");
 CHECK(!loom_private_pool_publish(&l,ref),"publish succeeds");
 CHECK(loom_private_pool_rollback(&l,ref)==-T_E_NOENT && buffer.refs==4,"live rollback retains pins");
 CHECK(!loom_pool_stream_get(&pools.pools[ref.slot]),"stream get");
 CHECK(loom_private_pool_reap(&l,ref)==-T_E_BUSY && buffer.refs==4,"busy reap retains pins");
 CHECK(!loom_pool_stream_put(&pools.pools[ref.slot]),"stream put");
 CHECK(!loom_private_pool_reap(&l,ref) && buffer.refs==2,"reap drops exactly member pins");
 CHECK(!loom_private_pool_prepare(&l,&p,&in,&ref),"prepare before close");
 l.service_closing=true;loom_slots_close(&slots);
 CHECK(loom_private_pool_publish(&l,ref)==-T_E_CANCELED,"close refuses publication");
 CHECK(loom_private_pool_prepare(&l,&p,&in,&second)==-T_E_CANCELED,"close refuses prepare");
 on_unlock=replace_table_after_unlock;
 CHECK(!loom_private_pool_rollback(&l,ref) && buffer.refs==0 && a.pages==9,
       "after-close rollback settles final pins after concurrent table replacement");
 CHECK(loom_service_pools_empty(&pools) && !held,"empty ledgers and released locks");
 puts("PASS private pool canonical ownership and rollback");return 0;
}
'''
def main():
 os.chdir(ROOT)
 lease=subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True)
 assert re.search(r'^mac +HELD by you',lease,re.M)
 st=os.statvfs('.');assert st.f_bavail*st.f_frsize>=8*1024**3
 out=Path('work/oct5-as-r9/astra-takeover')/('private-pool-pins-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 source=Path('kernel/loom.c').read_text()
 funcs={n:extract.fun(source,n) for n in ['loom_private_drop_buffer','loom_private_pool_prepare','loom_private_pool_publish','loom_private_pool_remove','loom_private_pool_rollback','loom_private_pool_reap']}
 def program(fs):return PRE+'\n'+'\n'.join(fs.values())+'\n'+MAIN
 control=program(funcs)
 cases=[('control',None,None,None,None),
 ('omit-image','loom_private_pool_prepare','p->as != l->service_as || ','','wrong image rejected'),
 ('omit-creator','loom_private_pool_prepare',' || p->stripes != l->service_creator','','wrong creator rejected'),
 ('allow-tail','loom_private_pool_prepare','u32 i = in->count;','u32 i = LOOM_SERVICE_POOL_MEMBERS;','unused tail rejected'),
 ('omit-canonical-offset','loom_private_pool_prepare','kva - base + m->offset,m->length','m->offset,m->length','canonical alias overlap rejected'),
 ('allow-ring','loom_private_pool_prepare','b->burrow == l->ring ||','','ring backing rejected'),
 ('allow-fixed-admission','loom_private_pool_prepare',' || l->admitting','','unresolved fixed admission rejected'),
 ('omit-member-pin','loom_private_pool_prepare','            burrow_ref((struct Burrow *)extents[i].backing);','            if (false) burrow_ref((struct Burrow *)extents[i].backing);','member pins acquired exactly'),
 ('omit-drop','loom_private_pool_remove','            loom_private_drop_buffer(l->service_as,pins[i]);','            if (false) loom_private_drop_buffer(l->service_as,pins[i]);','rollback drops exactly member pins'),
 ('drop-on-refusal','loom_private_pool_remove','    if (!rc)\n','    if (true)\n','live rollback retains pins'),
 ('omit-refund','loom_private_drop_buffer','        addrspace_uncharge_pages(as, refund);','        if (false) addrspace_uncharge_pages(as, refund);','after-close rollback settles final pins after concurrent table replacement')]
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 modules=['kernel/loom_service_pool.c','kernel/loom_service_slots.c','kernel/loom_service_pool_table.c']
 module_hashes={m:hashlib.sha256(Path(m).read_bytes()).hexdigest() for m in modules}
 for tag,fn,old,new,want in cases:
  fs=dict(funcs)
  if fn:
   assert fs[fn].count(old)==1,(tag,old);fs[fn]=fs[fn].replace(old,new)
  src=out/(tag+'.c');src.write_text(program(fs));exe=out/tag
  (out/(tag+'.diff')).write_text(''.join(difflib.unified_diff(control.splitlines(True),src.read_text().splitlines(True),fromfile='control',tofile=tag)))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(ROOT/'kernel/include'),str(src),*modules,'-o',str(exe)]
  (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
  with (out/(tag+'-build.log')).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  r=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=15,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
  text=r.stdout+r.stderr;(out/(tag+'.log')).write_text(text)
  expected=want or 'PASS private pool canonical ownership and rollback';rc=11 if want else 0
  assert r.returncode==rc and text==expected+'\n',(tag,r.returncode,text)
  results.append(dict(case=tag,rc=rc,expected=expected,generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()));print('PASS',tag,expected,flush=True)
 assert source==Path('kernel/loom.c').read_text()
 assert module_hashes=={m:hashlib.sha256(Path(m).read_bytes()).hexdigest() for m in modules}
 (out/'result.json').write_text(json.dumps({'scope':__doc__,'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'loom_sha256':hashlib.sha256(source.encode()).hexdigest(),'modules':module_hashes,'results':results},indent=2)+'\n');print(out)
if __name__=='__main__':main()
