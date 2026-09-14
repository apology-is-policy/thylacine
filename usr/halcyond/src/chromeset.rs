// chromeset -- the per-leaf tag-bar surfaces: the syscalling half of
// `halcyond::chrome` (HALCYON.md 13.1: the bin owns the surfaces, the fds
// and the event pump; the lib owns every rule). One Role::Chrome surface
// per leaf with a carved strip, placed by the compositor at the leaf's
// `tagbar` rect (H-3b-2), painted whole here from the lib's list.
//
// WHEN it runs: after the first successful console present (first-present-
// wins scanout: chrome must never precede the console) and after every
// structural relayout -- the compositor fans the main surface a CONFIGURE
// on each one, so that event is the guaranteed wake; there is no timer. A
// focus-only epoch reaches the tiles themselves as a same-size CONFIGURE
// (the redraw request), and `pump` reports it so the caller reconciles.
//
// HALCYON-INSTRUMENT 6 / 9.1 / 14.6 (I-3): under the Instrument profile the
// strip is the tile's HEADER -- one per tile of a stack, collapsed ones
// included (a collapsed leaf is `hidden` in the layout yet carries a
// `tagbar`) -- and a lone EMPTY leaf's strip is its whole interior, the
// empty pane's placard. A header is a POINTER TARGET: the compositor routes
// motion, buttons and LEAVE to it; this pump keeps the hover state and
// turns a press into a `ChromeAction` for the owner to act on under its own
// pane authority (the index or the name focuses, `x` closes, a secondary
// press summons the tile menu, the placard's action opens a shell). Under
// the legacy profile the strip and the pump are what they were: no hover,
// no actions, the same bytes.

use alloc::collections::BTreeMap;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use halcyond::chrome::{
    console_name, header_hit, header_list, header_regions, key_for, metadata_for, parse_rect,
    parse_tree, placard_list, strip_list, Described, HeaderHit, HeaderState, Key, MetaInk,
};
use halcyond::layout::Sheet;
use halcyond::raster::GlyphSource;
use libhalcyon::instrument::Profile;
use libthyla_rs::{t_close, t_open, t_read, t_write, T_OREAD, T_OWRITE};
use tapestry::{
    EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_PTR_BTN, TEV_PTR_LEAVE,
    TEV_PTR_MOVE,
};

/// evdev BTN_LEFT / BTN_RIGHT (the tapestry PTR_BTN `code`).
const BTN_LEFT: u16 = 0x110;
const BTN_RIGHT: u16 = 0x111;

fn say(s: &str) {
    let mut t = String::from(s);
    t.push('\n');
    let _ = libthyla_rs::t_putstr(&t);
}

/// Read a pane-tree file to EOF. Reads until a zero-length return (never
/// one `t_read`: a `layout` past one read's worth would silently drop the
/// leaves after the cut -- the H-3b round F3), bounded by `READ_MAX`
/// (the tree's files are small by construction; the bound is a backstop
/// against a runaway server, not a size the parse relies on).
pub fn read_file(root: i64, path: &str) -> Option<String> {
    const READ_MAX: usize = 1 << 20;
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
    if fd < 0 {
        return None;
    }
    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = alloc::vec![0u8; 4096];
    loop {
        let n = unsafe { t_read(fd, chunk.as_mut_ptr(), chunk.len()) };
        if n < 0 {
            unsafe { t_close(fd) };
            return None;
        }
        if n == 0 {
            break;
        }
        buf.extend_from_slice(&chunk[..n as usize]);
        if buf.len() >= READ_MAX {
            break;
        }
    }
    unsafe { t_close(fd) };
    String::from_utf8(buf).ok()
}

/// Write one pane-tree file (a verb on a ctl, a tag). True on success.
pub fn write_file(root: i64, path: &str, data: &str) -> bool {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
    if fd < 0 {
        return false;
    }
    let rc = unsafe { t_write(fd, data.as_ptr(), data.len()) };
    unsafe { t_close(fd) };
    rc >= 0
}

/// What a chrome surface paints (the Instrument profile; the legacy strip
/// is `Kind::Strip`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum Kind {
    /// The legacy Daylight tag bar.
    Strip,
    /// An Instrument tile header (6.4).
    Header,
    /// The empty pane's placard (14.6): the whole interior of a lone empty
    /// leaf. `can_spawn` shows the `Open shell` action.
    Placard { can_spawn: bool },
}

