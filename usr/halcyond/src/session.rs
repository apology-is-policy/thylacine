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
use beacon::verbs::{parse as parse_verbs, Rule};
use halcyond::chrome::{abbrev_home, parse_leaves_all, parse_rect, program_name};
use halcyond::downq::DownQueue;
use halcyond::input::{map_key, normal_key, Mode, NormalAct};
use halcyond::layout::layout_block;
use halcyond::layout::{sheet_for, Sheet};
use halcyond::menu::{
    build_menu, hit_run, obj_of, run_rect, runs_on_row, step_run_with, Action, Menu, ObjRun,
};
use halcyond::raster::GlyphSource;
use halcyond::select::{flatten_with_grid, FlatRow, Sel, GRID_BLOCK};
use halcyond::session_init;
use halcyond::tile::Tile;
use halcyond::tile::{Mark, GRID_KEY};
use halcyond::tiles::{plan_tiles, tile_command};
use kaua_term::wire::{encode_input, parse_record, FrameDecoder, Input};
use kaua_term::{Record, ScreenMode};
use libhalcyon::scale;
use libhalcyon::instrument;
use libhalcyon::theme::{self, env_palette};
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::Write;
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{t_poll, t_read, t_write, TPollFd, T_POLLHUP, T_POLLIN, T_POLLOUT};
use tapestry::{
    DisplayInfo, EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_FOCUS, TEV_KEY,
    TEV_LAYOUT, TEV_PTR_BTN, TEV_PTR_MOVE,
};

use crate::chromeset::{self, read_file, ChromeAction};
use crate::railset;
use crate::menuset::{self, MenuEvent};
use crate::statusset;
use halcyond::chrome::{Described, Fate};
use halcyond::menu::{tile_menu, workspace_menu};
use halcyond::rail::{hints_from_chords, reset_plan, RailModel};

/// evdev BTN_LEFT (the tapestry PTR_BTN `code`).
const BTN_LEFT: u16 = 0x110;

/// H-4d: a request to summon the verb menu over one tile: the built menu,
/// the anchor point and the obj run's rect, all in the tile's SURFACE
/// coordinates (the loop adds the tile's content origin).
struct MenuReq {
    model: Menu,
    ax: i32,
    ay: i32,
    run: (i32, i32, i32, i32),
}

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
use crate::chromeset::{E_AGAIN, VERB_NAP_MS, VERB_RETRIES};
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
/// While the session init child (halcyon.rc / the default restore) runs, the
/// idle wait is bounded so its exit is reaped promptly (a zombie holds a
/// proc-table slot until the next wake otherwise).
const INIT_REAP_POLL_MS: i32 = 200;

/// H-4d: the mark this compositor leaves in the session's /env (every tile
/// and the init child inherit it): a `halcyon layout restore` that sees it
/// only TAGS the leaves it builds -- the compositor hosts each tag in a
/// terminal tile once the tool's conn is gone.
const SESSION_ENV_PATH: &str = "/env/HALCYON_SESSION";
/// The session's resolved theme palette, published for the programs its tiles
/// run (s7a-3). A hosted pts program (nora) reads it to follow Daylight instead
/// of a hardcoded palette; the format is `libhalcyon::theme::env_palette`.
const HALCYON_PALETTE_ENV_PATH: &str = "/env/HALCYON_PALETTE";
/// The user's display-scale preference (HALCYON-SCALE 6): a percent the
/// session writes ONCE as the compositor's gated `scale` verb at start. A
/// preference, never a source -- the compositor stays the authority and its
/// ctl the channel, so the carve and the paint cannot disagree.
const HALCYON_SCALE_ENV_PATH: &str = "/env/HALCYON_SCALE";
/// The user's motion preference (HALCYON-INSTRUMENT 9.5 as amended at I-8):
/// motion is ON and `0` is the opt-out. Read at session start exactly as the
/// scale is, and for the same stated reason -- a user's preference rather
/// than a guess made on their behalf. Unlike the scale it is NOT forwarded to
/// the compositor: what it governs here is halcyond's own paint (section 10's
/// caret blink), and the compositor's own motion reads its own lever.
const HALCYON_MOTION_ENV_PATH: &str = "/env/HALCYON_MOTION";

/// How many connect iterations tolerate a refused `session on` before the
/// compositor runs UNDECLARED: the seat may be mid-handover (the previous
/// compositor's conn not yet retired), which clears within milliseconds.
const DECLARE_TRIES: u32 = 40;

/// Write one layout verb (`close <leaf>`) to the compositor `layout` file,
/// retried through the per-pass mutation budget. Best-effort: a wedged
/// compositor is caught by the ring/poll error path, and the `closed` set is
/// the authoritative respawn guard regardless of this verb's fate.
fn layout_verb(troot: i64, cmd: &str) -> bool {
    layout_verb_in(troot, cmd, &mut chromeset::VerbBudget::pass())
}

/// `layout_verb` on a shared retry budget: a plan of many verbs (the reset)
/// naps at most one verb's worth across the whole pass (r2 C-F8).
fn layout_verb_in(troot: i64, cmd: &str, budget: &mut chromeset::VerbBudget) -> bool {
    loop {
        // The path's LENGTH is the slice's: this said 5 for six bytes since
        // I-3 -- opening `layou` -- and no gate had written a layout verb
        // from the session until I-4's rail pressed SPLIT H.
        let path = b"layout";
        let fd = unsafe { libthyla_rs::t_open(troot, path.as_ptr(), path.len(), libthyla_rs::T_OWRITE) };
        if fd < 0 {
            say!("halcyond: layout verb \"{}\": open failed rc {}", cmd, fd);
            return false;
        }
        let rc = unsafe { t_write(fd, cmd.as_ptr(), cmd.len()) };
        unsafe { libthyla_rs::t_close(fd) };
        if rc != E_AGAIN {
            if rc < 0 {
                say!("halcyond: layout verb \"{}\" refused rc {}", cmd, rc);
            }
            return rc >= 0;
        }
        if !budget.nap() {
            say!("halcyond: layout verb \"{}\" still busy after {} tries (the pass's budget)", cmd, VERB_RETRIES);
            return false;
        }
        let _ = sleep(Duration::from_millis(VERB_NAP_MS));
    }
}

/// HALCYON-INSTRUMENT 14.9: parse a tile menu action `tile <verb> <id>`.
fn tile_verb(act: &str) -> Option<(&str, u32)> {
    let mut it = act.strip_prefix("tile ")?.split_ascii_whitespace();
    let verb = it.next()?;
    let id: u32 = it.next()?.parse().ok()?;
    if it.next().is_some() {
        return None;
    }
    Some((verb, id))
}

/// The size of the stack holding leaf `id`, off the layout (1 for a stack
/// of one; the final-tile rule's input when a menu choice, not a header
/// press, asks for a close).
pub(crate) fn tile_count(troot: i64, id: u32) -> u32 {
    read_file(troot, "layout")
        .map(|l| {
            halcyond::chrome::parse_tree(&l)
                .iter()
                .find(|t| t.leaf.id == id)
                .map_or(1, |t| t.count)
        })
        .unwrap_or(1)
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
    leaf: u32,
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
    /// The one-shot "this tile presents objects" witness (test builds).
    objs_said: bool,
    /// H-4d: the Helix-modal transcript mode (HALCYON.md 4): Esc leaves
    /// Insert for Normal, where the cursor walks the rows and `w`/`b` walk
    /// the obj runs; Enter opens the run's verb menu; `i` returns.
    mode: Mode,
    /// The flat row list + the cursor, live only in Normal mode (rebuilt
    /// when the transcript moved: `flat_seq` vs `scrollback.seq`).
    flat: Vec<FlatRow>,
    flat_seq: u64,
    sel: Option<Sel>,
    /// The view's offset from the content bottom (pixels); the cursor drags
    /// it in Normal mode, Insert re-anchors at 0.
    scroll_up: i32,
    /// The pointer's last surface position (a BTN event carries none).
    ptr: (i32, i32),
    /// None = live; Some(code) = the child is gone. Under the legacy
    /// profile a clean exit closes the leaf immediately (reaped there) and
    /// anything else keeps the tile as a frozen affordance (14.11.10) --
    /// its pipe skipped, its last frame held -- reaped only when the user
    /// closes the leaf. Under Instrument every gone child is a RETAINED
    /// tile (HALCYON-INSTRUMENT 14.6) with a `fate`.
    exit: Option<i32>,
    /// HALCYON-INSTRUMENT 14.6: what became of the child, once gone -- the
    /// header's metadata word and the body's mark (`Tile.fate` mirrors it).
    fate: Fate,
    /// The command line the tile hosts (`tile_command`), kept for a
    /// Restart (14.6: a distinct NEW process in the same leaf).
    argv: Vec<String>,
    /// The tile's program (its command line's first word: `ut` for the
    /// shell) -- the strip's name (HALCYON-VISUAL 4.1).
    program: String,
    /// The working directory the strip's trail last showed: a `cd` moves
    /// the trail with no relayout to repaint it, so the loop compares.
    trail_painted: String,
}

/// What one ingest pass concluded for a tile.
enum Ingested {
    /// Records applied (or a harmless empty wake); the tile is live.
    Live,
    /// `Control::Exit` with this status (interleaved, or at the EOF that
    /// follows it): the tile's program ended. Legacy: a clean one closes
    /// the pane (tmux rule), a non-clean one freezes; Instrument: retained
    /// as `EXIT n` (14.6).
    Ended(i32),
    /// An up-pipe EOF with NO exit reported: the connection went without a
    /// word (14.6 `DISCONNECTED`; legacy: frozen).
    Disconnected,
    /// A `WireError` (an oversize / malformed frame from the crash-isolated
    /// parser): the stream desynced (14.6 `CRASHED`; legacy: frozen).
    Crashed,
}

impl SessionTile {
    /// Spawn a `kaua-term` hosting `argv` (the tile's command line: the
    /// shell, or the leaf's tag -- `tile_command`) sized to `surf`, wired to
    /// the two pipes.
    fn spawn(
        leaf: u32,
        surf: Surface,
        geom: Geom,
        argv: &[String],
        budget: usize,
        // The RESOLVED theme's terminal palette (HALCYON-THEME 3.4). The tile
        // is born in it AND the child is told it, from this one value, so the
        // grid and the transcript beside it cannot disagree.
        palette: vt::Palette,
    ) -> Option<SessionTile> {
        let cols = ((surf.w as i32 / geom.cell_w).max(1)) as u16;
        let rows = ((surf.h as i32 / geom.cell_h).max(1)) as u16;
        let mut cmd = Command::new("/bin/kaua-term");
        // Everything this compositor DECLARES to the tile: the RICH render
        // tier (halcyond rasterizes the transcript, which is what arms a tile
        // shell's zones and a tool's objects -- KAUA-TERM.md R1), and the
        // palette its cells are born in (HALCYON-THEME 3.1 -- the seam ships
        // resolved RGB, so this is the only moment the theme can be chosen).
        // Built by `session_init::tile_argv`, which is host-tested against the
        // parser the child actually runs.
        for a in session_init::tile_argv(kaua_term::cmdline::Tier::Rich, &palette, cols, rows, argv)
        {
            cmd.arg(a);
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
            "halcyond: session tile leaf={} spawned pid={} {}x{} {}",
            leaf,
            pid,
            cols,
            rows,
            argv.join(" ")
        );
        Some(SessionTile {
            leaf,
            surf,
            child,
            _down: down,
            _up: up,
            down_fd,
            up_fd,
            down: DownQueue::new(),
            drop_said: false,
            tile: Tile::with_budget(cols as usize, rows as usize, palette, budget),
            dec: FrameDecoder::new(),
            cols,
            rows,
            dirty: true,
            objs_said: false,
            mode: Mode::Insert,
            flat: Vec::new(),
            flat_seq: u64::MAX,
            sel: None,
            program: program_name(argv.first().map(|s| s.as_str()).unwrap_or("")),
            trail_painted: String::new(),
            scroll_up: 0,
            ptr: (0, 0),
            exit: None,
            fate: Fate::Live,
            argv: argv.to_vec(),
        })
    }

