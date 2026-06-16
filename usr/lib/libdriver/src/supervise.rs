// The supervision decision -- the pure policy half of the warden's restart loop
// (MENAGERIE.md section 5). Given the outcome of one driver run, the manifest's
// restart policy, and how many restarts have already been spent on this device,
// decide whether to re-spawn (with a back-off delay) or settle on a terminal
// disposition.
//
// PURE (no libthyla-rs), like `manifest` + `resource` + `source`: the state
// machine -- the part with the subtle edges (crash-vs-clean, the give-up bound,
// the policy cross-product) -- is exercised on the host. The warden owns the
// impure half (spawn / poll readiness / reap / sleep) and merely feeds each run's
// result in and acts on the returned step.
//
// The load-bearing distinction is `GaveUp` (SOFT) vs `Failed` (HARD): a driver
// that crashes and exhausts its restarts leaves its device unavailable but the
// system fine -- it must NOT fail the boot. Only a structural failure (the warden
// could not even spawn the binary) is hard. The warden's exit code keys on the
// hard count alone.

use crate::manifest::Restart;

/// The result of one driver run attempt, as the supervisor sees it. The warden
/// maps its impure spawn/readiness/reap result into one of these.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RunOutcome {
    /// The driver signalled readiness and served until the warden removed it
    /// (DeviceRemoved), i.e. a clean bring-up. Not a candidate for restart --
    /// the removal was deliberate.
    Served,
    /// The driver exited on its own with this status code (`None` = killed with
    /// no code, e.g. a hung driver the warden had to terminate -- treated as a
    /// crash). The supervisor reads crash-vs-clean as `code != Some(0)`.
    ///
    /// v1.0 SEAM: the kernel collapses every non-"ok" exit to status `1`
    /// (`sys_exits_handler`; the structured 64-bit exit_status is a v1.x lift per
    /// docs/ERRORS.md), so the warden only ever feeds `Some(0)` or `Some(1)` here
    /// -- the supervisor distinguishes clean-vs-crashed, NOT specific codes. A
    /// finer policy (e.g. do-not-restart `EXIT_BIND` since it is a warden bug, vs
    /// restart `EXIT_PROBE`) needs the structured status and lands with it.
    /// `next_step` already handles arbitrary codes, so no change is owed here when
    /// it arrives -- only the warden's `RunOutcome` mapping.
    Exited(Option<i32>),
    /// The warden could not spawn or track the driver (a missing binary, an OOM,
    /// a descriptor-encode bug) -- structural, never a restart candidate.
    HardFail,
}

/// A device's terminal supervision disposition -- what the warden tallies.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Disposition {
    /// The device came up (served then removed, or a clean one-shot proof).
    Up,
    /// The driver crashed and the supervisor exhausted its restarts (or the
    /// policy was `never`). A SOFT per-device failure: the device is
    /// unavailable, the system is not. Does NOT fail the boot.
    GaveUp,
    /// A structural failure (the warden could not spawn/track the driver). A
    /// HARD failure -- fails the boot, since it signals a misconfiguration (a
    /// driver binary that did not bake) rather than a device that misbehaved.
    Failed,
}

/// What the supervisor should do next after one run.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SuperviseStep {
    /// Re-spawn the driver after `delay_ms` of back-off. The caller increments
    /// the restart count it passes back on the next `next_step`.
    Restart { delay_ms: u32 },
    /// Stop. The device's terminal disposition.
    Settle(Disposition),
}

/// The maximum number of restarts the supervisor will spend on a crashing driver
/// before giving up (a total of `RESTART_LIMIT + 1` spawns: the initial plus the
/// restarts). Small + bounded: a crash-loop must converge quickly to "device
/// unavailable" rather than wedge the boot ladder.
pub const RESTART_LIMIT: u32 = 3;

/// The base back-off before the first restart (milliseconds). Doubles per
/// restart, capped at `BACKOFF_MAX_MS`.
pub const BACKOFF_BASE_MS: u32 = 50;

/// The back-off cap (milliseconds) -- a crashing driver never delays the ladder
/// by more than this between attempts.
pub const BACKOFF_MAX_MS: u32 = 500;

/// The back-off before the `restart_n`-th restart (1-based). Exponential from
/// `BACKOFF_BASE_MS`, capped at `BACKOFF_MAX_MS`. `restart_n == 0` is no delay.
/// Saturating, so a pathological count can never wrap.
pub fn backoff_ms(restart_n: u32) -> u32 {
    if restart_n == 0 {
        return 0;
    }
    // Cap the shift well before u32's width so `1 << shift` never overflows; the
    // BACKOFF_MAX_MS clamp dominates long before then anyway.
    let shift = (restart_n - 1).min(16);
    BACKOFF_BASE_MS
        .saturating_mul(1u32 << shift)
        .min(BACKOFF_MAX_MS)
}

