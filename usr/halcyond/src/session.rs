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

use halcyond::chrome::{parse_leaves, parse_rect};
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
use libthyla_rs::{t_poll, t_read, t_write, TPollFd, T_POLLHUP, T_POLLIN};
use tapestry::{EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_KEY};

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

fn write_all_fd(fd: i64, buf: &[u8]) {
    let mut off = 0usize;
    while off < buf.len() {
        // SAFETY: SVC wrapper over this thread's own buffer.
        let w = unsafe { t_write(fd, buf.as_ptr().add(off), buf.len() - off) };
        if w <= 0 {
            break;
        }
        off += w as usize;
    }
}

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
    fn spawn(leaf: u32, surf: Surface, geom: Geom) -> Option<SessionTile> {
        let cols = ((surf.w as i32 / geom.cell_w).max(1)) as u16;
        let rows = ((surf.h as i32 / geom.cell_h).max(1)) as u16;
        let mut child = Command::new("/bin/kaua-term")
            .arg(format!("{}", cols))
            .arg(format!("{}", rows))
            .arg("/bin/ut")
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
            tile: Tile::new(cols as usize, rows as usize, daylight_palette()),
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
) {
    let layout = match read_file(troot, "layout") {
        Some(s) => s,
        None => return,
    };
    let leaves = parse_leaves(&layout);
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
            Err(_) => continue,
        };
        match SessionTile::spawn(leaf, surf, geom) {
            Some(t) => {
                tiles.insert(leaf, t);
            }
            None => say!("halcyond: session tile leaf={} spawn failed", leaf),
        }
    }
}

/// Connect to tapestryd as the user + take a fullscreen surface (d-2's proven
/// bootstrap, with the console path's bounded connect retry). SQPOLL from the
/// start: the unified poll needs the ring pollable off-thread.
fn connect() -> Option<(EventRing, Surface)> {
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
        match Surface::fullscreen_on(&r) {
            Ok(s) => return Some((r, s)),
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
    parse_leaves(&layout)
        .into_iter()
        .find(|l| l.surface == Some(sid))
        .map(|l| l.id)
}

pub fn run() -> i64 {
    let (ring, root_surf) = match connect() {
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
    match SessionTile::spawn(root_leaf, root_surf, geom) {
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
                    say!("halcyond: session up {}x{} px", disp_w, disp_h);
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
                                    write_all_fd(t.down_fd, &wire_out);
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
                                write_all_fd(t.down_fd, &wire_out);
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
            reconcile(&ring, troot, &mut tiles, &mut closed, geom);
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
        let nfds = fds.len();
        if unsafe { t_poll(fds.as_mut_ptr(), nfds, -1) } < 0 {
            say!("halcyond: session poll failed (compositor gone); exiting");
            logout = Some(1);
            break;
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
