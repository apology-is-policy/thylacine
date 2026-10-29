//! Physical input and the kernel-bound episode pump. This remains independent
//! of normal compositor RPC progress: a parked GPU request cannot stall SAK.
use alloc::vec::Vec;
use crate::{endpoint as ep, keymap::{self, Mods}, model::{Model, State}};
use super::{device::Device, screen::Screen, input::{InputDev, RawInputEvent, EV_KEY}};
#[cfg(feature = "test-mode")]
use super::device::SeatShot;

pub struct Input {
    pub device: InputDev,
    pub keyboard: bool,
    pub events: Vec<(u16, u16, u32)>,
    held: [bool; 768],
}
impl Input {
    pub fn new(device: InputDev) -> Self {
        let keyboard = !device.supports_abs() && !device.supports_rel();
        Self { device, keyboard, events: Vec::new(), held: [false; 768] }
    }
}
#[derive(Default)]
pub struct Seat {
    pub phase: u32,
    pub generation: u64,
    quiescence: crate::quiescence::Gate,
    mods: Mods,
    /// The generation whose trusted scanout is up. Keyed, so state an episode
    /// left behind can never stand in for a later takeover's exclusion.
    active: Option<u64>,
    sequence: u64,
    masked: u32,
    model: Model,
    failed_since: Option<libthyla_rs::time::Instant>,
    /// A takeover began, so normal output may not be showing: owed a restore
    /// until the device completes one or its engine dies.
    displaced: bool,
    /// The engine died with the normal output displaced. A trusted frame may
    /// stay frozen on the glass, so input never reaches the compositor again
    /// this boot (TRUSTED-PATH section 7, vote 30).
    withheld: bool,
    withheld_said: bool,
    #[cfg(feature = "test-mode")]
    test_refuse_restored: bool,
}
/// How long the failure notice stays up before normal output returns.
const FAILURE_NOTICE_MS: u128 = 1500;
impl Seat {
    fn active(&self) -> bool { self.active == Some(self.generation) }
    pub fn acknowledge_cancellation(&mut self, generation: u64) -> bool {
        self.phase == 1 && self.generation == generation && self.quiescence.acknowledge(generation)
    }

