#!/usr/bin/env python3
"""Turn every native-adapter test red once with a targeted adapter sabotage.

Each case edits the tree, runs exactly its test, and must see that test FAIL
(or, for a should_panic test, not panic). The file is restored from the bytes
read before the edit, never from git, so uncommitted work survives.
Usage: sabotage.py <out-dir> [case-name ...]
"""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
SRC = os.path.join(ROOT, 'usr/lib/loom-client/src')
HOST = subprocess.run(['rustc', '-vV'], capture_output=True, text=True).stdout
HOST = next(l.split()[1] for l in HOST.splitlines() if l.startswith('host:'))

# (case, test, [(file, old, new), ...])
CASES = [
 ('ring-detach', 'setup_registers_every_buffer_and_drop_detaches_every_region',
  [('native/kernel.rs', '''        if let Some((va, len)) = self.ring.take() {
            self.kernel.detach(va, len);
        }''', '''        let _ = self.ring.take();''')]),
 ('attach-unwind', 'a_refused_attach_detaches_what_was_attached',
  [('native/kernel.rs', '''                for &(va, len) in &regions {
                    kernel.detach(va, len);
                }''', '')]),
 ('fixed-offset', 'connect_walk_open_write_then_read_round_trips',
  [('native/handles.rs', 's.args[0] = range.offset;', 's.args[0] = 0;')]),
 ('connect-drop-reap', 'a_failed_connect_reaps_its_scope',
  [('native/handles.rs', '''            self.inner.shared().abort(scope);
            self.inner.shared().set_state(scope, SlotState::Reap);''',
    '''            self.inner.shared().abort(scope);''')]),
 ('name-late', 'each_walk_name_is_read_inside_the_enter_that_consumes_it',
  [('native/mod.rs', '''        unsafe { self.sq.push(&sqe) };
        self.enter();
        Ok(cookie)''', '''        let late = self.sq.next_slot() as usize;
        unsafe { core::ptr::write_bytes(self.sq.names.add(late * NAME_CELL), 0, NAME_CELL) };
        unsafe { self.sq.push(&sqe) };
        self.enter();
        Ok(cookie)''')]),
 ('name-one-cell', 'queued_walks_keep_their_own_name_cells',
  [('native/mod.rs', 'self.sq.names.add(slot * NAME_CELL),', 'self.sq.names,'),
   ('native/mod.rs', 'sqe.args[0] = (slot * NAME_CELL) as u64;', 'sqe.args[0] = 0;')]),
 ('stream-order', 'a_multishot_stream_runs_on_out_of_order_returns',
  [('native/mod.rs', 's.payloads.push_back(payload);', 's.payloads.push_front(payload);')]),
 ('no-deadline', 'a_deadline_ends_the_operation_and_aborts_its_scope',
  [('native/handles.rs', 'deadline.map_or(0, |d| d.max(1))', 'deadline.map_or(0, |_| 0)')]),
 ('poll-floor', 'poll_rounds_the_time_left_up_to_a_whole_millisecond',
  [('native/mod.rs', 'left.div_ceil(1_000_000)', '(left / 1_000_000)')]),
 ('no-abort', 'dropping_a_future_aborts_its_scope_and_settles_its_entry',
  [('native/kernel.rs', '''    fn abort_scope(&self, scope: SlotRef) {
        self.0.abort(scope);
    }''', '''    fn abort_scope(&self, _scope: SlotRef) {}''')]),
 ('burst-loss', 'a_full_cq_defers_completions_and_loses_none',
  [('native/mod.rs', 'if s.open {', 'if s.open && s.payloads.len() < 3 {')]),
 ('no-turn-enter', 'a_short_enter_leaves_the_rest_queued_for_a_later_turn',
  [('native/mod.rs', '''        if self.sq.pending() != 0 {
            self.enter();
        }''', '')]),
 ('no-space-check', 'a_full_sq_answers_enospc',
  [('native/mod.rs', '''        if !self.sq.space() {
            return Err(-ENOSPC);
        }''', '')]),
 ('open-masks-errno', 'a_rejected_sqe_ends_its_future_with_the_kernels_errno',
  [('native/handles.rs', 'r.and_then(|c| if c.result < 0 { Err(c.result) } else { Ok(()) })',
    'r.map(|_| ())')]),
 ('dropped-ignored', 'a_dropped_sqe_ends_the_process',
  [('native/mod.rs', '''            panic!("loom-client: the kernel dropped an SQE the adapter wrote in range");''', '')]),
 ('forged-not-failed', 'a_forged_completion_fails_the_ring',
  [('native/mod.rs', '''        let cookie = match routed {
            Err(_) => {
                self.fail();
                return Err(-EIO);
            }''', '''        let cookie = match routed {
            Err(_) => {
                return Err(-EIO);
            }''')]),
 ('close-detaches', 'closing_with_leases_held_keeps_their_bytes_until_they_drop',
  [('native/kernel.rs', '''        let fd = self.fd.replace(-1);
        if fd >= 0 {
            self.kernel.close(fd);
        }''', '''        let fd = self.fd.replace(-1);
        if fd >= 0 {
            self.kernel.close(fd);
            for &(va, len) in self.regions.iter() {
                self.kernel.detach(va, len);
            }
        }''')]),
 ('enter-ebadf-ignored', 'an_enter_that_finds_the_descriptor_closed_forgets_it',
  [('native/mod.rs', 'if rc == -(ECANCELED as i64) || rc == -(EBADF as i64) {', 'if rc == -(ECANCELED as i64) {')]),
 ('enter-ecanceled-ignored', 'an_enter_that_races_the_close_forgets_the_descriptor',
  [('native/mod.rs', 'if rc == -(ECANCELED as i64) || rc == -(EBADF as i64) {', 'if rc == -(EBADF as i64) {')]),
 ('pollnval-ignored', 'a_poll_that_finds_the_descriptor_closed_forgets_it',
  [('native/mod.rs', 'if events & (POLLHUP | POLLNVAL) != 0 {', 'if events & POLLHUP != 0 {')]),
 ('pollhup-ignored', 'a_hangup_while_polling_forgets_the_descriptor',
  [('native/mod.rs', 'if events & (POLLHUP | POLLNVAL) != 0 {', 'if events & POLLNVAL != 0 {')]),
 ('lost-closes', 'an_enter_that_races_the_close_forgets_the_descriptor',
  [('native/mod.rs', '''        self.shared.forget_fd();
        self.close();''', '''        self.close();''')]),
 ('retire-no-abort', 'retire_waits_until_the_kernel_retires_every_scope',
  [('native/kernel.rs', '''        for scope in scopes.into_iter().flatten() {
            self.0.abort(scope);
        }''', '''        let _ = scopes;''')]),
 ('retired-early', 'retire_waits_until_the_kernel_retires_every_scope',
  [('native/kernel.rs', '''        if self.0.admission.get() || self.0.fd() < 0 {
            return false;
        }''', '''        if !self.0.admission.get() {
            return true;
        }''')]),
 ('no-reap-retry', 'an_exhausted_slot_table_reaps_before_it_refuses',
  [('native/handles.rs', '''        Err(e) if e == -ENOSPC => {
            inner.shared().reap_pass();
            inner.shared().reserve(kind, scope)
        }''', '''        Err(e) if e == -ENOSPC => Err(e),''')]),
 ('reap-one-pass', 'a_dropped_scope_is_reaped_after_its_fids_without_clunking_them',
  [('native/kernel.rs', '''            if !freed {
                break;
            }''', '''            let _ = freed;
            break;''')]),
 ('walk-drop-clunks', 'a_failed_walk_dropped_unobserved_is_reaped_without_a_clunk',
  [('native/handles.rs', '''            let state = match self.request.take() {
                Ok(Some(c)) if c.result < 0 => SlotState::Reap,
                _ => SlotState::Clunk,
            };''', '''            let state = SlotState::Clunk;''')]),
 ('clunk-aborted-scope', 'a_walk_dropped_in_flight_aborts_and_reaps_its_reservation_unclunked',
  [('native/mod.rs', '.is_some_and(|s| !s.aborted && s.state == kernel::SlotState::Live);',
    '.is_some_and(|s| s.state == kernel::SlotState::Live);')]),
 ('abandon-no-abort', 'an_open_dropped_in_flight_aborts_its_scope',
  [('fixed_requests.rs', '''                if let Some(scope) = c.book.abandon(self.cookie).expect("active owner") {
                    c.owner.abort_scope(scope);
                }''', '''                let _ = c.book.abandon(self.cookie).expect("active owner");''')]),
 ('abort-twice', 'an_explicit_scope_abort_ends_its_operations_once',
  [('native/kernel.rs', 'Some(e) if e.r == scope => !core::mem::replace(&mut e.aborted, true),',
    'Some(e) if e.r == scope => { e.aborted = true; true }')]),
 ('reap-busy-dropped', 'a_target_dropped_under_its_connect_is_reaped_after_it',
  [('native/kernel.rs', '} else if rc != -(EBUSY as i64) {', '} else {')]),
 ('book-wiped', 'a_future_held_across_retirement_keeps_its_terminal',
  [('request_book.rs', '''            if entry.as_ref().is_some_and(|e| e.abandoned) {
                *entry = None;
            }''', '''            *entry = None;''')]),
 ('retired-before-posted', 'retirement_waits_for_terminals_posted_after_the_scope_retires',
  [('native/kernel.rs', '''        if self.0.submitted.get() != self.0.ended.get() {
            return false;
        }
''', '')]),
 ('unentered-sqe', 'an_sqe_not_yet_entered_holds_retirement_back',
  [('native/kernel.rs', '''        if self.0.submitted.get() != self.0.ended.get() {
            return false;
        }
''', '')]),
 ('cq-unrouted', 'a_terminal_posted_but_not_yet_routed_holds_retirement_back',
  [('native/kernel.rs', '''        if self.0.submitted.get() != self.0.ended.get() {
            return false;
        }
''', '')]),
 ('refusal-unposted', 'an_enter_refusal_not_yet_posted_holds_retirement_back',
  [('native/kernel.rs', '''        if self.0.submitted.get() != self.0.ended.get() {
            return false;
        }
''', '')]),
 ('closing-lease-kept', 'a_terminal_posted_but_not_yet_routed_holds_retirement_back',
  [('native/mod.rs', '''                        if cqe.result > 0 {
                            let rc = self.shared.return_buffer(d.record.receipt);''', '''                        if false {
                            let rc = self.shared.return_buffer(d.record.receipt);''')]),
 ('dropped-stream-lease-kept', 'a_stream_dropped_before_its_end_returns_the_shots_still_arriving',
  [('native/mod.rs', 'drop(rejected); // An abandoned stream', 'core::mem::forget(rejected); // An abandoned stream')]),
]


