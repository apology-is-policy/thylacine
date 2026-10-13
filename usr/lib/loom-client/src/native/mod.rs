//! The native adapter: drives one private service ring from EL0 over the client
//! cores (docs/ASYNC-SERVICE-ABI.md, "Native adapter"). Creator thread only.
extern crate alloc;
mod handles;
mod kernel;

pub use crate::owned_mapping::Setup;
pub use crate::service_abi::PoolMember;
pub use handles::{
    Connect, Fid, Open, Payload, Pool, Read, ReadDone, Scope, Shot, Stream, Target, Walk, Write,
};
pub use kernel::{Kernel, POLLERR, POLLHUP, POLLIN, POLLNVAL};

use crate::{
    fixed_requests::{Error as DriverError, FixedDriver, Routed},
    local_wait::{WaitSource, WakeReason},
    owned_mapping::OwnedMapping,
    paired_cq::Record,
    request_book::Error as BookError,
    ring_payload::{Error as StorageError, Payload as CorePayload, Pool as CorePool},
    ring_records::{
        BufferRange, CQ_HEAD, CQ_TAIL, DROPPED, ENTER_NONBLOCK, MAX_REG_BUFFERS, REGISTER_BUFFERS,
        SQ_HEAD, SQ_TAIL, Submission,
    },
    service_abi::{SETUP_PRIVATE_SERVICE, SETUP_SERVICE_BUFFERS},
};
use alloc::{boxed::Box, collections::VecDeque, rc::Rc, sync::Arc, task::Wake, vec::Vec};
use core::{
    cell::{Cell, RefCell},
    future::Future,
    pin::pin,
    ptr::NonNull,
    sync::atomic::{AtomicBool, AtomicU32, Ordering},
    task::{Context, Poll, Waker},
};
use kernel::{Backend, EBADF, ECANCELED, EINTR, EINVAL, EIO, ENOSPC, ETIMEDOUT, Map, PAGE, Shared};

/// A negative errno.
pub type Result<T> = core::result::Result<T, i32>;

/// Bytes the kernel reads one WALK component from; one cell per SQ slot.
const NAME_CELL: usize = 256;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum State {
    Open,
    Retiring,
    Closed,
    Failed,
}

/// The SQ's only producer. Its pointers were validated with the mapping.
struct Producer {
    hdr: *mut u8,
    array: *mut u32,
    sqes: *mut Submission,
    entries: u32,
    names: *mut u8,
    name_buffer: u32,
    dropped: u32,
}
impl Producer {
    fn word(&self, offset: usize) -> &AtomicU32 {
        unsafe { &*self.hdr.add(offset).cast::<AtomicU32>() }
    }
    fn pending(&self) -> u32 {
        let tail = self.word(SQ_TAIL).load(Ordering::Relaxed);
        tail.wrapping_sub(self.word(SQ_HEAD).load(Ordering::Acquire))
    }
    fn space(&self) -> bool {
        self.pending() < self.entries
    }
    /// The slot the next SQE takes, and with it the name cell a WALK uses.
    fn next_slot(&self) -> u32 {
        self.word(SQ_TAIL).load(Ordering::Relaxed) & (self.entries - 1)
    }
    /// # Safety
    /// space() held, and nothing fallible remains for this submission: the
    /// release store below publishes it to the kernel.
    unsafe fn push(&self, sqe: &Submission) {
        let tail = self.word(SQ_TAIL).load(Ordering::Relaxed);
        let slot = tail & (self.entries - 1);
        unsafe {
            core::ptr::write(self.sqes.add(slot as usize), *sqe);
            core::ptr::write(self.array.add(slot as usize), slot);
        }
        self.word(SQ_TAIL)
            .store(tail.wrapping_add(1), Ordering::Release);
    }
    fn dropped_changed(&self) -> bool {
        self.word(DROPPED).load(Ordering::Acquire) != self.dropped
    }
    fn cq_ready(&self) -> bool {
        self.word(CQ_TAIL).load(Ordering::Acquire) != self.word(CQ_HEAD).load(Ordering::Relaxed)
    }
}

pub(crate) struct Route<K: Kernel> {
    cookie: u64,
    pool: Rc<CorePool<Backend<K>>>,
    stream: Rc<RefCell<StreamState<K>>>,
}
pub(crate) struct StreamState<K: Kernel> {
    payloads: VecDeque<CorePayload<Backend<K>>>,
    open: bool,
}

