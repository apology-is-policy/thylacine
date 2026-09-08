// Layout: the pure function from a frozen Block + a width + the stylesheet
// to positioned line boxes (HALCYON.md section 13.3 -- store semantics,
// derive pixels; reflow-on-resize is re-running this function, so it must
// be deterministic and width-total). Rendering is then a trivial
// translation of laid lines into cartoon ops.
//
// The composition is HALCYON-COMPOSITION.md made computable: the type scale
// (section 2), the vertical rhythm with CSS-collapsing margins (section 3 --
// the mockup was rendered by a browser, so adjacent margins collapse to the
// larger), and the baseline rule for monospace islands (section 4: the
// proportional face alone sets the line box; a Cornucopia run is aligned
// onto its baseline and may not stretch the box).
//
// The class rule, per line (HALCYON.md 14.13 + HALCYON-VISUAL 7/9, the
// operator's sc3 ruling; `transcript::LineClass`): a PROMPT line is the
// shell's own, proportional at the prompt size; a DOC line is Beacon-
// structured content -- proportional prose (IBM Plex Sans) with mono
// islands for `em class=code` and the `pre` block; a RAW line is a program's
// un-annotated terminal bytes -- "preformatted output, terminal content" --
// set in the mono island with the `.hal-out` chrome. Alt-screen is a
// separate raw-grid path. Weight/slant per section 8: em strong (and foreign
// SGR bold on an annotated run) is the one bold; a heading is Regular-weight
// (400) italic, ranked by SIZE never weight; em emph is Text italic;
// everything else is the Text-weight (450) body.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use vt::ATTR_BOLD;

use crate::raster::{
    mono_advances, GlyphSource, FACE_BODY, FACE_BODY_BOLD, FACE_BODY_ITALIC, FACE_HEADING_ITALIC,
    FACE_MONO,
};
use crate::transcript::{
    hdr_is_title, hdr_level, Block, BlockKind, Item, LineClass, Style, TCell, EM_CODE, EM_DIM,
    EM_EMPH, EM_STRONG,
};
use libhalcyon::theme::Metrics;

/// The stylesheet: the paper-light theme's numbers (section 3 -- dark ink
/// in full daylight) AT A DISPLAY SCALE (HALCYON-SCALE 6): every size below
/// is PHYSICAL px, derived once from its logical value by `daylight_sheet`,
/// and every logical constant in this module reaches a pixel only through
/// `px` / `ipx` -- the ONE place halcyond scales. Colors are ARGB like
/// everything in the weave.
#[derive(Clone, Copy)]
pub struct Sheet {
    pub ground: u32, // Daylight surface
    pub ink: u32,    // Daylight fg
    pub dim: u32,    // Daylight fg_dim (em--dim, raw output ink, the Normal-mode caret)
    pub accent: u32, // Daylight ember (the Insert caret / turnstile / running mark)
    pub obj: u32,    // Daylight syntax.slate (presentation refs -- section 1.5)
    pub err: u32,    // Daylight cinnabar (exit failure)
    pub ok: u32,     // Daylight fen (exit success; reserved -- H-2 is failure-only)
    pub rule: u32,   // Daylight border (rules, the obj pill stroke)
    pub sel_bg: u32,
    /// The island ground: the code span, the `pre` block and raw output all
    /// sit on the Daylight header tone (`.hal-em--code` / `.hal-out`).
    pub island_ground: u32,
    /// The island's leading gutter rule (`.hal-out` border-left).
    pub island_rule: u32,
    /// The display scale in percent (`libhalcyon::scale`): the compositor's
    /// value, read off its ctl; 100 is the identity on every size here.
    pub scale: u16,
    /// The chrome metrics at `scale` (`Metrics::at`): the tag-bar padding,
    /// the hairline, the bar heights -- the SAME table the compositor
    /// carves with, so the two painters agree by construction.
    pub metrics: Metrics,
    /// The structural hairline at `scale` (`metrics.hairline`): rules, the
    /// obj pill's stroke, the island gutter's unit, the menu's border.
    pub hairline: i32,
    /// The 2-px structural marks at `scale` (`ipx(2)`): the caret and the
    /// selected run's underline.
    pub mark_w: i32,
    pub body_px: f32,
    /// The prompt zone's size (path / turnstile / command, section 2).
    pub prompt_px: f32,
    /// The heading sizes by level (HALCYON-VISUAL 8.1: 17.5 / 14.5 / 12.5
    /// logical), at `scale`.
    pub hdr_px: [f32; 3],
    /// The two mono ems at `scale`: twice the selected bakes' advances
    /// (`raster::mono_advances`; the bake's 0.5-em rule) -- what a mono
    /// span asks the glyph source for, so it lands on the island or the
    /// grid atlas the source selected for the same scale.
    pub mono_island_px: f32,
    pub mono_grid_px: f32,
    pub pad_x: i32,
    pub block_gap: i32,
    pub table_col_gap: i32,
    /// The two-column list's gap between its (name, value) groups.
    pub kv_col_gap: i32,
    /// The smoothing stroke on every proportional glyph, thousandths of an
    /// em (`Theme.smooth_mem`, HALCYON-TYPE 4.2): the glyph source is set
    /// to this wherever the sheet is built, so the rasters follow the
    /// theme the sheet was built from.
    pub smooth_mem: u16,
    /// Bumps on any sheet change; part of the layout-cache key.
    pub gen: u32,
}

impl Sheet {
    /// A logical size in physical px at this sheet's scale (fractional:
    /// glyph sizes stay sub-pixel, COMPOSITION 1).
    #[inline]
    pub fn px(&self, logical: f32) -> f32 {
        libhalcyon::scale::px(logical, self.scale)
    }

    /// A logical integer size in physical px, round half up (paddings,
    /// gaps, pills, rules).
    #[inline]
    pub fn ipx(&self, logical: i32) -> i32 {
        libhalcyon::scale::ipx(logical, self.scale)
    }
}

/// The paper-light transcript sheet at a display scale (percent), built
/// from the Daylight visual scripture (docs/HALCYON-VISUAL.md via
/// libhalcyon::theme -- the single token source the H-3 split names).
/// Replaces the H-2 approximation seeded from vt::THEMES[1]: the transcript
/// now matches the chrome that H-3a's compositor bevels + tag bar draw
/// around it, because both derive from DAYLIGHT. At 100 every size is the
/// logical value (nothing at 1.0 moves; pinned by test).
pub fn daylight_sheet(scale: u16) -> Sheet {
    let d = &libhalcyon::theme::DAYLIGHT;
    let metrics = Metrics::at(scale);
    let (island, grid) = mono_advances(scale);
    let px = |v: f32| libhalcyon::scale::px(v, scale);
    let ipx = |v: i32| libhalcyon::scale::ipx(v, scale);
    Sheet {
        ground: d.surface,
        smooth_mem: d.smooth_mem,
        ink: d.fg,
        dim: d.fg_dim,
        accent: d.ember,
        obj: d.syntax.slate,
        err: d.cinnabar.key,
        ok: d.syntax.fen,
        rule: d.border,
        // A parchment-compatible selection band: a warm step between surface
        // and header (Daylight has no transcript-selection token; this sits in
        // the same family, darker than surface, lighter than header).
        sel_bg: 0xFFDF_D6C7,
        island_ground: d.header,
        // `.hal-out`'s border-left -- the one transcript stroke the mockup
        // stylesheet carries as a literal rather than a token.
        island_rule: 0xFF7A_6850,
        scale,
        metrics,
        hairline: metrics.hairline,
        mark_w: ipx(2),
        // The body/prose size the Daylight mockup runs at (halcyon-daylight.css
        // .hal-prose 11.5px; HALCYON-VISUAL 7-8 type scale).
        body_px: px(11.5),
        // The prompt runs at the BASE size (the operator, 2026-09-08, on the
        // first live look: "rather small and not prominent"; it was 10 --
        // smaller than the prose it introduces). One value, never below the
        // body's.
        prompt_px: px(11.5),
        hdr_px: [px(17.5), px(14.5), px(12.5)],
        mono_island_px: 2.0 * island as f32,
        mono_grid_px: 2.0 * grid as f32,
        // The text inset, MEASURED off the operator's mockup render
        // (halcyon_text_composition_mockup.png at 2x: the prose starts 22
        // image px inside the pane's parchment edge -- 11 logical; the
        // stylesheet's 10 + 2 + 8 would put it 9 px further in).
        pad_x: ipx(12),
        block_gap: ipx(6),
        table_col_gap: ipx(16),
        kv_col_gap: ipx(28),
        gen: 0,
    }
}

// The vertical rhythm (HALCYON-COMPOSITION 2-3), LOGICAL px: every use
// below goes through `Sheet::ipx` (the factors are unitless and unchanged).
const LH_BODY: f32 = 1.5; // prose line-height
const LH_HDR: f32 = 1.25; // heading line-height
/// The mono row pitch of an island / raw output: round(10 x 1.55), the
/// `.hal-out` line box; the cell sits centred in it.
const PRE_LINE_H: i32 = 16;
const HDR_TOP: [i32; 3] = [10, 8, 6];
const HDR_BOTTOM: i32 = 2;
const PROSE_MARGIN: i32 = 2;
const RULE_MARGIN: i32 = 8;
const TABLE_TOP: i32 = 4;
const TABLE_BOTTOM: i32 = 6;
const TABLE_ROW_GAP: i32 = 3;
/// The gap under a headed table's header rule.
const TABLE_HDR_GAP: i32 = 3;
const ISLAND_MARGIN: i32 = 2;
const ISLAND_PAD_Y: i32 = 2;
const ISLAND_PAD_X: i32 = 8;
const ISLAND_RULE_W: i32 = 2;
const DECK_TOP: i32 = 4;
const DECK_BOTTOM: i32 = 8;
const PROMPT_BOTTOM: i32 = 2;
/// The inline chrome's horizontal padding: the obj pill (`.hal-obj`) and the
/// code span's ground (`.hal-em--code`).
const OBJ_PAD: i32 = 4;
const CODE_PAD: i32 = 3;
/// The narrowest width a block lays at: the two insets plus this.
const MIN_CONTENT_W: i32 = 8;
/// The line box a caret falls back to when nothing laid out.
const FALLBACK_LINE_H: i32 = 16;

/// Inline chrome a seg asks for (painted under it at line close).
pub const CHROME_NONE: u8 = 0;
pub const CHROME_CODE: u8 = 1;
pub const CHROME_OBJ: u8 = 2;

