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
use alloc::vec;
use alloc::vec::Vec;

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

/// The display-mode selection (cfg-3): the compositor tier the
/// environment pushes via the GATED global ctl (AURORA-CONFIG.md
/// section 3.3/3.4). Auto = adopt the GPU's preferred rect (never pushed
/// at startup -- it IS the boot default); Fixed pushes `mode W H`.
#[derive(Clone, Copy, PartialEq, Debug)]
pub enum Mode {
    Auto,
    Fixed(u32, u32),
}

/// The OSD's mode presets. Base virtio-gpu reports one preferred rect,
/// not a mode list -- these are common raster modes inside the server's
/// 320x200..3840x2160 bounds. A hand-edited config mode outside this
/// list still pushes at startup; the OSD's cycler just seeds at Auto.
pub const MODE_PRESETS: [Mode; 7] = [
    Mode::Auto,
    Mode::Fixed(1024, 768),
    Mode::Fixed(1280, 800),
    Mode::Fixed(1280, 720),
    Mode::Fixed(1600, 900),
    Mode::Fixed(1920, 1080),
    Mode::Fixed(2560, 1440),
];

/// The aurora-local settings the OSD edits, plus the pushed compositor
/// tier (`mode`, and cfg-4 `chords`/`gaps`).
pub struct Settings {
    pub theme: usize, // index into vt::THEMES
    pub cursor_blink: bool,
    pub mode: Mode, // cfg-3: applied via the gated ctl, never via OSC
    /// cfg-4: config `chord <combo> <action>` lines (compositor tier,
    /// pushed via the gated ctl at startup). The OSD does not edit them
    /// (hand-edited / halcyon.rc), but render() round-trips them so an
    /// OSD theme/mode save never wipes them.
    pub chords: Vec<(String, String)>,
    /// cfg-4: the inter-pane gap (px); None = the compositor default (1).
    pub gaps: Option<u32>,
    /// cfg-5: the Cornucopia cell advance (px) -- one of cornucopia::ADVANCES.
    /// Renderer-local (no compositor round-trip, no gate): selects which baked
    /// atlas Aurora blits, which sets the cell dims and thus cols/rows.
    pub font: u8,
}

impl Settings {
    pub fn new() -> Settings {
        Settings {
            theme: 0,
            cursor_blink: true,
            mode: Mode::Auto,
            chords: Vec::new(),
            gaps: None,
            font: cornucopia::DEFAULT_ADVANCE,
        }
    }
}

/// What a handled key asks the main loop to do.
pub enum OsdOut {
    None,
    Close,           // repaint the terminal (full_fill)
    ThemeChanged,    // call Vt::set_theme(settings.theme) + persist (cfg-2a)
    SettingChanged,  // a non-theme setting moved -> persist (cfg-2a)
    ModeApply(Mode), // cfg-3: write the gated ctl; persist ONLY on success
    FontChanged,     // cfg-5: rebuild Metrics + cols/rows + winsize; persist
}

const SEC_DISPLAY: usize = 0;
const SEC_APPEARANCE: usize = 1;
const SEC_NAMES: [&str; 2] = ["Display", "Appearance"];
// Rows per section: Display { Mode (live cycler, Enter applies),
// Resolution (info), Zoom policy (info) }, Appearance { Theme,
// Cursor blink, Font (cfg-5 live cycler) }.
const SEC_LEN: [usize; 2] = [3, 3];

pub struct Osd {
    pub open: bool,
    pub dirty: bool,
    sec: usize,
    sel: usize,
    /// The PENDING mode choice (cfg-3): </> cycles it; only Enter applies
    /// (the monitor-OSD semantic -- a mode change reconfigures the whole
    /// display and must never fire on mere navigation). Seeded from the
    /// applied settings at each open.
    mode_sel: usize,
}

impl Osd {
    pub fn new() -> Osd {
        Osd { open: false, dirty: false, sec: SEC_APPEARANCE, sel: 0, mode_sel: 0 }
    }

