//! fit -- find the rendering parameters that reproduce a reference glyph
//! raster (a browser / CoreText capture) with our own substrate: skrifa
//! outlines through zeno, an outline stroke, a blend space, a coverage
//! curve, a fractional pen. Random search + local refinement over the
//! continuous parameters, exhaustive over the discrete ones.
//!
//!   fit <target.png> <ttf> <char> <px> <out-prefix> [samples]
//!
//! The target is an RGB(A) PNG of the glyph on a light ground; ink is read
//! off the green channel against the measured ground and the measured
//! darkest pixel. The objective is the RMS ink error over the target's
//! bbox (+2 px margin) at the best integer alignment.
use std::fs;
use std::io::BufWriter;

use skrifa::instance::{LocationRef, Size};
use skrifa::outline::{DrawSettings, Engine, HintingInstance, HintingOptions, OutlinePen, SmoothMode, Target};
use skrifa::{FontRef, MetadataProvider};

struct ZPen {
    cmds: Vec<zeno::Command>,
}
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

#[derive(Clone, Copy, Debug, PartialEq)]
enum Hint {
    None,
    Light,
    Interp,
}

#[derive(Clone, Copy, Debug)]
struct Params {
    hint: Hint,
    /// Outline stroke width as a fraction of the em (dilation per side = half).
    stroke_em: f32,
    /// The blend space exponent: 1.0 = lerp of the encoded values (the
    /// as-built and CoreText), 2.2 = close to linear light.
    gamma: f32,
    /// Coverage curve: a' = a^k (k < 1 darkens the fringe).
    k: f32,
    /// Fractional pen offset in px (x right, y down).
    dx: f32,
    dy: f32,
}

/// A glyph mask: coverage bytes + placement (left, top in image rows).
struct Mask {
    w: usize,
    h: usize,
    left: i32,
    top: i32,
    cov: Vec<u8>,
}

/// The outline commands for a glyph at a size under a hinting mode; cached
/// per (hint) since the pen offset and stroke are applied by zeno.
fn outline(font: &FontRef, ch: char, px: f32, hint: Hint) -> Vec<zeno::Command> {
    let size = Size::new(px);
    let gid = font.charmap().map(ch).expect("glyph");
    let outlines = font.outline_glyphs();
    let glyph = outlines.get(gid).expect("outline");
    let mut pen = ZPen { cmds: Vec::new() };
    let opts = |engine, mode| HintingOptions {
        engine,
        target: Target::Smooth { mode, symmetric_rendering: false, preserve_linear_metrics: true },
    };
    match hint {
        Hint::None => {
            glyph.draw(DrawSettings::unhinted(size, LocationRef::default()), &mut pen).expect("draw");
        }
        Hint::Light => {
            let h = HintingInstance::new(&outlines, size, LocationRef::default(), opts(Engine::Auto(None), SmoothMode::Light)).expect("hinter");
            glyph.draw(DrawSettings::hinted(&h, false), &mut pen).expect("draw");
        }
        Hint::Interp => {
            let h = HintingInstance::new(&outlines, size, LocationRef::default(), opts(Engine::Interpreter, SmoothMode::Normal)).expect("hinter");
            glyph.draw(DrawSettings::hinted(&h, false), &mut pen).expect("draw");
        }
    }
    pen.cmds
}

fn render(cmds: &[zeno::Command], px: f32, p: &Params) -> Mask {
    let run = |style: zeno::Style| -> (Vec<u8>, zeno::Placement) {
        let mut m = zeno::Mask::new(cmds);
        m.origin(zeno::Origin::TopLeft);
        m.offset(zeno::Vector::new(p.dx, p.dy));
        m.style(style);
        m.render()
    };
    let (fill, fp) = run(zeno::Style::Fill(zeno::Fill::NonZero));
    let mut mask = Mask { w: fp.width as usize, h: fp.height as usize, left: fp.left, top: fp.top, cov: fill };
    let sw = p.stroke_em * px;
    if sw > 0.002 {
        let (sk, sp) = run(zeno::Style::Stroke(zeno::Stroke::new(sw)));
        let (w, h) = (sp.width as usize, sp.height as usize);
        let mut cov = sk;
        for y in 0..mask.h {
            for x in 0..mask.w {
                let gx = x as i32 + mask.left - sp.left;
                let gy = y as i32 + mask.top - sp.top;
                if gx >= 0 && gy >= 0 && (gx as usize) < w && (gy as usize) < h {
                    let i = gy as usize * w + gx as usize;
                    let f = mask.cov[y * mask.w + x] as f32 / 255.0;
                    let s = cov[i] as f32 / 255.0;
                    cov[i] = ((f + s - f * s) * 255.0 + 0.5) as u8;
                }
            }
        }
        mask = Mask { w, h, left: sp.left, top: sp.top, cov };
    }
    mask
}

