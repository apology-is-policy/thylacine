from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh'
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('local-tasks-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['local_tasks.rs','local_wait.rs','fixed_futures.rs','retirement_executor.rs','fixed_requests.rs','test_local_tasks.rs','ring_payload.rs','request_book.rs','paired_cq.rs','lease_ledger.rs','run-local-tasks.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={str(D/p):sha(D/p) for p in files};p=R/'usr/lib/libthyla-rs/src/loom/service_abi.rs';manifest[str(p)]=sha(p)
(run/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');(run/'source.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']));(run/'source.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary']))
for n in files:shutil.copy2(D/n,run/n)
records=[]
def limits():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def cmd(args,name,fail=False,diagnostic=None):
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize//1024**2>8256
 log=run/(name+'.log')
 with log.open('wb') as f:r=subprocess.run([str(a) for a in args],stdout=f,stderr=subprocess.STDOUT,preexec_fn=limits,timeout=30)
 records.append(dict(name=name,rc=r.returncode,expected_failure=fail,diagnostic=diagnostic))
 assert (r.returncode!=0) if fail else (r.returncode==0),(name,r.returncode)
 if diagnostic:assert diagnostic in log.read_text(),name
try:
 cmd(['rustc','--version'],'rust-version')
 for opt in ['0','2']:
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_local_tasks.rs','-o',binary],'rust-build-'+opt);cmd([binary,'task_','--nocapture'],'rust-run-'+opt,diagnostic='10 passed; 0 failed')
 prefix=(run/'test_local_tasks.rs').read_text()
 cases={
 'no-clone-tasks':('let t=Tasks::new(1).unwrap();let _=t.clone();','E0599'),
 'no-send-tasks':('fn send<T:Send>(_:T){} send(Tasks::new(1).unwrap());','E0277'),
 'valid-task':('let mut t=Tasks::new(1).unwrap();t.spawn(||async{}).unwrap();assert_eq!(t.dispatch(1).completed,1);',None),
 }

 for name,(body,diag) in cases.items():
  src=run/(name+'.rs');src.write_text(prefix+'\nfn main(){'+body+'}\n');cmd(['rustc','--edition=2024',src,'-o',run/name],name+'-build',bool(diag),diag)
  if not diag:cmd([run/name],name+'-run')
 variants={
 'lost-self-wake':('local_tasks.rs','self.slots[i]=Some(entry);','entry.ready.ready.store(false,Ordering::Release);self.slots[i]=Some(entry);'),
 'poll-quiet-tasks':('local_tasks.rs','e.ready.ready.swap(false,Ordering::AcqRel)','{e.ready.ready.store(false,Ordering::Release);true}'),
 'ignore-budget':('local_tasks.rs','budget.min(self.slots.len())','self.slots.len()'),
 'stale-cancel':('local_tasks.rs','e.id==task.id','true'),
 'drop-pending-task':('local_tasks.rs','self.slots[i]=Some(entry);','drop(entry);'),
 'lost-wake':('local_tasks.rs','self.ready.store(true,Ordering::Release);','self.ready.store(false,Ordering::Release);'),
 }

 for name,(target,a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/target;s=src.read_text();assert s.count(a)>=1;src.write_text(s.replace(a,b));binary=m/'test'
  cmd(['rustc','--edition=2024','--test',m/'test_local_tasks.rs','-o',binary],name+'-build');cmd([binary,'task_'],name+'-run',True,'FAILED')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=10,profiles=2,compile_fail=2,positive_compile_control=1,mutants_rejected=6,records=records,limits='Bounded creator-thread task dispatch and fixed-Future ownership on simulated backends; no native event wake or wait/readiness composition qualification.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
