// railset -- the top rail's surface: the syscalling half of
// `halcyond::rail` (HALCYON-INSTRUMENT 8 / 8.1 / 8.3 / 14.1). ONE
// Role::Rail surface on the owner's ring, minted once the display is up
// under the Instrument profile (under legacy no rail exists and none is
// asked for), the display width by the profile's `rail_h`; the compositor
// places it at the top strip its carve always reserves. Painted whole from
// the lib's list whenever its model or its pointer state changes, and on
// the compositor's CONFIGURE.
//
// A POINTER TARGET like a header (9.1): the compositor routes MOVE, BTN and
// LEAVE to it; this pump keeps the hover and the pressed button, repaints
// for them in place, and turns a primary press into a `RailAction` for the
// owner to act on under its own authority -- the split twins, the reset,
// the workspace list; the picker and the help say what they are not yet.

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use halcyond::layout::Sheet;
use halcyond::rail::{rail_hit, rail_list, RailHit, RailInk, RailModel, RailZones};
use halcyond::raster::GlyphSource;
use libhalcyon::instrument::Profile;
use halcyond::picker;
use halcyond::rail::NARROW_W;
use tapestry::{
    EventRing, Surface, TapError, TEV_CHORD, TEV_CLOSE, TEV_CONFIGURE, TEV_PTR_BTN, TEV_PTR_LEAVE,
    TEV_PTR_MOVE,
};

/// evdev BTN_LEFT (the tapestry PTR_BTN `code`).
const BTN_LEFT: u16 = 0x110;

fn say(s: &str) {
    let mut t = String::from(s);
    t.push('\n');
    let _ = libthyla_rs::t_putstr(&t);
}

/// A primary press on the rail (8.1 / 14.1), for the owner.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RailAction {
    /// The split chords' pointer twins (9.5).
    SplitH,
    SplitV,
    /// The theme control (the picker, I-7): the display-point anchor for the
    /// picker's top-left (the control's right edge minus the picker width).
    Theme { x: u32, y: u32 },
    /// Reset (9.5).
    Reset,
    /// The keyboard reference (the help modal, I-7b).
    Help,
    /// The structural close chord (Super+Q) the compositor DELIVERED here
    /// rather than acting on it (9.5 / 14.5, I-7b), carrying the pane it
    /// says is focused. The owner asks first when that tile's job is
    /// running, then closes by verb under its own authority -- without 6.5's
    /// final-tile protection, which Super+Q deliberately does not carry.
    CloseFocused(u32),
    /// The mark: the workspace list, at display point (x, y).
    Workspaces { x: u32, y: u32 },
    /// A chip: switch to workspace NUMBER `n` (S4 -- an identity, not a
    /// position, so the handler can hand it straight to the compositor's
    /// `workspace` verb without knowing where the chip sat).
    Workspace(u8),
    /// The ‹ / › reveal: scroll the chips by one.
    ChipsScroll(i8),
}

pub struct RailBar {
    ring: EventRing,
    surf: Option<Surface>,
    /// What was painted last (model + pointer state): a repaint happens
    /// only on a change.
    painted: Option<(RailModel, RailInk)>,
    /// The zones of the last paint: the hit test's input.
    zones: RailZones,
    /// The workspace NUMBERS of the last paint, set in the SAME place as
    /// `zones` so the two cannot disagree (r2 F6). The chip arm resolved its
    /// number through `painted` instead, which a CONFIGURE nulls (and a
    /// dropped present never restores) while `zones` stays populated: the
    /// press then hit-tested fine and resolved to nothing, so it was silently
    /// swallowed. A hit and its meaning must come from one paint.
    painted_ws: Vec<u8>,
    /// The zones last said (test builds), keyed on the targets only.
    said_zones: Option<RailZones>,
    failed_said: bool,
    /// The mint arm (the status bar's cadence: `rearm` per pass).
    want_mint: bool,
    /// The pointer's last surface position; None once it left.
    hover: Option<(i32, i32)>,
    ink: RailInk,
    actions: Vec<RailAction>,
}

impl RailBar {
    pub fn new(ring: EventRing) -> RailBar {
        RailBar {
            ring,
            surf: None,
            painted: None,
            zones: RailZones::default(),
            painted_ws: Vec::new(),
            said_zones: None,
            failed_said: false,
            want_mint: true,
            hover: None,
            ink: RailInk::default(),
            actions: Vec::new(),
        }
    }

    /// The actions the pump collected since the last take.
    pub fn take_actions(&mut self) -> Vec<RailAction> {
        core::mem::take(&mut self.actions)
    }

