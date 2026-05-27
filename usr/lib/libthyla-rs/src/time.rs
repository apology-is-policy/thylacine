// libthyla-rs::time — sleep + Duration re-export.
//
// Foundation chunk: U-2g per docs/UTOPIA-SHELL-DESIGN.md section 15.6.11.
//
// At v1.0 the kernel exposes NO native SYS_CLOCK_GETTIME / SYS_NANOSLEEP
// surface. `sleep` is built on the only ambient-time primitive the
// kernel has -- `SYS_TORPOR_WAIT` with a timeout against an atomic
// the caller never wakes. The kernel returns `-ETIMEDOUT` when the
// timeout lapses; we map that to `Ok(())`. The atomic itself is a
// stack-local sentinel; nobody else has its address, so spurious
// matches are impossible.
//
// `Duration` is re-exported from `core::time` since libthyla-rs is
// no_std (no `std::time`).
//
// v1.x extensions (when the kernel grows the surface):
//   - `now()`: monotonic + wall clock.
//   - `Instant`: timestamp newtype.
//   - `SystemTime`: epoch-based timestamp.

pub use core::time::Duration;
use core::sync::atomic::AtomicU32;

use crate::err::{Error, Result};
use crate::torpor;

/// Block the calling Thread for at least `dur`.
///
/// Built on `SYS_TORPOR_WAIT` with a stack-local sentinel atomic and
/// a non-matching expected -- the kernel sleeps the calling thread
/// for the timeout and returns ETIMEDOUT. We map that to success.
///
/// `Duration::ZERO` is a no-op (returns immediately).
///
/// Durations longer than `T_TORPOR_MAX_TIMEOUT_US` (1 hour) saturate
/// at the kernel cap; callers wanting longer sleeps should loop. A
/// v1.x extension may add `sleep_until(deadline)` once a monotonic
/// clock exists.
///
/// Errors:
///   - `Error::Io`: defense-in-depth on a kernel return that's
///     neither 0 nor -ETIMEDOUT. Unreachable in practice.
pub fn sleep(dur: Duration) -> Result<()> {
    if dur.is_zero() {
        return Ok(());
    }
    // Stack-local sentinel. We initialize to 0 and pass expected = 0,
    // so the kernel's "value matches => sleep" path is taken. Nobody
    // else can address this stack slot, so the wait can only end via
    // timeout. (A signal can't preempt it -- SYS_TORPOR_WAIT is not
    // interruptible by notes at v1.0.)
    let sentinel = AtomicU32::new(0);
    match torpor::wait(&sentinel, 0, Some(dur)) {
        Ok(torpor::WaitResult::TimedOut) => Ok(()),
        Ok(torpor::WaitResult::Woken) => {
            // Unreachable: nobody else holds the sentinel's address.
            // Defense-in-depth.
            Ok(())
        }
        Ok(torpor::WaitResult::ValueMismatch) => {
            // Unreachable: we wrote 0 ourselves and passed expected = 0.
            // Defense-in-depth.
            Ok(())
        }
        Err(e) => Err(e),
    }
}

// `Error` is imported for the public surface only; the inner Result<()>
// uses `?` propagation through torpor::wait already.
#[allow(dead_code)]
fn _force_error_in_scope(_e: Error) {}
