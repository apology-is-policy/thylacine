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

mod retirement_executor;
use retirement_executor::{FutureExecutor,Error as QueueError};
fn future_client(e:&mut FutureExecutor<Backing>)->(Rc<Probe>,retirement_executor::Managed<FutureDriver<Backing>>){let p=Probe::new();let c=e.admit(||Ok(FutureDriver::new(fixed_requests::FixedDriver::new(Backing::new(p.clone()),1)?))).unwrap();(p,c)}
#[test]fn composed_receive_wake_can_drop_another_client_and_poll(){
 let mut e=FutureExecutor::new(2).unwrap();let(p,mut a)=future_client(&mut e);let(q,b)=future_client(&mut e);let mut f=a.read(id(0,1),range(8)).unwrap();let cookie=unsafe{f.publish().unwrap()};let w=hook_waker();assert!(poll(&mut f,&w).is_pending());let flag=Rc::new(Cell::new(false));let seen=flag.clone();set_hook(2,move||{drop(b);let(_,plain)=counter();seen.set(matches!(poll(&mut f,&plain),Poll::Ready(Ok(r))if r.bytes().unwrap()==&[1;3]));});unsafe{a.receive(typed_record(cookie,3,0)).unwrap();}assert!(flag.get());assert_eq!(q.retires.get(),1);q.retired.set(true);assert_eq!(e.progress(2).reclaimed,1);assert_eq!(q.freed.get(),1);drop(a);p.retired.set(true);assert_eq!(e.progress(2).reclaimed,1);
}
#[test]fn composed_client_drop_wake_can_access_peer_and_retire_both(){
 let mut e=FutureExecutor::new(2).unwrap();let(p,a)=future_client(&mut e);let(q,b)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();unsafe{f.publish().unwrap();}let w=hook_waker();assert!(poll(&mut f,&w).is_pending());let flag=Rc::new(Cell::new(false));let seen=flag.clone();set_hook(2,move||{let other=b.read(id(0,1),range(8)).unwrap();drop(other);drop(b);let(_,plain)=counter();seen.set(matches!(poll(&mut f,&plain),Poll::Ready(Err(fixed_requests::Error::Closing))));});drop(a);assert!(flag.get());assert_eq!(p.aborts.get(),1);assert_eq!(e.progress(2).reclaimed,0);p.retired.set(true);q.retired.set(true);assert_eq!(e.progress(2).reclaimed,2);
}
#[test]fn composed_queue_shutdown_wake_can_drop_peer_without_borrow_conflict(){
 let mut e=FutureExecutor::new(2).unwrap();let(p,a)=future_client(&mut e);let(q,b)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();unsafe{f.publish().unwrap();}let w=hook_waker();assert!(poll(&mut f,&w).is_pending());let flag=Rc::new(Cell::new(false));let seen=flag.clone();set_hook(2,move||{assert!(matches!(b.read(id(0,1),range(8)),Err(QueueError::Closed)));drop(b);let(_,plain)=counter();seen.set(matches!(poll(&mut f,&plain),Poll::Ready(Err(fixed_requests::Error::Closing))));});drop(e);assert!(flag.get());assert_eq!(p.closes.get(),1);assert_eq!(q.closes.get(),1);drop(a);assert_eq!(p.freed.get(),1);assert_eq!(q.freed.get(),1);
}
#[test]fn composed_payload_result_blocks_cell_until_last_view_ends(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,mut a)=future_client(&mut e);let mut f=a.read(id(0,1),range(8)).unwrap();let cookie=unsafe{f.publish().unwrap()};unsafe{a.receive(typed_record(cookie,5,0)).unwrap();}let(_,w)=counter();let result=match poll(&mut f,&w){Poll::Ready(Ok(r))=>r,_=>panic!("not ready")};drop(f);drop(a);p.retired.set(true);assert_eq!(e.progress(1).reclaimed,0);assert!(matches!(e.admit(||unreachable!()),Err(QueueError::Full)));assert_eq!(result.bytes().unwrap(),&[1;5]);drop(result);assert_eq!(e.progress(1).reclaimed,1);assert_eq!(p.freed.get(),1);
}
#[test]fn composed_terminal_survives_client_retire_before_poll(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,mut a)=future_client(&mut e);let mut f=a.write(id(0,1),range(0),&[7;8]).unwrap();let cookie=unsafe{f.publish().unwrap()};let(n,w)=counter();assert!(poll(&mut f,&w).is_pending());unsafe{a.receive(typed_record(cookie,5,0)).unwrap();}drop(a);assert_eq!(n.0.load(Ordering::SeqCst),1);assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(r))if r.result==5));drop(f);p.retired.set(true);assert_eq!(e.progress(1).reclaimed,1);
}
#[test]fn composed_drop_progress_no_internal_allocation(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();let(_,w)=counter();assert!(poll(&mut f,&w).is_pending());LATE_ALLOCS.with(|x|x.set(0));WATCH_ALLOCS.with(|x|x.set(true));drop(a);assert!(matches!(poll(&mut f,&w),Poll::Ready(Err(fixed_requests::Error::Closing))));drop(f);p.retired.set(true);assert_eq!(e.progress(1).reclaimed,1);WATCH_ALLOCS.with(|x|x.set(false));assert_eq!(LATE_ALLOCS.with(|x|x.get()),0);
}

