// nora::theme -- the Bonfire palette for the editor (docs/UTOPIA-VISUAL.md U-2).
//
// nora carries its own copies of the Bonfire RGB values rather than depend on
// the whole shell library (it needs only these). The constant NAMES are
// nora-local roles; each maps to a canonical Bonfire role (noted inline). The
// mode-chip accents reuse Bonfire's syntax/diagnostic hues (moss/dusk/sand).
//
// (Pre-#124 these carried the retired U-1 "Pale Fire" cold values --
// `#0e1018`/`#d8e4f4`/`#8898b4`, the exact hexes UTOPIA-VISUAL.md section 8 flags
// as residue; corrected to Bonfire here alongside the palette-box styles.)

use kaua::style::{Attr, Color, Style};

use crate::syntax::HlClass;

/// Editor background -- Bonfire `bg` (warm near-black).
pub const BG: Color = Color::Rgb(0x0e, 0x0c, 0x0c);
/// Body text -- Bonfire `fg` (warm off-white).
pub const FG: Color = Color::Rgb(0xe4, 0xdd, 0xd8);
/// Line numbers / dim furniture -- Bonfire `fg_muted`.
pub const DIM: Color = Color::Rgb(0x9a, 0x8f, 0x8a);
/// Accent / Normal-mode chip / cursor -- Bonfire `ember`.
pub const EMBER: Color = Color::Rgb(0xe0, 0x78, 0x40);
/// Status-bar + popup background -- Bonfire `surface` (lifted warm dark).
pub const BAR: Color = Color::Rgb(0x18, 0x0f, 0x0e);
/// Popup / divider border -- Bonfire `border`.
pub const BORDER: Color = Color::Rgb(0x3a, 0x2a, 0x26);
/// Insert-mode chip -- Bonfire `moss` (green).
pub const GREEN: Color = Color::Rgb(0xb8, 0xd0, 0x98);
/// Visual-mode chip + selection -- Bonfire `dusk` (purple).
pub const VIOLET: Color = Color::Rgb(0xa8, 0x98, 0xc8);
/// Command-mode chip -- Bonfire `sand` (amber).
pub const GOLD: Color = Color::Rgb(0xc8, 0xa8, 0x82);
/// Buffer-tab strip -- Bonfire `slate` (keyword / blue ANSI; UTOPIA-VISUAL U-2).
pub const SLATE: Color = Color::Rgb(0x8a, 0x9a, 0xc8);
/// Diagnostic error -- a warm rust that reads as ALARM against the ember accent
/// without leaving the Bonfire family (a raw ANSI red would clash with it).
pub const RUST: Color = Color::Rgb(0xd0, 0x5a, 0x4a);

/// The cursor's line gets a subtly lifted background (Bonfire `surface`) so the
/// active row stands out without a loud bar.
pub fn current_line() -> Style {
    Style::new().fg(FG).bg(BAR)
}

/// The line the debugger is stopped at -- a warm ember-tinted row, distinct from
/// the cursor's neutral surface, so "where execution is" reads at a glance
/// (NORA-IDE-UX section 2.3). Dark enough that body text stays readable on it.
pub fn debug_line() -> Style {
    Style::new().fg(FG).bg(Color::Rgb(0x33, 0x1e, 0x12))
}

/// The `▸` execution marker + line number on the stopped line (ember, bold).
pub fn debug_gutter() -> Style {
    Style::new().fg(EMBER).bg(Color::Rgb(0x33, 0x1e, 0x12)).attr(Attr::BOLD)
}

/// The gutter number on the cursor's line -- brighter (ember) than the dim
/// furniture, over the current-line background.
pub fn current_gutter() -> Style {
    Style::new().fg(EMBER).bg(BAR).attr(Attr::BOLD)
}

/// The block cursor cell: the glyph under the cursor inverted onto the active
/// mode's accent colour (Normal ember / Insert moss / Visual dusk / Command sand).
pub fn cursor_block(accent: Color) -> Style {
    Style::new().fg(BG).bg(accent)
}

/// The buffer-tab strip fill (and inactive tabs sit on it): Bonfire slate.
pub fn tab_strip() -> Style {
    Style::new().fg(BG).bg(SLATE)
}

/// The active buffer tab -- an ember chip on the slate strip, bold.
pub fn tab_active() -> Style {
    Style::new().fg(BG).bg(EMBER).attr(Attr::BOLD)
}

/// An inactive buffer tab -- dark text on the slate strip.
pub fn tab_inactive() -> Style {
    Style::new().fg(BG).bg(SLATE)
}

/// Body text over the editor background.
pub fn text() -> Style {
    Style::new().fg(FG).bg(BG)
}

/// The foreground colour for a syntax-highlight class (the native lexer
/// highlighter, docs/KAUA.md section 12), drawn from Bonfire's syntax hues. The
/// caller composes it over the line's background (current-line / normal). Text
/// and (the not-yet-emitted) Operator fall back to the body `fg`.
pub fn syntax(class: HlClass) -> Color {
    match class {
        HlClass::Text | HlClass::Operator => FG,
        HlClass::Keyword => SLATE,
        HlClass::Str => GREEN,
        HlClass::Var => VIOLET,
        HlClass::Comment => DIM,
        HlClass::Number => GOLD,
    }
}

