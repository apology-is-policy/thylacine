from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh'
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('fixed-io-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['ring_payload.rs','test_ring_payload.rs','lease_ledger.rs','run-fixed-io.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
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
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_ring_payload.rs','-o',binary],'rust-build-'+opt);cmd([binary,'--nocapture'],'rust-run-'+opt,diagnostic='20 passed; 0 failed')
 prefix=(run/'test_ring_payload.rs').read_text()
 cases={
 'fixed-borrow':('let (_p,mut r,c)=fixture();let mut f=c.reserve_fixed(901,id(0,1),range(0),false).unwrap();unsafe{f.publish().unwrap();r.finish_fixed(901,8).unwrap();}let v=f.bytes().unwrap();drop(f);println!("{:?}",v);','E0505'),
 'fixed-mutable-view':('let (_p,mut r,c)=fixture();let mut f=c.reserve_fixed(901,id(0,1),range(0),false).unwrap();unsafe{f.publish().unwrap();r.finish_fixed(901,8).unwrap();}f.bytes().unwrap()[0]=3;','E0594'),
 'return-borrow':('let (_p,_root,c)=fixture();let pool=attach(&c,0,0);let a=accept(&pool,0);let view=a.bytes();let _=a.try_return();println!("{:?}",view);','E0505'),
 'mutate-view':('let (_p,_root,c)=fixture();let pool=attach(&c,0,0);let a=accept(&pool,0);a.bytes()[0]=0;','E0594'),
 'moved-storage':('let p=Probe::new();let b=Backing::new(p);let _=RingOwner::new(b);let _pointer=b.bytes.get();','E0382'),
 'legacy-reap':('let (_p,_root,c)=fixture();let _=c.reap();','E0599'),
 'valid-return':('let (_p,_root,c)=fixture();let pool=attach(&c,0,0);let a=accept(&pool,0);assert_eq!(a.bytes(),&[0;8]);assert!(a.try_return().is_ok());',None),
 }
 for name,(body,diag) in cases.items():
  src=run/(name+'.rs');src.write_text(prefix+'\nfn main(){'+body+'}\n');cmd(['rustc','--edition=2024',src,'-o',run/name],name+'-build',bool(diag),diag)
  if not diag:cmd([run/name],name+'-run')
 variants={
 'early-fixed-view':('let FixedPhase::Terminal(n)=f.phase else{return Err(Error::Busy)};','let n=match f.phase{FixedPhase::Terminal(n)=>n,_=>f.range.length as i32};'),
 'abandon-refund':('f.abandoned=true;Some(f.scope)','let scope=f.scope;s.fixed[self.index]=None;Some(scope)'),
 'missing-abort':('self.core.backend.abort_scope(scope);','let _=scope;'),
 'fixed-pool-overlap':('if overlap(f.range,*r)','if false && overlap(f.range,*r)'),
 'pool-fixed-overlap':('p.ranges.iter().any(|r|overlap(*r,range))','p.ranges.iter().any(|r|false && overlap(*r,range))'),
 'early-retired-refund':('if retired{for slot','if true{for slot'),
 'oversized-fixed-result':('if result>0&&result as u64>f.range.length','if false&&result>0&&result as u64>f.range.length'),
 'late-write':('!f.write||f.phase!=FixedPhase::Prepared','!f.write'),
 }

 for name,(a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/'ring_payload.rs';s=src.read_text();assert s.count(a)==1;src.write_text(s.replace(a,b));binary=m/'test'
  cmd(['rustc','--edition=2024','--test',m/'test_ring_payload.rs','-o',binary],name+'-build');cmd([binary],name+'-run',True,'FAILED')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=20,profiles=2,compile_fail=6,positive_compile_control=1,mutants_rejected=8,records=records,limits='Rust fixed-I/O extends shared backing owner; simulated terminal/abort/retirement. C parity, request integration and executor pending; SQ reuse parked.'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
