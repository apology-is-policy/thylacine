//! One cooperative local task/timer/retirement turn; simulated wait adapters only.
use super::{local_tasks::{Tasks,Dispatch},local_wait::{self,WaitSource,TimedTurn,Turn,Outcome},retirement_executor::{RetireQueue,Retirable,Progress}};
#[derive(Clone,Copy)]pub(crate) struct Budgets{pub tasks:usize,pub retirement:usize,pub rings:usize,pub fixed:usize}
pub(crate) struct LoopTurn{pub dispatch:Dispatch,pub wait:TimedTurn}
/// Poll at most one rotating task quantum, then service bounded timers and
/// retirement even if a task keeps self-waking. Runnable work prevents waiting
/// both before arm and after all nonblocking event-source callbacks. The caller
/// still delivers actual CQ events and grants positive budgets for fair progress.
/// No cross-thread/native wake-to-park protocol is supplied by this composition.
pub(crate) fn turn<D:Retirable,W:WaitSource>(tasks:&mut Tasks,queue:&mut RetireQueue<D>,source:&mut W,b:Budgets,outer_deadline:Option<u64>)->LoopTurn{
 if outer_deadline.is_some_and(|d|source.now()>=d){return LoopTurn{dispatch:Dispatch{visited:0,polled:0,completed:0},wait:TimedTurn{turn:Turn{progress:Progress{visited:0,reclaimed:0,occupied:queue.occupied()},outcome:Outcome::Deadline},cancellation_requests:0}};}
 let dispatch=tasks.dispatch(b.tasks);
 let wait=local_wait::timed_turn_ready(queue,source,b.retirement,b.rings,b.fixed,outer_deadline,||tasks.ready());
 LoopTurn{dispatch,wait}
}
