//! The paste key's fetch over a private ring: the binary's half, as Nora's clip_host
//! is. One ring per paste. A shell sits at its prompt for days and most never paste,
//! and while a foreground command runs nothing here can pump, so nothing is held open
//! between pastes or into a command. No clipboard byte crosses the pts, and nothing
//! here can block.
use interaction_native::{parse_locator, Host};
use libhalcyon::interaction_client::{Event, Why};
use libthyla_rs::io::Write;
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::{env, loom::ThylaKernel, t_close, t_open, T_OPATH, T_WALK_OPEN_FROM_ROOT};
use libutopia::repl::Repl;

const MS: u64 = 1_000_000;

struct Fetch {
    host: Host<ThylaKernel>,
    /// The edit it was asked from. Dropping the host cancels the connection.
    edit: u64,
}

pub struct Clip {
    fetch: Option<Fetch>,
}

fn start() -> Option<Host<ThylaKernel>> {
    let value = env::var("HALCYON_INTERACTION")?;
    let locator = parse_locator(value.as_bytes())?;
    let path = b"/srv";
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OPATH) };
    if srv < 0 {
        return None;
    }
    let host = Host::new(ThylaKernel, srv as i32, locator);
    unsafe { t_close(srv) };
    host.ok()
}

fn failure(why: Why) -> &'static str {
    match why {
        Why::Refused(_) => "clipboard: the session refused the paste",
        Why::Lost => "clipboard: the connection went away, nothing pasted",
        Why::TimedOut => "clipboard: no answer in time, nothing pasted",
        Why::Malformed => "clipboard: a reply broke the protocol, nothing pasted",
        Why::Cancelled => "clipboard: cancelled",
    }
}

impl Clip {
    pub fn new() -> Self {
        Self { fetch: None }
    }

    pub fn poll_fd(&self) -> Option<i32> {
        self.fetch.as_ref().and_then(|f| f.host.poll_fd())
    }

    /// Block with no fetch out; otherwise only as long as the client owes nothing.
    pub fn poll_timeout(&self) -> PollTimeout {
        match self
            .fetch
            .as_ref()
            .and_then(|f| f.host.next_deadline().map(|at| (f, at)))
        {
            Some((f, at)) => {
                let left = at.saturating_sub(f.host.now()).div_ceil(MS);
                PollTimeout::Millis(left.min(u32::MAX as u64) as u32)
            }
            None => PollTimeout::Block,
        }
    }

    /// A line was accepted and is about to run: what it was typed under is over.
    pub fn line_accepted(&mut self) {
        self.fetch = None;
    }

    /// Once per turn of the loop, after any input: start what the key asked for, drop
    /// a fetch whose edit has ended, and run what the ring has ready.
    pub fn service(&mut self, woke: bool, repl: &mut Repl, out: &mut impl Write) {
        if self.fetch.as_ref().is_some_and(|f| f.edit != repl.edit()) {
            self.fetch = None;
        }
        if repl.take_paste_wanted() {
            self.ask(repl, out);
        }
        let Some(fetch) = self.fetch.as_mut() else {
            return;
        };
        let now = fetch.host.now();
        if woke || fetch.host.next_deadline().is_some_and(|at| at <= now) {
            fetch.host.pump(now);
        }
        let mut done = false;
        while let Some(event) = fetch.host.take_event() {
            match event {
                Event::Pasted(bytes) => {
                    repl.paste(&bytes, out);
                    done = true;
                }
                Event::PasteFailed(why) => {
                    repl.notice(failure(why), out);
                    done = true;
                }
                // This client only ever pastes, and it reports no mode.
                Event::Copied { .. }
                | Event::CopyUnknown
                | Event::CopyFailed(_)
                | Event::ModeRefused(_) => {}
            }
        }
        if done {
            self.fetch = None;
        }
    }

    fn ask(&mut self, repl: &mut Repl, out: &mut impl Write) {
        if self.fetch.is_some() {
            repl.notice("clipboard: still fetching the last paste", out);
            return;
        }
        let Some(mut host) = start() else {
            repl.notice("clipboard: not available here", out);
            return;
        };
        let now = host.now();
        match host.paste(now) {
            Ok(()) => {
                self.fetch = Some(Fetch {
                    host,
                    edit: repl.edit(),
                })
            }
            Err(_) => repl.notice("clipboard: refused before it started", out),
        }
    }
}
