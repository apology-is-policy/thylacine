// nora::text -- the native text-buffer engine (the tui-textarea replacement).
//
// A line-oriented gap-free buffer: a `Vec<String>` of logical lines plus a
// (row, col) cursor in CHARACTER coordinates (col is a char index, never a
// byte offset -- every public position is char-addressed so multi-byte UTF-8
// can't split a grapheme). The engine is pure (no kaua, no libthyla-rs, no
// terminal) and fully host-testable; the editor (nora::editor) drives it and
// the view (nora::view) reads it.
//
// INVARIANT: `lines` is never empty -- an empty document is one empty line.
// `col` is always in `[0, char_len(row)]` (col == len is the just-past-the-
// last-char insert position, the standard text-cursor semantics).
//
// UNDO: a bounded checkpoint stack. The editor calls `checkpoint()` at the
// start of an edit session (entering insert, before a normal-mode structural
// edit) so one `undo()` reverts one user action (vim granularity), not one
// keystroke; the stack is capped (UNDO_CAP) so a long session can't grow it
// without bound (the #65 resource-floor spirit, in userspace).
//
// TABS: a tab is one character in the buffer (it round-trips to disk
// unchanged); the VIEW decides how to render it (v1.0 shows a control char as
// a single space -- expand-to-tabstop is a documented seam). Insert-mode Tab
// inserts soft spaces (nora::editor), so new content stays 1-cell-per-char.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

/// A buffer position: `(row, col)`, both 0-based, `col` a CHARACTER index in
/// `[0, char_len(row)]`.
pub type Pos = (usize, usize);

/// Max retained undo checkpoints. A snapshot clones the line vector, so the
/// cap bounds the editor's worst-case memory at roughly UNDO_CAP * file_size.
const UNDO_CAP: usize = 64;

/// Convert a BYTE offset within `line` to this engine's CHARACTER column.
///
/// The bridge for anything that arrives byte-addressed (a language server's
/// UTF-8 position, a byte-oriented tool's column) into the char coordinates
/// every `Pos` in nora uses. An offset past the end yields the line's char
/// length; one landing mid-character rounds DOWN to that character's column,
/// so the result is always a column the cursor can legally occupy.
pub fn byte_to_char_col(line: &str, byte: usize) -> usize {
    let mut n = 0usize;
    for (off, ch) in line.char_indices() {
        // The byte falls inside (or before) this character.
        if off + ch.len_utf8() > byte {
            return n;
        }
        n += 1;
    }
    n
}

/// Convert this engine's CHARACTER column to a BYTE offset within `line` --
/// the inverse of [`byte_to_char_col`], for handing a position to something
/// byte-addressed. A column past the end yields `line.len()`.
pub fn char_col_to_byte(line: &str, col: usize) -> usize {
    line.char_indices()
        .nth(col)
        .map(|(off, _)| off)
        .unwrap_or(line.len())
}

/// Character class for word motion. A small-word (`w`/`b`/`e`) is a maximal run
/// of ONE class -- WORD chars (alphanumeric + `_`) OR punctuation; a big-WORD
/// (`W`) is any maximal run of non-whitespace. `Space` (whitespace + line
/// breaks) separates words; `End` is the end of the document.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Class {
    Word,
    Punct,
    Space,
    End,
}

#[derive(Clone)]
struct Snapshot {
    lines: Vec<String>,
    row: usize,
    col: usize,
}

/// The editable text buffer. `Clone` parks a backgrounded buffer's full state
/// (lines + cursor + undo stack) when nora holds multiple open buffers.
#[derive(Clone)]
pub struct TextBuffer {
    lines: Vec<String>,
    row: usize,
    col: usize,
    undo: Vec<Snapshot>,
    /// Bumped by every CONTENT mutation (never by cursor movement). Lets a
    /// consumer answer "did the text change since I last looked" in O(1)
    /// instead of comparing whole documents -- the LSP document-sync test
    /// (8e-2) runs on the typing path, where an O(buffer) compare per
    /// keystroke would be real work for nothing.
    ///
    /// Monotonic within a buffer's life; `replace_content` bumps it too (the
    /// content did change), so it is a change DETECTOR, not a content hash:
    /// equal revisions imply equal content, a different revision only implies
    /// a mutation happened.
    rev: u64,
}

impl TextBuffer {
    /// Build a buffer from `content`, splitting on `'\n'`. An empty string is
    /// one empty line. `split('\n')` (not `lines()`) so a trailing newline
    /// becomes a trailing empty line and the content round-trips byte-for-byte
    /// through `content()`.
    pub fn new(content: &str) -> Self {
        let mut lines: Vec<String> = content.split('\n').map(String::from).collect();
        if lines.is_empty() {
            lines.push(String::new());
        }
        TextBuffer {
            lines,
            row: 0,
            col: 0,
            undo: Vec::new(),
            rev: 0,
        }
    }

