from pathlib import Path
import hashlib,subprocess,os,json,re,shutil
# Run only after reviewed FID coherent frontend/operation sources are installed and rebuilt; output is a fresh directory.
import sys
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
resources=subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
assert os.statvfs('.').f_bavail*os.statvfs('.').f_frsize>=8*1024**3
def digest(p):
 with open(p,'rb') as f:return hashlib.file_digest(f,'sha256').hexdigest()
paths=['build/kernel/thylacine.elf','build/kernel/thylacine.bin','build/ramfs.cpio','build/fixtures/pool.img.baked-snapshot','build/fixtures/system.key','build/fixtures/system.key.baked-snapshot']
inputs={p:digest(p) for p in paths}
head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
diff=subprocess.check_output(['git','diff','HEAD','--binary'])
(out/'source.txt').write_text(head+'\n');(out/'source.diff').write_bytes(diff)
manifest=json.loads((Path(__file__).resolve().parent/'verified-helpers.json').read_text())
extra={p:digest(p) for p in manifest['files']}
assert extra==manifest['files'],'installed source differs from reviewed manifest'
for suffix in ['elf','bin']:subprocess.run(['cp','-c','build/kernel/thylacine.'+suffix,str(out/('thylacine.'+suffix))],check=True)
with (out/'harness.log').open('wb') as f:
 r=subprocess.Popen(['tools/test.sh'],stdout=f,stderr=subprocess.STDOUT,env={**os.environ,'THYLACINE_TEST_CPUS':'1','BOOT_TIMEOUT':'300'},start_new_session=True)
 import time,signal
 minimum=10**12;floor_breach=False
 while r.poll() is None:
  fs=os.statvfs('.');minimum=min(minimum,fs.f_bavail*fs.f_frsize//1024**2)
  if minimum<8192:floor_breach=True;os.killpg(r.pid,signal.SIGTERM);break
  time.sleep(1)
 r.wait()
shutil.copy2('build/test-boot.log',out/'serial.log')
s=(out/'serial.log').read_text(errors='replace').replace('\r','')
cycles=re.findall(r'joey: /sbin/login E2E OK for (d7user[0-9]{2}) ',s)
a=s.find('[test] loom.private_frontend');b=s.find('[test]',a+6);block=s[a:b]
result={'minimum_free_mib':minimum,'floor_breach':floor_breach,'source_head':head,'source_diff_sha256':hashlib.sha256(diff).hexdigest(),'extra_sources':extra,'inputs':inputs,'rc':r.returncode,
'source_stable':diff==subprocess.check_output(['git','diff','HEAD','--binary']),
'inputs_stable':inputs=={p:digest(p) for p in paths},'extra_stable':extra=={p:digest(p) for p in extra},
'all1855':'tests: 1855/1855 PASS' in s,'frontend':a>=0 and bool(re.search(r'PASS\s*$',block)) and 'FAIL' not in block,
'boot_banner':'Thylacine boot OK' in s,'d7_overlap':'D7 overlapping login probe PASS' in s,'d7_cycles':cycles,
'serial_sha256':digest(out/'serial.log'),'harness_sha256':digest(out/'harness.log')}
(out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print(json.dumps(result,indent=2))
assert not floor_breach and r.returncode==0 and all(result[k] for k in ['source_stable','inputs_stable','extra_stable','all1855','frontend','boot_banner','d7_overlap']) and cycles==[f'd7user{i:02}' for i in range(20)]
