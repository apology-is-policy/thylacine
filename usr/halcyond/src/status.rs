// status -- the status bar's rules (HALCYON.md 13.6 H-3d; HALCYON-VISUAL
// section 6; the operator's Daylight mockups for the content): the pure
// half. One bar at the bottom of the screen, 20px, dark against the light
// theme -- the one piece of chrome that belongs to the system rather than
// to any pane. Four slots, left to right: workspaces (ONE filled indicator
// until a workspace list exists -- the 2026-09-02 vote), the focused
// context (the focused tile's program, its working directory, its
// running-or-last command -- centred in what the other slots leave), the
// condition (the turnstile and `ok` / `exit N` in the key's ink, from the
// focused pane's recorded status -- the SAME record the live tile keys; the
// bar is the redundant channel), and the clock. Every slot is the
// proportional face (section 7: a path or a command in chrome is
// proportional; a mono island means a program's verbatim output, which no
// chrome is). The bin (`statusset`) owns the surface and the sources; every
// pixel decision is here, under host tests.

use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use libhalcyon::theme::{Argb, DAYLIGHT, METRICS};

use crate::raster::{GlyphSource, FACE_BODY};

/// The bar's typeface size (the mockups' `.hal-status`: 10px).
pub const STATUS_PX: f32 = 10.0;

/// The condition slot's state -- the focused pane's `status` file, section
/// 1.4's two states (sage / cinnabar) plus idle.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Condition {
    Idle,
    Ok,
    Err,
}

/// The pane's recorded status text (`resting|ok|err`) as a condition. Only
/// `err` is the failure state; `ok` AND `resting` are the good one --
/// section 4.2's "exit 0 (or nothing has run yet)" is one state, and the
/// mockup shows `⊢ ok` on a tile nothing has run in. Anything else
/// (unreadable, unknown) is idle: the slot claims nothing.
pub fn condition_for(status: &str) -> Condition {
    match status.trim() {
        "ok" | "resting" => Condition::Ok,
        "err" => Condition::Err,
        _ => Condition::Idle,
    }
}

/// What the bar shows. `workspaces`/`active` are 1/0 until H-4.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct StatusModel {
    pub workspaces: u8,
    pub active: u8,
    /// The focused tile's program (its strip's name); empty when nothing is
    /// focused.
    pub name: String,
    /// The focused tile's working directory (OSC 7); empty otherwise.
    pub cwd: String,
    /// The focused tile's running-or-last command; empty otherwise.
    pub cmd: String,
    pub condition: Condition,
    /// The focused tile's last exit code (the `exit N` label); None when
    /// unknown -- the label then says `err`.
    pub exit_code: Option<i64>,
    /// Hours and minutes (the wall clock's UTC; the RTC's own zone).
    pub hour: u8,
    pub minute: u8,
}

impl StatusModel {
    pub fn empty() -> StatusModel {
        StatusModel {
            workspaces: 1,
            active: 0,
            name: String::new(),
            cwd: String::new(),
            cmd: String::new(),
            condition: Condition::Idle,
            exit_code: None,
            hour: 0,
            minute: 0,
        }
    }
}

/// The context slot's text: the parts joined by the middle dot, empties
/// dropped -- "transcript · /lib/aurora · make check".
pub fn context_text(name: &str, cwd: &str, cmd: &str) -> String {
    let mut out = String::new();
    for part in [name, cwd, cmd] {
        let p = part.trim();
        if p.is_empty() {
            continue;
        }
        if !out.is_empty() {
            out.push_str(" \u{b7} ");
        }
        out.push_str(p);
    }
    out
}

/// The condition's label: `ok`; `exit N` (or `err` with no code known);
/// nothing while idle.
pub fn condition_label(c: Condition, exit_code: Option<i64>) -> String {
    match c {
        Condition::Idle => String::new(),
        Condition::Ok => String::from("ok"),
        Condition::Err => match exit_code {
            Some(n) => {
                let mut s = String::new();
                let _ = core::fmt::write(&mut s, format_args!("exit {}", n));
                s
            }
            None => String::from("err"),
        },
    }
}

