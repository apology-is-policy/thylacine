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
//   - render(prompt: &str) -> String emitter (single-line)
//
// What's at U-4b:
//   - BalanceState struct + balance(s: &str) lightweight tracker --
//     brackets ({}/()/[]), single + double quotes, '\\' escape inside
//     double-quoted + unquoted contexts, '#' line-comments, trailing
//     unescaped backslash. Per UTOPIA-SHELL-DESIGN.md section 5.3 +
//     11.4 trip hazard: this is intentionally lightweight; the U-5
//     parser is authoritative.
//   - Enter behaviour: if !balance(buffer).is_balanced(), insert '\n'
//     at cursor instead of submitting Accept. If buffer ends in an
//     unescaped '\\' AND cursor is at end-of-buffer, strip the
//     trailing backslash before inserting '\n' (rc/sh trailing-
//     backslash line continuation).
//   - Multi-line render: emits prompt on line 0, then for each
//     subsequent buffer line emits "\r\n\x1b[K<continuation_prefix>",
//     where continuation_prefix is per UTOPIA-VISUAL.md section 3.2 --
//     padded so the user's continuation text aligns with the user's
//     first-line text; the receded-steel `⋮` glyph lives at column
//     (prompt_width - 2).
//   - Backspace + Delete auto-join lines (falls out of the existing
//     UTF-8 boundary-walking code: the '\n' is just another char to
//     delete).
//
// What's deferred:
//   - Column-preserving Up/Down cursor nav across multi-line buffer
//     (in U-4b, Up/Down stay as history-only as in U-4a -- matches
//     bash's behaviour; the zsh/fish "Up = cursor-up in multi-line,
//     history otherwise" semantics can land at U-4c or v1.x).
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
// BalanceState -- lightweight bracket / quote / backslash tracker (U-4b).
// =============================================================================
//
// Per UTOPIA-SHELL-DESIGN.md section 11.4 trip hazard: this is a
// MINIMAL tracker for the line editor to decide "submit on Enter?" vs
// "insert newline?". It is NOT a tokenizer; the U-5 parser is the
// authoritative parse. The tracker handles:
//
//   - Brace depth `{` `}` (per-bracket-type counters so `{)` is
//     detected as having mismatched closers).
//   - Paren depth `(` `)`.
//   - Bracket depth `[` `]`.
//   - Single-quoted `'...'` (literal; no escapes; brackets inside don't count).
//   - Double-quoted `"..."` (interpolating; `\\` escapes the next char;
//     brackets inside don't count).
//   - `#` comments to end-of-line in unquoted contexts.
//   - Trailing unescaped `\\` (line-continuation trigger).
//
// Depth counters are i32 (not u32) so stray closers like `}` at top
// level go negative; is_balanced treats negative depth as "balanced"
// (the line is malformed but submitting lets the parser report the
// error, which is the natural shell experience).

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct BalanceState {
    pub brace_depth: i32,
    pub paren_depth: i32,
    pub bracket_depth: i32,
    pub in_single_quote: bool,
    pub in_double_quote: bool,
    /// True iff the buffer's last unquoted character (after any escape
    /// run) was an UNESCAPED `\\`. Used by Enter handling to strip the
    /// trailing backslash before inserting `\n` (rc/sh trailing-
    /// backslash line continuation).
    pub trailing_unescaped_backslash: bool,
}

impl BalanceState {
    /// True iff everything is closed and no continuation is awaited.
    /// Used by Enter handling -- if true, submit (Accept); if false,
    /// insert `\n` at cursor.
    pub fn is_balanced(&self) -> bool {
        self.brace_depth <= 0
            && self.paren_depth <= 0
            && self.bracket_depth <= 0
            && !self.in_single_quote
            && !self.in_double_quote
            && !self.trailing_unescaped_backslash
    }

    /// Inverse of is_balanced -- the line editor's reason to continue
    /// to a new line.
    pub fn awaits_continuation(&self) -> bool {
        !self.is_balanced()
    }
}

/// Walk `s` once, producing a BalanceState reflecting its end-of-string
/// bracket / quote / escape state. O(s.len()).
pub fn balance(s: &str) -> BalanceState {
    let mut st = BalanceState::default();
    let mut escaped = false;
    // The trailing-backslash flag is "is the next-to-last token an
    // unescaped backslash at EOS?". We update it as we walk: clear on
    // every char, set on an unescaped `\\` in an unquoted-or-double-
    // quote context. After the loop, its final value is what we want.
    //
    // Implementation: when we see `\\` and we'd set escaped=true, also
    // set trailing_unescaped_backslash=true; we clear it on every
    // OTHER char so by EOS only the trailing-most `\\` remains.

    let mut chars = s.chars();
    while let Some(ch) = chars.next() {
        let was_unescaped_backslash_about_to_be_set = !escaped
            && !st.in_single_quote
            && ch == '\\';

        if escaped {
            // Previous backslash escaped this char.
            escaped = false;
            st.trailing_unescaped_backslash = false;
            continue;
        }

        if st.in_single_quote {
            if ch == '\'' {
                st.in_single_quote = false;
            }
            st.trailing_unescaped_backslash = false;
            continue;
        }

        if st.in_double_quote {
            match ch {
                '"' => st.in_double_quote = false,
                '\\' => escaped = true,
                _ => {}
            }
            st.trailing_unescaped_backslash = was_unescaped_backslash_about_to_be_set;
            continue;
        }

        // Unquoted.
        match ch {
            '\'' => st.in_single_quote = true,
            '"' => st.in_double_quote = true,
            '\\' => escaped = true,
            '{' => st.brace_depth += 1,
            '}' => st.brace_depth -= 1,
            '(' => st.paren_depth += 1,
            ')' => st.paren_depth -= 1,
            '[' => st.bracket_depth += 1,
            ']' => st.bracket_depth -= 1,
            '#' => {
                // Comment to end-of-line. Skip until '\n' (or EOS).
                for c in chars.by_ref() {
                    if c == '\n' {
                        break;
                    }
                }
                // The comment's contents and the terminating newline
                // (if any) shouldn't leave a trailing backslash.
                st.trailing_unescaped_backslash = false;
                continue;
            }
            _ => {}
        }
        st.trailing_unescaped_backslash = was_unescaped_backslash_about_to_be_set;
    }

    st
}

