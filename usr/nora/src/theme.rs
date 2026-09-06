// nora::theme -- the editor's colour palette.
//
// The palette is a runtime value (`Palette`: 11 semantic roles plus one derived
// tint) rather than a fixed set of constants, so nora running under a Halcyon
// session can follow the session's theme instead of a hardcoded one. `BONFIRE`
// is the compiled default -- nora's own identity, and the console fallback. A
// session conveys its resolved role colours out of band and nora adopts them at
// startup via `set_palette` (s7a-2: /env/HALCYON_PALETTE + a nora dotfile).
// Until something adopts a palette, `active()` is `BONFIRE` and every style
// function reads through it, so an unthemed nora renders exactly as before.
//
// The role NAMES are nora-local; each maps 1:1 (by name) to a Halcyon
// `libhalcyon::theme` role -- the mapping is applied where the /env palette is
// parsed (s7a-2), not here. The Bonfire values are docs/UTOPIA-VISUAL.md U-2.
//
// (Pre-#124 these carried the retired U-1 "Pale Fire" cold values --
// `#0e1018`/`#d8e4f4`/`#8898b4`, the exact hexes UTOPIA-VISUAL.md section 8 flags
// as residue; corrected to Bonfire alongside the palette-box styles.)

use core::cell::UnsafeCell;

use kaua::style::{Attr, Color, Style};

use crate::syntax::HlClass;

/// The editor's colour roles. A program under a Halcyon session fills these from
/// the session's resolved palette (s7a-2); the console default is `BONFIRE`.
#[derive(Clone, Copy)]
pub struct Palette {
    /// Editor background -- Bonfire `bg` (warm near-black).
    pub bg: Color,
    /// Body text -- Bonfire `fg` (warm off-white).
    pub fg: Color,
    /// Line numbers / dim furniture -- Bonfire `fg_muted`.
    pub dim: Color,
    /// Accent / Normal-mode chip / cursor -- Bonfire `ember`.
    pub ember: Color,
    /// Status-bar + popup background -- Bonfire `surface` (lifted warm dark).
    pub bar: Color,
    /// Popup / divider border -- Bonfire `border`.
    pub border: Color,
    /// Insert-mode chip -- Bonfire `moss` (green).
    pub green: Color,
    /// Visual-mode chip + selection -- Bonfire `dusk` (purple).
    pub violet: Color,
    /// Command-mode chip -- Bonfire `sand` (amber).
    pub gold: Color,
    /// Buffer-tab strip -- Bonfire `slate` (keyword / blue ANSI; UTOPIA-VISUAL U-2).
    pub slate: Color,
    /// Diagnostic error -- Bonfire `cinnabar`, a warm rust that reads as ALARM
    /// against the ember accent without leaving the family (a raw ANSI red clashes).
    pub rust: Color,
    /// The debugger's stopped-line background -- a warm tint of `bg` toward
    /// `ember`. Not a session role (no palette conveys it): `BONFIRE` pins the
    /// Bonfire value and the from-roles constructor derives it (s7a-2).
    pub debug_bg: Color,
}

/// nora's own palette (docs/UTOPIA-VISUAL.md U-2) -- the compiled default and
/// the console (non-session) fallback. These are the exact pre-refactor role
/// values; `bonfire_roles_are_byte_pinned` guards them against silent drift.
pub const BONFIRE: Palette = Palette {
    bg: Color::Rgb(0x0e, 0x0c, 0x0c),
    fg: Color::Rgb(0xe4, 0xdd, 0xd8),
    dim: Color::Rgb(0x9a, 0x8f, 0x8a),
    ember: Color::Rgb(0xe0, 0x78, 0x40),
    bar: Color::Rgb(0x18, 0x0f, 0x0e),
    border: Color::Rgb(0x3a, 0x2a, 0x26),
    green: Color::Rgb(0xb8, 0xd0, 0x98),
    violet: Color::Rgb(0xa8, 0x98, 0xc8),
    gold: Color::Rgb(0xc8, 0xa8, 0x82),
    slate: Color::Rgb(0x8a, 0x9a, 0xc8),
    rust: Color::Rgb(0xd0, 0x5a, 0x4a),
    debug_bg: Color::Rgb(0x33, 0x1e, 0x12),
};

