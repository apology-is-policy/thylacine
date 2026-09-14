// halcyon -- the pure half of the Halcyon session tool (H-4).
//
// Argument dispatch, layout-name validation, and session-tier path building --
// no I/O, no libthyla-rs, no libhalcyon. The binary (src/main.rs) wires these
// to the /dev/tapestry walk and the durable write. Split off so the decision
// logic is host-testable (the bin can only run on aarch64-unknown-none).
//
// `#![cfg_attr(not(test), no_std)]`: no_std for the device build, std under
// `cargo test` so the logic runs on the host (nora's precedent):
//   cargo test -p halcyon --no-default-features --lib --target <host-triple>

#![cfg_attr(not(test), no_std)]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;

/// A layout name's maximum length (a filename in the user's own namespace; the
/// real names are short -- "work", "coding", "default").
pub const MAX_NAME_LEN: usize = 64;

/// A parsed `halcyon` command line (argv[0] excluded). The name is borrowed
/// from the caller's token slice -- no allocation here.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Cmd<'a> {
    /// `halcyon layout save <name>` -- serialize the live pane tree.
    LayoutSave { name: &'a str },
    /// `halcyon layout restore <name>` -- rebuild a saved tree (H-4b).
    LayoutRestore { name: &'a str },
    /// `halcyon layout list` -- every saved layout, both tiers (H-4c).
    LayoutList,
    /// `halcyon layout delete <name>` -- remove a session-tier layout (H-4c).
    LayoutDelete { name: &'a str },
    /// `halcyon welcome` -- the first-launch tour, then the user's shell
    /// (the device `default` layout's left tile, H-4d).
    Welcome,
    /// `halcyon theme lint [<path>]` -- check a theme file (TH-4c). With no
    /// path, the two tiers the session actually resolves.
    ThemeLint { path: Option<&'a str> },
    /// `halcyon`, `halcyon help`, `--help`, `-h`.
    Help,
}

/// Why a command line was rejected. Each maps to one diagnostic line in the
/// binary; kept a plain enum so the dispatch is exhaustively host-testable.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CmdError {
    /// The first token was not a known subcommand.
    UnknownCommand,
    /// `layout` with no verb, or a verb that is not save/restore/list/delete.
    BadLayoutVerb,
    /// `layout save|restore|delete` with no name operand.
    MissingName,
    /// A trailing operand after the name.
    ExtraOperand,
    /// The name is not a safe single path component.
    BadName,
    /// `theme` with no verb, or a verb that is not `lint`.
    BadThemeVerb,
    /// The theme-file operand is not usable as a path.
    BadPath,
}

/// Parse argv[1..] (`tokens`) into a [`Cmd`]. Pure: the name is borrowed from
/// `tokens`, nothing is read or allocated.
pub fn parse_cmd<'a>(tokens: &[&'a str]) -> Result<Cmd<'a>, CmdError> {
    match tokens.first().copied() {
        None | Some("help") | Some("--help") | Some("-h") => Ok(Cmd::Help),
        Some("layout") => parse_layout(&tokens[1..]),
        Some("theme") => parse_theme(&tokens[1..]),
        Some("welcome") => {
            if tokens.len() > 1 {
                Err(CmdError::ExtraOperand)
            } else {
                Ok(Cmd::Welcome)
            }
        }
        Some(_) => Err(CmdError::UnknownCommand),
    }
}

fn parse_layout<'a>(rest: &[&'a str]) -> Result<Cmd<'a>, CmdError> {
    let verb = rest.first().copied().ok_or(CmdError::BadLayoutVerb)?;
    if verb == "list" {
        if rest.len() > 1 {
            return Err(CmdError::ExtraOperand);
        }
        return Ok(Cmd::LayoutList);
    }
    if verb != "save" && verb != "restore" && verb != "delete" {
        return Err(CmdError::BadLayoutVerb);
    }
    let name = *rest.get(1).ok_or(CmdError::MissingName)?;
    if rest.len() > 2 {
        return Err(CmdError::ExtraOperand);
    }
    if !name_is_valid(name) {
        return Err(CmdError::BadName);
    }
    Ok(match verb {
        "save" => Cmd::LayoutSave { name },
        "restore" => Cmd::LayoutRestore { name },
        _ => Cmd::LayoutDelete { name },
    })
}

