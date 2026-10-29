from pathlib import Path
import subprocess,json,hashlib,datetime,re,os,difflib,importlib.util
D=Path(__file__).resolve().parent;R=D.parents[3];T=D.parent
W=T/'wire-draft';P=T/'program-draft'
spec=importlib.util.spec_from_file_location('extract',R/'work/oct5-as-r9/final-reclaim-control.py');extract=importlib.util.module_from_spec(spec);spec.loader.exec_module(extract)
M=[
 ('creator-image','io','p->as!=l->service_as || ','','exact creator image refusal'),
 ('creator-incarnation','io',' || p->stripes!=l->service_creator','','exact creator incarnation refusal'),
 ('canonical-offset','io','+e.offset;',';','prepared canonical slice owns independent pin'),
 ('component-copy','io','o->program.wire.component[i]=view[i];','o->program.wire.component[i]=view[i]&0;','walk owns copied component and burns wire fid'),
 ('component-validate','io','rc=loom_private_walk_component(o->program.wire.component,s->len);','rc=0;','invalid component unwinds before fid allocation'),
 ('empty-offset','io','offset>b->len','false','empty extent still bounds offset'),
 ('empty-index','io','index>=l->n_reg_buf','false','empty extent still validates registration'),
 ('fid-exhaustion','io','if(o->program.wire.new_fid==P9_NOFID)','if(false)','exhausted monotone allocator unwinds'),
 ('payload-pointer-clear','io','o->program.wire.payload=NULL;','/* retain stale pointer */','physical payload release clears pointer'),
 ('payload-capacity-clear','io','o->program.wire.payload_capacity=0;','/* retain stale capacity */','physical payload release clears pointer'),
 ('queued-drop','io','o->phase!=LRPC_NEW && o->phase!=LRPC_SETTLING','o->phase!=LRPC_NEW && o->phase!=LRPC_SETTLING && o->phase!=LRPC_QUEUED','queued ownership cannot release payload'),
 ('drop-barrier','io','o->fixed_dropping=true;','o->fixed_dropping=false;','unlink is not physical settlement'),
 ('fixed-owner-free','operation','!op->fixed.owner && ','','storage free after physical holds'),
 ('backing-pin','fixed','            burrow_ref((struct Burrow *)e.backing);','            if(false)burrow_ref((struct Burrow *)e.backing);','prepared canonical slice owns independent pin'),
 ('fixed-ref','fixed','            loom_ref(l);','            if(false)loom_ref(l);','prepared canonical slice owns independent pin'),
 ('wire-fid-burn','allocator','    c->next_fid++;','    /* rewind bug */','walk owns copied component and burns wire fid'),
 ('fresh-publish-time','io','loom_admission_enqueue_locked(o,o->source,o->destination,now)','loom_admission_enqueue_locked(o,o->source,o->destination,0)','fresh publication deadline retains provisional hold'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def stripped(s):return re.sub(r'^#include[^\n]*\n','',s,flags=re.M)
def main():
 resources=subprocess.check_output([str(R/'work/oct5-as-r9/astra-yip.sh'),'resources'],text=True);assert re.search(r'^mac +HELD by you',resources,re.M)
 st=os.statvfs(R);assert st.f_bavail*st.f_frsize>=8*1024**3
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 modules=['operation','rpc','admission','io'];parts={n:stripped((D/f'kernel/loom_service_{n}.c').read_text()) for n in modules}
 parts['fixed']='\n'.join(extract.fun((R/'kernel/loom.c').read_text(),n) for n in ['loom_private_extent_locked','loom_private_fixed_acquire','loom_private_fixed_release'])
 parts['allocator']=extract.fun((R/'kernel/9p_client.c').read_text(),'p9_client_alloc_fid')
 linked=[W/'kernel/loom_service_wire.c',P/'kernel/loom_service_program.c']+[R/n for n in ['kernel/perm.c','kernel/9p_session.c','kernel/9p_wire.c','kernel/9p_transport.c','kernel/loom_service_reply.c','kernel/loom_service_requests.c','kernel/loom_service_slots.c','kernel/loom_service_nodes.c','kernel/loom_service_fids.c','kernel/loom_service_pool.c','kernel/loom_service_decode.c']]
 paths=[*D.glob('kernel/**/*.c'),*D.glob('kernel/**/*.h'),D/'fixture.c',D/'run-controls.py',P/'host/thylacine/spinlock.h',R/'kernel/loom.c',R/'kernel/9p_client.c',R/'work/oct5-as-r9/final-reclaim-control.py',*linked]
 hashes={str(p.relative_to(R)):sha(p) for p in paths};fixture=(D/'fixture.c').read_text();results=[]
 def assembled(parts):
  s=fixture
  for n,marker in [('operation','CONSTRUCTOR'),('rpc','RPC'),('admission','ADMISSION'),('io','IO'),('fixed','FIXED'),('allocator','FID_ALLOCATOR')]:s=s.replace('/* ACTUAL_'+marker+' */',parts[n])
  return s
 original=assembled(parts);sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
 for name,unit,old,new,want in [('control',None,None,None,None)]+M:
  variant=dict(parts)
  if unit:
   assert variant[unit].count(old)==1,(name,variant[unit].count(old));variant[unit]=variant[unit].replace(old,new)
  source=assembled(variant);p=out/(name+'.c');p.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True),fromfile='control.c',tofile=name+'.c')))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-g','-fsanitize=address,undefined','-isysroot',sdk,'-I'+str(P/'host'),'-I'+str(D/'kernel/include'),'-I'+str(P/'kernel/include'),'-I'+str(W/'kernel/include'),'-I'+str(R/'kernel/include'),str(p),*[str(p) for p in linked],'-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd)+'\n')
  build=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(build.stdout+build.stderr);assert not build.returncode,(name,build.stderr.decode())
  run=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(run.stdout+run.stderr)
  assert (run.returncode==1 and run.stderr==f'FAIL {want}\n'.encode()) if want else (run.returncode==0 and not run.stderr),(name,run.returncode,run.stdout,run.stderr)
  results.append(dict(case=name,expected=want,rc=run.returncode,source_sha256=sha(p),binary_sha256=sha(exe)));print('PASS',name,want or run.stdout.decode().strip(),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in paths}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,limits='Real data headers and actual SQE preparation, fixed helper, constructor, FID allocator and ledger/program code. Host address translation, allocation, Proc snapshot, refs/wake/engine and physical backing drop boundaries. No wire dispatch; admitted destructor is manual fixture cleanup. No native frontend/driver/close/concurrency proof.'),indent=2)+'\n');print(out)
if __name__=='__main__':main()
