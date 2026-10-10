//! Owned fixed-I/O Futures over the simulated/adapter-only typed controller.
//! This is not a SQ publisher or a sleeping event loop. Native integration gated.
extern crate alloc;
use alloc::rc::Rc;
use core::{cell::RefCell,future::Future,pin::Pin,task::{Context,Poll,Waker}};
use super::{fixed_requests::{FixedDriver,ReadRequest,WriteRequest,ReadCompletion,Error},ring_payload::RingBackend,paired_cq::{Record,Completion},service_abi::{SlotRef,PoolMember}};
struct Registration{waker:Option<Waker>}
struct Hub{closed:bool,slots:[Option<Registration>;64]}
impl Hub{
 fn reserve(&mut self)->Result<usize,Error>{if self.closed{return Err(Error::Closing);}let i=self.slots.iter().position(Option::is_none).ok_or(Error::Storage(super::ring_payload::Error::Busy))?;self.slots[i]=Some(Registration{waker:None});Ok(i)}
 // Moving Wakers out never calls user code. Invoke/drop them only after every
 // RefCell borrow has ended; custom Waker vtables may reenter or allocate.
 fn batch(&mut self)->[Option<Waker>;64]{core::array::from_fn(|i|self.slots[i].as_mut().and_then(|r|r.waker.take()))}
}
fn release(hub:&Rc<RefCell<Hub>>,slot:&mut Option<usize>){if let Some(i)=slot.take(){let old={hub.borrow_mut().slots[i].take()};drop(old);}}
fn wake(batch:[Option<Waker>;64]){for w in batch.into_iter().flatten(){w.wake();}}
fn finish<T>(hub:&Rc<RefCell<Hub>>,slot:&mut Option<usize>,status:Result<Option<T>,Error>,waker:Waker)->Poll<Result<T,Error>>{
 match status{
  Ok(Some(value))=>{release(hub,slot);Poll::Ready(Ok(value))},
  Err(e)=>{release(hub,slot);Poll::Ready(Err(e))},
  Ok(None)=>{
   // Clone happened before checking completion. If clone reentered receive,
   // the subsequent completion check observes it. Replacement/drop happens
   // after registration and outside the borrow, so a new event can wake us.
   if hub.borrow().closed{release(hub,slot);return Poll::Ready(Err(Error::Closing));}
   let old={let mut h=hub.borrow_mut();h.slots[slot.expect("pending future owns registration")].as_mut().unwrap().waker.replace(waker)};
   drop(old);Poll::Pending
  }
 }
}
pub(crate) struct FutureDriver<B:RingBackend>{driver:Option<FixedDriver<B>>,hub:Rc<RefCell<Hub>>}
pub(crate) struct ReadFuture<B:RingBackend>{request:ReadRequest<B>,hub:Rc<RefCell<Hub>>,slot:Option<usize>}
pub(crate) struct WriteFuture<B:RingBackend>{request:WriteRequest<B>,hub:Rc<RefCell<Hub>>,slot:Option<usize>}
impl<B:RingBackend> FutureDriver<B>{
 pub(crate) fn new(driver:FixedDriver<B>)->Self{Self{driver:Some(driver),hub:Rc::new(RefCell::new(Hub{closed:false,slots:core::array::from_fn(|_|None)}))}}
 pub(crate) fn read(&self,scope:SlotRef,range:PoolMember)->Result<ReadFuture<B>,Error>{
  let mut slot=Some(self.hub.borrow_mut().reserve()?);
  match self.driver.as_ref().unwrap().read(scope,range){Ok(request)=>Ok(ReadFuture{request,hub:self.hub.clone(),slot}),Err(e)=>{release(&self.hub,&mut slot);Err(e)}}
 }
 pub(crate) fn write(&self,scope:SlotRef,range:PoolMember,bytes:&[u8])->Result<WriteFuture<B>,Error>{
  let mut slot=Some(self.hub.borrow_mut().reserve()?);
  match self.driver.as_ref().unwrap().write(scope,range,bytes){Ok(request)=>Ok(WriteFuture{request,hub:self.hub.clone(),slot}),Err(e)=>{release(&self.hub,&mut slot);Err(e)}}
 }
 /// Same exact paired-CQ and ended local access contract as FixedDriver.receive.
 pub(crate) unsafe fn receive(&mut self,record:Record)->Result<(),Error>{
  let result=unsafe{self.driver.as_mut().unwrap().receive(record)};
  if result.is_err(){self.close();}else{let batch={self.hub.borrow_mut().batch()};drop(batch);}
  result
 }
 pub(crate) fn close(&mut self){
  self.driver.as_mut().unwrap().close();
  let batch={let mut h=self.hub.borrow_mut();h.closed=true;h.batch()};drop(batch);
 }
 pub(crate) fn registrations(&self)->usize{self.hub.borrow().slots.iter().filter(|s|s.is_some()).count()}
}
impl<B:RingBackend> Drop for FutureDriver<B>{fn drop(&mut self){self.close();}}
impl<B:RingBackend> ReadFuture<B>{pub(crate) unsafe fn publish(&mut self)->Result<u64,Error>{unsafe{self.request.publish()}}}
impl<B:RingBackend> WriteFuture<B>{pub(crate) unsafe fn publish(&mut self)->Result<u64,Error>{unsafe{self.request.publish()}}}
impl<B:RingBackend> Future for ReadFuture<B>{type Output=Result<ReadCompletion<B>,Error>;fn poll(self:Pin<&mut Self>,cx:&mut Context<'_>)->Poll<Self::Output>{let me=self.get_mut();if me.slot.is_none(){return Poll::Ready(Err(Error::Consumed));}let waker=cx.waker().clone();let status=me.request.try_complete();finish(&me.hub,&mut me.slot,status,waker)}}
impl<B:RingBackend> Future for WriteFuture<B>{type Output=Result<Completion,Error>;fn poll(self:Pin<&mut Self>,cx:&mut Context<'_>)->Poll<Self::Output>{let me=self.get_mut();if me.slot.is_none(){return Poll::Ready(Err(Error::Consumed));}let waker=cx.waker().clone();let status=me.request.try_complete();finish(&me.hub,&mut me.slot,status,waker)}}
impl<B:RingBackend> Drop for ReadFuture<B>{fn drop(&mut self){release(&self.hub,&mut self.slot);}}
impl<B:RingBackend> Drop for WriteFuture<B>{fn drop(&mut self){release(&self.hub,&mut self.slot);}}
