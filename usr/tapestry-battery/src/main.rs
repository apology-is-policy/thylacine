// /bin/tapestry-battery -- the G-6 compositor acceptance battery's
// in-guest half (TAPESTRY.md section 18.9, the G-6 gate: "the acceptance
// battery on synthetic clients"). One process hosts BOTH synthetic
// clients (two private libtapestry sessions) AND the layout driver (a
// third session writing the `layout` file), so the scenario is
// deterministic and single-threaded; the host half (the ls-gfx-panes
// expect scenario) types keys over QMP and pixel-asserts screendumps at
// the coordinates this binary prints.
//
// The legs, in order (each "battery: <stage> ..." line is an exp sync
// point; pixel stages sleep ~1.5 s so the host dump lands on a static
// screen):
//   focus event : TEV_FOCUS gained arrives at A's host-at-create (G-6c);
//   structure   : layout text vs pane geometry files, disjoint rects;
//   stage1      : A RED + B BLUE center pixels (the G-6a compose blit);
//   resize      : the 18.3 protocol (G-6b) -- ack negative probes, then
//                 B reweaves onto its pane's exact size;
//   multirect   : ONE present carrying TWO rects paints B's halves green
//                 + yellow -- both quarter points must land (G-6c);
//   multirect-v : the same, split TOP/BOTTOM (Warp-C C-3): a vertical
//                 asymmetry that a mirrored or displaced composition blit
//                 cannot fake -- probed, not dumped;
//   tabbed      : mode tabbed on [A/B] -- A hides, the D7 glyph-free
//                 strip paints (segment colors sampled), `tab next`
//                 cycles the active child (G-6c); the revealed A then
//                 presents red and its center is probed (the C-2d redraw
//                 contract on the composed path, Warp-C C-3);
//   zoom        : the focused-pane zoom toggle -- A alone at full
//                 display (the direct-scanout path), then restore;
//   move        : directional re-parenting (D6) -- B pulls out of the
//                 nested splitv beside it, then swaps right (G-6c);
//   focus legs  : QMP-typed keys arrive on the FOCUSED surface only;
//   chord       : QMP Super+Left moves focus compositor-side -- A gets
//                 TEV_FOCUS gained and B never sees the arrow KEY (the
//                 section 18.4 interception) (G-6c);
//   test-mode   : the section 18.6 determinism mode -- the FRAME clock
//                 freezes and `tick` drives it (G-6c);
//   hold        : TPRESENT_HOLD defers the scanout push until release
//                 (B stays blue on screen with magenta already blitted;
//                 release flips it) (G-6c);
//   close       : a compositor pane close delivers TEV_CLOSE (G-6b).
//
// Clipping is exercised deliberately: A (display-sized) is larger than
// its pane -> the compose blit crops it; solid fills keep the pixel
// asserts exact either way.
//
// Every pixel stage also PROBES its sample points through the compositor's
// `probe-screen` verb (Warp-C C-3): tapestryd reads the texel back from the
// composed screen -- the resource itself on a GL host, the buffer on the
// 2D device -- and says it, so the same coordinates the host dumps here are
// asserted in-guest on the GL host where no display capture exists.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::String;

use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{
    t_close, t_open, t_read, t_write, T_ORDWR, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT,
};
use tapestry::{
    Event, Rect, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_FOCUS, TEV_KEY, TEV_PTR_REL,
};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

const RED: u32 = 0xFFFF_0000;
const BLUE: u32 = 0xFF00_00FF;
const GREEN: u32 = 0xFF00_FF00;
const YELLOW: u32 = 0xFFFF_FF00;
const MAGENTA: u32 = 0xFFFF_00FF;

/// Give up a focus-leg wait after this many non-matching events (~20 s of
/// FRAME ticks): the host harness failed; exit loudly instead of hanging.
const LEG_EVENT_BUDGET: u32 = 1200;

/// The static-screen window a pixel stage holds for the host dump.
const DUMP_MS: u64 = 1500;

fn nap(ms: u64) {
    let _ = sleep(Duration::from_millis(ms));
}

/// H-3b-2: mint a surface and write ONE raw `create ...` line to its ctl,
/// returning t_write's rc (negative = -errno); the minted surface is
/// destroyed again. The chrome-create gate probes need the errno, not
/// write_file's bool.
/// One write on the battery's own conn, returning the raw rc (the errno,
/// negated) rather than a bool: the gate probes need to tell E_PERM from
/// any other refusal.
fn raw_write(root: i64, path: &str, cmd: &str) -> i64 {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
    if fd < 0 {
        return fd;
    }
    let rc = unsafe { t_write(fd, cmd.as_ptr(), cmd.len()) };
    unsafe { t_close(fd) };
    rc
}

fn raw_ctl(root: i64, cmd: &str) -> i64 {
    raw_write(root, "ctl", cmd)
}

/// The pane hosting a surface that is NOT one of `ours` -- on the default
/// image the console renderer's (aurora's) leaf. None on a headless run.
fn foreign_pane(layout: &str, ours: &[u32]) -> Option<u32> {
    for line in layout.lines() {
        let line = line.trim();
        if !line.contains(" leaf ") {
            continue;
        }
        let mut it = line.split_ascii_whitespace();
        let id: u32 = it.next()?.trim_end_matches('*').parse().ok()?;
        let n: Option<u32> = it
            .find(|t| t.starts_with("surface="))
            .and_then(|t| t["surface=".len()..].parse().ok());
        if let Some(n) = n {
            if !ours.contains(&n) {
                return Some(id);
            }
        }
    }
    None
}

/// The FOCUSED empty leaf's pane id (a `<id>*` `leaf` row whose surface
/// token is the bare `empty`), if any -- a split focuses its new empty leaf.
fn empty_pane(layout: &str) -> Option<u32> {
    for line in layout.lines() {
        let line = line.trim();
        if !line.contains(" leaf ") {
            continue;
        }
        let mut it = line.split_ascii_whitespace();
        let idtok = it.next()?;
        if !idtok.ends_with('*') {
            continue;
        }
        let id: u32 = idtok.trim_end_matches('*').parse().ok()?;
        if it.any(|t| t == "empty") {
            return Some(id);
        }
    }
    None
}

/// Read one `pane/<id>/claim` mint: exactly 32 hex digits, else None.
fn read_claim(root: i64, path: &str) -> Option<u128> {
    let s = read_file(root, path)?;
    let t = s.trim();
    if t.len() != 32 {
        return None;
    }
    u128::from_str_radix(t, 16).ok()
}

fn raw_create(root: i64, cmd: &str) -> i64 {
    let ctl = unsafe { t_open(root, b"surface/new".as_ptr(), 11, T_ORDWR) };
    if ctl < 0 {
        return ctl;
    }
    let mut idbuf = [0u8; 16];
    let _ = unsafe { t_read(ctl, idbuf.as_mut_ptr(), idbuf.len()) };
    let rc = unsafe { t_write(ctl, cmd.as_ptr(), cmd.len()) };
    let _ = unsafe { t_write(ctl, b"destroy".as_ptr(), 7) };
    unsafe { t_close(ctl) };
    rc
}

fn read_file(root: i64, path: &str) -> Option<String> {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf = alloc::vec![0u8; 4096];
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(fd) };
    if n < 0 {
        return None;
    }
    buf.truncate(n as usize);
    String::from_utf8(buf).ok()
}

fn write_file(root: i64, path: &str, data: &str) -> bool {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
    if fd < 0 {
        return false;
    }
    let rc = unsafe { t_write(fd, data.as_ptr(), data.len()) };
    unsafe { t_close(fd) };
    rc >= 0
}

#[derive(Clone, Copy)]
struct PaneInfo {
    id: u32,
    x: u32,
    y: u32,
    w: u32,
    h: u32,
    hidden: bool,
}

