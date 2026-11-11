// display -- the mode a display takes for its host, and when it follows the
// host's changes (docs/AURORA-CONFIG.md 3.4). Pure and host-tested: lictor
// sizes its trusted screen with `fit_mode`, tapestryd its modes, and
// tapestryd's pass drives `Follow`.
//
// A host's display is whatever its window happens to be. Not every size can
// be a mode (a surface has a cap, a trusted screen a floor), and a window
// being dragged changes size many times before it rests.

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
    moved_at: Option<u64>,
    pinned: bool,
}

impl Follow {
    /// Following, with no event seen. The count starts at zero in the driver
    /// too, so one that is not zero is an event since the driver's own first
    /// question, which is the size a display starts from.
    pub const fn new() -> Self {
        Self { seen: 0, moved_at: None, pinned: false }
    }

    /// A mode was asked for by size: it stands whatever the host's window
    /// does.
    pub fn pin(&mut self) {
        self.pinned = true;
    }

    /// The mode follows the host again. The caller asks the host now; an
    /// event still waiting out its quiet time is asked for again when it
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
    /// reported once, and then forgotten whether or not the mode followed.
    pub fn step(&mut self, count: u32, now_ms: u64) -> Step {
        if count != self.seen {
            self.seen = count;
            self.moved_at = Some(now_ms);
        }
        match self.moved_at {
            Some(at) if now_ms.saturating_sub(at) >= QUIET_MS => {
                self.moved_at = None;
                if self.pinned {
                    Step::Pinned
                } else {
                    Step::Ask
                }
            }
            _ => Step::Wait,
        }
    }
}

impl Default for Follow {
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
        // (the caller asks as it unpins), the next one is.
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
