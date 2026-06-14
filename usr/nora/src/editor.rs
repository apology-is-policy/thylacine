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
    /// The `[Space]` which-key menu, open over Normal: the next key invokes the
    /// matching entry (Helix-style), not an arrow-navigated selection.
    Menu,
    /// The buffer picker (Space-b): an arrow-selectable list of open buffers;
    /// Enter switches. `sel` is the highlighted buffer index.
    BufferPicker { sel: usize },
    /// The fuzzy file picker (Space-f): a `query` line over the cwd's files; the
    /// filtered list is arrow-selectable, Enter opens the file in a new buffer.
    FilePicker { query: String, sel: usize },
}

/// A file operation the editor requests but does not perform; the binary
/// executes it (file I/O lives outside the pure core) and reports back via
/// `mark_saved` / `load` / `set_status`.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Request {
    /// Write the buffer. `Some(path)` is a save-as; `None` saves to the
    /// current filename.
    Save(Option<String>),
    /// Open `path` in a new buffer (`:e` and the file picker).
    Open(String),
    /// List `path`'s files to populate the fuzzy file picker (Space-f). The
    /// binary reads the directory and calls `open_file_picker` back.
    ListDir(String),
}

/// A backgrounded buffer's parked state -- the per-buffer half of `Editor`. The
/// ACTIVE buffer lives in the Editor's own fields (so the per-mode handlers are
/// unchanged by multi-buffer); a buffer is saved here on switch-away and loaded
/// back on switch-to.
#[derive(Clone)]
struct DocState {
    text: TextBuffer,
    filename: Option<String>,
    readonly: bool,
    modified: bool,
    top: usize,
    top_sub: usize,
    left: usize,
    anchor: Option<Pos>,
    last_search: String,
}

impl DocState {
    fn new(filename: Option<String>, content: &str, readonly: bool) -> Self {
        DocState {
            text: TextBuffer::new(content),
            filename,
            readonly,
            modified: false,
            top: 0,
            top_sub: 0,
            left: 0,
            anchor: None,
            last_search: String::new(),
        }
    }
}

/// The editor state. The first block of fields is the ACTIVE buffer (mirrored
/// into `bufs[active]` on switch-away); `mode`/`wrap`/`register`/`status` etc.
/// are editor-global (shared across buffers).
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
    /// First visible column (horizontal scroll). Non-wrap only -- a clipped long
    /// line scrolls sideways to keep the cursor visible; always 0 in wrap mode.
    /// Maintained by `scroll_to`.
    pub left: usize,
    /// Soft-wrap mode: wrap long logical lines at the viewport width vs. clip
    /// them at the right edge. Toggled at runtime via the `[Space]` menu.
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
    /// All open buffers' parked state. `bufs[active]` mirrors the live fields
    /// above (refreshed on switch-away); `bufs.len() >= 1` always.
    bufs: Vec<DocState>,
    /// Index of the active buffer within `bufs`.
    active: usize,
    /// Cached text-region width from the last `scroll_to` (the binary's
    /// viewport). Wrap-aware vertical movement needs the wrap width at key time,
    /// before the next render; 0 until the first frame (the initial redraw runs
    /// before any key, so this is set before the first movement).
    vp_tw: u16,
    /// Fuzzy-file-picker candidates (the cwd's file names), set by the binary
    /// when the picker opens (Space-f -> Request::ListDir -> open_file_picker).
    file_entries: Vec<String>,
}

impl Editor {
    /// A new editor over `content`. `filename` is the save target (`None` for
    /// an unnamed buffer); `readonly` disables edits (a `nora -R` view).
    pub fn new(filename: Option<String>, content: &str, readonly: bool) -> Self {
        let d = DocState::new(filename, content, readonly);
        let mut ed = Editor {
            text: TextBuffer::new(""),
            mode: Mode::Normal,
            filename: None,
            readonly: false,
            modified: false,
            top: 0,
            top_sub: 0,
            left: 0,
            wrap: false,
            status: None,
            quit: false,
            anchor: None,
            register: String::new(),
            last_search: String::new(),
            request: None,
            bufs: Vec::new(),
            active: 0,
            vp_tw: 0,
            file_entries: Vec::new(),
        };
        ed.bufs.push(d.clone());
        ed.load_active(d);
        ed
    }

