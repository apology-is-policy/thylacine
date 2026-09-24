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
    /// D4: zsh-style menu completion. Tab with multiple candidates (after the
    /// shared prefix is exhausted) enters a cycling menu: the editor has
    /// applied `candidates[selected]` to the buffer, and the main loop should
    /// render a one-line candidate strip below the prompt with `selected`
    /// highlighted, then restore the cursor to the prompt. Each subsequent Tab
    /// re-emits this with the next `selected` (the buffer is re-applied); Enter
    /// finalizes (dismiss the strip, keep the selection, no submit); any other
    /// key dismisses the menu and is processed normally. The Vec is in source
    /// order (NOT sorted) so the source's natural ordering propagates.
    /// `unlisted` counts matches the source found but did not hand over; no Tab
    /// can cycle to them, so the strip says they exist (`menu_strip`).
    MenuShow {
        candidates: Vec<String>,
        selected: usize,
        unlisted: usize,
    },
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
// CompletionSource (U-4d) -- pluggable Tab completion.
// =============================================================================
//
// The line editor doesn't know about file paths, command names, env
// vars, or any other completion source. Those live above the engine
// (the shell's main loop, which has access to $path, the alias
// table, the function table, the cap registry, etc.). U-4d defines
// the trait the shell implements + the editor-side machinery (Tab
// key dispatches to the source; the source returns a structured
// Completions; the editor inserts the shared extension, then opens
// the D4 cycling menu (MenuShow) when there is none).
//
// The text that goes into the line is the SOURCE's to write, never the
// engine's. A source completes some grammar -- the shell's words, where a
// name holding a space or a quote must be quoted -- and only it knows how a
// value is spelled there. So each string it hands over is already the text
// to insert, and so is the prefix its matches share: the engine cannot take
// that prefix from the inserted forms, because quoting changes it. Quoted,
// two names that part after a space can share a prefix ending inside the
// quoting, or share nothing at all when only one of them needs quotes.
//
// The engine also ships a `StaticCompletionSource` over a fixed list, for
// tests and the boot probes; the shell's is `completion::ShellCompletionSource`.