#[test]fn local_result_drop_schedules_retirement_without_new_cq(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,mut a)=future_client(&mut e);let mut f=a.read(id(0,1),range(8)).unwrap();let cookie=unsafe{f.publish().unwrap()};unsafe{a.receive(typed_record(cookie,3,0)).unwrap();}let(_,w)=counter();let result=match poll(&mut f,&w){Poll::Ready(Ok(r))=>r,_=>panic!("not ready")};drop(f);drop(a);p.retired.set(true);assert_eq!(e.progress_notified(1).reclaimed,0);assert!(!e.needs_progress());assert_eq!(result.bytes().unwrap(),&[1;3]);drop(result);assert!(e.needs_progress());assert_eq!(e.progress_notified(1).reclaimed,1);assert!(!e.needs_progress());assert_eq!(p.freed.get(),1);
}
#[test]fn local_last_future_drop_schedules_after_closing_poll(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();unsafe{f.publish().unwrap();}let(_,w)=counter();assert!(poll(&mut f,&w).is_pending());drop(a);assert!(matches!(poll(&mut f,&w),Poll::Ready(Err(fixed_requests::Error::Closing))));p.retired.set(true);assert_eq!(e.progress_notified(1).reclaimed,0);assert!(!e.needs_progress());drop(f);assert!(e.needs_progress());assert_eq!(e.progress_notified(1).reclaimed,1);assert_eq!(p.aborts.get(),1);
}
#[test]fn local_native_retirement_requires_explicit_event_not_busy_spin(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,a)=future_client(&mut e);drop(a);assert_eq!(e.progress_notified(1).visited,1);assert!(!e.needs_progress());for _ in 0..8{assert_eq!(e.progress_notified(64).visited,0);}assert_eq!(e.occupied(),1);p.retired.set(true);assert_eq!(e.progress_notified(1).visited,0);e.notify_progress();assert!(e.needs_progress());assert_eq!(e.progress_notified(1).reclaimed,1);assert!(!e.needs_progress());
}
#[test]fn local_zero_partial_budget_and_coalesced_events_preserve_sweep(){
 let mut e=FutureExecutor::new(3).unwrap();let(p,a)=future_client(&mut e);let(q,b)=future_client(&mut e);let(r,c)=future_client(&mut e);drop(a);drop(b);drop(c);p.retired.set(true);q.retired.set(true);r.retired.set(true);for _ in 0..100{e.notify_progress();}assert_eq!(e.progress_notified(0).visited,0);assert!(e.needs_progress());for remaining in [2,1,0]{let step=e.progress_notified(1);assert_eq!(step.visited,1);assert_eq!(step.reclaimed,1);assert_eq!(step.occupied,remaining);assert_eq!(e.needs_progress(),remaining!=0);}assert_eq!(e.progress_notified(usize::MAX).visited,0);
}
#[test]fn local_terminal_cq_signals_even_without_registered_waker(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,mut a)=future_client(&mut e);let mut f=a.write(id(0,1),range(0),&[3;8]).unwrap();let cookie=unsafe{f.publish().unwrap()};assert_eq!(e.progress_notified(1).visited,1);assert!(!e.needs_progress());unsafe{a.receive(typed_record(cookie,8,0)).unwrap();}assert!(e.needs_progress());assert_eq!(e.progress_notified(1).reclaimed,0);assert!(!e.needs_progress());let(_,w)=counter();assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(_))));assert!(e.needs_progress());drop(f);drop(a);p.retired.set(true);assert_eq!(e.progress_notified(1).reclaimed,1);
}
#[test]fn local_pool_and_payload_release_share_noncallback_signal(){
 let(_p,mut owner,client)=fixture();let signal=ProgressSignal::new();owner.bind_progress(signal.clone());let pool=attach(&client,0,0);let value=accept(&pool,0);assert!(!signal.take());drop(pool);assert!(signal.take());assert!(!signal.pending());assert!(value.try_return().is_ok());assert!(signal.take());assert!(!signal.pending());drop(client);assert!(signal.take());
}
#[test]fn local_pulse_drop_and_bounded_progress_do_not_allocate(){
 let mut e=FutureExecutor::new(1).unwrap();let(p,a)=future_client(&mut e);let f=a.read(id(0,1),range(0)).unwrap();drop(a);e.progress_notified(1);assert!(!e.needs_progress());p.retired.set(true);LATE_ALLOCS.with(|x|x.set(0));WATCH_ALLOCS.with(|x|x.set(true));drop(f);assert!(e.needs_progress());assert_eq!(e.progress_notified(1).reclaimed,1);WATCH_ALLOCS.with(|x|x.set(false));assert_eq!(LATE_ALLOCS.with(|x|x.get()),0);
}