    /// The short mode tag for the status line.
    pub fn mode_str(&self) -> &'static str {
        match self.mode {
            // The menu + pickers overlay Normal -- show the underlying chip.
            Mode::Normal | Mode::Menu | Mode::BufferPicker { .. } | Mode::FilePicker { .. } => {
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

    /// The `[Space]` which-key menu entries as `(key, label)` (for the view).
    pub fn menu_entries(&self) -> Vec<(char, &'static str)> {
        MENU.iter().map(|m| (m.key, m.label)).collect()
    }

    /// Whether the `[Space]` which-key menu is open.
    pub fn menu_open(&self) -> bool {
        matches!(self.mode, Mode::Menu)
    }

    /// The buffer-picker's highlighted index, if it is open.
    pub fn buffer_picker_sel(&self) -> Option<usize> {
        match self.mode {
            Mode::BufferPicker { sel } => Some(sel),
            _ => None,
        }
    }

    /// The file-picker's `(query, sel)`, if it is open.
    pub fn file_picker_state(&self) -> Option<(&str, usize)> {
        match &self.mode {
            Mode::FilePicker { query, sel } => Some((query.as_str(), *sel)),
            _ => None,
        }
    }

    /// The file-picker candidates filtered by the current query (a fuzzy
    /// subsequence match), in the original (sorted) order.
    pub fn file_picker_filtered(&self) -> Vec<String> {
        let q = match &self.mode {
            Mode::FilePicker { query, .. } => query.as_str(),
            _ => return Vec::new(),
        };
        self.file_entries
            .iter()
            .filter(|n| fuzzy_match(q, n))
            .cloned()
            .collect()
    }

    /// Populate + open the fuzzy file picker (the binary calls this in response
    /// to a `Request::ListDir`, passing the directory's file names).
    pub fn open_file_picker(&mut self, entries: Vec<String>) {
        self.file_entries = entries;
        self.mode = Mode::FilePicker {
            query: String::new(),
            sel: 0,
        };
    }

    /// The `:` ex-commands matching what is typed, as `(name, description)` for
    /// the live command popup (empty unless in `:` command mode). The verb is the
    /// first token after `:`; an empty verb lists every command.
    pub fn command_completions(&self) -> Vec<(&'static str, &'static str)> {
        let line = match &self.mode {
            Mode::Command(b) => b.as_str(),
            _ => return Vec::new(),
        };
        // Only the ':' command line gets completions, not '/' search.
        let rest = match line.strip_prefix(':') {
            Some(r) => r,
            None => return Vec::new(),
        };
        let verb = rest.split_whitespace().next().unwrap_or("");
        COMMANDS
            .iter()
            .filter(|c| c.0.starts_with(verb))
            .copied()
            .collect()
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

    // -- multiple buffers (T3) --------------------------------------------

    /// Snapshot the live (active) fields into a `DocState`.
    fn save_active(&self) -> DocState {
        DocState {
            text: self.text.clone(),
            filename: self.filename.clone(),
            readonly: self.readonly,
            modified: self.modified,
            top: self.top,
            top_sub: self.top_sub,
            left: self.left,
            anchor: self.anchor,
            last_search: self.last_search.clone(),
        }
    }

    /// Load a `DocState` into the live (active) fields.
    fn load_active(&mut self, d: DocState) {
        self.text = d.text;
        self.filename = d.filename;
        self.readonly = d.readonly;
        self.modified = d.modified;
        self.top = d.top;
        self.top_sub = d.top_sub;
        self.left = d.left;
        self.anchor = d.anchor;
        self.last_search = d.last_search;
    }

    /// The number of open buffers.
    pub fn buffer_count(&self) -> usize {
        self.bufs.len()
    }

    /// `[active/count]` when more than one buffer is open (for the status bar).
    pub fn buffer_indicator(&self) -> Option<String> {
        if self.bufs.len() > 1 {
            Some(string_fmt_buffers(self.active + 1, self.bufs.len()))
        } else {
            None
        }
    }

    /// Show the top buffer-tab strip when more than one buffer is open (Helix
    /// `bufferline = "multiple"`).
    pub fn show_tabs(&self) -> bool {
        self.bufs.len() > 1
    }

    /// Each open buffer as `(basename, is_active)` for the tab strip. The active
    /// buffer's name comes from the live field (its parked copy is stale until a
    /// switch-away); the rest from their parked state.
    pub fn buffer_tabs(&self) -> Vec<(String, bool)> {
        self.bufs
            .iter()
            .enumerate()
            .map(|(i, d)| {
                let name = if i == self.active {
                    self.filename.as_deref()
                } else {
                    d.filename.as_deref()
                }
                .unwrap_or("[No Name]");
                let base = name.rsplit('/').next().unwrap_or(name);
                (base.to_string(), i == self.active)
            })
            .collect()
    }

    /// Whether the cursor is drawn as a mode-coloured block (text modes) vs. the
    /// terminal cursor on the command line (Command). The block IS the cursor in
    /// text modes, so the binary hides the real one there.
    pub fn block_cursor(&self) -> bool {
        matches!(self.mode, Mode::Normal | Mode::Insert | Mode::Visual)
    }

    /// Switch the active buffer to index `i` (parking the current one). Resets
    /// to Normal mode for a clean cross-buffer state.
    fn switch_to(&mut self, i: usize) {
        if i == self.active || i >= self.bufs.len() {
            return;
        }
        self.bufs[self.active] = self.save_active();
        let d = self.bufs[i].clone();
        self.load_active(d);
        self.active = i;
        self.mode = Mode::Normal;
        self.status = self.buffer_indicator();
    }

    /// Switch to the next / previous open buffer (cyclic). A single buffer is a
    /// no-op with a hint.
    pub fn next_buffer(&mut self) {
        let n = self.bufs.len();
        if n > 1 {
            self.switch_to((self.active + 1) % n);
        } else {
            self.status = Some("only one buffer".to_string());
        }
    }

    pub fn prev_buffer(&mut self) {
        let n = self.bufs.len();
        if n > 1 {
            self.switch_to((self.active + n - 1) % n);
        } else {
            self.status = Some("only one buffer".to_string());
        }
    }

    /// Open `content` in a buffer (`:e` success). Replaces a lone, pristine,
    /// unnamed buffer in place (vim-like); otherwise adds a new buffer and makes
    /// it active.
    pub fn open_buffer(&mut self, filename: Option<String>, content: &str) {
        let d = DocState::new(filename, content, false);
        let pristine = self.bufs.len() == 1
            && !self.modified
            && self.filename.is_none()
            && self.text.content().is_empty();
        if pristine {
            self.bufs[0] = d.clone();
        } else {
            self.bufs[self.active] = self.save_active();
            self.bufs.push(d.clone());
            self.active = self.bufs.len() - 1;
        }
        self.load_active(d);
        self.mode = Mode::Normal;
        self.status = self.buffer_indicator().or(Some("opened".to_string()));
    }

    /// Close the active buffer (`:bd`). Refuses to discard unsaved changes
    /// unless `force`; the last buffer cannot be closed.
    pub fn close_buffer(&mut self, force: bool) {
        if self.bufs.len() <= 1 {
            self.status = Some("only one buffer".to_string());
            return;
        }
        if self.modified && !force {
            self.status = Some("unsaved changes (:bd! to discard)".to_string());
            return;
        }
        self.bufs.remove(self.active);
        if self.active >= self.bufs.len() {
            self.active = self.bufs.len() - 1;
        }
        let d = self.bufs[self.active].clone();
        self.load_active(d);
        self.mode = Mode::Normal;
        self.status = self.buffer_indicator();
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
        // Cache the wrap width so wrap-aware vertical movement -- which runs at key
        // time, before the next render -- knows the wrap geometry.
        self.vp_tw = tw;
        if !self.wrap {
            self.top_sub = 0;
            let (row, col) = self.text.cursor();
            if row < self.top {
                self.top = row;
            } else if row >= self.top + height {
                self.top = row + 1 - height;
            }
            // Horizontal scroll: keep the cursor column within [left, left+tw).
            let twc = tw.max(1) as usize;
            if col < self.left {
                self.left = col;
            } else if col >= self.left + twc {
                self.left = col + 1 - twc;
            }
            return;
        }
        self.left = 0;

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

    /// Toggle soft-wrap (the `[Space]` menu entry), re-anchoring cleanly.
    pub fn toggle_wrap(&mut self) {
        self.wrap = !self.wrap;
        self.top_sub = 0;
        self.left = 0;
    }

    /// Move the cursor down one row. In soft-wrap mode this is one VISUAL row, so
    /// a long wrapped line is traversed sub-row by sub-row (the cursor keeps its
    /// column within the row); otherwise one logical line. The wrap walk uses the
    /// cached viewport width `vp_tw`.
    pub fn move_down(&mut self) {
        if !(self.wrap && self.vp_tw > 0) {
            self.text.move_down();
            return;
        }
        let tw = self.vp_tw as usize;
        let (row, col) = self.text.cursor();
        let sub = col / tw;
        let vcol = col - sub * tw; // preferred column within the visual row
        if sub + 1 < wrap::row_rows(&self.text, row, self.vp_tw) {
            let new = (sub + 1) * tw + vcol;
            self.text.set_cursor(row, new.min(self.text.char_len(row)));
        } else if row + 1 < self.text.line_count() {
            self.text
                .set_cursor(row + 1, vcol.min(self.text.char_len(row + 1)));
        }
    }

    /// Move the cursor up one row (one VISUAL row in soft-wrap mode, else one
    /// logical line).
    pub fn move_up(&mut self) {
        if !(self.wrap && self.vp_tw > 0) {
            self.text.move_up();
            return;
        }
        let tw = self.vp_tw as usize;
        let (row, col) = self.text.cursor();
        let sub = col / tw;
        let vcol = col - sub * tw;
        if sub > 0 {
            self.text.set_cursor(row, (sub - 1) * tw + vcol);
        } else if row > 0 {
            let prow = row - 1;
            let last_sub = wrap::row_rows(&self.text, prow, self.vp_tw) - 1;
            let new = last_sub * tw + vcol;
            self.text.set_cursor(prow, new.min(self.text.char_len(prow)));
        }
    }

    /// Dispatch one key by mode.
    pub fn handle_key(&mut self, key: KeyEvent) {
        self.status = None;
        match &self.mode {
            Mode::Normal => self.normal(key),
            Mode::Insert => self.insert(key),
            Mode::Visual => self.visual(key),
            Mode::Command(_) => self.command(key),
            Mode::Menu => self.menu(key),
            Mode::BufferPicker { .. } => self.buffer_picker(key),
            Mode::FilePicker { .. } => self.file_picker(key),
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
            KeyCode::Char(' ') => self.mode = Mode::Menu,
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
            KeyCode::Char('j') | KeyCode::Down => self.move_down(),
            KeyCode::Char('k') | KeyCode::Up => self.move_up(),
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
            // Helix allows cursor motion in every mode -- arrows (and Home/End/
            // PageUp/Down) navigate without leaving Insert.
            KeyCode::Left => self.text.move_left(),
            KeyCode::Right => self.text.move_right(),
            KeyCode::Up => self.move_up(),
            KeyCode::Down => self.move_down(),
            KeyCode::Home => self.text.move_home(),
            KeyCode::End => self.text.move_end(),
            KeyCode::PageUp => self.page(true),
            KeyCode::PageDown => self.page(false),
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
            KeyCode::Char('j') | KeyCode::Down => self.move_down(),
            KeyCode::Char('k') | KeyCode::Up => self.move_up(),
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

    fn menu(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Esc | KeyCode::Char(' ') => self.mode = Mode::Normal,
            KeyCode::Char(c) => {
                // Which-key: a bound key runs its action; any other key dismisses
                // the menu (Helix-style -- no arrow navigation, no selection).
                let action = MENU.iter().find(|m| m.key == c).map(|m| m.action);
                self.mode = Mode::Normal;
                if let Some(a) = action {
                    self.run_menu(a);
                }
            }
            _ => {}
        }
    }

    fn run_menu(&mut self, action: MenuAction) {
        match action {
            MenuAction::FilePicker => {
                // Ask the binary to list the cwd; it calls open_file_picker back.
                self.request = Some(Request::ListDir(".".to_string()));
            }
            MenuAction::BufferPicker => {
                self.mode = Mode::BufferPicker { sel: self.active };
            }
            MenuAction::NextBuffer => self.next_buffer(),
            MenuAction::PrevBuffer => self.prev_buffer(),
            MenuAction::CloseBuffer => self.close_buffer(false),
            MenuAction::ToggleWrap => {
                self.toggle_wrap();
                self.status = Some(if self.wrap {
                    "soft-wrap: on".to_string()
                } else {
                    "soft-wrap: off".to_string()
                });
            }
            MenuAction::Save => {
                if self.readonly {
                    self.status = Some("read-only".to_string());
                } else {
                    self.request = Some(Request::Save(None));
                }
            }
            MenuAction::Quit => {
                if self.modified {
                    self.status = Some("unsaved changes (:q! to discard)".to_string());
                } else {
                    self.quit = true;
                }
            }
        }
    }

    fn buffer_picker(&mut self, key: KeyEvent) {
        let (sel, n) = match self.mode {
            Mode::BufferPicker { sel } => (sel, self.bufs.len()),
            _ => return,
        };
        match key.code {
            KeyCode::Esc => self.mode = Mode::Normal,
            KeyCode::Up | KeyCode::Char('k') => {
                self.mode = Mode::BufferPicker {
                    sel: (sel + n - 1) % n,
                }
            }
            KeyCode::Down | KeyCode::Char('j') => {
                self.mode = Mode::BufferPicker {
                    sel: (sel + 1) % n,
                }
            }
            KeyCode::Enter => {
                self.mode = Mode::Normal;
                self.switch_to(sel);
            }
            _ => {}
        }
    }

    fn file_picker(&mut self, key: KeyEvent) {
        let (mut query, mut sel) = match &self.mode {
            Mode::FilePicker { query, sel } => (query.clone(), *sel),
            _ => return,
        };
        // The list reflects the CURRENT (pre-key) query; an arrow / Enter acts on
        // what is displayed. A char / Backspace re-filters and resets the cursor.
        let count = self.file_picker_filtered().len();
        match key.code {
            KeyCode::Esc => {
                self.mode = Mode::Normal;
                return;
            }
            KeyCode::Enter => {
                if let Some(name) = self.file_picker_filtered().into_iter().nth(sel) {
                    self.request = Some(Request::Open(name)); // opens in a new buffer
                }
                self.mode = Mode::Normal;
                return;
            }
            KeyCode::Up => sel = sel.saturating_sub(1),
            KeyCode::Down => {
                if sel + 1 < count {
                    sel += 1;
                }
            }
            KeyCode::Char('p') if key.mods.contains(Mods::CTRL) => sel = sel.saturating_sub(1),
            KeyCode::Char('n') if key.mods.contains(Mods::CTRL) => {
                if sel + 1 < count {
                    sel += 1;
                }
            }
            KeyCode::Backspace => {
                query.pop();
                sel = 0;
            }
            KeyCode::Char(c) => {
                query.push(c);
                sel = 0;
            }
            _ => {}
        }
        self.mode = Mode::FilePicker { query, sel };
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
            "bn" => self.next_buffer(),
            "bp" => self.prev_buffer(),
            "bd" => self.close_buffer(false),
            "bd!" => self.close_buffer(true),
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
                self.move_up();
            } else {
                self.move_down();
            }
        }
    }
}

/// Lines moved per PageUp/PageDown (a fixed step; viewport-relative paging is a
/// nicety deferred -- the editor does not know the viewport height).
const PAGE: usize = 20;

/// One `[Space]` which-key menu entry: a single key + a description (rendered
/// like a `:` command line), invoked by pressing the key.
#[derive(Clone, Copy)]
struct MenuItem {
    key: char,
    label: &'static str,
    action: MenuAction,
}

#[derive(Clone, Copy)]
enum MenuAction {
    FilePicker,
    BufferPicker,
    NextBuffer,
    PrevBuffer,
    CloseBuffer,
    ToggleWrap,
    Save,
    Quit,
}

/// The fixed `[Space]` menu (Helix bindings where they map; nora-specific where
/// they don't). A richer / user-extensible set accretes later.
const MENU: &[MenuItem] = &[
    MenuItem {
        key: 'f',
        label: "open file picker",
        action: MenuAction::FilePicker,
    },
    MenuItem {
        key: 'b',
        label: "open buffer picker",
        action: MenuAction::BufferPicker,
    },
    MenuItem {
        key: 'n',
        label: "next buffer",
        action: MenuAction::NextBuffer,
    },
    MenuItem {
        key: 'p',
        label: "previous buffer",
        action: MenuAction::PrevBuffer,
    },
    MenuItem {
        key: 'c',
        label: "close buffer",
        action: MenuAction::CloseBuffer,
    },
    MenuItem {
        key: 'w',
        label: "toggle soft-wrap",
        action: MenuAction::ToggleWrap,
    },
    MenuItem {
        key: 's',
        label: "save file",
        action: MenuAction::Save,
    },
    MenuItem {
        key: 'q',
        label: "quit",
        action: MenuAction::Quit,
    },
];

/// A fuzzy subsequence match (case-insensitive): every char of `query` appears in
/// `candidate` in order. An empty query matches everything.
fn fuzzy_match(query: &str, candidate: &str) -> bool {
    let mut q = query.chars().map(|c| c.to_ascii_lowercase());
    let mut want = match q.next() {
        Some(c) => c,
        None => return true,
    };
    for cc in candidate.chars().map(|c| c.to_ascii_lowercase()) {
        if cc == want {
            want = match q.next() {
                Some(c) => c,
                None => return true,
            };
        }
    }
    false
}

/// The `:` ex-commands as `(name, description)`, for the live command popup
/// (C2). Mirrors the verbs handled in `run_command`; a drift here is cosmetic
/// (the popup is help text, not the dispatcher).
const COMMANDS: &[(&str, &str)] = &[
    ("w", "write the buffer (:w <name> to save-as)"),
    ("wq", "write the buffer and quit"),
    ("q", "quit (blocked if there are unsaved changes)"),
    ("q!", "quit, discarding unsaved changes"),
    ("e", "open a file in a new buffer (:e <name>)"),
    ("e!", "open a file, discarding unsaved changes"),
    ("bn", "next buffer"),
    ("bp", "previous buffer"),
    ("bd", "close the current buffer"),
    ("bd!", "close the buffer, discarding changes"),
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

fn string_fmt_buffers(idx: usize, total: usize) -> String {
    let mut s = String::from("[");
    s.push_str(&itoa(idx));
    s.push('/');
    s.push_str(&itoa(total));
    s.push(']');
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
    fn space_menu_esc_closes() {
        let mut ed = Editor::new(None, "x", false);
        ed.handle_key(ch(' '));
        assert!(ed.menu_open());
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.mode, Mode::Normal);
        assert!(!ed.menu_open());
    }

    #[test]
    fn space_menu_quit_guards_unsaved() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "z");
        ed.handle_key(code(KeyCode::Esc)); // modified
        ed.handle_key(ch(' ')); // menu
        ed.handle_key(ch('q')); // 'q' = quit
        assert!(!ed.quit); // blocked: unsaved changes
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

    #[test]
    fn open_buffer_replaces_pristine_then_adds() {
        let mut ed = Editor::new(None, "", false); // pristine (empty, unnamed)
        assert_eq!(ed.buffer_count(), 1);
        ed.open_buffer(Some("a".to_string()), "AAA");
        assert_eq!(ed.buffer_count(), 1); // replaced in place
        assert_eq!(ed.text.content(), "AAA");
        assert_eq!(ed.filename.as_deref(), Some("a"));
        ed.open_buffer(Some("b".to_string()), "BBB");
        assert_eq!(ed.buffer_count(), 2); // added (no longer pristine)
        assert_eq!(ed.text.content(), "BBB");
    }

    #[test]
    fn buffer_edits_persist_across_switch() {
        let mut ed = Editor::new(Some("a".to_string()), "AAA", false);
        ed.open_buffer(Some("b".to_string()), "BBB"); // active b
        ed.handle_key(ch('i'));
        type_str(&mut ed, "Z");
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.text.content(), "ZBBB");
        ed.prev_buffer(); // -> a
        assert_eq!(ed.text.content(), "AAA");
        ed.next_buffer(); // -> b
        assert_eq!(ed.text.content(), "ZBBB"); // b's edit preserved
    }

    #[test]
    fn close_buffer_guards_unsaved_and_removes() {
        let mut ed = Editor::new(Some("a".to_string()), "AAA", false);
        ed.open_buffer(Some("b".to_string()), "BBB");
        ed.handle_key(ch('i'));
        type_str(&mut ed, "Z");
        ed.handle_key(code(KeyCode::Esc));
        assert!(ed.modified);
        ed.close_buffer(false); // guarded
        assert_eq!(ed.buffer_count(), 2);
        ed.close_buffer(true); // forced
        assert_eq!(ed.buffer_count(), 1);
        assert_eq!(ed.text.content(), "AAA");
        ed.close_buffer(true); // last buffer can't close
        assert_eq!(ed.buffer_count(), 1);
    }

    #[test]
    fn buffer_indicator_shows_only_when_multiple() {
        let mut ed = Editor::new(None, "x", false);
        assert_eq!(ed.buffer_indicator(), None);
        ed.open_buffer(Some("b".to_string()), "y");
        assert_eq!(ed.buffer_indicator(), Some("[2/2]".to_string()));
        ed.prev_buffer();
        assert_eq!(ed.buffer_indicator(), Some("[1/2]".to_string()));
    }

    #[test]
    fn insert_mode_arrows_navigate_without_leaving_insert() {
        // A2: Helix-style -- arrows move the cursor in Insert mode.
        let mut ed = Editor::new(None, "abc\ndef", false);
        ed.handle_key(ch('i')); // Insert at (0,0)
        ed.handle_key(code(KeyCode::Right));
        assert_eq!(ed.text.cursor(), (0, 1));
        ed.handle_key(code(KeyCode::Down));
        assert_eq!(ed.text.cursor(), (1, 1));
        ed.handle_key(code(KeyCode::Home));
        assert_eq!(ed.text.cursor(), (1, 0));
        assert_eq!(ed.mode, Mode::Insert); // arrows do not exit Insert
    }

    #[test]
    fn wrap_vertical_move_steps_visual_rows() {
        // A3: in soft-wrap, Down moves one VISUAL row (staying on the long logical
        // line) before crossing to the next line; Up reverses it.
        let mut ed = Editor::new(None, "0123456789abcdefghij\nxyz", false); // line 0 = 20 chars
        ed.toggle_wrap();
        ed.scroll_to(10, 5); // tw=10 -> line 0 spans 2 visual rows; caches vp_tw
        assert_eq!(ed.text.cursor(), (0, 0));
        ed.move_down(); // visual row 1 of line 0 (same logical line)
        assert_eq!(ed.text.cursor(), (0, 10));
        ed.move_down(); // now cross to line 1
        assert_eq!(ed.text.cursor(), (1, 0));
        ed.move_up(); // back to the last visual row of line 0
        assert_eq!(ed.text.cursor(), (0, 10));
    }

    #[test]
    fn horizontal_scroll_anchor_tracks_the_cursor() {
        // A1: a long line, cursor past the viewport width -> `left` advances so the
        // cursor column stays within [left, left+tw).
        let mut ed = Editor::new(None, "0123456789abcdefghijKLMNOP", false);
        assert_eq!(ed.left, 0);
        for _ in 0..15 {
            ed.text.move_right();
        }
        ed.scroll_to(10, 5); // tw=10, cursor at col 15 -> left = 15+1-10 = 6
        assert_eq!(ed.left, 6);
        // Move back to the start -> left re-anchors to 0.
        ed.text.move_home();
        ed.scroll_to(10, 5);
        assert_eq!(ed.left, 0);
    }

    #[test]
    fn space_menu_is_which_key() {
        // C1: Space opens a which-key menu; a bound key runs its action + closes;
        // an unbound key just dismisses (no arrow navigation, no selection).
        let mut ed = Editor::new(None, "hello", false);
        ed.handle_key(ch(' '));
        assert!(ed.menu_open());
        ed.handle_key(ch('w')); // 'w' = toggle soft-wrap
        assert!(!ed.menu_open());
        assert!(ed.wrap);
        ed.handle_key(ch(' '));
        assert!(ed.menu_open());
        ed.handle_key(ch('z')); // unbound
        assert!(!ed.menu_open());
        assert!(ed.wrap); // unchanged
    }

    #[test]
    fn buffer_picker_navigates_and_switches() {
        // C3: Space-b opens an arrow-selectable buffer list; Enter switches.
        let mut ed = Editor::new(Some("alpha".to_string()), "x", false);
        ed.open_buffer(Some("beta".to_string()), "y"); // active = beta (1)
        ed.handle_key(ch(' '));
        ed.handle_key(ch('b'));
        assert_eq!(ed.buffer_picker_sel(), Some(1)); // starts at the active buffer
        ed.handle_key(code(KeyCode::Up));
        assert_eq!(ed.buffer_picker_sel(), Some(0));
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.buffer_picker_sel(), None);
        assert_eq!(ed.buffer_indicator(), Some("[1/2]".to_string())); // active now alpha
    }

