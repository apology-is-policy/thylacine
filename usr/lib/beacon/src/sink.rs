//! The per-tier realization API (BEACON.md 12.5; "binding in shape, not in
//! identifier"). Programs describe output once; the Sink realizes it at the
//! effective tier. The invariant the API enforces by construction: every
//! method writes the plain payload at EVERY tier and adds frames only at
//! Rich -- so the 12.8 P1 strip property holds for any program using it.
//!
//! Two recorded deviations from the 12.5 sketch, both at implementation:
//!   - zones are explicit `zone_open`/`zone_close`, not a scope guard: the
//!     shell's zones are NOT lexically scoped (the prompt zone opens in
//!     draw_prompt and closes in the accept arm, a different call).
//!   - `em`/`obj` at the Cells tier are payload-only: the cells tier's look
//!     is the bins' existing box+SGR language (boxd/color/palette, used
//!     directly); object identity is a Rich concept. SGR never appears
//!     inside Rich beacon-structured output (the renderer stylesheet owns
//!     typography there).

use crate::wire::{self, Op, VALUE_MAX};
use crate::Tier;
use alloc::string::String;
use alloc::vec::Vec;

/// The byte sink programs write through (the no_std write shim; libthyla-rs
/// stdout adapts to it trivially).
pub trait Out {
    fn out(&mut self, bytes: &[u8]);
}

impl Out for Vec<u8> {
    fn out(&mut self, bytes: &[u8]) {
        self.extend_from_slice(bytes);
    }
}

/// The shell's transcript zones (12.2; emitted by the SHELL only).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Zone {
    Prompt,
    Command,
    Output,
}

impl Zone {
    fn arg(self) -> &'static str {
        match self {
            Zone::Prompt => "prompt",
            Zone::Command => "command",
            Zone::Output => "output",
        }
    }
}

/// Emphasis by class, never by face (12.2).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Em {
    Emph,
    Strong,
    Dim,
    Code,
}

impl Em {
    fn arg(self) -> &'static str {
        match self {
            Em::Emph => "emph",
            Em::Strong => "strong",
            Em::Dim => "dim",
            Em::Code => "code",
        }
    }
}

/// A presentation: this run of text presents an object of a type,
/// canonically named by its ref (a cleaned ABSOLUTE 9P path for `Path`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum ObjType {
    Path,
    Pid,
    Url,
    Commit,
    User,
}

impl ObjType {
    fn arg(self) -> &'static str {
        match self {
            ObjType::Path => "path",
            ObjType::Pid => "pid",
            ObjType::Url => "url",
            ObjType::Commit => "commit",
            ObjType::User => "user",
        }
    }
}

/// The emitting half. Construct with the effective tier (the
/// `crate::effective_tier` gate's answer) and write output through it.
pub struct Sink<'a> {
    out: &'a mut dyn Out,
    tier: Tier,
    buf: Vec<u8>,
}

