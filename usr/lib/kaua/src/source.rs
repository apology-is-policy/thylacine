// kaua::source -- the event-loop's input side + the Loom seam (KAUA.md 4.4).
//
// `EventSource` abstracts "produce the next batch of Events"; v1.0 has exactly
// one implementation, `PollSource`, the LS-8c poll over fd 0 (the pollable cons,
// LS-8a). A future `LoomSource` (input as a multishot LOOM_OP_READ draining a
// CQ) implements the SAME trait with zero change to the output `Terminal` or any
// widget/app code -- that decoupling is the seam.
//
// Input lives HERE, separate from the output `Terminal`, precisely so the seam
// is real: swapping in a LoomSource replaces the input half without touching the
// diff->fd1 output half. kaua::input does the VT parsing; this is the fd 0 +
// poll glue around it. Backend-gated (the only-fd-touching layers, with term).
//
// The console read is #811 death-interruptible, so a dying app unwinds cleanly.

use alloc::vec::Vec;

use libthyla_rs::err::Result;
use libthyla_rs::io::{stdin, Read, Stdin};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};

use crate::event::Event;
use crate::input::Parser;

/// The reused fd-0 read chunk -- comfortably holds an input burst or a long
/// escape sequence within one read.
const READ_CHUNK: usize = 1024;

/// Max fd-0 reads drained into the parser within ONE `poll` -- a livelock bound
/// against an unbounded writer. On the cap we return WITHOUT flushing, so a
/// sequence still mid-assembly stays in the retained parser for the next `poll`
/// (never mis-keyed; #106-F2). 64 * READ_CHUNK = 64 KiB/round, far above any
/// real paste (the cons ring is 256 B).
const DRAIN_MAX: usize = 64;

/// ESC-disambiguation holdoff (#173). When a drain sweep leaves the parser
/// holding a bare ESC, the NEXT sweep waits up to this long (instead of polling
/// non-blocking) for the continuation byte -- so a split arrow `ESC | [B` (a
/// slow/dribbled HVF console, or a #172-batched RX IRQ, delivering the ESC head
/// alone in one read) assembles instead of mis-resolving the lone ESC to an
/// Escape key and mis-keying the tail (input.rs's documented residual). A true
/// lone Escape press pays this once before registering -- the standard terminal
/// ESC timeout (cf. vim ttimeoutlen). Bounded by the DRAIN_MAX sweep cap.
const ESC_HOLDOFF_MS: u32 = 50;

// F2 -- the pts readiness fd. A pts slave's DATA fd is not pollable: dev9p.poll
// reports POLLIN-always for it (kernel/dev9p_poll.c), so the console drain's
// re-poll of fd 0 never sees "not readable" and blocks in read() on an empty
// ring, batching keystrokes ("renders but not interactive"). The per-pts
// `/dev/pts/<n>ready` file (PTY item 10, a QTPOLL sibling) reports ACCURATE
// readiness; the app's mux polls THAT (via poll_fd) for the fd-0 wake, and this
// source then reads fd 0 ONCE trusting the mux -- a SINGLE poller of the
// one-shot/async ready cache (a second poller busy-loops it). Ported from
// libutopia console.rs::pts_slave_n_of_fd0 + repl.rs's item-10 ready open.
const PTS_QID_FLAG: u64 = 1 << 40;
const PTS_FK_SLAVE: u64 = 2;

/// If fd 0 is a pts SLAVE, open + return its `/dev/pts/<n>ready` fd. `None` on
/// the console / a pipe / a file / a pts master, or if the ready open fails --
/// the caller then polls fd 0 (the console path, unchanged). The two-gate
/// discrimination: S_IFCHR first (keeps a netd `/net` fd 0, which also carries
/// bit 40 but reports S_IFREG, out), then the qid flag + filekind.
fn pts_ready_fd() -> Option<i32> {
    let mut st = [0u8; 88]; // t_stat ABI is 88 bytes (#100)
    // SAFETY: t_fstat is the SYS_FSTAT wrapper; st is a valid 88-byte t_stat.
    if unsafe { libthyla_rs::t_fstat(0, st.as_mut_ptr()) } != 0 {
        return None;
    }
    let mut w = [0u8; 4];
    w.copy_from_slice(&st[40..44]); // t_stat.mode @40
    if (u32::from_le_bytes(w) & 0o170000) != 0o020000 {
        return None; // not a character device
    }
    let mut q = [0u8; 8];
    q.copy_from_slice(&st[8..16]); // t_stat.qid_path @8
    let qid = u64::from_le_bytes(q);
    if qid & PTS_QID_FLAG == 0 || (qid & 0xff) != PTS_FK_SLAVE {
        return None;
    }
    let n = ((qid >> 8) & 0xff_ffff) as u32;
    let rpath = alloc::format!("/dev/pts/{}ready", n);
    // SAFETY: t_open SVC wrapper; rpath is a valid NUL-free byte slice.
    let fd = unsafe {
        libthyla_rs::t_open(
            libthyla_rs::T_WALK_OPEN_FROM_ROOT,
            rpath.as_ptr(),
            rpath.len(),
            libthyla_rs::T_OREAD,
        )
    };
    if fd >= 0 {
        Some(fd as i32)
    } else {
        None
    }
}

