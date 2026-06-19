//! Shared metadata presentation: the entry Kind (its color / classify suffix /
//! realm), the perms string, the owner, and the 9P qid -- the logic ls / stat /
//! realm / qid all present. Backend-gated (uses libthyla-rs `Metadata`).

use crate::palette;
use alloc::format;
use alloc::string::String;
use libthyla_rs::fs::Metadata;

/// The kind of a namespace entry -- drives its color, classify suffix, and the
/// REALM column. `Graft` is the Thylacine flavor: a live kernel namespace mount
/// (an entry `readdir` reports as a directory but `fstat` cannot cross).
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    Dir,
    Exec,
    File,
    Dev,
    Graft,
}

impl Kind {
    pub fn color(self) -> &'static str {
        match self {
            Kind::Dir => palette::SLATE,
            Kind::Exec => palette::GREEN,
            Kind::Dev => palette::GOLD,
            Kind::Graft => palette::VIOLET,
            Kind::File => palette::FG,
        }
    }

    pub fn suffix(self) -> &'static str {
        match self {
            Kind::Dir | Kind::Graft => "/",
            Kind::Exec => "*",
            _ => "",
        }
    }

    pub fn realm(self) -> &'static str {
        match self {
            Kind::Graft => "graft",
            Kind::Dev => "dev",
            _ => "fs",
        }
    }
}

/// Classify from what `readdir` said (was it a directory?) and the `fstat`
/// result (`None` == fstat failed). A directory that fails fstat is a graft; a
/// non-dir that fails is shown as a plain file.
pub fn classify(rd_dir: bool, md: &Option<Metadata>) -> Kind {
    match md {
        Some(m) => kind_of(m),
        None if rd_dir => Kind::Graft,
        None => Kind::File,
    }
}

/// The kind of a stattable entry (never a graft -- fstat succeeded).
pub fn kind_of(m: &Metadata) -> Kind {
    if m.is_dir() {
        Kind::Dir
    } else if m.is_char_device() {
        Kind::Dev
    } else if m.permissions() & 0o111 != 0 {
        Kind::Exec
    } else {
        Kind::File
    }
}

/// The 10-char permission string (drwxr-xr-x).
pub fn perms(m: &Metadata) -> [u8; 10] {
    let mut s = [b'-'; 10];
    s[0] = if m.is_dir() {
        b'd'
    } else if m.is_symlink() {
        b'l'
    } else if m.is_char_device() {
        b'c'
    } else {
        b'-'
    };
    let p = m.permissions();
    let bits: [(u32, usize, u8); 9] = [
        (0o400, 1, b'r'),
        (0o200, 2, b'w'),
        (0o100, 3, b'x'),
        (0o040, 4, b'r'),
        (0o020, 5, b'w'),
        (0o010, 6, b'x'),
        (0o004, 7, b'r'),
        (0o002, 8, b'w'),
        (0o001, 9, b'x'),
    ];
    for (mask, i, ch) in bits {
        if p & mask != 0 {
            s[i] = ch;
        }
    }
    s
}

/// The 10-char permission string as a `String` (the form ls / stat print).
pub fn perms_string(m: &Metadata) -> String {
    let p = perms(m);
    String::from(core::str::from_utf8(&p).unwrap_or("----------"))
}

/// The owner label: `system` for the kernel principal, else the numeric uid
/// (there is no uid->name service yet -- LS-K seam).
pub fn owner(uid: u32) -> String {
    if uid == libthyla_rs::T_PRINCIPAL_SYSTEM {
        String::from("system")
    } else {
        format!("{}", uid)
    }
}

fn qid_type(m: &Metadata) -> char {
    if m.is_dir() {
        'd'
    } else if m.is_char_device() {
        'c'
    } else {
        'f'
    }
}

/// The compact 9P qid `t:0x{path}` (the ls column form).
pub fn qid_compact(m: &Metadata) -> String {
    format!("{}:0x{:x}", qid_type(m), m.qid_path())
}

/// The full 9P qid `t:v{vers}:0x{path}` (the stat / qid-tool form).
pub fn qid_full(m: &Metadata) -> String {
    format!("{}:v{}:0x{:x}", qid_type(m), m.qid_vers(), m.qid_path())
}
