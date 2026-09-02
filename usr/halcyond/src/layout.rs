// Layout: the pure function from a frozen Block + a width + the stylesheet
// to positioned line boxes (HALCYON.md section 13.3 -- store semantics,
// derive pixels; reflow-on-resize is re-running this function, so it must
// be deterministic and width-total). Rendering is then a trivial
// translation of laid lines into cartoon ops.
//
// The metrics-mixing rule (13.5, plus the recorded all-mono refinement):
// a line's box uses the BODY line height whenever any proportional seg is
// present -- mono islands sit ON the body baseline and may not stretch the
// box (a tall cell glyph may clip; deliberate) -- while a line composed
// entirely of mono cells uses the mono cell metrics, so a foreign
// terminal block reads exactly as a terminal would (section 4's promise).
//
// The face rule, per cell (the MVP realization of sections 3-4):
//   em class=code  -> MONO;
//   annotated (obj / em / hdr / table cell) -> BODY (bold under
//     ATTR_BOLD / em strong / hdr);
//   un-annotated   -> MONO ("plain output renders in monospace exactly as
//     a terminal would" -- SGR color alone is not annotation).

use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use vt::ATTR_BOLD;

use crate::raster::{GlyphSource, FACE_BODY, FACE_BODY_BOLD, FACE_MONO};
use crate::transcript::{Block, BlockKind, Item, Style, TCell, EM_CODE, EM_DIM, EM_STRONG};

/// The stylesheet: the paper-light theme's numbers (section 3 -- dark ink
/// in full daylight). Colors are ARGB like everything in the weave.
#[derive(Clone, Copy)]
pub struct Sheet {
    pub ground: u32, // Daylight surface
    pub ink: u32,    // Daylight fg
    pub dim: u32,    // Daylight fg_muted (secondary text, the Normal-mode caret)
    pub accent: u32, // Daylight ember (the Insert caret / turnstile / running mark)
    pub obj: u32,    // Daylight syntax.slate (presentation refs -- section 1.5)
    pub err: u32,    // Daylight cinnabar (exit failure)
    pub ok: u32,     // Daylight fen (exit success; reserved -- H-2 is failure-only)
    pub rule: u32,   // Daylight border (table rules)
    pub sel_bg: u32,
    pub body_px: f32,
    pub pad_x: i32,
    pub block_gap: i32,
    pub table_col_gap: i32,
    /// Bumps on any sheet change; part of the layout-cache key.
    pub gen: u32,
}

/// The paper-light transcript sheet, built from the Daylight visual scripture
/// (docs/HALCYON-VISUAL.md via libhalcyon::theme -- the single token source the
/// H-3 split names). Replaces the H-2 approximation seeded from vt::THEMES[1]:
/// the transcript now matches the chrome that H-3a's compositor bevels + tag
/// bar draw around it, because both derive from DAYLIGHT.
pub fn daylight_sheet() -> Sheet {
    let d = &libhalcyon::theme::DAYLIGHT;
    Sheet {
        ground: d.surface,
        ink: d.fg,
        dim: d.fg_muted,
        accent: d.ember,
        obj: d.syntax.slate,
        err: d.cinnabar.key,
        ok: d.syntax.fen,
        rule: d.border,
        // A parchment-compatible selection band: a warm step between surface
        // and header (Daylight has no transcript-selection token; this sits in
        // the same family, darker than surface, lighter than header).
        sel_bg: 0xFFDF_D6C7,
        body_px: 16.0,
        pad_x: 8,
        block_gap: 6,
        table_col_gap: 16,
        gen: 0,
    }
}

/// One positioned run: glyphs sharing a face/color/background, with the
/// per-glyph pen x recorded for hit-testing (xs[i] is glyph i's pen; the
/// run ends at `x_end`).
pub struct Seg {
    pub x: i32,
    pub x_end: i32,
    pub color: u32,
    pub bg: Option<u32>,
    pub face: u8,
    pub px: f32,
    pub refs: Vec<GlyphRef>,
    pub xs: Vec<i32>,
    /// Source addressing for selection: the item index in the block and
    /// the starting cell column this seg covers (columns advance one per
    /// glyph). Table content carries the table's item index with col 0
    /// (table selection is a recorded later).
    pub src_item: usize,
    pub src_col: usize,
    /// The obj-table index+1 covering this seg (0 = none) -- the
    /// presentation hit target.
    pub obj: u16,
}

