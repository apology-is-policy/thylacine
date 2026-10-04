#!/usr/bin/env python3
"""Actual pool module + host boundary fixture; requires a Yip lease.
No kernel runtime, caller lock, pin lifetime or wire claims. Mutants stay in temp.
"""
from pathlib import Path
import argparse,os,shlex,subprocess,tempfile,json,hashlib
ROOT=Path(__file__).resolve().parent.parent
MUTANTS=[
 ('ignore-nonce',' || c->nonce != r->lease','', 'wrong nonce cannot return'),
 ('ignore-generation','a.slot == b.slot && a.incarnation == b.incarnation','a.slot == b.slot','wrong incarnation cannot return'),
 ('quota','if (free < count) return -T_E_NOSPC;','(void)free; if (false) return -T_E_NOSPC;','combined member ceiling'),
 ('provisional-alias','b->cells[i].phase != LMEMBER_FREE && overlaps(e, b->cells[i].extent)','b->cells[i].phase == LMEMBER_LEASED && overlaps(e, b->cells[i].extent)','provisional extent excludes fixed I/O'),
 ('omit-overlap','if (overlaps(e[i], e[j])) return -T_E_INVAL;','if (false) return -T_E_INVAL;','canonical aliases rejected'),
 ('recycle-at-delivery','c->phase = LMEMBER_LEASED;','c->phase = LMEMBER_AVAILABLE;','snapshot conserves members'),
 ('reuse-held','if (!c || c->phase != LMEMBER_AVAILABLE) continue;','if (!c || c->phase == LMEMBER_BUSY) continue;','empty pool waits without reusing payload'),
 ('nonce-wrap','if (b->next_nonce == UINT64_MAX) return -T_E_NOSPC;','if (false) return -T_E_NOSPC;','exhaustion cannot wait or wrap'),
 ('oversized-reply','if (!length || length > c->requested) return -T_E_INVAL;','if (!length) return -T_E_INVAL;','reply bounded by request'),
 ('reap-payload','b->cells[i].phase != LMEMBER_AVAILABLE','b->cells[i].phase == LMEMBER_BUSY','retirement cannot recycle held payload'),
 ('drop-correlation','c->user_data = user_data; c->more = more;','c->user_data = 0; c->more = more; (void)user_data;','receipt correlation intact'),
]
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);p.add_argument('--sanitize',action='store_true');p.add_argument('--mutants',action='store_true');a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 source=(ROOT/'kernel/loom_service_pool.c').read_text();results=[]
 with tempfile.TemporaryDirectory(prefix='loom-pool-fixture-') as tmp:
  for name,old,new,want in [('clean',None,None,None)]+(MUTANTS if a.mutants else []):
   s=source
   if old:
    assert old in s,name;s=s.replace(old,new)
   path=Path(tmp)/(name+'.c');path.write_text(s);exe=path.with_suffix('')
   cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-Wall','-Wextra','-Werror','-I',str(ROOT/'kernel/include'),str(path),str(ROOT/'tools/host-tests/loom-service-pool.c'),'-o',str(exe)]+shlex.split(os.environ.get('CFLAGS',''))
   if a.sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-g']
   with (a.logs/(name+'-build.log')).open('wb') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode==1 and 'FAIL '+want in r.stderr,(name,r.returncode,r.stdout,r.stderr)
   else:assert r.returncode==0,(r.returncode,r.stdout,r.stderr)
   print('PASS',name,want or r.stdout.strip(),flush=True);results.append(name)
 (a.logs/'result.json').write_text(json.dumps(dict(cases=results,sanitized=a.sanitize,source_sha256=hashlib.sha256(source.encode()).hexdigest()),indent=2)+'\n')
if __name__=='__main__':main()
