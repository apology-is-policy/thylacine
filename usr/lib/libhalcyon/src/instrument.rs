// instrument -- the Instrument profile's theme schema, the two projections,
// the profile word and the resolved bundle (docs/HALCYON-INSTRUMENT.md 4).
//
// A theme file now answers to ONE of two schemas, decided by `[meta] profile`
// (`theme::load` dispatches): absent is the 57-key legacy schema `theme.rs`
// owns; `"instrument-v1"` is this one -- the 35 colour roles of the Astra
// kit's `resolved-tokens.json`, exactly, plus the authored ANSI-16 and the
// smoothing stroke. No geometry: geometry belongs to the PROFILE, a compiled
// table, never to a file a theme could point at (HALCYON-THEME 2's identity
// rule, extended by one axis).
//
// Whichever schema a file is in, a session resolves a BUNDLE: the profile in
// force, the legacy `Theme` and the `InstrumentTheme`, one of them native and
// the other PROJECTED (4.3) so any theme renders under either profile. The
// projections are approximations by construction and say so; exactness holds
// only in a theme's native profile.

use alloc::string::String;
use alloc::vec::Vec;
use core::fmt::Write as _;

use crate::theme::{self, Argb, LiveKey, LoadError, Metrics, Syntax, Theme};
use crate::toml::{Entry, Value};

/// The `[meta] profile` value this schema answers to.
pub const PROFILE_WORD: &str = "instrument-v1";

/// The largest Instrument theme file that will load (4.2). A complete file is
/// under 2 KiB; the bound is about truncation, as `theme::THEME_MAX` is.
pub const INSTRUMENT_MAX: usize = 16 * 1024;

/// The longest gallery id. `[a-z][a-z0-9_-]{0,31}`: it is a FILENAME under
/// `/lib/halcyon/themes/` and the picker's key, so it is validated as an id
/// before it is ever a path component (`is_gallery_id`).
pub const ID_MAX: usize = 32;

/// The system gallery directory (HALCYON-INSTRUMENT 4.1).
pub const GALLERY_DIR: &str = "/lib/halcyon/themes";
/// The system profile word, and the user's relative to `$HOME`.
pub const SYSTEM_PROFILE_PATH: &str = "/lib/halcyon/profile";
pub const USER_PROFILE_REL: &str = "/lib/halcyon/profile";
/// The user's gallery pick (one id), written by the picker (9.4), relative
/// to `$HOME`.
pub const USER_PICK_REL: &str = "/lib/halcyon/theme";

/// The Instrument theme: the 35 roles in `resolved-tokens.json`'s order, the
/// ANSI-16, the stroke, the polarity. `Copy` + `Eq` like `Theme`, and pinned
/// by `the_registry_covers_every_field` the same way.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct InstrumentTheme {
    // [color] -- grounds
    pub desktop: Argb,
    pub pane: Argb,
    pub open: Argb,
    pub header: Argb,
    pub hover: Argb,
    // inks
    pub text: Argb,
    pub secondary: Argb,
    pub dim: Argb,
    // structure
    pub structure: Argb,
    pub separator: Argb,
    // signals
    pub amber: Argb,
    pub amber_muted: Argb,
    pub error: Argb,
    pub success: Argb,
    pub terminal_path: Argb,
    // chrome
    pub rail: Argb,
    pub pane_border: Argb,
    pub focus_neutral: Argb,
    // document
    pub body_text: Argb,
    pub code_text: Argb,
    pub code_bg: Argb,
    pub code_body: Argb,
    pub terminal_bg: Argb,
    pub terminal_text: Argb,
    pub dialog_bg: Argb,
    pub kbd_bg: Argb,
    // the nine syntax roles
    pub syntax_keyword: Argb,
    pub syntax_type: Argb,
    pub syntax_function: Argb,
    pub syntax_string: Argb,
    pub syntax_number: Argb,
    pub syntax_attribute: Argb,
    pub syntax_lifetime: Argb,
    pub syntax_comment: Argb,
    pub syntax_punctuation: Argb,
    // [terminal] ansi -- the authored sixteen (Appendix A); the terminal's
    // default pair is `terminal_bg` / `terminal_text` above.
    pub ansi: [Argb; 16],
    // [type] smooth, thousandths of an em (HALCYON-TYPE 4.2).
    pub smooth_mem: u16,
    // [meta] color_scheme: `light` selects the smoothing default and the ANSI
    // polarity the lint checks.
    pub light: bool,
}

/// Every settable non-meta key, as `(table, key)`; `set_key` must agree with
/// it in both directions (`the_registry_covers_every_field`).
pub const KEYS: &[(&str, &str)] = &[
    ("color", "desktop"),
    ("color", "pane"),
    ("color", "open"),
    ("color", "header"),
    ("color", "hover"),
    ("color", "text"),
    ("color", "secondary"),
    ("color", "dim"),
    ("color", "structure"),
    ("color", "separator"),
    ("color", "amber"),
    ("color", "amber_muted"),
    ("color", "error"),
    ("color", "success"),
    ("color", "terminal_path"),
    ("color", "rail"),
    ("color", "pane_border"),
    ("color", "focus_neutral"),
    ("color", "body_text"),
    ("color", "code_text"),
    ("color", "code_bg"),
    ("color", "code_body"),
    ("color", "terminal_bg"),
    ("color", "terminal_text"),
    ("color", "dialog_bg"),
    ("color", "kbd_bg"),
    ("color", "syntax_keyword"),
    ("color", "syntax_type"),
    ("color", "syntax_function"),
    ("color", "syntax_string"),
    ("color", "syntax_number"),
    ("color", "syntax_attribute"),
    ("color", "syntax_lifetime"),
    ("color", "syntax_comment"),
    ("color", "syntax_punctuation"),
    ("terminal", "ansi"),
    ("type", "smooth"),
];

/// The `[meta]` keys, every one required.
const META_KEYS: &[&str] = &["schema", "profile", "id", "name", "color_scheme"];

/// The OPTIONAL `[meta]` keys (HALCYON-INSTRUMENT 4.2, amended at I-7): the
/// picker's group, subtitle and order. Validated when present, refused
/// when malformed, defaulted when absent -- a gallery file may carry none.
const META_OPTIONAL: &[&str] = &["group", "tagline", "rank"];

/// The rank a file without one sorts at: after every ranked file, then by
/// id.
pub const RANK_UNRANKED: u8 = 255;

/// The picker's groups (9.4): the mockup's three, in its order; a file
/// without a `group` word lands after them (`Group::label` is the row the
/// picker paints; the trailing group's label is `OTHER`).
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub enum Group {
    Dark,
    Terminal,
    Light,
}

impl Group {
    /// The `[meta] group` word.
    pub fn parse(word: &str) -> Option<Group> {
        Some(match word {
            "dark" => Group::Dark,
            "terminal" => Group::Terminal,
            "light" => Group::Light,
            _ => return None,
        })
    }

    /// The picker's group row (the mockup's `.theme-group-label` text).
    pub fn label(self) -> &'static str {
        match self {
            Group::Dark => "DARK FIELD",
            Group::Terminal => "TERMINAL STUDIES",
            Group::Light => "LIGHT FIELD",
        }
    }
}

/// The label of the trailing group for files that name none.
pub const OTHER_GROUP_LABEL: &str = "OTHER";

/// The tables this schema knows. No `[geometry]`: an Instrument file that
/// carries one is refused at its header.
const TABLES: &[&str] = &["meta", "color", "terminal", "type"];

// Carbon Optics: the kit's default theme, the Instrument floor. The 35 are
// `round2/ui-palettes/carbon.toml` (= `resolved-tokens.json`, Carbon
// unchanged by round 2), the ANSI is the adopted set (`tools/halcyon/
// ansi16.json`, Astra's Carbon verbatim); `carbon_matches_the_record` pins
// every value against those files. Same visibility split as `DAYLIGHT`
// (HALCYON-THEME 3.2): production reaches it through `builtin()` only.
const CARBON_THEME: InstrumentTheme = InstrumentTheme {
    desktop: 0xFF05_0607,
    pane: 0xFF0B_0D0E,
    open: 0xFF12_1516,
    header: 0xFF08_0A0B,
    hover: 0xFF19_1C1D,
    text: 0xFFF2_F3EF,
    secondary: 0xFFAF_B4B0,
    dim: 0xFF73_7A76,
    structure: 0xFF45_4B48,
    separator: 0xFF29_2D2B,
    amber: 0xFFC7_B98B,
    amber_muted: 0xFF81_785D,
    error: 0xFFBD_7770,
    success: 0xFF81_9B85,
    terminal_path: 0xFF96_AAA6,
    rail: 0xFF07_090A,
    pane_border: 0xFF1B_1F1E,
    focus_neutral: 0xFF55_5B58,
    body_text: 0xFFD9_DDD9,
    code_text: 0xFFC7_B98B,
    code_bg: 0xFF09_0B0C,
    code_body: 0xFFC5_CAC6,
    terminal_bg: 0xFF09_0C0D,
    terminal_text: 0xFFCB_D0CC,
    dialog_bg: 0xFF0D_1011,
    kbd_bg: 0xFF06_0809,
    syntax_keyword: 0xFFC7_B98B,
    syntax_type: 0xFF8E_A4B8,
    syntax_function: 0xFF91_AA98,
    syntax_string: 0xFFB9_9A7B,
    syntax_number: 0xFFA6_93AD,
    syntax_attribute: 0xFFB5_8B70,
    syntax_lifetime: 0xFF9D_8FA5,
    syntax_comment: 0xFF77_807C,
    syntax_punctuation: 0xFFA6_ACA8,
    ansi: [
        0xFF66_6D68,
        0xFFB9_7B77,
        0xFF82_9F88,
        0xFFB5_A26B,
        0xFF81_9EBB,
        0xFFA4_8CAE,
        0xFF79_A6AA,
        0xFFBE_C5BF,
        0xFF92_9B94,
        0xFFD6_9A95,
        0xFFA5_BFAA,
        0xFFD1_C08D,
        0xFFA6_BDCF,
        0xFFC3_ACCB,
        0xFF9E_C3C4,
        0xFFF2_F3EF,
    ],
    smooth_mem: 0,
    light: false,
};

