// /bin/aurora -- the Aurora renderer MVP (Tapestry G-4; AURORA.md section 3).
//
// The screen-side of the terminal protocol: an ORDINARY tapestryd client
// (libtapestry, the same protocol the demo proved) that interprets the
// console byte stream into a Cornucopia-set cell grid and presents it --
// the fbcon claim. The kernel side is the G-4 drain/feed pair:
//
//   /dev/consdrain  -- every EL0 console-output byte (program output +
//                      line-discipline echo) rings here; Aurora reads +
//                      interprets (vt.rs) + blits (render.rs).
//   /dev/consfeed   -- Aurora's decoded keyboard runes enter the EXISTING
//                      LS-8 line discipline (cooking/ECHO/ISIG unchanged);
//                      the echo comes back through the drain, so typing
//                      paints itself.
//
// Both leaves are SPAWN_PERM_CONSOLE_RENDERER-gated (the I-27 third console
// role -- no elevation authority, no Ctrl-C-target authority); joey spawns
// aurora WITH the perm during bringup. Kernel diagnostics (SYS_PUTS,
// extinction) stay serial-only by design -- the screen shows the SESSION.
//
// The loop BLOCKS on the tapestry event stream (wait_event) -- the 60 Hz
// FRAME clock is the heartbeat, so every pass turns within one frame. This
// is load-bearing, not a style choice: a non-SQPOLL Loom ring's completions
// are pumped by the thread BLOCKED in enter (the Loom-4 CQ-wait drives the
// elected 9P reader); a never-blocking reap loop would never pump and no
// event CQE would ever materialize (measured: the frame clock went silent
// under a poll-only loop). Each wake handles the event (KEY -> feed;
// FRAME -> cursor blink; CLOSE -> exit), drains further queued events,
// services the drain NON-blockingly (bounded reads per pass -- a burst
// larger than the pass budget rides the kernel ring's drop-oldest, the
// skip-ahead a fast scroll wants), then renders the contiguous dirty row
// span into the CURRENT slot and presents exactly that rect (slots rotate
// per present, so presenting rows the pass did not just render would
// transfer stale slot content -- the span render keeps every transferred
// row fresh).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

mod render;
mod vt;

use alloc::vec::Vec;
use cornucopia::Atlas;
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{
    t_open, t_poll, t_read, t_write, TPollFd, T_OREAD, T_OWRITE, T_POLLIN,
    T_WALK_OPEN_FROM_ROOT,
};
use render::{render_rows, Metrics};
use tapestry::{Rect, Surface, TEV_CLOSE, TEV_FRAME, TEV_KEY};
use vt::{Vt, BG};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

const CONNECT_TRIES: u32 = 25;
const CONNECT_DELAY_MS: u64 = 200;
const BLINK_FRAMES: u64 = 30; // cursor phase flip every half second at 60 Hz

fn open_path(path: &str, omode: u32) -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), omode) }
}

