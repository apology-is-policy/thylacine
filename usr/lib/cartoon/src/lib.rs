// cartoon -- the display list (HALCYON.md section 13.2) and its CPU executor.
//
// A tapestry CARTOON is the full-size design a weaver executes -- exactly
// this artifact's role in the section-13.1 architecture: halcyond (the only
// place that thinks) draws the cartoon; a dumb executor weaves it into
// pixels. The CPU executor here is the universal floor (runs wherever
// aurora runs); the vk executor (H-6) will execute the SAME ops via the
// serialized wire form this op set is shaped for.
//
// Division of knowledge (13.2's rules, binding):
//   - The executor never measures text: glyph runs arrive resolved (ids +
//     advances from the author's shaping); the executor only blits and
//     advances the pen.
//   - `atlas_gen` names the atlas-store generation the ops were authored
//     against; the executor paints glyphs only when it equals the store it
//     was HANDED, so a stale page reference is impossible by construction.
//   - The executor does not diff: damage is the author's job
//     (`present_rects`); `clip` here is an execution bound, not a diff.
//   - `Embed` paints nothing (v0): it reserves flow space for a compositor-
//     placed inline surface; the author paints any placeholder ground
//     beneath it with `Rect`.
//
// Pure no_std + alloc, zero dependencies (the vt/beacon crate pattern);
// host-tested (`cargo test -p cartoon --target aarch64-apple-darwin`).

#![no_std]

extern crate alloc;

use alloc::vec::Vec;

/// The op-set version. In-process consumers ignore it; the H-6 wire
/// encoding (little-endian, length-prefixed) carries it per list.
pub const CARTOON_V0: u32 = 0;

/// One drawing op. Coordinates are surface-local pixels, signed because
/// scrolled content legitimately starts above/left of the viewport; every
/// write is clipped at execute time.
pub enum Op {
    /// Whole-surface ground (under a clip: the clip rect).
    Clear { color: u32 },
    /// Filled rectangle: fills, rules, selection bands, strip segments.
    Rect { x: i32, y: i32, w: u32, h: u32, color: u32 },
    /// A resolved glyph run: `runs[start .. start+count]` blit left to
    /// right from `(baseline_x, baseline_y)`, each entry advancing the pen
    /// AFTER its blit. `color` is the text color; pages carry alpha only.
    Glyphs { atlas_gen: u32, baseline_x: i32, baseline_y: i32, color: u32, start: u32, count: u32 },
    /// Decoded raster (a blob halcyond owns), composited src-over.
    Image { blob_id: u32, x: i32, y: i32, w: u32, h: u32 },
    /// An inline Tapestry surface's place in the flow. Paints nothing in
    /// the CPU executor (see the header); the compositor places the actual
    /// surface (TAPESTRY section 14 inline-live).
    Embed { surface_ref: u32, x: i32, y: i32, w: u32, h: u32 },
}

/// One glyph of a run: an index into `AtlasStore.glyphs` plus the pen
/// advance to the NEXT glyph (author-resolved; kerning already applied).
#[derive(Clone, Copy)]
pub struct GlyphRef {
    pub glyph: u32,
    pub advance: i32,
}

/// The display list: ops in paint order + the flat glyph-run pool they
/// index. Flat storage keeps the in-process form allocation-light and is
/// already the shape the H-6 wire form serializes.
pub struct Cartoon {
    pub ops: Vec<Op>,
    pub runs: Vec<GlyphRef>,
}

impl Default for Cartoon {
    fn default() -> Cartoon {
        Cartoon::new()
    }
}

impl Cartoon {
    pub fn new() -> Cartoon {
        Cartoon { ops: Vec::new(), runs: Vec::new() }
    }

    /// Reset for the next frame, keeping both allocations.
    pub fn reset(&mut self) {
        self.ops.clear();
        self.runs.clear();
    }

    /// Append a glyph run and its op in one step. Returns the run's start
    /// index (useful to the author's damage bookkeeping).
    pub fn push_glyphs(
        &mut self,
        atlas_gen: u32,
        baseline_x: i32,
        baseline_y: i32,
        color: u32,
        glyphs: &[GlyphRef],
    ) -> u32 {
        let start = self.runs.len() as u32;
        self.runs.extend_from_slice(glyphs);
        self.ops.push(Op::Glyphs {
            atlas_gen,
            baseline_x,
            baseline_y,
            color,
            start,
            count: glyphs.len() as u32,
        });
        start
    }
}

