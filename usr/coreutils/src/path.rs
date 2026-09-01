//! Lexical path canonicalization, shared by `realpath` and the Beacon
//! emitters (an `obj type=path` ref MUST be the cleaned ABSOLUTE form --
//! BEACON.md 12.2's obj rule; a relative or dirty ref is a wrong ref).
//!
//! Lexical only: collapse `.` / `..` / `//`, no symlink resolution (G11) and
//! no existence requirement -- `realpath -m -s` semantics.

use alloc::string::String;
use alloc::vec::Vec;

/// Collapse `.`, `..`, and repeated `/` in an already-absolute (or
/// root-anchored) path. `..` at the root stays at the root.
pub fn normalize(path: &str) -> String {
    let mut stack: Vec<&str> = Vec::new();
    for comp in path.split('/') {
        match comp {
            "" | "." => {}
            ".." => {
                stack.pop();
            }
            c => stack.push(c),
        }
    }
    let mut out = String::from("/");
    out.push_str(&stack.join("/"));
    out
}

/// The cleaned absolute form of `path`: an absolute input is normalized as
/// is; a relative one is anchored at the per-Proc cwd first. `None` when the
/// cwd is unreadable (the caller degrades -- for an obj ref that means "emit
/// no frame", never a guessed ref).
#[cfg(feature = "backend")]
pub fn abs(path: &str) -> Option<String> {
    if path.starts_with('/') {
        return Some(normalize(path));
    }
    let mut cwd = libthyla_rs::env::current_dir().ok()?;
    if !cwd.ends_with('/') {
        cwd.push('/');
    }
    cwd.push_str(path);
    Some(normalize(&cwd))
}

#[cfg(test)]
mod tests {
    use super::normalize;

    #[test]
    fn collapses() {
        assert_eq!(normalize("/a/b/../c"), "/a/c");
        assert_eq!(normalize("/a//b/./c/"), "/a/b/c");
        assert_eq!(normalize("/../../x"), "/x");
        assert_eq!(normalize("/"), "/");
        assert_eq!(normalize("/a/.."), "/");
    }
}
