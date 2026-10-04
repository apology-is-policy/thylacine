#!/usr/bin/env python3
"""Actual WeftFlow getters/wait methods with controlled enter/reap failures.
Host doubles cover only syscall completion, not Weft's transport ownership.
"""
from pathlib import Path
import argparse,subprocess,tempfile
ROOT=Path(__file__).resolve().parent.parent
def method(s,name):
 start=s.index('    pub fn '+name+'(');br=s.index('{',start);depth=1;i=br+1
 while depth:
  if s[i]=='{':depth+=1
  elif s[i]=='}':depth-=1
  i+=1
 return s[start:i]
STUBS='''#![allow(dead_code)]
#[derive(Debug,PartialEq)] enum Error { WouldBlock, InvalidArgument }
type Result<T> = core::result::Result<T,Error>;
mod loom {pub const ENTER_GETEVENTS:u32=1;}
struct Ticket(u64);
#[derive(Clone,Copy)] struct Inflight {tok:u64,recv:bool}
struct Completion {bytes:usize,recv:bool}
struct Cqe; impl Cqe {fn ok(self)->Result<i32> {Ok(1)}}
struct Ring {enter_fails:bool}
impl Ring {fn enter(&self,_:u32,_:u32,_:u32)->Result<i64> {if self.enter_fails {Err(Error::InvalidArgument)} else {Ok(1)}}}
struct Geom {payload_off:u32,payload_size:u32}
struct WeftFlow {ring:Ring,ring_va:u64,geom:Geom,inflight:Option<Inflight>}
impl WeftFlow {
fn reap_token(&self,_:u64)->Result<Cqe> {Err(Error::InvalidArgument)}
'''
TEST='''
}
fn main() {
 let mut bytes=[0u8;8];
 let mut f=WeftFlow {ring:Ring{enter_fails:false},ring_va:bytes.as_mut_ptr() as u64,
 geom:Geom{payload_off:0,payload_size:8},inflight:None};
 f.tx_buf().unwrap()[0]=21;
 assert_eq!(f.rx_buf().unwrap()[0],21);
 for enter_fails in [true,false] {
  f.ring.enter_fails=enter_fails; f.inflight=Some(Inflight{tok:1,recv:true});
  assert!(f.wait(Ticket(1)).is_err());
  assert!(f.inflight.is_some(),"wait error retains ownership");
  assert!(matches!(f.tx_buf(),Err(Error::WouldBlock)),"pending write borrow rejected");
  assert!(matches!(f.rx_buf(),Err(Error::WouldBlock)),"pending read borrow rejected");
 }
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 s=(ROOT/'usr/lib/libthyla-rs/src/net.rs').read_text();s=s[s.index('impl WeftFlow {'):];methods={n:method(s,n) for n in ['tx_buf','rx_buf','wait']}
 with tempfile.TemporaryDirectory(prefix='weft-borrow-') as tmp:
  for name,target,want in [('clean',None,None),('tx-unguarded','tx_buf','pending write borrow rejected'),('rx-unguarded','rx_buf','pending read borrow rejected')]:
   m=dict(methods)
   if target:
    old='if self.inflight.is_some() { return Err(Error::WouldBlock); }';assert old in m[target];m[target]=m[target].replace(old,'')
   src=Path(tmp)/(name+'.rs');src.write_text(STUBS+'\n'.join(m.values())+TEST);exe=src.with_suffix('')
   r=subprocess.run(['rustc','--edition=2021','--crate-name','weft_borrow',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=60);(a.logs/(name+'-build.log')).write_text(r.stdout+r.stderr);assert r.returncode==0,r.stderr
   r=subprocess.run([str(exe)],capture_output=True,text=True,timeout=10);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode!=0 and want in r.stderr,r.stderr
   else:assert r.returncode==0,r.stderr
   print('PASS',name,want or 'actual getters refuse enter/reap failure borrows',flush=True)
if __name__=='__main__':main()
