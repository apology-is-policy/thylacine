"""Verify all raw serial/harness logs, pinned binaries and source for the matrix."""
from pathlib import Path
import sys,json,re,hashlib,datetime,subprocess,shutil
R=Path.cwd();D=Path(sys.argv[1]).resolve()
def sha(p):
 with p.open('rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
source=(D/'source.txt').read_text().strip();assert subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()==source
assert not subprocess.check_output(['git','diff','HEAD','--'])
start=datetime.datetime.fromisoformat((D/'start.txt').read_text().strip()).timestamp()
rows=json.loads((D/'rows.json').read_text());assert len(rows)==5
assert json.loads((D/'row-results.json').read_text())==[r[0] for r in rows]
expected=len(re.findall(r'^\s*\{\s*"[^"]+"',(R/'kernel/test/test.c').read_text(),re.M));assert expected==1855
inputs=json.loads((D/'inputs.json').read_text());assert len(inputs)==8
for p,v in inputs.items():
 assert sha(R/p)==v,p
 copy=D/'inputs'/('thylacine-undefined.'+p.rsplit('.',1)[1] if 'kernel-undefined' in p else Path(p).name)
 assert sha(copy)==v,p
names=[]
with (D/'inputs/ramfs.cpio').open('rb') as f:
 while True:
  hdr=f.read(110);assert hdr[:6]==b'070701';v=[int(hdr[6+i*8:14+i*8],16) for i in range(13)]
  name=f.read(v[11]).rstrip(b'\0').decode();names.append(name);f.seek((-f.tell())%4,1)
  if name=='TRAILER!!!':break
  f.seek(v[6],1);f.seek((-f.tell())%4,1)
optional=['thylacine-venus-prove','thylacine-vk-sdl-prove'];assert not any(Path(n).name in optional for n in names)
allowed={f'joey: spawn {n} FAILED (absent from this image, or refused)' for n in optional}
fixtures=['loom.private_owner_lifecycle','loom.service_slots','loom.private_slot_owner','loom.service_pool_table','loom.private_buffers','loom.private_pool_pins','loom.private_fixed','loom.private_targets','loom.private_node_owner','loom.service_nodes','loom.service_requests','loom.private_requests','loom.private_protocol','loom.private_connect','loom.private_engine','loom.private_close','loom.private_control','loom.private_executor','loom.private_operation_storage','loom.private_frontend']
records=[];out=D/'verified-logs';out.mkdir(exist_ok=True)
for label,cpus,san,timeout in rows:
 matrix=(D/(label+'-matrix.log')).read_text();assert f'== {label}: 10 PASS / 0 CORRUPTION / 0 external-kill / 0 inject-miss / 0 timing / 0 other  (N=10) ==' in matrix
 assert len(list((R/'build/multiboot-logs').glob(label+'-*.log')))==20
 for i in range(1,11):
  p=R/'build/multiboot-logs'/f'{label}-{i}.log';h=p.with_name(f'{label}-{i}-harness.log')
  assert p.stat().st_mtime>=start and h.stat().st_mtime>=start
  s=p.read_text(errors='replace').replace('\r','');hs=h.read_text(errors='replace')
  assert f'tests: {expected}/{expected} PASS' in s and 'EXTINCTION:' not in s
  failures=[line for line in s.splitlines() if re.search(r'\bFAIL(?:ED)?\b',line)];assert set(failures)==allowed and len(failures)==2,(label,i,failures)
  for fixture in fixtures:
   a=s.index('[test] '+fixture);b=s.find('[test]',a+6);block=s[a:b if b>=0 else None];assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,fixture,block)
  for marker in ['D7 three distinct login sessions simultaneously ready','D7 overlapping login probe PASS','D7 twenty distinct login/logout cycles PASS','Thylacine boot OK']:assert marker in s,(label,i,marker)
  cycles=re.findall(r'joey: /sbin/login E2E OK for (d7user[0-9]{2}) ',s);assert cycles==[f'd7user{n:02}' for n in range(20)]
  assert 'PASS: boot banner observed.' in hs
  digests={}
  for f in [p,h]:
   target=out/f.name
   if target.exists():assert target.read_bytes()==f.read_bytes()
   else:shutil.copy2(f,target)
   digests[f.name]=sha(target)
  records.append(dict(label=label,boot=i,tests=expected,fixtures=fixtures,d7_cycles=cycles,sha256=digests))
flight=json.loads((R/'work/oct5-as-r9/astra-takeover/async-integration-matrix-flight.json').read_text());assert flight['source']==source and flight['minimum_free_mib']>=8192
S=Path('/Users/northkillpd/projects/stratum-astra/v2');assert subprocess.check_output(['git','-C',str(S),'rev-parse','HEAD'],text=True).strip()=='61dde3727921e70e2c72fbd3c9e2044a192f4a54';assert not subprocess.check_output(['git','-C',str(S),'status','--porcelain'])
A=Path('/Users/northkillpd/projects/thylacine-astra');a=json.loads((A/'work/oct6-modal-caret/pending.json').read_text())
for group in ['paths','protected']:
 for p,v in a[group].items():assert sha(A/p)==v,p
result=dict(owner='astra',source=source,clean_boots=50,expected_tests=1855,rows=rows,inputs_sha256=inputs,minimum_free_mib=flight['minimum_free_mib'],records=records,limits=['Matrix stage with rebuilt default/UBSan kernels and pinned native-qualified image inputs; not a completed ci-smp-gate whole-image rebake.','Mac HVF only, no Pi/A72/KVM qualification.','Internal CONNECT frontend and existing storage/close probes; native general operations/provided/MORE qualification still owed.','Private public ENTER stays gated, no Main/clipboard activation or128MiB removal.','Single-agent self-review, not independent audit.'])
(D/'verified-result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps({k:v for k,v in result.items() if k not in ['records','inputs_sha256']},indent=2))
