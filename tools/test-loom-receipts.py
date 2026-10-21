#!/usr/bin/env python3
"""Actual Loom constructors and paired publication (loom_provided_deliver, the
live path) with the real pool, slot and request ledgers and the shared guest
fixture. Host doubles: spinlocks, allocations, arm64 dsb and poll wakes. Store
observer checks publication state at the release-tail boundary; not an ARM
memory model.
"""
from pathlib import Path
import argparse,json,os,shlex,shutil,subprocess,tempfile,importlib.util
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('progress_fixture',ROOT/'tools/test-9p-client-progress.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
# Apple clang's ASan runtime hangs at startup on macOS 26 (before main); the
# Homebrew LLVM toolchain does not (test-loom-service-fids.py, the same choice).
LLVM_CC=Path(os.environ.get('LLVM_PREFIX','/opt/homebrew/opt/llvm'))/'bin/clang'
NAMES=['align_up_u32','is_pow2_u32','loom_measure','loom_create_layout','loom_create','loom_create_with_receipts','loom_post_cqe']
PROVIDED=['provided_same','provided_pool','provided_cell','loom_provided_deliver']
LEDGERS=['kernel/loom_service_pool.c','kernel/loom_service_pool_table.c','kernel/loom_service_slots.c','kernel/loom_service_requests.c']
OBSERVED='publication pairs payload receipt and lease before tail'
MUTANTS=[
 ('early-tail','    receipts[idx]=o->receipt;','    __atomic_store_n(&h->cq_tail,tail+1u,__ATOMIC_RELEASE);\n    receipts[idx]=o->receipt;',OBSERVED),
 ('wrong-receipt','    receipts[idx]=o->receipt;','    receipts[idx]=(struct loom_service_buffer_receipt){0};',OBSERVED),
 ('not-leased','    if(loom_pool_deliver(&l->service_pools->bank,p,&o->receipt))extinction("provided delivery changed inside Ll");\n','',OBSERVED),
 ('shared-tail','    u32 tail=l->cq_tail,head=','    u32 tail=h->cq_tail,head=','paired payload published'),
 ('no-backpressure','if((u32)(tail-head)>=l->cq_entries ||','if((u32)(tail-head)>l->cq_entries ||','full CQ retains pending payload'),
 ('no-detached-refusal','    if(l->service_requests.detached){rc=-T_E_CANCELED;goto done;}\n','','detached consumer refuses publication'),
 ('missing-clear','        r[idx] = (struct loom_service_buffer_receipt){0};','        (void)r;','nonleased CQE clears old receipt'),
 ('false-buffer','    if (flags & LOOM_CQE_SERVICE_BUFFER) return -1;','    (void)flags;','untyped publication cannot mint buffer flag'),
 ('short-receipts','cq_entries * (u32)sizeof(struct loom_service_buffer_receipt)','cq_entries * 16u','receipt geometry bounded aligned'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--mutants',action='store_true');a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'kernel/loom.c').read_text();actual='\n\n'.join(helper.function(source,n) for n in NAMES)
 # loom_measure writes through a struct loom_layout, so the type has to come
 # across with it or the extracted translation unit does not compile.
 _s=source.index('struct loom_layout {');actual=source[_s:source.index('};',_s)+2]+'\n'+actual
 actual=actual.replace('__asm__ __volatile__("dsb ish" ::: "memory");','__atomic_thread_fence(__ATOMIC_SEQ_CST);')
 provided=(ROOT/'kernel/loom_service_provided.c').read_text()
 actual+='\n\n'+'\n\n'.join(helper.function(provided,n) for n in PROVIDED)
 template=(ROOT/'tools/host-tests/loom-receipts.c').read_text();assert template.count('/* ACTUAL_LOOM */')==1
 cc=os.environ.get('CC') or (str(LLVM_CC) if LLVM_CC.exists() else 'clang')
 sdk=subprocess.run(['xcrun','--show-sdk-path'],capture_output=True,text=True) if shutil.which('xcrun') else None
 with tempfile.TemporaryDirectory(prefix='loom-receipts-') as tmp:
  for name,old,new,want in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=actual
   if old:assert s.count(old)==1,name;s=s.replace(old,new)
   f=Path(tmp)/(name+'.c');f.write_text(template.replace('/* ACTUAL_LOOM */',s));exe=f.with_suffix('')
   cmd=[cc,'-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-I',str(ROOT/'kernel/include'),'-I',str(ROOT/'kernel/test'),str(f)]+[str(ROOT/x) for x in LEDGERS]+['-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   if sdk and sdk.returncode==0 and '-isysroot' not in cmd:cmd+=['-isysroot',sdk.stdout.strip()]
   (a.logs/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
   with (a.logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode!=0 and want in r.stderr,(name,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.stdout,r.stderr)
   print('PASS '+name+(': '+want if want else ''),flush=True)
if __name__=='__main__':main()
