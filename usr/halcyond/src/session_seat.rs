//! Clipboard cancellation owner, polled by the session service executor.
//! No Surface/EventRing or normal presentation wait is borrowed here. Public
//! clipboard dispatch stays disabled until its controller/protocol adapter is
//! complete; this owner holds the shared interaction owner and its seat lifecycle.
use halcyond::{hostbindings::{Bindings, Desired}, interaction::Interaction, paneroute::Routes};
use libhalcyon::seat_control::{Op, Request, Snapshot};
use libthyla_rs::{
    err::{Error, Result},
    fs::File,
};
use tapestry::seat::Channel;
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
    routes: Routes,
    state: Snapshot,
    next: u64,
    joined: bool,
    stopping: bool,
    retired: bool,
    acknowledged: Option<(u64, u64)>,
}
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
            routes: Routes::empty(),
            state: setup.snapshot,
            next: 1,
            joined: false,
            stopping: false,
            retired: false,
            acknowledged: None,
        };
        link.send(Op::Join)?;
        Ok(link)
    }
    fn send(&mut self, op: Op) -> Result<()> {
        let request = self.next;
        self.next = request.checked_add(1).ok_or(Error::Io)?;
        let q = Request {
            op,
            request,
            registration: self.state.registration,
            generation: if op == Op::State {
                0
            } else {
                self.state.generation
            },
            revision: self.state.revision,
        };
        self.channel.start(q).map_err(|_| Error::Io)?;
        self.channel.pump().map_err(|_| Error::Io)
    }
    pub fn fd(&self) -> i32 {
        self.channel.poll_fd()
    }
    pub fn admission_fd(&self) -> i32 {
        self.admission.as_ref().map_or(-1, |a| a.poll_fd())
    }
    pub fn ready(&self) -> bool {
        self.joined && self.admission.as_ref().is_some_and(|a| a.ready())
    }
    pub fn runnable(&self) -> bool {
        self.admission.as_ref().is_some_and(|a| a.runnable())
    }
    #[cfg(feature="test-mode")]
    pub fn bound_count(&self) -> u64 { self.bound_count }
    pub fn retired(&self) -> bool {
        self.retired
    }
    pub fn stop(&mut self) {
        self.stopping = true;
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
    fn normal(&self) -> Option<u64> {
        (self.joined && !self.stopping && !self.retired && self.state.enabled && self.state.phase == 0)
            .then_some(self.state.generation)
    }
    pub fn pump(&mut self) -> Result<()> {
        self.pump_seat()?;
        let now = libthyla_rs::time::monotonic_ns() / 1_000_000;
        let _ = self.interaction.expire(now);
        if self.interaction.drain_required() {
            // Channel drops its ring (which joins SQPOLL) before registered
            // storage. A timer alone never frees borrowed transport buffers.
            drop(self.admission.take());
            let _ = self.interaction.transport_closed(now);
            return Err(Error::TimedOut);
        }
        let admission = self.admission.as_mut().ok_or(Error::Io)?;
        admission.pump().map_err(|_| Error::Io)?;
        if let Some(record) = admission.take() {
            use libhalcyon::interaction_events::Body;
            match record.body {
                Body::Ready(_) => {}
                Body::Decision { leaf, binding, request, op, result } => {
                    let q = self.bindings.request().ok_or(Error::Io)?;
                    if (q.leaf, q.binding, q.request, q.op) != (leaf, binding, request, op) {
                        return Err(Error::Io);
                    }
                    // Remember the remote side effect independently of the
                    // locally usable receipt (which can be Gone after SAK).
                    if !self.bindings.complete(q, result.map(|_| ())) { return Err(Error::Io); }
                    let _ = self.interaction.complete(q, None,
                        result.map_err(|_| libhalcyon::interaction_wire::Failure::Denied), now);
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
                    let _ = self.interaction.observe(body).map_err(|_| Error::Io)?;
                }
            }
        }
        if !self.interaction.busy() && self.admission.as_ref().is_some_and(|a| a.ready()) {
            let normal = self.normal();
            if let Some(action) = self.bindings.plan(&self.hosts, normal) {
                let q = self.interaction.control(action.op,
                    halcyond::controllers::RouteKey { leaf: action.host.route.leaf,
                        incarnation: action.host.route.incarnation },
                    if action.op == libhalcyon::interaction_control::Op::Bind { action.host.pid } else { 0 },
                    action.host.binding, now).map_err(|_| Error::Io)?;
                if !self.bindings.started(action, q, self.state.generation) { return Err(Error::Io); }
                self.admission.as_mut().ok_or(Error::Io)?.start(q).map_err(|_| Error::Io)?;
            }
        }
        Ok(())
    }
    fn pump_seat(&mut self) -> Result<()> {
        if self.retired {
            return Ok(());
        }
        // The cancellation lane never waits for normal graphics or an
        // ordered admission response.
        self.channel.pump().map_err(|_| Error::Io)?;
        let Some(done) = self.channel.take() else {
            return Ok(());
        };
        let reply = match done.result {
            Ok(r) => r,
            // A phase may advance before a cancellation/retirement reaches the
            // coordinator. Re-sample without restoring local authority.
            Err(tapestry::seat::Error::Transport(-1))
                if self.joined && matches!(done.request.op, Op::Cancelled | Op::Retire) =>
            {
                let _ = self.interaction.seat(None);
                self.state.revision = 0;
                return self.send(Op::State);
            }
            _ => return Err(Error::Io),
        };
        self.state = reply.state;
        if done.request.op == Op::Retire {
            self.retired = true;
            return Ok(());
        }
        if done.request.op == Op::Join {
            if !self.state.enabled || self.state.phase != 0 {
                return Err(Error::PermissionDenied);
            }
            self.joined = true;
            let _ = self.interaction.seat(Some(self.state.generation));
        }
        if self.stopping {
            let _ = self.interaction.seat(None);
            return self.send(Op::Retire);
        }
        if self.state.phase != 0 {
            // With public clipboard dispatch still off there are no app replies
            // to retire. Its eventual adapter MUST discard unsent output and
            // close partial frames here before this exact acknowledgement.
            let _ = self.interaction.seat(None);
            let identity = (self.state.generation, self.state.revision);
            if done.request.op == Op::Cancelled {
                self.acknowledged = Some(identity);
            }
            if self.state.phase == 1 && self.acknowledged != Some(identity) {
                return self.send(Op::Cancelled);
            }
        } else if !self.state.enabled {
            let _ = self.interaction.seat(None);
            self.acknowledged = None;
            return self.send(Op::Join);
        }
        if self.normal().is_some() {
            let _ = self.interaction.seat(self.normal());
        }
        self.send(Op::State)
    }
}