/// The condition's ink (the mockups' `.hal-status-ok` / `-err`): ember for
/// the good state -- sage does not read on the dark bar, and the ember is
/// the theme's own accent, the same "fine, carry on" the turnstile means at
/// the prompt -- and the cinnabar key for a failure.
pub fn condition_ink(c: Condition) -> Argb {
    let d = &DAYLIGHT;
    match c {
        Condition::Idle => d.status_idle,
        Condition::Ok => d.ember,
        Condition::Err => d.cinnabar.key,
    }
}

/// Where each slot landed, in bar pixels (x, w) -- the witness reads these
/// off the bin's say line to know where to look. `ctx` is the span the
/// context may use; `ctx_ink` is where its text actually landed (centred
/// when it fits, from the span's left when it had to truncate).
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Slots {
    pub ws: (i32, i32),
    pub ctx: (i32, i32),
    pub ctx_ink: (i32, i32),
    pub cond: (i32, i32),
    pub clock: (i32, i32),
}

impl Slots {
    /// The slot GEOMETRY alone -- `ctx_ink` zeroed. The say line is keyed on
    /// this, never on where the centred text landed: that moves with every
    /// context text, and a say per paint writes itself into the console
    /// transcript it witnesses (the drain mirrors every daemon line), which
    /// is one extra row after every command for the row-relative legs.
    pub fn geometry(&self) -> Slots {
        Slots {
            ctx_ink: (0, 0),
            ..*self
        }
    }
}

/// The horizontal padding at the bar's ends and around the context.
const PAD: i32 = 8;
/// The gap inside the right group (condition, clock).
const GAP: i32 = 8;
/// A workspace indicator's horizontal padding; the box is the bar's height.
const WS_PAD: i32 = 7;
/// The turnstile, the prompt's own glyph, leading the condition label.
const TURNSTILE: char = '\u{22A2}';

struct Run {
    refs: Vec<GlyphRef>,
    width: i32,
}

fn shape(gs: &mut GlyphSource, text: &str) -> Run {
    let mut refs: Vec<GlyphRef> = Vec::new();
    let mut width = 0;
    for ch in text.chars() {
        if let Some(g) = gs.glyph(FACE_BODY, STATUS_PX, ch) {
            width += g.advance;
            refs.push(g);
        }
    }
    Run { refs, width }
}