/// One laid glyph: the codepoint and the advance it was laid with (kerning
/// folded in). NOT an atlas id: a laid block never references the atlas,
/// so an atlas eviction invalidates no layout and laying a block out packs
/// nothing -- `render_block` resolves the id for what it paints, and only
/// then (the atlas working set is the painted set, never the transcript).
/// The whole-pixel width the sub-pixel pen lays a run of chars at: the
/// SAME accumulation the lay loop performs (1/256 px, one whole division
/// at the end), never a sum of rounded per-glyph advances. Measuring one
/// way and laying the other is how a right-aligned run drifts off its
/// edge and a table column comes up a pixel short -- the two must share
/// an accumulator, not merely agree in spirit.
///
/// That includes the KERN `lay_span` folds into each step. It is zero for
/// every pair today (nothing reads a pair table), so this term changes no
/// current measurement -- but a measure that omits a term the lay adds is
/// the drift above, waiting for the shaper to arrive. Held here so the
/// GPOS seam lands without a second bug.
fn run_width(gs: &mut GlyphSource, face: u8, px: f32, chars: impl Iterator<Item = char>) -> i32 {
    let mut q = 0i32;
    let mut prev: Option<char> = None;
    for ch in chars {
        if let Some(p) = prev {
            if face != FACE_MONO {
                q += gs.kern(face, px, p, ch) * GlyphSource::PEN_SCALE;
            }
        }
        if let Some(a) = gs.advance_fx(face, px, ch) {
            q += a;
            prev = Some(ch);
        }
    }
    q.div_euclid(GlyphSource::PEN_SCALE)
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct LaidGlyph {
    pub ch: char,
    /// The WHOLE-pixel step from this glyph's pen to the next one's. Not
    /// the font advance: with a sub-pixel pen the font advance is
    /// fractional, and what the executor (which accumulates integers) must
    /// be handed is the difference between consecutive WHOLE pen
    /// positions. Summing these over a run gives the run's whole width by
    /// construction, so measuring and painting cannot disagree.
    pub advance: i32,
    /// This glyph's horizontal phase, quarter-pixels 0..=3 -- the fraction
    /// of the true pen that the whole position dropped, which the atlas
    /// serves as a distinct raster (HALCYON-TYPE 4.3).
    pub phase: u8,
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
    pub refs: Vec<LaidGlyph>,
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
    /// CHROME_*: the inline ground/pill painted under this seg.
    pub chrome: u8,
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

fn face_for(st: &Style, in_table: bool) -> u8 {
    // An inline `em class=code` literal is the only mono case reaching here
    // (8.2); a `pre` block / a raw line is forced mono at the lay_span call,
    // and alt-screen is a separate raw-grid path. Everything else is
    // proportional (14.13).
    if st.em == EM_CODE {
        return FACE_MONO;
    }
    // `annotated` gates only whether a foreign SGR bold promotes to the
    // reserved bold: strong/emph/hdr are themselves annotations, so a plain
    // run is the proportional body regardless of SGR bold -- foreign bold is
    // NOT the one em-strong bold (8.2).
    let annotated = st.annotated() || in_table;
    if !annotated {
        return FACE_BODY;
    }
    // Genera type discipline (HALCYON.md section 3 + HALCYON-VISUAL 8): bold is
    // RESERVED for extreme emphasis -- em class=strong and foreign SGR bold on
    // an annotated run, nothing else; emphasis and headings go ITALIC, heading
    // RANK carried by size (px_for), never weight -- bold headings are retired.
    if st.em == EM_STRONG || st.attrs & ATTR_BOLD != 0 {
        FACE_BODY_BOLD
    } else if st.hdr != 0 {
        // Headings are the Regular-weight (400) italic, a DISTINCT weight from
        // the body italic (operator's baseline=Text / bigger=Regular rule);
        // rank stays size-carried (px_for), never weight.
        FACE_HEADING_ITALIC
    } else if st.em == EM_EMPH {
        FACE_BODY_ITALIC
    } else {
        // An obj / table-cell presentation with no weight or slant is the Text
        // body: an object is a colour + hit overlay (the pill), not a font
        // change, so it matches surrounding proportional text; inside a `pre`
        // block the pre flag forces it mono instead, keeping the grid's cell
        // metrics.
        FACE_BODY
    }
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

/// The proportional size of a run: a heading's rank is the ABSOLUTE size
/// HALCYON-VISUAL 8.1 pins (17.5 / 14.5 / 12.5 logical, the sheet's
/// `hdr_px` at its scale), all above the body (11.5); the weight is fixed
/// Regular (face_for -> FACE_HEADING_ITALIC). `base` is the class's own
/// size (body, or the prompt's).
fn px_for(st: &Style, base: f32, sheet: &Sheet) -> f32 {
    match hdr_level(st.hdr) {
        1 => sheet.hdr_px[0],
        2 => sheet.hdr_px[1],
        3 => sheet.hdr_px[2],
        _ => base,
    }
}

/// Round half up to whole pixels (HALCYON-COMPOSITION 1).
#[inline]
fn round_px(v: f32) -> i32 {
    (v + 0.5) as i32
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

/// How a span is set: the class's face/size rule and its wrap discipline.
#[derive(Clone, Copy, PartialEq, Eq)]
enum SpanMode {
    /// Beacon-structured content: the face rule, the body size, word-wrap.
    Doc,
    /// The shell's prompt: the face rule at the prompt size, word-wrap.
    Prompt,
    /// A `pre` block's line: mono, verbatim (no wrap).
    Pre,
    /// Raw terminal output: mono, wrapped at the character like a terminal.
    Raw,
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
    /// The pen's sub-pixel remainder in 1/256 px, 0..=255 (HALCYON-TYPE
    /// 4.3). `pen_x` stays the WHOLE pixel so every width comparison in
    /// this builder keeps its meaning; this carries what the whole part
    /// dropped. A glyph's PHASE is read off it by rounding to the nearest
    /// quarter -- a per-glyph decision that is never fed back, so the
    /// coarse four-phase grid costs no accumulated drift. Reset to 0
    /// wherever `pen_x` is ASSIGNED (a line or block start is a whole
    /// pixel); untouched by `+= pad`, since a whole-pixel pad cannot
    /// change a fraction.
    pen_q: i32,
    /// The left inset of the current line (the text inset, or the island's).
    x0: i32,
    any_body: bool,
    line_px: f32,
    line_hdr: bool,
    /// The class of the line under construction (an empty line's box).
    line_class: LineClass,
    /// Centre the line's content in the available width (the herald).
    center: bool,
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
            pen_q: 0,
            x0: sheet.pad_x,
            any_body: false,
            line_px: 0.0,
            line_hdr: false,
            line_class: LineClass::Doc,
            center: false,
        }
    }

    fn note_metrics(&mut self, face: u8, px: f32, hdr: bool) {
        if face != FACE_MONO {
            self.any_body = true;
            if px > self.line_px {
                self.line_px = px;
            }
            self.line_hdr |= hdr;
        }
    }

    /// The proportional line box at `px`: exactly the line-height's pixels
    /// (section 2's 1.5 / 1.25 -- CSS semantics: the box is the line-height,
    /// the face's own ascent/descent [the content box] centred in it, the
    /// leading split above and below; Plex's 1.3-em content overflows a
    /// 1.25 heading box by a pixel, as it does in the browser). Returns
    /// (ascent, descent) of the box and (ascent, descent) of the content.
    fn body_box(gs: &GlyphSource, px: f32, factor: f32) -> ((i32, i32), (i32, i32)) {
        let (asc, desc) = match gs.line_metrics(FACE_BODY, px) {
            Some(m) => (m.ascent, m.descent),
            None => (round_px(px), round_px(px * 0.3)),
        };
        let lead = round_px(px * factor).max(1) - (asc + desc);
        let top = lead / 2;
        ((asc + top, desc + lead - top), (asc, desc))
    }

    /// The mono row box: the island cell centred in the `.hal-out` pitch.
    fn mono_box(gs: &GlyphSource, sheet: &Sheet) -> ((i32, i32), (i32, i32)) {
        let (_, chh, base) = gs.island_cell();
        let lead = (sheet.ipx(PRE_LINE_H) - chh).max(0);
        let top = lead / 2;
        ((base + top, chh - base + lead - top), (base, chh - base))
    }

    /// Close the current visual line (13.5 + COMPOSITION 4: the body
    /// metrics OWN a mixed line -- mono islands sit on the body baseline and
    /// may not stretch the box; an all-mono line keeps the mono row box).
    fn break_line(&mut self, gs: &GlyphSource) {
        let ((asc, desc), (asc_c, desc_c)) = if self.segs.is_empty() {
            // An empty line still occupies its class's line box.
            match self.line_class {
                LineClass::Raw => Self::mono_box(gs, self.sheet),
                LineClass::Prompt => Self::body_box(gs, self.sheet.prompt_px, LH_BODY),
                _ => Self::body_box(gs, self.sheet.body_px, LH_BODY),
            }
        } else if self.any_body {
            let factor = if self.line_hdr { LH_HDR } else { LH_BODY };
            Self::body_box(gs, self.line_px, factor)
        } else {
            Self::mono_box(gs, self.sheet)
        };
        let h = asc + desc;
        let baseline = self.y + asc;
        let mut segs = core::mem::take(&mut self.segs);
        // The herald: centre the content between the insets.
        if self.center && !segs.is_empty() {
            let first = segs.first().map(|s| s.x).unwrap_or(self.x0);
            let last = segs.last().map(|s| s.x_end).unwrap_or(first);
            let avail = self.width - self.sheet.pad_x - self.x0;
            let shift = (avail - (last - first)) / 2;
            if shift > 0 {
                for s in segs.iter_mut() {
                    s.x += shift;
                    s.x_end += shift;
                    for x in s.xs.iter_mut() {
                        *x += shift;
                    }
                }
            }
        }
        // The inline chrome: the code ground under a mono island (the cell
        // box), the pill around an obj (the content box + a hairline).
        let (_, ichh, ibase) = gs.island_cell();
        let (code_pad, obj_pad, hair) = (
            self.sheet.ipx(CODE_PAD),
            self.sheet.ipx(OBJ_PAD),
            self.sheet.hairline,
        );
        for s in segs.iter() {
            if s.refs.is_empty() {
                continue;
            }
            match s.chrome {
                CHROME_CODE => {
                    self.rects.push(RectSpec {
                        x: s.x - code_pad,
                        y: baseline - ibase,
                        w: (s.x_end - s.x + 2 * code_pad).max(0) as u32,
                        h: ichh as u32,
                        color: self.sheet.island_ground,
                    });
                }
                CHROME_OBJ => {
                    let x = s.x - obj_pad;
                    let w = (s.x_end - s.x + 2 * obj_pad).max(0) as u32;
                    let y = baseline - asc_c;
                    let hh = (asc_c + desc_c) as u32;
                    self.rects.push(RectSpec {
                        x,
                        y,
                        w,
                        h: hh,
                        color: self.sheet.island_ground,
                    });
                    let stroke = self.sheet.rule;
                    let hu = hair as u32;
                    for (rx, ry, rw, rh) in [
                        (x, y, w, hu),
                        (x, y + hh as i32 - hair, w, hu),
                        (x, y, hu, hh),
                        (x + w as i32 - hair, y, hu, hh),
                    ] {
                        self.rects.push(RectSpec {
                            x: rx,
                            y: ry,
                            w: rw,
                            h: rh,
                            color: stroke,
                        });
                    }
                }
                _ => {}
            }
        }
        self.lines.push(LaidLine {
            y: self.y,
            h,
            baseline,
            segs,
            src_item: usize::MAX,
            src_row: usize::MAX,
        });
        self.y += h;
        self.pen_x = self.x0;
        self.pen_q = 0;
        self.any_body = false;
        self.line_px = 0.0;
        self.line_hdr = false;
    }

    /// Lay one styled span, wrapping at the right edge (word-wrap breaks at
    /// the last space on the line when one exists, else hard-breaks; a raw
    /// line breaks at the character; a pre line never breaks).
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
        mode: SpanMode,
    ) {
        let mono = matches!(mode, SpanMode::Pre | SpanMode::Raw);
        let face = if mono { FACE_MONO } else { face_for(st, in_table) };
        let base_px = if mode == SpanMode::Prompt {
            self.sheet.prompt_px
        } else {
            self.sheet.body_px
        };
        let px = if face == FACE_MONO {
            self.sheet.mono_island_px
        } else {
            px_for(st, base_px, self.sheet)
        };
        let mut color = color_for(st, self.sheet);
        if mode == SpanMode::Raw && st.fg == self.sheet.ink {
            // `.hal-out`: raw output's default ink is the dim step.
            color = self.sheet.dim;
        }
        let chrome = if mode == SpanMode::Doc && st.obj != 0 {
            CHROME_OBJ
        } else if mode == SpanMode::Doc && face == FACE_MONO {
            CHROME_CODE
        } else {
            CHROME_NONE
        };
        let pad = match chrome {
            CHROME_OBJ => self.sheet.ipx(OBJ_PAD),
            CHROME_CODE => self.sheet.ipx(CODE_PAD),
            _ => 0,
        };
        let word_wrap = matches!(mode, SpanMode::Doc | SpanMode::Prompt);
        let no_wrap = mode == SpanMode::Pre;
        // A space-less span (a code island, a pill, one long word) that will
        // not fit the rest of the line moves WHOLE to the next line when the
        // line already holds content -- the break opportunity is the space
        // that ended the previous span, which this span cannot see.
        if word_wrap && self.pen_x > self.x0 && !cells.iter().any(|c| c.ch == ' ') {
            let w = 2 * pad + run_width(gs, face, px, cells.iter().map(|c| c.ch));
            if self.pen_x + w > self.width - self.sheet.pad_x && w <= self.width - self.sheet.pad_x - self.x0 {
                self.break_line(gs);
            }
        }
        self.note_metrics(face, px, st.hdr != 0);
        self.pen_x += pad;
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
            chrome,
        };
        let mut col = src_col;
        let mut last_space: Option<(usize, usize)> = None; // (refs idx AFTER the space, col after)
        let mut i = 0;
        let right = self.width - self.sheet.pad_x - pad;
        while i < cells.len() {
            let ch = cells[i].ch;
            let Some(adv_q) = gs.advance_fx(face, px, ch) else {
                i += 1;
                col += 1;
                continue;
            };
            // The pen advances in QUARTER-pixels; kerning is whole px (and
            // 0 on Plex, which ships no pair table this reads), so it
            // scales in. The glyph's own phase and whole step are resolved
            // AFTER the wrap decision below, because a wrap moves the pen
            // to a line start and re-zeroes the fraction.
            let step_q = adv_q
                + if face != FACE_MONO && i + 1 < cells.len() {
                    gs.kern(face, px, ch, cells[i + 1].ch) * GlyphSource::PEN_SCALE
                } else {
                    0
                };
            let provisional = (self.pen_q + step_q).div_euclid(GlyphSource::PEN_SCALE);
            if !no_wrap && self.pen_x + provisional > right && !seg.refs.is_empty() {
                let cut = match last_space {
                    Some((c, cc)) if word_wrap && c > 0 && c < seg.refs.len() => Some((c, cc)),
                    _ => None,
                };
                let (color2, bg2, face2, px2, obj2, chrome2) =
                    (seg.color, seg.bg, seg.face, seg.px, seg.obj, seg.chrome);
                if let Some((cut, cut_col)) = cut {
                    // Wrap at the last space boundary inside this seg; the
                    // spilled glyphs re-lay at the new line start.
                    let spill_refs: Vec<LaidGlyph> = seg.refs.split_off(cut);
                    seg.xs.truncate(cut);
                    seg.x_end = seg.xs.last().copied().unwrap_or(seg.x)
                        + seg.refs.last().map(|r| r.advance).unwrap_or(0);
                    self.segs.push(seg);
                    self.break_line(gs);
                    self.note_metrics(face2, px2, st.hdr != 0);
                    self.pen_x += pad;
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
                        chrome: chrome2,
                    };
                    // The spilled glyphs re-lay at a NEW pen, so their
                    // phases and whole steps are re-derived rather than
                    // carried: a phase is relative to the pen it was laid
                    // at, and reusing one across a line break offsets the
                    // whole tail by up to 3/4 px. (Re-deriving also drops
                    // the kern folded into the old advance, which is the
                    // right answer anyway -- the pair at a wrap boundary
                    // is not the pair that was there before it -- and is
                    // exactly 0 on Plex today.)
                    for mut r in spill_refs {
                        let aq = gs
                            .advance_fx(face2, px2, r.ch)
                            .unwrap_or(r.advance * GlyphSource::PEN_SCALE);
                        let total = self.pen_q + aq;
                        r.phase = crate::raster::phase_of(self.pen_q);
                        r.advance = total.div_euclid(GlyphSource::PEN_SCALE);
                        seg.xs.push(self.pen_x);
                        self.pen_x += r.advance;
                        self.pen_q = total.rem_euclid(GlyphSource::PEN_SCALE);
                        seg.refs.push(r);
                    }
                    seg.x_end = self.pen_x;
                } else {
                    // Hard break before the current glyph.
                    seg.x_end = self.pen_x;
                    self.segs.push(seg);
                    self.break_line(gs);
                    self.note_metrics(face2, px2, st.hdr != 0);
                    self.pen_x += pad;
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
                        chrome: chrome2,
                    };
                }
                last_space = None;
            }
            let total = self.pen_q + step_q;
            let gr = LaidGlyph {
                ch,
                advance: total.div_euclid(GlyphSource::PEN_SCALE),
                phase: crate::raster::phase_of(self.pen_q),
            };
            seg.xs.push(self.pen_x);
            self.pen_x += gr.advance;
            self.pen_q = total.rem_euclid(GlyphSource::PEN_SCALE);
            seg.refs.push(gr);
            if ch == ' ' {
                last_space = Some((seg.refs.len(), col + 1));
            }
            i += 1;
            col += 1;
        }
        seg.x_end = self.pen_x;
        self.pen_x += pad;
        if !seg.refs.is_empty() {
            self.segs.push(seg);
        }
    }
}

