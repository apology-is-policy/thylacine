// libutopia::palette -- the Bonfire palette per UTOPIA-VISUAL.md (U-2).
//
// Bonfire supersedes the U-1 *Pale Fire* palette: a warm shift of the same
// design logic. The background is a near-black with a barely-perceptible red
// cast (`#0e0c0c`), as if a bonfire a few hundred metres away shifts the dark
// without illuminating it; the foreground is a warm off-white; the accent is
// the same ember `#e07840`.
//
// The role NAMES (not the hex values) are the stable interface -- a future
// retheming changes hex without sweeping every emission site. Programs compose
// with libutopia::ansi (the SGR emission primitives).
//
// Per UTOPIA-VISUAL.md section 4.1: Utopia's OWN disciplined programs use only
// the four discipline roles -- `bg`, `fg`, `path`, `glyph`. The extended roles
// (the section 1.4 syntax palette + the section 1.5 diagnostic palette) are
// exposed here for third-party + host-editor (Helix) integration. The ONE
// disciplined-surface exception is the shell's command-line VALIDITY coloring
// (a live typing affordance, distinct from error OUTPUT, which stays
// context-coded): a resolvable command renders `fen`, an unresolvable one
// `cinnabar` (UTOPIA-VISUAL.md section 8, the #115c addendum).

/// One semantic colour role in the Bonfire palette. Programs use the role
/// rather than raw RGB so the palette discipline holds. Covers every name in
/// UTOPIA-VISUAL.md section 4.1.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Role {
    // -- Discipline roles (section 4.1): the only roles Utopia's own
    //    disciplined programs use. --
    /// Background -- warm near-black `#0e0c0c`. Terminals render the bg of
    /// every cell; disciplined programs rarely emit a bg-colour escape.
    Background,
    /// Foreground -- warm off-white `#e4ddd8`. Default text colour; primary
    /// text + all program output.
    Foreground,
    /// Path segment -- `#9a8f8a` (== `fg_muted`). The prompt path, the
    /// continuation glyph (`⋮`), and any text that should recede.
    Path,
    /// Glyph -- ember orange `#e07840`. The Bonfire fire; reserved for the
    /// prompt `⊢` glyph (and the cursor / active indicator).
    Glyph,

    // -- Foundation (section 1.1) + text scale (section 1.2) + accent (1.3). --
    /// Lifted warm dark `#180f0e` -- selection background, popup surfaces.
    Surface,
    /// `#2a1f1c` -- UI borders, ruler column, pane dividers.
    Gutter,
    /// `#c8bdb8` -- secondary text, inactive pane text, parameter names.
    FgDim,
    /// `#9a8f8a` -- prompt path, inactive UI chrome, mode indicators (alias of
    /// `Path`; both resolve to the same hex, per section 4.1).
    FgMuted,
    /// `#5a4e48` -- indent guides, decorative brackets, gutter annotations.
    FgSubtle,
    /// `#b85f2a` -- insert-mode cursor, secondary ember uses.
    EmberDim,

    // -- Syntax palette (section 1.4): host-editor / third-party use. --
    /// `#8a9ac8` -- keywords, control flow, storage modifiers (+ info ANSI).
    Slate,
    /// `#8ab8a8` -- types, structs, primitives, imports.
    Sage,
    /// `#c8a882` -- struct fields, object members, attributes (+ warning).
    Sand,
    /// `#b8d098` -- constants, macros, enum variants.
    Moss,
    /// `#b07060` -- function names, identifiers, git hashes.
    Ash,
    /// `#a898c8` -- string literals, interpolations.
    Dusk,
    /// `#7a8a7a` -- line and block comments.
    Smoke,

    // -- Diagnostic palette (section 1.5): the only roles carrying urgency. --
    /// `#c06050` -- errors, failing checks, dangerous operations. The shell's
    /// command-line validity coloring uses this for an unresolvable command.
    Cinnabar,
    /// `#6a9a6a` -- passing checks, successful operations. The shell's
    /// command-line validity coloring uses this for a resolvable command.
    Fen,
}

/// RGB triple -- a (red, green, blue) component triplet in 0..=255.
/// Consumed by libutopia::ansi to compose 24-bit SGR escapes.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub struct Rgb {
    pub r: u8,
    pub g: u8,
    pub b: u8,
}

impl Rgb {
    pub const fn new(r: u8, g: u8, b: u8) -> Self {
        Self { r, g, b }
    }
}

// Foundation (UTOPIA-VISUAL.md section 1.1).
/// Background: `#0e0c0c` -- warm near-black, the Bonfire backdrop.
pub const BG: Rgb = Rgb::new(0x0e, 0x0c, 0x0c);
/// Surface: `#180f0e` -- lifted warm dark.
pub const SURFACE: Rgb = Rgb::new(0x18, 0x0f, 0x0e);
/// Gutter: `#2a1f1c` -- UI borders, ruler column, pane dividers.
pub const GUTTER: Rgb = Rgb::new(0x2a, 0x1f, 0x1c);

