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
    /// When this generation's takeover first reached the device, and how many fenced
    /// chains stood on it then: what the takeover line reports.
    takeover_began: Option<(u64, u32, libthyla_rs::time::Instant)>,
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
    /// A held stall ends when the kernel closes the episode it held up.
    #[cfg(feature = "test-mode")]
    test_release_on_failure: bool,
    #[cfg(feature = "test-mode")]
    test_stall_frame: bool,
    /// What the frame's shot did. This process says nothing in an open episode
    /// (`diagnostic`), so it says so afterwards.
    #[cfg(feature = "test-mode")]
    test_frame_stalled: Option<bool>,
    /// How many paints of a frame failed, and whether VISIBLE ever went out over one.
    #[cfg(feature = "test-mode")]
    test_frame_retries: u32,
    #[cfg(feature = "test-mode")]
    test_frame_unpainted: bool,
    /// Keep the next ordinary return's restore waiting, with the keys left in the ring.
    #[cfg(feature = "test-mode")]
    test_hold_restore: bool,
    /// The generation whose shot is taken: a takeover that starts over after a
    /// stall is the same takeover.
    #[cfg(feature = "test-mode")]
    test_shot_generation: Option<u64>,
    /// Taken before the first paint, which it may stall, and acted on after it.
    #[cfg(feature = "test-mode")]
    test_shot_due: Option<SeatShot>,
    /// The key that stops the serve loop was pressed; the armed lever decides.
    #[cfg(feature = "test-mode")]
    pub test_blind: bool,
}
/// The key that stops the serve loop under the armed lever: F8.
#[cfg(feature = "test-mode")]
const TEST_BLIND_KEY: u16 = 66;
#[cfg(feature = "test-mode")]
const TEST_HOLD_RESTORE_MS: u64 = 3000;
/// How long the failure notice stays up before normal output returns.
const FAILURE_NOTICE_MS: u128 = 1500;
impl Seat {
    fn active(&self) -> bool { self.active == Some(self.generation) }
    pub fn acknowledge_cancellation(&mut self, generation: u64) -> bool {
        self.phase == 1 && self.generation == generation && self.quiescence.acknowledge(generation)
    }

