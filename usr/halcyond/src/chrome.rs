// chrome -- the per-leaf Daylight tag bar (HALCYON.md 13.6, RATIFIED
// PER-LEAF; HALCYON-VISUAL section 4). H-3b-3: DISPLAY-only.
//
// halcyond owns one Role::Chrome surface per visible leaf that carries a
// tag-bar strip, paints the whole strip (bg + separator + name), and the
// compositor PLACES it at the leaf's `tagbar` rect (H-3b-2). Strip rects and
// names come from the pane 9P tree (`layout`, `pane/<id>/tagbar`,
// `pane/<id>/tag`) -- the section 13.7 file-walk bias, no new verb.
//
// WHEN it runs: after the first successful console present (first-present-
// wins scanout: chrome must never precede the console) and after every
// structural relayout -- the compositor fans the main surface a CONFIGURE
// on each one, so that event is the guaranteed wake; there is no timer.
// Pills are commands and commands are H-3c; the live sage/cinnabar states
// ride H-3b-4's status verb. This chunk renders the two Resting states.

use alloc::collections::BTreeMap;
use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use cartoon::{Cartoon, GlyphRef, Op};
use libhalcyon::theme::{DAYLIGHT, METRICS};
use libthyla_rs::{t_close, t_open, t_read, t_write, T_OREAD, T_OWRITE};
use tapestry::{Surface, TapError, TEV_CLOSE, TEV_CONFIGURE};

use crate::raster::{GlyphSource, FACE_BODY};

/// The name typeface size (section 4.3: 10.5px, proportional).
const NAME_PX: f32 = 10.5;

fn say(s: &str) {
    let mut t = String::from(s);
    t.push('\n');
    let _ = libthyla_rs::t_putstr(&t);
}

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

/// One visible leaf as the `layout` text reports it.
struct Leaf {
    id: u32,
    focused: bool,
    surface: Option<u32>,
}

/// Parse the leaf lines of the `layout` text: "<id>[*] leaf surface=<n>|empty
/// [x,y,w,h][ hidden]" (tapestryd pane.rs render_pane). Hidden leaves are
/// skipped: they carve no strip.
fn parse_leaves(layout: &str) -> Vec<Leaf> {
    let mut out = Vec::new();
    for line in layout.lines() {
        let line = line.trim();
        if !line.contains(" leaf ") || line.ends_with("hidden") {
            continue;
        }
        let mut it = line.split_ascii_whitespace();
        let idtok = match it.next() {
            Some(t) => t,
            None => continue,
        };
        let focused = idtok.ends_with('*');
        let id: u32 = match idtok.trim_end_matches('*').parse() {
            Ok(v) => v,
            Err(_) => continue,
        };
        let surface = it
            .find_map(|t| t.strip_prefix("surface="))
            .and_then(|s| s.parse().ok());
        out.push(Leaf { id, focused, surface });
    }
    out
}

fn parse_rect(s: &str) -> Option<(u32, u32, u32, u32)> {
    let mut it = s.split_ascii_whitespace();
    let x = it.next()?.parse().ok()?;
    let y = it.next()?.parse().ok()?;
    let w = it.next()?.parse().ok()?;
    let h = it.next()?.parse().ok()?;
    Some((x, y, w, h))
}

struct Tile {
    surf: Surface,
    focused: bool,
    name: String,
    dirty: bool,
    dead: bool,
}

/// The live chrome surfaces, keyed by the bound pane's public id (ids are
/// never reused, so a stale key can only mean "gone").
pub struct ChromeSet {
    tiles: BTreeMap<u32, Tile>,
    own_named: bool,
}

impl ChromeSet {
    pub fn new() -> ChromeSet {
        ChromeSet { tiles: BTreeMap::new(), own_named: false }
    }

