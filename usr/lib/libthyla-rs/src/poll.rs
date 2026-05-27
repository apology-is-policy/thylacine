// libthyla-rs::poll — ergonomic wrapper around SYS_POLL.
//
// PollSet owns a Vec<TPollFd> and a parallel Vec of the user's
// "events" requests (so we can re-arm cleanly on each poll); poll()
// drains the kernel's revents back into a PollResults that callers
// iterate. AsFd is a tiny trait so kernel-object-backed types
// (Notes, File, future SrvConn) can drop into add() without exposing
// their raw fd publicly.
//
// Foundation chunk: U-2e per docs/UTOPIA-SHELL-DESIGN.md §15.6.7.
// Backs the shell's main loop (the canonical Thylacine concurrency
// model — every blocking op routes through one poll()).
//
// CAPACITY:
//   The kernel bounds nfds at PROC_HANDLE_MAX = 64. add() does not
//   enforce that at runtime; the kernel rejects > 64 with -1, which
//   becomes Error::InvalidArgument on poll(). A v1.x extension could
//   enforce capacity at add() if needed.
//
// RE-ARM:
//   t_poll writes revents into TPollFd.revents in place; subsequent
//   calls would see the previous revents stuck on the same record.
//   PollSet::poll re-arms events from a separately-tracked vector
//   before each kernel call so callers get clean semantics.

use crate::err::{Error, Result};
use crate::{t_poll, TPollFd, T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLNVAL, T_POLLOUT};
use alloc_crate::vec::Vec;

// =============================================================================
// AsFd — abstraction over kernel-object-backed types that have a raw fd.
// =============================================================================

/// Anything that wraps a kernel handle and can yield its raw index.
///
/// `t::fs::File`, `t::notes::Notes`, future `t::ninep::Client` all
/// impl `AsFd`; PollSet::add accepts them by reference without
/// requiring callers to reach into the wrapper internals.
///
/// The returned `i32` is a handle-table index in the calling Proc's
/// table. Borrowing the wrapper (`&self`) ensures the handle is alive
/// for the duration of the call.
pub trait AsFd {
    /// Yield the underlying handle-table index.
    fn as_raw_fd(&self) -> i32;
}

// =============================================================================
// PollEvents — bitmask of requested / observed events on a fd.
// =============================================================================

/// Bitmask of poll events. Mirrors `T_POLL*` constants but typed.
///
/// Compose with `|`; subtract with `without`; query with `contains`.
/// READ / WRITE are the request bits (and also report bits — POLLIN
/// means "data ready" both as a request and an observation). ERROR /
/// HUP / NVAL are kernel-fill-only on revents — they don't need to
/// be requested.
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub struct PollEvents(i16);

impl PollEvents {
    /// No events.
    pub const NONE: PollEvents = PollEvents(0);

    /// `POLLIN` — data ready to read (a SYS_READ wouldn't block).
    pub const READ: PollEvents = PollEvents(T_POLLIN);

    /// `POLLOUT` — write space available (a SYS_WRITE wouldn't block).
    pub const WRITE: PollEvents = PollEvents(T_POLLOUT);

    /// `POLLERR` — kernel-fill on a fd that errored. Implicit: the
    /// kernel raises this without callers having to request it.
    pub const ERROR: PollEvents = PollEvents(T_POLLERR);

    /// `POLLHUP` — kernel-fill on a fd whose peer hung up. Implicit.
    pub const HUP: PollEvents = PollEvents(T_POLLHUP);

    /// `POLLNVAL` — kernel-fill on an invalid fd (e.g., closed).
    /// Implicit.
    pub const NVAL: PollEvents = PollEvents(T_POLLNVAL);

    /// Construct from raw kernel bits.
    #[inline]
    #[must_use]
    pub const fn from_bits(bits: i16) -> Self {
        Self(bits)
    }

    /// Raw bits.
    #[inline]
    #[must_use]
    pub const fn bits(self) -> i16 {
        self.0
    }

    /// `true` iff every event in `other` is set in `self`.
    #[inline]
    #[must_use]
    pub const fn contains(self, other: PollEvents) -> bool {
        self.0 & other.0 == other.0
    }

    /// `true` iff `self` and `other` share at least one bit.
    #[inline]
    #[must_use]
    pub const fn intersects(self, other: PollEvents) -> bool {
        self.0 & other.0 != 0
    }

