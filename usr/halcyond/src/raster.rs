// The glyph source: fontdue faces -> a cartoon atlas, cached. The author-
// side half of the 13.2 division of knowledge -- layout asks THIS for
// glyph ids + advances and writes resolved runs; executors never see a
// font, only the finished alpha pages.
//
// Sizes are quantized to half pixels for the cache key (the stylesheet
// speaks whole px today; the quantum keeps a future fractional size from
// silently splitting the cache). Advances are rounded to integer pixels
// (the MVP pen; subpixel positioning is a stylesheet-era refinement).

use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use cartoon::{AtlasPacker, GlyphRef};

/// A face slot in this source: the four vendored IBM Plex Sans faces
/// (HALCYON-VISUAL.md section 7 + HALCYON.md section 4), plus the system
/// monospace -- the baked Cornucopia atlases (fixed cell, one size per
/// advance), serving mono islands + terminal content through the SAME
/// packer/id space so one atlas store feeds the executor. The operator's
/// weight rule (baseline = Text 450, bigger type = Regular 400) makes
/// headings a DISTINCT weight from body, so heading-italic is its own slot
/// rather than the body italic at a larger size. The four proportional faces
/// index `self.faces` directly; FACE_MONO is a sentinel, special-cased before
/// any `faces[]` access -- never a slot.
pub const FACE_BODY: u8 = 0; // Plex Text (450): baseline prose, prompt, chrome, objects
pub const FACE_BODY_BOLD: u8 = 1; // Plex Bold (700): em--strong, the one bold
pub const FACE_BODY_ITALIC: u8 = 2; // Plex Text Italic (450): em--emph, baseline-size slant
pub const FACE_HEADING_ITALIC: u8 = 3; // Plex Regular Italic (400): headings, italic in full
pub const FACE_MONO: u8 = 4;

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
pub const MONO_GRID_PX: f32 = 20.0;
/// The two mono advances at 100%: the island's 6 and the grid's 10.
pub const MONO_ISLAND_ADVANCE: u8 = 6;
pub const MONO_GRID_ADVANCE: u8 = cornucopia::DEFAULT_ADVANCE;

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

