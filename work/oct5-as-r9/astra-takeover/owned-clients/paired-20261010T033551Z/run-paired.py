from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R)
Y=R/'work/oct5-as-r9/astra-yip.sh';CC='/opt/homebrew/opt/llvm@22/bin/clang'
SDK=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('paired-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['paired_cq.rs','paired_cq.c','paired_cq.h','test_paired_cq.rs','test_paired_cq.c','run-paired.py']
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={str(D/p):sha(D/p) for p in files}
for p in ['usr/lib/libthyla-rs/src/loom/service_abi.rs','usr/lib/libt/include/thyla/loom_service.h','kernel/include/thylacine/loom.h']:manifest[str(R/p)]=sha(R/p)
(run/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
(run/'source.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']))
(run/'source.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary']))
for n in files:shutil.copy2(D/n,run/n)
records=[]
def limits():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def cmd(args,name,fail=False):
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize//1024**2>8256
 with (run/(name+'.log')).open('wb') as f:r=subprocess.run([str(a) for a in args],stdout=f,stderr=subprocess.STDOUT,preexec_fn=limits,timeout=30)
 records.append(dict(name=name,rc=r.returncode,expected_failure=fail))
 assert (r.returncode!=0) if fail else (r.returncode==0),(name,r.returncode)
try:
 cmd(['rustc','--version'],'rust-version');cmd([CC,'--version'],'clang-version')
 for opt in ['0','2']:
  binary=run/('rust-tests-'+opt);cmd(['rustc','--edition=2024','--test','-C','opt-level='+opt,run/'test_paired_cq.rs','-o',binary],'rust-build-'+opt);cmd([binary,'--nocapture'],'rust-run-'+opt)
 cmd([CC,'-isysroot',SDK,'-std=c11','-Wall','-Wextra','-Werror','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-idirafter',R/'usr/lib/libt/include',run/'paired_cq.c',run/'test_paired_cq.c','-o',run/'c-tests'],'c-build');cmd([run/'c-tests'],'c-run')
 variants={
 'receipt-index':('self.receipts.add(slot)','r->receipts[slot]','self.receipts.add(slot ^ 1)','r->receipts[slot ^ 1]'),
 'double-ack':('self.next.wrapping_add(1)','++r->next','self.next.wrapping_add(2)','r->next+=2'),
 'overlap':('if start < prior+bytes','if(starts[i]<starts[j]+sizes[j]','if false && start < prior+bytes','if(0 && starts[i]<starts[j]+sizes[j]'),
 }
 for name,(ra,ca,rb,cb) in variants.items():
  for lang,a,b in [('rust',ra,rb),('c',ca,cb)]:
   m=run/(lang+'-mutant-'+name);m.mkdir()
   for p in files[:-1]:shutil.copy2(D/p,m/p)
   src=m/('paired_cq.rs' if lang=='rust' else 'paired_cq.c');s=src.read_text();assert s.count(a)==1;src.write_text(s.replace(a,b))
   binary=m/'test'
   args=['rustc','--edition=2024','--test',m/'test_paired_cq.rs','-o',binary] if lang=='rust' else [CC,'-isysroot',SDK,'-std=c11','-pthread','-idirafter',R/'usr/lib/libt/include',m/'paired_cq.c',m/'test_paired_cq.c','-o',binary]
   cmd(args,lang+'-mutant-'+name+'-build');cmd([binary],lang+'-mutant-'+name+'-run',True)
 assert all(sha(Path(p))==h for p,h in manifest.items())
 result=dict(owner='astra',state='passed',rust_scenarios=4,rust_profiles=2,c_sanitizers=['address','undefined'],concurrent_records_per_run=100000,mutants_rejected=6,records=records,limits='Uninstalled paired reader only; concurrency stress is not a memory-model proof or owned-client/native qualification')
 (run/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