    /// The content revision -- see the field. Compare two samples to learn
    /// whether the document changed; the value itself means nothing.
    pub fn rev(&self) -> u64 {
        self.rev
    }

    /// The whole document as a single string, lines joined by `'\n'`. Inverse
    /// of `new` -- `new(&t.content()).content() == t.content()`.
    pub fn content(&self) -> String {
        self.lines.join("\n")
    }

    /// The logical lines.
    #[inline]
    pub fn lines(&self) -> &[String] {
        &self.lines
    }

    /// The line count (>= 1).
    #[inline]
    pub fn line_count(&self) -> usize {
        self.lines.len()
    }

    /// Line `row`, or `""` if out of range.
    #[inline]
    pub fn line(&self, row: usize) -> &str {
        self.lines.get(row).map(String::as_str).unwrap_or("")
    }

    /// The cursor `(row, col)`.
    #[inline]
    pub fn cursor(&self) -> Pos {
        (self.row, self.col)
    }

    /// Character length of line `row` (0 if out of range).
    #[inline]
    pub fn char_len(&self, row: usize) -> usize {
        self.lines.get(row).map(|s| s.chars().count()).unwrap_or(0)
    }

    /// The character at `(row, col)`, or `None` past the end of the line.
    #[inline]
    pub fn char_at(&self, row: usize, col: usize) -> Option<char> {
        self.lines.get(row).and_then(|l| l.chars().nth(col))
    }

    /// Set the cursor, clamping `row` to a valid line and `col` to that line's
    /// `[0, char_len]`.
    pub fn set_cursor(&mut self, row: usize, col: usize) {
        let last = self.lines.len().saturating_sub(1);
        self.row = row.min(last);
        self.col = col.min(self.char_len(self.row));
    }

    // -- navigation (cursor moves; no mutation) ---------------------------

    /// Left one char; at column 0 wraps to the end of the previous line
    /// (tui-textarea `Back` semantics, shared by `h` and Left).
    pub fn move_left(&mut self) {
        if self.col > 0 {
            self.col -= 1;
        } else if self.row > 0 {
            self.row -= 1;
            self.col = self.char_len(self.row);
        }
    }

    /// Right one char; at end-of-line wraps to the start of the next line.
    pub fn move_right(&mut self) {
        if self.col < self.char_len(self.row) {
            self.col += 1;
        } else if self.row + 1 < self.lines.len() {
            self.row += 1;
            self.col = 0;
        }
    }

    /// Up one line, preserving the column (clamped to the new line's length).
    pub fn move_up(&mut self) {
        if self.row > 0 {
            self.row -= 1;
            self.col = self.col.min(self.char_len(self.row));
        }
    }

    /// Down one line, preserving the column (clamped).
    pub fn move_down(&mut self) {
        if self.row + 1 < self.lines.len() {
            self.row += 1;
            self.col = self.col.min(self.char_len(self.row));
        }
    }

    /// To column 0 of the current line.
    pub fn move_home(&mut self) {
        self.col = 0;
    }

    /// To the end of the current line.
    pub fn move_end(&mut self) {
        self.col = self.char_len(self.row);
    }

    /// To the first line (column clamped).
    pub fn move_top(&mut self) {
        self.row = 0;
        self.col = self.col.min(self.char_len(self.row));
    }

    /// To the last line (column clamped).
    pub fn move_bottom(&mut self) {
        self.row = self.lines.len().saturating_sub(1);
        self.col = self.col.min(self.char_len(self.row));
    }

    /// Forward to the start of the next small-word (`w`): skip the current
    /// class-run (WORD chars OR punctuation), then any whitespace / line breaks.
    pub fn move_word_forward(&mut self) {
        self.word_forward(false);
    }

    /// Forward to the start of the next big-WORD (`W`): whitespace-delimited
    /// only -- punctuation does not break a WORD.
    pub fn move_long_word_forward(&mut self) {
        self.word_forward(true);
    }

    /// Forward to the END of the next small-word (`e`): the last char of the run.
    pub fn move_word_end(&mut self) {
        let (mut r, mut c) = match self.next_pos(self.row, self.col) {
            Some(p) => p,
            None => return,
        };
        while self.class(r, c) == Class::Space {
            match self.next_pos(r, c) {
                Some(p) => {
                    r = p.0;
                    c = p.1;
                }
                None => {
                    self.set_cursor(r, c);
                    return;
                }
            }
        }
        let run = self.class(r, c);
        while let Some(p) = self.next_pos(r, c) {
            if in_run(self.class(p.0, p.1), run, false) {
                r = p.0;
                c = p.1;
            } else {
                break;
            }
        }
        self.set_cursor(r, c);
    }

