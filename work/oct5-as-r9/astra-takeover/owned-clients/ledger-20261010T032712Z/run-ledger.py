from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R)
Y=R/'work/oct5-as-r9/astra-yip.sh'
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('ledger-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['lease_ledger.rs','lease_ledger.c','lease_ledger.h','test_lease_ledger.rs','test_lease_ledger.c','run-ledger.py']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={str(D/p):sha(D/p) for p in files}
manifest[str(R/'usr/lib/libthyla-rs/src/loom/service_abi.rs')]=sha(R/'usr/lib/libthyla-rs/src/loom/service_abi.rs')
manifest[str(R/'usr/lib/libt/include/thyla/loom_service.h')]=sha(R/'usr/lib/libt/include/thyla/loom_service.h')
(run/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(run/'source.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']))
(run/'source.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary']))
for n in files:shutil.copy2(D/n,run/n)
records=[]
def limits():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def cmd(args,name,expect_fail=False):
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize//1024**2>8256
 with (run/(name+'.log')).open('wb') as f:
  r=subprocess.run([str(a) for a in args],stdout=f,stderr=subprocess.STDOUT,preexec_fn=limits)
 records.append(dict(name=name,rc=r.returncode,expected_failure=expect_fail))
 assert (r.returncode!=0) if expect_fail else (r.returncode==0),(name,r.returncode)
try:
 cmd(['rustc','--version'],'rust-version');cmd(['clang','--version'],'clang-version')
 for opt in ['0','2']:
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_lease_ledger.rs','-o',binary],'rust-build-'+opt);cmd([binary,'--nocapture'],'rust-run-'+opt)
 cmd(['clang','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-idirafter',R/'usr/lib/libt/include',run/'lease_ledger.c',run/'test_lease_ledger.c','-o',run/'c-tests'],'c-build');cmd([run/'c-tests'],'c-run')
 variants={
 'owner':('token.owner != self.owner || ','t->owner!=b->owner || ','',''),
 'stale':('if receipt.lease <= member.last { return Err(LeaseError::Stale); }','if(r.lease<=m->last)return T_LEASE_STALE;','',''),
 'failed-return':('if syscall_result == 0 {','if(rc==0)','if syscall_result <= 0 {','if(rc<=0)'),
 'busy':('if member.state != State::Available { return Err(LeaseError::Busy); }','if(m->state!=AVAILABLE)return T_LEASE_BUSY;','',''),
 'final-release':('return Ok(None);','return 0; /* EOF/final/error','self.outstanding = 0; return Ok(None);','b->outstanding=0;return 0; /* EOF/final/error'),
 'overlap':('if prior.buffer_index == range.buffer_index &&','if(p.buffer_index==r.buffer_index &&','if false && prior.buffer_index == range.buffer_index &&','if(0 && p.buffer_index==r.buffer_index &&'),
 }
 for name,(ra,ca,rb,cb) in variants.items():
  for lang,a,b in [('rust',ra,rb),('c',ca,cb)]:
   m=run/(lang+'-mutant-'+name);m.mkdir()
   for p in ['lease_ledger.rs','lease_ledger.c','lease_ledger.h','test_lease_ledger.rs','test_lease_ledger.c']:shutil.copy2(D/p,m/p)
   src=m/('lease_ledger.rs' if lang=='rust' else 'lease_ledger.c');s=src.read_text();assert s.count(a)==1,(lang,name,s.count(a));src.write_text(s.replace(a,b))
   binary=m/'test'
   args=['rustc','--edition=2024','--test',m/'test_lease_ledger.rs','-o',binary] if lang=='rust' else ['clang','-std=c11','-idirafter',R/'usr/lib/libt/include',m/'lease_ledger.c',m/'test_lease_ledger.c','-o',binary]
   cmd(args,lang+'-mutant-'+name+'-build');cmd([binary],lang+'-mutant-'+name+'-run',True)
 assert all(sha(Path(p))==h for p,h in manifest.items())
 result=dict(owner='astra',state='passed',rust_scenarios=6,rust_profiles=2,c_sanitizers=['address','undefined'],mutants_rejected=12,records=records,limits='Uninstalled metadata ledger only; no safe storage/borrow, syscall, paired-CQ or native client qualification')
 (run/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n')
 subprocess.run([str(Y),'release','mac'],check=False)
