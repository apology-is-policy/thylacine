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
///
/// It stays 0 while the op set grows, and that is not an oversight: a
/// version discriminates SERIALIZED streams, and no encoder exists yet
/// (there is no `encode`/`decode` in the tree; every consumer is an
/// in-process painter). So no v0 stream can exist that predates a variant,
/// and there is nothing for a bump to tell apart. The first encoder to ship
/// freezes this number; growth after that bumps it.
pub const CARTOON_V0: u32 = 0;

/// The blur radius cap, in surface pixels — the executor's hard bound,
/// applied on the READ side. Shared by BOTH blur ops (`Op::Glow`'s coverage
/// blur and `Op::Blur`'s destination blur): one section-10 derivation, so
/// one constant rather than two to keep in step.
///
/// Section 10 of HALCYON-INSTRUMENT bounds the radius at 16 "at 100 %,
/// scaled", and the display scale tops out at 200 % (`libhalcyon::scale`'s
/// `SCALE_MAX`), so an author scaling the limit lands at 32. cartoon carries
/// zero dependencies, so that derivation lives here as a number rather than
/// as an import; the test asserts the 32 ABSOLUTELY, because a bound
/// asserted in terms of its own constant moves when the constant does.
pub const GLOW_RADIUS_MAX: u32 = 32;

/// One drawing op. Coordinates are surface-local pixels, signed because
/// scrolled content legitimately starts above/left of the viewport; every
/// write is clipped at execute time.
pub enum Op {
    /// Whole-surface ground (under a clip: the clip rect).
    Clear { color: u32 },
    /// Filled rectangle: fills, rules, selection bands, strip segments.
    Rect { x: i32, y: i32, w: u32, h: u32, color: u32 },
    /// Translucent fill: `color` over the destination at `alpha` coverage,
    /// through the same sRGB `blend` the glyph blit uses. Clipped and
    /// bounded exactly like `Rect`; `alpha` 255 is `Rect` and 0 paints
    /// nothing. The §7.3 overlays over client bodies and the flat effects
    /// of §10.
    RectAlpha { x: i32, y: i32, w: u32, h: u32, color: u32, alpha: u8 },
    /// A soft glow: `color` at `alpha`, masked by the rect's coverage run
    /// through a separable box blur of `radius` (§10's shadows and glows).
    ///
    /// The mask is the rect's indicator function, which is itself separable,
    /// so the blurred coverage is exactly the product of a horizontal and a
    /// vertical 1-D box blur — no mask buffer is allocated and the result is
    /// exact rather than approximate. Paint reaches `radius` pixels beyond
    /// the rect on every side, and that spread is the work bound: `radius`
    /// is clamped to `GLOW_RADIUS_MAX` HERE, in the executor, not merely
    /// where the op is built, because an executor consumes a display list it
    /// did not author.
    ///
    /// The radius is a BOX-blur radius, not a CSS `blur()` length: an author
    /// mapping §10's literal blur values (10, 55, 80) picks the radius, and
    /// the cap genuinely truncates the two large shadows.
    Glow { x: i32, y: i32, w: u32, h: u32, color: u32, alpha: u8, radius: u32 },
    /// Box-blur the DESTINATION in place over the rect (§10's modal
    /// backdrop), bounded and clipped exactly like `Rect`.
    ///
    /// The one op that READS the surface it paints into, and that is why it
    /// exists: `Glow` blurs a rect's coverage MASK — which is what a drop
    /// shadow is — and cannot express a blur of whatever happens to lie
    /// underneath. A second blur implementation inside the compositor was
    /// the alternative, and it is the shape that produced three
    /// HALCYON-WORKSPACES defects.
    ///
    /// Separable, so two 1-D passes rather than a (2r+1)^2 kernel. Taps
    /// falling outside the rect are not sampled and the divisor is the tap
    /// count actually taken, so the edge neither darkens toward black nor
    /// drags in the un-blurred scene — a constant field is preserved
    /// EXACTLY, edges included.
    ///
    /// §10 says "downsampled"; this is a direct box blur. Downsampling is a
    /// large-radius GPU optimisation, and at the backdrop's 3 px (6 at the
    /// 200 % scale ceiling) a direct blur is both cheaper and exact. The
    /// specified appearance is the blur, not the means.
    Blur { x: i32, y: i32, w: u32, h: u32, radius: u32 },
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
    /// The image blobs `Op::Image` ops index (I-47 inline media). Per-cartoon
    /// (per-frame) resources, unlike the persistent `AtlasStore`: the author
    /// pushes a decoded/resampled raster with `add_blob` and emits an
    /// `Op::Image { blob_id }` naming it. Bundling them with the ops keeps the
    /// display list self-contained -- `execute` reads `cart.blobs`.
    pub blobs: BlobStore,
}

impl Default for Cartoon {
    fn default() -> Cartoon {
        Cartoon::new()
    }
}

impl Cartoon {
    pub fn new() -> Cartoon {
        Cartoon { ops: Vec::new(), runs: Vec::new(), blobs: BlobStore::new() }
    }

    /// Reset for the next frame, keeping every allocation.
    pub fn reset(&mut self) {
        self.ops.clear();
        self.runs.clear();
        self.blobs.blobs.clear();
    }

