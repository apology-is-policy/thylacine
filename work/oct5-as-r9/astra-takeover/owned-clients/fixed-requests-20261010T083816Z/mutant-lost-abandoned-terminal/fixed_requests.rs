//! Typed fixed request ownership, uninstalled. No SQ publishing or Future wakeup.
extern crate alloc;
use alloc::rc::Rc;
use core::cell::RefCell;
use super::{paired_cq::{Completion,Record},request_book::{RequestBook,Kind,Error as BookError},ring_payload::{RingBackend,RingOwner,RingClient,FixedIo,Error as StorageError},service_abi::{SlotRef,PoolMember}};
#[derive(Debug,PartialEq,Eq)]pub(crate) enum Error{Book(BookError),Storage(StorageError),Closing,Consumed}
struct Core<B:RingBackend>{owner:RingOwner<B>,client:Option<RingClient<B>>,book:RequestBook,failed:bool}
impl<B:RingBackend> Core<B>{
 fn retire(&mut self){self.client.take();self.owner.begin_retire();}
 fn fail(&mut self){self.failed=true;self.retire();self.owner.close();}
}
pub(crate) struct FixedDriver<B:RingBackend>{core:Rc<RefCell<Core<B>>>}
struct Request<B:RingBackend>{core:Rc<RefCell<Core<B>>>,fixed:Option<FixedIo<B>>,cookie:u64,published:bool}
pub(crate) struct ReadRequest<B:RingBackend>{inner:Request<B>}
pub(crate) struct WriteRequest<B:RingBackend>{inner:Request<B>}
pub(crate) struct ReadCompletion<B:RingBackend>{fixed:FixedIo<B>,completion:Completion}
impl<B:RingBackend> FixedDriver<B>{
 pub(crate) fn new(backend:B,cq_capacity:usize)->Result<Self,Error>{
  // Allocate request metadata before creating the live wrapper. On bad capacity,
  // close the consumed backend explicitly (its pins still obey backend contract).
  let book=match RequestBook::new(cq_capacity){Ok(b)=>b,Err(e)=>{backend.begin_retire();backend.close();return Err(Error::Book(e));}};
  let(owner,client)=RingOwner::new(backend).map_err(Error::Storage)?;
  Ok(Self{core:Rc::new(RefCell::new(Core{owner,client:Some(client),book,failed:false}))})
 }
 fn prepare(&self,scope:SlotRef,range:PoolMember,input:Option<&[u8]>)->Result<Request<B>,Error>{
  let mut core=self.core.borrow_mut();if core.failed||core.client.is_none(){return Err(Error::Closing);}
  // Match storage limits before the narrowing cast or request reservation.
  if range.length==0||range.length>i32::MAX as u64{return Err(Error::Storage(StorageError::Geometry));}
  let kind=if input.is_some(){Kind::FixedWrite(range.length as u32)}else{Kind::FixedRead(range.length as u32)};
  let cookie=core.book.reserve(scope,kind).map_err(Error::Book)?;
  let storage=core.client.as_ref().unwrap().reserve_fixed(cookie,scope,range,input.is_some());
  let mut fixed=match storage{Ok(f)=>f,Err(e)=>{core.book.cancel_unpublished(cookie).expect("fresh request");return Err(Error::Storage(e));}};
  if let Some(bytes)=input{if let Err(e)=fixed.write_from(bytes){core.book.cancel_unpublished(cookie).expect("fresh request");return Err(Error::Storage(e));}}
  Ok(Request{core:self.core.clone(),fixed:Some(fixed),cookie,published:false})
 }
 pub(crate) fn read(&self,scope:SlotRef,range:PoolMember)->Result<ReadRequest<B>,Error>{Ok(ReadRequest{inner:self.prepare(scope,range,None)?})}
 pub(crate) fn write(&self,scope:SlotRef,range:PoolMember,bytes:&[u8])->Result<WriteRequest<B>,Error>{Ok(WriteRequest{inner:self.prepare(scope,range,Some(bytes))?})}
 /// # Safety
 /// Paired completion from this driver's exclusive native mapping, after local
 /// fixed-buffer access has ended. It is never synthesized from ENTER/ABORT rc.
 pub(crate) unsafe fn receive(&mut self,record:Record)->Result<(),Error>{
  let mut c=self.core.borrow_mut();if c.failed{return Err(Error::Closing);}
  let delivery=match c.book.accept(record){Ok(d)=>d,Err(e)=>{c.fail();return Err(Error::Book(e));}};
  if let Err(e)=unsafe{c.owner.finish_fixed(delivery.cookie,delivery.record.cqe.result)}{c.fail();return Err(Error::Storage(e));}
  if false && delivery.abandoned{c.book.take_terminal(delivery.cookie).expect("fixed terminal");}
  Ok(())
 }
 pub(crate) fn begin_retire(&mut self){self.core.borrow_mut().retire();}
 /// No completion drain or allocation here; bounded preallocated owner scans.
 /// Once true, all fixed views/requests are gone and whole-owner local access
 /// is retired, so abandoned completion metadata can finally be discarded.
 pub(crate) fn progress(&mut self)->bool{let mut c=self.core.borrow_mut();if c.owner.progress(){unsafe{c.book.clear_retired();}true}else{false}}
 pub(crate) fn occupied(&self)->usize{self.core.borrow().book.occupied()}
}
impl<B:RingBackend> Drop for FixedDriver<B>{fn drop(&mut self){self.core.borrow_mut().retire();}}
impl<B:RingBackend> Request<B>{
 /// # Safety
 /// Prepare the SQE with returned cookie, activate immediately before release
 /// SQ tail, with no fallible work after success. Native publication is parked.
 unsafe fn publish(&mut self)->Result<u64,Error>{
  let mut c=self.core.borrow_mut();if c.failed||c.client.is_none(){return Err(Error::Closing);}
  let f=self.fixed.as_mut().ok_or(Error::Consumed)?;
  if self.published{return Err(Error::Book(BookError::State));}
  unsafe{f.publish().map_err(Error::Storage)?;}
  // Same creator, private cookie and no callback between these transitions.
  // Fresh prepared book entry makes this transition infallible by construction.
  c.book.publish(self.cookie).expect("owned prepared request");self.published=true;Ok(self.cookie)
 }
 fn take(&mut self)->Result<Option<(Completion,FixedIo<B>)>,Error>{
  if self.fixed.is_none(){return Err(Error::Consumed);}
  let mut c=self.core.borrow_mut();if c.failed{return Err(Error::Closing);}
  if !self.published{return if c.client.is_none(){Err(Error::Closing)}else{Ok(None)};}
  match c.book.take_terminal(self.cookie){Ok(completion)=>Ok(Some((completion,self.fixed.take().unwrap()))),Err(BookError::State)=>if c.client.is_none(){Err(Error::Closing)}else{Ok(None)},Err(e)=>Err(Error::Book(e))}
 }
}
impl<B:RingBackend> Drop for Request<B>{fn drop(&mut self){
 if self.fixed.is_none(){return;}
 {let mut c=self.core.borrow_mut();
  if !self.published{c.book.cancel_unpublished(self.cookie).expect("unpublished owner");}
  else{match c.book.take_terminal(self.cookie){Ok(_)=>{},Err(BookError::State)=>{c.book.abandon(self.cookie).expect("active owner");},Err(_)=>{c.fail();}}}
 }
 // FixedIo drop performs the single scope abort for active abandonment. Book
 // abandonment supplies metadata only; never issue the same abort twice here.
 self.fixed.take();
}}
impl<B:RingBackend> ReadRequest<B>{
 pub(crate) unsafe fn publish(&mut self)->Result<u64,Error>{unsafe{self.inner.publish()}}
 pub(crate) fn try_complete(&mut self)->Result<Option<ReadCompletion<B>>,Error>{Ok(self.inner.take()?.map(|(completion,fixed)|ReadCompletion{fixed,completion}))}
}
impl<B:RingBackend> WriteRequest<B>{
 pub(crate) unsafe fn publish(&mut self)->Result<u64,Error>{unsafe{self.inner.publish()}}
 pub(crate) fn try_complete(&mut self)->Result<Option<Completion>,Error>{Ok(self.inner.take()?.map(|(completion,_fixed)|completion))}
}
impl<B:RingBackend> ReadCompletion<B>{
 pub(crate) fn result(&self)->i32{self.completion.result}
 pub(crate) fn bytes(&self)->Result<&[u8],StorageError>{self.fixed.bytes()}
}