/// The loop's event producer. One `poll` returns every Event decoded from the
/// bytes available this round (a single read can carry many keys). A future
/// `LoomSource` is the other implementation; the trait is the substitution seam.
pub trait EventSource {
    /// Block up to `timeout` for input and return the decoded Events. An empty
    /// Vec means the timeout elapsed with no event. I/O errors propagate.
    fn poll(&mut self, timeout: PollTimeout) -> Result<Vec<Event>>;

    /// The fd the app's mux should poll for fd-0 input readiness: the pollable
    /// cons itself (fd 0) on the console, or the `/dev/pts/<n>ready` sibling on a
    /// pts slave (F2 -- the pts data fd is POLLIN-always, so fd 0 is unpollable).
    /// The mux polls THIS; when it fires, the mux calls `poll`, which reads fd 0.
    /// Default 0 for sources without a distinct readiness fd.
    fn poll_fd(&self) -> i32 {
        0
    }

    /// True once the input stream reported EOF / HUP -- the loop's quit signal.
    fn is_eof(&self) -> bool;
}

/// The v1.0 `EventSource`: a `poll(2)` over fd 0 feeding the VT parser.
pub struct PollSource {
    poll: PollSet,
    inp: Stdin,
    parser: Parser,
    inbuf: [u8; READ_CHUNK],
    eof: bool,
    /// F2: the `/dev/pts/<n>ready` fd when fd 0 is a pts slave, else None (the
    /// console). Some(_) makes the drain poll THIS (accurate) instead of fd 0
    /// (POLLIN-always); the data is still read from fd 0. None keeps the fd-0
    /// drain (the cons poll is accurate).
    ready_fd: Option<i32>,
    /// F2: set when the APP's mux polls `poll_fd` itself and calls `poll` only
    /// on a fire (nora). A pts `poll` then reads fd 0 ONCE without re-polling the
    /// ready fd (a second poller busy-loops its one-shot cache). Unset
    /// (prowl/quarry, which call `poll` directly with a tick/block): a pts `poll`
    /// polls the ready fd itself, honoring the timeout.
    external_mux: bool,
    /// Bytes already pulled from fd 0 before the loop began (the launch
    /// size-probe's pre-reply type-ahead, kaua::query #117-F2) -- fed through
    /// the parser on the first `poll` so a keystroke typed at launch is not lost.
    pending: Vec<u8>,
}

impl PollSource {
    pub fn new() -> Self {
        Self::with_pending(Vec::new())
    }

    /// Construct a source that first replays `pending` (bytes read from fd 0
    /// before this source existed -- e.g. `kaua::query::terminal_size`'s
    /// pre-reply type-ahead) through the VT parser, then reads fd 0 as usual.
    pub fn with_pending(pending: Vec<u8>) -> Self {
        let ready_fd = pts_ready_fd();
        let mut poll = PollSet::new();
        // Poll the READINESS fd: fd 0 on the console (the pollable cons), or the
        // pts `/dev/pts/<n>ready` sibling on a hosted tile (fd 0 is POLLIN-always
        // there -- F2). The data is always read from fd 0 (self.inp).
        match ready_fd {
            Some(rfd) => poll.add_raw(rfd, PollEvents::READ),
            None => poll.add(&stdin(), PollEvents::READ),
        }
        PollSource {
            poll,
            inp: stdin(),
            parser: Parser::new(),
            inbuf: [0; READ_CHUNK],
            eof: false,
            ready_fd,
            external_mux: false,
            pending,
        }
    }

    /// Declare that the app's mux polls `poll_fd` and calls `poll` only when it
    /// fires (nora). A pts `poll` then reads fd 0 ONCE without re-polling the
    /// ready fd. Leave unset for an app that calls `poll` directly with its own
    /// tick/block timeout (prowl/quarry) -- a pts `poll` then polls the ready fd.
    pub fn set_external_mux(&mut self) {
        self.external_mux = true;
    }

    /// Replay the launch type-ahead (`pending`) into the parser and return its
    /// events WITHOUT reading fd 0. `poll` calls this first; a MUX app (nora)
    /// also calls it once BEFORE its loop, because its mux polls the pts ready fd
    /// -- which never fires for already-drained pending, so `poll` alone would
    /// strand a typed-ahead command until the next real keystroke (audit F1). Does
    /// NOT flush a trailing partial -- the retained parser continues it on the
    /// next read.
    pub fn drain_pending(&mut self) -> Vec<Event> {
        let mut out = Vec::new();
        if !self.pending.is_empty() {
            let bytes = core::mem::take(&mut self.pending);
            for b in bytes {
                if let Some(e) = self.parser.feed(b) {
                    out.push(Event::Key(e));
                }
                if let Some((c, r)) = self.parser.take_resize() {
                    out.push(Event::Resize(c, r));
                }
            }
        }
        out
    }
}

