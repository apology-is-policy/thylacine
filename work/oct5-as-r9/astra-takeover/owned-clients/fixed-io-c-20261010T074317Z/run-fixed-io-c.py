from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh';CC='/opt/homebrew/opt/llvm@22/bin/clang';SDK=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('fixed-io-c-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['ring_payload.c','ring_payload.h','test_ring_payload.c','lease_ledger.c','lease_ledger.h','run-fixed-io-c.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
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
 if name in ['sanitized-run','ndebug-run']:assert 'C seven fixed ownership scenarios PASS' in (run/(name+'.log')).read_text()
common=[CC,'-isysroot',SDK,'-std=c11','-Dcalloc=t_test_calloc','-Wall','-Wextra','-Werror','-idirafter',R/'usr/lib/libt/include']
try:
 cmd([CC,'--version'],'clang-version')
 for mode in ['sanitized','ndebug']:
  extra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2','-DNDEBUG']
  objects=[]
  for src in ['ring_payload.c','lease_ledger.c']:
   obj=run/(mode+'-'+src+'.o');objects.append(obj);cmd(common+extra+['-c',run/src,'-o',obj],mode+'-'+src+'-build')
  # Test assertions stay enabled even when production objects use NDEBUG.
  testextra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2']
  binary=run/(mode+'-tests');cmd(common+testextra+[run/'test_ring_payload.c',*objects,'-o',binary],mode+'-link');cmd([binary],mode+'-run')
 variants={
 'early-fixed-view':('f->write||f->phase!=FIX_TERMINAL','f->write'),
 'abandon-refund':('f->abandoned=1;','f->abandoned=1;f->phase=FIX_FREE;'),
 'missing-abort':('c->b.abort_scope(c->b.context,f->scope);','(void)f->scope;'),
 'fixed-pool-overlap':('if(fixed_overlap(c->fixed[i].range,ranges[j]))','if(0 && fixed_overlap(c->fixed[i].range,ranges[j]))'),
 'pool-fixed-overlap':('if(fixed_overlap(p->members[k].range,range))','if(0 && fixed_overlap(p->members[k].range,range))'),
 'early-retired-refund':('if(done&&f->abandoned)','if(f->abandoned)'),
 'oversized-fixed-result':('if(result>0&&(uint64_t)result>f->range.length)','if(0 && result>0&&(uint64_t)result>f->range.length)'),
 'late-write':('!f->write||f->phase!=FIX_PREPARED','!f->write'),
 }

 for name,(a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/'ring_payload.c';s=src.read_text();assert s.count(a)==1;src.write_text(s.replace(a,b));binary=m/'test';cmd(common+[m/'ring_payload.c',m/'lease_ledger.c',m/'test_ring_payload.c','-o',binary],name+'-build');cmd([binary],name+'-run',True)
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=21,profiles=['asan+ubsan','optimized-ndebug-core'],mutants_rejected=8,records=records,limits='C fixed-I/O extends shared backing owner; simulated terminal/abort/retirement. Request integration and executor pending; SQ reuse parked.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
