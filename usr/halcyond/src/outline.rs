// The outline path (HALCYON-TYPE section 4): skrifa reads a face and scales
// its glyph outlines to a pixel size; zeno fills them, and STROKES them by
// the theme's smoothing amount -- the em-relative dilation the Mac's "font
// smoothing" was measured to be (+18% stem weight, section 3.2), unioned
// into the fill. No hinting (section 4.4): the outline lands where the
// design puts it, at the whole-pixel pen the atlas caches one raster per
// (face, size, char) for. Since TY-4 the MONO cells come through here too
// -- `mono_cell` gives the grid the bake computes and `raster.rs` clips the
// glyph into it -- so there is one rasterizer and therefore one stroke rule
// for every tier. The bakes remain, for the consumers that must carry no
// rasterizer at all.
//
// Orientation, because it bit once: font space is y-UP, zeno's default
// TopLeft origin wants y-DOWN rows, and a BottomLeft mask is stored
// bottom-up. So the pen negates y as it records the outline and the fill
// renders upright with top-down rows; zeno's `Placement.top` is then the
// mask's top edge in rows below the baseline (negative above it), and the
// bearing cartoon wants -- UP from the baseline to the first row -- is its
// negation.

use alloc::vec::Vec;

use skrifa::charmap::Charmap;
use skrifa::instance::{LocationRef, Size};
use skrifa::outline::{DrawSettings, OutlineGlyphCollection, OutlinePen};
use skrifa::raw::tables::hmtx::Hmtx;
use skrifa::raw::TableProvider;
use skrifa::{FontRef, GlyphId, MetadataProvider};

/// A parsed face over the vendored bytes: the table directory plus the
/// three lookups every glyph needs (charmap, outlines, advances), parsed
/// once.
pub struct Face {
    font: FontRef<'static>,
    upem: u16,
    charmap: Charmap<'static>,
    outlines: OutlineGlyphCollection<'static>,
    hmtx: Option<Hmtx<'static>>,
}

/// One rendered glyph: `w x h` coverage bytes (rows tight, top-down) and
/// its bearing in the atlas's convention -- `left` from the pen, `top` UP
/// from the baseline to the first row. An empty outline (a space) is the
/// 0 x 0 raster, which packs as a zero-area entry with its advance.
pub struct Raster {
    pub w: u32,
    pub h: u32,
    pub left: i32,
    pub top: i32,
    pub alpha: Vec<u8>,
}

impl Raster {
    fn empty() -> Raster {
        Raster { w: 0, h: 0, left: 0, top: 0, alpha: Vec::new() }
    }
}

/// The pen skrifa draws into: zeno path commands, y negated (see the
/// header).
struct Pen {
    cmds: Vec<zeno::Command>,
}

impl OutlinePen for Pen {
    fn move_to(&mut self, x: f32, y: f32) {
        self.cmds.push(zeno::Command::MoveTo(zeno::Point::new(x, -y)));
    }
    fn line_to(&mut self, x: f32, y: f32) {
        self.cmds.push(zeno::Command::LineTo(zeno::Point::new(x, -y)));
    }
    fn quad_to(&mut self, cx0: f32, cy0: f32, x: f32, y: f32) {
        self.cmds.push(zeno::Command::QuadTo(
            zeno::Point::new(cx0, -cy0),
            zeno::Point::new(x, -y),
        ));
    }
    fn curve_to(&mut self, cx0: f32, cy0: f32, cx1: f32, cy1: f32, x: f32, y: f32) {
        self.cmds.push(zeno::Command::CurveTo(
            zeno::Point::new(cx0, -cy0),
            zeno::Point::new(cx1, -cy1),
            zeno::Point::new(x, -y),
        ));
    }
    fn close(&mut self) {
        self.cmds.push(zeno::Command::Close);
    }
}

/// The union of two partial coverages, 8-bit: `f + s - f*s`, never
/// `max(f, s)`. The stroke straddles the outline, so on an edge pixel about
/// half of it is already inside the fill and the other half is new ink; a
/// max counts none of the new ink (+3 % weight where +18 % is due, measured
/// in the lab). Exhaustively pinned over all 65536 pairs by the test below:
/// never exceeds 255, never falls below `max(f, s)`, never more than one
/// level from the real-valued union.
#[inline]
fn union8(f: u8, s: u8) -> u8 {
    let (f, s) = (f as u32, s as u32);
    (f + (s * (255 - f) + 127) / 255) as u8
}

