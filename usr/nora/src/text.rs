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

/// Character class for word motion. A "word" is a maximal run of non-
/// whitespace (vim's WORD); whitespace and line breaks separate words.
#[derive(Clone, Copy, PartialEq, Eq)]
enum Class {
    Word,
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
        }
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

    /// Forward to the start of the next WORD (whitespace-delimited), crossing
    /// line breaks; stops at end of document.
    pub fn move_word_forward(&mut self) {
        let (mut r, mut c) = (self.row, self.col);
        // Skip the current word, if the cursor is on one.
        while self.class(r, c) == Class::Word {
            match self.next_pos(r, c) {
                Some(p) => {
                    r = p.0;
                    c = p.1;
                }
                None => break,
            }
        }
        // Skip the whitespace run (and line breaks) to the next word start.
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

    /// Backward to the start of the current-or-previous WORD; stops at start
    /// of document.
    pub fn move_word_back(&mut self) {
        // Step back once: 'b' moves off the current position before scanning.
        let (mut r, mut c) = match self.prev_pos(self.row, self.col) {
            Some(p) => p,
            None => return,
        };
        // Skip whitespace / line breaks backward.
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
        // On a word char: walk to the start of this word.
        while self.class(r, c) == Class::Word {
            match self.prev_pos(r, c) {
                Some(p) if self.class(p.0, p.1) == Class::Word => {
                    r = p.0;
                    c = p.1;
                }
                _ => break,
            }
        }
        self.set_cursor(r, c);
    }

    // -- editing (mutation) -----------------------------------------------

    /// Insert one character at the cursor, advancing the column. A `'\n'`
    /// splits the line (delegates to `insert_newline`).
    pub fn insert_char(&mut self, ch: char) {
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
        let bi = byte_of(&self.lines[self.row], self.col);
        let tail = self.lines[self.row].split_off(bi);
        self.lines.insert(self.row + 1, tail);
        self.row += 1;
        self.col = 0;
    }

    /// Insert a string at the cursor, honoring embedded newlines.
    pub fn insert_str(&mut self, s: &str) {
        for ch in s.chars() {
            self.insert_char(ch);
        }
    }

    /// Delete the character before the cursor (Backspace); at column 0 joins
    /// the current line onto the end of the previous one.
    pub fn backspace(&mut self) {
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
                true
            }
            None => false,
        }
    }

    // -- internal helpers -------------------------------------------------

    /// Class of the char at `(r, c)`; a line-end (`c == len`) on a non-final
    /// line is `Space` (the newline); on the final line it is `End`.
    fn class(&self, r: usize, c: usize) -> Class {
        let len = self.char_len(r);
        if c < len {
            match self.lines[r].chars().nth(c) {
                Some(ch) if ch.is_whitespace() => Class::Space,
                Some(_) => Class::Word,
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
    use super::*;
    use alloc::string::ToString;

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
    fn set_cursor_clamps_out_of_range() {
        let mut t = TextBuffer::new("ab\nc");
        t.set_cursor(99, 99);
        assert_eq!(t.cursor(), (1, 1)); // last line, end
    }
}
