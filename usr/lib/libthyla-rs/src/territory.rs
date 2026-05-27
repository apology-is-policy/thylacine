// libthyla-rs::territory — Plan 9 namespace composition for the calling
// Proc.
//
// Surfaces the kernel's SYS_MOUNT / SYS_UNMOUNT / SYS_CHROOT /
// SYS_PIVOT_ROOT primitives as typed Rust functions. The shell's
// bind / mount / unmount / chroot / pivot_root builtins consume this
// module directly; programs that need to construct per-Proc namespaces
// (containers, sandboxes, capability-restricted children) consume it
// indirectly through Command::namespace (a v1.x extension).
//
// Foundation chunk: U-2f per docs/UTOPIA-SHELL-DESIGN.md section 15.6.8.
//
// PATH-ID vs STRING PATHS at v1.0:
//   The kernel's mount table at v1.0 is keyed by an abstract u32
//   `path_id_t`. Userspace callers pick path_ids and maintain their
//   own path_id <-> string convention. String-path resolution for
//   mount/bind/unmount is a v1.x extension that requires the
//   namespace-aware fd-syscall walk subsystem. The scripture sketch
//   (UTOPIA-SHELL-DESIGN.md section 15.6.8) was forward-looking; this
//   implementation honors the v1.0 kernel ABI honestly and documents
//   the future lift.
//
// SOURCE TYPE — AsFd:
//   Mount / chroot / pivot_root take `&impl AsFd` from `t::poll`. The
//   kernel validates that the fd is a KOBJ_SPOOR with the right
//   rights; a Notes fd (also AsFd) would be rejected at the syscall
//   boundary. Using AsFd avoids defining a parallel AsSpoor trait
//   when one will do; if a future SrvConn type wraps a Spoor handle,
//   it just impls AsFd and composes for free.
//
// RIGHTS GATE:
//   SYS_MOUNT / SYS_CHROOT / SYS_PIVOT_ROOT require RIGHT_READ on the
//   source. Files opened via File::open or File::create both hold
//   RIGHT_READ (the kernel grants READ|WRITE|TRANSFER on a successful
//   SYS_WALK_OPEN). A pipe-read fd has RIGHT_READ too -- legal but
//   degenerate (a pipe-as-mount mostly produces -1 on walks). The
//   kernel takes care of validation; libthyla-rs does not duplicate.

use crate::err::{Error, Result};
use crate::poll::AsFd;
use crate::{
    t_chroot, t_mount, t_pivot_root, t_unmount, T_MAFTER, T_MBEFORE, T_MCREATE, T_MREPL,
    TPathId,
};

/// Abstract path-token used by mount/unmount/bind at v1.0.
///
/// Re-exported from the crate root so callers don't have to reach
/// into libthyla_rs::TPathId. A v1.x extension lifts this to a path
/// string at the syscall boundary; until then, callers pick values
/// and maintain their own path_id <-> path mapping.
pub type PathId = TPathId;

// =============================================================================
// MountFlags — bitmask passed to SYS_MOUNT.
// =============================================================================

/// Plan 9 mount-flag bitmask. Compose with `|`; query with `contains`.
///
/// At v1.0 only `REPL` has distinguished semantics in the kernel; the
/// remaining flags are accepted and recorded but treated additively at
/// the C-API level. The shell's bind builtin uses BEFORE/AFTER to
/// position one mount relative to another at the same path_id (a
/// future kernel extension).
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub struct MountFlags(u32);

impl MountFlags {
    /// No flags set. Equivalent to "add to table; fail if already
    /// present" -- though the v1.0 kernel may collapse semantics.
    pub const NONE: MountFlags = MountFlags(0);

    /// Replace any existing mount at the target path_id.
    pub const REPL: MountFlags = MountFlags(T_MREPL);

    /// Position the new mount BEFORE the existing entry at the same
    /// path_id (lookup hits the new mount first).
    pub const BEFORE: MountFlags = MountFlags(T_MBEFORE);

    /// Position the new mount AFTER the existing entry at the same
    /// path_id (lookup hits the existing first; new is the fallback).
    pub const AFTER: MountFlags = MountFlags(T_MAFTER);

    /// Reserved for "create the target if missing" semantics; not
    /// distinguished from no-create at v1.0.
    pub const CREATE: MountFlags = MountFlags(T_MCREATE);

    /// Construct from raw bits.
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

    /// `true` iff every flag in `other` is set in `self`.
    #[inline]
    #[must_use]
    pub const fn contains(self, other: MountFlags) -> bool {
        self.0 & other.0 == other.0
    }

    /// `true` iff no flags are set.
    #[inline]
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// `self` minus `other`.
    #[inline]
    #[must_use]
    pub const fn without(self, other: MountFlags) -> Self {
        Self(self.0 & !other.0)
    }
}

