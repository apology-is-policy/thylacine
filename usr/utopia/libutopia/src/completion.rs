// libutopia::completion -- the namespace-driven Tab completion source (#115a).
//
// The U-4d line editor (`line_editor.rs`) ships the completion ENGINE -- the
// `CompletionSource` trait, the shared-prefix extension, the single-vs-menu
// dispatch -- but no real source: a bare `ut` left Tab inert because the only
// source was the test-only `StaticCompletionSource`. This module is the
// production source the shell installs (`Repl::install_completion`).
//
// Two completion contexts, classified from the word under the cursor:
//
//   - COMMAND position (the first word of a command: at the start, or after a
//     `| ; & { (` or a newline) with a name holding no `/` -> complete against
//     the COMMAND INDEX: builtins + aliases + funcs + the `/bin` scan (the #58
//     exec namespace, the same set `resolve_command` searches). The index is
//     precomputed by the shell (it owns the alias / func tables + can readdir
//     `/bin`) and handed to this source.
//
//   - ARGUMENT position (any later word), OR a command-by-path (a name holding
//     a `/`, like `./script` or `/bin/foo`) -> PATH completion: split the name
//     at its last `/` into a directory prefix + a file prefix, `read_dir` the
//     directory live, and offer the entries whose name extends the file
//     prefix. `cd <TAB>` restricts to directories (the only sensible `cd`
//     target). A relative directory resolves against the per-Proc cwd (LS-4
//     `SYS_CHDIR`, which `cd` keeps synced), so `read_dir(".")` is right.
//
// Each candidate carries its terminator -- a trailing space for a command or
// regular file, a trailing `/` for a directory -- so a unique completion lands
// ready for the next token (and a directory can be drilled with a second Tab).
// This is the readline convention.
//
// === Reading the word, and writing it back ===
//
// A name is matched as the shell will READ the word and inserted as the shell
// must SPELL it. `word_at` reads the word under the cursor with the lexer's own
// rules -- its word characters, escapes, quotes, comments, and the nesting of
// `$(`, `{` and `(` -- and removes the quoting to give the text a name must
// begin with. A word the lexer would not read as one literal piece completes to
// nothing rather than to a guess: a `$var`, a double quote holding one, a
// comment, and a word glued to what precedes it, such as `$home/fo` -- two
// words in ut, where only `^`, `~` and `=` join adjacent pieces.
//
// `quote_word` then spells each match as rc and Plan 9's `%q` do: whole, and
// only when it needs it, in single quotes with `''` for a quote -- unless the
// user opened a double quote, whose escapes it continues. A directory is left
// open (`'my dir/`), as bash leaves it, because `'my dir'/` would be two words.
// Single quotes rather than backslashes: they are the literal form scripture
// documents (UTOPIA-SHELL-DESIGN.md 6.4), and an escaped glob character still
// globs.
//
// The shared prefix is taken from the NAMES and then spelled -- readline's
// order. Taken from the spelled forms it could end inside the quoting, and a
// quoted name and a bare one share no prefix at all.
//
// A name holding a control character other than tab or newline has no
// spelling: a single-quoted string would carry the raw byte, and the editor
// draws its line verbatim, so the byte would reach the terminal -- under
// Halcyon, a stream whose escape frames are parsed. Such a match is counted but
// never listed or inserted, and the shared prefix stops before one. Tab and
// newline are spelled in double quotes, as `\t` and `\n`.
//
// Every match is read and counted, but at most `MAX_CANDIDATES` are held: the
// first that many alphabetically, which is what the menu lists. The rest are
// counted as unlisted, and the extension is the prefix EVERY match shares --
// the held ones' can run past matches the cap left out. That is the zsh model
// the menu was designed on (`LISTMAX` caps what is shown, never what is
// matched); capping the matching would let Tab extend the line past valid
// completions in any directory of more than 256 matches.
//
// Per the Plan 9 native split + UTOPIA-SHELL-DESIGN.md section 11.2: pure
// userspace logic over libthyla-rs `fs::read_dir` (already audited, RW-8); the
// audit-bearing raw-mode editor + consctl surface this rides on was discharged
// at the Kaua T-4 audit (#101). The reading, the quoting and the path split
// are pure, and the one syscall -- the directory read, taken solely on Tab --
// goes through a `ListDir`, so the tests below drive path completion over a
// fixed tree while the shell hands in the live filesystem.

use alloc::collections::BinaryHeap;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use crate::eval::glob::has_meta;
use crate::line_editor::{longest_common_prefix, CompletionSource, Completions};
use crate::parser::lexer::{is_var_name_byte, is_var_name_start_byte, is_word_char_byte};
use crate::parser::TokenKind;

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

/// One Tab's matches under the cap. `kept` holds the least `MAX_CANDIDATES`
/// that can be spelled; `least` and `greatest` range over EVERY match, because
/// the longest common prefix of a set is that of its least and greatest
/// members, so those two give what all of them share without holding the
/// matches in between.
struct Gather {
    /// The least spellable matches seen, as (name, is a directory), in a
    /// max-heap so the greatest of them is the one a smaller newcomer
    /// displaces.
    kept: BinaryHeap<(String, bool)>,
    unlisted: usize,
    least: Option<String>,
    greatest: String,
}

impl Gather {
    fn new() -> Self {
        Self {
            kept: BinaryHeap::new(),
            unlisted: 0,
            least: None,
            greatest: String::new(),
        }
    }

    /// Count `name` as a match. It is copied only if it is kept.
    fn offer(&mut self, name: &str, is_dir: bool) {
        match &mut self.least {
            Some(least) if name >= least.as_str() => {}
            Some(least) => {
                least.clear();
                least.push_str(name);
            }
            None => self.least = Some(String::from(name)),
        }
        if name > self.greatest.as_str() {
            self.greatest.clear();
            self.greatest.push_str(name);
        }
        if !spellable(name) {
            self.unlisted += 1;
            return;
        }
        if self.kept.len() < MAX_CANDIDATES {
            self.kept.push((String::from(name), is_dir));
            return;
        }
        // Full: one of `name` and the greatest kept match goes unlisted.
        self.unlisted += 1;
        if let Some(mut top) = self.kept.peek_mut() {
            if name < top.0.as_str() {
                top.0.clear();
                top.0.push_str(name);
                top.1 = is_dir;
            }
        }
    }

