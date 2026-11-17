// The session's latency record (ASYNC-SERVICE-LIFECYCLE AS-4f; HALCYON-INTERACTION
// 12: "record render latency and idle wakeups before/after instead of asserting
// 'smooth' from a still image"). Test builds only: the session loop feeds it under
// the `test-mode` feature, which production strips (vote 136).
//
// A BURST opens at the first key a tile's input queue accepts. A key, a key's answer
// or a present after program output keeps it open; it ends once that has not happened
// for QUIET_NS with no tile owed an answer, GIVE_UP_NS whatever is owed, or SPAN_MAX_NS
// after it opened whatever else (`cut=1`). Its one line says, in microseconds:
//   - first-output: from a tile's first unanswered key entering its queue to that
//     tile's first present after the program's NEXT output, whatever that output
//     answers. Later keys ride with it. Under a backlog the next output answers an
//     older key, so this is a lower bound on input latency, not the latency itself;
//   - settle: from the burst's last key to the present after the burst's last program
//     output -- when the screen caught up with everything typed, backlog included;
//   - frame: one tile's render + present, every one presented while the burst is open;
//   - busy-max: the longest stretch from the loop's wake to its next wait; an event can
//     sit unread across the tail of one stretch and the head of the next, so for at
//     most twice that;
//   - wakes: how often the loop's wait returned while the burst was open.
// `owed-tiles` counts tiles whose first unanswered key never got an answer; a riding
// key is not checked one by one. Nothing opens a burst but a key, so an idle session
// says nothing and the record adds no wake to it.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

/// A burst with nothing owed ends this long after its last event.
pub const QUIET_NS: u64 = 600_000_000;
/// A burst ends this long after its last event even with keys still owed.
pub const GIVE_UP_NS: u64 = 5_000_000_000;
/// A burst ends this long after it opened, whatever is still happening.
pub const SPAN_MAX_NS: u64 = 30_000_000_000;
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
    last_key_ns: u64,
    last_output_ns: u64,
    keys: u32,
    dropped: u32,
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
        self.last_key_ns = self.last_key_ns.max(now);
        self.last_ns = self.last_ns.max(now);
    }

    /// A tile's input queue refused a key (its program stopped draining); counted
    /// only inside a burst.
    pub fn dropped(&mut self) {
        if self.open {
            self.dropped = self.dropped.saturating_add(1);
        }
    }

    /// A tile presented at `now` after its program wrote.
    pub fn output_presented(&mut self, now: u64) {
        if !self.open {
            return;
        }
        self.last_output_ns = self.last_output_ns.max(now);
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
        let at = (self.last_ns + if owed { GIVE_UP_NS } else { QUIET_NS })
            .min(self.first_ns + SPAN_MAX_NS);
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
        let cut = now.saturating_sub(self.first_ns) >= SPAN_MAX_NS;
        if !cut && idle < GIVE_UP_NS && (owed > 0 || idle < QUIET_NS) {
            return None;
        }
        let keys = spread(&mut self.key_us, self.key_max_ns);
        let frames = spread(&mut self.frame_us, self.frame_max_ns);
        let settle = if self.last_output_ns > self.last_key_ns {
            format!("{}", us(self.last_output_ns - self.last_key_ns))
        } else {
            String::from("-")
        };
        let line = format!(
            "halcyond: latency: keys={} dropped={} answered={} owed-tiles={} first-output-us {} settle-us={} frames={} frame-us {} busy-max-us={} wakes={} span-ms={} cut={} over={}",
            self.keys,
            self.dropped,
            self.answered,
            owed,
            keys,
            settle,
            self.frames,
            frames,
            us(self.busy_max_ns),
            self.wakes,
            self.last_ns.saturating_sub(self.first_ns) / 1_000_000,
            cut as u8,
            self.over
        );
        *self = Self::default();
        Some(line)
    }
}

/// Polls an idle stretch takes before it says why it woke: an idle session that
/// wakes a few times a second says nothing for minutes; one that wakes a hundred
/// times a second says a line every ten seconds.
pub const IDLE_POLLS_SAID: u32 = 1000;

/// Distinct surface event kinds an idle tally names; later kinds count in `events`.
pub const IDLE_KINDS: usize = 4;
/// A wait armed for less than this is `short`: a deadline, not idleness.
pub const IDLE_SHORT_MS: i32 = 50;

