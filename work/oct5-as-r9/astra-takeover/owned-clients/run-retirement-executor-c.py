from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh';CC='/opt/homebrew/opt/llvm@22/bin/clang';SDK=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('retirement-executor-c-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['retirement_executor.c','retirement_executor.h','fixed_requests.c','fixed_requests.h','test_retirement_executor.c','test_ring_payload.c','ring_payload.c','ring_payload.h','request_book.c','request_book.h','paired_cq.h','lease_ledger.c','lease_ledger.h','run-retirement-executor-c.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={str(D/p):sha(D/p) for p in files};p=R/'usr/lib/libt/include/thyla/loom_service.h';manifest[str(p)]=sha(p)
(run/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');(run/'source.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']));(run/'source.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary']))
for n in files:shutil.copy2(D/n,run/n)
records=[]
def limits():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def cmd(args,name,fail=False):
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize//1024**2>8256
 with (run/(name+'.log')).open('wb') as f:r=subprocess.run([str(a) for a in args],stdout=f,stderr=subprocess.STDOUT,preexec_fn=limits,timeout=30)
 records.append(dict(name=name,rc=r.returncode,expected_failure=fail));assert (r.returncode!=0) if fail else (r.returncode==0),(name,r.returncode)
 if fail:assert 'Assertion failed' in (run/(name+'.log')).read_text()
 if name in ['sanitized-run','ndebug-run']:assert 'C eight retirement owner scenarios PASS' in (run/(name+'.log')).read_text()
common=[CC,'-isysroot',SDK,'-std=c11','-Dcalloc=t_test_calloc','-Wall','-Wextra','-Werror','-idirafter',R/'usr/lib/libt/include']
try:
 cmd([CC,'--version'],'clang-version')
 for mode in ['sanitized','ndebug']:
  extra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2','-DNDEBUG']
  objects=[]
  for src in ['retirement_executor.c','fixed_requests.c','request_book.c','ring_payload.c','lease_ledger.c']:
   obj=run/(mode+'-'+src+'.o');objects.append(obj);cmd(common+extra+['-c',run/src,'-o',obj],mode+'-'+src+'-build')
  # Test assertions stay enabled even when production objects use NDEBUG.
  testextra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2']
  binary=run/(mode+'-tests');cmd(common+testextra+[run/'test_retirement_executor.c',*objects,'-o',binary],mode+'-link');cmd([binary],mode+'-run')
 variants={
 'early-reclaim':('c->retiring&&t_fixed_progress(c->driver)','c->retiring'),
 'missing-retire':('cell->retiring=1;t_fixed_begin_retire(cell->driver);','cell->retiring=1;'),
 'frozen-cursor':('e->cursor=(i+1)%e->capacity;','e->cursor=i;'),
 'excess-budget':('visited=budget<e->capacity?budget:e->capacity','visited=(budget<e->capacity?budget:e->capacity)+1'),
 'no-shutdown-close':('t_fixed_close(e->cells[i].driver);','t_fixed_begin_retire(e->cells[i].driver);'),
 'premature-admission':('if(slot==e->capacity)return T_RETIREMENT_FULL;','if(slot==e->capacity)slot=0;'),
 }

 for name,(a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/'retirement_executor.c';s=src.read_text();assert s.count(a)>=1;src.write_text(s.replace(a,b));binary=m/'test';cmd(common+[m/'retirement_executor.c',m/'fixed_requests.c',m/'request_book.c',m/'ring_payload.c',m/'lease_ledger.c',m/'test_retirement_executor.c','-o',binary],name+'-build');cmd([binary],name+'-run',True)
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=8,profiles=['asan+ubsan','optimized-ndebug-core'],mutants_rejected=6,records=records,limits='C bounded retirement owner; simulated backend. No native SQ or event/wakeup qualification; SQ fix parked.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
