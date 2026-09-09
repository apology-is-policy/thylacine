// theme -- the whole coherent visual, as one type (docs/HALCYON-THEME.md).
//
// `Theme` is the single token source the ratified H-3 split names: halcyond's
// transcript Sheet + chrome surface AND tapestryd's pane bevel / hairline /
// cast-shadow all read a `&Theme` and nothing else. Since TH-1 it also carries
// the TERMINAL palette (foreign SGR's fg/bg + ANSI-16), which used to live as
// a hand-copied const in `vt`; since TH-2 nothing in production may name a
// theme CONSTANT at all -- see the visibility split below.
//
// Colours are `Argb` = 0xAARRGGBB with the alpha byte 0xFF (opaque) -- the
// pixel format the cartoon executor writes and tapestryd's chrome painter
// fills (the compositor's blank fill is `blank`, same convention).
//
// A second theme is a FILE, not a second const (HALCYON-THEME 3.3): the struct
// is theme-agnostic (HALCYON-VISUAL section 1.4/4/9 -- only the palette differs
// between themes), so Nocturne and Frutiger Aero are TOML, parsed into this
// exact shape. `DAYLIGHT` is the built-in floor, reachable through `builtin()`.

use alloc::string::String;
use core::fmt::Write as _;

/// 0xAARRGGBB, alpha 0xFF opaque.
pub type Argb = u32;

/// One live-tile status key (HALCYON-VISUAL section 1.4): the sage (exit 0) or
/// cinnabar (exit non-zero) family. `key` is the load-bearing colour --
/// separator, content hairline, active pill; the rest tint the tag strip.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct LiveKey {
    pub key: Argb,      // separator, content hairline, active pill, cast shadow tint
    pub tint: Argb,     // tag bar background
    pub raised: Argb,   // pill background
    pub border: Argb,   // vertical rule, muted pill stroke
    pub fg: Argb,       // tag name
    pub fg_dim: Argb,   // trailing metadata
    pub fg_muted: Argb, // muted pill text
}

/// The syntax palette (HALCYON-VISUAL section 1.5): content halcyon renders
/// itself. Content inside an embedded terminal is Bonfire's, not Daylight's.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Syntax {
    pub slate: Argb,    // keyword / info / OBJECT REFERENCE (the presentation colour)
    pub sage: Argb,     // type
    pub sand: Argb,     // member / warning
    pub moss: Argb,     // constant
    pub ash: Argb,      // function / identifier
    pub dusk: Argb,     // string
    pub smoke: Argb,    // comment
    pub fen: Argb,      // success
    pub cinnabar: Argb, // error
}

/// The full theme: every colour, the terminal palette, and the type stroke.
/// One instance per Halcyon theme, resolved once and threaded as `&Theme`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Theme {
    // Ground (section 1.1)
    pub floor: Argb,   // workspace floor; the bevel perceptual midpoint
    pub surface: Argb, // pane surface (parchment)
    pub header: Argb,  // tag bar bg; ALSO the inner hairline (section 2.4)
    pub raised: Argb,  // pill bg
    pub border: Argb,  // explicit strokes, tag-bar separators, the cast shadow
    // The compositor's fill for a pane with nothing in it yet. NOT a shade of
    // the ground: it is what the display shows before any client presents, and
    // a theme that leaves it behind paints a near-black hole in a light
    // workspace (it was `tapestryd::pane::BG_COLOR`, a literal, until TH-2).
    pub blank: Argb,
    // Transcript-only grounds the chrome scripture has no token for, promoted
    // here at TH-2 because a literal in a paint site is a Daylight colour that
    // survives a theme change -- the exact failure HALCYON-THEME exists to
    // close. `selection` is a warm step between surface and header;
    // `island_rule` is the mono island's left stroke (`.hal-out`'s
    // border-left, the one transcript stroke the mockup stylesheet carries as
    // a literal rather than a token).
    pub selection: Argb,
    pub island_rule: Argb,
    // Ink (section 1.2)
    pub fg: Argb,
    pub fg_dim: Argb,
    pub fg_muted: Argb,
    pub fg_subtle: Argb,
    // Bevel, NNW (section 2.1) -- a DERIVATION from the one light direction,
    // regenerated together or not at all (never adjust a single edge).
    pub bevel_top: Argb,    // key light, near-perpendicular
    pub bevel_left: Argb,   // grazing incidence
    pub bevel_right: Argb,  // facing away, some bounce
    pub bevel_bottom: Argb, // fully shadowed
    // Accent (section 1.3) -- the ember, shared verbatim with Bonfire.
    pub ember: Argb,      // prompt turnstile, caret, running indicator, active ws
    pub ember_dim: Argb,  // pill stroke on an active tile
    pub ember_deep: Argb, // separator under a resting pane's active tile
    // Live-tile keys (section 1.4)
    pub sage: LiveKey,
    pub cinnabar: LiveKey,
    // Syntax (section 1.5)
    pub syntax: Syntax,
    // Status bar (section 6)
    pub status_bg: Argb,
    pub status_fg: Argb,
    pub status_muted: Argb,
    pub status_idle: Argb,
    /// The chrome geometry (HALCYON-VISUAL section 3.1/4.3), as the LOGICAL
    /// base -- `metrics.at(pct)` is the scaled table both painters read. In
    /// the theme since TH-3b: a bevel's width and its four face colours are
    /// ONE decision, and a theme that moves the light without the geometry
    /// reads wrong (HALCYON-THEME 2).
    pub metrics: Metrics,
    // The terminal palette foreign SGR renders through (HALCYON-THEME 3.1):
    // the default fg/bg plus the ANSI-16. Halcyon's OWN output renders
    // through the Sheet, which is built from the chrome tokens above; this is
    // what a hosted program's escape sequences resolve against. It lives here
    // -- rather than as a const in `vt` -- because it is a THEME decision:
    // `vt` owns the type and the renderer, the theme owns the colours.
    pub terminal: vt::Palette,
    // Type (HALCYON-TYPE section 4.2): the smoothing stroke on every
    // proportional glyph, in THOUSANDTHS of an em (the doc's
    // `type_smooth_em` x 1000, kept integral so the theme stays `Eq`). The
    // Mac's comfort measured as an em-relative dilation; 12 (0.012 em) is
    // the fit's single constant across sizes. A dark ground takes 0: light
    // ink on dark already reads heavy (the doc's per-theme rule).
    pub smooth_mem: u16,
}

