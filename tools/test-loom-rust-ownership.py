#!/usr/bin/env python3
"""Compile actual Loom Rust module with host syscall/handle doubles.
Exercise range access and insist safe raw registration/invalid borrows fail.
Native builds validate real platform dependencies separately. Hold a Yip lease.
"""
from pathlib import Path
import argparse,subprocess,tempfile,shutil
ROOT=Path(__file__).resolve().parent.parent
STUBS=r'''
#![allow(dead_code)]
mod err {
 #[derive(Debug)] pub enum Error { InvalidArgument, WouldBlock }
 pub type Result<T> = core::result::Result<T,Error>;
 impl From<i32> for Error { fn from(_:i32)->Self { Self::InvalidArgument } }
}
mod handle {
 #[derive(Copy,Clone)] pub struct Rights;
 impl Rights { pub const READ:Self=Self; pub const WRITE:Self=Self; }
 impl core::ops::BitOr for Rights { type Output=Self; fn bitor(self,_:Self)->Self {self} }
 pub struct Handle(i32);
 impl Handle { pub fn from_raw(n:i32,_:Rights)->Self {Self(n)} pub fn raw(&self)->i32 {self.0} }
}
unsafe fn t_burrow_attach(n:u64)->i64 { std::alloc::alloc_zeroed(std::alloc::Layout::from_size_align(n as usize,8).unwrap()) as i64 }
unsafe fn t_burrow_detach(p:u64,n:u64)->i64 { std::alloc::dealloc(p as *mut u8,std::alloc::Layout::from_size_align(n as usize,8).unwrap()); 0 }
unsafe fn t_loom_setup(_:u64,_:u64)->i64 {-1}
unsafe fn t_loom_register(_:u64,_:u64,_:u64,_:u64)->i64 {0}
unsafe fn t_loom_enter(_:u64,_:u64,_:u64,_:u64)->i64 {0}
mod loom;
'''
CASES={
 'raw-safe':('fn bad(r:&loom::Ring,b:&[loom::BufReg]) { let _=r.register_buffers(b); }\nfn main() {}','E0133'),
 'range-alias':('fn bad(b:&mut loom::RegisteredBuffer) { let r=b.as_slice_range(0..1).unwrap(); let _w=b.as_mut_range(1..2); std::hint::black_box(r); }\nfn main() {}','E0502'),
 'range-drop':('fn bad(b:loom::RegisteredBuffer) { let r=b.as_slice_range(0..1).unwrap(); drop(b); std::hint::black_box(r); }\nfn main() {}','E0505'),
}
CLEAN=r'''
fn main() {
 let mut b=loom::RegisteredBuffer::new(16).unwrap();
 b.as_mut_range(2..4).unwrap().copy_from_slice(&[17,29]);
 assert_eq!(b.as_slice_range(2..4),Some(&[17,29][..]));
 assert_eq!(b.as_slice_range(0..2),Some(&[0,0][..]));
 assert!(b.as_slice_range(16..16).unwrap().is_empty());
 assert!(b.as_slice_range(2..1).is_none());
 assert!(b.as_mut_range(0..17).is_none());
 assert!(b.as_slice_range(usize::MAX..usize::MAX).is_none());
}
'''
def main():
 p=argparse.ArgumentParser();p.add_argument('--logs',type=Path,required=True);a=p.parse_args();a.logs.mkdir(parents=True,exist_ok=True)
 with tempfile.TemporaryDirectory(prefix='loom-rust-ownership-') as tmp:
  t=Path(tmp);shutil.copyfile(ROOT/'usr/lib/libthyla-rs/src/loom.rs',t/'loom.rs');(t/'loom').mkdir();shutil.copyfile(ROOT/'usr/lib/libthyla-rs/src/loom/service_abi.rs',t/'loom/service_abi.rs')
  for name,(body,want) in {'clean':(CLEAN,None),**CASES}.items():
   src=t/(name+'.rs');src.write_text(STUBS+body);exe=t/name
   r=subprocess.run(['rustc','--edition=2021','--crate-name','ownership',str(src),'-o',str(exe)],capture_output=True,text=True,timeout=60);(a.logs/(name+'.log')).write_text(r.stdout+r.stderr)
   if want:assert r.returncode!=0 and f'error[{want}]' in r.stderr,(name,r.stderr)
   else:
    assert r.returncode==0,r.stderr
    subprocess.run([str(exe)],check=True,timeout=10)
   print('PASS',name,want or 'actual range accesses',flush=True)
if __name__=='__main__':main()
