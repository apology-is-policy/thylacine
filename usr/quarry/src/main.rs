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
//                              comparison table. A trailing leg list selects,
//                              orders and sizes the legs (`hw-gl@640x480`).
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
use libthyla_rs::poll::PollTimeout;
use libthyla_rs::process::Command;
use libthyla_rs::time::{self, Duration, Instant};
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
/// A timedemo leg's bound. The existing glq-bench lane allows 1800 s for the
/// same demo on these hosts, so 180 s was never a hang bound -- it was a
/// bound below the honest runtime, which reports a healthy leg as hung.
const BENCH_DEADLINE_MS: u64 = 600_000;

/// The engine's own console log. `-condebug` routes every `Con_Printf`
/// through `Sys_DebugLog`, which is a bare open(O_APPEND)/write/close per
/// line -- no stdio buffer anywhere in the path. That is the whole reason
/// this file exists rather than a pipe: `+timedemo` does NOT quit the
/// engine when the demo ends, so a piped stdout holds the fps line in the
/// child's 4-KiB buffer until a process exit that never comes. build.sh
/// chmod 0777s the gamedir precisely so the session user can write here.
const QCONSOLE_LOG: &str = "/quake/id1/qconsole.log";

const ENV_DRIVER: &str = "GALLIUM_DRIVER";
const ENV_DRIVER_PATH: &str = "/env/GALLIUM_DRIVER";

const ENV_NOPACE: &str = "SDL_THYLACINE_NOPACE";
const ENV_NOPACE_PATH: &str = "/env/SDL_THYLACINE_NOPACE";

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
        // under a hardware label. TWO-STEP (connect the service root, then
        // open ctl RELATIVE to it) -- opening /srv/warp is a CONNECT and a
        // single-shot walk of /srv/warp/ctl does not compose through it
        // (joey's probe and warp_client both use this shape; the one-open
        // form probed "missing" on a live 3D boot).
        match read_warp_ctl() {
            Some(s) if s.starts_with("virgl 1") => {}
            Some(_) => return Status::Missing("2D device (virgl 0)".to_string()),
            None => return Status::Missing("no /srv/warp 3D device".to_string()),
        }
    }
    Status::Ready
}

fn read_warp_ctl() -> Option<String> {
    use libthyla_rs::{t_close, t_open, t_pread, T_OREAD, T_WALK_OPEN_FROM_ROOT};
    let path = b"/srv/warp";
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
    if root < 0 {
        return None;
    }
    let ctl = b"ctl";
    let fd = unsafe { t_open(root, ctl.as_ptr(), ctl.len(), T_OREAD) };
    if fd < 0 {
        unsafe { t_close(root) };
        return None;
    }
    let mut buf = [0u8; 256];
    let n = unsafe { t_pread(fd, buf.as_mut_ptr(), buf.len() - 1, 0) };
    unsafe {
        t_close(fd);
        t_close(root);
    }
    if n <= 0 {
        return None;
    }
    Some(String::from_utf8_lossy(&buf[..n as usize]).into_owned())
}

// ---------------------------------------------------------------------------
// Driver env plumbing (/env inheritance at spawn).
// ---------------------------------------------------------------------------

/// Pin GALLIUM_DRIVER in OUR env (children inherit a copy) and hand back the
/// previous value for restore. Remove-then-create sidesteps any question of
/// devenv write-at-offset semantics: each set is a fresh value file.
fn env_set(name: &str, path: &str, val: &str) -> Option<String> {
    let old = env::var(name);
    let _ = fs::remove_file(path);
    if let Ok(mut f) = File::create(path) {
        use libthyla_rs::io::Write;
        let _ = f.write_all(val.as_bytes());
    }
    old
}

fn env_restore(path: &str, old: Option<String>) {
    let _ = fs::remove_file(path);
    if let Some(v) = old {
        if let Ok(mut f) = File::create(path) {
            use libthyla_rs::io::Write;
            let _ = f.write_all(v.as_bytes());
        }
    }
}

fn driver_set(val: &str) -> Option<String> {
    env_set(ENV_DRIVER, ENV_DRIVER_PATH, val)
}

fn driver_restore(old: Option<String>) {
    env_restore(ENV_DRIVER_PATH, old)
}

