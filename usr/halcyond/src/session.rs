// halcyond::session (bin) -- the per-user session compositor's MULTI-TILE
// body (HALCYON.md 14.11.6/.7/.9/.10 + 14.12, KT-1.5d-3). One `kaua-term`
// process + content Surface + `Tile` model per compositor leaf, reconciled
// off the `layout` file each relayout; every tile's up-pipe folds into ONE
// poll { ring | up_0..up_N }; input reaches the focused tile because the
// compositor delivers KEY only to the focused surface (server key_event ->
// layout.focused_surface), so a per-surface event drain is inherently
// focus-routed -- halcyond never reads focus for routing. A tile's exit,
// crash, or close is contained to that tile (14.11.10); the session logs out
// when the last tile is gone.
//
// d-2 was the single-tile special case of this loop; d-3 generalizes it. The
// pure create/drop diff lives in the host-tested `halcyond::tiles`; this
// module is the I/O half of the 13.1 lib/bin split: connect, spawn, claim,
// poll, ingest -- none of it host-buildable (guest syscalls + Surface).

use alloc::collections::{BTreeMap, BTreeSet};
use alloc::format;
use alloc::vec::Vec;

use alloc::string::String;
use halcyond::chrome::{parse_leaves_all, parse_rect};
use halcyond::downq::DownQueue;
use halcyond::input::map_key;
use halcyond::layout::{daylight_sheet, Sheet};
use halcyond::raster::GlyphSource;
use halcyond::tile::Tile;
use halcyond::tiles::plan_tiles;
use kaua_term::wire::{encode_input, parse_record, FrameDecoder, Input};
use libhalcyon::theme::daylight_palette;
use libthyla_rs::fs::File;
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{t_poll, t_read, t_write, TPollFd, T_POLLHUP, T_POLLIN, T_POLLOUT};
use tapestry::{
    EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_FOCUS, TEV_KEY, TEV_LAYOUT,
};

use crate::chromeset::read_file;

