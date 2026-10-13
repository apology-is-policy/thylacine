//! What the adapter lends out: targets, scopes, fids, pools, streams, payloads,
//! and the futures of the operations that make or use them. Dropping a future
//! cancels its operation, and Loom cancellation ends the whole scope.
extern crate alloc;
use super::{
    Inner, Kernel, Result, Route, StreamState, errno,
    kernel::{Backend, EINVAL, EIO, ENOSPC, SlotEntry, SlotKind, SlotState, ZERO_REF},
};
use crate::{
    fixed_requests::{BookRequest, ReadCompletion, ReadRequest, WriteRequest},
    paired_cq::Completion,
    request_book::Kind,
    ring_payload::{Payload as CorePayload, Pool as CorePool},
    ring_records::{OP_CLUNK, OP_LOPEN, OP_READ, OP_WALK, OP_WRITE, SQE_MULTISHOT, Submission},
    service_abi::{
        OP_SERVICE_CONNECT, PoolMember, REGISTER_SERVICE_TARGET, SERVICE_FID, SERVICE_SCOPE,
        SQE_BUFFER_SELECT, SlotRef, Target as TargetRecord,
    },
};
use alloc::{collections::VecDeque, rc::Rc};
use core::{
    cell::RefCell,
    future::Future,
    pin::Pin,
    task::{Context, Poll},
};

/// Zero means "no deadline" to the kernel, so an absolute zero becomes one.
fn deadline_word(deadline: Option<u64>) -> u64 {
    deadline.map_or(0, |d| d.max(1))
}
fn sqe(opcode: u8, source: SlotRef, deadline: Option<u64>) -> Submission {
    Submission {
        opcode,
        handle_idx: source.slot,
        args: [0, source.incarnation, 0, deadline_word(deadline)],
        ..Submission::default()
    }
}
pub(crate) fn clunk_sqe(fid: SlotRef) -> Submission {
    sqe(OP_CLUNK, fid, None)
}
fn minted(r: SlotRef) -> bool {
    r.slot < 64 && r.reserved == 0 && r.incarnation != 0
}
/// A SCOPE or FID reservation. Exhaustion first retries the slots awaiting REAP.
fn reserve<K: Kernel>(inner: &Inner<K>, kind: u32, scope: SlotRef) -> Result<SlotRef> {
    match inner.shared().reserve(kind, scope) {
        Err(e) if e == -ENOSPC => {
            inner.shared().reap_pass();
            inner.shared().reserve(kind, scope)
        }
        r => r,
    }
}
fn track<K: Kernel>(inner: &Inner<K>, r: SlotRef, kind: SlotKind, scope: SlotRef) {
    inner.shared().track(SlotEntry {
        r,
        kind,
        scope,
        state: SlotState::Live,
        aborted: false,
    });
}
fn poll_book<K: Kernel>(
    inner: &Inner<K>,
    request: &mut BookRequest<Backend<K>>,
    cx: &mut Context<'_>,
) -> Poll<Result<Completion>> {
    match request.take() {
        Ok(Some(c)) => {
            inner.forget(request.cookie());
            Poll::Ready(Ok(c))
        }
        Ok(None) => {
            inner.register(request.cookie(), cx.waker());
            Poll::Pending
        }
        Err(e) => {
            inner.forget(request.cookie());
            Poll::Ready(Err(inner.fault(e)))
        }
    }
}

pub(crate) fn target<K: Kernel>(
    inner: &Rc<Inner<K>>,
    registry_fd: i32,
    name: &[u8],
) -> Result<Target<K>> {
    inner.admit()?;
    if name.is_empty() || name.len() > 255 {
        return Err(-EINVAL);
    }
    let mut record = TargetRecord {
        size: 288,
        version: 1,
        flags: 0,
        registry_fd,
        name_len: name.len() as u32,
        name: [0; 256],
        result: ZERO_REF,
    };
    record.name[..name.len()].copy_from_slice(name);
    let rc = inner
        .shared()
        .register(REGISTER_SERVICE_TARGET, &mut record);
    if rc < 0 {
        return Err(rc as i32);
    }
    if !minted(record.result) {
        return Err(-EIO);
    }
    track(inner, record.result, SlotKind::Target, ZERO_REF);
    Ok(Target {
        inner: inner.clone(),
        r: record.result,
    })
}