/// The mono advances at a display scale (HALCYON-SCALE 6): the island
/// `round_half_up(6 x s)`, the grid `round_half_up(10 x s)` -- 6/10, 8/13,
/// 9/15, 11/18, 12/20 at the five values -- each the bake it names, or the
/// nearest SMALLER bake when that one is absent (a smaller cell never
/// overflows the row pitch the sheet sized for the wanted one; a larger
/// would), down to the legibility floor of 6. Pure: the Sheet derives its
/// mono ems from the same answer the source selects its atlases by, so the
/// two cannot disagree.
pub fn mono_advances(pct: u16) -> (u8, u8) {
    let baked_at_most = |want: i32| -> u8 {
        let mut a = want.clamp(MONO_ISLAND_ADVANCE as i32, u8::MAX as i32) as u8;
        while a > MONO_ISLAND_ADVANCE && !cornucopia::Atlas::is_baked(a) {
            a -= 1;
        }
        a
    };
    (
        baked_at_most(libhalcyon::scale::ipx(MONO_ISLAND_ADVANCE as i32, pct)),
        baked_at_most(libhalcyon::scale::ipx(MONO_GRID_ADVANCE as i32, pct)),
    )
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

/// Fonts + packer + cache, one generation at a time. `regen()` evicts all
/// three together, so a cached id can never outlive the pages it points
/// into (the 13.2 stale rule holds by construction on the author side
/// too; the executor's gen check is the belt).
pub struct GlyphSource {
    faces: Vec<fontdue::Font>,
    grid: cornucopia::Atlas,
    island: cornucopia::Atlas,
    pub packer: AtlasPacker,
    cache: BTreeMap<(u8, u32, char), Cached>,
    /// The display scale the mono atlases were selected for (percent).
    scale: u16,
    /// The between-frames eviction bound (`atlas_pages_for` of the last
    /// `set_display`; the floor before one).
    evict_pages: usize,
}

/// The cache key's size quantum: half pixels.
#[inline]
fn size_q(px: f32) -> u32 {
    (px * 2.0 + 0.5) as u32
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
        px >= (self.island.cell_w() + self.grid.cell_w()) as f32
    }

    /// Build over the vendored faces at 100% on the floor bound. `page` is
    /// the atlas page geometry (one page holds many shelves; 512 fits
    /// several sizes of a Latin working set).
    pub fn new_vendored(page: u32) -> GlyphSource {
        let mut faces = Vec::new();
        // Order matches the FACE_* indices: Text, Bold, Text-Italic, then the
        // Regular-weight heading italic (a distinct weight from body, per the
        // operator's baseline=Text / bigger=Regular rule).
        for bytes in [
            crate::IBM_PLEX_SANS_TEXT,
            crate::IBM_PLEX_SANS_BOLD,
            crate::IBM_PLEX_SANS_TEXT_ITALIC,
            crate::IBM_PLEX_SANS_HEADING_ITALIC,
        ] {
            // The vendored faces parse by construction; a fontdue reject
            // here is a build-input defect, not a runtime input -- panic
            // in tests, but stay total in the API: skip the face (its
            // glyphs then miss, and text falls back per the caller).
            if let Ok(f) = fontdue::Font::from_bytes(bytes, fontdue::FontSettings::default()) {
                faces.push(f);
            }
        }
        let mut packer = AtlasPacker::new(page, page);
        packer.set_max_pages((MAX_ATLAS_PAGES + ATLAS_PAGE_SLACK) as u32);
        GlyphSource {
            faces,
            grid: cornucopia::Atlas::for_advance(MONO_GRID_ADVANCE),
            island: cornucopia::Atlas::for_advance(MONO_ISLAND_ADVANCE),
            packer,
            cache: BTreeMap::new(),
            scale: 100,
            evict_pages: MAX_ATLAS_PAGES,
        }
    }

    /// The display scale the mono atlases serve (percent).
    pub fn scale(&self) -> u16 {
        self.scale
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
        self.island = cornucopia::Atlas::for_advance(island);
        self.grid = cornucopia::Atlas::for_advance(grid);
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

    fn mono_atlas(&self, px: f32) -> &cornucopia::Atlas {
        if self.mono_is_grid(px) {
            &self.grid
        } else {
            &self.island
        }
    }

    /// The GRID mono cell geometry (w, h, baseline): the alt-screen raw-VT
    /// cell and the pts geometry (cols/rows) every tile is sized from.
    pub fn mono_cell(&self) -> (i32, i32, i32) {
        (
            self.grid.cell_w() as i32,
            self.grid.cell_h() as i32,
            self.grid.baseline() as i32,
        )
    }

    /// The ISLAND mono cell geometry (w, h, baseline): the document's mono
    /// -- islands, pre, raw output, the menu's literals.
    pub fn island_cell(&self) -> (i32, i32, i32) {
        (
            self.island.cell_w() as i32,
            self.island.cell_h() as i32,
            self.island.baseline() as i32,
        )
    }

    pub fn face_count(&self) -> usize {
        self.faces.len()
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
        let q = if face == FACE_MONO {
            self.mono_is_grid(px) as u32
        } else {
            size_q(px)
        };
        if let Some(c) = self.cache.get(&(face, q, ch)) {
            return Some(c.advance);
        }
        if face == FACE_MONO {
            return Some(self.mono_atlas(px).cell_w() as i32);
        }
        let f = self.faces.get(face as usize)?;
        if f.lookup_glyph_index(ch) == 0 && ch != '\u{FFFD}' && self.island.glyph(ch).is_some() {
            return Some(self.island.cell_w() as i32);
        }
        Some((f.metrics(ch, px).advance_width + 0.5) as i32)
    }

    /// The glyph for `ch` at `px` in `face`, rasterizing on first use.
    /// None: unknown face, or the bitmap can never fit a page. A missing
    /// codepoint is NOT None -- fontdue rasterizes its .notdef box, which
    /// is the correct visible outcome for unmapped input.
    ///
    /// FACE_MONO's `px` selects the atlas (island or grid; the bake has one
    /// size per advance) and serves the Cornucopia cell; a box-drawing /
    /// block-element codepoint (U+2500-259F, deliberately absent from the
    /// bake) is drawn PROCEDURALLY on the cell so joins are pixel-exact
    /// across cells; any other codepoint the bake lacks falls back to the
    /// body face (Plex Text) rasterized to the cell height with the advance
    /// FORCED to the cell width (the grid survives; the glyph may clip).
    pub fn glyph(&mut self, face: u8, px: f32, ch: char) -> Option<GlyphRef> {
        let q = if face == FACE_MONO {
            self.mono_is_grid(px) as u32
        } else {
            size_q(px)
        };
        let key = (face, q, ch);
        if let Some(c) = self.cache.get(&key) {
            return Some(GlyphRef {
                glyph: c.id,
                advance: c.advance,
            });
        }
        if face == FACE_MONO {
            let atlas = *self.mono_atlas(px);
            let (cw, chh, base) = (
                atlas.cell_w() as i32,
                atlas.cell_h() as i32,
                atlas.baseline() as i32,
            );
            if let Some(alpha) = atlas.glyph(ch) {
                let id = self.packer.insert(cw as u32, chh as u32, alpha, 0, base)?;
                self.cache.insert(key, Cached { id, advance: cw });
                return Some(GlyphRef {
                    glyph: id,
                    advance: cw,
                });
            }
            // The light stroke is the hairline at this scale (COMPOSITION
            // 1: a flat structural line scales `max(1, round(s))`, and a
            // box line joining cells is one) -- 1 px up to 125%, 2 from 150.
            let light = libhalcyon::scale::ipx(1, self.scale).max(1) as usize;
            if let Some(alpha) = boxglyph::alpha(cw as usize, chh as usize, ch, light) {
                let id = self.packer.insert(cw as u32, chh as u32, &alpha, 0, base)?;
                self.cache.insert(key, Cached { id, advance: cw });
                return Some(GlyphRef {
                    glyph: id,
                    advance: cw,
                });
            }
            // Fallback: body-rasterized at cell height, grid-advance.
            let f = self.faces.get(FACE_BODY as usize)?;
            let (m, bitmap) = f.rasterize(ch, (chh - 4) as f32);
            let id = self.packer.insert(
                m.width as u32,
                m.height as u32,
                &bitmap,
                m.xmin,
                m.height as i32 + m.ymin,
            )?;
            self.cache.insert(key, Cached { id, advance: cw });
            return Some(GlyphRef {
                glyph: id,
                advance: cw,
            });
        }
        let f = self.faces.get(face as usize)?;
        // A codepoint the proportional face lacks (IBM Plex Sans has no
        // U+22A2 -- ut's turnstile) is served from the island bake rather
        // than as Plex's .notdef box: Cornucopia carries the prompt glyph by
        // design, and a symbol at the island cell reads as the symbol, not
        // as tofu. Still None for a glyph neither has (the .notdef box then).
        if f.lookup_glyph_index(ch) == 0 && ch != '\u{FFFD}' {
            let atlas = self.island;
            if let Some(alpha) = atlas.glyph(ch) {
                let (cw, chh, base) = (
                    atlas.cell_w() as i32,
                    atlas.cell_h() as i32,
                    atlas.baseline() as i32,
                );
                let id = self.packer.insert(cw as u32, chh as u32, alpha, 0, base)?;
                self.cache.insert(key, Cached { id, advance: cw });
                return Some(GlyphRef {
                    glyph: id,
                    advance: cw,
                });
            }
        }
        let (m, bitmap) = f.rasterize(ch, px);
        // fontdue's bitmap is w*h coverage bytes; its `ymin` is the
        // bitmap BOTTOM relative to the baseline (y-up), so the cartoon
        // bearing (top, y-down from the baseline) is height + ymin.
        let id = self.packer.insert(
            m.width as u32,
            m.height as u32,
            &bitmap,
            m.xmin,
            m.height as i32 + m.ymin,
        )?;
        let advance = (m.advance_width + 0.5) as i32;
        self.cache.insert(key, Cached { id, advance });
        Some(GlyphRef { glyph: id, advance })
    }

    /// Vertical metrics for a face at a size (integer px, y-down).
    /// FACE_MONO's are the selected atlas cell's.
    pub fn line_metrics(&self, face: u8, px: f32) -> Option<LineMetrics> {
        if face == FACE_MONO {
            let atlas = self.mono_atlas(px);
            let (chh, base) = (atlas.cell_h() as i32, atlas.baseline() as i32);
            return Some(LineMetrics {
                ascent: base,
                descent: chh - base,
                line_height: chh,
            });
        }
        let f = self.faces.get(face as usize)?;
        let lm = f.horizontal_line_metrics(px)?;
        let ascent = (lm.ascent + 0.5) as i32;
        let descent = (-lm.descent + 0.5) as i32; // fontdue descent is negative
        let gap = (lm.line_gap + 0.5) as i32;
        Some(LineMetrics {
            ascent,
            descent,
            line_height: ascent + descent + gap,
        })
    }

    /// The kerning adjustment between two glyphs at a size (integer px), 0
    /// when the face carries no pair. The author adds this into the PRECEDING
    /// glyph's resolved advance. IBM Plex Sans ships kerning only in GPOS, and
    /// fontdue's `horizontal_kern` reads only the legacy `kern` table, so this
    /// returns 0 for every pair on the vendored faces -- Plex renders with flat
    /// advances (an MVP posture; a GPOS shaper is the future refinement).
    pub fn kern(&self, face: u8, px: f32, left: char, right: char) -> i32 {
        let Some(f) = self.faces.get(face as usize) else {
            return 0;
        };
        match f.horizontal_kern(left, right, px) {
            Some(k) => {
                if k >= 0.0 {
                    (k + 0.5) as i32
                } else {
                    -((-k + 0.5) as i32)
                }
            }
            None => 0,
        }
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
        assert_eq!(gs.face_count(), 4, "all four vendored IBM Plex Sans faces parse (Text, Bold, Text-Italic, Regular-Italic)");
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
    }

    #[test]
    fn kern_is_zero_plex_ships_no_legacy_kern_table() {
        // IBM Plex Sans carries kerning in GPOS only; fontdue reads only the
        // legacy `kern` table, so every pair returns 0 -- flat advances, the
        // recorded MVP posture (a GPOS shaper is the future refinement). This
        // also guards the other direction: a future face WITH a legacy table
        // would change layout metrics, and this test would catch it.
        let mut gs = GlyphSource::new_vendored(512);
        gs.glyph(FACE_BODY, 32.0, 'A').unwrap();
        gs.glyph(FACE_BODY, 32.0, 'V').unwrap();
        assert_eq!(
            gs.kern(FACE_BODY, 32.0, 'A', 'V'),
            0,
            "no legacy kern pair on Plex (GPOS is not read by fontdue)"
        );
        assert_eq!(gs.kern(FACE_BODY, 32.0, 'x', 'x'), 0, "no pair either way");
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
    fn two_mono_sizes_island_below_grid() {
        // The island (document mono) is the advance-6 cell, the grid (alt
        // screen / pts) the advance-10 cell; the requested px picks the atlas
        // and the cache keys them apart.
        let mut gs = GlyphSource::new_vendored(512);
        let (iw, ih, ib) = gs.island_cell();
        let (gw, gh, gb) = gs.mono_cell();
        assert_eq!((iw, gw), (6, 10), "island advance 6, grid advance 10");
        assert!(ih < gh && ib < gb, "the island cell is the smaller box ({ih} < {gh})");
        let a_island = gs.glyph(FACE_MONO, MONO_ISLAND_PX, 'a').unwrap();
        let a_grid = gs.glyph(FACE_MONO, MONO_GRID_PX, 'a').unwrap();
        assert_eq!(a_island.advance, iw);
        assert_eq!(a_grid.advance, gw);
        assert_ne!(a_island.glyph, a_grid.glyph, "two atlases, two glyph ids");
        // Any island-range px shares the island entry (one cache key per atlas).
        let a_island2 = gs.glyph(FACE_MONO, 10.5, 'a').unwrap();
        assert_eq!(a_island.glyph, a_island2.glyph);
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
        let mut refs = alloc::vec::Vec::new();
        let mut prev: Option<char> = None;
        for ch in "Halcyon".chars() {
            let mut gr = gs.glyph(FACE_BODY, 16.0, ch).unwrap();
            if let Some(p) = prev {
                // Kern into the preceding advance the way layout will.
                let k = gs.kern(FACE_BODY, 16.0, p, ch);
                if let Some(last) = refs.last_mut() {
                    let l: &mut GlyphRef = last;
                    l.advance += k;
                }
                let _ = &mut gr;
            }
            refs.push(gr);
            prev = Some(ch);
        }
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
    // operator's table (round half up of 6s and 10s), every one a bake; an
    // off-table percent lands on the nearest smaller bake, never above
    // (a larger cell would overflow the row pitch the sheet sized for it),
    // never below the legibility floor.
    #[test]
    fn mono_advances_are_the_scale_table_and_every_one_is_baked() {
        assert_eq!(mono_advances(100), (6, 10));
        assert_eq!(mono_advances(125), (8, 13), "7.5 up, 12.5 up");
        assert_eq!(mono_advances(150), (9, 15));
        assert_eq!(mono_advances(175), (11, 18), "10.5 up, 17.5 up");
        assert_eq!(mono_advances(200), (12, 20));
        for p in [100u16, 125, 150, 175, 200] {
            let (i, g) = mono_advances(p);
            assert!(cornucopia::Atlas::is_baked(i) && cornucopia::Atlas::is_baked(g), "{p}: {i}/{g} baked");
            assert!(i < g, "the island cell is always the smaller");
        }
        // Off the table: 140% wants 8 / 14 -- 14 is not baked, 13 is.
        assert_eq!(mono_advances(140), (8, 13), "the nearest smaller bake");
        // Below 100 (not a v1 value; the function is total): the floor.
        assert_eq!(mono_advances(50), (6, 6));
        assert_eq!(mono_advances(0), (6, 6));
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
        assert_eq!(gs.mono_cell().0, 20);
        assert_eq!(gs.island_cell().1, 27);
        assert_eq!(gs.mono_cell().1, 44);
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
        assert_eq!((gs.island_cell().0, gs.mono_cell().0), (6, 10), "back to the 1.0 cells");
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
}
