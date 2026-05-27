// libthyla-rs::handle — RAII handle wrapper for Thylacine kernel objects.
//
// Every kernel-object-backed type in libthyla-rs (t::fs::File,
// t::notes::Notes, t::hardware::{Mmio, Irq, Dma}, t::process::Child,
// future SrvConn wrappers, ...) composes a `Handle` internally.
// `Handle` owns a slot index in the calling Proc's handle table; `Drop`
// releases it via SYS_CLOSE so no caller has to remember a manual
// close.
//
// Foundation chunk: U-2a per docs/UTOPIA-SHELL-DESIGN.md §15. Every
// resource-bearing typed wrapper builds on this.
//
// CONSTRUCTION DISCIPLINE:
//   - `Handle::from_raw` is `pub(crate)`. External code cannot forge a
//     `Handle` out of an arbitrary i32. The only way to obtain one is
//     to call a typed constructor (e.g., `t::fs::File::open`), which
//     gates rights at creation. This preserves the rights-monotonicity
//     invariant (ARCH §28 I-2 + I-6): rights are set at handle creation
//     and never rise.
//   - Cloning is intentionally not implemented. The kernel has no
//     dup-with-same-rights syscall; a future SYS_HANDLE_DUP would
//     create a separate handle with new (reduced) rights, not a copy.
//   - `Drop` calls SYS_CLOSE; any error is silently dropped. EBADF
//     here would indicate a `Handle` minted for a non-existent slot
//     (programmer bug, not a runtime concern); Drop has no error
//     return path anyway.
//
// RIGHTS:
//   - The `Rights` newtype mirrors `RIGHT_*` in
//     kernel/include/thylacine/handle.h. The kernel enforces
//     `RIGHT_ALL = 0x3f` (six bits) at handle-creation syscall time;
//     bits outside that range are rejected by the kernel. This type
//     allows callers to compose rights ergonomically without bringing
//     in the `bitflags` crate (kept zero-dep at U-2a).
//   - Compose with `|`, query with `contains` / `intersects`, subtract
//     with `without`. Convert to/from `u32` via `From`.

use crate::t_close;

// =============================================================================
// Handle — RAII wrapper.
// =============================================================================

/// A kernel-object handle held by the calling Proc.
///
/// RAII: `Drop` releases the slot via `SYS_CLOSE`.
///
/// Construction is `pub(crate)` so only libthyla-rs's typed wrappers
/// can mint a `Handle`. External code obtains handles by calling a
/// constructor on a higher-level type (e.g., `t::fs::File::open`),
/// which gates the rights granted by the kernel at handle creation.
///
/// `Handle` is not `Clone`: the kernel has no dup-with-same-rights
/// syscall; a future `SYS_HANDLE_DUP` would create a distinct handle
/// with new rights, never a copy.
pub struct Handle {
    idx: i32,
    rights: Rights,
}

impl Handle {
    /// Mint a `Handle` for a kernel-table slot returned by a syscall.
    ///
    /// Intended only for libthyla-rs's internal wrappers that have
    /// just performed a syscall (e.g., `SYS_WALK_OPEN`, `SYS_MMIO_CREATE`)
    /// that returned a non-negative handle index and know the rights
    /// they requested.
    ///
    /// Contract (informal; `pub(crate)`): `idx` is a valid handle
    /// index in the current Proc's handle table, not previously
    /// closed. `rights` matches the rights the kernel granted
    /// (typically the rights bitmask passed to the creating syscall —
    /// which the kernel either honoured or rejected at create time).
    #[inline]
    // Dead at U-2a (this chunk). First caller lands at U-2c when
    // t::fs::File::open mints handles from SYS_WALK_OPEN returns.
    #[allow(dead_code)]
    pub(crate) fn from_raw(idx: i32, rights: Rights) -> Self {
        Self { idx, rights }
    }

    /// The rights bits the kernel granted at handle creation.
    #[inline]
    #[must_use]
    pub const fn rights(&self) -> Rights {
        self.rights
    }

    /// The underlying handle table index.
    ///
    /// Exposed so libthyla-rs's typed wrappers can pass the index to
    /// the kernel via `svc`. Not intended for ad-hoc use in caller
    /// code; if you find yourself reaching for this from outside
    /// libthyla-rs, prefer a higher-level wrapper or open a new one.
    #[inline]
    #[must_use]
    pub const fn raw(&self) -> i32 {
        self.idx
    }
}

