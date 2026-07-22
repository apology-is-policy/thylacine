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

/// Parse the kernel `/proc/<pid>/kstack` backtrace into kernel `StackRow`s (the
/// kernel half of the unified stack, section 5; 8f-3b). The file's owner-axis
/// form is one frame per line, `#<i>  <symbol>` -- `<name>+0x<off>` (the Halls
/// HX-2 symtab), or `<running>` / `<unknown>`. The `#<i>` index is dropped (the
/// unified stack renumbers continuously past the Go frames), and a kernel frame
/// carries no source (kernel DWARF is deferred, so `location` stays empty). Blank
/// lines + surrounding whitespace are tolerated; a line without the `#<i>` shape
/// is skipped (never a wrong row).
pub fn parse_kstack(text: &str) -> Vec<StackRow> {
    let mut out = Vec::new();
    for line in text.lines() {
        let line = line.trim();
        let rest = match line.strip_prefix('#') {
            Some(r) => r,
            None => continue,
        };
        let sym = rest.trim_start_matches(|c: char| c.is_ascii_digit()).trim_start();
        if sym.is_empty() {
            continue;
        }
        out.push(StackRow::kernel(String::from(sym), String::new()));
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_kstack_reads_symbolic_frames() {
        let text = "#0  sleep+0x1c\n#1  sched.c::rendezvous+0x40\n#2  el0_return+0x8\n";
        let rows = parse_kstack(text);
        assert_eq!(rows.len(), 3);
        assert!(rows.iter().all(|r| r.kernel), "every parsed row is a kernel frame");
        assert_eq!(rows[0].func, "sleep+0x1c");
        assert_eq!(rows[1].func, "sched.c::rendezvous+0x40");
        assert!(rows[0].location.is_empty(), "kernel frames carry no source");
    }

    #[test]
    fn parse_kstack_handles_running_unknown_and_blanks() {
        let rows = parse_kstack("\n#0  <running>\n\n#1  <unknown>\n");
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].func, "<running>");
        assert_eq!(rows[1].func, "<unknown>");
    }

    #[test]
    fn parse_kstack_skips_malformed_and_empty() {
        assert!(parse_kstack("").is_empty());
        assert!(parse_kstack("no hash here\n").is_empty());
        assert!(parse_kstack("#\n#3\n").is_empty(), "a hash with no symbol is skipped");
    }
}
