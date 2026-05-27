// libutopia::line_editor -- pure-logic line editor engine for the Utopia
// shell (U-4a). Hand-rolled (NOT reedline; reedline assumes std).
//
// Per docs/UTOPIA-SHELL-DESIGN.md section 11.2: the line editor is
// implemented in libutopia, no_std + alloc. Approximate scope across
// the U-4a/b/c/d sub-arc: 1500-2500 LOC.
//
// Strategic framing per the U-4 handoff (memory/project_next_session.md):
// the line editor is a PURE-LOGIC ENGINE that consumes byte streams +
// produces editor actions. Raw-mode I/O (termios via /dev/consctl) is
// DEFERRED to U-6 (the main loop wiring) + U-PTY (the substrate). v1.0
// has no PTY surface, so a pure-logic engine is the only thing that can
// land before U-PTY without blocking on the kernel.
//
// What's at U-4a:
//   - LineEditor struct + EditorAction enum (the engine API)
//   - ANSI input escape parser state machine (arrow keys, Home/End,
//     Delete via CSI[3~)
//   - Emacs editing primitives: Ctrl-A/E/B/F/K/U/W/Y/D/C/L, Backspace,
//     Enter, printable
//   - UTF-8 multi-byte accumulation (paste "héllo" arrives byte-at-
//     a-time, the engine assembles the char before inserting)
//   - render(prompt: &str) -> String emitter (single-line; multi-line
//     is U-4b)
//
// What's deferred:
//   - Multi-line + bracket-balance continuation -> U-4b
//   - History up/down nav + Ctrl-R incremental search -> U-4c (the
//     arrows are recognized + dispatched, but at U-4a the action is
//     no-op when history.is_empty())
//   - Tab completion via pluggable CompletionSource -> U-4d
//   - vi mode -> v1.x (UTOPIA-SHELL-DESIGN section 11.2)
//   - Modifier-key recognition (Ctrl-arrow, Alt-x) -> v1.x
//   - Bracketed-paste mode -> v1.x
//   - Grapheme-cluster cursor + display width -> v1.x
//     (v1.0 treats one char as one column; emoji + combining marks
//     render visually inconsistent but the buffer stays valid UTF-8)

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

// =============================================================================
// EditorAction -- the engine's externally-visible output.
// =============================================================================
//
// The main loop reads bytes from stdin one at a time (or in batches),
// calls feed_byte / feed_bytes, and dispatches based on this enum:
//
//   NoChange     -- input absorbed; nothing externally visible (e.g.
//                   partial multi-byte sequence still accumulating,
//                   or an arrow key that hit an empty history).
//   Redraw       -- buffer or cursor changed; the main loop should call
//                   render(prompt) and emit the result to stdout.
//   Accept(line) -- Enter pressed; the line is ready for evaluation.
//                   The engine has reset its internal buffer.
//   Cancel       -- Ctrl-C; the current edit should be discarded and a
//                   fresh prompt drawn. The engine has reset its buffer.
//   Eof          -- Ctrl-D on an empty buffer; the main loop should
//                   exit the interactive session.
//   ClearScreen  -- Ctrl-L; the main loop should clear the screen
//                   (typically by emitting "\x1b[2J\x1b[H") and then
//                   redraw the prompt + buffer. The engine has NOT
//                   touched its state.

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EditorAction {
    /// Nothing externally visible changed. The main loop does not redraw.
    NoChange,
    /// The buffer / cursor changed. The main loop should call render().
    Redraw,
    /// Enter was pressed. The submitted line is enclosed. The engine
    /// has reset its internal buffer (a subsequent feed_byte starts a
    /// fresh edit).
    Accept(String),
    /// Ctrl-C. Current edit discarded. The engine's buffer is empty.
    Cancel,
    /// Ctrl-D on empty buffer. The main loop should exit.
    Eof,
    /// Ctrl-L. The main loop should clear the screen and redraw the
    /// prompt + buffer. The engine's state is unchanged.
    ClearScreen,
}

// =============================================================================
// Action -- internal enum the parser decodes a byte sequence into. NOT
// part of the public API; the public surface is feed_byte/feed_bytes +
// EditorAction.
// =============================================================================

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Action {
    /// Insert a complete char at cursor.
    InsertChar(char),
    /// Backspace -- delete the char immediately BEFORE cursor.
    Backspace,
    /// Delete -- delete the char immediately AT cursor.
    DeleteChar,
    /// Cursor motion.
    CursorLeft,
    CursorRight,
    CursorHome,
    CursorEnd,
    /// Kill primitives (move text to kill_buffer).
    KillToEnd,
    KillToStart,
    KillPrevWord,
    /// Yank -- insert kill_buffer at cursor.
    Yank,
    /// History navigation -- recognized at U-4a, no-op until U-4c lands.
    HistoryPrev,
    HistoryNext,
    /// Control-flow.
    Accept,
    Cancel,
    /// Ctrl-D dual behaviour -- Eof on empty buffer, DeleteChar otherwise.
    EofOrDelete,
    /// Ctrl-L -- screen clear.
    ClearScreen,
    /// No-op (decoded but ignored, e.g. unknown CSI final byte).
    Ignore,
}

