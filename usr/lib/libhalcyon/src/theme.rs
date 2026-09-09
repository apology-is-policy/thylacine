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
// between themes), so Nightjar and Frutiger Aero are TOML, parsed into this
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
        // A file this big is refused BEFORE parsing, and the bound is not
        // about memory -- the parser's own caps handle that. It is about
        // TRUNCATION: a caller's slurp stops at its own limit and returns
        // what it got, and a truncated theme can be perfectly valid TOML,
        // which is worse than malformed because 4.2 never fires. Refusing
        // anything that could have been cut is the only way to tell.
        if src.len() > THEME_MAX {
            return Err(LoadError::Syntax(crate::toml::Error {
                line: 1,
                kind: crate::toml::Kind::TooLarge,
            }));
        }
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

/// `env_palette(&DAYLIGHT)` -- the built-in's roles as /env text.
///
/// TEST-ONLY since TH-4a: the session publishes `env_palette(&resolved)`, so
/// the export is a RENDERING of the theme in force (3.5) rather than a second
/// hand-kept list. A production caller here would publish Daylight's roles to
/// a hosted program while the session itself painted something else.
#[cfg(test)]
pub fn daylight_env_palette() -> String {
    env_palette(&DAYLIGHT)
}

/// The largest theme file that will be read. Comfortably above any real one
/// (the full 57-key Nightjar is a few KiB) and comfortably BELOW any reader's
/// truncation point, so a file that was cut short is refused rather than
/// parsed as a valid prefix.
pub const THEME_MAX: usize = 64 * 1024;

/// The system theme file (HALCYON-THEME 3.4). `/lib/halcyon/` is already the
/// established home (`/lib/halcyon/renderer`, `/lib/halcyon/layouts`), so this
/// adds a file rather than a convention.
pub const SYSTEM_THEME_PATH: &str = "/lib/halcyon/theme.toml";
/// The user's, relative to `$HOME`. Read by the user's SESSION only: the
/// console renderer is not anyone's session and takes the system file.
pub const USER_THEME_REL: &str = "/lib/halcyon/theme.toml";

/// The RESOLVED theme as one line, for the seam that pushes it to another
/// process (HALCYON-THEME 3.4's display coherence).
///
/// 72 comma-separated fields in a fixed order: 64 colours as `RRGGBB` (the
/// opaque alpha is not on the wire), then `smooth`, then the 7 geometry
/// integers. Both ends call THIS pair, so the format cannot drift between
/// them, and `a_distinct_theme_survives_the_wire` round-trips a theme whose
/// every field differs -- so a field left out of `to_wire` comes back as the
/// built-in's and fails, which is the only way to catch an omission here.
///
/// Why not push the TOML text instead: a ctl verb is one LINE, TOML is not,
/// and the user's file may be up to `THEME_MAX`. This is bounded at ~500
/// bytes and needs no parser on the far side.
pub fn to_wire(t: &Theme) -> String {
    let mut s = String::new();
    let mut c = |v: Argb| {
        if !s.is_empty() {
            s.push(',');
        }
        let _ = write!(s, "{:06x}", v & 0x00FF_FFFF);
    };
    for v in [
        t.floor,
        t.surface,
        t.header,
        t.raised,
        t.border,
        t.blank,
        t.selection,
        t.island_rule,
        t.fg,
        t.fg_dim,
        t.fg_muted,
        t.fg_subtle,
        t.bevel_top,
        t.bevel_left,
        t.bevel_right,
        t.bevel_bottom,
        t.ember,
        t.ember_dim,
        t.ember_deep,
        t.status_bg,
        t.status_fg,
        t.status_muted,
        t.status_idle,
        t.sage.key,
        t.sage.tint,
        t.sage.raised,
        t.sage.border,
        t.sage.fg,
        t.sage.fg_dim,
        t.sage.fg_muted,
        t.cinnabar.key,
        t.cinnabar.tint,
        t.cinnabar.raised,
        t.cinnabar.border,
        t.cinnabar.fg,
        t.cinnabar.fg_dim,
        t.cinnabar.fg_muted,
        t.syntax.slate,
        t.syntax.sage,
        t.syntax.sand,
        t.syntax.moss,
        t.syntax.ash,
        t.syntax.dusk,
        t.syntax.smoke,
        t.syntax.fen,
        t.syntax.cinnabar,
        t.terminal.bg,
        t.terminal.fg,
    ] {
        c(v);
    }
    for v in t.terminal.ansi {
        c(v);
    }
    for n in [
        t.smooth_mem as i32,
        t.metrics.bevel,
        t.metrics.gap,
        t.metrics.hairline,
        t.metrics.header_h,
        t.metrics.status_h,
        t.metrics.tag_pad_x,
        t.metrics.tab_strip_h,
    ] {
        let _ = write!(s, ",{n}");
    }
    s
}

