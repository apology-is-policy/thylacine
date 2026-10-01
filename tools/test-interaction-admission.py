#!/usr/bin/env python3
"""Test the actual compositor admission body with controlled boundary adapters.

Also compile the actual connection-ID method and surface-incarnation advance,
and require named assertion failures for six deliberate admission mutations.
No syscall/runtime or graphical success is inferred from this host fixture.
"""
from pathlib import Path
import argparse
import json
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--out', type=Path)
args = parser.parse_args()
out = (args.out or Path(tempfile.mkdtemp(prefix='hi-admission-'))).resolve()
out.mkdir(parents=True, exist_ok=True)
fixture = (ROOT/'tools/host-tests/interaction-admission.rs').read_text()
fixture = fixture.replace('../../usr/', str(ROOT/'usr')+'/')
source = (ROOT/'usr/tapestryd/src/interaction.rs').read_text()
results = []
def run(name, body, selected=None, negative=False):
    src = out/(name+'.rs'); src.write_text(body)
    binary = out/(name+'-test')
    r = subprocess.run(['rustc','--edition=2021','--test',str(src),'-o',str(binary)], capture_output=True, text=True)
    (out/(name+'-compile.log')).write_text(r.stdout+r.stderr)
    assert r.returncode == 0, f'{name}: compilation failed'
    r = subprocess.run([str(binary)]+([selected,'--exact'] if selected else []), capture_output=True, text=True)
    (out/(name+'.log')).write_text(r.stdout+r.stderr)
    if negative:
        assert r.returncode != 0 and selected+' ... FAILED' in r.stdout and '1 failed' in r.stdout, name
    else:
        assert r.returncode == 0, name
    results.append(dict(name=name, exit=r.returncode, intended_failure=negative))
run('admission', fixture)
server = (ROOT/'usr/tapestryd/src/server.rs').read_text()
a = server.index('    pub fn next_conn_id(')
b = server.index('\n    }',a)+6
method = server[a:b]
a = server.index('    fn mint(')
b = server.index('        self.surfaces[n] = ',a)
advance = next(line.strip() for line in server[a:b].splitlines() if 'self.gen_seq =' in line)
run('identity', 'struct Comp {conn_seq:u64,gen_seq:u32}\nimpl Comp {\n'+method+'\nfn next_gen(&mut self)->Option<()> {\n'+advance+'\nSome(())}}\n'+'''
#[test] fn exhaustion_never_reuses_identity() {
 let mut c=Comp{conn_seq:u64::MAX-1,gen_seq:u32::MAX-1};
 assert_eq!(c.next_conn_id(),Some(u64::MAX));
 assert_eq!(c.next_conn_id(),None);assert_eq!(c.next_conn_id(),None);
 assert_eq!(c.next_gen(),Some(()));assert_eq!(c.gen_seq,u32::MAX);
 assert_eq!(c.next_gen(),None);assert_eq!(c.next_gen(),None);
 assert_eq!(c.gen_seq,u32::MAX);
}
''')
for name,old,new,selected in [
 ('host','if s.binder_pid != r.binder_pid {','if false {','only_declared_exact_surface_and_real_host'),
 ('focus','if self.layout.focused_surface() != Some(surface) {','if false {','background_and_wrong_context_never_reach_kernel_check'),
 ('failed-ack','b.context = None;\n            b.id.acknowledge','b.id.acknowledge','failed_replacement_revokes_old_context'),
 ('seat','if self.interaction_seat != Some(seat) {','if false {','seat_generation_change_between_loop_and_request_revokes_context'),
 ('surface','!= Some((b.surface, b.generation))','!= Some((b.surface, self.surface.gen))','surface_reuse_retires_watch_and_binding'),
 ('context','c.subject == r.subject && c.epoch < r.epoch','c.subject == r.subject','publish_generation_and_context_replay_refused'),
]:
    assert old in source, name
    mutated = out/(name+'-body.rs'); mutated.write_text(source.replace(old,new,1))
    run(name, fixture.replace(str(ROOT/'usr/tapestryd/src/interaction.rs'),str(mutated)),selected,True)
(out/'results.json').write_text(json.dumps(results,indent=2)+'\n')
print('PASS: admission and identity tests; six named counterexamples. Evidence:', out)
