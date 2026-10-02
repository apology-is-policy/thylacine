#!/usr/bin/env python3
"""Actual ordered wire/journal/channel tests and named counterexamples."""
from pathlib import Path
import subprocess,tempfile
root=Path(__file__).resolve().parent.parent
modules={
 'interaction_control':'usr/lib/libhalcyon/src/interaction_control.rs',
 'interaction_events':'usr/lib/libhalcyon/src/interaction_events.rs',
 'admission':'usr/lib/libtapestry/src/admission.rs',
 'ordered':'usr/lib/libtapestry/src/ordered.rs',
}
sources={k:(root/v).read_text() for k,v in modules.items()}
cases=[
 ('lost-local-rearm','ordered','self.selected && self.read.is_none() && self.record.is_none()','false','ordered::tests::local_rearm_and_retained_records_do_not_need_an_unrelated_wakeup'),
 ('blocked-decision-spins','ordered','!matches!(r.body, Body::Decision { .. })','true','ordered::tests::local_rearm_and_retained_records_do_not_need_an_unrelated_wakeup'),
 ('reject-initial-seat','interaction_events','|| w[2] == u64::MAX','|| w[2] == u64::MAX || w[3] == 0','interaction_events::tests::initial_normal_seat_generation_zero_is_valid'),
 ('overflow-overwrite','interaction_events','self.len == CAPACITY','false','interaction_events::tests::overflow_discards_history_and_never_recovers_in_place'),
 ('sequence-gap','interaction_events','r.sequence != self.next','false','interaction_events::tests::duplicate_gap_wrong_ready_and_exhaustion_poison'),
 ('receiver-recovers','interaction_events','if self.failed\n            || r.is_none_or','if false\n            || r.is_none_or','interaction_events::tests::duplicate_gap_wrong_ready_and_exhaustion_poison'),
 ('reserved-data','interaction_events','|| b[28..32] != [0; 4]','','interaction_events::tests::independent_little_endian_fixture_and_malformed_records'),
 ('early-decision','ordered','if self.write.is_some() || self.outbound.is_some() {','if false {','ordered::tests::parked_read_does_not_block_write_and_cqe_order_cannot_release_early'),
 ('unsolicited-decision','ordered','if !self.request.is_some_and','if false && !self.request.is_some_and','ordered::tests::short_eof_transport_and_wrong_decision_poison'),
 ('transport-recovers','ordered','self.failed = true;\n            self.record = None;','self.record = None;','ordered::tests::short_eof_transport_and_wrong_decision_poison'),
]
with tempfile.TemporaryDirectory(prefix='hi-ordered-') as tmp:
 out=Path(tmp)
 def run(name,mutation=None,test=None):
  wrapper='extern crate self as libhalcyon;\n'
  for module,path in modules.items():
   if mutation and module==mutation[0]:
    before,after=mutation[1:];assert sources[module].count(before)==1,(name,'ambiguous source match')
    p=out/(name+'-'+module+'.rs');p.write_text(sources[module].replace(before,after))
   else:p=root/path
   wrapper+=f'#[path="{p}"] pub mod {module};\n'
  entry=out/(name+'.rs');entry.write_text(wrapper);exe=out/name
  c=subprocess.run(['rustc','--edition=2021','--test',str(entry),'-o',str(exe)],capture_output=True,text=True)
  assert c.returncode==0,c.stdout+c.stderr
  r=subprocess.run([str(exe)]+([test,'--exact'] if test else []),capture_output=True,text=True)
  evidence=r.stdout+r.stderr
  if test:
   assert r.returncode!=0 and 'assertion' in evidence and '1 failed' in evidence,evidence
   print(name+': intended assertion failure at '+test,flush=True)
  else:assert r.returncode==0,evidence;print(evidence,flush=True)
 run('clean')
 for name,module,before,after,test in cases:run(name,(module,before,after),test)

# Compile the actual native selector identity predicate, with sampled peer data.
server=(root/'usr/tapestryd/src/server.rs').read_text()
a=server.index('    fn peer_is_declared_session(');b=server.index('\n    }',a)+6
method=server[a:b]
fixture='#[derive(Default,Clone,Copy)] struct TSrvPeerInfo {alive:u32,stripes:u64,principal_id:u32}\nthread_local! {static PEER:std::cell::Cell<TSrvPeerInfo>=std::cell::Cell::new(TSrvPeerInfo::default());}\nunsafe fn t_srv_peer(_:i64,out:&mut TSrvPeerInfo)->i64 {*out=PEER.with(|p|p.get());0}\nstruct Comp {declared:u64} impl Comp {fn session_declared(&self,id:u64)->bool {self.declared==id}}\nstruct Conn {handle:i64,conn_id:u64,peer_stripes:u64,peer_principal:u32}\nimpl Conn { METHOD }\n#[test] fn declared_session_is_not_console_renderer_and_rejects_identity_changes() {\n let c=Conn {handle:4,conn_id:3,peer_stripes:7,peer_principal:1000};let comp=Comp {declared:3};\n PEER.with(|p|p.set(TSrvPeerInfo{alive:1,stripes:7,principal_id:1000}));\n assert!(c.peer_is_declared_session(&comp));assert!(!c.peer_is_declared_session(&Comp{declared:99}));\n for peer in [TSrvPeerInfo{alive:0,stripes:7,principal_id:1000},TSrvPeerInfo{alive:1,stripes:8,principal_id:1000},TSrvPeerInfo{alive:1,stripes:7,principal_id:1001}] {\n  PEER.with(|p|p.set(peer));assert!(!c.peer_is_declared_session(&comp));\n }\n}\n'
with tempfile.TemporaryDirectory(prefix='hi-ordered-identity-') as tmp:
 p=Path(tmp)/'identity.rs';p.write_text(fixture.replace('METHOD',method));exe=Path(tmp)/'identity'
 subprocess.run(['rustc','--edition=2021','--test',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)