/// Why the loop woke while no burst was open (test builds): its iterations, how
/// long each wait was armed for, its poll's returns by cause, the surface events
/// it drained by kind, and its presents.
#[derive(Default)]
pub struct IdleTally {
    since_ns: u64,
    iterations: u32,
    polls: u32,
    forever: u32,
    short: u32,
    timeouts: u32,
    ring: u32,
    up: u32,
    other: u32,
    events: u32,
    kinds: [(u16, u32); IDLE_KINDS],
    presents: u32,
}

impl IdleTally {
    pub fn new() -> Self {
        Self::default()
    }

    /// A burst opened: the tally counts idle stretches only.
    pub fn reset(&mut self) {
        *self = Self::default();
    }

    pub fn iteration(&mut self) {
        self.iterations = self.iterations.saturating_add(1);
    }

    pub fn presented(&mut self) {
        self.presents = self.presents.saturating_add(1);
    }

    /// A tile's surface event of `kind` was drained.
    pub fn event(&mut self, kind: u16) {
        self.events = self.events.saturating_add(1);
        for slot in self.kinds.iter_mut() {
            if slot.1 == 0 {
                *slot = (kind, 1);
                return;
            }
            if slot.0 == kind {
                slot.1 = slot.1.saturating_add(1);
                return;
            }
        }
    }

