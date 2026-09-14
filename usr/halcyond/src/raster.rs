// The glyph source: the vendored faces through the outline path (skrifa +
// zeno, `outline.rs`; HALCYON-TYPE section 4) -> a cartoon atlas, cached.
// Since TY-4 that includes the MONO tier: the system face is Cornucopia's
// subset outline, rasterized into the cell, not the baked atlases (which
// stay for the consumers that must carry no rasterizer -- Aurora, the
// kernel trusted sink, Halls). One path, so one stroke rule: a cell now
// carries the theme's smoothing like the prose beside it.
// The author-side half of the 13.2 division of knowledge -- layout asks
// THIS for glyph ids + advances and writes resolved runs; executors never
// see a font, only the finished alpha pages.
//
// Sizes are quantized to half pixels for the cache key (the stylesheet
// speaks whole px today; the quantum keeps a future fractional size from
// silently splitting the cache). Advances are rounded to integer pixels
// (the MVP pen; quarter-pixel phases are HALCYON-TYPE's TY-3). The
// smoothing stroke (`smooth_mem`, the theme's) is a property of the whole
// store: a change regens, so no cached raster carries a stale amount.

use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use cartoon::{AtlasPacker, GlyphRef};

use crate::outline::Face;

/// A face slot in this source. The proportional faces are the vendored IBM
/// Plex Sans cuts (HALCYON-VISUAL.md section 7 + HALCYON.md section 4 for
/// the legacy four; HALCYON-INSTRUMENT 7.1 for the Instrument three); the
/// monospace ones are Cornucopia's subset outlines (`usr/lib/cornucopia`),
/// all served through the SAME packer/id space so one atlas store feeds the
/// executor.
///
/// Two KINDS of mono slot (HALCYON-INSTRUMENT 7.2, I-5). The CELL faces
/// (`FACE_MONO`, `FACE_MONO_ITALIC`) rasterize into the fixed cell the
/// cells tier shares -- the alt-screen grid, `pre` islands, raw output:
/// wherever columns must align and the procedural box glyphs join. The
/// TEXT face (`FACE_MONO_TEXT`) runs Cornucopia FREE at any px, like a
/// proportional face: the sub-pixel pen, the four phases, the font's own
/// line metrics. The chrome's mono roles -- the header's index and
/// metadata, the clock, the footer, a menu hint -- are 10 and 11 px runs
/// with no grid, where the cell's 6 px advance floor was the wrong tool
/// (I-3 and I-4 laid them at the 12 px island size and said so).
///
/// The legacy profile's four Plex cuts keep slots 0..3 (the operator's
/// rule: baseline = Text 450, bigger type = Regular 400 italic, the one
/// bold -- so heading-italic is its own slot, a DISTINCT weight from the
/// body italic); the Instrument profile's three (400 / 500 / 600) sit above
/// the mono sentinel. `FACE_MONO` stays 4: it is a SENTINEL in the cache
/// key and in tests, never an index -- `prop_slot` maps every proportional
/// id to its `faces[]` slot, so no id is ever used as an index by accident.
pub const FACE_BODY: u8 = 0; // Plex Text (450): the legacy body, prompt, chrome, objects
pub const FACE_BODY_BOLD: u8 = 1; // Plex Bold (700): em--strong, the one bold
pub const FACE_BODY_ITALIC: u8 = 2; // Plex Text Italic (450): the legacy em--emph
pub const FACE_HEADING_ITALIC: u8 = 3; // Plex Regular Italic (400): legacy headings; Instrument emphasis
pub const FACE_MONO: u8 = 4; // Cornucopia Regular in the CELL
pub const FACE_SANS: u8 = 5; // Plex Regular (400): the Instrument body, rails, buttons, pills
pub const FACE_SANS_MEDIUM: u8 = 6; // Plex Medium (500): header names, headings, strong chrome
pub const FACE_SANS_SEMIBOLD: u8 = 7; // Plex SemiBold (600): the brand
pub const FACE_MONO_ITALIC: u8 = 8; // Cornucopia Italic in the CELL (comments, lifetimes)
pub const FACE_MONO_TEXT: u8 = 9; // Cornucopia Regular free-running (the chrome's mono roles)

/// Whether `face` is one of the CELL mono faces (a fixed cell, no phase).
#[inline]
pub fn is_cell_face(face: u8) -> bool {
    face == FACE_MONO || face == FACE_MONO_ITALIC
}

/// Whether `face` is any Cornucopia face -- a cell or the free-running one.
/// What a line box asks ("does this run grow the box?"): a mono run never
/// does (HALCYON-COMPOSITION 4), in either kind.
#[inline]
/// `f32::round` for a `no_std` crate: half away from zero, the rule the
/// kern and tracking terms need on both signs (`(x + 0.5) as i32` rounds
/// -335.87 to -335).
pub fn round_half_away(x: f32) -> i32 {
    if x >= 0.0 {
        (x + 0.5) as i32
    } else {
        (x - 0.5) as i32
    }
}

pub fn is_mono_face(face: u8) -> bool {
    is_cell_face(face) || face == FACE_MONO_TEXT
}

/// Whether `face` is one of the Instrument Sans cuts (whose missing
/// symbols the free-running mono serves at the same px).
#[inline]
pub fn is_instrument_sans(face: u8) -> bool {
    face == FACE_SANS || face == FACE_SANS_MEDIUM || face == FACE_SANS_SEMIBOLD
}

/// The `faces[]` slot of a proportional id; None for a mono id or an
/// unknown one. Explicit, so the mono sentinel sitting in the middle of
/// the id space can never be mistaken for a slot.
#[inline]
fn prop_slot(face: u8) -> Option<usize> {
    match face {
        FACE_BODY => Some(0),
        FACE_BODY_BOLD => Some(1),
        FACE_BODY_ITALIC => Some(2),
        FACE_HEADING_ITALIC => Some(3),
        FACE_SANS => Some(4),
        FACE_SANS_MEDIUM => Some(5),
        FACE_SANS_SEMIBOLD => Some(6),
        _ => None,
    }
}

/// The proportional face an id names, out of the slot table. A free
/// function over the field (not a `&self` method) so a caller holding it
/// can still borrow the packer: the borrow is on `faces` alone.
#[inline]
fn prop_of(faces: &[Option<Face>], face: u8) -> Option<&Face> {
    faces.get(prop_slot(face)?)?.as_ref()
}

/// The vendored proportional faces in `prop_slot` order: the legacy four
/// (Text, Bold, Text-Italic, the Regular-weight heading italic), then the
/// Instrument three (Regular, Medium, SemiBold). ONE list: `new_vendored`
/// parses it, `VENDORED_FACES` is its length, and both owners' startup
/// guards compare against THAT -- a literal count in the bins (`!= 4`,
/// which no host test could see) was what made halcyond refuse to start
/// on the first image that carried seven, at I-5a.
const VENDORED: [&[u8]; 7] = [
    crate::IBM_PLEX_SANS_TEXT,
    crate::IBM_PLEX_SANS_BOLD,
    crate::IBM_PLEX_SANS_TEXT_ITALIC,
    crate::IBM_PLEX_SANS_HEADING_ITALIC,
    crate::IBM_PLEX_SANS_REGULAR,
    crate::IBM_PLEX_SANS_MEDIUM,
    crate::IBM_PLEX_SANS_SEMIBOLD,
];
/// How many proportional faces a healthy source holds: the startup
/// guards' expectation, derived from the list rather than restated.
pub const VENDORED_FACES: usize = VENDORED.len();

/// Whether the Italic cut lands in the Regular's cell at EVERY advance the
/// scale table can ask for: the cells tier's contract, judged from the two
/// faces' own tables (`Face::mono_cell` -- upem, the OS/2 Windows pair, the
/// advance of `x`). The subset tool refuses a cut that differs; this is the
/// runtime's own check of the same fact, so a re-cut that skipped the tool
/// cannot ship an italic on the wrong grid.
fn italic_shares_the_cell(regular: &Face, italic: &Face) -> bool {
    (MONO_ISLAND_ADVANCE..=MONO_ADVANCE_MAX).all(|a| regular.mono_cell(a) == italic.mono_cell(a))
}

/// The two mono SIZES at 100% (HALCYON-COMPOSITION 2; Cornucopia bakes 0.5
/// em per advance px), LOGICAL px: the Sheet scales them (HALCYON-SCALE 6)
/// and the atlases are selected by advance (`mono_advances`). The ISLAND is
/// the document's mono -- inline `em class=code`, a `pre` block, raw
/// terminal output, the menu's literals -- the advance-6 bake (6x14, a 12
/// px em: the closest cell to the mockup's 10 px Cornucopia, advance 5
/// being below the box-glyph legibility floor). The GRID is the alt-screen
/// / pts cell (advance 10, 10x22): a full-screen program owns its cells and
/// the pts geometry is sized from it. `glyph`/`line_metrics` pick the atlas
/// from the requested px (the island below the midpoint of the two selected
/// ems, the grid from it), so a caller says which mono it means the same
/// way it says a proportional size -- at the SHEET's size, never these.
pub const MONO_ISLAND_PX: f32 = 12.0;
/// ONE mono size (the operator, 2026-09-08, on the first live look: a
/// fullscreen mono program ran bigger than the preformatted block -- "they
/// should use the preformatted block's size, so it's uniform"): the grid
/// em IS the island em. The island/grid plumbing stays (two atlas slots,
/// one bake in both) so a later split costs nothing; the numbers agree.
pub const MONO_GRID_PX: f32 = MONO_ISLAND_PX;
/// The mono advance at 100%: 6 -- the island's, and since 2026-09-08 the
/// grid's too.
pub const MONO_ISLAND_ADVANCE: u8 = 6;
pub const MONO_GRID_ADVANCE: u8 = MONO_ISLAND_ADVANCE;

/// The atlas page bound `evict_if_full` enforces between frames at the
/// reference display (1280x800): 16 pages of the 512-px page every source
/// is built with = 4 MiB of alpha, ~16x a Latin working set (four faces at
/// three sizes plus both mono cells pack into about one page). The
/// transcript's bytes are untrusted; without a bound a program printing
/// distinct codepoints grew the store ~10 MB per size until the
/// compositor's fixed heap died mute (I-32's in-process face). The painted
/// set is bounded by the DISPLAY AREA, which a larger scanout multiplies
/// (HALCYON-SCALE 7), so the live bound is `atlas_pages_for` -- this
/// constant is its floor, and at 1280x800 its value.
pub const MAX_ATLAS_PAGES: usize = 16;
/// The in-frame slack above the eviction bound: the packer's HARD cap is
/// the bound + `ATLAS_PAGE_SLACK` pages (6 MiB at the floor), past which an
/// insert is refused (the glyph paints blank this frame; the next frame's
/// eviction re-packs). The eviction bounds the steady state; this bounds
/// the frame, whose insert count the untrusted stream would otherwise
/// decide. Since layout measures with `advance` (no packing), a frame
/// inserts only what it paints -- a screen of glyphs, a few pages -- so the
/// slack is never reached by honest content and the cap never bites the
/// eviction's re-pack of a visible set.
pub const ATLAS_PAGE_SLACK: usize = 8;

/// The eviction bound for a `display_w x display_h` scanout on `page`-px
/// pages (HALCYON-SCALE 7): the visible glyph area is at most the screen
/// area and shelf packing wastes up to about half a page per size and
/// shelf, so twice the display area in pages, never below the floor. At
/// 1280x800 on 512-px pages this is the floor (16); a 4K scanout is 64.
pub fn atlas_pages_for(display_w: u32, display_h: u32, page: u32) -> usize {
    let page_area = (page as u64 * page as u64).max(1);
    let display_area = display_w as u64 * display_h as u64;
    let want = (2 * display_area).div_ceil(page_area) as usize;
    want.max(MAX_ATLAS_PAGES)
}

/// Whether the compiled-in system mono faces are usable: the Regular
/// parses AND yields a cell at the floor advance, and the Italic parses
/// AND shares that cell at every advance. The startup check, callable
/// before any `GlyphSource` exists so BOTH renderer paths can make it --
/// the `cornucopia::verify_all` this replaced sat after the session path's
/// early return and so only ever guarded the console one. Both faces are
/// build inputs: a false here is a defect to fix at build time, reported
/// loudly rather than discovered as a hole in the grid or an upright
/// "italic".
pub fn mono_face_ok() -> bool {
    let Some(regular) = Face::parse(cornucopia::SUBSET_TTF) else {
        return false;
    };
    if regular.mono_cell(MONO_ISLAND_ADVANCE).is_none() {
        return false;
    }
    Face::parse(cornucopia::SUBSET_ITALIC_TTF).is_some_and(|it| italic_shares_the_cell(&regular, &it))
}

/// The largest mono advance this source will serve. The scale range caps
/// the island at 12 (200% of 6), so this is headroom, not a live limit --
/// it exists because the cell is what the outline is rasterized into, and
/// an unbounded advance is an unbounded raster on the untrusted side of
/// I-32 (a hostile EDID reaching an unclamped percent). 20 is the largest
/// cell the bake ever cut, so it is also the largest any cells-tier
/// consumer has geometry for.
pub const MONO_ADVANCE_MAX: u8 = 20;

/// The mono advances at a display scale (HALCYON-SCALE 6): `round_half_up(6
/// x s)` -- 6, 8, 9, 11, 12 at the five values -- bounded below by the
/// legibility floor of 6 (the procedural box glyphs need it) and above by
/// `MONO_ADVANCE_MAX`. Returned as (island, grid) for the two cell slots,
/// and since 2026-09-08 the two are EQUAL (one mono size: the grid runs at
/// the preformatted block's). Pure: the Sheet derives its mono ems from the
/// same answer the source selects its cells by, so the two cannot disagree.
///
/// Since TY-4 there is no "nearest smaller bake" step: the cell is cut from
/// the outline at whatever advance is asked, so every advance in range is
/// available and the answer is the scale table itself. The five reachable
/// percents were all baked sizes anyway, so no value here moved -- which
/// the test pins as literals rather than re-deriving.
pub fn mono_advances(pct: u16) -> (u8, u8) {
    let want = libhalcyon::scale::ipx(MONO_ISLAND_ADVANCE as i32, pct);
    let one = want.clamp(MONO_ISLAND_ADVANCE as i32, MONO_ADVANCE_MAX as i32) as u8;
    (one, one)
}

/// The phase a sub-pixel remainder (1/256 px) rounds to: the nearest
/// quarter of a pixel, clamped to the last one. A remainder in the top
/// eighth would round to a whole pixel -- i.e. to the NEXT pixel's phase
/// 0 -- and moving the glyph there would desynchronise it from the whole
/// pen the executor reconstructs, so it is clamped instead. The cost is
/// bounded and unaccumulated, and it is NOT uniform: 1/8 px across the
/// unclamped band (remainders 0..223), rising through the clamped tail to
/// **0.246 px at remainder 255** -- measured over the whole domain at
/// TY-6 F5, which found this comment claiming 1/8 everywhere.
#[inline]
pub fn phase_of(rem_256: i32) -> u8 {
    (((rem_256 + 32) / 64).clamp(0, 3)) as u8
}

/// Per-(face, size) vertical metrics, integer pixels, y-down. `ascent` is
/// baseline distance from the line top; `line_height` includes the gap.
#[derive(Clone, Copy)]
pub struct LineMetrics {
    pub ascent: i32,
    pub descent: i32,
    pub line_height: i32,
}

struct Cached {
    id: u32,
    advance: i32,
}

/// One monospace cell: the geometry the cells tier shares (width, height,
/// baseline rows from the top) plus the em the outline is rasterized at to
/// fill it. Derived from the mono face's own tables by the bake's formula
/// (`Face::mono_cell`), so the live path and the baked atlases land on the
/// same grid.
#[derive(Clone, Copy)]
struct MonoCell {
    w: i32,
    h: i32,
    baseline: i32,
    em: f32,
}

impl MonoCell {
    /// The cell `advance` names in `face`. None only for a face with no
    /// usable metrics -- a build-input defect the startup check catches.
    fn derive(face: &Face, advance: u8) -> Option<MonoCell> {
        let (h, baseline, em) = face.mono_cell(advance)?;
        Some(MonoCell { w: advance as i32, h, baseline, em })
    }

    /// The cell used when the mono face is unusable. Only reachable in a
    /// process that has already failed `GlyphSource::mono_ok` and is on its
    /// way out (halcyond's startup check, the `verify_all` of the bakes it
    /// replaced): square, non-degenerate, and safe for every division and
    /// subtraction downstream. No glyph renders into it.
    fn degenerate(advance: u8) -> MonoCell {
        let w = advance.max(1) as i32;
        MonoCell { w, h: w, baseline: w, em: w as f32 }
    }
}

/// Rasterize `gid` from the mono face INTO the cell: the outline at the
/// cell's em, placed with the pen at the cell's left edge and the baseline
/// where the cell puts it, then clipped to the cell.
///
/// The clip is the contract, not a limitation: every consumer of a mono
/// glyph -- the alt-screen grid, the pts geometry, the procedural box
/// glyphs it joins against -- expects a cell to paint its own cell and no
/// other, and the bake it replaces clipped identically (it rasterized into
/// a cell-sized grid). A glyph whose ink exceeds its advance therefore
/// loses the overhang here exactly as it lost it there.
fn mono_cell_alpha(
    face: &Face,
    gid: skrifa::GlyphId,
    cell: MonoCell,
    smooth_mem: u16,
) -> Vec<u8> {
    // Phase 0 always: a fixed cell has no sub-pixel placement to carry.
    let r = face.raster(gid, cell.em, smooth_mem, 0);
    let mut out = alloc::vec![0u8; (cell.w * cell.h) as usize];
    let (x0, y0) = (r.left, cell.baseline - r.top);
    for y in 0..r.h as i32 {
        let cy = y0 + y;
        if cy < 0 || cy >= cell.h {
            continue;
        }
        for x in 0..r.w as i32 {
            let cx = x0 + x;
            if cx < 0 || cx >= cell.w {
                continue;
            }
            out[(cy * cell.w + cx) as usize] = r.alpha[(y * r.w as i32 + x) as usize];
        }
    }
    out
}