/// The horizontal offset a phase names, in pixels: 0, 1/4, 1/2, 3/4.
#[inline]
pub fn phase_dx(phase: u8) -> f32 {
    (phase & 3) as f32 / 4.0
}

/// How many distinct horizontal phases a glyph can be rasterized at.
pub const PHASES: u8 = 4;

/// The largest em `mono_cell` will hand back. A monospace cell's em is a
/// small multiple of its advance (2x for Cornucopia), and the advance is
/// capped at `raster::MONO_ADVANCE_MAX`; this is well clear of that and
/// well inside a packer page, so it bounds the raster allocation without
/// being reachable by any real cell.
const EM_MAX: f32 = 512.0;

fn render(cmds: &[zeno::Command], style: zeno::Style, dx: f32) -> (Vec<u8>, zeno::Placement) {
    let mut m = zeno::Mask::new(cmds);
    m.origin(zeno::Origin::TopLeft);
    if dx != 0.0 {
        // BOTH, and the pair is load-bearing. `offset` alone moves the
        // rendered BOUNDS and leaves the path where it was, so the box
        // slides off the glyph: measured at dx = 3/4 it clipped a column
        // and lost 15 % of the ink, and at dx = 1/4 it did nothing at all.
        // `render_offset` is what actually translates the path. zeno's own
        // doc says it -- "to translate both the path and its rendered
        // bounding box, set both" -- and the pair reproduces a hand-
        // translated command list to within a coverage level.
        m.offset(zeno::Vector::new(dx, 0.0));
        m.render_offset(zeno::Vector::new(dx, 0.0));
    }
    m.style(style);
    m.render()
}

impl Face {
    /// Parse a face; None for bytes that are not a font (the vendored faces
    /// parse by construction -- a None here is a build-input defect).
    pub fn parse(bytes: &'static [u8]) -> Option<Face> {
        let font = FontRef::new(bytes).ok()?;
        let upem = font.head().ok()?.units_per_em();
        if upem == 0 {
            return None;
        }
        let charmap = font.charmap();
        let outlines = font.outline_glyphs();
        let hmtx = font.hmtx().ok();
        Some(Face { font, upem, charmap, outlines, hmtx })
    }

    /// The glyph for a char: .notdef (0) for one the face does not map,
    /// whose outline is the box -- the correct visible outcome for unmapped
    /// input, and what `raster` then draws.
    pub fn glyph_id(&self, ch: char) -> GlyphId {
        self.charmap.map(ch).unwrap_or_default()
    }

    /// Whether the face maps `ch` to a real glyph (not .notdef).
    pub fn has(&self, ch: char) -> bool {
        self.glyph_id(ch).to_u32() != 0
    }

    /// The advance of a glyph at `px`, fractional px: the hmtx advance
    /// scaled by px/upem, the expression fontdue evaluated, so every
    /// integer advance the layout inherited from it is unchanged.
    pub fn advance(&self, gid: GlyphId, px: f32) -> f32 {
        let units = self.hmtx.as_ref().and_then(|h| h.advance(gid)).unwrap_or(0);
        (px / self.upem as f32) * units as f32
    }

