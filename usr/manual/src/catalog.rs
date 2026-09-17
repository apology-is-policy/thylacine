//! Section lookup (MANUAL-DESIGN.md section 5): which files in `/manual` are
//! sections, their book order, and how an operand names one.

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

/// An installed section, described by its file name `NN-<name>.md`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Entry {
    pub number: u8,
    pub name: String,
    pub file: String,
}

/// `(number, name)` for a section file name `NN-<name>.md` (3.1), where the
/// name is lowercase ASCII letters, digits and hyphens; `None` otherwise.
pub fn parse_file_name(file: &str) -> Option<(u8, &str)> {
    let stem = file.strip_suffix(".md")?;
    let b = stem.as_bytes();
    if b.len() < 4 || !b[0].is_ascii_digit() || !b[1].is_ascii_digit() || b[2] != b'-' {
        return None;
    }
    let name = &stem[3..];
    if !name
        .bytes()
        .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == b'-')
    {
        return None;
    }
    Some(((b[0] - b'0') * 10 + (b[1] - b'0'), name))
}

/// The sections among `files`, in book order: by number, then by name.
pub fn entries<'a, I: IntoIterator<Item = &'a str>>(files: I) -> Vec<Entry> {
    let mut v: Vec<Entry> = files
        .into_iter()
        .filter_map(|f| {
            parse_file_name(f).map(|(number, name)| Entry {
                number,
                name: name.to_string(),
                file: f.to_string(),
            })
        })
        .collect();
    v.sort_by(|a, b| (a.number, &a.name).cmp(&(b.number, &b.name)));
    v
}

#[derive(Debug, PartialEq, Eq)]
pub enum Lookup<'a> {
    Found(&'a Entry),
    Missing,
    Ambiguous(Vec<&'a Entry>),
}

fn pick(v: Vec<&Entry>) -> Option<Lookup<'_>> {
    match v.len() {
        0 => None,
        1 => Some(Lookup::Found(v[0])),
        _ => Some(Lookup::Ambiguous(v)),
    }
}

/// Resolve an operand to a section (5): an exact name, then a number, then the
/// full `NN-<name>`, then the only name the operand begins. Letter case is
/// ignored. A step that matches several sections is ambiguous and ends the
/// search.
pub fn lookup<'a>(entries: &'a [Entry], operand: &str) -> Lookup<'a> {
    let op = operand.to_ascii_lowercase();
    if op.is_empty() {
        return Lookup::Missing;
    }
    if let Some(l) = pick(entries.iter().filter(|e| e.name == op).collect()) {
        return l;
    }
    if op.len() <= 2 && op.bytes().all(|b| b.is_ascii_digit()) {
        let n: u8 = op.parse().unwrap_or(0);
        if let Some(l) = pick(entries.iter().filter(|e| e.number == n).collect()) {
            return l;
        }
    }
    let file = format!("{}.md", op);
    if let Some((n, name)) = parse_file_name(&file) {
        if let Some(l) = pick(
            entries
                .iter()
                .filter(|e| e.number == n && e.name == name)
                .collect(),
        ) {
            return l;
        }
    }
    pick(
        entries
            .iter()
            .filter(|e| e.name.starts_with(op.as_str()))
            .collect(),
    )
    .unwrap_or(Lookup::Missing)
}

/// True when an operand names a file rather than a section (5).
pub fn is_path_operand(operand: &str) -> bool {
    operand.contains('/') || operand.ends_with(".md")
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn book() -> Vec<Entry> {
        entries(vec![
            "41-audio.md",
            "05-containers.md",
            "06-clade.md",
            "40-dosbox.md",
            ".gitkeep",
            "README.md",
            "5-short.md",
            "07-Upper.md",
        ])
    }

    fn found(l: Lookup<'_>) -> &str {
        match l {
            Lookup::Found(e) => e.name.as_str(),
            other => panic!("expected a match, got {:?}", other),
        }
    }

    #[test]
    fn file_names() {
        assert_eq!(parse_file_name("05-containers.md"), Some((5, "containers")));
        assert_eq!(parse_file_name("40-dos-box2.md"), Some((40, "dos-box2")));
        assert_eq!(parse_file_name("5-short.md"), None);
        assert_eq!(parse_file_name("05-.md"), None);
        assert_eq!(parse_file_name("05_containers.md"), None);
        assert_eq!(parse_file_name("05-Containers.md"), None);
        assert_eq!(parse_file_name("05-containers.txt"), None);
        assert_eq!(parse_file_name(".gitkeep"), None);
    }

    #[test]
    fn entries_are_in_book_order_and_skip_other_files() {
        let b = book();
        let names: Vec<&str> = b.iter().map(|e| e.name.as_str()).collect();
        assert_eq!(names, vec!["containers", "clade", "dosbox", "audio"]);
        assert_eq!(b[0].file, "05-containers.md");
    }

    #[test]
    fn lookup_forms() {
        let b = book();
        assert_eq!(found(lookup(&b, "containers")), "containers");
        assert_eq!(found(lookup(&b, "Containers")), "containers");
        assert_eq!(found(lookup(&b, "05")), "containers");
        assert_eq!(found(lookup(&b, "5")), "containers");
        assert_eq!(found(lookup(&b, "41-audio")), "audio");
        assert_eq!(found(lookup(&b, "dos")), "dosbox");
        assert_eq!(found(lookup(&b, "cont")), "containers");
    }

    #[test]
    fn lookup_ambiguity_and_absence() {
        let b = book();
        match lookup(&b, "c") {
            Lookup::Ambiguous(v) => {
                let names: Vec<&str> = v.iter().map(|e| e.name.as_str()).collect();
                assert_eq!(names, vec!["containers", "clade"]);
            }
            other => panic!("{:?}", other),
        }
        assert_eq!(lookup(&b, "networking"), Lookup::Missing);
        assert_eq!(lookup(&b, ""), Lookup::Missing);
        assert_eq!(lookup(&b, "99"), Lookup::Missing);
        assert_eq!(lookup(&[], "audio"), Lookup::Missing);
    }

    #[test]
    fn an_exact_name_wins_over_a_longer_prefix_match() {
        let b = entries(vec!["10-net.md", "11-network.md"]);
        assert_eq!(found(lookup(&b, "net")), "net");
    }

    #[test]
    fn a_shared_number_is_ambiguous() {
        let b = entries(vec!["10-net.md", "10-disk.md"]);
        assert!(matches!(lookup(&b, "10"), Lookup::Ambiguous(_)));
        assert_eq!(found(lookup(&b, "10-disk")), "disk");
    }

    #[test]
    fn path_operands() {
        assert!(is_path_operand("./draft.md"));
        assert!(is_path_operand("/manual/05-containers.md"));
        assert!(is_path_operand("draft.md"));
        assert!(!is_path_operand("containers"));
        assert!(!is_path_operand("05"));
    }
}
