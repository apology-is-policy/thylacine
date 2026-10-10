//! Private pooled-ring mapping owner. Uninstalled; native setup remains gated.
use core::{ptr::NonNull,sync::atomic::{AtomicU32,Ordering}};
use super::paired_cq::{PairedCq,Record};
#[repr(C)]
#[derive(Clone,Copy,Debug)]
pub(crate) struct Setup {
 pub flags:u32,pub sq_entries:u32,pub cq_entries:u32,pub ring_size:u32,pub ring_va:u64,
 pub hdr_off:u32,pub sq_array_off:u32,pub sqe_off:u32,pub cqe_off:u32,
 pub sq_array_size:u32,pub sqe_size:u32,pub cqe_size:u32,pub reserved:u32,pub receipt:[u64;4],
}
const _:()=assert!(core::mem::size_of::<Setup>()==88);
const _:()=assert!(core::mem::offset_of!(Setup,ring_va)==16);
const _:()=assert!(core::mem::offset_of!(Setup,receipt)==56);
#[derive(Debug,PartialEq,Eq)]pub(crate) enum Error{Geometry,Header,Closed,Corrupt}
/// # Safety
/// Sole owner of a fresh private ring descriptor and stable writable mapping.
/// Truthful base/len, no escaped legacy reap/submit, geometry writers or mapping
/// aliases. Kernel publishes CQ+receipt with release-tail and respects head.
/// close is bounded/idempotent and closes the owned descriptor once; no callback
/// reentry/panic. Drop unmaps only the originally owned mapping and preserves
/// independent kernel pins/charges. Never detach using unvalidated setup output.
pub(crate) unsafe trait Mapping {
 fn base(&self)->NonNull<u8>;
 fn len(&self)->usize;
 fn close(&mut self);
}
fn geometry(p:Setup,base:usize,len:usize)->Result<(),Error>{
 if p.flags!=12||p.reserved!=0||p.receipt[3]!=0||p.receipt[2]!=32||base==0||base%4096!=0||
    len==0||len>isize::MAX as usize||base.checked_add(len).is_none()||p.ring_va!=base as u64||p.ring_size as usize!=len||len%4096!=0||
    p.sq_entries==0||!p.sq_entries.is_power_of_two()||p.sq_entries>4096||
    p.cq_entries<p.sq_entries||!p.cq_entries.is_power_of_two()||p.cq_entries>8192{return Err(Error::Geometry);}
 if p.sq_array_size!=p.sq_entries*4||p.sqe_size!=p.sq_entries*64||p.cqe_size!=p.cq_entries*16||p.receipt[1]!=p.cq_entries as u64*32{return Err(Error::Geometry);}
 let starts=[p.hdr_off as u64,p.sq_array_off as u64,p.sqe_off as u64,p.cqe_off as u64,p.receipt[0]];
 let sizes=[64,p.sq_array_size as u64,p.sqe_size as u64,p.cqe_size as u64,p.receipt[1]];
 for i in 0..5{
  let end=starts[i].checked_add(sizes[i]).ok_or(Error::Geometry)?;
  if starts[i]%64!=0||end>len as u64{return Err(Error::Geometry);}
  for j in 0..i{if starts[i]<starts[j]+sizes[j]&&starts[j]<end{return Err(Error::Geometry);}}
 }
 if p.receipt[0]<p.cqe_off as u64+p.cqe_size as u64{return Err(Error::Geometry);}
 Ok(())
}
/// Owns the only CQ consumer and its mapping; no raw address or descriptor API.
pub(crate) struct OwnedMapping<M:Mapping>{reader:PairedCq,mapping:M,closed:bool}
impl<M:Mapping> OwnedMapping<M>{
 pub(crate) fn new(mut mapping:M,p:Setup)->Result<Self,Error>{
  let base=mapping.base().as_ptr();let len=mapping.len();
  let make=||->Result<PairedCq,Error>{
   geometry(p,base as usize,len)?;
   // All pointer ranges have been validated, including SQ-vs-CQ overlap. These
   // setup-only mirrors never become authoritative masks after construction.
   let word=|n:usize|unsafe{&*base.add(p.hdr_off as usize+n*4).cast::<AtomicU32>()}.load(Ordering::Acquire);
   if word(0)!=0||word(1)!=0||word(2)!=p.sq_entries-1||word(3)!=p.sq_entries||
      word(4)!=0||word(5)!=0||word(6)!=p.cq_entries-1||word(7)!=p.cq_entries||word(8)&!1!=0||word(9)!=0||word(10)!=0{return Err(Error::Header);}
   unsafe{PairedCq::new(base,len,p.hdr_off as usize,p.cqe_off as usize,p.receipt[0]as usize,p.cq_entries)}.map_err(|_|Error::Geometry)
  };
  match make(){Ok(reader)=>Ok(Self{reader,mapping,closed:false}),Err(e)=>{mapping.close();Err(e)}}
 }
 pub(crate) fn reap(&mut self)->Result<Option<Record>,Error>{if false && self.closed{return Err(Error::Closed);}self.reader.reap().map_err(|_|Error::Corrupt)}
 pub(crate) fn close(&mut self){if !self.closed{self.closed=true;self.mapping.close();}}
}
impl<M:Mapping> Drop for OwnedMapping<M>{fn drop(&mut self){self.close();}}
