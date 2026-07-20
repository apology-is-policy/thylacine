//! The persistent-child transport (Stage 8e-1c; behind the `backend` feature).
//!
//! The one platform-touching layer of `parley`: it spawns and drives a
//! long-lived language/debug server (`gopls`, `Ambush`) over piped stdio, and
//! multiplexes several such children plus the editor's own fd 0 in a single
//! `poll(2)`. This is the async substrate `docs/NORA-IDE-UX.md` section 6 calls
//! the load-bearing decision: Nora's one-shot-filter subprocess model
//! (`gofmt_source` -- write-all, close stdin, read to EOF, reap) deadlocks a
//! server that must stay alive across many request/response round-trips.
//!
//! **Mechanism only; policy stays in the caller** (the 8e-2 LSP / 8e-3 DAP
//! clients). A [`Server`] owns one child and streams framed messages to/from
//! it; a [`Mux`] blocks on a caller-supplied `(fd, tag)` set and reports which
//! fds woke. The json/frame/jsonrpc layers stay pure and host-testable; this
//! module needs libthyla-rs, so it is gated behind `backend` and proven end to
//! end by the in-guest `parley-probe` (spawn an echo child, frame-send a
//! request, poll, pump, decode, assert the round-trip).

use crate::frame::{self, Decoder, FrameError};
use crate::json::Value;
use alloc::string::ToString;
use alloc::vec::Vec;
use libthyla_rs::err::{Error, Result};
use libthyla_rs::io::{Read, Write};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::process::{Child, Command, ExitStatus, Stdio};

/// Bytes drained from a child's stdout per [`Server::pump`]. One `read(2)` per
/// readable report -- the poll re-arms while bytes remain buffered, so a second
/// read never blocks on a now-empty pipe.
const READ_CHUNK: usize = 16 * 1024;

/// Bytes drained (and discarded) from a child's stderr per
/// [`Server::drain_stderr`].
const STDERR_CHUNK: usize = 4 * 1024;

/// A persistent language/debug server: a spawned child with piped stdin (we
/// write `Content-Length`-framed requests), piped stdout (framed responses,
/// streamed through a [`Decoder`]), and piped stderr (drained + discarded so a
/// chatty server can neither scribble the editor's alt-screen nor wedge on a
/// full stderr pipe).
pub struct Server {
    child: Child,
    dec: Decoder,
    rbuf: Vec<u8>,
}

impl Server {
    /// Spawn `cmd` as a persistent server. The caller sets argv/caps on `cmd`;
    /// this forces all three stdio slots to `Piped` and keeps the pipes open
    /// for the child's lifetime.
    pub fn spawn(cmd: &mut Command) -> Result<Server> {
        cmd.stdin(Stdio::Piped).stdout(Stdio::Piped).stderr(Stdio::Piped);
        let child = cmd.spawn()?;
        Ok(Server { child, dec: Decoder::new(), rbuf: alloc::vec![0u8; READ_CHUNK] })
    }

    /// The child's pid.
    pub fn pid(&self) -> i32 {
        self.child.pid()
    }

    /// The child's stdout fd (register it in a [`Mux`]), or `-1` if stdout is
    /// closed.
    pub fn stdout_fd(&self) -> i32 {
        self.child.stdout.as_ref().map(|f| f.as_raw_fd()).unwrap_or(-1)
    }

    /// The child's stderr fd (register it in a [`Mux`]), or `-1` if stderr is
    /// closed.
    pub fn stderr_fd(&self) -> i32 {
        self.child.stderr.as_ref().map(|f| f.as_raw_fd()).unwrap_or(-1)
    }

    /// Serialize `msg` (compact JSON), frame it (`Content-Length`), and write
    /// the whole message to the child's stdin. `BrokenPipe` if stdin is closed.
    pub fn send(&mut self, msg: &Value) -> Result<()> {
        let wire = frame::encode(msg.to_string().as_bytes());
        let stdin = self.child.stdin.as_mut().ok_or(Error::BrokenPipe)?;
        stdin.write_all(&wire)
    }

    /// Read the bytes currently ready on stdout into the decoder (one `read(2)`).
    /// Call when a [`Mux`] reports [`stdout_fd`](Self::stdout_fd) readable, then
    /// drain complete messages with [`next_frame`](Self::next_frame). Returns
    /// `Ok(true)` on EOF (the child closed stdout / exited).
    pub fn pump(&mut self) -> Result<bool> {
        let stdout = match self.child.stdout.as_mut() {
            Some(s) => s,
            None => return Ok(true),
        };
        let n = stdout.read(&mut self.rbuf)?;
        if n == 0 {
            return Ok(true);
        }
        self.dec.push(&self.rbuf[..n]);
        Ok(false)
    }

