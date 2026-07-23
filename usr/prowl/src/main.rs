// prowl -- the scheduler-aware process monitor (docs/PROWL-DESIGN.md; prowl-2).
//
// A native Kaua TUI (like nora): it polls the prowl-1 kernel telemetry --
// /ctl/procs (the all-pids process table + cumulative cpu_ns) + /ctl/sched (the
// online CPU count) -- diffs cpu_ns across ~1.5 s ticks for %CPU (the htop
// method), renders a per-core-aware CPU meter + a sortable process list, and (as
// a manager) kills the selected process via /proc/<pid>/ctl. prowl adds NO
// authority of its own: the kernel's I-26 two-axis gate decides every kill, so a
// confined user kills only its own processes.
//
// CONSOLE DISCIPLINE (KAUA.md / I-27; the nora contract): prowl owns the SCREEN
// on fd 1 (Terminal) and reads keys on fd 0 (PollSource); it NEVER touches the
// line discipline (consctl). ut sets raw termios before the spawn (the T-4
// dance -- prowl is in ut's is_raw_command set) and re-cooks + restores the
// screen on prowl's exit OR crash (the panic=abort backstop, since a no_std Drop
// does not run on a crash). prowl is never console-attached -> I-27 untouched.
// A buggy prowl corrupts only its own screen -- it validates nothing on the
// kernel's behalf; the kernel gates every read + the kill.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{Read, Write};
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::time::Instant;
use libthyla_rs::{env, t_putstr};

use kaua::event::{Event, KeyCode, KeyEvent};
use kaua::rect::Rect;
use kaua::source::{EventSource, PollSource};
use kaua::term::Terminal;

mod sample;
mod ui;

use sample::{CpuRow, CpuSampler, ProcRow, Sampler, SchedDetail, Sort};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

// Fallback console geometry (a dumb terminal / the non-interactive harness),
// mirroring nora + the ls-7 LS-CI PTY pin.
const COLS: u16 = 80;
const ROWS: u16 = 24;
const MIN_DIM: u16 = 1;
const MAX_DIM: u16 = 1000;
const SIZE_QUERY_TIMEOUT_MS: u32 = 150;
/// Refresh cadence: the poll timeout doubles as the max tick gap. ~1.5 s is the
/// htop default -- long enough that the cpu_ns delta is meaningful, short enough
/// to feel live. Keys wake the loop early (responsive nav) without resampling
/// unless a full interval has elapsed.
const REFRESH_MS: u32 = 1500;
/// /ctl/procs caps at DEVCTL_READ_BUF (2048) kernel-side; 4 KiB holds the whole
/// snapshot in one read with headroom. The past-~30-procs truncation is a kernel
/// pagination seam (the #62 perf backlog), not prowl's.
const CTL_BUF: usize = 4096;

/// The whole prowl UI state.
pub struct App {
    sampler: Sampler,
    pub rows: Vec<ProcRow>,
    pub sort: Sort,
    /// The cursor tracks a PID, not an index, so it stays on a process across
    /// re-sorts + list churn (the htop cursor-follows behaviour).
    selected_pid: Option<i64>,
    last_sample: Instant,
    pub ncpus: usize,
    pub confirm_kill: Option<(i64, String)>,
    pub status: Option<String>,
    // prowl-3c: the per-CPU meter (from /ctl/cpu) + the toggleable per-thread
    // scheduler detail (from /proc/<pid>/sched, the OQ-4-gated deep view).
    cpu_sampler: CpuSampler,
    pub cpus: Vec<CpuRow>,
    pub show_detail: bool,
    /// The selected process's parsed /proc/<pid>/sched, refreshed while the detail
    /// pane is open. None == unavailable (denied by the OQ-4 gate, or the process
    /// exited) -- the pane says so.
    pub detail: Option<SchedDetail>,
    /// prowl-4: render the process list as a parent->child tree (indented by ppid
    /// depth) instead of the flat sorted list. Toggled by `t`.
    pub show_tree: bool,
}

impl App {
    fn new(sort: Sort) -> App {
        App {
            sampler: Sampler::new(),
            rows: Vec::new(),
            sort,
            selected_pid: None,
            last_sample: Instant::now(),
            ncpus: 1,
            confirm_kill: None,
            status: None,
            cpu_sampler: CpuSampler::new(),
            cpus: Vec::new(),
            show_detail: false,
            detail: None,
            show_tree: false,
        }
    }

