// libthyla-rs::notes — Thylacine note delivery, fd-shaped.
//
// `Notes` is RAII over a SYS_NOTE_OPEN fd. `read` blocks for the next
// deliverable note; `try_read` returns immediately if the queue is
// empty. `send` (free fn) posts a note to self or to a child Proc.
// `mask` / `with_mask` adjust the calling Thread's note_mask (Plan 9
// signal-blocking semantics, per-Thread).
//
// Foundation chunk: U-2e per docs/UTOPIA-SHELL-DESIGN.md §15.6.6.
// Designed for the shell's main loop, which polls one notes fd per
// child plus its own and dispatches synchronously when the kernel
// wakes us. Mask-then-poll is safe — kernel post-then-wake serializes
// under the queue lock; sub-chunk 13a audited the wakeup contract.
//
// FD vs HANDLER:
//   The async-handler path (SYS_NOTIFY / SYS_NOTED) is the libc-compat
//   opt-in (pouch wraps that path for musl). Native Thylacine programs
//   should use the fd-shaped path here — no async-cancel-safety hell,
//   no signal-restart torture, the same poll() loop everything else
//   uses. NOVEL.md §3.1 documents the choice.
//
// PERMISSION GATE:
//   `send` accepts NoteTarget::SelfProc unconditionally and
//   NoteTarget::Pid(p) only when p is a child of the calling Proc.
//   v1.0 has no CAP_KILL; v1.x will extend the gate.
//
// `snare:` PREFIX:
//   `snare:*` names are reserved for kernel-synthetic fault posters.
//   The kernel rejects userspace SYS_POSTNOTE with a `snare:`-prefixed
//   name; libthyla-rs surfaces that rejection cleanly via
//   Error::InvalidArgument.

use crate::err::{Error, Result};
use crate::handle::{Handle, Rights};
use crate::poll::AsFd;
use crate::{
    t_note_mask, t_note_open, t_postnote, t_read, t_poll, TNoteRecord, TPollFd,
    T_NOTE_BIT_CHILD_EXIT, T_NOTE_BIT_INTERRUPT, T_NOTE_BIT_KILL, T_NOTE_BIT_PIPE,
    T_NOTE_BIT_SNARE, T_NOTE_NAME_MAX, T_POLLIN, T_POSTNOTE_SELF_PID,
};
use alloc_crate::string::String;
use core::mem;

/// Process identifier. Mirrors the kernel's `pid_t` (i32).
pub type Pid = i32;

// =============================================================================
// Note + NoteTarget + NoteClass.
// =============================================================================

/// One note delivered to the calling Proc.
///
/// `name` is the note class (e.g. "interrupt", "kill", "child_exit",
/// "pipe", or a user-defined name). `arg` is a small int slot that
/// kernel-synthetic posters use to pack additional data — `child_exit`
/// packs `(child_pid << 16) | (status & 0xffff)`; user-posted notes
/// carry `arg == 0` at v1.0 (SYS_POSTNOTE doesn't accept an arg yet).
/// `from` is the sender's pid, or `None` if the kernel synthesized
/// the note (proc-exit, pipe-EPIPE).
#[derive(Clone, Debug)]
pub struct Note {
    /// Note name; mask-routable via `NoteClass`.
    pub name: String,
    /// Sender-supplied or kernel-packed argument.
    pub arg: u32,
    /// Sender's pid, or None for kernel-synthetic.
    pub from: Option<Pid>,
    /// Monotonic kernel time at post.
    pub timestamp_ns: u64,
}

impl Note {
    /// `true` iff this note's name is `"kill"`. `kill` is the special
    /// non-catchable note; the kernel removes it from fd consumption
    /// in `notes_dequeue_for_fd_locked` (R2-F1 audit), so a Note
    /// observed through `Notes::read` is NEVER a kill — but `is_kill`
    /// is still useful for code that consumes the kernel's fd via
    /// some other route (`stratumd` byte-mode SrvConn, future
    /// `t::process::Child::wait_kill_or_exit`).
    #[must_use]
    pub fn is_kill(&self) -> bool {
        self.name == "kill"
    }

    /// `true` iff the name has the `snare:` prefix (kernel-synthetic
    /// fault family — segv / bus / align / bti / brk / ill / fpe).
    /// Notes::send rejects userspace posts of `snare:*` at the
    /// kernel boundary; this predicate is for read-side classification.
    #[must_use]
    pub fn is_snare(&self) -> bool {
        self.name.starts_with("snare:")
    }
}

/// Who to send a note to.
///
/// At v1.0, `Pid(p)` is only allowed if `p` is a child of the calling
/// Proc OR equals the caller's own pid. SelfProc is the canonical
/// self-post (maps to the kernel's `pid == 0` self-post sentinel).
/// Future v1.x adds process groups + CAP_KILL.
#[derive(Copy, Clone, Debug)]
pub enum NoteTarget {
    /// Send the note to the calling Proc. Always allowed.
    SelfProc,
    /// Send to a specific Proc by pid. Caller must be the target's
    /// parent or be the target itself.
    Pid(Pid),
}

