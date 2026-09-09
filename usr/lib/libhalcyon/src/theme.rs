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
#[derive(Clone, Copy, PartialEq, Eq)]
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
#[derive(Clone, Copy, PartialEq, Eq)]
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
#[derive(Clone, Copy, PartialEq, Eq)]
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
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Metrics {
    pub bevel: i32,       // pane bevel width (2)
    pub gap: i32,         // inter-pane gap AND workspace padding (2)
    pub hairline: i32,    // structural hairline (1)
    pub header_h: i32,    // tag bar height (20)
    pub status_h: i32,    // status bar height (20)
    pub tag_pad_x: i32,   // tag bar horizontal padding (6)
    pub tab_strip_h: i32, // tab/stack indicator strip (5); glyph-free, G-6c/D7
}

pub const METRICS: Metrics = Metrics {
    bevel: 2,
    gap: 2,
    hairline: 1,
    header_h: 20,
    status_h: 20,
    tag_pad_x: 6,
    tab_strip_h: 5,
};

impl Metrics {
    /// The chrome metrics at a display scale (HALCYON-SCALE 5; the percent
    /// of `scale::scale_pct`): every size round-half-up scaled, the
    /// hairline never below 1 and the bevel never below 2 (COMPOSITION
    /// 1's structural-mark floors). The ONE function both the compositor's
    /// carve and halcyond's paint use, so the two cannot drift; `at(100)`
    /// is `METRICS` exactly (pinned by test).
    pub const fn at(pct: u16) -> Metrics {
        let hair = crate::scale::ipx(METRICS.hairline, pct);
        let bevel = crate::scale::ipx(METRICS.bevel, pct);
        Metrics {
            bevel: if bevel < 2 { 2 } else { bevel },
            gap: crate::scale::ipx(METRICS.gap, pct),
            hairline: if hair < 1 { 1 } else { hair },
            header_h: crate::scale::ipx(METRICS.header_h, pct),
            status_h: crate::scale::ipx(METRICS.status_h, pct),
            tag_pad_x: crate::scale::ipx(METRICS.tag_pad_x, pct),
            tab_strip_h: crate::scale::ipx(METRICS.tab_strip_h, pct),
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
        assert!(Metrics::at(100) == METRICS, "at(100) is METRICS exactly");
        let m125 = Metrics::at(125);
        assert_eq!((m125.header_h, m125.status_h), (25, 25));
        assert_eq!(m125.hairline, 1, "round(1.25) = 1, the floor holds");
        assert_eq!(m125.bevel, 3, "round(2.5) = 3 (half up)");
        assert_eq!(m125.gap, 3);
        assert_eq!(m125.tag_pad_x, 8, "7.5 up");
        assert_eq!(m125.tab_strip_h, 6, "6.25 down");
        let m150 = Metrics::at(150);
        assert_eq!((m150.header_h, m150.status_h, m150.bevel, m150.gap, m150.hairline), (30, 30, 3, 3, 2));
        assert_eq!((m150.tag_pad_x, m150.tab_strip_h), (9, 8));
        let m175 = Metrics::at(175);
        assert_eq!((m175.header_h, m175.bevel, m175.hairline, m175.gap), (35, 4, 2, 4));
        let m200 = Metrics::at(200);
        assert_eq!((m200.header_h, m200.status_h, m200.bevel, m200.gap, m200.hairline), (40, 40, 4, 4, 2));
        assert_eq!((m200.tag_pad_x, m200.tab_strip_h), (12, 10));
        // The floors bite below 100 too (not a v1 value, but the function is total).
        let m50 = Metrics::at(50);
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
