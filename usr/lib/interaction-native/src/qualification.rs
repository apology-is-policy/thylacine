//! The client half of SAK at every operation (ASYNC-SERVICE-LIFECYCLE, AS-4c), here so
//! that the host tests run it against a simulated SAK before a guest does. It reaches
//! the adapter only through the `Kernel` it is built on, so what a guest qualifies is
//! the `Host` a program ships, with no test hook in it.
use super::Host;
use alloc::{rc::Rc, vec::Vec};
use core::cell::Cell;
use libhalcyon::interaction_client::{Event, Why};
use libhalcyon::interaction_wire::{Failure, Mode};
use loom_client::native::{Kernel, Setup};

pub type Result<T> = core::result::Result<T, &'static str>;

const MS: u64 = 1_000_000;
/// Past the client's whole reconnect schedule, so running out means no progress.
const PATIENCE_NS: u64 = 60_000 * MS;
const LONGEST_SLEEP_MS: u64 = 1_000;
/// The first use of a connection a SAK ended fails, and a new binding may be refused
/// while the old owner is leaving. More than this is not recovery.
const PASTE_ATTEMPTS: u32 = 4;
const LABEL: &str = "async qualification";

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Moment {
    /// Held with the submission still queued, so it reaches the kernel on a connection
    /// the SAK has already ended.
    Before,
    /// Held just after it, with the operation in flight or its completion unrouted.
    After,
}

#[derive(Default)]
pub struct Progress {
    submitted: Cell<u32>,
    /// The number the hold came at, 0 before it.
    held_at: Cell<u32>,
}

impl Progress {
    pub fn submitted(&self) -> u32 {
        self.submitted.get()
    }
    pub fn held(&self) -> bool {
        self.held_at.get() > 0
    }
}

/// A ring kernel that holds its thread once, at one numbered submission. Every SQE of
/// a run passes through ENTER, whichever step or background clunk made it, so the
/// numbers cover every operation and need no list of phases to go stale.
pub struct Stepping<K, H> {
    kernel: K,
    at: Option<(u32, Moment)>,
    hold: H,
    progress: Rc<Progress>,
}

impl<K, H: Fn()> Stepping<K, H> {
    pub fn new(kernel: K, at: Option<(u32, Moment)>, hold: H) -> (Self, Rc<Progress>) {
        let progress = Rc::new(Progress::default());
        let stepping = Self {
            kernel,
            at,
            hold,
            progress: progress.clone(),
        };
        (stepping, progress)
    }

    fn hold_once(&self, at: u32) {
        if self.progress.held_at.get() == 0 {
            self.progress.held_at.set(at);
            (self.hold)();
        }
    }
}

// SAFETY: every argument and result passes unchanged. `hold` only delays one ENTER,
// which the adapter cannot tell from a thread descheduled there. It must not reenter.
unsafe impl<K: Kernel, H: Fn()> Kernel for Stepping<K, H> {
    fn setup(&self, entries: u32, params: &mut Setup) -> i64 {
        self.kernel.setup(entries, params)
    }
    unsafe fn register(&self, fd: i32, op: u32, arg: *mut u8, nargs: u32) -> i64 {
        unsafe { self.kernel.register(fd, op, arg, nargs) }
    }
    fn enter(&self, fd: i32, to_submit: u32, min_complete: u32, flags: u32) -> i64 {
        if let Some((at, Moment::Before)) = self.at {
            if to_submit > 0 && self.progress.submitted.get() + to_submit >= at {
                self.hold_once(at);
            }
        }
        let rc = self.kernel.enter(fd, to_submit, min_complete, flags);
        if rc > 0 {
            self.progress
                .submitted
                .set(self.progress.submitted.get() + rc as u32);
        }
        if let Some((at, Moment::After)) = self.at {
            if self.progress.submitted.get() >= at {
                self.hold_once(at);
            }
        }
        rc
    }
    fn poll(&self, fd: i32, events: i16, timeout_ms: i32) -> i64 {
        self.kernel.poll(fd, events, timeout_ms)
    }
    fn close(&self, fd: i32) -> i64 {
        self.kernel.close(fd)
    }
    fn attach(&self, len: usize) -> i64 {
        self.kernel.attach(len)
    }
    fn detach(&self, va: usize, len: usize) -> i64 {
        self.kernel.detach(va, len)
    }
    fn now(&self) -> u64 {
        self.kernel.now()
    }
}

/// What the clipboard may hold after a copy that ended as `copy`: a reported outcome
/// is a promise, and only an unknown one may be either.
pub fn check_value(before: &[u8], text: &[u8], copy: &Event, pasted: &[u8]) -> Result<()> {
    match copy {
        Event::Copied { .. } if pasted == text => Ok(()),
        Event::Copied { .. } => Err("a copy reported done did not land"),
        Event::CopyUnknown if pasted == text || pasted == before => Ok(()),
        Event::CopyUnknown => Err("neither value after an unknown commit"),
        Event::CopyFailed(_) if pasted == before => Ok(()),
        Event::CopyFailed(_) => Err("a copy reported failed changed the clipboard"),
        _ => Err("not a copy's outcome"),
    }
}

#[derive(Debug)]
pub struct Report {
    pub held: bool,
    /// Whether the connection was bound when the hold began, so far as a sample
    /// between pumps can say. A bound connection must not outlive the SAK.
    pub bound_at_hold: bool,
    pub down_after: bool,
    pub submitted: u32,
    pub copy: Event,
    /// Pastes that failed on the way back. The value is checked on the first success.
    pub lost: u32,
    pub value: Vec<u8>,
}