#[cfg(feature = "theme-fixture")]
pub const CARBON: InstrumentTheme = CARBON_THEME;
#[cfg(not(feature = "theme-fixture"))]
pub(crate) const CARBON: InstrumentTheme = CARBON_THEME;

/// The Instrument floor: what the loader falls back to under the Instrument
/// profile when no theme file loads. Call it at the one place that resolves
/// a session's bundle, never at a paint site.
pub const fn builtin() -> InstrumentTheme {
    CARBON
}

/// The legacy geometry an Instrument theme PROJECTS to (THEME-CONVERSION 3):
/// the kit's stock files carry exactly this table, and `project_legacy`
/// reproduces them byte for byte. It is evidence of why a palette install is
/// not the migration -- top 34 is absent, the 7 px track is a 3 px gap.
pub const LEGACY_PROJECTED_METRICS: Metrics = Metrics::legacy(2, 3, 1, 32, 25, 0, 0);

/// The Instrument profile's geometry (HALCYON-INSTRUMENT 5.1 / 5.7): the ONE
/// table, a compiled constant no file carries -- an Instrument theme has no
/// `[geometry]`, so the carve cannot move under a theme. The legacy fields
/// it shares: `header_h` 32 (the tile header), `status_h` 25 (the bottom
/// rail), `hairline` 1 (the separator and the index rule); `bevel`, `gap`
/// and `tab_strip_h` are ABSENT (0), which is how the legacy painters and
/// the legacy carve stay inert under this profile.
pub const INSTRUMENT_BASE: Metrics = Metrics {
    bevel: 0,
    gap: 0,
    hairline: 1,
    header_h: 32,
    status_h: 25,
    tag_pad_x: 0,
    tab_strip_h: 0,
    rail_h: 34,
    outer_pad: 3,
    track: 7,
    rule: 2,
    rule_off: 2,
    joint: 7,
    frame: 1,
    index_w: 32,
    header_gap: 7,
    action_w: 28,
    mark_w: 2,
    mark_inset_y: 6,
    min_pane_w: crate::carve::MIN_PANE_W,
    min_body_h: crate::carve::MIN_BODY_H,
};

/// The logical geometry table a profile carves and paints with (4.4 /
/// 5.7): a legacy theme's own `[geometry]` under `legacy`, the compiled
/// `INSTRUMENT_BASE` under `instrument` -- whatever the theme's projected
/// table says. The one function `Bundle::at` and halcyond's sheet share.
pub fn metrics_base(profile: Profile, legacy: &Theme) -> Metrics {
    match profile {
        Profile::Legacy => legacy.metrics,
        Profile::Instrument => INSTRUMENT_BASE,
    }
}

/// An Instrument theme file, loaded.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LoadedInstrument {
    pub theme: InstrumentTheme,
    /// `[meta] id`: the gallery filename and the picker's key.
    pub id: String,
    /// `[meta] name`, presentable (the legacy rule).
    pub name: String,
    /// `[meta] group` (9.4), None when the file names none.
    pub group: Option<Group>,
    /// `[meta] tagline`, the picker's subtitle; empty when absent (the
    /// picker shows the id then).
    pub tagline: String,
    /// `[meta] rank`, the order within the group; `RANK_UNRANKED` when
    /// absent.
    pub rank: u8,
}

/// Is this word a gallery id -- and therefore safe to become the path
/// component `/lib/halcyon/themes/<id>.toml`? `[a-z][a-z0-9_-]{0,31}`; so
/// never empty, never `.`, `..`, a slash or anything a shell would read.
pub fn is_gallery_id(word: &str) -> bool {
    let b = word.as_bytes();
    if b.is_empty() || b.len() > ID_MAX || !b[0].is_ascii_lowercase() {
        return false;
    }
    b.iter()
        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || *c == b'_' || *c == b'-')
}

/// The id a pick FILE holds: the word with at most one trailing newline
/// (an editor adds one), and only if it is a gallery id. ONE normalisation
/// for every reader of `$HOME/lib/halcyon/theme` -- the session, the lint
/// and `resolve_bundle` -- so no reader can accept a word another refuses.
pub fn pick_id(word: &str) -> Option<&str> {
    let w = word.strip_suffix('\n').unwrap_or(word);
    is_gallery_id(w).then_some(w)
}

/// The gallery file for an id, or `None` if the word is not an id.
pub fn gallery_path(id: &str) -> Option<String> {
    if !is_gallery_id(id) {
        return None;
    }
    let mut s = String::from(GALLERY_DIR);
    s.push('/');
    s.push_str(id);
    s.push_str(".toml");
    Some(s)
}

fn colour(v: &Value, line: u32) -> Result<Argb, LoadError> {
    match v {
        Value::Str(s) => theme::parse_colour(s).ok_or(LoadError::BadColour { line }),
        _ => Err(LoadError::BadShape { line }),
    }
}

/// Apply one non-meta entry. `Ok(true)` handled; `Ok(false)` no such key.
fn set_key(t: &mut InstrumentTheme, table: &str, key: &str, v: &Value, line: u32) -> Result<bool, LoadError> {
    match (table, key) {
        ("color", k) => {
            let slot: &mut Argb = match k {
                "desktop" => &mut t.desktop,
                "pane" => &mut t.pane,
                "open" => &mut t.open,
                "header" => &mut t.header,
                "hover" => &mut t.hover,
                "text" => &mut t.text,
                "secondary" => &mut t.secondary,
                "dim" => &mut t.dim,
                "structure" => &mut t.structure,
                "separator" => &mut t.separator,
                "amber" => &mut t.amber,
                "amber_muted" => &mut t.amber_muted,
                "error" => &mut t.error,
                "success" => &mut t.success,
                "terminal_path" => &mut t.terminal_path,
                "rail" => &mut t.rail,
                "pane_border" => &mut t.pane_border,
                "focus_neutral" => &mut t.focus_neutral,
                "body_text" => &mut t.body_text,
                "code_text" => &mut t.code_text,
                "code_bg" => &mut t.code_bg,
                "code_body" => &mut t.code_body,
                "terminal_bg" => &mut t.terminal_bg,
                "terminal_text" => &mut t.terminal_text,
                "dialog_bg" => &mut t.dialog_bg,
                "kbd_bg" => &mut t.kbd_bg,
                "syntax_keyword" => &mut t.syntax_keyword,
                "syntax_type" => &mut t.syntax_type,
                "syntax_function" => &mut t.syntax_function,
                "syntax_string" => &mut t.syntax_string,
                "syntax_number" => &mut t.syntax_number,
                "syntax_attribute" => &mut t.syntax_attribute,
                "syntax_lifetime" => &mut t.syntax_lifetime,
                "syntax_comment" => &mut t.syntax_comment,
                "syntax_punctuation" => &mut t.syntax_punctuation,
                _ => return Ok(false),
            };
            *slot = colour(v, line)?;
        }
        ("terminal", "ansi") => match v {
            Value::Array(items) => {
                // Exactly 16, then the slot rule: sixteen distinct values,
                // except that bright white may equal the terminal text (the
                // alias `vt` allows). A short array or a duplicated slot is
                // refused rather than half-applied.
                if items.len() != 16 {
                    return Err(LoadError::BadShape { line });
                }
                let mut ansi = [0u32; 16];
                for (i, s) in items.iter().enumerate() {
                    ansi[i] = theme::parse_colour(s).ok_or(LoadError::BadColour { line })?;
                }
                for i in 0..16 {
                    for j in (i + 1)..16 {
                        if ansi[i] == ansi[j] {
                            return Err(LoadError::AnsiNotDistinct { line });
                        }
                    }
                }
                t.ansi = ansi;
            }
            _ => return Err(LoadError::BadShape { line }),
        },
        ("type", "smooth") => match v {
            Value::Int(n) if (theme::smooth_bounds().0..=theme::smooth_bounds().1).contains(n) => {
                t.smooth_mem = *n as u16
            }
            Value::Int(_) => return Err(LoadError::OutOfRange { line }),
            _ => return Err(LoadError::BadShape { line }),
        },
        _ => return Ok(false),
    }
    Ok(true)
}