impl NoteTarget {
    #[inline]
    fn to_kernel_pid(self) -> i64 {
        match self {
            NoteTarget::SelfProc => T_POSTNOTE_SELF_PID,
            NoteTarget::Pid(p) => p as i64,
        }
    }
}

/// One of the kernel's mask-routable note classes. Surfaces the
/// `NOTE_BIT_*` positions as a typed enum so callers don't pass raw
/// integers around. Bit position is exposed via `bit()`.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum NoteClass {
    /// `interrupt` — Ctrl-C / SIGINT analogue.
    Interrupt,
    /// `kill` — non-catchable; the mask bit is reserved but the
    /// kernel ignores it for kill delivery (N-4 invariant).
    Kill,
    /// `pipe` — write-to-closed analogue (SIGPIPE).
    Pipe,
    /// `child_exit` — a child Proc reaped (SIGCHLD analogue).
    ChildExit,
    /// `snare:*` family — kernel-synthetic fault notes. The mask bit
    /// is reserved at v1.0 (proc_fault_terminate bypasses notes_post)
    /// and becomes load-bearing in v1.x.
    Snare,
}

impl NoteClass {
    /// The `NOTE_BIT_*` bit position for this class.
    #[inline]
    #[must_use]
    pub const fn bit(self) -> u8 {
        match self {
            NoteClass::Interrupt => T_NOTE_BIT_INTERRUPT,
            NoteClass::Kill => T_NOTE_BIT_KILL,
            NoteClass::Pipe => T_NOTE_BIT_PIPE,
            NoteClass::ChildExit => T_NOTE_BIT_CHILD_EXIT,
            NoteClass::Snare => T_NOTE_BIT_SNARE,
        }
    }
}

// =============================================================================
// NoteMask — bitflag set of NoteClass.
// =============================================================================

/// Per-Thread mask of deferred notes.
///
/// Bit set = the corresponding class is deferred (held back from
/// delivery). Compose with `|`; query with `contains`; subtract with
/// `without`. POSIX `pthread_sigmask` semantics: the mask is per-
/// Thread, NOT per-Proc. Multi-thread Procs (Phase 6 sub-chunk 9a)
/// can have different threads accept different notes.
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub struct NoteMask(u64);

impl NoteMask {
    /// Empty mask — every note is delivered.
    pub const NONE: NoteMask = NoteMask(0);

    /// Construct from raw bits.
    #[inline]
    #[must_use]
    pub const fn from_bits(bits: u64) -> Self {
        Self(bits)
    }

    /// Raw bits.
    #[inline]
    #[must_use]
    pub const fn bits(self) -> u64 {
        self.0
    }

    /// Construct a mask containing one class.
    #[inline]
    #[must_use]
    pub const fn just(class: NoteClass) -> Self {
        Self(1u64 << class.bit() as u32)
    }

    /// `true` iff `class` is deferred by this mask.
    #[inline]
    #[must_use]
    pub const fn contains(self, class: NoteClass) -> bool {
        self.0 & (1u64 << class.bit() as u32) != 0
    }

    /// `true` iff every bit in `other` is set in `self`.
    #[inline]
    #[must_use]
    pub const fn contains_mask(self, other: NoteMask) -> bool {
        self.0 & other.0 == other.0
    }

    /// `true` iff no classes are deferred.
    #[inline]
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// Add `class` to the deferred set.
    #[inline]
    #[must_use]
    pub const fn with(self, class: NoteClass) -> Self {
        Self(self.0 | (1u64 << class.bit() as u32))
    }

    /// Remove `class` from the deferred set.
    #[inline]
    #[must_use]
    pub const fn without(self, class: NoteClass) -> Self {
        Self(self.0 & !(1u64 << class.bit() as u32))
    }
}

