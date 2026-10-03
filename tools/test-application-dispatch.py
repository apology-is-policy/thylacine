#!/usr/bin/env python3
"""Compile the actual HIN1 dispatcher and its named counterexamples.

A compiler error never counts as a rejected mutation. Run under a Mac lease.
"""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
modules = {
    'application': 'usr/halcyond/src/application.rs',
    'apprecord': 'usr/halcyond/src/apprecord.rs',
    'interaction_frame': 'usr/lib/libhalcyon/src/interaction_frame.rs',
    'hostbindings': 'usr/halcyond/src/hostbindings.rs',
    'paneroute': 'usr/halcyond/src/paneroute.rs',
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
    ('fresh-peer', 'application', 'if fresh != self.peer || !fresh.alive', 'if false', 'stale_fresh_peer_cannot_dispatch_or_read_cached_text'),
    ('check-peer-exit', 'application', 'if let Err(error) = self.peer(fresh)', 'if let Err(error) = Ok::<(), Failure>(())', 'peer_exit_before_check_completion_cannot_publish'),
    ('other-fid-budget', 'application', '.checked_sub(others)', '.checked_sub(0)', 'aggregate_fid_input_uses_transport_remainder_and_never_per_fid_quota'),
    ('wrong-target', 'application', 'if target == p.target =>', 'if true =>', 'wrong_completion_cannot_consume_pending_target'),
    ('lost-unbind-completion', 'application', 'if let Some(done) = retired { self.complete(Completion::Clipboard(done)); }', 'let _ = retired;', 'second_fid_unbind_completes_the_pending_admission_locally'),
    ('lost-cache-retirement', 'application', 'for f in self.fids.iter_mut().flatten() { f.record.cancel(); }', '', 'seat_retirement_drops_cache_and_partial_request_before_ack'),
    ('reused-fid', 'application', 'if id <= self.last_fid || route.incarnation == 0', 'if route.incarnation == 0', 'clunk_cancels_publication_and_late_receipt_cannot_bind_reused_fid'),
    ('forgotten-route', 'application', 'if self.route.is_some_and(|pinned| pinned != route)', 'if false', 'controller_route_remains_pinned_after_all_fids_clunk'),
    ('leaked-publish', 'application', '                owner.disconnect(self.peer.connection);', '', 'clunk_cancels_publication_and_late_receipt_cannot_bind_reused_fid'),
    ('ninth-fid', 'application', 'pub const FIDS: usize = 8;', 'pub const FIDS: usize = 9;', 'aggregate_fid_input_uses_transport_remainder_and_never_per_fid_quota'),
]

with tempfile.TemporaryDirectory(prefix='thylacine-application-dispatch-') as tmp:
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
        if test: args += ['application::tests::' + test, '--exact']
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
