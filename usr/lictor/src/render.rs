//! Baked, theme-independent Lex curiata rasterizer. Only validated semantic
//! frames enter here. The output must remain private to the trusted backend.
use alloc::{format, string::String};
use crate::model::{Model, State};
use cornucopia::Atlas;
const BG: u32 = 0xff17191c;
const PANEL: u32 = 0xff22252a;
const INK: u32 = 0xffece8de;
const QUIET: u32 = 0xffaaa79e;
const AMBER: u32 = 0xffd9b36c;
const RULE: u32 = 0xff494a48;
pub const MIN_WIDTH: u32 = 800;
pub const MIN_HEIGHT: u32 = 720;
const MAX_PIXELS: usize = 4096 * 2160;
const CAP_TEXT: &[(u64, &str, &str)] = &[
    (1 << 7, "CAP_DAC_OVERRIDE", "Bypass file permission checks."),
    (1 << 8, "CAP_CHOWN", "Change file ownership."),
    (1 << 9, "CAP_KILL", "Send signals across identity boundaries."),
    (1 << 10, "CAP_DEBUG", "Inspect and control other processes."),
    (1 << 11, "CAP_JIT", "Create executable code at runtime."),
    (1 << 12, "CAP_AUDIO_GRAPH", "Control the complete audio graph."),
    (1 << 13, "CAP_POST_SERVICE", "Publish services for other processes."),
];
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RenderError { Geometry, Content }
struct Canvas<'a> { pixels: &'a mut [u32], w: usize, h: usize }
impl Canvas<'_> {
    fn rect(&mut self, x: usize, y: usize, w: usize, h: usize, color: u32) {
        for yy in y..y.saturating_add(h).min(self.h) {
            for xx in x..x.saturating_add(w).min(self.w) { self.pixels[yy * self.w + xx] = color; }
        }
    }
    fn text(&mut self, x: usize, y: usize, text: &str, advance: u8, color: u32) {
        let atlas = Atlas::for_advance(advance);
        let (cw, ch) = (atlas.cell_w(), atlas.cell_h());
        for (i, c) in text.chars().enumerate() {
            let Some(alpha) = atlas.glyph(c) else { continue; };
            for yy in 0..ch {
                let py = y + yy;
                if py >= self.h { continue; }
                for xx in 0..cw {
                    let px = x + i * cw + xx;
                    if px >= self.w { continue; }
                    let a = alpha[yy * cw + xx] as u32;
                    if a == 0 { continue; }
                    let at = py * self.w + px;
                    let old = self.pixels[at];
                    let mut blended = 0xff000000;
                    for shift in [0, 8, 16] {
                        let channel = (((color >> shift) & 255) * a + ((old >> shift) & 255) * (255 - a) + 127) / 255;
                        blended |= channel << shift;
                    }
                    self.pixels[at] = blended;
                }
            }
        }
    }
}
fn ascii(bytes: &[u8]) -> &str { core::str::from_utf8(bytes).unwrap_or("") }
fn term(m: &Model) -> String {
    if m.term_ns == 0 { return String::from("Until abdication or the requesting process exits."); }
    if m.term_ns % 1_000_000_000 != 0 {
        return format!("{} ns from activation; ends on abdication or exit.", m.term_ns);
    }
    let seconds = m.term_ns / 1_000_000_000;
    if seconds % 3600 == 0 { format!("{} hours from activation; ends on abdication or exit.", seconds / 3600) }
    else if seconds % 60 == 0 { format!("{} minutes from activation; ends on abdication or exit.", seconds / 60) }
    else { format!("{} seconds from activation; ends on abdication or exit.", seconds) }
}
/// A backdrop is accepted only after the backend has copied a completed normal
/// frame into immutable private memory. This function never samples client memory.
pub fn render(pixels: &mut [u32], width: u32, height: u32, backdrop: Option<&[u32]>, model: &Model, masked: usize) -> Result<(), RenderError> {
    let (w, h) = (width as usize, height as usize);
    let n = w.checked_mul(h).ok_or(RenderError::Geometry)?;
    if width < MIN_WIDTH || height < MIN_HEIGHT || n > MAX_PIXELS || pixels.len() != n { return Err(RenderError::Geometry); }
    // All fallible checks precede painting; an invalid frame never partially
    // overwrites a valid authorization. Unknown capability bits are rejected.
    model.encode().map_err(|_| RenderError::Content)?;
    if masked > 256 { return Err(RenderError::Content); }
    let count = CAP_TEXT.iter().filter(|(bit, _, _)| model.caps & bit != 0).count();
    let request = model.pid != 0;
    let ph = if request { 354 + count * 42 } else { 250 };
    if ph > h - 64 { return Err(RenderError::Geometry); }
    if let Some(backdrop) = backdrop {
        if backdrop.len() != n { return Err(RenderError::Geometry); }
        for (out, old) in pixels.iter_mut().zip(backdrop) {
            *out = 0xff000000;
            for shift in [0, 8, 16] { *out |= (((old >> shift) & 255) * 3 / 8) << shift; }
        }
    } else { pixels.fill(BG); }
    let mut c = Canvas { pixels, w, h };
    c.rect(0, 0, w, 48, BG);
    c.rect(0, 47, w, 1, RULE);
    c.text(28, 12, "CORVUS  /  LEX CURIATA", 9, AMBER);
    c.text(w - 248, 12, "SECURE ATTENTION", 9, QUIET);
    let pw = 688;
    let x = (w - pw) / 2;
    let y = 64 + (h - 64 - ph) / 2;
    c.rect(x - 1, y - 1, pw + 2, ph + 2, RULE);
    c.rect(x, y, pw, ph, PANEL);
    let tx = x + 24;
    let heading = if request { "Conferring imperium" } else { "Secure attention" };
    c.text(tx, y + 22, heading, 13, INK);
    // The fasces are an authority cue, never a claim that the icon is uncopyable.
    for i in 0..5 { c.rect(x + pw - 52 + i * 3, y + 24, 1, 30, AMBER); }
    c.rect(x + pw - 54, y + 32, 18, 2, AMBER);
    c.rect(x + pw - 54, y + 44, 18, 2, AMBER);
    if model.caps & (1 << 9) != 0 { c.rect(x + pw - 36, y + 25, 10, 12, AMBER); }
    if !request {
        if model.state == State::Failed {
            c.text(tx, y + 88, "The trusted path could not complete.", 10, AMBER);
            c.text(tx, y + 132, "No authority was conferred. Release all keys to return.", 9, QUIET);
        } else if !model.notice.is_empty() {
            c.text(tx, y + 88, ascii(&model.notice), 10, INK);
            c.text(tx, y + 132, "Release the secure attention keys.", 9, QUIET);
        } else {
            c.text(tx, y + 88, "No authorization request is waiting.", 10, INK);
            c.text(tx, y + 132, "Your workspace is suspended.", 9, QUIET);
            c.text(tx, y + 192, "Press any key to return.", 9, AMBER);
        }
        return Ok(());
    }
    c.text(tx, y + 58, &format!("{}  /  process {}", ascii(&model.user), model.pid), 10, INK);
    c.text(tx, y + 82, &format!("Level {}  /  identity {}", ascii(&model.level), model.principal), 8, QUIET);
    c.rect(tx, y + 103, pw - 48, 1, RULE);
    c.text(tx, y + 115, "PROVINCIA", 8, AMBER);
    let mut cy = y + 141;
    for &(bit, name, explanation) in CAP_TEXT {
        if model.caps & bit == 0 { continue; }
        c.text(tx, cy, name, 9, INK);
        c.text(tx, cy + 21, explanation, 8, QUIET);
        cy += 42;
    }
    c.rect(tx, cy + 4, pw - 48, 1, RULE);
    c.text(tx, cy + 16, "TERM", 8, AMBER);
    c.text(tx, cy + 37, &term(model), 8, INK);
    c.text(tx, cy + 59, if model.propagating { "Applies to this process and its descendants." } else { "Applies to the requesting process." }, 8, QUIET);
    cy += 93;
    if model.state == State::Pending {
        c.text(tx, cy, "IMPERIUM KEY", 8, AMBER);
        c.rect(tx, cy + 26, pw - 48, 38, BG);
        c.rect(tx, cy + 63, pw - 48, 1, AMBER);
        // Bounded masked display, with a count when the field would overflow.
        let visible = masked.min(50);
        let mut stars = alloc::vec![b'*'; visible];
        if masked > visible { stars.extend_from_slice(format!("  ({})", masked).as_bytes()); }
        c.text(tx + 12, cy + 31, ascii(&stars), 9, INK);
        c.text(tx, cy + 76, "Enter: confer    Escape: cancel", 8, QUIET);
    } else {
        let text = match model.state {
            State::Verifying => "Verifying the imperium key...",
            State::Denied => "Authorization was not conferred.",
            State::Locked => "Authorization is locked after repeated failures.",
            State::Expired => "The authorization request has expired.",
            State::Gone => "The requesting process is no longer available.",
            State::Cancelled => "Authorization cancelled.",
            State::Success => "Imperium conferred.",
            _ => "The trusted operation could not be completed.",
        };
        c.text(tx, cy + 8, text, 10, AMBER);
        for (line, bytes) in model.notice.chunks(78).enumerate() {
            c.text(tx, cy + 36 + line * 18, ascii(bytes), 8, QUIET);
        }
        if model.state != State::Verifying { c.text(tx, cy + 80, "Press any key to return.", 9, QUIET); }
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn full_capability_set_fits_minimum_display() {
        let model = Model {
            state: State::Pending, pid: 1, principal: 1000, stripes: 1,
            caps: 0x3f80, term_ns: 14_400_000_000_000, request_deadline_ns: 1,
            propagating: false, user: b"michael".to_vec(), level: b"imperium".to_vec(),
            notice: alloc::vec::Vec::new(),
        };
        let mut pixels = alloc::vec![0; 800 * 720];
        assert_eq!(render(&mut pixels, 800, 720, None, &model, 256), Ok(()));
        for advance in [6, 7, 8, 9, 10, 11, 12, 13, 15, 18, 20] {
            let glyph = Atlas::for_advance(advance).glyph('‖').expect("fasces rod baked");
            assert!(glyph.iter().any(|&alpha| alpha != 0), "rod must have visible ink");
        }
    }
    #[test]
    fn small_or_invalid_display_never_partially_paints() {
        let mut pixels = alloc::vec![0x12345678; 800 * 600];
        assert_eq!(render(&mut pixels, 800, 600, None, &Model::default(), 0), Err(RenderError::Geometry));
        assert!(pixels.iter().all(|p| *p == 0x12345678));
        let mut pixels = alloc::vec![0x12345678; 800 * 720];
        let m = Model { caps: 1 << 63, ..Model::default() };
        assert_eq!(render(&mut pixels, 800, 720, None, &m, 0), Err(RenderError::Content));
        assert!(pixels.iter().all(|p| *p == 0x12345678));
    }
}