    /// Re-read the selected process's /proc/<pid>/sched into `detail` (only while
    /// the pane is open -- the gated per-process read is skipped otherwise). None
    /// when no process is selected, denied, or the process is gone.
    fn refresh_detail(&mut self) {
        if !self.show_detail {
            self.detail = None;
            return;
        }
        self.detail = match self.selected_pid {
            Some(pid) => sample::parse_sched(&read_ctl_file(&format!("/proc/{}/sched", pid))),
            None => None,
        };
    }

    /// The index of the cursor's process in the current (sorted) list, or 0 if
    /// the tracked pid vanished.
    pub fn cur_index(&self) -> usize {
        match self.selected_pid {
            Some(pid) => self.rows.iter().position(|r| r.pid == pid).unwrap_or(0),
            None => 0,
        }
    }

    fn move_selection(&mut self, delta: isize) {
        if self.rows.is_empty() {
            self.selected_pid = None;
            return;
        }
        let cur = self.cur_index() as isize;
        let last = self.rows.len() as isize - 1;
        let idx = (cur + delta).clamp(0, last) as usize;
        self.selected_pid = Some(self.rows[idx].pid);
    }

    fn select_index(&mut self, i: usize) {
        if self.rows.is_empty() {
            self.selected_pid = None;
            return;
        }
        let idx = i.min(self.rows.len() - 1);
        self.selected_pid = Some(self.rows[idx].pid);
    }