// =============================================================================
// ParserState -- ANSI input escape state machine.
// =============================================================================
//
// stdin bytes arrive interleaved with ANSI CSI sequences. The parser
// handles three multi-byte cases:
//   - ESC + '[' + ... + final_char  (CSI sequence; arrows, Home/End, Del)
//   - 0xC0..=0xF7 + continuation bytes (UTF-8 multi-byte char)
//   - a stray ESC followed by an unrecognized byte (abort to Ground)
//
// State transitions:
//   Ground -> ESC seen -> Escape
//   Escape -> '[' seen -> Csi (CSI/CSI prefix; parse params + final)
//   Escape -> anything else -> Ground (sequence aborted)
//   Csi -> digit -> accumulate param
//   Csi -> ';' -> next param
//   Csi -> final char -> apply action, Ground
//   Ground -> first UTF-8 continuation high byte -> Utf8
//   Utf8 -> continuation byte (0x80..=0xBF) -> accumulate
//   Utf8 -> complete -> emit InsertChar, Ground
//   Utf8 -> invalid byte (not a continuation) -> Ground (drop sequence)

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ParserState {
    Ground,
    Escape,
    Csi {
        params: [u32; 4],
        param_count: u8,
        current_has_digits: bool,
    },
    Utf8 {
        buf: [u8; 4],
        expected: u8,
        have: u8,
    },
}

// =============================================================================
// LineEditor -- the engine itself.
// =============================================================================

/// Per-Proc maximum buffer length. A defensive cap so a runaway paste
/// can't grow the line indefinitely. 64 KiB is well above any sensible
/// shell line.
const MAX_BUFFER_LEN: usize = 64 * 1024;

pub struct LineEditor {
    buffer: String,
    /// Cursor as a byte index into `buffer` -- always on a UTF-8
    /// character boundary.
    cursor: usize,
    /// Most-recent kill for Ctrl-Y yank.
    kill_buffer: String,
    /// In-memory history. Empty at U-4a; populated by U-4c.
    history: Vec<String>,
    /// History navigation position: None == editing current; Some(i)
    /// == viewing history[i]. When the user starts navigating away
    /// from the current edit, the current buffer is saved in
    /// `saved_current`; navigating back to None restores it.
    history_pos: Option<usize>,
    saved_current: String,
    parser: ParserState,
}

impl Default for LineEditor {
    fn default() -> Self {
        Self::new()
    }
}

impl LineEditor {
    pub fn new() -> Self {
        Self {
            buffer: String::new(),
            cursor: 0,
            kill_buffer: String::new(),
            history: Vec::new(),
            history_pos: None,
            saved_current: String::new(),
            parser: ParserState::Ground,
        }
    }

    /// Clear the current edit + reset the parser. Used by the main
    /// loop after handling EditorAction::Cancel or after Accept.
    pub fn reset(&mut self) {
        self.buffer.clear();
        self.cursor = 0;
        self.history_pos = None;
        self.saved_current.clear();
        self.parser = ParserState::Ground;
        // kill_buffer survives Cancel/Accept -- yank can paste across
        // a Cancel boundary (standard emacs behaviour).
    }

    pub fn buffer(&self) -> &str {
        &self.buffer
    }

    pub fn cursor(&self) -> usize {
        self.cursor
    }

    pub fn kill_buffer(&self) -> &str {
        &self.kill_buffer
    }

    pub fn history(&self) -> &[String] {
        &self.history
    }

    /// Append a line to history. v1.0 history is in-memory only; the
    /// disk-backed history file at ~/.config/utopia/history (per
    /// UTOPIA-SHELL-DESIGN section 12) lands later.
    pub fn push_history(&mut self, line: String) {
        if !line.is_empty() {
            self.history.push(line);
        }
    }

    /// Process one byte of input. Returns the EditorAction the main
    /// loop should react to.
    pub fn feed_byte(&mut self, byte: u8) -> EditorAction {
        // Drive the parser one byte at a time. Most bytes are
        // self-contained (Ground -> Action -> done); a few transition
        // through Escape / Csi / Utf8 multi-byte states.
        let action = match self.parser {
            ParserState::Ground => self.parse_ground(byte),
            ParserState::Escape => self.parse_escape(byte),
            ParserState::Csi { .. } => self.parse_csi(byte),
            ParserState::Utf8 { .. } => self.parse_utf8(byte),
        };
        self.apply(action)
    }

