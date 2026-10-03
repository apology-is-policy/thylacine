#!/usr/bin/env python3
"""Actual host-binding state machine, including named counterexamples."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
modules = {
    'interaction_control': 'usr/lib/libhalcyon/src/interaction_control.rs',
    'paneroute': 'usr/halcyond/src/paneroute.rs',
    'hostbindings': 'usr/halcyond/src/hostbindings.rs',
}
source = (root / modules['hostbindings']).read_text()
cases = [
    ('lost-remote-cleanup', 'e.remote = true', 'e.remote = false',
     'late_success_keeps_cleanup_and_replacement_cannot_bypass_it'),
    ('retirement-revival', 'result.is_ok() && !f.retired', 'result.is_ok()',
     'retirement_before_success_cannot_revive_or_rebind'),
    ('refusal-spin', '&& e.attempted != Some((Op::Bind, seat))', '',
     'refusal_is_bounded_and_suspend_keeps_remote_observer'),
    ('permission-as-absence', 'result == Err(2)', 'result == Err(1)',
     'late_success_keeps_cleanup_and_replacement_cannot_bypass_it'),
    ('id-only-completion', 'if f.request != request', 'if f.request.request != request.request',
     'exact_completion_and_unrelated_retirement'),
    ('leaf-only-retirement', 'e.host.binding == binding', 'true',
     'exact_completion_and_unrelated_retirement'),
    ('skip-incarnation', '!routes.current(host.route)', 'false',
     'exact_host_metadata_and_removal_at_capacity'),
    ('overwrite-live-host', 'return *old == host;', 'return true;',
     'exact_host_metadata_and_removal_at_capacity'),
]
with tempfile.TemporaryDirectory(prefix='thyla-host-bindings-') as tmp:
    out = Path(tmp)
    def run(name, change=None, test=None):
        wrapper = 'extern crate alloc;\nextern crate self as libhalcyon;\n'
        for module, path in modules.items():
            file = root / path
            if module == 'hostbindings' and change:
                old, new = change
                assert source.count(old) == 1, (name, 'ambiguous mutation')
                file = out / (name + '-module.rs')
                file.write_text(source.replace(old, new))
            wrapper += f'#[path="{file}"] mod {module};\n'
        entry = out / (name + '.rs'); entry.write_text(wrapper)
        exe = out / name
        build = subprocess.run(['rustc', '--edition=2021', '--test', str(entry), '-o', str(exe)], capture_output=True, text=True)
        assert build.returncode == 0, build.stdout + build.stderr
        args = [str(exe)]
        if test: args += ['hostbindings::tests::' + test, '--exact']
        result = subprocess.run(args, capture_output=True, text=True)
        evidence = result.stdout + result.stderr
        if test:
            assert result.returncode != 0 and 'assertion' in evidence and '1 failed' in evidence, evidence
            print(name + ': intended assertion failure at ' + test, flush=True)
        else:
            assert result.returncode == 0, evidence
            print(evidence, flush=True)
    run('clean')
    for name, old, new, test in cases: run(name, (old, new), test)
