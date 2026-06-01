// libthyla-rs::cap — Thylacine capability surface.
//
// Foundational chunk: U-2f per docs/UTOPIA-SHELL-DESIGN.md section 15.6.9.
//
// THE TWO-PHASE GRANT MODEL (CORVUS-DESIGN.md section 5.5.1, the
// `cap` device, invariant I-2 / I-21):
//
//   1. A holder of `CAP_GRANT_HOSTOWNER` registers a pending grant of
//      `CAP_HOSTOWNER` against a target Proc's stripes (`grant`).
//   2. The target Proc -- once it has `PROC_FLAG_CONSOLE_ATTACHED` --
//      redeems the pending grant for itself (`use_grant`). The grant
//      is consumed (one-shot).
//
//   At v1.0 only `CAP_HOSTOWNER` is grantable through this path. Other
//   caps either flow through `rfork_with_caps` (monotonic reduction,
//   invariant I-2) at spawn time, or are constants the kernel stamps
//   at Proc creation. The cap surface here is the elevation-only path.
//
// NO CURRENT() / NO DROP() AT v1.0:
//   The kernel exposes no syscall to query the calling Proc's caps or
//   to drop caps without spawning a child. Both are v1.x extensions
//   (CORVUS-DESIGN.md section 5.5 reserves the syscall numbers).
//   Programs that need to drop caps at v1.0 spawn a child with
//   reduced caps via t::process::Command (see U-2d's `caps(...)`
//   builder method, which routes through SYS_SPAWN_FULL_ARGV's
//   `cap_mask` field).
//
// TRAPS:
//   - `CAP_HOSTOWNER` is ELEVATION-ONLY. The `rfork` path (v1.x) MUST
//     reject attempts to grant HOSTOWNER through forks; only the cap
//     device path (this module's `grant` + `use_grant`) can convey
//     it. The kernel enforces this; libthyla-rs is documentation.
//   - The grant/use protocol is two-Proc: the granter calls `grant`,
//     the receiver calls `use_grant`. A receiver cannot `use_grant`
//     unless (a) a pending grant exists for them, AND (b) they hold
//     `PROC_FLAG_CONSOLE_ATTACHED`.

use crate::err::{Error, Result};
use crate::{
    t_cap_grant, t_cap_grant_clearance, t_cap_use, T_CAP_CHOWN, T_CAP_CSPRNG_READ,
    T_CAP_DAC_OVERRIDE, T_CAP_GRANT_CLEARANCE, T_CAP_GRANT_HOSTOWNER, T_CAP_HOSTOWNER,
    T_CAP_HW_CREATE, T_CAP_KILL, T_CAP_LOCK_PAGES,
};

// =============================================================================
// Caps — bitmask of capability bits.
// =============================================================================

/// Capability bitmask carried per-Proc.
///
/// Mirrors `CAP_*` in `kernel/include/thylacine/caps.h`. Compose with
/// `|`; query with `contains`; subtract with `without`. The kernel
/// enforces the full set semantically; this newtype is a typed wrapper
/// so callers don't pass raw u64s around.
#[derive(Copy, Clone, PartialEq, Eq, Debug, Default)]
pub struct Caps(u64);

impl Caps {
    /// No capabilities.
    pub const NONE: Caps = Caps(0);

    /// `CAP_HW_CREATE` — permits `SYS_MMIO_CREATE` / `SYS_IRQ_CREATE`
    /// / `SYS_DMA_CREATE`. Fork-grantable (monotonic reduction).
    pub const HW_CREATE: Caps = Caps(T_CAP_HW_CREATE);

    /// `CAP_LOCK_PAGES` — permits `SYS_MLOCKALL`. Fork-grantable.
    pub const LOCK_PAGES: Caps = Caps(T_CAP_LOCK_PAGES);

    /// `CAP_CSPRNG_READ` — permits `SYS_GETRANDOM`. Fork-grantable.
    pub const CSPRNG_READ: Caps = Caps(T_CAP_CSPRNG_READ);

    /// `CAP_HOSTOWNER` — host-administration cap. ELEVATION-ONLY:
    /// flows only through the `cap` device (this module's `grant` +
    /// `use_grant`); cannot be conveyed by `rfork`. Invariant I-2 +
    /// I-21.
    pub const HOSTOWNER: Caps = Caps(T_CAP_HOSTOWNER);

