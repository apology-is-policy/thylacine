// menuset -- the ONE Role::Menu surface, and the three models that ride it
// (HALCYON.md 13.1 split; 13.6 "Menus -- THE GATE"; HALCYON-INSTRUMENT 9.4 /
// 14.5, I-7). ONE Role::Menu surface at a time, minted on the pane-tree
// session (the chrome precedent), painted whole from the lib's list, PLACED
// by the compositor through the gated `menu place` verb -- from which point
// the compositor owns it: every key and pointer event is routed here while
// it is up, and Esc / a click outside it / a chord / this process's death
// all tear it down compositor-side. This pump learns of that as a dead
// stream or a CLOSE and drops its half; a CHOICE (a verb, a theme, a dialog
// button) dismisses from this side (`menu dismiss`), after which the caller
// acts on it.
//
// Since I-7 the surface carries a `Model`: the verb Menu (H-3c), the theme
// Picker (9.4), a modal Dialog (14.5), or -- since I-7b -- the keyboard
// reference Help (9.5). One surface, one grab, one dismiss path; the model
// only changes what is painted and how a key/click reads.

use alloc::format;
use alloc::string::String;

use halcyond::dialog::{self, Dialog};
use halcyond::help::{self, Help};
use halcyond::layout::Sheet;
use halcyond::menu::{menu_key, menu_list, menu_size, Action, Menu};
use halcyond::picker::{self, Picker};
use halcyond::raster::GlyphSource;
use tapestry::{
    EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE, TEV_KEY, TEV_PTR_BTN, TEV_PTR_MOVE,
    TEV_SCROLL,
};

/// evdev BTN_LEFT (the tapestry PTR_BTN `code`).
const BTN_LEFT: u16 = 0x110;
/// The tapestry event's Shift bit (keymap.rs MOD_SHIFT); a modal needs only
/// Shift (for Shift+Tab). Local so the client keeps no compositor-crate dep.
const MOD_SHIFT: u16 = 1 << 0;

/// The display dims off the pane-tree session's `ctl` (`display W H`): the
/// picker's height caps at H - 72, the dialog centres on W x H, and every
/// menu's surface height is bounded by H (the H-3c round F3). Unreadable = a
/// large fallback so a mint still succeeds.
fn display_dims(troot: i64) -> (u32, u32) {
    crate::chromeset::read_file(troot, "ctl")
        .and_then(|t| {
            t.lines().find_map(|l| {
                let r = l.strip_prefix("display ")?;
                let mut it = r.split_ascii_whitespace();
                let w: u32 = it.next()?.parse().ok()?;
                let h: u32 = it.next()?.parse().ok()?;
                Some((w, h))
            })
        })
        .unwrap_or((u32::MAX, u32::MAX))
}

fn say(s: &str) {
    let mut t = String::from(s);
    t.push('\n');
    let _ = libthyla_rs::t_putstr(&t);
}

/// What the pump reports for one pass.
pub enum MenuEvent {
    None,
    /// The verb menu: the user chose an item.
    Chosen(Action),
    /// The picker: the user committed a theme (its gallery id).
    ThemeChosen(String),
    /// A dialog: the user activated a button (its tag).
    Dialog(String),
    /// The keyboard reference: the user closed it from inside (its x, Enter
    /// or Space). The compositor's own dismiss is `Closed`, as for every
    /// model -- and for the reference the two mean the same thing.
    HelpClosed,
    /// The compositor dismissed it (Esc, click-away, a chord, a wedge). For
    /// a dialog this is Cancel; for the picker, applied nothing.
    Closed,
}

/// The four models on the menu surface.
enum Model {
    Verbs(Menu),
    Picker(Picker),
    Dialog(Dialog),
    Help(Help),
}

struct Open {
    surf: Surface,
    model: Model,
    /// The last pointer point the compositor routed here (surface coords), so
    /// a BTN edge acts where the last MOVE landed (the grab routes MOVE
    /// before BTN); None until the first MOVE.
    ptr: Option<(i32, i32)>,
}

pub struct MenuSet {
    /// The renderer's ONE ring + session (the H-3c-2 event set).
    ring: EventRing,
    troot: i64,
    open: Option<Open>,
}

impl MenuSet {
    pub fn new(ring: EventRing) -> MenuSet {
        let troot = ring.root();
        MenuSet {
            ring,
            troot,
            open: None,
        }
    }

    /// Is a model up now?
    pub fn is_open(&self) -> bool {
        self.open.is_some()
    }

    /// Summon the verb `model` at display point (x, y): mint, paint, place.
    /// `run` is the obj run's display rect, said with the placement. False
    /// (said once here) when the compositor refuses.
    pub fn open(
        &mut self,
        model: Menu,
        x: u32,
        y: u32,
        run: (u32, u32, u32, u32),
        sheet: &Sheet,
        gs: &mut GlyphSource,
    ) -> bool {
        let (_, dh) = display_dims(self.troot);
        let (w, h) = menu_size(&model, sheet, gs, dh);
        let desc = format!(
            "{} {} run at {} {} {} {}",
            model.ty, model.refv, run.0, run.1, run.2, run.3
        );
        self.summon(Model::Verbs(model), w, h, x, y, &desc, sheet, gs)
    }

