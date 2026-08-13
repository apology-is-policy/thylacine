// quarry -- the GPU demo-bench launcher: one place to hunt Quake with every
// renderer the box carries.
//
// The renderer matrix is DATA (`RENDERERS`): software tyr-quake, GL tyr-glquake
// on llvmpipe or on virgl (the /srv/warp hardware seam), and seam rows for the
// Vulkan pair (lavapipe + venus) that light up when Warp-6 stages a VK engine.
// Each row knows its binary, its GALLIUM_DRIVER value, and how to probe
// availability, so a new renderer is one table row.
//
// Three faces:
//   quarry                     the Kaua TUI menu (Enter play, d timedemo,
//                              b bench-all, r re-probe, q quit)
//   quarry <key> [args...]     CLI launch (inherited console), args passed
//                              through to the engine
//   quarry list | bench [demo] the agent-facing CLI: probe table / run the
//                              timedemo on every ready renderer and print a
//                              comparison table
//
// Driver selection rides the /env device: GALLIUM_DRIVER is written into
// quarry's OWN environment (a create+write of /env/GALLIUM_DRIVER), the child
// inherits a deep copy at spawn (env_clone_into), and the previous value is
// restored after. No spawn-ABI env argument exists (#151); this is the
// sanctioned inheritance route.
//
// CONSOLE DISCIPLINE (KAUA.md; the prowl/nora contract): quarry owns the
// SCREEN on fd 1 and reads keys on fd 0; it never touches consctl. ut sets raw
// termios before the spawn (quarry is in ut's is_raw_command set) and re-cooks
// on exit or crash. A PLAYED game takes the graphics surface (tapestry) and
// scribbles the text console with its own prints, so the TUI leaves the screen
// for the game's duration and redraws on reap; BENCH runs are piped, so the
// screen stays intact.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::{String, ToString};
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::Read;
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::time::Instant;
use libthyla_rs::{env, t_putstr};

use kaua::buffer::Buffer;
use kaua::event::{Event, KeyCode, KeyEvent};
use kaua::layout::{Constraint, Layout};
use kaua::rect::Rect;
use kaua::source::{EventSource, PollSource};
use kaua::style::{Attr, Color, Style};
use kaua::term::Terminal;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

const COLS: u16 = 80;
const ROWS: u16 = 24;
const MIN_DIM: u16 = 1;
const MAX_DIM: u16 = 1000;
const SIZE_QUERY_TIMEOUT_MS: u32 = 150;

/// One bench leg's ceiling: the slowest legitimate timedemo (llvmpipe at
/// 1280x800 runs ~16 fps -> ~60 s) plus engine start/asset load. A leg past
/// this is hung, not slow: the child is killed and the row says so.
const BENCH_DEADLINE_MS: u64 = 180_000;

const ENV_DRIVER: &str = "GALLIUM_DRIVER";
const ENV_DRIVER_PATH: &str = "/env/GALLIUM_DRIVER";

// ---------------------------------------------------------------------------
// The renderer matrix.
// ---------------------------------------------------------------------------

#[derive(Clone, Copy, PartialEq)]
enum Kind {
    Sw,
    Gl,
    Vk,
}

struct Renderer {
    key: &'static str,
    label: &'static str,
    bin: &'static str,
    /// The GALLIUM_DRIVER value this row pins. Explicit even where the build
    /// default would match (hw-gl), so a row never depends on fallback order
    /// -- the 0006 fork tries virgl first and falls back to llvmpipe LOUDLY,
    /// and a bench row that relied on that would mislabel a fallen-back run.
    driver: Option<&'static str>,
    kind: Kind,
}