/// Parse a layout line like `5* leaf surface=1 [641,1,638,397]` for the
/// leaf hosting `surf`; returns its pane id + content rect.
/// Warp-C C-3: ask the compositor to read texel (x, y) of its composed
/// SCREEN back and say it -- `tapestryd: screen-probe (x,y) = #rrggbb via
/// readback|backing [...]` -- the pixel oracle the composed-screen gate
/// asserts on the GL host, where no display capture exists (#195). Best
/// effort: a refusal (a production build strips the verb) is not a battery
/// failure; the host-side scenario decides what to require.
fn probe(root: i64, x: u32, y: u32) {
    let _ = write_file(root, "ctl", &alloc::format!("probe-screen {} {}", x, y));
}

fn find_pane(layout: &str, surf: u32) -> Option<PaneInfo> {
    let want = alloc::format!("surface={}", surf);
    for line in layout.lines() {
        let line = line.trim();
        if !line.contains(" leaf ") || !line.split_ascii_whitespace().any(|t| t == want) {
            continue;
        }
        let idtok = line.split_ascii_whitespace().next()?;
        let id: u32 = idtok.trim_end_matches('*').parse().ok()?;
        let lb = line.find('[')?;
        let rb = line.find(']')?;
        let mut it = line[lb + 1..rb].split(',');
        let x: u32 = it.next()?.parse().ok()?;
        let y: u32 = it.next()?.parse().ok()?;
        let w: u32 = it.next()?.parse().ok()?;
        let h: u32 = it.next()?.parse().ok()?;
        return Some(PaneInfo {
            id,
            x,
            y,
            w,
            h,
            hidden: line.ends_with("hidden"),
        });
    }
    None
}

/// The surface a layout line's leaf hosts (None: not a hosted leaf line).
/// Siblings print in child order, so the line index is the ORDER witness
/// when rects cannot order two panes (a zero-rect backgrounded leaf).
fn leaf_surface(line: &str) -> Option<u32> {
    if !line.contains(" leaf ") {
        return None;
    }
    line.split_ascii_whitespace()
        .find_map(|t| t.strip_prefix("surface="))
        .and_then(|v| v.parse().ok())
}

/// The root container's pane id: the first pane line after the `epoch` header.
fn layout_root_id(layout: &str) -> Option<u32> {
    let line = layout.lines().nth(1)?;
    line.split_ascii_whitespace()
        .next()?
        .trim_end_matches('*')
        .parse()
        .ok()
}

/// A pane's content size from its `geometry` file (`x y w h`); (0, 0) when
/// unreadable -- callers treat that as the backgrounded ZERO rect, so a
/// parse failure fails closed.
fn pane_wh(root: i64, id: u32) -> (u32, u32) {
    let g = read_file(root, &alloc::format!("pane/{}/geometry", id)).unwrap_or_default();
    let mut it = g.split_ascii_whitespace().skip(2);
    let w: u32 = it.next().and_then(|t| t.parse().ok()).unwrap_or(0);
    let h: u32 = it.next().and_then(|t| t.parse().ok()).unwrap_or(0);
    (w, h)
}

/// Parse `<key> <n>` from the ctl text.
fn ctl_u64(ctl: &str, key: &str) -> Option<u64> {
    for line in ctl.lines() {
        if let Some(rest) = line.strip_prefix(key) {
            return rest.trim().parse().ok();
        }
    }
    None
}

fn fill(surf: &mut Surface, color: u32) {
    for p in surf.pixels().iter_mut() {
        *p = color;
    }
}

/// The compositor's placement, mirrored for sample points. The battery
/// presents FULL-FRAME only (present(None) everywhere), so it never
/// trips the #56 patchwork latch and always LETTERBOXES -- centered,
/// scaled up or down -- meaning the pane center always samples the
/// fill. (The pre-#56 size discriminator needed a covered-region-center
/// arm for the overflow crop; a latched accumulator would need it back.)
fn sample_point(px: u32, py: u32, pw: u32, ph: u32, _sw: u32, _sh: u32) -> (u32, u32) {
    (px + pw / 2, py + ph / 2)
}

fn overlap(a: PaneInfo, b: PaneInfo) -> bool {
    a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h
}

/// Wait for an event of `kind` on `surf`; None = budget exhausted or the
/// stream closed.
fn wait_kind(surf: &mut Surface, kind: u16, tag: &str) -> Option<Event> {
    let mut budget = LEG_EVENT_BUDGET;
    loop {
        match surf.wait_event() {
            Ok(ev) => {
                if ev.kind == kind {
                    return Some(ev);
                }
                budget -= 1;
                if budget == 0 {
                    say!("tapestry-battery: FAIL {} never arrived", tag);
                    return None;
                }
            }
            Err(e) => {
                say!("tapestry-battery: FAIL {} event stream {:?}", tag, e);
                return None;
            }
        }
    }
}

/// Wait for TEV_FOCUS gained (value 1) on `surf`, skipping everything
/// else -- including stale FOCUS-lost tails from earlier transitions.
fn wait_focus_gained(surf: &mut Surface, tag: &str) -> bool {
    let mut budget = LEG_EVENT_BUDGET;
    loop {
        match surf.wait_event() {
            Ok(ev) => {
                if ev.kind == TEV_FOCUS && ev.value == 1 {
                    return true;
                }
                budget -= 1;
                if budget == 0 {
                    say!("tapestry-battery: FAIL {} focus-gained never arrived", tag);
                    return false;
                }
            }
            Err(e) => {
                say!("tapestry-battery: FAIL {} event stream {:?}", tag, e);
                return false;
            }
        }
    }
}

/// Wait for a pressed KEY on `surf`, printing it tagged; false = budget
/// exhausted or the stream closed.
fn wait_key(surf: &mut Surface, tag: &str) -> bool {
    let mut budget = LEG_EVENT_BUDGET;
    loop {
        match surf.wait_event() {
            Ok(ev) => {
                if ev.kind == TEV_KEY && ev.value != 0 {
                    say!("battery: {} key code={} rune={:#x}", tag, ev.code, ev.rune);
                    return true;
                }
                budget -= 1;
                if budget == 0 {
                    say!("tapestry-battery: FAIL {} key never arrived", tag);
                    return false;
                }
            }
            Err(e) => {
                say!("tapestry-battery: FAIL {} event stream {:?}", tag, e);
                return false;
            }
        }
    }
}

/// Wait for a TEV_PTR_REL on `surf`; returns the decoded signed deltas.
/// Non-REL events (FRAME ticks, MOVE noise from the motion itself) count
/// down the budget -- the 60 Hz FRAME stream bounds the wait.
fn wait_rel(surf: &mut Surface, tag: &str) -> Option<(i32, i32)> {
    let mut budget = LEG_EVENT_BUDGET;
    loop {
        match surf.wait_event() {
            Ok(ev) => {
                if ev.kind == TEV_PTR_REL {
                    let dx = (ev.value >> 16) as u16 as i16 as i32;
                    let dy = (ev.value & 0xFFFF) as u16 as i16 as i32;
                    return Some((dx, dy));
                }
                budget -= 1;
                if budget == 0 {
                    say!("tapestry-battery: FAIL {} rel never arrived", tag);
                    return None;
                }
            }
            Err(e) => {
                say!("tapestry-battery: FAIL {} rel stream {:?}", tag, e);
                return None;
            }
        }
    }
}

