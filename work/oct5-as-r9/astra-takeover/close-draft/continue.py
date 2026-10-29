from pathlib import Path
import json,subprocess,os,time,datetime,hashlib
R=Path('/Users/northkillpd/projects/thylacine-corona');os.chdir(R);D=Path(__file__).resolve().parent;T=D.parent
state=D/'continuation.json'
assert not state.exists(),'A close continuation already exists; inspect it, do not duplicate.'
s={'owner':'astra','pid':os.getpid(),'phase':'waiting for exact owner-index matrix','started_utc':datetime.datetime.now(datetime.timezone.utc).isoformat()}
def save(phase):
 s['phase']=phase;s['updated_utc']=datetime.datetime.now(datetime.timezone.utc).isoformat();state.write_text(json.dumps(s,indent=2)+'\n');print(phase,flush=True)
def run(cmd,log):
 with (D/log).open('wb') as f:subprocess.run(cmd,stdout=f,stderr=subprocess.STDOUT,check=True)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def preserve():
 a=Path('/Users/northkillpd/projects/thylacine-astra');m=json.loads((a/'work/oct6-modal-caret/pending.json').read_text())
 for k in ['paths','protected']:
  for p,d in m[k].items():assert sha(a/p)==d,p
 st=Path('/Users/northkillpd/projects/stratum-astra/v2')
 assert subprocess.check_output(['git','rev-parse','HEAD'],cwd=st,text=True).strip()=='61dde3727921e70e2c72fbd3c9e2044a192f4a54'
 assert not subprocess.check_output(['git','status','--porcelain'],cwd=st)
 assert os.statvfs(R).f_bavail*os.statvfs(R).f_frsize>=8*1024**3