/// Fonts + packer + cache, one generation at a time. `regen()` evicts all
/// three together, so a cached id can never outlive the pages it points
/// into (the 13.2 stale rule holds by construction on the author side
/// too; the executor's gen check is the belt).
pub struct GlyphSource {
    /// The proportional faces by `prop_slot`. A slot is None only when its
    /// bytes failed to parse -- a build-input defect (the faces are
    /// compiled in) that must never SHIFT the slots after it, which a
    /// positional `Vec<Face>` with a skipped push would have done.
    faces: Vec<Option<Face>>,
    /// The system monospace face -- the Cornucopia subset outline, live
    /// since TY-4 (HALCYON-TYPE 4.5). It replaced the baked atlases here
    /// so the mono tier carries the theme's smoothing stroke like every
    /// other tier; the bakes stay for the consumers that must not carry a
    /// rasterizer (Aurora, the kernel trusted sink, Halls). Serves both
    /// `FACE_MONO` (the cell) and `FACE_MONO_TEXT` (free-running).
    mono: Option<Face>,
    /// The true Italic, in the SAME cell (`italic_shares_the_cell`; a cut
    /// that would not share it is refused here and the Regular serves
    /// italic requests upright -- `mono_face_ok` reports it at startup).
    mono_italic: Option<Face>,
    grid: MonoCell,
    island: MonoCell,
    pub packer: AtlasPacker,
    /// Keyed by (face, size quantum, horizontal phase, char). The phase is
    /// part of the key because a phased raster IS a different bitmap
    /// (HALCYON-TYPE 4.3); FACE_MONO is always phase 0 (a fixed cell has
    /// no phase).
    cache: BTreeMap<(u8, u32, u8, char), Cached>,
    /// I-5d: whether proportional runs are kerned (the Instrument profile;
    /// the owner sets it from `Sheet.kerning`). Off, `kern` is 0 for every
    /// pair -- the legacy bytes.
    kerning: bool,
    /// Pair adjustments in FONT UNITS by (face, left, right): size-free, so
    /// a rescale evicts nothing; bounded by a clear at `KERN_MEMO_MAX`.
    kern_memo: BTreeMap<(u8, char, char), i32>,
    /// The display scale the mono atlases were selected for (percent).
    scale: u16,
    /// The smoothing stroke every proportional raster carries, in
    /// thousandths of an em (`Theme.smooth_mem`; 0 = the plain fill).
    smooth_mem: u16,
    /// The between-frames eviction bound (`atlas_pages_for` of the last
    /// `set_display`; the floor before one).
    evict_pages: usize,
}

/// The cache key's size: the EXACT bits of the requested px.
///
/// It was a half-pixel quantum until TY-6 F2, justified by "the stylesheet
/// speaks whole px today" -- which was false when it was written. The exit
/// badge lays at `body_px * 0.9` (10.35 at 100%) and the chrome/menu name
/// at 10.5; both quantized to 21, so ONE cache entry served both and the
/// raster each got was whichever painted first -- re-decided at every
/// regen, and flipping with whether a failed command happened to be on
/// screen. The same collision at 125% (12.9375 and 13.125 -> 26).
///
/// Exact bits cannot alias. Two sizes that differ only below the float's
/// resolution are the same size. The cost is that two NEARLY equal sizes
/// no longer share entries, which is bounded: the live set is whatever the
/// sheet names for one scale, a handful of sizes, and a scale change
/// regens the store anyway.
#[inline]
fn size_q(px: f32) -> u32 {
    px.to_bits()
}

impl GlyphSource {
    /// Which mono atlas a requested size means: the grid cell from the
    /// midpoint of the two SELECTED ems up (an em is twice the bake's
    /// advance), the island below it. Two atlases, so the cache key is the
    /// choice, not the px -- every island request shares one entry per
    /// glyph -- and the threshold follows `set_scale`, so a sheet's scaled
    /// island em lands on the island at every scale.
    #[inline]
    fn mono_is_grid(&self, px: f32) -> bool {
        px >= (self.island.w + self.grid.w) as f32
    }

    /// Whether the system mono face parsed and yielded a cell. False is a
    /// build-input defect (the subset TTF is compiled in), and halcyond
    /// fails loudly at startup on it rather than discovering it as a hole
    /// in the grid -- the role `cornucopia::verify_all` played for the
    /// bakes this replaced.
    pub fn mono_ok(&self) -> bool {
        self.mono.is_some()
    }

    /// Build over the vendored faces at 100% on the floor bound. `page` is
    /// the atlas page geometry (one page holds many shelves; 512 fits
    /// several sizes of a Latin working set).
    pub fn new_vendored(page: u32) -> GlyphSource {
        // `VENDORED` in `prop_slot` order. The vendored faces parse by
        // construction; a parse reject here is a build-input defect, not a
        // runtime input -- panic in tests, but stay total in the API: the
        // SLOT stays (None), so no later face shifts into it, and that
        // face's glyphs miss per the caller (the startup guard, comparing
        // `face_count` to `VENDORED_FACES`, refuses to start on it).
        let faces: Vec<Option<Face>> = VENDORED.into_iter().map(Face::parse).collect();
        let mut packer = AtlasPacker::new(page, page);
        packer.set_max_pages((MAX_ATLAS_PAGES + ATLAS_PAGE_SLACK) as u32);
        // The system mono face. A None here is the same class of defect a
        // failed `Face::parse` is above -- caught loudly by `mono_ok` at
        // startup, not silently by an empty grid.
        let mono = Face::parse(cornucopia::SUBSET_TTF)
            .filter(|f| MonoCell::derive(f, MONO_ISLAND_ADVANCE).is_some());
        // The Italic, admitted only into the Regular's cell.
        let mono_italic = Face::parse(cornucopia::SUBSET_ITALIC_TTF)
            .filter(|it| mono.as_ref().is_some_and(|re| italic_shares_the_cell(re, it)));
        let cell = |a: u8| {
            mono.as_ref()
                .and_then(|f| MonoCell::derive(f, a))
                .unwrap_or_else(|| MonoCell::degenerate(a))
        };
        GlyphSource {
            faces,
            grid: cell(MONO_GRID_ADVANCE),
            island: cell(MONO_ISLAND_ADVANCE),
            mono,
            mono_italic,
            packer,
            cache: BTreeMap::new(),
            kerning: false,
            kern_memo: BTreeMap::new(),
            scale: 100,
            smooth_mem: 0,
            evict_pages: MAX_ATLAS_PAGES,
        }
    }

    /// The display scale the mono atlases serve (percent).
    pub fn scale(&self) -> u16 {
        self.scale
    }

    /// The smoothing stroke the proportional rasters carry (thousandths of
    /// an em).
    pub fn smooth(&self) -> u16 {
        self.smooth_mem
    }

    /// Set the smoothing stroke (HALCYON-TYPE 4.2: the theme's amount --
    /// `Theme.smooth_mem`; 0 = the plain fill) and evict everything, so no
    /// cached raster carries the old amount. A no-op (false) at the current
    /// value. Em-relative, so a display-scale change needs no re-set.
    pub fn set_smooth(&mut self, mem: u16) -> bool {
        if mem == self.smooth_mem {
            return false;
        }
        self.smooth_mem = mem;
        self.regen();
        true
    }

    /// Switch pair kerning on or off (I-5d: on under the Instrument profile,
    /// off under legacy -- `Sheet.kerning`). Returns whether it changed; a
    /// change moves every proportional pen, so the caller relays out.
    /// Nothing rasterized depends on it, so no eviction.
    pub fn set_kerning(&mut self, on: bool) -> bool {
        if on == self.kerning {
            return false;
        }
        self.kerning = on;
        true
    }

    /// Whether pair kerning is on.
    pub fn kerning(&self) -> bool {
        self.kerning
    }

    /// Select the mono atlases for a display scale (HALCYON-SCALE 6: the
    /// island and grid bakes `mono_advances` names) and evict everything --
    /// the author's size-change point: every cached id and every page went
    /// with the old cells, and the packer's generation bumps so no executor
    /// can read a stale run. A no-op (false) at the current scale.
    pub fn set_scale(&mut self, pct: u16) -> bool {
        if pct == self.scale {
            return false;
        }
        let (island, grid) = mono_advances(pct);
        let cell = |a: u8| {
            self.mono
                .as_ref()
                .and_then(|f| MonoCell::derive(f, a))
                .unwrap_or_else(|| MonoCell::degenerate(a))
        };
        self.island = cell(island);
        self.grid = cell(grid);
        self.scale = pct;
        self.regen();
        true
    }

    /// Re-derive the atlas bound for a display (HALCYON-SCALE 7): the
    /// between-frames eviction bound follows the display area, the packer's
    /// hard cap sits `ATLAS_PAGE_SLACK` above it. Called at start and on
    /// every display change; a shrink leaves the store to the next
    /// `evict_if_full`, which sees it over the new bound.
    pub fn set_display(&mut self, display_w: u32, display_h: u32) {
        self.evict_pages = atlas_pages_for(display_w, display_h, self.packer.page_w());
        self.packer.set_max_pages((self.evict_pages + ATLAS_PAGE_SLACK) as u32);
    }

    /// The between-frames eviction bound in pages (the hard cap is this plus
    /// `ATLAS_PAGE_SLACK`).
    pub fn evict_pages(&self) -> usize {
        self.evict_pages
    }

    fn mono_cell_at(&self, px: f32) -> MonoCell {
        if self.mono_is_grid(px) {
            self.grid
        } else {
            self.island
        }
    }

    /// The GRID mono cell geometry (w, h, baseline): the alt-screen raw-VT
    /// cell and the pts geometry (cols/rows) every tile is sized from.
    pub fn mono_cell(&self) -> (i32, i32, i32) {
        (self.grid.w, self.grid.h, self.grid.baseline)
    }

    /// The ISLAND mono cell geometry (w, h, baseline): the document's mono
    /// -- islands, pre, raw output, the menu's literals.
    pub fn island_cell(&self) -> (i32, i32, i32) {
        (self.island.w, self.island.h, self.island.baseline)
    }

    /// How many proportional faces parsed (the slots that are Some).
    pub fn face_count(&self) -> usize {
        self.faces.iter().flatten().count()
    }

    /// Whether the true Italic is in service (parsed AND in the Regular's
    /// cell); false means `FACE_MONO_ITALIC` is served upright.
    pub fn mono_italic_ok(&self) -> bool {
        self.mono_italic.is_some()
    }

    /// The Cornucopia face a CELL id names: the Regular, or the Italic
    /// when it is in service (the Regular otherwise, upright -- total, so
    /// a refused italic degrades to roman rather than to a hole).
    #[inline]
    fn cell_face(&self, face: u8) -> Option<&Face> {
        if face == FACE_MONO_ITALIC {
            self.mono_italic.as_ref().or(self.mono.as_ref())
        } else {
            self.mono.as_ref()
        }
    }

    /// The advance of `ch` at `px` in `face` -- metrics only, from the
    /// font's tables: NOTHING is rasterized or packed. This is what layout
    /// measures with, so laying a block out touches no atlas page and the
    /// atlas working set is exactly what a frame PAINTS (`glyph`), never
    /// what the transcript holds. Agrees with `glyph`'s advance for every
    /// codepoint, including the mono cell and the island fallback for a
    /// codepoint the proportional face lacks. None only for an unknown
    /// face.
    pub fn advance(&mut self, face: u8, px: f32, ch: char) -> Option<i32> {
        let q = if is_cell_face(face) {
            self.mono_is_grid(px) as u32
        } else {
            size_q(px)
        };
        // The advance does not depend on the phase, so the probe uses
        // phase 0 and a miss simply re-derives it from the tables.
        if let Some(c) = self.cache.get(&(face, q, 0, ch)) {
            return Some(c.advance);
        }
        if is_cell_face(face) {
            return Some(self.mono_cell_at(px).w);
        }
        // The integer path IS the fractional path's `+0.5` truncation, for
        // every face and every fallback arm -- one rule, so a caller that
        // measures with one and paints with the other cannot drift (the
        // test `the_fractional_advance_rounds_to_the_integer_one`).
        self.advance_f(face, px, ch).map(|a| (a + 0.5) as i32)
    }

    /// The FRACTIONAL advance of `ch` at `px` in `face` -- what a
    /// sub-pixel pen accumulates (HALCYON-TYPE 4.3). Rounding this is
    /// `advance()` exactly, so a caller that measures with one and paints
    /// with the other cannot drift: the integer path is the fractional
    /// path's `+0.5` truncation, by construction and by test. The mono
    /// cells and the island fallback are whole by nature (a fixed cell),
    /// so they return their integer width as an f32. Packs nothing.
    /// The pen's fixed-point scale: 1/256 px. Deliberately FINER than the
    /// four phases it feeds (HALCYON-TYPE 4.3). The phase is a placement
    /// decision taken per glyph and never accumulated; the PEN is a
    /// running total, so quantizing it to quarters would compound up to
    /// 1/8 px of error per glyph -- measured at 1.4 px over one line of
    /// prose, which is most of the drift sub-pixel placement exists to
    /// remove. At 1/256 the same line drifts by hundredths.
    pub const PEN_SCALE: i32 = 256;

    /// The kern memo's bound (pairs); a session's text reuses a few
    /// thousand distinct pairs, so this is rarely reached and cheap when it is.
    const KERN_MEMO_MAX: usize = 4096;

    /// The advance in 1/256 px -- what the sub-pixel pen accumulates.
    pub fn advance_fx(&mut self, face: u8, px: f32, ch: char) -> Option<i32> {
        self.advance_f(face, px, ch)
            .map(|a| (a * Self::PEN_SCALE as f32 + 0.5) as i32)
    }

    /// Shape a SINGLE-STYLE run at the sub-pixel pen: each glyph carries
    /// the WHOLE step to the next one, and the returned width is their
    /// sum, so a caller that measures with this and paints with this
    /// cannot disagree with itself. The one place the sub-pixel pen is
    /// implemented for simple runs -- the chrome strip, the status bar,
    /// the menu; the transcript's own loop needs wrapping and per-segment
    /// styles and keeps its own, against the same `advance_fx`.
    ///
    /// A glyph the atlas cannot serve is skipped WITHOUT advancing the
    /// pen, which is what these callers did before and what keeps a
    /// refused glyph from opening a hole in the run.
    ///
    /// The KERN is folded into the preceding glyph's step, exactly as the
    /// transcript's own lay loop does. Zero for every pair today, so it
    /// moves nothing -- it is here so the GPOS seam cannot land a shaper
    /// that the chrome strip, the status bar and the menu silently ignore.
    pub fn shape_run(
        &mut self,
        face: u8,
        px: f32,
        chars: impl Iterator<Item = char>,
    ) -> (Vec<GlyphRef>, i32) {
        self.shape_run_spaced(face, px, 0.0, chars)
    }

    /// `shape_run` with letter-spacing: `tracking` px added to every
    /// glyph's advance, the last one included (the CSS rule; HALCYON-
    /// INSTRUMENT 8: the rails' uppercase runs at 0.08 em). Accumulated in
    /// the sub-pixel pen like the advances themselves, so a tracked run's
    /// width is the pen's, not a per-glyph rounding.
    pub fn shape_run_spaced(
        &mut self,
        face: u8,
        px: f32,
        tracking: f32,
        chars: impl Iterator<Item = char>,
    ) -> (Vec<GlyphRef>, i32) {
        let track_fx = (tracking * Self::PEN_SCALE as f32) as i32;
        let mut refs: Vec<GlyphRef> = Vec::new();
        let (mut rem, mut width) = (0i32, 0i32);
        let mut prev: Option<char> = None;
        for ch in chars {
            let Some(aq) = self.advance_fx(face, px, ch) else {
                continue;
            };
            let aq = aq + track_fx;
            if let Some(p) = prev {
                if !is_mono_face(face) {
                    // The kern belongs to the PRECEDING glyph's step, which
                    // is already pushed: fold it into the pen's carry, so
                    // its whole part re-lands on that ref's advance (and
                    // the running width) and its fraction rides into this
                    // glyph's phase -- the pen and the returned width stay
                    // the same number, at 1/256 px.
                    let k = self.kern(face, px, p, ch);
                    if k != 0 {
                        let carried = rem + k;
                        let whole = carried.div_euclid(Self::PEN_SCALE);
                        if let Some(last) = refs.last_mut() {
                            last.advance += whole;
                        }
                        width += whole;
                        rem = carried.rem_euclid(Self::PEN_SCALE);
                    }
                }
            }
            let total = rem + aq;
            let step = total.div_euclid(Self::PEN_SCALE);
            if let Some(g) = self.glyph_at(face, px, ch, phase_of(rem)) {
                refs.push(GlyphRef { glyph: g.glyph, advance: step });
                width += step;
                rem = total.rem_euclid(Self::PEN_SCALE);
                // The pair the NEXT glyph kerns with is this one, the last
                // SERVED glyph -- a refused glyph advanced nothing and is
                // nobody's left neighbour.
                prev = Some(ch);
            }
        }
        (refs, width)
    }