    /// Mark the tile retained with `fate` (14.6): the child is killed and
    /// reaped if it can be (a gone child costs one non-blocking wait), the
    /// pipe drops out of the poll set (`exit`), the body repaints with its
    /// state mark and no caret, the header's metadata says the word.
    fn retain(&mut self, fate: Fate, code: i32) {
        self.exit = Some(code);
        self.fate = fate;
        self.tile.fate = fate;
        self.dirty = true;
        let _ = self.child.kill();
        let _ = self.child.try_wait();
    }

    /// Take the surface and the command line out of a retained tile for a
    /// Restart (14.6): the terminal is hung up and reaped (killed past the
    /// grace); the surface stays live, so the leaf keeps its place, its
    /// weight and its frame.
    fn into_parts(self) -> (Surface, Vec<String>) {
        let SessionTile {
            surf,
            argv,
            mut child,
            _down,
            leaf,
            ..
        } = self;
        drop(_down);
        end_terminal(&mut child, leaf);
        (surf, argv)
    }

    /// The Normal-mode cursor as `Tile::render` paints it.
    fn mark(&self) -> Option<Mark> {
        if self.mode != Mode::Normal {
            return None;
        }
        let s = self.sel.as_ref()?;
        let fr = self.flat.get(s.cursor)?;
        let block = if fr.block == GRID_BLOCK {
            GRID_KEY
        } else if fr.block == usize::MAX {
            u64::MAX
        } else {
            self.tile.scrollback.frozen_blocks().get(fr.block)?.id
        };
        Some(Mark {
            block,
            item: fr.item,
            row: fr.row,
            obj: s.obj,
        })
    }

    /// Keep the flat row list current: new output moves the rows.
    fn refresh_flat(&mut self) {
        let sb = &self.tile.scrollback;
        if self.flat_seq != sb.seq {
            self.flat_seq = sb.seq;
            // The live grid's rows trail the transcript's (14.11.5).
            self.flat = flatten_with_grid(sb, self.tile.grid.dims().1);
            if let Some(s) = self.sel.as_mut() {
                s.clamp(self.flat.len());
            }
        }
    }

    /// A row's obj runs: the transcript's for its rows, the cell spans' for
    /// a live-grid row.
    fn runs_for(&self, fr: FlatRow) -> Vec<ObjRun> {
        if fr.block == GRID_BLOCK {
            self.tile.grid_runs(fr.item)
        } else {
            runs_on_row(&self.tile.scrollback, fr)
        }
    }

    /// Back to Insert: the prompt, re-anchored at the bottom.
    fn leave_normal(&mut self) {
        self.mode = Mode::Insert;
        self.sel = None;
        self.scroll_up = 0;
        self.dirty = true;
        #[cfg(feature = "test-mode")]
        say!("halcyond: session tile leaf={} insert mode", self.leaf);
    }

    /// The block index (usize::MAX = the open block) for a frame key.
    fn block_index(&self, key: u64) -> Option<usize> {
        if key == u64::MAX {
            Some(usize::MAX)
        } else {
            self.tile
                .scrollback
                .frozen_blocks()
                .iter()
                .position(|b| b.id == key)
        }
    }

    /// Lay out the block at `index` (the open block for usize::MAX).
    fn lay(
        &self,
        index: usize,
        sheet: &Sheet,
        gs: &mut GlyphSource,
    ) -> Option<halcyond::layout::LaidBlock> {
        let sb = &self.tile.scrollback;
        let b = if index == usize::MAX {
            sb.open_block()
        } else {
            sb.frozen_blocks().get(index)?
        };
        Some(layout_block(b, self.surf.w as i32, sheet, gs))
    }

    /// A KEY in Normal mode, or the Esc that enters it (the caller gates on
    /// the VT being in its normal screen -- a full-screen app owns Esc).
    /// Some(req) = Enter on an obj run: the verb menu to summon.
    fn normal_input(
        &mut self,
        e: &tapestry::Event,
        rules: &[Rule],
        sheet: &Sheet,
        gs: &mut GlyphSource,
    ) -> Option<MenuReq> {
        if self.mode == Mode::Insert {
            // Esc enters Normal (the Helix-modal boundary); the cursor
            // starts on the newest row.
            if e.rune == 0x1b && e.value == 1 {
                self.mode = Mode::Normal;
                self.flat_seq = u64::MAX;
                self.refresh_flat();
                // The cursor starts on the grid's cursor row -- the prompt --
                // not the screen's last (usually blank) row.
                let n = self.flat.len();
                let (crow, _, _) = self.tile.grid.cursor();
                let grid_rows = self.tile.grid.dims().1;
                let cursor = (n.saturating_sub(grid_rows) + crow).min(n.saturating_sub(1));
                self.sel = Some(Sel {
                    cursor,
                    anchor: None,
                    obj: None,
                });
                self.dirty = true;
                #[cfg(feature = "test-mode")]
                say!(
                    "halcyond: session tile leaf={} normal mode ({} rows)",
                    self.leaf,
                    self.flat.len()
                );
            }
            return None;
        }
        self.refresh_flat();
        let (_, cell_h, _) = gs.mono_cell();
        let page_rows = ((self.surf.h as i32 / cell_h) / 2).max(1);
        let act = normal_key(e.code, e.rune);
        let n = self.flat.len();
        match act {
            NormalAct::ScrollLines(k) => {
                if let Some(s) = self.sel.as_mut() {
                    s.mv(-k, n);
                }
                self.dirty = true;
            }
            NormalAct::ScrollHalfPage(k) => {
                if let Some(s) = self.sel.as_mut() {
                    s.mv(-k * page_rows, n);
                }
                self.dirty = true;
            }
            NormalAct::Top => {
                if let Some(s) = self.sel.as_mut() {
                    s.cursor = 0;
                    s.obj = None;
                }
                self.dirty = true;
            }
            NormalAct::Bottom => {
                if let Some(s) = self.sel.as_mut() {
                    s.cursor = n.saturating_sub(1);
                    s.obj = None;
                }
                self.dirty = true;
            }
            NormalAct::ToggleSelect if e.value == 1 => {
                if let Some(s) = self.sel.as_mut() {
                    s.toggle_anchor();
                }
                self.dirty = true;
            }
            NormalAct::Collapse => {
                if let Some(s) = self.sel.as_mut() {
                    s.anchor = None;
                }
                self.dirty = true;
            }
            // The yank register is the console renderer's; a tile pastes
            // nothing (`p`) and `y` only collapses the selection. A tile's
            // register lands with the pts clipboard work (KT-4).
            NormalAct::ToInsert | NormalAct::Paste => self.leave_normal(),
            NormalAct::Yank => {
                if let Some(s) = self.sel.as_mut() {
                    s.anchor = None;
                }
                self.dirty = true;
            }
            NormalAct::NextRun | NormalAct::PrevRun => {
                let fwd = act == NormalAct::NextRun;
                let stepped = self.sel.as_ref().map(|s| {
                    (
                        s.cursor,
                        step_run_with(&self.flat, s.cursor, s.obj, fwd, |fr| self.runs_for(fr)),
                    )
                });
                if let (Some(s), Some((from, stepped))) = (self.sel.as_mut(), stepped) {
                    match stepped {
                        Some((row, obj)) => {
                            s.cursor = row;
                            s.anchor = None;
                            s.obj = Some(obj);
                            #[cfg(feature = "test-mode")]
                            say!(
                                "halcyond: session tile leaf={} run -> row {} obj {}",
                                self.leaf,
                                row,
                                obj
                            );
                        }
                        None => {
                            #[cfg(feature = "test-mode")]
                            say!(
                                "halcyond: session tile leaf={} no run {} from row {}",
                                self.leaf,
                                if fwd { "forward" } else { "back" },
                                from
                            );
                        }
                    }
                }
                self.dirty = true;
            }
            NormalAct::Act if e.value == 1 => {
                // The verb menu for the selected run (the row's first when
                // none is), anchored under the run as the last frame laid it.
                let at = self
                    .sel
                    .as_ref()
                    .and_then(|s| self.flat.get(s.cursor).map(|fr| (*fr, s.obj)));
                let (fr, cur) = match at {
                    Some(v) => v,
                    None => {
                        #[cfg(feature = "test-mode")]
                        say!(
                            "halcyond: session tile leaf={} act: no cursor row",
                            self.leaf
                        );
                        return None;
                    }
                };
                let obj = cur.or_else(|| self.runs_for(fr).first().map(|r| r.obj));
                let obj = match obj {
                    Some(o) => o,
                    None => {
                        #[cfg(feature = "test-mode")]
                        say!(
                            "halcyond: session tile leaf={} act: no obj run on row {}",
                            self.leaf,
                            self.sel.as_ref().map_or(0, |s| s.cursor)
                        );
                        return None;
                    }
                };
                if let Some(s) = self.sel.as_mut() {
                    s.obj = Some(obj);
                }
                self.dirty = true;
                // Where the run sits in the last frame, and what it presents:
                // a grid row from the cells + the frame's grid origin, a
                // transcript row from its laid block.
                let placed: Option<((i32, i32, i32, i32), (String, String))> =
                    if fr.block == GRID_BLOCK {
                        // PL-4b: the live grid tail is proportional, so the
                        // run's rect comes from the cached layout
                        // (grid_run_rect), tail-relative, lifted by the GRID_KEY
                        // frame y -- the click path's twin (both summon the same
                        // GRID_KEY menu). The old mono c0*cw / row*ch anchored a
                        // KEYBOARD-summoned menu at the wrong x on the tail.
                        let (cw, ch, _) = gs.mono_cell();
                        let gy = self
                            .tile
                            .frame
                            .iter()
                            .find(|f| f.0 == GRID_KEY)
                            .map(|f| f.1);
                        let rect = self.tile.grid_run_rect(fr.item, obj, cw, ch);
                        let o = self
                            .tile
                            .grid_run_obj(fr.item, obj)
                            .map(|(t, r)| (String::from(t), String::from(r)));
                        match (gy, rect, o) {
                            (Some(gy), Some((rx, ry, rw, rh)), Some(o)) => {
                                Some(((rx, gy + ry, rw, rh), o))
                            }
                            _ => None,
                        }
                    } else {
                        let key = if fr.block == usize::MAX {
                            Some(u64::MAX)
                        } else {
                            self.tile
                                .scrollback
                                .frozen_blocks()
                                .get(fr.block)
                                .map(|b| b.id)
                        };
                        let by = key
                            .and_then(|k| self.tile.frame.iter().find(|f| f.0 == k).map(|f| f.1));
                        let rect = self
                            .lay(fr.block, sheet, gs)
                            .and_then(|l| run_rect(&l, fr.item, fr.row, obj));
                        let o = obj_of(&self.tile.scrollback, fr.block, obj)
                            .map(|(t, r)| (String::from(t), String::from(r)));
                        match (by, rect, o) {
                            (Some(by), Some((rx, ry, rw, rh)), Some(o)) => {
                                Some(((rx, by + ry, rw, rh), o))
                            }
                            _ => None,
                        }
                    };
                let ((rx, ry, rw, rh), (ty, refv)) = match placed {
                    Some(v) => v,
                    None => {
                        #[cfg(feature = "test-mode")]
                        say!(
                            "halcyond: session tile leaf={} act: obj {} unplaced",
                            self.leaf,
                            obj
                        );
                        return None;
                    }
                };
                let model = build_menu(rules, &ty, &refv);
                return Some(MenuReq {
                    model,
                    ax: rx,
                    ay: ry + rh,
                    run: (rx, ry, rw, rh),
                });
            }
            // An autorepeat of a one-shot is not another press.
            NormalAct::None | NormalAct::Act | NormalAct::ToggleSelect => {}
        }
        None
    }

