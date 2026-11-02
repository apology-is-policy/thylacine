//! Clipboard cancellation owner, polled by the session service executor.
//! No Surface/EventRing or normal presentation wait is borrowed here. Public
//! clipboard dispatch shares this owner with the native protocol adapter.
//! All retirement callbacks finish before acknowledgement of a trusted episode.
use halcyond::{hostbindings::{Bindings, Desired}, interaction::Interaction, paneroute::Routes};
use libhalcyon::seat_control::{Op, Participant, Snapshot};
use libthyla_rs::{
    err::{Error, Result},
    fs::File,
};
use tapestry::seat::Channel;
/// Application lifecycle callbacks run on the same service owner. Retirement
/// must close partial application frames before returning to the HSC sender.
pub trait Applications {
    fn retire(&mut self, owner: &mut Interaction) -> Result<()>;
    fn prune(&mut self, owner: &mut Interaction) -> Result<()>;
    fn complete(&mut self, done: halcyond::interaction::Completion);
    fn decision(&mut self, owner: &mut Interaction, request: libhalcyon::interaction_control::Request,
        result: core::result::Result<libhalcyon::interaction_control::Reply, libhalcyon::interaction_wire::Failure>, now: u64) -> Result<bool>;
}
impl Applications for () {
    fn retire(&mut self, _: &mut Interaction) -> Result<()> { Ok(()) }
    fn prune(&mut self, _: &mut Interaction) -> Result<()> { Ok(()) }
    fn complete(&mut self, _: halcyond::interaction::Completion) {}
    fn decision(&mut self, _: &mut Interaction, _: libhalcyon::interaction_control::Request,
        _: core::result::Result<libhalcyon::interaction_control::Reply, libhalcyon::interaction_wire::Failure>, _: u64) -> Result<bool> { Ok(false) }
}
pub struct Setup {
    pub reservation: File,
    pub admission: File,
    pub snapshot: Snapshot,
}
pub struct Link {
    channel: Channel,
    admission: Option<tapestry::ordered::Channel>,
    hosts: Desired,
    #[cfg(feature="test-mode")]
    bound_count: u64,
    bindings: Bindings,
    // Keep the normal reservation fid pinned through retirement of this lane.
    _reservation: File,
    interaction: Interaction,
    queued: Option<libhalcyon::interaction_control::Request>,
    routes: Routes,
    seat: Participant,
    next: u64,
    #[cfg(feature="test-mode")]
    first_join_lost: bool,
}
const _: () = assert!(core::mem::size_of::<Link>() <= 48 * 1024);
pub const FIXED_BYTES: usize = core::mem::size_of::<Link>();
impl Link {
    pub fn new(setup: Setup, principal: u32) -> Result<Self> {
        let interaction = Interaction::new(setup.snapshot.registration, principal)
            .map_err(|_| Error::InvalidArgument)?;
        let mut link = Self {
            channel: Channel::open().map_err(|_| Error::Io)?,
            admission: Some(tapestry::ordered::Channel::from_file(setup.admission)
                .map_err(|_| Error::Io)?),
            hosts: Desired::empty(),
            #[cfg(feature="test-mode")]
            bound_count: 0,
            bindings: Bindings::new(),
            _reservation: setup.reservation,
            interaction,
            queued: None,
            routes: Routes::empty(),
            seat: Participant::new(setup.snapshot),
            next: 1,
            #[cfg(feature="test-mode")]
            first_join_lost: false,
        };
        link.send(Op::Join)?;
        Ok(link)
    }
    fn send(&mut self, op: Op) -> Result<()> {
        let request = self.next;
        self.next = request.checked_add(1).ok_or(Error::Io)?;
        self.channel.start(self.seat.request(op, request)).map_err(|_| Error::Io)?;
        self.channel.pump().map_err(|_| Error::Io)
    }
    pub fn fd(&self) -> i32 {
        self.channel.poll_fd()
    }
    pub fn admission_fd(&self) -> i32 {
        self.admission.as_ref().map_or(-1, |a| a.poll_fd())
    }
    pub fn ready(&self) -> bool {
        self.seat.joined() && self.admission.as_ref().is_some_and(|a| a.ready())
    }
    pub fn joined(&self) -> bool {
        self.seat.joined()
    }
    /// The compositor admits no one and answers nothing while this holds.
    pub fn parked(&self) -> bool {
        self.seat.parked()
    }
    pub fn runnable(&self) -> bool {
        self.queued.is_some() || self.admission.as_ref().is_some_and(|a| a.runnable())
    }
    #[cfg(feature="test-mode")]
    pub fn bound_count(&self) -> u64 { self.bound_count }
    pub fn retired(&self) -> bool {
        self.seat.retired()
    }
    pub fn stop(&mut self) {
        self.seat.stop();
        let _ = self.interaction.seat(None);
    }
    /// Called before draining admission records. Coalescing may hide a removal,
    /// but never its new route incarnation. No app output exists yet; the app
    /// adapter must also deliver/retire the returned cancellation before ACK.
    pub fn routes(&mut self, desired: Routes, mut hosts: Desired) {
        desired.retired_since(&self.routes, |r| {
            let _ = self.interaction.route_gone(halcyond::controllers::RouteKey {
                leaf: r.leaf,
                incarnation: r.incarnation,
            });
        });
        hosts.retain(&desired);
        self.hosts = hosts;
        self.routes = desired;
    }
    pub fn session(&self) -> u64 { self.seat.state().registration }
    pub fn context(&mut self, peer: halcyond::controllers::Peer) -> halcyond::application::Context<'_> {
        halcyond::application::Context { owner: &mut self.interaction, bindings: &self.bindings,
            desired: &self.hosts, queued: &mut self.queued, peer,
            now: libthyla_rs::time::monotonic_ns() / 1_000_000 }
    }
    pub fn disconnect(&mut self, connection: u64) { self.interaction.disconnect(connection); }
    pub fn deadline(&self) -> Option<u64> { self.interaction.deadline().map(|d| d.saturating_mul(1_000_000)) }
    pub fn pump(&mut self) -> Result<()> { self.pump_with(&mut ()) }
    pub fn pump_with(&mut self, apps: &mut impl Applications) -> Result<()> {
        self.pump_seat(apps)?;
        apps.prune(&mut self.interaction)?;
        if let Some(q) = self.queued.take() {
            self.admission.as_mut().ok_or(Error::Io)?.start(q).map_err(|_| Error::Io)?;
        }
        let now = libthyla_rs::time::monotonic_ns() / 1_000_000;
        if self.seat.parked() { self.interaction.hold(now); }
        if let Some(done) = self.interaction.expire(now) { apps.complete(done); }
        retire_overdue(&mut self.interaction, &mut self.admission, now)?;
        let admission = self.admission.as_mut().ok_or(Error::Io)?;
        admission.pump().map_err(|_| Error::Io)?;
        if let Some(record) = admission.take() {
            use libhalcyon::interaction_events::Body;
            match record.body {
                Body::Ready(_) => {}
                Body::Decision { leaf, binding, request, op, result } => {
                    let q = self.interaction.request().ok_or(Error::Io)?;
                    if (q.leaf, q.binding, q.request, q.op) != (leaf, binding, request, op) {
                        return Err(Error::Io);
                    }
                    // Remember the remote side effect independently of the
                    // locally usable receipt (which can be Gone after SAK).
                    let raw_result = result;
                    let result = result.map_err(|_| libhalcyon::interaction_wire::Failure::Denied);
                    if matches!(op, libhalcyon::interaction_control::Op::Bind | libhalcyon::interaction_control::Op::Unbind) {
                        if !self.bindings.complete(q, raw_result.map(|_| ())) { return Err(Error::Io); }
                        let _ = self.interaction.complete(q, None, result, now);
                    } else if !apps.decision(&mut self.interaction, q, result, now)? {
                        // A cancelled/closed client's exact receipt still drains
                        // the shared lane; it cannot revive an application fid.
                        if let Some(done) = self.interaction.complete(q, None, result, now) { apps.complete(done); }
                    }
                    #[cfg(feature="test-mode")]
                    if op == libhalcyon::interaction_control::Op::Bind && result.is_ok() {
                        self.bound_count = self.bound_count.checked_add(1).ok_or(Error::Io)?;
                    }
                }
                body => {
                    match body {
                        Body::Retired { leaf, binding } => self.bindings.retired(leaf, binding),
                        Body::Terminal { leaf, binding, foreground, .. } =>
                            self.bindings.terminal_state(leaf, binding, foreground),
                        _ => {}
                    }
                    if let Some(done) = self.interaction.observe(body).map_err(|_| Error::Io)? {
                        apps.complete(halcyond::interaction::Completion::Clipboard(done));
                    }
                    apps.prune(&mut self.interaction)?;
                }
            }
        }
        if !self.interaction.busy() && self.admission.as_ref().is_some_and(|a| a.ready()) {
            let normal = self.seat.normal();
            if let Some(action) = self.bindings.plan(&self.hosts, normal) {
                let q = self.interaction.control(action.op,
                    halcyond::controllers::RouteKey { leaf: action.host.route.leaf,
                        incarnation: action.host.route.incarnation },
                    if action.op == libhalcyon::interaction_control::Op::Bind { action.host.pid } else { 0 },
                    action.host.binding, now).map_err(|_| Error::Io)?;
                if !self.bindings.started(action, q, self.seat.state().generation) { return Err(Error::Io); }
                self.admission.as_mut().ok_or(Error::Io)?.start(q).map_err(|_| Error::Io)?;
            }
        }
        Ok(())
    }
    fn pump_seat(&mut self, apps: &mut impl Applications) -> Result<()> {
        if self.seat.retired() {
            return Ok(());
        }
        // The cancellation lane never waits for normal graphics or an
        // ordered admission response.
        self.channel.pump().map_err(|_| Error::Io)?;
        let Some(done) = self.channel.take() else {
            return Ok(());
        };
        // Rlerror is the table's decision; anything else broke the lane.
        let reply = match done.result {
            Ok(r) => Some(r.state),
            Err(tapestry::seat::Error::Transport(-1)) => None,
            Err(_) => return Err(Error::Io),
        };
        #[cfg(feature="test-mode")]
        let rejoining = self.seat.joined() && done.request.op == Op::Join && reply.is_some();
        #[cfg(feature="test-mode")]
        if !self.seat.joined() && reply.is_none() {
            self.first_join_lost = true;
        }
        let step = self.seat.complete(&done.request, reply).map_err(|e| match e {
            libhalcyon::seat_control::Error::Invalid => Error::PermissionDenied,
            _ => Error::Io,
        })?;
        // Safe to write: this member now owes a cancellation, so the seat
        // cannot reach EXCLUSIVE, where console output parks, without us.
        #[cfg(feature="test-mode")]
        if rejoining {
            let _ = libthyla_rs::t_putstr(&alloc::format!(
                "halcyond: seat link rejoined generation={}\n", self.seat.state().generation));
        }
        // Not when the refusal lands: console output parks during the episode, and
        // this owner must keep telling setup that it is alive.
        #[cfg(feature="test-mode")]
        if self.first_join_lost && self.seat.joined() {
            self.first_join_lost = false;
            let _ = libthyla_rs::t_putstr(&alloc::format!(
                "halcyond: seat link joined after a lost first join generation={}\n", self.seat.state().generation));
        }
        if step.revoke {
            let _ = self.interaction.seat(None);
            // No rendering or normal HIA wait occurs in this barrier. The
            // callback closes application fds and drops cached/partial replies.
            apps.retire(&mut self.interaction)?;
        }
        if step.normal.is_some() {
            let _ = self.interaction.seat(step.normal);
        }
        match step.send {
            Some(op) => self.send(op),
            None => Ok(()),
        }
    }
}

/// An expired receipt is not proof that borrowed I/O has finished. Close/join
/// the ordered ring before releasing registered storage or the owner's flight.
/// Shared with the native stalled-WRITE qualification, without a test clock.
pub fn retire_overdue(owner: &mut Interaction, channel: &mut Option<tapestry::ordered::Channel>, now: u64) -> Result<()> {
    if !owner.drain_required() { return Ok(()); }
    drop(channel.take());
    let _ = owner.transport_closed(now);
    Err(Error::TimedOut)
}