    /// `true` iff no event bits are set.
    #[inline]
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// `self` minus `other`.
    #[inline]
    #[must_use]
    pub const fn without(self, other: PollEvents) -> Self {
        Self(self.0 & !other.0)
    }
}

impl core::ops::BitOr for PollEvents {
    type Output = Self;
    #[inline]
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for PollEvents {
    type Output = Self;
    #[inline]
    fn bitand(self, rhs: Self) -> Self {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::BitOrAssign for PollEvents {
    #[inline]
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

// =============================================================================
// PollTimeout — Block / Zero / Millis(u32).
// =============================================================================

/// How long PollSet::poll waits when no fd is ready.
#[derive(Copy, Clone, Debug)]
pub enum PollTimeout {
    /// Block until a fd becomes ready (kernel's `timeout_ms == -1`).
    Block,
    /// Return immediately after the first scan
    /// (kernel's `timeout_ms == 0`). Useful for `try_read`-style
    /// polling.
    Zero,
    /// Wait up to N milliseconds. Capped at `i32::MAX` at the syscall
    /// boundary; larger values saturate.
    Millis(u32),
}

impl PollTimeout {
    #[inline]
    fn to_kernel_ms(self) -> i32 {
        match self {
            PollTimeout::Block => -1,
            PollTimeout::Zero => 0,
            PollTimeout::Millis(ms) => {
                if ms > i32::MAX as u32 {
                    i32::MAX
                } else {
                    ms as i32
                }
            }
        }
    }
}

// =============================================================================
// PollSet — the ergonomic wrapper.
// =============================================================================

/// A reusable set of poll-able file descriptors.
///
/// Add fds with `add` (or `add_raw` for an i32 you already hold);
/// call `poll` to block until at least one is ready. The returned
/// `PollResults` iterates `(fd, revents)` pairs for fds whose
/// `revents` is non-zero.
///
/// Re-arms each fd's `events` request before every kernel call so
/// callers don't see leaked `revents` between iterations.
///
/// Bound: the kernel allows up to PROC_HANDLE_MAX (= 64) fds per
/// poll. PollSet doesn't enforce that at add() time; the kernel
/// rejects with -1 on poll(), surfacing as Error::InvalidArgument.
pub struct PollSet {
    // Kernel-side pollfds. Updated in place by every poll() call;
    // re-armed from `events` before each call.
    pollfds: Vec<TPollFd>,
    // Parallel events-requested vector; PollSet::poll restores
    // pollfds[i].events from this before each kernel call (the
    // kernel may have populated revents but doesn't otherwise
    // mutate events).
    events: Vec<i16>,
}

impl PollSet {
    /// Construct an empty set.
    #[inline]
    #[must_use]
    pub fn new() -> Self {
        Self {
            pollfds: Vec::new(),
            events: Vec::new(),
        }
    }

    /// Construct an empty set with capacity for `n` fds.
    #[inline]
    #[must_use]
    pub fn with_capacity(n: usize) -> Self {
        Self {
            pollfds: Vec::with_capacity(n),
            events: Vec::with_capacity(n),
        }
    }

    /// Number of fds currently in the set.
    #[inline]
    #[must_use]
    pub fn len(&self) -> usize {
        self.pollfds.len()
    }

    /// `true` iff no fds are tracked.
    #[inline]
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.pollfds.is_empty()
    }

    /// Register `fd` to be observed for `events`. If `fd` is already
    /// present, the requested events are OR'd with the prior request
    /// (additive — common idiom is to add READ first and add WRITE
    /// later only while a write is pending).
    pub fn add<F: AsFd + ?Sized>(&mut self, fd: &F, events: PollEvents) {
        self.add_raw(fd.as_raw_fd(), events);
    }

    /// Register a raw handle-table index. Same semantics as `add`
    /// but takes an i32 directly — for callers holding an fd they
    /// don't own a typed wrapper for.
    pub fn add_raw(&mut self, fd: i32, events: PollEvents) {
        for (i, p) in self.pollfds.iter_mut().enumerate() {
            if p.fd == fd {
                let merged = p.events | events.bits();
                p.events = merged;
                self.events[i] = merged;
                return;
            }
        }
        self.pollfds.push(TPollFd {
            fd,
            events: events.bits(),
            revents: 0,
        });
        self.events.push(events.bits());
    }

    /// Stop tracking `fd`. Idempotent: removing a fd not in the set
    /// is a no-op.
    pub fn remove<F: AsFd + ?Sized>(&mut self, fd: &F) {
        self.remove_raw(fd.as_raw_fd());
    }

    /// Stop tracking a raw fd. Idempotent.
    pub fn remove_raw(&mut self, fd: i32) {
        if let Some(i) = self.pollfds.iter().position(|p| p.fd == fd) {
            self.pollfds.swap_remove(i);
            self.events.swap_remove(i);
        }
    }

    /// Drop every fd from the set.
    pub fn clear(&mut self) {
        self.pollfds.clear();
        self.events.clear();
    }

    /// Block (subject to `timeout`) until at least one tracked fd is
    /// ready. Returns a `PollResults` that iterates `(fd, revents)`
    /// pairs for fds with `revents != 0`.
    ///
    /// Empty set: returns Ok with a results iterator over zero fds
    /// immediately (the kernel would reject nfds == 0 with -1; we
    /// short-circuit to a more useful "nothing to do").
    ///
    /// Errors:
    ///   - `Error::InvalidArgument` if the kernel rejects (nfds > 64,
    ///     fds out of user-VA, or other -1 return).
    pub fn poll(&mut self, timeout: PollTimeout) -> Result<PollResults<'_>> {
        if self.pollfds.is_empty() {
            return Ok(PollResults {
                pollfds: &self.pollfds,
                pos: 0,
                remaining: 0,
            });
        }
        for (i, p) in self.pollfds.iter_mut().enumerate() {
            p.events = self.events[i];
            p.revents = 0;
        }
        let timeout_ms = timeout.to_kernel_ms();
        // SAFETY: pollfds.as_mut_ptr() is a writable user-VA pointer
        // to pollfds.len() consecutive TPollFd records (Vec invariant).
        let nready = unsafe {
            t_poll(
                self.pollfds.as_mut_ptr(),
                self.pollfds.len(),
                timeout_ms,
            )
        };
        if nready < 0 {
            return Err(Error::InvalidArgument);
        }
        Ok(PollResults {
            pollfds: &self.pollfds,
            pos: 0,
            remaining: nready as usize,
        })
    }
}

impl Default for PollSet {
    fn default() -> Self {
        Self::new()
    }
}

// =============================================================================
// PollResults — iterates fds with non-zero revents.
// =============================================================================

/// Iterator over poll-ready fds. Yields `(fd, revents)` for every
/// TPollFd whose `revents != 0`.
pub struct PollResults<'a> {
    pollfds: &'a [TPollFd],
    pos: usize,
    remaining: usize,
}

impl<'a> Iterator for PollResults<'a> {
    type Item = PollEvent;