/// Chrome metrics (HALCYON-VISUAL section 3.1 / 4.3). Pixels.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Metrics {
    pub bevel: i32,       // pane bevel width (2)
    pub gap: i32,         // inter-pane gap AND workspace padding (2)
    pub hairline: i32,    // structural hairline (1)
    pub header_h: i32,    // tag bar height (20)
    pub status_h: i32,    // status bar height (20)
    pub tag_pad_x: i32,   // tag bar horizontal padding (6)
    pub tab_strip_h: i32, // tab/stack indicator strip (5); glyph-free, G-6c/D7
}

const METRICS_BASE: Metrics = Metrics {
    bevel: 2,
    gap: 2,
    hairline: 1,
    header_h: 20,
    status_h: 20,
    tag_pad_x: 6,
    tab_strip_h: 5,
};

/// The built-in geometry. A CONSTANT, so the TH-2 rule applies: production
/// reaches it through `Theme.metrics`, and this is the test fixture.
///
/// Stricter than `DAYLIGHT`'s split, and deliberately: nothing in this crate
/// reads it outside tests (the built-in theme is built from `METRICS_BASE`),
/// so in a production build it does not EXIST rather than merely being
/// private. Clippy noticing the dead arm is what made that clear.
#[cfg(feature = "theme-fixture")]
pub const METRICS: Metrics = METRICS_BASE;
#[cfg(all(test, not(feature = "theme-fixture")))]
pub(crate) const METRICS: Metrics = METRICS_BASE;

impl Metrics {
    /// THIS base table at a display scale (HALCYON-SCALE 5; the percent of
    /// `scale::scale_pct`): every size round-half-up scaled, the hairline
    /// never below 1 and the bevel never below 2 (COMPOSITION 1's
    /// structural-mark floors). The ONE function both the compositor's carve
    /// and halcyond's paint use, so the two cannot drift; `at(100)` is the
    /// base exactly (pinned by test).
    ///
    /// Takes `&self` since TH-3b: geometry is a THEME decision (a bevel's
    /// width and its four face colours are one decision), so the base comes
    /// from `Theme.metrics` rather than from a module constant.
    pub const fn at(&self, pct: u16) -> Metrics {
        let hair = crate::scale::ipx(self.hairline, pct);
        let bevel = crate::scale::ipx(self.bevel, pct);
        Metrics {
            bevel: if bevel < 2 { 2 } else { bevel },
            gap: crate::scale::ipx(self.gap, pct),
            hairline: if hair < 1 { 1 } else { hair },
            header_h: crate::scale::ipx(self.header_h, pct),
            status_h: crate::scale::ipx(self.status_h, pct),
            tag_pad_x: crate::scale::ipx(self.tag_pad_x, pct),
            tab_strip_h: crate::scale::ipx(self.tab_strip_h, pct),
        }
    }
}

/// The terminal palette for a theme on a LIGHT ground: the theme's own
/// surface and ink over `vt`'s proven light ANSI-16, with bright-white
/// aliased to the ink.
///
/// The alias is `vt`'s slot-uniqueness rule (its `PARCHMENT` note): within a
/// palette no two slots may share a value EXCEPT `ansi[15] == fg`, which every
/// theme carries so a `set_theme` remap maps it consistently. Written once
/// here so a second light theme inherits the rule instead of restating it.
pub const fn light_terminal(ground: Argb, ink: Argb) -> vt::Palette {
    let mut ansi = vt::PARCHMENT.ansi;
    ansi[15] = ink;
    vt::Palette {
        bg: ground,
        fg: ink,
        ansi,
    }
}

// Daylight's ground and ink, named because a const initializer cannot refer to
// the const it is defining and these two are each used twice within it -- once
// as a chrome token, once through `light_terminal`. One literal per colour is
// the whole point: the terminal palette is DERIVED from the chrome, not
// transcribed beside it.
const DAYLIGHT_SURFACE: Argb = 0xFFF2_EBE0;
const DAYLIGHT_INK: Argb = 0xFF1A_120A;

// The value. Always private; the two arms below decide who may NAME it.
const DAYLIGHT_THEME: Theme = Theme {
    floor: 0xFF8A_7660,
    surface: DAYLIGHT_SURFACE,
    header: 0xFFCE_C4B6,
    raised: 0xFFBD_B0A0,
    border: 0xFFA8_9880,
    blank: 0xFF10_1014,
    selection: 0xFFDF_D6C7,
    island_rule: 0xFF7A_6850,
    fg: DAYLIGHT_INK,
    fg_dim: 0xFF3A_2E22,
    fg_muted: 0xFF6A_5A48,
    fg_subtle: 0xFF9A_8878,
    bevel_top: 0xFFF8_F2E6,
    bevel_left: 0xFFE2_D6C0,
    bevel_right: 0xFF36_2410,
    bevel_bottom: 0xFF22_1405,
    ember: 0xFFE0_7840,
    ember_dim: 0xFFB8_5F2A,
    ember_deep: 0xFFC8_6030,
    sage: LiveKey {
        key: 0xFF1E_5844,
        tint: 0xFFB8_CCC4,
        raised: 0xFFA6_BDB4,
        border: 0xFF86_A096,
        fg: 0xFF0C_2820,
        fg_dim: 0xFF14_342A,
        fg_muted: 0xFF33_604F,
    },
    cinnabar: LiveKey {
        key: 0xFF98_2818,
        tint: 0xFFDC_B8B0,
        raised: 0xFFD0_A89E,
        border: 0xFFB8_8C80,
        fg: 0xFF3C_1008,
        fg_dim: 0xFF52_1A10,
        fg_muted: 0xFF7A_4034,
    },
    syntax: Syntax {
        slate: 0xFF3A_4878,
        sage: 0xFF1E_5844,
        sand: 0xFF7A_5020,
        moss: 0xFF3A_5818,
        ash: 0xFF6A_3828,
        dusk: 0xFF4A_3868,
        smoke: 0xFF6A_7060,
        fen: 0xFF1E_5828,
        cinnabar: 0xFF98_2818,
    },
    // Four independent roles in the scripture's own table (HALCYON-VISUAL
    // section 6), NOT derivations -- they equal the ink and the surface today,
    // and a theme may legitimately separate them. Literals, deliberately.
    status_bg: 0xFF1A_120A,
    status_fg: 0xFFF2_EBE0,
    status_muted: 0xFFC8_B89A,
    status_idle: 0xFF3A_2E22,
    metrics: METRICS_BASE,
    terminal: light_terminal(DAYLIGHT_SURFACE, DAYLIGHT_INK),
    smooth_mem: 12,
};

