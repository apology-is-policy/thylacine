//! Loom -- the native userspace API for the kernel's io_uring-inverted 9P ring
//! transport (`docs/LOOM.md`). A [`Ring`] wraps `SYS_LOOM_SETUP` / `_REGISTER` /
//! `_ENTER` (66 / 67 / 68): setup maps the shared SQ/CQ Burrow into this Proc and
//! reports its geometry; submission writes an [`Sqe`] into the SQ and the kernel's
//! #841 elected-reader 9P client drives it; replies return as [`Cqe`]s in the CQ.
//!
//! This is the backend the libtapestry `Loom` seam trait targets
//! (`impl tapestry::Loom for libthyla_rs::loom::Ring`, `docs/TAPESTRY.md`):
//! present = a [`Sqe::write`] submit, input/vsync = a multishot [`Sqe::read`],
//! reap = [`Ring::reap`] draining the CQ. The kernel enforces I-29 (completion
//! integrity) and I-30 (submit-time capability pin); this layer is a thin,
//! memory-model-correct view over the shared ring -- a buggy producer can only
//! corrupt its own ring view, never the kernel's (the kernel copies every SQE to
//! private memory and keeps `sq_head` / `cq_tail` kernel-private).

use core::sync::atomic::{AtomicU32, Ordering};

use crate::err::{Error, Result};
use crate::handle::{Handle, Rights};
use crate::{t_burrow_detach, t_loom_enter, t_loom_register, t_loom_setup};

// ---------------------------------------------------------------------------
// ABI constants -- mirror kernel/include/thylacine/loom.h. Kept in lockstep;
// the `repr(C)` structs below carry compile-time size asserts so a layout drift
// fails the build, not at runtime.
// ---------------------------------------------------------------------------

/// Opcodes -- the `p9_client_*` surface (`LOOM_OP_*`).
pub mod op {
    pub const NOP: u8 = 0;
    pub const WALK: u8 = 1;
    pub const LOPEN: u8 = 2;
    pub const LCREATE: u8 = 3;
    pub const READ: u8 = 4;
    pub const WRITE: u8 = 5;
    pub const GETATTR: u8 = 6;
    pub const SETATTR: u8 = 7;
    pub const READDIR: u8 = 8;
    pub const FSYNC: u8 = 9;
    pub const CLUNK: u8 = 10;
    pub const RENAMEAT: u8 = 11;
    pub const UNLINKAT: u8 = 12;
    pub const MKDIR: u8 = 13;
    pub const SYMLINK: u8 = 14;
    pub const LINK: u8 = 15;
    pub const MKNOD: u8 = 16;
    pub const READLINK: u8 = 17;
    pub const STATFS: u8 = 18;
}

/// Per-SQE flags (`LOOM_SQE_*`).
pub mod sqe_flag {
    pub const LINK: u8 = 1 << 0;
    pub const DRAIN: u8 = 1 << 1;
    pub const CQE_SKIP: u8 = 1 << 2;
    pub const MULTISHOT: u8 = 1 << 3;
}

/// `LOOM_CQE_MORE` -- set on every non-terminal shot of a multishot stream.
pub const CQE_MORE: u32 = 1 << 0;

/// `LOOM_SETUP_SQPOLL` -- start a kernel poll-thread (zero-syscall submission).
pub const SETUP_SQPOLL: u32 = 1 << 0;

/// `LOOM_ENTER_*` flags.
pub const ENTER_GETEVENTS: u32 = 1 << 0;
pub const ENTER_NONBLOCK: u32 = 1 << 1;

/// `LOOM_RING_SQ_NEED_WAKEUP` -- set by an idled SQPOLL kthread in the ring flags.
pub const RING_SQ_NEED_WAKEUP: u32 = 1 << 0;

/// `LOOM_HANDLE_RAW` -- a path-resolved-at-submit sentinel (reserved, not v1.0).
pub const HANDLE_RAW: u32 = 0xFFFF_FFFF;

/// Register sub-ops (`LOOM_REGISTER_*`).
const REGISTER_HANDLES: u32 = 0;
const REGISTER_BUFFERS: u32 = 1;

