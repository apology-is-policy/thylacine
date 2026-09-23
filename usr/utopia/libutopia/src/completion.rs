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
// Every match is read and counted, but at most `MAX_CANDIDATES` are held: the
// first that many alphabetically, which is what the menu lists. When more
// exist the source says so (`Extent::Truncated`) and reports the prefix EVERY
// match shares, because the engine extends the line to the common prefix of
// what it is handed, and a subset's can run past matches the subset left out.
// That is the zsh model the menu was designed on (`LISTMAX` caps what is
// shown, never what is matched); capping the matching would let Tab extend the
// line past valid completions in any directory of more than 256 matches.
//
// Per the Plan 9 native split + UTOPIA-SHELL-DESIGN.md section 11.2: pure
// userspace logic over libthyla-rs `fs::read_dir` (already audited, RW-8); the
// audit-bearing raw-mode editor + consctl surface this rides on was discharged
// at the Kaua T-4 audit (#101). The classification + path-split are pure, and
// the one syscall -- the directory read, taken solely on Tab -- goes through a
// `ListDir`, so the tests below drive path completion over a fixed tree while
// the shell hands in the live filesystem.

use alloc::collections::BinaryHeap;
use alloc::string::String;
use alloc::vec::Vec;
use core::ops::Range;

use crate::line_editor::{longest_common_prefix, CompletionSource, Completions, Extent};

/// Cap on the matches one Tab holds and lists (UT-NORA-ERGONOMICS.md's
/// `LISTMAX`-ish "show N + ... M more"). A directory of thousands must flood
/// neither the menu nor the shell's heap, which a `no_std` program cannot
/// overrun and survive. It bounds what is HELD; every match is still read and
/// counted.
const MAX_CANDIDATES: usize = 256;

/// How path completion reads a directory: `visit(name, is_dir)` once per entry.
/// An unreadable directory visits nothing. It STREAMS rather than returning the
/// listing so the source holds at most `MAX_CANDIDATES` names however large the
/// directory is; a returned listing would hold all of them.
pub(crate) type ListDir = fn(dir: &str, visit: &mut dyn FnMut(&str, bool));

/// The live `ListDir`: libthyla-rs `fs::read_dir`. An entry the read cannot
/// return is skipped, not fatal -- a partial menu beats none -- and a failed
/// refill ends the iteration, so the loop terminates.
#[cfg(feature = "backend")]
fn read_dir_live(dir: &str, visit: &mut dyn FnMut(&str, bool)) {
    if let Ok(rd) = libthyla_rs::fs::read_dir(dir) {
        for ent in rd.flatten() {
            visit(ent.file_name(), ent.is_dir());
        }
    }
}

/// One Tab's matches under the cap: the first `MAX_CANDIDATES` alphabetically,
/// a count of the rest, and the greatest match. The longest common prefix of a
/// set is that of its least and greatest members, and the least is always
/// kept, so those two give the prefix every match shares without holding the
/// matches in between.
struct Gather {
    /// The least matches seen, as a max-heap so the greatest of them is the
    /// one a smaller newcomer displaces.
    kept: BinaryHeap<String>,
    unlisted: usize,
    greatest: String,
}

impl Gather {
    fn new() -> Self {
        Self {
            kept: BinaryHeap::new(),
            unlisted: 0,
            greatest: String::new(),
        }
    }

    /// Count `cand` as a match. It is copied only if it is kept.
    fn offer(&mut self, cand: &str) {
        if cand > self.greatest.as_str() {
            self.greatest.clear();
            self.greatest.push_str(cand);
        }
        if self.kept.len() < MAX_CANDIDATES {
            self.kept.push(String::from(cand));
            return;
        }
        // Full: one of `cand` and the greatest kept match goes unlisted.
        self.unlisted += 1;
        if let Some(mut top) = self.kept.peek_mut() {
            if cand < top.as_str() {
                top.clear();
                top.push_str(cand);
            }
        }
    }

    fn finish(self, replace_range: Range<usize>) -> Completions {
        let candidates = self.kept.into_sorted_vec();
        let extent = match candidates.first() {
            Some(least) if self.unlisted > 0 => Extent::Truncated {
                unlisted: self.unlisted,
                shared: longest_common_prefix(&[least.as_str(), self.greatest.as_str()]),
            },
            _ => Extent::Complete,
        };
        Completions {
            replace_range,
            candidates,
            extent,
        }
    }
}

