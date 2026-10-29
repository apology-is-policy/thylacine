from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent;O=T/'retirement-draft';P=T/'program-draft';W=T/'wire-draft'
M=[
('deadline-priority','(pick->action==LREADY_NONE ||\n           (action==LREADY_QUIESCE && pick->action!=LREADY_QUIESCE))','pick->action==LREADY_NONE','expired later owner preempts earlier submission'),
('deadline','if(active && r->deadline && (!a.deadline || r->deadline<a.deadline))','if(active && r->deadline && !a.deadline)','earliest deadline scans past first runnable owner'),
('rotation','if(o && o!=p->op_tail) {','if(false && o && o!=p->op_tail) {','round robin rotates every admitted owner'),
('driver-cursor','|| p->driving || p->users)', '|| p->users)', 'interest never reads live driver cursors'),
('tx-shortage','&& !p->progress.io.tx &&','&& true &&','occupied TX parks queued owners on native readiness'),
('tag-shortage','p9_session_has_free_tag(&p->client.session);','true;','full tag pool waits for receive without submit spin'),
('pool-incarnation','c->pool.incarnation==o->pool.incarnation &&','true &&','old pool incarnation cannot wake stream'),
('busy-retry','if(!o->pool_stream || o->pool_busy)return true;','if(!o->pool_stream)return true;','local no-wire retry keeps busy member eligible'),
('lease-exhaust','if(b->next_nonce==UINT64_MAX)return true;','if(false)return true;','exhausted leases are actionable without available member'),
('clunk-drain','if(!clunk_wait)action=LREADY_SUBMIT;','(void)clunk_wait;action=LREADY_SUBMIT;','CLUNK drain cannot block unrelated owner'),
('early-reply','o->callback_seen && !o->rpc.sending)','o->callback_seen)','early reply still waits for TX owner'),
('terminal-deadline','bool active=o->phase!=LRPC_TERMINAL;','bool active=true;','retired pending owners park without expired-deadline spin'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
src=(D/'kernel/loom_service_ready.c').read_text()
for name,old,new,want in M:assert src.count(old)==1,(name,src.count(old))
out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
inputs=[D/'kernel/loom_service_ready.c',D/'kernel/include/thylacine/loom_service_ready.h',D/'fixture.c',D/'run-controls.py',R/'kernel/9p_session.c',R/'kernel/9p_wire.c',P/'host/thylacine/spinlock.h',*O.glob('kernel/include/**/*.h')]
hashes={str(p.relative_to(R)):sha(p) for p in inputs};results=[];sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
for name,old,new,want in [('control',None,None,None)]+M:
 s=src.replace(old,new) if old else src;p=out/(name+'.c');p.write_text(s)
 (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(src.splitlines(True),s.splitlines(True))))
 exe=out/(name+'.exe');cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined','-isysroot',sdk,*['-I'+str(x) for x in [P/'host',D/'kernel/include',O/'kernel/include',P/'kernel/include',W/'kernel/include',R/'kernel/include']],str(p),str(D/'fixture.c'),str(R/'kernel/9p_session.c'),str(R/'kernel/9p_wire.c'),'-o',str(exe)]
 (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n');b=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(b.stdout+b.stderr);assert not b.returncode,(name,b.stderr.decode())
 r=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(r.stdout+r.stderr)
 assert (r.returncode==1 and r.stderr==f'FAIL {want}\n'.encode()) if want else (not r.returncode and not r.stderr),(name,r.returncode,r.stdout,r.stderr)
 results.append(dict(case=name,expected=want,rc=r.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,r.stdout.decode().strip() if not want else want,flush=True)
assert hashes=={str(p.relative_to(R)):sha(p) for p in inputs}
(out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results),indent=2)+'\n');print(out)