/// Decide the next supervision step from one run's outcome, the manifest policy,
/// and the restarts already spent on this device.
///
/// - `Served` -> `Up` (deliberate removal, never restart).
/// - `HardFail` -> `Failed` (structural, never restart).
/// - `Exited(code)`: crash = `code != Some(0)`. Whether to restart follows the
///   policy (`Never` no; `OnCrash` iff crashed; `Always` yes). When restarting
///   is wanted but `restarts_so_far` has reached `RESTART_LIMIT`, settle:
///   `GaveUp` if the last exit crashed, else `Up` (an `Always` one-shot did its
///   job each time -- we simply stop respawning).
pub fn next_step(outcome: RunOutcome, policy: Restart, restarts_so_far: u32) -> SuperviseStep {
    match outcome {
        RunOutcome::Served => SuperviseStep::Settle(Disposition::Up),
        RunOutcome::HardFail => SuperviseStep::Settle(Disposition::Failed),
        RunOutcome::Exited(code) => {
            let crashed = code != Some(0);
            let want_restart = match policy {
                Restart::Never => false,
                Restart::OnCrash => crashed,
                Restart::Always => true,
            };
            if !want_restart || restarts_so_far >= RESTART_LIMIT {
                return SuperviseStep::Settle(if crashed {
                    Disposition::GaveUp
                } else {
                    Disposition::Up
                });
            }
            SuperviseStep::Restart {
                delay_ms: backoff_ms(restarts_so_far + 1),
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Run the pure state machine to its terminal disposition, the way the
    /// warden's impure loop would (minus the actual spawn/sleep). `runs` yields
    /// the outcome of each successive attempt.
    fn drive(policy: Restart, mut runs: impl FnMut(u32) -> RunOutcome) -> (Disposition, u32) {
        let mut restarts = 0u32;
        let mut attempt = 0u32;
        loop {
            let outcome = runs(attempt);
            attempt += 1;
            match next_step(outcome, policy, restarts) {
                SuperviseStep::Settle(d) => return (d, restarts),
                SuperviseStep::Restart { .. } => restarts += 1,
            }
        }
    }

    #[test]
    fn served_is_up_under_every_policy() {
        for p in [Restart::Never, Restart::OnCrash, Restart::Always] {
            assert_eq!(
                next_step(RunOutcome::Served, p, 0),
                SuperviseStep::Settle(Disposition::Up)
            );
        }
    }

    #[test]
    fn hard_fail_is_failed_and_never_restarts() {
        for p in [Restart::Never, Restart::OnCrash, Restart::Always] {
            assert_eq!(
                next_step(RunOutcome::HardFail, p, 0),
                SuperviseStep::Settle(Disposition::Failed)
            );
        }
    }

    #[test]
    fn clean_oneshot_is_up_without_restart_under_oncrash() {
        assert_eq!(
            next_step(RunOutcome::Exited(Some(0)), Restart::OnCrash, 0),
            SuperviseStep::Settle(Disposition::Up)
        );
    }

    #[test]
    fn crash_under_oncrash_restarts_until_limit_then_gives_up() {
        // The crash-probe scenario: EXIT_PROBE (72) every time, on-crash policy.
        let (d, restarts) = drive(Restart::OnCrash, |_| RunOutcome::Exited(Some(72)));
        assert_eq!(d, Disposition::GaveUp);
        assert_eq!(restarts, RESTART_LIMIT); // RESTART_LIMIT restarts, then give up
    }

    #[test]
    fn crash_under_never_gives_up_immediately() {
        let (d, restarts) = drive(Restart::Never, |_| RunOutcome::Exited(Some(72)));
        assert_eq!(d, Disposition::GaveUp);
        assert_eq!(restarts, 0); // never restarted
    }

    #[test]
    fn clean_exit_under_never_is_up() {
        assert_eq!(
            next_step(RunOutcome::Exited(Some(0)), Restart::Never, 0),
            SuperviseStep::Settle(Disposition::Up)
        );
    }

    #[test]
    fn transient_crash_then_recovers_is_up() {
        // Crashes once, then comes up: counts as up, having spent one restart.
        let (d, restarts) = drive(Restart::OnCrash, |attempt| {
            if attempt == 0 {
                RunOutcome::Exited(Some(72))
            } else {
                RunOutcome::Served
            }
        });
        assert_eq!(d, Disposition::Up);
        assert_eq!(restarts, 1);
    }

    #[test]
    fn killed_no_code_counts_as_crash() {
        // A hung driver the warden terminated (no exit code) is a crash.
        assert_eq!(
            next_step(RunOutcome::Exited(None), Restart::OnCrash, 0),
            SuperviseStep::Restart {
                delay_ms: backoff_ms(1)
            }
        );
    }

    #[test]
    fn always_respawns_a_clean_exit_then_settles_up() {
        // `Always` re-spawns even a clean one-shot, bounded; the final settle is
        // Up (it served each time), not GaveUp.
        let (d, restarts) = drive(Restart::Always, |_| RunOutcome::Exited(Some(0)));
        assert_eq!(d, Disposition::Up);
        assert_eq!(restarts, RESTART_LIMIT);
    }

    #[test]
    fn backoff_is_exponential_and_capped() {
        assert_eq!(backoff_ms(0), 0);
        assert_eq!(backoff_ms(1), 50);
        assert_eq!(backoff_ms(2), 100);
        assert_eq!(backoff_ms(3), 200);
        assert_eq!(backoff_ms(4), 400);
        assert_eq!(backoff_ms(5), 500); // capped at BACKOFF_MAX_MS
        assert_eq!(backoff_ms(100), 500); // still capped, no overflow
    }
}
