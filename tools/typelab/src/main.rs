//! typelab -- the Halcyon typography lab: the same Daylight text through the
//! rendering variants under study, side by side, with a measurement beside
//! the eye. Host-only research tooling (HALCYON-TYPE).
//!
//! Producers: fontdue (the as-built rasterizer), skrifa + zeno (outline +
//! autohinter), FreeType via ./ftdump (masks), CoreText via ./ct (composited
//! reference). Everything is composited here except CoreText, which owns its
//! own compositing and is the reference.

use std::collections::{BTreeMap, BTreeSet, HashMap};
use std::fs;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};
use std::process::Command;

use skrifa::instance::{LocationRef, Size};
use skrifa::outline::{DrawSettings, Engine, HintingInstance, HintingOptions, OutlinePen, SmoothMode, Target};
use skrifa::{FontRef, MetadataProvider};

const REPO: &str = "/Users/northkillpd/projects/thylacine";
const CORNUCOPIA: &str = "/Users/northkillpd/projects/cornucopia-font/cornucopia-Regular.ttf";

// Daylight (HALCYON-VISUAL 1.1 / 1.2).
const SURFACE: u32 = 0xF2EBE0;
const FG: u32 = 0x1A120A;
const FG_DIM: u32 = 0x3A2E22;

fn face_path(key: &str) -> String {
    match key {
        "text" => format!("{REPO}/third_party/ibm-plex/ttf/IBMPlexSans-Text.ttf"),
        "bold" => format!("{REPO}/third_party/ibm-plex/ttf/IBMPlexSans-Bold.ttf"),
        "ti" => format!("{REPO}/third_party/ibm-plex/ttf/IBMPlexSans-TextItalic.ttf"),
        "hi" => format!("{REPO}/third_party/ibm-plex/ttf/IBMPlexSans-Italic.ttf"),
        "mono" => CORNUCOPIA.to_string(),
        _ => panic!("unknown face {key}"),
    }
}

// ---------------------------------------------------------------------------
// The sample definition (one source of truth for every producer).

struct Run {
    face: &'static str,
    px: f32,
    color: u32,
    text: String,
}
struct Line {
    baseline: i32,
    x0: f32,
    runs: Vec<Run>,
}
struct Crop {
    name: &'static str,
    x: u32,
    y: u32,
    w: u32,
    h: u32,
    zoom: u32,
}
struct Sample {
    name: String,
    w: u32,
    h: u32,
    lines: Vec<Line>,
    crops: Vec<Crop>,
    probe: bool,
}

fn run(face: &'static str, px: f32, color: u32, text: &str) -> Run {
    Run { face, px, color, text: text.to_string() }
}

fn samples(scale: f32) -> Vec<Sample> {
    let s = scale;
    let h1 = 17.5 * s;
    let body = 11.5 * s;
    let mono = 12.0 * s;
    let sc = |v: f32| (v * s).round() as i32;
    let mut v = Vec::new();
    // The tour: the welcome screen's heading, a prose line with mono islands,
    // and the dim closing line (halcyon-daylight-mockups.html lines 935-963).
    v.push(Sample {
        name: "tour".into(),
        w: (390.0 * s) as u32,
        h: (75.0 * s) as u32,
        lines: vec![
            Line { baseline: sc(22.0), x0: 6.0 * s, runs: vec![run("hi", h1, FG, "Halcyon Terminal of Thylacine OS 1.1")] },
            Line {
                baseline: sc(46.0),
                x0: 6.0 * s,
                runs: vec![
                    run("text", body, FG, "Territory namespaces are visible at "),
                    run("mono", mono, FG, "/"),
                    run("text", body, FG, "; try "),
                    run("mono", mono, FG, "ls /dev"),
                    run("text", body, FG, " to start."),
                ],
            },
            Line {
                baseline: sc(64.0),
                x0: 6.0 * s,
                runs: vec![
                    run("text", body, FG_DIM, "Thylacine is free and open software; see "),
                    run("mono", mono, FG_DIM, "LICENSE"),
                    run("text", body, FG_DIM, ". This build carries no warranty."),
                ],
            },
        ],
        crops: vec![
            Crop { name: "heading", x: (70.0 * s) as u32, y: (5.0 * s) as u32, w: (100.0 * s) as u32, h: (23.0 * s) as u32, zoom: (8.0 / s) as u32 },
            Crop { name: "prose", x: (56.0 * s) as u32, y: (34.0 * s) as u32, w: (90.0 * s) as u32, h: (18.0 * s) as u32, zoom: (8.0 / s) as u32 },
        ],
        probe: false,
    });
    // Probes: one glyph, integer origin, the middle band of its rows measured.
    // Italic n is the operator's own magnified glyph; italic l the plainest
    // slanted stem; roman y/v/A the body's diagonals; roman n the control
    // (vertical stems: nothing to notch).
    for (face, px, ch, tag) in [
        ("hi", h1, 'n', "italic-n"),
        ("hi", h1, 'l', "italic-l"),
        ("text", body, 'y', "roman-y"),
        ("text", body, 'v', "roman-v"),
        ("text", body, 'A', "roman-A"),
        ("text", body, 'n', "roman-n"),
    ] {
        let w = (px * 1.3) as u32 + 6;
        let h = (px * 1.5) as u32 + 4;
        v.push(Sample {
            name: format!("probe-{tag}"),
            w,
            h,
            lines: vec![Line { baseline: (px * 1.1) as i32 + 2, x0: 3.0, runs: vec![run(face, px, FG, &ch.to_string())] }],
            crops: vec![Crop { name: "glyph", x: 0, y: 0, w, h, zoom: (200 / w).max(4) }],
            probe: true,
        });
    }
    v
}