    /// The MONOSPACE cell this face fills at a cell width of `advance` px:
    /// (cell_h, baseline, em_px), or None for a face with no OS/2 table, no
    /// 'x', or a zero advance. This is `tools/bake-cornucopia.py`'s formula
    /// -- the cell is the OS/2 Windows ink bounds scaled by the ratio the
    /// advance asks for -- so the outline path lands on the SAME grid the
    /// baked atlases carry, which is the contract the cells tier shares
    /// (Aurora, the kernel trusted sink, the pts geometry every tile is
    /// sized from). `the_derived_cell_table_is_the_baked_one` proves the
    /// agreement at every baked advance rather than asserting it.
    ///
    /// The bake takes the ceiling in float; this takes it in integers,
    /// which is exact and needs no libm. They can only disagree where the
    /// product is exactly an integer, and it never is: 889 and 1097 are
    /// both coprime to the 500-unit advance, so no cell width under 500
    /// divides evenly.
    pub fn mono_cell(&self, advance: u8) -> Option<(i32, i32, f32)> {
        let os2 = self.font.os2().ok()?;
        let units = self
            .hmtx
            .as_ref()
            .and_then(|h| h.advance(self.glyph_id('x')))
            .filter(|u| *u != 0)? as u32;
        let a = advance as u32;
        let ceil_div = |n: u32| ((n * a) + units - 1) / units;
        let asc = os2.us_win_ascent() as u32;
        let desc = os2.us_win_descent() as u32;
        let cell_h = ceil_div(asc + desc);
        let baseline = ceil_div(asc);
        if cell_h == 0 || baseline == 0 || baseline > cell_h {
            return None;
        }
        // The em the outline is rasterized at: the size whose advance IS
        // the cell width. Cornucopia's advance is half its em, so this is
        // 2x the cell width -- derived, not assumed, so a re-cut font with
        // a different ratio still lands in its cell.
        //
        // BOUNDED, because it sizes an allocation: `mono_cell_alpha` asks
        // zeno for a raster at this em BEFORE the packer gets to refuse an
        // oversize one, so a font whose 'x' advance is tiny against its
        // upem would allocate first and be rejected after. The face is a
        // build input and cannot be hostile today; this is one comparison
        // on a path whose whole purpose is to be bounded.
        let em = a as f32 * self.upem as f32 / units as f32;
        if !(em > 0.0) || em > EM_MAX {
            return None;
        }
        Some((cell_h as i32, baseline as i32, em))
    }

    /// The line metrics at `px`, fractional: (ascent, descent, line gap),
    /// the descent negative as the tables carry it. skrifa's table choice
    /// (hhea; OS/2 typo under USE_TYPO_METRICS; the Windows pair last) and
    /// its px/upem scale are fontdue's, so the rounded values the layout
    /// inherited are unchanged (the raster tests pin them).
    pub fn line_metrics(&self, px: f32) -> (f32, f32, f32) {
        let m = self.font.metrics(Size::new(px), LocationRef::default());
        (m.ascent, m.descent, m.leading)
    }

