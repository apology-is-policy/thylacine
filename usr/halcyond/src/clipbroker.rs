//! Single-owner clipboard admission, between authenticated controller routing
//! and the asynchronous compositor channel. This module is NOT a peer decoder:
//! Authority must come from the registered kernel-backed controller, never an
//! application's claimed PID/scope. No endpoint is activated by this module.
//!
//! One pending decision globally serializes publication and bounds metadata.
//! Get pins bytes and Commit validates them before issuing CHECK. IDs never
//! repeat. Completion, invalidation, expiry and publication run on the service owner.
//! A graphical focus loss records an epoch boundary for an outstanding Get or
//! Commit: an earlier admitted decision may finish, but a later one cannot use
//! focus that returned in the meantime. Controller loss/SAK cancels immediately.
use crate::clipboard::{Clipboard, CommitTicket, Owner, ReadTicket, Snapshot};
use libhalcyon::{
    interaction_control::{Op, Reply, Request},
    interaction_wire::{Failure, MAX_TEXT},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Authority {
    pub owner: Owner,
    pub leaf: u32,
    pub binding: u64,
    pub foreground: u64,
    pub subject: u64,
}
/// The connection and fid incarnation are broker identities; request is the
/// client's HIN1 request ID. A reused numeric fid must have a new incarnation.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Target {
    pub connection: u64,
    pub fid: u64,
    pub request: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Outcome {
    Begun(u64),
    Clipboard(Snapshot),
    Committed(u64),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Completed {
    pub target: Target,
    pub result: Result<Outcome, Failure>,
}
enum Action {
    Begin(usize),
    Get(ReadTicket),
    Commit(CommitTicket),
}
struct Pending {
    authority: Authority,
    target: Target,
    check: Request,
    action: Action,
    started: u64,
    focus_lost: Option<u64>,
    seat: u64,
}
/// Time is monotonic milliseconds, as in the payload store. Transport expiry
/// never grants authority and never licenses reuse of an in-flight I/O buffer.
pub const ADMISSION_MS: u64 = 30_000;
pub struct Broker {
    store: Clipboard,
    session: u64,
    seat: Option<u64>,
    next: u64,
    pending: Option<Pending>,
}
const _: () = assert!(core::mem::size_of::<Broker>() <= 1024);

impl Broker {
    pub fn new(session: u64) -> Result<Self, Failure> {
        Ok(Self {
            store: Clipboard::new(session)?,
            session,
            seat: None,
            next: 1,
            pending: None,
        })
    }
    pub fn generation(&self) -> u64 {
        self.store.generation()
    }
    pub fn payload_reservation(&self) -> usize {
        self.store.payload_reservation()
    }
    /// The interaction owner shares this sequence with Bind/Publish/Unbind.
    /// Its transport slot, not `pending`, retains cancelled in-flight control
    /// work until the actual completion. Never create a second HIA sequencer.
    pub(crate) fn control_id(&mut self) -> Result<u64, Failure> {
        if self.seat.is_none() {
            return Err(Failure::Denied);
        }
        if self.pending.is_some() {
            return Err(Failure::Busy);
        }
        let id = self.next;
        self.next = id.checked_add(1).ok_or(Failure::Busy)?;
        Ok(id)
    }
    fn check(&self, a: Authority, t: Target) -> Result<Request, Failure> {
        if self.seat.is_none() {
            return Err(Failure::Denied);
        }
        if self.pending.is_some() {
            return Err(Failure::Busy);
        }
        if t.connection != a.owner.connection
            || t.connection == 0
            || t.fid == 0
            || t.request == 0
            || a.owner.scope.session != self.session
        {
            return Err(Failure::Denied);
        }
        self.next.checked_add(1).ok_or(Failure::Busy)?;
        let r = Request {
            op: Op::Check,
            request: self.next,
            leaf: a.leaf,
            binder_pid: 0,
            binding: a.binding,
            foreground: a.foreground,
            subject: a.subject,
            controller: a.owner.scope.controller,
            context: a.owner.scope.context,
            epoch: a.owner.scope.epoch,
        };
        if Request::decode(&r.encode()) != Some(r) {
            return Err(Failure::Invalid);
        }
        Ok(r)
    }
    fn enqueue(
        &mut self,
        a: Authority,
        t: Target,
        r: Request,
        action: Action,
        now: u64,
    ) -> Request {
        self.next += 1; // check() validated overflow before any payload mutation.
        self.pending = Some(Pending {
            authority: a,
            target: t,
            check: r,
            action,
            started: now,
            focus_lost: None,
            seat: self.seat.unwrap(),
        });
        r
    }
    pub fn begin(
        &mut self,
        a: Authority,
        t: Target,
        length: usize,
        now: u64,
    ) -> Result<Request, Failure> {
        let r = self.check(a, t)?;
        if length > MAX_TEXT {
            return Err(Failure::TooLarge);
        }
        Ok(self.enqueue(a, t, r, Action::Begin(length), now))
    }
    pub fn get(&mut self, a: Authority, t: Target, now: u64) -> Result<Request, Failure> {
        let r = self.check(a, t)?;
        let ticket = self.store.prepare_read(a.owner, now)?;
        Ok(self.enqueue(a, t, r, Action::Get(ticket), now))
    }
    pub fn commit(
        &mut self,
        a: Authority,
        t: Target,
        id: u64,
        expected: u64,
        now: u64,
    ) -> Result<Request, Failure> {
        let r = self.check(a, t)?;
        let ticket = self.store.prepare_commit(a.owner, id, expected, now)?;
        Ok(self.enqueue(a, t, r, Action::Commit(ticket), now))
    }
    fn reject(&mut self, p: Pending, error: Failure) -> Completed {
        match p.action {
            Action::Begin(_) => {}
            Action::Get(t) => self.store.reject_read(t),
            Action::Commit(t) => self.store.reject_commit(t),
        }
        Completed {
            target: p.target,
            result: Err(error),
        }
    }
    /// Reply comes exclusively from the trusted channel. Wrong/late request IDs
    /// cannot consume a newer pending action. A malformed MATCHING reply fails
    /// that action and releases its reserved payload; it is never retried.
    pub fn complete(
        &mut self,
        id: u64,
        result: Result<Reply, Failure>,
        now: u64,
    ) -> Option<Completed> {
        if self.pending.as_ref()?.check.request != id {
            return None;
        }
        let p = self.pending.take().unwrap();
        if now.checked_sub(p.started).is_none_or(|n| n >= ADMISSION_MS) {
            return Some(self.reject(p, Failure::Timeout));
        }
        let r = match result {
            Ok(r) => r,
            Err(e) => return Some(self.reject(p, e)),
        };
        if r.op != Op::Check
            || r.request != id
            || r.foreground != p.authority.foreground
            || self.seat != Some(p.seat)
            || r.seat != p.seat
            || r.focus == 0
            || r.focus == u64::MAX
            || p.focus_lost.is_some_and(|lost| r.focus >= lost)
        {
            return Some(self.reject(p, Failure::Gone));
        }
        let result = match p.action {
            Action::Begin(length) => self
                .store
                .begin_admitted(p.authority.owner, length, now)
                .map(Outcome::Begun),
            Action::Get(t) => self.store.admit_read(t, now).map(Outcome::Clipboard),
            Action::Commit(t) => self.store.publish(t, now).map(Outcome::Committed),
        };
        Some(Completed {
            target: p.target,
            result,
        })
    }
    /// Focus epochs come from ordered compositor notifications. A pending copy
    /// Begin is cancelled outright: allocating staging after observed focus loss
    /// would resurrect a write that should already have been discarded.
    pub fn lose_focus(&mut self, owner: Owner, epoch: u64) -> Option<Completed> {
        let matching = self
            .pending
            .as_ref()
            .is_some_and(|p| p.authority.owner == owner);
        let preserve =
            matching && matches!(self.pending.as_ref().unwrap().action, Action::Commit(_));
        if !preserve {
            self.store.lose_focus(owner);
        }
        if matching {
            if matches!(self.pending.as_ref().unwrap().action, Action::Begin(_)) {
                let p = self.pending.take().unwrap();
                return Some(self.reject(p, Failure::Gone));
            }
            let p = self.pending.as_mut().unwrap();
            p.focus_lost = Some(p.focus_lost.map_or(epoch, |e| e.min(epoch)));
        }
        None
    }
    /// Seat samples are trusted; None means SAK/unavailable. Any generation
    /// transition cancels work before processing a completion from that pass.
    pub fn seat(&mut self, normal: Option<u64>) -> Option<Completed> {
        if self.seat == normal {
            return None;
        }
        self.seat = normal;
        let cancelled = self.pending.take().map(|p| self.reject(p, Failure::Gone));
        self.store.cancel_all_transfers();
        cancelled
    }
    pub fn drop_owner(&mut self, owner: Owner) -> Option<Completed> {
        let c = if self
            .pending
            .as_ref()
            .is_some_and(|p| p.authority.owner == owner)
        {
            let p = self.pending.take().unwrap();
            Some(self.reject(p, Failure::Gone))
        } else {
            None
        };
        self.store.drop_owner(owner);
        c
    }
    pub fn drop_connection(&mut self, connection: u64) -> Option<Completed> {
        let c = if self
            .pending
            .as_ref()
            .is_some_and(|p| p.target.connection == connection)
        {
            let p = self.pending.take().unwrap();
            Some(self.reject(p, Failure::Gone))
        } else {
            None
        };
        self.store.drop_connection(connection);
        c
    }
    /// Clunk/cancel names the exact transaction, not merely a reusable fid ID.
    pub fn cancel_pending(&mut self, t: Target) -> Option<Completed> {
        if self.pending.as_ref()?.target != t {
            return None;
        }
        let p = self.pending.take().unwrap();
        Some(self.reject(p, Failure::Gone))
    }
    pub fn expire(&mut self, now: u64) -> Option<Completed> {
        let c = if self
            .pending
            .as_ref()
            .is_some_and(|p| now.checked_sub(p.started).is_none_or(|n| n >= ADMISSION_MS))
        {
            let p = self.pending.take().unwrap();
            Some(self.reject(p, Failure::Timeout))
        } else {
            None
        };
        self.store.expire(now);
        c
    }
    pub fn deadline(&self) -> Option<u64> {
        let a = self
            .pending
            .as_ref()
            .map(|p| p.started.saturating_add(ADMISSION_MS));
        match (a, self.store.next_deadline()) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (a, b) => a.or(b),
        }
    }
    pub fn write(
        &mut self,
        owner: Owner,
        id: u64,
        offset: usize,
        bytes: &[u8],
        now: u64,
    ) -> Result<usize, Failure> {
        if self.seat.is_none() {
            return Err(Failure::Denied);
        }
        self.store.write(owner, id, offset, bytes, now)
    }
    pub fn read(
        &mut self,
        owner: Owner,
        id: u64,
        offset: usize,
        count: usize,
        now: u64,
    ) -> Result<&[u8], Failure> {
        if self.seat.is_none() {
            return Err(Failure::Denied);
        }
        self.store.read(owner, id, offset, count, now)
    }
    pub fn cancel_transfer(&mut self, owner: Owner, id: u64) -> Result<bool, Failure> {
        self.store.cancel(owner, id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libhalcyon::interaction_body::Scope;
    fn a(n: u64) -> Authority {
        Authority {
            owner: Owner {
                connection: n,
                scope: Scope {
                    session: 7,
                    controller: n,
                    context: 1,
                    epoch: 1,
                },
            },
            leaf: n as u32,
            binding: n,
            foreground: 1,
            subject: n,
        }
    }
    fn t(n: u64) -> Target {
        Target {
            connection: n,
            fid: 1,
            request: 1,
        }
    }
    fn broker() -> Broker {
        let mut b = Broker::new(7).unwrap();
        b.seat(Some(1));
        b
    }
    fn reply(q: Request, focus: u64) -> Reply {
        Reply {
            op: Op::Check,
            request: q.request,
            focus,
            seat: 1,
            foreground: q.foreground,
        }
    }
    fn done(b: &mut Broker, q: Request) -> Outcome {
        b.complete(q.request, Ok(reply(q, 10)), 0)
            .unwrap()
            .result
            .unwrap()
    }
    fn begin(b: &mut Broker, n: u64, len: usize) -> u64 {
        let q = b.begin(a(n), t(n), len, 0).unwrap();
        match done(b, q) {
            Outcome::Begun(id) => id,
            _ => panic!(),
        }
    }
    fn stage(b: &mut Broker, n: u64, text: &[u8]) -> u64 {
        let id = begin(b, n, text.len());
        if !text.is_empty() {
            b.write(a(n).owner, id, 0, text, 0).unwrap();
        }
        id
    }
    fn copy(b: &mut Broker, n: u64, text: &[u8]) {
        let id = stage(b, n, text);
        let q = b.commit(a(n), t(n), id, b.generation(), 0).unwrap();
        assert!(matches!(done(b, q), Outcome::Committed(_)));
    }
    #[test]
    fn delayed_mismatched_duplicate_and_reused_fid_replies() {
        let mut b = broker();
        let q = b.begin(a(1), t(1), 0, 0).unwrap();
        assert_eq!(b.get(a(2), t(2), 0), Err(Failure::Busy));
        assert_eq!(b.complete(q.request + 1, Ok(reply(q, 10)), 0), None);
        let wrong = Target { fid: 2, ..t(1) };
        assert_eq!(b.cancel_pending(wrong), None);
        assert!(b.cancel_pending(t(1)).is_some());
        let q2 = b.begin(a(1), Target { fid: 2, ..t(1) }, 0, 0).unwrap();
        assert_ne!(q.request, q2.request);
        assert_eq!(b.complete(q.request, Ok(reply(q, 10)), 0), None);
        assert!(matches!(done(&mut b, q2), Outcome::Begun(_)));
        assert_eq!(b.complete(q2.request, Ok(reply(q2, 10)), 0), None);
    }
    #[test]
    fn focus_loss_after_admission_preserves_commit_but_cannot_admit_after_return() {
        for admitted_first in [true, false] {
            let mut b = broker();
            copy(&mut b, 2, b"old");
            let id = stage(&mut b, 1, b"new");
            let q = b.commit(a(1), t(1), id, 1, 0).unwrap();
            b.lose_focus(a(1).owner, 11);
            let c = b
                .complete(
                    q.request,
                    Ok(reply(q, if admitted_first { 10 } else { 12 })),
                    0,
                )
                .unwrap();
            if admitted_first {
                assert_eq!(c.result, Ok(Outcome::Committed(2)));
            } else {
                assert_eq!(c.result, Err(Failure::Gone));
                assert_eq!(b.generation(), 1);
            }
        }
    }
    #[test]
    fn begin_is_not_resurrected_and_existing_staging_dies_on_focus_loss() {
        let mut b = broker();
        let q = b.begin(a(1), t(1), 5, 0).unwrap();
        b.lose_focus(a(1).owner, 11);
        assert_eq!(b.complete(q.request, Ok(reply(q, 10)), 0), None);
        assert_eq!(b.payload_reservation(), 0);
        let id = stage(&mut b, 1, b"stale");
        b.lose_focus(a(1).owner, 12);
        assert_eq!(b.commit(a(1), t(1), id, 0, 0), Err(Failure::Gone));
    }
    #[test]
    fn admitted_reads_finish_after_focus_but_not_controller_loss() {
        let mut b = broker();
        copy(&mut b, 1, b"first");
        let q = b.get(a(2), t(2), 0).unwrap();
        b.lose_focus(a(2).owner, 11);
        let Outcome::Clipboard(s) = done(&mut b, q) else {
            panic!()
        };
        copy(&mut b, 1, b"second");
        assert_eq!(b.read(a(2).owner, s.transfer, 0, 16, 0), Ok(&b"first"[..]));
        b.drop_owner(a(2).owner);
        assert_eq!(b.read(a(2).owner, s.transfer, 0, 16, 0), Err(Failure::Gone));
        let q = b.get(a(2), t(2), 0).unwrap();
        b.lose_focus(a(2).owner, 11);
        assert_eq!(
            b.complete(q.request, Ok(reply(q, 12)), 0).unwrap().result,
            Err(Failure::Gone)
        );
    }
    #[test]
    fn revocation_before_receipt_cancels_even_an_admitted_commit() {
        for cause in 0..4 {
            let mut b = broker();
            copy(&mut b, 2, b"retain");
            let id = stage(&mut b, 1, b"new");
            let q = b.commit(a(1), t(1), id, 1, 0).unwrap();
            match cause {
                0 => {
                    b.drop_owner(a(1).owner);
                }
                1 => {
                    b.drop_connection(1);
                }
                2 => {
                    b.seat(None);
                }
                _ => {
                    b.seat(Some(2));
                }
            }
            assert_eq!(b.complete(q.request, Ok(reply(q, 10)), 0), None);
            assert_eq!(b.generation(), 1);
            assert_eq!(b.payload_reservation(), 6);
        }
    }
    #[test]
    fn receipt_must_match_operation_foreground_and_seat() {
        for bad in 0..5 {
            let mut b = broker();
            let q = b.get(a(1), t(1), 0).unwrap();
            let mut r = reply(q, 10);
            match bad {
                0 => r.op = Op::Publish,
                1 => r.request += 1,
                2 => r.foreground += 1,
                3 => r.seat += 1,
                _ => r.focus = u64::MAX,
            }
            assert_eq!(
                b.complete(q.request, Ok(r), 0).unwrap().result,
                Err(Failure::Gone)
            );
            assert!(b.get(a(1), t(1), 0).is_ok());
        }
    }
    #[test]
    fn allocation_validation_and_conflict_precede_check() {
        let mut b = broker();
        assert_eq!(b.begin(a(1), t(1), MAX_TEXT + 1, 0), Err(Failure::TooLarge));
        let id = stage(&mut b, 1, b"\xff");
        assert_eq!(b.commit(a(1), t(1), id, 0, 0), Err(Failure::Invalid));
        b.cancel_transfer(a(1).owner, id).unwrap();
        copy(&mut b, 2, b"old");
        let id = stage(&mut b, 1, b"new");
        assert_eq!(b.commit(a(1), t(1), id, 0, 0), Err(Failure::Conflict));
        assert!(b.pending.is_none());
        let q = b.commit(a(1), t(1), id, 1, 0).unwrap();
        assert_eq!(
            b.complete(q.request, Err(Failure::Denied), 0)
                .unwrap()
                .result,
            Err(Failure::Denied)
        );
        assert_eq!(b.generation(), 1);
    }
    #[test]
    fn deadline_regression_and_identifier_exhaustion_release_reservations() {
        for now in [0, 30_001] {
            let mut b = broker();
            let q = b.get(a(1), t(1), 1).unwrap();
            assert_eq!(b.deadline(), Some(30_001));
            assert_eq!(
                b.complete(q.request, Ok(reply(q, 10)), now).unwrap().result,
                Err(Failure::Timeout)
            );
            assert_eq!(b.deadline(), None);
        }
        let mut b = broker();
        let q = b.get(a(1), t(1), 0).unwrap();
        assert!(b.expire(30_000).is_some());
        assert_eq!(b.complete(q.request, Ok(reply(q, 10)), 30_000), None);
        b.next = u64::MAX;
        assert_eq!(b.get(a(1), t(1), 0), Err(Failure::Busy));
        assert_eq!(b.deadline(), None);
    }
    #[test]
    fn wrong_session_connection_and_uninitialized_seat_cannot_queue() {
        let mut b = Broker::new(7).unwrap();
        assert_eq!(b.begin(a(1), t(1), 0, 0), Err(Failure::Denied));
        b.seat(Some(1));
        assert_eq!(b.get(a(1), t(2), 0), Err(Failure::Denied));
        let mut wrong = a(1);
        wrong.owner.scope.session = 8;
        assert_eq!(b.get(wrong, t(1), 0), Err(Failure::Denied));
        assert!(b.pending.is_none());
    }
}
