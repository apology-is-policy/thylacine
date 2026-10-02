//! Executor-owned terminal controller lifetimes; not a wire authentication API.
//!
//! Routes/context come from the terminal host adapter and Peer from fresh kernel
//! connection metadata. A HIN1 scope or pane token alone cannot create either.
//! Publish completion is provisional until its exact receipt and live peer are
//! checked. Retirement cancels the exact broker Owner through a synchronous
//! callback; the callback must also retire application output before a SAK ACK.
//! No I/O, locks, heap allocation, focus inference or kernel roles live here.
use crate::{clipboard::Owner, clipbroker::Authority};
use libhalcyon::{
    interaction_body::{Scope, MAX_CONTROLLERS},
    interaction_control::{Op, Reply, Request},
    interaction_wire::{Failure, Mode, MAX_LABEL},
};

/// Sampled from the actual accepted connection; never from an application body.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Peer {
    pub connection: u64,
    pub stripes: u64,
    pub principal: u32,
    pub alive: bool,
}
/// Local route incarnation, not a reusable leaf number or an authority token.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct RouteKey {
    pub leaf: u32,
    pub incarnation: u64,
}
/// Published by the authenticated host adapter after the HIA binding setup.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Terminal {
    pub route: RouteKey,
    pub binding: u64,
    pub foreground: u64,
    pub context: u64,
    pub epoch: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ModeReport {
    pub mode: Mode,
    pub readonly: bool,
    pub sequence: u64,
    label: [u8; MAX_LABEL],
    length: u8,
}
impl ModeReport {
    pub fn label(&self) -> &str {
        // Only report() constructs this after validating a complete UTF-8 str.
        core::str::from_utf8(&self.label[..self.length as usize]).unwrap()
    }
}
#[derive(Clone, Copy)]
struct Entry {
    terminal: Terminal,
    peer: Peer,
    authority: Authority,
    pending: Option<Request>,
    report: Option<ModeReport>,
}
pub struct Controllers {
    session: u64,
    principal: u32,
    normal: Option<u64>,
    next: u64,
    last_publish: u64,
    entries: [Option<Entry>; MAX_CONTROLLERS as usize],
}
const _: () = assert!(core::mem::size_of::<Controllers>() <= 16 * 1024);

