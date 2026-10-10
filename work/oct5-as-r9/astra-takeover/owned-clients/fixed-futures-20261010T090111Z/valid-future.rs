#![allow(dead_code)]
#[path="/Users/northkillpd/projects/thylacine-corona/usr/lib/libthyla-rs/src/loom/service_abi.rs"] mod service_abi;
mod lease_ledger;
mod ring_payload;
use ring_payload::*;
use service_abi::*;
use std::{cell::{Cell,UnsafeCell},ptr::NonNull,rc::Rc};
struct Probe{aborts:Cell<usize>,last_abort:Cell<SlotRef>,register_rc:Cell<i64>,register_id:Cell<SlotRef>,registrations:Cell<usize>,rc:Cell<i64>,destroy_rc:Cell<i64>,returns:Cell<usize>,pool_retires:[Cell<usize>;64],destroyed:[Cell<usize>;64],retires:Cell<usize>,closes:Cell<usize>,freed:Cell<usize>,retired:Cell<bool>}
impl Probe{fn new()->Rc<Self>{Rc::new(Self{aborts:Cell::new(0),last_abort:Cell::new(id(0,0)),register_rc:Cell::new(0),register_id:Cell::new(id(0,1)),registrations:Cell::new(0),rc:Cell::new(0),destroy_rc:Cell::new(0),returns:Cell::new(0),pool_retires:std::array::from_fn(|_|Cell::new(0)),destroyed:std::array::from_fn(|_|Cell::new(0)),retires:Cell::new(0),closes:Cell::new(0),freed:Cell::new(0),retired:Cell::new(false)})}}
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
 fn abort_scope(&self,scope:SlotRef){self.p.aborts.set(self.p.aborts.get()+1);self.p.last_abort.set(scope);}
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