    /// Append an image blob and return its id (its index in `blobs`), for an
    /// `Op::Image { blob_id, .. }` to name.
    pub fn add_blob(&mut self, b: Blob) -> u32 {
        let id = self.blobs.blobs.len() as u32;
        self.blobs.blobs.push(b);
        id
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
/// `(pen_x + left, baseline_y - top)` (the classic FreeType convention:
/// `top` is the distance baseline -> bitmap top, y-up).
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
/// agnostic: the caller (halcyond's glyph source) hands finished bitmaps.
pub struct AtlasPacker {
    pub store: AtlasStore,
    page_w: u32,
    page_h: u32,
    shelf: Shelf,
    /// The hard page cap (0 = unbounded): an insert that would open a page
    /// past it is REFUSED rather than grown into. The author's between-frames
    /// eviction reclaims; this is the in-frame bound that eviction cannot
    /// be, because the untrusted stream decides how many distinct glyphs
    /// one frame paints.
    max_pages: u32,
}

impl AtlasPacker {
    /// `page_w x page_h` is the page geometry (one page holds many shelves).
    pub fn new(page_w: u32, page_h: u32) -> AtlasPacker {
        AtlasPacker {
            store: AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() },
            page_w,
            page_h,
            shelf: Shelf { x: 0, y: 0, h: 0 },
            max_pages: 0,
        }
    }

    /// Bound the store at `n` pages (0 = unbounded): past it `insert`
    /// returns None instead of opening a page.
    pub fn set_max_pages(&mut self, n: u32) {
        self.max_pages = n;
    }

    /// The page geometry this packer opens pages at.
    pub fn page_w(&self) -> u32 {
        self.page_w
    }

    pub fn page_h(&self) -> u32 {
        self.page_h
    }

    /// Insert one alpha bitmap (`w x h`, rows tight) with its bearing;
    /// returns the glyph id, or None when the bitmap can never fit (larger
    /// than a page) or the store is at its page cap. An insert that fills
    /// the current page opens a new one WITHOUT a gen bump (pages are
    /// append-only within a gen); `regen()` is the author's explicit reset
    /// for eviction, which is what bumps.
    pub fn insert(&mut self, w: u32, h: u32, alpha: &[u8], left: i32, top: i32) -> Option<u32> {
        if w > self.page_w || h > self.page_h {
            return None;
        }
        debug_assert_eq!(alpha.len(), (w as usize) * (h as usize));
        if self.store.pages.is_empty() && !self.open_page() {
            return None;
        }
        // Fit on the current shelf, else open a shelf, else a page.
        if self.shelf.x + w > self.page_w {
            let ny = self.shelf.y + self.shelf.h;
            self.shelf = Shelf { x: 0, y: ny, h: 0 };
        }
        if self.shelf.y + h > self.page_h && !self.open_page() {
            return None;
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

    /// Open a page; false (and nothing opened) at the page cap.
    fn open_page(&mut self) -> bool {
        if self.max_pages != 0 && self.store.pages.len() >= self.max_pages as usize {
            return false;
        }
        let alpha = alloc::vec![0u8; (self.page_w * self.page_h) as usize];
        self.store.pages.push(AtlasPage { w: self.page_w, h: self.page_h, alpha });
        self.shelf = Shelf { x: 0, y: 0, h: 0 };
        true
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

impl Blob {
    /// Nearest-neighbor resample to `dw x dh`. The v0 letterbox scaler: the
    /// author (halcyond's layout) scales a source raster to the reserved rect
    /// so the executor stays a 1:1 blitter (`Op::Image` paints native size --
    /// lib header + the H-7/I-47 note). Bilinear is a later refinement; an
    /// empty/zero request yields an empty blob (fail-safe, like every other
    /// malformed reference here).
    pub fn scaled(&self, dw: u32, dh: u32) -> Blob {
        if dw == 0 || dh == 0 || self.w == 0 || self.h == 0 {
            return Blob { w: 0, h: 0, argb: Vec::new() };
        }
        if dw == self.w && dh == self.h {
            return Blob { w: self.w, h: self.h, argb: self.argb.clone() };
        }
        let mut argb = Vec::with_capacity((dw as usize) * (dh as usize));
        for y in 0..dh {
            let sy = ((y as u64 * self.h as u64) / dh as u64) as u32;
            let srow = (sy * self.w) as usize;
            for x in 0..dw {
                let sx = ((x as u64 * self.w as u64) / dw as u64) as u32;
                argb.push(self.argb[srow + sx as usize]);
            }
        }
        Blob { w: dw, h: dh, argb }
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

    /// Fill an already-clipped rect with `color` at `alpha` coverage.
    #[inline]
    fn fill_alpha(&mut self, x0: i32, y0: i32, x1: i32, y1: i32, color: u32, alpha: u8) {
        if x0 >= x1 || y0 >= y1 || alpha == 0 {
            return;
        }
        for y in y0..y1 {
            let row = y as usize * self.w;
            for p in self.px[row + x0 as usize..row + x1 as usize].iter_mut() {
                *p = blend(*p, color, alpha);
            }
        }
    }

    /// Paint `color` at `alpha` under the box-blurred coverage of the rect
    /// `(rx, ry, rw, rh)`, spread by `radius` (clamped to `GLOW_RADIUS_MAX`
    /// here -- the executor is handed a list it did not author).
    ///
    /// The rect's mask is separable, and so is a box blur, so the blurred
    /// coverage at a pixel is exactly the product of the two 1-D window
    /// overlaps: no mask buffer, no approximation, two O(1) counts per
    /// pixel. Rect bounds are taken in i64 because `rw`/`rh` are u32 and an
    /// author may hand over anything.
    fn glow(&mut self, rx: i32, ry: i32, rw: u32, rh: u32, color: u32, alpha: u8, radius: u32) {
        if alpha == 0 || rw == 0 || rh == 0 {
            return;
        }
        let r = radius.min(GLOW_RADIUS_MAX);
        let (gx, gy) = (rx.saturating_sub(r as i32), ry.saturating_sub(r as i32));
        let (gw, gh) = (rw.saturating_add(r * 2), rh.saturating_add(r * 2));
        let (x0, y0, x1, y1) = self.isect(gx, gy, gw, gh);
        if x0 >= x1 || y0 >= y1 {
            return;
        }
        let (mx0, my0) = (rx as i64, ry as i64);
        let (mx1, my1) = (mx0 + rw as i64, my0 + rh as i64);
        let n = 2 * r as i64 + 1;
        let denom = (n * n) as u64;
        // Taps of the window [p-r, p+r] landing inside [lo, hi); <= n.
        let cov = |p: i32, lo: i64, hi: i64| -> u32 {
            let a = (p as i64 - r as i64).max(lo);
            let b = (p as i64 + r as i64).min(hi - 1);
            if b < a { 0 } else { (b - a + 1) as u32 }
        };
        for y in y0..y1 {
            let vcov = cov(y, my0, my1);
            if vcov == 0 {
                continue;
            }
            let row = y as usize * self.w;
            for x in x0..x1 {
                let hcov = cov(x, mx0, mx1);
                if hcov == 0 {
                    continue;
                }
                // u64 so the arithmetic is sound INDEPENDENTLY of the
                // clamp: in u32 this overflows once n passes ~4100
                // (255*n*n), which would quietly make the radius bound
                // carry a second, unrelated obligation. Two guards, one
                // job each. The quotient is <= 255*n*n/n^2 == 255, so the
                // u8 is exact.
                let a = (alpha as u64 * hcov as u64 * vcov as u64) / denom;
                let d = &mut self.px[row + x as usize];
                *d = blend(*d, color, a as u8);
            }
        }
    }

    /// Box-blur the destination over the rect, in place: a horizontal pass
    /// across every row, then a vertical pass down every column.
    fn blur(&mut self, rx: i32, ry: i32, rw: u32, rh: u32, radius: u32) {
        let r = radius as usize;
        if r == 0 || rw == 0 || rh == 0 {
            return;
        }
        let (x0, y0, x1, y1) = self.isect(rx, ry, rw, rh);
        if x0 >= x1 || y0 >= y1 {
            return;
        }
        let (bw, bh) = ((x1 - x0) as usize, (y1 - y0) as usize);
        let stride = self.w;
        for y in y0..y1 {
            blur_line(self.px, y as usize * stride + x0 as usize, 1, bw, r);
        }
        for x in x0..x1 {
            blur_line(self.px, y0 as usize * stride + x as usize, stride, bh, r);
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

/// Box-blur ONE line of `n` pixels starting at `base`, every `stride`
/// apart, in place — the arithmetic-sequence form serves both of `blur`'s
/// passes (stride 1 across a row, stride `w` down a column).
///
/// ALLOCATION-FREE, deliberately. A separable in-place blur normally wants
/// a scratch of the region's area (megabytes for a full-display region),
/// but only the `r + 1` values already OVERWRITTEN need keeping —
/// everything at or ahead of the cursor is still original in `px` — so a
/// fixed ring of `GLOW_RADIUS_MAX + 1` entries serves any permitted radius.
/// cartoon is `no_std`, where a failed allocation aborts, and an executor
/// whose contract is "always produces a validly-clamped frame" must not be
/// able to fail.
///
/// A RUNNING window, so a pixel costs the same at any radius: the sums gain
/// the value entering at `i + r` (still original: it is ahead of the
/// cursor) and lose the one leaving at `i - 1 - r` (already overwritten, so
/// it comes from the ring). Summing every tap per pixel cost 4-6x at the
/// modal backdrop's full-display extent (a 2560x1664 field on the host at
/// the release profile: 144 ms against 32 ms at radius 6, 182 against 29 at
/// radius 8), and the output is identical — the window, its clipped edges
/// and the floor division are the same.
///
/// The leaving value sits in slot `(i - 1 - r) % (r + 1)`, which is `i %
/// (r + 1)`: exactly the slot step `i` is about to overwrite. So it is read
/// BEFORE the write, and no slot is clobbered under a reader that still
/// needs it (a window at `i` reaches back only to `i - r`).
///
/// Channels are summed independently in u32: at most `2 * GLOW_RADIUS_MAX
/// + 1` == 65 taps of one byte each is 16575 per channel, nowhere near
/// overflow, and every subtraction removes a value the same sum gained.
fn blur_line(px: &mut [u32], base: usize, stride: usize, n: usize, r: usize) {
    // The cap is applied HERE, the one place it is structurally REQUIRED:
    // `keep` is sized from it, so a larger radius would index past the ring
    // -- the bound is memory safety, not cosmetics. One guard, at the site
    // that needs it; clamping in the caller as well would leave neither
    // testable, since a sabotage of either would be masked by the other.
    let r = r.min(GLOW_RADIUS_MAX as usize);
    if n == 0 || r == 0 {
        return;
    }
    let m = r + 1;
    let mut keep = [0u32; GLOW_RADIUS_MAX as usize + 1];
    let (mut sa, mut sr, mut sg, mut sb) = (0u32, 0u32, 0u32, 0u32);
    for k in 0..=r.min(n - 1) {
        let p = px[base + k * stride];
        sa += p >> 24;
        sr += (p >> 16) & 0xFF;
        sg += (p >> 8) & 0xFF;
        sb += p & 0xFF;
    }
    for i in 0..n {
        if i > r {
            let p = keep[i % m];
            sa -= p >> 24;
            sr -= (p >> 16) & 0xFF;
            sg -= (p >> 8) & 0xFF;
            sb -= p & 0xFF;
        }
        if i > 0 && i + r < n {
            let p = px[base + (i + r) * stride];
            sa += p >> 24;
            sr += (p >> 16) & 0xFF;
            sg += (p >> 8) & 0xFF;
            sb += p & 0xFF;
        }
        keep[i % m] = px[base + i * stride];
        let lo = i.saturating_sub(r);
        let hi = (i + r).min(n - 1);
        let c = (hi - lo + 1) as u32;
        px[base + i * stride] =
            ((sa / c) << 24) | ((sr / c) << 16) | ((sg / c) << 8) | (sb / c);
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
            Op::RectAlpha { x, y, w: rw, h: rh, color, alpha } => {
                let (x0, y0, x1, y1) = ex.isect(x, y, rw, rh);
                ex.fill_alpha(x0, y0, x1, y1, color, alpha);
            }
            Op::Glow { x, y, w: rw, h: rh, color, alpha, radius } => {
                ex.glow(x, y, rw, rh, color, alpha, radius);
            }
            Op::Blur { x, y, w: rw, h: rh, radius } => {
                ex.blur(x, y, rw, rh, radius);
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

    const BLACK: u32 = 0xFF00_0000;
    const WHITE: u32 = 0xFFFF_FFFF;

    fn empty_atlas() -> AtlasStore {
        AtlasStore { gen: 0, pages: Vec::new(), glyphs: Vec::new() }
    }

    #[test]
    fn rect_alpha_blends_by_hand_derived_values() {
        // HAND-DERIVED from the blend formula (na = 256-a, >>8), not
        // recomputed through the code under test. bg 0xFF102030 under fg
        // 0xFFFF0000 at a=127 (na=129):
        //   R: (0xFF*127 + 0x10*129) >> 8 = (32385+2064)>>8 = 134 = 0x86
        //   G: (0x00*127 + 0x20*129) >> 8 =          4128>>8 =  16 = 0x10
        //   B: (0x00*127 + 0x30*129) >> 8 =          6192>>8 =  24 = 0x18
        let mut px = surface(2, 1);
        let mut c = Cartoon::new();
        c.ops.push(Op::RectAlpha { x: 0, y: 0, w: 1, h: 1, color: RED, alpha: 127 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 2, None);
        assert_eq!(px[0], 0xFF86_1018, "a=127 hand-derived");
        assert_eq!(px[1], BG, "outside the rect");
    }

    #[test]
    fn rect_alpha_at_the_extremes_is_a_fill_or_nothing() {
        let mut px = surface(2, 1);
        let mut c = Cartoon::new();
        c.ops.push(Op::RectAlpha { x: 0, y: 0, w: 1, h: 1, color: RED, alpha: 255 });
        c.ops.push(Op::RectAlpha { x: 1, y: 0, w: 1, h: 1, color: RED, alpha: 0 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 2, None);
        assert_eq!(px[0], RED, "a=255 is an opaque fill");
        assert_eq!(px[1], BG, "a=0 paints nothing");
    }

    #[test]
    fn rect_alpha_clips_at_every_edge() {
        // Opaque, so the clip is the only thing under test (the twin of
        // `rect_clips_at_every_edge`).
        let mut px = surface(4, 4);
        let mut c = Cartoon::new();
        c.ops.push(Op::RectAlpha { x: -2, y: -2, w: 4, h: 4, color: RED, alpha: 255 });
        c.ops.push(Op::RectAlpha { x: 0, y: 10, w: 2, h: 2, color: RED, alpha: 255 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 4, None);
        for y in 0..4usize {
            for x in 0..4usize {
                let want = if x < 2 && y < 2 { RED } else { BG };
                assert_eq!(px[y * 4 + x], want, "({},{})", x, y);
            }
        }
    }

    #[test]
    fn glow_coverage_is_the_product_of_two_box_blurs() {
        // A 2x2 rect at (2,2), radius 1: n=3 taps per axis, denom 9.
        // Horizontal coverage by column is 1,2,2,1 across x=1..=4 and zero
        // elsewhere; vertical the same. Effective alpha is 255*hcov*vcov/9,
        // and WHITE over BLACK makes every channel (255*a)>>8 -- so the
        // expected pixels are HAND-DERIVED end to end:
        //   (2,2): a = 255*2*2/9 = 113 -> (255*113)>>8 = 112 = 0x70
        //   (1,2): a = 255*1*2/9 =  56 -> (255*56 )>>8 =  55 = 0x37
        //   (1,1): a = 255*1*1/9 =  28 -> (255*28 )>>8 =  27 = 0x1B
        let mut px = vec![BLACK; 6 * 6];
        let mut c = Cartoon::new();
        c.ops.push(Op::Glow { x: 2, y: 2, w: 2, h: 2, color: WHITE, alpha: 255, radius: 1 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 6, None);
        assert_eq!(px[2 * 6 + 2], 0xFF70_7070, "the interior");
        assert_eq!(px[2 * 6 + 1], 0xFF37_3737, "one column outside");
        assert_eq!(px[1 * 6 + 1], 0xFF1B_1B1B, "the diagonal corner");
        assert_eq!(px[0], BLACK, "past the spread");
        assert_eq!(px[5 * 6 + 5], BLACK, "past the spread");
    }

    #[test]
    fn a_zero_radius_glow_is_a_flat_alpha_fill() {
        // n=1, denom=1: coverage is 1 inside the rect and 0 outside, so the
        // op must be pixel-identical to the RectAlpha of the same rect.
        let mut a = surface(5, 5);
        let mut b = surface(5, 5);
        let mut cg = Cartoon::new();
        cg.ops.push(Op::Glow { x: 1, y: 1, w: 2, h: 3, color: RED, alpha: 96, radius: 0 });
        let mut cr = Cartoon::new();
        cr.ops.push(Op::RectAlpha { x: 1, y: 1, w: 2, h: 3, color: RED, alpha: 96 });
        execute(&cg, &empty_atlas(), &BlobStore::new(), &mut a, 5, None);
        execute(&cr, &empty_atlas(), &BlobStore::new(), &mut b, 5, None);
        assert_ne!(a[1 * 5 + 1], BG, "the control must actually paint");
        assert_eq!(a, b);
    }

    #[test]
    fn the_glow_radius_cap_is_thirty_two() {
        // ABSOLUTE, deliberately: every assertion below is written against
        // the cap, so a bound stated only in terms of GLOW_RADIUS_MAX would
        // move with it and witness nothing. 16 at 100 %, doubled because the
        // display scale tops out at 200 %.
        assert_eq!(GLOW_RADIUS_MAX, 32);
    }

    #[test]
    fn an_oversize_glow_radius_is_clamped_to_the_cap() {
        // The bound holds on the READ side: a list asking for radius 4000
        // paints EXACTLY what 32 paints and reaches no further. The rect
        // starts at x=40, so the cap puts the spread's first painted column
        // at x=8 -- and x=7 must stay untouched. Both halves matter: the
        // equality alone is satisfied by two renders that painted nothing.
        let mk = |radius: u32| {
            let mut px = vec![BLACK; 120 * 120];
            let mut c = Cartoon::new();
            c.ops.push(Op::Glow { x: 40, y: 40, w: 40, h: 40, color: WHITE, alpha: 255, radius });
            execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 120, None);
            px
        };
        let capped = mk(32);
        let asked = mk(4000);
        // The centre is NOT fully covered, and that is correct box-blur
        // behaviour rather than a shortfall: the window is 2r+1 = 65 wide
        // while the rect is 40, so no pixel ever sees 65 taps. At (60,60)
        // the window [28,92] meets [40,80) in 40 taps on each axis, so
        // a = 255*40*40/4225 = 96 and the channel is (255*96)>>8 = 95.
        assert_eq!(capped[60 * 120 + 60], 0xFF5F_5F5F, "the centre, hand-derived");
        assert_eq!(capped[60 * 120 + 7], BLACK, "one column short of the cap's reach");
        assert_ne!(capped[60 * 120 + 8], BLACK, "the cap's reach is painted");
        assert_eq!(asked, capped, "an oversize radius is the cap, not the ask");
    }

    #[test]
    fn glow_is_bounded_by_the_caller_clip() {
        let mut px = vec![BLACK; 8 * 8];
        let mut c = Cartoon::new();
        c.ops.push(Op::Glow { x: 3, y: 3, w: 2, h: 2, color: WHITE, alpha: 255, radius: 3 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 8,
                Some(ClipRect { x0: 3, y0: 3, x1: 5, y1: 5 }));
        for y in 0..8usize {
            for x in 0..8usize {
                let inside = (3..5).contains(&x) && (3..5).contains(&y);
                if inside {
                    assert_ne!(px[y * 8 + x], BLACK, "({},{}) inside the clip", x, y);
                } else {
                    assert_eq!(px[y * 8 + x], BLACK, "({},{}) outside the clip", x, y);
                }
            }
        }
    }

    #[test]
    fn extreme_glow_geometry_cannot_panic() {
        // The expansion saturates and the overlap counts are i64, so an
        // author's i32::MIN origin / u32::MAX extent clips instead of
        // wrapping. A degenerate rect paints nothing at all.
        let mut px = surface(4, 4);
        let mut c = Cartoon::new();
        c.ops.push(Op::Glow { x: i32::MIN, y: i32::MIN, w: u32::MAX, h: u32::MAX,
                              color: RED, alpha: 255, radius: 32 });
        c.ops.push(Op::Glow { x: 0, y: 0, w: 0, h: 4, color: RED, alpha: 255, radius: 4 });
        c.ops.push(Op::Glow { x: 0, y: 0, w: 4, h: 4, color: RED, alpha: 0, radius: 4 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 4, None);
        assert!(px.iter().all(|&p| p == BG), "nothing painted");
        // A huge extent anchored inside the surface still paints, clipped.
        let mut px2 = surface(4, 4);
        let mut c2 = Cartoon::new();
        c2.ops.push(Op::Glow { x: 0, y: 0, w: u32::MAX, h: u32::MAX,
                               color: RED, alpha: 255, radius: 32 });
        execute(&c2, &empty_atlas(), &BlobStore::new(), &mut px2, 4, None);
        assert!(px2.iter().all(|&p| p != BG), "the whole surface is covered");
    }

    #[test]
    fn a_blur_averages_its_neighbourhood() {
        // HAND-DERIVED end to end, one lone WHITE pixel at (2,2) on BLACK,
        // radius 1. The horizontal pass rewrites row 2 alone -- windows of
        // 3 taps at the interior, 2 at the ends:
        //   i=1,2,3: (0+0+255)/3 = 85 = 0x55;  i=0,4: 0
        // The vertical pass then rewrites columns 1..3 from [0,0,85,0,0]:
        //   j=1,2,3: (0+0+85)/3 = 28 = 0x1C;   j=0,4: 0
        // so the result is a 3x3 block of 0x1C centred on (2,2). Alpha is
        // 255 in every tap, so it averages to 255 and stays opaque.
        let mut px = vec![BLACK; 5 * 5];
        px[2 * 5 + 2] = WHITE;
        let mut c = Cartoon::new();
        c.ops.push(Op::Blur { x: 0, y: 0, w: 5, h: 5, radius: 1 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 5, None);
        for y in 0..5usize {
            for x in 0..5usize {
                let inside = (1..4).contains(&x) && (1..4).contains(&y);
                let want = if inside { 0xFF1C_1C1C } else { BLACK };
                assert_eq!(px[y * 5 + x], want, "at ({}, {})", x, y);
            }
        }
    }

    #[test]
    fn a_zero_radius_blur_is_identity() {
        // The negative alone is satisfied by a blur that never runs, so the
        // control sits one variable away: the SAME field at radius 1 must
        // actually move.
        let mk = |radius: u32| {
            let mut px = vec![BLACK; 5 * 5];
            px[2 * 5 + 2] = WHITE;
            let mut c = Cartoon::new();
            c.ops.push(Op::Blur { x: 0, y: 0, w: 5, h: 5, radius });
            execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 5, None);
            px
        };
        let mut original = vec![BLACK; 5 * 5];
        original[2 * 5 + 2] = WHITE;
        assert_eq!(mk(0), original, "radius 0 is a window of one");
        assert_ne!(mk(1), original, "the control: radius 1 must move it");
    }

    #[test]
    fn a_constant_field_survives_the_blur_exactly() {
        // The divisor is the tap count ACTUALLY TAKEN, so a window hanging
        // off the edge averages fewer taps rather than averaging in black.
        // A constant field is therefore preserved EXACTLY, edges and corners
        // included -- the property that keeps the backdrop from ringing
        // darker around its own border. Control: a two-tone field at the
        // same radius must move, or this passes on a blur that does nothing.
        let run = |seed: &dyn Fn(usize) -> u32| {
            let mut px: alloc::vec::Vec<u32> = (0..7 * 7).map(|i| seed(i)).collect();
            let mut c = Cartoon::new();
            c.ops.push(Op::Blur { x: 0, y: 0, w: 7, h: 7, radius: 3 });
            execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 7, None);
            px
        };
        let flat = run(&|_| 0xFF20_3040);
        assert!(flat.iter().all(|&p| p == 0xFF20_3040), "constant field, edges included");
        let two_tone = run(&|i| if i % 7 < 3 { BLACK } else { WHITE });
        let before: alloc::vec::Vec<u32> =
            (0..7 * 7).map(|i| if i % 7 < 3 { BLACK } else { WHITE }).collect();
        assert_ne!(two_tone, before, "the control: a non-constant field must move");
    }

    #[test]
    fn blur_is_bounded_by_the_caller_clip() {
        let before: alloc::vec::Vec<u32> =
            (0..8 * 8).map(|i| if i % 3 == 0 { WHITE } else { BLACK }).collect();
        let mut px = before.clone();
        let mut c = Cartoon::new();
        c.ops.push(Op::Blur { x: 0, y: 0, w: 8, h: 8, radius: 2 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 8,
                Some(ClipRect { x0: 3, y0: 3, x1: 6, y1: 6 }));
        let mut moved = false;
        for y in 0..8usize {
            for x in 0..8usize {
                if (3..6).contains(&x) && (3..6).contains(&y) {
                    moved |= px[y * 8 + x] != before[y * 8 + x];
                } else {
                    assert_eq!(px[y * 8 + x], before[y * 8 + x],
                               "({}, {}) is outside the clip", x, y);
                }
            }
        }
        assert!(moved, "the clip's interior must actually blur");
    }

    #[test]
    fn an_oversize_blur_radius_is_clamped_to_the_cap() {
        // The bound holds on the READ side, exactly as the glow's does: a
        // list asking for 4000 paints what 32 paints. The inequality against
        // the original is the half that matters -- equality alone is
        // satisfied by two renders that both did nothing.
        let mk = |radius: u32| {
            let mut px = vec![BLACK; 120 * 120];
            for y in 40..80usize {
                for x in 40..80usize {
                    px[y * 120 + x] = WHITE;
                }
            }
            let mut c = Cartoon::new();
            c.ops.push(Op::Blur { x: 0, y: 0, w: 120, h: 120, radius });
            execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 120, None);
            px
        };
        let capped = mk(32);
        assert_ne!(capped[60 * 120 + 60], WHITE, "the cap must actually blur");
        assert_eq!(mk(4000), capped, "an oversize radius is the cap, not the ask");
    }

    #[test]
    fn extreme_blur_geometry_cannot_panic() {
        // Saturating expansion and a clipped rect, so an author's i32::MIN
        // origin / u32::MAX extent clips instead of wrapping; a degenerate
        // rect and a zero radius paint nothing at all.
        let mut px = surface(4, 4);
        let before = px.clone();
        let mut c = Cartoon::new();
        c.ops.push(Op::Blur { x: 0, y: 0, w: 0, h: 4, radius: 4 });
        c.ops.push(Op::Blur { x: 0, y: 0, w: 4, h: 4, radius: 0 });
        execute(&c, &empty_atlas(), &BlobStore::new(), &mut px, 4, None);
        assert_eq!(px, before, "nothing painted");
        // A huge extent anchored outside the surface still clips cleanly.
        let mut px2 = vec![BLACK; 4 * 4];
        px2[0] = WHITE;
        let mut c2 = Cartoon::new();
        c2.ops.push(Op::Blur { x: i32::MIN, y: i32::MIN, w: u32::MAX, h: u32::MAX, radius: 32 });
        execute(&c2, &empty_atlas(), &BlobStore::new(), &mut px2, 4, None);
    }

    /// The per-tap sum `blur_line` replaced, kept VERBATIM as the oracle:
    /// the running window is only a faster way to compute these values, so
    /// the two must agree on every pixel, not merely look alike.
    fn reference_blur_line(px: &mut [u32], base: usize, stride: usize, n: usize, r: usize) {
        let r = r.min(GLOW_RADIUS_MAX as usize);
        if n == 0 || r == 0 {
            return;
        }
        let m = r + 1;
        let mut keep = [0u32; GLOW_RADIUS_MAX as usize + 1];
        for i in 0..n {
            keep[i % m] = px[base + i * stride];
            let lo = i.saturating_sub(r);
            let hi = (i + r).min(n - 1);
            let (mut sa, mut sr, mut sg, mut sb) = (0u32, 0u32, 0u32, 0u32);
            for k in lo..=hi {
                let p = if k < i { keep[k % m] } else { px[base + k * stride] };
                sa += p >> 24;
                sr += (p >> 16) & 0xFF;
                sg += (p >> 8) & 0xFF;
                sb += p & 0xFF;
            }
            let c = (hi - lo + 1) as u32;
            px[base + i * stride] =
                ((sa / c) << 24) | ((sr / c) << 16) | ((sg / c) << 8) | (sb / c);
        }
    }

    #[test]
    fn the_running_window_matches_the_per_tap_sum_everywhere() {
        // Random fields over every shape class the window has: n below,
        // at and above 2r + 1 (all-edge, one interior pixel, a long
        // interior), radii past the cap, stride > 1 (the column pass) and a
        // non-zero base. A deterministic xorshift, so a failure replays.
        let mut seed = 0x9E37_79B9_7F4A_7C15u64;
        let mut rnd = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let mut cases = 0;
        for n in [0usize, 1, 2, 3, 4, 7, 8, 13, 64, 65, 66, 129] {
            for r in [0usize, 1, 2, 3, 6, 31, 32, 33, 4000] {
                for stride in [1usize, 3] {
                    let base = 5;
                    let len = base + n * stride + 2;
                    let field: alloc::vec::Vec<u32> = (0..len).map(|_| rnd() as u32).collect();
                    let mut want = field.clone();
                    reference_blur_line(&mut want, base, stride, n, r);
                    let mut got = field.clone();
                    blur_line(&mut got, base, stride, n, r);
                    assert_eq!(got, want, "n={} r={} stride={}", n, r, stride);
                    cases += 1;
                }
            }
        }
        assert_eq!(cases, 12 * 9 * 2);
        // The control: the comparison must be able to fail. A field the
        // blur moves differs from its own input.
        let field: alloc::vec::Vec<u32> = (0..40).map(|_| rnd() as u32).collect();
        let mut moved = field.clone();
        blur_line(&mut moved, 0, 1, 40, 2);
        assert_ne!(moved, field, "the blur must move a random field");
    }

    /// `Op::Blur` run under a CLIP that is a target rect grown by the
    /// radius yields, inside the target, exactly the pixels the unclipped
    /// blur yields there -- the property the compositor relies on to blur
    /// only the pixels one upload carries (HALCYON-INSTRUMENT 10, revised
    /// 2026-09-16: effects are laid on at upload, never stored).
    ///
    /// Why it holds: a pixel's window reaches at most `r` either way, and
    /// the vertical pass reads horizontal results no further than `r` above
    /// or below, each of which read no further than `r` across. So every
    /// tap lies in the grown clip, and where the clip meets the field's (or
    /// the op rect's) own edge, both runs clip the window identically.
    ///
    /// Random fields, op rects that overhang the field, targets at every
    /// edge, and radii past the cap -- where the clip must grow by the
    /// CLAMPED radius, since that is how far the executor actually reads.
    #[test]
    fn a_blur_clipped_to_the_grown_target_is_exact_inside_the_target() {
        let (w, h) = (40usize, 30usize);
        let mut seed = 0xD1B5_4A32_D192_ED03u64;
        let mut rnd = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let mut compared = 0u64;
        for case in 0..400u32 {
            let field: alloc::vec::Vec<u32> = (0..w * h).map(|_| rnd() as u32).collect();
            let r = [1u32, 2, 3, 6, 8, 32, 40][(case % 7) as usize];
            let reach = r.min(GLOW_RADIUS_MAX) as i32;
            // The op rect: anywhere, overhanging the field on any side.
            let ox = (rnd() % 50) as i32 - 5;
            let oy = (rnd() % 40) as i32 - 5;
            let ow = (rnd() % 45) as u32 + 1;
            let oh = (rnd() % 35) as u32 + 1;
            // The target, inside the field (an upload never leaves it).
            let tx = (rnd() % w as u64) as i32;
            let ty = (rnd() % h as u64) as i32;
            let tw = (rnd() % (w as u64 - tx as u64)) as i32 + 1;
            let th = (rnd() % (h as u64 - ty as u64)) as i32 + 1;
            let mut c = Cartoon::new();
            c.ops.push(Op::Blur { x: ox, y: oy, w: ow, h: oh, radius: r });
            let mut want = field.clone();
            execute(&c, &AtlasStore { gen: 0, pages: alloc::vec::Vec::new(), glyphs: alloc::vec::Vec::new() }, &BlobStore::new(), &mut want, w, None);
            let clip = ClipRect {
                x0: (tx - reach).max(0),
                y0: (ty - reach).max(0),
                x1: (tx + tw + reach).min(w as i32),
                y1: (ty + th + reach).min(h as i32),
            };
            let mut got = field.clone();
            execute(&c, &AtlasStore { gen: 0, pages: alloc::vec::Vec::new(), glyphs: alloc::vec::Vec::new() }, &BlobStore::new(), &mut got, w, Some(clip));
            for y in ty..ty + th {
                for x in tx..tx + tw {
                    let i = y as usize * w + x as usize;
                    assert_eq!(got[i], want[i], "case {} r {} at ({}, {})", case, r, x, y);
                    compared += 1;
                }
            }
        }
        assert!(compared > 10_000, "the comparison ran: {}", compared);
        // The control: clipped to the target ITSELF, not grown, the edge
        // pixels read a narrower window and differ -- so the equality above
        // is a property of the growth, not of blurring anything at all.
        let field: alloc::vec::Vec<u32> = (0..w * h).map(|_| rnd() as u32).collect();
        let mut c = Cartoon::new();
        c.ops.push(Op::Blur { x: 0, y: 0, w: w as u32, h: h as u32, radius: 3 });
        let mut want = field.clone();
        execute(&c, &AtlasStore { gen: 0, pages: alloc::vec::Vec::new(), glyphs: alloc::vec::Vec::new() }, &BlobStore::new(), &mut want, w, None);
        let mut got = field.clone();
        let tight = ClipRect { x0: 10, y0: 10, x1: 20, y1: 20 };
        execute(&c, &AtlasStore { gen: 0, pages: alloc::vec::Vec::new(), glyphs: alloc::vec::Vec::new() }, &BlobStore::new(), &mut got, w, Some(tight));
        let differs = (10..20).any(|y| (10..20).any(|x| got[y * w + x] != want[y * w + x]));
        assert!(differs, "an ungrown clip must be inexact at its edge");
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

    #[test]
    fn packer_refuses_past_the_page_cap_until_a_regen() {
        // The in-frame bound: a page cap is a refusal, never growth. 4x4
        // pages, one 4x4 glyph per page, cap 2: the third insert is None
        // and the store holds exactly the cap; the earlier ids still stand;
        // a regen (the between-frames eviction) reopens the store.
        let mut p = AtlasPacker::new(4, 4);
        p.set_max_pages(2);
        let a = p.insert(4, 4, &[1u8; 16], 0, 0).unwrap();
        let b = p.insert(4, 4, &[2u8; 16], 0, 0).unwrap();
        assert_eq!(p.store.pages.len(), 2);
        assert!(p.insert(4, 4, &[3u8; 16], 0, 0).is_none(), "at the cap: refused");
        assert!(p.insert(4, 4, &[3u8; 16], 0, 0).is_none(), "still refused");
        assert_eq!(p.store.pages.len(), 2, "no page opened by a refusal");
        assert_eq!(p.store.glyphs.len(), 2);
        assert_eq!(p.store.glyphs[a as usize].page, 0);
        assert_eq!(p.store.glyphs[b as usize].page, 1);
        // A glyph that fits the current page is still accepted at the cap.
        let mut q = AtlasPacker::new(4, 4);
        q.set_max_pages(1);
        assert!(q.insert(2, 2, &[1u8; 4], 0, 0).is_some());
        assert!(q.insert(2, 2, &[1u8; 4], 0, 0).is_some(), "shelf 0 still has room");
        assert!(q.insert(2, 2, &[1u8; 4], 0, 0).is_some(), "shelf 1 on the same page");
        assert!(q.insert(2, 2, &[1u8; 4], 0, 0).is_some());
        assert!(q.insert(2, 2, &[1u8; 4], 0, 0).is_none(), "the page is full and a second is past the cap");
        p.regen();
        assert_eq!(p.store.pages.len(), 0);
        assert!(p.insert(4, 4, &[3u8; 16], 0, 0).is_some(), "the regen reopened the store");
        // Unbounded (the default) still grows.
        let mut u = AtlasPacker::new(4, 4);
        for _ in 0..5 {
            assert!(u.insert(4, 4, &[0u8; 16], 0, 0).is_some());
        }
        assert_eq!(u.store.pages.len(), 5);
    }
}
