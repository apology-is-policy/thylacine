from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh';CC='/opt/homebrew/opt/llvm@22/bin/clang';SDK=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('composed-ring-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['test_composed_ring.rs','test_composed_ring.c','test_ring_payload.rs','test_ring_payload.c','ring_payload.rs','ring_payload.c','ring_payload.h','lease_ledger.rs','lease_ledger.c','lease_ledger.h','request_book.rs','request_book.c','request_book.h','paired_cq.rs','paired_cq.c','paired_cq.h','run-composed-ring.py']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();manifest={str(D/p):sha(D/p) for p in files}
for p in ['usr/lib/libthyla-rs/src/loom/service_abi.rs','usr/lib/libt/include/thyla/loom_service.h']:manifest[str(R/p)]=sha(R/p)
(run/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');(run/'source.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']));(run/'source.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary']))
for n in files:shutil.copy2(D/n,run/n)
records=[]
def limits():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def cmd(args,name,fail=False,diag=None):
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize//1024**2>8256
 with (run/(name+'.log')).open('wb') as f:p=subprocess.run([str(a) for a in args],stdout=f,stderr=subprocess.STDOUT,preexec_fn=limits,timeout=30)
 records.append(dict(name=name,rc=p.returncode,expected_failure=fail,diagnostic=diag));assert (p.returncode!=0) if fail else (p.returncode==0),(name,p.returncode)
 if diag:assert diag in (run/(name+'.log')).read_text(),name
common=[CC,'-isysroot',SDK,'-std=c11','-Dcalloc=t_test_calloc','-Wall','-Wextra','-Werror','-idirafter',R/'usr/lib/libt/include'];cs=['ring_payload.c','lease_ledger.c','request_book.c','paired_cq.c']
try:
 cmd(['rustc','--version'],'rust-version');cmd([CC,'--version'],'clang-version')
 for opt in ['0','2']:
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_composed_ring.rs','-o',binary],'rust-build-'+opt);cmd([binary,'composed_','--nocapture'],'rust-run-'+opt,diag='2 passed; 0 failed')
 for mode in ['sanitized','ndebug']:
  extra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2','-DNDEBUG'];objects=[]
  for src in cs:
   obj=run/(mode+'-'+src+'.o');objects.append(obj);cmd(common+extra+['-c',run/src,'-o',obj],mode+'-'+src+'-build')
  testflags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2'];binary=run/(mode+'-tests');cmd(common+testflags+[run/'test_composed_ring.c',*objects,'-o',binary],mode+'-link');cmd([binary],mode+'-run',diag='C two composed CQ request payload scenarios PASS')
 variants=[
 ('rust','wrong-receipt','paired_cq.rs','ptr::read(self.receipts.add(slot))','ptr::read(self.receipts.add((slot+1)&(self.entries as usize-1)))'),
 ('rust','wrong-pool','request_book.rs','record.receipt.pool != pool','false && record.receipt.pool != pool'),
 ('rust','untracked-lease','ring_payload.rs','p.live+=1;s.live+=1;','p.live+=1;'),
 ('c','wrong-receipt','paired_cq.c','r->receipts[slot]','r->receipts[(slot+1)&(r->entries-1)]'),
 ('c','wrong-pool','request_book.c','!same(r->receipt.pool,e->spec.pool)','(0 && !same(r->receipt.pool,e->spec.pool))'),
 ('c','untracked-lease','ring_payload.c','++p->live;++c->live;','++p->live;'),
 ]
 for lang,name,source,a,b in variants:
  m=run/('mutant-'+lang+'-'+name);m.mkdir()
  for n in files[:-1]:shutil.copy2(D/n,m/n)
  src=m/source;s=src.read_text();assert s.count(a)==1;src.write_text(s.replace(a,b));binary=m/'test';label=lang+'-'+name
  if lang=='rust':cmd(['rustc','--edition=2024','--test',m/'test_composed_ring.rs','-o',binary],label+'-build');cmd([binary,'composed_'],label+'-run',True,'FAILED')
  else:cmd(common+[*[m/n for n in cs],m/'test_composed_ring.c','-o',binary],label+'-build');cmd([binary],label+'-run',True,'Assertion failed')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios_per_language=2,rust_profiles=2,c_profiles=['asan+ubsan','ndebug-core'],mutants=6,records=records,limits='Composed actual client cores with simulated backend/mapping producer; no real syscall/ABORT/fixed-I/O/future/executor integration'),indent=2)+'\n');print(run,flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
