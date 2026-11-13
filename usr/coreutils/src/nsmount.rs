//! A namespace's mount list, parsed from the kernel's `/proc/<pid>/ns` text
//! (`territory_format_ns`; ARCH 9.5, HAUL-DESIGN 4.8). `ns` renders it, and
//! `ls` / `stat` / `realm` read a mount point's REALM from it.
//!
//! The kernel writes one `mount <point> <source>[ <suffix>]...` line per entry,
//! then `binds: N`, then `root: pheno-linux` when the namespace carries the
//! declaration. A name writes space, tab, newline and backslash as `\040 \011
//! \012 \134` (operator vote 97), so the fields split on spaces and the lines
//! on newlines; `parse_line` unescapes the point and the source. A consumer
//! must still degrade on a malformed line, never panic.
//!
//! Pure (no libthyla-rs): host-tested.

use crate::path;
use alloc::borrow::Cow;
use alloc::string::String;
use alloc::vec::Vec;

/// One `mount` line.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Mount<'a> {
    /// Unescaped: the name as the namespace holds it.
    pub point: Cow<'a, str>,
    /// Unescaped, as `point`.
    pub source: Cow<'a, str>,
    pub noexec: bool,
    pub pheno_linux: bool,
    /// A union's covered entry: the directory underneath, not a member anyone
    /// mounted.
    pub covered: bool,
    /// The source belongs to a 9P session declared remote.
    pub remote: bool,
    /// Suffixes this parser does not know, as written.
    pub unknown: Vec<&'a str>,
}

/// One line of the text.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Line<'a> {
    Mount(Mount<'a>),
    Binds(u64),
    RootPheno,
    Other(&'a str),
}

/// The REALM `ns` shows for an entry: `remote` when the kernel marks it so;
/// else what its source names. A `#<dc>` device spec names a device root, by
/// its character -- except `#|`, the pipe a 9P session came over (the kernel
/// names a session root by its transport file), which reads `9p` like `#9`,
/// the session root with no name at all. A namespace name reads `fs`: a
/// mounted subtree, or a session root named by its `/srv` connection.
pub fn entry_realm(m: &Mount) -> &'static str {
    if m.remote {
        return "remote";
    }
    source_realm(&m.source)
}

pub fn source_realm(src: &str) -> &'static str {
    match src.strip_prefix('#').and_then(|s| s.chars().next()) {
        Some('9') | Some('|') => "9p",
        Some('r') | Some('M') => "boot",
        Some('p') => "proc",
        Some('s') => "srv",
        Some('H') => "hw",
        Some('n') => "notes",
        Some('d') => "dev",
        Some('c') | Some('C') => "cons",
        Some(_) => "dev",
        None => "fs",
    }
}

/// A name as a mount line writes it, unescaped: a backslash and three octal
/// digits below `\200` is that character (the kernel writes only `\040 \011
/// \012 \134`); anything else stays as written.
pub fn unescape(field: &str) -> Cow<'_, str> {
    if !field.contains('\\') {
        return Cow::Borrowed(field);
    }
    let b = field.as_bytes();
    let mut out = String::with_capacity(field.len());
    let (mut i, mut lit) = (0, 0);
    while i < b.len() {
        if b[i] == b'\\' && i + 4 <= b.len() && b[i + 1..i + 4].iter().all(|c| (b'0'..=b'7').contains(c)) {
            let v = (b[i + 1] - b'0') as u32 * 64 + (b[i + 2] - b'0') as u32 * 8 + (b[i + 3] - b'0') as u32;
            if v < 0o200 {
                out.push_str(&field[lit..i]);
                out.push(v as u8 as char);
                i += 4;
                lit = i;
                continue;
            }
        }
        i += 1;
    }
    out.push_str(&field[lit..]);
    Cow::Owned(out)
}

pub fn parse_line(line: &str) -> Line<'_> {
    if let Some(rest) = line.strip_prefix("mount ") {
        let mut fields = rest.split(' ').filter(|f| !f.is_empty());
        let (point, source) = match (fields.next(), fields.next()) {
            (Some(p), Some(s)) => (p, s),
            _ => return Line::Other(line),
        };
        let mut m = Mount {
            point: unescape(point),
            source: unescape(source),
            noexec: false,
            pheno_linux: false,
            covered: false,
            remote: false,
            unknown: Vec::new(),
        };
        for suffix in fields {
            match suffix {
                "noexec" => m.noexec = true,
                "pheno-linux" => m.pheno_linux = true,
                "covered" => m.covered = true,
                "remote" => m.remote = true,
                other => m.unknown.push(other),
            }
        }
        return Line::Mount(m);
    }
    if let Some(n) = line.strip_prefix("binds: ") {
        if let Ok(v) = n.trim().parse() {
            return Line::Binds(v);
        }
    }
    if line == "root: pheno-linux" {
        return Line::RootPheno;
    }
    Line::Other(line)
}

