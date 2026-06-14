// nora::editor -- the modal core (Helix/vim-style), adapted from Stratum's
// tui/src/editor.rs onto the native nora::text engine + kaua key events.
//
// Pure + host-testable: `handle_key` maps a `kaua::KeyEvent` to a buffer edit
// or a mode change. File I/O is NOT done here -- a `:w` / `:e` raises a
// `Request` the binary (nora::main) executes against libthyla-rs::fs, so the
// editor stays terminal-free and testable. The Stratum POC's system clipboard
// (pbcopy/pbpaste) becomes an internal yank register (Thylacine has no system
// clipboard at v1.0); everything else -- the four modes, the normal/insert/
// visual/command handlers, the `:w`/`:q`/`:wq`/`:q!` ex-commands -- ports 1:1,
// plus `/` search and `:e`.

use alloc::string::{String, ToString};
use alloc::vec::Vec;
use kaua::event::{KeyCode, KeyEvent, Mods};

use crate::text::{Pos, TextBuffer};
use crate::wrap;

/// The editor mode. `Command(buf)` holds the in-progress command/search line
/// (the leading `:` or `/` distinguishes the two on Enter).
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Mode {
    Normal,
    Insert,
    Visual,
    Command(String),
    /// The `[Space]` command palette, open over Normal; `sel` is the highlighted
    /// entry index.
    Palette { sel: usize },
}

/// A file operation the editor requests but does not perform; the binary
/// executes it (file I/O lives outside the pure core) and reports back via
/// `mark_saved` / `load` / `set_status`.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Request {
    /// Write the buffer. `Some(path)` is a save-as; `None` saves to the
    /// current filename.
    Save(Option<String>),
    /// Replace the buffer with the contents of `path` (`:e`).
    Open(String),
}

/// The editor state.
pub struct Editor {
    pub text: TextBuffer,
    pub mode: Mode,
    pub filename: Option<String>,
    pub readonly: bool,
    pub modified: bool,
    /// First visible logical row (vertical scroll); maintained by `scroll_to`.
    pub top: usize,
    /// First visible VISUAL sub-row within `top`'s logical line (soft-wrap only;
    /// always 0 when `wrap` is off). Maintained by `scroll_to`.
    pub top_sub: usize,
    /// Soft-wrap mode: wrap long logical lines at the viewport width vs. clip
    /// them at the right edge. Toggled at runtime via the `[Space]` palette.
    pub wrap: bool,
    /// A transient one-line message (cleared at the next key).
    pub status: Option<String>,
    /// Set when the editor wants to terminate (the binary breaks its loop).
    pub quit: bool,
    /// Visual-mode selection anchor (the other end is the cursor).
    anchor: Option<Pos>,
    /// The internal yank/paste register (replaces the system clipboard).
    register: String,
    /// Last `/` pattern, repeated by `n`.
    last_search: String,
    /// A pending file op for the binary to execute.
    request: Option<Request>,
}

impl Editor {
    /// A new editor over `content`. `filename` is the save target (`None` for
    /// an unnamed buffer); `readonly` disables edits (a `nora -R` view).
    pub fn new(filename: Option<String>, content: &str, readonly: bool) -> Self {
        Editor {
            text: TextBuffer::new(content),
            mode: Mode::Normal,
            filename,
            readonly,
            modified: false,
            top: 0,
            top_sub: 0,
            wrap: false,
            status: None,
            quit: false,
            anchor: None,
            register: String::new(),
            last_search: String::new(),
            request: None,
        }
    }