    /// After a resample re-sorted/churned the list, keep the cursor valid: if the
    /// tracked pid is gone (or was never set), snap to the top.
    fn reconcile_selection(&mut self) {
        if self.rows.is_empty() {
            self.selected_pid = None;
            return;
        }
        let live = self
            .selected_pid
            .map_or(false, |pid| self.rows.iter().any(|r| r.pid == pid));
        if !live {
            self.selected_pid = Some(self.rows[0].pid);
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let sort = parse_args();

    let mut app = App::new(sort);
    app.ncpus = sample::parse_ncpus(&read_ctl_file("/ctl/sched"));

    // Measure the real console (CPR round-trip); fall back to 80x24.
    let probe = kaua::query::terminal_size(SIZE_QUERY_TIMEOUT_MS);
    let (cols, rows) = probe
        .size
        .map(|(c, r)| (c.clamp(MIN_DIM, MAX_DIM), r.clamp(MIN_DIM, MAX_DIM)))
        .unwrap_or((COLS, ROWS));

    let mut term = match Terminal::enter(Rect::new(0, 0, cols, rows)) {
        Ok(t) => t,
        Err(_) => {
            t_putstr("prowl: cannot acquire the console screen\n");
            return 1;
        }
    };
    // Replay any keystroke typed during the launch probe (kaua::query #117-F2).
    let mut src = PollSource::with_pending(probe.pending);

    let code = run(&mut term, &mut src, &mut app);
    // Explicit restore (Drop also runs it; both idempotent). ut re-cooks the
    // console + re-emits this on a crash (the panic=abort backstop).
    let _ = term.leave();
    code as i64
}

fn run(term: &mut Terminal, src: &mut PollSource, app: &mut App) -> i32 {
    // Prime the sampler: the first frame reads cumulative cpu_ns with no delta
    // (0% everywhere); the next tick shows real rates.
    resample(app);
    if ui::render(term, app).is_err() {
        return 1;
    }
    loop {
        if src.is_eof() {
            return 0;
        }
        // Block up to one refresh interval for a key. An empty return == the
        // timeout lapsed == a tick: re-sample + redraw.
        let events = match src.poll(PollTimeout::Millis(REFRESH_MS)) {
            Ok(e) => e,
            Err(_) => return 1,
        };
        let mut dirty = false;
        if events.is_empty() {
            resample(app);
            dirty = true;
        } else {
            for ev in events {
                match ev {
                    Event::Key(k) => match handle_key(app, k) {
                        Action::Quit => return 0,
                        Action::Redraw => dirty = true,
                        Action::None => {}
                    },
                    // A late CPR the launch probe missed (slow HVF serial): resize
                    // to the real console. There is no winsize signal over UART, so
                    // this is the only live-resize path (mirrors nora).
                    Event::Resize(c, r) => {
                        let c = c.clamp(MIN_DIM, MAX_DIM);
                        let r = r.clamp(MIN_DIM, MAX_DIM);
                        if (c, r) != (term.area().width, term.area().height) {
                            term.resize(Rect::new(0, 0, c, r));
                            dirty = true;
                        }
                    }
                    _ => {}
                }
            }
            // Refresh even under sustained key activity once the interval lapses,
            // so %CPU never stalls while the user navigates.
            if app.last_sample.elapsed().as_millis() as u64 >= REFRESH_MS as u64 {
                resample(app);
                dirty = true;
            }
        }
        if dirty && ui::render(term, app).is_err() {
            return 1;
        }
    }
}

/// The outcome of a keypress.
enum Action {
    None,
    Redraw,
    Quit,
}

fn handle_key(app: &mut App, k: KeyEvent) -> Action {
    // Confirm-kill mode intercepts every key -- a safety gate so a stray `k`
    // cannot terminate a process.
    if let Some((pid, name)) = app.confirm_kill.clone() {
        match k.code {
            KeyCode::Char('y') | KeyCode::Char('Y') => {
                app.confirm_kill = None;
                app.status = Some(if kill_pid(pid) {
                    format!("killed {} {}", pid, name)
                } else {
                    format!("kill {} denied", pid)
                });
                return Action::Redraw;
            }
            KeyCode::Char('n') | KeyCode::Char('N') | KeyCode::Esc => {
                app.confirm_kill = None;
                app.status = Some(String::from("kill cancelled"));
                return Action::Redraw;
            }
            _ => return Action::None,
        }
    }
    // Ctrl-C (ut has -isig, so it arrives as a raw key) quits like q.
    if k.is_ctrl('c') {
        return Action::Quit;
    }
    app.status = None; // a fresh action clears a stale status line
    match k.code {
        KeyCode::Char('q') | KeyCode::Esc => Action::Quit,
        KeyCode::Up => {
            app.move_selection(-1);
            app.refresh_detail(); // the pane follows the cursor (no-op if closed)
            Action::Redraw
        }
        KeyCode::Down => {
            app.move_selection(1);
            app.refresh_detail();
            Action::Redraw
        }
        KeyCode::PageUp => {
            app.move_selection(-10);
            app.refresh_detail();
            Action::Redraw
        }
        KeyCode::PageDown => {
            app.move_selection(10);
            app.refresh_detail();
            Action::Redraw
        }
        KeyCode::Home => {
            app.select_index(0);
            app.refresh_detail();
            Action::Redraw
        }
        KeyCode::End => {
            app.select_index(usize::MAX);
            app.refresh_detail();
            Action::Redraw
        }
        // Toggle the per-thread scheduler detail pane (the OQ-4-gated deep view).
        KeyCode::Char('d') => {
            app.show_detail = !app.show_detail;
            app.refresh_detail();
            Action::Redraw
        }
        KeyCode::Char('s') => {
            app.sort = app.sort.next();
            app.sort.apply(&mut app.rows);
            Action::Redraw
        }
        KeyCode::Char('r') | KeyCode::Char(' ') => {
            resample(app);
            Action::Redraw
        }
        KeyCode::Char('k') => {
            if let Some(pid) = app.selected_pid {
                if let Some(row) = app.rows.iter().find(|r| r.pid == pid) {
                    app.confirm_kill = Some((pid, row.name.clone()));
                    return Action::Redraw;
                }
            }
            Action::None
        }
        // prowl-4: toggle the parent->child tree view.
        KeyCode::Char('t') => {
            app.show_tree = !app.show_tree;
            Action::Redraw
        }
        // prowl-4: job-control suspend (z, the Ctrl-Z/SIGTSTP mnemonic) + resume
        // (c, the SIGCONT mnemonic) via /proc/<pid>/ctl. NOT confirm-gated -- both
        // are reversible (unlike kill); the status line reports the outcome, and
        // the STATE column shows STOPPED on the next tick. The kernel's I-26 gate
        // decides authority (same as kill) -- prowl confers none.
        KeyCode::Char('z') => job_action(app, b"suspend", "suspended", "suspend"),
        KeyCode::Char('c') => job_action(app, b"resume", "resumed", "resume"),
        _ => Action::None,
    }
}

/// prowl-4: apply a job-control verb (suspend/resume) to the selected process and
/// set the status line. `verb` is the ctl bytes; `ok_word`/`deny_word` frame the
/// status. Reversible, so no confirm gate (unlike kill).
fn job_action(app: &mut App, verb: &[u8], ok_word: &str, deny_word: &str) -> Action {
    if let Some(pid) = app.selected_pid {
        let name = app
            .rows
            .iter()
            .find(|r| r.pid == pid)
            .map(|r| r.name.clone())
            .unwrap_or_default();
        app.status = Some(if ctl_write(pid, verb) {
            format!("{} {} {}", ok_word, pid, name)
        } else {
            format!("{} {} denied", deny_word, pid)
        });
        return Action::Redraw;
    }
    Action::None
}

/// Read /ctl/procs + /ctl/cpu, derive %CPU + per-CPU util against the previous
/// poll, sort, store; refresh the detail pane if open.
fn resample(app: &mut App) {
    let elapsed_ns = app.last_sample.elapsed().as_nanos() as u64;
    app.last_sample = Instant::now();

    let mut rows = sample::parse_procs(&read_ctl_file("/ctl/procs"));
    app.sampler.update(&mut rows, elapsed_ns);
    app.sort.apply(&mut rows);
    app.rows = rows;
    app.reconcile_selection();

    // prowl-3c: per-CPU utilization (the meter denominator) -- one cheap /ctl/cpu
    // read per poll, diffed for util.
    let mut cpus = sample::parse_cpu(&read_ctl_file("/ctl/cpu"));
    app.cpu_sampler.update(&mut cpus, elapsed_ns);
    app.cpus = cpus;

    app.refresh_detail();
}

/// Slurp a /ctl or /proc text file into a String (the cpubench idiom: one bounded
/// buffer, read to EOF). "" on any error -- prowl degrades to an empty list
/// rather than failing.
fn read_ctl_file(path: &str) -> String {
    let mut f = match File::open(path) {
        Ok(f) => f,
        Err(_) => return String::new(),
    };
    let mut buf = [0u8; CTL_BUF];
    let mut total = 0usize;
    loop {
        if total >= buf.len() {
            break;
        }
        match f.read(&mut buf[total..]) {
            Ok(0) => break,
            Ok(k) => total += k,
            Err(_) => break,
        }
    }
    String::from(core::str::from_utf8(&buf[..total]).unwrap_or(""))
}

/// Write a control verb to /proc/<pid>/ctl. The kernel enforces authority: kill /
/// killgrp AND suspend / resume all take the SAME I-26 two-axis gate (owner OR
/// CAP_HOSTOWNER/CAP_KILL -- stopping is strictly weaker than killing); a denied
/// write returns -1 -> Err -> false. prowl confers no authority of its own -- a
/// confined user acts only on processes it already may kill.
fn ctl_write(pid: i64, cmd: &[u8]) -> bool {
    let path = format!("/proc/{}/ctl", pid);
    match OpenOptions::new().write(true).open(&path) {
        Ok(mut f) => f.write_all(cmd).is_ok(),
        Err(_) => false,
    }
}

/// Terminate `pid` by writing "kill" to /proc/<pid>/ctl (the confirm-gated action).
fn kill_pid(pid: i64) -> bool {
    ctl_write(pid, b"kill")
}

/// Parse `[-s cpu|pid|mem|name]` (the initial sort). Unknown flags are ignored.
fn parse_args() -> Sort {
    let mut sort = Sort::Cpu;
    let mut want_sort = false;
    for op in env::args().operands() {
        if want_sort {
            sort = match op {
                b"cpu" => Sort::Cpu,
                b"pid" => Sort::Pid,
                b"mem" => Sort::Mem,
                b"name" => Sort::Name,
                _ => sort,
            };
            want_sort = false;
        } else if op == b"-s" {
            want_sort = true;
        }
    }
    sort
}