pub(crate) fn pool<K: Kernel>(inner: &Rc<Inner<K>>, ranges: &[PoolMember]) -> Result<Pool<K>> {
    inner.admit()?;
    let core = inner.driver().register_pool(ranges).map_err(errno)?;
    Ok(Pool {
        inner: inner.clone(),
        r: core.identity(),
        core: Rc::new(core),
        members: ranges.len(),
    })
}

/// A service named in a registry. Dropping it reaps the slot once no CONNECT
/// still borrows it.
pub struct Target<K: Kernel> {
    inner: Rc<Inner<K>>,
    r: SlotRef,
}
impl<K: Kernel> Target<K> {
    /// Open a new connection: a fresh 9P session to the service, and the scope
    /// that owns it. `deadline` (absolute, monotonic ns) bounds the handshake.
    pub fn connect(&self, deadline: Option<u64>) -> Result<Connect<K>> {
        let inner = &self.inner;
        inner.admit()?;
        let scope = reserve(inner, SERVICE_SCOPE, ZERO_REF)?;
        track(inner, scope, SlotKind::Scope, scope);
        let mut s = sqe(OP_SERVICE_CONNECT as u8, self.r, deadline);
        s.offset = scope.slot as u64;
        s.args[2] = scope.incarnation;
        let request = inner.driver().request(scope, Kind::Slot(scope.slot));
        let submitted = request.map_err(errno).and_then(|mut request| {
            inner
                .submit(s, &[], || unsafe { request.publish() })
                .map(|_| request)
        });
        match submitted {
            Ok(request) => Ok(Connect {
                inner: inner.clone(),
                scope: Some(scope),
                request,
            }),
            Err(e) => {
                inner.shared().set_state(scope, SlotState::Reap);
                Err(e)
            }
        }
    }
}
impl<K: Kernel> Drop for Target<K> {
    fn drop(&mut self) {
        self.inner.shared().set_state(self.r, SlotState::Reap);
    }
}

/// CONNECT in flight. Dropping it aborts the scope it was creating.
pub struct Connect<K: Kernel> {
    inner: Rc<Inner<K>>,
    scope: Option<SlotRef>,
    request: BookRequest<Backend<K>>,
}
impl<K: Kernel> Future for Connect<K> {
    type Output = Result<Scope<K>>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let Some(scope) = me.scope else {
            return Poll::Ready(Err(-EINVAL));
        };
        match poll_book(&me.inner, &mut me.request, cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Ok(c)) if c.result >= 0 => {
                me.scope = None;
                Poll::Ready(Ok(Scope {
                    core: Rc::new(ScopeCore {
                        inner: me.inner.clone(),
                        r: scope,
                    }),
                }))
            }
            Poll::Ready(Ok(c)) => Poll::Ready(Err(c.result)),
            Poll::Ready(Err(e)) => Poll::Ready(Err(e)),
        }
    }
}
impl<K: Kernel> Drop for Connect<K> {
    fn drop(&mut self) {
        self.inner.forget(self.request.cookie());
        if let Some(scope) = self.scope.take() {
            self.inner.shared().abort(scope);
            self.inner.shared().set_state(scope, SlotState::Reap);
        }
    }
}

struct ScopeCore<K: Kernel> {
    inner: Rc<Inner<K>>,
    r: SlotRef,
}
impl<K: Kernel> Drop for ScopeCore<K> {
    fn drop(&mut self) {
        self.inner.shared().abort(self.r);
        self.inner.shared().set_state(self.r, SlotState::Reap);
    }
}

/// One connection. It lives while this handle, a fid of it, or an operation on
/// it does; when the last goes, the scope is aborted and its slot reaped.
pub struct Scope<K: Kernel> {
    core: Rc<ScopeCore<K>>,
}
impl<K: Kernel> Scope<K> {
    /// Walk one component from the service root: a name of 1..=255 bytes with no
    /// slash or NUL, neither "." nor "..", or empty to clone the root.
    pub fn walk(&self, name: &[u8], deadline: Option<u64>) -> Result<Walk<K>> {
        walk(&self.core, self.core.r, name, deadline)
    }
    /// Cancel the connection: every operation on it ends, siblings included,
    /// and no new one is admitted.
    pub fn abort(&self) {
        self.core.inner.shared().abort(self.core.r);
    }
}