// NOTHING IN PRODUCTION MAY NAME A THEME CONSTANT (HALCYON-THEME 3.2). Every
// colour, stroke and geometry token is reached through a `&Theme` threaded
// from the ONE place that resolves it; a paint site that reaches for
// `DAYLIGHT` instead is a Daylight colour surviving a theme change, which is
// the failure this whole arc exists to close.
//
// Stating that rule in a comment is how it rots, so it is a VISIBILITY SPLIT:
// outside the `theme-fixture` feature the const is crate-private and a
// production reference does not COMPILE. Consumers enable the feature as a
// DEV-dependency, so their scripture-pinning tests keep it and their shipped
// binaries do not (resolver 2 keeps a dev-only feature out of `cargo build`
// -- the same mechanism TY-4 used for cornucopia's atlases).
//
// The loader's floor is `builtin()`, which is public on purpose: something
// must be able to say "no theme file, use the built-in".

/// Daylight (HALCYON-VISUAL section 1). Values are the doc's #rrggbb widened to
/// opaque Argb; the test below pins every one against the scripture.
#[cfg(feature = "theme-fixture")]
pub const DAYLIGHT: Theme = DAYLIGHT_THEME;
#[cfg(not(feature = "theme-fixture"))]
pub(crate) const DAYLIGHT: Theme = DAYLIGHT_THEME;

/// The built-in theme: the floor no installation can remove, and what the
/// loader falls back to when there is no theme file (HALCYON-THEME 4.1).
///
/// Call this at THE ONE PLACE that resolves a session's theme -- never at a
/// paint site, which must be handed the resolved `&Theme`. Until the loader
/// lands (TH-4) that one place is each renderer's startup.
pub const fn builtin() -> Theme {
    DAYLIGHT_THEME
}

// ---------------------------------------------------------------------------
// The theme FILE (HALCYON-THEME 3.3 / 4 / 5).
// ---------------------------------------------------------------------------

/// Every settable key, as `(table, key)`, in the order a `missing` report
/// names them.
///
/// This list and `set_key` below MUST agree: a key here that `set_key` does
/// not handle would be reported missing forever; a key `set_key` handles that
/// is not here would escape the completeness check. They sit beside each other
/// and beside the struct on purpose, and
/// `every_registered_key_is_settable_and_every_settable_key_is_registered`
/// proves the agreement in both directions rather than trusting it.
pub const KEYS: &[(&str, &str)] = &[
    ("palette", "floor"),
    ("palette", "surface"),
    ("palette", "header"),
    ("palette", "raised"),
    ("palette", "border"),
    ("palette", "blank"),
    ("palette", "selection"),
    ("palette", "island_rule"),
    ("palette", "fg"),
    ("palette", "fg_dim"),
    ("palette", "fg_muted"),
    ("palette", "fg_subtle"),
    ("palette", "bevel_top"),
    ("palette", "bevel_left"),
    ("palette", "bevel_right"),
    ("palette", "bevel_bottom"),
    ("palette", "ember"),
    ("palette", "ember_dim"),
    ("palette", "ember_deep"),
    ("palette", "status_bg"),
    ("palette", "status_fg"),
    ("palette", "status_muted"),
    ("palette", "status_idle"),
    ("palette.sage", "key"),
    ("palette.sage", "tint"),
    ("palette.sage", "raised"),
    ("palette.sage", "border"),
    ("palette.sage", "fg"),
    ("palette.sage", "fg_dim"),
    ("palette.sage", "fg_muted"),
    ("palette.cinnabar", "key"),
    ("palette.cinnabar", "tint"),
    ("palette.cinnabar", "raised"),
    ("palette.cinnabar", "border"),
    ("palette.cinnabar", "fg"),
    ("palette.cinnabar", "fg_dim"),
    ("palette.cinnabar", "fg_muted"),
    ("palette.syntax", "slate"),
    ("palette.syntax", "sage"),
    ("palette.syntax", "sand"),
    ("palette.syntax", "moss"),
    ("palette.syntax", "ash"),
    ("palette.syntax", "dusk"),
    ("palette.syntax", "smoke"),
    ("palette.syntax", "fen"),
    ("palette.syntax", "cinnabar"),
    ("terminal", "bg"),
    ("terminal", "fg"),
    ("terminal", "ansi"),
    ("type", "smooth"),
    ("geometry", "bevel"),
    ("geometry", "gap"),
    ("geometry", "hairline"),
    ("geometry", "header_h"),
    ("geometry", "status_h"),
    ("geometry", "tag_pad_x"),
    ("geometry", "tab_strip_h"),
];

/// What went wrong loading a theme file. Every variant carries the LINE, so
/// the refusal names where to look -- a theme is refused WHOLE (4.2), so the
/// author gets one place to fix rather than a half-applied visual to debug.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum LoadError {
    /// The file is not this TOML subset.
    Syntax(crate::toml::Error),
    /// A `[table]` this schema has no keys in.
    UnknownTable { line: u32 },
    /// A key this schema does not define -- refused, because a silently
    /// ignored key is a token the author believes they set.
    UnknownKey { line: u32 },
    /// A colour that is not `"#RRGGBB"`.
    BadColour { line: u32 },
    /// A value of the wrong shape for its key (a string where an integer
    /// belongs, an array where a colour belongs, an ANSI array that is not
    /// exactly 16 long).
    BadShape { line: u32 },
    /// A geometry or type value outside what the compositor's own bounds
    /// accept (4.4) -- a negative hairline, a bar taller than any display.
    OutOfRange { line: u32 },
    /// `base` names a theme that does not exist.
    UnknownBase { line: u32 },
    /// No `base`, so every key was required, and these were not set (4.3).
    /// Named, not counted: "some hardcoded daylight colour kicked in" is
    /// impossible when the loader tells you exactly which keys you owe.
    Incomplete { missing: alloc::vec::Vec<String> },
}

/// A theme file, loaded.
#[derive(Clone, Debug)]
pub struct Loaded {
    pub theme: Theme,
    /// `[meta] name`, or empty.
    pub name: String,
    /// The keys this file did NOT set, and therefore inherited from `base`.
    /// Empty when the file set everything. This is what `halcyon theme lint`
    /// reports (4.3), so the convenient mode stays auditable.
    pub inherited: alloc::vec::Vec<String>,
}

/// `"#RRGGBB"` -> opaque `Argb`. Rejects any other shape, INCLUDING
/// `#RRGGBBAA`: alpha is not a theme decision (3.3), so admitting it would
/// let a file set a translucency the pixel format cannot honour.
fn parse_colour(s: &str) -> Option<Argb> {
    let hex = s.strip_prefix('#')?;
    if hex.len() != 6 || !hex.bytes().all(|b| b.is_ascii_hexdigit()) {
        return None;
    }
    let mut v: u32 = 0;
    for c in hex.chars() {
        v = (v << 4) | c.to_digit(16)?;
    }
    Some(0xFF00_0000 | v)
}

