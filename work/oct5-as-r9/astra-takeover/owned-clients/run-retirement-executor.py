from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh'
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('retirement-executor-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['fixed_futures.rs','retirement_executor.rs','fixed_requests.rs','test_retirement_executor.rs','ring_payload.rs','request_book.rs','paired_cq.rs','lease_ledger.rs','run-retirement-executor.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
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
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_retirement_executor.rs','-o',binary],'rust-build-'+opt);cmd([binary,'executor_','--nocapture'],'rust-run-'+opt,diagnostic='8 passed; 0 failed')
 prefix=(run/'test_retirement_executor.rs').read_text()
 cases={
 'no-clone-client':('let mut e=RetirementExecutor::new(1).unwrap();let(_,c)=managed(&mut e);let _=c.clone();','E0599'),
 'no-send-executor':('fn send<T:Send>(_:T){} let e=RetirementExecutor::<Backing>::new(1).unwrap();send(e);','E0277'),
 'valid-client':('let mut e=RetirementExecutor::new(1).unwrap();let(p,c)=managed(&mut e);drop(c);p.retired.set(true);assert_eq!(e.progress(1).reclaimed,1);',None),
 }

 for name,(body,diag) in cases.items():
  src=run/(name+'.rs');src.write_text(prefix+'\nfn main(){'+body+'}\n');cmd(['rustc','--edition=2024',src,'-o',run/name],name+'-build',bool(diag),diag)
  if not diag:cmd([run/name],name+'-run')
 variants={
 'early-reclaim':('e.retiring&&e.driver.progress()','e.retiring'),
 'missing-retire':('e.retiring=true;e.driver.retire()','e.retiring=true;WakeBatch::empty()'),
 'frozen-cursor':('s.cursor=(i+1)%s.slots.len();','s.cursor=i;'),
 'excess-budget':('budget.min(s.slots.len())','budget.min(s.slots.len())+1'),
 'no-shutdown-close':('e.driver.close()','e.driver.retire()'),
 'premature-admission':('.position(Option::is_none).ok_or(Error::Full)?','.position(Option::is_none).unwrap_or(0)'),
 }

 for name,(a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/'retirement_executor.rs';s=src.read_text();assert s.count(a)>=1;src.write_text(s.replace(a,b));binary=m/'test'
  cmd(['rustc','--edition=2024','--test',m/'test_retirement_executor.rs','-o',binary],name+'-build');cmd([binary,'executor_'],name+'-run',True,'unreachable code' if name=='premature-admission' else 'assertion')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=8,profiles=2,compile_fail=2,positive_compile_control=1,mutants_rejected=6,records=records,limits='Rust bounded retirement owner; simulated backend. No native SQ, event/wakeup or Future qualification; SQ fix parked.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
