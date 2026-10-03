#!/usr/bin/env python3
"""Actual HIN1 transaction owner: clean fixtures and named mutation failures."""
from pathlib import Path
import re, subprocess, tempfile
root=Path(__file__).resolve().parent.parent
modules={
 'interaction_wire':'usr/lib/libhalcyon/src/interaction_wire.rs',
 'interaction_body':'usr/lib/libhalcyon/src/interaction_body.rs',
 'interaction_frame':'usr/lib/libhalcyon/src/interaction_frame.rs',
 'apprecord':'usr/halcyond/src/apprecord.rs',
}
source=(root/modules['apprecord']).read_text()
limit=re.search(r'^pub const MAX_PANES: usize = .*?;', (root/'usr/lib/libhalcyon/src/layout.rs').read_text(), re.M).group()
cases=[
 ('collapsed-wire-error', 'Error::Unsupported => Failure::Unsupported', 'Error::Unsupported => Failure::Invalid', 'protocol_failures_preserve_the_reserved_error_codes'),
 ('changed-replay-body', 'if body.get(start..end) != Some(bytes)', 'if false', 'same_id_changed_header_or_body_never_replays'),
 ('same-id-redispatch', 'header.request_id == self.last', 'false', 'every_fragment_boundary_and_exact_replay_dispatch_only_once'),
 ('fid-reuse-receipt', 'ticket != self.ticket() { return Err(Failure::Gone); }', 'ticket.request != self.last { return Err(Failure::Gone); }', 'clunk_and_fid_reuse_cannot_accept_an_old_completion'),
 ('cancel-id-reuse', 'self.record.reset();\n        self.reply = None;', 'self.last = 0;\n        self.record.reset();\n        self.reply = None;', 'pending_write_refusal_does_not_lose_admission_and_cancel_burns_id'),
 ('reordered-fragments', 'if offset != self.received as u64', 'if false', 'surplus_wrong_offsets_and_invalid_bodies_never_dispatch'),
 ('output-capacity-undercharge', 'self.capacity()', 'self.len()', 'all_fids_share_input_and_output_budgets_including_cache_capacity'),
 ('uncounted-prefix', 'HEADER_BYTES + self.record.reserved_bytes()', 'self.record.reserved_bytes()', 'pending_write_refusal_does_not_lose_admission_and_cancel_burns_id'),
 ('late-reply-exposed', '(self.phase == Phase::Answered).then_some(self.reply.as_ref()).flatten()', 'self.reply.as_ref()', 'every_fragment_boundary_and_exact_replay_dispatch_only_once'),
 ('body-validation-skipped', 'Request::decode_body(header, body).map_err(wire_failure)?;', 'let _ = (header, body);', 'surplus_wrong_offsets_and_invalid_bodies_never_dispatch'),
]
with tempfile.TemporaryDirectory(prefix='thyla-app-records-') as tmp:
 out=Path(tmp)
 def run(name, change=None, test=None):
  wrapper='extern crate alloc;\nextern crate self as libhalcyon;\npub mod layout {'+limit+'}\n'
  for module,path in modules.items():
   file=root/path
   if module=='apprecord' and change:
    old,new=change; assert source.count(old)==1,(name,'ambiguous mutation')
    file=out/(name+'-module.rs');file.write_text(source.replace(old,new))
   wrapper+=f'#[path="{file}"] pub mod {module};\n'
  entry=out/(name+'.rs');entry.write_text(wrapper);exe=out/name
  build=subprocess.run(['rustc','--edition=2021','--test',str(entry),'-o',str(exe)],capture_output=True,text=True)
  assert build.returncode==0,build.stdout+build.stderr
  args=[str(exe)]+(['apprecord::tests::'+test,'--exact'] if test else [])
  result=subprocess.run(args,capture_output=True,text=True);evidence=result.stdout+result.stderr
  if test:
   assert result.returncode!=0 and 'assertion' in evidence and '1 failed' in evidence,evidence
   print(name+': intended assertion failure at '+test,flush=True)
  else:
   assert result.returncode==0,evidence;print(evidence,flush=True)
 run('clean')
 for name,old,new,test in cases:run(name,(old,new),test)