    /// The listing, and the extension, spelled to replace `word`, which ends
    /// at `end`. `naming` says the matches are command names.
    fn finish(self, word: &WordAt, end: usize, naming: bool) -> Completions {
        let mut unlisted = self.unlisted;
        let mut candidates = Vec::with_capacity(self.kept.len());
        for (name, is_dir) in self.kept.into_sorted_vec() {
            // A command named like a keyword runs only quoted: bare, the
            // parser reads the keyword.
            let quote = match word.quote {
                Quote::Bare if naming && TokenKind::reserved_word(&name).is_some() => Quote::Single,
                q => q,
            };
            match spell(&name, is_dir, quote) {
                Some(w) => candidates.push(w),
                None => unlisted += 1,
            }
        }
        let greatest = self.greatest;
        let extension = self.least.and_then(|least| {
            let mut shared = longest_common_prefix(&[least.as_str(), greatest.as_str()]);
            if let Some(i) = shared.find(unspellable) {
                shared.truncate(i);
            }
            if shared.len() > word.text.len() {
                quote_word(&shared, word.quote, false)
            } else {
                None
            }
        });
        Completions {
            replace_range: word.start..end,
            candidates,
            extension,
            unlisted,
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
    /// guarantees it). Paths complete against the live filesystem.
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
}

impl CompletionSource for ShellCompletionSource {
    fn complete(&self, buffer: &str, cursor: usize) -> Completions {
        let word = match word_at(buffer, cursor) {
            Some(w) => w,
            None => {
                return Completions {
                    replace_range: cursor..cursor,
                    ..Completions::default()
                }
            }
        };
        let mut gather = Gather::new();
        // A bare name in command position completes against the command index;
        // a name holding a '/' (a command-by-path) is a path, like
        // `resolve_command`'s "used as-is" branch. Every name in the index is
        // offered even though it is sorted: its first 256 matches can share a
        // longer prefix than all of them do.
        let naming = word.command_position && !word.text.contains('/');
        if naming {
            for c in self
                .commands
                .iter()
                .filter(|c| c.starts_with(word.text.as_str()))
            {
                gather.offer(c, false);
            }
        } else {
            let dirs_only = word.command.as_deref() == Some("cd");
            complete_path(self.list_dir, &word.text, dirs_only, &mut gather);
        }
        gather.finish(&word, cursor, naming)
    }
}

// -----------------------------------------------------------------------------
// Spelling a name as a word
// -----------------------------------------------------------------------------

/// How a word is quoted: how the user began it, and so how its completion is
/// written.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Quote {
    /// Unquoted, backslash escapes and all: a completion is written plain
    /// when every character can be, else single-quoted.
    Bare,
    Single,
    Double,
}

/// A character no quoting spells: a control character other than tab and
/// newline. Inside single quotes it stays raw, and the editor draws its line
/// verbatim.
fn unspellable(c: char) -> bool {
    c.is_control() && c != '\t' && c != '\n'
}

fn spellable(text: &str) -> bool {
    !text.contains(unspellable)
}

/// A character a bare word carries as itself wherever it stands: one of the
/// lexer's word characters. Its glob metas are word characters too, which
/// is why `quote_word` also asks the glob matcher's own `has_meta`.
fn plain(c: char) -> bool {
    if c.is_ascii() {
        is_word_char_byte(c as u8)
    } else {
        !c.is_control()
    }
}

/// `text` spelled as a word the lexer reads back as exactly `text`: as it is,
/// when `quote` is `Bare` and every character is plain; else in the quotes the
/// user opened, or in single quotes -- save that a tab or newline takes double
/// quotes, the only ones with escapes for them. `close` ends the quoting; a
/// shared prefix and a directory are left open, since the user goes on inside
/// the word. `None` when `text` has no spelling.
fn quote_word(text: &str, quote: Quote, close: bool) -> Option<String> {
    if !spellable(text) {
        return None;
    }
    let style = match quote {
        Quote::Bare if text.chars().all(plain) && !has_meta(text) => {
            return Some(String::from(text))
        }
        Quote::Double => Quote::Double,
        _ if text.contains(['\t', '\n']) => Quote::Double,
        _ => Quote::Single,
    };
    let mut out = String::with_capacity(text.len() + 2);
    if style == Quote::Single {
        out.push('\'');
        for c in text.chars() {
            if c == '\'' {
                out.push_str("''");
            } else {
                out.push(c);
            }
        }
        if close {
            out.push('\'');
        }
    } else {
        out.push('"');
        for c in text.chars() {
            match c {
                '\\' => out.push_str("\\\\"),
                '"' => out.push_str("\\\""),
                '$' => out.push_str("\\$"),
                '\t' => out.push_str("\\t"),
                '\n' => out.push_str("\\n"),
                _ => out.push(c),
            }
        }
        if close {
            out.push('"');
        }
    }
    Some(out)
}

/// A match spelled for the line: a directory left open with its `/` inside the
/// word, anything else closed and followed by a space.
fn spell(name: &str, is_dir: bool, quote: Quote) -> Option<String> {
    if is_dir {
        let mut path = String::with_capacity(name.len() + 1);
        path.push_str(name);
        path.push('/');
        quote_word(&path, quote, false)
    } else {
        let mut word = quote_word(name, quote, true)?;
        word.push(' ');
        Some(word)
    }
}

// -----------------------------------------------------------------------------
// Path completion
// -----------------------------------------------------------------------------

/// Split a path at its last `/` into (directory prefix INCLUDING the trailing
/// `/`, file-name prefix). A path with no `/` has an empty directory prefix
/// (the whole of it is the file prefix, resolved against the cwd).
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

/// Path completion: offer the entries of `path`'s directory whose names extend
/// its file prefix, each re-prefixed with its directory part. A read failure
/// (missing / unsearchable directory) offers nothing -- Tab is then simply
/// inert, never an error.
fn complete_path(list_dir: ListDir, path: &str, dirs_only: bool, gather: &mut Gather) {
    let (dir_prefix, file_prefix) = split_path_token(path);
    let mut name = String::new();
    list_dir(readdir_target(dir_prefix), &mut |entry, is_dir| {
        if !entry.starts_with(file_prefix) {
            return;
        }
        // Hide dotfiles unless the user explicitly typed a leading '.'.
        if file_prefix.is_empty() && entry.starts_with('.') {
            return;
        }
        if dirs_only && !is_dir {
            return;
        }
        name.clear();
        name.push_str(dir_prefix);
        name.push_str(entry);
        gather.offer(&name, is_dir);
    });
}

// -----------------------------------------------------------------------------
// Reading the word under the cursor
// -----------------------------------------------------------------------------

/// The word the cursor ends, as the lexer will read it.
#[derive(Debug, PartialEq, Eq)]
struct WordAt {
    /// Where the word begins: a completion replaces from here to the cursor.
    start: usize,
    /// Its text so far, quotes and escapes removed: what a name must begin
    /// with to complete it.
    text: String,
    /// How it is quoted, and so how a completion of it is written.
    quote: Quote,
    /// It is the first word of a command.
    command_position: bool,
    /// The command it is an argument to, when that command's name is literal.
    command: Option<String>,
}

/// What directly precedes the scan position.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum After {
    /// A blank, a newline, the start, or an operator after which a word stands
    /// alone (`| ; & ( { < > =` and their kin).
    Break,
    /// Something a word starting here would be glued to or cannot follow: a
    /// value (a word, a quote, a `$var`), `^`, `~`, `?`, `!`, `)` or `}`.
    Joined,
}

