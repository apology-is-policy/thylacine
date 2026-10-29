//! Cursor geometry is shared by software composition and optional device planes.
//! Coordinates and hotspots are independent of any GPU or window-system ABI.

use crate::scale;

pub const LOGICAL_SIZE: u32 = 24;
pub const MAX_SIZE: usize = 48;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum Shape {
    #[default]
    Arrow,
    Text,
    Link,
    ResizeHorizontal,
    ResizeVertical,
}

impl Shape {
    pub fn from_code(code: u8) -> Option<Self> {
        Some(match code {
            0 => Self::Arrow,
            1 => Self::Text,
            2 => Self::Link,
            3 => Self::ResizeHorizontal,
            4 => Self::ResizeVertical,
            _ => return None,
        })
    }

    pub fn parse(name: &str) -> Option<Self> {
        match name {
            "arrow" => Some(Self::Arrow),
            "text" => Some(Self::Text),
            "link" => Some(Self::Link),
            "resize-h" => Some(Self::ResizeHorizontal),
            "resize-v" => Some(Self::ResizeVertical),
            _ => None,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            Self::Arrow => "arrow",
            Self::Text => "text",
            Self::Link => "link",
            Self::ResizeHorizontal => "resize-h",
            Self::ResizeVertical => "resize-v",
        }
    }

    pub fn hotspot(self, pct: u16) -> Option<(u32, u32)> {
        if !scale::is_valid_pct(pct) {
            return None;
        }
        let (x, y) = match self {
            Self::Arrow => (2, 2),
            Self::Link => (9, 2),
            _ => (12, 12),
        };
        Some((scale::ipx(x, pct) as u32, scale::ipx(y, pct) as u32))
    }
}

/// Raster dimensions and clipped display placement, with source offsets retained.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Placement {
    pub x: u32,
    pub y: u32,
    pub w: u32,
    pub h: u32,
    pub sx: u32,
    pub sy: u32,
}

pub fn size(pct: u16) -> Option<usize> {
    scale::is_valid_pct(pct).then(|| scale::ipx(LOGICAL_SIZE as i32, pct) as usize)
}

pub fn place(shape: Shape, pct: u16, x: u32, y: u32, w: u32, h: u32) -> Option<Placement> {
    let n = size(pct)? as i64;
    let (hx, hy) = shape.hotspot(pct)?;
    if w == 0 || h == 0 {
        return None;
    }
    let left = i64::from(x.min(w - 1)) - i64::from(hx);
    let top = i64::from(y.min(h - 1)) - i64::from(hy);
    let x0 = left.max(0);
    let y0 = top.max(0);
    let x1 = (left + n).min(i64::from(w));
    let y1 = (top + n).min(i64::from(h));
    if x1 <= x0 || y1 <= y0 {
        return None;
    }
    Some(Placement {
        x: x0 as u32,
        y: y0 as u32,
        w: (x1 - x0) as u32,
        h: (y1 - y0) as u32,
        sx: (x0 - left) as u32,
        sy: (y0 - top) as u32,
    })
}