// Text scale (section 1.2).
/// Foreground: `#e4ddd8` -- warm off-white; the default text colour.
pub const FG: Rgb = Rgb::new(0xe4, 0xdd, 0xd8);
/// `#c8bdb8` -- secondary text.
pub const FG_DIM: Rgb = Rgb::new(0xc8, 0xbd, 0xb8);
/// `#9a8f8a` -- prompt path / muted chrome.
pub const FG_MUTED: Rgb = Rgb::new(0x9a, 0x8f, 0x8a);
/// Path segment: `#9a8f8a` -- the prompt path colour (== `FG_MUTED`).
pub const PATH: Rgb = FG_MUTED;
/// `#5a4e48` -- indent guides, decorative brackets.
pub const FG_SUBTLE: Rgb = Rgb::new(0x5a, 0x4e, 0x48);

// Accent (section 1.3).
/// Glyph: `#e07840` -- ember orange; reserved for the `⊢` glyph + cursor.
pub const GLYPH: Rgb = Rgb::new(0xe0, 0x78, 0x40);
/// `#b85f2a` -- the dimmer ember stop.
pub const EMBER_DIM: Rgb = Rgb::new(0xb8, 0x5f, 0x2a);

// Syntax palette (section 1.4).
/// `#8a9ac8` -- keyword / info.
pub const SLATE: Rgb = Rgb::new(0x8a, 0x9a, 0xc8);
/// `#8ab8a8` -- type.
pub const SAGE: Rgb = Rgb::new(0x8a, 0xb8, 0xa8);
/// `#c8a882` -- member / warning.
pub const SAND: Rgb = Rgb::new(0xc8, 0xa8, 0x82);
/// `#b8d098` -- constant.
pub const MOSS: Rgb = Rgb::new(0xb8, 0xd0, 0x98);
/// `#b07060` -- function / identifier.
pub const ASH: Rgb = Rgb::new(0xb0, 0x70, 0x60);
/// `#a898c8` -- string.
pub const DUSK: Rgb = Rgb::new(0xa8, 0x98, 0xc8);
/// `#7a8a7a` -- comment.
pub const SMOKE: Rgb = Rgb::new(0x7a, 0x8a, 0x7a);

// Diagnostic palette (section 1.5).
/// Cinnabar: `#c06050` -- error / failing checks / an unresolvable command.
pub const CINNABAR: Rgb = Rgb::new(0xc0, 0x60, 0x50);
/// Fen: `#6a9a6a` -- success / passing checks / a resolvable command.
pub const FEN: Rgb = Rgb::new(0x6a, 0x9a, 0x6a);

/// Resolve a Role to its RGB triple. Compile-time match; zero runtime cost
/// when the role is a literal.
pub const fn rgb_of(role: Role) -> Rgb {
    match role {
        Role::Background => BG,
        Role::Foreground => FG,
        Role::Path => PATH,
        Role::Glyph => GLYPH,
        Role::Surface => SURFACE,
        Role::Gutter => GUTTER,
        Role::FgDim => FG_DIM,
        Role::FgMuted => FG_MUTED,
        Role::FgSubtle => FG_SUBTLE,
        Role::EmberDim => EMBER_DIM,
        Role::Slate => SLATE,
        Role::Sage => SAGE,
        Role::Sand => SAND,
        Role::Moss => MOSS,
        Role::Ash => ASH,
        Role::Dusk => DUSK,
        Role::Smoke => SMOKE,
        Role::Cinnabar => CINNABAR,
        Role::Fen => FEN,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_values_match_scripture() {
        // UTOPIA-VISUAL.md section 4.1 table -- bound for life of U-2.
        assert_eq!(BG, Rgb::new(0x0e, 0x0c, 0x0c));
        assert_eq!(FG, Rgb::new(0xe4, 0xdd, 0xd8));
        assert_eq!(PATH, Rgb::new(0x9a, 0x8f, 0x8a));
        assert_eq!(GLYPH, Rgb::new(0xe0, 0x78, 0x40));
        // The #115c command-line validity colours.
        assert_eq!(CINNABAR, Rgb::new(0xc0, 0x60, 0x50));
        assert_eq!(FEN, Rgb::new(0x6a, 0x9a, 0x6a));
        // PATH is the fg_muted alias.
        assert_eq!(PATH, FG_MUTED);
    }

    #[test]
    fn rgb_of_resolves_each_role() {
        assert_eq!(rgb_of(Role::Background), BG);
        assert_eq!(rgb_of(Role::Foreground), FG);
        assert_eq!(rgb_of(Role::Path), PATH);
        assert_eq!(rgb_of(Role::Glyph), GLYPH);
        assert_eq!(rgb_of(Role::Cinnabar), CINNABAR);
        assert_eq!(rgb_of(Role::Fen), FEN);
        assert_eq!(rgb_of(Role::Sage), SAGE);
    }
}