    /// A left press at the pointer: the obj run under it -- as the last
    /// frame laid it -- gets its verb menu at the pointer.
    fn click(&mut self, rules: &[Rule], sheet: &Sheet, gs: &mut GlyphSource) -> Option<MenuReq> {
        let (key, by) = self.tile.hit(self.ptr.1)?;
        let (rect, (ty, refv)) = if key == GRID_KEY {
            // The live grid tail: the run under the pointer. `by` is the tail's
            // screen-y (the GRID_KEY frame entry), so the pointer is made
            // tail-relative for the hit; the run's rect comes back tail-relative
            // and is lifted by `by`. PL-4b: the tail is proportional, so
            // grid_hit / grid_run_rect invert through the cached layout (the
            // grid analogue of the scrollback path's hit_run + run_rect).
            let (cw, ch, _) = gs.mono_cell();
            let (r, k) = self.tile.grid_hit(self.ptr.0, self.ptr.1 - by, cw, ch)?;
            let (t, rf) = self.tile.grid_run_obj(r, k)?;
            let (rx, ry, rw, rh) = self.tile.grid_run_rect(r, k, cw, ch)?;
            ((rx, by + ry, rw, rh), (String::from(t), String::from(rf)))
        } else {
            let bi = self.block_index(key)?;
            let laid = self.lay(bi, sheet, gs)?;
            let (i, r, o) = hit_run(&laid, self.ptr.0, self.ptr.1 - by)?;
            let (rx, ry, rw, rh) = run_rect(&laid, i, r, o)?;
            let (t, rf) = obj_of(&self.tile.scrollback, bi, o)?;
            ((rx, by + ry, rw, rh), (String::from(t), String::from(rf)))
        };
        let model = build_menu(rules, &ty, &refv);
        Some(MenuReq {
            model,
            ax: self.ptr.0,
            ay: self.ptr.1,
            run: rect,
        })
    }

    /// Re-derive the grid from the surface at `geom`'s cell -- a CONFIGURE,
    /// or a scale change (the cell moved under the same surface): the model
    /// reshapes and the Resize goes down the wire when the grid changed. A
    /// dead tile keeps its last grid (its terminal is gone).
    fn fit_to_surface(&mut self, geom: Geom, wire_out: &mut Vec<u8>) {
        let nc = ((self.surf.w as i32 / geom.cell_w).max(1)) as u16;
        let nr = ((self.surf.h as i32 / geom.cell_h).max(1)) as u16;
        if (nc != self.cols || nr != self.rows) && self.exit.is_none() {
            self.cols = nc;
            self.rows = nr;
            self.tile.resize(nc as usize, nr as usize);
            wire_out.clear();
            encode_input(&Input::Resize { cols: nc, rows: nr }, wire_out);
            self.queue_resize(wire_out);
            // The resize's witness (test builds): a divider drag reaches a
            // tile as a CONFIGURE, and this is where it becomes the pts
            // winsize (HALCYON-INSTRUMENT 9.2, the existing path).
            #[cfg(feature = "test-mode")]
            say!(
                "halcyond: session tile leaf={} fit {}x{} px -> {} cols {} rows",
                self.leaf,
                self.surf.w,
                self.surf.h,
                nc,
                nr
            );
        }
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
        if self.mode == Mode::Normal {
            self.refresh_flat();
        }
        let mark = self.mark();
        self.tile
            .render(cart, sw, sh, gs, sheet, &mut self.scroll_up, mark);
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
                Some(c) => Ingested::Ended(c),
                None => Ingested::Disconnected,
            };
        }
        self.dec.push(&buf[..n as usize]);
        loop {
            match self.dec.next_frame() {
                Some(Ok((tag, payload))) => match parse_record(tag, &payload) {
                    Ok(rec) => {
                        let alt_enter = matches!(rec, Record::Mode(ScreenMode::AltScreen));
                        #[cfg(feature = "test-mode")]
                        match &rec {
                            Record::Mode(ScreenMode::AltScreen) => {
                                say!("halcyond: session tile leaf={} screenmode -> AltScreen", self.leaf)
                            }
                            Record::Mode(ScreenMode::Normal) => {
                                say!("halcyond: session tile leaf={} screenmode -> Normal", self.leaf)
                            }
                            _ => {}
                        }
                        self.tile.apply(rec);
                        // A program entering the alt screen takes every key
                        // (the modal gate keys on the tile's screen mode), so
                        // the transcript's Normal mode could only linger to
                        // resume, with a stale selection, when the app leaves
                        // -- leave it now (the H-arc round-1 audit, B-F5).
                        if alt_enter && self.mode == Mode::Normal {
                            self.leave_normal();
                        }
                        self.dirty = true;
                    }
                    // A malformed record from the untrusted parser: desync.
                    Err(_) => return Ingested::Crashed,
                },
                // An oversize frame: unrecoverable stream desync.
                Some(Err(_)) => return Ingested::Crashed,
                None => break,
            }
        }
        // Test builds: the first time this tile's transcript holds a Beacon
        // object, say so -- the witness that a tagged program's rich frames
        // reached the renderer's model (the welcome's tour, a menu-run `ls`).
        #[cfg(feature = "test-mode")]
        if !self.objs_said {
            let sb = &self.tile.scrollback;
            let n = sb.open_block().objs.len()
                + sb.frozen_blocks()
                    .iter()
                    .map(|b| b.objs.len())
                    .sum::<usize>();
            if n > 0 {
                self.objs_said = true;
                say!(
                    "halcyond: session tile leaf={} presents objs ({})",
                    self.leaf,
                    n
                );
            }
        }
        // An exit arrived interleaved in the record stream.
        match self.tile.exited() {
            Some(c) => Ingested::Ended(c),
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

    /// HALCYON-INSTRUMENT 9.4 (I-7): tell the tile's pts host the new
    /// palette; never dropped, ahead of keys (like the geometry record).
    fn queue_palette(&mut self, bytes: &[u8]) {
        self.down.push_palette(bytes);
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

    /// The terminal is hung up and reaped; the surface and the pipe ends
    /// drop (Surface::drop says `destroy`).
    fn teardown(self) {
        let SessionTile {
            mut child,
            _down,
            leaf,
            ..
        } = self;
        drop(_down);
        end_terminal(&mut child, leaf);
    }
}

/// The teardown's grace: on the down channel's EOF the kaua-term hangs its
/// program up and exits by itself once it has REAPED it (the program's
/// zombie is its parent's to collect, never joey's -- an orphaned zombie
/// keeps its namespace, the user's home mount, until init reaps it, and init
/// cannot while login waits on the session: the logout stall measured at
/// I-4, the r1 B-F4 finding). Past the grace the kill is the fallback, for a
/// terminal that does not come down (a program whose children hold the pts).
const TEARDOWN_GRACE_MS: u64 = 2_000;
const TEARDOWN_POLL_MS: u64 = 25;

/// Hang up: `down` is already closed by the caller. Wait for the terminal
/// to exit within the grace, else kill it; reaped either way.
fn end_terminal(child: &mut Child, leaf: u32) {
    let mut waited = 0u64;
    loop {
        match child.try_wait() {
            Ok(Some(_)) | Err(_) => {
                #[cfg(feature = "test-mode")]
                say!("halcyond: tile {} hung up ({} ms)", leaf, waited);
                return;
            }
            Ok(None) => {}
        }
        if waited >= TEARDOWN_GRACE_MS {
            break;
        }
        let _ = sleep(Duration::from_millis(TEARDOWN_POLL_MS));
        waited += TEARDOWN_POLL_MS;
    }
    let _ = child.kill();
    let _ = child.wait();
    say!("halcyond: tile {} killed after the hangup grace", leaf);
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
    // The session's resolved terminal palette: a tile created LATER must be
    // born in the same theme as the ones already up.
    palette: vt::Palette,
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
        // HALCYON-WORKSPACES W-3: `plan.drop` is a CANDIDATE list -- it means
        // only "absent from the rows we were given", and since W-1a those rows
        // are the ACTIVE root's. Absence therefore means "not on screen", never
        // "gone". The global `pane/` tree is the existence oracle: W-1a kept
        // `live_ids` workspace-spanning on purpose and the readdir enumerates
        // it, so a DORMANT tile still walks and a closed one does not.
        //
        // Without this probe a workspace switch dropped EVERY tile, tore each
        // one down, and then broke the session loop on `tiles.is_empty()` --
        // a full logout on a single keystroke. That is the same class as the
        // two defects W-1a found by reading: invisible until a second root
        // exists.
        if read_file(troot, &format!("pane/{}/geometry", leaf)).is_some() {
            continue;
        }
        // Round 1 F5: `read_file` answers None for a failed OPEN, a failed
        // READ and non-UTF-8 alike, so "it is gone" and "I could not ask"
        // were the same answer. A transient failure (fid budget, a wedged
        // conn, an interrupted read) during the reconcile that follows a
        // switch would drop EVERY tile, latch each id into `closed`
        // PERMANENTLY (ids are never reused), and then break the session
        // loop on `tiles.is_empty()` -- the same full logout W-3 fixed,
        // reached from an error instead of a keystroke.
        //
        // A negative needs a POSITIVE CONTROL one variable away: `layout` is
        // served by the same conn and always exists, so if it cannot be read
        // either, this is "could not ask" and nothing is torn down this pass.
        // A tile wrongly kept costs one reconcile; TEV_CLOSE remains the
        // authoritative teardown signal either way.
        if read_file(troot, "layout").is_none() {
            continue;
        }
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
        // H-4d: the leaf's tag is the tile's command line (a restore tool
        // names the leaves it builds and leaves them to us); empty = the
        // shell. Read AFTER the mint: the mint is the ownership proof, and a
        // tag written before the tool's conn went is what the mint's
        // reservation guaranteed we did not race.
        let tag = read_file(troot, &format!("pane/{}/tag", leaf)).unwrap_or_default();
        let argv = tile_command(tag.trim_end_matches('\n'), home, |p| fs::exists(p));
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
        match SessionTile::spawn(leaf, surf, geom, &argv, budget, palette) {
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

/// HALCYON-THEME 3.4: push our RESOLVED theme to the compositor, so the
/// chrome it paints and the content we paint are the same theme.
///
/// Only a DECLARED session may: the verb is seat-gated exactly like `scale`.
/// And only a push can carry it -- the user's theme file lives in the user's
/// home, which tapestryd is not entitled to read, so the compositor would
/// otherwise be stuck on the system file while the pane inside it was
/// something else.
///
/// A refusal is SAID, never silently absorbed: the visible result would be a
/// pane in one theme inside chrome in another, and the operator deserves to
/// know which half failed.
/// `pub(crate)` so the CONSOLE renderer uses this one too. It had its own
/// inline version with no `Busy` arm at all, so a push landing in a service
/// pass whose four-verb layout budget was already spent got exactly one
/// attempt and gave up for the life of the boot -- and the console's push is
/// the LOAD-BEARING one, since tapestryd comes up before the pool it would
/// read the theme file from is mounted. One implementation, both renderers.
pub(crate) fn push_theme(ring: &EventRing, bundle: &libhalcyon::instrument::Bundle) -> bool {
    let cmd = format!("theme {}", libhalcyon::theme::to_wire(bundle));
    for _ in 0..VERB_RETRIES {
        match ring.global_ctl(&cmd) {
            Ok(()) => {
                say!("halcyond: theme pushed to the compositor");
                return true;
            }
            Err(TapError::Busy) => {
                let _ = sleep(Duration::from_millis(VERB_NAP_MS));
            }
            Err(e) => {
                say!(
                    "halcyond: theme push refused ({:?}) -- the chrome keeps its own",
                    e
                );
                return false;
            }
        }
    }
    say!("halcyond: theme push kept busy -- the chrome keeps its own");
    false
}

/// HALCYON-INSTRUMENT 9.4 (I-7): the picker's registry -- every
/// `/lib/halcyon/themes/*.toml` that loads as an Instrument theme, as a
/// `PickerTheme` (id, name, tagline, group, rank; the four miniature colours
/// = the theme's own desktop / open / structure / amber). A file that fails
/// to load, or is a legacy-schema theme, is skipped. Read at every open (13
/// small files), so a theme dropped in appears without a restart.
pub(crate) fn read_gallery() -> Vec<halcyond::picker::PickerTheme> {
    let mut out = Vec::new();
    let rd = match fs::read_dir(instrument::GALLERY_DIR) {
        Ok(rd) => rd,
        Err(_) => return out,
    };
    for ent in rd.flatten() {
        let name = ent.file_name();
        // The preview set must equal the committable set: require the stem to be
        // a gallery id (gallery_bundle re-validates the same at commit), which
        // also holds the dirent name to a single path component -- a hostile 9P
        // server bound over the gallery dir cannot inject a "../" name here
        // (defense in depth; halcyond has only the user's own authority).
        let Some(stem) = name.strip_suffix(".toml") else {
            continue;
        };
        if !instrument::is_gallery_id(stem) {
            continue;
        }
        let mut path = String::from(instrument::GALLERY_DIR);
        path.push('/');
        path.push_str(name);
        let Some(text) = read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, &path) else {
            continue;
        };
        if let Ok(instrument::LoadedAny::Instrument(l)) = theme::load(&text) {
            out.push(halcyond::picker::PickerTheme {
                id: l.id,
                name: l.name,
                tagline: l.tagline,
                group: l.group,
                rank: l.rank,
                pv_bg: l.theme.desktop,
                pv_pane: l.theme.open,
                pv_rule: l.theme.structure,
                pv_signal: l.theme.amber,
            });
        }
    }
    out
}

/// The resolved bundle for a gallery id under `profile` (9.4's commit): read
/// the gallery file, load it, project to the profile. `(bundle, name)`, or
/// None (the id names no loadable gallery file).
pub(crate) fn gallery_bundle(id: &str, profile: instrument::Profile) -> Option<(instrument::Bundle, String)> {
    let path = instrument::gallery_path(id)?;
    let text = read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, &path)?;
    let any = theme::load(&text).ok()?;
    let name = String::from(any.name());
    Some((any.bundle(profile), name))
}

/// Durable-write the picker's word to `$HOME/lib/halcyon/theme` (9.4): mkdir
/// -p the two components, then tmp + fsync + rename + fsync on the SAME
/// OWRITE fd (the aurora-config idiom). False on any failure -- the theme is
/// in force either way; only its persistence failed.
fn write_user_pick(home: Option<&str>, id: &str) -> bool {
    let Some(home) = home else { return false };
    let base = alloc::format!("{}/lib", home.trim_end_matches('/'));
    let dir = alloc::format!("{}/halcyon", base);
    for d in [&base, &dir] {
        match fs::create_dir(d) {
            Ok(()) => {}
            Err(e) if e == libthyla_rs::err::Error::Exists => {}
            Err(_) => return false,
        }
    }
    let path = alloc::format!("{}/theme", dir);
    let tmp = alloc::format!("{}.tmp", path);
    let mut f = match File::create(&tmp) {
        Ok(f) => f,
        Err(_) => return false,
    };
    if f.write_all(id.as_bytes()).is_err() {
        return false;
    }
    let _ = unsafe { libthyla_rs::t_fsync(f.as_raw_fd() as i64, 0) };
    if fs::rename(&tmp, &path).is_err() {
        return false;
    }
    let ok = unsafe { libthyla_rs::t_fsync(f.as_raw_fd() as i64, 0) == 0 };
    drop(f);
    ok
}

/// HALCYON-SCALE 6: the user's `/env/HALCYON_SCALE` preference, written
/// once as the gated `scale <pct>` verb (retried through the per-pass verb
/// budget). Absent: nothing; unparsable or off the five values: said and
/// ignored; refused (the seat is not ours): said -- the compositor's own
/// scale stands either way.
fn request_env_scale(ring: &EventRing) {
    let Some(text) = read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, HALCYON_SCALE_ENV_PATH) else {
        return;
    };
    let want = text.trim();
    let pct = match want.parse::<u16>() {
        Ok(p) if scale::is_valid_pct(p) => p,
        _ => {
            say!(
                "halcyond: {} ignored: {:?} is not one of 100/125/150/175/200",
                HALCYON_SCALE_ENV_PATH,
                want
            );
            return;
        }
    };
    let cmd = format!("scale {}", pct);
    for _ in 0..VERB_RETRIES {
        match ring.global_ctl(&cmd) {
            Ok(()) => {
                say!("halcyond: scale {} requested ({})", pct, HALCYON_SCALE_ENV_PATH);
                return;
            }
            Err(TapError::Busy) => {
                let _ = sleep(Duration::from_millis(VERB_NAP_MS));
            }
            Err(e) => {
                say!("halcyond: scale {} refused {:?} ({})", pct, e, HALCYON_SCALE_ENV_PATH);
                return;
            }
        }
    }
    say!("halcyond: scale {} not admitted (budget) ({})", pct, HALCYON_SCALE_ENV_PATH);
}

