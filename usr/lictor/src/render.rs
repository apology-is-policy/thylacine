//! Build-baked Lex curiata. Only validated semantic frames enter the private raster.
use alloc::{format, string::String, vec::Vec};
use crate::{model::{Model, State}, typography::{self, Font}};
const BG: u32 = 0xff080b0c;
const PANEL: u32 = 0xff111616;
const INK: u32 = 0xffdce0dc;
const QUIET: u32 = 0xff9ba7a0;
const LABEL: u32 = 0xff919e97;
const AMBER: u32 = 0xffc7b98b;
const RULE: u32 = 0xff343e39;
const BORDER: u32 = 0xff59635e;
pub const MIN_WIDTH: u32 = 800;
pub const MIN_HEIGHT: u32 = 720;
const MAX_PIXELS: usize = 4096 * 2160;
const CAP_TEXT: &[(u64, &str, &str)] = &[
    (1 << 7, "CAP_DAC_OVERRIDE", "Bypass file permission checks."),
    (1 << 8, "CAP_CHOWN", "Change file ownership."),
    (1 << 9, "CAP_KILL", "Send signals across identity boundaries, including termination."),
    (1 << 10, "CAP_DEBUG", "Inspect and control other processes."),
    (1 << 11, "CAP_JIT", "Create executable code at runtime."),
    (1 << 12, "CAP_AUDIO_GRAPH", "Control the complete audio graph."),
    (1 << 13, "CAP_POST_SERVICE", "Post services within the bounds of this scope."),
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
    fn outline(&mut self, x: usize, y: usize, w: usize, h: usize, color: u32) {
        self.rect(x, y, w, 1, color); self.rect(x, y + h - 1, w, 1, color);
        self.rect(x, y, 1, h, color); self.rect(x + w - 1, y, 1, h, color);
    }
    fn text(&mut self, mut x: usize, y: usize, text: &str, font: Font, color: u32, tracking: usize) {
        for ch in text.chars() {
            let (advance, cw, ch, alpha) = typography::glyph(font, ch);
            for yy in 0..ch {
                let py = y + yy;
                if py >= self.h { continue; }
                for xx in 0..cw {
                    let px = (x + xx).saturating_sub(2); // baked bearing room
                    if px >= self.w { continue; }
                    let a = alpha[yy * cw + xx] as u32;
                    if a == 0 { continue; }
                    let at = py * self.w + px;
                    let old = self.pixels[at];
                    let mut blended = 0xff000000;
                    for shift in [0, 8, 16] {
                        blended |= ((((color >> shift) & 255) * a + ((old >> shift) & 255) * (255 - a) + 127) / 255) << shift;
                    }
                    self.pixels[at] = blended;
                }
            }
            x += advance + tracking;
        }
    }
    fn lines(&mut self, x: usize, y: usize, lines: &[String], font: Font, color: u32, leading: usize) {
        for (i, line) in lines.iter().enumerate() { self.text(x, y + i * leading, line, font, color, 0); }
    }
    fn fasces(&mut self, x: usize, y: usize, axe: bool) {
        for (dx, top, height) in [(6, 5, 44), (11, 3, 48), (16, 2, 50), (21, 3, 48), (26, 5, 44)] {
            self.rect(x + dx, y + top, 2, height, AMBER);
        }
        for dy in [15, 38] { self.rect(x + 3, y + dy, 27, 2, AMBER); }
        for dy in 0..26 {
            self.rect(x + 5 + dy * 22 / 25, y + 14 + dy, 2, 1, AMBER);
            self.rect(x + 27 - dy * 22 / 25, y + 14 + dy, 2, 1, AMBER);
        }
        if axe { self.rect(x + 29, y + 8, 12, 17, AMBER); }
    }
}
fn ascii(bytes: &[u8]) -> &str { core::str::from_utf8(bytes).unwrap_or("") }
// Wrap even unbroken identities; never ellipsize authority-bearing content.
fn wrap(text: &str, font: Font, width: usize) -> Vec<String> {
    let mut lines = Vec::new();
    let mut line = String::new();
    for word in text.split_whitespace() {
        let candidate = if line.is_empty() { String::from(word) } else { format!("{} {}", line, word) };
        if typography::width(font, &candidate, 0) <= width { line = candidate; continue; }
        if !line.is_empty() { lines.push(core::mem::take(&mut line)); }
        for ch in word.chars() {
            let mut next = line.clone(); next.push(ch);
            if typography::width(font, &next, 0) > width && !line.is_empty() {
                lines.push(core::mem::take(&mut line));
            }
            line.push(ch);
        }
    }
    if !line.is_empty() { lines.push(line); }
    lines
}
fn term(m: &Model) -> String {
    if m.term_ns == 0 { return String::from("Revoked on exit or abdication. No expiry is requested."); }
    if m.term_ns % 1_000_000_000 != 0 {
        return format!("{} ns from activation; also revoked on exit or abdication.", m.term_ns);
    }
    let seconds = m.term_ns / 1_000_000_000;
    if seconds % 3600 == 0 { format!("{} hours from activation; also revoked on exit or abdication.", seconds / 3600) }
    else if seconds % 60 == 0 { format!("{} minutes from activation; also revoked on exit or abdication.", seconds / 60) }
    else { format!("{} seconds from activation; also revoked on exit or abdication.", seconds) }
}
fn verdict(state: State) -> &'static str {
    match state {
        State::Verifying => "Verifying the imperium key...",
        State::Denied => "Authorization was not conferred.",
        State::Locked => "Authorization is locked after repeated failures.",
        State::Expired => "The authorization request has expired.",
        State::Gone => "The requesting process is no longer available.",
        State::Cancelled => "Authorization cancelled.",
        State::Success => "Imperium conferred.",
        _ => "The trusted operation could not be completed.",
    }
}
struct Layout {
    dense: bool, ph: usize, identity: Vec<String>, identity_h: usize,
    rows: Vec<(String, Vec<String>, usize)>, term: Vec<String>, notice: Vec<String>,
}
impl Layout {
    fn new(model: &Model, dense: bool) -> Self {
        let font = if dense { Font::Label } else { Font::Body };
        let identity = wrap(ascii(&model.user), if dense { Font::Body } else { Font::Identity }, 282);
        let identity_h = (if dense { 58 } else { 72 }) + identity.len().saturating_sub(1) * 20;
        let mut rows = Vec::new();
        for &(bit, name, explanation) in CAP_TEXT {
            if model.caps & bit == 0 { continue; }
            let lines = wrap(explanation, font, 560);
            let rh = if dense { 16 + lines.len() * 16 } else { 42 + lines.len() * 20 };
            rows.push((String::from(name), lines, rh));
        }
        let term = wrap(&term(model), font, 596);
        let notice = wrap(ascii(&model.notice), font, 596);
        let ph = if model.pid == 0 { 290 + notice.len().saturating_sub(1) * 20 } else {
            let header = if dense { 76 } else { 104 };
            let pad = if dense { 10 } else { 24 };
            let province = if dense { 24 } else { 42 };
            let term_h = if dense { 60 + term.len() * 16 } else { 104 + term.len() * 22 };
            let key_h = if dense { 83 } else { 105 };
            let feedback_h = (if dense { 44 } else { 54 }) + notice.len() * (if dense { 16 } else { 20 });
            header + pad + identity_h + province + rows.iter().map(|r| r.2).sum::<usize>()
                + term_h + key_h.max(feedback_h) + (if dense { 54 } else { 76 })
        };
        Self { dense, ph, identity, identity_h, rows, term, notice }
    }
}
/// A backdrop must already be an immutable private copy of a completed normal
/// frame. No ordinary-client memory is sampled here.
pub fn render(pixels: &mut [u32], width: u32, height: u32, backdrop: Option<&[u32]>, model: &Model, masked: usize) -> Result<(), RenderError> {
    let (w, h) = (width as usize, height as usize);
    let n = w.checked_mul(h).ok_or(RenderError::Geometry)?;
    if width < MIN_WIDTH || height < MIN_HEIGHT || n > MAX_PIXELS || pixels.len() != n { return Err(RenderError::Geometry); }
    model.encode().map_err(|_| RenderError::Content)?;
    if masked > 256 { return Err(RenderError::Content); }
    if backdrop.is_some_and(|b| b.len() != n) { return Err(RenderError::Geometry); }
    let mut layout = Layout::new(model, false);
    if layout.ph > h - 80 { layout = Layout::new(model, true); }
    if layout.ph > h - 80 { return Err(RenderError::Geometry); }
    // Validate and lay out in full before replacing any part of the old frame.
    if let Some(backdrop) = backdrop {
        for (out, old) in pixels.iter_mut().zip(backdrop) {
            *out = 0xff000000;
            for shift in [0, 8, 16] { *out |= (((old >> shift) & 255) * 26 / 100) << shift; }
        }
    } else { pixels.fill(BG); }
    let mut c = Canvas { pixels, w, h };
    c.rect(0, 0, w, 48, 0xff0b0e0e); c.rect(0, 47, w, 1, 0xff4b514d);
    c.text(28, 15, "CORVUS   /   LEX CURIATA", Font::Rail, AMBER, 1);
    let rail = "SECURE ATTENTION · WORKSPACE SUSPENDED";
    c.text(w - 28 - typography::width(Font::Rail, rail, 1), 15, rail, Font::Rail, LABEL, 1);
    let pw = 660;
    let x = (w - pw) / 2;
    let y = 56 + (h - 56 - layout.ph) / 2;
    c.rect(x, y, pw, layout.ph, PANEL); c.outline(x, y, pw, layout.ph, BORDER);
    let dense = layout.dense;
    let head = if dense { 76 } else { 104 };
    let tx = x + 32;
    let hy = y + if dense { 13 } else { 25 };
    c.fasces(tx, hy, model.caps & (1 << 9) != 0);
    c.text(tx + 54, hy, "AUTHORITY FOR THIS SCOPE", Font::SmallMono, AMBER, 1);
    c.text(tx + 54, hy + 22, if model.pid == 0 { "Secure attention" } else { "Confer imperium" }, Font::Title, INK, 0);
    c.rect(x + 1, y + head, pw - 2, 1, RULE);
    let mut cy = y + head + if dense { 10 } else { 24 };
    if model.pid == 0 {
        let text = if model.state == State::Failed { "The trusted path could not complete." }
            else if model.notice.is_empty() { "No authorization request is waiting." }
            else { ascii(&model.notice) };
        let lines = wrap(text, Font::Body, 596);
        c.lines(tx, cy, &lines, Font::Body, INK, 20);
        cy += 32 + lines.len().saturating_sub(1) * 20;
        c.text(tx, cy, if model.state == State::Failed { "No authority was conferred. Release all keys to return." }
            else if !model.notice.is_empty() { "Release the secure attention keys." }
            else { "Your workspace is suspended. Press any key to return." }, Font::Body, QUIET, 0);
        return Ok(());
    }
    let body = if dense { Font::Label } else { Font::Body };
    c.text(tx, cy, "Principal", Font::Label, LABEL, 0);
    c.text(tx + 310, cy, "Requesting process", Font::Label, LABEL, 0);
    let identity_font = if dense { Font::Body } else { Font::Identity };
    c.lines(tx, cy + 20, &layout.identity, identity_font, INK, 20);
    c.text(tx + 310, cy + 20, &format!("pid {}", model.pid), Font::Mono, INK, 0);
    let uid = format!("· uid {}", model.principal);
    let name_width = typography::width(identity_font, ascii(&model.user), 0);
    if layout.identity.len() == 1 && name_width + 12 + typography::width(Font::Mono, &uid, 0) <= 282 {
        c.text(tx + name_width + 12, cy + 23, &uid, Font::Mono, QUIET, 0);
    } else {
        // A long principal remains complete; its numeric identity uses the
        // spare line under the PID rather than crossing the column boundary.
        c.text(tx + 310, cy + 40, &format!("uid {}", model.principal), Font::Mono, QUIET, 0);
    }
    c.text(tx, cy + 20 + layout.identity.len() * 20, &format!("Level {}", ascii(&model.level)), Font::Label, QUIET, 0);
    cy += layout.identity_h;
    c.rect(tx, cy, 596, 1, RULE);
    c.text(tx, cy + if dense { 5 } else { 18 }, "Provincia — authority requested", Font::Section, QUIET, 0);
    cy += if dense { 24 } else { 42 };
    for (name, explanation, rh) in &layout.rows {
        c.rect(tx, cy, 596, *rh, 0xff19201d); c.rect(tx, cy, 2, *rh, AMBER);
        c.text(tx + 18, cy + if dense { 1 } else { 10 }, name, if dense { Font::Rail } else { Font::Mono }, INK, 0);
        c.lines(tx + 18, cy + if dense { 15 } else { 32 }, explanation, body, 0xffb5bfb8, if dense { 16 } else { 20 });
        cy += rh;
    }
    cy += if dense { 7 } else { 22 };
    c.text(tx, cy, "Term — lifetime", Font::Section, QUIET, 0);
    cy += if dense { 21 } else { 32 };
    c.text(tx, cy, if model.propagating { "This process and its descendants." } else { "The requesting process only." }, body, INK, 0);
    cy += if dense { 16 } else { 22 };
    c.lines(tx, cy, &layout.term, body, QUIET, if dense { 16 } else { 22 });
    cy += layout.term.len() * (if dense { 16 } else { 22 }) + if dense { 16 } else { 28 };
    let action_y = y + layout.ph - if dense { 54 } else { 76 };
    if model.state == State::Pending {
        c.text(tx, cy, "Imperium key", Font::Label, LABEL, 0);
        let field_y = cy + 22;
        let field_h = if dense { 33 } else { 43 };
        c.rect(tx, field_y, 596, field_h, 0xff0b1010); c.outline(tx, field_y, 596, field_h, 0xff56635b);
        if masked == 0 { c.text(tx + 13, field_y + 7, "Enter your imperium key", Font::Mono, 0xff85928a, 0); }
        else {
            let visible = masked.min(50);
            let mut stars = alloc::vec![b'*'; visible];
            if masked > visible { stars.extend_from_slice(format!("  ({})", masked).as_bytes()); }
            c.text(tx + 13, field_y + 7, ascii(&stars), Font::Mono, INK, 0);
        }
        c.text(tx, field_y + field_h + 7, "Use your imperium key, separate from your sign-in password.", Font::Label, LABEL, 0);
    } else {
        c.text(tx, cy, verdict(model.state), body, AMBER, 0);
        c.lines(tx, cy + 26, &layout.notice, body, QUIET, if dense { 16 } else { 20 });
    }
    c.rect(x + 1, action_y, pw - 2, 1, RULE);
    if model.state == State::Pending {
        let by = action_y + if dense { 11 } else { 18 };
        c.outline(tx, by + 10, 29, 21, 0xff566059);
        c.text(tx + 5, by + 13, "ESC", Font::SmallMono, QUIET, 0);
        c.text(tx + 40, by + 11, "Cancel request", Font::Body, QUIET, 0);
        let bx = x + pw - 32 - 192;
        c.rect(bx, by, 192, 40, AMBER); c.outline(bx, by, 192, 40, 0xff7a8069);
        c.outline(bx + 12, by + 9, 42, 22, 0xff7a8069);
        c.text(bx + 17, by + 13, "ENTER", Font::SmallMono, 0xff121813, 0);
        c.text(bx + 63, by + 10, "Confer authority", Font::Body, 0xff121813, 0);
    } else {
        c.text(tx, action_y + 19, if model.state == State::Verifying { "Please wait for the authorization verdict." }
            else { "Press any key to return to your workspace." }, Font::Body, QUIET, 0);
    }
    Ok(())
}
#[cfg(test)]
mod tests {
    use super::*;
    fn model() -> Model { Model { state: State::Pending, pid: u32::MAX, principal: u32::MAX,
        stripes: 1, caps: 0x3f80, term_ns: u64::MAX, request_deadline_ns: 1,
        propagating: true, user: alloc::vec![b'W'; 32], level: alloc::vec![b'W'; 32], notice: Vec::new() } }
    #[test]
    fn maximum_authority_fits_without_elision() {
        let model = model();
        let layout = Layout::new(&model, true);
        assert!(layout.ph <= 640, "maximum request must fit 800x720: {}", layout.ph);
        assert_eq!(layout.rows.len(), 7);
        assert_eq!(layout.identity.concat(), ascii(&model.user));
        let mut pixels = alloc::vec![0; 800 * 720];
        assert_eq!(render(&mut pixels, 800, 720, None, &model, 256), Ok(()));
    }
    #[test]
    fn every_capability_subset_and_verdict_fits_the_minimum() {
        for subset in 1..128 {
            for state in [State::Pending, State::Verifying, State::Denied, State::Locked,
                State::Expired, State::Gone, State::Cancelled, State::Success, State::Failed] {
                let mut m = model();
                m.caps = subset << 7;
                m.state = state;
                m.notice = alloc::vec![b'W'; 128];
                let layout = Layout::new(&m, true);
                assert!(layout.ph <= 640, "subset={subset} state={state:?} height={}", layout.ph);
                for (_, lines, _) in &layout.rows {
                    assert!(lines.iter().all(|s| typography::width(Font::Label, s, 0) <= 560));
                }
                assert_eq!(layout.notice.concat(), ascii(&m.notice));
            }
        }
    }
    #[test]
    fn invalid_frame_never_partially_paints() {
        let mut pixels = alloc::vec![0x12345678; 800 * 600];
        assert_eq!(render(&mut pixels, 800, 600, None, &Model::default(), 0), Err(RenderError::Geometry));
        assert!(pixels.iter().all(|p| *p == 0x12345678));
        let mut pixels = alloc::vec![0x12345678; 800 * 720];
        let m = Model { caps: 1 << 63, ..Model::default() };
        assert_eq!(render(&mut pixels, 800, 720, None, &m, 0), Err(RenderError::Content));
        assert!(pixels.iter().all(|p| *p == 0x12345678));
    }
}