    /// Backward to the start of the current-or-previous small-word (`b`); stops
    /// at start of document.
    pub fn move_word_back(&mut self) {
        // Step back once: 'b' moves off the current position before scanning.
        let (mut r, mut c) = match self.prev_pos(self.row, self.col) {
            Some(p) => p,
            None => return,
        };
        while self.class(r, c) == Class::Space {
            match self.prev_pos(r, c) {
                Some(p) => {
                    r = p.0;
                    c = p.1;
                }
                None => {
                    self.set_cursor(r, c);
                    return;
                }
            }
        }
        let run = self.class(r, c);
        while let Some(p) = self.prev_pos(r, c) {
            if in_run(self.class(p.0, p.1), run, false) {
                r = p.0;
                c = p.1;
            } else {
                break;
            }
        }
        self.set_cursor(r, c);
    }

    /// Skip the current class-run then whitespace, forward. `big` collapses
    /// WORD + punctuation into one run (the `W` motion).
    fn word_forward(&mut self, big: bool) {
        let (mut r, mut c) = (self.row, self.col);
        let start = self.class(r, c);
        if start == Class::Word || start == Class::Punct {
            while in_run(self.class(r, c), start, big) {
                match self.next_pos(r, c) {
                    Some(p) => {
                        r = p.0;
                        c = p.1;
                    }
                    None => break,
                }
            }
        }
        while self.class(r, c) == Class::Space {
            match self.next_pos(r, c) {
                Some(p) => {
                    r = p.0;
                    c = p.1;
                }
                None => break,
            }
        }
        self.set_cursor(r, c);
    }

    // -- position helpers (text-object range math) ------------------------

    /// The position immediately after `p` (the next char, or the start of the
    /// next line at a line end), or `None` at end of document. The public face
    /// of the internal stepper, for the editor's text-object range math.
    #[inline]
    pub fn pos_after(&self, p: Pos) -> Option<Pos> {
        self.next_pos(p.0, p.1)
    }

    /// The position immediately before `p` (the previous char, or the end of the
    /// previous line at column 0), or `None` at start of document.
    #[inline]
    pub fn pos_before(&self, p: Pos) -> Option<Pos> {
        self.prev_pos(p.0, p.1)
    }

    /// The inclusive span of the same-class run under `pos` on its own line --
    /// the `iw` text object. `None` only on an empty line. Whitespace is its own
    /// run (so `iw` on a gap selects the gap, vim-style); words do not span lines.
    pub fn word_span_at(&self, pos: Pos) -> Option<(Pos, Pos)> {
        let (r, _) = pos;
        let len = self.char_len(r);
        if len == 0 {
            return None;
        }
        let c = pos.1.min(len - 1); // clamp a just-past-end cursor onto the last char
        let target = self.class(r, c);
        let mut lo = c;
        while lo > 0 && self.class(r, lo - 1) == target {
            lo -= 1;
        }
        let mut hi = c;
        while hi + 1 < len && self.class(r, hi + 1) == target {
            hi += 1;
        }
        Some(((r, lo), (r, hi)))
    }

    // -- editing (mutation) -----------------------------------------------

    /// Insert one character at the cursor, advancing the column. A `'\n'`
    /// splits the line (delegates to `insert_newline`).
    pub fn insert_char(&mut self, ch: char) {
        self.rev += 1;
        if ch == '\n' {
            self.insert_newline();
            return;
        }
        let bi = byte_of(&self.lines[self.row], self.col);
        self.lines[self.row].insert(bi, ch);
        self.col += 1;
    }

    /// Split the current line at the cursor; the tail becomes a new line below
    /// and the cursor moves to its start.
    pub fn insert_newline(&mut self) {
        self.rev += 1;
        let bi = byte_of(&self.lines[self.row], self.col);
        let tail = self.lines[self.row].split_off(bi);
        self.lines.insert(self.row + 1, tail);
        self.row += 1;
        self.col = 0;
    }

    /// Insert a string at the cursor, honoring embedded newlines.
    pub fn insert_str(&mut self, s: &str) {
        self.rev += 1;
        for ch in s.chars() {
            self.insert_char(ch);
        }
    }