/// Translate one KEY event into the terminal byte sequence a keyboard would
/// produce. Press + autorepeat feed; release is silent. Runes are fed as
/// UTF-8 (the tapestryd keymap already resolved shift/ctrl -- Enter is CR,
/// Ctrl-C is 0x03, which ISIG cooks kernel-side); non-rune keys map to the
/// classic CSI sequences ut's editor and Kaua both parse.
fn key_bytes(code: u16, value: u32, rune: u32, out: &mut Vec<u8>) {
    if value == 0 {
        return; // release
    }
    if rune != 0 {
        if let Some(ch) = char::from_u32(rune) {
            let mut b = [0u8; 4];
            out.extend_from_slice(ch.encode_utf8(&mut b).as_bytes());
        }
        return;
    }
    let seq: &[u8] = match code {
        103 => b"\x1b[A", // Up
        108 => b"\x1b[B", // Down
        106 => b"\x1b[C", // Right
        105 => b"\x1b[D", // Left
        102 => b"\x1b[H", // Home
        107 => b"\x1b[F", // End
        104 => b"\x1b[5~", // PgUp
        109 => b"\x1b[6~", // PgDn
        111 => b"\x1b[3~", // Delete
        110 => b"\x1b[2~", // Insert
        _ => return,
    };
    out.extend_from_slice(seq);
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    if !Atlas::verify() {
        say!("aurora: FAIL atlas magic/version (rebake tools/bake-cornucopia.py)");
        return 1;
    }

    // The drain/feed pair FIRST: without the renderer grant these opens fail
    // and the surface should never be created (fail loudly, leave the
    // scanout to whoever else presents).
    let drain = open_path("/dev/consdrain", T_OREAD);
    if drain < 0 {
        say!("aurora: FAIL open /dev/consdrain (not the bound renderer?)");
        return 1;
    }
    let feed = open_path("/dev/consfeed", T_OWRITE);
    if feed < 0 {
        say!("aurora: FAIL open /dev/consfeed");
        return 1;
    }

    // Bounded connect retry (the demo discipline): tapestryd is
    // warden-spawned long before this, but a slow bring-up must not flake.
    let mut surf: Option<Surface> = None;
    for i in 0..CONNECT_TRIES {
        match Surface::fullscreen() {
            Ok(s) => {
                surf = Some(s);
                break;
            }
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("aurora: FAIL connect/create {:?}", e);
                    return 1;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
            }
        }
    }
    let mut surf = surf.unwrap();
    let (w, h) = (surf.w as usize, surf.h as usize);

    let m = Metrics {
        cell_w: Atlas::cell_w(),
        cell_h: Atlas::cell_h(),
        baseline: Atlas::baseline(),
        off_x: 0,
        off_y: 0,
    };
    let cols = w / m.cell_w;
    let rows = h / m.cell_h;
    if cols < 20 || rows < 5 {
        say!("aurora: FAIL degenerate grid {}x{} on {}x{}", cols, rows, w, h);
        return 1;
    }
    let mut term = Vt::new(cols, rows);

    // Frame 0: clear the whole mode to the Bonfire bg (the margins outside
    // the grid stay this fill forever) + the initial empty grid + cursor.
    {
        let px = surf.pixels();
        for p in px.iter_mut() {
            *p = BG;
        }
        render_rows(&term, &m, px, w, 0, rows, Some((0, 0)));
    }
    if let Err(e) = surf.present(None) {
        say!("aurora: FAIL first present {:?}", e);
        return 1;
    }
    for d in term.dirty.iter_mut() {
        *d = false;
    }
    say!("aurora: console up {}x{} cells ({}x{} px, cell {}x{})",
         cols, rows, w, h, m.cell_w, m.cell_h);

    let mut frames: u64 = 0;
    let mut blink_on = true;
    let mut prev_cursor: Option<(usize, usize)> = Some((0, 0));
    let mut keybuf: Vec<u8> = Vec::new();
    let mut drainbuf = [0u8; 2048];
    let mut drain_eof = false;

    loop {
        // (1) Block for the next event (<= one FRAME period), then drain the
        // locally-queued backlog non-blockingly.
        let first = match surf.wait_event() {
            Ok(ev) => ev,
            Err(_) => {
                say!("aurora: event stream ended (compositor gone); exiting");
                return 1;
            }
        };
        let mut ev = Some(first);
        while let Some(e) = ev {
            match e.kind {
                TEV_KEY => {
                    keybuf.clear();
                    key_bytes(e.code, e.value, e.rune, &mut keybuf);
                    if !keybuf.is_empty() {
                        let _ = unsafe { t_write(feed, keybuf.as_ptr(), keybuf.len()) };
                    }
                }
                TEV_FRAME => {
                    frames += 1;
                    if frames % BLINK_FRAMES == 0 {
                        blink_on = !blink_on;
                        term.dirty[term.cy] = true; // repaint the cursor cell
                    }
                }
                TEV_CLOSE => {
                    say!("aurora: CLOSE received; exiting");
                    return 0;
                }
                _ => {}
            }
            ev = match surf.poll_event() {
                Ok(next) => next,
                Err(_) => {
                    say!("aurora: event stream ended (compositor gone); exiting");
                    return 1;
                }
            };
        }

        // (2) The drain, non-blocking: read while ready, bounded per pass.
        if !drain_eof {
            for _ in 0..8 {
                let mut pfd =
                    [TPollFd { fd: drain as i32, events: T_POLLIN, revents: 0 }];
                let rc = unsafe { t_poll(pfd.as_mut_ptr(), 1, 0) };
                if rc <= 0 || (pfd[0].revents & T_POLLIN) == 0 {
                    break;
                }
                let n = unsafe { t_read(drain, drainbuf.as_mut_ptr(), drainbuf.len()) };
                if n > 0 {
                    term.feed(&drainbuf[..n as usize]);
                } else {
                    if n == 0 {
                        say!("aurora: drain EOF (backend disarmed); staying up");
                        drain_eof = true;
                    }
                    break;
                }
            }
        }

        // (3) Damage: cursor movement dirties its old + new rows; then the
        // contiguous dirty span renders into the CURRENT slot + presents.
        let cursor = if term.cursor_visible && blink_on {
            Some((term.cx.min(cols - 1), term.cy))
        } else {
            None
        };
        if cursor != prev_cursor {
            if let Some((_, r)) = prev_cursor {
                term.dirty[r] = true;
            }
            if let Some((_, r)) = cursor {
                term.dirty[r] = true;
            }
        }
        let mut r0 = usize::MAX;
        let mut r1 = 0usize;
        for (r, d) in term.dirty.iter().enumerate() {
            if *d {
                if r < r0 {
                    r0 = r;
                }
                r1 = r + 1;
            }
        }
        if r0 < r1 {
            {
                let px = surf.pixels();
                render_rows(&term, &m, px, w, r0, r1, cursor);
            }
            let rect = Rect {
                x: 0,
                y: (m.off_y + r0 * m.cell_h) as u32,
                w: w as u32,
                h: ((r1 - r0) * m.cell_h) as u32,
            };
            if let Err(e) = surf.present(Some(rect)) {
                say!("aurora: FAIL present {:?}", e);
                return 1;
            }
            for d in term.dirty[r0..r1].iter_mut() {
                *d = false;
            }
            prev_cursor = cursor;
        }
    }
}