mod local_wait;
use local_wait::{WaitSource,WakeReason,Outcome};
struct Events{clock:u64,epoch:u64,ready:bool,armed:bool,arms:usize,disarms:usize,waits:usize,sleeps:usize,advance:bool,fail_arm:bool,fail_wait:bool,reason:WakeReason,seen:Option<u64>,on_arm:Option<Box<dyn FnOnce()>>,on_ready:Option<Box<dyn FnOnce()>>,arm_time:Option<u64>}
impl Events{fn new()->Self{Self{clock:1,epoch:1,ready:false,armed:false,arms:0,disarms:0,waits:0,sleeps:0,advance:false,fail_arm:false,fail_wait:false,reason:WakeReason::Spurious,seen:None,on_arm:None,on_ready:None,arm_time:None}}}
impl WaitSource for Events{
 type Ticket=u64;
 fn now(&mut self)->u64{self.clock}
 fn arm(&mut self)->Result<u64,i32>{assert!(!self.armed);self.arms+=1;if self.fail_arm{return Err(-1);}self.armed=true;if let Some(f)=self.on_arm.take(){f();}if let Some(t)=self.arm_time.take(){self.clock=t;}Ok(self.epoch)}
 fn ready(&mut self)->bool{assert!(self.armed);if let Some(f)=self.on_ready.take(){f();}self.ready}
 fn wait(&mut self,ticket:u64,deadline:Option<u64>)->Result<WakeReason,i32>{assert!(self.armed);self.waits+=1;self.seen=deadline;if self.fail_wait{return Err(-2);}if self.advance{self.epoch+=1;}if ticket!=self.epoch{return Ok(WakeReason::Changed);}if deadline.is_some_and(|d|self.clock>=d){return Ok(WakeReason::Deadline);}self.sleeps+=1;Ok(self.reason)}
 fn disarm(&mut self,ticket:u64){assert!(self.armed);assert!(ticket<=self.epoch);self.armed=false;self.disarms+=1;}
}
fn quiet_queue()->FutureExecutor<Backing>{FutureExecutor::new(1).unwrap()}
#[test]fn wait_local_release_during_arm_prevents_sleep(){
 let mut e=quiet_queue();let(p,a)=future_client(&mut e);let f=a.read(id(0,1),range(0)).unwrap();drop(a);p.retired.set(true);let mut w=Events::new();w.on_arm=Some(Box::new(move||drop(f)));let t=local_wait::turn(&mut e,&mut w,1,None);assert_eq!(t.outcome,Outcome::Retry);assert_eq!(w.waits,0);assert_eq!(w.disarms,1);assert_eq!(e.progress_notified(1).reclaimed,1);
}
#[test]fn wait_ready_callback_release_is_checked_last(){
 let mut e=quiet_queue();let(p,a)=future_client(&mut e);let f=a.read(id(0,1),range(0)).unwrap();drop(a);p.retired.set(true);let mut w=Events::new();w.on_ready=Some(Box::new(move||drop(f)));assert_eq!(local_wait::turn(&mut e,&mut w,1,None).outcome,Outcome::Retry);assert_eq!(w.waits,0);assert_eq!(w.disarms,1);
}
#[test]fn wait_native_ready_and_intervening_event_are_not_lost(){
 let mut e=quiet_queue();let mut w=Events::new();w.ready=true;assert_eq!(local_wait::turn(&mut e,&mut w,1,None).outcome,Outcome::Retry);assert_eq!(w.waits,0);assert!(e.needs_progress());w.ready=false;w.advance=true;assert_eq!(local_wait::turn(&mut e,&mut w,1,None).outcome,Outcome::Woke(WakeReason::Changed));assert_eq!(w.sleeps,0);assert_eq!(w.disarms,2);assert!(e.needs_progress());
}
#[test]fn wait_absolute_deadline_before_and_after_arm(){
 let mut e=quiet_queue();let mut w=Events::new();assert_eq!(local_wait::turn(&mut e,&mut w,1,Some(1)).outcome,Outcome::Deadline);assert_eq!(w.arms,0);w.arm_time=Some(9);assert_eq!(local_wait::turn(&mut e,&mut w,1,Some(9)).outcome,Outcome::Deadline);assert_eq!(w.waits,0);assert_eq!(w.disarms,1);
}
#[test]fn wait_zero_and_partial_quantum_do_not_sleep(){
 let mut e=FutureExecutor::new(3).unwrap();let(_,a)=future_client(&mut e);let(_,b)=future_client(&mut e);let(_,c)=future_client(&mut e);drop(a);drop(b);drop(c);let mut w=Events::new();assert_eq!(local_wait::turn(&mut e,&mut w,0,None).outcome,Outcome::Retry);assert_eq!(w.arms,0);for _ in 0..2{assert_eq!(local_wait::turn(&mut e,&mut w,1,None).outcome,Outcome::Retry);}assert_eq!(w.arms,0);let t=local_wait::turn(&mut e,&mut w,1,None);assert_eq!(t.progress.visited,1);assert_eq!(t.progress.occupied,3);assert_eq!(t.outcome,Outcome::Woke(WakeReason::Spurious));assert_eq!(w.waits,1);
}
#[test]fn wait_spurious_wakes_keep_original_deadline_and_schedule_progress(){
 let mut e=quiet_queue();let mut w=Events::new();for now in [1,4,8]{w.clock=now;assert_eq!(local_wait::turn(&mut e,&mut w,1,Some(10)).outcome,Outcome::Woke(WakeReason::Spurious));assert_eq!(w.seen,Some(10));assert!(e.needs_progress());assert!(!w.armed);}w.clock=10;assert_eq!(local_wait::turn(&mut e,&mut w,1,Some(10)).outcome,Outcome::Deadline);assert_eq!(w.waits,3);
}
#[test]fn wait_failures_cleanup_without_retry_loop(){
 let mut e=quiet_queue();let mut w=Events::new();w.fail_arm=true;assert_eq!(local_wait::turn(&mut e,&mut w,1,None).outcome,Outcome::Failed(-1));assert_eq!(w.disarms,0);w.fail_arm=false;w.fail_wait=true;assert_eq!(local_wait::turn(&mut e,&mut w,1,None).outcome,Outcome::Failed(-2));assert_eq!(w.disarms,1);assert_eq!(w.waits,1);assert!(!w.armed);
}
#[test]fn wait_timeout_is_not_terminal_storage_proof(){
 let mut e=quiet_queue();let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();unsafe{f.publish().unwrap();}drop(a);let mut w=Events::new();w.reason=WakeReason::Deadline;let t=local_wait::turn(&mut e,&mut w,1,Some(9));assert_eq!(t.outcome,Outcome::Woke(WakeReason::Deadline));assert_eq!(e.occupied(),1);assert_eq!(p.freed.get(),0);drop(f);assert_eq!(e.progress_notified(1).reclaimed,0);p.retired.set(true);e.notify_progress();assert_eq!(e.progress_notified(1).reclaimed,1);
}
#[test]fn wait_bounded_turn_has_no_internal_allocation(){
 let mut e=quiet_queue();let mut w=Events::new();LATE_ALLOCS.with(|x|x.set(0));WATCH_ALLOCS.with(|x|x.set(true));let t=local_wait::turn(&mut e,&mut w,1,None);WATCH_ALLOCS.with(|x|x.set(false));assert_eq!(LATE_ALLOCS.with(|x|x.get()),0);assert_eq!(t.outcome,Outcome::Woke(WakeReason::Spurious));assert_eq!(w.disarms,1);
}

