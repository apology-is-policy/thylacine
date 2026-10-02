//! Clipboard cancellation owner, polled by the session service executor.
//! No Surface/EventRing or normal presentation wait is borrowed here. Public
//! clipboard dispatch stays disabled until its controller/protocol adapter is
//! complete; this owner holds the shared interaction owner and its seat lifecycle.
use halcyond::interaction::Interaction;
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
    admission: tapestry::ordered::Channel,
    // Keep the normal reservation fid pinned through retirement of this lane.
    _reservation: File,
    interaction: Interaction,
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
            admission: tapestry::ordered::Channel::from_file(setup.admission)
                .map_err(|_| Error::Io)?,
            _reservation: setup.reservation,
            interaction,
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
        self.admission.poll_fd()
    }
    pub fn ready(&self) -> bool {
        self.joined && self.admission.ready()
    }
    pub fn runnable(&self) -> bool {
        self.admission.runnable()
    }
    pub fn retired(&self) -> bool {
        self.retired
    }
    pub fn stop(&mut self) {
        self.stopping = true;
        let _ = self.interaction.seat(None);
    }
    pub fn pump(&mut self) -> Result<()> {
        self.pump_seat()?;
        self.admission.pump().map_err(|_| Error::Io)?;
        if let Some(record) = self.admission.take() {
            use libhalcyon::interaction_events::Body;
            match record.body {
                Body::Ready(_) => {}
                // App dispatch remains disabled; an unsolicited decision is a protocol fault.
                Body::Decision { .. } => return Err(Error::Io),
                body => {
                    let _ = self.interaction.observe(body).map_err(|_| Error::Io)?;
                }
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
        self.send(Op::State)
    }
}