    /// Bring the chrome set in line with the layout: drop tiles for leaves
    /// that are gone or bar-free, create tiles for new strips, repaint the
    /// rest (focus and names may have moved). `own_surface` is the console
    /// surface's id: the leaf hosting it is named "halcyon" once, through
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
        if !self.own_named {
            if let Some(mine) = leaves.iter().find(|l| l.surface == Some(own_surface)) {
                if write_file(troot, &format!("pane/{}/tag", mine.id), "halcyon") {
                    self.own_named = true;
                }
            }
        }
        // The wanted set: every visible leaf with a carved strip.
        let mut want: Vec<(u32, u32, u32, bool, String)> = Vec::new();
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
            want.push((l.id, tb.2, tb.3, l.focused, name));
        }
        // Gone (or bar-free): drop -- Drop closes the tile's conn and the
        // compositor retires the surface.
        let keep: Vec<u32> = want.iter().map(|w| w.0).collect();
        self.tiles.retain(|id, t| keep.contains(id) && !t.dead);
        for (id, w, h, focused, name) in want {
            match self.tiles.get_mut(&id) {
                Some(t) => {
                    if t.focused != focused || t.name != name {
                        t.focused = focused;
                        t.name = name;
                        t.dirty = true;
                    }
                    // A strip resize arrives as the surface's own CONFIGURE
                    // (pump handles it); a same-size relayout needs a repaint
                    // anyway -- the compositor's structural repaint blanked
                    // the strip to its resting fill.
                    t.dirty = true;
                }
                None => match Surface::chrome_on(id, w, h) {
                    Ok(surf) => {
                        let mut t = Tile { surf, focused, name, dirty: true, dead: false };
                        paint(&mut t, gs);
                        self.tiles.insert(id, t);
                    }
                    Err(e) => say(&format!("halcyond: chrome for pane {} failed {:?}", id, e)),
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
    /// then reconciles, which re-reads the layout (focus, names) and paints
    /// -- painting here would flash the stale state first. FRAME is droppable
    /// and never queues up.
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

    pub fn len(&self) -> usize {
        self.tiles.len()
    }
}

/// Paint one strip (section 4.1/4.2 Resting states, 4.3 metrics) as one
/// display list: the header ground, the 1px separator on the bottom edge
/// (ember_deep on the focused leaf's tile -- "resting, active tile" -- else
/// border), and the name in the proportional face, vertically centred in the
/// strip above the separator. No pills (a resting bar has none), no trail.
fn paint(t: &mut Tile, gs: &mut GlyphSource) {
    let d = &DAYLIGHT;
    let (w, h) = (t.surf.w, t.surf.h);
    if w == 0 || h == 0 {
        t.dirty = false;
        return;
    }
    let mut cart = Cartoon::new();
    cart.ops.push(Op::Clear { color: d.header });
    let sep = if t.focused { d.ember_deep } else { d.border };
    cart.ops.push(Op::Rect { x: 0, y: h as i32 - 1, w, h: 1, color: sep });
    if !t.name.is_empty() {
        let color = if t.focused { d.fg } else { d.fg_muted };
        let (asc, desc) = gs
            .line_metrics(FACE_BODY, NAME_PX)
            .map(|m| (m.ascent, m.descent))
            .unwrap_or((8, 2));
        let box_h = asc + desc;
        let baseline = ((h as i32 - 1) - box_h) / 2 + asc;
        let mut refs: Vec<GlyphRef> = Vec::new();
        for ch in t.name.chars() {
            if let Some(g) = gs.glyph(FACE_BODY, NAME_PX, ch) {
                refs.push(g);
            }
        }
        if !refs.is_empty() {
            cart.push_glyphs(gs.gen(), METRICS.tag_pad_x, baseline, color, &refs);
        }
    }
    let px = t.surf.pixels();
    cartoon::execute(&cart, &gs.packer.store, &cartoon::BlobStore::new(), px, w as usize, None);
    if t.surf.present(None).is_err() {
        // A dropped frame, never death: the next relayout repaints.
    }
    t.dirty = false;
}
