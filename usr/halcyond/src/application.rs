//! Per-connection HIN1 dispatch over the authenticated interaction owner.
//!
//! The native adapter supplies a fresh kernel peer, a walked live route, and
//! remaining transport budgets. Eight independently replayable fids share one
//! controller and one pending admission. Cached reads retain only transfer
//! coordinates: every read borrows the existing admitted snapshot again. No
//! response owns a second text buffer. Ordered invalidations and output-frame
//! retirement must precede HSC acknowledgement in the native executor.
use crate::{apprecord::{CachedReply, Progress, Record, Ticket},
    clipbroker::{Completed, Outcome, Target}, controllers::Peer,
    hostbindings::{Bindings, Desired}, interaction::{Completion, Interaction}, paneroute::Route};
use libhalcyon::{interaction_body::{EncodedResponse, Request, Response, Scope},
    interaction_control::Request as Admission, interaction_wire::{Failure, MAX_RECORD}};

pub const FIDS: usize = 8;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WriteResult { Partial, Answered, Pending(Admission) }
#[derive(Clone, Copy)]
enum Saved {
    Fixed(Response<'static>),
    Read { scope: Scope, transfer: u64, offset: u32, count: u32 },
}
impl CachedReply for Saved { fn reserved_bytes(&self) -> usize { 0 } }
struct Fid { id: u64, route: Route, record: Record<Result<Saved, Failure>>, answered: u64 }
#[derive(Clone, Copy)]
struct Pending { ticket: Ticket, request: Admission, target: Target }
pub struct Application {
    session: u64,
    peer: Peer,
    scope: Option<Scope>,
    fids: [Option<Fid>; FIDS],
    pending: Option<Pending>,
    last_fid: u64,
    route: Option<Route>,
}
const _: () = assert!(core::mem::size_of::<Application>() <= 4 * 1024);
impl Application {
    /// Peer fields originate from t_srv_peer on the accepted connection.
    pub fn new(session: u64, peer: Peer) -> Result<Self, Failure> {
        if session == 0 || peer.connection == 0 || peer.stripes == 0 || !peer.alive {
            return Err(Failure::Denied);
        }
        Ok(Self { session, peer, scope: None, fids: core::array::from_fn(|_| None),
            pending: None, last_fid: 0, route: None })
    }
    fn peer(&self, fresh: Peer) -> Result<(), Failure> {
        if fresh != self.peer || !fresh.alive { Err(Failure::Gone) } else { Ok(()) }
    }
    fn index(&self, id: u64) -> Result<usize, Failure> {
        self.fids.iter().position(|f| f.as_ref().is_some_and(|f| f.id == id)).ok_or(Failure::BadHandle)
    }
    /// Incarnations are monotone local IDs, never client-selected numeric fids.
    pub fn open(&mut self, id: u64, route: Route) -> Result<(), Failure> {
        if id <= self.last_fid || route.incarnation == 0 { return Err(Failure::Invalid); }
        // A bound control connection cannot smuggle another leaf's locator.
        if self.route.is_some_and(|pinned| pinned != route) { return Err(Failure::Denied); }
        let slot = self.fids.iter_mut().find(|f| f.is_none()).ok_or(Failure::Busy)?;
        *slot = Some(Fid { id, route, record: Record::new(id)?, answered: 0 });
        self.last_fid = id;
        self.route = Some(route);
        Ok(())
    }
    pub fn input_reserved(&self) -> usize {
        self.fids.iter().flatten().map(|f| f.record.input_reserved()).sum()
    }
    pub fn output_reserved(&self) -> usize { 0 } // all reply metadata is inline
    pub fn pending_request(&self) -> Option<Admission> { self.pending.map(|p| p.request) }
    pub fn scope(&self) -> Option<Scope> { self.scope }
    pub fn write(&mut self, fid: u64, fresh: Peer, offset: u64, bytes: &[u8],
        input_allowance: usize, owner: &mut Interaction, bindings: &Bindings,
        desired: &Desired, now: u64) -> Result<WriteResult, Failure> {
        self.peer(fresh)?;
        let index = self.index(fid)?;
        let others = self.input_reserved() - self.fids[index].as_ref().unwrap().record.input_reserved();
        let allowance = input_allowance.min(MAX_RECORD).checked_sub(others).ok_or(Failure::TooLarge)?;
        let f = self.fids[index].as_mut().unwrap();
        let ticket = match f.record.write(offset, bytes, allowance)? {
            Progress::Partial => return Ok(WriteResult::Partial),
            Progress::Replay => return Ok(WriteResult::Answered),
            Progress::Dispatch(ticket) => ticket,
        };
        let target = Target { connection: fresh.connection, fid, request: ticket.request() };
        let request = f.record.request(ticket).ok_or(Failure::Invalid)?;
        // A second request may complete synchronously (including Cancel), but
        // never takes the first one's single asynchronous reply slot.
        let mut retired = None;
        let result = dispatch(self.session, &mut self.scope, self.pending.is_some(), f.route,
            fresh, target, request, owner, bindings, desired, now, &mut retired);
        match result {
            Ok(Action::Pending(request)) => {
                self.pending = Some(Pending { ticket, request, target });
                Ok(WriteResult::Pending(request))
            }
            result => {
                let saved = result.map(|a| match a { Action::Ready(s) => s, _ => unreachable!() });
                f.record.finish(ticket, saved, 0)?;
                f.answered = ticket.request();
                if let Some(done) = retired { self.complete(Completion::Clipboard(done)); }
                Ok(WriteResult::Answered)
            }
        }
    }
    /// Native HIA decision path. Re-sample this accepted connection before
    /// calling: peer exit must cancel CHECK as well as provisional Publish,
    /// before a successful receipt can mutate the clipboard. On peer loss the
    /// caller closes the application connection; the HIA receipt still drains.
    pub fn decision(&mut self, owner: &mut Interaction, request: Admission,
        fresh: Peer, result: Result<libhalcyon::interaction_control::Reply, Failure>,
        now: u64) -> Result<bool, Failure> {
        if self.pending.map(|p| p.request) != Some(request) { return Ok(false); }
        if let Err(error) = self.peer(fresh) {
            self.retire(owner);
            owner.complete(request, Some(fresh), Err(Failure::Gone), now);
            return Err(error);
        }
        Ok(owner.complete(request, Some(fresh), result, now).is_some_and(|done| self.complete(done)))
    }
    /// Called after Interaction::complete has validated the exact HIA receipt
    /// and, for Publish, sampled the accepted connection's fresh native peer.
    pub fn complete(&mut self, done: Completion) -> bool {
        let Some(p) = self.pending else { return false; };
        let saved = match done {
            Completion::Published { request, result } if request == p.request => {
                result.map(|scope| { self.scope = Some(scope); Saved::Fixed(Response::Bound { controller: scope.controller }) })
            }
            Completion::Clipboard(Completed { target, result }) if target == p.target =>
                result.map(|outcome| Saved::Fixed(match outcome {
                    Outcome::Begun(transfer) => Response::Begun { transfer },
                    Outcome::Clipboard(s) => Response::Clipboard { transfer: s.transfer, generation: s.generation, length: s.length as u32 },
                    Outcome::Committed(generation) => Response::Committed { generation },
                })),
            _ => return false,
        };
        self.pending = None;
        let Ok(index) = self.index(p.target.fid) else { return false; };
        let f = self.fids[index].as_mut().unwrap();
        if f.record.finish(p.ticket, saved, 0).is_err() { return false; }
        f.answered = p.ticket.request();
        true
    }
    pub fn response<'a>(&self, fid: u64, fresh: Peer, owner: &'a mut Interaction,
        now: u64) -> Result<EncodedResponse<'a>, Failure> {
        self.peer(fresh)?;
        let f = self.fids[self.index(fid)?].as_ref().unwrap();
        let saved = f.record.reply().ok_or(Failure::Busy)?.as_ref().map_err(|e| *e)?;
        let response = match *saved {
            Saved::Fixed(r) => r,
            Saved::Read { scope, transfer, offset, count } => Response::Read { offset,
                data: owner.read(fresh, scope, transfer, offset as usize, count as usize, now)? },
        };
        response.encoded(f.answered).map_err(|_| Failure::Invalid)
    }
    /// Cancel the exact fid's admission before dropping its replay state. The
    /// shared HIA slot remains occupied until the transport drains its receipt.
    pub fn cancel(&mut self, fid: u64, owner: &mut Interaction) -> Result<(), Failure> {
        let index = self.index(fid)?;
        if let Some(p) = self.pending.filter(|p| p.target.fid == fid) {
            if p.request.op == libhalcyon::interaction_control::Op::Publish {
                owner.disconnect(self.peer.connection);
                self.scope = None;
            } else { owner.cancel_pending(p.target); }
            self.pending = None;
        }
        self.fids[index].as_mut().unwrap().record.cancel();
        Ok(())
    }
    pub fn clunk(&mut self, fid: u64, owner: &mut Interaction) -> Result<(), Failure> {
        self.cancel(fid, owner)?;
        let index = self.index(fid)?;
        self.fids[index] = None;
        Ok(())
    }
    /// Part of the SAK barrier, not its whole: the caller must separately drop
    /// unsent transport output and close any partially written reply frame.
    pub fn retire(&mut self, owner: &mut Interaction) {
        owner.disconnect(self.peer.connection);
        self.pending = None;
        self.scope = None;
        for f in self.fids.iter_mut().flatten() { f.record.cancel(); }
    }
}
enum Action { Ready(Saved), Pending(Admission) }
fn dispatch(session: u64, current: &mut Option<Scope>, busy: bool, route: Route,
    peer: Peer, target: Target, request: Request<'_>, owner: &mut Interaction,
    bindings: &Bindings, desired: &Desired, now: u64, retired: &mut Option<Completed>) -> Result<Action, Failure> {
    let scope = |s: Scope| if Some(s) == *current { Ok(s) } else { Err(Failure::Gone) };
    let ready = |r| Ok(Action::Ready(Saved::Fixed(r)));
    match request {
        Request::Hello => ready(Response::Hello { session }),
        Request::Bind { session: wanted, context, epoch } => {
            if wanted != session { return Err(Failure::Gone); }
            if busy || current.is_some() { return Err(Failure::Busy); }
            owner.publish_on(bindings, desired, route, peer, context, epoch, now).map(Action::Pending)
        }
        Request::Mode { scope: s, sequence, mode, readonly, label } => {
            owner.report(peer, scope(s)?, sequence, mode, readonly, label)?;
            ready(Response::Mode)
        }
        Request::Get { scope: s } => {
            if busy { return Err(Failure::Busy); }
            owner.get(peer, scope(s)?, target, now).map(Action::Pending)
        }
        Request::Begin { scope: s, length } => {
            if busy { return Err(Failure::Busy); }
            owner.begin(peer, scope(s)?, target, length as usize, now).map(Action::Pending)
        }
        Request::Commit { scope: s, transfer, expected } => {
            if busy { return Err(Failure::Busy); }
            owner.commit(peer, scope(s)?, target, transfer, expected, now).map(Action::Pending)
        }
        Request::Read { transfer, offset, count } => {
            let s = current.ok_or(Failure::Gone)?;
            owner.read(peer, s, transfer, offset as usize, count as usize, now)?;
            Ok(Action::Ready(Saved::Read { scope: s, transfer, offset, count }))
        }
        Request::Write { transfer, offset, data } => {
            let count = owner.write(peer, current.ok_or(Failure::Gone)?, transfer, offset as usize, data, now)?;
            ready(Response::Written { count: count as u32 })
        }
        Request::Cancel { transfer } => {
            owner.cancel_transfer(peer, current.ok_or(Failure::Gone)?, transfer)?;
            ready(Response::Cancelled)
        }
        Request::Unbind { scope: s } => {
            *retired = owner.unbind(peer, scope(s)?)?;
            *current = None;
            ready(Response::Unbound)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::{vec, vec::Vec};
    use crate::{controllers::RouteKey, hostbindings::Host, paneroute::Routes};
    use libhalcyon::{interaction_control::{Op, Reply}, interaction_wire::{Mode, Operation}};
    struct Harness { app: Application, owner: Interaction, bindings: Bindings, desired: Desired, peer: Peer, route: Route }
    fn receipt(q: Admission) -> Reply { Reply { op: q.op, request: q.request, seat: 0, foreground: q.foreground.max(5), focus: 1 } }
    impl Harness {
        fn new() -> Self {
            let peer = Peer { connection: 7, stripes: 19, principal: 1000, alive: true };
            let mut routes = Routes::empty(); assert!(routes.insert(123, 1));
            let route = *routes.get(&123).unwrap();
            let mut desired = Desired::empty(); assert!(desired.announce(&routes, Host { route, pid: 99, binding: 9 }));
            let mut bindings = Bindings::new();
            let mut owner = Interaction::new(3, 1000).unwrap(); owner.seat(Some(0));
            let action = bindings.plan(&desired, Some(0)).unwrap();
            let q = owner.control(Op::Bind, RouteKey { leaf: 1, incarnation: route.incarnation }, 99, 9, 0).unwrap();
            assert!(bindings.started(action, q, 0)); bindings.terminal_state(1, 9, 5);
            assert!(bindings.complete(q, Ok(()))); assert!(owner.complete(q, None, Ok(receipt(q)), 0).is_some());
            let mut app = Application::new(3, peer).unwrap(); app.open(1, route).unwrap();
            Self { app, owner, bindings, desired, peer, route }
        }
        fn send(&mut self, fid: u64, id: u64, request: Request<'_>) -> WriteResult {
            self.app.write(fid, self.peer, 0, &request.encode(id).unwrap(), MAX_RECORD,
                &mut self.owner, &self.bindings, &self.desired, 0).unwrap()
        }
        fn finish(&mut self, q: Admission) {
            assert_eq!(self.app.decision(&mut self.owner, q, self.peer, Ok(receipt(q)), 0), Ok(true));
        }
        fn do_request(&mut self, fid: u64, id: u64, request: Request<'_>) -> Vec<u8> {
            if let WriteResult::Pending(q) = self.send(fid, id, request) { self.finish(q); }
            self.reply(fid, 0).unwrap()
        }
        fn reply(&mut self, fid: u64, now: u64) -> Result<Vec<u8>, Failure> {
            let view = self.app.response(fid, self.peer, &mut self.owner, now)?;
            let mut bytes = vec![0; view.len()]; view.copy_range(0, &mut bytes); Ok(bytes)
        }
        fn bind(&mut self) -> Scope {
            let result = self.do_request(1, 1, Request::Bind { session: 3, context: 4, epoch: 5 });
            let scope = self.app.scope().unwrap();
            assert_eq!(Response::decode(&result, Operation::BindController, 1), Ok(Response::Bound { controller: scope.controller }));
            scope
        }
        fn copy(&mut self, start: u64, bytes: &[u8]) -> u64 {
            let scope = self.app.scope().unwrap();
            let answer = self.do_request(1, start, Request::Begin { scope, length: bytes.len() as u32 });
            let Response::Begun { transfer } = Response::decode(&answer, Operation::BeginCopy, start).unwrap() else { panic!() };
            if !bytes.is_empty() { self.do_request(1, start + 1, Request::Write { transfer, offset: 0, data: bytes }); }
            let answer = self.do_request(1, start + 2, Request::Commit { scope, transfer, expected: self.owner.generation() });
            let Response::Committed { generation } = Response::decode(&answer, Operation::CommitCopy, start + 2).unwrap() else { panic!() };
            generation
        }
        fn get(&mut self, fid: u64, id: u64) -> u64 {
            let answer = self.do_request(fid, id, Request::Get { scope: self.app.scope().unwrap() });
            let Response::Clipboard { transfer, .. } = Response::decode(&answer, Operation::GetClipboard, id).unwrap() else { panic!() };
            transfer
        }
    }
    #[test]
    fn framed_copy_commit_replay_and_borrowed_read() {
        let mut h = Harness::new(); let scope = h.bind();
        assert_eq!(h.copy(2, b"clipboard"), 1);
        let old = h.reply(1, 0).unwrap();
        let transfer = h.get(1, 5);
        let before = h.owner.payload_reservation();
        let response = h.do_request(1, 6, Request::Read { transfer, offset: 0, count: 32 });
        assert_eq!(Response::decode(&response, Operation::ReadClipboard, 6), Ok(Response::Read { offset: 0, data: b"clipboard" }));
        assert_eq!(h.owner.payload_reservation(), before);
        assert_eq!(h.app.output_reserved(), 0);
        assert_eq!(h.send(1, 6, Request::Read { transfer, offset: 0, count: 32 }), WriteResult::Answered);
        assert_eq!(h.reply(1, 0).unwrap(), response);
        assert_eq!(h.owner.generation(), 1);
        assert_eq!(Response::decode(&old, Operation::CommitCopy, 4), Ok(Response::Committed { generation: 1 }));
        h.do_request(1, 7, Request::Cancel { transfer });
        h.do_request(1, 8, Request::Mode { scope, sequence: 1, mode: Mode::Normal, readonly: false, label: "shell" });
    }
    #[test]
    fn commit_replay_is_exactly_once_and_changed_bytes_poison() {
        let mut h = Harness::new(); let scope = h.bind();
        let begin = h.do_request(1, 2, Request::Begin { scope, length: 0 });
        let Response::Begun { transfer } = Response::decode(&begin, Operation::BeginCopy, 2).unwrap() else { panic!() };
        let commit = Request::Commit { scope, transfer, expected: 0 };
        let original = h.do_request(1, 3, commit);
        assert_eq!(h.send(1, 3, commit), WriteResult::Answered);
        assert_eq!(h.reply(1, 0).unwrap(), original); assert_eq!(h.owner.generation(), 1);
        assert_eq!(h.app.write(1, h.peer, 0, &Request::Commit { scope, transfer, expected: 1 }.encode(3).unwrap(), MAX_RECORD,
            &mut h.owner, &h.bindings, &h.desired, 0), Err(Failure::Invalid));
        assert_eq!(h.owner.generation(), 1);
    }
    #[test]
    fn every_fragment_boundary_dispatches_only_when_complete() {
        let bytes = Request::Bind { session: 3, context: 4, epoch: 5 }.encode(1).unwrap();
        for split in 1..bytes.len() {
            let mut h = Harness::new();
            assert_eq!(h.app.write(1, h.peer, 0, &bytes[..split], MAX_RECORD, &mut h.owner, &h.bindings, &h.desired, 0), Ok(WriteResult::Partial));
            assert!(!h.owner.busy());
            let q = h.app.write(1, h.peer, split as u64, &bytes[split..], MAX_RECORD, &mut h.owner, &h.bindings, &h.desired, 0).unwrap();
            assert!(matches!(q, WriteResult::Pending(_)));
        }
    }
    #[test]
    fn stale_fresh_peer_cannot_dispatch_or_read_cached_text() {
        let mut h = Harness::new(); h.bind(); h.copy(2, b"private");
        let transfer = h.get(1, 5); h.do_request(1, 6, Request::Read { transfer, offset: 0, count: 7 });
        for peer in [Peer { alive: false, ..h.peer }, Peer { stripes: 20, ..h.peer }, Peer { principal: 1, ..h.peer }, Peer { connection: 8, ..h.peer }] {
            assert_eq!(h.app.response(1, peer, &mut h.owner, 0).err(), Some(Failure::Gone));
            assert_eq!(h.app.write(1, peer, 0, &Request::Hello.encode(7).unwrap(), MAX_RECORD, &mut h.owner, &h.bindings, &h.desired, 0), Err(Failure::Gone));
        }
    }
    #[test]
    fn fresh_peer_loss_at_publish_receipt_refuses_scope() {
        let mut h = Harness::new();
        let WriteResult::Pending(q) = h.send(1, 1, Request::Bind { session: 3, context: 4, epoch: 5 }) else { panic!() };
        let done = h.owner.complete(q, Some(Peer { alive: false, ..h.peer }), Ok(receipt(q)), 0).unwrap();
        assert!(h.app.complete(done)); assert_eq!(h.app.scope(), None);
        assert_eq!(h.reply(1, 0), Err(Failure::Gone));
    }
    #[test]
    fn clunk_cancels_publication_and_late_receipt_cannot_bind_reused_fid() {
        let mut h = Harness::new();
        let WriteResult::Pending(q) = h.send(1, 1, Request::Bind { session: 3, context: 4, epoch: 5 }) else { panic!() };
        h.app.clunk(1, &mut h.owner).unwrap(); assert!(h.owner.busy());
        assert_eq!(h.app.open(1, h.route), Err(Failure::Invalid));
        h.app.open(2, h.route).unwrap();
        let done = h.owner.complete(q, Some(h.peer), Ok(receipt(q)), 0).unwrap();
        assert!(!h.app.complete(done)); assert_eq!(h.app.scope(), None);
        assert_eq!(h.reply(2, 0), Err(Failure::Busy));
        let fresh = h.send(2, 1, Request::Bind { session: 3, context: 4, epoch: 6 });
        assert_eq!(matches!(fresh, WriteResult::Pending(_)), true, "cancelled publication leaked a controller");
    }
    #[test]
    fn second_fid_unbind_completes_the_pending_admission_locally() {
        let mut h = Harness::new(); let scope = h.bind(); h.app.open(2, h.route).unwrap();
        let WriteResult::Pending(q) = h.send(1, 2, Request::Get { scope }) else { panic!() };
        assert_eq!(h.send(2, 1, Request::Unbind { scope }), WriteResult::Answered);
        assert!(h.app.pending_request().is_none());
        assert_eq!(h.reply(1, 0), Err(Failure::Gone));
        assert!(h.owner.busy()); assert!(h.owner.complete(q, Some(h.peer), Ok(receipt(q)), 0).is_none());
    }
    #[test]
    fn seat_retirement_drops_cache_and_partial_request_before_ack() {
        let mut h = Harness::new(); let scope = h.bind(); h.copy(2, b"private");
        let transfer = h.get(1, 5); h.do_request(1, 6, Request::Read { transfer, offset: 0, count: 7 });
        h.app.open(2, h.route).unwrap();
        let bytes = Request::Mode { scope, sequence: 1, mode: Mode::Insert, readonly: false, label: "partial" }.encode(1).unwrap();
        h.app.write(2, h.peer, 0, &bytes[..30], MAX_RECORD, &mut h.owner, &h.bindings, &h.desired, 0).unwrap();
        h.owner.seat(None); h.app.retire(&mut h.owner);
        assert_eq!(h.reply(1, 0), Err(Failure::Busy));
        assert_eq!(h.app.input_reserved(), 96); assert_eq!(h.app.scope(), None);
        h.owner.seat(Some(2)); assert_eq!(h.reply(1, 0), Err(Failure::Busy));
        assert!(h.app.write(2, h.peer, 30, &bytes[30..], MAX_RECORD, &mut h.owner, &h.bindings, &h.desired, 0).is_err());
    }
    #[test]
    fn cached_snapshot_revalidates_expiry_cancel_and_owner_loss() {
        for cause in 0..3 {
            let mut h = Harness::new(); let scope = h.bind(); h.copy(2, b"private");
            let transfer = h.get(1, 5); h.do_request(1, 6, Request::Read { transfer, offset: 0, count: 7 });
            match cause { 0 => { h.owner.expire(30_000); }, 1 => { h.owner.cancel_transfer(h.peer, scope, transfer).unwrap(); }, _ => { h.owner.disconnect(h.peer.connection); } }
            assert!(h.reply(1, 30_000).is_err());
        }
    }
    #[test]
    fn aggregate_fid_input_uses_transport_remainder_and_never_per_fid_quota() {
        let mut h = Harness::new();
        for fid in 2..=8 { h.app.open(fid, h.route).unwrap(); }
        assert_eq!(h.app.open(9, h.route), Err(Failure::Busy));
        let remaining = h.app.input_reserved();
        let bytes = Request::Bind { session: 3, context: 4, epoch: 5 }.encode(1).unwrap();
        assert_eq!(h.app.write(1, h.peer, 0, &bytes, remaining, &mut h.owner, &h.bindings, &h.desired, 0), Err(Failure::TooLarge));
        assert!(!h.owner.busy()); assert_eq!(h.app.output_reserved(), 0);
    }
    #[test]
    fn second_client_snapshot_stays_immutable_across_another_commit() {
        let mut h = Harness::new(); h.bind(); h.copy(2, b"first");
        let peer2 = Peer { connection: 8, stripes: 20, ..h.peer };
        let mut routes = Routes::empty(); assert!(routes.insert(123, 1)); assert!(routes.insert(456, 2));
        let route2 = *routes.get(&456).unwrap();
        assert!(h.desired.announce(&routes, Host { route: route2, pid: 100, binding: 10 }));
        let action = h.bindings.plan(&h.desired, Some(0)).unwrap();
        let q = h.owner.control(Op::Bind, RouteKey { leaf: 2, incarnation: route2.incarnation }, 100, 10, 0).unwrap();
        assert!(h.bindings.started(action, q, 0)); h.bindings.terminal_state(2, 10, 5);
        assert!(h.bindings.complete(q, Ok(()))); assert!(h.owner.complete(q, None, Ok(receipt(q)), 0).is_some());
        let mut other = Application::new(3, peer2).unwrap(); other.open(1, route2).unwrap();
        core::mem::swap(&mut h.app, &mut other); h.peer = h.app.peer;
        h.bind(); let transfer = h.get(1, 2);
        let bytes = h.do_request(1, 3, Request::Read { transfer, offset: 0, count: 10 });
        assert_eq!(Response::decode(&bytes, Operation::ReadClipboard, 3), Ok(Response::Read { offset: 0, data: b"first" }));
        core::mem::swap(&mut h.app, &mut other); h.peer = h.app.peer;
        h.copy(5, b"second");
        core::mem::swap(&mut h.app, &mut other); h.peer = h.app.peer;
        assert_eq!(h.reply(1, 0).unwrap(), bytes);
        // Cached read references the old admitted slot, not the mutable current.
        assert_eq!(h.app.output_reserved(), 0);
        h.do_request(1, 4, Request::Cancel { transfer });
        let next = h.get(1, 5);
        let bytes = h.do_request(1, 6, Request::Read { transfer: next, offset: 0, count: 10 });
        assert_eq!(Response::decode(&bytes, Operation::ReadClipboard, 6), Ok(Response::Read { offset: 0, data: b"second" }));
    }
    #[test]
    fn wrong_scope_and_session_do_not_reach_admission() {
        let mut h = Harness::new();
        assert_eq!(h.send(1, 1, Request::Bind { session: 99, context: 1, epoch: 1 }), WriteResult::Answered);
        assert_eq!(h.reply(1, 0), Err(Failure::Gone)); assert!(!h.owner.busy());
        h.do_request(1, 2, Request::Bind { session: 3, context: 1, epoch: 1 });
        let scope = h.app.scope().unwrap();
        for (id, bad) in [Scope { session: 4, ..scope }, Scope { controller: scope.controller + 1, ..scope }, Scope { context: 2, ..scope }, Scope { epoch: 2, ..scope }].into_iter().enumerate() {
            assert_eq!(h.send(1, 3 + id as u64, Request::Begin { scope: bad, length: 4 }), WriteResult::Answered);
            assert_eq!(h.reply(1, 0), Err(Failure::Gone)); assert!(!h.owner.busy());
        }
    }
    #[test]
    fn peer_exit_before_check_completion_cannot_publish() {
        let mut h = Harness::new(); let scope = h.bind();
        let bytes = h.do_request(1, 2, Request::Begin { scope, length: 0 });
        let Response::Begun { transfer } = Response::decode(&bytes, Operation::BeginCopy, 2).unwrap() else { panic!() };
        let WriteResult::Pending(q) = h.send(1, 3, Request::Commit { scope, transfer, expected: 0 }) else { panic!() };
        assert_eq!(h.app.decision(&mut h.owner, q, Peer { alive: false, ..h.peer }, Ok(receipt(q)), 0), Err(Failure::Gone));
        assert_eq!(h.owner.generation(), 0);
        assert_eq!(h.app.scope(), None); assert!(!h.owner.busy());
        assert_eq!(h.reply(1, 0), Err(Failure::Busy));
    }
    #[test]
    fn wrong_completion_cannot_consume_pending_target() {
        let mut h = Harness::new(); let scope = h.bind();
        let WriteResult::Pending(q) = h.send(1, 2, Request::Begin { scope, length: 0 }) else { panic!() };
        assert!(!h.app.complete(Completion::Clipboard(Completed { target: Target { connection: 8, fid: 1, request: 2 }, result: Ok(Outcome::Begun(100)) })));
        assert_eq!(h.app.pending_request(), Some(q));
        h.finish(q); assert!(h.reply(1, 0).is_ok());
    }
    #[test]
    fn controller_route_remains_pinned_after_all_fids_clunk() {
        let mut h = Harness::new(); h.bind(); h.app.clunk(1, &mut h.owner).unwrap();
        assert_eq!(h.app.open(2, Route { incarnation: 2, ..h.route }), Err(Failure::Denied));
    }
}
