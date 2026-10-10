from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh';CC='/opt/homebrew/opt/llvm@22/bin/clang';SDK=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('owned-mapping-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['owned_mapping.rs','owned_mapping.c','owned_mapping.h','paired_cq.rs','paired_cq.c','paired_cq.h','test_owned_mapping.rs','test_owned_mapping.c','run-owned-mapping.py']
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
common=[CC,'-isysroot',SDK,'-std=c11','-Dcalloc=t_test_calloc','-Wall','-Wextra','-Werror','-idirafter',R/'usr/lib/libt/include'];cs=['owned_mapping.c','paired_cq.c']
try:
 cmd(['rustc','--version'],'rust-version');cmd([CC,'--version'],'clang-version')
 for opt in ['0','2']:
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_owned_mapping.rs','-o',binary],'rust-build-'+opt);cmd([binary,'--nocapture'],'rust-run-'+opt,diag='5 passed; 0 failed')
 for mode in ['sanitized','ndebug']:
  extra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2','-DNDEBUG'];objects=[]
  for src in cs:
   obj=run/(mode+'-'+src+'.o');objects.append(obj);cmd(common+extra+['-c',run/src,'-o',obj],mode+'-'+src+'-build')
  testflags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2'];binary=run/(mode+'-tests');cmd(common+testflags+[run/'test_owned_mapping.c',*objects,'-o',binary],mode+'-link');cmd([binary],mode+'-run',diag='C six owned mapping scenarios PASS')
 for name,body,diag in [
  ('moved-backend','let _n=b.len();','E0382'),
  ('clone-owner','let _copy=m.clone();','no method named'),
  ('raw-mapping','let _raw=m.mapping.base();','private'),
  ('owned-control','m.close();',None)]:
  source=run/(name+'.rs');source.write_text((run/'test_owned_mapping.rs').read_text()+'\nfn main(){let(b,p,_q)=fixture(4,8);let mut m=OwnedMapping::new(b,p).unwrap();'+body+'}\n')
  cmd(['rustc','--edition=2024',source,'-o',run/(name+'-test')],name,diag is not None,diag)
 variants=[
 ('rust','sq-overlap','owned_mapping.rs','for j in 0..i{','for j in 0..0{'),
 ('rust','receipt-stride','owned_mapping.rs','p.receipt[2]!=32','false && p.receipt[2]!=32'),
 ('rust','failed-close','owned_mapping.rs','mapping.close();Err(e)','Err(e)'),
 ('rust','after-close','owned_mapping.rs','if self.closed{return Err(Error::Closed);}','if false && self.closed{return Err(Error::Closed);}'),
 ('rust','fresh-header','owned_mapping.rs','word(0)!=0','false && word(0)!=0'),
 ('c','sq-overlap','owned_mapping.c','j<i','j<i && i==99'),
 ('c','receipt-stride','owned_mapping.c','p.receipt[2]!=32','(0 && p.receipt[2]!=32)'),
 ('c','failed-close','owned_mapping.c','b.close(b.context);b.destroy(b.context);','b.destroy(b.context);'),
 ('c','after-close','owned_mapping.c','if(m->closed)return T_MAP_CLOSED;','if(0 && m->closed)return T_MAP_CLOSED;'),
 ('c','fresh-header','owned_mapping.c','if(words[0]||','if((0 && words[0])||'),
 ]
 for lang,name,source,a,b in variants:
  m=run/('mutant-'+lang+'-'+name);m.mkdir()
  for n in files[:-1]:shutil.copy2(D/n,m/n)
  src=m/source;s=src.read_text();assert s.count(a)==1;src.write_text(s.replace(a,b));binary=m/'test';label=lang+'-'+name
  if lang=='rust':cmd(['rustc','--edition=2024','--test',m/'test_owned_mapping.rs','-o',binary],label+'-build');cmd([binary],label+'-run',True,'FAILED')
  else:cmd(common+[*[m/n for n in cs],m/'test_owned_mapping.c','-o',binary],label+'-build');cmd([binary],label+'-run',True,'Assertion failed')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',rust_scenarios=5,c_scenarios=6,rust_profiles=2,c_profiles=['asan+ubsan','ndebug-core'],mutants=10,compile_failures=3,records=records,limits='Owned mapping geometry/lifecycle with simulated backend/producer; no native setup/syscall adapter or SQ publisher yet'),indent=2)+'\n');print(run,flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