    fn next(&mut self) -> Option<PollEvent> {
        if self.remaining == 0 {
            return None;
        }
        while self.pos < self.pollfds.len() {
            let p = &self.pollfds[self.pos];
            self.pos += 1;
            if p.revents != 0 {
                self.remaining -= 1;
                return Some(PollEvent {
                    fd: p.fd,
                    revents: PollEvents::from_bits(p.revents),
                });
            }
        }
        None
    }
}

/// One ready fd from a poll() result.
#[derive(Copy, Clone, Debug)]
pub struct PollEvent {
    /// The handle-table index that became ready.
    pub fd: i32,
    /// The events the kernel reported.
    pub revents: PollEvents,
}

impl PollEvent {
    /// Convenience: data ready to read.
    #[inline]
    #[must_use]
    pub fn is_readable(self) -> bool {
        self.revents.contains(PollEvents::READ)
    }

    /// Convenience: write space available.
    #[inline]
    #[must_use]
    pub fn is_writable(self) -> bool {
        self.revents.contains(PollEvents::WRITE)
    }

    /// Convenience: peer hung up.
    #[inline]
    #[must_use]
    pub fn is_hup(self) -> bool {
        self.revents.contains(PollEvents::HUP)
    }

    /// Convenience: fd errored or invalidated.
    #[inline]
    #[must_use]
    pub fn is_err(self) -> bool {
        self.revents.intersects(PollEvents::ERROR | PollEvents::NVAL)
    }
}
