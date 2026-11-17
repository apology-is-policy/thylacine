// The session's latency record (ASYNC-SERVICE-LIFECYCLE AS-4f; HALCYON-INTERACTION
// 12: "record render latency and idle wakeups before/after instead of asserting
// 'smooth' from a still image"). Test builds only: the session loop feeds it under
// the `test-mode` feature, which production strips (vote 136).
//
// A BURST opens at the first key the session hands a program and ends once nothing
// has happened for QUIET_NS with no key still owed its answer, or GIVE_UP_NS after
// the last event whatever is owed. Its one line says, in microseconds:
//   - key-to-present: from the moment a key went into a tile's input queue to the
//     return of that tile's first present after the program's next output (the
//     first unanswered key of a tile; later keys before that answer ride with it);
//   - frame: one tile's render + present, every one presented while the burst is
//     open;
//   - busy-max: the longest stretch from the loop's wake to its next wait, which
//     bounds how long an event can sit unread in the compositor's queues;
//   - wakes: how often the loop's wait returned while the burst was open.
// Nothing opens a burst but a key, so an idle session says nothing and the record
// adds no wake to it.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

/// A burst with nothing owed ends this long after its last event.
pub const QUIET_NS: u64 = 600_000_000;
/// A burst ends this long after its last event even with keys still owed.
pub const GIVE_UP_NS: u64 = 5_000_000_000;
/// Samples kept per burst and kind; past it a sample counts in `over` and the
/// maximum stays exact.
pub const SAMPLES_MAX: usize = 4096;

/// The value at the nearest rank `p` percent into `sorted` (ascending).
pub fn percentile(sorted: &[u32], p: usize) -> Option<u32> {
    if sorted.is_empty() {
        return None;
    }
    let rank = (p * sorted.len()).div_ceil(100).max(1);
    Some(sorted[rank.min(sorted.len()) - 1])
}

fn us(ns: u64) -> u32 {
    (ns / 1000).min(u32::MAX as u64) as u32
}

fn spread(samples: &mut [u32], max: u64) -> String {
    samples.sort_unstable();
    match (percentile(samples, 50), percentile(samples, 99)) {
        (Some(p50), Some(p99)) => format!("p50={} p99={} max={}", p50, p99, us(max)),
        _ => String::from("p50=- p99=- max=-"),
    }
}

#[derive(Default)]
pub struct Latency {
    open: bool,
    first_ns: u64,
    last_ns: u64,
    keys: u32,
    answered: u32,
    key_us: Vec<u32>,
    key_max_ns: u64,
    frames: u32,
    frame_us: Vec<u32>,
    frame_max_ns: u64,
    over: u32,
    busy_max_ns: u64,
    wakes: u32,
    woke_ns: Option<u64>,
}

impl Latency {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn is_open(&self) -> bool {
        self.open
    }

    fn keep(&mut self, kind_is_key: bool, ns: u64) {
        let v = if kind_is_key { &mut self.key_us } else { &mut self.frame_us };
        if v.len() < SAMPLES_MAX {
            v.push(us(ns));
        } else {
            self.over = self.over.saturating_add(1);
        }
    }

    /// A key went into a tile's input queue at `now`.
    pub fn key(&mut self, now: u64) {
        if !self.open {
            self.open = true;
            self.first_ns = now;
        }
        self.keys = self.keys.saturating_add(1);
        self.last_ns = self.last_ns.max(now);
    }

    /// The first present after the output answering a key queued at `queued`.
    pub fn answered(&mut self, queued: u64, now: u64) {
        if !self.open {
            return;
        }
        let ns = now.saturating_sub(queued);
        self.answered = self.answered.saturating_add(1);
        self.key_max_ns = self.key_max_ns.max(ns);
        self.keep(true, ns);
        self.last_ns = self.last_ns.max(now);
    }