const RENDERERS: &[Renderer] = &[
    Renderer {
        key: "sw",
        label: "software renderer  (tyr-quake)",
        // The post-pivot /bin bind (#58): ramfs bins live at /bin in the
        // session namespace, not the pre-pivot cpio root.
        bin: "/bin/tyr-quake",
        driver: None,
        kind: Kind::Sw,
    },
    Renderer {
        key: "llvmpipe",
        label: "OpenGL / llvmpipe  (software GL)",
        bin: "/clade/bin/tyr-glquake",
        driver: Some("llvmpipe"),
        kind: Kind::Gl,
    },
    Renderer {
        key: "hw-gl",
        label: "OpenGL / virgl     (hardware GL)",
        bin: "/clade/bin/tyr-glquake",
        driver: Some("virpipe"),
        kind: Kind::Gl,
    },
    // The Vulkan seam rows (Warp-6): they probe MISSING until a VK engine is
    // staged. The ICD selector (lavapipe vs venus) lands with that arc; the
    // rows exist now so the menu shape and CLI keys are stable.
    Renderer {
        key: "lavapipe",
        label: "Vulkan / lavapipe  (software VK)",
        bin: "/clade/bin/vkquake",
        driver: None,
        kind: Kind::Vk,
    },
    Renderer {
        key: "hw-vk",
        label: "Vulkan / venus     (hardware VK)",
        bin: "/clade/bin/vkquake",
        driver: None,
        kind: Kind::Vk,
    },
];

#[derive(Clone)]
enum Status {
    Ready,
    Missing(String),
}

fn probe(r: &Renderer) -> Status {
    if fs::metadata(r.bin).is_err() {
        return Status::Missing(match r.kind {
            Kind::Vk => "awaits Warp-6 (Venus)".to_string(),
            _ => "binary not staged".to_string(),
        });
    }
    if r.key == "hw-gl" {
        // The 3D device probe: /srv/warp's global ctl leads with
        // "virgl <0|1>". An absent service or a 2D device downgrades the row
        // honestly instead of letting the GL build fall back to llvmpipe
        // under a hardware label.
        match read_small("/srv/warp/ctl") {
            Some(s) if s.starts_with("virgl 1") => {}
            Some(_) => return Status::Missing("2D device (virgl 0)".to_string()),
            None => return Status::Missing("no /srv/warp 3D device".to_string()),
        }
    }
    Status::Ready
}

/// Slurp a small file (the prowl read_ctl_file idiom). None on any error.
fn read_small(path: &str) -> Option<String> {
    let mut f = File::open(path).ok()?;
    let mut buf = [0u8; 512];
    let mut total = 0usize;
    while total < buf.len() {
        match f.read(&mut buf[total..]) {
            Ok(0) => break,
            Ok(k) => total += k,
            Err(_) => return None,
        }
    }
    Some(String::from(core::str::from_utf8(&buf[..total]).ok()?))
}

// ---------------------------------------------------------------------------
// Driver env plumbing (/env inheritance at spawn).
// ---------------------------------------------------------------------------

/// Pin GALLIUM_DRIVER in OUR env (children inherit a copy) and hand back the
/// previous value for restore. Remove-then-create sidesteps any question of
/// devenv write-at-offset semantics: each set is a fresh value file.
fn driver_set(val: &str) -> Option<String> {
    let old = env::var(ENV_DRIVER);
    let _ = fs::remove_file(ENV_DRIVER_PATH);
    if let Ok(mut f) = File::create(ENV_DRIVER_PATH) {
        use libthyla_rs::io::Write;
        let _ = f.write_all(val.as_bytes());
    }
    old
}

fn driver_restore(old: Option<String>) {
    let _ = fs::remove_file(ENV_DRIVER_PATH);
    if let Some(v) = old {
        if let Ok(mut f) = File::create(ENV_DRIVER_PATH) {
            use libthyla_rs::io::Write;
            let _ = f.write_all(v.as_bytes());
        }
    }
}

// ---------------------------------------------------------------------------
// Launching.
// ---------------------------------------------------------------------------

/// The engine invocation every lane uses. -window is the tested tapestry
/// path; -nosound because no audio device exists (the engine probes forever
/// otherwise).
const BASE_ARGS: &[&str] = &["-window", "-nosound"];

/// Play interactively: inherited console, wait, return the exit status.
fn play(r: &Renderer, extra: &[String]) -> Result<i32, String> {
    let old = r.driver.map(driver_set);
    let mut cmd = Command::new(r.bin);
    for a in BASE_ARGS {
        cmd.arg(*a);
    }
    for a in extra {
        cmd.arg(a.as_str());
    }
    let res = (|| {
        let mut child = cmd.spawn().map_err(|e| format!("spawn: {:?}", e))?;
        let st = child.wait().map_err(|e| format!("wait: {:?}", e))?;
        Ok(st.code().unwrap_or(-1))
    })();
    if let Some(o) = old {
        driver_restore(o);
    }
    res
}