macro_rules! say {
    ($($arg:tt)*) => {{
        let mut s = alloc::string::String::new();
        let _ = core::fmt::write(&mut s, format_args!($($arg)*));
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

const CONNECT_TRIES: u32 = 200;
const CONNECT_DELAY_MS: u64 = 25;
/// A layout verb (`close`) can be refused E_AGAIN by the compositor's
/// per-pass mutation budget; retry through it, as the restore tool does.
const VERB_RETRIES: u32 = 40;
const VERB_NAP_MS: u64 = 10;
const E_AGAIN: i64 = -11;
/// A never-succeeding present is a wedge, not a dropped frame (#31); this many
/// consecutive failures on any tile ends the session rather than spinning.
const PRESENT_FAILS_FATAL: u32 = 240;
const INGEST_BUF: usize = 8192;

/// The shared geometry: the mono cell size (for cols/rows) and the display
/// size (the create hint for a claim-placed surface, corrected by CONFIGURE).
#[derive(Clone, Copy)]
struct Geom {
    cell_w: i32,
    cell_h: i32,
    disp_w: u32,
    disp_h: u32,
}

/// The scrollback budget ONE session shares across all its tiles (their sum
/// must fit the 64 MiB heap; each tile's share moves as tiles come and go).
const SESSION_SCROLLBACK_BUDGET: usize = 32 << 20;

/// The kernel's `POLL_MAX_NFDS` (poll.h): a larger set is refused -1 before
/// any fd is looked at, and the loop reads -1 as "compositor gone". At most
/// 30 tiles exist (`MAX_PANES` 32 holds the root container + the console
/// leaf), so the ring + one POLLIN + one POLLOUT per tile is 61 -- the
/// ceiling below is a defence against a raised pane cap, not a live bound.
const POLL_MAX_NFDS: usize = 64;

/// The bounded wait when a tile's POLLOUT entry did not fit the poll set
/// (unreachable at today's pane cap; the defence a raised cap would need).
const DOWN_OMITTED_POLL_MS: i32 = 10;

/// How many connect iterations tolerate a refused `session on` before the
/// compositor runs UNDECLARED: the seat may be mid-handover (the previous
/// compositor's conn not yet retired), which clears within milliseconds.
const DECLARE_TRIES: u32 = 40;

/// Write one layout verb (`close <leaf>`) to the compositor `layout` file,
/// retried through the per-pass mutation budget. Best-effort: a wedged
/// compositor is caught by the ring/poll error path, and the `closed` set is
/// the authoritative respawn guard regardless of this verb's fate.
fn layout_verb(troot: i64, cmd: &str) {
    for _ in 0..VERB_RETRIES {
        let fd =
            unsafe { libthyla_rs::t_open(troot, b"layout".as_ptr(), 5, libthyla_rs::T_OWRITE) };
        if fd < 0 {
            return;
        }
        let rc = unsafe { t_write(fd, cmd.as_ptr(), cmd.len()) };
        unsafe { libthyla_rs::t_close(fd) };
        if rc != E_AGAIN {
            return;
        }
        let _ = sleep(Duration::from_millis(VERB_NAP_MS));
    }
}

/// Mint + read the one-shot placement claim on leaf `id` (`pane/<id>/claim`).
/// The offset-0 read mints a fresh token iff the reader OWNS the empty leaf
/// (server-side owner+emptiness authority, HALCYON.md 13.7); a failed read
/// (E_PERM: not ours or occupied, E_NOENT: not a leaf) yields None, which the
/// caller treats as "not mine to fill". Read-to-EOF via `read_file` spends one
/// mint (the token is pinned to the fid).
fn mint_claim(troot: i64, id: u32) -> Option<u128> {
    let s = read_file(troot, &format!("pane/{}/claim", id))?;
    u128::from_str_radix(s.trim(), 16).ok()
}

/// The leaf's content rect (w, h) from `pane/<id>/geometry`, so a claim-placed
/// surface is minted at the leaf's exact size -- no display-sized transient
/// that a follow-up CONFIGURE would immediately shrink. None (fall back to the
/// display size) if the file is absent or degenerate; a CONFIGURE still
/// corrects any staleness.
fn leaf_geometry(troot: i64, id: u32) -> Option<(u32, u32)> {
    let s = read_file(troot, &format!("pane/{}/geometry", id))?;
    let r = parse_rect(&s)?;
    if r.2 == 0 || r.3 == 0 {
        return None;
    }
    Some((r.2, r.3))
}

/// One session tile: a `kaua-term` child hosting `ut`, its content Surface in
/// leaf `leaf`, the two pipe ends (kept alive by value so their fds stay
/// open), and the `Tile` grid+scrollback model fed by the child's records.
struct SessionTile {
    surf: Surface,
    child: Child,
    // The parent pipe ends: fd 0 = down (we write Key/Resize Input), fd 1 =
    // up (child writes Records). Held by value so the fds outlive the tile.
    _down: File,
    _up: File,
    down_fd: i64,
    up_fd: i64,
    /// Encoded Key/Resize input not yet delivered down the pipe (bounded).
    down: DownQueue,
    drop_said: bool,
    tile: Tile,
    dec: FrameDecoder,
    cols: u16,
    rows: u16,
    dirty: bool,
    /// None = live; Some(code) = the child is gone. A clean exit closes the
    /// leaf immediately (reaped there); a crash keeps the tile as a frozen
    /// affordance (14.11.10) -- its pipe skipped, its last frame held -- reaped
    /// only when the user closes the leaf.
    exit: Option<i32>,
}

/// What one ingest pass concluded for a tile.
enum Ingested {
    /// Records applied (or a harmless empty wake); the tile is live.
    Live,
    /// `Control::Exit` with a clean status: the shell exited normally, so the
    /// pane closes (tmux rule) -- collapse the leaf, reap the tile.
    CleanExit(i32),
    /// An up-pipe EOF with a non-clean prior exit, or a `WireError` (an
    /// oversize/malformed frame from the crash-isolated parser): keep the tile
    /// frozen as an affordance.
    Crash,
}

impl SessionTile {
    /// Spawn a `kaua-term` on `ut` sized to `surf`, wired to the two pipes.
    fn spawn(
        leaf: u32,
        surf: Surface,
        geom: Geom,
        home: Option<&str>,
        budget: usize,
    ) -> Option<SessionTile> {
        let cols = ((surf.w as i32 / geom.cell_w).max(1)) as u16;
        let rows = ((surf.h as i32 / geom.cell_h).max(1)) as u16;
        let mut cmd = Command::new("/bin/kaua-term");
        cmd.arg(format!("{}", cols))
            .arg(format!("{}", rows))
            .arg("/bin/ut");
        if let Some(h) = home {
            cmd.arg("--home").arg(h);
        }
        // The identity axis stops here whatever the parent holds: a tile's
        // programs never spawn as another principal (login masks it too; this
        // is the second hop's own guard).
        let mut child = cmd
            .caps(!libthyla_rs::T_CAP_SET_IDENTITY)
            .stdin(Stdio::Piped)
            .stdout(Stdio::Piped)
            .stderr(Stdio::Inherit)
            .spawn()
            .ok()?;
        let pid = child.pid();
        // Stdio::Piped guarantees both ends, but never leak a spawned kaua-term:
        // Child has no reaping Drop, so a missing end must kill + reap here.
        let (down, up) = match (child.stdin.take(), child.stdout.take()) {
            (Some(d), Some(u)) => (d, u),
            _ => {
                let _ = child.kill();
                let _ = child.wait();
                return None;
            }
        };
        let down_fd = down.as_raw_fd() as i64;
        let up_fd = up.as_raw_fd() as i64;
        say!(
            "halcyond: session tile leaf={} spawned pid={} {}x{}",
            leaf,
            pid,
            cols,
            rows
        );
        Some(SessionTile {
            surf,
            child,
            _down: down,
            _up: up,
            down_fd,
            up_fd,
            down: DownQueue::new(),
            drop_said: false,
            tile: Tile::with_budget(cols as usize, rows as usize, daylight_palette(), budget),
            dec: FrameDecoder::new(),
            cols,
            rows,
            dirty: true,
            exit: None,
        })
    }

    /// Render + present the tile if dirty. Returns false on a wedge (present
    /// failed too many times in a row -- the caller counts it globally).
    fn render_if_dirty(
        &mut self,
        cart: &mut cartoon::Cartoon,
        gs: &mut GlyphSource,
        sheet: &Sheet,
    ) -> bool {
        if !self.dirty {
            return true;
        }
        self.dirty = false;
        let (sw, sh) = (self.surf.w as usize, self.surf.h as usize);
        self.tile.render(cart, sw, sh, gs, sheet, 0);
        {
            let px = self.surf.pixels();
            cartoon::execute(
                cart,
                &gs.packer.store,
                &cartoon::BlobStore::new(),
                px,
                sw,
                None,
            );
        }
        match self.surf.present(None) {
            Ok(()) => true,
            Err(_) => {
                // A dropped frame, never death (#31); re-render next pass.
                self.dirty = true;
                false
            }
        }
    }

    /// Drain one wake's worth of records from the up-pipe into the tile.
    fn ingest(&mut self, buf: &mut [u8]) -> Ingested {
        // SAFETY: SVC wrapper over the caller's stack buffer.
        let n = unsafe { t_read(self.up_fd, buf.as_mut_ptr(), buf.len()) };
        if n <= 0 {
            // EOF. If the child already reported a clean exit we handled it;
            // an unexpected EOF (no Exit record) is an abnormal death.
            return match self.tile.exited() {
                Some(0) => Ingested::CleanExit(0),
                Some(c) => {
                    let _ = c;
                    Ingested::Crash
                }
                None => Ingested::Crash,
            };
        }
        self.dec.push(&buf[..n as usize]);
        loop {
            match self.dec.next_frame() {
                Some(Ok((tag, payload))) => match parse_record(tag, &payload) {
                    Ok(rec) => {
                        self.tile.apply(rec);
                        self.dirty = true;
                    }
                    // A malformed record from the untrusted parser: desync.
                    Err(_) => return Ingested::Crash,
                },
                // An oversize frame: unrecoverable stream desync.
                Some(Err(_)) => return Ingested::Crash,
                None => break,
            }
        }
        // A clean exit arrived interleaved in the record stream.
        match self.tile.exited() {
            Some(0) => Ingested::CleanExit(0),
            Some(_) => Ingested::Crash,
            None => Ingested::Live,
        }
    }

    /// Kill + reap the child so it never lingers as a zombie, then let the
    /// Queue an encoded key record for the tile's kaua-term (delivered by
    /// `drain_down`). Bounded: past the cap the NEWEST keys drop, said once.
    fn queue_key(&mut self, bytes: &[u8]) {
        if !self.down.push_key(bytes) && !self.drop_said {
            self.drop_said = true;
            say!("halcyond: session tile input dropped (its terminal is not draining)");
        }
    }

    /// Queue the geometry record: never dropped, delivered before any
    /// further key (see `DownQueue`).
    fn queue_resize(&mut self, bytes: &[u8]) {
        self.down.push_resize(bytes);
    }

    /// Deliver queued input without ever blocking the compositor: natives
    /// cannot mark a pipe non-blocking, and a whole-key write that does not
    /// fit the ring parks the writer -- but POLLOUT means at least one free
    /// byte and this thread is the pipe's only writer, so a one-byte write
    /// after a ready POLLOUT can never block. Stops at the first "no room".
    fn drain_down(&mut self) {
        while let Some(b) = self.down.next_byte() {
            let mut pfd = [TPollFd {
                fd: self.down_fd as i32,
                events: T_POLLOUT,
                revents: 0,
            }];
            // SAFETY: SVC wrapper over this thread's own array.
            if unsafe { t_poll(pfd.as_mut_ptr(), 1, 0) } <= 0 || pfd[0].revents & T_POLLOUT == 0 {
                break;
            }
            let byte = [b];
            // SAFETY: SVC wrapper over this thread's own buffer, one byte.
            let w = unsafe { t_write(self.down_fd, byte.as_ptr(), 1) };
            if w <= 0 {
                break;
            }
            self.down.advance();
        }
    }

    /// Surface + pipe ends drop (Surface::drop says `destroy`; the pipe fds
    /// close, EOFing the kaua-term's input pump if it still lives).
    fn teardown(mut self) {
        let _ = self.child.kill();
        let _ = self.child.wait();
    }
}

/// Bring the tile set in line with the layout: reap orphaned tiles (leaf
/// gone), spawn tiles for new empty leaves we own (claim-gated). `closed` is
/// the permanent respawn guard.
fn reconcile(
    ring: &EventRing,
    troot: i64,
    tiles: &mut BTreeMap<u32, SessionTile>,
    closed: &mut BTreeSet<u32>,
    geom: Geom,
    home: Option<&str>,
) {
    let layout = match read_file(troot, "layout") {
        Some(s) => s,
        None => return,
    };
    let leaves = parse_leaves_all(&layout);
    let have: Vec<u32> = tiles.keys().copied().collect();
    let closed_v: Vec<u32> = closed.iter().copied().collect();
    let plan = plan_tiles(&leaves, &have, &closed_v);

    for leaf in plan.drop {
        if let Some(t) = tiles.remove(&leaf) {
            closed.insert(leaf);
            t.teardown();
        }
    }
    for leaf in plan.create {
        // The claim IS the owner+emptiness authority: a leaf that is not ours
        // (or already taken) fails here and is skipped.
        let token = match mint_claim(troot, leaf) {
            Some(t) => t,
            None => continue,
        };
        // Mint at the leaf's own content rect (a CONFIGURE still corrects any
        // staleness); fall back to the display size if geometry is unreadable.
        let (w, h) = leaf_geometry(troot, leaf).unwrap_or((geom.disp_w, geom.disp_h));
        let surf = match Surface::open_claim_on(ring, w, h, token) {
            Ok(s) => s,
            Err(e) => {
                // A leaf the compositor cannot host (the surface pool is at
                // its cap) must not stay empty and focused with the keyboard
                // routed into it: say once and close it.
                say!(
                    "halcyond: session tile leaf={} refused {:?} -- closing",
                    leaf,
                    e
                );
                let mut cmd = alloc::string::String::new();
                let _ = core::fmt::write(&mut cmd, format_args!("close {}", leaf));
                layout_verb(troot, &cmd);
                continue;
            }
        };
        let budget = SESSION_SCROLLBACK_BUDGET / (tiles.len() + 1);
        match SessionTile::spawn(leaf, surf, geom, home, budget) {
            Some(t) => {
                tiles.insert(leaf, t);
            }
            None => say!("halcyond: session tile leaf={} spawn failed", leaf),
        }
    }
    // Every tile's share of the ONE scrollback budget follows the tile count.
    let share = SESSION_SCROLLBACK_BUDGET / tiles.len().max(1);
    for t in tiles.values_mut() {
        t.tile.scrollback.set_max_cost(share);
    }
}

/// Connect to tapestryd as the user + take a fullscreen surface (d-2's proven
/// bootstrap, with the console path's bounded connect retry). SQPOLL from the
/// start: the unified poll needs the ring pollable off-thread. Returns whether
/// the display handoff was DECLARED: a refused declaration (the seat held by
/// another principal's live tiles, or a conn without a session principal) is
/// retried through `DECLARE_TRIES` and then tolerated -- the session runs
/// UNDECLARED, its tiles beside the console like any user window. Degraded,
/// but a session; exiting here would hand login a non-zero status and the
/// seat a re-prompt loop.
fn connect() -> Option<(EventRing, Surface, bool)> {
    let mut undeclared_said = false;
    for i in 0..CONNECT_TRIES {
        let r = match EventRing::connect_sqpoll() {
            Ok(r) => r,
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("halcyond: FAIL session connect {:?}", e);
                    return None;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
                continue;
            }
        };
        // The display handoff is an explicit act of the session compositor,
        // declared on its own conn BEFORE its first surface hosts: a program
        // merely drawing a window never takes the display from the console.
        let declared = match r.global_ctl("session on") {
            Ok(()) => true,
            Err(e) => {
                if i + 1 < DECLARE_TRIES {
                    let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
                    continue;
                }
                if !undeclared_said {
                    undeclared_said = true;
                    say!(
                        "halcyond: session declare refused {:?} -- running UNDECLARED beside the console",
                        e
                    );
                }
                false
            }
        };
        match Surface::fullscreen_on(&r) {
            Ok(s) => {
                // Re-verify now that a surface hosts: between the
                // declaration and this mint the conn held nothing, so an
                // idle re-claimer could take the seat back in that window;
                // a repeat `session on` is idempotent for the holder and a
                // takeover of an idle usurper, and its verdict is the one
                // that describes the session that actually runs.
                let declared = declared && r.global_ctl("session on").is_ok();
                if !declared && !undeclared_said {
                    say!("halcyond: session declaration lost before the first surface -- running UNDECLARED");
                }
                return Some((r, s, declared));
            }
            Err(e) => {
                if i == CONNECT_TRIES - 1 {
                    say!("halcyond: FAIL session connect/create {:?}", e);
                    return None;
                }
                let _ = sleep(Duration::from_millis(CONNECT_DELAY_MS));
            }
        }
    }
    None
}

/// Find the leaf hosting surface `sid` (our bootstrap root), so the root tile
/// keys on the same leaf id the reconcile diff uses.
fn leaf_hosting(troot: i64, sid: u32) -> Option<u32> {
    let layout = read_file(troot, "layout")?;
    parse_leaves_all(&layout)
        .into_iter()
        .find(|l| l.surface == Some(sid))
        .map(|l| l.id)
}

pub fn run(home: Option<String>) -> i64 {
    let (ring, root_surf, declared) = match connect() {
        Some(x) => x,
        None => return 1,
    };
    let troot = ring.root();

    // The render brain (HALCYON.md 14.12: the per-user compositor REUSES the
    // console render brain) -- ONE mono glyph source + Daylight sheet shared
    // across every tile.
    let mut gs = GlyphSource::new_vendored(512);
    if gs.face_count() != 2 {
        say!("halcyond: FAIL vendored face parse");
        return 1;
    }
    let sheet = daylight_sheet();
    let (cell_w, cell_h, _) = gs.mono_cell();
    let (disp_w, disp_h) = (root_surf.w, root_surf.h);
    let geom = Geom {
        cell_w,
        cell_h,
        disp_w,
        disp_h,
    };

    // The root tile, keyed on the leaf that hosts the bootstrap surface.
    let root_leaf = match leaf_hosting(troot, root_surf.id) {
        Some(l) => l,
        None => {
            say!("halcyond: FAIL locate session root leaf");
            return 1;
        }
    };
    let mut tiles: BTreeMap<u32, SessionTile> = BTreeMap::new();
    let mut closed: BTreeSet<u32> = BTreeSet::new();
    match SessionTile::spawn(
        root_leaf,
        root_surf,
        geom,
        home.as_deref(),
        SESSION_SCROLLBACK_BUDGET,
    ) {
        Some(t) => {
            tiles.insert(root_leaf, t);
        }
        None => {
            say!("halcyond: FAIL session root tile spawn");
            return 1;
        }
    }

    let mut cart = cartoon::Cartoon::new();
    let mut inbuf = [0u8; INGEST_BUF];
    let mut wire_out: Vec<u8> = Vec::new();
    let mut relayout = true;
    let mut up_announced = false;
    let mut ingest_announced = false;
    let mut present_fails: u32 = 0;
    let mut logout: Option<i32> = None;

    loop {
        // (1) Render dirty tiles at the TOP: the root's first present precedes
        // any wait (first-present-wins scanout; frame ticks reach only visible
        // surfaces).
        for t in tiles.values_mut() {
            let was_dirty = t.dirty;
            let ok = t.render_if_dirty(&mut cart, &mut gs, &sheet);
            if !was_dirty {
                continue;
            }
            if ok {
                present_fails = 0;
                // "session up" witnesses a SUCCESSFUL present (the post-present
                // marker rule), not merely the connect -- printed once, on the
                // first tile that presents.
                if !up_announced {
                    up_announced = true;
                    say!(
                        "halcyond: session up {}x{} px{}",
                        disp_w,
                        disp_h,
                        if declared { "" } else { " (undeclared)" }
                    );
                }
            } else {
                present_fails += 1;
                if present_fails >= PRESENT_FAILS_FATAL {
                    say!(
                        "halcyond: {} consecutive present failures; exiting",
                        present_fails
                    );
                    logout = Some(1);
                }
            }
        }
        if logout.is_some() {
            break;
        }

        // (2) Drain every tile's surface events (CONFIGURE/CLOSE/KEY). A KEY
        // reaches only the focused surface (compositor-routed), so per-surface
        // drain is inherently focus-routed (14.11.9). A CLOSE (the user closed
        // the leaf, or a leaf collapse) marks the tile for reap.
        let mut reap: Vec<u32> = Vec::new();
        for (&leaf, t) in tiles.iter_mut() {
            loop {
                match t.surf.poll_event() {
                    Ok(Some(e)) => match e.kind {
                        TEV_CLOSE => reap.push(leaf),
                        TEV_CONFIGURE => match t.surf.handle_configure(&e) {
                            Ok(_) => {
                                let nc = ((t.surf.w as i32 / cell_w).max(1)) as u16;
                                let nr = ((t.surf.h as i32 / cell_h).max(1)) as u16;
                                if (nc != t.cols || nr != t.rows) && t.exit.is_none() {
                                    t.cols = nc;
                                    t.rows = nr;
                                    t.tile.resize(nc as usize, nr as usize);
                                    wire_out.clear();
                                    encode_input(
                                        &Input::Resize { cols: nc, rows: nr },
                                        &mut wire_out,
                                    );
                                    t.queue_resize(&wire_out);
                                }
                                t.dirty = true;
                                // A relayout may have added or removed leaves.
                                relayout = true;
                            }
                            Err(TapError::Busy) => {}
                            Err(_) => reap.push(leaf),
                        },
                        // A dead tile's keys drop (its ut is gone); a live
                        // tile only ever sees KEY when focused (compositor
                        // routing), so this is the focus-routed input path.
                        TEV_KEY if t.exit.is_none() => {
                            if let Some(kev) = map_key(e.code, e.rune, e.value) {
                                wire_out.clear();
                                encode_input(&Input::Key(kev), &mut wire_out);
                                t.queue_key(&wire_out);
                            }
                        }
                        // A focus move may follow a split of an EMPTY leaf
                        // (nothing hosted yet, so no CONFIGURE arrives), and
                        // a structural change with no hosted surface in it
                        // fans only TEV_LAYOUT to the declared conn: let the
                        // reconcile see the new leaves now, not later.
                        TEV_FOCUS | TEV_LAYOUT => relayout = true,
                        _ => {}
                    },
                    Ok(None) => break,
                    Err(_) => {
                        say!("halcyond: session event stream ended (compositor gone); exiting");
                        logout = Some(1);
                        break;
                    }
                }
            }
            if logout.is_some() {
                break;
            }
        }
        if logout.is_some() {
            break;
        }
        for leaf in reap {
            if let Some(t) = tiles.remove(&leaf) {
                closed.insert(leaf);
                t.teardown();
            }
        }
        if tiles.is_empty() {
            break;
        }

        // (3) Reconcile if a relayout happened (a split added a leaf; a close
        // removed one). New tiles come up dirty; the loop re-renders below.
        if relayout {
            relayout = false;
            reconcile(&ring, troot, &mut tiles, &mut closed, geom, home.as_deref());
            if tiles.is_empty() {
                break;
            }
        }

        // If any tile needs a paint (a new tile, a resize), render before we
        // block, so no dirty tile waits on the next wake.
        if tiles.values().any(|t| t.dirty) {
            continue;
        }

        // (4) Block: poll { ring | each LIVE tile's up-pipe }. A dead/crashed
        // tile's pipe is skipped (EOF'd); its surface events still arrive via
        // the ring.
        let mut fds: Vec<TPollFd> = Vec::with_capacity(1 + tiles.len());
        fds.push(TPollFd {
            fd: ring.poll_fd(),
            events: T_POLLIN,
            revents: 0,
        });
        let mut up_leaves: Vec<u32> = Vec::with_capacity(tiles.len());
        for (&leaf, t) in tiles.iter() {
            if t.exit.is_none() {
                fds.push(TPollFd {
                    fd: t.up_fd as i32,
                    events: T_POLLIN,
                    revents: 0,
                });
                up_leaves.push(leaf);
            }
        }
        // Undelivered input wakes the loop when its pipe has room. Appended
        // AFTER the up entries, so the up_leaves[i] <-> fds[i+1] map holds;
        // capped at the kernel's set ceiling. A tile left OUT has nothing
        // watching its pipe, and a quiet session would park on it forever
        // (a lost wake: readiness the set cannot see) -- so the wait is then
        // bounded instead, and the omitted tile is drained on that tick.
        let mut omitted = false;
        for t in tiles.values() {
            if t.exit.is_none() && !t.down.is_empty() {
                if fds.len() >= POLL_MAX_NFDS {
                    omitted = true;
                    break;
                }
                fds.push(TPollFd {
                    fd: t.down_fd as i32,
                    events: T_POLLOUT,
                    revents: 0,
                });
            }
        }
        let nfds = fds.len();
        let timeout = if omitted { DOWN_OMITTED_POLL_MS } else { -1 };
        if unsafe { t_poll(fds.as_mut_ptr(), nfds, timeout) } < 0 {
            say!("halcyond: session poll failed (compositor gone); exiting");
            logout = Some(1);
            break;
        }
        for t in tiles.values_mut() {
            if t.exit.is_none() && !t.down.is_empty() {
                t.drain_down();
            }
        }

        // (5) Ingest each readable tile's records. up_leaves[i] <-> fds[i+1].
        let mut reap: Vec<u32> = Vec::new();
        for (i, &leaf) in up_leaves.iter().enumerate() {
            if fds[i + 1].revents & (T_POLLIN | T_POLLHUP) == 0 {
                continue;
            }
            let Some(t) = tiles.get_mut(&leaf) else {
                continue;
            };
            match t.ingest(&mut inbuf) {
                Ingested::Live => {
                    if !ingest_announced {
                        ingest_announced = true;
                        say!("halcyond: session tile ingest live");
                    }
                }
                Ingested::CleanExit(code) => {
                    say!(
                        "halcyond: session tile leaf={} exited (code {}) -- closing",
                        leaf,
                        code
                    );
                    // The shell exited: collapse the leaf (tmux rule). The
                    // `closed` set guarantees no respawn even if the verb is
                    // refused; the tile is reaped here.
                    t.exit = Some(code);
                    closed.insert(leaf);
                    layout_verb(troot, &format!("close {}", leaf));
                    relayout = true;
                    reap.push(leaf);
                }
                Ingested::Crash => {
                    // The crash-isolated parser died or the stream desynced:
                    // freeze the tile as an affordance (14.11.10), stop polling
                    // its pipe, kill the kaua-term. Reaped when the user closes
                    // the leaf. Contained -- the environment lives on.
                    say!(
                        "halcyond: session tile leaf={} crashed -- affordance held",
                        leaf
                    );
                    t.exit = Some(1);
                    t.dirty = true;
                    let _ = t.child.kill();
                }
            }
        }
        for leaf in reap {
            if let Some(t) = tiles.remove(&leaf) {
                t.teardown();
            }
        }
        if tiles.is_empty() {
            break;
        }
    }

    // Teardown: every surviving tile (its Surface drop says `destroy`; the
    // child is killed + reaped). login's wait() then returns -> getty -> the
    // next login -> aurora un-backgrounds + resumes (14.12 step 4).
    let code = logout.unwrap_or(0);
    say!("halcyond: session logout (code {})", code);
    for (_, t) in core::mem::take(&mut tiles) {
        t.teardown();
    }
    code as i64
}
