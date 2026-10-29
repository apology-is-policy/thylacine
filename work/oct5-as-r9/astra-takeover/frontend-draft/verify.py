from pathlib import Path
import json,hashlib,os,subprocess
D=Path(__file__).resolve().parent;R=D.parents[3]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
run=D/'controls-20261010T005558Z';h=json.loads((run/'result.json').read_text())
for p,v in h['inputs'].items():assert sha(R/p)==v,p
for x in h['results']:
 p=run/x['case'];assert sha(p.with_suffix('.c'))==x['source_sha256'];assert sha(p.with_suffix('.exe'))==x['binary_sha256']
 log=p.with_suffix('.log').read_text();want=x['expected']
 if want:assert log.endswith(('INVARIANT '+want[1:] if want.startswith('!') else 'FAIL '+want)+'\n')
 else:assert log.endswith('PASS immutable frontend: 3674 checks\n') and x['rc']==0
 assert not p.with_name(p.name+'-build.log').read_bytes()
a=json.loads((D/'arm64-result.json').read_text())
for x in a:
 for p,v in x['inputs'].items():assert sha(R/p)==v,p
 cmd=x['command'];assert sha(Path(cmd[cmd.index('-o')+1]))==x['object_sha256']
models=D/'lifecycle-models-20261010T005543Z';m=json.loads((models/'result.json').read_text())
for p,v in m['inputs'].items():assert sha(Path(p))==v,p
for x in m['results']:assert x['ok'] and sha(models/(Path(x['config']).stem+'.log'))==x['sha256']
A=Path('/Users/northkillpd/projects/thylacine-astra');d=json.loads((A/'work/oct6-modal-caret/pending.json').read_text())
for group in ['paths','protected']:
 for p,v in d[group].items():assert sha(A/p)==v,p
S='/Users/northkillpd/projects/stratum-astra/v2'
assert subprocess.check_output(['git','-C',S,'rev-parse','HEAD'],text=True).strip()=='61dde3727921e70e2c72fbd3c9e2044a192f4a54'
assert not subprocess.check_output(['git','-C',S,'status','--porcelain'])
assert not subprocess.check_output(['git','diff','97d495fcb6c319111429073dedffb544c9355a1a','--','kernel'],cwd=R)
st=os.statvfs(R);free=st.f_bavail*st.f_frsize//1024**2;assert free>=8192
raw={str(p.relative_to(R)):sha(p) for p in D.rglob('*') if p.is_file() and p.suffix in ['.log','.c','.h','.py','.json','.diff','.d'] and p.name!='verified-sources.json'}
result=dict(owner='astra',input_hashes=len(h['inputs']),raw_pairs=len(h['results']),arm64_objects=len(a),model_results=len(m['results']),checks=3674,mutants=13,free_mib=free,astra_preserved=10,stratum='clean61dde37',raw_sha256=raw)
(D/'verified-sources.json').write_text(json.dumps(result,indent=2)+'\n');print({k:v for k,v in result.items() if k!='raw_sha256'})