    /// Delete the character before the cursor (Backspace); at column 0 joins
    /// the current line onto the end of the previous one.
    pub fn backspace(&mut self) {
        self.rev += 1;
        if self.col > 0 {
            let bi = byte_of(&self.lines[self.row], self.col - 1);
            self.lines[self.row].remove(bi);
            self.col -= 1;
        } else if self.row > 0 {
            let cur = self.lines.remove(self.row);
            self.row -= 1;
            self.col = self.char_len(self.row);
            self.lines[self.row].push_str(&cur);
        }
    }

    /// Delete the character at the cursor (`x` / Delete); at end-of-line joins
    /// the next line up (deletes the line break).
    pub fn delete_char(&mut self) {
        self.rev += 1;
        let len = self.char_len(self.row);
        if self.col < len {
            let bi = byte_of(&self.lines[self.row], self.col);
            self.lines[self.row].remove(bi);
        } else if self.row + 1 < self.lines.len() {
            let next = self.lines.remove(self.row + 1);
            self.lines[self.row].push_str(&next);
        }
    }

    /// Delete the whole current line (`dd`). The buffer keeps at least one
    /// line; the cursor clamps to the line now at `row` (or the new last line).
    pub fn delete_line(&mut self) {
        self.rev += 1;
        if self.lines.len() == 1 {
            self.lines[0].clear();
            self.col = 0;
            return;
        }
        self.lines.remove(self.row);
        if self.row >= self.lines.len() {
            self.row = self.lines.len() - 1;
        }
        self.col = self.col.min(self.char_len(self.row));
    }

    /// The text spanning `[a, b]` inclusive (positions normalized internally),
    /// with `'\n'` between lines -- the visual-mode yank/cut payload.
    pub fn range_text(&self, a: Pos, b: Pos) -> String {
        let (lo, hi) = order(a, b);
        let mut out = String::new();
        if lo.0 == hi.0 {
            // hi.1 is inclusive -> +1 for the exclusive char-slice end.
            out.push_str(char_slice(self.line(lo.0), lo.1, hi.1 + 1));
            return out;
        }
        let first = self.line(lo.0);
        out.push_str(char_slice(first, lo.1, self.char_len(lo.0)));
        out.push('\n');
        for r in (lo.0 + 1)..hi.0 {
            out.push_str(self.line(r));
            out.push('\n');
        }
        out.push_str(char_slice(self.line(hi.0), 0, hi.1 + 1));
        out
    }

    /// Delete the inclusive span `[a, b]`; the cursor lands at the span start.
    pub fn delete_range(&mut self, a: Pos, b: Pos) {
        self.rev += 1;
        let (lo, hi) = order(a, b);
        let prefix = char_slice(self.line(lo.0), 0, lo.1).to_string();
        let suffix = char_slice(self.line(hi.0), hi.1 + 1, self.char_len(hi.0)).to_string();
        let mut merged = prefix;
        merged.push_str(&suffix);
        // Replace lines lo.0..=hi.0 with the single merged line.
        self.lines.splice(lo.0..=hi.0, core::iter::once(merged));
        self.row = lo.0;
        self.col = lo.1.min(self.char_len(self.row));
    }

    // -- search -----------------------------------------------------------

    /// Find `pat` starting just after the cursor, wrapping to the top. Returns
    /// the match-start position. An empty pattern finds nothing.
    /// All non-overlapping literal matches of `pat` across the buffer, each as
    /// an inclusive char span `(start, end)`. Single-line only (a pattern is not
    /// matched across a newline). The multi-cursor select (`s`) substrate.
    pub fn find_all(&self, pat: &str) -> Vec<(Pos, Pos)> {
        let mut out = Vec::new();
        if pat.is_empty() {
            return out;
        }
        let plen = pat.chars().count();
        for (r, line) in self.lines.iter().enumerate() {
            for (byte, _) in line.match_indices(pat) {
                let start = char_col_of(line, byte);
                out.push(((r, start), (r, start + plen - 1)));
            }
        }
        out
    }

    pub fn find(&self, pat: &str) -> Option<Pos> {
        if pat.is_empty() {
            return None;
        }
        let n = self.lines.len();
        // Probe each line once, starting from the cursor's line; the cursor
        // line is also retried from the top on the wrap pass so a match before
        // the cursor on the same line is found.
        for off in 0..=n {
            let r = (self.row + off) % n;
            let line = &self.lines[r];
            let from_byte = if off == 0 {
                byte_of(line, self.col + 1)
            } else {
                0
            };
            if from_byte <= line.len() {
                if let Some(rel) = line[from_byte..].find(pat) {
                    let abs = from_byte + rel;
                    return Some((r, char_col_of(line, abs)));
                }
            }
            if off == n {
                // Wrap pass over the cursor line: search its head up to the
                // cursor so an earlier match on that line is reachable.
                if let Some(rel) = self.lines[self.row][..].find(pat) {
                    let abs = rel;
                    return Some((self.row, char_col_of(&self.lines[self.row], abs)));
                }
            }
        }
        None
    }

