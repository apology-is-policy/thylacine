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

use crate::debug::DebugView;
use crate::syntax::Lang;
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
    /// The completion list (Ctrl-N in Insert), open over Insert: the candidates
    /// a language server offered for the cursor position. Enter inserts, Esc
    /// cancels, and BOTH return to Insert -- completion is a detour within a
    /// typing session, not a departure from it.
    Completion { sel: usize },
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

/// A language-server query the editor wants made AT THE CURSOR, raised for the
/// binary's LSP client to issue (8e-2c).
///
/// A separate axis from [`Request`] on purpose. `Request` is file I/O the
/// binary performs synchronously and reports back; an `LspRequest` goes to a
/// server that may be absent, slow, or never answer, and its reply arrives on
/// a later poll-wake. Keeping them apart means the audited save/open path is
/// untouched by the language-server feature, and a client that never answers
/// can never wedge a save.
///
/// The editor deliberately does NOT name the file or the position: it has no
/// URI and no notion of the server's position encoding. It says "definition at
/// my cursor" and the host translates.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum LspRequest {
    /// Jump to the definition of the symbol at the cursor (`gd`).
    Definition,
    /// Describe the symbol at the cursor (`K`).
    Hover,
    /// Offer completions for the cursor position (Ctrl-N in Insert).
    Completion,
}

/// A debugger command the editor wants issued, raised by a `:` debug verb for
/// the binary's DAP host to drive against Ambush (8e-3e).
///
/// A third async axis alongside [`Request`] and [`LspRequest`], for the same
/// reasons (see [`LspRequest`]): the debugger is a persistent child that may be
/// absent, slow, or exit, and its events arrive on a later poll-wake. The editor
/// only relays these verbs and shows a status/scratch result -- it holds no
/// session state and speaks no protocol. Unlike an `LspRequest` (implicit at the
/// cursor) a debug command carries its own argument, since it names a program,
/// a breakpoint, or an expression rather than a screen position.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum DapRequest {
    /// Start debugging a program (`:debug <program>`).
    Launch(String),
    /// Set a breakpoint -- a function name, or `file:line` (`:break <spec>`).
    Break(String),
    /// Resume execution (`:cont` / `:c`).
    Continue,
    /// Step over one source line (`:next` / `:n`).
    Next,
    /// Step into a call (`:step` / `:s`).
    Step,
    /// Step out of the current function (`:stepout` / `:so`).
    StepOut,
    /// Show the call stack (`:bt`).
    Backtrace,
    /// Evaluate an expression in the current frame (`:print <expr>` / `:p`).
    Print(String),
    /// Select a call-stack frame by its dashboard row index (Call Stack `Enter`):
    /// jump the editor to the frame's source and re-scope the Variables tile to
    /// it. The index resolves against the host's own frame list.
    SelectFrame(usize),
    /// Select a goroutine by its dashboard row index (Goroutines `Enter`): switch
    /// the inspected thread and re-root the Call Stack on it. The index resolves
    /// against the host's own goroutine list.
    SelectGoroutine(usize),
    /// End the debug session (`:kill`).
    Kill,
}

/// Which dashboard pane holds keyboard focus (`Tab` cycles them; the focused
/// tile takes an ember border, NORA-IDE-UX section 2.3). `Editor` is the source
/// buffer; the rest are the debug tiles. Only meaningful while a debug session
/// is live -- the dashboard is collapsed otherwise.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DashPane {
    Editor,
    Variables,
    CallStack,
    Goroutines,
    Console,
}

/// The debugger dashboard UI state (8f-2): the display/selection state the
/// renderer reads. The DAP data itself lives in the pushed [`DebugView`]; this
/// is only "which pane has focus" and "which Console tab is up", so it survives
/// a data refresh (a stop must not steal focus off the tile you were reading).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct DashState {
    /// The focused pane (`Tab` cycles it).
    pub focus: DashPane,
    /// The active Console tab (Program = 0, Debug = 1).
    pub console_tab: usize,
    /// The highlighted row in each selectable sidebar tile (`j`/`k` move it).
    /// `var_sel` indexes the flattened Variables rows -- the `locals` group node,
    /// then its leaves when expanded; the others index their flat lists. Clamped
    /// to the live row count at render, since a stop can shrink the data under a
    /// stale selection. Highlighted only on the FOCUSED tile.
    pub var_sel: usize,
    pub stack_sel: usize,
    pub gor_sel: usize,
    /// Whether the Variables `locals` group is expanded (`l`/`h` toggle it). The
    /// per-variable nested expand (a struct's fields, a slice's elements) is a
    /// later leg -- this is the one group node's collapse.
    pub locals_expanded: bool,
}

impl DashState {
    /// The state a freshly-opened dashboard starts in: focus on the source
    /// buffer, the Program console tab up, every selection at the top, the
    /// `locals` group open.
    fn new() -> Self {
        DashState {
            focus: DashPane::Editor,
            console_tab: 0,
            var_sel: 0,
            stack_sel: 0,
            gor_sel: 0,
            locals_expanded: true,
        }
    }
}

/// One completion candidate, as the editor renders and applies it. The
/// protocol-free twin of the client's item -- same reasoning as `nora::diag`.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Candidate {
    /// What the list shows.
    pub label: String,
    /// Optional type/signature detail, shown dimmed after the label.
    pub detail: Option<String>,
    /// What Enter inserts (often the label, but a server may differ).
    pub insert: String,
}

/// A selection: a range from `anchor` to `head` (the caret). The rendered span
/// is `[min(anchor, head), max(anchor, head)]` inclusive. Multi-cursor
/// (`Editor::carets`) is a `Vec` of these; a match selected by `s` has its head
/// at the match's last char.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Sel {
    pub anchor: Pos,
    pub head: Pos,
}

impl Sel {
    /// The ordered inclusive range `(lo, hi)`.
    pub fn range(&self) -> (Pos, Pos) {
        if self.anchor <= self.head {
            (self.anchor, self.head)
        } else {
            (self.head, self.anchor)
        }
    }
}

/// One simultaneous multi-cursor edit (applied at every caret per keystroke).
#[derive(Clone, Copy)]
enum MultiEdit {
    Char(char),
    Newline,
    Backspace,
}

/// Shift a caret position `p` that lies AFTER an edit applied at `at`, so the
/// not-yet-processed carets stay valid as the buffer mutates (the multi-cursor
/// "apply, then shift the later markers" discipline -- the correctness pivot).
fn shift_after(p: Pos, at: Pos, edit: MultiEdit) -> Pos {
    match edit {
        MultiEdit::Char(_) => {
            if p.0 == at.0 && p.1 >= at.1 {
                (p.0, p.1 + 1)
            } else {
                p
            }
        }
        MultiEdit::Newline => {
            if p.0 == at.0 && p.1 >= at.1 {
                (p.0 + 1, p.1 - at.1)
            } else if p.0 > at.0 {
                (p.0 + 1, p.1)
            } else {
                p
            }
        }
        MultiEdit::Backspace => {
            // A backspace deleted the char before `at` (when at.1 > 0); same-line
            // positions after `at` shift left by one. A col-0 line-join does not
            // shift other carets at v1 (rare in a multi-insert).
            if at.1 > 0 && p.0 == at.0 && p.1 >= at.1 {
                (p.0, p.1 - 1)
            } else {
                p
            }
        }
    }
}

/// Shift `p` after a single-line inclusive delete of `[lo, hi]` (matches from
/// `s` are single-line, so `lo.0 == hi.0`).
fn shift_after_delete(p: Pos, lo: Pos, hi: Pos) -> Pos {
    if p.0 == lo.0 && p.1 > hi.1 {
        (p.0, p.1 - (hi.1 - lo.1 + 1))
    } else {
        p
    }
}

/// A find-char motion (`f`/`F`/`t`/`T`), repeatable by `;` / `,`.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum FindKind {
    Forward,      // f: onto the next <char>
    ForwardTill,  // t: just before the next <char>
    Backward,     // F: onto the previous <char>
    BackwardTill, // T: just after the previous <char>
}

impl FindKind {
    /// The opposite direction (`,` repeats reversed).
    fn reversed(self) -> FindKind {
        match self {
            FindKind::Forward => FindKind::Backward,
            FindKind::Backward => FindKind::Forward,
            FindKind::ForwardTill => FindKind::BackwardTill,
            FindKind::BackwardTill => FindKind::ForwardTill,
        }
    }
}

/// A prefix awaiting its next key: a find-char target; the match-mode selector
/// (`m` -> `m` matches the bracket, `i`/`a` open a text object); or a text
/// object awaiting its object char (`mi(` / `ma{` / `miw`).
#[derive(Clone, Copy)]
enum Pending {
    FindChar(FindKind),
    Match,
    TextObject { around: bool },
    /// The `g` goto prefix (Helix goto-mode): `gg` top, `ge` end, `gd`
    /// definition. A count still binds directly (`42g` is goto-line, resolved
    /// before the prefix is ever set).
    Goto,
    /// The `]`/`[` bracket prefix: `]d`/`[d` next/prev diagnostic.
    Bracket { forward: bool },
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
    /// Language-server diagnostics for the ACTIVE buffer, published by the
    /// binary's LSP client (8e-2). Protocol-free (see `nora::diag`); the
    /// engine only renders them. Cleared on every buffer switch/open, since
    /// they are keyed to the file that was showing.
    pub diags: crate::diag::Diagnostics,
    /// A language-server description of the symbol under the cursor (`K`),
    /// drawn last as a transient popup. NON-MODAL: the next key dismisses it
    /// and still does its job, so hover never costs a keystroke.
    hover: Option<String>,
    /// The candidates behind `Mode::Completion`. Held outside the mode so the
    /// list survives the selection moving, and is dropped when the mode closes.
    completion: Vec<Candidate>,
    /// The prefix already typed at the cursor when completion was requested,
    /// replaced by the accepted candidate. Without it, accepting `Println`
    /// after typing `Pri` would yield `PriPrintln`.
    completion_prefix: String,
    /// A language-server query awaiting the binary (see [`LspRequest`]).
    lsp_request: Option<LspRequest>,
    /// A debugger command awaiting the binary (see [`DapRequest`]).
    dap_request: Option<DapRequest>,
    /// The debugger dashboard snapshot (8f-2), pushed by the binary's DAP host.
    /// `Some` == a session is live == the dashboard is shown; `None` == the
    /// editor is full-width (NORA-IDE-UX section 2.2, auto-collapse).
    debug: Option<DebugView>,
    /// The dashboard focus/tab state, persisting across data refreshes.
    dash: DashState,
    /// Where to put the cursor once a cross-file jump's buffer has loaded.
    /// Applied by `open_buffer`, which is the single point where a newly-read
    /// file becomes the active buffer.
    pending_jump: Option<Pos>,
    /// Visual-mode selection anchor (the other end is the cursor).
    anchor: Option<Pos>,
    /// The internal yank/paste register (replaces the system clipboard).
    register: String,
    /// Multi-cursor selections (Helix `%`/`s`/`,`). Empty == single-cursor mode
    /// (the `TextBuffer` cursor + `anchor`), so single-cursor paths are
    /// untouched; non-empty == multi, with `carets[0]` the primary (synced to
    /// the `TextBuffer` cursor). Transient -- cleared on a buffer switch.
    carets: Vec<Sel>,
    /// The in-progress `s` (select-within) literal pattern; `Some` routes keys
    /// to the prompt and renders a `select:` line.
    split_buf: Option<String>,
    /// A one-key prefix awaiting its next key (`f`/`F`/`t`/`T` target, or `m`).
    pending: Option<Pending>,
    /// The numeric count prefix being typed (`3w`, `5j`); `None` until a digit
    /// starts it. Consumed (reset to `None`) by the next motion / edit.
    count: Option<usize>,
    /// The last find-char motion, repeated by `;` (same) / `,` (reversed).
    last_find: Option<(FindKind, char)>,
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
            diags: crate::diag::Diagnostics::new(),
            hover: None,
            completion: Vec::new(),
            completion_prefix: String::new(),
            lsp_request: None,
            dap_request: None,
            debug: None,
            dash: DashState::new(),
            pending_jump: None,
            anchor: None,
            register: String::new(),
            carets: Vec::new(),
            split_buf: None,
            pending: None,
            count: None,
            last_find: None,
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
            // Completion overlays Insert -- the chip stays INS because that is
            // the mode Enter and Esc both return to.
            Mode::Insert | Mode::Completion { .. } => "INS",
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

    // -- language-server seam (8e-2c) -------------------------------------
    //
    // The editor raises requests and accepts answers; it never speaks a
    // protocol, knows a URI, or blocks. Every answer is optional: a server
    // that says nothing simply leaves the editor as it was.

