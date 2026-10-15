#!/usr/bin/env python3
"""Turn every C native-adapter scenario red with a targeted sabotage.

The C twin of work/oct13-na4b/sabotage.py. Each case edits the tree, builds
test_adapter the way the sanitized profile of tools/test-loom-client.sh does
(-O0, ASan + UBSan, no recovery), runs exactly its scenario, and must see it
fail. Files are restored from the bytes read before the edit, never from git,
so uncommitted work survives.

A control runs first: the unedited tree, every scenario a case names, each of
which must pass. A scenario that fails unedited would read RED under any edit.

A scenario is one of test_adapter's by name; or test_<core>, a whole core test
binary; or rust:<module>::<test>, one #[test] of loom-client run with cargo.

Usage: sabotage.py <out-dir> [case-name ...]
"""
import os, subprocess, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
LIBT = os.path.join(ROOT, 'usr/lib/libt')
SRC = os.path.join(LIBT, 'src/loom_client')
TESTS = os.path.join(LIBT, 'tests/loom_client')
CC = os.path.join(os.environ.get('LLVM_PREFIX', '/opt/homebrew/opt/llvm'), 'bin/clang')
CORES = ['adapter', 'fixed_requests', 'lease_ledger', 'owned_mapping', 'paired_cq', 'request_book',
         'retirement_executor', 'ring_payload']