/// The number of fields `to_wire` emits. A short line is refused rather than
/// applied to whatever it reached, so a truncated push cannot half-theme a
/// display.
pub const WIRE_FIELDS: usize = 72;

/// The inverse of `to_wire`. `None` on any deviation -- a wrong count, a bad
/// colour, a geometry value outside the same bounds the FILE must satisfy.
///
/// The bounds are re-checked here on purpose: this arrives from another
/// process, so it is untrusted input in its own right, and a display whose
/// hairline came over a wire it did not validate is a `scale`-class hazard
/// wearing a theme's clothes.
pub fn from_wire(line: &str) -> Option<Theme> {
    let f: alloc::vec::Vec<&str> = line.trim().split(',').collect();
    if f.len() != WIRE_FIELDS {
        return None;
    }
    let col = |i: usize| -> Option<Argb> {
        let h = f[i];
        if h.len() != 6 || !h.bytes().all(|b| b.is_ascii_hexdigit()) {
            return None;
        }
        let mut v = 0u32;
        for ch in h.chars() {
            v = (v << 4) | ch.to_digit(16)?;
        }
        Some(0xFF00_0000 | v)
    };
    let num = |i: usize, key: &str| -> Option<i32> {
        let n: i64 = f[i].parse().ok()?;
        let (lo, hi) = geometry_bounds(key);
        if lo == 0 && hi == 0 {
            return None;
        }
        if n < lo || n > hi {
            return None;
        }
        Some(n as i32)
    };
    let mut ansi = [0u32; 16];
    for (j, slot) in ansi.iter_mut().enumerate() {
        *slot = col(48 + j)?;
    }
    let smooth: i64 = f[64].parse().ok()?;
    if !(0..=200).contains(&smooth) {
        return None;
    }
    Some(Theme {
        floor: col(0)?,
        surface: col(1)?,
        header: col(2)?,
        raised: col(3)?,
        border: col(4)?,
        blank: col(5)?,
        selection: col(6)?,
        island_rule: col(7)?,
        fg: col(8)?,
        fg_dim: col(9)?,
        fg_muted: col(10)?,
        fg_subtle: col(11)?,
        bevel_top: col(12)?,
        bevel_left: col(13)?,
        bevel_right: col(14)?,
        bevel_bottom: col(15)?,
        ember: col(16)?,
        ember_dim: col(17)?,
        ember_deep: col(18)?,
        status_bg: col(19)?,
        status_fg: col(20)?,
        status_muted: col(21)?,
        status_idle: col(22)?,
        sage: LiveKey {
            key: col(23)?,
            tint: col(24)?,
            raised: col(25)?,
            border: col(26)?,
            fg: col(27)?,
            fg_dim: col(28)?,
            fg_muted: col(29)?,
        },
        cinnabar: LiveKey {
            key: col(30)?,
            tint: col(31)?,
            raised: col(32)?,
            border: col(33)?,
            fg: col(34)?,
            fg_dim: col(35)?,
            fg_muted: col(36)?,
        },
        syntax: Syntax {
            slate: col(37)?,
            sage: col(38)?,
            sand: col(39)?,
            moss: col(40)?,
            ash: col(41)?,
            dusk: col(42)?,
            smoke: col(43)?,
            fen: col(44)?,
            cinnabar: col(45)?,
        },
        terminal: vt::Palette {
            bg: col(46)?,
            fg: col(47)?,
            ansi,
        },
        smooth_mem: smooth as u16,
        metrics: Metrics {
            bevel: num(65, "bevel")?,
            gap: num(66, "gap")?,
            hairline: num(67, "hairline")?,
            header_h: num(68, "header_h")?,
            status_h: num(69, "status_h")?,
            tag_pad_x: num(70, "tag_pad_x")?,
            tab_strip_h: num(71, "tab_strip_h")?,
        },
    })
}

