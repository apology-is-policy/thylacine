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

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::collections::BTreeMap;
use alloc::vec::Vec;

use halcyond::input::{
    feed_drain_with, key_bytes, normal_key, wait_is_bounded, FeedLoss, Mode, NormalAct,
    FEED_PENDING_MAX, FEED_RETRY_MS,
};
use halcyond::layout::{
    cursor_pos, layout_block, layout_pending, parchment_sheet, render_block, LaidBlock, Sheet,
};
use halcyond::raster::GlyphSource;
use halcyond::transcript::Transcript;
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{t_open, t_poll, t_read, t_write, TPollFd, T_OREAD, T_OWRITE, T_POLLIN,
    T_WALK_OPEN_FROM_ROOT};
use tapestry::{Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_KEY};

macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

const CONNECT_TRIES: u32 = 25;
const CONNECT_DELAY_MS: u64 = 200;

fn open_path(path: &str, omode: u32) -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), omode) }
}

fn feed_drain(fd: i64, pending: &mut Vec<u8>, dropped: &mut u64, logged: &mut bool) {
    let mut w = |b: &[u8]| unsafe { t_write(fd, b.as_ptr(), b.len()) };
    let loss = feed_drain_with(&mut w, pending, FEED_PENDING_MAX, dropped);
    if loss == FeedLoss::WriteErr {
        if !*logged {
            *logged = true;
            say!("halcyond: consfeed WRITE FAILED; discarded queued input (total {})", *dropped);
        }
    } else if loss == FeedLoss::OverBound {
        if !*logged {
            *logged = true;
            say!("halcyond: consfeed backlog over {} bytes; dropping input (total {})",
                 FEED_PENDING_MAX, *dropped);
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
        LayoutCache { map: BTreeMap::new() }
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
            self.map.insert(b.id, CacheEnt { width, sheet_gen: sheet.gen, atlas_gen: gen, laid });
        }
        &self.map.get(&b.id).unwrap().laid
    }

    fn evict_missing(&mut self, live: &dyn Fn(u64) -> bool) {
        self.map.retain(|id, _| live(*id));
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
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

    // The surface (bounded connect retry; aurora's discipline).
    let mut surf: Option<Surface> = None;
    for i in 0..CONNECT_TRIES {
        match Surface::fullscreen() {
            Ok(s) => {
                surf = Some(s);
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
    let mut surf = surf.unwrap();
    let (mut w, mut h) = (surf.w as usize, surf.h as usize);

    let mut gs = GlyphSource::new_vendored(512);
    if gs.face_count() != 2 {
        say!("halcyond: FAIL vendored face parse");
        return 1;
    }
    let sheet = parchment_sheet();
    let mut t = Transcript::new(vt::THEMES[1].1);
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
    let mut feed_pending: Vec<u8> = Vec::new();
    let mut feed_dropped: u64 = 0;
    let mut feed_logged = false;
    let mut keybuf: Vec<u8> = Vec::new();
    let mut drainbuf = [0u8; 2048];
    let mut drain_eof = false;
    let mut present_fails: u32 = 0;
    const PRESENT_FAILS_FATAL: u32 = 240;

    say!("halcyond: console up {}x{} px (rich transcript; mono grid {}x{})",
         w, h, (w as i32 / cell_w).max(1), (h as i32 / cell_h).max(1));

    loop {
        // (0) Retry held input unconditionally (#129/#135).
        feed_drain(feed, &mut feed_pending, &mut feed_dropped, &mut feed_logged);

        // (1) The next event (bounded wait only while input is held).
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
            match surf.wait_event() {
                Ok(e) => Some(e),
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
                            match normal_key(e.code, e.rune) {
                                NormalAct::ScrollLines(n) => {
                                    scroll_up += n * cell_h;
                                    dirty = true;
                                }
                                NormalAct::ScrollHalfPage(n) => {
                                    scroll_up += n * (h as i32 / 2);
                                    dirty = true;
                                }
                                NormalAct::Top => {
                                    scroll_up = i32::MAX / 2;
                                    dirty = true;
                                }
                                NormalAct::Bottom => {
                                    scroll_up = 0;
                                    dirty = true;
                                }
                                NormalAct::ToInsert => {
                                    mode = Mode::Insert;
                                    scroll_up = 0;
                                    dirty = true;
                                }
                                NormalAct::None => {}
                            }
                        } else if e.rune == 0x1b {
                            // Esc enters Normal (the Helix-modal boundary;
                            // full-screen ESC consumers live in raw-VT
                            // panes, H-3).
                            mode = Mode::Normal;
                            dirty = true;
                        } else {
                            keybuf.clear();
                            key_bytes(e.code, e.value, e.rune, &mut keybuf);
                            if !keybuf.is_empty() {
                                feed_pending.extend_from_slice(&keybuf);
                                feed_drain(feed, &mut feed_pending, &mut feed_dropped,
                                           &mut feed_logged);
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
                        }
                        Err(TapError::Busy) => {}
                        Err(e2) => {
                            say!("halcyond: reweave failed {:?}; exiting", e2);
                            return 1;
                        }
                    }
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
        if !drain_eof {
            for _ in 0..8 {
                let mut pfd = [TPollFd { fd: drain as i32, events: T_POLLIN, revents: 0 }];
                let rc = unsafe { t_poll(pfd.as_mut_ptr(), 1, 0) };
                if rc <= 0 || (pfd[0].revents & T_POLLIN) == 0 {
                    break;
                }
                let n = unsafe { t_read(drain, drainbuf.as_mut_ptr(), drainbuf.len()) };
                if n > 0 {
                    t.feed(&drainbuf[..n as usize]);
                } else if n == 0 {
                    drain_eof = true;
                    break;
                } else {
                    drain_eof = true;
                    say!("halcyond: consdrain read error {}", n);
                    break;
                }
            }
        }

        // (3) Render when the transcript moved or the view is dirty.
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
            // Lay everything visible; measure total height first.
            let mut heights: Vec<(u64, i32)> = Vec::new();
            let mut total: i32 = sheet.block_gap;
            for b in t.frozen_blocks().iter() {
                let lh = cache.get(b, widthi, &sheet, &mut gs).height;
                heights.push((b.id, lh));
                total += lh + sheet.block_gap;
            }
            let open_laid = layout_block(t.open_block(), widthi, &sheet, &mut gs);
            let pending_laid =
                layout_pending(t.pending_line(), &t.open_block().styles, widthi, &sheet, &mut gs);
            let open_h = open_laid.height + pending_laid.height;
            total += open_h;

            let viewh = h as i32;
            let max_up = (total - viewh).max(0);
            if scroll_up > max_up {
                scroll_up = max_up;
            }
            if scroll_up < 0 {
                scroll_up = 0;
            }
            // Bottom-anchored: the content's bottom sits at the view bottom
            // (raised by scroll_up).
            let y0 = if total <= viewh { 0 } else { viewh - total + scroll_up };

            let mut cart = cartoon::Cartoon::new();
            cart.ops.push(cartoon::Op::Clear { color: sheet.ground });
            let mut y = y0 + sheet.block_gap;
            for b in t.frozen_blocks().iter() {
                let lh = heights.iter().find(|(id, _)| *id == b.id).map(|(_, h)| *h).unwrap_or(0);
                if y + lh >= 0 && y <= viewh {
                    let laid = cache.get(b, widthi, &sheet, &mut gs);
                    render_block(&mut cart, laid, y, &gs);
                }
                y += lh + sheet.block_gap;
            }
            if y + open_h >= 0 && y <= viewh {
                render_block(&mut cart, &open_laid, y, &gs);
            }
            let py = y + open_laid.height;
            render_block(&mut cart, &pending_laid, py, &gs);
            // The cursor: a beam at the pending column (Insert ink; Normal
            // renders it hollow-dim -- the mode is visible at a glance).
            let (cx, cy, ch2) = cursor_pos(&pending_laid, t.pending_col(), &sheet);
            let ccol = if mode == Mode::Insert { sheet.ink } else { sheet.dim };
            cart.ops.push(cartoon::Op::Rect {
                x: cx,
                y: py + cy,
                w: 2,
                h: ch2.max(4) as u32,
                color: ccol,
            });

            let px = surf.pixels();
            cartoon::execute(&cart, &gs.packer.store, &cartoon::BlobStore::new(), px, w, None);
            match surf.present(None) {
                Ok(()) => {
                    present_fails = 0;
                }
                Err(_) => {
                    // A dropped frame, never death (#31); the next pass
                    // re-renders. A live-stream-but-never-presents wedge
                    // is the backstop below.
                    present_fails += 1;
                    dirty = true;
                    if present_fails >= PRESENT_FAILS_FATAL {
                        say!("halcyond: {} consecutive present failures; exiting", present_fails);
                        return 1;
                    }
                }
            }
        }
    }
}