/// What a block item IS for the vertical rhythm (COMPOSITION 3), decided in
/// a pre-pass so a line knows its neighbours (the herald's deck, an island's
/// extent).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Role {
    Empty,
    Prose,
    /// A heading: (level, title-page?).
    Hdr(u8, bool),
    /// A herald's deck line (the dim pair under a title): (first, last).
    Deck(bool, bool),
    Prompt,
    Raw,
    Table,
    Rule,
    Pre,
}

impl Role {
    /// (top, bottom) margins at the sheet's scale (COMPOSITION 3; collapsed
    /// pairwise -- scaling then taking the larger is taking the larger
    /// then scaling, `ipx` being monotone).
    fn margins(self, sheet: &Sheet) -> (i32, i32) {
        let (t, b) = match self {
            Role::Empty => (0, 0),
            Role::Prose => (PROSE_MARGIN, PROSE_MARGIN),
            Role::Hdr(level, _) => (HDR_TOP[(level.clamp(1, 3) - 1) as usize], HDR_BOTTOM),
            Role::Deck(first, last) => (
                if first { DECK_TOP } else { 0 },
                if last { DECK_BOTTOM } else { 0 },
            ),
            Role::Prompt => (0, PROMPT_BOTTOM),
            Role::Raw | Role::Pre => (ISLAND_MARGIN, ISLAND_MARGIN),
            Role::Table => (TABLE_TOP, TABLE_BOTTOM),
            Role::Rule => (RULE_MARGIN, RULE_MARGIN),
        };
        (sheet.ipx(t), sheet.ipx(b))
    }
}

fn roles_of(b: &Block) -> Vec<Role> {
    let block_class = b.class();
    let mut roles: Vec<Role> = Vec::with_capacity(b.items.len());
    // The herald's deck: the dim, non-empty lines directly under a title.
    let mut deck_open = false;
    let mut deck_first = true;
    for item in b.items.iter() {
        let role = match item {
            Item::Table(_) => Role::Table,
            Item::Rule => Role::Rule,
            Item::Pre(_) => Role::Pre,
            Item::Line(line) => {
                let class = if line.class == LineClass::Inherit {
                    block_class
                } else {
                    line.class
                };
                match class {
                    LineClass::Prompt => Role::Prompt,
                    LineClass::Raw => Role::Raw,
                    _ => {
                        if line.cells.is_empty() {
                            Role::Empty
                        } else {
                            let first = b.styles.get(line.cells[0].style as usize).copied();
                            let hdr = first.map(|s| s.hdr).unwrap_or(0);
                            if hdr != 0 {
                                Role::Hdr(hdr_level(hdr), hdr_is_title(hdr))
                            } else if deck_open
                                && line.cells.iter().all(|c| {
                                    b.styles
                                        .get(c.style as usize)
                                        .map(|s| s.em == EM_DIM && s.hdr == 0)
                                        .unwrap_or(false)
                                })
                            {
                                Role::Deck(deck_first, false)
                            } else {
                                Role::Prose
                            }
                        }
                    }
                }
            }
        };
        match role {
            Role::Hdr(_, true) => {
                deck_open = true;
                deck_first = true;
            }
            Role::Deck(..) => {
                deck_first = false;
            }
            _ => {
                deck_open = false;
            }
        }
        roles.push(role);
    }
    // Mark the last deck line of each run.
    let n = roles.len();
    for i in 0..n {
        if let Role::Deck(first, _) = roles[i] {
            let last = i + 1 >= n || !matches!(roles[i + 1], Role::Deck(..));
            roles[i] = Role::Deck(first, last);
        }
    }
    roles
}

