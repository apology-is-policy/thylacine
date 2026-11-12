//! Immutable, build-baked type. No font parsing, theme lookup or shared atlas
//! is permitted on the trusted path. Input text is validated by Model first.
#[derive(Clone, Copy)]
pub enum Font { Label, Body, Section, Identity, Title, SmallMono, Rail, Mono }
include!("type_metrics.rs");
// One set for each scale the screen is drawn at: `k` pixels to a layout pixel.
const MASKS: [&[u8]; SCALES] = [include_bytes!("type.bin"), include_bytes!("type2.bin")];
/// A glyph at `k` pixels to a layout pixel, `k` from 1 to `SCALES`: its
/// advance and its box in layout pixels, and the box's alpha, `k` times as
/// wide and as high. The layout is the same at every scale.
pub fn glyph(font: Font, k: usize, ch: char) -> (usize, usize, usize, &'static [u8]) {
    let index = match ch { ' '..='~' => ch as usize - 32, '·' => 95, '—' => 96, _ => b'?' as usize - 32 };
    let (offsets, w, h) = metrics(font);
    let (set, n) = (MASKS[k - 1], w * k * h * k);
    let at = offsets[k - 1] + index * (1 + n);
    (set[at] as usize, w, h, &set[at + 1..at + 1 + n])
}
/// In layout pixels.
pub fn width(font: Font, text: &str, tracking: usize) -> usize {
    text.chars().map(|ch| glyph(font, 1, ch).0 + tracking).sum::<usize>().saturating_sub(tracking)
}
#[cfg(test)]
mod tests {
    use super::*;
    const FONTS: [Font; 8] = [Font::Label, Font::Body, Font::Section, Font::Identity, Font::Title,
        Font::SmallMono, Font::Rail, Font::Mono];
    #[test]
    fn the_sets_hold_every_glyph_and_nothing_else() {
        for k in 1..=SCALES {
            let mut end = 0;
            for (i, font) in FONTS.into_iter().enumerate() {
                let (offsets, w, h) = metrics(font);
                // The faces follow one another in the order of the enum.
                assert_eq!(offsets[k - 1], end, "face {i} at {k}");
                end += 97 * (1 + w * k * h * k);
            }
            assert_eq!(end, MASKS[k - 1].len(), "the set at {k}");
            // And it is, byte for byte, the set these metrics were baked with.
            let hash = MASKS[k - 1].iter().fold(0xcbf29ce484222325u64, |h, b| (h ^ *b as u64).wrapping_mul(0x100000001b3));
            assert_eq!(hash, HASHES[k - 1], "the set at {k} is not the one its metrics were baked with");
        }
    }
    #[test]
    fn a_glyph_at_two_is_the_glyph_at_one_drawn_finer() {
        // The same advance and box. And the same ink in the same place: not
        // pixel for pixel, because the set at one is sampled four times a
        // pixel each way and a thin stem falls on those samples as it may,
        // but over a face the ink's middle and its amount agree.
        for (i, font) in FONTS.into_iter().enumerate() {
            let (mut dx, mut dy, mut ink, mut inked) = (0.0f64, 0.0f64, [0.0f64; 2], 0);
            for code in (32u32..127).chain([0xb7, 0x2014]) {
                let ch = char::from_u32(code).unwrap();
                let (a1, w, h, one) = glyph(font, 1, ch);
                let (a2, w2, h2, two) = glyph(font, 2, ch);
                assert_eq!((a1, w, h), (a2, w2, h2), "{ch:?}");
                assert_eq!((one.len(), two.len()), (w * h, 4 * w * h));
                // The ink's amount, and its middle in layout pixels.
                let middle = |mask: &[u8], k: usize| {
                    let (mut m, mut x, mut y) = (0.0, 0.0, 0.0);
                    for (at, a) in mask.iter().enumerate() {
                        m += *a as f64;
                        x += *a as f64 * ((at % (w * k)) as f64 + 0.5) / k as f64;
                        y += *a as f64 * ((at / (w * k)) as f64 + 0.5) / k as f64;
                    }
                    (m / (k * k) as f64, x / m, y / m)
                };
                let (m1, x1, y1) = middle(one, 1);
                let (m2, x2, y2) = middle(two, 2);
                assert_eq!(m1 == 0.0, m2 == 0.0, "{ch:?}");
                if m1 == 0.0 { continue; }
                // No glyph is anywhere near a device pixel out.
                assert!((x2 - x1).abs() < 0.48 && (y2 - y1).abs() < 0.48, "face {i} {ch:?}: {} {}", x2 - x1, y2 - y1);
                dx += x2 - x1; dy += y2 - y1; ink[0] += m1; ink[1] += m2; inked += 1;
            }
            assert!(inked >= 95, "face {i}: {inked} glyphs with ink");
            let (dx, dy) = (dx / inked as f64, dy / inked as f64);
            assert!(dx.abs() < 0.03 && dy.abs() < 0.03, "face {i}: the ink at two is {dx},{dy} of a layout pixel from the ink at one");
            assert!((ink[1] / ink[0] - 1.0).abs() < 0.02, "face {i}: {} of the ink at one", ink[1] / ink[0]);
        }
    }
}
