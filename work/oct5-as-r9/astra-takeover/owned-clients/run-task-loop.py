from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh'
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('task-loop-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['task_loop.rs','local_tasks.rs','local_wait.rs','fixed_futures.rs','retirement_executor.rs','fixed_requests.rs','test_task_loop.rs','ring_payload.rs','request_book.rs','paired_cq.rs','lease_ledger.rs','run-task-loop.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
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
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_task_loop.rs','-o',binary],'rust-build-'+opt);cmd([binary,'loop_','--nocapture'],'rust-run-'+opt,diagnostic='9 passed; 0 failed')
 prefix=(run/'test_task_loop.rs').read_text()
 cases={
 'no-clone-tasks':('let t=Tasks::new(1).unwrap();let _=t.clone();','E0599'),
 'no-send-tasks':('fn send<T:Send>(_:T){} send(Tasks::new(1).unwrap());','E0277'),
 'valid-task':('let mut t=Tasks::new(1).unwrap();t.spawn(||async{}).unwrap();assert_eq!(t.dispatch(1).completed,1);',None),
 }

 for name,(body,diag) in cases.items():
  src=run/(name+'.rs');src.write_text(prefix+'\nfn main(){'+body+'}\n');cmd(['rustc','--edition=2024',src,'-o',run/name],name+'-build',bool(diag),diag)
  if not diag:cmd([run/name],name+'-run')
 variants={
 'skip-initial-runnable':('local_wait.rs','if queue.needs_progress()||runnable(){return','if queue.needs_progress(){return'),
 'skip-final-runnable':('local_wait.rs','else if queue.needs_progress()||runnable(){','else if queue.needs_progress(){'),
 'skip-task-dispatch':('task_loop.rs','tasks.dispatch(b.tasks)','tasks.dispatch(0)'),
 'ignore-task-budget':('task_loop.rs','tasks.dispatch(b.tasks)','tasks.dispatch(64)'),
 'poll-after-outer-expiry':('task_loop.rs','if outer_deadline.is_some_and(|d|source.now()>=d)','if false'),
 'ready-starves-timers':('local_wait.rs','let cancellation_requests=queue.expire(now,ring_budget,fixed_budget);','let cancellation_requests=0;'),
 }

 for name,(target,a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/target;s=src.read_text();assert s.count(a)>=1;src.write_text(s.replace(a,b));binary=m/'test'
  cmd(['rustc','--edition=2024','--test',m/'test_task_loop.rs','-o',binary],name+'-build');cmd([binary,'loop_'],name+'-run',True,'FAILED')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=9,profiles=2,compile_fail=2,positive_compile_control=1,mutants_rejected=6,records=records,limits='Bounded task/timer/retirement composition with simulated creator-thread event callbacks; no native/cross-thread wake-to-park qualification.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
