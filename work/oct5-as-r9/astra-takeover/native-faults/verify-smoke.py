from pathlib import Path
import hashlib,json,re,subprocess,sys
run=Path(sys.argv[1]);rows=[('default1',1,''),('default4',4,''),('default8',8,''),('ubsan4',4,'undefined'),('ubsan8',8,'undefined')]
def sha(p):
 with open(p,'rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
assert (run/'runner.rc').read_text().strip()=='0'
proof=[]
for label,cpus,san in rows:
 d=run/label;r=json.loads((d/'result.json').read_text());s=(d/'serial.log').read_text().replace('\r','')
 assert r['rc']==0 and r['cpus']==cpus and r['sanitize']==san and not r['floor_breach'] and not r['guard_stopped'] and not r['interrupted_signal']
 assert r['minimum_free_mib']>8256
 assert 'tests: 1857/1857 PASS' in s and f'smp:  {cpus}/{cpus} cpus online' in s
 for fixture in ['loom.private_native_operations','loom.private_native_faults']:
  a=s.index('[test] '+fixture);b=s.find('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,fixture
 assert 'Thylacine boot OK' in s and 'D7 overlapping login probe PASS' in s
 assert re.findall(r'joey: /sbin/login E2E OK for (d7user[0-9]{2}) ',s)==[f'd7user{i:02}' for i in range(20)]
 assert sha(d/'serial.log')==r['serial_sha256'] and sha(d/'harness.log')==r['harness_sha256']
 assert sha(d/'source.diff')==r['source_diff_sha256']
 assert (d/'source.txt').read_text().strip()==r['source_head']
 assert r['source_stable'] and r['extra_stable'] and r['inputs_stable']
 assert all(sha(p)==h for p,h in r['extra_sources'].items())
 assert all(sha(p)==h for p,h in r['inputs'].items())
 for suffix in ['elf','bin']:
  key=next(p for p in r['inputs'] if p.endswith('thylacine.'+suffix))
  assert sha(d/('thylacine.'+suffix))==r['inputs'][key]
 proof.append(dict(row=label,source=r['source_head'],diff=r['source_diff_sha256'],serial=r['serial_sha256'],harness=r['harness_sha256'],minimum_free_mib=r['minimum_free_mib']))
assert len({p['source'] for p in proof})==1 and len({p['diff'] for p in proof})==1
assert proof[0]['source']==subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
assert proof[0]['diff']==hashlib.sha256(subprocess.check_output(['git','diff','HEAD','--binary'])).hexdigest()
stratum='/Users/northkillpd/projects/stratum-astra/v2'
assert subprocess.check_output(['git','-C',stratum,'rev-parse','HEAD'],text=True).strip()=='61dde3727921e70e2c72fbd3c9e2044a192f4a54'
assert not subprocess.check_output(['git','-C',stratum,'status','--porcelain'])
astra=Path('/Users/northkillpd/projects/thylacine-astra');m=json.loads((astra/'work/oct6-modal-caret/pending.json').read_text());assert all(sha(astra/p)==h for p,h in {**m['paths'],**m['protected']}.items())
result=dict(owner='astra',rows=proof,raw_logs=10,tests=1857,schedules_per_class=6,classes=2,protected_files=10,stratum_clean=True,review='single-agent',limit='Five one-boot smoke rows; not50boot qualification or complete AS3')
(run/'reverified.json').write_text(json.dumps(result,indent=2)+'\n');print('Verified five rows,10rawlogs,1857tests,six READ fault schedules per class,source/artifact/protected hashes')