#[test]fn fixed_read_owns_range_through_terminal_and_view(){
 let(p,mut r,c)=fixture();let mut f=c.reserve_fixed(101,id(4,9),range(8),false).unwrap();assert!(f.bytes().is_err());
 assert!(unsafe{r.finish_fixed(101,8)}.is_err());unsafe{f.publish().unwrap();}assert!(f.bytes().is_err());
 assert!(unsafe{r.finish_fixed(101,9)}.is_err());assert_eq!(r.fixed_count(),1);unsafe{r.finish_fixed(101,3).unwrap();}
 assert_eq!(f.bytes().unwrap(),&[1;3]);assert!(c.reserve_fixed(102,id(4,9),range(8),false).is_err());
 drop(c);drop(r);assert_eq!(p.closes.get(),1);assert_eq!(p.freed.get(),0);assert_eq!(f.bytes().unwrap(),&[1;3]);drop(f);assert_eq!(p.freed.get(),1);
}
#[test]fn fixed_abandon_is_scope_cancel_not_refund(){
 let(p,mut r,c)=fixture();let mut f=c.reserve_fixed(201,id(3,7),range(8),false).unwrap();unsafe{f.publish().unwrap();}drop(f);
 assert_eq!(p.aborts.get(),1);assert_eq!(p.last_abort.get(),id(3,7));assert_eq!(r.fixed_count(),1);assert!(!r.progress());assert_eq!(r.fixed_count(),1);
 assert!(c.reserve_fixed(202,id(3,7),range(8),false).is_err());assert!(unsafe{r.finish_fixed(999,-2)}.is_err());assert_eq!(r.fixed_count(),1);
 unsafe{r.finish_fixed(201,-2).unwrap();}assert_eq!(r.fixed_count(),0);let f=c.reserve_fixed(202,id(3,7),range(8),false).unwrap();drop(f);assert_eq!(p.aborts.get(),1);
}
#[test]fn fixed_and_provided_exclude_overlaps_both_directions(){
 let(_p,mut r,c)=fixture();let pool=attach(&c,1,8);assert!(c.reserve_fixed(301,id(0,1),range(8),false).is_err());
 let f=c.reserve_fixed(301,id(0,1),range(0),false).unwrap();assert!(unsafe{c.attach_pool(id(2,1),&[range(0)])}.is_err());
 assert!(c.register_pool(&[range(0)]).is_err());drop(pool);r.progress();drop(f);assert_eq!(r.fixed_count(),0);let _pool=attach(&c,2,0);
}
#[test]fn fixed_write_freezes_after_publish_and_error_preserves_owner(){
 let(_p,mut r,c)=fixture();let mut f=c.reserve_fixed(401,id(0,1),range(16),true).unwrap();assert!(f.write_from(&[3;7]).is_err());f.write_from(&[3;8]).unwrap();
 unsafe{f.publish().unwrap();}assert!(f.write_from(&[4;8]).is_err());assert!(f.bytes().is_err());assert!(unsafe{f.publish()}.is_err());
 // A failed ENTER does not have a rollback API; reservation remains active.
 assert_eq!(r.fixed_count(),1);unsafe{r.finish_fixed(401,-2).unwrap();}assert!(unsafe{r.finish_fixed(401,0)}.is_err());assert_eq!(r.fixed_count(),1);drop(f);assert_eq!(r.fixed_count(),0);
 let pool=attach(&c,1,16);let a=accept(&pool,1);assert_eq!(a.bytes(),&[3;8]);drop(a);
}
#[test]fn fixed_retirement_reclaims_abandoned_only_after_proof(){
 let(p,mut r,c)=fixture();let mut f=c.reserve_fixed(501,id(0,1),range(0),false).unwrap();unsafe{f.publish().unwrap();}drop(f);drop(c);assert!(!r.progress());assert_eq!(r.fixed_count(),1);
 p.retired.set(true);assert!(r.progress());assert_eq!(r.fixed_count(),0);drop(r);assert_eq!(p.freed.get(),1);
}
#[test]fn fixed_bad_geometry_and_precise_error(){
 let(_p,mut r,c)=fixture();for x in [PoolMember{length:0,..range(0)},PoolMember{reserved:1,..range(0)},PoolMember{offset:u64::MAX,..range(0)},PoolMember{length:2048,..range(0)},PoolMember{buffer_index:1,..range(0)}]{assert!(c.reserve_fixed(601,id(0,1),x,false).is_err());}
 assert!(c.reserve_fixed(0,id(0,1),range(0),false).is_err());let mut f=c.reserve_fixed(601,id(0,1),range(0),false).unwrap();unsafe{f.publish().unwrap();r.finish_fixed(601,-14).unwrap();}assert_eq!(f.bytes(),Err(Error::Backend(-14)));drop(f);assert_eq!(r.fixed_count(),0);
}
#[test]fn fixed_unpublished_and_closed_drop_do_not_abort(){
 let(p,r,c)=fixture();let f=c.reserve_fixed(701,id(0,1),range(0),false).unwrap();drop(f);assert_eq!(p.aborts.get(),0);
 let mut f=c.reserve_fixed(702,id(0,1),range(0),false).unwrap();unsafe{f.publish().unwrap();}drop(r);assert_eq!(p.closes.get(),1);drop(f);assert_eq!(p.aborts.get(),0);drop(c);assert_eq!(p.freed.get(),1);
}