pub(crate) struct Inner<K: Kernel> {
    shared: Rc<Shared<K>>,
    cq: RefCell<OwnedMapping<Map<K>>>,
    sq: Producer,
    driver: RefCell<FixedDriver<Backend<K>>>,
    wakers: RefCell<Box<[Option<(u64, Waker)>]>>,
    routes: RefCell<Box<[Option<Route<K>>]>>,
    clunks: RefCell<[Option<crate::fixed_requests::BookRequest<Backend<K>>>; 64]>,
    state: Cell<State>,
    retired: Cell<bool>,
    cq_entries: usize,
}

/// One private service ring and everything built on it. Dropping it closes the
/// ring: every scope is aborted and every pending operation ends -ECANCELED.
/// Only the creating thread may use it, and a forked child must not: fork does
/// not carry the ring's mapping (NA-1).
pub struct Ring<K: Kernel> {
    inner: Rc<Inner<K>>,
}

fn round_page(len: usize) -> Option<usize> {
    len.checked_add(PAGE - 1).map(|n| n & !(PAGE - 1))
}

impl<K: Kernel> Ring<K> {
    /// A ring of `entries` SQEs (a power of two up to 4096) whose fixed and pooled
    /// I/O use `buffers`: one registered region per length, each rounded up to a
    /// page. At most 63 buffers; the adapter registers one more for WALK names.
    pub fn setup(kernel: K, entries: u32, buffers: &[usize]) -> Result<Ring<K>> {
        if entries == 0 || entries > 4096 || !entries.is_power_of_two() {
            return Err(-EINVAL);
        }
        if buffers.is_empty() || buffers.len() >= MAX_REG_BUFFERS as usize {
            return Err(-EINVAL);
        }
        let mut lengths = Vec::with_capacity(buffers.len() + 1);
        for &len in buffers {
            match round_page(len) {
                Some(n) if len > 0 && n <= isize::MAX as usize => lengths.push(n),
                _ => return Err(-EINVAL),
            }
        }
        lengths.push(round_page(entries as usize * NAME_CELL).unwrap());
        let shared = Rc::new(Shared::attach(kernel, &lengths, buffers.len())?);
        let mut params = Setup {
            flags: SETUP_PRIVATE_SERVICE | SETUP_SERVICE_BUFFERS,
            sq_entries: 0,
            cq_entries: 0,
            ring_size: 0,
            ring_va: 0,
            hdr_off: 0,
            sq_array_off: 0,
            sqe_off: 0,
            cqe_off: 0,
            sq_array_size: 0,
            sqe_size: 0,
            cqe_size: 0,
            reserved: 0,
            receipt: [0; 4],
        };
        let fd = shared.kernel.setup(entries, &mut params);
        if fd < 0 {
            return Err(fd as i32);
        }
        if fd > i32::MAX as i64 {
            return Err(-EIO);
        }
        shared.set_fd(fd as i32);
        let Some(base) = NonNull::new(params.ring_va as usize as *mut u8) else {
            shared.close_fd();
            return Err(-EIO);
        };
        let map = Map {
            shared: shared.clone(),
            base,
            len: params.ring_size as usize,
        };
        // Validates every region, the header and the receipts; closes on refusal.
        let cq = OwnedMapping::new(map, params).map_err(|_| -EIO)?;
        shared.set_ring(
            base.as_ptr() as usize,
            params.ring_size as usize,
            base.as_ptr() as usize + params.hdr_off as usize,
        );
        if params.sq_entries != entries {
            shared.close_fd();
            return Err(-EIO);
        }
        let mut table = [BufferRange::default(); MAX_REG_BUFFERS as usize];
        for (i, &(va, len)) in shared.regions().iter().enumerate() {
            table[i] = BufferRange {
                va: va as u64,
                len: len as u64,
            };
        }
        let n = shared.regions().len() as u32;
        let rc = unsafe {
            shared
                .kernel
                .register(shared.fd(), REGISTER_BUFFERS, table.as_mut_ptr().cast(), n)
        };
        if rc < 0 {
            shared.close_fd();
            return Err(rc as i32);
        }
        let cq_entries = params.cq_entries as usize;
        let driver = FixedDriver::new(Backend(shared.clone()), cq_entries).map_err(errno)?;
        let ring = base.as_ptr();
        let names = shared.region(buffers.len()).0 as *mut u8;
        let sq = Producer {
            hdr: unsafe { ring.add(params.hdr_off as usize) },
            array: unsafe { ring.add(params.sq_array_off as usize).cast() },
            sqes: unsafe { ring.add(params.sqe_off as usize).cast() },
            entries,
            names,
            name_buffer: buffers.len() as u32,
            dropped: 0,
        };
        Ok(Ring {
            inner: Rc::new(Inner {
                shared,
                cq: RefCell::new(cq),
                sq,
                driver: RefCell::new(driver),
                wakers: RefCell::new((0..cq_entries).map(|_| None).collect()),
                routes: RefCell::new((0..cq_entries).map(|_| None).collect()),
                clunks: RefCell::new(core::array::from_fn(|_| None)),
                state: Cell::new(State::Open),
                retired: Cell::new(false),
                cq_entries,
            }),
        })
    }
    /// The kernel's monotonic clock, in the nanoseconds every deadline uses.
    pub fn now(&self) -> u64 {
        self.inner.shared.kernel.now()
    }
    /// Name a strict 9P service posted in the registry `registry_fd` (from
    /// open("/srv", O_PATH)). Synchronous: the kernel checks the name now and
    /// the service when a scope connects.
    pub fn target(&self, registry_fd: i32, name: &[u8]) -> Result<Target<K>> {
        handles::target(&self.inner, registry_fd, name)
    }
    /// Register `ranges` of the ring's buffers as a pool for pooled reads.
    pub fn pool(&self, ranges: &[PoolMember]) -> Result<Pool<K>> {
        handles::pool(&self.inner, ranges)
    }
    /// One bounded step: submit what the SQ holds, route every completion the
    /// CQ holds and retry deferred cleanup. If none of that found work, wait
    /// once for a completion or until `deadline` (absolute, monotonic ns).
    /// Returns the completions routed.
    pub fn turn(&self, deadline: Option<u64>) -> Result<usize> {
        let routed = self.inner.service()?;
        if routed > 0 || self.inner.retired.get() {
            return Ok(routed);
        }
        self.inner.wait(deadline)?;
        self.inner.service()
    }
    /// Run `future` to completion, turning the ring while it waits. Past
    /// `deadline` the future is dropped, which cancels what it started (its
    /// whole scope), and the result is -ETIMEDOUT.
    pub fn block_on<F: Future>(&self, future: F, deadline: Option<u64>) -> Result<F::Output> {
        struct Flag(AtomicBool);
        impl Wake for Flag {
            fn wake(self: Arc<Self>) {
                self.0.store(true, Ordering::Release);
            }
            fn wake_by_ref(self: &Arc<Self>) {
                self.0.store(true, Ordering::Release);
            }
        }
        let flag = Arc::new(Flag(AtomicBool::new(true)));
        let waker = Waker::from(flag.clone());
        let mut cx = Context::from_waker(&waker);
        let mut future = pin!(future);
        loop {
            if flag.0.swap(false, Ordering::AcqRel) {
                if let Poll::Ready(v) = future.as_mut().poll(&mut cx) {
                    return Ok(v);
                }
                continue;
            }
            if deadline.is_some_and(|d| self.now() >= d) {
                return Err(-ETIMEDOUT);
            }
            if let Err(e) = self.turn(deadline) {
                if flag.0.swap(false, Ordering::AcqRel) {
                    if let Poll::Ready(v) = future.as_mut().poll(&mut cx) {
                        return Ok(v);
                    }
                }
                return Err(e);
            }
        }
    }
    /// Stop admission and abort every scope; turns then drive the ring until
    /// retired() reports that the kernel retired every scope, every terminal
    /// has been routed, and no fixed or pooled buffer is still lent out. A
    /// future still held keeps its terminal. A scope's retirement is not a poll
    /// event, so a caller waiting for retired() passes turn a deadline.
    pub fn retire(&self) {
        self.inner.begin_retire();
    }
    pub fn retired(&self) -> bool {
        self.inner.retired.get()
    }
    /// Close the ring now. Payload leases keep their bytes readable; the
    /// mapping and buffers are detached when the last of them is dropped.
    pub fn close(&self) {
        self.inner.close();
    }
}
impl<K: Kernel> Drop for Ring<K> {
    fn drop(&mut self) {
        self.inner.close();
    }
}

