//! The GPU command fence sequence.
//!
//! Every command carries a fence id, and the id space is smaller than it
//! looks: legacy virgl completion callbacks return a 32-bit id, and the device
//! retires a legacy-fenced command when `its id <= the signalled id`. Two
//! rules follow. The sequence must be MONOTONE AMONG THE COMMANDS IN FLIGHT,
//! or an early completion retires a later command; and it must never pass
//! `FENCE_LIMIT`, or the truncated echo never matches and the command never
//! retires at all.
//!
//! A bare counter therefore latches the engine dead after 2^32 commands --
//! weeks of desktop uptime. The sequence instead REWINDS to 1 when it has
//! passed `FENCE_REWIND_AT` and the device holds nothing: with no command in
//! flight there is no id for the rewound one to be compared against, so
//! monotonicity among in-flight commands is kept exactly. The rewind point is
//! 2^31 rather than the limit so ids also stay inside the positive range of
//! the `int` the legacy renderer entry point takes, whenever the device is
//! ever idle -- which a compositor is, between every frame. A device that is
//! never once idle across the last 2^31 ids fails closed, as it always did.

pub const FENCE_LIMIT: u64 = u32::MAX as u64;
pub const FENCE_REWIND_AT: u64 = 1 << 31;

/// Test builds start just short of the rewind point, so every gate that
/// reaches the desktop has crossed a real rewind against the real device.
pub const FENCE_START: u64 = if cfg!(feature = "test-mode") { FENCE_REWIND_AT - 64 } else { 0 };

/// The id after `current`. `device_idle` means the device holds no command
/// chain at all -- no tagged chain, and no abandoned one still to retire.
/// `None` is exhaustion; the caller fails closed.
pub fn next(current: u64, device_idle: bool) -> Option<u64> {
    let from = if device_idle && current >= FENCE_REWIND_AT { 0 } else { current };
    from.checked_add(1).filter(|id| *id <= FENCE_LIMIT)
}

/// Whether `next(current, device_idle)` is a rewind (for the witness line).
pub fn rewinds(current: u64, device_idle: bool) -> bool {
    device_idle && current >= FENCE_REWIND_AT
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn counts_up_below_the_rewind_point_whether_or_not_the_device_is_idle() {
        for idle in [false, true] {
            assert_eq!(next(0, idle), Some(1));
            assert_eq!(next(41, idle), Some(42));
            assert_eq!(next(FENCE_REWIND_AT - 2, idle), Some(FENCE_REWIND_AT - 1));
            assert_eq!(next(FENCE_REWIND_AT - 1, idle), Some(FENCE_REWIND_AT));
            assert!(!rewinds(FENCE_REWIND_AT - 1, idle));
        }
    }

    #[test]
    fn rewinds_only_when_the_device_holds_nothing() {
        assert_eq!(next(FENCE_REWIND_AT, true), Some(1));
        assert!(rewinds(FENCE_REWIND_AT, true));
        // Busy: a rewound id would sit BELOW ids still in flight, and the
        // first of those to signal would retire it early.
        assert_eq!(next(FENCE_REWIND_AT, false), Some(FENCE_REWIND_AT + 1));
        assert!(!rewinds(FENCE_REWIND_AT, false));
        assert_eq!(next(FENCE_LIMIT - 1, false), Some(FENCE_LIMIT));
        assert_eq!(next(FENCE_LIMIT - 1, true), Some(1));
    }

    #[test]
    fn exhaustion_fails_closed_only_for_a_device_that_was_never_idle() {
        assert_eq!(next(FENCE_LIMIT, false), None);
        assert_eq!(next(FENCE_LIMIT, true), Some(1));
        assert_eq!(next(u64::MAX, false), None);
    }

    #[test]
    fn a_run_of_commands_never_leaves_the_id_space_and_never_repeats_in_flight() {
        // A compositor's shape: bursts of three commands, idle between them.
        let mut cur = FENCE_REWIND_AT - 5;
        let mut rewound = 0;
        for burst in 0..8 {
            let mut in_flight: [u64; 3] = [0; 3];
            for (i, slot) in in_flight.iter_mut().enumerate() {
                let idle = i == 0;
                if rewinds(cur, idle) { rewound += 1; }
                cur = next(cur, idle).expect("never exhausts while idle between bursts");
                assert!(cur >= 1 && cur <= FENCE_LIMIT, "burst {burst}");
                *slot = cur;
            }
            assert!(in_flight[0] < in_flight[1] && in_flight[1] < in_flight[2],
                "monotone among the commands in flight");
        }
        assert_eq!(rewound, 1);
        assert!(cur < FENCE_REWIND_AT);
    }

    #[test]
    fn test_builds_start_short_of_the_rewind_point_and_production_at_zero() {
        if cfg!(feature = "test-mode") {
            assert!(FENCE_START < FENCE_REWIND_AT && FENCE_REWIND_AT - FENCE_START <= 64);
        } else {
            assert_eq!(FENCE_START, 0);
        }
    }
}
