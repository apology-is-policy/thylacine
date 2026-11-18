// The FRAME clock's schedule (TAPESTRY 18.4): a synthesized fixed-rate tick
// counted from an anchor, rate-exact. Tick k is due once k/hz seconds have
// passed since the anchor, so the count never drifts from `hz` per second
// however the millisecond poll timeouts round. An integer-millisecond period
// (1000 / hz) cannot do that: 1000 / 60 = 16 ms ticks at 62.5 Hz.

const NS_PER_S: u128 = 1_000_000_000;
const NS_PER_MS: u128 = 1_000_000;

/// How many ticks are due `elapsed_ns` after the anchor at `hz` (0 reads as 1).
pub fn ticks_due(elapsed_ns: u64, hz: u32) -> u64 {
    (elapsed_ns as u128 * hz.max(1) as u128 / NS_PER_S) as u64
}

/// The poll timeout, in whole milliseconds, until the next tick falls due:
/// rounded UP, so a wake is never early, and at least 1.
pub fn ms_until_next(elapsed_ns: u64, hz: u32) -> i32 {
    let hz = hz.max(1) as u128;
    let next = elapsed_ns as u128 * hz / NS_PER_S + 1;
    // The first nanosecond at which `next` ticks are due.
    let at_ns = (next * NS_PER_S + hz - 1) / hz;
    let remain_ns = at_ns - elapsed_ns as u128;
    let ms = (remain_ns + NS_PER_MS - 1) / NS_PER_MS;
    ms.clamp(1, 1000) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    const S: u64 = 1_000_000_000;

    // Drive the loop the way tapestryd does: tick when the due count passes
    // the fired count (at most one per pass), then sleep the poll timeout
    // plus a pass overhead. Returns the frame_tick calls made by the first
    // pass at or past `secs` -- a coalesced backlog counts once, as it does.
    fn run(hz: u32, secs: u64, overhead_ns: u64) -> u64 {
        let (mut t, mut fired, mut calls) = (0u64, 0u64, 0u64);
        loop {
            let due = ticks_due(t, hz);
            if due > fired {
                fired = due;
                calls += 1;
            }
            if t >= secs * S {
                return calls;
            }
            t += ms_until_next(t, hz) as u64 * 1_000_000 + overhead_ns;
        }
    }

    #[test]
    fn the_clock_ticks_at_its_rate_not_at_a_rounded_period() {
        for hz in [60u32, 15, 240, 7, 1, 144] {
            assert_eq!(ticks_due(10 * S, hz), 10 * hz as u64, "hz {hz}");
            assert_eq!(run(hz, 10, 0), 10 * hz as u64, "hz {hz}, no overhead");
        }
        // A pass that runs past the boundary coalesces nothing at these
        // overheads (each under one period): still the full count.
        assert_eq!(run(60, 10, 900_000), 600);
        assert_eq!(run(15, 10, 20_000_000), 150);
    }

    #[test]
    fn a_wake_is_never_early_and_never_skips_a_tick() {
        for hz in [60u32, 15, 240, 7, 1, 144] {
            let mut t = 0u64;
            while t < 3 * S {
                let ms = ms_until_next(t, hz);
                assert!(ms >= 1, "hz {hz} t {t}: timeout {ms}");
                let wake = t + ms as u64 * 1_000_000;
                // Never early: the wake has the next tick due.
                assert!(ticks_due(wake, hz) > ticks_due(t, hz), "hz {hz} t {t}: early wake");
                // Never late by a whole tick: one millisecond earlier it was not
                // yet two ticks on (a rounding-up wait overshoots by < 1 ms).
                assert!(
                    ticks_due(wake - 1_000_000, hz) <= ticks_due(t, hz) + 1,
                    "hz {hz} t {t}: slept past a tick"
                );
                t = wake + 137_000; // an odd pass overhead walks many phases
            }
            // The phases a millisecond walk never lands on: a whole number of
            // milliseconds before a tick's exact (fractional) boundary, and a
            // nanosecond either side of it.
            for k in 1..=2 * hz as u64 {
                let at = (k as u128 * 1_000_000_000).div_ceil(hz as u128) as u64;
                for m in 1..=3u64 {
                    for t in [at - m * 1_000_000 - 1, at - m * 1_000_000, at - m * 1_000_000 + 1] {
                        let wake = t + ms_until_next(t, hz) as u64 * 1_000_000;
                        assert!(ticks_due(wake, hz) > ticks_due(t, hz), "hz {hz} t {t}: early wake");
                    }
                }
            }
        }
    }

    #[test]
    fn the_rate_is_exact_where_the_old_period_was_fast() {
        // 1000/60 = 16 ms, 1000/15 = 66 ms, 1000/240 = 4 ms: the integer-ms
        // period ran 62.5, 15.15 and 250 Hz.
        assert_eq!(ticks_due(S, 60), 60);
        assert_eq!(ticks_due(S - 1, 60), 59);
        assert_eq!(ticks_due(S, 15), 15);
        assert_eq!(ticks_due(S, 240), 240);
        // The tick boundaries themselves: tick 1 at 60 Hz is due at
        // 16,666,667 ns, not 16,000,000.
        assert_eq!(ticks_due(16_666_666, 60), 0);
        assert_eq!(ticks_due(16_666_667, 60), 1);
        assert_eq!(ms_until_next(0, 60), 17);
        assert_eq!(ms_until_next(16_000_000, 60), 1);
    }
}
