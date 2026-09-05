// tag -- a pane's tag as its COMMAND LINE (acme; HALCYON.md 13.7). The argv
// split + the program search the shell uses, shared by the two hosts of a
// tagged leaf: the restore tool (which spawns the tag itself on the console
// path) and the per-user session compositor (which hosts it in a terminal
// tile, H-4d). One definition, so a saved tag resolves to the same binary
// whoever runs it.

use alloc::string::String;
use alloc::vec::Vec;

/// A leaf's tag as the argv a host spawns: the tag IS the command line
/// (acme), split on ASCII whitespace -- no quoting, no shell (a tag that
/// needs either names a shell explicitly). Empty for an empty/blank tag.
pub fn argv_of(tag: &str) -> Vec<&str> {
    tag.split_ascii_whitespace().collect()
}

/// The directories a bare program name is searched in, in order -- the shell's
/// `resolve_command` list (`usr/utopia/.../eval/stmt.rs`), so a saved tag
/// resolves to the same binary the shell would run. The kernel resolves a
/// spawn name relative to the child's CWD (not a `$path` search), so a host
/// must expand a bare name itself.
pub const PROG_DIRS: [&str; 3] = ["/bin/", "/", "/goroot/bin/"];

/// The candidate paths for `argv0`, in probe order. A name containing `/` is
/// used verbatim (one candidate); a bare name expands to the PROG_DIRS joins.
/// The caller probes each and, on no hit, spawns the first (`/bin/<name>`)
/// for a clean, shell-identical spawn error.
pub fn prog_candidates(argv0: &str) -> Vec<String> {
    if argv0.contains('/') {
        return alloc::vec![String::from(argv0)];
    }
    PROG_DIRS
        .iter()
        .map(|d| {
            let mut s = String::from(*d);
            s.push_str(argv0);
            s
        })
        .collect()
}

/// The path to spawn for `argv0`: the first candidate `exists` says is
/// there, else the first candidate (so the spawn fails with the error the
/// shell would give for the same name).
pub fn resolve_prog(argv0: &str, exists: impl Fn(&str) -> bool) -> String {
    let cands = prog_candidates(argv0);
    cands
        .iter()
        .find(|c| exists(c))
        .cloned()
        .unwrap_or_else(|| cands.into_iter().next().unwrap_or_default())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn resolve_takes_the_first_existing_candidate() {
        assert_eq!(
            resolve_prog("hx", |p| p == "/goroot/bin/hx"),
            "/goroot/bin/hx"
        );
        assert_eq!(resolve_prog("hx", |p| p == "/bin/hx"), "/bin/hx");
        // Nothing found: the shell-identical first candidate.
        assert_eq!(resolve_prog("nope", |_| false), "/bin/nope");
        // A path is used verbatim, existing or not.
        assert_eq!(resolve_prog("./thing", |_| false), "./thing");
    }
}