    // -- undo -------------------------------------------------------------

    /// Push an undo checkpoint (call at the start of an edit session). The
    /// stack is capped at `UNDO_CAP`; the oldest checkpoint is dropped on
    /// overflow.
    pub fn checkpoint(&mut self) {
        if self.undo.len() == UNDO_CAP {
            self.undo.remove(0);
        }
        self.undo.push(Snapshot {
            lines: self.lines.clone(),
            row: self.row,
            col: self.col,
        });
    }

    /// Revert to the most recent checkpoint. Returns `true` if a checkpoint
    /// was restored, `false` if the stack was empty.
    pub fn undo(&mut self) -> bool {
        match self.undo.pop() {
            Some(s) => {
                self.lines = s.lines;
                self.set_cursor(s.row, s.col);
                // Restoring a snapshot IS a content change -- rev is a
                // detector, not a hash, so it moves FORWARD on an undo too.
                self.rev += 1;
                true
            }
            // Nothing to undo: the content did not change, so neither does rev.
            None => false,
        }
    }

    /// Replace the whole content (format-on-save), pushing an undo checkpoint
    /// first so a single `u` restores the pre-format text, and re-clamping the
    /// cursor to the new bounds (a formatter mostly preserves line structure,
    /// so the clamped position stays near the edit point). The same
    /// trailing-newline round-trip rule as `new()`.
    pub fn replace_content(&mut self, content: &str) {
        self.rev += 1;
        self.checkpoint();
        let mut lines: Vec<String> = content.split('\n').map(String::from).collect();
        if lines.is_empty() {
            lines.push(String::new());
        }
        self.lines = lines;
        let (row, col) = (self.row, self.col);
        self.set_cursor(row, col);
    }

    // -- internal helpers -------------------------------------------------

    /// Class of the char at `(r, c)`; a line-end (`c == len`) on a non-final
    /// line is `Space` (the newline); on the final line it is `End`.
    fn class(&self, r: usize, c: usize) -> Class {
        let len = self.char_len(r);
        if c < len {
            match self.lines[r].chars().nth(c) {
                Some(ch) if ch.is_whitespace() => Class::Space,
                Some(ch) if is_word_char(ch) => Class::Word,
                Some(_) => Class::Punct,
                None => Class::Space,
            }
        } else if r + 1 < self.lines.len() {
            Class::Space
        } else {
            Class::End
        }
    }

    /// The position after `(r, c)`: the next char, or the start of the next
    /// line at a line-end, or `None` at end of document.
    fn next_pos(&self, r: usize, c: usize) -> Option<Pos> {
        if c < self.char_len(r) {
            Some((r, c + 1))
        } else if r + 1 < self.lines.len() {
            Some((r + 1, 0))
        } else {
            None
        }
    }

    /// The position before `(r, c)`: the previous char, or the end of the
    /// previous line at column 0, or `None` at start of document.
    fn prev_pos(&self, r: usize, c: usize) -> Option<Pos> {
        if c > 0 {
            Some((r, c - 1))
        } else if r > 0 {
            Some((r - 1, self.char_len(r - 1)))
        } else {
            None
        }
    }
}

/// A WORD-class character (alphanumeric or underscore); the small-word motions
/// treat a run of these as one word and a run of other punctuation as another.
fn is_word_char(c: char) -> bool {
    c.is_alphanumeric() || c == '_'
}

/// Whether class `cur` continues the run that began with `start`. `Space`/`End`
/// never continue; in `big` (WORD) mode any non-space continues; otherwise the
/// class must match (WORD vs punctuation stay distinct runs).
fn in_run(cur: Class, start: Class, big: bool) -> bool {
    match cur {
        Class::Space | Class::End => false,
        _ if big => true,
        _ => cur == start,
    }
}

/// Byte offset of the `col`-th character of `s`, or `s.len()` if `col` is at
/// or past the end (the insert-at-end position).
fn byte_of(s: &str, col: usize) -> usize {
    match s.char_indices().nth(col) {
        Some((i, _)) => i,
        None => s.len(),
    }
}

/// The character index of byte offset `b` within `s` (inverse of `byte_of`).
fn char_col_of(s: &str, b: usize) -> usize {
    s[..b.min(s.len())].chars().count()
}

/// The substring of `s` spanning characters `[start, end)` (char indices).
fn char_slice(s: &str, start: usize, end: usize) -> &str {
    let a = byte_of(s, start);
    let b = byte_of(s, end);
    &s[a..b.max(a)]
}

