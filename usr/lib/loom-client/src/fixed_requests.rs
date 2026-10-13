//! The ring's request driver: one book for fixed, control and pooled requests.
//! No SQ publishing or Future wakeup.
extern crate alloc;
use super::{
    paired_cq::{Completion, Record},
    request_book::{Delivery, Error as BookError, Kind, RequestBook},
    ring_payload::{Error as StorageError, FixedIo, Pool, RingBackend, RingClient, RingOwner},
    service_abi::{PoolMember, SlotRef},
};
use alloc::rc::Rc;
use core::cell::RefCell;
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Error {
    Book(BookError),
    Storage(StorageError),
    Closing,
    Consumed,
}
struct Core<B: RingBackend> {
    owner: RingOwner<B>,
    client: Option<RingClient<B>>,
    book: RequestBook,
    failed: bool,
}
impl<B: RingBackend> Core<B> {
    fn retire(&mut self) {
        self.client.take();
        self.owner.begin_retire();
    }
    fn fail(&mut self) {
        self.failed = true;
        self.retire();
        self.owner.close();
    }
}
pub(crate) struct FixedDriver<B: RingBackend> {
    core: Rc<RefCell<Core<B>>>,
}
struct Request<B: RingBackend> {
    core: Rc<RefCell<Core<B>>>,
    fixed: Option<FixedIo<B>>,
    cookie: u64,
    published: bool,
}
pub(crate) struct ReadRequest<B: RingBackend> {
    inner: Request<B>,
}
pub(crate) struct WriteRequest<B: RingBackend> {
    inner: Request<B>,
}
pub(crate) struct ReadCompletion<B: RingBackend> {
    fixed: FixedIo<B>,
    completion: Completion,
}
/// A control, slot or pooled request: book metadata only, no fixed buffer.
pub(crate) struct BookRequest<B: RingBackend> {
    core: Rc<RefCell<Core<B>>>,
    cookie: u64,
    published: bool,
    taken: bool,
}
/// What an accepted completion was. Fixed and control terminals stay in the book
/// for their owners; a pooled shot goes to the pool its request names.
#[derive(Debug)]
pub(crate) enum Routed {
    Fixed(u64),
    Control(u64),
    Pooled(Delivery),
}
impl<B: RingBackend> FixedDriver<B> {
    pub(crate) fn new(backend: B, cq_capacity: usize) -> Result<Self, Error> {
        // Allocate request metadata before creating the live wrapper. On bad capacity,
        // close the consumed backend explicitly (its pins still obey backend contract).
        let book = match RequestBook::new(cq_capacity) {
            Ok(b) => b,
            Err(e) => {
                backend.begin_retire();
                backend.close();
                return Err(Error::Book(e));
            }
        };
        let (owner, client) = RingOwner::new(backend).map_err(Error::Storage)?;
        Ok(Self {
            core: Rc::new(RefCell::new(Core {
                owner,
                client: Some(client),
                book,
                failed: false,
            })),
        })
    }
    fn prepare(
        &self,
        scope: SlotRef,
        range: PoolMember,
        input: Option<&[u8]>,
    ) -> Result<Request<B>, Error> {
        let mut core = self.core.borrow_mut();
        if core.failed || core.client.is_none() {
            return Err(Error::Closing);
        }
        // Match storage limits before the narrowing cast or request reservation.
        if range.length == 0 || range.length > i32::MAX as u64 {
            return Err(Error::Storage(StorageError::Geometry));
        }
        let kind = if input.is_some() {
            Kind::FixedWrite(range.length as u32)
        } else {
            Kind::FixedRead(range.length as u32)
        };
        let cookie = core.book.reserve(scope, kind).map_err(Error::Book)?;
        let storage =
            core.client
                .as_ref()
                .unwrap()
                .reserve_fixed(cookie, scope, range, input.is_some());
        let mut fixed = match storage {
            Ok(f) => f,
            Err(e) => {
                core.book.cancel_unpublished(cookie).expect("fresh request");
                return Err(Error::Storage(e));
            }
        };
        if let Some(bytes) = input {
            if let Err(e) = fixed.write_from(bytes) {
                core.book.cancel_unpublished(cookie).expect("fresh request");
                return Err(Error::Storage(e));
            }
        }
        Ok(Request {
            core: self.core.clone(),
            fixed: Some(fixed),
            cookie,
            published: false,
        })
    }
    pub(crate) fn read(&self, scope: SlotRef, range: PoolMember) -> Result<ReadRequest<B>, Error> {
        Ok(ReadRequest {
            inner: self.prepare(scope, range, None)?,
        })
    }
    pub(crate) fn write(
        &self,
        scope: SlotRef,
        range: PoolMember,
        bytes: &[u8],
    ) -> Result<WriteRequest<B>, Error> {
        Ok(WriteRequest {
            inner: self.prepare(scope, range, Some(bytes))?,
        })
    }
    /// Control, slot and pooled kinds only; fixed requests own a buffer and use
    /// read or write.
    pub(crate) fn request(&self, scope: SlotRef, kind: Kind) -> Result<BookRequest<B>, Error> {
        let mut core = self.core.borrow_mut();
        if core.failed || core.client.is_none() {
            return Err(Error::Closing);
        }
        if matches!(kind, Kind::FixedRead(_) | Kind::FixedWrite(_)) {
            return Err(Error::Book(BookError::Geometry));
        }
        let cookie = core.book.reserve(scope, kind).map_err(Error::Book)?;
        Ok(BookRequest {
            core: self.core.clone(),
            cookie,
            published: false,
            taken: false,
        })
    }
    pub(crate) fn register_pool(&self, ranges: &[PoolMember]) -> Result<Pool<B>, Error> {
        let core = self.core.borrow();
        if core.failed {
            return Err(Error::Closing);
        }
        let client = core.client.as_ref().ok_or(Error::Closing)?;
        client.register_pool(ranges).map_err(Error::Storage)
    }
    /// # Safety
    /// Paired completion from this driver's exclusive native mapping, after local
    /// fixed-buffer access has ended. It is never synthesized from ENTER/ABORT rc.
    pub(crate) unsafe fn receive(&mut self, record: Record) -> Result<(), Error> {
        unsafe { self.route(record) }.map(|_| ())
    }
    /// # Safety
    /// As receive. A pooled delivery's receipt is not yet owned by anyone: the
    /// caller hands it to the pool's accept before any other ring work.
    pub(crate) unsafe fn route(&mut self, record: Record) -> Result<Routed, Error> {
        let mut c = self.core.borrow_mut();
        if c.failed {
            return Err(Error::Closing);
        }
        let delivery = match c.book.accept(record) {
            Ok(d) => d,
            Err(e) => {
                c.fail();
                return Err(Error::Book(e));
            }
        };
        let cookie = delivery.cookie;
        match delivery.kind {
            Kind::FixedRead(_) | Kind::FixedWrite(_) => {
                if let Err(e) = unsafe { c.owner.finish_fixed(cookie, delivery.record.cqe.result) }
                {
                    c.fail();
                    return Err(Error::Storage(e));
                }
                if delivery.abandoned {
                    c.book.take_terminal(cookie).expect("fixed terminal");
                }
                Ok(Routed::Fixed(cookie))
            }
            Kind::Control | Kind::Slot(_) => {
                if delivery.abandoned {
                    c.book.take_terminal(cookie).expect("control terminal");
                }
                Ok(Routed::Control(cookie))
            }
            Kind::PooledRead { .. } => {
                if delivery.terminal && delivery.abandoned {
                    c.book.take_terminal(cookie).expect("pooled terminal");
                }
                Ok(Routed::Pooled(delivery))
            }
        }
    }
    /// Scheduling/cancellation bookkeeping only; no synthetic completion or refund.
    pub(crate) fn next_deadline(&self) -> Option<u64> {
        self.core.borrow().owner.next_deadline()
    }
    pub(crate) fn expire(&mut self, now: u64, budget: usize) -> usize {
        self.core.borrow_mut().owner.expire(now, budget)
    }
    pub(crate) fn bind_progress(&mut self, signal: super::ring_payload::ProgressSignal) {
        self.core.borrow_mut().owner.bind_progress(signal);
    }
    pub(crate) fn close(&mut self) {
        self.core.borrow_mut().fail();
    }
    pub(crate) fn begin_retire(&mut self) {
        self.core.borrow_mut().retire();
    }
    /// No completion drain or allocation here; bounded preallocated owner scans.
    /// Once true, all fixed views/requests are gone and whole-owner local access
    /// is retired, so abandoned completion metadata can finally be discarded.
    pub(crate) fn progress(&mut self) -> bool {
        let mut c = self.core.borrow_mut();
        if c.owner.progress() {
            unsafe {
                c.book.clear_retired();
            }
            true
        } else {
            false
        }
    }
    pub(crate) fn occupied(&self) -> usize {
        self.core.borrow().book.occupied()
    }
}
impl<B: RingBackend> Drop for FixedDriver<B> {
    fn drop(&mut self) {
        self.core.borrow_mut().retire();
    }
}
impl<B: RingBackend> Request<B> {
    /// # Safety
    /// Prepare the SQE with returned cookie, activate immediately before release
    /// SQ tail, with no fallible work after success. Native publication is parked.
    unsafe fn publish(&mut self) -> Result<u64, Error> {
        let mut c = self.core.borrow_mut();
        if c.failed || c.client.is_none() {
            return Err(Error::Closing);
        }
        let f = self.fixed.as_mut().ok_or(Error::Consumed)?;
        if self.published {
            return Err(Error::Book(BookError::State));
        }
        unsafe {
            f.publish().map_err(Error::Storage)?;
        }
        // Same creator, private cookie and no callback between these transitions.
        // Fresh prepared book entry makes this transition infallible by construction.
        c.book.publish(self.cookie).expect("owned prepared request");
        self.published = true;
        Ok(self.cookie)
    }
    fn take(&mut self) -> Result<Option<(Completion, FixedIo<B>)>, Error> {
        if self.fixed.is_none() {
            return Err(Error::Consumed);
        }
        let mut c = self.core.borrow_mut();
        if c.failed {
            return Err(Error::Closing);
        }
        if !self.published {
            return if c.client.is_none() {
                Err(Error::Closing)
            } else {
                Ok(None)
            };
        }
        match c.book.take_terminal(self.cookie) {
            Ok(completion) => Ok(Some((completion, self.fixed.take().unwrap()))),
            Err(BookError::State) => {
                if c.client.is_none() {
                    Err(Error::Closing)
                } else {
                    Ok(None)
                }
            }
            Err(e) => Err(Error::Book(e)),
        }
    }
}
impl<B: RingBackend> Drop for Request<B> {
    fn drop(&mut self) {
        if self.fixed.is_none() {
            return;
        }
        {
            let mut c = self.core.borrow_mut();
            if !self.published {
                c.book
                    .cancel_unpublished(self.cookie)
                    .expect("unpublished owner");
            } else {
                match c.book.take_terminal(self.cookie) {
                    Ok(_) => {}
                    Err(BookError::State) => {
                        c.book.abandon(self.cookie).expect("active owner");
                    }
                    Err(_) => {
                        c.fail();
                    }
                }
            }
        }
        // FixedIo drop performs the single scope abort for active abandonment. Book
        // abandonment supplies metadata only; never issue the same abort twice here.
        self.fixed.take();
    }
}
impl<B: RingBackend> ReadRequest<B> {
    pub(crate) fn set_deadline(&mut self, deadline: Option<u64>) -> Result<(), Error> {
        self.inner
            .fixed
            .as_mut()
            .ok_or(Error::Consumed)?
            .set_deadline(deadline)
            .map_err(Error::Storage)
    }
    /// Cancellation is scope-wide, including siblings; success means requested,
    /// never remote rollback, terminal delivery or permission to reuse storage.
    pub(crate) fn request_cancel(&mut self) -> Result<bool, Error> {
        self.inner
            .fixed
            .as_mut()
            .ok_or(Error::Consumed)?
            .request_cancel()
            .map_err(Error::Storage)
    }