SAN = ['-O0', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer']

A = 'src/loom_client/adapter.c'
F = 'src/loom_client/fixed_requests.c'
B = 'src/loom_client/request_book.c'
SIM = 'tests/loom_client/sim_kernel.c'
L = 'src/loom_client/lease_ledger.c'
RB = 'src/loom_client/request_book.c'
RUST = '../loom-client/src/request_book.rs'

REG_EBADF = (A, '    if (rc == -E_BADF && (op != T_LOOM_REGISTER_SERVICE_TARGET || gone(r))) {',
             '    if (rc == -E_BADF && op == T_LOOM_REGISTER_SERVICE_TARGET && gone(r)) {')
UNSETTLED = (A, '    if (r->submitted != r->ended) return 0;\n', '')
OPEN_RESULT = '    if (rc != 1) return rc;\n    *result = c.result;\n    consumed(op->ring, op, 1);'

# (case, scenario, [(file under usr/lib/libt, old, new), ...])
CASES = [
 ('ring-detach', 'setup_registers_every_buffer_and_drop_detaches_every_region',
  [(A, '    if (r->ring_valid) r->k.detach(r->k.context, r->ring_va, r->ring_len);\n', '')]),
 ('attach-unwind', 'a_refused_attach_detaches_what_was_attached',
  [(A, '            if (va > 0) k->detach(k->context, (uintptr_t)va, len[i]);\n            release_holder(r);',
    '            r->regions = 0;\n            release_holder(r);')]),
 ('fixed-offset', 'connect_walk_open_write_then_read_round_trips',
  [(A, '    sqe._resv1[0] = range.offset;', '    sqe._resv1[0] = 0;')]),
 ('connect-fail-unreaped', 'a_failed_connect_reaps_its_scope',
  [(A, '    abort_scope(r, op->dest);\n    e->state = S_REAP;\n    consumed(r, op, 0);',
    '    abort_scope(r, op->dest);\n    consumed(r, op, 0);')]),
 ('name-late', 'each_walk_name_is_read_inside_the_enter_that_consumes_it',
  [(A, '    sq_push(r, sqe);\n    ++r->submitted;',
    '    t_bytes_zero(r->names + (size_t)sq_next_slot(r) * NAME_CELL, NAME_CELL);\n'
    '    sq_push(r, sqe);\n    ++r->submitted;')]),
 ('name-one-cell', 'queued_walks_keep_their_own_name_cells',
  [(A, 't_bytes_copy(r->names + (size_t)slot * NAME_CELL, name, len);', 't_bytes_copy(r->names, name, len);'),
   (A, 'sqe->_resv1[0] = (uint64_t)slot * NAME_CELL;', 'sqe->_resv1[0] = 0;')]),
 ('stream-order', 'a_multishot_stream_runs_on_out_of_order_returns',
  [(A, '(!first || r->shots[i].seq < first->seq)', '(!first || r->shots[i].seq > first->seq)')]),
 ('no-deadline', 'a_deadline_ends_the_operation_and_aborts_its_scope',
  [(A, '    s._resv1[3] = deadline_word(has_deadline, deadline);', '    s._resv1[3] = 0;')]),
 ('poll-floor', 'poll_rounds_the_time_left_up_to_a_whole_millisecond',
  [(A, 'uint64_t ms = left / 1000000 + (left % 1000000 != 0);', 'uint64_t ms = left / 1000000;')]),
 ('no-abort', 'dropping_an_operation_aborts_its_scope_and_settles_its_entry',
  [(A, 'static void b_abort_scope(void *v, struct t_loom_service_ref scope) { abort_scope(v, scope); }',
    'static void b_abort_scope(void *v, struct t_loom_service_ref scope) { (void)v; (void)scope; }')]),
 ('burst-loss', 'a_full_cq_defers_completions_and_loses_none',
  [(A, '        if (op->handle)\n            queue_push(r, op, payload);',
    '        if (op->handle && r->shot_seq < 3)\n            queue_push(r, op, payload);')]),
 ('no-turn-enter', 'a_short_enter_leaves_the_rest_queued_for_a_later_turn',
  [(A, '''    if (sq_pending(r)) {
        enter_ring(r);
        // A close learnt here must not reach the routing below as a failure.
        if ((rc = check(r))) return rc;
    }
''', '')]),
 ('no-space-check', 'a_full_sq_answers_enospc',
  [(A, '    if (!sq_space(r)) return -E_NOSPC;\n    uint64_t c = 0;', '    uint64_t c = 0;')]),
 ('open-masks-errno', 'a_rejected_sqe_ends_its_operation_with_the_kernels_errno',
  [(A, OPEN_RESULT, OPEN_RESULT.replace('*result = c.result;', '*result = c.result < 0 ? 0 : c.result;'))]),
 ('dropped-ignored', 'a_dropped_sqe_ends_the_process',
  [(A, '''    T_LOOM_CHECK(atomic_load_explicit(word(r, offsetof(struct t_loom_ring_hdr, dropped)), memory_order_acquire) ==
                 r->dropped);
''', '')]),
 ('forged-not-failed', 'a_forged_completion_fails_the_ring',
  [(A, '    if (t_fixed_route(r->driver, record, &routed)) {\n        fail(r);\n        return -E_IO;',
    '    if (t_fixed_route(r->driver, record, &routed)) {\n        return -E_IO;')]),
 ('close-detaches', 'closing_with_leases_held_keeps_their_bytes_until_they_drop',
  [(A, '    t_mapping_destroy(&r->map);\n    release_holder(r);\n}',
    '    t_mapping_destroy(&r->map);\n'
    '    for (size_t i = 0; i < r->regions; ++i) r->k.detach(r->k.context, r->region_va[i], r->region_len[i]);\n'
    '    r->regions = 0;\n    release_holder(r);\n}')]),
 ('enter-ebadf-ignored', 'an_enter_that_finds_the_descriptor_closed_forgets_it',
  [(A, '    if (rc == -E_CANCELED || rc == -E_BADF)\n        lost_ring(r);', '    if (rc == -E_CANCELED)\n        lost_ring(r);')]),
 ('enter-ecanceled-ignored', 'an_enter_that_races_the_close_forgets_the_descriptor',
  [(A, '    if (rc == -E_CANCELED || rc == -E_BADF)\n        lost_ring(r);', '    if (rc == -E_BADF)\n        lost_ring(r);')]),
 ('lost-closes', 'an_enter_that_races_the_close_forgets_the_descriptor',
  [(A, 'static void forget_fd(struct t_loom_ring *r) {\n    r->fd = -1;',
    'static void forget_fd(struct t_loom_ring *r) {\n    close_fd(r);')]),
 ('pollnval-ignored', 'a_poll_that_finds_the_descriptor_closed_forgets_it',
  [(A, '    if (events & (POLLHUP | POLLNVAL)) {', '    if (events & POLLHUP) {')]),
 ('pollhup-ignored', 'a_hangup_while_polling_forgets_the_descriptor',
  [(A, '    if (events & (POLLHUP | POLLNVAL)) {', '    if (events & POLLNVAL) {')]),
 ('retire-no-abort', 'retire_waits_until_the_kernel_retires_every_scope',
  [(A, '    for (size_t i = 0; i < 64; ++i)\n        if (r->slots[i].kind == S_SCOPE) abort_scope(r, r->slots[i].ref);\n', '')]),
 ('retired-early', 'retire_waits_until_the_kernel_retires_every_scope',
  [(A, '''    for (size_t i = 0; i < 64; ++i) {
        if (r->slots[i].kind != S_SCOPE) continue;
        uint32_t state = 0;
        int rc = query_state(r, r->slots[i].ref, &state);
        if (rc < 0 ? rc != -E_NOENT : state != T_LOOM_SERVICE_RETIRED) return 0;
    }
''', '')]),
 ('clunk-skipped', 'a_dropped_fid_is_clunked_then_reaped',
  [(A, '    e->handle = 0;\n    e->state = S_CLUNK;', '    e->handle = 0;\n    e->state = S_REAP;')]),
 ('reap-one-pass', 'a_dropped_scope_is_reaped_after_its_fids_without_clunking_them',
  [(A, '    } while (freed);', '    } while (0);')]),
 ('walk-drop-clunks', 'a_failed_walk_dropped_unobserved_is_reaped_without_a_clunk',
  [(A, 'taken == 1 && c.result < 0 ? S_REAP : S_CLUNK', 'S_CLUNK')]),
 ('clunk-aborted-scope', 'a_walk_dropped_in_flight_aborts_and_reaps_its_reservation_unclunked',
  [(A, 'if (!scope || scope->aborted || scope->state != S_LIVE || admit(r)) {',
    'if (!scope || scope->state != S_LIVE || admit(r)) {')]),
 ('abandon-no-abort', 'an_open_dropped_in_flight_aborts_its_scope',
  [(F, 'if(first==1)t_ring_owner_abort_scope(d->owner,scope);', '(void)first;(void)scope;')]),
 ('abort-twice', 'an_explicit_scope_abort_ends_its_operations_once',
  [(A, '    if (!e || e->aborted) return;', '    if (!e) return;')]),
 ('reap-busy-dropped', 'a_target_dropped_under_its_connect_is_reaped_after_it',
  [(A, '} else if (rc != -E_BUSY) {', '} else {')]),
 ('no-reap-retry', 'an_exhausted_slot_table_reaps_before_it_refuses',
  [(A, '    reap_pass(r);\n    return reserve_once(r, kind, scope, out);', '    return rc;')]),
 ('book-wiped', 'an_operation_held_across_retirement_keeps_its_terminal',
  [(B, 'if(b->entries[i].abandoned)b->entries[i].phase=FREE;', 'b->entries[i].phase=FREE;')]),
 ('retired-before-posted', 'retirement_waits_for_terminals_posted_after_the_scope_retires', [UNSETTLED]),
 ('unentered-sqe', 'an_sqe_not_yet_entered_holds_retirement_back', [UNSETTLED]),
 ('cq-unrouted', 'a_terminal_posted_but_not_yet_routed_holds_retirement_back', [UNSETTLED]),
 ('closing-lease-kept', 'a_terminal_posted_but_not_yet_routed_holds_retirement_back',
  [(A, '        if (refused == 1) {', '        if (0) {')]),
 ('refusal-unposted', 'an_enter_refusal_not_yet_posted_holds_retirement_back', [UNSETTLED]),
 ('dropped-stream-lease-kept', 'a_stream_dropped_before_its_end_returns_the_shots_still_arriving',
  [(A, "            t_ring_abandon(payload); // An abandoned stream's lease goes straight back.",
    "            (void)payload; // An abandoned stream's lease goes straight back.")]),
 ('return-lost-ignored', 'a_late_shot_after_the_ring_closed_under_forgets_the_descriptor',
  [(A, '''            if (back == -E_CANCELED) {
                lost_ring(r);
                return -E_CANCELED;
            }
''', '')]),
 ('register-ebadf-kept', 'a_scope_dropped_after_the_ring_closed_under_never_closes_the_number', [REG_EBADF]),
 ('register-ebadf-kept-retired', 'a_ring_whose_register_finds_it_closed_never_reports_retired', [REG_EBADF]),
 ('register-ebadf-kept-connect', 'a_connect_after_the_ring_closed_under_answers_a_close', [REG_EBADF]),
 ('register-ebadf-kept-pool', 'a_pool_made_after_the_ring_closed_under_answers_a_close', [REG_EBADF]),
 ('register-ebadf-kept-payload', 'a_payload_dropped_after_the_ring_closed_under_closes_the_ring', [REG_EBADF]),
 ('register-ebadf-kept-pool-reap', 'a_pool_dropped_after_the_ring_closed_under_never_closes_the_number',
  [REG_EBADF]),
 ('target-probe-skipped', 'a_target_refused_for_its_registry_keeps_the_ring',
  [(A, '    if (rc == -E_BADF && (op != T_LOOM_REGISTER_SERVICE_TARGET || gone(r))) {', '    if (rc == -E_BADF) {')]),
 ('target-ebadf-kept', 'a_target_registered_after_the_ring_closed_under_never_closes_the_number',
  [(A, '    if (rc == -E_BADF) forget_fd(r);\n    return rc == -E_BADF;', '    (void)rc;\n    return 0;')]),
 ('enter-recheck-skipped', 'a_close_learnt_by_the_turns_enter_is_reported_as_a_close',
  [(A, '''        enter_ring(r);
        // A close learnt here must not reach the routing below as a failure.
        if ((rc = check(r))) return rc;''', '        enter_ring(r);')]),
 ('claim-gate-open', 'a_held_refusal_claims_no_further_sqe',
  [(SIM, 'if (!cq_room(s) || s->ndeferred || s->nrejected || tail == s->sq_head) break;',
    'if (!cq_room(s) || s->ndeferred || tail == s->sq_head) break;')]),
 ('replay-returned', 'a_replayed_shot_under_a_held_payload_fails_the_ring_without_a_return',
  [(A, '        if (refused < 0) {\n            fail(r);\n            return -E_IO;\n        }',
    '        if (refused < 0) refused = 1;')]),
 ('not-ready-remapped', 'an_op_on_a_fid_not_ready_answers_enoent_and_on_a_scope_not_ready_ecanceled',
  [(A, OPEN_RESULT, OPEN_RESULT.replace('*result = c.result;',
                                        '*result = c.result == -E_NOENT ? -E_CANCELED : c.result;'))]),
 ('walk-from-scope', 'a_walk_from_a_file_fid_clones_it_or_answers_enotdir',
  [(A, '    return walk(f->ring, e->scope.slot, e->ref, name, len, has_deadline, deadline, out);',
    '    return walk(f->ring, e->scope.slot, e->scope, name, len, has_deadline, deadline, out);')]),
 ('return-errno-masked', 'a_refused_return_hands_the_payload_back',
  [(A, '    if (!rc) return 0;\n    return rc < 0 && rc > -1000 ? (int)rc : -E_IO;', '    return rc ? -E_IO : 0;')]),
 ('enter-errno-ignored', 'an_enter_refusal_that_is_not_a_close_fails_the_ring',
  [(A, '        lost_ring(r);\n    else if (rc < 0)\n        fail(r);', '        lost_ring(r);')]),
 ('eintr-fails', 'a_poll_interrupted_by_a_note_is_a_spurious_wake',
  [(A, '    if (events < 0) return events == -E_INTR ? 0 : (int)events;', '    if (events < 0) return (int)events;')]),
 ('pollerr-ignored', 'a_poll_error_fails_the_ring',
  [(A, '    if (events & POLLERR) {\n        fail(r);\n        return -E_IO;\n    }\n', '')]),
 ('close-noop', 'an_explicit_close_with_live_handles_cancels_their_use',
  [(A, 'void t_loom_close(struct t_loom_ring *r) {\n    T_LOOM_CHECK(r->root);\n    close_ring(r);',
    'void t_loom_close(struct t_loom_ring *r) {\n    T_LOOM_CHECK(r->root);')]),
 # The core edits' own tests.
 ('refuse-takes-lease', 'test_lease_ledger',
  [(L, '    m->last=r.lease;*out=r;return 1;', '    m->last=r.lease;m->state=LEASED;++b->outstanding;*out=r;return 1;')]),
 ('refuse-keeps-order', 'test_lease_ledger', [(L, '    m->last=r.lease;*out=r;return 1;', '    *out=r;return 1;')]),
 ('slot-any-result', 'test_request_book',
  [(RB, '&&(uint32_t)result!=e->spec.slot)return T_REQ_RECORD;', '&&0)return T_REQ_RECORD;')]),
 ('slot-unbounded', 'test_request_book', [(RB, 'spec.kind==T_REQ_SLOT?spec.slot>=64:', 'spec.kind==T_REQ_SLOT?0:')]),
 ('rust-slot-any-result', 'rust:tests::request_book::a_slot_request_ends_with_its_own_slot_or_an_errno',
  [(RUST, '(result >= 0 && result as u32 != slot)', '(result >= 0 && false)')]),
]


def build(out, test='test_adapter'):
    exe = os.path.join(out, test)
    srcs = [os.path.join(SRC, c + '.c') for c in CORES]
    sdk = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip()
    objs = []
    for s in srcs:
        o = os.path.join(out, os.path.basename(s)[:-2] + '.o')
        p = subprocess.run([CC, '-isysroot', sdk, '-Wall', '-Wextra', '-std=c99', *SAN, '-DT_REQUEST_TEST',
                            '-I' + os.path.join(LIBT, 'include'), '-c', s, '-o', o], capture_output=True, text=True)
        if p.returncode:
            return None, p.stderr
        objs.append(o)
    p = subprocess.run([CC, '-isysroot', sdk, '-Wall', '-Wextra', '-std=c11', *SAN, '-DT_REQUEST_TEST',
                        '-I' + os.path.join(LIBT, 'include'), os.path.join(TESTS, test + '.c'),
                        *([os.path.join(TESTS, 'sim_kernel.c')] if test == 'test_adapter' else []),
                        os.path.join(TESTS, 'host_fatal.c'), *objs,
                        '-lpthread', '-o', exe], capture_output=True, text=True)
    if p.returncode:
        return None, p.stderr
    return exe, ''


def binary(scenario):
    return scenario if scenario.startswith('test_') else 'test_adapter'


def run_one(exe, scenario, log):
    """(rc, log text, ran, passed) for one scenario."""
    if scenario.startswith('rust:'):
        test = scenario[5:]
        host = subprocess.run(['rustc', '-vV'], capture_output=True, text=True).stdout
        host = next(l.split()[1] for l in host.splitlines() if l.startswith('host:'))
        p = subprocess.run(['cargo', 'test', '-p', 'loom-client', '--lib', '--target', host, '--', test, '--exact'],
                           cwd=os.path.join(ROOT, 'usr'), capture_output=True, text=True, timeout=900)
        text = p.stdout + p.stderr
        ran, passed = 'running 1 test' in text, 'test result: ok. 1 passed' in text
    elif scenario.startswith('test_'):
        p = subprocess.run([exe], capture_output=True, text=True, timeout=300)
        text = p.stdout + p.stderr
        ran, passed = True, p.returncode == 0 and ' PASS' in text
    else:
        p = subprocess.run([exe, scenario], capture_output=True, text=True, timeout=300)
        text = p.stdout + p.stderr
        ran, passed = p.returncode != 2 and f'running {scenario}' in text, f'scenario {scenario} PASS' in text
    with open(log, 'w') as f:
        f.write(text)
    return p.returncode, text, ran, passed


def why(text):
    for line in text.splitlines():
        if any(k in line for k in ('Assertion failed', 'loom client fatal', 'loom-client fatal',
                                   'ERROR: AddressSanitizer', 'runtime error:', 'no fatal there', 'panicked at')):
            return line.strip()[:160]
    return '(no failure line)'


def dry():
    """Every anchor matches exactly once, edits applied in order; builds nothing."""
    bad = 0
    for case, _, edits in CASES:
        texts = {}
        for rel, old, new in edits:
            text = texts.setdefault(rel, open(os.path.join(LIBT, rel)).read())
            if text.count(old) != 1:
                print(f'ANCHOR-ROT   {case}: {rel}: {text.count(old)} matches for {old[:50]!r}')
                bad += 1
                break
            texts[rel] = text.replace(old, new)
    print(f'sabotage --dry: {len(CASES) - bad} of {len(CASES)} cases apply')
    sys.exit(1 if bad else 0)


def main():
    if sys.argv[1:] == ['--dry']:
        dry()
    out = sys.argv[1]
    only = set(sys.argv[2:])
    os.makedirs(out, exist_ok=True)
    cases = [c for c in CASES if not only or c[0] in only]
    names = [l.split('(')[1].split(')')[0] for l in open(os.path.join(TESTS, 'test_adapter.c'))
             if l.startswith('    T(')]
    covered = {c[1] for c in CASES} & set(names)
    missing = [n for n in names if n not in covered]
    print(f'sabotage: {len(CASES)} cases over {len(covered)} of {len(names)} scenarios'
          + (f'; NO CASE FOR: {" ".join(missing)}' if missing else ''))
    # Control: the unedited tree passes every scenario named.
    ctl = os.path.join(out, 'control')
    os.makedirs(ctl, exist_ok=True)
    bad = []
    exes = {}
    for scenario in sorted({c[1] for c in cases}):
        b = binary(scenario)
        if b not in exes and not scenario.startswith('rust:'):
            exes[b], err = build(ctl, b)
            if not exes[b]:
                print(err)
                print(f'sabotage: control {b} did not build')
                sys.exit(1)
        rc, text, ran, passed = run_one(exes.get(b), scenario, os.path.join(ctl, scenario.replace(':', '_') + '.log'))
        if rc or not ran or not passed:
            print(f'CONTROL-RED  {scenario}: {why(text)}')
            bad.append('control:' + scenario)
    if bad:
        print('sabotage: the unedited tree fails; no case can be judged')
        sys.exit(1)
    print(f'sabotage: control -- {len({c[1] for c in cases})} scenarios pass unedited')
    for case, scenario, edits in cases:
        saved = {}
        rotten = None
        exe = err = None
        d = os.path.join(out, case)
        os.makedirs(d, exist_ok=True)
        try:
            for rel, old, new in edits:
                path = os.path.join(LIBT, rel)
                if path not in saved:
                    saved[path] = open(path).read()
                text = open(path).read()
                if text.count(old) != 1:
                    # A case that cannot apply is a verdict of its own, never a
                    # reason to stop the census before the cases after it.
                    rotten = f'{rel}: {text.count(old)} matches for {old[:50]!r}'
                    break
                open(path, 'w').write(text.replace(old, new))
            if rotten is None and not scenario.startswith('rust:'):
                exe, err = build(d, binary(scenario))
            if rotten is None and scenario.startswith('rust:'):
                exe = 'cargo'
                rc, text, ran, passed = run_one(None, scenario, os.path.join(d, 'run.log'))
        finally:
            for path, body in saved.items():
                open(path, 'w').write(body)
        if rotten is not None:
            print(f'{"ANCHOR-ROT":12} {case:30} {rotten}')
            bad.append(case)
            continue
        if not exe:
            with open(os.path.join(d, 'build.log'), 'w') as f:
                f.write(err)
            print(f'{"BUILD-ERROR":12} {case:30} see {d}/build.log')
            bad.append(case)
            continue
        if not scenario.startswith('rust:'):
            rc, text, ran, passed = run_one(exe, scenario, os.path.join(d, 'run.log'))
        if not ran:
            verdict = 'RAN-NOTHING'
        elif rc != 0 and not passed:
            verdict = 'RED'
        else:
            verdict = 'GREEN'
        print(f'{verdict:12} {case:30} {scenario}' + (f'\n             {why(text)}' if verdict == 'RED' else ''))
        if verdict != 'RED':
            bad.append(case)
    print('sabotage: ' + ('every case red' if not bad else 'NOT RED: ' + ' '.join(bad)))
    sys.exit(1 if bad else 0)


main()