/// Load an Instrument theme from parsed entries (`theme::load` dispatched
/// here on `[meta] profile`). Every key required, no `base`, unknown keys
/// and tables refused, the meta strict: `schema = 1`, `profile` this word,
/// `id` a gallery id, `name` presentable, `color_scheme` dark or light. A
/// file is accepted whole or refused whole (HALCYON-THEME 4.2).
pub fn from_entries(entries: &[Entry<'_>], src_len: usize) -> Result<LoadedInstrument, LoadError> {
    if src_len > INSTRUMENT_MAX {
        return Err(LoadError::Syntax(crate::toml::Error {
            line: 1,
            kind: crate::toml::Kind::TooLarge,
        }));
    }
    let mut id = String::new();
    let mut name = String::new();
    let mut light: Option<bool> = None;
    let mut group: Option<Group> = None;
    let mut tagline = String::new();
    let mut rank = RANK_UNRANKED;
    let mut meta_seen = [false; META_KEYS.len()];
    for e in entries.iter().filter(|e| e.table == "meta") {
        if META_OPTIONAL.contains(&e.key) {
            match (e.key, &e.value) {
                ("group", Value::Str(w)) => {
                    group = Some(Group::parse(w).ok_or(LoadError::OutOfRange { line: e.line })?);
                }
                ("tagline", Value::Str(t)) => {
                    if !theme::name_is_presentable(t) {
                        return Err(LoadError::BadName { line: e.line });
                    }
                    tagline = String::from(*t);
                }
                ("rank", Value::Int(r)) => {
                    rank = u8::try_from(*r).map_err(|_| LoadError::OutOfRange { line: e.line })?;
                }
                _ => return Err(LoadError::BadShape { line: e.line }),
            }
            continue;
        }
        let idx = META_KEYS
            .iter()
            .position(|k| *k == e.key)
            .ok_or(LoadError::UnknownKey { line: e.line })?;
        match (e.key, &e.value) {
            ("schema", Value::Int(1)) => {}
            ("schema", Value::Int(_)) => return Err(LoadError::OutOfRange { line: e.line }),
            ("profile", Value::Str(PROFILE_WORD)) => {}
            ("profile", Value::Str(_)) => return Err(LoadError::UnknownProfile { line: e.line }),
            ("id", Value::Str(s)) => {
                if !is_gallery_id(s) {
                    return Err(LoadError::BadId { line: e.line });
                }
                id = String::from(*s);
            }
            ("name", Value::Str(s)) => {
                if !theme::name_is_presentable(s) {
                    return Err(LoadError::BadName { line: e.line });
                }
                name = String::from(*s);
            }
            ("color_scheme", Value::Str("dark")) => light = Some(false),
            ("color_scheme", Value::Str("light")) => light = Some(true),
            ("color_scheme", Value::Str(_)) => return Err(LoadError::OutOfRange { line: e.line }),
            _ => return Err(LoadError::BadShape { line: e.line }),
        }
        meta_seen[idx] = true;
    }

    // Start from the floor ANYWAY, as the legacy loader does: every key is
    // required, so nothing of it survives into the result; the type stays
    // total without an all-`Option` mirror.
    let mut t = builtin();
    let mut seen = [false; KEYS.len()];
    for e in entries {
        if e.table == "meta" {
            continue;
        }
        if e.table.is_empty() {
            return Err(LoadError::NoTable { line: e.line });
        }
        if !TABLES.contains(&e.table) {
            return Err(LoadError::UnknownTable { line: e.table_line });
        }
        if !set_key(&mut t, e.table, e.key, &e.value, e.line)? {
            return Err(LoadError::UnknownKey { line: e.line });
        }
        if let Some(i) = KEYS.iter().position(|(tb, k)| *tb == e.table && *k == e.key) {
            seen[i] = true;
        }
    }
    let mut missing: Vec<String> = Vec::new();
    for (i, k) in META_KEYS.iter().enumerate() {
        if !meta_seen[i] {
            missing.push(theme::key_name("meta", k));
        }
    }
    for (i, (tb, k)) in KEYS.iter().enumerate() {
        if !seen[i] {
            missing.push(theme::key_name(tb, k));
        }
    }
    if !missing.is_empty() {
        return Err(LoadError::Incomplete { missing });
    }
    t.light = light.unwrap_or(false);
    Ok(LoadedInstrument {
        theme: t,
        id,
        name,
        group,
        tagline,
        rank,
    })
}

// ---------------------------------------------------------------------------
// The projections (4.3): pure, approximate by construction, labelled so.
// ---------------------------------------------------------------------------

/// `a` moved toward `b` by `p`, per channel, rounded half up -- the kit's
/// `mix()` exactly (`build_bundle.py`), in the same double arithmetic, so the
/// projected stock files are reproduced byte for byte rather than nearly.
fn mix(a: Argb, b: Argb, p: f64) -> Argb {
    let ch = |shift: u32| -> u32 {
        let x = ((a >> shift) & 0xFF) as f64;
        let y = ((b >> shift) & 0xFF) as f64;
        (x * (1.0 - p) + y * p + 0.5) as u32
    };
    0xFF00_0000 | (ch(16) << 16) | (ch(8) << 8) | ch(0)
}

/// An Instrument theme as a legacy `Theme`, the kit's stock mapping
/// (THEME-CONVERSION 3) as code: `floor = desktop`, `surface = open`, the
/// bevel faces a coherent light around `desktop` (the mockup has no bevel),
/// the live-key families neutral, the nine syntax roles by semantics, the
/// legacy geometry `LEGACY_PROJECTED_METRICS`. It exists so an Instrument
/// theme renders under the legacy painters and so `/env/HALCYON_PALETTE`
/// keeps its shape for hosted programs.
pub fn project_legacy(i: &InstrumentTheme) -> Theme {
    let white = 0xFFFF_FFFF;
    let black = 0xFF00_0000;
    let family = |key: Argb| LiveKey {
        key,
        tint: i.header,
        raised: i.hover,
        border: i.separator,
        fg: i.text,
        fg_dim: i.secondary,
        fg_muted: i.dim,
    };
    Theme {
        floor: i.desktop,
        surface: i.open,
        header: i.header,
        raised: i.hover,
        border: i.structure,
        blank: i.pane,
        selection: mix(i.open, i.amber, 0.15),
        island_rule: i.amber_muted,
        fg: i.text,
        fg_dim: i.body_text,
        fg_muted: i.secondary,
        fg_subtle: i.dim,
        bevel_top: mix(i.desktop, white, 0.16),
        bevel_left: mix(i.desktop, white, 0.09),
        bevel_right: mix(i.desktop, black, 0.22),
        bevel_bottom: mix(i.desktop, black, 0.46),
        ember: i.amber,
        ember_dim: i.amber_muted,
        ember_deep: i.amber_muted,
        sage: family(i.success),
        cinnabar: family(i.error),
        syntax: Syntax {
            slate: i.syntax_keyword,
            sage: i.syntax_type,
            sand: i.syntax_attribute,
            moss: i.syntax_number,
            ash: i.syntax_function,
            dusk: i.syntax_string,
            smoke: i.syntax_comment,
            fen: i.success,
            cinnabar: i.error,
        },
        status_bg: i.rail,
        status_fg: i.text,
        status_muted: i.secondary,
        status_idle: i.dim,
        metrics: LEGACY_PROJECTED_METRICS,
        terminal: vt::Palette {
            bg: i.terminal_bg,
            fg: i.terminal_text,
            ansi: i.ansi,
        },
        smooth_mem: i.smooth_mem,
    }
}

/// A rough relative brightness, enough to tell a light ground from a dark
/// one (the only question asked of it).
fn brightness(c: Argb) -> u32 {
    ((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)
}

/// A legacy `Theme` as an Instrument theme -- the reverse mapping (4.3), so
/// Daylight and Nightjar render under the Instrument geometry. Roles the
/// legacy schema lacks (lifetime, punctuation, the neutral focus frame) take
/// the nearest ink; the polarity is read off the ground and the ink.
pub fn project_instrument(t: &Theme) -> InstrumentTheme {
    InstrumentTheme {
        desktop: t.floor,
        pane: t.blank,
        open: t.surface,
        header: t.header,
        hover: t.raised,
        text: t.fg,
        secondary: t.fg_muted,
        dim: t.fg_subtle,
        structure: t.border,
        separator: t.border,
        amber: t.ember,
        amber_muted: t.ember_dim,
        error: t.cinnabar.key,
        success: t.sage.key,
        terminal_path: t.syntax.slate,
        rail: t.status_bg,
        pane_border: t.border,
        focus_neutral: t.fg_muted,
        body_text: t.fg_dim,
        code_text: t.syntax.dusk,
        code_bg: t.header,
        code_body: t.fg,
        terminal_bg: t.terminal.bg,
        terminal_text: t.terminal.fg,
        dialog_bg: t.raised,
        kbd_bg: t.header,
        syntax_keyword: t.syntax.slate,
        syntax_type: t.syntax.sage,
        syntax_function: t.syntax.ash,
        syntax_string: t.syntax.dusk,
        syntax_number: t.syntax.moss,
        syntax_attribute: t.syntax.sand,
        syntax_lifetime: t.fg_muted,
        syntax_comment: t.syntax.smoke,
        syntax_punctuation: t.fg_muted,
        ansi: t.terminal.ansi,
        smooth_mem: t.smooth_mem,
        light: brightness(t.surface) > brightness(t.fg),
    }
}

// ---------------------------------------------------------------------------
// The profile, the bundle, the resolution (4.1, 4.4).
// ---------------------------------------------------------------------------

/// Which painters' state machine, geometry and type map a display runs. A
/// compiled table, never a runtime file a theme could name.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Profile {
    Legacy,
    Instrument,
}

impl Profile {
    /// The one word a profile file holds. Exact, lowercase; a trailing
    /// newline is tolerated because an editor adds one.
    pub fn parse(word: &str) -> Option<Profile> {
        match word.strip_suffix('\n').unwrap_or(word) {
            "legacy" => Some(Profile::Legacy),
            "instrument" => Some(Profile::Instrument),
            _ => None,
        }
    }

    pub const fn word(self) -> &'static str {
        match self {
            Profile::Legacy => "legacy",
            Profile::Instrument => "instrument",
        }
    }
}

/// Which schema a loaded file was in.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Schema {
    Legacy,
    Instrument,
}

/// The resolved, scale-free bundle: the profile in force and BOTH themes,
/// one native and one projected. This is what crosses the wire; `at(pct)`
/// makes it the `Visual` a painter is handed.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Bundle {
    pub profile: Profile,
    pub theme: Theme,
    pub inst: InstrumentTheme,
}