/// The static bounds a geometry token must satisfy (4.4). These are SHAPE
/// bounds, not display bounds -- the loader cannot know the panel size, so
/// the compositor's own carve still has the last word; this only refuses
/// values that are wrong at any size. The floors match `Metrics::at`'s
/// (COMPOSITION 1: a hairline is at least 1 px, a bevel at least 2).
fn geometry_bounds(key: &str) -> (i64, i64) {
    match key {
        "bevel" => (2, 64),
        "gap" => (0, 64),
        "hairline" => (1, 32),
        "header_h" | "status_h" => (1, 256),
        "tag_pad_x" => (0, 128),
        "tab_strip_h" => (0, 128),
        _ => (0, 0),
    }
}

/// Apply one entry. `Ok(true)` = handled; `Ok(false)` = this schema has no
/// such key (the caller decides whether that is an unknown key or an unknown
/// table, so the error names the more useful of the two).
fn set_key(
    t: &mut Theme,
    table: &str,
    key: &str,
    v: &crate::toml::Value,
    line: u32,
) -> Result<bool, LoadError> {
    use crate::toml::Value;
    let colour = |v: &Value| -> Result<Argb, LoadError> {
        match v {
            Value::Str(s) => parse_colour(s).ok_or(LoadError::BadColour { line }),
            _ => Err(LoadError::BadShape { line }),
        }
    };
    match (table, key) {
        ("palette", "floor") => t.floor = colour(v)?,
        ("palette", "surface") => t.surface = colour(v)?,
        ("palette", "header") => t.header = colour(v)?,
        ("palette", "raised") => t.raised = colour(v)?,
        ("palette", "border") => t.border = colour(v)?,
        ("palette", "blank") => t.blank = colour(v)?,
        ("palette", "selection") => t.selection = colour(v)?,
        ("palette", "island_rule") => t.island_rule = colour(v)?,
        ("palette", "fg") => t.fg = colour(v)?,
        ("palette", "fg_dim") => t.fg_dim = colour(v)?,
        ("palette", "fg_muted") => t.fg_muted = colour(v)?,
        ("palette", "fg_subtle") => t.fg_subtle = colour(v)?,
        ("palette", "bevel_top") => t.bevel_top = colour(v)?,
        ("palette", "bevel_left") => t.bevel_left = colour(v)?,
        ("palette", "bevel_right") => t.bevel_right = colour(v)?,
        ("palette", "bevel_bottom") => t.bevel_bottom = colour(v)?,
        ("palette", "ember") => t.ember = colour(v)?,
        ("palette", "ember_dim") => t.ember_dim = colour(v)?,
        ("palette", "ember_deep") => t.ember_deep = colour(v)?,
        ("palette", "status_bg") => t.status_bg = colour(v)?,
        ("palette", "status_fg") => t.status_fg = colour(v)?,
        ("palette", "status_muted") => t.status_muted = colour(v)?,
        ("palette", "status_idle") => t.status_idle = colour(v)?,
        ("palette.sage", k) => match k {
            "key" => t.sage.key = colour(v)?,
            "tint" => t.sage.tint = colour(v)?,
            "raised" => t.sage.raised = colour(v)?,
            "border" => t.sage.border = colour(v)?,
            "fg" => t.sage.fg = colour(v)?,
            "fg_dim" => t.sage.fg_dim = colour(v)?,
            "fg_muted" => t.sage.fg_muted = colour(v)?,
            _ => return Ok(false),
        },
        ("palette.cinnabar", k) => match k {
            "key" => t.cinnabar.key = colour(v)?,
            "tint" => t.cinnabar.tint = colour(v)?,
            "raised" => t.cinnabar.raised = colour(v)?,
            "border" => t.cinnabar.border = colour(v)?,
            "fg" => t.cinnabar.fg = colour(v)?,
            "fg_dim" => t.cinnabar.fg_dim = colour(v)?,
            "fg_muted" => t.cinnabar.fg_muted = colour(v)?,
            _ => return Ok(false),
        },
        ("palette.syntax", k) => match k {
            "slate" => t.syntax.slate = colour(v)?,
            "sage" => t.syntax.sage = colour(v)?,
            "sand" => t.syntax.sand = colour(v)?,
            "moss" => t.syntax.moss = colour(v)?,
            "ash" => t.syntax.ash = colour(v)?,
            "dusk" => t.syntax.dusk = colour(v)?,
            "smoke" => t.syntax.smoke = colour(v)?,
            "fen" => t.syntax.fen = colour(v)?,
            "cinnabar" => t.syntax.cinnabar = colour(v)?,
            _ => return Ok(false),
        },
        ("terminal", "bg") => t.terminal.bg = colour(v)?,
        ("terminal", "fg") => t.terminal.fg = colour(v)?,
        ("terminal", "ansi") => match v {
            Value::Array(items) => {
                // Exactly 16. A short array would leave slots holding the
                // BASE theme's colours -- the half-applied palette again, and
                // the one place it could hide inside a single key.
                if items.len() != 16 {
                    return Err(LoadError::BadShape { line });
                }
                for (i, s) in items.iter().enumerate() {
                    t.terminal.ansi[i] = parse_colour(s).ok_or(LoadError::BadColour { line })?;
                }
            }
            _ => return Err(LoadError::BadShape { line }),
        },
        ("type", "smooth") => match v {
            // Thousandths of an em. 0 is the dark-theme value; 200 (0.2 em)
            // is far past any legible stroke and bounds the raster dilation.
            Value::Int(n) if (0..=200).contains(n) => t.smooth_mem = *n as u16,
            Value::Int(_) => return Err(LoadError::OutOfRange { line }),
            _ => return Err(LoadError::BadShape { line }),
        },
        ("geometry", k) => {
            let n = match v {
                Value::Int(n) => *n,
                _ => return Err(LoadError::BadShape { line }),
            };
            let (lo, hi) = geometry_bounds(k);
            if lo == 0 && hi == 0 {
                return Ok(false);
            }
            if n < lo || n > hi {
                return Err(LoadError::OutOfRange { line });
            }
            let n = n as i32;
            match k {
                "bevel" => t.metrics.bevel = n,
                "gap" => t.metrics.gap = n,
                "hairline" => t.metrics.hairline = n,
                "header_h" => t.metrics.header_h = n,
                "status_h" => t.metrics.status_h = n,
                "tag_pad_x" => t.metrics.tag_pad_x = n,
                "tab_strip_h" => t.metrics.tab_strip_h = n,
                _ => return Ok(false),
            }
        }
        _ => return Ok(false),
    }
    Ok(true)
}

