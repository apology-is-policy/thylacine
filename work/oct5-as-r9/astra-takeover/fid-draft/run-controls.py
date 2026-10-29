#!/usr/bin/env python3
"""Isolated actual-C FID state controls, not RPC or concurrent-engine qualification."""
import datetime,difflib,hashlib,json,subprocess
from pathlib import Path
D=Path(__file__).resolve().parent
R=D.parents[3]
M=[
 ('borrow-overflow','if(n->fid_borrows==UINT32_MAX)return -T_E_NOSPC;','', 'borrow saturation unchanged'),
 ('abort-borrow','if(n->fid_claim || n->fid_borrows)return -T_E_BUSY;\n    (void)loom_slot_retire','if(n->fid_claim)return -T_E_BUSY;\n    (void)loom_slot_retire','abort waits for source borrower'),
 ('low-incarnation','s->incarnation!=r.incarnation','(u32)s->incarnation!=(u32)r.incarnation','full incarnation commit required'),
 ('reserved-ref',' || r.reserved',' || false','reserved commit ref denied'),
 ('wrong-parent','fid_same(n->parent,scope);','true;','wrong parent claim denied'),
 ('ignore-abort','s->state==LOOM_SERVICE_READY && !s->reason','!s->reason','abort beats walk commit'),
 ('walk-owner','n->fid_claim!=id || n->fid_flags','false || n->fid_flags','wrong owner cannot publish'),
 ('root-alias','if(fid_duplicate(t,nodes,scope,wire))return -T_E_BUSY;\n    n->wire_fid=wire;n->fid_flags=LFID_BOUND;n->state','if(false)return -T_E_BUSY;\n    n->wire_fid=wire;n->fid_flags=LFID_BOUND;n->state','root alias denied'),
 ('drop-owner','if(!id || n->fid_claim!=id ||','if(!id || false ||','wrong owner cannot drop'),
 ('drop-pin','int rc=loom_slot_unpin(t,dest);','int rc=0;','walk physical release'),
 ('borrow-claim','if(n->fid_claim)return -T_E_BUSY;\n    if(n->fid_borrows','if(false)return -T_E_BUSY;\n    if(n->fid_borrows','publication retains physical claim'),
 ('borrow-access','(n->fid_access&need)!=need','false','write permission retained'),
 ('borrow-count','++n->fid_borrows;','(void)0;','metadata borrow tracked'),
 ('put-count','if(!rc)--n->fid_borrows;','if(!rc)(void)0;','borrow release exact'),
 ('open-borrow','if(n->fid_claim || n->fid_borrows)return -T_E_BUSY;\n    int rc=loom_slot_pin','if(n->fid_claim)return -T_E_BUSY;\n    int rc=loom_slot_pin','open waits for source borrowers'),
 ('open-owner','n->fid_claim!=id || n->fid_borrows || !t->slots[source.slot].pins','false || n->fid_borrows || !t->slots[source.slot].pins','open exact owner'),
 ('aggregate-root-pins','if(!(n->fid_flags&LFID_OPENING)','if(t->slots[source.slot].pins!=1 || !(n->fid_flags&LFID_OPENING)','root readwrite commit'),
 ('clunk-admission','(void)loom_slot_retire(t,source);\n    return 0;','(void)0;\n    return 0;','clunk closes admission with reader'),
 ('clunk-drain','!t->slots[source.slot].pins || n->fid_borrows)return -T_E_BUSY;','!t->slots[source.slot].pins)return -T_E_BUSY;','clunk waits for prior reader'),
 ('clunk-owner','n->fid_claim!=id || !t->slots[source.slot].pins','false || !t->slots[source.slot].pins','clunk exact owner'),
 ('early-claim-release','if((n->fid_flags&LFID_CLUNKING) && n->state!=LOOM_SERVICE_RETIRED)','if(false)','clunk owner held until quiescent'),
 ('retiring-alias','s->phase==LSLOT_LIVE || s->phase==LSLOT_RETIRING','s->phase==LSLOT_LIVE','retiring wire alias denied'),
 ('parent-leak','(void)loom_slot_unpin(t,parent);','(void)0;','reap drops parent and mapping'),
 ('abort-owner','if(n->fid_claim || n->fid_borrows)return -T_E_BUSY;\n    (void)loom_slot_retire','if(n->fid_borrows)return -T_E_BUSY;\n    (void)loom_slot_retire','abort waits for physical claim'),
]
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 out=D/('controls-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));out.mkdir()
 inputs=[D/'kernel/loom_service_fids.c',D/'kernel/include/thylacine/loom_service_fids.h',D/'kernel/loom_service_nodes.c',D/'kernel/include/thylacine/loom_service_nodes.h',D/'fixture.c',D/'run-controls.py',R/'kernel/loom_service_slots.c',R/'kernel/include/thylacine/loom_service_slots.h',R/'kernel/include/thylacine/loom_service_abi.h',R/'kernel/include/thylacine/types.h',R/'kernel/include/thylacine/errno.h']
 hashes={str(p.relative_to(R)):sha(p) for p in inputs};original=inputs[0].read_text();results=[]
 sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
 for name,old,new,expected in [('control',None,None,None)]+M:
  if old is not None:assert original.count(old)==1,(name,original.count(old))
  source=original if old is None else original.replace(old,new)
  c=out/(name+'.c');c.write_text(source);exe=out/(name+'.exe')
  (out/(name+'.diff')).write_text(''.join(difflib.unified_diff(original.splitlines(True),source.splitlines(True))))
  cmd=['/opt/homebrew/opt/llvm@22/bin/clang','-isysroot',sdk,'-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter','-I'+str(D/'kernel/include'),'-I'+str(R/'kernel/include'),str(c),str(D/'kernel/loom_service_nodes.c'),str(R/'kernel/loom_service_slots.c'),str(D/'fixture.c'),'-fsanitize=address,undefined','-fno-omit-frame-pointer','-g','-o',str(exe)]
  (out/(name+'-command.json')).write_text(json.dumps(cmd))
  r=subprocess.run(cmd,capture_output=True);(out/(name+'-build.log')).write_bytes(r.stdout+r.stderr);assert r.returncode==0,(name,r.stderr.decode())
  r=subprocess.run([str(exe)],capture_output=True,timeout=20);(out/(name+'.log')).write_bytes(r.stdout+r.stderr)
  assert ((r.returncode==1 and r.stderr.decode()==f'FAIL {expected}\n') if expected else (r.returncode==0 and not r.stderr)),(name,r.returncode,r.stdout,r.stderr)
  results.append(dict(case=name,expected=expected,rc=r.returncode,source_sha256=sha(c),binary_sha256=sha(exe)))
  print('PASS',name,(expected or r.stdout.decode().strip()),flush=True)
 assert hashes=={str(p.relative_to(R)):sha(p) for p in inputs}
 (out/'result.json').write_text(json.dumps(dict(inputs=hashes,results=results,sanitized=True,scope='Sequential helper only. No real RPC, DAC, allocator, engine lock or concurrency proof.'),indent=2)+'\n')
 print(out)
if __name__=='__main__':main()
