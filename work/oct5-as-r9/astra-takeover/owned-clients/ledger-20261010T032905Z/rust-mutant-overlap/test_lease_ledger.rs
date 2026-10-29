#![allow(dead_code)]
#[path="/Users/northkillpd/projects/thylacine-corona/usr/lib/libthyla-rs/src/loom/service_abi.rs"]
mod service_abi;
#[path="lease_ledger.rs"] mod ledger;
use ledger::*;
use service_abi::*;
fn pool() -> SlotRef { SlotRef {slot:4,reserved:0,incarnation:17} }
fn ranges() -> [PoolMember;2] { [PoolMember {buffer_index:0,reserved:0,offset:0,length:4}, PoolMember {buffer_index:0,reserved:0,offset:4,length:4}] }
fn book() -> LeaseLedger { LeaseLedger::new(pool(),&ranges(),&[8]).unwrap() }
fn receipt(member:u32,lease:u64) -> BufferReceipt { BufferReceipt {pool:pool(),member,reserved:0,lease} }
fn zero() -> BufferReceipt { BufferReceipt {pool:SlotRef {slot:0,reserved:0,incarnation:0},member:0,reserved:0,lease:0} }
#[test] fn final_and_out_of_order_return_preserve_earlier_payload() {
 let mut b=book();let a=b.accept_read(4,5,receipt(0,11)).unwrap().unwrap();
 let c=b.accept_read(2,4,receipt(1,9)).unwrap().unwrap(); // cross-member delivery need not sort lease numbers
 assert_eq!(a.length(),4);assert_eq!(c.length(),2);assert_eq!(a.receipt(),receipt(0,11));
 assert_eq!(b.outstanding(),2);assert!(b.accept_read(0,0,zero()).unwrap().is_none());
 assert_eq!(b.outstanding(),2);b.begin_return(&c).unwrap();b.finish_return(&c,0).unwrap();
 assert_eq!(b.outstanding(),1);
 assert!(matches!(b.accept_read(1,4,receipt(0,12)),Err(LeaseError::Busy)));
 b.begin_return(&a).unwrap();b.finish_return(&a,0).unwrap();assert_eq!(b.outstanding(),0);
}
#[test] fn stale_and_failed_returns_cannot_recycle() {
 let mut b=book();let a=b.accept_read(4,4,receipt(0,1)).unwrap().unwrap();
 assert!(matches!(b.finish_return(&a,0),Err(LeaseError::State)));
 b.begin_return(&a).unwrap();b.finish_return(&a,-2).unwrap();assert_eq!(b.outstanding(),1);
 assert!(matches!(b.accept_read(4,4,receipt(0,2)),Err(LeaseError::Busy)));
 b.begin_return(&a).unwrap();b.finish_return(&a,0).unwrap();
 assert!(matches!(b.begin_return(&a),Err(LeaseError::State)));
 assert!(matches!(b.accept_read(4,4,receipt(0,1)),Err(LeaseError::Stale)));
 let next=b.accept_read(4,4,receipt(0,2)).unwrap().unwrap();
 assert!(matches!(b.begin_return(&a),Err(LeaseError::Stale)));
 b.begin_return(&next).unwrap();b.finish_return(&next,1).unwrap();assert_eq!(b.outstanding(),1);
 b.finish_return(&next,0).unwrap();assert_eq!(b.outstanding(),0);
}
#[test] fn owner_namespace_prevents_cross_ring_token_return() {
 let mut a=book();let mut b=book();let token=a.accept_read(4,4,receipt(0,1)).unwrap().unwrap();
 let own=b.accept_read(4,4,receipt(0,1)).unwrap().unwrap();
 assert!(matches!(b.begin_return(&token),Err(LeaseError::Stale)));
 assert_eq!(b.outstanding(),1);b.begin_return(&own).unwrap();b.finish_return(&own,0).unwrap();
 assert_eq!(a.outstanding(),1);
}
#[test] fn malformed_receipts_do_not_mutate_ownership() {
 let mut b=book();
 for (result,flags) in [(0,4),(-1,4),(5,4),(4,0),(4,1),(4,6),(0,1)] {
  assert!(b.accept_read(result,flags,receipt(0,1)).is_err());assert_eq!(b.outstanding(),0);
 }
 let mut wrong=receipt(0,1);wrong.pool.incarnation+=1;
 assert!(b.accept_read(4,4,wrong).is_err());wrong=receipt(0,1);wrong.reserved=1;
 assert!(b.accept_read(4,4,wrong).is_err());wrong=receipt(0,1);wrong.pool.reserved=1;
 assert!(b.accept_read(4,4,wrong).is_err());
 assert!(b.accept_read(4,4,receipt(2,1)).is_err());assert!(b.accept_read(4,4,receipt(0,0)).is_err());
 assert!(b.accept_read(0,0,receipt(0,1)).is_err());assert_eq!(b.outstanding(),0);
 assert!(b.accept_read(-125,0,zero()).unwrap().is_none());assert_eq!(b.outstanding(),0);
}
#[test] fn geometry_is_checked_before_any_receipt() {
 let mut r=ranges();r[1].offset=3;assert!(LeaseLedger::new(pool(),&r,&[8]).is_err());
 r=ranges();r[1].offset=u64::MAX;assert!(LeaseLedger::new(pool(),&r,&[8]).is_err());
 r=ranges();r[1].buffer_index=1;assert!(LeaseLedger::new(pool(),&r,&[8]).is_err());
 r=ranges();r[1].reserved=1;assert!(LeaseLedger::new(pool(),&r,&[8]).is_err());
 r=ranges();r[1].length=0;assert!(LeaseLedger::new(pool(),&r,&[8]).is_err());
 assert!(LeaseLedger::new(pool(),&[],&[8]).is_err());
 assert!(LeaseLedger::new(pool(),&ranges(),&[7]).is_err());
 let mut p=pool();p.incarnation=0;assert!(LeaseLedger::new(p,&ranges(),&[8]).is_err());
}
#[test] fn bounded_members_and_lease_wrap() {
 let ranges:[PoolMember;64]=core::array::from_fn(|i|PoolMember{buffer_index:0,reserved:0,offset:i as u64,length:1});
 let mut b=LeaseLedger::new(pool(),&ranges,&[64]).unwrap();let mut tokens=Vec::new();
 for i in 0..64 {tokens.push(b.accept_read(1,4,receipt(i,u64::MAX)).unwrap().unwrap());}
 assert_eq!(b.outstanding(),64);
 assert!(b.accept_read(1,4,receipt(64,1)).is_err());
 for t in &tokens {b.begin_return(t).unwrap();b.finish_return(t,0).unwrap();}
 assert_eq!(b.outstanding(),0);assert!(matches!(b.accept_read(1,4,receipt(0,1)),Err(LeaseError::Stale)));
 let too_many=[ranges[0];65];assert!(LeaseLedger::new(pool(),&too_many,&[64]).is_err());
}