    pub fn input(&mut self, inputs: &mut [Input]) {
        for input in inputs.iter_mut() {
            let mut events: Vec<RawInputEvent> = Vec::new();
            input.device.drain(|event| events.push(event));
            for event in events {
                if event.etype == EV_KEY && (event.code as usize) < input.held.len() {
                    input.held[event.code as usize] = event.value != 0;
                }
                if input.keyboard && event.etype == EV_KEY && event.code < 256 {
                    self.mods.update(event.code, event.value != 0);
                    let mut m = ep::Message::default();
                    m.code = event.code as u32; m.value = event.value;
                    if event.value == 1 {
                        if let Some(ch) = char::from_u32(keymap::resolve(event.code, self.mods.mask())) {
                            if ch != '\0' { m.length = ch.encode_utf8(&mut m.data[..4]).len() as u32; }
                        }
                    }
                    if ep::call(ep::INPUT, &mut m).is_err() { self.phase = 4; }
                    else { self.phase = m.phase; self.generation = m.generation; }
                }
                if self.phase == 0 && self.withheld {
                    if !core::mem::replace(&mut self.withheld_said, true) {
                        super::diagnostic("lictor: input withheld from the compositor (display lost in a trusted episode)\n");
                    }
                } else if self.phase == 0 {
                    if input.events.len() < 256 { input.events.push((event.etype, event.code, event.value)); }
                    else {
                        // Losing a release could stick a key. Drop the batch
                        // and report SYN_DROPPED, which forces an input reset.
                        input.events.clear(); input.events.push((0, 3, 0));
                    }
                }
            }
        }
        if self.phase != 0 { for input in inputs { input.events.clear(); } }
    }
    fn fail(&mut self, why: &str) {
        self.phase = 4;
        let mut m = ep::Message::default(); m.generation = self.generation;
        let _ = ep::call(ep::FAIL, &mut m);
        super::diagnostic(&alloc::format!("lictor: seat failed: {} generation={}\n", why, self.generation));
    }
    pub fn step(&mut self, device: &mut Device, screen: &mut Screen, inputs: &mut [Input]) {
        let mut m = ep::Message::default();
        if ep::call(ep::STATUS, &mut m).is_err() { self.fail("status"); return; }
        self.phase = m.phase; self.generation = m.generation;
        self.quiescence.observe(self.generation, self.phase);
        let released = inputs.iter().all(|i| i.held.iter().all(|held| !held));
        match self.phase {
            0 => {},
            1 => {
                for input in inputs.iter_mut() { input.events.clear(); }
                device.reap();
                if !device.gpu.all_work_retired() { return; }
                if !self.active() {
                    self.model = Model { notice: b"Preparing the trusted path.".to_vec(), ..Model::default() };
                    if screen.paint(&mut device.gpu, &self.model, 0, None).is_err() {
                        self.fail("private scanout activation"); return;
                    }
                    #[cfg(feature = "test-mode")]
                    let shot = device.test_latch_seat_shot();
                    #[cfg(feature = "test-mode")]
                    {
                        screen.test_stall_after_exclusion = shot == Some(SeatShot::StallAfterExclusion);
                        device.gpu.test_cursor_stall_next(shot == Some(SeatShot::CursorStall));
                        self.test_refuse_restored = shot == Some(SeatShot::RefuseRestored);
                    }
                    // A takeover that fails part-way may already have disabled
                    // outputs or bound the trusted image.
                    self.displaced = true;
                    let activated = screen.activate(&mut device.gpu);
                    #[cfg(feature = "test-mode")]
                    device.gpu.test_cursor_stall_next(false);
                    if activated.is_err() { self.fail("private scanout activation"); return; }
                    self.active = Some(self.generation); self.sequence = 0; self.masked = 0;
                    super::diagnostic(&alloc::format!("lictor: trusted scanout active generation={}\n", self.generation));
                    #[cfg(feature = "test-mode")]
                    if shot == Some(SeatShot::StallHeld) { device.gpu.test_latch_now(true); }
                }
                if released && self.quiescence.ready(self.generation) {
                    self.mods = Mods::default();
                    if ep::call(ep::ACK, &mut m).is_err() { self.fail("quiescence acknowledgement"); }
                    // ACK intentionally doesn't return a status envelope. Read
                    // it on the next pass; no local guess enables secret input.
                }
            }
            2 => {
                if !self.active() { self.fail("exclusive without active scanout"); return; }
                if m.sequence != self.sequence || m.code != self.masked {
                    let model = match Model::decode(&m.data[..m.length as usize]) {
                        Ok(model) => model, Err(_) => { self.fail("frame decode"); return; }
                    };
                    if screen.paint(&mut device.gpu, &model, m.code as usize, None).is_err() {
                        self.fail("private frame paint"); return;
                    }
                    if ep::call(ep::VISIBLE, &mut m).is_err() { self.fail("visibility acknowledgement"); return; }
                    self.model = model; self.sequence = m.sequence; self.masked = m.code;
                }
            }
            3 => {
                for input in inputs.iter_mut() { input.events.clear(); }
                if !released { return; }
                if device.restore().is_err() { self.fail("normal scanout restoration"); return; }
                screen.deactivated();
                // The trusted scanout is down; restoration stays owed until
                // the kernel acknowledges it.
                self.active = None;
                self.mods = Mods::default(); self.model = Model::default(); self.masked = 0;
                #[cfg(feature = "test-mode")]
                let refused = core::mem::take(&mut self.test_refuse_restored);
                #[cfg(not(feature = "test-mode"))]
                let refused = false;
                if refused { super::diagnostic("lictor: RESTORED refused (INJECTED by test-latch-dead)\n"); }
                // A refused RESTORED leaves restoration owed: the FAILED arm
                // rebinds again before the seat may return.
                if refused || ep::call(ep::RESTORED, &mut m).is_err() { self.fail("restoration acknowledgement"); return; }
                self.phase = m.phase; self.displaced = false;
            }
            _ => {
                // The kernel already cancelled any grant and scrubbed the key
                // queue, so what is left is giving the workspace back. Say
                // what happened first, then restore and tell the kernel; a
                // refused restore (a stalled engine) is retried every pass and
                // confers nothing in the meantime. A dead engine can restore
                // nothing, so the seat goes back without its display.
                for input in inputs.iter_mut() { input.events.clear(); }
                let since = *self.failed_since.get_or_insert_with(|| {
                    super::diagnostic(&alloc::format!("lictor: kernel closed trusted seat (scanout {})\n",
                        if self.active == Some(self.generation) { "private" }
                        else if self.displaced { "displaced" } else { "normal" }));
                    if self.active == Some(self.generation) {
                        let model = Model { state: State::Failed, ..Model::default() };
                        let _ = screen.paint(&mut device.gpu, &model, 0, None);
                    }
                    libthyla_rs::time::Instant::now()
                });
                if !released || (self.active() && since.elapsed().as_millis() < FAILURE_NOTICE_MS) { return; }
                if self.displaced || self.active.is_some() {
                    if device.gpu.engine_dead() {
                        // Nothing can redraw the glass, and keys typed into a
                        // frozen trusted frame must reach no one.
                        self.withheld = true;
                        super::diagnostic(&alloc::format!(
                            "lictor: GPU engine dead with normal output displaced -- the seat returns \
                             without its display; input withheld for the rest of the boot generation={}\n",
                            self.generation));
                    } else if device.restore().is_err() {
                        return;
                    } else {
                        super::diagnostic(&alloc::format!(
                            "lictor: normal scanout restored generation={}\n", self.generation));
                        #[cfg(feature = "test-mode")]
                        device.gpu.test_cursor_release();
                    }
                    screen.deactivated();
                    self.active = None;
                    self.displaced = false;
                }
                self.mods = Mods::default(); self.model = Model::default(); self.masked = 0;
                if ep::call(ep::RESTORED, &mut m).is_ok() {
                    self.phase = m.phase; self.failed_since = None;
                    super::diagnostic("lictor: trusted seat recovered; nothing was conferred\n");
                }
            }
        }
    }
}