fn walk<K: Kernel>(
    core: &Rc<ScopeCore<K>>,
    source: SlotRef,
    name: &[u8],
    deadline: Option<u64>,
) -> Result<Walk<K>> {
    let inner = &core.inner;
    inner.admit()?;
    if name.len() > 255 {
        return Err(-EINVAL);
    }
    let fid = reserve(inner, SERVICE_FID, core.r)?;
    track(inner, fid, SlotKind::Fid, core.r);
    let mut s = sqe(OP_WALK, source, deadline);
    s.offset = fid.slot as u64;
    s.args[2] = fid.incarnation;
    let request = inner.driver().request(core.r, Kind::Slot(fid.slot));
    let submitted = request.map_err(errno).and_then(|mut request| {
        inner
            .submit(s, name, || unsafe { request.publish() })
            .map(|_| request)
    });
    match submitted {
        Ok(request) => Ok(Walk {
            scope: core.clone(),
            fid: Some(fid),
            request,
        }),
        Err(e) => {
            inner.shared().set_state(fid, SlotState::Reap);
            Err(e)
        }
    }
}

/// WALK in flight. Dropping it cancels the scope (Loom has no narrower cancel).
pub struct Walk<K: Kernel> {
    scope: Rc<ScopeCore<K>>,
    fid: Option<SlotRef>,
    request: BookRequest<Backend<K>>,
}
impl<K: Kernel> Future for Walk<K> {
    type Output = Result<Fid<K>>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        let Some(fid) = me.fid else {
            return Poll::Ready(Err(-EINVAL));
        };
        match poll_book(&me.scope.inner, &mut me.request, cx) {
            Poll::Pending => Poll::Pending,
            Poll::Ready(Ok(c)) if c.result >= 0 => {
                me.fid = None;
                Poll::Ready(Ok(Fid {
                    scope: me.scope.clone(),
                    r: fid,
                }))
            }
            Poll::Ready(Ok(c)) => {
                me.fid = None;
                me.scope.inner.shared().set_state(fid, SlotState::Reap);
                Poll::Ready(Err(c.result))
            }
            Poll::Ready(Err(e)) => Poll::Ready(Err(e)),
        }
    }
}
impl<K: Kernel> Drop for Walk<K> {
    fn drop(&mut self) {
        self.scope.inner.forget(self.request.cookie());
        if let Some(fid) = self.fid.take() {
            // Only a WALK that succeeded unobserved left a live fid to clunk; a
            // failed one left the reservation, which REAP frees.
            let state = match self.request.take() {
                Ok(Some(c)) if c.result < 0 => SlotState::Reap,
                _ => SlotState::Clunk,
            };
            self.scope.inner.shared().set_state(fid, state);
        }
    }
}