/// Lay a frozen (or the open) block at `width`. Pure in its inputs modulo
/// the glyph cache (rasterize-on-miss mutates `gs`; the RESULT is width-
/// and content-deterministic either way -- the property the reflow E2E
/// pins).
pub fn layout_block(b: &Block, width: i32, sheet: &Sheet, gs: &mut GlyphSource) -> LaidBlock {
    let mut lb = LineBuilder::new(sheet, width.max(2 * sheet.pad_x + sheet.ipx(MIN_CONTENT_W)));
    let roles = roles_of(b);
    let (island_pad_y, island_pad_x, island_rule_w, island_margin) = (
        sheet.ipx(ISLAND_PAD_Y),
        sheet.ipx(ISLAND_PAD_X),
        sheet.ipx(ISLAND_RULE_W),
        sheet.ipx(ISLAND_MARGIN),
    );
    // CSS margin collapsing: the gap before an element is the larger of the
    // previous element's bottom margin and its own top; the first element's
    // top margin is dropped (the first-child reset) unless it is the herald,
    // whose own top margin is the sanctioned override (COMPOSITION 3).
    let mut prev_bottom: Option<i32> = None;
    // A run of raw lines is ONE island: its chrome rects span the run.
    let mut island_top: Option<i32> = None;
    let close_island = |lb: &mut LineBuilder<'_>, top: i32| {
        lb.y += island_pad_y;
        let h = (lb.y - top).max(0) as u32;
        if h > 0 {
            lb.rects.push(RectSpec {
                x: sheet.pad_x,
                y: top,
                w: (lb.width - 2 * sheet.pad_x).max(0) as u32,
                h,
                color: sheet.island_ground,
            });
            lb.rects.push(RectSpec {
                x: sheet.pad_x,
                y: top,
                w: island_rule_w as u32,
                h,
                color: sheet.island_rule,
            });
        }
    };
    for (item_idx, item) in b.items.iter().enumerate() {
        let role = roles[item_idx];
        let (top, bottom) = role.margins(sheet);
        // Consecutive raw lines share one island: no margin between them.
        let joins_island = role == Role::Raw && island_top.is_some();
        if !joins_island {
            if let Some(t) = island_top.take() {
                close_island(&mut lb, t);
                prev_bottom = Some(island_margin);
            }
            let gap = match prev_bottom {
                None => match role {
                    Role::Hdr(_, true) => top,
                    _ => 0,
                },
                Some(pb) => pb.max(top),
            };
            lb.y += gap;
        }
        let lines_before = lb.lines.len();
        lb.x0 = sheet.pad_x;
        lb.pen_x = sheet.pad_x;
        lb.pen_q = 0;
        lb.center = false;
        lb.line_class = LineClass::Doc;
        match item {
            Item::Line(line) => {
                let (mode, class) = match role {
                    Role::Prompt => (SpanMode::Prompt, LineClass::Prompt),
                    Role::Raw => (SpanMode::Raw, LineClass::Raw),
                    _ => (SpanMode::Doc, LineClass::Doc),
                };
                lb.line_class = class;
                if role == Role::Raw {
                    if island_top.is_none() {
                        island_top = Some(lb.y);
                        lb.y += island_pad_y;
                    }
                    lb.x0 = sheet.pad_x + island_rule_w + island_pad_x;
                    lb.pen_x = lb.x0;
                    lb.pen_q = 0;
                }
                lb.center = matches!(role, Role::Hdr(_, true) | Role::Deck(..));
                for (s, e, sid) in runs_of(&line.cells) {
                    let st = b.styles[sid as usize];
                    let bg =
                        if st.bg != sheet.ground && st.bg != libhalcyon::theme::DAYLIGHT.surface {
                            Some(st.bg)
                        } else {
                            None
                        };
                    lb.lay_span(gs, &line.cells[s..e], &st, false, item_idx, s, bg, mode);
                }
                lb.break_line(gs);
            }
            Item::Table(t) => {
                lay_table(&mut lb, t, b, item_idx, sheet, gs);
            }
            Item::Rule => {
                lb.rects.push(RectSpec {
                    x: sheet.pad_x,
                    y: lb.y,
                    w: (lb.width - 2 * sheet.pad_x).max(0) as u32,
                    h: sheet.hairline as u32,
                    color: sheet.rule,
                });
                lb.y += sheet.hairline;
            }
            Item::Pre(lines) => {
                // PL-1b: the preformatted code-fence island (HALCYON.md
                // 110-113): each line is laid MONO + verbatim (no wrap), inset
                // past the leading gutter; the own ground + the gutter rule
                // are added AFTER the lines (their y-extent is then known);
                // render_block paints rects before glyphs, so they sit BEHIND
                // the mono text.
                let top = lb.y;
                lb.y += island_pad_y;
                lb.line_class = LineClass::Raw;
                for (li, line) in lines.iter().enumerate() {
                    lb.x0 = sheet.pad_x + island_rule_w + island_pad_x;
                    lb.pen_x = lb.x0;
                    lb.pen_q = 0;
                    let first = lb.lines.len();
                    for (s, e, sid) in runs_of(&line.cells) {
                        let st = b.styles[sid as usize];
                        // A run's own SGR background still shows through; the
                        // block ground is the default carrier otherwise.
                        let bg = if st.bg != sheet.ground
                            && st.bg != libhalcyon::theme::DAYLIGHT.surface
                        {
                            Some(st.bg)
                        } else {
                            None
                        };
                        lb.lay_span(gs, &line.cells[s..e], &st, false, item_idx, s, bg, SpanMode::Pre);
                    }
                    lb.break_line(gs);
                    // Each pre line is its own source row: the lines share
                    // the item, and a hit on the pre must name the line.
                    for l in lb.lines[first..].iter_mut() {
                        l.src_row = li;
                    }
                }
                close_island(&mut lb, top);
            }
        }
        // Stamp the item's visual lines with their source address (tables
        // stamped per-row inside lay_table already carry src_row).
        for l in lb.lines[lines_before..].iter_mut() {
            if l.src_item == usize::MAX {
                l.src_item = item_idx;
            }
        }
        if role != Role::Raw {
            prev_bottom = Some(bottom);
        }
    }
    if let Some(t) = island_top.take() {
        close_island(&mut lb, t);
    }
    lb.x0 = sheet.pad_x;
    lb.pen_x = sheet.pad_x;
    lb.pen_q = 0;
    lb.center = false;
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
    LaidBlock {
        height: lb.y,
        lines: lb.lines,
        rects,
    }
}

/// The vertical gap between two consecutive blocks of a transcript: a prompt
/// runs straight into its command's output -- one entry, `.hal-prompt`'s own
/// 2px under the prompt line -- every other boundary is the block gap
/// (`.hal-block` margin-bottom 6).
pub fn block_gap_between(prev: BlockKind, next: BlockKind, sheet: &Sheet) -> i32 {
    if prev == BlockKind::Prompt && next == BlockKind::Output {
        sheet.ipx(PROMPT_BOTTOM)
    } else {
        sheet.block_gap
    }
}

