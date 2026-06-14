// libutopia::completion -- the namespace-driven Tab completion source (#115a).
//
// The U-4d line editor (`line_editor.rs`) ships the completion ENGINE -- the
// `CompletionSource` trait, the longest-common-prefix extension, the
// single-vs-menu dispatch -- but no real source: a bare `ut` left Tab inert
// because the only source was the test-only `StaticCompletionSource`. This
// module is the production source the shell installs (`Repl::install_completion`).
//
// Two completion contexts, classified from the buffer + cursor:
//
//   - COMMAND position (token-0, or after a `| ; & { (` operator) with a bare
//     name -> complete against the COMMAND INDEX: builtins + aliases + funcs +
//     the `/bin` scan (the #58 exec namespace, the same set `resolve_command`
//     searches). The index is precomputed by the shell (it owns the alias /
//     func tables + can readdir `/bin`) and handed to this source.
//
//   - ARGUMENT position (any later token), OR a command-by-path (a token
//     containing `/`, like `./script` or `/bin/foo`) -> PATH completion:
//     split the token at its last `/` into a directory prefix + a file prefix,
//     `read_dir` the directory live, and offer the entries whose name extends
//     the file prefix. `cd <TAB>` restricts to directories (the only sensible
//     `cd` target). A relative directory resolves against the per-Proc cwd
//     (LS-4 `SYS_CHDIR`, which `cd` keeps synced), so `read_dir(".")` is right.
//
// Each candidate carries its terminator -- a trailing space for a command or
// regular file, a trailing `/` for a directory -- so a unique completion lands
// ready for the next token (and a directory can be drilled with a second Tab).
// This is the readline convention; the engine's LCP math is unaffected because
// the terminator always falls AFTER the first differing character of any two
// distinct candidates (two entries can't share a name).
//
// Per the Plan 9 native split + UTOPIA-SHELL-DESIGN.md section 11.2: pure
// userspace logic over libthyla-rs `fs::read_dir` (already audited, RW-8); the
// audit-bearing raw-mode editor + consctl surface this rides on was discharged
// at the Kaua T-4 audit (#101). The classification + path-split are pure (unit
// tested below); only the directory read is a syscall, taken solely on Tab.

use alloc::string::String;
use alloc::vec::Vec;

use crate::line_editor::{Completions, CompletionSource};

/// Cap on the candidates returned from one Tab (the design's `LISTMAX`). A
/// directory of thousands of entries must not flood the completion menu; the
/// cap bounds both the work and the `MenuShow` cycle payload.
const MAX_CANDIDATES: usize = 256;

/// The production Tab-completion source. Owns a precomputed command index
/// (builtins + aliases + funcs + `/bin`, sorted + deduped) for command-position
/// completion; path completion reads the live filesystem on demand.
pub struct ShellCompletionSource {
    /// Known command names, sorted + deduped. Also the artifact #115c coloring
    /// consults (`LineEditor::set_known_commands`), built once by the shell.
    commands: Vec<String>,
}

impl ShellCompletionSource {
    /// `commands` must be sorted + deduped (the shell's `refresh_command_index`
    /// guarantees it); command-position completion preserves that order so the
    /// menu reads alphabetically.
    pub fn new(commands: Vec<String>) -> Self {
        Self { commands }
    }

    /// Command-position completion: the known names extending `token`, each
    /// terminated with a space so a unique pick lands ready for the next word.
    fn complete_command(&self, token: &str, start: usize, cursor: usize) -> Completions {
        let mut candidates: Vec<String> = Vec::new();
        for c in &self.commands {
            if c.starts_with(token) {
                let mut s = c.clone();
                s.push(' ');
                candidates.push(s);
                if candidates.len() >= MAX_CANDIDATES {
                    break;
                }
            }
        }
        Completions {
            replace_range: start..cursor,
            candidates,
        }
    }
}