fn parse_theme<'a>(rest: &[&'a str]) -> Result<Cmd<'a>, CmdError> {
    let verb = rest.first().copied().ok_or(CmdError::BadThemeVerb)?;
    if verb != "lint" {
        return Err(CmdError::BadThemeVerb);
    }
    if rest.len() > 2 {
        return Err(CmdError::ExtraOperand);
    }
    match rest.get(1).copied() {
        None => Ok(Cmd::ThemeLint { path: None }),
        Some(p) if path_is_usable(p) => Ok(Cmd::ThemeLint { path: Some(p) }),
        Some(_) => Err(CmdError::BadPath),
    }
}

/// Is a theme-file operand usable as a path? Unlike a layout name this IS a
/// path, so `/` is fine and no traversal check applies -- the file is read as
/// the user, who may read any file they own. Refused: empty; option-shaped
/// (`halcyon theme lint --help` is a mistyped help request, and reading a file
/// literally called `--help` is never what was meant); and any control byte,
/// which could only ever mangle the report's own lines.
pub fn path_is_usable(p: &str) -> bool {
    !p.is_empty() && !p.starts_with('-') && !p.bytes().any(|b| b < 0x20 || b == 0x7F)
}

/// A layout name is a single safe path component: non-empty, <= MAX_NAME_LEN,
/// no leading `.` (so `.`, `..`, and hidden names are all out), no leading `-`
/// (a name is never mistaken for an option: `halcyon layout restore -h` cannot
/// name a layout, and a verb menu's `{}` needs no `--`), not the save's
/// temporary suffix, and drawn only from `[A-Za-z0-9._-]` (so no `/`
/// traversal and no whitespace/control). The name lands in the user's OWN
/// namespace, but a conservative charset keeps a saved layout a predictable
/// filename and closes traversal by construction.
pub fn name_is_valid(name: &str) -> bool {
    if name.is_empty()
        || name.len() > MAX_NAME_LEN
        || name.starts_with('.')
        || name.starts_with('-')
        || name.ends_with(SAVE_TMP_SUFFIX)
    {
        return false;
    }
    name.bytes()
        .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_' || b == b'.')
}

/// The suffix of a save's temporary file (`<name>.tmp`, renamed over the
/// layout once its content is durable). A crash between the write and the
/// rename leaves one behind; `list` never shows it and no name may end in it.
pub const SAVE_TMP_SUFFIX: &str = ".tmp";

/// Which tier a listed layout was found in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum LayoutTier {
    /// `$home/lib/halcyon/layouts` -- the user's own, writable.
    Session,
    /// `/lib/halcyon/layouts` -- the image's, read-only to the session tool.
    Device,
}

impl LayoutTier {
    pub fn as_str(self) -> &'static str {
        match self {
            LayoutTier::Session => "session",
            LayoutTier::Device => "device",
        }
    }
}

/// One row of `halcyon layout list`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LayoutRow {
    pub name: String,
    pub tier: LayoutTier,
    /// A device-tier layout a session-tier one of the same name hides: a
    /// restore of that name takes the session one (13.7's order).
    pub shadowed: bool,
}

/// The list rows from the two directories' entries: invalid names (a save's
/// `.tmp`, a stray dotfile) dropped, sorted by name with a session row before
/// the device row it shadows.
pub fn list_rows(session: &[String], device: &[String]) -> Vec<LayoutRow> {
    let mut rows: Vec<LayoutRow> = Vec::new();
    for n in session {
        if name_is_valid(n) {
            rows.push(LayoutRow {
                name: n.clone(),
                tier: LayoutTier::Session,
                shadowed: false,
            });
        }
    }
    for n in device {
        if name_is_valid(n) {
            let shadowed = session.iter().any(|s| s == n);
            rows.push(LayoutRow {
                name: n.clone(),
                tier: LayoutTier::Device,
                shadowed,
            });
        }
    }
    rows.sort_by(|a, b| {
        a.name
            .cmp(&b.name)
            .then((a.tier == LayoutTier::Device).cmp(&(b.tier == LayoutTier::Device)))
    });
    rows
}