impl Palette {
    /// Apply `text` -- a `role=RRGGBB` list, one entry per line -- over this
    /// palette, role by role, returning the result. `role` is a semantic
    /// palette-role name (the /env/HALCYON_PALETTE and nora-dotfile vocabulary),
    /// `RRGGBB` six hex digits (no `#`). The role names are the Halcyon palette
    /// roles, each mapped to nora's own field:
    ///
    /// | role       | nora field | Halcyon `libhalcyon::theme` source |
    /// |------------|------------|------------------------------------|
    /// | `bg`       | `bg`       | `surface`                          |
    /// | `fg`       | `fg`       | `fg`                               |
    /// | `dim`      | `dim`      | `fg_muted`                         |
    /// | `accent`   | `ember`    | `ember`                            |
    /// | `surface`  | `bar`      | `status_bg`                        |
    /// | `border`   | `border`   | `border`                           |
    /// | `moss`     | `green`    | `syntax.moss`                      |
    /// | `dusk`     | `violet`   | `syntax.dusk`                      |
    /// | `sand`     | `gold`     | `syntax.sand`                      |
    /// | `slate`    | `slate`    | `syntax.slate`                     |
    /// | `cinnabar` | `rust`     | `syntax.cinnabar`                  |
    ///
    /// Unknown roles, malformed hex, comment lines (`#...`) and blank lines are
    /// ignored: a hostile or partial source degrades to the roles it could parse
    /// and never panics (nora runs under an untrusted session; KT-1 format-fuzz).
    /// The debugger stopped-line tint is NOT a role -- it is re-derived from the
    /// resulting `bg`/`ember` so it follows whatever theme is applied.
    pub fn with_overrides(mut self, text: &str) -> Palette {
        for line in text.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((name, hex)) = line.split_once('=') else {
                continue;
            };
            let Some(color) = parse_hex_rgb(hex.trim()) else {
                continue;
            };
            match name.trim() {
                "bg" => self.bg = color,
                "fg" => self.fg = color,
                "dim" => self.dim = color,
                "accent" => self.ember = color,
                "surface" => self.bar = color,
                "border" => self.border = color,
                "moss" => self.green = color,
                "dusk" => self.violet = color,
                "sand" => self.gold = color,
                "slate" => self.slate = color,
                "cinnabar" => self.rust = color,
                _ => {}
            }
        }
        self.debug_bg = blend(self.bg, self.ember, 3, 16);
        self
    }
}

/// Parse exactly six hex digits (`RRGGBB`, no `#`) into an RGB colour. Returns
/// `None` for any other length or a non-hex byte -- byte-indexed, so a 6-byte
/// non-ASCII input fails cleanly rather than panicking on a char boundary.
fn parse_hex_rgb(s: &str) -> Option<Color> {
    let b = s.as_bytes();
    if b.len() != 6 {
        return None;
    }
    let byte = |hi: usize, lo: usize| -> Option<u8> {
        Some((hex_nibble(b[hi])? << 4) | hex_nibble(b[lo])?)
    };
    Some(Color::Rgb(byte(0, 1)?, byte(2, 3)?, byte(4, 5)?))
}

/// A single hex digit's value, or `None` for a non-hex byte.
fn hex_nibble(c: u8) -> Option<u8> {
    match c {
        b'0'..=b'9' => Some(c - b'0'),
        b'a'..=b'f' => Some(c - b'a' + 10),
        b'A'..=b'F' => Some(c - b'A' + 10),
        _ => None,
    }
}

/// `a` moved `num/den` of the way toward `b`, per channel. A non-`Rgb` input
/// (never a real role) leaves `a` unchanged. Used to derive the debugger tint
/// from `bg` toward `ember`, so it follows any applied theme.
fn blend(a: Color, b: Color, num: u32, den: u32) -> Color {
    match (a, b) {
        (Color::Rgb(ar, ag, ab), Color::Rgb(br, bg, bb)) => Color::Rgb(
            mix_channel(ar, br, num, den),
            mix_channel(ag, bg, num, den),
            mix_channel(ab, bb, num, den),
        ),
        _ => a,
    }
}

/// One channel of `blend`: `x` moved `num/den` toward `y`, saturating in u8.
fn mix_channel(x: u8, y: u8, num: u32, den: u32) -> u8 {
    let x = x as u32;
    let y = y as u32;
    let v = if y >= x {
        x + (y - x) * num / den
    } else {
        x - (x - y) * num / den
    };
    v as u8
}