    /// Rasterize a glyph at `px`, unhinted, at horizontal phase `phase`
    /// quarter-pixels (0..=3; anything larger is taken modulo 4), with a
    /// smoothing stroke of `smooth_mem` thousandths of an em (0 = the plain
    /// fill). The stroke is centred on the outline, so it dilates the fill
    /// by half its width and the raster grows by at most `ceil(stroke/2)`
    /// px on each side -- at 12 mem that is under one px through 166 px.
    ///
    /// The phase shifts the OUTLINE before rasterizing, so the returned
    /// `left` is already the floor of the shifted bbox: a caller blits at
    /// `whole_pen + left` and gets the sub-pixel placement for free. The
    /// vertical is deliberately unphased (HALCYON-TYPE section 4.3:
    /// baselines are integers and a vertical phase buys nothing on
    /// horizontal stems).
    pub fn raster(&self, gid: GlyphId, px: f32, smooth_mem: u16, phase: u8) -> Raster {
        let mut pen = Pen { cmds: Vec::new() };
        if let Some(glyph) = self.outlines.get(gid) {
            // A draw error mid-glyph (a malformed outline in a vendored
            // face) keeps what was emitted; the faces are build inputs.
            let _ = glyph.draw(
                DrawSettings::unhinted(Size::new(px), LocationRef::default()),
                &mut pen,
            );
        }
        if pen.cmds.is_empty() {
            return Raster::empty();
        }
        let dx = phase_dx(phase);
        let (fill, fp) = render(&pen.cmds, zeno::Style::Fill(zeno::Fill::NonZero), dx);
        if fp.width == 0 || fp.height == 0 {
            return Raster::empty();
        }
        let stroke_px = px * smooth_mem as f32 / 1000.0;
        if smooth_mem == 0 || stroke_px <= 0.0 {
            return Raster { w: fp.width, h: fp.height, left: fp.left, top: -fp.top, alpha: fill };
        }
        let (stroke, sp) = render(&pen.cmds, zeno::Style::Stroke(zeno::Stroke::new(stroke_px)), dx);
        if sp.width == 0 || sp.height == 0 {
            return Raster { w: fp.width, h: fp.height, left: fp.left, top: -fp.top, alpha: fill };
        }
        // The union of the two coverages on the box that holds both (the
        // stroke straddles the outline, so its box holds the fill's; the
        // explicit union costs nothing and assumes nothing). A pixel half
        // inside the fill and half under the stroke's outer half is
        // `f + s - f*s` covered -- a max would count the stroke's new ink as
        // none (+3% weight where +18% was measured, section 3.2).
        let left = fp.left.min(sp.left);
        let top = fp.top.min(sp.top);
        let right = (fp.left + fp.width as i32).max(sp.left + sp.width as i32);
        let bottom = (fp.top + fp.height as i32).max(sp.top + sp.height as i32);
        let (w, h) = ((right - left) as usize, (bottom - top) as usize);
        let mut alpha = alloc::vec![0u8; w * h];
        let mut lay = |cov: &[u8], p: &zeno::Placement, union: bool| {
            for y in 0..p.height as usize {
                let oy = (p.top - top) as usize + y;
                for x in 0..p.width as usize {
                    let ox = (p.left - left) as usize + x;
                    let v = cov[y * p.width as usize + x];
                    let dst = &mut alpha[oy * w + ox];
                    *dst = if union { union8(*dst, v) } else { v };
                }
            }
        };
        lay(&fill, &fp, false);
        lay(&stroke, &sp, true);
        Raster { w: w as u32, h: h as u32, left, top: -top, alpha }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn text() -> Face {
        Face::parse(crate::IBM_PLEX_SANS_TEXT).expect("Plex Text parses")
    }

    // The union, over its WHOLE domain -- 65536 pairs, so this is a proof
    // and not a sample. It is the one piece of arithmetic in the stroke
    // path where being wrong is silent: a saturating bug shows as text
    // that is merely a little heavy, and a max-instead-of-union shows as
    // text that is merely a little light. Neither would fail a gate.
    #[test]
    fn the_union_is_exact_and_bounded_everywhere() {
        for f in 0..=255u8 {
            for s in 0..=255u8 {
                let r = union8(f, s);
                assert!(r >= f.max(s), "union({f},{s}) = {r} lost ink");
                // r is a u8, so "never exceeds 255" is the type's; what
                // must be checked is that it never WRAPPED to get there.
                let exact = 255.0 * (f as f32 / 255.0 + s as f32 / 255.0 - (f as f32 / 255.0) * (s as f32 / 255.0));
                assert!((r as f32 - exact).abs() <= 1.0, "union({f},{s}) = {r}, exact {exact}");
            }
        }
        assert_eq!(union8(255, 255), 255, "full over full stays full");
        assert_eq!(union8(0, 255), 255, "the stroke alone on bare ground");
        assert_eq!(union8(255, 0), 255, "the fill alone");
        assert_eq!(union8(128, 128), 192, "half over half is three quarters, not half");
        assert!(union8(128, 128) > 128, "a max would have said 128 -- the +3%-not-+18% bug");
    }

    #[test]
    fn parses_and_maps() {
        let f = text();
        assert_eq!(f.upem, 1000, "Plex is a 1000-unit em");
        assert!(f.has('A') && f.has('a') && f.has(' '));
        assert!(!f.has('\u{22A2}'), "no turnstile in Plex (the island serves it)");
        assert!(!f.has('\u{4E00}'), "no CJK");
        assert_eq!(f.glyph_id('\u{4E00}').to_u32(), 0, "unmapped -> .notdef");
        assert!(Face::parse(b"not a font").is_none());
    }

    #[test]
    fn the_raster_sits_on_the_baseline_upright() {
        // 'A' at 16 px: the top bearing is the cap height (~0.7 em = 11 px),
        // nothing hangs below the baseline; 'g' hangs its descender below.
        // Upright = the crossbar of the 'A' is nearer the bottom than the
        // top, and the apex row is the narrowest inked row.
        let f = text();
        let a = f.raster(f.glyph_id('A'), 16.0, 0, 0);
        assert!(a.w > 4 && a.h > 8, "{}x{}", a.w, a.h);
        assert!((10..=13).contains(&a.top), "cap top {}", a.top);
        assert_eq!(a.h as i32 - a.top, 0, "'A' ends on the baseline");
        let inked = |r: &Raster, y: u32| (0..r.w).filter(|&x| r.alpha[(y * r.w + x) as usize] > 64).count();
        assert!(inked(&a, 0) < inked(&a, a.h - 1), "apex narrow, feet wide: upright");
        let g = f.raster(f.glyph_id('g'), 16.0, 0, 0);
        assert!(g.h as i32 - g.top > 0, "'g' descends {} rows", g.h as i32 - g.top);
        let sp = f.raster(f.glyph_id(' '), 16.0, 0, 0);
        assert_eq!((sp.w, sp.h), (0, 0), "a space has no coverage");
        assert!(f.advance(f.glyph_id(' '), 16.0) > 0.0, "but an advance");
    }

    // HALCYON-TYPE 4.3: the phase shifts the outline before rasterizing,
    // so the sub-pixel placement rides the mask's own `left` and the
    // caller still blits at a whole pixel. What must hold: the four
    // phases are DISTINCT rasters (a phase that renders identically buys
    // nothing and would be a silent no-op), the ink is conserved within
    // a rounding of the exact area, the box never grows by more than a
    // pixel, and phase 0 is byte-identical to the unphased raster.
    #[test]
    fn the_four_phases_are_distinct_and_conserve_ink() {
        let f = text();
        let n = f.glyph_id('n');
        let px = 17.5;
        let base = f.raster(n, px, 0, 0);
        let mut seen: Vec<(i32, Vec<u8>)> = Vec::new();
        for p in 0..PHASES {
            let r = f.raster(n, px, 0, p);
            assert!(r.w <= base.w + 1, "phase {p}: {} vs {}", r.w, base.w);
            assert_eq!(r.h, base.h, "phase {p}: the vertical is unphased");
            assert_eq!(r.top, base.top, "phase {p}: the baseline does not move");
            let ink: i64 = r.alpha.iter().map(|&a| a as i64).sum();
            let b: i64 = base.alpha.iter().map(|&a| a as i64).sum();
            // Shifting a shape cannot create or destroy area; only the
            // pixel grid's rounding moves, so allow 2%.
            assert!((ink - b).abs() * 50 <= b, "phase {p}: ink {ink} vs {b}");
            seen.push((r.left, r.alpha.clone()));
        }
        assert_eq!(seen[0].1, base.alpha, "phase 0 IS the unphased raster");
        for i in 0..seen.len() {
            for j in (i + 1)..seen.len() {
                assert_ne!(
                    seen[i], seen[j],
                    "phases {i} and {j} render identically -- the phase is a no-op"
                );
            }
        }
        // The phase is taken modulo 4, so a caller cannot index off the end.
        assert_eq!(f.raster(n, px, 0, 4).alpha, seen[0].1);
        assert_eq!(f.raster(n, px, 0, 7).alpha, seen[3].1);
        assert_eq!(phase_dx(0), 0.0);
        assert_eq!(phase_dx(3), 0.75);
        assert_eq!(phase_dx(5), 0.25, "modulo 4");
    }

    #[test]
    fn the_stroke_dilates_and_bounds() {
        // The smoothing stroke: more ink, the same box within one px a
        // side, and the advance untouched (it is the table's, not the
        // raster's). Stroke 0 is the plain fill byte for byte.
        let f = Face::parse(crate::IBM_PLEX_SANS_HEADING_ITALIC).unwrap();
        let n = f.glyph_id('n');
        let plain = f.raster(n, 35.0, 0, 0);
        let again = f.raster(n, 35.0, 0, 0);
        assert_eq!(plain.alpha, again.alpha, "deterministic");
        let smooth = f.raster(n, 35.0, 12, 0);
        let ink = |r: &Raster| r.alpha.iter().map(|&a| a as u64).sum::<u64>();
        let (i0, i1) = (ink(&plain), ink(&smooth));
        assert!(i1 > i0, "more ink: {i1} vs {i0}");
        assert!(smooth.w <= plain.w + 2 && smooth.h <= plain.h + 2, "grows at most a px a side");
        assert!(smooth.left >= plain.left - 1 && smooth.top <= plain.top + 1);
        assert_eq!(f.advance(n, 35.0), f.advance(n, 35.0));
        // Every pixel of the union is at least the fill's coverage there.
        for y in 0..plain.h {
            for x in 0..plain.w {
                let ox = (plain.left - smooth.left) as u32 + x;
                let oy = (smooth.top - plain.top) as u32 + y;
                assert!(smooth.alpha[(oy * smooth.w + ox) as usize] >= plain.alpha[(y * plain.w + x) as usize]);
            }
        }
    }
}