    /// The next complete framed message body decoded so far. Loop it after each
    /// [`pump`](Self::pump) -- a single read can deliver several frames.
    /// `Ok(None)` means "need more bytes"; `Err` means the stream is malformed
    /// (tear the server down).
    pub fn next_frame(&mut self) -> core::result::Result<Option<Vec<u8>>, FrameError> {
        self.dec.next_frame()
    }

    /// Drain + discard one read of stderr (call when a [`Mux`] reports
    /// [`stderr_fd`](Self::stderr_fd) readable). Keeps a logging server from
    /// filling its stderr pipe and blocking on write. `Ok(true)` on stderr EOF.
    pub fn drain_stderr(&mut self) -> Result<bool> {
        let stderr = match self.child.stderr.as_mut() {
            Some(s) => s,
            None => return Ok(true),
        };
        let mut scratch = [0u8; STDERR_CHUNK];
        Ok(stderr.read(&mut scratch)? == 0)
    }

    /// Close the child's stdin so it observes EOF -- the graceful-shutdown
    /// signal for a well-behaved server. Pair with [`wait`](Self::wait).
    pub fn close_stdin(&mut self) {
        self.child.stdin = None;
    }

    /// Reap the child if it has already exited (non-blocking). Do not hot-loop
    /// this; poll a liveness source or fall back to [`wait`](Self::wait).
    pub fn try_wait(&mut self) -> Result<Option<ExitStatus>> {
        self.child.try_wait()
    }

    /// Block until the child exits and reap it. Pair with
    /// [`close_stdin`](Self::close_stdin) or [`kill`](Self::kill) so it is not a
    /// hang.
    pub fn wait(&mut self) -> Result<ExitStatus> {
        self.child.wait()
    }

    /// Terminate the child's thread-group (nora-exit cleanup / crash restart).
    /// Does not reap -- follow with [`wait`](Self::wait) or
    /// [`try_wait`](Self::try_wait).
    pub fn kill(&self) -> Result<()> {
        self.child.kill()
    }
}

/// A caller-chosen tag echoed back on readiness. The caller maps it to a
/// concrete source -- which [`Server`], stdout vs stderr, or fd 0. `parley`
/// supplies the multiplex mechanism; the client supplies the meaning.
pub type Tag = u32;

/// One registered fd the kernel reported an event on.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ready {
    pub tag: Tag,
    pub readable: bool,
    /// The peer hung up or errored (EOF / `POLLERR` / `POLLNVAL`) -- drain any
    /// final readable bytes, then treat the source as dead.
    pub hup: bool,
}

/// Multiplex fd 0 + N child stdio fds in one `poll(2)`. The `(fd, tag)` set is
/// passed to every [`poll`](Self::poll) and the internal [`PollSet`] is rebuilt
/// each call, so a restarted or closed server never leaves a stale fd
/// registered (the set is tiny -- fd 0 plus a couple of servers -- so the
/// rebuild is free and stale-fd bugs are structurally impossible).
pub struct Mux {
    poll: PollSet,
}

impl Default for Mux {
    fn default() -> Mux {
        Mux::new()
    }
}

impl Mux {
    pub fn new() -> Mux {
        Mux { poll: PollSet::new() }
    }

    /// Block (or time out) until at least one of `fds` is ready. Each entry is
    /// `(fd, tag)`; a negative fd (a dead source) is skipped. Returns one
    /// [`Ready`] per fd the kernel flagged, tagged for the caller to dispatch.
    /// An all-negative `fds` returns empty without blocking.
    pub fn poll(&mut self, fds: &[(i32, Tag)], timeout: PollTimeout) -> Result<Vec<Ready>> {
        self.poll.clear();
        for &(fd, _) in fds {
            if fd >= 0 {
                self.poll.add_raw(fd, PollEvents::READ);
            }
        }
        let mut out = Vec::new();
        for ev in self.poll.poll(timeout)? {
            if let Some(&(_, tag)) = fds.iter().find(|&&(fd, _)| fd == ev.fd) {
                out.push(Ready {
                    tag,
                    readable: ev.is_readable(),
                    hup: ev.is_hup() || ev.is_err(),
                });
            }
        }
        Ok(out)
    }
}