    /// Open the panel DETERMINISTICALLY: always the first section, first
    /// row (a settings dialog reopening on a stale tab with a stale
    /// selection is surprising -- and a keystroke recipe against it,
    /// like the E2E's, would silently act on the wrong rows), with the
    /// pending mode choice re-seeded from the APPLIED settings (a
    /// pending-but-unapplied cycle must not survive a close/reopen).
    pub fn open_at(&mut self, s: &Settings) {
        self.open = true;
        self.dirty = true;
        self.sec = SEC_APPEARANCE;
        self.sel = 0;
        self.mode_sel = MODE_PRESETS.iter().position(|m| *m == s.mode).unwrap_or(0);
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
                if self.sec == SEC_DISPLAY {
                    // Row 0 = Mode: </> cycles the PENDING choice; only
                    // Enter applies (cfg-3). Rows 1..2 are info.
                    if self.sel != 0 {
                        return OsdOut::None;
                    }
                    let n = MODE_PRESETS.len();
                    self.dirty = true;
                    return match code {
                        KEY_LEFT => {
                            self.mode_sel = (self.mode_sel + n - 1) % n;
                            OsdOut::None
                        }
                        KEY_RIGHT => {
                            self.mode_sel = (self.mode_sel + 1) % n;
                            OsdOut::None
                        }
                        _ => OsdOut::ModeApply(MODE_PRESETS[self.mode_sel]),
                    };
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
                    1 => {
                        s.cursor_blink = !s.cursor_blink;
                        OsdOut::SettingChanged
                    }
                    _ => {
                        // cfg-5 Font: cycle the baked cell advances (largest
                        // first). LEFT = one step, RIGHT/Enter = the other --
                        // the theme cycler's mechanics; the value label shows
                        // the resulting cell dims so the direction is
                        // unambiguous. Live-apply: the main loop rebuilds
                        // Metrics + cols/rows + winsize (renderer-local, cheap).
                        let adv = cornucopia::ADVANCES;
                        let i = adv.iter().position(|&a| a == s.font).unwrap_or(0);
                        let n = adv.len();
                        s.font = if code == KEY_LEFT {
                            adv[(i + n - 1) % n]
                        } else {
                            adv[(i + 1) % n]
                        };
                        OsdOut::FontChanged
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

        // Item rows from row 4: (label, value, live). Live rows carry the
        // < value > cyclers (white-on-bar / blue values); info rows are
        // dark gray. Display row 0 shows the PENDING mode choice; the
        // Resolution row is the live display truth beside it.
        let items: Vec<(String, String, bool)> = if self.sec == SEC_DISPLAY {
            let mode_val = match MODE_PRESETS[self.mode_sel % MODE_PRESETS.len()] {
                Mode::Auto => String::from("< auto >"),
                Mode::Fixed(mw, mh) => format!("< {} x {} >", mw, mh),
            };
            vec![
                (String::from("Mode"), mode_val, true),
                (
                    String::from("Resolution"),
                    format!("{} x {}", disp_w, disp_h),
                    false,
                ),
                (String::from("Zoom policy"), String::from("letterbox"), false),
            ]
        } else {
            // cfg-5: the Font row shows the resulting cell dims (self-
            // describing -- the config value is the advance, but the user
            // reads the cell it produces).
            let fa = cornucopia::Atlas::for_advance(s.font);
            vec![
                (
                    String::from("Theme"),
                    format!("< {} >", THEMES[s.theme % THEMES.len()].0),
                    true,
                ),
                (
                    String::from("Cursor blink"),
                    format!("< {} >", if s.cursor_blink { "on" } else { "off" }),
                    true,
                ),
                (
                    String::from("Font"),
                    format!("< {} x {} >", fa.cell_w(), fa.cell_h()),
                    true,
                ),
            ]
        };
        for (i, (label, value, live)) in items.iter().enumerate() {
            let r = pr + 4 + i * 2;
            if r + 1 >= pr + ph - 1 {
                break;
            }
            let selected = i == self.sel;
            let (lfg, vfg, bg) = if selected {
                (EGA_BLACK, if *live { EGA_WHITE } else { EGA_DGRAY }, EGA_CYAN)
            } else {
                (EGA_BLACK, if *live { EGA_BLUE } else { EGA_DGRAY }, EGA_GRAY)
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
        let hint = if self.sec == SEC_DISPLAY {
            " </> choose  Enter apply  Tab  Esc close "
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
    fn font_row_cycles_baked_sizes() {
        let mut o = Osd::new();
        let mut s = Settings::new();
        o.open_at(&s); // Appearance, sel 0 = Theme
        o.handle_key(KEY_DOWN, 1, &mut s); // -> Cursor blink (sel 1)
        o.handle_key(KEY_DOWN, 1, &mut s); // -> Font (sel 2, the cfg-5 row)
        assert_eq!(o.sel, 2);
        assert_eq!(s.font, cornucopia::DEFAULT_ADVANCE);
        // RIGHT steps to the next baked advance (index+1 in the largest-first
        // list = the next-smaller cell) and emits FontChanged (live-apply).
        assert!(matches!(o.handle_key(KEY_RIGHT, 1, &mut s), OsdOut::FontChanged));
        assert_eq!(s.font, cornucopia::ADVANCES[1]);
        // LEFT returns to the default (index 0).
        assert!(matches!(o.handle_key(KEY_LEFT, 1, &mut s), OsdOut::FontChanged));
        assert_eq!(s.font, cornucopia::ADVANCES[0]);
        // LEFT from index 0 wraps to the smallest baked size (the last entry).
        assert!(matches!(o.handle_key(KEY_LEFT, 1, &mut s), OsdOut::FontChanged));
        assert_eq!(s.font, cornucopia::ADVANCES[cornucopia::ADVANCES.len() - 1]);
        // Nav stays in bounds for the now-3-row Appearance section.
        o.handle_key(KEY_DOWN, 1, &mut s);
        assert!(o.sel < SEC_LEN[o.sec]);
    }

    #[test]
    fn display_info_rows_inert_and_nav_bounds() {
        let mut o = Osd::new();
        let mut s = Settings::new();
        o.open_at(&s);
        o.handle_key(KEY_TAB, 1, &mut s); // -> Display, sel 0 = Mode
        o.handle_key(KEY_DOWN, 1, &mut s); // -> Resolution (info)
        let theme0 = s.theme;
        assert!(matches!(o.handle_key(KEY_RIGHT, 1, &mut s), OsdOut::None));
        assert!(matches!(o.handle_key(KEY_ENTER, 1, &mut s), OsdOut::None));
        assert_eq!(s.theme, theme0, "info rows change nothing");
        // Nav clamps at the section bounds (no underflow/overflow).
        o.handle_key(KEY_UP, 1, &mut s);
        o.handle_key(KEY_UP, 1, &mut s);
        o.handle_key(KEY_DOWN, 1, &mut s);
        o.handle_key(KEY_DOWN, 1, &mut s);
        o.handle_key(KEY_DOWN, 1, &mut s);
        assert!(o.sel < SEC_LEN[o.sec]);
    }

    #[test]
    fn mode_row_cycles_pending_and_applies_on_enter_only() {
        let mut o = Osd::new();
        let mut s = Settings::new();
        o.open_at(&s); // seeds the pending choice from Auto -> index 0
        o.handle_key(KEY_TAB, 1, &mut s); // -> Display, sel 0 = Mode
        // </> cycles the PENDING choice without applying anything.
        assert!(matches!(o.handle_key(KEY_RIGHT, 1, &mut s), OsdOut::None));
        assert!(s.mode == Mode::Auto, "cycling never applies");
        // Enter emits the apply for the pending preset (1024x768 is
        // MODE_PRESETS[1]); the MAIN loop writes the gated ctl and
        // persists only on an accepted write -- not the OSD's job.
        assert!(matches!(
            o.handle_key(KEY_ENTER, 1, &mut s),
            OsdOut::ModeApply(Mode::Fixed(1024, 768))
        ));
        assert!(s.mode == Mode::Auto, "the OSD itself never mutates mode");
        // A close/reopen re-seeds the pending choice from the APPLIED
        // settings (a stale pending cycle must not survive) AND resets
        // to the first section/row -- the panel must reopen
        // deterministically (the E2E's keystroke recipes act on absolute
        // positions; a persisted section made Tab land on the WRONG
        // section and cycle themes instead of modes).
        s.mode = Mode::Fixed(1600, 900);
        o.open_at(&s);
        assert_eq!(o.mode_sel, 4, "re-seeded from the applied mode");
        assert_eq!(o.sec, SEC_APPEARANCE, "reopen resets the section");
        assert_eq!(o.sel, 0, "reopen resets the row");
    }
}