struct Tile {
    surf: Surface,
    kind: Kind,
    /// The legacy key (the strip's row).
    key: Key,
    /// The Instrument header's facts (index, expanded, last, focus).
    state: HeaderState,
    /// The stack's size, for the actions' final-tile rule.
    count: u32,
    name: String,
    /// The legacy trail, or the Instrument metadata with its ink.
    trail: String,
    meta: (String, MetaInk),
    /// The pointer's last surface position over this surface; None once it
    /// left (TEV_PTR_LEAVE) or before it arrived.
    hover: Option<(i32, i32)>,
    /// The placard's action rect (x, y, w, h) as last painted.
    action: Option<(i32, i32, i32, i32)>,
    /// The surface's display origin (the tagbar's x, y): a menu is summoned
    /// at display coordinates.
    origin: (u32, u32),
    dirty: bool,
    dead: bool,
}

/// What a host says about a leaf it hosts (the name, the trail, the fate,
/// the running / last-exit facts); None for a leaf it does not host.
pub type Describe<'a> = &'a dyn Fn(u32) -> Option<Described>;

/// A pointer action on a header or a placard (HALCYON-INSTRUMENT 9.1 /
/// 6.5 / 14.9 / 14.6), for the owner to act on under its own authority.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum ChromeAction {
    /// A primary press on the index, the name or the metadata: focus (and
    /// so expand) the tile.
    Focus(u32),
    /// A primary press on `x`: close the tile; `count` is its stack's size
    /// (a stack's final tile is protected, 6.5).
    Close { id: u32, count: u32 },
    /// A secondary press anywhere but `x`: the tile verb menu at display
    /// point (x, y).
    Menu { id: u32, count: u32, x: u32, y: u32 },
    /// The placard's `Open shell`.
    OpenShell(u32),
}

/// The live chrome surfaces, keyed by the bound pane's public id (ids are
/// never reused, so a stale key can only mean "gone").
pub struct ChromeSet {
    /// The renderer's ONE ring + session (the H-3c-2 event set): every tile
    /// is minted on it, so their CONFIGUREs wake and land with the console's.
    ring: EventRing,
    tiles: BTreeMap<u32, Tile>,
    own_named: bool,
    /// The leaf hosting the console surface, as the last layout read named
    /// it (the target of the status verb).
    own_pane: Option<u32>,
    /// Panes whose chrome mint failed and was said (the retry is per
    /// reconcile -- it fails fast at the mint now that no connect is
    /// involved -- but the line is said once per pane).
    failed_said: Vec<u32>,
    /// H-3d: the focused leaf as the last layout read named it -- its pane
    /// id, its tag name, its recorded status text -- the status bar's
    /// context + condition sources.
    focused: Option<(u32, String, String)>,
    /// Actions the pump collected, taken by the caller.
    actions: Vec<ChromeAction>,
    /// HALCYON-INSTRUMENT 8.2: the workspace's panes as of the last
    /// reconcile (`rail::pane_count`), the footer's right group.
    pane_count: u32,
}

impl ChromeSet {
    pub fn new(ring: EventRing) -> ChromeSet {
        ChromeSet {
            ring,
            tiles: BTreeMap::new(),
            own_named: false,
            own_pane: None,
            failed_said: Vec::new(),
            focused: None,
            actions: Vec::new(),
            pane_count: 1,
        }
    }

    /// 8.2: the panes of the last layout read (a stack counts once).
    pub fn pane_count(&self) -> u32 {
        self.pane_count
    }

    /// The public id of the leaf hosting the console surface, once a
    /// layout read has named it.
    pub fn own_pane(&self) -> Option<u32> {
        self.own_pane
    }

    /// H-3d: the focused leaf (pane id, tag name, status text) as of the
    /// last reconcile; None before one, or with nothing focused.
    pub fn focused(&self) -> Option<&(u32, String, String)> {
        self.focused.as_ref()
    }

    /// The actions the pump collected since the last take (9.1).
    pub fn take_actions(&mut self) -> Vec<ChromeAction> {
        core::mem::take(&mut self.actions)
    }

