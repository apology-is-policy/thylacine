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

use libhalcyon::instrument::Profile;
use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use vt::{ATTR_BOLD, ATTR_ITALIC};

use crate::raster::{
    is_cell_face, is_mono_face, mono_advances, GlyphSource, FACE_BODY, FACE_BODY_BOLD, FACE_BODY_ITALIC, FACE_HEADING_ITALIC,
    FACE_MONO, FACE_MONO_ITALIC, FACE_MONO_TEXT, FACE_SANS, FACE_SANS_MEDIUM, FACE_SANS_SEMIBOLD,
};
use crate::transcript::{
    hdr_is_title, hdr_level, Block, BlockKind, Item, LineClass, Style, TCell, EM_CODE, EM_DIM,
    EM_EMPH, EM_STRONG,
};
use libhalcyon::theme::Metrics;

/// How the document's vertical flow accumulates (HALCYON-INSTRUMENT 7.5 as
/// built at I-5b). The legacy composition rounds EACH line box to whole
/// pixels (17 for 11.5 x 1.5) and stacks the integers; the Instrument
/// document is the browser's flow: fractional line boxes (24.3 for 15 x
/// 1.62) accumulated in 1/64 px -- Blink's LayoutUnit, truncated where it
/// truncates -- each line's top rounded to a row at paint, the glyphs at
/// the line top plus an INTEGER half-leading (the floor). Keyed on the
/// profile because the two are not one rule at different numbers: the
/// legacy gates are byte-pinned on the per-line rounding.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Flow {
    PerLine,
    Fractional,
}

/// The document's vertical rhythm, LOGICAL px (HALCYON-INSTRUMENT 7.5 under
/// Instrument; HALCYON-COMPOSITION 3 under legacy): the margins each role
/// opens, collapsed pairwise to the larger (`Role::margins`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Rhythm {
    /// A prose line's margins: the legacy 2 on every side; the paragraph
    /// margin under Instrument, where consecutive prose lines are ONE
    /// paragraph (the flow opens nothing between them -- `layout_block`)
    /// and the EMPTY line is the break.
    pub prose: i32,
    /// An empty document line's margins: 0 under legacy (a full line box);
    /// the paragraph margin under Instrument, where the line is a
    /// zero-height break that collapses THROUGH (two empties open one gap).
    pub empty: i32,
    pub hdr_top: [i32; 3],
    pub hdr_bottom: [i32; 3],
    pub pre: i32,
    pub raw: i32,
    pub table_top: i32,
    pub table_bottom: i32,
    pub rule: i32,
    pub prompt_bottom: i32,
    pub deck_top: i32,
    pub deck_bottom: i32,
}

