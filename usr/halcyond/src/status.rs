// status -- the status bar's rules (HALCYON.md 13.6 H-3d; HALCYON-VISUAL
// section 6): the pure half. One bar at the bottom of the screen, 20px,
// dark against the light theme -- the one piece of chrome that belongs to
// the system rather than to any pane. Four slots, left to right:
// workspaces (ONE filled indicator until H-4's layouts supply the list --
// the 2026-09-02 vote), the focused context (the focused leaf's name, its
// working directory, its running-or-last command; the last two known for
// the console only), the condition (the focused pane's recorded status, the
// SAME record the live tile keys -- the bar is the redundant channel), and
// the clock. The bin (`statusset`) owns the surface and the sources; every
// pixel decision is here, under host tests.

use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use libhalcyon::theme::{Argb, DAYLIGHT, METRICS};

use crate::chrome::NAME_PX;
use crate::raster::{GlyphSource, FACE_BODY, FACE_MONO};

/// The condition slot's state -- the focused pane's `status` file, section
/// 1.4's two states (sage / cinnabar) plus resting.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Condition {
    Idle,
    Ok,
    Err,
}

/// The pane's recorded status text (`resting|ok|err`) as a condition: only
/// `err` is cinnabar, only `ok` is sage; anything else is idle.
pub fn condition_for(status: &str) -> Condition {
    match status.trim() {
        "ok" => Condition::Ok,
        "err" => Condition::Err,
        _ => Condition::Idle,
    }
}

/// What the bar shows. `workspaces`/`active` are 1/0 until H-4.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct StatusModel {
    pub workspaces: u8,
    pub active: u8,
    /// The focused leaf's name (its tag's program); empty when nothing is
    /// focused.
    pub name: String,
    /// The focused console's working directory (OSC 7); empty otherwise.
    pub cwd: String,
    /// The focused console's running-or-last command; empty otherwise.
    pub cmd: String,
    pub condition: Condition,
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

/// Where each slot landed, in bar pixels (x, w) -- the witness reads these
/// off the bin's say line to know where to look.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Slots {
    pub ws: (i32, i32),
    pub ctx: (i32, i32),
    pub cond: (i32, i32),
    pub clock: (i32, i32),
}

/// The horizontal padding at the bar's ends and between slots.
const PAD: i32 = 8;
/// The workspace indicator's box (a filled square with the number).
const WS_BOX: i32 = 14;
/// The condition dot.
const DOT: i32 = 8;

struct Run {
    refs: Vec<GlyphRef>,
    width: i32,
}

fn shape(gs: &mut GlyphSource, face: u8, text: &str) -> Run {
    let mut refs: Vec<GlyphRef> = Vec::new();
    let mut width = 0;
    for ch in text.chars() {
        if let Some(g) = gs.glyph(face, NAME_PX, ch) {
            width += g.advance;
            refs.push(g);
        }
    }
    Run { refs, width }
}

/// The condition slot's colours: the dot, and its label.
pub fn condition_colors(c: Condition) -> (Argb, &'static str) {
    let d = &DAYLIGHT;
    match c {
        Condition::Idle => (d.status_idle, ""),
        Condition::Ok => (d.sage.key, "ok"),
        Condition::Err => (d.cinnabar.key, "err"),
    }
}