def run_one(test, log):
    cmd = ['cargo', 'test', '-p', 'loom-client', '--lib', '--target', HOST, '--',
           'tests::native::' + test, '--exact']
    p = subprocess.run(cmd, cwd=os.path.join(ROOT, 'usr'), capture_output=True, text=True)
    with open(log, 'w') as f:
        f.write(p.stdout + p.stderr)
    return p.returncode, p.stdout + p.stderr


def main():
    out = sys.argv[1]
    only = set(sys.argv[2:])
    os.makedirs(out, exist_ok=True)
    bad = []
    for case, test, edits in CASES:
        if only and case not in only:
            continue
        saved = {}
        try:
            for rel, old, new in edits:
                path = os.path.join(SRC, rel)
                if path not in saved:
                    saved[path] = open(path).read()
                text = open(path).read()
                if text.count(old) != 1:
                    raise SystemExit(f'{case}: anchor not unique in {rel}: {old[:50]!r}')
                open(path, 'w').write(text.replace(old, new))
            rc, text = run_one(test, os.path.join(out, case + '.log'))
        finally:
            for path, body in saved.items():
                open(path, 'w').write(body)
        if 'error[' in text or 'could not compile' in text:
            verdict = 'BUILD-ERROR'
        elif 'running 1 test' not in text:
            verdict = 'RAN-NOTHING'
        elif rc != 0 and ('test result: FAILED. 0 passed; 1 failed' in text or 'process didn\'t exit successfully' in text):
            verdict = 'RED'
        else:
            verdict = 'GREEN'
        print(f'{verdict:12} {case:24} {test}')
        if verdict != 'RED':
            bad.append(case)
    print('sabotage: ' + ('every case red' if not bad else 'NOT RED: ' + ' '.join(bad)))
    sys.exit(1 if bad else 0)


main()