pub struct LaidLine {
    pub y: i32,
    pub h: i32,
    pub baseline: i32,
    pub segs: Vec<Seg>,
    /// Source addressing for selection: the block item this visual line
    /// came from (usize::MAX = layout furniture), and the table row when
    /// the item is a table (usize::MAX for a plain line). Wrapped lines
    /// share their item's address -- selection is row-wise over items.
    pub src_item: usize,
    pub src_row: usize,
}

/// A rectangle the block wants painted UNDER its text (SGR backgrounds,
/// table rules, the exit-badge pill).
pub struct RectSpec {
    pub x: i32,
    pub y: i32,
    pub w: u32,
    pub h: u32,
    pub color: u32,
}

pub struct LaidBlock {
    pub height: i32,
    pub lines: Vec<LaidLine>,
    pub rects: Vec<RectSpec>,
}

fn face_for(st: &Style, in_table: bool) -> (u8, bool) {
    if st.em == EM_CODE {
        return (FACE_MONO, false);
    }
    let annotated = st.obj != 0 || st.em != 0 || st.hdr != 0 || in_table;
    if !annotated {
        return (FACE_MONO, false);
    }
    let bold = st.attrs & ATTR_BOLD != 0 || st.em == EM_STRONG || st.hdr != 0;
    (if bold { FACE_BODY_BOLD } else { FACE_BODY }, bold)
}

fn color_for(st: &Style, sheet: &Sheet) -> u32 {
    if st.em == EM_DIM && st.fg == sheet.ink {
        return sheet.dim;
    }
    if st.obj != 0 && st.fg == sheet.ink {
        // Presentation refs take the object-reference colour (Daylight slate,
        // section 1.5), NOT the ember accent -- the accent is the caret/turnstile.
        return sheet.obj;
    }
    st.fg
}

fn px_for(st: &Style, sheet: &Sheet) -> f32 {
    match st.hdr {
        1 => sheet.body_px * 1.4,
        2 => sheet.body_px * 1.2,
        3 => sheet.body_px * 1.05,
        _ => sheet.body_px,
    }
}

/// Group a cell row into style-run spans (adjacent same-style cells).
fn runs_of(cells: &[TCell]) -> Vec<(usize, usize, u16)> {
    let mut out = Vec::new();
    let mut i = 0;
    while i < cells.len() {
        let s = cells[i].style;
        let start = i;
        while i < cells.len() && cells[i].style == s {
            i += 1;
        }
        out.push((start, i, s));
    }
    out
}

struct LineBuilder<'a> {
    sheet: &'a Sheet,
    width: i32,
    lines: Vec<LaidLine>,
    rects: Vec<RectSpec>,
    y: i32,
    // The line under construction.
    segs: Vec<Seg>,
    pen_x: i32,
    any_body: bool,
    max_asc: i32,
    max_desc: i32,
}