    pub fn input(&mut self, inputs: &mut [Input]) {
        let mut lost = false;
        for input in inputs.iter_mut() {
            let mut events: Vec<RawInputEvent> = Vec::new();
            let overran = input.device.drain(|event| events.push(event));
            for event in events { self.deliver(input, event, &mut lost); }
            // A release may be among what the device dropped, and it cannot be asked
            // what is down. A release of a key still down costs one press. A release that
            // never comes parks the seat: this side withholds its ACK and the kernel its
            // restore, on every chord until that key is pressed again.
            if overran {
                let mut released = 0;
                for code in 0..input.held.len() {
                    if input.held[code] {
                        self.deliver(input, RawInputEvent { etype: EV_KEY, code: code as u16, value: 0 }, &mut lost);
                        released += 1;
                    }
                }
                if released != 0 {
                    super::diagnostic(&alloc::format!("lictor: input ring overran, {} held keys released\n", released));
                }
            }
        }
        // A refused chord moves no generation, so nothing else tells the compositor
        // that a release it was owed is gone.
        if self.phase != 0 || lost {
            for input in inputs {
                input.events.clear();
                if lost && !self.withheld { input.events.push((0, 3, 0)); }
            }
        }
    }
    // Whatever is in the rings as the seat returns was pressed at a trusted screen. The
    // kernel and `held` still read it; the compositor must not.
    fn drain_episode_tail(&mut self, inputs: &mut [Input]) {
        self.input(inputs);
        let mut kept_back = 0;
        for input in inputs {
            let before = input.events.len();
            input.events.retain(|event| *event == (0, 3, 0));
            kept_back += before - input.events.len();
        }
        if kept_back != 0 {
            super::diagnostic(&alloc::format!(
                "lictor: {} input events pressed at a trusted screen kept from the compositor\n", kept_back));
        }
    }
    fn deliver(&mut self, input: &mut Input, event: RawInputEvent, lost: &mut bool) {
        #[cfg(feature = "test-mode")]
        if event.etype == EV_KEY && event.code == TEST_BLIND_KEY && event.value == 1 { self.test_blind = true; }
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
            if ep::call(ep::INPUT, &mut m).is_err() { self.phase = 4; *lost = true; }
            else { self.adopt(&m); }
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
    // SeatState serves this pair, so a reply is taken whole: a recovery moves the
    // generation, and a new phase with the old one stales a client's Join.
    fn adopt(&mut self, m: &ep::Message) {
        self.phase = m.phase;
        self.generation = m.generation;
    }
    fn fail(&mut self, why: &str) {
        self.phase = 4;
        let mut m = ep::Message::default(); m.generation = self.generation;
        let _ = ep::call(ep::FAIL, &mut m);
        super::diagnostic(&alloc::format!("lictor: seat failed: {} generation={}\n", why, self.generation));
    }
    // A stall is other work ahead of ours on the device, not an answer from it: the
    // chain retires, the engine revives and the next pass starts over. The kernel's
    // deadline bounds that.
    fn takeover_refused(&mut self, device: &Device) {
        if device.gpu.stall_pending() {
            super::diagnostic(&alloc::format!(
                "lictor: takeover met a stall, starting over generation={}\n", self.generation));
        } else {
            self.fail("private scanout activation");
        }
    }
    pub fn step(&mut self, device: &mut Device, screen: &mut Screen, inputs: &mut [Input]) {
        let mut m = ep::Message::default();
        if ep::call(ep::STATUS, &mut m).is_err() { self.fail("status"); return; }
        self.adopt(&m);
        #[cfg(feature = "test-mode")]
        if self.phase != 2 {
            if let Some(stalled) = self.test_frame_stalled.take() {
                super::diagnostic(&alloc::format!(
                    "lictor: frame stall shot: stalled={} retries={} unpainted={} generation={}\n",
                    stalled as u8, core::mem::take(&mut self.test_frame_retries),
                    core::mem::take(&mut self.test_frame_unpainted) as u8, self.generation));
            }
        }
        self.quiescence.observe(self.generation, self.phase);
        device.gpu.seat_bound(self.phase != 0);
        let released = inputs.iter().all(|i| i.held.iter().all(|held| !held));
        match self.phase {
            0 => {},
            1 => {
                for input in inputs.iter_mut() { input.events.clear(); }
                device.reap();
                if !device.gpu.takeover_clear() { return; }
                if !self.active() {
                    if self.takeover_began.is_none_or(|(generation, ..)| generation != self.generation) {
                        self.takeover_began = Some((self.generation, device.gpu.fenced_in_flight(),
                                                    libthyla_rs::time::Instant::now()));
                    }
                    self.model = Model { notice: b"Preparing the trusted path.".to_vec(), ..Model::default() };
                    #[cfg(feature = "test-mode")]
                    if self.test_shot_generation != Some(self.generation) {
                        self.test_shot_generation = Some(self.generation);
                        self.test_shot_due = device.test_latch_seat_shot();
                        if self.test_shot_due == Some(SeatShot::StallInPaint) { device.gpu.test_latch_now(false); }
                    }
                    if screen.paint(&mut device.gpu, &self.model, 0, None).is_err() {
                        self.takeover_refused(device);
                        return;
                    }
                    #[cfg(feature = "test-mode")]
                    let shot = self.test_shot_due.take();
                    #[cfg(feature = "test-mode")]
                    {
                        screen.test_stall_after_exclusion = shot == Some(SeatShot::StallAfterExclusion);
                        screen.test_hold_after_exclusion = shot == Some(SeatShot::HoldAfterExclusion);
                        screen.test_refuse_bind = shot == Some(SeatShot::RefuseBind);
                        device.gpu.test_cursor_stall_next(matches!(shot, Some(SeatShot::CursorStall | SeatShot::CursorAbandon)));
                        if shot == Some(SeatShot::CursorAbandon) { device.gpu.test_cursor_abandon_next(); }
                        if shot == Some(SeatShot::BehindReadback) { device.gpu.test_behind_readback(); }
                        self.test_refuse_restored |= shot == Some(SeatShot::RefuseRestored);
                        self.test_release_on_failure |= shot == Some(SeatShot::HoldAfterExclusion);
                        self.test_stall_frame |= shot == Some(SeatShot::StallInFrame);
                        screen.test_refuse_frame = shot == Some(SeatShot::RefuseFrame);
                        self.test_hold_restore |= shot == Some(SeatShot::HoldRestore);
                    }
                    // A takeover that fails part-way may already have disabled
                    // outputs or bound the trusted image.
                    self.displaced = true;
                    let activated = screen.activate(&mut device.gpu);
                    #[cfg(feature = "test-mode")]
                    device.gpu.test_cursor_stall_next(false);
                    if activated.is_err() {
                        self.takeover_refused(device);
                        return;
                    }
                    self.active = Some(self.generation); self.sequence = 0; self.masked = 0;
                    let (chains, ms) = self.takeover_began.take()
                        .map_or((0, 0), |(_, chains, t0)| (chains, t0.elapsed().as_millis()));
                    super::diagnostic(&alloc::format!(
                        "lictor: trusted scanout active generation={} chains={} took_ms={}\n",
                        self.generation, chains, ms));
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
                    // Nothing can reach the device until the engine revives.
                    if device.gpu.stall_pending() { return; }
                    let model = match Model::decode(&m.data[..m.length as usize]) {
                        Ok(model) => model, Err(_) => { self.fail("frame decode"); return; }
                    };
                    #[cfg(feature = "test-mode")]
                    if core::mem::take(&mut self.test_stall_frame) {
                        self.test_frame_stalled = Some(device.gpu.test_latch_now(false));
                    }
                    // The frame stays owed, so the next pass paints it again. Corvus
                    // reads no key until it is visible, and bounds that wait.
                    let painted = screen.paint(&mut device.gpu, &model, m.code as usize, None);
                    #[cfg(feature = "test-mode")]
                    if painted.is_err() { self.test_frame_retries += 1; }
                    if painted.is_err() {
                        if !device.gpu.stall_pending() { self.fail("private frame paint"); }
                        return;
                    }
                    #[cfg(feature = "test-mode")]
                    { self.test_frame_unpainted |= painted.is_err(); }
                    // VISIBLE's reply is the seat's state now, and a dot Corvus added
                    // during the paint is not in this frame.
                    let masked = m.code;
                    if ep::call(ep::VISIBLE, &mut m).is_err() { self.fail("visibility acknowledgement"); return; }
                    self.model = model; self.sequence = m.sequence; self.masked = masked;
                }
            }
            3 => {
                for input in inputs.iter_mut() { input.events.clear(); }
                if !released { return; }
                #[cfg(feature = "test-mode")]
                if core::mem::take(&mut self.test_hold_restore) {
                    super::diagnostic("lictor: restore held (INJECTED by test-latch-dead)\n");
                    let _ = libthyla_rs::time::sleep(core::time::Duration::from_millis(TEST_HOLD_RESTORE_MS));
                }
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
                self.adopt(&m); self.displaced = false;
                self.drain_episode_tail(inputs);
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
                #[cfg(feature = "test-mode")]
                if core::mem::take(&mut self.test_release_on_failure) { device.gpu.test_stall_release(); }
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
                    self.adopt(&m); self.failed_since = None;
                    self.drain_episode_tail(inputs);
                    super::diagnostic("lictor: trusted seat recovered; nothing was conferred\n");
                }
            }
        }
    }
}