// ---------------------------------------------------------------------------
// Masks.

#[derive(Clone)]
struct Mask {
    w: usize,
    h: usize,
    /// Horizontal bearing: the mask's left column relative to the pen origin.
    left: i32,
    /// Vertical bearing, y-UP: rows from the baseline up to the mask's top row.
    top: i32,
    cov: Vec<u8>,
    /// The unrounded advance.
    adv: f32,
    /// The as-built advance: `(adv + 0.5) as i32`.
    adv_int: i32,
}

impl Mask {
    fn empty(adv: f32) -> Mask {
        Mask { w: 0, h: 0, left: 0, top: 0, cov: Vec::new(), adv, adv_int: (adv + 0.5) as i32 }
    }
}

/// Mask-domain emboldening by `r` px: the edge MOVES outward by r. A pixel
/// gains from a 4-neighbour only when that neighbour is within r of full
/// (`(n - (1 - r)) / r`: the edge lies inside the last r of it), so a fringe
/// pixel's faint neighbours spawn nothing -- a plain bleed (`r * (1 - c) *
/// max(n)`) haloed every edge (measured: the fringe doubled, 3.8 -> 7.1
/// partial px per row, while the Mac's smoothing leaves it at 3.8). The
/// stand-in for stroking the outline where there is no outline (fontdue).
fn dilate(m: &Mask, r: f32) -> Mask {
    if m.w == 0 || m.h == 0 || r <= 0.0 {
        return m.clone();
    }
    let (w, h) = (m.w + 2, m.h + 2);
    let at = |x: i32, y: i32| -> f32 {
        let (x, y) = (x - 1, y - 1);
        if x < 0 || y < 0 || x as usize >= m.w || y as usize >= m.h {
            0.0
        } else {
            m.cov[y as usize * m.w + x as usize] as f32 / 255.0
        }
    };
    let near = |n: f32| ((n - (1.0 - r)) / r).clamp(0.0, 1.0);
    let mut cov = vec![0u8; w * h];
    for y in 0..h as i32 {
        for x in 0..w as i32 {
            let c = at(x, y);
            // Each side contributes its own edge motion; union them.
            let mut v = c;
            for n in [at(x - 1, y), at(x + 1, y), at(x, y - 1), at(x, y + 1)] {
                let g = r * near(n);
                v = v + g - v * g;
            }
            cov[y as usize * w + x as usize] = (v.min(1.0) * 255.0 + 0.5) as u8;
        }
    }
    Mask { w, h, left: m.left - 1, top: m.top + 1, cov, adv: m.adv, adv_int: m.adv_int }
}

// ---------------------------------------------------------------------------
// fontdue.

struct FdFace {
    font: fontdue::Font,
}

impl FdFace {
    fn load(path: &str) -> FdFace {
        let bytes = fs::read(path).expect("font file");
        FdFace { font: fontdue::Font::from_bytes(bytes, fontdue::FontSettings::default()).expect("fontdue parse") }
    }

    /// `k` thirds of a pixel to the right (0..3), via fontdue's 3x raster.
    fn mask(&self, ch: char, px: f32, k: u32) -> Mask {
        let idx = self.font.lookup_glyph_index(ch);
        if k == 0 {
            let (m, bm) = self.font.rasterize_indexed(idx, px);
            if m.width == 0 || m.height == 0 {
                return Mask::empty(m.advance_width);
            }
            return Mask {
                w: m.width,
                h: m.height,
                left: m.xmin,
                top: m.height as i32 + m.ymin,
                cov: bm,
                adv: m.advance_width,
                adv_int: (m.advance_width + 0.5) as i32,
            };
        }
        let (m, bm) = self.font.rasterize_indexed_subpixel(idx, px);
        if m.width == 0 || m.height == 0 {
            return Mask::empty(m.advance_width);
        }
        let sw = m.width * 3;
        let w = m.width + 1;
        let mut cov = vec![0u8; w * m.height];
        for y in 0..m.height {
            let row = &bm[y * sw..(y + 1) * sw];
            for x in 0..w {
                let mut acc = 0u32;
                for j in 0..3 {
                    let sj = 3 * x as i32 - k as i32 + j;
                    if sj >= 0 && (sj as usize) < sw {
                        acc += row[sj as usize] as u32;
                    }
                }
                cov[y * w + x] = ((acc + 1) / 3) as u8;
            }
        }
        Mask { w, h: m.height, left: m.xmin, top: m.height as i32 + m.ymin, cov, adv: m.advance_width, adv_int: (m.advance_width + 0.5) as i32 }
    }
}