/// Ring-size + table bounds (`LOOM_MAX_*`).
pub const MAX_ENTRIES: u32 = 4096;
pub const MAX_REG_HANDLES: u32 = 64;
pub const MAX_REG_BUFFERS: u32 = 64;

// ---------------------------------------------------------------------------
// Ring entries + setup struct. `repr(C)` byte-pinned to loom.h.
// ---------------------------------------------------------------------------

/// One submission entry (`struct loom_sqe`, 64 bytes). Construct via the
/// per-opcode helpers ([`Sqe::nop`], [`Sqe::read`], ...) so the reserved fields
/// stay zeroed; the kernel reads `_resv1` for some ops.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Sqe {
    pub opcode: u8,
    pub flags: u8,
    _resv0: u16,
    pub handle_idx: u32,
    pub offset: u64,
    pub len: u32,
    pub buf_idx_or_off: u32,
    pub user_data: u64,
    _resv1: [u64; 4],
}

const _: () = assert!(core::mem::size_of::<Sqe>() == 64);

/// One completion entry (`struct loom_cqe`, 16 bytes).
#[repr(C)]
#[derive(Copy, Clone, Debug)]
pub struct Cqe {
    /// Echoed verbatim from the submitting SQE's `user_data`.
    pub user_data: u64,
    /// `>= 0` = byte count / 0 / packed qid; `< 0` = `-errno` (Rlerror).
    pub result: i32,
    /// `LOOM_CQE_*` (e.g. [`CQE_MORE`]).
    pub flags: u32,
}

const _: () = assert!(core::mem::size_of::<Cqe>() == 16);

/// One registered-buffer descriptor (`struct loom_buf_reg`, 16 bytes): a VA range
/// within one anonymous RW VMA the kernel pins for zero-copy payload.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct BufReg {
    pub va: u64,
    pub len: u64,
}

const _: () = assert!(core::mem::size_of::<BufReg>() == 16);

/// `SYS_LOOM_SETUP` in/out struct (`struct loom_params`, 88 bytes). `flags` is the
/// only IN field; the kernel fills the rest with the mapped ring geometry.
#[repr(C)]
#[derive(Copy, Clone)]
struct Params {
    flags: u32,
    sq_entries: u32,
    cq_entries: u32,
    ring_size: u32,
    ring_va: u64,
    hdr_off: u32,
    sq_array_off: u32,
    sqe_off: u32,
    cqe_off: u32,
    sq_array_size: u32,
    sqe_size: u32,
    cqe_size: u32,
    _resv0: u32,
    _resv1: [u64; 4],
}

const _: () = assert!(core::mem::size_of::<Params>() == 88);

impl Sqe {
    /// An all-zero SQE (opcode NOP, no flags). The base every helper builds on.
    #[inline]
    pub const fn zeroed() -> Sqe {
        Sqe {
            opcode: op::NOP,
            flags: 0,
            _resv0: 0,
            handle_idx: 0,
            offset: 0,
            len: 0,
            buf_idx_or_off: 0,
            user_data: 0,
            _resv1: [0; 4],
        }
    }

    /// A no-op -- exercises the whole ring path with no fid (inline-completed).
    #[inline]
    pub const fn nop(user_data: u64) -> Sqe {
        let mut s = Sqe::zeroed();
        s.opcode = op::NOP;
        s.user_data = user_data;
        s
    }

    /// `fsync` a registered file handle (requires `RIGHT_WRITE` at register time).
    #[inline]
    pub const fn fsync(handle_idx: u32, user_data: u64) -> Sqe {
        let mut s = Sqe::zeroed();
        s.opcode = op::FSYNC;
        s.handle_idx = handle_idx;
        s.user_data = user_data;
        s
    }

    /// `read` `len` bytes at `file_off` from a registered file handle into the
    /// registered buffer `buf_idx` at `buf_off` (requires `RIGHT_READ`).
    #[inline]
    pub const fn read(
        handle_idx: u32,
        file_off: u64,
        len: u32,
        buf_idx: u32,
        buf_off: u64,
        user_data: u64,
    ) -> Sqe {
        let mut s = Sqe::zeroed();
        s.opcode = op::READ;
        s.handle_idx = handle_idx;
        s.offset = file_off;
        s.len = len;
        s.buf_idx_or_off = buf_idx;
        s._resv1[0] = buf_off; // LOOM_SQE_BUF_OFF
        s.user_data = user_data;
        s
    }

