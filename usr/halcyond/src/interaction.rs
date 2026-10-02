//! Single service owner for registration, clipboard decisions and cancellation.
//!
//! One HIA request sequence, one transport slot, and no second authority table.
//! Trusted event delivery precedes completion dispatch in each executor pass.
//! Local cancellation never frees an in-flight slot: only its exact completion
//! does. Transport storage still follows Channel's separate CQE/drop discipline.
//! No kernel peer sampling or application wire dispatch is performed here.
use crate::{
    clipboard::Owner,
    clipbroker::{Broker, Completed, Target},
    controllers::{Controllers, ModeReport, Peer, RouteKey, Terminal},
};
use libhalcyon::{
    interaction_body::Scope,
    interaction_control::{Op, Reply, Request},
    interaction_wire::{Failure, Mode},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    Control,
    Publish,
    Clipboard,
}
#[derive(Clone, Copy)]
struct Flight {
    request: Request,
    route: Option<RouteKey>,
    kind: Kind,
    usable: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Completion {
    Control {
        request: Request,
        result: Result<Reply, Failure>,
    },
    Published {
        request: Request,
        result: Result<Scope, Failure>,
    },
    Clipboard(Completed),
}
pub struct Interaction {
    controllers: Controllers,
    broker: Broker,
    normal: Option<u64>,
    flight: Option<Flight>,
}
const _: () = assert!(core::mem::size_of::<Interaction>() <= 18 * 1024);

impl Interaction {
    pub fn new(session: u64, principal: u32) -> Result<Self, Failure> {
        Ok(Self {
            controllers: Controllers::new(session, principal)?,
            broker: Broker::new(session)?,
            normal: None,
            flight: None,
        })
    }
    fn idle(&self) -> Result<(), Failure> {
        if self.flight.is_some() {
            Err(Failure::Busy)
        } else if self.normal.is_none() {
            Err(Failure::Denied)
        } else {
            Ok(())
        }
    }
    fn start(&mut self, request: Request, route: Option<RouteKey>, kind: Kind) -> Request {
        self.flight = Some(Flight {
            request,
            route,
            kind,
            usable: true,
        });
        request
    }
    pub fn busy(&self) -> bool {
        self.flight.is_some()
    }
    /// Internal authenticated host setup, never application-selected authority.
    pub fn control(
        &mut self,
        op: Op,
        route: RouteKey,
        binder_pid: u32,
        binding: u64,
    ) -> Result<Request, Failure> {
        self.idle()?;
        if route.incarnation == 0 || !matches!(op, Op::Bind | Op::Unbind) {
            return Err(Failure::Invalid);
        }
        let r = Request {
            op,
            request: self.broker.control_id()?,
            leaf: route.leaf,
            binder_pid,
            binding,
            foreground: 0,
            subject: 0,
            controller: 0,
            context: 0,
            epoch: 0,
        };
        if Request::decode(&r.encode()) != Some(r) {
            return Err(Failure::Invalid);
        }
        if op == Op::Unbind {
            // Local invalidation precedes transport; a failed unbind cannot
            // restore transfers for a terminal the host has already retired.
            self.route_gone(route);
        }
        Ok(self.start(r, Some(route), Kind::Control))
    }
    pub fn publish(&mut self, terminal: Terminal, peer: Peer) -> Result<Request, Failure> {
        self.idle()?;
        let id = self.broker.control_id()?;
        let r = self.controllers.prepare(terminal, peer, id)?;
        Ok(self.start(r, Some(terminal.route), Kind::Publish))
    }
    pub fn begin(
        &mut self,
        peer: Peer,
        scope: Scope,
        target: Target,
        length: usize,
        now: u64,
    ) -> Result<Request, Failure> {
        self.idle()?;
        let a = self.controllers.authority(peer, scope)?;
        let r = self.broker.begin(a, target, length, now)?;
        Ok(self.start(r, None, Kind::Clipboard))
    }
    pub fn get(
        &mut self,
        peer: Peer,
        scope: Scope,
        target: Target,
        now: u64,
    ) -> Result<Request, Failure> {
        self.idle()?;
        let a = self.controllers.authority(peer, scope)?;
        let r = self.broker.get(a, target, now)?;
        Ok(self.start(r, None, Kind::Clipboard))
    }
    pub fn commit(
        &mut self,
        peer: Peer,
        scope: Scope,
        target: Target,
        transfer: u64,
        expected: u64,
        now: u64,
    ) -> Result<Request, Failure> {
        self.idle()?;
        let a = self.controllers.authority(peer, scope)?;
        let r = self.broker.commit(a, target, transfer, expected, now)?;
        Ok(self.start(r, None, Kind::Clipboard))
    }
    /// Apply all observed invalidations before calling. `fresh` is required for
    /// publication; it is freshly sampled kernel metadata, not a cached Hello.
    pub fn complete(
        &mut self,
        request: Request,
        fresh: Option<Peer>,
        result: Result<Reply, Failure>,
        now: u64,
    ) -> Option<Completion> {
        let f = self.flight?;
        if f.request != request {
            return None;
        }
        self.flight = None;
        let result = if f.usable { result } else { Err(Failure::Gone) };
        match f.kind {
            Kind::Control => {
                let result = result.and_then(|r| {
                    if r.op != request.op
                        || r.request != request.request
                        || self.normal != Some(r.seat)
                        || r.focus == 0
                        || r.focus == u64::MAX
                        || (request.op == Op::Bind && r.foreground == 0)
                        || (request.op == Op::Unbind && r.foreground != 0)
                    {
                        Err(Failure::Gone)
                    } else {
                        Ok(r)
                    }
                });
                Some(Completion::Control { request, result })
            }
            Kind::Publish => {
                let p = fresh.unwrap_or(Peer {
                    connection: 0,
                    stripes: 0,
                    principal: 0,
                    alive: false,
                });
                let b = &mut self.broker;
                let result = self.controllers.finish(request, p, result, |owner| {
                    b.drop_owner(owner);
                });
                Some(Completion::Published { request, result })
            }
            Kind::Clipboard => self
                .broker
                .complete(request.request, result, now)
                .map(Completion::Clipboard),
        }
    }
    /// Called only with records from the authenticated ordered compositor stream.
    pub fn observe(
        &mut self,
        body: libhalcyon::interaction_events::Body,
    ) -> Result<Option<Completed>, Failure> {
        use libhalcyon::interaction_events::Body;
        let (leaf, binding) = match body {
            Body::FocusLost { leaf, binding, .. }
            | Body::Terminal { leaf, binding, .. }
            | Body::Retired { leaf, binding } => (leaf, binding),
            Body::Reset => {
                let normal = self.normal;
                let done = self.seat(None);
                self.seat(normal);
                return Ok(done);
            }
            _ => return Err(Failure::Invalid),
        };
        let route = self
            .controllers
            .route_for_binding(leaf, binding)
            .or_else(|| {
                self.flight
                    .filter(|f| f.request.leaf == leaf && f.request.binding == binding)
                    .and_then(|f| f.route)
            });
        let Some(route) = route else {
            return Ok(None);
        };
        Ok(match body {
            Body::FocusLost { epoch, .. } => self.focus_lost(route, epoch),
            Body::Terminal {
                foreground,
                subject,
                ..
            } => self.terminal_state(route, foreground, subject),
            Body::Retired { .. } => self.route_gone(route),
            _ => unreachable!(),
        })
    }
    pub fn report(
        &mut self,
        peer: Peer,
        scope: Scope,
        sequence: u64,
        mode: Mode,
        readonly: bool,
        label: &str,
    ) -> Result<(), Failure> {
        self.controllers
            .report(peer, scope, sequence, mode, readonly, label)
    }
    pub fn mode(&self, route: RouteKey) -> Option<ModeReport> {
        self.controllers.mode(route)
    }
    pub fn write(
        &mut self,
        peer: Peer,
        scope: Scope,
        transfer: u64,
        offset: usize,
        bytes: &[u8],
        now: u64,
    ) -> Result<usize, Failure> {
        let a = self.controllers.authority(peer, scope)?;
        self.broker.write(a.owner, transfer, offset, bytes, now)
    }
    pub fn read(
        &mut self,
        peer: Peer,
        scope: Scope,
        transfer: u64,
        offset: usize,
        count: usize,
        now: u64,
    ) -> Result<&[u8], Failure> {
        let a = self.controllers.authority(peer, scope)?;
        self.broker.read(a.owner, transfer, offset, count, now)
    }
    pub fn cancel_transfer(
        &mut self,
        peer: Peer,
        scope: Scope,
        transfer: u64,
    ) -> Result<bool, Failure> {
        let a = self.controllers.authority(peer, scope)?;
        self.broker.cancel_transfer(a.owner, transfer)
    }
    pub fn cancel_pending(&mut self, target: Target) -> Option<Completed> {
        self.broker.cancel_pending(target)
    }
    pub fn expire(&mut self, now: u64) -> Option<Completed> {
        self.broker.expire(now)
    }
    pub fn deadline(&self) -> Option<u64> {
        self.broker.deadline()
    }
    pub fn generation(&self) -> u64 {
        self.broker.generation()
    }
    pub fn payload_reservation(&self) -> usize {
        self.broker.payload_reservation()
    }