// ---------------------------------------------------------------------------
// The atlas store: alpha pages + the glyph table.

/// One 8-bit-alpha page. Rows are `w`-tight.
pub struct AtlasPage {
    pub w: u32,
    pub h: u32,
    pub alpha: Vec<u8>,
}

/// Where one rasterized glyph lives and how it hangs on the pen: blit rect
/// on `page`, then `left`/`top` are the bearing -- the blit's top-left is
/// `(pen_x + left, baseline_y - top)` (the classic FreeType convention
/// fontdue also reports: `top` is the distance baseline -> bitmap top).
#[derive(Clone, Copy)]
pub struct GlyphEntry {
    pub page: u32,
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
    pub left: i32,
    pub top: i32,
}

/// The store an executor is handed: a generation, its pages, the table.
/// Pages are append-only within a generation; when the packer cannot place
/// a glyph it bumps `gen` and starts over (13.2), so an op authored against
/// gen N never blits from a gen N+1 layout.
pub struct AtlasStore {
    pub gen: u32,
    pub pages: Vec<AtlasPage>,
    pub glyphs: Vec<GlyphEntry>,
}

/// Shelf packer state for one page under construction.
struct Shelf {
    x: u32,
    y: u32,
    h: u32,
}

/// The packer: appends rasterized alpha bitmaps into the store's last page
/// (opening pages/shelves as needed), returning stable glyph ids. Rasterizer-
/// agnostic: the caller (halcyond's fontdue wrapper) hands finished bitmaps.
pub struct AtlasPacker {
    pub store: AtlasStore,
    page_w: u32,
    page_h: u32,
    shelf: Shelf,
}

impl AtlasPacker {
    /// `page_w x page_h` is the page geometry (one page holds many shelves).
    pub fn new(page_w: u32, page_h: u32) -> AtlasPacker {
        AtlasPacker {
            store: AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
            page_w,
            page_h,
            shelf: Shelf { x: 0, y: 0, h: 0 },
        }
    }

    /// Insert one alpha bitmap (`w x h`, rows tight) with its bearing;
    /// returns the glyph id, or None when the bitmap can never fit (larger
    /// than a page). An insert that fills the current page opens a new one
    /// WITHOUT a gen bump (pages are append-only within a gen); `regen()`
    /// is the author's explicit reset for eviction, which is what bumps.
    pub fn insert(&mut self, w: u32, h: u32, alpha: &[u8], left: i32, top: i32) -> Option<u32> {
        if w > self.page_w || h > self.page_h {
            return None;
        }
        debug_assert_eq!(alpha.len(), (w as usize) * (h as usize));
        if self.store.pages.is_empty() {
            self.open_page();
        }
        // Fit on the current shelf, else open a shelf, else a page.
        if self.shelf.x + w > self.page_w {
            let ny = self.shelf.y + self.shelf.h;
            self.shelf = Shelf { x: 0, y: ny, h: 0 };
        }
        if self.shelf.y + h > self.page_h {
            self.open_page();
        }
        let page_idx = (self.store.pages.len() - 1) as u32;
        let (gx, gy) = (self.shelf.x, self.shelf.y);
        {
            let page = self.store.pages.last_mut().unwrap();
            for row in 0..h {
                let src = (row * w) as usize;
                let dst = ((gy + row) * self.page_w + gx) as usize;
                page.alpha[dst..dst + w as usize]
                    .copy_from_slice(&alpha[src..src + w as usize]);
            }
        }
        self.shelf.x += w;
        if h > self.shelf.h {
            self.shelf.h = h;
        }
        let id = self.store.glyphs.len() as u32;
        self.store.glyphs.push(GlyphEntry { page: page_idx, x: gx, y: gy, w, h, left, top });
        Some(id)
    }

    /// Drop every page and glyph and bump the generation: the author's
    /// eviction point (a stylesheet/size change). Ops authored against the
    /// old gen skip harmlessly at execute (the 13.2 stale rule).
    pub fn regen(&mut self) {
        self.store.pages.clear();
        self.store.glyphs.clear();
        self.store.gen += 1;
        self.shelf = Shelf { x: 0, y: 0, h: 0 };
    }