impl<'a> Sink<'a> {
    pub fn new(out: &'a mut dyn Out, tier: Tier) -> Sink<'a> {
        Sink {
            out,
            tier,
            buf: Vec::new(),
        }
    }

    pub fn tier(&self) -> Tier {
        self.tier
    }

    fn flush_frame(&mut self) {
        if !self.buf.is_empty() {
            let b = core::mem::take(&mut self.buf);
            self.out.out(&b);
        }
    }

    /// Plain payload, every tier.
    pub fn text(&mut self, s: &str) {
        self.out.out(s.as_bytes());
    }

    pub fn em(&mut self, class: Em, s: &str) {
        if self.tier == Tier::Rich {
            wire::open(&mut self.buf, Op::Em, &[("class", class.arg())]);
            self.flush_frame();
        }
        self.text(s);
        if self.tier == Tier::Rich {
            wire::close(&mut self.buf, Op::Em);
            self.flush_frame();
        }
    }

    /// A presentation wrapping the shown text. A ref the emitter cannot fit
    /// (over VALUE_MAX) emits NO frame -- plain text only (12.2's obj rule):
    /// a truncated ref would be a WRONG ref, and a wrong ref is a lie the
    /// verb menu would then act on.
    pub fn obj(&mut self, ty: ObjType, obj_ref: &str, shown: &str) {
        let frame = self.tier == Tier::Rich && obj_ref.len() <= VALUE_MAX;
        if frame {
            wire::open(
                &mut self.buf,
                Op::Obj,
                &[("type", ty.arg()), ("ref", obj_ref)],
            );
            self.flush_frame();
        }
        self.text(shown);
        if frame {
            wire::close(&mut self.buf, Op::Obj);
            self.flush_frame();
        }
    }

    /// A heading wrapping its text.
    pub fn hdr(&mut self, level: u8, s: &str) {
        let lvl = match level {
            1 => "1",
            2 => "2",
            _ => "3",
        };
        if self.tier == Tier::Rich {
            wire::open(&mut self.buf, Op::Hdr, &[("level", lvl)]);
            self.flush_frame();
        }
        self.text(s);
        if self.tier == Tier::Rich {
            wire::close(&mut self.buf, Op::Hdr);
            self.flush_frame();
        }
    }

    /// The semantic separator mark. The caller emits its own visible rule
    /// line as `text()` (the plain realization); this is only the annotation.
    pub fn rule(&mut self) {
        if self.tier == Tier::Rich {
            wire::point(&mut self.buf, Op::Rule, &[]);
            self.flush_frame();
        }
    }

    /// Transcript structure (the shell only; 12.6). Explicitly non-scoped.
    pub fn zone_open(&mut self, z: Zone) {
        if self.tier == Tier::Rich {
            wire::open(&mut self.buf, Op::Zone, &[("k", z.arg())]);
            self.flush_frame();
        }
    }

    pub fn zone_close(&mut self, z: Zone) {
        let _ = z; // one close form; the arg documents intent at call sites
        if self.tier == Tier::Rich {
            wire::close(&mut self.buf, Op::Zone);
            self.flush_frame();
        }
    }

    /// The command-completion mark (between the output close and the next
    /// prompt open).
    pub fn mark_exit(&mut self, code: i64) {
        if self.tier == Tier::Rich {
            let mut num = [0u8; 24];
            let s = fmt_i64(&mut num, code);
            wire::point(&mut self.buf, Op::Mark, &[("k", "exit"), ("code", s)]);
            self.flush_frame();
        }
    }
}

fn fmt_i64<'b>(buf: &'b mut [u8; 24], v: i64) -> &'b str {
    let mut i = buf.len();
    let neg = v < 0;
    let mut u = v.unsigned_abs();
    loop {
        i -= 1;
        buf[i] = b'0' + (u % 10) as u8;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    if neg {
        i -= 1;
        buf[i] = b'-';
    }
    core::str::from_utf8(&buf[i..]).unwrap()
}

// ---------------------------------------------------------------------------
// Table
// ---------------------------------------------------------------------------

/// One table cell: the shown text, optionally presenting an object.
#[derive(Clone, Debug, Default)]
pub struct Cell {
    pub text: String,
    pub obj: Option<(ObjType, String)>,
}

impl Cell {
    pub fn plain(text: &str) -> Cell {
        Cell {
            text: String::from(text),
            obj: None,
        }
    }

    pub fn obj(ty: ObjType, obj_ref: &str, text: &str) -> Cell {
        Cell {
            text: String::from(text),
            obj: Some((ty, String::from(obj_ref))),
        }
    }
}

/// A table described once, realized per tier (12.2's `table`/`row`/`cell`).
///
/// The plain realization (every tier's payload): columns aligned per the
/// spec (`l`/`r`/`c`, one char per column), two spaces between columns, one
/// row per line. Rich adds the frames AROUND that exact payload -- padding
/// stays OUTSIDE cell frames, so strip() recovers the aligned plain table
/// byte-exactly. The Cells box realization is each emitter's concern at its
/// conversion (the bins keep their bespoke boxd layouts until then; the
/// generic realization here is plain-aligned at Cells too).
pub struct Table {
    cols: Vec<u8>,
    hdr: bool,
    rows: Vec<Vec<Cell>>,
}

impl Table {
    /// `cols`: one alignment char per column -- `l`, `r`, or `c`.
    pub fn new(cols: &str) -> Table {
        Table {
            cols: cols.bytes().collect(),
            hdr: false,
            rows: Vec::new(),
        }
    }

    /// The first pushed row is a header row.
    pub fn hdr(mut self) -> Table {
        self.hdr = true;
        self
    }

    pub fn push_row(&mut self, cells: Vec<Cell>) {
        self.rows.push(cells);
    }

    fn width(s: &str) -> usize {
        s.chars().count()
    }