    /// Take the pending language-server query, if any (the binary's LSP client
    /// polls this like `take_request`).
    pub fn take_lsp_request(&mut self) -> Option<LspRequest> {
        self.lsp_request.take()
    }

    /// Take the pending debugger command, if any (the binary's DAP host polls
    /// this like `take_lsp_request`). `DapRequest::Launch` starts a session; the
    /// rest act on the live one.
    pub fn take_dap_request(&mut self) -> Option<DapRequest> {
        self.dap_request.take()
    }

    /// The debugger dashboard snapshot, or `None` when no session is live (the
    /// dashboard is collapsed and the editor renders full-width).
    pub fn debug_view(&self) -> Option<&DebugView> {
        self.debug.as_ref()
    }

    /// The dashboard focus/tab state the renderer reads.
    pub fn dash(&self) -> &DashState {
        &self.dash
    }

    /// Is a debug session live (the dashboard shown)?
    pub fn debugging(&self) -> bool {
        self.debug.is_some()
    }

    /// Push (or clear) the dashboard snapshot -- the binary's DAP host is the
    /// only caller. Opening a session (the first `Some` after a `None`) resets
    /// the dashboard to focus the source buffer; ending it (a `None`) collapses
    /// back. A data refresh MID-session keeps the current focus + tab, so a stop
    /// never yanks focus off the tile you were reading.
    pub fn set_debug_view(&mut self, view: Option<DebugView>) {
        let was_live = self.debug.is_some();
        if view.is_none() || !was_live {
            self.dash = DashState::new();
        }
        self.debug = view;
    }

    /// Cycle dashboard focus (`Tab`): Editor -> Variables -> CallStack ->
    /// Goroutines -> Console -> Editor. A no-op when not debugging.
    fn cycle_focus(&mut self) {
        if !self.debugging() {
            return;
        }
        self.dash.focus = match self.dash.focus {
            DashPane::Editor => DashPane::Variables,
            DashPane::Variables => DashPane::CallStack,
            DashPane::CallStack => DashPane::Goroutines,
            DashPane::Goroutines => DashPane::Console,
            DashPane::Console => DashPane::Editor,
        };
    }