/// The session-tier layouts directory: `<home>/lib/halcyon/layouts` (HALCYON.md
/// 13.7). `home` is `$HOME` (e.g. `/home/cora`); a trailing slash is trimmed.
pub fn session_layouts_dir(home: &str) -> String {
    let mut s = String::from(home.trim_end_matches('/'));
    s.push_str("/lib/halcyon/layouts");
    s
}

/// The full path of a named layout in the session tier. `name` MUST have
/// passed [`name_is_valid`], so this is a plain join (no traversal possible).
pub fn session_layout_path(home: &str, name: &str) -> String {
    let mut s = session_layouts_dir(home);
    s.push('/');
    s.push_str(name);
    s
}

/// The device-tier layouts directory (HALCYON.md 13.7): the image's shipped
/// layouts, read by a restore when the session tier has no layout of that
/// name; never written by the session tool.
pub const DEVICE_LAYOUTS_DIR: &str = "/lib/halcyon/layouts";

/// The full path of a named layout in the device tier. `name` MUST have
/// passed [`name_is_valid`].
pub fn device_layout_path(name: &str) -> String {
    let mut s = String::from(DEVICE_LAYOUTS_DIR);
    s.push('/');
    s.push_str(name);
    s
}

/// Is a tile's `pane/<id>/owner` value the ENVIRONMENT's -- one a session
/// restore must NOT respawn? `owner` is the owner-file read: `Some(principal)`
/// on success, `None` when unreadable/unparseable. It is env iff the owner is
/// NOT the caller (`me`): a real other principal (the console = SYSTEM, another
/// user), OR the INVALID/unowned principal 0 (nobody's -- a tile hosting a
/// principal-0 surface is not the session's to reconstruct), OR unreadable
/// (fail-CLOSED). Only the caller's own tiles (`owner == me`) are NOT env. An
/// EMPTY leaf owned by the session (owner == me, stamped at split) is rebuilt
/// as an empty pane; an empty leaf owned by 0/another is env and pruned, which
/// is harmless (its tag is empty, so it is never respawned anyway) -- the
/// distinction MATTERS only for an OCCUPIED tile, where fail-OPEN on owner 0
/// would respawn a principal-0 surface's command line as the user.
pub fn owner_is_env(owner: Option<u32>, me: u32) -> bool {
    match owner {
        Some(o) => o != me,
        None => true,
    }
}

/// The tag-as-command-line helpers now live in `libhalcyon::tag` (H-4d:
/// the session compositor hosts tagged leaves too, off the same
/// definitions); re-exported so the tool's callers + tests are unchanged.
pub use libhalcyon::tag::{argv_of, prog_candidates, PROG_DIRS};

/// The directory chain to `mkdir -p` (top-down, each ignoring "already
/// exists") before a session write: `<home>/lib`, `<home>/lib/halcyon`,
/// `<home>/lib/halcyon/layouts`. The kernel create is exclusive and errors on
/// a missing parent, so the order matters.
pub fn session_dir_chain(home: &str) -> Vec<String> {
    let base = home.trim_end_matches('/');
    ["/lib", "/lib/halcyon", "/lib/halcyon/layouts"]
        .iter()
        .map(|suf| {
            let mut s = String::from(base);
            s.push_str(suf);
            s
        })
        .collect()
}

// =============================================================================
// theme lint (TH-4c) -- HALCYON-THEME 4.3's promise, made readable
// =============================================================================
//
// The loader already computes everything this reports: `Theme::from_toml`
// returns the keys a based file did NOT set, and `theme::describe` renders any
// refusal as one line naming the line number. So the lint is a RENDERER, not a
// second implementation of the schema -- which is the point: a lint that
// re-derived "is this file valid" could disagree with the loader, and then the
// tool that exists to build confidence would be the thing undermining it.
//
// It deliberately does NOT try to guess whether an inherited key is a MISTAKE
// (say, a light Daylight grey inherited into a dark theme). That check would
// have to be a heuristic, and a heuristic that says OK is worse than no check
// at all. 4.3's structural answer is to omit `base`, which makes the loader
// itself name every unset key -- so the lint's job is to make the inheritance
// VISIBLE, and let the author decide.