/// A headerless table whose spec is (l, r) pairs is the two-column LIST of
/// COMPOSITION 3 (the loaded-systems list): each pair is a name/value group;
/// the groups share the width equally with `kv_col_gap` between them, the
/// name at a group's left edge and the value right-aligned at its right.
fn is_kv_list(t: &crate::transcript::TableModel) -> bool {
    !t.hdr
        && t.cols.len() >= 2
        && t.cols.len().is_multiple_of(2)
        && t.cols
            .iter()
            .enumerate()
            .all(|(i, &c)| c == if i % 2 == 0 { b'l' } else { b'r' })
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
    let ncols = t
        .rows
        .iter()
        .map(|r| r.len())
        .max()
        .unwrap_or(0)
        .max(t.cols.len());
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
                let face = face_for(&st, true);
                let px = if face == FACE_MONO {
                    sheet.mono_island_px
                } else {
                    px_for(&st, sheet.body_px, sheet)
                };
                w += run_width(gs, face, px, cell[s..e].iter().map(|c| c.ch));
            }
            if ci < ncols && w > col_w[ci] {
                col_w[ci] = w;
            }
            ws.push(w);
        }
        cellw.push(ws);
    }
    let kv = is_kv_list(t);
    // Column x origins (a kv-list spreads its groups over the width).
    let mut col_x = alloc::vec![0i32; ncols];
    let mut x = sheet.pad_x;
    if kv {
        let groups = (ncols / 2) as i32;
        let inner = (lb.width - 2 * sheet.pad_x - (groups - 1) * sheet.kv_col_gap).max(0);
        let group_w = inner / groups;
        for g in 0..groups as usize {
            let gx = sheet.pad_x + g as i32 * (group_w + sheet.kv_col_gap);
            col_x[2 * g] = gx;
            col_w[2 * g] = group_w; // the pair shares the group; the value right-aligns in it
            col_x[2 * g + 1] = gx;
            col_w[2 * g + 1] = group_w;
        }
        x = lb.width - sheet.pad_x + sheet.table_col_gap;
    } else {
        for c in 0..ncols {
            col_x[c] = x;
            x = x.saturating_add(col_w[c].saturating_add(sheet.table_col_gap));
        }
    }
    // Lay rows: one visual line each (table cells are single-line by
    // construction -- the transcript capture maps controls to spaces).
    let nrows = t.rows.len();
    for (ri, row) in t.rows.iter().enumerate() {
        for (ci, cell) in row.iter().enumerate() {
            if ci >= ncols || cell.is_empty() {
                continue;
            }
            let align = t.cols.get(ci).copied().unwrap_or(b'l');
            let w = cellw[ri][ci];
            // A cell wider than its column (a kv-list value past its group)
            // starts at the column, never left of it.
            let x0 = match align {
                b'r' => (col_x[ci] + col_w[ci] - w).max(col_x[ci]),
                b'c' => (col_x[ci] + (col_w[ci] - w) / 2).max(col_x[ci]),
                _ => col_x[ci],
            };
            lb.pen_x = x0;
            lb.pen_q = 0;
            // The cell's runs carry their SOURCE columns (the cell's start
            // in the row + the run's offset in the cell), so a laid glyph
            // inverts to the grid cell it came from.
            let cell_start = t.starts.get(ri).and_then(|r| r.get(ci)).copied().unwrap_or(0);
            for (s, e, sid) in runs_of(cell) {
                let mut st = b.styles[sid as usize];
                if t.hdr && ri == 0 {
                    st.attrs |= ATTR_BOLD;
                }
                // NOTE: table spans never wrap (the cell was measured);
                // width is temporarily unbounded for the span.
                let saved_w = lb.width;
                lb.width = i32::MAX / 2;
                lb.lay_span(gs, &cell[s..e], &st, true, item_idx, cell_start + s, None, SpanMode::Doc);
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
                h: sheet.hairline as u32,
                color: sheet.rule,
            });
            lb.y += sheet.ipx(TABLE_HDR_GAP);
        } else if kv && ri + 1 < nrows {
            lb.y += sheet.ipx(TABLE_ROW_GAP);
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
    let w = run_width(gs, FACE_BODY, px, text.chars());
    lb.pen_x = (lb.width - sheet.pad_x - w).max(sheet.pad_x);
    lb.pen_q = 0;
    let st = Style {
        fg: sheet.err,
        bg: sheet.ground,
        attrs: 0,
        em: 0,
        obj: 0,
        hdr: 0,
    };
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
        chrome: CHROME_NONE,
    };
    lb.note_metrics(FACE_BODY, px, false);
    // The badge runs its own pen (it bypasses the block builder), so it
    // carries its own quarter-pixel remainder too.
    let mut q = 0i32;
    for c in cells.iter() {
        if let Some(aq) = gs.advance_fx(FACE_BODY, px, c.ch) {
            let total = q + aq;
            seg.xs.push(seg_start_x);
            let whole = total.div_euclid(GlyphSource::PEN_SCALE);
            seg.refs.push(LaidGlyph { ch: c.ch, advance: whole, phase: crate::raster::phase_of(q) });
            seg_start_x += whole;
            q = total.rem_euclid(GlyphSource::PEN_SCALE);
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
    // The pending line IS the prompt under the cursor (the console flow), so
    // it takes the prompt class -- never the raw mono of an un-annotated
    // foreign block.
    let b = Block {
        id: u64::MAX,
        kind: BlockKind::Prompt,
        continuation: false,
        exit: None,
        cmd: None,
        items: alloc::vec![Item::Line(crate::transcript::Line::plain(
            cells.to_vec()
        ))],
        styles: styles.to_vec(),
        objs: Vec::new(),
        cost: 0,
        annotated_own: styles.iter().any(|s| s.annotated()),
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
    (sheet.pad_x, 0, sheet.ipx(FALLBACK_LINE_H))
}

/// PL-4: the pixel position of column `col` within ONE logical line of a
/// multi-line laid block. `cursor_pos` counts a GLOBAL column across the whole
/// block, which is wrong for the live grid, whose block holds many logical
/// lines; this scopes to the LaidLines of source (`item`, `row`) (a logical
/// line's wrapped pieces share both; `row` is `usize::MAX` for a plain line
/// and matches any row then, a table row's / pre line's index otherwise).
/// `col` is the column within that logical line; the per-glyph `seg.xs` give
/// the x directly. A column past the item's content lands at the end of its
/// last laid line. Returns (x, line.y, line.h); a line box of the sheet's
/// fallback height when the item laid nothing.
pub fn caret_in_block(
    laid: &LaidBlock,
    item: usize,
    row: usize,
    col: usize,
    sheet: &Sheet,
) -> (i32, i32, i32) {
    let mut end: Option<(i32, i32, i32)> = None;
    for line in laid.lines.iter() {
        if line.src_item != item || (row != usize::MAX && line.src_row != row) {
            continue;
        }
        for seg in line.segs.iter() {
            let n = seg.refs.len();
            if col >= seg.src_col && col < seg.src_col + n {
                return (seg.xs[col - seg.src_col], line.y, line.h);
            }
        }
        let x = line.segs.last().map(|s| s.x_end).unwrap_or(0);
        end = Some((x, line.y, line.h));
    }
    end.unwrap_or((0, 0, sheet.ipx(FALLBACK_LINE_H)))
}

/// The visual span (block-relative y, height) of one source row -- a Line
/// item, one row of a table, one line of a pre -- across its (possibly
/// wrapped) laid lines. `row == usize::MAX` names the whole item (a Line's
/// only row; every line of a pre, the one selectable unit `select::flatten`
/// makes of it). None when the row laid nothing.
pub fn laid_line_for(laid: &LaidBlock, item: usize, row: usize) -> Option<(i32, i32)> {
    let mut y0: Option<i32> = None;
    let mut y1 = 0;
    for l in laid.lines.iter() {
        if l.src_item == item && (row == usize::MAX || l.src_row == row) {
            if y0.is_none() {
                y0 = Some(l.y);
            }
            y1 = l.y + l.h;
        }
    }
    y0.map(|y| (y, y1 - y))
}

/// Emit a laid block into the cartoon at (0, y0): background rects first,
/// then glyph runs (paint order is the op order). The glyph ids are
/// resolved HERE, for the glyphs this call paints (rasterized + packed on
/// first use): the laid block carries codepoints and advances only, so the
/// atlas working set is the painted set and an eviction between frames
/// invalidates nothing laid. A glyph the atlas refuses (the page cap) or
/// cannot serve paints blank with its laid advance kept -- the line's
/// geometry never moves with the atlas.
pub fn render_block(cart: &mut Cartoon, laid: &LaidBlock, y0: i32, gs: &mut GlyphSource) {
    for r in laid.rects.iter() {
        cart.ops.push(Op::Rect {
            x: r.x,
            y: y0 + r.y,
            w: r.w,
            h: r.h,
            color: r.color,
        });
    }
    let gen = gs.gen();
    let mut refs: Vec<GlyphRef> = Vec::new();
    for line in laid.lines.iter() {
        for seg in line.segs.iter() {
            if seg.refs.is_empty() {
                continue;
            }
            refs.clear();
            for g in seg.refs.iter() {
                let id = gs
                    .glyph_at(seg.face, seg.px, g.ch, g.phase)
                    .map(|r| r.glyph)
                    .unwrap_or(u32::MAX);
                refs.push(GlyphRef {
                    glyph: id,
                    advance: g.advance,
                });
            }
            // `baseline` is block-absolute (line.y + ascent).
            cart.push_glyphs(gen, seg.x, y0 + line.baseline, seg.color, &refs);
        }
    }
}

/// Frozen-block layout, cached by block id (stable identity; the open block
/// + pending line never cache -- they change every feed). Keyed on the
/// width and the sheet generation ONLY: a laid block holds no atlas id, so
/// an atlas eviction keeps every entry (the paint re-resolves what it
/// draws) -- a whole-history re-lay after an eviction, the shape that once
/// packed every distinct glyph of the transcript in ONE frame, cannot
/// happen. The bound is the LIVE set: the owner evicts the ids the budget
/// dropped (`evict_missing`) every frame, so the map never exceeds the
/// transcript's block cap. It is NOT a size-triggered reset: the console
/// walks every frozen block per frame, so a reset above some count re-laid
/// the whole history on every keystroke once the transcript outgrew it --
/// the atlas F1 shape on the layout axis.
pub struct LayoutCache {
    map: BTreeMap<u64, (i32, u32, LaidBlock)>,
    misses: u64,
}

impl LayoutCache {
    pub fn new() -> LayoutCache {
        LayoutCache {
            map: BTreeMap::new(),
            misses: 0,
        }
    }

    pub fn get(&mut self, b: &Block, width: i32, sheet: &Sheet, gs: &mut GlyphSource) -> &LaidBlock {
        let hit = matches!(self.map.get(&b.id), Some(e) if e.0 == width && e.1 == sheet.gen);
        if !hit {
            self.misses += 1;
            let laid = layout_block(b, width, sheet, gs);
            self.map.insert(b.id, (width, sheet.gen, laid));
        }
        &self.map.get(&b.id).unwrap().2
    }

    /// Layouts computed so far (every miss). A repeat walk over a warm
    /// cache at one width + generation adds none.
    pub fn misses(&self) -> u64 {
        self.misses
    }

    pub fn evict_missing(&mut self, live: &dyn Fn(u64) -> bool) {
        self.map.retain(|id, _| live(*id));
    }

    /// Drop everything (a width change: every entry is stale by key).
    pub fn clear(&mut self) {
        self.map.clear();
    }

    pub fn len(&self) -> usize {
        self.map.len()
    }

    pub fn is_empty(&self) -> bool {
        self.map.is_empty()
    }
}

impl Default for LayoutCache {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transcript::{
        Transcript, DEFAULT_MAX_BLOCKS, DEFAULT_MAX_COST, DEFAULT_MAX_LINES_PER_BLOCK, HDR_TITLE,
    };
    use alloc::string::String;
    use alloc::vec::Vec;
    use beacon::wire::{self, Op as BOp};

    fn daylight() -> vt::Palette {
        libhalcyon::theme::daylight_palette()
    }

    fn gs() -> GlyphSource {
        GlyphSource::new_vendored(512)
    }

    fn body_h(_g: &GlyphSource, sheet: &Sheet) -> i32 {
        round_px(sheet.body_px * LH_BODY)
    }

    // HALCYON-TYPE 4.3, the two things the sub-pixel pen must be: LIVE
    // (a pen that lands on whole pixels every time is an inert feature
    // that every test would still pass), and HONEST -- what layout
    // measured is what the executor paints. The second is the one that
    // matters: the executor accumulates the laid whole steps from the
    // segment's x, so if those steps did not reproduce the laid `xs`,
    // every hit-test and every right edge would drift from the ink.
    #[test]
    fn the_sub_pixel_pen_is_live_and_measures_what_it_paints() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        // An ANNOTATED zone: that is what makes the lines Doc rather than
        // Raw, and Doc is what lays in the proportional faces. A raw
        // output zone is mono, whose cells are whole by construction and
        // would have made this witness vacuously green -- which is exactly
        // what it did on the first run.
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"A Lisp Machine's presentations on a ");
        wire::open(&mut buf, BOp::Em, &[("class", "strong")]);
        buf.extend_from_slice(b"Plan 9");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b" shell -- a thing thought gone, brought back.\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let b = t.frozen_blocks().front().expect("the frozen output block");
        let laid = layout_block(b, 600, &sheet, &mut g);

        let mut phases = [0usize; 4];
        let mut laid_total = 0i32;
        let mut n = 0usize;
        for line in laid.lines.iter() {
            for seg in line.segs.iter() {
                // The executor's arithmetic, replayed exactly.
                let mut pen = seg.x;
                for (i, gl) in seg.refs.iter().enumerate() {
                    assert_eq!(pen, seg.xs[i], "glyph {i}: executor pen != laid x");
                    assert!(gl.phase < 4, "phase {} out of range", gl.phase);
                    phases[gl.phase as usize] += 1;
                    pen += gl.advance;
                    laid_total += gl.advance;
                    n += 1;
                }
                assert_eq!(pen, seg.x_end, "the run ends where the segment says");
            }
        }
        assert!(n > 40, "laid {n} glyphs");
        assert!(
            phases[0] < n,
            "every glyph landed on phase 0 -- the sub-pixel pen is inert"
        );
        assert!(
            phases.iter().filter(|&&c| c > 0).count() >= 3,
            "only {:?} of the four phases used",
            phases
        );

        // Honest about width: the sub-pixel pen tracks the font's exact
        // total at least as closely as the old whole-pixel pen, which
        // re-rounded at EVERY glyph and drifted by up to half a pixel each
        // time. Same text, three measures.
        let mut exact = 0.0f32;
        let mut integer_pen = 0i32;
        for line in laid.lines.iter() {
            for seg in line.segs.iter() {
                for gl in seg.refs.iter() {
                    exact += g.advance_f(seg.face, seg.px, gl.ch).unwrap_or(0.0);
                    integer_pen += g.advance(seg.face, seg.px, gl.ch).unwrap_or(0);
                }
            }
        }
        let new_err = (laid_total as f32 - exact).abs();
        let old_err = (integer_pen as f32 - exact).abs();
        assert!(
            new_err <= old_err,
            "sub-pixel err {new_err} should not exceed whole-pixel err {old_err} (exact {exact})"
        );
        assert!(new_err <= 1.0, "sub-pixel width within a pixel of exact: {new_err}");
    }

    #[test]
    fn laying_out_packs_nothing_and_painting_packs_only_what_it_paints() {
        // The atlas working set is the PAINTED set: laying a block of 3000
        // distinct codepoints (each a .notdef under its own key -- the shape
        // that grew the store a page per few hundred glyphs) opens no page;
        // painting it does; painting it again inserts nothing new; and an
        // eviction between the two invalidates nothing laid -- the same
        // laid block paints again against the fresh generation.
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        let mut s = String::new();
        for cp in 0x4E00u32..0x4E00 + 3000 {
            s.push(char::from_u32(cp).unwrap());
        }
        buf.extend_from_slice(s.as_bytes());
        buf.extend_from_slice(b"\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let b = t.frozen_blocks().front().expect("the frozen output block");
        let laid = layout_block(b, 600, &sheet, &mut g);
        let n: usize = laid.lines.iter().flat_map(|l| l.segs.iter()).map(|s| s.refs.len()).sum();
        assert_eq!(n, 3000, "every codepoint laid");
        assert_eq!(g.packer.store.pages.len(), 0, "layout packed nothing");
        assert!(g.packer.store.glyphs.is_empty());
        let mut cart = Cartoon::new();
        render_block(&mut cart, &laid, 0, &mut g);
        let painted = g.packer.store.glyphs.len();
        assert_eq!(painted, 3000, "painting packed every laid glyph once");
        assert!(g.packer.store.pages.len() > 0);
        assert_eq!(cart.runs.len(), 3000, "every laid glyph is a run entry");
        cart.reset();
        render_block(&mut cart, &laid, 0, &mut g);
        assert_eq!(g.packer.store.glyphs.len(), painted, "a second paint inserted nothing");
        // Evict between frames: the laid block is untouched and paints
        // against the new generation (no stale id: every run resolves).
        g.regen();
        assert_eq!(g.packer.store.pages.len(), 0);
        cart.reset();
        render_block(&mut cart, &laid, 0, &mut g);
        assert_eq!(g.packer.store.glyphs.len(), painted, "re-resolved the visible set only");
        assert!(
            cart.runs.iter().all(|r| (r.glyph as usize) < g.packer.store.glyphs.len()),
            "every run names a glyph of the CURRENT generation"
        );
        assert!(cart.ops.iter().all(|op| !matches!(op, Op::Glyphs { atlas_gen, .. } if *atlas_gen != g.gen())));
        // The in-frame cap on the real paint path: tiny pages (64 px) hold
        // a few dozen .notdef boxes each, so the cap (24 pages) bites inside
        // ONE paint of this block -- the store stops exactly at the cap, the
        // refused glyphs paint blank (an id no table has) with their laid
        // advance kept, and the line's geometry is the one laid above.
        let mut small = GlyphSource::new_vendored(64);
        let cap = crate::raster::MAX_ATLAS_PAGES + crate::raster::ATLAS_PAGE_SLACK;
        let mut cart2 = Cartoon::new();
        render_block(&mut cart2, &laid, 0, &mut small);
        assert_eq!(small.packer.store.pages.len(), cap, "the frame stopped at the cap");
        let served = small.packer.store.glyphs.len();
        assert!(served > 0 && served < 3000, "some served ({served}), the rest refused");
        assert_eq!(cart2.runs.len(), 3000, "every laid glyph still advances the pen");
        let blank = cart2.runs.iter().filter(|r| r.glyph == u32::MAX).count();
        assert_eq!(blank, 3000 - served, "a refused glyph is a blank run entry, nothing else");
        let laid_again = layout_block(b, 600, &sheet, &mut small);
        assert_eq!(laid_again.height, laid.height, "the geometry never moved with the atlas");
    }

    #[test]
    fn the_laid_cache_survives_an_atlas_eviction_and_follows_width_and_sheet() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"a line of text\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let mut sheet = daylight_sheet(100);
        let mut g = gs();
        let mut cache = LayoutCache::new();
        let b = t.frozen_blocks().front().expect("frozen");
        let h1 = cache.get(b, 300, &sheet, &mut g).height;
        assert_eq!(cache.len(), 1);
        assert_eq!(g.packer.store.pages.len(), 0, "a cached layout packed nothing");
        g.regen();
        let p = cache.get(b, 300, &sheet, &mut g) as *const LaidBlock;
        assert_eq!(cache.len(), 1, "an eviction is not a miss");
        let p2 = cache.get(b, 300, &sheet, &mut g) as *const LaidBlock;
        assert_eq!(p, p2, "the same entry served");
        assert_eq!(cache.get(b, 300, &sheet, &mut g).height, h1);
        // A width change is a miss (a re-lay), a sheet change too.
        let _ = cache.get(b, 120, &sheet, &mut g);
        assert_eq!(cache.len(), 1, "replaced, not accumulated");
        sheet.gen += 1;
        let _ = cache.get(b, 120, &sheet, &mut g);
        cache.evict_missing(&|_| false);
        assert!(cache.is_empty());
    }

    #[test]
    fn face_rule_plain_and_obj_are_both_body() {
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
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let laid = layout_block(b, 600, &sheet, &mut g);
        let line = &laid.lines[0];
        assert!(line.segs.len() >= 2);
        assert_eq!(
            line.segs[0].face, FACE_BODY,
            "plain output in a Beacon-structured block is proportional body (14.13)"
        );
        assert_eq!(
            line.segs[1].face, FACE_BODY,
            "the obj span is body too -- an object is colour + hit, not a face change"
        );
        assert_eq!(
            line.segs[1].color, sheet.obj,
            "obj at default ink takes the slate object colour"
        );
        assert!(line.segs[1].obj > 0, "the obj hit target rides the seg");
        assert_eq!(line.segs[1].chrome, CHROME_OBJ, "the obj wears the pill");
        // The pill: a ground rect + a hairline stroke around the obj seg.
        let pill = laid
            .rects
            .iter()
            .filter(|r| r.color == sheet.island_ground && r.x == line.segs[1].x - OBJ_PAD)
            .count();
        assert_eq!(pill, 1, "one pill ground under the obj");
        assert!(
            laid.rects.iter().filter(|r| r.color == sheet.rule).count() >= 4,
            "the pill's four hairline strokes"
        );
        assert_eq!(line.h, body_h(&g, &sheet), "the prose line box is the 1.5 line-height");
    }

    #[test]
    fn genera_headings_and_emph_are_italic_strong_is_bold() {
        // The Genera type discipline (HALCYON.md section 3 + HALCYON-VISUAL 8):
        // headings and emphasis go ITALIC, never bold; bold is reserved for
        // strong (extreme emphasis) + foreign SGR bold on an annotated run.
        // Headings take the DISTINCT Regular-weight italic (FACE_HEADING_ITALIC,
        // operator's baseline=Text / bigger=Regular rule); RANK is size (px_for),
        // not weight. Plain output is the proportional body (14.13).
        let base = Style { fg: 0, bg: 0, attrs: 0, em: 0, obj: 0, hdr: 0 };
        let with = |em: u8, hdr: u8, attrs: u8| Style { em, hdr, attrs, ..base };

        assert_eq!(face_for(&with(0, 1, 0), false), FACE_HEADING_ITALIC, "hdr 1 is the regular-weight italic, never bold");
        assert_eq!(face_for(&with(0, 3, 0), false), FACE_HEADING_ITALIC, "hdr 3 is the regular-weight italic too");
        assert_eq!(face_for(&with(EM_EMPH, 0, 0), false), FACE_BODY_ITALIC, "emph is the Text-weight italic");
        assert_eq!(face_for(&with(EM_STRONG, 0, 0), false), FACE_BODY_BOLD, "strong is the reserved bold");
        // A plain (un-annotated) cell is the proportional body (14.13's
        // proportional-live model). A foreign SGR bold on it does NOT promote
        // to the reserved bold -- that is em-strong's alone (8.2); the SGR-bold
        // path fires only on an ANNOTATED run (a table cell below).
        assert_eq!(face_for(&with(0, 0, ATTR_BOLD), false), FACE_BODY, "plain SGR bold stays body -- foreign bold is not the em-strong bold");
        assert_eq!(face_for(&with(0, 0, ATTR_BOLD), true), FACE_BODY_BOLD, "annotated + foreign SGR bold -> the reserved bold");
        assert_eq!(face_for(&with(EM_CODE, 0, 0), false), FACE_MONO, "inline code is the only mono case here");
        assert_eq!(face_for(&base, false), FACE_BODY, "plain ordinary output is proportional body (14.13)");
        assert_eq!(face_for(&Style { obj: 1, ..base }, false), FACE_BODY, "an obj presentation is regular body");
        // px_for still carries heading rank by SIZE (unchanged), so italic
        // headings are not flattened to one size; a title heading keeps its
        // level's size (the flag rides the high bits).
        let sheet = daylight_sheet(100);
        assert!(px_for(&with(0, 1, 0), sheet.body_px, &sheet) > px_for(&with(0, 3, 0), sheet.body_px, &sheet), "hdr 1 > hdr 3 by size");
        assert!(px_for(&with(0, 3, 0), sheet.body_px, &sheet) > px_for(&base, sheet.body_px, &sheet), "any heading > body by size");
        assert_eq!(px_for(&with(0, 1 | HDR_TITLE, 0), sheet.body_px, &sheet), px_for(&with(0, 1, 0), sheet.body_px, &sheet));
    }

    #[test]
    fn mixed_line_uses_body_box_pre_line_uses_mono_box() {
        // A `pre` block's lines are all-mono (the pre flag forces FACE_MONO), so
        // they keep the mono row pitch (the island cell centred in the
        // `.hal-out` line box); a mixed proportional line takes the body box
        // (13.5 + COMPOSITION 4: the island never stretches it).
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Pre, &[]);
        buf.extend_from_slice(b"pure mono line\n");
        wire::close(&mut buf, BOp::Pre);
        wire::open(&mut buf, BOp::Em, &[("class", "strong")]);
        buf.extend_from_slice(b"mixed");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b" line with ");
        wire::open(&mut buf, BOp::Em, &[("class", "code")]);
        buf.extend_from_slice(b"code");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b"\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let b = &t.frozen_blocks()[0];
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let (_, cell_h, _) = g.island_cell();
        let laid = layout_block(b, 600, &sheet, &mut g);
        assert_eq!(laid.lines[0].h, PRE_LINE_H.max(cell_h), "the pre line keeps the mono row pitch");
        assert!(laid.lines[0].segs.iter().all(|s| s.face == FACE_MONO));
        assert_eq!(
            laid.lines[0].segs[0].refs[0].advance,
            g.island_cell().0,
            "the pre's mono is the ISLAND cell (advance 6), not the grid"
        );
        assert_eq!(laid.lines[1].h, body_h(&g, &sheet), "the mixed proportional line takes the body box");
        let code = laid.lines[1].segs.iter().find(|s| s.face == FACE_MONO).expect("the code island");
        assert_eq!(code.chrome, CHROME_CODE, "the inline code wears its ground");
        assert_eq!(code.refs[0].advance, g.island_cell().0);
    }

    #[test]
    fn raw_output_is_a_mono_island_prompt_and_doc_are_proportional() {
        // The sc3 ruling: a program's un-annotated output is terminal content
        // -- the mono island with the `.hal-out` chrome; the shell's prompt
        // zone and any Beacon-structured block stay proportional.
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "prompt")]);
        buf.extend_from_slice(b"~ > cat notes\n");
        wire::close(&mut buf, BOp::Zone);
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"col1   col2\nabc    def\n");
        wire::close(&mut buf, BOp::Zone);
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"prose with an ");
        wire::open(&mut buf, BOp::Em, &[("class", "emph")]);
        buf.extend_from_slice(b"emphasis");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b"\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let blocks = t.frozen_blocks();
        let prompt = layout_block(&blocks[0], 600, &sheet, &mut g);
        assert_eq!(blocks[0].class(), LineClass::Prompt);
        assert!(prompt.lines[0].segs.iter().all(|s| s.face == FACE_BODY && s.px == sheet.prompt_px), "the prompt is proportional at the prompt size");
        let raw = layout_block(&blocks[1], 600, &sheet, &mut g);
        assert_eq!(blocks[1].class(), LineClass::Raw);
        assert_eq!(raw.lines.len(), 2);
        assert!(raw.lines.iter().all(|l| l.segs.iter().all(|s| s.face == FACE_MONO)), "raw output is mono");
        assert_eq!(raw.lines[0].segs[0].color, sheet.dim, "raw ink is the dim step");
        let (iw, _, _) = g.island_cell();
        assert_eq!(raw.lines[0].segs[0].refs[0].advance, iw, "the island cell, not the grid");
        assert_eq!(raw.lines[0].segs[0].x, sheet.pad_x + ISLAND_RULE_W + ISLAND_PAD_X, "inset past the gutter");
        // ONE island: a ground rect + a gutter spanning both lines.
        let ground: Vec<&RectSpec> = raw.rects.iter().filter(|r| r.color == sheet.island_ground).collect();
        assert_eq!(ground.len(), 1, "one ground for the run of raw lines");
        assert_eq!(ground[0].h as i32, raw.lines[1].y + raw.lines[1].h + ISLAND_PAD_Y - ground[0].y);
        assert!(raw.rects.iter().any(|r| r.color == sheet.island_rule && r.w == ISLAND_RULE_W as u32), "the gutter rule");
        let doc = layout_block(&blocks[2], 600, &sheet, &mut g);
        assert_eq!(blocks[2].class(), LineClass::Doc);
        assert!(doc.lines[0].segs.iter().all(|s| s.face != FACE_MONO), "a block with any annotation is a document: proportional");
        assert_eq!(doc.lines[0].segs[0].px, sheet.body_px);
    }

    #[test]
    fn raw_lines_wrap_at_the_character_like_a_terminal() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"abcdefghijklmnopqrstuvwxyz0123456789abcdefghijklmnopqrstuvwxyz\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let (iw, _, _) = g.island_cell();
        // 62 cells at advance 6 = 372 px of text; a 240-px tile must wrap it.
        let laid = layout_block(&t.frozen_blocks()[0], 240, &sheet, &mut g);
        assert!(laid.lines.len() >= 2, "wrapped: {} lines", laid.lines.len());
        let total: usize = laid.lines.iter().flat_map(|l| l.segs.iter()).map(|s| s.refs.len()).sum();
        assert_eq!(total, 62, "no character lost");
        for l in laid.lines.iter() {
            for s in l.segs.iter() {
                assert!(s.x_end <= 240 - sheet.pad_x, "stays inside the width");
                assert_eq!(s.refs[0].advance, iw);
            }
        }
    }

    #[test]
    fn vertical_rhythm_collapses_margins() {
        // COMPOSITION 3 under CSS collapsing: hdr2 (top 8) after prose (bottom
        // 2) opens 8; prose after a heading opens 2 (the heading's bottom);
        // prose after prose opens 2 (2 and 2 collapse); the first element
        // opens 0; a rule opens 8 either side.
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Hdr, &[("level", "2")]);
        buf.extend_from_slice(b"Section");
        wire::close(&mut buf, BOp::Hdr);
        buf.extend_from_slice(b"\nfirst para\nsecond para\n");
        wire::point(&mut buf, BOp::Rule, &[]);
        buf.extend_from_slice(b"after the rule\n");
        wire::open(&mut buf, BOp::Hdr, &[("level", "3")]);
        buf.extend_from_slice(b"Sub");
        wire::close(&mut buf, BOp::Hdr);
        buf.extend_from_slice(b"\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let laid = layout_block(&t.frozen_blocks()[0], 600, &sheet, &mut g);
        let ls = &laid.lines;
        assert_eq!(ls[0].y, 0, "the first element opens with no top margin");
        let hdr2_h = round_px(14.5 * LH_HDR);
        assert_eq!(ls[0].h, hdr2_h, "hdr 2 line box is 14.5 x 1.25");
        assert_eq!(ls[1].y, ls[0].y + ls[0].h + HDR_BOTTOM, "prose sits 2 under its heading");
        assert_eq!(ls[2].y, ls[1].y + ls[1].h + PROSE_MARGIN, "prose after prose: 2 (collapsed)");
        // The rule: 8 after the para, the 1px line, 8 before the next para.
        let rule = laid.rects.iter().find(|r| r.color == sheet.rule && r.h == 1 && r.x == sheet.pad_x).expect("the rule");
        assert_eq!(rule.y, ls[2].y + ls[2].h + RULE_MARGIN);
        assert_eq!(ls[3].y, rule.y + 1 + RULE_MARGIN);
        assert_eq!(ls[4].y, ls[3].y + ls[3].h + HDR_TOP[2], "hdr 3 opens 6 (> the para's 2)");
        assert_eq!(laid.height, ls[4].y + ls[4].h, "the last bottom margin is dropped");
    }

    #[test]
    fn herald_title_and_deck_are_centred_and_tightened() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Hdr, &[("level", "1"), ("class", "title")]);
        buf.extend_from_slice(b"Halcyon Terminal");
        wire::close(&mut buf, BOp::Hdr);
        buf.extend_from_slice(b"\n");
        wire::open(&mut buf, BOp::Em, &[("class", "dim")]);
        buf.extend_from_slice(b"Booted on aarch64");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b"\n");
        wire::open(&mut buf, BOp::Em, &[("class", "dim")]);
        buf.extend_from_slice(b"4 cpus");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b"\nGetting started\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let w = 600;
        let laid = layout_block(&t.frozen_blocks()[0], w, &sheet, &mut g);
        let ls = &laid.lines;
        // The title overrides the first-child reset with its own top margin.
        assert_eq!(ls[0].y, HDR_TOP[0], "the herald keeps its 10px top");
        // Centred: symmetric slack around the content.
        let centred = |l: &LaidLine| {
            let x0 = l.segs.first().unwrap().x;
            let x1 = l.segs.last().unwrap().x_end;
            let slack_l = x0 - sheet.pad_x;
            let slack_r = (w - sheet.pad_x) - x1;
            (slack_l - slack_r).abs() <= 1 && slack_l > 20
        };
        assert!(centred(&ls[0]), "the title is centred");
        assert!(centred(&ls[1]) && centred(&ls[2]), "the deck lines are centred");
        assert!(!centred(&ls[3]), "the prose after the deck is left-aligned");
        assert_eq!(ls[3].segs[0].x, sheet.pad_x);
        // Tightened: 4 under the title (collapsed with its 2), 0 between the
        // pair, 8 after it.
        assert_eq!(ls[1].y, ls[0].y + ls[0].h + DECK_TOP);
        assert_eq!(ls[2].y, ls[1].y + ls[1].h);
        assert_eq!(ls[3].y, ls[2].y + ls[2].h + DECK_BOTTOM);
        assert_eq!(ls[1].segs[0].color, sheet.dim, "the deck is dim");
        // The hit map moved with the glyphs.
        assert_eq!(ls[0].segs[0].xs[0], ls[0].segs[0].x);
    }

    #[test]
    fn kv_list_spreads_its_groups_over_the_width() {
        // `table cols=lrlr` without a header is the two-column list: two
        // name/value groups sharing the width, values right-aligned at each
        // group's right edge, 3px between rows, 4/6 around.
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"intro\n");
        wire::open(&mut buf, BOp::Table, &[("cols", "lrlr"), ("hdr", "0")]);
        for (a, bb, c, d) in [("kernel", "1", "utopia", "22"), ("loom", "333", "halcyon", "4")] {
            wire::open(&mut buf, BOp::Row, &[]);
            for cell in [a, bb, c, d] {
                wire::open(&mut buf, BOp::Cell, &[]);
                buf.extend_from_slice(cell.as_bytes());
                wire::close(&mut buf, BOp::Cell);
            }
            wire::close(&mut buf, BOp::Row);
            buf.extend_from_slice(b"\n");
        }
        wire::close(&mut buf, BOp::Table);
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let w = 640;
        let laid = layout_block(&t.frozen_blocks()[0], w, &sheet, &mut g);
        let ls = &laid.lines;
        assert_eq!(ls.len(), 3);
        let groups = 2;
        let group_w = (w - 2 * sheet.pad_x - (groups - 1) * sheet.kv_col_gap) / groups;
        for row in [&ls[1], &ls[2]] {
            assert_eq!(row.segs.len(), 4);
            assert_eq!(row.segs[0].x, sheet.pad_x, "name at the group's left");
            assert_eq!(row.segs[1].x_end, sheet.pad_x + group_w, "value right-aligned at the group's right");
            assert_eq!(row.segs[2].x, sheet.pad_x + group_w + sheet.kv_col_gap, "second group past the gap");
            assert_eq!(row.segs[3].x_end, w - sheet.pad_x, "last value at the right inset");
        }
        assert_eq!(ls[1].y, ls[0].y + ls[0].h + TABLE_TOP, "4 above the list (> prose's 2)");
        assert_eq!(ls[2].y, ls[1].y + ls[1].h + TABLE_ROW_GAP, "3 between rows");
        assert_eq!(laid.height, ls[2].y + ls[2].h, "no trailing margin");
    }

    #[test]
    fn wrap_reflows_deterministically() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Em, &[("class", "emph")]);
        buf.extend_from_slice(
            b"the quick brown fox jumps over the lazy dog and keeps going yet further\n",
        );
        wire::close(&mut buf, BOp::Em);
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let b = &t.frozen_blocks()[0];
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let wide = layout_block(b, 600, &sheet, &mut g);
        let narrow = layout_block(b, 220, &sheet, &mut g);
        let narrow2 = layout_block(b, 220, &sheet, &mut g);
        assert!(narrow.lines.len() > wide.lines.len(), "narrow wraps more");
        assert_eq!(
            narrow.lines.len(),
            narrow2.lines.len(),
            "same width, same shape"
        );
        assert_eq!(narrow.height, narrow2.height);
        // Every glyph stays inside the width.
        for l in narrow.lines.iter() {
            for s in l.segs.iter() {
                assert!(s.x_end <= 220, "seg spills: {}", s.x_end);
            }
        }
        // No content lost: total glyph count matches.
        let count = |lb: &LaidBlock| {
            lb.lines
                .iter()
                .flat_map(|l| l.segs.iter())
                .map(|s| s.refs.len())
                .sum::<usize>()
        };
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
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let laid = layout_block(b, 600, &sheet, &mut g);
        assert_eq!(laid.lines.len(), 3, "three table rows lay as three lines");
        // The right-aligned column: row ends align.
        let end = |l: &LaidLine| l.segs.last().map(|s| s.x_end).unwrap_or(0);
        let e1 = end(&laid.lines[1]);
        let e2 = end(&laid.lines[2]);
        assert_eq!(e1, e2, "r-aligned column shares the right edge");
        assert!(!laid.rects.is_empty(), "the header rule painted");
        // A headed table is the compact table, never the spread list.
        assert!(e1 < 600 - sheet.pad_x, "compact: the value column hugs its content");
    }

    #[test]
    fn exit_badge_only_on_failure() {
        let sheet = daylight_sheet(100);
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
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"did things\n");
        wire::point(
            &mut buf,
            BOp::Mark,
            &[("k", "exit"), ("code", "-9223372036854775808")],
        );
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        // Must not panic; the failure badge renders (nonzero exit).
        let laid = layout_block(&t.frozen_blocks()[0], 400, &sheet, &mut g);
        let has_err_seg = laid
            .lines
            .iter()
            .flat_map(|l| l.segs.iter())
            .any(|s| s.color == sheet.err);
        assert!(
            has_err_seg,
            "the i64::MIN exit still renders a failure badge"
        );
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
        let sheet = daylight_sheet(100);
        let mut g = gs();
        let mut cart = Cartoon::new();
        cart.ops.push(Op::Clear {
            color: sheet.ground,
        });
        let mut y = 4;
        for b in t.frozen_blocks().iter() {
            let laid = layout_block(b, 300, &sheet, &mut g);
            render_block(&mut cart, &laid, y, &mut g);
            y += laid.height + sheet.block_gap;
        }
        let w = 300usize;
        let h = (y + 4) as usize;
        let mut px = alloc::vec![0u32; w * h];
        cartoon::execute(
            &cart,
            &g.packer.store,
            &cartoon::BlobStore::new(),
            &mut px,
            w,
            None,
        );
        let ink = px.iter().filter(|&&p| p != sheet.ground).count();
        assert!(ink > 100, "the session inked {} px", ink);
    }

    // HALCYON-SCALE 6/7: the sheet at 100 is the logical table exactly
    // (nothing at 1.0 moves), and at 200 it is the operator's worked table
    // (COMPOSITION 6) -- every size through the one round-half-up.
    #[test]
    fn the_sheet_at_100_is_the_logical_table_and_at_200_the_operators() {
        let s = daylight_sheet(100);
        assert_eq!(s.scale, 100);
        assert!(s.metrics == libhalcyon::theme::METRICS);
        assert_eq!((s.hairline, s.mark_w), (1, 2));
        assert_eq!((s.body_px, s.prompt_px), (11.5, 11.5), "the prompt at the base size (the operator, 2026-09-08)");
        assert_eq!(s.hdr_px, [17.5, 14.5, 12.5]);
        assert_eq!((s.mono_island_px, s.mono_grid_px), (crate::raster::MONO_ISLAND_PX, crate::raster::MONO_GRID_PX));
        assert_eq!((s.pad_x, s.block_gap, s.table_col_gap, s.kv_col_gap), (12, 6, 16, 28));
        assert_eq!((s.ipx(16), s.ipx(3), s.px(10.5)), (16, 3, 10.5), "the identity");
        let d = daylight_sheet(200);
        assert_eq!(d.scale, 200);
        assert_eq!((d.metrics.header_h, d.metrics.status_h, d.metrics.tag_pad_x), (40, 40, 12));
        assert_eq!((d.hairline, d.mark_w), (2, 4));
        assert_eq!((d.body_px, d.prompt_px), (23.0, 23.0), "prose 23 at 2.0 (COMPOSITION 6); the prompt with it");
        assert_eq!(d.hdr_px, [35.0, 29.0, 25.0], "35 / 29 / 25 at 2.0");
        assert_eq!((d.mono_island_px, d.mono_grid_px), (24.0, 24.0), "twice the 12 advance: one mono size, the grid at the island's");
        assert_eq!((d.pad_x, d.block_gap, d.table_col_gap, d.kv_col_gap), (24, 12, 32, 56));
        let m = daylight_sheet(150);
        assert_eq!((m.body_px, m.hdr_px[0], m.hairline, m.metrics.header_h), (17.25, 26.25, 2, 30));
        assert_eq!((m.mono_island_px, m.mono_grid_px), (18.0, 18.0), "advance 9, both");
        assert_eq!(m.ipx(5), 8, "7.5 rounds up");
        assert_eq!(m.ipx(3), 5, "4.5 rounds up");
        let q = daylight_sheet(125);
        assert_eq!(q.ipx(1), 1, "1.25 rounds down; the hairline floor holds");
        assert_eq!(q.hairline, 1);
        assert_eq!(q.mark_w, 3, "2.5 rounds up");
    }

    // A block laid at 200% is the 100% block scaled: the line boxes are the
    // scaled sizes' line-heights, the margins double, the island's inset
    // and cell are the 200% bake's, the rules are 2 px hairlines -- and the
    // same source lays to the same glyph count either way.
    #[test]
    fn a_warm_layout_cache_lays_nothing_on_a_repeat_walk() {
        // The scale round's F1: a size-triggered reset (`> 512` entries)
        // re-laid the WHOLE transcript every frame once it held more blocks
        // than the threshold -- the console walks every frozen block per
        // frame, so a long session paid O(history) per keystroke (measured
        // by the prosecutor: 600 blocks, misses per pass [600, 600, 600]).
        // The bound is the live set, never a reset.
        let mut t = Transcript::with_caps(daylight(), 1000, usize::MAX, 8);
        let mut buf = Vec::new();
        for i in 0..600 {
            wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
            buf.extend_from_slice(alloc::format!("line {}\n", i).as_bytes());
            wire::close(&mut buf, BOp::Zone);
        }
        t.feed(&buf);
        let blocks = t.frozen_blocks();
        assert_eq!(blocks.len(), 600);
        let mut g = gs();
        let s = daylight_sheet(100);
        let mut cache = LayoutCache::new();
        let mut misses = Vec::new();
        for _ in 0..3 {
            let before = cache.misses();
            for b in blocks.iter() {
                let _ = cache.get(b, 600, &s, &mut g);
            }
            misses.push(cache.misses() - before);
        }
        assert_eq!(misses, alloc::vec![600, 0, 0], "a warm cache lays nothing on a repeat walk");
        assert_eq!(cache.len(), 600, "one entry per live block, none reset away");
    }

    #[test]
    fn a_block_laid_at_200_is_the_operators_table_in_pixels() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        wire::open(&mut buf, BOp::Hdr, &[("level", "1")]);
        buf.extend_from_slice(b"Title");
        wire::close(&mut buf, BOp::Hdr);
        buf.extend_from_slice(b"\nprose line\n");
        wire::point(&mut buf, BOp::Rule, &[]);
        buf.extend_from_slice(b"after\n");
        wire::close(&mut buf, BOp::Zone);
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"raw terminal bytes\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        let blocks = t.frozen_blocks();
        let mut g1 = gs();
        let s1 = daylight_sheet(100);
        let a = layout_block(&blocks[0], 600, &s1, &mut g1);
        let mut g2 = gs();
        g2.set_scale(200);
        let s2 = daylight_sheet(200);
        let b = layout_block(&blocks[0], 1200, &s2, &mut g2);
        assert_eq!(a.lines.len(), b.lines.len(), "the same lines either way");
        assert_eq!(a.lines[0].h, round_px(17.5 * LH_HDR), "hdr 1 at 1.0: 22");
        assert_eq!(b.lines[0].h, round_px(35.0 * LH_HDR), "hdr 1 at 2.0: 44");
        assert_eq!(b.lines[1].h, round_px(23.0 * LH_BODY), "prose at 2.0: 35");
        assert_eq!(b.lines[1].y, b.lines[0].y + b.lines[0].h + 2 * HDR_BOTTOM, "the heading's bottom margin doubled");
        let rule = b.rects.iter().find(|r| r.color == s2.rule).expect("the rule");
        assert_eq!(rule.h, 2, "a 2 px hairline at 2.0");
        assert_eq!(rule.x, 24, "at the doubled inset");
        assert_eq!(rule.y, b.lines[1].y + b.lines[1].h + 2 * RULE_MARGIN);
        assert_eq!(b.lines[2].y, rule.y + 2 + 2 * RULE_MARGIN, "the rule's height and margin both scaled");
        assert!(b.lines[0].segs[0].px == 35.0 && b.lines[1].segs[0].px == 23.0);
        let count = |lb: &LaidBlock| lb.lines.iter().flat_map(|l| l.segs.iter()).map(|s| s.refs.len()).sum::<usize>();
        assert_eq!(count(&a), count(&b));
        // The raw island: the 200% island bake (12x27) inset past the
        // doubled gutter (2) and pad (8) from the doubled text inset (12).
        let raw = layout_block(&blocks[1], 1200, &s2, &mut g2);
        assert_eq!(raw.lines[0].segs[0].refs[0].advance, 12, "the advance-12 bake");
        assert_eq!(raw.lines[0].segs[0].x, 24 + 4 + 16);
        assert_eq!(raw.lines[0].h, 32.max(g2.island_cell().1), "the mono pitch doubled (32) holds the 27-px cell");
        let gutter = raw.rects.iter().find(|r| r.color == s2.island_rule).expect("the gutter");
        assert_eq!(gutter.w, 4, "the 2 px gutter rule doubled");
        assert_eq!(g2.island_cell(), (12, 27, g2.island_cell().2));
        // Nothing at 1.0 moved: the 100% raw island is the advance-6 bake
        // at the 1.0 inset.
        let raw1 = layout_block(&blocks[1], 600, &s1, &mut g1);
        assert_eq!(raw1.lines[0].segs[0].refs[0].advance, 6);
        assert_eq!(raw1.lines[0].segs[0].x, 12 + 2 + 8);
    }
}