    /// Handle a key while a dashboard tile (not the editor) holds focus.
    /// Navigation only: `j`/`k` move the selection, `g`/`G` jump to the
    /// ends, `l`/`h` open/shut the Variables group or step the Console tabs,
    /// `Tab` cycles focus, `Esc` returns to the editor. Every other key is
    /// inert, so a stray keystroke over a tile can never edit the buffer.
    fn dash_nav(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Tab => self.cycle_focus(),
            KeyCode::Esc => self.dash.focus = DashPane::Editor,
            KeyCode::Char('j') | KeyCode::Down => self.dash_move(1),
            KeyCode::Char('k') | KeyCode::Up => self.dash_move(-1),
            KeyCode::Char('g') => self.dash_move_top(),
            KeyCode::Char('G') => self.dash_move_bottom(),
            KeyCode::Char('l') | KeyCode::Right => self.dash_expand(true),
            KeyCode::Char('h') | KeyCode::Left => self.dash_expand(false),
            KeyCode::Enter => self.dash_activate(),
            _ => {}
        }
    }

    /// `Enter` on a tile: act on the selected row. Call Stack raises a
    /// `SelectFrame` (the host jumps the editor to the frame + re-scopes the
    /// Variables tile); Goroutines raises a `SelectGoroutine` (the host re-roots
    /// the stack on that thread). The request carries the row index clamped to
    /// the live count, so a stale selection after a data shrink names a live row,
    /// and the host resolves it against its own list. The other tiles carry no
    /// per-row action yet (the per-variable expand arrives with the lazy tree),
    /// so `Enter` is inert there -- never an edit.
    fn dash_activate(&mut self) {
        let rows = self.dash_rows();
        if rows == 0 {
            return;
        }
        let idx = |sel: usize| sel.min(rows - 1);
        match self.dash.focus {
            DashPane::CallStack => {
                self.dap_request = Some(DapRequest::SelectFrame(idx(self.dash.stack_sel)));
            }
            DashPane::Goroutines => {
                self.dap_request = Some(DapRequest::SelectGoroutine(idx(self.dash.gor_sel)));
            }
            DashPane::Variables | DashPane::Console | DashPane::Editor => {}
        }
    }

    /// The number of selectable rows in the focused tile (for clamping the
    /// selection). Variables counts the flattened tree rows -- the `locals`
    /// group node, plus its leaves when the group is expanded; the others are
    /// their flat lists. The Console tail-follows, so it has no row cursor.
    fn dash_rows(&self) -> usize {
        let dv = match &self.debug {
            Some(d) => d,
            None => return 0,
        };
        match self.dash.focus {
            DashPane::Variables => {
                if self.dash.locals_expanded {
                    1 + dv.locals.len()
                } else {
                    1
                }
            }
            DashPane::CallStack => dv.frames.len(),
            DashPane::Goroutines => dv.goroutines.len(),
            DashPane::Console | DashPane::Editor => 0,
        }
    }

    /// A mutable handle on the focused tile's selection field (`None` for the
    /// Console + the Editor, which carry no row cursor).
    fn dash_sel_mut(&mut self) -> Option<&mut usize> {
        match self.dash.focus {
            DashPane::Variables => Some(&mut self.dash.var_sel),
            DashPane::CallStack => Some(&mut self.dash.stack_sel),
            DashPane::Goroutines => Some(&mut self.dash.gor_sel),
            DashPane::Console | DashPane::Editor => None,
        }
    }

    /// Move the focused tile's selection by `delta` rows, clamping a stale
    /// selection to the live row count first (a stop can shrink the data).
    fn dash_move(&mut self, delta: i32) {
        let rows = self.dash_rows();
        if rows == 0 {
            return;
        }
        let max = (rows - 1) as i64;
        if let Some(sel) = self.dash_sel_mut() {
            let cur = (*sel as i64).min(max);
            *sel = (cur + delta as i64).clamp(0, max) as usize;
        }
    }

    fn dash_move_top(&mut self) {
        if let Some(sel) = self.dash_sel_mut() {
            *sel = 0;
        }
    }

    fn dash_move_bottom(&mut self) {
        let rows = self.dash_rows();
        if rows == 0 {
            return;
        }
        let last = rows - 1;
        if let Some(sel) = self.dash_sel_mut() {
            *sel = last;
        }
    }

    /// `l`/`h` (expand / collapse): open or shut the Variables `locals` group,
    /// or step the two Console tabs. Inert on the flat Call Stack / Goroutines
    /// tiles (their rows have no children yet).
    fn dash_expand(&mut self, open: bool) {
        match self.dash.focus {
            DashPane::Variables => {
                self.dash.locals_expanded = open;
                if !open {
                    // Collapsed -> the group node is the only row left.
                    self.dash.var_sel = 0;
                }
            }
            // Two tabs (Program = 0, Debug = 1): l -> Debug, h -> Program.
            DashPane::Console => self.dash.console_tab = usize::from(open),
            _ => {}
        }
    }

    /// The hover text currently displayed, if any.
    pub fn hover(&self) -> Option<&str> {
        self.hover.as_deref()
    }

    /// Show hover text for the cursor's symbol. Empty text is treated as no
    /// answer, so a server replying with a blank string cannot open an empty
    /// box over the user's code.
    pub fn show_hover(&mut self, text: String) {
        if text.trim().is_empty() {
            self.status = Some("no hover information".to_string());
            return;
        }
        self.hover = Some(text);
    }

    /// The completion list and its highlighted index, if the popup is open.
    pub fn completion_state(&self) -> Option<(&[Candidate], usize)> {
        match self.mode {
            Mode::Completion { sel } => Some((self.completion.as_slice(), sel)),
            _ => None,
        }
    }

    /// Open the completion popup over Insert mode.
    ///
    /// An empty list says so and stays in Insert -- an empty popup would be a
    /// mode the user has to escape from to learn nothing. Ignored unless the
    /// editor is still in Insert: the answer is asynchronous, and by the time
    /// it lands the user may have left insert, switched buffers, or opened a
    /// picker. Completing into a mode that did not ask for it would be an edit
    /// the user never initiated.
    pub fn show_completion(&mut self, items: Vec<Candidate>) {
        if self.mode != Mode::Insert {
            return;
        }
        if items.is_empty() {
            self.status = Some("no completions".to_string());
            return;
        }
        self.completion_prefix = self.word_prefix_at_cursor();
        self.completion = items;
        self.mode = Mode::Completion { sel: 0 };
    }

    /// Move the cursor to `(line, col)` -- in `path` when it differs from the
    /// active buffer's file, in the current buffer otherwise (go-to-definition).
    ///
    /// A cross-file jump cannot complete here: the file has to be read first,
    /// which is the binary's job. So it raises `Request::Open` and parks the
    /// position, which `open_buffer` applies once the buffer exists.
    pub fn jump_to(&mut self, path: Option<String>, line: usize, col: usize) {
        let elsewhere = match (&path, &self.filename) {
            (Some(p), Some(cur)) => p != cur,
            (Some(_), None) => true,
            (None, _) => false,
        };
        if elsewhere {
            self.pending_jump = Some((line, col));
            self.request = Some(Request::Open(path.unwrap_or_default()));
            return;
        }
        self.text.set_cursor(line, col);
    }

    /// The word characters immediately before the cursor -- the prefix a
    /// completion replaces.
    fn word_prefix_at_cursor(&self) -> String {
        let (row, col) = self.text.cursor();
        let line = self.text.line(row);
        let chars: Vec<char> = line.chars().take(col).collect();
        let start = chars
            .iter()
            .rposition(|c| !(c.is_alphanumeric() || *c == '_'))
            .map(|i| i + 1)
            .unwrap_or(0);
        chars[start..].iter().collect()
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

    /// The syntax-highlight language for the active buffer, from its filename
    /// (the renderer asks per frame; no stored state to keep in sync).
    pub fn lang(&self) -> Lang {
        Lang::from_filename(self.filename.as_deref())
    }

    /// Whether to show the new-user `:help` nudge in the corner: a pristine,
    /// unnamed, unmodified buffer with no overlay open (it fades the moment you
    /// type, open, or name a file -- the "I'm just exploring" moment only).
    pub fn show_hint(&self) -> bool {
        self.filename.is_none()
            && !self.modified
            && self.text.content().is_empty()
            && self.carets.is_empty()
            && matches!(self.mode, Mode::Normal)
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
        // Multi-cursor + any half-typed prefix are transient; a switch drops them.
        self.carets.clear();
        self.split_buf = None;
        self.pending = None;
        self.count = None;
        // Diagnostics are keyed to the file that WAS showing -- carrying them
        // across a switch would paint another file's errors on these lines.
        // The binary republishes for the new buffer when the server answers.
        self.diags.clear();
        // Same reasoning for the language-server overlays: hover text and a
        // completion list describe a position in the file we just left.
        self.hover = None;
        self.completion.clear();
        self.completion_prefix.clear();
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
        self.split_buf.is_none() && matches!(self.mode, Mode::Normal | Mode::Insert | Mode::Visual)
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
        // A cross-file go-to-definition parked its target here: this is the one
        // point where the requested file has become the active buffer, so the
        // cursor can finally be placed. Taken (not peeked) so an ordinary later
        // `:e` can never inherit a stale jump.
        if let Some((line, col)) = self.pending_jump.take() {
            self.text.set_cursor(line, col);
        }
    }

    /// Open the built-in manual in a read-only scratch buffer (`:help`). Always
    /// ADDS a buffer (never replaces a pristine one) so closing it returns to the
    /// user's work; re-focuses an already-open help buffer instead of stacking.
    pub fn open_help(&mut self) {
        if self.filename.as_deref() == Some(HELP_NAME) {
            return; // already viewing help
        }
        if let Some(i) = self
            .bufs
            .iter()
            .position(|d| d.filename.as_deref() == Some(HELP_NAME))
        {
            self.switch_to(i);
            return;
        }
        let d = DocState::new(Some(HELP_NAME.to_string()), HELP_TEXT, true);
        self.bufs[self.active] = self.save_active();
        self.bufs.push(d.clone());
        self.active = self.bufs.len() - 1;
        self.load_active(d);
        self.mode = Mode::Normal;
        self.status = Some("help -- press q to close".to_string());
    }

    /// Show `content` in a read-only scratch buffer named `name` (e.g.
    /// `*backtrace*`), focusing it. A DAP host uses this for multi-line output
    /// (a call stack) the one-line status cannot hold; the dashboard (8f)
    /// supersedes it with real tiles.
    ///
    /// REFRESHES an existing scratch of the same name in place, so a fresh `:bt`
    /// replaces the stale stack rather than stacking buffers. Otherwise ADDS a
    /// buffer (never overwrites the user's work); `q` / `:bd` returns to it.
    pub fn open_scratch(&mut self, name: &str, content: &str) {
        let fresh = DocState::new(Some(name.to_string()), content, true);
        if let Some(i) = self
            .bufs
            .iter()
            .position(|d| d.filename.as_deref() == Some(name))
        {
            // Already open: replace its content and focus it. Snapshot the
            // current buffer first UNLESS it is the scratch itself (a `:bt`
            // while already viewing `*backtrace*` refreshes in place).
            if self.active != i {
                self.bufs[self.active] = self.save_active();
                self.active = i;
            }
            self.bufs[i] = fresh.clone();
            self.load_active(fresh);
        } else {
            self.bufs[self.active] = self.save_active();
            self.bufs.push(fresh.clone());
            self.active = self.bufs.len() - 1;
            self.load_active(fresh);
        }
        self.mode = Mode::Normal;
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
        // Hover is transient: the next key takes it down AND still does its
        // job. Dismissing without consuming the key is what keeps a popup that
        // arrives unbidden (it lands whenever the server answers) from ever
        // costing the user a keystroke.
        self.hover = None;
        if self.split_buf.is_some() {
            self.split_prompt(key);
            return;
        }
        if let Some(p) = self.pending.take() {
            self.resolve_pending(p, key);
            return;
        }
        match &self.mode {
            Mode::Normal => self.normal(key),
            Mode::Insert => self.insert(key),
            Mode::Visual => self.visual(key),
            Mode::Command(_) => self.command(key),
            Mode::Menu => self.menu(key),
            Mode::BufferPicker { .. } => self.buffer_picker(key),
            Mode::FilePicker { .. } => self.file_picker(key),
            Mode::Completion { .. } => self.completion_key(key),
        }
    }

    // -- per-mode handlers ------------------------------------------------

    fn normal(&mut self, key: KeyEvent) {
        // A focused dashboard tile owns the navigation keys (nothing here edits
        // the buffer). Only reachable while debugging -- with no session the
        // focus is always the editor, so ordinary editing is untouched.
        if self.debugging() && self.dash.focus != DashPane::Editor {
            self.dash_nav(key);
            return;
        }
        // Multi-cursor (after Esc from a multi-insert): `,` collapses to the
        // primary; any other key collapses first (no stuck multi-state), then
        // acts as a single cursor.
        if !self.carets.is_empty() {
            self.collapse_carets();
            if key.code == KeyCode::Char(',') {
                return;
            }
        }
        // Numeric count prefix (3w, 5j, 2d): a digit accumulates; `0` is a digit
        // only mid-count (else it is the move-home motion). The count echoes in
        // the status and is consumed by the next motion / edit.
        if let KeyCode::Char(c) = key.code {
            if c.is_ascii_digit() && (c != '0' || self.count.is_some()) {
                let d = c as usize - '0' as usize;
                let acc = self.count.unwrap_or(0).saturating_mul(10).saturating_add(d);
                let acc = acc.min(COUNT_MAX);
                self.count = Some(acc);
                self.status = Some(itoa(acc));
                return;
            }
        }
        let explicit = self.count.is_some();
        let n = self.count.take().unwrap_or(1);
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
            KeyCode::Char('%') => {
                // Select the whole buffer (Helix `%`).
                self.anchor = Some((0, 0));
                self.text.move_bottom();
                self.text.move_end();
                self.mode = Mode::Visual;
            }
            KeyCode::Char(':') => self.mode = Mode::Command(":".to_string()),
            KeyCode::Char('/') => self.mode = Mode::Command("/".to_string()),
            KeyCode::Char(' ') => self.mode = Mode::Menu,
            // Cycle dashboard focus while debugging; inert (falls through to the
            // no-op) when there is no session, so normal-mode Tab is unchanged.
            KeyCode::Tab if self.debugging() => self.cycle_focus(),
            KeyCode::Char('n') if !self.last_search.is_empty() => {
                self.search(&self.last_search.clone());
            }
            KeyCode::Char('p') if editable => {
                if !self.register.is_empty() {
                    self.text.checkpoint();
                    for _ in 0..n {
                        self.text.insert_str(&self.register);
                    }
                    self.modified = true;
                }
            }

            // Navigation. A count repeats a motion; on g/G it is a go-to-line.
            KeyCode::Char('h') | KeyCode::Left => self.repeat(n, |t| t.move_left()),
            KeyCode::Char('j') | KeyCode::Down => {
                for _ in 0..n {
                    self.move_down();
                }
            }
            KeyCode::Char('k') | KeyCode::Up => {
                for _ in 0..n {
                    self.move_up();
                }
            }
            KeyCode::Char('l') | KeyCode::Right => self.repeat(n, |t| t.move_right()),
            KeyCode::Char('0') | KeyCode::Home => self.text.move_home(),
            KeyCode::Char('$') | KeyCode::End => self.text.move_end(),
            KeyCode::Char('g') if explicit => self.goto_line(n),
            KeyCode::Char('G') if explicit => self.goto_line(n),
            // `g` opens Helix goto-mode (8e-2c): `gg` top, `ge` end, `gd`
            // definition. Bare `g` was top before the language server needed
            // somewhere to live; `gg` is the Helix binding and `G` still goes
            // to the bottom, so the motion is never more than one extra key.
            KeyCode::Char('g') => self.pending = Some(Pending::Goto),
            KeyCode::Char('G') => self.text.move_bottom(),
            // Diagnostic navigation (`]d` / `[d`), Helix/vim bracket motions.
            KeyCode::Char(']') => self.pending = Some(Pending::Bracket { forward: true }),
            KeyCode::Char('[') => self.pending = Some(Pending::Bracket { forward: false }),
            // Hover (vim's `K`): describe the symbol under the cursor.
            KeyCode::Char('K') => self.lsp_request = Some(LspRequest::Hover),
            KeyCode::Char('w') => self.repeat(n, |t| t.move_word_forward()),
            KeyCode::Char('W') => self.repeat(n, |t| t.move_long_word_forward()),
            KeyCode::Char('e') => self.repeat(n, |t| t.move_word_end()),
            KeyCode::Char('b') => self.repeat(n, |t| t.move_word_back()),
            KeyCode::Char('f') => self.pending = Some(Pending::FindChar(FindKind::Forward)),
            KeyCode::Char('F') => self.pending = Some(Pending::FindChar(FindKind::Backward)),
            KeyCode::Char('t') => self.pending = Some(Pending::FindChar(FindKind::ForwardTill)),
            KeyCode::Char('T') => self.pending = Some(Pending::FindChar(FindKind::BackwardTill)),
            KeyCode::Char('m') => self.pending = Some(Pending::Match),
            KeyCode::Char(';') => self.repeat_find(false),
            KeyCode::Char(',') => self.repeat_find(true),
            KeyCode::PageUp => self.page(true),
            KeyCode::PageDown => self.page(false),

            // Editing. A count multiplies under ONE checkpoint (one undo reverts
            // the whole 3x / 2d).
            KeyCode::Char('x') if editable => {
                self.text.checkpoint();
                for _ in 0..n {
                    self.text.delete_char();
                }
                self.modified = true;
            }
            KeyCode::Char('d') if editable => {
                self.text.checkpoint();
                for _ in 0..n {
                    self.text.delete_line();
                }
                self.modified = true;
            }
            KeyCode::Char('u') if editable => {
                if self.text.undo() {
                    self.modified = true;
                } else {
                    self.status = Some("already at oldest change".to_string());
                }
            }

            KeyCode::Char('q') | KeyCode::Esc if self.readonly => {
                // A read-only buffer in a multi-buffer session (e.g. :help opened
                // over your work) closes on q/Esc and returns you to it; a lone
                // view (nora -R) has nothing to return to, so it quits.
                if self.bufs.len() > 1 {
                    self.close_buffer(true);
                } else {
                    self.quit = true;
                }
            }
            _ => {}
        }
    }

    fn insert(&mut self, key: KeyEvent) {
        if !self.carets.is_empty() {
            self.multi_insert(key);
            return;
        }
        // Ctrl-N asks for completions (vim's key; the pickers already use
        // Ctrl-N/Ctrl-P to move a list, so the letter is consistent here).
        // Matched before the Char arm so the literal 'n' still types.
        if key.is_ctrl('n') {
            self.lsp_request = Some(LspRequest::Completion);
            return;
        }
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
                self.carets.clear();
                self.mode = Mode::Normal;
            }
            KeyCode::Char('s') => self.split_buf = Some(String::new()),
            KeyCode::Char('c') if !self.readonly => self.change(),
            KeyCode::Char(',') if !self.carets.is_empty() => self.collapse_carets(),
            KeyCode::Char('y') => {
                if let Some((lo, hi)) = self.selection() {
                    self.register = self.text.range_text(lo, hi);
                }
                self.anchor = None;
                self.mode = Mode::Normal;
                self.status = Some("yanked".to_string());
            }
            KeyCode::Char('d') | KeyCode::Char('x') if !self.readonly => self.delete(),

            // Navigation extends the selection (the anchor stays put).
            KeyCode::Char('h') | KeyCode::Left => self.text.move_left(),
            KeyCode::Char('j') | KeyCode::Down => self.move_down(),
            KeyCode::Char('k') | KeyCode::Up => self.move_up(),
            KeyCode::Char('l') | KeyCode::Right => self.text.move_right(),
            KeyCode::Char('0') | KeyCode::Home => self.text.move_home(),
            KeyCode::Char('$') | KeyCode::End => self.text.move_end(),
            // Same goto prefix as Normal, so `gg`/`ge` mean one thing
            // everywhere; here the motion extends the selection.
            KeyCode::Char('g') => self.pending = Some(Pending::Goto),
            KeyCode::Char('G') => self.text.move_bottom(),
            KeyCode::Char('w') => self.text.move_word_forward(),
            KeyCode::Char('W') => self.text.move_long_word_forward(),
            KeyCode::Char('e') => self.text.move_word_end(),
            KeyCode::Char('b') => self.text.move_word_back(),
            KeyCode::PageUp => self.page(true),
            KeyCode::PageDown => self.page(false),
            _ => {}
        }
    }

    /// The `s` (select-within) pattern prompt: collect the literal pattern;
    /// Enter splits the buffer's matches into carets, Esc cancels.
    fn split_prompt(&mut self, key: KeyEvent) {
        let mut buf = match self.split_buf.take() {
            Some(b) => b,
            None => return,
        };
        match key.code {
            KeyCode::Esc => {}
            KeyCode::Enter => self.run_split(&buf),
            KeyCode::Backspace => {
                buf.pop();
                self.split_buf = Some(buf);
            }
            KeyCode::Char(c) => {
                buf.push(c);
                self.split_buf = Some(buf);
            }
            _ => self.split_buf = Some(buf),
        }
    }

    /// Split every literal match of `pat` into its own selection (Helix `s`).
    fn run_split(&mut self, pat: &str) {
        if pat.is_empty() {
            self.status = Some("empty pattern".to_string());
            return;
        }
        let matches = self.text.find_all(pat);
        if matches.is_empty() {
            self.status = Some("no matches".to_string());
            return;
        }
        self.carets = matches
            .iter()
            .map(|&(anchor, head)| Sel { anchor, head })
            .collect();
        // The primary caret drives scrolling + the block cursor.
        let head = self.carets[0].head;
        self.text.set_cursor(head.0, head.1);
        self.anchor = None;
        self.mode = Mode::Visual;
        self.status = Some(alloc::format!("{} cursors", self.carets.len()));
    }

    /// Collapse multi-cursor to the primary selection's caret (Helix `,`).
    fn collapse_carets(&mut self) {
        if let Some(primary) = self.carets.first().copied() {
            self.text.set_cursor(primary.head.0, primary.head.1);
        }
        self.carets.clear();
        self.split_buf = None;
        self.anchor = None;
        self.mode = Mode::Normal;
        self.status = Some("single cursor".to_string());
    }

    /// The multi-cursor selections (empty in single-cursor mode); the renderer
    /// paints each head as a caret.
    pub fn carets(&self) -> &[Sel] {
        &self.carets
    }

    /// Every selection range to paint: the carets' ranges in multi-cursor mode,
    /// else the single visual selection (or none).
    pub fn selection_ranges(&self) -> Vec<(Pos, Pos)> {
        if !self.carets.is_empty() {
            self.carets.iter().map(|s| s.range()).collect()
        } else if let Some(s) = self.selection() {
            alloc::vec![s]
        } else {
            Vec::new()
        }
    }

    /// The in-progress `s` pattern (for the `select:` prompt line), if any.
    pub fn split_buf(&self) -> Option<&str> {
        self.split_buf.as_deref()
    }

    /// Change (Helix `c`): delete every selection and drop into Insert. Multi
    /// (carets non-empty) changes at every caret; otherwise the single visual
    /// selection. The deletes run ascending with the later carets shifted back,
    /// so positions stay valid (match spans are single-line).
    fn change(&mut self) {
        if self.readonly {
            return;
        }
        if self.carets.is_empty() {
            if let Some((lo, hi)) = self.selection() {
                self.text.checkpoint();
                self.text.delete_range(lo, hi);
                self.anchor = None;
                self.mode = Mode::Insert;
                self.modified = true;
            }
            return;
        }
        self.delete_all_carets();
        self.mode = Mode::Insert;
    }

    /// Delete every caret's selection range, collapsing each caret to its start
    /// (ascending, shifting the later carets back). Shared by `c` (change ->
    /// Insert) and `d` (delete -> stay in Normal). Match spans are single-line.
    fn delete_all_carets(&mut self) {
        self.text.checkpoint();
        let n = self.carets.len();
        for i in 0..n {
            let (lo, hi) = self.carets[i].range();
            self.text.delete_range(lo, hi);
            self.carets[i].anchor = lo;
            self.carets[i].head = lo;
            for j in (i + 1)..n {
                // Shift anchor + head independently -- collapsing both to the
                // head would destroy a multi-char selection's range.
                self.carets[j].head = shift_after_delete(self.carets[j].head, lo, hi);
                self.carets[j].anchor = shift_after_delete(self.carets[j].anchor, lo, hi);
            }
        }
        let h = self.carets[0].head;
        self.text.set_cursor(h.0, h.1);
        self.anchor = None;
        self.modified = true;
    }

    /// Delete (Helix `d`): remove every selection. Multi (carets) deletes at
    /// every caret and stays in multi-Normal (carets collapse to the deletion
    /// points, ready for `,`); a single visual selection cuts (yank + delete).
    fn delete(&mut self) {
        if self.readonly {
            return;
        }
        if self.carets.is_empty() {
            if let Some((lo, hi)) = self.selection() {
                self.register = self.text.range_text(lo, hi);
                self.text.checkpoint();
                self.text.delete_range(lo, hi);
                self.modified = true;
            }
            self.anchor = None;
            self.mode = Mode::Normal;
            return;
        }
        let count = self.carets.len();
        self.delete_all_carets();
        self.mode = Mode::Normal;
        self.status = Some(alloc::format!("deleted {} selections", count));
    }

    /// Apply one edit at every caret simultaneously: edit at the caret, then
    /// shift the later carets by its delta so they stay valid (ascending).
    fn multi_apply(&mut self, edit: MultiEdit) {
        let n = self.carets.len();
        for i in 0..n {
            let at = self.carets[i].head;
            self.text.set_cursor(at.0, at.1);
            match edit {
                MultiEdit::Char(c) => self.text.insert_char(c),
                MultiEdit::Newline => self.text.insert_newline(),
                MultiEdit::Backspace => self.text.backspace(),
            }
            let np = self.text.cursor();
            self.carets[i].head = np;
            self.carets[i].anchor = np;
            for j in (i + 1)..n {
                let s = shift_after(self.carets[j].head, at, edit);
                self.carets[j].head = s;
                self.carets[j].anchor = s;
            }
        }
        // The primary caret drives scrolling + the block cursor.
        let h = self.carets[0].head;
        self.text.set_cursor(h.0, h.1);
        self.modified = true;
    }

    /// Insert-mode keys with carets active: edit at every caret. Esc commits
    /// (carets preserved -> multi-Normal); in-insert movement is a v1.x add.
    fn multi_insert(&mut self, key: KeyEvent) {
        match key.code {
            KeyCode::Esc => self.mode = Mode::Normal,
            KeyCode::Enter => self.multi_apply(MultiEdit::Newline),
            KeyCode::Backspace => self.multi_apply(MultiEdit::Backspace),
            KeyCode::Tab => {
                for _ in 0..4 {
                    self.multi_apply(MultiEdit::Char(' '));
                }
            }
            KeyCode::Char(c) => self.multi_apply(MultiEdit::Char(c)),
            _ => {}
        }
    }

    /// Resolve a one-key prefix with its follow key.
    fn resolve_pending(&mut self, p: Pending, key: KeyEvent) {
        match p {
            Pending::FindChar(kind) => {
                if let KeyCode::Char(c) = key.code {
                    self.last_find = Some((kind, c));
                    self.find_char(kind, c);
                }
            }
            // `m` opens match-mode: `m` matches the bracket, `i`/`a` begin a text
            // object (then the object char selects it).
            Pending::Match => match key.code {
                KeyCode::Char('m') => self.match_bracket(),
                KeyCode::Char('i') => self.pending = Some(Pending::TextObject { around: false }),
                KeyCode::Char('a') => self.pending = Some(Pending::TextObject { around: true }),
                _ => {}
            },
            Pending::TextObject { around } => {
                if let KeyCode::Char(c) = key.code {
                    self.select_text_object(around, c);
                }
            }
            // Helix goto-mode. An unknown second key cancels silently -- a
            // prefix that swallowed the key and did something else would be
            // worse than one that does nothing.
            Pending::Goto => match key.code {
                KeyCode::Char('g') => self.text.move_top(),
                KeyCode::Char('e') => self.text.move_bottom(),
                KeyCode::Char('d') => self.lsp_request = Some(LspRequest::Definition),
                _ => {}
            },
            // Only `d` (diagnostics) at 8e-2c; `]f`/`]c` and friends are the
            // obvious later additions, which is why this stays a prefix.
            Pending::Bracket { forward } => {
                if key.code == KeyCode::Char('d') {
                    self.goto_diagnostic(forward);
                }
            }
        }
    }

    /// Move to the next/previous diagnostic, wrapping, and echo its message.
    ///
    /// Pure editor state -- no server round-trip. The published set is already
    /// here, so this answers instantly even while gopls is busy or gone.
    fn goto_diagnostic(&mut self, forward: bool) {
        let (row, _) = self.text.cursor();
        let found = if forward {
            self.diags.next_after(row)
        } else {
            self.diags.prev_before(row)
        };
        let (line, col, msg) = match found {
            Some(d) => (d.line, d.col, d.message.clone()),
            None => {
                self.status = Some("no diagnostics".to_string());
                return;
            }
        };
        // `set_cursor` clamps both coordinates, so a diagnostic the server
        // published against a version we have since edited past lands
        // harmlessly at the nearest valid position rather than out of bounds.
        self.text.set_cursor(line, col);
        self.status = Some(msg);
    }

    /// Repeat the last find-char motion (`;` same direction, `,` reversed).
    fn repeat_find(&mut self, reverse: bool) {
        if let Some((k, c)) = self.last_find {
            let kind = if reverse { k.reversed() } else { k };
            self.find_char(kind, c);
        }
    }

    /// Move the cursor to a `<char>` on the current line per the find kind. A
    /// `t` repeat may not advance past an immediately-adjacent target at v1
    /// (vim's `;`-after-`t` skip is a v1.x refinement).
    fn find_char(&mut self, kind: FindKind, ch: char) {
        let (row, col) = self.text.cursor();
        let line: Vec<char> = self.text.line(row).chars().collect();
        let n = line.len();
        let forward = matches!(kind, FindKind::Forward | FindKind::ForwardTill);
        let till = matches!(kind, FindKind::ForwardTill | FindKind::BackwardTill);
        let found = if forward {
            (col + 1..n).find(|&i| line[i] == ch)
        } else {
            (0..col).rev().find(|&i| line[i] == ch)
        };
        if let Some(i) = found {
            let dest = if till {
                if forward {
                    i - 1
                } else {
                    i + 1
                }
            } else {
                i
            };
            self.text.set_cursor(row, dest);
        }
    }

    /// Jump to the bracket matching the one under the cursor (Helix `mm`),
    /// nesting-aware and across lines.
    fn match_bracket(&mut self) {
        let (row, col) = self.text.cursor();
        let here = match self.text.char_at(row, col) {
            Some(c) => c,
            None => return,
        };
        let dest = match here {
            '(' => self.scan_forward_match('(', ')', (row, col)),
            ')' => self.scan_backward_match('(', ')', (row, col)),
            '[' => self.scan_forward_match('[', ']', (row, col)),
            ']' => self.scan_backward_match('[', ']', (row, col)),
            '{' => self.scan_forward_match('{', '}', (row, col)),
            '}' => self.scan_backward_match('{', '}', (row, col)),
            _ => None,
        };
        if let Some(p) = dest {
            self.text.set_cursor(p.0, p.1);
        }
    }

    /// From an OPEN bracket at `from`, the matching close (nesting-aware, across
    /// lines), or `None` if unbalanced.
    fn scan_forward_match(&self, open: char, close: char, from: Pos) -> Option<Pos> {
        let mut depth = 0i32;
        for r in from.0..self.text.line_count() {
            let line: Vec<char> = self.text.line(r).chars().collect();
            let start = if r == from.0 { from.1 } else { 0 };
            for (i, &c) in line.iter().enumerate().skip(start) {
                if c == open {
                    depth += 1;
                } else if c == close {
                    depth -= 1;
                    if depth == 0 {
                        return Some((r, i));
                    }
                }
            }
        }
        None
    }

    /// From a CLOSE bracket at `from`, the matching open.
    fn scan_backward_match(&self, open: char, close: char, from: Pos) -> Option<Pos> {
        let mut depth = 0i32;
        for r in (0..=from.0).rev() {
            let line: Vec<char> = self.text.line(r).chars().collect();
            let upto = if r == from.0 { from.1 + 1 } else { line.len() };
            for i in (0..upto).rev() {
                if line[i] == close {
                    depth += 1;
                } else if line[i] == open {
                    depth -= 1;
                    if depth == 0 {
                        return Some((r, i));
                    }
                }
            }
        }
        None
    }

    /// The nearest UNMATCHED open before `from` -- the enclosing pair's open when
    /// the cursor sits strictly inside it. Walks char by char across lines.
    fn scan_back_enclosing_open(&self, open: char, close: char, from: Pos) -> Option<Pos> {
        let mut depth = 0i32;
        let mut p = self.text.pos_before(from)?;
        loop {
            if let Some(c) = self.text.char_at(p.0, p.1) {
                if c == close {
                    depth += 1;
                } else if c == open {
                    if depth == 0 {
                        return Some(p);
                    }
                    depth -= 1;
                }
            }
            p = self.text.pos_before(p)?;
        }
    }

    /// The bracket pair enclosing (or under) the cursor: on the open -> its
    /// close; on the close -> its open; otherwise the nearest enclosing pair.
    fn find_enclosing_pair(&self, open: char, close: char) -> Option<(Pos, Pos)> {
        let cur = self.text.cursor();
        match self.text.char_at(cur.0, cur.1) {
            Some(c) if c == open => Some((cur, self.scan_forward_match(open, close, cur)?)),
            Some(c) if c == close => Some((self.scan_backward_match(open, close, cur)?, cur)),
            _ => {
                let op = self.scan_back_enclosing_open(open, close, cur)?;
                Some((op, self.scan_forward_match(open, close, op)?))
            }
        }
    }

    /// Select a text object (`mi<o>` / `ma<o>`): the range of object `obj`,
    /// `around` including its delimiters. Found -> enter Visual over it.
    fn select_text_object(&mut self, around: bool, obj: char) {
        let range = match obj {
            '(' | ')' | 'b' => self.pair_object('(', ')', around),
            '[' | ']' => self.pair_object('[', ']', around),
            '{' | '}' | 'B' => self.pair_object('{', '}', around),
            '"' => self.quote_object('"', around),
            '\'' => self.quote_object('\'', around),
            'w' => self.word_object(around),
            _ => None,
        };
        if let Some((lo, hi)) = range {
            self.anchor = Some(lo);
            self.text.set_cursor(hi.0, hi.1);
            self.mode = Mode::Visual;
        }
    }

    /// A bracket-pair object. `around` selects the brackets too; inside selects
    /// between them (`None` for an empty pair, nothing to act on).
    fn pair_object(&self, open: char, close: char, around: bool) -> Option<(Pos, Pos)> {
        let (op, cp) = self.find_enclosing_pair(open, close)?;
        if around {
            return Some((op, cp));
        }
        let lo = self.text.pos_after(op)?;
        let hi = self.text.pos_before(cp)?;
        if (lo.0, lo.1) <= (hi.0, hi.1) {
            Some((lo, hi))
        } else {
            None
        }
    }

    /// A quote object on the cursor's line (quotes do not nest). Picks the pair
    /// ending at or after the cursor (inside it, or the next string on the line).
    fn quote_object(&self, q: char, around: bool) -> Option<(Pos, Pos)> {
        let (row, col) = self.text.cursor();
        let line: Vec<char> = self.text.line(row).chars().collect();
        let quotes: Vec<usize> = line
            .iter()
            .enumerate()
            .filter(|(_, &c)| c == q)
            .map(|(i, _)| i)
            .collect();
        let mut i = 0;
        while i + 1 < quotes.len() {
            let (a, b) = (quotes[i], quotes[i + 1]);
            if col <= b {
                return if around {
                    Some(((row, a), (row, b)))
                } else if b > a + 1 {
                    Some(((row, a + 1), (row, b - 1)))
                } else {
                    None // empty "" -> nothing inside
                };
            }
            i += 2;
        }
        None
    }

    /// A word object: inside (`iw`) is the same-class run; around (`aw`) adds the
    /// trailing whitespace, or the leading whitespace when there is no trailing.
    fn word_object(&self, around: bool) -> Option<(Pos, Pos)> {
        let cur = self.text.cursor();
        let (mut lo, mut hi) = self.text.word_span_at(cur)?;
        if around {
            let len = self.text.char_len(cur.0);
            let mut end = hi.1;
            while end + 1 < len && self.text.char_at(cur.0, end + 1).is_some_and(char_ws) {
                end += 1;
            }
            if end > hi.1 {
                hi = (hi.0, end);
            } else {
                let mut start = lo.1;
                while start > 0 && self.text.char_at(cur.0, start - 1).is_some_and(char_ws) {
                    start -= 1;
                }
                lo = (lo.0, start);
            }
        }
        Some((lo, hi))
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

    /// The completion popup's keys. Every exit returns to Insert -- the user
    /// asked for a completion mid-word and expects to keep typing either way.
    fn completion_key(&mut self, key: KeyEvent) {
        let sel = match self.mode {
            Mode::Completion { sel } => sel,
            _ => return,
        };
        let len = self.completion.len();
        // The mode and the list must agree. `show_completion` refuses an empty
        // list, so they always do today -- but the wrap arithmetic below is a
        // `% len`, and a future path that clears the candidates without
        // leaving the mode (a buffer switch, say) would turn a keypress into a
        // divide-by-zero. Fail closed into Insert instead of trusting that no
        // such path is ever added.
        if len == 0 {
            self.close_completion();
            return;
        }
        // Ctrl-N/Ctrl-P move the list, matching the file picker (and vim's
        // completion), checked before the printable arm so they never type.
        if key.is_ctrl('n') {
            self.mode = Mode::Completion { sel: (sel + 1) % len };
            return;
        }
        if key.is_ctrl('p') {
            self.mode = Mode::Completion {
                sel: if sel == 0 { len - 1 } else { sel - 1 },
            };
            return;
        }
        match key.code {
            KeyCode::Esc => self.close_completion(),
            KeyCode::Down => self.mode = Mode::Completion { sel: (sel + 1) % len },
            KeyCode::Up => {
                self.mode = Mode::Completion {
                    sel: if sel == 0 { len - 1 } else { sel - 1 },
                }
            }
            KeyCode::Enter | KeyCode::Tab => self.accept_completion(sel),
            // Any other key closes the popup and is handled as normal typing.
            // The list was computed for the position the cursor had when it was
            // requested; once the text moves, the candidates are stale, and
            // filtering a stale list would show matches the server never
            // offered for what is now under the cursor. Re-requesting as you
            // type is the v1.x refinement (NORA-IDE-UX section 4).
            _ => {
                self.close_completion();
                self.insert(key);
            }
        }
    }

    /// Dismiss the popup, back to Insert, candidates dropped.
    fn close_completion(&mut self) {
        self.completion.clear();
        self.completion_prefix.clear();
        self.mode = Mode::Insert;
    }

    /// Replace the typed prefix with candidate `sel` and resume typing.
    fn accept_completion(&mut self, sel: usize) {
        let insert = match self.completion.get(sel) {
            Some(c) => c.insert.clone(),
            None => {
                self.close_completion();
                return;
            }
        };
        let prefix_len = self.completion_prefix.chars().count();
        self.close_completion();
        if self.readonly {
            self.status = Some("read-only".to_string());
            return;
        }
        // One checkpoint so a single undo reverts the whole completion --
        // prefix removal and insertion together, never half of it.
        self.text.checkpoint();
        for _ in 0..prefix_len {
            self.text.backspace();
        }
        self.text.insert_str(&insert);
        self.modified = true;
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
            "help" => self.open_help(),
            // -- debugger (8e-3e): the verbs relay to the binary's DAP host --
            "debug" if arg.is_empty() => {
                self.status = Some(":debug needs a program (e.g. :debug /goroot/bin/prog)".to_string())
            }
            "debug" => self.dap_request = Some(DapRequest::Launch(arg.to_string())),
            "break" | "br" if arg.is_empty() => {
                self.status = Some(":break needs a function or file:line".to_string())
            }
            "break" | "br" => self.dap_request = Some(DapRequest::Break(arg.to_string())),
            "cont" | "c" => self.dap_request = Some(DapRequest::Continue),
            "next" | "n" => self.dap_request = Some(DapRequest::Next),
            "step" | "s" => self.dap_request = Some(DapRequest::Step),
            "stepout" | "so" => self.dap_request = Some(DapRequest::StepOut),
            "bt" | "stack" => self.dap_request = Some(DapRequest::Backtrace),
            "print" | "p" if arg.is_empty() => {
                self.status = Some(":print needs an expression".to_string())
            }
            "print" | "p" => self.dap_request = Some(DapRequest::Print(arg.to_string())),
            "kill" | "stop" => self.dap_request = Some(DapRequest::Kill),
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

    /// Run a `TextBuffer` motion `n` times -- the count-prefix repeater for the
    /// buffer-level motions (wrap-aware j/k loop on `self` directly instead).
    fn repeat(&mut self, n: usize, f: impl Fn(&mut TextBuffer)) {
        for _ in 0..n {
            f(&mut self.text);
        }
    }

    /// Jump to 1-based line `n` (clamped), column 0 -- the `{count}G` / `{count}g`
    /// go-to-line form.
    fn goto_line(&mut self, n: usize) {
        self.text.set_cursor(n.saturating_sub(1), 0);
    }
}

/// Lines moved per PageUp/PageDown (a fixed step; viewport-relative paging is a
/// nicety deferred -- the editor does not know the viewport height).
const PAGE: usize = 20;

/// Upper bound on a typed count prefix -- a guard against an absurd `9999999w`
/// spinning the motion loop (motions clamp at the buffer edge, but the loop
/// still runs; this caps it).
const COUNT_MAX: usize = 100_000;

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
    ("help", "open the manual in a new buffer"),
    ("debug", "start debugging a program (:debug <prog>)"),
    ("break", "set a breakpoint (:break <func>)"),
    ("cont", "continue the debuggee (:c)"),
    ("next", "step over one line (:n)"),
    ("step", "step into a call (:s)"),
    ("stepout", "step out of the function (:so)"),
    ("bt", "show the call stack"),
    ("print", "evaluate an expression (:p <expr>)"),
    ("kill", "end the debug session"),
];