    fn open_page(&mut self) {
        let alpha = alloc::vec![0u8; (self.page_w * self.page_h) as usize];
        self.store.pages.push(AtlasPage { w: self.page_w, h: self.page_h, alpha });
        self.shelf = Shelf { x: 0, y: 0, h: 0 };
    }
}

// ---------------------------------------------------------------------------
// Blobs (decoded rasters for `Op::Image`).

/// A decoded ARGB raster halcyond owns; `argb` rows are `w`-tight.
pub struct Blob {
    pub w: u32,
    pub h: u32,
    pub argb: Vec<u32>,
}

/// The blob table an executor is handed. Ids index it; an out-of-range id
/// skips (fail-safe, like every other malformed reference here).
pub struct BlobStore {
    pub blobs: Vec<Blob>,
}

impl Default for BlobStore {
    fn default() -> BlobStore {
        BlobStore::new()
    }
}

impl BlobStore {
    pub fn new() -> BlobStore {
        BlobStore { blobs: Vec::new() }
    }
}

// ---------------------------------------------------------------------------
// The CPU executor.

/// Integer pixel rect (half-open), the executor's clip currency.
#[derive(Clone, Copy)]
pub struct ClipRect {
    pub x0: i32,
    pub y0: i32,
    pub x1: i32,
    pub y1: i32,
}

/// Alpha-blend fg over bg (a = fg coverage). Copied VERBATIM from aurora's
/// render.rs blend with its hard-won lane-safety lesson: the packed R|B
/// lane trick is only lane-safe with na = 256-a and >>8 -- each 16-bit
/// lane's sum is then <= 255*256 = 0xFF00 and the shift moves whole lanes.
/// A /255 of the PACKED word does NOT distribute over lanes (65536 == 1
/// mod 255): interiors (the a==0/255 short-circuits) stay exact while
/// every antialiased EDGE pixel gets a garbage B correlated with R --
/// measured on real screendumps as wholesale-violet thin glyphs.
#[inline]
pub fn blend(bg: u32, fg: u32, a: u8) -> u32 {
    if a == 0 {
        return bg;
    }
    if a == 255 {
        return fg;
    }
    let a = a as u32;
    let na = 256 - a;
    let rb = (((fg & 0x00FF_00FF) * a + (bg & 0x00FF_00FF) * na) >> 8) & 0x00FF_00FF;
    let g = (((fg & 0x0000_FF00) * a + (bg & 0x0000_FF00) * na) >> 8) & 0x0000_FF00;
    0xFF00_0000 | rb | g
}

/// The execution target: the pixel buffer, its stride, and the effective
/// clip (the surface intersected with the caller's). Bundling these is what
/// keeps every painting helper's signature narrow.
struct Exec<'a> {
    px: &'a mut [u32],
    w: usize,
    clip: ClipRect,
}

impl Exec<'_> {
    /// Intersect an op rect (origin + size) with the effective clip;
    /// half-open pixel bounds (possibly empty).
    #[inline]
    fn isect(&self, x: i32, y: i32, rw: u32, rh: u32) -> (i32, i32, i32, i32) {
        let x1 = x.saturating_add(rw.min(i32::MAX as u32) as i32);
        let y1 = y.saturating_add(rh.min(i32::MAX as u32) as i32);
        (
            x.max(self.clip.x0),
            y.max(self.clip.y0),
            x1.min(self.clip.x1),
            y1.min(self.clip.y1),
        )
    }

    #[inline]
    fn fill(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, color: u32) {
        if x0 >= x1 || y0 >= y1 {
            return;
        }
        for y in y0..y1 {
            let row = y as usize * self.w;
            for p in self.px[row + x0 as usize..row + x1 as usize].iter_mut() {
                *p = color;
            }
        }
    }

    /// Blit one glyph's alpha rect at (dx, dy), blending `color` over dst.
    #[inline]
    fn blit_alpha(&mut self, page: &AtlasPage, ge: &GlyphEntry, dx: i32, dy: i32, color: u32) {
        let (x0, y0, x1, y1) = self.isect(dx, dy, ge.w, ge.h);
        if x0 >= x1 || y0 >= y1 {
            return;
        }
        for y in y0..y1 {
            let srow = ((ge.y + (y - dy) as u32) * page.w + ge.x) as usize;
            let drow = y as usize * self.w;
            for x in x0..x1 {
                let a = page.alpha[srow + (x - dx) as usize];
                let d = &mut self.px[drow + x as usize];
                *d = blend(*d, color, a);
            }
        }
    }
}

