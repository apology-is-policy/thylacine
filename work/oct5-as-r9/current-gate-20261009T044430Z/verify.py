from pathlib import Path
import re,json,hashlib,datetime,shutil
root=Path.cwd(); run=root/'work/oct5-as-r9/current-gate-20261009T044430Z'
start=datetime.datetime.fromisoformat((run/'start.txt').read_text().strip().replace('Z','+00:00')).timestamp()
source=(run/'source.txt').read_text().strip()
assert source=='262ff29d3fe306dd700481f36d1fa4dd40aaede0'
assert (run/'end-source.txt').read_text().strip()==source
assert (run/'end-source.diff').stat().st_size==0
for f in ['gate.rc','runner.rc']:assert (run/f).read_text().strip()=='0'
log=(run/'gate.log').read_text(); rows=['default-smp1','default-smp4','default-smp8','ubsan-smp4','ubsan-smp8']
expected=len(re.findall(r'^\s*\{\s*"[^"]+"',(root/'kernel/test/test.c').read_text(),re.M))
assert expected==1844
# Nonfatal graphics diagnostics are not kernel test failures. Prove these
# optional programs absent from this exact ramfs, not merely from staging.
names=[]
with (run/'inputs/ramfs.cpio').open('rb') as f:
 while True:
  hdr=f.read(110); assert hdr[:6]==b'070701'
  v=[int(hdr[6+i*8:14+i*8],16) for i in range(13)]
  name=f.read(v[11]).rstrip(b'\0').decode();names.append(name)
  f.seek((-f.tell())%4,1)
  if name=='TRAILER!!!':break
  f.seek(v[6],1);f.seek((-f.tell())%4,1)
optional=['thylacine-venus-prove','thylacine-vk-sdl-prove']
assert not any(Path(n).name in optional for n in names)
allowed={f'joey: spawn {n} FAILED (absent from this image, or refused)' for n in optional}
out=run/'verified-logs';out.mkdir(exist_ok=True)
records=[]
for label in rows:
 assert f'== {label}: 10 PASS / 0 CORRUPTION / 0 external-kill / 0 inject-miss / 0 timing / 0 other  (N=10) ==' in log
 assert len(list((root/'build/multiboot-logs').glob(label+'-*.log')))==20
 for i in range(1,11):
  f=root/'build/multiboot-logs'/f'{label}-{i}.log';h=f.with_name(f'{label}-{i}-harness.log')
  assert f.stat().st_mtime>=start and h.stat().st_mtime>=start
  s=f.read_bytes().decode('utf8',errors='replace').replace('\r','');hs=h.read_text(errors='replace')
  assert f'tests: {expected}/{expected} PASS' in s
  assert 'EXTINCTION:' not in s
  failures=[line for line in s.splitlines() if re.search(r'\bFAIL(?:ED)?\b',line)]
  assert set(failures)==allowed and len(failures)==2,(label,i,failures)
  a=s.index('[test] loom.private_owner_lifecycle');b=s.index('[test]',a+6);block=s[a:b]
  assert '[lp-mark] cleanup-owner-drop normal-fallthrough' in block
  assert re.search(r'^PASS$',block,re.M)
  a=s.index('[test] loom.service_slots');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  a=s.index('[test] loom.private_slot_owner');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  a=s.index('[test] loom.service_pool_table');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  a=s.index('[test] loom.private_buffers');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  a=s.index('[test] loom.private_pool_pins');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  a=s.index('[test] loom.private_fixed');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  a=s.index('[test] loom.private_targets');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  a=s.index('[test] loom.service_nodes');b=s.index('[test]',a+6);block=s[a:b]
  assert re.search(r'PASS\s*$',block) and 'FAIL' not in block,(label,i,block)
  for marker in ['D7 three distinct login sessions simultaneously ready','D7 overlapping login probe PASS','D7 twenty distinct login/logout cycles PASS','Thylacine boot OK']:
   assert marker in s,(label,i,marker)
  cycles=re.findall(r'joey: /sbin/login E2E OK for (d7user[0-9]{2}) ',s)
  assert cycles==[f'd7user{n:02}' for n in range(20)],(label,i,cycles)
  assert 'PASS: boot banner observed.' in hs
  hashes={}
  for file in [f,h]:
   target=out/file.name
   if target.exists():assert target.read_bytes()==file.read_bytes()
   else:shutil.copy2(file,target)
   hashes[file.name]=hashlib.sha256(target.read_bytes()).hexdigest()
  records.append({'label':label,'boot':i,'tests':expected,'private_owner_normal_fallthrough':True,'service_pool_table':True,'private_buffers':True,'private_pool_pins':True,'private_fixed':True,'private_targets':True,'service_nodes':True,'service_slots':True,'private_slot_owner':True,'d7_overlap':True,'d7_twenty_cycles':True,'distinct_cycle_users':cycles,'boot_banner':True,'optional_graphics_absent':optional,'kernel_skip_lines':len(re.findall(r'\[skip\]',s)),'sha256':hashes})
inputs={}
for line in (run/'inputs/HASHES.txt').read_text().splitlines():
 digest,name=line.split(None,1);file=run/'inputs'/name
 assert hashlib.sha256(file.read_bytes()).hexdigest()==digest
 inputs[name]=digest
assert len(inputs)==10
floor=[int(x) for x in re.findall(r'disk [^\n]*?: (\d+) MiB free, floor 8192',log)]
assert floor and min(floor)>=8192
result={'source':source,'run':str(run.relative_to(root)),'gate_rc':0,'runner_rc':0,'clean_boots':50,'expected_tests':expected,'rows':rows,'d7_overlap_boots':50,'d7_twenty_cycle_boots':50,'private_owner_boots':50,'service_pool_table_boots':50,'private_buffers_boots':50,'private_pool_pins_boots':50,'private_fixed_boots':50,'private_targets_boots':50,'service_nodes_boots':50,'service_slots_boots':50,'private_slot_owner_boots':50,'minimum_recorded_admission_free_mib':min(floor),'stratum':(run/'stratum.txt').read_text().strip(),'inputs_sha256':inputs,'gate_log_sha256':hashlib.sha256((run/'gate.log').read_bytes()).hexdigest(),'verification_script_sha256':hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),'records':records,'limits':['Mac HVF only; Pi/A72/KVM not run','guest allocator reclamation remains open; bounded host destruction-call controls are separate','internal native targets, fixed reservations, pool member pins and buffer pin/charge fixtures covered; scope/fid metadata fixture covered with simulated protocol states; charged node ownership and unused cleanup covered; protocol resources, request/CQ admission, usercopy authority and worker integration pending','optional Venus/Vulkan graphics probes absent from captured ramfs, no graphics qualification','self-review, not independent audit','no Main integration or private async/clipboard activation; 128 MiB protection retained']}
p=root/'work/oct5-as-r9/astra-takeover/current-gate-20261009T044430Z-result.json';p.write_text(json.dumps(result,indent=2)+'\n')
(run/'inputs/PINNED').write_text('Current-source full Mac gate, 50 clean boots. See sibling logs and takeover result JSON. Preserve this complete generation.\n')
print(json.dumps({k:result[k] for k in ['source','clean_boots','expected_tests','d7_overlap_boots','d7_twenty_cycle_boots','private_owner_boots','minimum_recorded_admission_free_mib']}))
print('100 serial/harness logs retained; 10 input hashes verified.')
