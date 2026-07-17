// libthyla-rs::time — sleep + Duration re-export.
//
// Foundation chunk: U-2g per docs/UTOPIA-SHELL-DESIGN.md section 15.6.11.
//
// `sleep` is built on the only ambient-time primitive the kernel has for
// blocking -- `SYS_TORPOR_WAIT` with a timeout against a stack-local sentinel
// atomic the caller never wakes (kernel returns `-ETIMEDOUT` -> `Ok(())`; nobody
// else has the atomic's address, so spurious matches are impossible).
//
// Timestamps (`now`/`Instant`/`SystemTime`, below) land on `CLOCK_MONOTONIC` /
// `CLOCK_REALTIME`: the #343 vDSO fast-path (CNTVCT_EL0 + the kernel timekeeping
// page, no syscall -- read_clock -> crate::vdso_now_ns) with a
// `SYS_CLOCK_GETTIME` (LS-K) fallback when the page is absent.
//
// `Duration` is re-exported from `core::time` since libthyla-rs is no_std.

pub use core::time::Duration;
use core::sync::atomic::AtomicU32;

use crate::err::{Error, Result};
use crate::torpor;
use crate::{T_CLOCK_MONOTONIC, T_CLOCK_REALTIME, t_clock_gettime, t_clock_settime};

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
    // else can address this stack slot, so for a SURVIVING caller the
    // wait can only end via timeout. (LS-5c: a terminate-disposition
    // `interrupt` -- and group-exit death -- DOES end the wait early,
    // but only on the way to terminating the whole Proc at the kernel
    // return tail, so this function never observes it returning.)
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

// ---------------------------------------------------------------------------
// LS-K: monotonic + wall-clock timestamps over SYS_CLOCK_GETTIME (ARCH §22.6).
// ---------------------------------------------------------------------------

// Mirror of the kernel's `struct t_timespec` (syscall.h): 16 bytes, two i64.
#[repr(C)]
struct TimeSpec {
    tv_sec: i64,
    tv_nsec: i64,
}

// Read a clock into a Duration. Fast-path: the #343 vDSO page (CNTVCT_EL0 + the
// kernel timekeeping page, no syscall) when present. Fallback: SYS_CLOCK_GETTIME
// (an older kernel without the vDSO). The kernel only returns non-zero on a bad
// clk_id (we pass the two valid constants) or a bad buffer (ours is a valid
// stack slot), so a non-zero return is unreachable here -- map it to ZERO
// defensively. tv_sec >= 0 (epoch or uptime); tv_nsec is clamped to [0, 1e9).
fn read_clock(clk_id: u64) -> Duration {
    if let Some(ns) = crate::vdso_now_ns(clk_id) {
        return Duration::new(ns / 1_000_000_000, (ns % 1_000_000_000) as u32);
    }
    let mut ts = TimeSpec { tv_sec: 0, tv_nsec: 0 };
    let rc = unsafe { t_clock_gettime(clk_id, &mut ts as *mut TimeSpec as u64) };
    if rc != 0 {
        return Duration::ZERO;
    }
    Duration::new(ts.tv_sec.max(0) as u64, ts.tv_nsec.clamp(0, 999_999_999) as u32)
}

/// A monotonic timestamp (`CLOCK_MONOTONIC` -- nanoseconds since boot). Never
/// goes backward; use it to measure elapsed intervals.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct Instant(Duration);

impl Instant {
    /// The current monotonic time.
    pub fn now() -> Instant {
        Instant(read_clock(T_CLOCK_MONOTONIC))
    }

    /// The Duration elapsed since this Instant (saturating at zero).
    pub fn elapsed(&self) -> Duration {
        Instant::now().0.saturating_sub(self.0)
    }

    /// The Duration from `earlier` to `self` (saturating at zero).
    pub fn duration_since(&self, earlier: Instant) -> Duration {
        self.0.saturating_sub(earlier.0)
    }
}

/// A wall-clock timestamp (`CLOCK_REALTIME`), as a Duration since the Unix
/// epoch. May read `1970 + uptime` on a platform with no real-time clock (the
/// fail-soft path; ARCH §22.6).
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub struct SystemTime(Duration);

impl SystemTime {
    /// 1970-01-01T00:00:00Z.
    pub const UNIX_EPOCH: SystemTime = SystemTime(Duration::ZERO);

    /// The current wall-clock time.
    pub fn now() -> SystemTime {
        SystemTime(read_clock(T_CLOCK_REALTIME))
    }

    /// A wall-clock time `secs`.`nsecs` after the Unix epoch (`nsecs` clamped to
    /// [0, 1e9)). The SNTP client builds its computed time this way before
    /// stepping the clock.
    pub fn from_unix(secs: u64, nsecs: u32) -> SystemTime {
        SystemTime(Duration::new(secs, nsecs.min(999_999_999)))
    }

    /// The Duration since the Unix epoch.
    pub fn since_epoch(&self) -> Duration {
        self.0
    }

    /// The Duration from `earlier` to `self`; `Err` (carrying the back-step
    /// amount) if `earlier` is later. Like std's `duration_since`, with the
    /// `SystemTimeError` collapsed to the Duration it would carry.
    pub fn duration_since(&self, earlier: SystemTime) -> core::result::Result<Duration, Duration> {
        if self.0 >= earlier.0 {
            Ok(self.0 - earlier.0)
        } else {
            Err(earlier.0 - self.0)
        }
    }
}

/// Step `CLOCK_REALTIME` to `t` (net-7a; the SNTP client's clock-step path).
/// Requires `CAP_HOSTOWNER` -- a clock step is system-global, so it is the host
/// owner's authority. `CLOCK_MONOTONIC` is unaffected.
///
/// Errors:
///   - `Error::PermissionDenied`: the caller lacks `CAP_HOSTOWNER` (the common,
///     expected case for a non-elevated tool).
///   - `Error::InvalidArgument`: the time is out of range (only reachable for an
///     epoch beyond ~year 2286; `SystemTime::now`/`from_unix` stay well within).
///   - `Error::BadAddress` / `Error::Io`: defense-in-depth on the kernel return.
pub fn set_realtime(t: SystemTime) -> Result<()> {
    let ts = TimeSpec { tv_sec: t.0.as_secs() as i64, tv_nsec: t.0.subsec_nanos() as i64 };
    let rc = unsafe { t_clock_settime(T_CLOCK_REALTIME, &ts as *const TimeSpec as u64) };
    Error::from_syscall_return(rc).map(|_| ())
}
