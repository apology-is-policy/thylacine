// display -- the mode a display takes for its host, and when it follows the
// host's changes (docs/AURORA-CONFIG.md 3.4). Pure and host-tested: lictor
// sizes its trusted screen with `fit_mode`, tapestryd its modes, and
// tapestryd's pass drives `Follow`.
//
// A host's display is whatever its window happens to be. Not every size can
// be a mode (a surface has a cap, a trusted screen a floor), and a window
// being dragged changes size many times before it rests.

/// The display scale, in `scale::scale_pct`'s percent, of a screen of `own`
/// pixels shown over the whole of a display of `host` pixels. A declared
/// scale is the display's, so it is carried over to the screen's pixels;
/// with none declared it is what the display's millimetres make of the
/// screen's own pixels; with neither it is 100. A screen made at half its
/// host's size is at half its host's scale, and the frontend makes up the rest.
pub fn own_pct(host: (u32, u32), own: (u32, u32), declared: Option<u16>, mm: Option<(u32, u32)>) -> u16 {
    match (declared, mm) {
        (Some(pct), _) if host.0 > 0 => {
            (pct as u64 * own.0 as u64 / host.0 as u64).min(u16::MAX as u64) as u16
        }
        (Some(pct), _) => pct,
        (None, Some((w, h))) => crate::scale::scale_pct(own.0, own.1, w, h),
        (None, None) => crate::scale::SCALE_MIN,
    }
}

/// The largest whole number `fit_mode` divides or multiplies a host's size
/// by. 8192 a side, the most a display is taken to report, is 2048 at 4.
pub const FIT_MAX_FACTOR: u32 = 4;

/// The mode for a display whose host prefers `host_w x host_h`, when not
/// every size can be made: the host's own size if `can` make it, else that
/// size over 2, times 2, over 3, times 3 and so on -- the first `can`
/// accepts. A whole factor keeps the host's picture of the mode sharp (a
/// guest pixel is k by k of the host's, or k by k of them are one), and the
/// scale follows by itself: the same millimetres under half the pixels are
/// half the DPI. None when no factor up to FIT_MAX_FACTOR gives a size `can`
/// accepts: the caller keeps the mode it has.
pub fn fit_mode(
    host_w: u32,
    host_h: u32,
    mut can: impl FnMut(u32, u32) -> bool,
) -> Option<(u32, u32)> {
    if host_w == 0 || host_h == 0 {
        return None;
    }
    if can(host_w, host_h) {
        return Some((host_w, host_h));
    }
    for k in 2..=FIT_MAX_FACTOR {
        let (w, h) = (host_w / k, host_h / k);
        if w > 0 && h > 0 && can(w, h) {
            return Some((w, h));
        }
        if let (Some(w), Some(h)) = (host_w.checked_mul(k), host_h.checked_mul(k)) {
            if can(w, h) {
                return Some((w, h));
            }
        }
    }
    None
}

/// How long the host's display must have stood since its last event before
/// it is followed. Each size followed is a new screen, a reweave of every
/// surface and a repaint by every client.
pub const QUIET_MS: u64 = 150;

/// How many times an asking that was made and failed is made again. The
/// waits double from twice QUIET_MS, so six are 18.9 s in all. A device that
/// is stalled before it is asked is waited for with `hold`, uncounted; a
/// stall that begins inside an asking is a failure of that asking.
pub const RETRY_MAX: u8 = 6;

/// The most a `Backoff`'s wait doubles: QUIET_MS << 6 is 9.6 s.
pub const BACKOFF_MAX_SHIFT: u8 = 6;

/// What a pass does about the host's display.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Step {
    /// Nothing is owed: no event, or one that has not stood yet.
    Wait,
    /// The display changed and has stood: ask the host, take the mode for
    /// its answer.
    Ask,
    /// The display changed and has stood, and the mode is pinned: it stands.
    Pinned,
}

/// Whether and when a display follows its host. The device's driver counts
/// the display events it takes (wrapping); every pass hands that count and
/// the time to `step`.
#[derive(Clone, Copy, Debug)]
pub struct Follow {
    seen: u32,
    /// When the event that is waiting is to be reported.
    due_at: Option<u64>,
    /// Askings made again for that event so far.
    retries: u8,
    /// That event has been held since it arrived (`hold`).
    held: bool,
    pinned: bool,
}

