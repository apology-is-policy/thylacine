// carve -- the Instrument profile's split and stack arithmetic
// (HALCYON-INSTRUMENT 5.2 / 5.4), pure and host-tested. tapestryd's pane
// tree calls it with the scaled table and stores the rectangles; nothing
// here knows a pane, a surface or a display.
//
// The one snap rule (5.2): a container's boundaries are computed as exact
// rationals and snapped ONCE, round half up; adjacent extents are
// differences of snapped boundaries, never two independently rounded
// widths, so the children partition the parent exactly minus the tracks.
// Measured against Chromium's raster of the reference layout at 1440 x 900
// (JOURNAL run 46o, "I-2"): the browser lays out in 1/64 px units and snaps
// each box's edges to the nearest device pixel, which is this rule applied
// to the same boundaries -- the root divider lands on columns 738..744 and
// the right column's on rows 443..449 in both.

use alloc::vec::Vec;

/// A half-open span along one axis.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Span {
    pub start: u32,
    pub end: u32,
}

impl Span {
    pub fn len(self) -> u32 {
        self.end.saturating_sub(self.start)
    }
    pub fn is_empty(self) -> bool {
        self.end <= self.start
    }
}

/// The logical minima (5.2), at 100; the caller scales them. A pane is never
/// narrower than `MIN_PANE_W` outside, and a stack's open body never shorter
/// than `MIN_BODY_H`.
pub const MIN_PANE_W: i32 = 260;
pub const MIN_BODY_H: i32 = 54;

/// A container's children carry `u16` weights, sum-normalised, equal by
/// default (5.2). The default is 1: a saved tree with every weight default
/// is byte-identical `halcyon-layout v1`.
pub const DEFAULT_WEIGHT: u16 = 1;

/// Divide `extent` from `origin` among `weights.len()` children separated
/// by `track`-wide tracks. Child `i` gets `U * w_i / sum(w)` of the usable
/// extent `U = extent - (n - 1) * track`, clamped up to `minima[i]` with the
/// deficit taken from the others in proportion (the flex rule: a child at
/// its minimum is frozen and the rest re-share the remainder). When the
/// minima alone exceed `U` every child is laid at its minimum from the
/// origin and the last ones overrun `extent` -- the caller clips; a display
/// that small keeps its data and scrolls (5.2), it never drops a child.
///
/// A zero weight counts as one (the verb refuses 0; this stays total). A
/// missing minimum is 0.
pub fn split_spans(origin: u32, extent: u32, track: u32, weights: &[u16], minima: &[u32]) -> Vec<Span> {
    let n = weights.len();
    let mut out: Vec<Span> = Vec::with_capacity(n);
    if n == 0 {
        return out;
    }
    let t = track as u128;
    let tracks = (n as u128 - 1) * t;
    let usable = (extent as u128).saturating_sub(tracks);
    let w = |i: usize| -> u128 { weights[i].max(1) as u128 };
    let min = |i: usize| -> u128 { minima.get(i).copied().unwrap_or(0) as u128 };
    let min_sum: u128 = (0..n).map(min).sum();
    if min_sum > usable {
        // Overflow: minima from the origin, integer, no snap needed.
        let mut b = origin as u128;
        for i in 0..n {
            let start = b;
            let end = start + min(i);
            out.push(Span {
                start: clamp_u32(start),
                end: clamp_u32(end),
            });
            b = end + t;
        }
        return out;
    }
    // The flex loop: freeze every child whose ideal share falls below its
    // minimum, then re-share the remainder among the rest until none does.
    let mut frozen = alloc::vec![false; n];
    let (mut s, mut r);
    loop {
        s = (0..n).filter(|&i| !frozen[i]).map(w).sum::<u128>();
        r = usable - (0..n).filter(|&i| frozen[i]).map(min).sum::<u128>();
        if s == 0 {
            break;
        }
        let mut any = false;
        for i in 0..n {
            if !frozen[i] && r * w(i) < min(i) * s {
                frozen[i] = true;
                any = true;
            }
        }
        if !any {
            break;
        }
    }
    // Every child frozen: the extents are the minima over a unit
    // denominator (r is then exactly the slack, which nobody takes -- the
    // last child ends short of `extent`; the caller sees a shorter last
    // span, never an overrun).
    let den = if s == 0 { 1 } else { s };
    let ext_num = |i: usize| -> u128 {
        if frozen[i] {
            min(i) * den
        } else {
            r * w(i)
        }
    };
    // b_k = origin + sum_{i<k} ext_i + k * t, snapped once; child_k =
    // [b_k, b_{k+1} - t).
    let snap = |num: u128| -> u128 { (num + den / 2) / den };
    let mut num = origin as u128 * den;
    let mut starts: Vec<u128> = Vec::with_capacity(n + 1);
    for i in 0..n {
        starts.push(snap(num));
        num += ext_num(i) + t * den;
    }
    starts.push(snap(num));
    for i in 0..n {
        let start = starts[i];
        let end = starts[i + 1].saturating_sub(t).max(start);
        out.push(Span {
            start: clamp_u32(start),
            end: clamp_u32(end),
        });
    }
    out
}

