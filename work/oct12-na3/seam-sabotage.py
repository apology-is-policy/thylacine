#!/usr/bin/env python3
"""Each NA-3 seam test fails when its path is broken, and no original test does.
Usage: work/oct12-na3/seam-sabotage.py SCRATCH_DIR   (the mac lease must be held)"""
import re, shutil, subprocess, sys, os
from pathlib import Path
R = Path(__file__).resolve().parents[2]; S = Path(sys.argv[1])
SAB = [  # file, anchor (unique), replacement, the test that must fail
 ('fixed_futures.rs', 'impl<B: RingBackend> ReadFuture<B> {\n    pub(crate) fn set_deadline(&mut self, deadline: Option<u64>) -> Result<(), Error> {\n        self.request.set_deadline(deadline)\n    }\n    pub(crate) fn request_cancel(&mut self) -> Result<bool, Error> {\n        self.request.request_cancel()',
  None, 'timer_read_future_cancel_waits_for_actual_terminal'),
 ('local_wait.rs', 'Outcome::Woke(reason)', 'Outcome::Woke(if reason == WakeReason::Event { WakeReason::Spurious } else { reason })',
  'wait_event_wake_is_reported_and_releases_the_subscription'),
 ('request_book.rs', '            kind: e.kind,\n', '            kind: Kind::Control,\n', 'delivery_reports_its_own_reserved_kind'),
 ('fixed_futures.rs', 'ok_or(Error::Storage(super::ring_payload::Error::Full))',
  'ok_or(Error::Storage(super::ring_payload::Error::Busy))', 'future_full_registration_table_answers_full'),
]
bad = 0
for f, a, b, test in SAB:
    if b is None: b = a[:a.rindex('self.request.request_cancel()')] + 'Ok(false)'
    d = S / 'sab'; shutil.rmtree(d, ignore_errors=True)
    shutil.copytree(R / 'usr/lib/loom-client', d / 'crate')
    t = (d / 'crate/Cargo.toml').read_text().replace('publish.workspace = true', 'publish = false').replace('license.workspace = true', 'license = "0BSD"')
    (d / 'crate/Cargo.toml').write_text(t + '\n[workspace]\n')
    p = d / 'crate/src' / f; s = p.read_text()
    assert s.count(a) == 1, (f, s.count(a)); p.write_text(s.replace(a, b))
    r = subprocess.run(['cargo', 'test', '--lib'], cwd=d / 'crate', capture_output=True, text=True,
                       env=dict(os.environ, CARGO_TARGET_DIR=str(S / 'sab-target'), CARGO_INCREMENTAL='0'))
    out = r.stdout + r.stderr
    failed = re.findall(r'^test (\S+) \.\.\. FAILED$', out, re.M)
    summ = re.search(r'^test result: .*$', out, re.M)
    ok = r.returncode != 0 and [x.split('::')[-1] for x in failed] == [test]
    bad += not ok
    print(f"{'RED ' if ok else 'BAD '} {f}: failing {failed} | {summ.group(0) if summ else 'no summary'}")
shutil.rmtree(S / 'sab', ignore_errors=True); shutil.rmtree(S / 'sab-target', ignore_errors=True)
sys.exit(bad)