/// The stylesheet: the paper-light theme's numbers (section 3 -- dark ink
/// in full daylight) AT A DISPLAY SCALE (HALCYON-SCALE 6): every size below
/// is PHYSICAL px, derived once from its logical value by `daylight_sheet`,
/// and every logical constant in this module reaches a pixel only through
/// `px` / `ipx` -- the ONE place halcyond scales. Colors are ARGB like
/// everything in the weave.
#[derive(Clone, Copy)]
pub struct Sheet {
    /// The RESOLVED theme this sheet was built from (HALCYON-THEME 3.2).
    /// Every painter already takes a `&Sheet`, so carrying the theme here is
    /// what lets the chrome helpers reach a token without naming a constant.
    /// The fields below stay as they are: they are the sheet's own
    /// vocabulary, derived once, and a painter that wants `ground` should not
    /// have to know it is the theme's `surface`.
    pub theme: libhalcyon::theme::Theme,
    pub ground: u32, // the theme's surface
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
    /// The profile the sheet was built for (HALCYON-INSTRUMENT 4): what
    /// `metrics` below is the table of, and what a rescale rebuilds under.
    pub profile: Profile,
    /// The resolved bundle's Instrument palette (HALCYON-INSTRUMENT 4.4):
    /// the authored 35 under an Instrument theme, the projection under a
    /// legacy one. The Instrument painters (the header, the placard, the
    /// tile states, the menu) read their tokens here; the legacy painters
    /// keep reading `theme`.
    pub inst: libhalcyon::instrument::InstrumentTheme,
    /// The derived opaques (7.3), resolved once with the bundle.
    pub derived: libhalcyon::instrument::Derived,
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
    /// logical under legacy; HALCYON-INSTRUMENT 7.2: clamp(23, 2.4 vw, 34)
    /// / 17 / 15 under Instrument), at `scale`.
    pub hdr_px: [f32; 3],
    /// The two mono ems at `scale`: twice the selected bakes' advances
    /// (`raster::mono_advances`; the bake's 0.5-em rule) -- what a mono
    /// span asks the glyph source for, so it lands on the island or the
    /// grid atlas the source selected for the same scale.
    pub mono_island_px: f32,
    pub mono_grid_px: f32,
    /// The document's text inset (7.5 under Instrument: clamp(20, 3 vw, 48)
    /// of the LOGICAL display width; the legacy 12).
    pub pad_x: i32,
    /// The document's top and bottom paddings (7.5: clamp(18, 2.2 vw, 34)
    /// and 50 under Instrument; the legacy block gap and 0).
    pub pad_top: i32,
    pub pad_bottom: i32,
    pub block_gap: i32,
    pub table_col_gap: i32,
    /// The two-column list's gap between its (name, value) groups.
    pub kv_col_gap: i32,
    /// The smoothing stroke on every proportional glyph, thousandths of an
    /// em (`Theme.smooth_mem`, HALCYON-TYPE 4.2): the glyph source is set
    /// to this wherever the sheet is built, so the rasters follow the
    /// theme the sheet was built from.
    pub smooth_mem: u16,
    /// The faces by ROLE for this sheet's profile (HALCYON-INSTRUMENT 7.1
    /// / 7.2 under Instrument; HALCYON-VISUAL 7 under legacy), so a
    /// painter names the role and the profile picks the cut:
    ///
    /// | role         | legacy                 | Instrument            |
    /// |--------------|------------------------|-----------------------|
    /// | `face_body`  | Text 450               | Regular 400           |
    /// | `face_strong`| Bold 700               | Bold 700              |
    /// | `face_emph`  | Text Italic 450        | Regular Italic 400    |
    /// | `face_hdr`   | Regular Italic 400     | Medium 500, roman     |
    /// | `face_medium`| Text 450               | Medium 500            |
    /// | `face_brand` | Text 450               | SemiBold 600          |
    /// | `face_mono_text`   | the island CELL  | Cornucopia free-running|
    /// | `face_mono_italic` | the island CELL (roman) | the Italic CELL |
    /// | `face_code`  | the island CELL        | Cornucopia free-running at 0.86 x body |
    ///
    /// The legacy column is exactly the faces the legacy painters named
    /// as constants before I-5 (byte-identical by construction); the
    /// Instrument column is the type map's.
    pub face_body: u8,
    pub face_strong: u8,
    pub face_emph: u8,
    pub face_hdr: u8,
    pub face_medium: u8,
    pub face_brand: u8,
    pub face_mono_text: u8,
    pub face_mono_italic: u8,
    pub face_code: u8,
    /// The inline code span's size at `scale`: the island em under legacy
    /// (the cell), 0.86 x the body under Instrument (7.2; it inherits the
    /// body's line box and never grows it).
    pub code_px: f32,
    /// The chrome's mono sizes at `scale` (7.2): the header's index and
    /// metadata, the footer and a menu hint at 10; the clock at 11 --
    /// through `face_mono_text`. Under legacy both are the island em (the
    /// legacy chrome has no such roles; the values keep a painter on the
    /// cell).
    pub chrome_mono_px: f32,
    pub clock_px: f32,
    /// The document's composition (7.5 as built at I-5b): how the flow
    /// accumulates, the line-height FACTORS by role (the body, the three
    /// heading ranks, the `pre` row, the terminal row), and the rhythm.
    pub flow: Flow,
    pub lh_body: f32,
    pub lh_hdr: [f32; 3],
    pub lh_pre: f32,
    pub lh_raw: f32,
    /// I-5d: whether the glyph source kerns proportional runs (GPOS pair
    /// adjustments) -- the Instrument profile; legacy stays at 0, byte for
    /// byte. The owner hands it to `GlyphSource::set_kerning` beside the
    /// smoothing.
    pub kerning: bool,
    /// The heading ranks' letter-spacing in em (7.2: the H1's -0.025 em,
    /// the H2 and H3 none), added to every glyph's advance of a heading
    /// run, the last included (the CSS rule). Zero under legacy.
    pub hdr_track: [f32; 3],
    /// 7.5: a space at a line's end HANGS past the measure the way CSS
    /// collapses it (the browser never wraps a word because the space after
    /// it would not fit); legacy keeps wrapping the word before it, byte
    /// for byte.
    pub hang_spaces: bool,
    pub rhythm: Rhythm,
    /// The measure a block wraps at, px (7.5: H1, prose and `pre` cap at
    /// 720 under Instrument -- each block, not a centred column; H2 and a
    /// table are uncapped); `NO_CAP` under legacy.
    pub measure_cap: i32,
    /// The two islands' geometry at `scale` (7.3 / 7.5 / 7.6 under
    /// Instrument: the code block pads 15 / 17 inside a 2 px leading rule,
    /// the terminal view pads 14 / 16 with no rule; the one `.hal-out`
    /// island under legacy: 2 / 8 inside a 2 px rule, no right padding).
    pub pre_pad_y: i32,
    pub pre_pad_x: i32,
    pub pre_pad_r: i32,
    pub pre_rule_w: i32,
    pub raw_pad_y: i32,
    pub raw_pad_x: i32,
    pub raw_pad_r: i32,
    pub raw_rule_w: i32,
    /// The inline code span's horizontal padding (`.hal-em--code`'s 3
    /// under legacy; 0 under Instrument, where the span carries no ground).
    pub code_pad: i32,
    /// The islands' grounds and rules (7.3): `code_bg` + `amber_muted` for
    /// a `pre`, `terminal_bg` for raw output under Instrument; the legacy
    /// `header` + `island_rule` for both. The inline code span's ground is
    /// `header` under legacy and none under Instrument.
    pub ground_pre: u32,
    pub rule_pre: u32,
    pub ground_raw: u32,
    pub rule_raw: u32,
    pub ground_code: Option<u32>,
    /// The DEFAULT inks by role (7.3): what a cell that chose no colour
    /// paints in. Under legacy each is the pen's default (`[terminal] fg`,
    /// which is what the cell already carries, so substituting it is the
    /// identity) except the two dim steps (`fg_dim`); under Instrument
    /// prose is `body_text`, headings and strong and the prompt's input
    /// `text`, inline code `code_text`, a `pre` `code_body`, raw output
    /// `terminal_text`, commentary `dim`.
    pub ink_prose: u32,
    pub ink_prompt: u32,
    pub ink_hdr: u32,
    pub ink_strong: u32,
    pub ink_code: u32,
    pub ink_pre: u32,
    pub ink_raw: u32,
    pub ink_dim: u32,
    /// The PHYSICAL display width the `vw`-clamped values were derived
    /// from, so a rebuild at another scale (`sheet_for(&s.bundle(), pct,
    /// s.display_w)`) keeps them.
    pub display_w: u32,
    /// The position indicator (7.7) is painted on overflow: Instrument
    /// only.
    pub indicator: bool,
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

/// The transcript sheet for a RESOLVED theme at a display scale (percent).
///
/// The transcript matches the chrome that the compositor bevels + tag bar
/// draw around it because both derive from the same `Theme` -- which is now
/// a parameter rather than a constant this function reaches for
/// (HALCYON-THEME 3.2). At 100 every size is the logical value (nothing at
/// 1.0 moves; pinned by test).
/// The built-in theme's sheet. TEST-ONLY: production resolves a theme once at
/// startup and calls `sheet_for` with it, so a paint path cannot reach a
/// Daylight-specific constructor (HALCYON-THEME 3.2).
#[cfg(test)]
pub fn daylight_sheet(scale: u16) -> Sheet {
    sheet_for(
        &libhalcyon::instrument::Bundle::from_legacy(Profile::Legacy, libhalcyon::theme::builtin()),
        scale,
        TEST_DISPLAY_W,
    )
}

/// The display width the host tests build their sheets at (the reference
/// 1280 x 800 display; the legacy sheet reads no width at all).
#[cfg(test)]
pub const TEST_DISPLAY_W: u32 = 1280;

impl Sheet {
    /// The bundle this sheet was built from, for a rebuild at another
    /// scale (`sheet_for(&sheet.bundle(), pct, sheet.display_w)`): the
    /// profile, the legacy theme and the Instrument palette travel
    /// together, so a rescale can never drop the authored side of the pair.
    pub fn bundle(&self) -> libhalcyon::instrument::Bundle {
        libhalcyon::instrument::Bundle {
            profile: self.profile,
            theme: self.theme,
            inst: self.inst,
        }
    }
}

/// The sheet for a resolved theme under `profile` at `scale`, on a display
/// `display_w` PHYSICAL px wide. The metrics come through
/// `instrument::metrics_base` -- the PROFILE picks the table
/// (HALCYON-INSTRUMENT 4.4 / 5.7: the theme's own `[geometry]` under
/// legacy, the compiled Instrument table under instrument), scaled by the
/// one `Metrics::at` the compositor's carve also reads, so the two painters
/// cannot drift. The display width feeds only the Instrument document's
/// `vw` clamps (7.5: the paddings and H1 follow the DISPLAY, never the
/// pane); the legacy sheet reads nothing off it.
pub fn sheet_for(b: &libhalcyon::instrument::Bundle, scale: u16, display_w: u32) -> Sheet {
    let v = b.at(scale);
    let d = &b.theme;
    let i = &v.inst;
    let profile = b.profile;
    let metrics = v.metrics;
    let (island, grid) = mono_advances(scale);
    let px = |v: f32| libhalcyon::scale::px(v, scale);
    let ipx = |v: i32| libhalcyon::scale::ipx(v, scale);
    let inst = profile == Profile::Instrument;
    let mono_island_px = 2.0 * island as f32;
    // 7.5: the `vw` values are of the LOGICAL display width, clamped in
    // logical px, then scaled like every other size (a fractional logical
    // value rounds once, after scaling: 31.68 -> 32 at 100, 63 at 200).
    let disp_logical = display_w as f32 * 100.0 / scale.max(1) as f32;
    let vw = |pct: f32, lo: f32, hi: f32| (disp_logical * pct / 100.0).clamp(lo, hi);
    let block_gap = if inst { ipx(PARA_MARGIN) } else { ipx(6) };
    let term = d.terminal;
    Sheet {
        theme: *d,
        inst: v.inst,
        derived: v.derived,
        ground: d.surface,
        smooth_mem: d.smooth_mem,
        ink: d.fg,
        dim: d.fg_dim,
        accent: d.ember,
        obj: d.syntax.slate,
        err: d.cinnabar.key,
        ok: d.syntax.fen,
        rule: d.border,
        // 7.3: the precomputed derived selection under Instrument; the legacy
        // token stands under legacy (r2 B-F2's sibling).
        sel_bg: if profile == Profile::Instrument { v.derived.selection } else { d.selection },
        island_ground: d.header,
        island_rule: d.island_rule,
        scale,
        profile,
        metrics,
        hairline: metrics.hairline,
        mark_w: ipx(2),
        // The body/prose size the Daylight mockup runs at (halcyon-daylight.css
        // .hal-prose 11.5px; HALCYON-VISUAL 7-8 type scale); the Instrument
        // body 15 (7.2).
        body_px: if inst { px(BODY_PX_INST) } else { px(11.5) },
        // The prompt runs at the BASE size (the operator, 2026-09-08, on the
        // first live look: "rather small and not prominent"; it was 10 --
        // smaller than the prose it introduces). One value, never below the
        // body's. Under Instrument the prompt's three glyphs carry their
        // roles at the body size (7.4).
        prompt_px: if inst { px(BODY_PX_INST) } else { px(11.5) },
        hdr_px: if inst {
            [px(vw(2.4, 23.0, 34.0)), px(H2_PX_INST), px(H3_PX_INST)]
        } else {
            [px(17.5), px(14.5), px(12.5)]
        },
        mono_island_px,
        mono_grid_px: 2.0 * grid as f32,
        // The text inset, MEASURED off the operator's mockup render
        // (halcyon_text_composition_mockup.png at 2x: the prose starts 22
        // image px inside the pane's parchment edge -- 11 logical; the
        // stylesheet's 10 + 2 + 8 would put it 9 px further in). Under
        // Instrument the kit's `.editor` padding (7.5).
        pad_x: if inst { round_px(px(vw(3.0, 20.0, 48.0))) } else { ipx(12) },
        pad_top: if inst { round_px(px(vw(2.2, 18.0, 34.0))) } else { block_gap },
        pad_bottom: if inst { ipx(DOC_PAD_BOTTOM) } else { 0 },
        block_gap,
        table_col_gap: ipx(16),
        kv_col_gap: ipx(28),
        face_body: if inst { FACE_SANS } else { FACE_BODY },
        face_strong: FACE_BODY_BOLD,
        face_emph: if inst { FACE_HEADING_ITALIC } else { FACE_BODY_ITALIC },
        face_hdr: if inst { FACE_SANS_MEDIUM } else { FACE_HEADING_ITALIC },
        face_medium: if inst { FACE_SANS_MEDIUM } else { FACE_BODY },
        face_brand: if inst { FACE_SANS_SEMIBOLD } else { FACE_BODY },
        face_mono_text: if inst { FACE_MONO_TEXT } else { FACE_MONO },
        face_mono_italic: if inst { FACE_MONO_ITALIC } else { FACE_MONO },
        face_code: if inst { FACE_MONO_TEXT } else { FACE_MONO },
        code_px: if inst { px(BODY_PX_INST * CODE_EM) } else { mono_island_px },
        chrome_mono_px: if inst { px(CHROME_MONO_PX) } else { mono_island_px },
        clock_px: if inst { px(CLOCK_PX) } else { mono_island_px },
        flow: if inst { Flow::Fractional } else { Flow::PerLine },
        lh_body: if inst { LH_BODY_INST } else { LH_BODY },
        lh_hdr: if inst { LH_HDR_INST } else { [LH_HDR; 3] },
        lh_pre: LH_PRE_INST,
        lh_raw: LH_RAW_INST,
        kerning: inst,
        hdr_track: if inst { HDR_TRACK_INST } else { [0.0; 3] },
        hang_spaces: inst,
        rhythm: if inst { INSTRUMENT_RHYTHM } else { LEGACY_RHYTHM },
        measure_cap: if inst { ipx(MEASURE_CAP) } else { NO_CAP },
        pre_pad_y: ipx(if inst { PRE_PAD_Y_INST } else { ISLAND_PAD_Y }),
        pre_pad_x: ipx(if inst { PRE_PAD_X_INST } else { ISLAND_PAD_X }),
        pre_pad_r: if inst { ipx(PRE_PAD_X_INST) } else { 0 },
        pre_rule_w: ipx(if inst { PRE_RULE_W_INST } else { ISLAND_RULE_W }),
        raw_pad_y: ipx(if inst { RAW_PAD_Y_INST } else { ISLAND_PAD_Y }),
        raw_pad_x: ipx(if inst { RAW_PAD_X_INST } else { ISLAND_PAD_X }),
        raw_pad_r: if inst { ipx(RAW_PAD_X_INST) } else { 0 },
        raw_rule_w: if inst { 0 } else { ipx(ISLAND_RULE_W) },
        code_pad: if inst { 0 } else { ipx(CODE_PAD) },
        ground_pre: if inst { i.code_bg } else { d.header },
        // The code block's 2 px rule is the Instrument side's `amber_muted`
        // (7.3) -- never the legacy palette's `island_rule`, which a legacy-
        // schema theme under the Instrument profile keeps as its own (r2 B-F2).
        rule_pre: if inst { i.amber_muted } else { d.island_rule },
        ground_raw: if inst { i.terminal_bg } else { d.header },
        rule_raw: d.island_rule,
        ground_code: if inst { None } else { Some(d.header) },
        ink_prose: if inst { i.body_text } else { term.fg },
        ink_prompt: if inst { i.text } else { term.fg },
        ink_hdr: if inst { i.text } else { term.fg },
        ink_strong: if inst { i.text } else { term.fg },
        ink_code: if inst { i.code_text } else { term.fg },
        ink_pre: if inst { i.code_body } else { term.fg },
        ink_raw: if inst { i.terminal_text } else { d.fg_dim },
        ink_dim: if inst { i.dim } else { d.fg_dim },
        display_w,
        indicator: inst,
        gen: 0,
    }
}

/// The Instrument chrome's mono sizes, LOGICAL (HALCYON-INSTRUMENT 7.2):
/// the index, the metadata, the footer, a menu hint; and the clock.
pub const CHROME_MONO_PX: f32 = 10.0;
pub const CLOCK_PX: f32 = 11.0;

/// The Instrument document (HALCYON-INSTRUMENT 7.2 / 7.5), LOGICAL: the
/// body 15 / 1.62, H2 17 / 1.3, H3 15 / 1.3 (the kit defines no H3; a step
/// under H2 in its ratios, recorded in 7.2), the `pre` row 12 x 1.65, the
/// terminal row 12 x 1.6, inline code 0.86 x the body. H1 clamp(23, 2.4
/// vw, 34) / 1.12 is computed in `sheet_for`.
const BODY_PX_INST: f32 = 15.0;
const H2_PX_INST: f32 = 17.0;
const H3_PX_INST: f32 = 15.0;
const CODE_EM: f32 = 0.86;
const LH_BODY_INST: f32 = 1.62;
const LH_HDR_INST: [f32; 3] = [1.12, 1.3, 1.3];
const LH_PRE_INST: f32 = 1.65;
const LH_RAW_INST: f32 = 1.6;
/// 7.2: the heading ranks' letter-spacing in em -- the H1's -0.025 em; the
/// H2 and H3 none. The golden's H1 is 34.85 px narrower than its glyphs'
/// advances over 41 characters, exactly this.
const HDR_TRACK_INST: [f32; 3] = [-0.025, 0.0, 0.0];
/// The UA's 1 em block margin at the body size: paragraphs, lists, tables
/// and the block gap (7.5, collapsed).
const PARA_MARGIN: i32 = 15;
const DOC_PAD_BOTTOM: i32 = 50;
const MEASURE_CAP: i32 = 720;
/// "No cap": wide enough that no width reaches it, small enough that the
/// arithmetic around a table's unbounded lay (`i32::MAX / 2`) cannot
/// overflow.
const NO_CAP: i32 = i32::MAX / 4;
const PRE_PAD_Y_INST: i32 = 15;
const PRE_PAD_X_INST: i32 = 17;
const PRE_RULE_W_INST: i32 = 2;
const PRE_MARGIN_INST: i32 = 18;
const RAW_PAD_Y_INST: i32 = 14;
const RAW_PAD_X_INST: i32 = 16;
/// The flow's fixed point: 1/64 px (Blink's LayoutUnit), so the line pitch
/// truncates where the browser's did (24.3 -> 24.296875) and the golden's
/// rows reproduce to the pixel.
const FLOW_SCALE: i32 = 64;

/// A flow position rounded to its row (round half up; `LayoutUnit::Round`).
#[inline]
fn ypx(q: i32) -> i32 {
    // Saturating like the accumulator it reads (r2 B-F5): a saturated flow
    // must not wrap on the half-row it adds for the rounding.
    q.saturating_add(FLOW_SCALE / 2).div_euclid(FLOW_SCALE)
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
/// The legacy rhythm: exactly the constants above (byte-pinned).
const LEGACY_RHYTHM: Rhythm = Rhythm {
    prose: PROSE_MARGIN,
    empty: 0,
    hdr_top: HDR_TOP,
    hdr_bottom: [HDR_BOTTOM; 3],
    pre: ISLAND_MARGIN,
    raw: ISLAND_MARGIN,
    table_top: TABLE_TOP,
    table_bottom: TABLE_BOTTOM,
    rule: RULE_MARGIN,
    prompt_bottom: PROMPT_BOTTOM,
    deck_top: DECK_TOP,
    deck_bottom: DECK_BOTTOM,
};
/// The Instrument rhythm (7.5): H1 0 / 14, H2 28 / 10, H3 22 / 8 (the
/// kit's step, recorded), a paragraph and the empty line 15 (collapsed;
/// nothing between the lines OF a paragraph), a `pre` 18, raw output 0
/// (the terminal view follows its prompt at the pitch), a table 15, the
/// rule the UA's 8, the prompt straight into its output; the herald's deck
/// keeps its own two values.
const INSTRUMENT_RHYTHM: Rhythm = Rhythm {
    prose: PARA_MARGIN,
    empty: PARA_MARGIN,
    hdr_top: [0, 28, 22],
    hdr_bottom: [14, 10, 8],
    pre: PRE_MARGIN_INST,
    raw: 0,
    table_top: PARA_MARGIN,
    table_bottom: PARA_MARGIN,
    rule: RULE_MARGIN,
    prompt_bottom: 0,
    deck_top: DECK_TOP,
    deck_bottom: DECK_BOTTOM,
};

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
/// That includes the KERN `lay_span` folds into each step (I-5d: the GPOS
/// pair adjustment in the pen's unit, 0 under legacy) and a heading's
/// letter-spacing -- a measure that omits a term the lay adds is the drift
/// above.
fn run_width(gs: &mut GlyphSource, face: u8, px: f32, chars: impl Iterator<Item = char>) -> i32 {
    run_width_fx(gs, face, px, 0.0, chars).div_euclid(GlyphSource::PEN_SCALE)
}

/// `run_width` before its one division: the run's width in 1/256 px, with
/// `track_em` of letter-spacing added to every glyph's advance (the last
/// included, the CSS rule) -- what the lay loop's pen travels.
fn run_width_fx(gs: &mut GlyphSource, face: u8, px: f32, track_em: f32, chars: impl Iterator<Item = char>) -> i32 {
    let track_q = track_fx(px, track_em);
    let mut q = 0i32;
    let mut prev: Option<char> = None;
    for ch in chars {
        if let Some(p) = prev {
            if !is_mono_face(face) {
                q += gs.kern(face, px, p, ch);
            }
        }
        if let Some(a) = gs.advance_fx(face, px, ch) {
            q += a + track_q;
            prev = Some(ch);
        }
    }
    q
}

/// Letter-spacing of `em` at `px` in 1/256 px, rounded once (0 for none).
fn track_fx(px: f32, em: f32) -> i32 {
    if em == 0.0 {
        0
    } else {
        crate::raster::round_half_away(px * em * GlyphSource::PEN_SCALE as f32)
    }
}

/// A heading rank's letter-spacing in em: `hdr` is the Beacon rank (0 = not
/// a heading), the em from the sheet's table (7.2).
fn hdr_track_em(sheet: &Sheet, hdr: u8) -> f32 {
    if hdr == 0 {
        return 0.0;
    }
    sheet.hdr_track[usize::from(hdr.min(3) - 1)]
}

/// `hdr_track_em` in 1/256 px at `px`.
fn hdr_track_fx(sheet: &Sheet, hdr: u8, px: f32) -> i32 {
    track_fx(px, hdr_track_em(sheet, hdr))
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

fn face_for(st: &Style, in_table: bool, sheet: &Sheet) -> u8 {
    // An inline `em class=code` literal is the only mono case reaching here
    // (8.2): the island cell under legacy, Cornucopia free-running at 0.86 x
    // the body under Instrument (7.2); a `pre` block / a raw line is forced
    // mono at the lay_span call, and alt-screen is a separate raw-grid path.
    // Everything else is proportional (14.13), through the sheet's roles.
    if st.em == EM_CODE {
        return sheet.face_code;
    }
    // `annotated` gates only whether a foreign SGR bold promotes to the
    // reserved bold: strong/emph/hdr are themselves annotations, so a plain
    // run is the proportional body regardless of SGR bold -- foreign bold is
    // NOT the one em-strong bold (8.2).
    let annotated = st.annotated() || in_table;
    if !annotated {
        return sheet.face_body;
    }
    // Genera type discipline (HALCYON.md section 3 + HALCYON-VISUAL 8): bold is
    // RESERVED for extreme emphasis -- em class=strong and foreign SGR bold on
    // an annotated run, nothing else; emphasis and headings go ITALIC, heading
    // RANK carried by size (px_for), never weight -- bold headings are retired.
    if st.em == EM_STRONG || st.attrs & ATTR_BOLD != 0 {
        sheet.face_strong
    } else if st.hdr != 0 {
        // Headings are the Regular-weight (400) italic, a DISTINCT weight from
        // the body italic (operator's baseline=Text / bigger=Regular rule);
        // rank stays size-carried (px_for), never weight. Under Instrument
        // the roman Medium (7.2).
        sheet.face_hdr
    } else if st.em == EM_EMPH {
        sheet.face_emph
    } else if sheet.profile == Profile::Instrument && st.attrs & ATTR_ITALIC != 0 {
        // A foreign SGR italic on an annotated run takes the italic under
        // Instrument (7.2: "where it asks an italic"); legacy never read
        // the attribute and is byte-pinned on that.
        sheet.face_emph
    } else {
        // An obj / table-cell presentation with no weight or slant is the Text
        // body: an object is a colour + hit overlay (the pill), not a font
        // change, so it matches surrounding proportional text; inside a `pre`
        // block the pre flag forces it mono instead, keeping the grid's cell
        // metrics.
        sheet.face_body
    }
}

/// The size a span of `face` lays at: a cell face at the island em, the
/// free-running code face at the sheet's code size, a proportional face at
/// its rank (`px_for`) over `base`.
fn span_px(face: u8, st: &Style, base: f32, sheet: &Sheet) -> f32 {
    if is_cell_face(face) {
        sheet.mono_island_px
    } else if face == FACE_MONO_TEXT {
        sheet.code_px
    } else {
        px_for(st, base, sheet)
    }
}

/// Did this cell CHOOSE no foreground? -- i.e. is it still carrying the vt
/// pen's default, which is `[terminal] fg`?
///
/// The TERMINAL tier, not `sheet.ink` (`[palette] fg`). They are separate
/// settable keys, and a theme may legitimately set them apart -- the bg half
/// of this same test says so in its own comment and gets it right. Comparing
/// against `sheet.ink` is correct only by AUTHORSHIP convention (both shipped
/// themes happen to set them equal), and when an author breaks that
/// convention every hook below silently stops firing: em-dim, object
/// colouring and the raw dim step all go dead, which is TH-6 F1's symptom
/// class reachable through a supported input instead of through a constant.
fn is_default_ink(st: &Style, sheet: &Sheet) -> bool {
    st.fg == sheet.theme.terminal.fg
}

fn color_for(st: &Style, sheet: &Sheet) -> u32 {
    if st.em == EM_DIM && is_default_ink(st, sheet) {
        return sheet.dim;
    }
    if st.obj != 0 && is_default_ink(st, sheet) {
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
    /// The flow's y in 1/64 px (`FLOW_SCALE`). Whole under the per-line
    /// flow, where every box is an integer; fractional under the
    /// Instrument flow, rounded to a row (`ypx`) wherever a pixel is read.
    y_q: i32,
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
    /// The current item's right inset inside its island (the code block's
    /// / the terminal view's right padding under Instrument; 0 elsewhere).
    right_inset: i32,
    /// The current item's measure cap, px (`Sheet::measure_cap` for the
    /// capped roles, `NO_CAP` for the rest).
    cap: i32,
    any_body: bool,
    line_px: f32,
    /// The heading rank of the line under construction (0 = none).
    line_hdr: u8,
    /// The class of the line under construction (an empty line's box).
    line_class: LineClass,
    /// The line belongs to a `pre` block (the code row's pitch, not the
    /// terminal row's).
    line_pre: bool,
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
            y_q: 0,
            segs: Vec::new(),
            pen_x: sheet.pad_x,
            pen_q: 0,
            x0: sheet.pad_x,
            right_inset: 0,
            cap: NO_CAP,
            any_body: false,
            line_px: 0.0,
            line_hdr: 0,
            line_class: LineClass::Doc,
            line_pre: false,
            center: false,
        }
    }

    /// The flow's current row.
    #[inline]
    fn y(&self) -> i32 {
        ypx(self.y_q)
    }

    /// Advance the flow by whole pixels (a margin, a padding, a rule).
    /// Saturating, like the pitch below: a block is bounded by its line
    /// and cost caps, far under the i32 in 1/64 px, but a wrap would put a
    /// later line ABOVE an earlier one, and a saturated flow merely stops.
    #[inline]
    fn advance(&mut self, px: i32) {
        self.y_q = self.y_q.saturating_add(px.saturating_mul(FLOW_SCALE));
    }

    /// The right edge of the content area under a measure cap: the inset
    /// plus the lesser of the available width and the cap (7.5's 720).
    #[inline]
    fn right_with(&self, cap: i32) -> i32 {
        let avail = (self.width - 2 * self.sheet.pad_x).min(cap);
        self.sheet.pad_x + avail
    }

    /// The right edge of the current item's content area.
    #[inline]
    fn content_right(&self) -> i32 {
        self.right_with(self.cap)
    }

    /// Reset the builder for a new item at the text inset.
    fn start_item(&mut self, cap: i32) {
        self.x0 = self.sheet.pad_x;
        self.pen_x = self.sheet.pad_x;
        self.pen_q = 0;
        self.center = false;
        self.line_class = LineClass::Doc;
        self.line_pre = false;
        self.right_inset = 0;
        self.cap = cap;
    }

    fn note_metrics(&mut self, face: u8, px: f32, hdr: u8) {
        // A mono run never grows the box, in either kind (COMPOSITION 4).
        if !is_mono_face(face) {
            self.any_body = true;
            if px > self.line_px {
                self.line_px = px;
            }
            if hdr > self.line_hdr {
                self.line_hdr = hdr;
            }
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

    /// The legacy line box (the per-line flow): (pitch in 1/64 px, the
    /// baseline's offset from the line top, the content's ascent and
    /// descent) -- `body_box` / `mono_box` by the line's kind.
    fn per_line_box(&self, gs: &GlyphSource) -> (i32, i32, i32, i32) {
        let sheet = self.sheet;
        let ((asc, desc), (asc_c, desc_c)) = if self.segs.is_empty() {
            // An empty line still occupies its class's line box.
            match self.line_class {
                LineClass::Raw => Self::mono_box(gs, sheet),
                LineClass::Prompt => Self::body_box(gs, sheet.prompt_px, LH_BODY),
                _ => Self::body_box(gs, sheet.body_px, LH_BODY),
            }
        } else if self.any_body {
            let factor = if self.line_hdr > 0 { LH_HDR } else { LH_BODY };
            Self::body_box(gs, self.line_px, factor)
        } else {
            Self::mono_box(gs, sheet)
        };
        ((asc + desc) * FLOW_SCALE, asc, asc_c, desc_c)
    }

    /// The Instrument line box (the fractional flow; 7.2 / 7.5 as built):
    /// the pitch is the size times the role's factor, truncated to 1/64 px
    /// as the browser truncates it; the content box is the face's rounded
    /// ascent + descent (a cell's, for a mono row); the glyphs sit at the
    /// line top plus the FLOORED half-leading (Blink's `AddLeading`),
    /// which goes negative where the content overflows the box (the H1's
    /// 44 in 38.08: the golden's fragment tops sit 3 above its line). A
    /// mono row is the island cell in the `pre` (12 x 1.65) or the
    /// terminal (12 x 1.6) pitch; a document line with no proportional run
    /// (inline code alone) keeps the body box, which the code inherits and
    /// never grows; the EMPTY document line is the paragraph break: no box.
    fn flow_box(&self, gs: &GlyphSource) -> (i32, i32, i32, i32) {
        let sheet = self.sheet;
        let leading = |lh_q: i32, content: i32| -> i32 {
            ((lh_q - content * FLOW_SCALE) / 2).div_euclid(FLOW_SCALE)
        };
        let mono_pitch = |factor: f32| -> (i32, i32, i32, i32) {
            // The row places its baseline by the face's CONTENT box (the
            // hhea pair: 11 + 2 at 12 px -- the golden's 13 px fragment),
            // not by the cell, which is one row taller (the OS/2 Windows
            // descent the cell is cut to); the cell paints where the
            // baseline puts it, its extra row of descent under the box.
            let (asc, desc) = match gs.line_metrics(FACE_MONO_TEXT, sheet.mono_island_px) {
                Some(m) => (m.ascent, m.descent),
                None => {
                    let (_, chh, base) = gs.island_cell();
                    (base, chh - base)
                }
            };
            let lh_q = (sheet.mono_island_px * factor * FLOW_SCALE as f32) as i32;
            (lh_q, leading(lh_q, asc + desc) + asc, asc, desc)
        };
        if self.line_pre {
            return mono_pitch(sheet.lh_pre);
        }
        if self.line_class == LineClass::Raw {
            return mono_pitch(sheet.lh_raw);
        }
        if self.segs.is_empty() && self.line_class != LineClass::Prompt {
            return (0, 0, 0, 0);
        }
        let (px, factor) = if self.any_body {
            let f = if self.line_hdr > 0 {
                sheet.lh_hdr[(self.line_hdr.min(3) - 1) as usize]
            } else {
                sheet.lh_body
            };
            (self.line_px, f)
        } else {
            let base = if self.line_class == LineClass::Prompt {
                sheet.prompt_px
            } else {
                sheet.body_px
            };
            (base, sheet.lh_body)
        };
        let (asc, desc) = match gs.line_metrics(sheet.face_body, px) {
            Some(m) => (m.ascent, m.descent),
            None => (round_px(px), round_px(px * 0.3)),
        };
        let lh_q = (px * factor * FLOW_SCALE as f32) as i32;
        (lh_q, leading(lh_q, asc + desc) + asc, asc, desc)
    }

    /// Close the current visual line (13.5 + COMPOSITION 4: the body
    /// metrics OWN a mixed line -- mono islands sit on the body baseline and
    /// may not stretch the box; an all-mono line keeps the mono row box).
    fn break_line(&mut self, gs: &GlyphSource) {
        let (pitch_q, asc, asc_c, desc_c) = match self.sheet.flow {
            Flow::PerLine => self.per_line_box(gs),
            Flow::Fractional => self.flow_box(gs),
        };
        let y = self.y();
        let h = ypx(self.y_q.saturating_add(pitch_q)) - y;
        let baseline = y + asc;
        let mut segs = core::mem::take(&mut self.segs);
        // The herald: centre the content between the insets.
        if self.center && !segs.is_empty() {
            let first = segs.first().map(|s| s.x).unwrap_or(self.x0);
            let last = segs.last().map(|s| s.x_end).unwrap_or(first);
            let avail = self.content_right() - self.x0;
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
        // box; none under Instrument), the pill around an obj (the content
        // box + a hairline).
        let (_, ichh, ibase) = gs.island_cell();
        let (code_pad, obj_pad, hair) = (
            self.sheet.code_pad,
            self.sheet.ipx(OBJ_PAD),
            self.sheet.hairline,
        );
        for s in segs.iter() {
            if s.refs.is_empty() {
                continue;
            }
            match s.chrome {
                CHROME_CODE => {
                    if let Some(ground) = self.sheet.ground_code {
                        self.rects.push(RectSpec {
                            x: s.x - code_pad,
                            y: baseline - ibase,
                            w: (s.x_end - s.x + 2 * code_pad).max(0) as u32,
                            h: ichh as u32,
                            color: ground,
                        });
                    }
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
            y,
            h,
            baseline,
            segs,
            src_item: usize::MAX,
            src_row: usize::MAX,
        });
        self.y_q = self.y_q.saturating_add(pitch_q);
        self.pen_x = self.x0;
        self.pen_q = 0;
        self.any_body = false;
        self.line_px = 0.0;
        self.line_hdr = 0;
    }

    /// Lay one styled span, wrapping at the right edge (word-wrap breaks at
    /// the last space on the line when one exists, else hard-breaks; a raw
    /// line breaks at the character; a pre line never breaks -- and under
    /// Instrument is cut at its box, since the cartoon cannot clip and the
    /// kit's horizontal scroll is not built).
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
        let sheet = self.sheet;
        let mono = matches!(mode, SpanMode::Pre | SpanMode::Raw);
        let face = if mono {
            // 7.2: an SGR italic in a mono run is the true Italic cell (the
            // role resolves to the Regular's cell under legacy, so the
            // legacy bytes stand).
            if st.attrs & ATTR_ITALIC != 0 {
                sheet.face_mono_italic
            } else {
                FACE_MONO
            }
        } else {
            face_for(st, in_table, sheet)
        };
        let base_px = if mode == SpanMode::Prompt {
            sheet.prompt_px
        } else {
            sheet.body_px
        };
        let px = span_px(face, st, base_px, sheet);
        // 7.2: a heading run is tracked (the H1's -0.025 em); the term rides
        // every advance below -- the fit, the wrap, the spill, the pen.
        let track_em = if mono { 0.0 } else { hdr_track_em(sheet, st.hdr) };
        let track_q = track_fx(px, track_em);
        // The ink: an explicit SGR colour stands; a cell that chose none
        // takes the two legacy hooks (em-dim, the object colour) and then
        // its ROLE's default (7.3) -- raw output's overrides both (`.hal-out`:
        // the dim step under legacy, `terminal_text` under Instrument).
        let dflt = is_default_ink(st, sheet);
        let mut color = color_for(st, sheet);
        if dflt {
            if mode == SpanMode::Raw {
                color = sheet.ink_raw;
            } else if st.em == EM_DIM {
                color = sheet.ink_dim;
            } else if st.obj == 0 {
                color = match mode {
                    SpanMode::Pre => sheet.ink_pre,
                    SpanMode::Prompt => sheet.ink_prompt,
                    _ => {
                        if st.em == EM_CODE {
                            sheet.ink_code
                        } else if st.hdr != 0 {
                            sheet.ink_hdr
                        } else if face == sheet.face_strong {
                            sheet.ink_strong
                        } else {
                            sheet.ink_prose
                        }
                    }
                };
            }
        }
        let chrome = if mode == SpanMode::Doc && st.obj != 0 {
            CHROME_OBJ
        } else if mode == SpanMode::Doc && face == sheet.face_code {
            CHROME_CODE
        } else {
            CHROME_NONE
        };
        let pad = match chrome {
            CHROME_OBJ => sheet.ipx(OBJ_PAD),
            CHROME_CODE => sheet.code_pad,
            _ => 0,
        };
        let word_wrap = matches!(mode, SpanMode::Doc | SpanMode::Prompt);
        let no_wrap = mode == SpanMode::Pre;
        let cut_at_box = no_wrap && sheet.flow == Flow::Fractional;
        // A line-end space hangs (7.5): it never decides a wrap; the next
        // glyph does, and cuts after it. Proportional runs only -- a mono
        // grid has no collapsing space.
        let hang = sheet.hang_spaces && !mono;
        let right_edge = self.content_right() - self.right_inset;
        // A space-less span (a code island, a pill, one long word) that will
        // not fit the rest of the line moves WHOLE to the next line when the
        // line already holds content -- the break opportunity is the space
        // that ended the previous span, which this span cannot see. Measured
        // with the tracking the lay applies (r2 A-F5: an H1's second span
        // measured 9-17 px wide of its own lay and wrapped a word early).
        // Under Instrument it moves whether or not it fits a fresh line, as
        // CSS `overflow-wrap: break-word` takes the soft break first and
        // breaks the word only on its own line; the legacy gate ("only when
        // it fits a line") stands for the legacy bytes. That gate made a
        // block SHORTER at a narrower measure, which is what spun the
        // indicator lane's two-pass forever (r2 B-F1); without it the break
        // rule is monotone in the width again.
        if word_wrap && self.pen_x > self.x0 && !cells.iter().any(|c| c.ch == ' ') {
            let w = 2 * pad
                + run_width_fx(gs, face, px, track_em, cells.iter().map(|c| c.ch))
                    .div_euclid(GlyphSource::PEN_SCALE);
            let fits_a_line = w <= right_edge - self.x0;
            if self.pen_x + w > right_edge && (fits_a_line || sheet.flow == Flow::Fractional) {
                self.break_line(gs);
            }
        }
        let hdr = hdr_level(st.hdr);
        self.note_metrics(face, px, hdr);
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
        let right = right_edge - pad;
        while i < cells.len() {
            let ch = cells[i].ch;
            let Some(adv_q) = gs.advance_fx(face, px, ch) else {
                i += 1;
                col += 1;
                continue;
            };
            let adv_q = adv_q + track_q;
            // The pen advances in 1/256 px and so does the kern this glyph's
            // step carries toward the next (I-5d; 0 under legacy). The
            // glyph's own phase and whole step are resolved AFTER the wrap
            // decision below, because a wrap moves the pen to a line start
            // and re-zeroes the fraction. The wrap is judged on the glyph's
            // OWN advance: the kern toward the next glyph belongs to a pair
            // that may not share the line (r2 A-F9 -- a space-less run
            // otherwise broke a glyph late by up to the widest pair).
            let step_q = adv_q
                + if !is_mono_face(face) && i + 1 < cells.len() {
                    gs.kern(face, px, ch, cells[i + 1].ch)
                } else {
                    0
                };
            let provisional = (self.pen_q + adv_q).div_euclid(GlyphSource::PEN_SCALE);
            if cut_at_box && self.pen_x + provisional > right && self.pen_x > self.x0 {
                // The code block's overflow is cut at its box (the rest of
                // the line is unaddressable, as clipped text is).
                break;
            }
            if !no_wrap && self.pen_x + provisional > right && !seg.refs.is_empty() && !(hang && ch == ' ') {
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
                    let spill_chars: Vec<char> = spill_refs.iter().map(|r| r.ch).collect();
                    seg.xs.truncate(cut);
                    seg.x_end = seg.xs.last().copied().unwrap_or(seg.x)
                        + seg.refs.last().map(|r| r.advance).unwrap_or(0);
                    self.segs.push(seg);
                    self.break_line(gs);
                    self.note_metrics(face2, px2, hdr);
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
                    // whole tail by up to 3/4 px. Re-deriving also drops the
                    // kern folded into the old advance, so each spilled
                    // glyph re-kerns with the pair it now opens: its spilled
                    // successor, or -- for the last one -- the glyph that
                    // caused the wrap, which the main loop lays next. The
                    // pair at the wrap boundary itself is gone with the
                    // line: the space's step keeps a kern that ends a line.
                    let spill_n = spill_chars.len();
                    for (j, mut r) in spill_refs.into_iter().enumerate() {
                        let aq = gs
                            .advance_fx(face2, px2, r.ch)
                            .unwrap_or(r.advance * GlyphSource::PEN_SCALE)
                            + track_q;
                        let next = if j + 1 < spill_n { spill_chars[j + 1] } else { ch };
                        let k = if mono { 0 } else { gs.kern(face2, px2, r.ch, next) };
                        let total = self.pen_q + aq + k;
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
                    self.note_metrics(face2, px2, hdr);
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
    /// (top, bottom) margins at the sheet's scale (COMPOSITION 3 under
    /// legacy, HALCYON-INSTRUMENT 7.5 under Instrument -- the sheet's
    /// `rhythm`; collapsed pairwise -- scaling then taking the larger is
    /// taking the larger then scaling, `ipx` being monotone).
    fn margins(self, sheet: &Sheet) -> (i32, i32) {
        let r = &sheet.rhythm;
        let (t, b) = match self {
            Role::Empty => (r.empty, r.empty),
            Role::Prose => (r.prose, r.prose),
            Role::Hdr(level, _) => {
                let i = (level.clamp(1, 3) - 1) as usize;
                (r.hdr_top[i], r.hdr_bottom[i])
            }
            Role::Deck(first, last) => (
                if first { r.deck_top } else { 0 },
                if last { r.deck_bottom } else { 0 },
            ),
            Role::Prompt => (0, r.prompt_bottom),
            Role::Raw => (r.raw, r.raw),
            Role::Pre => (r.pre, r.pre),
            Role::Table => (r.table_top, r.table_bottom),
            Role::Rule => (r.rule, r.rule),
        };
        (sheet.ipx(t), sheet.ipx(b))
    }

    /// The measure this role wraps at (7.5: H1, prose, the prompt, a `pre`
    /// and the deck cap at the sheet's measure; H2 / H3, a table, a rule
    /// and raw output run the width). `NO_CAP` under legacy either way.
    fn cap(self, sheet: &Sheet) -> i32 {
        match self {
            Role::Hdr(level, _) if level >= 2 => NO_CAP,
            Role::Table | Role::Rule | Role::Raw => NO_CAP,
            _ => sheet.measure_cap,
        }
    }
}

fn roles_of(b: &Block, fractional: bool) -> Vec<Role> {
    let block_class = b.class();
    let mut roles: Vec<Role> = Vec::with_capacity(b.items.len());
    // The herald's deck: the dim, non-empty lines directly under a title.
    let mut deck_open = false;
    let mut deck_first = true;
    // Whether the previous item was a raw line (an island is open).
    let mut raw_open = false;
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
                    // 7.5 / 7.6 (r2 B-F3): a blank un-annotated line INSIDE
                    // a terminal-view island stays a row of it (byte
                    // conservation with the terminal), but one that would
                    // open an island of its own is the paragraph break --
                    // zero height, not 47 px of `terminal_bg` around nothing.
                    // Instrument only: the legacy island keeps its bytes.
                    LineClass::Raw if fractional && line.cells.is_empty() && !raw_open => Role::Empty,
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
        raw_open = role == Role::Raw;
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
    // The glyph source follows the sheet in force here, at the entry the
    // owners and every test share (r2 A-F2: forty Instrument painter tests
    // ran unkerned against a sheet that said `kerning`); the memo is
    // size-free, so the switch costs a compare.
    gs.set_kerning(sheet.kerning);
    let mut lb = LineBuilder::new(sheet, width.max(2 * sheet.pad_x + sheet.ipx(MIN_CONTENT_W)));
    let fractional = sheet.flow == Flow::Fractional;
    let roles = roles_of(b, fractional);
    let raw_margin = sheet.ipx(sheet.rhythm.raw);
    // CSS margin collapsing: the gap before an element is the larger of the
    // previous element's bottom margin and its own top; the first element's
    // top margin is dropped (the first-child reset) unless it is the herald,
    // whose own top margin is the sanctioned override (COMPOSITION 3).
    let mut prev_bottom: Option<i32> = None;
    // The previous item's role: under Instrument two consecutive prose
    // lines are one paragraph (no gap between them), and a prompt line runs
    // straight into what follows it in the same block (the live tail
    // straddles zones; the frozen prompt and output blocks meet at the
    // same 0, `block_gap_between`), so the two never disagree by a margin.
    let mut prev_role: Option<Role> = None;
    // A run of raw lines is ONE island: its chrome rects span the run. The
    // island's top, in the flow's 1/64 px.
    let mut island_top: Option<i32> = None;
    // An island's box: its ground and, when it has one, its leading rule --
    // the terminal view for raw output, the code block for a `pre` (7.3 /
    // 7.6 under Instrument; the one `.hal-out` island under legacy). The
    // code block keeps the measure cap; the terminal view runs the width.
    let close_island = |lb: &mut LineBuilder<'_>, top_q: i32, pre: bool| {
        lb.advance(if pre { sheet.pre_pad_y } else { sheet.raw_pad_y });
        let top = ypx(top_q);
        let h = (lb.y() - top).max(0) as u32;
        if h > 0 {
            let (ground, rule, rule_w, cap) = if pre {
                (sheet.ground_pre, sheet.rule_pre, sheet.pre_rule_w, sheet.measure_cap)
            } else {
                (sheet.ground_raw, sheet.rule_raw, sheet.raw_rule_w, NO_CAP)
            };
            lb.rects.push(RectSpec {
                x: sheet.pad_x,
                y: top,
                w: (lb.right_with(cap) - sheet.pad_x).max(0) as u32,
                h,
                color: ground,
            });
            if rule_w > 0 {
                lb.rects.push(RectSpec {
                    x: sheet.pad_x,
                    y: top,
                    w: rule_w as u32,
                    h,
                    color: rule,
                });
            }
        }
    };
    for (item_idx, item) in b.items.iter().enumerate() {
        let role = roles[item_idx];
        let (top, bottom) = role.margins(sheet);
        // Consecutive raw lines share one island: no margin between them.
        let joins_island = role == Role::Raw && island_top.is_some();
        if !joins_island {
            if let Some(t) = island_top.take() {
                close_island(&mut lb, t, false);
                prev_bottom = Some(raw_margin);
            }
            if fractional && role == Role::Empty {
                // 7.5: the empty document line is a zero-height paragraph
                // break whose margins collapse THROUGH -- it advances
                // nothing and folds its margins into the pending bottom, so
                // two blank lines open one paragraph gap, never two. It
                // still lays a (zero-height) line, so the item keeps its
                // source address for selection.
                prev_bottom = Some(match prev_bottom {
                    None => bottom,
                    Some(pb) => pb.max(top).max(bottom),
                });
                lb.start_item(role.cap(sheet));
                lb.break_line(gs);
                if let Some(l) = lb.lines.last_mut() {
                    l.src_item = item_idx;
                }
                prev_role = Some(role);
                continue;
            }
            let gap = match prev_bottom {
                None => match role {
                    Role::Hdr(_, true) => top,
                    _ => 0,
                },
                Some(pb) => {
                    if fractional && role == Role::Prose && prev_role == Some(Role::Prose) {
                        0
                    } else if fractional && prev_role == Some(Role::Prompt) {
                        sheet.ipx(sheet.rhythm.prompt_bottom)
                    } else {
                        pb.max(top)
                    }
                }
            };
            lb.advance(gap);
        }
        let lines_before = lb.lines.len();
        lb.start_item(role.cap(sheet));
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
                        island_top = Some(lb.y_q);
                        lb.advance(sheet.raw_pad_y);
                    }
                    lb.x0 = sheet.pad_x + sheet.raw_rule_w + sheet.raw_pad_x;
                    lb.pen_x = lb.x0;
                    lb.pen_q = 0;
                    lb.right_inset = sheet.raw_pad_r;
                }
                lb.center = matches!(role, Role::Hdr(_, true) | Role::Deck(..));
                for (s, e, sid) in runs_of(&line.cells) {
                    let st = b.styles[sid as usize];
                    let bg = if st.bg != sheet.ground && st.bg != sheet.theme.terminal.bg {
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
                    y: lb.y(),
                    w: (lb.content_right() - sheet.pad_x).max(0) as u32,
                    h: sheet.hairline as u32,
                    color: sheet.rule,
                });
                lb.advance(sheet.hairline);
            }
            Item::Pre(lines) => {
                // PL-1b: the preformatted code-fence island (HALCYON.md
                // 110-113): each line is laid MONO + verbatim (no wrap), inset
                // past the leading gutter; the own ground + the gutter rule
                // are added AFTER the lines (their y-extent is then known);
                // render_block paints rects before glyphs, so they sit BEHIND
                // the mono text. Under Instrument it is the code block (7.3:
                // `code_bg`, the 2 px `amber_muted` rule, 15 / 17 inside,
                // the 12 x 1.65 row, capped at the measure).
                let top_q = lb.y_q;
                lb.advance(sheet.pre_pad_y);
                lb.line_class = LineClass::Raw;
                lb.line_pre = true;
                lb.right_inset = sheet.pre_pad_r;
                for (li, line) in lines.iter().enumerate() {
                    lb.x0 = sheet.pad_x + sheet.pre_rule_w + sheet.pre_pad_x;
                    lb.pen_x = lb.x0;
                    lb.pen_q = 0;
                    let first = lb.lines.len();
                    for (s, e, sid) in runs_of(&line.cells) {
                        let st = b.styles[sid as usize];
                        // A run's own SGR background still shows through; the
                        // block ground is the default carrier otherwise. The
                        // second arm is the TERMINAL default, not a constant:
                        // a cell that never set a background carries the vt
                        // palette's bg, which a theme may set apart from the
                        // sheet's ground (HALCYON-THEME 3.1's `[terminal]`).
                        let bg = if st.bg != sheet.ground && st.bg != sheet.theme.terminal.bg {
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
                lb.line_pre = false;
                lb.right_inset = 0;
                close_island(&mut lb, top_q, true);
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
        prev_role = Some(role);
    }
    if let Some(t) = island_top.take() {
        close_island(&mut lb, t, false);
    }
    lb.start_item(NO_CAP);
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
        height: lb.y(),
        lines: lb.lines,
        rects,
    }
}

/// The vertical gap between two consecutive blocks of a transcript: a prompt
/// runs straight into its command's output -- one entry, `.hal-prompt`'s own
/// 2px under the prompt line (0 under Instrument: the output follows the
/// prompt at the pitch, 7.5) -- every other boundary is the block gap
/// (`.hal-block` margin-bottom 6; the paragraph margin under Instrument).
pub fn block_gap_between(prev: BlockKind, next: BlockKind, sheet: &Sheet) -> i32 {
    if prev == BlockKind::Prompt && next == BlockKind::Output {
        sheet.ipx(sheet.rhythm.prompt_bottom)
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
                let face = face_for(&st, true, sheet);
                let px = span_px(face, &st, sheet.body_px, sheet);
                w += run_width_fx(gs, face, px, hdr_track_em(sheet, st.hdr), cell[s..e].iter().map(|c| c.ch))
                    .div_euclid(GlyphSource::PEN_SCALE);
            }
            if ci < ncols && w > col_w[ci] {
                col_w[ci] = w;
            }
            ws.push(w);
        }
        cellw.push(ws);
    }
    // A row longer than the `cols` spec has cells no group owns; such a
    // table lays as a plain one, every column at or right of the inset
    // (r2 B-F8: the extra cells laid at x = 0).
    let kv = is_kv_list(t) && ncols == t.cols.len();
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
            let y = lb.y();
            lb.rects.push(RectSpec {
                x: sheet.pad_x,
                y,
                w: (x - sheet.table_col_gap - sheet.pad_x).max(0) as u32,
                h: sheet.hairline as u32,
                color: sheet.rule,
            });
            lb.advance(sheet.ipx(TABLE_HDR_GAP));
        } else if kv && ri + 1 < nrows {
            lb.advance(sheet.ipx(TABLE_ROW_GAP));
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
    let w = run_width(gs, sheet.face_body, px, text.chars());
    lb.pen_x = (lb.content_right() - w).max(sheet.pad_x);
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
        face: sheet.face_body,
        px,
        refs: Vec::new(),
        xs: Vec::new(),
        src_item: usize::MAX,
        src_col: 0,
        obj: 0,
        chrome: CHROME_NONE,
    };
    lb.note_metrics(sheet.face_body, px, 0);
    // The badge runs its own pen (it bypasses the block builder), so it
    // carries its own quarter-pixel remainder too -- and the kern of each
    // pair folded into the preceding step, exactly as `run_width` measured
    // the width it was right-aligned on (r2 A-F4: a kerned measure and an
    // unkerned lay put the badge a pixel past `content_right`).
    let mut q = 0i32;
    let mut prev: Option<char> = None;
    for c in cells.iter() {
        if let Some(aq) = gs.advance_fx(sheet.face_body, px, c.ch) {
            if let Some(p) = prev {
                let k = gs.kern(sheet.face_body, px, p, c.ch);
                if k != 0 {
                    let carried = q + k;
                    let whole = carried.div_euclid(GlyphSource::PEN_SCALE);
                    if let Some(last) = seg.refs.last_mut() {
                        last.advance += whole;
                    }
                    seg_start_x += whole;
                    q = carried.rem_euclid(GlyphSource::PEN_SCALE);
                }
            }
            prev = Some(c.ch);
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
    gs.set_kerning(sheet.kerning);
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
    // A zero-height line (the Instrument paragraph break, 7.5) still
    // carries a caret of the body's pitch: the cursor resting on a blank
    // row IS a line, as a caret in an empty block is in the browser.
    let box_h = |h: i32| {
        if h > 0 {
            h
        } else {
            round_px(sheet.body_px * sheet.lh_body)
        }
    };
    let mut end: Option<(i32, i32, i32)> = None;
    for line in laid.lines.iter() {
        if line.src_item != item || (row != usize::MAX && line.src_row != row) {
            continue;
        }
        for seg in line.segs.iter() {
            let n = seg.refs.len();
            if col >= seg.src_col && col < seg.src_col + n {
                return (seg.xs[col - seg.src_col], line.y, box_h(line.h));
            }
        }
        // A blank line's caret sits at the text inset, as `cursor_pos`'s
        // does (r2 B-F6: it sat at the tile's left edge).
        let x = line.segs.last().map(|s| s.x_end).unwrap_or(sheet.pad_x);
        end = Some((x, line.y, box_h(line.h)));
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
pub(crate) mod tests {
    use super::*;

    // THE OPERATOR'S QUESTION, made mechanical: "if a theme is made and all
    // colours are changed, some hardcoded Daylight colour won't kick in
    // somewhere". Retint every token the sheet reads and assert NO sheet
    // colour still holds a Daylight value. This is what caught the two
    // literals `daylight_sheet` carried before TH-2 (`sel_bg` and
    // `island_rule`, neither of which was a theme token at all), and it
    // fails again the day a new literal is written into `sheet_for`.
    #[test]
    fn a_fully_retinted_theme_leaves_no_daylight_colour_in_the_sheet() {
        // A bijection on the colour space: distinct inputs stay distinct, and
        // no colour maps to itself (c == !c is impossible in 24 bits).
        let flip = |c: u32| 0xFF00_0000 | (!c & 0x00FF_FFFF);
        let base = libhalcyon::theme::builtin();
        let mut t = base;
        t.floor = flip(t.floor);
        t.surface = flip(t.surface);
        t.header = flip(t.header);
        t.raised = flip(t.raised);
        t.border = flip(t.border);
        t.blank = flip(t.blank);
        t.selection = flip(t.selection);
        t.island_rule = flip(t.island_rule);
        t.fg = flip(t.fg);
        t.fg_dim = flip(t.fg_dim);
        t.fg_muted = flip(t.fg_muted);
        t.fg_subtle = flip(t.fg_subtle);
        t.ember = flip(t.ember);
        t.syntax.slate = flip(t.syntax.slate);
        t.syntax.fen = flip(t.syntax.fen);
        t.cinnabar.key = flip(t.cinnabar.key);

        let daylight: &[u32] = &[
            base.floor,
            base.surface,
            base.header,
            base.raised,
            base.border,
            base.blank,
            base.selection,
            base.island_rule,
            base.fg,
            base.fg_dim,
            base.fg_muted,
            base.fg_subtle,
            base.ember,
            base.syntax.slate,
            base.syntax.fen,
            base.cinnabar.key,
        ];
        // The retint must not accidentally land on ANOTHER Daylight colour,
        // or a survivor could pass by coincidence. Checked, not assumed.
        for c in [
            t.surface,
            t.fg,
            t.fg_dim,
            t.ember,
            t.syntax.slate,
            t.cinnabar.key,
            t.syntax.fen,
            t.border,
            t.selection,
            t.header,
            t.island_rule,
        ] {
            assert!(!daylight.contains(&c), "the retint collided with Daylight");
        }

        let s = sheet_for(&libhalcyon::instrument::Bundle::from_legacy(Profile::Legacy, t), 100, TEST_DISPLAY_W);
        for (name, c) in [
            ("ground", s.ground),
            ("ink", s.ink),
            ("dim", s.dim),
            ("accent", s.accent),
            ("obj", s.obj),
            ("err", s.err),
            ("ok", s.ok),
            ("rule", s.rule),
            ("sel_bg", s.sel_bg),
            ("island_ground", s.island_ground),
            ("island_rule", s.island_rule),
        ] {
            assert!(
                !daylight.contains(&c),
                "sheet.{name} is still a Daylight colour ({c:#010x}) -- it did \
                 not come from the theme"
            );
        }
        // And the sheet followed the theme, rather than merely differing from
        // Daylight: the positive control.
        assert_eq!(s.ground, t.surface);
        assert_eq!(s.sel_bg, t.selection);
        assert_eq!(s.island_rule, t.island_rule);
        assert_eq!(s.theme.surface, t.surface, "the sheet carries the theme");
    }

    // The built-in is the scripture's Daylight -- the loader's floor, so a
    // change here re-themes every installation with no theme file.
    #[test]
    fn the_builtin_is_daylight() {
        let b = libhalcyon::theme::builtin();
        assert_eq!(b.surface, 0xFFF2EBE0);
        assert_eq!(b.fg, 0xFF1A120A);
        assert_eq!(b.ember, 0xFFE07840);
    }
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

    // HALCYON-INSTRUMENT 7.1 / 7.2 (I-5): the sheet names the faces by
    // ROLE and the profile picks the cut -- the legacy column is exactly
    // the constants the legacy painters used before (byte-identical by
    // construction), the Instrument column the type map's; the chrome's
    // mono sizes are 10 / 11 under Instrument and the island em under
    // legacy, scaled with the sheet.
    #[test]
    fn the_sheet_picks_faces_and_chrome_sizes_by_profile() {
        use libhalcyon::instrument::{Bundle, Profile};
        let l = daylight_sheet(100);
        assert_eq!(
            (l.face_body, l.face_strong, l.face_emph, l.face_hdr),
            (FACE_BODY, FACE_BODY_BOLD, FACE_BODY_ITALIC, FACE_HEADING_ITALIC),
            "legacy: the pre-I-5 constants"
        );
        assert_eq!(
            (l.face_medium, l.face_brand, l.face_mono_text, l.face_mono_italic),
            (FACE_BODY, FACE_BODY, FACE_MONO, FACE_MONO)
        );
        assert_eq!((l.chrome_mono_px, l.clock_px), (l.mono_island_px, l.mono_island_px));
        let i = sheet_for(&Bundle::builtin(Profile::Instrument), 100, TEST_DISPLAY_W);
        assert_eq!(
            (i.face_body, i.face_strong, i.face_emph, i.face_hdr),
            (FACE_SANS, FACE_BODY_BOLD, FACE_HEADING_ITALIC, FACE_SANS_MEDIUM)
        );
        assert_eq!(
            (i.face_medium, i.face_brand, i.face_mono_text, i.face_mono_italic),
            (FACE_SANS_MEDIUM, FACE_SANS_SEMIBOLD, FACE_MONO_TEXT, FACE_MONO_ITALIC)
        );
        assert_eq!((i.chrome_mono_px, i.clock_px), (10.0, 11.0));
        let i2 = sheet_for(&Bundle::builtin(Profile::Instrument), 200, TEST_DISPLAY_W);
        assert_eq!((i2.chrome_mono_px, i2.clock_px), (20.0, 22.0), "scaled with the sheet");
        // The rebuild round-trips the profile (a rescale never drops it).
        let i3 = sheet_for(&i.bundle(), 150, TEST_DISPLAY_W);
        assert_eq!((i3.face_body, i3.chrome_mono_px), (FACE_SANS, 15.0));
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
        let sheet = daylight_sheet(100);

        assert_eq!(face_for(&with(0, 1, 0), false, &sheet), FACE_HEADING_ITALIC, "hdr 1 is the regular-weight italic, never bold");
        assert_eq!(face_for(&with(0, 3, 0), false, &sheet), FACE_HEADING_ITALIC, "hdr 3 is the regular-weight italic too");
        assert_eq!(face_for(&with(EM_EMPH, 0, 0), false, &sheet), FACE_BODY_ITALIC, "emph is the Text-weight italic");
        assert_eq!(face_for(&with(EM_STRONG, 0, 0), false, &sheet), FACE_BODY_BOLD, "strong is the reserved bold");
        // A plain (un-annotated) cell is the proportional body (14.13's
        // proportional-live model). A foreign SGR bold on it does NOT promote
        // to the reserved bold -- that is em-strong's alone (8.2); the SGR-bold
        // path fires only on an ANNOTATED run (a table cell below).
        assert_eq!(face_for(&with(0, 0, ATTR_BOLD), false, &sheet), FACE_BODY, "plain SGR bold stays body -- foreign bold is not the em-strong bold");
        assert_eq!(face_for(&with(0, 0, ATTR_BOLD), true, &sheet), FACE_BODY_BOLD, "annotated + foreign SGR bold -> the reserved bold");
        assert_eq!(face_for(&with(EM_CODE, 0, 0), false, &sheet), FACE_MONO, "inline code is the only mono case here");
        assert_eq!(face_for(&base, false, &sheet), FACE_BODY, "plain ordinary output is proportional body (14.13)");
        assert_eq!(face_for(&Style { obj: 1, ..base }, false, &sheet), FACE_BODY, "an obj presentation is regular body");
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

    // TH-6 ROUND 2, R2-F1: `[palette] fg` and `[terminal] fg` are SEPARATE
    // settable keys, and the hooks that decide "this cell chose no colour"
    // must test the pen's default -- the TERMINAL tier -- not the sheet's ink.
    //
    // Nothing constructed a sheet and a pen from DIFFERENT tiers of one theme
    // before this, which is exactly why comparing against `sheet.ink` looked
    // right: both shipped themes happen to set the two equal, so every test
    // that existed agreed with the wrong comparison. This builds the theme an
    // author is free to write and checks the hooks still fire.
    #[test]
    fn the_ink_hooks_follow_the_terminal_tier_not_the_palette_tier() {
        let mut d = libhalcyon::theme::DAYLIGHT;
        // The freedom the format documents: a theme whose terminal ink differs
        // from its chrome ink.
        d.terminal.fg = 0xFFFF_FFFF;
        assert_ne!(d.terminal.fg, d.fg, "the fixture must actually split them");
        let sheet = sheet_for(&libhalcyon::instrument::Bundle::from_legacy(Profile::Legacy, d), 100, TEST_DISPLAY_W);
        assert_eq!(sheet.ink, d.fg, "the sheet's ink is the PALETTE tier");

        // A cell that set no colour carries the PEN's default.
        let base = Style { fg: d.terminal.fg, bg: d.terminal.bg, attrs: 0, em: 0, obj: 0, hdr: 0 };
        assert_eq!(
            color_for(&Style { em: EM_DIM, ..base }, &sheet),
            sheet.dim,
            "em-dim must still take the dim step when the tiers differ"
        );
        assert_eq!(
            color_for(&Style { obj: 1, ..base }, &sheet),
            sheet.obj,
            "an object reference must still take the object colour"
        );
        // ...and a cell that DID choose a colour is still left alone.
        let chosen = Style { fg: 0xFF00_FF00, ..base };
        assert_eq!(
            color_for(&Style { em: EM_DIM, ..chosen }, &sheet),
            0xFF00_FF00,
            "an explicit SGR foreground is never overridden"
        );
        // The control, one variable away: with the tiers equal -- every theme
        // shipped today -- the same hooks fire, so this is not a behaviour
        // change for Daylight or Nightjar.
        let agreed = sheet_for(&libhalcyon::instrument::Bundle::from_legacy(Profile::Legacy, libhalcyon::theme::DAYLIGHT), 100, TEST_DISPLAY_W);
        let ab = Style { fg: agreed.theme.terminal.fg, bg: 0, attrs: 0, em: 0, obj: 0, hdr: 0 };
        assert_eq!(color_for(&Style { em: EM_DIM, ..ab }, &agreed), agreed.dim);
        assert_eq!(color_for(&Style { obj: 1, ..ab }, &agreed), agreed.obj);
    }
    /// The pre-I-5b legacy layout, PINNED as a fingerprint (FNV-1a over every
    /// laid number of a rich transcript at two widths and two scales). The
    /// Instrument document table (I-5b) added a second column to the sheet;
    /// this is the proof that the legacy column is still the bytes the
    /// legacy gates were green on -- measured on the tree BEFORE the change,
    /// not asserted after it.
    pub(crate) fn fnv(h: &mut u64, bytes: &[u8]) {
        for &b in bytes {
            *h ^= b as u64;
            *h = h.wrapping_mul(0x100000001b3);
        }
    }

    pub(crate) fn fp_i32(h: &mut u64, v: i32) {
        fnv(h, &v.to_le_bytes());
    }

    pub(crate) fn fp_u32(h: &mut u64, v: u32) {
        fnv(h, &v.to_le_bytes());
    }

    pub(crate) fn fp_laid(h: &mut u64, laid: &LaidBlock) {
        fp_i32(h, laid.height);
        fp_u32(h, laid.lines.len() as u32);
        for l in laid.lines.iter() {
            fp_i32(h, l.y);
            fp_i32(h, l.h);
            fp_i32(h, l.baseline);
            fp_u32(h, l.src_item as u32);
            fp_u32(h, l.src_row as u32);
            fp_u32(h, l.segs.len() as u32);
            for s in l.segs.iter() {
                fp_i32(h, s.x);
                fp_i32(h, s.x_end);
                fp_u32(h, s.color);
                fp_u32(h, s.bg.unwrap_or(1));
                fnv(h, &[s.face, s.chrome]);
                fp_u32(h, s.px.to_bits());
                fp_u32(h, s.src_item as u32);
                fp_u32(h, s.src_col as u32);
                fp_u32(h, s.obj as u32);
                for (g, x) in s.refs.iter().zip(s.xs.iter()) {
                    fp_u32(h, g.ch as u32);
                    fp_i32(h, g.advance);
                    fnv(h, &[g.phase]);
                    fp_i32(h, *x);
                }
            }
        }
        fp_u32(h, laid.rects.len() as u32);
        for r in laid.rects.iter() {
            fp_i32(h, r.x);
            fp_i32(h, r.y);
            fp_u32(h, r.w);
            fp_u32(h, r.h);
            fp_u32(h, r.color);
        }
    }

    /// A transcript touching every layout arm: a prompt with the turnstile
    /// (the cell fallback), a title herald and its deck, three heading
    /// ranks, wrapping prose, an empty line, every em class, an obj, SGR
    /// italic / bold / colour on annotated and raw runs, a rule, a headed
    /// table, a pre block with a blank line, a failed exit, a foreign raw
    /// block with a blank row.
    pub(crate) fn rich_transcript() -> Transcript {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, BOp::Zone, &[("k", "prompt")]);
        buf.extend_from_slice("~/src \u{22a2} ls -l\n".as_bytes());
        wire::close(&mut buf, BOp::Zone);
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
        buf.extend_from_slice(b"\n");
        wire::open(&mut buf, BOp::Hdr, &[("level", "2")]);
        buf.extend_from_slice(b"Section one");
        wire::close(&mut buf, BOp::Hdr);
        buf.extend_from_slice(b"\nThe quick brown fox jumps over the lazy dog and keeps running well past the edge of a narrow line so that it wraps\n\nMixed ");
        wire::open(&mut buf, BOp::Em, &[("class", "strong")]);
        buf.extend_from_slice(b"bold");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b" and ");
        wire::open(&mut buf, BOp::Em, &[("class", "emph")]);
        buf.extend_from_slice(b"italic");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b" and ");
        wire::open(&mut buf, BOp::Em, &[("class", "code")]);
        buf.extend_from_slice(b"mono_code");
        wire::close(&mut buf, BOp::Em);
        buf.extend_from_slice(b" and ");
        wire::open(&mut buf, BOp::Obj, &[("type", "path"), ("ref", "/etc/hosts")]);
        buf.extend_from_slice(b"hosts");
        wire::close(&mut buf, BOp::Obj);
        buf.extend_from_slice(b" and \x1b[3mSGR italic\x1b[0m \x1b[1mSGR bold\x1b[0m \x1b[31mred\x1b[0m\n");
        wire::point(&mut buf, BOp::Rule, &[]);
        wire::open(&mut buf, BOp::Hdr, &[("level", "3")]);
        buf.extend_from_slice(b"Sub");
        wire::close(&mut buf, BOp::Hdr);
        buf.extend_from_slice(b"\n");
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
        wire::open(&mut buf, BOp::Pre, &[]);
        buf.extend_from_slice(b"fn main() {\n    \x1b[3m// comment\x1b[0m \x1b[1mbold\x1b[0m\n    let x = \x1b[32m1\x1b[0m;\n\n}\n");
        wire::close(&mut buf, BOp::Pre);
        wire::point(&mut buf, BOp::Mark, &[("k", "exit"), ("code", "7")]);
        wire::close(&mut buf, BOp::Zone);
        wire::open(&mut buf, BOp::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"col1   col2\nabc    def\n\x1b[3mital\x1b[0m \x1b[1mbold\x1b[0m\n\nlast raw line that is long enough to wrap at the character when the width is narrow\n");
        wire::close(&mut buf, BOp::Zone);
        t.feed(&buf);
        t
    }

    pub(crate) fn layout_fingerprint(sheet: &Sheet, widths: &[i32]) -> u64 {
        let t = rich_transcript();
        let mut g = gs();
        let mut h: u64 = 0xcbf29ce484222325;
        for &w in widths {
            let blocks = t.frozen_blocks();
            for (i, b) in blocks.iter().enumerate() {
                let laid = layout_block(b, w, sheet, &mut g);
                fp_laid(&mut h, &laid);
                if let Some(next) = blocks.get(i + 1) {
                    fp_i32(&mut h, block_gap_between(b.kind, next.kind, sheet));
                }
            }
            let open = layout_block(t.open_block(), w, sheet, &mut g);
            fp_laid(&mut h, &open);
        }
        h
    }

    #[test]
    fn legacy_layout_is_byte_identical_to_the_pre_i5b_tree() {
        // Both fingerprints were read off the tree at 69f71541 (the I-5a
        // close) by this very test with the constants at 0.
        let fp100 = layout_fingerprint(&daylight_sheet(100), &[600, 300]);
        let fp200 = layout_fingerprint(&daylight_sheet(200), &[1200, 600]);
        assert_eq!(fp100, LEGACY_FP_100, "legacy layout at 100% drifted: got {fp100:#018x}");
        assert_eq!(fp200, LEGACY_FP_200, "legacy layout at 200% drifted: got {fp200:#018x}");
    }

    const LEGACY_FP_100: u64 = 0xaee6ab4f4db2d167;
    const LEGACY_FP_200: u64 = 0x2ebd8c8178cf4392;
    // ---- I-5b: the Instrument document (HALCYON-INSTRUMENT 7.2 / 7.3 / 7.5) ----

    fn inst_sheet(display_w: u32) -> Sheet {
        use libhalcyon::instrument::{Bundle, Profile};
        sheet_for(&Bundle::builtin(Profile::Instrument), 100, display_w)
    }

    /// A transcript whose cells carry `pal` as their default pen -- the
    /// palette the sheet's theme declares to the tile in production, so a
    /// cell that chose no colour reads as DEFAULT ink under that sheet.
    fn inst_transcript(pal: vt::Palette, build: impl Fn(&mut Vec<u8>)) -> Transcript {
        let mut t = Transcript::new(pal);
        let mut buf = Vec::new();
        build(&mut buf);
        t.feed(&buf);
        t
    }

    fn hdr(buf: &mut Vec<u8>, level: &str, text: &str) {
        wire::open(buf, BOp::Hdr, &[("level", level)]);
        buf.extend_from_slice(text.as_bytes());
        wire::close(buf, BOp::Hdr);
        buf.extend_from_slice(b"\n");
    }

    fn em(buf: &mut Vec<u8>, class: &str, text: &str) {
        wire::open(buf, BOp::Em, &[("class", class)]);
        buf.extend_from_slice(text.as_bytes());
        wire::close(buf, BOp::Em);
    }

    /// The type map and the composition table at the golden's display
    /// (1440 x 900 at 100), at the reference display, at 200, and the
    /// legacy column's identity.
    #[test]
    fn the_instrument_sheet_is_the_type_map_and_the_composition_table() {
        let s = inst_sheet(1440);
        let i = s.inst;
        assert_eq!(s.flow, Flow::Fractional);
        assert_eq!((s.body_px, s.prompt_px), (15.0, 15.0));
        assert_eq!(s.hdr_px, [34.0, 17.0, 15.0], "H1 clamp(23, 2.4 vw, 34) = 34 at 1440");
        assert!((s.code_px - 12.9).abs() < 1e-4, "0.86 x 15");
        assert_eq!((s.lh_body, s.lh_hdr, s.lh_pre, s.lh_raw), (1.62, [1.12, 1.3, 1.3], 1.65, 1.6));
        assert_eq!((s.pad_top, s.pad_x, s.pad_bottom), (32, 43, 50), "31.68 / 43.2 / 50 at 1440");
        assert_eq!((s.block_gap, s.measure_cap), (15, 720));
        assert_eq!((s.pre_pad_y, s.pre_pad_x, s.pre_pad_r, s.pre_rule_w), (15, 17, 17, 2));
        assert_eq!((s.raw_pad_y, s.raw_pad_x, s.raw_pad_r, s.raw_rule_w), (14, 16, 16, 0));
        assert_eq!(s.code_pad, 0);
        assert_eq!((s.face_code, s.face_mono_italic), (FACE_MONO_TEXT, FACE_MONO_ITALIC));
        assert_eq!((s.ground_pre, s.rule_pre), (i.code_bg, i.amber_muted));
        // One variable away from the projection that satisfied the line
        // above by coincidence (r2 B-F2): a LEGACY-schema theme under the
        // Instrument profile keeps its own `island_rule` and `selection`,
        // and the sheet must still read the Instrument side's tokens.
        let dlb = libhalcyon::instrument::Bundle::from_legacy(
            libhalcyon::instrument::Profile::Instrument,
            libhalcyon::theme::DAYLIGHT,
        );
        let dl = sheet_for(&dlb, 100, 1440);
        assert_ne!(dlb.theme.island_rule, dlb.inst.amber_muted, "the control: the two tokens differ here");
        assert_eq!(dl.rule_pre, dlb.inst.amber_muted);
        assert_ne!(dlb.theme.selection, dl.derived.selection, "the control: the two selections differ here");
        assert_eq!(dl.sel_bg, dl.derived.selection);
        assert_eq!((s.ground_raw, s.ground_code), (i.terminal_bg, None));
        assert_eq!(
            (s.ink_prose, s.ink_prompt, s.ink_hdr, s.ink_strong),
            (i.body_text, i.text, i.text, i.text)
        );
        assert_eq!((s.ink_code, s.ink_pre, s.ink_raw, s.ink_dim), (i.code_text, i.code_body, i.terminal_text, i.dim));
        assert_eq!(s.rhythm, INSTRUMENT_RHYTHM);
        assert!(s.indicator);
        assert_eq!(s.display_w, 1440);
        // The reference display: the clamps move with the width.
        let r = inst_sheet(1280);
        assert_eq!((r.pad_top, r.pad_x), (28, 38), "28.16 / 38.4 at 1280");
        assert!((r.hdr_px[0] - 30.72).abs() < 1e-4, "2.4 vw of 1280 = 30.72");
        // A narrow display floors; a wide one ceils.
        let n = inst_sheet(600);
        assert_eq!((n.pad_top, n.pad_x, n.hdr_px[0]), (18, 20, 23.0));
        let w = inst_sheet(2000);
        assert_eq!((w.pad_top, w.pad_x, w.hdr_px[0]), (34, 48, 34.0));
        // 200% on a display of the same LOGICAL width as the golden's.
        let d = sheet_for(&s.bundle(), 200, 2880);
        assert_eq!((d.pad_top, d.pad_x, d.pad_bottom), (63, 86, 100), "63.36 / 86.4 / 100");
        assert_eq!(d.hdr_px, [68.0, 34.0, 30.0]);
        assert_eq!((d.block_gap, d.measure_cap, d.pre_pad_x), (30, 1440, 34));
        assert!((d.code_px - 25.8).abs() < 1e-4);
        // The legacy column is the pre-I-5b table (the fingerprint test pins
        // the bytes; this pins the fields it pins them through).
        let l = daylight_sheet(100);
        assert_eq!(l.flow, Flow::PerLine);
        assert_eq!((l.pad_top, l.pad_x, l.pad_bottom, l.block_gap), (6, 12, 0, 6));
        assert!(l.measure_cap > 1 << 28, "no cap");
        assert_eq!((l.face_code, l.face_mono_italic), (FACE_MONO, FACE_MONO));
        assert_eq!(l.code_px, l.mono_island_px);
        assert_eq!((l.ground_pre, l.ground_raw, l.ground_code), (l.theme.header, l.theme.header, Some(l.theme.header)));
        assert_eq!((l.ink_prose, l.ink_prompt, l.ink_hdr, l.ink_code, l.ink_pre), (l.theme.terminal.fg, l.theme.terminal.fg, l.theme.terminal.fg, l.theme.terminal.fg, l.theme.terminal.fg));
        assert_eq!((l.ink_raw, l.ink_dim), (l.theme.fg_dim, l.theme.fg_dim));
        assert_eq!(l.rhythm, LEGACY_RHYTHM);
        assert!(!l.indicator);
    }

    /// The fractional flow against the golden's DOM boxes
    /// (`matrix-carbon-1440x900-s100-baseDpr1/geometry-styles.json`): an H1
    /// of 34 / 38.08 whose 44 px content overflows its box by 3 above (the
    /// fragment top 168.672 under a box top of 171.672), a paragraph of 15
    /// / 24.3 with its glyphs 2 under the line top (264.828 under 262.828),
    /// the H2 of 17 / 22.1 flush (339.422), the margins collapsed to the
    /// larger (15 after the H1, 28 before the H2, 15 after it, 18 before
    /// the `pre`), the `pre` padded 15 with its 13 px cell 3 under each
    /// 19.8 row (495.406 = 477.406 + 15 + 3) -- every row where the
    /// browser's 1/64 px accumulation puts it.
    #[test]
    fn the_fractional_flow_reproduces_the_goldens_document_rows() {
        let s = inst_sheet(1440);
        let build = |buf: &mut Vec<u8>| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            hdr(buf, "1", "Compositor geometry");
            buf.extend_from_slice(b"The layout is a recursive tree.\n");
            hdr(buf, "2", "Resize constraints");
            buf.extend_from_slice(b"Reserve the header budget.\n");
            wire::open(buf, BOp::Pre, &[]);
            buf.extend_from_slice(b"const A;\n\n}\n");
            wire::close(buf, BOp::Pre);
            buf.extend_from_slice(b"after\n");
            wire::close(buf, BOp::Zone);
        };
        let t = inst_transcript(s.theme.terminal, build);
        let mut g = gs();
        // Cornucopia's 12 px CELL is 14 rows (cut to the OS/2 Windows
        // descent, baseline 11); the browser's content box is the hhea
        // pair, 11 + 2 = 13 -- the golden's fragment height. The row is
        // placed by the content box; the cell paints under the baseline.
        assert_eq!(g.island_cell(), (6, 14, 11));
        let m = g.line_metrics(FACE_MONO_TEXT, 12.0).unwrap();
        assert_eq!((m.ascent, m.descent), (11, 2));
        let laid = layout_block(&t.frozen_blocks()[0], 733, &s, &mut g);
        let ls = &laid.lines;
        let row = |l: &LaidLine| (l.y, l.h, l.baseline);
        // H1: pitch 2437/64 = 38.078; content 35 + 9 = 44; half-leading
        // floor(-2.96) = -3; baseline 0 - 3 + 35.
        assert_eq!(row(&ls[0]), (0, 38, 32));
        assert!(ls[0].segs.iter().all(|sg| sg.face == FACE_SANS_MEDIUM && sg.px == 34.0 && sg.color == s.inst.text));
        // + max(14, 15) = 15: 53.08 -> 53; pitch 1555/64 = 24.297; 2 + 15.
        assert_eq!(row(&ls[1]), (53, 24, 70));
        assert!(ls[1].segs.iter().all(|sg| sg.face == FACE_SANS && sg.px == 15.0 && sg.color == s.inst.body_text));
        // + max(15, 28) = 28: 105.375 -> 105; pitch 1414/64 = 22.094; the
        // 22 px content fits its box: half-leading 0.
        assert_eq!(row(&ls[2]), (105, 22, 122));
        assert!(ls[2].segs.iter().all(|sg| sg.face == FACE_SANS_MEDIUM && sg.px == 17.0));
        // + max(10, 15) = 15: 142.47 -> 142; the row rounds to 25 here --
        // the accumulation, not a per-line 24.
        assert_eq!(row(&ls[3]), (142, 25, 159));
        // + max(15, 18) = 18: the box at 184.77 -> 185; 15 in: 199.77 ->
        // 200; each row 1267/64 = 19.797 with the cell 3 under its top.
        assert_eq!(row(&ls[4]), (200, 20, 214));
        assert_eq!(row(&ls[5]), (220, 19, 234), "the blank pre row keeps its pitch");
        assert_eq!(row(&ls[6]), (239, 20, 253));
        let boxes: Vec<&RectSpec> = laid.rects.iter().filter(|r| r.color == s.inst.code_bg).collect();
        assert_eq!(boxes.len(), 1, "one code block ground");
        assert_eq!((boxes[0].x, boxes[0].y, boxes[0].w, boxes[0].h), (43, 185, 647, 89), "the box: pad_x in, min(647, 720) wide, 3 x 19.797 + 30 tall");
        let rule = laid.rects.iter().find(|r| r.color == s.inst.amber_muted).expect("the 2 px rule");
        assert_eq!((rule.x, rule.y, rule.w, rule.h), (43, 185, 2, 89));
        assert_eq!(ls[4].segs[0].x, 43 + 2 + 17, "the code starts 19 in: the rule and the padding");
        assert!(ls[4].segs.iter().all(|sg| sg.face == FACE_MONO && sg.px == s.mono_island_px && sg.color == s.inst.code_body));
        // + max(18, 15) = 18 after the box (274.16 -> 274): 292.16 -> 292.
        assert_eq!(row(&ls[7]), (292, 24, 309));
        assert_eq!(laid.height, 316, "316.45 -> 316: the flow's end, rounded once");
        // The same block under legacy is the per-line flow: whole pitches.
        let l = daylight_sheet(100);
        let t = inst_transcript(daylight(), build);
        let legacy = layout_block(&t.frozen_blocks()[0], 733, &l, &mut g);
        assert_eq!(legacy.lines[1].h, 17);
        assert_eq!(legacy.lines[0].h, round_px(17.5 * LH_HDR));
    }

    /// 7.5: the empty document line is the paragraph break -- zero height,
    /// margins that collapse THROUGH -- so two blank lines open one gap; it
    /// still lays a (zero-height) line for its source address. Legacy keeps
    /// its full line box.
    #[test]
    fn an_empty_document_line_is_a_paragraph_break_that_collapses_through() {
        let s = inst_sheet(1280);
        let one_b = |buf: &mut Vec<u8>| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            em(buf, "strong", "A");
            buf.extend_from_slice(b" line\n\nsecond\n");
            wire::close(buf, BOp::Zone);
        };
        let one = inst_transcript(s.theme.terminal, one_b);
        let two = inst_transcript(s.theme.terminal, |buf| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            em(buf, "strong", "A");
            buf.extend_from_slice(b" line\n\n\nsecond\n");
            wire::close(buf, BOp::Zone);
        });
        let mut g = gs();
        let a = layout_block(&one.frozen_blocks()[0], 600, &s, &mut g);
        let b = layout_block(&two.frozen_blocks()[0], 600, &s, &mut g);
        assert_eq!(a.lines.len(), 3);
        assert_eq!((a.lines[1].h, a.lines[1].src_item), (0, 1), "the break: no box, still addressed");
        // 24.297 + 15 = 39.3 -> 39, not 24 + 24.
        assert_eq!(a.lines[2].y, 39);
        assert_eq!(b.lines.len(), 4);
        assert_eq!(b.lines[3].y, 39, "two breaks collapse to one paragraph gap");
        // Consecutive prose lines are one paragraph: at the pitch, no margin.
        let para = inst_transcript(s.theme.terminal, |buf| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            em(buf, "strong", "A");
            buf.extend_from_slice(b" line\nsecond\nthird\n");
            wire::close(buf, BOp::Zone);
        });
        let p = layout_block(&para.frozen_blocks()[0], 600, &s, &mut g);
        assert_eq!((p.lines[1].y, p.lines[2].y), (24, 49), "0, 24.297, 48.59");
        // Legacy: a full line box (17) and its own rhythm.
        let l = daylight_sheet(100);
        let one = inst_transcript(daylight(), one_b);
        let la = layout_block(&one.frozen_blocks()[0], 600, &l, &mut g);
        assert_eq!(la.lines[1].h, 17);
    }

    /// 7.2: inline code is Cornucopia free-running at 0.86 x the body, on
    /// the body baseline, inheriting the body's box and never growing it;
    /// 7.3: no ground, `code_text` ink. A line of code alone keeps the body
    /// box. Legacy: the island cell on its own ground.
    #[test]
    fn inline_code_runs_free_at_point_86_of_the_body_and_never_grows_the_box() {
        let s = inst_sheet(1440);
        let build = |buf: &mut Vec<u8>| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            buf.extend_from_slice(b"Mixed ");
            em(buf, "code", "expandedTile");
            buf.extend_from_slice(b" end\n");
            em(buf, "code", "alone");
            buf.extend_from_slice(b"\n");
            wire::close(buf, BOp::Zone);
        };
        let t = inst_transcript(s.theme.terminal, build);
        let mut g = gs();
        let laid = layout_block(&t.frozen_blocks()[0], 733, &s, &mut g);
        let code = laid.lines[0].segs.iter().find(|sg| sg.face == FACE_MONO_TEXT).expect("the code span");
        assert!((code.px - 12.9).abs() < 1e-4);
        assert_eq!(code.color, s.inst.code_text);
        assert_eq!(code.chrome, CHROME_CODE);
        assert!(laid.rects.is_empty(), "no ground under the span");
        assert_eq!((laid.lines[0].h, laid.lines[0].baseline), (24, 17), "the body box, unchanged by the span");
        assert_eq!((laid.lines[1].y, laid.lines[1].h, laid.lines[1].baseline - laid.lines[1].y), (24, 25, 17), "code alone: still the body box (24.297 .. 48.59: the accumulation's 25)");
        assert_eq!(laid.lines[0].segs[0].color, s.inst.body_text);
        // The golden's `expandedTile` fragment: 77.578 wide at 12.9 px --
        // 12 glyphs at 6.45 -- reproduced by the free-running advance.
        let w: i32 = code.refs.iter().map(|r| r.advance).sum();
        assert!((w - 77).abs() <= 1, "the span is {} wide (golden 77.578)", w);
        let l = daylight_sheet(100);
        let t = inst_transcript(daylight(), build);
        let legacy = layout_block(&t.frozen_blocks()[0], 733, &l, &mut g);
        let code = legacy.lines[0].segs.iter().find(|sg| sg.face == FACE_MONO).expect("the cell span");
        assert_eq!(code.px, l.mono_island_px);
        assert!(legacy.rects.iter().any(|r| r.color == l.island_ground), "the legacy ground");
    }

    /// 7.3 / 7.6: raw output is the terminal view (`terminal_bg`, no rule,
    /// 14 / 16 in, the 12 x 1.6 row, `terminal_text`, the width uncapped);
    /// a `pre` is the code block (capped at the measure, cut at its box).
    #[test]
    fn raw_output_is_the_terminal_view_and_a_pre_the_code_block() {
        let s = inst_sheet(1440);
        let build = |buf: &mut Vec<u8>| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            buf.extend_from_slice(b"col1   col2\nabc    def\n");
            wire::close(buf, BOp::Zone);
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            wire::open(buf, BOp::Pre, &[]);
            buf.extend_from_slice(b"short\n");
            for _ in 0..200 {
                buf.extend_from_slice(b"x");
            }
            buf.extend_from_slice(b"\n");
            wire::close(buf, BOp::Pre);
            wire::close(buf, BOp::Zone);
        };
        let t = inst_transcript(s.theme.terminal, build);
        let mut g = gs();
        let blocks = t.frozen_blocks();
        let raw = layout_block(&blocks[0], 1000, &s, &mut g);
        assert_eq!(raw.rects.len(), 1, "the ground only: no rule");
        let ground = &raw.rects[0];
        assert_eq!((ground.x, ground.w, ground.color), (43, 1000 - 86, s.inst.terminal_bg), "the terminal view runs the width, uncapped");
        assert_eq!((ground.y, ground.h), (0, 14 + 38 + 14), "14 in, two 19.19 rows (38.375 -> 38), 14 out");
        assert_eq!(raw.lines[0].segs[0].x, 43 + 16);
        assert!(raw.lines[0].segs.iter().all(|sg| sg.color == s.inst.terminal_text && sg.face == FACE_MONO));
        assert_eq!((raw.lines[0].y, raw.lines[0].h, raw.lines[0].baseline), (14, 19, 14 + 3 + 11));
        let pre = layout_block(&blocks[1], 1000, &s, &mut g);
        let ground = pre.rects.iter().find(|r| r.color == s.inst.code_bg).expect("the code block");
        assert_eq!((ground.x, ground.w), (43, 720), "capped at the measure");
        assert!(pre.rects.iter().any(|r| r.color == s.inst.amber_muted && r.w == 2));
        let long = &pre.lines[1];
        let end = long.segs.last().map(|sg| sg.x_end).unwrap();
        assert!(end <= 43 + 720 - 17, "the overflow is cut at the box's inner edge ({end})");
        assert!(long.segs[0].refs.len() < 200 && long.segs[0].refs.len() > 100);
        assert!(pre.lines[0].segs.iter().all(|sg| sg.color == s.inst.code_body));
        // Legacy: the one island with its rule, uncut.
        let l = daylight_sheet(100);
        let t = inst_transcript(daylight(), build);
        let legacy = layout_block(&t.frozen_blocks()[1], 1000, &l, &mut g);
        assert_eq!(legacy.lines[1].segs[0].refs.len(), 200);
    }

    /// 7.3's default inks by role under Instrument; an explicit SGR colour
    /// and the object colour stand in both profiles.
    #[test]
    fn the_default_inks_follow_the_role_under_instrument() {
        let s = inst_sheet(1280);
        let t = inst_transcript(s.theme.terminal, |buf| {
            wire::open(buf, BOp::Zone, &[("k", "prompt")]);
            buf.extend_from_slice(b"~ > ls\n");
            wire::close(buf, BOp::Zone);
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            hdr(buf, "2", "Head");
            em(buf, "strong", "strong");
            em(buf, "dim", "dim");
            em(buf, "emph", "emph");
            wire::open(buf, BOp::Obj, &[("type", "path"), ("ref", "/x")]);
            buf.extend_from_slice(b"obj");
            wire::close(buf, BOp::Obj);
            buf.extend_from_slice(b" plain \x1b[31mred\x1b[0m\n");
            wire::close(buf, BOp::Zone);
        });
        let mut g = gs();
        let blocks = t.frozen_blocks();
        let prompt = layout_block(&blocks[0], 600, &s, &mut g);
        assert!(prompt.lines[0].segs.iter().all(|sg| sg.color == s.inst.text && sg.face == FACE_SANS && sg.px == 15.0), "the prompt's input is `text` at the body size");
        let doc = layout_block(&blocks[1], 600, &s, &mut g);
        assert_eq!(doc.lines[0].segs[0].color, s.inst.text, "a heading");
        let segs = &doc.lines[1].segs;
        let by = |i: usize| (segs[i].color, segs[i].face);
        assert_eq!(by(0), (s.inst.text, FACE_BODY_BOLD), "strong");
        assert_eq!(by(1), (s.inst.dim, FACE_SANS), "dim");
        assert_eq!(by(2), (s.inst.body_text, FACE_HEADING_ITALIC), "emph: the Regular Italic in body ink");
        assert_eq!(by(3), (s.obj, FACE_SANS), "an object keeps the object colour");
        assert_eq!(by(4), (s.inst.body_text, FACE_SANS), "plain prose");
        let red = segs.last().unwrap();
        assert_eq!(red.color, s.theme.terminal.ansi[1], "an explicit SGR colour (ANSI red) stands");
    }

    /// The golden's text lines (matrix-carbon-1440x900-s100-baseDpr1,
    /// geometry-styles.json textLines): the SPAN of each visual line -- the
    /// last fragment's right minus the first's left -- NOT the sum of the
    /// fragment widths, which Blink floors and ceils to 1/64 px per
    /// character (+1.44 px over the 94-character paragraph line). Regular
    /// 15 for prose and list items, Medium 17 for the H2, Medium 34 at
    /// -0.025 em for the H1: (face, px, tracking em, text, span px).
    const GOLDEN_SPANS: &[(u8, f32, f32, &str, f32)] = &[
        (FACE_SANS_MEDIUM, 34.0, -0.025, "Compositor geometry should remain legible", 644.750),
        (FACE_SANS_MEDIUM, 34.0, -0.025, "under motion.", 207.719),
        (FACE_SANS, 15.0, 0.0, "The layout is a recursive tree of splits. Leaves own ordered tile stacks; every leaf maintains", 606.359),
        (FACE_SANS_MEDIUM, 17.0, 0.0, "Resize constraints", 141.969),
        (FACE_SANS, 15.0, 0.0, "Reserve the header budget before allocating body height.", 384.438),
        (FACE_SANS, 15.0, 0.0, "Clamp each pane to its minimum usable dimension.", 346.719),
        (FACE_SANS, 15.0, 0.0, "Keep divider movement continuous and reversible.", 340.297),
        (FACE_SANS_MEDIUM, 34.0, -0.025, "Hard geometry, quiet surfaces.", 453.500),
        (FACE_SANS, 15.0, 0.0, "The divider network is the workspace chassis. Its intersections are small joints; its lines", 586.062),
        (FACE_SANS, 15.0, 0.0, "remain visible without demanding attention.", 296.219),
        (FACE_SANS_MEDIUM, 17.0, 0.0, "Visual hierarchy", 124.719),
        (FACE_SANS, 15.0, 0.0, "Pane focus is a structural state, not decoration.", 315.625),
        (FACE_SANS, 15.0, 0.0, "Expanded content becomes fractionally brighter.", 325.859),
        (FACE_SANS, 15.0, 0.0, "Amber means action, focus, or attention\u{2014}nothing else.", 366.875),
    ];

    /// I-5d: the sheet switches kerning and tracks the H1 by profile --
    /// both off under legacy (the fingerprints stand), both on under
    /// Instrument (7.2, 7.5).
    #[test]
    fn the_sheet_kerns_and_tracks_the_h1_under_instrument_only() {
        let d = daylight_sheet(100);
        assert!(!d.kerning);
        assert_eq!(d.hdr_track, [0.0; 3]);
        let s = inst_sheet(1440);
        assert!(s.kerning);
        assert_eq!(s.hdr_track, [-0.025, 0.0, 0.0]);
        assert!(s.hang_spaces);
        assert!(!d.hang_spaces);
        assert_eq!(hdr_track_fx(&s, 1, 34.0), -218, "34 x -0.025 x 256 = -217.6");
        assert_eq!(hdr_track_fx(&s, 2, 17.0), 0);
        assert_eq!(hdr_track_fx(&s, 0, 15.0), 0);
        assert_eq!(hdr_track_fx(&d, 1, 17.5), 0);
    }

    /// 7.5 (I-5d): with kerning on, the sub-pixel pen measures every golden
    /// line to the browser's span within a quarter pixel (the per-glyph
    /// 1/256 rounding over up to 94 glyphs) -- HarfBuzz's `kern` through
    /// read-fonts, the H1 tracked. The control, one variable away: unkerned,
    /// the widest miss is over a pixel; untracked, the H1 misses by 35.
    #[test]
    fn the_goldens_lines_measure_the_browsers_spans_with_kerning_on() {
        let mut g = gs();
        assert!(g.set_kerning(true));
        let mut unkerned_gap = 0.0f32;
        // The control is PER LINE (r2 A-F10): a line whose unkerned width
        // also lands inside the tolerance proves nothing about kerning.
        // Ten of the fourteen discriminate (misses 0.35 .. 1.95 px); the
        // four that do not (0.24, 0.13, 0.07, 0.03 px -- the H1's own line
        // among them) witness the faces and the tracking only.
        let mut discriminating = 0usize;
        for &(face, px, track, text, span) in GOLDEN_SPANS {
            let w = run_width_fx(&mut g, face, px, track, text.chars()) as f32 / 256.0;
            assert!((w - span).abs() < 0.25, "{text:?}: {w} vs the browser's {span}");
            g.set_kerning(false);
            let u = run_width_fx(&mut g, face, px, track, text.chars()) as f32 / 256.0;
            g.set_kerning(true);
            let miss = (u - span).abs();
            if miss >= 0.25 {
                discriminating += 1;
            }
            unkerned_gap = unkerned_gap.max(miss);
        }
        assert_eq!(discriminating, 10, "ten lines must miss the tolerance unkerned (max unkerned miss {unkerned_gap})");
        assert!(unkerned_gap > 1.0, "kerning must matter to at least one line or this proves nothing (max unkerned miss {unkerned_gap})");
        let w0 = run_width_fx(&mut g, FACE_SANS_MEDIUM, 34.0, 0.0, "Compositor geometry should remain legible".chars()) as f32 / 256.0;
        assert!((w0 - 644.750).abs() > 30.0, "untracked H1 {w0}");
    }

    /// The golden's H1 wraps after "legible" at the pane's 647 px measure
    /// (733 wide, 43 of padding a side): 644.75 fits and "under" does not.
    /// Untracked and unkerned the same words are 679.8 wide and wrap a
    /// word earlier -- the browser's wrap point needs both, AND the space
    /// after "legible" left to hang (644.75 + a space is 652: a rule that
    /// fits the space wraps the word, which this test found). The
    /// paragraph wraps after "maintains" as the golden does.
    #[test]
    fn the_goldens_h1_and_paragraph_wrap_where_the_browser_wraps() {
        let s = inst_sheet(1440);
        let t = inst_transcript(s.theme.terminal, |buf| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            hdr(buf, "1", "Compositor geometry should remain legible under motion.");
            buf.extend_from_slice(b"The layout is a recursive tree of splits. Leaves own ordered tile stacks; every leaf maintains exactly one expanded tile.\n");
            wire::close(buf, BOp::Zone);
        });
        let line_text = |l: &LaidLine| -> String { l.segs.iter().flat_map(|sg| sg.refs.iter().map(|r| r.ch)).collect() };
        let mut g = gs();
        g.set_kerning(true);
        let laid = layout_block(&t.frozen_blocks()[0], 733, &s, &mut g);
        let texts: Vec<String> = laid.lines.iter().map(line_text).collect();
        assert_eq!(texts[0].trim_end(), "Compositor geometry should remain legible", "{texts:?}");
        assert_eq!(texts[1].trim_end(), "under motion.");
        assert_eq!(texts[2].trim_end(), "The layout is a recursive tree of splits. Leaves own ordered tile stacks; every leaf maintains");
        assert!(texts[3].starts_with("exactly one"), "{texts:?}");
        // The H1's laid width is the run measure's to the pixel (the lay
        // and the measure share one accumulator), and the browser's span
        // plus the hanging space (7.65 px at Medium 34 with the tracking).
        let h1 = &laid.lines[0].segs;
        let w = h1.last().unwrap().x_end - h1[0].x;
        let measured = run_width_fx(&mut g, FACE_SANS_MEDIUM, 34.0, -0.025, "Compositor geometry should remain legible ".chars())
            .div_euclid(GlyphSource::PEN_SCALE);
        assert_eq!(w, measured, "the lay disagrees with the measure");
        assert!((6..=9).contains(&(w - 645)), "H1 laid {w} px wide: 644.75 plus the hanging space");
        // The control: untracked and unkerned, the H1 wraps a word earlier.
        let mut plain = inst_sheet(1440);
        plain.hdr_track = [0.0; 3];
        let mut g0 = gs();
        let laid0 = layout_block(&t.frozen_blocks()[0], 733, &plain, &mut g0);
        assert_eq!(line_text(&laid0.lines[0]).trim_end(), "Compositor geometry should remain");
    }