/// The bar's display list for a `w` x `h` surface, and where the slots
/// landed. Right to left: the clock, the condition; then the workspaces at
/// the left; the context takes what is left between them -- centred there
/// when it fits, else from the left, truncated with an ellipsis (the slot
/// that yields). A zero-sized bar yields an empty list.
pub fn status_list(m: &StatusModel, w: u32, h: u32, gs: &mut GlyphSource) -> (Cartoon, Slots) {
    let mut cart = Cartoon::new();
    let mut slots = Slots::default();
    if w == 0 || h == 0 {
        return (cart, slots);
    }
    let d = &DAYLIGHT;
    let (wi, hi) = (w as i32, h as i32);
    cart.ops.push(Op::Clear { color: d.status_bg });
    let (asc, desc) = gs
        .line_metrics(FACE_BODY, STATUS_PX)
        .map(|mm| (mm.ascent, mm.descent))
        .unwrap_or((8, 2));
    let baseline = (hi - (asc + desc)) / 2 + asc;
    let gen = gs.gen();

    // The clock, right-aligned, in the bar's muted ink.
    let mut clock = String::new();
    let _ = core::fmt::write(&mut clock, format_args!("{:02}:{:02}", m.hour, m.minute));
    let crun = shape(gs, &clock);
    let clock_x = wi - PAD - crun.width;
    if !crun.refs.is_empty() && clock_x > 0 {
        cart.push_glyphs(gen, clock_x, baseline, d.status_muted, &crun.refs);
    }
    slots.clock = (clock_x, crun.width);

    // The condition: the turnstile + its label in the key's ink, left of
    // the clock; nothing (and no width) while idle.
    let label = condition_label(m.condition, m.exit_code);
    let (cond_x, cond_w) = if label.is_empty() {
        (clock_x - GAP, 0)
    } else {
        let mut text = String::new();
        text.push(TURNSTILE);
        text.push(' ');
        text.push_str(&label);
        let run = shape(gs, &text);
        let x = clock_x - GAP - run.width;
        if !run.refs.is_empty() && x > 0 {
            cart.push_glyphs(gen, x, baseline, condition_ink(m.condition), &run.refs);
        }
        (x, run.width)
    };
    slots.cond = (cond_x, cond_w);

    // The workspaces: one indicator per workspace, the bar's full height;
    // the active one an ember box with the number in the bar's own dark,
    // the rest the number in `status_idle` on the bar.
    let mut x = PAD;
    for i in 0..m.workspaces.max(1) {
        let mut num = String::new();
        let _ = core::fmt::write(&mut num, format_args!("{}", i + 1));
        let nrun = shape(gs, &num);
        let box_w = nrun.width + 2 * WS_PAD;
        if i == m.active {
            cart.ops.push(Op::Rect {
                x,
                y: 0,
                w: box_w as u32,
                h: h,
                color: d.ember,
            });
            if !nrun.refs.is_empty() {
                cart.push_glyphs(gen, x + WS_PAD, baseline, d.status_bg, &nrun.refs);
            }
        } else if !nrun.refs.is_empty() {
            cart.push_glyphs(gen, x + WS_PAD, baseline, d.status_idle, &nrun.refs);
        }
        x += box_w;
    }
    slots.ws = (PAD, x - PAD);

    // The context, in what is left between the workspaces and the right
    // group: centred when it fits, else from the left with an ellipsis.
    let span_x = x + PAD;
    let avail = cond_x - PAD - span_x;
    slots.ctx = (span_x, avail.max(0));
    if avail > 0 {
        let text = context_text(&m.name, &m.cwd, &m.cmd);
        let mut run = shape(gs, &text);
        if run.width <= avail {
            let tx = span_x + (avail - run.width) / 2;
            if !run.refs.is_empty() {
                cart.push_glyphs(gen, tx, baseline, d.status_fg, &run.refs);
            }
            slots.ctx_ink = (tx, run.width);
        } else {
            let ell = shape(gs, "\u{2026}");
            while run.width + ell.width > avail {
                match run.refs.pop() {
                    Some(g) => run.width -= g.advance,
                    None => break,
                }
            }
            run.refs.extend_from_slice(&ell.refs);
            run.width += ell.width;
            if !run.refs.is_empty() {
                cart.push_glyphs(gen, span_x, baseline, d.status_fg, &run.refs);
            }
            slots.ctx_ink = (span_x, run.width);
        }
    }
    (cart, slots)
}

/// The bar's height: the one vertical unit (Daylight 8).
pub fn bar_height() -> u32 {
    METRICS.status_h as u32
}

#[cfg(test)]
mod tests {
    use super::*;

    fn model() -> StatusModel {
        StatusModel {
            workspaces: 1,
            active: 0,
            name: String::from("transcript"),
            cwd: String::from("/lib/aurora"),
            cmd: String::from("make check"),
            condition: Condition::Ok,
            exit_code: Some(0),
            hour: 14,
            minute: 22,
        }
    }

    /// The glyph runs of a list: (x, ink, glyph count, width), in order.
    fn runs(c: &Cartoon) -> Vec<(i32, Argb, usize, i32)> {
        c.ops
            .iter()
            .filter_map(|op| match *op {
                Op::Glyphs {
                    baseline_x,
                    color,
                    start,
                    count,
                    ..
                } => {
                    let g = &c.runs[start as usize..(start + count) as usize];
                    Some((baseline_x, color, g.len(), g.iter().map(|r| r.advance).sum()))
                }
                _ => None,
            })
            .collect()
    }