/// One bench leg's outcome. fps/renderer stay the engine's own strings --
/// re-formatting a measurement invites drift.
#[derive(Clone)]
struct Bench {
    key: &'static str,
    frames: String,
    secs: String,
    fps: String,
    gl_renderer: Option<String>,
    errors: usize,
    exit: i32,
    note: Option<&'static str>,
}

/// Run `+timedemo <demo>` piped and parse the engine's own report. Both pipes
/// drain through one PollSet so a chatty stderr can never deadlock the child
/// against a full pipe; a leg past BENCH_DEADLINE_MS is killed via
/// /proc/<pid>/ctl and reported hung.
fn bench_one(r: &Renderer, demo: &str) -> Result<Bench, String> {
    let old = r.driver.map(driver_set);
    let mut cmd = Command::new(r.bin);
    for a in BASE_ARGS {
        cmd.arg(*a);
    }
    cmd.arg("+timedemo").arg(demo);
    cmd.stdout(Stdio::Piped).stderr(Stdio::Piped);
    let spawned = cmd.spawn().map_err(|e| format!("spawn: {:?}", e));
    if let Some(o) = old {
        driver_restore(o);
    }
    let mut child = spawned?;
    let mut out = child.stdout.take();
    let mut errp = child.stderr.take();
    let mut out_buf: Vec<u8> = Vec::new();
    let mut err_buf: Vec<u8> = Vec::new();
    let started = Instant::now();
    let mut note = None;

    loop {
        if out.is_none() && errp.is_none() {
            break;
        }
        if started.elapsed().as_millis() as u64 >= BENCH_DEADLINE_MS {
            // Hung engine: kill it so the bench (and any TUI above it)
            // survives. The kernel's I-26 gate authorizes -- it is our child.
            let _ = write_ctl(child.pid(), b"kill");
            note = Some("hung: killed at the bench deadline");
            break;
        }
        let mut ps = PollSet::new();
        if let Some(f) = &out {
            ps.add(f, PollEvents::READ);
        }
        if let Some(f) = &errp {
            ps.add(f, PollEvents::READ);
        }
        match ps.poll(PollTimeout::Millis(2000)) {
            Ok(_) => {}
            Err(_) => break,
        }
        // Read whatever is ready; 0 = that side's EOF. A poll timeout falls
        // through to the deadline check -- reads on unready fds are avoided
        // by trying only after a poll round, and a spurious ready costs one
        // short read.
        drain_side(&mut out, &mut out_buf);
        drain_side(&mut errp, &mut err_buf);
    }
    let exit = child.wait().ok().and_then(|s| s.code()).unwrap_or(-1);

    let stdout = String::from_utf8_lossy(&out_buf).into_owned();
    let stderr = String::from_utf8_lossy(&err_buf).into_owned();
    let mut b = Bench {
        key: r.key,
        frames: String::new(),
        secs: String::new(),
        fps: String::new(),
        gl_renderer: None,
        errors: 0,
        exit,
        note,
    };
    for line in stdout.lines().chain(stderr.lines()) {
        if let Some(rest) = line.trim().strip_prefix("GL_RENDERER:") {
            if b.gl_renderer.is_none() {
                b.gl_renderer = Some(rest.trim().to_string());
            }
        }
        if line.contains("GL_OUT_OF_MEMORY") || line.contains("Mesa: error") {
            b.errors += 1;
        }
        // "969 frames  21.7 seconds  44.7 fps" -- the engine's timedemo line.
        if line.contains(" frames") && line.trim_end().ends_with("fps") {
            let toks: Vec<&str> = line.split_whitespace().collect();
            if toks.len() >= 6 && toks[1] == "frames" && toks[5] == "fps" {
                b.frames = toks[0].to_string();
                b.secs = toks[2].to_string();
                b.fps = toks[4].to_string();
            }
        }
    }
    Ok(b)
}

