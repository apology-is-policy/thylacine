// halcyond (bin) -- the body: the console renderer that owns the scanout
// and weaves the transcript (HALCYON.md sections 4 + 13). The loop is
// aurora's proven shape (drain/feed/consctl + Surface + the event loop,
// including the #129/#135 held-feed discipline, whose policy lives in the
// LIB where it is host-tested); what differs is what a frame IS: aurora
// paints a cell grid, halcyond lays transcript blocks through the cartoon
// CPU executor (the section-13.1 universal floor).
//
// Advertises `beacon rich` -- the FIRST rich advertiser (the H-1 F7/F10
// obligations land against this). Programs then emit rich frames; the
// transcript realizes them; `strip(rich)` fidelity is the emitters'
// pinned property.

#![no_std]
#![no_main]

extern crate alloc;

// A 64 MiB LAZY heap (demand-zero; physical pages commit as touched):
// halcyond's working set -- two parsed DejaVu faces, atlas pages, the
// transcript's 13.3 content budget -- does not fit the 4 MiB default,
// and the death is a SILENT exit(1) (the no_std OOM panics into
// t_exits). Found the honest way: the first on-device boot died between
// the rich advertisement and console-up.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAllocN<{ 64 * 1024 * 1024 }> =
    libthyla_rs::alloc::ThylaAllocN;

use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use beacon::verbs::{parse as parse_verbs, Rule};
use halcyond::input::{
    feed_drain_with, key_bytes, normal_key, wait_is_bounded, FeedLoss, Mode, NormalAct,
    FEED_PENDING_MAX, FEED_RETRY_MS,
};
use halcyond::layout::{
    cursor_pos, daylight_sheet, layout_block, layout_pending, render_block, LaidBlock, Sheet,
};
use halcyond::menu::{build_menu, hit_run, obj_of, run_rect, runs_on_row, step_run, Action, Menu};
use halcyond::raster::GlyphSource;
use halcyond::select::{FlatRow, Sel};
use halcyond::transcript::Transcript;
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{
    t_open, t_poll, t_read, t_write, TPollFd, T_OREAD, T_OWRITE, T_POLLHUP, T_POLLIN,
    T_WALK_OPEN_FROM_ROOT,
};
use tapestry::{
    EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_FOCUS, TEV_KEY, TEV_PTR_BTN,
    TEV_PTR_MOVE,
};

// KT-1.5d-2: the per-user session tile -- spawn one kaua-term, ingest its record
// stream into the ii-a Tile model, render it, route input, exit on logout.
use halcyond::input::map_key;
use halcyond::tile::Tile;
use kaua_term::wire::{encode_input, parse_record, FrameDecoder, Input};
use libhalcyon::theme::daylight_palette;
use libthyla_rs::process::{Command, Stdio};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

mod chromeset;
mod menuset;
mod statusset;

/// evdev BTN_LEFT (the tapestry PTR_BTN `code`).
const BTN_LEFT: u16 = 0x110;

const CONNECT_TRIES: u32 = 25;
const CONNECT_DELAY_MS: u64 = 200;

fn open_path(path: &str, omode: u32) -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), omode) }
}

/// Write all of `buf` to a raw fd (the tile down-channel), looping on short
/// writes. A pipe write to a live kaua-term takes the whole buffer; a failure
/// (<=0) means the tile is gone and stops the loop -- the up-pipe EOF / exit
/// latch is the authoritative teardown, so a lost down-write is harmless here.
fn write_all_fd(fd: i64, buf: &[u8]) {
    let mut off = 0usize;
    while off < buf.len() {
        let w = unsafe { t_write(fd, buf.as_ptr().add(off), buf.len() - off) };
        if w <= 0 {
            break;
        }
        off += w as usize;
    }
}

fn feed_drain(fd: i64, pending: &mut Vec<u8>, dropped: &mut u64, logged: &mut bool) {
    let mut w = |b: &[u8]| unsafe { t_write(fd, b.as_ptr(), b.len()) };
    let loss = feed_drain_with(&mut w, pending, FEED_PENDING_MAX, dropped);
    if loss == FeedLoss::WriteErr {
        if !*logged {
            *logged = true;
            say!(
                "halcyond: consfeed WRITE FAILED; discarded queued input (total {})",
                *dropped
            );
        }
    } else if loss == FeedLoss::OverBound {
        if !*logged {
            *logged = true;
            say!(
                "halcyond: consfeed backlog over {} bytes; dropping input (total {})",
                FEED_PENDING_MAX,
                *dropped
            );
        }
    } else if pending.is_empty() {
        *logged = false;
    }
}

fn write_ctl(fd: i64, s: &str) -> bool {
    if fd < 0 {
        return false;
    }
    unsafe { t_write(fd, s.as_ptr(), s.len()) == s.len() as i64 }
}

/// A source (item, row)'s visual extent within a laid block: the union of
/// its (possibly wrapped) lines as (y, h). None when the item laid no line
/// (evicted / empty).
fn laid_line_for(laid: &LaidBlock, item: usize, row: usize) -> Option<(i32, i32)> {
    let mut y0: Option<i32> = None;
    let mut y1 = 0;
    for l in laid.lines.iter() {
        if l.src_item == item && l.src_row == row {
            if y0.is_none() {
                y0 = Some(l.y);
            }
            y1 = l.y + l.h;
        }
    }
    y0.map(|y| (y, y1 - y))
}

/// Drain the console mirror, non-blocking, bounded: at most 8 reads per
/// call. Feeds the transcript and latches the exit mark.
fn drain_console(
    drain: i64,
    t: &mut Transcript,
    buf: &mut [u8],
    eof: &mut bool,
    pending_exit: &mut Option<i64>,
) {
    if *eof {
        return;
    }
    for _ in 0..8 {
        let mut pfd = [TPollFd {
            fd: drain as i32,
            events: T_POLLIN,
            revents: 0,
        }];
        let rc = unsafe { t_poll(pfd.as_mut_ptr(), 1, 0) };
        if rc <= 0 || (pfd[0].revents & T_POLLIN) == 0 {
            break;
        }
        let n = unsafe { t_read(drain, buf.as_mut_ptr(), buf.len()) };
        if n > 0 {
            t.feed(&buf[..n as usize]);
            if let Some(code) = t.take_exit() {
                *pending_exit = Some(code);
            }
        } else if n == 0 {
            *eof = true;
            break;
        } else {
            *eof = true;
            say!("halcyond: consdrain read error {}", n);
            break;
        }
    }
}

/// H-3c: the selected obj run's rect within a laid block, when the Normal
/// cursor row lives in block `bi` (usize::MAX = the open block) and a run is
/// selected there -- the ember underline's geometry.
fn run_mark(
    mode: Mode,
    sel: Option<&Sel>,
    flat: &[FlatRow],
    bi: usize,
    laid: &LaidBlock,
) -> Option<(i32, i32, i32, i32)> {
    if mode != Mode::Normal {
        return None;
    }
    let s = sel?;
    let obj = s.obj?;
    let fr = flat.get(s.cursor)?;
    if fr.block != bi {
        return None;
    }
    run_rect(laid, fr.item, fr.row, obj)
}

