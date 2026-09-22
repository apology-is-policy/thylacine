// motion -- section 10's motion and section 9.5's opt-out (HALCYON-INSTRUMENT).
// Pure and host-tested: the durations, the easing, the phase a painter asks
// for, and the one rule that decides whether any of it runs at all.
//
// Shared rather than halcyond's, because the movement of section 10 has TWO
// consumers: halcyond animates the tile, the hover, the body and the caret,
// while the split flash is the compositor's. `instrument::effects` already
// holds `SPLIT_FLASH_MS` for that reason, and it stays there -- effects is
// what section 10 PAINTS, this module is how section 10 MOVES.

/// Section 10's motion durations, in milliseconds, pinned to the text:
/// "tile expansion 180 ms `cubic-bezier(.2,.8,.2,1)` on the allocated size,
/// hover 120 ms, body opacity 100 ms after 70 ms, caret 1100 ms
/// `steps(2, start)` with opacity 0 at 55 %".
pub const TILE_EXPAND_MS: u32 = 180;
pub const HOVER_MS: u32 = 120;
pub const BODY_OPACITY_MS: u32 = 100;
/// The body's opacity starts AFTER this delay, so its whole span is
/// `BODY_OPACITY_DELAY_MS + BODY_OPACITY_MS`.
pub const BODY_OPACITY_DELAY_MS: u32 = 70;
/// One full caret cycle; it is a two-step function, not a fade.
pub const CARET_PERIOD_MS: u32 = 1100;
/// Where in the cycle the caret's opacity becomes 0 (per cent).
pub const CARET_OFF_AT_PCT: u32 = 55;

/// The tile expansion's curve, `cubic-bezier(.2,.8,.2,1)` -- the control
/// points between the implicit (0,0) and (1,1).
pub const EXPAND_X1: f32 = 0.2;
pub const EXPAND_Y1: f32 = 0.8;
pub const EXPAND_X2: f32 = 0.2;
pub const EXPAND_Y2: f32 = 1.0;

/// The animation frame cadence, in milliseconds.
///
/// OURS, not section 10's -- the text pins durations and curves and states no
/// frame rate, so this is a compositor-independent choice and is marked as
/// one. 16 ms is ~60 Hz, which puts about eleven frames inside the shortest
/// span (`TILE_EXPAND_MS`) and two inside nothing shorter. It is a poll
/// TIMEOUT, not a clock: the loops wake at most this often while something is
/// animating, and not at all when nothing is.
pub const FRAME_MS: i32 = 16;

/// Does motion run at all?
///
/// TWO conditions, and the second is the interesting one. `word` is the
/// trimmed contents of `/env/HALCYON_MOTION` (None when the file is absent):
/// section 9.5 as amended at I-8 makes motion ON by default, with `0` the
/// opt-out that "restores exactly the behaviour this bullet describes" -- a
/// static caret and no transient animation.
///
/// `now_ns` is a sample of the monotonic clock, and a ZERO turns motion OFF.
/// `libthyla_rs::time::monotonic_ns` is documented fail-soft: it returns 0
/// when the clock is unreadable, forever. Every deadline built on it would
/// then sit permanently in the future and no animation would ever complete --
/// a tile frozen mid-expansion, which reads as a hung compositor rather than
/// as a broken clock. Degrading to the static default instead is not a
/// fallback invented here: it is a mode section 9.5 already specifies and
/// supports.
pub fn admitted(word: Option<&str>, now_ns: u64) -> bool {
    now_ns != 0 && stated(word)
}

/// The user's STATED preference, from the word ALONE -- [`admitted`] without
/// its clock conjunct.
///
/// Split out because the two conditions belong to different parties. The word
/// is the USER's and travels: the session reads it once and forwards it to the
/// compositor, which cannot read the user's `/env` at all. The clock is the
/// READER's, and the two readers do not even share a substrate -- halcyond
/// animates against `monotonic_ns` deadlines while tapestryd animates against
/// its own `Instant`-paced frame tick. Forwarding the folded verdict would
/// therefore hand the compositor one process's clock fault dressed as the
/// other process's user preference, and turn animations off on a machine whose
/// compositor clock is fine.
pub fn stated(word: Option<&str>) -> bool {
    !matches!(word.map(str::trim), Some("0"))
}