impl<'a> LineBuilder<'a> {
    fn new(sheet: &'a Sheet, width: i32) -> LineBuilder<'a> {
        LineBuilder {
            sheet,
            width,
            lines: Vec::new(),
            rects: Vec::new(),
            y: 0,
            segs: Vec::new(),
            pen_x: sheet.pad_x,
            any_body: false,
            max_asc: 0,
            max_desc: 0,
        }
    }

    fn note_metrics(&mut self, gs: &GlyphSource, face: u8, px: f32) {
        if face != FACE_MONO {
            self.any_body = true;
        }
        if let Some(lm) = gs.line_metrics(face, px) {
            if lm.ascent > self.max_asc {
                self.max_asc = lm.ascent;
            }
            if lm.descent > self.max_desc {
                self.max_desc = lm.descent;
            }
        }
    }

    /// Close the current visual line (13.5: the body metrics own a mixed
    /// line; an all-mono line keeps the exact cell box).
    fn break_line(&mut self, gs: &GlyphSource) {
        let (asc, desc) = if self.segs.is_empty() {
            // An empty line still occupies a body line box.
            let lm = gs.line_metrics(FACE_MONO, self.sheet.body_px);
            match lm {
                Some(m) => (m.ascent, m.descent),
                None => (12, 4),
            }
        } else if self.any_body {
            // 13.5 verbatim: the body metrics OWN a mixed line; mono
            // islands sit on the body baseline and may not stretch the
            // box (a taller cell glyph clips -- deliberate).
            let lm = gs.line_metrics(FACE_BODY, self.sheet.body_px);
            match lm {
                Some(m) => (m.ascent, m.descent),
                None => (self.max_asc, self.max_desc),
            }
        } else {
            (self.max_asc, self.max_desc)
        };
        let h = asc + desc;
        let segs = core::mem::take(&mut self.segs);
        self.lines.push(LaidLine { y: self.y, h, baseline: self.y + asc, segs, src_item: usize::MAX, src_row: usize::MAX });
        self.y += h;
        self.pen_x = self.sheet.pad_x;
        self.any_body = false;
        self.max_asc = 0;
        self.max_desc = 0;
    }

    /// Lay one styled span, wrapping at the right edge (break at the last
    /// space on the line when one exists, else hard-break).
    #[allow(clippy::too_many_arguments)]
    fn lay_span(
        &mut self,
        gs: &mut GlyphSource,
        cells: &[TCell],
        st: &Style,
        in_table: bool,
        src_item: usize,
        src_col: usize,
        bg: Option<u32>,
    ) {
        let (face, _) = face_for(st, in_table);
        let px = px_for(st, self.sheet);
        let color = color_for(st, self.sheet);
        self.note_metrics(gs, face, px);
        let mut seg = Seg {
            x: self.pen_x,
            x_end: self.pen_x,
            color,
            bg,
            face,
            px,
            refs: Vec::new(),
            xs: Vec::new(),
            src_item,
            src_col,
            obj: st.obj,
        };
        let mut col = src_col;
        let mut last_space: Option<(usize, usize)> = None; // (refs idx AFTER the space, col after)
        let mut i = 0;
        while i < cells.len() {
            let ch = cells[i].ch;
            let Some(mut gr) = gs.glyph(face, px, ch) else {
                i += 1;
                col += 1;
                continue;
            };
            if face != FACE_MONO && i + 1 < cells.len() {
                gr.advance += gs.kern(face, px, ch, cells[i + 1].ch);
            }
            if self.pen_x + gr.advance > self.width - self.sheet.pad_x
                && !seg.refs.is_empty()
            {
                // Wrap: prefer the last space boundary inside this seg.
                if let Some((cut, cut_col)) = last_space {
                    if cut > 0 && cut < seg.refs.len() {
                        let spill_refs: Vec<GlyphRef> = seg.refs.split_off(cut);
                        seg.xs.truncate(cut);
                        seg.x_end = seg.xs.last().copied().unwrap_or(seg.x)
                            + seg.refs.last().map(|r| r.advance).unwrap_or(0);
                        let color2 = seg.color;
                        let bg2 = seg.bg;
                        let face2 = seg.face;
                        let px2 = seg.px;
                        let obj2 = seg.obj;
                        self.segs.push(seg);
                        self.break_line(gs);
                        self.note_metrics(gs, face2, px2);
                        seg = Seg {
                            x: self.pen_x,
                            x_end: self.pen_x,
                            color: color2,
                            bg: bg2,
                            face: face2,
                            px: px2,
                            refs: Vec::new(),
                            xs: Vec::new(),
                            src_item,
                            src_col: cut_col,
                            obj: obj2,
                        };
                        // Re-lay the spilled glyphs at the new line start.
                        for r in spill_refs {
                            seg.xs.push(self.pen_x);
                            self.pen_x += r.advance;
                            seg.refs.push(r);
                        }
                        seg.x_end = self.pen_x;
                        last_space = None;
                        // fall through to place the current glyph
                    } else {
                        let color2 = seg.color;
                        let bg2 = seg.bg;
                        let face2 = seg.face;
                        let px2 = seg.px;
                        let obj2 = seg.obj;
                        seg.x_end = self.pen_x;
                        self.segs.push(seg);
                        self.break_line(gs);
                        self.note_metrics(gs, face2, px2);
                        seg = Seg {
                            x: self.pen_x,
                            x_end: self.pen_x,
                            color: color2,
                            bg: bg2,
                            face: face2,
                            px: px2,
                            refs: Vec::new(),
                            xs: Vec::new(),
                            src_item,
                            src_col: col,
                            obj: obj2,
                        };
                        last_space = None;
                    }
                } else {
                    let color2 = seg.color;
                    let bg2 = seg.bg;
                    let face2 = seg.face;
                    let px2 = seg.px;
                    let obj2 = seg.obj;
                    seg.x_end = self.pen_x;
                    self.segs.push(seg);
                    self.break_line(gs);
                    self.note_metrics(gs, face2, px2);
                    seg = Seg {
                        x: self.pen_x,
                        x_end: self.pen_x,
                        color: color2,
                        bg: bg2,
                        face: face2,
                        px: px2,
                        refs: Vec::new(),
                        xs: Vec::new(),
                        src_item,
                        src_col: col,
                        obj: obj2,
                    };
                    last_space = None;
                }
            }
            seg.xs.push(self.pen_x);
            self.pen_x += gr.advance;
            seg.refs.push(gr);
            if ch == ' ' {
                last_space = Some((seg.refs.len(), col + 1));
            }
            i += 1;
            col += 1;
        }
        seg.x_end = self.pen_x;
        if !seg.refs.is_empty() {
            // The SGR-background rect under this seg's extent.
            self.segs.push(seg);
        }
    }
}

/// Lay a frozen (or the open) block at `width`. Pure in its inputs modulo
/// the glyph cache (rasterize-on-miss mutates `gs`; the RESULT is width-
/// and content-deterministic either way -- the property the reflow E2E
/// pins).
pub fn layout_block(b: &Block, width: i32, sheet: &Sheet, gs: &mut GlyphSource) -> LaidBlock {
    let mut lb = LineBuilder::new(sheet, width.max(2 * sheet.pad_x + 8));
    for (item_idx, item) in b.items.iter().enumerate() {
        let lines_before = lb.lines.len();
        match item {
            Item::Line(line) => {
                for (s, e, sid) in runs_of(&line.cells) {
                    let st = b.styles[sid as usize];
                    let bg = if st.bg != sheet.ground && st.bg != libhalcyon::theme::DAYLIGHT.surface {
                        Some(st.bg)
                    } else {
                        None
                    };
                    lb.lay_span(gs, &line.cells[s..e], &st, false, item_idx, s, bg);
                }
                lb.break_line(gs);
            }
            Item::Table(t) => {
                lay_table(&mut lb, t, b, item_idx, sheet, gs);
            }
            Item::Rule => {
                let y = lb.y + 3;
                lb.rects.push(RectSpec {
                    x: sheet.pad_x,
                    y,
                    w: (lb.width - 2 * sheet.pad_x).max(0) as u32,
                    h: 1,
                    color: sheet.rule,
                });
                lb.y += 7;
            }
        }
        // Stamp the item's visual lines with their source address (tables
        // stamped per-row inside lay_table already carry src_row).
        for l in lb.lines[lines_before..].iter_mut() {
            if l.src_item == usize::MAX {
                l.src_item = item_idx;
            }
        }
    }
    // The exit badge: only a FAILED command earns ink (section 4's exit
    // badge; success is silence).
    if let Some(code) = b.exit {
        if code != 0 {
            lay_exit_badge(&mut lb, code, sheet, gs);
        }
    }
    // SGR background rects derive from the laid segs (under the text).
    let mut rects = core::mem::take(&mut lb.rects);
    for line in lb.lines.iter() {
        for seg in line.segs.iter() {
            if let Some(bg) = seg.bg {
                rects.push(RectSpec {
                    x: seg.x,
                    y: line.y,
                    w: (seg.x_end - seg.x).max(0) as u32,
                    h: line.h as u32,
                    color: bg,
                });
            }
        }
    }
    LaidBlock { height: lb.y, lines: lb.lines, rects }
}

fn lay_table(
    lb: &mut LineBuilder,
    t: &crate::transcript::TableModel,
    b: &Block,
    item_idx: usize,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) {
    // Measure: each cell's natural width at its style.
    let ncols = t.rows.iter().map(|r| r.len()).max().unwrap_or(0).max(t.cols.len());
    if ncols == 0 {
        return;
    }
    let mut col_w = alloc::vec![0i32; ncols];
    let mut cellw: Vec<Vec<i32>> = Vec::new();
    for row in t.rows.iter() {
        let mut ws = Vec::new();
        for (ci, cell) in row.iter().enumerate() {
            let mut w = 0i32;
            for (s, e, sid) in runs_of(cell) {
                let st = b.styles[sid as usize];
                let (face, _) = face_for(&st, true);
                let px = px_for(&st, sheet);
                for c in cell[s..e].iter() {
                    if let Some(gr) = gs.glyph(face, px, c.ch) {
                        w += gr.advance;
                    }
                }
            }
            if ci < ncols && w > col_w[ci] {
                col_w[ci] = w;
            }
            ws.push(w);
        }
        cellw.push(ws);
    }
    // Column x origins.
    let mut col_x = alloc::vec![0i32; ncols];
    let mut x = sheet.pad_x;
    for c in 0..ncols {
        col_x[c] = x;
        x = x.saturating_add(col_w[c].saturating_add(sheet.table_col_gap));
    }
    // Lay rows: one visual line each (table cells are single-line by
    // construction -- the transcript capture maps controls to spaces).
    for (ri, row) in t.rows.iter().enumerate() {
        for (ci, cell) in row.iter().enumerate() {
            if ci >= ncols || cell.is_empty() {
                continue;
            }
            let align = t.cols.get(ci).copied().unwrap_or(b'l');
            let w = cellw[ri][ci];
            let x0 = match align {
                b'r' => col_x[ci] + col_w[ci] - w,
                b'c' => col_x[ci] + (col_w[ci] - w) / 2,
                _ => col_x[ci],
            };
            lb.pen_x = x0;
            for (s, e, sid) in runs_of(cell) {
                let mut st = b.styles[sid as usize];
                if t.hdr && ri == 0 {
                    st.attrs |= ATTR_BOLD;
                }
                // NOTE: table spans never wrap (the cell was measured);
                // width is temporarily unbounded for the span.
                let saved_w = lb.width;
                lb.width = i32::MAX / 2;
                lb.lay_span(gs, &cell[s..e], &st, true, item_idx, 0, None);
                lb.width = saved_w;
            }
        }
        lb.break_line(gs);
        if let Some(last) = lb.lines.last_mut() {
            last.src_item = item_idx;
            last.src_row = ri;
        }
        if t.hdr && ri == 0 {
            // The header rule.
            let y = lb.y;
            lb.rects.push(RectSpec {
                x: sheet.pad_x,
                y,
                w: (x - sheet.table_col_gap - sheet.pad_x).max(0) as u32,
                h: 1,
                color: sheet.rule,
            });
            lb.y += 3;
        }
    }
}

fn lay_exit_badge(lb: &mut LineBuilder, code: i64, sheet: &Sheet, gs: &mut GlyphSource) {
    let mut text = alloc::string::String::new();
    text.push_str("exit ");
    // Tiny itoa (i64, no_std).
    let mut buf = [0u8; 20];
    // The magnitude is taken UNSIGNED: `-code` panics on i64::MIN (no
    // positive i64), and `code` is an untrusted `mark k=exit` frame value.
    let neg = code < 0;
    let mut n = code.unsigned_abs();
    let mut i = buf.len();
    loop {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
        if n == 0 {
            break;
        }
    }
    if neg {
        i -= 1;
        buf[i] = b'-';
    }
    for &b in &buf[i..] {
        text.push(b as char);
    }
    let px = sheet.body_px * 0.9;
    let mut w = 0i32;
    for ch in text.chars() {
        if let Some(gr) = gs.glyph(FACE_BODY, px, ch) {
            w += gr.advance;
        }
    }
    lb.pen_x = (lb.width - sheet.pad_x - w).max(sheet.pad_x);
    let st = Style { fg: sheet.err, bg: sheet.ground, attrs: 0, em: 0, obj: 0, hdr: 0 };
    let cells: Vec<TCell> = text.chars().map(|ch| TCell { ch, style: 0 }).collect();
    // A synthetic span: bypass the block style table (the badge is layout
    // furniture, not content -- selection never addresses it).
    let saved = lb.width;
    lb.width = i32::MAX / 2;
    let mut seg_start_x = lb.pen_x;
    let mut seg = Seg {
        x: seg_start_x,
        x_end: seg_start_x,
        color: st.fg,
        bg: None,
        face: FACE_BODY,
        px,
        refs: Vec::new(),
        xs: Vec::new(),
        src_item: usize::MAX,
        src_col: 0,
        obj: 0,
    };
    lb.note_metrics(gs, FACE_BODY, px);
    for c in cells.iter() {
        if let Some(gr) = gs.glyph(FACE_BODY, px, c.ch) {
            seg.xs.push(seg_start_x);
            seg_start_x += gr.advance;
            seg.refs.push(gr);
        }
    }
    seg.x_end = seg_start_x;
    lb.segs.push(seg);
    lb.width = saved;
    lb.break_line(gs);
}

/// Lay the OPEN block's un-frozen tail line (the prompt under the
/// cursor). Cells reference the open block's style table, so both come
/// in; the temp block is layout furniture (never cached, id-less).
pub fn layout_pending(
    cells: &[TCell],
    styles: &[Style],
    width: i32,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) -> LaidBlock {
    let b = Block {
        id: u64::MAX,
        kind: BlockKind::Foreign,
        continuation: false,
        exit: None,
        cmd: None,
        items: alloc::vec![Item::Line(crate::transcript::Line { cells: cells.to_vec() })],
        styles: styles.to_vec(),
        objs: Vec::new(),
        cost: 0,
    };
    layout_block(&b, width, sheet, gs)
}

/// The cursor's pixel position on a laid single-item block: the x of
/// column `col` (or the end of the content when col is past it) plus the
/// line's y/h. Columns count glyphs across the laid segs in order.
pub fn cursor_pos(laid: &LaidBlock, col: usize, sheet: &Sheet) -> (i32, i32, i32) {
    let mut remaining = col;
    for line in laid.lines.iter() {
        for seg in line.segs.iter() {
            if remaining < seg.refs.len() {
                return (seg.xs[remaining], line.y, line.h);
            }
            remaining -= seg.refs.len();
        }
        // Column beyond this line's content: if it is the LAST line, the
        // cursor sits at the content end; otherwise spill to the next.
    }
    if let Some(last) = laid.lines.last() {
        let x = last.segs.last().map(|s| s.x_end).unwrap_or(sheet.pad_x);
        return (x, last.y, last.h);
    }
    (sheet.pad_x, 0, 16)
}

/// Emit a laid block into the cartoon at (0, y0): background rects first,
/// then glyph runs (paint order is the op order).
pub fn render_block(cart: &mut Cartoon, laid: &LaidBlock, y0: i32, gs: &GlyphSource) {
    for r in laid.rects.iter() {
        cart.ops.push(Op::Rect { x: r.x, y: y0 + r.y, w: r.w, h: r.h, color: r.color });
    }
    let gen = gs.gen();
    for line in laid.lines.iter() {
        for seg in line.segs.iter() {
            if seg.refs.is_empty() {
                continue;
            }
            // `baseline` is block-absolute (line.y + ascent).
            cart.push_glyphs(gen, seg.x, y0 + line.baseline, seg.color, &seg.refs);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transcript::{Transcript, DEFAULT_MAX_BLOCKS, DEFAULT_MAX_COST, DEFAULT_MAX_LINES_PER_BLOCK};
    use alloc::vec::Vec;
    use beacon::wire::{self, Op as BOp};

    fn daylight() -> vt::Palette {
        libhalcyon::theme::daylight_palette()
    }

    fn gs() -> GlyphSource {
        GlyphSource::new_vendored(512)
    }

    #[test]
    fn face_rule_plain_is_mono_annotated_is_body() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"plain ");
        wire::open(&mut buf, BOp::Obj, &[("type", "path"), ("ref", "/x")]);
        buf.extend_from_slice(b"linked");
        wire::close(&mut buf, BOp::Obj);
        buf.extend_from_slice(b"\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let b = &t.frozen_blocks()[0];
        let sheet = daylight_sheet();
        let mut g = gs();
        let laid = layout_block(b, 600, &sheet, &mut g);
        let line = &laid.lines[0];
        assert!(line.segs.len() >= 2);
        assert_eq!(line.segs[0].face, FACE_MONO, "un-annotated text is mono");
        assert_eq!(line.segs[1].face, FACE_BODY, "the obj span is body");
        assert_eq!(line.segs[1].color, sheet.obj, "obj at default ink takes the slate object colour");
        assert!(line.segs[1].obj > 0, "the obj hit target rides the seg");
    }

    #[test]
    fn mixed_line_uses_body_box_mono_line_uses_cell_box() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"pure mono line\n");
        wire::open(&mut buf, BOp::Em, &[("class", "strong")]);
        buf.extend_from_slice(b"mixed");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b" line\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let b = &t.frozen_blocks()[0];
        let sheet = daylight_sheet();
        let mut g = gs();
        let (_, cell_h, _) = g.mono_cell();
        let laid = layout_block(b, 600, &sheet, &mut g);
        assert_eq!(laid.lines[0].h, cell_h, "all-mono line keeps the exact cell box");
        let body_lm = g.line_metrics(FACE_BODY, sheet.body_px).unwrap();
        assert_eq!(laid.lines[1].h, body_lm.line_height + 0, "mixed line takes the body box");
    }

