#![allow(dead_code)]
#[path="/Users/northkillpd/projects/thylacine-corona/usr/lib/libthyla-rs/src/loom/service_abi.rs"] mod service_abi;
mod lease_ledger;
mod ring_payload;
use ring_payload::*;
use service_abi::*;
use std::{cell::{Cell,UnsafeCell},ptr::NonNull,rc::Rc};
struct Probe{register_rc:Cell<i64>,register_id:Cell<SlotRef>,registrations:Cell<usize>,rc:Cell<i64>,destroy_rc:Cell<i64>,returns:Cell<usize>,pool_retires:[Cell<usize>;64],destroyed:[Cell<usize>;64],retires:Cell<usize>,closes:Cell<usize>,freed:Cell<usize>,retired:Cell<bool>}
impl Probe{fn new()->Rc<Self>{Rc::new(Self{register_rc:Cell::new(0),register_id:Cell::new(id(0,1)),registrations:Cell::new(0),rc:Cell::new(0),destroy_rc:Cell::new(0),returns:Cell::new(0),pool_retires:std::array::from_fn(|_|Cell::new(0)),destroyed:std::array::from_fn(|_|Cell::new(0)),retires:Cell::new(0),closes:Cell::new(0),freed:Cell::new(0),retired:Cell::new(false)})}}
struct Backing{bytes:Box<UnsafeCell<[u8;1024]>>,p:Rc<Probe>}
impl Backing{fn new(p:Rc<Probe>)->Self{let mut bytes=[0;1024];for i in 0..128{bytes[i*8..i*8+8].fill(i as u8);}Self{bytes:Box::new(UnsafeCell::new(bytes)),p}}}
unsafe impl RingBackend for Backing{
 fn register_pool(&self,ranges:&[PoolMember],out:&mut SlotRef)->i64{
  assert!(!ranges.is_empty());self.p.registrations.set(self.p.registrations.get()+1);
  *out=self.p.register_id.get();let rc=self.p.register_rc.get();
  WATCH_ALLOCS.with(|x|x.set(true));rc
 }
 fn buffer_count(&self)->usize{1}fn buffer_len(&self,_:usize)->usize{1024}fn buffer(&self,_:usize)->NonNull<u8>{NonNull::new(self.bytes.get().cast()).unwrap()}
 fn return_buffer(&self,_:BufferReceipt)->i64{self.p.returns.set(self.p.returns.get()+1);self.p.rc.get()}
 fn retire_pool(&self,r:SlotRef){let n=&self.p.pool_retires[r.slot as usize];n.set(n.get()+1);}
 fn destroy_pool(&self,r:SlotRef)->i64{let rc=self.p.destroy_rc.get();if rc==0{let n=&self.p.destroyed[r.slot as usize];n.set(n.get()+1);}rc}
 fn begin_retire(&self){self.p.retires.set(self.p.retires.get()+1);}fn retired(&self)->bool{self.p.retired.get()}fn close(&self){self.p.closes.set(self.p.closes.get()+1);}
}
impl Drop for Backing{fn drop(&mut self){assert_eq!(self.p.closes.get(),1);self.p.freed.set(self.p.freed.get()+1);}}
fn id(slot:u32,generation:u64)->SlotRef{SlotRef{slot,reserved:0,incarnation:generation}}
fn range(offset:u64)->PoolMember{PoolMember{buffer_index:0,reserved:0,offset,length:8}}
fn receipt(slot:u32,generation:u64,member:u32,lease:u64)->BufferReceipt{BufferReceipt{pool:id(slot,generation),member,reserved:0,lease}}
fn fixture()->(Rc<Probe>,RingOwner<Backing>,RingClient<Backing>){let p=Probe::new();let(r,c)=RingOwner::new(Backing::new(p.clone())).unwrap();(p,r,c)}
fn attach(c:&RingClient<Backing>,slot:u32,offset:u64)->Pool<Backing>{unsafe{c.attach_pool(id(slot,1),&[range(offset)])}.unwrap()}
fn accept(p:&Pool<Backing>,slot:u32)->Payload<Backing>{unsafe{p.accept(8,4,receipt(slot,1,0,1))}.unwrap().unwrap()}
#[test]fn one_pool_drop_preserves_other_pool_and_ring(){let(p,mut r,c)=fixture();let a=attach(&c,1,0);let b=attach(&c,2,8);let av=accept(&a,1);let bv=accept(&b,2);assert_eq!(av.bytes(),&[0;8]);assert_eq!(bv.bytes(),&[1;8]);drop(a);assert_eq!(p.pool_retires[1].get(),1);assert_eq!(p.retires.get(),0);assert_eq!(p.closes.get(),0);assert!(!r.progress());assert_eq!(r.members(),2);assert_eq!(bv.bytes(),&[1;8]);drop(av);assert!(!r.progress());assert_eq!(p.destroyed[1].get(),1);assert_eq!(r.members(),1);assert_eq!(bv.bytes(),&[1;8]);drop(bv);drop(b);r.progress();assert_eq!(r.members(),0);drop(c);p.retired.set(true);assert!(r.progress());drop(r);assert_eq!(p.freed.get(),1);}
#[test]fn overlap_and_global64_members_are_rejected(){let(_p,mut r,c)=fixture();let aa:Vec<_>=(0..32).map(|i|range(i*8)).collect();let bb:Vec<_>=(32..64).map(|i|range(i*8)).collect();let a=unsafe{c.attach_pool(id(0,1),&aa)}.unwrap();assert!(matches!(unsafe{c.attach_pool(id(1,1),&[range(0)])},Err(Error::Geometry)));let b=unsafe{c.attach_pool(id(1,1),&bb)}.unwrap();assert_eq!(r.members(),64);assert!(matches!(unsafe{c.attach_pool(id(2,1),&[range(512)])},Err(Error::Busy)));drop(a);r.progress();assert_eq!(r.members(),32);let d=attach(&c,2,512);assert_eq!(r.members(),33);drop(d);drop(b);drop(c);drop(r);}
#[test]fn pool_destroy_error_retains_exclusion_and_incarnation(){let(p,mut r,c)=fixture();let a=attach(&c,3,0);drop(a);p.destroy_rc.set(-11);r.progress();assert_eq!(r.members(),1);assert!(matches!(unsafe{c.attach_pool(id(4,1),&[range(0)])},Err(Error::Geometry)));assert!(matches!(unsafe{c.attach_pool(id(3,2),&[range(8)])},Err(Error::Busy)));p.destroy_rc.set(0);r.progress();assert_eq!(r.members(),0);assert!(matches!(unsafe{c.attach_pool(id(3,1),&[range(8)])},Err(Error::Stale)));let b=unsafe{c.attach_pool(id(3,2),&[range(8)])}.unwrap();assert!(unsafe{b.accept(8,4,receipt(3,1,0,1))}.is_err());let v=unsafe{b.accept(8,4,receipt(3,2,0,1))}.unwrap().unwrap();assert_eq!(v.bytes(),&[1;8]);drop(v);drop(b);drop(c);drop(r);}
#[test]fn pending_returns_are_global_bounded_and_block_pool_reclaim(){let(p,mut r,c)=fixture();let aa:Vec<_>=(0..32).map(|i|range(i*8)).collect();let bb:Vec<_>=(32..64).map(|i|range(i*8)).collect();let a=unsafe{c.attach_pool(id(0,1),&aa)}.unwrap();let b=unsafe{c.attach_pool(id(1,1),&bb)}.unwrap();p.rc.set(-2);for i in 0..32{drop(unsafe{a.accept(8,4,receipt(0,1,i,i as u64+1))}.unwrap());drop(unsafe{b.accept(8,4,receipt(1,1,i,i as u64+1))}.unwrap());}assert_eq!(r.pending(),64);drop(a);drop(b);let n=p.returns.get();r.progress();assert_eq!(p.returns.get()-n,64);assert_eq!(r.members(),64);assert_eq!(p.destroyed[0].get()+p.destroyed[1].get(),0);p.rc.set(0);r.progress();assert_eq!(r.pending(),0);assert_eq!(r.members(),0);drop(c);p.retired.set(true);assert!(r.progress());drop(r);assert_eq!(p.freed.get(),1);}
#[test]fn client_and_controller_close_preserve_all_live_payloads(){let(p,mut r,c)=fixture();let a=attach(&c,0,0);let b=attach(&c,1,8);let av=accept(&a,0);let bv=accept(&b,1);drop(c);assert_eq!(p.retires.get(),1);assert!(matches!(unsafe{a.accept(8,4,receipt(0,1,0,2))},Err(Error::Closing)));p.retired.set(true);assert!(!r.progress());drop(r);assert_eq!(p.closes.get(),1);drop(a);drop(b);assert_eq!(av.bytes(),&[0;8]);assert_eq!(bv.bytes(),&[1;8]);assert_eq!(p.freed.get(),0);drop(av);assert_eq!(p.freed.get(),0);drop(bv);assert_eq!(p.freed.get(),1);}
#[test]fn last_generation_cannot_wrap_or_reuse(){let(_p,mut r,c)=fixture();let a=unsafe{c.attach_pool(id(63,u64::MAX),&[range(0)])}.unwrap();drop(a);r.progress();assert!(matches!(unsafe{c.attach_pool(id(63,1),&[range(0)])},Err(Error::Stale)));assert!(matches!(unsafe{c.attach_pool(id(63,u64::MAX),&[range(0)])},Err(Error::Stale)));drop(c);drop(r);}

