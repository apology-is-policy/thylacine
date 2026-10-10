from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh'
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('fixed-requests-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['fixed_requests.rs','test_fixed_requests.rs','ring_payload.rs','request_book.rs','paired_cq.rs','lease_ledger.rs','run-fixed-requests.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
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
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_fixed_requests.rs','-o',binary],'rust-build-'+opt);cmd([binary,'typed_','--nocapture'],'rust-run-'+opt,diagnostic='10 passed; 0 failed')
 prefix=(run/'test_fixed_requests.rs').read_text()
 cases={
 'no-active-view':('let (_p,d)=typed_driver(1);let r=d.read(id(0,1),range(0)).unwrap();let _=r.bytes();','E0599'),
 'no-clone':('let (_p,d)=typed_driver(1);let r=d.read(id(0,1),range(0)).unwrap();let _=r.clone();','E0599'),
 'result-borrow':('let (_p,mut d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};unsafe{d.receive(typed_record(cookie,3,0)).unwrap();}let done=r.try_complete().unwrap().unwrap();let v=done.bytes().unwrap();drop(done);println!("{:?}",v);','E0505'),
 'no-mutable-result':('let (_p,mut d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};unsafe{d.receive(typed_record(cookie,3,0)).unwrap();}let done=r.try_complete().unwrap().unwrap();done.bytes().unwrap()[0]=7;','E0594'),
 'valid-result':('let (_p,mut d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};unsafe{d.receive(typed_record(cookie,3,0)).unwrap();}let done=r.try_complete().unwrap().unwrap();assert_eq!(done.bytes().unwrap(),&[0;3]);',None),
 }

 for name,(body,diag) in cases.items():
  src=run/(name+'.rs');src.write_text(prefix+'\nfn main(){'+body+'}\n');cmd(['rustc','--edition=2024',src,'-o',run/name],name+'-build',bool(diag),diag)
  if not diag:cmd([run/name],name+'-run')
 variants={
 'unpublished-leak':('core.book.cancel_unpublished(cookie).expect("fresh request");','let _=cookie;'),
 'lost-abandoned-terminal':('if delivery.abandoned{','if false && delivery.abandoned{'),
 'retirement-without-proof':('if c.owner.progress(){','if true{'),
 'retired-metadata-leak':('unsafe{c.book.clear_retired();}',''),
 'missing-storage-terminal':('c.owner.finish_fixed(delivery.cookie,delivery.record.cqe.result)','{let _=delivery.cookie;Ok::<(),StorageError>(())}'),
 'corrupt-keeps-open':('self.owner.close();',''),
 }

 for name,(a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/'fixed_requests.rs';s=src.read_text();assert s.count(a)>=1;src.write_text(s.replace(a,b));binary=m/'test'
  cmd(['rustc','--edition=2024','--test',m/'test_fixed_requests.rs','-o',binary],name+'-build');cmd([binary,'typed_'],name+'-run',True,'FAILED')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=10,profiles=2,compile_fail=4,positive_compile_control=1,mutants_rejected=6,records=records,limits='Rust typed fixed request controller; simulated paired records, abort/retirement. No native SQ, wakers, std Future or executor qualification. SQ fix parked.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