fn clamp_u32(v: u128) -> u32 {
    if v > u32::MAX as u128 {
        u32::MAX
    } else {
        v as u32
    }
}

/// The stack's header/body allocation (5.4) inside a frame's inner box
/// (`inner_y`, `inner_h`): `n` tiles, `open` expanded, each header
/// `header` tall (a collapsed tile's box, its separator inside it), the open
/// tile's separator `sep` below its body unless it is last. Headers before
/// the open tile stack down from the top, headers after it stack up from
/// the bottom, the body is what remains. Returns the header spans in tile
/// order and the body span (empty when the box cannot hold it; the caller
/// clips every span to the inner box).
pub fn stack_alloc(inner_y: u32, inner_h: u32, n: usize, open: usize, header: u32, sep: u32) -> (Vec<Span>, Span) {
    let mut headers: Vec<Span> = Vec::with_capacity(n);
    if n == 0 {
        return (
            headers,
            Span {
                start: inner_y,
                end: inner_y + inner_h,
            },
        );
    }
    let open = open.min(n - 1);
    let bottom = inner_y.saturating_add(inner_h);
    for i in 0..n {
        let start = if i <= open {
            inner_y.saturating_add((i as u32).saturating_mul(header))
        } else {
            bottom.saturating_sub(((n - i) as u32).saturating_mul(header))
        };
        headers.push(Span {
            start,
            end: start.saturating_add(header),
        });
    }
    let body_start = headers[open].end;
    let after = (n - 1 - open) as u32;
    let b = if open + 1 < n { sep } else { 0 };
    let body_end = bottom.saturating_sub(after.saturating_mul(header)).saturating_sub(b);
    let body = Span {
        start: body_start,
        end: body_end.max(body_start),
    };
    (headers, body)
}