/// One file for the lint to check.
pub struct ThemeFile<'a> {
    /// The tier's name (`system` / `user`), or empty when a path was named
    /// explicitly and there is no tier to speak of.
    pub label: &'a str,
    pub path: &'a str,
    /// The file's text; `None` = it is not there, which for a TIER is not an
    /// error (HALCYON-THEME 4.1). A path named on the command line that does
    /// not exist never reaches here -- that is an I/O failure at the caller.
    pub text: Option<&'a str>,
}

/// What `halcyon theme lint` prints, and whether it failed.
pub struct LintReport {
    pub lines: Vec<String>,
    /// A file was present and did not load -- the tool's exit status. An
    /// ABSENT tier is not a refusal.
    pub refused: bool,
}

/// Columns the inherited-key list wraps at. A console is 80 wide; the list is
/// indented four, so this leaves margin rather than filling to the edge.
const LINT_WRAP: usize = 72;

/// Render one line per file (plus the indented inherited-key list where a
/// based file left keys unset). Pure -- the contents are injected, so every
/// verdict in the report is host-testable without a filesystem.
pub fn lint_files(files: &[ThemeFile]) -> LintReport {
    use core::fmt::Write as _;
    let mut lines: Vec<String> = Vec::new();
    let mut refused = false;

    for f in files {
        let mut head = String::new();
        if !f.label.is_empty() {
            head.push_str(f.label);
            head.push(' ');
        }
        head.push_str(f.path);
        head.push_str(": ");

        let Some(text) = f.text else {
            head.push_str("absent");
            lines.push(head);
            continue;
        };

        match libhalcyon::theme::Theme::from_toml(text) {
            Err(e) => {
                refused = true;
                let _ = write!(head, "REFUSED -- {}", libhalcyon::theme::describe(&e));
                lines.push(head);
            }
            Ok(l) => {
                let name: &str = if l.name.is_empty() {
                    "(unnamed)"
                } else {
                    &l.name
                };
                let total = libhalcyon::theme::KEYS.len();
                if l.inherited.is_empty() {
                    let _ = write!(head, "OK -- \"{name}\", all {total} keys set");
                    lines.push(head);
                } else {
                    let _ = write!(
                        head,
                        "OK -- \"{name}\", base daylight, {} of {total} keys inherited",
                        l.inherited.len()
                    );
                    lines.push(head);
                    for w in wrap_tokens(&l.inherited, "    ", LINT_WRAP) {
                        lines.push(w);
                    }
                }
            }
        }
    }
    LintReport { lines, refused }
}

/// The `active:` line: which tier a renderer would actually paint from.
///
/// It calls [`libhalcyon::theme::resolve`] rather than re-deciding the tier
/// order, so the line cannot drift from what the compositor and the session
/// really do -- including the fall-ONE-tier-down rule (a user file with a typo
/// leaves the system theme in place, not the built-in).
pub fn lint_active_line(system: Option<&str>, user: Option<&str>) -> String {
    use libhalcyon::theme::Source;
    let r = libhalcyon::theme::resolve(system, user);
    let mut s = String::from("active: ");
    match r.source {
        Source::BuiltIn => {
            s.push_str("built-in (Daylight)");
            return s;
        }
        Source::System => s.push_str("system"),
        Source::User => s.push_str("user"),
    }
    s.push_str(" -- \"");
    s.push_str(if r.name.is_empty() {
        "(unnamed)"
    } else {
        &r.name
    });
    s.push('"');
    s
}