fn polygon(x: f32, y: f32, points: &[(f32, f32)]) -> bool {
    let mut inside = false;
    let mut j = points.len() - 1;
    for i in 0..points.len() {
        let (xi, yi) = points[i];
        let (xj, yj) = points[j];
        if (yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi {
            inside = !inside;
        }
        j = i;
    }
    inside
}

fn body(shape: Shape, x: f32, y: f32) -> bool {
    match shape {
        Shape::Arrow => polygon(
            x,
            y,
            &[
                (2., 2.),
                (2., 19.),
                (7., 14.),
                (11., 22.),
                (14., 20.5),
                (10., 13.),
                (17., 13.),
            ],
        ),
        Shape::Link => polygon(
            x,
            y,
            &[
                (7., 20.),
                (3., 13.),
                (3., 11.),
                (5., 10.),
                (8., 13.),
                (8., 3.),
                (9., 2.),
                (10., 3.),
                (10., 10.),
                (12., 8.),
                (14., 10.),
                (16., 9.),
                (18., 11.),
                (20., 11.),
                (21., 13.),
                (20., 20.),
                (18., 22.),
                (9., 22.),
            ],
        ),
        Shape::Text => {
            ((11.25..=12.75).contains(&x) && (3.0..=21.0).contains(&y))
                || ((8.0..=16.0).contains(&x)
                    && ((3.0..=4.5).contains(&y) || (19.5..=21.0).contains(&y)))
        }
        Shape::ResizeHorizontal | Shape::ResizeVertical => {
            let (x, y) = if shape == Shape::ResizeVertical {
                (y, x)
            } else {
                (x, y)
            };
            polygon(
                x,
                y,
                &[
                    (2., 12.),
                    (7., 7.),
                    (7., 10.5),
                    (17., 10.5),
                    (17., 7.),
                    (22., 12.),
                    (17., 17.),
                    (17., 13.5),
                    (7., 13.5),
                    (7., 17.),
                ],
            )
        }
    }
}

/// Premultiplied ARGB; output outside the scaled raster remains untouched.
pub fn raster(shape: Shape, pct: u16, output: &mut [u32], stride: usize) -> bool {
    let Some(n) = size(pct) else { return false };
    if stride < n || stride.checked_mul(n).is_none_or(|need| output.len() < need) {
        return false;
    }
    for y in 0..n {
        for x in 0..n {
            let mut rgba = [0u32; 4];
            for sy in 0..4 {
                for sx in 0..4 {
                    let px = (x as f32 + (sx as f32 + 0.5) / 4.) * 100. / pct as f32;
                    let py = (y as f32 + (sy as f32 + 0.5) / 4.) * 100. / pct as f32;
                    let inner = body(shape, px, py);
                    let outline = inner
                        || [
                            (-0.85, 0.),
                            (0.85, 0.),
                            (0., -0.85),
                            (0., 0.85),
                            (-0.6, -0.6),
                            (-0.6, 0.6),
                            (0.6, -0.6),
                            (0.6, 0.6),
                        ]
                        .iter()
                        .any(|&(dx, dy)| body(shape, px + dx, py + dy));
                    if outline {
                        let color = if inner { [232, 235, 224] } else { [16, 22, 19] };
                        rgba[0] += 255;
                        for c in 0..3 {
                            rgba[c + 1] += color[c];
                        }
                    }
                }
            }
            output[y * stride + x] = ((rgba[0] / 16) << 24)
                | ((rgba[1] / 16) << 16)
                | ((rgba[2] / 16) << 8)
                | (rgba[3] / 16);
        }
    }
    true
}

/// Blend onto a cursor-free scene copy; callers retain the untouched scene.
pub fn composite(
    scene: &mut [u32],
    width: usize,
    height: usize,
    pixels: &[u32],
    stride: usize,
    p: Placement,
) -> bool {
    if width.checked_mul(height).is_none_or(|n| scene.len() < n)
        || u64::from(p.x) + u64::from(p.w) > width as u64
        || u64::from(p.y) + u64::from(p.h) > height as u64
        || u64::from(p.sx) + u64::from(p.w) > stride as u64
        || (p.sy as usize)
            .checked_add(p.h as usize)
            .and_then(|h| h.checked_mul(stride))
            .is_none_or(|n| pixels.len() < n)
    {
        return false;
    }
    for y in 0..p.h as usize {
        for x in 0..p.w as usize {
            let source = pixels[(p.sy as usize + y) * stride + p.sx as usize + x];
            let dest = &mut scene[(p.y as usize + y) * width + p.x as usize + x];
            let inverse = 255 - (source >> 24);
            let mut result = 0xff00_0000;
            for shift in [0, 8, 16] {
                let channel =
                    ((source >> shift) & 255) + (((*dest >> shift) & 255) * inverse + 127) / 255;
                result |= channel.min(255) << shift;
            }
            *dest = result;
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn scaled_shapes_are_bounded_and_have_visible_contrast() {
        for shape in [
            Shape::Arrow,
            Shape::Text,
            Shape::Link,
            Shape::ResizeHorizontal,
            Shape::ResizeVertical,
        ] {
            for pct in [100, 125, 150, 175, 200] {
                let n = size(pct).unwrap();
                let mut pixels = vec![0; MAX_SIZE * MAX_SIZE];
                assert!(raster(shape, pct, &mut pixels, MAX_SIZE));
                assert!(pixels.iter().any(|p| p & 0x00ff_ffff == 0x00e8_ebe0));
                // Subpixel outlines need not contain an exactly solid dark
                // pixel at every scale. Require substantial dark coverage.
                assert!(
                    pixels
                        .iter()
                        .any(|p| (p >> 24) >= 128 && ((p >> 16) & 255) < (p >> 24) / 2),
                    "{shape:?} at {pct}"
                );
                assert!(pixels
                    .iter()
                    .enumerate()
                    .all(|(i, &p)| (i / MAX_SIZE < n && i % MAX_SIZE < n) || p == 0));
                assert_eq!(Shape::parse(shape.name()), Some(shape));
            }
        }
    }

    #[test]
    fn clipping_keeps_the_hotspot_and_source_aligned() {
        assert_eq!(
            place(Shape::Arrow, 100, 0, 0, 100, 100),
            Some(Placement {
                x: 0,
                y: 0,
                w: 22,
                h: 22,
                sx: 2,
                sy: 2,
            })
        );
        let p = place(Shape::Text, 200, u32::MAX, u32::MAX, 100, 100).unwrap();
        assert_eq!((p.x, p.y, p.w, p.h), (75, 75, 25, 25));
        assert!(place(Shape::Arrow, 100, 1, 1, 0, 20).is_none());
    }

    #[test]
    fn software_cursor_uses_each_new_scene_without_a_saved_backdrop() {
        let mut pixels = vec![0; 24 * 24];
        assert!(raster(Shape::Arrow, 100, &mut pixels, 24));
        let original = vec![0xff12_3456; 64 * 64];
        let mut first = original.clone();
        let p = place(Shape::Arrow, 100, 10, 10, 64, 64).unwrap();
        assert!(composite(&mut first, 64, 64, &pixels, 24, p));
        assert_ne!(first, original);
        let mut next = vec![0xff98_7654; 64 * 64];
        let p = place(Shape::Arrow, 100, 40, 40, 64, 64).unwrap();
        assert!(composite(&mut next, 64, 64, &pixels, 24, p));
        assert_eq!(next[10 * 64 + 10], 0xff98_7654);
        assert!(original.iter().all(|p| *p == 0xff12_3456));
    }

    #[test]
    fn malformed_geometry_does_not_modify_output() {
        let mut output = vec![0x1234; 100];
        assert!(!raster(Shape::Arrow, 99, &mut output, 24));
        assert!(!raster(Shape::Arrow, 100, &mut output, 23));
        let p = Placement {
            x: u32::MAX,
            y: 0,
            w: 24,
            h: 24,
            sx: 0,
            sy: 0,
        };
        assert!(!composite(&mut output, 10, 10, &[], 24, p));
        assert!(output.iter().all(|p| *p == 0x1234));
    }
}
