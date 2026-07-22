// nora::debug -- the debugger dashboard display model (pure, host-testable).
//
// The protocol-free snapshot the DAP host (`dap_host.rs`) pushes into the
// `Editor` for the dashboard to render (8f-2, NORA-IDE-UX section 2). Like
// `nora::diag`, it holds plain data -- no `parley`, no DAP types -- so a second
// debugger backend or a test can populate the same struct, and the renderer +
// its tests need no process. The DAP host owns the protocol; this is the
// already-decoded state the tiles draw.
//
// `Editor::debug_view()` is `Some` exactly while a session is live; `None`
// collapses the dashboard to a full-width editor (NORA-IDE-UX section 2.2).

use alloc::string::String;
use alloc::vec::Vec;

/// One frame of the Call Stack tile (the unified user->kernel stack, section 5).
/// Go frames come first, then the kernel frames beneath the SVC boundary; the
/// renderer draws the `── kernel ──` divider at the transition and dims the
/// kernel rows (8f-3a). The DAP (Go) half is built from Ambush's `stackTrace`;
/// the kernel half is read from `/proc/<pid>/kstack` (8f-3b).
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct StackRow {
    /// The function name (`main.parkLoop`, or a kernel `sched.c::sleep`).
    pub func: String,
    /// `"main.go:23"`, or `""` when the frame has no source (kernel frames, and
    /// Go runtime asm frames).
    pub location: String,
    /// This frame is below the user->kernel boundary -- dim-styled, and the
    /// first such frame carries the `── kernel ──` divider above it.
    pub kernel: bool,
}

impl StackRow {
    /// A user (Go) frame.
    pub fn go(func: String, location: String) -> Self {
        StackRow {
            func,
            location,
            kernel: false,
        }
    }

    /// A kernel frame (below the SVC boundary; the symbolized `func+offset` from
    /// the Halls HX-2 symtab via `/proc/<pid>/kstack`, section 5).
    pub fn kernel(func: String, location: String) -> Self {
        StackRow {
            func,
            location,
            kernel: true,
        }
    }
}

/// One visible row of the Variables tile: the flattened, visible-only view of
/// the variable tree (8f-2b-3). A row is a frame local or, when its parent is
/// expanded, a nested field. The DAP host owns the tree (per-node children +
/// expand state) and flattens the visible nodes into this list on every publish,
/// so the renderer + the editor's row cursor stay a simple flat index.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct VarRow {
    pub name: String,
    pub value: String,
    /// Nesting depth (0 = a frame's top-level local; 1+ = a field of an expanded
    /// struct / slice / map).
    pub depth: u16,
    /// The value is structured and can be expanded (DAP `variablesReference`
    /// != 0) -- shows a ▸/▾ marker.
    pub expandable: bool,
    /// This expandable node is open; the following deeper rows are its children.
    pub expanded: bool,
}

/// One row of the Goroutines tile.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct GoroutineRow {
    pub id: i64,
    /// The debugger's one-line goroutine description (Delve puts the state +
    /// location here).
    pub state: String,
}

/// The dashboard snapshot for one moment of a debug session: the current
/// one-line status, the three sidebar tiles, and the console scrollback. The
/// DAP host rebuilds and pushes this on every meaningful event; the renderer
/// only draws it.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct DebugView {
    /// The persistent state line (`"stopped: breakpoint at main.parkLoop"`,
    /// `"running"`, `"launching foo ..."`), shown at the top of the Console.
    pub status: String,
    /// The call stack, top frame first.
    pub frames: Vec<StackRow>,
    /// The current frame's locals.
    pub locals: Vec<VarRow>,
    /// The debuggee's goroutines/threads.
    pub goroutines: Vec<GoroutineRow>,
    /// The console scrollback (bounded by the host): program output + notable
    /// debug events, oldest first.
    pub console: Vec<String>,
}

impl DebugView {
    /// An empty view carrying just a status line -- the dashboard shape appears
    /// (expanded) the instant a session starts, before any stop fills the
    /// tiles.
    pub fn launching(status: String) -> Self {
        DebugView {
            status,
            frames: Vec::new(),
            locals: Vec::new(),
            goroutines: Vec::new(),
            console: Vec::new(),
        }
    }
}
