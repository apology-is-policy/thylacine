// libutopia::palette -- the Pale Fire palette per UTOPIA-VISUAL.md.
//
// Four colours at v1.0; each named after its semantic role. The role
// names (not the hex values) are the stable interface -- a future v1.x
// retheming can change hex without sweeping every emission site.
//
// Per UTOPIA-VISUAL.md section 1:
//   Background  #0e1018  Cold near-black, faint blue cast
//   Foreground  #d8e4f4  Moonlight; all text, all output
//   Path        #8898b4  Receded steel; path segment only
//   Glyph       #e07840  Ember orange; the `⊢` turnstile only
//
// Programs compose with libutopia::ansi (the SGR emission primitives).

/// One semantic colour role in the Pale Fire palette. Programs use the
/// role rather than raw RGB so the four-colour discipline holds.
#[derive(Copy, Clone, Debug, Eq, PartialEq)]
pub enum Role {
    /// Background -- cold near-black. Terminals render the bg of every
    /// cell; disciplined programs rarely emit a bg-color escape.
    Background,
    /// Foreground -- moonlight. Default text colour. Disciplined
    /// programs rarely emit FG explicitly -- this is the terminal's
    /// default after a reset.
    Foreground,
    /// Path segment -- receded steel. Used for the prompt path,
    /// continuation glyph (`⋮`), and any text that should recede.
    Path,
    /// Glyph -- ember orange. The Pale Fire warm note; reserved
    /// strictly for the prompt `⊢` glyph.
    Glyph,
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

/// Background: `#0e1018` -- the Pale Fire backdrop.
pub const BG: Rgb = Rgb::new(0x0e, 0x10, 0x18);

/// Foreground: `#d8e4f4` -- moonlight; the default text colour.
pub const FG: Rgb = Rgb::new(0xd8, 0xe4, 0xf4);

/// Path segment: `#8898b4` -- receded steel; the prompt path colour.
pub const PATH: Rgb = Rgb::new(0x88, 0x98, 0xb4);

/// Glyph: `#e07840` -- ember orange; reserved for the `⊢` glyph.
pub const GLYPH: Rgb = Rgb::new(0xe0, 0x78, 0x40);

/// Resolve a Role to its RGB triple. Compile-time match; zero runtime
/// cost when the role is a literal.
pub const fn rgb_of(role: Role) -> Rgb {
    match role {
        Role::Background => BG,
        Role::Foreground => FG,
        Role::Path => PATH,
        Role::Glyph => GLYPH,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn hex_values_match_scripture() {
        // UTOPIA-VISUAL.md section 1 table -- bound for life of v1.0.
        assert_eq!(BG, Rgb::new(0x0e, 0x10, 0x18));
        assert_eq!(FG, Rgb::new(0xd8, 0xe4, 0xf4));
        assert_eq!(PATH, Rgb::new(0x88, 0x98, 0xb4));
        assert_eq!(GLYPH, Rgb::new(0xe0, 0x78, 0x40));
    }

    #[test]
    fn rgb_of_resolves_each_role() {
        assert_eq!(rgb_of(Role::Background), BG);
        assert_eq!(rgb_of(Role::Foreground), FG);
        assert_eq!(rgb_of(Role::Path), PATH);
        assert_eq!(rgb_of(Role::Glyph), GLYPH);
    }
}
