#!/usr/bin/env python3
"""Actual clipboard store/broker and async exchange schedules, plus named mutants.

Compiles production pure modules directly. No guest syscall behavior is inferred.
The sole layout boundary supplies the production MAX_PANES constant.
"""
from pathlib import Path
import argparse, json, re, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path);a=p.parse_args()
out=(a.out or Path(tempfile.mkdtemp(prefix='clip-admission-'))).resolve();out.mkdir(parents=True,exist_ok=True)
layout=(root/'usr/lib/libhalcyon/src/layout.rs').read_text()
bound=re.search(r'pub const MAX_PANES: usize = ([0-9]+);',layout);assert bound
paths={
 'interaction_wire':'usr/lib/libhalcyon/src/interaction_wire.rs',
 'interaction_body':'usr/lib/libhalcyon/src/interaction_body.rs',
 'interaction_control':'usr/lib/libhalcyon/src/interaction_control.rs',
 'clipboard':'usr/halcyond/src/clipboard.rs',
 'clipbroker':'usr/halcyond/src/clipbroker.rs',
 'admission':'usr/lib/libtapestry/src/admission.rs',
}
fixture='extern crate alloc;\nextern crate self as libhalcyon;\npub mod layout {pub const MAX_PANES:usize='+bound[1]+';}\n'
fixture+='\n'.join(f'#[path="{root/path}"] pub mod {name};' for name,path in paths.items())
rows=[]
def run(name,source,selected=None):
 src=out/(name+'.rs');src.write_text(source);exe=out/(name+'-test')
 r=subprocess.run(['rustc','--edition=2021','--test',str(src),'-o',str(exe)],text=True,capture_output=True)
 (out/(name+'-compile.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,name+' compile failed'
 r=subprocess.run([str(exe)]+([selected,'--exact'] if selected else []),text=True,capture_output=True)
 (out/(name+'.log')).write_text(r.stdout+r.stderr)
 if selected:assert r.returncode!=0 and selected+' ... FAILED' in r.stdout and '1 failed' in r.stdout,name+' missing intended failure'
 else:assert r.returncode==0,name+' baseline failed'
 rows.append({'name':name,'exit':r.returncode,'intended_failure':bool(selected)})
run('clean',fixture)
for name,module,old,new,selected in [
 ('pending-id','clipbroker','if self.pending.as_ref()?.check.request != id {','if false {','delayed_mismatched_duplicate_and_reused_fid_replies'),
 ('focus-boundary','clipbroker','r.focus >= lost','false','focus_loss_after_admission_preserves_commit_but_cannot_admit_after_return'),
 ('seat-receipt','clipbroker','|| r.seat != p.seat','|| false','receipt_must_match_operation_foreground_and_seat'),
 ('foreground-receipt','clipbroker','|| r.foreground != p.authority.foreground','|| false','receipt_must_match_operation_foreground_and_seat'),
 ('owner-revocation','clipbroker','self.store.drop_owner(owner);','// deliberately omitted','admitted_reads_finish_after_focus_but_not_controller_loss'),
 ('completion-tag','admission','if s.tag != tag {','if false {','delayed_partial_and_duplicate_completions'),
 ('request-reuse','admission','request.request <= self.last_request','false','delayed_partial_and_duplicate_completions'),
]:
 source=(root/paths[module]).read_text();assert source.count(old)==1,(name,old)
 mutated=out/(name+'-body.rs');mutated.write_text(source.replace(old,new,1))
 run(name,fixture.replace(str(root/paths[module]),str(mutated)),module+'::tests::'+selected)
(out/'results.json').write_text(json.dumps(rows,indent=2)+'\n')
print('PASS: actual broker/exchange schedules and seven intended counterexamples; evidence:',out)