    /// `CAP_GRANT_HOSTOWNER` — permits the holder to register a
    /// pending HOSTOWNER grant via `grant`. The holder is typically
    /// `joey` (the system bringup Proc); the receiver is typically
    /// `corvus` (the authentication daemon).
    pub const GRANT_HOSTOWNER: Caps = Caps(T_CAP_GRANT_HOSTOWNER);

    /// `CAP_GRANT_CLEARANCE` (A-4a) — permits the holder to register a
    /// pending CLEARANCE grant via `grant_clearance` (the legate path).
    /// Fork-grantable; held only by `corvus` (the clearance authority).
    pub const GRANT_CLEARANCE: Caps = Caps(T_CAP_GRANT_CLEARANCE);

    /// `CAP_DAC_OVERRIDE` (A-4a) — `perm_check` rwx bypass, split out of
    /// `CAP_HOSTOWNER`. ELEVATION-ONLY: acquired only via a clearance
    /// grant (the legate's `cap` device redeem), never by `rfork`.
    pub const DAC_OVERRIDE: Caps = Caps(T_CAP_DAC_OVERRIDE);

    /// `CAP_CHOWN` (A-4a) — chown/chgrp-to-any, split out of
    /// `CAP_HOSTOWNER`. ELEVATION-ONLY.
    pub const CHOWN: Caps = Caps(T_CAP_CHOWN);

    /// `CAP_KILL` (A-4a) — cross-identity kill override (the third
    /// `/proc/<pid>/ctl` authority axis, A-4b). ELEVATION-ONLY.
    pub const KILL: Caps = Caps(T_CAP_KILL);

    /// The clearance-grantable set — the elevation-only caps a clearance
    /// grant may confer. Mirrors `CAP_GRANTABLE_CLEARANCE`
    /// (`kernel/include/thylacine/devcap.h`). A clearance `cap_mask` must
    /// be a non-empty subset of this; `CAP_HOSTOWNER` is NOT in it (that
    /// stays on the console-gated hostowner path).
    pub const GRANTABLE_CLEARANCE: Caps =
        Caps(T_CAP_DAC_OVERRIDE | T_CAP_CHOWN | T_CAP_KILL);

    /// Construct from raw bits. Bits outside the currently-known set
    /// are accepted at the type level; the kernel rejects unknown
    /// bits when consumed.
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

    /// `true` iff every cap in `other` is also in `self`.
    #[inline]
    #[must_use]
    pub const fn contains(self, other: Caps) -> bool {
        self.0 & other.0 == other.0
    }

    /// `true` iff `self` and `other` share at least one cap.
    #[inline]
    #[must_use]
    pub const fn intersects(self, other: Caps) -> bool {
        self.0 & other.0 != 0
    }

    /// `true` iff no caps are set.
    #[inline]
    #[must_use]
    pub const fn is_empty(self) -> bool {
        self.0 == 0
    }

    /// `self` minus `other` -- drops every cap also in `other`.
    /// Useful for "all but X" constructions: `Caps::ALL.without(
    /// Caps::HOSTOWNER)`.
    #[inline]
    #[must_use]
    pub const fn without(self, other: Caps) -> Self {
        Self(self.0 & !other.0)
    }
}

impl core::ops::BitOr for Caps {
    type Output = Self;
    #[inline]
    fn bitor(self, rhs: Self) -> Self {
        Self(self.0 | rhs.0)
    }
}

impl core::ops::BitAnd for Caps {
    type Output = Self;
    #[inline]
    fn bitand(self, rhs: Self) -> Self {
        Self(self.0 & rhs.0)
    }
}

impl core::ops::BitOrAssign for Caps {
    #[inline]
    fn bitor_assign(&mut self, rhs: Self) {
        self.0 |= rhs.0;
    }
}

impl core::ops::BitAndAssign for Caps {
    #[inline]
    fn bitand_assign(&mut self, rhs: Self) {
        self.0 &= rhs.0;
    }
}

// =============================================================================
// Stripes — target identification for grant.
// =============================================================================