    #[test]
    fn file_picker_fuzzy_filters_and_opens_in_a_new_buffer() {
        // C3: Space-f -> a fuzzy filter over the cwd files; Enter raises Open.
        let mut ed = Editor::new(None, "", false);
        ed.open_file_picker(alloc::vec![
            String::from("main.rs"),
            String::from("lib.rs"),
            String::from("Cargo.toml"),
        ]);
        assert!(ed.file_picker_state().is_some());
        assert_eq!(ed.file_picker_filtered().len(), 3); // empty query matches all
        ed.handle_key(ch('r'));
        ed.handle_key(ch('s')); // fuzzy "rs" -> main.rs, lib.rs (subsequence)
        assert_eq!(
            ed.file_picker_filtered(),
            alloc::vec![String::from("main.rs"), String::from("lib.rs")]
        );
        ed.handle_key(code(KeyCode::Enter)); // open sel 0
        assert_eq!(ed.take_request(), Some(Request::Open("main.rs".to_string())));
        assert!(ed.file_picker_state().is_none());
    }

    #[test]
    fn fuzzy_match_is_case_insensitive_subsequence() {
        assert!(fuzzy_match("", "anything"));
        assert!(fuzzy_match("mrs", "main.rs"));
        assert!(fuzzy_match("MRS", "main.rs"));
        assert!(!fuzzy_match("xyz", "main.rs"));
        assert!(!fuzzy_match("rsm", "main.rs")); // order matters
    }

    #[test]
    fn command_completions_filter_by_prefix() {
        // C2: the ':' command popup lists matching ex-commands; '/' search has none.
        let mut ed = Editor::new(None, "x", false);
        ed.handle_key(ch(':')); // ":" lists every command
        assert_eq!(ed.command_completions().len(), COMMANDS.len());
        ed.handle_key(ch('w')); // ":w" -> w, wq
        let names: alloc::vec::Vec<&str> = ed.command_completions().iter().map(|c| c.0).collect();
        assert_eq!(names, alloc::vec!["w", "wq"]);
        // '/' search gets no command list.
        let mut s = Editor::new(None, "x", false);
        s.handle_key(ch('/'));
        assert!(s.command_completions().is_empty());
    }
}
