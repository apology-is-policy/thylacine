#![allow(dead_code)]
#[path="/Users/northkillpd/projects/thylacine-corona/usr/lib/libthyla-rs/src/loom/service_abi.rs"] mod service_abi;
mod paired_cq;
use paired_cq::*;
use service_abi::*;
use std::{cell::UnsafeCell,sync::{Arc,atomic::{AtomicU32,Ordering}},thread};
const ZERO:BufferReceipt=BufferReceipt{pool:SlotRef{slot:0,reserved:0,incarnation:0},member:0,reserved:0,lease:0};
#[repr(C,align(8))]
struct Memory{header:[AtomicU32;16],cq:[UnsafeCell<Completion>;4],receipts:[UnsafeCell<BufferReceipt>;4]}
// Exactly one producer/consumer; release-tail/acquire-tail and release-head/
// acquire-head protect UnsafeCells. Test never shares mutable payload references.
unsafe impl Sync for Memory{}
impl Memory {
 fn new(cursor:u32)->Arc<Self>{let m=Arc::new(Self{header:std::array::from_fn(|_|AtomicU32::new(0)),cq:std::array::from_fn(|_|UnsafeCell::new(Completion{user_data:0,result:0,flags:0})),receipts:std::array::from_fn(|_|UnsafeCell::new(ZERO))});m.header[4].store(cursor,Ordering::Relaxed);m.header[5].store(cursor,Ordering::Relaxed);m}
 fn base(&self)->*mut u8{self as *const Self as *mut u8}
 unsafe fn reader(&self)->PairedCq{unsafe{PairedCq::new(self.base(),256,0,64,128,4).unwrap()}}
 fn publish(&self,cursor:u32,seq:u64){let slot=(cursor&3)as usize;unsafe{self.cq[slot].get().write(Completion{user_data:seq,result:(seq%4096)as i32,flags:4});self.receipts[slot].get().write(BufferReceipt{pool:SlotRef{slot:7,reserved:0,incarnation:seq+91},member:slot as u32,reserved:0,lease:seq+1});}self.header[5].store(cursor.wrapping_add(1),Ordering::Release);}
}
#[test]fn copies_receipt_before_slot_reuse_and_wrap(){
 let m=Memory::new(u32::MAX-1);assert_eq!(std::mem::size_of::<Memory>(),256);let mut q=unsafe{m.reader()};assert!(q.reap().unwrap().is_none());
 let mut records=Vec::new();for n in 0..4{m.publish((u32::MAX-1).wrapping_add(n),n as u64);records.push(q.reap().unwrap().unwrap());}
 m.publish(2,99);for(n,r)in records.iter().enumerate(){assert_eq!(r.cqe.user_data,n as u64);assert_eq!(r.receipt.lease,n as u64+1);assert_eq!(r.receipt.pool.incarnation,n as u64+91);}
 assert_eq!(m.header[4].load(Ordering::Acquire),2);assert_eq!(q.reap().unwrap().unwrap().cqe.user_data,99);
}
#[test]fn refuses_corrupt_distance_or_second_head_writer(){
 let m=Memory::new(0);let mut q=unsafe{m.reader()};m.header[5].store(5,Ordering::Release);assert_eq!(q.reap().unwrap_err(),CqError::Corrupt);assert_eq!(m.header[4].load(Ordering::Relaxed),0);
 m.header[5].store(1,Ordering::Release);m.header[4].store(1,Ordering::Relaxed);assert_eq!(q.reap().unwrap_err(),CqError::Corrupt);
}
#[test]fn geometry_refuses_overlap_alignment_and_overflow(){
 let m=Memory::new(0);
 for(len,h,c,r,n)in [(256,0,64,128,0),(256,0,64,128,3),(256,0,64,128,16384),(255,0,64,128,4),(256,0,60,128,4),(256,0,64,64,4),(256,0,64,0,4),(256,usize::MAX-7,64,128,4),(usize::MAX,0,64,128,4)]{
  assert!(matches!(unsafe{PairedCq::new(m.base(),len,h,c,r,n)},Err(CqError::Geometry)));
 }
}
#[test]fn concurrent_pair_publication_and_reuse(){
 let m=Memory::new(u32::MAX-50);let producer=m.clone();let t=thread::spawn(move||{
  for n in 0..100000u32{let cursor=(u32::MAX-50).wrapping_add(n);while cursor.wrapping_sub(producer.header[4].load(Ordering::Acquire))==4{thread::yield_now();}producer.publish(cursor,n as u64);}
 });
 let mut q=unsafe{m.reader()};for n in 0..100000u64{let record=loop{if let Some(x)=q.reap().unwrap(){break x;}thread::yield_now();};assert_eq!(record.cqe.user_data,n);assert_eq!(record.cqe.result,(n%4096)as i32);assert_eq!(record.cqe.flags,4);assert_eq!(record.receipt.lease,n+1);assert_eq!(record.receipt.pool.incarnation,n+91);}
 t.join().unwrap();assert!(q.reap().unwrap().is_none());
}
