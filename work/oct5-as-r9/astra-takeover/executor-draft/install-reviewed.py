"""One reviewed installation; refuses until the frozen controls matrix is verified.
Does not build, start a gate, commit, retry, or activate user dispatch.
"""
from pathlib import Path
import subprocess,json,hashlib,os,datetime
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
assert not (D/'installed.json').exists(),'historical installation must not be rerun'
manifest=json.loads((D/'verified-manifest.json').read_text());assert manifest['state']=='host-and-syntax-verified'
assert all(x['rc']==0 for x in manifest['checks'])
flight=json.loads((R/'work/oct5-as-r9/astra-takeover/control-current-flight.json').read_text())
assert flight['state']=='verified' and flight['verification_rc']==0
for pid in [42917,42938]:
 try:os.kill(pid,0)
 except ProcessLookupError:pass
 else:raise SystemExit(f'prior runner/monitor pid{pid} still exists; no changes made')
result=json.loads((R/'work/oct5-as-r9/astra-takeover/current-gate-20261009T203019Z-result.json').read_text())
assert result['source']==manifest['source_base']=='ade9c69d17e20a5aa5ac337bfef15d7a5e2304c4'
assert result['clean_boots']==50 and result['expected_tests']==1852 and result['gate_rc']==result['runner_rc']==0
run=R/result['run'];assert (run/'inputs/PINNED').exists()
assert len(result['records'])==50 and len(result['inputs_sha256'])==10
for rec in result['records']:
 for name,digest in rec['sha256'].items():assert sha(run/'verified-logs'/name)==digest
for name,digest in result['inputs_sha256'].items():assert sha(run/'inputs'/name)==digest
assert subprocess.check_output(['git','branch','--show-current'],cwd=R,text=True).strip()=='corona/async-memory'
subprocess.run(['git','diff','--quiet','HEAD'],cwd=R,check=True)
paths=list(manifest['files'])
subprocess.run(['git','diff','--quiet',manifest['source_base'],'HEAD','--',*paths],cwd=R,check=True)
stratum=Path('/Users/northkillpd/projects/stratum-astra/v2')
assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=stratum,text=True).strip()=='61dde3727921e70e2c72fbd3c9e2044a192f4a54'
assert not subprocess.check_output(['git','status','--porcelain'],cwd=stratum,text=True)
astra=Path('/Users/northkillpd/projects/thylacine-astra');pending=json.loads((astra/'work/oct6-modal-caret/pending.json').read_text())
for group in ['paths','protected']:
 for name,digest in pending[group].items():assert sha(astra/name)==digest
assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize>=8*1024**3
resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True)
assert any(x.startswith('mac ') and 'HELD by you' in x for x in resources.splitlines()),resources
source=Path(manifest['install_from']);assert all(sha(source/p)==v for p,v in manifest['files'].items())
record=dict(owner='astra',base=subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),previous_gate=result['source'],previous_gate_run=result['run'],source_files=manifest['files'],original_files={p:sha(R/p) for p in paths},installed=[],state='installing',utc=datetime.datetime.now(datetime.timezone.utc).isoformat())
(D/'installed.json').write_text(json.dumps(record,indent=2)+'\n')
for path in paths:
 assert sha(R/path)==record['original_files'][path]
 (R/path).write_bytes((source/path).read_bytes())
 record['installed'].append(path);(D/'installed.json').write_text(json.dumps(record,indent=2)+'\n')
assert all(sha(R/p)==v for p,v in manifest['files'].items())
record['state']='installed-native-pending';(D/'installed.json').write_text(json.dumps(record,indent=2)+'\n')
print('Installed reviewed executor sources; native qualification pending.')
