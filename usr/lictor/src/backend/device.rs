//! Normal GPU admission. Every request is decoded in full before this layer.
//! The authenticated connection owner supplies object references; only this
//! layer resolves them into device resources and pinned backing addresses.
use alloc::vec::Vec;
use libdriver::Error;
use crate::{objects::{Objects, Kind, Retirement}, gpu_api::*, wire::Wire};
use super::{gpu::Gpu, import::Pin};
#[derive(Clone, Copy)]
pub enum Presentation {
    None,
    Image { resource: u32, width: u32, height: u32 },
    Blob { resource: u32, width: u32, height: u32, format: u32, stride: u32 },
}
/// How the seat's restore left scanout 0.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Restored {
    /// The compositor's last frame is bound again.
    Rebound,
    /// Nothing is bound: there was no frame, or the device refused it.
    Blank,
}
/// Why the seat's restore is not done.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum Unrestored {
    /// The device has not answered, because a stall stands: ask again.
    Waiting,
    /// The engine is dead, or the device refused to disable the scanout.
    Lost,
}
struct Backing { resource: u32, _pin: Pin }
/// The latch lever's size (`create 97 89`, tools/interactive/ls-gpu-dead.exp):
/// only resources created at it can fire the lever, so a boot probe's
/// retirement never does.
#[cfg(feature = "test-mode")]
const TEST_LATCH_SIZE: (u32, u32) = (97, 89);
#[cfg(feature = "test-mode")]
const TEST_LATCH_TARGETS: usize = 16;
#[cfg(feature = "test-mode")]
const TEST_BLIND_MS: u64 = 5000;
struct Retiring { resource: u32, ticket: Retirement, done: bool }
pub struct Device {
    // Drop order matters: reset the GPU before releasing any imported pins.
    pub gpu: Gpu,
    objects: Objects,
    backings: Vec<Backing>,
    retiring: Vec<Retiring>,
    rings: Vec<HostRing>,
    owner: u64,
    pub presentation: Presentation,
    refused_retirements: u64,
    taken_frees: u64,
    /// The kernel's command line declared this boot one whose levers may be
    /// armed (`TEST_LEVERS_WORD`). The compositor asks for the arming and is
    /// not trusted to: every shot below breaks a trusted takeover.
    #[cfg(feature = "test-mode")]
    test_declared: bool,
    /// The same for the chained shots (`TEST_CHAINED_WORD`).
    #[cfg(feature = "test-mode")]
    test_chained_declared: bool,
    /// A refused arming has been said, per lever: a client that repeats the
    /// request gets the refusal and no second line.
    #[cfg(feature = "test-mode")]
    test_refusal_said: [bool; 2],
    /// Latch lever shots left: 2 = the revive shot is next, 1 = the hold.
    #[cfg(feature = "test-mode")]
    test_latch_shots: u8,
    #[cfg(feature = "test-mode")]
    test_latch_targets: Vec<u32>,
    /// Resources whose retirement fired a shot: their completed retirement
    /// is said, the witness's proof that the retirement ran.
    #[cfg(feature = "test-mode")]
    test_latch_fired: Vec<u32>,
    /// Seat shots left, one per trusted takeover (`test_latch_seat_shot`).
    #[cfg(feature = "test-mode")]
    test_seat_shots: u8,
    /// Chained shots left (`Request::TestArmChained`), taken before any seat shot.
    #[cfg(feature = "test-mode")]
    test_chained_shots: u8,
    #[cfg(feature = "test-mode")]
    test_blind_armed: bool,
    /// Stall shots left for the compositor to aim (`Request::TestStallNext`).
    #[cfg(feature = "test-mode")]
    test_stall_shots: u8,
    /// The connection whose next request a `TestStallShot` rides.
    #[cfg(feature = "test-mode")]
    test_stall_shot_for: Option<i64>,
    /// The context whose reachable owed frees the pay pass passes over, and
    /// since when (`Request::TestOwedDefer`).
    #[cfg(feature = "test-mode")]
    test_owed_defer: Option<(u32, libthyla_rs::time::Instant)>,
    /// Defer-lever arms left this boot.
    #[cfg(feature = "test-mode")]
    test_owed_defers: u8,
    /// The resource whose owed free's next repeat command the stall shot
    /// rides, and since when (`Request::TestOwedStall`).
    #[cfg(feature = "test-mode")]
    test_owed_stall: Option<(u32, libthyla_rs::time::Instant)>,
    /// Owed-stall shots left this boot.
    #[cfg(feature = "test-mode")]
    test_owed_stalls: u8,
}
/// Defer-lever arms per boot: the readback leg's attempts at BUSY, LIFT and
/// APART, which arms twice.
#[cfg(feature = "test-mode")]
const TEST_OWED_DEFERS: u8 = 16;
/// How long one arm of the defer lever lasts at most; an owed-stall shot
/// no repeat took is gone as soon.
#[cfg(feature = "test-mode")]
const TEST_OWED_DEFER_MS: u64 = 2000;
/// Owed-stall shots per boot: the readback leg's attempts at YIELD, APART
/// and LIFT.
#[cfg(feature = "test-mode")]
const TEST_OWED_STALLS: u8 = 12;
/// How the lever breaks a trusted takeover's return, in firing order.
#[cfg(feature = "test-mode")]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum SeatShot {
    /// A controlq stall between exclusion and bind; the device retires it.
    StallAfterExclusion,
    /// A controlq stall on the takeover's first paint; the device retires it.
    StallInPaint,
    /// The same stall with its retirement withheld past the kernel's deadline.
    HoldAfterExclusion,
    /// The device refuses the trusted bind: an answer, so the takeover fails at once.
    RefuseBind,
    /// A readback stands ahead of the takeover's commands for longer than their bound.
    BehindReadback,
    /// A controlq stall on the episode's first frame; the device retires it.
    StallInFrame,
    /// The device refuses the episode's first frame: an answer, so the episode fails.
    RefuseFrame,
    /// The ordinary return's restore waits with keys left in the ring.
    HoldRestore,
    /// A cursor stall inside exclusion whose retirement is held 8 s: the takeover waits it out.
    CursorStall,
    /// The same cursor stall, held past the abandonment bound.
    CursorAbandon,
    /// The kernel's RESTORED refused after a completed restore.
    RefuseRestored,
    /// A controlq stall standing when the ordinary return's restore begins; the
    /// device retires it.
    StallInRestore,
    /// A controlq stall under the trusted scanout whose retirement is
    /// withheld: the engine dies.
    StallHeld,
    /// A controlq stall after exclusion held 8 s, then a cursor stall held past
    /// every bound: the takeover's bound from the chord abandons the lane. At 34 s
    /// from the chord a controlq stall is withheld and the seat fails behind it:
    /// only the restore waits on that one, so no bound may judge it.
    ControlThenCursor,
    /// A cursor stall held 8 s, then a controlq stall withheld for good: the
    /// takeover's bound from the chord kills the engine.
    CursorThenControl,
}
impl Device {
    pub fn new(mut gpu: Gpu, owner: u64) -> Result<Self, Error> {
        gpu.enable_retirement_journal();
        gpu.begin_serving();
        Ok(Self { gpu, objects: Objects::default(), backings: Vec::new(),
            retiring: Vec::new(), rings: Vec::new(), owner, presentation: Presentation::None,
            refused_retirements: 0,
            taken_frees: 0,
            #[cfg(feature = "test-mode")] test_declared: false,
            #[cfg(feature = "test-mode")] test_chained_declared: false,
            #[cfg(feature = "test-mode")] test_refusal_said: [false; 2],
            #[cfg(feature = "test-mode")] test_latch_shots: 0,
            #[cfg(feature = "test-mode")] test_seat_shots: 0,
            #[cfg(feature = "test-mode")] test_chained_shots: 0,
            #[cfg(feature = "test-mode")] test_blind_armed: false,
            #[cfg(feature = "test-mode")] test_stall_shots: 0,
            #[cfg(feature = "test-mode")] test_stall_shot_for: None,
            #[cfg(feature = "test-mode")] test_owed_defer: None,
            #[cfg(feature = "test-mode")] test_owed_defers: TEST_OWED_DEFERS,
            #[cfg(feature = "test-mode")] test_owed_stall: None,
            #[cfg(feature = "test-mode")] test_owed_stalls: TEST_OWED_STALLS,
            #[cfg(feature = "test-mode")] test_latch_targets: Vec::new(),
            #[cfg(feature = "test-mode")] test_latch_fired: Vec::new() })
    }
    #[cfg(feature = "test-mode")]
    pub fn test_declare(&mut self, declared: bool) { self.test_declared = declared; }
    #[cfg(feature = "test-mode")]
    pub fn test_declare_chained(&mut self, declared: bool) { self.test_chained_declared = declared; }
    #[cfg(feature = "test-mode")]
    fn test_undeclared(&mut self, lever: usize, word: &[u8]) -> Error {
        if !core::mem::replace(&mut self.test_refusal_said[lever], true) {
            super::diagnostic(&alloc::format!(
                "lictor: test lever arming refused: this boot's command line does not declare {}\n",
                core::str::from_utf8(word).unwrap_or("its word")));
        }
        Error::BadField
    }
    pub fn bind_owner(&mut self, owner: u64) -> bool {
        if owner == 0 || (self.owner != 0 && self.owner != owner) { return false; }
        self.owner = owner; true
    }
    fn check(&self, id: u32, kind: Kind) -> Result<(), Error> {
        self.objects.check(self.owner, id, kind).map_err(|_| Error::BadField)
    }
    fn ordinary_resource(&self, id: u32) -> Result<(), Error> {
        self.check(id, Kind::Resource)?;
        // Host-ring mappings have their own reference-counted retirement path.
        // Neither a generic unref nor scanout may bypass that ownership.
        if self.rings.iter().any(|r| r.res_id == id) { return Err(Error::BadField); }
        Ok(())
    }
    fn reserve(&mut self, id: u32, kind: Kind) -> Result<(), Error> {
        self.objects.reserve(self.owner, id, kind).map_err(|_| Error::BadField)
    }
    fn reserve_in(&mut self, id: u32, ctx: u32) -> Result<(), Error> {
        self.objects.reserve_in(self.owner, id, ctx).map_err(|_| Error::BadField)
    }
    /// A failed command left nothing on the device when no stall began
    /// across it and the engine lives: refused before staging, or refused by
    /// the device. Only then may its bookkeeping be undone (GPU-DESIGN 4.5.15).
    fn left_nothing(&self, stalls: u32) -> bool {
        self.gpu.stalls() == stalls && !self.gpu.engine_dead()
    }
    /// Release the reservation a create made, when its failure left nothing.
    /// One whose unwind left a free owed retires once that free is paid,
    /// whichever way the create went: the paid free proves the id gone.
    fn unreserve(&mut self, id: u32, kind: Kind, stalls: u32) {
        let owed = kind == Kind::Resource && self.gpu.owes_free(id);
        if owed || self.left_nothing(stalls) {
            if let Ok(ticket) = self.objects.retire(self.owner, id, kind) {
                if owed {
                    self.retiring.push(Retiring { resource: id, ticket, done: false });
                } else {
                    let _ = self.objects.retired(ticket);
                }
            }
        }
    }
    fn attach(&mut self, connection: i64, resource: u32, source: BufferRef) -> Result<(), Error> {
        self.check(resource, Kind::Resource)?;
        if self.backings.iter().any(|b| b.resource == resource) { return Err(Error::BadField); }
        let pin = Pin::claim(connection, source)?;
        let stalls = self.gpu.stalls();
        let result = self.gpu.attach_backing(resource, pin.segments());
        // A stall (or a death) is an ambiguous device completion: retain the
        // pin through a later proven detach/unref, never unwind the DMA.
        if result.is_ok() || !self.left_nothing(stalls) {
            self.backings.push(Backing { resource, _pin: pin });
        }
        result
    }
    /// Repeat the frees the device has not acknowledged (`Gpu::pay_owed_frees`),
    /// then finish the ledger entries they paid. Never inside a request: a
    /// stall one meets would read as that request's.
    pub fn pay_owed_frees(&mut self) {
        let deferred = self.test_owed_deferred();
        let aimed = self.test_owed_stall_aimed();
        let mut fired = false;
        let (objects, owner) = (&self.objects, self.owner);
        let unlinked = self.gpu.pay_owed_frees(
            |ctx, res| objects.holds(ctx, res),
            |ctx| objects.held_by(ctx),
            |res| deferred.is_some_and(|ctx| objects.holds(ctx, res)),
            |res| objects.links(owner, res),
            |res| {
                let hit = !fired && aimed == Some(res);
                fired |= hit;
                hit
            },
        );
        #[cfg(feature = "test-mode")]
        if fired {
            self.test_owed_stall = None;
        }
        for (ctx, res) in unlinked {
            let _ = self.objects.detached(self.owner, ctx, res);
        }
        self.reap();
    }
    /// Take a free that a chain which may still reach the resource stands in
    /// the way of, as a debt: the ledger entry retires now and the free is
    /// owed unsent, so the owed-free pass detaches the resource's context
    /// links and frees it once no chain can reach it. Refused instead, the
    /// entry and its pins would stay for the boot, since no client asks
    /// again. A resource on the display, a dead engine or a full debt is
    /// refused as before, its pins kept.
    fn take_unref(&mut self, resource_id: u32) -> bool {
        if self.gpu.engine_dead() || self.ordinary_resource(resource_id).is_err()
            || matches!(self.presentation, Presentation::Image { resource, .. } | Presentation::Blob { resource, .. } if resource == resource_id)
        {
            return false;
        }
        let Ok(ticket) = self.objects.retire(self.owner, resource_id, Kind::Resource) else { return false };
        if !self.gpu.owe_unsent_free(resource_id) {
            let _ = self.objects.unretire(ticket);
            return false;
        }
        self.retiring.push(Retiring { resource: resource_id, ticket, done: false });
        self.taken_frees += 1;
        if self.taken_frees.is_power_of_two() {
            crate::backend::diagnostic(&alloc::format!(
                "lictor: resource {} free taken while a chain can reach it -- owed until none can (pins kept until paid; {} so far)\n",
                resource_id, self.taken_frees));
        }
        true
    }
    /// Before every completion pump, what a new chain may not go to the device
    /// past (`OwedReach`). Every serve pass begins with a pump, so a chain
    /// commits against a reach at most a pass old; a stale reach only lets a
    /// chain go to the device, where the free waits for it as for any, or
    /// keeps one parked until the next pump.
    fn refresh_owed_reach(&mut self) {
        let objects = &self.objects;
        let (holding, standing_down) = self.gpu.owed_barrier();
        let reach = OwedReach::of(
            holding,
            &standing_down,
            &self.gpu.poisoned_reach(),
            |ctx| objects.held_by(ctx),
            |set: &dyn Fn(u32) -> bool| objects.holders(set),
        );
        self.gpu.set_owed_reach(reach);
    }
    /// The context the defer lever names, while its arm lasts.
    fn test_owed_deferred(&mut self) -> Option<u32> {
        #[cfg(feature = "test-mode")]
        {
            let (ctx, since) = self.test_owed_defer?;
            if since.elapsed().as_millis() < u128::from(TEST_OWED_DEFER_MS) {
                return Some(ctx);
            }
            self.test_owed_defer = None;
            crate::backend::diagnostic(&alloc::format!("lictor: owed-free defer on ctx {} expired\n", ctx));
            None
        }
        #[cfg(not(feature = "test-mode"))]
        {
            None
        }
    }
    /// The resource the owed-stall shot is aimed at, while it lasts.
    fn test_owed_stall_aimed(&mut self) -> Option<u32> {
        #[cfg(feature = "test-mode")]
        {
            let (res, since) = self.test_owed_stall?;
            if since.elapsed().as_millis() < u128::from(TEST_OWED_DEFER_MS) {
                return Some(res);
            }
            self.test_owed_stall = None;
            crate::backend::diagnostic(&alloc::format!("lictor: owed-stall shot on resource {} expired unspent\n", res));
            None
        }
        #[cfg(not(feature = "test-mode"))]
        {
            None
        }
    }
    /// Arm the shot `connection` asked for onto this request of its, whatever
    /// it is; `test_stall_settle` spends it if no synchronous command took it.
    #[cfg(feature = "test-mode")]
    pub fn test_stall_ride(&mut self, connection: i64) -> bool {
        if self.test_stall_shot_for != Some(connection) { return false; }
        self.test_stall_shot_for = None;
        self.gpu.test_stall_next();
        true
    }
    #[cfg(feature = "test-mode")]
    pub fn test_stall_settle(&mut self) { self.gpu.test_stall_unspent(); }
    /// A connection gone leaves no shot for whoever reuses its descriptor.
    #[cfg(feature = "test-mode")]
    pub fn test_stall_forget(&mut self, connection: i64) {
        if self.test_stall_shot_for == Some(connection) { self.test_stall_shot_for = None; }
    }
    pub fn reap(&mut self) {
        self.refresh_owed_reach();
        self.gpu.poll_completions();
        for resource in self.gpu.take_retired_resources() {
            if let Some(r) = self.retiring.iter_mut().find(|r| r.resource == resource) { r.done = true; }
        }
        let mut i = 0;
        while i < self.retiring.len() {
            let r = &self.retiring[i];
            if r.done && self.objects.retired(r.ticket).is_ok() {
                let resource = r.resource;
                self.backings.retain(|b| b.resource != resource);
                self.retiring.swap_remove(i);
                #[cfg(feature = "test-mode")]
                if let Some(k) = self.test_latch_fired.iter().position(|&f| f == resource) {
                    self.test_latch_fired.swap_remove(k);
                    crate::backend::diagnostic(&alloc::format!(
                        "lictor: latch-lever resource {} retired after its stall (pins released)\n", resource));
                }
            } else { i += 1; }
        }
    }
    /// Used only after kernel END. Ordinary admission remains closed while a
    /// complete previous normal frame is rebound and flushed. The compositor
    /// must also observe the changed seat epoch and repaint after it resumes.
    /// Only synchronous work holds it (GPU-DESIGN 4.5.15), so neither a cursor
    /// queue poisoned on a live engine nor a client's poisoned chain costs the
    /// display.
    ///
    /// The frame is the client's, so whether it can be bound again is the
    /// client's to spoil (a backing detached, on a device that wants one) and
    /// is not the episode's to pay for: a frame the device REFUSES is given up
    /// and the output left blank until the client binds a frame again. A stall is no answer, so nothing is concluded from one and the
    /// caller asks again. Only the disable, which names nothing of a client's,
    /// can leave the seat unrestored on a live engine.
    pub fn restore(&mut self) -> Result<Restored, Unrestored> {
        if self.gpu.engine_dead() { return Err(Unrestored::Lost); }
        if !self.gpu.takes_commands() { return Err(Unrestored::Waiting); }
        let stalls = self.gpu.stalls();
        let frame = match self.presentation {
            Presentation::None => None,
            Presentation::Image { resource, width, height } =>
                Some((resource, self.rebind_image(resource, width, height))),
            Presentation::Blob { resource, width, height, format, stride } =>
                Some((resource, self.rebind_blob(resource, width, height, format, stride))),
        };
        match frame {
            Some((_, Ok(()))) => return Ok(Restored::Rebound),
            Some((resource, Err(_))) => match met(self.gpu.engine_dead(), stalls, self.gpu.stalls()) {
                Met::Dead => return Err(Unrestored::Lost),
                Met::Stalled => return Err(Unrestored::Waiting),
                Met::Refused => {
                    self.presentation = Presentation::None;
                    crate::backend::diagnostic(&alloc::format!(
                        "lictor: the compositor's frame (resource {}) was refused at the restore; the seat returns to a blank output\n",
                        resource));
                }
            },
            None => {}
        }
        match self.gpu.set_scanout_restore(0, 0, 0) {
            Ok(()) => Ok(Restored::Blank),
            Err(_) => Err(match met(self.gpu.engine_dead(), stalls, self.gpu.stalls()) {
                Met::Stalled => Unrestored::Waiting,
                Met::Dead | Met::Refused => Unrestored::Lost,
            }),
        }
    }
    fn rebind_image(&mut self, resource: u32, width: u32, height: u32) -> Result<(), Error> {
        self.check(resource, Kind::Resource)?;
        self.gpu.set_scanout_restore(resource, width, height)?;
        self.gpu.flush(resource, 0, 0, width, height)
    }
    fn rebind_blob(&mut self, resource: u32, width: u32, height: u32, format: u32, stride: u32) -> Result<(), Error> {
        self.check(resource, Kind::Resource)?;
        self.gpu.set_scanout_blob_restore(resource, width, height, format, stride)
    }
    /// The armed latch lever, asked of each pending request before the server
    /// admits it: a retirement of a resource created at `TEST_LATCH_SIZE`
    /// stalls the engine -- the state a command that missed its synchronous
    /// deadline leaves, met by a retirement exactly when the park decides it.
    /// The first shot's chain retires (revival); the second's is withheld
    /// (death). Each shot forgets the marks before it, so the next one needs
    /// a resource created after it.
    #[cfg(feature = "test-mode")]
    pub fn test_latch_consider(&mut self, request: &Request) {
        if self.test_latch_shots == 0 { return; }
        match *request {
            Request::DetachBacking { resource_id } | Request::ResourceUnref { resource_id }
                if self.test_latch_targets.contains(&resource_id) => {
                let hold = self.test_latch_shots == 1;
                self.test_latch_shots -= 1;
                self.test_latch_targets.clear();
                if self.gpu.test_latch_now(hold) { self.test_latch_fired.push(resource_id); }
            }
            _ => {}
        }
    }
    /// The same lever's input half: stops the serve loop as a synchronous wait stops
    /// it, and asks the device for nothing. The input thread stopped itself at the pass
    /// that read the key (`intake::test`), so the rings stay undrained until this ends.
    #[cfg(feature = "test-mode")]
    pub fn test_blind_loop(&self) {
        if !self.test_blind_armed { return; }
        super::diagnostic("lictor: serve loop stopped (INJECTED by test-latch-dead)\n");
        let _ = libthyla_rs::time::sleep(core::time::Duration::from_millis(TEST_BLIND_MS));
        super::intake::test::resume();
        super::diagnostic("lictor: serve loop resumed\n");
    }
    /// The same lever's seat half, one shot per trusted takeover in this
    /// order, so one boot walks every way an episode's return can go wrong.
    #[cfg(feature = "test-mode")]
    pub fn test_latch_seat_shot(&mut self) -> Option<SeatShot> {
        if self.test_chained_shots > 0 {
            let shot = if self.test_chained_shots == 2 { SeatShot::ControlThenCursor } else { SeatShot::CursorThenControl };
            self.test_chained_shots -= 1;
            return Some(shot);
        }
        let shot = match self.test_seat_shots {
            13 => SeatShot::StallAfterExclusion,
            12 => SeatShot::StallInPaint,
            11 => SeatShot::HoldAfterExclusion,
            10 => SeatShot::RefuseBind,
            9 => SeatShot::BehindReadback,
            8 => SeatShot::StallInFrame,
            7 => SeatShot::RefuseFrame,
            6 => SeatShot::HoldRestore,
            5 => SeatShot::CursorStall,
            4 => SeatShot::CursorAbandon,
            3 => SeatShot::RefuseRestored,
            2 => SeatShot::StallInRestore,
            1 => SeatShot::StallHeld,
            _ => return None,
        };
        self.test_seat_shots -= 1;
        Some(shot)
    }
    #[cfg(feature = "test-mode")]
    fn test_latch_mark(&mut self, resource_id: u32, w: u32, h: u32) {
        if self.test_latch_shots > 0 && (w, h) == TEST_LATCH_SIZE
            && !self.test_latch_targets.contains(&resource_id)
            && self.test_latch_targets.len() < TEST_LATCH_TARGETS {
            self.test_latch_targets.push(resource_id);
        }
    }
    /// What stands between `request` and its object, for a retirement: the
    /// device's chains that can reach that object, and no others.
    pub fn retirement_hold(&self, request: &Request) -> Hold {
        match request.retired_object() {
            Some(object) => self.gpu.retirement_hold(object, |ctx, res| self.objects.holds(ctx, res)),
            None => Hold::Clear,
        }
    }
    /// A retirement runs only once every chain that can reach its object has
    /// really retired (an abandoned chain does not qualify); the server parks
    /// it until then, and one admitted without it (`gpu_api::admit`: a hold
    /// nothing can clear, or the park bound) is refused here before any ledger
    /// change, so no `retiring` entry is left that `reap` could never finish --
    /// except a resource's free, which is taken as a debt the owed-free pass
    /// pays once no chain can reach it (`take_unref`).
    pub fn execute(&mut self, connection: i64, request: Request) -> Result<Vec<u8>, Error> {
        let hold = self.retirement_hold(&request);
        if hold != Hold::Clear {
            // Answered as a refusal either way: the free has not happened.
            if let Request::ResourceUnref { resource_id } = request {
                if self.take_unref(resource_id) { return Err(Error::Hardware); }
            }
            self.refused_retirements += 1;
            if self.refused_retirements.is_power_of_two() {
                let why = if hold == Hold::Parks || self.gpu.engine_dead() {
                    "on a non-quiescent engine"
                } else {
                    "while a poisoned chain can reach its object"
                };
                crate::backend::diagnostic(&alloc::format!(
                    "lictor: retirement refused {} (pins kept; {} so far)\n",
                    why, self.refused_retirements));
            }
            return Err(Error::Hardware);
        }
        let mut out = Vec::new();
        match request {
            Request::Cursor { shape, scale, x, y, visible } => {
                self.gpu.cursor(shape, scale, x, y, visible).put(&mut out);
            }
            Request::Info => {
                Info::from(&self.gpu).put(&mut out);
            }
            Request::PairProtocolSelftest => {
                // A test lever: a production service carries no selftest and
                // refuses the verb rather than run device work on request.
                #[cfg(feature = "test-mode")]
                self.gpu.pair_protocol_selftest().put(&mut out);
                #[cfg(not(feature = "test-mode"))]
                return Err(Error::BadField);
            }
            Request::QueryEdid => {
                let result = self.gpu.query_edid();
                result.put(&mut out);
            }
            Request::QueryDisplayInfo => {
                let result = self.gpu.query_display_info();
                result.put(&mut out);
            }
            Request::ResourceCreate2d { resource_id, w, h } => {
                if w == 0 || h == 0 || w > 8192 || h > 8192 { return Err(Error::BadField); }
                self.reserve(resource_id, Kind::Resource)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.resource_create_2d(resource_id, w, h);
                if result.is_err() { self.unreserve(resource_id, Kind::Resource, stalls); }
                #[cfg(feature = "test-mode")]
                if result.is_ok() { self.test_latch_mark(resource_id, w, h); }
                result.put(&mut out);
            }
            Request::AttachBacking { resource_id, backing } => {
                self.attach(connection, resource_id, backing).put(&mut out);
            }
            Request::DetachBacking { resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                let result = self.gpu.detach_backing(resource_id);
                if result.is_ok() { self.backings.retain(|b| b.resource != resource_id); }
                result.put(&mut out);
            }
            Request::ResourceUnref { resource_id } => {
                self.ordinary_resource(resource_id)?;
                if matches!(self.presentation, Presentation::Image { resource, .. } | Presentation::Blob { resource, .. } if resource == resource_id) {
                    self.gpu.condemn(resource_id);
                    // The ledger refuses a retiring id, so a takeover that ends
                    // before the compositor binds a successor must restore to a
                    // blank output, not fail the seat on a resource that is gone.
                    self.presentation = Presentation::None;
                }
                let ticket = self.objects.retire(self.owner, resource_id, Kind::Resource).map_err(|_| Error::BadField)?;
                self.retiring.push(Retiring { resource: resource_id, ticket, done: false });
                self.gpu.resource_unref(resource_id).put(&mut out);
            }
            Request::Condemn { res_id } => {
                self.check(res_id, Kind::Resource)?;
                let result = self.gpu.condemn(res_id);
                result.put(&mut out);
            }
            Request::ArmScanoutDisableRefusal => {
                let result = self.gpu.arm_scanout_disable_refusal();
                result.put(&mut out);
            }
            Request::TakeInjectedRefusal => {
                let result = self.gpu.take_injected_refusal();
                result.put(&mut out);
            }
            Request::CondemnedCount => {
                let result = self.gpu.condemned_count();
                result.put(&mut out);
            }
            Request::ResourceCreateBlob { .. } => {
                return Err(Error::BadField);
            }
            Request::CreateRingBlob { resource_id, backing, len } => {
                if backing.length != len as u64 { return Err(Error::BadField); }
                let pin = Pin::claim(connection, backing)?;
                if pin.count != 1 { return Err(Error::BadField); }
                self.reserve(resource_id, Kind::Resource)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.create_ring_blob(resource_id, pin.segments[0].pa, len);
                if result.is_ok() || !self.left_nothing(stalls) {
                    self.backings.push(Backing { resource: resource_id, _pin: pin });
                } else {
                    self.unreserve(resource_id, Kind::Resource, stalls);
                }
                result.put(&mut out);
            }
            Request::CreateHost3dBlob { resource_id, ctx_id, blob_flags, len, blob_id } => {
                self.check(ctx_id, Kind::Context)?;
                if len == 0 || len as u64 > 64 * 1024 * 1024 { return Err(Error::BadField); }
                self.reserve_in(resource_id, ctx_id)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.create_host3d_blob(resource_id, ctx_id, blob_flags, len, blob_id);
                if result.is_err() { self.unreserve(resource_id, Kind::Resource, stalls); }
                result.put(&mut out);
            }
            Request::MapBlob { .. } => {
                return Err(Error::BadField);
            }
            Request::UnmapBlob { .. } => {
                return Err(Error::BadField);
            }
            Request::MintHost3dRing { res_id, ctx_id, len, blob_id } => {
                self.check(ctx_id, Kind::Context)?;
                if len == 0 || len > 1024 * 1024 { return Err(Error::BadField); }
                self.reserve_in(res_id, ctx_id)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.mint_host3d_ring(res_id, ctx_id, len, blob_id);
                if result.is_err() { self.unreserve(res_id, Kind::Resource, stalls); }
                let result = result.map(|ring| {
                    let info = RingInfo { res_id: ring.res_id, size: ring.size, cache: ring.cache };
                    self.rings.push(ring); info
                });
                result.put(&mut out);
            }
            Request::RetireHost3dRing { resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                let i = self.rings.iter().position(|r| r.res_id == resource_id).ok_or(Error::BadField)?;
                let ticket = self.objects.retire(self.owner, resource_id, Kind::Resource).map_err(|_| Error::BadField)?;
                self.retiring.push(Retiring { resource: resource_id, ticket, done: false });
                let ring = self.rings.swap_remove(i);
                self.gpu.retire_host3d_ring(ring);
                ().put(&mut out);
            }
            Request::HostmemParkCount => {
                let result = self.gpu.hostmem_park_count();
                result.put(&mut out);
            }
            Request::HostmemReapCount => {
                let result = self.gpu.hostmem_reap_count();
                result.put(&mut out);
            }
            Request::DropHost3dRing { resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                let i = self.rings.iter().position(|r| r.res_id == resource_id).ok_or(Error::BadField)?;
                let ticket = self.objects.retire(self.owner, resource_id, Kind::Resource).map_err(|_| Error::BadField)?;
                self.retiring.push(Retiring { resource: resource_id, ticket, done: false });
                let ring = self.rings.swap_remove(i);
                self.gpu.retire_host3d_ring(ring);
                ().put(&mut out);
            }
            Request::CtxCreate { ctx_id, debug_name } => {
                if debug_name.len() > 64 { return Err(Error::BadField); }
                self.reserve(ctx_id, Kind::Context)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.ctx_create(ctx_id, &debug_name);
                if result.is_err() { self.unreserve(ctx_id, Kind::Context, stalls); }
                result.put(&mut out);
            }
            Request::CtxCreateVenus { ctx_id } => {
                self.reserve(ctx_id, Kind::Context)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.ctx_create_venus(ctx_id);
                if result.is_err() { self.unreserve(ctx_id, Kind::Context, stalls); }
                result.put(&mut out);
            }
            Request::CtxCreateCapset { ctx_id, capset_id, debug_name } => {
                if debug_name.len() > 64 { return Err(Error::BadField); }
                self.reserve(ctx_id, Kind::Context)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.ctx_create_capset(ctx_id, capset_id, &debug_name);
                if result.is_err() { self.unreserve(ctx_id, Kind::Context, stalls); }
                result.put(&mut out);
            }
            Request::CtxDestroy { ctx_id } => {
                self.check(ctx_id, Kind::Context)?;
                #[cfg(feature = "test-mode")]
                if self.test_owed_defer.is_some_and(|(c, _)| c == ctx_id) {
                    self.test_owed_defer = None;
                }
                let ticket = self.objects.retire(self.owner, ctx_id, Kind::Context).map_err(|_| Error::BadField)?;
                let result = self.gpu.ctx_destroy(ctx_id);
                if result.is_ok() {
                    self.objects.context_destroyed(self.owner, ctx_id).map_err(|_| Error::Hardware)?;
                    self.objects.retired(ticket).map_err(|_| Error::Hardware)?;
                } else if !self.gpu.engine_dead() {
                    // Refused before staging, answered, or met by a stall: the id
                    // stays named and may be destroyed again. A repeat of one that
                    // ran is harmless: QEMU's CTX_DESTROY only drops the handle
                    // from virglrenderer's table (QEMU v10.0
                    // virgl_cmd_context_destroy, virglrenderer 1.1.0
                    // virgl_context_remove), and no create can take the id meanwhile.
                    let _ = self.objects.unretire(ticket);
                }
                result.put(&mut out);
            }
            Request::CtxAttachResource { ctx_id, resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                self.check(ctx_id, Kind::Context)?;
                self.objects.attach(self.owner, ctx_id, resource_id).map_err(|_| Error::BadField)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.ctx_attach_resource(ctx_id, resource_id);
                // Linked before the device answers; a failure that left
                // nothing unlinks, so the resource can still retire.
                if result.is_err() && self.left_nothing(stalls) {
                    let _ = self.objects.detached(self.owner, ctx_id, resource_id);
                }
                result.put(&mut out);
            }
            Request::CtxDetachResource { ctx_id, resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                self.check(ctx_id, Kind::Context)?;
                let result = self.gpu.ctx_detach_resource(ctx_id, resource_id);
                if result.is_ok() { self.objects.detached(self.owner, ctx_id, resource_id).map_err(|_| Error::BadField)?; }
                result.put(&mut out);
            }
            Request::ResourceCreate3d { resource_id, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags } => {
                self.reserve(resource_id, Kind::Resource)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.resource_create_3d(resource_id, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags);
                if result.is_err() { self.unreserve(resource_id, Kind::Resource, stalls); }
                result.put(&mut out);
            }
            Request::SetScanout { resource_id, w, h } => {
                if resource_id != 0 { self.ordinary_resource(resource_id)?; }
                let result = self.gpu.set_scanout_client(resource_id, w, h);
                if result.is_ok() { self.presentation = if resource_id == 0 { Presentation::None } else { Presentation::Image { resource: resource_id, width: w, height: h } }; }
                result.put(&mut out);
            }
            Request::SetScanoutBlobProbe { resource_id, w, h, format, stride } => {
                if resource_id != 0 { self.ordinary_resource(resource_id)?; }
                let result = self.gpu.set_scanout_blob_probe(resource_id, w, h, format, stride);
                if matches!(result, Ok(0x1100)) { self.presentation = if resource_id == 0 { Presentation::None } else { Presentation::Blob { resource: resource_id, width: w, height: h, format, stride } }; }
                result.put(&mut out);
            }
            Request::SetScanoutBlob { resource_id, w, h, format, stride } => {
                if resource_id != 0 { self.ordinary_resource(resource_id)?; }
                let result = self.gpu.set_scanout_blob(resource_id, w, h, format, stride);
                if result.is_ok() { self.presentation = if resource_id == 0 { Presentation::None } else { Presentation::Blob { resource: resource_id, width: w, height: h, format, stride } }; }
                result.put(&mut out);
            }
            Request::SetScanoutBlobThenFlush { resource_id, w, h, format, stride } => {
                if resource_id != 0 { self.ordinary_resource(resource_id)?; }
                let result = self.gpu.set_scanout_blob_then_flush(resource_id, w, h, format, stride);
                if result.is_ok() { self.presentation = if resource_id == 0 { Presentation::None } else { Presentation::Blob { resource: resource_id, width: w, height: h, format, stride } }; }
                result.put(&mut out);
            }
            Request::CreatePresentable { res_id, ctx_id, len, blob_id } => {
                self.check(ctx_id, Kind::Context)?;
                if len == 0 || len as u64 > 64 * 1024 * 1024 { return Err(Error::BadField); }
                self.reserve_in(res_id, ctx_id)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.create_presentable(res_id, ctx_id, len, blob_id);
                if result.is_err() { self.unreserve(res_id, Kind::Resource, stalls); }
                result.put(&mut out);
            }
            Request::CtxAttachResourceProbe { ctx_id, resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                self.check(ctx_id, Kind::Context)?;
                // Linked before the device answers, as CtxAttachResource is:
                // a retirement reads this ledger for what a chain can reach,
                // so a link the device holds must never be missing from it.
                self.objects.attach(self.owner, ctx_id, resource_id).map_err(|_| Error::BadField)?;
                let stalls = self.gpu.stalls();
                let result = self.gpu.ctx_attach_resource_probe(ctx_id, resource_id);
                if !matches!(result, Ok(0x1100)) && self.left_nothing(stalls) {
                    let _ = self.objects.detached(self.owner, ctx_id, resource_id);
                }
                result.put(&mut out);
            }
            Request::Transfer { resource_id, offset, x, y, w, h } => {
                self.check(resource_id, Kind::Resource)?;
                let result = self.gpu.transfer(resource_id, offset, x, y, w, h);
                result.put(&mut out);
            }
            Request::Flush { resource_id, x, y, w, h } => {
                self.check(resource_id, Kind::Resource)?;
                let result = self.gpu.flush(resource_id, x, y, w, h);
                result.put(&mut out);
            }
            Request::TransferThenFlush { resource_id, offset, x, y, w, h } => {
                self.check(resource_id, Kind::Resource)?;
                let result = self.gpu.transfer_then_flush(resource_id, offset, x, y, w, h);
                result.put(&mut out);
            }
            Request::Submit3d { ctx_id, ctx_pub, stream, ring_idx } => {
                self.check(ctx_id, Kind::Context)?;
                if stream.len() > 0x9000 - 32 || stream.len() % 4 != 0 { return Err(Error::BadField); }
                if ring_idx > 3 { return Err(Error::BadField); }
                let result = self.gpu.submit_3d(ctx_id, ctx_pub, &stream, ring_idx);
                result.put(&mut out);
            }
            Request::Submit3dSync { ctx_id, stream } => {
                self.check(ctx_id, Kind::Context)?;
                if stream.len() > 0x9000 - 32 || stream.len() % 4 != 0 { return Err(Error::BadField); }
                let result = self.gpu.submit_3d_sync(ctx_id, &stream);
                result.put(&mut out);
            }
            Request::TransferTo3dSync { ctx_id, res_id, w, h, stride } => {
                self.check(res_id, Kind::Resource)?;
                if ctx_id != 0 { self.check(ctx_id, Kind::Context)?; }
                let result = self.gpu.transfer_to_3d_sync(ctx_id, res_id, w, h, stride);
                result.put(&mut out);
            }
            Request::TransferTo3dBoxSync { ctx_id, res_id, x, y, w, h, offset, stride } => {
                self.check(res_id, Kind::Resource)?;
                if ctx_id != 0 { self.check(ctx_id, Kind::Context)?; }
                let result = self.gpu.transfer_to_3d_box_sync(ctx_id, res_id, x, y, w, h, offset, stride);
                result.put(&mut out);
            }
            Request::Transfer3d { to_host, ctx_id, ctx_pub, res_id, level, x, y, z, w, h, d, offset, stride, layer_stride } => {
                self.check(res_id, Kind::Resource)?;
                if ctx_id != 0 { self.check(ctx_id, Kind::Context)?; }
                let result = self.gpu.transfer_3d(to_host, ctx_id, ctx_pub, res_id, level, x, y, z, w, h, d, offset, stride, layer_stride);
                result.put(&mut out);
            }
            Request::TransferFrom3dComp { ctx_id, ctx_pub, res_id, w, h, stride } => {
                self.check(res_id, Kind::Resource)?;
                if ctx_id != 0 { self.check(ctx_id, Kind::Context)?; }
                let result = self.gpu.transfer_from_3d_comp(ctx_id, ctx_pub, res_id, w, h, stride);
                result.put(&mut out);
            }
            Request::TransferFrom3dSync { ctx_id, res_id, w, h, stride } => {
                self.check(res_id, Kind::Resource)?;
                if ctx_id != 0 { self.check(ctx_id, Kind::Context)?; }
                let result = self.gpu.transfer_from_3d_sync(ctx_id, res_id, w, h, stride);
                result.put(&mut out);
            }
            Request::TransferFrom3dBoxSync { ctx_id, res_id, x, y, w, h, offset, stride } => {
                self.check(res_id, Kind::Resource)?;
                if ctx_id != 0 { self.check(ctx_id, Kind::Context)?; }
                let result = self.gpu.transfer_from_3d_box_sync(ctx_id, res_id, x, y, w, h, offset, stride);
                result.put(&mut out);
            }
            Request::PollCompletions => {
                self.refresh_owed_reach();
                let result = self.gpu.poll_completions();
                result.put(&mut out);
            }
            Request::TakeCompletions => {
                let result = self.gpu.take_completions();
                result.put(&mut out);
            }
            Request::TakeVindications => {
                let result = self.gpu.take_vindications();
                result.put(&mut out);
            }
            Request::TestHoldCtx { ctx_pub } => {
                #[cfg(feature = "test-mode")] { let result = self.gpu.test_hold_ctx(ctx_pub); result.put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestHoldCtxCurrent => {
                #[cfg(feature = "test-mode")] { let result = self.gpu.test_hold_ctx_current(); result.put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestAbandonCtx { ctx_pub } => {
                #[cfg(feature = "test-mode")] { let result = self.gpu.test_abandon_ctx(ctx_pub); result.put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestHoldCtxDied { ctx_pub } => {
                #[cfg(feature = "test-mode")] { let result = self.gpu.test_hold_ctx_died(ctx_pub); result.put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestAbandonedTotal => {
                #[cfg(feature = "test-mode")] { let result = self.gpu.test_abandoned_total(); result.put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::FencedHeld => {
                #[cfg(feature = "test-mode")] { let result = self.gpu.fenced_held(); result.put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestFencedFree => {
                #[cfg(feature = "test-mode")] { let result = self.gpu.test_fenced_free(); result.put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestArmLatch => {
                #[cfg(feature = "test-mode")] {
                    if !self.test_declared { return Err(self.test_undeclared(0, crate::gpu_api::TEST_LEVERS_WORD)); }
                    self.test_latch_shots = 2; self.test_seat_shots = 13; self.test_stall_shots = 3;
                    self.test_blind_armed = true;
                    super::intake::test::arm();
                    ().put(&mut out);
                }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestArmChained => {
                #[cfg(feature = "test-mode")] {
                    if !self.test_chained_declared { return Err(self.test_undeclared(1, crate::gpu_api::TEST_CHAINED_WORD)); }
                    self.test_chained_shots = 2; ().put(&mut out);
                }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestStallNext => {
                #[cfg(feature = "test-mode")] {
                    let armed = self.test_stall_shots > 0;
                    if armed { self.test_stall_shots -= 1; self.gpu.test_stall_next(); }
                    armed.put(&mut out);
                }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestStallShot => {
                #[cfg(feature = "test-mode")] { self.test_stall_shot_for = Some(connection); ().put(&mut out); }
                #[cfg(not(feature = "test-mode"))] { return Err(Error::BadField); }
            }
            Request::TestOwedDefer { ctx_id, on } => {
                #[cfg(feature = "test-mode")] {
                    // One context at a time; only its own arm ends it early.
                    self.check(ctx_id, Kind::Context)?;
                    let current = self.test_owed_deferred();
                    let done = if !on {
                        let mine = current == Some(ctx_id);
                        if mine { self.test_owed_defer = None; }
                        mine
                    } else if current.is_some_and(|c| c != ctx_id) || self.test_owed_defers == 0 {
                        false
                    } else {
                        self.test_owed_defers -= 1;
                        self.test_owed_defer = Some((ctx_id, libthyla_rs::time::Instant::now()));
                        true
                    };
                    done.put(&mut out);
                }
                #[cfg(not(feature = "test-mode"))] { let _ = (ctx_id, on); return Err(Error::BadField); }
            }
            Request::TestOwedStall { resource_id } => {
                #[cfg(feature = "test-mode")] {
                    // One shot at a time, on a live resource of the caller's.
                    self.check(resource_id, Kind::Resource)?;
                    let armed = self.test_owed_stall_aimed().is_none() && self.test_owed_stalls > 0;
                    if armed {
                        self.test_owed_stalls -= 1;
                        self.test_owed_stall = Some((resource_id, libthyla_rs::time::Instant::now()));
                    }
                    armed.put(&mut out);
                }
                #[cfg(not(feature = "test-mode"))] { let _ = resource_id; return Err(Error::BadField); }
            }
            Request::CompSlotState => {
                let result = self.gpu.comp_slot_state();
                result.put(&mut out);
            }
            Request::CtxFencesInFlight { ctx_pub } => {
                let result = self.gpu.ctx_fences_in_flight(ctx_pub);
                result.put(&mut out);
            }
            Request::CtxHasPoisonedSlot { ctx_pub } => {
                let result = self.gpu.ctx_has_poisoned_slot(ctx_pub);
                result.put(&mut out);
            }
            Request::FencedInFlight => {
                // Parked chains included: the compositor's pump publishes them.
                let result = self.gpu.fenced_outstanding();
                result.put(&mut out);
            }
            Request::EngineDead => {
                let result = self.gpu.engine_dead();
                result.put(&mut out);
            }
            Request::FreesOwed => {
                let retiring = u32::try_from(self.retiring.len()).unwrap_or(u32::MAX);
                Frees { retiring, ..self.gpu.frees_owed() }.put(&mut out);
            }
            Request::InputInfo { .. } => {
                return Err(Error::BadField);
            }
            Request::InputDrain { .. } => {
                return Err(Error::BadField);
            }
            Request::SeatState | Request::SeatQuiesced { .. } => {
                return Err(Error::BadField);
            }
        }
        self.reap();
        Ok(out)
    }
    pub fn ring(&self, resource: u32) -> Option<&HostRing> {
        self.check(resource, Kind::Resource).ok()?;
        self.rings.iter().find(|r| r.res_id == resource)
    }
}
