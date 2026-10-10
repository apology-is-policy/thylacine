//! Bounded, creator-thread retirement owner. No event-loop or native SQ adapter.
extern crate alloc;
use alloc::{rc::Rc,vec::Vec};
use core::cell::RefCell;
use super::{fixed_futures::{FutureDriver,ReadFuture,WriteFuture,WakeBatch},fixed_requests::{FixedDriver,ReadRequest,WriteRequest,Error as DriverError},ring_payload::RingBackend,paired_cq::Record,service_abi::{SlotRef,PoolMember}};
#[derive(Debug,PartialEq,Eq)]pub(crate) enum Error{Capacity,Full,Closed,Driver(DriverError)}
#[derive(Debug,PartialEq,Eq)]pub(crate) struct Progress{pub visited:usize,pub reclaimed:usize,pub occupied:usize}
pub(crate) trait Retirable{
 type Read;type Write;
 fn read(&self,scope:SlotRef,range:PoolMember)->Result<Self::Read,DriverError>;
 fn write(&self,scope:SlotRef,range:PoolMember,input:&[u8])->Result<Self::Write,DriverError>;
 unsafe fn receive(&mut self,record:Record)->(Result<(),DriverError>,WakeBatch);
 fn retire(&mut self)->WakeBatch;fn close(&mut self)->WakeBatch;fn progress(&mut self)->bool;
}
impl<B:RingBackend> Retirable for FixedDriver<B>{
 type Read=ReadRequest<B>;type Write=WriteRequest<B>;
 fn read(&self,s:SlotRef,r:PoolMember)->Result<Self::Read,DriverError>{FixedDriver::read(self,s,r)}
 fn write(&self,s:SlotRef,r:PoolMember,b:&[u8])->Result<Self::Write,DriverError>{FixedDriver::write(self,s,r,b)}
 unsafe fn receive(&mut self,r:Record)->(Result<(),DriverError>,WakeBatch){(unsafe{FixedDriver::receive(self,r)},WakeBatch::empty())}
 fn retire(&mut self)->WakeBatch{self.begin_retire();WakeBatch::empty()}
 fn close(&mut self)->WakeBatch{FixedDriver::close(self);WakeBatch::empty()}
 fn progress(&mut self)->bool{FixedDriver::progress(self)}
}
impl<B:RingBackend> Retirable for FutureDriver<B>{
 type Read=ReadFuture<B>;type Write=WriteFuture<B>;
 fn read(&self,s:SlotRef,r:PoolMember)->Result<Self::Read,DriverError>{FutureDriver::read(self,s,r)}
 fn write(&self,s:SlotRef,r:PoolMember,b:&[u8])->Result<Self::Write,DriverError>{FutureDriver::write(self,s,r,b)}
 unsafe fn receive(&mut self,r:Record)->(Result<(),DriverError>,WakeBatch){unsafe{self.receive_deferred(r)}}
 fn retire(&mut self)->WakeBatch{self.retire_deferred()}
 fn close(&mut self)->WakeBatch{self.close_deferred()}
 fn progress(&mut self)->bool{FutureDriver::progress(self)}
}
struct Entry<D:Retirable>{driver:D,retiring:bool}
struct State<D:Retirable>{slots:Vec<Option<Entry<D>>>,cursor:usize,open:bool}
pub(crate) struct RetireQueue<D:Retirable>{state:Rc<RefCell<State<D>>>}
pub(crate) struct Managed<D:Retirable>{state:Rc<RefCell<State<D>>>,slot:usize}
pub(crate) type RetirementExecutor<B> = RetireQueue<FixedDriver<B>>;
pub(crate) type ManagedClient<B> = Managed<FixedDriver<B>>;
pub(crate) type FutureExecutor<B> = RetireQueue<FutureDriver<B>>;
impl<D:Retirable> RetireQueue<D>{
 /// Conservative library bound, not a new kernel quota. Every admitted wrapper
 /// reserves its retirement cell before its factory may create native resources.
 pub(crate) fn new(capacity:usize)->Result<Self,Error>{
  if capacity==0||capacity>64{return Err(Error::Capacity);}
  let mut slots=Vec::with_capacity(capacity);slots.resize_with(capacity,||None);
  Ok(Self{state:Rc::new(RefCell::new(State{slots,cursor:0,open:true}))})
 }
 /// Factory runs only after capacity check, outside the shared-state borrow.
 /// Unique &mut self and creator-thread ownership prevent concurrent admission;
 /// client callbacks can only retire their own occupied cell. Failure leaves the
 /// free cell free and must clean any factory-owned partial resources normally.
 pub(crate) fn admit(&mut self,make:impl FnOnce()->Result<D,DriverError>)->Result<Managed<D>,Error>{
  let slot={let s=self.state.borrow();if !s.open{return Err(Error::Closed);}s.slots.iter().position(Option::is_none).ok_or(Error::Full)?};
  let driver=make().map_err(Error::Driver)?;
  let mut s=self.state.borrow_mut();assert!(s.slots[slot].is_none());s.slots[slot]=Some(Entry{driver,retiring:false});
  Ok(Managed{state:self.state.clone(),slot})
 }
 /// Visit at most min(budget,capacity) cells, once each, in rotating order.
 /// No allocation/wait/retry loop. Busy entries retain their exact owner and
 /// admission charge. The caller must arrange later progress after local events.
 pub(crate) fn progress(&mut self,budget:usize)->Progress{
  let visited={let s=self.state.borrow();budget.min(s.slots.len())};let mut reclaimed=0;
  for _ in 0..visited{
   let detached={let mut s=self.state.borrow_mut();let i=s.cursor;s.cursor=(i+1)%s.slots.len();
    if s.slots[i].as_mut().is_some_and(|e|e.retiring&&e.driver.progress()){s.slots[i].take()}else{None}};
   if detached.is_some(){reclaimed+=1;}drop(detached); // Driver Drop outside borrow.
  }
  Progress{visited,reclaimed,occupied:self.occupied()}
 }
 pub(crate) fn occupied(&self)->usize{self.state.borrow().slots.iter().filter(|e|e.is_some()).count()}
}
impl<D:Retirable> Drop for RetireQueue<D>{fn drop(&mut self){
 let capacity={let mut s=self.state.borrow_mut();s.open=false;s.slots.len()};
 for i in 0..capacity{
  let batch={let mut s=self.state.borrow_mut();match s.slots[i].as_mut(){Some(e)=>e.driver.close(),None=>WakeBatch::empty()}};
  let _borrow=self.state.borrow_mut();batch.dispatch(); // Allows task callbacks to drop clients without borrow conflict.
 }
}}
impl<D:Retirable> Managed<D>{
 pub(crate) fn read(&self,scope:SlotRef,range:PoolMember)->Result<D::Read,Error>{let s=self.state.borrow();if !s.open{return Err(Error::Closed);}s.slots[self.slot].as_ref().unwrap().driver.read(scope,range).map_err(Error::Driver)}
 pub(crate) fn write(&self,scope:SlotRef,range:PoolMember,input:&[u8])->Result<D::Write,Error>{let s=self.state.borrow();if !s.open{return Err(Error::Closed);}s.slots[self.slot].as_ref().unwrap().driver.write(scope,range,input).map_err(Error::Driver)}
 /// Same authenticated, locally quiescent paired-CQ contract as FixedDriver.
 pub(crate) unsafe fn receive(&mut self,record:Record)->Result<(),Error>{
  let(result,batch)={let mut s=self.state.borrow_mut();if !s.open{return Err(Error::Closed);}unsafe{s.slots[self.slot].as_mut().unwrap().driver.receive(record)}};
  batch.dispatch();result.map_err(Error::Driver)
 }
}
impl<D:Retirable> Drop for Managed<D>{fn drop(&mut self){
 let batch={let mut s=self.state.borrow_mut();let e=s.slots[self.slot].as_mut().expect("attached slot never reclaimed");e.retiring=true;e.driver.retire()};
 batch.dispatch(); // Existing cell retains driver; no retirement-node allocation.
}}