    /// Process a slice of input bytes. Returns the sequence of
    /// EditorActions in order. Useful for unit tests and for the
    /// main loop when stdin returns more than one byte per read.
    pub fn feed_bytes(&mut self, bytes: &[u8]) -> Vec<EditorAction> {
        let mut out = Vec::with_capacity(bytes.len());
        for &b in bytes {
            out.push(self.feed_byte(b));
        }
        out
    }

    /// Render the prompt + current buffer + cursor positioning as an
    /// ANSI byte sequence. The main loop emits this to stdout after
    /// any Redraw EditorAction.
    ///
    /// Strategy (single-line at U-4a; multi-line is U-4b):
    ///   1. \r       -- cursor to column 0
    ///   2. \x1b[K   -- erase to end of line
    ///   3. prompt + buffer
    ///   4. \r       -- cursor to column 0
    ///   5. \x1b[<n>C  -- cursor right n positions, where n =
    ///                    visible_width(prompt) + visible chars of
    ///                    buffer up to cursor.
    ///
    /// If `cursor_offset` ends up at column 0, we omit step 5
    /// (\x1b[0C is a no-op but emitting nothing is cleaner).
    pub fn render(&self, prompt: &str) -> String {
        let mut out = String::new();
        out.push('\r');
        out.push_str("\x1b[K");
        out.push_str(prompt);
        out.push_str(&self.buffer);
        // Position cursor: prompt_width + visible chars of buffer[..cursor].
        let prompt_w = crate::ansi::visible_width(prompt);
        let buffer_to_cursor = &self.buffer[..self.cursor];
        let buffer_w = crate::ansi::visible_width(buffer_to_cursor);
        let total = prompt_w + buffer_w;
        if total > 0 {
            out.push('\r');
            out.push_str(&format!("\x1b[{}C", total));
        }
        out
    }

    // -------------------------------------------------------------------------
    // Parser implementation.
    // -------------------------------------------------------------------------

    fn parse_ground(&mut self, byte: u8) -> Action {
        match byte {
            // C0 control characters -- emacs keybindings + ANSI ESC entry.
            0x01 => Action::CursorHome,    // Ctrl-A
            0x02 => Action::CursorLeft,    // Ctrl-B
            0x03 => Action::Cancel,        // Ctrl-C
            0x04 => Action::EofOrDelete,   // Ctrl-D
            0x05 => Action::CursorEnd,     // Ctrl-E
            0x06 => Action::CursorRight,   // Ctrl-F
            0x08 => Action::Backspace,     // Ctrl-H (some terminals)
            0x09 => Action::Ignore,        // Tab -- U-4d completion hook
            0x0a => Action::Accept,        // Ctrl-J / LF (Enter)
            0x0b => Action::KillToEnd,     // Ctrl-K
            0x0c => Action::ClearScreen,   // Ctrl-L
            0x0d => Action::Accept,        // Ctrl-M / CR (Enter)
            0x0e => Action::HistoryNext,   // Ctrl-N
            0x10 => Action::HistoryPrev,   // Ctrl-P
            0x12 => Action::Ignore,        // Ctrl-R -- U-4c history search
            0x15 => Action::KillToStart,   // Ctrl-U
            0x17 => Action::KillPrevWord,  // Ctrl-W
            0x19 => Action::Yank,          // Ctrl-Y
            0x1b => {                      // ESC -- enter CSI sequence
                self.parser = ParserState::Escape;
                Action::Ignore
            }
            0x7f => Action::Backspace,     // DEL (most terminals send this for Backspace)
            // Printable ASCII inserts directly.
            0x20..=0x7e => Action::InsertChar(byte as char),
            // C0 controls we don't bind do nothing.
            0x00..=0x1f => Action::Ignore,
            // UTF-8 leading byte -> enter Utf8 state.
            0xc2..=0xdf => {
                self.parser = ParserState::Utf8 {
                    buf: [byte, 0, 0, 0],
                    expected: 2,
                    have: 1,
                };
                Action::Ignore
            }
            0xe0..=0xef => {
                self.parser = ParserState::Utf8 {
                    buf: [byte, 0, 0, 0],
                    expected: 3,
                    have: 1,
                };
                Action::Ignore
            }
            0xf0..=0xf4 => {
                self.parser = ParserState::Utf8 {
                    buf: [byte, 0, 0, 0],
                    expected: 4,
                    have: 1,
                };
                Action::Ignore
            }
            // Invalid UTF-8 lead byte (0x80..=0xc1, 0xf5..=0xff) -- drop.
            _ => Action::Ignore,
        }
    }