/// A pluggable source for Tab completion candidates. Implementors
/// receive the current buffer + cursor position and return the byte
/// range to replace + the texts that may replace it.
pub trait CompletionSource {
    fn complete(&self, buffer: &str, cursor: usize) -> Completions;
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Completions {
    /// Byte range in the buffer that a completion replaces.
    pub replace_range: core::ops::Range<usize>,
    /// The listed matches, each as the text that replaces `replace_range`.
    pub candidates: Vec<String>,
    /// What every match shares, listed or not, as the text that replaces
    /// `replace_range` -- present only when it says more than the buffer does.
    pub extension: Option<String>,
    /// Matches missing from `candidates`, which no Tab reaches: a source that
    /// bounds what it holds lists only some, and one completing a grammar can
    /// meet a match it has no way to write.
    pub unlisted: usize,
}

/// A simple completion source backed by a fixed candidate list. Used
/// by the U-4d tests + boot probes; the shell installs
/// `completion::ShellCompletionSource`.
///
/// `complete(buffer, cursor)`: finds the start of the current word
/// (cursor backward to whitespace or buffer start) and returns the
/// subset of `candidates` whose first chars match the word prefix.
/// Each is inserted exactly as given: this source quotes nothing.
pub struct StaticCompletionSource {
    pub candidates: Vec<String>,
}

impl StaticCompletionSource {
    pub fn new(candidates: Vec<String>) -> Self {
        Self { candidates }
    }
}

impl CompletionSource for StaticCompletionSource {
    fn complete(&self, buffer: &str, cursor: usize) -> Completions {
        let word_start = buffer[..cursor]
            .rfind(|c: char| c.is_whitespace())
            .map(|i| i + 1)
            .unwrap_or(0);
        let prefix = &buffer[word_start..cursor];
        let mut matches: Vec<String> = self
            .candidates
            .iter()
            .filter(|c| c.starts_with(prefix))
            .cloned()
            .collect();
        // Preserve source-order; do NOT sort.
        matches.shrink_to_fit();
        // Inserted as given, so what they share is their own common prefix.
        let shared = longest_common_prefix(&matches);
        Completions {
            replace_range: word_start..cursor,
            extension: (shared.len() > prefix.len()).then_some(shared),
            candidates: matches,
            unlisted: 0,
        }
    }
}

/// Longest common byte prefix of all `strs`. UTF-8 safe: the returned
/// length is rounded DOWN to the nearest char boundary in the first
/// string. Returns "" for empty input.
pub(crate) fn longest_common_prefix<S: AsRef<str>>(strs: &[S]) -> String {
    if strs.is_empty() {
        return String::new();
    }
    if strs.len() == 1 {
        return String::from(strs[0].as_ref());
    }
    let first = strs[0].as_ref().as_bytes();
    let mut common_len = first.len();
    for s in &strs[1..] {
        let other = s.as_ref().as_bytes();
        let mut i = 0;
        while i < common_len && i < other.len() && first[i] == other[i] {
            i += 1;
        }
        common_len = i;
    }
    // Round to char boundary.
    let first = strs[0].as_ref();
    while common_len > 0 && !first.is_char_boundary(common_len) {
        common_len -= 1;
    }
    String::from(&first[..common_len])
}

/// D4: the one-line candidate strip `render` draws below the block in `Menu`
/// mode, for a terminal `width` columns wide. The `selected` candidate is
/// reverse-video highlighted; candidates join with two spaces, and when they
/// would not fit, a contiguous window AROUND `selected` is shown, with `<` /
/// `>` where it leaves candidates out, so the current pick is always visible.
/// `unlisted` matches -- ones the source never handed over -- are counted at
/// the end: the window markers mean "cycle to see more", but no Tab reaches
/// these, and without the count a partial menu reads as the whole set. The
/// window gives way to the count, never the reverse.
///
/// The strip is ONE row, whatever it holds: it is erased as one row, so a tail
/// that wrapped would stay on screen. It stops short of the last column as
/// well, which leaves no terminal in its pending-wrap state.
fn menu_strip(cands: &[String], selected: usize, unlisted: usize, width: usize) -> String {
    if cands.is_empty() {
        return String::new();
    }
    let room = width.saturating_sub(1);
    let more = if unlisted > 0 {
        format!("+{} more", unlisted)
    } else {
        String::new()
    };
    // Four columns for the `< ` / ` >` markers, and the count's own.
    let count_w = if more.is_empty() { 0 } else { 2 + more.len() };
    let budget = room.saturating_sub(4 + count_w);
    let sel = selected.min(cands.len() - 1);
    let widths: Vec<usize> = cands
        .iter()
        .map(|c| crate::ansi::visible_width(c))
        .collect();
    // Grow a window [lo, hi) outward from `sel` while it fits the budget.
    let mut lo = sel;
    let mut hi = sel + 1;
    let mut used = widths[sel];
    loop {
        let mut grew = false;
        if hi < cands.len() && used + 2 + widths[hi] <= budget {
            used += 2 + widths[hi];
            hi += 1;
            grew = true;
        }
        if lo > 0 && used + 2 + widths[lo - 1] <= budget {
            lo -= 1;
            used += 2 + widths[lo];
            grew = true;
        }
        if !grew {
            break;
        }
    }
    let mut out = String::new();
    if lo > 0 {
        out.push_str("< ");
    }
    for (n, i) in (lo..hi).enumerate() {
        if n > 0 {
            out.push_str("  ");
        }
        if i == sel {
            out.push_str("\x1b[7m"); // reverse video
            out.push_str(&cands[i]);
            out.push_str("\x1b[0m"); // reset (self-contained so DECRC is clean)
        } else {
            out.push_str(&cands[i]);
        }
    }
    if hi < cands.len() {
        out.push_str(" >");
    }
    if !more.is_empty() {
        out.push_str("  ");
        out.push_str(&crate::ansi::fg(crate::palette::Role::Path, &more));
    }
    // The window always holds `selected`, which alone can be wider than a
    // narrow terminal.
    clip_visible(&out, room)
}

/// `s` cut to at most `max` visible columns, measured as `ansi::visible_width`
/// measures: a CSI escape costs nothing and is kept, and a cut string ends with
/// a reset so no colour opened before the cut stays open.
fn clip_visible(s: &str, max: usize) -> String {
    if crate::ansi::visible_width(s) <= max {
        return String::from(s);
    }
    let mut out = String::new();
    let mut cols = 0usize;
    let mut chars = s.chars().peekable();
    while let Some(c) = chars.next() {
        if c == '\x1b' && chars.peek() == Some(&'[') {
            out.push(c);
            out.push('[');
            chars.next();
            // Parameter and intermediate bytes, then one final byte --
            // whatever it is, as `visible_width` takes it.
            while let Some(&n) = chars.peek() {
                if !('\x20'..='\x3f').contains(&n) {
                    break;
                }
                out.push(n);
                chars.next();
            }
            if let Some(n) = chars.next() {
                out.push(n);
            }
            continue;
        }
        if cols == max {
            break;
        }
        out.push(c);
        cols += 1;
    }
    out.push_str(crate::ansi::RESET);
    out
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
    /// Tab -- dispatch to the registered CompletionSource (U-4d).
    Complete,
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
//   Escape -> ESC -> Escape (restart: the first ESC is abandoned,
//                             the second begins a fresh sequence -- the
//                             VT rule, and why ESC ESC does not return
//                             to Ground)
//   Escape -> anything else -> Ground (sequence aborted; the byte is
//                             CONSUMED, which is the slot reserved for
//                             future Alt-<letter> bindings)
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
    /// D4: zsh-style cycling completion menu. Entered from a Tab with multiple
    /// candidates whose shared prefix is exhausted. `candidates[selected]` is
    /// currently APPLIED to the buffer, occupying `[anchor, anchor +
    /// candidates[selected].len())` with the cursor just after it. Tab cycles
    /// `selected`; Enter finalizes; any other key dismisses + redispatches.
    Menu {
        candidates: Vec<String>,
        selected: usize,
        /// Buffer byte offset where the completed word begins.
        anchor: usize,
        /// Matches the source left out of `candidates` (re-emitted per cycle).
        unlisted: usize,
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
    /// U-4d: pluggable completion source. None == Tab is a no-op.
    /// Plugged via `set_completion_source`; the shell main loop
    /// installs a real source at U-6.
    completion_source: Option<alloc::boxed::Box<dyn CompletionSource>>,
    /// #115c: the command index for command-line validity coloring (the
    /// SAME sorted set the shell gives the completion source). Empty (the
    /// default) disables coloring -- render emits the buffer verbatim, so
    /// host tests + the bare-spawn boot check stay byte-identical.
    known_commands: Vec<String>,
    /// Terminal width in columns, once known -- from `/dev/winsize` (the
    /// shell's fast path) or a Cursor-Position-Report the CSI parser
    /// recognizes (`set_cols`). `None` == "width unknown": render falls back
    /// to the newline-only geometry (today's behaviour, correct for a dumb
    /// pipe or an unanswered probe) rather than GUESS a width -- a wrong width
    /// emits wrong cursor-up counts and corrupts the display, strictly worse
    /// than not wrapping. `Some(c)` enables visual-wrapped-row rendering.
    cols: Option<usize>,
    /// The physical row offset (from the rendered block's top) where the
    /// previous wrapped render left the terminal cursor. The next wrapped
    /// render moves UP this many rows to reach the block top before clearing
    /// and re-emitting -- without it a redraw of a line that wrapped past the
    /// terminal edge clears only the cursor's current physical row and
    /// re-emits from there, duplicating the line on every keystroke (the
    /// reported bug). Zeroed whenever the caller moves the cursor to a fresh
    /// line (submit / cancel / clear-screen / `reset_render_position`).
    /// Untouched while `cols` is `None`.
    prev_cursor_row: usize,
    /// The completion-menu strip on screen, if one is: `render` draws it below
    /// the block while the editor is in `Menu` mode, and every later render or
    /// `clear_menu` erases it. Recorded RELATIVE to where the render left the
    /// cursor, because a strip drawn below the bottom row scrolls the screen
    /// and an absolute position (DECSC) would no longer name the prompt.
    strip: Option<StripAt>,
}

/// Where a drawn menu strip sits: `down` rows below the cursor's row, which
/// the cursor left at column `col`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct StripAt {
    down: usize,
    col: usize,
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
            completion_source: None,
            known_commands: Vec::new(),
            cols: None,
            prev_cursor_row: 0,
            strip: None,
        }
    }

    /// Record the terminal width (columns). The shell calls this from the
    /// `/dev/winsize` fast path; the CSI parser calls it when a CPR reply
    /// arrives. Floored at 1 so the wrapped-row division is always safe.
    /// Setting a width enables visual-wrapped-row rendering; a resize just
    /// re-calls this with the new width (the next redraw reflows).
    pub fn set_cols(&mut self, cols: usize) {
        self.cols = Some(cols.max(1));
    }

    /// The known terminal width, or `None` when unknown. Test/introspection
    /// hook; the shell never needs to read it back.
    pub fn cols(&self) -> Option<usize> {
        self.cols
    }

    /// Forget where the previous render left the terminal cursor: the caller
    /// has just moved it to a fresh line by an emission the editor did not
    /// see (`\x1b[2J\x1b[H` on clear-screen, the `\r\n` before an async note
    /// notification). The next render then draws from the current line as a
    /// fresh block instead of trying to move up to a stale block top.
    pub fn reset_render_position(&mut self) {
        self.prev_cursor_row = 0;
        self.strip = None;
    }

    /// The bytes that erase the completion-menu strip, if one is drawn, and
    /// put the cursor back where the render left it; "" when none is. The
    /// caller emits them before moving the cursor itself (an accepted line's
    /// `\r\n`, a notification): after that the strip's position is no longer
    /// known, and it would stay on screen.
    pub fn clear_menu(&mut self) -> String {
        let mut out = String::new();
        if let Some(at) = self.strip.take() {
            out.push_str(&format!("\x1b[{}B\r\x1b[K\x1b[{}A\r", at.down, at.down));
            if at.col > 0 {
                out.push_str(&format!("\x1b[{}C", at.col));
            }
        }
        out
    }

    /// Install a Tab completion source (U-4d). The shell main loop
    /// plugs a real source here; tests + early-bringup use the
    /// shipped StaticCompletionSource.
    pub fn set_completion_source(
        &mut self,
        source: alloc::boxed::Box<dyn CompletionSource>,
    ) {
        self.completion_source = Some(source);
    }

    /// Drop the current Tab completion source. After this, Tab is a
    /// no-op.
    pub fn clear_completion_source(&mut self) {
        self.completion_source = None;
    }

    /// #115c: install the command index used for command-line validity
    /// coloring. `cmds` must be sorted (the shell's `refresh_command_index`
    /// guarantees it -- it is the SAME index handed to the completion
    /// source). An empty set (the default) disables coloring: `render` emits
    /// the buffer verbatim, so callers that never install an index (host
    /// tests, the bare-spawn boot check) get byte-identical output.
    pub fn set_known_commands(&mut self, cmds: Vec<String>) {
        self.known_commands = cmds;
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
        self.prev_cursor_row = 0;
        self.strip = None;
        // kill_buffer survives Cancel/Accept -- yank can paste across
        // a Cancel boundary (standard emacs behaviour). cols persists --
        // the terminal did not change size.
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
    /// any Redraw EditorAction. In `Menu` mode it also draws the completion
    /// strip below the block, and it erases a strip it no longer wants.
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
    pub fn render(&mut self, prompt: &str) -> String {
        // U-4c: in Search mode, render shows the readline-style
        // search prompt + the matched line (or empty if no match).
        // Cursor positions at the end of the query inside the
        // (reverse-i-search)`...': prefix.
        if matches!(self.mode, LineEditorMode::Search { .. }) {
            let mut out = self.clear_menu();
            if let LineEditorMode::Search {
                query, match_index, ..
            } = &self.mode
            {
                out.push_str(&self.render_search(query, *match_index));
            }
            return out;
        }
        // Width unknown -> the newline-only geometry (today's behaviour): a
        // dumb pipe or an unanswered probe has no width to wrap against, and a
        // GUESS would emit wrong cursor-up counts. Width known -> the
        // visual-wrapped-row path that fixes the wrap-and-move duplication.
        let (mut out, below, col) = match self.cols {
            None => {
                // This path rewrites only its own rows, so a strip drawn below
                // them is erased first.
                let mut out = self.clear_menu();
                let (body, below, col) = self.render_unwrapped(prompt);
                out.push_str(&body);
                (out, below, col)
            }
            Some(cols) => {
                // Its erase-below from the block's top takes the strip with it.
                self.strip = None;
                self.render_wrapped(prompt, cols)
            }
        };
        if let LineEditorMode::Menu {
            candidates,
            selected,
            unlisted,
            ..
        } = &self.mode
        {
            // D4: the strip goes on the row below the whole block -- not below
            // the cursor, which a completion mid-buffer leaves above other
            // rows -- and the way back is relative. At the bottom row the
            // `\r\n` scrolls the screen; a save/restore of the absolute
            // position would then return to the strip's row, not the prompt's.
            let strip = menu_strip(candidates, *selected, *unlisted, self.cols.unwrap_or(80));
            if below > 0 {
                out.push_str(&format!("\x1b[{}B", below));
            }
            out.push_str("\r\n\x1b[K");
            out.push_str(&strip);
            out.push_str(&format!("\x1b[{}A\r", below + 1));
            if col > 0 {
                out.push_str(&format!("\x1b[{}C", col));
            }
            self.strip = Some(StripAt {
                down: below + 1,
                col,
            });
        }
        out
    }

    /// The newline-only render (the `cols == None` fallback). Positions the
    /// cursor by counting LOGICAL '\n' lines, not visual wrapped rows -- so a
    /// single logical line that overflows the terminal width is mis-positioned
    /// (the pre-fix behaviour, kept verbatim for the width-unknown case where
    /// nothing better is possible). Takes `&self`: it tracks no cross-render
    /// state. Also returns where it left the cursor: the rows from its row down
    /// to the block's last, and its column.
    fn render_unwrapped(&self, prompt: &str) -> (String, usize, usize) {
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
            // #115c: colour line 0's command token (fen if it resolves,
            // cinnabar if not). The inserted SGR escapes are zero-width
            // (ansi::visible_width strips them), so the cursor math below --
            // computed from the PLAIN buffer -- is unaffected.
            out.push_str(&self.colorize_line0(first));
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

        (out, lines_to_up, target_col)
    }

    /// The visual-wrapped-row render (`cols == Some`). Counts the PHYSICAL
    /// rows each logical line occupies once the terminal wraps it at `cols`,
    /// so cursor motion on a line that overflowed the width no longer clears
    /// only one physical row and re-emits from the wrong place (the reported
    /// duplication bug).
    ///
    /// Sequence, all cursor moves RELATIVE (scroll-safe, linenoise-style):
    ///   1. Up `prev_cursor_row` rows + CR -> the block's top-left. (The
    ///      previous render left the cursor at its logical row within the
    ///      block; `prev_cursor_row` records that row.)
    ///   2. `\x1b[J` -- erase from here to end of screen (clears the whole old
    ///      block, including rows a shrink left stale -- the U-6 caveat, now
    ///      closed for the width-known path).
    ///   3. Emit prompt + buffer; a `\r\n` + continuation prefix joins logical
    ///      lines. Autowrap lays each logical line across its physical rows.
    ///   4. If the last logical line exactly fills its final row, emit one
    ///      `\r\n` so the terminal leaves "pending wrap" for a real row -- the
    ///      cursor's end position is then unambiguous.
    ///   5. Move from that end position UP to the cursor's physical row, CR,
    ///      then right to its column.
    ///   6. Record the cursor's physical row for the next render.
    ///
    /// Also returns where it left the cursor: the rows from its row down to
    /// the block's last (the forced row included), and its column.
    fn render_wrapped(&mut self, prompt: &str, cols: usize) -> (String, usize, usize) {
        let cols = cols.max(1); // defensive: never divide by zero
        let prompt_w = crate::ansi::visible_width(prompt);
        let cont = continuation_prefix(prompt_w);
        let cont_w = crate::ansi::visible_width(&cont);

        // Physical rows a logical line of visible width `w` occupies when its
        // on-screen prefix (prompt / continuation) is `p` wide: ceil over the
        // width, but at least one row (an empty line still shows a row).
        let line_rows = |p: usize, w: usize| -> usize {
            let cells = p + w;
            if cells == 0 {
                1
            } else {
                (cells + cols - 1) / cols // ceil(cells / cols)
            }
        };

        let lines: Vec<&str> = self.buffer.split('\n').collect();
        let prefix_w = |i: usize| if i == 0 { prompt_w } else { cont_w };

        // Cursor's logical line + its visible column within that line.
        let bytes_up_to_cursor = &self.buffer[..self.cursor];
        let cursor_line = bytes_up_to_cursor.matches('\n').count();
        let cursor_line_start = bytes_up_to_cursor.rfind('\n').map(|i| i + 1).unwrap_or(0);
        let col_in_line = crate::ansi::visible_width(&self.buffer[cursor_line_start..self.cursor]);

        // Physical row where each logical line begins (running sum of rows).
        // `content_rows` = rows the emitted block occupies before any forced
        // trailing newline; `cursor_row`/`cursor_col` = the cursor's physical
        // cell within it.
        let mut start = 0usize;
        let mut cursor_row = 0usize;
        let mut cursor_col = 0usize;
        for (i, line) in lines.iter().enumerate() {
            let w = crate::ansi::visible_width(line);
            if i == cursor_line {
                let cells = prefix_w(i) + col_in_line;
                cursor_row = start + cells / cols;
                cursor_col = cells % cols;
            }
            start += line_rows(prefix_w(i), w);
        }
        let content_rows = start; // sum over all lines

        // The last logical line exactly fills its final row -> the terminal
        // parks in pending-wrap; a forced `\r\n` gives the cursor a real row.
        let last = lines.len() - 1;
        let last_cells = prefix_w(last) + crate::ansi::visible_width(lines[last]);
        let forced = last_cells > 0 && last_cells % cols == 0;
        // Row the terminal cursor sits on right after emission (+ forced NL).
        let end_row = if forced { content_rows } else { content_rows - 1 };

        let mut out = String::new();
        // 1. Up to the block top, column 0.
        if self.prev_cursor_row > 0 {
            out.push_str(&format!("\x1b[{}A", self.prev_cursor_row));
        }
        out.push('\r');
        // 2. Clear the whole old block (and anything a shrink left below).
        out.push_str("\x1b[J");
        // 3. Emit prompt + buffer. No per-line \x1b[K -- (2) cleared already.
        out.push_str(prompt);
        out.push_str(&self.colorize_line0(lines[0]));
        for line in &lines[1..] {
            out.push_str("\r\n");
            out.push_str(&cont);
            out.push_str(line);
        }
        // 4. Force the pending-wrap into a real row when the tail fills it.
        if forced {
            out.push_str("\r\n");
        }
        // 5. Move from the emission end up to the cursor's row, then across.
        if end_row > cursor_row {
            out.push_str(&format!("\x1b[{}A", end_row - cursor_row));
        }
        out.push('\r');
        if cursor_col > 0 {
            out.push_str(&format!("\x1b[{}C", cursor_col));
        }
        // 6. Remember where this render left the cursor.
        self.prev_cursor_row = cursor_row;

        (out, end_row.saturating_sub(cursor_row), cursor_col)
    }

    /// #115c: colour line 0's first token (the command) -- `fen` if it
    /// resolves in the installed command index, `cinnabar` if not. Returns the
    /// line VERBATIM when coloring is disabled (empty index -> host tests + the
    /// bare-spawn boot check stay byte-identical), when the line has no command
    /// token (blank / whitespace-only), or when the token contains `/` (a
    /// command-by-path the name index cannot speak to -- left default rather
    /// than mis-flagged red). The rest of the line is emitted unchanged.
    fn colorize_line0(&self, line: &str) -> String {
        if self.known_commands.is_empty() {
            return String::from(line);
        }
        let lead = line.len() - line.trim_start().len(); // leading-whitespace bytes
        let rest = &line[lead..];
        let tok_end = rest.find(char::is_whitespace).unwrap_or(rest.len());
        let token = &rest[..tok_end];
        if token.is_empty() || token.contains('/') {
            return String::from(line);
        }
        let known = self
            .known_commands
            .binary_search_by(|c| c.as_str().cmp(token))
            .is_ok();
        let role = if known {
            crate::palette::Role::Fen
        } else {
            crate::palette::Role::Cinnabar
        };
        let mut out = String::with_capacity(line.len() + 16);
        out.push_str(&line[..lead]); // leading whitespace, uncoloured
        out.push_str(&crate::ansi::fg(role, token)); // the coloured command + reset
        out.push_str(&rest[tok_end..]); // the remainder verbatim
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
            0x09 => Action::Complete,      // Tab -- U-4d completion hook
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
            b'R' => {
                // Cursor-Position Report: ESC[<rows>;<cols>R. The terminal's
                // reply to the width probe the shell emits (ESC[9999;9999H
                // parks the cursor -> the terminal clamps to the bottom-right,
                // so the reported position IS the screen size). Recognized
                // here byte-at-a-time, so a reply dribbled across reads (the
                // HVF serial round-trip) reassembles for free; absorbed as
                // NoChange, never surfaced as a phantom key. Two non-zero
                // params required (rows;cols); the width is params[1].
                self.parser = ParserState::Ground;
                if param_count >= 1 && current_has_digits && params[0] != 0 && params[1] != 0 {
                    self.set_cols(params[1] as usize);
                }
                Action::Ignore
            }
            _ => {
                // Unknown CSI final -- reset; v1.x can add SGR (m),
                // device-status (n), etc.
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
        // D4: in the cycling completion menu, Tab advances + a few keys are
        // special; everything else dismisses the menu and is re-dispatched in
        // Normal mode (so typing after a completion appends to it).
        if matches!(self.mode, LineEditorMode::Menu { .. }) {
            return self.apply_in_menu(action);
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
            Action::Complete => self.do_complete(),
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

    // -------------------------------------------------------------------------
    // U-4d: Tab completion dispatch.
    // -------------------------------------------------------------------------

    fn do_complete(&mut self) -> EditorAction {
        let comp = match &self.completion_source {
            Some(src) => src.complete(&self.buffer, self.cursor),
            None => return EditorAction::NoChange,
        };
        // A lone candidate is the completion only when nothing else matched.
        if comp.candidates.len() == 1 && comp.unlisted == 0 {
            return self.apply_completion(comp.replace_range, &comp.candidates[0]);
        }
        // Several matches: first extend to what they all share (zsh: complete
        // the common part). When that is exhausted, enter the D4 cycling menu
        // -- apply candidate[0] and let `render` draw the highlighted strip.
        if let Some(ext) = &comp.extension {
            return self.apply_completion(comp.replace_range, ext);
        }
        if comp.candidates.is_empty() {
            return EditorAction::NoChange;
        }
        let unlisted = comp.unlisted;
        let anchor = comp.replace_range.start;
        let cand0 = comp.candidates[0].clone();
        // Apply candidate[0]; if it cannot be applied (would exceed the buffer
        // cap), do not enter menu mode.
        if matches!(
            self.apply_completion(comp.replace_range, &cand0),
            EditorAction::NoChange
        ) {
            return EditorAction::NoChange;
        }
        let candidates = comp.candidates;
        self.mode = LineEditorMode::Menu {
            candidates: candidates.clone(),
            selected: 0,
            anchor,
            unlisted,
        };
        EditorAction::MenuShow {
            candidates,
            selected: 0,
            unlisted,
        }
    }

    /// D4: action dispatch while the completion menu is open. Tab cycles to the
    /// next candidate; Enter finalizes (dismiss + keep the selection, NO
    /// submit -- "ready for more typing"); Cancel clears the line; an unknown
    /// byte is ignored (stays in the menu); any other action dismisses the menu
    /// (keeping the applied selection) and is re-dispatched in Normal mode.
    fn apply_in_menu(&mut self, action: Action) -> EditorAction {
        match action {
            Action::Complete => self.menu_cycle(),
            Action::Accept => {
                self.mode = LineEditorMode::Normal;
                EditorAction::Redraw
            }
            Action::Cancel => {
                self.mode = LineEditorMode::Normal;
                self.do_cancel()
            }
            Action::Ignore => EditorAction::NoChange,
            other => {
                self.mode = LineEditorMode::Normal;
                self.apply(other)
            }
        }
    }

    /// D4: replace the applied candidate with the next one in the cycle, wrap at
    /// the end, and re-emit MenuShow. Bounds-defensive: if the applied span no
    /// longer fits the buffer (it always should -- only menu actions mutate it
    /// while open), or the next candidate would exceed the buffer cap, bail out
    /// of menu mode cleanly.
    fn menu_cycle(&mut self) -> EditorAction {
        let (anchor, old_len, next, candidates, unlisted) = match &self.mode {
            LineEditorMode::Menu {
                candidates,
                selected,
                anchor,
                unlisted,
            } => {
                let next = (*selected + 1) % candidates.len();
                (
                    *anchor,
                    candidates[*selected].len(),
                    next,
                    candidates.clone(),
                    *unlisted,
                )
            }
            _ => return EditorAction::NoChange,
        };
        let end = anchor + old_len;
        if end > self.buffer.len() || !self.buffer.is_char_boundary(anchor) || !self.buffer.is_char_boundary(end) {
            self.mode = LineEditorMode::Normal;
            return EditorAction::Redraw;
        }
        let new_cand = candidates[next].clone();
        if self.buffer.len() - old_len + new_cand.len() > MAX_BUFFER_LEN {
            return EditorAction::NoChange; // cannot grow; stay on the current pick
        }
        self.buffer.replace_range(anchor..end, &new_cand);
        self.cursor = anchor + new_cand.len();
        self.mode = LineEditorMode::Menu {
            candidates: candidates.clone(),
            selected: next,
            anchor,
            unlisted,
        };
        EditorAction::MenuShow {
            candidates,
            selected: next,
            unlisted,
        }
    }

    fn apply_completion(
        &mut self,
        range: core::ops::Range<usize>,
        replacement: &str,
    ) -> EditorAction {
        let removed_len = range.end - range.start;
        let new_total = self.buffer.len() - removed_len + replacement.len();
        if new_total > MAX_BUFFER_LEN {
            return EditorAction::NoChange;
        }
        let start = range.start;
        self.buffer.replace_range(range, replacement);
        self.cursor = start + replacement.len();
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
        // Balanced: submit. The caller moves the terminal past the edited
        // line (a `\r\n`) and draws a fresh prompt, so the next render starts
        // a fresh block -- forget the accepted block's cursor row.
        let line = core::mem::take(&mut self.buffer);
        self.cursor = 0;
        self.history_pos = None;
        self.saved_current.clear();
        self.prev_cursor_row = 0;
        EditorAction::Accept(line)
    }

    fn do_cancel(&mut self) -> EditorAction {
        self.buffer.clear();
        self.cursor = 0;
        self.history_pos = None;
        self.saved_current.clear();
        self.prev_cursor_row = 0;
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
// Tests. `cargo test -p libutopia --lib --no-default-features --target <host>`
// runs these; `tools/test-rust.sh` does it for the whole tree.
//
// Until 2026-09-22 this block said a host run "would exercise these", which was
// true of the command and false of the world: libthyla-rs was an unconditional
// dependency, so the crate could not be built for a host target at all and
// these tests had never compiled, let alone run. The twenty errors that turned
// up the first time they were asked to compile were all this one missing
// import -- benign, but only findable by actually running them.
// =============================================================================

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

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

    /// UT-EDIT-1, investigated and WITHDRAWN: the editor is right.
    ///
    /// This asserted that `ESC ESC` returns to Ground, so a following `a` is
    /// inserted. It does not, deliberately: `parse_escape` treats a second ESC
    /// as RESTARTING the sequence, which is what the VT state machine does
    /// (an ESC in escape state clears and re-enters escape), and the `a` is
    /// then consumed as the final byte of `ESC a` -- the slot reserved for
    /// Alt-key bindings.
    ///
    /// It is also internally consistent, which is the argument that settles
    /// it: a SINGLE ESC already swallows the next printable for the same
    /// reason, so `ESC ESC a` losing one character is the same rule applied
    /// twice, not a second surprise. Having never run, the test encoded an
    /// expectation the editor had not adopted.
    #[test]
    fn esc_esc_restarts_the_sequence_and_esc_letter_is_reserved() {
        let mut le = LineEditor::new();
        feed(&mut le, b"\x1b\x1b");
        le.feed_byte(b'a');
        assert_eq!(le.buffer(), "", "ESC a is a reserved sequence, not text");
        // Still live afterwards: the NEXT byte types normally, so a stray ESC
        // costs one character and never wedges the line.
        le.feed_byte(b'b');
        assert_eq!(le.buffer(), "b");

        // The single-ESC form is the same rule, stated once.
        let mut le = LineEditor::new();
        le.feed_byte(0x1b);
        le.feed_byte(b'x');
        assert_eq!(le.buffer(), "");
        le.feed_byte(b'y');
        assert_eq!(le.buffer(), "y");
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
        let mut le = LineEditor::new();
        let s = le.render("");
        // Empty prompt + empty buffer -> no \x1b[<n>C.
        assert!(!s.contains("\x1b["[..].trim_start_matches('\x1b')) || !s.ends_with('C'));
    }

    #[test]
    fn tab_with_no_completion_source_is_noop() {
        // U-4d: without a registered CompletionSource, Tab is NoChange.
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

    // =========================================================================
    // U-4d tests -- Tab completion + CompletionSource trait.
    // =========================================================================

    #[test]
    fn longest_common_prefix_basic() {
        let strs = vec![
            String::from("apple"),
            String::from("apricot"),
            String::from("application"),
        ];
        assert_eq!(longest_common_prefix(&strs), "ap");
    }

    #[test]
    fn longest_common_prefix_single() {
        let strs = vec![String::from("hello")];
        assert_eq!(longest_common_prefix(&strs), "hello");
    }

    #[test]
    fn longest_common_prefix_empty() {
        let strs: Vec<String> = vec![];
        assert_eq!(longest_common_prefix(&strs), "");
    }

    #[test]
    fn longest_common_prefix_utf8_safe() {
        // Common prefix "café" -- if byte-truncation hit mid-codepoint
        // the result would panic. Verify the helper rounds to char
        // boundary.
        let strs = vec![
            String::from("café-au-lait"),
            String::from("café-noir"),
        ];
        assert_eq!(longest_common_prefix(&strs), "café-");
    }

    #[test]
    fn static_source_matches_word_prefix() {
        let src = StaticCompletionSource::new(vec![
            String::from("apple"),
            String::from("apricot"),
            String::from("banana"),
        ]);
        let comp = src.complete("ap", 2);
        assert_eq!(comp.replace_range, 0..2);
        assert_eq!(comp.candidates, vec!["apple", "apricot"]);
    }

    #[test]
    fn static_source_finds_word_at_cursor() {
        let src = StaticCompletionSource::new(vec![String::from("argument")]);
        let comp = src.complete("foo arg", 7);
        assert_eq!(comp.replace_range, 4..7);
        assert_eq!(comp.candidates, vec!["argument"]);
    }

    #[test]
    fn tab_single_candidate_completes() {
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![String::from("apple")],
        )));
        feed(&mut le, b"ap");
        let r = le.feed_byte(0x09); // Tab
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "apple");
        assert_eq!(le.cursor(), 5);
    }

    #[test]
    fn tab_multi_candidate_extends_to_common_prefix() {
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![
                String::from("apple"),
                String::from("application"),
                String::from("apparatus"),
            ],
        )));
        feed(&mut le, b"a");
        let r = le.feed_byte(0x09);
        // Common prefix of all three is "app" -- "ap" extends to "app".
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "app");
        assert_eq!(le.cursor(), 3);
    }

    #[test]
    fn tab_multi_candidate_no_extension_enters_menu() {
        // D4: when the shared prefix is exhausted, Tab enters the cycling menu,
        // applies candidate[0], and emits MenuShow{selected: 0}.
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![
                String::from("apple"),
                String::from("application"),
                String::from("apparatus"),
            ],
        )));
        feed(&mut le, b"app");
        let r = le.feed_byte(0x09);
        match r {
            EditorAction::MenuShow {
                candidates,
                selected,
                unlisted,
            } => {
                assert_eq!(candidates.len(), 3);
                assert_eq!(selected, 0);
                assert_eq!(unlisted, 0);
            }
            other => panic!("expected MenuShow; got {:?}", other),
        }
        // candidate[0] ("apple") is applied to the buffer.
        assert_eq!(le.buffer(), "apple");
        assert_eq!(le.cursor(), 5);
    }

    #[test]
    fn tab_menu_cycles_and_wraps() {
        // D4: each subsequent Tab applies the next candidate; the cycle wraps.
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![
                String::from("apple"),
                String::from("application"),
                String::from("apparatus"),
            ],
        )));
        feed(&mut le, b"app");
        let _ = le.feed_byte(0x09); // -> apple (selected 0)
        assert_eq!(le.buffer(), "apple");
        let r = le.feed_byte(0x09); // -> application (selected 1)
        assert!(matches!(r, EditorAction::MenuShow { selected: 1, .. }));
        assert_eq!(le.buffer(), "application");
        assert_eq!(le.cursor(), "application".len());
        let _ = le.feed_byte(0x09); // -> apparatus (selected 2)
        assert_eq!(le.buffer(), "apparatus");
        let r = le.feed_byte(0x09); // wraps -> apple (selected 0)
        assert!(matches!(r, EditorAction::MenuShow { selected: 0, .. }));
        assert_eq!(le.buffer(), "apple");
    }

    #[test]
    fn tab_menu_enter_finalizes_without_submitting() {
        // D4: Enter while the menu is open dismisses it + keeps the selection,
        // but does NOT submit (Redraw, not Accept) -- "ready for more typing".
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![String::from("apple"), String::from("apricot")],
        )));
        feed(&mut le, b"ap");
        let _ = le.feed_byte(0x09); // common prefix is "ap"; already there -> menu, apply "apple"
        assert_eq!(le.buffer(), "apple");
        let r = le.feed_byte(b'\r'); // Enter finalizes
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "apple");
        // A subsequent Enter now submits (normal mode).
        let r2 = le.feed_byte(b'\r');
        assert_eq!(r2, EditorAction::Accept(String::from("apple")));
    }

    #[test]
    fn tab_menu_typing_dismisses_and_appends() {
        // D4: any non-menu key dismisses the menu (keeping the applied pick)
        // and is processed in Normal mode -- a char appends after the word.
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![String::from("apple"), String::from("apricot")],
        )));
        feed(&mut le, b"ap");
        let _ = le.feed_byte(0x09); // menu -> "apple"
        assert_eq!(le.buffer(), "apple");
        let r = le.feed_byte(b'X'); // dismisses + appends
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "appleX");
        assert_eq!(le.cursor(), 6);
    }

    #[test]
    fn tab_zero_candidates_is_noop() {
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![String::from("apple")],
        )));
        feed(&mut le, b"xy");
        let r = le.feed_byte(0x09);
        assert_eq!(r, EditorAction::NoChange);
        assert_eq!(le.buffer(), "xy");
    }

    #[test]
    fn clear_completion_source_disables_tab() {
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            vec![String::from("apple")],
        )));
        le.clear_completion_source();
        feed(&mut le, b"ap");
        let r = le.feed_byte(0x09);
        assert_eq!(r, EditorAction::NoChange);
        assert_eq!(le.buffer(), "ap");
    }

    /// A source with one fixed answer, so the engine can be handed any
    /// `Completions` -- ones no shipped source produces on demand included.
    struct Fixed(Completions);

    impl CompletionSource for Fixed {
        fn complete(&self, _buffer: &str, _cursor: usize) -> Completions {
            self.0.clone()
        }
    }

    /// Type `typed`, install a source answering `listed`, `extension` and
    /// `unlisted` for the whole of it, and press Tab.
    fn tab_fixed(
        typed: &str,
        listed: &[&str],
        extension: Option<&str>,
        unlisted: usize,
    ) -> (LineEditor, EditorAction) {
        let mut le = LineEditor::new();
        le.set_completion_source(alloc::boxed::Box::new(Fixed(Completions {
            replace_range: 0..typed.len(),
            candidates: listed.iter().map(|s| String::from(*s)).collect(),
            extension: extension.map(String::from),
            unlisted,
        })));
        feed(&mut le, typed.as_bytes());
        let r = le.feed_byte(0x09);
        (le, r)
    }

    #[test]
    fn tab_extends_to_the_sources_extension_not_the_listed_prefix() {
        // The listed two share "fab"; the source says every match shares only
        // "fa" -- a list it capped, or matches it cannot write. Its word wins.
        let (le, r) = tab_fixed("f", &["fab1 ", "fab2 "], Some("fa"), 3);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "fa");
    }

    #[test]
    fn the_engine_takes_no_prefix_from_the_inserted_texts() {
        // Quoted, these share "'my file" -- a prefix ending inside the quoting.
        // With no extension from the source, Tab opens the menu instead.
        let (le, r) = tab_fixed("my", &["'my file1' ", "'my file2' "], None, 0);
        assert_eq!(menu_at(&r), Some((0, 0)), "{:?}", r);
        assert_eq!(le.buffer(), "'my file1' ");
    }

    #[test]
    fn an_extension_applies_with_nothing_listed() {
        // Every match unwritable: nothing to list, but the shared part is.
        let (le, r) = tab_fixed("e", &[], Some("esc"), 2);
        assert_eq!(r, EditorAction::Redraw);
        assert_eq!(le.buffer(), "esc");
        let (le, r) = tab_fixed("esc", &[], None, 2);
        assert_eq!(r, EditorAction::NoChange);
        assert_eq!(le.buffer(), "esc");
    }

    #[test]
    fn a_lone_listed_candidate_of_a_larger_set_is_not_the_completion() {
        // Applied as a unique match this would be a Redraw with the same
        // buffer; the menu is what says other matches exist.
        let (le, r) = tab_fixed("f", &["fab1 "], None, 3);
        match r {
            EditorAction::MenuShow {
                candidates,
                selected: 0,
                unlisted: 3,
            } => assert_eq!(candidates, vec![String::from("fab1 ")]),
            other => panic!("expected the menu; got {:?}", other),
        }
        assert_eq!(le.buffer(), "fab1 ");
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

    #[test]
    fn the_menu_carries_the_unlisted_count_through_every_cycle() {
        let (mut le, r) = tab_fixed("fa", &["fa1 ", "fa2 "], None, 7);
        assert_eq!(menu_at(&r), Some((0, 7)), "{:?}", r);
        let r = le.feed_byte(0x09);
        assert_eq!(menu_at(&r), Some((1, 7)), "{:?}", r);
        let r = le.feed_byte(0x09);
        assert_eq!(menu_at(&r), Some((0, 7)), "{:?}", r);
        assert_eq!(le.buffer(), "fa1 ");
    }

    // ----- D4: the completion-menu candidate strip --------------------------

    #[test]
    fn menu_strip_highlights_selected() {
        let c = [
            String::from("apple"),
            String::from("application"),
            String::from("apparatus"),
        ];
        let r = menu_strip(&c, 1, 0, 80);
        assert!(
            r.contains("\x1b[7mapplication\x1b[0m"),
            "highlight: {:?}",
            r
        );
        assert!(r.contains("apple") && r.contains("apparatus"));
        // All three fit the budget -> no truncation markers, and no count.
        assert!(!r.starts_with("< ") && !r.ends_with(" >"));
        assert!(!r.contains("more"), "{:?}", r);
    }

    /// Twenty candidates of 19 columns each -- far past one 80-column line.
    fn wide_candidates() -> Vec<String> {
        (0..20)
            .map(|i| format!("candidate-number-{:02}", i))
            .collect()
    }

    #[test]
    fn menu_strip_windows_around_selected_when_overflowing() {
        // Many wide candidates: the selected one stays visible + markers appear.
        let r = menu_strip(&wide_candidates(), 15, 0, 80);
        assert!(
            r.contains("\x1b[7mcandidate-number-15\x1b[0m"),
            "selected visible: {:?}",
            r
        );
        assert!(r.starts_with("< "), "left truncation marker: {:?}", r);
    }

    #[test]
    fn menu_strip_counts_the_matches_no_tab_reaches() {
        let c = [String::from("fa1 "), String::from("fa2 ")];
        let r = menu_strip(&c, 0, 44, 80);
        let count = crate::ansi::fg(crate::palette::Role::Path, "+44 more");
        assert!(r.ends_with(&count), "{:?}", r);
        assert!(
            r.contains("\x1b[7mfa1 \x1b[0m") && r.contains("fa2 "),
            "{:?}",
            r
        );
    }

    #[test]
    fn menu_strip_keeps_to_one_row_at_any_width() {
        // The narrow set is the one that can fail the budget: two-column
        // candidates fill the window to within a column of it, so a count the
        // budget did not make room for runs the line over (and the clip then
        // cuts the count off). The wide set overflows narrow terminals on its
        // own, which only the clip can stop.
        let narrow: Vec<String> = (0..100).map(|i| format!("{:02}", i)).collect();
        let count = crate::ansi::fg(crate::palette::Role::Path, "+65536 more");
        for width in [80, 40, 20, 12, 5, 1] {
            for (cands, sels) in [
                (wide_candidates(), [0, 10, 19]),
                (narrow.clone(), [0, 50, 99]),
            ] {
                for sel in sels {
                    let r = menu_strip(&cands, sel, 65536, width);
                    let w = crate::ansi::visible_width(&r);
                    assert!(
                        w < width.max(1),
                        "{} columns at width {}: {:?}",
                        w,
                        width,
                        r
                    );
                    if width >= 40 {
                        assert!(r.ends_with(&count), "width {}: {:?}", width, r);
                    }
                }
            }
        }
    }

    #[test]
    fn clip_visible_counts_columns_not_escapes() {
        assert_eq!(clip_visible("abc", 5), "abc");
        let cut = clip_visible("\x1b[7mabcdef\x1b[0m", 3);
        assert_eq!(cut, "\x1b[7mabc\x1b[0m");
        assert_eq!(crate::ansi::visible_width(&cut), 3);
        assert_eq!(
            crate::ansi::visible_width(&clip_visible("x\x1b[38;2;1;2;3myz", 2)),
            2
        );
    }

    // ----- D4: the strip on a real terminal model ---------------------------
    //
    // These feed the editor's bytes to `vt` -- the terminal Halcyon renders
    // with -- and assert on the SCREEN. The strip used to be drawn with a
    // save/restore of the absolute cursor position; at the bottom row the
    // newline before it scrolled the screen, the restore landed on the strip's
    // row, and each Tab left a stale copy of the prompt behind. Every
    // assertion on emitted bytes passed.

    /// What the REPL does with each action, screen-wise: anything that leaves
    /// the menu erases the strip first; a redraw or the menu renders. One byte
    /// per read, as typing arrives. A single chunk renders every action in the
    /// FINAL state, and that hid the bottom-row defect: the erase before each
    /// redraw moved up from the wrong row and landed on the prompt by luck.
    fn drive(vt: &mut vt::Vt, le: &mut LineEditor, prompt: &str, bytes: &[u8]) {
        for &b in bytes {
            for a in le.feed_bytes(&[b]) {
                if !matches!(a, EditorAction::MenuShow { .. } | EditorAction::NoChange) {
                    vt.feed(le.clear_menu().as_bytes());
                }
                if matches!(a, EditorAction::Redraw | EditorAction::MenuShow { .. }) {
                    vt.feed(le.render(prompt).as_bytes());
                }
            }
        }
    }

    /// The same, with every byte in one read (a paste): each action then
    /// renders the final state.
    fn drive_as_one_read(vt: &mut vt::Vt, le: &mut LineEditor, prompt: &str, bytes: &[u8]) {
        for a in le.feed_bytes(bytes) {
            if !matches!(a, EditorAction::MenuShow { .. } | EditorAction::NoChange) {
                vt.feed(le.clear_menu().as_bytes());
            }
            if matches!(a, EditorAction::Redraw | EditorAction::MenuShow { .. }) {
                vt.feed(le.render(prompt).as_bytes());
            }
        }
    }

    fn screen(vt: &vt::Vt) -> Vec<String> {
        (0..vt.rows).map(|r| screen_row(vt, r)).collect()
    }

    fn screen_row(vt: &vt::Vt, r: usize) -> String {
        let s: String = (0..vt.cols).map(|c| vt.cells[r * vt.cols + c].ch).collect();
        String::from(s.trim_end())
    }

    /// A `cols`x5 screen with `above` rows of output, then the prompt; the
    /// editor told the width or not.
    fn prompt_on_screen(
        cols: usize,
        above: usize,
        width_known: bool,
        cands: &[&str],
    ) -> (vt::Vt, LineEditor) {
        let mut vt = vt::Vt::new(cols, 5);
        for i in 0..above {
            vt.feed(format!("out{}\r\n", i).as_bytes());
        }
        let mut le = LineEditor::new();
        if width_known {
            le.set_cols(cols);
        }
        le.set_completion_source(alloc::boxed::Box::new(StaticCompletionSource::new(
            cands.iter().map(|s| String::from(*s)).collect(),
        )));
        vt.feed(le.render("% ").as_bytes());
        (vt, le)
    }

    const APPS: [&str; 3] = ["apple", "application", "apparatus"];
    const STRIP: &str = "apple  application  apparatus";

    #[test]
    fn the_strip_survives_a_scroll_at_the_bottom_row() {
        for width_known in [true, false] {
            let (mut vt, mut le) = prompt_on_screen(40, 4, width_known, &APPS);
            drive(&mut vt, &mut le, "% ", b"app\t");
            // The strip's newline scrolled the screen by one row.
            assert_eq!(
                screen_row(&vt, 3),
                "% apple",
                "width known: {}",
                width_known
            );
            assert_eq!(screen_row(&vt, 4), STRIP);
            assert_eq!((vt.cy, vt.cx), (3, 7));
            drive(&mut vt, &mut le, "% ", b"\t\t");
            assert_eq!(screen_row(&vt, 0), "out1");
            assert_eq!(screen_row(&vt, 2), "out3");
            assert_eq!(screen_row(&vt, 3), "% apparatus");
            assert_eq!(screen_row(&vt, 4), STRIP);
            assert_eq!((vt.cy, vt.cx), (3, 11));
            // A key that leaves the menu takes the strip with it.
            drive(&mut vt, &mut le, "% ", b"x");
            assert_eq!(screen_row(&vt, 3), "% apparatusx");
            assert_eq!(screen_row(&vt, 4), "");
            assert_eq!((vt.cy, vt.cx), (3, 12));
            // The same keys as one read (a paste) end outside the menu, so no
            // strip is ever drawn and nothing scrolls. The screen differs from
            // the typed one only by that scroll, and must be as clean: one
            // prompt row, the cursor after its text, no strip anywhere.
            let (mut pasted, mut le2) = prompt_on_screen(40, 4, width_known, &APPS);
            drive_as_one_read(&mut pasted, &mut le2, "% ", b"app\t\t\tx");
            let rows = screen(&pasted);
            let prompts: Vec<usize> = (0..rows.len())
                .filter(|&r| rows[r].starts_with("% "))
                .collect();
            assert_eq!(prompts, [pasted.cy], "{:?}", rows);
            assert_eq!(rows[pasted.cy], "% apparatusx");
            assert_eq!(pasted.cx, 12);
            assert!(
                !rows.iter().any(|r| r.contains("application")),
                "{:?}",
                rows
            );
        }
    }

    #[test]
    fn a_paste_that_ends_in_the_menu_leaves_what_typing_does() {
        // Every action in the read renders the final state, strip included, so
        // the strip is drawn -- at the bottom row -- once per action.
        for width_known in [true, false] {
            let (mut typed, mut le) = prompt_on_screen(40, 4, width_known, &APPS);
            drive(&mut typed, &mut le, "% ", b"app\t\t");
            let (mut pasted, mut le2) = prompt_on_screen(40, 4, width_known, &APPS);
            drive_as_one_read(&mut pasted, &mut le2, "% ", b"app\t\t");
            assert_eq!(
                screen(&pasted),
                screen(&typed),
                "width known: {}",
                width_known
            );
            assert_eq!((pasted.cy, pasted.cx), (typed.cy, typed.cx));
            assert_eq!(screen_row(&typed, 3), "% application");
            assert_eq!(screen_row(&typed, 4), STRIP);
        }
    }

    #[test]
    fn with_room_below_nothing_scrolls() {
        // The control: the same keys where the prompt has rows beneath it.
        for width_known in [true, false] {
            let (mut vt, mut le) = prompt_on_screen(40, 1, width_known, &APPS);
            drive(&mut vt, &mut le, "% ", b"app\t\t");
            assert_eq!(screen_row(&vt, 0), "out0");
            assert_eq!(screen_row(&vt, 1), "% application");
            assert_eq!(screen_row(&vt, 2), STRIP);
            assert_eq!((vt.cy, vt.cx), (1, 13));
            drive(&mut vt, &mut le, "% ", b"x");
            assert_eq!(screen_row(&vt, 2), "");
        }
    }

    #[test]
    fn the_strip_goes_below_the_block_not_below_the_cursor() {
        // `{ app` + Enter continues the line (the brace is open), `}` closes
        // it, and two Lefts put the cursor back after `app` on the first row.
        let (mut vt, mut le) = prompt_on_screen(40, 0, true, &APPS);
        drive(&mut vt, &mut le, "% ", b"{ app\r}\x1b[D\x1b[D\t");
        assert_eq!(le.buffer(), "{ apple\n}");
        assert_eq!(screen_row(&vt, 0), "% { apple");
        assert!(
            screen_row(&vt, 1).ends_with('}'),
            "{:?}",
            screen_row(&vt, 1)
        );
        assert_eq!(screen_row(&vt, 2), STRIP);
        assert_eq!((vt.cy, vt.cx), (0, 9));
        drive(&mut vt, &mut le, "% ", b"x");
        assert!(
            screen_row(&vt, 1).ends_with('}'),
            "{:?}",
            screen_row(&vt, 1)
        );
        assert_eq!(screen_row(&vt, 2), "");
    }

    #[test]
    fn a_narrow_terminal_keeps_the_strip_to_one_row() {
        let long = ["apple-one-two-three-four", "application-name-too-long"];
        let (mut vt, mut le) = prompt_on_screen(20, 0, true, &long);
        drive(&mut vt, &mut le, "% ", b"app\t");
        assert_eq!(le.buffer(), "appl");
        drive(&mut vt, &mut le, "% ", b"\t");
        // The applied pick wraps the prompt's own block (two rows); the strip
        // is the one row below it, and nothing below that.
        assert!(!screen_row(&vt, 2).is_empty());
        assert_eq!(screen_row(&vt, 3), "");
        drive(&mut vt, &mut le, "% ", b"x");
        assert_eq!(screen_row(&vt, 2), "");
        assert_eq!(screen_row(&vt, 3), "");
    }

    #[test]
    fn clear_menu_erases_the_strip_before_the_cursor_moves_elsewhere() {
        // The REPL's note path: erase, move off the line, print, redraw.
        let (mut vt, mut le) = prompt_on_screen(40, 0, true, &APPS);
        drive(&mut vt, &mut le, "% ", b"app\t");
        assert_eq!(screen_row(&vt, 1), STRIP);
        vt.feed(le.clear_menu().as_bytes());
        // Erased, and the cursor is back where the render left it.
        assert_eq!(screen_row(&vt, 1), "");
        assert_eq!((vt.cy, vt.cx), (0, 7));
        vt.feed(b"\r\nnote\r\n");
        le.reset_render_position();
        vt.feed(le.render("% ").as_bytes());
        // The old strip is gone; the menu is still open, so it is redrawn
        // below the fresh prompt.
        assert_eq!(screen_row(&vt, 0), "% apple");
        assert_eq!(screen_row(&vt, 1), "note");
        assert_eq!(screen_row(&vt, 2), "% apple");
        assert_eq!(screen_row(&vt, 3), STRIP);
        assert_eq!(le.clear_menu().is_empty(), false);
        assert!(
            le.clear_menu().is_empty(),
            "a second clear has nothing to erase"
        );
    }

    #[test]
    fn a_render_that_leaves_the_menu_erases_the_strip_itself() {
        // The REPL erases first; a render must not depend on it.
        for width_known in [true, false] {
            let (mut vt, mut le) = prompt_on_screen(40, 0, width_known, &APPS);
            drive(&mut vt, &mut le, "% ", b"app\t");
            assert_eq!(screen_row(&vt, 1), STRIP);
            le.feed_byte(b'x');
            vt.feed(le.render("% ").as_bytes());
            assert_eq!(screen_row(&vt, 1), "", "width known: {}", width_known);
            assert!(le.clear_menu().is_empty(), "the render took the strip");
            // Into search mode, the same.
            let (mut vt, mut le) = prompt_on_screen(40, 0, width_known, &APPS);
            drive(&mut vt, &mut le, "% ", b"app\t");
            le.feed_byte(0x12); // Ctrl-R
            assert!(le.is_searching());
            vt.feed(le.render("% ").as_bytes());
            assert_eq!(screen_row(&vt, 1), "", "width known: {}", width_known);
        }
    }

    #[test]
    fn a_cursor_moved_elsewhere_forgets_the_strip() {
        // After the caller moves the cursor (clear-screen, a notification),
        // the strip's recorded place names nothing: erasing "there" could
        // erase a row of whatever is drawn now.
        let (mut vt, mut le) = prompt_on_screen(40, 0, true, &APPS);
        drive(&mut vt, &mut le, "% ", b"app\t");
        le.reset_render_position();
        assert!(le.clear_menu().is_empty());
        let (mut vt, mut le) = prompt_on_screen(40, 0, true, &APPS);
        drive(&mut vt, &mut le, "% ", b"app\t");
        le.reset();
        assert!(le.clear_menu().is_empty());
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

    // =========================================================================
    // #115c tests -- command-line validity coloring (Bonfire fen / cinnabar).
    // =========================================================================

    // The Bonfire diagnostic SGR runs (UTOPIA-VISUAL.md section 4.1):
    //   fen      = #6a9a6a = 106,154,106 (a resolvable command)
    //   cinnabar = #c06050 = 192,96,80   (an unresolvable command)
    const FEN_SGR: &str = "38;2;106;154;106";
    const CINNABAR_SGR: &str = "38;2;192;96;80";

    #[test]
    fn colorize_known_command_is_fen() {
        let mut le = LineEditor::new();
        le.set_known_commands(vec![String::from("cat"), String::from("ls")]);
        feed(&mut le, b"ls -la");
        let s = le.render("> ");
        assert!(s.contains(FEN_SGR), "a known command should render fen: {:?}", s);
        assert!(!s.contains(CINNABAR_SGR));
    }

    #[test]
    fn colorize_unknown_command_is_cinnabar() {
        let mut le = LineEditor::new();
        le.set_known_commands(vec![String::from("ls")]);
        feed(&mut le, b"lx -la");
        let s = le.render("> ");
        assert!(
            s.contains(CINNABAR_SGR),
            "an unknown command should render cinnabar: {:?}",
            s
        );
        assert!(!s.contains(FEN_SGR));
    }

    #[test]
    fn colorize_disabled_when_index_empty() {
        // No set_known_commands -> coloring off -> the command renders plain
        // (the byte-identical-to-pre-#115c property host tests rely on).
        let mut le = LineEditor::new();
        feed(&mut le, b"ls");
        let s = le.render("> ");
        assert!(!s.contains(FEN_SGR));
        assert!(!s.contains(CINNABAR_SGR));
        assert!(s.contains("> ls"));
    }

    #[test]
    fn colorize_skips_command_by_path() {
        // A '/'-bearing token is a command-by-path the name index cannot
        // speak to -- left default rather than mis-flagged cinnabar.
        let mut le = LineEditor::new();
        le.set_known_commands(vec![String::from("ls")]);
        feed(&mut le, b"./script");
        let s = le.render("> ");
        assert!(!s.contains(FEN_SGR));
        assert!(!s.contains(CINNABAR_SGR));
    }

    // ---- winsize / line-wrap (the CPR width handshake + wrapped-row render).
    // The runnable proof lives in /u-repl-test (this crate is no_std, so these
    // #[cfg(test)] cases document the contract without executing).

    #[test]
    fn cpr_reply_sets_width_and_is_not_a_key() {
        // A Cursor-Position-Report ESC[<rows>;<cols>R sets the width to its
        // 2nd param and is absorbed (never a keystroke).
        let mut le = LineEditor::new();
        assert_eq!(le.cols(), None);
        feed(&mut le, b"\x1b[24;80R");
        assert_eq!(le.cols(), Some(80));
        assert_eq!(le.buffer(), "");
    }

    #[test]
    fn cpr_reply_reassembles_when_split() {
        // The byte-at-a-time CSI parser reassembles a reply dribbled across
        // reads (the HVF serial split) for free.
        let mut le = LineEditor::new();
        feed(&mut le, b"\x1b[40");
        feed(&mut le, b";132R");
        assert_eq!(le.cols(), Some(132));
    }

    #[test]
    fn non_cpr_and_zero_size_leave_width_unset() {
        let mut le = LineEditor::new();
        feed(&mut le, b"\x1b[80R"); // one param -> not a CPR
        feed(&mut le, b"\x1b[0;0R"); // zero size -> rejected
        assert_eq!(le.cols(), None);
    }

    #[test]
    fn width_unknown_render_is_byte_preserved() {
        // cols == None keeps the pre-fix newline-only render: it opens with
        // the single-line clear "\r\x1b[K".
        let mut le = LineEditor::new();
        feed(&mut le, b"hello");
        let s = le.render("> ");
        assert!(s.starts_with("\r\x1b[K"));
    }

    #[test]
    fn wrapped_render_moves_up_to_the_block_top() {
        // The discriminator. cols=20, prompt "> " (2) + 30 chars = 32 cells ->
        // 2 physical rows; the cursor ends on row 1. After a cursor move the
        // next render moves UP to the block top ("\x1b[1A") before clearing --
        // the fix. A newline-only render would treat the wrapped line as ONE
        // line and emit no up-move (the duplication bug).
        let mut le = LineEditor::new();
        le.set_cols(20);
        feed(&mut le, &[b'a'; 30]);
        let _ = le.render("> "); // records prev_cursor_row = 1
        feed(&mut le, &[0x02]); // Ctrl-B: cursor left, still row 1
        let s = le.render("> ");
        assert!(s.starts_with("\x1b[1A\r\x1b[J"));
    }
}