mod paired_cq;mod request_book;mod fixed_requests;
fn typed_driver(cap:usize)->(Rc<Probe>,fixed_requests::FixedDriver<Backing>){let p=Probe::new();let driver=fixed_requests::FixedDriver::new(Backing::new(p.clone()),cap).unwrap();(p,driver)}
fn typed_record(cookie:u64,result:i32,flags:u32)->paired_cq::Record{paired_cq::Record{cqe:paired_cq::Completion{user_data:cookie,result,flags},receipt:BufferReceipt{pool:id(0,0),member:0,reserved:0,lease:0}}}
#[test]fn typed_read_owns_until_consumed_view_ends(){
 let(p,mut d)=typed_driver(1);let mut r=d.read(id(1,3),range(8)).unwrap();assert!(r.try_complete().unwrap().is_none());let cookie=unsafe{r.publish().unwrap()};assert!(r.try_complete().unwrap().is_none());unsafe{d.receive(typed_record(cookie,3,0)).unwrap();}assert_eq!(d.occupied(),1);assert!(d.read(id(1,3),range(0)).is_err());let done=r.try_complete().unwrap().unwrap();assert_eq!(done.result(),3);assert_eq!(done.bytes().unwrap(),&[1;3]);assert_eq!(d.occupied(),0);assert!(r.try_complete().is_err());assert!(d.read(id(1,3),range(0)).is_err());assert_eq!(d.occupied(),0);drop(r);drop(d);assert_eq!(p.closes.get(),1);assert_eq!(p.freed.get(),0);assert_eq!(done.bytes().unwrap(),&[1;3]);drop(done);assert_eq!(p.freed.get(),1);
}
#[test]fn typed_abandon_retains_obligation_until_terminal(){
 let(p,mut d)=typed_driver(1);let mut r=d.read(id(5,6),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};drop(r);assert_eq!(p.aborts.get(),1);assert_eq!(p.last_abort.get(),id(5,6));assert_eq!(d.occupied(),1);assert!(d.read(id(5,6),range(8)).is_err());unsafe{d.receive(typed_record(cookie,-2,0)).unwrap();}assert_eq!(d.occupied(),0);let r=d.read(id(5,6),range(8)).unwrap();drop(r);assert_eq!(p.aborts.get(),1);assert_eq!(d.occupied(),0);
}
#[test]fn typed_prepare_rolls_back_both_tables(){
 let(p,d)=typed_driver(1);assert!(d.read(id(0,1),range(2048)).is_err());assert_eq!(d.occupied(),0);assert!(d.write(id(0,1),range(0),&[9;7]).is_err());assert_eq!(d.occupied(),0);let r=d.read(id(0,1),range(0)).unwrap();assert_eq!(d.occupied(),1);drop(r);assert_eq!(d.occupied(),0);assert_eq!(p.aborts.get(),0);
}
#[test]fn typed_corruption_closes_without_refunding_active_storage(){
 let(p,mut d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};assert!(unsafe{d.receive(typed_record(cookie,3,1))}.is_err());assert_eq!(p.closes.get(),1);assert_eq!(d.occupied(),1);assert!(r.try_complete().is_err());p.retired.set(true);assert!(!d.progress());drop(r);assert!(d.progress());assert_eq!(d.occupied(),0);assert_eq!(p.aborts.get(),0);drop(d);assert_eq!(p.freed.get(),1);
}
#[test]fn typed_retirement_without_cq_keeps_metadata_until_proof(){
 let(p,mut d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();unsafe{r.publish().unwrap();}drop(r);d.begin_retire();assert!(!d.progress());assert_eq!(d.occupied(),1);p.retired.set(true);assert!(d.progress());assert_eq!(d.occupied(),0);assert!(d.read(id(0,1),range(0)).is_err());
}
#[test]fn typed_write_and_read_errors_are_preserved(){
 let(p,mut d)=typed_driver(1);let mut w=d.write(id(0,1),range(0),&[7;8]).unwrap();let cookie=unsafe{w.publish().unwrap()};assert!(unsafe{w.publish()}.is_err());unsafe{d.receive(typed_record(cookie,-14,0)).unwrap();}assert_eq!(w.try_complete().unwrap().unwrap().result,-14);assert_eq!(d.occupied(),0);drop(w);assert_eq!(p.aborts.get(),0);
 let mut r=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};unsafe{d.receive(typed_record(cookie,0,0)).unwrap();}assert_eq!(r.try_complete().unwrap().unwrap().bytes().unwrap(),&[]);drop(r);
 let mut w=d.write(id(0,1),range(0),&[7;8]).unwrap();let cookie=unsafe{w.publish().unwrap()};unsafe{d.receive(typed_record(cookie,5,0)).unwrap();}assert_eq!(w.try_complete().unwrap().unwrap().result,5);drop(w);
 let mut r=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};unsafe{d.receive(typed_record(cookie,-14,0)).unwrap();}assert_eq!(r.try_complete().unwrap().unwrap().bytes(),Err(Error::Backend(-14)));
}
#[test]fn typed_drop_terminal_does_not_abort_or_leak(){
 let(p,mut d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{r.publish().unwrap()};unsafe{d.receive(typed_record(cookie,3,0)).unwrap();}drop(r);assert_eq!(d.occupied(),0);assert_eq!(p.aborts.get(),0);let r=d.read(id(0,1),range(8)).unwrap();drop(r);
}
#[test]fn typed_foreign_cookie_fails_only_its_driver(){
 let(p,mut a)=typed_driver(1);let(q,mut b)=typed_driver(1);let mut ra=a.read(id(0,1),range(0)).unwrap();let mut rb=b.read(id(0,1),range(0)).unwrap();let ca=unsafe{ra.publish().unwrap()};let cb=unsafe{rb.publish().unwrap()};assert_ne!(ca,cb);assert!(unsafe{a.receive(typed_record(cb,1,0))}.is_err());assert_eq!(p.closes.get(),1);assert_eq!(q.closes.get(),0);unsafe{b.receive(typed_record(cb,1,0)).unwrap();}assert_eq!(rb.try_complete().unwrap().unwrap().result(),1);
}
#[test]fn typed_retire_prepared_prevents_publication(){
 let(p,mut d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();d.begin_retire();assert!(unsafe{r.publish()}.is_err());drop(r);assert_eq!(d.occupied(),0);assert_eq!(p.aborts.get(),0);p.retired.set(true);assert!(d.progress());
}