    fn parse_escape(&mut self, byte: u8) -> Action {
        match byte {
            b'[' => {
                self.parser = ParserState::Csi {
                    params: [0; 4],
                    param_count: 0,
                    current_has_digits: false,
                };
                Action::Ignore
            }
            0x1b => {
                // ESC ESC -- restart the parser (the first ESC was
                // abandoned; the second begins a fresh sequence).
                Action::Ignore
            }
            _ => {
                // Unknown ESC <byte> -- abort the sequence. v1.x can
                // add Alt-key bindings (ESC <letter>) here.
                self.parser = ParserState::Ground;
                Action::Ignore
            }
        }
    }

    fn parse_csi(&mut self, byte: u8) -> Action {
        // Pull the params + state out for local mutation, write back
        // before any branch that doesn't return to Ground.
        let (mut params, mut param_count, mut current_has_digits) = match self.parser {
            ParserState::Csi {
                params,
                param_count,
                current_has_digits,
            } => (params, param_count, current_has_digits),
            _ => unreachable!(),
        };
        match byte {
            b'0'..=b'9' => {
                let slot = param_count as usize;
                if slot < params.len() {
                    let d = (byte - b'0') as u32;
                    params[slot] = params[slot].saturating_mul(10).saturating_add(d);
                    current_has_digits = true;
                }
                self.parser = ParserState::Csi {
                    params,
                    param_count,
                    current_has_digits,
                };
                Action::Ignore
            }
            b';' => {
                if param_count < params.len() as u8 - 1 {
                    param_count += 1;
                    current_has_digits = false;
                }
                self.parser = ParserState::Csi {
                    params,
                    param_count,
                    current_has_digits,
                };
                Action::Ignore
            }
            // Final bytes -- apply the action + reset to Ground.
            b'A' => {
                self.parser = ParserState::Ground;
                Action::HistoryPrev
            }
            b'B' => {
                self.parser = ParserState::Ground;
                Action::HistoryNext
            }
            b'C' => {
                self.parser = ParserState::Ground;
                Action::CursorRight
            }
            b'D' => {
                self.parser = ParserState::Ground;
                Action::CursorLeft
            }
            b'H' => {
                self.parser = ParserState::Ground;
                Action::CursorHome
            }
            b'F' => {
                self.parser = ParserState::Ground;
                Action::CursorEnd
            }
            b'~' => {
                // Tilde-terminated sequences: CSI <n> ~. n=1 Home,
                // n=3 Delete, n=4 End, n=5 PageUp, n=6 PageDown.
                let n = if current_has_digits {
                    params[0]
                } else {
                    0
                };
                self.parser = ParserState::Ground;
                match n {
                    1 | 7 => Action::CursorHome,
                    3 => Action::DeleteChar,
                    4 | 8 => Action::CursorEnd,
                    _ => Action::Ignore,
                }
            }
            _ => {
                // Unknown CSI final -- reset; v1.x can add SGR (m),
                // device-status (n), cursor-position (R), etc.
                self.parser = ParserState::Ground;
                Action::Ignore
            }
        }
    }

    fn parse_utf8(&mut self, byte: u8) -> Action {
        let (mut buf, expected, mut have) = match self.parser {
            ParserState::Utf8 { buf, expected, have } => (buf, expected, have),
            _ => unreachable!(),
        };
        if !(0x80..=0xbf).contains(&byte) {
            // Not a continuation byte -- the sequence is broken.
            // Drop the partial sequence; the offending byte will be
            // re-processed by Ground (a leading byte starts a fresh
            // sequence; a control byte triggers its action).
            self.parser = ParserState::Ground;
            return self.parse_ground(byte);
        }
        buf[have as usize] = byte;
        have += 1;
        if have == expected {
            self.parser = ParserState::Ground;
            // Validate + decode.
            match core::str::from_utf8(&buf[..have as usize]) {
                Ok(s) => {
                    if let Some(ch) = s.chars().next() {
                        return Action::InsertChar(ch);
                    }
                    Action::Ignore
                }
                Err(_) => Action::Ignore,
            }
        } else {
            self.parser = ParserState::Utf8 {
                buf,
                expected,
                have,
            };
            Action::Ignore
        }
    }

    // -------------------------------------------------------------------------
    // Action dispatch -- map a decoded Action onto buffer state.
    // -------------------------------------------------------------------------

