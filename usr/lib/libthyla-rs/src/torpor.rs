// libthyla-rs::torpor — futex-style wait-on-address primitive.
//
// Surfaces SYS_TORPOR_WAIT / SYS_TORPOR_WAKE as typed Rust functions
// over `core::sync::atomic::AtomicU32`. The kernel-side guarantee
// (CLAUDE.md "Spec-to-code suspended -- validated by reasoning, not
// specs/futex.tla"): a consumer that PARKS atomically loads under
// `torpor_lock` and registers before sleeping; the producer's WAKE
// takes the same lock to walk. No lost wakeup. (A value MISMATCH may
// return without the lock -- no waiter registers on that path, and
// the caller's mandatory re-check below carries its own ordering.)
//
// Foundation chunk: U-2g per docs/UTOPIA-SHELL-DESIGN.md section 15.6.10.
//
// NAMING:
//   `torpor` is the marsupial deep-sleep state (CLAUDE.md "Thematic
//   naming"). The kernel side already uses this name; userspace
//   mirrors. POSIX terminology would call this a futex (fast userspace
//   mutex); Linux calls it `futex(2)`. The semantics are the same.
//
// USAGE:
//   The standard pattern for "wait until *addr changes from v":
//
//     while addr.load(Acquire) == expected {
//         match torpor::wait(&addr, expected, None)? {
//             WaitResult::Woken | WaitResult::ValueMismatch => break,
//             WaitResult::TimedOut => unreachable!(), // timeout=None
//         }
//     }
//
//   The check-then-wait race is closed by the kernel doing its own
//   load under the same lock that wake takes; if the producer's
//   store + wake interleaved between userspace's load and the syscall
//   entry, the kernel observes the new value and returns
//   `ValueMismatch` immediately without sleeping.
//
// SPURIOUS WAKES:
//   `WaitResult::Woken` is best-effort, like every futex API in the
//   wild. Callers MUST re-check the atomic value before assuming
//   progress.
//
// CROSS-PROC SHARED FUTEX:
//   Not at v1.0 -- the kernel hashes by `(Proc *, addr_va)`, so a
//   shared mapping in two Procs uses two distinct queues. Cross-Proc
//   shared-futex waits on Tier-2 burrows (POUCH-DESIGN.md section 10).

use core::sync::atomic::AtomicU32;
use core::time::Duration;

use crate::err::{Error, Result};
use crate::{
    t_torpor_wait, t_torpor_wake, T_TORPOR_MAX_TIMEOUT_US, T_TORPOR_TIMEOUT_INDEFINITE,
};

// =============================================================================
// WaitResult — three outcomes of a wait.
// =============================================================================

/// Outcome of a `torpor::wait` call.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum WaitResult {
    /// A producer's `wake` matched. The atomic word may have changed;
    /// caller MUST re-load and re-check.
    Woken,
    /// The atomic word didn't match `expected` at the kernel's
    /// load-under-lock; no sleep happened.
    ValueMismatch,
    /// The timeout lapsed without a wake.
    TimedOut,
}

// =============================================================================
// wait + wake.
// =============================================================================

/// Block on `addr` until a producer wakes us OR `*addr != expected`
/// is observed under the kernel's torpor lock.
///
/// `timeout`:
///   - `None`              => block indefinitely.
///   - `Some(Duration)`    => block at most that long. Saturates at
///     `T_TORPOR_MAX_TIMEOUT_US` (1 hour) -- larger durations are
///     clamped. `Duration::ZERO` is a probe that returns `TimedOut`
///     if `*addr` still matches `expected`.
///
/// The kernel's no-lost-wakeup proof relies on the standard futex
/// discipline: callers must `addr.load(Acquire) == expected` before
/// calling `wait`; if a producer stores + wakes between the load and
/// the syscall, the kernel observes the new value and returns
/// `ValueMismatch` without sleeping.
///
/// Errors:
///   - `Error::InvalidArgument`: bad alignment (the caller's
///     `AtomicU32` is by-construction 4-byte aligned; this can only
///     happen if a future API extension passes a misaligned address),
///     OR a `Duration` so large it overflows the kernel's i64 micro-
///     second representation (unreachable at v1.0 after our saturation).
///   - `Error::BadAddress`: `addr` was mapped at the time the kernel
///     loaded but became unmapped (or rights-denied) before the
///     compare ran. Unreachable with `&AtomicU32` borrowed for the
///     call.
pub fn wait(addr: &AtomicU32, expected: u32, timeout: Option<Duration>) -> Result<WaitResult> {
    let timeout_us = duration_to_kernel_us(timeout);
    // SAFETY: `&AtomicU32` guarantees alignment + lifetime for the
    // syscall's duration.
    let rc = unsafe {
        t_torpor_wait(
            addr as *const AtomicU32 as *const u32,
            expected,
            timeout_us,
        )
    };
    match rc {
        0 => Ok(WaitResult::Woken),
        // ETIMEDOUT = 110. Saturating arithmetic above means we won't
        // mis-attribute a kernel error to a timeout.
        n if n == -110 => Ok(WaitResult::TimedOut),
        n if n < 0 => {
            // Other errno -- map through the canonical decoder.
            // (Note: kernel returns explicit -errno here, NOT -1.)
            let err = Error::from_syscall_return(n).err().unwrap_or(Error::Io);
            // -EINVAL on value-mismatch fast path: actually no, the
            // kernel returns 0 on value-mismatch (matching the "woken
            // OR mismatch" pattern). Real -EINVAL means a bad arg.
            Err(err)
        }
        // Any positive rc would be a kernel ABI violation; defense.
        _ => Err(Error::Io),
    }
}

/// Wake up to `count` waiters on `addr`. `count == u32::MAX` is the
/// wake-all convention (pthread_cond_broadcast).
///
/// Returns the number of waiters actually woken (>= 0).
///
/// Errors:
///   - `Error::InvalidArgument`: bad alignment (unreachable with
///     `&AtomicU32`).
pub fn wake(addr: &AtomicU32, count: u32) -> Result<u32> {
    // SAFETY: `&AtomicU32` is 4-byte aligned by construction.
    let rc = unsafe { t_torpor_wake(addr as *const AtomicU32 as *const u32, count) };
    if rc < 0 {
        return Err(Error::from_syscall_return(rc).err().unwrap_or(Error::Io));
    }
    Ok(rc as u32)
}

/// Wake one waiter -- shorthand for `wake(addr, 1)`. Matches
/// `pthread_cond_signal` semantics.
pub fn wake_one(addr: &AtomicU32) -> Result<u32> {
    wake(addr, 1)
}

/// Wake every waiter -- shorthand for `wake(addr, u32::MAX)`. Matches
/// `pthread_cond_broadcast` semantics.
pub fn wake_all(addr: &AtomicU32) -> Result<u32> {
    wake(addr, u32::MAX)
}

// =============================================================================
// Internal — Duration to kernel timeout_us.
// =============================================================================

pub(crate) fn duration_to_kernel_us(timeout: Option<Duration>) -> i64 {
    match timeout {
        None => T_TORPOR_TIMEOUT_INDEFINITE,
        Some(d) => {
            // Duration::as_micros returns u128; the kernel's cap is
            // T_TORPOR_MAX_TIMEOUT_US (3.6e9 us); saturate.
            let us_u128 = d.as_micros();
            if us_u128 > T_TORPOR_MAX_TIMEOUT_US as u128 {
                T_TORPOR_MAX_TIMEOUT_US
            } else {
                us_u128 as i64
            }
        }
    }
}