/// The active palette, held so every style function can read the roles in force.
/// nora is single-threaded and sets this ONCE at startup, before the first
/// render (`set_palette`); there is never a concurrent reader and writer, which
/// is what makes the `Sync` impl sound.
struct Active(UnsafeCell<Palette>);

// SAFETY: nora is single-threaded; `set_palette` runs once at startup, before
// any render reads `active()`. No concurrent access to the cell ever exists.
unsafe impl Sync for Active {}

static ACTIVE: Active = Active(UnsafeCell::new(BONFIRE));

/// The palette in force. Defaults to `BONFIRE` until `set_palette` adopts a
/// session palette at startup.
pub fn active() -> &'static Palette {
    // SAFETY: single-threaded, set-once-before-render (see `Active`).
    unsafe { &*ACTIVE.0.get() }
}

/// Adopt `p` as the active palette. Call ONCE, at startup, before the first
/// render: nora is single-threaded and this is deliberately not synchronised
/// for concurrent or mid-render use.
pub fn set_palette(p: Palette) {
    // SAFETY: single-threaded, called once at startup (see `Active`).
    unsafe {
        *ACTIVE.0.get() = p;
    }
}

/// The cursor's line gets a subtly lifted background (Bonfire `surface`) so the
/// active row stands out without a loud bar.
pub fn current_line() -> Style {
    let p = active();
    Style::new().fg(p.fg).bg(p.bar)
}

/// The line the debugger is stopped at -- a warm ember-tinted row, distinct from
/// the cursor's neutral surface, so "where execution is" reads at a glance
/// (NORA-IDE-UX section 2.3). Dark enough that body text stays readable on it.
pub fn debug_line() -> Style {
    let p = active();
    Style::new().fg(p.fg).bg(p.debug_bg)
}

/// The `▸` execution marker + line number on the stopped line (ember, bold).
pub fn debug_gutter() -> Style {
    let p = active();
    Style::new().fg(p.ember).bg(p.debug_bg).attr(Attr::BOLD)
}

/// The gutter number on the cursor's line -- brighter (ember) than the dim
/// furniture, over the current-line background.
pub fn current_gutter() -> Style {
    let p = active();
    Style::new().fg(p.ember).bg(p.bar).attr(Attr::BOLD)
}

/// The block cursor cell: the glyph under the cursor inverted onto the active
/// mode's accent colour (Normal ember / Insert moss / Visual dusk / Command sand).
pub fn cursor_block(accent: Color) -> Style {
    Style::new().fg(active().bg).bg(accent)
}

/// The buffer-tab strip fill (and inactive tabs sit on it): Bonfire slate.
pub fn tab_strip() -> Style {
    let p = active();
    Style::new().fg(p.bg).bg(p.slate)
}

/// The active buffer tab -- an ember chip on the slate strip, bold.
pub fn tab_active() -> Style {
    let p = active();
    Style::new().fg(p.bg).bg(p.ember).attr(Attr::BOLD)
}

/// An inactive buffer tab -- dark text on the slate strip.
pub fn tab_inactive() -> Style {
    let p = active();
    Style::new().fg(p.bg).bg(p.slate)
}

/// Body text over the editor background.
pub fn text() -> Style {
    let p = active();
    Style::new().fg(p.fg).bg(p.bg)
}

/// The foreground colour for a syntax-highlight class (the native lexer
/// highlighter, docs/KAUA.md section 12), drawn from the palette's syntax hues.
/// The caller composes it over the line's background (current-line / normal).
/// Text and (the not-yet-emitted) Operator fall back to the body `fg`.
pub fn syntax(class: HlClass) -> Color {
    let p = active();
    match class {
        HlClass::Text | HlClass::Operator => p.fg,
        HlClass::Keyword => p.slate,
        HlClass::Str => p.green,
        HlClass::Var => p.violet,
        HlClass::Comment => p.dim,
        HlClass::Number => p.gold,
    }
}

/// A blank editor cell (the background fill).
pub fn blank() -> Style {
    Style::new().bg(active().bg)
}

/// Gutter line numbers.
pub fn gutter() -> Style {
    let p = active();
    Style::new().fg(p.dim).bg(p.bg)
}

/// The gutter number on a line carrying an ERROR: rust, bold. Deliberately a
/// recolor of the existing number rather than an extra marker column -- the
/// gutter width is shared with the wrapped renderer and a width change would
/// reflow every visual row.
pub fn gutter_error() -> Style {
    let p = active();
    Style::new().fg(p.rust).bg(p.bg).attr(Attr::BOLD)
}