#[test]fn final_and_explicit_return_errors_preserve_full_ring_ownership(){let(p,mut r,c)=fixture();let pool=attach(&c,0,0);let a=accept(&pool,0);let zero=BufferReceipt{pool:id(0,0),member:0,reserved:0,lease:0};assert!(unsafe{pool.accept(0,0,zero)}.unwrap().is_none());assert_eq!(r.live(),1);p.rc.set(-2);let(rc,a)=a.try_return().err().unwrap();assert_eq!(rc,-2);assert_eq!(a.bytes(),&[0;8]);assert_eq!(r.live(),1);p.rc.set(1);let(rc,a)=a.try_return().err().unwrap();assert_eq!(rc,1);assert_eq!(r.live(),1);assert_eq!(a.bytes(),&[0;8]);p.rc.set(0);assert!(a.try_return().is_ok());assert_eq!(r.live(),0);drop(pool);r.progress();assert_eq!(r.members(),0);drop(c);drop(r);assert_eq!(p.freed.get(),1);}

// Observe allocations only on this test thread, starting at native callback exit.
// No fail allocator is used: ordinary Rust global OOM abort remains the policy.
thread_local!{static WATCH_ALLOCS:Cell<bool>=const{Cell::new(false)};static LATE_ALLOCS:Cell<usize>=const{Cell::new(0)};}
struct Allocator;
unsafe impl std::alloc::GlobalAlloc for Allocator{
 unsafe fn alloc(&self,l:std::alloc::Layout)->*mut u8{if WATCH_ALLOCS.try_with(|x|x.get()).unwrap_or(false){let _=LATE_ALLOCS.try_with(|x|x.set(x.get()+1));}unsafe{std::alloc::System.alloc(l)}}
 unsafe fn dealloc(&self,p:*mut u8,l:std::alloc::Layout){unsafe{std::alloc::System.dealloc(p,l)}}
 unsafe fn realloc(&self,p:*mut u8,l:std::alloc::Layout,n:usize)->*mut u8{if WATCH_ALLOCS.try_with(|x|x.get()).unwrap_or(false){let _=LATE_ALLOCS.try_with(|x|x.set(x.get()+1));}unsafe{std::alloc::System.realloc(p,l,n)}}
}
#[global_allocator]static ALLOCATOR:Allocator=Allocator;
fn register(c:&RingClient<Backing>,ranges:&[PoolMember])->Result<Pool<Backing>,Error>{
 LATE_ALLOCS.with(|x|x.set(0));let result=c.register_pool(ranges);WATCH_ALLOCS.with(|x|x.set(false));assert_eq!(LATE_ALLOCS.with(|x|x.get()),0,"allocation after native callback");result
}
#[test]fn registration_success_binds_actual_identity_without_late_allocation(){
 let(p,mut r,c)=fixture();p.register_id.set(id(7,91));let a=register(&c,&[range(8)]).unwrap();assert_eq!(p.registrations.get(),1);assert_eq!(r.members(),1);
 assert!(unsafe{a.accept(8,4,receipt(7,90,0,1))}.is_err());let v=unsafe{a.accept(8,4,receipt(7,91,0,1))}.unwrap().unwrap();assert_eq!(v.bytes(),&[1;8]);drop(v);drop(a);r.progress();assert_eq!(r.members(),0);drop(c);drop(r);assert_eq!(p.freed.get(),1);
}
#[test]fn registration_rejects_before_syscall_and_refunds_definite_error(){
 let(p,mut r,c)=fixture();let a=register(&c,&[range(0)]).unwrap();assert!(register(&c,&[range(0)]).is_err());assert!(register(&c,&[range(u64::MAX)]).is_err());assert!(register(&c,&[]).is_err());assert_eq!(p.registrations.get(),1);
 p.register_rc.set(-14);p.register_id.set(id(0,0));assert!(matches!(register(&c,&[range(8)]),Err(Error::Backend(-14))));assert_eq!(r.members(),1);assert_eq!(p.closes.get(),0);
 p.register_rc.set(0);p.register_id.set(id(1,1));let b=register(&c,&[range(8)]).unwrap();assert_eq!(r.members(),2);drop(a);drop(b);r.progress();assert_eq!(r.members(),0);drop(c);drop(r);
}
#[test]fn registration_preflight_global_capacity_avoids_native_admission(){
 let(p,mut r,c)=fixture();let ranges:Vec<_>=(0..64).map(|i|range(i*8)).collect();let a=register(&c,&ranges).unwrap();assert!(matches!(register(&c,&[range(512)]),Err(Error::Busy)));assert_eq!(p.registrations.get(),1);drop(a);r.progress();assert_eq!(r.members(),0);drop(c);drop(r);
}
#[test]fn uncertain_registration_closes_and_quarantines_with_live_payload(){
 for(rc,identity)in[(0,id(0,0)),(0,id(64,1)),(0,SlotRef{slot:1,reserved:1,incarnation:1}),(0,id(0,1)),(1,id(1,1)),(-14,id(1,1))]{
  let(p,mut r,c)=fixture();let a=attach(&c,0,0);let v=accept(&a,0);p.register_rc.set(rc);p.register_id.set(identity);
  assert!(matches!(register(&c,&[range(8)]),Err(Error::Uncertain(x))if x==rc));assert_eq!(p.closes.get(),1);assert_eq!(r.members(),2);assert_eq!(v.bytes(),&[0;8]);assert!(matches!(register(&c,&[range(16)]),Err(Error::Closing)));assert_eq!(p.registrations.get(),1);
  p.retired.set(true);assert!(!r.progress());assert_eq!(p.destroyed.iter().map(|x|x.get()).sum::<usize>(),0);drop(c);drop(r);drop(a);assert_eq!(p.freed.get(),0);assert_eq!(v.bytes(),&[0;8]);drop(v);assert_eq!(p.freed.get(),1);
 }
}
#[test]fn stale_native_registration_keeps_precharged_reservation_until_close(){
 let(p,mut r,c)=fixture();let a=attach(&c,9,0);drop(a);r.progress();p.register_id.set(id(9,1));assert!(matches!(register(&c,&[range(8)]),Err(Error::Uncertain(0))));assert_eq!(r.members(),1);assert_eq!(p.destroyed[9].get(),1);assert_eq!(p.closes.get(),1);r.progress();assert_eq!(p.destroyed[9].get(),1);drop(c);drop(r);assert_eq!(p.freed.get(),1);
}
#[test]fn ledger_cannot_accept_unbound_or_rebind_after_return(){
 use lease_ledger::*;let mut l=LeaseLedger::prepare(&[range(0)],&[1024]).unwrap();assert!(matches!(l.accept_read(8,4,receipt(0,1,0,1)),Err(LeaseError::State)));assert!(l.bind(id(64,1)).is_err());l.bind(id(0,1)).unwrap();let t=l.accept_read(8,4,receipt(0,1,0,1)).unwrap().unwrap();l.begin_return(&t).unwrap();l.finish_return(&t,0).unwrap();assert_eq!(l.outstanding(),0);assert_eq!(l.bind(id(1,2)),Err(LeaseError::State));
}

fn main(){let (_p,_root,c)=fixture();let pool=attach(&c,0,0);let a=accept(&pool,0);let view=a.bytes();let _=a.try_return();println!("{:?}",view);}