// ---------------------------------------------------------------------------
// skrifa + zeno.

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
enum Hint {
    None,
    AutoLight,
    AutoNormal,
    Interp,
}

struct ZPen {
    cmds: Vec<zeno::Command>,
}

// The outline arrives y-up (font space); the pen emits it y-DOWN so zeno's
// default TopLeft origin renders it upright with top-down rows (a BottomLeft
// mask is stored bottom-up: the glyphs came out flipped).
impl OutlinePen for ZPen {
    fn move_to(&mut self, x: f32, y: f32) {
        self.cmds.push(zeno::Command::MoveTo(zeno::Point::new(x, -y)));
    }
    fn line_to(&mut self, x: f32, y: f32) {
        self.cmds.push(zeno::Command::LineTo(zeno::Point::new(x, -y)));
    }
    fn quad_to(&mut self, cx0: f32, cy0: f32, x: f32, y: f32) {
        self.cmds.push(zeno::Command::QuadTo(zeno::Point::new(cx0, -cy0), zeno::Point::new(x, -y)));
    }
    fn curve_to(&mut self, cx0: f32, cy0: f32, cx1: f32, cy1: f32, x: f32, y: f32) {
        self.cmds.push(zeno::Command::CurveTo(zeno::Point::new(cx0, -cy0), zeno::Point::new(cx1, -cy1), zeno::Point::new(x, -y)));
    }
    fn close(&mut self) {
        self.cmds.push(zeno::Command::Close);
    }
}

struct SkFace {
    data: Vec<u8>,
}

impl SkFace {
    fn load(path: &str) -> SkFace {
        SkFace { data: fs::read(path).expect("font file") }
    }

    /// Exact fractional offset `frac` (0..1) px to the right; `stroke` > 0
    /// dilates by stroking the outline (width = 2 x the dilation radius).
    fn mask(&self, ch: char, px: f32, hint: Hint, frac: f32, stroke: f32) -> Mask {
        let font = FontRef::new(&self.data).expect("skrifa parse");
        let size = Size::new(px);
        let gid = font.charmap().map(ch).unwrap_or_default();
        let adv = font.glyph_metrics(size, LocationRef::default()).advance_width(gid).unwrap_or(0.0);
        let outlines = font.outline_glyphs();
        let Some(glyph) = outlines.get(gid) else { return Mask::empty(adv) };
        let mut pen = ZPen { cmds: Vec::new() };
        let opts = |engine, mode| HintingOptions {
            engine,
            target: Target::Smooth { mode, symmetric_rendering: false, preserve_linear_metrics: true },
        };
        let hinter = match hint {
            Hint::None => None,
            Hint::AutoLight => Some(HintingInstance::new(&outlines, size, LocationRef::default(), opts(Engine::Auto(None), SmoothMode::Light)).expect("hinter")),
            Hint::AutoNormal => Some(HintingInstance::new(&outlines, size, LocationRef::default(), opts(Engine::Auto(None), SmoothMode::Normal)).expect("hinter")),
            Hint::Interp => Some(HintingInstance::new(&outlines, size, LocationRef::default(), opts(Engine::Interpreter, SmoothMode::Normal)).expect("hinter")),
        };
        match &hinter {
            Some(h) => glyph.draw(DrawSettings::hinted(h, false), &mut pen).expect("draw"),
            None => glyph.draw(DrawSettings::unhinted(size, LocationRef::default()), &mut pen).expect("draw"),
        };
        if pen.cmds.is_empty() {
            return Mask::empty(adv);
        }
        let render = |style: zeno::Style| -> (Vec<u8>, zeno::Placement) {
            let mut m = zeno::Mask::new(&pen.cmds[..]);
            m.origin(zeno::Origin::TopLeft);
            m.offset(zeno::Vector::new(frac, 0.0));
            m.style(style);
            m.render()
        };
        let (fill, p) = render(zeno::Style::Fill(zeno::Fill::NonZero));
        if p.width == 0 || p.height == 0 {
            return Mask::empty(adv);
        }
        // With the y-down outline, zeno's placement `top` is the mask's top
        // edge in image rows (negative above the baseline); our bearing is y-up.
        let mut mask = Mask { w: p.width as usize, h: p.height as usize, left: p.left, top: -p.top, cov: fill, adv, adv_int: (adv + 0.5) as i32 };
        if stroke > 0.0 {
            let (sk, sp) = render(zeno::Style::Stroke(zeno::Stroke::new(stroke)));
            // fill UNION stroke on the stroke's (larger) placement. The union
            // of two partial coverages is `f + s - f*s`, not `max(f, s)`: the
            // stroke straddles the edge, so on an edge pixel about half of it
            // is already inside the fill and the other half is new ink (a max
            // counted none of it: +3% weight where +18% was expected).
            let (w, h) = (sp.width as usize, sp.height as usize);
            let sp_top = -sp.top;
            let mut cov = sk;
            for y in 0..mask.h {
                for x in 0..mask.w {
                    let gx = x as i32 + mask.left - sp.left;
                    let gy = (sp_top - mask.top) + y as i32;
                    if gx >= 0 && gy >= 0 && (gx as usize) < w && (gy as usize) < h {
                        let i = gy as usize * w + gx as usize;
                        let f = mask.cov[y * mask.w + x] as f32 / 255.0;
                        let s = cov[i] as f32 / 255.0;
                        cov[i] = ((f + s - f * s) * 255.0 + 0.5) as u8;
                    }
                }
            }
            mask = Mask { w, h, left: sp.left, top: sp_top, cov, adv, adv_int: mask.adv_int };
        }
        mask
    }
}

