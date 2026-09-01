// theme -- the Daylight visual scripture as code (docs/HALCYON-VISUAL.md).
//
// This is the SINGLE token source the ratified H-3 split names: halcyond's
// transcript Sheet + chrome surface AND tapestryd's pane bevel/hairline/
// cast-shadow constants both derive from here and nowhere else (the doc's
// "consumed by libhalcyon::theme; the tag-bar and pane compositor read their
// values from here and nowhere else").
//
// Colours are `Argb` = 0xAARRGGBB with the alpha byte 0xFF (opaque) -- the
// pixel format the cartoon executor writes and tapestryd's chrome painter
// fills (tapestryd's own BG_COLOR is 0xFF101014, same convention).
//
// The struct is theme-agnostic (HALCYON-VISUAL section 1.4/4/9: only the
// palette differs between themes). Frutiger Aero (deferred to a later chunk)
// is a second `Theme` const of this exact shape; nothing structural changes.

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
    pub slate: Argb, // keyword / info / OBJECT REFERENCE (the presentation colour)
    pub sage: Argb,  // type
    pub sand: Argb,  // member / warning
    pub moss: Argb,  // constant
    pub ash: Argb,   // function / identifier
    pub dusk: Argb,  // string
    pub smoke: Argb, // comment
    pub fen: Argb,   // success
    pub cinnabar: Argb, // error
}

/// The full theme. One instance per Halcyon theme; `DAYLIGHT` is the only one
/// at H-3a.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Theme {
    // Ground (section 1.1)
    pub floor: Argb,    // workspace floor; the bevel perceptual midpoint
    pub surface: Argb,  // pane surface (parchment)
    pub header: Argb,   // tag bar bg; ALSO the inner hairline (section 2.4)
    pub raised: Argb,   // pill bg
    pub border: Argb,   // explicit strokes, tag-bar separators, the cast shadow
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
}

/// Chrome metrics (HALCYON-VISUAL section 3.1 / 4.3). Pixels.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Metrics {
    pub bevel: i32,     // pane bevel width (2)
    pub gap: i32,       // inter-pane gap AND workspace padding (2)
    pub hairline: i32,  // structural hairline (1)
    pub header_h: i32,  // tag bar height (20)
    pub status_h: i32,  // status bar height (20)
    pub tag_pad_x: i32, // tag bar horizontal padding (6)
}

pub const METRICS: Metrics = Metrics {
    bevel: 2,
    gap: 2,
    hairline: 1,
    header_h: 20,
    status_h: 20,
    tag_pad_x: 6,
};

/// Daylight (HALCYON-VISUAL section 1). Values are the doc's #rrggbb widened to
/// opaque Argb; the test below pins every one against the scripture.
pub const DAYLIGHT: Theme = Theme {
    floor: 0xFF8A_7660,
    surface: 0xFFF2_EBE0,
    header: 0xFFCE_C4B6,
    raised: 0xFFBD_B0A0,
    border: 0xFFA8_9880,
    fg: 0xFF1A_120A,
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
    status_bg: 0xFF1A_120A,
    status_fg: 0xFFF2_EBE0,
    status_muted: 0xFFC8_B89A,
    status_idle: 0xFF3A_2E22,
};

/// The inner hairline (section 2.4) is `header` by construction -- it vanishes
/// alongside a tag bar and shows only against content. One name for the intent.
pub const fn hairline(t: &Theme) -> Argb {
    t.header
}

/// The transcript's vt palette, grounded in Daylight so it AGREES with the
/// `Sheet` built from `DAYLIGHT` (bg == surface, fg == fg). This agreement is
/// load-bearing: halcyond's "default ink" test (`st.fg == sheet.ink`, the
/// hook that applies the obj/dim semantic colours) only fires when the pen's
/// default fg -- which comes from THIS palette -- equals `sheet.ink`. The
/// ANSI-16 is the proven light set (vt's PARCHMENT, which Daylight formalizes),
/// with bright-white pinned to the default fg for coherence. Foreign-program
/// SGR renders through this; halcyon's own output renders through the Sheet.
pub fn daylight_palette() -> vt::Palette {
    let mut ansi = vt::THEMES[1].1.ansi;
    ansi[15] = DAYLIGHT.fg; // bright white == default fg
    vt::Palette {
        bg: DAYLIGHT.surface,
        fg: DAYLIGHT.fg,
        ansi,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

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
        assert!(d.bevel_top != d.bevel_left, "NNW gives four distinct edges, not two");
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
    }

    #[test]
    fn metrics_match_the_scripture() {
        assert_eq!(METRICS.bevel, 2);
        assert_eq!(METRICS.gap, 2);
        assert_eq!(METRICS.hairline, 1);
        assert_eq!(METRICS.header_h, 20);
        assert_eq!(METRICS.status_h, 20);
    }

    // The ember is shared VERBATIM with Bonfire (section 1.3) -- the link
    // between the two surfaces. Bonfire's ember is 0xFFE07840.
    #[test]
    fn ember_is_the_bonfire_ember() {
        assert_eq!(DAYLIGHT.ember, 0xFFE07840);
    }
}
