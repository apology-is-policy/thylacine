//! Physical input and the kernel-bound episode pump. This remains independent
//! of normal compositor RPC progress: a parked GPU request cannot stall SAK.
use alloc::{collections::VecDeque, vec::Vec};
use crate::{endpoint as ep, keymap::{self, Mods}, model::{Model, State}};
use super::{device::Device, screen::Screen, input::{InputDev, RawInputEvent, Report, EV_KEY}, intake::{self, Entry}};
#[cfg(feature = "test-mode")]
use super::device::SeatShot;

/// One input device as the serve thread sees it. The device itself belongs to the input
/// thread (`intake`), so what InputInfo answers is read once, here: virtio-input's
/// configuration does not change.
pub struct Input {
    /// EV_ABS, EV_REL, and the X and Y maxima.
    pub info: (bool, bool, u32, u32),
    pub keyboard: bool,
    pub events: Vec<(u16, u16, u32)>,
    held: [bool; 768],
}
impl Input {
    pub fn new(device: &InputDev) -> Self {
        let info = (device.supports_abs(), device.supports_rel(), device.abs_max(0), device.abs_max(1));
        Self { info, keyboard: !info.0 && !info.1, events: Vec::new(), held: [false; 768] }
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
    /// The generation whose takeover deadline was last renewed, and when.
    renewed: Option<(u64, libthyla_rs::time::Instant)>,
    /// The generation whose takeover was reported held by a cursor lane out of service.
    lane_noted: Option<u64>,
    /// A generation's first sight in QUIESCING: the reply to the key that made the
    /// chord, so this is the kernel's own start to within one call.
    chord: Option<(u64, libthyla_rs::time::Instant)>,
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
    /// A withheld stall is let through this long after the shot, past the kernel's
    /// first deadline, or when the kernel closes the episode it held up, if sooner.
    #[cfg(feature = "test-mode")]
    test_release_at: Option<libthyla_rs::time::Instant>,
    /// The same for a held cursor chain (leg C), past the first deadline.
    #[cfg(feature = "test-mode")]
    test_cursor_release_at: Option<libthyla_rs::time::Instant>,
    /// A chained shot's second wait, armed when its first is released: the cursor lane
    /// held past every bound after the control queue, or the control queue withheld
    /// for good after the lane.
    #[cfg(feature = "test-mode")]
    test_then_cursor: bool,
    #[cfg(feature = "test-mode")]
    test_then_control: bool,
    /// The cursor half, released and due at the next paint that succeeds: the paint's
    /// shot block sets the cursor lever every pass, so it is armed there.
    #[cfg(feature = "test-mode")]
    test_cursor_due: bool,
    /// Once the takeover's bound has abandoned the lane, a stall withheld past it and
    /// the seat failed with it: no pump may then apply the bound to that stall.
    #[cfg(feature = "test-mode")]
    test_fail_late: bool,
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
    /// The serve thread's half of the intake swap, kept for its capacity.
    taken: VecDeque<(u8, Entry)>,
    /// The seat returned when the input thread had finished fewer passes than this: what it
    /// hands over until then was pressed at a trusted screen (`drain_episode_tail`).
    tail_until: Option<u64>,
}
#[cfg(feature = "test-mode")]
const TEST_HOLD_RESTORE_MS: u64 = 3000;
/// Past the kernel's five-second takeover deadline, well inside FENCE_ABANDON_MS.
#[cfg(feature = "test-mode")]
const TEST_RELEASE_AFTER_MS: u64 = 8000;
/// The chained late shot: past the takeover's bound, before the kernel's limit.
#[cfg(feature = "test-mode")]
const TEST_FAIL_LATE_MS: u64 = 34_000;
/// How long the failure notice stays up before normal output returns.
const FAILURE_NOTICE_MS: u128 = 1500;
impl Seat {
    fn active(&self) -> bool { self.active == Some(self.generation) }
    pub fn acknowledge_cancellation(&mut self, generation: u64) -> bool {
        self.phase == 1 && self.generation == generation && self.quiescence.acknowledge(generation)
    }