    fn apply(&mut self, action: Action) -> EditorAction {
        match action {
            Action::InsertChar(ch) => self.do_insert(ch),
            Action::Backspace => self.do_backspace(),
            Action::DeleteChar => self.do_delete_char(),
            Action::CursorLeft => self.do_cursor_left(),
            Action::CursorRight => self.do_cursor_right(),
            Action::CursorHome => self.do_cursor_home(),
            Action::CursorEnd => self.do_cursor_end(),
            Action::KillToEnd => self.do_kill_to_end(),
            Action::KillToStart => self.do_kill_to_start(),
            Action::KillPrevWord => self.do_kill_prev_word(),
            Action::Yank => self.do_yank(),
            Action::HistoryPrev => self.do_history_prev(),
            Action::HistoryNext => self.do_history_next(),
            Action::Accept => self.do_accept(),
            Action::Cancel => self.do_cancel(),
            Action::EofOrDelete => self.do_eof_or_delete(),
            Action::ClearScreen => EditorAction::ClearScreen,
            Action::Ignore => EditorAction::NoChange,
        }
    }

    fn do_insert(&mut self, ch: char) -> EditorAction {
        let ch_len = ch.len_utf8();
        if self.buffer.len() + ch_len > MAX_BUFFER_LEN {
            return EditorAction::NoChange;
        }
        self.buffer.insert(self.cursor, ch);
        self.cursor += ch_len;
        EditorAction::Redraw
    }

    fn do_backspace(&mut self) -> EditorAction {
        if self.cursor == 0 {
            return EditorAction::NoChange;
        }
        let prev = self.prev_char_boundary(self.cursor);
        self.buffer.replace_range(prev..self.cursor, "");
        self.cursor = prev;
        EditorAction::Redraw
    }

    fn do_delete_char(&mut self) -> EditorAction {
        if self.cursor >= self.buffer.len() {
            return EditorAction::NoChange;
        }
        let next = self.next_char_boundary(self.cursor);
        self.buffer.replace_range(self.cursor..next, "");
        EditorAction::Redraw
    }

    fn do_cursor_left(&mut self) -> EditorAction {
        if self.cursor == 0 {
            return EditorAction::NoChange;
        }
        self.cursor = self.prev_char_boundary(self.cursor);
        EditorAction::Redraw
    }

    fn do_cursor_right(&mut self) -> EditorAction {
        if self.cursor >= self.buffer.len() {
            return EditorAction::NoChange;
        }
        self.cursor = self.next_char_boundary(self.cursor);
        EditorAction::Redraw
    }

    fn do_cursor_home(&mut self) -> EditorAction {
        if self.cursor == 0 {
            return EditorAction::NoChange;
        }
        self.cursor = 0;
        EditorAction::Redraw
    }

    fn do_cursor_end(&mut self) -> EditorAction {
        if self.cursor == self.buffer.len() {
            return EditorAction::NoChange;
        }
        self.cursor = self.buffer.len();
        EditorAction::Redraw
    }

    fn do_kill_to_end(&mut self) -> EditorAction {
        if self.cursor >= self.buffer.len() {
            return EditorAction::NoChange;
        }
        self.kill_buffer.clear();
        self.kill_buffer.push_str(&self.buffer[self.cursor..]);
        self.buffer.truncate(self.cursor);
        EditorAction::Redraw
    }

    fn do_kill_to_start(&mut self) -> EditorAction {
        if self.cursor == 0 {
            return EditorAction::NoChange;
        }
        self.kill_buffer.clear();
        self.kill_buffer.push_str(&self.buffer[..self.cursor]);
        self.buffer.replace_range(..self.cursor, "");
        self.cursor = 0;
        EditorAction::Redraw
    }

    fn do_kill_prev_word(&mut self) -> EditorAction {
        // Walk back across whitespace, then back across non-whitespace.
        // The killed range is everything from the resulting position
        // up to the original cursor.
        if self.cursor == 0 {
            return EditorAction::NoChange;
        }
        let bytes = self.buffer.as_bytes();
        let mut i = self.cursor;
        // Skip whitespace immediately before cursor.
        while i > 0 && is_whitespace_byte(bytes[i - 1]) {
            i = self.prev_char_boundary(i);
        }
        // Then skip non-whitespace.
        while i > 0 && !is_whitespace_byte(bytes[i - 1]) {
            i = self.prev_char_boundary(i);
        }
        if i == self.cursor {
            // Nothing changed (cursor was already on a boundary that
            // would yield zero motion -- unreachable if cursor > 0,
            // but defensively returns NoChange).
            return EditorAction::NoChange;
        }
        self.kill_buffer.clear();
        self.kill_buffer.push_str(&self.buffer[i..self.cursor]);
        self.buffer.replace_range(i..self.cursor, "");
        self.cursor = i;
        EditorAction::Redraw
    }

