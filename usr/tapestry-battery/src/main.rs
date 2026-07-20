// /bin/tapestry-battery -- the G-6 compositor acceptance battery's
// in-guest half (TAPESTRY.md section 18.9, the G-6 gate: "the acceptance
// battery on synthetic clients"). One process hosts BOTH synthetic
// clients (two private libtapestry sessions) AND the layout driver (a
// third session writing the `layout` file), so the scenario is
// deterministic and single-threaded; the host half (the ls-gfx-panes
// expect scenario) types keys over QMP and pixel-asserts screendumps at
// the pane centers this binary prints.
//
// The G-6a leg (`panes`, the default):
//   1. client A creates a display-sized surface, fills RED, presents ->
//      auto-hosts by splitting the console's root leaf (aurora | A);
//   2. client B creates a half-sized surface, fills BLUE, presents ->
//      auto-hosts by splitting A's leaf (the aspect flips the
//      orientation: a NESTED splitv -- aurora | [A / B]);
//   3. both re-present (the second hosting's chrome repaint blanked
//      A's region -- panes heal on their next present, by design);
//   4. reads `layout`, parses its own panes, cross-checks each against
//      pane/<id>/geometry, asserts disjoint nonzero rects
//      ("structure OK"), prints machine-parseable center lines;
//   5. the focus legs: focuses A's pane, waits for a QMP-typed key on
//      A's event stream (proving focused-leaf routing), then focuses
//      B's pane and waits for a key there;
//   6. restores focus to the console's pane and exits (Drop retires the
//      surfaces; the panes collapse; the console returns fullscreen).
//
// Clipping is exercised deliberately: A (display-sized) is larger than
// its pane -> the compose blit crops it; solid fills keep the center
// pixel asserts exact either way.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::{t_close, t_open, t_read, t_write, T_OREAD, T_OWRITE, T_WALK_OPEN_FROM_ROOT};
use tapestry::{Surface, TEV_KEY};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

const RED: u32 = 0xFFFF_0000;
const BLUE: u32 = 0xFF00_00FF;

/// Give up a focus-leg wait after this many non-key events (~20 s of
/// FRAME ticks): the host harness failed; exit loudly instead of hanging.
const LEG_EVENT_BUDGET: u32 = 1200;

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
        return Some(PaneInfo { id, x, y, w, h });
    }
    None
}

fn fill(surf: &mut Surface, color: u32) {
    for p in surf.pixels().iter_mut() {
        *p = color;
    }
}

fn overlap(a: PaneInfo, b: PaneInfo) -> bool {
    a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h
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

    // Client A: display-sized (will be cropped into its pane). Client B:
    // half-sized (fits its quarter-ish pane loosely).
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
    say!("battery: A pane={} center={} {}", pa.id, pa.x + pa.w / 2, pa.y + pa.h / 2);
    say!("battery: B pane={} center={} {}", pb.id, pb.x + pb.w / 2, pb.y + pb.h / 2);

    // Focus leg 1: A takes focus; a QMP-typed key must arrive on A's
    // stream (and nowhere else -- the exp asserts no "battery: B key"
    // before the switch).
    if !write_file(root, "layout", &alloc::format!("focus {}", pa.id)) {
        say!("tapestry-battery: FAIL focus A");
        return 1;
    }
    say!("tapestry-battery: panes ready");
    if !wait_key(&mut a, "A") {
        return 1;
    }
    // Focus leg 2: switch to B via B's own pane ctl (exercising the
    // per-pane ctl verb path, vs the layout-file path used for A).
    if !write_file(root, &alloc::format!("pane/{}/ctl", pb.id), "focus") {
        say!("tapestry-battery: FAIL focus B");
        return 1;
    }
    say!("tapestry-battery: focus B");
    if !wait_key(&mut b, "B") {
        return 1;
    }

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
                    if n != a.id && n != b.id {
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
    // (a, b) drop on return: the surfaces retire, their panes collapse,
    // and the console pane returns to fullscreen direct scanout.
    0
}