impl core::ops::BitOr for MountFlags {
    type Output = Self;
    #[inline]
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for MountFlags {
    type Output = Self;
    #[inline]
    fn bitand(self, rhs: Self) -> Self {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::BitOrAssign for MountFlags {
    #[inline]
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

// =============================================================================
// mount / bind variants / unmount.
// =============================================================================

/// Mount `source`'s tree at `target_path_id` in the calling Proc's
/// territory. `source` is any handle with a kernel-side spoor type
/// (typically a `t::fs::File`); the kernel validates the KOBJ kind
/// and rights at the syscall boundary.
///
/// `flags` follows Plan 9 mount-semantics; see `MountFlags`.
///
/// Errors:
///   - `Error::BadHandle`: `source` is not a KOBJ_SPOOR / out-of-range.
///   - `Error::PermissionDenied`: missing RIGHT_READ on `source`.
///   - `Error::InvalidArgument`: `flags` contains bits outside the
///     supported set.
///   - `Error::OutOfRange`: territory mount table is full.
///   - `Error::Io`: defense-in-depth on unmapped kernel collapse.
pub fn mount<F: AsFd + ?Sized>(
    source: &F,
    target_path_id: PathId,
    flags: MountFlags,
) -> Result<()> {
    // SAFETY: t_mount only reads the fd index; the kernel takes care
    // of validation. No user-VA arguments to bound-check here.
    let rc = unsafe { t_mount(source.as_raw_fd() as i64, target_path_id, flags.bits()) };
    if rc < 0 {
        // v1.0 kernel collapses every failure to -1; map to a
        // representative variant. v1.x will return -errno.
        return Err(Error::InvalidArgument);
    }
    Ok(())
}

/// Bind-replace: shorthand for `mount(source, target_path_id,
/// MountFlags::REPL)`. The kernel's MREPL semantics replace the
/// existing mount at `target_path_id` (atomically, under the
/// territory lock).
pub fn bind_replace<F: AsFd + ?Sized>(source: &F, target_path_id: PathId) -> Result<()> {
    mount(source, target_path_id, MountFlags::REPL)
}

/// Bind-before: shorthand for `mount(source, target_path_id,
/// MountFlags::BEFORE)`. Positions the new mount BEFORE any existing
/// at the same `target_path_id`.
pub fn bind_before<F: AsFd + ?Sized>(source: &F, target_path_id: PathId) -> Result<()> {
    mount(source, target_path_id, MountFlags::BEFORE)
}

/// Bind-after: shorthand for `mount(source, target_path_id,
/// MountFlags::AFTER)`. Positions the new mount AFTER any existing at
/// the same `target_path_id`.
pub fn bind_after<F: AsFd + ?Sized>(source: &F, target_path_id: PathId) -> Result<()> {
    mount(source, target_path_id, MountFlags::AFTER)
}

/// Remove the mount entry at `target_path_id` from the calling Proc's
/// territory. Drops the per-entry Spoor refcount; the Spoor's Dev
/// close runs if this was the last ref.
///
/// Errors:
///   - `Error::NotFound`: no entry at `target_path_id`.
pub fn unmount(target_path_id: PathId) -> Result<()> {
    // SAFETY: takes only a u32; no user-VA arguments.
    let rc = unsafe { t_unmount(target_path_id) };
    if rc < 0 {
        return Err(Error::NotFound);
    }
    Ok(())
}

// =============================================================================
// chroot / pivot_root.
// =============================================================================

/// Replace the calling Proc's territory root with `new_root`'s tree
/// (atomic spoor_ref / spoor_clunk-displaced under the territory
/// lock). Plan 9 chroot semantics; intended for the initial chroot.
/// Idempotent on same-spoor (calling with the same source twice is a
/// no-op).
///
/// For long-running Procs that need to swap roots after performing
/// work (e.g., joey's pivot from devramfs to disk-backed FS), prefer
/// `pivot_root` -- it's the audit-tracked primitive for that flow.
///
/// Errors:
///   - `Error::BadHandle`: `new_root` is not a KOBJ_SPOOR.
///   - `Error::PermissionDenied`: missing RIGHT_READ on `new_root`.
pub fn chroot<F: AsFd + ?Sized>(new_root: &F) -> Result<()> {
    let rc = unsafe { t_chroot(new_root.as_raw_fd() as i64) };
    if rc < 0 {
        return Err(Error::InvalidArgument);
    }
    Ok(())
}

/// Atomically swap the territory root to `new_root` (the long-running-
/// Proc primitive). Distinct from `chroot` for audit-trackability and
/// semantic clarity: chroot is the initial-bringup primitive; pivot_root
/// is for a Proc that has already established its territory and now
/// needs to flip its root.
///
/// Errors are the same as `chroot`.
pub fn pivot_root<F: AsFd + ?Sized>(new_root: &F) -> Result<()> {
    let rc = unsafe { t_pivot_root(new_root.as_raw_fd() as i64) };
    if rc < 0 {
        return Err(Error::InvalidArgument);
    }
    Ok(())
}