    /// Summon the theme picker anchored at display point (x, y) -- the
    /// control's bottom-left (9.4); the compositor clamps into the display.
    pub fn open_picker(&mut self, picker: Picker, x: u32, y: u32, sheet: &Sheet, gs: &mut GlyphSource) -> bool {
        let (_, dh) = display_dims(self.troot);
        let cap = dh.saturating_sub(sheet.ipx(picker::DISPLAY_MARGIN).max(0) as u32);
        let (w, h) = picker::picker_size(&picker, sheet, cap);
        let cur = String::from(picker.selected_id().unwrap_or(""));
        let desc = format!("picker {}", cur);
        self.summon(Model::Picker(picker), w, h, x, y, &desc, sheet, gs)
    }

    /// Summon a modal dialog, centred on the display (14.5); the compositor
    /// clamps.
    pub fn open_dialog(&mut self, d: Dialog, sheet: &Sheet, gs: &mut GlyphSource) -> bool {
        let (dw, dh) = display_dims(self.troot);
        let dwf = if dw == u32::MAX { 1440 } else { dw };
        let dhf = if dh == u32::MAX { 900 } else { dh };
        let (w, h) = dialog::dialog_size(&d, sheet, dwf, dhf, gs);
        let x = dwf.saturating_sub(w) / 2;
        let y = dhf.saturating_sub(h) / 2;
        let desc = format!("dialog {}", d.kind);
        self.summon(Model::Dialog(d), w, h, x, y, &desc, sheet, gs)
    }

    /// Summon the keyboard reference, centred on the display (9.5, I-7b);
    /// the compositor clamps. Its frame is the help card's, not 14.5's.
    pub fn open_help(&mut self, h: Help, sheet: &Sheet, gs: &mut GlyphSource) -> bool {
        let (dw, dh) = display_dims(self.troot);
        let dwf = if dw == u32::MAX { 1440 } else { dw };
        let dhf = if dh == u32::MAX { 900 } else { dh };
        let (w, hh) = help::help_size(&h, sheet, dwf, dhf, gs);
        let x = dwf.saturating_sub(w) / 2;
        let y = dhf.saturating_sub(hh) / 2;
        let desc = format!("help {} rows", h.rows.len());
        self.summon(Model::Help(h), w, hh, x, y, &desc, sheet, gs)
    }

    fn summon(
        &mut self,
        model: Model,
        w: u32,
        h: u32,
        x: u32,
        y: u32,
        desc: &str,
        sheet: &Sheet,
        gs: &mut GlyphSource,
    ) -> bool {
        self.close();
        let surf = match Surface::menu_on(&self.ring, w, h) {
            Ok(s) => s,
            Err(e) => {
                say(&format!("halcyond: menu surface failed {:?}", e));
                return false;
            }
        };
        let mut o = Open {
            surf,
            model,
            ptr: None,
        };
        // Section 10 as revised 2026-09-16: a dialog and the keyboard
        // reference take the backdrop, and the compositor learns the class
        // from this one word -- a bare placement is a menu.
        let class = match o.model {
            Model::Dialog(_) | Model::Help(_) => " dialog",
            Model::Verbs(_) | Model::Picker(_) => "",
        };
        let cmd = format!("menu place {} {} {}{}", o.surf.id, x, y, class);
        if let Err(e) = o.surf.global_ctl(&cmd) {
            say(&format!("halcyond: menu place refused {:?}", e));
            return false; // Drop destroys the surface
        }
        paint(&mut o, sheet, gs);
        say(&format!(
            "halcyond: menu {} placed at {} {} ({}x{}) for {}",
            o.surf.id, x, y, w, h, desc
        ));
        self.open = Some(o);
        true
    }

