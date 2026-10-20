// kaua::source -- the event loop's input side + the Loom seam (KAUA.md 4.4).
//
// `EventSource` abstracts "produce the next Event"; v1.0 has exactly one
// implementation, `PollSource`: a poll over fd 0 (the pollable cons, LS-8a, or a
// pts's ready sibling) and a one-byte read, driven by kaua::intake, which owns
// the decode and the reason for reading one byte at a time. A future
// `LoomSource` (input as a multishot LOOM_OP_READ draining a CQ) implements the
// SAME trait with zero change to the output `Terminal` or any widget/app code --
// that decoupling is the seam.
//
// Input lives HERE, separate from the output `Terminal`, precisely so the seam
// is real: swapping in a LoomSource replaces the input half without touching the
// diff->fd1 output half. Backend-gated (the only-fd-touching layers, with term).
//
// The console read is #811 death-interruptible, so a dying app unwinds cleanly.

use alloc::vec::Vec;

use libthyla_rs::err::{Error, Result};
use libthyla_rs::io::{stdin, Read, Stdin};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};

use crate::event::Event;
use crate::intake::{Burst, Fd0, Intake, Readiness, Wait, ESC_HOLDOFF_MS};

// F2 -- the pts readiness fd. A pts slave's DATA fd is not pollable: dev9p.poll
// reports POLLIN-always for it (kernel/dev9p_poll.c), so a readiness wait on
// fd 0 never sees "not readable" and the read blocks on an empty ring. The
// per-pts `/dev/pts/<n>ready` file (PTY item 10, a QTPOLL sibling) reports
// ACCURATE readiness, so the source waits on THAT and reads fd 0. Since #98 a
// zero-timeout poll of it answers from a fresh SNAPSHOT, never a cache, so the
// app's own mux and this source may both poll it (NoFalseNotReady /
// NoFalseReady, sub-kernel-ninep-dev9p-poll). Ported from libutopia
// console.rs::pts_slave_n_of_fd0 + repl.rs's item-10 ready open.
const PTS_QID_FLAG: u64 = 1 << 40;
const PTS_FK_SLAVE: u64 = 2;

/// fd 0's `/dev/pts/<n>ready` sibling, closed when dropped. The launch probe
/// (kaua::query) opens it and the source keeps it (`PollSource::with_probe`), so
/// a program holds one ready fid, the one ptyfs's per-pts budget counts.
pub(crate) struct ReadyFd(i32);

impl ReadyFd {
    pub(crate) fn raw(&self) -> i32 {
        self.0
    }
}

impl Drop for ReadyFd {
    fn drop(&mut self) {
        // SAFETY: t_close SVC wrapper on an fd this value alone owns.
        unsafe {
            libthyla_rs::t_close(self.0 as i64);
        }
    }
}

/// If fd 0 is a pts SLAVE, open its `/dev/pts/<n>ready` fd. `None` on the
/// console / a pipe / a file / a pts master -- the caller then polls fd 0 (the
/// console path, unchanged). The two-gate discrimination: S_IFCHR first (keeps a
/// netd `/net` fd 0, which also carries bit 40 but reports S_IFREG, out), then
/// the qid flag + filekind. A pts slave whose ready file will not open is also
/// `None`, and said on the diagnostic UART: fd 0 then polls as always readable,
/// so the app's timeout never lapses and a lone Escape waits for the next key.
pub(crate) fn pts_ready_fd() -> Option<ReadyFd> {
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
        return Some(ReadyFd(fd as i32));
    }
    // The errno cannot say why: a walk ptyfs refuses for a full fid table
    // reaches the caller as ENOENT, as a pts that has gone does.
    libthyla_rs::t_putstr(&alloc::format!(
        "kaua: {} will not open (the pts is gone, or ptyfs's fid table is full): fd 0 polls as always readable\n",
        rpath
    ));
    None
}

/// The loop's event producer.
pub trait EventSource {
    /// The next Event, waiting up to `timeout` for it; `None` when the timeout
    /// lapsed with none. Never reads fd 0 past the byte that completes it.
    fn next(&mut self, timeout: PollTimeout) -> Result<Option<Event>>;