/// A fid of a scope. Dropping it clunks it in the background.
pub struct Fid<K: Kernel> {
    scope: Rc<ScopeCore<K>>,
    r: SlotRef,
}
impl<K: Kernel> Fid<K> {
    pub fn walk(&self, name: &[u8], deadline: Option<u64>) -> Result<Walk<K>> {
        walk(&self.scope, self.r, name, deadline)
    }
    /// LOPEN with mode 0 (read), 1 (write) or 2 (read-write).
    pub fn open(&self, mode: u32, deadline: Option<u64>) -> Result<Open<K>> {
        let inner = &self.scope.inner;
        inner.admit()?;
        let mut s = sqe(OP_LOPEN, self.r, deadline);
        s.offset = mode as u64;
        let mut request = inner
            .driver()
            .request(self.scope.r, Kind::Control)
            .map_err(errno)?;
        inner.submit(s, &[], || unsafe { request.publish() })?;
        Ok(Open {
            scope: self.scope.clone(),
            request,
        })
    }
    /// Read file bytes at `offset` into `range` of a ring buffer. A buffer holds
    /// one fixed operation at a time, and none overlapping a pool.
    pub fn read(&self, range: PoolMember, offset: u64, deadline: Option<u64>) -> Result<Read<K>> {
        let inner = &self.scope.inner;
        inner.admit()?;
        let mut request = inner.driver().read(self.scope.r, range).map_err(errno)?;
        let s = fixed_sqe(OP_READ, self.r, range, offset, deadline);
        let cookie = inner.submit(s, &[], || unsafe { request.publish() })?;
        Ok(Read {
            scope: self.scope.clone(),
            request,
            cookie,
        })
    }
    /// Write `bytes` (exactly `range.length` of them) at `offset`, through `range`.
    pub fn write(
        &self,
        range: PoolMember,
        offset: u64,
        bytes: &[u8],
        deadline: Option<u64>,
    ) -> Result<Write<K>> {
        let inner = &self.scope.inner;
        inner.admit()?;
        let mut request = inner
            .driver()
            .write(self.scope.r, range, bytes)
            .map_err(errno)?;
        let s = fixed_sqe(OP_WRITE, self.r, range, offset, deadline);
        let cookie = inner.submit(s, &[], || unsafe { request.publish() })?;
        Ok(Write {
            scope: self.scope.clone(),
            request,
            cookie,
        })
    }
    /// Pooled reads of at most `limit` bytes each, from `offset`: one, or with
    /// `multishot` a stream that runs until its final completion. Each payload is
    /// a lease on a pool member, returned when it is dropped.
    pub fn stream(
        &self,
        pool: &Pool<K>,
        offset: u64,
        limit: u32,
        multishot: bool,
        deadline: Option<u64>,
    ) -> Result<Stream<K>> {
        let inner = &self.scope.inner;
        inner.admit()?;
        if !Rc::ptr_eq(inner, &pool.inner) {
            return Err(-EINVAL);
        }
        let state = Rc::new(RefCell::new(StreamState {
            payloads: VecDeque::with_capacity(pool.members),
            open: true,
        }));
        let kind = Kind::PooledRead {
            pool: pool.r,
            limit,
            multi: multishot,
        };
        let mut request = inner.driver().request(self.scope.r, kind).map_err(errno)?;
        let mut s = sqe(OP_READ, self.r, deadline);
        s.flags = SQE_BUFFER_SELECT as u8 | if multishot { SQE_MULTISHOT } else { 0 };
        s.offset = offset;
        s.len = limit;
        s.buf_idx_or_off = pool.r.slot;
        s.args[2] = pool.r.incarnation;
        let cookie = inner.submit(s, &[], || unsafe { request.publish() })?;
        inner.route(Route {
            cookie,
            pool: pool.core.clone(),
            stream: state.clone(),
        });
        Ok(Stream {
            scope: self.scope.clone(),
            request,
            state,
            ended: false,
        })
    }
}
impl<K: Kernel> Drop for Fid<K> {
    fn drop(&mut self) {
        self.scope
            .inner
            .shared()
            .set_state(self.r, SlotState::Clunk);
    }
}
fn fixed_sqe(
    opcode: u8,
    fid: SlotRef,
    range: PoolMember,
    offset: u64,
    deadline: Option<u64>,
) -> Submission {
    let mut s = sqe(opcode, fid, deadline);
    s.offset = offset;
    s.len = range.length as u32;
    s.buf_idx_or_off = range.buffer_index;
    s.args[0] = range.offset;
    s
}

/// LOPEN in flight.
pub struct Open<K: Kernel> {
    scope: Rc<ScopeCore<K>>,
    request: BookRequest<Backend<K>>,
}
impl<K: Kernel> Future for Open<K> {
    type Output = Result<()>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        poll_book(&me.scope.inner, &mut me.request, cx)
            .map(|r| r.and_then(|c| if c.result < 0 { Err(c.result) } else { Ok(()) }))
    }
}
impl<K: Kernel> Drop for Open<K> {
    fn drop(&mut self) {
        self.scope.inner.forget(self.request.cookie());
    }
}

/// A fixed READ in flight.
pub struct Read<K: Kernel> {
    scope: Rc<ScopeCore<K>>,
    request: ReadRequest<Backend<K>>,
    cookie: u64,
}
/// A finished fixed READ. It holds its buffer range until dropped.
pub struct ReadDone<K: Kernel> {
    done: ReadCompletion<Backend<K>>,
}
impl<K: Kernel> ReadDone<K> {
    pub fn bytes(&self) -> &[u8] {
        self.done.bytes().expect("a successful read's bytes")
    }
}
impl<K: Kernel> Future for Read<K> {
    type Output = Result<ReadDone<K>>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        match me.request.try_complete() {
            Ok(Some(done)) => {
                me.scope.inner.forget(me.cookie);
                if done.result() < 0 {
                    Poll::Ready(Err(done.result()))
                } else {
                    Poll::Ready(Ok(ReadDone { done }))
                }
            }
            Ok(None) => {
                me.scope.inner.register(me.cookie, cx.waker());
                Poll::Pending
            }
            Err(e) => {
                me.scope.inner.forget(me.cookie);
                Poll::Ready(Err(me.scope.inner.fault(e)))
            }
        }
    }
}
impl<K: Kernel> Drop for Read<K> {
    fn drop(&mut self) {
        self.scope.inner.forget(self.cookie);
    }
}