/// The displayed ink of a coverage value under the blend model, in the
/// same units as the target's ink (0 = ground, 1 = full fg), measured on
/// the green channel: the blend of bg and fg in the gamma space, then the
/// ink read back linearly in the encoded channel as the target reader does.
fn ink_of(cov: u8, p: &Params, bg: f32, fg: f32) -> f32 {
    let a = (cov as f32 / 255.0).powf(p.k);
    let g = p.gamma;
    let b = (bg / 255.0).powf(g);
    let f = (fg / 255.0).powf(g);
    let v = (b + (f - b) * a).powf(1.0 / g) * 255.0;
    ((bg - v) / (bg - fg)).clamp(0.0, 1.0)
}

struct Reference {
    w: usize,
    h: usize,
    ink: Vec<f32>,
    bg: f32,
    fg: f32,
    // bbox of ink
    x0: i32,
    y0: i32,
    x1: i32,
    y1: i32,
}

fn load_target(path: &str) -> Reference {
    let dec = png::Decoder::new(std::io::BufReader::new(fs::File::open(path).expect("target")));
    let mut reader = dec.read_info().expect("png");
    let mut buf = vec![0; reader.output_buffer_size().expect("size")];
    let info = reader.next_frame(&mut buf).expect("frame");
    let (w, h) = (info.width as usize, info.height as usize);
    let ch = info.color_type.samples();
    let gch = if ch >= 3 { 1 } else { 0 };
    let mut g = vec![0f32; w * h];
    for i in 0..w * h {
        g[i] = buf[i * ch + gch] as f32;
    }
    let bg = g.iter().cloned().fold(0.0, f32::max);
    let fg = g.iter().cloned().fold(255.0, f32::min);
    let ink: Vec<f32> = g.iter().map(|&v| ((bg - v) / (bg - fg)).clamp(0.0, 1.0)).collect();
    let (mut x0, mut y0, mut x1, mut y1) = (w as i32, h as i32, -1, -1);
    for y in 0..h {
        for x in 0..w {
            if ink[y * w + x] > 0.06 {
                x0 = x0.min(x as i32);
                y0 = y0.min(y as i32);
                x1 = x1.max(x as i32);
                y1 = y1.max(y as i32);
            }
        }
    }
    Reference { w, h, ink, bg, fg, x0, y0, x1, y1 }
}

/// RMS ink error at the best integer alignment (the candidate's bbox is
/// slid over the target's, +-4 px), plus the shift found.
fn score(t: &Reference, m: &Mask, p: &Params) -> (f32, i32, i32) {
    // candidate ink image + its bbox
    let mut ci = vec![0f32; m.w * m.h];
    for i in 0..m.w * m.h {
        ci[i] = ink_of(m.cov[i], p, t.bg, t.fg);
    }
    let (mut cx0, mut cy0, mut cx1, mut cy1) = (m.w as i32, m.h as i32, -1, -1);
    for y in 0..m.h {
        for x in 0..m.w {
            if ci[y * m.w + x] > 0.06 {
                cx0 = cx0.min(x as i32);
                cy0 = cy0.min(y as i32);
                cx1 = cx1.max(x as i32);
                cy1 = cy1.max(y as i32);
            }
        }
    }
    if cx1 < 0 {
        return (1.0, 0, 0);
    }
    let mut best = (f32::MAX, 0, 0);
    for dy in -4..=4 {
        for dx in -4..=4 {
            let mut se = 0.0f32;
            let mut n = 0usize;
            for y in (t.y0 - 3)..=(t.y1 + 3) {
                for x in (t.x0 - 3)..=(t.x1 + 3) {
                    let tv = if x >= 0 && y >= 0 && (x as usize) < t.w && (y as usize) < t.h { t.ink[y as usize * t.w + x as usize] } else { 0.0 };
                    let cx = x - t.x0 + cx0 + dx;
                    let cy = y - t.y0 + cy0 + dy;
                    let cv = if cx >= 0 && cy >= 0 && (cx as usize) < m.w && (cy as usize) < m.h { ci[cy as usize * m.w + cx as usize] } else { 0.0 };
                    se += (tv - cv) * (tv - cv);
                    n += 1;
                }
            }
            let rms = (se / n as f32).sqrt();
            if rms < best.0 {
                best = (rms, dx, dy);
            }
        }
    }
    best
}