/// Order two positions so the first is <= the second (row-major).
fn order(a: Pos, b: Pos) -> (Pos, Pos) {
    if (a.0, a.1) <= (b.0, b.1) {
        (a, b)
    } else {
        (b, a)
    }
}

#[cfg(test)]
mod tests {
    /// Every CONTENT mutator must bump `rev`, and no CURSOR movement may.
    /// Enumerated deliberately: a mutator added later without a bump silently
    /// costs the LSP client a document sync (fail-soft, but wrong), and this
    /// is the thing that catches it.
    #[test]
    fn rev_bumps_on_every_content_mutation_only() {
        // Movement never bumps.
        let mut t = TextBuffer::new("alpha\nbeta\ngamma");
        let r0 = t.rev();
        t.move_right();
        t.move_down();
        t.move_end();
        t.move_word_forward();
        t.move_top();
        t.set_cursor(1, 0);
        assert_eq!(t.rev(), r0, "cursor movement must not bump rev");

        // Each mutator bumps, from a fresh buffer so one cannot mask another.
        let bump = |name: &str, f: &dyn Fn(&mut TextBuffer)| {
            let mut t = TextBuffer::new("alpha\nbeta\ngamma");
            t.set_cursor(1, 2);
            let before = t.rev();
            f(&mut t);
            assert!(t.rev() > before, "{} did not bump rev", name);
        };
        bump("insert_char", &|t| t.insert_char('x'));
        bump("insert_newline", &|t| t.insert_newline());
        bump("insert_str", &|t| t.insert_str("xy"));
        bump("backspace", &|t| t.backspace());
        bump("delete_char", &|t| t.delete_char());
        bump("delete_line", &|t| t.delete_line());
        bump("delete_range", &|t| t.delete_range((0, 0), (1, 1)));
        bump("replace_content", &|t| t.replace_content("new"));

        // undo bumps only when it actually restores something.
        let mut t = TextBuffer::new("alpha");
        let empty = t.rev();
        assert!(!t.undo(), "nothing to undo yet");
        assert_eq!(t.rev(), empty, "a no-op undo must not bump rev");
        t.checkpoint();
        t.insert_char('z');
        let edited = t.rev();
        assert!(t.undo());
        assert!(t.rev() > edited, "a real undo must bump rev");
    }

    use super::*;

    #[test]
    fn byte_and_char_columns_convert_both_ways() {
        // ASCII: the two coordinates coincide, which is exactly why a
        // byte-vs-char mixup survives every ASCII test and breaks on the first
        // accented character.
        assert_eq!(byte_to_char_col("abc", 2), 2);
        assert_eq!(char_col_to_byte("abc", 2), 2);

        // "aébc": bytes a=0, é=1..3, b=3, c=4.
        let s = "aébc";
        assert_eq!(char_col_to_byte(s, 0), 0);
        assert_eq!(char_col_to_byte(s, 1), 1);
        assert_eq!(char_col_to_byte(s, 2), 3); // 'b'
        assert_eq!(byte_to_char_col(s, 0), 0);
        assert_eq!(byte_to_char_col(s, 1), 1);
        assert_eq!(byte_to_char_col(s, 3), 2);
        // Round-trip every valid column.
        for col in 0..=s.chars().count() {
            assert_eq!(byte_to_char_col(s, char_col_to_byte(s, col)), col);
        }
    }

    #[test]
    fn column_conversion_clamps_out_of_range() {
        let s = "aé";
        // Past the end -> the line's char length / byte length.
        assert_eq!(byte_to_char_col(s, 99), 2);
        assert_eq!(char_col_to_byte(s, 99), s.len());
        // A byte offset INSIDE 'é' (byte 2) rounds down to that char's column,
        // so the result is always a column the cursor may occupy.
        assert_eq!(byte_to_char_col(s, 2), 1);
        // Empty line: every offset is column 0.
        assert_eq!(byte_to_char_col("", 5), 0);
        assert_eq!(char_col_to_byte("", 5), 0);
    }

    #[test]
    fn new_and_content_round_trip() {
        assert_eq!(TextBuffer::new("").content(), "");
        assert_eq!(TextBuffer::new("a\nb").content(), "a\nb");
        // A trailing newline becomes a trailing empty line, preserved on save.
        assert_eq!(TextBuffer::new("a\nb\n").content(), "a\nb\n");
        assert_eq!(TextBuffer::new("").line_count(), 1);
        assert_eq!(TextBuffer::new("a\nb\n").line_count(), 3);
    }

