"""Astra's source-frozen Mac matrix with native-qualified image inputs.
Rebuilds both kernel variants only; does not claim ci-smp-gate's userspace bake.
Derives every row from its DEFAULT_MATRIX and uses the actual multiboot harness.
"""
from pathlib import Path
import subprocess,os,re,json,datetime,hashlib,signal,time,sys
R=Path(__file__).resolve().parents[4];os.chdir(R)
D=R/'work/oct5-as-r9'/('pinned-matrix-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));D.mkdir()
Y=R/'work/oct5-as-r9/astra-yip.sh';F=R/'work/oct5-as-r9/astra-takeover/async-integration-matrix-flight.json'
source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
flight=dict(owner='astra',source=source,pid=os.getpid(),state='starting',run=str(D.relative_to(R)),floor_mib=8192,expected_tests=1855,launched_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),next='Keep tracked source frozen; inspect progress/result and raw evidence. This is the five-row matrix stage with rebuilt default/UBSan kernels and pinned native-qualified images, not a successful whole-image ci-smp-gate rebuild. After verification checkpoint, add native general operation/provided exercises then AS3/4 and MM0-MM4.')
def save():F.write_text(json.dumps(flight,indent=2)+'\n')
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
def free():
 s=os.statvfs(R);return s.f_bavail*s.f_frsize//1024**2
def frozen():
 assert subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()==source
 assert not subprocess.check_output(['git','diff','HEAD','--'])
def command(cmd,log,env=None):
 with log.open('wb') as f:
  p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT,env=env,start_new_session=True)
  try:
   while p.poll() is None:
    n=free();flight['minimum_free_mib']=min(flight.get('minimum_free_mib',n),n);flight['current_free_mib']=n;flight['child_pid']=p.pid;save()
    if n<=8256:
     flight['failure']='disk guard reached64MiB margin above8192MiBfloor';save();os.killpg(p.pid,signal.SIGTERM);p.wait();raise RuntimeError(flight['failure'])
    time.sleep(1)
   assert p.returncode==0,(cmd,p.returncode,str(log))
  finally:
   if p.poll() is None:
    os.killpg(p.pid,signal.SIGTERM);p.wait()
  flight.pop('child_pid',None);save()
def terminate(signum,frame):raise SystemExit(128+signum)
signal.signal(signal.SIGTERM,terminate);signal.signal(signal.SIGINT,terminate)
rc=1;save()
try:
 resources=subprocess.check_output([str(Y),'resources'],text=True);(D/'lease.txt').write_text(resources);assert re.search(r'^mac +HELD by you',resources,re.M)
 subprocess.run([str(Y),'api','lease_update',json.dumps(dict(resource='mac',phase='pinned-image full five-row Mac matrix',pids=[os.getpid()]))],check=True,stdout=(D/'lease-update.txt').open('w'))
 frozen();assert free()>8256
 S=Path('/Users/northkillpd/projects/stratum-astra/v2');assert subprocess.check_output(['git','-C',str(S),'rev-parse','HEAD'],text=True).strip()=='61dde3727921e70e2c72fbd3c9e2044a192f4a54';assert not subprocess.check_output(['git','-C',str(S),'status','--porcelain'])
 prior=json.loads((R/'work/oct5-as-r9/astra-takeover/async-integration/native-20261010T010700Z/result.json').read_text())
 image=['build/ramfs.cpio','build/fixtures/pool.img.baked-snapshot','build/fixtures/system.key','build/fixtures/system.key.baked-snapshot']
 for p in image:assert sha(R/p)==prior['inputs'][p],p
 (D/'source.txt').write_text(source+'\n');(D/'start.txt').write_text(datetime.datetime.now(datetime.timezone.utc).isoformat()+'\n')
 matrix_text=(R/'tools/ci-smp-gate.sh').read_text();body=re.search(r'^DEFAULT_MATRIX=\(\n(.*?)^\)',matrix_text,re.M|re.S).group(1)
 rows=[x.split() for x in re.findall(r'^\s*"([^"]+)"',body,re.M)];assert len(rows)==5 and all(len(x)==4 for x in rows)
 (D/'rows.json').write_text(json.dumps(rows,indent=2)+'\n')
 for build,san in [('kernel',''),('kernel-undefined','undefined')]:
  flight.update(state='building',phase=build);save();cache=(R/f'build/{build}/CMakeCache.txt').read_text()
  assert f'CMAKE_HOME_DIRECTORY:INTERNAL={R}' in cache and 'KERNEL_TESTS:BOOL=ON' in cache and f'THYLACINE_SANITIZE:STRING={san}\n' in cache
  command(['cmake','--build',f'build/{build}','--target','thylacine.elf','-j','4'],D/(build+'-build.log'))
  command(['tools/regen-halls-symtab.sh',f'build/{build}'],D/(build+'-symtab.log'));frozen()
 for p in image:assert sha(R/p)==prior['inputs'][p],p
 paths=image+[f'build/{b}/thylacine.{e}' for b in ['kernel','kernel-undefined'] for e in ['elf','bin']]
 inputs={p:sha(R/p) for p in paths};(D/'inputs.json').write_text(json.dumps(inputs,indent=2)+'\n')
 C=D/'inputs';C.mkdir()
 for p in paths:
  dst=C/('thylacine-undefined.'+p.rsplit('.',1)[1] if 'kernel-undefined' in p else Path(p).name)
  subprocess.run(['cp','-c',str(R/p),str(dst)],check=True)
 verdicts=[]
 for label,cpus,san,timeout in rows:
  flight.update(state='running',phase=label,completed_rows=len(verdicts));save();frozen()
  for p,v in inputs.items():assert sha(R/p)==v,p
  env={**os.environ,'SMP_KEEP_LOGS':'1','BOOT_TIMEOUT':timeout}
  command(['tools/smp-multiboot.sh',label,cpus,'10','' if san=='-' else san],D/(label+'-matrix.log'),env)
  verdicts.append(label);(D/'row-results.json').write_text(json.dumps(verdicts)+'\n')
 # Every material result is checked in the independent raw-evidence pass below.
 frozen()
 for p,v in inputs.items():assert sha(R/p)==v,p
 flight.update(state='verifying',completed_rows=5);save()
 command(['python3',str(Path(__file__).with_name('verify-pinned-matrix.py')),str(D)],D/'verification.log')
 rc=0;flight['state']='verified'
except BaseException as e:
 flight.update(state='failed',failure=repr(e));print(repr(e),flush=True)
 rc=e.code if isinstance(e,SystemExit) and isinstance(e.code,int) else 1
finally:
 flight['rc']=rc;flight['finished_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();save()
 (D/'runner.rc').write_text(str(rc)+'\n')
 with (D/'release.txt').open('w') as f:subprocess.run([str(Y),'release','mac'],stdout=f,stderr=subprocess.STDOUT)
print(json.dumps(flight,indent=2));sys.exit(rc)
