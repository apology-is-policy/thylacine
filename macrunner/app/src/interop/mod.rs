// The Mac's end of the line to the Mac (docs/MAC-RUNNER.md 4.4): the channel framing `mac1` on
// QEMU's socket, and on each channel one 9P2000.L session over the tree version 1 -- `snarf`, the
// Mac pasteboard's text, and `folders/<name>/`, each folder the person shares.
mod errno;
mod folder;
mod line;
mod locks;
mod log;
mod snarf;
mod tree;

use std::path::PathBuf;

pub use line::serve;
pub use log::Log;
pub use snarf::{MemoryPasteboard, Pasteboard, SystemPasteboard};

/// A folder the person shares: its name under `folders/`, the host folder, and whether the guest
/// may change it.
#[derive(Clone, Debug)]
pub struct Share {
    pub name: String,
    pub path: PathBuf,
    pub read_only: bool,
}

impl Share {
    /// `<name>=<path>` or `<name>=<path>:ro`. The name is non-empty, holds no `/` or NUL, and is
    /// not `.` or `..` (4.4: it is one directory's name in the guest).
    pub fn parse(arg: &str) -> Result<Share, String> {
        let (name, rest) = arg
            .split_once('=')
            .ok_or_else(|| format!("a share is <name>=<path>[:ro], not '{arg}'"))?;
        let (path, read_only) = match rest.strip_suffix(":ro") {
            Some(p) => (p, true),
            None => (rest, false),
        };
        if name.is_empty()
            || name == "."
            || name == ".."
            || name.contains('/')
            || name.contains('\0')
        {
            return Err(format!("'{name}' cannot name a shared folder"));
        }
        if path.is_empty() {
            return Err(format!("the share '{name}' names no folder"));
        }
        Ok(Share {
            name: name.to_string(),
            path: PathBuf::from(path),
            read_only,
        })
    }
}

/// How the app serves the line.
pub struct Config {
    /// QEMU's socket behind the port `org.thylacine.mac`.
    pub socket: PathBuf,
    pub shares: Vec<Share>,
    /// Serve one connection and return when it ends (a test's); otherwise connect again, as QEMU
    /// takes a new client after a disconnect.
    pub once: bool,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_share_is_a_name_a_path_and_maybe_ro() {
        let s = Share::parse("work=/Users/me/work").unwrap();
        assert_eq!(
            (s.name.as_str(), s.path.to_str().unwrap(), s.read_only),
            ("work", "/Users/me/work", false)
        );
        let s = Share::parse("docs=/Users/me/Documents:ro").unwrap();
        assert_eq!(
            (s.name.as_str(), s.path.to_str().unwrap(), s.read_only),
            ("docs", "/Users/me/Documents", true)
        );
        // Only the first `=` splits: a path may hold one.
        assert_eq!(Share::parse("a=/x=y").unwrap().path.to_str(), Some("/x=y"));
    }

    #[test]
    fn a_name_that_is_no_directory_name_is_refused() {
        for bad in ["=/x", ".=/x", "..=/x", "a/b=/x", "a\0b=/x", "nopath", "a="] {
            assert!(Share::parse(bad).is_err(), "{bad:?}");
        }
    }
}