    pub fn realize(&self, s: &mut Sink) {
        // Column widths from content (visible chars, the boxd discipline).
        let ncols = self.cols.len();
        let mut w: Vec<usize> = Vec::new();
        w.resize(ncols, 0);
        for row in &self.rows {
            for (i, c) in row.iter().enumerate().take(ncols) {
                let cw = Self::width(&c.text);
                if cw > w[i] {
                    w[i] = cw;
                }
            }
        }

        let rich = s.tier() == Tier::Rich;
        if rich {
            let colspec = core::str::from_utf8(&self.cols).unwrap_or("");
            wire::open(
                &mut s.buf,
                Op::Table,
                &[("cols", colspec), ("hdr", if self.hdr { "1" } else { "0" })],
            );
            s.flush_frame();
        }
        for row in &self.rows {
            if rich {
                wire::open(&mut s.buf, Op::Row, &[]);
                s.flush_frame();
            }
            for (i, c) in row.iter().enumerate().take(ncols) {
                let align = self.cols[i];
                let pad = w[i].saturating_sub(Self::width(&c.text));
                let (before, after) = match align {
                    b'r' => (pad, 0),
                    b'c' => (pad / 2, pad - pad / 2),
                    _ => (0, pad),
                };
                if i > 0 {
                    s.text("  ");
                }
                // Padding is payload OUTSIDE the cell frame; the frame wraps
                // only the cell's own text (the strip identity).
                spaces(s, before);
                if rich {
                    wire::open(&mut s.buf, Op::Cell, &[]);
                    s.flush_frame();
                }
                match &c.obj {
                    Some((ty, r)) => s.obj(*ty, r, &c.text),
                    None => s.text(&c.text),
                }
                if rich {
                    wire::close(&mut s.buf, Op::Cell);
                    s.flush_frame();
                }
                // The last column's after-pad would be trailing whitespace;
                // drop it (alignment only needs it between columns).
                if i + 1 < row.len().min(ncols) {
                    spaces(s, after);
                }
            }
            if rich {
                wire::close(&mut s.buf, Op::Row);
                s.flush_frame();
            }
            s.text("\n");
        }
        if rich {
            wire::close(&mut s.buf, Op::Table);
            s.flush_frame();
        }
    }
}

fn spaces(s: &mut Sink, n: usize) {
    for _ in 0..n {
        s.text(" ");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::wire::strip;
    use alloc::vec;

    fn realized(tier: Tier, f: impl Fn(&mut Sink)) -> Vec<u8> {
        let mut out: Vec<u8> = Vec::new();
        {
            let mut s = Sink::new(&mut out, tier);
            f(&mut s);
        }
        out
    }

    #[test]
    fn p1_strip_property_over_the_sink() {
        // The property every emitter is held to, driven through every method.
        let drive = |s: &mut Sink| {
            s.zone_open(Zone::Prompt);
            s.text("$ ");
            s.zone_close(Zone::Prompt);
            s.zone_open(Zone::Output);
            s.em(Em::Strong, "match");
            s.text(" in ");
            s.obj(ObjType::Path, "/etc/hosts", "hosts");
            s.text("\n");
            s.hdr(1, "HEAD");
            s.text("\n----\n");
            s.rule();
            s.zone_close(Zone::Output);
            s.mark_exit(0);
        };
        let rich = realized(Tier::Rich, drive);
        let none = realized(Tier::None, drive);
        let cells = realized(Tier::Cells, drive);
        assert_eq!(strip(&rich), none, "strip(rich) == none");
        assert_eq!(cells, none, "sink methods add nothing at cells");
        assert_eq!(none, b"$ match in hosts\nHEAD\n----\n".to_vec());
    }

    #[test]
    fn obj_over_value_max_degrades_to_plain() {
        let long_ref = "x".repeat(VALUE_MAX + 1);
        let rich = realized(Tier::Rich, |s| {
            s.obj(ObjType::Path, &long_ref, "shown")
        });
        assert_eq!(rich, b"shown".to_vec()); // no frame at all
    }

    #[test]
    fn table_p1_and_alignment() {
        let mut t = Table::new("lrl").hdr();
        t.push_row(vec![
            Cell::plain("NAME"),
            Cell::plain("SIZE"),
            Cell::plain("KIND"),
        ]);
        t.push_row(vec![
            Cell::obj(ObjType::Path, "/a/long", "long"),
            Cell::plain("12345"),
            Cell::plain("file"),
        ]);
        t.push_row(vec![
            Cell::plain("x"),
            Cell::plain("7"),
            Cell::plain("dir"),
        ]);
        let rich = realized(Tier::Rich, |s| t.realize(s));
        let none = realized(Tier::None, |s| t.realize(s));
        assert_eq!(strip(&rich), none);
        let want = "NAME   SIZE  KIND\nlong  12345  file\nx         7  dir\n";
        assert_eq!(none, want.as_bytes().to_vec());
    }

    #[test]
    fn table_rich_structure_parses() {
        let mut t = Table::new("ll");
        t.push_row(vec![Cell::plain("a"), Cell::plain("b")]);
        let rich = realized(Tier::Rich, |s| t.realize(s));
        let evs = crate::wire::parse(&rich);
        // table > row > 2x(cell text) with padding text between, all closed.
        assert!(matches!(evs[0], crate::wire::Event::Open(Op::Table, _)));
        assert!(matches!(evs.last(), Some(crate::wire::Event::Close(Op::Table))));
    }
}