    /// The fd an app's own mux should poll for fd-0 input readiness: the
    /// pollable cons itself (fd 0) on the console, or the `/dev/pts/<n>ready`
    /// sibling on a pts slave (F2). Default 0 for sources without a distinct
    /// readiness fd.
    fn poll_fd(&self) -> i32 {
        0
    }

    /// True once the input stream reported EOF / HUP -- the loop's quit signal.
    fn is_eof(&self) -> bool;
}

/// The v1.0 `EventSource`: a `poll(2)` over the readiness fd and a one-byte
/// read of fd 0, feeding kaua::intake.
pub struct PollSource {
    poll: PollSet,
    inp: Stdin,
    intake: Intake,
    /// The fd the readiness wait polls: the `/dev/pts/<n>ready` fd when fd 0 is
    /// a pts slave (F2), else 0 (the console). The data is always read from fd 0.
    rfd: i32,
    _ready: Option<ReadyFd>,
}

impl PollSource {
    /// A source for a program that ran no launch size probe.
    pub fn new() -> Self {
        Self::build(Vec::new(), pts_ready_fd())
    }

    /// Construct the source a launch size probe leads into: the bytes the probe
    /// read that were not its reply (`kaua::query::terminal_size`'s type-ahead)
    /// are replayed through the VT parser first, and on a pts the ready fd the
    /// probe polled is kept, not opened again.
    #[cfg(feature = "backend")]
    pub fn with_probe(probe: crate::query::ProbeResult) -> Self {
        Self::build(probe.pending, probe.ready)
    }

    fn build(pending: Vec<u8>, ready: Option<ReadyFd>) -> Self {
        let mut poll = PollSet::new();
        let rfd = match &ready {
            Some(r) => {
                poll.add_raw(r.raw(), PollEvents::READ);
                r.raw()
            }
            None => {
                poll.add(&stdin(), PollEvents::READ);
                0
            }
        };
        PollSource {
            poll,
            inp: stdin(),
            intake: Intake::new(pending),
            rfd,
            _ready: ready,
        }
    }

    /// One run of events to handle before painting: the first waits up to
    /// `timeout`, the rest take only what fd 0 already holds (kaua::intake::
    /// Burst). Stop pulling at an event that may end the app or hand the
    /// terminal to a child; nothing typed behind it has been read.
    pub fn burst(&mut self, timeout: PollTimeout) -> Burst<'_, Fd0Poll<'_>> {
        let fd = Fd0Poll {
            poll: &mut self.poll,
            inp: &mut self.inp,
            rfd: self.rfd,
            timeout,
        };
        Burst::new(&mut self.intake, fd)
    }
}

impl Default for PollSource {
    fn default() -> Self {
        Self::new()
    }
}

impl EventSource for PollSource {
    fn next(&mut self, timeout: PollTimeout) -> Result<Option<Event>> {
        let mut fd = Fd0Poll {
            poll: &mut self.poll,
            inp: &mut self.inp,
            rfd: self.rfd,
            timeout,
        };
        self.intake.next(&mut fd, Wait::Caller)
    }

    fn poll_fd(&self) -> i32 {
        self.rfd
    }

    fn is_eof(&self) -> bool {
        self.intake.is_eof()
    }
}

/// fd 0 as kaua::intake sees it: a readiness wait on the readiness fd, and a
/// one-byte read.
pub struct Fd0Poll<'a> {
    poll: &'a mut PollSet,
    inp: &'a mut Stdin,
    rfd: i32,
    timeout: PollTimeout,
}

impl Fd0 for Fd0Poll<'_> {
    type Error = Error;

    fn ready(&mut self, wait: Wait) -> Result<Readiness> {
        let t = match wait {
            Wait::Caller => self.timeout,
            Wait::Zero => PollTimeout::Zero,
            Wait::Holdoff => PollTimeout::Millis(ESC_HOLDOFF_MS),
        };
        let mut r = Readiness::default();
        for ev in self.poll.poll(t)? {
            if ev.fd == self.rfd {
                r.readable |= ev.is_readable();
                r.hup |= ev.is_hup() || ev.is_err();
            }
        }
        Ok(r)
    }

    fn read_byte(&mut self) -> Result<Option<u8>> {
        let mut b = [0u8; 1];
        Ok(match self.inp.read(&mut b)? {
            0 => None,
            _ => Some(b[0]),
        })
    }
}