    pub fn advance_f(&mut self, face: u8, px: f32, ch: char) -> Option<f32> {
        if is_cell_face(face) {
            return Some(self.mono_cell_at(px).w as f32);
        }
        if face == FACE_MONO_TEXT {
            // Free-running Cornucopia: the table's advance at `px` (half
            // the em for this font), fractional, like any face. A
            // codepoint the subset lacks is served by the Instrument Sans
            // at the same px with ITS advance -- the mirror of the rule
            // below that serves the turnstile from the cell: the symbol
            // rather than a row of .notdef boxes. `glyph_at` takes the
            // same branch, so the measure and the paint agree.
            let served = self.mono_has(ch) || ch == '\u{FFFD}';
            if !served {
                return self.advance_f(FACE_SANS, px, ch);
            }
            let m = self.mono.as_ref()?;
            return Some(m.advance(m.glyph_id(ch), px));
        }
        let f = prop_of(&self.faces, face)?;
        // One charmap query, not two (TY-6 F9). This is the MEASUREMENT
        // path -- every laid glyph and every pre-measure comes through it,
        // uncached since TY-3b -- so the doubled lookup was paid per glyph
        // per lay, measured at 2.4x the cached integer path.
        let gid = f.glyph_id(ch);
        if gid.to_u32() == 0 && ch != '\u{FFFD}' && self.mono_has(ch) {
            // A codepoint the proportional face lacks, served by Cornucopia:
            // the legacy cuts take the island CELL (byte-pinned); the
            // Instrument cuts take the free-running face AT THE SAME PX
            // (I-5b: the prompt's turnstile at the body size, 7.4), the
            // mirror of `FACE_MONO_TEXT`'s Sans fallback -- terminating,
            // because that arm is taken only when the mono face HAS the
            // codepoint and this one only when it does.
            if is_instrument_sans(face) {
                return self.advance_f(FACE_MONO_TEXT, px, ch);
            }
            return Some(self.island.w as f32);
        }
        Some(f.advance(gid, px))
    }

    /// Whether the system mono face carries `ch` -- the test the
    /// proportional fallback and the mono cell both key on.
    #[inline]
    fn mono_has(&self, ch: char) -> bool {
        self.mono.as_ref().is_some_and(|f| f.has(ch))
    }

    /// The glyph for `ch` at `px` in `face`, rasterizing on first use.
    /// None: unknown face, or the bitmap can never fit a page. A missing
    /// codepoint is NOT None -- the face's .notdef box is drawn, which is
    /// the correct visible outcome for unmapped input.
    ///
    /// A CELL face's `px` selects the cell (island or grid) and serves the
    /// Cornucopia cell -- the Regular's or the Italic's, one grid; a
    /// box-drawing / block-element codepoint (U+2500-259F) is drawn
    /// PROCEDURALLY on the cell so joins are pixel-exact across cells,
    /// and that path is consulted BEFORE the face (the subset carries the
    /// block since I-5, for the free-running path only); any other
    /// codepoint the subset lacks falls back to the body face (Plex Text)
    /// rasterized to the cell height with the advance FORCED to the cell
    /// width (the grid survives; the glyph may clip). `FACE_MONO_TEXT`
    /// rasterizes Cornucopia free at `px`, phased, and serves a codepoint
    /// the subset lacks from the Instrument Sans.
    pub fn glyph(&mut self, face: u8, px: f32, ch: char) -> Option<GlyphRef> {
        self.glyph_at(face, px, ch, 0)
    }

    /// `glyph` at a horizontal PHASE (0..=3 quarter-pixels; taken modulo 4
    /// -- HALCYON-TYPE 4.3). The phase is part of the cache key because a
    /// phased raster is a different bitmap, and it rides the OUTLINE, so
    /// the returned bearing already carries the sub-pixel placement and the
    /// caller still blits at a whole pixel. FACE_MONO ignores it: a fixed
    /// cell has no phase, and phasing it would blur the grid the box glyphs
    /// join across.
    pub fn glyph_at(&mut self, face: u8, px: f32, ch: char, phase: u8) -> Option<GlyphRef> {
        let cell_face = is_cell_face(face);
        let phase = if cell_face { 0 } else { phase & 3 };
        let q = if cell_face {
            self.mono_is_grid(px) as u32
        } else {
            size_q(px)
        };
        let key = (face, q, phase, ch);
        if let Some(c) = self.cache.get(&key) {
            return Some(GlyphRef {
                glyph: c.id,
                advance: c.advance,
            });
        }
        if cell_face {
            let cell = self.mono_cell_at(px);
            let (cw, chh, base) = (cell.w, cell.h, cell.baseline);
            let smooth = self.smooth_mem;
            // The procedural box glyphs FIRST. The cells tier's contract
            // is a line that continues pixel-exactly into its neighbours,
            // which a font's box glyph -- bound to ITS line box -- never
            // does; the bake omits the block for that reason. The SUBSET
            // carries U+2500-257F since I-5 for the free-running path, so
            // the order here is load-bearing: the face is asked only after
            // the procedural path declines. The light stroke is the
            // hairline at this scale (COMPOSITION 1: a flat structural
            // line scales `max(1, round(s))`, and a box line joining cells
            // is one) -- 1 px up to 125%, 2 from 150.
            let light = libhalcyon::scale::ipx(1, self.scale).max(1) as usize;
            if let Some(alpha) = boxglyph::alpha(cw as usize, chh as usize, ch, light) {
                let id = self.packer.insert(cw as u32, chh as u32, &alpha, 0, base)?;
                self.cache.insert(key, Cached { id, advance: cw });
                return Some(GlyphRef {
                    glyph: id,
                    advance: cw,
                });
            }
            // Cornucopia's own outline (the Regular's or the Italic's),
            // rasterized into the cell -- and stroked, which is the whole
            // point of TY-4: before it, a baked cell was the one tier that
            // could not carry the theme's smoothing, so a proportional
            // glyph falling back into a cell was heavier than the
            // Cornucopia glyph beside it.
            let alpha = self
                .cell_face(face)
                .filter(|f| f.has(ch))
                .map(|f| mono_cell_alpha(f, f.glyph_id(ch), cell, smooth));
            if let Some(alpha) = alpha {
                let id = self.packer.insert(cw as u32, chh as u32, &alpha, 0, base)?;
                self.cache.insert(key, Cached { id, advance: cw });
                return Some(GlyphRef {
                    glyph: id,
                    advance: cw,
                });
            }
            // Fallback: body-rasterized at cell height, grid-advance --
            // for a codepoint neither Cornucopia nor the procedural box
            // path has. Since TY-4 it carries the same stroke as the cells
            // beside it rather than being the only stroked thing in the
            // grid.
            //
            // It goes through the SAME clipper as a Cornucopia cell. It
            // did not until TY-6 F1: it packed the proportional glyph's
            // own tight raster with its own bearing and then returned the
            // cell's advance, so a wide glyph overhung its neighbour --
            // measured at 80 of 121 Greek/Cyrillic entries at 200%, the
            // worst by 11 px, nearly a whole extra cell. The clip is the
            // contract this arm was quietly outside of.
            let f = prop_of(&self.faces, FACE_BODY)?;
            let fallback = MonoCell { em: (chh - 4) as f32, ..cell };
            let alpha = mono_cell_alpha(f, f.glyph_id(ch), fallback, smooth);
            let id = self.packer.insert(cw as u32, chh as u32, &alpha, 0, base)?;
            self.cache.insert(key, Cached { id, advance: cw });
            return Some(GlyphRef {
                glyph: id,
                advance: cw,
            });
        }
        if face == FACE_MONO_TEXT {
            // Cornucopia free-running: the outline at `px`, phased and
            // stroked like any proportional face, its own bearing. A
            // codepoint the subset lacks is the Instrument Sans's entry at
            // the same px (cached under ITS key; `advance_f` took the same
            // branch, so the laid step and the painted glyph agree).
            let served = self.mono_has(ch) || ch == '\u{FFFD}';
            if !served {
                return self.glyph_at(FACE_SANS, px, ch, phase);
            }
            let smooth = self.smooth_mem;
            let (r, advance) = {
                let m = self.mono.as_ref()?;
                let gid = m.glyph_id(ch);
                (m.raster(gid, px, smooth, phase), (m.advance(gid, px) + 0.5) as i32)
            };
            let id = self.packer.insert(r.w, r.h, &r.alpha, r.left, r.top)?;
            self.cache.insert(key, Cached { id, advance });
            return Some(GlyphRef { glyph: id, advance });
        }
        let f = prop_of(&self.faces, face)?;
        // A codepoint the proportional face lacks (IBM Plex Sans has no
        // U+22A2 -- ut's turnstile) is served from the island bake rather
        // than as Plex's .notdef box: Cornucopia carries the prompt glyph by
        // design, and a symbol at the island cell reads as the symbol, not
        // as tofu. Still None for a glyph neither has (the .notdef box then).
        // One charmap query for the whole path (TY-6 F9).
        let gid = f.glyph_id(ch);
        if gid.to_u32() == 0 && ch != '\u{FFFD}' && self.mono_has(ch) {
            if is_instrument_sans(face) {
                // The free-running mono's entry at this px (cached under ITS
                // key; `advance_f` took the same branch).
                return self.glyph_at(FACE_MONO_TEXT, px, ch, phase);
            }
            let cell = self.island;
            let smooth = self.smooth_mem;
            let alpha = self
                .mono
                .as_ref()
                .map(|m| mono_cell_alpha(m, m.glyph_id(ch), cell, smooth));
            if let Some(alpha) = alpha {
                let id = self
                    .packer
                    .insert(cell.w as u32, cell.h as u32, &alpha, 0, cell.baseline)?;
                self.cache.insert(key, Cached { id, advance: cell.w });
                return Some(GlyphRef {
                    glyph: id,
                    advance: cell.w,
                });
            }
        }
        let r = f.raster(gid, px, self.smooth_mem, phase);
        let id = self.packer.insert(r.w, r.h, &r.alpha, r.left, r.top)?;
        let advance = (f.advance(gid, px) + 0.5) as i32;
        self.cache.insert(key, Cached { id, advance });
        Some(GlyphRef { glyph: id, advance })
    }

    /// Vertical metrics for a face at a size (integer px, y-down). A cell
    /// face's are the selected cell's; `FACE_MONO_TEXT`'s are Cornucopia's
    /// own tables at `px` (hhea: 889 / -170 / 38 per 1000 -- 9 / 2 / 0 at
    /// 10 px), read the way every proportional face's are.
    pub fn line_metrics(&self, face: u8, px: f32) -> Option<LineMetrics> {
        if is_cell_face(face) {
            let cell = self.mono_cell_at(px);
            return Some(LineMetrics {
                ascent: cell.baseline,
                descent: cell.h - cell.baseline,
                line_height: cell.h,
            });
        }
        let f = if face == FACE_MONO_TEXT {
            self.mono.as_ref()?
        } else {
            prop_of(&self.faces, face)?
        };
        let (a, d, g) = f.line_metrics(px);
        let ascent = (a + 0.5) as i32;
        let descent = (-d + 0.5) as i32; // the table's descent is negative
        let gap = (g + 0.5) as i32;
        Some(LineMetrics {
            ascent,
            descent,
            line_height: ascent + descent + gap,
        })
    }

    /// The kerning adjustment between two glyphs at `px`, in 1/256 px (the
    /// sub-pixel pen's unit), which the caller folds into the PRECEDING
    /// glyph's step. Zero unless kerning is on (`set_kerning`: the
    /// Instrument profile; legacy stays byte-identical at 0) and never for
    /// a mono face (the cell is a grid, and Cornucopia carries no pair
    /// table anyway). The value is the face's GPOS `kern` feature
    /// (`outline::Face::kern_units`: HarfBuzz's first match per lookup,
    /// summed across lookups), memoized in font units per (face, pair) and
    /// scaled here -- the same expression the advances take, rounded once
    /// to the pen's unit.
    pub fn kern(&mut self, face: u8, px: f32, left: char, right: char) -> i32 {
        if !self.kerning || is_mono_face(face) {
            return 0;
        }
        let units = self.kern_units(face, left, right);
        if units == 0 {
            return 0;
        }
        let upem = prop_of(&self.faces, face).map(|f| f.upem()).unwrap_or(1000);
        round_half_away(units as f32 * px / upem as f32 * Self::PEN_SCALE as f32)
    }

    /// The pair adjustment in FONT UNITS (see `kern`), regardless of the
    /// switch: what the face's tables say. 0 for a mono face or a face
    /// that did not parse.
    pub fn kern_units(&mut self, face: u8, left: char, right: char) -> i32 {
        if is_mono_face(face) {
            return 0;
        }
        let key = (face, left, right);
        if let Some(&u) = self.kern_memo.get(&key) {
            return u;
        }
        let u = prop_of(&self.faces, face)
            .map(|f| f.kern_units(f.glyph_id(left), f.glyph_id(right)))
            .unwrap_or(0);
        if self.kern_memo.len() >= Self::KERN_MEMO_MAX {
            self.kern_memo.clear();
        }
        self.kern_memo.insert(key, u);
        u
    }

    /// Evict everything: pages, glyph table, cache -- and bump the store
    /// generation. The author's stylesheet/size-change point.
    pub fn regen(&mut self) {
        self.packer.regen();
        self.cache.clear();
    }

    /// The growth bound, applied BETWEEN frames: when the store holds the
    /// eviction bound (`evict_pages`: the display's `atlas_pages_for`, the
    /// `MAX_ATLAS_PAGES` floor before a display is known) or more, evict
    /// everything (`regen`) so the next frame re-packs only its working
    /// set. Within a frame `glyph()` only ever inserts, so a frame's
    /// `gen()` stamp stays valid across it (tile::paint_grid reads it
    /// once). The frame itself is bounded by the packer's hard cap
    /// (`ATLAS_PAGE_SLACK`), and its working set is what it PAINTS: layout
    /// measures through `advance`, which packs nothing, and a laid block
    /// holds codepoints + advances, not glyph ids, so an eviction
    /// invalidates no layout -- the next paint simply re-resolves the
    /// visible glyphs. Returns true when it evicted.
    pub fn evict_if_full(&mut self) -> bool {
        if self.packer.store.pages.len() >= self.evict_pages {
            self.regen();
            return true;
        }
        false
    }

    /// The current atlas generation (what the author stamps into ops).
    pub fn gen(&self) -> u32 {
        self.packer.store.gen
    }
}

/// Procedural box-drawing + block-element glyphs (U+2500-259F), drawn on the
/// mono cell so a line continues pixel-exactly into its neighbours -- the
/// reason the Cornucopia bake omits them (a font's box glyphs are bound to
/// ITS line box, not the cell). Light arms are one pixel, heavy arms a
/// centred band, double arms the OUTLINE of a wider band (the outline of a
/// union of bands gives every corner and junction its inner and outer
/// contours for free, which per-arm drawing never does); a single arm never
/// inks the interior of a double band, so it butts against the near line as
/// the reference glyphs do. Diagonals and arcs are not drawn (arcs become
/// square corners; diagonals fall back to the body face).
pub mod boxglyph {
    use alloc::vec::Vec;

    const NONE: u8 = 0;
    const LIGHT: u8 = 1;
    const HEAVY: u8 = 2;
    const DOUBLE: u8 = 3;

    /// (up, down, left, right) arm weights for U+2500..U+257F, in codepoint
    /// order. Dashed variants draw solid; arcs draw as their square corner;
    /// the three diagonals (U+2571-2573) have no arms and are not drawn.
    #[rustfmt::skip]
    const ARMS: [(u8, u8, u8, u8); 128] = [
        (0,0,1,1),(0,0,2,2),(1,1,0,0),(2,2,0,0),(0,0,1,1),(0,0,2,2),(1,1,0,0),(2,2,0,0), // 2500
        (0,0,1,1),(0,0,2,2),(1,1,0,0),(2,2,0,0),(0,1,0,1),(0,1,0,2),(0,2,0,1),(0,2,0,2), // 2508
        (0,1,1,0),(0,1,2,0),(0,2,1,0),(0,2,2,0),(1,0,0,1),(1,0,0,2),(2,0,0,1),(2,0,0,2), // 2510
        (1,0,1,0),(1,0,2,0),(2,0,1,0),(2,0,2,0),(1,1,0,1),(1,1,0,2),(2,1,0,1),(1,2,0,1), // 2518
        (2,2,0,1),(2,1,0,2),(1,2,0,2),(2,2,0,2),(1,1,1,0),(1,1,2,0),(2,1,1,0),(1,2,1,0), // 2520
        (2,2,1,0),(2,1,2,0),(1,2,2,0),(2,2,2,0),(0,1,1,1),(0,1,2,1),(0,1,1,2),(0,1,2,2), // 2528
        (0,2,1,1),(0,2,2,1),(0,2,1,2),(0,2,2,2),(1,0,1,1),(1,0,2,1),(1,0,1,2),(1,0,2,2), // 2530
        (2,0,1,1),(2,0,2,1),(2,0,1,2),(2,0,2,2),(1,1,1,1),(1,1,2,1),(1,1,1,2),(1,1,2,2), // 2538
        (2,1,1,1),(1,2,1,1),(2,2,1,1),(2,1,2,1),(2,1,1,2),(1,2,2,1),(1,2,1,2),(2,1,2,2), // 2540
        (1,2,2,2),(2,2,2,1),(2,2,1,2),(2,2,2,2),(0,0,1,1),(0,0,2,2),(1,1,0,0),(2,2,0,0), // 2548
        (0,0,3,3),(3,3,0,0),(0,1,0,3),(0,3,0,1),(0,3,0,3),(0,1,3,0),(0,3,1,0),(0,3,3,0), // 2550
        (1,0,0,3),(3,0,0,1),(3,0,0,3),(1,0,3,0),(3,0,1,0),(3,0,3,0),(1,1,0,3),(3,3,0,1), // 2558
        (3,3,0,3),(1,1,3,0),(3,3,1,0),(3,3,3,0),(0,1,3,3),(0,3,1,1),(0,3,3,3),(1,0,3,3), // 2560
        (3,0,1,1),(3,0,3,3),(1,1,3,3),(3,3,1,1),(3,3,3,3),(0,1,0,1),(0,1,1,0),(1,0,1,0), // 2568
        (1,0,0,1),(0,0,0,0),(0,0,0,0),(0,0,0,0),(0,0,1,0),(1,0,0,0),(0,0,0,1),(0,1,0,0), // 2570
        (0,0,2,0),(2,0,0,0),(0,0,0,2),(0,2,0,0),(0,0,1,2),(1,2,0,0),(0,0,2,1),(2,1,0,0), // 2578
    ];