    pub(crate) unsafe fn publish(&mut self) -> Result<u64, Error> {
        unsafe { self.inner.publish() }
    }
    pub(crate) fn try_complete(&mut self) -> Result<Option<ReadCompletion<B>>, Error> {
        Ok(self
            .inner
            .take()?
            .map(|(completion, fixed)| ReadCompletion { fixed, completion }))
    }
}
impl<B: RingBackend> WriteRequest<B> {
    pub(crate) fn set_deadline(&mut self, deadline: Option<u64>) -> Result<(), Error> {
        self.inner
            .fixed
            .as_mut()
            .ok_or(Error::Consumed)?
            .set_deadline(deadline)
            .map_err(Error::Storage)
    }
    /// Cancellation is scope-wide, including siblings; success means requested,
    /// never remote rollback, terminal delivery or permission to reuse storage.
    pub(crate) fn request_cancel(&mut self) -> Result<bool, Error> {
        self.inner
            .fixed
            .as_mut()
            .ok_or(Error::Consumed)?
            .request_cancel()
            .map_err(Error::Storage)
    }

    pub(crate) unsafe fn publish(&mut self) -> Result<u64, Error> {
        unsafe { self.inner.publish() }
    }
    pub(crate) fn try_complete(&mut self) -> Result<Option<Completion>, Error> {
        Ok(self.inner.take()?.map(|(completion, _fixed)| completion))
    }
}
impl<B: RingBackend> BookRequest<B> {
    pub(crate) fn cookie(&self) -> u64 {
        self.cookie
    }
    /// # Safety
    /// As a fixed request's publish: the SQE carries this cookie, and the SQ tail
    /// is released next with no fallible work between.
    pub(crate) unsafe fn publish(&mut self) -> Result<u64, Error> {
        let mut c = self.core.borrow_mut();
        if c.failed || c.client.is_none() {
            return Err(Error::Closing);
        }
        if self.published || self.taken {
            return Err(Error::Book(BookError::State));
        }
        c.book.publish(self.cookie).expect("owned prepared request");
        self.published = true;
        Ok(self.cookie)
    }
    pub(crate) fn take(&mut self) -> Result<Option<Completion>, Error> {
        if self.taken {
            return Err(Error::Consumed);
        }
        let mut c = self.core.borrow_mut();
        if c.failed {
            return Err(Error::Closing);
        }
        if !self.published {
            return if c.client.is_none() {
                Err(Error::Closing)
            } else {
                Ok(None)
            };
        }
        match c.book.take_terminal(self.cookie) {
            Ok(completion) => {
                self.taken = true;
                Ok(Some(completion))
            }
            Err(BookError::State) => {
                if c.client.is_none() {
                    Err(Error::Closing)
                } else {
                    Ok(None)
                }
            }
            Err(e) => Err(Error::Book(e)),
        }
    }
}
impl<B: RingBackend> Drop for BookRequest<B> {
    fn drop(&mut self) {
        if self.taken {
            return;
        }
        let mut c = self.core.borrow_mut();
        if !self.published {
            c.book
                .cancel_unpublished(self.cookie)
                .expect("unpublished owner");
            return;
        }
        match c.book.take_terminal(self.cookie) {
            Ok(_) => {}
            Err(BookError::State) => {
                // Only the first abandonment of a live request asks for the abort.
                if let Some(scope) = c.book.abandon(self.cookie).expect("active owner") {
                    c.owner.abort_scope(scope);
                }
            }
            Err(_) => c.fail(),
        }
    }
}
impl<B: RingBackend> ReadCompletion<B> {
    pub(crate) fn result(&self) -> i32 {
        self.completion.result
    }
    pub(crate) fn bytes(&self) -> Result<&[u8], StorageError> {
        self.fixed.bytes()
    }
}