/// The motion preference's WORD, said once with the posture it resolves to.
///
/// The word is kept rather than the verdict because `motion::admitted` folds
/// it with a live clock sample, and the clock is the second condition:
/// `monotonic_ns` is fail-soft 0, so a session that cannot read it degrades
/// to 9.5's static caret -- a mode scripture already specifies -- instead of
/// to a caret frozen mid-step, which would read as a hung compositor.
fn env_motion_word() -> Option<String> {
    let word = read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, HALCYON_MOTION_ENV_PATH);
    let now = libthyla_rs::time::monotonic_ns();
    // The STATED preference outranks the clock in this message, though both
    // turn motion off: a user who wrote `0` and is then told the clock is
    // unreadable would go hunting a fault they caused on purpose.
    let why = match (word.as_deref().map(str::trim), now) {
        (Some("0"), _) => "=0",
        (_, 0) => "monotonic clock unreadable",
        (None, _) => "absent: on by default since I-8",
        (Some(w), _) if w.is_empty() => "empty: not the opt-out word",
        _ => "set",
    };
    say!(
        "halcyond: motion {} ({} {})",
        if libhalcyon::motion::admitted(word.as_deref(), now) { "on" } else { "off" },
        HALCYON_MOTION_ENV_PATH,
        why
    );
    word
}