impl Controllers {
    pub fn new(session: u64, principal: u32) -> Result<Self, Failure> {
        if session == 0 {
            return Err(Failure::Invalid);
        }
        Ok(Self {
            session,
            principal,
            normal: None,
            next: 1,
            last_publish: 0,
            entries: [None; MAX_CONTROLLERS as usize],
        })
    }
    fn peer_valid(&self, p: Peer) -> bool {
        p.alive && p.connection != 0 && p.stripes != 0 && p.principal == self.principal
    }
    /// Generate Publish, not authority. `request` must come from the SINGLE
    /// channel sequencer shared with Bind/Check/Unbind. Only one publication may
    /// be pending; the transport additionally serializes all kinds of exchange.
    pub fn prepare(&mut self, t: Terminal, p: Peer, request: u64) -> Result<Request, Failure> {
        if self.normal.is_none() || !self.peer_valid(p) {
            return Err(Failure::Denied);
        }
        if t.route.leaf == 0
            || t.route.incarnation == 0
            || [t.binding, t.foreground, t.context, t.epoch, request].contains(&0)
            || t.binding > i64::MAX as u64
            || request <= self.last_publish
        {
            return Err(Failure::Invalid);
        }
        if self.entries.iter().flatten().any(|e| {
            e.pending.is_some()
                || e.terminal.route.leaf == t.route.leaf
                || e.terminal.binding == t.binding
        }) {
            return Err(Failure::Busy);
        }
        let slot = self
            .entries
            .iter()
            .position(Option::is_none)
            .ok_or(Failure::Busy)?;
        let next = self.next.checked_add(1).ok_or(Failure::Busy)?;
        let scope = Scope {
            session: self.session,
            controller: self.next,
            context: t.context,
            epoch: t.epoch,
        };
        let authority = Authority {
            owner: Owner {
                connection: p.connection,
                scope,
            },
            leaf: t.route.leaf,
            binding: t.binding,
            foreground: t.foreground,
            subject: p.stripes,
        };
        let publish = Request {
            op: Op::Publish,
            request,
            leaf: t.route.leaf,
            binder_pid: 0,
            binding: t.binding,
            foreground: t.foreground,
            subject: p.stripes,
            controller: scope.controller,
            context: scope.context,
            epoch: scope.epoch,
        };
        self.next = next;
        self.last_publish = request;
        self.entries[slot] = Some(Entry {
            terminal: t,
            peer: p,
            authority,
            pending: Some(publish),
            report: None,
        });
        Ok(publish)
    }
    /// An exact refused or invalid completion retires only its own provisional
    /// entry. A stale/wrong request cannot consume a different pending entry.
    pub fn finish(
        &mut self,
        request: Request,
        fresh: Peer,
        result: Result<Reply, Failure>,
        mut retire: impl FnMut(Owner),
    ) -> Result<Scope, Failure> {
        let slot = self
            .entries
            .iter()
            .position(|e| e.is_some_and(|e| e.pending == Some(request)))
            .ok_or(Failure::Gone)?;
        let e = self.entries[slot].unwrap();
        let valid = match result {
            Ok(r)
                if r.op == Op::Publish
                    && r.request == request.request
                    && r.foreground == request.foreground
                    && r.focus != 0
                    && r.focus != u64::MAX
                    && self.normal == Some(r.seat)
                    && self.peer_valid(fresh)
                    && fresh == e.peer =>
            {
                Ok(())
            }
            Ok(_) => Err(Failure::Gone),
            Err(f) => Err(f),
        };
        if let Err(f) = valid {
            self.remove(slot, &mut retire);
            return Err(f);
        }
        self.entries[slot].as_mut().unwrap().pending = None;
        Ok(e.authority.owner.scope)
    }
    /// Registration supplies scope, never focus admission. The Broker still
    /// issues a fresh ordered CHECK for operations that require keyboard focus.
    pub fn authority(&self, fresh: Peer, scope: Scope) -> Result<Authority, Failure> {
        if self.normal.is_none() || !self.peer_valid(fresh) {
            return Err(Failure::Denied);
        }
        self.entries
            .iter()
            .flatten()
            .find(|e| e.pending.is_none() && e.peer == fresh && e.authority.owner.scope == scope)
            .map(|e| e.authority)
            .ok_or(Failure::Gone)
    }
    pub fn report(
        &mut self,
        fresh: Peer,
        scope: Scope,
        sequence: u64,
        mode: Mode,
        readonly: bool,
        label: &str,
    ) -> Result<(), Failure> {
        let authority = self.authority(fresh, scope)?;
        if label.len() > MAX_LABEL {
            return Err(Failure::TooLarge);
        }
        if sequence == 0 || label.chars().any(char::is_control) {
            return Err(Failure::Invalid);
        }
        let e = self
            .entries
            .iter_mut()
            .flatten()
            .find(|e| e.authority == authority)
            .unwrap();
        if e.report.is_some_and(|r| sequence <= r.sequence) {
            return Err(Failure::Conflict);
        }
        let mut bytes = [0; MAX_LABEL];
        bytes[..label.len()].copy_from_slice(label.as_bytes());
        e.report = Some(ModeReport {
            mode,
            readonly,
            sequence,
            label: bytes,
            length: label.len() as u8,
        });
        Ok(())
    }
    /// Caller passes the authoritative focused route, never a peer's claimed
    /// focus. None means no report (APP or local transcript policy at the UI).
    pub fn mode(&self, focused: RouteKey) -> Option<ModeReport> {
        if self.normal.is_none() {
            return None;
        }
        self.entries
            .iter()
            .flatten()
            .find(|e| e.pending.is_none() && e.terminal.route == focused)
            .and_then(|e| e.report)
    }
    /// Only for the owner's trusted ordered notifications; never a wire query.
    pub(crate) fn on_route(&self, route: RouteKey, mut visit: impl FnMut(Authority)) {
        for e in self.entries.iter().flatten() {
            if e.pending.is_none() && e.terminal.route == route {
                visit(e.authority);
            }
        }
    }
    fn remove(&mut self, slot: usize, retire: &mut impl FnMut(Owner)) {
        if let Some(e) = self.entries[slot].take() {
            retire(e.authority.owner);
        }
    }
    pub fn route_gone(&mut self, route: RouteKey, mut retire: impl FnMut(Owner)) {
        for i in 0..self.entries.len() {
            if self.entries[i].is_some_and(|e| e.terminal.route == route) {
                self.remove(i, &mut retire);
            }
        }
    }
    /// Ordered terminal notification: even an A -> B -> A handover carries a
    /// new foreground epoch. A delayed old route notification cannot touch its
    /// replacement. ACK can also replace the subject without changing that
    /// epoch. Pass subject zero for an unacknowledged/dead nomination. The
    /// adapter must deliver complete snapshots in source order.
    pub fn terminal_state(
        &mut self,
        route: RouteKey,
        epoch: u64,
        subject: u64,
        mut retire: impl FnMut(Owner),
    ) {
        for i in 0..self.entries.len() {
            if self.entries[i].is_some_and(|e| {
                e.terminal.route == route
                    && (e.terminal.foreground != epoch || e.peer.stripes != subject)
            }) {
                self.remove(i, &mut retire);
            }
        }
    }
    pub fn disconnect(&mut self, connection: u64, mut retire: impl FnMut(Owner)) {
        for i in 0..self.entries.len() {
            if self.entries[i].is_some_and(|e| e.peer.connection == connection) {
                self.remove(i, &mut retire);
            }
        }
    }
    pub fn unbind(
        &mut self,
        fresh: Peer,
        scope: Scope,
        mut retire: impl FnMut(Owner),
    ) -> Result<(), Failure> {
        let authority = self.authority(fresh, scope)?;
        let i = self
            .entries
            .iter()
            .position(|e| e.is_some_and(|e| e.authority == authority))
            .unwrap();
        self.remove(i, &mut retire);
        Ok(())
    }
    pub fn seat(&mut self, normal: Option<u64>, mut retire: impl FnMut(Owner)) {
        let normal = normal.filter(|n| *n != 0);
        if self.normal == normal {
            return;
        }
        self.normal = normal;
        for i in 0..self.entries.len() {
            self.remove(i, &mut retire);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::{vec, vec::Vec};
    fn peer() -> Peer {
        Peer {
            connection: 7,
            stripes: 17,
            principal: 1000,
            alive: true,
        }
    }
    fn terminal(n: u64) -> Terminal {
        Terminal {
            route: RouteKey {
                leaf: n as u32,
                incarnation: n,
            },
            binding: n,
            foreground: 1,
            context: n,
            epoch: 1,
        }
    }
    fn table() -> Controllers {
        let mut c = Controllers::new(9, 1000).unwrap();
        c.seat(Some(3), |_| {});
        c
    }
    fn reply(r: Request) -> Reply {
        Reply {
            op: r.op,
            request: r.request,
            focus: 1,
            seat: 3,
            foreground: r.foreground,
        }
    }
    fn bind(c: &mut Controllers, n: u64) -> Scope {
        let r = c.prepare(terminal(n), peer(), n).unwrap();
        c.finish(r, peer(), Ok(reply(r)), |_| {}).unwrap()
    }
    #[test]
    fn pending_is_not_a_controller_and_receipt_is_single_use() {
        let mut c = table();
        let r = c.prepare(terminal(1), peer(), 1).unwrap();
        let s = Scope {
            session: 9,
            controller: r.controller,
            context: r.context,
            epoch: r.epoch,
        };
        assert_eq!(c.authority(peer(), s), Err(Failure::Gone));
        assert_eq!(c.prepare(terminal(2), peer(), 2), Err(Failure::Busy));
        let mut wrong = r;
        wrong.request += 1;
        assert_eq!(
            c.finish(wrong, peer(), Ok(reply(wrong)), |_| panic!(
                "wrong receipt retired owner"
            )),
            Err(Failure::Gone)
        );
        assert_eq!(c.finish(r, peer(), Ok(reply(r)), |_| {}), Ok(s));
        assert!(c.authority(peer(), s).is_ok());
        assert_eq!(
            c.finish(r, peer(), Ok(reply(r)), |_| {}),
            Err(Failure::Gone)
        );
    }
    #[test]
    fn receipt_checks_every_authorizing_field_and_fresh_peer() {
        for field in 0..10 {
            let mut c = table();
            let r = c.prepare(terminal(1), peer(), 1).unwrap();
            let mut q = reply(r);
            let mut p = peer();
            match field {
                0 => q.request += 1,
                1 => q.op = Op::Check,
                2 => q.foreground += 1,
                3 => q.seat += 1,
                4 => q.focus = 0,
                5 => q.focus = u64::MAX,
                6 => p.alive = false,
                7 => p.stripes += 1,
                8 => p.connection += 1,
                _ => p.principal += 1,
            }
            let mut retired = Vec::new();
            assert_eq!(
                c.finish(r, p, Ok(q), |o| retired.push(o)),
                Err(Failure::Gone),
                "field {field}"
            );
            assert_eq!(retired.len(), 1);
            let newer = c.prepare(terminal(1), peer(), 2).unwrap();
            assert!(newer.controller > r.controller);
        }
    }
    #[test]
    fn exact_principal_connection_stripes_and_scope_are_required() {
        let mut c = table();
        let s = bind(&mut c, 1);
        for field in 0..8 {
            let mut p = peer();
            let mut s = s;
            match field {
                0 => p.alive = false,
                1 => p.principal += 1,
                2 => p.stripes += 1,
                3 => p.connection += 1,
                4 => s.session += 1,
                5 => s.controller += 1,
                6 => s.context += 1,
                _ => s.epoch += 1,
            }
            assert!(c.authority(p, s).is_err(), "field {field}");
        }
        assert!(c.authority(peer(), s).is_ok());
        let mut p = peer();
        p.principal += 1;
        assert_eq!(c.prepare(terminal(2), p, 2), Err(Failure::Denied));
    }
    #[test]
    fn replacement_and_foreground_round_trip_never_revive_an_owner() {
        let mut c = table();
        let s = bind(&mut c, 1);
        let mut retired = Vec::new();
        c.terminal_state(terminal(1).route, 2, peer().stripes, |o| retired.push(o));
        assert_eq!(retired[0].scope, s);
        c.terminal_state(terminal(1).route, 1, peer().stripes, |_| {});
        assert!(c.authority(peer(), s).is_err());
        let mut t = terminal(1);
        t.route.incarnation = 2;
        let r = c.prepare(t, peer(), 2).unwrap();
        let s2 = c.finish(r, peer(), Ok(reply(r)), |_| {}).unwrap();
        c.route_gone(terminal(1).route, |_| {
            panic!("old route retired replacement")
        });
        assert!(c.authority(peer(), s2).is_ok());
        assert!(s2.controller > s.controller);
    }
    #[test]
    fn disconnect_cancels_pending_and_active_without_touching_other_peer() {
        let mut c = table();
        let s = bind(&mut c, 1);
        let mut p = peer();
        p.connection = 8;
        let r = c.prepare(terminal(2), p, 2).unwrap();
        let mut gone = Vec::new();
        c.disconnect(7, |o| gone.push(o));
        assert_eq!(gone.len(), 1);
        assert_eq!(gone[0].scope, s);
        c.disconnect(8, |o| gone.push(o));
        assert_eq!(gone.len(), 2);
        assert_eq!(c.finish(r, p, Ok(reply(r)), |_| {}), Err(Failure::Gone));
    }
    #[test]
    fn seat_restoration_requires_new_publication() {
        let mut c = table();
        let s = bind(&mut c, 1);
        let mut gone = Vec::new();
        c.seat(None, |o| gone.push(o));
        assert_eq!(gone.len(), 1);
        assert_eq!(c.prepare(terminal(1), peer(), 2), Err(Failure::Denied));
        c.seat(Some(4), |_| {});
        assert!(c.authority(peer(), s).is_err());
        let r = c.prepare(terminal(1), peer(), 2).unwrap();
        assert_eq!(
            c.finish(r, peer(), Ok(reply(r)), |_| {}),
            Err(Failure::Gone)
        );
        let r = c.prepare(terminal(1), peer(), 3).unwrap();
        let mut q = reply(r);
        q.seat = 4;
        assert!(c.finish(r, peer(), Ok(q), |_| {}).is_ok());
        assert!(r.controller > s.controller);
    }
    #[test]
    fn mode_reports_are_ordered_bounded_and_selected_by_exact_route() {
        let mut c = table();
        let s = bind(&mut c, 1);
        let s2 = bind(&mut c, 2);
        assert_eq!(c.mode(terminal(1).route), None);
        c.report(peer(), s, 2, Mode::Visual, true, "Nóra").unwrap();
        c.report(peer(), s2, 1, Mode::Insert, false, "Address")
            .unwrap();
        assert_eq!(c.mode(terminal(1).route).unwrap().label(), "Nóra");
        assert_eq!(c.mode(terminal(2).route).unwrap().mode, Mode::Insert);
        assert_eq!(
            c.mode(RouteKey {
                leaf: 1,
                incarnation: 3
            }),
            None
        );
        assert_eq!(
            c.report(peer(), s, 1, Mode::Insert, false, "late"),
            Err(Failure::Conflict)
        );
        assert_eq!(
            c.report(peer(), s, 2, Mode::Insert, false, "duplicate"),
            Err(Failure::Conflict)
        );
        assert_eq!(
            c.report(peer(), s, 3, Mode::Insert, false, "bad\nlabel"),
            Err(Failure::Invalid)
        );
        assert_eq!(
            c.report(peer(), s, 3, Mode::Insert, false, &"x".repeat(65)),
            Err(Failure::TooLarge)
        );
        assert_eq!(c.mode(terminal(1).route).unwrap().mode, Mode::Visual);
        c.report(peer(), s, 3, Mode::Normal, false, &"é".repeat(32))
            .unwrap();
        assert_eq!(c.mode(terminal(1).route).unwrap().label().len(), 64);
    }
    #[test]
    fn quotas_duplicates_and_counters_fail_without_evicting_live_entries() {
        let mut c = table();
        let s = bind(&mut c, 1);
        let mut duplicate = terminal(2);
        duplicate.binding = 1;
        assert_eq!(c.prepare(duplicate, peer(), 2), Err(Failure::Busy));
        c.next = u64::MAX;
        assert_eq!(c.prepare(terminal(2), peer(), 2), Err(Failure::Busy));
        assert!(c.authority(peer(), s).is_ok());
        let mut c = table();
        for n in 1..=MAX_CONTROLLERS as u64 {
            bind(&mut c, n);
        }
        assert_eq!(c.prepare(terminal(33), peer(), 33), Err(Failure::Busy));
        c.unbind(
            peer(),
            Scope {
                session: 9,
                controller: 1,
                context: 1,
                epoch: 1,
            },
            |_| {},
        )
        .unwrap();
        let r = c.prepare(terminal(33), peer(), 33).unwrap();
        assert_eq!(r.controller, 33);
    }
    #[test]
    fn retirement_callback_cancels_real_broker_work() {
        use crate::clipbroker::{Broker, Target};
        let mut c = table();
        let s = bind(&mut c, 1);
        let a = c.authority(peer(), s).unwrap();
        let mut b = Broker::new(9).unwrap();
        b.seat(Some(3));
        let q = b
            .begin(
                a,
                Target {
                    connection: 7,
                    fid: 1,
                    request: 1,
                },
                3,
                0,
            )
            .unwrap();
        let mut cancelled = vec![];
        c.terminal_state(terminal(1).route, 2, peer().stripes, |o| {
            cancelled.push(b.drop_owner(o))
        });
        assert_eq!(cancelled.len(), 1, "retirement callback missing");
        assert!(cancelled[0].is_some());
        assert!(b.complete(q.request, Ok(reply(q)), 1).is_none());
    }
    #[test]
    fn same_epoch_changed_or_missing_nomination_retires_the_controller() {
        for subject in [0, peer().stripes + 1] {
            let mut c = table();
            let s = bind(&mut c, 1);
            let mut gone = Vec::new();
            c.terminal_state(terminal(1).route, 1, subject, |o| gone.push(o));
            assert_eq!(gone.len(), 1, "changed nomination retained controller");
            assert_eq!(gone[0].scope, s);
            assert!(c.authority(peer(), s).is_err());
        }
    }
    #[test]
    fn failed_publication_and_old_request_cannot_reuse_a_generation() {
        let mut c = table();
        let r = c.prepare(terminal(1), peer(), 5).unwrap();
        let mut gone = Vec::new();
        assert_eq!(
            c.finish(r, peer(), Err(Failure::Denied), |o| gone.push(o)),
            Err(Failure::Denied)
        );
        assert_eq!(gone.len(), 1);
        assert_eq!(c.prepare(terminal(1), peer(), 5), Err(Failure::Invalid));
        let r2 = c.prepare(terminal(1), peer(), 6).unwrap();
        assert!(r2.controller > r.controller);
        c.seat(None, |_| {});
        c.seat(Some(3), |_| {});
        assert_eq!(
            c.finish(r2, peer(), Ok(reply(r2)), |_| {}),
            Err(Failure::Gone)
        );
    }
}