    fn retire(b: &mut Broker, flight: &mut Option<Flight>, owner: Owner) -> Option<Completed> {
        if let Some(f) = flight.as_mut() {
            if f.request.controller == owner.scope.controller {
                f.usable = false;
            }
        }
        b.drop_owner(owner)
    }
    pub fn terminal_state(
        &mut self,
        route: RouteKey,
        epoch: u64,
        subject: u64,
    ) -> Option<Completed> {
        let mut done = None;
        let b = &mut self.broker;
        let f = &mut self.flight;
        self.controllers.terminal_state(route, epoch, subject, |o| {
            if let Some(c) = Self::retire(b, f, o) {
                done = Some(c);
            }
        });
        done
    }
    pub fn route_gone(&mut self, route: RouteKey) -> Option<Completed> {
        // A Bind has no controller entry yet; retain its local incarnation too.
        if let Some(f) = self.flight.as_mut() {
            if f.route == Some(route) {
                f.usable = false;
            }
        }
        let mut done = None;
        let b = &mut self.broker;
        let f = &mut self.flight;
        self.controllers.route_gone(route, |o| {
            if let Some(c) = Self::retire(b, f, o) {
                done = Some(c);
            }
        });
        done
    }
    pub fn disconnect(&mut self, connection: u64) -> Option<Completed> {
        let mut done = None;
        let b = &mut self.broker;
        let f = &mut self.flight;
        self.controllers.disconnect(connection, |o| {
            if let Some(c) = Self::retire(b, f, o) {
                done = Some(c);
            }
        });
        // Also remove any application target whose registration was already retired.
        self.broker.drop_connection(connection).or(done)
    }
    pub fn unbind(&mut self, peer: Peer, scope: Scope) -> Result<Option<Completed>, Failure> {
        let mut done = None;
        let b = &mut self.broker;
        let f = &mut self.flight;
        self.controllers.unbind(peer, scope, |o| {
            done = Self::retire(b, f, o);
        })?;
        Ok(done)
    }
    pub fn focus_lost(&mut self, route: RouteKey, epoch: u64) -> Option<Completed> {
        let mut done = None;
        let b = &mut self.broker;
        self.controllers.on_route(route, |a| {
            done = b.lose_focus(a.owner, epoch);
        });
        done
    }
    pub fn seat(&mut self, normal: Option<u64>) -> Option<Completed> {
        let normal = normal.filter(|n| *n != 0);
        if self.normal == normal {
            return None;
        }
        self.normal = normal;
        if let Some(f) = self.flight.as_mut() {
            f.usable = false;
        }
        let mut done = None;
        let b = &mut self.broker;
        self.controllers.seat(normal, |owner| {
            if let Some(c) = b.drop_owner(owner) {
                done = Some(c);
            }
        });
        self.broker.seat(normal).or(done)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::clipbroker::{Outcome, ADMISSION_MS};
    fn peer() -> Peer {
        Peer {
            connection: 7,
            stripes: 17,
            principal: 1000,
            alive: true,
        }
    }
    fn terminal() -> Terminal {
        Terminal {
            route: RouteKey {
                leaf: 2,
                incarnation: 3,
            },
            binding: 4,
            foreground: 5,
            context: 6,
            epoch: 7,
        }
    }
    fn target() -> Target {
        Target {
            connection: 7,
            fid: 8,
            request: 9,
        }
    }
    fn owner() -> Interaction {
        let mut i = Interaction::new(1, 1000).unwrap();
        i.seat(Some(2));
        i
    }
    fn receipt(q: Request, focus: u64) -> Reply {
        Reply {
            op: q.op,
            request: q.request,
            focus,
            seat: 2,
            foreground: match q.op {
                Op::Bind => 5,
                Op::Unbind => 0,
                _ => q.foreground,
            },
        }
    }
    fn finish(i: &mut Interaction, q: Request, focus: u64) -> Option<Completion> {
        i.complete(q, Some(peer()), Ok(receipt(q, focus)), 0)
    }
    fn register(i: &mut Interaction) -> Scope {
        let q = i.publish(terminal(), peer()).unwrap();
        match finish(i, q, 10).unwrap() {
            Completion::Published { result: Ok(s), .. } => s,
            other => panic!("{other:?}"),
        }
    }
    fn stage(i: &mut Interaction, s: Scope, bytes: &[u8]) -> u64 {
        let q = i.begin(peer(), s, target(), bytes.len(), 0).unwrap();
        let Some(Completion::Clipboard(Completed {
            result: Ok(Outcome::Begun(id)),
            ..
        })) = finish(i, q, 10)
        else {
            panic!()
        };
        assert_eq!(i.write(peer(), s, id, 0, bytes, 0), Ok(bytes.len()));
        id
    }
    fn copy(i: &mut Interaction, s: Scope, bytes: &[u8]) {
        let id = stage(i, s, bytes);
        let q = i
            .commit(peer(), s, target(), id, i.generation(), 0)
            .unwrap();
        assert!(matches!(
            finish(i, q, 10),
            Some(Completion::Clipboard(Completed {
                result: Ok(Outcome::Committed(_)),
                ..
            }))
        ));
    }
    #[test]
    fn control_publication_and_clipboard_share_one_sequence_and_slot() {
        let mut i = owner();
        let q = i.control(Op::Bind, terminal().route, 42, 4).unwrap();
        assert_eq!(q.request, 1);
        assert_eq!(i.publish(terminal(), peer()), Err(Failure::Busy));
        assert!(matches!(
            finish(&mut i, q, 10),
            Some(Completion::Control { result: Ok(_), .. })
        ));
        let p = i.publish(terminal(), peer()).unwrap();
        assert_eq!(p.request, 2);
        assert_eq!(
            i.control(Op::Unbind, terminal().route, 0, 4),
            Err(Failure::Busy)
        );
        let Some(Completion::Published { result: Ok(s), .. }) = finish(&mut i, p, 10) else {
            panic!()
        };
        let c = i.begin(peer(), s, target(), 0, 0).unwrap();
        assert_eq!(c.request, 3);
        assert_eq!(i.publish(terminal(), peer()), Err(Failure::Busy));
        assert!(finish(&mut i, c, 10).is_some());
        assert_eq!(
            i.control(Op::Unbind, terminal().route, 0, 4)
                .unwrap()
                .request,
            4
        );
    }
    #[test]
    fn cancellation_holds_transport_slot_until_exact_completion() {
        let mut i = owner();
        let s = register(&mut i);
        let q = i.begin(peer(), s, target(), 3, 0).unwrap();
        let wrong = Target {
            fid: 88,
            ..target()
        };
        assert_eq!(i.cancel_pending(wrong), None);
        assert!(i.cancel_pending(target()).is_some());
        assert!(i.busy());
        assert_eq!(i.begin(peer(), s, wrong, 0, 0), Err(Failure::Busy));
        assert_eq!(finish(&mut i, Request { binding: 99, ..q }, 10), None);
        assert!(i.busy());
        assert_eq!(finish(&mut i, q, 10), None);
        assert!(!i.busy());
        let next = i.begin(peer(), s, wrong, 0, 0).unwrap();
        assert!(next.request > q.request);
        assert_eq!(finish(&mut i, q, 10), None);
        assert!(i.busy());
        assert!(finish(&mut i, next, 10).is_some());
    }
    #[test]
    fn bind_retirement_uses_incarnation_even_without_controller() {
        for old_event in [true, false] {
            let mut i = owner();
            let route = terminal().route;
            let q = i.control(Op::Bind, route, 42, 4).unwrap();
            i.route_gone(if old_event {
                RouteKey {
                    incarnation: 1,
                    ..route
                }
            } else {
                route
            });
            assert!(i.busy());
            let Some(Completion::Control { result, .. }) = finish(&mut i, q, 10) else {
                panic!()
            };
            assert_eq!(result.is_ok(), old_event);
        }
    }
    #[test]
    fn pending_publication_cannot_survive_retirement_or_missing_peer() {
        for cause in 0..5 {
            let mut i = owner();
            let q = i.publish(terminal(), peer()).unwrap();
            match cause {
                0 => {
                    i.disconnect(peer().connection);
                }
                1 => {
                    i.route_gone(terminal().route);
                }
                2 => {
                    i.terminal_state(terminal().route, 5, 99);
                }
                3 => {
                    i.seat(None);
                    i.seat(Some(2));
                }
                _ => {}
            }
            assert!(i.busy());
            assert!(matches!(
                i.complete(
                    q,
                    if cause == 4 { None } else { Some(peer()) },
                    Ok(receipt(q, 10)),
                    0
                ),
                Some(Completion::Published { result: Err(_), .. })
            ));
            let s = register(&mut i);
            assert!(s.controller > q.controller);
        }
    }
    #[test]
    fn same_epoch_subject_retirement_cancels_payload_and_mode_together() {
        let mut i = owner();
        let s = register(&mut i);
        i.report(peer(), s, 1, Mode::Normal, false, "nora").unwrap();
        let id = stage(&mut i, s, b"hello");
        let q = i.commit(peer(), s, target(), id, 0, 0).unwrap();
        assert!(i.terminal_state(terminal().route, 5, 99).is_some());
        assert_eq!(i.mode(terminal().route), None);
        assert!(i.busy());
        assert_eq!(i.payload_reservation(), 0);
        assert_eq!(finish(&mut i, q, 10), None);
        assert_eq!(i.generation(), 0);
        assert!(i.report(peer(), s, 2, Mode::Normal, false, "nora").is_err());
    }
    #[test]
    fn focus_retains_mode_and_earliest_boundary_for_snapshot_reads() {
        for focus in [10, 11, 12, 99] {
            let mut i = owner();
            let s = register(&mut i);
            copy(&mut i, s, b"proportional \xc3\xa9");
            i.report(peer(), s, 1, Mode::Visual, true, "selection")
                .unwrap();
            let q = i.get(peer(), s, target(), 0).unwrap();
            assert_eq!(i.focus_lost(terminal().route, 11), None);
            i.focus_lost(terminal().route, 13);
            assert_eq!(i.mode(terminal().route).unwrap().mode, Mode::Visual);
            let Some(Completion::Clipboard(done)) = finish(&mut i, q, focus) else {
                panic!()
            };
            if focus < 11 {
                let Outcome::Clipboard(snapshot) = done.result.unwrap() else {
                    panic!()
                };
                assert_eq!(
                    i.read(peer(), s, snapshot.transfer, 0, 100, 0),
                    Ok(&b"proportional \xc3\xa9"[..])
                );
                i.disconnect(peer().connection);
                assert!(i.read(peer(), s, snapshot.transfer, 0, 100, 0).is_err());
            } else {
                assert_eq!(done.result, Err(Failure::Gone));
            }
        }
    }
    #[test]
    fn focus_loss_cancels_begin_but_does_not_release_flight() {
        let mut i = owner();
        let s = register(&mut i);
        let q = i.begin(peer(), s, target(), 5, 0).unwrap();
        assert!(i.focus_lost(terminal().route, 11).is_some());
        assert!(i.busy());
        assert_eq!(finish(&mut i, q, 10), None);
        assert_eq!(i.payload_reservation(), 0);
        assert!(i.begin(peer(), s, target(), 1, 0).is_ok());
    }
    #[test]
    fn expiry_and_sak_drain_without_publishing_or_restoring_authority() {
        for sak in [false, true] {
            let mut i = owner();
            let s = register(&mut i);
            let id = stage(&mut i, s, b"private");
            let q = i.commit(peer(), s, target(), id, 0, 0).unwrap();
            assert_eq!(i.deadline(), Some(ADMISSION_MS));
            let done = if sak {
                i.seat(None)
            } else {
                i.expire(ADMISSION_MS)
            };
            assert!(done.unwrap().result.is_err());
            assert!(i.busy());
            if sak {
                i.seat(Some(3));
            }
            assert_eq!(i.publish(terminal(), peer()), Err(Failure::Busy));
            assert_eq!(finish(&mut i, q, 10), None);
            assert_eq!(i.generation(), 0);
            assert!(!i.busy());
            if sak {
                assert!(i.report(peer(), s, 1, Mode::Normal, false, "").is_err());
            }
        }
    }
    #[test]
    fn seat_round_trip_cannot_revive_pending_host_control() {
        let mut i = owner();
        let q = i.control(Op::Bind, terminal().route, 42, 4).unwrap();
        i.seat(None);
        i.seat(Some(2));
        assert!(i.busy());
        assert!(matches!(
            finish(&mut i, q, 10),
            Some(Completion::Control {
                result: Err(Failure::Gone),
                ..
            })
        ));
        assert!(!i.busy());
    }
    #[test]
    fn invalid_controls_and_receipts_never_authorize() {
        let mut i = owner();
        let r = terminal().route;
        assert_eq!(i.control(Op::Bind, r, 0, 4), Err(Failure::Invalid));
        let q = i.control(Op::Bind, r, 42, 4).unwrap();
        assert_eq!(q.request, 2);
        assert!(matches!(
            finish(&mut i, q, 0),
            Some(Completion::Control {
                result: Err(Failure::Gone),
                ..
            })
        ));
        let s = register(&mut i);
        let q = i.begin(peer(), s, target(), 5, 0).unwrap();
        assert_eq!(
            finish(&mut i, q, 0),
            Some(Completion::Clipboard(Completed {
                target: target(),
                result: Err(Failure::Gone)
            }))
        );
        assert_eq!(i.payload_reservation(), 0);
    }
    #[test]
    fn host_unbind_retires_locally_even_when_transport_refuses() {
        let mut i = owner();
        let s = register(&mut i);
        let id = stage(&mut i, s, b"retire");
        let q = i.control(Op::Unbind, terminal().route, 0, 4).unwrap();
        assert!(i.write(peer(), s, id, 0, b"x", 0).is_err());
        assert_eq!(i.payload_reservation(), 0);
        assert!(matches!(
            i.complete(q, None, Err(Failure::Denied), 0),
            Some(Completion::Control { result: Err(_), .. })
        ));
        assert!(i.begin(peer(), s, target(), 0, 0).is_err());
    }
    #[test]
    fn stream_events_match_binding_before_resolving_local_route() {
        use libhalcyon::interaction_events::Body;
        let mut i = owner();
        let s = register(&mut i);
        let q = i.begin(peer(), s, target(), 3, 0).unwrap();
        assert_eq!(
            i.observe(Body::Retired {
                leaf: terminal().route.leaf,
                binding: 99
            }),
            Ok(None)
        );
        assert_eq!(
            i.observe(Body::FocusLost {
                leaf: terminal().route.leaf,
                binding: 99,
                epoch: 11
            }),
            Ok(None)
        );
        assert!(i
            .observe(Body::FocusLost {
                leaf: terminal().route.leaf,
                binding: 4,
                epoch: 11
            })
            .unwrap()
            .is_some());
        assert_eq!(finish(&mut i, q, 10), None);
        let q = i.begin(peer(), s, target(), 3, 0).unwrap();
        assert!(i
            .observe(Body::Terminal {
                leaf: terminal().route.leaf,
                binding: 4,
                foreground: 5,
                subject: 99
            })
            .unwrap()
            .is_some());
        assert_eq!(finish(&mut i, q, 10), None);
        assert!(i.begin(peer(), s, target(), 0, 0).is_err());
    }
    #[test]
    fn stream_reset_retires_without_granting_seat_membership() {
        use libhalcyon::interaction_events::Body;
        let mut i = owner();
        let s = register(&mut i);
        let q = i.begin(peer(), s, target(), 1, 0).unwrap();
        assert!(i.observe(Body::Reset).unwrap().is_some());
        assert_eq!(finish(&mut i, q, 10), None);
        assert!(i.begin(peer(), s, target(), 0, 0).is_err());
        let s2 = register(&mut i);
        assert!(s2.controller > s.controller);
        i.seat(None);
        i.observe(Body::Reset).unwrap();
        assert_eq!(i.publish(terminal(), peer()), Err(Failure::Denied));
    }
    #[test]
    fn explicit_unbind_cancels_the_real_broker_and_retains_slot() {
        let mut i = owner();
        let s = register(&mut i);
        let q = i.begin(peer(), s, target(), 5, 0).unwrap();
        assert!(i.unbind(peer(), s).unwrap().is_some());
        assert!(i.busy());
        assert_eq!(finish(&mut i, q, 10), None);
        assert!(i.begin(peer(), s, target(), 0, 0).is_err());
        assert!(register(&mut i).controller > s.controller);
    }
}
