from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh'
CC='/opt/homebrew/opt/llvm@22/bin/clang';SDK=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('request-book-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['request_book.rs','request_book.c','request_book.h','test_request_book.rs','test_request_book.c','paired_cq.rs','paired_cq.h','run-request-book.py']
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
common=[CC,'-isysroot',SDK,'-std=c11','-DT_REQUEST_TEST','-Wall','-Wextra','-Werror','-idirafter',R/'usr/lib/libt/include']
try:
 cmd(['rustc','--version'],'rust-version');cmd([CC,'--version'],'clang-version')
 for opt in ['0','2']:
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_request_book.rs','-o',binary],'rust-build-'+opt);cmd([binary,'--nocapture'],'rust-run-'+opt)
 for mode in ['sanitized','ndebug']:
  extra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2','-DNDEBUG']
  obj=run/(mode+'.o');cmd(common+extra+['-c',run/'request_book.c','-o',obj],mode+'-build')
  testflags=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2']
  binary=run/(mode+'-tests');cmd(common+testflags+[run/'test_request_book.c',obj,'-o',binary],mode+'-link');cmd([binary],mode+'-run')
 prefix=(run/'test_request_book.rs').read_text()
 for name,body,diag in [('clone','let b=RequestBook::new(2).unwrap();let _=b.clone();','E0599'),('send','let b=RequestBook::new(2).unwrap();std::thread::spawn(move||drop(b));','E0277'),('valid','let mut b=RequestBook::new(2).unwrap();let a=b.reserve(id(1,1),Kind::Control).unwrap();b.cancel_unpublished(a).unwrap();',None)]:
  src=run/(name+'.rs');src.write_text(prefix+'\nfn main(){'+body+'}\n');cmd(['rustc','--edition=2024',src,'-o',run/name],name+'-build',bool(diag),diag)
  if not diag:cmd([run/name],name+'-run')
 variants={
 'rust':[
 ('reused-cookie','let id = cookie(&NEXT_COOKIE)?;','let _ = cookie(&NEXT_COOKIE)?; let id = 1;'),
 ('more-final','let terminal = flags & 1 == 0;','let terminal = true;'),
 ('duplicate-terminal','e.phase = Phase::Terminal; e.terminal','e.phase = Phase::Active; e.terminal'),
 ('wrong-pool','record.receipt.pool != pool','false && record.receipt.pool != pool'),
 ('oversized-fixed','(result > 0 && result as u32 > limit)','false'),
 ('repeated-abort','!e.abandoned && e.phase == Phase::Active','e.phase == Phase::Active'),
 ],
 'c':[
 ('reused-cookie','*out=n;return 0;','*out=1;return 0;'),
 ('more-final','unsigned terminal=!(flags&1u);','unsigned terminal=1;'),
 ('duplicate-terminal','e->phase=TERMINAL;e->terminal','e->phase=ACTIVE;e->terminal'),
 ('wrong-pool','!same(r->receipt.pool,e->spec.pool)','(0 && !same(r->receipt.pool,e->spec.pool))'),
 ('oversized-fixed','result>0&&(uint32_t)result>e->spec.limit','result>0&&0'),
 ('repeated-abort','!e->abandoned&&e->phase==ACTIVE','e->phase==ACTIVE'),
 ]}
 for lang,items in variants.items():
  for name,a,b in items:
   m=run/('mutant-'+lang+'-'+name);m.mkdir()
   for n in files[:-1]:shutil.copy2(D/n,m/n)
   src=m/('request_book.rs' if lang=='rust' else 'request_book.c');s=src.read_text();assert s.count(a)==1,(lang,name,s.count(a));src.write_text(s.replace(a,b));binary=m/'test';label=lang+'-'+name
   if lang=='rust':cmd(['rustc','--edition=2024','--test',m/'test_request_book.rs','-o',binary],label+'-build')
   else:cmd(common+[m/'request_book.c',m/'test_request_book.c','-o',binary],label+'-build')
   cmd([binary],label+'-run',True,'FAILED' if lang=='rust' else 'Assertion failed')
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios_per_language=7,rust_profiles=2,c_profiles=['asan+ubsan','ndebug-core'],compile_fail=2,mutants=12,records=records,limits='Uninstalled metadata only; no native mapping/syscall, payload transfer, fixed I/O or future/executor ownership integration'),indent=2)+'\n');print(run,flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