/// The gutter number on a line carrying a WARNING (or a lesser diagnostic).
pub fn gutter_warn() -> Style {
    let p = active();
    Style::new().fg(p.gold).bg(p.bg).attr(Attr::BOLD)
}

/// A diagnostic message shown on the status line (cursor sitting on the line).
pub fn status_error() -> Style {
    let p = active();
    Style::new().fg(p.rust).bg(p.bar)
}

pub fn status_warn() -> Style {
    let p = active();
    Style::new().fg(p.gold).bg(p.bar)
}

/// The `~` past-end-of-buffer markers (vim style).
pub fn tilde() -> Style {
    let p = active();
    Style::new().fg(p.dim).bg(p.bg)
}

/// The faint bottom-right `:help` nudge on a pristine buffer -- dim furniture
/// colour so it reads as a hint, never as content.
pub fn hint() -> Style {
    let p = active();
    Style::new().fg(p.dim).bg(p.bg)
}

/// The status-bar fill.
pub fn statusbar() -> Style {
    let p = active();
    Style::new().fg(p.fg).bg(p.bar)
}

/// A transient status message on the bar.
pub fn status_msg() -> Style {
    let p = active();
    Style::new().fg(p.ember).bg(p.bar)
}

/// The command/search line (`:`/`/`), drawn over the editor background.
pub fn cmdline() -> Style {
    let p = active();
    Style::new().fg(p.fg).bg(p.bg)
}

/// A visual-mode selected cell.
pub fn selection() -> Style {
    let p = active();
    Style::new().fg(p.bg).bg(p.violet)
}

/// A mode chip: dark text on the mode's accent colour, bold.
pub fn mode_chip(accent: Color) -> Style {
    Style::new().fg(active().bg).bg(accent).attr(Attr::BOLD)
}

/// The `[Space]` command-palette popup surface (entry text on Bonfire surface).
pub fn palette_surface() -> Style {
    let p = active();
    Style::new().fg(p.fg).bg(p.bar)
}

/// The palette popup border.
pub fn palette_border() -> Style {
    let p = active();
    Style::new().fg(p.border).bg(p.bar)
}

/// The palette popup title.
pub fn palette_title() -> Style {
    let p = active();
    Style::new().fg(p.ember).bg(p.bar).attr(Attr::BOLD)
}

/// The selected palette entry (a full-width highlight bar: dark text on ember).
pub fn palette_selected() -> Style {
    let p = active();
    Style::new().fg(p.bg).bg(p.ember).attr(Attr::BOLD)
}

/// A debugger dashboard tile border -- ember when the tile holds focus, dim
/// otherwise (the keyboard-focus cue, NORA-IDE-UX section 2.3).
pub fn tile_border(focused: bool) -> Style {
    let p = active();
    Style::new().fg(if focused { p.ember } else { p.border }).bg(p.bg)
}

/// A dashboard tile title -- ember+bold when focused, dim otherwise.
pub fn tile_title(focused: bool) -> Style {
    let p = active();
    let fg = if focused { p.ember } else { p.dim };
    Style::new().fg(fg).bg(p.bg).attr(Attr::BOLD)
}

/// A dashboard tile's body text (frame/variable/goroutine rows).
pub fn tile_text() -> Style {
    let p = active();
    Style::new().fg(p.fg).bg(p.bg)
}

/// A dashboard tile's dimmed label (a `Tree` group node, an inactive tab).
pub fn tile_dim() -> Style {
    let p = active();
    Style::new().fg(p.dim).bg(p.bg)
}

/// The selected row in a FOCUSED dashboard tile (a full-width ember highlight,
/// like the palette selection). Drawn only on the focused tile -- an unfocused
/// tile shows no cursor.
pub fn tile_selected() -> Style {
    let p = active();
    Style::new().fg(p.bg).bg(p.ember).attr(Attr::BOLD)
}

/// The Call Stack's `── kernel ──` divider row -- ember box-drawing marking the
/// user->kernel boundary (NORA-IDE-UX section 5). Ember is Bonfire's divider
/// accent, so the boundary is unmistakable without a bright line.
pub fn stack_kernel_divider() -> Style {
    let p = active();
    Style::new().fg(p.ember).bg(p.bg)
}