/// Progress through a span, 0.0 at the start and 1.0 once complete.
///
/// Saturating at both ends, and DELIBERATELY 1.0 rather than 0.0 on a dead
/// clock: a caller that reaches here with `now_ns == 0` despite [`admitted`]
/// lands on the animation's final state, which looks like no animation at
/// all. The opposite default would freeze it at its first frame.
pub fn phase(start_ns: u64, now_ns: u64, duration_ms: u32) -> f32 {
    if duration_ms == 0 || now_ns == 0 {
        return 1.0;
    }
    let elapsed_ms = now_ns.saturating_sub(start_ns) / 1_000_000;
    if elapsed_ms >= duration_ms as u64 {
        return 1.0;
    }
    elapsed_ms as f32 / duration_ms as f32
}

/// One coordinate of a cubic Bezier with endpoints 0 and 1 at parameter `t`.
#[inline]
fn bezier_axis(c1: f32, c2: f32, t: f32) -> f32 {
    let u = 1.0 - t;
    3.0 * u * u * t * c1 + 3.0 * u * t * t * c2 + t * t * t
}

/// A CSS timing function: given progress along X, the eased value on Y.
///
/// Solved by BISECTION over a fixed 20 iterations rather than Newton's
/// method, for two reasons that both matter here. `core` has no `f32::abs`
/// (nor `sqrt`/`powi`), so a convergence test on the residual would need
/// hand-rolling; and a fixed iteration count is deterministic, which a golden
/// comparison depends on. 20 halvings bound the parameter error at 2^-20,
/// far under a pixel of any span this drives.
pub fn cubic_bezier(x1: f32, y1: f32, x2: f32, y2: f32, progress: f32) -> f32 {
    if progress <= 0.0 {
        return 0.0;
    }
    if progress >= 1.0 {
        return 1.0;
    }
    let (mut lo, mut hi) = (0.0f32, 1.0f32);
    let mut t = progress;
    let mut i = 0;
    while i < 20 {
        if bezier_axis(x1, x2, t) < progress {
            lo = t;
        } else {
            hi = t;
        }
        t = (lo + hi) * 0.5;
        i += 1;
    }
    bezier_axis(y1, y2, t)
}

/// CSS `ease-out`, which is `cubic-bezier(0, 0, .58, 1)` by definition.
///
/// The split flash's curve (`animation: flash .25s ease-out forwards` in the
/// kit). Named as the CSS KEYWORD rather than spelled at its call site,
/// because the four control points are the keyword's definition and not a
/// choice anyone made here -- a call site that wrote them inline would read
/// as a tuning knob.
pub const EASE_OUT_X1: f32 = 0.0;
pub const EASE_OUT_Y1: f32 = 0.0;
pub const EASE_OUT_X2: f32 = 0.58;
pub const EASE_OUT_Y2: f32 = 1.0;

/// CSS `ease-out` applied to `progress`.
#[inline]
pub fn ease_out(progress: f32) -> f32 {
    cubic_bezier(EASE_OUT_X1, EASE_OUT_Y1, EASE_OUT_X2, EASE_OUT_Y2, progress)
}

/// Section 10's tile expansion curve applied to `progress`.
#[inline]
pub fn ease_expand(progress: f32) -> f32 {
    cubic_bezier(EXPAND_X1, EXPAND_Y1, EXPAND_X2, EXPAND_Y2, progress)
}