#[test]fn timer_future_cancel_waits_for_actual_terminal(){
 let(p,mut d)=future_driver();let mut f=d.write(id(0,1),range(0),&[3;8]).unwrap();f.set_deadline(Some(5)).unwrap();let cookie=unsafe{f.publish().unwrap()};let(n,w)=counter();assert!(poll(&mut f,&w).is_pending());assert!(f.request_cancel().unwrap());assert!(!f.request_cancel().unwrap());assert_eq!(d.expire(5,64),0);assert_eq!(n.0.load(Ordering::SeqCst),0);assert!(poll(&mut f,&w).is_pending());unsafe{d.receive(typed_record(cookie,8,0)).unwrap();}assert!(matches!(poll(&mut f,&w),Poll::Ready(Ok(r))if r.result==8));assert_eq!(p.aborts.get(),1);
}
#[test]fn timer_wait_uses_earliest_and_expiry_preserves_storage(){
 let mut e=quiet_queue();let(p,mut a)=future_client(&mut e);let mut f=a.write(id(0,1),range(0),&[3;8]).unwrap();f.set_deadline(Some(5)).unwrap();let cookie=unsafe{f.publish().unwrap()};let mut w=Events::new();let t=local_wait::timed_turn(&mut e,&mut w,1,1,64,Some(10));assert_eq!(t.cancellation_requests,0);assert_eq!(w.seen,Some(5));w.clock=5;let t=local_wait::timed_turn(&mut e,&mut w,1,1,64,Some(10));assert_eq!(t.cancellation_requests,1);assert_eq!(w.seen,Some(10));assert_eq!(e.occupied(),1);assert_eq!(p.freed.get(),0);let(_,task)=counter();assert!(poll(&mut f,&task).is_pending());unsafe{a.receive(typed_record(cookie,8,0)).unwrap();}assert!(matches!(poll(&mut f,&task),Poll::Ready(Ok(r))if r.result==8));
}
#[test]fn timer_queue_minimum_and_independent_fair_cursor(){
 let mut e=FutureExecutor::new(3).unwrap();let mut clients=Vec::new();let mut futures=Vec::new();let mut probes=Vec::new();for deadline in [7,3,5]{let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();f.set_deadline(Some(deadline)).unwrap();unsafe{f.publish().unwrap();}clients.push(a);futures.push(f);probes.push(p);}
 assert_eq!(e.next_deadline(),Some(3));let mut w=Events::new();w.clock=9;for n in 0..3{e.progress(1);let t=local_wait::timed_turn(&mut e,&mut w,3,1,64,None);assert_eq!(t.cancellation_requests,1);assert_eq!(probes[n].aborts.get(),1);if n<2{assert_eq!(t.turn.outcome,Outcome::Retry);assert_eq!(w.waits,0);}}assert_eq!(w.waits,1);assert_eq!(e.next_deadline(),None);assert_eq!(e.occupied(),3);
}
#[test]fn timer_zero_expiry_budgets_preserve_due_work(){
 let mut e=quiet_queue();let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();f.set_deadline(Some(1)).unwrap();unsafe{f.publish().unwrap();}let mut w=Events::new();for (rings,fixed) in [(0,64),(1,0)]{let t=local_wait::timed_turn(&mut e,&mut w,1,rings,fixed,None);assert_eq!(t.cancellation_requests,0);assert_eq!(t.turn.outcome,Outcome::Retry);assert_eq!(w.waits,0);assert_eq!(p.aborts.get(),0);}assert_eq!(local_wait::timed_turn(&mut e,&mut w,1,1,64,None).cancellation_requests,1);
}
#[test]fn timer_due_after_arm_retries_then_cancels(){
 let mut e=quiet_queue();let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();f.set_deadline(Some(5)).unwrap();unsafe{f.publish().unwrap();}let mut w=Events::new();w.arm_time=Some(5);let t=local_wait::timed_turn(&mut e,&mut w,1,1,64,None);assert_eq!(t.turn.outcome,Outcome::Retry);assert_eq!(t.cancellation_requests,0);assert_eq!(w.waits,0);assert_eq!(w.disarms,1);assert_eq!(local_wait::timed_turn(&mut e,&mut w,1,1,64,None).cancellation_requests,1);assert_eq!(p.aborts.get(),1);
}
#[test]fn timer_outer_deadline_keeps_separate_policy(){
 let mut e=quiet_queue();let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();f.set_deadline(Some(5)).unwrap();unsafe{f.publish().unwrap();}let mut w=Events::new();local_wait::timed_turn(&mut e,&mut w,1,1,64,Some(3));assert_eq!(w.seen,Some(3));w.clock=3;let t=local_wait::timed_turn(&mut e,&mut w,1,1,64,Some(3));assert_eq!(t.turn.outcome,Outcome::Deadline);assert_eq!(t.cancellation_requests,0);assert_eq!(p.aborts.get(),0);assert_eq!(e.occupied(),1);
}
#[test]fn timer_retiring_driver_still_owns_deadline(){
 let mut e=quiet_queue();let(p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();f.set_deadline(Some(1)).unwrap();unsafe{f.publish().unwrap();}drop(a);assert_eq!(e.next_deadline(),Some(1));let mut w=Events::new();assert_eq!(local_wait::timed_turn(&mut e,&mut w,1,1,64,None).cancellation_requests,1);assert_eq!(e.occupied(),1);drop(f);assert_eq!(p.aborts.get(),1);p.retired.set(true);assert_eq!(e.progress_notified(1).reclaimed,1);
}
#[test]fn timer_publication_during_arm_prevents_stale_sleep(){
 let mut e=quiet_queue();let(_p,a)=future_client(&mut e);let mut f=a.read(id(0,1),range(0)).unwrap();f.set_deadline(Some(5)).unwrap();let f=Rc::new(std::cell::RefCell::new(f));let captured=f.clone();let mut w=Events::new();w.on_arm=Some(Box::new(move||unsafe{captured.borrow_mut().publish().unwrap();}));let t=local_wait::timed_turn(&mut e,&mut w,1,1,64,None);assert_eq!(t.turn.outcome,Outcome::Retry);assert_eq!(w.waits,0);local_wait::timed_turn(&mut e,&mut w,1,1,64,None);assert_eq!(w.seen,Some(5));
}
#[test]fn timer_polling_driver_uses_same_queue_and_no_allocation(){
 let mut e=retirement_executor::RetirementExecutor::new(1).unwrap();let p=Probe::new();let a=e.admit(||fixed_requests::FixedDriver::new(Backing::new(p.clone()),1)).unwrap();let mut r=a.read(id(0,1),range(0)).unwrap();r.set_deadline(Some(1)).unwrap();unsafe{r.publish().unwrap();}let mut w=Events::new();LATE_ALLOCS.with(|x|x.set(0));WATCH_ALLOCS.with(|x|x.set(true));let t=local_wait::timed_turn(&mut e,&mut w,1,1,64,None);WATCH_ALLOCS.with(|x|x.set(false));assert_eq!(LATE_ALLOCS.with(|x|x.get()),0);assert_eq!(t.cancellation_requests,1);assert_eq!(p.aborts.get(),1);assert_eq!(e.occupied(),1);
}

