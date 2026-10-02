#!/usr/bin/env python3
"""Actual route table and 9P media protocol, with named lifetime counterexamples."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parent.parent
route = (root/'usr/halcyond/src/paneroute.rs').read_text()
s = (root/'usr/halcyond/src/paneplace.rs').read_text()
# Extract syscall-free production definitions, not a second protocol model.
body = s[s.index('const SRV_MSIZE:'):s.index('// The accepted endpoint')]
body += s[s.index('struct Protocol {'):s.index('/// Once setup may have published')]
body += s[s.index('const _: () = assert!(core::mem::size_of::<PaneCompletedImage>'):s.index('// The desired route table')]
tests = r'''
#[cfg(test)] mod protocol_tests {
use super::*;
fn call(p: &mut Protocol, r: &Routes, kind:u8, data:&[u8]) -> Vec<u8> {
 let mut frame = vec![0;7]; frame[4]=kind; frame[5]=23;
 frame.extend_from_slice(data); let n=frame.len() as u32; frame[..4].copy_from_slice(&n.to_le_bytes());
 let mut out=Vec::new();
 let d=p.dispatch(&frame,p9::peek_header(&frame).unwrap(),&mut out,r,
  Budget{max_pixels:64,others_reserved:0,residual_bytes:512,completion_slots:2},&mut Diag::default());
 match d { Disp::Reply(n)=>p.out_buf[..n].to_vec(), Disp::Fatal=>panic!("fatal") }
}
fn walk(p:&mut Protocol,r:&Routes,from:u32,to:u32,names:&[&[u8]]) -> Vec<u8> {
 let mut b=from.to_le_bytes().to_vec();b.extend_from_slice(&to.to_le_bytes());b.extend_from_slice(&(names.len() as u16).to_le_bytes());
 for name in names {b.extend_from_slice(&(name.len() as u16).to_le_bytes());b.extend_from_slice(name);}
 call(p,r,p9::P9_TWALK,&b)
}
fn setup() -> (Protocol,Routes) {
 let mut r=Routes::empty();assert!(r.insert(10,7));let mut p=Protocol::new();p.version_done=true;
 assert!(p.fid_set(1,Node::Root,None));
 assert_eq!(walk(&mut p,&r,1,2,&[&paneroute::hex32(10)])[4],p9::P9_RWALK);
 assert_eq!(walk(&mut p,&r,2,3,&[b"place"])[4],p9::P9_RWALK);
 (p,r)
}
fn open(p:&mut Protocol,r:&Routes) -> Vec<u8> {
 let mut b=3u32.to_le_bytes().to_vec();b.extend_from_slice(&2u32.to_le_bytes());call(p,r,p9::P9_TLOPEN,&b)
}
fn write(p:&mut Protocol,r:&Routes,offset:u64,data:&[u8]) -> Vec<u8> {
 let mut b=3u32.to_le_bytes().to_vec();b.extend_from_slice(&offset.to_le_bytes());b.extend_from_slice(&(data.len() as u32).to_le_bytes());b.extend_from_slice(data);call(p,r,p9::P9_TWRITE,&b)
}
fn noent(reply:Vec<u8>) {assert_eq!(reply[4],p9::P9_RLERROR);assert_eq!(u32::from_le_bytes(reply[7..11].try_into().unwrap()),p9::E_NOENT);}
#[test] fn reused_name_cannot_reopen_clone_walk_or_stat_old_fids() {
 let (mut p,mut r)=setup();r.remove_leaf(7);assert!(r.insert(10,7));
 noent(open(&mut p,&r));
 noent(walk(&mut p,&r,2,4,&[]));
 noent(walk(&mut p,&r,2,4,&[b".."]));
 noent(walk(&mut p,&r,2,4,&[b"place"]));
 let mut b=3u32.to_le_bytes().to_vec();b.extend_from_slice(&u64::MAX.to_le_bytes());
 noent(call(&mut p,&r,p9::P9_TGETATTR,&b));
 assert_eq!(call(&mut p,&r,p9::P9_TCLUNK,&3u32.to_le_bytes())[4],p9::P9_RCLUNK);
 assert_eq!(walk(&mut p,&r,1,3,&[&paneroute::hex32(10),b"place"])[4],p9::P9_RWALK);
 assert_eq!(open(&mut p,&r)[4],p9::P9_RLOPEN);
}
#[test] fn revoked_partial_upload_is_freed_and_never_retargeted() {
 let (mut p,mut r)=setup();assert_eq!(open(&mut p,&r)[4],p9::P9_RLOPEN);
 let mut h=inlinewire::PlaceHeader::argb(1,1);h.id=9;
 assert_eq!(write(&mut p,&r,0,&h.pack())[4],p9::P9_RWRITE);
 assert!(p.reserved()>0);r.remove_leaf(7);assert!(r.insert(10,7));
 noent(write(&mut p,&r,32,&[0;4]));assert_eq!(p.reserved(),0);
 let mut b=3u32.to_le_bytes().to_vec();b.extend_from_slice(&0u64.to_le_bytes());b.extend_from_slice(&64u32.to_le_bytes());
 noent(call(&mut p,&r,p9::P9_TREAD,&b));
}
#[test] fn completion_is_pinned_even_if_token_and_leaf_are_reused() {
 let (_,mut r)=setup();let image=PaneCompletedImage{route:*r.get(&10).unwrap(),leaf:7,id:9,w:1,h:1,argb:vec![0]};
 assert!(completion_route_current(&r,&image));r.remove_leaf(7);assert!(r.insert(10,7));
 assert!(!completion_route_current(&r,&image));
}
}
'''
cases=[
 ('retarget-live', 'route','return old.leaf == leaf','return true','paneroute::tests::live_route_cannot_be_retargeted'),
 ('duplicate-leaf', 'route','if self.slots.iter().flatten().any(|r| r.leaf == leaf)', 'if false','paneroute::tests::live_route_cannot_be_retargeted'),
 ('reuse-incarnation', 'route','self.next = next;', 'let _ = next;', 'paneroute::tests::coalesced_replacement_retires_even_identical_names'),
 ('name-only-fid', 'route','token == r.token && self.current(r)', 'token == r.token && self.contains_key(&token)', 'protocol_tests::reused_name_cannot_reopen_clone_walk_or_stat_old_fids'),
 ('lost-retirement', 'route','if !self.current(r) { retire(r); }','if false { retire(r); }','paneroute::tests::coalesced_replacement_retires_even_identical_names'),
 ('name-only-completion', 'body','image.leaf == image.route.leaf && routes.current(image.route)','routes.contains_key(&image.route.token)','protocol_tests::completion_is_pinned_even_if_token_and_leaf_are_reused'),
]
with tempfile.TemporaryDirectory(prefix='thyla-route-') as tmp:
 out=Path(tmp)
 def build(args):
  r=subprocess.run(['rustc','--edition=2021',*args],capture_output=True,text=True);assert r.returncode==0,r.stdout+r.stderr
 inline=out/'libinlinewire.rlib';build(['--crate-type=lib','--crate-name=inlinewire',str(root/'usr/lib/inlinewire/src/lib.rs'),'-o',str(inline)])
 def run(name, area=None, before=None, after=None, test=None):
  rsrc,bsrc=route,body
  if area:
   src={'route':route,'body':body}[area];assert src.count(before)==1,(name,'ambiguous mutation')
   if area=='route':rsrc=src.replace(before,after)
   else:bsrc=src.replace(before,after)
  rp=out/(name+'-route.rs');rp.write_text(rsrc)
  prefix='extern crate alloc;\nuse alloc::vec::Vec;\n'
  for module,path in [('paneroute',rp),('p9',root/'usr/lib/ninep/src/lib.rs'),('inlineaccum',root/'usr/halcyond/src/inlineaccum.rs'),('servicewire',root/'usr/halcyond/src/servicewire.rs')]:
   prefix+=f'#[path="{path}"] mod {module};\n'
  prefix+='extern crate self as halcyond;\nuse paneroute::{Node,Quiet,Route,Routes};\nuse inlineaccum::{AccumStep,PlaceAccum};\n'
  src=out/(name+'.rs');src.write_text(prefix+bsrc+tests);exe=out/name
  build(['--test',str(src),'--extern','inlinewire='+str(inline),'-o',str(exe)])
  r=subprocess.run([str(exe)]+([test,'--exact'] if test else []),capture_output=True,text=True);text=r.stdout+r.stderr
  if test:
   assert r.returncode!=0 and 'assertion' in text and '1 failed' in text,text
   print(name+': intended assertion failure at '+test,flush=True)
  else:
   assert r.returncode==0,text;print(text,flush=True)
 run('clean')
 for case in cases:run(*case)