impl CompletionSource for ShellCompletionSource {
    fn complete(&self, buffer: &str, cursor: usize) -> Completions {
        let start = word_start(buffer, cursor);
        let token = &buffer[start..cursor];

        // A bare name in command position completes against the command index;
        // a name containing '/' (a command-by-path) falls through to path
        // completion, like `resolve_command`'s "used as-is" branch.
        if is_command_position(buffer, start) && !token.contains('/') {
            return self.complete_command(token, start, cursor);
        }

        // Argument / command-by-path -> path completion. `cd` takes only dirs.
        let dirs_only = first_token(buffer) == "cd";
        complete_path(token, start, cursor, dirs_only)
    }
}

/// The byte index of the start of the word the cursor is in: just past the last
/// whitespace before the cursor, or 0. Mirrors the engine's word boundary so a
/// completion replaces exactly the current token.
fn word_start(buffer: &str, cursor: usize) -> usize {
    buffer[..cursor]
        .rfind(|c: char| c.is_whitespace())
        .map(|i| i + 1)
        .unwrap_or(0)
}

/// Whether the word starting at `word_start` is in command position: nothing
/// but whitespace precedes it, or the preceding non-space text ends with a
/// command-introducing operator. v1.0 heuristic -- `&&` / `||` end in `&` / `|`
/// so they are covered; a full parse of the command position is a v1.x refine.
fn is_command_position(buffer: &str, word_start: usize) -> bool {
    let before = buffer[..word_start].trim_end();
    before.is_empty()
        || before.ends_with('|')
        || before.ends_with(';')
        || before.ends_with('&')
        || before.ends_with('{')
        || before.ends_with('(')
}

/// The first whitespace-delimited token of the line (the command), for the
/// `cd`-completes-dirs-only special case. "" when the line is blank.
fn first_token(buffer: &str) -> &str {
    buffer.trim_start().split_whitespace().next().unwrap_or("")
}

/// Split a path token at its last `/` into (directory prefix INCLUDING the
/// trailing `/`, file-name prefix). A token with no `/` has an empty directory
/// prefix (the whole token is the file prefix, resolved against the cwd).
fn split_path_token(token: &str) -> (&str, &str) {
    match token.rfind('/') {
        Some(i) => (&token[..=i], &token[i + 1..]),
        None => ("", token),
    }
}

/// The path to `read_dir` for a directory prefix: the cwd (".") for an empty
/// prefix, the root for "/", else the prefix with its trailing slash stripped
/// (so "src/" reads "src" and "a/b/" reads "a/b" -- `stalk` resolves either,
/// but the stripped form is the canonical one).
fn readdir_target(dir_prefix: &str) -> &str {
    if dir_prefix.is_empty() {
        "."
    } else if dir_prefix == "/" {
        "/"
    } else {
        dir_prefix.trim_end_matches('/')
    }
}