/// A fixed WRITE in flight; it finishes with the byte count the server took.
pub struct Write<K: Kernel> {
    scope: Rc<ScopeCore<K>>,
    request: WriteRequest<Backend<K>>,
    cookie: u64,
}
impl<K: Kernel> Future for Write<K> {
    type Output = Result<usize>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        let me = self.get_mut();
        match me.request.try_complete() {
            Ok(Some(c)) => {
                me.scope.inner.forget(me.cookie);
                Poll::Ready(if c.result < 0 {
                    Err(c.result)
                } else {
                    Ok(c.result as usize)
                })
            }
            Ok(None) => {
                me.scope.inner.register(me.cookie, cx.waker());
                Poll::Pending
            }
            Err(e) => {
                me.scope.inner.forget(me.cookie);
                Poll::Ready(Err(me.scope.inner.fault(e)))
            }
        }
    }
}
impl<K: Kernel> Drop for Write<K> {
    fn drop(&mut self) {
        self.scope.inner.forget(self.cookie);
    }
}

/// Registered pool members for pooled reads. The pool's slot is reaped once
/// this handle and every stream that names it are gone and every lease is back.
pub struct Pool<K: Kernel> {
    inner: Rc<Inner<K>>,
    core: Rc<CorePool<Backend<K>>>,
    r: SlotRef,
    members: usize,
}

/// One leased pool member's bytes. Dropping it returns the lease.
pub struct Payload<K: Kernel>(CorePayload<Backend<K>>);
impl<K: Kernel> Payload<K> {
    pub fn bytes(&self) -> &[u8] {
        self.0.bytes()
    }
    /// Return the lease now; a refusal hands the payload back with the errno.
    pub fn try_return(self) -> core::result::Result<(), (i32, Payload<K>)> {
        self.0.try_return().map_err(|(rc, p)| {
            let rc = if rc < 0 && rc >= i32::MIN as i64 {
                rc as i32
            } else {
                -EIO
            };
            (rc, Payload(p))
        })
    }
}

/// What a stream yields: a payload, or its final completion's result (a byte
/// count, or a negative errno) after which it yields nothing.
pub enum Shot<K: Kernel> {
    Payload(Payload<K>),
    End(i32),
}

/// A pooled READ, single or multishot. Dropping it before its end cancels the
/// scope; payloads already delivered stay valid.
pub struct Stream<K: Kernel> {
    scope: Rc<ScopeCore<K>>,
    request: BookRequest<Backend<K>>,
    state: Rc<RefCell<StreamState<K>>>,
    ended: bool,
}
impl<K: Kernel> Stream<K> {
    /// The next shot, or None while none has arrived.
    pub fn try_next(&mut self) -> Result<Option<Shot<K>>> {
        if self.ended {
            return Err(-EINVAL);
        }
        let payload = self.state.borrow_mut().payloads.pop_front();
        if let Some(p) = payload {
            return Ok(Some(Shot::Payload(Payload(p))));
        }
        match self.request.take() {
            Ok(Some(c)) => {
                self.ended = true;
                self.scope.inner.forget(self.request.cookie());
                Ok(Some(Shot::End(c.result)))
            }
            Ok(None) => Ok(None),
            Err(e) => {
                self.ended = true;
                self.scope.inner.forget(self.request.cookie());
                Err(self.scope.inner.fault(e))
            }
        }
    }
    pub fn poll_next(&mut self, cx: &mut Context<'_>) -> Poll<Result<Shot<K>>> {
        match self.try_next() {
            Ok(Some(shot)) => Poll::Ready(Ok(shot)),
            Ok(None) => {
                self.scope.inner.register(self.request.cookie(), cx.waker());
                Poll::Pending
            }
            Err(e) => Poll::Ready(Err(e)),
        }
    }
    /// A future for the next shot.
    pub fn next(&mut self) -> Next<'_, K> {
        Next { stream: self }
    }
}
impl<K: Kernel> Drop for Stream<K> {
    fn drop(&mut self) {
        self.scope.inner.forget(self.request.cookie());
        let held = {
            let mut s = self.state.borrow_mut();
            s.open = false;
            core::mem::take(&mut s.payloads)
        };
        drop(held);
    }
}
pub struct Next<'a, K: Kernel> {
    stream: &'a mut Stream<K>,
}
impl<K: Kernel> Future for Next<'_, K> {
    type Output = Result<Shot<K>>;
    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
        self.get_mut().stream.poll_next(cx)
    }
}