    /// The short mode tag for the status line.
    pub fn mode_str(&self) -> &'static str {
        match self.mode {
            // The palette overlays Normal -- show the underlying chip.
            Mode::Normal | Mode::Palette { .. } => {
                if self.readonly {
                    "VIEW"
                } else {
                    "NOR"
                }
            }
            Mode::Insert => "INS",
            Mode::Visual => "VIS",
            Mode::Command(_) => "CMD",
        }
    }

    /// The in-progress command line (`:`/`/` + text), if in command mode.
    pub fn command_buf(&self) -> Option<&str> {
        match &self.mode {
            Mode::Command(s) => Some(s),
            _ => None,
        }
    }

    /// The `[Space]` palette entry labels (for the view to render).
    pub fn palette_labels(&self) -> Vec<&'static str> {
        PALETTE.iter().map(|p| p.label).collect()
    }

    /// The highlighted palette entry index, if the palette is open.
    pub fn palette_sel(&self) -> Option<usize> {
        match self.mode {
            Mode::Palette { sel } => Some(sel),
            _ => None,
        }
    }

    /// The visual selection span `(lo, hi)` inclusive, if in visual mode.
    pub fn selection(&self) -> Option<(Pos, Pos)> {
        match (self.mode.clone(), self.anchor) {
            (Mode::Visual, Some(a)) => {
                let c = self.text.cursor();
                if a <= c {
                    Some((a, c))
                } else {
                    Some((c, a))
                }
            }
            _ => None,
        }
    }

    /// Take the pending file request (the binary calls this after each key).
    pub fn take_request(&mut self) -> Option<Request> {
        self.request.take()
    }

    /// Record a successful save (the binary calls this after writing).
    pub fn mark_saved(&mut self, path: String, bytes: usize) {
        self.status = Some(string_fmt_saved(&path, bytes));
        self.filename = Some(path);
        self.modified = false;
    }

    /// Replace the buffer with freshly-loaded content (`:e` success).
    pub fn load(&mut self, path: Option<String>, content: &str) {
        self.text = TextBuffer::new(content);
        self.filename = path;
        self.modified = false;
        self.top = 0;
        self.top_sub = 0;
        self.mode = Mode::Normal;
        self.anchor = None;
        self.status = Some("opened".to_string());
    }

    /// Set the transient status message (the binary reports errors here).
    pub fn set_status(&mut self, msg: String) {
        self.status = Some(msg);
    }

    /// Adjust the scroll anchor so the cursor stays within a `height`-row
    /// viewport of text width `tw`. Without wrap this is a logical-row scroll
    /// (`tw` unused); with wrap it scrolls in VISUAL rows so a long wrapped line
    /// never pushes the cursor off screen.
    pub fn scroll_to(&mut self, tw: u16, height: usize) {
        if height == 0 {
            return;
        }
        if !self.wrap {
            self.top_sub = 0;
            let row = self.text.cursor().0;
            if row < self.top {
                self.top = row;
            } else if row >= self.top + height {
                self.top = row + 1 - height;
            }
            return;
        }

        let cur = wrap::cursor_visual(&self.text, tw);
        let anchor = (self.top, self.top_sub);
        // Cursor visually above the anchor (incl. a stale anchor past a delete)
        // -> top-align to it.
        if cur.0 < anchor.0 || (cur.0 == anchor.0 && cur.1 < anchor.1) {
            self.top = cur.0;
            self.top_sub = cur.1;
            return;
        }
        // Forward-walk up to `height` visual rows from the anchor; if the cursor
        // is reached it is visible (no change), else scroll down so the cursor
        // sits on the last visible row.
        let mut p = anchor;
        let mut steps = 0;
        loop {
            if p == cur {
                return; // visible at screen row `steps`
            }
            if steps == height - 1 {
                break;
            }
            match wrap::forward(&self.text, p, tw) {
                Some(q) => {
                    p = q;
                    steps += 1;
                }
                // cur unreachable forward (shouldn't happen for a valid cursor
                // >= anchor) -> top-align defensively.
                None => {
                    self.top = cur.0;
                    self.top_sub = cur.1;
                    return;
                }
            }
        }
        let (tr, ts) = wrap::back_n(&self.text, cur, height - 1, tw);
        self.top = tr;
        self.top_sub = ts;
    }

    /// Toggle soft-wrap (the `[Space]` palette entry), re-anchoring cleanly.
    pub fn toggle_wrap(&mut self) {
        self.wrap = !self.wrap;
        self.top_sub = 0;
    }

    /// Dispatch one key by mode.
    pub fn handle_key(&mut self, key: KeyEvent) {
        self.status = None;
        match &self.mode {
            Mode::Normal => self.normal(key),
            Mode::Insert => self.insert(key),
            Mode::Visual => self.visual(key),
            Mode::Command(_) => self.command(key),
            Mode::Palette { .. } => self.palette(key),
        }
    }

    // -- per-mode handlers ------------------------------------------------

    fn normal(&mut self, key: KeyEvent) {
        let editable = !self.readonly;
        match key.code {
            KeyCode::Char('i') if editable => {
                self.text.checkpoint();
                self.mode = Mode::Insert;
            }
            KeyCode::Char('a') if editable => {
                self.text.checkpoint();
                self.text.move_right();
                self.mode = Mode::Insert;
            }
            KeyCode::Char('o') if editable => {
                self.text.checkpoint();
                self.text.move_end();
                self.text.insert_newline();
                self.mode = Mode::Insert;
                self.modified = true;
            }
            KeyCode::Char('A') if editable => {
                self.text.checkpoint();
                self.text.move_end();
                self.mode = Mode::Insert;
            }
            KeyCode::Char('v') => {
                self.anchor = Some(self.text.cursor());
                self.mode = Mode::Visual;
            }
            KeyCode::Char(':') => self.mode = Mode::Command(":".to_string()),
            KeyCode::Char('/') => self.mode = Mode::Command("/".to_string()),
            KeyCode::Char(' ') => self.mode = Mode::Palette { sel: 0 },
            KeyCode::Char('n') if !self.last_search.is_empty() => {
                self.search(&self.last_search.clone());
            }
            KeyCode::Char('p') if editable => {
                if !self.register.is_empty() {
                    self.text.checkpoint();
                    self.text.insert_str(&self.register);
                    self.modified = true;
                }
            }

            // Navigation.
            KeyCode::Char('h') | KeyCode::Left => self.text.move_left(),
            KeyCode::Char('j') | KeyCode::Down => self.text.move_down(),
            KeyCode::Char('k') | KeyCode::Up => self.text.move_up(),
            KeyCode::Char('l') | KeyCode::Right => self.text.move_right(),
            KeyCode::Char('0') | KeyCode::Home => self.text.move_home(),
            KeyCode::Char('$') | KeyCode::End => self.text.move_end(),
            KeyCode::Char('g') => self.text.move_top(),
            KeyCode::Char('G') => self.text.move_bottom(),
            KeyCode::Char('w') => self.text.move_word_forward(),
            KeyCode::Char('b') => self.text.move_word_back(),
            KeyCode::PageUp => self.page(true),
            KeyCode::PageDown => self.page(false),

            // Editing.
            KeyCode::Char('x') if editable => {
                self.text.checkpoint();
                self.text.delete_char();
                self.modified = true;
            }
            KeyCode::Char('d') if editable => {
                self.text.checkpoint();
                self.text.delete_line();
                self.modified = true;
            }
            KeyCode::Char('u') if editable => {
                if self.text.undo() {
                    self.modified = true;
                } else {
                    self.status = Some("already at oldest change".to_string());
                }
            }

            KeyCode::Char('q') | KeyCode::Esc if self.readonly => self.quit = true,
            _ => {}
        }
    }

    fn insert(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Esc => self.mode = Mode::Normal,
            KeyCode::Enter => {
                self.text.insert_newline();
                self.modified = true;
            }
            KeyCode::Backspace => {
                self.text.backspace();
                self.modified = true;
            }
            KeyCode::Delete => {
                self.text.delete_char();
                self.modified = true;
            }
            KeyCode::Tab => {
                // Soft tab: spaces keep the 1-cell-per-char model intact (a
                // literal tab read from disk still round-trips -- see text.rs).
                self.text.insert_str("    ");
                self.modified = true;
            }
            KeyCode::Char(c) => {
                self.text.insert_char(c);
                self.modified = true;
            }
            _ => {}
        }
    }

    fn visual(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Esc => {
                self.anchor = None;
                self.mode = Mode::Normal;
            }
            KeyCode::Char('y') => {
                if let Some((lo, hi)) = self.selection() {
                    self.register = self.text.range_text(lo, hi);
                }
                self.anchor = None;
                self.mode = Mode::Normal;
                self.status = Some("yanked".to_string());
            }
            KeyCode::Char('d') | KeyCode::Char('x') if !self.readonly => {
                if let Some((lo, hi)) = self.selection() {
                    self.register = self.text.range_text(lo, hi);
                    self.text.checkpoint();
                    self.text.delete_range(lo, hi);
                    self.modified = true;
                }
                self.anchor = None;
                self.mode = Mode::Normal;
            }

            // Navigation extends the selection (the anchor stays put).
            KeyCode::Char('h') | KeyCode::Left => self.text.move_left(),
            KeyCode::Char('j') | KeyCode::Down => self.text.move_down(),
            KeyCode::Char('k') | KeyCode::Up => self.text.move_up(),
            KeyCode::Char('l') | KeyCode::Right => self.text.move_right(),
            KeyCode::Char('0') | KeyCode::Home => self.text.move_home(),
            KeyCode::Char('$') | KeyCode::End => self.text.move_end(),
            KeyCode::Char('g') => self.text.move_top(),
            KeyCode::Char('G') => self.text.move_bottom(),
            KeyCode::Char('w') => self.text.move_word_forward(),
            KeyCode::Char('b') => self.text.move_word_back(),
            KeyCode::PageUp => self.page(true),
            KeyCode::PageDown => self.page(false),
            _ => {}
        }
    }

    fn command(&mut self, key: KeyEvent) {
        let buf = match &self.mode {
            Mode::Command(b) => b.clone(),
            _ => return,
        };
        match key.code {
            KeyCode::Esc => self.mode = Mode::Normal,
            KeyCode::Backspace => {
                let mut b = buf;
                b.pop();
                // Erasing the leading ':' / '/' exits command mode.
                if b.is_empty() {
                    self.mode = Mode::Normal;
                } else {
                    self.mode = Mode::Command(b);
                }
            }
            KeyCode::Enter => self.run_command(&buf),
            KeyCode::Char(c) => {
                let mut b = buf;
                b.push(c);
                self.mode = Mode::Command(b);
            }
            _ => {}
        }
    }

    fn palette(&mut self, key: KeyEvent) {
        let sel = match self.mode {
            Mode::Palette { sel } => sel,
            _ => return,
        };
        let n = PALETTE.len();
        match key.code {
            KeyCode::Esc | KeyCode::Char(' ') => self.mode = Mode::Normal,
            KeyCode::Down | KeyCode::Char('j') => self.mode = Mode::Palette { sel: (sel + 1) % n },
            KeyCode::Up | KeyCode::Char('k') => {
                self.mode = Mode::Palette { sel: (sel + n - 1) % n }
            }
            KeyCode::Char('n') if key.mods.contains(Mods::CTRL) => {
                self.mode = Mode::Palette { sel: (sel + 1) % n }
            }
            KeyCode::Char('p') if key.mods.contains(Mods::CTRL) => {
                self.mode = Mode::Palette { sel: (sel + n - 1) % n }
            }
            KeyCode::Enter => {
                self.mode = Mode::Normal;
                self.run_palette(sel);
            }
            _ => {}
        }
    }

    fn run_palette(&mut self, sel: usize) {
        let action = match PALETTE.get(sel) {
            Some(p) => p.action,
            None => return,
        };
        match action {
            PaletteAction::ToggleWrap => {
                self.toggle_wrap();
                self.status = Some(if self.wrap {
                    "soft-wrap: on".to_string()
                } else {
                    "soft-wrap: off".to_string()
                });
            }
            PaletteAction::Save => {
                if self.readonly {
                    self.status = Some("read-only".to_string());
                } else {
                    self.request = Some(Request::Save(None));
                }
            }
            PaletteAction::Quit => {
                if self.modified {
                    self.status = Some("unsaved changes (:q! to discard)".to_string());
                } else {
                    self.quit = true;
                }
            }
        }
    }

    // -- command + search execution ---------------------------------------

    fn run_command(&mut self, buf: &str) {
        self.mode = Mode::Normal;
        if let Some(pat) = buf.strip_prefix('/') {
            let pat = pat.to_string();
            self.last_search = pat.clone();
            self.search(&pat);
            return;
        }
        let cmd = buf.trim_start_matches(':').trim();
        // Split a leading verb from an optional argument (":w name", ":e f").
        let (verb, arg) = match cmd.split_once(char::is_whitespace) {
            Some((v, a)) => (v, a.trim()),
            None => (cmd, ""),
        };
        match verb {
            "w" if self.readonly => self.status = Some("read-only".to_string()),
            "w" => self.request = Some(Request::Save(opt(arg))),
            "wq" if self.readonly => self.status = Some("read-only".to_string()),
            "wq" => {
                self.request = Some(Request::Save(opt(arg)));
                self.quit = true;
            }
            "q" => {
                if self.modified {
                    self.status = Some("unsaved changes (:q! to discard)".to_string());
                } else {
                    self.quit = true;
                }
            }
            "q!" => self.quit = true,
            "e" if arg.is_empty() => self.status = Some(":e needs a filename".to_string()),
            "e" => {
                if self.modified {
                    self.status = Some("unsaved changes (:w first, or :e!)".to_string());
                } else {
                    self.request = Some(Request::Open(arg.to_string()));
                }
            }
            "e!" if arg.is_empty() => self.status = Some(":e! needs a filename".to_string()),
            "e!" => self.request = Some(Request::Open(arg.to_string())),
            _ => self.status = Some(string_fmt_unknown(cmd)),
        }
    }

    fn search(&mut self, pat: &str) {
        if pat.is_empty() {
            return;
        }
        match self.text.find(pat) {
            Some((r, c)) => self.text.set_cursor(r, c),
            None => self.status = Some(string_fmt_not_found(pat)),
        }
    }

    fn page(&mut self, up: bool) {
        for _ in 0..PAGE {
            if up {
                self.text.move_up();
            } else {
                self.text.move_down();
            }
        }
    }
}