/// The production Tab-completion source. Owns a precomputed command index
/// (builtins + aliases + funcs + `/bin`, sorted + deduped) for command-position
/// completion; path completion reads directories on demand through `list_dir`.
pub struct ShellCompletionSource {
    /// Known command names, sorted + deduped. Also the artifact #115c coloring
    /// consults (`LineEditor::set_known_commands`), built once by the shell.
    commands: Vec<String>,
    list_dir: ListDir,
}

impl ShellCompletionSource {
    /// `commands` must be sorted + deduped (the shell's `refresh_command_index`
    /// guarantees it); command-position completion preserves that order so the
    /// menu reads alphabetically. Paths complete against the live filesystem.
    #[cfg(feature = "backend")]
    pub fn new(commands: Vec<String>) -> Self {
        Self::with_dir_lister(commands, read_dir_live)
    }

    /// The same source, reading directories through `list_dir`. Only `new`
    /// and the tests construct one.
    #[cfg(any(test, feature = "backend"))]
    pub(crate) fn with_dir_lister(commands: Vec<String>, list_dir: ListDir) -> Self {
        Self { commands, list_dir }
    }

    /// Command-position completion: the known names extending `token`, each
    /// terminated with a space so a unique pick lands ready for the next word.
    /// Every name is offered even though the index is sorted: its first 256
    /// matches can share a longer prefix than all of them do.
    fn complete_command(&self, token: &str, start: usize, cursor: usize) -> Completions {
        let mut gather = Gather::new();
        let mut cand = String::new();
        for c in self.commands.iter().filter(|c| c.starts_with(token)) {
            cand.clear();
            cand.push_str(c);
            cand.push(' ');
            gather.offer(&cand);
        }
        gather.finish(start..cursor)
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
        complete_path(self.list_dir, token, start, cursor, dirs_only)
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
fn complete_path(
    list_dir: ListDir,
    token: &str,
    start: usize,
    cursor: usize,
    dirs_only: bool,
) -> Completions {
    let (dir_prefix, file_prefix) = split_path_token(token);
    let mut gather = Gather::new();
    let mut cand = String::new();
    list_dir(readdir_target(dir_prefix), &mut |name, is_dir| {
        if !name.starts_with(file_prefix) {
            return;
        }
        // Hide dotfiles unless the user explicitly typed a leading '.'.
        if file_prefix.is_empty() && name.starts_with('.') {
            return;
        }
        if dirs_only && !is_dir {
            return;
        }
        cand.clear();
        cand.push_str(dir_prefix);
        cand.push_str(name);
        cand.push(if is_dir { '/' } else { ' ' });
        gather.offer(&cand);
    });
    // Read order is the filesystem's; which matches are kept, and the menu's
    // order, depend only on the names.
    gather.finish(start..cursor)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::line_editor::{EditorAction, LineEditor};
    use alloc::boxed::Box;
    use alloc::vec;
    use core::fmt::Write;
    use core::sync::atomic::{AtomicUsize, Ordering};

    /// A fixed tree standing in for the filesystem:
    ///   .      script  src/  .hidden
    ///   src    main.rs  mod/
    ///   /      bin/  etc/
    /// Any other directory is unreadable.
    fn tree(dir: &str, visit: &mut dyn FnMut(&str, bool)) {
        let entries: &[(&str, bool)] = match dir {
            "." => &[("script", false), ("src", true), (".hidden", false)],
            "src" => &[("main.rs", false), ("mod", true)],
            "/" => &[("bin", true), ("etc", true)],
            _ => &[],
        };
        for &(name, is_dir) in entries {
            visit(name, is_dir);
        }
    }

    fn src(names: &[&str]) -> ShellCompletionSource {
        let mut v: Vec<String> = names.iter().map(|s| String::from(*s)).collect();
        v.sort();
        v.dedup();
        ShellCompletionSource::with_dir_lister(v, tree)
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
        // `./scr` is a command-by-path -> PATH completion, not the index. The
        // assertion is positive on purpose: the index cannot hold a name with
        // a '/', so an empty result would pass whichever route the token took.
        // `script` can only have come from the directory.
        let s = src(&["scrap"]);
        let c = s.complete("./scr", 5);
        assert_eq!(c.candidates, vec!["./script "]);
        assert_eq!(c.replace_range, 0..5);
        // The same prefix without the slash is command position: the index.
        assert_eq!(s.complete("scr", 3).candidates, vec!["scrap "]);
    }

    #[test]
    fn argument_position_completes_paths_with_terminators() {
        let s = src(&["cat"]);
        let c = s.complete("cat s", 5);
        assert_eq!(c.replace_range, 4..5);
        // A file ends in a space, a directory in a slash; sorted.
        assert_eq!(c.candidates, vec!["script ", "src/"]);
    }

    #[test]
    fn cd_completes_directories_only() {
        let s = src(&["cd"]);
        assert_eq!(s.complete("cd s", 4).candidates, vec!["src/"]);
    }

    #[test]
    fn dotfiles_hide_until_a_dot_is_typed() {
        let s = src(&["cat"]);
        assert_eq!(s.complete("cat ", 4).candidates, vec!["script ", "src/"]);
        assert_eq!(s.complete("cat .", 5).candidates, vec![".hidden "]);
    }

    #[test]
    fn a_directory_prefix_is_kept_on_every_candidate() {
        let s = src(&["cat"]);
        assert_eq!(
            s.complete("cat src/m", 9).candidates,
            vec!["src/main.rs ", "src/mod/"]
        );
        assert_eq!(s.complete("cat /e", 6).candidates, vec!["/etc/"]);
    }

    #[test]
    fn an_unreadable_directory_completes_to_nothing() {
        let s = src(&["cat"]);
        assert!(s.complete("cat nope/x", 10).candidates.is_empty());
    }

    /// Entries this lister has handed out, across the whole test binary --
    /// only `the_cap_bounds_what_is_held_not_what_is_read` uses it.
    static MANY_VISITED: AtomicUsize = AtomicUsize::new(0);

    fn many(_dir: &str, visit: &mut dyn FnMut(&str, bool)) {
        let mut name = String::new();
        for i in 0..MAX_CANDIDATES + 44 {
            name.clear();
            let _ = write!(name, "f{:03}", i);
            MANY_VISITED.fetch_add(1, Ordering::Relaxed);
            visit(&name, false);
        }
    }

    #[test]
    fn the_cap_bounds_what_is_held_not_what_is_read() {
        let s = ShellCompletionSource::with_dir_lister(Vec::new(), many);
        let c = s.complete("cat f", 5);
        // Every entry is read, so all 300 are counted ...
        assert_eq!(MANY_VISITED.load(Ordering::Relaxed), MAX_CANDIDATES + 44);
        // ... and the first 256 alphabetically are held.
        assert_eq!(c.candidates.len(), MAX_CANDIDATES);
        assert_eq!(c.candidates[0], "f000 ");
        assert_eq!(c.candidates[MAX_CANDIDATES - 1], "f255 ");
        assert_eq!(
            c.extent,
            Extent::Truncated {
                unlisted: 44,
                shared: String::from("f"),
            }
        );
    }

    /// 300 entries, all matching "f": 256 `fa…` followed by 44 `fb…`. The
    /// first 256 share "fa"; the whole directory shares only "f".
    fn fa_then_fb(_dir: &str, visit: &mut dyn FnMut(&str, bool)) {
        runs(&[("fa", MAX_CANDIDATES), ("fb", 44)], visit);
    }

    /// The same 300 entries, read in the other order.
    fn fb_then_fa(_dir: &str, visit: &mut dyn FnMut(&str, bool)) {
        runs(&[("fb", 44), ("fa", MAX_CANDIDATES)], visit);
    }

    /// 300 entries that all share "fab".
    fn fab_300(_dir: &str, visit: &mut dyn FnMut(&str, bool)) {
        runs(&[("fab", MAX_CANDIDATES + 44)], visit);
    }

    /// `lead000`, `lead001`, ... for each (lead, count), in that order.
    fn runs(spec: &[(&str, usize)], visit: &mut dyn FnMut(&str, bool)) {
        let mut name = String::new();
        for &(lead, n) in spec {
            for i in 0..n {
                name.clear();
                let _ = write!(name, "{}{:03}", lead, i);
                visit(&name, false);
            }
        }
    }

    /// `(selected, unlisted)` when `r` is a `MenuShow`.
    fn menu_at(r: &EditorAction) -> Option<(usize, usize)> {
        match r {
            EditorAction::MenuShow {
                selected, unlisted, ..
            } => Some((*selected, *unlisted)),
            _ => None,
        }
    }

    fn tab(src: ShellCompletionSource, typed: &str) -> (EditorAction, String) {
        let mut le = LineEditor::new();
        le.set_completion_source(Box::new(src));
        let _ = le.feed_bytes(typed.as_bytes());
        let r = le.feed_byte(0x09);
        (r, String::from(le.buffer()))
    }

    #[test]
    fn tab_never_extends_past_a_match_the_cap_left_out() {
        let s = ShellCompletionSource::with_dir_lister(Vec::new(), fa_then_fb);
        let (r, line) = tab(s, "cat f");
        // Every match begins "f" and no longer prefix: nothing to extend, so
        // Tab opens the menu on the first match.
        assert_eq!(menu_at(&r), Some((0, 44)), "{:?}", r);
        assert_eq!(line, "cat fa000 ");
    }

    #[test]
    fn a_truncated_directory_still_extends_to_what_every_entry_shares() {
        // The control for the test above: the cap must not stop Tab extending.
        let s = ShellCompletionSource::with_dir_lister(Vec::new(), fab_300);
        let (r, line) = tab(s, "cat f");
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(line, "cat fab");
    }

    #[test]
    fn which_entries_are_kept_does_not_depend_on_read_order() {
        let a = ShellCompletionSource::with_dir_lister(Vec::new(), fa_then_fb).complete("cat f", 5);
        let b = ShellCompletionSource::with_dir_lister(Vec::new(), fb_then_fa).complete("cat f", 5);
        assert_eq!(a, b);
        assert_eq!(a.candidates[0], "fa000 ");
        assert_eq!(a.candidates[MAX_CANDIDATES - 1], "fa255 ");
    }

    /// The gatherer against the obvious implementation -- hold every match,
    /// sort, take the head, take the common prefix of all -- over a set far
    /// larger than the cap, offered in a scrambled order. The least and
    /// greatest names differ inside a two-byte character (`è` and `é` share
    /// their lead byte), so the shared prefix must be cut back to a character
    /// boundary; uncut, the slice would panic.
    #[test]
    fn gather_matches_holding_everything_and_sorting() {
        let mut all: Vec<String> = Vec::new();
        for i in 0..1000u32 {
            let mut s = String::new();
            let accent = if i % 7 == 0 { "\u{e9}" } else { "\u{e8}" };
            let _ = write!(s, "k/{}{}{}-{}", accent, i % 3, i % 5, i);
            all.push(s);
        }
        // A fixed LCG permutation: deterministic, far from sorted.
        let mut order: Vec<usize> = (0..all.len()).collect();
        let mut x: u32 = 12345;
        for i in (1..order.len()).rev() {
            x = x.wrapping_mul(1_103_515_245).wrapping_add(12_345);
            order.swap(i, (x as usize >> 8) % (i + 1));
        }
        let mut g = Gather::new();
        for &i in &order {
            g.offer(&all[i]);
        }
        let got = g.finish(0..0);
        let mut sorted = all.clone();
        sorted.sort();
        assert_eq!(got.candidates, sorted[..MAX_CANDIDATES]);
        assert_eq!(
            got.extent,
            Extent::Truncated {
                unlisted: all.len() - MAX_CANDIDATES,
                shared: longest_common_prefix(&all),
            }
        );
        // And at or under the cap it is exactly the sorted set, complete.
        let mut g = Gather::new();
        for s in sorted[..MAX_CANDIDATES].iter().rev() {
            g.offer(s);
        }
        let got = g.finish(0..0);
        assert_eq!(got.candidates, sorted[..MAX_CANDIDATES]);
        assert_eq!(got.extent, Extent::Complete);
    }

    #[test]
    fn a_sorted_command_index_is_not_safe_either() {
        // The first 256 of the sorted index all begin "aa"; `ab` sorts after.
        let mut v: Vec<String> = (0..300)
            .map(|i| {
                let mut s = String::new();
                let _ = write!(s, "aa{:03}", i);
                s
            })
            .collect();
        v.push(String::from("ab"));
        let s = ShellCompletionSource::with_dir_lister(v, tree);
        let (r, line) = tab(s, "a");
        assert_eq!(menu_at(&r), Some((0, 45)), "{:?}", r);
        assert_eq!(line, "aa000 ");
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