    fn do_yank(&mut self) -> EditorAction {
        if self.kill_buffer.is_empty() {
            return EditorAction::NoChange;
        }
        if self.buffer.len() + self.kill_buffer.len() > MAX_BUFFER_LEN {
            return EditorAction::NoChange;
        }
        // Clone to dodge the &mut borrow that insert_str would lock
        // against on self.kill_buffer.
        let to_insert = self.kill_buffer.clone();
        self.buffer.insert_str(self.cursor, &to_insert);
        self.cursor += to_insert.len();
        EditorAction::Redraw
    }

    fn do_history_prev(&mut self) -> EditorAction {
        if self.history.is_empty() {
            return EditorAction::NoChange;
        }
        let new_pos = match self.history_pos {
            None => {
                // Save the in-progress edit before jumping into history.
                self.saved_current.clear();
                self.saved_current.push_str(&self.buffer);
                self.history.len() - 1
            }
            Some(0) => 0,
            Some(i) => i - 1,
        };
        self.history_pos = Some(new_pos);
        self.buffer.clear();
        self.buffer.push_str(&self.history[new_pos]);
        self.cursor = self.buffer.len();
        EditorAction::Redraw
    }

    fn do_history_next(&mut self) -> EditorAction {
        match self.history_pos {
            None => EditorAction::NoChange,
            Some(i) if i + 1 < self.history.len() => {
                self.history_pos = Some(i + 1);
                self.buffer.clear();
                self.buffer.push_str(&self.history[i + 1]);
                self.cursor = self.buffer.len();
                EditorAction::Redraw
            }
            Some(_) => {
                // Walked past the most recent entry -- restore the
                // saved edit.
                self.history_pos = None;
                self.buffer.clear();
                self.buffer.push_str(&self.saved_current);
                self.cursor = self.buffer.len();
                self.saved_current.clear();
                EditorAction::Redraw
            }
        }
    }

    fn do_accept(&mut self) -> EditorAction {
        let line = core::mem::take(&mut self.buffer);
        self.cursor = 0;
        self.history_pos = None;
        self.saved_current.clear();
        EditorAction::Accept(line)
    }

    fn do_cancel(&mut self) -> EditorAction {
        self.buffer.clear();
        self.cursor = 0;
        self.history_pos = None;
        self.saved_current.clear();
        EditorAction::Cancel
    }

    fn do_eof_or_delete(&mut self) -> EditorAction {
        if self.buffer.is_empty() {
            EditorAction::Eof
        } else {
            self.do_delete_char()
        }
    }

    // -------------------------------------------------------------------------
    // UTF-8 boundary walking. The buffer is always valid UTF-8 (Rust's
    // String invariant), so str::is_char_boundary is exact.
    // -------------------------------------------------------------------------

    fn prev_char_boundary(&self, from: usize) -> usize {
        let mut i = from;
        while i > 0 {
            i -= 1;
            if self.buffer.is_char_boundary(i) {
                return i;
            }
        }
        0
    }

    fn next_char_boundary(&self, from: usize) -> usize {
        let mut i = from;
        let n = self.buffer.len();
        while i < n {
            i += 1;
            if self.buffer.is_char_boundary(i) {
                return i;
            }
        }
        n
    }
}

fn is_whitespace_byte(b: u8) -> bool {
    matches!(b, b' ' | b'\t' | b'\n' | b'\r')
}

