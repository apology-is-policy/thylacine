// menuset -- the obj verb menu's surface: the syscalling half of
// `halcyond::menu` (HALCYON.md 13.1 split; 13.6 "Menus -- THE GATE"). ONE
// Role::Menu surface at a time, minted on the pane-tree session (the chrome
// precedent), painted whole from the lib's list, PLACED by the compositor
// through the gated `menu place <surface> <x> <y>` verb -- from which point
// the compositor owns it: every key and pointer event is routed here while
// it is up, and Esc / a click outside it / a chord / this process's death
// all tear it down compositor-side. This pump learns of that as a dead
// stream or a CLOSE and drops its half; only a CHOICE (Enter) dismisses
// from this side (`menu dismiss`), after which the chosen command runs.

use alloc::format;
use alloc::string::String;

use halcyond::menu::{menu_key, menu_list, menu_size, Action, Menu};
use halcyond::raster::GlyphSource;
use tapestry::{EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_KEY, TEV_SCROLL};

/// The display height off the pane-tree session's `ctl` (its `display W H`
/// line) -- the cap on a menu's surface height (the H-3c round F3: the
/// compositor refuses a taller surface). Unreadable = uncapped.
fn display_h(troot: i64) -> u32 {
    crate::chromeset::read_file(troot, "ctl")
        .and_then(|t| {
            t.lines()
                .find_map(|l| l.strip_prefix("display "))
                .and_then(|r| r.split_ascii_whitespace().nth(1))
                .and_then(|h| h.parse().ok())
        })
        .unwrap_or(u32::MAX)
}

fn say(s: &str) {
    let mut t = String::from(s);
    t.push('\n');
    let _ = libthyla_rs::t_putstr(&t);
}

/// What the pump reports for one pass.
pub enum MenuEvent {
    None,
    /// The user chose an item (the menu is still up; the caller closes it).
    Chosen(Action),
    /// The compositor dismissed it (Esc, click-away, a chord, a wedge).
    Closed,
}

struct Open {
    surf: Surface,
    model: Menu,
}

pub struct MenuSet {
    /// The renderer's ONE ring + session (the H-3c-2 event set).
    ring: EventRing,
    open: Option<Open>,
}

impl MenuSet {
    pub fn new(ring: EventRing) -> MenuSet {
        MenuSet { ring, open: None }
    }

    /// Summon `model` at display point (x, y): mint, paint, place, present.
    /// `run` is the obj run's display rect, said with the placement so a
    /// witness can find both. False (said once here) when the compositor
    /// refuses -- no menu, the transcript is unaffected.
    pub fn open(
        &mut self,
        model: Menu,
        x: u32,
        y: u32,
        run: (u32, u32, u32, u32),
        gs: &mut GlyphSource,
    ) -> bool {
        self.close();
        let (w, h) = menu_size(&model, gs, display_h(self.ring.root()));
        let surf = match Surface::menu_on(&self.ring, w, h) {
            Ok(s) => s,
            Err(e) => {
                say(&format!("halcyond: menu surface failed {:?}", e));
                return false;
            }
        };
        let mut o = Open { surf, model };
        let cmd = format!("menu place {} {} {}", o.surf.id, x, y);
        if let Err(e) = o.surf.global_ctl(&cmd) {
            say(&format!("halcyond: menu place refused {:?}", e));
            return false; // Drop destroys the surface
        }
        // Placed FIRST, then ONE painted present: it composes at once (a
        // present before the place composes nowhere), and every present
        // carries pixels -- the slots rotate per present, so a bare second
        // `present` would show the next slot's zeros (a black menu, caught
        // on the lever). The compositor's redraw CONFIGURE repaints again;
        // harmless.
        paint(&mut o, gs);
        say(&format!(
            "halcyond: menu {} placed at {} {} ({}x{}) for {} {} run at {} {} {} {}",
            o.surf.id, x, y, w, h, o.model.ty, o.model.refv, run.0, run.1, run.2, run.3
        ));
        self.open = Some(o);
        true
    }

    /// Service the menu surface's events (non-blocking): keys move the
    /// selection or choose; a CONFIGURE repaints (the redraw request; a
    /// resize offer reweaves); a CLOSE or a dead stream means the compositor
    /// dismissed it. The menu lives on the renderer's one ring (the H-3c-2
    /// event set), so the loop's unified poll on that ring wakes for its keys
    /// like the console's; before the event set this pump had to WAIT on the
    /// menu's own ring while a menu was up, because a 9P session's replies
    /// are read only by a thread waiting on that session (the lever found
    /// the console-parked loop never saw a menu key).
    pub fn service(&mut self, gs: &mut GlyphSource) -> MenuEvent {
        let o = match self.open.as_mut() {
            Some(o) => o,
            None => return MenuEvent::None,
        };
        let mut chosen: Option<Action> = None;
        let mut repaint = false;
        let mut dead = false;
        loop {
            let next = o.surf.poll_event();
            match next {
                Ok(Some(e)) => match e.kind {
                    TEV_KEY => {
                        if e.value >= 1 {
                            match o.model.key(menu_key(e.code, e.rune)) {
                                Some(a) => {
                                    chosen = Some(a);
                                    break;
                                }
                                None => repaint = true,
                            }
                        }
                    }
                    TEV_CONFIGURE => match o.surf.handle_configure(&e) {
                        Ok(_) => {
                            #[cfg(feature = "test-mode")]
                            say(&format!(
                                "halcyond: menu {} configure {}x{} -> repaint",
                                o.surf.id,
                                e.value >> 16,
                                e.value & 0xffff
                            ));
                            repaint = true;
                        }
                        Err(TapError::Busy) => {}
                        Err(_) => {
                            dead = true;
                            break;
                        }
                    },
                    TEV_CLOSE => {
                        dead = true;
                        break;
                    }
                    TEV_SCROLL => {
                        o.model.wheel(e.value as i32);
                        repaint = true;
                    }
                    _ => {}
                },
                Ok(None) => break,
                Err(_) => {
                    dead = true;
                    break;
                }
            }
        }
        if dead {
            self.open = None; // Drop: destroy (stale after the compositor's retire) + close
            say("halcyond: menu closed by the compositor");
            return MenuEvent::Closed;
        }
        if let Some(a) = chosen {
            return MenuEvent::Chosen(a);
        }
        if repaint {
            paint(o, gs);
        }
        MenuEvent::None
    }

    /// This side's dismiss (after a choice): tell the compositor, drop the
    /// surface. Nothing to do when the compositor already closed it.
    pub fn close(&mut self) {
        if let Some(o) = self.open.take() {
            let _ = o.surf.global_ctl("menu dismiss");
        }
    }
}

fn paint(o: &mut Open, gs: &mut GlyphSource) {
    let (w, h) = (o.surf.w, o.surf.h);
    if w == 0 || h == 0 {
        return;
    }
    let cart = menu_list(&o.model, w, h, gs);
    let px = o.surf.pixels();
    cartoon::execute(
        &cart,
        &gs.packer.store,
        &cartoon::BlobStore::new(),
        px,
        w as usize,
        None,
    );
    let rc = o.surf.present(None);
    #[cfg(feature = "test-mode")]
    if let Err(e) = &rc {
        say(&format!(
            "halcyond: menu {} present failed {:?}",
            o.surf.id, e
        ));
    }
    let _ = rc;
}