fn drain_side(side: &mut Option<File>, buf: &mut Vec<u8>) {
    if let Some(f) = side {
        let mut tmp = [0u8; 4096];
        match f.read(&mut tmp) {
            Ok(0) => *side = None,
            Ok(n) => buf.extend_from_slice(&tmp[..n]),
            Err(_) => {}
        }
    }
}

fn write_ctl(pid: i32, verb: &[u8]) -> bool {
    use libthyla_rs::io::Write;
    match fs::OpenOptions::new()
        .write(true)
        .open(&format!("/proc/{}/ctl", pid))
    {
        Ok(mut f) => f.write_all(verb).is_ok(),
        Err(_) => false,
    }
}

// ---------------------------------------------------------------------------
// The CLI face.
// ---------------------------------------------------------------------------

fn usage() -> i32 {
    t_putstr(
        "quarry -- the GPU demo-bench launcher\n\
         usage:\n\
         \x20 quarry                  interactive menu (Kaua TUI)\n\
         \x20 quarry list             probe the renderer matrix\n\
         \x20 quarry bench [demo]     timedemo every ready renderer (default demo1)\n\
         \x20 quarry <key> [args...]  launch one renderer; keys: sw llvmpipe hw-gl\n\
         \x20                         lavapipe hw-vk (VK rows await Warp-6)\n",
    );
    2
}

fn cli_list() -> i32 {
    for r in RENDERERS {
        let st = probe(r);
        let line = match st {
            Status::Ready => format!("{:<9} ready    {}  [{}]\n", r.key, r.label, r.bin),
            Status::Missing(m) => format!("{:<9} missing  {}  ({})\n", r.key, r.label, m),
        };
        t_putstr(&line);
    }
    0
}

fn cli_bench(demo: &str) -> i32 {
    let mut results: Vec<Bench> = Vec::new();
    for r in RENDERERS {
        match probe(r) {
            Status::Ready => {
                t_putstr(&format!("quarry: benching {} ({})...\n", r.key, demo));
                match bench_one(r, demo) {
                    Ok(b) => results.push(b),
                    Err(e) => {
                        t_putstr(&format!("quarry: {} failed: {}\n", r.key, e));
                    }
                }
            }
            Status::Missing(m) => {
                t_putstr(&format!("quarry: skipping {} ({})\n", r.key, m));
            }
        }
    }
    t_putstr("\nrenderer   frames   seconds   fps      errors  exit  backend\n");
    for b in &results {
        t_putstr(&format!(
            "{:<9}  {:>6}  {:>8}  {:>7}  {:>6}  {:>4}  {}{}\n",
            b.key,
            none_dash(&b.frames),
            none_dash(&b.secs),
            none_dash(&b.fps),
            b.errors,
            b.exit,
            b.gl_renderer.as_deref().unwrap_or("(software)"),
            b.note.map(|n| format!("  [{}]", n)).unwrap_or_default(),
        ));
    }
    if results.iter().any(|b| b.fps.is_empty()) {
        1
    } else {
        0
    }
}

fn none_dash(s: &str) -> &str {
    if s.is_empty() {
        "-"
    } else {
        s
    }
}

// ---------------------------------------------------------------------------
// The TUI face.
// ---------------------------------------------------------------------------

fn ember() -> Style {
    Style::new().fg(Color::Rgb(0xE0, 0x78, 0x40)).attr(Attr::BOLD)
}
fn dim() -> Style {
    Style::new().fg(Color::Rgb(0x9a, 0x8f, 0x86))
}
fn normal() -> Style {
    Style::new().fg(Color::Rgb(0xd8, 0xcf, 0xc6))
}
fn selected() -> Style {
    Style::new().attr(Attr::REVERSE)
}
struct App {
    rows: Vec<(usize, Status, Option<Bench>)>,
    sel: usize,
    status: Option<String>,
}

impl App {
    fn new() -> App {
        let mut app = App {
            rows: Vec::new(),
            sel: 0,
            status: None,
        };
        app.reprobe();
        app
    }
    fn reprobe(&mut self) {
        let old: Vec<Option<Bench>> = if self.rows.is_empty() {
            RENDERERS.iter().map(|_| None).collect()
        } else {
            self.rows.drain(..).map(|(_, _, b)| b).collect()
        };
        for (i, r) in RENDERERS.iter().enumerate() {
            self.rows.push((i, probe(r), old.get(i).cloned().flatten()));
        }
    }
}

