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

/// A face slot in this source: the two vendored DejaVu weights, plus the
/// system monospace -- the baked Cornucopia atlas (fixed cell, one size
/// per advance), serving mono islands + foreign terminal output through
/// the SAME packer/id space so one atlas store feeds the executor.
pub const FACE_BODY: u8 = 0;
pub const FACE_BODY_BOLD: u8 = 1;
pub const FACE_MONO: u8 = 2;

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
    mono: cornucopia::Atlas,
    pub packer: AtlasPacker,
    cache: BTreeMap<(u8, u32, char), Cached>,
}

/// The cache key's size quantum: half pixels.
#[inline]
fn size_q(px: f32) -> u32 {
    (px * 2.0 + 0.5) as u32
}

impl GlyphSource {
    /// Build over the vendored faces. `page` is the atlas page geometry
    /// (one page holds many shelves; 512 fits several sizes of a Latin
    /// working set).
    pub fn new_vendored(page: u32) -> GlyphSource {
        let mut faces = Vec::new();
        for bytes in [
            crate::DEJAVU_SANS_CONDENSED,
            crate::DEJAVU_SANS_CONDENSED_BOLD,
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
            mono: cornucopia::Atlas::for_advance(cornucopia::DEFAULT_ADVANCE),
            packer: AtlasPacker::new(page, page),
            cache: BTreeMap::new(),
        }
    }

    /// The mono cell geometry (w, h, baseline) -- the raw-VT / foreign-
    /// block metrics, and the mono island's advance.
    pub fn mono_cell(&self) -> (i32, i32, i32) {
        (
            self.mono.cell_w() as i32,
            self.mono.cell_h() as i32,
            self.mono.baseline() as i32,
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
    /// FACE_MONO ignores `px` (the baked atlas has one size per advance)
    /// and serves the Cornucopia cell; a codepoint the 207-glyph bake
    /// lacks falls back to DejaVu rasterized to the cell height with the
    /// advance FORCED to the cell width (the grid survives; the glyph may
    /// clip -- recorded MVP posture; box drawing stays a renderer concern).
    pub fn glyph(&mut self, face: u8, px: f32, ch: char) -> Option<GlyphRef> {
        let q = if face == FACE_MONO { 0 } else { size_q(px) };
        let key = (face, q, ch);
        if let Some(c) = self.cache.get(&key) {
            return Some(GlyphRef {
                glyph: c.id,
                advance: c.advance,
            });
        }
        if face == FACE_MONO {
            let (cw, chh, base) = self.mono_cell();
            if let Some(alpha) = self.mono.glyph(ch) {
                let id = self.packer.insert(cw as u32, chh as u32, alpha, 0, base)?;
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
    /// FACE_MONO's are the baked cell's (px ignored).
    pub fn line_metrics(&self, face: u8, px: f32) -> Option<LineMetrics> {
        if face == FACE_MONO {
            let (_, chh, base) = self.mono_cell();
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

    /// The kerning adjustment between two glyphs at a size (integer px),
    /// 0 when the face carries no pair. The author adds this into the
    /// PRECEDING glyph's resolved advance (DejaVu carries real pairs --
    /// HALCYON.md section 2).
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

    /// The current atlas generation (what the author stamps into ops).
    pub fn gen(&self) -> u32 {
        self.packer.store.gen
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn vendored_faces_parse() {
        let gs = GlyphSource::new_vendored(512);
        assert_eq!(gs.face_count(), 2, "both vendored DejaVu weights parse");
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
    fn kerning_pairs_exist_in_dejavu() {
        let mut gs = GlyphSource::new_vendored(512);
        // Force both glyphs so the face is warm (not required, but mirrors use).
        gs.glyph(FACE_BODY, 32.0, 'A').unwrap();
        gs.glyph(FACE_BODY, 32.0, 'V').unwrap();
        let k = gs.kern(FACE_BODY, 32.0, 'A', 'V');
        assert!(k < 0, "AV kerns negative in DejaVu at 32px: {}", k);
        assert_eq!(gs.kern(FACE_BODY, 32.0, 'x', 'x'), 0, "xx carries no pair");
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
