#!/usr/bin/env python3
"""Compile the actual interaction owner and test its intended counterexamples.

A compiler error never counts as a rejected mutation. Run under a Mac lease.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
modules = {
    'interaction_events': 'usr/lib/libhalcyon/src/interaction_events.rs',
    'interaction_wire': 'usr/lib/libhalcyon/src/interaction_wire.rs',
    'interaction_body': 'usr/lib/libhalcyon/src/interaction_body.rs',
    'interaction_control': 'usr/lib/libhalcyon/src/interaction_control.rs',
    'clipboard': 'usr/halcyond/src/clipboard.rs',
    'clipbroker': 'usr/halcyond/src/clipbroker.rs',
    'controllers': 'usr/halcyond/src/controllers.rs',
    'interaction': 'usr/halcyond/src/interaction.rs',
}
sources = {name: (root / path).read_text() for name, path in modules.items()}
limit = re.search(r'^pub const MAX_PANES: usize = .*?;',
                  (root / 'usr/lib/libhalcyon/src/layout.rs').read_text(), re.M).group()
cases = [
    ('lost-cancelled-drain', 'interaction',
     'self.flight.is_some_and(|f| f.reported)', 'false',
     'cancelled_check_still_requests_transport_drain_at_its_deadline'),
    ('late-control-success', 'interaction',
     'if Self::overdue(f, now)', 'if false',
     'delayed_control_completion_checks_time_without_an_expiry_pass'),
    ('duplicate-timeout-result', 'interaction',
     'if f.reported {', 'if false {',
     'control_expiry_reports_once_and_holds_slot_until_exact_drain'),
    ('missing-control-timer', 'interaction',
     '!f.reported && Self::overdue(*f, now)', 'false',
     'control_expiry_reports_once_and_holds_slot_until_exact_drain'),
    ('reopen-closed-owner', 'interaction',
     'self.closed = true;', 'self.closed = false;',
     'transport_close_is_terminal_and_does_not_duplicate_expired_results'),
    ('initial-seat-owner', 'interaction',
     '// Generation zero is the initial normal seat; only None revokes it.',
     'let normal = normal.filter(|n| *n != 0);',
     'initial_zero_seat_registers_copies_and_retires_without_revival'),
    ('initial-seat-controller', 'controllers',
     '// Generation zero is the initial normal seat; only None revokes it.',
     'let normal = normal.filter(|n| *n != 0);',
     'initial_zero_seat_registers_copies_and_retires_without_revival'),
    ('released-slot', 'interaction', 'if self.flight.is_some() {', 'if false {',
     'cancellation_holds_transport_slot_until_exact_completion'),
    ('id-only-reply', 'interaction', 'if f.request != request {',
     'if f.request.request != request.request {',
     'cancellation_holds_transport_slot_until_exact_completion'),
    ('leaf-only-bind', 'interaction', 'if f.route == Some(route) {',
     'if f.request.leaf == route.leaf {',
     'bind_retirement_uses_incarnation_even_without_controller'),
    ('no-broker-retirement', 'interaction', '        b.drop_owner(owner)\n    }', '        None\n    }',
     'same_epoch_subject_retirement_cancels_payload_and_mode_together'),
    ('no-focus-loss', 'interaction', 'done = b.lose_focus(a.owner, epoch);',
     'let _ = (a, epoch, &b);',
     'focus_retains_mode_and_earliest_boundary_for_snapshot_reads'),
    ('zero-focus', 'clipbroker', '|| r.focus == 0', '',
     'invalid_controls_and_receipts_never_authorize'),
    ('reused-control-id', 'clipbroker', 'self.next = id.checked_add(1).ok_or(Failure::Busy)?;',
     'let _ = id.checked_add(1).ok_or(Failure::Busy)?;',
     'control_publication_and_clipboard_share_one_sequence_and_slot'),
    ('seat-revival', 'interaction', 'if f.usable { result } else { Err(Failure::Gone) }',
     'result', 'seat_round_trip_cannot_revive_pending_host_control'),
    ('late-host-retirement', 'interaction', 'self.route_gone(route);', '',
     'host_unbind_retires_locally_even_when_transport_refuses'),
]
with tempfile.TemporaryDirectory(prefix='thylacine-interaction-owner-') as tmp:
    out = Path(tmp)
    def run(name, changed=None, test=None):
        wrapper = 'extern crate alloc;\nextern crate self as libhalcyon;\n'
        wrapper += 'pub mod layout {' + limit + '}\n'
        for module, path in modules.items():
            if changed and module == changed[0]:
                source = out / (name + '-' + module + '.rs')
                assert sources[module].count(changed[1]) == 1, (name, 'ambiguous mutation')
                source.write_text(sources[module].replace(changed[1], changed[2]))
            else:
                source = root / path
            wrapper += f'#[path="{source}"] pub mod {module};\n'
        entry = out / (name + '.rs'); entry.write_text(wrapper)
        exe = out / name
        build = subprocess.run(['rustc', '--edition=2021', '--test', str(entry), '-o', str(exe)],
                               capture_output=True, text=True)
        assert build.returncode == 0, build.stdout + build.stderr
        args = [str(exe)]
        if test: args += ['interaction::tests::' + test, '--exact']
        result = subprocess.run(args, capture_output=True, text=True)
        evidence = result.stdout + result.stderr
        if test:
            assert result.returncode != 0 and 'assertion' in evidence and '1 failed' in evidence, evidence
            print(name + ': intended assertion failure at ' + test, flush=True)
        else:
            assert result.returncode == 0, evidence
            print(evidence, flush=True)
    run('clean')
    for name, module, before, after, test in cases:
        run(name, (module, before, after), test)