/// A blank editor cell (the background fill).
pub fn blank() -> Style {
    Style::new().bg(BG)
}

/// Gutter line numbers.
pub fn gutter() -> Style {
    Style::new().fg(DIM).bg(BG)
}

/// The gutter number on a line carrying an ERROR: rust, bold. Deliberately a
/// recolor of the existing number rather than an extra marker column -- the
/// gutter width is shared with the wrapped renderer and a width change would
/// reflow every visual row.
pub fn gutter_error() -> Style {
    Style::new().fg(RUST).bg(BG).attr(Attr::BOLD)
}

/// The gutter number on a line carrying a WARNING (or a lesser diagnostic).
pub fn gutter_warn() -> Style {
    Style::new().fg(GOLD).bg(BG).attr(Attr::BOLD)
}

/// A diagnostic message shown on the status line (cursor sitting on the line).
pub fn status_error() -> Style {
    Style::new().fg(RUST).bg(BAR)
}

pub fn status_warn() -> Style {
    Style::new().fg(GOLD).bg(BAR)
}

/// The `~` past-end-of-buffer markers (vim style).
pub fn tilde() -> Style {
    Style::new().fg(DIM).bg(BG)
}

/// The faint bottom-right `:help` nudge on a pristine buffer -- dim furniture
/// colour so it reads as a hint, never as content.
pub fn hint() -> Style {
    Style::new().fg(DIM).bg(BG)
}

/// The status-bar fill.
pub fn statusbar() -> Style {
    Style::new().fg(FG).bg(BAR)
}

/// A transient status message on the bar.
pub fn status_msg() -> Style {
    Style::new().fg(EMBER).bg(BAR)
}

/// The command/search line (`:`/`/`), drawn over the editor background.
pub fn cmdline() -> Style {
    Style::new().fg(FG).bg(BG)
}

/// A visual-mode selected cell.
pub fn selection() -> Style {
    Style::new().fg(BG).bg(VIOLET)
}

/// A mode chip: dark text on the mode's accent colour, bold.
pub fn mode_chip(accent: Color) -> Style {
    Style::new().fg(BG).bg(accent).attr(Attr::BOLD)
}

/// The `[Space]` command-palette popup surface (entry text on Bonfire surface).
pub fn palette_surface() -> Style {
    Style::new().fg(FG).bg(BAR)
}

/// The palette popup border.
pub fn palette_border() -> Style {
    Style::new().fg(BORDER).bg(BAR)
}

/// The palette popup title.
pub fn palette_title() -> Style {
    Style::new().fg(EMBER).bg(BAR).attr(Attr::BOLD)
}

/// The selected palette entry (a full-width highlight bar: dark text on ember).
pub fn palette_selected() -> Style {
    Style::new().fg(BG).bg(EMBER).attr(Attr::BOLD)
}

/// A debugger dashboard tile border -- ember when the tile holds focus, dim
/// otherwise (the keyboard-focus cue, NORA-IDE-UX section 2.3).
pub fn tile_border(focused: bool) -> Style {
    Style::new().fg(if focused { EMBER } else { BORDER }).bg(BG)
}

/// A dashboard tile title -- ember+bold when focused, dim otherwise.
pub fn tile_title(focused: bool) -> Style {
    let fg = if focused { EMBER } else { DIM };
    Style::new().fg(fg).bg(BG).attr(Attr::BOLD)
}

/// A dashboard tile's body text (frame/variable/goroutine rows).
pub fn tile_text() -> Style {
    Style::new().fg(FG).bg(BG)
}

/// A dashboard tile's dimmed label (a `Tree` group node, an inactive tab).
pub fn tile_dim() -> Style {
    Style::new().fg(DIM).bg(BG)
}

/// The selected row in a FOCUSED dashboard tile (a full-width ember highlight,
/// like the palette selection). Drawn only on the focused tile -- an unfocused
/// tile shows no cursor.
pub fn tile_selected() -> Style {
    Style::new().fg(BG).bg(EMBER).attr(Attr::BOLD)
}

/// The Call Stack's `── kernel ──` divider row -- ember box-drawing marking the
/// user->kernel boundary (NORA-IDE-UX section 5). Ember is Bonfire's divider
/// accent, so the boundary is unmistakable without a bright line.
pub fn stack_kernel_divider() -> Style {
    Style::new().fg(EMBER).bg(BG)
}

/// A kernel frame in the Call Stack -- dim, so the cross-boundary rows read as
/// furniture beneath the Go frames (NORA-IDE-UX section 5).
pub fn stack_kernel_frame() -> Style {
    Style::new().fg(DIM).bg(BG)
}

/// A tile scrollbar track (the dim `│` rail; shown only when the tile overflows).
pub fn tile_scroll_track() -> Style {
    Style::new().fg(BORDER).bg(BG)
}

/// A tile scrollbar thumb (the ember `█`, so the scroll position reads at a
/// glance against the dim track).
pub fn tile_scroll_thumb() -> Style {
    Style::new().fg(EMBER).bg(BG)
}