    /// Bring the chrome set in line with the layout: drop tiles for leaves
    /// that are gone or bar-free, create tiles for new strips, repaint the
    /// rest (focus, statuses, names and trails may have moved). `own_surface`
    /// is the console surface's id: the leaf hosting it is named once,
    /// through the pane's `tag` file. `describe` is the host's word on the
    /// leaves it hosts -- the tile's program as the name and its status as
    /// the trail (section 4.1), its fate and its command facts under
    /// Instrument; a leaf it does not describe shows its `tag` text as the
    /// name and no trail. `can_spawn`: whether this owner may fill an empty
    /// pane (the placard's action shows only then).
    pub fn reconcile(
        &mut self,
        troot: i64,
        own_surface: u32,
        sheet: &Sheet,
        gs: &mut GlyphSource,
        describe: Describe,
        can_spawn: bool,
    ) {
        if troot < 0 {
            return;
        }
        let layout = match read_file(troot, "layout") {
            Some(s) => s,
            None => return,
        };
        let inst = sheet.profile == Profile::Instrument;
        let tree = parse_tree(&layout);
        let panes = halcyond::rail::pane_count(&tree, |t| {
            t.leaf.surface.is_some() && describe(t.leaf.id).is_none()
        })
        .max(1);
        // Said on a change, under Instrument only (test builds): a gate reads
        // the count and the leaves behind it; the legacy console's transcript
        // must not grow a row for it.
        #[cfg(feature = "test-mode")]
        if inst && panes != self.pane_count {
            let ids: Vec<u32> = tree.iter().map(|t| t.leaf.id).collect();
            say(&format!("halcyond: pane count {} (leaves {:?})", panes, ids));
        }
        self.pane_count = panes;
        if let Some(mine) = tree.iter().find(|t| t.leaf.surface == Some(own_surface)) {
            self.own_pane = Some(mine.leaf.id);
            if !self.own_named
                && write_file(troot, &format!("pane/{}/tag", mine.leaf.id), &console_name())
            {
                self.own_named = true;
            }
        }
        // The wanted set: every leaf with a carved strip -- under Instrument
        // a collapsed (hidden) leaf carries a header too, and a lone empty
        // leaf its placard; under legacy a hidden leaf's tagbar is ZERO.
        struct Want {
            id: u32,
            w: u32,
            h: u32,
            origin: (u32, u32),
            kind: Kind,
            key: Key,
            state: HeaderState,
            count: u32,
            name: String,
            trail: String,
            meta: (String, MetaInk),
        }
        let mut want: Vec<Want> = Vec::new();
        self.focused = None;
        for t in tree.iter() {
            let l = &t.leaf;
            // The name: the host's word for a leaf it hosts, else the
            // pane's tag text. Read once per leaf; both consumers below.
            let d = match describe(l.id) {
                Some(d) => d,
                None => Described::plain(
                    read_file(troot, &format!("pane/{}/tag", l.id))
                        .map(|s| String::from(s.trim()))
                        .unwrap_or_default(),
                    String::new(),
                ),
            };
            if l.focused {
                // H-3d: the status bar's sources, read whether or not the
                // leaf carves a strip (a single fullscreen leaf carves none).
                let status = read_file(troot, &format!("pane/{}/status", l.id)).unwrap_or_default();
                self.focused = Some((l.id, d.name.clone(), status));
            }
            if !inst && l.hidden {
                continue;
            }
            let tb = match read_file(troot, &format!("pane/{}/tagbar", l.id))
                .and_then(|s| parse_rect(&s))
            {
                Some(r) => r,
                None => continue,
            };
            if tb.2 == 0 || tb.3 == 0 {
                continue;
            }
            // The status is read only where it can show (the live tile).
            let status = if l.focused {
                read_file(troot, &format!("pane/{}/status", l.id)).unwrap_or_default()
            } else {
                String::new()
            };
            let kind = if !inst {
                Kind::Strip
            } else if t.empty && t.count == 1 {
                Kind::Placard { can_spawn }
            } else {
                Kind::Header
            };
            let meta = metadata_for(&d);
            want.push(Want {
                id: l.id,
                w: tb.2,
                h: tb.3,
                origin: (tb.0, tb.1),
                kind,
                key: key_for(l.focused, &status),
                state: HeaderState {
                    focused: l.focused,
                    expanded: !l.hidden,
                    hovered: false,
                    hover_close: false,
                    index: t.index,
                    last: t.last,
                },
                count: t.count,
                name: d.name,
                trail: d.trail,
                meta,
            });
        }
        // Gone (or bar-free): drop -- the tile lives on the shared session,
        // so its Drop says `destroy` (the explicit retire) before closing
        // its fds; a bare close would leak the slot server-side.
        let keep: Vec<u32> = want.iter().map(|w| w.id).collect();
        self.tiles.retain(|id, t| keep.contains(id) && !t.dead);
        for w in want {
            match self.tiles.get_mut(&w.id) {
                Some(t) => {
                    // The hover facts are the pump's; the rest is the layout's.
                    let mut state = w.state;
                    state.hovered = t.state.hovered;
                    state.hover_close = t.state.hover_close;
                    t.key = w.key;
                    t.state = state;
                    t.count = w.count;
                    t.name = w.name;
                    t.trail = w.trail;
                    t.meta = w.meta;
                    t.origin = w.origin;
                    if t.kind != w.kind {
                        t.kind = w.kind;
                        t.hover = None;
                        t.state.hovered = false;
                        t.state.hover_close = false;
                    }
                    // A strip resize arrives as the surface's own CONFIGURE
                    // (pump handles it); a same-size relayout needs a repaint
                    // anyway -- the compositor's structural repaint blanked
                    // the strip to its resting fill.
                    t.dirty = true;
                }
                // Minted on the renderer's one ring + session, never on a
                // session of its own: the H-3b round R2-F2 -- a session per
                // bar exhausted the compositor's conn pool at three windows,
                // and every further mint became a 5 s blocking connect
                // inside this single-threaded loop; and (H-3c-2) a ring of
                // its own left its events unread until a pane-tree RPC.
                None => match Surface::chrome_on(&self.ring, w.id, w.w, w.h) {
                    Ok(surf) => {
                        let mut t = Tile {
                            surf,
                            kind: w.kind,
                            key: w.key,
                            state: w.state,
                            count: w.count,
                            name: w.name,
                            trail: w.trail,
                            meta: w.meta,
                            hover: None,
                            action: None,
                            origin: w.origin,
                            dirty: true,
                            dead: false,
                        };
                        paint(&mut t, sheet, gs);
                        // Where the strip sits (test builds): a gate that
                        // must press a header finds it here.
                        #[cfg(feature = "test-mode")]
                        say(&format!(
                            "halcyond: chrome {} for pane {} at {},{} {}x{}",
                            t.surf.id, w.id, w.origin.0, w.origin.1, w.w, w.h
                        ));
                        self.failed_said.retain(|&f| f != w.id);
                        self.tiles.insert(w.id, t);
                    }
                    Err(e) => {
                        if !self.failed_said.contains(&w.id) {
                            self.failed_said.push(w.id);
                            say(&format!("halcyond: chrome for pane {} failed {:?}", w.id, e));
                        }
                    }
                },
            }
        }
        for t in self.tiles.values_mut() {
            if t.dirty {
                paint(t, sheet, gs);
            }
        }
    }