/// What every painter is handed; nothing else reads a constant (4.4).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Visual {
    pub profile: Profile,
    pub theme: Theme,
    pub inst: InstrumentTheme,
    pub metrics: Metrics,
    /// The derived opaques (7.3), computed once here so no painter blends
    /// where the substrate is known.
    pub derived: Derived,
}

/// HALCYON-INSTRUMENT 7.3, the derived opaques: colours the mockup states
/// as an alpha over a KNOWN substrate, resolved once per theme so every
/// painter fills them flat. The arithmetic is the executor's `blend`
/// (cartoon: an 8-bit alpha over 256, per lane, truncating) -- the same
/// lerp the transcript's antialiased edges take, so a header and a glyph on
/// it agree on what 1.5 % of `text` over `open` is. Carbon's `open_header`
/// is `#151819`, the kit's own figure (`carbon_derives_the_kits_opaques`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Derived {
    /// The expanded tile's header ground: `text` at 1.5 % over `open`.
    pub open_header: Argb,
    /// The selection band: `amber` at 15 % over `open`.
    pub selection: Argb,
    /// The focused frame's 1 px inset over a collapsed header: `text` at
    /// 3 % over `header`.
    pub focus_inset_header: Argb,
    /// The same inset over an open tile: `text` at 3 % over `open`.
    pub focus_inset_open: Argb,
    /// The theme control's swatch ring (8.1): white at 12 % over `amber`
    /// -- the kit's `inset 0 0 0 1px rgba(255,255,255,.12)`.
    pub swatch_ring: Argb,
}

/// `fg` over `bg` at `a` / 256 -- the executor's lerp, lane by lane.
/// (`cartoon::blend` is this exact function; it is repeated here rather
/// than depended on so the theme library stays free of the painter's
/// crate, and the test pins the two agree on the kit's figure.)
pub const fn over(bg: Argb, fg: Argb, a: u8) -> Argb {
    if a == 0 {
        return bg;
    }
    if a == 255 {
        return fg;
    }
    let a = a as u32;
    let na = 256 - a;
    let rb = (((fg & 0x00FF_00FF) * a + (bg & 0x00FF_00FF) * na) >> 8) & 0x00FF_00FF;
    let g = (((fg & 0x0000_FF00) * a + (bg & 0x0000_FF00) * na) >> 8) & 0x0000_FF00;
    0xFF00_0000 | rb | g
}

/// The alpha byte for a percentage of 256 (round half up): 1.5 % -> 4,
/// 3 % -> 8, 15 % -> 38.
const fn pct256(tenths: u32) -> u8 {
    ((tenths * 256 + 500) / 1000) as u8
}

impl Derived {
    pub const fn of(i: &InstrumentTheme) -> Derived {
        Derived {
            open_header: over(i.open, i.text, pct256(15)),
            selection: over(i.open, i.amber, pct256(150)),
            focus_inset_header: over(i.header, i.text, pct256(30)),
            focus_inset_open: over(i.open, i.text, pct256(30)),
            swatch_ring: over(i.amber, 0xFFFF_FFFF, pct256(120)),
        }
    }
}

/// HALCYON-INSTRUMENT 10's effect literals -- the SOURCE's own effect
/// colours, which DO NOT tokenise.
///
/// Section 10 is explicit: they "stay amber / green literals on every theme
/// (the CSS does not tokenise them)". That is why they live here as
/// constants rather than as `InstrumentTheme` fields, and the distinction is
/// not cosmetic -- it is MEASURABLE. Carbon's `amber` is `#C7B98B`, a pale
/// sand; the divider glow is `#D59A42`, a saturated orange. Carbon's
/// `success` is `#819B85`; the status glow is `#70A17C`. A painter reaching
/// for the token paints the wrong colour under Carbon and a DIFFERENT wrong
/// colour under every other theme, which is exactly the drift section 10
/// forbids. The tests below pin each literal against its token so a
/// re-tokenisation fails loudly instead of looking plausible.
///
/// Alphas are `pct256` of the stated percentage, the same rounding
/// `Derived` takes, so an effect and a derived opaque that both say ".25"
/// agree to the byte.
pub mod effects {
    use super::{Argb, pct256};

    /// The divider drag glow: `rgba(213,154,66,.25)`, blur 10 (9.2's
    /// `.dragging` rule).
    pub const DIVIDER_DRAG: Argb = 0xFFD5_9A42;
    pub const DIVIDER_DRAG_ALPHA: u8 = pct256(250);
    pub const DIVIDER_DRAG_BLUR: i32 = 10;

    /// The split flash: `rgba(213,154,66,.04)` for 250 ms, with a 1 px
    /// `amber` border inset 5 (that border IS the token -- only the fill is
    /// a literal).
    pub const SPLIT_FLASH: Argb = 0xFFD5_9A42;
    pub const SPLIT_FLASH_ALPHA: u8 = pct256(40);
    pub const SPLIT_FLASH_MS: u32 = 250;

    /// The status condition glow: `rgba(112,161,124,.25)`, blur 8. Carried
    /// by SUCCESS alone (8.2 keeps RUNNING pulse-free; section 10's I-8
    /// amendment).
    pub const STATUS_SUCCESS: Argb = 0xFF70_A17C;
    pub const STATUS_SUCCESS_ALPHA: u8 = pct256(250);
    pub const STATUS_SUCCESS_BLUR: i32 = 8;

    /// The ONE drop shadow every menu card takes: black .35 at (0, 24),
    /// blur 80 (the help card's heavier pair).
    ///
    /// Section 10 states TWO -- the picker at .32 / (0,20) / blur 55 -- and
    /// they collapsed here, operator-answered 2026-09-16 and recorded in
    /// section 10's amendment. The compositor cannot tell a picker from a
    /// help card: all four halcyond models ride one `Role::Menu` surface,
    /// `MenuState` carries only `{n, gen, rect}`, `surf.title` is written
    /// and never read, and `menu place` takes only coordinates. And the
    /// radius cap had already erased most of the difference -- it flattens
    /// blur 55 and blur 80 to the SAME value, leaving alpha 82 vs 90 and
    /// dy 20 vs 24.
    pub const CARD_SHADOW: Argb = 0xFF00_0000;
    pub const CARD_SHADOW_ALPHA: u8 = pct256(350);
    pub const CARD_SHADOW_DY: i32 = 24;
    pub const CARD_SHADOW_BLUR: i32 = 80;

    /// The modal backdrop: `rgb(3,4,4)` at .72 over a 3 px blur of the
    /// scene. The BLUR is the compositor's own machinery (no cartoon op
    /// blurs existing pixels); this is the tint composited over it.
    pub const BACKDROP: Argb = 0xFF03_0404;
    pub const BACKDROP_ALPHA: u8 = pct256(720);
    pub const BACKDROP_BLUR: i32 = 3;

    // The seventh effect -- the swatch's white .12 inset border -- is NOT
    // here: its substrate is known (`amber`), so 7.3 resolves it once as
    // `Derived.swatch_ring` instead of compositing it per frame.
}

impl Bundle {
    /// A native legacy theme; the Instrument side projected.
    pub fn from_legacy(profile: Profile, theme: Theme) -> Bundle {
        Bundle {
            profile,
            theme,
            inst: project_instrument(&theme),
        }
    }

    /// A native Instrument theme; the legacy side projected.
    pub fn from_instrument(profile: Profile, inst: InstrumentTheme) -> Bundle {
        Bundle {
            profile,
            theme: project_legacy(&inst),
            inst,
        }
    }

    /// The floor for a profile: Daylight under `legacy`, Carbon under
    /// `instrument` -- each native, the other side projected.
    pub fn builtin(profile: Profile) -> Bundle {
        match profile {
            Profile::Legacy => Bundle::from_legacy(profile, theme::builtin()),
            Profile::Instrument => Bundle::from_instrument(profile, builtin()),
        }
    }

    /// The bundle at a display scale: the PROFILE picks the geometry table
    /// (HALCYON-INSTRUMENT 4.4 / 5.7) -- the theme's own `[geometry]` under
    /// `legacy`, the compiled `INSTRUMENT_BASE` under `instrument`, each
    /// through the one `Metrics::at`. This is the function both the
    /// compositor's carve and halcyond's sheet read, so a profile flip moves
    /// both painters together or neither.
    pub fn at(&self, pct: u16) -> Visual {
        Visual {
            profile: self.profile,
            theme: self.theme,
            inst: self.inst,
            metrics: self.metrics_base().at(pct),
            derived: Derived::of(&self.inst),
        }
    }

    /// The profile's logical geometry table (the base `at` scales).
    pub fn metrics_base(&self) -> Metrics {
        metrics_base(self.profile, &self.theme)
    }
}

/// A loaded theme file of either schema.
#[derive(Clone, Debug)]
pub enum LoadedAny {
    Legacy(theme::Loaded),
    Instrument(LoadedInstrument),
}

impl LoadedAny {
    pub fn schema(&self) -> Schema {
        match self {
            LoadedAny::Legacy(_) => Schema::Legacy,
            LoadedAny::Instrument(_) => Schema::Instrument,
        }
    }

    pub fn name(&self) -> &str {
        match self {
            LoadedAny::Legacy(l) => &l.name,
            LoadedAny::Instrument(l) => &l.name,
        }
    }

    /// The bundle this file makes under a profile: its own schema native,
    /// the other projected.
    pub fn bundle(&self, profile: Profile) -> Bundle {
        match self {
            LoadedAny::Legacy(l) => Bundle::from_legacy(profile, l.theme),
            LoadedAny::Instrument(l) => Bundle::from_instrument(profile, l.theme),
        }
    }
}

/// Where a resolved component came from.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tier {
    /// The binary's floor.
    BuiltIn,
    /// `/lib/halcyon/...`.
    System,
    /// `$HOME/lib/halcyon/theme.toml` or `.../profile`.
    User,
    /// `$HOME/lib/halcyon/theme`, the picker's gallery id.
    Pick,
}

