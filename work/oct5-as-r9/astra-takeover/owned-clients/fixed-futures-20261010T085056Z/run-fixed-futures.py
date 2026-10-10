from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh'
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('fixed-futures-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['fixed_futures.rs','fixed_requests.rs','test_fixed_futures.rs','ring_payload.rs','request_book.rs','paired_cq.rs','lease_ledger.rs','run-fixed-futures.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
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
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_fixed_futures.rs','-o',binary],'rust-build-'+opt);cmd([binary,'future_','--nocapture'],'rust-run-'+opt,diagnostic='11 passed; 0 failed')
 prefix=(run/'test_fixed_futures.rs').read_text()
 cases={
 'no-clone-future':('let(_,d)=future_driver();let f=d.read(id(0,1),range(0)).unwrap();let _=f.clone();','E0599'),
 'no-send-future':('fn send<T:Send>(_:T){} let(_,d)=future_driver();let f=d.read(id(0,1),range(0)).unwrap();send(f);','E0277'),
 'valid-future':('let(_,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let c=unsafe{f.publish().unwrap()};unsafe{d.receive(typed_record(c,1,0)).unwrap();}let(_,w)=counter();assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(r))if r.result()==1));',None),
 }

 for name,(body,diag) in cases.items():
  src=run/(name+'.rs');src.write_text(prefix+'\nfn main(){'+body+'}\n');cmd(['rustc','--edition=2024',src,'-o',run/name],name+'-build',bool(diag),diag)
  if not diag:cmd([run/name],name+'-run')
 variants={
 'missing-wakeup':('wake(batch);','drop(batch);'),
 'missing-unregister':('hub.borrow_mut().slots[i].take()','{let _=i;None::<Registration>}'),
 'stale-waker':('h.slots[slot.expect("pending future owns registration")].as_mut().unwrap().waker.replace(waker)','{let r=h.slots[slot.unwrap()].as_mut().unwrap();if r.waker.is_none(){r.waker=Some(waker);}None::<Waker>}'),
 'callback-under-borrow':('let batch={self.hub.borrow_mut().batch()};wake(batch);','let mut h=self.hub.borrow_mut();wake(h.batch());'),
 'clone-after-check':('let waker=cx.waker().clone();let status=me.request.try_complete();','let status=me.request.try_complete();let waker=cx.waker().clone();'),
 'preparation-slot-leak':('Err(e)=>{release(&self.hub,&mut slot);Err(e)}','Err(e)=>{Err(e)}'),
 }

 for name,(a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/'fixed_futures.rs';s=src.read_text();assert s.count(a)>=1;src.write_text(s.replace(a,b));binary=m/'test'
  cmd(['rustc','--edition=2024','--test',m/'test_fixed_futures.rs','-o',binary],name+'-build');cmd([binary,'future_'],name+'-run',True,'FAILED')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=11,profiles=2,compile_fail=2,positive_compile_control=1,mutants_rejected=6,records=records,limits='Rust typed Future adapter with bounded wake registrations and simulated completions. No native SQ, sleeping event loop, or retirement-owner integration; SQ fix parked.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