pub(crate) fn errno(e: DriverError) -> i32 {
    match e {
        DriverError::Closing | DriverError::Storage(StorageError::Closing) => -ECANCELED,
        DriverError::Book(BookError::Full) | DriverError::Storage(StorageError::Full) => -ENOSPC,
        DriverError::Book(BookError::Exhausted) => -ENOSPC,
        DriverError::Book(BookError::Geometry)
        | DriverError::Storage(StorageError::Geometry)
        | DriverError::Storage(StorageError::Ledger(_)) => -EINVAL,
        DriverError::Storage(StorageError::Busy) => -kernel::EBUSY,
        DriverError::Storage(StorageError::Backend(rc) | StorageError::Uncertain(rc)) => {
            if rc < 0 && rc >= i32::MIN as i64 {
                rc as i32
            } else {
                -EIO
            }
        }
        DriverError::Consumed => -EINVAL,
        DriverError::Book(_) | DriverError::Storage(StorageError::Stale) => -EIO,
    }
}

/// The ring's WaitSource: poll(2) on the ring descriptor. The kernel registers
/// the waiter before it samples the CQ (loom_poll), which is the atomic compare
/// the trait requires of wait.
struct RingWait<'a, K: Kernel> {
    inner: &'a Inner<K>,
}
impl<K: Kernel> WaitSource for RingWait<'_, K> {
    type Ticket = u32;
    fn now(&mut self) -> u64 {
        self.inner.shared.kernel.now()
    }
    fn arm(&mut self) -> core::result::Result<u32, i32> {
        Ok(self.inner.sq.word(CQ_TAIL).load(Ordering::Acquire))
    }
    /// Queued SQEs are not readiness: the kernel stops consuming only for a full
    /// CQ, which service drains, or a pending rejection, which arrives as a CQE.
    fn ready(&mut self) -> bool {
        self.inner.sq.cq_ready()
    }
    fn wait(
        &mut self,
        ticket: u32,
        deadline: Option<u64>,
    ) -> core::result::Result<WakeReason, i32> {
        if self.inner.sq.word(CQ_TAIL).load(Ordering::Acquire) != ticket {
            return Ok(WakeReason::Event);
        }
        let fd = self.inner.shared.fd();
        if fd < 0 {
            return Err(-ECANCELED);
        }
        // Rounded up: poll never returns before the deadline it was given.
        let timeout = match deadline {
            None => -1,
            Some(d) => {
                let left = d.saturating_sub(self.inner.shared.kernel.now());
                left.div_ceil(1_000_000).min(i32::MAX as u64) as i32
            }
        };
        let rc = self.inner.shared.kernel.poll(fd, POLLIN, timeout);
        if rc < 0 {
            return if rc == -(EINTR as i64) {
                Ok(WakeReason::Spurious)
            } else {
                Err(rc as i32)
            };
        }
        if rc == 0 {
            return Ok(WakeReason::Deadline);
        }
        let events = rc as i16;
        if events & (POLLHUP | POLLNVAL) != 0 {
            // The ring was closed under us; nothing more will arrive.
            self.inner.lost();
            return Ok(WakeReason::Event);
        }
        if events & POLLERR != 0 {
            return Err(-EBADF);
        }
        Ok(WakeReason::Event)
    }
    fn disarm(&mut self, _ticket: u32) {}
}