/// The file contents a resolution reads, injected so the policy is pure and
/// host-tested (the I/O stays at the caller, as `theme::resolve`'s does).
#[derive(Clone, Copy, Debug, Default)]
pub struct Sources<'a> {
    /// `/lib/halcyon/profile`.
    pub system_profile: Option<&'a str>,
    /// `$HOME/lib/halcyon/profile`.
    pub user_profile: Option<&'a str>,
    /// `$HOME/lib/halcyon/theme` -- the word itself; the caller resolves it
    /// to a gallery file only when `is_gallery_id` says it may.
    pub user_pick: Option<&'a str>,
    /// The gallery file that word named, if the caller found one.
    pub pick_file: Option<&'a str>,
    /// `$HOME/lib/halcyon/theme.toml`.
    pub user_file: Option<&'a str>,
    /// `/lib/halcyon/theme.toml`.
    pub system_file: Option<&'a str>,
}

/// A resolved bundle plus everything the resolution is owed to say.
#[derive(Clone, Debug)]
pub struct ResolvedBundle {
    pub bundle: Bundle,
    pub profile_tier: Tier,
    pub theme_tier: Tier,
    pub schema: Schema,
    /// The winning file's `[meta] name`, or empty.
    pub name: String,
    /// The winning file's `[meta] id` (Instrument) or the pick word, or empty.
    pub id: String,
    /// LOUD refusals, one per tier that failed (HALCYON-THEME 4.2); a
    /// missing file is silent (4.1).
    pub notes: Vec<String>,
    /// Keys the winning legacy file inherited from its base (4.3).
    pub inherited: Vec<String>,
}

/// The profile the two tiers select -- the user's word, else the system's,
/// else legacy -- with the tier it came from and the label ("user",
/// "system") of each word before it that was not a profile. The first step
/// of `resolve_bundle`, alone, for a caller that needs only the profile:
/// `halcyon layout restore` lays a saved stack's container members flat
/// under Instrument (HALCYON-INSTRUMENT 6.1).
pub fn resolve_profile(user: Option<&str>, system: Option<&str>) -> (Profile, Tier, Vec<&'static str>) {
    let mut refused: Vec<&'static str> = Vec::new();
    for (word, tier, label) in [(user, Tier::User, "user"), (system, Tier::System, "system")] {
        let Some(w) = word else { continue };
        match Profile::parse(w) {
            Some(p) => return (p, tier, refused),
            None => refused.push(label),
        }
    }
    (Profile::Legacy, Tier::BuiltIn, refused)
}

/// Resolve a session's bundle (HALCYON-INSTRUMENT 4.1). The profile: the
/// user's word, then the system's, then `legacy` (until the rollout flips
/// the floor, 12). The theme: the picker's gallery choice, then the user's
/// file, then the system's, then the profile's floor -- each refused tier
/// falling ONE step down, loudly, as `theme::resolve` does. A theme in the
/// other schema is projected, never refused.
pub fn resolve_bundle(src: Sources<'_>) -> ResolvedBundle {
    let mut notes: Vec<String> = Vec::new();
    let (profile, profile_tier, refused) = resolve_profile(src.user_profile, src.system_profile);
    for label in refused {
        let mut n = String::new();
        let _ = write!(n, "theme: {label} profile REFUSED -- not `legacy` or `instrument`");
        notes.push(n);
    }

    // The pick is a WORD naming a gallery file. A word that is not an id is
    // refused here even if the caller (wrongly) supplied a file for it: the
    // word is user-authored and the path it would form is not. A valid id
    // with no file behind it is SAID too -- a stale pick after a gallery
    // change would otherwise read as "the picker did nothing".
    let pick_file = match src.user_pick.map(pick_id) {
        Some(None) => {
            notes.push(String::from("theme: user pick REFUSED -- not a gallery id"));
            None
        }
        Some(Some(id)) if src.pick_file.is_none() => {
            let mut n = String::new();
            let _ = write!(n, "theme: user pick `{id}` names no gallery file");
            notes.push(n);
            None
        }
        Some(Some(_)) => src.pick_file,
        None => None,
    };
    for (text, tier, label) in [
        (pick_file, Tier::Pick, "picked gallery"),
        (src.user_file, Tier::User, "user"),
        (src.system_file, Tier::System, "system"),
    ] {
        let Some(text) = text else { continue };
        match theme::load(text) {
            Ok(any) => {
                let (id, inherited) = match &any {
                    LoadedAny::Instrument(l) => (l.id.clone(), Vec::new()),
                    LoadedAny::Legacy(l) => (String::new(), l.inherited.clone()),
                };
                return ResolvedBundle {
                    bundle: any.bundle(profile),
                    profile_tier,
                    theme_tier: tier,
                    schema: any.schema(),
                    name: String::from(any.name()),
                    id,
                    notes,
                    inherited,
                };
            }
            Err(e) => {
                let mut n = String::new();
                let _ = write!(n, "theme: {label} theme REFUSED -- {}", theme::describe(&e));
                notes.push(n);
            }
        }
    }
    ResolvedBundle {
        bundle: Bundle::builtin(profile),
        profile_tier,
        theme_tier: Tier::BuiltIn,
        schema: match profile {
            Profile::Legacy => Schema::Legacy,
            Profile::Instrument => Schema::Instrument,
        },
        name: String::new(),
        id: String::new(),
        notes,
        inherited: Vec::new(),
    }
}

#[cfg(test)]
mod tests {
    /// The profile alone resolves exactly as `resolve_bundle`'s first step:
    /// the user's word wins, a word that is not a profile is skipped and
    /// named, and nothing at all is legacy from the built-in tier.
    #[test]
    fn the_profile_resolves_user_over_system_skipping_a_bad_word() {
        use super::{resolve_profile, Profile, Tier};
        assert_eq!(resolve_profile(None, None), (Profile::Legacy, Tier::BuiltIn, alloc::vec![]));
        assert_eq!(resolve_profile(None, Some("instrument")), (Profile::Instrument, Tier::System, alloc::vec![]));
        assert_eq!(resolve_profile(Some("legacy"), Some("instrument")), (Profile::Legacy, Tier::User, alloc::vec![]));
        assert_eq!(resolve_profile(Some("bogus"), Some("instrument")), (Profile::Instrument, Tier::System, alloc::vec!["user"]));
        assert_eq!(resolve_profile(Some("bogus"), Some("nope")), (Profile::Legacy, Tier::BuiltIn, alloc::vec!["user", "system"]));
    }

    /// Section 10: the effect literals are LITERALS, and the proof is that
    /// each differs from the token a painter would otherwise reach for.
    /// This is the assertion whose absence let the status glow ship
    /// tokenised as `inst.success` -- plausible because Carbon's success and
    /// the kit's sage sit close together, and wrong on every theme.
    #[test]
    fn the_effect_literals_are_not_theme_tokens() {
        use super::effects;
        assert_eq!(effects::DIVIDER_DRAG, 0xFFD5_9A42, "10's rgba(213,154,66)");
        assert_eq!(effects::STATUS_SUCCESS, 0xFF70_A17C, "10's rgba(112,161,124)");
        assert_eq!(effects::BACKDROP, 0xFF03_0404, "10's rgb(3,4,4)");
        // The discriminating half: a painter reaching for the token paints
        // something else, under Carbon and under every other theme.
        assert_ne!(effects::DIVIDER_DRAG, CARBON.amber, "the glow is not `amber`");
        assert_ne!(effects::STATUS_SUCCESS, CARBON.success, "the glow is not `success`");
        // The alphas are pct256 of the stated percentage, Derived's rounding.
        assert_eq!(effects::STATUS_SUCCESS_ALPHA, 64, ".25");
        assert_eq!(effects::CARD_SHADOW_ALPHA, 90, ".35 -- the help card's, for every card");
        // The collapse is deliberate (section 10, amended 2026-09-16): the
        // radius cap flattens blur 55 and blur 80 alike, so a second shadow
        // would differ only by 8/256 of alpha and four pixels of offset.
        assert_eq!(effects::CARD_SHADOW_BLUR, 80, "clamped to GLOW_RADIUS_MAX at paint");
        assert_eq!(effects::CARD_SHADOW_DY, 24);
        assert_eq!(effects::BACKDROP_ALPHA, 184, ".72");
    }

    use super::*;
    use crate::theme::{describe, DAYLIGHT};
    extern crate std;
    use std::string::ToString;

    /// The record: the kit's round-2 package, read off the docs tree at test
    /// time so a drift between the record and the compiled floor is caught
    /// here rather than trusted.
    fn docs(rel: &str) -> std::string::String {
        let p = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../../docs/halcyon-carbon-handoff")
            .join(rel);
        std::fs::read_to_string(&p).unwrap_or_else(|e| panic!("{}: {e}", p.display()))
    }

    /// `{theme: [16 hex]}` out of an ansi16.json, without a JSON crate: the
    /// file is one object of arrays of quoted `#RRGGBB`, and the walk below
    /// is exact for that shape (a control reads Carbon's slot 0 back).
    fn ansi_json(rel: &str) -> std::collections::BTreeMap<std::string::String, [Argb; 16]> {
        let text = docs(rel);
        let mut out = std::collections::BTreeMap::new();
        let mut rest = text.as_str();
        while let Some(q) = rest.find('"') {
            let after = &rest[q + 1..];
            let end = after.find('"').unwrap();
            let key = &after[..end];
            rest = &after[end + 1..];
            if key.starts_with('#') {
                continue;
            }
            let open = rest.find('[').unwrap();
            let close = rest.find(']').unwrap();
            let body = &rest[open + 1..close];
            let mut slots = [0u32; 16];
            let mut n = 0;
            for piece in body.split(',') {
                let h = piece.trim().trim_matches('"');
                slots[n] = theme::parse_colour(h).unwrap();
                n += 1;
            }
            assert_eq!(n, 16, "{key}");
            out.insert(key.to_string(), slots);
            rest = &rest[close + 1..];
        }
        assert_eq!(out["carbon"][0], 0xFF66_6D68, "the reader's control");
        out
    }