/// H-3c: summon the verb menu for an obj at surface point (ax, ay) -- the
/// run's display rect rides the say line for the witnesses. Surface coords
/// become display coords through the console pane's content origin (the
/// pane's `geometry` file; the console is display-sized when no layout has
/// named its pane yet, so (0, 0) is then exact).
fn summon(
    troot: i64,
    own_pane: Option<u32>,
    menus: &mut menuset::MenuSet,
    model: Menu,
    ax: i32,
    ay: i32,
    run: (i32, i32, i32, i32),
    gs: &mut GlyphSource,
) {
    let (gx, gy) = own_pane
        .and_then(|id| chromeset::read_file(troot, &alloc::format!("pane/{}/geometry", id)))
        .and_then(|s| halcyond::chrome::parse_rect(&s))
        .map(|r| (r.0 as i32, r.1 as i32))
        .unwrap_or((0, 0));
    let d = |v: i32, o: i32| (v + o).max(0) as u32;
    let run_d = (
        d(run.0, gx),
        d(run.1, gy),
        run.2.max(0) as u32,
        run.3.max(0) as u32,
    );
    menus.open(model, d(ax, gx), d(ay, gy), run_d, gs);
}

struct CacheEnt {
    width: i32,
    sheet_gen: u32,
    atlas_gen: u32,
    laid: LaidBlock,
}

/// Frozen-block layout, cached by block id (stable identity; the open
/// block + pending line never cache -- they change every feed).
struct LayoutCache {
    map: BTreeMap<u64, CacheEnt>,
}

impl LayoutCache {
    fn new() -> LayoutCache {
        LayoutCache {
            map: BTreeMap::new(),
        }
    }

    fn get(
        &mut self,
        b: &halcyond::transcript::Block,
        width: i32,
        sheet: &Sheet,
        gs: &mut GlyphSource,
    ) -> &LaidBlock {
        let gen = gs.gen();
        let hit = matches!(self.map.get(&b.id),
            Some(e) if e.width == width && e.sheet_gen == sheet.gen && e.atlas_gen == gen);
        if !hit {
            if self.map.len() > 512 {
                // Crude LRU stand-in: reset and re-lay the visible set.
                self.map.clear();
            }
            let laid = layout_block(b, width, sheet, gs);
            self.map.insert(
                b.id,
                CacheEnt {
                    width,
                    sheet_gen: sheet.gen,
                    atlas_gen: gen,
                    laid,
                },
            );
        }
        &self.map.get(&b.id).unwrap().laid
    }

    fn evict_missing(&mut self, live: &dyn Fn(u64) -> bool) {
        self.map.retain(|id, _| live(*id));
    }
}

