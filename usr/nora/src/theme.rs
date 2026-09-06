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
}