fn render(term: &mut Terminal, app: &App) -> libthyla_rs::err::Result<()> {
    let area = term.area();
    {
        let buf = term.back_mut();
        buf.reset();
        let chunks = Layout::vertical(&[
            Constraint::Length(2),
            Constraint::Min(1),
            Constraint::Length(1),
        ])
        .split(area);
        render_header(buf, chunks[0]);
        render_rows(buf, chunks[1], app);
        render_footer(buf, chunks[2], app);
    }
    term.flush()
}

fn render_header(buf: &mut Buffer, area: Rect) {
    let x = buf.set_str(area.x, area.y, "quarry", ember());
    buf.set_str(x, area.y, "   the GPU demo bench", dim());
}

fn render_rows(buf: &mut Buffer, area: Rect, app: &App) {
    let mut y = area.y;
    for (i, (ri, st, bench)) in app.rows.iter().enumerate() {
        if y >= area.y + area.height {
            break;
        }
        let r = &RENDERERS[*ri];
        let row_style = if i == app.sel { selected() } else { normal() };
        let mut x = buf.set_str(area.x + 1, y, &format!("{:<9}", r.key), row_style);
        x = buf.set_str(x, y, &format!(" {}", r.label), row_style);
        let tail = match st {
            Status::Ready => match bench {
                Some(b) if !b.fps.is_empty() => format!(
                    "  {} fps{}",
                    b.fps,
                    if b.errors > 0 {
                        format!(" ({} errors)", b.errors)
                    } else {
                        String::new()
                    }
                ),
                Some(b) if b.note.is_some() => format!("  [{}]", b.note.unwrap()),
                _ => "  ready".to_string(),
            },
            Status::Missing(m) => format!("  ({})", m),
        };
        let tail_style = if i == app.sel { selected() } else { dim() };
        buf.set_str(x, y, &tail, tail_style);
        y += 1;
    }
}

fn render_footer(buf: &mut Buffer, area: Rect, app: &App) {
    let text = match &app.status {
        Some(s) => s.clone(),
        None => "Enter play   d timedemo   b bench all   r re-probe   q quit".to_string(),
    };
    buf.set_str(area.x, area.y, &text, dim());
}

enum Action {
    None,
    Redraw,
    Quit,
    Play,
    Demo,
    BenchAll,
}

fn handle_key(app: &mut App, k: KeyEvent) -> Action {
    match k.code {
        KeyCode::Char('q') | KeyCode::Esc => Action::Quit,
        KeyCode::Up => {
            if app.sel > 0 {
                app.sel -= 1;
            }
            Action::Redraw
        }
        KeyCode::Down => {
            if app.sel + 1 < app.rows.len() {
                app.sel += 1;
            }
            Action::Redraw
        }
        KeyCode::Enter => Action::Play,
        KeyCode::Char('d') => Action::Demo,
        KeyCode::Char('b') => Action::BenchAll,
        KeyCode::Char('r') => {
            app.reprobe();
            app.status = Some("re-probed".to_string());
            Action::Redraw
        }
        _ => Action::None,
    }
}

fn sel_ready(app: &App) -> Option<&'static Renderer> {
    let (ri, st, _) = &app.rows[app.sel];
    match st {
        Status::Ready => Some(&RENDERERS[*ri]),
        Status::Missing(_) => None,
    }
}

