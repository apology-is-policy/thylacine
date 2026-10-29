#!/usr/bin/env python3
"""Continue this approved checkpoint after the frozen request-owner matrix ends.

One event wait, no retries or activation. Failures retain evidence and stop.
"""
from pathlib import Path
import datetime,hashlib,json,os,select,subprocess,traceback
root=next(p for p in Path(__file__).resolve().parents if (p/'kernel/loom.c').exists())
os.chdir(root)
draft=Path(__file__).resolve().parent
base=root/'work/oct5-as-r9'
gate=base/'current-gate-20261009T065820Z'
state_path=draft/'continuation.json'
state={'status':'waiting_for_request_gate','gate':str(gate),'gate_pid':10770,'pid':os.getpid()}
held=False
def record(status,**kw):
 state.update(status=status,updated=datetime.datetime.now(datetime.timezone.utc).isoformat(),**kw)
 state_path.write_text(json.dumps(state,indent=2)+'\n');print(status,flush=True)
def run(cmd,log):
 with (draft/log).open('wb') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
def floor():
 st=os.statvfs(root);assert st.f_bavail*st.f_frsize>=8*1024**3
def pins():
 a=Path('/Users/northkillpd/projects/thylacine-astra');m=json.loads((a/'work/oct6-modal-caret/pending.json').read_text())
 assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=a,text=True).strip()==m['head']
 assert hashlib.sha256(subprocess.check_output(['git','diff','--cached'],cwd=a)).hexdigest()==m['index_sha256']
 for name,h in (m['paths']|m['protected']).items():assert hashlib.sha256((a/name).read_bytes()).hexdigest()==h,name
 s='/Users/northkillpd/projects/stratum-astra/v2'
 assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=s,text=True).strip()=='61dde3727921e70e2c72fbd3c9e2044a192f4a54'
 assert not subprocess.check_output(['git','status','--porcelain'],cwd=s)
try:
 record('waiting_for_request_gate')
 if not (gate/'runner.rc').exists():
  q=select.kqueue()
  try:
   event=select.kevent(10770,filter=select.KQ_FILTER_PROC,flags=select.KQ_EV_ADD|select.KQ_EV_ONESHOT,fflags=select.KQ_NOTE_EXIT)
   try:q.control([event],1,None)
   except ProcessLookupError:pass
  finally:q.close()
 assert (gate/'runner.rc').read_text().strip()=='0'
 assert (gate/'gate.rc').read_text().strip()=='0'
 record('verifying_request_gate')
 run(['python3',str(gate/'verify.py')],'request-gate-verification.log')
 flight=base/'astra-takeover/request-owner-current-flight.json';d=json.loads(flight.read_text())
 d.update(status='completed',verified_at=datetime.datetime.now(datetime.timezone.utc).isoformat(),result=str(base/'astra-takeover/current-gate-20261009T065820Z-result.json'))
 flight.write_text(json.dumps(d,indent=2)+'\n')
 pins();floor()
 run(['sh',str(base/'astra-yip.sh'),'hold','mac','Astra charged protocol storage qualification','--for','2h','--now'],'lease-acquire.log');held=True
 record('installing_protocol_draft')
 run(['python3',str(draft/'install-draft.py')],'install.log')
 record('building_protocol_checkpoint')
 floor();run(['cmake','--build','build/kernel','-j4'],'build.log')
 run(['tools/regen-halls-symtab.sh','build/kernel',''],'symbols.log')
 stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
 out=base/'astra-takeover'/('protocol-owner-diagnostic-'+stamp);out.mkdir()
 source=(base/'astra-takeover/request-owner-diagnostic-20261009T064808Z/run.py').read_text()
 source=source.replace("['kernel/test/loom_requests_fixture.h','kernel/include/thylacine/loom_service_requests.h','kernel/loom_service_requests.c']","['kernel/include/thylacine/loom_service_protocol.h','kernel/loom_service_protocol.c']")
 source=source.replace('loom.private_requests','loom.private_protocol').replace('all1847','all1848').replace('1847/1847','1848/1848').replace("'node_owner'","'protocol_owner'")
 (out/'run.py').write_text(source)
 record('running_protocol_native_diagnostic',diagnostic=str(out))
 floor();run(['python3',str(out/'run.py')],'native-diagnostic.log')
 pins();record('protocol_native_passed',result=str(out/'result.json'))
except BaseException as e:
 record('failed',error=repr(e));traceback.print_exc();raise
finally:
 if held:run(['sh',str(base/'astra-yip.sh'),'release','mac'],'lease-release.log')