    /// `write` `len` bytes from the registered buffer `buf_idx` at `buf_off` to a
    /// registered file handle at `file_off` (requires `RIGHT_WRITE`).
    #[inline]
    pub const fn write(
        handle_idx: u32,
        file_off: u64,
        len: u32,
        buf_idx: u32,
        buf_off: u64,
        user_data: u64,
    ) -> Sqe {
        let mut s = Sqe::zeroed();
        s.opcode = op::WRITE;
        s.handle_idx = handle_idx;
        s.offset = file_off;
        s.len = len;
        s.buf_idx_or_off = buf_idx;
        s._resv1[0] = buf_off;
        s.user_data = user_data;
        s
    }

    /// Set per-SQE [`sqe_flag`] bits (LINK / DRAIN / CQE_SKIP / MULTISHOT).
    #[inline]
    pub const fn with_flags(mut self, flags: u8) -> Sqe {
        self.flags |= flags;
        self
    }
}

impl Cqe {
    /// `true` if more completions follow this one (a non-terminal multishot shot).
    #[inline]
    pub const fn more(&self) -> bool {
        self.flags & CQE_MORE != 0
    }

    /// Map the result to a `Result`: `>= 0` -> `Ok(result)`, `< 0` -> the errno.
    #[inline]
    pub fn ok(&self) -> Result<i32> {
        if self.result < 0 {
            // result is -errno; negate into the libthyla-rs Error mapping.
            Err(Error::from(self.result.unsigned_abs() as i32))
        } else {
            Ok(self.result)
        }
    }
}

// ---------------------------------------------------------------------------
// The Ring.
// ---------------------------------------------------------------------------

/// A native handle to a Loom ring: the `KObj_Loom` fd plus a view of the mapped
/// SQ/CQ Burrow. Dropping it unmaps the ring and closes the fd (the #847
/// dual-refcount frees the pages once both the mapping and the handle are gone).
pub struct Ring {
    handle: Handle,
    ring_va: u64,
    ring_size: usize,
    hdr: *mut u8,
    sq_array: *mut u32,
    sqes: *mut Sqe,
    cqes: *mut Cqe,
    sq_entries: u32,
    sq_mask: u32,
    cq_mask: u32,
}

// Header field byte offsets within `struct loom_ring_hdr` (verified by the
// loom.h layout: each is a naturally-aligned u32 in declaration order).
const HDR_SQ_HEAD: usize = 0;
const HDR_SQ_TAIL: usize = 4;
const HDR_CQ_HEAD: usize = 16;
const HDR_CQ_TAIL: usize = 20;
const HDR_FLAGS: usize = 32;

impl Ring {
    /// Create a ring with `entries` SQ slots (a power of two, `1..=MAX_ENTRIES`).
    /// `flags` accepts [`SETUP_SQPOLL`]. The kernel maps the SQ/CQ Burrow into
    /// this Proc and reports its geometry; the returned `Ring` owns both.
    pub fn setup(entries: u32, flags: u32) -> Result<Ring> {
        let mut params: Params = Params {
            flags,
            sq_entries: 0,
            cq_entries: 0,
            ring_size: 0,
            ring_va: 0,
            hdr_off: 0,
            sq_array_off: 0,
            sqe_off: 0,
            cqe_off: 0,
            sq_array_size: 0,
            sqe_size: 0,
            cqe_size: 0,
            _resv0: 0,
            _resv1: [0; 4],
        };
        // SYS_LOOM_SETUP returns the loom_fd (>= 0) and fills `params` with the
        // mapped ring geometry, or -1 (bad args / OOM / handle-table-full).
        let rc = unsafe { t_loom_setup(entries as u64, &mut params as *mut Params as u64) };
        if rc < 0 {
            return Err(Error::InvalidArgument);
        }
        let handle = Handle::from_raw(rc as i32, Rights::READ | Rights::WRITE);

        let base = params.ring_va;
        let hdr = (base + params.hdr_off as u64) as *mut u8;
        let sq_array = (base + params.sq_array_off as u64) as *mut u32;
        let sqes = (base + params.sqe_off as u64) as *mut Sqe;
        let cqes = (base + params.cqe_off as u64) as *mut Cqe;

        Ok(Ring {
            handle,
            ring_va: base,
            ring_size: params.ring_size as usize,
            hdr,
            sq_array,
            sqes,
            cqes,
            sq_entries: params.sq_entries,
            sq_mask: params.sq_entries.wrapping_sub(1),
            cq_mask: params.cq_entries.wrapping_sub(1),
        })
    }