    /// An Instrument theme file assembled from a round-2 sidecar (the 35
    /// roles + meta) and an ANSI table.
    fn instrument_file(sidecar: &str, ansi: &[Argb; 16], smooth: u16) -> std::string::String {
        let mut s = std::string::String::from(sidecar);
        s.push_str("\n[terminal]\nansi = [");
        for (i, c) in ansi.iter().enumerate() {
            if i > 0 {
                s.push_str(", ");
            }
            let _ = write!(s, "\"#{:06X}\"", c & 0x00FF_FFFF);
        }
        s.push_str("]\n\n[type]\nsmooth = ");
        let _ = write!(s, "{smooth}\n");
        s
    }

    const IDS: [&str; 13] = [
        "signal", "carbon", "abyssal", "oxide", "combine", "deusex", "shock", "sin", "mesa",
        "strogg", "genera", "mineral", "logic",
    ];
    const LIGHT: [&str; 3] = ["genera", "mineral", "logic"];

    // The compiled floor IS the record: every one of Carbon's 35 + 16 values
    // read back from the round-2 sidecar and the adopted ANSI file.
    /// 7.3's derived opaques against the kit's own figure: Carbon's
    /// `open_header` is `#151819` (the CSS's `text` at 1.5 % over `open`,
    /// as Chromium rendered it on the golden: rows 70..101 of the first
    /// pane read (21, 24, 25)). The alpha bytes are 4 / 8 / 38 of 256; the
    /// other three are worked by hand from the same lerp.
    #[test]
    fn carbon_derives_the_kits_opaques() {
        let d = Derived::of(&CARBON);
        assert_eq!(d.open_header, 0xFF15_1819);
        assert_eq!(d.selection, 0xFF2C_2D27, "amber at 15 % over open");
        assert_eq!(d.focus_inset_header, 0xFF0F_1112, "text at 3 % over header");
        assert_eq!(d.focus_inset_open, 0xFF19_1B1C, "text at 3 % over open");
        // The golden's swatch ring (1440 x 900 at 100 %, row 13 at x 1154):
        // #CDC199 -- 12 % of 256 rounds to 31, and 30 lands one short in B.
        assert_eq!(d.swatch_ring, 0xFFCD_C199, "white at 12 % over amber");
        assert_eq!(pct256(120), 31);
        assert_eq!(pct256(15), 4);
        assert_eq!(pct256(30), 8);
        assert_eq!(pct256(150), 38);
        assert_eq!(over(0xFF10_2030, 0xFFFF_FFFF, 0), 0xFF10_2030);
        assert_eq!(over(0xFF10_2030, 0xFFFF_FFFF, 255), 0xFFFF_FFFF);
        let b = Bundle::builtin(Profile::Instrument);
        assert_eq!(b.at(100).derived, d, "the visual carries them");
    }

    #[test]
    fn carbon_matches_the_record() {
        let ansi = ansi_json("../../tools/halcyon/ansi16.json");
        let file = instrument_file(&docs("round2/ui-palettes/carbon.toml"), &ansi["carbon"], 0);
        let entries = crate::toml::parse(&file).expect("the sidecar parses");
        let l = from_entries(&entries, file.len()).unwrap_or_else(|e| panic!("{}", describe(&e)));
        assert_eq!(l.theme, CARBON, "the compiled Carbon differs from the record");
        assert_eq!(l.id, "carbon");
        assert_eq!(l.name, "Carbon Optics");
        assert!(!l.theme.light);
    }

    // THE PROJECTION IS THE KIT'S, BYTE FOR BYTE: for all 13, the FIRST
    // kit's sidecar (+ the ANSI its stock file carries, which the projection
    // passes through) projected to the legacy schema equals that kit's own
    // stock file, which its build script derived in one pass with the same
    // mapping. The first kit is the fixture because it is self-consistent;
    // round 2's is not (next test).
    #[test]
    fn every_kit_sidecar_projects_to_its_stock_twin_exactly() {
        for id in IDS {
            let smooth = if LIGHT.contains(&id) { 12 } else { 0 };
            let stock = Theme::from_toml(&docs(&std::format!("palettes/{id}.toml")))
                .unwrap_or_else(|e| panic!("{id} stock: {}", describe(&e)))
                .theme;
            let file = instrument_file(&docs(&std::format!("ui-palettes/{id}.toml")), &stock.terminal.ansi, smooth);
            let any = theme::load(&file).unwrap_or_else(|e| panic!("{id}: {}", describe(&e)));
            let LoadedAny::Instrument(l) = any else { panic!("{id}: dispatched to the wrong schema") };
            assert_eq!(l.id, id);
            assert_eq!(l.theme.light, LIGHT.contains(&id), "{id}: color_scheme");
            assert_eq!(project_legacy(&l.theme), stock, "{id}: project_legacy != the kit's stock file");
        }
    }

    // ROUND 2'S STOCK FILES ARE NOT A FAITHFUL PROJECTION OF ITS SIDECARS,
    // and this pins exactly how: Astra's `build_palettes.py` rewrote
    // `fg_subtle`, the two `fg_muted`s and the syntax slots from the amended
    // `dim`, but not `status_idle`, which the mapping also derives from `dim`
    // -- so on the 12 themes whose `dim` changed, the stock file's
    // `status_idle` is the FIRST kit's value. Nothing installs those files
    // (the loader projects from the sidecar, which is right), but the record
    // must be known for what it is. Carbon, unchanged by round 2, agrees.
    #[test]
    fn round2_stock_files_lag_their_sidecars_in_exactly_status_idle() {
        let record = ansi_json("round2/ansi16.json");
        for id in IDS {
            let smooth = if LIGHT.contains(&id) { 12 } else { 0 };
            let file = instrument_file(&docs(&std::format!("round2/ui-palettes/{id}.toml")), &record[id], smooth);
            let LoadedAny::Instrument(l) = theme::load(&file).unwrap() else { panic!("{id}") };
            let stock = Theme::from_toml(&docs(&std::format!("round2/palettes/{id}.toml"))).unwrap().theme;
            let mut projected = project_legacy(&l.theme);
            if id == "carbon" {
                assert_eq!(projected, stock, "carbon is unchanged by round 2");
                continue;
            }
            let old_dim = Theme::from_toml(&docs(&std::format!("palettes/{id}.toml"))).unwrap().theme.fg_subtle;
            assert_eq!(stock.status_idle, old_dim, "{id}: the lag is the first kit's dim");
            assert_ne!(projected.status_idle, stock.status_idle, "{id}: the discrepancy must exist");
            projected.status_idle = stock.status_idle;
            assert_eq!(projected, stock, "{id}: nothing else differs");
        }
    }