/// KT-1.5d-1a (HALCYON 14.12): the per-user SESSION compositor. login spawns
/// this AS the user (`/bin/halcyond --session`) with the user identity and the
/// mounted `/home/<user>` -- ZERO identity delegation (I-22 clean): the only
/// identity-stamp is login's, and this process holds no `CAP_SET_IDENTITY` and
/// no `SPAWN_PERM_CONSOLE_RENDERER`. It connects to the SYSTEM tapestryd
/// (`/srv/tapestry`) as an ordinary-user `Session` actor -- connecting is
/// ungated -- and presents a fullscreen surface.
///
/// d-1a presents a BLANK Daylight ground (the bootstrap: prove login -> a
/// per-user compositor connects + presents). It reuses the connect/surface/
/// present primitives but NOT the console render brain: no fonts, no atlas, no
/// transcript, no chrome -- those wire in at KT-1.5d-2 (the first kaua-term tile,
/// the ii-a `Tile` model). The aurora relinquish -> `Direct(halcyond)` handoff
/// is d-1b; d-1a is content to compose alongside aurora.
fn session_main() -> i64 {
    // Connect to tapestryd + take a fullscreen surface, with the console
    // path's bounded connect retry (tapestryd may still be coming up). SQPOLL
    // is harmless with no tile pipes yet and is what KT-1.5d-2's unified poll
    // needs, so the session ring is SQPOLL from the start.
    let mut ring: Option<EventRing> = None;
    let mut surf: Option<Surface> = None;
    for i in 0..CONNECT_TRIES {
        let r = match EventRing::connect_sqpoll() {
            Ok(r) => r,
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("halcyond: FAIL session connect {:?}", e);
                    return 1;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
                continue;
            }
        };
        match Surface::fullscreen_on(&r) {
            Ok(s) => {
                surf = Some(s);
                ring = Some(r);
                break;
            }
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("halcyond: FAIL session connect/create {:?}", e);
                    return 1;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
            }
        }
    }
    let ring = ring.unwrap();
    let mut surf = surf.unwrap();

    // The render brain (HALCYON.md 14.12: the per-user compositor REUSES the
    // console render brain) -- the mono glyph source + the Daylight sheet. The
    // grid is a terminal (FACE_MONO); the scrollback flows through the same
    // proportional layout the console transcript uses.
    let mut gs = GlyphSource::new_vendored(512);
    if gs.face_count() != 2 {
        say!("halcyond: FAIL vendored face parse");
        return 1;
    }
    let sheet = daylight_sheet();
    let (cell_w, cell_h, _) = gs.mono_cell();
    let cols_of = |w: usize| -> u16 { ((w as i32 / cell_w).max(1)) as u16 };
    let rows_of = |h: usize| -> u16 { ((h as i32 / cell_h).max(1)) as u16 };

    // Spawn ONE kaua-term hosting ut, AS OURSELVES -- the identity login already
    // stamped (14.12: zero delegation, I-22 clean; plain spawn, no cap). fd0 =
    // the down pipe (we write Key/Resize), fd1 = the up pipe (we read the record
    // stream), fd2 inherits our log. The kaua-term mints the pts internally, so
    // ut never sees these pipes (the non-inheritance ptyhost relies on).
    let mut tcols = cols_of(surf.w as usize);
    let mut trows = rows_of(surf.h as usize);
    let mut child = match Command::new("/bin/kaua-term")
        .arg(alloc::format!("{}", tcols))
        .arg(alloc::format!("{}", trows))
        .arg("/bin/ut")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Inherit)
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            say!("halcyond: FAIL session tile spawn {:?}", e);
            return 1;
        }
    };
    let tile_pid = child.pid();
    let (down, up) = match (child.stdin.take(), child.stdout.take()) {
        (Some(d), Some(u)) => (d, u),
        _ => {
            say!("halcyond: FAIL session tile pipe ends missing");
            let _ = child.kill();
            let _ = child.wait();
            return 1;
        }
    };
    let down_fd = down.as_raw_fd() as i64;
    let up_fd = up.as_raw_fd() as i64;
    say!(
        "halcyond: session tile spawned pid={} {}x{} (/bin/ut)",
        tile_pid,
        tcols,
        trows
    );

    // The tile model (grid + scrollback, ii-a) + the ingest decoder (the trust
    // boundary: bounds-checked wire, 14.11.10/.12) + the reusable frame list.
    let mut tile = Tile::new(tcols as usize, trows as usize, daylight_palette());
    let mut dec = FrameDecoder::new();
    let mut inbuf = [0u8; 8192];
    let mut wire_out: Vec<u8> = Vec::new();
    let mut cart = cartoon::Cartoon::new();

    let mut announced = false;
    let mut ingest_announced = false;
    let mut dirty = true;
    let mut present_fails: u32 = 0;
    const PRESENT_FAILS_FATAL: u32 = 240;
    // The logout latch: Some(code) once the tile's child is gone (Control::Exit,
    // up-pipe EOF), the compositor is gone (CLOSE / stream-end), or the render
    // wedged. Draining to a clean teardown, so a code set mid-drain still reaps.
    let mut logout: Option<i32> = None;

    loop {
        // (1) Render at the TOP: the first present precedes any wait (the scanout
        // is first-present-wins and frame ticks reach only VISIBLE surfaces).
        // tile.render composes the live grid (mono tail) + the scrollback flow.
        if dirty && logout.is_none() {
            dirty = false;
            let (sw, sh) = (surf.w as usize, surf.h as usize);
            tile.render(&mut cart, sw, sh, &mut gs, &sheet, 0);
            {
                let px = surf.pixels();
                cartoon::execute(
                    &cart,
                    &gs.packer.store,
                    &cartoon::BlobStore::new(),
                    px,
                    sw,
                    None,
                );
            }
            match surf.present(None) {
                Ok(()) => {
                    present_fails = 0;
                    if !announced {
                        announced = true;
                        say!("halcyond: session up {}x{} px", sw, sh);
                    }
                }
                Err(_) => {
                    // A dropped frame, never death (#31); the next pass
                    // re-renders. A never-succeeds wedge is the backstop.
                    present_fails += 1;
                    dirty = true;
                    if present_fails >= PRESENT_FAILS_FATAL {
                        say!(
                            "halcyond: {} consecutive session present failures; exiting",
                            present_fails
                        );
                        logout = Some(1);
                    }
                }
            }
        }

        // (2) Drain queued surface events (poll_event pumps the SQPOLL ring's
        // CQEs): CLOSE -> logout; CONFIGURE -> resize the tile + send Resize
        // down; KEY -> route to the tile's down-channel (14.11.9; one tile, so
        // always focused -- d-3 adds focus routing).
        loop {
            match surf.poll_event() {
                Ok(Some(e)) => match e.kind {
                    TEV_CLOSE => {
                        say!("halcyond: session CLOSE received -- logout");
                        logout = Some(0);
                    }
                    TEV_CONFIGURE => match surf.handle_configure(&e) {
                        Ok(_) => {
                            let (nc, nr) = (cols_of(surf.w as usize), rows_of(surf.h as usize));
                            if nc != tcols || nr != trows {
                                tcols = nc;
                                trows = nr;
                                tile.resize(nc as usize, nr as usize);
                                wire_out.clear();
                                encode_input(&Input::Resize { cols: nc, rows: nr }, &mut wire_out);
                                write_all_fd(down_fd, &wire_out);
                            }
                            dirty = true;
                        }
                        Err(TapError::Busy) => {}
                        Err(e2) => {
                            say!("halcyond: session reweave failed {:?}; exiting", e2);
                            logout = Some(1);
                        }
                    },
                    TEV_KEY => {
                        if let Some(kev) = map_key(e.code, e.rune, e.value) {
                            wire_out.clear();
                            encode_input(&Input::Key(kev), &mut wire_out);
                            write_all_fd(down_fd, &wire_out);
                        }
                    }
                    _ => {}
                },
                Ok(None) => break,
                Err(_) => {
                    say!("halcyond: session event stream ended (compositor gone); exiting");
                    logout = Some(1);
                    break;
                }
            }
            if logout.is_some() {
                break;
            }
        }
        if logout.is_some() {
            break;
        }

        // If the drain produced a render need, re-render before blocking.
        if dirty {
            continue;
        }

        // (3) Block: poll { compositor ring | tile up-pipe }. The SQPOLL kthread
        // posts ring CQEs off-thread, so ring POLLIN means a surface event; up
        // POLLIN/HUP means the tile wrote records or closed.
        let mut waitfds = [
            TPollFd {
                fd: ring.poll_fd(),
                events: T_POLLIN,
                revents: 0,
            },
            TPollFd {
                fd: up_fd as i32,
                events: T_POLLIN,
                revents: 0,
            },
        ];
        if unsafe { t_poll(waitfds.as_mut_ptr(), 2, -1) } < 0 {
            say!("halcyond: session poll failed (compositor gone); exiting");
            logout = Some(1);
            break;
        }

        // (4) Ingest the tile's record stream (one read per wake; the decoder
        // buffers a partial frame for the next). A wire error / EOF / Exit tears
        // down ONLY that tile (14.11.10/.12); with one tile, that is the logout.
        if waitfds[1].revents & (T_POLLIN | T_POLLHUP) != 0 {
            let n = unsafe { t_read(up_fd, inbuf.as_mut_ptr(), inbuf.len()) };
            if n <= 0 {
                if tile.exited().is_none() {
                    say!("halcyond: session tile pipe EOF -- logout");
                }
                logout = Some(tile.exited().unwrap_or(0));
            } else {
                dec.push(&inbuf[..n as usize]);
                loop {
                    match dec.next_frame() {
                        Some(Ok((tag, payload))) => match parse_record(tag, &payload) {
                            Ok(rec) => {
                                tile.apply(rec);
                                dirty = true;
                                if !ingest_announced {
                                    ingest_announced = true;
                                    say!("halcyond: session tile ingest live");
                                }
                            }
                            Err(_) => {
                                say!("halcyond: session tile record wire error -- logout");
                                logout = Some(1);
                                break;
                            }
                        },
                        Some(Err(_)) => {
                            say!("halcyond: session tile frame wire error -- logout");
                            logout = Some(1);
                            break;
                        }
                        None => break,
                    }
                }
            }
        }

        // (5) The child exited (a Control::Exit landed above) -> logout.
        if logout.is_none() {
            if let Some(code) = tile.exited() {
                say!("halcyond: session tile exited (code {}) -- logout", code);
                logout = Some(code);
            }
        }
        if logout.is_some() {
            break;
        }
    }

    // Teardown: drop the down pipe (EOF -> the kaua-term's input pump exits ->
    // the kaua-term group tears down, taking ut with it), belt-and-suspenders
    // kill, then reap so the tile does not linger as a zombie. Then return the
    // code; login's wait() returns -> getty -> the next login -> aurora
    // un-backgrounds + resumes (14.12 step 4).
    let code = logout.unwrap_or(0);
    drop(down);
    drop(up);
    let _ = child.kill();
    let _ = child.wait();
    code as i64
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // KT-1.5d-1a (HALCYON 14.12): `--session` selects the per-user SESSION
    // compositor -- login spawns this AS the user, NOT joey as the system
    // console renderer. The session variant holds no g_console_renderer role:
    // it skips the /dev/cons drain/feed/consctl trio entirely and hosts the
    // session in tapestryd (later: pts tiles), never the console mirror. The
    // console-renderer body below is unchanged (the proven aurora-shaped path,
    // still selected by joey when the device names halcyond as the renderer).
    if libthyla_rs::env::args()
        .operands()
        .any(|a| a == b"--session")
    {
        return session_main();
    }
    if !cornucopia::verify_all() {
        say!("halcyond: FAIL atlas magic/version");
        return 1;
    }

    // The renderer role: drain/feed first (fail loudly without the grant --
    // leave the scanout to whoever else presents).
    let drain = open_path("/dev/consdrain", T_OREAD);
    if drain < 0 {
        say!("halcyond: FAIL open /dev/consdrain (not the bound renderer?)");
        return 1;
    }
    let feed = open_path("/dev/consfeed", T_OWRITE);
    if feed < 0 {
        say!("halcyond: FAIL open /dev/consfeed");
        return 1;
    }
    let consctl = open_path("/dev/consctl", T_OWRITE);
    if consctl < 0 {
        say!("halcyond: /dev/consctl open failed (winsize + tier reporting off)");
    }

    // THE RICH ADVERTISEMENT (BEACON.md 12.3): halcyond is the first rich
    // renderer; ut reads /dev/beacon and exports BEACON=rich, and the
    // emitters frame their output. Best-effort like winsize; the kernel
    // resets the tier when this drain closes.
    if write_ctl(consctl, "beacon rich") {
        say!("halcyond: beacon rich advertised");
    } else if consctl >= 0 {
        say!("halcyond: beacon tier advertise failed (clients see none)");
    }

    // THE EVENT SET (H-3c-2): ONE session + ONE Loom ring for every surface
    // this renderer opens -- the console, the tag-bar tiles, the menu -- so
    // one wait wakes for any of their events and one session's reader demuxes
    // all of them. (Two sessions under one thread starved whichever the
    // thread was not waiting on: a tile's CONFIGURE landed only at the next
    // pane-tree RPC, a menu's key never -- the H-3c lever.)
    //
    // KT-1.5b: the ring is SQPOLL, so the kernel poll-thread drives the
    // session reader and posts completions off-thread. The main loop then
    // blocks in ONE poll(2) over { ring.poll_fd() | /dev/consdrain } instead
    // of the ring alone, so shell output wakes the renderer at once rather
    // than at the next compositor frame tick (the frame-coupled console
    // latency). The console surface + the ring (bounded connect retry;
    // aurora keeps the non-SQPOLL connect()).
    let mut ring: Option<EventRing> = None;
    let mut surf: Option<Surface> = None;
    for i in 0..CONNECT_TRIES {
        let r = match EventRing::connect_sqpoll() {
            Ok(r) => r,
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("halcyond: FAIL connect {:?}", e);
                    return 1;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
                continue;
            }
        };
        match Surface::fullscreen_on(&r) {
            Ok(s) => {
                surf = Some(s);
                ring = Some(r);
                break;
            }
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("halcyond: FAIL connect/create {:?}", e);
                    return 1;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
            }
        }
    }
    let ring = ring.unwrap();
    let mut surf = surf.unwrap();
    let (mut w, mut h) = (surf.w as usize, surf.h as usize);

    // H-3b-3: the per-leaf chrome, on the same session (its root serves the
    // pane 9P tree reads) and ring; the chrome set reconciles after the
    // first successful present and on every structural relayout (the main
    // surface's CONFIGURE) or a tile's own CONFIGURE (a focus-only epoch),
    // and pumps its tiles' queues every pass.
    let troot = ring.root();
    let mut chrome = chromeset::ChromeSet::new(ring.clone());
    let mut relayout = true;
    // H-3b-4: the exit of the last completed command, taken from the
    // transcript's exit mark and owed to the compositor as the console
    // tile's status (the gated `tag <id> status` verb). Held until the
    // tile's pane is known and the console is up; only the LAST exit
    // matters, so a newer one replaces an unsent older one. A refusal is
    // said once and the exit is dropped; the NEXT exit mark tries again
    // (the H-3b round F4: a one-shot latch turned one transient refusal
    // into a session-long loss of the live key -- display-only, so a
    // cheap retry per command is the right posture).
    let mut pending_exit: Option<i64> = None;
    let mut status_refusal_said = false;
    // H-3c: the verb table (BEACON.md 7; the system tier, read once) + the
    // one menu, the last frame's block placement (the hit map for
    // click-a-path and the keyboard menu's anchor: block id, screen y,
    // height; u64::MAX = the open block), the open block's last layout, and
    // the pointer's last surface position (a BTN event carries none).
    let rules: Vec<Rule> = match chromeset::read_file(T_WALK_OPEN_FROM_ROOT, "/lib/beacon/verbs") {
        Some(text) => parse_verbs(&text, cfg!(feature = "test-mode")),
        None => Vec::new(),
    };
    say!("halcyond: {} verb rules loaded", rules.len());
    let mut menus = menuset::MenuSet::new(ring.clone());
    // H-3d: the status bar -- one Role::Status surface on the same ring,
    // minted once the console is up (step 0d).
    let mut status = statusset::StatusBar::new(ring.clone());
    let mut frame: Vec<(u64, i32, i32)> = Vec::new();
    let mut last_open_laid: Option<LaidBlock> = None;
    let mut ptr: (i32, i32) = (0, 0);

    let mut gs = GlyphSource::new_vendored(512);
    if gs.face_count() != 2 {
        say!("halcyond: FAIL vendored face parse");
        return 1;
    }
    let sheet = daylight_sheet();
    let mut t = Transcript::new(libhalcyon::theme::daylight_palette());
    let mut cache = LayoutCache::new();

    // The winsize report: the transcript is flowed, but programs wrap to a
    // COLUMN count -- report the mono-grid equivalent (foreign/plain
    // content is mono, so this is the terminal-compatible answer).
    let (cell_w, cell_h, _) = gs.mono_cell();
    let report_winsize = |ctl: i64, w: usize, h: usize| {
        let cols = (w as i32 / cell_w).max(1);
        let rows = (h as i32 / cell_h).max(1);
        let cmd = alloc::format!("winsize {} {}", cols, rows);
        let _ = write_ctl(ctl, &cmd);
    };
    report_winsize(consctl, w, h);

    let mut mode = Mode::Insert;
    let mut scroll_up: i32 = 0; // px above the bottom anchor (0 = anchored)
    let mut last_seq: u64 = u64::MAX;
    let mut dirty = true;
    // Helix-modal selection (v0, row-wise): the flat row list + the
    // selection state live only while in Normal mode; new output while
    // selecting re-flattens + clamps (the transcript keeps moving).
    let mut flat: Vec<halcyond::select::FlatRow> = Vec::new();
    let mut flat_seq: u64 = u64::MAX;
    let mut sel: Option<halcyond::select::Sel> = None;
    let mut yank_buf: Vec<u8> = Vec::new();
    let mut feed_pending: Vec<u8> = Vec::new();
    let mut feed_dropped: u64 = 0;
    let mut feed_logged = false;
    let mut keybuf: Vec<u8> = Vec::new();
    let mut drainbuf = [0u8; 2048];
    let mut drain_eof = false;
    let mut present_fails: u32 = 0;
    const PRESENT_FAILS_FATAL: u32 = 240;

    let mut announced = false;

    loop {
        // (0) The render pass runs at the TOP: pass 1 paints + presents the
        // first frame BEFORE any wait -- the scanout is first-present-wins
        // and frame ticks reach only VISIBLE surfaces, so a renderer that
        // waits before presenting stays dark and event-starved forever
        // (caught designing the first E2E: aurora paints frame 0 before its
        // loop for exactly this reason).
        // (0b) Retry held input unconditionally (#129/#135).
        feed_drain(feed, &mut feed_pending, &mut feed_dropped, &mut feed_logged);

        // (0c) Render when the transcript moved or the view is dirty
        // (pass 1 always: dirty starts true -- the first present).
        if t.seq != last_seq || dirty {
            last_seq = t.seq;
            dirty = false;
            // Evict layouts for blocks the budget dropped.
            {
                let frozen = t.frozen_blocks();
                let mut live: alloc::collections::BTreeSet<u64> =
                    alloc::collections::BTreeSet::new();
                for b in frozen.iter() {
                    live.insert(b.id);
                }
                cache.evict_missing(&|id| live.contains(&id));
            }
            let widthi = w as i32;
            // Lay everything; measure heights + each block's y RELATIVE to
            // the content top (the emit pass and the selection follow both
            // key off it).
            let mut heights: Vec<(u64, i32, i32)> = Vec::new(); // (id, h, rel_y)
            let mut total: i32 = sheet.block_gap;
            for b in t.frozen_blocks().iter() {
                let lh = cache.get(b, widthi, &sheet, &mut gs).height;
                heights.push((b.id, lh, total));
                total += lh + sheet.block_gap;
            }
            let open_rel = total;
            let open_laid = layout_block(t.open_block(), widthi, &sheet, &mut gs);
            let pending_laid = layout_pending(
                t.pending_line(),
                &t.open_block().styles,
                widthi,
                &sheet,
                &mut gs,
            );
            let open_h = open_laid.height + pending_laid.height;
            total += open_h;

            let viewh = h as i32;
            let max_up = (total - viewh).max(0);
            // The selection cursor drags the view (Helix: the view follows
            // the cursor, not the wheel): locate the cursor row's rel_y and
            // adjust scroll_up so it is visible BEFORE anchoring.
            if mode == Mode::Normal {
                if let Some(s) = sel.as_ref() {
                    if let Some(fr) = flat.get(s.cursor).copied() {
                        let (rel, lh) = if fr.block == usize::MAX {
                            match laid_line_for(&open_laid, fr.item, fr.row) {
                                Some((ly, lh)) => (open_rel + ly, lh),
                                None => (open_rel, open_laid.height.max(1)),
                            }
                        } else if let (Some(b), Some(&(_, bh, rel))) =
                            (t.frozen_blocks().get(fr.block), heights.get(fr.block))
                        {
                            let laid = cache.get(b, widthi, &sheet, &mut gs);
                            match laid_line_for(laid, fr.item, fr.row) {
                                Some((ly, lh)) => (rel + ly, lh),
                                None => (rel, bh.max(1)),
                            }
                        } else {
                            (0, 1)
                        };
                        // The row's distance from the content BOTTOM decides
                        // scroll_up directly: visible iff
                        //   scroll_up <= (total - rel - lh) <= scroll_up + viewh - lh
                        let from_bottom = total - rel - lh;
                        if from_bottom < scroll_up {
                            scroll_up = from_bottom;
                        } else if from_bottom > scroll_up + viewh - lh {
                            scroll_up = from_bottom - (viewh - lh).max(0);
                        }
                    }
                }
            }
            if scroll_up > max_up {
                scroll_up = max_up;
            }
            if scroll_up < 0 {
                scroll_up = 0;
            }
            // Bottom-anchored: the content's bottom sits at the view bottom
            // (raised by scroll_up).
            let y0 = if total <= viewh {
                0
            } else {
                viewh - total + scroll_up
            };

            // The selection band set, grouped for the emit pass.
            let sel_rows: Vec<halcyond::select::FlatRow> = match (mode, sel.as_ref()) {
                (Mode::Normal, Some(s)) => {
                    let (lo, hi) = s.range();
                    flat.iter().skip(lo).take(hi - lo + 1).copied().collect()
                }
                _ => Vec::new(),
            };

            let mut cart = cartoon::Cartoon::new();
            cart.ops.push(cartoon::Op::Clear {
                color: sheet.ground,
            });
            let mut y = y0 + sheet.block_gap;
            frame.clear();
            for (bi, b) in t.frozen_blocks().iter().enumerate() {
                let lh = heights
                    .iter()
                    .find(|(id, _, _)| *id == b.id)
                    .map(|(_, h, _)| *h)
                    .unwrap_or(0);
                frame.push((b.id, y, lh));
                if y + lh >= 0 && y <= viewh {
                    // Bands under the block's selected rows, then the text.
                    for fr in sel_rows.iter().filter(|fr| fr.block == bi) {
                        let laid = cache.get(b, widthi, &sheet, &mut gs);
                        if let Some(line) = laid_line_for(laid, fr.item, fr.row) {
                            cart.ops.push(cartoon::Op::Rect {
                                x: 0,
                                y: y + line.0,
                                w: w as u32,
                                h: line.1 as u32,
                                color: sheet.sel_bg,
                            });
                        }
                    }
                    let laid = cache.get(b, widthi, &sheet, &mut gs);
                    render_block(&mut cart, laid, y, &gs);
                    if let Some(r) = run_mark(mode, sel.as_ref(), &flat, bi, laid) {
                        cart.ops.push(cartoon::Op::Rect {
                            x: r.0,
                            y: y + r.1 + r.3 - 2,
                            w: r.2.max(1) as u32,
                            h: 2,
                            color: libhalcyon::theme::DAYLIGHT.ember,
                        });
                    }
                }
                y += lh + sheet.block_gap;
            }
            frame.push((u64::MAX, y, open_h));
            if y + open_h >= 0 && y <= viewh {
                for fr in sel_rows.iter().filter(|fr| fr.block == usize::MAX) {
                    if let Some(line) = laid_line_for(&open_laid, fr.item, fr.row) {
                        cart.ops.push(cartoon::Op::Rect {
                            x: 0,
                            y: y + line.0,
                            w: w as u32,
                            h: line.1 as u32,
                            color: sheet.sel_bg,
                        });
                    }
                }
                render_block(&mut cart, &open_laid, y, &gs);
                if let Some(r) = run_mark(mode, sel.as_ref(), &flat, usize::MAX, &open_laid) {
                    cart.ops.push(cartoon::Op::Rect {
                        x: r.0,
                        y: y + r.1 + r.3 - 2,
                        w: r.2.max(1) as u32,
                        h: 2,
                        color: libhalcyon::theme::DAYLIGHT.ember,
                    });
                }
            }
            let py = y + open_laid.height;
            last_open_laid = Some(open_laid);
            render_block(&mut cart, &pending_laid, py, &gs);
            // The cursor: a beam at the pending column (Insert ink; Normal
            // renders it hollow-dim -- the mode is visible at a glance).
            let (cx, cy, ch2) = cursor_pos(&pending_laid, t.pending_col(), &sheet);
            let ccol = if mode == Mode::Insert {
                sheet.ink
            } else {
                sheet.dim
            };
            cart.ops.push(cartoon::Op::Rect {
                x: cx,
                y: py + cy,
                w: 2,
                h: ch2.max(4) as u32,
                color: ccol,
            });

            let px = surf.pixels();
            cartoon::execute(
                &cart,
                &gs.packer.store,
                &cartoon::BlobStore::new(),
                px,
                w,
                None,
            );
            match surf.present(None) {
                Ok(()) => {
                    present_fails = 0;
                    if !announced {
                        announced = true;
                        say!(
                            "halcyond: console up {}x{} px (rich transcript; mono grid {}x{})",
                            w,
                            h,
                            (w as i32 / cell_w).max(1),
                            (h as i32 / cell_h).max(1)
                        );
                    }
                }
                Err(_) => {
                    // A dropped frame, never death (#31); the next pass
                    // re-renders. A live-stream-but-never-presents wedge
                    // is the backstop below.
                    present_fails += 1;
                    dirty = true;
                    if present_fails >= PRESENT_FAILS_FATAL {
                        say!(
                            "halcyond: {} consecutive present failures; exiting",
                            present_fails
                        );
                        return 1;
                    }
                }
            }
        }

        // (0d) H-3b-3: the chrome. Only once the console is up (first-
        // present-wins scanout: chrome never precedes it), then on every
        // relayout; the pump is per pass (FRAME never queues, CONFIGURE
        // coalesces, so this is cheap when idle).
        if announced {
            if chrome.pump() {
                relayout = true;
            }
            if relayout {
                relayout = false;
                chrome.reconcile(troot, surf.id, &mut gs);
                // A relayout re-arms the status bar's mint retry (H-3d F5):
                // a prior mint failure may now succeed, ChromeSet's cadence.
                status.rearm();
            }
            // The status feed: tell the compositor the console tile's last
            // exit (it draws the live hairline + shadow from it; the strip
            // re-reads it on the reconcile below). Rides the console
            // surface's own conn -- the gate reads the CONN's peer, and
            // this process holds the renderer role. Display-only: a refusal
            // drops this exit (said once) and the next exit mark retries.
            if let (Some(code), Some(pane)) = (pending_exit, chrome.own_pane()) {
                let st = if code == 0 { "ok" } else { "err" };
                pending_exit = None;
                match surf.global_ctl(&alloc::format!("tag {} status {}", pane, st)) {
                    Ok(()) => chrome.reconcile(troot, surf.id, &mut gs),
                    Err(e) => {
                        if !status_refusal_said {
                            status_refusal_said = true;
                            say!("halcyond: tag status refused {:?}; the live-tile key lags until the next exit", e);
                        }
                    }
                }
            }
            // (0d') H-3d: the status bar. Minted once the console is up
            // (never before it: first-present-wins), pumped per pass; its
            // model is re-derived per pass from the sources -- the focused
            // leaf's name + status (the last reconcile), the console's
            // directory + running-or-last command (the transcript), the
            // minute -- and painted only on a change.
            status.ensure();
            status.pump();
            let sm = statusset::model_from(
                chrome.focused(),
                chrome.own_pane(),
                t.cwd(),
                t.last_command(),
            );
            status.refresh(&sm, &mut gs);
        }

        // (0e) H-3c: the menu. A choice closes it from this side and types
        // the expanded command into the console (the tag line's "executes
        // typed text" -- the gesture is the choice); the compositor's own
        // dismiss (Esc / click-away / a chord) arrives as a closed stream.
        // While a menu is up this WAITS on the menu's ring (see
        // `MenuSet::service`: its session is read only by a waiter on it;
        // the menu's FRAME ticks bound the wait) and step (1) polls the
        // console's stream instead of blocking on it.
        match menus.service(&mut gs) {
            menuset::MenuEvent::Chosen(Action::Command(cmd)) => {
                menus.close();
                say!("halcyond: menu ran: {}", cmd);
                mode = Mode::Insert;
                sel = None;
                scroll_up = 0;
                dirty = true;
                // ^E ^U first (SA-8): ut's line editor takes them as
                // CursorEnd + KillToStart, so a half-typed draft moves to the
                // kill buffer (^Y restores it) instead of being run INTO --
                // `echo fo` + the verb typed `echo fols -l ...`. A canonical
                // reader sees VKILL; a raw-mode program sees two keys, as it
                // would from the keyboard.
                feed_pending.extend_from_slice(b"\x05\x15");
                feed_pending.extend_from_slice(cmd.as_bytes());
                feed_pending.push(b'\n');
                feed_drain(feed, &mut feed_pending, &mut feed_dropped, &mut feed_logged);
            }
            menuset::MenuEvent::Chosen(Action::Internal(act)) => {
                // THE GATE's lever (test builds only, the #880 strip class):
                // freeze this renderer with the menu still up -- a wedged
                // owner holding a modal. The compositor must dismiss it and
                // restore input routing on its own; when this loop wakes, the
                // menu's stream is dead and the console's queue holds what
                // was typed meanwhile.
                #[cfg(feature = "test-mode")]
                {
                    match act
                        .strip_prefix("#wedge ")
                        .and_then(|v| v.trim().parse::<u64>().ok())
                    {
                        Some(ms) => {
                            say!("halcyond: wedge-test: frozen {} ms with the menu up", ms);
                            let _ = sleep(Duration::from_millis(ms));
                            say!("halcyond: wedge-test: woke");
                        }
                        None => say!("halcyond: unknown internal action {}", act),
                    }
                }
                #[cfg(not(feature = "test-mode"))]
                say!(
                    "halcyond: internal action {} ignored (production build)",
                    act
                );
            }
            menuset::MenuEvent::Closed => {
                dirty = true;
            }
            menuset::MenuEvent::None => {}
        }

        // (1) The next event. Bounded wait while input is held (the
        // held-feed discipline's pace); otherwise the console's queue, and
        // when it is empty a wait on the RING -- any surface's event wakes
        // it (a tile's CONFIGURE, a menu key), after which this pass may
        // find the console's queue still empty and simply run the pumps.
        let mut ev = if wait_is_bounded(feed_pending.len()) {
            match surf.poll_event() {
                Ok(next) => {
                    if next.is_none() {
                        let _ = sleep(Duration::from_millis(FEED_RETRY_MS));
                    }
                    next
                }
                Err(_) => {
                    say!("halcyond: event stream ended (compositor gone); exiting");
                    return 1;
                }
            }
        } else {
            match surf.poll_event() {
                Ok(Some(e)) => Some(e),
                Ok(None) => {
                    // KT-1.5b-i: block in ONE poll over the SQPOLL ring AND
                    // the console mirror, so shell output wakes us at once
                    // instead of at the next compositor frame tick (the
                    // frame-coupled console latency). The ring's kthread
                    // drives the session reader and posts CQEs off-thread, so
                    // poll(ring_fd) reports POLLIN for any surface's event
                    // (console / tile / menu) exactly as the old ring.wait()
                    // woke on any completion; timeout -1 blocks indefinitely,
                    // matching it. A console-only wake leaves this surface's
                    // queue empty (take_event -> None); step (2) drains the
                    // console and the top re-renders.
                    let mut waitfds = [
                        TPollFd {
                            fd: ring.poll_fd(),
                            events: T_POLLIN,
                            revents: 0,
                        },
                        TPollFd {
                            fd: drain as i32,
                            events: T_POLLIN,
                            revents: 0,
                        },
                    ];
                    if unsafe { t_poll(waitfds.as_mut_ptr(), 2, -1) } < 0 {
                        say!("halcyond: unified poll failed (compositor gone); exiting");
                        return 1;
                    }
                    match surf.poll_event() {
                        Ok(next) => next,
                        Err(_) => {
                            say!("halcyond: event stream ended (compositor gone); exiting");
                            return 1;
                        }
                    }
                }
                Err(_) => {
                    say!("halcyond: event stream ended (compositor gone); exiting");
                    return 1;
                }
            }
        };
        while let Some(e) = ev {
            match e.kind {
                TEV_KEY => {
                    if e.value >= 1 {
                        if mode == Mode::Normal {
                            // Keep the flat list current before acting: new
                            // output during Normal mode moves the rows.
                            if flat_seq != t.seq {
                                flat_seq = t.seq;
                                flat = halcyond::select::flatten(&t);
                                if let Some(s) = sel.as_mut() {
                                    s.clamp(flat.len());
                                }
                            }
                            let page_rows = ((h as i32 / cell_h) / 2).max(1);
                            let act = normal_key(e.code, e.rune);
                            match act {
                                NormalAct::ScrollLines(n) => {
                                    // The cursor moves; the view follows at
                                    // render (Helix, not a scroll wheel).
                                    if let Some(s) = sel.as_mut() {
                                        s.mv(-n, flat.len());
                                    }
                                    dirty = true;
                                }
                                NormalAct::ScrollHalfPage(n) => {
                                    if let Some(s) = sel.as_mut() {
                                        s.mv(-n * page_rows, flat.len());
                                    }
                                    dirty = true;
                                }
                                NormalAct::Top => {
                                    if let Some(s) = sel.as_mut() {
                                        s.cursor = 0;
                                    }
                                    dirty = true;
                                }
                                NormalAct::Bottom => {
                                    if let Some(s) = sel.as_mut() {
                                        s.cursor = flat.len().saturating_sub(1);
                                    }
                                    dirty = true;
                                }
                                NormalAct::ToggleSelect if e.value == 1 => {
                                    if let Some(s) = sel.as_mut() {
                                        s.toggle_anchor();
                                    }
                                    dirty = true;
                                }
                                NormalAct::Yank => {
                                    if let Some(s) = sel.as_ref() {
                                        let text = s.yank(&t, &flat);
                                        yank_buf.clear();
                                        yank_buf.extend_from_slice(text.as_bytes());
                                        say!("halcyond: yanked {} bytes", yank_buf.len());
                                    }
                                    if let Some(s) = sel.as_mut() {
                                        s.anchor = None;
                                    }
                                    dirty = true;
                                }
                                NormalAct::Paste if e.value == 1 => {
                                    // Paste = type the register into the
                                    // prompt: back to Insert, re-anchored.
                                    mode = Mode::Insert;
                                    sel = None;
                                    scroll_up = 0;
                                    if !yank_buf.is_empty() {
                                        feed_pending.extend_from_slice(&yank_buf);
                                        feed_drain(
                                            feed,
                                            &mut feed_pending,
                                            &mut feed_dropped,
                                            &mut feed_logged,
                                        );
                                    }
                                    dirty = true;
                                }
                                NormalAct::Collapse => {
                                    if let Some(s) = sel.as_mut() {
                                        s.anchor = None;
                                    }
                                    dirty = true;
                                }
                                NormalAct::ToInsert => {
                                    mode = Mode::Insert;
                                    sel = None;
                                    scroll_up = 0;
                                    dirty = true;
                                }
                                NormalAct::NextRun | NormalAct::PrevRun => {
                                    // H-3c: step the obj-run selection
                                    // (across rows); the view follows.
                                    let fwd = act == NormalAct::NextRun;
                                    if let Some(s) = sel.as_mut() {
                                        if let Some((row, obj)) =
                                            step_run(&t, &flat, s.cursor, s.obj, fwd)
                                        {
                                            s.cursor = row;
                                            s.anchor = None;
                                            s.obj = Some(obj);
                                        }
                                    }
                                    dirty = true;
                                    // Test builds: say the selected run's
                                    // CURRENT display rect + the mono row
                                    // height -- the lever's click leg aims
                                    // at it (this line itself scrolls the
                                    // transcript by one such row; the
                                    // witness subtracts it).
                                    #[cfg(feature = "test-mode")]
                                    {
                                        let at = sel.as_ref().and_then(|s| {
                                            flat.get(s.cursor).map(|fr| (*fr, s.obj))
                                        });
                                        if let Some((fr, Some(obj))) = at {
                                            let key = if fr.block == usize::MAX {
                                                Some(u64::MAX)
                                            } else {
                                                t.frozen_blocks().get(fr.block).map(|b| b.id)
                                            };
                                            let by = key.and_then(|k| {
                                                frame.iter().find(|f| f.0 == k).map(|f| f.1)
                                            });
                                            let rect = if fr.block == usize::MAX {
                                                last_open_laid
                                                    .as_ref()
                                                    .and_then(|l| run_rect(l, fr.item, fr.row, obj))
                                            } else {
                                                t.frozen_blocks().get(fr.block).and_then(|b| {
                                                    let laid =
                                                        cache.get(b, w as i32, &sheet, &mut gs);
                                                    run_rect(laid, fr.item, fr.row, obj)
                                                })
                                            };
                                            if let (Some(by), Some((rx, ry, rw, rh))) = (by, rect) {
                                                let (gx, gy) = chrome
                                                    .own_pane()
                                                    .and_then(|id| {
                                                        chromeset::read_file(
                                                            troot,
                                                            &alloc::format!("pane/{}/geometry", id),
                                                        )
                                                    })
                                                    .and_then(|s| halcyond::chrome::parse_rect(&s))
                                                    .map(|r| (r.0 as i32, r.1 as i32))
                                                    .unwrap_or((0, 0));
                                                say!(
                                                    "halcyond: run at {} {} {} {} rowh {}",
                                                    gx + rx,
                                                    gy + by + ry,
                                                    rw,
                                                    rh,
                                                    cell_h
                                                );
                                            }
                                        }
                                    }
                                }
                                NormalAct::Act if e.value == 1 => {
                                    // H-3c: the verb menu for the selected
                                    // run (the row's first when none is),
                                    // anchored under the run as the last
                                    // frame laid it.
                                    let at = sel
                                        .as_ref()
                                        .and_then(|s| flat.get(s.cursor).map(|fr| (*fr, s.obj)));
                                    if let Some((fr, cur)) = at {
                                        let obj = cur
                                            .or_else(|| runs_on_row(&t, fr).first().map(|r| r.obj));
                                        // Nothing to act on is said in test
                                        // builds: a silent no-op is not a
                                        // diagnosable outcome on a lever run.
                                        #[cfg(feature = "test-mode")]
                                        if obj.is_none() {
                                            say!("halcyond: act: no obj run on row {}/{} (block {} item {})",
                                                 sel.as_ref().map_or(0, |s| s.cursor), flat.len(), fr.block, fr.item);
                                        }
                                        if let Some(obj) = obj {
                                            if let Some(s) = sel.as_mut() {
                                                s.obj = Some(obj);
                                            }
                                            dirty = true;
                                            let key = if fr.block == usize::MAX {
                                                Some(u64::MAX)
                                            } else {
                                                t.frozen_blocks().get(fr.block).map(|b| b.id)
                                            };
                                            let by = key.and_then(|k| {
                                                frame.iter().find(|f| f.0 == k).map(|f| f.1)
                                            });
                                            let rect = if fr.block == usize::MAX {
                                                last_open_laid
                                                    .as_ref()
                                                    .and_then(|l| run_rect(l, fr.item, fr.row, obj))
                                            } else {
                                                match t.frozen_blocks().get(fr.block) {
                                                    Some(b) => {
                                                        let laid =
                                                            cache.get(b, w as i32, &sheet, &mut gs);
                                                        run_rect(laid, fr.item, fr.row, obj)
                                                    }
                                                    None => None,
                                                }
                                            };
                                            #[cfg(feature = "test-mode")]
                                            if by.is_none() || rect.is_none() {
                                                say!("halcyond: act: obj {} unplaced (frame-y {:?} rect {:?})", obj, by, rect);
                                            }
                                            if let (Some(by), Some((rx, ry, rw, rh))) = (by, rect) {
                                                if let Some((ty, refv)) = obj_of(&t, fr.block, obj)
                                                {
                                                    let model = build_menu(&rules, ty, refv);
                                                    summon(
                                                        troot,
                                                        chrome.own_pane(),
                                                        &mut menus,
                                                        model,
                                                        rx,
                                                        by + ry + rh,
                                                        (rx, by + ry, rw, rh),
                                                        &mut gs,
                                                    );
                                                }
                                            }
                                        }
                                    }
                                }
                                // An autorepeat of a one-shot is not another
                                // press: the press acted (a held Enter would
                                // re-summon the menu at the repeat rate -- the
                                // H-3c-2 round F6); movement keys repeat.
                                NormalAct::None
                                | NormalAct::Act
                                | NormalAct::Paste
                                | NormalAct::ToggleSelect => {}
                            }
                        } else if e.rune == 0x1b {
                            // Esc enters Normal (the Helix-modal boundary;
                            // full-screen ESC consumers live in raw-VT
                            // panes, H-3). The cursor starts on the newest
                            // row -- of everything printed so far: the
                            // console mirror is drained FIRST, so output the
                            // shell has already written (and the user has
                            // seen on any other path) is in the rows the
                            // cursor lands on. Keys are serviced before the
                            // drain within a pass; without this, an Esc sent
                            // right after a command's output froze the view
                            // one command behind (the H-3c lever caught it).
                            drain_console(
                                drain,
                                &mut t,
                                &mut drainbuf,
                                &mut drain_eof,
                                &mut pending_exit,
                            );
                            mode = Mode::Normal;
                            flat_seq = t.seq;
                            flat = halcyond::select::flatten(&t);
                            sel = Some(halcyond::select::Sel::at_end(flat.len()));
                            dirty = true;
                        } else {
                            keybuf.clear();
                            key_bytes(e.code, e.value, e.rune, &mut keybuf);
                            if !keybuf.is_empty() {
                                feed_pending.extend_from_slice(&keybuf);
                                feed_drain(
                                    feed,
                                    &mut feed_pending,
                                    &mut feed_dropped,
                                    &mut feed_logged,
                                );
                            }
                        }
                    }
                }
                TEV_PTR_MOVE => {
                    ptr = (
                        (e.value >> 16) as u16 as i32,
                        (e.value & 0xffff) as u16 as i32,
                    );
                }
                TEV_PTR_BTN => {
                    // H-3c click-a-path (HALCYON.md 5/6): a left press on an
                    // obj run's glyphs -- as the last frame laid them --
                    // opens its verb menu at the pointer.
                    if e.code == BTN_LEFT && e.value == 1 {
                        let hit = frame
                            .iter()
                            .find(|f| ptr.1 >= f.1 && ptr.1 < f.1 + f.2)
                            .copied();
                        if let Some((id, by, _)) = hit {
                            let block = if id == u64::MAX {
                                Some(usize::MAX)
                            } else {
                                t.frozen_blocks().iter().position(|b| b.id == id)
                            };
                            let found = match block {
                                Some(usize::MAX) => last_open_laid.as_ref().and_then(|l| {
                                    hit_run(l, ptr.0, ptr.1 - by)
                                        .map(|(i, r, o)| (i, r, o, run_rect(l, i, r, o)))
                                }),
                                Some(bi) => match t.frozen_blocks().get(bi) {
                                    Some(b) => {
                                        let laid = cache.get(b, w as i32, &sheet, &mut gs);
                                        hit_run(laid, ptr.0, ptr.1 - by)
                                            .map(|(i, r, o)| (i, r, o, run_rect(laid, i, r, o)))
                                    }
                                    None => None,
                                },
                                None => None,
                            };
                            if let (Some(bi), Some((_, _, obj, Some((rx, ry, rw, rh))))) =
                                (block, found)
                            {
                                if let Some((ty, refv)) = obj_of(&t, bi, obj) {
                                    let model = build_menu(&rules, ty, refv);
                                    summon(
                                        troot,
                                        chrome.own_pane(),
                                        &mut menus,
                                        model,
                                        ptr.0,
                                        ptr.1,
                                        (rx, by + ry, rw, rh),
                                        &mut gs,
                                    );
                                }
                            }
                        }
                    }
                }
                TEV_CLOSE => {
                    say!("halcyond: CLOSE received; exiting");
                    return 0;
                }
                TEV_CONFIGURE => {
                    match surf.handle_configure(&e) {
                        Ok(false) => {
                            dirty = true; // same-size redraw request
                            relayout = true; // a structural relayout fanned it
                        }
                        Ok(true) => {
                            w = surf.w as usize;
                            h = surf.h as usize;
                            // Width changed: every cached layout is stale
                            // by key; drop them wholesale (the reflow
                            // E2E's moment).
                            cache.map.clear();
                            report_winsize(consctl, w, h);
                            dirty = true;
                            relayout = true;
                        }
                        Err(TapError::Busy) => {}
                        Err(e2) => {
                            say!("halcyond: reweave failed {:?}; exiting", e2);
                            return 1;
                        }
                    }
                }
                TEV_FOCUS => {
                    // H-3b-3: focus moved onto or off the console -- the
                    // tag bars re-key; re-read the layout next pass.
                    relayout = true;
                }
                _ => {}
            }
            ev = match surf.poll_event() {
                Ok(next) => next,
                Err(_) => {
                    say!("halcyond: event stream ended (compositor gone); exiting");
                    return 1;
                }
            };
        }

        // (2) The drain, non-blocking, bounded per pass.
        drain_console(
            drain,
            &mut t,
            &mut drainbuf,
            &mut drain_eof,
            &mut pending_exit,
        );
    }
}
