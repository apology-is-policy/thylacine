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
//   tabbed      : mode tabbed on [A/B] -- A hides, the D7 glyph-free
//                 strip paints (segment colors sampled), `tab next`
//                 cycles the active child (G-6c);
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

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::String;

use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{t_close, t_open, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT};
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
        return Some(PaneInfo { id, x, y, w, h, hidden: line.ends_with("hidden") });
    }
    None
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
    let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry".as_ptr(), 13, T_OREAD) };
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
        say!("tapestry-battery: FAIL rects (a=[{},{},{},{}] b=[{},{},{},{}])",
            pa.x, pa.y, pa.w, pa.h, pb.x, pb.y, pb.w, pb.h);
        return 1;
    }
    if pa.x + pa.w > disp.0 || pa.y + pa.h > disp.1 || pb.x + pb.w > disp.0
        || pb.y + pb.h > disp.1
    {
        say!("tapestry-battery: FAIL rects out of display");
        return 1;
    }
    for (p, nm) in [(pa, "A"), (pb, "B")] {
        let path = alloc::format!("pane/{}/geometry", p.id);
        let g = read_file(root, &path).unwrap_or_default();
        let want = alloc::format!("{} {} {} {}", p.x, p.y, p.w, p.h);
        if g.trim() != want {
            say!("tapestry-battery: FAIL {} geometry file '{}' != layout '{}'",
                nm, g.trim(), want);
            return 1;
        }
    }
    say!("tapestry-battery: structure OK");
    say!("battery: stage1 centers {} {} {} {}",
        pa.x + pa.w / 2, pa.y + pa.h / 2, pb.x + pb.w / 2, pb.y + pb.h / 2);
    nap(DUMP_MS);

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
            say!("tapestry-battery: FAIL reweave geometry '{}' != '{}'", g.trim(), want);
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
            Rect { x: 0, y: 0, w: bw / 2, h: bh },
            Rect { x: bw / 2, y: 0, w: bw - bw / 2, h: bh },
        ];
        if b.present_rects(&rects).is_err() {
            say!("tapestry-battery: FAIL multirect present");
            return 1;
        }
        say!("battery: multirect ready {} {} {} {}",
            pb.x + bw / 4, pb.y + bh / 2, pb.x + 3 * bw / 4, pb.y + bh / 2);
        nap(DUMP_MS);
    }
    fill(&mut b, BLUE);
    if b.present(None).is_err() {
        say!("tapestry-battery: FAIL post-multirect restore");
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
            say!("tapestry-battery: FAIL tabbed visibility (A hidden={} B hidden={})",
                ta.hidden, tb.hidden);
            return 1;
        }
        // B heals into the enlarged tab content (its bottom is cropped
        // until it re-acks; solid blue keeps the samples exact).
        fill(&mut b, BLUE);
        let _ = b.present(None);
        // The strip geometry from B's content rect: the container's
        // outer rect is content + the 1px leaf inset on x, and strip(5)
        // + inset above it on y (TAB_STRIP_H = 5).
        let cx = tb.x - 1;
        let cw = tb.w + 2;
        let sy = tb.y - 1 - 5 + 2; // strip row center
        let sax = cx + cw / 4;
        let sbx = cx + 3 * cw / 4;
        say!("battery: tabbed ready {} {} {}", sy, sax, sbx);
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
            say!("tapestry-battery: FAIL zoom geometry '{}' != '{}'", g.trim(), wantg);
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
            say!("tapestry-battery: FAIL unzoom geometry '{}' != '{}'", g.trim(), wantg);
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
        if mb.x >= ma.x || mb.x < disp.0 / 6 {
            // B must sit BETWEEN aurora (the leftmost third) and A -- a
            // B at the left edge means the pull-out landed wrong.
            say!("tapestry-battery: FAIL move-left order (B.x={} A.x={})", mb.x, ma.x);
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
                    if ev.kind == TEV_KEY
                        && matches!(ev.code, 103 | 105 | 106 | 108)
                    {
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
        let t1 = ctl_u64(&read_file(root, "ctl").unwrap_or_default(), "tick ")
            .unwrap_or(u64::MAX - 1);
        if t0 != t1 {
            say!("tapestry-battery: FAIL frozen clock advanced ({} -> {})", t0, t1);
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
    say!("battery: gate OK");

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
    if !read_file(root, "ctl").unwrap_or_default().contains("test-mode off") {
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

    // Restore focus to the console's pane (the leaf hosting neither A nor
    // B), so the session keyboard works the instant we exit.
    if let Some(fresh) = read_file(root, "layout") {
        let mut done = false;
        for line in fresh.lines() {
            let line = line.trim();
            if !line.contains(" leaf ") || !line.contains("surface=") {
                continue;
            }
            if let Some(tok) = line.split_ascii_whitespace().find(|t| t.starts_with("surface="))
            {
                let n: Option<u32> = tok["surface=".len()..].parse().ok();
                if let Some(n) = n {
                    if n != a.id && n != b_id {
                        if let Some(info) = find_pane(&fresh, n) {
                            let _ = write_file(root, "layout",
                                &alloc::format!("focus {}", info.id));
                            done = true;
                            break;
                        }
                    }
                }
            }
        }
        if !done {
            say!("battery: no console pane to refocus (headless run)");
        }
    }
    unsafe { t_close(root) };
    say!("tapestry-battery: PASS");
    // `a` drops on return (`b` already did, scenario 3): the surfaces
    // retire, the panes collapse, and the console pane returns to
    // fullscreen direct scanout.
    0
}
