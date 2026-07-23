// The F10 settings overlay -- Aurora's BUILT-IN system dialog
// (AURORA-CONFIG.md section 3.6). Deliberately Turbo-Vision RAW: EGA colors,
// a double-line frame, a drop shadow -- so it reads as "system dialog, not
// your session", contrasty against both the Bonfire terminal and Kaua's fine
// style. It is not a program: it lives inside the renderer, so nothing (a
// future Halcyon included) can invoke or spoof it -- each environment owns
// its own settings surface.
//
// Chunk 1 scope: the shell + the Appearance section (aurora-local settings,
// live-apply, no gate) with Display info-only -- the `mode` value pushes
// arrive with the gated compositor ctl (section 3.3/5 sequencing), and the
// config-file persistence (section 3.2) is the next sub-chunk; until then
// settings are session-lived.
//
// Modality: while open, every key event routes here (nothing feeds the
// terminal); the terminal keeps updating UNDER the panel (the drain still
// feeds the Vt) -- the main loop composes the panel over the grid each
// damaged pass. Known cosmetic edge: holding Esc to close leaks its
// autorepeats to the terminal after the close (a tap does not); the chord
// plane's swallow-set discipline would be overkill for a settings dialog.

use crate::render::{darken_rect, draw_run, Metrics};
use crate::vt::THEMES;
use alloc::format;
use alloc::string::String;

// EGA -- the rawness is the point (classic TV: gray dialog, black text, a
// cyan focus bar, blue values, a dark hint row).
const EGA_GRAY: u32 = 0xFFAA_AAAA;
const EGA_BLACK: u32 = 0xFF00_0000;
const EGA_BLUE: u32 = 0xFF00_00AA;
const EGA_CYAN: u32 = 0xFF00_AAAA;
const EGA_WHITE: u32 = 0xFFFF_FFFF;
const EGA_DGRAY: u32 = 0xFF55_5555;

// evdev codes (linux/input-event-codes.h; the tapestryd keymap resolves no
// rune for any of these except Esc, and key routing goes by code here).
pub const KEY_F10: u16 = 68;
const KEY_ESC: u16 = 1;
const KEY_TAB: u16 = 15;
const KEY_ENTER: u16 = 28;
const KEY_UP: u16 = 103;
const KEY_DOWN: u16 = 108;
const KEY_LEFT: u16 = 105;
const KEY_RIGHT: u16 = 106;

/// The aurora-local settings the OSD edits (session-lived until the
/// config-file sub-chunk).
pub struct Settings {
    pub theme: usize, // index into vt::THEMES
    pub cursor_blink: bool,
}

impl Settings {
    pub fn new() -> Settings {
        Settings { theme: 0, cursor_blink: true }
    }
}

/// What a handled key asks the main loop to do.
pub enum OsdOut {
    None,
    Close,          // repaint the terminal (full_fill)
    ThemeChanged,   // call Vt::set_theme(settings.theme) + persist (cfg-2a)
    SettingChanged, // a non-theme setting moved -> persist (cfg-2a)
}

const SEC_DISPLAY: usize = 0;
const SEC_APPEARANCE: usize = 1;
const SEC_NAMES: [&str; 2] = ["Display", "Appearance"];
// Rows per section: Display { Resolution, Zoom policy } (info-only),
// Appearance { Theme, Cursor blink } (live).
const SEC_LEN: [usize; 2] = [2, 2];

pub struct Osd {
    pub open: bool,
    pub dirty: bool,
    sec: usize,
    sel: usize,
}

impl Osd {
    pub fn new() -> Osd {
        Osd { open: false, dirty: false, sec: SEC_APPEARANCE, sel: 0 }
    }

