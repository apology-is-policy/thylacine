// libthyla-rs::sched -- per-thread scheduling attributes (priority + CPU affinity).
//
// FORWARD-DESIGN SEAM (user-directed 2026-06-22): priority + affinity are a
// system we want, just not built yet. There is NO kernel syscall for it at
// v1.0, so EVERY function here returns `Error::NotImplemented` today. This module
// is the SINGLE PLUG POINT: when a `SYS_SCHED_SETATTR` (or equivalent) lands,
// only these bodies change, and every consumer (cpubench's mixed/affinity modes,
// ut's interactive-shell lean, a future `nice`/`taskset`) lights up at once.
//
// Keeping the call sites in the tree NOW -- gated on this honest stub -- is what
// makes the future system "easily pluggable": the measurement scaffold, the API
// shape, and the consumers already exist; the kernel half is the only gap.
//
// API SHAPE (deliberately minimal + plausible, so the future syscall design is
// not boxed in):
//   * `priority`: a signed niceness-like value, Linux-convention -- LOWER means
//     MORE CPU / more interactive (so -20 = most-favoured, +19 = most-yielding,
//     0 = default). The kernel's EEVDF band model (INTERACTIVE/NORMAL/IDLE) maps
//     onto ranges of this when the syscall is designed; callers should use the
//     `prio::*` consts rather than bare integers so a band remap is transparent.
//   * `affinity_mask`: a bitset of permitted CPUs (bit c set => may run on CPU
//     c). 0 is the sentinel for "unset / all CPUs" so a default-constructed attr
//     is a no-op.

use crate::err::{Error, Result};

/// Niceness-like priority constants. Lower = more CPU (Linux convention). Use
/// these rather than bare integers so a future band remap stays transparent.
pub mod prio {
    /// Most-favoured (the INTERACTIVE band lean -- e.g. a foreground shell).
    pub const INTERACTIVE: i32 = -10;
    /// The default band (NORMAL).
    pub const NORMAL: i32 = 0;
    /// Most-yielding (the IDLE/batch band -- runs only when nothing else wants the CPU).
    pub const IDLE: i32 = 19;
}

/// The full per-thread scheduling attribute set. A default-constructed value
/// (`NORMAL` priority, affinity 0 = all CPUs) is a no-op.
#[derive(Clone, Copy, Debug)]
pub struct SchedAttr {
    /// Niceness-like priority; see `prio`. Lower = more CPU.
    pub priority: i32,
    /// Permitted-CPU bitset; bit c set => may run on CPU c. 0 = all CPUs.
    pub affinity_mask: u64,
}

impl Default for SchedAttr {
    fn default() -> Self {
        SchedAttr { priority: prio::NORMAL, affinity_mask: 0 }
    }
}

/// A single-CPU affinity mask (pin to exactly `cpu`).
pub const fn affinity_for(cpu: u32) -> u64 {
    if cpu >= 64 {
        0
    } else {
        1u64 << cpu
    }
}

/// True iff a `SYS_SCHED_SETATTR`-class syscall is wired. Today: always false.
/// A caller uses this to choose between the real-measurement path and a
/// "scaffold ready, not yet enforced" report (see cpubench's mixed/affinity).
#[inline]
pub fn is_supported() -> bool {
    false
}

/// Set the calling thread's priority. The single plug point for a future
/// `SYS_SCHED_SETATTR` (priority axis). Today: `Err(NotImplemented)`.
pub fn set_self_priority(_priority: i32) -> Result<()> {
    // PLUG POINT: when the syscall lands, marshal `_priority` and SVC here.
    Err(Error::NotImplemented)
}

/// Set the calling thread's CPU affinity mask. The single plug point for the
/// affinity axis. Today: `Err(NotImplemented)`.
pub fn set_self_affinity(_cpu_mask: u64) -> Result<()> {
    // PLUG POINT: when the syscall lands, marshal `_cpu_mask` and SVC here.
    Err(Error::NotImplemented)
}

/// Apply a full `SchedAttr` to the calling thread. Today: `Err(NotImplemented)`.
/// A default attr (NORMAL + mask 0) is treated as a no-op success so callers can
/// unconditionally apply without branching on "is this the default".
pub fn set_self_attr(attr: &SchedAttr) -> Result<()> {
    if attr.priority == prio::NORMAL && attr.affinity_mask == 0 {
        return Ok(());
    }
    // PLUG POINT: a real impl applies both axes in one syscall.
    Err(Error::NotImplemented)
}