    /// Every strip repaints on the next reconcile (a sheet change: the
    /// scale moved and the compositor re-carved every bar).
    pub fn invalidate(&mut self) {
        for t in self.tiles.values_mut() {
            t.dirty = true;
        }
    }

    /// Drain every tile's events (non-blocking): a CONFIGURE reweaves to the
    /// new strip size (the compositor sends one on a relayout AND on a focus
    /// move -- same-size = the redraw request); a CLOSE or a dead stream
    /// marks the tile. Returns true when any CONFIGURE was seen: the caller
    /// then reconciles, which re-reads the layout (focus, statuses, names)
    /// and paints -- painting here would flash the stale state first. FRAME
    /// is droppable and never queues up. The tiles share the console's ring,
    /// so a focus-only CONFIGURE (no console event) still wakes the loop's
    /// `EventRing::wait` and is here on the next pass -- the H-3c-2 event
    /// set; before it, a tile's event landed only at the next pane-tree RPC.
    ///
    /// HALCYON-INSTRUMENT 9.1 (I-3): pointer events reach a header too.
    /// MOVE keeps the hover position (a hovered collapsed header lights and
    /// shows its `x`; the `x` under the pointer reddens), LEAVE clears it,
    /// and a press becomes a `ChromeAction` (`take_actions`), judged by
    /// the lib's regions. The hover changes repaint here directly (no
    /// layout fact moved). Under the legacy profile the strip ignores all
    /// three: no hover, no actions.
    pub fn pump(&mut self, sheet: &Sheet, gs: &mut GlyphSource) -> bool {
        let mut relayout = false;
        let inst = sheet.profile == Profile::Instrument;
        for (&id, t) in self.tiles.iter_mut() {
            loop {
                match t.surf.poll_event() {
                    Ok(Some(e)) => match e.kind {
                        TEV_CONFIGURE => {
                            relayout = true;
                            match t.surf.handle_configure(&e) {
                                Ok(_) => t.dirty = true,
                                Err(TapError::Busy) => {}
                                Err(_) => t.dead = true,
                            }
                        }
                        TEV_CLOSE => t.dead = true,
                        TEV_PTR_MOVE if inst => {
                            let p = ((e.value >> 16) as u16 as i32, (e.value & 0xffff) as u16 as i32);
                            t.hover = Some(p);
                            if hover_apply(t, sheet) {
                                t.dirty = true;
                            }
                        }
                        TEV_PTR_LEAVE if inst => {
                            t.hover = None;
                            if hover_apply(t, sheet) {
                                t.dirty = true;
                            }
                        }
                        TEV_PTR_BTN if inst && e.value == 1 => {
                            let (x, y) = t.hover.unwrap_or((0, 0));
                            let (w, h) = (t.surf.w, t.surf.h);
                            match t.kind {
                                Kind::Header => {
                                    let hit = header_hit(x, y, w, h, sheet);
                                    let action = match (e.code, hit) {
                                        (BTN_LEFT, HeaderHit::Action) => {
                                            Some(ChromeAction::Close { id, count: t.count })
                                        }
                                        (BTN_LEFT, HeaderHit::Tile) => Some(ChromeAction::Focus(id)),
                                        (BTN_RIGHT, HeaderHit::Tile) => Some(ChromeAction::Menu {
                                            id,
                                            count: t.count,
                                            x: t.origin.0.saturating_add(x.max(0) as u32),
                                            y: t.origin.1.saturating_add(y.max(0) as u32),
                                        }),
                                        _ => None,
                                    };
                                    if let Some(a) = action {
                                        #[cfg(feature = "test-mode")]
                                        say(&format!("halcyond: header {} press {:?}", id, a));
                                        self.actions.push(a);
                                    }
                                }
                                Kind::Placard { can_spawn } => {
                                    if e.code == BTN_LEFT && can_spawn && in_rect(t.action, x, y) {
                                        #[cfg(feature = "test-mode")]
                                        say(&format!("halcyond: placard {} press OpenShell", id));
                                        self.actions.push(ChromeAction::OpenShell(id));
                                    }
                                }
                                Kind::Strip => {}
                            }
                        }
                        _ => {}
                    },
                    Ok(None) => break,
                    Err(_) => {
                        t.dead = true;
                        break;
                    }
                }
            }
            if t.dirty && !t.dead && !relayout {
                // A hover change alone: repaint now (a relayout repaints in
                // the reconcile that follows it).
                paint(t, sheet, gs);
            }
        }
        self.tiles.retain(|_, t| !t.dead);
        relayout
    }
}