impl Follow {
    /// Following, with no event seen. The count starts at zero in the driver
    /// too, so one that is not zero is an event since the driver's own first
    /// question, which is the size a display starts from.
    pub const fn new() -> Self {
        Self { seen: 0, due_at: None, retries: 0, held: false, pinned: false }
    }

    /// A mode was asked for by size: it stands whatever the host's window
    /// does.
    pub fn pin(&mut self) {
        self.pinned = true;
    }

    /// The mode follows the host again. The caller has just asked the host;
    /// an event still waiting out its quiet time is asked for again when it
    /// ends, which costs a question and changes nothing.
    pub fn unpin(&mut self) {
        self.pinned = false;
    }

    pub fn pinned(&self) -> bool {
        self.pinned
    }

    /// The count as of the last `step`.
    pub fn seen(&self) -> u32 {
        self.seen
    }

    /// One pass. A count that moved restarts the quiet time, so a train of
    /// events is answered once, after its last. An event that has stood is
    /// reported once, and then forgotten unless `retry` says otherwise.
    pub fn step(&mut self, count: u32, now_ms: u64) -> Step {
        if count != self.seen {
            self.seen = count;
            self.again(now_ms);
        }
        match self.due_at {
            Some(due) if now_ms >= due => {
                self.due_at = None;
                if self.pinned {
                    Step::Pinned
                } else {
                    Step::Ask
                }
            }
            _ => Step::Wait,
        }
    }

    /// An asking is owed after the quiet time, with all its askings, as for
    /// a new event: `step` does this for a count that moved, and a caller
    /// that asked the host itself does it when what it began is not
    /// finished.
    pub fn again(&mut self, now_ms: u64) {
        self.due_at = Some(now_ms.saturating_add(QUIET_MS));
        self.retries = 0;
        self.held = false;
    }

    /// The asking `step` called for cannot be made: the device is stalled
    /// and says so, and nothing was asked of it. The event stays owed and
    /// is called for again after the quiet time, without a count: the
    /// device's driver ends a stall itself, by answering again or by
    /// declaring the device dead, and the caller stops at the second. So
    /// one stall holds an event for no longer than the driver lets a stall
    /// last; an event is held again for each stall that is standing when
    /// its asking comes. True the first time for an event, so that the
    /// caller says it once.
    pub fn hold(&mut self, now_ms: u64) -> bool {
        self.due_at = Some(now_ms.saturating_add(QUIET_MS));
        !core::mem::replace(&mut self.held, true)
    }

    /// The asking `step` called for was made and failed (a change the
    /// device did not take, an allocation that failed, a stall that began
    /// inside it): it is called for again, after a wait that doubles each
    /// time. False once RETRY_MAX askings have been made again: the event
    /// is then forgotten, and the next one starts afresh. Holds between
    /// them neither spend these askings nor give them back.
    pub fn retry(&mut self, now_ms: u64) -> bool {
        if self.retries >= RETRY_MAX {
            return false;
        }
        self.retries += 1;
        self.due_at = Some(now_ms.saturating_add(QUIET_MS << self.retries));
        true
    }
}

impl Default for Follow {
    fn default() -> Self {
        Self::new()
    }
}

/// When to do again something that failed and stays owed for as long as it
/// fails (a display's bind to its screen): at once the first time, then
/// after waits that double from twice QUIET_MS to QUIET_MS <<
/// BACKOFF_MAX_SHIFT and stay there. It never gives up; its caller does,
/// when the device is dead.
#[derive(Clone, Copy, Debug)]
pub struct Backoff {
    next_at: Option<u64>,
    fails: u8,
}

impl Backoff {
    pub const fn new() -> Self {
        Self { next_at: None, fails: 0 }
    }

    /// May it be tried now?
    pub fn ready(&self, now_ms: u64) -> bool {
        self.next_at.map_or(true, |at| now_ms >= at)
    }

    /// It was tried and failed.
    pub fn failed(&mut self, now_ms: u64) {
        if self.fails < BACKOFF_MAX_SHIFT {
            self.fails += 1;
        }
        self.next_at = Some(now_ms.saturating_add(QUIET_MS << self.fails));
    }

    /// It was done: the next failure starts from the shortest wait.
    pub fn reset(&mut self) {
        *self = Self::new();
    }
}

impl Default for Backoff {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::scale::scale_pct;