// ---------------------------------------------------------------------------
// Launching.
// ---------------------------------------------------------------------------

/// The engine invocation for a PLAYED game: -window is the tested tapestry
/// path; sound is on (Nocturne N-2a-3 -- SDL's thylacine audio driver, or its
/// dummy fallback on a soundless machine, so the engine never probes forever).
const PLAY_ARGS: &[&str] = &["-window"];

/// A BENCH leg keeps -nosound: the audio thread's blocking writes and the
/// mixer's interrupt load would perturb the fps figure, and a benchmark's
/// number must stay a property of the renderer, not of the sound path.
const BENCH_ARGS: &[&str] = &["-window", "-nosound"];

/// One bench leg: a renderer row plus the resolution to run it at.
///
/// Per-leg rather than per-run so ONE command sweeps a single renderer across
/// modes. That is the instrument that separates the two candidate explanations
/// for the GL deficit: a FILL-bound path's fps tracks 1/pixels, a
/// PER-SUBMIT-bound path's barely moves. Keeping the sweep inside one boot
/// also holds the boot constant, and repeating a resolution as the last leg
/// measures any within-boot drift (#168) instead of assuming its absence.
struct Leg {
    r: &'static Renderer,
    /// Whether the SDL frame pacer stays ON for this leg.
    ///
    /// A bench leg defaults to UNPACED, because a paced present blocks on the
    /// compositor's FRAME tick and the number stops being a property of the
    /// renderer: it becomes a property of the clock. That is what
    /// SDL_THYLACINE_NOPACE exists for ("benchmarks", per the pacer's own
    /// comment), and it is what every other bench lane already does via the
    /// rp5/rp6/rp7 wrappers -- quarry not doing it made its figures
    /// non-comparable with all of them.
    ///
    /// `:paced` opts back in, because the paced number is the one a USER
    /// actually sees. The pair is the measurement: unpaced is renderer
    /// throughput, paced is delivered frame rate, and the gap between them is
    /// the compositor's contribution.
    paced: bool,
    /// None leaves the engine at its own windowed default -- 640x480, seeded
    /// by sdl_common.c's SDL_Init path, NOT by the vid_width/vid_height cvars
    /// (which default to 800x600 and are never consulted once -window makes
    /// VID_GetCmdlineMode answer).
    res: Option<(u32, u32)>,
}

impl Leg {
    /// The leg's name everywhere it is reported. Carrying the resolution in
    /// the label keeps a sweep's table rows self-describing, so a pasted
    /// result cannot lose the one variable it was varying.
    fn label(&self) -> String {
        let base = match self.res {
            Some((w, h)) => format!("{}@{}x{}", self.r.key, w, h),
            None => self.r.key.to_string(),
        };
        if self.paced {
            format!("{}:paced", base)
        } else {
            base
        }
    }
}

/// `key` or `key@WxH`.
///
/// An explicit resolution reaches the engine as `-width W -height H`. Those
/// are honoured for any size because both arg sets already pass `-window`:
/// VID_GetCmdlineMode writes the request straight into vid_windowed_mode and
/// returns, where the fullscreen path would instead search the modelist and
/// Sys_Error on a miss.
fn parse_leg(spec: &str) -> Result<Leg, String> {
    let (spec, paced) = match spec.strip_suffix(":paced") {
        Some(rest) => (rest, true),
        None => (spec, false),
    };
    let (key, res) = match spec.split_once('@') {
        None => (spec, None),
        Some((k, dims)) => {
            let (w, h) = dims
                .split_once('x')
                .ok_or_else(|| format!("resolution '{}' is not WxH", dims))?;
            let w: u32 = w.parse().map_err(|_| format!("bad width '{}'", w))?;
            let h: u32 = h.parse().map_err(|_| format!("bad height '{}'", h))?;
            if w == 0 || h == 0 {
                return Err(format!("resolution {}x{} has a zero axis", w, h));
            }
            (k, Some((w, h)))
        }
    };
    match RENDERERS.iter().find(|r| r.key == key) {
        Some(r) => Ok(Leg { r, res, paced }),
        None => Err(format!("unknown renderer '{}'", key)),
    }
}

