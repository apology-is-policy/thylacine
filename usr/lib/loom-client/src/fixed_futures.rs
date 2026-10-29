//! Owned fixed-I/O Futures over the simulated/adapter-only typed controller.
//! This is not a SQ publisher or a sleeping event loop. Native integration gated.
extern crate alloc;
use super::{
    fixed_requests::{Error, FixedDriver, ReadCompletion, ReadRequest, WriteRequest},
    paired_cq::{Completion, Record},
    ring_payload::RingBackend,
    service_abi::{PoolMember, SlotRef},
};
use alloc::rc::Rc;
use core::{
    cell::RefCell,
    future::Future,
    pin::Pin,
    task::{Context, Poll, Waker},
};
struct Registration {
    waker: Option<Waker>,
}
struct Hub {
    closed: bool,
    slots: [Option<Registration>; 64],
}
impl Hub {
    fn reserve(&mut self) -> Result<usize, Error> {
        if self.closed {
            return Err(Error::Closing);
        }
        let i = self
            .slots
            .iter()
            .position(Option::is_none)
            .ok_or(Error::Storage(super::ring_payload::Error::Full))?;
        self.slots[i] = Some(Registration { waker: None });
        Ok(i)
    }
    // Moving Wakers out never calls user code. Invoke/drop them only after every
    // RefCell borrow has ended; custom Waker vtables may reenter or allocate.
    fn batch(&mut self) -> [Option<Waker>; 64] {
        core::array::from_fn(|i| self.slots[i].as_mut().and_then(|r| r.waker.take()))
    }
}
fn release(hub: &Rc<RefCell<Hub>>, slot: &mut Option<usize>) {
    if let Some(i) = slot.take() {
        let old = { hub.borrow_mut().slots[i].take() };
        drop(old);
    }
}
/// Invoke only after releasing every enclosing manager borrow. Dropping an
/// undispatched batch can also invoke user Waker-drop code.
#[must_use]
pub(crate) struct WakeBatch([Option<Waker>; 64]);
impl WakeBatch {
    pub(crate) fn empty() -> Self {
        Self(core::array::from_fn(|_| None))
    }
    pub(crate) fn dispatch(self) {
        for w in self.0.into_iter().flatten() {
            w.wake();
        }
    }
}
fn finish<T>(
    hub: &Rc<RefCell<Hub>>,
    slot: &mut Option<usize>,
    status: Result<Option<T>, Error>,
    waker: Waker,
) -> Poll<Result<T, Error>> {
    match status {
        Ok(Some(value)) => {
            release(hub, slot);
            Poll::Ready(Ok(value))
        }
        Err(e) => {
            release(hub, slot);
            Poll::Ready(Err(e))
        }
        Ok(None) => {
            // Clone happened before checking completion. If clone reentered receive,
            // the subsequent completion check observes it. Replacement/drop happens
            // after registration and outside the borrow, so a new event can wake us.
            if hub.borrow().closed {
                release(hub, slot);
                return Poll::Ready(Err(Error::Closing));
            }
            let old = {
                let mut h = hub.borrow_mut();
                h.slots[slot.expect("pending future owns registration")]
                    .as_mut()
                    .unwrap()
                    .waker
                    .replace(waker)
            };
            drop(old);
            Poll::Pending
        }
    }
}
pub(crate) struct FutureDriver<B: RingBackend> {
    driver: Option<FixedDriver<B>>,
    hub: Rc<RefCell<Hub>>,
}
pub(crate) struct ReadFuture<B: RingBackend> {
    request: ReadRequest<B>,
    hub: Rc<RefCell<Hub>>,
    slot: Option<usize>,
}
pub(crate) struct WriteFuture<B: RingBackend> {
    request: WriteRequest<B>,
    hub: Rc<RefCell<Hub>>,
    slot: Option<usize>,
}
impl<B: RingBackend> FutureDriver<B> {
    pub(crate) fn new(driver: FixedDriver<B>) -> Self {
        Self {
            driver: Some(driver),
            hub: Rc::new(RefCell::new(Hub {
                closed: false,
                slots: core::array::from_fn(|_| None),
            })),
        }
    }
    pub(crate) fn read(&self, scope: SlotRef, range: PoolMember) -> Result<ReadFuture<B>, Error> {
        let mut slot = Some(self.hub.borrow_mut().reserve()?);
        match self.driver.as_ref().unwrap().read(scope, range) {
            Ok(request) => Ok(ReadFuture {
                request,
                hub: self.hub.clone(),
                slot,
            }),
            Err(e) => {
                release(&self.hub, &mut slot);
                Err(e)
            }
        }
    }
    pub(crate) fn write(
        &self,
        scope: SlotRef,
        range: PoolMember,
        bytes: &[u8],
    ) -> Result<WriteFuture<B>, Error> {
        let mut slot = Some(self.hub.borrow_mut().reserve()?);
        match self.driver.as_ref().unwrap().write(scope, range, bytes) {
            Ok(request) => Ok(WriteFuture {
                request,
                hub: self.hub.clone(),
                slot,
            }),
            Err(e) => {
                release(&self.hub, &mut slot);
                Err(e)
            }
        }
    }
    /// Same exact paired-CQ and ended local access contract as FixedDriver.receive.
    pub(crate) unsafe fn receive(&mut self, record: Record) -> Result<(), Error> {
        let (result, batch) = unsafe { self.receive_deferred(record) };
        batch.dispatch();
        result
    }
    /// Adapter for enclosing retirement owners: never invoke user Wakers here.
    pub(crate) unsafe fn receive_deferred(
        &mut self,
        record: Record,
    ) -> (Result<(), Error>, WakeBatch) {
        let result = unsafe { self.driver.as_mut().unwrap().receive(record) };
        let batch = if result.is_err() {
            self.close_deferred()
        } else {
            WakeBatch(self.hub.borrow_mut().batch())
        };
        (result, batch)
    }
    pub(crate) fn close(&mut self) {
        let batch = self.close_deferred();
        batch.dispatch();
    }
    pub(crate) fn close_deferred(&mut self) -> WakeBatch {
        self.driver.as_mut().unwrap().close();
        let mut h = self.hub.borrow_mut();
        h.closed = true;
        WakeBatch(h.batch())
    }
    pub(crate) fn retire_deferred(&mut self) -> WakeBatch {
        self.driver.as_mut().unwrap().begin_retire();
        let mut h = self.hub.borrow_mut();
        h.closed = true;
        WakeBatch(h.batch())
    }
    pub(crate) fn next_deadline(&self) -> Option<u64> {
        self.driver.as_ref().unwrap().next_deadline()
    }
    pub(crate) fn expire(&mut self, now: u64, budget: usize) -> usize {
        self.driver.as_mut().unwrap().expire(now, budget)
    }
    pub(crate) fn bind_progress(&mut self, signal: super::ring_payload::ProgressSignal) {
        self.driver.as_mut().unwrap().bind_progress(signal);
    }
    pub(crate) fn progress(&mut self) -> bool {
        self.driver.as_mut().unwrap().progress()
    }
    pub(crate) fn registrations(&self) -> usize {
        self.hub
            .borrow()
            .slots
            .iter()
            .filter(|s| s.is_some())
            .count()
    }
}
impl<B: RingBackend> Drop for FutureDriver<B> {
    fn drop(&mut self) {
        self.close();
    }
}
impl<B: RingBackend> ReadFuture<B> {
    pub(crate) fn set_deadline(&mut self, deadline: Option<u64>) -> Result<(), Error> {
        self.request.set_deadline(deadline)
    }
    pub(crate) fn request_cancel(&mut self) -> Result<bool, Error> {
        self.request.request_cancel()
    }
    pub(crate) unsafe fn publish(&mut self) -> Result<u64, Error> {
        unsafe { self.request.publish() }
    }
}
impl<B: RingBackend> WriteFuture<B> {
    pub(crate) fn set_deadline(&mut self, deadline: Option<u64>) -> Result<(), Error> {
        self.request.set_deadline(deadline)
    }
    pub(crate) fn request_cancel(&mut self) -> Result<bool, Error> {
        self.request.request_cancel()
    }
    pub(crate) unsafe fn publish(&mut self) -> Result<u64, Error> {
        unsafe { self.request.publish() }
    }
}
impl<B: RingBackend> Future for ReadFuture<B> {
    type Output = Result<ReadCompletion<B>, Error>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        if me.slot.is_none() {
            return Poll::Ready(Err(Error::Consumed));
        }
        let waker = cx.waker().clone();
        let status = me.request.try_complete();
        finish(&me.hub, &mut me.slot, status, waker)
    }
}
impl<B: RingBackend> Future for WriteFuture<B> {
    type Output = Result<Completion, Error>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        if me.slot.is_none() {
            return Poll::Ready(Err(Error::Consumed));
        }
        let waker = cx.waker().clone();
        let status = me.request.try_complete();
        finish(&me.hub, &mut me.slot, status, waker)
    }
}
impl<B: RingBackend> Drop for ReadFuture<B> {
    fn drop(&mut self) {
        release(&self.hub, &mut self.slot);
    }
}
impl<B: RingBackend> Drop for WriteFuture<B> {
    fn drop(&mut self) {
        release(&self.hub, &mut self.slot);
    }
}
