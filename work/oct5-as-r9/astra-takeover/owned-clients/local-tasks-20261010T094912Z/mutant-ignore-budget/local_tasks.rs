//! Uninstalled bounded creator-thread task dispatcher; no OS wake primitive.
extern crate alloc;
use alloc::{boxed::Box,rc::Rc,sync::Arc,task::Wake,vec::Vec};
use core::{future::Future,marker::PhantomData,pin::Pin,sync::atomic::{AtomicBool,AtomicU64,Ordering},task::{Context,Poll,Waker}};
static NEXT_TASK:AtomicU64=AtomicU64::new(1);
#[derive(Clone,Copy,Debug,PartialEq,Eq)]pub(crate) struct TaskId{slot:usize,id:u64}
#[derive(Debug,PartialEq,Eq)]pub(crate) enum Error{Capacity,Full,Exhausted}
#[derive(Debug,PartialEq,Eq)]pub(crate) struct Dispatch{pub visited:usize,pub polled:usize,pub completed:usize}
struct Ready{active:AtomicBool,ready:AtomicBool}
impl Wake for Ready{
 fn wake(self:Arc<Self>){self.wake_by_ref();}
 fn wake_by_ref(self:&Arc<Self>){if self.active.load(Ordering::Acquire){self.ready.store(true,Ordering::Release);}}
}
struct Entry{id:u64,ready:Arc<Ready>,future:Pin<Box<dyn Future<Output=()>+'static>>}
impl Drop for Entry{fn drop(&mut self){self.ready.active.store(false,Ordering::Release);}}
/// Capacity1..64 applies to live task owners. Future/Arc allocations occur only
/// at admission, after capacity check. Cloned external Wakers own their old flag;
/// a new task receives a distinct flag, so stale wakes cannot address reused cells.
/// Task bodies stay on the creator; only the atomic Waker flag is Send+Sync.
/// Wakes do not interrupt an OS wait: native event integration remains required.
pub(crate) struct Tasks{slots:Vec<Option<Entry>>,cursor:usize,_creator:PhantomData<Rc<()>>}
impl Tasks{
 pub(crate) fn new(capacity:usize)->Result<Self,Error>{
  if capacity==0||capacity>64{return Err(Error::Capacity);}let mut slots=Vec::with_capacity(capacity);slots.resize_with(capacity,||None);Ok(Self{slots,cursor:0,_creator:PhantomData})
 }
 /// Factory is never called when full. Callers must retain their input owners
 /// if a failed admission should retry; ownership moved into the closure drops
 /// normally on failure. Cooperative tasks must not block or reenter this queue.
 pub(crate) fn spawn<F:Future<Output=()>+'static>(&mut self,make:impl FnOnce()->F)->Result<TaskId,Error>{
  let slot=self.slots.iter().position(Option::is_none).ok_or(Error::Full)?;
  let id=NEXT_TASK.fetch_update(Ordering::Relaxed,Ordering::Relaxed,|n|n.checked_add(1)).map_err(|_|Error::Exhausted)?;
  let future=Box::pin(make());let ready=Arc::new(Ready{active:AtomicBool::new(true),ready:AtomicBool::new(true)});
  self.slots[slot]=Some(Entry{id,ready,future});Ok(TaskId{slot,id})
 }
 pub(crate) fn cancel(&mut self,task:TaskId)->bool{
  let Some(slot)=self.slots.get_mut(task.slot)else{return false;};
  if !slot.as_ref().is_some_and(|e|e.id==task.id){return false;}drop(slot.take());true
 }
 /// Visit each cell at most once per bounded round, so self-waking tasks cannot
 /// monopolize this call. Clear readiness BEFORE poll, preserving wake-during-poll.
 /// Detach the entry first: unwind drops its Future/owned requests and invalidates
 /// its Wakers. No internal retry or task callbacks under a shared RefCell borrow.
 pub(crate) fn dispatch(&mut self,budget:usize)->Dispatch{
  let visited=self.slots.len();let mut polled=0;let mut completed=0;
  for _ in 0..visited{let i=self.cursor;self.cursor=(i+1)%self.slots.len();
   if !self.slots[i].as_ref().is_some_and(|e|e.ready.ready.swap(false,Ordering::AcqRel)){continue;}
   let mut entry=self.slots[i].take().unwrap();let waker=Waker::from(entry.ready.clone());polled+=1;
   if let Poll::Ready(())=entry.future.as_mut().poll(&mut Context::from_waker(&waker)){completed+=1;}else{self.slots[i]=Some(entry);}
  }Dispatch{visited,polled,completed}
 }
 pub(crate) fn ready(&self)->bool{self.slots.iter().flatten().any(|e|e.ready.ready.load(Ordering::Acquire))}
 pub(crate) fn occupied(&self)->usize{self.slots.iter().flatten().count()}
}
