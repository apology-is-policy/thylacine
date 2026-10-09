#!/usr/bin/env python3
"""Extract actual private table admission and settled pin-drop functions.

Sequential ASan/UBSan witness with checked image, lock, VMA-resolution and Burrow
settlement doubles. Native fixture covers real pins/charges separately. Neither
proves concurrent protocol retirement or actual guest final allocator reclamation.
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
#define LOOM_MAX_REG_BUFFERS 2
#define CHECK(c,s) do { if(!(c)) { puts(s);exit(11); } } while(0)
typedef struct { int held; } spin_lock_t;
static int held;
static void spin_lock(spin_lock_t *l) { CHECK(!l->held,"lock reentry");l->held=1;held++; }
static void spin_unlock(spin_lock_t *l) { CHECK(l->held,"lock release");l->held=0;held--; }
struct AddrSpace { spin_lock_t lock;u32 pages; };
struct Burrow { struct AddrSpace *payer;int refs;bool mapped; };
struct Proc { u32 magic;struct AddrSpace *as;u64 stripes; };
struct loom_buf_reg { u64 va,len; };
struct loom_reg_buffer { struct Burrow *burrow;u8 *kva;u32 len,_pad; };
struct Loom {
 u32 magic;spin_lock_t lock;struct AddrSpace *service_as;u64 service_creator;
 bool service_closing;struct loom_service_pool_table *service_pools;
 struct Burrow *ring;struct loom_reg_buffer reg_buf[2];u32 n_reg_buf;
};
static struct AddrSpace a={.pages=10},other={.pages=10};
static struct Burrow buffer={.payer=&a,.mapped=true},ring={.payer=&a,.mapped=true};
static bool loom_is_private(struct Loom *l) { return l && l->service_as; }
static bool burrow_unref_settled_in(struct Burrow *b,struct AddrSpace *as,u32 *refund) {
 CHECK(!held && b->refs>0,"drop outside locks with owned pin");b->refs--;
 *refund=(!b->refs && !b->mapped && b->payer==as)?1:0;return !b->refs && !b->mapped;
}
static void addrspace_uncharge_pages(struct AddrSpace *as,u32 n) {
 CHECK(as->lock.held && n<=as->pages,"exact-image refund under lock");as->pages-=n;
}
static int loom_resolve_buf(struct Proc *p,const struct loom_buf_reg *input,struct loom_reg_buffer *out) {
 CHECK(p->as->lock.held,"resolution holds creator image lock");
 if(input->len!=64 || (input->va!=1 && input->va!=2))return -1;
 struct Burrow *b=input->va==1?&buffer:&ring;
 b->refs++;*out=(struct loom_reg_buffer){.burrow=b,.len=64};return 0;
}
'''
MAIN=r'''
int main(void) {
 struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_service_pool_table pools={0};
 struct Loom l={.magic=LOOM_MAGIC,.service_as=&a,.service_creator=99,.service_pools=&pools,.ring=&ring};
 struct loom_buf_reg good={1,64},bad[2]={{1,64},{3,64}},self={2,64};
 p.as=&other;
 CHECK(loom_private_register_buffers(&l,&p,NULL,0)==-T_E_ACCES && !buffer.refs,"wrong image refused before pin");
 p.as=&a;p.stripes=100;
 CHECK(loom_private_register_buffers(&l,&p,&good,1)==-T_E_ACCES && !buffer.refs,"wrong creator refused before pin");p.stripes=99;
 CHECK(!loom_private_register_buffers(&l,&p,&good,1) && buffer.refs==1 && l.n_reg_buf==1,"table adopts exactly one pin");
 CHECK(loom_private_register_buffers(&l,&p,bad,2)==-T_E_INVAL && buffer.refs==1 && l.reg_buf[0].burrow==&buffer,"partial refusal drops speculative pin");
 CHECK(loom_private_register_buffers(&l,&p,&self,1)==-T_E_INVAL && !ring.refs && buffer.refs==1,"own ring refused without leak");
 pools.pools[0].phase=LPOOL_RESERVED;
 CHECK(loom_private_register_buffers(&l,&p,&good,1)==-T_E_BUSY && buffer.refs==1,"provisional pool blocks replacement");
 pools.pools[0].phase=LPOOL_LIVE;
 CHECK(loom_private_register_buffers(&l,&p,NULL,0)==-T_E_BUSY && buffer.refs==1,"live pool blocks clear");
 pools.pools[0].phase=LPOOL_EMPTY;
 CHECK(!loom_private_register_buffers(&l,&p,&good,1) && buffer.refs==1,"replacement releases displaced pin");
 buffer.mapped=false;
 CHECK(!loom_private_register_buffers(&l,&p,NULL,0) && !buffer.refs && a.pages==9,"last pin settles creator charge");
 buffer.payer=&other;buffer.mapped=true;
 CHECK(!loom_private_register_buffers(&l,&p,&good,1),"foreign backing allowed");buffer.mapped=false;
 CHECK(!loom_private_register_buffers(&l,&p,NULL,0) && !buffer.refs && a.pages==9 && other.pages==10,"foreign backing never refunds caller");
 l.service_closing=true;
 CHECK(loom_private_register_buffers(&l,&p,&good,1)==-T_E_CANCELED && !buffer.refs,"close rejects admission and drops pin");
 CHECK(!held,"no lock retained");puts("PASS private buffer admission and pin settlement");return 0;
}
'''
def main():
 os.chdir(ROOT)
 lease=subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True)
 assert re.search(r'^mac +HELD by you',lease,re.M)
 st=os.statvfs('.');assert st.f_bavail*st.f_frsize>=8*1024**3
 out=Path('work/oct5-as-r9/astra-takeover')/('private-buffers-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 source=Path('kernel/loom.c').read_text();pool=Path('kernel/loom_service_pool_table.c').read_text()
 funcs={n:extract.fun(source,n) for n in ['loom_private_drop_buffer','loom_private_register_buffers']}
 funcs={'loom_service_pools_empty':extract.fun(pool,'loom_service_pools_empty'),**funcs}
 def program(fs):return PRE+'\n'+'\n'.join(fs.values())+'\n'+MAIN
 control=program(funcs)
 cases=[('control',None,None,None,None),
 ('omit-image','loom_private_register_buffers','p->as != as || ','','wrong image refused before pin'),
 ('omit-creator','loom_private_register_buffers',' || p->stripes != l->service_creator','','wrong creator refused before pin'),
 ('allow-ring','loom_private_register_buffers','fresh[done++].burrow == l->ring','(done++, false)','own ring refused without leak'),
 ('allow-pool','loom_private_register_buffers','!loom_service_pools_empty(l->service_pools)','!loom_service_pools_empty(l->service_pools) && false','provisional pool blocks replacement'),
 ('allow-close','loom_private_register_buffers','l->service_closing) rc','l->service_closing && false) rc','close rejects admission and drops pin'),
 ('omit-drop','loom_private_register_buffers','        loom_private_drop_buffer(as, fresh[i].burrow);','        if (false) loom_private_drop_buffer(as, fresh[i].burrow);','partial refusal drops speculative pin'),
 ('omit-refund','loom_private_drop_buffer','        addrspace_uncharge_pages(as, refund);','        if (false) addrspace_uncharge_pages(as, refund);','last pin settles creator charge')]
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip();results=[]
 for tag,fn,old,new,want in cases:
  fs=dict(funcs)
  if fn:
   assert fs[fn].count(old)==1,(tag,old);fs[fn]=fs[fn].replace(old,new)
  src=out/(tag+'.c');src.write_text(program(fs));exe=out/tag
  (out/(tag+'.diff')).write_text(''.join(difflib.unified_diff(control.splitlines(True),src.read_text().splitlines(True),fromfile='control',tofile=tag)))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I',str(ROOT/'kernel/include'),str(src),'-o',str(exe)]
  (out/(tag+'-command.json')).write_text(json.dumps(cmd)+'\n')
  with (out/(tag+'-build.log')).open('wb') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True)
  r=subprocess.run([str(exe.resolve())],capture_output=True,text=True,timeout=15,env={**os.environ,'UBSAN_OPTIONS':'halt_on_error=1'})
  text=r.stdout+r.stderr;(out/(tag+'.log')).write_text(text)
  expected=want or 'PASS private buffer admission and pin settlement';rc=11 if want else 0
  assert r.returncode==rc and text==expected+'\n',(tag,r.returncode,text)
  results.append(dict(case=tag,rc=rc,expected=expected,generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()));print('PASS',tag,expected,flush=True)
 assert source==Path('kernel/loom.c').read_text() and pool==Path('kernel/loom_service_pool_table.c').read_text()
 (out/'result.json').write_text(json.dumps({'scope':__doc__,'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'loom_sha256':hashlib.sha256(source.encode()).hexdigest(),'pool_sha256':hashlib.sha256(pool.encode()).hexdigest(),'results':results},indent=2)+'\n');print(out)
if __name__=='__main__':main()
