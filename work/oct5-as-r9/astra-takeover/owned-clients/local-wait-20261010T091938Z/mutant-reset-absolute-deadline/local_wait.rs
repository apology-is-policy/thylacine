//! Uninstalled creator-thread wait coordinator; no native event adapter.
use super::retirement_executor::{Retirable,RetireQueue,Progress};
#[derive(Clone,Copy,Debug,PartialEq,Eq)]pub(crate) enum WakeReason{Event,Changed,Spurious,Deadline}
#[derive(Debug,PartialEq,Eq)]pub(crate) enum Outcome{Retry,Deadline,Woke(WakeReason),Failed(i32)}
pub(crate) struct Turn{pub progress:Progress,pub outcome:Outcome}
/// Event source contract: arm subscribes before readiness is sampled. wait must
/// atomically compare the armed ticket with current event state before parking,
/// retain wakeups until observed, and honor the SAME absolute monotonic deadline.
/// now and ready are nonblocking. wait must not invoke creator-thread callbacks
/// between our last local predicate check and parking. All return paths, including
/// errors and spurious wakes, release the subscription. A native implementation
/// needs separate qualification; deterministic host sources only are used here.
pub(crate) trait WaitSource{
 type Ticket:Copy;
 fn now(&mut self)->u64;
 fn arm(&mut self)->Result<Self::Ticket,i32>;
 fn ready(&mut self)->bool;
 fn wait(&mut self,ticket:Self::Ticket,deadline:Option<u64>)->Result<WakeReason,i32>;
 fn disarm(&mut self,ticket:Self::Ticket);
}
struct Armed<'a,W:WaitSource>{source:&'a mut W,ticket:W::Ticket}
impl<W:WaitSource> Drop for Armed<'_,W>{fn drop(&mut self){self.source.disarm(self.ticket);}}
fn expired(now:u64,deadline:Option<u64>)->bool{deadline.is_some_and(|d|now>=d)}
/// One bounded retirement turn, at most one wait, never an internal retry loop.
/// deadline is absolute; None means unbounded. Deadline reports scheduling
/// expiry only: it does not fabricate a terminal CQ or release active storage.
/// Caller services ready tasks/CQs and performs policy cancellation separately.
pub(crate) fn turn<D:Retirable,W:WaitSource>(queue:&mut RetireQueue<D>,source:&mut W,budget:usize,deadline:Option<u64>)->Turn{
 if expired(source.now(),deadline){return Turn{progress:Progress{visited:0,reclaimed:0,occupied:queue.occupied()},outcome:Outcome::Deadline};}
 let progress=queue.progress_notified(budget);
 if queue.needs_progress(){return Turn{progress,outcome:Outcome::Retry};}
 let ticket=match source.arm(){Ok(t)=>t,Err(e)=>return Turn{progress,outcome:Outcome::Failed(e)}};
 let armed=Armed{source,ticket};
 // Recheck external predicates first: callbacks in these nonblocking methods
 // may release local owners. The local pulse check must therefore be last.
 let ready=armed.source.ready();let elapsed=expired(armed.source.now(),deadline);
 let outcome=if elapsed{Outcome::Deadline}
 else if ready{queue.notify_progress();Outcome::Retry}
 else if queue.needs_progress(){Outcome::Retry}
 else{match armed.source.wait(armed.ticket,deadline.map(|d|d+1)){Ok(reason)=>{queue.notify_progress();Outcome::Woke(reason)},Err(e)=>Outcome::Failed(e)}};
 drop(armed);
 Turn{progress,outcome}
}
