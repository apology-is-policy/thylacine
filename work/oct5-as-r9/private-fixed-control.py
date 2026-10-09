#!/usr/bin/env python3
"""Extract fixed reservation owners and pool owners with actual joint ledgers.
Sequential checked lock/Burrow/Loom doubles under ASan/UBSan; no protocol or
concurrency proof. Native fixture separately checks actual pins and retirement.
"""
from pathlib import Path
import datetime,difflib,hashlib,importlib.util,json,os,re,subprocess
ROOT=Path(__file__).resolve().parents[2]
def module(name,file):
 spec=importlib.util.spec_from_file_location(name,Path(__file__).with_name(file));m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m);return m
base=module('poolpins','private-pool-pins-control.py');extract=base.extract
PRE=base.PRE[:base.PRE.index('static void replace_table_after_unlock')]
PRE=PRE.replace(' u32 magic;spin_lock_t lock;',' u32 magic;int refs;u32 cq_entries;spin_lock_t lock;')
PRE+=r'''
static int retired;
static void loom_ref(struct Loom *l) {CHECK(held==1 && l->refs>0,"owner pin under admission lock");l->refs++;}
static void loom_unref(struct Loom *l) {
 CHECK(!held && l->refs>0,"owner drop outside lock");
 if (!--l->refs) {CHECK(!l->service_fixed && !l->service_fixed_count && !buffer.refs && a.pages==9,"backing settled before final owner drop");retired++;}
}
'''
MAIN=r'''
int main(void) {
 struct Proc p={.magic=PROC_MAGIC,.as=&a,.stripes=99};
 struct loom_service_pool_table pools={0};struct loom_slot_table slots={0};
 struct Loom l={.magic=LOOM_MAGIC,.refs=1,.cq_entries=2,.service_as=&a,.service_creator=99,.service_pools=&pools,
   .service_slots=&slots,.receipt_size=64,.ring=&ring,.n_reg_buf=2,
   .reg_buf={{&buffer,storage+128,1024,0},{&buffer,storage+192,512,0}}};
 struct loom_private_fixed f={0},g={0},h={0};
 struct loom_service_pool_member m={0,0,64,64};
 p.as=&other;
 CHECK(loom_private_fixed_acquire(&l,&p,&m,&f)==-T_E_ACCES,"fixed image authority");p.as=&a;
 p.stripes++;
 CHECK(loom_private_fixed_acquire(&l,&p,&m,&f)==-T_E_ACCES,"fixed creator authority");p.stripes--;
 CHECK(!loom_private_fixed_acquire(&l,&p,&m,&f) && f.extent.offset==192 && buffer.refs==3 && l.refs==2 && l.service_fixed_count==1,
       "fixed owns canonical pin and ring");
 CHECK(loom_private_fixed_acquire(&l,&p,&m,&f)==-T_E_BUSY && buffer.refs==3 && l.refs==2,"duplicate fixed token refused");
 struct loom_service_pool_create in={.size=sizeof(in),.version=1,.count=1,.members={{1,0,0,64}}};
 struct loom_service_ref pool={0};
 CHECK(loom_private_pool_prepare(&l,&p,&in,&pool)==-T_E_BUSY && !pool.incarnation && buffer.refs==3,"fixed excludes pool alias");
 in.members[0].offset=64;
 CHECK(!loom_private_pool_prepare(&l,&p,&in,&pool),"touching pool accepted");
 m.offset=128;
 CHECK(loom_private_fixed_acquire(&l,&p,&m,&g)==-T_E_BUSY && !g.owner && buffer.refs==4,"provisional pool excludes fixed alias");
 m.offset=192;
 CHECK(!loom_private_fixed_acquire(&l,&p,&m,&g) && l.service_fixed_count==2 && buffer.refs==5 && l.refs==3,"disjoint fixed accepted");
 CHECK(loom_private_fixed_acquire(&l,&p,&m,&h)==-T_E_AGAIN && !h.owner,"fixed capacity bounded");
 CHECK(!loom_private_pool_rollback(&l,pool) && buffer.refs==4,"pool rollback retains fixed pins");
 struct Loom wrong={.magic=LOOM_MAGIC,.service_as=&other};
 CHECK(loom_private_fixed_release(&wrong,&f)==-T_E_NOENT && buffer.refs==4 && l.refs==3,"wrong ring release retains pins");
 // A real rebind may remove table pins while accepted fixed users survive.
 buffer.refs-=2;l.n_reg_buf=0;
 CHECK(!loom_private_fixed_release(&l,&f) && !f.owner && !f.next && !f.extent.backing && !f.extent.length &&
       buffer.refs==1 && l.refs==2 && l.service_fixed_count==1,"fixed release drops exactly one ownership pair");
 CHECK(loom_private_fixed_release(&l,&f)==-T_E_NOENT && buffer.refs==1 && l.refs==2,"double release unchanged");
 l.service_closing=true;loom_slots_close(&slots);
 CHECK(loom_private_fixed_acquire(&l,&p,&m,&h)==-T_E_CANCELED && !h.owner,"fixed close refusal");
 loom_unref(&l);
 CHECK(!retired && l.refs==1 && g.owner==&l,"fixed retains closed owner after handle leaves");
 CHECK(!loom_private_fixed_release(&l,&g) && !buffer.refs && !l.refs && a.pages==9 && retired==1,"final fixed release settles and retires");
 CHECK(!held,"no retained lock");puts("PASS fixed reservations exclude pools and retain local ownership");return 0;
}
'''
def main():
 os.chdir(ROOT)
 assert re.search(r'^mac +HELD by you',subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True),re.M)
 st=os.statvfs('.');assert st.f_bavail*st.f_frsize>=8*1024**3
 out=Path('work/oct5-as-r9/astra-takeover')/('private-fixed-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 source=Path('kernel/loom.c').read_text()
 funcs={n:extract.fun(source,n) for n in ['loom_private_drop_buffer','loom_private_extent_locked','loom_private_fixed_conflicts_locked','loom_private_fixed_acquire','loom_private_fixed_release','loom_private_pool_prepare','loom_private_pool_remove','loom_private_pool_rollback']}
 def program(fs):return PRE+'\n'+'\n'.join(fs.values())+'\n'+MAIN
 control=program(funcs)
 cases=[('control',None,None,None,None),
 ('omit-image','loom_private_fixed_acquire','p->as != l->service_as || ','','fixed image authority'),
 ('omit-creator','loom_private_fixed_acquire',' || p->stripes != l->service_creator','','fixed creator authority'),
 ('omit-backing-pin','loom_private_fixed_acquire','            burrow_ref((struct Burrow *)e.backing);','            if(false) burrow_ref((struct Burrow *)e.backing);','fixed owns canonical pin and ring'),
 ('omit-ring-pin','loom_private_fixed_acquire','            loom_ref(l);','            if(false) loom_ref(l);','fixed owns canonical pin and ring'),
 ('pool-over-fixed','loom_private_pool_prepare','loom_private_fixed_conflicts_locked(l,extents[i])','loom_private_fixed_conflicts_locked(l,extents[i]) && false','fixed excludes pool alias'),
 ('fixed-over-pool','loom_private_fixed_acquire','loom_pool_conflicts(&l->service_pools->bank,e)','loom_pool_conflicts(&l->service_pools->bank,e) && false','provisional pool excludes fixed alias'),
 ('omit-bound','loom_private_fixed_acquire','l->service_fixed_count >= l->cq_entries','l->service_fixed_count >= l->cq_entries && false','fixed capacity bounded'),
 ('omit-drop','loom_private_fixed_release','    loom_private_drop_buffer(as,b);','    if(false) loom_private_drop_buffer(as,b);','fixed release drops exactly one ownership pair'),
 ('omit-owner-drop','loom_private_fixed_release','    loom_unref(l);','    if(false) loom_unref(l);','fixed release drops exactly one ownership pair'),
 ('allow-close','loom_private_fixed_acquire','l->service_closing) rc','l->service_closing && false) rc','fixed close refusal')]
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
  expected=want or 'PASS fixed reservations exclude pools and retain local ownership';rc=11 if want else 0
  assert r.returncode==rc and text==expected+'\n',(tag,r.returncode,text)
  results.append(dict(case=tag,rc=rc,expected=expected,generated_sha256=hashlib.sha256(src.read_bytes()).hexdigest()));print('PASS',tag,expected,flush=True)
 assert source==Path('kernel/loom.c').read_text()
 assert module_hashes=={m:hashlib.sha256(Path(m).read_bytes()).hexdigest() for m in modules}
 (out/'result.json').write_text(json.dumps({'scope':__doc__,'source_head':subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip(),'loom_sha256':hashlib.sha256(source.encode()).hexdigest(),'modules':module_hashes,'results':results},indent=2)+'\n');print(out)
if __name__=='__main__':main()