    /// The raw `KObj_Loom` fd (for spawning a child that shares it -- though
    /// KObj_Loom is non-transferable, so this is only meaningful in-Proc).
    #[inline]
    pub fn raw_fd(&self) -> i32 {
        self.handle.raw()
    }

    #[inline]
    fn atom(&self, off: usize) -> &AtomicU32 {
        // The header control words are shared with the kernel, which accesses
        // them via __atomic_* on the same physical page. AtomicU32 has the same
        // layout as u32; every access from this side goes through this view.
        unsafe { &*(self.hdr.add(off) as *const AtomicU32) }
    }

    /// Install `fds` (each a `KOBJ_SPOOR` fd) into the ring's fixed-handle table.
    /// Each is resolved once to its `(client, fid)` + a rights snapshot -- the
    /// I-30 submit-time-pin substrate. Replaces any prior table. `fds.len()` must
    /// be `<= MAX_REG_HANDLES`.
    pub fn register_handles(&self, fds: &[i32]) -> Result<()> {
        if fds.len() as u64 > MAX_REG_HANDLES as u64 {
            return Err(Error::InvalidArgument);
        }
        // The kernel reads each fd as a little-endian u32; an i32 fd index (always
        // non-negative) has identical bytes, so the slice maps verbatim.
        let rc = unsafe {
            t_loom_register(
                self.handle.raw() as u64,
                REGISTER_HANDLES as u64,
                fds.as_ptr() as u64,
                fds.len() as u64,
            )
        };
        if rc < 0 {
            Err(Error::InvalidArgument)
        } else {
            Ok(())
        }
    }

    /// Pin `bufs` (each a VA range within one anonymous RW VMA) for zero-copy
    /// payload. Replaces any prior buffer table. `bufs.len() <= MAX_REG_BUFFERS`.
    pub fn register_buffers(&self, bufs: &[BufReg]) -> Result<()> {
        if bufs.len() as u64 > MAX_REG_BUFFERS as u64 {
            return Err(Error::InvalidArgument);
        }
        let rc = unsafe {
            t_loom_register(
                self.handle.raw() as u64,
                REGISTER_BUFFERS as u64,
                bufs.as_ptr() as u64,
                bufs.len() as u64,
            )
        };
        if rc < 0 {
            Err(Error::InvalidArgument)
        } else {
            Ok(())
        }
    }

    /// Place one SQE into the SQ. Returns [`Error::WouldBlock`] if the SQ is full
    /// (its slots all unconsumed). The op runs on the next [`enter`](Ring::enter)
    /// (or immediately under SQPOLL). Single-producer: this Proc's threads must
    /// not call it concurrently on the same ring without external serialization.
    pub fn try_submit(&self, sqe: &Sqe) -> Result<()> {
        // We own sq_tail (relaxed self-read); sq_head is kernel-advanced (acquire,
        // so a freed slot is observed). Free-running u32: the wrapping difference
        // is the in-flight count.
        let tail = self.atom(HDR_SQ_TAIL).load(Ordering::Relaxed);
        let head = self.atom(HDR_SQ_HEAD).load(Ordering::Acquire);
        if tail.wrapping_sub(head) >= self.sq_entries {
            return Err(Error::WouldBlock);
        }
        let slot = tail & self.sq_mask;
        unsafe {
            // 1:1 submission-slot -> SQE-slot. Write the SQE body + the indirection
            // index, then release-bump sq_tail. The release store orders both prior
            // writes before any acquire-load of sq_tail (the kernel's loom_drain_sq,
            // which then copies the SQE to private memory -- ring TOCTOU-safe).
            core::ptr::write(self.sqes.add(slot as usize), *sqe);
            core::ptr::write(self.sq_array.add(slot as usize), slot);
        }
        self.atom(HDR_SQ_TAIL)
            .store(tail.wrapping_add(1), Ordering::Release);
        Ok(())
    }

