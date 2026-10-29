//! Bounded, creator-thread retirement owner. No event-loop or native SQ adapter.
extern crate alloc;
use alloc::{rc::Rc,vec::Vec};
use core::cell::RefCell;
use super::{fixed_requests::{FixedDriver,ReadRequest,WriteRequest,Error as DriverError},ring_payload::RingBackend,paired_cq::Record,service_abi::{SlotRef,PoolMember}};
#[derive(Debug,PartialEq,Eq)]pub(crate) enum Error{Capacity,Full,Closed,Driver(DriverError)}
#[derive(Debug,PartialEq,Eq)]pub(crate) struct Progress{pub visited:usize,pub reclaimed:usize,pub occupied:usize}
struct Entry<B:RingBackend>{driver:FixedDriver<B>,retiring:bool}
struct State<B:RingBackend>{slots:Vec<Option<Entry<B>>>,cursor:usize,open:bool}
pub(crate) struct RetirementExecutor<B:RingBackend>{state:Rc<RefCell<State<B>>>}
pub(crate) struct ManagedClient<B:RingBackend>{state:Rc<RefCell<State<B>>>,slot:usize}
impl<B:RingBackend> RetirementExecutor<B>{
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
 pub(crate) fn admit(&mut self,make:impl FnOnce()->Result<FixedDriver<B>,DriverError>)->Result<ManagedClient<B>,Error>{
  let slot={let s=self.state.borrow();if !s.open{return Err(Error::Closed);}s.slots.iter().position(Option::is_none).ok_or(Error::Full)?};
  let driver=make().map_err(Error::Driver)?;
  let mut s=self.state.borrow_mut();assert!(s.slots[slot].is_none());s.slots[slot]=Some(Entry{driver,retiring:false});
  Ok(ManagedClient{state:self.state.clone(),slot})
 }
 /// Visit at most min(budget,capacity) cells, once each, in rotating order.
 /// No allocation/wait/retry loop. Busy entries retain their exact owner and
 /// admission charge. The caller must arrange later progress after local events.
 pub(crate) fn progress(&mut self,budget:usize)->Progress{
  let mut s=self.state.borrow_mut();let visited=budget.min(s.slots.len());let mut reclaimed=0;
  for _ in 0..visited{let i=s.cursor;s.cursor=i;
   if s.slots[i].as_mut().is_some_and(|e|e.retiring&&e.driver.progress()){
    s.slots[i]=None;reclaimed+=1;
   }
  }
  Progress{visited,reclaimed,occupied:s.slots.iter().filter(|e|e.is_some()).count()}
 }
 pub(crate) fn occupied(&self)->usize{self.state.borrow().slots.iter().filter(|e|e.is_some()).count()}
}
impl<B:RingBackend> Drop for RetirementExecutor<B>{fn drop(&mut self){
 let mut s=self.state.borrow_mut();s.open=false;
 // Executor shutdown cannot promise continued driving. Close each descriptor
 // explicitly; independent kernel pins and owned views remain authoritative.
 for e in s.slots.iter_mut().flatten(){e.driver.close();}
}}
impl<B:RingBackend> ManagedClient<B>{
 pub(crate) fn read(&self,scope:SlotRef,range:PoolMember)->Result<ReadRequest<B>,Error>{let s=self.state.borrow();if !s.open{return Err(Error::Closed);}s.slots[self.slot].as_ref().unwrap().driver.read(scope,range).map_err(Error::Driver)}
 pub(crate) fn write(&self,scope:SlotRef,range:PoolMember,input:&[u8])->Result<WriteRequest<B>,Error>{let s=self.state.borrow();if !s.open{return Err(Error::Closed);}s.slots[self.slot].as_ref().unwrap().driver.write(scope,range,input).map_err(Error::Driver)}
 /// Same authenticated, locally quiescent paired-CQ contract as FixedDriver.
 pub(crate) unsafe fn receive(&mut self,record:Record)->Result<(),Error>{let mut s=self.state.borrow_mut();if !s.open{return Err(Error::Closed);}unsafe{s.slots[self.slot].as_mut().unwrap().driver.receive(record)}.map_err(Error::Driver)}
}
impl<B:RingBackend> Drop for ManagedClient<B>{fn drop(&mut self){
 let mut s=self.state.borrow_mut();let e=s.slots[self.slot].as_mut().expect("attached slot never reclaimed");
 e.retiring=true;e.driver.begin_retire();
 // Existing cell becomes retirement record; no allocation and no lost owner.
}}