/// Is the caret painted at `elapsed_ms` into its cycle?
///
/// `steps(2, start)` with opacity 0 at 55 %: a two-valued function, not a
/// fade, so the caret is solid for the first 55 % of each period and absent
/// for the rest. Under reduced motion the caller does not call this at all --
/// section 9.5's default is a STATIC caret, which is painted, not blinking.
pub fn caret_visible(elapsed_ms: u64) -> bool {
    let period = CARET_PERIOD_MS as u64;
    let off_at = period * CARET_OFF_AT_PCT as u64 / 100;
    elapsed_ms % period < off_at
}

/// Milliseconds until [`caret_visible`] next changes value.
///
/// A poll deadline, and the reason it is not [`FRAME_MS`]. `steps(2, start)`
/// is a square wave: it changes exactly TWICE per period, and 1100 ms holds
/// 68.75 frames at 16 ms, so a loop that woke every frame would wake about
/// 34 times for each change it could see and 33 of those would paint
/// nothing. The distance to the next edge alternates 605 / 495 ms and is
/// never zero, so folding it into a poll timeout cannot spin.
///
/// The tweens of section 10 are the opposite shape -- continuous over 180 ms
/// -- and it is THEY that want `FRAME_MS`. One motion, one cadence, chosen
/// from what the motion actually does.
pub fn caret_next_step_ms(elapsed_ms: u64) -> i32 {
    let period = CARET_PERIOD_MS as u64;
    let off_at = period * CARET_OFF_AT_PCT as u64 / 100;
    let pos = elapsed_ms % period;
    let next = if pos < off_at { off_at } else { period };
    (next - pos) as i32
}

