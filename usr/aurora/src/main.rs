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

mod config;
mod osd;
mod render;
mod vt;

use alloc::string::String;
use alloc::vec::Vec;
use cornucopia::Atlas;
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{
    t_open, t_poll, t_read, t_write, TPollFd, T_OREAD, T_OWRITE, T_POLLIN,
    T_WALK_OPEN_FROM_ROOT,
};
use render::{render_rows, Metrics};
use tapestry::{Rect, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_FRAME, TEV_KEY};
use vt::Vt;

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

    // #55: the winsize writer (ARCH 23.5.3). Aurora reports its cell grid via
    // the consctl `winsize <cols> <rows>` verb -- the renderer-widened mint
    // gate lets it self-serve by name like the drain/feed pair. Best-effort:
    // a failed open leaves /dev/winsize at `winsize 0 0` and clients fall
    // back to the CPR probe (the serial posture) -- degraded, never fatal.
    let consctl = open_path("/dev/consctl", T_OWRITE);
    if consctl < 0 {
        say!("aurora: /dev/consctl open failed (winsize reporting off)");
    }

    // cfg-2a/cfg-3: the system-tier config seeds settings BEFORE the
    // connect, so the compositor tier can push AHEAD of the surface create
    // -- the console surface is then born at the configured mode (no
    // boot-time reweave) and the pre-login screen wears the persisted
    // theme (the monitor-OSD semantic).
    let mut settings = osd::Settings::new();
    config::load(&mut settings);
    if let osd::Mode::Fixed(mw, mh) = settings.mode {
        // Push-on-start (AURORA-CONFIG.md section 3.1): the gated `mode`
        // verb on aurora's OWN throwaway conn (the gate's peer identity is
        // per-conn; aurora holds the renderer role). Bounded retry only
        // while tapestryd is not yet accepting; best-effort -- a refused
        // or failed push leaves the default mode and the OSD still works.
        let mut pushed = false;
        for _ in 0..CONNECT_TRIES {
            match tapestry::global_ctl_once(&alloc::format!("mode {} {}", mw, mh)) {
                Ok(()) => {
                    pushed = true;
                    break;
                }
                Err(TapError::Connect) => {
                    let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
                }
                Err(e) => {
                    say!("aurora: startup mode push ({}x{}) write failed {:?}", mw, mh, e);
                    pushed = true; // reported; don't double-log below
                    break;
                }
            }
        }
        if !pushed {
            say!("aurora: startup mode push ({}x{}) no compositor", mw, mh);
        } else {
            // Verify-readback: one diagnostic line per fixed-mode boot --
            // the push is load-bearing for the whole session's geometry,
            // so its outcome must be evidence, not silence.
            match tapestry::display_dims() {
                Some((dw, dh)) if dw == mw && dh == mh => {
                    say!("aurora: mode push verified {}x{}", dw, dh);
                }
                Some((dw, dh)) => {
                    say!("aurora: mode push MISMATCH want {}x{} got {}x{}", mw, mh, dw, dh);
                }
                None => say!("aurora: mode push verify read failed"),
            }
        }
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
    let (mut w, mut h) = (surf.w as usize, surf.h as usize);

    // cfg-4: push the compositor tier (chords + gaps) on aurora's OWN
    // renderer conn (surf.global_ctl -- the gated ctl; aurora holds the
    // renderer role). These do NOT affect the surface geometry (unlike
    // the pre-connect mode push), so they ride the live surface conn.
    // Reset-first (the environment discipline): a config that REMOVES a
    // chord line reverts it to the compiled default. Best-effort -- a
    // refused/failed push leaves the defaults; a buggy config never
    // wedges aurora.
    if !settings.chords.is_empty() {
        let _ = surf.global_ctl("chord-reset");
        for (combo, action) in &settings.chords {
            let _ = surf.global_ctl(&alloc::format!("chord {} {}", combo, action));
        }
    }
    if let Some(g) = settings.gaps {
        let _ = surf.global_ctl(&alloc::format!("gaps {}", g));
    }
    if !settings.chords.is_empty() || settings.gaps.is_some() {
        // A single startup record of the cfg-4 push (the honest "what did
        // the config change" line; the ls-gfx-chords skip-probe gates on
        // the config CONTENT in-guest, not this line).
        match settings.gaps {
            Some(g) => say!("aurora: cfg-4 pushed {} chord(s), gaps={}",
                            settings.chords.len(), g),
            None => say!("aurora: cfg-4 pushed {} chord(s), gaps=default",
                         settings.chords.len()),
        }
    }

    let m = Metrics {
        cell_w: Atlas::cell_w(),
        cell_h: Atlas::cell_h(),
        baseline: Atlas::baseline(),
        off_x: 0,
        off_y: 0,
    };
    let mut cols = w / m.cell_w;
    let mut rows = h / m.cell_h;
    if cols < 20 || rows < 5 {
        say!("aurora: FAIL degenerate grid {}x{} on {}x{}", cols, rows, w, h);
        return 1;
    }
    let mut term = Vt::new(cols, rows);

    // The F10 settings overlay (osd.rs); settings were loaded pre-connect
    // (the theme applies here, where the Vt exists).
    if settings.theme != 0 {
        term.set_theme(settings.theme);
    }
    let mut ui = osd::Osd::new();

    // Frame 0: clear the whole mode to the theme bg (the margins outside
    // the grid stay this fill until a theme change or reweave refills) +
    // the initial empty grid + cursor.
    {
        let bg = term.pal.bg;
        let px = surf.pixels();
        for p in px.iter_mut() {
            *p = bg;
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

    // #55: report the boot geometry -- /dev/winsize serves real cells from
    // the first present on. The 0x0 -> real transition IS a change, so the
    // kernel attempts a winch, but the bringup console owner (joey) sits in
    // the pgid-0 boot group, which notes_post_pgrp refuses -- the boot write
    // is silent by construction.
    write_winsize(consctl, cols, rows);

    let mut frames: u64 = 0;
    let mut blink_on = true;
    let mut prev_cursor: Option<(usize, usize)> = Some((0, 0));
    let mut keybuf: Vec<u8> = Vec::new();
    let mut drainbuf = [0u8; 2048];
    let mut drain_eof = false;
    // Consecutive present failures. A transient failure (#31: a compositor
    // GPU hiccup) is a DROPPED FRAME, never death -- the dirty rows stay
    // set so the next pass re-renders + re-presents. Real compositor death
    // ends the event stream (the wait_event exit above); this backstop only
    // catches a live-stream-but-presents-never-succeed wedge.
    let mut present_fails: u32 = 0;
    const PRESENT_FAILS_FATAL: u32 = 240;
    // #55: a reweave hands the damage pass one FULL frame (BG-fill the whole
    // slot + render every row + present(None)) instead of the rect path --
    // the new generation's slots are zeroed, so the margins need the fill.
    // Cleared only on a successful present (slots rotate per attempt, so a
    // retry must re-fill; the same discipline as the dirty rows).
    let mut full_fill = false;

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
                    // The OSD is MODAL: while open, every key routes to it
                    // and nothing feeds the terminal. Bare F10 (which
                    // key_bytes always dropped -- no app ever saw it) opens
                    // it; press-only (value 1) so the opening key's own
                    // autorepeat cannot bounce it shut.
                    if ui.open {
                        match ui.handle_key(e.code, e.value, &mut settings) {
                            osd::OsdOut::ThemeChanged => {
                                term.set_theme(settings.theme);
                                // cfg-2a write-through (the monitor-OSD
                                // semantic). Best-effort: a failed save
                                // never disturbs the live settings.
                                if !config::save(&settings) {
                                    say!("aurora: config save failed ({})",
                                         config::CONFIG_PATH);
                                }
                            }
                            osd::OsdOut::SettingChanged => {
                                if !config::save(&settings) {
                                    say!("aurora: config save failed ({})",
                                         config::CONFIG_PATH);
                                }
                            }
                            osd::OsdOut::ModeApply(mode) => {
                                // cfg-3: the compositor tier rides the
                                // GATED ctl on aurora's own conn. Persist
                                // ONLY on an accepted apply -- a refused/
                                // failed write must not seed the startup
                                // push with a mode the compositor never
                                // took.
                                let cmd = match mode {
                                    osd::Mode::Auto => String::from("mode auto"),
                                    osd::Mode::Fixed(w, h) => {
                                        alloc::format!("mode {} {}", w, h)
                                    }
                                };
                                if surf.global_ctl(&cmd).is_ok() {
                                    settings.mode = mode;
                                    if !config::save(&settings) {
                                        say!("aurora: config save failed ({})",
                                             config::CONFIG_PATH);
                                    }
                                } else {
                                    say!("aurora: mode apply refused ({})", cmd);
                                }
                            }
                            osd::OsdOut::Close => full_fill = true,
                            osd::OsdOut::None => {}
                        }
                    } else if e.value == 1 && e.code == osd::KEY_F10 {
                        ui.open_at(&settings);
                    } else {
                        keybuf.clear();
                        key_bytes(e.code, e.value, e.rune, &mut keybuf);
                        if !keybuf.is_empty() {
                            let _ =
                                unsafe { t_write(feed, keybuf.as_ptr(), keybuf.len()) };
                        }
                    }
                }
                TEV_FRAME => {
                    frames += 1;
                    if frames % BLINK_FRAMES == 0 && settings.cursor_blink {
                        blink_on = !blink_on;
                        term.dirty[term.cy] = true; // repaint the cursor cell
                    }
                }
                TEV_CLOSE => {
                    say!("aurora: CLOSE received; exiting");
                    return 0;
                }
                TEV_CONFIGURE => {
                    // The compositor's redraw/resize request (G-6,
                    // TAPESTRY.md 18.3). A same-size CONFIGURE is the
                    // full-REDRAW request: mark the whole grid dirty
                    // (structural repaints blank pane content, and the
                    // row-damage renderer would otherwise heal only rows
                    // that happen to change). A size offer is the #55
                    // REWEAVE (ARCH 23.5.3 / AURORA.md section 4) --
                    // UNLESS the offered grid is sub-floor (< 20x5
                    // cells): acking a degenerate size would strand the
                    // fbcon, so the pre-#55 ignore/crop posture is
                    // retired to exactly that case (keep the grid + old
                    // generation; the compositor crops the top-left).
                    // No diagnostic print on the hot arms: aurora shares
                    // /dev/cons with whatever it renders.
                    let ow = (e.value >> 16) as usize;
                    let oh = (e.value & 0xffff) as usize;
                    let sub_floor =
                        ow / m.cell_w < 20 || oh / m.cell_h < 5;
                    if sub_floor && !(ow == w && oh == h) {
                        for d in term.dirty.iter_mut() {
                            *d = true;
                        }
                    } else {
                        match surf.handle_configure(&e) {
                            Ok(false) => {
                                // Same-size: the redraw request.
                                for d in term.dirty.iter_mut() {
                                    *d = true;
                                }
                            }
                            Ok(true) => {
                                // Reweaved onto the new generation: the
                                // old pixel view is gone. Re-derive the
                                // grid, resize the Vt (cursor-anchored),
                                // and hand the damage pass a FULL frame
                                // (BG-fill + all rows + present(None) --
                                // the new generation's slots are zeroed,
                                // so the margins need the fill).
                                w = surf.w as usize;
                                h = surf.h as usize;
                                // #55 audit F3: the sub-floor gate keys on the
                                // OFFERED size, but the reweave adopts the
                                // acked surf.w/h. tapestryd acks the size it
                                // offered (verified: the G-6b resize protocol
                                // fences on the offer), so the resulting grid
                                // equals the gated one; a hypothetical
                                // ack-smaller-than-offered would slip a tiny
                                // grid past the guard -- memory-safe (.max(1)
                                // + Vt::resize handle cols/rows >= 1), only a
                                // cosmetically-small fbcon, never a panic.
                                cols = (w / m.cell_w).max(1);
                                rows = (h / m.cell_h).max(1);
                                term.resize(cols, rows);
                                full_fill = true;
                                // #55 audit F1: the reweave can SHRINK the
                                // grid below the old cursor row. prev_cursor
                                // is main-loop-local -- Vt::resize clamps
                                // term.cy but never touches it, so the step-3
                                // damage `term.dirty[prev_cursor.row]` would
                                // index past the shrunk dirty vec (an OOB
                                // panic -> no_std abort -> a dark console). A
                                // full_fill repaints every row, so the old
                                // cursor position is meaningless: drop it.
                                prev_cursor = None;
                                // The kernel relays a changed size as
                                // tty:winch to the session (iff-changed
                                // at the verb, so this is post-exact).
                                write_winsize(consctl, cols, rows);
                                // NO diagnostic print: reweaves are routine
                                // steady-state (every pane split/unsplit)
                                // and fire concurrent with session output,
                                // so a SYS_PUTS line here interleaves at
                                // the UART FIFO and tears byte patterns
                                // mid-line (it split the panes battery's
                                // own PASS marker). /dev/winsize + the
                                // ls-gfx winsize leg carry the proof.
                            }
                            Err(TapError::Busy) => {
                                // Stale offer -- a newer CONFIGURE is in
                                // the queue and carries the current one.
                            }
                            Err(e) => {
                                say!("aurora: reweave failed {:?}; exiting", e);
                                return 1;
                            }
                        }
                    }
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
                    // cfg-2b: the in-band settings channel (OSC 7770 -- the
                    // session push). SESSION-SCOPED by scripture (3.2): the
                    // applied values are deliberately NEVER config::save'd
                    // -- only the F10 OSD persists. `reset system` re-seeds
                    // from the system file (aurora-push emits it first, so
                    // every session start = system defaults + user
                    // overrides; a stale prior-session push dies there).
                    if !term.settings_req.is_empty() {
                        let reqs: Vec<String> = term.settings_req.drain(..).collect();
                        for line in reqs {
                            let old = settings.theme;
                            if line == "reset system" {
                                // Re-seed from aurora's OWN file (the mode
                                // it carries was already pushed at boot;
                                // nothing re-pushes here).
                                settings = osd::Settings::new();
                                config::load(&mut settings);
                            } else {
                                // cfg-3: the AUTHORITY-KEY allowlist. The
                                // OSC channel is cosmetic/session-scoped
                                // by scripture (3.2/3.3) and must never
                                // carry `mode` -- a session-injected mode
                                // reaching settings would launder session
                                // authority into the OSD's config::save +
                                // the gated startup push. Only the
                                // renderer-local keys pass.
                                let key =
                                    line.split_whitespace().next().unwrap_or("");
                                if key == "theme" || key == "cursor-blink" {
                                    config::parse(&line, &mut settings);
                                } else {
                                    say!("aurora: OSC settings key {:?} refused",
                                         key);
                                }
                            }
                            if settings.theme != old {
                                term.set_theme(settings.theme);
                            }
                        }
                        term.dirty[term.cy] = true; // reflect a blink change
                    }
                    // Terminal ANSWERS (CPR [6n etc.): the reply is keyboard
                    // input -- write it into the consfeed, the same wire the
                    // key events ride. Kaua's size handshake reads it to
                    // learn the real grid (128x36, not the 80x24 fallback).
                    // A short/failed write only degrades the querier to its
                    // 80x24 fallback, but silently -- log it (G-5 SA-2).
                    if !term.reply.is_empty() {
                        let wr = unsafe {
                            t_write(feed, term.reply.as_ptr(), term.reply.len())
                        };
                        if wr < term.reply.len() as i64 {
                            say!("aurora: consfeed reply write short/failed ({} of {})",
                                 wr, term.reply.len());
                        }
                        term.reply.clear();
                    }
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
        // Blink off (the OSD setting) means always-solid, not never-shown.
        let cursor = if term.cursor_visible && (blink_on || !settings.cursor_blink) {
            Some((term.cx.min(cols - 1), term.cy))
        } else {
            None
        };
        if cursor != prev_cursor {
            // #55 audit F1: bound both marks (mirrors Vt::mark's `row < rows`
            // guard). prev_cursor is reset to None on a shrink reweave above,
            // so this is belt-and-suspenders -- a stale row can never index
            // past the (possibly just-shrunk) dirty vec.
            if let Some((_, r)) = prev_cursor {
                if r < term.dirty.len() {
                    term.dirty[r] = true;
                }
            }
            if let Some((_, r)) = cursor {
                if r < term.dirty.len() {
                    term.dirty[r] = true;
                }
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
        // The OSD composes over a FULL frame (slot rotation: a partial rect
        // could transfer stale panel pixels from an older slot), so an open
        // OSD routes every damaged pass through the full-frame branch.
        let osd_pass = ui.open && (ui.dirty || r0 < r1 || full_fill);
        if full_fill || osd_pass {
            // #55: the post-reweave full frame (the frame-0 pattern applied
            // through the single present site so the retry discipline holds).
            let (dw, dh) = (surf.w, surf.h);
            {
                let bg = term.pal.bg;
                let px = surf.pixels();
                for p in px.iter_mut() {
                    *p = bg;
                }
                render_rows(&term, &m, px, w, 0, rows, cursor);
                if ui.open {
                    ui.draw(px, w, &m, cols, rows, &settings, dw, dh);
                }
            }
            if let Err(e) = surf.present(None) {
                present_fails += 1;
                if present_fails <= 3 || present_fails % 64 == 0 {
                    say!("aurora: FAIL present {:?} ({} consecutive); frame dropped", e, present_fails);
                }
                if present_fails >= PRESENT_FAILS_FATAL {
                    say!("aurora: presents failing persistently; exiting");
                    return 1;
                }
                // full_fill + dirty (rows AND ui) stay set: the retry
                // re-fills + re-composes the next slot.
            } else {
                present_fails = 0;
                full_fill = false;
                ui.dirty = false;
                for d in term.dirty.iter_mut() {
                    *d = false;
                }
                prev_cursor = cursor;
            }
        } else if r0 < r1 {
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
                present_fails += 1;
                if present_fails <= 3 || present_fails % 64 == 0 {
                    say!("aurora: FAIL present {:?} ({} consecutive); frame dropped", e, present_fails);
                }
                if present_fails >= PRESENT_FAILS_FATAL {
                    say!("aurora: presents failing persistently; exiting");
                    return 1;
                }
                // Keep the dirty rows + prev_cursor: slots rotate per
                // present, so the retry MUST re-render before re-presenting.
            } else {
                present_fails = 0;
                for d in term.dirty[r0..r1].iter_mut() {
                    *d = false;
                }
                prev_cursor = cursor;
            }
        }
    }
}

// #55: write `winsize <cols> <rows>` to the consctl fd (the ARCH 23.5.3
// verb). Best-effort -- a short/failed write only leaves /dev/winsize stale
// (clients fall back to CPR); the renderer must never die over it.
fn write_winsize(fd: i64, cols: usize, rows: usize) {
    if fd < 0 {
        return;
    }
    let mut buf = [0u8; 24];
    let mut n = 0usize;
    for b in b"winsize " {
        buf[n] = *b;
        n += 1;
    }
    n += fmt_dec(&mut buf[n..], cols);
    buf[n] = b' ';
    n += 1;
    n += fmt_dec(&mut buf[n..], rows);
    let wr = unsafe { t_write(fd, buf.as_ptr(), n) };
    if wr < n as i64 {
        say!("aurora: consctl winsize write short/failed ({} of {})", wr, n);
    }
}

// Render `v` (clamped to the u16 band the verb accepts) in decimal.
fn fmt_dec(out: &mut [u8], v: usize) -> usize {
    let mut v = if v > 65535 { 65535 } else { v };
    let mut tmp = [0u8; 5];
    let mut t = 0usize;
    loop {
        tmp[t] = b'0' + (v % 10) as u8;
        t += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    for i in 0..t {
        out[i] = tmp[t - 1 - i];
    }
    t
}