/// A kernel frame in the Call Stack -- dim, so the cross-boundary rows read as
/// furniture beneath the Go frames (NORA-IDE-UX section 5).
pub fn stack_kernel_frame() -> Style {
    let p = active();
    Style::new().fg(p.dim).bg(p.bg)
}

/// A tile scrollbar track (the dim `│` rail; shown only when the tile overflows).
pub fn tile_scroll_track() -> Style {
    let p = active();
    Style::new().fg(p.border).bg(p.bg)
}

/// A tile scrollbar thumb (the ember `█`, so the scroll position reads at a
/// glance against the dim track).
pub fn tile_scroll_thumb() -> Style {
    let p = active();
    Style::new().fg(p.ember).bg(p.bg)
}

#[cfg(test)]
mod tests {
    use super::*;

    // The compiled default is nora's identity and the console fallback; a silent
    // drift here would change every unthemed render. These are the exact
    // pre-refactor `const` role values.
    #[test]
    fn bonfire_roles_are_byte_pinned() {
        assert_eq!(BONFIRE.bg, Color::Rgb(0x0e, 0x0c, 0x0c));
        assert_eq!(BONFIRE.fg, Color::Rgb(0xe4, 0xdd, 0xd8));
        assert_eq!(BONFIRE.dim, Color::Rgb(0x9a, 0x8f, 0x8a));
        assert_eq!(BONFIRE.ember, Color::Rgb(0xe0, 0x78, 0x40));
        assert_eq!(BONFIRE.bar, Color::Rgb(0x18, 0x0f, 0x0e));
        assert_eq!(BONFIRE.border, Color::Rgb(0x3a, 0x2a, 0x26));
        assert_eq!(BONFIRE.green, Color::Rgb(0xb8, 0xd0, 0x98));
        assert_eq!(BONFIRE.violet, Color::Rgb(0xa8, 0x98, 0xc8));
        assert_eq!(BONFIRE.gold, Color::Rgb(0xc8, 0xa8, 0x82));
        assert_eq!(BONFIRE.slate, Color::Rgb(0x8a, 0x9a, 0xc8));
        assert_eq!(BONFIRE.rust, Color::Rgb(0xd0, 0x5a, 0x4a));
        assert_eq!(BONFIRE.debug_bg, Color::Rgb(0x33, 0x1e, 0x12));
    }

    // The default active palette is BONFIRE, so an unthemed nora renders exactly
    // as before the palette became a runtime value. Tests never call
    // `set_palette`: the host harness runs them in parallel threads and the
    // active palette is a shared global, so mutating it would race a concurrent
    // reader. The override path is exercised at boot (s7a-4).
    #[test]
    fn active_defaults_to_bonfire() {
        assert_eq!(active().bg, BONFIRE.bg);
        assert_eq!(active().ember, BONFIRE.ember);
        assert_eq!(active().rust, BONFIRE.rust);
        assert_eq!(active().debug_bg, BONFIRE.debug_bg);
    }

    // The style functions compose fg/bg/attr from the active palette: a
    // spot-check that the composition survived the const->field refactor.
    #[test]
    fn style_functions_compose_from_the_active_palette() {
        assert_eq!(text().fg, BONFIRE.fg);
        assert_eq!(text().bg, BONFIRE.bg);
        assert_eq!(current_line().bg, BONFIRE.bar);
        assert_eq!(debug_line().bg, BONFIRE.debug_bg);
        assert_eq!(syntax(HlClass::Keyword), BONFIRE.slate);
        assert_eq!(syntax(HlClass::Str), BONFIRE.green);
        assert_eq!(syntax(HlClass::Text), BONFIRE.fg);

        let chip = mode_chip(BONFIRE.ember);
        assert_eq!(chip.bg, BONFIRE.ember);
        assert_eq!(chip.fg, BONFIRE.bg);
        assert!(chip.attr.contains(Attr::BOLD));
    }