mod local_tasks;
use local_tasks::{Tasks,Error as TaskError};
struct TaskProbe{polls:Rc<Cell<usize>>,drops:Rc<Cell<usize>>,waker:Rc<std::cell::RefCell<Option<Waker>>>,self_wake:bool,finish:bool,panic:bool}
impl Future for TaskProbe{type Output=();fn poll(self:Pin<&mut Self>,cx:&mut Context<'_>)->Poll<()>{let s=self.get_mut();s.polls.set(s.polls.get()+1);*s.waker.borrow_mut()=Some(cx.waker().clone());if s.panic{panic!("task panic probe");}if s.self_wake{cx.waker().wake_by_ref();}if s.finish{Poll::Ready(())}else{Poll::Pending}}}
impl Drop for TaskProbe{fn drop(&mut self){self.drops.set(self.drops.get()+1);}}
fn task_probe(self_wake:bool,finish:bool,panic:bool)->(TaskProbe,Rc<Cell<usize>>,Rc<Cell<usize>>,Rc<std::cell::RefCell<Option<Waker>>>){let polls=Rc::new(Cell::new(0));let drops=Rc::new(Cell::new(0));let waker=Rc::new(std::cell::RefCell::new(None));(TaskProbe{polls:polls.clone(),drops:drops.clone(),waker:waker.clone(),self_wake,finish,panic},polls,drops,waker)}
#[test]fn task_capacity_precedes_factory_and_ids_are_owner_specific(){
 assert!(Tasks::new(0).is_err());assert!(Tasks::new(65).is_err());let mut a=Tasks::new(1).unwrap();let mut b=Tasks::new(1).unwrap();let id=a.spawn(||async{}).unwrap();let bid=b.spawn(||async{}).unwrap();assert_eq!(a.spawn(||{panic!("full factory ran");async{}}),Err(TaskError::Full));assert!(!a.cancel(bid));assert!(a.cancel(id));assert!(!a.cancel(id));assert_eq!(b.occupied(),1);
}
#[test]fn task_pending_stays_quiet_until_woken(){
 let mut t=Tasks::new(1).unwrap();let(f,p,d,w)=task_probe(false,false,false);t.spawn(||f).unwrap();assert!(t.ready());assert_eq!(t.dispatch(1).polled,1);assert!(!t.ready());assert_eq!(t.dispatch(1).polled,0);w.borrow().as_ref().unwrap().wake_by_ref();assert!(t.ready());assert_eq!(t.dispatch(1).polled,1);assert_eq!(p.get(),2);assert_eq!(d.get(),0);
}
#[test]fn task_self_wake_survives_poll_and_is_bounded(){
 let mut t=Tasks::new(2).unwrap();let(a,p,_,_)=task_probe(true,false,false);let(b,q,_,_)=task_probe(true,false,false);t.spawn(||a).unwrap();t.spawn(||b).unwrap();assert_eq!(t.dispatch(usize::MAX).polled,2);assert_eq!(p.get(),1);assert_eq!(q.get(),1);assert!(t.ready());assert_eq!(t.dispatch(1).polled,1);assert_eq!(p.get(),2);assert_eq!(q.get(),1);assert_eq!(t.dispatch(1).polled,1);assert_eq!(q.get(),2);
}
#[test]fn task_completed_and_cancelled_wakers_cannot_wake_reused_slot(){
 for finish in [false,true]{let mut t=Tasks::new(1).unwrap();let(f,_,d,w)=task_probe(false,finish,false);let old=t.spawn(||f).unwrap();t.dispatch(1);if !finish{assert!(t.cancel(old));}assert_eq!(d.get(),1);let(g,p,_,_)=task_probe(false,false,false);let new=t.spawn(||g).unwrap();assert_ne!(old,new);t.dispatch(1);assert!(!t.ready());w.borrow().as_ref().unwrap().wake_by_ref();assert!(!t.ready());assert!(!t.cancel(old));assert_eq!(t.dispatch(1).polled,0);assert_eq!(p.get(),1);assert!(t.cancel(new));}
}
#[test]fn task_zero_budget_and_many_wakes_coalesce(){
 let mut t=Tasks::new(1).unwrap();let(f,p,_,w)=task_probe(false,false,false);t.spawn(||f).unwrap();assert_eq!(t.dispatch(0).polled,0);assert!(t.ready());t.dispatch(1);for _ in 0..100{w.borrow().as_ref().unwrap().wake_by_ref();}assert_eq!(t.dispatch(64).polled,1);assert_eq!(p.get(),2);assert!(!t.ready());
}
#[test]fn task_completion_and_root_drop_release_owned_futures(){
 let mut t=Tasks::new(2).unwrap();let(a,_,da,wa)=task_probe(true,true,false);let(b,_,db,wb)=task_probe(false,false,false);t.spawn(||a).unwrap();t.spawn(||b).unwrap();assert_eq!(t.dispatch(2).completed,1);assert_eq!(da.get(),1);assert_eq!(t.occupied(),1);drop(t);assert_eq!(db.get(),1);wa.borrow().as_ref().unwrap().wake_by_ref();wb.borrow().as_ref().unwrap().wake_by_ref();
}
#[test]fn task_unwind_detaches_owner_and_keeps_other_tasks(){
 let mut t=Tasks::new(2).unwrap();let(a,_,d,w)=task_probe(false,false,true);let(b,p,_,_)=task_probe(false,true,false);t.spawn(||a).unwrap();t.spawn(||b).unwrap();assert!(std::panic::catch_unwind(std::panic::AssertUnwindSafe(||t.dispatch(2))).is_err());assert_eq!(d.get(),1);assert_eq!(t.occupied(),1);w.borrow().as_ref().unwrap().wake_by_ref();assert_eq!(t.dispatch(2).completed,1);assert_eq!(p.get(),1);
}
#[test]fn task_dispatch_and_wake_have_no_internal_allocation(){
 let mut t=Tasks::new(1).unwrap();let(f,_,_,w)=task_probe(true,false,false);t.spawn(||f).unwrap();LATE_ALLOCS.with(|x|x.set(0));WATCH_ALLOCS.with(|x|x.set(true));t.dispatch(1);w.borrow().as_ref().unwrap().wake_by_ref();t.dispatch(1);WATCH_ALLOCS.with(|x|x.set(false));assert_eq!(LATE_ALLOCS.with(|x|x.get()),0);
}
#[test]fn task_fixed_future_cancel_retains_storage_until_terminal(){
 let(p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let mut t=Tasks::new(1).unwrap();let task=t.spawn(||async move{let _=f.await;}).unwrap();t.dispatch(1);assert!(!t.ready());assert!(t.cancel(task));assert_eq!(p.aborts.get(),1);assert!(d.read(id(0,1),range(8)).is_err());unsafe{d.receive(typed_record(cookie,-2,0)).unwrap();}assert!(d.read(id(0,1),range(8)).is_ok());assert_eq!(t.occupied(),0);
}
#[test]fn task_fixed_completion_wakes_and_result_stays_owned(){
 let(p,mut d)=future_driver();let mut f=d.read(id(0,1),range(0)).unwrap();let cookie=unsafe{f.publish().unwrap()};let result=Rc::new(std::cell::RefCell::new(None));let out=result.clone();let mut t=Tasks::new(1).unwrap();t.spawn(||async move{*out.borrow_mut()=Some(f.await.unwrap());}).unwrap();t.dispatch(1);assert!(!t.ready());unsafe{d.receive(typed_record(cookie,4,0)).unwrap();}assert!(t.ready());assert_eq!(t.dispatch(1).completed,1);drop(d);assert_eq!(p.freed.get(),0);assert_eq!(result.borrow().as_ref().unwrap().bytes().unwrap().len(),4);result.borrow_mut().take();assert_eq!(p.freed.get(),1);
}

fn main(){let t=Tasks::new(1).unwrap();let _=t.clone();}
