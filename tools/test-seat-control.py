#!/usr/bin/env python3
"""Actual-source seat/route schedules and nine named counterexamples.

Run from Astra's resource-leased checkout. No source edits or persistent model
state: extracted production definitions and mutants live in a temporary directory.
A compiler error is never an intended mutant success.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent

def compile_run(out, name, source, test=None, expected_failure=None):
    src = out / (name + '.rs')
    exe = out / name
    src.write_text(source)
    built = subprocess.run(['rustc', '--edition=2021', '--test', str(src), '-o', str(exe)], capture_output=True, text=True)
    assert built.returncode == 0, built.stdout + built.stderr
    command = [str(exe)] + (['tests::' + test, '--exact'] if test else [])
    result = subprocess.run(command, capture_output=True, text=True)
    text = result.stdout + result.stderr
    if expected_failure:
        assert result.returncode != 0 and expected_failure in text, text
        print(name + ': intended assertion failure')
    else:
        assert result.returncode == 0, text
        print(name + ': ' + next(line for line in text.splitlines() if line.startswith('test result:')))

c=(root/'usr/lib/libhalcyon/src/seat_control.rs').read_text()
g=(root/'usr/lictor/src/quiescence.rs').read_text()
cases=[
 ('skip-member',c,'|| self.members.iter().flatten().any(|m| m.potential)','|| false','every_visible_and_hidden_member_must_cancel'),
 ('eof-is-cancel',c,'m.lost = true;','m.lost = true; m.potential = false;','notification_and_eof_do_not_discharge_obligation'),
 ('stale-generation',c,'if self.phase != QUIESCING\n            || generation != self.generation','if self.phase != QUIESCING','exact_peer_lane_generation_and_revision_required'),
 ('wrong-lane',c,'|| m.lane != Some(lane)','|| false','exact_peer_lane_generation_and_revision_required'),
 ('wrong-stripes',c,'|| m.peer.stripes != stripes','|| false','exact_peer_lane_generation_and_revision_required'),
 ('drop-on-normal-eof',c,'v.enabled = false;','v.enabled = false; v.potential = false;','declaration_loss_keeps_live_obligation_and_prevents_rejoin'),
 ('gate-without-ack',g,'&& self.acknowledged','&& true','exact_generation_and_quiescing_required'),
 ('gate-replay',g,'self.acknowledged = false;','/* stale acknowledgement survives */','no_replay_across_episode_failure_or_restoration'),
 ]
s=(root/'usr/halcyond/src/paneplace.rs').read_text()
body=s[s.index('pub struct PaneCompletedImage {'):s.index('#[derive(Copy, Clone)]\nstruct Fid')]+s[s.index('const _: () = assert!(core::mem::size_of::<PaneCompletedImage>'):s.index('// The desired route table')]
body = 'extern crate alloc;\n#[path="'+str(root/'usr/halcyond/src/paneroute.rs')+'"] mod paneroute;\nuse paneroute::{Route,Routes};\n'+body
tests='''
#[test] fn exact_route_survives_but_replacement_does_not() {
 let mut r=Routes::empty(); assert!(r.insert(10,7));
 let image=PaneCompletedImage {route:*r.get(&10).unwrap(),id:55,leaf:7,w:1,h:1,argb:vec![1]};
 assert!(completion_route_current(&r,&image));
 r.remove_leaf(7);assert!(r.insert(10,7));
 assert!(!completion_route_current(&r,&image), "retired token reached replacement tile");
}
#[test] fn full_metadata_cannot_drop_revocation() {
 let mut r=Routes::empty();for i in 1..=32 {assert!(r.insert(i,i as u32));}
 assert!(!r.insert(33,33));r.remove_leaf(10);assert!(!r.contains_key(&10));
 assert!(r.insert(33,10));assert_eq!(r.get(&33).map(|r|r.leaf),Some(10));assert!(!r.contains_key(&10));
}
'''

with tempfile.TemporaryDirectory(prefix='thylacine-seat-') as directory:
    out=Path(directory)
    compile_run(out, 'coordinator', c)
    compile_run(out, 'lictor-gate', g)
    compile_run(out, 'media-route', body+tests)
    for name, source, before, after, test in cases:
        assert source.count(before) == 1, name
        compile_run(out, name, source.replace(before,after), test, 'assertion')
    before='image.leaf == image.route.leaf && routes.current(image.route)'
    assert body.count(before) == 1
    compile_run(out, 'leaf-only', body.replace(before, 'true')+tests,
                expected_failure='retired token reached replacement tile')