/// The tables this schema knows, so an unknown one is named as a TABLE error
/// rather than as an unknown key in a table that does not exist.
const TABLES: &[&str] = &[
    "meta",
    "palette",
    "palette.sage",
    "palette.cinnabar",
    "palette.syntax",
    "terminal",
    "type",
    "geometry",
];

impl Theme {
    /// Load a theme from a TOML file's text (HALCYON-THEME 3.3).
    ///
    /// `[meta] base = "daylight"` inherits every unset key. **Omitting
    /// `base` requires every key**, and a file that misses some is refused
    /// with those keys NAMED (4.3) -- which is what makes "some hardcoded
    /// Daylight colour kicked in" impossible for a serious theme rather than
    /// merely unlikely.
    ///
    /// A file is accepted WHOLE or refused whole (4.2). Nothing here mutates
    /// a caller's theme: the result is a new value, so a refusal cannot leave
    /// a half-applied visual behind.
    pub fn from_toml(src: &str) -> Result<Loaded, LoadError> {
        let entries = crate::toml::parse(src).map_err(LoadError::Syntax)?;

        // `base` first: it decides the starting point AND whether the file
        // must be complete, so it cannot be read in file order.
        let mut based = false;
        let mut name = String::new();
        for e in &entries {
            if e.table != "meta" {
                continue;
            }
            match (e.key, &e.value) {
                ("base", crate::toml::Value::Str("daylight")) => based = true,
                ("base", crate::toml::Value::Str(_)) => {
                    return Err(LoadError::UnknownBase { line: e.line })
                }
                ("base", _) => return Err(LoadError::BadShape { line: e.line }),
                ("name", crate::toml::Value::Str(s)) => name = String::from(*s),
                ("name", _) => return Err(LoadError::BadShape { line: e.line }),
                _ => return Err(LoadError::UnknownKey { line: e.line }),
            }
        }

        // Without a base, start from the built-in ANYWAY: every key is then
        // required, so no built-in value survives into the result, and this
        // keeps the type total without an all-fields-`Option` mirror of the
        // struct. That claim rests on KEYS covering every field -- which is
        // guarded by `the_registry_covers_every_field` (a size pin: adding a
        // field changes `size_of::<Theme>()` and fails the test, pointing the
        // author at KEYS), not merely intended.
        let mut t = builtin();
        let mut seen = [false; 64];
        debug_assert!(KEYS.len() <= 64);

        for e in &entries {
            if e.table == "meta" {
                continue;
            }
            if !TABLES.contains(&e.table) {
                // The HEADER's line: a mistyped `[palete]` is one typo, and
                // pointing at the first colour inside it sends the author to
                // the wrong line.
                return Err(LoadError::UnknownTable { line: e.table_line });
            }
            if !set_key(&mut t, e.table, e.key, &e.value, e.line)? {
                return Err(LoadError::UnknownKey { line: e.line });
            }
            if let Some(i) = KEYS
                .iter()
                .position(|(tb, k)| *tb == e.table && *k == e.key)
            {
                seen[i] = true;
            }
        }

        let mut missing: alloc::vec::Vec<String> = alloc::vec::Vec::new();
        for (i, (tb, k)) in KEYS.iter().enumerate() {
            if !seen[i] {
                missing.push(key_name(tb, k));
            }
        }
        if !based && !missing.is_empty() {
            return Err(LoadError::Incomplete { missing });
        }
        Ok(Loaded {
            theme: t,
            name,
            inherited: missing,
        })
    }
}

/// The `table.key` label a `missing` report names.
///
/// BUILT rather than stored as a third registry column: `fg` alone appears in
/// four tables, so the bare key would be ambiguous, and a duplicated literal
/// could drift from the pair it labels. This costs one short allocation per
/// missing key, on an error path only.
fn key_name(table: &str, key: &str) -> String {
    let mut s = String::from(table);
    s.push('.');
    s.push_str(key);
    s
}

/// The inner hairline (section 2.4) is `header` by construction -- it vanishes
/// alongside a tag bar and shows only against content. One name for the intent.
pub const fn hairline(t: &Theme) -> Argb {
    t.header
}

/// The transcript's vt palette: Daylight's own `terminal`.
///
/// It AGREES with the `Sheet` built from `DAYLIGHT` (bg == surface, fg == fg)
/// BY CONSTRUCTION since HALCYON-THEME TH-1 -- `light_terminal` is handed the
/// same two consts the chrome tokens use, so there is nothing left to drift.
/// The agreement is load-bearing: halcyond's "default ink" test
/// (`st.fg == sheet.ink`, the hook that applies the obj/dim semantic colours)
/// only fires when the pen's default fg -- which comes from THIS palette --
/// equals `sheet.ink`. Foreign-program SGR renders through this; halcyon's own
/// output renders through the Sheet.
pub const fn daylight_palette() -> vt::Palette {
    DAYLIGHT.terminal
}

/// The session palette as the `role=RRGGBB` text a Halcyon session publishes to
/// `/env/HALCYON_PALETTE`, resolved from `theme`. The role names are the
/// program-agnostic Halcyon palette roles; a hosted pts program maps them to
/// its own fields (e.g. `nora`'s `theme::Palette::with_overrides`). This is the
/// WRITE side of the seam `vt`'s palette comment named for v1.x -- the
/// compositor plumbs its resolved palette to the programs it hosts. Each role's
/// `Argb` is emitted as `RRGGBB` (the opaque alpha byte is dropped).
///
/// The `surface` role -- a lifted PANEL a program paints its own dark ink on
/// (nora's status bar, popups, current-line) -- resolves from `header`, NOT
/// `status_bg`: `status_bg` is Halcyon's own dark bottom strip (worn with the
/// light `status_fg`), so a program that paints its `fg` on it would render
/// dark-on-dark. `header` is the light lift that keeps that contrast.
pub fn env_palette(theme: &Theme) -> String {
    let roles: [(&str, Argb); 11] = [
        ("bg", theme.surface),
        ("fg", theme.fg),
        ("dim", theme.fg_muted),
        ("accent", theme.ember),
        ("surface", theme.header),
        ("border", theme.border),
        ("moss", theme.syntax.moss),
        ("dusk", theme.syntax.dusk),
        ("sand", theme.syntax.sand),
        ("slate", theme.syntax.slate),
        ("cinnabar", theme.syntax.cinnabar),
    ];
    let mut s = String::new();
    for (name, argb) in roles {
        // writeln! into a String is infallible; the `let _` documents that.
        let _ = writeln!(s, "{}={:06x}", name, argb & 0x00FF_FFFF);
    }
    s
}

