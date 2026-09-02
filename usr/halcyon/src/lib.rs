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
    /// `halcyon`, `halcyon help`, `--help`, `-h`.
    Help,
}

/// Why a command line was rejected. Each maps to one diagnostic line in the
/// binary; kept a plain enum so the dispatch is exhaustively host-testable.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CmdError {
    /// The first token was not a known subcommand.
    UnknownCommand,
    /// `layout` with no verb, or a verb that is not save/restore.
    BadLayoutVerb,
    /// `layout save|restore` with no name operand.
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
    if verb != "save" && verb != "restore" {
        return Err(CmdError::BadLayoutVerb);
    }
    let name = *rest.get(1).ok_or(CmdError::MissingName)?;
    if rest.len() > 2 {
        return Err(CmdError::ExtraOperand);
    }
    if !name_is_valid(name) {
        return Err(CmdError::BadName);
    }
    Ok(if verb == "save" {
        Cmd::LayoutSave { name }
    } else {
        Cmd::LayoutRestore { name }
    })
}

/// A layout name is a single safe path component: non-empty, <= MAX_NAME_LEN,
/// no leading `.` (so `.`, `..`, and hidden names are all out), and drawn only
/// from `[A-Za-z0-9._-]` (so no `/` traversal and no whitespace/control). The
/// name lands in the user's OWN namespace, but a conservative charset keeps a
/// saved layout a predictable filename and closes traversal by construction.
pub fn name_is_valid(name: &str) -> bool {
    if name.is_empty() || name.len() > MAX_NAME_LEN || name.starts_with('.') {
        return false;
    }
    name.bytes()
        .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_' || b == b'.')
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