struct Rng(u64);
impl Rng {
    fn next(&mut self) -> f32 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        (self.0 >> 11) as f32 / (1u64 << 53) as f32
    }
    fn range(&mut self, lo: f32, hi: f32) -> f32 {
        lo + (hi - lo) * self.next()
    }
}

fn write_png(path: &str, w: usize, h: usize, rgb: &[u8]) {
    let f = BufWriter::new(fs::File::create(path).expect("png create"));
    let mut enc = png::Encoder::new(f, w as u32, h as u32);
    enc.set_color(png::ColorType::Rgb);
    enc.set_depth(png::BitDepth::Eight);
    enc.write_header().expect("hdr").write_image_data(rgb).expect("data");
}

/// Side-by-side: target | candidate | abs difference (red = candidate
/// darker, blue = target darker), each magnified `z`x, on the target's
/// bbox frame.
fn composite(t: &Reference, m: &Mask, p: &Params, dx: i32, dy: i32, z: usize, path: &str) {
    let fw = (t.x1 - t.x0 + 7) as usize;
    let fh = (t.y1 - t.y0 + 7) as usize;
    let mut ci = vec![0f32; m.w * m.h];
    for i in 0..m.w * m.h {
        ci[i] = ink_of(m.cov[i], p, t.bg, t.fg);
    }
    let (mut cx0, mut cy0) = (m.w as i32, m.h as i32);
    for y in 0..m.h {
        for x in 0..m.w {
            if ci[y * m.w + x] > 0.06 {
                cx0 = cx0.min(x as i32);
                cy0 = cy0.min(y as i32);
            }
        }
    }
    let gap = 2;
    let W = fw * 3 + gap * 2;
    let mut img = vec![0u8; W * z * fh * z * 3];
    let put = |img: &mut Vec<u8>, px: usize, py: usize, r: u8, g: u8, b: u8| {
        for yy in 0..z {
            for xx in 0..z {
                let i = ((py * z + yy) * W * z + px * z + xx) * 3;
                img[i] = r;
                img[i + 1] = g;
                img[i + 2] = b;
            }
        }
    };
    for fy in 0..fh {
        for fx in 0..fw {
            let x = t.x0 - 3 + fx as i32;
            let y = t.y0 - 3 + fy as i32;
            let tv = if x >= 0 && y >= 0 && (x as usize) < t.w && (y as usize) < t.h { t.ink[y as usize * t.w + x as usize] } else { 0.0 };
            let cx = x - t.x0 + cx0 + dx;
            let cy = y - t.y0 + cy0 + dy;
            let cv = if cx >= 0 && cy >= 0 && (cx as usize) < m.w && (cy as usize) < m.h { ci[cy as usize * m.w + cx as usize] } else { 0.0 };
            let shade = |v: f32| -> (u8, u8, u8) {
                // Daylight ground to ink, lerp in encoded space for display
                let l = |a: u8, b: u8| (a as f32 + (b as f32 - a as f32) * v) as u8;
                (l(0xF2, 0x1A), l(0xEB, 0x12), l(0xE0, 0x0A))
            };
            let (r, g, b) = shade(tv);
            put(&mut img, fx, fy, r, g, b);
            let (r, g, b) = shade(cv);
            put(&mut img, fx + fw + gap, fy, r, g, b);
            let d = cv - tv;
            let mag = (d.abs() * 2.0).min(1.0);
            let (r, g, b) = if d > 0.0 {
                (0xF2, (0xEB as f32 * (1.0 - mag)) as u8, (0xE0 as f32 * (1.0 - mag)) as u8)
            } else {
                ((0xF2 as f32 * (1.0 - mag)) as u8, (0xEB as f32 * (1.0 - mag)) as u8, 0xE0)
            };
            put(&mut img, fx + 2 * (fw + gap), fy, r, g, b);
        }
    }
    // the gaps: a border colour
    for fy in 0..fh {
        for g in 0..gap {
            put(&mut img, fw + g, fy, 0xA8, 0x98, 0x80);
            put(&mut img, 2 * fw + gap + g, fy, 0xA8, 0x98, 0x80);
        }
    }
    write_png(path, W * z, fh * z, &img);
}