    // Every semantic role name maps to the RIGHT nora field (a mis-mapping --
    // accent painting bg, say -- would be caught by the distinct per-role hex).
    #[test]
    fn with_overrides_maps_every_role_to_its_field() {
        let text = "\
bg=010203
fg=040506
dim=070809
accent=0a0b0c
surface=0d0e0f
border=101112
moss=131415
dusk=161718
sand=191a1b
slate=1c1d1e
cinnabar=1f2021
";
        let p = BONFIRE.with_overrides(text);
        assert_eq!(p.bg, Color::Rgb(0x01, 0x02, 0x03));
        assert_eq!(p.fg, Color::Rgb(0x04, 0x05, 0x06));
        assert_eq!(p.dim, Color::Rgb(0x07, 0x08, 0x09));
        assert_eq!(p.ember, Color::Rgb(0x0a, 0x0b, 0x0c)); // accent -> ember
        assert_eq!(p.bar, Color::Rgb(0x0d, 0x0e, 0x0f)); // surface -> bar
        assert_eq!(p.border, Color::Rgb(0x10, 0x11, 0x12));
        assert_eq!(p.green, Color::Rgb(0x13, 0x14, 0x15)); // moss -> green
        assert_eq!(p.violet, Color::Rgb(0x16, 0x17, 0x18)); // dusk -> violet
        assert_eq!(p.gold, Color::Rgb(0x19, 0x1a, 0x1b)); // sand -> gold
        assert_eq!(p.slate, Color::Rgb(0x1c, 0x1d, 0x1e));
        assert_eq!(p.rust, Color::Rgb(0x1f, 0x20, 0x21)); // cinnabar -> rust
        // The debugger tint is re-derived from the NEW bg/ember, not the literal.
        assert_ne!(p.debug_bg, BONFIRE.debug_bg);
        assert_eq!(p.debug_bg, blend(p.bg, p.ember, 3, 16));
    }

    // A hostile / partial source degrades to the roles it could parse: comments,
    // blanks, unknown roles, and malformed hex are skipped, and unmentioned
    // roles keep their prior value. (nora runs under an untrusted session.)
    #[test]
    fn with_overrides_ignores_junk_and_keeps_unmentioned_roles() {
        let text = "\
# a comment
accent=ff8800

nope=123456
fg=zzzzzz
fg=00ff00
";
        let p = BONFIRE.with_overrides(text);
        assert_eq!(p.ember, Color::Rgb(0xff, 0x88, 0x00)); // the valid accent applied
        assert_eq!(p.fg, Color::Rgb(0x00, 0xff, 0x00)); // malformed skipped, valid wins
        assert_eq!(p.bg, BONFIRE.bg); // unmentioned role unchanged
        assert_eq!(p.slate, BONFIRE.slate); // unknown 'nope' touched nothing
    }

    // Precedence is "last source wins, role by role": the dotfile beats /env on
    // an overlapping role, while an /env-only role survives.
    #[test]
    fn with_overrides_last_source_wins_on_overlap() {
        let env = "accent=ff0000\nbg=111111\n";
        let dot = "accent=00ff00\n";
        let p = BONFIRE.with_overrides(env).with_overrides(dot);
        assert_eq!(p.ember, Color::Rgb(0x00, 0xff, 0x00)); // dotfile beat /env
        assert_eq!(p.bg, Color::Rgb(0x11, 0x11, 0x11)); // /env-only role survives
    }

    #[test]
    fn parse_hex_rgb_rejects_bad_input() {
        assert_eq!(parse_hex_rgb("aabbcc"), Some(Color::Rgb(0xaa, 0xbb, 0xcc)));
        assert_eq!(parse_hex_rgb("AABBCC"), Some(Color::Rgb(0xaa, 0xbb, 0xcc)));
        assert_eq!(parse_hex_rgb("abc"), None); // too short
        assert_eq!(parse_hex_rgb("aabbccdd"), None); // too long
        assert_eq!(parse_hex_rgb("gggggg"), None); // non-hex
        assert_eq!(parse_hex_rgb("#aabbc"), None); // stray '#'
        assert_eq!(parse_hex_rgb("ééé"), None); // 6 BYTES, non-ASCII: byte-indexed, no panic
    }

    // The derived debug tint sits between bg and ember (moved toward ember), so
    // it reads on whatever ground the theme sets -- a light bg here.
    #[test]
    fn the_debug_tint_stays_between_bg_and_ember() {
        let p = BONFIRE.with_overrides("bg=faf4e8\naccent=c05a2a\n");
        let (Color::Rgb(dr, _, _), Color::Rgb(br, _, _), Color::Rgb(er, _, _)) =
            (p.debug_bg, p.bg, p.ember)
        else {
            panic!("roles are always Rgb")
        };
        // bg.r (0xfa) > ember.r (0xc0), so the tint's red lies between them.
        assert!(er <= dr && dr <= br, "tint {dr:#x} between ember {er:#x} and bg {br:#x}");
    }
}