/// Execute `cart` into `px` (stride `w` pixels; `px.len()` a multiple of
/// `w`). `clip`, when given, bounds every write (surface-local pixels);
/// painting is fully clamped either way -- no op can write outside `px`.
pub fn execute(
    cart: &Cartoon,
    atlas: &AtlasStore,
    blobs: &BlobStore,
    px: &mut [u32],
    w: usize,
    clip: Option<ClipRect>,
) {
    if w == 0 || px.is_empty() {
        return;
    }
    let h = px.len() / w;
    let mut eff = ClipRect { x0: 0, y0: 0, x1: w as i32, y1: h as i32 };
    if let Some(c) = clip {
        if c.x0 > eff.x0 { eff.x0 = c.x0; }
        if c.y0 > eff.y0 { eff.y0 = c.y0; }
        if c.x1 < eff.x1 { eff.x1 = c.x1; }
        if c.y1 < eff.y1 { eff.y1 = c.y1; }
    }
    if eff.x0 >= eff.x1 || eff.y0 >= eff.y1 {
        return;
    }
    let mut ex = Exec { px, w, clip: eff };

    for op in cart.ops.iter() {
        match *op {
            Op::Clear { color } => {
                let c = ex.clip;
                ex.fill(c.x0, c.y0, c.x1, c.y1, color);
            }
            Op::Rect { x, y, w: rw, h: rh, color } => {
                let (x0, y0, x1, y1) = ex.isect(x, y, rw, rh);
                ex.fill(x0, y0, x1, y1, color);
            }
            Op::Glyphs { atlas_gen, baseline_x, baseline_y, color, start, count } => {
                // The 13.2 stale rule: paint only against the store the
                // ops were authored for. A mismatch skips whole (the
                // author redraws next frame against the new gen).
                if atlas_gen != atlas.gen {
                    continue;
                }
                let s = start as usize;
                let e = s.saturating_add(count as usize);
                let Some(run) = cart.runs.get(s..e.min(cart.runs.len())) else {
                    continue;
                };
                let mut pen = baseline_x;
                for gr in run {
                    let Some(ge) = atlas.glyphs.get(gr.glyph as usize) else {
                        pen += gr.advance;
                        continue;
                    };
                    let Some(page) = atlas.pages.get(ge.page as usize) else {
                        pen += gr.advance;
                        continue;
                    };
                    ex.blit_alpha(page, ge, pen + ge.left, baseline_y - ge.top, color);
                    pen += gr.advance;
                }
            }
            Op::Image { blob_id, x, y, w: iw, h: ih, .. } => {
                let Some(b) = blobs.blobs.get(blob_id as usize) else {
                    continue;
                };
                // v0 paints at native size; the op's w/h are the flow
                // reservation (scaling is not in the executor's v0
                // vocabulary -- the author downscales at decode).
                let pw = b.w.min(iw);
                let ph = b.h.min(ih);
                let (x0, y0, x1, y1) = ex.isect(x, y, pw, ph);
                for py in y0..y1 {
                    let srow = ((py - y) as u32 * b.w) as usize;
                    let drow = py as usize * ex.w;
                    for pxx in x0..x1 {
                        let s = b.argb[srow + (pxx - x) as usize];
                        let a = (s >> 24) as u8;
                        let d = &mut ex.px[drow + pxx as usize];
                        *d = blend(*d, s | 0xFF00_0000, a);
                    }
                }
            }
            Op::Embed { .. } => {
                // Paints nothing (v0): flow reservation only; the
                // compositor places the actual surface.
            }
        }
    }
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    const BG: u32 = 0xFF10_2030;
    const RED: u32 = 0xFFFF_0000;

    fn surface(w: usize, h: usize) -> Vec<u32> {
        vec![BG; w * h]
    }

    #[test]
    fn clear_paints_the_ground() {
        let mut px = surface(4, 3);
        let mut c = Cartoon::new();
        c.ops.push(Op::Clear { color: RED });
        execute(&c, &AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
                &BlobStore::new(), &mut px, 4, None);
        assert!(px.iter().all(|&p| p == RED));
    }

    #[test]
    fn rect_clips_at_every_edge() {
        let mut px = surface(4, 4);
        let mut c = Cartoon::new();
        // Straddles the top-left corner: only the inside quarter paints.
        c.ops.push(Op::Rect { x: -2, y: -2, w: 4, h: 4, color: RED });
        // Fully below the surface: nothing.
        c.ops.push(Op::Rect { x: 0, y: 10, w: 2, h: 2, color: 0xFF00_FF00 });
        execute(&c, &AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
                &BlobStore::new(), &mut px, 4, None);
        for y in 0..4usize {
            for x in 0..4usize {
                let want = if x < 2 && y < 2 { RED } else { BG };
                assert_eq!(px[y * 4 + x], want, "({},{})", x, y);
            }
        }
    }

    #[test]
    fn caller_clip_bounds_every_op() {
        let mut px = surface(4, 4);
        let mut c = Cartoon::new();
        c.ops.push(Op::Clear { color: RED });
        execute(&c, &AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
                &BlobStore::new(), &mut px, 4,
                Some(ClipRect { x0: 1, y0: 1, x1: 3, y1: 3 }));
        for y in 0..4usize {
            for x in 0..4usize {
                let inside = (1..3).contains(&x) && (1..3).contains(&y);
                assert_eq!(px[y * 4 + x], if inside { RED } else { BG });
            }
        }
    }

    // The packer + a real blit: a 2x2 glyph with the three interesting
    // alphas. Expected values are HAND-DERIVED from the blend formula
    // (na = 256-a, >>8), not recomputed through the code under test:
    //   a=255 -> fg exactly; a=0 -> bg exactly;
    //   a=127 over bg 0xFF102030 with fg 0xFFFFFFFF:
    //     R: (0xFF*127 + 0x10*129) >> 8 = (32385+2064)>>8 = 134 = 0x86
    //     G: (0xFF*127 + 0x20*129) >> 8 = (32385+4128)>>8 = 142 = 0x8E
    //     B: (0xFF*127 + 0x30*129) >> 8 = (32385+6192)>>8 = 150 = 0x96
    #[test]
    fn glyph_blit_blends_by_hand_derived_values() {
        let mut p = AtlasPacker::new(8, 8);
        let id = p.insert(2, 2, &[255, 0, 127, 255], 0, 2).unwrap();
        let mut px = surface(4, 4);
        let mut c = Cartoon::new();
        c.push_glyphs(0, 1, 3, 0xFFFF_FFFF, &[GlyphRef { glyph: id, advance: 3 }]);
        execute(&c, &p.store, &BlobStore::new(), &mut px, 4, None);
        // top = 2: the blit's top-left is (1+left=1, 3-2=1).
        assert_eq!(px[1 * 4 + 1], 0xFFFF_FFFF, "a=255");
        assert_eq!(px[1 * 4 + 2], BG, "a=0");
        assert_eq!(px[2 * 4 + 1], 0xFF86_8E96, "a=127 hand-derived");
        assert_eq!(px[2 * 4 + 2], 0xFFFF_FFFF, "a=255");
    }

    #[test]
    fn glyph_run_advances_the_pen() {
        let mut p = AtlasPacker::new(8, 8);
        let id = p.insert(1, 1, &[255], 0, 1).unwrap();
        let mut px = surface(6, 2);
        let mut c = Cartoon::new();
        c.push_glyphs(0, 0, 1, RED, &[
            GlyphRef { glyph: id, advance: 2 },
            GlyphRef { glyph: id, advance: 2 },
        ]);
        execute(&c, &p.store, &BlobStore::new(), &mut px, 6, None);
        assert_eq!(px[0], RED, "first glyph at pen 0");
        assert_eq!(px[1], BG);
        assert_eq!(px[2], RED, "second at pen 2");
    }

    #[test]
    fn stale_atlas_gen_skips_whole_op() {
        let mut p = AtlasPacker::new(8, 8);
        let id = p.insert(1, 1, &[255], 0, 1).unwrap();
        p.regen(); // gen 0 -> 1; the table is gone
        let mut px = surface(2, 2);
        let mut c = Cartoon::new();
        c.push_glyphs(0, 0, 1, RED, &[GlyphRef { glyph: id, advance: 1 }]);
        execute(&c, &p.store, &BlobStore::new(), &mut px, 2, None);
        assert!(px.iter().all(|&p| p == BG), "gen-0 op against a gen-1 store paints nothing");
    }

    #[test]
    fn malformed_ids_skip_without_panic() {
        let mut px = surface(2, 2);
        let mut c = Cartoon::new();
        // Out-of-range glyph id, blob id, and a run window past the pool.
        c.push_glyphs(0, 0, 1, RED, &[GlyphRef { glyph: 99, advance: 1 }]);
        c.ops.push(Op::Glyphs { atlas_gen: 0, baseline_x: 0, baseline_y: 1,
                                color: RED, start: 50, count: 9 });
        c.ops.push(Op::Image { blob_id: 7, x: 0, y: 0, w: 2, h: 2 });
        execute(&c, &AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
                &BlobStore::new(), &mut px, 2, None);
        assert!(px.iter().all(|&p| p == BG));
    }

    #[test]
    fn image_composites_src_over() {
        let mut px = surface(2, 1);
        let mut blobs = BlobStore::new();
        blobs.blobs.push(Blob { w: 2, h: 1, argb: vec![0xFFFF_FFFF, 0x00FF_FFFF] });
        let mut c = Cartoon::new();
        c.ops.push(Op::Image { blob_id: 0, x: 0, y: 0, w: 2, h: 1 });
        execute(&c, &AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
                &blobs, &mut px, 2, None);
        assert_eq!(px[0], 0xFFFF_FFFF, "opaque src replaces");
        assert_eq!(px[1], BG, "a=0 src leaves dst");
    }

    #[test]
    fn embed_paints_nothing() {
        let mut px = surface(2, 2);
        let mut c = Cartoon::new();
        c.ops.push(Op::Embed { surface_ref: 3, x: 0, y: 0, w: 2, h: 2 });
        execute(&c, &AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
                &BlobStore::new(), &mut px, 2, None);
        assert!(px.iter().all(|&p| p == BG));
    }

    #[test]
    fn packer_opens_shelves_and_pages() {
        let mut p = AtlasPacker::new(4, 4);
        // Three 2x2 glyphs: two fill shelf 0, the third opens shelf 1.
        let a = p.insert(2, 2, &[1, 2, 3, 4], 0, 0).unwrap();
        let b = p.insert(2, 2, &[5, 6, 7, 8], 0, 0).unwrap();
        let c3 = p.insert(2, 2, &[9, 10, 11, 12], 0, 0).unwrap();
        assert_eq!(p.store.pages.len(), 1);
        let (ga, gb, gc) = (p.store.glyphs[a as usize], p.store.glyphs[b as usize], p.store.glyphs[c3 as usize]);
        assert_eq!((ga.x, ga.y), (0, 0));
        assert_eq!((gb.x, gb.y), (2, 0));
        assert_eq!((gc.x, gc.y), (0, 2), "shelf 1");
        // A fourth 4x4 cannot fit page 0 -> a new page, same gen.
        let d = p.insert(4, 4, &[0u8; 16], 0, 0).unwrap();
        assert_eq!(p.store.pages.len(), 2);
        assert_eq!(p.store.glyphs[d as usize].page, 1);
        assert_eq!(p.store.gen, 0, "page growth never bumps the gen");
        // Oversize can never fit.
        assert!(p.insert(5, 1, &[0u8; 5], 0, 0).is_none());
        // The page-0 bytes landed where the entries say.
        let pg = &p.store.pages[0];
        assert_eq!(pg.alpha[(ga.y * pg.w + ga.x) as usize], 1);
        assert_eq!(pg.alpha[(gb.y * pg.w + gb.x) as usize], 5);
        assert_eq!(pg.alpha[(gc.y * pg.w + gc.x) as usize], 9);
    }
}
