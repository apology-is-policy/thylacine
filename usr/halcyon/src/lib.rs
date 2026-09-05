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
}

/// Parse argv[1..] (`tokens`) into a [`Cmd`]. Pure: the name is borrowed from
/// `tokens`, nothing is read or allocated.
pub fn parse_cmd<'a>(tokens: &[&'a str]) -> Result<Cmd<'a>, CmdError> {
    match tokens.first().copied() {
        None | Some("help") | Some("--help") | Some("-h") => Ok(Cmd::Help),
        Some("layout") => parse_layout(&tokens[1..]),
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