// ---------------------------------------------------------------------------
// FreeType masks (from ./ftdump).

fn ft_key(face: &str, px: f32, mode: &str, k: u32) -> String {
    format!("{face}-{}-{mode}-s{k}", (px * 100.0).round() as u32)
}

fn ft_dump(dir: &Path, face: &str, px: f32, mode: &str, k: u32, chars: &str) -> HashMap<char, Mask> {
    let prefix = ft_key(face, px, mode, k);
    let st = Command::new("./ftdump")
        .args([&face_path(face), &format!("{px}"), mode, dir.to_str().unwrap(), &prefix, chars, &k.to_string()])
        .status()
        .expect("ftdump");
    assert!(st.success(), "ftdump failed for {prefix}");
    let idx = fs::read_to_string(dir.join(format!("{prefix}.idx"))).expect("idx");
    let mut out = HashMap::new();
    for line in idx.lines() {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.first() != Some(&"g") {
            continue;
        }
        let cp: u32 = f[1].parse().unwrap();
        let (w, h): (usize, usize) = (f[2].parse().unwrap(), f[3].parse().unwrap());
        let (left, top): (i32, i32) = (f[4].parse().unwrap(), f[5].parse().unwrap());
        let adv_hinted: f32 = f[6].parse().unwrap();
        let adv_lin: f32 = f[7].parse().unwrap();
        let cov = if w > 0 && h > 0 { read_pgm(&dir.join(f[8])) } else { Vec::new() };
        let ch = char::from_u32(cp).unwrap();
        // The hinted advance is what a hinting stack advances by on an integer
        // pen; the linear one is the fractional pen's.
        out.insert(ch, Mask { w, h, left, top, cov, adv: adv_lin, adv_int: (adv_hinted + 0.5) as i32 });
    }
    out
}

fn read_pgm(p: &Path) -> Vec<u8> {
    let b = fs::read(p).expect("pgm");
    // P5\nW H\n255\n<data>
    let mut pos = 0;
    let mut fields = Vec::new();
    while fields.len() < 4 {
        while b[pos].is_ascii_whitespace() {
            pos += 1;
        }
        let s = pos;
        while !b[pos].is_ascii_whitespace() {
            pos += 1;
        }
        fields.push(std::str::from_utf8(&b[s..pos]).unwrap().to_string());
    }
    pos += 1;
    b[pos..].to_vec()
}

// ---------------------------------------------------------------------------
// Variants.