fn main() {
    let a: Vec<String> = std::env::args().collect();
    if a.len() < 6 {
        eprintln!("usage: fit <target.png> <ttf> <char> <px> <out-prefix> [samples]");
        std::process::exit(2);
    }
    let t = load_target(&a[1]);
    let data = fs::read(&a[2]).expect("ttf");
    let font = FontRef::new(&data).expect("font");
    let ch = a[3].chars().next().unwrap();
    let px: f32 = a[4].parse().unwrap();
    let prefix = a[5].clone();
    let samples: usize = a.get(6).map(|s| s.parse().unwrap()).unwrap_or(4000);
    // Optional constraints: fix a parameter to what a pipeline can do
    // (`gamma=1` keeps cartoon::blend; `stroke=0` needs no outline).
    let mut fix_gamma: Option<f32> = None;
    let mut fix_stroke: Option<f32> = None;
    let mut fix_k: Option<f32> = None;
    for arg in a.iter().skip(7) {
        if let Some(v) = arg.strip_prefix("gamma=") {
            fix_gamma = Some(v.parse().unwrap());
        } else if let Some(v) = arg.strip_prefix("stroke=") {
            fix_stroke = Some(v.parse().unwrap());
        } else if let Some(v) = arg.strip_prefix("k=") {
            fix_k = Some(v.parse().unwrap());
        }
    }
    let clampp = |p: Params| Params {
        gamma: fix_gamma.unwrap_or(p.gamma),
        stroke_em: fix_stroke.unwrap_or(p.stroke_em),
        k: fix_k.unwrap_or(p.k),
        ..p
    };
    eprintln!("target {}x{} bg {} fg {} bbox ({},{})-({},{})", t.w, t.h, t.bg, t.fg, t.x0, t.y0, t.x1, t.y1);

    let hints = [Hint::None, Hint::Light, Hint::Interp];
    let outlines: Vec<Vec<zeno::Command>> = hints.iter().map(|&h| outline(&font, ch, px, h)).collect();

    // Baselines: the named recipes at their phase-0 pen, for the record.
    let named = [
        ("as-built (stroke 0, gamma 1, k 1)", Params { hint: Hint::None, stroke_em: 0.0, gamma: 1.0, k: 1.0, dx: 0.0, dy: 0.0 }),
        ("N0 (stroke 0.015, gamma 1, k 1)", Params { hint: Hint::None, stroke_em: 0.015, gamma: 1.0, k: 1.0, dx: 0.0, dy: 0.0 }),
        ("linear blend (stroke 0, gamma 2.2)", Params { hint: Hint::None, stroke_em: 0.0, gamma: 2.2, k: 1.0, dx: 0.0, dy: 0.0 }),
    ];
    for (name, p) in &named {
        // let the pen phase float for a fair baseline
        let mut best = (f32::MAX, *p, 0, 0);
        for iy in 0..4 {
            for ix in 0..8 {
                let q = Params { dx: ix as f32 / 8.0, dy: iy as f32 / 4.0, ..*p };
                let m = render(&outlines[0], px, &q);
                let (s, ddx, ddy) = score(&t, &m, &q);
                if s < best.0 {
                    best = (s, q, ddx, ddy);
                }
            }
        }
        println!("baseline {:<40} rms {:.4} (dx {:.3} dy {:.2})", name, best.0, best.1.dx, best.1.dy);
    }

    let mut rng = Rng(0x9E3779B97F4A7C15);
    let mut best: Vec<(f32, Params, i32, i32)> = Vec::new();
    let lo = Params { hint: Hint::None, stroke_em: 0.0, gamma: 0.8, k: 0.5, dx: 0.0, dy: 0.0 };
    let hi = Params { hint: Hint::None, stroke_em: 0.06, gamma: 2.6, k: 1.6, dx: 1.0, dy: 1.0 };
    for (hi_i, &hint) in hints.iter().enumerate() {
        for _ in 0..samples {
            let p = clampp(Params {
                hint,
                stroke_em: rng.range(lo.stroke_em, hi.stroke_em),
                gamma: rng.range(lo.gamma, hi.gamma),
                k: rng.range(lo.k, hi.k),
                dx: rng.range(lo.dx, hi.dx),
                dy: rng.range(lo.dy, hi.dy),
            });
            let m = render(&outlines[hi_i], px, &p);
            let (s, dx, dy) = score(&t, &m, &p);
            best.push((s, p, dx, dy));
        }
    }
    best.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    best.truncate(12);
    // Local refinement of the top 12: shrinking Gaussian-ish perturbations.
    for slot in best.iter_mut() {
        let mut cur = *slot;
        let hi_i = hints.iter().position(|&h| h == cur.1.hint).unwrap();
        let mut step = 0.25f32;
        for _round in 0..8 {
            let mut improved = false;
            for _ in 0..60 {
                let p = clampp(Params {
                    hint: cur.1.hint,
                    stroke_em: (cur.1.stroke_em + rng.range(-step, step) * 0.06).clamp(0.0, 0.08),
                    gamma: (cur.1.gamma + rng.range(-step, step) * 1.8).clamp(0.6, 3.0),
                    k: (cur.1.k + rng.range(-step, step) * 1.1).clamp(0.3, 2.0),
                    dx: (cur.1.dx + rng.range(-step, step)).rem_euclid(1.0),
                    dy: (cur.1.dy + rng.range(-step, step)).rem_euclid(1.0),
                });
                let m = render(&outlines[hi_i], px, &p);
                let (s, dx, dy) = score(&t, &m, &p);
                if s < cur.0 {
                    cur = (s, p, dx, dy);
                    improved = true;
                }
            }
            if !improved {
                step *= 0.5;
            }
        }
        *slot = cur;
    }
    best.sort_by(|a, b| a.0.partial_cmp(&b.0).unwrap());
    println!("\nrank  rms     hint    stroke_em  gamma  k      dx     dy");
    for (i, (s, p, _, _)) in best.iter().enumerate() {
        println!("{:>4}  {:.4}  {:<7}  {:.4}     {:.3}  {:.3}  {:.3}  {:.3}", i, s, format!("{:?}", p.hint), p.stroke_em, p.gamma, p.k, p.dx, p.dy);
    }
    let (s, p, dx, dy) = best[0];
    let hi_i = hints.iter().position(|&h| h == p.hint).unwrap();
    let m = render(&outlines[hi_i], px, &p);
    composite(&t, &m, &p, dx, dy, 8, &format!("{prefix}-best.png"));
    // And the as-built + N0 composites at their best phases, for the eye.
    for (tag, q) in [("asbuilt", named[0].1), ("n0", named[1].1)] {
        let mut b = (f32::MAX, q, 0, 0);
        for iy in 0..4 {
            for ix in 0..8 {
                let r = Params { dx: ix as f32 / 8.0, dy: iy as f32 / 4.0, ..q };
                let mm = render(&outlines[0], px, &r);
                let (sc, ddx, ddy) = score(&t, &mm, &r);
                if sc < b.0 {
                    b = (sc, r, ddx, ddy);
                }
            }
        }
        let mm = render(&outlines[0], px, &b.1);
        composite(&t, &mm, &b.1, b.2, b.3, 8, &format!("{prefix}-{tag}.png"));
    }
    println!("\nbest rms {:.4}: {:?}; composites {prefix}-best.png / -asbuilt.png / -n0.png", s, p);
}