    #[test]
    fn wrap_reflows_deterministically() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Em, &[("class", "emph")]);
        buf.extend_from_slice(b"the quick brown fox jumps over the lazy dog and keeps going yet further\n");
        wire::close(&mut buf, BOp::Em);
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let b = &t.frozen_blocks()[0];
        let sheet = daylight_sheet();
        let mut g = gs();
        let wide = layout_block(b, 600, &sheet, &mut g);
        let narrow = layout_block(b, 220, &sheet, &mut g);
        let narrow2 = layout_block(b, 220, &sheet, &mut g);
        assert!(narrow.lines.len() > wide.lines.len(), "narrow wraps more");
        assert_eq!(narrow.lines.len(), narrow2.lines.len(), "same width, same shape");
        assert_eq!(narrow.height, narrow2.height);
        // Every glyph stays inside the width.
        for l in narrow.lines.iter() {
            for s in l.segs.iter() {
                assert!(s.x_end <= 220, "seg spills: {}", s.x_end);
            }
        }
        // No content lost: total glyph count matches.
        let count = |lb: &LaidBlock| lb.lines.iter().flat_map(|l| l.segs.iter()).map(|s| s.refs.len()).sum::<usize>();
        assert_eq!(count(&wide), count(&narrow), "reflow loses nothing");
    }

    #[test]
    fn table_aligns_and_rules() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Table, &[("cols", "lr"), ("hdr", "1")]);
        for (a, bb) in [("NAME", "SIZE"), ("x", "12345"), ("longer", "7")] {
            wire::open(&mut buf, BOp::Row, &[]);
            wire::open(&mut buf, BOp::Cell, &[]);
            buf.extend_from_slice(a.as_bytes());
            wire::close(&mut buf, BOp::Cell);
            wire::open(&mut buf, BOp::Cell, &[]);
            buf.extend_from_slice(bb.as_bytes());
            wire::close(&mut buf, BOp::Cell);
            wire::close(&mut buf, BOp::Row);
            buf.extend_from_slice(b"\n");
        }
        wire::close(&mut buf, BOp::Table);
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let b = &t.frozen_blocks()[0];
        let sheet = daylight_sheet();
        let mut g = gs();
        let laid = layout_block(b, 600, &sheet, &mut g);
        assert_eq!(laid.lines.len(), 3, "three table rows lay as three lines");
        // The right-aligned column: row ends align.
        let end = |l: &LaidLine| l.segs.last().map(|s| s.x_end).unwrap_or(0);
        let e1 = end(&laid.lines[1]);
        let e2 = end(&laid.lines[2]);
        assert_eq!(e1, e2, "r-aligned column shares the right edge");
        assert!(!laid.rects.is_empty(), "the header rule painted");
    }

    #[test]
    fn exit_badge_only_on_failure() {
        let sheet = daylight_sheet();
        let mut g = gs();
        for (code, expect_badge) in [(0i64, false), (7, true)] {
            let mut t = Transcript::new(daylight());
            let mut buf = Vec::new();
            wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
            buf.extend_from_slice(b"did things\n");
            let code_s = if code == 0 { "0" } else { "7" };
            wire::point(&mut buf, BOp::Mark, &[("k", "exit"), ("code", code_s)]);
            wire::close(&mut buf, BOp::Zone);
            t.feed(&buf);
            let laid = layout_block(&t.frozen_blocks()[0], 400, &sheet, &mut g);
            let has_err_seg = laid
                .lines
                .iter()
                .flat_map(|l| l.segs.iter())
                .any(|s| s.color == sheet.err);
            assert_eq!(has_err_seg, expect_badge, "exit {}", code);
        }
    }

    #[test]
    fn exit_badge_i64_min_does_not_panic() {
        // `mark k=exit code=<i64::MIN>` is an untrusted frame; the badge's
        // magnitude must come from unsigned_abs, not `-code` (which panics on
        // i64::MIN under overflow-checks -> the console dies) (F2).
        let sheet = daylight_sheet();
        let mut g = gs();
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"did things\n");
        wire::point(&mut buf, BOp::Mark, &[("k", "exit"), ("code", "-9223372036854775808")]);
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        // Must not panic; the failure badge renders (nonzero exit).
        let laid = layout_block(&t.frozen_blocks()[0], 400, &sheet, &mut g);
        let has_err_seg = laid
            .lines
            .iter()
            .flat_map(|l| l.segs.iter())
            .any(|s| s.color == sheet.err);
        assert!(has_err_seg, "the i64::MIN exit still renders a failure badge");
    }

    #[test]
    fn end_to_end_pixels_and_reflow() {
        let mut t = Transcript::with_caps(
            daylight(),
            DEFAULT_MAX_BLOCKS,
            DEFAULT_MAX_COST,
            DEFAULT_MAX_LINES_PER_BLOCK,
        );
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "prompt")]);
        buf.extend_from_slice(b"$ ls\n");
        wire::close(&mut buf, BOp::Zone);
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Obj, &[("type", "path"), ("ref", "/version")]);
        buf.extend_from_slice(b"version");
        wire::close(&mut buf, BOp::Obj);
        buf.extend_from_slice(b"\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet();
        let mut g = gs();
        let mut cart = Cartoon::new();
        cart.ops.push(Op::Clear { color: sheet.ground });
        let mut y = 4;
        for b in t.frozen_blocks().iter() {
            let laid = layout_block(b, 300, &sheet, &mut g);
            render_block(&mut cart, &laid, y, &g);
            y += laid.height + sheet.block_gap;
        }
        let w = 300usize;
        let h = (y + 4) as usize;
        let mut px = alloc::vec![0u32; w * h];
        cartoon::execute(&cart, &g.packer.store, &cartoon::BlobStore::new(), &mut px, w, None);
        let ink = px.iter().filter(|&&p| p != sheet.ground).count();
        assert!(ink > 100, "the session inked {} px", ink);
    }
}