#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
enum Raster {
    Fontdue,
    Skrifa(Hint),
    Ft(&'static str),
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Pen {
    /// The as-built pen: integer advances (`(adv + 0.5) as i32`), one raster.
    Int,
    /// Thirds of a pixel (fontdue's 3x raster; ftdump's outline shift).
    Third,
    /// Exact fractional placement (zeno's offset).
    Exact,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Blend {
    /// cartoon::blend verbatim: a lerp of the sRGB-encoded channels.
    Srgb,
    /// Linear-light: decode, lerp, re-encode.
    Linear,
    /// The classic compromise: lerp in a gamma-1.45 space (Photoshop's "blend
    /// text colors using gamma 1.45"; between the encoded and the linear).
    Mid,
}

fn blend_mid(bg: u32, fg: u32, a: u8) -> u32 {
    if a == 0 {
        return bg;
    }
    if a == 255 {
        return fg;
    }
    let t = a as f32 / 255.0;
    let mut out = 0u32;
    for sh in [16, 8, 0] {
        let b = (((bg >> sh) & 0xFF) as f32 / 255.0).powf(1.45);
        let f = (((fg >> sh) & 0xFF) as f32 / 255.0).powf(1.45);
        let v = (b + (f - b) * t).powf(1.0 / 1.45);
        out |= ((v.clamp(0.0, 1.0) * 255.0 + 0.5) as u32) << sh;
    }
    out
}

struct Variant {
    name: &'static str,
    raster: Raster,
    pen: Pen,
    blend: Blend,
    /// Mask-domain emboldening, px per side (fontdue / FreeType masks).
    dilate: f32,
    /// Outline stroke width as a FRACTION OF THE EM (skrifa): the dilation
    /// per side is half of it. 0.03 em = 0.5 px at 35 px, 0.35 px at 23.
    stroke_em: f32,
}

fn variants() -> Vec<Variant> {
    let v = |name, raster, pen, blend, dilate, stroke_em| Variant { name, raster, pen, blend, dilate, stroke_em };
    vec![
        // The compositing axis, on the as-built rasterizer.
        v("A-asbuilt", Raster::Fontdue, Pen::Int, Blend::Srgb, 0.0, 0.0),
        v("A2-asbuilt-embolden", Raster::Fontdue, Pen::Int, Blend::Srgb, 0.25, 0.0),
        v("B-gamma", Raster::Fontdue, Pen::Int, Blend::Linear, 0.0, 0.0),
        v("B2-gamma145", Raster::Fontdue, Pen::Int, Blend::Mid, 0.0, 0.0),
        v("C-gamma-subpx", Raster::Fontdue, Pen::Third, Blend::Linear, 0.0, 0.0),
        v("D-gamma-subpx-embolden", Raster::Fontdue, Pen::Third, Blend::Linear, 0.25, 0.0),
        v("D2-srgb-subpx-embolden", Raster::Fontdue, Pen::Third, Blend::Srgb, 0.25, 0.0),
        // The rasterizer axis (skrifa outlines through zeno).
        v("E-skrifa-unhinted-gamma-subpx", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Linear, 0.0, 0.0),
        v("E2-skrifa-unhinted-srgb-subpx", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Srgb, 0.0, 0.0),
        v("F-skrifa-light-gamma-subpx", Raster::Skrifa(Hint::AutoLight), Pen::Exact, Blend::Linear, 0.0, 0.0),
        v("F2-skrifa-light-srgb-subpx", Raster::Skrifa(Hint::AutoLight), Pen::Exact, Blend::Srgb, 0.0, 0.0),
        v("H-skrifa-interp-gamma-int", Raster::Skrifa(Hint::Interp), Pen::Int, Blend::Linear, 0.0, 0.0),
        // The candidates: the Mac's recipe on our substrate -- unhinted
        // outlines, gamma-space blend, exact positioning, an em-relative
        // outline dilation (three amounts to bracket the Mac's +18%).
        v("N0-skrifa-srgb-subpx-stroke015", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Srgb, 0.0, 0.015),
        v("N1-skrifa-srgb-subpx-stroke020", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Srgb, 0.0, 0.020),
        v("N2-skrifa-srgb-subpx-stroke030", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Srgb, 0.0, 0.030),
        v("N3-skrifa-srgb-subpx-stroke040", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Srgb, 0.0, 0.040),
        v("N4-skrifa-light-srgb-subpx-stroke030", Raster::Skrifa(Hint::AutoLight), Pen::Exact, Blend::Srgb, 0.0, 0.030),
        v("N5-skrifa-gamma-subpx-stroke030", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Linear, 0.0, 0.030),
        v("N6-skrifa-gamma145-subpx-stroke020", Raster::Skrifa(Hint::None), Pen::Exact, Blend::Mid, 0.0, 0.020),
        // FreeType references.
        v("I-ft-light-srgb-int", Raster::Ft("light"), Pen::Int, Blend::Srgb, 0.0, 0.0),
        v("J-ft-light-gamma-subpx", Raster::Ft("light"), Pen::Third, Blend::Linear, 0.0, 0.0),
        v("K-ft-lightdark-gamma-subpx", Raster::Ft("light-dark"), Pen::Third, Blend::Linear, 0.0, 0.0),
        v("L-ft-normal-gamma-int", Raster::Ft("normal"), Pen::Int, Blend::Linear, 0.0, 0.0),
        v("M-ft-nohint-srgb-subpx", Raster::Ft("nohint"), Pen::Third, Blend::Srgb, 0.0, 0.0),
    ]
}

// ---------------------------------------------------------------------------
// Compositing.

fn srgb_to_linear(c: u8) -> f32 {
    let s = c as f32 / 255.0;
    if s <= 0.04045 {
        s / 12.92
    } else {
        ((s + 0.055) / 1.055).powf(2.4)
    }
}

fn linear_to_srgb(l: f32) -> u8 {
    let s = if l <= 0.0031308 { 12.92 * l } else { 1.055 * l.powf(1.0 / 2.4) - 0.055 };
    (s.clamp(0.0, 1.0) * 255.0 + 0.5) as u8
}

/// cartoon::blend, verbatim (usr/lib/cartoon/src/lib.rs:311).
fn blend_srgb(bg: u32, fg: u32, a: u8) -> u32 {
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
    rb | g
}

fn blend_linear(bg: u32, fg: u32, a: u8, s2l: &[f32; 256]) -> u32 {
    if a == 0 {
        return bg;
    }
    if a == 255 {
        return fg;
    }
    let t = a as f32 / 255.0;
    let mut out = 0u32;
    for sh in [16, 8, 0] {
        let b = s2l[((bg >> sh) & 0xFF) as usize];
        let f = s2l[((fg >> sh) & 0xFF) as usize];
        out |= (linear_to_srgb(b + (f - b) * t) as u32) << sh;
    }
    out
}

struct Canvas {
    w: usize,
    h: usize,
    px: Vec<u32>,
}

impl Canvas {
    fn new(w: u32, h: u32, bg: u32) -> Canvas {
        Canvas { w: w as usize, h: h as usize, px: vec![bg; (w * h) as usize] }
    }

    fn paint(&mut self, m: &Mask, ox: i32, baseline: i32, color: u32, blend: Blend, s2l: &[f32; 256]) {
        for y in 0..m.h {
            let py = baseline - m.top + y as i32;
            if py < 0 || py as usize >= self.h {
                continue;
            }
            for x in 0..m.w {
                let px = ox + m.left + x as i32;
                if px < 0 || px as usize >= self.w {
                    continue;
                }
                let a = m.cov[y * m.w + x];
                let i = py as usize * self.w + px as usize;
                self.px[i] = match blend {
                    Blend::Srgb => blend_srgb(self.px[i], color, a),
                    Blend::Linear => blend_linear(self.px[i], color, a, s2l),
                    Blend::Mid => blend_mid(self.px[i], color, a),
                };
            }
        }
    }

    fn crop_zoom(&self, c: &Crop) -> Canvas {
        let (w, h) = (c.w as usize * c.zoom as usize, c.h as usize * c.zoom as usize);
        let mut out = Canvas { w, h, px: vec![0; w * h] };
        for y in 0..h {
            for x in 0..w {
                let sx = (c.x as usize + x / c.zoom as usize).min(self.w - 1);
                let sy = (c.y as usize + y / c.zoom as usize).min(self.h - 1);
                out.px[y * w + x] = self.px[sy * self.w + sx];
            }
        }
        out
    }

    fn write_png(&self, p: &Path) {
        let f = BufWriter::new(fs::File::create(p).expect("png create"));
        let mut enc = png::Encoder::new(f, self.w as u32, self.h as u32);
        enc.set_color(png::ColorType::Rgb);
        enc.set_depth(png::BitDepth::Eight);
        let mut wr = enc.write_header().expect("png header");
        let mut rgb = Vec::with_capacity(self.w * self.h * 3);
        for &p in &self.px {
            rgb.push((p >> 16) as u8);
            rgb.push((p >> 8) as u8);
            rgb.push(p as u8);
        }
        wr.write_image_data(&rgb).expect("png data");
    }

    fn read_ppm(p: &Path) -> Canvas {
        let b = fs::read(p).expect("ppm");
        let mut pos = 0;
        let mut fields = Vec::new();
        while fields.len() < 4 {
            while b[pos].is_ascii_whitespace() {
                pos += 1;
            }
            let s = pos;
            while !b[pos].is_ascii_whitespace() {
                pos += 1;
            }
            fields.push(std::str::from_utf8(&b[s..pos]).unwrap().to_string());
        }
        pos += 1;
        let (w, h): (usize, usize) = (fields[1].parse().unwrap(), fields[2].parse().unwrap());
        let mut px = Vec::with_capacity(w * h);
        for i in 0..w * h {
            let r = b[pos + i * 3] as u32;
            let g = b[pos + i * 3 + 1] as u32;
            let bl = b[pos + i * 3 + 2] as u32;
            px.push((r << 16) | (g << 8) | bl);
        }
        Canvas { w, h, px }
    }
}

// ---------------------------------------------------------------------------
// Measurement: per-row ink along a glyph's middle band, in linear light and in
// CIE L*. A straight stroke has constant per-row ink by construction, so the
// row-to-row roughness is the rendering's own texture -- the notches.

fn luminance(p: u32) -> f32 {
    0.2126 * srgb_to_linear((p >> 16) as u8) + 0.7152 * srgb_to_linear((p >> 8) as u8) + 0.0722 * srgb_to_linear(p as u8)
}

fn lstar(y: f32) -> f32 {
    let f = if y > 0.008856 { y.cbrt() } else { 7.787 * y + 16.0 / 116.0 };
    116.0 * f - 16.0
}

struct Measure {
    rough_lin: f32,
    rough_l: f32,
    cv_lin: f32,
    cv_l: f32,
    weight_l: f32,
    /// The stroke's CORE: per row, the darkest pixel's ink (L*, 0..1). A
    /// stroke whose core column stays dark in every row reads as solid; one
    /// whose core alternates between a dark pixel and two mid pixels reads
    /// as notched -- the thorn. Mean, minimum, and row-to-row roughness.
    core_mean: f32,
    core_min: f32,
    core_rough: f32,
    /// The anti-aliasing FRINGE: the partial pixels (0.05 < ink < 0.95 in
    /// L*) of the band. Their mean ink says how dark the fringe reads; their
    /// count per row how wide it is. A light, wide fringe hangs off a stem as
    /// the icicle; a dark, narrow one reads as the stem's edge.
    fringe_ink: f32,
    fringe_n: f32,
    rows: usize,
}

fn measure(c: &Canvas, bg: u32, fg: u32) -> Measure {
    let (ybg, yfg) = (luminance(bg), luminance(fg));
    let (lbg, lfg) = (lstar(ybg), lstar(yfg));
    let mut ink_lin = vec![0f32; c.h];
    let mut ink_l = vec![0f32; c.h];
    let mut core = vec![0f32; c.h];
    let mut fringe_sum = vec![0f32; c.h];
    let mut fringe_cnt = vec![0f32; c.h];
    for y in 0..c.h {
        for x in 0..c.w {
            let p = c.px[y * c.w + x];
            let yl = luminance(p);
            let il = ((lbg - lstar(yl)) / (lbg - lfg)).clamp(0.0, 1.0);
            ink_lin[y] += ((ybg - yl) / (ybg - yfg)).max(0.0);
            ink_l[y] += il;
            core[y] = core[y].max(il);
            if il > 0.05 && il < 0.95 {
                fringe_sum[y] += il;
                fringe_cnt[y] += 1.0;
            }
        }
    }
    let inked: Vec<usize> = (0..c.h).filter(|&y| ink_lin[y] > 0.05).collect();
    if inked.len() < 4 {
        return Measure { rough_lin: 0.0, rough_l: 0.0, cv_lin: 0.0, cv_l: 0.0, weight_l: 0.0, core_mean: 0.0, core_min: 0.0, core_rough: 0.0, fringe_ink: 0.0, fringe_n: 0.0, rows: 0 };
    }
    let (first, last) = (inked[0], *inked.last().unwrap());
    let span = (last - first) as f32;
    let y0 = first + (span * 0.3).round() as usize;
    let y1 = first + (span * 0.7).round() as usize;
    let band: Vec<usize> = (y0..=y1).collect();
    let stats = |ink: &[f32]| -> (f32, f32) {
        let n = band.len() as f32;
        let mean = band.iter().map(|&y| ink[y]).sum::<f32>() / n;
        let var = band.iter().map(|&y| (ink[y] - mean).powi(2)).sum::<f32>() / n;
        let rough = band.windows(2).map(|w| (ink[w[1]] - ink[w[0]]).abs()).sum::<f32>() / (n - 1.0).max(1.0);
        (rough / mean.max(1e-6), var.sqrt() / mean.max(1e-6))
    };
    let (rough_lin, cv_lin) = stats(&ink_lin);
    let (rough_l, cv_l) = stats(&ink_l);
    let weight_l = band.iter().map(|&y| ink_l[y]).sum::<f32>() / band.len() as f32;
    let core_mean = band.iter().map(|&y| core[y]).sum::<f32>() / band.len() as f32;
    let core_min = band.iter().map(|&y| core[y]).fold(1.0f32, f32::min);
    let core_rough = band.windows(2).map(|w| (core[w[1]] - core[w[0]]).abs()).sum::<f32>() / (band.len() as f32 - 1.0).max(1.0);
    let fs: f32 = band.iter().map(|&y| fringe_sum[y]).sum();
    let fc: f32 = band.iter().map(|&y| fringe_cnt[y]).sum();
    let fringe_ink = if fc > 0.0 { fs / fc } else { 0.0 };
    let fringe_n = fc / band.len() as f32;
    Measure { rough_lin, rough_l, cv_lin, cv_l, weight_l, core_mean, core_min, core_rough, fringe_ink, fringe_n, rows: band.len() }
}

// ---------------------------------------------------------------------------
// The driver.

struct Lab {
    out: PathBuf,
    fd: HashMap<&'static str, FdFace>,
    sk: HashMap<&'static str, SkFace>,
    ft: HashMap<String, HashMap<char, Mask>>,
    s2l: [f32; 256],
    metrics: fs::File,
    index: fs::File,
}

impl Lab {
    fn glyph(&mut self, raster: Raster, face: &'static str, px: f32, ch: char, k: u32, frac: f32, dil: f32, stroke_em: f32) -> Mask {
        let m = match raster {
            Raster::Fontdue => self.fd.get(face).unwrap().mask(ch, px, k),
            Raster::Skrifa(h) => self.sk.get(face).unwrap().mask(ch, px, h, frac, stroke_em * px),
            Raster::Ft(mode) => self.ft.get(&ft_key(face, px, mode, k)).unwrap().get(&ch).cloned().unwrap_or(Mask::empty(0.0)),
        };
        if dil > 0.0 {
            dilate(&m, dil)
        } else {
            m
        }
    }

    fn render(&mut self, sample: &Sample, v: &Variant) -> Canvas {
        let mut c = Canvas::new(sample.w, sample.h, SURFACE);
        for line in &sample.lines {
            let mut pen = line.x0;
            for r in &line.runs {
                for ch in r.text.chars() {
                    let (ox, k, frac) = match v.pen {
                        Pen::Int => (pen.round() as i32, 0, 0.0),
                        Pen::Third => {
                            let t = (pen * 3.0).round() as i32;
                            (t.div_euclid(3), t.rem_euclid(3) as u32, t.rem_euclid(3) as f32 / 3.0)
                        }
                        Pen::Exact => {
                            let o = pen.floor();
                            (o as i32, 0, pen - o)
                        }
                    };
                    let m = self.glyph(v.raster, r.face, r.px, ch, k, frac, v.dilate, v.stroke_em);
                    c.paint(&m, ox, line.baseline, r.color, v.blend, &self.s2l);
                    pen += match v.pen {
                        Pen::Int => m.adv_int as f32,
                        _ => m.adv,
                    };
                }
            }
        }
        c
    }

    fn emit(&mut self, scale_tag: &str, sample: &Sample, vname: &str, c: &Canvas) {
        let dir = self.out.join(scale_tag).join(&sample.name);
        fs::create_dir_all(&dir).unwrap();
        let full = dir.join(format!("{vname}.png"));
        c.write_png(&full);
        let mut crops = Vec::new();
        for cr in &sample.crops {
            let p = dir.join(format!("{vname}.crop-{}.png", cr.name));
            c.crop_zoom(cr).write_png(&p);
            crops.push(format!("{}={}", cr.name, p.strip_prefix(&self.out).unwrap().display()));
        }
        let m = measure(c, SURFACE, FG);
        writeln!(
            self.metrics,
            "{scale_tag}\t{}\t{vname}\t{:.4}\t{:.4}\t{:.4}\t{:.4}\t{:.3}\t{:.3}\t{:.3}\t{:.4}\t{:.3}\t{:.2}\t{}",
            sample.name, m.rough_lin, m.rough_l, m.cv_lin, m.cv_l, m.weight_l, m.core_mean, m.core_min, m.core_rough, m.fringe_ink, m.fringe_n, m.rows
        )
        .unwrap();
        writeln!(self.index, "{scale_tag}\t{}\t{vname}\t{}\t{}", sample.name, full.strip_prefix(&self.out).unwrap().display(), crops.join(",")).unwrap();
    }

    fn coretext(&mut self, scale_tag: &str, sample: &Sample, smooth: bool, subpx: bool) {
        let spec_dir = self.out.join("spec");
        fs::create_dir_all(&spec_dir).unwrap();
        let mut spec = String::new();
        spec += &format!("size {} {}\nbg {:06x}\n", sample.w, sample.h, SURFACE);
        let mut faces = BTreeSet::new();
        for l in &sample.lines {
            for r in &l.runs {
                faces.insert(r.face);
            }
        }
        for f in faces {
            spec += &format!("face {f} {}\n", face_path(f));
        }
        for l in &sample.lines {
            spec += &format!("line {} {}\n", l.baseline, l.x0);
            for r in &l.runs {
                spec += &format!("run {} {} {:06x} {}\n", r.face, r.px, r.color, r.text);
            }
        }
        let vname = format!("CT-{}{}", if smooth { "smooth" } else { "nosmooth" }, if subpx { "-subpx" } else { "-quant" });
        let spec_path = spec_dir.join(format!("{scale_tag}-{}-{vname}.spec", sample.name));
        fs::write(&spec_path, spec).unwrap();
        let ppm = spec_dir.join(format!("{scale_tag}-{}-{vname}.ppm", sample.name));
        let st = Command::new("./ct")
            .args([spec_path.to_str().unwrap(), ppm.to_str().unwrap(), &format!("smooth={}", smooth as u8), &format!("subpx={}", subpx as u8), "kern=0"])
            .status()
            .expect("ct");
        assert!(st.success(), "ct failed");
        let c = Canvas::read_ppm(&ppm);
        self.emit(scale_tag, sample, &vname, &c);
    }
}

fn main() {
    let out = PathBuf::from("out");
    let _ = fs::remove_dir_all(&out);
    fs::create_dir_all(&out).unwrap();
    let mut s2l = [0f32; 256];
    for (i, v) in s2l.iter_mut().enumerate() {
        *v = srgb_to_linear(i as u8);
    }
    let mut lab = Lab {
        out: out.clone(),
        fd: HashMap::new(),
        sk: HashMap::new(),
        ft: HashMap::new(),
        s2l,
        metrics: fs::File::create(out.join("metrics.tsv")).unwrap(),
        index: fs::File::create(out.join("index.tsv")).unwrap(),
    };
    writeln!(lab.metrics, "scale\tsample\tvariant\trough_lin\trough_L\tcv_lin\tcv_L\tweight_L\tcore_mean\tcore_min\tcore_rough\tfringe_ink\tfringe_n\trows").unwrap();
    for face in ["text", "bold", "ti", "hi", "mono"] {
        lab.fd.insert(face, FdFace::load(&face_path(face)));
        lab.sk.insert(face, SkFace::load(&face_path(face)));
    }
    let ft_dir = out.join("ft");
    fs::create_dir_all(&ft_dir).unwrap();
    let vs = variants();
    for (scale, tag) in [(1.0f32, "1x"), (2.0f32, "2x")] {
        let ss = samples(scale);
        // The FreeType masks every (face, px, mode, shift) needs.
        let mut chars: BTreeMap<(&'static str, u32), BTreeSet<char>> = BTreeMap::new();
        for s in &ss {
            for l in &s.lines {
                for r in &l.runs {
                    chars.entry((r.face, (r.px * 100.0).round() as u32)).or_default().extend(r.text.chars());
                }
            }
        }
        for ((face, pxk), set) in &chars {
            let px = *pxk as f32 / 100.0;
            let text: String = set.iter().collect();
            for mode in ["light", "light-dark", "normal", "nohint"] {
                for k in 0..3u32 {
                    let masks = ft_dump(&ft_dir, face, px, mode, k, &text);
                    lab.ft.insert(ft_key(face, px, mode, k), masks);
                }
            }
        }
        for s in &ss {
            for v in &vs {
                let c = lab.render(s, v);
                lab.emit(tag, s, v.name, &c);
            }
            for (smooth, subpx) in [(true, true), (false, true), (true, false), (false, false)] {
                lab.coretext(tag, s, smooth, subpx);
            }
        }
    }
    eprintln!("done: {}", out.display());
}
