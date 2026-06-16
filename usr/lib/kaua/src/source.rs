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

/// The loop's event producer. One `poll` returns every Event decoded from the
/// bytes available this round (a single read can carry many keys). A future
/// `LoomSource` is the other implementation; the trait is the substitution seam.
pub trait EventSource {
    /// Block up to `timeout` for input and return the decoded Events. An empty
    /// Vec means the timeout elapsed with no event. I/O errors propagate.
    fn poll(&mut self, timeout: PollTimeout) -> Result<Vec<Event>>;

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
        let mut poll = PollSet::new();
        poll.add(&stdin(), PollEvents::READ);
        PollSource {
            poll,
            inp: stdin(),
            parser: Parser::new(),
            inbuf: [0; READ_CHUNK],
            eof: false,
            pending,
        }
    }
}

impl Default for PollSource {
    fn default() -> Self {
        Self::new()
    }
}

impl EventSource for PollSource {
    fn poll(&mut self, timeout: PollTimeout) -> Result<Vec<Event>> {
        let mut out = Vec::new();

        // Replay any pre-loop type-ahead (kaua::query #117-F2) through the same
        // retained parser FIRST, but do NOT flush here: a VT sequence split
        // between the type-ahead tail and the first fd-0 read is assembled by the
        // one parser across both. The flush happens once, after the drain below.
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

        // Drain every byte immediately available on fd 0 into the single retained
        // parser before deciding a dangling ESC is a real Escape. A paste larger
        // than the 256 B cons ring arrives across several reads; flushing between
        // them would mis-key a sequence straddling a read boundary (#106-F2). The
        // first sweep blocks for `timeout` UNLESS the type-ahead already produced
        // events (then we only collect what is instantly ready -- never block on
        // top of work in hand). Later sweeps are non-blocking; `drained_dry` marks
        // a clean end (fd 0 not readable / EOF), the only point a lone ESC is real.
        let mut drained_dry = false;
        for i in 0..DRAIN_MAX {
            let t = if i == 0 && out.is_empty() {
                timeout
            } else if self.parser.pending_escape() {
                // A bare ESC is pending -- it may be the head of a split arrow/
                // function-key sequence whose `[..` tail is still in transit.
                // Hold off briefly for it instead of declaring the drain dry and
                // letting flush() mis-resolve the ESC to an Escape key (#173).
                PollTimeout::Millis(ESC_HOLDOFF_MS)
            } else {
                PollTimeout::Zero
            };
            let mut readable = false;
            for ev in self.poll.poll(t)? {
                if ev.fd == 0 {
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

    fn is_eof(&self) -> bool {
        self.eof
    }
}