try:
 save(s['phase'])
 while True:
  flight=json.loads((T/'engine-current-flight.json').read_text())
  if flight['state']=='failed':raise RuntimeError('owner-index matrix failed; no installation')
  if flight['state']=='verified':break
  time.sleep(5)
 gate=R/flight['run']
 while not (gate/'release.txt').exists():time.sleep(1)
 assert (gate/'runner.rc').read_text().strip()=='0'
 assert not subprocess.check_output(['git','status','--porcelain','-uno'])
 preserve()
 result=T/'current-gate-20261009T175812Z-result.json';v=json.loads(result.read_text())
 assert v['source']=='a978d19c388f512a645cdcbaeea60a81a0a06727' and v['clean_boots']==50 and v['expected_tests']==1850
 for row in v['records']:
  assert row['private_engine'] and row['tests']==1850
  for name,digest in row['sha256'].items():assert sha(gate/'verified-logs'/name)==digest
 save('verified 50 raw owner-index boots; recording evidence checkpoint')
 note=f'''\n\n## October 9: full owner-index Mac gate verified\n\nSource {v['source']} passed50/50 Mac HVF boots in\ncurrent-gate-20261009T175812Z: default SMP1/4/8 and kernel UBSan SMP4/8,\nten each. Every boot passed1850/1850 including loom.private_engine, D7\noverlap, twenty distinct login/logout cycles and the banner. Both rc0;\n100 serial/harness logs and ten input hashes verified and retained/PINNED.\nMinimum recorded admission {v['minimum_recorded_admission_free_mib']}MiB, above8192MiB.\nResult: work/oct5-as-r9/astra-takeover/current-gate-20261009T175812Z-result.json.\n\nThis qualifies the internal charged index and explicit progress at that source.\nThe separate close queue draft still owes native and full-matrix verification.\nSelf-review only; no independent audit, Main landing, private async/clipboard\nactivation or128MiB protection change. Continue close queue, active executor,\nremaining private ops/provided buffers, AS-3/4 and MM0-MM4.\n'''
 notes=['docs/ASYNC-SERVICE-STATUS.md','docs/JOURNAL.md','vault/system/kernel/async/sub-kernel-loom.md','work/oct5-as-r9/pickup.md']
 for p in notes:
  with (R/p).open('a') as f:f.write(note)
 pickup=R/'work/oct5-as-r9/astra-takeover/pickup-latest.md'
 pickup.write_text('''# Astra GUI continuation\n\nSole active GUI thread01a121cd-4ad5-73d1-bb8f-21a4b799519e; no handoff.\nImplementation corona/async-memory in thylacine-corona; Yip Astra through\nwork/oct5-as-r9/astra-yip.sh (internally runs in thylacine-astra).\n\nOwner-index a978d19c3 passed full50/50 Mac matrix,1850/1850+D7 every boot.\nRaw current-gate-20261009T175812Z source/inputs/logs verified/PINNED.\nClose integration prepared separately and host-qualified; its latest state is\nastra-takeover/close-draft/continuation.json and status.json. Do not duplicate\nthe running continuation or rerun build-draft.py / historical installers.\nClose native/full matrix still pending until their source-matched results exist.\nThen active readiness executor, query/reap, remaining ops/buffers, AS-3/4/MM0-MM4.\n\nKeep8192MiB floor, exact clean Stratum61dde37, Astra six pending/four protected\nfiles. No Main/private async/clipboard activation/128MiB change. Self-review only.\nWorking set: AGENTS.md; docs/ASYNC-SERVICE-STATUS.md latest; close-draft/\ncontinuation.json, status.json, self-review.txt, install-manifest.json.\n''')
 selected=[str(gate.relative_to(R)/p.name) for p in gate.iterdir() if p.is_file()]
 selected += [str(p.relative_to(R)) for p in (gate/'verified-logs').iterdir() if p.is_file()]
 selected += [str((gate/'inputs'/n).relative_to(R)) for n in ['.config-default','.config-undefined','HASHES.txt','PINNED']]
 selected += notes+[str(pickup.relative_to(R)),str(result.relative_to(R)),'work/oct5-as-r9/astra-takeover/engine-current-flight.json']
 subprocess.run(['git','add','-f','--',*selected],check=True)
 run(['git','commit','-m','Record full Mac qualification for indexed CONNECT owners'],'index-evidence-commit.log')
 s['index_evidence_commit']=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip()
 save('acquiring Astra Mac for close native verification')
 run(['work/oct5-as-r9/astra-yip.sh','hold','mac','Astra close lifecycle native verification','--for','2h'],'native-lease.log')
 lease=subprocess.check_output(['work/oct5-as-r9/astra-yip.sh','resources'],text=True)
 assert __import__('re').search(r'^mac +HELD by you',lease,__import__('re').M),lease
 preserve()
 run(['python3',str(D/'install.py')],'install.log');save('reviewed close integration installed; building native image')
 text=(D/'documentation.txt').read_text()+'\nNative execution pending; draft ASan/UBSan controls and ARM64 syntax pass.\n'
 for p in ['docs/ASYNC-SERVICE-STATUS.md','vault/system/kernel/async/sub-kernel-loom.md']:
  with (R/p).open('a') as f:f.write('\n\n'+text)
 vault=R/'vault/system/kernel/async/sub-kernel-loom.md';txt=vault.read_text().replace('  - kernel/loom.c\n','  - kernel/loom.c\n  - kernel/loom_service_close_queue.c\n  - kernel/include/thylacine/loom_service_close_queue.h\n');vault.write_text(txt)
 run(['go','-C','vault/meta/quaestor','run','.','render','--root',str(R)],'vault-render.log')
 run(['cmake','--build','build/kernel','--parallel','4'],'native-build.log')
 run(['tools/regen-halls-symtab.sh','build/kernel'],'native-halls.log')
 diagnostic=T/('close-diagnostic-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));s['diagnostic']=str(diagnostic.relative_to(R));save('running close native diagnostic on rebuilt image')
 run(['python3',str(D/'native-diagnostic.py'),str(diagnostic)],'native-diagnostic.log')
 save('native diagnostic verified; close checkpoint and full matrix next')
except Exception as e:
 s['failure']=repr(e);save('failed; preserve evidence and diagnose before retry');raise