    /// The picker's anchor (9.4): the theme control's right edge minus the
    /// picker width, at the rail's bottom + 5 (the compositor clamps into
    /// the display); +44 at a narrow display (8.3). The same action a press
    /// on the control and a Super+T chord both produce.
    fn theme_anchor(zones: &RailZones, sw: i32, sh: i32, sheet: &Sheet) -> RailAction {
        let pw = sheet.ipx(picker::WIDTH);
        let (tx, tw) = zones
            .buttons
            .iter()
            .find(|(h, _)| *h == RailHit::Theme)
            .map(|(_, b)| (b.0, b.2))
            .unwrap_or((0, 0));
        let mut x = (tx + tw - pw).max(0);
        if sw > 0 && sw <= sheet.ipx(NARROW_W) {
            x += sheet.ipx(44);
        }
        let y = sh + sheet.ipx(5);
        RailAction::Theme {
            x: x.max(0) as u32,
            y: y.max(0) as u32,
        }
    }

    /// Re-arm the mint retry (free while the rail is up).
    pub fn rearm(&mut self) {
        if self.surf.is_none() {
            self.want_mint = true;
        }
    }

    /// Mint the rail if there is none and the profile has one: the display
    /// width by the carve's `rail_h` at the sheet's scale. Said once on a
    /// refusal; retried per re-arm.
    pub fn ensure(&mut self, sheet: &Sheet) {
        if self.surf.is_some() || !self.want_mint || sheet.profile != Profile::Instrument {
            return;
        }
        self.want_mint = false;
        let (dw, _) = match self.ring.display_dims() {
            Some(d) => d,
            None => return,
        };
        let h = sheet.metrics.rail_h.max(0) as u32;
        if h == 0 {
            return;
        }
        match Surface::rail_on(&self.ring, dw, h) {
            Ok(s) => {
                #[cfg(feature = "test-mode")]
                say(&format!("halcyond: rail {} minted ({}x{})", s.id, dw, h));
                self.surf = Some(s);
                self.painted = None;
                self.said_zones = None;
                self.failed_said = false;
                self.hover = None;
                self.ink = RailInk::default();
            }
            Err(e) => {
                if !self.failed_said {
                    self.failed_said = true;
                    say(&format!("halcyond: rail failed {:?}", e));
                }
            }
        }
    }