    #[test]
    fn condition_is_the_panes_two_states_plus_idle() {
        assert_eq!(condition_for("ok\n"), Condition::Ok);
        assert_eq!(condition_for("err"), Condition::Err);
        assert_eq!(
            condition_for("resting\n"),
            Condition::Ok,
            "nothing run yet is the good state (section 4.2)"
        );
        assert_eq!(condition_for(""), Condition::Idle, "unreadable claims nothing");
        assert_eq!(
            condition_for("warning"),
            Condition::Idle,
            "warnings do not promote"
        );
    }

    #[test]
    fn the_label_is_ok_or_the_exit_code() {
        assert_eq!(condition_label(Condition::Ok, Some(0)), "ok");
        assert_eq!(condition_label(Condition::Ok, None), "ok");
        assert_eq!(condition_label(Condition::Err, Some(1)), "exit 1");
        assert_eq!(condition_label(Condition::Err, Some(-1)), "exit -1");
        assert_eq!(condition_label(Condition::Err, None), "err");
        assert_eq!(condition_label(Condition::Idle, Some(3)), "");
    }

    #[test]
    fn the_condition_inks_are_the_mockups() {
        assert_eq!(condition_ink(Condition::Ok), DAYLIGHT.ember);
        assert_eq!(condition_ink(Condition::Err), DAYLIGHT.cinnabar.key);
        assert_eq!(condition_ink(Condition::Idle), DAYLIGHT.status_idle);
    }

    #[test]
    fn context_joins_the_present_parts_with_the_middle_dot() {
        assert_eq!(
            context_text("transcript", "/lib/aurora", "make check"),
            "transcript \u{b7} /lib/aurora \u{b7} make check"
        );
        assert_eq!(context_text("hx", "", ""), "hx");
        assert_eq!(context_text("", "/x", ""), "/x");
        assert_eq!(context_text("", "", ""), "");
    }

    #[test]
    fn the_list_is_dark_ground_then_the_four_slots_right_to_left_of_each_other() {
        let mut gs = GlyphSource::new_vendored(64);
        let (c, s) = status_list(&model(), 1280, 20, &mut gs);
        assert!(matches!(c.ops[0], Op::Clear { color: 0xFF1A120A }));
        assert!(
            s.ws.0 == PAD && s.ws.1 >= 2 * WS_PAD,
            "the workspace indicator at the left: {:?}",
            s.ws
        );
        assert!(s.ctx.0 > s.ws.0 + s.ws.1, "the context after the workspaces");
        assert!(s.cond.0 > s.ctx.0, "the condition after the context");
        assert!(
            s.clock.0 > s.cond.0 + s.cond.1,
            "the clock after the condition"
        );
        assert!(
            s.clock.0 + s.clock.1 <= 1280 - PAD,
            "the clock ends inside the right pad"
        );
        // The active indicator is an ember box the bar's full height.
        assert!(
            c.ops.iter().any(|o| matches!(
                o,
                Op::Rect {
                    x: PAD,
                    y: 0,
                    h: 20,
                    color: 0xFFE07840,
                    ..
                }
            )),
            "the ember box, full height"
        );
        let r = runs(&c);
        // The condition run: the turnstile + " ok" in ember; the clock in
        // the muted ink; the context centred in its span in the bar's ink.
        let cond = r
            .iter()
            .find(|x| x.0 == s.cond.0)
            .expect("the condition run at its slot");
        assert_eq!(cond.1, DAYLIGHT.ember);
        assert_eq!(cond.2, "\u{22A2} ok".chars().count());
        assert_eq!(cond.3, s.cond.1);
        let clock = r.iter().find(|x| x.0 == s.clock.0).expect("the clock run");
        assert_eq!(clock.1, DAYLIGHT.status_muted);
        let ctx = r
            .iter()
            .find(|x| x.0 == s.ctx_ink.0 && x.1 == DAYLIGHT.status_fg)
            .expect("the context run");
        assert_eq!(ctx.3, s.ctx_ink.1);
        let left = s.ctx_ink.0 - s.ctx.0;
        let right = (s.ctx.0 + s.ctx.1) - (s.ctx_ink.0 + s.ctx_ink.1);
        assert!(
            (left - right).abs() <= 1,
            "centred in the span: left margin {} vs right {}",
            left,
            right
        );
        // A failure: the same slot in cinnabar, labelled with the code; the
        // clock does not move with the condition's width.
        let (e, es) = status_list(
            &StatusModel {
                condition: Condition::Err,
                exit_code: Some(1),
                ..model()
            },
            1280,
            20,
            &mut gs,
        );
        assert_eq!(es.clock, s.clock, "the clock does not move with the condition");
        let er = runs(&e);
        let econd = er
            .iter()
            .find(|x| x.0 == es.cond.0)
            .expect("the failure's condition run");
        assert_eq!(econd.1, DAYLIGHT.cinnabar.key);
        assert_eq!(econd.2, "\u{22A2} exit 1".chars().count());
        assert!(es.cond.1 > s.cond.1, "`exit 1` is wider than `ok`");
        // Idle: no condition drawn, no width; the context span reaches the
        // clock's gap.
        let (ci, is) = status_list(
            &StatusModel {
                condition: Condition::Idle,
                ..model()
            },
            1280,
            20,
            &mut gs,
        );
        assert_eq!(is.cond.1, 0);
        assert!(
            !runs(&ci).iter().any(|x| x.1 == DAYLIGHT.ember && x.0 > PAD + 2 * WS_PAD),
            "no ember run beyond the workspace box while idle"
        );
        assert!(is.ctx.1 > s.ctx.1, "the context span grew into the idle slot");
    }

