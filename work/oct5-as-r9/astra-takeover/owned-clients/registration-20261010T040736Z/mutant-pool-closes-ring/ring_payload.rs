//! Uninstalled full-ring Rust payload ownership; syscall adapter still pending.
extern crate alloc;
use alloc::{boxed::Box,rc::Rc};
use core::{cell::{Cell,RefCell},ptr::NonNull};
use super::{lease_ledger::{LeaseLedger,LeaseToken,LeaseError},service_abi::{SlotRef,PoolMember,BufferReceipt}};
/// # Safety
/// Own stable eager backing, distinct allocations by index, with immutable truthful counts/lengths.
/// Authenticate paired READ records before acceptance; exclude all writers from
/// leased ranges until zero RETURN. Nonzero RETURN preserves leased bytes.
/// Pool retirement stops users of that pool only (scope-wide cancellation must
/// be explicit in the final adapter); pool destruction success proves no kernel
/// reference remains. Ring retirement stops admission globally. `retired` proves
/// all local writers stopped. `close` is bounded/idempotent, preserves leased
/// bytes and independent kernel pins/payer charges beyond backend destruction.
/// No callback may reenter, panic, block on peers or expose mutable backing.
pub(crate) unsafe trait RingBackend {
 /// Zero creates exactly the returned pool. Negative + all-zero output proves
 /// no surviving registration/pins (including copyout rollback). Any other
 /// combination is uncertain and forces whole-owner close. No submission may
 /// use the pool before this function returns and local publication completes.
 fn register_pool(&self,ranges:&[PoolMember],out:&mut SlotRef)->i64;
 fn buffer_count(&self)->usize;
 fn buffer_len(&self,index:usize)->usize;
 fn buffer(&self,index:usize)->NonNull<u8>;
 fn return_buffer(&self,receipt:BufferReceipt)->i64;
 fn retire_pool(&self,pool:SlotRef);
 fn destroy_pool(&self,pool:SlotRef)->i64;
 fn begin_retire(&self);
 fn retired(&self)->bool;
 fn close(&self);
}
struct PoolState {
 identity:SlotRef,ledger:LeaseLedger,ranges:Box<[PoolMember]>,
 pending:Box<[Option<LeaseToken>]>,live:usize,closing:bool,
}
struct State {quarantine:Option<Box<PoolState>>,pools:[Option<Box<PoolState>>;64],last:[u64;64],members:usize,live:usize}
struct Core<B:RingBackend>{backend:B,state:RefCell<State>,closing:Cell<bool>,closed:Cell<bool>}
impl<B:RingBackend> Core<B>{
 fn retire(&self){if !self.closing.replace(true){self.backend.begin_retire();}}
 fn close(&self){self.retire();if !self.closed.replace(true){self.backend.close();}}
 fn give_back(&self,token:LeaseToken)->Result<(),(i64,LeaseToken)>{
  let mut s=self.state.borrow_mut();let p=s.pools[token.receipt().pool.slot as usize].as_mut().expect("live pool");
  let receipt=p.ledger.begin_return(&token).expect("owned exact token");let rc=self.backend.return_buffer(receipt);
  p.ledger.finish_return(&token,rc).expect("same returning token");if rc==0{Ok(())}else{Err((rc,token))}
 }
 fn finish_live(&self,slot:usize){let mut s=self.state.borrow_mut();s.pools[slot].as_mut().unwrap().live-=1;s.live-=1;}
 fn abandon(&self,token:LeaseToken){let slot=token.receipt().pool.slot as usize;
  if let Err((_,token))=self.give_back(token){let i=token.receipt().member as usize;let mut s=self.state.borrow_mut();let p=s.pools[slot].as_mut().unwrap();assert!(p.pending[i].is_none());p.pending[i]=Some(token);}
  self.finish_live(slot);
 }
}
impl<B:RingBackend> Drop for Core<B>{fn drop(&mut self){self.close();}}
pub(crate) struct RingOwner<B:RingBackend>{core:Rc<Core<B>>}
pub(crate) struct RingClient<B:RingBackend>{core:Rc<Core<B>>}
pub(crate) struct Pool<B:RingBackend>{core:Rc<Core<B>>,identity:SlotRef}
pub(crate) struct Payload<B:RingBackend>{core:Rc<Core<B>>,token:Option<LeaseToken>}
#[derive(Debug,PartialEq,Eq)]
pub(crate) enum Error{Closing,Geometry,Busy,Stale,Backend(i64),Uncertain(i64),Ledger(LeaseError)}
impl<B:RingBackend> RingOwner<B>{
 pub(crate) fn new(backend:B)->Result<(Self,RingClient<B>),Error>{
  let count=backend.buffer_count();let valid=count>0&&count<=64&&(0..count).all(|i|{let n=backend.buffer_len(i);n>0&&n<=isize::MAX as usize});
  if !valid{backend.begin_retire();backend.close();return Err(Error::Geometry);}
  let core=Rc::new(Core{backend,state:RefCell::new(State{quarantine:None,pools:core::array::from_fn(|_|None),last:[0;64],members:0,live:0}),closing:Cell::new(false),closed:Cell::new(false)});
  Ok((Self{core:core.clone()},RingClient{core}))
 }
 /// At most64 RETURN and64 pool-destroy attempts; no allocation on this path.
 pub(crate) fn progress(&mut self)->bool{
  for slot in 0..64{
   let count=self.core.state.borrow().pools[slot].as_ref().map_or(0,|p|p.ranges.len());
   for member in 0..count{
    let token=self.core.state.borrow_mut().pools[slot].as_mut().unwrap().pending[member].take();
    if let Some(token)=token{if let Err((_,token))=self.core.give_back(token){self.core.state.borrow_mut().pools[slot].as_mut().unwrap().pending[member]=Some(token);}}
   }
   let identity={let s=self.core.state.borrow();s.pools[slot].as_ref().and_then(|p|if p.closing&&p.live==0&&p.ledger.outstanding()==0{Some(p.identity)}else{None})};
   if let Some(identity)=identity{
    if self.core.backend.destroy_pool(identity)==0{let mut s=self.core.state.borrow_mut();let p=s.pools[slot].take().unwrap();s.members-=p.ranges.len();}
   }
  }
  self.core.closing.get()&&self.core.state.borrow().live==0&&self.core.backend.retired()
 }
 pub(crate) fn members(&self)->usize{self.core.state.borrow().members}
 pub(crate) fn live(&self)->usize{self.core.state.borrow().live}
 pub(crate) fn pending(&self)->usize{self.core.state.borrow().pools.iter().flatten().map(|p|p.pending.iter().filter(|x|x.is_some()).count()).sum()}
}
impl<B:RingBackend> Drop for RingOwner<B>{fn drop(&mut self){self.core.close();}}
impl<B:RingBackend> RingClient<B>{
 fn prepare(&self,ranges:&[PoolMember])->Result<Box<PoolState>,Error>{
  if self.core.closing.get(){return Err(Error::Closing);}
  let s=self.core.state.borrow();
  if ranges.is_empty()||ranges.len()>64{return Err(Error::Geometry);}
  if ranges.len()>64-s.members{return Err(Error::Busy);}
  let mut lengths=[0u64;64];let count=self.core.backend.buffer_count();
  for(i,n)in lengths[..count].iter_mut().enumerate(){*n=self.core.backend.buffer_len(i)as u64;}
  let ledger=LeaseLedger::prepare(ranges,&lengths[..count]).map_err(Error::Ledger)?;
  for p in s.pools.iter().flatten(){for a in p.ranges.iter(){for b in ranges{
   if a.buffer_index==b.buffer_index&&a.offset<b.offset+b.length&&b.offset<a.offset+a.length{return Err(Error::Geometry);}
  }}}
  // Every heap allocation precedes native admission. OOM cannot orphan a pool.
  Ok(Box::new(PoolState{identity:SlotRef{slot:0,reserved:0,incarnation:0},ledger,ranges:ranges.into(),pending:(0..ranges.len()).map(|_|None).collect::<alloc::vec::Vec<_>>().into_boxed_slice(),live:0,closing:false}))
 }
 fn identity_ok(s:&State,identity:SlotRef)->Result<(),Error>{
  if identity.reserved!=0||identity.incarnation==0||identity.slot>=64{return Err(Error::Geometry);}
  let slot=identity.slot as usize;
  if s.pools[slot].is_some(){return Err(Error::Busy);}
  if identity.incarnation<=s.last[slot]{return Err(Error::Stale);}
  Ok(())
 }
 /// Prepare/precharge -> synchronous native admission -> allocation-free bind.
 /// Uncertain results retain reservation/backing and close globally; never reap
 /// a malformed or colliding identity that could name an existing live pool.
 pub(crate) fn register_pool(&self,ranges:&[PoolMember])->Result<Pool<B>,Error>{
  let mut p=self.prepare(ranges)?;
  let mut s=self.core.state.borrow_mut();s.members+=p.ranges.len();
  let mut identity=SlotRef{slot:0,reserved:0,incarnation:0};
  let rc=self.core.backend.register_pool(&p.ranges,&mut identity);
  if rc<0&&identity==(SlotRef{slot:0,reserved:0,incarnation:0}){
   s.members-=p.ranges.len();return Err(Error::Backend(rc));
  }
  if rc!=0||Self::identity_ok(&s,identity).is_err(){
   p.identity=identity;p.closing=true;s.quarantine=Some(p);
   // Drop state borrow before retirement callback; no error can re-open admission.
   drop(s);self.core.close();return Err(Error::Uncertain(rc));
  }
  p.ledger.bind(identity).expect("fresh preallocated ledger");p.identity=identity;
  let slot=identity.slot as usize;s.last[slot]=identity.incarnation;s.pools[slot]=Some(p);
  Ok(Pool{core:self.core.clone(),identity})
 }
 /// # Safety
 /// Historical adapter/testing boundary only: caller owns exact native pool
 /// and cleanup on every error. New adapters use register_pool instead.
 pub(crate) unsafe fn attach_pool(&self,identity:SlotRef,ranges:&[PoolMember])->Result<Pool<B>,Error>{
  if self.core.closing.get(){return Err(Error::Closing);}
  Self::identity_ok(&self.core.state.borrow(),identity)?;
  let mut p=self.prepare(ranges)?;p.ledger.bind(identity).map_err(Error::Ledger)?;p.identity=identity;
  let mut s=self.core.state.borrow_mut();let slot=identity.slot as usize;
  s.members+=ranges.len();s.last[slot]=identity.incarnation;s.pools[slot]=Some(p);
  Ok(Pool{core:self.core.clone(),identity})
 }
}
impl<B:RingBackend> Drop for RingClient<B>{fn drop(&mut self){self.core.retire();}}
impl<B:RingBackend> Pool<B>{
 /// # Safety
 /// Record must be the authenticated paired CQ result of a READ for this pool.
 pub(crate) unsafe fn accept(&self,result:i32,flags:u32,receipt:BufferReceipt)->Result<Option<Payload<B>>,Error>{
  if self.core.closing.get(){return Err(Error::Closing);}
  let mut s=self.core.state.borrow_mut();let p=s.pools[self.identity.slot as usize].as_mut().ok_or(Error::Stale)?;
  if p.identity!=self.identity{return Err(Error::Stale);}if p.closing{return Err(Error::Closing);}
  let token=p.ledger.accept_read(result,flags,receipt).map_err(Error::Ledger)?;
  if let Some(token)=token{p.live+=1;s.live+=1;Ok(Some(Payload{core:self.core.clone(),token:Some(token)}))}else{Ok(None)}
 }
}
impl<B:RingBackend> Drop for Pool<B>{fn drop(&mut self){let mut s=self.core.state.borrow_mut();let p=s.pools[self.identity.slot as usize].as_mut().expect("pool handle keeps slot");assert_eq!(p.identity,self.identity);p.closing=true;self.core.retire();}}
impl<B:RingBackend> Payload<B>{
 pub(crate) fn bytes(&self)->&[u8]{let token=self.token.as_ref().unwrap();let r=token.receipt();let range=self.core.state.borrow().pools[r.pool.slot as usize].as_ref().unwrap().ranges[r.member as usize];unsafe{core::slice::from_raw_parts(self.core.backend.buffer(range.buffer_index as usize).as_ptr().add(range.offset as usize),token.length())}}
 pub(crate) fn try_return(mut self)->Result<(),(i64,Self)>{let token=self.token.take().unwrap();let slot=token.receipt().pool.slot as usize;match self.core.give_back(token){Ok(())=>{self.core.finish_live(slot);Ok(())},Err((rc,token))=>{self.token=Some(token);Err((rc,self))}}}
}
impl<B:RingBackend> Drop for Payload<B>{fn drop(&mut self){if let Some(token)=self.token.take(){self.core.abandon(token);}}}
