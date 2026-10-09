from pathlib import Path
import hashlib,json,subprocess,datetime
R=Path('/Users/northkillpd/projects/thylacine-corona');D=Path(__file__).resolve().parent
h=lambda f:hashlib.sha256(f.read_bytes()).hexdigest()
assert json.loads((D.parent/'engine-current-flight.json').read_text())['state']=='verified'
assert not subprocess.check_output(['git','status','--porcelain','-uno'],cwd=R)
m=json.loads((D/'install-manifest.json').read_text())
for path,item in m['sources'].items():
 assert '__pycache__' not in path and Path(path).suffix!='.pyc'
 assert h(D/path)==item['draft_sha256'],path
 assert (h(R/path) if (R/path).exists() else None)==item['base_sha256'],path
assert not (D/'installed.json').exists(),'already installed: inspect before repeating'
for path in m['sources']:
 (R/path).parent.mkdir(parents=True,exist_ok=True);(R/path).write_bytes((D/path).read_bytes())
(D/'installed.json').write_text(json.dumps({'owner':'astra','utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'base_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'manifest_sha256':h(D/'install-manifest.json'),'paths':list(m['sources']),'state':'installed; native build/diagnostic pending'},indent=2)+'\n')
print('Installed',len(m['sources']),'reviewed source paths; no runtime qualification yet.')