/// Where a resolved theme came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Source {
    /// No theme file, or none that loaded. The default installation.
    BuiltIn,
    /// `/lib/halcyon/theme.toml`.
    System,
    /// `$HOME/lib/halcyon/theme.toml`.
    User,
}

/// A resolved theme plus what the resolution is owed to say.
#[derive(Clone, Debug)]
pub struct Resolved {
    pub theme: Theme,
    pub source: Source,
    /// `[meta] name`, or empty.
    pub name: String,
    /// What to SAY. A missing file is silent (4.1); a malformed one is LOUD
    /// and names its line and its tier, because a theme that quietly did not
    /// apply is indistinguishable from one that did nothing.
    pub notes: alloc::vec::Vec<String>,
    /// Keys the winning file inherited from its base -- what
    /// `halcyon theme lint` reports (4.3).
    pub inherited: alloc::vec::Vec<String>,
}

/// One line of English for a refusal. The author reads this and knows where
/// to look, which is the entire point of refusing whole rather than partly.
pub fn describe(e: &LoadError) -> String {
    let mut s = String::new();
    let _ = match e {
        LoadError::Syntax(p) => {
            let what = match p.kind {
                crate::toml::Kind::BadTable => "malformed [table] header",
                crate::toml::Kind::BadKey => "not a key = value line",
                crate::toml::Kind::BadValue => {
                    "unsupported value (no floats, bools, dates or inline tables)"
                }
                crate::toml::Kind::BadString => "malformed string (no escapes in this subset)",
                crate::toml::Kind::BadInt => "malformed integer",
                crate::toml::Kind::BadArray => "malformed array",
                crate::toml::Kind::Duplicate => "the same key twice in one table",
                crate::toml::Kind::TooLarge => "too large",
            };
            write!(s, "line {}: {}", p.line, what)
        }
        LoadError::UnknownTable { line } => write!(s, "line {line}: unknown [table]"),
        LoadError::UnknownKey { line } => write!(s, "line {line}: unknown key"),
        LoadError::BadColour { line } => write!(s, "line {line}: not a \"#RRGGBB\" colour"),
        LoadError::BadShape { line } => write!(s, "line {line}: wrong kind of value for this key"),
        LoadError::OutOfRange { line } => write!(s, "line {line}: value out of range"),
        LoadError::UnknownBase { line } => write!(s, "line {line}: unknown base theme"),
        LoadError::Incomplete { missing } => {
            let _ = write!(
                s,
                "no [meta] base, so every key is required; {} missing:",
                missing.len()
            );
            // Name them -- capped, because a file that set nothing would
            // otherwise print the whole schema at a console.
            for k in missing.iter().take(8) {
                let _ = write!(s, " {k}");
            }
            if missing.len() > 8 {
                let _ = write!(s, " ... and {} more", missing.len() - 8);
            }
            Ok(())
        }
    };
    s
}