/// The REALM of each mount point, by the LR-1 rule (COREUTILS-THYLACINE-DESIGN
/// REALM): `remote` when any member at the point is marked remote, `mount`
/// otherwise. A covered entry is not a member. Only an absolute point name
/// counts (the kernel writes `?` for a point with no retained name), and names
/// are compared cleaned.
#[derive(Debug, Clone, Default)]
pub struct MountRealms {
    points: Vec<(String, bool)>,
    truncated: bool,
}

impl MountRealms {
    pub fn from_text(text: &str) -> MountRealms {
        let mut points: Vec<(String, bool)> = Vec::new();
        let mut whole = false;
        // Newlines only: `lines` would take a name's trailing carriage return.
        for line in text.split('\n') {
            let m = match parse_line(line) {
                Line::Mount(m) => m,
                Line::Binds(_) => {
                    whole = true;
                    continue;
                }
                _ => continue,
            };
            if m.covered || !m.point.starts_with('/') {
                continue;
            }
            let point = path::normalize(&m.point);
            match points.iter_mut().find(|(p, _)| *p == point) {
                Some(entry) => entry.1 |= m.remote,
                None => points.push((point, m.remote)),
            }
        }
        MountRealms { points, truncated: !whole }
    }

    /// The kernel writes `binds:` only after a list it rendered whole (#66b),
    /// so a text without it was cut, and a mount point past the cut is missing
    /// here. An unread list (`default`) is not a cut one.
    pub fn truncated(&self) -> bool {
        self.truncated
    }

    /// The realm of `abs`, a cleaned absolute path, when it is a mount point.
    pub fn realm(&self, abs: &str) -> Option<&'static str> {
        self.points
            .iter()
            .find(|(p, _)| p == abs)
            .map(|(_, remote)| if *remote { "remote" } else { "mount" })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_plain_line() {
        assert_eq!(
            parse_line("mount /srv #s"),
            Line::Mount(Mount {
                point: Cow::Borrowed("/srv"),
                source: Cow::Borrowed("#s"),
                noexec: false,
                pheno_linux: false,
                covered: false,
                remote: false,
                unknown: Vec::new(),
            })
        );
    }

    #[test]
    fn every_known_suffix_and_an_unknown_one() {
        let Line::Mount(m) = parse_line("mount /u /u noexec pheno-linux covered remote shiny") else {
            panic!("not a mount line");
        };
        assert_eq!((&*m.point, &*m.source), ("/u", "/u"));
        assert!(m.noexec && m.pheno_linux && m.covered && m.remote);
        assert_eq!(m.unknown, alloc::vec!["shiny"]);
    }

    #[test]
    fn a_session_root_reads_by_the_file_it_came_over() {
        assert_eq!(source_realm("#|"), "9p", "a pipe-borne session root");
        assert_eq!(source_realm("#9"), "9p", "a session root with no name");
        assert_eq!(source_realm("/srv/home-joey"), "fs", "named by its connection");
        assert_eq!(source_realm("#s"), "srv");
        assert_eq!(source_realm("#x"), "dev", "an unknown device");
        let Line::Mount(m) = parse_line("mount /tmp/host2 #| remote") else {
            panic!("not a mount line");
        };
        assert_eq!(entry_realm(&m), "remote", "the mark outranks the source");
        let Line::Mount(m) = parse_line("mount /tmp/host2 #|") else {
            panic!("not a mount line");
        };
        assert_eq!(entry_realm(&m), "9p");
    }

    #[test]
    fn the_other_lines() {
        assert_eq!(parse_line("binds: 3"), Line::Binds(3));
        assert_eq!(parse_line("root: pheno-linux"), Line::RootPheno);
        assert_eq!(parse_line("binds: x"), Line::Other("binds: x"));
        assert_eq!(parse_line("mount /only"), Line::Other("mount /only"));
        assert_eq!(parse_line("mount "), Line::Other("mount "));
        assert_eq!(parse_line(""), Line::Other(""));
    }

    const UNION: &str = "mount /tmp/host #9 remote\n\
                         mount /n #- \n\
                         mount /n /n covered\n\
                         mount /mixed #9\n\
                         mount /mixed #9 remote\n\
                         mount /h #-\n\
                         mount /h /h covered remote\n\
                         mount ? #p\n\
                         binds: 0\n";