    #[test]
    fn insert_char_advances() {
        let mut t = TextBuffer::new("");
        t.insert_char('h');
        t.insert_char('i');
        assert_eq!(t.content(), "hi");
        assert_eq!(t.cursor(), (0, 2));
    }

    #[test]
    fn insert_char_is_char_indexed_for_utf8() {
        let mut t = TextBuffer::new("aé");
        t.set_cursor(0, 2); // past 'é' (2 chars, 3 bytes)
        t.insert_char('z');
        assert_eq!(t.content(), "aéz");
    }

    #[test]
    fn newline_splits_line() {
        let mut t = TextBuffer::new("abcd");
        t.set_cursor(0, 2);
        t.insert_newline();
        assert_eq!(t.content(), "ab\ncd");
        assert_eq!(t.cursor(), (1, 0));
    }

    #[test]
    fn backspace_within_and_across_lines() {
        let mut t = TextBuffer::new("ab\ncd");
        t.set_cursor(1, 1);
        t.backspace(); // delete 'c'
        assert_eq!(t.content(), "ab\nd");
        t.set_cursor(1, 0);
        t.backspace(); // join onto line 0
        assert_eq!(t.content(), "abd");
        assert_eq!(t.cursor(), (0, 2));
    }

    #[test]
    fn delete_char_joins_at_eol() {
        let mut t = TextBuffer::new("ab\ncd");
        t.set_cursor(0, 0);
        t.delete_char(); // 'a'
        assert_eq!(t.content(), "b\ncd");
        t.set_cursor(0, 1); // end of "b"
        t.delete_char(); // join next line up
        assert_eq!(t.content(), "bcd");
    }

    #[test]
    fn delete_line_keeps_one_line() {
        let mut t = TextBuffer::new("a\nb\nc");
        t.set_cursor(1, 0);
        t.delete_line();
        assert_eq!(t.content(), "a\nc");
        let mut single = TextBuffer::new("only");
        single.delete_line();
        assert_eq!(single.content(), "");
        assert_eq!(single.line_count(), 1);
    }

    #[test]
    fn motion_clamps_and_wraps() {
        let mut t = TextBuffer::new("ab\ncde");
        t.set_cursor(0, 2); // end of "ab"
        t.move_right(); // wrap to (1,0)
        assert_eq!(t.cursor(), (1, 0));
        t.move_left(); // wrap back to (0,2)
        assert_eq!(t.cursor(), (0, 2));
        t.set_cursor(1, 3);
        t.move_up(); // clamp col to len("ab")==2
        assert_eq!(t.cursor(), (0, 2));
    }

    #[test]
    fn word_forward_and_back() {
        let mut t = TextBuffer::new("the quick  fox");
        t.set_cursor(0, 0);
        t.move_word_forward();
        assert_eq!(t.cursor(), (0, 4)); // "quick"
        t.move_word_forward();
        assert_eq!(t.cursor(), (0, 11)); // "fox" (double space skipped)
        t.move_word_back();
        assert_eq!(t.cursor(), (0, 4)); // back to "quick"
    }

    #[test]
    fn word_forward_crosses_lines() {
        let mut t = TextBuffer::new("end\nnext");
        t.set_cursor(0, 0);
        t.move_word_forward();
        assert_eq!(t.cursor(), (1, 0)); // "next" on the following line
    }

    #[test]
    fn small_word_stops_at_punctuation() {
        let mut t = TextBuffer::new("foo.bar baz");
        t.set_cursor(0, 0);
        t.move_word_forward();
        assert_eq!(t.cursor(), (0, 3)); // the "." is its own small-word
        t.move_word_forward();
        assert_eq!(t.cursor(), (0, 4)); // "bar"
        t.move_word_forward();
        assert_eq!(t.cursor(), (0, 8)); // "baz"
    }

    #[test]
    fn long_word_spans_punctuation() {
        let mut t = TextBuffer::new("foo.bar baz");
        t.set_cursor(0, 0);
        t.move_long_word_forward();
        assert_eq!(t.cursor(), (0, 8)); // WORD skips "foo.bar" whole -> "baz"
    }

    #[test]
    fn word_end_lands_on_last_char() {
        let mut t = TextBuffer::new("foo bar");
        t.set_cursor(0, 0);
        t.move_word_end();
        assert_eq!(t.cursor(), (0, 2)); // end of "foo"
        t.move_word_end();
        assert_eq!(t.cursor(), (0, 6)); // end of "bar"
    }

