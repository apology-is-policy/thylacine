//! Immutable, build-baked type. No font parsing, theme lookup or shared atlas
//! is permitted on the trusted path. Input text is validated by Model first.
#[derive(Clone, Copy)]
pub enum Font { Label, Body, Section, Identity, Title, SmallMono, Rail, Mono }
include!("type_metrics.rs");
const MASKS: &[u8] = include_bytes!("type.bin");
pub fn glyph(font: Font, ch: char) -> (usize, usize, usize, &'static [u8]) {
    let index = match ch { ' '..='~' => ch as usize - 32, '·' => 95, '—' => 96, _ => b'?' as usize - 32 };
    let (offset, w, h) = metrics(font);
    let at = offset + index * (1 + w * h);
    (MASKS[at] as usize, w, h, &MASKS[at + 1..at + 1 + w * h])
}
pub fn width(font: Font, text: &str, tracking: usize) -> usize {
    text.chars().map(|ch| glyph(font, ch).0 + tracking).sum::<usize>().saturating_sub(tracking)
}