    /// Enter the kernel: consume up to `to_submit` queued SQEs, then block until
    /// at least `min_complete` CQEs are available (unless [`ENTER_NONBLOCK`]).
    /// Returns the number of SQEs consumed. The wait is death-interruptible.
    pub fn enter(&self, to_submit: u32, min_complete: u32, flags: u32) -> Result<i64> {
        let rc = unsafe {
            t_loom_enter(
                self.handle.raw() as u64,
                to_submit as u64,
                min_complete as u64,
                flags as u64,
            )
        };
        if rc < 0 {
            Err(Error::InvalidArgument)
        } else {
            Ok(rc)
        }
    }

    /// Reap one completion, or `None` if the CQ is empty. Single-consumer: this
    /// Proc's threads must not call it concurrently without external serialization.
    pub fn reap(&self) -> Option<Cqe> {
        // cq_head is ours (relaxed); cq_tail is kernel-posted (acquire pairs with
        // the kernel's release after it wrote the CQE body).
        let head = self.atom(HDR_CQ_HEAD).load(Ordering::Relaxed);
        let tail = self.atom(HDR_CQ_TAIL).load(Ordering::Acquire);
        if head == tail {
            return None;
        }
        let slot = head & self.cq_mask;
        // The acquire-load of cq_tail above happens-before this read of the CQE.
        let cqe = unsafe { core::ptr::read(self.cqes.add(slot as usize)) };
        // Release the slot: orders the CQE read above before the kernel's
        // acquire-load of cq_head observes the advance -- so the kernel cannot
        // reuse (overwrite) the slot while we are still reading it.
        self.atom(HDR_CQ_HEAD)
            .store(head.wrapping_add(1), Ordering::Release);
        Some(cqe)
    }

    /// Submit one SQE and block until its terminal CQE, returning it. The simple
    /// synchronous request/response shape (one op in flight). Spins reaping after
    /// the blocking enter so a multishot or out-of-order layout still returns the
    /// op's own completion; for the single-op case the first reap is it.
    pub fn submit_one_wait(&self, sqe: &Sqe) -> Result<Cqe> {
        self.try_submit(sqe)?;
        self.enter(1, 1, ENTER_GETEVENTS)?;
        loop {
            if let Some(c) = self.reap() {
                return Ok(c);
            }
            // The blocking enter guarantees >= 1 CQE; a spurious wakeup (or a
            // sibling reaper) re-enters non-blocking to make progress.
            self.enter(0, 1, ENTER_GETEVENTS)?;
        }
    }

    /// `true` if an idled SQPOLL kthread needs a wake-up [`enter`](Ring::enter)
    /// (it set `LOOM_RING_SQ_NEED_WAKEUP`). Always `false` on a non-SQPOLL ring.
    #[inline]
    pub fn sq_need_wakeup(&self) -> bool {
        self.atom(HDR_FLAGS).load(Ordering::Acquire) & RING_SQ_NEED_WAKEUP != 0
    }

    /// Current count of queued-but-unconsumed SQEs (diagnostic).
    #[inline]
    pub fn sq_pending(&self) -> u32 {
        let tail = self.atom(HDR_SQ_TAIL).load(Ordering::Relaxed);
        let head = self.atom(HDR_SQ_HEAD).load(Ordering::Acquire);
        tail.wrapping_sub(head)
    }
}

impl Drop for Ring {
    fn drop(&mut self) {
        // Unmap the ring VA (drops the Burrow's mapping_count), then the `handle`
        // field drops (closing the loom_fd -> loom_unref -> drops handle_count).
        // Order-independent: the #847 dual-refcount frees the pages once both hit
        // zero. A failed detach (already unmapped) is harmless -- the handle close
        // still reclaims. The kernel side touches the ring via its own direct-map
        // alias, so unmapping the user VA never strands an in-flight op or the
        // SQPOLL kthread (loom_free joins it at the handle-close last-ref drop).
        unsafe {
            let _ = t_burrow_detach(self.ring_va, self.ring_size as u64);
        }
    }
}