    /// Route one key event (press value==1, repeat value==2; releases are
    /// ignored). The toggle keys (F10/Esc) act on value==1 ONLY, so holding
    /// F10 cannot open-then-instantly-close via autorepeat; arrows accept
    /// repeat (value cycling wants it).
    pub fn handle_key(&mut self, code: u16, value: u32, s: &mut Settings) -> OsdOut {
        if value == 0 {
            return OsdOut::None;
        }
        match code {
            KEY_F10 | KEY_ESC => {
                if value == 1 {
                    self.open = false;
                    self.dirty = true;
                    return OsdOut::Close;
                }
                OsdOut::None
            }
            KEY_TAB => {
                self.sec = (self.sec + 1) % SEC_NAMES.len();
                self.sel = 0;
                self.dirty = true;
                OsdOut::None
            }
            KEY_UP => {
                if self.sel > 0 {
                    self.sel -= 1;
                    self.dirty = true;
                }
                OsdOut::None
            }
            KEY_DOWN => {
                if self.sel + 1 < SEC_LEN[self.sec] {
                    self.sel += 1;
                    self.dirty = true;
                }
                OsdOut::None
            }
            KEY_LEFT | KEY_RIGHT | KEY_ENTER => {
                if self.sec != SEC_APPEARANCE {
                    return OsdOut::None; // Display is info-only this chunk
                }
                self.dirty = true;
                match self.sel {
                    0 => {
                        let n = THEMES.len();
                        s.theme = if code == KEY_LEFT {
                            (s.theme + n - 1) % n
                        } else {
                            (s.theme + 1) % n
                        };
                        OsdOut::ThemeChanged
                    }
                    _ => {
                        s.cursor_blink = !s.cursor_blink;
                        OsdOut::SettingChanged
                    }
                }
            }
            _ => OsdOut::None,
        }
    }

    /// Compose the panel over the already-rendered grid (called after
    /// render_rows in the main loop's full-frame pass). Geometry derives
    /// from the CURRENT grid each draw, so a reweave needs no notification;
    /// at a degenerate grid the panel clamps (and the sub-floor console is
    /// already a crop case).
    pub fn draw(
        &self,
        px: &mut [u32],
        w: usize,
        m: &Metrics,
        cols: usize,
        rows: usize,
        s: &Settings,
        disp_w: u32,
        disp_h: u32,
    ) {
        let pw = 46.min(cols.saturating_sub(2)).max(8);
        let ph = 14.min(rows.saturating_sub(1)).max(4);
        let pc = (cols - pw) / 2;
        let pr = (rows - ph) / 2;

        // Drop shadow first (one cell right, one row down), then the field.
        darken_rect(
            px,
            w,
            m.off_x + (pc + 1) * m.cell_w,
            m.off_y + (pr + 1) * m.cell_h,
            pw * m.cell_w,
            ph * m.cell_h,
        );
        let blank: String = core::iter::repeat(' ').take(pw).collect();
        for r in 0..ph {
            draw_run(m, px, w, pc, pr + r, &blank, EGA_BLACK, EGA_GRAY);
        }

        // The double-line frame + the title in the top border.
        let mut top = String::new();
        let mut bot = String::new();
        top.push('\u{2554}');
        bot.push('\u{255A}');
        for _ in 0..pw.saturating_sub(2) {
            top.push('\u{2550}');
            bot.push('\u{2550}');
        }
        top.push('\u{2557}');
        bot.push('\u{255D}');
        draw_run(m, px, w, pc, pr, &top, EGA_BLACK, EGA_GRAY);
        draw_run(m, px, w, pc, pr + ph - 1, &bot, EGA_BLACK, EGA_GRAY);
        for r in 1..ph - 1 {
            draw_run(m, px, w, pc, pr + r, "\u{2551}", EGA_BLACK, EGA_GRAY);
            draw_run(m, px, w, pc + pw - 1, pr + r, "\u{2551}", EGA_BLACK, EGA_GRAY);
        }
        let title = " Aurora Settings ";
        if pw > title.len() + 2 {
            draw_run(m, px, w, pc + (pw - title.len()) / 2, pr, title, EGA_BLACK, EGA_GRAY);
        }

        // Section tabs (row 1) + the separator (row 2).
        let mut c = pc + 2;
        for (i, name) in SEC_NAMES.iter().enumerate() {
            let label = format!(" {} ", name);
            let (tfg, tbg) = if i == self.sec {
                (EGA_BLACK, EGA_CYAN)
            } else {
                (EGA_DGRAY, EGA_GRAY)
            };
            if c + label.len() < pc + pw {
                draw_run(m, px, w, c, pr + 1, &label, tfg, tbg);
            }
            c += label.len() + 1;
        }
        if ph > 4 {
            let mut sep = String::new();
            sep.push('\u{255F}');
            for _ in 0..pw.saturating_sub(2) {
                sep.push('\u{2500}');
            }
            sep.push('\u{2562}');
            draw_run(m, px, w, pc, pr + 2, &sep, EGA_BLACK, EGA_GRAY);
        }

        // Item rows from row 4. Display is info-only (values in dark gray);
        // Appearance carries the live < value > cyclers.
        let info = self.sec == SEC_DISPLAY;
        let items: [(String, String); 2] = if info {
            [
                (String::from("Resolution"), format!("{} x {}", disp_w, disp_h)),
                (String::from("Zoom policy"), String::from("letterbox")),
            ]
        } else {
            [
                (
                    String::from("Theme"),
                    format!("< {} >", THEMES[s.theme % THEMES.len()].0),
                ),
                (
                    String::from("Cursor blink"),
                    format!("< {} >", if s.cursor_blink { "on" } else { "off" }),
                ),
            ]
        };
        for (i, (label, value)) in items.iter().enumerate() {
            let r = pr + 4 + i * 2;
            if r + 1 >= pr + ph - 1 {
                break;
            }
            let selected = i == self.sel;
            let (lfg, vfg, bg) = if selected {
                (EGA_BLACK, if info { EGA_DGRAY } else { EGA_WHITE }, EGA_CYAN)
            } else {
                (EGA_BLACK, if info { EGA_DGRAY } else { EGA_BLUE }, EGA_GRAY)
            };
            if selected {
                let bar: String = core::iter::repeat(' ').take(pw - 2).collect();
                draw_run(m, px, w, pc + 1, r, &bar, EGA_BLACK, EGA_CYAN);
            }
            draw_run(m, px, w, pc + 3, r, label, lfg, bg);
            let vcol = pc + pw.saturating_sub(value.len() + 3);
            if vcol > pc + 3 + label.len() {
                draw_run(m, px, w, vcol, r, value, vfg, bg);
            }
        }

        // The hint row (inside the bottom border).
        let hint = if info {
            " read-only until the mode ctl lands "
        } else {
            " Up/Dn  </> change  Tab  Esc close "
        };
        if ph > 6 && hint.len() + 2 < pw {
            draw_run(
                m,
                px,
                w,
                pc + (pw - hint.len()) / 2,
                pr + ph - 2,
                hint,
                EGA_DGRAY,
                EGA_GRAY,
            );
        }
    }
}