    pub fn input(&mut self, inputs: &mut [Input]) {
        let mut lost = false;
        let passes = intake::passes();
        let mut taken = core::mem::take(&mut self.taken);
        let dropped = intake::take(&mut taken);
        for (index, entry) in taken.drain(..) {
            let Some(input) = inputs.get_mut(index as usize) else { continue };
            match entry {
                Entry::Report(Report::Event(event)) => self.deliver(input, event, &mut lost),
                Entry::Report(Report::Low { free, depth, avail, used }) => super::diagnostic(&alloc::format!(
                    "tapestryd: input eventq LOW: {} free of {} (avail {} used {})\n", free, depth, avail, used)),
                Entry::Report(Report::Malformed { id, len }) => super::diagnostic(&alloc::format!(
                    "tapestryd: input malformed used elem id={} len={}\n", id, len)),
                Entry::Overran => self.release_held(input, "input ring", &mut lost),
            }
        }
        self.taken = taken;
        for (index, input) in inputs.iter_mut().enumerate() {
            if dropped.get(index).copied().unwrap_or(false) { self.release_held(input, "input queue", &mut lost); }
        }
        // Only a prefix is kept back, so a release may reach the compositor without its
        // press, never a press without its release.
        if let Some(until) = self.tail_until {
            let mut kept_back = 0;
            for input in inputs.iter_mut() {
                let before = input.events.len();
                input.events.retain(|event| *event == (0, 3, 0));
                kept_back += before - input.events.len();
            }
            if kept_back != 0 {
                super::diagnostic(&alloc::format!(
                    "lictor: {} input events pressed at a trusted screen kept from the compositor\n", kept_back));
            }
            if passes >= until { self.tail_until = None; }
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
    // A release may be among what was dropped, and the device cannot be asked what is
    // down. A release of a key still down costs one press. A release that never comes
    // parks the seat: this side withholds its ACK and the kernel its restore, on every
    // chord until that key is pressed again.
    fn release_held(&mut self, input: &mut Input, what: &str, lost: &mut bool) {
        let mut released = 0;
        for code in 0..input.held.len() {
            if input.held[code] {
                self.deliver(input, RawInputEvent { etype: EV_KEY, code: code as u16, value: 0 }, lost);
                released += 1;
            }
        }
        if released != 0 {
            super::diagnostic(&alloc::format!("lictor: {} overran, {} held keys released\n", what, released));
        }
    }
    // Whatever the devices had placed as the seat returns was pressed at a trusted screen,
    // and the input thread may not have moved it yet: it moves the rings every few ms, later
    // under load. The kernel and `held` still read it; the compositor must not, until a
    // pass begun after this point has been taken. A pass under way now ends at passes() + 1
    // and may have read its ring already, so the first pass begun after it ends at + 2.
    fn drain_episode_tail(&mut self, inputs: &mut [Input]) {
        self.tail_until = Some(intake::passes() + 2);
        self.input(inputs);
    }
    fn deliver(&mut self, input: &mut Input, event: RawInputEvent, lost: &mut bool) {
        #[cfg(feature = "test-mode")]
        if event.etype == EV_KEY && event.code == intake::test::BLIND_KEY && event.value == 1 { self.test_blind = true; }
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
        if m.phase == 1 && self.chord.is_none_or(|(generation, _)| generation != m.generation) {
            self.chord = Some((m.generation, libthyla_rs::time::Instant::now()));
        }
    }
    /// The chord of a takeover not yet up: what bounds every wait it makes on the device.
    fn waiting_chord(&self) -> Option<libthyla_rs::time::Instant> {
        if self.phase != 1 || self.active() { return None; }
        self.chord.filter(|(generation, _)| *generation == self.generation).map(|(_, at)| at)
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
    // A backlog ahead of the trusted paint delays the takeover (vote 71): while the
    // engine is alive and the device holds the takeover up, the kernel's
    // deadline is renewed, at once when first seen and then at most once a second,
    // never past the limit the chord set. One chain that runs for seconds retires
    // nothing meanwhile and is renewed all the same; a chain that never retires kills
    // the engine at FENCE_ABANDON_MS, inside that limit. So a chord behind a client's
    // work ends in the trusted screen or a dead engine. A cursor chain waits on the same
    // host loop a client's readback holds, so a stalled cursor lane renews too (vote
    // 79); its abandonment at that bound ends the wait, and the kernel's deadline then
    // fails the takeover. The two waits can run in series, so both are bounded from the
    // chord as well (`waiting_chord`, vote 80).
    fn renew_behind_backlog(&mut self, device: &Device) {
        if !device.gpu.takeover_waiting() { return; }
        let last = self.renewed.map(|(generation, at)| (generation, at.elapsed().as_millis() as u64));
        if !crate::gpu_api::renew_due(last, self.generation) { return; }
        // A refused renewal (the seat already left QUIESCING) is not retried every pass.
        self.renewed = Some((self.generation, libthyla_rs::time::Instant::now()));
        let mut m = ep::Message::default(); m.generation = self.generation;
        if ep::call(ep::RENEW, &mut m).is_ok() {
            super::diagnostic(&alloc::format!(
                "lictor: takeover renewed behind a draining backlog generation={} chains={}\n",
                self.generation, device.gpu.fenced_in_flight()));
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
        let released = inputs.iter().all(|i| i.held.iter().all(|held| !held));
        match self.phase {
            0 => {},
            1 => {
                for input in inputs.iter_mut() { input.events.clear(); }
                // The bound on a takeover's waits lives only across this reap, the one pump
                // after a STATUS that read QUIESCING. The kernel fails an expired chord lazily,
                // at the next call, so any later pump could kill a stall only the restore waits on.
                device.gpu.bound_takeover(self.waiting_chord());
                device.reap();
                device.gpu.bound_takeover(None);
                #[cfg(feature = "test-mode")]
                if self.test_release_at.is_some_and(|at| at.elapsed().as_millis() as u64 >= TEST_RELEASE_AFTER_MS) {
                    self.test_release_at = None;
                    device.gpu.test_stall_release();
                    if core::mem::take(&mut self.test_then_cursor) {
                        self.test_cursor_due = true;
                        super::diagnostic(&alloc::format!(
                            "lictor: chained shot: the cursor lane is held next, past every bound generation={}\n",
                            self.generation));
                    }
                }
                #[cfg(feature = "test-mode")]
                if self.test_cursor_release_at.is_some_and(|at| at.elapsed().as_millis() as u64 >= TEST_RELEASE_AFTER_MS) {
                    self.test_cursor_release_at = None;
                    device.gpu.test_cursor_release();
                    if core::mem::take(&mut self.test_then_control) {
                        let stalled = device.gpu.test_latch_now(true);
                        super::diagnostic(&alloc::format!(
                            "lictor: chained shot: a synchronous chain withheld for good behind the cursor lane stalled={} generation={}\n",
                            stalled as u8, self.generation));
                    }
                }
                if !self.active() { self.renew_behind_backlog(device); }
                if !device.gpu.takeover_clear() {
                    if let Some(why) = device.gpu.cursor_out_of_service() {
                        if self.lane_noted != Some(self.generation) {
                            self.lane_noted = Some(self.generation);
                            super::diagnostic(&alloc::format!(
                                "lictor: takeover held by a cursor lane out of service ({}) generation={}; the chord fails at the kernel's deadline\n",
                                why, self.generation));
                        }
                        #[cfg(feature = "test-mode")]
                        if let Some(ms) = self.chord.filter(|(generation, _)| self.test_fail_late && *generation == self.generation)
                            .map(|(_, at)| at.elapsed().as_millis() as u64).filter(|ms| *ms >= TEST_FAIL_LATE_MS)
                        {
                            self.test_fail_late = false;
                            let stalled = device.gpu.test_latch_now(true);
                            self.test_release_at = Some(libthyla_rs::time::Instant::now());
                            super::diagnostic(&alloc::format!(
                                "lictor: late shot: a synchronous chain withheld {} ms from the chord, and the seat fails behind it stalled={} generation={}\n",
                                ms, stalled as u8, self.generation));
                            // The kernel is failed and Lictor's phase stays 1 to the next STATUS, the
                            // state its lazy expiry leaves, so a clear keyed to that phase meets it.
                            let mut m = ep::Message { generation: self.generation, ..Default::default() };
                            let _ = ep::call(ep::FAIL, &mut m);
                            super::diagnostic(&alloc::format!(
                                "lictor: seat failed: late shot (test-latch-chained) generation={}\n", self.generation));
                        }
                    }
                    return;
                }
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
                        screen.test_hold_after_exclusion = matches!(shot, Some(SeatShot::HoldAfterExclusion | SeatShot::ControlThenCursor));
                        screen.test_refuse_bind = shot == Some(SeatShot::RefuseBind);
                        let chained_cursor = core::mem::take(&mut self.test_cursor_due);
                        device.gpu.test_cursor_stall_next(chained_cursor
                            || matches!(shot, Some(SeatShot::CursorStall | SeatShot::CursorAbandon | SeatShot::CursorThenControl)));
                        if chained_cursor || shot == Some(SeatShot::CursorAbandon) { device.gpu.test_cursor_abandon_next(); }
                        if shot == Some(SeatShot::BehindReadback) { device.gpu.test_behind_readback(); }
                        self.test_refuse_restored |= shot == Some(SeatShot::RefuseRestored);
                        if matches!(shot, Some(SeatShot::HoldAfterExclusion | SeatShot::ControlThenCursor)) {
                            self.test_release_at = Some(libthyla_rs::time::Instant::now());
                        }
                        if matches!(shot, Some(SeatShot::CursorStall | SeatShot::CursorThenControl)) {
                            self.test_cursor_release_at = Some(libthyla_rs::time::Instant::now());
                        }
                        self.test_then_cursor |= shot == Some(SeatShot::ControlThenCursor);
                        self.test_fail_late |= shot == Some(SeatShot::ControlThenCursor);
                        self.test_then_control |= shot == Some(SeatShot::CursorThenControl);
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
                let _resume = if core::mem::take(&mut self.test_hold_restore) {
                    // The input thread stops too, so keys typed while the restore waits are
                    // still in the rings when the tail is drained.
                    intake::test::stop();
                    super::diagnostic("lictor: restore held (INJECTED by test-latch-dead)\n");
                    let _ = libthyla_rs::time::sleep(core::time::Duration::from_millis(TEST_HOLD_RESTORE_MS));
                    Some(intake::test::Resume)
                } else {
                    None
                };
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
                if self.test_release_at.take().is_some() { device.gpu.test_stall_release(); }
                // A takeover that failed before it displaced anything is not restored, so
                // the held cursor chain is let go here.
                #[cfg(feature = "test-mode")]
                if self.test_cursor_release_at.take().is_some() { device.gpu.test_cursor_release(); }
                // A chained shot's second wait is armed only inside the takeover it breaks.
                #[cfg(feature = "test-mode")]
                { self.test_then_cursor = false; self.test_then_control = false; self.test_cursor_due = false; self.test_fail_late = false; }
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