    #[test]
    fn a_mode_is_the_hosts_size_over_or_times_a_whole_number() {
        // A compositor's bound: three buffers of the mode within 64 MiB, and
        // no smaller than 320x200.
        let surface = |w: u32, h: u32| {
            w >= 320 && h >= 200 && (w as u64) * (h as u64) * 4 * 3 <= 64 << 20
        };
        // A size that can be made is the mode.
        assert_eq!(fit_mode(2560, 1600, surface), Some((2560, 1600)));
        assert_eq!(fit_mode(2940, 1848, surface), Some((2940, 1848)));
        // Full screen on larger panels cannot: half of it can, and the same
        // millimetres under half the pixels are half the scale.
        assert_eq!(fit_mode(3456, 2160, surface), Some((1728, 1080)));
        assert_eq!(fit_mode(3024, 1890, surface), Some((1512, 945)));
        assert_eq!(fit_mode(5120, 2880, surface), Some((2560, 1440)));
        assert_eq!(scale_pct(3456, 2160, 457, 286), 200);
        assert_eq!(scale_pct(1728, 1080, 457, 286), 100);
        // The most a display reports needs a quarter; an odd size loses the
        // remainder.
        assert_eq!(fit_mode(8192, 8192, surface), Some((2048, 2048)));
        assert_eq!(fit_mode(6016, 3384, surface), Some((3008, 1692)));
        assert_eq!(fit_mode(2941, 1913, surface), Some((1470, 956)));
        // A trusted screen has a floor (lictor's render::fits, written out):
        // a small host's size is doubled, and the scale doubles with it.
        let screen = |w: u32, h: u32| w >= 800 && h >= 720 && w * h <= 4096 * 2160;
        assert_eq!(fit_mode(640, 400, screen), Some((1280, 800)));
        assert_eq!(scale_pct(640, 400, 169, 106), 100);
        assert_eq!(scale_pct(1280, 800, 169, 106), 200);
        assert_eq!(fit_mode(320, 200, screen), Some((1280, 800)));
        assert_eq!(fit_mode(1024, 768, screen), Some((1024, 768)));
        assert_eq!(fit_mode(3456, 2160, screen), Some((3456, 2160)));
        assert_eq!(fit_mode(5120, 2880, screen), Some((2560, 1440)));
        // The order: the size itself, then over 2, times 2, over 3, times 3.
        let mut asked = [(0u32, 0u32); 8];
        let mut n = 0;
        assert_eq!(
            fit_mode(600, 300, |w, h| {
                asked[n] = (w, h);
                n += 1;
                false
            }),
            None
        );
        assert_eq!(
            asked[..n],
            [(600, 300), (300, 150), (1200, 600), (200, 100), (1800, 900), (150, 75), (2400, 1200)]
        );
        // Nothing that can be made, no size at all, a division to nothing,
        // and a product past u32: None, and never a size of nothing asked.
        assert_eq!(fit_mode(100, 90, screen), None);
        assert_eq!(fit_mode(0, 800, |_, _| true), None);
        assert_eq!(fit_mode(800, 0, |_, _| true), None);
        assert_eq!(fit_mode(3, 1, |w, h| w * h == 0), None);
        assert_eq!(fit_mode(u32::MAX, u32::MAX, |w, _| w < 1 << 30), Some((u32::MAX / 4, u32::MAX / 4)));
    }

    #[test]
    fn a_screens_scale_is_its_displays_carried_to_its_own_pixels() {
        // The display's own size: what is declared, else what its millimetres say, else 100.
        assert_eq!(own_pct((2560, 1600), (2560, 1600), Some(200), None), 200);
        assert_eq!(own_pct((2560, 1600), (2560, 1600), Some(125), Some((339, 212))), 125);
        assert_eq!(own_pct((2560, 1600), (2560, 1600), None, Some((339, 212))), 200);
        assert_eq!(own_pct((1280, 800), (1280, 800), None, Some((339, 212))), 100);
        assert_eq!(own_pct((2560, 1600), (2560, 1600), None, None), 100);
        // A screen at half its host's size is at half its scale, either way it is known.
        assert_eq!(own_pct((5120, 2880), (2560, 1440), Some(200), None), 100);
        assert_eq!(own_pct((5120, 2880), (2560, 1440), None, Some((677, 381))), 100);
        assert_eq!(scale_pct(5120, 2880, 677, 381), 200);
        // And one at twice its host's size at twice.
        assert_eq!(own_pct((640, 400), (1280, 800), Some(100), None), 200);
        assert_eq!(own_pct((640, 400), (1280, 800), None, Some((169, 106))), 200);
        // A display with no width declares for itself; nothing overflows.
        assert_eq!(own_pct((0, 0), (1280, 800), Some(150), None), 150);
        assert_eq!(own_pct((1, 1), (u32::MAX, u32::MAX), Some(200), None), u16::MAX);
    }

