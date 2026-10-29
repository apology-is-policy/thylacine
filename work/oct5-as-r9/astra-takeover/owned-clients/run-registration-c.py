from pathlib import Path
import subprocess,hashlib,json,datetime,os,resource,shutil
D=Path(__file__).resolve().parent;R=D.parents[3];os.chdir(R);Y=R/'work/oct5-as-r9/astra-yip.sh';CC='/opt/homebrew/opt/llvm@22/bin/clang';SDK=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
assert 'mac   HELD by you' in subprocess.check_output([str(Y),'resources'],text=True)
run=D/('registration-c-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
files=['ring_payload.c','ring_payload.h','test_ring_payload.c','lease_ledger.c','lease_ledger.h','run-registration-c.py'];sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
manifest={str(D/p):sha(D/p) for p in files};p=R/'usr/lib/libt/include/thyla/loom_service.h';manifest[str(p)]=sha(p)
(run/'source-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n');(run/'source.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']));(run/'source.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--binary']))
for n in files:shutil.copy2(D/n,run/n)
records=[]
def limits():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
def cmd(args,name,fail=False):
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize//1024**2>8256
 with (run/(name+'.log')).open('wb') as f:r=subprocess.run([str(a) for a in args],stdout=f,stderr=subprocess.STDOUT,preexec_fn=limits,timeout=30)
 records.append(dict(name=name,rc=r.returncode,expected_failure=fail));assert (r.returncode!=0) if fail else (r.returncode==0),(name,r.returncode)
common=[CC,'-isysroot',SDK,'-Dcalloc=t_test_calloc','-std=c11','-Wall','-Wextra','-Werror','-idirafter',R/'usr/lib/libt/include']
try:
 cmd([CC,'--version'],'clang-version')
 for mode in ['sanitized','ndebug']:
  extra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2','-DNDEBUG']
  objects=[]
  for src in ['ring_payload.c','lease_ledger.c']:
   obj=run/(mode+'-'+src+'.o');objects.append(obj);cmd(common+extra+['-c',run/src,'-o',obj],mode+'-'+src+'-build')
  # Test assertions stay enabled even when production objects use NDEBUG.
  testextra=['-fsanitize=address,undefined','-fno-omit-frame-pointer'] if mode=='sanitized' else ['-O2']
  binary=run/(mode+'-tests');cmd(common+testextra+[run/'test_ring_payload.c',*objects,'-o',binary],mode+'-link');cmd([binary],mode+'-run')
 variants={
 'live-retire':('c->closing&&!c->live&&','c->closing&&'),
 'pool-closes-ring':('c->b.retire_pool(c->b.context,p->identity);','retire(c);'),
 'global-cap':('if(n>64-c->members)return T_LEASE_BUSY;',''),
 'cross-pool-overlap':('if(a.buffer_index==b.buffer_index','if(0 && a.buffer_index==b.buffer_index'),
 'failed-destroy':('if(c->b.destroy_pool(c->b.context,p->identity)==0)','if(c->b.destroy_pool(c->b.context,p->identity)<=0)'),
 'stale-pool':('if(identity.incarnation<=c->last[slot])return T_LEASE_STALE;',''),
 'missing-precharge':('c->members+=n;struct t_loom_service_ref identity={0};','struct t_loom_service_ref identity={0};'),
 'uncertain-no-close':('c->quarantine=p;close_owner(c);','c->quarantine=p;'),
 'rollback-no-refund':('c->members-=n;free(p);return result;','free(p);return result;'),
 'accept-nonzero-output':('result<0&&!identity.slot&&!identity.reserved&&!identity.incarnation','result<0'),
 }
 for name,(a,b) in variants.items():
  m=run/('mutant-'+name);m.mkdir()
  for p in files[:-1]:shutil.copy2(D/p,m/p)
  src=m/'ring_payload.c';s=src.read_text();assert s.count(a)==1;src.write_text(s.replace(a,b));binary=m/'test';cmd(common+[m/'ring_payload.c',m/'lease_ledger.c',m/'test_ring_payload.c','-o',binary],name+'-build');cmd([binary],name+'-run',True)
 assert all(sha(Path(p))==h for p,h in manifest.items())
 (run/'result.json').write_text(json.dumps(dict(owner='astra',state='passed',scenarios=14,profiles=['asan+ubsan','optimized-ndebug-core'],mutants_rejected=10,records=records,limits='Uninstalled full-ring C payload core; simulated backend, native syscall/CQ/futures/executor integration pending'),indent=2)+'\n');print(str(run),flush=True)
finally:
 (run/'commands.json').write_text(json.dumps(records,indent=2)+'\n');subprocess.run([str(Y),'release','mac'],check=False)