    /// The poll, armed for `timeout` ms (negative: no bound), returned `rc` at
    /// `now`; which of its entries were ready. The line, once IDLE_POLLS_SAID polls
    /// have been counted, and a fresh tally.
    pub fn polled(&mut self, now: u64, timeout: i32, rc: i64, ring: bool, up: bool, other: bool) -> Option<String> {
        if self.polls == 0 {
            self.since_ns = now;
        }
        self.polls += 1;
        if timeout < 0 {
            self.forever += 1;
        } else if timeout < IDLE_SHORT_MS {
            self.short += 1;
        }
        if rc == 0 {
            self.timeouts += 1;
        }
        self.ring += ring as u32;
        self.up += up as u32;
        self.other += other as u32;
        if self.polls < IDLE_POLLS_SAID {
            return None;
        }
        let mut kinds = String::new();
        for (kind, n) in self.kinds.iter().filter(|k| k.1 > 0) {
            if !kinds.is_empty() {
                kinds.push(',');
            }
            kinds.push_str(&format!("{}:{}", kind, n));
        }
        if kinds.is_empty() {
            kinds.push('-');
        }
        let line = format!(
            "halcyond: idle wakes: polls={} ms={} iterations={} forever={} short={} timeout={} ring={} up={} other={} events={} kinds={} presents={}",
            self.polls,
            now.saturating_sub(self.since_ns) / 1_000_000,
            self.iterations,
            self.forever,
            self.short,
            self.timeouts,
            self.ring,
            self.up,
            self.other,
            self.events,
            kinds,
            self.presents
        );
        self.reset();
        Some(line)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_idle_tally_says_once_per_thousand_polls() {
        let mut t = IdleTally::new();
        for i in 0..(IDLE_POLLS_SAID - 1) {
            t.iteration();
            let timeout = match i % 3 {
                0 => -1,
                1 => IDLE_SHORT_MS - 1,
                _ => IDLE_SHORT_MS,
            };
            assert_eq!(t.polled(1_000 * MS + i as u64 * MS, timeout, if i % 2 == 0 { 0 } else { 1 }, i % 2 == 1, false, i % 4 == 1), None);
        }
        t.presented();
        let line = t.polled(1_000 * MS + 999 * MS, 0, 1, false, true, false).unwrap();
        assert_eq!(
            line,
            "halcyond: idle wakes: polls=1000 ms=999 iterations=999 forever=333 short=334 timeout=500 ring=499 up=1 other=250 events=0 kinds=- presents=1"
        );
        assert_eq!(t.polled(5_000 * MS, -1, 0, false, false, false), None);
    }

    #[test]
    fn the_idle_tally_names_the_first_kinds_and_counts_the_rest() {
        let mut t = IdleTally::new();
        for kind in [7u16, 3, 7, 9, 11, 7, 13, 13, 3] {
            t.event(kind);
        }
        for _ in 0..(IDLE_POLLS_SAID - 1) {
            assert_eq!(t.polled(0, -1, 1, true, false, false), None);
        }
        let line = t.polled(MS, -1, 1, true, false, false).unwrap();
        assert!(line.contains(" events=9 kinds=7:3,3:2,9:1,11:1 presents=0"), "{}", line);
        t.event(5);
        for _ in 0..IDLE_POLLS_SAID {
            let _ = t.polled(0, -1, 1, true, false, false);
        }
        assert_eq!(t.events, 0, "the line resets the kinds too");
        assert_eq!(t.kinds, [(0, 0); IDLE_KINDS]);
    }

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
            "halcyond: latency: keys=1 dropped=0 answered=1 owed-tiles=0 first-output-us p50=12000 p99=12000 max=12000 settle-us=- frames=1 frame-us p50=2000 p99=2000 max=2000 busy-max-us=4000 wakes=1 span-ms=12 cut=0 over=0"
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
        assert!(line.contains("keys=1 dropped=0 answered=1 owed-tiles=0 first-output-us p50=2000 "), "{}", line);
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
            "halcyond: latency: keys=1 dropped=0 answered=1 owed-tiles=0 first-output-us p50=2000 p99=2000 max=2000 settle-us=- frames=0 frame-us p50=- p99=- max=- busy-max-us=0 wakes=0 span-ms=2 cut=0 over=0"
        );
    }

    #[test]
    fn an_owed_key_holds_the_burst_until_give_up() {
        let mut l = Latency::new();
        l.key(0);
        assert_eq!(l.end(QUIET_NS + MS, 1), None);
        assert_eq!(l.due_ms(0, true), Some(GIVE_UP_NS / MS));
        let line = l.end(GIVE_UP_NS, 1).expect("give up");
        assert!(line.contains("keys=1 dropped=0 answered=0 owed-tiles=1 first-output-us p50=- p99=- max=- settle-us=-"), "{}", line);
    }

    #[test]
    fn a_long_stall_is_measured_not_given_up() {
        let mut l = Latency::new();
        l.key(0);
        assert_eq!(l.end(2_000 * MS, 1), None);
        l.answered(0, 2_000 * MS);
        let line = l.end(2_000 * MS + QUIET_NS, 0).expect("quiet");
        assert!(line.contains("first-output-us p50=2000000 p99=2000000 max=2000000"), "{}", line);
    }

    #[test]
    fn settle_runs_from_the_last_key_to_the_last_output_and_holds_the_burst() {
        let mut l = Latency::new();
        l.key(0);
        l.answered(0, 5 * MS); // an early answer: output for this key or any other
        l.key(30 * MS);
        l.key(60 * MS); // the last key; a backlog keeps the program writing
        l.output_presented(400 * MS);
        assert_eq!(l.end(60 * MS + QUIET_NS, 0), None, "output keeps the burst open");
        l.output_presented(900 * MS);
        l.dropped();
        let line = l.end(900 * MS + QUIET_NS, 0).expect("quiet after the last output");
        assert!(line.contains("keys=3 dropped=1 answered=1 owed-tiles=0 first-output-us p50=5000 "), "{}", line);
        assert!(line.contains("settle-us=840000 "), "{}", line);
        assert!(line.contains("span-ms=900 cut=0 "), "{}", line);
    }

    #[test]
    fn output_before_the_first_key_and_drops_outside_a_burst_count_nothing() {
        let mut l = Latency::new();
        l.output_presented(MS);
        l.dropped();
        assert!(!l.is_open());
        l.key(10 * MS);
        let line = l.end(GIVE_UP_NS + 10 * MS, 1).unwrap();
        assert!(line.contains("keys=1 dropped=0 "), "{}", line);
        assert!(line.contains("settle-us=- "), "{}", line);
    }

    #[test]
    fn a_burst_that_never_quiets_is_cut_at_its_span() {
        let mut l = Latency::new();
        l.key(0);
        let mut t = 0;
        while t < SPAN_MAX_NS - 100 * MS {
            t += 100 * MS;
            l.output_presented(t);
            assert_eq!(l.end(t, 0), None);
        }
        assert_eq!(l.due_ms(t, false), Some(100));
        let line = l.end(SPAN_MAX_NS, 0).expect("cut");
        assert!(line.contains(" cut=1 "), "{}", line);
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