/// One level of code: the line, or the body of a `(`, `$(`, `{`, `` `{ ``,
/// `<(`, `>(` or `((` still open.
struct Level {
    /// The byte that closes it; 0 for the line.
    closer: u8,
    /// Arithmetic `((`, which `))` closes as one token.
    arith: bool,
    /// The next word begins a command.
    command_next: bool,
    /// The current command's name, when literal.
    command: Option<String>,
    /// The next word is a redirect's target, never a command.
    target_next: bool,
    /// The next token may be a `/regex/` (it follows `=~`).
    regex_next: bool,
    after: After,
}

impl Level {
    fn new(closer: u8, arith: bool, commands: bool) -> Self {
        Self {
            closer,
            arith,
            command_next: commands,
            command: None,
            target_next: false,
            regex_next: false,
            after: After::Break,
        }
    }

    fn begin_command(&mut self) {
        self.command_next = true;
        self.command = None;
        self.target_next = false;
        self.after = After::Break;
    }

    /// A value just ended here, holding `literal` when it is one. A `(`, `$(`
    /// or the like opening here counts as one, not literal.
    fn value_done(&mut self, literal: Option<String>) {
        if self.target_next {
            self.target_next = false;
        } else if self.command_next {
            self.command = literal;
            self.command_next = false;
        }
        self.after = After::Joined;
    }

    /// The word beginning at `start` and running to the cursor.
    fn word(&self, start: usize, text: String, quote: Quote) -> WordAt {
        let command_position = self.command_next && !self.target_next;
        WordAt {
            start,
            text,
            quote,
            command_position,
            command: if command_position {
                None
            } else {
                self.command.clone()
            },
        }
    }
}

/// A double-quoted word being read.
struct Dq {
    /// The word as it began: its start, how it stood, and where.
    at: WordAt,
    /// It began alone rather than glued to what precedes it.
    alone: bool,
    /// Nothing in it expands so far, so its value is `at.text`.
    literal: bool,
}

enum Frame {
    Code(Level),
    Dq(Dq),
}

/// The char at byte `i` of `s` and its length in bytes.
fn char_at(s: &str, i: usize) -> (char, usize) {
    let c = s[i..].chars().next().unwrap_or('\u{fffd}');
    (c, c.len_utf8())
}

fn skip_var_name(b: &[u8], mut i: usize) -> usize {
    while b.get(i).is_some_and(|&c| is_var_name_byte(c)) {
        i += 1;
    }
    i
}

/// Past the tag of a heredoc whose `<<` ends before `i`; `None` when the tag
/// reaches the end of `b`, the cursor.
fn skip_heredoc_tag(b: &[u8], mut i: usize) -> Option<usize> {
    if b.get(i) == Some(&b'-') {
        i += 1;
    }
    while matches!(b.get(i), Some(b' ') | Some(b'\t')) {
        i += 1;
    }
    if b.get(i) == Some(&b'"') {
        i += 1;
        while *b.get(i)? != b'"' {
            i += 1;
        }
        i += 1;
    } else {
        i = skip_var_name(b, i);
    }
    (i < b.len()).then_some(i)
}

/// Past a `/regex/` whose opening `/` ends before `i`; `None` when it reaches
/// the cursor.
fn skip_regex(b: &[u8], mut i: usize) -> Option<usize> {
    loop {
        match *b.get(i)? {
            b'\n' => return Some(i),
            b'\\' if b.get(i + 1) == Some(&b'/') => i += 2,
            b'/' => return Some(i + 1),
            _ => i += 1,
        }
    }
}