    /// The cell's alpha (row-major, cw*ch bytes) for a box/block codepoint,
    /// None for anything else (or an undrawable member: the diagonals).
    /// `light` is the light stroke's width in px (the hairline at the
    /// display scale; 1 at 100%).
    pub fn alpha(cw: usize, ch: usize, c: char, light: usize) -> Option<Vec<u8>> {
        let cp = c as u32;
        if cw < 2 || ch < 2 {
            return None;
        }
        if (0x2500..=0x257F).contains(&cp) {
            let (u, d, l, r) = ARMS[(cp - 0x2500) as usize];
            if u == NONE && d == NONE && l == NONE && r == NONE {
                return None;
            }
            return Some(arms(cw, ch, [u, d, l, r], light.max(1)));
        }
        if (0x2580..=0x259F).contains(&cp) {
            return Some(block(cw, ch, cp));
        }
        None
    }

    fn arms(cw: usize, ch: usize, w: [u8; 4], light: usize) -> Vec<u8> {
        let mut px = alloc::vec![0u8; cw * ch];
        let cx = cw / 2;
        let cy = ch / 2;
        // Stroke geometry per cell size: the heavy band and the double gap
        // scale with the cell so a 6-px island and a 10-px grid both read;
        // the heavy band always outweighs the light stroke.
        let heavy = (if cw >= 9 { 3 } else { 2 }).max(light + 1);
        let g = if cw >= 9 { 2 } else { 1 };
        let [u, d, l, r] = w;

        // The double band: the union of every double arm's (2g+1)-wide band,
        // plus the centre square when two or more double arms meet (a lone
        // double arm ends at the centre line, where a single arm butts it).
        let ndouble = w.iter().filter(|&&a| a == DOUBLE).count();
        let mut band = alloc::vec![false; cw * ch];
        let mut any_band = false;
        let set_band = |x0: usize, x1: usize, y0: usize, y1: usize, band: &mut Vec<bool>| {
            for y in y0..=y1.min(ch - 1) {
                for x in x0..=x1.min(cw - 1) {
                    band[y * cw + x] = true;
                }
            }
        };
        let (bx0, bx1) = (cx.saturating_sub(g), (cx + g).min(cw - 1));
        let (by0, by1) = (cy.saturating_sub(g), (cy + g).min(ch - 1));
        if u == DOUBLE {
            set_band(bx0, bx1, 0, cy, &mut band);
            any_band = true;
        }
        if d == DOUBLE {
            set_band(bx0, bx1, cy, ch - 1, &mut band);
            any_band = true;
        }
        if l == DOUBLE {
            set_band(0, cx, by0, by1, &mut band);
            any_band = true;
        }
        if r == DOUBLE {
            set_band(cx, cw - 1, by0, by1, &mut band);
            any_band = true;
        }
        if ndouble >= 2 {
            set_band(bx0, bx1, by0, by1, &mut band);
        }
        // The outline: a band pixel with an in-cell 4-neighbour outside the
        // band. A cell-edge pixel in the arm's own direction has no such
        // neighbour, so the lines run off the cell and join the next one.
        let mut interior = alloc::vec![false; cw * ch];
        if any_band {
            for y in 0..ch {
                for x in 0..cw {
                    if !band[y * cw + x] {
                        continue;
                    }
                    let mut edge = false;
                    if x > 0 && !band[y * cw + x - 1] {
                        edge = true;
                    }
                    if x + 1 < cw && !band[y * cw + x + 1] {
                        edge = true;
                    }
                    if y > 0 && !band[(y - 1) * cw + x] {
                        edge = true;
                    }
                    if y + 1 < ch && !band[(y + 1) * cw + x] {
                        edge = true;
                    }
                    if edge {
                        px[y * cw + x] = 255;
                    } else {
                        interior[y * cw + x] = true;
                    }
                }
            }
        }
        // Light / heavy arms: a centred stroke from the centre to the edge,
        // never inking a double band's interior. A light stroke sits ON the
        // centre line; a heavy one is the band [c - (t-1)/2, c + t/2].
        let stroke = |t: usize, c: usize, n: usize| -> (usize, usize) {
            let lo = c.saturating_sub((t - 1) / 2);
            let hi = (c + t / 2).min(n - 1);
            (lo, hi)
        };
        let ink = |x0: usize, x1: usize, y0: usize, y1: usize, px: &mut Vec<u8>| {
            for y in y0..=y1.min(ch - 1) {
                for x in x0..=x1.min(cw - 1) {
                    if !interior[y * cw + x] {
                        px[y * cw + x] = 255;
                    }
                }
            }
        };
        let width = |a: u8| if a == HEAVY { heavy } else { light };
        if u == LIGHT || u == HEAVY {
            let (x0, x1) = stroke(width(u), cx, cw);
            ink(x0, x1, 0, cy, &mut px);
        }
        if d == LIGHT || d == HEAVY {
            let (x0, x1) = stroke(width(d), cx, cw);
            ink(x0, x1, cy, ch - 1, &mut px);
        }
        if l == LIGHT || l == HEAVY {
            let (y0, y1) = stroke(width(l), cy, ch);
            ink(0, cx, y0, y1, &mut px);
        }
        if r == LIGHT || r == HEAVY {
            let (y0, y1) = stroke(width(r), cy, ch);
            ink(cx, cw - 1, y0, y1, &mut px);
        }
        // A junction of a heavy arm with light ones: the centre square takes
        // the widest weight so the heavy stroke reads continuous through it.
        let tmax = w.iter().filter(|&&a| a == LIGHT || a == HEAVY).map(|&a| width(a)).max();
        if let Some(t) = tmax {
            if t > 1 && w.iter().filter(|&&a| a == LIGHT || a == HEAVY).count() >= 2 {
                let (x0, x1) = stroke(t, cx, cw);
                let (y0, y1) = stroke(t, cy, ch);
                ink(x0, x1, y0, y1, &mut px);
            }
        }
        px
    }

