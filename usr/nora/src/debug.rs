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

/// One frame of the Call Stack tile (the unified user->kernel stack, section 5;
/// the `── kernel ──` divider + `kernel` styling arrive with 8f-3).
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct StackRow {
    /// The function name (`main.parkLoop`).
    pub func: String,
    /// `"main.go:23"`, or `""` when the frame has no source.
    pub location: String,
}

/// One row of the Variables tile (flat at 8f-2a; a real expandable `Tree` at
/// 8f-2b).
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct VarRow {
    pub name: String,
    pub value: String,
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