/// Lines moved per PageUp/PageDown (a fixed step; viewport-relative paging is a
/// nicety deferred -- the editor does not know the viewport height).
const PAGE: usize = 20;

/// One `[Space]` command-palette entry.
#[derive(Clone, Copy)]
struct PaletteItem {
    label: &'static str,
    action: PaletteAction,
}

#[derive(Clone, Copy)]
enum PaletteAction {
    ToggleWrap,
    Save,
    Quit,
}

/// The fixed palette entries (a real menu mechanism; user aliases / a richer set
/// accrete later -- T3 adds the buffer entries on this list).
const PALETTE: &[PaletteItem] = &[
    PaletteItem {
        label: "toggle soft-wrap",
        action: PaletteAction::ToggleWrap,
    },
    PaletteItem {
        label: "save",
        action: PaletteAction::Save,
    },
    PaletteItem {
        label: "quit",
        action: PaletteAction::Quit,
    },
];

/// `""` -> `None`, else `Some(trimmed)`. For an optional command argument.
fn opt(arg: &str) -> Option<String> {
    if arg.is_empty() {
        None
    } else {
        Some(arg.to_string())
    }
}

// Small no_std format helpers (avoid pulling `format!` into the hot path's
// signature; each is one allocation).
fn string_fmt_saved(path: &str, bytes: usize) -> String {
    let mut s = String::from("\"");
    s.push_str(path);
    s.push_str("\" ");
    s.push_str(&itoa(bytes));
    s.push_str("B written");
    s
}