    #[test]
    fn small_word_back_stops_at_punctuation() {
        let mut t = TextBuffer::new("foo.bar");
        t.set_cursor(0, 6); // on the last "r"
        t.move_word_back();
        assert_eq!(t.cursor(), (0, 4)); // start of "bar"
        t.move_word_back();
        assert_eq!(t.cursor(), (0, 3)); // the "."
        t.move_word_back();
        assert_eq!(t.cursor(), (0, 0)); // start of "foo"
    }

    #[test]
    fn word_span_at_covers_the_run() {
        let t = TextBuffer::new("foo bar");
        assert_eq!(t.word_span_at((0, 5)), Some(((0, 4), (0, 6)))); // "bar"
        assert_eq!(t.word_span_at((0, 3)), Some(((0, 3), (0, 3)))); // the gap
        assert_eq!(TextBuffer::new("").word_span_at((0, 0)), None); // empty line
    }

    #[test]
    fn range_text_and_delete_same_line() {
        let mut t = TextBuffer::new("hello");
        let s = t.range_text((0, 1), (0, 3)); // 'e','l','l' inclusive
        assert_eq!(s, "ell");
        t.delete_range((0, 1), (0, 3));
        assert_eq!(t.content(), "ho");
        assert_eq!(t.cursor(), (0, 1));
    }

    #[test]
    fn range_text_and_delete_multi_line() {
        let mut t = TextBuffer::new("abc\ndef\nghi");
        // (0,1)..(2,1): "bc\ndef\ngh"
        assert_eq!(t.range_text((0, 1), (2, 1)), "bc\ndef\ngh");
        t.delete_range((0, 1), (2, 1));
        assert_eq!(t.content(), "ai");
        assert_eq!(t.cursor(), (0, 1));
    }

    #[test]
    fn range_normalizes_reversed_endpoints() {
        let t = TextBuffer::new("hello");
        assert_eq!(t.range_text((0, 3), (0, 1)), "ell");
    }

    #[test]
    fn find_wraps() {
        let mut t = TextBuffer::new("foo\nbar\nfoo");
        t.set_cursor(0, 0);
        assert_eq!(t.find("foo"), Some((2, 0))); // next match after cursor
        t.set_cursor(2, 0);
        assert_eq!(t.find("foo"), Some((0, 0))); // wraps to the top
        assert_eq!(t.find("zzz"), None);
        assert_eq!(t.find(""), None);
    }

    #[test]
    fn undo_reverts_one_checkpoint() {
        let mut t = TextBuffer::new("abc");
        t.set_cursor(0, 3);
        t.checkpoint();
        t.insert_str("def");
        assert_eq!(t.content(), "abcdef");
        assert!(t.undo());
        assert_eq!(t.content(), "abc");
        assert_eq!(t.cursor(), (0, 3));
        assert!(!t.undo()); // stack empty
    }

    #[test]
    fn undo_stack_is_capped() {
        let mut t = TextBuffer::new("x");
        for _ in 0..(UNDO_CAP + 10) {
            t.checkpoint();
        }
        assert_eq!(t.undo.len(), UNDO_CAP);
    }

    #[test]
    fn replace_content_clamps_cursor_and_undoes() {
        // Cursor on the last line, past the new content's bounds.
        let mut t = TextBuffer::new("aaaa\nbbbb\ncccc");
        t.set_cursor(2, 4);
        t.replace_content("zz\ny");
        assert_eq!(t.content(), "zz\ny");
        // Row clamped to the last line (1), col to its char_len (1).
        assert_eq!(t.cursor(), (1, 1));
        // One undo restores the pre-replace text AND cursor.
        assert!(t.undo());
        assert_eq!(t.content(), "aaaa\nbbbb\ncccc");
        assert_eq!(t.cursor(), (2, 4));
    }

    #[test]
    fn set_cursor_clamps_out_of_range() {
        let mut t = TextBuffer::new("ab\nc");
        t.set_cursor(99, 99);
        assert_eq!(t.cursor(), (1, 1)); // last line, end
    }

    #[test]
    fn find_all_single_char_inclusive() {
        let t = TextBuffer::new("a ba a\nxa");
        assert_eq!(
            t.find_all("a"),
            alloc::vec![((0, 0), (0, 0)), ((0, 3), (0, 3)), ((0, 5), (0, 5)), ((1, 1), (1, 1))]
        );
    }

    #[test]
    fn find_all_multichar_end_is_inclusive() {
        let t = TextBuffer::new("foo foo");
        assert_eq!(t.find_all("foo"), alloc::vec![((0, 0), (0, 2)), ((0, 4), (0, 6))]);
    }

    #[test]
    fn find_all_empty_pattern_is_empty() {
        assert!(TextBuffer::new("abc").find_all("").is_empty());
    }
}
