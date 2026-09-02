// chromeset -- the per-leaf tag-bar surfaces: the syscalling half of
// `halcyond::chrome` (HALCYON.md 13.1: the bin owns the surfaces, the fds
// and the event pump; the lib owns every rule). One Role::Chrome surface
// per visible leaf with a carved strip, placed by the compositor at the
// leaf's `tagbar` rect (H-3b-2), painted whole here from the lib's list.
//
// WHEN it runs: after the first successful console present (first-present-
// wins scanout: chrome must never precede the console) and after every
// structural relayout -- the compositor fans the main surface a CONFIGURE
// on each one, so that event is the guaranteed wake; there is no timer. A
// focus-only epoch reaches the tiles themselves as a same-size CONFIGURE
// (the redraw request), and `pump` reports it so the caller reconciles.

use alloc::collections::BTreeMap;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use halcyond::chrome::{console_name, key_for, parse_leaves, parse_rect, strip_list, Key};
use halcyond::raster::GlyphSource;
use libthyla_rs::{t_close, t_open, t_read, t_write, T_OREAD, T_OWRITE};
use tapestry::{Surface, TapError, TEV_CLOSE, TEV_CONFIGURE};

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

fn write_file(root: i64, path: &str, data: &str) -> bool {
    let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OWRITE) };
    if fd < 0 {
        return false;
    }
    let rc = unsafe { t_write(fd, data.as_ptr(), data.len()) };
    unsafe { t_close(fd) };
    rc >= 0
}

struct Tile {
    surf: Surface,
    key: Key,
    name: String,
    dirty: bool,
    dead: bool,
}

/// The live chrome surfaces, keyed by the bound pane's public id (ids are
/// never reused, so a stale key can only mean "gone").
pub struct ChromeSet {
    tiles: BTreeMap<u32, Tile>,
    own_named: bool,
    /// The leaf hosting the console surface, as the last layout read named
    /// it (the target of the status verb).
    own_pane: Option<u32>,
    /// Panes whose chrome mint failed and was said (the retry is per
    /// reconcile -- it fails fast at the mint now that no connect is
    /// involved -- but the line is said once per pane).
    failed_said: Vec<u32>,
}

impl ChromeSet {
    pub fn new() -> ChromeSet {
        ChromeSet { tiles: BTreeMap::new(), own_named: false, own_pane: None, failed_said: Vec::new() }
    }

    /// The public id of the leaf hosting the console surface, once a
    /// layout read has named it.
    pub fn own_pane(&self) -> Option<u32> {
        self.own_pane
    }

    /// Bring the chrome set in line with the layout: drop tiles for leaves
    /// that are gone or bar-free, create tiles for new strips, repaint the
    /// rest (focus, statuses and names may have moved). `own_surface` is
    /// the console surface's id: the leaf hosting it is named once, through
    /// the pane's `tag` file (section 4.1: the name is the tile's program).
    pub fn reconcile(&mut self, troot: i64, own_surface: u32, gs: &mut GlyphSource) {
        if troot < 0 {
            return;
        }
        let layout = match read_file(troot, "layout") {
            Some(s) => s,
            None => return,
        };
        let leaves = parse_leaves(&layout);
        if let Some(mine) = leaves.iter().find(|l| l.surface == Some(own_surface)) {
            self.own_pane = Some(mine.id);
            if !self.own_named && write_file(troot, &format!("pane/{}/tag", mine.id), &console_name()) {
                self.own_named = true;
            }
        }
        // The wanted set: every visible leaf with a carved strip.
        let mut want: Vec<(u32, u32, u32, Key, String)> = Vec::new();
        for l in leaves.iter() {
            let tb = match read_file(troot, &format!("pane/{}/tagbar", l.id)).and_then(|s| parse_rect(&s)) {
                Some(r) => r,
                None => continue,
            };
            if tb.2 == 0 || tb.3 == 0 {
                continue;
            }
            let name = read_file(troot, &format!("pane/{}/tag", l.id))
                .map(|s| String::from(s.trim()))
                .unwrap_or_default();
            // The status is read only where it can show (the live tile).
            let status = if l.focused {
                read_file(troot, &format!("pane/{}/status", l.id)).unwrap_or_default()
            } else {
                String::new()
            };
            want.push((l.id, tb.2, tb.3, key_for(l.focused, &status), name));
        }
        // Gone (or bar-free): drop -- the tile lives on the shared session,
        // so its Drop says `destroy` (the explicit retire) before closing
        // its fds; a bare close would leak the slot server-side.
        let keep: Vec<u32> = want.iter().map(|w| w.0).collect();
        self.tiles.retain(|id, t| keep.contains(id) && !t.dead);
        for (id, w, h, key, name) in want {
            match self.tiles.get_mut(&id) {
                Some(t) => {
                    if t.key != key || t.name != name {
                        t.key = key;
                        t.name = name;
                    }
                    // A strip resize arrives as the surface's own CONFIGURE
                    // (pump handles it); a same-size relayout needs a repaint
                    // anyway -- the compositor's structural repaint blanked
                    // the strip to its resting fill.
                    t.dirty = true;
                }
                // Minted on the pane-tree session (`troot`), never on a
                // session of its own: the H-3b round R2-F2 -- a session per
                // bar exhausted the compositor's conn pool at three windows,
                // and every further mint became a 5 s blocking connect
                // inside this single-threaded loop.
                None => match Surface::chrome_on_shared(troot, id, w, h) {
                    Ok(surf) => {
                        let mut t = Tile { surf, key, name, dirty: true, dead: false };
                        paint(&mut t, gs);
                        self.failed_said.retain(|&f| f != id);
                        self.tiles.insert(id, t);
                    }
                    Err(e) => {
                        if !self.failed_said.contains(&id) {
                            self.failed_said.push(id);
                            say(&format!("halcyond: chrome for pane {} failed {:?}", id, e));
                        }
                    }
                },
            }
        }
        for t in self.tiles.values_mut() {
            if t.dirty {
                paint(t, gs);
            }
        }
    }

    /// Drain every tile's events (non-blocking): a CONFIGURE reweaves to the
    /// new strip size (the compositor sends one on a relayout AND on a focus
    /// move -- same-size = the redraw request); a CLOSE or a dead stream
    /// marks the tile. Returns true when any CONFIGURE was seen: the caller
    /// then reconciles, which re-reads the layout (focus, statuses, names)
    /// and paints -- painting here would flash the stale state first. FRAME
    /// is droppable and never queues up.
    pub fn pump(&mut self) -> bool {
        let mut relayout = false;
        for t in self.tiles.values_mut() {
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
                        _ => {}
                    },
                    Ok(None) => break,
                    Err(_) => {
                        t.dead = true;
                        break;
                    }
                }
            }
        }
        self.tiles.retain(|_, t| !t.dead);
        relayout
    }
}

/// Execute the lib's strip list into the tile's surface and present it.
fn paint(t: &mut Tile, gs: &mut GlyphSource) {
    let (w, h) = (t.surf.w, t.surf.h);
    if w == 0 || h == 0 {
        t.dirty = false;
        return;
    }
    let cart = strip_list(t.key, &t.name, w, h, gs);
    let px = t.surf.pixels();
    cartoon::execute(&cart, &gs.packer.store, &cartoon::BlobStore::new(), px, w as usize, None);
    if t.surf.present(None).is_err() {
        // A dropped frame, never death: the next relayout repaints.
    }
    t.dirty = false;
}