impl Drop for Handle {
    fn drop(&mut self) {
        // SAFETY: by from_raw's contract, idx is a valid handle in
        // this Proc's table. SYS_CLOSE returns 0 on success and a
        // negative errno on EBADF; EBADF here would indicate a Handle
        // minted for a non-existent slot — a programmer bug, not a
        // runtime concern. Drop has no path to report; silently drop.
        unsafe { t_close(self.idx as i64); }
    }
}

// =============================================================================
// Rights — small bitflags-style newtype over u32.
// =============================================================================

/// Right bits a `Handle` may hold.
///
/// Mirrors `RIGHT_*` in `kernel/include/thylacine/handle.h`. The kernel
/// bound (`RIGHT_ALL = 0x3f`) is enforced at handle-creation syscall
/// time; bits outside that range are rejected by the kernel even if
/// expressible at this type level.
///
/// Compose with `|`. Subtract with `without`. Query with `contains` or
/// `intersects`.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct Rights(u32);

impl Rights {
    /// No rights.
    pub const NONE: Rights = Rights(0);

    /// `RIGHT_READ` — permits `SYS_READ` and read-shaped ops.
    pub const READ: Rights = Rights(1 << 0);

    /// `RIGHT_WRITE` — permits `SYS_WRITE` and write-shaped ops.
    pub const WRITE: Rights = Rights(1 << 1);

    /// `RIGHT_MAP` — permits installing user-VA mappings
    /// (`SYS_MMIO_MAP`, `SYS_DMA_MAP`, `SYS_BURROW_ATTACH`).
    pub const MAP: Rights = Rights(1 << 2);

    /// `RIGHT_TRANSFER` — permits handing the handle to another Proc
    /// via 9P. Hardware handles (`KObj_MMIO`, `KObj_IRQ`, `KObj_DMA`)
    /// are rejected at create time when this bit is requested
    /// (invariant I-5).
    pub const TRANSFER: Rights = Rights(1 << 3);

    /// `RIGHT_DMA` — permits DMA buffer ops on a `KObj_DMA` handle.
    pub const DMA: Rights = Rights(1 << 4);

    /// `RIGHT_SIGNAL` — permits `SYS_IRQ_WAIT` on a `KObj_IRQ` handle,
    /// and note-signalling ops on note-bearing handles.
    pub const SIGNAL: Rights = Rights(1 << 5);

    /// `RIGHT_ALL` — the union of every currently-defined right bit
    /// (kernel's `RIGHT_ALL = 0x3f`). Useful for asserting "all rights
    /// available." Phase 5+ may grow the set; the kernel enforces.
    pub const ALL: Rights = Rights(0x3f);

    /// Construct from raw bits.
    ///
    /// Bits outside `Rights::ALL` are allowed at the type level; the
    /// kernel rejects out-of-range bits at handle-creation syscall
    /// time.
    #[inline]
    #[must_use]
    pub const fn from_bits(bits: u32) -> Self {
        Self(bits)
    }

    /// Raw bits.
    #[inline]
    #[must_use]
    pub const fn bits(self) -> u32 {
        self.0
    }

    /// `true` iff every right in `other` is also in `self`.
    #[inline]
    #[must_use]
    pub const fn contains(self, other: Rights) -> bool {
        self.0 & other.0 == other.0
    }

    /// `true` iff `self` and `other` share at least one right.
    #[inline]
    #[must_use]
    pub const fn intersects(self, other: Rights) -> bool {
        self.0 & other.0 != 0
    }

    /// `true` iff no rights are set.
    #[inline]
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// `self` minus `other` — drops every right also present in
    /// `other` from `self`. Idiomatic shape for "all rights except X."
    #[inline]
    #[must_use]
    pub const fn without(self, other: Rights) -> Self {
        Self(self.0 & !other.0)
    }
}

impl core::ops::BitOr for Rights {
    type Output = Self;
    #[inline]
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Rights {
    type Output = Self;
    #[inline]
    fn bitand(self, rhs: Self) -> Self {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::BitOrAssign for Rights {
    #[inline]
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

impl core::ops::BitAndAssign for Rights {
    #[inline]
    fn bitand_assign(&mut self, rhs: Self) {
        self.0 &= rhs.0;
    }
}

impl From<u32> for Rights {
    #[inline]
    fn from(bits: u32) -> Self {
        Self(bits)
    }
}

impl From<Rights> for u32 {
    #[inline]
    fn from(r: Rights) -> u32 {
        r.0
    }
}