    #[test]
    fn an_event_is_followed_once_it_has_stood() {
        let mut f = Follow::new();
        // No event: nothing, however long.
        assert_eq!(f.step(0, 0), Step::Wait);
        assert_eq!(f.step(0, 10_000), Step::Wait);
        // An event: nothing until it has stood QUIET_MS, then one asking.
        assert_eq!(f.step(1, 10_000), Step::Wait);
        assert_eq!(f.step(1, 10_000 + QUIET_MS - 1), Step::Wait);
        assert_eq!(f.step(1, 10_000 + QUIET_MS), Step::Ask);
        assert_eq!(f.seen(), 1);
        // Asked once: the same count is not news again.
        assert_eq!(f.step(1, 10_000 + QUIET_MS + 1), Step::Wait);
        assert_eq!(f.step(1, 99_000), Step::Wait);
        // A pass that comes late still asks.
        assert_eq!(f.step(2, 100_000), Step::Wait);
        assert_eq!(f.step(2, 160_000), Step::Ask);
    }

    #[test]
    fn a_train_of_events_is_one_asking_after_its_last() {
        let mut f = Follow::new();
        let mut asked = 0;
        // An event every 100 ms for a second: each restarts the quiet time.
        for i in 0..10u64 {
            for t in (i * 100)..(i * 100 + 100) {
                if f.step(i as u32 + 1, t) == Step::Ask {
                    asked += 1;
                }
            }
        }
        assert_eq!(asked, 0);
        // The last was at 900 ms: the asking comes at 900 + QUIET_MS.
        assert_eq!(f.step(10, 900 + QUIET_MS - 1), Step::Wait);
        assert_eq!(f.step(10, 900 + QUIET_MS), Step::Ask);
        assert_eq!(f.step(10, 900 + QUIET_MS + 1), Step::Wait);
        // Several events between two passes are one change of the count.
        assert_eq!(f.step(14, 5_000), Step::Wait);
        assert_eq!(f.step(14, 5_000 + QUIET_MS), Step::Ask);
    }

    #[test]
    fn the_count_wraps_and_a_first_count_that_is_not_zero_is_news() {
        // The driver counted an event before the compositor's first pass.
        let mut f = Follow::new();
        assert_eq!(f.step(3, 50), Step::Wait);
        assert_eq!(f.step(3, 50 + QUIET_MS), Step::Ask);
        // Across the wrap a change is still a change.
        let mut f = Follow::new();
        assert_eq!(f.step(u32::MAX, 0), Step::Wait);
        assert_eq!(f.step(u32::MAX, QUIET_MS), Step::Ask);
        assert_eq!(f.step(0, 1_000), Step::Wait);
        assert_eq!(f.step(0, 1_000 + QUIET_MS), Step::Ask);
        // A clock that reads earlier than the event (it should not): no
        // asking, and no panic.
        let mut f = Follow::new();
        assert_eq!(f.step(1, 1_000), Step::Wait);
        assert_eq!(f.step(1, 10), Step::Wait);
        assert_eq!(f.step(1, 1_000 + QUIET_MS), Step::Ask);
    }