/// `env_palette(&DAYLIGHT)` -- the session's Daylight roles as /env text.
pub fn daylight_env_palette() -> String {
    env_palette(&DAYLIGHT)
}

#[cfg(test)]
mod tests {
    use super::*;

    // ---- the theme file (HALCYON-THEME 3.3 / 4) ----

    /// A minimal file setting exactly one key, for the registry sweep.
    fn only(table: &str, key: &str, val: &str) -> String {
        let mut s = String::from("[meta]\nbase = \"daylight\"\n[");
        s.push_str(table);
        s.push_str("]\n");
        s.push_str(key);
        s.push_str(" = ");
        s.push_str(val);
        s.push('\n');
        s
    }

    fn sample(key: &str, table: &str) -> &'static str {
        match (table, key) {
            (_, "ansi") => {
                "[\"#010101\", \"#020202\", \"#030303\", \"#040404\", \"#050505\", \
                 \"#060606\", \"#070707\", \"#080808\", \"#090909\", \"#0a0a0a\", \
                 \"#0b0b0b\", \"#0c0c0c\", \"#0d0d0d\", \"#0e0e0e\", \"#0f0f0f\", \
                 \"#101010\"]"
            }
            ("type", _) => "7",
            ("geometry", "hairline") => "2",
            ("geometry", "bevel") => "3",
            ("geometry", _) => "9",
            _ => "\"#123456\"",
        }
    }

    // THE REGISTRY AND THE SETTER MUST AGREE, BOTH WAYS. A key in KEYS that
    // `set_key` ignores would be reported missing forever; a key `set_key`
    // handles that is not in KEYS would escape the completeness check.
    // Neither is visible from one side alone.
    #[test]
    fn every_registered_key_is_settable_and_every_settable_key_is_registered() {
        for (table, key) in KEYS {
            let src = only(table, key, sample(key, table));
            let l = Theme::from_toml(&src)
                .unwrap_or_else(|e| panic!("KEYS has {table}.{key} but it did not load: {e:?}"));
            let want = key_name(table, key);
            assert!(
                !l.inherited.contains(&want),
                "{table}.{key} loaded but was still reported inherited"
            );
        }
        assert_eq!(KEYS.len(), 57, "the registry size is DERIVED, not claimed");
        let mut full = String::new();
        let mut last = "";
        for (table, key) in KEYS {
            if *table != last {
                full.push('[');
                full.push_str(table);
                full.push_str("]\n");
                last = table;
            }
            full.push_str(key);
            full.push_str(" = ");
            full.push_str(sample(key, table));
            full.push('\n');
        }
        let l = Theme::from_toml(&full).expect("a file setting every key needs no base");
        assert!(l.inherited.is_empty(), "inherited: {:?}", l.inherited);
    }

    // THE GUARD BEHIND "a baseless file inherits nothing". `from_toml` starts
    // from the built-in even with no base, so the claim that no built-in value
    // survives rests ENTIRELY on KEYS covering every field. A field added to
    // `Theme` and forgotten in KEYS would inherit Daylight silently, in exactly
    // the mode meant to make that impossible -- and no behavioural test can see
    // it, because the field it would have to check is the one nobody wrote.
    //
    // So: pin the STRUCT SIZE. Adding a field changes it, this fails, and the
    // message says where to look. The compile-time-invariant pattern CLAUDE.md
    // prescribes for format changes, applied to a registry instead.
    #[test]
    fn the_registry_covers_every_field() {
        assert_eq!(
            core::mem::size_of::<Theme>(),
            288,
            "Theme changed size -- a field was added or removed. Add it to \
             KEYS and to `set_key` (both, in both directions), update the \
             KEYS.len() pin, then update this number."
        );
        assert_eq!(core::mem::size_of::<Metrics>(), 28, "7 x i32");
        assert_eq!(core::mem::size_of::<LiveKey>(), 28, "7 x Argb");
        assert_eq!(core::mem::size_of::<Syntax>(), 36, "9 x Argb");
    }

    // 4.3, the operator's actual worry: omit `base` and the file must set
    // EVERYTHING, and a miss is NAMED rather than filled in from Daylight.
    #[test]
    fn a_baseless_file_that_misses_a_key_is_refused_and_names_it() {
        let mut full = String::new();
        let mut last = "";
        for (table, key) in KEYS {
            if *table == "geometry" && *key == "hairline" {
                continue; // the one omission
            }
            if *table != last {
                full.push('[');
                full.push_str(table);
                full.push_str("]\n");
                last = table;
            }
            full.push_str(key);
            full.push_str(" = ");
            full.push_str(sample(key, table));
            full.push('\n');
        }
        match Theme::from_toml(&full) {
            Err(LoadError::Incomplete { missing }) => {
                assert_eq!(missing, alloc::vec!["geometry.hairline"]);
            }
            other => panic!("expected Incomplete, got {other:?}"),
        }
        // The control: the SAME file with a base is accepted, and reports the
        // omission as inherited rather than refusing it.
        let based = alloc::format!("[meta]\nbase = \"daylight\"\n{full}");
        let l = Theme::from_toml(&based).expect("with a base, an omission inherits");
        assert_eq!(l.inherited, alloc::vec!["geometry.hairline"]);
        assert_eq!(
            l.theme.metrics.hairline, METRICS.hairline,
            "the inherited key really came from the base"
        );
    }

    #[test]
    fn a_based_file_retints_only_what_it_names() {
        let src = "[meta]\nname = \"Half\"\nbase = \"daylight\"\n\
                   [palette]\nsurface = \"#101010\"\n[geometry]\nbevel = 4\n";
        let l = Theme::from_toml(src).unwrap();
        assert_eq!(l.name, "Half");
        assert_eq!(l.theme.surface, 0xFF101010);
        assert_eq!(l.theme.metrics.bevel, 4);
        assert_eq!(l.theme.fg, DAYLIGHT.fg, "unset keys are the base's");
        assert_eq!(l.inherited.len(), KEYS.len() - 2);
    }

    // 4.2: every refusal, each with the POSITIVE control one variable away.
    #[test]
    fn a_malformed_theme_file_is_refused_whole_and_names_its_line() {
        let head = "[meta]\nbase = \"daylight\"\n";
        let cases: &[(&str, LoadError)] = &[
            (
                "[palette]\nsurface = \"1A1714\"\n",
                LoadError::BadColour { line: 4 },
            ),
            (
                "[palette]\nsurface = \"#1A171\"\n",
                LoadError::BadColour { line: 4 },
            ),
            (
                "[palette]\nsurface = \"#1A1714FF\"\n",
                LoadError::BadColour { line: 4 },
            ),
            (
                "[palette]\nsurface = \"#GGGGGG\"\n",
                LoadError::BadColour { line: 4 },
            ),
            ("[palette]\nsurface = 5\n", LoadError::BadShape { line: 4 }),
            (
                "[palette]\nnope = \"#101010\"\n",
                LoadError::UnknownKey { line: 4 },
            ),
            ("[nope]\nx = 1\n", LoadError::UnknownTable { line: 3 }),
            (
                "[geometry]\nhairline = 0\n",
                LoadError::OutOfRange { line: 4 },
            ),
            (
                "[geometry]\nhairline = -1\n",
                LoadError::OutOfRange { line: 4 },
            ),
            ("[geometry]\nbevel = 1\n", LoadError::OutOfRange { line: 4 }),
            (
                "[geometry]\nheader_h = 9999\n",
                LoadError::OutOfRange { line: 4 },
            ),
            (
                "[geometry]\nbevel = \"3\"\n",
                LoadError::BadShape { line: 4 },
            ),
            ("[type]\nsmooth = 500\n", LoadError::OutOfRange { line: 4 }),
            (
                "[terminal]\nansi = [\"#010101\"]\n",
                LoadError::BadShape { line: 4 },
            ),
            (
                "[terminal]\nansi = \"#010101\"\n",
                LoadError::BadShape { line: 4 },
            ),
        ];
        for (tail, want) in cases {
            let src = alloc::format!("{head}{tail}");
            assert_eq!(
                Theme::from_toml(&src).err(),
                Some(want.clone()),
                "for {tail:?}"
            );
        }
        for tail in [
            "[palette]\nsurface = \"#1A1714\"\n",
            "[geometry]\nhairline = 1\n",
            "[geometry]\nbevel = 2\n",
            "[type]\nsmooth = 0\n",
        ] {
            let src = alloc::format!("{head}{tail}");
            assert!(
                Theme::from_toml(&src).is_ok(),
                "control failed for {tail:?}"
            );
        }
    }

    // A 15-slot ANSI array is the one place a half-applied palette could hide
    // INSIDE a single key: the missing slot would silently keep the base's.
    #[test]
    fn a_short_ansi_array_is_refused_rather_than_partly_applied() {
        let mut a = String::from("[meta]\nbase = \"daylight\"\n[terminal]\nansi = [");
        for i in 0..15u32 {
            let _ = core::fmt::write(&mut a, format_args!("\"#{:02x}{:02x}{:02x}\",", i, i, i));
        }
        a.push_str("]\n");
        assert!(matches!(
            Theme::from_toml(&a),
            Err(LoadError::BadShape { .. })
        ));
        // Build the 16th in rather than `replace("]\n", ..)`, which would
        // also hit the `[meta]` and `[terminal]` HEADERS.
        let ok = alloc::format!("{}\"#0f0f0f\"]\n", &a[..a.len() - 2]);
        let l = Theme::from_toml(&ok).expect("16 is the legal length");
        assert_eq!(l.theme.terminal.ansi[0], 0xFF000000);
        assert_eq!(l.theme.terminal.ansi[15], 0xFF0F0F0F);
    }

    #[test]
    fn an_unknown_base_is_named_as_such() {
        let e = Theme::from_toml("[meta]\nbase = \"twilight\"\n").err();
        assert_eq!(e, Some(LoadError::UnknownBase { line: 2 }));
        let e = Theme::from_toml("[meta]\nnope = 1\n").err();
        assert_eq!(e, Some(LoadError::UnknownKey { line: 2 }));
    }

    #[test]
    fn a_syntax_error_carries_the_parsers_line() {
        let e = Theme::from_toml("[meta]\nbase = \"daylight\"\n[palette\n").err();
        assert_eq!(
            e,
            Some(LoadError::Syntax(crate::toml::Error {
                line: 3,
                kind: crate::toml::Kind::BadTable
            }))
        );
    }

    // Loading never mutates a caller's theme: a refusal cannot leave a
    // half-applied visual behind (4.2).
    #[test]
    fn a_refusal_leaves_nothing_behind() {
        let before = builtin();
        let _ = Theme::from_toml(
            "[meta]\nbase = \"daylight\"\n[palette]\nsurface = \"#101010\"\nfg = \"nope\"\n",
        );
        assert!(builtin() == before, "the built-in is a const, not a target");
        let l = Theme::from_toml("[meta]\nbase = \"daylight\"\n[palette]\nsurface = \"#101010\"\n")
            .unwrap();
        assert_eq!(l.theme.surface, 0xFF101010);
        assert_eq!(builtin().surface, before.surface);
    }

    // The terminal palette IS the theme's ground and ink (HALCYON-THEME 3.1).
    // Before TH-1 this compared two independently-written consts in two
    // crates; now it states one relationship the construction already
    // guarantees, which is what makes it cheap to keep: it fails only if
    // someone gives `terminal` a hand-written value again.
    #[test]
    fn the_terminal_palette_is_the_theme_ground_and_ink() {
        let p = daylight_palette();
        assert_eq!(p.bg, DAYLIGHT.surface, "bg is the surface");
        assert_eq!(p.fg, DAYLIGHT.fg, "fg is the ink");
        let mut want = vt::PARCHMENT.ansi;
        want[15] = DAYLIGHT.fg;
        assert_eq!(p.ansi, want, "the light ANSI-16 with bright-white aliased");
        // vt's slot-uniqueness rule: within a palette no two slots share a
        // value EXCEPT ansi[15] == fg. A theme that broke it would mis-slot
        // cells across a `set_theme` remap.
        for (i, c) in p.ansi.iter().enumerate() {
            for (j, d) in p.ansi.iter().enumerate() {
                assert!(i == j || c != d, "ansi[{i}] and ansi[{j}] share a value");
            }
            assert!(i == 15 || *c != p.fg, "ansi[{i}] aliases fg but is not 15");
        }
    }

    // The seam a compositor uses to TELL a producer its theme: Daylight must
    // survive the argv round trip, or a session tile's cells are born in a
    // different palette than the transcript beside them.
    #[test]
    fn daylight_survives_the_palette_spec_round_trip() {
        let spec = vt::palette_to_spec(&daylight_palette());
        assert_eq!(vt::palette_from_spec(&spec), Some(daylight_palette()));
    }

    // Every Daylight value pinned against HALCYON-VISUAL section 1/2/6. A drift
    // here is a scripture divergence, not a taste change.
    #[test]
    fn daylight_matches_the_scripture() {
        let d = &DAYLIGHT;
        // ground
        assert_eq!(d.floor, 0xFF8A7660);
        assert_eq!(d.surface, 0xFFF2EBE0);
        assert_eq!(d.header, 0xFFCEC4B6);
        assert_eq!(d.raised, 0xFFBDB0A0);
        assert_eq!(d.border, 0xFFA89880);
        assert_eq!(hairline(d), 0xFFCEC4B6); // == header, section 2.4
                                             // ink
        assert_eq!(d.fg, 0xFF1A120A);
        assert_eq!(d.fg_dim, 0xFF3A2E22);
        assert_eq!(d.fg_muted, 0xFF6A5A48);
        assert_eq!(d.fg_subtle, 0xFF9A8878);
        // bevel (NNW, four distinct values -- section 2.1)
        assert_eq!(d.bevel_top, 0xFFF8F2E6);
        assert_eq!(d.bevel_left, 0xFFE2D6C0);
        assert_eq!(d.bevel_right, 0xFF362410);
        assert_eq!(d.bevel_bottom, 0xFF221405);
        assert!(
            d.bevel_top != d.bevel_left,
            "NNW gives four distinct edges, not two"
        );
        assert!(d.bevel_right != d.bevel_bottom);
        // accent
        assert_eq!(d.ember, 0xFFE07840);
        assert_eq!(d.ember_dim, 0xFFB85F2A);
        assert_eq!(d.ember_deep, 0xFFC86030);
        // live keys
        assert_eq!(d.sage.key, 0xFF1E5844);
        assert_eq!(d.sage.tint, 0xFFB8CCC4);
        assert_eq!(d.cinnabar.key, 0xFF982818);
        assert_eq!(d.cinnabar.tint, 0xFFDCB8B0);
        // syntax: slate is the object-reference colour (section 1.5)
        assert_eq!(d.syntax.slate, 0xFF3A4878);
        assert_eq!(d.syntax.fen, 0xFF1E5828);
        assert_eq!(d.syntax.cinnabar, 0xFF982818);
        // status bar (section 6)
        assert_eq!(d.status_bg, 0xFF1A120A);
        assert_eq!(d.status_fg, 0xFFF2EBE0);
        // type (HALCYON-TYPE 4.2, ratified 2026-09-08): 0.012 em on the
        // light ground
        assert_eq!(d.smooth_mem, 12);
    }

    #[test]
    fn metrics_match_the_scripture() {
        assert_eq!(METRICS.bevel, 2);
        assert_eq!(METRICS.gap, 2);
        assert_eq!(METRICS.hairline, 1);
        assert_eq!(METRICS.header_h, 20);
        assert_eq!(METRICS.status_h, 20);
        assert_eq!(METRICS.tab_strip_h, 5);
    }

    // HALCYON-SCALE 5: the scaled metrics -- the identity at 100 (nothing at
    // 1.0 moves), round half up above it, the hairline/bevel floors held
    // (COMPOSITION 1: `max(1, round(1 x s))`, `max(2, round(2 x s))`), and
    // the operator's worked table (COMPOSITION 6) at 150 and 200.
    #[test]
    fn scaled_metrics_are_the_identity_at_100_and_the_worked_table_above() {
        assert!(METRICS.at(100) == METRICS, "at(100) is the base exactly");
        let m125 = METRICS.at(125);
        assert_eq!((m125.header_h, m125.status_h), (25, 25));
        assert_eq!(m125.hairline, 1, "round(1.25) = 1, the floor holds");
        assert_eq!(m125.bevel, 3, "round(2.5) = 3 (half up)");
        assert_eq!(m125.gap, 3);
        assert_eq!(m125.tag_pad_x, 8, "7.5 up");
        assert_eq!(m125.tab_strip_h, 6, "6.25 down");
        let m150 = METRICS.at(150);
        assert_eq!(
            (
                m150.header_h,
                m150.status_h,
                m150.bevel,
                m150.gap,
                m150.hairline
            ),
            (30, 30, 3, 3, 2)
        );
        assert_eq!((m150.tag_pad_x, m150.tab_strip_h), (9, 8));
        let m175 = METRICS.at(175);
        assert_eq!(
            (m175.header_h, m175.bevel, m175.hairline, m175.gap),
            (35, 4, 2, 4)
        );
        let m200 = METRICS.at(200);
        assert_eq!(
            (
                m200.header_h,
                m200.status_h,
                m200.bevel,
                m200.gap,
                m200.hairline
            ),
            (40, 40, 4, 4, 2)
        );
        assert_eq!((m200.tag_pad_x, m200.tab_strip_h), (12, 10));
        // The floors bite below 100 too (not a v1 value, but the function is total).
        let m50 = METRICS.at(50);
        assert_eq!((m50.hairline, m50.bevel), (1, 2));
    }

    // The ember is shared VERBATIM with Bonfire (section 1.3) -- the link
    // between the two surfaces. Bonfire's ember is 0xFFE07840.
    #[test]
    fn ember_is_the_bonfire_ember() {
        assert_eq!(DAYLIGHT.ember, 0xFFE07840);
    }

    // The /env palette the session publishes resolves DAYLIGHT under the
    // program-agnostic role names a hosted program (nora) reads. A drift in a
    // role's SOURCE here silently re-themes every hosted program.
    #[test]
    fn daylight_env_palette_resolves_every_role() {
        let text = daylight_env_palette();
        assert_eq!(text.lines().count(), 11);
        assert!(text.contains("bg=f2ebe0\n")); // surface
        assert!(text.contains("fg=1a120a\n")); // fg
        assert!(text.contains("dim=6a5a48\n")); // fg_muted
        assert!(text.contains("accent=e07840\n")); // ember
        assert!(text.contains("border=a89880\n"));
        assert!(text.contains("moss=3a5818\n")); // syntax.moss
        assert!(text.contains("dusk=4a3868\n")); // syntax.dusk
        assert!(text.contains("sand=7a5020\n")); // syntax.sand
        assert!(text.contains("slate=3a4878\n")); // syntax.slate
        assert!(text.contains("cinnabar=982818\n")); // syntax.cinnabar
                                                     // The panel role is the LIGHT lift (header), NOT the dark status strip:
                                                     // a program paints its own dark ink on it (nora's status bar), so
                                                     // status_bg would be dark-on-dark. This pins the contrast fix.
        assert!(text.contains("surface=cec4b6\n")); // header
        assert!(!text.contains("surface=1a120a\n")); // NOT status_bg (the dark strip)
                                                     // No alpha leaked: every value is exactly six hex digits.
        for line in text.lines() {
            let hex = line.split('=').nth(1).unwrap();
            assert_eq!(hex.len(), 6, "{line} is not RRGGBB");
        }
    }
}