impl Default for PollSource {
    fn default() -> Self {
        Self::new()
    }
}

impl EventSource for PollSource {
    fn poll(&mut self, timeout: PollTimeout) -> Result<Vec<Event>> {
        // Replay any pre-loop type-ahead (kaua::query #117-F2) through the same
        // retained parser FIRST, no flush here (a VT sequence split between the
        // type-ahead tail and the first fd-0 read is assembled by the one parser
        // across both; the flush happens once, after the drain below). A mux app
        // (nora) has already drained this via drain_pending before its loop (F1),
        // so this is a no-op there.
        let mut out = self.drain_pending();

        // F2: pts slave WITH an external mux (nora) -- the mux polled `poll_fd`
        // (/dev/pts/<n>ready) and it fired, so fd 0 has data NOW; read it ONCE
        // here, SKIPPING the drain's first poll (which would return 0 -- the mux
        // consumed the one-shot ready cache -- and strand the data). Then FALL
        // THROUGH to the drain loop: it handles a burst remainder AND the #173 ESC
        // holdoff by polling the ready fd (a single poller now -- the app's mux is
        // not polling while it is inside this call), and does the single flush. No
        // flush/return here: a bare ESC from a dribbled RAW-forwarding pts (ptyhost
        // / pouch-pty, not kaua-term) must NOT resolve to Escape before its
        // continuation -- the drain's pending_escape holdoff catches it (audit F2).
        if self.ready_fd.is_some() && self.external_mux {
            let n = self.inp.read(&mut self.inbuf)?;
            if n == 0 {
                self.eof = true;
            }
            for &b in &self.inbuf[..n] {
                if let Some(e) = self.parser.feed(b) {
                    out.push(Event::Key(e));
                }
                if let Some((c, r)) = self.parser.take_resize() {
                    out.push(Event::Resize(c, r));
                }
            }
            // fall through to the drain loop (below) for any burst tail + the
            // #173 ESC holdoff + the single flush.
        }

        // Drain every byte immediately available on fd 0 into the single retained
        // parser before deciding a dangling ESC is a real Escape. A paste larger
        // than the 256 B cons ring arrives across several reads; flushing between
        // them would mis-key a sequence straddling a read boundary (#106-F2). The
        // first sweep blocks for `timeout` UNLESS the type-ahead already produced
        // events (then we only collect what is instantly ready -- never block on
        // top of work in hand). Later sweeps are non-blocking; `drained_dry` marks
        // a clean end (fd 0 not readable / EOF), the only point a lone ESC is real.
        // The fd the drain watches for readiness: the pts ready sibling, or fd 0
        // on the console. A pts here has no external mux (prowl/quarry), so this
        // source is the SOLE poller of the ready fd -- no cache-race.
        let rfd = self.ready_fd.unwrap_or(0);
        let mut drained_dry = false;
        for i in 0..DRAIN_MAX {
            let t = if self.parser.pending_escape() {
                // A bare ESC is pending -- it may be the head of a split arrow/
                // function-key sequence whose `[..` tail is still in transit. Hold
                // off briefly for it instead of declaring the drain dry and letting
                // flush() mis-resolve the ESC to an Escape key (#173). Checked
                // FIRST so the external_mux first read's bare ESC (audit F2) and a
                // prior DRAIN_MAX-cut ESC are held, not flushed, even at i==0.
                PollTimeout::Millis(ESC_HOLDOFF_MS)
            } else if i == 0 && out.is_empty() {
                timeout
            } else {
                PollTimeout::Zero
            };
            let mut readable = false;
            for ev in self.poll.poll(t)? {
                if ev.fd == rfd {
                    if ev.is_readable() {
                        readable = true;
                    }
                    if ev.is_hup() || ev.is_err() {
                        self.eof = true;
                    }
                }
            }
            if !readable {
                drained_dry = true;
                break;
            }
            let n = self.inp.read(&mut self.inbuf)?;
            if n == 0 {
                self.eof = true;
                drained_dry = true;
                break;
            }
            for &b in &self.inbuf[..n] {
                if let Some(e) = self.parser.feed(b) {
                    out.push(Event::Key(e));
                }
                // A recognized CPR (the launch size reply, or a late one the HVF
                // serial leaked past the probe) surfaces as a resize, never a key
                // (bug_nora_hvf_cpr_handshake).
                if let Some((c, r)) = self.parser.take_resize() {
                    out.push(Event::Resize(c, r));
                }
            }
        }

        // Resolve a dangling lone ESC to an Escape key ONLY once fd 0 is fully
        // drained -- no continuation byte is waiting, so it cannot be the head of
        // a split sequence. If the drain stopped on DRAIN_MAX (input still ready),
        // keep the parser's partial state for the next `poll` instead of guessing.
        if drained_dry {
            if let Some(e) = self.parser.flush() {
                out.push(Event::Key(e));
            }
        }

        Ok(out)
    }

    fn poll_fd(&self) -> i32 {
        self.ready_fd.unwrap_or(0)
    }

    fn is_eof(&self) -> bool {
        self.eof
    }
}
