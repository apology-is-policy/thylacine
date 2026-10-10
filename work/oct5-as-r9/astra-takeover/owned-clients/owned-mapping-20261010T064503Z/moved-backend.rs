#![allow(dead_code)]
#[path="/Users/northkillpd/projects/thylacine-corona/usr/lib/libthyla-rs/src/loom/service_abi.rs"] mod service_abi;
mod paired_cq;mod owned_mapping;
use owned_mapping::*;
use std::{alloc::{alloc_zeroed,dealloc,Layout},cell::Cell,rc::Rc,ptr::NonNull,sync::atomic::{AtomicU32,Ordering}};
#[derive(Default)]struct Probe{closed:Cell<usize>,freed:Cell<usize>}
struct Backing{base:NonNull<u8>,len:usize,probe:Rc<Probe>}
unsafe impl Mapping for Backing{fn base(&self)->NonNull<u8>{self.base}fn len(&self)->usize{self.len}fn close(&mut self){self.probe.closed.set(self.probe.closed.get()+1);}}
impl Drop for Backing{fn drop(&mut self){assert_eq!(self.probe.closed.get(),1);self.probe.freed.set(self.probe.freed.get()+1);unsafe{dealloc(self.base.as_ptr(),Layout::from_size_align(self.len,4096).unwrap())}}}
fn align(v:u32,a:u32)->u32{(v+a-1)&!(a-1)}
fn fixture(sq:u32,cq:u32)->(Backing,Setup,Rc<Probe>){
 let sqe=align(64+sq*4,64);let cqe=align(sqe+sq*64,64);let receipt=align(cqe+cq*16,64);let len=align(receipt+cq*32,4096)as usize;
 let base=NonNull::new(unsafe{alloc_zeroed(Layout::from_size_align(len,4096).unwrap())}).unwrap();let probe=Rc::new(Probe::default());
 let p=Setup{flags:12,sq_entries:sq,cq_entries:cq,ring_size:len as u32,ring_va:base.as_ptr()as u64,hdr_off:0,sq_array_off:64,sqe_off:sqe,cqe_off:cqe,sq_array_size:sq*4,sqe_size:sq*64,cqe_size:cq*16,reserved:0,receipt:[receipt as u64,cq as u64*32,32,0]};
 for (i,v)in [(2,sq-1),(3,sq),(6,cq-1),(7,cq)]{unsafe{(*base.as_ptr().cast::<AtomicU32>().add(i)).store(v,Ordering::Relaxed)}}
 (Backing{base,len,probe:probe.clone()},p,probe)
}
#[test]fn min_max_copy_and_cleanup(){for(sq,cq)in [(1,1),(2,2),(4096,8192)]{
 let(b,p,q)=fixture(sq,cq);let raw=b.base.as_ptr();let mut m=OwnedMapping::new(b,p).unwrap();assert!(m.reap().unwrap().is_none());
 unsafe{raw.add(p.cqe_off as usize).cast::<paired_cq::Completion>().write(paired_cq::Completion{user_data:77,result:8,flags:4});raw.add(p.receipt[0]as usize).cast::<service_abi::BufferReceipt>().write(service_abi::BufferReceipt{pool:service_abi::SlotRef{slot:3,reserved:0,incarnation:9},member:2,reserved:0,lease:42});(*raw.cast::<AtomicU32>().add(5)).store(1,Ordering::Release);}
 let r=m.reap().unwrap().unwrap();assert_eq!(r.cqe.user_data,77);assert_eq!(r.receipt.lease,42);assert_eq!(unsafe{(*raw.cast::<AtomicU32>().add(4)).load(Ordering::Acquire)},1);
 m.close();m.close();assert!(matches!(m.reap(),Err(Error::Closed)));assert_eq!(q.closed.get(),1);assert_eq!(q.freed.get(),0);drop(m);assert_eq!(q.freed.get(),1);assert_eq!(r.receipt.lease,42);
}}
#[test]fn malformed_geometry_cleanup(){for case in 0..23{
 let(b,mut p,q)=fixture(4,8);match case{0=>p.flags=4,1=>p.reserved=1,2=>p.receipt[3]=1,3=>p.receipt[2]=16,4=>p.ring_va+=4096,5=>p.ring_size-=1,6=>p.sq_entries=3,7=>p.sq_entries=8192,8=>p.cq_entries=2,9=>p.cq_entries=7,10=>p.cq_entries=16384,11=>p.sq_array_size+=4,12=>p.sqe_size+=64,13=>p.cqe_size+=16,14=>p.receipt[1]+=32,15=>p.hdr_off=1,16=>p.sq_array_off=1,17=>p.sqe_off=p.ring_size,18=>p.cqe_off=p.ring_size,19=>p.receipt[0]=u64::MAX-31,20=>p.sqe_off=p.cqe_off,21=>p.sq_array_off=p.cqe_off,22=>p.receipt[0]=0,_=>unreachable!()}
 assert!(matches!(OwnedMapping::new(b,p),Err(Error::Geometry)),"case {case}");assert_eq!(q.closed.get(),1);assert_eq!(q.freed.get(),1);
}}
#[test]fn every_region_pair_rejects_overlap(){for i in 0..5{for j in 0..i{
 let(b,mut p,q)=fixture(4,8);let starts=[p.hdr_off,p.sq_array_off,p.sqe_off,p.cqe_off,p.receipt[0]as u32];match i{1=>p.sq_array_off=starts[j],2=>p.sqe_off=starts[j],3=>p.cqe_off=starts[j],4=>p.receipt[0]=starts[j]as u64,_=>unreachable!()};assert!(matches!(OwnedMapping::new(b,p),Err(Error::Geometry)));assert_eq!(q.freed.get(),1);
}}}
#[test]fn fresh_header_checks_and_wakeup(){for i in 0..11{let(b,p,q)=fixture(4,8);unsafe{let h=&*b.base.as_ptr().cast::<AtomicU32>().add(i);h.fetch_xor(if i==8{2}else{1},Ordering::Relaxed);};assert!(matches!(OwnedMapping::new(b,p),Err(Error::Header)),"header {i}");assert_eq!(q.freed.get(),1);}
 let(b,p,q)=fixture(4,8);unsafe{(*b.base.as_ptr().cast::<AtomicU32>().add(8)).store(1,Ordering::Relaxed)};drop(OwnedMapping::new(b,p).unwrap());assert_eq!(q.freed.get(),1);
}
#[test]fn post_setup_corruption_is_detected(){let(b,p,q)=fixture(4,8);let raw=b.base.as_ptr();let mut m=OwnedMapping::new(b,p).unwrap();unsafe{(*raw.cast::<AtomicU32>().add(5)).store(9,Ordering::Release)};assert!(matches!(m.reap(),Err(Error::Corrupt)));drop(m);assert_eq!(q.freed.get(),1);}

fn main(){let(b,p,_q)=fixture(4,8);let mut m=OwnedMapping::new(b,p).unwrap();let _n=b.len();}