/// Resolve the session's theme from the two file tiers (HALCYON-THEME 3.4).
///
/// Pure: the CONTENTS are injected, so the policy is host-tested and the I/O
/// stays at the caller. `None` means the file is absent, which is not an
/// error (4.1) -- the default installation has neither.
///
/// The user's file wins over the system's. A file that fails to load falls
/// through to the NEXT TIER DOWN rather than to the built-in directly: a user
/// whose own file has a typo still gets the system theme, which is what they
/// were seeing before they wrote it.
pub fn resolve(system: Option<&str>, user: Option<&str>) -> Resolved {
    let mut notes: alloc::vec::Vec<String> = alloc::vec::Vec::new();
    for (src, source, tier) in [
        (user, Source::User, "user"),
        (system, Source::System, "system"),
    ] {
        let Some(text) = src else { continue };
        match Theme::from_toml(text) {
            Ok(l) => {
                return Resolved {
                    theme: l.theme,
                    source,
                    name: l.name,
                    notes,
                    inherited: l.inherited,
                }
            }
            Err(e) => {
                let mut n = String::new();
                let _ = write!(n, "theme: {tier} theme.toml REFUSED -- {}", describe(&e));
                notes.push(n);
            }
        }
    }
    Resolved {
        theme: builtin(),
        source: Source::BuiltIn,
        name: String::new(),
        notes,
        inherited: alloc::vec::Vec::new(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // ---- the theme file (HALCYON-THEME 3.3 / 4) ----

    /// The shipped dark theme, compiled in for the test only -- the guest
    /// reads it off the filesystem.
    const NIGHTJAR: &str = include_str!("../../halcyon/themes/nightjar.toml");

    // TH-5: THE ARC'S PROOF. An arc that ships only the theme it started with
    // has proved nothing -- every mechanism could be subtly Daylight-shaped
    // and nobody would know. Nightjar is written with NO `base`, so the
    // loader requires all 57 keys and this test fails, naming them, the day
    // one is forgotten.
    #[test]
    fn nightjar_is_complete_coherent_and_nothing_like_daylight() {
        let l = Theme::from_toml(NIGHTJAR).unwrap_or_else(|e| {
            panic!("the shipped Nightjar must load: {}", describe(&e));
        });
        assert_eq!(l.name, "Nightjar");
        assert!(
            l.inherited.is_empty(),
            "no base means nothing may be inherited, but {:?} were",
            l.inherited
        );
        let n = l.theme;
        let d = builtin();

        // It is a DIFFERENT theme, not a retint of two roles: every ground
        // and every ink differs from Daylight's.
        for (name, a, b) in [
            ("floor", n.floor, d.floor),
            ("surface", n.surface, d.surface),
            ("header", n.header, d.header),
            ("raised", n.raised, d.raised),
            ("border", n.border, d.border),
            ("blank", n.blank, d.blank),
            ("selection", n.selection, d.selection),
            ("island_rule", n.island_rule, d.island_rule),
            ("fg", n.fg, d.fg),
            ("fg_dim", n.fg_dim, d.fg_dim),
            ("fg_muted", n.fg_muted, d.fg_muted),
            ("fg_subtle", n.fg_subtle, d.fg_subtle),
            ("status_bg", n.status_bg, d.status_bg),
            ("terminal.bg", n.terminal.bg, d.terminal.bg),
        ] {
            assert_ne!(a, b, "{name} is still Daylight's");
        }

        // It is DARK: every ground is darker than every ink. This is the
        // property a "dark theme" actually names, and it catches a paste
        // error no colour-by-colour comparison would.
        let lum = |c: Argb| {
            let (r, g, b) = ((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
            (2 * r + 5 * g + b) / 8
        };
        for (gn, gc) in [
            ("floor", n.floor),
            ("surface", n.surface),
            ("header", n.header),
            ("blank", n.blank),
            ("status_bg", n.status_bg),
        ] {
            for (inn, ic) in [("fg", n.fg), ("fg_dim", n.fg_dim), ("status_fg", n.status_fg)] {
                assert!(
                    lum(gc) < lum(ic),
                    "ground {gn} ({:#08x}) is not darker than ink {inn} ({:#08x})",
                    gc,
                    ic
                );
            }
        }
        assert!(lum(n.blank) <= lum(n.floor), "an empty pane is a hole");
        assert!(lum(n.floor) < lum(n.surface), "a pane is lifted off the floor");

        // HALCYON-VISUAL 1.3: the ember is shared VERBATIM. It is how the
        // surfaces read as one system, so it is the one colour a theme may
        // not move.
        assert_eq!(n.ember, d.ember, "the ember is shared verbatim");

        // 2.1: four distinct bevel faces from ONE light direction, and the
        // lit pair really is lighter than the shadowed pair.
        assert!(n.bevel_top != n.bevel_left && n.bevel_right != n.bevel_bottom);
        assert!(lum(n.bevel_top) > lum(n.bevel_left));
        assert!(lum(n.bevel_left) > lum(n.bevel_right));
        assert!(lum(n.bevel_right) > lum(n.bevel_bottom));

        // HALCYON-TYPE 4.2's per-theme rule: light ink on dark already reads
        // heavy, so a dark theme takes no smoothing stroke.
        assert_eq!(n.smooth_mem, 0, "a dark ground takes 0");

        // The terminal agreements TH-1 makes true by construction for the
        // built-in must be true by AUTHORSHIP here -- a file can set them
        // apart, and halcyond's default-ink hook only fires when they match.
        assert_eq!(n.terminal.bg, n.surface, "bg is the pane surface");
        assert_eq!(n.terminal.fg, n.fg, "fg is the ink");
        // vt's slot-uniqueness rule, checked for the AUTHORED palette: no two
        // slots share a value except ansi[15] == fg. A violation mis-slots
        // cells across a `set_theme` remap.
        for (i, c) in n.terminal.ansi.iter().enumerate() {
            for (j, e) in n.terminal.ansi.iter().enumerate() {
                assert!(i == j || c != e, "nightjar ansi[{i}] and ansi[{j}] collide");
            }
            assert!(i == 15 || *c != n.fg, "ansi[{i}] aliases fg but is not 15");
        }
        assert_eq!(n.terminal.ansi[15], n.fg);

        // And it survives the push seam, so a Nightjar session can actually
        // hand its theme to the compositor.
        assert_eq!(from_wire(&to_wire(&n)), Some(n));
    }

    // The sheet built from Nightjar carries no Daylight colour -- the TH-2
    // retint test's claim, made against a REAL second theme rather than a
    // synthetic inversion.
    #[test]
    fn a_nightjar_sheet_carries_nothing_of_daylight() {
        let n = Theme::from_toml(NIGHTJAR).unwrap().theme;
        let d = builtin();
        let daylight: &[Argb] = &[
            d.surface, d.header, d.fg, d.fg_dim, d.ember, d.border, d.selection,
            d.island_rule, d.syntax.slate, d.syntax.fen, d.cinnabar.key,
        ];
        for (name, c) in [
            ("surface", n.surface),
            ("header", n.header),
            ("fg", n.fg),
            ("fg_dim", n.fg_dim),
            ("border", n.border),
            ("selection", n.selection),
            ("island_rule", n.island_rule),
            ("syntax.slate", n.syntax.slate),
            ("syntax.fen", n.syntax.fen),
            ("cinnabar.key", n.cinnabar.key),
        ] {
            assert!(
                !daylight.contains(&c),
                "nightjar {name} ({c:#08x}) is a Daylight colour"
            );
        }
        // The ember is the deliberate exception, and it must still be there.
        assert!(daylight.contains(&n.ember));
    }

    // A theme file big enough to have been TRUNCATED by its reader is refused
    // whole. A cut file can be valid TOML -- so 4.2 would never fire, and the
    // author would get a silently half-applied visual, which is the exact
    // outcome the whole refusal policy exists to prevent.
    #[test]
    fn an_oversized_file_is_refused_before_it_can_be_a_valid_prefix() {
        let mut big = String::from("[meta]\nbase = \"daylight\"\n[palette]\n");
        // Legal, parseable content -- the point is that SIZE alone refuses it.
        while big.len() <= THEME_MAX {
            big.push_str("# a comment line that is entirely valid\n");
        }
        assert!(matches!(
            Theme::from_toml(&big),
            Err(LoadError::Syntax(crate::toml::Error {
                kind: crate::toml::Kind::TooLarge,
                ..
            }))
        ));
        // The control: the same content just under the cap loads fine, so the
        // refusal is the SIZE's and not the content's.
        let ok = &big[..THEME_MAX];
        assert!(Theme::from_toml(ok).is_ok(), "just under the cap must load");
    }

    // THE PUSH SEAM (3.4's display coherence). A theme whose EVERY field
    // differs from the built-in must survive the round trip -- so a field
    // left out of `to_wire` comes back as the built-in's and this fails,
    // which is the only way to catch an omission in a hand-written codec.
    #[test]
    fn a_distinct_theme_survives_the_wire() {
        // Build it from a file setting all 57 keys to distinct values: the
        // same machinery the registry sweep uses, so the two cannot disagree
        // about what "every field" means.
        let mut full = String::new();
        let mut last = "";
        let mut n = 0u32;
        for (table, key) in KEYS {
            if *table != last {
                full.push('[');
                full.push_str(table);
                full.push_str("]\n");
                last = table;
            }
            full.push_str(key);
            full.push_str(" = ");
            // A distinct value per key, so no two fields can be confused.
            if *key == "ansi" {
                full.push('[');
                for i in 0..16u32 {
                    let _ = core::fmt::write(
                        &mut full,
                        format_args!("\"#{:02x}{:02x}{:02x}\",", 0x40 + i, i, 0x80 + i),
                    );
                }
                full.push(']');
            } else if *table == "geometry" || *table == "type" {
                // Inside each token's own bounds, and distinct where it can be.
                let v = match *key {
                    "bevel" => 5,
                    "gap" => 6,
                    "hairline" => 7,
                    "header_h" => 31,
                    "status_h" => 33,
                    "tag_pad_x" => 11,
                    "tab_strip_h" => 13,
                    _ => 9, // type.smooth
                };
                let _ = core::fmt::write(&mut full, format_args!("{v}"));
            } else {
                n += 1;
                let _ = core::fmt::write(&mut full, format_args!("\"#{:06x}\"", 0x112200 + n));
            }
            full.push('\n');
        }
        let t = Theme::from_toml(&full)
            .expect("the all-keys file must load")
            .theme;
        assert!(t != builtin(), "the fixture must differ from the built-in");

        let wire = to_wire(&t);
        assert_eq!(wire.split(',').count(), WIRE_FIELDS);
        assert_eq!(from_wire(&wire), Some(t), "a field did not survive to_wire");
        assert!(wire.len() < 700, "the line is bounded: {}", wire.len());
    }

    // Untrusted in its own right: this arrives from ANOTHER PROCESS, so a
    // display whose hairline came over an unvalidated wire is a scale-class
    // hazard in a theme's clothes.
    #[test]
    fn a_malformed_wire_is_refused_with_its_bounds_rechecked() {
        let good = to_wire(&builtin());
        assert!(from_wire(&good).is_some(), "the control must pass");
        assert_eq!(from_wire(""), None);
        assert_eq!(from_wire(&good[..good.len() - 1]), None, "a truncated push");
        assert_eq!(
            from_wire(&alloc::format!("{good},0")),
            None,
            "one field too many"
        );
        // Mutate a NAMED FIELD, never a substring: `replacen("ff", ..)` on
        // this line finds nothing (the alpha byte is not on the wire), so it
        // would have re-tested the unmodified control and passed.
        let mut f: alloc::vec::Vec<&str> = good.split(',').collect();
        let was = f[0];
        f[0] = "zz1122";
        assert_ne!(f[0], was, "the mutation must actually mutate");
        assert_eq!(from_wire(&f.join(",")), None, "non-hex");
        f[0] = was;
        assert!(
            from_wire(&f.join(",")).is_some(),
            "the control, one field back"
        );
        // The geometry bounds are re-checked HERE, not trusted from the far
        // side: a hairline of 0 is refused on the wire exactly as in a file.
        f[67] = "0";
        assert_eq!(from_wire(&f.join(",")), None, "hairline 0 on the wire");
        f[67] = "1";
        assert!(
            from_wire(&f.join(",")).is_some(),
            "the control, one field back"
        );
        f[64] = "999";
        assert_eq!(from_wire(&f.join(",")), None, "smooth out of range");
    }

    // 3.4: the user's file wins, and a REFUSED file falls through to the next
    // tier DOWN -- not straight to the built-in. A user whose own file has a
    // typo keeps the system theme, which is what they were seeing before they
    // wrote it.
    #[test]
    fn the_tiers_resolve_in_order_and_a_refusal_falls_one_step() {
        let sys = "[meta]\nbase = \"daylight\"\nname = \"Sys\"\n[palette]\nsurface = \"#111111\"\n";
        let usr = "[meta]\nbase = \"daylight\"\nname = \"Usr\"\n[palette]\nsurface = \"#222222\"\n";
        let bad = "[meta]\nbase = \"daylight\"\n[palette]\nsurface = \"nope\"\n";

        let r = resolve(None, None);
        assert_eq!(r.source, Source::BuiltIn);
        assert!(r.notes.is_empty(), "a MISSING file is silent (4.1)");

        let r = resolve(Some(sys), None);
        assert_eq!((r.source, r.theme.surface), (Source::System, 0xFF111111));

        let r = resolve(Some(sys), Some(usr));
        assert_eq!((r.source, r.theme.surface), (Source::User, 0xFF222222));
        assert_eq!(r.name, "Usr");

        // The user's is refused: the SYSTEM one wins, and the refusal is LOUD.
        let r = resolve(Some(sys), Some(bad));
        assert_eq!((r.source, r.theme.surface), (Source::System, 0xFF111111));
        assert_eq!(r.notes.len(), 1);
        assert!(r.notes[0].contains("user"), "{}", r.notes[0]);
        assert!(r.notes[0].contains("line 4"), "{}", r.notes[0]);

        // Both refused: the built-in, and BOTH said -- a silent fallback here
        // is indistinguishable from a theme that applied and looked the same.
        let r = resolve(Some(bad), Some(bad));
        assert_eq!(r.source, Source::BuiltIn);
        assert_eq!(r.notes.len(), 2);
        assert!(r.notes[0].contains("user") && r.notes[1].contains("system"));
    }

    // Every refusal produces a line a person can act on: a tier, a line
    // number, and what was wrong. A note that just said "theme failed" would
    // satisfy the LOUD requirement while helping nobody.
    #[test]
    fn every_refusal_describes_itself_usefully() {
        let cases = [
            "[palette\n",
            "[meta]\nbase = \"daylight\"\n[palette]\nsurface = \"nope\"\n",
            "[meta]\nbase = \"daylight\"\n[palette]\nnope = \"#111111\"\n",
            "[meta]\nbase = \"twilight\"\n",
            "[meta]\nbase = \"daylight\"\n[geometry]\nhairline = 0\n",
            "[palette]\nsurface = \"#111111\"\n",
        ];
        for src in cases {
            let e = Theme::from_toml(src).unwrap_err();
            let d = describe(&e);
            assert!(!d.is_empty(), "for {src:?}");
            assert!(
                d.contains("line") || d.contains("missing"),
                "{d:?} names neither a line nor the missing keys"
            );
        }
        // The incomplete case names actual keys, capped so a file that set
        // nothing cannot print the whole schema at a console.
        let d = describe(&Theme::from_toml("[palette]\nsurface = \"#111111\"\n").unwrap_err());
        assert!(d.contains("palette.floor"), "{d}");
        assert!(d.contains("more"), "the list is capped: {d}");
    }

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