    #[test]
    fn an_asking_that_failed_is_made_again_a_bounded_number_of_times() {
        let mut f = Follow::new();
        assert_eq!(f.step(1, 0), Step::Wait);
        assert_eq!(f.step(1, QUIET_MS), Step::Ask);
        // Not asked again by itself.
        assert_eq!(f.step(1, 60_000), Step::Wait);
        // Asked again when told the asking failed: after twice the quiet
        // time, then four times, each from when it was told.
        let mut now = 60_000;
        let mut total = 0;
        for n in 1..=RETRY_MAX {
            assert!(f.retry(now));
            let wait = QUIET_MS << n;
            assert_eq!(f.step(1, now + wait - 1), Step::Wait, "retry {n}");
            assert_eq!(f.step(1, now + wait), Step::Ask, "retry {n}");
            assert_eq!(f.step(1, now + wait + 1), Step::Wait, "retry {n}");
            now += wait;
            total += wait;
        }
        assert_eq!(total, 18_900);
        // And then no more: the event is forgotten.
        assert!(!f.retry(now));
        assert_eq!(f.step(1, now + 3_600_000), Step::Wait);
        // A new event starts afresh, with all its askings.
        assert_eq!(f.step(2, now), Step::Wait);
        assert_eq!(f.step(2, now + QUIET_MS), Step::Ask);
        assert!(f.retry(now + QUIET_MS));
        // An event during the wait for another asking takes its place, and
        // is asked after the ordinary quiet time.
        assert_eq!(f.step(3, now + QUIET_MS + 10), Step::Wait);
        assert_eq!(f.step(3, now + QUIET_MS + 10 + QUIET_MS), Step::Ask);
        for _ in 0..RETRY_MAX {
            assert!(f.retry(now));
        }
        assert!(!f.retry(now));
        // A mode pinned while an asking waits to be made again stands.
        let mut f = Follow::new();
        assert_eq!(f.step(1, 0), Step::Wait);
        assert_eq!(f.step(1, QUIET_MS), Step::Ask);
        assert!(f.retry(QUIET_MS));
        f.pin();
        assert_eq!(f.step(1, QUIET_MS + (QUIET_MS << 1)), Step::Pinned);
        // A clock at its end does not wrap the wait into the past.
        let mut f = Follow::new();
        assert_eq!(f.step(1, u64::MAX - 1), Step::Wait);
        assert_eq!(f.step(1, u64::MAX), Step::Ask);
        assert!(f.retry(u64::MAX));
        assert_eq!(f.step(1, 1_000), Step::Wait);
    }

    #[test]
    fn an_event_is_held_for_as_long_as_the_device_is_stalled() {
        let mut f = Follow::new();
        assert_eq!(f.step(1, 0), Step::Wait);
        assert_eq!(f.step(1, QUIET_MS), Step::Ask);
        // Held: called for again every quiet time, far past any count of
        // askings made again (500 are 75 s), and said once.
        let mut now = QUIET_MS;
        assert!(f.hold(now));
        for _ in 0..500 {
            assert_eq!(f.step(1, now + QUIET_MS - 1), Step::Wait);
            assert_eq!(f.step(1, now + QUIET_MS), Step::Ask);
            now += QUIET_MS;
            assert!(!f.hold(now));
        }
        // Holding costs none of the askings a failure has.
        assert_eq!(f.step(1, now + QUIET_MS), Step::Ask);
        for _ in 0..RETRY_MAX {
            assert!(f.retry(now));
        }
        assert!(!f.retry(now));
        // A new event that is held is said again.
        assert_eq!(f.step(2, now), Step::Wait);
        assert_eq!(f.step(2, now + QUIET_MS), Step::Ask);
        assert!(f.hold(now + QUIET_MS));
        // A mode pinned while an event is held stands.
        f.pin();
        assert_eq!(f.step(2, now + 2 * QUIET_MS), Step::Pinned);
    }

    #[test]
    fn holds_between_failed_askings_spend_none_and_give_none_back() {
        let mut f = Follow::new();
        assert_eq!(f.step(1, 0), Step::Wait);
        assert_eq!(f.step(1, QUIET_MS), Step::Ask);
        let mut now = QUIET_MS;
        // Stalled before the first asking: held, and said.
        assert!(f.hold(now));
        for n in 1..=RETRY_MAX {
            now += QUIET_MS;
            assert_eq!(f.step(1, now), Step::Ask, "after the hold before failure {n}");
            // An asking that was made and failed: the wait is the one for
            // its number, whatever was held before it.
            assert!(f.retry(now), "failure {n}");
            let wait = QUIET_MS << n;
            assert_eq!(f.step(1, now + wait - 1), Step::Wait, "failure {n}");
            now += wait;
            assert_eq!(f.step(1, now), Step::Ask, "failure {n}");
            // Stalled when that asking comes: held for the quiet time, not
            // for the doubled wait, and not said a second time for the event.
            assert!(!f.hold(now), "the hold after failure {n} is said again");
            assert_eq!(f.step(1, now + QUIET_MS - 1), Step::Wait, "the hold after failure {n}");
        }
        now += QUIET_MS;
        assert_eq!(f.step(1, now), Step::Ask);
        // The askings are spent however many holds came between them.
        assert!(!f.retry(now));
        assert_eq!(f.step(1, now + 3_600_000), Step::Wait);
    }