    fn block(cw: usize, ch: usize, cp: u32) -> Vec<u8> {
        let mut px = alloc::vec![0u8; cw * ch];
        let mut fill = |x0: usize, x1: usize, y0: usize, y1: usize, a: u8| {
            for y in y0..y1.min(ch) {
                for x in x0..x1.min(cw) {
                    px[y * cw + x] = a;
                }
            }
        };
        let eighth_h = |n: usize| (ch * n + 4) / 8; // rows for n/8 of the height
        let eighth_w = |n: usize| (cw * n + 4) / 8;
        let (hx, hy) = (cw / 2, ch / 2);
        match cp {
            0x2580 => fill(0, cw, 0, hy, 255),                         // upper half
            0x2581..=0x2587 => {
                let n = (cp - 0x2580) as usize;                        // lower n/8
                fill(0, cw, ch - eighth_h(n), ch, 255)
            }
            0x2588 => fill(0, cw, 0, ch, 255),                         // full
            0x2589..=0x258F => {
                let n = (0x2590 - cp) as usize;                        // left n/8
                fill(0, eighth_w(n), 0, ch, 255)
            }
            0x2590 => fill(hx, cw, 0, ch, 255),                        // right half
            0x2591 => fill(0, cw, 0, ch, 64),                          // light shade
            0x2592 => fill(0, cw, 0, ch, 128),                         // medium shade
            0x2593 => fill(0, cw, 0, ch, 192),                         // dark shade
            0x2594 => fill(0, cw, 0, eighth_h(1), 255),                // upper 1/8
            0x2595 => fill(cw - eighth_w(1), cw, 0, ch, 255),          // right 1/8
            0x2596..=0x259F => {
                // Quadrants: bit 0 = lower-left, 1 = lower-right, 2 = upper-left,
                // 3 = upper-right, per the block's own ordering.
                let q: u8 = match cp {
                    0x2596 => 0b0001,
                    0x2597 => 0b0010,
                    0x2598 => 0b0100,
                    0x2599 => 0b0111,
                    0x259A => 0b0110,
                    0x259B => 0b1101,
                    0x259C => 0b1110,
                    0x259D => 0b1000,
                    0x259E => 0b1001,
                    _ => 0b1011, // 259F: upper-right + lower-left + lower-right
                };
                if q & 0b0001 != 0 {
                    fill(0, hx, hy, ch, 255);
                }
                if q & 0b0010 != 0 {
                    fill(hx, cw, hy, ch, 255);
                }
                if q & 0b0100 != 0 {
                    fill(0, hx, 0, hy, 255);
                }
                if q & 0b1000 != 0 {
                    fill(hx, cw, 0, hy, 255);
                }
            }
            _ => {}
        }
        px
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vendored_faces_parse() {
        let gs = GlyphSource::new_vendored(512);
        assert_eq!(gs.face_count(), 7, "all seven vendored IBM Plex Sans cuts parse (Text, Bold, Text-Italic, Regular-Italic; Regular, Medium, SemiBold)");
        let lm = gs.line_metrics(FACE_BODY, 16.0).unwrap();
        assert!(
            lm.ascent > 8 && lm.ascent < 24,
            "16px ascent sane: {}",
            lm.ascent
        );
        assert!(
            lm.descent > 0 && lm.descent < 12,
            "descent sane: {}",
            lm.descent
        );
        assert!(lm.line_height >= lm.ascent + lm.descent);
    }

    #[test]
    fn rasterize_covers_and_caches() {
        let mut gs = GlyphSource::new_vendored(512);
        let a1 = gs.glyph(FACE_BODY, 16.0, 'A').unwrap();
        let a2 = gs.glyph(FACE_BODY, 16.0, 'A').unwrap();
        assert_eq!(a1.glyph, a2.glyph, "second lookup hits the cache");
        assert!(
            a1.advance > 3 && a1.advance < 20,
            "16px 'A' advance sane: {}",
            a1.advance
        );
        // The bitmap actually covers pixels.
        let ge = gs.packer.store.glyphs[a1.glyph as usize];
        assert!(ge.w > 0 && ge.h > 0);
        let page = &gs.packer.store.pages[ge.page as usize];
        let mut on = 0usize;
        for row in 0..ge.h {
            for col in 0..ge.w {
                if page.alpha[((ge.y + row) * page.w + ge.x + col) as usize] > 0 {
                    on += 1;
                }
            }
        }
        assert!(
            on > (ge.w * ge.h / 8) as usize,
            "the 'A' covers: {}/{}",
            on,
            ge.w * ge.h
        );
        // A different size is a different glyph.
        let a3 = gs.glyph(FACE_BODY, 24.0, 'A').unwrap();
        assert_ne!(a1.glyph, a3.glyph);
        // Bold is a distinct face with its own id space entry.
        let ab = gs.glyph(FACE_BODY_BOLD, 16.0, 'A').unwrap();
        assert_ne!(a1.glyph, ab.glyph);
    }

    #[test]
    fn space_has_advance_without_coverage() {
        let mut gs = GlyphSource::new_vendored(512);
        let sp = gs.glyph(FACE_BODY, 16.0, ' ').unwrap();
        assert!(sp.advance > 0, "space advances the pen");
        let ge = gs.packer.store.glyphs[sp.glyph as usize];
        assert_eq!((ge.w, ge.h), (0, 0), "a zero-area entry, as before the swap");
    }

    // The swap witness (HALCYON-TYPE TY-1): the outline path reports the
    // metrics fontdue reported -- the SAME table choice and the SAME
    // px/upem scale -- so no line height, advance or bearing moved when
    // the rasterizer did. The literals are fontdue 0.9.4's own output on
    // these faces (captured before its removal); a drift here is a layout
    // change wearing a rasterizer swap.
    const SAMPLE: &str = "The quick brown fox 0123456789 ~/kernel/sched Halcyon Wy.,;";
    const LINE_METRICS: [(f32, i32, i32, i32); 11] = [
        (10.0, 10, 3, 13),
        (11.5, 12, 3, 15),
        (12.0, 12, 3, 15),
        (14.0, 14, 4, 18),
        (16.0, 16, 4, 20),
        (17.5, 18, 5, 23),
        (20.0, 21, 6, 27),
        (24.0, 25, 7, 32),
        (28.0, 29, 8, 37),
        (35.0, 36, 10, 46),
        (70.0, 72, 19, 91),
    ];
    // (face, px, the advances of SAMPLE)
    const ADVANCES: [(u8, f32, &str); 12] = [
        (FACE_BODY, 11.5, "7,7,6,3,7,7,3,6,6,3,7,4,6,9,7,3,4,6,6,3,7,7,7,7,7,7,7,7,7,7,3,7,5,6,6,4,7,6,3,5,6,6,7,6,7,3,8,6,3,6,6,6,7,3,10,6,3,3,3"),
        (FACE_BODY, 17.5, "10,10,10,4,10,10,4,9,9,4,10,7,10,14,10,4,6,10,9,4,11,11,11,11,11,11,11,11,11,11,4,11,7,9,10,7,10,10,5,7,9,9,10,10,10,4,12,9,5,9,9,10,10,4,16,9,5,5,5"),
        (FACE_BODY, 35.0, "20,20,19,8,20,20,9,18,19,8,20,13,20,27,20,8,12,20,18,8,21,21,21,21,21,21,21,21,21,21,8,21,14,19,19,13,20,19,10,14,17,18,20,19,20,8,25,19,10,18,18,20,20,8,32,18,10,10,10"),
        (FACE_BODY_BOLD, 11.5, "7,7,6,3,7,7,3,6,7,3,7,5,6,10,7,3,4,6,6,3,7,7,7,7,7,7,7,7,7,7,3,7,5,7,6,5,7,6,3,5,6,6,7,6,7,3,8,7,3,6,6,6,7,3,11,6,4,4,4"),
        (FACE_BODY_BOLD, 17.5, "10,10,10,4,11,10,5,9,10,4,11,7,10,15,10,4,6,10,10,4,11,11,11,11,11,11,11,11,11,11,4,11,8,10,10,7,10,10,5,8,9,9,10,10,11,4,13,10,5,9,9,10,10,4,17,9,5,5,6"),
        (FACE_BODY_BOLD, 35.0, "20,21,20,8,21,21,10,18,20,8,21,14,20,29,21,8,13,20,20,8,21,21,21,21,21,21,21,21,21,21,8,21,16,20,20,14,21,20,11,16,18,18,21,20,21,8,25,20,11,18,19,20,21,8,34,19,11,11,12"),
        (FACE_BODY_ITALIC, 11.5, "6,7,6,3,7,7,3,6,6,3,7,4,6,9,7,3,4,6,6,3,7,7,7,7,7,7,7,7,7,7,3,7,5,6,6,4,7,6,3,5,5,6,7,6,7,3,8,7,3,6,6,6,7,3,10,6,3,3,3"),
        (FACE_BODY_ITALIC, 17.5, "10,10,9,4,10,10,5,9,9,4,10,6,10,13,10,4,6,10,9,4,11,11,11,11,11,11,11,11,11,11,4,11,7,9,9,6,10,9,5,7,8,9,10,9,10,4,12,10,5,9,9,10,10,4,15,9,5,5,5"),
        (FACE_BODY_ITALIC, 35.0, "20,20,18,8,20,20,9,17,18,8,20,13,19,26,20,8,11,19,18,8,21,21,21,21,21,21,21,21,21,21,8,21,14,18,18,13,20,18,9,14,17,17,20,18,20,8,24,20,9,17,17,19,20,8,31,17,10,10,11"),
        (FACE_HEADING_ITALIC, 11.5, "6,6,6,3,7,6,3,6,6,3,7,4,6,9,6,3,4,6,6,3,7,7,7,7,7,7,7,7,7,7,3,7,4,6,6,4,6,6,3,4,5,6,6,6,7,3,8,7,3,6,6,6,6,3,10,6,3,3,3"),
        (FACE_HEADING_ITALIC, 17.5, "10,10,9,4,10,10,5,9,9,4,10,6,10,13,10,4,5,10,9,4,11,11,11,11,11,11,11,11,11,11,4,11,7,9,9,6,10,9,5,7,8,9,10,9,10,4,12,10,5,9,8,10,10,4,15,8,5,5,5"),
        (FACE_HEADING_ITALIC, 35.0, "19,20,18,8,20,20,9,17,18,8,20,13,19,26,20,8,11,19,17,8,21,21,21,21,21,21,21,21,21,21,8,21,13,18,18,13,20,18,9,13,16,17,20,18,20,8,24,20,9,17,17,19,20,8,30,17,10,10,10"),
    ];
    // (face, px, char, left, top, w, h): fontdue's bearing + box.
    const BEARINGS: [(u8, f32, char, i32, i32, u32, u32); 18] = [
        (FACE_BODY, 16.0, 'A', 0, 12, 11, 12),
        (FACE_BODY, 16.0, 'g', 0, 10, 9, 14),
        (FACE_BODY, 16.0, 'H', 1, 12, 9, 12),
        (FACE_BODY, 16.0, 'y', 0, 9, 8, 13),
        (FACE_BODY, 16.0, '.', 1, 3, 3, 4),
        (FACE_BODY, 16.0, 'n', 1, 9, 7, 9),
        (FACE_BODY, 35.0, 'n', 2, 19, 16, 19),
        (FACE_BODY, 35.0, 'H', 3, 25, 19, 25),
        (FACE_BODY, 35.0, 'g', 1, 21, 18, 29),
        (FACE_BODY_BOLD, 16.0, 'A', 0, 12, 11, 12),
        (FACE_BODY_BOLD, 16.0, 'g', 0, 11, 9, 15),
        (FACE_BODY_BOLD, 35.0, 'g', 0, 22, 20, 30),
        (FACE_BODY_ITALIC, 16.0, 'A', -1, 12, 10, 12),
        (FACE_BODY_ITALIC, 16.0, 'y', -1, 9, 10, 13),
        (FACE_BODY_ITALIC, 35.0, 'H', 1, 25, 24, 25),
        (FACE_HEADING_ITALIC, 16.0, '.', 0, 2, 3, 3),
        (FACE_HEADING_ITALIC, 35.0, 'n', 1, 19, 17, 19),
        (FACE_HEADING_ITALIC, 35.0, 'g', -1, 21, 20, 29),
    ];

    #[test]
    fn line_metrics_are_the_fontdue_values() {
        let gs = GlyphSource::new_vendored(512);
        for face in [FACE_BODY, FACE_BODY_BOLD, FACE_BODY_ITALIC, FACE_HEADING_ITALIC] {
            for &(px, ascent, descent, line_height) in &LINE_METRICS {
                let lm = gs.line_metrics(face, px).unwrap();
                assert_eq!(
                    (lm.ascent, lm.descent, lm.line_height),
                    (ascent, descent, line_height),
                    "face {face} at {px} px"
                );
            }
        }
    }

    #[test]
    fn advances_are_the_fontdue_values() {
        let mut gs = GlyphSource::new_vendored(512);
        for &(face, px, want) in &ADVANCES {
            let got: Vec<alloc::string::String> = SAMPLE
                .chars()
                .map(|c| alloc::format!("{}", gs.advance(face, px, c).unwrap()))
                .collect();
            assert_eq!(got.join(","), want, "face {face} at {px} px");
        }
    }

    #[test]
    fn bearings_are_the_fontdue_values() {
        // The bearing is exact (both rasterizers floor the left edge and
        // ceil the top); the box may differ by the fractional edge's
        // rounding, never by more than a pixel.
        let mut gs = GlyphSource::new_vendored(512);
        for &(face, px, ch, left, top, w, h) in &BEARINGS {
            let g = gs.glyph(face, px, ch).unwrap();
            let ge = gs.packer.store.glyphs[g.glyph as usize];
            assert_eq!((ge.left, ge.top), (left, top), "face {face} {ch:?} at {px} px: bearing");
            assert!(
                ge.w.abs_diff(w) <= 1 && ge.h.abs_diff(h) <= 1,
                "face {face} {ch:?} at {px} px: box {}x{} vs fontdue {w}x{h}",
                ge.w,
                ge.h
            );
        }
    }

    // HALCYON-TYPE 4.3: the phase is part of the cache key (a phased
    // raster IS a different bitmap), it never moves an ADVANCE, and the
    // mono cells refuse it.
    #[test]
    fn the_phase_keys_the_cache_and_never_moves_an_advance() {
        let mut gs = GlyphSource::new_vendored(512);
        let px = 17.5;
        let ids: Vec<u32> = (0..4).map(|p| gs.glyph_at(FACE_BODY, px, 'n', p).unwrap().glyph).collect();
        for i in 0..4 {
            for j in (i + 1)..4 {
                assert_ne!(ids[i], ids[j], "phases {i}/{j} share a cache entry");
            }
        }
        // Re-asking hits the cache, not a fresh insert.
        for (p, &id) in ids.iter().enumerate() {
            assert_eq!(gs.glyph_at(FACE_BODY, px, 'n', p as u8).unwrap().glyph, id);
        }
        assert_eq!(gs.glyph(FACE_BODY, px, 'n').unwrap().glyph, ids[0], "glyph() IS phase 0");
        assert_eq!(gs.glyph_at(FACE_BODY, px, 'n', 6).unwrap().glyph, ids[2], "phase modulo 4");
        // The advance is the font's, whatever the phase.
        let want = gs.advance(FACE_BODY, px, 'n').unwrap();
        for p in 0..4 {
            assert_eq!(gs.glyph_at(FACE_BODY, px, 'n', p).unwrap().advance, want, "phase {p}");
        }
        // The mono cell has no phase: every phase is the same entry.
        let m0 = gs.glyph_at(FACE_MONO, MONO_ISLAND_PX, 'a', 0).unwrap().glyph;
        for p in 1..4 {
            assert_eq!(gs.glyph_at(FACE_MONO, MONO_ISLAND_PX, 'a', p).unwrap().glyph, m0, "mono phase {p}");
        }
    }

    // The fractional advance is what a sub-pixel pen accumulates, and it
    // must round to the integer advance the layout has always used --
    // otherwise measuring with one and painting with the other drifts a
    // box open. Checked over the sample, both mono tiers, the island
    // fallback and the .notdef box.
    #[test]
    fn the_fractional_advance_rounds_to_the_integer_one() {
        let mut gs = GlyphSource::new_vendored(512);
        let mut sample: Vec<(u8, f32, char)> = Vec::new();
        for ch in SAMPLE.chars() {
            for (f, px) in [(FACE_BODY, 11.5f32), (FACE_BODY_BOLD, 17.5), (FACE_HEADING_ITALIC, 35.0), (FACE_MONO, MONO_ISLAND_PX)] {
                sample.push((f, px, ch));
            }
        }
        sample.push((FACE_BODY, 10.0, '\u{22A2}')); // the island fallback
        sample.push((FACE_BODY, 11.5, '\u{4E00}')); // .notdef
        for (f, px, ch) in sample {
            let i = gs.advance(f, px, ch).expect("known face");
            let x = gs.advance_f(f, px, ch).expect("known face");
            assert_eq!((x + 0.5) as i32, i, "{f}/{px}/{ch:?}: {x} rounds to {i}");
            assert!(x >= 0.0, "{f}/{px}/{ch:?}: negative advance {x}");
        }
        assert_eq!(gs.advance_f(99, 11.5, 'a'), None, "unknown face");
        assert_eq!(gs.packer.store.pages.len(), 0, "measuring packs nothing");
    }

    // HALCYON-SCALE 7 under phases (HALCYON-TYPE 4.3's bound claim): the
    // store can never hold more entries than the frame PAINTS, because a
    // painted instance paints exactly ONE phase. The adversarial shape is
    // therefore not "more phases" but "every painted instance a distinct
    // (codepoint, phase) pair" -- which is the same count as today's
    // distinct-codepoint stream, and this pins that equality rather than
    // assuming it.
    #[test]
    fn the_atlas_bound_holds_under_phases() {
        let screen = |phased: bool| -> (usize, usize) {
            let mut gs = GlyphSource::new_vendored(512);
            gs.set_display(1280, 800);
            gs.set_scale(200);
            let sheet = crate::layout::daylight_sheet(200);
            let px = sheet.hdr_px[0];
            let lm = gs.line_metrics(FACE_BODY, px).unwrap();
            let rows = 800 / ((px * 1.25 + 0.5) as i32);
            let per_row = 1280 / (lm.ascent / 2).max(8);
            let mut cp = 0x4E00u32;
            let mut served = 0usize;
            let mut n = 0u8;
            for _ in 0..rows {
                for _ in 0..per_row {
                    // phased: every instance a distinct (cp, phase) pair.
                    let phase = if phased { n % 4 } else { 0 };
                    if gs.glyph_at(FACE_BODY, px, char::from_u32(cp).unwrap(), phase).is_some() {
                        served += 1;
                    }
                    n = n.wrapping_add(1);
                    cp += 1;
                }
            }
            assert_eq!(served, (rows * per_row) as usize, "every glyph of the screen served");
            assert!(!gs.evict_if_full(), "a screen of headings does not trip the eviction");
            (gs.packer.store.pages.len(), gs.evict_pages())
        };
        let (plain_pages, bound) = screen(false);
        let (phased_pages, _) = screen(true);
        assert!(plain_pages <= bound, "{plain_pages} pages vs bound {bound}");
        assert!(phased_pages <= bound, "phased {phased_pages} pages vs bound {bound}");
        assert_eq!(phased_pages, plain_pages, "one phase per painted instance: the same store");
        // And the repeat-heavy shape -- ONE codepoint at all four phases --
        // costs four entries, not four pages.
        let mut gs = GlyphSource::new_vendored(512);
        for p in 0..4 {
            gs.glyph_at(FACE_BODY, 17.5, 'n', p).unwrap();
        }
        assert_eq!(gs.packer.store.glyphs.len(), 4, "four phases, four entries");
        assert_eq!(gs.packer.store.pages.len(), 1, "on one page");
    }

    fn ink(gs: &GlyphSource, id: u32) -> u64 {
        let (_, _, a) = glyph_alpha(gs, id);
        a.iter().map(|&v| v as u64).sum()
    }

    // HALCYON-TYPE 4.2: the smoothing stroke is the theme's, it is a
    // property of the whole store (a change regens), it adds the measured
    // weight to every proportional raster, and it moves no metric: the
    // advance and the line metrics are the tables', not the raster's.
    #[test]
    fn the_smoothing_stroke_is_the_themes_and_regens_on_change() {
        assert_eq!(libhalcyon::theme::DAYLIGHT.smooth_mem, 12, "0.012 em on the light ground");
        assert_eq!(crate::layout::daylight_sheet(100).smooth_mem, 12, "the sheet carries it");
        let mut plain = GlyphSource::new_vendored(512);
        assert_eq!(plain.smooth(), 0, "a fresh source is the plain fill");
        let mut gs = GlyphSource::new_vendored(512);
        let n0 = gs.glyph(FACE_HEADING_ITALIC, 35.0, 'n').unwrap();
        assert!(gs.set_smooth(12));
        assert_eq!(gs.gen(), 1, "the amount changed: everything evicted");
        assert!(gs.packer.store.glyphs.is_empty());
        assert!(!gs.set_smooth(12), "the same amount is a no-op");
        assert_eq!(gs.gen(), 1);
        let n1 = gs.glyph(FACE_HEADING_ITALIC, 35.0, 'n').unwrap();
        let p = plain.glyph(FACE_HEADING_ITALIC, 35.0, 'n').unwrap();
        assert_eq!((n0.advance, n1.advance, p.advance), (20, 20, 20), "the advance is the table's");
        let (i0, i1) = (ink(&plain, p.glyph), ink(&gs, n1.glyph));
        // fontdue's fill of this glyph summed 30182: the two exact-area
        // rasterizers agree on the plain fill within a few percent.
        assert!(i0.abs_diff(30182) * 100 <= 30182 * 3, "plain fill ink {i0} vs fontdue 30182");
        // The stroke's weight: the lab measured the Mac's smoothing at +18%
        // on this glyph (HALCYON-TYPE 3.2), and 0.012 em lands there --
        // measured +18% INK (the sum of coverage) at the swap: 35652 over
        // 30146. The band is the lab's prediction (15-22%) minus a point.
        let pct = (i1 * 100) / i0;
        assert!((114..=122).contains(&pct), "stroked ink {i1} vs plain {i0}: +{}%", pct as i64 - 100);
        // One row taller, never wider, the bearing untouched: the stroke's
        // half-width at 35 px is 0.21 px, under the mask's rounding.
        let (gp, pp) = (gs.packer.store.glyphs[n1.glyph as usize], plain.packer.store.glyphs[p.glyph as usize]);
        assert_eq!((pp.w, pp.h, gp.w, gp.h), (17, 19, 17, 20));
        assert_eq!((pp.left, pp.top), (gp.left, gp.top));
        for px in [11.5f32, 17.5, 35.0] {
            assert_eq!(gs.line_metrics(FACE_BODY, px).unwrap().line_height, plain.line_metrics(FACE_BODY, px).unwrap().line_height);
            for c in SAMPLE.chars() {
                assert_eq!(gs.advance(FACE_BODY, px, c), plain.advance(FACE_BODY, px, c), "{c:?} at {px}");
            }
        }
        // The stroke reaches the mono fallback (a codepoint the bake lacks,
        // rasterized from the body face) the same way -- one outline path.
        let f0 = plain.glyph(FACE_MONO, MONO_ISLAND_PX, '\u{0424}').unwrap(); // CYRILLIC EF
        let f1 = gs.glyph(FACE_MONO, MONO_ISLAND_PX, '\u{0424}').unwrap();
        assert_eq!((f0.advance, f1.advance), (6, 6), "the cell advance either way");
        assert!(ink(&gs, f1.glyph) > ink(&plain, f0.glyph), "stroked in the cell too");
        // Back to 0: the plain fill again, byte for byte.
        assert!(gs.set_smooth(0));
        let n2 = gs.glyph(FACE_HEADING_ITALIC, 35.0, 'n').unwrap();
        assert_eq!(glyph_alpha(&gs, n2.glyph), glyph_alpha(&plain, p.glyph));
    }

    // I-5d: the GPOS shaper the pre-I-5d test named as "the test that
    // changes" -- a fresh source (the legacy profile) still answers 0 for
    // every pair, byte for byte; switched on (Instrument) it reads Plex's
    // `kern` feature in the pen's unit: A/V is -41 units of 1000 in the
    // Regular cut, -1.312 px at 32, -336 in 1/256 px rounded.
    #[test]
    fn kern_is_zero_until_switched_on_and_then_reads_plex_gpos_in_the_pens_unit() {
        let mut gs = GlyphSource::new_vendored(512);
        assert!(!gs.kerning());
        assert_eq!(gs.kern(FACE_SANS, 32.0, 'A', 'V'), 0, "off: A/V unkerned");
        assert_eq!(gs.kern(FACE_BODY, 32.0, 'A', 'V'), 0);
        assert!(gs.set_kerning(true));
        assert!(!gs.set_kerning(true), "a no-op says so");
        assert_eq!(gs.kern_units(FACE_SANS, 'A', 'V'), -41);
        assert_eq!(gs.kern(FACE_SANS, 32.0, 'A', 'V'), -336);
        assert_eq!(gs.kern(FACE_SANS, 15.0, 'A', 'V'), round_half_away(-41.0f32 * 15.0 / 1000.0 * 256.0));
        assert_eq!(round_half_away(-335.87), -336);
        assert_eq!(round_half_away(-0.4), 0);
        assert_eq!(round_half_away(2.5), 3);
        assert_eq!(gs.kern(FACE_SANS, 32.0, 'x', 'x'), 0, "no pair either way");
        assert_ne!(gs.kern(FACE_BODY, 32.0, 'A', 'V'), 0, "the Text cut kerns too");
        assert_ne!(gs.kern(FACE_HEADING_ITALIC, 32.0, 'A', 'V'), 0, "and the Italic");
        // Never a mono face: the cell is a grid, and Cornucopia carries no
        // pair table.
        for face in [FACE_MONO, FACE_MONO_ITALIC, FACE_MONO_TEXT] {
            assert_eq!(gs.kern(face, 32.0, 'A', 'V'), 0);
            assert_eq!(gs.kern_units(face, 'A', 'V'), 0);
        }
        // Units are size-free: a regen evicts rasters, not pairs.
        gs.regen();
        assert_eq!(gs.kern(FACE_SANS, 32.0, 'A', 'V'), -336);
        assert!(gs.set_kerning(false));
        assert_eq!(gs.kern(FACE_SANS, 32.0, 'A', 'V'), 0);
    }

    // The chrome's shaper folds each kern into the preceding glyph's step
    // through the pen's carry: the refs' steps still sum to the width, and
    // the width is the sub-pixel sum rounded once, not a per-glyph
    // rounding.
    #[test]
    fn a_kerned_run_folds_the_kern_into_the_preceding_step_at_the_pens_precision() {
        let mut gs = GlyphSource::new_vendored(512);
        let (off, w_off) = gs.shape_run(FACE_SANS, 32.0, "AVATAR".chars());
        gs.set_kerning(true);
        let (on, w_on) = gs.shape_run(FACE_SANS, 32.0, "AVATAR".chars());
        assert_eq!(on.len(), 6);
        assert_eq!(on.iter().map(|r| r.advance).sum::<i32>(), w_on, "the refs' steps are the width");
        assert_eq!(off.iter().map(|r| r.advance).sum::<i32>(), w_off);
        assert!(w_on < w_off, "kerned {w_on} vs unkerned {w_off}");
        let mut q = 0i32;
        let mut prev = None;
        for ch in "AVATAR".chars() {
            if let Some(p) = prev {
                q += gs.kern(FACE_SANS, 32.0, p, ch);
            }
            q += gs.advance_fx(FACE_SANS, 32.0, ch).unwrap();
            prev = Some(ch);
        }
        assert_eq!(w_on, q.div_euclid(GlyphSource::PEN_SCALE), "the pen's whole width of {q}");
    }

    #[test]
    fn regen_evicts_cache_and_bumps_gen() {
        let mut gs = GlyphSource::new_vendored(512);
        let a1 = gs.glyph(FACE_BODY, 16.0, 'A').unwrap();
        assert_eq!(gs.gen(), 0);
        gs.regen();
        assert_eq!(gs.gen(), 1);
        assert!(
            gs.packer.store.glyphs.is_empty(),
            "the table went with the pages"
        );
        let a2 = gs.glyph(FACE_BODY, 16.0, 'A').unwrap();
        assert_eq!(a2.glyph, 0, "fresh table restarts ids");
        let _ = a1;
    }

    #[test]
    fn atlas_growth_is_bounded_between_frames() {
        // Distinct codepoints Plex lacks each rasterize the .notdef box under
        // their OWN cache key, so a stream of them grows the store one glyph
        // per codepoint (the hostile-output shape). Tiny pages (32 px) make
        // the bound reachable in a test: WITHOUT the between-frames check the
        // store grows past MAX_ATLAS_PAGES (the positive control that growth
        // is real); WITH it, each "frame" starts under the bound and ends at
        // most one frame's glyphs past it.
        let mut gs = GlyphSource::new_vendored(32);
        let mut cp = 0x4E00u32; // CJK ideographs: not in Plex
        let mut next = |gs: &mut GlyphSource, n: usize| {
            for _ in 0..n {
                let ch = char::from_u32(cp).unwrap();
                cp += 1;
                let _ = gs.glyph(FACE_BODY, 11.5, ch);
            }
        };
        next(&mut gs, 400);
        let cap = MAX_ATLAS_PAGES + ATLAS_PAGE_SLACK;
        assert_eq!(
            gs.packer.store.pages.len(),
            cap,
            "the control: 400 glyphs on 32-px pages want more than the cap, and got exactly the cap"
        );
        // Past the cap an insert is REFUSED, not grown into: the frame's
        // bound (the eviction below cannot bound a frame -- the stream
        // decides how many distinct glyphs one frame paints).
        let refused = '\u{9000}'; // a CJK codepoint the stream never reaches
        assert!(gs.glyph(FACE_BODY, 11.5, refused).is_none(), "at the cap: refused");
        assert_eq!(gs.packer.store.pages.len(), cap, "a refusal opens nothing");
        assert!(gs.evict_if_full(), "over the bound: evicted");
        assert_eq!(gs.packer.store.pages.len(), 0);
        assert_eq!(gs.gen(), 1, "the generation bumped");
        assert!(!gs.evict_if_full(), "empty: nothing to evict");
        assert!(gs.glyph(FACE_BODY, 11.5, refused).is_some(), "the eviction reopened the store");
        // Frames of 20 glyphs each: the store never exceeds the bound by
        // more than one frame's growth (and never the hard cap), and the
        // gen keeps bumping.
        let per_frame = 20;
        let mut peak = 0;
        for _ in 0..200 {
            gs.evict_if_full();
            next(&mut gs, per_frame);
            peak = peak.max(gs.packer.store.pages.len());
        }
        assert!(peak <= MAX_ATLAS_PAGES + per_frame, "peak {peak} pages: bounded by the frame's growth");
        assert!(peak <= cap, "peak {peak} pages: never past the hard cap");
        assert!(gs.gen() >= 2, "evicted again along the way (gen {})", gs.gen());
        // A glyph looked up after an eviction is served fresh under the new
        // generation, not from the cleared cache.
        let a = gs.glyph(FACE_BODY, 11.5, 'a').expect("a");
        assert!((a.glyph as usize) < gs.packer.store.glyphs.len());
    }

    #[test]
    fn advance_measures_without_packing_and_agrees_with_glyph() {
        // Layout measures through `advance`: the atlas stays EMPTY however
        // much is measured (5000 distinct codepoints Plex lacks, the shape
        // that grew the store a page per few hundred glyphs), and for every
        // codepoint the advance is the one `glyph` later paints with --
        // the proportional face, the mono cells, the island fallback for a
        // codepoint Plex lacks (the turnstile), and the .notdef box.
        let mut gs = GlyphSource::new_vendored(512);
        let mut sample: Vec<(u8, f32, char)> = Vec::new();
        for ch in "The quick brown fox 0123456789 ~/kernel/sched".chars() {
            sample.push((FACE_BODY, 11.5, ch));
            sample.push((FACE_BODY_BOLD, 17.5, ch));
            sample.push((FACE_MONO, MONO_ISLAND_PX, ch));
            sample.push((FACE_MONO, MONO_GRID_PX, ch));
        }
        sample.push((FACE_BODY, 10.0, '\u{22A2}'));
        sample.push((FACE_MONO, MONO_ISLAND_PX, '\u{2500}'));
        for cp in 0x4E00u32..0x4E00 + 5000 {
            sample.push((FACE_BODY, 11.5, char::from_u32(cp).unwrap()));
        }
        let advances: Vec<i32> = sample
            .iter()
            .map(|&(f, px, ch)| gs.advance(f, px, ch).expect("a known face"))
            .collect();
        assert_eq!(gs.packer.store.pages.len(), 0, "measuring packed nothing");
        assert!(gs.packer.store.glyphs.is_empty());
        assert_eq!(gs.gen(), 0);
        for (i, &(f, px, ch)) in sample.iter().enumerate().take(400) {
            let g = gs.glyph(f, px, ch).expect("paints");
            assert_eq!(g.advance, advances[i], "{f}/{px}/{ch:?}: paint advance == measured advance");
            // And the cache hit path agrees too.
            assert_eq!(gs.advance(f, px, ch), Some(advances[i]));
        }
        assert!(gs.packer.store.pages.len() >= 1, "painting packs");
        assert_eq!(gs.advance(99, 11.5, 'a'), None, "an unknown face measures nothing");
    }

    #[test]
    fn one_mono_size_the_grid_cell_is_the_island_cell() {
        // One mono size (the operator, 2026-09-08): the grid (alt screen /
        // pts) runs at the preformatted block's cell. Two atlas slots, one
        // bake in both -- every request lands on the same geometry.
        let mut gs = GlyphSource::new_vendored(512);
        let (iw, ih, ib) = gs.island_cell();
        let (gw, gh, gb) = gs.mono_cell();
        assert_eq!((iw, ih, ib), (gw, gh, gb), "the island cell IS the grid cell");
        assert_eq!(iw, 6, "advance 6 at 100%");
        let a_island = gs.glyph(FACE_MONO, MONO_ISLAND_PX, 'a').unwrap();
        let a_grid = gs.glyph(FACE_MONO, MONO_GRID_PX, 'a').unwrap();
        assert_eq!((a_island.advance, a_grid.advance), (iw, gw));
        let a_low = gs.glyph(FACE_MONO, 10.5, 'a').unwrap();
        assert_eq!(a_low.advance, iw, "a below-em request lands on the same cell");
        let lm = gs.line_metrics(FACE_MONO, MONO_ISLAND_PX).unwrap();
        assert_eq!(lm.line_height, ih);
        let lg = gs.line_metrics(FACE_MONO, MONO_GRID_PX).unwrap();
        assert_eq!(lg.line_height, gh);
    }

    fn glyph_alpha(gs: &GlyphSource, id: u32) -> (u32, u32, Vec<u8>) {
        let ge = gs.packer.store.glyphs[id as usize];
        let page = &gs.packer.store.pages[ge.page as usize];
        let mut out = Vec::new();
        for row in 0..ge.h {
            for col in 0..ge.w {
                out.push(page.alpha[((ge.y + row) * page.w + ge.x + col) as usize]);
            }
        }
        (ge.w, ge.h, out)
    }

    #[test]
    fn box_drawing_is_procedural_on_both_cells() {
        // U+2500-259F are absent from the Cornucopia bake by design; the
        // renderer draws them on the cell. A horizontal line reaches BOTH
        // cell edges on its centre row (so neighbours join), a vertical line
        // both the top and bottom rows, a corner exactly one of each; a
        // heavy line is wider than a light one; a double line is two lines
        // with a gap; the full block covers the whole cell.
        let mut gs = GlyphSource::new_vendored(512);
        for px in [MONO_ISLAND_PX, MONO_GRID_PX] {
            let (cw, chh, _) = if px >= 16.0 { gs.mono_cell() } else { gs.island_cell() };
            let (cw, chh) = (cw as u32, chh as u32);
            let cy = chh / 2;
            let cx = cw / 2;
            let at = |a: &[u8], x: u32, y: u32| a[(y * cw + x) as usize];
            // ─ light horizontal
            let h = gs.glyph(FACE_MONO, px, '\u{2500}').unwrap();
            assert_eq!(h.advance, cw as i32, "a box glyph keeps the cell advance");
            let (w, hh, a) = glyph_alpha(&gs, h.glyph);
            assert_eq!((w, hh), (cw, chh), "drawn on the full cell");
            assert!(at(&a, 0, cy) == 255 && at(&a, cw - 1, cy) == 255, "reaches both edges");
            assert!(at(&a, cx, 0) == 0 && at(&a, cx, chh - 1) == 0, "no vertical ink");
            // │ light vertical
            let v = gs.glyph(FACE_MONO, px, '\u{2502}').unwrap();
            let (_, _, a) = glyph_alpha(&gs, v.glyph);
            assert!(at(&a, cx, 0) == 255 && at(&a, cx, chh - 1) == 255);
            assert!(at(&a, 0, cy) == 0 && at(&a, cw - 1, cy) == 0);
            // ┌ down-right corner: bottom edge + right edge only
            let c = gs.glyph(FACE_MONO, px, '\u{250C}').unwrap();
            let (_, _, a) = glyph_alpha(&gs, c.glyph);
            assert!(at(&a, cx, chh - 1) == 255 && at(&a, cw - 1, cy) == 255);
            assert!(at(&a, cx, 0) == 0 && at(&a, 0, cy) == 0);
            // ━ heavy is wider than ─ light (rows inked at the left edge)
            let hv = gs.glyph(FACE_MONO, px, '\u{2501}').unwrap();
            let (_, _, ah) = glyph_alpha(&gs, hv.glyph);
            let (_, _, al) = glyph_alpha(&gs, h.glyph);
            let rows_light = (0..chh).filter(|&y| at(&al, 0, y) == 255).count();
            let rows_heavy = (0..chh).filter(|&y| at(&ah, 0, y) == 255).count();
            assert!(rows_heavy > rows_light, "heavy {rows_heavy} > light {rows_light}");
            // ═ double: two inked rows at x=0 with an un-inked row between
            let d = gs.glyph(FACE_MONO, px, '\u{2550}').unwrap();
            let (_, _, ad) = glyph_alpha(&gs, d.glyph);
            let inked: Vec<u32> = (0..chh).filter(|&y| at(&ad, 0, y) == 255).collect();
            assert_eq!(inked.len(), 2, "double = two lines at {px}px: {inked:?}");
            assert!(inked[1] - inked[0] >= 2, "with a gap between them");
            // ╔ double corner: the outer lines reach the far edges, the inner
            // corner exists, and nothing is inked above/left of the outer corner.
            let dc = gs.glyph(FACE_MONO, px, '\u{2554}').unwrap();
            let (_, _, ac) = glyph_alpha(&gs, dc.glyph);
            assert_eq!(at(&ac, inked[0].min(cx), 0), 0, "nothing above the corner on the far side");
            assert!(at(&ac, cw - 1, inked[0]) == 255 && at(&ac, cw - 1, inked[1]) == 255, "both lines run right");
            // █ full block covers the cell
            let f = gs.glyph(FACE_MONO, px, '\u{2588}').unwrap();
            let (_, _, af) = glyph_alpha(&gs, f.glyph);
            assert!(af.iter().all(|&p| p == 255), "the full block covers every pixel");
            // ▄ lower half: top row empty, bottom row full
            let lh = gs.glyph(FACE_MONO, px, '\u{2584}').unwrap();
            let (_, _, al) = glyph_alpha(&gs, lh.glyph);
            assert!(at(&al, cx, 0) == 0 && at(&al, cx, chh - 1) == 255);
        }
    }

    // The end-to-end floor: rasterize a word and execute it through the
    // cartoon CPU executor -- the whole H-2c stack in one assertion.
    #[test]
    fn word_renders_through_the_executor() {
        let mut gs = GlyphSource::new_vendored(512);
        let lm = gs.line_metrics(FACE_BODY, 16.0).unwrap();
        // Shaped at the sub-pixel pen; the kern fold (off in a fresh
        // source) is shape_run's own.
        let (refs, _) = gs.shape_run(FACE_BODY, 16.0, "Halcyon".chars());
        let mut cart = cartoon::Cartoon::new();
        cart.ops.push(cartoon::Op::Clear { color: 0xFFF1_EAE0 }); // parchment ground
        cart.push_glyphs(gs.gen(), 4, 4 + lm.ascent, 0xFF2B_2320, &refs);
        let w = 96usize;
        let h = (lm.line_height + 8) as usize;
        let mut px = alloc::vec![0u32; w * h];
        cartoon::execute(
            &cart,
            &gs.packer.store,
            &cartoon::BlobStore::new(),
            &mut px,
            w,
            None,
        );
        let ink = px.iter().filter(|&&p| p != 0xFFF1_EAE0).count();
        assert!(ink > 60, "the word inked {} pixels", ink);
        // Nothing painted outside the first line box + margins.
        assert!(
            px[..w].iter().filter(|&&p| p != 0xFFF1_EAE0).count() < w / 2,
            "row 0 is mostly ground"
        );
    }

    // HALCYON-SCALE 6: the mono advances at the five values are the
    // operator's table (round half up of 6s), bounded below by the
    // legibility floor and above by MONO_ADVANCE_MAX.
    //
    // The values are LITERALS, not re-derived: TY-4 removed the
    // nearest-smaller-bake step (the outline serves any advance), so this
    // is where a claim that the removal moved nothing is falsifiable. It
    // moved nothing because every reachable percent already named a bake.
    #[test]
    fn mono_advances_are_the_scale_table() {
        // One mono size (2026-09-08): the grid advance IS the island's.
        assert_eq!(mono_advances(100), (6, 6));
        assert_eq!(mono_advances(125), (8, 8), "7.5 up");
        assert_eq!(mono_advances(150), (9, 9));
        assert_eq!(mono_advances(175), (11, 11), "10.5 up");
        assert_eq!(mono_advances(200), (12, 12));
        for p in [100u16, 125, 150, 175, 200] {
            let (i, g) = mono_advances(p);
            assert_eq!(i, g, "one mono size: the grid cell is the island cell");
        }
        // Off the table: 140% wants 8.4 -> 8.
        assert_eq!(mono_advances(140), (8, 8));
        // Below 100 (not a v1 value; the function is total): the floor.
        assert_eq!(mono_advances(50), (6, 6));
        assert_eq!(mono_advances(0), (6, 6));
        // Above the range (SCALE_MAX is 200, so unreachable): the ceiling,
        // which bounds the cell -- and therefore the raster -- rather than
        // letting a percent decide how big an atlas entry gets.
        assert_eq!(mono_advances(1000), (MONO_ADVANCE_MAX, MONO_ADVANCE_MAX));
        assert_eq!(mono_advances(u16::MAX), (MONO_ADVANCE_MAX, MONO_ADVANCE_MAX));
    }

    // TY-6 F1: the mono tier's THIRD arm -- a codepoint neither Cornucopia
    // nor the procedural box path has, rasterized from the body face -- has
    // to keep the cell contract too. It did not: it packed the
    // proportional glyph's own tight raster with its own bearing and then
    // returned the cell's advance, so the ink overhung into the next
    // cell's columns and was blended under that cell's glyph. Measured at
    // 80 of 121 Greek/Cyrillic entries at 200%, worst 11 px.
    #[test]
    fn the_mono_fallback_is_clipped_to_its_cell_like_every_other_arm() {
        for pct in [100u16, 200] {
            let mut gs = GlyphSource::new_vendored(512);
            gs.set_scale(pct);
            gs.set_smooth(12);
            let (cw, chh, base) = gs.mono_cell();
            let mut checked = 0;
            // Greek + Cyrillic: outside the 208-glyph subset, outside
            // U+2500-259F, so both earlier arms miss and this one fires.
            for ch in ['\u{0416}', '\u{0424}', '\u{0429}', '\u{03A9}', '\u{03BE}'] {
                let g = gs.glyph(FACE_MONO, MONO_GRID_PX, ch).expect("served");
                let e = gs.packer.store.glyphs[g.glyph as usize];
                assert_eq!(
                    (e.w as i32, e.h as i32),
                    (cw, chh),
                    "{pct}% {ch:?}: the entry must BE the cell, not the glyph's own box"
                );
                assert_eq!((e.left, e.top), (0, base), "{pct}% {ch:?}: the cell's bearing");
                assert_eq!(g.advance, cw, "{pct}% {ch:?}: the cell's advance");
                checked += 1;
            }
            assert_eq!(checked, 5, "the fallback arm actually fired");
        }
    }

    // TY-6 F2: the cache key must separate two sizes the stylesheet
    // actually asks for. The exit badge lays at `body_px * 0.9` and the
    // chrome/menu name at NAME_PX; under the old half-pixel quantum both
    // landed on one entry at 100% (10.35 and 10.5 -> 21) and at 125%
    // (12.9375 and 13.125 -> 26), so the raster each got was whichever
    // painted first -- and which that was flipped with whether a failed
    // command was on screen.
    #[test]
    fn two_sizes_the_stylesheet_asks_for_never_share_one_entry() {
        for pct in [100u16, 125, 150, 175, 200] {
            let mut gs = GlyphSource::new_vendored(512);
            gs.set_scale(pct);
            let sheet = crate::layout::daylight_sheet(pct);
            let badge = sheet.body_px * 0.9;
            let name = sheet.px(crate::chrome::NAME_PX);
            assert!(badge != name, "{pct}%: the two sizes differ in the first place");
            let a = gs.glyph(FACE_BODY, badge, 'W').unwrap();
            let b = gs.glyph(FACE_BODY, name, 'W').unwrap();
            assert_ne!(
                a.glyph, b.glyph,
                "{pct}%: {badge} and {name} must not share an atlas entry"
            );
        }
    }

    // TY-6 F3: `shape_run` folds the kern into the PRECEDING glyph's step,
    // so the width it returns is the sum of the advances it returns. Zero
    // for every pair today (no pair table is read), so this pins the
    // internal consistency the kern arm must preserve when a GPOS shaper
    // lands -- a kern added to the width but not to a ref, or the reverse,
    // walks the run off its own reported edge.
    #[test]
    fn shape_runs_width_is_the_sum_of_the_advances_it_returns() {
        let mut gs = GlyphSource::new_vendored(512);
        for (face, px) in [(FACE_BODY, 11.5f32), (FACE_BODY_BOLD, 17.5), (FACE_MONO, MONO_ISLAND_PX)] {
            let (refs, width) = gs.shape_run(face, px, "Wave AV To ff 123".chars());
            assert!(!refs.is_empty(), "the run shaped something");
            let sum: i32 = refs.iter().map(|r| r.advance).sum();
            assert_eq!(sum, width, "face {face}: the width IS the steps");
        }
    }

    // THE WIRING, which the tests above do NOT cover: they call
    // `mono_cell_alpha` and `MonoCell::derive` directly, so they prove the
    // functions and not that `glyph_at` reaches them. Sabotage-measured:
    // with the mono face never consulted -- every cell falling through to
    // the box path and then to the Plex fallback, i.e. TY-4 undone -- all
    // 199 other tests still passed. This is the one that fails.
    //
    // Proven by byte-equality against the cell rasterizer's own output, so
    // "it came from Cornucopia" is exact rather than plausible, plus the
    // dimensions, which the Plex fallback (rasterized at cell_h - 4 and
    // packed at its own tight size) cannot match.
    #[test]
    fn glyph_at_serves_the_mono_cell_from_cornucopia() {
        let mut gs = GlyphSource::new_vendored(512);
        assert!(gs.mono_ok(), "the system mono face is usable");
        gs.set_smooth(12);
        let f = Face::parse(cornucopia::SUBSET_TTF).expect("the subset parses");
        for (px, advance) in [
            (MONO_ISLAND_PX, gs.island_cell().0 as u8),
            (MONO_GRID_PX, gs.mono_cell().0 as u8),
        ] {
            let cell = MonoCell::derive(&f, advance).expect("a cell");
            let want = mono_cell_alpha(&f, f.glyph_id('n'), cell, 12);
            let g = gs.glyph(FACE_MONO, px, 'n').expect("a mono glyph");
            let e = gs.packer.store.glyphs[g.glyph as usize];
            assert_eq!((e.w as i32, e.h as i32), (cell.w, cell.h), "the entry IS the cell");
            assert_eq!((e.left, e.top), (0, cell.baseline), "the cell's bearing");
            assert_eq!(g.advance, cell.w, "a mono advance is the cell width");
            let page = &gs.packer.store.pages[e.page as usize];
            let got: Vec<u8> = (0..e.h)
                .flat_map(|y| {
                    let row = ((e.y + y) * page.w + e.x) as usize;
                    page.alpha[row..row + e.w as usize].to_vec()
                })
                .collect();
            assert_eq!(got, want, "the packed cell is Cornucopia's, stroked");
            assert!(got.iter().any(|&v| v != 0), "and it is not blank");
        }
    }

    // THE CONTRACT WITH THE CELLS TIER (HALCYON-TYPE 4.5): halcyond derives
    // its cell geometry from the font; Aurora, the kernel trusted sink and
    // Halls read it out of a baked atlas. The two must be the same grid, or
    // a pts geometry sized from one tier does not fit the other.
    //
    // The two sides are genuinely independent: the atlas numbers were
    // computed by a Python tool's float ceiling and frozen into a binary
    // blob; these come from integer arithmetic on the font's OS/2 and hmtx
    // at runtime. Nothing here reads an atlas to answer.
    #[test]
    fn the_derived_cell_table_is_the_baked_one() {
        let f = Face::parse(cornucopia::SUBSET_TTF).expect("the subset parses");
        let mut n = 0;
        for a in cornucopia::ADVANCES
            .iter()
            .chain(cornucopia::SCALE_ADVANCES.iter())
        {
            assert!(cornucopia::Atlas::is_baked(*a), "adv {a} is a bake");
            let atlas = cornucopia::Atlas::for_advance(*a);
            let (h, base, em) = f.mono_cell(*a).expect("a cell at every baked advance");
            assert_eq!(atlas.cell_w() as i32, *a as i32, "adv {a}: cell_w IS the advance");
            assert_eq!(h, atlas.cell_h() as i32, "adv {a}: cell_h");
            assert_eq!(base, atlas.baseline() as i32, "adv {a}: baseline");
            // Cornucopia's monospace advance is half its em, so the size
            // that fills a cell of width A is 2A. Derived from the font's
            // own ratio, not assumed -- this pins what the font says.
            assert_eq!(em, *a as f32 * 2.0, "adv {a}: em");
            n += 1;
        }
        assert_eq!(n, 11, "every baked size checked");
        // The control: an advance no atlas holds still yields a cell, and a
        // cell no atlas could have supplied. A derivation that secretly
        // looked one up would have nothing to return here.
        assert_eq!(f.mono_cell(14), Some((31, 25, 28.0)));
        assert_eq!(f.mono_cell(17), Some((38, 31, 34.0)));
    }

    // The two tiers must also carry the same GLYPHS. `subset-cornucopia.py`
    // reads its codepoint list out of `atlas.bin` so this holds by
    // construction -- which is exactly the kind of claim that stops being
    // true the day someone re-bakes without re-subsetting. Every codepoint
    // the cells tier serves, the outline tier serves.
    #[test]
    fn the_subset_carries_every_baked_codepoint() {
        let f = Face::parse(cornucopia::SUBSET_TTF).expect("the subset parses");
        let atlas = cornucopia::Atlas::for_advance(cornucopia::DEFAULT_ADVANCE);
        let mut n = 0;
        for cp in 0u32..0x2600 {
            let Some(ch) = char::from_u32(cp) else { continue };
            if atlas.glyph(ch).is_none() {
                continue;
            }
            assert!(f.has(ch), "U+{cp:04X} is baked but not in the subset");
            n += 1;
        }
        assert_eq!(n, 207, "the bake's glyph count");
        // The box glyphs stay OUT of the bake (the procedural path owns
        // them on the cell; a font's box glyphs are metrics-bound to its
        // own line box). Since I-5 the SUBSET carries U+2500-257F for the
        // free-running face -- a superset, never a different set -- and
        // the block elements (U+2580-259F) stay out of both.
        for ch in ['\u{2500}', '\u{2502}', '\u{250C}'] {
            assert!(atlas.glyph(ch).is_none(), "{ch:?} not baked");
            assert!(f.has(ch), "{ch:?} in the subset (I-5)");
        }
        assert!(atlas.glyph('\u{2588}').is_none() && !f.has('\u{2588}'), "the block elements in neither");
    }

    // HALCYON-INSTRUMENT 7.1 (I-5): the subset carries the six Instrument
    // glyphs and the box-drawing block BEYOND the bake, for the free-running
    // path; the bake does not (the control -- an atlas that also carried
    // them would make the "superset" claim vacuous).
    #[test]
    fn the_subset_carries_the_instrument_glyphs_beyond_the_bake() {
        let f = Face::parse(cornucopia::SUBSET_TTF).expect("the subset parses");
        let atlas = cornucopia::Atlas::for_advance(cornucopia::DEFAULT_ADVANCE);
        for ch in ['\u{03BB}', '\u{2713}', '\u{2039}', '\u{203A}', '\u{2212}', '\u{2318}'] {
            assert!(f.has(ch), "{ch:?} in the subset");
            assert!(atlas.glyph(ch).is_none(), "{ch:?} not baked (the control)");
        }
        let mut n = 0;
        for cp in 0x2500u32..=0x257F {
            let ch = char::from_u32(cp).unwrap();
            assert!(f.has(ch), "U+{cp:04X} in the subset");
            assert!(atlas.glyph(ch).is_none(), "U+{cp:04X} not baked");
            n += 1;
        }
        assert_eq!(n, 128);
        // The Italic carries the same set.
        let it = Face::parse(cornucopia::SUBSET_ITALIC_TTF).expect("the italic subset parses");
        for cp in (0x20u32..0x2600).filter(|&cp| cp != 0xFFFD) {
            let Some(ch) = char::from_u32(cp) else { continue };
            assert_eq!(f.has(ch), it.has(ch), "U+{cp:04X}: the two cuts differ");
        }
        // And the free-running face serves them at the type map's size, at
        // Cornucopia's own advance (half the em): 5 px at 10.
        let mut gs = GlyphSource::new_vendored(512);
        for ch in ['\u{03BB}', '\u{2713}', '\u{2318}', '\u{2550}', '\u{2551}'] {
            let g = gs.glyph(FACE_MONO_TEXT, 10.0, ch).expect("served");
            assert_eq!(g.advance, 5, "{ch:?} at 10 px");
            let e = gs.packer.store.glyphs[g.glyph as usize];
            assert!(e.w > 0 && e.h > 0, "{ch:?} has ink");
        }
    }

    // The free-running mono face (7.2): Cornucopia at the type map's 10 /
    // 11 px and at inline code's 0.86 x 15, with the font's own fractional
    // advance (0.5 em), its own line metrics (hhea), the four phases, the
    // store's stroke -- a proportional face in every respect but the
    // glyphs. The cell's 6 px floor is what it exists to escape: at 10 px
    // the advance is 5.
    #[test]
    fn the_text_mono_runs_free_at_the_type_maps_sizes() {
        let mut gs = GlyphSource::new_vendored(512);
        for (px, adv) in [(10.0f32, 5.0f32), (11.0, 5.5), (12.9, 6.45), (12.0, 6.0)] {
            for ch in ['n', 'W', '0', ' '] {
                let a = gs.advance_f(FACE_MONO_TEXT, px, ch).unwrap();
                assert!((a - adv).abs() < 1e-4, "{ch:?} at {px}: {a} vs {adv}");
                assert_eq!(gs.advance(FACE_MONO_TEXT, px, ch).unwrap(), (adv + 0.5) as i32);
            }
        }
        let lm = gs.line_metrics(FACE_MONO_TEXT, 10.0).unwrap();
        assert_eq!((lm.ascent, lm.descent, lm.line_height), (9, 2, 11), "hhea 889 / -170 / 38 at 10 px");
        let lm = gs.line_metrics(FACE_MONO_TEXT, 11.0).unwrap();
        assert_eq!((lm.ascent, lm.descent, lm.line_height), (10, 2, 12));
        // Not a cell: the raster is the glyph's own box, no wider than the
        // island cell and shorter than it, at its own bearing.
        let (cw, chh, _) = gs.island_cell();
        let n = gs.glyph(FACE_MONO_TEXT, 10.0, 'n').unwrap();
        let e = gs.packer.store.glyphs[n.glyph as usize];
        assert!(e.w as i32 <= cw && (e.h as i32) < chh, "{}x{} vs the {cw}x{chh} cell", e.w, e.h);
        assert_ne!((e.left, e.top), (0, chh), "its own bearing, not the cell's");
        // Phased: four distinct entries.
        let ids: Vec<u32> = (0..crate::outline::PHASES)
            .map(|p| gs.glyph_at(FACE_MONO_TEXT, 10.0, 'n', p).unwrap().glyph)
            .collect();
        for i in 0..ids.len() {
            for j in (i + 1)..ids.len() {
                assert_ne!(ids[i], ids[j], "phases {i} and {j} share an entry");
            }
        }
        // The stroke follows the store, as for every other face.
        let (_, _, plain) = glyph_alpha(&gs, n.glyph);
        gs.set_smooth(12);
        let n2 = gs.glyph(FACE_MONO_TEXT, 10.0, 'n').unwrap();
        let (_, _, stroked) = glyph_alpha(&gs, n2.glyph);
        let sum = |a: &Vec<u8>| a.iter().map(|&v| v as u64).sum::<u64>();
        assert!(sum(&stroked) > sum(&plain), "the stroke adds ink");
    }

    // A codepoint the subset lacks, asked of the free-running face, is the
    // Instrument Sans's glyph at the same size -- the symbol, not a row of
    // .notdef boxes -- and the measure agrees with the paint.
    #[test]
    fn the_text_mono_falls_back_to_the_sans_for_a_missing_codepoint() {
        let mut gs = GlyphSource::new_vendored(512);
        for ch in ['\u{0416}', '\u{03A9}'] {
            assert!(!gs.mono_has(ch), "{ch:?} outside the subset (the premise)");
            let via_mono = gs.glyph(FACE_MONO_TEXT, 10.0, ch).unwrap();
            let via_sans = gs.glyph(FACE_SANS, 10.0, ch).unwrap();
            assert_eq!(via_mono.glyph, via_sans.glyph, "{ch:?}: the Sans's own entry");
            assert_eq!(via_mono.advance, via_sans.advance);
            assert_eq!(gs.advance_f(FACE_MONO_TEXT, 10.0, ch), gs.advance_f(FACE_SANS, 10.0, ch));
            assert_eq!(gs.advance(FACE_MONO_TEXT, 10.0, ch), Some(via_sans.advance));
        }
    }

    // The true Italic (7.1, ruling 11): in service, in the Regular's cell
    // at every advance, and actually slanted -- a different raster from
    // the Regular's on the same cell -- while the box glyphs stay
    // procedural on it too.
    #[test]
    fn the_italic_cell_is_the_regulars_cell_and_it_slants() {
        assert!(mono_face_ok(), "both mono faces usable");
        let re = Face::parse(cornucopia::SUBSET_TTF).unwrap();
        let it = Face::parse(cornucopia::SUBSET_ITALIC_TTF).unwrap();
        assert!(italic_shares_the_cell(&re, &it));
        for a in cornucopia::ADVANCES.iter().chain(cornucopia::SCALE_ADVANCES.iter()) {
            assert_eq!(re.mono_cell(*a), it.mono_cell(*a), "adv {a}");
        }
        let mut gs = GlyphSource::new_vendored(512);
        assert!(gs.mono_italic_ok());
        gs.set_smooth(12);
        let (cw, chh, base) = gs.island_cell();
        let r = gs.glyph(FACE_MONO, MONO_ISLAND_PX, 'a').unwrap();
        let i = gs.glyph(FACE_MONO_ITALIC, MONO_ISLAND_PX, 'a').unwrap();
        assert_ne!(r.glyph, i.glyph, "two entries");
        let ei = gs.packer.store.glyphs[i.glyph as usize];
        assert_eq!((ei.w as i32, ei.h as i32, ei.left, ei.top), (cw, chh, 0, base), "the italic entry IS the cell");
        assert_eq!(i.advance, cw);
        let (_, _, ar) = glyph_alpha(&gs, r.glyph);
        let (_, _, ai) = glyph_alpha(&gs, i.glyph);
        assert_ne!(ar, ai, "the italic differs from the roman");
        assert!(ai.iter().any(|&v| v != 0));
        // Box glyphs: procedural on the italic cell, byte-equal to the roman's.
        let hr = gs.glyph(FACE_MONO, MONO_ISLAND_PX, '\u{2500}').unwrap();
        let hi = gs.glyph(FACE_MONO_ITALIC, MONO_ISLAND_PX, '\u{2500}').unwrap();
        assert_eq!(glyph_alpha(&gs, hr.glyph).2, glyph_alpha(&gs, hi.glyph).2);
        // The metrics are the cell's for both; a phase is refused on both.
        assert_eq!(gs.line_metrics(FACE_MONO_ITALIC, MONO_ISLAND_PX).map(|m| m.line_height), Some(chh));
        let p = gs.glyph_at(FACE_MONO_ITALIC, MONO_ISLAND_PX, 'a', 2).unwrap();
        assert_eq!(p.glyph, i.glyph, "a cell has no phase");
    }

    // The order in the cell path is load-bearing since the subset carries
    // U+2500-257F: the procedural box glyph must win over the font's. The
    // font's glyph is what the free-running face serves, and it is NOT the
    // full-cell line (Cornucopia's box glyphs are bound to its line box).
    #[test]
    fn box_drawing_stays_procedural_in_the_cell_although_the_subset_carries_it() {
        let mut gs = GlyphSource::new_vendored(512);
        assert!(gs.mono_has('\u{2500}'), "the subset carries the light horizontal (the premise)");
        let (cw, chh, _) = gs.island_cell();
        let cell = gs.glyph(FACE_MONO, MONO_ISLAND_PX, '\u{2500}').unwrap();
        let (w, h, a) = glyph_alpha(&gs, cell.glyph);
        let light = libhalcyon::scale::ipx(1, 100).max(1) as usize;
        let want = boxglyph::alpha(cw as usize, chh as usize, '\u{2500}', light).unwrap();
        assert_eq!((w as i32, h as i32), (cw, chh));
        assert_eq!(a, want, "the cell's line is the procedural one, byte for byte");
        // The same codepoint through the free-running face is the font's.
        let free = gs.glyph(FACE_MONO_TEXT, MONO_ISLAND_PX, '\u{2500}').unwrap();
        let (fw, fh, fa) = glyph_alpha(&gs, free.glyph);
        assert!((fw, fh) != (w, h) || fa != a, "the font's glyph, not the procedural cell");
    }

    // The three Instrument weights parse into their slots and are
    // distinct, ordered cuts: at 13 px the ink of 'n' rises 400 -> 450 ->
    // 500 -> 600 -> 700. The slot map never aliases the mono sentinel.
    #[test]
    fn the_instrument_sans_weights_are_distinct_and_the_slots_never_alias() {
        let mut gs = GlyphSource::new_vendored(512);
        assert_eq!(gs.face_count(), VENDORED_FACES, "what the bins' startup guards expect");
        assert_eq!(VENDORED_FACES, 7);
        assert_eq!(prop_slot(FACE_SANS_SEMIBOLD), Some(VENDORED_FACES - 1), "the slot map covers the list exactly");
        assert_eq!(prop_slot(FACE_MONO), None);
        assert_eq!(prop_slot(FACE_MONO_ITALIC), None);
        assert_eq!(prop_slot(FACE_MONO_TEXT), None);
        let slots: Vec<usize> = [
            FACE_BODY,
            FACE_BODY_BOLD,
            FACE_BODY_ITALIC,
            FACE_HEADING_ITALIC,
            FACE_SANS,
            FACE_SANS_MEDIUM,
            FACE_SANS_SEMIBOLD,
        ]
        .iter()
        .map(|f| prop_slot(*f).unwrap())
        .collect();
        assert_eq!(slots, alloc::vec![0, 1, 2, 3, 4, 5, 6]);
        let ink = |gs: &mut GlyphSource, face: u8| {
            let g = gs.glyph(face, 13.0, 'n').unwrap();
            glyph_alpha(gs, g.glyph).2.iter().map(|&v| v as u64).sum::<u64>()
        };
        let r = ink(&mut gs, FACE_SANS);
        let t = ink(&mut gs, FACE_BODY);
        let m = ink(&mut gs, FACE_SANS_MEDIUM);
        let sb = ink(&mut gs, FACE_SANS_SEMIBOLD);
        let b = ink(&mut gs, FACE_BODY_BOLD);
        assert!(r < t && t < m && m < sb && sb < b, "400 {r} < 450 {t} < 500 {m} < 600 {sb} < 700 {b}");
        assert!(is_mono_face(FACE_MONO_TEXT) && is_mono_face(FACE_MONO_ITALIC) && !is_mono_face(FACE_SANS));
        assert!(!is_cell_face(FACE_MONO_TEXT));
    }

    // TY-4's actual behaviour change: a mono cell now carries the theme's
    // smoothing stroke. Before it, the baked bitmap could not, so a
    // proportional glyph falling back INTO a cell was the only stroked
    // thing in the grid -- heavier than the Cornucopia glyph beside it.
    //
    // Measured on the cell's own alpha, not through the packer: the ink
    // must rise, at every advance the scale table can ask for, and the cell
    // must stay exactly a cell.
    //
    // MEASURED 2026-09-08 over 'n' at mem 12: +18% at the shipping cell
    // (advance 6), then 18 / 13 / 20 / 18 across the 125..200% cells and
    // +15% at the largest bake -- the same order the proportional text got
    // at TY-2 (+18% on a 35 px italic), which is what one stroke rule for
    // every tier should produce. The spread is pixel quantization: a
    // 0.14--0.24 px stroke lands differently on the grid per size.
    //
    // The LOWER bound is the load-bearing half. The union bug TY-1 caught
    // -- max(f, s) where f + s - f*s was due -- counts none of the stroke's
    // new ink and yields about +3%. A floor of 8% is what makes this test
    // able to fail on that class rather than merely observe a difference.

    #[test]
    fn the_mono_cell_is_stroked_and_stays_a_cell() {
        let f = Face::parse(cornucopia::SUBSET_TTF).expect("the subset parses");
        for advance in [6u8, 8, 9, 11, 12, 20] {
            let cell = MonoCell::derive(&f, advance).expect("a cell");
            let ink = |mem: u16| -> u32 {
                let a = mono_cell_alpha(&f, f.glyph_id('n'), cell, mem);
                assert_eq!(a.len(), (cell.w * cell.h) as usize, "the cell is the cell");
                a.iter().map(|&v| v as u32).sum()
            };
            let (plain, smoothed) = (ink(0), ink(12));
            assert!(plain > 0, "adv {advance}: the plain fill inked nothing");
            let gain = (smoothed - plain) * 100 / plain;
            assert!(
                (8..=30).contains(&gain),
                "adv {advance}: gain {gain}% ({plain} -> {smoothed}) out of the measured band"
            );
        }
    }

    // The cell is a CLIP, and that is the contract every consumer holds:
    // the alt-screen grid, the pts geometry, and the procedural box glyphs
    // a cell joins against all assume a cell paints its own cell and no
    // other.
    //
    // AND THE CLIP CUTS REAL INK, which measuring it here is how we found
    // out. Cornucopia's OS/2 `usWinAscent` is 889 but its true ink reaches
    // `head.yMax` = 978, so the cell is 89 units (1.07 px at advance 6)
    // short of the font, and every accented Latin-1 capital loses the top
    // row of its diacritic. The BAKE does this too and always has -- its
    // cell comes from the same OS/2 fields (`bake-cornucopia.py`), so
    // Aurora, the kernel trusted sink and Halls have clipped the identical
    // row since G-4. Ground truth: the baked 'A-tilde' holds 14011 ink
    // against this path's 13844, 1.2% apart, which is scanline-vs-zeno and
    // not a different clip.
    //
    // This test pins the defect rather than describing it, so it FAILS the
    // day the geometry is corrected (`head.yMax/yMin` in both tools, all 11
    // atlases re-baked, the row pitch chased downstream -- its own chunk;
    // memory `bug-mono-cell-clips-every-accented-capital`). That is the
    // right moment to update these numbers, and it cannot happen silently.
    //
    // IT RUNS AT EVERY REACHABLE CELL, because TY-6 F4 caught this test
    // asserting "exactly one row, never more" from the shipping cell alone
    // -- and the depth is `ceil(978a/500) - ceil(889a/500)`, which GROWS:
    // two rows at 175% and 200%, four at the largest bake. Worse, the
    // one-cell version could not see the DESCENDER clip at all, since it
    // is zero at advance 6 and worst (19 glyphs) at 150% -- not even at
    // the largest cell. A pin that only ever looks where the number is
    // smallest reports the number as smallest.
    #[test]
    fn the_cell_clips_the_diacritics_the_bake_clips() {
        let f = Face::parse(cornucopia::SUBSET_TTF).expect("the subset parses");
        let atlas = cornucopia::Atlas::for_advance(cornucopia::DEFAULT_ADVANCE);
        // (advance, top rows lost, glyphs losing them, bottom rows lost,
        // glyphs losing those) -- measured 2026-09-08 at every advance the
        // scale table reaches, plus the largest bake.
        let table = [
            (6u8, 1i32, 26u32, 0i32, 0u32),
            (8, 1, 26, 1, 3),
            (9, 1, 26, 1, 19),
            (11, 2, 26, 0, 0),
            (12, 2, 26, 1, 3),
            (20, 4, 26, 1, 3),
        ];
        for (advance, want_top, want_top_n, want_bot, want_bot_n) in table {
            let cell = MonoCell::derive(&f, advance).expect("a cell");
            let (mut top_n, mut top_d) = (0u32, 0i32);
            let (mut bot_n, mut bot_d) = (0u32, 0i32);
            for cp in 0u32..0x2600 {
                let Some(ch) = char::from_u32(cp) else { continue };
                if atlas.glyph(ch).is_none() {
                    continue;
                }
                // Unstroked, so this measures the GEOMETRY, not the stroke.
                let r = f.raster(f.glyph_id(ch), cell.em, 0, 0);
                let a = mono_cell_alpha(&f, f.glyph_id(ch), cell, 0);
                assert_eq!(
                    a.len(),
                    (cell.w * cell.h) as usize,
                    "adv {advance} {ch:?}: the buffer IS the cell -- containment \
                     holds even where ink is lost"
                );
                if r.w == 0 {
                    continue;
                }
                let above = r.top - cell.baseline;
                if above > 0 {
                    top_n += 1;
                    top_d = top_d.max(above);
                }
                let below = (cell.baseline - r.top + r.h as i32) - cell.h;
                if below > 0 {
                    bot_n += 1;
                    bot_d = bot_d.max(below);
                }
                // Nothing escapes horizontally without the stroke, at any cell.
                assert!(r.left >= 0, "adv {advance} {ch:?}: ink left of the cell");
                assert!(
                    r.left + r.w as i32 <= cell.w,
                    "adv {advance} {ch:?}: ink right of the cell"
                );
            }
            assert_eq!(top_n, want_top_n, "adv {advance}: glyphs clipped at the top");
            assert_eq!(top_d, want_top, "adv {advance}: rows lost at the top");
            assert_eq!(bot_n, want_bot_n, "adv {advance}: glyphs clipped at the bottom");
            assert_eq!(bot_d, want_bot, "adv {advance}: rows lost at the bottom");
        }
    }

    // `set_scale` selects the bakes `mono_advances` names, the cells follow
    // (12x27 / 20x44 at 200%), the SHEET's ems land on the right atlas at
    // every scale (the selector's threshold moves with the selection), and
    // the store regens exactly once per change -- a repeat is a no-op.
    #[test]
    fn set_scale_selects_the_bakes_and_regens_once() {
        let mut gs = GlyphSource::new_vendored(512);
        assert_eq!(gs.scale(), 100);
        let _ = gs.glyph(FACE_BODY, 11.5, 'a').unwrap();
        assert!(!gs.set_scale(100), "the current scale is a no-op");
        assert_eq!(gs.gen(), 0, "and evicts nothing");
        assert!(gs.set_scale(200));
        assert_eq!(gs.gen(), 1, "one eviction");
        assert!(gs.packer.store.glyphs.is_empty(), "the old cells' glyphs went");
        assert_eq!(gs.island_cell().0, 12);
        assert_eq!(gs.mono_cell().0, 12, "one mono size: the grid cell is the island cell");
        assert_eq!(gs.island_cell().1, 27);
        assert_eq!(gs.mono_cell().1, 27);
        for pct in [100u16, 125, 150, 175, 200] {
            gs.set_scale(pct);
            let (i, g) = mono_advances(pct);
            let sheet = crate::layout::daylight_sheet(pct);
            assert_eq!(gs.advance(FACE_MONO, sheet.mono_island_px, 'x'), Some(i as i32), "{pct}: the sheet's island em is the island cell");
            assert_eq!(gs.advance(FACE_MONO, sheet.mono_grid_px, 'x'), Some(g as i32), "{pct}: the sheet's grid em is the grid cell");
            let lm = gs.line_metrics(FACE_MONO, sheet.mono_island_px).unwrap();
            assert_eq!(lm.line_height, gs.island_cell().1);
        }
        assert!(gs.set_scale(100));
        assert_eq!((gs.island_cell().0, gs.mono_cell().0), (6, 6), "back to the 1.0 cell (one mono size)");
    }

    // HALCYON-SCALE 7: the eviction bound follows the display area -- the
    // floor at the reference display (the 1337a218 constant, unchanged at
    // 1280x800), eight times it on a 4K scanout -- and `set_display`
    // moves both the bound and the packer's hard cap.
    #[test]
    fn the_atlas_bound_follows_the_display_area() {
        assert_eq!(atlas_pages_for(1280, 800, 512), MAX_ATLAS_PAGES, "the reference display is the floor");
        assert_eq!(atlas_pages_for(640, 480, 512), MAX_ATLAS_PAGES, "smaller never below the floor");
        assert_eq!(atlas_pages_for(3840, 2160, 512), 64, "4K: twice the area in pages");
        assert_eq!(atlas_pages_for(2560, 1600, 512), 32);
        assert_eq!(atlas_pages_for(0, 0, 512), MAX_ATLAS_PAGES, "no display: the floor");
        let mut gs = GlyphSource::new_vendored(32);
        assert_eq!(gs.evict_pages(), MAX_ATLAS_PAGES);
        // 4K on 32-px pages: 2 x 8294400 / 1024 = 16200 pages; the point is
        // the bound moves, not its size -- use a display the tiny pages make
        // reachable: 128x128 -> ceil(2 x 16384 / 1024) = 32.
        gs.set_display(128, 128);
        assert_eq!(gs.evict_pages(), 32);
        let mut cp = 0x4E00u32;
        let mut next = |gs: &mut GlyphSource, n: usize| {
            for _ in 0..n {
                let _ = gs.glyph(FACE_BODY, 11.5, char::from_u32(cp).unwrap());
                cp += 1;
            }
        };
        next(&mut gs, 800);
        assert_eq!(gs.packer.store.pages.len(), 32 + ATLAS_PAGE_SLACK, "the hard cap moved with the bound");
        assert!(gs.evict_if_full());
        // A display under the floor on these pages (64x64 -> 8): the floor.
        gs.set_display(64, 64);
        assert_eq!(gs.evict_pages(), MAX_ATLAS_PAGES, "back to the floor");
        next(&mut gs, 800);
        assert_eq!(gs.packer.store.pages.len(), MAX_ATLAS_PAGES + ATLAS_PAGE_SLACK);
    }

    // HALCYON-SCALE 7's pin: a full screen of the largest heading at 200%
    // on the reference display packs under the cap -- every glyph served,
    // none refused, the store well inside the bound.
    #[test]
    fn a_screen_of_the_largest_heading_at_200_packs_under_the_cap() {
        let mut gs = GlyphSource::new_vendored(512);
        gs.set_display(1280, 800);
        gs.set_scale(200);
        let sheet = crate::layout::daylight_sheet(200);
        let px = sheet.hdr_px[0];
        let lm = gs.line_metrics(FACE_BODY, px).unwrap();
        let line_h = (px * 1.25 + 0.5) as i32;
        let rows = 800 / line_h;
        // Distinct codepoints Plex lacks: each its own .notdef box at 35
        // px, the widest honest working set a screen can hold.
        let per_row = 1280 / (lm.ascent / 2).max(8);
        let mut cp = 0x4E00u32;
        let mut served = 0;
        for _ in 0..rows {
            for _ in 0..per_row {
                if gs.glyph(FACE_BODY, px, char::from_u32(cp).unwrap()).is_some() {
                    served += 1;
                }
                cp += 1;
            }
        }
        assert_eq!(served, (rows * per_row) as usize, "every glyph of the screen served");
        let pages = gs.packer.store.pages.len();
        assert!(pages <= gs.evict_pages(), "{pages} pages: inside the eviction bound ({})", gs.evict_pages());
        assert!(!gs.evict_if_full(), "a screen of headings does not trip the eviction");
    }

    // The procedural box strokes follow the hairline rule (COMPOSITION 1):
    // a light line is 1 px through 125% and 2 px from 150%, a heavy line
    // always wider than it; the cell's own geometry (the double gap) is
    // the bake's.
    #[test]
    fn box_light_stroke_is_the_hairline_at_scale() {
        let rows_at_x0 = |gs: &GlyphSource, id: u32, cw: u32, chh: u32| {
            let (_, _, a) = glyph_alpha(gs, id);
            (0..chh).filter(|&y| a[(y * cw) as usize] == 255).count()
        };
        for (pct, want_light) in [(100u16, 1usize), (125, 1), (150, 2), (175, 2), (200, 2)] {
            let mut gs = GlyphSource::new_vendored(512);
            gs.set_scale(pct);
            let sheet = crate::layout::daylight_sheet(pct);
            for px in [sheet.mono_island_px, sheet.mono_grid_px] {
                let (cw, chh, _) = if px >= sheet.mono_grid_px { gs.mono_cell() } else { gs.island_cell() };
                let (cw, chh) = (cw as u32, chh as u32);
                let light = gs.glyph(FACE_MONO, px, '\u{2500}').unwrap();
                assert_eq!(rows_at_x0(&gs, light.glyph, cw, chh), want_light, "{pct}% cell {cw}: the light stroke is the hairline");
                let heavy = gs.glyph(FACE_MONO, px, '\u{2501}').unwrap();
                assert!(rows_at_x0(&gs, heavy.glyph, cw, chh) > want_light, "{pct}% cell {cw}: heavy outweighs light");
            }
        }
    }
    /// I-5b: a symbol the Instrument Sans lacks (ut's turnstile) is served
    /// by the free-running mono AT THE SAME PX -- the prompt's glyphs at
    /// the body size (7.4) -- where the legacy cuts keep the island cell;
    /// a codepoint neither face has is the Sans's .notdef, not a loop.
    #[test]
    fn the_instrument_sans_serves_a_missing_symbol_from_the_free_running_mono() {
        let mut gs = GlyphSource::new_vendored(64);
        assert_eq!(gs.advance_f(FACE_SANS, 15.0, '\u{22a2}'), Some(7.5), "Cornucopia's 0.5 em at 15");
        assert_eq!(gs.advance_f(FACE_SANS_MEDIUM, 34.0, '\u{22a2}'), Some(17.0));
        assert_eq!(gs.advance_f(FACE_BODY, 11.5, '\u{22a2}'), Some(6.0), "legacy: the island cell");
        let via_sans = gs.glyph_at(FACE_SANS, 15.0, '\u{22a2}', 1).expect("served");
        let direct = gs.glyph_at(FACE_MONO_TEXT, 15.0, '\u{22a2}', 1).expect("served");
        assert_eq!((via_sans.glyph, via_sans.advance), (direct.glyph, direct.advance), "the mono's own entry");
        // Neither face: Plex's .notdef, finite, terminating.
        let none = gs.advance_f(FACE_SANS, 15.0, '\u{4e2d}').expect("the .notdef advance");
        assert!(none > 0.0 && none < 15.0);
        assert!(gs.glyph_at(FACE_SANS, 15.0, '\u{4e2d}', 0).is_some());
        // And the mono's own fallback still goes the other way.
        assert_eq!(gs.advance_f(FACE_MONO_TEXT, 15.0, '\u{4e2d}'), Some(none));
    }
}
