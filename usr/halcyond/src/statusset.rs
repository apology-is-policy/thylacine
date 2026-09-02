// statusset -- the status bar's surface: the syscalling half of
// `halcyond::status` (HALCYON.md 13.6 H-3d; Daylight section 6). ONE
// Role::Status surface on the renderer's ring, minted once the console is
// up (first-present-wins scanout: chrome never precedes the console), the
// display width by the one vertical unit; the compositor carves the display
// for it and places it at the bottom strip. Painted whole from the lib's
// list whenever its model changes -- a relayout or a focus move (the pane
// tree), the console's directory or command (the transcript), the focused
// pane's status, the minute -- and on the compositor's CONFIGURE (a redraw
// request, or a resize offer on a display change).

use alloc::format;
use alloc::string::String;

use halcyond::raster::GlyphSource;
use halcyond::status::{bar_height, condition_for, status_list, StatusModel};
use libthyla_rs::{t_clock_gettime, T_CLOCK_REALTIME};
use tapestry::{EventRing, Surface, TapError, TEV_CLOSE, TEV_CONFIGURE};

fn say(s: &str) {
    let mut t = String::from(s);
    t.push('\n');
    let _ = libthyla_rs::t_putstr(&t);
}

/// The wall clock's hour and minute (UTC: the RTC's own zone; no zone
/// database on the device yet).
pub fn clock_hm() -> (u8, u8) {
    let mut ts = [0i64; 2];
    let rc = unsafe { t_clock_gettime(T_CLOCK_REALTIME, ts.as_mut_ptr() as u64) };
    if rc < 0 || ts[0] < 0 {
        return (0, 0);
    }
    let secs = ts[0] as u64;
    (((secs / 3600) % 24) as u8, ((secs / 60) % 60) as u8)
}

pub struct StatusBar {
    ring: EventRing,
    surf: Option<Surface>,
    /// The model last painted (a repaint happens only on a change).
    painted: Option<StatusModel>,
    /// The slot geometry last said (test builds): the witness needs the
    /// rects when they MOVE, and every say line lands in the transcript
    /// (the observer effect) -- said per change of geometry, never per
    /// paint, so the row-relative legs after a command see no extra row.
    said_slots: Option<halcyond::status::Slots>,
    failed_said: bool,
    /// Whether a mint should be attempted: true at start and after a CLOSE
    /// (the compositor dropped the bar), cleared by each attempt. A FAILED
    /// mint waits for the next `rearm` (a relayout) before retrying, so a
    /// persistent failure costs one attempt per relayout -- ChromeSet's
    /// cadence -- not two sync RPCs every pass (the H-3d round F5).
    want_mint: bool,
}

impl StatusBar {
    pub fn new(ring: EventRing) -> StatusBar {
        StatusBar {
            ring,
            surf: None,
            painted: None,
            said_slots: None,
            failed_said: false,
            want_mint: true,
        }
    }

    /// Re-arm the mint retry: called on a relayout (the compositor/display
    /// state changed, so a prior failure may now succeed) -- the retry
    /// cadence ChromeSet gets for free from reconcile. A no-op once the bar
    /// is up.
    pub fn rearm(&mut self) {
        if self.surf.is_none() {
            self.want_mint = true;
        }
    }

    /// Mint the bar if there is none: the display width (off the ring's
    /// `ctl`) by the bar height. Said once on a refusal; retried per call.
    pub fn ensure(&mut self) {
        if self.surf.is_some() || !self.want_mint {
            return;
        }
        self.want_mint = false; // this attempt consumes the arm (rearm on a relayout)
        let (dw, _) = match self.ring.display_dims() {
            Some(d) => d,
            None => return,
        };
        match Surface::status_on(&self.ring, dw, bar_height()) {
            Ok(s) => {
                #[cfg(feature = "test-mode")]
                say(&format!("halcyond: status bar {} minted ({}x{})", s.id, dw, bar_height()));
                self.surf = Some(s);
                self.painted = None;
                self.said_slots = None;
                self.failed_said = false;
            }
            Err(e) => {
                if !self.failed_said {
                    self.failed_said = true;
                    say(&format!("halcyond: status bar failed {:?}", e));
                }
            }
        }
    }

    /// Drain the bar's events (non-blocking): a CONFIGURE (the redraw
    /// request; a resize offer reweaves) forces the next repaint; a CLOSE
    /// or a dead stream drops the surface (re-minted by the next ensure).
    pub fn pump(&mut self) {
        let mut dead = false;
        let mut repaint = false;
        if let Some(surf) = self.surf.as_mut() {
            loop {
                match surf.poll_event() {
                    Ok(Some(e)) => match e.kind {
                        TEV_CONFIGURE => match surf.handle_configure(&e) {
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
            self.want_mint = true; // a CLOSE is itself the re-mint signal
            say("halcyond: status bar closed by the compositor");
        } else if repaint {
            self.painted = None;
        }
    }

    /// Paint `model` if it differs from what is showing.
    pub fn refresh(&mut self, model: &StatusModel, gs: &mut GlyphSource) {
        if self.painted.as_ref() == Some(model) {
            return;
        }
        let surf = match self.surf.as_mut() {
            Some(s) => s,
            None => return,
        };
        let (w, h) = (surf.w, surf.h);
        if w == 0 || h == 0 {
            return;
        }
        let (cart, slots) = status_list(model, w, h, gs);
        let px = surf.pixels();
        cartoon::execute(&cart, &gs.packer.store, &cartoon::BlobStore::new(), px, w as usize, None);
        match surf.present(None) {
            Ok(()) => {
                #[cfg(feature = "test-mode")]
                if self.said_slots != Some(slots) {
                    self.said_slots = Some(slots);
                    say(&format!(
                    "halcyond: status bar {} painted ws [{} {}] ctx [{} {}] cond [{} {}] clock [{} {}] context \"{}\" condition {:?} clock {:02}:{:02}",
                    surf.id,
                    slots.ws.0, slots.ws.1, slots.ctx.0, slots.ctx.1, slots.cond.0, slots.cond.1,
                    slots.clock.0, slots.clock.1,
                    halcyond::status::context_text(&model.name, &model.cwd, &model.cmd),
                    model.condition, model.hour, model.minute
                    ));
                }
                let _ = slots;
                self.painted = Some(model.clone());
            }
            Err(_) => {
                // A dropped frame, never death: the next change repaints.
            }
        }
    }
}

/// The model from the sources: the focused leaf (pane id, name, status),
/// whether that leaf hosts the console (then the transcript's directory and
/// command apply), and the clock.
pub fn model_from(
    focused: Option<&(u32, String, String)>,
    own_pane: Option<u32>,
    cwd: &str,
    cmd: Option<&str>,
) -> StatusModel {
    let mut m = StatusModel::empty();
    if let Some((id, name, status)) = focused {
        m.name = name.clone();
        m.condition = condition_for(status);
        if Some(*id) == own_pane {
            m.cwd = String::from(cwd);
            m.cmd = String::from(cmd.unwrap_or(""));
        }
    }
    let (h, mi) = clock_hm();
    m.hour = h;
    m.minute = mi;
    m
}