/// Drain a surface's event backlog to quiet (3 consecutive empty polls).
fn drain_settle(surf: &mut Surface) {
    let mut quiet = 0;
    let mut budget = 4 * LEG_EVENT_BUDGET;
    while quiet < 3 && budget > 0 {
        budget -= 1;
        match surf.poll_event() {
            Ok(Some(_)) => quiet = 0,
            Ok(None) => {
                quiet += 1;
                nap(20);
            }
            Err(_) => return,
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // The driver session (layout ops are compositor-global; surfaces stay
    // on each client session per F2).
    let root = unsafe {
        t_open(
            T_WALK_OPEN_FROM_ROOT,
            b"/srv/tapestry".as_ptr(),
            13,
            T_OREAD,
        )
    };
    if root < 0 {
        say!("tapestry-battery: FAIL no /srv/tapestry ({})", root);
        return 1;
    }
    let ctl = match read_file(root, "ctl") {
        Some(s) => s,
        None => {
            say!("tapestry-battery: FAIL ctl read");
            return 1;
        }
    };
    let mut disp = (0u32, 0u32);
    for line in ctl.lines() {
        if let Some(rest) = line.strip_prefix("display ") {
            let mut it = rest.split_ascii_whitespace();
            disp = (
                it.next().and_then(|t| t.parse().ok()).unwrap_or(0),
                it.next().and_then(|t| t.parse().ok()).unwrap_or(0),
            );
        }
    }
    if disp.0 == 0 || disp.1 == 0 {
        say!("tapestry-battery: FAIL display geometry");
        return 1;
    }
    say!("battery: display {}x{}", disp.0, disp.1);

    // Client A: display-sized (will be cropped into its pane). Its
    // host-at-create takes focus -- the first TEV_FOCUS gained (G-6c).
    let mut a = match Surface::open(disp.0, disp.1) {
        Ok(s) => s,
        Err(e) => {
            say!("tapestry-battery: FAIL client A {:?}", e);
            return 1;
        }
    };
    fill(&mut a, RED);
    if a.present(None).is_err() {
        say!("tapestry-battery: FAIL A present");
        return 1;
    }
    if !wait_focus_gained(&mut a, "A create") {
        return 1;
    }
    say!("battery: focus event OK");

    // Deterministic structure: pane::host() picks the auto-split direction
    // from the focused leaf's shape, and F2's aurora-exclusion flips A from
    // TALL (pre-F2, aurora took a column -> splitV) to WIDE (aurora excluded
    // -> splitH). The mode-sensitive legs below (unzoom compares A's original
    // geometry after a `mode splitv`; move escalates through the splitv)
    // assume a NESTED splitV [A,B]. Force it independent of host(): split A's
    // leaf splitv -- a mode different from the root splith NESTS a [A,B] splitv
    // container, and B (hosted next) lands in the focused empty child.
    {
        let l = read_file(root, "layout").unwrap_or_default();
        match find_pane(&l, a.id) {
            Some(pa0) => {
                if !write_file(root, "layout", &alloc::format!("split {} v", pa0.id)) {
                    say!("tapestry-battery: FAIL split A splitv");
                    return 1;
                }
            }
            None => {
                say!("tapestry-battery: FAIL find A pre-split");
                return 1;
            }
        }
    }

    // Client B: half-sized (fits its quarter-ish pane loosely).
    let mut b = match Surface::open(disp.0 / 2, disp.1 / 2) {
        Ok(s) => s,
        Err(e) => {
            say!("tapestry-battery: FAIL client B {:?}", e);
            return 1;
        }
    };
    fill(&mut b, BLUE);
    if b.present(None).is_err() {
        say!("tapestry-battery: FAIL B present");
        return 1;
    }
    // Heal both panes after the second hosting's chrome repaint.
    fill(&mut a, RED);
    fill(&mut b, BLUE);
    if a.present(None).is_err() || b.present(None).is_err() {
        say!("tapestry-battery: FAIL heal presents");
        return 1;
    }

    let layout = match read_file(root, "layout") {
        Some(s) => s,
        None => {
            say!("tapestry-battery: FAIL layout read");
            return 1;
        }
    };
    for line in layout.lines() {
        say!("battery-layout: {}", line);
    }
    let (pa, pb) = match (find_pane(&layout, a.id), find_pane(&layout, b.id)) {
        (Some(pa), Some(pb)) => (pa, pb),
        _ => {
            say!("tapestry-battery: FAIL panes not in layout");
            return 1;
        }
    };

    // Structure asserts: nonzero, disjoint, inside the display; the pane
    // geometry file agrees with the layout text (two views, one truth).
    if pa.w == 0 || pa.h == 0 || pb.w == 0 || pb.h == 0 || overlap(pa, pb) {
        say!(
            "tapestry-battery: FAIL rects (a=[{},{},{},{}] b=[{},{},{},{}])",
            pa.x,
            pa.y,
            pa.w,
            pa.h,
            pb.x,
            pb.y,
            pb.w,
            pb.h
        );
        return 1;
    }
    if pa.x + pa.w > disp.0 || pa.y + pa.h > disp.1 || pb.x + pb.w > disp.0 || pb.y + pb.h > disp.1
    {
        say!("tapestry-battery: FAIL rects out of display");
        return 1;
    }
    for (p, nm) in [(pa, "A"), (pb, "B")] {
        let path = alloc::format!("pane/{}/geometry", p.id);
        let g = read_file(root, &path).unwrap_or_default();
        let want = alloc::format!("{} {} {} {}", p.x, p.y, p.w, p.h);
        if g.trim() != want {
            say!(
                "tapestry-battery: FAIL {} geometry file '{}' != layout '{}'",
                nm,
                g.trim(),
                want
            );
            return 1;
        }
    }
    // An UNDECLARED user client (this battery has not written `session on`)
    // must not put the console renderer to sleep: its leaf keeps a real
    // column. A None here while the layout carries a foreign surface is the
    // parser drifting, not a headless run -- fail, never skip.
    match foreign_pane(&layout, &[a.id, b.id]) {
        Some(cid) => {
            let (cw, ch) = pane_wh(root, cid);
            if cw == 0 || ch == 0 {
                say!(
                    "tapestry-battery: FAIL console leaf backgrounded by an undeclared client ({} {})",
                    cw,
                    ch
                );
                return 1;
            }
            say!("battery: console leaf tiled {} {}", cw, ch);
        }
        None => {
            if layout
                .lines()
                .any(|l| leaf_surface(l).is_some_and(|n| n != a.id && n != b.id))
            {
                say!("tapestry-battery: FAIL foreign_pane found no console leaf in a layout that hosts one");
                return 1;
            }
        }
    }
    say!("tapestry-battery: structure OK");
    say!(
        "battery: stage1 centers {} {} {} {}",
        pa.x + pa.w / 2,
        pa.y + pa.h / 2,
        pb.x + pb.w / 2,
        pb.y + pb.h / 2
    );
    probe(root, pa.x + pa.w / 2, pa.y + pa.h / 2);
    probe(root, pb.x + pb.w / 2, pb.y + pb.h / 2);
    nap(DUMP_MS);

    // H-3b-1: the per-leaf Daylight tag bar. Every >1-leaf leaf carves a
    // header_h strip off its content TOP (HALCYON-VISUAL 3.2/4); the `tagbar`
    // file reports it and the compositor fills it `header`-bg -- the resting
    // fallback before a halcyond Role::Chrome surface binds. Assert A's tagbar
    // abuts its content (same x/width, meeting on y) and reads `header` at its
    // centre (the positive render witness the .exp samples).
    {
        let tbs = read_file(root, &alloc::format!("pane/{}/tagbar", pa.id)).unwrap_or_default();
        let mut it = tbs.split_whitespace();
        let tx = it.next().and_then(|s| s.parse::<u32>().ok());
        let ty = it.next().and_then(|s| s.parse::<u32>().ok());
        let tw = it.next().and_then(|s| s.parse::<u32>().ok());
        let th = it.next().and_then(|s| s.parse::<u32>().ok());
        match (tx, ty, tw, th) {
            (Some(tx), Some(ty), Some(tw), Some(th)) if th > 0 => {
                if tx != pa.x || tw != pa.w || ty + th != pa.y {
                    say!(
                        "tapestry-battery: FAIL tagbar '{}' not above A [{},{},{},{}]",
                        tbs.trim(),
                        pa.x,
                        pa.y,
                        pa.w,
                        pa.h
                    );
                    return 1;
                }
                say!("battery: tagbar A {} {}", tx + tw / 2, ty + th / 2);
                probe(root, tx + tw / 2, ty + th / 2);
                nap(DUMP_MS);
            }
            _ => {
                say!("tapestry-battery: FAIL tagbar file '{}'", tbs.trim());
                return 1;
            }
        }
    }

    // H-3b-2: the chrome-create gate. `create W H` takes optional
    // `role=<content|chrome>` + `bind=<pane-id>`; syntax is judged first
    // (E_INVAL for every peer), then a well-formed chrome request is
    // renderer-gated (E_PERM -- this battery is NOT the console renderer).
    // Four probes, two errno classes: the errno separates the parser's
    // verdict from the gate's. The positive twin (the same line from a
    // renderer, composited at A's tag bar) is halcyond's -- ls-halcyon at
    // H-3b-3. Errnos per libthyla-rs err.rs: T_E_INVAL = 22, T_E_PERM = 1.
    {
        let a_bind = alloc::format!("create 64 20 role=chrome bind={}", pa.id);
        let probes: [(&str, i64, &str); 8] = [
            ("create 64 20 role=bogus", -22, "unknown role -> E_INVAL"),
            (
                "create 64 20 role=chrome",
                -22,
                "chrome without bind -> E_INVAL",
            ),
            ("create 64 20 bind=1", -22, "bind without chrome -> E_INVAL"),
            (a_bind.as_str(), -1, "chrome from a non-renderer -> E_PERM"),
            // H-3c: the menu role takes no bind (syntax) and is renderer-gated
            // (authority) -- an ungated menu would float over any pane and
            // take its input.
            (
                "create 64 20 role=menu bind=1",
                -22,
                "menu with a bind -> E_INVAL",
            ),
            (
                "create 64 20 role=menu",
                -1,
                "menu from a non-renderer -> E_PERM",
            ),
            // H-3d: the status bar takes no bind (syntax) and is renderer-gated
            // (authority) -- an ungated status role would let any client carve
            // the display and own the bar that speaks for the system.
            (
                "create 64 20 role=status bind=1",
                -22,
                "status with a bind -> E_INVAL",
            ),
            (
                "create 64 20 role=status",
                -1,
                "status from a non-renderer -> E_PERM",
            ),
        ];
        for (cmd, want, what) in probes.iter() {
            let rc = raw_create(root, cmd);
            if rc != *want {
                say!(
                    "tapestry-battery: FAIL chrome-create gate: '{}' rc {} want {} ({})",
                    cmd,
                    rc,
                    want,
                    what
                );
                return 1;
            }
        }
        say!("battery: chrome-create gate OK");
        // H-3d: with no renderer bar the compositor's `statusbar` file reads
        // zeros -- the file exists, the rect is empty (the positive twin, a
        // real bar's rect + the carve it makes, is halcyond's: ls-halcyon).
        match read_file(root, "statusbar") {
            Some(t) if t.trim() == "0 0 0 0" => {
                say!("battery: statusbar file reads empty (no bar)")
            }
            other => {
                say!("tapestry-battery: FAIL statusbar file: {:?}", other);
                return 1;
            }
        }
    }

    // H-3b-4: the tile-status verb is a gated global verb (`tag <id> status
    // ok|err|resting`; the cfg-3 default-deny judges authority BEFORE
    // syntax, so a non-renderer sees E_PERM whatever it writes). The
    // discriminating pair: the write is REFUSED (rc -1) AND the pane's
    // `status` file -- an ungated read -- still says `resting` afterwards
    // (a refusal that had applied the write would read `err`). The
    // positive control one variable away is this same conn's ungated
    // verbs (probe-screen above, test-mode below), which succeed. The
    // positive twin of the write is halcyond's (ls-halcyon).
    {
        let path = alloc::format!("pane/{}/status", pa.id);
        let s0 = read_file(root, &path).unwrap_or_default();
        if s0.trim() != "resting" {
            say!(
                "tapestry-battery: FAIL tag-status: fresh tile reads '{}' want resting",
                s0.trim()
            );
            return 1;
        }
        let rc = raw_ctl(root, &alloc::format!("tag {} status err", pa.id));
        if rc != -1 {
            say!(
                "tapestry-battery: FAIL tag-status: non-renderer write rc {} want -1 (E_PERM)",
                rc
            );
            return 1;
        }
        let s1 = read_file(root, &path).unwrap_or_default();
        if s1.trim() != "resting" {
            say!(
                "tapestry-battery: FAIL tag-status: the refused write changed the state to '{}'",
                s1.trim()
            );
            return 1;
        }
        say!("battery: tag-status gate OK");
    }

    // H-3c: the menu verbs (`menu place <surface> <x> <y>` / `menu dismiss`)
    // ride the same default-deny gate: a non-renderer sees E_PERM whatever
    // it writes (authority before syntax), and the ungated `ctl` read shows
    // no menu placed afterwards (a refusal that had placed one would read a
    // rect). The positive twin -- the renderer placing a menu, the
    // compositor dismissing it against a wedged owner -- is ls-halcyon's.
    {
        let probes: [(&str, &str); 3] = [
            ("menu place 0 0 0", "place from a non-renderer"),
            ("menu dismiss", "dismiss from a non-renderer"),
            ("menu bogus", "unknown menu verb from a non-renderer"),
        ];
        for (cmd, what) in probes.iter() {
            let rc = raw_ctl(root, cmd);
            if rc != -1 {
                say!(
                    "tapestry-battery: FAIL menu gate: '{}' rc {} want -1 (E_PERM; {})",
                    cmd,
                    rc,
                    what
                );
                return 1;
            }
        }
        let ctl = read_file(root, "ctl").unwrap_or_default();
        if !ctl.lines().any(|l| l == "menu none") {
            say!("tapestry-battery: FAIL menu gate: ctl reads no `menu none` line after the refusals: {}", ctl.trim());
            return 1;
        }
        say!("battery: menu gate OK");
    }

    // The pane tree's trust model (the H-3b round F2; HALCYON.md 13.6): a
    // client acts only on what it OWNS -- a leaf hosting its own surface, a
    // subtree whose hosted surfaces are all its own -- and the renderer on
    // anything. The console renderer's leaf is neither ours nor empty, so
    // from this battery: `close` (the console-kill), `focus` (the input
    // steal) and a `tag` write (the rendered-identity forge) are all E_PERM,
    // and the pane is intact afterwards (still hosting its surface, its tag
    // unchanged). The positive control one variable away: the same `focus`
    // on OUR pane succeeds (rc >= 0). Headless (no console): the leg is
    // skipped with a line, never a hollow pass.
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        match foreign_pane(&fresh, &[a.id, b.id]) {
            None => say!("battery: pane-tree gate SKIPPED (no foreign pane -- headless)"),
            Some(cid) => {
                let tag0 = read_file(root, &alloc::format!("pane/{}/tag", cid)).unwrap_or_default();
                let tagpath = alloc::format!("pane/{}/tag", cid);
                let probes: [(&str, alloc::string::String); 3] = [
                    ("layout", alloc::format!("close {}", cid)),
                    ("layout", alloc::format!("focus {}", cid)),
                    (tagpath.as_str(), alloc::string::String::from("forged")),
                ];
                for (path, cmd) in probes.iter() {
                    let rc = raw_write(root, path, cmd);
                    if rc != -1 {
                        say!("tapestry-battery: FAIL pane-tree gate: '{}' <- '{}' rc {} want -1 (E_PERM)",
                            path, cmd, rc);
                        return 1;
                    }
                }
                let after = read_file(root, "layout").unwrap_or_default();
                if foreign_pane(&after, &[a.id, b.id]) != Some(cid) {
                    say!(
                        "tapestry-battery: FAIL pane-tree gate: the refused close changed the tree"
                    );
                    return 1;
                }
                let tag1 = read_file(root, &alloc::format!("pane/{}/tag", cid)).unwrap_or_default();
                if tag1 != tag0 {
                    say!("tapestry-battery: FAIL pane-tree gate: the refused tag write landed ('{}' -> '{}')",
                        tag0.trim(), tag1.trim());
                    return 1;
                }
                let rc = raw_write(root, "layout", &alloc::format!("focus {}", pa.id));
                if rc < 0 {
                    say!(
                        "tapestry-battery: FAIL pane-tree gate: focus on OUR pane refused rc {}",
                        rc
                    );
                    return 1;
                }
                // The positive control MOVED focus (that is the proof); put it
                // back on B, which the scenarios below assume is the active
                // child (hosted last) -- also ours, so it must succeed.
                let rc = raw_write(root, "layout", &alloc::format!("focus {}", pb.id));
                if rc < 0 {
                    say!(
                        "tapestry-battery: FAIL pane-tree gate: focus back on B refused rc {}",
                        rc
                    );
                    return 1;
                }
                say!("battery: pane-tree gate OK");
            }
        }
    }

    // Scenario 2 (G-6b, the resize protocol). B's pane content differs
    // from B's surface size, so the hosting reconcile issued B a
    // size-changing CONFIGURE offer. Negative probes first -- neither
    // consumes the standing offer: a stale serial (0 -- every real
    // serial is >= 1) answers E_AGAIN, an unknown one E_INVAL.
    match b.reweave(1, 1, 0) {
        Err(TapError::Busy) => {}
        r => {
            say!("tapestry-battery: FAIL stale-serial probe {:?}", r);
            return 1;
        }
    }
    match b.reweave(1, 1, 60000) {
        Err(TapError::Protocol) => {}
        r => {
            say!("tapestry-battery: FAIL unknown-serial probe {:?}", r);
            return 1;
        }
    }
    say!("battery: resize rejects OK");
    // The real ack: drain to the offer, reweave onto the new generation,
    // repaint at the exact pane size, present (which also retires the
    // displaced generation server-side).
    let cfg = match wait_kind(&mut b, TEV_CONFIGURE, "B CONFIGURE") {
        Some(ev) => ev,
        None => return 1,
    };
    if (cfg.value >> 16, cfg.value & 0xffff) == (b.w, b.h) {
        say!("tapestry-battery: FAIL expected a size-changing offer, got same-size");
        return 1;
    }
    match b.handle_configure(&cfg) {
        Ok(true) => {}
        r => {
            say!("tapestry-battery: FAIL reweave {:?}", r);
            return 1;
        }
    }
    fill(&mut b, BLUE);
    if b.present(None).is_err() {
        say!("tapestry-battery: FAIL post-reweave present");
        return 1;
    }
    // Two views, one truth again: the pane geometry file (driver view)
    // agrees with the reweaved surface dimensions (client view) -- B now
    // fits its pane exactly.
    {
        let path = alloc::format!("pane/{}/geometry", pb.id);
        let g = read_file(root, &path).unwrap_or_default();
        let want = alloc::format!("{} {} {} {}", pb.x, pb.y, b.w, b.h);
        if g.trim() != want {
            say!(
                "tapestry-battery: FAIL reweave geometry '{}' != '{}'",
                g.trim(),
                want
            );
            return 1;
        }
    }
    say!("battery: resize OK {} {}", b.w, b.h);

    // Scenario 2b (G-6c): the multi-rect present. ONE present carries TWO
    // rects (left half green, right half yellow); both must land -- a
    // rect0-only server would leave the right half blue, which the host
    // quarter-point samples catch.
    {
        let (bw, bh) = (b.w, b.h);
        let px = b.pixels();
        for y in 0..bh {
            for x in 0..bw {
                px[(y * bw + x) as usize] = if x < bw / 2 { GREEN } else { YELLOW };
            }
        }
        let rects = [
            Rect {
                x: 0,
                y: 0,
                w: bw / 2,
                h: bh,
            },
            Rect {
                x: bw / 2,
                y: 0,
                w: bw - bw / 2,
                h: bh,
            },
        ];
        if b.present_rects(&rects).is_err() {
            say!("tapestry-battery: FAIL multirect present");
            return 1;
        }
        say!(
            "battery: multirect ready {} {} {} {}",
            pb.x + bw / 4,
            pb.y + bh / 2,
            pb.x + 3 * bw / 4,
            pb.y + bh / 2
        );
        probe(root, pb.x + bw / 4, pb.y + bh / 2);
        probe(root, pb.x + 3 * bw / 4, pb.y + bh / 2);
        nap(DUMP_MS);
    }
    fill(&mut b, BLUE);
    if b.present(None).is_err() {
        say!("tapestry-battery: FAIL post-multirect restore");
        return 1;
    }
    // Scenario 2b-v (Warp-C C-3): the same two-rect present split TOP /
    // BOTTOM (green over yellow). A vertical asymmetry is what a solid
    // fill and a left/right split can never show: a composition that lands
    // rows mirrored or displaced (a blit box measured from the wrong edge)
    // reads the wrong color at one of the two quarter points, while every
    // other stage in this battery reads right. Probed, not dumped: the host
    // dump scenarios were written for the stages above and skip this line.
    {
        let (bw, bh) = (b.w, b.h);
        let px = b.pixels();
        for y in 0..bh {
            for x in 0..bw {
                px[(y * bw + x) as usize] = if y < bh / 2 { GREEN } else { YELLOW };
            }
        }
        let rects = [
            Rect {
                x: 0,
                y: 0,
                w: bw,
                h: bh / 2,
            },
            Rect {
                x: 0,
                y: bh / 2,
                w: bw,
                h: bh - bh / 2,
            },
        ];
        if b.present_rects(&rects).is_err() {
            say!("tapestry-battery: FAIL multirect-v present");
            return 1;
        }
        say!(
            "battery: multirect-v ready {} {} {} {}",
            pb.x + bw / 2,
            pb.y + bh / 4,
            pb.x + bw / 2,
            pb.y + 3 * bh / 4
        );
        probe(root, pb.x + bw / 2, pb.y + bh / 4);
        probe(root, pb.x + bw / 2, pb.y + 3 * bh / 4);
    }
    fill(&mut b, BLUE);
    if b.present(None).is_err() {
        say!("tapestry-battery: FAIL post-multirect-v restore");
        return 1;
    }

    // Scenario 2c (G-6c): tabbed mode + the D7 glyph-free strip. mode on
    // A's pane targets its parent (the [A/B] splitv). The active child
    // is B (hosted last), so A hides; the strip paints two segments --
    // A's BORDER_COLOR, B's FOCUS_COLOR (focus is inside B).
    if !write_file(root, "layout", &alloc::format!("mode {} tabbed", pa.id)) {
        say!("tapestry-battery: FAIL mode tabbed");
        return 1;
    }
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        let (ta, tb) = match (find_pane(&fresh, a.id), find_pane(&fresh, b.id)) {
            (Some(ta), Some(tb)) => (ta, tb),
            _ => {
                say!("tapestry-battery: FAIL tabbed layout parse");
                return 1;
            }
        };
        if !ta.hidden || tb.hidden || tb.w == 0 {
            say!(
                "tapestry-battery: FAIL tabbed visibility (A hidden={} B hidden={})",
                ta.hidden,
                tb.hidden
            );
            return 1;
        }
        // B heals into the enlarged tab content (its bottom is cropped
        // until it re-acks; solid blue keeps the samples exact).
        fill(&mut b, BLUE);
        let _ = b.present(None);
        // The strip geometry from B's content rect: the container's outer
        // rect is content + the Daylight chrome ring on x, and strip(5) +
        // ring + tag bar(20) above it on y (TAB_STRIP_H = 5). The ring at the
        // default gaps=1 is floor(1)+bevel(2)+hairline(1) = 4 (HALCYON-VISUAL
        // section 2/2.4; tapestryd pane.rs recompute -- this driver runs on
        // the default image at default gaps). Since H-3b-1 each >1-leaf leaf
        // also carves a header_h(20) tag bar off its content TOP (section
        // 3.2/4), so B's content.y sits ring+tagbar below its rect and strip+
        // ring+tagbar below the container top.
        let inset = 4u32;
        let tagbar = 20u32; // METRICS.header_h

        // The tab strip sits at the container top. When aurora is excluded (F2
        // structural transparency) the tabbed root's active child is the ONLY
        // visible leaf, so it is borderless (no ring/tagbar) and sits at the
        // display top-left (tb.x=0, tb.y=strip). saturating_sub keeps the strip
        // coords valid there AND in the chromed >1-leaf case -- both land the
        // strip row center; a raw subtraction underflowed u32 -> OOB probe.
        let cx = tb.x.saturating_sub(inset);
        let cw = tb.w + 2 * inset;
        let sy = tb.y.saturating_sub(tagbar + inset + 5) + 2; // strip row center
        let sax = cx + cw / 4;
        let sbx = cx + 3 * cw / 4;
        say!("battery: tabbed ready {} {} {}", sy, sax, sbx);
        probe(root, sax, sy);
        probe(root, sbx, sy);
        nap(DUMP_MS);
    }
    // Cycle the active child: A reveals, B hides, focus follows into A.
    if !write_file(root, "layout", "tab next") {
        say!("tapestry-battery: FAIL tab next");
        return 1;
    }
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        let (ta, tb) = match (find_pane(&fresh, a.id), find_pane(&fresh, b.id)) {
            (Some(ta), Some(tb)) => (ta, tb),
            _ => {
                say!("tapestry-battery: FAIL tab-cycle layout parse");
                return 1;
            }
        };
        if ta.hidden || !tb.hidden {
            say!("tapestry-battery: FAIL tab-cycle visibility");
            return 1;
        }
    }
    say!("battery: tab cycled");
    // Warp-C C-3 / the C-2d redraw contract on the COMPOSED path: A was
    // hidden (its presents composed nothing) and is revealed by the cycle;
    // its next present must land in the revealed pane. Present A red, then
    // probe the pane's center: the composed-screen gate requires red on
    // both device legs.
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        if let Some(ta) = find_pane(&fresh, a.id) {
            fill(&mut a, RED);
            let _ = a.present(None);
            say!(
                "battery: tab-cycled ready {} {}",
                ta.x + ta.w / 2,
                ta.y + ta.h / 2
            );
            probe(root, ta.x + ta.w / 2, ta.y + ta.h / 2);
            nap(DUMP_MS);
        }
    }
    // Restore splitv; heal both.
    if !write_file(root, "layout", &alloc::format!("mode {} splitv", pa.id)) {
        say!("tapestry-battery: FAIL mode splitv restore");
        return 1;
    }
    fill(&mut a, RED);
    fill(&mut b, BLUE);
    let _ = a.present(None);
    let _ = b.present(None);

    // Scenario 2d (G-6c): zoom. A's pane fills the display alone; A is
    // display-sized, so the scanout goes DIRECT at A's next present.
    if !write_file(root, "layout", &alloc::format!("zoom {}", pa.id)) {
        say!("tapestry-battery: FAIL zoom");
        return 1;
    }
    fill(&mut a, RED);
    if a.present(None).is_err() {
        say!("tapestry-battery: FAIL zoom present");
        return 1;
    }
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        let want = alloc::format!("zoomed {}", pa.id);
        if !fresh.lines().next().unwrap_or("").contains(want.as_str()) {
            say!("tapestry-battery: FAIL zoom marker missing");
            return 1;
        }
        let g = read_file(root, &alloc::format!("pane/{}/geometry", pa.id)).unwrap_or_default();
        let wantg = alloc::format!("0 0 {} {}", disp.0, disp.1);
        if g.trim() != wantg {
            say!(
                "tapestry-battery: FAIL zoom geometry '{}' != '{}'",
                g.trim(),
                wantg
            );
            return 1;
        }
    }
    say!("battery: zoom ready");
    nap(DUMP_MS);
    // Toggle back; the layout (and A's pane rect) restore exactly.
    if !write_file(root, "layout", &alloc::format!("zoom {}", pa.id)) {
        say!("tapestry-battery: FAIL unzoom");
        return 1;
    }
    {
        let g = read_file(root, &alloc::format!("pane/{}/geometry", pa.id)).unwrap_or_default();
        let wantg = alloc::format!("{} {} {} {}", pa.x, pa.y, pa.w, pa.h);
        if g.trim() != wantg {
            say!(
                "tapestry-battery: FAIL unzoom geometry '{}' != '{}'",
                g.trim(),
                wantg
            );
            return 1;
        }
    }
    say!("battery: zoom restored");
    fill(&mut a, RED);
    fill(&mut b, BLUE);
    let _ = a.present(None);
    let _ = b.present(None);

    // Scenario 2e (G-6c): directional move (D6 re-parenting). B's parent
    // is the splitv -- a LEFT move escalates to the root splith, pulls B
    // out beside its subtree ([aurora | B | A] after the singleton
    // dissolves), then a RIGHT move swaps with A ([aurora | A | B]).
    if !write_file(root, "layout", &alloc::format!("move {} left", pb.id)) {
        say!("tapestry-battery: FAIL move left");
        return 1;
    }
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        let (ma, mb) = match (find_pane(&fresh, a.id), find_pane(&fresh, b.id)) {
            (Some(ma), Some(mb)) => (ma, mb),
            _ => {
                say!("tapestry-battery: FAIL move-left layout parse");
                return 1;
            }
        };
        // B must sit BETWEEN the console leaf and A: left of A by rect, and
        // AFTER the console leaf by child order. The console leaf is
        // backgrounded (zero-rect) while the battery holds the display, so
        // only the layout text's line order places B relative to it -- a
        // pull-out landing BEFORE the console leaf would print B first.
        let leaves: alloc::vec::Vec<Option<u32>> = fresh.lines().map(leaf_surface).collect();
        let pos = |surf: u32| leaves.iter().position(|s| *s == Some(surf));
        let console = leaves
            .iter()
            .position(|s| matches!(s, Some(n) if *n != a.id && *n != b.id));
        let ordered = matches!(
            (console, pos(b.id), pos(a.id)),
            (Some(c), Some(bl), Some(al)) if c < bl && bl < al
        );
        if mb.x >= ma.x || !ordered {
            say!(
                "tapestry-battery: FAIL move-left order (B.x={} A.x={} lines console={:?} B={:?} A={:?})",
                mb.x,
                ma.x,
                console,
                pos(b.id),
                pos(a.id)
            );
            return 1;
        }
    }
    if !write_file(root, "layout", &alloc::format!("move {} right", pb.id)) {
        say!("tapestry-battery: FAIL move right");
        return 1;
    }
    let (ma, mb) = {
        let fresh = read_file(root, "layout").unwrap_or_default();
        match (find_pane(&fresh, a.id), find_pane(&fresh, b.id)) {
            (Some(ma), Some(mb)) if ma.x < mb.x => (ma, mb),
            _ => {
                say!("tapestry-battery: FAIL move-right order");
                return 1;
            }
        }
    };
    fill(&mut a, RED);
    fill(&mut b, BLUE);
    if a.present(None).is_err() || b.present(None).is_err() {
        say!("tapestry-battery: FAIL move heal presents");
        return 1;
    }
    // Fork-2 placement-aware samples (see sample_point): fit-inside
    // letterboxes (pane center), overflow crops (covered-region center).
    let (sax, say_) = sample_point(ma.x, ma.y, ma.w, ma.h, a.w, a.h);
    let (sbx, sby) = sample_point(mb.x, mb.y, mb.w, mb.h, b.w, b.h);
    say!("battery: move OK {} {} {} {}", sax, say_, sbx, sby);
    nap(DUMP_MS);

    // Focus leg 1: A takes focus; a QMP-typed key must arrive on A's
    // stream (and nowhere else -- the exp asserts no "battery: B key"
    // before the switch).
    if !write_file(root, "layout", &alloc::format!("focus {}", ma.id)) {
        say!("tapestry-battery: FAIL focus A");
        return 1;
    }
    say!("tapestry-battery: panes ready");
    if !wait_key(&mut a, "A") {
        return 1;
    }
    // Focus leg 2: switch to B via B's own pane ctl (exercising the
    // per-pane ctl verb path, vs the layout-file path used for A).
    if !write_file(root, &alloc::format!("pane/{}/ctl", mb.id), "focus") {
        say!("tapestry-battery: FAIL focus B");
        return 1;
    }
    say!("tapestry-battery: focus B");
    if !wait_key(&mut b, "B") {
        return 1;
    }

    // The chord leg (G-6c): focus sits on B; the host sends Super+Left.
    // The compositor intercepts it ABOVE the event stream (section 18.4)
    // and moves focus spatially to A -- A sees TEV_FOCUS gained, and B
    // sees the FOCUS lost WITHOUT ever seeing the arrow KEY.
    drain_settle(&mut a);
    say!("battery: chord ready");
    if !wait_focus_gained(&mut a, "chord") {
        return 1;
    }
    {
        // B's stream up to its FOCUS lost must carry no arrow key (the
        // Super press itself -- a modifier -- may appear; that is the
        // documented mods-visible behavior).
        let mut budget = LEG_EVENT_BUDGET;
        loop {
            match b.wait_event() {
                Ok(ev) => {
                    if ev.kind == TEV_KEY && matches!(ev.code, 103 | 105 | 106 | 108) {
                        say!("tapestry-battery: FAIL chord leaked arrow key {}", ev.code);
                        return 1;
                    }
                    if ev.kind == TEV_FOCUS && ev.value == 0 {
                        break;
                    }
                    budget -= 1;
                    if budget == 0 {
                        say!("tapestry-battery: FAIL B focus-lost never arrived");
                        return 1;
                    }
                }
                Err(e) => {
                    say!("tapestry-battery: FAIL chord B stream {:?}", e);
                    return 1;
                }
            }
        }
    }
    say!("battery: chord focus OK");

    // The rel legs (the relative-mouse arc): focus sits on A after the
    // chord. Leg 1 -- the mouse device: one injected QMP `rel` frame
    // must arrive as ONE exact TEV_PTR_REL (proves the third-function
    // claim + the EV_REL-without-ABS classify + the drain + the focused
    // routing, end to end). Leg 2 -- the abs-synthesis twin (the
    // abs-only-frontend mouse-look path, cocoa): two tablet abs
    // injections at the same Y; the first only SEEDS the delta base (no
    // rel), the second must arrive as the exact display-pixel delta.
    drain_settle(&mut a);
    say!("battery: rel ready");
    match wait_rel(&mut a, "mouse") {
        Some((7, 3)) => say!("battery: rel OK 7 3"),
        Some((dx, dy)) => {
            say!("tapestry-battery: FAIL mouse rel got ({}, {})", dx, dy);
            return 1;
        }
        None => return 1,
    }
    say!("battery: relsynth ready");
    match wait_rel(&mut a, "synth") {
        Some((160, 0)) => say!("battery: relsynth OK 160"),
        Some((dx, dy)) => {
            say!("tapestry-battery: FAIL synth rel got ({}, {})", dx, dy);
            return 1;
        }
        None => return 1,
    }

    // The test-mode leg (section 18.6, G-6c): freeze the FRAME clock,
    // prove it holds still, then drive it one tick by hand.
    if !write_file(root, "ctl", "test-mode on") {
        say!("tapestry-battery: FAIL test-mode on");
        return 1;
    }
    {
        let c1 = read_file(root, "ctl").unwrap_or_default();
        if !c1.contains("test-mode on") {
            say!("tapestry-battery: FAIL test-mode not reported on");
            return 1;
        }
        let t0 = ctl_u64(&c1, "tick ").unwrap_or(u64::MAX);
        nap(300);
        let t1 =
            ctl_u64(&read_file(root, "ctl").unwrap_or_default(), "tick ").unwrap_or(u64::MAX - 1);
        if t0 != t1 {
            say!(
                "tapestry-battery: FAIL frozen clock advanced ({} -> {})",
                t0,
                t1
            );
            return 1;
        }
        if !write_file(root, "ctl", "tick") {
            say!("tapestry-battery: FAIL tick write");
            return 1;
        }
        let t2 = ctl_u64(&read_file(root, "ctl").unwrap_or_default(), "tick ").unwrap_or(0);
        if t2 != t0 + 1 {
            say!("tapestry-battery: FAIL tick step ({} -> {})", t0, t2);
            return 1;
        }
    }
    say!("battery: test-mode OK");

    // cfg-3: the apply-authority gate (AURORA-CONFIG.md section 3.3). The
    // battery is NOT the console renderer, so the AUTHORITY verbs must
    // refuse this conn -- while its ctl READ (the geometry parse above)
    // and the determinism verbs (the whole test-mode leg above) stay
    // live. A `mode` acceptance here would be exactly the privilege leak
    // the gate closes: any boot-chain client driving the shared display.
    if write_file(root, "ctl", "mode 640 480") {
        say!("tapestry-battery: FAIL gate: mode accepted from a non-renderer");
        return 1;
    }
    if write_file(root, "ctl", "clock-rate 30") {
        say!("tapestry-battery: FAIL gate: clock-rate accepted from a non-renderer");
        return 1;
    }
    // cfg-4: the runtime chord/gaps verbs are AUTHORITY too -- the
    // default-deny gate refuses them for this non-renderer conn by
    // construction (they are not in is_ungated_ctl). Valid commands that
    // would SUCCEED ungated, so a refusal proves the gate covers them.
    if write_file(root, "ctl", "chord super+g zoom") {
        say!("tapestry-battery: FAIL gate: chord accepted from a non-renderer");
        return 1;
    }
    if write_file(root, "ctl", "gaps 8") {
        say!("tapestry-battery: FAIL gate: gaps accepted from a non-renderer");
        return 1;
    }
    say!("battery: gate OK");

    // Warp-4 glsrc verb gate (the 2D leg): a warp ctx can never exist on
    // this box (the ctx mint is virgl-gated), so only the verb's own
    // edges are testable here -- and they must hold exactly. `off` with
    // nothing set is idempotent-Ok; naming any ctx is E_NOENT (the #178
    // fail-loud shape: the named half must exist at write); junk is
    // E_INVAL. Activation legs live in the thyla-gl quake gate. The
    // writes ride A's OWN conn (surface_ctl -- the minted ctl fd): F2
    // means the driver conn cannot even resolve A's surface dir, which
    // the first execution of these legs proved the hard way.
    if a.surface_ctl("glsrc off").is_err() {
        say!("tapestry-battery: FAIL glsrc off refused");
        return 1;
    }
    if a.surface_ctl("glsrc 1").is_ok() {
        say!("tapestry-battery: FAIL glsrc accepted a ctx that cannot exist");
        return 1;
    }
    if a.surface_ctl("glsrc bogus").is_ok() {
        say!("tapestry-battery: FAIL glsrc accepted junk");
        return 1;
    }
    if a.surface_ctl("glsrc off").is_err() {
        say!("tapestry-battery: FAIL glsrc off not idempotent");
        return 1;
    }
    say!("battery: glsrc gate OK");

    // The hold leg (TPRESENT_HOLD + release, G-6c): magenta blits into
    // the screen buffer NOW but the device push defers -- on screen B
    // stays blue until release. The host samples between the two dumps;
    // the typed key (routed to A, still focused from the chord) is the
    // sample-done handshake.
    fill(&mut b, MAGENTA);
    if b.present_hold(None).is_err() {
        say!("tapestry-battery: FAIL hold present");
        return 1;
    }
    let (hbx, hby) = sample_point(mb.x, mb.y, mb.w, mb.h, b.w, b.h);
    say!("battery: hold ready {} {}", hbx, hby);
    if !wait_key(&mut a, "hold-sync") {
        return 1;
    }
    if b.release().is_err() {
        say!("tapestry-battery: FAIL release");
        return 1;
    }
    say!("battery: released");
    nap(DUMP_MS);
    if !write_file(root, "ctl", "test-mode off") {
        say!("tapestry-battery: FAIL test-mode off");
        return 1;
    }
    if !read_file(root, "ctl")
        .unwrap_or_default()
        .contains("test-mode off")
    {
        say!("tapestry-battery: FAIL test-mode not reported off");
        return 1;
    }
    say!("battery: hold OK");

    // Scenario 3 (G-6b): the compositor-initiated pane close. Closing
    // B's pane strands the surface and queues the TEV_CLOSE exit
    // request; the surface stays live until the CLIENT destroys it
    // (drop) -- close is a request, never a forced retire.
    let b_id = b.id;
    if !write_file(root, "layout", &alloc::format!("close {}", mb.id)) {
        say!("tapestry-battery: FAIL close B pane");
        return 1;
    }
    if wait_kind(&mut b, TEV_CLOSE, "B CLOSE").is_none() {
        return 1;
    }
    if let Some(fresh) = read_file(root, "layout") {
        if find_pane(&fresh, b_id).is_some() {
            say!("tapestry-battery: FAIL B pane survived close");
            return 1;
        }
    }
    say!("battery: close event OK");
    drop(b);

    // H-4b: the one-shot placement claim (HALCYON.md 13.7 -- the layout-
    // restore placement primitive). Split OUR leaf (a new EMPTY leaf E takes
    // focus) and mint E's claim TWICE: the first token goes STALE under the
    // second (last mint wins). Move focus BACK onto A, so the focus path
    // would SPLIT A's tile. Then a create with the stale token must FALL
    // BACK to that path (it lands, but never in E, and E stays empty), and
    // a create with the live token must land in E itself -- the two legs
    // discriminate a working claim from a decorative one in both directions.
    // A malformed token is E_INVAL at the syntax gate (raw, no surface).
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        let pa2 = match find_pane(&fresh, a.id) {
            Some(p) => p,
            None => {
                say!("tapestry-battery: FAIL claim: A's pane not found");
                return 1;
            }
        };
        if raw_write(root, "layout", &alloc::format!("split {} h", pa2.id)) < 0 {
            say!("tapestry-battery: FAIL claim: split A refused");
            return 1;
        }
        let fresh = read_file(root, "layout").unwrap_or_default();
        let eid = match empty_pane(&fresh) {
            Some(id) => id,
            None => {
                say!("tapestry-battery: FAIL claim: no focused empty leaf after split");
                return 1;
            }
        };
        let claim_path = alloc::format!("pane/{}/claim", eid);
        let stale = match read_claim(root, &claim_path) {
            Some(t) => t,
            None => {
                say!("tapestry-battery: FAIL claim: first mint unreadable");
                return 1;
            }
        };
        let live = match read_claim(root, &claim_path) {
            Some(t) => t,
            None => {
                say!("tapestry-battery: FAIL claim: second mint unreadable");
                return 1;
            }
        };
        if stale == live {
            say!("tapestry-battery: FAIL claim: two mints returned one token");
            return 1;
        }
        if raw_write(root, "layout", &alloc::format!("focus {}", pa2.id)) < 0 {
            say!("tapestry-battery: FAIL claim: focus back on A refused");
            return 1;
        }
        let rc = raw_create(root, "create 16 16 claim=nothex");
        if rc != -22 {
            say!(
                "tapestry-battery: FAIL claim: malformed token rc {} want -22 (E_INVAL)",
                rc
            );
            return 1;
        }
        let c1 = match Surface::open_claim(disp.0 / 2, disp.1 / 2, stale) {
            Ok(s) => s,
            Err(e) => {
                say!(
                    "tapestry-battery: FAIL claim: stale token refused {:?} (want fallback)",
                    e
                );
                return 1;
            }
        };
        let fresh = read_file(root, "layout").unwrap_or_default();
        match find_pane(&fresh, c1.id) {
            Some(pc) if pc.id != eid => {}
            other => {
                say!(
                    "tapestry-battery: FAIL claim: stale token steered into {:?} (E is {})",
                    other.map(|p| p.id),
                    eid
                );
                return 1;
            }
        }
        let e_surf = read_file(root, &alloc::format!("pane/{}/surface", eid)).unwrap_or_default();
        if e_surf.trim() != "none" {
            say!(
                "tapestry-battery: FAIL claim: E not empty after the stale fallback ('{}')",
                e_surf.trim()
            );
            return 1;
        }
        let c2 = match Surface::open_claim(disp.0 / 2, disp.1 / 2, live) {
            Ok(s) => s,
            Err(e) => {
                say!("tapestry-battery: FAIL claim: live token refused {:?}", e);
                return 1;
            }
        };
        let fresh = read_file(root, "layout").unwrap_or_default();
        match find_pane(&fresh, c2.id) {
            Some(pc) if pc.id == eid => {}
            other => {
                say!(
                    "tapestry-battery: FAIL claim: live token landed in {:?}, want E ({})",
                    other.map(|p| p.id),
                    eid
                );
                return 1;
            }
        }
        drop(c2);
        drop(c1);
        say!("battery: claim OK");
    }

    // The declared-handoff control, one variable away from the undeclared
    // check above: the SAME console leaf goes to the ZERO rect the moment the
    // conn HOSTING A declares `session on` (a standalone Surface owns its own
    // conn; `root` here is a separate, idle one -- an idle declaration holds
    // no display, by design), and gets its column back on `session off`.
    // Without it, "tiled because undeclared" is indistinguishable from
    // "backgrounding is absent".
    {
        let fresh = read_file(root, "layout").unwrap_or_default();
        let Some(cid) = foreign_pane(&fresh, &[a.id]) else {
            say!("tapestry-battery: FAIL declare control: no console leaf to background");
            return 1;
        };
        let (w0, h0) = pane_wh(root, cid);
        if w0 == 0 || h0 == 0 {
            say!("tapestry-battery: FAIL declare control: console leaf not tiled beforehand");
            return 1;
        }
        if let Err(e) = a.global_ctl("session on") {
            say!(
                "tapestry-battery: FAIL declare control: session on refused {:?}",
                e
            );
            return 1;
        }
        let (w1, h1) = pane_wh(root, cid);
        if w1 != 0 || h1 != 0 {
            say!(
                "tapestry-battery: FAIL declare control: console leaf still tiled {} {} under a declared session",
                w1,
                h1
            );
            return 1;
        }
        say!("battery: console leaf backgrounded by the declaration");
        // The round-1 C-F2 regression: a DECLARED session's `close` on the
        // container holding the (transparent, backgrounded) console leaf is
        // refused -- the destructive verb sees the whole subtree, not the
        // session's view of it. Pre-fix the close succeeded and the console
        // renderer received TEV_CLOSE.
        let Some(rid) = layout_root_id(&fresh) else {
            say!("tapestry-battery: FAIL declare control: no root container in the layout");
            return 1;
        };
        let rc = raw_write(root, &alloc::format!("pane/{}/ctl", rid), "close");
        if rc != -1 {
            say!(
                "tapestry-battery: FAIL declare control: close of the console's container rc {} want -1 (E_PERM)",
                rc
            );
            return 1;
        }
        let (wc, hc) = pane_wh(root, cid);
        if (wc, hc) != (0, 0)
            || foreign_pane(&read_file(root, "layout").unwrap_or_default(), &[a.id]).is_none()
        {
            say!("tapestry-battery: FAIL declare control: the refused close still disturbed the console leaf");
            return 1;
        }
        say!("battery: declared session cannot close the console's container");
        if let Err(e) = a.global_ctl("session off") {
            say!(
                "tapestry-battery: FAIL declare control: session off refused {:?}",
                e
            );
            return 1;
        }
        let (w2, h2) = pane_wh(root, cid);
        if (w2, h2) != (w0, h0) {
            say!(
                "tapestry-battery: FAIL declare control: console leaf {} {} after session off, want {} {}",
                w2,
                h2,
                w0,
                h0
            );
            return 1;
        }
        say!("battery: console leaf restored {} {}", w2, h2);
    }

    // Focus returns to the console's pane by the CLOSE path, not by a write
    // from here: focusing a pane we do not own is exactly what the pane-tree
    // gate refuses (the H-3b round F2), and when our last surface retires
    // its leaf closes and the layout re-focuses the survivor.
    unsafe { t_close(root) };
    say!("tapestry-battery: PASS");
    // `a` drops on return (`b` already did, scenario 3): the surfaces
    // retire, the panes collapse, and the console pane returns to
    // fullscreen direct scanout.
    0
}
