// The line's record: one line per event, in the words tools/mac-app-fake.py writes, so that a
// guest witness reads the app's record as it reads the fake's.
use std::fs::{File, OpenOptions};
use std::io::Write;
use std::path::Path;
use std::sync::Mutex;
use std::time::{SystemTime, UNIX_EPOCH};

pub struct Log {
    file: Option<Mutex<File>>,
}

impl Log {
    /// A record appended to `path`, or none.
    pub fn open(path: Option<&Path>) -> std::io::Result<Log> {
        let file = match path {
            Some(p) => Some(Mutex::new(
                OpenOptions::new().create(true).append(true).open(p)?,
            )),
            None => None,
        };
        Ok(Log { file })
    }

    pub fn line(&self, what: &str) {
        let Some(f) = &self.file else { return };
        let now = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default();
        let mut f = f.lock().unwrap_or_else(|e| e.into_inner());
        let _ = writeln!(f, "{}.{:03} {what}", now.as_secs(), now.subsec_millis());
        let _ = f.flush();
    }
}

/// Python's `repr` of a string, as the fake's record writes one: single quotes unless the text
/// holds one and no double quote; a backslash, the quote, tab, newline, return and every other
/// control character escaped.
pub fn repr(text: &str) -> String {
    let quote = if text.contains('\'') && !text.contains('"') {
        '"'
    } else {
        '\''
    };
    let mut out = String::with_capacity(text.len() + 2);
    out.push(quote);
    for c in text.chars() {
        match c {
            '\\' => out.push_str("\\\\"),
            '\t' => out.push_str("\\t"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            c if c == quote => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 || (0x7f..0xa0).contains(&(c as u32)) => {
                out.push_str(&format!("\\x{:02x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push(quote);
    out
}

/// Bytes read as Latin-1, each byte one character (the fake decodes a uname and an aname so).
pub fn latin1(b: &[u8]) -> String {
    b.iter().map(|&c| c as char).collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn repr_writes_what_python_writes() {
        assert_eq!(repr(""), "''");
        assert_eq!(repr("from-the-guest"), "'from-the-guest'");
        assert_eq!(repr("it's"), "\"it's\"");
        assert_eq!(repr("both ' and \""), "'both \\' and \"'");
        assert_eq!(repr("a\\b\tc\nd\re"), "'a\\\\b\\tc\\nd\\re'");
        assert_eq!(repr("\u{1}\u{7f}\u{9f}"), "'\\x01\\x7f\\x9f'");
        assert_eq!(repr("snarf \u{e9}"), "'snarf \u{e9}'");
    }

    #[test]
    fn latin1_keeps_every_byte() {
        assert_eq!(latin1(b"michael"), "michael");
        assert_eq!(latin1(&[0xe9]), "\u{e9}");
    }
}