    /// Drain the rail's events (non-blocking): a CONFIGURE (the redraw
    /// request; a resize offer reweaves) forces the next repaint and is
    /// reported (true); a CLOSE or a dead stream drops the surface
    /// (re-minted by the next ensure); MOVE / LEAVE keep the hover and a
    /// BTN the pressed button, each repainting in place; a primary press
    /// becomes a `RailAction`.
    pub fn pump(&mut self, sheet: &Sheet, gs: &mut GlyphSource) -> bool {
        let mut dead = false;
        let mut configured = false;
        let mut repaint = false;
        if let Some(surf) = self.surf.as_mut() {
            loop {
                match surf.poll_event() {
                    Ok(Some(e)) => match e.kind {
                        TEV_CONFIGURE => match surf.handle_configure(&e) {
                            Ok(_) => {
                                configured = true;
                                self.painted = None;
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
                        TEV_PTR_MOVE => {
                            let p = ((e.value >> 16) as u16 as i32, (e.value & 0xffff) as u16 as i32);
                            self.hover = Some(p);
                            let hit = rail_hit(&self.zones, p.0, p.1);
                            if self.ink.hover != hit {
                                self.ink.hover = hit;
                                repaint = true;
                            }
                        }
                        TEV_PTR_LEAVE => {
                            self.hover = None;
                            if self.ink != RailInk::default() {
                                self.ink = RailInk::default();
                                repaint = true;
                            }
                        }
                        TEV_PTR_BTN if e.code == BTN_LEFT => {
                            if e.value == 1 {
                                // A press with no known position is not a press (r1 B-F7).
                                let Some((x, y)) = self.hover else { continue };
                                let hit = rail_hit(&self.zones, x, y);
                                self.ink.pressed = hit;
                                let action = match hit {
                                    // Surface coordinates ARE display coordinates
                                    // here: the compositor places the rail at the
                                    // display's origin (`rail_rect` = 0,0,W,rail_h)
                                    // (r1 B-F12).
                                    Some(RailHit::Brand) => Some(RailAction::Workspaces {
                                        x: x.max(0) as u32,
                                        y: y.max(0) as u32,
                                    }),
                                    // S4: the hit is a POSITION among the
                                    // painted chips; the action carries the
                                    // NUMBER that chip stands for. A sparse
                                    // set makes these differ, and sending the
                                    // position would switch to the wrong
                                    // workspace.
                                    Some(RailHit::Chip(n)) => self
                                        .painted_ws
                                        .get(n as usize)
                                        .copied()
                                        .map(RailAction::Workspace),
                                    Some(RailHit::ChipsPrev) => Some(RailAction::ChipsScroll(-1)),
                                    Some(RailHit::ChipsNext) => Some(RailAction::ChipsScroll(1)),
                                    Some(RailHit::SplitH) => Some(RailAction::SplitH),
                                    Some(RailHit::SplitV) => Some(RailAction::SplitV),
                                    Some(RailHit::Theme) => Some(Self::theme_anchor(&self.zones, surf.w as i32, surf.h as i32, sheet)),
                                    Some(RailHit::Reset) => Some(RailAction::Reset),
                                    Some(RailHit::Help) => Some(RailAction::Help),
                                    None => None,
                                };
                                if let Some(a) = action {
                                    #[cfg(feature = "test-mode")]
                                    say(&format!("halcyond: rail press {:?}", a));
                                    self.actions.push(a);
                                }
                            } else {
                                self.ink.pressed = None;
                            }
                            repaint = true;
                        }
                        // HALCYON-INSTRUMENT 9.3 (I-7 / I-7b): a chord the
                        // compositor delivered here (TEV_CHORD; code 1 =
                        // picker, 2 = help, 3 = close the focused pane, whose
                        // id rides in `value`) -- for 1 and 2 the same action
                        // a press on the control produces, so the owner opens
                        // at the same anchor. An unknown code is ignored.
                        TEV_CHORD => {
                            let a = match e.code {
                                1 => Some(Self::theme_anchor(&self.zones, surf.w as i32, surf.h as i32, sheet)),
                                2 => Some(RailAction::Help),
                                3 => Some(RailAction::CloseFocused(e.value)),
                                _ => None,
                            };
                            if let Some(a) = a {
                                #[cfg(feature = "test-mode")]
                                say(&format!("halcyond: rail chord {:?}", a));
                                self.actions.push(a);
                            }
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
        }
        if dead {
            self.surf = None; // Drop: destroy + leave + fds
            self.painted = None;
            self.want_mint = true;
            self.hover = None;
            self.ink = RailInk::default();
            say("halcyond: rail closed by the compositor");
        } else if repaint {
            // A pointer change alone: repaint the last model now.
            if let Some((m, _)) = self.painted.take() {
                self.paint(&m, sheet, gs);
            }
        }
        configured
    }

    /// The next `refresh` repaints whatever the model (a sheet change).
    pub fn invalidate(&mut self) {
        self.painted = None;
    }

    /// Paint `model` if it (or the pointer state) differs from what is showing.
    pub fn refresh(&mut self, model: &RailModel, sheet: &Sheet, gs: &mut GlyphSource) {
        if self.painted.as_ref() == Some(&(model.clone(), self.ink)) {
            return;
        }
        self.paint(model, sheet, gs);
    }

    fn paint(&mut self, model: &RailModel, sheet: &Sheet, gs: &mut GlyphSource) {
        let ink = self.ink;
        let surf = match self.surf.as_mut() {
            Some(s) => s,
            None => return,
        };
        let (w, h) = (surf.w, surf.h);
        if w == 0 || h == 0 {
            return;
        }
        let (cart, zones) = rail_list(model, ink, w, h, sheet, gs);
        let px = surf.pixels();
        cartoon::execute(
            &cart,
            &gs.packer.store,
            &cartoon::BlobStore::new(),
            px,
            w as usize,
            None,
        );
        match surf.present(None) {
            Ok(()) => {
                #[cfg(feature = "test-mode")]
                {
                    let key = zones.stable();
                    if self.said_zones.as_ref() != Some(&key) {
                        let b = |hit: RailHit| -> String {
                            zones
                                .buttons
                                .iter()
                                .find(|x| x.0 == hit)
                                .map(|x| format!("[{} {} {} {}]", x.1 .0, x.1 .1, x.1 .2, x.1 .3))
                                .unwrap_or_else(|| String::from("[]"))
                        };
                        say(&format!(
                            "halcyond: rail {} painted brand [{} {} {} {}] ctx [{} {}] splith {} splitv {} theme {} reset {} help {} clock [{} {}]",
                            surf.id,
                            zones.brand.0, zones.brand.1, zones.brand.2, zones.brand.3,
                            zones.ctx.0, zones.ctx.1,
                            b(RailHit::SplitH), b(RailHit::SplitV), b(RailHit::Theme),
                            b(RailHit::Reset), b(RailHit::Help),
                            zones.clock.0, zones.clock.1
                        ));
                        self.said_zones = Some(key);
                    }
                }
                self.zones = zones;
                self.painted_ws = model.workspaces.clone();
                self.painted = Some((model.clone(), ink));
            }
            Err(_) => {
                // A dropped frame, never death: the next change repaints.
            }
        }
    }
}