/// The scratch-buffer name of the built-in manual (`:help`); also the sentinel
/// `open_help` checks to avoid stacking duplicates.
const HELP_NAME: &str = "*help*";

/// The built-in manual shown by `:help`. Kept accurate to the bindings in
/// `normal`/`insert`/`visual`, the `:` commands, and the `[Space]` menu -- a
/// drift here misleads a new user, so update it alongside any binding change.
/// Lines stay within ~70 columns so an 80-wide console reads them without wrap.
const HELP_TEXT: &str = r#"                       Nora -- Quick Reference

Nora is a MODAL editor: a key does different things in each mode. Press
Esc to return to NORMAL at any time. Close this help with q (or :bd, or
Space then c) to return to your work.

  MODES
    NORMAL   navigate + run operators (the resting mode)
    INSERT   type text                   enter from NORMAL: i a o A
    VISUAL   select a range              enter from NORMAL: v  or  %
    COMMAND  the : command or / search line  enter: :  or  /
    VIEW     a read-only buffer (this one)

  NORMAL -- moving
    1-9 <key>     a count repeats the next motion / edit (5j, 3w, 2d, 3x);
                    {n}G or {n}g jumps to line n
    h j k l       left / down / up / right (or the arrow keys)
    0  $          start / end of line (or Home / End)
    g g   g e     top / bottom of the buffer  ( G also goes to the bottom )
    w  b  e       next-word / prev-word / word-end (punctuation-aware)
    W             next WORD (whitespace-delimited; spans punctuation)
    f<c> F<c>     jump onto the next / previous <c> on the line
    t<c> T<c>     jump just before / after the next / prev <c>
    ;  ,          repeat the last f/F/t/T -- same / reversed
    m m           jump to the matching bracket  ( ) [ ] { }
    mi<o> ma<o>   select inside / around an object <o>:
                    ( ) [ ] { } " '  or  w (the word)
    PageUp/Down   scroll a page

  NORMAL -- editing
    i  a          insert before / after the cursor
    o             open a new line below and insert
    A             append at the end of the line
    x             delete the character under the cursor
    d             delete the current line
    p             paste the yank register
    u             undo
    v             start a visual selection
    %             select the whole buffer
    /  then  n    search forward, then repeat
    :             run a command (see below)
    Space         open the which-key menu (see below)

  INSERT
    type to insert text        Esc        return to NORMAL
    Enter new line             Backspace  delete back
    Delete delete forward      Tab        four spaces
    arrows / Home / End        move without leaving INSERT

  VISUAL -- the selection extends as you move (h j k l w b g G 0 $)
    y    yank (copy) the selection        d / x  delete (cut) it
    c    change: delete it and drop to INSERT
    s    select within: type a word, Enter puts a cursor per match
    Esc  clear the selection

  MULTI-CURSOR (Helix-style) -- edit many places at once
    %               select the whole buffer
    s <word> Enter  put a cursor on every match of <word>
    c               delete the matches and drop to INSERT
    ...type...      your edit lands at every cursor at once
    Esc             commit (the cursors stay, ready for more)
    d               instead of c: just delete every match
    ,               collapse back to a single cursor

  COMMAND  (press :, type the command, Enter to run)
    :w [name]   write the buffer (:w name = save-as)
    :wq         write and quit         :q   quit (warns if unsaved)
    :q!         quit, discarding unsaved changes
    :e <name>   open a file in a new buffer    :e! discard + open
    :bn  :bp    next / previous buffer
    :bd  :bd!   close the buffer ( ! discards unsaved changes )
    :help       open this manual

  THE Space MENU  (press Space, then one key)
    f  open file picker        b  open buffer picker
    n  next buffer             p  previous buffer
    c  close buffer            w  toggle soft-wrap
    s  save file               q  quit

  LANGUAGE SERVER  (Go buffers, when the toolchain is installed)
    Errors and warnings appear as you edit: the line number is tinted,
    and the message for the cursor's line shows on the status bar. The
    right of the status bar counts them ( 2E 1W = 2 errors, 1 warning ).
    ] d   [ d     jump to the next / previous diagnostic (wraps)
    K             describe the symbol under the cursor -- any key closes
    g d           go to the definition (opens another file if it is there)
    Ctrl-N        in INSERT: offer completions
                    Ctrl-N / Ctrl-P or the arrows  move the selection
                    Enter or Tab   accept       Esc   cancel
                    keep typing    dismiss it and carry on
    Nothing here needs the server: without one, the editor is unchanged.

  DEBUGGER  (Go programs, when Ambush is installed)
    :debug <prog>   start debugging a compiled binary; it stops at entry
    :break <func>   set a breakpoint by function name (main.parkLoop)
    :cont   :c      continue until the next breakpoint or exit
    :next   :n      step over one source line
    :step   :s      step into a call     :stepout  :so   step back out
    :bt             show the call stack (in a scratch buffer)
    :print <expr>   evaluate an expression at the stopped frame  (:p)
    :kill           end the session
    The status bar reports where the program stopped and each result.

That's everything. Happy editing!
"#;

/// Whitespace test as a free fn (for `Option::is_some_and` in `word_object`).
fn char_ws(c: char) -> bool {
    c.is_whitespace()
}

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
        ed.handle_key(ch('g')); // goto prefix
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

    /// Run a `:` command line and return whatever debug request it raised.
    fn run_dap(ed: &mut Editor, line: &str) -> Option<DapRequest> {
        type_str(ed, line);
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.mode, Mode::Normal);
        ed.take_dap_request()
    }

    #[test]
    fn command_debug_raises_launch() {
        let mut ed = Editor::new(None, "", false);
        assert_eq!(
            run_dap(&mut ed, ":debug /goroot/bin/prog"),
            Some(DapRequest::Launch("/goroot/bin/prog".to_string()))
        );
    }

    #[test]
    fn command_break_raises_break_by_alias() {
        let mut ed = Editor::new(None, "", false);
        assert_eq!(
            run_dap(&mut ed, ":break main.parkLoop"),
            Some(DapRequest::Break("main.parkLoop".to_string()))
        );
        assert_eq!(
            run_dap(&mut ed, ":br main.other"),
            Some(DapRequest::Break("main.other".to_string()))
        );
    }

    #[test]
    fn command_print_raises_print() {
        let mut ed = Editor::new(None, "", false);
        assert_eq!(
            run_dap(&mut ed, ":print main.Sentinel"),
            Some(DapRequest::Print("main.Sentinel".to_string()))
        );
        assert_eq!(
            run_dap(&mut ed, ":p x + 1"),
            Some(DapRequest::Print("x + 1".to_string()))
        );
    }

    #[test]
    fn command_debug_control_verbs_and_aliases() {
        let mut ed = Editor::new(None, "", false);
        for (line, want) in [
            (":cont", DapRequest::Continue),
            (":c", DapRequest::Continue),
            (":next", DapRequest::Next),
            (":n", DapRequest::Next),
            (":step", DapRequest::Step),
            (":s", DapRequest::Step),
            (":stepout", DapRequest::StepOut),
            (":so", DapRequest::StepOut),
            (":bt", DapRequest::Backtrace),
            (":stack", DapRequest::Backtrace),
            (":kill", DapRequest::Kill),
            (":stop", DapRequest::Kill),
        ] {
            assert_eq!(run_dap(&mut ed, line), Some(want), "verb {line}");
        }
    }

    #[test]
    fn command_debug_empty_args_are_guarded() {
        let mut ed = Editor::new(None, "", false);
        // Each argument-taking verb reports rather than raising a request.
        for line in [":debug", ":break", ":print"] {
            assert_eq!(run_dap(&mut ed, line), None, "{line} must not raise");
            assert!(ed.status.is_some(), "{line} must report");
            ed.status = None;
        }
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
        ed.handle_key(ch('g')); // gg -> back to top
        ed.handle_key(ch('g'));
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

    // -- multi-cursor (Helix %, s, ,) -------------------------------------

    #[test]
    fn percent_selects_whole_buffer() {
        let mut ed = Editor::new(None, "ab\ncd", false);
        ed.handle_key(ch('%'));
        assert_eq!(ed.mode, Mode::Visual);
        assert_eq!(ed.selection(), Some(((0, 0), (1, 2)))); // (0,0)..(last row, end)
    }

    #[test]
    fn s_splits_matches_into_carets() {
        let mut ed = Editor::new(None, "foo foo foo", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        type_str(&mut ed, "foo");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.carets().len(), 3);
        assert_eq!(ed.selection_ranges().len(), 3);
        assert_eq!(ed.mode, Mode::Visual);
        assert_eq!(ed.text.cursor(), (0, 2)); // primary head = first match's last char
    }

    #[test]
    fn s_no_match_keeps_single_cursor() {
        let mut ed = Editor::new(None, "abc", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        type_str(&mut ed, "zzz");
        ed.handle_key(code(KeyCode::Enter));
        assert!(ed.carets().is_empty());
        assert_eq!(ed.status.as_deref(), Some("no matches"));
    }

    #[test]
    fn comma_collapses_to_primary() {
        let mut ed = Editor::new(None, "a a a", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        type_str(&mut ed, "a");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.carets().len(), 3);
        ed.handle_key(ch(','));
        assert!(ed.carets().is_empty());
        assert_eq!(ed.mode, Mode::Normal);
        assert_eq!(ed.text.cursor(), (0, 0)); // primary head
    }

    #[test]
    fn split_prompt_collects_then_escape_cancels() {
        let mut ed = Editor::new(None, "foo", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        ed.handle_key(ch('f'));
        ed.handle_key(ch('o'));
        assert_eq!(ed.split_buf(), Some("fo"));
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.split_buf(), None); // cancelled, no carets
        assert!(ed.carets().is_empty());
    }

    #[test]
    fn change_then_multi_insert_edits_all_carets() {
        // The full flow: foo foo foo -> %, s "foo" Enter -> c -> "bar" -> Esc -> ,
        let mut ed = Editor::new(None, "foo foo foo", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        type_str(&mut ed, "foo");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.carets().len(), 3);
        ed.handle_key(ch('c'));
        assert_eq!(ed.mode, Mode::Insert);
        assert_eq!(ed.text.content(), "  "); // the three "foo" removed
        type_str(&mut ed, "bar");
        assert_eq!(ed.text.content(), "bar bar bar"); // inserted at every caret
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.mode, Mode::Normal);
        ed.handle_key(ch(','));
        assert!(ed.carets().is_empty());
        assert_eq!(ed.text.content(), "bar bar bar");
    }

    #[test]
    fn multi_insert_backspace_at_every_caret() {
        let mut ed = Editor::new(None, "a a a", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        type_str(&mut ed, "a");
        ed.handle_key(code(KeyCode::Enter));
        ed.handle_key(ch('c'));
        type_str(&mut ed, "xy");
        assert_eq!(ed.text.content(), "xy xy xy");
        ed.handle_key(code(KeyCode::Backspace));
        assert_eq!(ed.text.content(), "x x x"); // a backspace at each caret
    }

    #[test]
    fn change_on_a_single_selection() {
        // `c` with a single visual selection (no carets) deletes it -> Insert.
        let mut ed = Editor::new(None, "hello", false);
        ed.handle_key(ch('v'));
        ed.handle_key(ch('l')); // select "he" (0,0)..(0,1)
        ed.handle_key(ch('c'));
        assert_eq!(ed.mode, Mode::Insert);
        assert_eq!(ed.text.content(), "llo");
        ed.handle_key(ch('X'));
        assert_eq!(ed.text.content(), "Xllo");
    }

    #[test]
    fn stray_key_collapses_multi_normal() {
        let mut ed = Editor::new(None, "a a a", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        type_str(&mut ed, "a");
        ed.handle_key(code(KeyCode::Enter));
        ed.handle_key(ch('c'));
        type_str(&mut ed, "z");
        ed.handle_key(code(KeyCode::Esc)); // multi-Normal, 3 carets
        assert_eq!(ed.carets().len(), 3);
        ed.handle_key(ch('l')); // a stray key collapses to the primary, then acts
        assert!(ed.carets().is_empty());
        assert_eq!(ed.mode, Mode::Normal);
    }

    #[test]
    fn d_deletes_all_selections() {
        let mut ed = Editor::new(None, "foo bar foo", false);
        ed.handle_key(ch('%'));
        ed.handle_key(ch('s'));
        type_str(&mut ed, "foo");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.carets().len(), 2);
        ed.handle_key(ch('d')); // delete every selection, stay in multi-Normal
        assert_eq!(ed.text.content(), " bar ");
        assert_eq!(ed.mode, Mode::Normal);
        assert_eq!(ed.carets().len(), 2); // carets preserved at the deletion points
        ed.handle_key(ch(',')); // collapse to single
        assert!(ed.carets().is_empty());
    }

    // -- find-char + match-bracket ---------------------------------------

    #[test]
    fn find_char_forward_then_repeat() {
        let mut ed = Editor::new(None, "abcabc", false);
        ed.handle_key(ch('f'));
        ed.handle_key(ch('c'));
        assert_eq!(ed.text.cursor(), (0, 2));
        ed.handle_key(ch(';')); // repeat forward
        assert_eq!(ed.text.cursor(), (0, 5));
        ed.handle_key(ch(',')); // repeat reversed
        assert_eq!(ed.text.cursor(), (0, 2));
    }

    #[test]
    fn find_till_and_backward() {
        let mut ed = Editor::new(None, "a.b.c", false);
        ed.handle_key(ch('t'));
        ed.handle_key(ch('.')); // just before the first '.' (col 1) -> col 0
        assert_eq!(ed.text.cursor(), (0, 0));
        ed.text.move_end();
        ed.handle_key(ch('F'));
        ed.handle_key(ch('a')); // back onto 'a' at col 0
        assert_eq!(ed.text.cursor(), (0, 0));
    }

    #[test]
    fn match_bracket_jumps_both_ways() {
        let mut ed = Editor::new(None, "(a (b) c)", false);
        ed.handle_key(ch('m'));
        ed.handle_key(ch('m'));
        assert_eq!(ed.text.cursor(), (0, 8)); // '(' at 0 -> matching ')' at 8
        ed.handle_key(ch('m'));
        ed.handle_key(ch('m'));
        assert_eq!(ed.text.cursor(), (0, 0)); // ')' at 8 -> '(' at 0
    }

    #[test]
    fn match_bracket_across_lines() {
        let mut ed = Editor::new(None, "{\n  x\n}", false);
        ed.handle_key(ch('m'));
        ed.handle_key(ch('m'));
        assert_eq!(ed.text.cursor(), (2, 0)); // '{' (0,0) -> '}' (2,0)
    }

    // -- word motions (w / W / e / b) -------------------------------------

    #[test]
    fn word_motions_are_punctuation_aware() {
        let mut ed = Editor::new(None, "foo.bar baz", false);
        ed.handle_key(ch('w'));
        assert_eq!(ed.text.cursor(), (0, 3)); // small w stops at "."
        ed.handle_key(ch('e'));
        assert_eq!(ed.text.cursor(), (0, 6)); // e -> end of "bar"
        ed.handle_key(ch('W'));
        assert_eq!(ed.text.cursor(), (0, 8)); // W (WORD) -> "baz"
        ed.handle_key(ch('b'));
        assert_eq!(ed.text.cursor(), (0, 4)); // b -> start of "bar"
    }

    #[test]
    fn visual_e_extends_to_word_end() {
        let mut ed = Editor::new(None, "foo bar", false);
        ed.handle_key(ch('v'));
        ed.handle_key(ch('e'));
        assert_eq!(ed.selection(), Some(((0, 0), (0, 2)))); // "foo"
    }

    // -- text objects (mi<o> / ma<o>) -------------------------------------

    #[test]
    fn text_object_inside_parens() {
        let mut ed = Editor::new(None, "foo(bar)baz", false);
        ed.text.set_cursor(0, 5); // inside, on 'a'
        ed.handle_key(ch('m'));
        ed.handle_key(ch('i'));
        ed.handle_key(ch('('));
        assert_eq!(ed.mode, Mode::Visual);
        assert_eq!(ed.selection(), Some(((0, 4), (0, 6)))); // "bar"
    }

    #[test]
    fn text_object_around_braces() {
        let mut ed = Editor::new(None, "a{bc}d", false);
        ed.text.set_cursor(0, 2); // inside
        ed.handle_key(ch('m'));
        ed.handle_key(ch('a'));
        ed.handle_key(ch('{'));
        assert_eq!(ed.selection(), Some(((0, 1), (0, 4)))); // "{bc}"
    }

    #[test]
    fn text_object_inside_word() {
        let mut ed = Editor::new(None, "foo bar", false);
        ed.text.set_cursor(0, 5); // in "bar"
        ed.handle_key(ch('m'));
        ed.handle_key(ch('i'));
        ed.handle_key(ch('w'));
        assert_eq!(ed.selection(), Some(((0, 4), (0, 6)))); // "bar"
    }

    #[test]
    fn text_object_quotes() {
        let mut ed = Editor::new(None, "say \"hi\" now", false);
        ed.text.set_cursor(0, 6); // inside the quotes
        ed.handle_key(ch('m'));
        ed.handle_key(ch('i'));
        ed.handle_key(ch('"'));
        assert_eq!(ed.selection(), Some(((0, 5), (0, 6)))); // "hi"
    }

    #[test]
    fn text_object_change_replaces_inside() {
        // mi( then c replaces the parens content (the headline use).
        let mut ed = Editor::new(None, "f(xy)", false);
        ed.text.set_cursor(0, 2); // on 'x'
        ed.handle_key(ch('m'));
        ed.handle_key(ch('i'));
        ed.handle_key(ch('('));
        ed.handle_key(ch('c')); // change selection -> Insert
        assert_eq!(ed.mode, Mode::Insert);
        assert_eq!(ed.text.content(), "f()");
        type_str(&mut ed, "Z");
        assert_eq!(ed.text.content(), "f(Z)");
    }

    #[test]
    fn text_object_empty_pair_is_a_noop() {
        let mut ed = Editor::new(None, "f()", false);
        ed.text.set_cursor(0, 1); // on '('
        ed.handle_key(ch('m'));
        ed.handle_key(ch('i'));
        ed.handle_key(ch('(')); // nothing inside -> no selection, stays Normal
        assert_eq!(ed.mode, Mode::Normal);
        assert_eq!(ed.selection(), None);
    }

    // -- count prefix (3w / 5j / 2d / {n}G) -------------------------------

    #[test]
    fn count_repeats_a_motion() {
        let mut ed = Editor::new(None, "alpha beta gamma delta", false);
        ed.handle_key(ch('3'));
        ed.handle_key(ch('w')); // 3 words forward
        assert_eq!(ed.text.cursor(), (0, 17)); // "delta"
    }

    #[test]
    fn count_resets_after_use() {
        let mut ed = Editor::new(None, "abcdefghij", false);
        ed.handle_key(ch('3'));
        ed.handle_key(ch('l')); // (0,3)
        ed.handle_key(ch('l')); // count was consumed -> +1 -> (0,4)
        assert_eq!(ed.text.cursor(), (0, 4));
    }

    #[test]
    fn multi_digit_count_and_zero_is_a_digit_mid_count() {
        let mut ed = Editor::new(None, "0123456789abc", false);
        ed.handle_key(ch('1'));
        ed.handle_key(ch('0')); // count = 10 (0 is a digit mid-count)
        ed.handle_key(ch('l'));
        assert_eq!(ed.text.cursor(), (0, 10));
    }

    #[test]
    fn bare_zero_is_move_home() {
        let mut ed = Editor::new(None, "hello", false);
        ed.handle_key(ch('$')); // end
        ed.handle_key(ch('0')); // home (no count in progress)
        assert_eq!(ed.text.cursor(), (0, 0));
    }

    #[test]
    fn count_deletes_n_lines_one_undo() {
        let mut ed = Editor::new(None, "a\nb\nc\nd", false);
        ed.handle_key(ch('2'));
        ed.handle_key(ch('d')); // delete 2 lines
        assert_eq!(ed.text.content(), "c\nd");
        ed.handle_key(ch('u')); // one undo reverts both
        assert_eq!(ed.text.content(), "a\nb\nc\nd");
    }

    #[test]
    fn count_x_deletes_n_chars() {
        let mut ed = Editor::new(None, "abcdef", false);
        ed.handle_key(ch('3'));
        ed.handle_key(ch('x'));
        assert_eq!(ed.text.content(), "def");
    }

    #[test]
    fn count_g_jumps_to_line_else_top_bottom() {
        let mut ed = Editor::new(None, "1\n2\n3\n4\n5", false);
        ed.handle_key(ch('3'));
        ed.handle_key(ch('G')); // -> line 3
        assert_eq!(ed.text.cursor(), (2, 0));
        ed.handle_key(ch('G')); // bare -> bottom
        assert_eq!(ed.text.cursor().0, 4);
        ed.handle_key(ch('g')); // gg -> top (the goto prefix, 8e-2c)
        ed.handle_key(ch('g'));
        assert_eq!(ed.text.cursor().0, 0);
    }

    // -- :help manual buffer + the corner hint ----------------------------

    #[test]
    fn help_command_opens_readonly_manual() {
        let mut ed = Editor::new(None, "", false); // pristine
        type_str(&mut ed, ":help");
        ed.handle_key(code(KeyCode::Enter));
        // Added a buffer (never replaced the pristine one), now read-only.
        assert_eq!(ed.buffer_count(), 2);
        assert!(ed.readonly);
        assert_eq!(ed.mode_str(), "VIEW");
        assert!(ed.text.content().contains("Quick Reference"));
        assert!(ed.text.content().contains("MULTI-CURSOR"));
    }

    #[test]
    fn help_is_not_duplicated() {
        let mut ed = Editor::new(None, "", false);
        type_str(&mut ed, ":help");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.buffer_count(), 2);
        // ':' + command mode work in a read-only buffer; a second :help no-ops.
        type_str(&mut ed, ":help");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.buffer_count(), 2);
    }

    #[test]
    fn readonly_q_closes_buffer_when_multiple() {
        let mut ed = Editor::new(Some("work".to_string()), "my work", false);
        type_str(&mut ed, ":help");
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.buffer_count(), 2);
        ed.handle_key(ch('q')); // close help, return to work (do NOT quit nora)
        assert!(!ed.quit);
        assert_eq!(ed.buffer_count(), 1);
        assert!(!ed.readonly);
        assert_eq!(ed.text.content(), "my work");
    }

    #[test]
    fn readonly_q_quits_a_lone_view() {
        let mut ed = Editor::new(None, "abc", true); // nora -R, single buffer
        ed.handle_key(ch('q'));
        assert!(ed.quit);
    }

    #[test]
    fn show_hint_only_on_a_pristine_buffer() {
        let mut ed = Editor::new(None, "", false);
        assert!(ed.show_hint()); // pristine: unnamed, empty, unmodified, Normal
        ed.handle_key(ch('i'));
        assert!(!ed.show_hint()); // Insert mode -- you are working now
        type_str(&mut ed, "z");
        ed.handle_key(code(KeyCode::Esc));
        assert!(!ed.show_hint()); // modified
        // A named buffer never shows it.
        let named = Editor::new(Some("f".to_string()), "", false);
        assert!(!named.show_hint());
    }

    // -- the language-server surface (8e-2c) ------------------------------

    fn ctrl(c: char) -> KeyEvent {
        KeyEvent::with(KeyCode::Char(c), kaua::event::Mods::CTRL)
    }

    fn diag(line: usize, col: usize, msg: &str) -> crate::diag::LineDiag {
        crate::diag::LineDiag {
            line,
            col,
            end_col: col,
            severity: crate::diag::Severity::Error,
            message: msg.to_string(),
        }
    }

    #[test]
    fn goto_prefix_gg_ge_and_gd() {
        let mut ed = Editor::new(None, "1\n2\n3", false);
        ed.handle_key(ch('G')); // bottom
        assert_eq!(ed.text.cursor().0, 2);
        ed.handle_key(ch('g'));
        ed.handle_key(ch('g'));
        assert_eq!(ed.text.cursor().0, 0);
        ed.handle_key(ch('g'));
        ed.handle_key(ch('e'));
        assert_eq!(ed.text.cursor().0, 2);
        // `gd` raises the request; it does NOT move the cursor itself (the
        // answer is asynchronous).
        let before = ed.text.cursor();
        ed.handle_key(ch('g'));
        ed.handle_key(ch('d'));
        assert_eq!(ed.take_lsp_request(), Some(LspRequest::Definition));
        assert_eq!(ed.text.cursor(), before);
        // Taken once.
        assert_eq!(ed.take_lsp_request(), None);
    }

    #[test]
    fn goto_prefix_unknown_key_cancels_without_acting() {
        let mut ed = Editor::new(None, "1\n2\n3", false);
        ed.handle_key(ch('G'));
        ed.handle_key(ch('g'));
        ed.handle_key(ch('z')); // not a goto target
        // Neither moved nor requested -- and crucially not treated as a fresh
        // normal-mode `z` either.
        assert_eq!(ed.text.cursor().0, 2);
        assert_eq!(ed.take_lsp_request(), None);
        // The prefix is spent: a following `g` starts a NEW prefix.
        ed.handle_key(ch('g'));
        ed.handle_key(ch('g'));
        assert_eq!(ed.text.cursor().0, 0);
    }

    #[test]
    fn count_g_still_goes_to_a_line_despite_the_prefix() {
        let mut ed = Editor::new(None, "1\n2\n3\n4\n5", false);
        ed.handle_key(ch('3'));
        ed.handle_key(ch('g')); // an explicit count binds directly
        assert_eq!(ed.text.cursor(), (2, 0));
    }

    #[test]
    fn bracket_d_walks_diagnostics_and_echoes_the_message() {
        let mut ed = Editor::new(None, "a\nb\nc\nd\ne", false);
        ed.diags.set(alloc::vec![
            diag(1, 0, "first"),
            diag(3, 0, "second"),
        ]);
        ed.handle_key(ch(']'));
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.cursor().0, 1);
        assert_eq!(ed.status.as_deref(), Some("first"));
        ed.handle_key(ch(']'));
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.cursor().0, 3);
        // Wraps forward.
        ed.handle_key(ch(']'));
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.cursor().0, 1);
        // And backward.
        ed.handle_key(ch('['));
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.cursor().0, 3);
    }

    #[test]
    fn bracket_d_with_no_diagnostics_says_so_and_stays_put() {
        let mut ed = Editor::new(None, "a\nb", false);
        ed.handle_key(ch('j'));
        ed.handle_key(ch(']'));
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.cursor().0, 1);
        assert_eq!(ed.status.as_deref(), Some("no diagnostics"));
    }

    #[test]
    fn a_diagnostic_column_lands_the_cursor_in_char_coordinates() {
        // The regression for the byte-vs-char column mismatch: with a
        // multi-byte character before the span, a BYTE column would overshoot.
        // "aé" is 3 bytes but 2 chars, so a diagnostic at char column 2 is the
        // 'x' -- a byte column 2 would be inside 'é'.
        let mut ed = Editor::new(None, "aéx", false);
        ed.diags.set(alloc::vec![diag(0, 2, "here")]);
        ed.handle_key(ch(']'));
        ed.handle_key(ch('d'));
        assert_eq!(ed.text.cursor(), (0, 2));
        assert_eq!(ed.text.char_at(0, 2), Some('x'));
    }

    #[test]
    fn diagnostic_past_the_buffer_end_clamps_rather_than_escaping() {
        let mut ed = Editor::new(None, "a\nb", false);
        // The server is a version behind: it published against a longer file.
        ed.diags.set(alloc::vec![diag(99, 40, "stale")]);
        ed.handle_key(ch(']'));
        ed.handle_key(ch('d'));
        let (row, col) = ed.text.cursor();
        assert!(row < ed.text.line_count());
        assert!(col <= ed.text.char_len(row));
    }

    #[test]
    fn k_requests_hover_and_the_next_key_dismisses_it() {
        let mut ed = Editor::new(None, "abc", false);
        ed.handle_key(ch('K'));
        assert_eq!(ed.take_lsp_request(), Some(LspRequest::Hover));
        ed.show_hover("func F(x int) error".to_string());
        assert_eq!(ed.hover(), Some("func F(x int) error"));
        // The dismissing key STILL does its job -- hover never costs a stroke.
        ed.handle_key(ch('l'));
        assert_eq!(ed.hover(), None);
        assert_eq!(ed.text.cursor(), (0, 1));
    }

    #[test]
    fn an_empty_hover_answer_reports_instead_of_opening_a_blank_box() {
        let mut ed = Editor::new(None, "abc", false);
        ed.show_hover("   \n\n".to_string());
        assert_eq!(ed.hover(), None);
        assert_eq!(ed.status.as_deref(), Some("no hover information"));
    }

    fn cand(label: &str, insert: &str) -> Candidate {
        Candidate {
            label: label.to_string(),
            detail: None,
            insert: insert.to_string(),
        }
    }

    #[test]
    fn ctrl_n_in_insert_requests_completion() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "fm");
        ed.handle_key(ctrl('n'));
        assert_eq!(ed.take_lsp_request(), Some(LspRequest::Completion));
        // The ctrl key did not type an 'n'.
        assert_eq!(ed.text.content(), "fm");
    }

    #[test]
    fn accepting_a_completion_replaces_the_typed_prefix() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "fmt.Pri");
        ed.show_completion(alloc::vec![cand("Println", "Println")]);
        assert!(ed.completion_state().is_some());
        ed.handle_key(code(KeyCode::Enter));
        // The prefix is the word chars only -- `fmt.` is not swallowed.
        assert_eq!(ed.text.content(), "fmt.Println");
        assert_eq!(ed.mode, Mode::Insert);
        // One undo reverts the whole completion, not half of it.
        ed.handle_key(code(KeyCode::Esc));
        ed.handle_key(ch('u'));
        assert_eq!(ed.text.content(), "fmt.Pri");
    }

    #[test]
    fn completion_selection_wraps_and_esc_cancels() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "P");
        ed.show_completion(alloc::vec![cand("Print", "Print"), cand("Println", "Println")]);
        assert_eq!(ed.completion_state().map(|(_, s)| s), Some(0));
        ed.handle_key(ctrl('n'));
        assert_eq!(ed.completion_state().map(|(_, s)| s), Some(1));
        ed.handle_key(ctrl('n')); // wraps
        assert_eq!(ed.completion_state().map(|(_, s)| s), Some(0));
        ed.handle_key(ctrl('p')); // wraps backwards
        assert_eq!(ed.completion_state().map(|(_, s)| s), Some(1));
        ed.handle_key(code(KeyCode::Esc));
        assert!(ed.completion_state().is_none());
        assert_eq!(ed.mode, Mode::Insert);
        assert_eq!(ed.text.content(), "P"); // nothing inserted
    }

    #[test]
    fn typing_through_the_completion_popup_closes_it_and_types() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        type_str(&mut ed, "P");
        ed.show_completion(alloc::vec![cand("Print", "Print")]);
        ed.handle_key(ch('r'));
        assert!(ed.completion_state().is_none());
        // The key was not swallowed: the stale list closes AND the char lands.
        assert_eq!(ed.text.content(), "Pr");
    }

    #[test]
    fn an_empty_or_late_completion_answer_never_opens_a_popup() {
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        ed.show_completion(Vec::new());
        assert!(ed.completion_state().is_none());
        assert_eq!(ed.status.as_deref(), Some("no completions"));
        // A late answer arriving after the user left Insert is dropped: it
        // would otherwise complete into a mode that never asked.
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.mode, Mode::Normal);
        ed.show_completion(alloc::vec![cand("Print", "Print")]);
        assert!(ed.completion_state().is_none());
        assert_eq!(ed.mode, Mode::Normal);
    }

    #[test]
    fn jump_to_moves_within_the_current_file() {
        let mut ed = Editor::new(Some("a.go".to_string()), "1\n2\n3\n4", false);
        ed.jump_to(Some("a.go".to_string()), 2, 0);
        assert_eq!(ed.text.cursor(), (2, 0));
        assert_eq!(ed.take_request(), None); // no file op needed
        // A None path means "wherever we are".
        ed.jump_to(None, 3, 0);
        assert_eq!(ed.text.cursor(), (3, 0));
    }

    #[test]
    fn a_cross_file_jump_opens_then_positions() {
        let mut ed = Editor::new(Some("a.go".to_string()), "x", false);
        ed.jump_to(Some("b.go".to_string()), 2, 1);
        // The editor cannot read the file; it asks the binary to.
        assert_eq!(ed.take_request(), Some(Request::Open("b.go".to_string())));
        // The cursor lands once the buffer exists.
        ed.open_buffer(Some("b.go".to_string()), "aa\nbb\ncc");
        assert_eq!(ed.text.cursor(), (2, 1));
        // The parked jump is spent -- a later unrelated open must not inherit it.
        ed.open_buffer(Some("c.go".to_string()), "zz\nyy\nxx");
        assert_eq!(ed.text.cursor(), (0, 0));
    }

    #[test]
    fn a_completion_mode_with_no_candidates_fails_closed_into_insert() {
        // The mode and the list must agree; if they ever drift, a keypress
        // must not divide by zero.
        let mut ed = Editor::new(None, "", false);
        ed.handle_key(ch('i'));
        ed.show_completion(alloc::vec![cand("Print", "Print")]);
        ed.completion.clear(); // simulate the drift
        ed.handle_key(ctrl('n'));
        assert_eq!(ed.mode, Mode::Insert);
    }

    #[test]
    fn a_buffer_switch_drops_hover_and_completion() {
        let mut ed = Editor::new(Some("a.go".to_string()), "abc", false);
        ed.show_hover("about a.go".to_string());
        assert!(ed.hover().is_some());
        ed.open_buffer(Some("b.go".to_string()), "def");
        // Overlays describe a position in the file we just left.
        assert!(ed.hover().is_none());
        assert!(ed.completion_state().is_none());
    }

    // -- the debugger dashboard (8f-2) --------------------------------------

    fn dbg_view() -> DebugView {
        DebugView {
            status: "stopped: breakpoint at main.parkLoop".to_string(),
            frames: alloc::vec![crate::debug::StackRow {
                func: "main.parkLoop".to_string(),
                location: "child.go:23".to_string(),
            }],
            locals: alloc::vec![crate::debug::VarRow {
                name: "i".to_string(),
                value: "3".to_string(),
            }],
            goroutines: alloc::vec![crate::debug::GoroutineRow {
                id: 1,
                state: "running".to_string(),
            }],
            console: alloc::vec!["stopped".to_string()],
        }
    }

    #[test]
    fn the_dashboard_is_collapsed_until_a_view_is_pushed() {
        let mut ed = Editor::new(Some("m.go".to_string()), "package main", false);
        assert!(!ed.debugging());
        assert!(ed.debug_view().is_none());
        ed.set_debug_view(Some(dbg_view()));
        assert!(ed.debugging());
        // Opening focuses the source buffer (NORA-IDE-UX section 2.2).
        assert_eq!(ed.dash().focus, DashPane::Editor);
        ed.set_debug_view(None);
        assert!(!ed.debugging());
    }

    #[test]
    fn a_mid_session_refresh_keeps_the_current_focus() {
        // A stop pushes a fresh view; it must not yank focus off the tile the
        // user was reading.
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view()));
        ed.handle_key(code(KeyCode::Tab)); // focus Variables
        assert_eq!(ed.dash().focus, DashPane::Variables);
        ed.set_debug_view(Some(dbg_view())); // a later stop refreshes the data
        assert_eq!(ed.dash().focus, DashPane::Variables);
    }

    #[test]
    fn tab_cycles_dashboard_focus_only_while_debugging() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        // Not debugging: Tab is inert in Normal (no session to focus).
        ed.handle_key(code(KeyCode::Tab));
        assert!(!ed.debugging());
        assert_eq!(ed.mode, Mode::Normal);

        ed.set_debug_view(Some(dbg_view()));
        ed.handle_key(code(KeyCode::Tab));
        assert_eq!(ed.dash().focus, DashPane::Variables);
        ed.handle_key(code(KeyCode::Tab));
        assert_eq!(ed.dash().focus, DashPane::CallStack);
        ed.handle_key(code(KeyCode::Tab));
        assert_eq!(ed.dash().focus, DashPane::Goroutines);
        ed.handle_key(code(KeyCode::Tab));
        assert_eq!(ed.dash().focus, DashPane::Console);
        ed.handle_key(code(KeyCode::Tab)); // wraps back to the editor
        assert_eq!(ed.dash().focus, DashPane::Editor);
    }

    // -- the navigable dashboard (8f-2b-1) ---------------------------------

    fn dbg_view_n(frames: usize, locals: usize, gors: usize) -> DebugView {
        DebugView {
            status: "stopped".to_string(),
            frames: (0..frames)
                .map(|i| crate::debug::StackRow {
                    func: alloc::format!("f{}", i),
                    location: alloc::format!("m.go:{}", i),
                })
                .collect(),
            locals: (0..locals)
                .map(|i| crate::debug::VarRow {
                    name: alloc::format!("v{}", i),
                    value: i.to_string(),
                })
                .collect(),
            goroutines: (0..gors)
                .map(|i| crate::debug::GoroutineRow {
                    id: i as i64,
                    state: "running".to_string(),
                })
                .collect(),
            console: alloc::vec!["x".to_string()],
        }
    }

    /// Focus a specific tile by cycling Tab from the editor.
    fn focus_tile(ed: &mut Editor, target: DashPane) {
        for _ in 0..6 {
            if ed.dash().focus == target {
                return;
            }
            ed.handle_key(code(KeyCode::Tab));
        }
        panic!("could not reach {:?}", target);
    }

    #[test]
    fn dashboard_j_k_move_the_focused_tiles_selection() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(3, 0, 0)));
        focus_tile(&mut ed, DashPane::CallStack);
        assert_eq!(ed.dash().stack_sel, 0);
        ed.handle_key(ch('j'));
        assert_eq!(ed.dash().stack_sel, 1);
        ed.handle_key(ch('j'));
        ed.handle_key(ch('j')); // clamps at the last row (2)
        assert_eq!(ed.dash().stack_sel, 2);
        ed.handle_key(ch('k'));
        assert_eq!(ed.dash().stack_sel, 1);
        ed.handle_key(ch('g')); // top
        assert_eq!(ed.dash().stack_sel, 0);
        ed.handle_key(ch('G')); // bottom
        assert_eq!(ed.dash().stack_sel, 2);
    }

    #[test]
    fn dashboard_selection_clamps_when_the_data_shrinks() {
        // A stop shrinks the stack under a stale selection; the next move clamps
        // to the live row count first, so the selection can never point off the
        // end.
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(3, 0, 0)));
        focus_tile(&mut ed, DashPane::CallStack);
        ed.handle_key(ch('G')); // stack_sel = 2
        assert_eq!(ed.dash().stack_sel, 2);
        ed.set_debug_view(Some(dbg_view_n(1, 0, 0))); // refresh: one frame now
        ed.handle_key(ch('j')); // clamp 2 -> 0 (max), then +1 -> stays 0
        assert_eq!(ed.dash().stack_sel, 0);
    }

    #[test]
    fn dashboard_h_l_collapse_and_expand_the_locals_group() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(0, 2, 0)));
        focus_tile(&mut ed, DashPane::Variables);
        assert!(ed.dash().locals_expanded);
        // Move onto a leaf, then collapse -> the group node is the only row left,
        // so the selection snaps back to it.
        ed.handle_key(ch('j'));
        assert_eq!(ed.dash().var_sel, 1);
        ed.handle_key(ch('h')); // collapse
        assert!(!ed.dash().locals_expanded);
        assert_eq!(ed.dash().var_sel, 0);
        ed.handle_key(ch('j')); // only the group node -> no move
        assert_eq!(ed.dash().var_sel, 0);
        ed.handle_key(ch('l')); // expand again
        assert!(ed.dash().locals_expanded);
        ed.handle_key(ch('j'));
        assert_eq!(ed.dash().var_sel, 1);
    }

    #[test]
    fn dashboard_l_h_step_the_console_tabs() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(1, 1, 1)));
        focus_tile(&mut ed, DashPane::Console);
        assert_eq!(ed.dash().console_tab, 0); // Program
        ed.handle_key(ch('l'));
        assert_eq!(ed.dash().console_tab, 1); // Debug
        ed.handle_key(ch('h'));
        assert_eq!(ed.dash().console_tab, 0);
    }

    #[test]
    fn dashboard_esc_returns_focus_to_the_editor() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(2, 0, 0)));
        focus_tile(&mut ed, DashPane::CallStack);
        ed.handle_key(code(KeyCode::Esc));
        assert_eq!(ed.dash().focus, DashPane::Editor);
    }

    #[test]
    fn dashboard_keys_are_inert_over_a_focused_editor() {
        // A session is live but the editor holds focus: `j` is an ordinary text
        // motion (moves the cursor down), never a tile move. This is the whole
        // no-regression contract -- editing is unchanged while debugging.
        let mut ed = Editor::new(Some("m.go".to_string()), "one\ntwo", false);
        ed.set_debug_view(Some(dbg_view_n(2, 0, 0)));
        assert_eq!(ed.dash().focus, DashPane::Editor);
        ed.handle_key(ch('j'));
        assert_eq!(ed.text.cursor().0, 1); // the buffer cursor moved down
        assert_eq!(ed.dash().stack_sel, 0); // the tile selection did not move
    }

    #[test]
    fn dashboard_enter_on_the_call_stack_raises_select_frame() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(3, 0, 0)));
        focus_tile(&mut ed, DashPane::CallStack);
        ed.handle_key(ch('j')); // stack_sel = 1
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_dap_request(), Some(DapRequest::SelectFrame(1)));
    }

    #[test]
    fn dashboard_enter_on_goroutines_raises_select_goroutine() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(1, 0, 3)));
        focus_tile(&mut ed, DashPane::Goroutines);
        ed.handle_key(ch('G')); // gor_sel = 2 (last)
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_dap_request(), Some(DapRequest::SelectGoroutine(2)));
    }

    #[test]
    fn dashboard_enter_clamps_a_stale_selection_before_selecting() {
        // The stack shrinks under a bottom-pinned selection; Enter must name a
        // live row (the clamp), never the stale off-the-end index.
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(3, 0, 0)));
        focus_tile(&mut ed, DashPane::CallStack);
        ed.handle_key(ch('G')); // stack_sel = 2
        ed.set_debug_view(Some(dbg_view_n(1, 0, 0))); // refresh: one frame now
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_dap_request(), Some(DapRequest::SelectFrame(0)));
    }

    #[test]
    fn dashboard_enter_is_inert_on_the_variables_and_console_tiles() {
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(1, 2, 1)));
        focus_tile(&mut ed, DashPane::Variables);
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_dap_request(), None);
        focus_tile(&mut ed, DashPane::Console);
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_dap_request(), None);
    }

    #[test]
    fn dashboard_enter_is_inert_over_an_empty_tile() {
        // No frames to select: Enter raises nothing (never an out-of-range index).
        let mut ed = Editor::new(Some("m.go".to_string()), "x", false);
        ed.set_debug_view(Some(dbg_view_n(0, 0, 1)));
        focus_tile(&mut ed, DashPane::CallStack);
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_dap_request(), None);
    }

    #[test]
    fn dashboard_enter_over_a_focused_editor_raises_no_request() {
        // A live session but the editor holds focus: Enter is an ordinary text
        // key, never a tile action -- the no-regression contract for editing
        // while debugging.
        let mut ed = Editor::new(Some("m.go".to_string()), "one\ntwo", false);
        ed.set_debug_view(Some(dbg_view_n(2, 0, 0)));
        assert_eq!(ed.dash().focus, DashPane::Editor);
        ed.handle_key(code(KeyCode::Enter));
        assert_eq!(ed.take_dap_request(), None);
    }
}