fn tui() -> i32 {
    let probe_q = kaua::query::terminal_size(SIZE_QUERY_TIMEOUT_MS);
    let (cols, rows) = probe_q
        .size
        .map(|(c, r)| (c.clamp(MIN_DIM, MAX_DIM), r.clamp(MIN_DIM, MAX_DIM)))
        .unwrap_or((COLS, ROWS));
    let area = Rect::new(0, 0, cols, rows);

    let mut term = match Terminal::enter(area) {
        Ok(t) => t,
        Err(_) => {
            t_putstr("quarry: cannot acquire the console screen\n");
            return 1;
        }
    };
    let mut src = PollSource::with_pending(probe_q.pending);
    let mut app = App::new();

    let code = loop {
        if render(&mut term, &app).is_err() {
            break 1;
        }
        if src.is_eof() {
            break 0;
        }
        let events = match src.poll(PollTimeout::Block) {
            Ok(e) => e,
            Err(_) => break 1,
        };
        let mut quit = false;
        for ev in events {
            match ev {
                Event::Key(k) => match handle_key(&mut app, k) {
                    Action::Quit => quit = true,
                    Action::Play => {
                        if let Some(r) = sel_ready(&app) {
                            // The game owns the console for its run: leave the
                            // screen (re-cooked output scrolls naturally),
                            // re-enter + full redraw on reap.
                            let _ = term.leave();
                            t_putstr(&format!("quarry: launching {}...\n", r.key));
                            let res = play(r, &[]);
                            term = match Terminal::enter(area) {
                                Ok(t) => t,
                                Err(_) => return 1,
                            };
                            app.status = Some(match res {
                                Ok(c) => format!("{} exited {}", r.key, c),
                                Err(e) => format!("{}: {}", r.key, e),
                            });
                        } else {
                            app.status = Some("selection not available".to_string());
                        }
                    }
                    Action::Demo => {
                        if let Some(r) = sel_ready(&app) {
                            app.status = Some(format!("timedemo on {} (piped)...", r.key));
                            let _ = render(&mut term, &app);
                            let b = bench_one(r, "demo1");
                            let sel = app.sel;
                            match b {
                                Ok(b) => {
                                    app.status = Some(format!(
                                        "{}: {} fps{}",
                                        r.key,
                                        none_dash(&b.fps),
                                        b.note.map(|n| format!(" [{}]", n)).unwrap_or_default()
                                    ));
                                    app.rows[sel].2 = Some(b);
                                }
                                Err(e) => app.status = Some(format!("{}: {}", r.key, e)),
                            }
                        } else {
                            app.status = Some("selection not available".to_string());
                        }
                    }
                    Action::BenchAll => {
                        for i in 0..app.rows.len() {
                            let (ri, st, _) = &app.rows[i];
                            let r = &RENDERERS[*ri];
                            if !matches!(st, Status::Ready) {
                                continue;
                            }
                            app.status = Some(format!("benching {}...", r.key));
                            let _ = render(&mut term, &app);
                            if let Ok(b) = bench_one(r, "demo1") {
                                app.rows[i].2 = Some(b);
                            }
                        }
                        app.status = Some("bench complete".to_string());
                    }
                    Action::Redraw => {}
                    Action::None => {}
                },
                Event::Resize(c, r) => {
                    let c = c.clamp(MIN_DIM, MAX_DIM);
                    let r = r.clamp(MIN_DIM, MAX_DIM);
                    if (c, r) != (term.area().width, term.area().height) {
                        term.resize(Rect::new(0, 0, c, r));
                    }
                }
                _ => {}
            }
        }
        if quit {
            break 0;
        }
    };
    let _ = term.leave();
    code
}

// ---------------------------------------------------------------------------
// Entry.
// ---------------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();
    let mut ops: Vec<String> = Vec::new();
    for op in args.operands() {
        if let Ok(s) = core::str::from_utf8(op) {
            ops.push(s.to_string());
        }
    }
    let code = match ops.first().map(|s| s.as_str()) {
        None => tui(),
        Some("list") => cli_list(),
        Some("bench") => cli_bench(ops.get(1).map(|s| s.as_str()).unwrap_or("demo1")),
        Some("help") | Some("-h") | Some("--help") => usage(),
        Some(key) => match RENDERERS.iter().find(|r| r.key == key) {
            Some(r) => match probe(r) {
                Status::Ready => match play(r, &ops[1..]) {
                    Ok(c) => c,
                    Err(e) => {
                        t_putstr(&format!("quarry: {}: {}\n", key, e));
                        1
                    }
                },
                Status::Missing(m) => {
                    t_putstr(&format!("quarry: {} not available: {}\n", key, m));
                    1
                }
            },
            None => usage(),
        },
    };
    code as i64
}