struct Run<K: Kernel + 'static, P> {
    host: Host<K>,
    poll: P,
    progress: Rc<Progress>,
    bound_at_hold: bool,
    down_after: bool,
    mode_refusals: u32,
    lost: u32,
}

impl<K: Kernel + 'static, P: FnMut(i32, i32)> Run<K, P> {
    /// Every call into the host, since each can pump, and so hold.
    fn step(&mut self, act: impl FnOnce(&mut Host<K>, u64)) {
        let was_held = self.progress.held();
        let bound = self.host.is_bound();
        let now = self.host.now();
        act(&mut self.host, now);
        if self.progress.held() {
            if !was_held {
                self.bound_at_hold = bound;
            }
            self.down_after |= !self.host.is_bound();
        }
    }

    /// The host's one poll, bounded by what the client owes.
    fn until<T>(&mut self, mut done: impl FnMut(&mut Self) -> Option<T>) -> Result<T> {
        let limit = self.host.now().saturating_add(PATIENCE_NS);
        loop {
            if let Some(value) = done(self) {
                return Ok(value);
            }
            let now = self.host.now();
            if now >= limit {
                return Err("no progress");
            }
            let fd = self.host.poll_fd().ok_or("the ring closed")?;
            let owed = self
                .host
                .next_deadline()
                .map_or(u64::MAX, |at| at.saturating_sub(now));
            let timeout = owed.div_ceil(MS).min(LONGEST_SLEEP_MS) as i32;
            (self.poll)(fd, timeout);
            self.step(|host, _| {
                let now = host.now();
                host.pump(now);
            });
        }
    }

    fn event(&mut self) -> Result<Event> {
        self.until(|run| loop {
            match run.host.take_event() {
                Some(Event::ModeRefused(_)) => run.mode_refusals += 1,
                other => return other,
            }
        })
    }

    /// One paste, once the client will take it: a failed job's release may still be
    /// running.
    fn paste(&mut self) -> Result<core::result::Result<Vec<u8>, Why>> {
        self.until(|run| {
            let mut started = false;
            run.step(|host, now| started = host.paste(now).is_ok());
            started.then_some(())
        })?;
        match self.event()? {
            Event::Pasted(bytes) => Ok(Ok(bytes)),
            Event::PasteFailed(why) => Ok(Err(why)),
            _ => Err("a copy's event for a paste"),
        }
    }

    fn paste_through(&mut self) -> Result<Vec<u8>> {
        for _ in 0..PASTE_ATTEMPTS {
            match self.paste()? {
                Ok(bytes) => return Ok(bytes),
                Err(Why::Lost | Why::TimedOut | Why::Refused(Failure::Gone | Failure::Busy))
                    if self.progress.held() =>
                {
                    self.lost += 1;
                }
                Err(_) => return Err("a paste failed with no SAK to blame"),
            }
        }
        Err("no recovery after the SAK")
    }
}

/// Report a mode, copy `text` over a value `before`, then read back through whatever
/// connection is left. Works the same with nothing held, as the plain round trip.
pub fn run<K: Kernel + 'static>(
    host: Host<K>,
    progress: Rc<Progress>,
    poll: impl FnMut(i32, i32),
    before: &[u8],
    text: Vec<u8>,
) -> Result<Report> {
    let mut run = Run {
        host,
        poll,
        progress,
        bound_at_hold: false,
        down_after: false,
        mode_refusals: 0,
        lost: 0,
    };
    let mut asked = Ok(());
    run.step(|host, now| asked = host.report_mode(Mode::Normal, false, LABEL, now));
    asked.map_err(|_| "the mode report was turned down")?;
    // Settled before the job, so that the lanes never interleave and a number names
    // one phase.
    run.until(|r| (r.host.is_bound() && r.host.next_deadline().is_none()).then_some(()))?;

    let mut asked = Ok(());
    run.step(|host, now| asked = host.copy(text.clone(), now));
    asked.map_err(|_| "the copy was turned down")?;
    let copy = run.event()?;
    if !run.progress.held() && !matches!(copy, Event::Copied { .. }) {
        return Err("a copy failed with no SAK to blame");
    }

    let value = run.paste_through()?;
    check_value(before, &text, &copy, &value)?;

    // Number 1 is the CONNECT itself, which may reach the server after the episode. Past
    // it the connection had formed when the SAK came, and a client that has not noticed
    // has not used it since. It must not serve this read: a paste may recover on a fresh
    // connection, but it has to have needed one.
    if run.progress.held_at.get() > 1 && !run.down_after {
        let reruns = run.host.reruns();
        match run.paste()? {
            Ok(_) if run.host.reruns() == reruns => return Err("the connection survived the SAK"),
            Ok(_) => {}
            Err(_) => run.lost += 1,
        }
    }
    // The read above may have finished before the SAK landed, so it says nothing about
    // what recovery finds.
    if run.progress.held() && run.paste_through()? != value {
        return Err("the value moved after recovery");
    }
    run.step(|host, now| host.pump(now));
    if run.host.take_event().is_some() {
        return Err("an event no job accounts for");
    }
    if run.mode_refusals > 0 && !run.progress.held() {
        return Err("a mode report refused with no SAK to blame");
    }
    Ok(Report {
        held: run.progress.held(),
        bound_at_hold: run.bound_at_hold,
        down_after: run.down_after,
        submitted: run.progress.submitted(),
        copy,
        lost: run.lost,
        value,
    })
}