    #[test]
    fn a_caller_that_asked_itself_has_the_asking_made_again() {
        let mut f = Follow::new();
        // With no event at all: an asking after the quiet time.
        f.again(1_000);
        assert_eq!(f.step(0, 1_000 + QUIET_MS - 1), Step::Wait);
        assert_eq!(f.step(0, 1_000 + QUIET_MS), Step::Ask);
        assert_eq!(f.step(0, 1_000 + QUIET_MS + 1), Step::Wait);
        // For an event whose askings were spent and whose hold was said:
        // all of them again, and a hold is said again.
        assert_eq!(f.step(1, 2_000), Step::Wait);
        assert_eq!(f.step(1, 2_000 + QUIET_MS), Step::Ask);
        assert!(f.hold(3_000));
        for _ in 0..RETRY_MAX {
            assert!(f.retry(3_000));
        }
        assert!(!f.retry(3_000));
        f.again(10_000);
        assert_eq!(f.step(1, 10_000 + QUIET_MS - 1), Step::Wait);
        assert_eq!(f.step(1, 10_000 + QUIET_MS), Step::Ask);
        assert!(f.hold(11_000));
        for _ in 0..RETRY_MAX {
            assert!(f.retry(11_000));
        }
        assert!(!f.retry(11_000));
        // Under a pinned mode it stands, as an event does.
        f.again(20_000);
        f.pin();
        assert_eq!(f.step(1, 20_000 + QUIET_MS), Step::Pinned);
    }

    #[test]
    fn what_stays_owed_is_tried_at_waits_that_double_to_a_cap() {
        let mut b = Backoff::new();
        // Nothing failed yet: at once, at any time.
        assert!(b.ready(0));
        assert!(b.ready(u64::MAX));
        // Each failure doubles the wait, from twice the quiet time to the
        // cap, and it stays there for as long as it fails.
        let mut now = 5_000u64;
        let mut total = 0;
        for n in 1..=40u32 {
            b.failed(now);
            let wait = QUIET_MS << n.min(BACKOFF_MAX_SHIFT as u32);
            assert!(!b.ready(now), "failure {n}");
            assert!(!b.ready(now + wait - 1), "failure {n}");
            assert!(b.ready(now + wait), "failure {n}");
            now += wait;
            total += wait;
        }
        // 300 + 600 + 1200 + 2400 + 4800, then 9.6 s each.
        assert_eq!(total, 9_300 + 35 * 9_600);
        // Done: at once again, and the next failure waits the shortest.
        b.reset();
        assert!(b.ready(0));
        b.failed(100);
        assert!(!b.ready(100 + 299));
        assert!(b.ready(100 + 300));
        // A clock that reads earlier than the failure: not yet, no panic.
        assert!(!b.ready(0));
        // A clock at its end does not wrap the wait into the past.
        let mut b = Backoff::new();
        b.failed(u64::MAX);
        assert!(!b.ready(1_000));
        assert!(b.ready(u64::MAX));
    }

    #[test]
    fn a_pinned_mode_stands_and_following_again_asks_for_the_next_event() {
        let mut f = Follow::new();
        assert!(!f.pinned());
        f.pin();
        assert!(f.pinned());
        // An event under a pinned mode is reported as standing, once.
        assert_eq!(f.step(1, 0), Step::Wait);
        assert_eq!(f.step(1, QUIET_MS), Step::Pinned);
        assert_eq!(f.step(1, QUIET_MS + 1), Step::Wait);
        // Pinned while an event waits: it stands too.
        assert_eq!(f.step(2, 1_000), Step::Wait);
        assert_eq!(f.step(2, 1_000 + QUIET_MS), Step::Pinned);
        // Following again: the event let stand is not asked for by itself
        // (the caller asked before it unpinned), the next one is.
        f.unpin();
        assert_eq!(f.step(2, 5_000), Step::Wait);
        assert_eq!(f.step(3, 6_000), Step::Wait);
        assert_eq!(f.step(3, 6_000 + QUIET_MS), Step::Ask);
        // Unpinned while an event waits out its quiet time: asked when it ends.
        f.pin();
        assert_eq!(f.step(4, 7_000), Step::Wait);
        f.unpin();
        assert_eq!(f.step(4, 7_000 + QUIET_MS), Step::Ask);
        // And pinned while one waits: it stands.
        assert_eq!(f.step(5, 8_000), Step::Wait);
        f.pin();
        assert_eq!(f.step(5, 8_000 + QUIET_MS), Step::Pinned);
    }
}