    // A file in the other schema is dispatched by `[meta] profile`: the
    // legacy loader refuses an Instrument file at that line, and `load`
    // routes it.
    #[test]
    fn the_dispatcher_routes_on_the_profile_word_and_the_legacy_loader_refuses_it() {
        let ansi = ansi_json("../../tools/halcyon/ansi16.json");
        let file = instrument_file(&docs("round2/ui-palettes/carbon.toml"), &ansi["carbon"], 0);
        match Theme::from_toml(&file) {
            Err(LoadError::UnknownKey { line }) => assert!(line <= 8, "refused at its meta, line {line}"),
            other => panic!("the legacy loader must refuse an Instrument file: {other:?}"),
        }
        assert!(matches!(theme::load(&file), Ok(LoadedAny::Instrument(_))));
        let nightjar = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../halcyon/themes/nightjar.toml"),
        )
        .unwrap();
        assert!(matches!(theme::load(&nightjar), Ok(LoadedAny::Legacy(_))));
        let bad = file.replacen("instrument-v1", "instrument-v2", 1);
        assert!(matches!(theme::load(&bad), Err(LoadError::UnknownProfile { .. })));
    }

    // Every key required, none inherited, every miss NAMED -- meta included.
    #[test]
    fn a_partial_file_is_refused_and_names_every_missing_key() {
        let file = "[meta]\nschema = 1\nprofile = \"instrument-v1\"\n[color]\ndesktop = \"#000000\"\n";
        match theme::load(file) {
            Err(LoadError::Incomplete { missing }) => {
                assert_eq!(missing.len(), 3 + 34 + 2, "{missing:?}");
                assert!(missing.contains(&"meta.id".to_string()));
                assert!(missing.contains(&"terminal.ansi".to_string()));
                assert!(!missing.contains(&"color.desktop".to_string()));
            }
            other => panic!("{other:?}"),
        }
    }

    #[test]
    fn the_meta_is_strict() {
        let ansi = ansi_json("../../tools/halcyon/ansi16.json");
        let good = instrument_file(&docs("round2/ui-palettes/carbon.toml"), &ansi["carbon"], 0);
        assert!(theme::load(&good).is_ok(), "the control");
        let cases: [(&str, &str, fn(&LoadError) -> bool); 7] = [
            ("schema = 1", "schema = 2", |e| matches!(e, LoadError::OutOfRange { .. })),
            ("id = \"carbon\"", "id = \"../carbon\"", |e| matches!(e, LoadError::BadId { .. })),
            ("id = \"carbon\"", "id = \"Carbon\"", |e| matches!(e, LoadError::BadId { .. })),
            ("color_scheme = \"dark\"", "color_scheme = \"dusk\"", |e| matches!(e, LoadError::OutOfRange { .. })),
            ("name = \"Carbon Optics\"", "name = \"Carbon\u{1b}[31m\"", |e| matches!(e, LoadError::BadName { .. })),
            ("schema = 1", "schema = 1\nbase = \"daylight\"", |e| matches!(e, LoadError::UnknownKey { .. })),
            ("[type]", "[geometry]\nbevel = 2\n[type]", |e| matches!(e, LoadError::UnknownTable { .. })),
        ];
        for (from, to, ok) in cases {
            let bad = good.replacen(from, to, 1);
            assert_ne!(bad, good, "the mutation must mutate: {from}");
            match theme::load(&bad) {
                Err(e) => assert!(ok(&e), "{from} -> {to}: wrong refusal {e:?}"),
                Ok(_) => panic!("{from} -> {to}: accepted"),
            }
        }
    }

    // The slot rule: sixteen exactly, all distinct; bright white MAY equal
    // the terminal text but nothing else may repeat.
    #[test]
    fn the_ansi_slot_rule_is_enforced() {
        let ansi = ansi_json("../../tools/halcyon/ansi16.json");
        let carbon = docs("round2/ui-palettes/carbon.toml");
        let mut dup = ansi["carbon"];
        dup[3] = dup[1];
        assert!(matches!(
            theme::load(&instrument_file(&carbon, &dup, 0)),
            Err(LoadError::AnsiNotDistinct { .. })
        ));
        let mut alias = ansi["carbon"];
        alias[15] = CARBON.terminal_text;
        assert!(theme::load(&instrument_file(&carbon, &alias, 0)).is_ok(), "the permitted alias");
        let seventeen = instrument_file(&carbon, &ansi["carbon"], 0).replacen("\"#F2F3EF\"]", "\"#F2F3EF\", \"#000001\"]", 1);
        assert!(matches!(theme::load(&seventeen), Err(LoadError::BadShape { .. })));
        let oversized = instrument_file(&carbon, &ansi["carbon"], 0) + &"#".repeat(INSTRUMENT_MAX);
        assert!(matches!(theme::load(&oversized), Err(LoadError::Syntax(_))));
    }

    #[test]
    fn the_registry_covers_every_field() {
        let InstrumentTheme {
            desktop,
            pane,
            open,
            header,
            hover,
            text,
            secondary,
            dim,
            structure,
            separator,
            amber,
            amber_muted,
            error,
            success,
            terminal_path,
            rail,
            pane_border,
            focus_neutral,
            body_text,
            code_text,
            code_bg,
            code_body,
            terminal_bg,
            terminal_text,
            dialog_bg,
            kbd_bg,
            syntax_keyword,
            syntax_type,
            syntax_function,
            syntax_string,
            syntax_number,
            syntax_attribute,
            syntax_lifetime,
            syntax_comment,
            syntax_punctuation,
            ansi,
            smooth_mem,
            light,
        } = builtin();
        // If this stopped compiling: a field was added. Add it to KEYS and to
        // `set_key` (both directions), extend this list, update the count.
        let _ = (
            desktop, pane, open, header, hover, text, secondary, dim, structure, separator, amber,
            amber_muted, error, success, terminal_path, rail, pane_border, focus_neutral, body_text,
            code_text, code_bg, code_body, terminal_bg, terminal_text, dialog_bg, kbd_bg,
            syntax_keyword, syntax_type, syntax_function, syntax_string, syntax_number,
            syntax_attribute, syntax_lifetime, syntax_comment, syntax_punctuation, ansi, smooth_mem,
            light,
        );
        assert_eq!(KEYS.len(), 37, "35 colours + ansi + smooth");
        assert_eq!(core::mem::size_of::<InstrumentTheme>(), 208, "35 x 4 + 16 x 4 + 2 + 1, padded");
        // Both directions: every registered key sets, every settable key is
        // registered -- a probe per row, and the unregistered probe refused.
        for (table, key) in KEYS {
            let mut t = builtin();
            let v = if *key == "ansi" {
                Value::Array(alloc::vec!["#000001", "#000002", "#000003", "#000004", "#000005", "#000006", "#000007", "#000008", "#000009", "#00000A", "#00000B", "#00000C", "#00000D", "#00000E", "#00000F", "#000010"])
            } else if *table == "type" {
                Value::Int(7)
            } else {
                Value::Str("#123456")
            };
            assert_eq!(set_key(&mut t, table, key, &v, 1), Ok(true), "{table}.{key} not settable");
        }
        let mut t = builtin();
        assert_eq!(set_key(&mut t, "color", "ember", &Value::Str("#123456"), 1), Ok(false));
        assert_eq!(set_key(&mut t, "geometry", "bevel", &Value::Int(2), 1), Ok(false));
    }

    // The reverse projection is total and reads the polarity off the ground:
    // Daylight is light, Nightjar is dark; and a projected theme re-projects
    // to a loadable, self-consistent bundle either way.
    #[test]
    fn project_instrument_is_total_and_reads_the_polarity() {
        let d = project_instrument(&DAYLIGHT);
        assert!(d.light, "Daylight is a light theme");
        assert_eq!(d.desktop, DAYLIGHT.floor);
        assert_eq!(d.ansi, DAYLIGHT.terminal.ansi);
        assert_eq!(d.smooth_mem, 12);
        let nightjar = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../halcyon/themes/nightjar.toml"),
        )
        .unwrap();
        let n = Theme::from_toml(&nightjar).unwrap().theme;
        assert!(!project_instrument(&n).light, "Nightjar is dark");
        // Carbon through the legacy side and back keeps every role that has
        // a legacy home; the ones that do not (lifetime, punctuation, the
        // neutral frame, separator vs structure) are the labelled losses.
        let back = project_instrument(&project_legacy(&CARBON));
        assert_eq!(back.desktop, CARBON.desktop);
        assert_eq!(back.text, CARBON.text);
        assert_eq!(back.amber, CARBON.amber);
        assert_eq!(back.ansi, CARBON.ansi);
        assert_ne!(back, CARBON, "the projections are approximations by construction");
    }

    #[test]
    fn a_bundle_fills_the_other_side_by_projection() {
        let b = Bundle::from_instrument(Profile::Legacy, CARBON);
        assert_eq!(b.theme, project_legacy(&CARBON));
        assert_eq!(b.inst, CARBON);
        let b = Bundle::from_legacy(Profile::Instrument, DAYLIGHT);
        assert_eq!(b.inst, project_instrument(&DAYLIGHT));
        assert_eq!(b.theme, DAYLIGHT);
        assert_eq!(Bundle::builtin(Profile::Legacy).theme, DAYLIGHT);
        assert_eq!(Bundle::builtin(Profile::Instrument).inst, CARBON);
        // At a scale the PROFILE picks the table (HALCYON-INSTRUMENT 4.4 /
        // 5.7): `INSTRUMENT_BASE` under `instrument` whatever the theme's
        // own `[geometry]` says, the theme's under `legacy`.
        let v = Bundle::builtin(Profile::Instrument).at(200);
        assert_eq!(v.metrics, INSTRUMENT_BASE.at(200));
        assert_ne!(v.metrics, LEGACY_PROJECTED_METRICS.at(200), "the projected legacy table is not the carve's");
        assert_eq!(Bundle::builtin(Profile::Legacy).at(100).metrics, DAYLIGHT.metrics);
        // The same THEME under the other profile: only the table moves.
        let li = Bundle::from_legacy(Profile::Instrument, DAYLIGHT).at(150);
        assert_eq!(li.metrics, INSTRUMENT_BASE.at(150));
        assert_eq!(li.theme, DAYLIGHT);
        let il = Bundle::from_instrument(Profile::Legacy, CARBON).at(150);
        assert_eq!(il.metrics, LEGACY_PROJECTED_METRICS.at(150), "an Instrument theme under legacy carves the kit's projected table");
    }

    /// HALCYON-INSTRUMENT 5.1 / 5.7: the Instrument table is the scripture's
    /// numbers, the identity at 100 (its absent legacy marks stay absent --
    /// no floor lifts a 0 bevel to 2), and scales by the one rule with its
    /// own floors above it.
    #[test]
    fn the_instrument_table_is_the_scripture_and_the_identity_at_100() {
        let b = INSTRUMENT_BASE;
        assert_eq!(
            (b.rail_h, b.status_h, b.outer_pad, b.track, b.rule, b.rule_off, b.joint, b.frame),
            (34, 25, 3, 7, 2, 2, 7, 1)
        );
        assert_eq!(
            (b.header_h, b.index_w, b.header_gap, b.action_w, b.mark_w, b.mark_inset_y, b.hairline),
            (32, 32, 7, 28, 2, 6, 1)
        );
        assert_eq!((b.bevel, b.gap, b.tab_strip_h, b.tag_pad_x), (0, 0, 0, 0), "absent under Instrument");
        assert_eq!((b.min_pane_w, b.min_body_h), (260, 54), "the minima ride the table (5.2)");
        assert_eq!((b.at(200).min_pane_w, b.at(200).min_body_h), (520, 108));
        assert_eq!(b.at(100), b, "at(100) is the table exactly");
        let m125 = b.at(125);
        assert_eq!((m125.rail_h, m125.status_h, m125.outer_pad, m125.track), (43, 31, 4, 9), "42.5 up, 31.25 down, 3.75 up, 8.75 up");
        assert_eq!((m125.rule, m125.rule_off, m125.joint, m125.frame), (3, 3, 9, 1), "a 2 px rule is 3 at 125 (the kit's own example)");
        assert_eq!((m125.header_h, m125.index_w, m125.header_gap, m125.action_w, m125.mark_w, m125.mark_inset_y), (40, 40, 9, 35, 3, 8));
        assert_eq!((m125.bevel, m125.gap, m125.tab_strip_h), (0, 0, 0), "absent stays absent");
        let m200 = b.at(200);
        assert_eq!((m200.rail_h, m200.status_h, m200.outer_pad, m200.track, m200.rule, m200.joint, m200.frame, m200.header_h), (68, 50, 6, 14, 4, 14, 2, 64));
        assert_eq!((m200.hairline, m200.bevel, m200.gap), (2, 0, 0));
        let m150 = b.at(150);
        assert_eq!((m150.rail_h, m150.status_h, m150.track, m150.rule, m150.joint, m150.frame, m150.header_h), (51, 38, 11, 3, 11, 2, 48), "37.5 up; 10.5 up; 1.5 up");
        let m175 = b.at(175);
        assert_eq!((m175.rail_h, m175.status_h, m175.track, m175.rule, m175.joint, m175.frame, m175.header_h), (60, 44, 12, 4, 12, 2, 56), "59.5 up; 43.75 up; 12.25 down; 3.5 up; 1.75 up");
        // The floors bite below 100 (not a v1 scale; the function is total).
        let m25 = b.at(25);
        assert_eq!((m25.rule, m25.joint, m25.frame, m25.mark_w, m25.hairline), (2, 3, 1, 2, 1));
        assert_eq!(m25.bevel, 0, "an absent mark is never floored into existence");
    }

    #[test]
    fn the_profile_word_parses_strictly() {
        assert_eq!(Profile::parse("legacy"), Some(Profile::Legacy));
        assert_eq!(Profile::parse("instrument\n"), Some(Profile::Instrument));
        for bad in ["Instrument", "", "\n", "instrument\n\n", " legacy", "legacy instrument", "instrument-v1"] {
            assert_eq!(Profile::parse(bad), None, "{bad:?}");
        }
        assert_eq!(Profile::parse(Profile::Instrument.word()), Some(Profile::Instrument));
    }

    /// I-7 (4.2 amended): the three optional meta keys load when present
    /// and default when absent; a malformed one refuses the file whole.
    #[test]
    fn the_optional_meta_keys_load_validate_and_default() {
        let base = docs("round2/ui-palettes/carbon.toml");
        let ansi = ansi_json("round2/ansi16.json");
        let file = instrument_file(&base, &ansi["carbon"], 0);
        let plain = theme::load(&file).unwrap();
        let LoadedAny::Instrument(l) = plain else { panic!("schema") };
        assert_eq!(l.group, None, "absent group");
        assert_eq!(l.tagline, "", "absent tagline");
        assert_eq!(l.rank, RANK_UNRANKED, "absent rank");
        let with = file.replacen(
            "[meta]\n",
            "[meta]\ngroup = \"terminal\"\ntagline = \"High contrast · pale champagne\"\nrank = 3\n",
            1,
        );
        let LoadedAny::Instrument(l) = theme::load(&with).unwrap() else { panic!("schema") };
        assert_eq!(l.group, Some(Group::Terminal));
        assert_eq!(l.tagline, "High contrast · pale champagne");
        assert_eq!(l.rank, 3);
        assert_eq!(l.id, "carbon", "the required keys still load");
        for (bad, why) in [
            ("group = \"dusk\"\n", "an unknown group word"),
            ("group = 1\n", "a group that is not a string"),
            ("rank = 256\n", "a rank past u8"),
            ("rank = -1\n", "a negative rank"),
            ("rank = \"1\"\n", "a rank that is not an integer"),
            ("tagline = \"a\u{1b}[31mb\"\n", "a tagline with a control byte"),
        ] {
            let text = file.replacen("[meta]\n", &std::format!("[meta]\n{bad}"), 1);
            assert!(theme::load(&text).is_err(), "{why} must refuse the file");
        }
        assert_eq!(Group::parse("dark"), Some(Group::Dark));
        assert_eq!(Group::parse("light"), Some(Group::Light));
        assert_eq!(Group::Dark.label(), "DARK FIELD");
        assert_eq!(Group::Terminal.label(), "TERMINAL STUDIES");
        assert_eq!(Group::Light.label(), "LIGHT FIELD");
        assert!(Group::Dark < Group::Terminal && Group::Terminal < Group::Light, "the mockup's order");
    }

    #[test]
    fn a_gallery_id_is_a_path_only_when_it_is_an_id() {
        for ok in ["carbon", "a", "deus-ex_2", &"z".repeat(32)] {
            assert!(is_gallery_id(ok), "{ok}");
            assert_eq!(gallery_path(ok).unwrap(), std::format!("/lib/halcyon/themes/{ok}.toml"));
        }
        for bad in ["", ".", "..", "../carbon", "carbon/x", "Carbon", "1st", "-x", "carbon.toml", "a b", &"z".repeat(33), "carbon\n"] {
            assert!(!is_gallery_id(bad), "{bad:?}");
            assert_eq!(gallery_path(bad), None, "{bad:?}");
        }
    }

    // 4.1: the tiers, in order, each refusal one step down and loud; a
    // theme in the other schema projected, never refused; a pick word that
    // is not an id refused even when a file is supplied for it.
    #[test]
    fn the_bundle_resolves_by_tier_and_falls_one_step_on_a_refusal() {
        let ansi = ansi_json("../../tools/halcyon/ansi16.json");
        let carbon = instrument_file(&docs("round2/ui-palettes/carbon.toml"), &ansi["carbon"], 0);
        let mesa = instrument_file(&docs("round2/ui-palettes/mesa.toml"), &ansi["mesa"], 0);
        let nightjar = std::fs::read_to_string(
            std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../halcyon/themes/nightjar.toml"),
        )
        .unwrap();
        // Nothing at all: legacy + Daylight, silently.
        let r = resolve_bundle(Sources::default());
        assert_eq!((r.bundle.profile, r.profile_tier, r.theme_tier), (Profile::Legacy, Tier::BuiltIn, Tier::BuiltIn));
        assert_eq!(r.bundle.theme, DAYLIGHT);
        assert!(r.notes.is_empty());
        // The system says instrument; the user's file is Nightjar: the
        // profile is instrument, the theme native legacy, projected across.
        let r = resolve_bundle(Sources { system_profile: Some("instrument\n"), user_file: Some(&nightjar), ..Default::default() });
        assert_eq!((r.bundle.profile, r.profile_tier, r.theme_tier, r.schema), (Profile::Instrument, Tier::System, Tier::User, Schema::Legacy));
        assert_eq!(r.bundle.inst, project_instrument(&r.bundle.theme));
        assert_eq!(r.name, "Nightjar");
        // The pick wins over the user file; the user's profile word wins
        // over the system's.
        let r = resolve_bundle(Sources {
            system_profile: Some("instrument"),
            user_profile: Some("legacy"),
            user_pick: Some("mesa\n"),
            pick_file: Some(&mesa),
            user_file: Some(&nightjar),
            system_file: Some(&carbon),
        });
        assert_eq!((r.bundle.profile, r.profile_tier, r.theme_tier, r.schema), (Profile::Legacy, Tier::User, Tier::Pick, Schema::Instrument));
        assert_eq!(r.id, "mesa");
        assert_eq!(r.bundle.theme, project_legacy(&r.bundle.inst));
        // A refused pick falls to the user file, loudly; a bad profile word
        // falls to the next tier, loudly.
        let broken = carbon.replacen("#050607", "#05060", 1);
        let r = resolve_bundle(Sources {
            system_profile: Some("instrumnet"),
            user_pick: Some("carbon"),
            pick_file: Some(&broken),
            user_file: Some(&nightjar),
            ..Default::default()
        });
        assert_eq!((r.bundle.profile, r.profile_tier, r.theme_tier), (Profile::Legacy, Tier::BuiltIn, Tier::User));
        assert_eq!(r.notes.len(), 2, "{:?}", r.notes);
        assert!(r.notes[0].contains("profile REFUSED"));
        assert!(r.notes[1].contains("picked gallery theme REFUSED") && r.notes[1].contains("line"), "{}", r.notes[1]);
        // A pick word that is not an id is refused BEFORE its file is read.
        let r = resolve_bundle(Sources { user_pick: Some("../carbon"), pick_file: Some(&carbon), ..Default::default() });
        assert_eq!(r.theme_tier, Tier::BuiltIn);
        assert!(r.notes[0].contains("not a gallery id"));
        // A valid pick with no file behind it is said, not swallowed.
        let r = resolve_bundle(Sources { user_pick: Some("mesa\n"), ..Default::default() });
        assert_eq!(r.theme_tier, Tier::BuiltIn);
        assert_eq!(r.notes, alloc::vec![String::from("theme: user pick `mesa` names no gallery file")]);
        // ONE normalisation: what `pick_id` accepts is what the resolution
        // accepts, newline for newline.
        for (w, ok) in [("mesa", true), ("mesa\n", true), ("mesa\n\n", false), ("mesa \n", false), ("Mesa\n", false)] {
            assert_eq!(pick_id(w).is_some(), ok, "{w:?}");
            let r = resolve_bundle(Sources { user_pick: Some(w), pick_file: Some(&mesa), ..Default::default() });
            assert_eq!(r.theme_tier == Tier::Pick, ok, "{w:?}");
        }
        // Every tier refused: the profile's floor, both tiers named.
        let r = resolve_bundle(Sources { system_profile: Some("instrument"), user_file: Some(&broken), system_file: Some("[palette]\n"), ..Default::default() });
        assert_eq!((r.bundle.profile, r.theme_tier), (Profile::Instrument, Tier::BuiltIn));
        assert_eq!(r.bundle.inst, CARBON);
        assert_eq!(r.notes.len(), 2);
    }
}