/// Play interactively: inherited console, wait, return the exit status.
fn play(r: &Renderer, extra: &[String]) -> Result<i32, String> {
    let old = r.driver.map(driver_set);
    let mut cmd = Command::new(r.bin);
    for a in PLAY_ARGS {
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
    key: String,
    frames: String,
    secs: String,
    fps: String,
    gl_renderer: Option<String>,
    errors: usize,
    exit: i32,
    note: Option<&'static str>,
}

/// Run `+timedemo <demo>` and read the engine's own `-condebug` log, which
/// `Sys_DebugLog` writes with a bare open/write/close per line -- so no stdio
/// buffer can withhold the report from a process that never exits. A leg past
/// BENCH_DEADLINE_MS is killed and reported hung.
fn bench_one(leg: &Leg, demo: &str) -> Result<Bench, String> {
    let r = leg.r;
    let tag = leg.label();

    // Start from a clean log so we read THIS leg's lines, not the last one's.
    let _ = fs::remove_file(QCONSOLE_LOG);

    let old = r.driver.map(driver_set);
    // Unpaced unless the leg asked otherwise: see Leg::paced. Set around the
    // spawn exactly like the driver, so the child inherits it at
    // env_clone_into and quarry's own environment is left as it was found.
    let old_pace = if leg.paced {
        None
    } else {
        Some(env_set(ENV_NOPACE, ENV_NOPACE_PATH, "1"))
    };
    let mut cmd = Command::new(r.bin);
    for a in BENCH_ARGS {
        cmd.arg(*a);
    }
    // Built before the loop because Command borrows each argument: a
    // format!() passed inline would be dropped at the end of the statement.
    let mut res_args: Vec<String> = Vec::new();
    if let Some((w, h)) = leg.res {
        res_args.push("-width".to_string());
        res_args.push(format!("{}", w));
        res_args.push("-height".to_string());
        res_args.push(format!("{}", h));
    }
    for a in &res_args {
        cmd.arg(a.as_str());
    }
    cmd.arg("-condebug");
    // Make the leg WITNESS its own resolution instead of trusting the flag.
    // A per-submit-bound renderer and a `-width` that never took effect
    // produce the identical signature -- a flat fps curve -- so a sweep that
    // only records what it REQUESTED cannot tell the measurement from the
    // bug. This console command puts the mode the engine actually selected
    // into the same log the fps line lands in.
    cmd.arg("+vid_describecurrentmode");
    cmd.arg("+timedemo").arg(demo);
    // stdout/stderr stay INHERITED: the console shows progress live, and
    // nothing here depends on reading them. Piping them was the #231 bug --
    // the drain called read() on fds a timed-out poll had NOT reported
    // ready, so it blocked in the callee and the deadline below, written on
    // the outer loop, was never evaluated again.
    let spawned = cmd.spawn().map_err(|e| format!("spawn: {:?}", e));
    if let Some(o) = old {
        driver_restore(o);
    }
    if let Some(o) = old_pace {
        env_restore(ENV_NOPACE_PATH, o);
    }
    let mut child = spawned?;
    t_putstr(&format!(
        "quarry: {} pace-witness {}\n",
        tag,
        if leg.paced {
            "PACED (blocks on the compositor FRAME tick)"
        } else {
            "unpaced (SDL_THYLACINE_NOPACE=1)"
        }
    ));
    // Announce the pid at SPAWN, not just at the kill: the poll loop below
    // prints nothing while it runs, so a stall inside it leaves no pid behind
    // to inspect -- which is exactly what happened the first time this hung.
    t_putstr(&format!("quarry: {} spawned pid={}\n", tag, child.pid()));

    let started = Instant::now();
    let mut note = None;
    let mut log;
    loop {
        log = read_text(QCONSOLE_LOG).unwrap_or_default();
        if fps_line(&log).is_some() {
            break;
        }
        // The engine outliving its own demo is NORMAL here, so a leg ends by
        // our clock or by our kill -- never by waiting for an exit.
        if let Ok(Some(_)) = child.try_wait() {
            note = Some("engine exited before reporting a timedemo");
            break;
        }
        if started.elapsed().as_millis() as u64 >= BENCH_DEADLINE_MS {
            note = Some("hung: killed at the bench deadline");
            break;
        }
        let _ = time::sleep(Duration::from_millis(500));
    }

    // When a leg ends WITHOUT an fps line, say what we actually read. The
    // whole question in that case is whether the engine's line is missing
    // from the log or merely unmatched by the parser, and only the bytes we
    // saw can tell those apart -- an empty log and a full one that failed to
    // parse are the same silence otherwise.
    if note.is_some() {
        // `present` is reported separately from `bytes` on purpose: an absent
        // file and an empty one both read as 0 bytes, and they accuse
        // different things -- the engine never opened the log at all, versus
        // it opened it and wrote nothing.
        let present = fs::File::open(QCONSOLE_LOG).is_ok();
        let last = log.lines().last().unwrap_or("");
        t_putstr(&format!(
            "quarry: {} log-at-end present={} bytes={} last_line={:?}\n",
            tag,
            present,
            log.len(),
            last
        ));
    }

    // `+timedemo` leaves the engine running, so every leg ends with a kill.
    // The markers bracketing the kill are load-bearing, not chatter: a wedge
    // in here emits nothing at all, and the LAST line printed is the only
    // thing that says which step blocked. `kill` is an unbounded
    // /proc/<pid>/ctl write; the reap below is bounded by its own loop.
    t_putstr(&format!("quarry: {} kill-begin pid={}\n", tag, child.pid()));
    let killed = child.kill();
    t_putstr(&format!(
        "quarry: {} kill-end {}\n",
        tag,
        if killed.is_ok() { "ok" } else { "err" }
    ));
    let mut exit = -1;
    for _ in 0..40 {
        match child.try_wait() {
            Ok(Some(st)) => {
                exit = st.code().unwrap_or(-1);
                break;
            }
            Ok(None) => {
                let _ = time::sleep(Duration::from_millis(50));
            }
            Err(_) => break,
        }
    }
    t_putstr(&format!("quarry: {} reaped exit={}\n", tag, exit));
    // Re-read after the kill: up to one poll interval of lines can land
    // between the last look and the kill, and the fps line is often the last
    // thing written.
    if let Some(t) = read_text(QCONSOLE_LOG) {
        log = t;
    }

    let mut b = Bench {
        key: tag,
        frames: String::new(),
        secs: String::new(),
        fps: String::new(),
        gl_renderer: None,
        errors: 0,
        exit,
        note,
    };
    if let Some((frames, secs, fps)) = fps_line(&log) {
        b.frames = frames;
        b.secs = secs;
        b.fps = fps;
    }
    // The resolution witness. Reported for EVERY leg, including the ones that
    // requested nothing -- for those it is the only statement of what the
    // engine's own default actually is, which a sweep is otherwise obliged to
    // assume. A disagreement is called out by name because the failure it
    // guards against is silent: an unhonoured -width yields a flat fps curve,
    // which is exactly the result the sweep is looking for.
    match mode_line(&log) {
        Some((w, h)) => {
            let agrees = leg.res.map(|(rw, rh)| rw == w && rh == h).unwrap_or(true);
            t_putstr(&format!(
                "quarry: {} mode-witness {}x{}{}\n",
                b.key,
                w,
                h,
                if agrees { "" } else { "  MISMATCH vs requested" }
            ));
        }
        None => {
            t_putstr(&format!(
                "quarry: {} mode-witness ABSENT (resolution unverified)\n",
                b.key
            ));
        }
    }
    for line in log.lines() {
        if let Some(rest) = line.trim().strip_prefix("GL_RENDERER:") {
            if b.gl_renderer.is_none() {
                b.gl_renderer = Some(rest.trim().to_string());
            }
        }
        if line.contains("GL_OUT_OF_MEMORY") || line.contains("Mesa: error") {
            b.errors += 1;
        }
    }
    Ok(b)
}

/// Slurp a regular file. Reads on a regular file always make progress, so
/// unlike the pipe drain this replaced, it cannot outlive a caller's bound.
fn read_text(path: &str) -> Option<String> {
    let mut f = fs::File::open(path).ok()?;
    let mut buf: Vec<u8> = Vec::new();
    let mut tmp = [0u8; 4096];
    loop {
        match f.read(&mut tmp) {
            Ok(0) => break,
            Ok(n) => buf.extend_from_slice(&tmp[..n]),
            Err(_) => break,
        }
    }
    Some(String::from_utf8_lossy(&buf).into_owned())
}

/// " 640 x  480 windowed" -> (640, 480), the mode the engine actually chose.
/// Emitted by `+vid_describecurrentmode`; the widths are %4d-padded, so parse
/// by tokens rather than by column.
fn mode_line(text: &str) -> Option<(u32, u32)> {
    for line in text.lines() {
        let toks: Vec<&str> = line.split_whitespace().collect();
        if toks.len() < 4 || toks[1] != "x" || toks[3] != "windowed" {
            continue;
        }
        if let (Ok(w), Ok(h)) = (toks[0].parse::<u32>(), toks[2].parse::<u32>()) {
            return Some((w, h));
        }
    }
    None
}

/// "969 frames  21.7 seconds  44.7 fps" -> (frames, seconds, fps).
fn fps_line(text: &str) -> Option<(String, String, String)> {
    for line in text.lines() {
        let t = line.trim_end();
        if !t.ends_with("fps") || !t.contains(" frames") {
            continue;
        }
        let toks: Vec<&str> = t.split_whitespace().collect();
        if toks.len() >= 6 && toks[1] == "frames" && toks[5] == "fps" {
            return Some((toks[0].into(), toks[2].into(), toks[4].into()));
        }
    }
    None
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
         \x20 quarry bench <demo> <leg>...  bench only these, in this order\n\
         \x20                         a leg is <key>[@<W>x<H>][:paced], so one\n\
         \x20                         command sweeps a renderer across modes:\n\
         \x20                         quarry bench demo1 hw-gl@320x240 hw-gl@1280x800\n\
         \x20                         bench legs are UNPACED by default (a paced\n\
         \x20                         present waits on the compositor tick, so the\n\
         \x20                         number stops being the renderer's); :paced\n\
         \x20                         opts back in to measure delivered fps\n\
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

fn cli_bench(demo: &str, specs: &[String]) -> i32 {
    // An explicit leg list SELECTS, ORDERS and SIZES the legs. All three are
    // load-bearing: order for #168 (within-boot degradation makes a late leg
    // read low, so a renderer must be measurable first) and #232 (whether a
    // symptom follows the GL leg or merely the LAST leg -- undecidable while
    // GL is always last); size for the resolution sweep, where repeating one
    // renderer at several modes IN ONE BOOT holds the boot constant.
    let mut legs: Vec<Leg> = Vec::new();
    if specs.is_empty() {
        legs.extend(RENDERERS.iter().map(|r| Leg { r, res: None, paced: false }));
    } else {
        for s in specs {
            match parse_leg(s) {
                Ok(leg) => legs.push(leg),
                Err(e) => {
                    t_putstr(&format!("quarry: {}\n", e));
                    return usage();
                }
            }
        }
    }
    let mut results: Vec<Bench> = Vec::new();
    for leg in &legs {
        let tag = leg.label();
        match probe(leg.r) {
            Status::Ready => {
                t_putstr(&format!("quarry: benching {} ({})...\n", tag, demo));
                match bench_one(leg, demo) {
                    Ok(b) => results.push(b),
                    Err(e) => {
                        t_putstr(&format!("quarry: {} failed: {}\n", tag, e));
                    }
                }
            }
            Status::Missing(m) => {
                t_putstr(&format!("quarry: skipping {} ({})\n", tag, m));
            }
        }
    }
    t_putstr("\nrenderer        frames   seconds   fps      errors  exit  backend\n");
    for b in &results {
        t_putstr(&format!(
            "{:<14}  {:>6}  {:>8}  {:>7}  {:>6}  {:>4}  {}{}\n",
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
                            // The TUI always benches at the engine default;
                            // a per-leg resolution is a CLI-sweep affordance.
                            let b = bench_one(&Leg { r, res: None, paced: false }, "demo1");
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
                            if let Ok(b) = bench_one(&Leg { r, res: None, paced: false }, "demo1") {
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
        Some("bench") => cli_bench(
            ops.get(1).map(|s| s.as_str()).unwrap_or("demo1"),
            if ops.len() > 2 { &ops[2..] } else { &[] },
        ),
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