impl<K: Kernel> Inner<K> {
    fn check(&self) -> Result<()> {
        match self.state.get() {
            State::Open | State::Retiring => Ok(()),
            State::Closed => Err(-ECANCELED),
            State::Failed => Err(-EIO),
        }
    }
    pub(crate) fn admit(&self) -> Result<()> {
        match self.state.get() {
            State::Open => Ok(()),
            State::Retiring | State::Closed => Err(-ECANCELED),
            State::Failed => Err(-EIO),
        }
    }
    /// A request's error as its owner sees it: a failed ring is -EIO.
    pub(crate) fn fault(&self, e: DriverError) -> i32 {
        if self.state.get() == State::Failed {
            -EIO
        } else {
            errno(e)
        }
    }
    pub(crate) fn shared(&self) -> &Shared<K> {
        &self.shared
    }
    pub(crate) fn driver(&self) -> core::cell::Ref<'_, FixedDriver<Backend<K>>> {
        self.driver.borrow()
    }
    /// Publish one SQE. `publish` activates its request; nothing fallible may
    /// follow it, so the space check comes first and the push cannot fail.
    pub(crate) fn submit(
        &self,
        mut sqe: Submission,
        name: &[u8],
        publish: impl FnOnce() -> core::result::Result<u64, DriverError>,
    ) -> Result<u64> {
        self.admit()?;
        if !self.sq.space() {
            return Err(-ENOSPC);
        }
        let cookie = publish().map_err(errno)?;
        sqe.user_data = cookie;
        if !name.is_empty() {
            // The kernel copies the component while the ENTER that consumes this
            // slot runs (loom_private_io_prepare), and this thread is the only
            // one that enters: the cell is free again before the slot is.
            let slot = self.sq.next_slot() as usize;
            unsafe {
                core::ptr::copy_nonoverlapping(
                    name.as_ptr(),
                    self.sq.names.add(slot * NAME_CELL),
                    name.len(),
                );
            }
            sqe.buf_idx_or_off = self.sq.name_buffer;
            sqe.args[0] = (slot * NAME_CELL) as u64;
            sqe.len = name.len() as u32;
        }
        unsafe { self.sq.push(&sqe) };
        self.enter();
        Ok(cookie)
    }
    /// ENTER(NONBLOCK, min_complete 0) for every queued SQE. A short count leaves
    /// the rest queued for a later turn; none is ever discarded.
    fn enter(&self) {
        let fd = self.shared.fd();
        if fd < 0 {
            return;
        }
        let rc = self
            .shared
            .kernel
            .enter(fd, self.sq.pending(), 0, ENTER_NONBLOCK);
        if self.sq.dropped_changed() {
            panic!("loom-client: the kernel dropped an SQE the adapter wrote in range");
        }
        if rc == -(ECANCELED as i64) || rc == -(EBADF as i64) {
            self.lost();
        }
    }
    fn wait(&self, deadline: Option<u64>) -> Result<()> {
        self.check()?;
        let mut source = RingWait { inner: self };
        if deadline.is_some_and(|d| source.now() >= d) {
            return Ok(());
        }
        let ticket = source.arm()?;
        if source.ready() || deadline.is_some_and(|d| source.now() >= d) {
            source.disarm(ticket);
            return Ok(());
        }
        let woke = source.wait(ticket, deadline);
        source.disarm(ticket);
        woke.map(|_| ())
    }
    /// Submit, route, then the local work no poll event announces: RETURN and
    /// pool REAP retries, background CLUNKs and slot REAPs.
    fn service(&self) -> Result<usize> {
        self.check()?;
        if self.sq.pending() != 0 {
            self.enter();
        }
        let mut routed = 0;
        while routed < self.cq_entries {
            let record = self.cq.borrow_mut().reap();
            match record {
                Ok(Some(record)) => {
                    self.deliver(record)?;
                    routed += 1;
                }
                Ok(None) => break,
                Err(_) => {
                    self.fail();
                    return Err(-EIO);
                }
            }
        }
        if routed > 0 {
            // Freed CQ space lets the kernel post what it deferred; ENTER is the kick.
            self.enter();
        }
        let retired = self.driver.borrow_mut().progress();
        self.retired.set(retired);
        self.clunk_pass();
        self.shared.reap_pass();
        self.check()?;
        Ok(routed)
    }
    fn deliver(&self, record: Record) -> Result<()> {
        let routed = unsafe { self.driver.borrow_mut().route(record) };
        let cookie = match routed {
            Err(_) => {
                self.fail();
                return Err(-EIO);
            }
            Ok(Routed::Fixed(cookie)) => cookie,
            Ok(Routed::Control(cookie)) => {
                self.finish_clunk(cookie);
                cookie
            }
            Ok(Routed::Pooled(d)) => {
                let route = {
                    let routes = self.routes.borrow();
                    routes
                        .iter()
                        .flatten()
                        .find(|r| r.cookie == d.cookie)
                        .map(|r| (r.pool.clone(), r.stream.clone()))
                };
                let Some((pool, stream)) = route else {
                    self.fail();
                    return Err(-EIO);
                };
                let cqe = d.record.cqe;
                match unsafe { pool.accept(cqe.result, cqe.flags, d.record.receipt) } {
                    Ok(Some(payload)) => {
                        let rejected = {
                            let mut s = stream.borrow_mut();
                            if s.open {
                                s.payloads.push_back(payload);
                                None
                            } else {
                                Some(payload)
                            }
                        };
                        drop(rejected); // An abandoned stream's lease goes straight back.
                    }
                    Ok(None) | Err(StorageError::Closing) => {}
                    Err(_) => {
                        self.fail();
                        return Err(-EIO);
                    }
                }
                if d.terminal {
                    let gone = {
                        let mut routes = self.routes.borrow_mut();
                        routes
                            .iter_mut()
                            .find(|r| r.as_ref().is_some_and(|r| r.cookie == d.cookie))
                            .and_then(Option::take)
                    };
                    drop(gone);
                }
                d.cookie
            }
        };
        self.wake(cookie);
        Ok(())
    }
    pub(crate) fn route(&self, route: Route<K>) {
        let mut routes = self.routes.borrow_mut();
        let cell = routes
            .iter_mut()
            .find(|r| r.is_none())
            .expect("one route per book entry");
        *cell = Some(route);
    }
    pub(crate) fn register(&self, cookie: u64, waker: &Waker) {
        let old = {
            let mut w = self.wakers.borrow_mut();
            let at = w
                .iter()
                .position(|e| e.as_ref().is_some_and(|(c, _)| *c == cookie))
                .or_else(|| w.iter().position(Option::is_none))
                .expect("one waker per book entry");
            w[at].replace((cookie, waker.clone()))
        };
        drop(old);
    }
    pub(crate) fn forget(&self, cookie: u64) {
        let old = {
            let mut w = self.wakers.borrow_mut();
            w.iter_mut()
                .find(|e| e.as_ref().is_some_and(|(c, _)| *c == cookie))
                .and_then(Option::take)
        };
        drop(old);
    }
    fn wake(&self, cookie: u64) {
        let found = {
            let mut w = self.wakers.borrow_mut();
            w.iter_mut()
                .find(|e| e.as_ref().is_some_and(|(c, _)| *c == cookie))
                .and_then(Option::take)
        };
        if let Some((_, waker)) = found {
            waker.wake();
        }
    }
    fn wake_all(&self) {
        loop {
            let next = {
                let mut w = self.wakers.borrow_mut();
                w.iter_mut().find_map(Option::take)
            };
            match next {
                Some((_, waker)) => waker.wake(),
                None => break,
            }
        }
    }
    /// Background CLUNKs for fids whose handles are gone. A fid of an aborted
    /// scope, or of a retiring ring, retires with its scope and is only reaped.
    fn clunk_pass(&self) {
        for i in 0..64 {
            let due = self.shared.slots.borrow()[i].filter(|e| e.state == kernel::SlotState::Clunk);
            let Some(e) = due else { continue };
            let scope_live = self
                .shared
                .entry(e.scope)
                .is_some_and(|s| !s.aborted && s.state == kernel::SlotState::Live);
            if !scope_live || self.admit().is_err() {
                self.shared.set_state(e.r, kernel::SlotState::Reap);
                continue;
            }
            if !self.sq.space() || self.check().is_err() {
                return;
            }
            let request = self
                .driver
                .borrow()
                .request(e.scope, crate::request_book::Kind::Control);
            let Ok(mut request) = request else { return };
            let sqe = handles::clunk_sqe(e.r);
            let submitted = self.submit(sqe, &[], || unsafe { request.publish() });
            // A ring that closed inside the submit already shut every CLUNK down.
            if submitted.is_ok() && self.check().is_ok() {
                self.shared.set_state(e.r, kernel::SlotState::Clunking);
                self.clunks.borrow_mut()[i] = Some(request);
            }
        }
    }
    fn finish_clunk(&self, cookie: u64) {
        let done = {
            let mut clunks = self.clunks.borrow_mut();
            let at = clunks
                .iter()
                .position(|c| c.as_ref().is_some_and(|c| c.cookie() == cookie));
            at.map(|i| (i, clunks[i].take().unwrap()))
        };
        if let Some((i, mut request)) = done {
            let _ = request.take();
            drop(request);
            let entry = self.shared.slots.borrow()[i];
            if let Some(e) = entry {
                self.shared.set_state(e.r, kernel::SlotState::Reap);
            }
        }
    }
    fn begin_retire(&self) {
        if self.state.get() != State::Open {
            return;
        }
        self.state.set(State::Retiring);
        self.driver.borrow_mut().begin_retire();
        self.wake_all();
    }
    fn fail(&self) {
        self.state.set(State::Failed);
        self.shutdown();
    }
    /// The kernel closed the ring under the adapter: forget its descriptor.
    fn lost(&self) {
        self.shared.forget_fd();
        self.close();
    }
    fn close(&self) {
        if matches!(self.state.get(), State::Open | State::Retiring) {
            self.state.set(State::Closed);
        }
        self.shutdown();
    }
    fn shutdown(&self) {
        self.driver.borrow_mut().close();
        let clunks: [Option<_>; 64] = core::mem::replace(
            &mut *self.clunks.borrow_mut(),
            core::array::from_fn(|_| None),
        );
        drop(clunks);
        self.wake_all();
    }
}