/// Wrap space-separated tokens into `indent`-prefixed lines of at most `width`
/// columns. A token longer than the width gets its own line rather than being
/// split -- a key name is an identifier, and half of one helps nobody.
fn wrap_tokens(tokens: &[String], indent: &str, width: usize) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    let mut cur = String::from(indent);
    let mut empty = true;
    for t in tokens {
        if !empty && cur.len() + 1 + t.len() > width {
            out.push(cur);
            cur = String::from(indent);
            empty = true;
        }
        if !empty {
            cur.push(' ');
        }
        cur.push_str(t);
        empty = false;
    }
    if !empty {
        out.push(cur);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn parse_save_and_restore() {
        assert_eq!(
            parse_cmd(&["layout", "save", "work"]),
            Ok(Cmd::LayoutSave { name: "work" })
        );
        assert_eq!(
            parse_cmd(&["layout", "restore", "coding"]),
            Ok(Cmd::LayoutRestore { name: "coding" })
        );
    }

    #[test]
    fn parse_list_and_delete() {
        assert_eq!(parse_cmd(&["layout", "list"]), Ok(Cmd::LayoutList));
        assert_eq!(
            parse_cmd(&["layout", "list", "extra"]),
            Err(CmdError::ExtraOperand)
        );
        assert_eq!(
            parse_cmd(&["layout", "delete", "work"]),
            Ok(Cmd::LayoutDelete { name: "work" })
        );
        assert_eq!(parse_cmd(&["layout", "delete"]), Err(CmdError::MissingName));
    }

    #[test]
    fn a_name_never_begins_with_a_dash_or_ends_in_the_tmp_suffix() {
        assert!(!name_is_valid("-h"));
        assert!(!name_is_valid("--help"));
        assert!(!name_is_valid("work.tmp"));
        assert!(name_is_valid("work-1.v2_x"));
        assert_eq!(
            parse_cmd(&["layout", "restore", "-h"]),
            Err(CmdError::BadName)
        );
    }

    #[test]
    fn list_rows_drop_temps_sort_by_name_and_mark_shadowed_device_rows() {
        let session = vec![
            String::from("work"),
            String::from("work.tmp"),
            String::from(".hidden"),
        ];
        let device = vec![String::from("default"), String::from("work")];
        let rows = list_rows(&session, &device);
        assert_eq!(
            rows,
            vec![
                LayoutRow {
                    name: String::from("default"),
                    tier: LayoutTier::Device,
                    shadowed: false
                },
                LayoutRow {
                    name: String::from("work"),
                    tier: LayoutTier::Session,
                    shadowed: false
                },
                LayoutRow {
                    name: String::from("work"),
                    tier: LayoutTier::Device,
                    shadowed: true
                },
            ]
        );
        assert!(list_rows(&[], &[]).is_empty());
    }

    #[test]
    fn parse_welcome() {
        assert_eq!(parse_cmd(&["welcome"]), Ok(Cmd::Welcome));
        assert_eq!(parse_cmd(&["welcome", "x"]), Err(CmdError::ExtraOperand));
    }

    #[test]
    fn parse_help_forms() {
        assert_eq!(parse_cmd(&[]), Ok(Cmd::Help));
        assert_eq!(parse_cmd(&["help"]), Ok(Cmd::Help));
        assert_eq!(parse_cmd(&["--help"]), Ok(Cmd::Help));
        assert_eq!(parse_cmd(&["-h"]), Ok(Cmd::Help));
    }

    #[test]
    fn parse_rejects_bad_command_lines() {
        assert_eq!(parse_cmd(&["frobnicate"]), Err(CmdError::UnknownCommand));
        assert_eq!(parse_cmd(&["layout"]), Err(CmdError::BadLayoutVerb));
        assert_eq!(
            parse_cmd(&["layout", "dance"]),
            Err(CmdError::BadLayoutVerb)
        );
        assert_eq!(parse_cmd(&["layout", "save"]), Err(CmdError::MissingName));
        assert_eq!(
            parse_cmd(&["layout", "save", "work", "extra"]),
            Err(CmdError::ExtraOperand)
        );
        assert_eq!(
            parse_cmd(&["layout", "save", "../etc/passwd"]),
            Err(CmdError::BadName)
        );
    }

    #[test]
    fn name_validation() {
        assert!(name_is_valid("work"));
        assert!(name_is_valid("coding_env-2"));
        assert!(name_is_valid("my.layout"));
        // The traversal / hidden / empty / charset rejections.
        assert!(!name_is_valid(""));
        assert!(!name_is_valid("."));
        assert!(!name_is_valid(".."));
        assert!(!name_is_valid(".hidden"));
        assert!(!name_is_valid("a/b"));
        assert!(!name_is_valid("has space"));
        assert!(!name_is_valid("tab\there"));
        assert!(!name_is_valid("null\0byte"));
        assert!(!name_is_valid(&"x".repeat(MAX_NAME_LEN + 1)));
        assert!(name_is_valid(&"x".repeat(MAX_NAME_LEN)));
    }

    #[test]
    fn session_paths() {
        assert_eq!(
            session_layouts_dir("/home/cora"),
            "/home/cora/lib/halcyon/layouts"
        );
        // A trailing slash on $HOME is trimmed (no doubled `//`).
        assert_eq!(
            session_layouts_dir("/home/cora/"),
            "/home/cora/lib/halcyon/layouts"
        );
        assert_eq!(
            session_layout_path("/home/cora", "work"),
            "/home/cora/lib/halcyon/layouts/work"
        );
    }

    #[test]
    fn device_paths() {
        assert_eq!(
            device_layout_path("default"),
            "/lib/halcyon/layouts/default"
        );
    }

    #[test]
    fn prog_candidates_mirror_the_shell() {
        // A bare name expands to the three probe dirs, /bin first.
        assert_eq!(
            prog_candidates("tapestry-demo"),
            vec![
                String::from("/bin/tapestry-demo"),
                String::from("/tapestry-demo"),
                String::from("/goroot/bin/tapestry-demo"),
            ]
        );
        // A slashed name is verbatim (one candidate).
        assert_eq!(prog_candidates("/bin/hx"), vec![String::from("/bin/hx")]);
        assert_eq!(
            prog_candidates("./local/thing"),
            vec![String::from("./local/thing")]
        );
    }

    #[test]
    fn owner_is_env_is_fail_closed_and_owner_0_is_env() {
        let me = 1000u32;
        // The session's own tile: NOT env.
        assert!(!owner_is_env(Some(1000), me));
        // Another real user, and the SYSTEM console: env.
        assert!(owner_is_env(Some(1001), me));
        assert!(owner_is_env(Some(0xFFFF_FFFE), me)); // T_PRINCIPAL_SYSTEM
                                                      // Owner 0 (INVALID / nobody): env -- the fail-OPEN arm F2 closed. An
                                                      // occupied principal-0 tile must never be respawned as the user.
        assert!(owner_is_env(Some(0), me));
        // Unreadable: fail-CLOSED (env, never respawned).
        assert!(owner_is_env(None, me));
    }

    #[test]
    fn argv_splits_a_tag_on_whitespace() {
        assert_eq!(argv_of("tapestry-demo"), vec!["tapestry-demo"]);
        assert_eq!(
            argv_of("hx  /lib/aurora/config\t-r"),
            vec!["hx", "/lib/aurora/config", "-r"]
        );
        assert!(argv_of("").is_empty());
        assert!(argv_of("   ").is_empty());
    }

    // ----- theme lint (TH-4c) ------------------------------------------------

    /// A COMPLETE theme file, built FROM the key registry rather than
    /// transcribed. Transcribing 57 keys would mean this fixture silently
    /// stops being complete the day a key is added -- and then every test
    /// below would keep passing while measuring a DIFFERENT thing (a based
    /// file's inheritance instead of a complete file's completeness). Built
    /// from `KEYS`, it cannot drift.
    fn complete_theme_toml() -> String {
        use libhalcyon::theme::KEYS;
        let mut s = String::from("[meta]\nname = \"fixture\"\n");
        let mut table = "";
        for (i, (tb, k)) in KEYS.iter().enumerate() {
            if *tb != table {
                s.push_str("\n[");
                s.push_str(tb);
                s.push_str("]\n");
                table = tb;
            }
            s.push_str(k);
            s.push_str(" = ");
            match (*tb, *k) {
                ("terminal", "ansi") => {
                    s.push('[');
                    for j in 0..16u32 {
                        if j > 0 {
                            s.push_str(", ");
                        }
                        s.push_str(&alloc::format!("\"#{:02X}20{:02X}\"", j * 9 + 1, j * 7 + 3));
                    }
                    s.push(']');
                }
                ("type", "smooth") => s.push_str("12"),
                ("geometry", g) => s.push_str(match g {
                    "bevel" => "2",
                    "gap" => "2",
                    "hairline" => "1",
                    "header_h" | "status_h" => "20",
                    "tag_pad_x" => "6",
                    _ => "5",
                }),
                _ => s.push_str(&alloc::format!("\"#{:02X}{:02X}40\"", i * 3 + 1, i * 5 + 2)),
            }
            s.push('\n');
        }
        s
    }

    #[test]
    fn a_complete_file_lints_as_complete() {
        let src = complete_theme_toml();
        let r = lint_files(&[ThemeFile {
            label: "system",
            path: "/lib/halcyon/theme.toml",
            text: Some(&src),
        }]);
        assert!(!r.refused, "{:?}", r.lines);
        assert_eq!(r.lines.len(), 1, "a complete file needs no key list");
        let n = libhalcyon::theme::KEYS.len();
        assert_eq!(
            r.lines[0],
            alloc::format!("system /lib/halcyon/theme.toml: OK -- \"fixture\", all {n} keys set")
        );
    }

    #[test]
    fn a_bad_colour_is_refused_by_line_and_the_same_file_fixed_is_not() {
        // The POSITIVE control, one variable away: a `refused == true`
        // assertion is satisfied by ANY broken fixture, so the identical file
        // with the single mutation undone must lint clean.
        let good = complete_theme_toml();
        let bad = good.replacen("ember = \"#", "ember = \"@", 1);
        assert_ne!(good, bad, "the mutation did not mutate");

        let ok = lint_files(&[ThemeFile {
            label: "",
            path: "/t.toml",
            text: Some(&good),
        }]);
        assert!(!ok.refused, "the control must load: {:?}", ok.lines);

        let r = lint_files(&[ThemeFile {
            label: "",
            path: "/t.toml",
            text: Some(&bad),
        }]);
        assert!(r.refused);
        assert_eq!(r.lines.len(), 1);
        // Names the file, the verdict, and where to look.
        assert!(
            r.lines[0].starts_with("/t.toml: REFUSED -- line "),
            "{}",
            r.lines[0]
        );
        assert!(r.lines[0].contains("#RRGGBB"), "{}", r.lines[0]);
    }

    #[test]
    fn a_based_file_names_every_key_it_inherited() {
        let src = "[meta]\nname = \"dusk\"\nbase = \"daylight\"\n\n\
                   [palette]\nsurface = \"#101010\"\nfg = \"#EEEEEE\"\n";
        let r = lint_files(&[ThemeFile {
            label: "user",
            path: "/home/cora/lib/halcyon/theme.toml",
            text: Some(src),
        }]);
        assert!(!r.refused);
        let n = libhalcyon::theme::KEYS.len();
        assert_eq!(
            r.lines[0],
            alloc::format!(
                "user /home/cora/lib/halcyon/theme.toml: OK -- \"dusk\", base daylight, {} of {n} keys inherited",
                n - 2
            )
        );
        // The keys themselves, wrapped -- this IS 4.3's promise: the
        // convenient mode stays auditable because the tool NAMES what was
        // inherited rather than counting it.
        let body: String = r.lines[1..].join(" ");
        for k in [
            "palette.floor",
            "palette.status_muted",
            "geometry.bevel",
            "terminal.ansi",
        ] {
            assert!(body.contains(k), "inherited list omitted {k}");
        }
        // ...and NOT the two the file actually set.
        assert!(!body.contains("palette.surface"), "{body}");
        assert!(!body.contains("palette.fg "), "{body}");
        for l in &r.lines[1..] {
            assert!(l.starts_with("    "), "unindented: {l}");
            assert!(l.len() <= LINT_WRAP, "{} cols: {l}", l.len());
        }
    }

    #[test]
    fn an_absent_tier_is_not_a_refusal() {
        let r = lint_files(&[ThemeFile {
            label: "user",
            path: "/home/cora/lib/halcyon/theme.toml",
            text: None,
        }]);
        assert!(!r.refused, "4.1: no file is the default installation");
        assert_eq!(
            r.lines,
            vec![String::from(
                "user /home/cora/lib/halcyon/theme.toml: absent"
            )]
        );
    }

    #[test]
    fn the_active_line_follows_resolve_including_the_fall_one_tier_down() {
        let good = complete_theme_toml();
        let bad = good.replacen("ember = \"#", "ember = \"@", 1);
        assert_ne!(good, bad);

        assert_eq!(lint_active_line(None, None), "active: built-in (Daylight)");
        assert_eq!(
            lint_active_line(Some(&good), None),
            "active: system -- \"fixture\""
        );
        assert_eq!(
            lint_active_line(None, Some(&good)),
            "active: user -- \"fixture\""
        );
        // The user's own file wins over the system's.
        assert_eq!(
            lint_active_line(Some(&good), Some(&good)),
            "active: user -- \"fixture\""
        );
        // A user typo falls ONE TIER DOWN, not to the built-in: the user keeps
        // seeing the system theme they had before they wrote their file.
        assert_eq!(
            lint_active_line(Some(&good), Some(&bad)),
            "active: system -- \"fixture\""
        );
        // Nothing below a refused system file: the built-in.
        assert_eq!(
            lint_active_line(Some(&bad), None),
            "active: built-in (Daylight)"
        );
        // A file with no [meta] name still resolves; it is just unnamed.
        let anon = good.replacen("name = \"fixture\"\n", "", 1);
        assert_ne!(good, anon);
        assert_eq!(
            lint_active_line(None, Some(&anon)),
            "active: user -- \"(unnamed)\""
        );
    }

    #[test]
    fn wrap_never_exceeds_the_width_and_never_splits_a_token() {
        let toks: Vec<String> = (0..12)
            .map(|i| alloc::format!("palette.key_{i:02}"))
            .collect();
        let out = wrap_tokens(&toks, "    ", 40);
        assert!(out.len() > 1, "12 keys must not fit on one 40-col line");
        for l in &out {
            assert!(l.len() <= 40, "{} cols: {l}", l.len());
        }
        // Every token survives, in order, exactly once.
        assert_eq!(
            out.join(" ").split_whitespace().collect::<Vec<_>>(),
            toks.iter().map(|s| s.as_str()).collect::<Vec<_>>()
        );
        // A token wider than the line gets its own line rather than a split.
        let long = vec![String::from("x".repeat(60)), String::from("y")];
        let out = wrap_tokens(&long, "  ", 20);
        assert_eq!(out.len(), 2);
        assert!(out[0].ends_with(&"x".repeat(60)));
        assert!(wrap_tokens(&[], "  ", 20).is_empty());
    }

    #[test]
    fn parse_theme_forms() {
        assert_eq!(
            parse_cmd(&["theme", "lint"]),
            Ok(Cmd::ThemeLint { path: None })
        );
        assert_eq!(
            parse_cmd(&["theme", "lint", "/lib/halcyon/themes/nightjar.toml"]),
            Ok(Cmd::ThemeLint {
                path: Some("/lib/halcyon/themes/nightjar.toml")
            })
        );
        assert_eq!(parse_cmd(&["theme"]), Err(CmdError::BadThemeVerb));
        assert_eq!(parse_cmd(&["theme", "show"]), Err(CmdError::BadThemeVerb));
        assert_eq!(
            parse_cmd(&["theme", "lint", "a", "b"]),
            Err(CmdError::ExtraOperand)
        );
        // An option is never a path -- otherwise `--help` reads as a filename.
        assert_eq!(
            parse_cmd(&["theme", "lint", "--help"]),
            Err(CmdError::BadPath)
        );
        assert_eq!(parse_cmd(&["theme", "lint", ""]), Err(CmdError::BadPath));
        assert_eq!(
            parse_cmd(&["theme", "lint", "bad\nname"]),
            Err(CmdError::BadPath)
        );
        // A path is a PATH: slashes and dots are fine, unlike a layout name.
        assert!(path_is_usable("./theme.toml"));
        assert!(path_is_usable("/a/b/../c.toml"));
    }

    #[test]
    fn dir_chain_is_top_down() {
        assert_eq!(
            session_dir_chain("/home/cora"),
            vec![
                String::from("/home/cora/lib"),
                String::from("/home/cora/lib/halcyon"),
                String::from("/home/cora/lib/halcyon/layouts"),
            ]
        );
    }
}
