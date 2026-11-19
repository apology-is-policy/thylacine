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

/// The poll timeout, in whole milliseconds, until tick `fired + 1` falls due:
/// 0 if it already has, else rounded UP so the wake is never early. Read
/// `elapsed_ns` just before the poll, so the pass's own work never delays a tick.
pub fn ms_until_tick(elapsed_ns: u64, hz: u32, fired: u64) -> i32 {
    let hz = hz.max(1) as u128;
    // The first nanosecond at which `fired + 1` ticks are due.
    let at_ns = ((fired as u128 + 1) * NS_PER_S).div_ceil(hz);
    let now = elapsed_ns as u128;
    if at_ns <= now {
        return 0;
    }
    (at_ns - now).div_ceil(NS_PER_MS).min(1000) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    const S: u64 = 1_000_000_000;
    const MS: u64 = 1_000_000;
    const RATES: [u32; 6] = [60, 15, 240, 7, 1, 144];

    fn at(k: u64, hz: u32) -> u64 {
        (k as u128 * NS_PER_S).div_ceil(hz as u128) as u64
    }

    // Drive the loop the way tapestryd does: tick when the due count passes
    // the fired count (at most one per pass), do `work_ns` of pass work, read
    // the clock again, sleep the timeout. Returns the frame_tick calls made by
    // the first pass at or past `secs`, and the latest a tick fired past its
    // exact boundary. A loop that passes without sleeping is a spin: it fails
    // here rather than hanging the test.
    fn run(hz: u32, secs: u64, work_ns: u64) -> (u64, u64) {
        let (mut t, mut fired, mut calls, mut late) = (0u64, 0u64, 0u64, 0u64);
        let mut passes = 0u64;
        loop {
            passes += 1;
            assert!(passes <= 4 * (secs * hz as u64 + 1), "hz {hz} work {work_ns}: the loop spins at t {t}");
            let due = ticks_due(t, hz);
            if due > fired {
                fired = due;
                calls += 1;
                late = late.max(t - at(due, hz));
            }
            if t >= secs * S {
                return (calls, late);
            }
            t += work_ns;
            t += ms_until_tick(t, hz, fired) as u64 * MS;
        }
    }

    #[test]
    fn the_clock_ticks_at_its_rate_and_the_pass_work_does_not_delay_it() {
        for hz in RATES {
            let period = S / hz as u64;
            for work in [0, 137_000, period / 2] {
                let (calls, late) = run(hz, 10, work);
                assert_eq!(calls, 10 * hz as u64, "hz {hz} work {work}");
                // A tick fires under a millisecond past its boundary: the
                // timeout rounds up by < 1 ms, and the pass work came before
                // the clock read the timeout was taken from.
                assert!(late < MS, "hz {hz} work {work}: a tick fired {late} ns late");
            }
        }
    }

    #[test]
    fn a_wake_is_never_early_and_never_skips_a_tick() {
        // Exactly one more tick due at the wake, none a millisecond before it.
        let check = |t: u64, hz: u32| {
            let fired = ticks_due(t, hz);
            let ms = ms_until_tick(t, hz, fired);
            assert!(ms >= 1, "hz {hz} t {t}: timeout {ms}");
            let wake = t + ms as u64 * MS;
            assert_eq!(ticks_due(wake, hz), fired + 1, "hz {hz} t {t}: wake {wake}");
            assert_eq!(ticks_due(wake - MS, hz), fired, "hz {hz} t {t}: wake {wake} overshot");
        };
        for hz in RATES {
            let mut t = 0u64;
            while t < 3 * S {
                check(t, hz);
                t += ms_until_tick(t, hz, ticks_due(t, hz)) as u64 * MS + 137_000;
            }
            // The phases a millisecond walk never lands on: a whole number of
            // milliseconds before a tick's exact (fractional) boundary, the
            // boundary itself, and a nanosecond either side of each.
            for k in 1..=2 * hz as u64 {
                for m in 0..=3u64 {
                    let b = at(k, hz) - m * MS;
                    for t in [b - 1, b, b + 1] {
                        check(t, hz);
                    }
                }
            }
        }
    }

    #[test]
    fn a_tick_already_due_is_polled_for_at_once() {
        // The pass ran past tick 1 (due at 16,666,667 ns at 60 Hz): no sleep.
        assert_eq!(ms_until_tick(16_666_667, 60, 0), 0);
        assert_eq!(ms_until_tick(40_000_000, 60, 1), 0);
        assert_eq!(ms_until_tick(16_666_666, 60, 0), 1);
        // A clock read behind the fired count sleeps, never spins.
        assert!(ms_until_tick(0, 60, 5) >= 1);
        assert_eq!(ms_until_tick(0, 1, 0), 1000);
    }

    #[test]
    fn the_rate_is_exact_where_the_old_period_was_fast() {
        // 1000/60 = 16 ms, 1000/15 = 66 ms, 1000/240 = 4 ms: the integer-ms
        // period ran 62.5, 15.15 and 250 Hz.
        assert_eq!(ticks_due(S, 60), 60);
        assert_eq!(ticks_due(S - 1, 60), 59);
        assert_eq!(ticks_due(S, 15), 15);
        assert_eq!(ticks_due(S, 240), 240);
        assert_eq!(ticks_due(16_666_666, 60), 0);
        assert_eq!(ticks_due(16_666_667, 60), 1);
        assert_eq!(ms_until_tick(0, 60, 0), 17);
        assert_eq!(ms_until_tick(16_000_000, 60, 0), 1);
    }
}