// DORMANT host-harness tests (the G-4f named seam, like render.rs/vt.rs):
// the state machine is pure logic; the draw path is proven by the in-guest
// ls-gfx OSD E2E.
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn f10_toggles_and_repeat_cannot_insta_close() {
        let mut o = Osd::new();
        let mut s = Settings::new();
        o.open = true;
        // An F10 REPEAT (value 2) must not close (the open keypress's own
        // autorepeat would otherwise bounce the panel shut).
        assert!(matches!(o.handle_key(KEY_F10, 2, &mut s), OsdOut::None));
        assert!(o.open);
        assert!(matches!(o.handle_key(KEY_F10, 1, &mut s), OsdOut::Close));
        assert!(!o.open);
    }

    #[test]
    fn theme_cycles_and_wraps_both_ways() {
        let mut o = Osd::new();
        let mut s = Settings::new();
        o.open = true; // starts on Appearance, sel 0 = Theme
        let n = THEMES.len();
        assert!(matches!(o.handle_key(KEY_RIGHT, 1, &mut s), OsdOut::ThemeChanged));
        assert_eq!(s.theme, 1 % n);
        assert!(matches!(o.handle_key(KEY_LEFT, 1, &mut s), OsdOut::ThemeChanged));
        assert_eq!(s.theme, 0);
        assert!(matches!(o.handle_key(KEY_LEFT, 1, &mut s), OsdOut::ThemeChanged));
        assert_eq!(s.theme, n - 1, "wraps backward");
    }

    #[test]
    fn display_section_is_inert_and_nav_bounds() {
        let mut o = Osd::new();
        let mut s = Settings::new();
        o.open = true;
        o.handle_key(KEY_TAB, 1, &mut s); // -> Display
        let theme0 = s.theme;
        assert!(matches!(o.handle_key(KEY_RIGHT, 1, &mut s), OsdOut::None));
        assert_eq!(s.theme, theme0, "info rows change nothing");
        // Nav clamps at the section bounds (no underflow/overflow).
        o.handle_key(KEY_UP, 1, &mut s);
        o.handle_key(KEY_DOWN, 1, &mut s);
        o.handle_key(KEY_DOWN, 1, &mut s);
        o.handle_key(KEY_DOWN, 1, &mut s);
        assert!(o.sel < SEC_LEN[o.sec]);
    }
}
