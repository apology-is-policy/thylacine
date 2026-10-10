//! Uninstalled owned-lease core. Backend/syscall and full-ring integration pending.
extern crate alloc;
use alloc::rc::Rc;
use core::{cell::{Cell,RefCell},ptr::NonNull};
use super::{lease_ledger::{LeaseLedger,LeaseToken,LeaseError},service_abi::{SlotRef,PoolMember,BufferReceipt}};
/// # Safety
/// Own all eager registered backing exclusively; stable pointers must cover the
/// lengths reported by buffer_len, with distinct indices naming disjoint owned
/// allocations. Only the kernel writes, and never to an outstanding leased
/// extent. RETURN may permit immediate reuse only on zero success; every nonzero
/// return keeps leased bytes unchanged. Retirement
/// stops new admission without changing leased bytes. `retired` proves all local
/// kernel writers ended. `close` detaches this consumer nonblockingly and preserves
/// independent kernel pins/payer charges until physical retirement, even if this
/// backend is then dropped. It never changes already leased bytes. All methods
/// must be non-reentrant, non-panicking and bounded; no user callbacks.
/// This internal boundary is NOT a safe constructor from arbitrary pointers.
/// The final ring adapter must share one retirement owner for all its pools and
/// enforce the ring-wide64-member envelope and cross-pool exclusion.
pub(crate) unsafe trait PayloadBackend {
    fn buffer_count(&self)->usize;
    fn buffer_len(&self,index:usize)->usize;
    fn buffer(&self,index:usize)->NonNull<u8>;
    fn return_buffer(&self,receipt:BufferReceipt)->i64;
    fn begin_retire(&self);
    fn retired(&self)->bool;
    fn close(&self);
}
struct State {
    ledger:LeaseLedger,
    pending:[Option<LeaseToken>;64],
    live:usize,
}
struct Core<B:PayloadBackend> {
    backend:B,
    ranges:[PoolMember;64],
    state:RefCell<State>,
    closing:Cell<bool>,
    closed:Cell<bool>,
}
impl<B:PayloadBackend> Core<B> {
    fn retire(&self){if !self.closing.replace(true){self.backend.begin_retire();}}
    fn close(&self){self.retire();if !self.closed.replace(true){self.backend.close();}}
    fn return_token(&self,token:LeaseToken)->Result<(),(i64,LeaseToken)> {
        let mut s=self.state.borrow_mut();
        let receipt=s.ledger.begin_return(&token).expect("internally owned exact token");
        let rc=self.backend.return_buffer(receipt);
        s.ledger.finish_return(&token,rc).expect("returning token remains exact");
        if rc==0{Ok(())}else{Err((rc,token))}
    }
    fn abandon(&self,token:LeaseToken){
        match self.return_token(token){
            Ok(())=>{},
            Err((_,token))=>{let index=token.receipt().member as usize;let mut s=self.state.borrow_mut();
                // One live/returning token per member: this cell is pre-reserved.
                assert!(s.pending[index].is_none());drop(token);}
        }
        self.state.borrow_mut().live-=1;
    }
}
impl<B:PayloadBackend> Drop for Core<B>{fn drop(&mut self){self.close();}}
/// Executor-held ownership record. Constructed before admission, never allocated
/// by lease/future/client drop. Actual controller integration remains pending.
pub(crate) struct RetirementOwner<B:PayloadBackend>{core:Rc<Core<B>>}
pub(crate) struct PayloadPool<B:PayloadBackend>{core:Rc<Core<B>>}
pub(crate) struct PayloadLease<B:PayloadBackend>{core:Rc<Core<B>>,token:Option<LeaseToken>}
#[derive(Debug,PartialEq,Eq)]
pub(crate) enum PayloadError{Closing,Ledger(LeaseError)}
impl<B:PayloadBackend> RetirementOwner<B>{
    /// Backend ownership is consumed, so safe callers cannot recover mutable
    /// storage. The unsafe backend implementation establishes pointer validity.
    pub(crate) fn new(backend:B,pool:SlotRef,ranges:&[PoolMember])->Result<(Self,PayloadPool<B>),LeaseError>{
        let validated=(||{
            let count=backend.buffer_count();if count==0 || count>64{return Err(LeaseError::Geometry);}
            let mut lengths=[0u64;64];
            for (i,length) in lengths[..count].iter_mut().enumerate(){
                let n=backend.buffer_len(i);if n==0 || n>isize::MAX as usize{return Err(LeaseError::Geometry);}
                *length=n as u64;
            }
            LeaseLedger::new(pool,ranges,&lengths[..count])
        })();
        let ledger=match validated{Ok(ledger)=>ledger,Err(e)=>{backend.begin_retire();backend.close();return Err(e);}};
        let mut owned=[PoolMember{buffer_index:0,reserved:0,offset:0,length:0};64];
        owned[..ranges.len()].copy_from_slice(ranges);
        let core=Rc::new(Core{backend,ranges:owned,state:RefCell::new(State{ledger,pending:core::array::from_fn(|_|None),live:0}),closing:Cell::new(false),closed:Cell::new(false)});
        Ok((Self{core:core.clone()},PayloadPool{core}))
    }
    /// At most64 RETURN attempts. EOF/CQ consumption never calls this implicitly.
    /// True permits controller removal only after client closure, no user leases
    /// and confirmed local retirement. Failed returns stay charged until then.
    pub(crate) fn progress(&mut self)->bool{
        for i in 0..64{
            let token=self.core.state.borrow_mut().pending[i].take();
            if let Some(token)=token{
                if let Err((_,token))=self.core.return_token(token){self.core.state.borrow_mut().pending[i]=Some(token);}
            }
        }
        self.core.closing.get() && self.core.state.borrow().live==0 && self.core.backend.retired()
    }
    pub(crate) fn outstanding(&self)->usize{self.core.state.borrow().ledger.outstanding()}
    pub(crate) fn pending_returns(&self)->usize{self.core.state.borrow().pending.iter().filter(|p|p.is_some()).count()}
}
impl<B:PayloadBackend> Drop for RetirementOwner<B>{
    // Controller destruction is whole-owner teardown, not normal pool drop.
    // Leases still hold backing via Rc; kernel keeps independent physical pins.
    fn drop(&mut self){self.core.close();}
}
impl<B:PayloadBackend> PayloadPool<B>{
    /// Called only with a copied paired CQ record for this pool's READ. No raw
    /// pointer, mutable storage view or legacy reap escape exists on this type.
    /// The final adapter must authenticate request-to-pool correlation first.
    pub(crate) unsafe fn accept_read(&self,result:i32,flags:u32,receipt:BufferReceipt)->Result<Option<PayloadLease<B>>,PayloadError>{
        if self.core.closing.get(){return Err(PayloadError::Closing);}
        let mut s=self.core.state.borrow_mut();
        let token=s.ledger.accept_read(result,flags,receipt).map_err(PayloadError::Ledger)?;
        Ok(token.map(|token|{s.live+=1;PayloadLease{core:self.core.clone(),token:Some(token)}}))
    }
}
impl<B:PayloadBackend> Drop for PayloadPool<B>{fn drop(&mut self){self.core.retire();}}
impl<B:PayloadBackend> PayloadLease<B>{
    pub(crate) fn bytes(&self)->&[u8]{
        let token=self.token.as_ref().expect("live lease");let range=self.core.ranges[token.receipt().member as usize];
        // Geometry and receipt length checked before transfer. Kernel cannot
        // reuse this exact member until self is consumed/dropped; Rust prevents
        // either while the returned borrow is subsequently used.
        unsafe{core::slice::from_raw_parts(self.core.backend.buffer(range.buffer_index as usize).as_ptr().add(range.offset as usize),token.length())}
    }
    /// Error returns the still-owning lease and exact syscall result. No errno
    /// collapse or inferred success (including NOENT/positive return).
    pub(crate) fn try_return(mut self)->Result<(),(i64,Self)>{
        let token=self.token.take().expect("live lease");
        match self.core.return_token(token){
            Ok(())=>{self.core.state.borrow_mut().live-=1;Ok(())},
            Err((rc,token))=>{self.token=Some(token);Err((rc,self))}
        }
    }
}
impl<B:PayloadBackend> Drop for PayloadLease<B>{fn drop(&mut self){if let Some(token)=self.token.take(){self.core.abandon(token);}}}
