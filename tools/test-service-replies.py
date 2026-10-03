#!/usr/bin/env python3
"""Actual stream schedules and assertion witnesses for parked reply retirement."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
source = (root / 'usr/halcyond/src/servicewire.rs').read_text()
cases = [
 ('input-allowance-ignored', 'handler.input_allowance().min(MAX_FRAME)', 'MAX_FRAME', 'transport_shares_budgets_with_retained_protocol_caches'),
 ('frame-length-as-capacity', 'handler.dispatch_buffered(&self.input[..len], self.input.capacity())', 'handler.dispatch_buffered(&self.input[..len], len)', 'transport_shares_budgets_with_retained_protocol_caches'),
 ('output-quota-ignored', '&& handler.output_reserved() <= handler.output_allowance().min(MAX_FRAME)', '', 'over_budget_output_is_refused_before_any_wire_byte'),

 ('overwrite-busy', '|| self.reply_len != 0', '', 'exact_park_resumes_only_after_immediate_reply_drains'),
 ('wrong-ticket', 'if self.closed || ticket == 0 || self.parked != ticket || self.reply_len != 0 {', 'if self.closed || ticket == 0 || self.reply_len != 0 {', 'exact_park_resumes_only_after_immediate_reply_drains'),
 ('retain-cancelled-park', 'self.parked = 0;\n        self.sent = 0;', 'self.sent = 0;', 'cancellation_preserves_ticket_monotonicity_and_drops_old_park'),
 ('reuse-park-ticket', 'ticket <= self.last_park', 'false', 'cancellation_preserves_ticket_monotonicity_and_drops_old_park'),
 ('reuse-partial-frame', 'self.closed |= self.sent != 0;', 'self.closed |= false;', 'cancelled_output_discards_buffered_input_and_partial_frames_poison'),
 ('retain-buffered-input', 'self.input = Vec::new();', 'let _ = &self.input;', 'cancelled_output_discards_buffered_input_and_partial_frames_poison'),
]
with tempfile.TemporaryDirectory(prefix='thylacine-service-replies-') as tmp:
 out = Path(tmp)
 def run(name, before=None, after=None, test=None):
  s = source
  if before:
   assert s.count(before) == 1, (name, 'ambiguous mutation')
   s = s.replace(before, after)
  unit = out / (name + '.rs'); unit.write_text(s)
  entry = out / (name + '-test.rs')
  entry.write_text('extern crate alloc;\n#[path="' + str(unit) + '"] mod servicewire;\n')
  exe = out / name
  r = subprocess.run(['rustc','--edition=2021','--test',str(entry),'-o',str(exe)],capture_output=True,text=True)
  assert r.returncode == 0, r.stdout + r.stderr
  args = [str(exe)] + (['servicewire::tests::'+test,'--exact'] if test else [])
  r = subprocess.run(args,capture_output=True,text=True)
  text = r.stdout + r.stderr
  if test:
   assert r.returncode != 0 and 'assertion' in text and '1 failed' in text, text
   print(name + ': intended assertion failure at ' + test, flush=True)
  else:
   assert r.returncode == 0, text
   print(text, flush=True)
 run('clean')
 for row in cases: run(*row)
