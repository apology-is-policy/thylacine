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
        // Replay any pre-loop type-ahead (kaua::query #117-F2) through the same
        // parser before touching fd 0. If it decodes to keys, return them this
        // round; if it decodes to nothing (a terminal-volunteered sequence),
        // fall through to a normal blocking poll (never return spuriously empty).
        if !self.pending.is_empty() {
            let mut out = Vec::new();
            let bytes = core::mem::take(&mut self.pending);
            for b in bytes {
                if let Some(e) = self.parser.feed(b) {
                    out.push(Event::Key(e));
                }
            }
            if let Some(e) = self.parser.flush() {
                out.push(Event::Key(e));
            }
            if !out.is_empty() {
                return Ok(out);
            }
        }

        let mut out = Vec::new();
        let mut readable = false;
        for ev in self.poll.poll(timeout)? {
            if ev.fd == 0 {
                if ev.is_readable() {
                    readable = true;
                }
                if ev.is_hup() || ev.is_err() {
                    self.eof = true;
                }
            }
        }
        if readable {
            let n = self.inp.read(&mut self.inbuf)?;
            if n == 0 {
                self.eof = true;
            }
            for &b in &self.inbuf[..n] {
                if let Some(e) = self.parser.feed(b) {
                    out.push(Event::Key(e));
                }
            }
            // The local console delivers each logical input within one ring
            // drain, so a dangling ESC at the chunk end is a real Escape key.
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