/// HALCYON-SCALE 6: the compositor's scale changed (its ctl says a new
/// percent): rebuild the render brain at it -- the Sheet (a new generation,
/// so every layout cache re-lays), the glyph source (the mono bakes for the
/// scale; the atlas regens), the cell geometry, and every tile: its cached
/// heights dropped, its grid re-fitted to its surface at the new cell (a
/// Resize down the wire when it changed), a repaint. The chrome, the status
/// bar and the menu are the caller's (they live beside the tiles).
fn rescale(
    pct: u16,
    display_w: u32,
    sheet: &mut Sheet,
    gs: &mut GlyphSource,
    geom: &mut Geom,
    tiles: &mut BTreeMap<u32, SessionTile>,
    wire_out: &mut Vec<u8>,
) {
    let from = sheet.scale;
    let gen = sheet.gen + 1;
    *sheet = sheet_for(&sheet.bundle(), pct, display_w);
    sheet.gen = gen;
    gs.set_scale(pct);
    gs.set_smooth(sheet.smooth_mem);
    gs.set_kerning(sheet.kerning);
    let (cw, ch, _) = gs.mono_cell();
    geom.cell_w = cw;
    geom.cell_h = ch;
    for t in tiles.values_mut() {
        t.tile.invalidate_heights();
        t.fit_to_surface(*geom, wire_out);
        t.dirty = true;
    }
    say!("halcyond: scale {} -> {} (cell {}x{})", from, pct, cw, ch);
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
    if gs.face_count() != halcyond::raster::VENDORED_FACES {
        say!("halcyond: FAIL vendored face parse");
        return 1;
    }
    // HALCYON-SCALE 6: the user's preference first -- written as the gated
    // verb only once declared and hosting (the seat-held-while-hosting rule
    // admits it; undeclared it would be refused) -- then the brain at the
    // COMPOSITOR's scale, read off its ctl with the display (re-read on
    // every relayout below). The atlas bound follows the display area.
    if declared {
        request_env_scale(&ring);
    }
    // The motion preference needs no seat and no verb -- it is halcyond's own
    // paint decision -- so it is read whether or not the declare took.
    let motion_word = env_motion_word();
    // A scale the table does not know is kept out of the sheet (r2 B-F11:
    // the /env path validated, the compositor's did not); said once per value.
    let mut scale_refused: Option<u16> = None;
    let mut display = ring.display_info().unwrap_or(DisplayInfo {
        w: root_surf.w,
        h: root_surf.h,
        scale: 100,
    });
    gs.set_scale(display.scale);
    gs.set_display(display.w, display.h);
    // THE ONE PLACE the session resolves its theme (HALCYON-THEME 3.2/3.4):
    // the system file, then the user's own, with the built-in as the floor
    // nothing can remove. Every note is SAID -- a refused theme file that
    // fell back quietly is indistinguishable from one that applied and
    // happened to look the same (4.2).
    // Since I-1 the resolution is a BUNDLE (HALCYON-INSTRUMENT 4.1): the
    // profile word (user, then system), then the theme -- the picker's
    // gallery choice, the user's file, the system's -- in either schema, the
    // other side projected. The pick is a WORD; it becomes a path only once
    // `gallery_path` has accepted it as an id (never `../`, never a slash).
    let under_home = |rel: &str| {
        home.as_deref()
            .map(|h| alloc::format!("{}{}", h.trim_end_matches('/'), rel))
            .and_then(|p| read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, &p))
    };
    let system_profile = read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, instrument::SYSTEM_PROFILE_PATH);
    let user_profile = under_home(instrument::USER_PROFILE_REL);
    let user_pick = under_home(instrument::USER_PICK_REL);
    let pick_file = user_pick
        .as_deref()
        .and_then(instrument::pick_id)
        .and_then(instrument::gallery_path)
        .and_then(|p| read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, &p));
    let system_file = read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, theme::SYSTEM_THEME_PATH);
    let user_file = under_home(theme::USER_THEME_REL);
    let resolved = instrument::resolve_bundle(instrument::Sources {
        system_profile: system_profile.as_deref(),
        user_profile: user_profile.as_deref(),
        user_pick: user_pick.as_deref(),
        pick_file: pick_file.as_deref(),
        user_file: user_file.as_deref(),
        system_file: system_file.as_deref(),
    });
    for n in &resolved.notes {
        say!("halcyond: {}", n);
    }
    say!(
        "halcyond: theme {} ({:?}, {:?}, {} inherited); profile {} ({:?})",
        if resolved.name.is_empty() {
            "built-in"
        } else {
            &resolved.name
        },
        resolved.theme_tier,
        resolved.schema,
        resolved.inherited.len(),
        resolved.bundle.profile.word(),
        resolved.profile_tier
    );
    // HALCYON-INSTRUMENT 8.1: the theme's name, the rail's theme control.
    let mut current_theme_id = resolved.id.clone();
    let mut theme_name = if resolved.name.is_empty() {
        String::from("built-in")
    } else {
        resolved.name.clone()
    };
    let bundle = resolved.bundle;
    let theme = bundle.theme;
    // The compositor paints the chrome around our panes and cannot read the
    // user's file; a declared seat is the only party that can tell it.
    if declared {
        let _ = push_theme(&ring, &bundle);
    }
    let mut sheet = sheet_for(&bundle, display.scale, display.w);
    gs.set_smooth(sheet.smooth_mem);
    gs.set_kerning(sheet.kerning);
    let (cell_w, cell_h, _) = gs.mono_cell();
    let (disp_w, disp_h) = (root_surf.w, root_surf.h);
    let mut geom = Geom {
        cell_w,
        cell_h,
        disp_w,
        disp_h,
    };
    say!(
        "halcyond: scale {} (cell {}x{}, atlas bound {} pages)",
        display.scale,
        cell_w,
        cell_h,
        gs.evict_pages()
    );

    // The root tile, keyed on the leaf that hosts the bootstrap surface.
    let root_leaf = match leaf_hosting(troot, root_surf.id) {
        Some(l) => l,
        None => {
            say!("halcyond: FAIL locate session root leaf");
            return 1;
        }
    };
    // H-4d: mark the session for the tools its tiles run (a restore defers
    // hosting to this compositor while the mark is set). Best-effort: without
    // it a restore spawns its tags itself, the console-path behaviour.
    if File::create(SESSION_ENV_PATH)
        .and_then(|mut f| f.write_all(b"on"))
        .is_err()
    {
        say!("halcyond: could not mark {}", SESSION_ENV_PATH);
    }
    // s7a-3: publish the session palette so a tile's programs (nora, ut)
    // follow the session theme. Written BEFORE the first tile spawn, so every
    // descendant inherits it via /env; best-effort, an unset value just leaves
    // the program on its own default.
    // DERIVED from the resolved bundle (3.5), not a second hand-kept list:
    // one direction, file -> Bundle -> env, never back. Keyed on the PROFILE:
    // under Instrument the export also carries the prompt roles and the
    // class-named syntax roles (HALCYON-INSTRUMENT 7.4), under legacy the
    // eleven it always did.
    let palette = env_palette(&bundle);
    match File::create(HALCYON_PALETTE_ENV_PATH).and_then(|mut f| f.write_all(palette.as_bytes())) {
        Ok(()) => say!(
            "halcyond: palette published ({} bytes) to {}",
            palette.len(),
            HALCYON_PALETTE_ENV_PATH
        ),
        Err(_) => say!("halcyond: could not write {}", HALCYON_PALETTE_ENV_PATH),
    }
    let mut tiles: BTreeMap<u32, SessionTile> = BTreeMap::new();
    let mut closed: BTreeSet<u32> = BTreeSet::new();
    let shell = tile_command("", home.as_deref(), |p| fs::exists(p));
    match SessionTile::spawn(
        root_leaf,
        root_surf,
        geom,
        &shell,
        SESSION_SCROLLBACK_BUDGET,
        theme.terminal,
    ) {
        Some(t) => {
            tiles.insert(root_leaf, t);
        }
        None => {
            say!("halcyond: FAIL session root tile spawn");
            return 1;
        }
    }

    // H-4d: the verb table (BEACON.md 7, the system tier, read once) + the
    // one menu on this session's ring (the H-3c-2 event set: its keys wake
    // the unified poll like a tile's), and the tile it was opened over.
    let rules: Vec<Rule> = match read_file(libthyla_rs::T_WALK_OPEN_FROM_ROOT, "/lib/beacon/verbs")
    {
        Some(text) => parse_verbs(&text, cfg!(feature = "test-mode")),
        None => Vec::new(),
    };
    say!("halcyond: {} verb rules loaded", rules.len());
    let mut menus = menuset::MenuSet::new(ring.clone());
    let mut menu_leaf: Option<u32> = None;
    // HALCYON-INSTRUMENT I-3: header actions awaiting the pass that acts on
    // them (the pump's, and the tile menu's choices), and a Restart request.
    let mut tile_actions: Vec<ChromeAction> = Vec::new();
    let mut restart_req: Option<u32> = None;
    // HALCYON-INSTRUMENT 14.5 (I-7): the tile a running-close confirmation is
    // asking about ((id, count)); the plan runs on the dialog's `close` tag.
    // (pane, count, protected): `protected` is 6.5's final-tile rule, which
    // the header's x carries and the Super+Q chord deliberately does not.
    let mut pending_close: Option<(u32, u32, bool)> = None;
    let inst_profile = bundle.profile == instrument::Profile::Instrument;
    // H-3b/H-3d: the per-leaf tag bars + the one display status bar, on the SAME
    // session ring (the H-3c-2 event set: their CONFIGUREs wake the unified
    // poll). The session compositor wires the Daylight chrome the single-tile
    // console path (main.rs) already drives; tapestryd carves each leaf's tagbar
    // rect and the session tags its own leaves, so the chrome only needs minting
    // + painting here.
    let mut chrome = chromeset::ChromeSet::new(ring.clone());
    let mut status = statusset::StatusBar::new(ring.clone());
    // HALCYON-INSTRUMENT 8: the top rail (a Role::Rail surface on the same
    // ring, under Instrument only) and the footer's chord hints from the
    // compositor's `chords` file, re-read with every relayout.
    let mut rail = railset::RailBar::new(ring.clone());
    let mut hints: Vec<(String, String)> = Vec::new();
    // The tile-status feed's one-shot refusal notice (the H-3b round F4
    // posture: a refusal drops that exit, the next exit mark retries).
    let mut status_refusal_said = false;

    let mut cart = cartoon::Cartoon::new();
    let mut inbuf = [0u8; INGEST_BUF];
    let mut wire_out: Vec<u8> = Vec::new();
    let mut relayout = true;
    // A layout change moves tag bars too; a chrome-surface CONFIGURE (the
    // focus move the compositor sends only to the chrome) sets it independently.
    let mut chrome_dirty = true;
    let mut up_announced = false;
    let mut ingest_announced = false;
    // The blink's POST-marker, in the same shape as the two above: `motion
    // on` at startup says what was RESOLVED, this says a step actually
    // reached a tile. Only the second one can tell a live blink from a lever
    // that parsed and then animated nothing -- the whole chain (clock ->
    // phase -> `paints_caret` -> dirty) has to have run for it to print.
    let mut caret_announced = false;
    let mut present_fails: u32 = 0;
    let mut logout: Option<i32> = None;
    // The session init child (H-4c: rio's `-i` idiom), spawned once after the
    // first present; reaped at the loop top; killed at logout.
    let mut init: Option<Child> = None;
    let mut init_spawned = false;

    loop {
        // The atlas bound, between frames: every tile re-lays its visible
        // blocks each pass and the chrome/status/menu surfaces look their
        // glyphs up afresh on repaint, so an eviction here costs one frame's
        // re-pack and nothing holds a stale id across it.
        gs.evict_if_full();

        // (0) Reap the session init child when it exits (the bounded poll
        // below keeps this reachable while it runs).
        if let Some(c) = init.as_mut() {
            if let Ok(Some(st)) = c.try_wait() {
                say!(
                    "halcyond: session init exited (code {})",
                    st.code().unwrap_or(-1)
                );
                init = None;
            }
        }

        // (0b) H-4d: the menu. A choice closes it from this side and types
        // the expanded command into the tile it was opened over (the tag
        // line's "executes typed text" -- the gesture is the choice); the
        // compositor's own dismiss (Esc / click-away / a chord) arrives as a
        // closed stream. `^E ^U` first (SA-8): ut's line editor takes them as
        // CursorEnd + KillToStart, so a half-typed draft moves to the kill
        // buffer instead of being run INTO. One Text record: the down-queue
        // drops it whole or not at all, never half a command.
        match menus.service(&sheet, &mut gs) {
            MenuEvent::Chosen(Action::Command(cmd)) => {
                menus.close();
                say!("halcyond: menu ran: {}", cmd);
                if let Some(t) = menu_leaf.take().and_then(|l| tiles.get_mut(&l)) {
                    t.leave_normal();
                    let mut bytes: Vec<u8> = b"\x05\x15".to_vec();
                    bytes.extend_from_slice(cmd.as_bytes());
                    bytes.push(b'\n');
                    wire_out.clear();
                    encode_input(&Input::Text(bytes), &mut wire_out);
                    t.queue_key(&wire_out);
                }
            }
            MenuEvent::Chosen(Action::Internal(act)) => {
                menus.close();
                menu_leaf = None;
                // HALCYON-INSTRUMENT 14.9: the tile verb menu's items are
                // `tile <verb> <id>`, interpreted here under the session's
                // own authority -- never a shell command.
                if let Some(n) = act.strip_prefix("workspace ") {
                    // 14.1: the workspace list's choice, acted on under this
                    // session's own authority -- the compositor's `workspace`
                    // verb on the layout file (W-2b). This used to only SAY
                    // the choice, which read as working precisely because the
                    // list it came from was itself hardcoded to one row.
                    let _ = layout_verb(troot, &format!("workspace {}", n.trim()));
                    continue;
                }
                match tile_verb(&act) {
                    Some(("close", id)) => tile_actions.push(ChromeAction::Close {
                        id,
                        count: tile_count(troot, id),
                    }),
                    Some(("restart", id)) => restart_req = Some(id),
                    Some((verb, id)) => {
                        say!("halcyond: tile {} {} is not available yet", verb, id)
                    }
                    None => say!("halcyond: internal action {} ignored (session)", act),
                }
            }
            MenuEvent::ThemeChosen(id) => {
                // HALCYON-INSTRUMENT 9.4 (I-7): the live theme transaction.
                menus.close();
                menu_leaf = None;
                match gallery_bundle(&id, sheet.bundle().profile) {
                    Some((newb, name)) if push_theme(&ring, &newb) => {
                        let old_term = sheet.theme.terminal;
                        // The push was ACCEPTED by the compositor (9.4 (b)):
                        // only now does the seat move, so the chrome and the
                        // panes can never disagree. A refused push (the arm
                        // below) keeps the previous bundle whole.
                        // Rebuild the render brain at the new theme (a new
                        // generation: every cached layout re-lays in the new
                        // colours); the geometry does not move.
                        let gen = sheet.gen + 1;
                        sheet = sheet_for(&newb, sheet.scale, sheet.display_w);
                        sheet.gen = gen;
                        gs.set_smooth(sheet.smooth_mem);
                        gs.set_kerning(sheet.kerning);
                        let new_term = sheet.theme.terminal;
                        // Re-theme every tile's retained history in place and
                        // tell its pts host the new palette (its own re-emit
                        // overwrites the live grid).
                        for t in tiles.values_mut() {
                            t.tile.set_palette(old_term, new_term);
                            wire_out.clear();
                            encode_input(&Input::Palette(new_term), &mut wire_out);
                            t.queue_palette(&wire_out);
                            t.dirty = true;
                        }
                        // Re-publish /env for FUTURE spawns (a running
                        // program is not reached -- 9.4 / 13).
                        let palette = env_palette(&newb);
                        let _ = File::create(HALCYON_PALETTE_ENV_PATH)
                            .and_then(|mut f| f.write_all(palette.as_bytes()));
                        chrome.invalidate();
                        status.invalidate();
                        rail.invalidate();
                        chrome_dirty = true;
                        current_theme_id = id.clone();
                        theme_name = if name.is_empty() { String::from("built-in") } else { name };
                        say!("halcyond: theme {} applied", id);
                        // Only the session seat persists the pick (9.4).
                        if write_user_pick(home.as_deref(), &id) {
                            say!("halcyond: theme {} written", id);
                            status.notify(&format!("THEME \u{b7} {}", theme_name.to_uppercase()), false);
                        } else {
                            say!("halcyond: theme {} not written (no home or write failed)", id);
                            status.notify(&format!("THEME \u{b7} {} (NOT SAVED)", theme_name.to_uppercase()), true);
                        }
                    }
                    Some(_) => {
                        // gallery_bundle succeeded but the compositor refused
                        // the push (E_PERM final, or the busy cadence spent):
                        // the previous bundle, check and colours stand (9.4 (b)).
                        say!("halcyond: theme {} refused by the compositor -- keeping the current", id);
                        status.notify("THEME REFUSED", true);
                    }
                    None => {
                        say!("halcyond: theme {} refused (no gallery file)", id);
                        status.notify("THEME REFUSED", true);
                    }
                }
            }
            MenuEvent::Dialog(tag) => {
                menus.close();
                menu_leaf = None;
                match tag.as_str() {
                    "reset" => {
                        let plan = read_file(troot, "layout").map(|l| reset_plan(&l)).unwrap_or_default();
                        say!("halcyond: reset: {} verb(s)", plan.len());
                        let planned = plan.len();
                        let mut landed = 0usize;
                        let mut budget = chromeset::VerbBudget::pass();
                        for (id, verb) in plan {
                            let (word, args) = verb.split_once(' ').unwrap_or((verb.as_str(), ""));
                            let cmd = if args.is_empty() {
                                format!("{} {}", word, id)
                            } else {
                                format!("{} {} {}", word, id, args)
                            };
                            if layout_verb_in(troot, &cmd, &mut budget) {
                                relayout = true;
                                landed += 1;
                            }
                        }
                        if planned == 0 || landed == planned {
                            status.notify("LAYOUT RESET", false);
                        } else if landed > 0 {
                            status.notify("LAYOUT RESET (PARTIAL)", true);
                        } else {
                            status.notify("RESET REFUSED", true);
                        }
                    }
                    "close" => {
                        if let Some((id, _, protected)) = pending_close.take() {
                            // Re-derive the count at resolution, not the snapshot
                            // taken when the dialog opened: the final-tile
                            // protection must hold against the CURRENT tree.
                            // Super+Q carries no such protection (6.5): it is
                            // the structural act, and the only reading under
                            // which a pane holding a RETAINED tile can be
                            // removed at all.
                            if protected && tile_count(troot, id) <= 1 {
                                status.notify("FINAL TILE IS PROTECTED", true);
                            } else {
                                layout_verb(troot, &format!("close {}", id));
                                relayout = true;
                            }
                        }
                    }
                    _ => {
                        // Cancel (or an unknown tag): nothing.
                        pending_close = None;
                    }
                }
            }
            MenuEvent::HelpClosed => {
                // The reference's own x (or Enter / Space): this side
                // dismisses, and the tile it covered repaints.
                menus.close();
                say!("halcyond: help closed");
                if let Some(t) = menu_leaf.take().and_then(|l| tiles.get_mut(&l)) {
                    t.dirty = true;
                }
            }
            MenuEvent::Closed => {
                // A dismissed dialog is a Cancel; drop any pending close.
                pending_close = None;
                if let Some(t) = menu_leaf.take().and_then(|l| tiles.get_mut(&l)) {
                    t.dirty = true;
                }
            }
            MenuEvent::None => {}
        }

        // (0e) HALCYON-INSTRUMENT section 10's caret: `steps(2, start)` over
        // 1100 ms with opacity 0 at 55 %. Resolved ONCE per pass from the
        // monotonic clock -- the phase is free-running, as a CSS animation
        // with no restart trigger is, so no caret carries an origin -- and
        // PUSHED into the tiles, because a tick that marks nothing paints
        // nothing: `render_if_dirty` returns early unless the tile is dirty,
        // and the poll wake below would otherwise be a wake for no reason.
        //
        // `inst_profile`, not the sheet's, is the profile word here on
        // purpose: it is what decides whether a dead child's tile is RETAINED
        // (the two arms below), and `paints_caret`'s 14.6 conjunct reads the
        // `fate` that retention sets. A caret that judged itself by a
        // different word than the retention did could suppress on a tile the
        // session never retained.
        let now_ns = libthyla_rs::time::monotonic_ns();
        let motion = libhalcyon::motion::admitted(motion_word.as_deref(), now_ns);
        // Under reduced motion the caret is STATIC, which is painted, not
        // absent (9.5) -- so the step's resting value is up, not down.
        let caret_on = !motion || libhalcyon::motion::caret_visible(now_ns / 1_000_000);
        for t in tiles.values_mut() {
            if t.tile.set_caret_on(caret_on, inst_profile) {
                t.dirty = true;
                if !caret_announced {
                    caret_announced = true;
                    say!(
                        "halcyond: session caret blink live (leaf {} -> {})",
                        t.leaf,
                        if caret_on { "on" } else { "off" }
                    );
                }
            }
        }

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
                if up_announced && !init_spawned {
                    init_spawned = true;
                    init = spawn_session_init(home.as_deref());
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
        let mut menu_req: Option<(u32, MenuReq)> = None;
        for (&leaf, t) in tiles.iter_mut() {
            loop {
                match t.surf.poll_event() {
                    Ok(Some(e)) => match e.kind {
                        TEV_CLOSE => reap.push(leaf),
                        TEV_CONFIGURE => match t.surf.handle_configure(&e) {
                            Ok(_) => {
                                t.fit_to_surface(geom, &mut wire_out);
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
                            // H-4d: on the VT's normal screen, Esc enters the
                            // transcript's Normal mode and Normal keeps every
                            // key (the Helix-modal boundary, HALCYON.md 4); a
                            // full-screen app (the alt screen) owns Esc.
                            let modal = t.tile.mode == ScreenMode::Normal
                                && e.value >= 1
                                && (t.mode == Mode::Normal || e.rune == 0x1b);
                            if modal {
                                if let Some(req) = t.normal_input(&e, &rules, &sheet, &mut gs) {
                                    menu_req = Some((leaf, req));
                                }
                            } else if let Some(kev) = map_key(e.code, e.rune, e.value) {
                                wire_out.clear();
                                encode_input(&Input::Key(kev), &mut wire_out);
                                t.queue_key(&wire_out);
                            }
                        }
                        TEV_PTR_MOVE => {
                            t.ptr = (
                                (e.value >> 16) as u16 as i32,
                                (e.value & 0xffff) as u16 as i32,
                            );
                        }
                        // H-4d click-a-path (HALCYON.md 5/6): a left press on
                        // an obj run's glyphs opens its verb menu at the
                        // pointer (the compositor focused the tile on the
                        // press; the menu grabs input while placed).
                        TEV_PTR_BTN if t.exit.is_none() => {
                            if e.code == BTN_LEFT && e.value == 1 {
                                if let Some(req) = t.click(&rules, &sheet, &mut gs) {
                                    menu_req = Some((leaf, req));
                                }
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
        // (2b) H-4d: summon the requested menu at display coordinates -- the
        // tile's content origin (its pane's `geometry`) plus the surface
        // point; the run's display rect rides the say line for the witnesses.
        if let Some((leaf, req)) = menu_req.take() {
            let (gx, gy) = read_file(troot, &format!("pane/{}/geometry", leaf))
                .and_then(|s| parse_rect(&s))
                .map(|r| (r.0 as i32, r.1 as i32))
                .unwrap_or((0, 0));
            let d = |v: i32, o: i32| (v + o).max(0) as u32;
            let run_d = (
                d(req.run.0, gx),
                d(req.run.1, gy),
                req.run.2.max(0) as u32,
                req.run.3.max(0) as u32,
            );
            if menus.open(req.model, d(req.ax, gx), d(req.ay, gy), run_d, &sheet, &mut gs) {
                menu_leaf = Some(leaf);
            }
        }

        // (3) Reconcile if a relayout happened (a split added a leaf; a close
        // removed one). New tiles come up dirty; the loop re-renders below.
        if relayout {
            relayout = false;
            reconcile(
                &ring,
                troot,
                &mut tiles,
                &mut closed,
                geom,
                home.as_deref(),
                sheet.theme.terminal,
            );
            if tiles.is_empty() {
                break;
            }
            // HALCYON-SCALE 6: the display + the scale, re-read where the
            // layout is re-read; a change rebuilds the render brain
            // (`rescale`) and drops what was sized beside it: a menu at the
            // old scale, the strips and the bar (the compositor re-carved
            // them; their CONFIGUREs deliver the new sizes).
            if let Some(di) = ring.display_info() {
                if (di.w, di.h) != (display.w, display.h) {
                    display.w = di.w;
                    display.h = di.h;
                    gs.set_display(di.w, di.h);
                }
                if di.scale != sheet.scale && !scale::is_valid_pct(di.scale) {
                    if scale_refused != Some(di.scale) {
                        say!("halcyond: display scale {} refused (not in the table); keeping {}", di.scale, sheet.scale);
                        scale_refused = Some(di.scale);
                    }
                } else if di.scale != sheet.scale {
                    rescale(di.scale, di.w, &mut sheet, &mut gs, &mut geom, &mut tiles, &mut wire_out);
                    display.scale = di.scale;
                    scale_refused = None;
                    menus.close();
                    menu_leaf = None;
                    chrome.invalidate();
                    status.invalidate();
                    rail.invalidate();
                } else if di.w != sheet.display_w {
                    // HALCYON-INSTRUMENT 7.5: the document's paddings and
                    // H1 follow the DISPLAY width, so a resize at the same
                    // scale rebuilds the sheet (a new generation) and every
                    // tile re-lays.
                    let gen = sheet.gen + 1;
                    sheet = sheet_for(&sheet.bundle(), sheet.scale, di.w);
                    sheet.gen = gen;
                    // The source follows the sheet in force at EVERY rebuild
                    // (r2 B-F7), not only the scale's.
                    gs.set_smooth(sheet.smooth_mem);
                    gs.set_kerning(sheet.kerning);
                    for t in tiles.values_mut() {
                        t.tile.invalidate_heights();
                        t.dirty = true;
                    }
                }
            }
            hints = hints_from_chords(&read_file(troot, "chords").unwrap_or_default());
            // A relayout re-arms the status bar's mint retry (the console
            // path's H-3d F5 cadence): the compositor retires a bar of the
            // old height on a scale change, and the re-mint at the new one
            // must not wait on a further relayout if its first try raced.
            status.rearm();
            chrome_dirty = true;
        }

        // (3b) H-3b/H-3d: the chrome. Only once a tile is up (first-present-wins:
        // chrome never precedes content), then per pass -- the pumps are cheap
        // idle (CONFIGURE coalesces, FRAME never queues) and refresh repaints
        // only on a change. A chrome CONFIGURE (a relayout or a focus move)
        // requests a reconcile; own_surface = u32::MAX matches no leaf, so
        // reconcile skips the console self-naming (the session's leaves are
        // described by their tiles: the program as the name, the working
        // directory as the trail) while still minting + keying every tag bar
        // and reading the focused leaf. The status bar draws the focused
        // tile's name/condition + its cwd + running-or-last command + last
        // exit (its transcript).
        if up_announced {
            // The tile-status feed (H-3b-4 on the session path): a tile's
            // transcript latches its shell's exit mark; the compositor
            // records it as the tile's status -- the live key, the hairline
            // and the bar's condition all read that ONE record. On the
            // ring's conn: the conn hosting the tile is what the gate
            // admits. Display-only: a refusal drops this exit (said once)
            // and the next exit mark retries.
            for (&leaf, t) in tiles.iter_mut() {
                if let Some(code) = t.tile.scrollback.take_exit() {
                    let st = if code == 0 { "ok" } else { "err" };
                    match ring.global_ctl(&format!("tag {} status {}", leaf, st)) {
                        Ok(()) => chrome_dirty = true,
                        Err(e) => {
                            if !status_refusal_said {
                                status_refusal_said = true;
                                say!(
                                    "halcyond: tag status refused {:?}; the live-tile key lags until the next exit",
                                    e
                                );
                            }
                        }
                    }
                }
                // A `cd` moves the strip's trail with no relayout behind it.
                if t.tile.scrollback.cwd() != t.trail_painted {
                    chrome_dirty = true;
                }
            }
            if chrome.pump(&sheet, &mut gs) {
                chrome_dirty = true;
            }
            // HALCYON-INSTRUMENT 9.1 / 6.5 / 14.9 / 14.6: the header
            // actions the pump collected, each a pane verb under THIS
            // session's authority (the compositor judges every write; a
            // refused one is a no-op here). Focus expands the tile (the
            // container's `active` follows focus); `x` closes it through
            // the tile's existing lifecycle unless it is its stack's final
            // tile, which is protected (the status notice says so);
            // a secondary press summons the tile menu; the placard's
            // action re-admits a closed empty leaf to the spawn plan.
            tile_actions.extend(chrome.take_actions());
            for a in core::mem::take(&mut tile_actions) {
                match a {
                    ChromeAction::Focus(id) => {
                        layout_verb(troot, &format!("focus {}", id));
                        relayout = true;
                    }
                    ChromeAction::Close { id, count } => {
                        if count <= 1 {
                            say!("halcyond: final tile is protected (pane {})", id);
                            status.notify("FINAL TILE IS PROTECTED", true);
                        } else if tiles.get(&id).is_some_and(|t| t.tile.scrollback.running()) {
                            // HALCYON-INSTRUMENT 14.5 (I-7): a tile whose last
                            // command is RUNNING asks before closing; the
                            // close runs on the dialog's `close` tag.
                            let t = tiles.get(&id).unwrap();
                            let name = if t.tile.title.trim().is_empty() {
                                t.program.clone()
                            } else {
                                String::from(t.tile.title.trim())
                            };
                            let cmd = halcyond::rail::sanitise_cmd(t.tile.scrollback.last_command().unwrap_or(""));
                            pending_close = Some((id, count, true));
                            if !menus.open_dialog(halcyond::dialog::Dialog::close_running(&name, &cmd), &sheet, &mut gs) {
                                // The confirmation surface could not be minted
                                // (surfaces scarce): refuse rather than close a
                                // RUNNING tile unasked (14.5 (i)); the user can
                                // retry once a surface frees.
                                pending_close = None;
                                status.notify("CANNOT CONFIRM CLOSE -- TRY AGAIN", true);
                            }
                        } else {
                            layout_verb(troot, &format!("close {}", id));
                            relayout = true;
                        }
                    }
                    ChromeAction::Menu { id, count, x, y } => {
                        let (name, retained) = tiles
                            .get(&id)
                            .map(|t| (t.program.clone(), t.fate != Fate::Live))
                            .unwrap_or_default();
                        if menus.open(tile_menu(id, &name, count, retained), x, y, (x, y, 0, 0), &sheet, &mut gs) {
                            menu_leaf = None;
                        }
                    }
                    ChromeAction::OpenShell(id) => {
                        closed.remove(&id);
                        relayout = true;
                    }
                }
            }
            if let Some(leaf) = restart_req.take() {
                // 14.6 Restart: a distinct NEW process in the same leaf --
                // the surface (and so the leaf's place, weight and frame)
                // survives; the transcript starts fresh.
                if let Some(old) = tiles.remove(&leaf) {
                    if old.fate == Fate::Live {
                        say!("halcyond: tile {} is live; restart refused", leaf);
                        tiles.insert(leaf, old);
                    } else {
                        let (surf, argv) = old.into_parts();
                        let budget = SESSION_SCROLLBACK_BUDGET / tiles.len().max(1);
                        match SessionTile::spawn(leaf, surf, geom, &argv, budget, sheet.theme.terminal) {
                            Some(t) => {
                                say!("halcyond: tile {} restarted", leaf);
                                tiles.insert(leaf, t);
                            }
                            None => {
                                // The surface went with the failed spawn (its
                                // drop destroys it): the leaf closes.
                                say!("halcyond: tile {} restart failed -- closing", leaf);
                                closed.insert(leaf);
                            }
                        }
                        chrome_dirty = true;
                    }
                }
            }
            if chrome_dirty {
                chrome_dirty = false;
                // The name: what the tile's program calls itself -- ut's
                // `mark k=prog` at every prompt (BEACON.md 12.12), or a
                // foreign program's OSC 0/2 title, latest wins -- else the
                // command line's program (a tile whose program has not
                // spoken yet, or never does). The fate and the command
                // facts ride along for the Instrument header's metadata.
                let describe = |leaf: u32| {
                    tiles.get(&leaf).map(|t| {
                        let title = t.tile.title.trim();
                        Described {
                            name: if title.is_empty() {
                                t.program.clone()
                            } else {
                                String::from(title)
                            },
                            trail: abbrev_home(t.tile.scrollback.cwd(), home.as_deref()),
                            fate: t.fate,
                            running: t.tile.scrollback.running(),
                            last_exit: t.tile.scrollback.last_exit_code(),
                            dirty: false,
                        }
                    })
                };
                chrome.reconcile(troot, u32::MAX, &sheet, &mut gs, &describe, true);
                for t in tiles.values_mut() {
                    t.trail_painted = String::from(t.tile.scrollback.cwd());
                }
            }
            // Unconditionally, like the console path (TY-6 F8): free while
            // the bar is up, and a session whose first mint raced the
            // console's retire must not then wait on a relayout it may
            // never be fanned.
            status.rearm();
            status.ensure(&sheet);
            status.pump();
            let focused_leaf = chrome.focused().map(|(id, _, _)| *id);
            let (cwd, cmd, exit_code) = focused_leaf
                .and_then(|l| tiles.get(&l))
                .map(|t| {
                    (
                        t.tile.scrollback.cwd(),
                        t.tile.scrollback.last_command(),
                        t.tile.scrollback.last_exit_code(),
                    )
                })
                .unwrap_or(("", None, None));
            // The context's directory folds home to `~` like the trail (the
            // mockups' `transcript · ~/thylacine · ut ~`).
            let cwd = abbrev_home(cwd, home.as_deref());
            let notice = status.notice();
            let running = focused_leaf
                .and_then(|l| tiles.get(&l))
                .map_or(false, |t| t.tile.scrollback.running());
            let sm = statusset::model_from(
                chrome.focused(),
                focused_leaf,
                &cwd,
                cmd,
                exit_code,
                notice,
                running,
                chrome.pane_count(),
                chrome.workspaces(),
                hints.clone(),
            );
            status.refresh(&sm, &sheet, &mut gs);
            // HALCYON-INSTRUMENT 8.1: the top rail -- the focused tile's
            // directory and name, the theme in force, the minute; its
            // buttons act under THIS session's authority (the layout
            // file), the picker and the help say what they are not yet.
            rail.rearm();
            rail.ensure(&sheet);
            let _ = rail.pump(&sheet, &mut gs);
            let title = focused_leaf
                .and_then(|l| tiles.get(&l))
                .map(|t| {
                    let title = t.tile.title.trim();
                    if title.is_empty() {
                        t.program.clone()
                    } else {
                        String::from(title)
                    }
                })
                .or_else(|| chrome.focused().map(|f| f.1.clone()))
                .unwrap_or_default();
            let (hour, minute) = statusset::clock_hm();
            // Round 1 F6: the rail's chips and the workspace list were BOTH
            // pinned to a single workspace -- `RailModel::empty()` sets
            // `workspaces: 1` and the list opened with `workspace_menu(1, 0)`
            // -- so neither could ever show a second workspace, and the
            // session gate's leg (the menu opens, Esc dismisses) passed
            // either way. The real pair already existed on the ChromeSet,
            // feeding the bar; it simply never reached here. The header's
            // `active` is ONE-based, `RailModel.active` zero-based.
            let (ws_list, ws_active) = chrome
                .workspaces()
                .unwrap_or_else(|| (alloc::vec![1], 1));
            // S4: the model paints from POSITIONS and labels from NUMBERS, so
            // the active number is resolved to its position here -- the same
            // resolution `statusset::model_from` does for the bar.
            let ws_pos = ws_list.iter().position(|&n| n == ws_active).unwrap_or(0) as u8;
            let rm = RailModel {
                cwd: cwd.clone(),
                title,
                theme: theme_name.clone(),
                hour,
                minute,
                workspaces: ws_list.clone(),
                active: ws_pos,
                ..RailModel::empty()
            };
            rail.refresh(&rm, &sheet, &mut gs);
            for a in rail.take_actions() {
                match a {
                    railset::RailAction::SplitH | railset::RailAction::SplitV => {
                        let dir = if a == railset::RailAction::SplitH { "h" } else { "v" };
                        if let Some(id) = focused_leaf {
                            if layout_verb(troot, &format!("split {} {}", id, dir)) {
                                relayout = true;
                            } else {
                                say!("halcyond: split {} on pane {} refused", dir, id);
                                status.notify("SPLIT REFUSED", true);
                            }
                        }
                    }
                    railset::RailAction::Reset => {
                        // HALCYON-INSTRUMENT 9.5 / 14.5 (I-7): RESET asks
                        // first now -- the plan runs on the dialog's `reset`
                        // tag (the menu-service Dialog arm), never here.
                        if !menus.open_dialog(halcyond::dialog::Dialog::reset(), &sheet, &mut gs) {
                            status.notify("RESET REFUSED", true);
                        }
                    }
                    railset::RailAction::Theme { x, y } => {
                        // HALCYON-INSTRUMENT 9.4 (I-7): open the picker at the
                        // control's anchor, focused on the current theme.
                        let gallery = read_gallery();
                        if gallery.is_empty() {
                            say!("halcyond: no gallery themes to pick");
                            status.notify("NO THEMES", true);
                        } else {
                            let picker = halcyond::picker::Picker::build(gallery, &current_theme_id);
                            if menus.open_picker(picker, x, y, &sheet, &mut gs) {
                                menu_leaf = None;
                            }
                        }
                    }
                    railset::RailAction::Help => {
                        // HALCYON-INSTRUMENT 9.5 (I-7b): the keyboard
                        // reference. Its rows are read from the compositor's
                        // `chords` file at every open, so they name the
                        // bindings in force -- never a literal, and a rebind
                        // is a rebind of the reference.
                        let text = read_file(troot, "chords").unwrap_or_default();
                        let h = halcyond::help::Help::from_chords(&text);
                        if h.rows.is_empty() {
                            say!("halcyond: no chords published -- no reference to show");
                            status.notify("NO CHORDS PUBLISHED", true);
                        } else if menus.open_help(h, &sheet, &mut gs) {
                            menu_leaf = None;
                        }
                    }
                    railset::RailAction::CloseFocused(id) => {
                        // 9.5 / 14.5 / 6.5 (I-7b): Super+Q, delivered here so
                        // that a running job can be asked about -- only this
                        // side knows there is one. NO final-tile protection
                        // (6.5): the chord is the structural act.
                        if let Some(t) = tiles.get(&id).filter(|t| t.tile.scrollback.running()) {
                            let name = if t.tile.title.trim().is_empty() {
                                t.program.clone()
                            } else {
                                String::from(t.tile.title.trim())
                            };
                            let cmd = halcyond::rail::sanitise_cmd(t.tile.scrollback.last_command().unwrap_or(""));
                            pending_close = Some((id, 0, false));
                            if !menus.open_dialog(halcyond::dialog::Dialog::close_running(&name, &cmd), &sheet, &mut gs) {
                                // The same refusal the header's x makes: never
                                // close a RUNNING tile unasked (14.5 (i)).
                                pending_close = None;
                                status.notify("CANNOT CONFIRM CLOSE -- TRY AGAIN", true);
                            }
                        } else if layout_verb(troot, &format!("close {}", id)) {
                            relayout = true;
                        } else {
                            // The compositor refused it (a pane this seat does
                            // not own). Say so: a chord that silently does
                            // nothing is worse than one that reports.
                            say!("halcyond: close of pane {} refused", id);
                            status.notify("CLOSE REFUSED", true);
                        }
                    }
                    railset::RailAction::Workspaces { x, y } => {
                        let wm = workspace_menu(&ws_list, ws_pos);
                        if menus.open(wm, x, y, (x, y, 0, 0), &sheet, &mut gs) {
                            menu_leaf = None;
                        }
                    }
                    railset::RailAction::Workspace(n) => {
                        // The chip ACTS, under this session's own authority,
                        // the way every other rail button does: the
                        // compositor's `workspace` verb on the layout file
                        // (W-2b). It used to only log.
                        //
                        // S4: `n` is already the workspace NUMBER -- railset
                        // maps the chip's position through the model's list --
                        // so it goes on the wire as-is. Adding one here would
                        // switch to the wrong workspace the moment the set is
                        // sparse.
                        let _ = layout_verb(troot, &format!("workspace {}", n as u32));
                    }
                    railset::RailAction::ChipsScroll(_) => {}
                }
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
        let timeout = if init.is_some() {
            INIT_REAP_POLL_MS
        } else if omitted {
            DOWN_OMITTED_POLL_MS
        } else {
            -1
        };
        // A transient status notice expires on the clock (8.2): wake for it,
        // so the live model returns without waiting on an unrelated event;
        // and the rails' clocks turn with the minute.
        let clock = statusset::clock_timeout_ms();
        let timeout = libhalcyon::motion::fold_timeout(timeout, Some(clock));
        let timeout = libhalcyon::motion::fold_timeout(timeout, status.notice_timeout_ms());
        // Section 10's caret blink is the THIRD deadline. It is the distance
        // to the next STEP, not `FRAME_MS`: a two-valued function changes
        // twice per period, so a frame-rate wake would paint nothing on 33 of
        // every 34 wakes (68.75 frames per 1100 ms, two of them useful).
        // Folded only while a caret is
        // actually on screen -- every tile may be retained, or every child
        // may have hidden its cursor, and then there is nothing to wake for.
        // Re-sampled here rather than reused from (0e) so the deadline is
        // measured from the wait it bounds.
        let caret_tick = if motion && tiles.values().any(|t| t.tile.paints_caret(inst_profile)) {
            Some(libhalcyon::motion::caret_next_step_ms(
                libthyla_rs::time::monotonic_ns() / 1_000_000,
            ))
        } else {
            None
        };
        let timeout = libhalcyon::motion::fold_timeout(timeout, caret_tick);
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
                Ingested::Ended(code) if !inst_profile => {
                    if code == 0 {
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
                    } else {
                        // Legacy: a non-clean end freezes the tile as an
                        // affordance (14.11.10), as it always did.
                        say!(
                            "halcyond: session tile leaf={} crashed -- affordance held",
                            leaf
                        );
                        t.exit = Some(1);
                        t.dirty = true;
                        let _ = t.child.kill();
                    }
                }
                Ingested::Disconnected | Ingested::Crashed if !inst_profile => {
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
                // HALCYON-INSTRUMENT 14.6: under Instrument a gone child is a
                // RETAINED tile -- header, order, body and title kept, the
                // metadata its word, no caret -- until the user closes or
                // restarts it; nothing is cleared, nothing restarts itself.
                Ingested::Ended(code) => {
                    say!(
                        "halcyond: session tile leaf={} ended (exit {}) -- retained",
                        leaf,
                        code
                    );
                    t.retain(Fate::Ended(code), code);
                    chrome_dirty = true;
                }
                Ingested::Disconnected => {
                    say!("halcyond: session tile leaf={} disconnected -- retained", leaf);
                    t.retain(Fate::Disconnected, 1);
                    chrome_dirty = true;
                }
                Ingested::Crashed => {
                    say!("halcyond: session tile leaf={} crashed -- retained", leaf);
                    t.retain(Fate::Crashed, 1);
                    chrome_dirty = true;
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
    // A still-running init script does not outlive the session.
    if let Some(mut c) = init.take() {
        let _ = c.kill();
        let _ = c.wait();
    }
    // One grace for all (r2 C-F3): every tile is hung up first, then all
    // are waited against a single deadline, then the stragglers killed --
    // not N serial graces (up to 64 s at MAX_PANES before login could
    // start unbinding).
    end_terminals(core::mem::take(&mut tiles).into_values().collect());
    code as i64
}

/// `end_terminal` over a set: every down channel closed first, one
/// `TEARDOWN_GRACE_MS` for all, then the kill for whoever is still up.
fn end_terminals(tiles: Vec<SessionTile>) {
    let mut pending: Vec<(Child, u32)> = Vec::new();
    for t in tiles {
        let SessionTile {
            child,
            _down,
            leaf,
            ..
        } = t;
        drop(_down);
        pending.push((child, leaf));
    }
    let mut waited = 0u64;
    loop {
        pending.retain_mut(|(child, leaf)| match child.try_wait() {
            Ok(Some(_)) | Err(_) => {
                #[cfg(feature = "test-mode")]
                say!("halcyond: tile {} hung up ({} ms)", leaf, waited);
                #[cfg(not(feature = "test-mode"))]
                let _ = leaf;
                false
            }
            Ok(None) => true,
        });
        if pending.is_empty() || waited >= TEARDOWN_GRACE_MS {
            break;
        }
        let _ = sleep(Duration::from_millis(TEARDOWN_POLL_MS));
        waited += TEARDOWN_POLL_MS;
    }
    for (mut child, leaf) in pending {
        let _ = child.kill();
        let _ = child.wait();
        say!("halcyond: tile {} killed after the hangup grace", leaf);
    }
}

/// Spawn the session's startup command (HALCYON.md 13.7, H-4c): the user's
/// `$home/lib/halcyon.rc` under `ut --home`, else the device `default`
/// layout's restore through the session tool, else nothing. AS the user (the
/// compositor already is), under the tile cap mask (the identity axis stops
/// here, as for every tile program); stdin from /dev/null (a script never
/// reads the console), stdout/stderr the compositor's own (its lines land in
/// the daemon log beside ours). None = nothing to run, or the spawn failed
/// (said; the session lives on -- an rc is a convenience, never a gate).
fn spawn_session_init(home: Option<&str>) -> Option<Child> {
    let init = session_init::decide(
        home,
        |p: &str| fs::exists(p),
        fs::exists(session_init::DEVICE_DEFAULT_PATH),
    );
    let argv = session_init::argv(&init)?;
    let mut cmd = Command::new(argv[0].clone());
    for a in &argv[1..] {
        cmd.arg(a.clone());
    }
    let stdin = match File::open("/dev/null") {
        Ok(f) => Stdio::File(f),
        Err(_) => Stdio::Inherit,
    };
    cmd.caps(!libthyla_rs::T_CAP_SET_IDENTITY)
        .stdin(stdin)
        .stdout(Stdio::Inherit)
        .stderr(Stdio::Inherit);
    match cmd.spawn() {
        Ok(c) => {
            say!(
                "halcyond: session init: {} (pid {})",
                argv.join(" "),
                c.pid()
            );
            Some(c)
        }
        Err(e) => {
            say!("halcyond: session init spawn failed: {:?}", e);
            None
        }
    }
}