/// A stack's minimum outer height (5.2): the frame, `n` headers, the open
/// tile's separator (worst case: the open tile is not last, so only when
/// there is more than one tile) and the body minimum.
pub fn stack_min_h(n: usize, header: u32, sep: u32, body_min: u32, frame: u32) -> u32 {
    let sep = if n > 1 { sep } else { 0 };
    (2 * frame)
        .saturating_add((n as u32).saturating_mul(header))
        .saturating_add(sep)
        .saturating_add(body_min)
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn spans(v: &[(u32, u32)]) -> Vec<Span> {
        v.iter().map(|&(start, end)| Span { start, end }).collect()
    }

    /// The 1440 x 900 reference (5.2; the browser's raster, JOURNAL run
    /// 46o "I-2"): the root divides 1434 at 0.515 with the divider on
    /// columns 738..744; the right column divides 835 at 0.49 with its
    /// divider on rows 443..449.
    #[test]
    fn the_reference_layout_snaps_where_chromium_did() {
        assert_eq!(
            split_spans(3, 1434, 7, &[515, 485], &[0, 0]),
            spans(&[(3, 738), (745, 1437)])
        );
        assert_eq!(
            split_spans(37, 835, 7, &[49, 51], &[0, 0]),
            spans(&[(37, 443), (450, 872)])
        );
        // At 200 % the same layout in a 2880 x 1800 framebuffer: every token
        // doubled, the boundary 1475.81 snapping to 1476 as the browser's
        // device-pixel edge did.
        assert_eq!(
            split_spans(6, 2868, 14, &[515, 485], &[0, 0]),
            spans(&[(6, 1476), (1490, 2874)])
        );
    }

    /// Two children ARE the mockup's flex result `first = r * (E - t)`,
    /// `second = (1 - r) * (E - t)` (5.2): with integer weights (p, q) the
    /// first extent is `U * p / (p + q)` exactly, snapped once.
    #[test]
    fn two_children_are_the_flex_identity() {
        for &(e, p, q) in &[(1434u32, 515u16, 485u16), (835, 49, 51), (1000, 1, 1), (777, 22, 78), (300, 78, 22)] {
            let s = split_spans(0, e, 7, &[p, q], &[0, 0]);
            let u = (e - 7) as u64;
            let ideal_x256 = u * 256 * p as u64 / (p + q) as u64; // 1/256 px
            let first = s[0].len() as u64;
            // Snapped once: within half a pixel of the ideal.
            assert!(
                (first * 256).abs_diff(ideal_x256) <= 128,
                "E {e} {p}:{q}: first {first} vs ideal {}/256",
                ideal_x256
            );
            assert_eq!(s[0].len() + s[1].len() + 7, e, "the pair partitions E minus the track");
            assert_eq!(s[1].start - s[0].end, 7, "one track between them");
        }
    }

    /// N children partition the extent minus the tracks exactly, in order,
    /// each track exactly `t` wide -- for weights a small LCG produces.
    #[test]
    fn children_partition_the_extent_minus_the_tracks() {
        let mut x: u32 = 12345;
        let mut next = || {
            x = x.wrapping_mul(1_103_515_245).wrapping_add(12345);
            (x >> 16) as u16
        };
        for n in 1..=8usize {
            for &(origin, extent, t) in &[(3u32, 1434u32, 7u32), (0, 100, 7), (37, 835, 7), (6, 2868, 14), (0, 50, 9)] {
                let w: Vec<u16> = (0..n).map(|_| next() % 900 + 1).collect();
                let s = split_spans(origin, extent, t, &w, &vec![0; n]);
                assert_eq!(s.len(), n);
                assert_eq!(s[0].start, origin);
                if (n as u32 - 1) * t > extent {
                    // The tracks alone overrun the extent: every child is
                    // empty at its track position and the last overruns --
                    // the caller clips (a 50 px parent with seven children).
                    for (i, sp) in s.iter().enumerate() {
                        assert_eq!((sp.start, sp.end), (origin + i as u32 * t, origin + i as u32 * t), "{:?}", s);
                    }
                    continue;
                }
                assert_eq!(s[n - 1].end, origin + extent, "the last child ends at E ({:?})", s);
                let mut total = 0;
                for i in 0..n {
                    assert!(s[i].end >= s[i].start, "{:?}", s);
                    total += s[i].len();
                    if i + 1 < n {
                        assert_eq!(s[i + 1].start - s[i].end, t, "track {i} of {:?}", s);
                    }
                }
                assert_eq!(total + (n as u32 - 1) * t, extent, "{:?}", s);
            }
        }
    }

    #[test]
    fn equal_weights_divide_equally_with_the_remainder_spread_by_the_snap() {
        // U = 86 over 3 = 28.667: boundaries 0, 35.667 -> 36, 71.333 -> 71.
        assert_eq!(split_spans(0, 100, 7, &[1, 1, 1], &[0; 3]), spans(&[(0, 29), (36, 64), (71, 100)]));
        // One child takes the whole extent, no track.
        assert_eq!(split_spans(5, 100, 7, &[1], &[0]), spans(&[(5, 105)]));
        assert!(split_spans(0, 100, 7, &[], &[]).is_empty());
    }

    #[test]
    fn a_minimum_freezes_a_child_and_the_rest_share_the_remainder() {
        // ideal_0 = 100 * 1/10 = 10 < 30: frozen at 30, the other takes 70.
        assert_eq!(split_spans(0, 107, 7, &[1, 9], &[30, 0]), spans(&[(0, 30), (37, 107)]));
        // Two frozen, one free: minima 40 + 40, the free child gets 100 - 80.
        assert_eq!(
            split_spans(0, 114, 7, &[1, 1, 98], &[40, 40, 0]),
            spans(&[(0, 40), (47, 87), (94, 114)])
        );
        // A minimum the ideal already clears changes nothing.
        assert_eq!(split_spans(0, 107, 7, &[1, 1], &[30, 30]), split_spans(0, 107, 7, &[1, 1], &[0, 0]));
    }

    #[test]
    fn minima_that_overflow_stack_from_the_origin() {
        // 60 + 60 > 93: each at its minimum, the second overruns 100 -- the
        // caller clips, nothing is dropped.
        assert_eq!(split_spans(0, 100, 7, &[1, 1], &[60, 60]), spans(&[(0, 60), (67, 127)]));
    }

    #[test]
    fn weights_of_zero_count_as_one() {
        assert_eq!(split_spans(0, 107, 7, &[0, 0], &[0, 0]), split_spans(0, 107, 7, &[1, 1], &[0, 0]));
    }

    /// The header/body allocation against the reference's panes (5.4; the
    /// browser's boxes): p1 -- inner 38..871, four tiles, the second open;
    /// p3 -- inner 451..871, three tiles, the second open; p2 -- three
    /// tiles, the FIRST open.
    #[test]
    fn the_stack_allocation_matches_the_reference() {
        let (h, body) = stack_alloc(38, 833, 4, 1, 32, 1);
        assert_eq!(h, spans(&[(38, 70), (70, 102), (807, 839), (839, 871)]));
        assert_eq!(body, Span { start: 102, end: 806 }, "704 tall: 737 - 32 - 1");
        let (h, body) = stack_alloc(451, 420, 3, 1, 32, 1);
        assert_eq!(h, spans(&[(451, 483), (483, 515), (839, 871)]));
        assert_eq!(body, Span { start: 515, end: 838 });
        let (h, body) = stack_alloc(38, 404, 3, 0, 32, 1);
        assert_eq!(h, spans(&[(38, 70), (378, 410), (410, 442)]));
        assert_eq!(body, Span { start: 70, end: 377 });
    }

    #[test]
    fn a_last_open_tile_has_no_separator_and_a_lone_tile_is_a_stack_of_one() {
        let (h, body) = stack_alloc(100, 300, 3, 2, 32, 1);
        assert_eq!(h, spans(&[(100, 132), (132, 164), (164, 196)]));
        assert_eq!(body, Span { start: 196, end: 400 }, "no separator below the last tile");
        let (h, body) = stack_alloc(38, 833, 1, 0, 32, 1);
        assert_eq!(h, spans(&[(38, 70)]));
        assert_eq!(body, Span { start: 70, end: 871 });
        // Too short for its headers: the body is empty, never negative; the
        // headers still have their positions (the caller clips).
        let (_, body) = stack_alloc(0, 40, 3, 1, 32, 1);
        assert!(body.is_empty());
        // An out-of-range open index is clamped to the last tile.
        let (_, body) = stack_alloc(0, 300, 2, 7, 32, 1);
        assert_eq!(body, Span { start: 64, end: 300 });
    }

    #[test]
    fn the_stack_minimum_is_the_frame_the_headers_the_separator_and_a_body() {
        assert_eq!(stack_min_h(1, 32, 1, 54, 1), 88, "2 + 32 + 54: a lone tile has no separator");
        assert_eq!(stack_min_h(4, 32, 1, 54, 1), 185, "2 + 128 + 1 + 54");
        assert_eq!(stack_min_h(2, 64, 2, 108, 2), 4 + 128 + 2 + 108, "the 200 % table");
    }
}