/// The bar's display list for a `w` x `h` surface, and where the slots
/// landed. Right to left: the clock, the condition; then the workspaces at
/// the left; the context takes what is left between them, truncated at its
/// right with an ellipsis when it does not fit (the slot that yields). A
/// zero-sized bar yields an empty list.
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
        .line_metrics(FACE_BODY, NAME_PX)
        .map(|mm| (mm.ascent, mm.descent))
        .unwrap_or((8, 2));
    let baseline = (hi - (asc + desc)) / 2 + asc;
    let gen = gs.gen();

    // The clock, right-aligned (monospace: literal).
    let mut clock = String::new();
    let _ = core::fmt::write(&mut clock, format_args!("{:02}:{:02}", m.hour, m.minute));
    let crun = shape(gs, FACE_MONO, &clock);
    let clock_x = wi - PAD - crun.width;
    if !crun.refs.is_empty() && clock_x > 0 {
        cart.push_glyphs(gen, clock_x, baseline, d.status_fg, &crun.refs);
    }
    slots.clock = (clock_x, crun.width);

    // The condition: a dot in the key colour + its label, left of the clock.
    let (dot_color, label) = condition_colors(m.condition);
    let lrun = shape(gs, FACE_BODY, label);
    let cond_w = DOT
        + if lrun.width > 0 {
            PAD / 2 + lrun.width
        } else {
            0
        };
    let cond_x = clock_x - PAD - cond_w;
    if cond_x > 0 {
        cart.ops.push(Op::Rect {
            x: cond_x,
            y: (hi - DOT) / 2,
            w: DOT as u32,
            h: DOT as u32,
            color: dot_color,
        });
        if !lrun.refs.is_empty() {
            cart.push_glyphs(
                gen,
                cond_x + DOT + PAD / 2,
                baseline,
                d.status_fg,
                &lrun.refs,
            );
        }
    }
    slots.cond = (cond_x, cond_w);

    // The workspaces: one indicator per workspace; the active one a filled
    // ember box with the number in the bar's own dark, the rest the number
    // in `status_idle` on the bar.
    let mut x = PAD;
    let ws_x = x;
    for i in 0..m.workspaces.max(1) {
        let mut num = String::new();
        let _ = core::fmt::write(&mut num, format_args!("{}", i + 1));
        let nrun = shape(gs, FACE_BODY, &num);
        let box_w = WS_BOX.max(nrun.width + 6);
        if i == m.active {
            cart.ops.push(Op::Rect {
                x,
                y: (hi - WS_BOX) / 2,
                w: box_w as u32,
                h: WS_BOX as u32,
                color: d.ember,
            });
            if !nrun.refs.is_empty() {
                cart.push_glyphs(
                    gen,
                    x + (box_w - nrun.width) / 2,
                    baseline,
                    d.status_bg,
                    &nrun.refs,
                );
            }
        } else if !nrun.refs.is_empty() {
            cart.push_glyphs(
                gen,
                x + (box_w - nrun.width) / 2,
                baseline,
                d.status_idle,
                &nrun.refs,
            );
        }
        x += box_w + PAD / 2;
    }
    slots.ws = (ws_x, x - PAD / 2 - ws_x);

    // The context, in what is left: the name proportional, the directory
    // and the command monospace islands (Daylight 7), truncated at the
    // right with an ellipsis.
    let ctx_x = x + PAD;
    let avail = cond_x - PAD - ctx_x;
    slots.ctx = (ctx_x, avail.max(0));
    if avail > 0 {
        let text = context_text(&m.name, &m.cwd, &m.cmd);
        let mono_from = m.name.trim().len(); // the name is proportional, the rest mono
        let ell = shape(gs, FACE_BODY, "\u{2026}");
        let mut runs: Vec<(u8, GlyphRef)> = Vec::new();
        let mut width = 0;
        let mut fits = true;
        let mut byte = 0;
        for ch in text.chars() {
            let face = if byte < mono_from {
                FACE_BODY
            } else {
                FACE_MONO
            };
            byte += ch.len_utf8();
            if let Some(g) = gs.glyph(face, NAME_PX, ch) {
                if width + g.advance > avail {
                    fits = false;
                    break;
                }
                width += g.advance;
                runs.push((face, g));
            }
        }
        if !fits {
            while width + ell.width > avail {
                match runs.pop() {
                    Some((_, g)) => width -= g.advance,
                    None => break,
                }
            }
            for g in ell.refs.iter() {
                runs.push((FACE_BODY, *g));
            }
        }
        // Emit as face-contiguous runs.
        let mut cx = ctx_x;
        let mut i = 0;
        while i < runs.len() {
            let face = runs[i].0;
            let mut j = i;
            let mut refs: Vec<GlyphRef> = Vec::new();
            let mut rw = 0;
            while j < runs.len() && runs[j].0 == face {
                refs.push(runs[j].1);
                rw += runs[j].1.advance;
                j += 1;
            }
            cart.push_glyphs(gen, cx, baseline, d.status_fg, &refs);
            cx += rw;
            i = j;
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
            hour: 14,
            minute: 22,
        }
    }

    #[test]
    fn condition_is_the_panes_two_states_plus_idle() {
        assert_eq!(condition_for("ok\n"), Condition::Ok);
        assert_eq!(condition_for("err"), Condition::Err);
        assert_eq!(condition_for("resting\n"), Condition::Idle);
        assert_eq!(condition_for(""), Condition::Idle);
        assert_eq!(
            condition_for("warning"),
            Condition::Idle,
            "warnings do not promote"
        );
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
            s.ws.0 == PAD && s.ws.1 >= WS_BOX,
            "the workspace indicator at the left: {:?}",
            s.ws
        );
        assert!(
            s.ctx.0 > s.ws.0 + s.ws.1,
            "the context after the workspaces"
        );
        assert!(s.cond.0 > s.ctx.0, "the condition after the context");
        assert!(
            s.clock.0 > s.cond.0 + s.cond.1,
            "the clock after the condition"
        );
        assert!(
            s.clock.0 + s.clock.1 <= 1280 - PAD,
            "the clock ends inside the right pad"
        );
        // The active indicator is an ember box; the condition dot is sage.
        assert!(
            c.ops.iter().any(|o| matches!(
                o,
                Op::Rect {
                    color: 0xFFE07840,
                    ..
                }
            )),
            "the ember box"
        );
        assert!(
            c.ops.iter().any(|o| matches!(
                o,
                Op::Rect {
                    color: 0xFF1E5844,
                    w: 8,
                    h: 8,
                    ..
                }
            )),
            "the sage dot"
        );
        let (_, e) = status_list(
            &StatusModel {
                condition: Condition::Err,
                ..model()
            },
            1280,
            20,
            &mut gs,
        );
        assert_eq!(
            e.clock, s.clock,
            "the clock does not move with the condition"
        );
        let (ci, _) = status_list(
            &StatusModel {
                condition: Condition::Idle,
                ..model()
            },
            1280,
            20,
            &mut gs,
        );
        assert!(
            ci.ops.iter().any(|o| matches!(
                o,
                Op::Rect {
                    color: 0xFF3A2E22,
                    w: 8,
                    h: 8,
                    ..
                }
            )),
            "the idle dot"
        );
    }

    #[test]
    fn a_narrow_bar_truncates_the_context_and_keeps_the_rest() {
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
        let (c, s) = status_list(&model(), 1, 20, &mut gs);
        assert_eq!(s.ctx.1, 0);
        assert!(matches!(c.ops[0], Op::Clear { .. }));
        assert!(status_list(&model(), 0, 20, &mut gs).0.ops.is_empty());
    }
}