    /// Service the menu surface's events (non-blocking): keys move / choose,
    /// a pointer click chooses (picker / dialog), a CONFIGURE repaints, a
    /// CLOSE or a dead stream means the compositor dismissed it.
    pub fn service(&mut self, sheet: &Sheet, gs: &mut GlyphSource) -> MenuEvent {
        let o = match self.open.as_mut() {
            Some(o) => o,
            None => return MenuEvent::None,
        };
        let (w, h) = (o.surf.w, o.surf.h);
        let mut chosen: Option<MenuEvent> = None;
        let mut repaint = false;
        let mut dead = false;
        loop {
            match o.surf.poll_event() {
                Ok(Some(e)) => match e.kind {
                    TEV_KEY => {
                        if e.value >= 1 {
                            let shift = e.mods & MOD_SHIFT != 0;
                            match model_key(&mut o.model, e.code, e.rune, shift, w, h, sheet, gs) {
                                Some(ev) => {
                                    chosen = Some(ev);
                                    break;
                                }
                                None => repaint = true,
                            }
                        }
                    }
                    TEV_PTR_MOVE => {
                        let p = ((e.value >> 16) as u16 as i32, (e.value & 0xffff) as u16 as i32);
                        o.ptr = Some(p);
                        if model_hover(&mut o.model, p.0, p.1, w, h, sheet, gs) {
                            repaint = true;
                        }
                    }
                    TEV_PTR_BTN => {
                        if e.code == BTN_LEFT && e.value == 1 {
                            if let Some((mx, my)) = o.ptr {
                                if let Some(ev) = model_click(&mut o.model, mx, my, w, h, sheet, gs) {
                                    chosen = Some(ev);
                                    break;
                                }
                            }
                        }
                    }
                    TEV_CONFIGURE => match o.surf.handle_configure(&e) {
                        Ok(_) => repaint = true,
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
                        model_wheel(&mut o.model, e.value as i32, w, h, sheet, gs);
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
            self.open = None; // Drop: destroy + close
            say("halcyond: menu closed by the compositor");
            return MenuEvent::Closed;
        }
        if let Some(ev) = chosen {
            return ev;
        }
        if repaint {
            if let Some(o) = self.open.as_mut() {
                paint(o, sheet, gs);
            }
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

fn model_key(
    m: &mut Model,
    code: u16,
    rune: u32,
    shift: bool,
    w: u32,
    h: u32,
    sheet: &Sheet,
    gs: &mut GlyphSource,
) -> Option<MenuEvent> {
    match m {
        Model::Verbs(menu) => menu.key(menu_key(code, rune)).map(MenuEvent::Chosen),
        Model::Picker(p) => p.key(picker::picker_key(code, rune)).map(MenuEvent::ThemeChosen),
        Model::Dialog(d) => d.key(dialog::dialog_key(code, rune, shift)).map(MenuEvent::Dialog),
        // The reference's scroll keys need the surface to clamp against, so
        // they take the geometry the caller already holds.
        Model::Help(hp) => hp
            .key(help::help_key(code, rune), w, h, sheet, gs)
            .map(|()| MenuEvent::HelpClosed),
    }
}

fn model_wheel(m: &mut Model, delta: i32, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) {
    match m {
        Model::Verbs(menu) => menu.wheel(delta),
        Model::Picker(p) => p.wheel(delta),
        Model::Dialog(_) => {}
        Model::Help(hp) => hp.wheel(delta, w, h, sheet, gs),
    }
}

/// Hover moves the selection / focus to what is under the pointer; returns
/// whether it changed (a repaint).
fn model_hover(m: &mut Model, x: i32, y: i32, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> bool {
    match m {
        Model::Verbs(_) => false,
        Model::Picker(p) => match picker::option_at(p, x, y, w, h, sheet) {
            Some(i) if p.sel != i => {
                p.sel = i;
                true
            }
            _ => false,
        },
        Model::Dialog(d) => match d.button_at(x, y, w, h, sheet, gs) {
            Some(i) if d.focus != i => {
                d.focus = i;
                true
            }
            _ => false,
        },
        Model::Help(hp) => {
            let over = hp.close_at(x, y, w, sheet);
            if hp.close_hover != over {
                hp.close_hover = over;
                true
            } else {
                false
            }
        }
    }
}

/// A primary press chooses on the picker (commit the option) and the dialog
/// (activate the button); the verb menu is keyboard-driven (a click inside
/// it is not an activation -- the compositor owns click-away).
fn model_click(m: &mut Model, x: i32, y: i32, w: u32, h: u32, sheet: &Sheet, gs: &mut GlyphSource) -> Option<MenuEvent> {
    match m {
        Model::Verbs(_) => None,
        Model::Picker(p) => {
            let i = picker::option_at(p, x, y, w, h, sheet)?;
            p.sel = i;
            p.selected_id().map(|s| MenuEvent::ThemeChosen(String::from(s)))
        }
        Model::Dialog(d) => {
            let i = d.button_at(x, y, w, h, sheet, gs)?;
            d.focus = i;
            d.buttons.get(i).map(|b| MenuEvent::Dialog(b.tag.clone()))
        }
        // The reference's only control is its x; a press anywhere else in it
        // is inert (the compositor owns click-AWAY).
        Model::Help(hp) => hp.close_at(x, y, w, sheet).then_some(MenuEvent::HelpClosed),
    }
}

fn paint(o: &mut Open, sheet: &Sheet, gs: &mut GlyphSource) {
    let (w, h) = (o.surf.w, o.surf.h);
    if w == 0 || h == 0 {
        return;
    }
    let cart = match &o.model {
        Model::Verbs(m) => menu_list(m, w, h, sheet, gs),
        Model::Picker(p) => picker::picker_list(p, w, h, sheet, gs),
        Model::Dialog(d) => dialog::dialog_list(d, w, h, sheet, gs),
        Model::Help(hp) => help::help_list(hp, w, h, sheet, gs),
    };
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
        say(&format!("halcyond: menu {} present failed {:?}", o.surf.id, e));
    }
    let _ = rc;
}