    // The say line's key: two paints whose only difference is where the
    // centred context landed share one geometry (no say between them); a
    // condition change moves the slots and does not.
    #[test]
    fn the_geometry_key_ignores_where_the_context_landed() {
        let mut gs = GlyphSource::new_vendored(64);
        let (_, a) = status_list(&model(), 1280, 20, &mut gs);
        let (_, b) = status_list(
            &StatusModel {
                cmd: String::from("a much longer command line than before"),
                ..model()
            },
            1280,
            20,
            &mut gs,
        );
        assert_ne!(a.ctx_ink, b.ctx_ink, "the centred text moved");
        assert_eq!(a.geometry(), b.geometry(), "the geometry did not");
        let (_, e) = status_list(
            &StatusModel {
                condition: Condition::Err,
                exit_code: Some(1),
                ..model()
            },
            1280,
            20,
            &mut gs,
        );
        assert_ne!(a.geometry(), e.geometry(), "a condition change is a geometry change");
    }

    #[test]
    fn a_narrow_bar_truncates_the_context_from_the_left_and_keeps_the_rest() {
        let mut gs = GlyphSource::new_vendored(64);
        let wide = status_list(&model(), 1280, 20, &mut gs);
        let narrow = status_list(&model(), 200, 20, &mut gs);
        assert!(narrow.1.ctx.1 < wide.1.ctx.1);
        assert!(
            narrow.1.clock.0 + narrow.1.clock.1 <= 200 - PAD,
            "the clock still fits"
        );
        assert!(
            narrow.0.runs.len() < wide.0.runs.len(),
            "fewer context glyphs on the narrow bar"
        );
        assert_eq!(
            narrow.1.ctx_ink.0, narrow.1.ctx.0,
            "a truncated context starts at the span's left"
        );
        assert!(
            narrow.1.ctx_ink.1 <= narrow.1.ctx.1,
            "and ends inside it (the ellipsis counted)"
        );
        let (c, s) = status_list(&model(), 1, 20, &mut gs);
        assert_eq!(s.ctx.1, 0);
        assert!(matches!(c.ops[0], Op::Clear { .. }));
        assert!(status_list(&model(), 0, 20, &mut gs).0.ops.is_empty());
    }
}