fn string_fmt_unknown(cmd: &str) -> String {
    let mut s = String::from("unknown command: ");
    s.push_str(cmd);
    s
}

fn string_fmt_not_found(pat: &str) -> String {
    let mut s = String::from("not found: ");
    s.push_str(pat);
    s
}

fn itoa(mut n: usize) -> String {
    if n == 0 {
        return String::from("0");
    }
    let mut buf = [0u8; 20];
    let mut i = buf.len();
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    // SAFETY: buf[i..] is ASCII digits.
    String::from(core::str::from_utf8(&buf[i..]).unwrap_or("0"))
}

#[cfg(test)]
mod tests {
    use super::*;
    use kaua::event::KeyEvent;

    fn ch(c: char) -> KeyEvent {
        KeyEvent::char(c)
    }
    fn code(c: KeyCode) -> KeyEvent {
        KeyEvent::new(c)
    }

    fn type_str(ed: &mut Editor, s: &str) {
        for c in s.chars() {
            ed.handle_key(ch(c));
        }
    }

    #[test]
    fn insert_text_then_escape() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        assert_eq!(ed.mode, Mode::Insert);
        type_str(&mut ed, "hello");
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.mode, Mode::Normal);
        assert_eq!(ed.text.content(), "hello");
        assert!(ed.modified);
    }

    #[test]
    fn enter_in_insert_splits() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "ab");
        ed.handle_key(code(KeyCode::Enter));
        type_str(&mut ed, "cd");
        assert_eq!(ed.text.content(), "ab\ncd");
    }

    #[test]
    fn o_opens_a_line_below() {
        let mut ed = Editor::new(None, "first", false);
        ed.handle_key(ch('o'));
        type_str(&mut ed, "second");
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.text.content(), "first\nsecond");
        assert_eq!(ed.mode, Mode::Normal);
    }

    #[test]
    fn normal_navigation_hjkl() {
        let mut ed = Editor::new(None, "abc\ndef", false);
        ed.handle_key(ch('l'));
        ed.handle_key(ch('j'));
        assert_eq!(ed.text.cursor(), (1, 1));
        ed.handle_key(ch('0'));
        assert_eq!(ed.text.cursor(), (1, 0));
        ed.handle_key(ch('G'));
        assert_eq!(ed.text.cursor().0, 1);
        ed.handle_key(ch('g'));
        assert_eq!(ed.text.cursor().0, 0);
    }

    #[test]
    fn x_deletes_and_undo_restores() {
        let mut ed = Editor::new(None, "abc", false);
        ed.handle_key(ch('x')); // delete 'a'
        assert_eq!(ed.text.content(), "bc");
        ed.handle_key(ch('u')); // undo
        assert_eq!(ed.text.content(), "abc");
    }

    #[test]
    fn dd_deletes_the_line() {
        let mut ed = Editor::new(None, "a\nb\nc", false);
        ed.handle_key(ch('j')); // to line 1
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.content(), "a\nc");
    }

    #[test]
    fn visual_yank_and_paste() {
        let mut ed = Editor::new(None, "hello", false);
        ed.handle_key(ch('v')); // visual at (0,0)
        ed.handle_key(ch('l')); // extend to (0,1) -> "he"
        ed.handle_key(ch('l')); // (0,2) -> "hel"
        ed.handle_key(ch('y')); // yank "hel"
        assert_eq!(ed.mode, Mode::Normal);
        ed.handle_key(ch('$')); // end of line
        ed.handle_key(ch('p')); // paste at end
        assert_eq!(ed.text.content(), "hellohel");
    }

    #[test]
    fn visual_delete_cuts_selection() {
        let mut ed = Editor::new(None, "abcdef", false);
        ed.handle_key(ch('v'));
        ed.handle_key(ch('l'));
        ed.handle_key(ch('l')); // select "abc"
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.content(), "def");
        assert_eq!(ed.mode, Mode::Normal);
    }

    #[test]
    fn command_write_raises_save_request() {
        let mut ed = Editor::new(Some("/f".to_string()), "x", false);
        type_str(&mut ed, ":w");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_request(), Some(Request::Save(None)));
        assert_eq!(ed.mode, Mode::Normal);
    }

    #[test]
    fn command_save_as_carries_name() {
        let mut ed = Editor::new(None, "x", false);
        type_str(&mut ed, ":w out.txt");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(
            ed.take_request(),
            Some(Request::Save(Some("out.txt".to_string())))
        );
    }

    #[test]
    fn command_q_guards_unsaved() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "z");
        ed.handle_key(code(KeyCode::Esc));
        type_str(&mut ed, ":q");
        ed.handle_key(code(KeyCode::Enter));
        assert!(!ed.quit); // blocked: modified
        type_str(&mut ed, ":q!");
        ed.handle_key(code(KeyCode::Enter));
        assert!(ed.quit);
    }

    #[test]
    fn command_wq_saves_and_quits() {
        let mut ed = Editor::new(Some("/f".to_string()), "x", false);
        type_str(&mut ed, ":wq");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_request(), Some(Request::Save(None)));
        assert!(ed.quit);
    }

    #[test]
    fn command_e_raises_open_request() {
        let mut ed = Editor::new(None, "", false);
        type_str(&mut ed, ":e other");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_request(), Some(Request::Open("other".to_string())));
    }

    #[test]
    fn command_backspace_exits_on_empty() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch(':'));
        ed.handle_key(code(KeyCode::Backspace)); // erase ':'
        assert_eq!(ed.mode, Mode::Normal);
    }

    #[test]
    fn search_moves_cursor_and_n_repeats() {
        let mut ed = Editor::new(None, "foo\nbar\nfoo", false);
        type_str(&mut ed, "/foo");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.text.cursor(), (2, 0)); // next "foo" after (0,0)
        ed.handle_key(ch('n'));
        assert_eq!(ed.text.cursor(), (0, 0)); // wrapped
    }

    #[test]
    fn readonly_blocks_edits_and_q_quits() {
        let mut ed = Editor::new(None, "abc", true);
        ed.handle_key(ch('x')); // ignored
        assert_eq!(ed.text.content(), "abc");
        assert_eq!(ed.mode_str(), "VIEW");
        ed.handle_key(ch('q'));
        assert!(ed.quit);
    }

    #[test]
    fn scroll_follows_cursor() {
        let mut ed = Editor::new(None, "0\n1\n2\n3\n4\n5\n6\n7", false);
        // viewport 3 rows; move to row 5. (wrap off -> width is irrelevant.)
        for _ in 0..5 {
            ed.handle_key(ch('j'));
        }
        ed.scroll_to(80, 3);
        assert_eq!(ed.top, 3); // rows 3,4,5 visible
        ed.handle_key(ch('g')); // back to top
        ed.scroll_to(80, 3);
        assert_eq!(ed.top, 0);
    }

    #[test]
    fn space_opens_palette_and_esc_closes() {
        let mut ed = Editor::new(None, "x", false);
        ed.handle_key(ch(' '));
        assert_eq!(ed.mode, Mode::Palette { sel: 0 });
        assert_eq!(ed.palette_sel(), Some(0));
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.mode, Mode::Normal);
        assert_eq!(ed.palette_sel(), None);
    }

    #[test]
    fn palette_toggles_wrap_via_enter() {
        let mut ed = Editor::new(None, "x", false);
        assert!(!ed.wrap);
        ed.handle_key(ch(' ')); // open palette (sel 0 == "toggle soft-wrap")
        ed.handle_key(code(KeyCode::Enter));
        assert!(ed.wrap);
        assert_eq!(ed.mode, Mode::Normal);
        // toggle back off
        ed.handle_key(ch(' '));
        ed.handle_key(code(KeyCode::Enter));
        assert!(!ed.wrap);
    }

    #[test]
    fn palette_navigates_to_quit_and_guards_unsaved() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "z");
        ed.handle_key(code(KeyCode::Esc)); // modified
        ed.handle_key(ch(' ')); // palette sel 0
        ed.handle_key(ch('j')); // sel 1 (save)
        ed.handle_key(ch('j')); // sel 2 (quit)
        assert_eq!(ed.palette_sel(), Some(2));
        ed.handle_key(code(KeyCode::Enter));
        assert!(!ed.quit); // blocked: unsaved
        assert_eq!(ed.mode, Mode::Normal);
    }

    #[test]
    fn wrap_scroll_keeps_cursor_on_a_long_line() {
        // One 200-char line; wrap on; tw 10, height 5.
        let line: String = core::iter::repeat('a').take(200).collect();
        let mut ed = Editor::new(None, &line, false);
        ed.toggle_wrap();
        ed.text.set_cursor(0, 150); // visual sub 15
        ed.scroll_to(10, 5);
        // cursor sub 15 placed on the last of 5 visible rows -> top_sub 11.
        assert_eq!(ed.top, 0);
        assert_eq!(ed.top_sub, 11);
        // moving home re-anchors to the top.
        ed.text.set_cursor(0, 0);
        ed.scroll_to(10, 5);
        assert_eq!(ed.top_sub, 0);
    }
}