// =============================================================================
// Tests -- cfg(test); cargo test on a host target would exercise these.
// The production build (aarch64-unknown-none) strips them at compile.
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;

    fn feed(le: &mut LineEditor, bytes: &[u8]) -> Vec<EditorAction> {
        le.feed_bytes(bytes)
    }

    #[test]
    fn insert_ascii_advances_cursor() {
        let mut le = LineEditor::new();
        let actions = feed(&mut le, b"abc");
        assert_eq!(actions.len(), 3);
        for a in &actions {
            assert_eq!(*a, EditorAction::Redraw);
        }
        assert_eq!(le.buffer(), "abc");
        assert_eq!(le.cursor(), 3);
    }

    #[test]
    fn enter_emits_accept_and_clears_buffer() {
        let mut le = LineEditor::new();
        feed(&mut le, b"hello");
        let last = le.feed_byte(b'\r');
        assert_eq!(last, EditorAction::Accept(String::from("hello")));
        assert_eq!(le.buffer(), "");
        assert_eq!(le.cursor(), 0);
    }

    #[test]
    fn lf_also_accepts() {
        let mut le = LineEditor::new();
        feed(&mut le, b"x");
        let last = le.feed_byte(b'\n');
        assert_eq!(last, EditorAction::Accept(String::from("x")));
    }

    #[test]
    fn ctrl_c_cancels() {
        let mut le = LineEditor::new();
        feed(&mut le, b"abc");
        let r = le.feed_byte(0x03);
        assert_eq!(r, EditorAction::Cancel);
        assert_eq!(le.buffer(), "");
    }

    #[test]
    fn ctrl_d_on_empty_yields_eof() {
        let mut le = LineEditor::new();
        let r = le.feed_byte(0x04);
        assert_eq!(r, EditorAction::Eof);
    }

    #[test]
    fn ctrl_d_mid_string_deletes_char() {
        let mut le = LineEditor::new();
        feed(&mut le, b"abc");
        // Move cursor to start.
        le.feed_byte(0x01);
        let r = le.feed_byte(0x04);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "bc");
        assert_eq!(le.cursor(), 0);
    }

    #[test]
    fn backspace_at_start_is_noop() {
        let mut le = LineEditor::new();
        let r = le.feed_byte(0x7f);
        assert_eq!(r, EditorAction::NoChange);
    }

    #[test]
    fn backspace_deletes_prev_char() {
        let mut le = LineEditor::new();
        feed(&mut le, b"abc");
        let r = le.feed_byte(0x7f);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "ab");
        assert_eq!(le.cursor(), 2);
    }

    #[test]
    fn cursor_motion_emacs() {
        let mut le = LineEditor::new();
        feed(&mut le, b"hello world");
        assert_eq!(le.cursor(), 11);
        // Ctrl-A
        assert_eq!(le.feed_byte(0x01), EditorAction::Redraw);
        assert_eq!(le.cursor(), 0);
        // Ctrl-E
        assert_eq!(le.feed_byte(0x05), EditorAction::Redraw);
        assert_eq!(le.cursor(), 11);
        // Ctrl-B
        assert_eq!(le.feed_byte(0x02), EditorAction::Redraw);
        assert_eq!(le.cursor(), 10);
        // Ctrl-F
        assert_eq!(le.feed_byte(0x06), EditorAction::Redraw);
        assert_eq!(le.cursor(), 11);
    }

    #[test]
    fn arrow_keys_via_csi() {
        let mut le = LineEditor::new();
        feed(&mut le, b"hello");
        // CSI D = Left arrow -> cursor left.
        assert_eq!(le.feed_bytes(b"\x1b[D"), vec![
            EditorAction::NoChange,  // ESC
            EditorAction::NoChange,  // [
            EditorAction::Redraw,    // D
        ]);
        assert_eq!(le.cursor(), 4);
        // CSI C = Right arrow.
        le.feed_bytes(b"\x1b[C");
        assert_eq!(le.cursor(), 5);
        // CSI H = Home.
        le.feed_bytes(b"\x1b[H");
        assert_eq!(le.cursor(), 0);
        // CSI F = End.
        le.feed_bytes(b"\x1b[F");
        assert_eq!(le.cursor(), 5);
    }

    #[test]
    fn csi_delete_via_tilde() {
        let mut le = LineEditor::new();
        feed(&mut le, b"abc");
        le.feed_byte(0x01); // Ctrl-A -> cursor=0
        // CSI 3 ~ = Delete
        le.feed_bytes(b"\x1b[3~");
        assert_eq!(le.buffer(), "bc");
        assert_eq!(le.cursor(), 0);
    }

    #[test]
    fn csi_home_end_via_tilde() {
        let mut le = LineEditor::new();
        feed(&mut le, b"abc");
        // CSI 1 ~ = Home
        le.feed_bytes(b"\x1b[1~");
        assert_eq!(le.cursor(), 0);
        // CSI 4 ~ = End
        le.feed_bytes(b"\x1b[4~");
        assert_eq!(le.cursor(), 3);
    }

    #[test]
    fn kill_to_end_then_yank() {
        let mut le = LineEditor::new();
        feed(&mut le, b"hello world");
        // Ctrl-A then Ctrl-K -> kill_buffer = "hello world", buffer = ""
        le.feed_byte(0x01);
        let r = le.feed_byte(0x0b);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "");
        assert_eq!(le.kill_buffer(), "hello world");
        // Ctrl-Y yanks it back.
        let r = le.feed_byte(0x19);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "hello world");
        assert_eq!(le.cursor(), 11);
    }

    #[test]
    fn kill_to_start() {
        let mut le = LineEditor::new();
        feed(&mut le, b"hello world");
        // Ctrl-U from end -> kills "hello world"
        let r = le.feed_byte(0x15);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "");
        assert_eq!(le.kill_buffer(), "hello world");
    }

    #[test]
    fn kill_prev_word() {
        let mut le = LineEditor::new();
        feed(&mut le, b"hello world ");
        // Ctrl-W -- skips trailing space then "world".
        let r = le.feed_byte(0x17);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "hello ");
        assert_eq!(le.kill_buffer(), "world ");
    }

    #[test]
    fn utf8_multi_byte_insert() {
        let mut le = LineEditor::new();
        // "héllo" = h \xc3 \xa9 l l o -> 6 bytes, 5 chars.
        feed(&mut le, "héllo".as_bytes());
        assert_eq!(le.buffer(), "héllo");
        assert_eq!(le.cursor(), 6); // byte index
    }

    #[test]
    fn utf8_backspace_walks_char_boundary() {
        let mut le = LineEditor::new();
        feed(&mut le, "héllo".as_bytes());
        // Backspace deletes 'o'.
        le.feed_byte(0x7f);
        assert_eq!(le.buffer(), "héll");
        // Backspace deletes 'l', 'l', then 'é' (which is 2 bytes).
        le.feed_byte(0x7f);
        le.feed_byte(0x7f);
        assert_eq!(le.buffer(), "hé");
        assert_eq!(le.cursor(), 3);
        le.feed_byte(0x7f);
        assert_eq!(le.buffer(), "h");
        assert_eq!(le.cursor(), 1);
    }

    #[test]
    fn invalid_utf8_continuation_drops_seq() {
        let mut le = LineEditor::new();
        // 0xc3 starts a 2-byte sequence; following 'a' (0x61) breaks it.
        // The parser drops the partial seq and re-processes 'a' on Ground.
        le.feed_byte(0xc3);
        le.feed_byte(b'a');
        assert_eq!(le.buffer(), "a");
    }

    #[test]
    fn esc_esc_resets_parser() {
        let mut le = LineEditor::new();
        feed(&mut le, b"\x1b\x1b");
        // Both ESCs should leave us in Ground; subsequent 'a' is inserted.
        le.feed_byte(b'a');
        assert_eq!(le.buffer(), "a");
    }

    #[test]
    fn ctrl_l_emits_clear_screen() {
        let mut le = LineEditor::new();
        feed(&mut le, b"abc");
        let r = le.feed_byte(0x0c);
        assert_eq!(r, EditorAction::ClearScreen);
        // State unchanged.
        assert_eq!(le.buffer(), "abc");
        assert_eq!(le.cursor(), 3);
    }

    #[test]
    fn history_prev_navigates_to_most_recent() {
        let mut le = LineEditor::new();
        le.push_history(String::from("first"));
        le.push_history(String::from("second"));
        feed(&mut le, b"in-progress");
        // Ctrl-P should pull most recent.
        let r = le.feed_byte(0x10);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "second");
        // Ctrl-P again -> "first".
        le.feed_byte(0x10);
        assert_eq!(le.buffer(), "first");
        // Ctrl-N -> "second" again.
        le.feed_byte(0x0e);
        assert_eq!(le.buffer(), "second");
        // Ctrl-N -> restore in-progress edit.
        le.feed_byte(0x0e);
        assert_eq!(le.buffer(), "in-progress");
    }

    #[test]
    fn history_prev_with_empty_history_noop() {
        let mut le = LineEditor::new();
        feed(&mut le, b"abc");
        let r = le.feed_byte(0x10);
        assert_eq!(r, EditorAction::NoChange);
        assert_eq!(le.buffer(), "abc");
    }

    #[test]
    fn render_single_line() {
        let mut le = LineEditor::new();
        feed(&mut le, b"hello");
        let s = le.render("> ");
        // Expected shape: \r \x1b[K > hello \r \x1b[7C
        assert!(s.starts_with("\r\x1b[K"));
        assert!(s.contains("> hello"));
        // Cursor at column 7 (prompt width 2 + buffer width 5).
        assert!(s.ends_with("\x1b[7C"));
    }

    #[test]
    fn render_at_column_zero_omits_cursor_motion() {
        let le = LineEditor::new();
        let s = le.render("");
        // Empty prompt + empty buffer -> no \x1b[<n>C.
        assert!(!s.contains("\x1b["[..].trim_start_matches('\x1b')) || !s.ends_with('C'));
    }

    #[test]
    fn tab_is_ignored_at_u4a() {
        // Tab completion is U-4d; at U-4a Ctrl-I (Tab) is no-op.
        let mut le = LineEditor::new();
        feed(&mut le, b"ab");
        let r = le.feed_byte(0x09);
        assert_eq!(r, EditorAction::NoChange);
        assert_eq!(le.buffer(), "ab");
    }
}