/// Fold one optional deadline into a poll timeout, where a NEGATIVE timeout
/// means "block indefinitely".
///
/// The two halcyond loops reduce their timeouts differently -- the console's
/// is always positive so it uses `min`, while the session's carries the `-1`
/// sentinel and needs the guard -- and the frame clock is a THIRD source that
/// would otherwise be folded in by hand at both, in two different forms. One
/// rule, testable without a compositor, in the shape `admit_status_bar`
/// established: `server.rs` and these loops are bin-side, where a decision
/// has no host witness at all.
#[inline]
pub fn fold_timeout(current: i32, next: Option<i32>) -> i32 {
    match next {
        Some(ms) if current < 0 || ms < current => ms,
        _ => current,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Section 10's durations, pinned ABSOLUTELY and read off the document
    /// rather than off any painter: a witness derived from the code it
    /// guards can only confirm that the code does what the code does.
    #[test]
    fn the_motion_durations_are_section_tens() {
        assert_eq!(TILE_EXPAND_MS, 180);
        assert_eq!(HOVER_MS, 120);
        assert_eq!(BODY_OPACITY_MS, 100);
        assert_eq!(BODY_OPACITY_DELAY_MS, 70, "the body starts AFTER 70 ms");
        assert_eq!(CARET_PERIOD_MS, 1100);
        assert_eq!(CARET_OFF_AT_PCT, 55);
        assert_eq!((EXPAND_X1, EXPAND_Y1, EXPAND_X2, EXPAND_Y2), (0.2, 0.8, 0.2, 1.0));
    }

    /// The curve's endpoints are exact, it never leaves the unit interval,
    /// and it is FRONT-LOADED -- `cubic-bezier(.2,.8,.2,1)` rises fast and
    /// settles, so half the time must buy well over half the distance. The
    /// last assertion is the one that would catch a transposed control pair,
    /// which the endpoint checks alone cannot see.
    #[test]
    fn the_expansion_curve_is_front_loaded_and_bounded() {
        assert_eq!(ease_expand(0.0), 0.0);
        assert_eq!(ease_expand(1.0), 1.0);
        let mid = ease_expand(0.5);
        assert!(mid > 0.5, "front-loaded: half the time buys more than half, got {}", mid);
        assert!(mid < 1.0);
        let mut prev = 0.0;
        for i in 0..=20 {
            let v = ease_expand(i as f32 / 20.0);
            assert!((0.0..=1.0).contains(&v), "left the unit interval at {}: {}", i, v);
            assert!(v >= prev, "not monotonic at {}", i);
            prev = v;
        }
    }

    /// `ease-out` is FRONT-LOADED like the expansion curve but less so, and
    /// the two must not be interchangeable: a flash that faded on the
    /// expansion's curve would hold its ink noticeably longer. The last
    /// assertion is the discriminating one -- it fails if either curve is
    /// substituted for the other, which an endpoints-and-monotonicity check
    /// would not see.
    #[test]
    fn ease_out_is_the_css_keyword_and_is_not_the_expansion_curve() {
        assert_eq!(ease_out(0.0), 0.0);
        assert_eq!(ease_out(1.0), 1.0);
        assert_eq!((EASE_OUT_X1, EASE_OUT_Y1, EASE_OUT_X2, EASE_OUT_Y2), (0.0, 0.0, 0.58, 1.0));
        let mid = ease_out(0.5);
        assert!(mid > 0.5, "front-loaded, got {}", mid);
        assert!(
            mid < ease_expand(0.5),
            "ease-out rises LESS fast than cubic-bezier(.2,.8,.2,1): {} vs {}",
            mid,
            ease_expand(0.5)
        );
        let mut prev = 0.0;
        for i in 0..=20 {
            let v = ease_out(i as f32 / 20.0);
            assert!((0.0..=1.0).contains(&v), "left the unit interval at {}", i);
            assert!(v >= prev, "not monotonic at {}", i);
            prev = v;
        }
    }

    /// Two steps, and the transition lands exactly where section 10 puts it:
    /// 55 % of 1100 ms is 605.
    #[test]
    fn the_caret_is_two_steps_with_the_off_at_fifty_five_percent() {
        assert!(caret_visible(0), "solid at the start of a cycle");
        assert!(caret_visible(604), "still solid one ms before the step");
        assert!(!caret_visible(605), "off at 55 %");
        assert!(!caret_visible(1099), "still off at the end of the cycle");
        assert!(caret_visible(1100), "and the next cycle begins solid");
        assert!(caret_visible(1100 + 604) && !caret_visible(1100 + 605), "it repeats");
    }

    /// The step deadline lands exactly on the edges, never past one and
    /// never on zero -- a zero would make the session's poll a spin. The
    /// last assertion is the load-bearing one: walking a whole period one
    /// millisecond at a time, the deadline must always point at the next
    /// disagreement of `caret_visible`, which is what makes a wake at that
    /// deadline the ONLY wake the caret needs.
    #[test]
    fn the_caret_deadline_is_the_distance_to_the_next_edge() {
        assert_eq!(caret_next_step_ms(0), 605, "a fresh cycle runs to the off step");
        assert_eq!(caret_next_step_ms(604), 1);
        assert_eq!(caret_next_step_ms(605), 495, "and from the off step to the next cycle");
        assert_eq!(caret_next_step_ms(1099), 1);
        assert_eq!(caret_next_step_ms(1100), 605, "the phase is free-running, not anchored");
        for pos in 0..CARET_PERIOD_MS as u64 {
            let d = caret_next_step_ms(pos) as u64;
            assert!(d > 0, "a zero deadline would spin the poll at {}", pos);
            assert_eq!(
                caret_visible(pos),
                caret_visible(pos + d - 1),
                "the value must hold right up to the deadline at {}",
                pos
            );
            assert_ne!(
                caret_visible(pos),
                caret_visible(pos + d),
                "and must have changed AT it at {}",
                pos
            );
        }
    }

    /// A dead monotonic clock turns motion OFF rather than freezing it, and
    /// the control is one variable away -- the same word with a live clock
    /// must still admit motion, or this passes on a function that always
    /// refuses.
    #[test]
    fn a_dead_clock_turns_motion_off() {
        assert!(!admitted(None, 0), "monotonic_ns is fail-soft 0; do not animate on it");
        assert!(admitted(None, 1), "the control: a live clock admits motion");
        assert!(!admitted(Some("1"), 0), "an explicit yes does not override a dead clock");
    }

    /// The word rule and the clock rule come apart exactly where they
    /// should: a dead clock refuses `admitted` while `stated` still reports
    /// what the user asked for, which is what the session forwards to a
    /// compositor whose own clock is a different clock.
    #[test]
    fn stated_is_the_word_alone_and_admitted_adds_the_clock() {
        assert!(stated(None), "absent means on");
        assert!(!stated(Some("0")));
        assert!(stated(Some("1")));
        assert!(!admitted(None, 0), "the clock refuses");
        assert!(stated(None), "but the user still asked for motion");
        for w in [None, Some("0"), Some("1"), Some(""), Some(" 0\n")] {
            assert_eq!(admitted(w, 7), stated(w), "a live clock leaves the word alone: {:?}", w);
            assert!(!admitted(w, 0), "a dead clock refuses everything: {:?}", w);
        }
    }

    /// Section 9.5 as amended: motion is ON by default and `0` is the
    /// opt-out. Absent file, empty file and any other word all mean on.
    #[test]
    fn the_env_word_zero_is_the_only_opt_out() {
        let t = 1_000_000_000u64;
        assert!(!admitted(Some("0"), t));
        assert!(!admitted(Some(" 0\n"), t), "trimmed, as the scale lever trims");
        assert!(admitted(None, t), "absent means ON since I-8");
        assert!(admitted(Some("1"), t));
        assert!(admitted(Some(""), t), "an empty file is not the opt-out word");
        assert!(admitted(Some("00"), t), "only the exact word opts out");
    }

    /// Progress saturates at both ends and completes on a dead clock rather
    /// than sticking at zero -- a frozen tween reads as a hung compositor.
    #[test]
    fn phase_saturates_and_completes_on_a_dead_clock() {
        let start = 1_000_000_000u64;
        assert_eq!(phase(start, start, TILE_EXPAND_MS), 0.0);
        assert_eq!(phase(start, start + 90_000_000, TILE_EXPAND_MS), 0.5, "90 of 180 ms");
        assert_eq!(phase(start, start + 180_000_000, TILE_EXPAND_MS), 1.0);
        assert_eq!(phase(start, start + 999_000_000, TILE_EXPAND_MS), 1.0, "saturates");
        assert_eq!(phase(start, start - 5, TILE_EXPAND_MS), 0.0, "a clock that went backwards");
        assert_eq!(phase(start, 0, TILE_EXPAND_MS), 1.0, "dead clock completes, never freezes");
        assert_eq!(phase(start, start + 1, 0), 1.0, "a zero span is already done");
    }

    /// The fold expresses BOTH existing call sites. The console's timeout is
    /// always positive and reduces by `min`; the session's carries the -1
    /// sentinel, where any finite deadline must win. A fold that forgot the
    /// sentinel would return -1 and block forever with an animation pending.
    #[test]
    fn fold_timeout_respects_the_infinite_sentinel() {
        assert_eq!(fold_timeout(-1, Some(16)), 16, "any deadline beats blocking forever");
        assert_eq!(fold_timeout(-1, None), -1, "nothing pending: keep blocking");
        assert_eq!(fold_timeout(60_000, Some(16)), 16, "the nearer deadline wins");
        assert_eq!(fold_timeout(16, Some(60_000)), 16, "and it is not order-dependent");
        assert_eq!(fold_timeout(16, None), 16);
        // The console's site, expressed through the fold.
        assert_eq!(fold_timeout(60_000, Some(1_800)), 1_800);
        // The session's site: a -1 base, then the clock, then a notice.
        let t = fold_timeout(fold_timeout(-1, Some(59_000)), Some(1_800));
        assert_eq!(t, 1_800);
    }
}