    /// One tile's render + present, started at `start`, returned at `now`.
    pub fn frame(&mut self, start: u64, now: u64) {
        if !self.open {
            return;
        }
        let ns = now.saturating_sub(start);
        self.frames = self.frames.saturating_add(1);
        self.frame_max_ns = self.frame_max_ns.max(ns);
        self.keep(false, ns);
    }

    /// The loop's wait returned at `now`.
    pub fn woke(&mut self, now: u64) {
        self.woke_ns = Some(now);
        if self.open {
            self.wakes = self.wakes.saturating_add(1);
        }
    }

    /// The loop is about to wait at `now`.
    pub fn waiting(&mut self, now: u64) {
        if let Some(w) = self.woke_ns.take() {
            if self.open {
                self.busy_max_ns = self.busy_max_ns.max(now.saturating_sub(w));
            }
        }
    }

    /// Milliseconds until an open burst may end, for the loop's wait; `owed` is
    /// whether some tile still owes a key its answer.
    pub fn due_ms(&self, now: u64, owed: bool) -> Option<u64> {
        if !self.open {
            return None;
        }
        let at = self.last_ns + if owed { GIVE_UP_NS } else { QUIET_NS };
        Some(at.saturating_sub(now).div_ceil(1_000_000).max(1))
    }