    /// 7.4 (I-5c): the producer's prompt inks stand under the Instrument
    /// table -- an explicit SGR colour on a prompt run is never re-inked by
    /// the role default, so the session's `λ` (amber), the cwd
    /// (terminal_path) and the `⊢` (secondary) reach the pixels as the
    /// export named them, the input after the delimiter in the role's `text`,
    /// all at the body size on the Sans.
    #[test]
    fn the_producers_prompt_inks_pass_through_the_instrument_table() {
        let s = inst_sheet(1280);
        let (a, p, d) = (s.inst.amber, s.inst.terminal_path, s.inst.secondary);
        let sgr = |c: u32| alloc::format!("\x1b[38;2;{};{};{}m", (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        let line = alloc::format!(
            "{}\u{3bb} \x1b[0m{}~/src\x1b[0m{} \u{22a2} \x1b[0mls -l\n",
            sgr(a),
            sgr(p),
            sgr(d)
        );
        let t = inst_transcript(s.theme.terminal, |buf| {
            wire::open(buf, BOp::Zone, &[("k", "prompt")]);
            buf.extend_from_slice(line.as_bytes());
            wire::close(buf, BOp::Zone);
        });
        let mut g = gs();
        let laid = layout_block(&t.frozen_blocks()[0], 600, &s, &mut g);
        let segs = &laid.lines[0].segs;
        let inks: Vec<u32> = segs.iter().map(|sg| sg.color).collect();
        assert_eq!(inks, alloc::vec![a, p, d, s.inst.text], "lambda, path, delimiter, input");
        assert!(segs.iter().all(|sg| sg.face == FACE_SANS && sg.px == 15.0), "the body size on the Sans");
        assert!(segs[0].x_end <= segs[1].x && segs[1].x_end <= segs[2].x, "left to right");
        assert_ne!(a, s.ink_prompt, "the amber is not the prompt role's default (else this proves nothing)");
    }

    /// 7.2: an SGR italic takes the true Italic cell in a mono run and the
    /// italic face on an annotated proportional run -- under Instrument;
    /// legacy never read the attribute (byte-pinned).
    #[test]
    fn sgr_italic_takes_the_italic_faces_under_instrument_only() {
        let s = inst_sheet(1280);
        let build = |buf: &mut Vec<u8>| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            wire::open(buf, BOp::Pre, &[]);
            buf.extend_from_slice(b"\x1b[3mcomment\x1b[0m code\n");
            wire::close(buf, BOp::Pre);
            wire::open(buf, BOp::Obj, &[("type", "path"), ("ref", "/x")]);
            buf.extend_from_slice(b"\x1b[3mslanted\x1b[0m");
            wire::close(buf, BOp::Obj);
            buf.extend_from_slice(b"\n");
            wire::close(buf, BOp::Zone);
        };
        let t = inst_transcript(s.theme.terminal, build);
        let mut g = gs();
        let laid = layout_block(&t.frozen_blocks()[0], 600, &s, &mut g);
        assert_eq!(laid.lines[0].segs[0].face, FACE_MONO_ITALIC, "the Italic cell");
        assert_eq!(laid.lines[0].segs[1].face, FACE_MONO, "the Regular cell beside it");
        assert_eq!(laid.lines[0].segs[0].px, laid.lines[0].segs[1].px, "one cell, two faces");
        assert_eq!(laid.lines[1].segs[0].face, FACE_HEADING_ITALIC, "the annotated run's italic");
        let l = daylight_sheet(100);
        let t = inst_transcript(daylight(), build);
        let legacy = layout_block(&t.frozen_blocks()[0], 600, &l, &mut g);
        assert_eq!(legacy.lines[0].segs[0].face, FACE_MONO);
        assert_eq!(legacy.lines[1].segs[0].face, FACE_BODY);
    }

    /// A caret resting on the paragraph break (a blank row of the live
    /// grid in a document zone) keeps the body's pitch under Instrument;
    /// the line itself stays zero-height.
    #[test]
    fn a_caret_on_the_paragraph_break_keeps_the_bodys_pitch() {
        let s = inst_sheet(1280);
        let t = inst_transcript(s.theme.terminal, |buf| {
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            em(buf, "strong", "A");
            buf.extend_from_slice(b" line\n\n");
            wire::close(buf, BOp::Zone);
        });
        let mut g = gs();
        let laid = layout_block(&t.frozen_blocks()[0], 600, &s, &mut g);
        assert_eq!(laid.lines[1].h, 0);
        let (_, y, h) = caret_in_block(&laid, 1, usize::MAX, 0, &s);
        assert_eq!((y, h), (laid.lines[1].y, 24), "the caret box is the body pitch on a zero-height line");
        let (_, _, h0) = caret_in_block(&laid, 0, usize::MAX, 0, &s);
        assert_eq!(h0, laid.lines[0].h, "a laid line keeps its own box");
    }

    /// 7.5: H1, prose and the prompt wrap at the 720 measure; H2 and raw
    /// output run the width; the block gap is the paragraph margin, and a
    /// prompt runs straight into its output.
    #[test]
    fn h1_and_prose_cap_at_the_measure_while_h2_runs_the_width() {
        let long = "word ".repeat(80);
        let s = inst_sheet(1440);
        let t = inst_transcript(s.theme.terminal, |buf| {
            wire::open(buf, BOp::Zone, &[("k", "prompt")]);
            buf.extend_from_slice(b"~ > cat\n");
            wire::close(buf, BOp::Zone);
            wire::open(buf, BOp::Zone, &[("k", "output")]);
            hdr(buf, "1", &long);
            hdr(buf, "2", &long);
            buf.extend_from_slice(long.as_bytes());
            buf.extend_from_slice(b"\n");
            wire::close(buf, BOp::Zone);
        });
        let mut g = gs();
        let blocks = t.frozen_blocks();
        let laid = layout_block(&blocks[1], 1400, &s, &mut g);
        let cap_right = 43 + 720;
        let h1: Vec<&LaidLine> = laid.lines.iter().filter(|l| l.src_item == 0).collect();
        assert!(h1.len() > 1 && h1.iter().all(|l| l.segs.last().unwrap().x_end <= cap_right), "H1 wraps at the measure");
        let h2: Vec<&LaidLine> = laid.lines.iter().filter(|l| l.src_item == 1).collect();
        assert!(h2.iter().any(|l| l.segs.last().unwrap().x_end > cap_right), "H2 runs past it");
        assert!(h2.iter().all(|l| l.segs.last().unwrap().x_end <= 1400 - 43));
        let p: Vec<&LaidLine> = laid.lines.iter().filter(|l| l.src_item == 2).collect();
        assert!(p.len() > 1 && p.iter().all(|l| l.segs.last().unwrap().x_end <= cap_right), "prose wraps at the measure");
        assert_eq!(block_gap_between(BlockKind::Prompt, BlockKind::Output, &s), 0);
        assert_eq!(block_gap_between(BlockKind::Output, BlockKind::Output, &s), 15);
        assert_eq!(block_gap_between(BlockKind::Output, BlockKind::Prompt, &s), 15);
    }
}