// =============================================================================
// Continuation prefix (U-4b) -- the ⋮ + padding emitted at the start of
// every multi-line continuation line. Per UTOPIA-VISUAL.md section 3.2.
// =============================================================================
//
// The continuation prefix occupies prompt_width columns total:
//   - (prompt_width - 2) leading spaces
//   - the `⋮` glyph at column (prompt_width - 2) in PATH role colour
//   - one trailing space (so user's continuation text aligns with
//     user's first-line text at column prompt_width)
//
// For prompt_width < 2, the prefix degenerates to just `⋮` (1 visible
// column), keeping a sentinel character visible but losing the
// alignment property. Most real prompts are >= 2 columns ("> " is
// the minimum disciplined Pale Fire prompt).

fn continuation_prefix(prompt_width: usize) -> alloc::string::String {
    let mut s = alloc::string::String::new();
    if prompt_width >= 2 {
        for _ in 0..(prompt_width - 2) {
            s.push(' ');
        }
        s.push_str(&crate::ansi::fg(crate::palette::Role::Path, "\u{22ee}"));
        s.push(' ');
    } else {
        s.push_str(&crate::ansi::fg(crate::palette::Role::Path, "\u{22ee}"));
    }
    s
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
    /// Ctrl-R -- enter incremental search mode (U-4c). In search mode,
    /// re-pressing Ctrl-R cycles to the next older match.
    SearchHistory,
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

/// Maximum in-memory history entries (U-4c). Per UTOPIA-SHELL-DESIGN.md
/// section 12.4 (`$HISTSIZE` default). Older entries are dropped when
/// the cap is exceeded. v1.x can add disk-backed history at
/// ~/.config/utopia/history.
const HISTORY_CAP: usize = 10_000;

/// Mode of the line editor (U-4c). Normal is the default; Search is
/// entered via Ctrl-R and exits on Enter (accept), Ctrl-G/Ctrl-C
/// (cancel + restore), or implicitly when any other action would
/// require leaving search.
#[derive(Debug, Clone, PartialEq, Eq)]
enum LineEditorMode {
    Normal,
    Search {
        /// What the user has typed since entering search mode.
        query: String,
        /// History index of the currently displayed match, if any.
        match_index: Option<usize>,
        /// Saved buffer + cursor to restore on Cancel.
        saved_buffer: String,
        saved_cursor: usize,
    },
}

pub struct LineEditor {
    buffer: String,
    /// Cursor as a byte index into `buffer` -- always on a UTF-8
    /// character boundary.
    cursor: usize,
    /// Most-recent kill for Ctrl-Y yank.
    kill_buffer: String,
    /// In-memory history (capped at HISTORY_CAP entries).
    history: Vec<String>,
    /// History navigation position: None == editing current; Some(i)
    /// == viewing history[i]. When the user starts navigating away
    /// from the current edit, the current buffer is saved in
    /// `saved_current`; navigating back to None restores it.
    history_pos: Option<usize>,
    saved_current: String,
    parser: ParserState,
    /// U-4c: search vs normal. Normal at U-4a; Search added at U-4c.
    mode: LineEditorMode,
    /// U-4c: desired visible column for column-preserving Up/Down nav
    /// across multi-line buffers. Set on Up/Down; cleared on any
    /// horizontal motion or edit. None == "use current column".
    desired_col: Option<usize>,
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
            mode: LineEditorMode::Normal,
            desired_col: None,
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
        self.mode = LineEditorMode::Normal;
        self.desired_col = None;
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

    /// Append a line to history. Empty lines are dropped. When the
    /// in-memory history reaches HISTORY_CAP entries, the oldest entry
    /// is removed to make room (oldest-first eviction).
    ///
    /// v1.0 history is in-memory only; the disk-backed history file
    /// at ~/.config/utopia/history (per UTOPIA-SHELL-DESIGN section
    /// 12) lands later.
    pub fn push_history(&mut self, line: String) {
        if line.is_empty() {
            return;
        }
        if self.history.len() >= HISTORY_CAP {
            // Evict oldest. Vec::remove(0) is O(n) but n <= HISTORY_CAP
            // (10000); each push amortizes to O(1) at steady state
            // since most pushes don't evict, only the trim boundary
            // does. v1.x can swap to a VecDeque ring if profiling
            // says otherwise.
            self.history.remove(0);
        }
        self.history.push(line);
    }

    /// True iff the editor is currently in incremental-search mode (U-4c).
    pub fn is_searching(&self) -> bool {
        matches!(self.mode, LineEditorMode::Search { .. })
    }

    /// In search mode, return the current query string. None when in
    /// Normal mode.
    pub fn search_query(&self) -> Option<&str> {
        match &self.mode {
            LineEditorMode::Search { query, .. } => Some(query.as_str()),
            _ => None,
        }
    }

    /// In search mode, return the history index of the current match.
    /// None when in Normal mode OR when the query has no match.
    pub fn search_match_index(&self) -> Option<usize> {
        match &self.mode {
            LineEditorMode::Search { match_index, .. } => *match_index,
            _ => None,
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
    /// Single-line strategy:
    ///   1. \r       -- cursor to column 0
    ///   2. \x1b[K   -- erase to end of line
    ///   3. prompt + buffer
    ///   4. \r + \x1b[<n>C  -- position cursor at prompt_width +
    ///      visible_chars_of_buffer_up_to_cursor.
    ///
    /// Multi-line strategy (U-4b; triggered when buffer contains '\n'):
    ///   1. \r\x1b[K -- clear current line
    ///   2. prompt + first line of buffer
    ///   3. For each subsequent buffer line:
    ///      "\r\n\x1b[K" + continuation_prefix + line content
    ///      (continuation_prefix per UTOPIA-VISUAL.md section 3.2 --
    ///      padded so the user's text aligns across lines.)
    ///   4. Position cursor: \x1b[<n>F (cursor up n lines + col 0) if
    ///      cursor is above the last emitted line, then \x1b[<col>C
    ///      to move right to the target column (= prompt_width OR
    ///      continuation_prefix width + visible_chars_in_cursor_line_up_to_cursor).
    ///
    /// Multi-line caveat (U-6 will revisit): if the previous render
    /// occupied more lines than this one (e.g. user deleted content
    /// shrinking the buffer), the trailing lines on screen are NOT
    /// cleared. U-6 will track prev_render_lines + emit \x1b[J to
    /// clear "from cursor to end of screen" at start of render. For
    /// U-4b the boot probe only checks emitted bytes (not screen
    /// state) so this is invisible.
    pub fn render(&self, prompt: &str) -> String {
        // U-4c: in Search mode, render shows the readline-style
        // search prompt + the matched line (or empty if no match).
        // Cursor positions at the end of the query inside the
        // (reverse-i-search)`...': prefix.
        if let LineEditorMode::Search {
            query, match_index, ..
        } = &self.mode
        {
            return self.render_search(query, *match_index);
        }
        let prompt_w = crate::ansi::visible_width(prompt);
        let cont = continuation_prefix(prompt_w);
        let cont_w = crate::ansi::visible_width(&cont);

        let mut out = String::new();
        out.push('\r');
        out.push_str("\x1b[K");

        // Emit each buffer line.
        let mut line_iter = self.buffer.split('\n');
        if let Some(first) = line_iter.next() {
            out.push_str(prompt);
            out.push_str(first);
        }
        let mut total_lines = 1usize;
        for line in line_iter {
            out.push_str("\r\n\x1b[K");
            out.push_str(&cont);
            out.push_str(line);
            total_lines += 1;
        }

        // Position cursor.
        //
        // cursor_line = number of '\n' chars in buffer[..cursor].
        let bytes_up_to_cursor = &self.buffer[..self.cursor];
        let cursor_line = bytes_up_to_cursor.matches('\n').count();
        // Cursor-line start byte = position just AFTER the last '\n' in
        // buffer[..cursor], or 0 if none.
        let cursor_line_start = bytes_up_to_cursor
            .rfind('\n')
            .map(|i| i + 1)
            .unwrap_or(0);
        let col_in_line = crate::ansi::visible_width(&self.buffer[cursor_line_start..self.cursor]);

        // After emitting all lines, the terminal cursor is at the END
        // of the last line. Move up to the cursor's line.
        let lines_to_up = total_lines - 1 - cursor_line;
        if lines_to_up > 0 {
            out.push_str(&format!("\r\x1b[{}F", lines_to_up));
        } else {
            // Stay on current line.
            out.push('\r');
        }

        // Move right to target column.
        let prefix_w = if cursor_line == 0 { prompt_w } else { cont_w };
        let target_col = prefix_w + col_in_line;
        if target_col > 0 {
            out.push_str(&format!("\x1b[{}C", target_col));
        }

        out
    }

    /// U-4c: render in incremental-search mode. Emits readline-style
    /// `(reverse-i-search)`<query>': <matched_line>` prefix with the
    /// cursor positioned at the end of the query (inside the prefix).
    /// On no match (query has no substring hits in history), emits
    /// `(failed reverse-i-search)`<query>':` with empty matched line.
    fn render_search(&self, query: &str, match_index: Option<usize>) -> String {
        let mut out = String::new();
        out.push('\r');
        out.push_str("\x1b[K");
        let (prefix, matched) = match match_index {
            Some(i) => (
                format!("(reverse-i-search)`{}': ", query),
                self.history
                    .get(i)
                    .map(|s| String::from(s.as_str()))
                    .unwrap_or_default(),
            ),
            None => (
                format!("(failed reverse-i-search)`{}': ", query),
                String::new(),
            ),
        };
        out.push_str(&prefix);
        out.push_str(&matched);
        // Cursor at end of query (between `' and `:`). Position is:
        //   prefix's visible_width is the length of the leading text
        //   up to and including the closing backtick + space.
        //   Actually the cursor should sit RIGHT AFTER the query (just
        //   before the closing `'`). That position equals
        //   visible_width("(reverse-i-search)`") + query_chars.
        let leading = if match_index.is_some() {
            "(reverse-i-search)`"
        } else {
            "(failed reverse-i-search)`"
        };
        let target_col =
            crate::ansi::visible_width(leading) + crate::ansi::visible_width(query);
        out.push('\r');
        if target_col > 0 {
            out.push_str(&format!("\x1b[{}C", target_col));
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
            0x12 => Action::SearchHistory, // Ctrl-R -- U-4c incremental search
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
        // U-4c: in Search mode, action dispatch is different. We
        // handle the search-mode-relevant subset here + cancel +
        // re-dispatch for everything else (a minimal interactive
        // search; v1.x can refine to readline's full set of bindings).
        if self.is_searching() {
            return self.apply_in_search(action);
        }
        // Most actions reset desired_col (the column-preserving Up/Down
        // tracker). Up/Down preserve it; only ascending/descending
        // through lines should sticky-stick to the same column.
        let preserves_desired_col = matches!(
            action,
            Action::HistoryPrev | Action::HistoryNext | Action::Ignore | Action::ClearScreen
        );
        if !preserves_desired_col {
            self.desired_col = None;
        }
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
            Action::SearchHistory => self.do_enter_search(),
            Action::Ignore => EditorAction::NoChange,
        }
    }

    /// Action dispatch when in Search mode. Only a few actions matter:
    ///   - InsertChar(ch)    -> append to query, re-search
    ///   - Backspace         -> drop from query, re-search
    ///   - SearchHistory     -> step backward to next-older match
    ///   - Accept            -> accept current match as buffer + submit
    ///   - Cancel            -> restore saved state, exit search
    ///   - EofOrDelete       -> cancel (Ctrl-D in readline acts as cancel)
    ///   - HistoryPrev       -> alias for SearchHistory (step back)
    ///   - HistoryNext       -> step forward to next-newer match
    ///   - Anything else     -> cancel search, do NOT re-dispatch.
    ///                          (v1.x readline-equivalent: cancel +
    ///                          re-dispatch the action in Normal mode.)
    fn apply_in_search(&mut self, action: Action) -> EditorAction {
        match action {
            Action::InsertChar(ch) => self.do_search_append(ch),
            Action::Backspace => self.do_search_backspace(),
            Action::SearchHistory | Action::HistoryPrev => self.do_search_step_back(),
            Action::HistoryNext => self.do_search_step_forward(),
            Action::Accept => self.do_search_accept(),
            Action::Cancel | Action::EofOrDelete => self.do_search_cancel(),
            Action::Ignore => EditorAction::NoChange,
            // Cancel for anything else (cursor motion, kill, etc.).
            // The user is in search mode; the only sensible default
            // for an unfamiliar key is to exit search + return to
            // Normal mode without applying. The user can re-press the
            // key in Normal mode if they wanted that.
            _ => self.do_search_cancel(),
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
        // U-4c smart Up: when the buffer has '\n' AND the cursor is
        // NOT on the first line, navigate cursor up one line
        // (column-preserving) instead of doing history-prev.
        if self.buffer.contains('\n') && self.line_start_byte(self.cursor) > 0 {
            return self.do_cursor_up_line();
        }
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
        // U-4c smart Down: when the buffer has '\n' AND the cursor is
        // NOT on the last line, navigate cursor down one line.
        if self.buffer.contains('\n') && self.line_end_byte(self.cursor) < self.buffer.len() {
            return self.do_cursor_down_line();
        }
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

    // -------------------------------------------------------------------------
    // U-4c: multi-line cursor up/down helpers + line-position helpers.
    // -------------------------------------------------------------------------

    /// Byte index of the start of the line containing `cursor`. Equals
    /// 0 for the first line; equals (last '\n' + 1) otherwise.
    fn line_start_byte(&self, cursor: usize) -> usize {
        self.buffer[..cursor].rfind('\n').map(|i| i + 1).unwrap_or(0)
    }

    /// Byte index of the end of the line containing `cursor` (the '\n'
    /// itself, or `buffer.len()` for the last line).
    fn line_end_byte(&self, cursor: usize) -> usize {
        self.buffer[cursor..]
            .find('\n')
            .map(|i| cursor + i)
            .unwrap_or(self.buffer.len())
    }

    /// Visible column of cursor within its line (UTF-8 aware via
    /// libutopia::ansi::visible_width).
    fn col_in_line(&self, cursor: usize) -> usize {
        let line_start = self.line_start_byte(cursor);
        crate::ansi::visible_width(&self.buffer[line_start..cursor])
    }

    /// Walk `line` until we reach the `target_col`-th visible char (or
    /// the end of the line). Returns the byte offset.
    fn byte_offset_at_col(line: &str, target_col: usize) -> usize {
        let mut col = 0usize;
        let mut last_byte = 0usize;
        for (i, ch) in line.char_indices() {
            if col >= target_col {
                return i;
            }
            col += 1;
            last_byte = i + ch.len_utf8();
        }
        last_byte
    }

    fn do_cursor_up_line(&mut self) -> EditorAction {
        let cur_line_start = self.line_start_byte(self.cursor);
        if cur_line_start == 0 {
            return EditorAction::NoChange;
        }
        let prev_line_end = cur_line_start - 1; // the '\n'
        let prev_line_start = self.line_start_byte(prev_line_end);
        let col = self
            .desired_col
            .unwrap_or_else(|| self.col_in_line(self.cursor));
        let prev_line = &self.buffer[prev_line_start..prev_line_end];
        let offset = Self::byte_offset_at_col(prev_line, col);
        self.cursor = prev_line_start + offset;
        self.desired_col = Some(col);
        EditorAction::Redraw
    }

    fn do_cursor_down_line(&mut self) -> EditorAction {
        let cur_line_end = self.line_end_byte(self.cursor);
        if cur_line_end == self.buffer.len() {
            return EditorAction::NoChange;
        }
        let next_line_start = cur_line_end + 1; // past the '\n'
        let next_line_end = self.line_end_byte(next_line_start);
        let col = self
            .desired_col
            .unwrap_or_else(|| self.col_in_line(self.cursor));
        let next_line = &self.buffer[next_line_start..next_line_end];
        let offset = Self::byte_offset_at_col(next_line, col);
        self.cursor = next_line_start + offset;
        self.desired_col = Some(col);
        EditorAction::Redraw
    }

    // -------------------------------------------------------------------------
    // U-4c: incremental-search mode (Ctrl-R).
    // -------------------------------------------------------------------------

    /// Find the index of the newest history entry that contains
    /// `query` as a substring AND has index <= `upto_idx`. Used by
    /// `step_back` (cycling to next-older match). Returns None if no
    /// match found.
    fn search_history_backward(&self, query: &str, upto_idx: usize) -> Option<usize> {
        if query.is_empty() {
            return None;
        }
        let mut i = upto_idx;
        loop {
            if self.history[i].contains(query) {
                return Some(i);
            }
            if i == 0 {
                return None;
            }
            i -= 1;
        }
    }

    /// Find the index of the oldest history entry that contains
    /// `query` as a substring AND has index >= `from_idx`. Used by
    /// `step_forward` (cycling toward newer matches).
    fn search_history_forward(&self, query: &str, from_idx: usize) -> Option<usize> {
        if query.is_empty() {
            return None;
        }
        let n = self.history.len();
        for i in from_idx..n {
            if self.history[i].contains(query) {
                return Some(i);
            }
        }
        None
    }

    fn do_enter_search(&mut self) -> EditorAction {
        // Save current buffer + cursor; switch to Search mode with
        // empty query + no match.
        let saved_buffer = core::mem::take(&mut self.buffer);
        let saved_cursor = self.cursor;
        self.buffer.clear();
        self.cursor = 0;
        self.mode = LineEditorMode::Search {
            query: String::new(),
            match_index: None,
            saved_buffer,
            saved_cursor,
        };
        EditorAction::Redraw
    }

    fn do_search_append(&mut self, ch: char) -> EditorAction {
        // Compute new query + new match BEFORE re-entering self.mode
        // mutably, so the &self borrows for history lookup don't
        // overlap the &mut self.mode borrow.
        let new_query = match &self.mode {
            LineEditorMode::Search { query, .. } => {
                let mut q = query.clone();
                q.push(ch);
                q
            }
            _ => return EditorAction::NoChange,
        };
        let new_match = if self.history.is_empty() {
            None
        } else {
            self.search_history_backward(&new_query, self.history.len() - 1)
        };
        if let LineEditorMode::Search {
            query, match_index, ..
        } = &mut self.mode
        {
            *query = new_query;
            *match_index = new_match;
        }
        EditorAction::Redraw
    }

    fn do_search_backspace(&mut self) -> EditorAction {
        // Compute new query + new match BEFORE re-entering self.mode
        // mutably (same pattern as do_search_append).
        let new_query = match &self.mode {
            LineEditorMode::Search { query, .. } => {
                if query.is_empty() {
                    return EditorAction::NoChange;
                }
                let mut q = query.clone();
                q.pop();
                q
            }
            _ => return EditorAction::NoChange,
        };
        let new_match = if new_query.is_empty() || self.history.is_empty() {
            None
        } else {
            self.search_history_backward(&new_query, self.history.len() - 1)
        };
        if let LineEditorMode::Search {
            query, match_index, ..
        } = &mut self.mode
        {
            *query = new_query;
            *match_index = new_match;
        }
        EditorAction::Redraw
    }

    fn do_search_step_back(&mut self) -> EditorAction {
        // Cycle to the next-older match: start from (current match - 1)
        // or from (history.len() - 1) if no current match.
        let (query, current_match) = match &self.mode {
            LineEditorMode::Search {
                query, match_index, ..
            } => (query.clone(), *match_index),
            _ => return EditorAction::NoChange,
        };
        let start = match current_match {
            Some(0) => return EditorAction::NoChange, // already at oldest match
            Some(i) => Some(i - 1),
            None => self.history.len().checked_sub(1),
        };
        let new_match = start.and_then(|s| self.search_history_backward(&query, s));
        if let LineEditorMode::Search { match_index, .. } = &mut self.mode {
            *match_index = new_match;
        }
        EditorAction::Redraw
    }

    fn do_search_step_forward(&mut self) -> EditorAction {
        // Cycle to the next-newer match: start from (current match + 1).
        let (query, current_match) = match &self.mode {
            LineEditorMode::Search {
                query, match_index, ..
            } => (query.clone(), *match_index),
            _ => return EditorAction::NoChange,
        };
        let start = match current_match {
            None => return EditorAction::NoChange,
            Some(i) if i + 1 >= self.history.len() => return EditorAction::NoChange,
            Some(i) => i + 1,
        };
        let new_match = self.search_history_forward(&query, start);
        if let LineEditorMode::Search { match_index, .. } = &mut self.mode {
            *match_index = new_match;
        }
        EditorAction::Redraw
    }

    fn do_search_accept(&mut self) -> EditorAction {
        // Replace buffer with the matched history line (if any), then
        // exit search mode and trigger the normal Accept path.
        let match_text = match &self.mode {
            LineEditorMode::Search { match_index, .. } => match_index
                .and_then(|i| self.history.get(i))
                .cloned()
                .unwrap_or_default(),
            _ => return EditorAction::NoChange,
        };
        // Exit search.
        self.mode = LineEditorMode::Normal;
        self.buffer = match_text;
        self.cursor = self.buffer.len();
        // Submit via the standard accept path so balance + reset
        // semantics apply uniformly.
        self.do_accept()
    }

    fn do_search_cancel(&mut self) -> EditorAction {
        // Restore the saved buffer + cursor, exit search.
        if let LineEditorMode::Search {
            saved_buffer,
            saved_cursor,
            ..
        } = core::mem::replace(&mut self.mode, LineEditorMode::Normal)
        {
            self.buffer = saved_buffer;
            self.cursor = saved_cursor;
        }
        EditorAction::Redraw
    }

    fn do_accept(&mut self) -> EditorAction {
        // U-4b: bracket / quote / trailing-backslash check.
        //
        // If the buffer's balance state is "awaiting continuation"
        // (unclosed brackets / quotes / trailing-unescaped-backslash),
        // insert '\n' at cursor instead of submitting. The trailing
        // backslash stays in the buffer verbatim: the parser (U-5+)
        // sees `\\\n` as the POSIX/rc line-continuation marker and
        // elides both atoms. Doing the elision in the editor would
        // require the parser to maintain a separate "raw buffer" with
        // continuations re-inserted, which is the opposite of the
        // separation of concerns scripted in UTOPIA-SHELL-DESIGN.md.
        let st = balance(&self.buffer);
        if st.awaits_continuation() {
            return self.do_insert('\n');
        }
        // Balanced: submit.
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

    // =========================================================================
    // U-4b tests -- balance tracker + multi-line.
    // =========================================================================

    #[test]
    fn balance_empty_is_balanced() {
        let st = balance("");
        assert!(st.is_balanced());
        assert!(!st.awaits_continuation());
    }

    #[test]
    fn balance_open_brace_unbalanced() {
        let st = balance("{");
        assert_eq!(st.brace_depth, 1);
        assert!(!st.is_balanced());
        assert!(st.awaits_continuation());
    }

    #[test]
    fn balance_open_close_brace_balanced() {
        let st = balance("{ foo }");
        assert_eq!(st.brace_depth, 0);
        assert!(st.is_balanced());
    }

    #[test]
    fn balance_nested_brackets() {
        let st = balance("{ ( [ ");
        assert_eq!(st.brace_depth, 1);
        assert_eq!(st.paren_depth, 1);
        assert_eq!(st.bracket_depth, 1);
        assert!(!st.is_balanced());
    }

    #[test]
    fn balance_negative_depth_is_balanced() {
        // Stray closer -- malformed but the editor submits + lets the
        // parser report the error.
        let st = balance("} unexpected");
        assert_eq!(st.brace_depth, -1);
        assert!(st.is_balanced());
    }

    #[test]
    fn balance_single_quote_isolates_brackets() {
        let st = balance("'a { b'");
        assert!(st.is_balanced());
        assert_eq!(st.brace_depth, 0);
    }

    #[test]
    fn balance_single_quote_unclosed_unbalanced() {
        let st = balance("'unclosed");
        assert!(st.in_single_quote);
        assert!(!st.is_balanced());
    }

    #[test]
    fn balance_double_quote_isolates_brackets() {
        let st = balance("\"a { b\"");
        assert!(st.is_balanced());
        assert_eq!(st.brace_depth, 0);
    }

    #[test]
    fn balance_double_quote_with_escaped_quote() {
        // "\\\"" inside source is "\"" inside the buffer.
        let st = balance("\"a \\\" b\"");
        assert!(st.is_balanced());
        assert!(!st.in_double_quote);
    }

    #[test]
    fn balance_trailing_backslash_unbalanced() {
        let st = balance("foo\\");
        assert!(st.trailing_unescaped_backslash);
        assert!(st.awaits_continuation());
    }

    #[test]
    fn balance_paired_backslashes_balanced() {
        let st = balance("foo\\\\");
        assert!(!st.trailing_unescaped_backslash);
        assert!(st.is_balanced());
    }

    #[test]
    fn balance_backslash_inside_single_quote_literal() {
        // Inside '...', \ is literal -- not a trailing escape.
        let st = balance("'foo\\'");
        assert!(!st.trailing_unescaped_backslash);
        assert!(st.is_balanced());
    }

    #[test]
    fn balance_comment_skipped() {
        // # comments to end of line in unquoted contexts.
        let st = balance("foo # { not a real open brace");
        assert!(st.is_balanced());
        assert_eq!(st.brace_depth, 0);
    }

    #[test]
    fn balance_comment_in_quote_is_literal() {
        let st = balance("\"foo # not a comment\"");
        assert!(st.is_balanced());
    }

    #[test]
    fn enter_with_unbalanced_inserts_newline() {
        let mut le = LineEditor::new();
        feed(&mut le, b"{");
        let r = le.feed_byte(b'\r');
        // Should NOT be Accept; should be Redraw (the do_insert path).
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "{\n");
        assert_eq!(le.cursor(), 2);
    }

    #[test]
    fn enter_with_balanced_submits() {
        let mut le = LineEditor::new();
        feed(&mut le, b"{}");
        let r = le.feed_byte(b'\r');
        assert_eq!(r, EditorAction::Accept(String::from("{}")));
    }

    #[test]
    fn multi_line_enter_eventually_accepts() {
        let mut le = LineEditor::new();
        // Open brace.
        feed(&mut le, b"{");
        // Enter -- multi-line.
        le.feed_byte(b'\r');
        assert_eq!(le.buffer(), "{\n");
        // Type body and close.
        feed(&mut le, b"  foo");
        le.feed_byte(b'\r');
        assert_eq!(le.buffer(), "{\n  foo\n");
        feed(&mut le, b"}");
        let r = le.feed_byte(b'\r');
        match r {
            EditorAction::Accept(s) => assert_eq!(s, "{\n  foo\n}"),
            _ => panic!("expected Accept; got {:?}", r),
        }
    }

    #[test]
    fn quote_in_single_quote_no_continuation() {
        let mut le = LineEditor::new();
        feed(&mut le, b"'{'");
        // Buffer is `'{'`; balance: in_single_quote was true after `'`,
        // then `{` literal, then closing `'` -> closed. Balanced.
        let r = le.feed_byte(b'\r');
        match r {
            EditorAction::Accept(s) => assert_eq!(s, "'{'"),
            _ => panic!("expected Accept; got {:?}", r),
        }
    }

    #[test]
    fn trailing_backslash_continues() {
        let mut le = LineEditor::new();
        feed(&mut le, b"foo\\");
        let r = le.feed_byte(b'\r');
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "foo\\\n");
    }

    #[test]
    fn render_multi_line_emits_continuation_prefix() {
        let mut le = LineEditor::new();
        feed(&mut le, b"{");
        le.feed_byte(b'\r');
        feed(&mut le, b"x");
        let s = le.render("> ");
        // First line: "\r\x1b[K> {"
        assert!(s.starts_with("\r\x1b[K> {"));
        // Subsequent line: "\r\n\x1b[K<cont>x"
        // <cont> for prompt_width 2 = `⋮ ` (the glyph in PATH color
        // wrapped in ANSI, then a space). Just check the literal text.
        assert!(s.contains("\r\n\x1b[K"));
        assert!(s.contains("\u{22ee}"));
        // After all the lines, cursor positioning: cursor is on line 1
        // (the second line) at col 1 (visible char "x" at col cont_w + 0
        // -- actually cursor is AFTER the x, so col = cont_w + 1 = 2 + 1 = 3
        // when prompt_width is 2).
        assert!(s.ends_with("\x1b[3C"));
    }

    #[test]
    fn render_multi_line_cursor_on_first_line() {
        let mut le = LineEditor::new();
        feed(&mut le, b"{");
        le.feed_byte(b'\r');
        feed(&mut le, b"x");
        // Move cursor to start of buffer.
        le.feed_byte(0x01); // Ctrl-A
        // Cursor should now be on line 0 (first line), column 0 of buffer.
        // After this re-Ctrl-A in multi-line, cursor lands at byte 0
        // (start of whole buffer); render needs to move cursor UP one line.
        let s = le.render("> ");
        // The cursor-up escape \x1b[1F should appear.
        assert!(s.contains("\x1b[1F"));
        // Final position: prompt_width + 0 = col 2.
        assert!(s.ends_with("\x1b[2C"));
    }

    // =========================================================================
    // U-4c tests -- smart Up/Down + search mode + history cap.
    // =========================================================================

    #[test]
    fn smart_up_in_multi_line_does_cursor_up() {
        let mut le = LineEditor::new();
        // Construct a multi-line buffer by hand.
        let _ = le.feed_bytes(b"{");
        le.feed_byte(b'\r');
        let _ = le.feed_bytes(b"  foo");
        // Cursor is at end of line 1 (byte 7).
        assert_eq!(le.cursor(), 7);
        // Ctrl-P (HistoryPrev) -- multi-line + cursor not on first line
        // -> should cursor-up to line 0 (preserving column).
        let r = le.feed_byte(0x10);
        assert_eq!(r, EditorAction::Redraw);
        // Now cursor should be on line 0. Line 0 is "{" (1 char), but
        // desired_col was 5 (after "  foo"); since line 0 is shorter,
        // cursor lands at end of line 0 = byte 1.
        assert_eq!(le.cursor(), 1);
    }

    #[test]
    fn smart_up_at_first_line_uses_history() {
        let mut le = LineEditor::new();
        le.push_history(String::from("older"));
        // Single-line buffer "x" -- Up should do history-prev.
        let _ = le.feed_bytes(b"x");
        le.feed_byte(0x10);
        assert_eq!(le.buffer(), "older");
    }

    #[test]
    fn smart_down_in_multi_line_does_cursor_down() {
        let mut le = LineEditor::new();
        // Build "abc\\\nxyz" via backslash-continuation.
        let _ = le.feed_bytes(b"abc");
        le.feed_byte(b'\\');
        le.feed_byte(b'\r');
        let _ = le.feed_bytes(b"xyz");
        let buf_len = le.buffer().len();
        assert_eq!(le.cursor(), buf_len);
        // Ctrl-A -> start of BUFFER (current Ctrl-A semantics; not
        // start-of-line. Whole-buffer cursor=0 is on line 0).
        le.feed_byte(0x01);
        assert_eq!(le.cursor(), 0);
        // Ctrl-N (Down) -- multi-line, line_end_byte(0) = 4 < buf_len ->
        // cursor down to line 1 at col 0. Line 1 starts at byte 5.
        le.feed_byte(0x0e);
        assert_eq!(le.cursor(), 5);
        // Ctrl-N again -- now on last line, falls through to history.
        // History is empty, history_pos is None -> NoChange. Cursor unchanged.
        le.feed_byte(0x0e);
        assert_eq!(le.cursor(), 5);
    }

    #[test]
    fn desired_col_preserved_across_consecutive_up_down() {
        let mut le = LineEditor::new();
        // Two lines of different length.
        let _ = le.feed_bytes(b"abcdef");
        le.feed_byte(b'\\');
        le.feed_byte(b'\r');
        let _ = le.feed_bytes(b"xy");
        // Cursor at end = byte 10 (= "abcdef\\\nxy".len()).
        // Visible col on line 1 (just "xy") at cursor = 2.
        // Ctrl-P up to line 0 -- col 2 within "abcdef\\" (7 chars wide
        // including the backslash). Cursor lands at col 2 = byte 2.
        le.feed_byte(0x10);
        assert_eq!(le.cursor(), 2);
        // Ctrl-N back down to line 1 -- col 2 within "xy" (2 chars).
        // min(2, 2) = 2 = end of line 1.
        le.feed_byte(0x0e);
        // Cursor at end of "xy" -- byte 10.
        assert_eq!(le.cursor(), 10);
    }

    #[test]
    fn push_history_caps_at_history_cap() {
        let mut le = LineEditor::new();
        for i in 0..(HISTORY_CAP + 5) {
            le.push_history(alloc::format!("entry-{}", i));
        }
        assert_eq!(le.history().len(), HISTORY_CAP);
        // The oldest entries should have been evicted.
        assert!(!le.history()[0].contains("entry-0"));
        assert!(le.history().last().unwrap().contains(&alloc::format!("entry-{}", HISTORY_CAP + 4)));
    }

    #[test]
    fn ctrl_r_enters_search_mode() {
        let mut le = LineEditor::new();
        let r = le.feed_byte(0x12);
        assert_eq!(r, EditorAction::Redraw);
        assert!(le.is_searching());
        assert_eq!(le.search_query(), Some(""));
    }

    #[test]
    fn search_appends_query_and_finds_match() {
        let mut le = LineEditor::new();
        le.push_history(String::from("apple pie"));
        le.push_history(String::from("banana bread"));
        le.push_history(String::from("cherry pie"));
        le.feed_byte(0x12); // Ctrl-R
        let _ = le.feed_bytes(b"pie");
        // Newest match for "pie" is index 2 ("cherry pie").
        assert_eq!(le.search_match_index(), Some(2));
        assert_eq!(le.search_query(), Some("pie"));
    }

    #[test]
    fn search_ctrl_r_steps_to_older_match() {
        let mut le = LineEditor::new();
        le.push_history(String::from("apple pie"));
        le.push_history(String::from("banana bread"));
        le.push_history(String::from("cherry pie"));
        le.feed_byte(0x12);
        let _ = le.feed_bytes(b"pie");
        assert_eq!(le.search_match_index(), Some(2));
        // Ctrl-R again -> step back to older "apple pie".
        le.feed_byte(0x12);
        assert_eq!(le.search_match_index(), Some(0));
    }

    #[test]
    fn search_backspace_widens_match() {
        let mut le = LineEditor::new();
        le.push_history(String::from("apple"));
        le.push_history(String::from("apricot"));
        le.feed_byte(0x12);
        let _ = le.feed_bytes(b"app");
        // Newest match for "app" is index 1 ("apricot" doesn't start
        // with "app"; "apple" does -> match index 0).
        assert_eq!(le.search_match_index(), Some(0));
        // Backspace once -> query is "ap".
        le.feed_byte(0x7f);
        assert_eq!(le.search_query(), Some("ap"));
        // Newest match for "ap" is index 1 ("apricot").
        assert_eq!(le.search_match_index(), Some(1));
    }

    #[test]
    fn search_enter_accepts_match() {
        let mut le = LineEditor::new();
        le.push_history(String::from("hello world"));
        le.feed_byte(0x12);
        let _ = le.feed_bytes(b"hello");
        let r = le.feed_byte(b'\r');
        match r {
            EditorAction::Accept(s) => assert_eq!(s, "hello world"),
            _ => panic!("expected Accept; got {:?}", r),
        }
        assert!(!le.is_searching());
    }

    #[test]
    fn search_cancel_restores_saved_buffer() {
        let mut le = LineEditor::new();
        le.push_history(String::from("foo"));
        let _ = le.feed_bytes(b"draft");
        assert_eq!(le.buffer(), "draft");
        le.feed_byte(0x12); // Ctrl-R; saves "draft"
        let _ = le.feed_bytes(b"foo");
        // Ctrl-C (cancel)
        let r = le.feed_byte(0x03);
        assert_eq!(r, EditorAction::Redraw);
        assert!(!le.is_searching());
        // Buffer restored to "draft"; cursor restored to end.
        assert_eq!(le.buffer(), "draft");
        assert_eq!(le.cursor(), 5);
    }

    #[test]
    fn search_no_match_fails_silently() {
        let mut le = LineEditor::new();
        le.push_history(String::from("apple"));
        le.feed_byte(0x12);
        let _ = le.feed_bytes(b"xyz");
        assert_eq!(le.search_match_index(), None);
        // Render should produce the failed-search prefix.
        let s = le.render("> ");
        assert!(s.contains("failed reverse-i-search"));
    }

    #[test]
    fn search_render_shows_query_and_match() {
        let mut le = LineEditor::new();
        le.push_history(String::from("hello world"));
        le.feed_byte(0x12);
        let _ = le.feed_bytes(b"hello");
        let s = le.render("> ");
        // Prefix should be (reverse-i-search)`hello': hello world
        assert!(s.contains("(reverse-i-search)`hello':"));
        assert!(s.contains("hello world"));
    }

    #[test]
    fn backspace_joins_continuation_line() {
        let mut le = LineEditor::new();
        feed(&mut le, b"foo");
        le.feed_byte(b'\r'); // unbalanced? No -- "foo" is balanced -> Accept
        // Reset since the above accepted. Restart with an actually-unbalanced
        // case.
        let mut le = LineEditor::new();
        feed(&mut le, b"{");
        le.feed_byte(b'\r');
        feed(&mut le, b"x");
        // Move cursor to between \n and x (i.e. byte 2).
        le.feed_byte(0x01); // Ctrl-A -> cursor=0
        // Cursor right twice -> over `{` then `\n` to position 2 = start of "x".
        le.feed_byte(0x06); // Ctrl-F
        le.feed_byte(0x06); // Ctrl-F
        assert_eq!(le.cursor(), 2);
        // Backspace: deletes the '\n' before cursor -- lines join.
        le.feed_byte(0x7f);
        assert_eq!(le.buffer(), "{x");
        assert_eq!(le.cursor(), 1);
    }
}
