from pathlib import Path
import time,json,subprocess,datetime,os
R=Path('/Users/northkillpd/projects/thylacine-corona');os.chdir(R);F=R/'work/oct5-as-r9/astra-takeover/async-integration-flight.json';D=Path(__file__).resolve().parent
while not (D/'runner.rc').exists():
 time.sleep(15)
 try:os.kill(67665,0)
 except ProcessLookupError:
  if not (D/'runner.rc').exists():
   f=json.loads(F.read_text());f.update(state='failed-runner-no-result');F.write_text(json.dumps(f,indent=2)+'\n');raise SystemExit(1)
# Final status is written after resource release; allow exit path to finish.
time.sleep(2)
rc=int((D/'runner.rc').read_text())
if rc==0:
 with (D/'verification.log').open('wb') as log:rc=subprocess.run(['python3',str(D/'verify.py')],stdout=log,stderr=subprocess.STDOUT).returncode
f=json.loads(F.read_text());f.update(state='verified' if rc==0 else 'failed',verification_rc=rc,verified_utc=datetime.datetime.now(datetime.timezone.utc).isoformat());F.write_text(json.dumps(f,indent=2)+'\n')
