from pathlib import Path
import subprocess,os,json,time,signal,hashlib,datetime
R=Path(__file__).resolve().parents[4];os.chdir(R);D=Path(__file__).resolve().parent
Y=R/'work/oct5-as-r9/astra-yip.sh';F=D.parent/'native-faults-flight.json'
source=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();diff=subprocess.check_output(['git','diff','HEAD','--binary'])
run=D/('smoke-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));run.mkdir()
f=dict(owner='astra',pid=os.getpid(),state='starting',source=source,diff_sha256=hashlib.sha256(diff).hexdigest(),run=str(run.relative_to(R)),next='Keep source frozen; five one-boot default1/4/8,UBSan4/8 smoke rows. Verify and checkpoint; not50boot qualification. Then AS-3 hostile peers/owned clients,AS-4,MM0-MM4. No activation.')
def save():F.write_text(json.dumps(f,indent=2)+'\n')
def frozen():
 assert source==subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
 assert diff==subprocess.check_output(['git','diff','HEAD','--binary'])
def cmd(args,log):
 with log.open('wb') as out:
  p=subprocess.Popen(args,stdout=out,stderr=subprocess.STDOUT,start_new_session=True)
  try:
   while p.poll() is None:
    st=os.statvfs(R);n=st.f_bavail*st.f_frsize//1024**2;f.update(child_pid=p.pid,minimum_free_mib=min(f.get('minimum_free_mib',n),n),free_mib=n);save()
    if n<=8256:raise RuntimeError('disk stop margin reached')
    time.sleep(1)
   assert p.returncode==0,(args,p.returncode)
  finally:
   if p.poll() is None:os.killpg(p.pid,signal.SIGTERM);p.wait()
   f.pop('child_pid',None);save()
def stop(signum,frame):raise SystemExit(128+signum)
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
rc=1;save()
try:
 resources=subprocess.check_output([str(Y),'resources'],text=True);assert 'mac   HELD by you' in resources
 (run/'lease.txt').write_text(resources)
 subprocess.run([str(Y),'api','lease_update',json.dumps(dict(resource='mac',phase='native fault five-row smoke',pids=[os.getpid()]))],check=True,stdout=(run/'lease-update.txt').open('w'))
 frozen()
 for build in ['kernel','kernel-undefined']:
  f.update(state='building',phase=build);save()
  cmd(['cmake','--build','build/'+build,'--target','thylacine.elf','-j','4'],run/(build+'-build.log'))
  cmd(['tools/regen-halls-symtab.sh','build/'+build],run/(build+'-symtab.log'))
 for label,cpus,san in [('default1',1,''),('default4',4,''),('default8',8,''),('ubsan4',4,'undefined'),('ubsan8',8,'undefined')]:
  frozen();f.update(state='running',phase=label);save()
  # Only mutable scratch is reset, after each prior VM has exited. Immutable
  # snapshot, matched kernel clones and raw logs stay untouched.
  p=R/'build/fixtures/pool.img';tmp=p.with_name('pool.img.astra-reset')
  subprocess.run(['cp','-c',str(p.with_name('pool.img.baked-snapshot')),str(tmp)],check=True);os.replace(tmp,p)
  cmd(['python3',str(D/'smoke-diagnostic.py'),str(run/label),str(cpus),san],run/(label+'.log'))
 frozen();f['state']='verified';rc=0
except BaseException as e:f.update(state='failed',failure=repr(e));print(repr(e),flush=True)
finally:
 f.update(rc=rc,finished_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());save()
 (run/'runner.rc').write_text(str(rc)+'\n')
 with (run/'release.txt').open('w') as out:subprocess.run([str(Y),'release','mac'],stdout=out,stderr=subprocess.STDOUT)
