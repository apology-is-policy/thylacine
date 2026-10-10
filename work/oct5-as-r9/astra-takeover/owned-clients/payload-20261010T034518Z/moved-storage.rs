#![allow(dead_code)]
#[path="/Users/northkillpd/projects/thylacine-corona/usr/lib/libthyla-rs/src/loom/service_abi.rs"] mod service_abi;
mod lease_ledger;
mod payload_owner;
use payload_owner::*;
use service_abi::*;
use std::{cell::{Cell,UnsafeCell},ptr::NonNull,rc::Rc};
#[derive(Default)]struct Probe{rc:Cell<i64>,returns:Cell<usize>,retires:Cell<usize>,closes:Cell<usize>,freed:Cell<usize>,retired:Cell<bool>}
struct Backing{bytes:Box<UnsafeCell<[u8;512]>>,probe:Rc<Probe>}
impl Backing{fn new(probe:Rc<Probe>)->Self{let mut bytes=[0u8;512];for i in 0..64{bytes[i*8..i*8+8].fill(65+i as u8);}Self{bytes:Box::new(UnsafeCell::new(bytes)),probe}}}
// Host simulation only: exclusive heap storage, eight bytes/member. A successful
// RETURN immediately rewrites only that member to model kernel reuse.
unsafe impl PayloadBackend for Backing{
 fn buffer_count(&self)->usize{1}
 fn buffer_len(&self,_:usize)->usize{512}
 fn buffer(&self,_:usize)->NonNull<u8>{NonNull::new(self.bytes.get().cast::<u8>()).unwrap()}
 fn return_buffer(&self,r:BufferReceipt)->i64{self.probe.returns.set(self.probe.returns.get()+1);let rc=self.probe.rc.get();if rc==0{unsafe{self.bytes.get().cast::<u8>().add(r.member as usize*8).write_bytes(0xdd,8);}}rc}
 fn begin_retire(&self){self.probe.retires.set(self.probe.retires.get()+1);}
 fn retired(&self)->bool{self.probe.retired.get()}
 fn close(&self){self.probe.closes.set(self.probe.closes.get()+1);}
}
impl Drop for Backing{fn drop(&mut self){assert_eq!(self.probe.closes.get(),1);self.probe.freed.set(self.probe.freed.get()+1);}}
fn poolref()->SlotRef{SlotRef{slot:2,reserved:0,incarnation:31}}
fn receipt(member:u32)->BufferReceipt{BufferReceipt{pool:poolref(),member,reserved:0,lease:member as u64+1}}
fn ranges()->[PoolMember;64]{std::array::from_fn(|i|PoolMember{buffer_index:0,reserved:0,offset:(i*8)as u64,length:8})}
fn fixture()->(Rc<Probe>,RetirementOwner<Backing>,PayloadPool<Backing>){let p=Rc::new(Probe::default());let (r,c)=RetirementOwner::new(Backing::new(p.clone()),poolref(),&ranges()).unwrap();(p,r,c)}
fn accept(pool:&PayloadPool<Backing>,member:u32)->PayloadLease<Backing>{unsafe{pool.accept_read(8,4,receipt(member))}.unwrap().unwrap()}
#[test]fn held_a_survives_b_return_and_final(){let(p,mut root,pool)=fixture();let a=accept(&pool,0);let b=accept(&pool,1);assert_eq!(a.bytes(),b"AAAAAAAA");assert_eq!(b.bytes(),b"BBBBBBBB");assert!(b.try_return().is_ok());assert_eq!(a.bytes(),b"AAAAAAAA");let zero=BufferReceipt{pool:SlotRef{slot:0,reserved:0,incarnation:0},member:0,reserved:0,lease:0};assert!(unsafe{pool.accept_read(0,0,zero)}.unwrap().is_none());assert_eq!(root.outstanding(),1);assert!(!root.progress());drop(pool);assert_eq!(p.retires.get(),1);p.retired.set(true);assert!(!root.progress());assert_eq!(a.bytes(),b"AAAAAAAA");drop(a);assert!(root.progress());drop(root);assert_eq!(p.freed.get(),1);}
#[test]fn failed_explicit_and_drop_returns_keep_storage(){let(p,mut root,pool)=fixture();p.rc.set(-2);let a=accept(&pool,0);let (rc,a)=a.try_return().err().unwrap();assert_eq!(rc,-2);assert_eq!(a.bytes(),b"AAAAAAAA");assert_eq!(root.outstanding(),1);drop(a);assert_eq!(root.pending_returns(),1);assert!(!root.progress());assert_eq!(root.pending_returns(),1);assert_eq!(p.freed.get(),0);p.rc.set(1);assert!(!root.progress());assert_eq!(root.pending_returns(),1);p.rc.set(0);assert!(!root.progress());assert_eq!(root.pending_returns(),0);assert_eq!(root.outstanding(),0);drop(pool);assert!(!root.progress());p.retired.set(true);assert!(root.progress());drop(root);assert_eq!(p.freed.get(),1);}
#[test]fn all64_failed_drop_slots_are_bounded_and_retire_without_return_success(){let(p,mut root,pool)=fixture();p.rc.set(-11);let held:Vec<_>=(0..64).map(|i|accept(&pool,i)).collect();assert_eq!(root.outstanding(),64);drop(held);assert_eq!(root.pending_returns(),64);drop(pool);let before=p.returns.get();assert!(!root.progress());assert_eq!(p.returns.get()-before,64);assert_eq!(p.freed.get(),0);p.retired.set(true);assert!(root.progress());assert_eq!(root.outstanding(),64);drop(root);assert_eq!(p.freed.get(),1);}
#[test]fn controller_teardown_keeps_live_payload_mapping(){let(p,root,pool)=fixture();let a=accept(&pool,0);drop(root);assert_eq!(p.closes.get(),1);assert_eq!(p.freed.get(),0);assert!(matches!(unsafe{pool.accept_read(8,4,receipt(1))},Err(PayloadError::Closing)));assert_eq!(a.bytes(),b"AAAAAAAA");drop(pool);assert_eq!(p.freed.get(),0);drop(a);assert_eq!(p.freed.get(),1);assert_eq!(p.closes.get(),1);}
#[test]fn invalid_geometry_closes_before_backing_drop(){let p=Rc::new(Probe::default());let mut r=ranges();r[0].offset=512;assert!(RetirementOwner::new(Backing::new(p.clone()),poolref(),&r).is_err());assert_eq!(p.retires.get(),1);assert_eq!(p.closes.get(),1);assert_eq!(p.freed.get(),1);}
#[test]fn malformed_or_duplicate_receipt_never_mints_view(){let(p,root,pool)=fixture();let a=accept(&pool,0);assert!(unsafe{pool.accept_read(8,4,receipt(0))}.is_err());assert!(unsafe{pool.accept_read(9,4,receipt(1))}.is_err());assert_eq!(root.outstanding(),1);assert_eq!(a.bytes(),b"AAAAAAAA");drop(a);drop(pool);drop(root);assert_eq!(p.freed.get(),1);}

fn main(){let p=Rc::new(Probe::default());let b=Backing::new(p);let _=RetirementOwner::new(b,poolref(),&ranges());let _pointer=b.bytes.get();}