impl core::ops::BitOr for NoteMask {
    type Output = Self;
    #[inline]
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for NoteMask {
    type Output = Self;
    #[inline]
    fn bitand(self, rhs: Self) -> Self {
        Self(self.0 & rhs.0)
    }
}

impl From<NoteClass> for NoteMask {
    #[inline]
    fn from(class: NoteClass) -> Self {
        NoteMask::just(class)
    }
}

// =============================================================================
// Notes — RAII handle on the per-Proc notes fd.
// =============================================================================

/// A handle on the calling Proc's note queue.
///
/// `open_self` mints a fresh fd; multiple `Notes` open against the
/// same Proc share the same kernel-side queue (N-5 invariant — the
/// queue lives with the Proc, not the fd). Closing one fd doesn't
/// affect notes the queue still holds.
///
/// Implements `AsFd` for use with `t::poll::PollSet`.
pub struct Notes {
    handle: Handle,
}

impl Notes {
    /// Open the calling Proc's note queue. Mints a fresh handle table
    /// slot; multiple opens are independent fds against the same
    /// underlying queue.
    ///
    /// Errors:
    ///   - `Error::OutOfRange`: handle table is full.
    ///   - `Error::InvalidArgument`: defense-in-depth on a kernel
    ///     return value that doesn't fit in i32 (should not happen).
    pub fn open_self() -> Result<Notes> {
        // SAFETY: SYS_NOTE_OPEN takes no args and only mutates the
        // calling Proc's handle table.
        let rc = unsafe { t_note_open() };
        if rc < 0 {
            // Kernel collapses every failure to -1 at v1.0; "handle
            // table full" is the only realistic cause (devnotes_open
            // is structurally infallible). Surface as OutOfRange
            // until the kernel exposes a finer-grained errno.
            return Err(Error::OutOfRange);
        }
        if rc > i32::MAX as i64 {
            return Err(Error::InvalidArgument);
        }
        // The kernel grants RIGHT_READ on the minted Spoor (the read
        // side of the note queue). We mirror that here.
        Ok(Notes {
            handle: Handle::from_raw(rc as i32, Rights::READ),
        })
    }

    /// Block until the next deliverable note arrives, then return it.
    ///
    /// Errors:
    ///   - `Error::UnexpectedEof`: the kernel returned a 0-byte read
    ///     (fd EOF; only happens if the Proc is in tear-down).
    ///   - `Error::InvalidArgument`: kernel returned a short read
    ///     other than 0 (defense-in-depth; ABI is one record per
    ///     read at v1.0).
    pub fn read(&self) -> Result<Note> {
        let mut buf = [0u8; mem::size_of::<TNoteRecord>()];
        // SAFETY: buf is writable for size_of::<TNoteRecord>() bytes
        // in our own stack frame.
        let n = unsafe { t_read(self.handle.raw() as i64, buf.as_mut_ptr(), buf.len()) };
        if n < 0 {
            // t_read returns -1 on bad fd / bad buf / fault. Map
            // through the canonical syscall-return decoder so the
            // errno is preserved.
            let decoded = Error::from_syscall_return(n);
            return Err(decoded.err().unwrap_or(Error::Io));
        }
        if n == 0 {
            return Err(Error::UnexpectedEof);
        }
        if (n as usize) != buf.len() {
            return Err(Error::InvalidArgument);
        }
        Ok(parse_note_record(&buf))
    }