/// Path completion: offer the entries of the token's directory whose names
/// extend its file prefix, each re-prefixed with the token's directory part and
/// terminated (`/` for a directory so it can be drilled, space otherwise). A
/// read failure (missing / unsearchable directory) yields no candidates -- Tab
/// is then simply inert, never an error.
fn complete_path(token: &str, start: usize, cursor: usize, dirs_only: bool) -> Completions {
    let (dir_prefix, file_prefix) = split_path_token(token);
    let mut candidates: Vec<String> = Vec::new();
    if let Ok(rd) = libthyla_rs::fs::read_dir(readdir_target(dir_prefix)) {
        for ent in rd.flatten() {
            let name = ent.file_name();
            if !name.starts_with(file_prefix) {
                continue;
            }
            // Hide dotfiles unless the user explicitly typed a leading '.'.
            if file_prefix.is_empty() && name.starts_with('.') {
                continue;
            }
            let is_dir = ent.is_dir();
            if dirs_only && !is_dir {
                continue;
            }
            let mut cand = String::with_capacity(dir_prefix.len() + name.len() + 1);
            cand.push_str(dir_prefix);
            cand.push_str(name);
            cand.push(if is_dir { '/' } else { ' ' });
            candidates.push(cand);
            if candidates.len() >= MAX_CANDIDATES {
                break;
            }
        }
    }
    // read_dir order is FS-defined; sort so the menu + LCP are deterministic.
    candidates.sort();
    Completions {
        replace_range: start..cursor,
        candidates,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn src(names: &[&str]) -> ShellCompletionSource {
        let mut v: Vec<String> = names.iter().map(|s| String::from(*s)).collect();
        v.sort();
        v.dedup();
        ShellCompletionSource::new(v)
    }

    #[test]
    fn command_position_completes_from_index() {
        let s = src(&["cat", "cd", "cp", "ls"]);
        let c = s.complete("c", 1);
        assert_eq!(c.replace_range, 0..1);
        // Sorted, each terminated with a trailing space.
        assert_eq!(c.candidates, vec!["cat ", "cd ", "cp "]);
    }

    #[test]
    fn command_unique_match_terminates_with_space() {
        let s = src(&["whoami", "ls"]);
        let c = s.complete("who", 3);
        assert_eq!(c.candidates, vec!["whoami "]);
    }

    #[test]
    fn command_no_match_is_empty() {
        let s = src(&["ls", "cat"]);
        let c = s.complete("zzz", 3);
        assert!(c.candidates.is_empty());
    }

    #[test]
    fn empty_command_token_lists_all() {
        let s = src(&["ls", "cat"]);
        let c = s.complete("", 0);
        assert_eq!(c.candidates, vec!["cat ", "ls "]);
    }

    #[test]
    fn command_token_with_slash_is_not_command_completion() {
        // `./scr` is a command-by-path -> path completion, NOT the index.
        // The index has no entry starting "./scr"; path completion will
        // readdir "." (no syscall on host -> empty), so candidates are empty
        // and -- crucially -- NOT filtered from the command index.
        let s = src(&["script-in-index"]);
        let c = s.complete("./scr", 5);
        assert!(c.candidates.is_empty());
        assert_eq!(c.replace_range, 0..5);
    }

    #[test]
    fn classify_command_position() {
        assert!(is_command_position("ls", 0)); // start of line
        assert!(is_command_position("  ls", 2)); // leading whitespace
        assert!(is_command_position("foo | ls", 6)); // after a pipe
        assert!(is_command_position("foo; ls", 5)); // after a semicolon
        assert!(is_command_position("a && ls", 5)); // && ends in &
        assert!(is_command_position("{ ls", 2)); // after a brace
        assert!(!is_command_position("ls foo", 3)); // an argument
        assert!(!is_command_position("cat a b", 6)); // a later argument
    }

    #[test]
    fn first_token_picks_the_command() {
        assert_eq!(first_token("cd /home"), "cd");
        assert_eq!(first_token("   ls -la"), "ls");
        assert_eq!(first_token(""), "");
        assert_eq!(first_token("   "), "");
    }

    #[test]
    fn split_path_token_at_last_slash() {
        assert_eq!(split_path_token("foo"), ("", "foo"));
        assert_eq!(split_path_token("src/fo"), ("src/", "fo"));
        assert_eq!(split_path_token("a/b/c"), ("a/b/", "c"));
        assert_eq!(split_path_token("/etc/pa"), ("/etc/", "pa"));
        assert_eq!(split_path_token("dir/"), ("dir/", ""));
        assert_eq!(split_path_token("/"), ("/", ""));
    }

    #[test]
    fn readdir_target_canonicalizes() {
        assert_eq!(readdir_target(""), ".");
        assert_eq!(readdir_target("/"), "/");
        assert_eq!(readdir_target("src/"), "src");
        assert_eq!(readdir_target("a/b/"), "a/b");
        assert_eq!(readdir_target("/etc/"), "/etc");
    }
}