fn in_rect(r: Option<(i32, i32, i32, i32)>, x: i32, y: i32) -> bool {
    match r {
        Some((rx, ry, rw, rh)) => x >= rx && x < rx + rw && y >= ry && y < ry + rh,
        None => false,
    }
}

/// Re-derive the hover facts from the pointer position: true when a fact
/// changed (the tile repaints). A header: hovered, and whether the pointer
/// is over the `x`; a placard: whether it is over the action.
fn hover_apply(t: &mut Tile, sheet: &Sheet) -> bool {
    let (w, h) = (t.surf.w, t.surf.h);
    let before = (t.state.hovered, t.state.hover_close);
    match t.kind {
        Kind::Header => {
            t.state.hovered = t.hover.is_some();
            t.state.hover_close = t.hover.map_or(false, |(x, y)| {
                header_hit(x, y, w, h, sheet) == HeaderHit::Action
            });
        }
        Kind::Placard { .. } => {
            t.state.hovered = t.hover.map_or(false, |(x, y)| in_rect(t.action, x, y));
            t.state.hover_close = false;
        }
        Kind::Strip => {}
    }
    before != (t.state.hovered, t.state.hover_close)
}

/// Execute the lib's list into the tile's surface and present it.
fn paint(t: &mut Tile, sheet: &Sheet, gs: &mut GlyphSource) {
    let (w, h) = (t.surf.w, t.surf.h);
    if w == 0 || h == 0 {
        t.dirty = false;
        return;
    }
    let cart = match t.kind {
        Kind::Strip => strip_list(t.key, &t.name, &t.trail, w, h, sheet, gs),
        Kind::Header => {
            let _ = header_regions(w, h, sheet);
            header_list(t.state, &t.name, &t.meta.0, t.meta.1, w, h, sheet, gs)
        }
        Kind::Placard { can_spawn } => {
            let (c, action) = placard_list(can_spawn, t.state.hovered, w, h, sheet, gs);
            t.action = action;
            c
        }
    };
    let px = t.surf.pixels();
    cartoon::execute(
        &cart,
        &gs.packer.store,
        &cartoon::BlobStore::new(),
        px,
        w as usize,
        None,
    );
    if t.surf.present(None).is_err() {
        // A dropped frame, never death: the next relayout repaints.
    }
    t.dirty = false;
}