    /// The burst's line, once it is over, and a fresh record; `owed` counts the
    /// tiles whose first unanswered key is still unanswered (the caller forgets
    /// those keys when a line comes back).
    pub fn end(&mut self, now: u64, owed: u32) -> Option<String> {
        if !self.open {
            return None;
        }
        let idle = now.saturating_sub(self.last_ns);
        if idle < GIVE_UP_NS && (owed > 0 || idle < QUIET_NS) {
            return None;
        }
        let keys = spread(&mut self.key_us, self.key_max_ns);
        let frames = spread(&mut self.frame_us, self.frame_max_ns);
        let line = format!(
            "halcyond: latency: keys={} answered={} unanswered={} key-to-present-us {} frames={} frame-us {} busy-max-us={} wakes={} span-ms={} over={}",
            self.keys,
            self.answered,
            owed,
            keys,
            self.frames,
            frames,
            us(self.busy_max_ns),
            self.wakes,
            self.last_ns.saturating_sub(self.first_ns) / 1_000_000,
            self.over
        );
        *self = Self::default();
        Some(line)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: u64 = 1_000_000;

    #[test]
    fn percentile_is_nearest_rank() {
        let v: Vec<u32> = (1..=100).collect();
        assert_eq!(percentile(&v, 50), Some(50));
        assert_eq!(percentile(&v, 99), Some(99));
        assert_eq!(percentile(&[7], 50), Some(7));
        assert_eq!(percentile(&[7], 99), Some(7));
        assert_eq!(percentile(&[1, 2], 50), Some(1));
        assert_eq!(percentile(&[1, 2], 99), Some(2));
        assert_eq!(percentile(&[], 50), None);
    }

    #[test]
    fn nothing_but_a_key_opens_a_burst() {
        let mut l = Latency::new();
        l.woke(MS);
        l.frame(MS, 2 * MS);
        l.answered(MS, 3 * MS);
        l.waiting(4 * MS);
        assert!(!l.is_open());
        assert_eq!(l.due_ms(5 * MS, false), None);
        assert_eq!(l.end(10_000 * MS, 0), None);
    }

    #[test]
    fn a_burst_reports_once_quiet_and_resets() {
        let mut l = Latency::new();
        l.woke(100 * MS);
        l.key(100 * MS);
        l.frame(101 * MS, 103 * MS);
        l.waiting(104 * MS);
        l.woke(110 * MS);
        l.answered(100 * MS, 112 * MS);
        l.waiting(113 * MS);
        assert_eq!(l.due_ms(112 * MS, false), Some(600));
        assert_eq!(l.end(711 * MS, 0), None);
        let line = l.end(712 * MS, 0).expect("quiet for QUIET_NS");
        assert_eq!(
            line,
            "halcyond: latency: keys=1 answered=1 unanswered=0 key-to-present-us p50=12000 p99=12000 max=12000 frames=1 frame-us p50=2000 p99=2000 max=2000 busy-max-us=4000 wakes=1 span-ms=12 over=0"
        );
        assert!(!l.is_open());
        assert_eq!(l.end(10_000 * MS, 0), None);
    }

    #[test]
    fn frames_before_the_first_key_are_not_the_bursts() {
        let mut l = Latency::new();
        l.frame(0, 9 * MS);
        l.answered(0, 9 * MS);
        l.key(10 * MS);
        l.frame(11 * MS, 12 * MS);
        l.answered(10 * MS, 12 * MS);
        let line = l.end(12 * MS + QUIET_NS, 0).unwrap();
        assert!(line.contains("keys=1 answered=1 unanswered=0 key-to-present-us p50=2000 "), "{}", line);
        assert!(line.contains("frames=1 frame-us p50=1000 p99=1000 max=1000 "), "{}", line);
    }

    #[test]
    fn the_next_burst_starts_from_nothing() {
        let mut l = Latency::new();
        l.key(0);
        l.frame(0, 50 * MS);
        l.answered(0, 50 * MS);
        l.woke(51 * MS);
        l.waiting(80 * MS);
        assert!(l.end(50 * MS + QUIET_NS, 0).is_some());
        let t = 10_000 * MS;
        l.key(t);
        l.answered(t, t + 2 * MS);
        let line = l.end(t + 2 * MS + QUIET_NS, 0).unwrap();
        assert_eq!(
            line,
            "halcyond: latency: keys=1 answered=1 unanswered=0 key-to-present-us p50=2000 p99=2000 max=2000 frames=0 frame-us p50=- p99=- max=- busy-max-us=0 wakes=0 span-ms=2 over=0"
        );
    }

    #[test]
    fn an_owed_key_holds_the_burst_until_give_up() {
        let mut l = Latency::new();
        l.key(0);
        assert_eq!(l.end(QUIET_NS + MS, 1), None);
        assert_eq!(l.due_ms(0, true), Some(GIVE_UP_NS / MS));
        let line = l.end(GIVE_UP_NS, 1).expect("give up");
        assert!(line.contains("keys=1 answered=0 unanswered=1 key-to-present-us p50=- p99=- max=-"), "{}", line);
    }

    #[test]
    fn a_long_stall_is_measured_not_given_up() {
        let mut l = Latency::new();
        l.key(0);
        assert_eq!(l.end(2_000 * MS, 1), None);
        l.answered(0, 2_000 * MS);
        let line = l.end(2_000 * MS + QUIET_NS, 0).expect("quiet");
        assert!(line.contains("key-to-present-us p50=2000000 p99=2000000 max=2000000"), "{}", line);
    }

    #[test]
    fn samples_past_the_cap_count_and_max_stays_exact() {
        let mut l = Latency::new();
        l.key(0);
        for i in 0..(SAMPLES_MAX as u64 + 3) {
            l.frame(0, (i + 1) * 1000);
        }
        let line = l.end(QUIET_NS, 0).unwrap();
        assert!(line.contains(&format!("frames={}", SAMPLES_MAX + 3)), "{}", line);
        assert!(line.contains(&format!("max={} ", SAMPLES_MAX + 3)), "{}", line);
        assert!(line.ends_with("over=3"), "{}", line);
    }

    #[test]
    fn busy_counts_only_inside_a_burst_and_pairs_wake_with_wait() {
        let mut l = Latency::new();
        l.woke(0);
        l.waiting(50 * MS); // before the burst: not counted
        l.woke(60 * MS);
        l.key(61 * MS);
        l.waiting(70 * MS); // the key's own iteration: 10 ms
        l.waiting(500 * MS); // no wake in between: nothing to pair
        l.answered(61 * MS, 65 * MS);
        let line = l.end(70 * MS + QUIET_NS, 0).unwrap();
        assert!(line.contains("busy-max-us=10000 wakes=0"), "{}", line);
    }
}