#[test]fn typed_driver_drop_reports_closing_to_pending_handle(){
 let(p,d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();unsafe{r.publish().unwrap();}drop(d);assert!(matches!(r.try_complete(),Err(fixed_requests::Error::Closing)));assert_eq!(p.freed.get(),0);drop(r);assert_eq!(p.freed.get(),1);
 let(p,d)=typed_driver(1);let mut r=d.read(id(0,1),range(0)).unwrap();drop(d);assert!(matches!(r.try_complete(),Err(fixed_requests::Error::Closing)));drop(r);assert_eq!(p.aborts.get(),0);assert_eq!(p.freed.get(),1);
}

mod fixed_futures;
use fixed_futures::FutureDriver;
use std::{future::Future,pin::Pin,task::{Wake,Waker,Context,Poll,RawWaker,RawWakerVTable},sync::{Arc,atomic::{AtomicUsize,Ordering}}};
struct WakeCount(AtomicUsize);
impl Wake for WakeCount{fn wake(self:Arc<Self>){self.0.fetch_add(1,Ordering::SeqCst);}fn wake_by_ref(self:&Arc<Self>){self.0.fetch_add(1,Ordering::SeqCst);}}
fn counter()->(Arc<WakeCount>,Waker){let a=Arc::new(WakeCount(AtomicUsize::new(0)));let w=Waker::from(a.clone());(a,w)}
fn poll<F:Future+Unpin>(f:&mut F,w:&Waker)->Poll<F::Output>{Pin::new(f).poll(&mut Context::from_waker(w))}
fn future_driver()->(Rc<Probe>,FutureDriver<Backing>){let(p,d)=typed_driver(1);(p,FutureDriver::new(d))}
#[test]fn future_read_wakes_and_retains_result(){let(p,mut d)=future_driver();let mut f=d.read(id(0,1),range(8)).unwrap();let cookie=unsafe{f.publish().unwrap()};let(n,w)=counter();assert!(poll(&mut f,&w).is_pending());unsafe{d.receive(typed_record(cookie,3,0)).unwrap();}assert_eq!(n.0.load(Ordering::SeqCst),1);let done=match poll(&mut f,&w){Poll::Ready(Ok(r))=>r,_=>panic!("not ready")};assert_eq!(d.registrations(),0);assert!(matches!(poll(&mut f,&w),Poll::Ready(Err(fixed_requests::Error::Consumed))));drop(f);drop(d);assert_eq!(p.freed.get(),0);assert_eq!(done.bytes().unwrap(),&[1;3]);drop(done);assert_eq!(p.freed.get(),1);}
#[test]fn future_completion_before_first_poll(){let(_p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};unsafe{d.receive(typed_record(cookie,0,0)).unwrap();}let(n,w)=counter();assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(r))if r.bytes().unwrap().is_empty()));assert_eq!(n.0.load(Ordering::SeqCst),0);assert_eq!(d.registrations(),0);}
#[test]fn future_replaces_waker_without_notifying_old_task(){let(_p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let(a,wa)=counter();let(b,wb)=counter();assert!(poll(&mut f,&wa).is_pending());assert!(poll(&mut f,&wb).is_pending());unsafe{d.receive(typed_record(cookie,1,0)).unwrap();}assert_eq!(a.0.load(Ordering::SeqCst),0);assert_eq!(b.0.load(Ordering::SeqCst),1);assert!(matches!(poll(&mut f,&wb),Poll::Ready(Ok(_))));}
#[test]fn future_drop_unregisters_but_keeps_active_storage(){let(p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let(n,w)=counter();assert!(poll(&mut f,&w).is_pending());drop(f);assert_eq!(p.aborts.get(),1);assert_eq!(d.registrations(),0);assert!(d.read(id(0,1),range(8)).is_err());unsafe{d.receive(typed_record(cookie,-2,0)).unwrap();}assert_eq!(n.0.load(Ordering::SeqCst),0);let f=d.read(id(0,1),range(8)).unwrap();drop(f);assert_eq!(d.registrations(),0);}
#[test]fn future_driver_close_and_drop_wake_unfinished(){for active in [false,true]{let(p,d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();if active{unsafe{f.publish().unwrap();}}let(n,w)=counter();assert!(poll(&mut f,&w).is_pending());drop(d);assert_eq!(n.0.load(Ordering::SeqCst),1);assert_eq!(p.closes.get(),1);assert!(matches!(poll(&mut f,&w),Poll::Ready(Err(fixed_requests::Error::Closing))));drop(f);assert_eq!(p.freed.get(),1);}}
#[test]fn future_malformed_record_wakes_then_fails_closed(){let(p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let(n,w)=counter();assert!(poll(&mut f,&w).is_pending());assert!(unsafe{d.receive(typed_record(cookie,3,1))}.is_err());assert_eq!(n.0.load(Ordering::SeqCst),1);assert_eq!(p.closes.get(),1);assert!(matches!(poll(&mut f,&w),Poll::Ready(Err(fixed_requests::Error::Closing))));assert_eq!(d.registrations(),0);}
#[test]fn future_failed_prepare_and_write_result(){let(_p,mut d)=future_driver();assert!(d.write(id(0,1),range(0),&[3;7]).is_err());assert_eq!(d.registrations(),0);let mut f=d.write(id(0,1),range(0),&[3;8]).unwrap();let cookie=unsafe{f.publish().unwrap()};let(n,w)=counter();assert!(poll(&mut f,&w).is_pending());unsafe{d.receive(typed_record(cookie,-14,0)).unwrap();}assert_eq!(n.0.load(Ordering::SeqCst),1);assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(r))if r.result==-14));assert_eq!(d.registrations(),0);}
thread_local!{static HOOK:std::cell::RefCell<Option<Box<dyn FnOnce()>>>=const{std::cell::RefCell::new(None)};static HOOK_KIND:Cell<u8>=const{Cell::new(0)};}
fn hook(kind:u8){if HOOK_KIND.with(|k|k.get())==kind{let f=HOOK.with(|h|h.borrow_mut().take());if let Some(f)=f{f();}}}
unsafe fn hook_clone(p:*const())->RawWaker{unsafe{Arc::increment_strong_count(p.cast::<WakeCount>());}hook(1);RawWaker::new(p,&HOOK_VTABLE)}
unsafe fn hook_wake(p:*const()){let a=unsafe{Arc::from_raw(p.cast::<WakeCount>())};a.0.fetch_add(1,Ordering::SeqCst);hook(2);}
unsafe fn hook_wake_ref(p:*const()){let a=unsafe{&*p.cast::<WakeCount>()};a.0.fetch_add(1,Ordering::SeqCst);hook(2);}
unsafe fn hook_drop(p:*const()){drop(unsafe{Arc::from_raw(p.cast::<WakeCount>())});hook(3);}
static HOOK_VTABLE:RawWakerVTable=RawWakerVTable::new(hook_clone,hook_wake,hook_wake_ref,hook_drop);
fn hook_waker()->Waker{let a=Arc::new(WakeCount(AtomicUsize::new(0)));unsafe{Waker::from_raw(RawWaker::new(Arc::into_raw(a).cast(),&HOOK_VTABLE))}}
fn set_hook(kind:u8,f:impl FnOnce()+'static){HOOK_KIND.with(|k|k.set(kind));HOOK.with(|h|*h.borrow_mut()=Some(Box::new(f)));}
#[test]fn future_clone_callback_completion_is_not_lost(){let(_p,d)=future_driver();let d=Rc::new(std::cell::RefCell::new(d));let mut f=d.borrow().read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let callback=d.clone();set_hook(1,move||unsafe{callback.borrow_mut().receive(typed_record(cookie,2,0)).unwrap();});let w=hook_waker();assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(r))if r.result()==2));assert_eq!(d.borrow().registrations(),0);}
#[test]fn future_replaced_waker_drop_can_deliver_completion(){let(_p,d)=future_driver();let d=Rc::new(std::cell::RefCell::new(d));let mut f=d.borrow().read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let w=hook_waker();assert!(poll(&mut f,&w).is_pending());let callback=d.clone();set_hook(3,move||unsafe{callback.borrow_mut().receive(typed_record(cookie,2,0)).unwrap();});let(n,new)=counter();assert!(poll(&mut f,&new).is_pending());assert_eq!(n.0.load(Ordering::SeqCst),1);assert!(matches!(poll(&mut f,&new),Poll::Ready(Ok(r))if r.result()==2));}
#[test]fn future_wake_callback_polls_outside_hub_borrow(){let(_p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let w=hook_waker();assert!(poll(&mut f,&w).is_pending());let observed=Rc::new(Cell::new(false));let flag=observed.clone();set_hook(2,move||{let(_,plain)=counter();flag.set(matches!(poll(&mut f,&plain),Poll::Ready(Ok(r))if r.result()==4));});unsafe{d.receive(typed_record(cookie,4,0)).unwrap();}assert!(observed.get());assert_eq!(d.registrations(),0);}
#[test]fn future_ready_path_has_no_internal_allocation_with_arc_waker(){let(_p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let(_,w)=counter();LATE_ALLOCS.with(|x|x.set(0));WATCH_ALLOCS.with(|x|x.set(true));assert!(poll(&mut f,&w).is_pending());unsafe{d.receive(typed_record(cookie,2,0)).unwrap();}let ready=poll(&mut f,&w);WATCH_ALLOCS.with(|x|x.set(false));assert_eq!(LATE_ALLOCS.with(|x|x.get()),0);assert!(matches!(ready,Poll::Ready(Ok(_))));}
struct ManyBacking{buffers:[Box<UnsafeCell<[u8;8]>>;64],p:Rc<Probe>}
unsafe impl RingBackend for ManyBacking{
 fn register_pool(&self,_:&[PoolMember],_:&mut SlotRef)->i64{-11}
 fn buffer_count(&self)->usize{64}fn buffer_len(&self,_:usize)->usize{8}fn buffer(&self,i:usize)->NonNull<u8>{NonNull::new(self.buffers[i].get().cast()).unwrap()}
 fn return_buffer(&self,_:BufferReceipt)->i64{-11}fn retire_pool(&self,_:SlotRef){}fn destroy_pool(&self,_:SlotRef)->i64{-11}
 fn abort_scope(&self,_:SlotRef){self.p.aborts.set(self.p.aborts.get()+1);}fn begin_retire(&self){self.p.retires.set(self.p.retires.get()+1);}fn retired(&self)->bool{self.p.retired.get()}fn close(&self){self.p.closes.set(self.p.closes.get()+1);}
}
impl Drop for ManyBacking{fn drop(&mut self){assert_eq!(self.p.closes.get(),1);self.p.freed.set(self.p.freed.get()+1);}}
#[test]fn future_all64_cells_wake_and_reuse_without_old_handle_interference(){
 let p=Probe::new();let b=ManyBacking{buffers:std::array::from_fn(|_|Box::new(UnsafeCell::new([0;8]))),p:p.clone()};let mut d=FutureDriver::new(fixed_requests::FixedDriver::new(b,128).unwrap());let(n,w)=counter();let mut futures=Vec::new();let mut cookies=Vec::new();
 for i in 0..64{let r=PoolMember{buffer_index:i,..range(0)};let mut f=d.write(id(0,1),r,&[3;8]).unwrap();cookies.push(unsafe{f.publish().unwrap()});assert!(poll(&mut f,&w).is_pending());futures.push(f);}
 assert_eq!(d.registrations(),64);assert!(d.write(id(0,1),range(0),&[3;8]).is_err());assert_eq!(d.registrations(),64);
 for cookie in cookies{unsafe{d.receive(typed_record(cookie,8,0)).unwrap();}}
 assert_eq!(n.0.load(Ordering::SeqCst),64);for f in &mut futures{assert!(matches!(poll(f,&w),Poll::Ready(Ok(r))if r.result==8));}assert_eq!(d.registrations(),0);
 let next=d.write(id(0,1),range(0),&[4;8]).unwrap();drop(futures);assert_eq!(d.registrations(),1);drop(next);assert_eq!(d.registrations(),0);drop(d);assert_eq!(p.freed.get(),1);
}

fn main(){let(_,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let c=unsafe{f.publish().unwrap()};unsafe{d.receive(typed_record(c,1,0)).unwrap();}let(_,w)=counter();assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(r))if r.result()==1));}
