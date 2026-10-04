#!/usr/bin/env python3
"""Actual Loom constructors/publication with real pool core and shared guest fixture.
Host doubles: spinlocks, allocations, arm64 dsb and poll wakes. Store observer
checks publication state at the release-tail boundary; not an ARM memory model.
"""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile,importlib.util
ROOT=Path(__file__).resolve().parent.parent
spec=importlib.util.spec_from_file_location('progress_fixture',ROOT/'tools/test-9p-client-progress.py');helper=importlib.util.module_from_spec(spec);spec.loader.exec_module(helper)
NAMES=['align_up_u32','is_pow2_u32','loom_create_layout','loom_create','loom_create_with_receipts','loom_post_cqe','loom_post_pool_cqe']
MUTANTS=[
 ('early-tail','    receipts[idx] = result.receipt;','    __atomic_store_n(&h->cq_tail, tail + 1u, __ATOMIC_RELEASE);\n    receipts[idx] = result.receipt;','publication pairs payload receipt and lease before tail'),
 ('wrong-receipt','    receipts[idx] = result.receipt;','    receipts[idx] = (struct loom_service_buffer_receipt){0};','publication pairs payload receipt and lease before tail'),
 ('not-leased','    rc = loom_pool_deliver(bank, pool, &result.receipt);','    rc = 0;','publication pairs payload receipt and lease before tail'),
 ('shared-tail','    u32 tail = l->cq_tail;','    u32 tail = h->cq_tail;','paired payload published'),
 ('missing-clear','        r[idx] = (struct loom_service_buffer_receipt){0};','        (void)r;','nonleased CQE clears old receipt'),
 ('false-buffer','    if (flags & LOOM_CQE_SERVICE_BUFFER) return -1;','    (void)flags;','untyped publication cannot mint buffer flag'),
 ('no-backpressure','if ((u32)(tail - head) >= l->cq_entries)', 'if ((u32)(tail - head) > l->cq_entries)','full CQ retains pending payload'),
 ('short-receipts','cq_entries * (u32)sizeof(struct loom_service_buffer_receipt)','cq_entries * 16u','receipt geometry bounded aligned'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--mutants',action='store_true');a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'kernel/loom.c').read_text();actual='\n\n'.join(helper.function(source,n) for n in NAMES)
 actual=actual.replace('__asm__ __volatile__("dsb ish" ::: "memory");','__atomic_thread_fence(__ATOMIC_SEQ_CST);')
 template=(ROOT/'tools/host-tests/loom-receipts.c').read_text()
 with tempfile.TemporaryDirectory(prefix='loom-receipts-') as tmp:
  for name,old,new,want in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=actual
   if old:assert old in s,name;s=s.replace(old,new)
   f=Path(tmp)/(name+'.c');f.write_text(template.replace('/* ACTUAL_LOOM */',s));exe=f.with_suffix('')
   cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-I',str(ROOT/'kernel/include'),'-I',str(ROOT/'kernel/test'),str(f),str(ROOT/'kernel/loom_service_pool.c'),'-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   with (a.logs/(name+'-build.log')).open('wb') as out:subprocess.run(cmd,stdout=out,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode!=0 and want in r.stderr,(name,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.stdout,r.stderr)
   print('PASS '+name+(': '+want if want else ''),flush=True)
if __name__=='__main__':main()