    #[test]
    fn a_remote_member_makes_the_point_remote() {
        let r = MountRealms::from_text(UNION);
        assert_eq!(r.realm("/tmp/host"), Some("remote"));
        assert_eq!(r.realm("/mixed"), Some("remote"), "any remote member");
        assert_eq!(r.realm("/n"), Some("mount"));
    }

    #[test]
    fn a_covered_entry_is_not_a_member() {
        // Even a covered line that says remote (the kernel never writes one)
        // does not make its point remote.
        let r = MountRealms::from_text(UNION);
        assert_eq!(r.realm("/h"), Some("mount"));
    }

    #[test]
    fn only_mount_points_have_a_realm_here() {
        let r = MountRealms::from_text(UNION);
        assert_eq!(r.realm("/tmp"), None, "the parent of a mount point");
        assert_eq!(r.realm("/tmp/host/x"), None, "inside a mount");
        assert_eq!(r.realm("/?"), None, "the nameless point never matches");
        assert_eq!(r.realm("?"), None);
    }

    #[test]
    fn names_compare_cleaned() {
        let r = MountRealms::from_text("mount /a//b/ #9 remote\n");
        assert_eq!(r.realm("/a/b"), Some("remote"));
    }

    #[test]
    fn a_broken_list_degrades_to_no_realm() {
        let r = MountRealms::from_text("mount\nmount /x\ngarbage\n\n");
        assert_eq!(r.realm("/x"), None);
        assert_eq!(MountRealms::from_text("").realm("/"), None);
        assert_eq!(MountRealms::default().realm("/srv"), None);
    }

    #[test]
    fn a_list_without_its_binds_line_was_cut() {
        assert!(!MountRealms::from_text("mount /a #s\nbinds: 0\n").truncated());
        assert!(!MountRealms::from_text("binds: 0\nroot: pheno-linux\n").truncated());
        let cut = MountRealms::from_text("mount /a #s\nmount /b / remote\n");
        assert!(cut.truncated());
        assert_eq!(cut.realm("/b"), Some("remote"), "a cut list still names what it holds");
        assert!(MountRealms::from_text("").truncated(), "binds: is written even for no mounts");
        assert!(MountRealms::from_text("mount /a #s\nbinds: x\n").truncated(), "a malformed count");
        assert!(!MountRealms::default().truncated(), "an unread list is not a cut one");
    }

    #[test]
    fn a_name_with_a_space_keeps_its_realm() {
        let r = MountRealms::from_text("mount /a\\040b #9 remote\nmount /c #s\n");
        assert_eq!(r.realm("/a b"), Some("remote"), "the escaped point is the name");
        assert_eq!(r.realm("/a"), None, "no part of it is a point");
        assert_eq!(r.realm("/c"), Some("mount"));
    }

    #[test]
    fn a_carriage_return_stays_in_its_name() {
        let r = MountRealms::from_text("mount /a\r #9 remote\nmount /b\r #s\nbinds: 0\n");
        assert_eq!(r.realm("/a\r"), Some("remote"), "a point ending in a carriage return");
        assert_eq!(r.realm("/b\r"), Some("mount"));
        assert_eq!(r.realm("/a"), None);
        assert!(!r.truncated());
        let Line::Mount(m) = parse_line("mount /s /srv/boot\r") else {
            panic!("not a mount line");
        };
        assert_eq!(&*m.source, "/srv/boot\r", "the last field keeps it too");
    }

    #[test]
    fn the_four_escapes_and_what_is_not_one() {
        assert_eq!(unescape("/a\\040b\\011c\\012d\\134e"), "/a b\tc\nd\\e");
        assert!(matches!(unescape("/plain"), Cow::Borrowed("/plain")), "nothing to undo borrows");
        assert_eq!(unescape("/x\\04"), "/x\\04", "two digits at the end stay");
        assert_eq!(unescape("/x\\08y"), "/x\\08y", "8 is not octal");
        assert_eq!(unescape("/x\\200"), "/x\\200", "past ASCII stays");
        assert_eq!(unescape("\\134134"), "\\134", "decoded once, left to right");
        assert_eq!(unescape("/\u{e9}\\040\u{e9}"), "/\u{e9} \u{e9}", "beside a multi-byte character");
        let Line::Mount(m) = parse_line("mount /a\\040b /s\\134p\\012q covered") else {
            panic!("not a mount line");
        };
        assert_eq!((&*m.point, &*m.source), ("/a b", "/s\\p\nq"));
        assert!(m.covered && m.unknown.is_empty(), "the suffixes stay suffixes");
    }
}
