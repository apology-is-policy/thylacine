from pathlib import Path
import datetime,json,subprocess,os
R=Path('/Users/northkillpd/projects/thylacine-corona');os.chdir(R)
D=R/'work/oct5-as-r9/astra-takeover/operations-draft';state=D.parent/'next-operations.json'
out=D.parent/('operation-storage-native-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'))
j=json.loads(state.read_text());j.update(state='native-storage-running',native_run=str(out.relative_to(R)),native_pid=os.getpid());state.write_text(json.dumps(j,indent=2)+'\n')
try:
 with (D/'native-launch.log').open('wb') as f:r=subprocess.run(['python3',str(D/'native-diagnostic.py'),str(out)],stdout=f,stderr=subprocess.STDOUT,env={**os.environ,'STRATUM_SRC':'/Users/northkillpd/projects/stratum-astra/v2'})
 j.update(state='native-storage-verified' if r.returncode==0 else 'native-storage-failed',native_rc=r.returncode,next='Read native raw/result and update docs/evidence checkpoint; bounded storage only. Continue RPC/FID/buffer integration; no general-operation/full-Mac qualification claim.')
 state.write_text(json.dumps(j,indent=2)+'\n');print(j['state'],out)
finally:
 with (D/'native-resource-release.log').open('wb') as f:subprocess.run([str(R/'work/oct5-as-r9/astra-yip.sh'),'release','mac'],stdout=f,stderr=subprocess.STDOUT)