    /// Return the next note if one is immediately available; `None`
    /// if the queue is empty.
    ///
    /// Uses `poll(timeout=0)` to probe readiness without blocking;
    /// if POLLIN is set, follows with a `read`. Errors propagate
    /// from the underlying read.
    pub fn try_read(&self) -> Result<Option<Note>> {
        let mut pollfd = TPollFd {
            fd: self.handle.raw(),
            events: T_POLLIN,
            revents: 0,
        };
        // SAFETY: pollfd is a single writable TPollFd in our stack frame.
        let nready = unsafe { t_poll(&mut pollfd as *mut TPollFd, 1, 0) };
        if nready < 0 {
            return Err(Error::InvalidArgument);
        }
        if nready == 0 {
            return Ok(None);
        }
        if (pollfd.revents & T_POLLIN) == 0 {
            // Some other event (ERR / HUP / NVAL) — treat as empty
            // queue at this layer; a subsequent read() would surface
            // the underlying error.
            return Ok(None);
        }
        self.read().map(Some)
    }
}

impl AsFd for Notes {
    #[inline]
    fn as_raw_fd(&self) -> i32 {
        self.handle.raw()
    }
}

// =============================================================================
// Free functions — send + mask.
// =============================================================================

/// Send a note to `target` with class `name`.
///
/// `name` is 1..=15 bytes of printable ASCII (`0x20..=0x7e`); the
/// kernel rejects empty, oversized, non-printable, or NUL-bearing
/// names. The `snare:` prefix is reserved for kernel-synthetic
/// posters; userspace `send` with a `snare:`-prefixed name returns
/// `Error::InvalidArgument`.
///
/// Permission gate (v1.0): SelfProc always allowed; `Pid(p)`
/// requires the caller to be the target's parent OR equal to `p`.
///
/// Errors:
///   - `Error::InvalidArgument`: bad name (length / chars / `snare:`
///     prefix) OR target name not in the kernel's supported set OR
///     bad pid.
///   - `Error::NotFound`: target pid does not exist.
///   - `Error::PermissionDenied`: caller is not the target's parent.
///   - `Error::Again`: target's note queue is full (best-effort
///     retry; coalesce is kernel-synthetic-only).
pub fn send(target: NoteTarget, name: &str) -> Result<()> {
    if name.is_empty() {
        return Err(Error::InvalidArgument);
    }
    if name.len() >= T_NOTE_NAME_MAX {
        return Err(Error::InvalidArgument);
    }
    if name.starts_with("snare:") {
        return Err(Error::InvalidArgument);
    }
    let bytes = name.as_bytes();
    // SAFETY: bytes points to name.len() readable bytes in the
    // caller's user-VA memory (a &str borrows that).
    let rc = unsafe { t_postnote(target.to_kernel_pid(), bytes.as_ptr(), bytes.len()) };
    if rc < 0 {
        // The kernel collapses every failure to -1 at v1.0; we
        // surface as InvalidArgument since we can't distinguish.
        // A v1.x kernel ABI upgrade (per docs/ERRORS.md) will return
        // specific errno values we can map here.
        return Err(Error::InvalidArgument);
    }
    Ok(())
}

/// Set the calling Thread's note mask and return the previous mask.
///
/// Bits outside `T_NOTE_MASK_SUPPORTED` are tolerated (no-op) so
/// future kernel-known notes don't break old userspace.
///
/// Errors:
///   - `Error::InvalidArgument`: should not happen — SYS_NOTE_MASK
///     can only fail if `old_mask_out_va` is bogus, and we pass our
///     own stack pointer.
pub fn set_mask(mask: NoteMask) -> Result<NoteMask> {
    let mut old: u64 = 0;
    // SAFETY: &mut old is a writable u64 in our stack frame.
    let rc = unsafe { t_note_mask(mask.bits(), &mut old as *mut u64) };
    if rc < 0 {
        return Err(Error::InvalidArgument);
    }
    Ok(NoteMask::from_bits(old))
}

/// Apply `mask` and return a guard that restores the previous mask
/// when dropped. Idiomatic for "block these notes for the duration of
/// this scope" — e.g. defer `interrupt` while writing a multi-line
/// prompt that must complete atomically.
///
/// ```ignore
/// // pseudocode shape
/// let _g = t::notes::with_mask(NoteMask::just(NoteClass::Interrupt))?;
/// // ... section with interrupt deferred ...
/// // _g drops here -> previous mask restored
/// ```
pub fn with_mask(mask: NoteMask) -> Result<MaskGuard> {
    let prev = set_mask(mask)?;
    Ok(MaskGuard {
        prev,
        active: true,
    })
}

/// RAII guard returned by `with_mask`. Restores the prior mask on
/// drop unless `forget` is called.
pub struct MaskGuard {
    prev: NoteMask,
    active: bool,
}

impl MaskGuard {
    /// Skip the restore on drop. Useful when the caller has already
    /// adjusted the mask further and doesn't want the original
    /// restored.
    pub fn forget(mut self) {
        self.active = false;
    }
}

impl Drop for MaskGuard {
    fn drop(&mut self) {
        if self.active {
            // Restore best-effort; nothing to do on error (we can't
            // panic from Drop and the kernel only fails on bogus
            // user-VA, which we don't pass here).
            let _ = set_mask(self.prev);
        }
    }
}

// =============================================================================
// Internal — parse the 32-byte note_record into a Note.
// =============================================================================

fn parse_note_record(buf: &[u8; 32]) -> Note {
    // Layout (mirrors kernel struct note_record):
    //   [0..16)  name (NUL-terminated within)
    //   [16..20) arg (u32 LE)
    //   [20..24) sender_pid (u32 LE)
    //   [24..32) timestamp_ns (u64 LE)
    let name_end = buf[..T_NOTE_NAME_MAX]
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(T_NOTE_NAME_MAX);
    let name_bytes = &buf[..name_end];
    // Note names are constrained to printable ASCII at post time
    // (kernel-side validation); core::str::from_utf8 is safe and
    // total here.
    let name = match core::str::from_utf8(name_bytes) {
        Ok(s) => String::from(s),
        Err(_) => {
            // Defense-in-depth: fall back to a lossy decode (replace
            // invalid bytes). Pre-allocate the result.
            let mut s = String::with_capacity(name_bytes.len());
            for &b in name_bytes {
                if b.is_ascii() {
                    s.push(b as char);
                } else {
                    s.push('?');
                }
            }
            s
        }
    };
    let arg = u32::from_le_bytes([buf[16], buf[17], buf[18], buf[19]]);
    let sender_pid = u32::from_le_bytes([buf[20], buf[21], buf[22], buf[23]]);
    let timestamp_ns = u64::from_le_bytes([
        buf[24], buf[25], buf[26], buf[27], buf[28], buf[29], buf[30], buf[31],
    ]);
    let from = if sender_pid == 0 {
        None
    } else {
        Some(sender_pid as Pid)
    };
    Note {
        name,
        arg,
        from,
        timestamp_ns,
    }
}

