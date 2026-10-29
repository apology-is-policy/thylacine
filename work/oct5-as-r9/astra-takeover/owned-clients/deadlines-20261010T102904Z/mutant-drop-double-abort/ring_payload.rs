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
/// Backing is initialized and writable. Provided and fixed users must obey this
/// owner's exclusive reservations; fixed terminal CQ ends all local access.
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
 /// Scope-wide cancellation; bounded, idempotent, no storage reuse implied.
 fn abort_scope(&self,scope:SlotRef);
 fn begin_retire(&self);
 fn retired(&self)->bool;
 fn close(&self);
}
struct PoolState {
 identity:SlotRef,ledger:LeaseLedger,ranges:Box<[PoolMember]>,
 pending:Box<[Option<LeaseToken>]>,live:usize,closing:bool,
}
#[derive(Clone,Copy,PartialEq,Eq)]
enum FixedPhase{Prepared,Active,Terminal(i32)}
#[derive(Clone,Copy)]
struct FixedState{cookie:u64,scope:SlotRef,range:PoolMember,write:bool,phase:FixedPhase,abandoned:bool,deadline:Option<u64>,abort_requested:bool}
fn overlap(a:PoolMember,b:PoolMember)->bool{a.buffer_index==b.buffer_index&&a.offset<b.offset+b.length&&b.offset<a.offset+a.length}
struct State {fixed:[Option<FixedState>;64],quarantine:Option<Box<PoolState>>,pools:[Option<Box<PoolState>>;64],last:[u64;64],members:usize,live:usize}
/// Creator-thread coalesced progress pulse. No callback, allocation, syscall or
/// cross-thread wake; safe to notify while an enclosing owner is borrowed.
/// The local event loop must inspect it before sleeping. Native readiness and
/// timer events must explicitly notify the queue; this is not their adapter.
#[derive(Clone)]pub(crate) struct ProgressSignal(Rc<Cell<bool>>);
impl ProgressSignal{
 pub(crate) fn new()->Self{Self(Rc::new(Cell::new(false)))}
 pub(crate) fn notify(&self){self.0.set(true);}
 pub(crate) fn pending(&self)->bool{self.0.get()}
 pub(crate) fn take(&self)->bool{self.0.replace(false)}
}
struct Core<B:RingBackend>{backend:B,state:RefCell<State>,closing:Cell<bool>,closed:Cell<bool>,signal:RefCell<Option<ProgressSignal>>,deadline_cursor:Cell<usize>}
impl<B:RingBackend> Core<B>{
 fn notify(&self){if let Some(s)=self.signal.borrow().as_ref(){s.notify();}}
 fn retire(&self){if !self.closing.replace(true){self.backend.begin_retire();self.notify();}}
 fn close(&self){self.retire();if !self.closed.replace(true){self.backend.close();}}
 /// Request scope-wide abort once per fixed owner. It never changes phase,
 /// removes storage, or fabricates a terminal result. Backend callbacks obey
 /// RingBackend's nonblocking/nonreentrant contract.
 fn cancel_fixed(&self,index:usize,cookie:u64)->Result<bool,Error>{
  if self.closed.get(){return Ok(false);}
  let scope={let mut s=self.state.borrow_mut();let f=s.fixed[index].as_mut().ok_or(Error::Stale)?;
   if f.cookie!=cookie{return Err(Error::Stale);}
   if f.phase==FixedPhase::Prepared{return Err(Error::Busy);}
   if f.phase!=FixedPhase::Active||f.abort_requested{return Ok(false);}
   f.abort_requested=true;f.scope};
  self.backend.abort_scope(scope);self.notify();Ok(true)
 }
 fn give_back(&self,token:LeaseToken)->Result<(),(i64,LeaseToken)>{
  let mut s=self.state.borrow_mut();let p=s.pools[token.receipt().pool.slot as usize].as_mut().expect("live pool");
  let receipt=p.ledger.begin_return(&token).expect("owned exact token");let rc=self.backend.return_buffer(receipt);
  p.ledger.finish_return(&token,rc).expect("same returning token");if rc==0{Ok(())}else{Err((rc,token))}
 }
 fn finish_live(&self,slot:usize){let mut s=self.state.borrow_mut();s.pools[slot].as_mut().unwrap().live-=1;s.live-=1;drop(s);self.notify();}
 fn abandon(&self,token:LeaseToken){let slot=token.receipt().pool.slot as usize;
  if let Err((_,token))=self.give_back(token){let i=token.receipt().member as usize;let mut s=self.state.borrow_mut();let p=s.pools[slot].as_mut().unwrap();assert!(p.pending[i].is_none());p.pending[i]=Some(token);}
  self.finish_live(slot);
 }
}
impl<B:RingBackend> Drop for Core<B>{fn drop(&mut self){self.close();}}
pub(crate) struct RingOwner<B:RingBackend>{core:Rc<Core<B>>}
pub(crate) struct RingClient<B:RingBackend>{core:Rc<Core<B>>}
/// Exclusive fixed-I/O range; no view while submitted. Shares the whole-ring owner.
pub(crate) struct FixedIo<B:RingBackend>{core:Rc<Core<B>>,index:usize,cookie:u64}
pub(crate) struct Pool<B:RingBackend>{core:Rc<Core<B>>,identity:SlotRef}
pub(crate) struct Payload<B:RingBackend>{core:Rc<Core<B>>,token:Option<LeaseToken>}
#[derive(Debug,PartialEq,Eq)]
pub(crate) enum Error{Closing,Geometry,Busy,Stale,Backend(i64),Uncertain(i64),Ledger(LeaseError)}
impl<B:RingBackend> RingOwner<B>{
 pub(crate) fn new(backend:B)->Result<(Self,RingClient<B>),Error>{
  let count=backend.buffer_count();let valid=count>0&&count<=64&&(0..count).all(|i|{let n=backend.buffer_len(i);n>0&&n<=isize::MAX as usize});
  if !valid{backend.begin_retire();backend.close();return Err(Error::Geometry);}
  let core=Rc::new(Core{backend,state:RefCell::new(State{fixed:[None;64],quarantine:None,pools:core::array::from_fn(|_|None),last:[0;64],members:0,live:0}),closing:Cell::new(false),closed:Cell::new(false),signal:RefCell::new(None),deadline_cursor:Cell::new(0)});
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
  let retired=self.core.backend.retired();
  if retired{for slot in &mut self.core.state.borrow_mut().fixed{if slot.as_ref().is_some_and(|f|f.abandoned){*slot=None;}}}
  self.core.closing.get()&&self.core.state.borrow().live==0&&self.core.state.borrow().fixed.iter().all(Option::is_none)&&retired
 }
 /// # Safety
 /// Only the authenticated terminal CQ for this exact ring/request after the
 /// final local fixed-buffer access has ended. ENTER failure/ABORT is not proof.
 pub(crate) unsafe fn finish_fixed(&mut self,cookie:u64,result:i32)->Result<(),Error>{
  let mut s=self.core.state.borrow_mut();let slot=s.fixed.iter_mut().find(|f|f.as_ref().is_some_and(|f|f.cookie==cookie)).ok_or(Error::Stale)?;
  let f=slot.as_mut().unwrap();if f.phase!=FixedPhase::Active{return Err(Error::Busy);}
  if result>0&&result as u64>f.range.length{return Err(Error::Geometry);}
  if f.abandoned{*slot=None;}else{f.phase=FixedPhase::Terminal(result);}drop(s);self.core.notify();Ok(())
 }
 pub(crate) fn bind_progress(&mut self,signal:ProgressSignal){let mut s=self.core.signal.borrow_mut();assert!(s.is_none(),"one retirement owner");*s=Some(signal);}
 /// Absolute monotonic deadline of active, not-yet-cancelled local requests.
 /// A terminal result or cancellation request removes timer interest, NOT ownership.
 pub(crate) fn next_deadline(&self)->Option<u64>{
  if self.core.closed.get(){return None;}
  self.core.state.borrow().fixed.iter().flatten().filter(|f|f.phase==FixedPhase::Active&&!f.abort_requested).filter_map(|f|f.deadline).min()
 }
 /// Bounded rotating scan of the existing 64 fixed-owner cells, no timer allocation.
 /// Expiry requests scope-wide abort; only terminal delivery/retirement ends access.
 pub(crate) fn expire(&mut self,now:u64,budget:usize)->usize{
  if self.core.closed.get(){return 0;}
  let mut requested=0;
  for _ in 0..budget.min(64){let i=self.core.deadline_cursor.get();self.core.deadline_cursor.set((i+1)%64);
   let due={let s=self.core.state.borrow();s.fixed[i].filter(|f|f.phase==FixedPhase::Active&&!f.abort_requested&&f.deadline.is_some_and(|d|now>=d)).map(|f|f.cookie)};
   if let Some(cookie)=due{if self.core.cancel_fixed(i,cookie).expect("same creator owns fixed cell"){requested+=1;}}
  }requested
 }
 pub(crate) fn fixed_count(&self)->usize{self.core.state.borrow().fixed.iter().flatten().count()}
 pub(crate) fn begin_retire(&mut self){self.core.retire();}
 pub(crate) fn close(&mut self){self.core.close();}
 pub(crate) fn members(&self)->usize{self.core.state.borrow().members}
 pub(crate) fn live(&self)->usize{self.core.state.borrow().live}
 pub(crate) fn pending(&self)->usize{self.core.state.borrow().pools.iter().flatten().map(|p|p.pending.iter().filter(|x|x.is_some()).count()).sum()}
}
impl<B:RingBackend> Drop for RingOwner<B>{fn drop(&mut self){self.core.close();}}
impl<B:RingBackend> RingClient<B>{
 /// One preallocated fixed reservation per existing registered buffer (<=64).
 /// Cookie is allocated by this ring's RequestBook before any SQ publication.
 /// Adapter excludes all native accesses except this request; backing is writable
 /// and initialized, and fixed ranges cannot overlap provided-pool reservations.
 pub(crate) fn reserve_fixed(&self,cookie:u64,scope:SlotRef,range:PoolMember,write:bool)->Result<FixedIo<B>,Error>{
  if self.core.closing.get(){return Err(Error::Closing);}
  let i=range.buffer_index as usize;
  if cookie==0||scope.slot>=64||scope.reserved!=0||scope.incarnation==0||range.reserved!=0||range.length==0||range.length>i32::MAX as u64||i>=self.core.backend.buffer_count()||range.offset.checked_add(range.length).is_none_or(|end|end>self.core.backend.buffer_len(i)as u64){return Err(Error::Geometry);}
  let mut s=self.core.state.borrow_mut();
  if s.fixed[i].is_some()||s.fixed.iter().flatten().any(|f|f.cookie==cookie){return Err(Error::Busy);}
  for p in s.pools.iter().flatten(){if p.ranges.iter().any(|r|overlap(*r,range)){return Err(Error::Geometry);}}
  s.fixed[i]=Some(FixedState{cookie,scope,range,write,phase:FixedPhase::Prepared,abandoned:false,deadline:None,abort_requested:false});
  Ok(FixedIo{core:self.core.clone(),index:i,cookie})
 }
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
  for f in s.fixed.iter().flatten(){for r in ranges{if overlap(f.range,*r){return Err(Error::Geometry);}}}
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
impl<B:RingBackend> Drop for Pool<B>{fn drop(&mut self){let mut s=self.core.state.borrow_mut();let p=s.pools[self.identity.slot as usize].as_mut().expect("pool handle keeps slot");assert_eq!(p.identity,self.identity);p.closing=true;self.core.backend.retire_pool(self.identity);drop(s);self.core.notify();}}
impl<B:RingBackend> Payload<B>{
 pub(crate) fn bytes(&self)->&[u8]{let token=self.token.as_ref().unwrap();let r=token.receipt();let range=self.core.state.borrow().pools[r.pool.slot as usize].as_ref().unwrap().ranges[r.member as usize];unsafe{core::slice::from_raw_parts(self.core.backend.buffer(range.buffer_index as usize).as_ptr().add(range.offset as usize),token.length())}}
 pub(crate) fn try_return(mut self)->Result<(),(i64,Self)>{let token=self.token.take().unwrap();let slot=token.receipt().pool.slot as usize;match self.core.give_back(token){Ok(())=>{self.core.finish_live(slot);Ok(())},Err((rc,token))=>{self.token=Some(token);Err((rc,self))}}}
}
impl<B:RingBackend> Drop for Payload<B>{fn drop(&mut self){if let Some(token)=self.token.take(){self.core.abandon(token);}}}

impl<B:RingBackend> FixedIo<B>{
 fn snapshot(&self)->FixedState{let f=self.core.state.borrow().fixed[self.index].expect("live fixed owner");assert_eq!(f.cookie,self.cookie);f}
 /// Configure before publication; activity cannot extend an absolute deadline.
 /// None has no timer. This local Option is not the native deadline-zero encoding.
 pub(crate) fn set_deadline(&mut self,deadline:Option<u64>)->Result<(),Error>{
  if self.core.closing.get(){return Err(Error::Closing);}
  let mut s=self.core.state.borrow_mut();let f=s.fixed[self.index].as_mut().unwrap();assert_eq!(f.cookie,self.cookie);
  if f.phase!=FixedPhase::Prepared{return Err(Error::Busy);}f.deadline=deadline;Ok(())
 }
 pub(crate) fn request_cancel(&mut self)->Result<bool,Error>{self.core.cancel_fixed(self.index,self.cookie)}
 /// Copy input before submission; never expose a mutable backing alias.
 pub(crate) fn write_from(&mut self,bytes:&[u8])->Result<(),Error>{
  let f=self.snapshot();if self.core.closing.get(){return Err(Error::Closing);}
  if !f.write||f.phase!=FixedPhase::Prepared{return Err(Error::Busy);}
  if bytes.len()!=f.range.length as usize{return Err(Error::Geometry);}
  unsafe{core::ptr::copy_nonoverlapping(bytes.as_ptr(),self.core.backend.buffer(self.index).as_ptr().add(f.range.offset as usize),bytes.len());}Ok(())
 }
 /// # Safety
 /// Integrator activates the matching request record and releases the SQ tail
 /// with no fallible work after this transition. No raw/second buffer user.
 pub(crate) unsafe fn publish(&mut self)->Result<(),Error>{
  if self.core.closing.get(){return Err(Error::Closing);}
  let mut s=self.core.state.borrow_mut();let f=s.fixed[self.index].as_mut().unwrap();
  if f.phase!=FixedPhase::Prepared{return Err(Error::Busy);}f.phase=FixedPhase::Active;drop(s);self.core.notify();Ok(())
 }
 /// Only successful terminal READ bytes, bounded by the actual completion.
 pub(crate) fn bytes(&self)->Result<&[u8],Error>{let f=self.snapshot();
  if f.write{return Err(Error::Busy);}let FixedPhase::Terminal(n)=f.phase else{return Err(Error::Busy)};
  if n<0{return Err(Error::Backend(n as i64));}
  Ok(unsafe{core::slice::from_raw_parts(self.core.backend.buffer(self.index).as_ptr().add(f.range.offset as usize),n as usize)})
 }
}
impl<B:RingBackend> Drop for FixedIo<B>{fn drop(&mut self){
 let abort={let mut s=self.core.state.borrow_mut();let f=s.fixed[self.index].as_mut().unwrap();assert_eq!(f.cookie,self.cookie);
  if f.phase==FixedPhase::Active{if false{f.abandoned=true;None}else{f.abort_requested=true;f.abandoned=true;Some(f.scope)}}else{s.fixed[self.index]=None;None}};
 if let Some(scope)=abort{if !self.core.closed.get(){self.core.backend.abort_scope(scope);}}
 self.core.notify();
}}