/// Caller-chosen target identification for `grant`. At v1.0 the
/// kernel's `cap` device uses stripes as an opaque u64 the granter
/// records and the redeemer matches; future device-side improvements
/// might lift this to a proper handle or pid.
pub type Stripes = u64;

// =============================================================================
// grant / use_grant — the two-Proc elevation flow.
// =============================================================================

/// Register a pending grant of `caps` against `target_stripes`.
///
/// Caller must hold `CAP_GRANT_HOSTOWNER`. At v1.0 only `CAP_HOSTOWNER`
/// can be granted through this path; the kernel rejects any other bit
/// in `caps`.
///
/// On success the grant is registered in the kernel's `cap` device
/// table; the receiver (a Proc with matching stripes and
/// `PROC_FLAG_CONSOLE_ATTACHED`) can redeem via `use_grant`. Grants
/// are one-shot: the redeeming `use_grant` consumes the entry.
///
/// Errors:
///   - `Error::PermissionDenied`: caller doesn't hold
///     `CAP_GRANT_HOSTOWNER`.
///   - `Error::InvalidArgument`: `caps` contains a bit other than
///     `HOSTOWNER`, OR the grant table is full, OR a duplicate is
///     already pending. The v1.0 kernel collapses; v1.x will
///     distinguish.
pub fn grant(caps: Caps, target_stripes: Stripes) -> Result<()> {
    // SAFETY: t_cap_grant takes only u64 scalars; no user-VA args.
    let rc = unsafe { t_cap_grant(caps.bits(), target_stripes) };
    if rc < 0 {
        return Err(Error::PermissionDenied);
    }
    Ok(())
}

/// Register a pending CLEARANCE grant of `caps` against `target_stripes`
/// (A-4a, the legate grant-side path). Caller must hold
/// `CAP_GRANT_CLEARANCE` (corvus, the clearance authority).
///
/// `caps` must be a non-empty subset of [`Caps::GRANTABLE_CLEARANCE`];
/// `valid_for_ns` is the legate lifetime duration (0 = no time bound, the
/// scope ends only on the legate root's exit); `session_id` is the
/// caller's audit tag (nonzero, fits u32). The target redeems via
/// [`use_grant`] (the same `cap` device `use` file as the hostowner path)
/// and becomes a legate root: its caps gain `caps & self_restriction` and
/// the kernel records the scope.
///
/// Errors:
///   - `Error::PermissionDenied`: caller lacks `CAP_GRANT_CLEARANCE`, OR
///     `caps` escapes `GRANTABLE_CLEARANCE` / is empty, OR `target_stripes`
///     is 0, OR `session_id` is 0 / exceeds u32, OR the grant table is
///     full. The v1.0 kernel collapses these to -1.
pub fn grant_clearance(
    caps: Caps,
    target_stripes: Stripes,
    valid_for_ns: u64,
    session_id: u64,
) -> Result<()> {
    // SAFETY: t_cap_grant_clearance takes only u64 scalars; no user-VA args.
    let rc =
        unsafe { t_cap_grant_clearance(caps.bits(), target_stripes, valid_for_ns, session_id) };
    if rc < 0 {
        return Err(Error::PermissionDenied);
    }
    Ok(())
}

/// Redeem a pending grant of `caps` for the calling Proc's own
/// stripes.
///
/// Caller must hold `PROC_FLAG_CONSOLE_ATTACHED` AND have a non-
/// expired pending grant with matching `caps`. On success the caller's
/// cap set gains `caps`; the grant entry is consumed.
///
/// Errors:
///   - `Error::PermissionDenied`: caller lacks
///     `PROC_FLAG_CONSOLE_ATTACHED`.
///   - `Error::NotFound`: no pending grant matches `caps`.
///   - `Error::InvalidArgument`: defense-in-depth on a malformed
///     `caps` argument.
pub fn use_grant(caps: Caps) -> Result<()> {
    // SAFETY: t_cap_use takes only a u64 scalar.
    let rc = unsafe { t_cap_use(caps.bits()) };
    if rc < 0 {
        // Distinguishing the failure modes is a v1.x kernel ABI lift
        // (docs/ERRORS.md). At v1.0 every failure collapses to -1;
        // we map to PermissionDenied as the most-common cause.
        return Err(Error::PermissionDenied);
    }
    Ok(())
}
