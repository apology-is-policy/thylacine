#!/usr/bin/env python3
"""Actual controller/broker sources and intended lifecycle counterexamples.

Run from a resource-leased checkout. Temporary extracted sources/executables
are removed on exit; compile failures never count as a successful mutant.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
source = (root / 'usr/halcyond/src/controllers.rs').read_text()
pane_limit = re.search(r'^pub const MAX_PANES: usize = .*?;',
                      (root / 'usr/lib/libhalcyon/src/layout.rs').read_text(), re.M).group()
modules = {
    'interaction_wire': 'usr/lib/libhalcyon/src/interaction_wire.rs',
    'interaction_body': 'usr/lib/libhalcyon/src/interaction_body.rs',
    'interaction_control': 'usr/lib/libhalcyon/src/interaction_control.rs',
    'clipboard': 'usr/halcyond/src/clipboard.rs',
    'clipbroker': 'usr/halcyond/src/clipbroker.rs',
}

cases = [
    ('pending-authority', 'e.pending.is_none() && e.peer == fresh', 'e.peer == fresh',
     'pending_is_not_a_controller_and_receipt_is_single_use'),
    ('wrong-peer', 'e.peer == fresh && e.authority.owner.scope == scope',
     'e.authority.owner.scope == scope', 'exact_principal_connection_stripes_and_scope_are_required'),
    ('wrong-seat', 'self.normal == Some(r.seat)', 'self.normal.is_some()',
     'receipt_checks_every_authorizing_field_and_fresh_peer'),
    ('stale-mode', 'sequence <= r.sequence', 'sequence < r.sequence',
     'mode_reports_are_ordered_bounded_and_selected_by_exact_route'),
    ('leaf-only-retirement', 'e.terminal.route == route', 'e.terminal.route.leaf == route.leaf',
     'replacement_and_foreground_round_trip_never_revive_an_owner'),
    ('missing-subject-retirement', 'e.terminal.foreground != epoch || e.peer.stripes != subject',
     'e.terminal.foreground != epoch', 'same_epoch_changed_or_missing_nomination_retires_the_controller'),
    ('reused-generation', 'self.next = next;', 'let _ = next;',
     'failed_publication_and_old_request_cannot_reuse_a_generation'),
    ('no-retirement-callback', 'retire(e.authority.owner);', 'let _ = e;',
     'retirement_callback_cancels_real_broker_work'),
]

with tempfile.TemporaryDirectory(prefix='thylacine-controllers-') as tmp:
    out = Path(tmp)
    def run(name, controller_source, test=None):
        controller = out / (name + '-controllers.rs')
        controller.write_text(controller_source)
        wrapper = 'extern crate alloc;\nextern crate self as libhalcyon;\n'
        wrapper += 'pub mod layout {' + pane_limit + '}\n'
        for module, path in modules.items():
            wrapper += f'#[path="{root / path}"] pub mod {module};\n'
        wrapper += f'#[path="{controller}"] pub mod controllers;\n'
        entry = out / (name + '.rs'); entry.write_text(wrapper)
        exe = out / name
        built = subprocess.run(['rustc', '--edition=2021', '--test', str(entry), '-o', str(exe)],
                               capture_output=True, text=True)
        assert built.returncode == 0, built.stdout + built.stderr
        args = [str(exe)]
        if test: args += ['controllers::tests::' + test, '--exact']
        result = subprocess.run(args, capture_output=True, text=True)
        evidence = result.stdout + result.stderr
        if test:
            expected = {'wrong-peer': '\nfield 2\n',
                        'leaf-only-retirement': 'old route retired replacement'}.get(name, 'assertion')
            assert result.returncode != 0 and expected in evidence, evidence
            assert '1 failed' in evidence, evidence
            print(name + ': intended assertion failure at ' + test, flush=True)
        else:
            assert result.returncode == 0, evidence
            print(evidence, flush=True)
    run('clean', source)
    for name, before, after, test in cases:
        assert before in source, name
        run(name, source.replace(before, after), test)
