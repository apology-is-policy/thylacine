from pathlib import Path
import datetime,json,os,subprocess,time
root=Path('/Users/northkillpd/projects/thylacine-corona');os.chdir(root)
f=root/'work/oct5-as-r9/astra-takeover/control-current-flight.json';d=json.loads(f.read_text());run=root/d['run']
# No build/source mutation. Preserve failed rc/verifier output without retries.
while not (run/'runner.rc').exists():
 try:os.kill(d['pid'],0)
 except ProcessLookupError:
  d.update(state='failed',failure='runner exited without runner.rc');f.write_text(json.dumps(d,indent=2)+'\n');raise SystemExit(1)
 time.sleep(5)
with (run/'verification.log').open('wb') as log:
 r=subprocess.run(['python3',str(root/d['verifier'])],stdout=log,stderr=subprocess.STDOUT)
d.update(state='verified' if r.returncode==0 else 'failed',verification_rc=r.returncode,verified_utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
if r.returncode==0:
 d['next']='Record verified evidence checkpoint, then continue only reviewed/tested next async control draft after acquiring Astra Mac lease. No Main/private activation/128MiB change.'
f.write_text(json.dumps(d,indent=2)+'\n')
raise SystemExit(r.returncode)