/// The word the cursor ends, read with the lexer's rules over the line up to
/// the cursor. `None` when the cursor is somewhere a name does not complete:
/// in a comment, a `$var`, a heredoc tag or a regex, in a double quote that
/// expands, or in a word glued to what precedes it.
fn word_at(buffer: &str, cursor: usize) -> Option<WordAt> {
    let s = buffer.get(..cursor)?;
    let b = s.as_bytes();
    let end = b.len();
    let mut stack: Vec<Frame> = vec![Frame::Code(Level::new(0, false, true))];
    let mut i = 0;
    while i < end {
        if let Some(Frame::Dq(dq)) = stack.last_mut() {
            // Inside a double quote: its escapes, and `$` forms that expand.
            match b[i] {
                b'"' => {
                    i += 1;
                    let Some(Frame::Dq(dq)) = stack.pop() else {
                        return None;
                    };
                    if i >= end {
                        return (dq.alone && dq.literal).then_some(dq.at);
                    }
                    if let Some(Frame::Code(lv)) = stack.last_mut() {
                        lv.value_done(dq.literal.then_some(dq.at.text));
                    }
                }
                b'\\' => {
                    let escaped = match b.get(i + 1) {
                        Some(b'n') => Some('\n'),
                        Some(b't') => Some('\t'),
                        Some(&e @ (b'\\' | b'"' | b'$')) => Some(e as char),
                        _ => None,
                    };
                    match (b.get(i + 1), escaped) {
                        // An escape the cursor has not finished.
                        (None, _) => i += 1,
                        (Some(b'\n'), _) => i += 2,
                        (Some(_), Some(ch)) => {
                            dq.at.text.push(ch);
                            i += 2;
                        }
                        // Any other escape is kept whole, as the lexer keeps it.
                        (Some(_), None) => {
                            let (ch, n) = char_at(s, i + 1);
                            dq.at.text.push('\\');
                            dq.at.text.push(ch);
                            i += 1 + n;
                        }
                    }
                }
                b'$' => match b.get(i + 1) {
                    Some(b'(') => {
                        dq.literal = false;
                        i += 2;
                        stack.push(Frame::Code(Level::new(b')', false, true)));
                    }
                    Some(b'#') => {
                        dq.literal = false;
                        i = skip_var_name(b, i + 2);
                    }
                    Some(&n) if is_var_name_start_byte(n) => {
                        dq.literal = false;
                        i = skip_var_name(b, i + 1);
                    }
                    // A `$` that begins nothing is itself.
                    _ => {
                        dq.at.text.push('$');
                        i += 1;
                    }
                },
                _ => {
                    let (ch, n) = char_at(s, i);
                    dq.at.text.push(ch);
                    i += n;
                }
            }
            continue;
        }
        let Some(Frame::Code(lv)) = stack.last_mut() else {
            return None;
        };
        let c = b[i];
        let regex_next = core::mem::replace(&mut lv.regex_next, false);
        match c {
            b' ' | b'\t' => {
                i += 1;
                lv.after = After::Break;
                lv.regex_next = regex_next;
            }
            b'\\' if b.get(i + 1) == Some(&b'\n') => {
                i += 2;
                lv.after = After::Break;
                lv.regex_next = regex_next;
            }
            // A comment runs to the newline; one reaching the cursor holds it.
            b'#' => i += s[i..].find('\n')?,
            b'\n' | b';' => {
                i += 1;
                lv.begin_command();
            }
            b'|' | b'&' => {
                i += if b.get(i + 1) == Some(&c) { 2 } else { 1 };
                lv.begin_command();
            }
            b'?' if b.get(i + 1) == Some(&b'|') => {
                i += 2;
                lv.begin_command();
            }
            b'?' if !b.get(i + 1).is_some_and(|&n| is_word_char_byte(n)) => {
                i += 1;
                lv.after = After::Joined;
            }
            b'=' => match b.get(i + 1) {
                // `=>` begins a case arm's body.
                Some(b'>') => {
                    i += 2;
                    lv.begin_command();
                }
                Some(b'~') => {
                    i += 2;
                    lv.after = After::Break;
                    lv.regex_next = true;
                }
                Some(b'=') => {
                    i += 2;
                    lv.after = After::Break;
                }
                // `=` glues, but what follows it is a word of its own.
                _ => {
                    i += 1;
                    lv.after = After::Break;
                }
            },
            b'!' => {
                i += if b.get(i + 1) == Some(&b'=') { 2 } else { 1 };
                lv.after = After::Joined;
            }
            b'<' | b'>' => match b.get(i + 1) {
                Some(b'(') => {
                    i += 2;
                    lv.value_done(None);
                    stack.push(Frame::Code(Level::new(b')', false, true)));
                }
                Some(b'<') if c == b'<' => {
                    i = skip_heredoc_tag(b, i + 2)?;
                    lv.after = After::Joined;
                }
                Some(b'=') => {
                    i += 2;
                    lv.after = After::Break;
                }
                next => {
                    i += if c == b'>' && next == Some(&b'>') {
                        2
                    } else {
                        1
                    };
                    lv.target_next = true;
                    lv.after = After::Break;
                }
            },
            b'(' => {
                // A subshell where a command may begin; elsewhere -- after
                // `if`, `for`, a `$var` -- an expression's parentheses.
                let arith = b.get(i + 1) == Some(&b'(');
                let commands = !arith && lv.command_next && !lv.target_next;
                i += if arith { 2 } else { 1 };
                lv.value_done(None);
                stack.push(Frame::Code(Level::new(b')', arith, commands)));
            }
            b'{' => {
                i += 1;
                lv.value_done(None);
                stack.push(Frame::Code(Level::new(b'}', false, true)));
            }
            b'`' if b.get(i + 1) == Some(&b'{') => {
                i += 2;
                lv.value_done(None);
                stack.push(Frame::Code(Level::new(b'}', false, true)));
            }
            b')' | b'}' if lv.closer == c => {
                let both = lv.arith && b.get(i + 1) == Some(&b')');
                i += if both { 2 } else { 1 };
                stack.pop();
                match stack.last_mut() {
                    Some(Frame::Code(outer)) => outer.after = After::Joined,
                    Some(Frame::Dq(_)) => {}
                    None => return None,
                }
            }
            b'$' => match b.get(i + 1) {
                Some(b'(') => {
                    i += 2;
                    lv.value_done(None);
                    stack.push(Frame::Code(Level::new(b')', false, true)));
                }
                Some(b'#') | Some(b'"') => {
                    i = skip_var_name(b, i + 2);
                    if i >= end {
                        return None;
                    }
                    lv.value_done(None);
                }
                Some(&n) if is_var_name_start_byte(n) => {
                    i = skip_var_name(b, i + 1);
                    if i >= end {
                        return None;
                    }
                    lv.value_done(None);
                }
                _ => {
                    i += 1;
                    lv.after = After::Joined;
                }
            },
            b'~' => {
                i += 1;
                lv.value_done(None);
            }
            b'/' if regex_next => {
                i = skip_regex(b, i + 1)?;
                lv.value_done(None);
            }
            b'\'' => {
                let alone = lv.after == After::Break;
                let start = i;
                let mut text = String::new();
                i += 1;
                while i < end {
                    if b[i] == b'\'' {
                        if b.get(i + 1) == Some(&b'\'') {
                            text.push('\'');
                            i += 2;
                            continue;
                        }
                        i += 1;
                        break;
                    }
                    let (ch, n) = char_at(s, i);
                    text.push(ch);
                    i += n;
                }
                if i >= end {
                    return alone.then(|| lv.word(start, text, Quote::Single));
                }
                lv.value_done(Some(text));
            }
            b'"' => {
                let dq = Dq {
                    at: lv.word(i, String::new(), Quote::Double),
                    alone: lv.after == After::Break,
                    literal: true,
                };
                i += 1;
                stack.push(Frame::Dq(dq));
            }
            _ if is_word_char_byte(c) || c == b'\\' || c == b'?' => {
                let alone = lv.after == After::Break;
                let start = i;
                let mut text = String::new();
                while i < end {
                    let c = b[i];
                    if c == b'\\' {
                        match b.get(i + 1) {
                            // An escape the cursor has not finished.
                            None => i += 1,
                            Some(b'\n') => i += 2,
                            Some(_) => {
                                let (ch, n) = char_at(s, i + 1);
                                text.push(ch);
                                i += 1 + n;
                            }
                        }
                        continue;
                    }
                    let ends = match c {
                        b'?' => !b.get(i + 1).is_some_and(|&n| is_word_char_byte(n)),
                        b'!' => b.get(i + 1) == Some(&b'='),
                        _ => !is_word_char_byte(c),
                    };
                    if ends {
                        break;
                    }
                    let (ch, n) = char_at(s, i);
                    text.push(ch);
                    i += n;
                }
                if i >= end {
                    return alone.then(|| lv.word(start, text, Quote::Bare));
                }
                lv.value_done(Some(text));
            }
            // `^`, an unmatched `)` or `}`, anything the lexer rejects.
            _ => {
                i += char_at(s, i).1;
                lv.after = After::Joined;
            }
        }
    }
    match stack.pop()? {
        Frame::Code(lv) => {
            (lv.after == After::Break).then(|| lv.word(end, String::new(), Quote::Bare))
        }
        Frame::Dq(dq) => (dq.alone && dq.literal).then_some(dq.at),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::line_editor::{EditorAction, LineEditor};
    use alloc::boxed::Box;
    use alloc::format;
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
        assert_eq!(c.unlisted, 44);
        // They share only the "f" already typed.
        assert_eq!(c.extension, None);
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

    /// A bare word, empty so far, at the start of an argument.
    fn fresh_word() -> WordAt {
        WordAt {
            start: 0,
            text: String::new(),
            quote: Quote::Bare,
            command_position: false,
            command: None,
        }
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
            g.offer(&all[i], false);
        }
        let got = g.finish(&fresh_word(), 0, false);
        let mut sorted = all.clone();
        sorted.sort();
        let spelled: Vec<String> = sorted.iter().map(|n| format!("{} ", n)).collect();
        assert_eq!(got.candidates, spelled[..MAX_CANDIDATES]);
        assert_eq!(got.unlisted, all.len() - MAX_CANDIDATES);
        assert_eq!(got.extension, Some(longest_common_prefix(&all)));
        // And at or under the cap it is exactly the sorted set, all listed.
        let mut g = Gather::new();
        for s in sorted[..MAX_CANDIDATES].iter().rev() {
            g.offer(s, false);
        }
        let got = g.finish(&fresh_word(), 0, false);
        assert_eq!(got.candidates, spelled[..MAX_CANDIDATES]);
        assert_eq!(got.unlisted, 0);
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

    extern crate std;

    std::thread_local! {
        /// The directory `listed` reads, set per test. Each test runs on its
        /// own thread, so no two tests see each other's entries.
        static ENTRIES: core::cell::RefCell<Vec<(String, bool)>> =
            core::cell::RefCell::new(Vec::new());
    }

    /// Every directory holds exactly `ENTRIES`.
    fn listed(_dir: &str, visit: &mut dyn FnMut(&str, bool)) {
        ENTRIES.with(|e| {
            for (name, is_dir) in e.borrow().iter() {
                visit(name, *is_dir);
            }
        });
    }

    /// A source over a directory holding `entries` (name, is_dir), for a
    /// shell whose only commands are `commands`.
    fn over(entries: &[(&str, bool)], commands: &[&str]) -> ShellCompletionSource {
        ENTRIES.with(|e| {
            *e.borrow_mut() = entries.iter().map(|&(n, d)| (String::from(n), d)).collect();
        });
        let mut v: Vec<String> = commands.iter().map(|s| String::from(*s)).collect();
        v.sort();
        v.dedup();
        ShellCompletionSource::with_dir_lister(v, listed)
    }

    /// The value each word of `line` stands for, as the shell will read it:
    /// every token must be a word that neither expands nor globs. Anything
    /// else -- an operator, a comment eating the rest, a `$`, a glob meta in a
    /// bare word -- is the failure, described.
    fn literal_words(line: &str) -> Result<Vec<String>, String> {
        use crate::eval::glob::has_unescaped_meta;
        use crate::parser::lexer::unescape;
        use crate::parser::{tokenize, DqPart, TokenKind};
        let toks = tokenize(line).map_err(|e| format!("{:?} lexing {:?}", e, line))?;
        let mut out = Vec::new();
        let mut last_end = None;
        for t in toks {
            if last_end == Some(t.span.start) && !matches!(t.kind, TokenKind::Eof) {
                return Err(format!(
                    "two tokens glued at {} in {:?}",
                    t.span.start, line
                ));
            }
            last_end = Some(t.span.end);
            match t.kind {
                TokenKind::Eof => break,
                TokenKind::Word(s) if !has_unescaped_meta(&s) => out.push(unescape(&s)),
                TokenKind::SingleQuoted(s) => out.push(s),
                TokenKind::DoubleQuoted(parts) => {
                    let mut v = String::new();
                    for p in parts {
                        match p {
                            DqPart::Literal(s) => v.push_str(&s),
                            other => return Err(format!("{:?} expands in {:?}", other, line)),
                        }
                    }
                    out.push(v);
                }
                other => return Err(format!("{:?} in {:?}", other, line)),
            }
        }
        Ok(out)
    }

    /// Names the shell gives meaning to, a character at a time, each in the
    /// first place and further in.
    const AWKWARD: &[&str] = &[
        "my file",
        "it's",
        "a*b",
        "q?x",
        "[ab]",
        "x]",
        "$home",
        "semi;colon",
        "pipe|x",
        "amp&x",
        "#hash",
        "a#b",
        "~",
        "~x",
        "!bang",
        "x!=y",
        "=eq",
        "a=b",
        "a^b",
        "(x)",
        "{x}",
        "<x>",
        "a\"b",
        "back\\slash",
        "`{x}",
        "tab\there",
        "nl\nhere",
    ];

    #[test]
    fn a_completed_name_reads_back_as_that_name() {
        for name in AWKWARD {
            let (_, line) = tab(over(&[(name, false)], &[]), "cat ");
            assert_eq!(
                literal_words(&line),
                Ok(vec![String::from("cat"), String::from(*name)]),
                "{:?} completed to {:?}",
                name,
                line
            );
            assert!(
                line.ends_with(' '),
                "{:?}: not terminated: {:?}",
                name,
                line
            );
        }
    }

    #[test]
    fn a_name_the_shell_reads_plainly_is_left_plain() {
        for name in ["plain.txt", "-dash", "%pct", "a+b:c@d,e", "na\u{ef}ve"] {
            let (_, line) = tab(over(&[(name, false)], &[]), "cat ");
            assert_eq!(line, format!("cat {} ", name));
        }
    }

    #[test]
    fn a_name_with_a_space_is_quoted_whole() {
        let (_, line) = tab(over(&[("my file", false)], &[]), "cat my");
        assert_eq!(line, "cat 'my file' ");
        // A quote inside is doubled, rc's only escape.
        let (_, line) = tab(over(&[("it's", false)], &[]), "cat it");
        assert_eq!(line, "cat 'it''s' ");
    }

    #[test]
    fn no_control_character_reaches_the_line() {
        // ESC, DEL, and a C1 CSI have no quoted form, so none may be written
        // into the line -- the editor draws the line verbatim. Tab and newline
        // do have one, `\t` and `\n` in double quotes, and take it.
        for name in [
            "esc\u{1b}[31mred",
            "del\u{7f}x",
            "csi\u{9b}x",
            "tab\there",
            "nl\nhere",
        ] {
            let (_, line) = tab(over(&[(name, false)], &[]), "cat ");
            assert!(
                !line.chars().any(char::is_control),
                "{:?} put a control character in {:?}",
                name,
                line
            );
        }
    }

    /// Type `typed` and press Tab `n` times, reporting the action and the line
    /// after each.
    fn tabs(src: ShellCompletionSource, typed: &str, n: usize) -> Vec<(EditorAction, String)> {
        let mut le = LineEditor::new();
        le.set_completion_source(Box::new(src));
        let _ = le.feed_bytes(typed.as_bytes());
        (0..n)
            .map(|_| {
                let r = le.feed_byte(0x09);
                (r, String::from(le.buffer()))
            })
            .collect()
    }

    #[test]
    fn quote_word_spells_what_the_lexer_reads_back() {
        let q = |t: &str, quote, close| quote_word(t, quote, close);
        assert_eq!(q("plain", Quote::Bare, true).as_deref(), Some("plain"));
        assert_eq!(q("x]", Quote::Bare, true).as_deref(), Some("x]"));
        assert_eq!(q("a*b", Quote::Bare, true).as_deref(), Some("'a*b'"));
        assert_eq!(q("it's", Quote::Bare, true).as_deref(), Some("'it''s'"));
        assert_eq!(q("it's", Quote::Bare, false).as_deref(), Some("'it''s"));
        assert_eq!(q("plain", Quote::Single, true).as_deref(), Some("'plain'"));
        assert_eq!(
            q("a\"$\\", Quote::Double, true).as_deref(),
            Some("\"a\\\"\\$\\\\\"")
        );
        assert_eq!(
            q("a\tb\nc", Quote::Single, true).as_deref(),
            Some("\"a\\tb\\nc\"")
        );
        // No quoting spells ESC, DEL or a C1 control.
        for t in ["a\u{1b}b", "a\u{7f}", "\u{9b}"] {
            for quote in [Quote::Bare, Quote::Single, Quote::Double] {
                assert_eq!(q(t, quote, true), None, "{:?} {:?}", t, quote);
            }
        }
    }

    #[test]
    fn the_cap_holds_only_names_it_can_list() {
        // 300 unspellable names sort ahead of the one that can be written; had
        // they taken the held places, it would go unlisted along with them.
        let mut names: Vec<String> = (0..300).map(|i| format!("a\u{1}{:03}", i)).collect();
        names.push(String::from("b1"));
        let entries: Vec<(&str, bool)> = names.iter().map(|n| (n.as_str(), false)).collect();
        let c = over(&entries, &[]).complete("cat ", 4);
        assert_eq!(c.candidates, vec!["b1 "]);
        assert_eq!(c.unlisted, 300);
    }

    #[test]
    fn the_shared_part_of_quoted_names_extends_inside_an_open_quote() {
        let got = tabs(
            over(&[("my file1", false), ("my file2", false)], &[]),
            "cat my",
            2,
        );
        assert_eq!(got[0].1, "cat 'my file");
        // The next Tab continues the quote the first one opened.
        assert_eq!(menu_at(&got[1].0), Some((0, 0)), "{:?}", got[1].0);
        assert_eq!(got[1].1, "cat 'my file1' ");
    }

    #[test]
    fn a_quoted_name_and_a_bare_one_still_share_their_prefix() {
        // Spelled, "'abc 2' " and "abc1 " share nothing; as names they share
        // "abc", and that is what Tab extends to.
        let got = tabs(over(&[("abc1", false), ("abc 2", false)], &[]), "cat a", 2);
        assert_eq!(got[0].1, "cat abc");
        // The menu lists by name: the space sorts first.
        assert_eq!(got[1].1, "cat 'abc 2' ");
    }

    #[test]
    fn a_directory_is_left_open_to_drill_into() {
        // `'my dir'/` would be two words; the quote stays open around the `/`.
        let got = tabs(over(&[("my dir", true)], &[]), "cat my", 2);
        assert_eq!(got[0].1, "cat 'my dir/");
        // Every directory here holds the same entry, so the next Tab drills.
        assert_eq!(got[1].1, "cat 'my dir/my dir/");
        let closed = format!("{}'", got[1].1);
        assert_eq!(
            literal_words(&closed),
            Ok(vec![String::from("cat"), String::from("my dir/my dir/")])
        );
    }

    #[test]
    fn a_quote_the_user_opened_is_continued() {
        let s = || over(&[("plain.txt", false)], &[]);
        assert_eq!(tab(s(), "cat 'pl").1, "cat 'plain.txt' ");
        assert_eq!(tab(s(), "cat \"pl").1, "cat \"plain.txt\" ");
        // Backslashes the user typed are read, and the word respelled whole.
        let (_, line) = tab(over(&[("my file", false)], &[]), "cat my\\ f");
        assert_eq!(line, "cat 'my file' ");
    }

    #[test]
    fn a_double_quote_escapes_what_it_must() {
        // Inside double quotes `"`, `$` and `\` would end, expand or escape.
        let name = "q\"d$v\\b";
        let (_, line) = tab(over(&[(name, false)], &[]), "cat \"q");
        assert_eq!(line, "cat \"q\\\"d\\$v\\\\b\" ");
        assert_eq!(
            literal_words(&line),
            Ok(vec![String::from("cat"), String::from(name)])
        );
    }

    #[test]
    fn a_glob_character_typed_is_matched_as_itself() {
        let (_, line) = tab(over(&[("a*b", false), ("ab", false)], &[]), "cat a*");
        assert_eq!(line, "cat 'a*b' ");
    }

    #[test]
    fn tab_and_newline_are_spelled_in_double_quotes() {
        let (_, line) = tab(over(&[("tab\there", false)], &[]), "cat ta");
        assert_eq!(line, "cat \"tab\\there\" ");
    }

    #[test]
    fn a_name_that_cannot_be_spelled_is_counted_not_listed() {
        let s = || over(&[("esc\u{1b}[1m", false), ("escape", false)], &[]);
        let got = tabs(s(), "cat e", 2);
        // Both share "esc", and no further: the control character stops it.
        assert_eq!(got[0].1, "cat esc");
        // `escape` is the one name listed, but not the one match: the menu,
        // counting the other, rather than a unique completion.
        assert_eq!(menu_at(&got[1].0), Some((0, 1)), "{:?}", got[1].0);
        assert_eq!(got[1].1, "cat escape ");
        // Alone, the unspellable name still extends as far as it can be spelled.
        let got = tabs(over(&[("ab\u{1b}X", false)], &[]), "cat a", 2);
        assert_eq!(got[0].1, "cat ab");
        assert_eq!(got[1], (EditorAction::NoChange, String::from("cat ab")));
    }

    #[test]
    fn a_command_named_like_a_keyword_is_quoted() {
        let got = tabs(over(&[], &["if", "ifconfig"]), "if", 1);
        assert_eq!(got[0].1, "'if' ");
        assert_eq!(literal_words(&got[0].1), Ok(vec![String::from("if")]));
        // An argument named like one is not: the parser reads it as a word.
        let (_, line) = tab(over(&[("if", false)], &["cat"]), "cat i");
        assert_eq!(line, "cat if ");
    }

    #[test]
    fn the_command_is_the_one_the_word_belongs_to() {
        let s = || over(&[("my dir", true), ("my file", false)], &[]);
        assert_eq!(tab(s(), "ls; cd m").1, "ls; cd 'my dir/");
        assert_eq!(tab(s(), "echo $(cat 'my f").1, "echo $(cat 'my file' ");
        // Inside a substitution the command is the inner one: this `cd` wants
        // a directory though `echo` would take either.
        assert_eq!(tab(s(), "echo $(cd 'my").1, "echo $(cd 'my dir/");
    }

    #[test]
    fn tab_is_inert_where_no_word_completes() {
        let (r, line) = tab(over(&[("fo", false)], &[]), "cat $home/f");
        assert_eq!((r, line.as_str()), (EditorAction::NoChange, "cat $home/f"));
    }

    /// `word_at` at the end of `line`: where the word starts, its text, how it
    /// is quoted, whether it names a command, and the command it belongs to.
    fn at_end(line: &str) -> Option<(usize, String, Quote, bool, Option<String>)> {
        word_at(line, line.len()).map(|w| (w.start, w.text, w.quote, w.command_position, w.command))
    }

    fn arg_of(
        cmd: &str,
        start: usize,
        text: &str,
        quote: Quote,
    ) -> Option<(usize, String, Quote, bool, Option<String>)> {
        Some((
            start,
            String::from(text),
            quote,
            false,
            Some(String::from(cmd)),
        ))
    }

    #[test]
    fn the_word_is_read_with_its_quoting_removed() {
        assert_eq!(
            at_end("cat my\\ fi"),
            arg_of("cat", 4, "my fi", Quote::Bare)
        );
        assert_eq!(
            at_end("cat 'it''s a"),
            arg_of("cat", 4, "it's a", Quote::Single)
        );
        assert_eq!(
            at_end("cat \"a\\tb \\$x"),
            arg_of("cat", 4, "a\tb $x", Quote::Double)
        );
        // A quote closed at the cursor is still the word.
        assert_eq!(
            at_end("cat 'my file'"),
            arg_of("cat", 4, "my file", Quote::Single)
        );
        assert_eq!(
            at_end("cat \"my file\""),
            arg_of("cat", 4, "my file", Quote::Double)
        );
        // An escape the cursor has not finished is no part of the text.
        assert_eq!(at_end("cat my\\"), arg_of("cat", 4, "my", Quote::Bare));
        // Past a blank, the word is empty and starts at the cursor.
        assert_eq!(at_end("cat "), arg_of("cat", 4, "", Quote::Bare));
    }

    #[test]
    fn the_command_position_follows_the_grammar() {
        let pos = |line: &str| at_end(line).map(|w| (w.3, w.4));
        let command = Some((true, None));
        for line in [
            "ls",
            "  ls",
            "foo | ls",
            "foo|ls",
            "foo; ls",
            "a && ls",
            "a ?| ls",
            "{ ls",
            "(ls",
            "echo $(ls",
            "a\nls",
            "x => ls",
        ] {
            assert_eq!(pos(line), command, "{:?}", line);
        }
        let arg = |c: &str| Some((false, Some(String::from(c))));
        assert_eq!(pos("ls foo"), arg("ls"));
        assert_eq!(pos("cat a b"), arg("cat"));
        // An escaped operator is text.
        assert_eq!(pos("echo a\\; b"), arg("echo"));
        // A redirect's target is never a command, even ahead of the command.
        assert_eq!(pos(">out"), Some((false, None)));
        assert_eq!(pos("cat > out"), arg("cat"));
        // Inside a substitution the command is its own; after it, the outer.
        assert_eq!(pos("cd $(which ls"), arg("which"));
        assert_eq!(pos("cd $(pwd) su"), arg("cd"));
        // A brace block holds commands; `if`'s parentheses do not.
        assert_eq!(pos("if (x) { cd s"), arg("cd"));
        assert_eq!(pos("if (x"), Some((false, None)));
        // `))` closes arithmetic as one token, leaving the substitution open.
        assert_eq!(pos("cd $( ((1)) x"), Some((false, None)));
    }

    #[test]
    fn a_word_right_after_an_operator_stands_alone() {
        assert_eq!(at_end("ls|gr").map(|w| w.1), Some(String::from("gr")));
        assert_eq!(at_end("cat >fo").map(|w| w.1), Some(String::from("fo")));
        assert_eq!(
            at_end("cc --out=fo").map(|w| (w.0, w.1)),
            Some((9, String::from("fo")))
        );
        assert_eq!(at_end("cat <(ls fo").map(|w| w.1), Some(String::from("fo")));
    }

    #[test]
    fn no_word_completes_where_the_lexer_reads_none() {
        for line in [
            "cat $home/fo",    // glued to a `$var`: two words in ut
            "cat ~/fo",        // glued to a `~`
            "cat a^b",         // a `^` concatenation
            "cat 'my dir'/fo", // glued to a quote
            "cat $ho",         // a `$var` itself
            "cat \"$home/fo",  // a double quote that expands
            "cat # fo",        // a comment
            "cat a#fo",        // `#` ends a word and starts a comment
            "cat <<EO",        // a heredoc tag
            "if ($x =~ /fo",   // a regex
            "cat x?",          // a `?` ending a word is the operator
            "cat !",           // `!` alone
            "cat a)",          // after an unmatched `)`
        ] {
            assert_eq!(at_end(line), None, "{:?}", line);
        }
    }

    /// For a line ending in a word the lexer reads whole, `word_at` reads the
    /// same word: the same start, the same value. The two are separate
    /// scanners over one grammar, and this is what keeps them one grammar.
    #[test]
    fn word_at_agrees_with_the_lexer() {
        use crate::parser::{tokenize, DqPart, TokenKind};
        for line in [
            "cat plain",
            "cat my\\ file",
            "cat 'it''s'",
            "cat \"a\\tb\\\\c\\\"d\\$e\\qf\"",
            "cat ab\\\ncd",
            "ls|grep x",
            "a; b",
            "x?y",
            "cat host!port",
            "echo a=b",
            "cc -o=out",
            "cat na\u{ef}ve 'caf\u{e9} au lait'",
            "{ cd 'my dir' }; ls \"$x\" sub",
            "echo $(ls 'a)b') last",
            "cat <<EOF x",
        ] {
            let toks = match tokenize(line) {
                Ok(t) => t,
                // A heredoc with no body does not lex; its line still reads.
                Err(_) if line.contains("<<") => {
                    assert_eq!(
                        at_end(line).map(|w| (w.0, w.1)),
                        Some((line.len() - 1, String::from("x")))
                    );
                    continue;
                }
                Err(e) => panic!("{:?}: {:?}", line, e),
            };
            let last = &toks[toks.len() - 2];
            let value = match &last.kind {
                TokenKind::Word(s) => crate::parser::lexer::unescape(s),
                TokenKind::SingleQuoted(s) => s.clone(),
                TokenKind::DoubleQuoted(parts) => parts
                    .iter()
                    .map(|p| match p {
                        DqPart::Literal(s) => s.as_str(),
                        other => panic!("{:?}: {:?}", line, other),
                    })
                    .collect(),
                other => panic!("{:?}: last token {:?}", line, other),
            };
            assert_eq!(last.span.end, line.len(), "{:?}", line);
            let w = word_at(line, line.len()).unwrap_or_else(|| panic!("{:?}: no word", line));
            assert_eq!((w.start, w.text), (last.span.start, value), "{:?}", line);
        }
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
