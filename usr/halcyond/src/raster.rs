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

/// The two mono SIZES (HALCYON-COMPOSITION 2; Cornucopia bakes 0.5 em per
/// advance px). The ISLAND is the document's mono -- inline `em class=code`,
/// a `pre` block, raw terminal output, the menu's literals -- the advance-6
/// bake (6x14, a 12 px em: the closest cell to the mockup's 10 px Cornucopia,
/// advance 5 being below the box-glyph legibility floor). The GRID is the
/// alt-screen / pts cell (advance 10, 10x22): a full-screen program owns its
/// cells and the pts geometry is sized from it. `glyph`/`line_metrics` pick
/// the atlas from the requested px, so a caller says which mono it means the
/// same way it says a proportional size.
pub const MONO_ISLAND_PX: f32 = 12.0;
pub const MONO_GRID_PX: f32 = 20.0;

/// The atlas page bound `evict_if_full` enforces between frames: 16 pages
/// of the 512-px page every source is built with = 4 MiB of alpha, ~16x a
/// Latin working set (four faces at three sizes plus both mono cells pack
/// into about one page). The transcript's bytes are untrusted; without a
/// bound a program printing distinct codepoints grew the store ~10 MB per
/// size until the compositor's fixed heap died mute (I-32's in-process
/// face).
pub const MAX_ATLAS_PAGES: usize = 16;
const MONO_ISLAND_ADVANCE: u8 = 6;
const MONO_GRID_ADVANCE: u8 = cornucopia::DEFAULT_ADVANCE;

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
}

/// The cache key's size quantum: half pixels.
#[inline]
fn size_q(px: f32) -> u32 {
    (px * 2.0 + 0.5) as u32
}

/// Which mono atlas a requested size means: the grid cell from the grid em
/// up, the island below it. Two atlases, so the cache key is the choice,
/// not the px -- every island request shares one entry per glyph.
#[inline]
fn mono_is_grid(px: f32) -> bool {
    px >= (MONO_ISLAND_PX + MONO_GRID_PX) / 2.0
}

impl GlyphSource {
    /// Build over the vendored faces. `page` is the atlas page geometry
    /// (one page holds many shelves; 512 fits several sizes of a Latin
    /// working set).
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
        GlyphSource {
            faces,
            grid: cornucopia::Atlas::for_advance(MONO_GRID_ADVANCE),
            island: cornucopia::Atlas::for_advance(MONO_ISLAND_ADVANCE),
            packer: AtlasPacker::new(page, page),
            cache: BTreeMap::new(),
        }
    }

    fn mono_atlas(&self, px: f32) -> &cornucopia::Atlas {
        if mono_is_grid(px) {
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
            mono_is_grid(px) as u32
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
            if let Some(alpha) = boxglyph::alpha(cw as usize, chh as usize, ch) {
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

    /// The growth bound, applied BETWEEN frames: when the store holds
    /// `MAX_ATLAS_PAGES` pages or more, evict everything (`regen`) so the
    /// next frame re-packs only its working set. Within a frame `glyph()`
    /// only ever inserts, so a frame's `gen()` stamp stays valid across it
    /// (tile::paint_grid reads it once); a stream of distinct codepoints --
    /// a program printing its way through the BMP -- can therefore grow the
    /// store by at most one frame's glyphs past the bound, never without
    /// limit toward the fixed heap's silent OOM exit. Returns true when it
    /// evicted (every layout cache keys on `gen()` and re-lays).
    pub fn evict_if_full(&mut self) -> bool {
        if self.packer.store.pages.len() >= MAX_ATLAS_PAGES {
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
    pub fn alpha(cw: usize, ch: usize, c: char) -> Option<Vec<u8>> {
        let cp = c as u32;
        if cw < 2 || ch < 2 {
            return None;
        }
        if (0x2500..=0x257F).contains(&cp) {
            let (u, d, l, r) = ARMS[(cp - 0x2500) as usize];
            if u == NONE && d == NONE && l == NONE && r == NONE {
                return None;
            }
            return Some(arms(cw, ch, [u, d, l, r]));
        }
        if (0x2580..=0x259F).contains(&cp) {
            return Some(block(cw, ch, cp));
        }
        None
    }

    fn arms(cw: usize, ch: usize, w: [u8; 4]) -> Vec<u8> {
        let mut px = alloc::vec![0u8; cw * ch];
        let cx = cw / 2;
        let cy = ch / 2;
        // Stroke geometry per cell size: the heavy band and the double gap
        // scale with the cell so a 6-px island and a 10-px grid both read.
        let heavy = if cw >= 9 { 3 } else { 2 };
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
        let width = |a: u8| if a == HEAVY { heavy } else { 1 };
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
        let unbounded = gs.packer.store.pages.len();
        assert!(unbounded > MAX_ATLAS_PAGES, "the control: 400 glyphs on 32-px pages exceed the bound ({unbounded} pages)");
        assert!(gs.evict_if_full(), "over the bound: evicted");
        assert_eq!(gs.packer.store.pages.len(), 0);
        assert_eq!(gs.gen(), 1, "the generation bumped");
        assert!(!gs.evict_if_full(), "empty: nothing to evict");
        // Frames of 20 glyphs each: the store never exceeds the bound by
        // more than one frame's growth, and the gen keeps bumping.
        let per_frame = 20;
        let mut peak = 0;
        for _ in 0..200 {
            gs.evict_if_full();
            next(&mut gs, per_frame);
            peak = peak.max(gs.packer.store.pages.len());
        }
        assert!(peak <= MAX_ATLAS_PAGES + per_frame, "peak {peak} pages: bounded by the frame's growth");
        assert!(gs.gen() >= 2, "evicted again along the way (gen {})", gs.gen());
        // A glyph looked up after an eviction is served fresh under the new
        // generation, not from the cleared cache.
        let a = gs.glyph(FACE_BODY, 11.5, 'a').expect("a");
        assert!((a.glyph as usize) < gs.packer.store.glyphs.len());
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
}
