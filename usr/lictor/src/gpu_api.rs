//! Data shared by the bounded GPU broker and its normal compositor proxy.
//! Device-private VAs, BARs and queue descriptors never cross this interface.
use alloc::vec::Vec;
use crate::wire::{Wire, Reader, Malformed};
use libdriver::Error;
pub struct HostRing {
    pub res_id: u32,
    pub offset: u64,
    pub va: u64,
    pub size: u64,
    pub cache: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FenceVindication {
    pub ctx_pub: u32,
    /// ROUND F3 [P1] / main#242: TRUE when the late-retiring chain was the
    /// COMPOSITOR's own readback on the reserved slot. The completion arm
    /// already guards its dense `fence_signaled` bump on `!tag.comp`, but a
    /// vindication is produced AFTER the tag was taken by abandonment, so
    /// without this bit the seam credited the CLIENT with a fence it never
    /// issued -- and the tag carries the client's ctx (AS-BUILT 1), so the
    /// credit lands squarely on it. The winsys computes `issued - signaled`
    /// on unsigned counters and `warp_fence_wait` returns on
    /// `signaled >= seq`: one ahead, permanently, means every wait returns
    /// ONE FENCE EARLY for the ctx's life -- the client may reuse a buffer
    /// the GPU is still writing. Sourced structurally from the slot index,
    /// which is the only thing that survives the abandonment.
    pub comp: bool,
    /// The venus timeline the abandoned chain rode (multi-queue F3). Same
    /// production problem as `comp`: the vindication is minted AFTER the tag
    /// was taken by abandonment, so the lane must be RETAINED per-slot
    /// (`fslot_poison_ring`, exactly like the ctx id) or a vindicated fence
    /// bumps the ctx total but never its timeline -- `timeline_signaled[t]`
    /// one short forever, the per-timeline replay of the #210 silent
    /// post-recovery park.
    pub ring_idx: u8,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FenceTag {
    pub fence_id: u64,
    /// The seam ctx whose RESOURCES this chain touches -- always the
    /// client's, even for a compositor-owned readback (`comp`): the
    /// abandonment bookkeeping (`fslot_poison_ctx`, `ctx_has_poisoned_slot`,
    /// the vindication) keys on this id, and a device write into a client
    /// BO that never retired must poison THAT ctx and hold up THAT ctx's
    /// vindication (round-4 F1: one late retire proves nothing about the
    /// rest). 0 is `warp_ctx_vindicate`'s no-slot sentinel and is never
    /// minted, so it can never be a marker here.
    pub ctx_pub: u32,
    /// TRANSFER_FROM_HOST_3D: the device READS the resource synchronously at
    /// processing time (Warp-C C-6, GPU-DESIGN 4.5.13), so while one is in
    /// flight the sync slot's stale-wake deadline is the fence bound, not
    /// the dead-device one -- a device stalled behind a legitimate readback
    /// is busy, and a false `dead` latch is the #31 loss.
    pub readback: bool,
    /// Compositor-owned (the composed-GL present's readback arm, C-6): rode
    /// the reserved slot; the fence pump routes it to
    /// `comp_readback_retired`, it is counted in the ctx's `fences_in_flight`
    /// (retire safety) but subtracted from admission, and its retire never
    /// bumps the client's `fence_signaled` (#210: the client counts fences it
    /// ISSUED).
    pub comp: bool,
    /// The device never retired this chain within FENCE_ABANDON_MS: the
    /// slot's bookkeeping was reclaimed so the engine stops counting it,
    /// but the chain may still be live device-side. NOT a completion --
    /// the owning ctx is poisoned (every later BO retire leaks rather
    /// than frees, since the device may still DMA the backing).
    pub abandoned: bool,
    /// ROUND F2 [P1]: the device's verdict on this chain. The pre-C-6b
    /// composed readback was SYNCHRONOUS and gated its compose on
    /// `transfer_from_3d_sync(...).is_ok()`; moving to the fenced lane
    /// dropped that gate on the floor, because `drain` logged a non-OK
    /// response type and pushed the tag anyway and the tag carried no
    /// status. `comp_readback_retired` then composed on an ERROR retire --
    /// painting whatever the backing held (zeros on a fresh BO: the pane
    /// blanks) and counting it `rb_landed`. False for an abandoned tag too:
    /// nothing was verified about a chain that never retired.
    pub ok: bool,
    /// Released unpublished (vote 81): a parked chain the device never saw,
    /// refused because its context's prefix was abandoned or the engine died.
    /// Not a completion: its fence never signals and is never counted, or a
    /// dense count would pass the fence ahead of it. A released client chain
    /// ends its context's fence stream for good (`WarpCtx::released`), and
    /// Lictor releases every later chain of that context too (`Released`).
    pub released: bool,
    /// The venus TIMELINE this fence rides (multi-queue F3): 0 = the
    /// ctx-global lane (every pre-multi-queue submission, transfers, the
    /// compositor readback); 1..=3 = a VkQueue's timeline (the submit
    /// carried INFO_RING_IDX). The seam's per-timeline `timeline_signaled`
    /// retires by this value -- server-minted, never client bytes, so it is
    /// always < WARP_TIMELINES by construction.
    pub ring_idx: u8,
    /// How long the chain waited in Lictor before the device saw it: a readback
    /// parks behind its context's earlier chains, and that context's later
    /// chains park behind the readback (operator vote 81, GPU-DESIGN 4.5.13).
    pub parked_ms: u32,
    /// Publication to retirement. For a readback this is what QEMU spent
    /// decoding it, which is short exactly when the parking rule held.
    pub exec_ms: u32,
}
/// Whether a new fenced chain of context `ctx` parks instead of publishing
/// (vote 81): behind any parked chain of its context, so the context's stream
/// order holds, and, a readback, while its context has a chain the device may
/// still run (`on_device`). `parked` is (context, readback) per parked chain.
/// A context with a released chain parks everything, and the pump releases it.
pub fn must_park(parked: &[(u32, bool)], ctx: u32, readback: bool, on_device: bool, released: &Released) -> bool {
    if released.holds(ctx) {
        return true;
    }
    parked.iter().any(|&(c, _)| c == ctx) || (readback && on_device)
}
/// The contexts with a released client chain (vote 81). The device never ran
/// that chain, so nothing later of its context may run: a later chain's
/// completion would count past it. Kept here, where the release happens,
/// because the seam latches the stream only on its next pump and admits from
/// the older view until then. One entry per device context, forgotten only
/// when a destroy of the device context the released chain rode completes.
#[derive(Clone, Debug, Default)]
pub struct Released(Vec<(u32, u32)>);
impl Released {
    /// Record a released chain of device context `dev`. A compositor
    /// readback's release does not end the client's stream.
    pub fn note(&mut self, dev: u32, tag: &FenceTag) {
        if !tag.comp {
            self.forget(dev);
            self.0.push((dev, tag.ctx_pub));
        }
    }
    pub fn forget(&mut self, dev: u32) {
        self.0.retain(|&(d, _)| d != dev);
    }
    pub fn holds(&self, ctx_pub: u32) -> bool {
        self.0.iter().any(|&(_, p)| p == ctx_pub)
    }
}
/// The tag of a parked chain released unpublished (device context `dev`):
/// refused, not abandoned, since the device never saw it; its context is
/// recorded in `released`.
pub fn release_tag(mut tag: FenceTag, parked_ms: u32, dev: u32, released: &mut Released) -> FenceTag {
    tag.ok = false;
    tag.released = true;
    tag.parked_ms = parked_ms;
    released.note(dev, &tag);
    tag
}
/// One `fenced-held` census row (W-4): (slot, fence_id, ctx_pub, ring_idx,
/// readback, comp, age_ms, parked). A parked chain's age is the time parked.
pub type HeldRow = (usize, u64, u32, u8, bool, bool, u64, bool);
/// What the completion pump does with one parked chain.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Unpark {
    Keep,
    Publish,
    /// Refused unpublished: the device never saw it, so its slot is free.
    Release,
}
/// What happens to each parked chain now, in order. A context's parked run
/// starts at a readback; it goes once the context has nothing the device may
/// still run, the chains behind it go with it, and the next readback of that
/// context waits for them. Another context's run is independent. A context
/// with an abandoned chain not yet retired (`poisoned`) has its parked chains
/// released: that chain may never retire, and a parked chain has no clock of
/// its own, so it would count in the lane forever. So does a context with a
/// released chain, for as long as its device context lives.
pub fn unpark_plan(
    parked: &[(u32, bool)],
    on_device: &dyn Fn(u32) -> bool,
    poisoned: &dyn Fn(u32) -> bool,
    released: &Released,
) -> Vec<Unpark> {
    let mut plan = alloc::vec![Unpark::Keep; parked.len()];
    let mut blocked: Vec<u32> = Vec::new();
    let mut sent: Vec<u32> = Vec::new();
    for (i, &(ctx, readback)) in parked.iter().enumerate() {
        if poisoned(ctx) || released.holds(ctx) {
            plan[i] = Unpark::Release;
            continue;
        }
        if blocked.contains(&ctx) {
            continue;
        }
        if readback && (sent.contains(&ctx) || on_device(ctx)) {
            blocked.push(ctx);
            continue;
        }
        plan[i] = Unpark::Publish;
        if !sent.contains(&ctx) {
            sent.push(ctx);
        }
    }
    plan
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FencedErr {
    /// Every fenced slot is in flight -- the client retries (E_AGAIN).
    /// Refuse-not-block: the serve loop must stay live (#31/#125).
    Again,
    /// The request exceeds the slot buffer (E_INVAL).
    TooBig,
    /// The engine is dead (latched) or the lane absent (E_IO).
    Dead,
}

macro_rules! record {
    ($name:ident { $($field:ident : $ty:ty),* $(,)? }) => {
        impl Wire for $name {
            fn put(&self, out: &mut Vec<u8>) { $(self.$field.put(out);)* }
            fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
                Ok(Self { $($field: <$ty>::get(r)?,)* })
            }
        }
    };
}
record!(FenceVindication { ctx_pub: u32, comp: bool, ring_idx: u8 });
record!(FenceTag { fence_id: u64, ctx_pub: u32, readback: bool, comp: bool,
    abandoned: bool, ok: bool, released: bool, ring_idx: u8, parked_ms: u32, exec_ms: u32 });
impl Wire for FencedErr {
    fn put(&self, out: &mut Vec<u8>) {
        match self { Self::Again => 0u8, Self::TooBig => 1, Self::Dead => 2 }.put(out);
    }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        Ok(match u8::get(r)? { 0 => Self::Again, 1 => Self::TooBig,
            2 => Self::Dead, _ => return Err(Malformed) })
    }
}
impl Wire for Error {
    fn put(&self, out: &mut Vec<u8>) {
        let code: u8 = match self {
            Self::Parse => 0, Self::NoMatch => 1, Self::TooManyWindows => 2,
            Self::TooManyIrqs => 3, Self::BadVersion => 4, Self::BadField => 5,
            Self::BadNumber => 6, Self::TooManyResources => 7, Self::NoDescriptor => 8,
            Self::NoSuchResource => 9, Self::Hardware => 10,
        }; code.put(out);
    }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        Ok(match u8::get(r)? {
            0 => Self::Parse, 1 => Self::NoMatch, 2 => Self::TooManyWindows,
            3 => Self::TooManyIrqs, 4 => Self::BadVersion, 5 => Self::BadField,
            6 => Self::BadNumber, 7 => Self::TooManyResources, 8 => Self::NoDescriptor,
            9 => Self::NoSuchResource, 10 => Self::Hardware, _ => return Err(Malformed),
        })
    }
}
/// One kernel-registered share, authenticated against the connection's peer.
/// `offset` and `length` select bytes within that object. This carries no PA.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BufferRef { pub share: u64, pub offset: u64, pub length: u64 }
record!(BufferRef { share: u64, offset: u64, length: u64 });
#[derive(Clone, Debug, Default)]
pub struct Info {
    pub width: u32,
    pub height: u32,
    pub virgl: bool,
    pub edid: bool,
    pub edid_mm: Option<(u32, u32)>,
    pub ctxinit: bool,
    pub blob: bool,
    pub cmd_seq: u64,
    pub last_scanout_seq: u64,
    pub last_unref_seq: u64,
    pub condemned_lost: u32,
    pub num_capsets: u32,
    pub capset_blob: alloc::vec::Vec<u8>,
    pub capset_id: u32,
    pub capset_ver: u32,
    pub venus_capset_blob: alloc::vec::Vec<u8>,
}
record!(Info { width: u32, height: u32, virgl: bool, edid: bool, edid_mm: Option<(u32, u32)>, ctxinit: bool, blob: bool, cmd_seq: u64, last_scanout_seq: u64, last_unref_seq: u64, condemned_lost: u32, num_capsets: u32, capset_blob: alloc::vec::Vec<u8>, capset_id: u32, capset_ver: u32, venus_capset_blob: alloc::vec::Vec<u8> });
#[cfg(feature = "backend")]
impl From<&crate::backend::gpu::Gpu> for Info {
    fn from(gpu: &crate::backend::gpu::Gpu) -> Self { Self {
        width: gpu.width.clone(),
        height: gpu.height.clone(),
        virgl: gpu.virgl.clone(),
        edid: gpu.edid.clone(),
        edid_mm: gpu.edid_mm.clone(),
        ctxinit: gpu.ctxinit.clone(),
        blob: gpu.blob.clone(),
        cmd_seq: gpu.cmd_seq.clone(),
        last_scanout_seq: gpu.last_scanout_seq.clone(),
        last_unref_seq: gpu.last_unref_seq.clone(),
        condemned_lost: gpu.condemned_lost.clone(),
        num_capsets: gpu.num_capsets.clone(),
        capset_blob: gpu.capset_blob.clone(),
        capset_id: gpu.capset_id.clone(),
        capset_ver: gpu.capset_ver.clone(),
        venus_capset_blob: gpu.venus_capset_blob.clone(),
    } }
}

/// Version 1 normal GPU protocol. All requests are fully decoded before acting.
#[derive(Debug)]
pub enum Request {
    Info,
    PairProtocolSelftest,
    QueryEdid,
    QueryDisplayInfo,
    ResourceCreate2d { resource_id: u32, w: u32, h: u32 },
    AttachBacking { resource_id: u32, backing: BufferRef },
    DetachBacking { resource_id: u32 },
    ResourceUnref { resource_id: u32 },
    Condemn { res_id: u32 },
    ArmScanoutDisableRefusal,
    TakeInjectedRefusal,
    CondemnedCount,
    ResourceCreateBlob { resource_id: u32, blob_mem: u32, blob_flags: u32, backing: BufferRef, len: u32 },
    CreateRingBlob { resource_id: u32, backing: BufferRef, len: u32 },
    CreateHost3dBlob { resource_id: u32, ctx_id: u32, blob_flags: u32, len: u32, blob_id: u64 },
    MapBlob { resource_id: u32, offset: u64 },
    UnmapBlob { resource_id: u32 },
    MintHost3dRing { res_id: u32, ctx_id: u32, len: u32, blob_id: u64 },
    RetireHost3dRing { resource_id: u32 },
    HostmemParkCount,
    HostmemReapCount,
    DropHost3dRing { resource_id: u32 },
    CtxCreate { ctx_id: u32, debug_name: Vec<u8> },
    CtxCreateVenus { ctx_id: u32 },
    CtxCreateCapset { ctx_id: u32, capset_id: u32, debug_name: Vec<u8> },
    CtxDestroy { ctx_id: u32 },
    CtxAttachResource { ctx_id: u32, resource_id: u32 },
    CtxDetachResource { ctx_id: u32, resource_id: u32 },
    ResourceCreate3d { resource_id: u32, target: u32, format: u32, bind: u32, width: u32, height: u32, depth: u32, array_size: u32, last_level: u32, nr_samples: u32, flags: u32 },
    SetScanout { resource_id: u32, w: u32, h: u32 },
    SetScanoutBlobProbe { resource_id: u32, w: u32, h: u32, format: u32, stride: u32 },
    SetScanoutBlob { resource_id: u32, w: u32, h: u32, format: u32, stride: u32 },
    SetScanoutBlobThenFlush { resource_id: u32, w: u32, h: u32, format: u32, stride: u32 },
    CreatePresentable { res_id: u32, ctx_id: u32, len: u32, blob_id: u64 },
    CtxAttachResourceProbe { ctx_id: u32, resource_id: u32 },
    Transfer { resource_id: u32, offset: u64, x: u32, y: u32, w: u32, h: u32 },
    Flush { resource_id: u32, x: u32, y: u32, w: u32, h: u32 },
    TransferThenFlush { resource_id: u32, offset: u64, x: u32, y: u32, w: u32, h: u32 },
    Submit3d { ctx_id: u32, ctx_pub: u32, stream: Vec<u8>, ring_idx: u8 },
    Submit3dSync { ctx_id: u32, stream: Vec<u8> },
    TransferTo3dSync { ctx_id: u32, res_id: u32, w: u32, h: u32, stride: u32 },
    TransferTo3dBoxSync { ctx_id: u32, res_id: u32, x: u32, y: u32, w: u32, h: u32, offset: u64, stride: u32 },
    Transfer3d { to_host: bool, ctx_id: u32, ctx_pub: u32, res_id: u32, level: u32, x: u32, y: u32, z: u32, w: u32, h: u32, d: u32, offset: u64, stride: u32, layer_stride: u32 },
    TransferFrom3dComp { ctx_id: u32, ctx_pub: u32, res_id: u32, w: u32, h: u32, stride: u32 },
    TransferFrom3dSync { ctx_id: u32, res_id: u32, w: u32, h: u32, stride: u32 },
    TransferFrom3dBoxSync { ctx_id: u32, res_id: u32, x: u32, y: u32, w: u32, h: u32, offset: u64, stride: u32 },
    PollCompletions,
    TakeCompletions,
    TakeVindications,
    TestHoldCtx { ctx_pub: Option<u32> },
    TestHoldCtxCurrent,
    TestAbandonCtx { ctx_pub: u32 },
    TestHoldCtxDied { ctx_pub: u32 },
    TestAbandonedTotal,
    /// Test lever: the engine latches dead, as its synchronous deadline
    /// would, at the next retirement the compositor asks for.
    TestArmLatch,
    /// Test lever, one shot per armed boot: the next synchronous command
    /// retires, then reports its deadline missed (a stall the next pump
    /// revives).
    TestStallNext,
    /// Test lever: the next two trusted takeovers each wait twice in series, the
    /// second wait outlasting the takeover's bound from the chord (vote 80).
    TestArmChained,
    FencedHeld,
    TestFencedFree,
    CompSlotState,
    CtxFencesInFlight { ctx_pub: u32 },
    CtxHasPoisonedSlot { ctx_pub: u32 },
    FencedInFlight,
    EngineDead,
    InputInfo { index: u32 },
    InputDrain { index: u32 },
    SeatState,
    SeatQuiesced { generation: u64 },
    /// Semantic cursor only; no client resource or output selection.
    Cursor { shape: u8, scale: u16, x: u32, y: u32, visible: bool },
}
impl Request {
    /// Gives back something the device may still read or write, so it runs
    /// only once every outstanding chain has retired.
    pub fn retires(&self) -> bool {
        self.retired_object().is_some()
    }
    /// What a retirement gives back, and so which chains it must wait for.
    pub fn retired_object(&self) -> Option<Retired> {
        match *self {
            Self::DetachBacking { resource_id } | Self::ResourceUnref { resource_id }
            | Self::RetireHost3dRing { resource_id } | Self::DropHost3dRing { resource_id } =>
                Some(Retired::Resource(resource_id)),
            Self::CtxDestroy { ctx_id } | Self::CtxDetachResource { ctx_id, .. } =>
                Some(Retired::Context(ctx_id)),
            _ => None,
        }
    }
    /// Seat traffic, served while ordinary GPU work is parked.
    pub fn seat(&self) -> bool {
        matches!(self, Self::InputInfo { .. } | Self::InputDrain { .. }
            | Self::SeatState | Self::SeatQuiesced { .. })
    }
}
/// What a retirement gives back (GPU-DESIGN 4.5.15).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Retired { Resource(u32), Context(u32) }
/// How far one fenced chain can reach: virgl and Venus resolve a stream's ids
/// through its context's table, and a fenced transfer names one resource.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ChainReach {
    /// The device context the chain rode; 0 only for a context-less transfer.
    pub ctx: u32,
    /// The resource a fenced transfer names; 0 for a stream.
    pub res: u32,
}
impl ChainReach {
    /// `holds(ctx, res)`: whether a stream on `ctx` can name `res`.
    fn reaches(self, object: Retired, holds: &impl Fn(u32, u32) -> bool) -> bool {
        match object {
            Retired::Context(ctx) => self.ctx == ctx,
            Retired::Resource(res) if self.res != 0 => self.res == res,
            Retired::Resource(res) => holds(self.ctx, res),
        }
    }
}
/// What stands between a retirement and its object.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Hold {
    /// Nothing the device runs can reach it.
    Clear,
    /// Work that ends within `PARK_BOUND_MS` either way: it retires, or the
    /// abandonment bound turns it into a poisoned chain or a dead engine.
    Parks,
    /// Only what may never end: a poisoned chain on the object, a dead engine.
    Refused,
}
/// A poisoned chain counts for less than one in flight, because the park is
/// bounded and the chain in flight may yet retire; either way the poison is
/// still there when the park ends, and the retirement is refused then.
pub fn retirement_hold(
    object: Retired,
    dead: bool,
    sync_pending: bool,
    mut in_flight: impl Iterator<Item = ChainReach>,
    mut poisoned: impl Iterator<Item = ChainReach>,
    holds: impl Fn(u32, u32) -> bool,
) -> Hold {
    if dead {
        Hold::Refused
    } else if sync_pending || in_flight.any(|c| c.reaches(object, &holds)) {
        Hold::Parks
    } else if poisoned.any(|c| c.reaches(object, &holds)) {
        Hold::Refused
    } else {
        Hold::Clear
    }
}
/// Whether the broker runs `request` now or leaves it parked. A park must end:
/// a seat episode ends, and in-flight chains retire. A retirement nothing can
/// clear is admitted at once, and one parked `PARK_BOUND_MS` is admitted
/// whatever the cause; either way `Device::execute` refuses it, keeping every
/// pin, and the caller degrades instead of waiting forever on a reply.
pub fn admit(request: &Request, phase: u32, hold: Hold, parked_ms: u64) -> bool {
    request.seat()
        || (phase == 0 && (!request.retires() || hold != Hold::Parks || parked_ms >= PARK_BOUND_MS))
}
/// The longest a retirement waits for quiescence before it is refused: the
/// bound every fenced chain already carries (backend gpu.rs asserts the two
/// equal), past which the device's own abandonment has given up on it too.
pub const PARK_BOUND_MS: u64 = 30_000;
/// What a stalled engine or cursor lane becomes at a completion pump
/// (GPU-DESIGN 4.5.15): a chain that has retired revives it however late. One
/// still outstanding at the abandonment bound, or at the bound of a takeover
/// waiting on it, kills an engine, and abandons a cursor lane until that chain
/// retires.
#[derive(Debug, PartialEq, Eq)]
pub enum Stall { Holding, Revived, Dead }
/// How often a takeover held up by the device renews the kernel's deadline (vote 71).
pub const RENEW_EVERY_MS: u64 = 1000;
/// Whether a takeover held up by the device renews now. `last` is the generation of
/// the last renewal and the ms since it. The first sight in a generation renews at
/// once: a hold first seen late in the kernel's five seconds would otherwise wait a
/// second more and miss them.
pub fn renew_due(last: Option<(u64, u64)>, generation: u64) -> bool {
    match last {
        Some((renewed, since_ms)) if renewed == generation => since_ms >= RENEW_EVERY_MS,
        _ => true,
    }
}
/// How long a takeover waits on the device, counted from the chord (vote 80). A wait
/// behind the cursor lane and then one behind the control queue each have their own
/// abandonment bound and may run in series, so the takeover's own bound ends both
/// inside the kernel's SEAT_QUIESCE_LIMIT_NS (35 s): a control stall still standing
/// kills the engine, and a cursor stall abandons the lane.
pub const TAKEOVER_BOUND_MS: u64 = 33_000;
// A stall begun within TAKEOVER_BOUND_MS - PARK_BOUND_MS of the chord dies of its own
// bound first; any later one, a second wait in series or a takeover's own command
// stalling past the bound, meets this one.
const _: () = assert!(PARK_BOUND_MS < TAKEOVER_BOUND_MS);
/// `takeover_ms` is how long ago the chord was, while a takeover waits on the device.
pub fn stall_verdict(outstanding: bool, stalled_ms: u64, takeover_ms: Option<u64>) -> Stall {
    if !outstanding {
        Stall::Revived
    } else if stalled_ms >= PARK_BOUND_MS || takeover_ms.is_some_and(|ms| ms >= TAKEOVER_BOUND_MS) {
        Stall::Dead
    } else {
        Stall::Holding
    }
}
/// The control queue's engine as a reply left it (GPU-DESIGN 4.5.15).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Engine { #[default] Live, Stalled, Dead }
impl Wire for Engine {
    fn put(&self, out: &mut Vec<u8>) {
        match self { Self::Live => 0u8, Self::Stalled => 1, Self::Dead => 2 }.put(out);
    }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        Ok(match u8::get(r)? { 0 => Self::Live, 1 => Self::Stalled, 2 => Self::Dead,
            _ => return Err(Malformed) })
    }
}
/// What a failed span of synchronous steps says about the work it carried.
#[derive(Debug, PartialEq, Eq)]
pub enum FailureCause {
    /// A stall began inside the span, or the engine is still stalled: some
    /// step's outcome is unknown, so the failure says nothing about the work.
    Stall,
    /// Nothing will run again this boot.
    EngineDead,
    /// The device answered every step: the failure is the work's own.
    Answer,
}
/// `stalls_before` is the count adopted before the span's first step; the
/// count is monotone and wraps, so any change at all means a stall began.
pub fn failure_cause(stalls_before: u32, stalls_after: u32, engine: Engine) -> FailureCause {
    match engine {
        Engine::Dead => FailureCause::EngineDead,
        Engine::Stalled => FailureCause::Stall,
        Engine::Live if stalls_after != stalls_before => FailureCause::Stall,
        Engine::Live => FailureCause::Answer,
    }
}
impl Wire for Request {
    fn put(&self, out: &mut Vec<u8>) {
        1u16.put(out);
        match self {
            Self::Info => { 1u16.put(out); },
            Self::PairProtocolSelftest => { 2u16.put(out); },
            Self::QueryEdid => { 3u16.put(out); },
            Self::QueryDisplayInfo => { 4u16.put(out); },
            Self::ResourceCreate2d { resource_id, w, h } => { 5u16.put(out); resource_id.put(out); w.put(out); h.put(out); },
            Self::AttachBacking { resource_id, backing } => { 6u16.put(out); resource_id.put(out); backing.put(out); },
            Self::DetachBacking { resource_id } => { 7u16.put(out); resource_id.put(out); },
            Self::ResourceUnref { resource_id } => { 8u16.put(out); resource_id.put(out); },
            Self::Condemn { res_id } => { 9u16.put(out); res_id.put(out); },
            Self::ArmScanoutDisableRefusal => { 10u16.put(out); },
            Self::TakeInjectedRefusal => { 11u16.put(out); },
            Self::CondemnedCount => { 12u16.put(out); },
            Self::ResourceCreateBlob { resource_id, blob_mem, blob_flags, backing, len } => { 13u16.put(out); resource_id.put(out); blob_mem.put(out); blob_flags.put(out); backing.put(out); len.put(out); },
            Self::CreateRingBlob { resource_id, backing, len } => { 14u16.put(out); resource_id.put(out); backing.put(out); len.put(out); },
            Self::CreateHost3dBlob { resource_id, ctx_id, blob_flags, len, blob_id } => { 15u16.put(out); resource_id.put(out); ctx_id.put(out); blob_flags.put(out); len.put(out); blob_id.put(out); },
            Self::MapBlob { resource_id, offset } => { 16u16.put(out); resource_id.put(out); offset.put(out); },
            Self::UnmapBlob { resource_id } => { 17u16.put(out); resource_id.put(out); },
            Self::MintHost3dRing { res_id, ctx_id, len, blob_id } => { 18u16.put(out); res_id.put(out); ctx_id.put(out); len.put(out); blob_id.put(out); },
            Self::RetireHost3dRing { resource_id } => { 19u16.put(out); resource_id.put(out); },
            Self::HostmemParkCount => { 20u16.put(out); },
            Self::HostmemReapCount => { 21u16.put(out); },
            Self::DropHost3dRing { resource_id } => { 22u16.put(out); resource_id.put(out); },
            Self::CtxCreate { ctx_id, debug_name } => { 23u16.put(out); ctx_id.put(out); debug_name.put(out); },
            Self::CtxCreateVenus { ctx_id } => { 24u16.put(out); ctx_id.put(out); },
            Self::CtxCreateCapset { ctx_id, capset_id, debug_name } => { 25u16.put(out); ctx_id.put(out); capset_id.put(out); debug_name.put(out); },
            Self::CtxDestroy { ctx_id } => { 26u16.put(out); ctx_id.put(out); },
            Self::CtxAttachResource { ctx_id, resource_id } => { 27u16.put(out); ctx_id.put(out); resource_id.put(out); },
            Self::CtxDetachResource { ctx_id, resource_id } => { 28u16.put(out); ctx_id.put(out); resource_id.put(out); },
            Self::ResourceCreate3d { resource_id, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags } => { 29u16.put(out); resource_id.put(out); target.put(out); format.put(out); bind.put(out); width.put(out); height.put(out); depth.put(out); array_size.put(out); last_level.put(out); nr_samples.put(out); flags.put(out); },
            Self::SetScanout { resource_id, w, h } => { 30u16.put(out); resource_id.put(out); w.put(out); h.put(out); },
            Self::SetScanoutBlobProbe { resource_id, w, h, format, stride } => { 31u16.put(out); resource_id.put(out); w.put(out); h.put(out); format.put(out); stride.put(out); },
            Self::SetScanoutBlob { resource_id, w, h, format, stride } => { 32u16.put(out); resource_id.put(out); w.put(out); h.put(out); format.put(out); stride.put(out); },
            Self::SetScanoutBlobThenFlush { resource_id, w, h, format, stride } => { 33u16.put(out); resource_id.put(out); w.put(out); h.put(out); format.put(out); stride.put(out); },
            Self::CreatePresentable { res_id, ctx_id, len, blob_id } => { 34u16.put(out); res_id.put(out); ctx_id.put(out); len.put(out); blob_id.put(out); },
            Self::CtxAttachResourceProbe { ctx_id, resource_id } => { 35u16.put(out); ctx_id.put(out); resource_id.put(out); },
            Self::Transfer { resource_id, offset, x, y, w, h } => { 36u16.put(out); resource_id.put(out); offset.put(out); x.put(out); y.put(out); w.put(out); h.put(out); },
            Self::Flush { resource_id, x, y, w, h } => { 37u16.put(out); resource_id.put(out); x.put(out); y.put(out); w.put(out); h.put(out); },
            Self::TransferThenFlush { resource_id, offset, x, y, w, h } => { 38u16.put(out); resource_id.put(out); offset.put(out); x.put(out); y.put(out); w.put(out); h.put(out); },
            Self::Submit3d { ctx_id, ctx_pub, stream, ring_idx } => { 39u16.put(out); ctx_id.put(out); ctx_pub.put(out); stream.put(out); ring_idx.put(out); },
            Self::Submit3dSync { ctx_id, stream } => { 40u16.put(out); ctx_id.put(out); stream.put(out); },
            Self::TransferTo3dSync { ctx_id, res_id, w, h, stride } => { 41u16.put(out); ctx_id.put(out); res_id.put(out); w.put(out); h.put(out); stride.put(out); },
            Self::TransferTo3dBoxSync { ctx_id, res_id, x, y, w, h, offset, stride } => { 42u16.put(out); ctx_id.put(out); res_id.put(out); x.put(out); y.put(out); w.put(out); h.put(out); offset.put(out); stride.put(out); },
            Self::Transfer3d { to_host, ctx_id, ctx_pub, res_id, level, x, y, z, w, h, d, offset, stride, layer_stride } => { 43u16.put(out); to_host.put(out); ctx_id.put(out); ctx_pub.put(out); res_id.put(out); level.put(out); x.put(out); y.put(out); z.put(out); w.put(out); h.put(out); d.put(out); offset.put(out); stride.put(out); layer_stride.put(out); },
            Self::TransferFrom3dComp { ctx_id, ctx_pub, res_id, w, h, stride } => { 44u16.put(out); ctx_id.put(out); ctx_pub.put(out); res_id.put(out); w.put(out); h.put(out); stride.put(out); },
            Self::TransferFrom3dSync { ctx_id, res_id, w, h, stride } => { 45u16.put(out); ctx_id.put(out); res_id.put(out); w.put(out); h.put(out); stride.put(out); },
            Self::TransferFrom3dBoxSync { ctx_id, res_id, x, y, w, h, offset, stride } => { 46u16.put(out); ctx_id.put(out); res_id.put(out); x.put(out); y.put(out); w.put(out); h.put(out); offset.put(out); stride.put(out); },
            Self::PollCompletions => { 47u16.put(out); },
            Self::TakeCompletions => { 48u16.put(out); },
            Self::TakeVindications => { 49u16.put(out); },
            Self::TestHoldCtx { ctx_pub } => { 50u16.put(out); ctx_pub.put(out); },
            Self::TestHoldCtxCurrent => { 51u16.put(out); },
            Self::TestAbandonCtx { ctx_pub } => { 52u16.put(out); ctx_pub.put(out); },
            Self::TestHoldCtxDied { ctx_pub } => { 53u16.put(out); ctx_pub.put(out); },
            Self::TestAbandonedTotal => { 54u16.put(out); },
            Self::TestArmLatch => { 67u16.put(out); },
            Self::TestStallNext => { 68u16.put(out); },
            Self::TestArmChained => { 69u16.put(out); },
            Self::FencedHeld => { 55u16.put(out); },
            Self::TestFencedFree => { 56u16.put(out); },
            Self::CompSlotState => { 57u16.put(out); },
            Self::CtxFencesInFlight { ctx_pub } => { 58u16.put(out); ctx_pub.put(out); },
            Self::CtxHasPoisonedSlot { ctx_pub } => { 59u16.put(out); ctx_pub.put(out); },
            Self::FencedInFlight => { 60u16.put(out); },
            Self::EngineDead => { 61u16.put(out); },
            Self::InputInfo { index } => { 62u16.put(out); index.put(out); },
            Self::InputDrain { index } => { 63u16.put(out); index.put(out); },
            Self::SeatState => { 64u16.put(out); },
            Self::SeatQuiesced { generation } => { 66u16.put(out); generation.put(out); },
            Self::Cursor { shape, scale, x, y, visible } => { 65u16.put(out); shape.put(out); scale.put(out); x.put(out); y.put(out); visible.put(out); },
        }
    }
    fn get(r: &mut Reader<'_>) -> Result<Self, Malformed> {
        if u16::get(r)? != 1 { return Err(Malformed); }
        Ok(match u16::get(r)? {
            1 => Self::Info,
            2 => Self::PairProtocolSelftest,
            3 => Self::QueryEdid,
            4 => Self::QueryDisplayInfo,
            5 => Self::ResourceCreate2d { resource_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)? },
            6 => Self::AttachBacking { resource_id: <u32>::get(r)?, backing: <BufferRef>::get(r)? },
            7 => Self::DetachBacking { resource_id: <u32>::get(r)? },
            8 => Self::ResourceUnref { resource_id: <u32>::get(r)? },
            9 => Self::Condemn { res_id: <u32>::get(r)? },
            10 => Self::ArmScanoutDisableRefusal,
            11 => Self::TakeInjectedRefusal,
            12 => Self::CondemnedCount,
            13 => Self::ResourceCreateBlob { resource_id: <u32>::get(r)?, blob_mem: <u32>::get(r)?, blob_flags: <u32>::get(r)?, backing: <BufferRef>::get(r)?, len: <u32>::get(r)? },
            14 => Self::CreateRingBlob { resource_id: <u32>::get(r)?, backing: <BufferRef>::get(r)?, len: <u32>::get(r)? },
            15 => Self::CreateHost3dBlob { resource_id: <u32>::get(r)?, ctx_id: <u32>::get(r)?, blob_flags: <u32>::get(r)?, len: <u32>::get(r)?, blob_id: <u64>::get(r)? },
            16 => Self::MapBlob { resource_id: <u32>::get(r)?, offset: <u64>::get(r)? },
            17 => Self::UnmapBlob { resource_id: <u32>::get(r)? },
            18 => Self::MintHost3dRing { res_id: <u32>::get(r)?, ctx_id: <u32>::get(r)?, len: <u32>::get(r)?, blob_id: <u64>::get(r)? },
            19 => Self::RetireHost3dRing { resource_id: <u32>::get(r)? },
            20 => Self::HostmemParkCount,
            21 => Self::HostmemReapCount,
            22 => Self::DropHost3dRing { resource_id: <u32>::get(r)? },
            23 => Self::CtxCreate { ctx_id: <u32>::get(r)?, debug_name: <Vec<u8>>::get(r)? },
            24 => Self::CtxCreateVenus { ctx_id: <u32>::get(r)? },
            25 => Self::CtxCreateCapset { ctx_id: <u32>::get(r)?, capset_id: <u32>::get(r)?, debug_name: <Vec<u8>>::get(r)? },
            26 => Self::CtxDestroy { ctx_id: <u32>::get(r)? },
            27 => Self::CtxAttachResource { ctx_id: <u32>::get(r)?, resource_id: <u32>::get(r)? },
            28 => Self::CtxDetachResource { ctx_id: <u32>::get(r)?, resource_id: <u32>::get(r)? },
            29 => Self::ResourceCreate3d { resource_id: <u32>::get(r)?, target: <u32>::get(r)?, format: <u32>::get(r)?, bind: <u32>::get(r)?, width: <u32>::get(r)?, height: <u32>::get(r)?, depth: <u32>::get(r)?, array_size: <u32>::get(r)?, last_level: <u32>::get(r)?, nr_samples: <u32>::get(r)?, flags: <u32>::get(r)? },
            30 => Self::SetScanout { resource_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)? },
            31 => Self::SetScanoutBlobProbe { resource_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, format: <u32>::get(r)?, stride: <u32>::get(r)? },
            32 => Self::SetScanoutBlob { resource_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, format: <u32>::get(r)?, stride: <u32>::get(r)? },
            33 => Self::SetScanoutBlobThenFlush { resource_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, format: <u32>::get(r)?, stride: <u32>::get(r)? },
            34 => Self::CreatePresentable { res_id: <u32>::get(r)?, ctx_id: <u32>::get(r)?, len: <u32>::get(r)?, blob_id: <u64>::get(r)? },
            35 => Self::CtxAttachResourceProbe { ctx_id: <u32>::get(r)?, resource_id: <u32>::get(r)? },
            36 => Self::Transfer { resource_id: <u32>::get(r)?, offset: <u64>::get(r)?, x: <u32>::get(r)?, y: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)? },
            37 => Self::Flush { resource_id: <u32>::get(r)?, x: <u32>::get(r)?, y: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)? },
            38 => Self::TransferThenFlush { resource_id: <u32>::get(r)?, offset: <u64>::get(r)?, x: <u32>::get(r)?, y: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)? },
            39 => Self::Submit3d { ctx_id: <u32>::get(r)?, ctx_pub: <u32>::get(r)?, stream: <Vec<u8>>::get(r)?, ring_idx: <u8>::get(r)? },
            40 => Self::Submit3dSync { ctx_id: <u32>::get(r)?, stream: <Vec<u8>>::get(r)? },
            41 => Self::TransferTo3dSync { ctx_id: <u32>::get(r)?, res_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, stride: <u32>::get(r)? },
            42 => Self::TransferTo3dBoxSync { ctx_id: <u32>::get(r)?, res_id: <u32>::get(r)?, x: <u32>::get(r)?, y: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, offset: <u64>::get(r)?, stride: <u32>::get(r)? },
            43 => Self::Transfer3d { to_host: <bool>::get(r)?, ctx_id: <u32>::get(r)?, ctx_pub: <u32>::get(r)?, res_id: <u32>::get(r)?, level: <u32>::get(r)?, x: <u32>::get(r)?, y: <u32>::get(r)?, z: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, d: <u32>::get(r)?, offset: <u64>::get(r)?, stride: <u32>::get(r)?, layer_stride: <u32>::get(r)? },
            44 => Self::TransferFrom3dComp { ctx_id: <u32>::get(r)?, ctx_pub: <u32>::get(r)?, res_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, stride: <u32>::get(r)? },
            45 => Self::TransferFrom3dSync { ctx_id: <u32>::get(r)?, res_id: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, stride: <u32>::get(r)? },
            46 => Self::TransferFrom3dBoxSync { ctx_id: <u32>::get(r)?, res_id: <u32>::get(r)?, x: <u32>::get(r)?, y: <u32>::get(r)?, w: <u32>::get(r)?, h: <u32>::get(r)?, offset: <u64>::get(r)?, stride: <u32>::get(r)? },
            47 => Self::PollCompletions,
            48 => Self::TakeCompletions,
            49 => Self::TakeVindications,
            50 => Self::TestHoldCtx { ctx_pub: <Option<u32>>::get(r)? },
            51 => Self::TestHoldCtxCurrent,
            52 => Self::TestAbandonCtx { ctx_pub: <u32>::get(r)? },
            53 => Self::TestHoldCtxDied { ctx_pub: <u32>::get(r)? },
            54 => Self::TestAbandonedTotal,
            67 => Self::TestArmLatch,
            68 => Self::TestStallNext,
            69 => Self::TestArmChained,
            55 => Self::FencedHeld,
            56 => Self::TestFencedFree,
            57 => Self::CompSlotState,
            58 => Self::CtxFencesInFlight { ctx_pub: <u32>::get(r)? },
            59 => Self::CtxHasPoisonedSlot { ctx_pub: <u32>::get(r)? },
            60 => Self::FencedInFlight,
            61 => Self::EngineDead,
            62 => Self::InputInfo { index: <u32>::get(r)? },
            63 => Self::InputDrain { index: <u32>::get(r)? },
            64 => Self::SeatState,
            66 => Self::SeatQuiesced { generation: u64::get(r)? },
            65 => Self::Cursor { shape: u8::get(r)?, scale: u16::get(r)?, x: u32::get(r)?, y: u32::get(r)?, visible: bool::get(r)? },
            _ => return Err(Malformed),
        })
    }
}

/// Every reply carries these, an error reply included: a caller reads a
/// failed step against the engine state it left (`failure_cause`).
#[derive(Default, Clone, Copy)]
pub struct Stats {
    pub cmd_seq: u64, pub last_scanout_seq: u64, pub last_unref_seq: u64,
    pub condemned_lost: u32, pub engine: Engine, pub stalls: u32,
}
record!(Stats { cmd_seq: u64, last_scanout_seq: u64, last_unref_seq: u64, condemned_lost: u32,
    engine: Engine, stalls: u32 });
#[cfg(feature = "backend")]
impl From<&crate::backend::gpu::Gpu> for Stats {
    fn from(g: &crate::backend::gpu::Gpu) -> Self {
        Self { cmd_seq: g.cmd_seq, last_scanout_seq: g.last_scanout_seq,
            last_unref_seq: g.last_unref_seq, condemned_lost: g.condemned_lost,
            engine: g.engine(), stalls: g.stalls() }
    }
}
/// A host allocation's description. The mapping arrives through authenticated
/// 9P Rweft on its ring fid, never as a device-private address in a ctl reply.
pub struct RingInfo { pub res_id: u32, pub size: u64, pub cache: u64 }
record!(RingInfo { res_id: u32, size: u64, cache: u64 });

#[cfg(test)]
mod admission_tests {
    use super::*;
    use alloc::vec;
    fn retirements() -> Vec<Request> {
        vec![Request::DetachBacking { resource_id: 1 }, Request::ResourceUnref { resource_id: 1 },
            Request::CtxDestroy { ctx_id: 1 }, Request::CtxDetachResource { ctx_id: 1, resource_id: 1 },
            Request::RetireHost3dRing { resource_id: 1 }, Request::DropHost3dRing { resource_id: 1 }]
    }
    fn ordinary() -> Vec<Request> {
        vec![Request::Info, Request::Submit3dSync { ctx_id: 1, stream: Vec::new() },
            Request::CtxCreate { ctx_id: 1, debug_name: Vec::new() },
            Request::Flush { resource_id: 1, x: 0, y: 0, w: 1, h: 1 }, Request::EngineDead,
            Request::TestArmLatch, Request::TestStallNext, Request::TestArmChained]
    }
    fn seat() -> Vec<Request> {
        vec![Request::InputInfo { index: 0 }, Request::InputDrain { index: 0 }, Request::SeatState,
            Request::SeatQuiesced { generation: 1 }]
    }
    #[test]
    fn the_classes_are_disjoint() {
        for r in retirements() { assert!(r.retires() && !r.seat(), "{r:?}"); }
        for r in ordinary() { assert!(!r.retires() && !r.seat(), "{r:?}"); }
        for r in seat() { assert!(r.seat() && !r.retires(), "{r:?}"); }
    }
    #[test]
    fn a_retirement_nothing_can_clear_runs_to_its_refusal() {
        // Parked, it would wait on what may never end, and the compositor
        // blocks on the reply for as long as the bound.
        for r in retirements() { assert!(admit(&r, 0, Hold::Refused, 0), "{r:?} parked on what may never end"); }
    }
    #[test]
    fn a_retirement_waits_for_a_chain_in_flight_on_a_live_engine() {
        for r in retirements() {
            assert!(!admit(&r, 0, Hold::Parks, 0), "{r:?} ran before its chains retired");
            assert!(admit(&r, 0, Hold::Clear, 0), "{r:?} parked with nothing in its way");
        }
    }
    #[test]
    fn a_retirement_parked_to_the_bound_runs_to_its_refusal() {
        for r in retirements() {
            assert!(!admit(&r, 0, Hold::Parks, PARK_BOUND_MS - 1), "{r:?} ran before the bound");
            assert!(admit(&r, 0, Hold::Parks, PARK_BOUND_MS), "{r:?} still parked at the bound");
        }
    }
    #[test]
    fn each_retirement_names_its_object_and_nothing_else_retires() {
        for (r, want) in [
            (Request::DetachBacking { resource_id: 7 }, Retired::Resource(7)),
            (Request::ResourceUnref { resource_id: 7 }, Retired::Resource(7)),
            (Request::RetireHost3dRing { resource_id: 7 }, Retired::Resource(7)),
            (Request::DropHost3dRing { resource_id: 7 }, Retired::Resource(7)),
            (Request::CtxDestroy { ctx_id: 3 }, Retired::Context(3)),
            // The link it removes is the context's: only its chains can use it.
            (Request::CtxDetachResource { ctx_id: 3, resource_id: 7 }, Retired::Context(3)),
        ] {
            assert_eq!(r.retired_object(), Some(want), "{r:?}");
        }
        for r in ordinary().into_iter().chain(seat()) {
            assert_eq!(r.retired_object(), None, "{r:?}");
        }
    }
    /// Chain `C` on context 3, which holds resource 7 and nothing else.
    fn hold(object: Retired, in_flight: &[ChainReach], poisoned: &[ChainReach]) -> Hold {
        retirement_hold(object, false, false, in_flight.iter().copied(), poisoned.iter().copied(),
            |ctx, res| (ctx, res) == (3, 7))
    }
    const STREAM: ChainReach = ChainReach { ctx: 3, res: 0 };
    #[test]
    fn a_stream_in_flight_parks_only_what_its_context_can_reach() {
        assert_eq!(hold(Retired::Resource(7), &[STREAM], &[]), Hold::Parks);
        assert_eq!(hold(Retired::Context(3), &[STREAM], &[]), Hold::Parks);
        assert_eq!(hold(Retired::Resource(8), &[STREAM], &[]), Hold::Clear, "not attached to it");
        assert_eq!(hold(Retired::Context(4), &[STREAM], &[]), Hold::Clear, "another context");
    }
    #[test]
    fn a_poisoned_stream_refuses_only_what_its_context_can_reach() {
        assert_eq!(hold(Retired::Resource(7), &[], &[STREAM]), Hold::Refused);
        assert_eq!(hold(Retired::Context(3), &[], &[STREAM]), Hold::Refused);
        assert_eq!(hold(Retired::Resource(8), &[], &[STREAM]), Hold::Clear, "another client's surface");
        assert_eq!(hold(Retired::Context(4), &[], &[STREAM]), Hold::Clear, "another client's context");
    }
    #[test]
    fn a_transfer_holds_the_one_resource_it_names_and_its_context() {
        let xfer = ChainReach { ctx: 3, res: 9 };
        for (slots, want) in [((&[xfer][..], &[][..]), Hold::Parks), ((&[][..], &[xfer][..]), Hold::Refused)] {
            let (in_flight, poisoned) = slots;
            assert_eq!(hold(Retired::Resource(9), in_flight, poisoned), want, "named, though unattached");
            assert_eq!(hold(Retired::Context(3), in_flight, poisoned), want, "its context");
            assert_eq!(hold(Retired::Resource(7), in_flight, poisoned), Hold::Clear,
                "its context holds this, but a transfer names only the one");
        }
    }
    #[test]
    fn a_transfer_with_no_context_is_reached_by_name_alone() {
        let xfer = ChainReach { ctx: 0, res: 9 };
        assert_eq!(hold(Retired::Resource(9), &[xfer], &[]), Hold::Parks);
        assert_eq!(hold(Retired::Resource(7), &[xfer], &[]), Hold::Clear);
        assert_eq!(hold(Retired::Context(3), &[xfer], &[]), Hold::Clear);
    }
    #[test]
    fn a_chain_in_flight_parks_ahead_of_a_poisoned_one_on_the_same_object() {
        assert_eq!(hold(Retired::Resource(7), &[STREAM], &[STREAM]), Hold::Parks);
    }
    #[test]
    fn a_chain_in_flight_elsewhere_does_not_soften_a_refusal() {
        let elsewhere = ChainReach { ctx: 4, res: 0 };
        assert_eq!(hold(Retired::Resource(7), &[elsewhere], &[STREAM]), Hold::Refused);
    }
    #[test]
    fn the_device_wide_work_holds_every_object_and_a_dead_engine_refuses_every_one() {
        let held = |dead, sync, object| retirement_hold(object, dead, sync,
            core::iter::empty(), core::iter::empty(), |_, _| false);
        for object in [Retired::Resource(8), Retired::Context(4)] {
            assert_eq!(held(false, false, object), Hold::Clear);
            assert_eq!(held(false, true, object), Hold::Parks);
            assert_eq!(held(true, false, object), Hold::Refused);
            assert_eq!(held(true, true, object), Hold::Refused, "death outranks the sync chain");
        }
    }
    #[test]
    fn an_episode_parks_every_ordinary_request_whatever_the_engine() {
        // The bound is phase 0's: an episode's own deadlines end its park, and
        // ordinary admission stays closed for the trusted path's whole span.
        for phase in [1, 2, 3, 4] {
            for r in retirements().into_iter().chain(ordinary()) {
                for held in [Hold::Clear, Hold::Parks, Hold::Refused] {
                    for parked in [0, PARK_BOUND_MS, u64::MAX] {
                        assert!(!admit(&r, phase, held, parked), "{r:?} ran in phase {phase}");
                    }
                }
            }
        }
    }
    #[test]
    fn a_retired_chain_revives_the_engine_however_late() {
        // The device proved it is done with the buffers; the clock's verdict
        // is only for a chain that never comes back.
        for ms in [0, PARK_BOUND_MS - 1, PARK_BOUND_MS, u64::MAX] {
            for chord in [None, Some(0), Some(TAKEOVER_BOUND_MS), Some(u64::MAX)] {
                assert_eq!(stall_verdict(false, ms, chord), Stall::Revived, "retired at {ms} ms, chord {chord:?}");
            }
        }
    }
    #[test]
    fn an_outstanding_chain_holds_the_stall_until_the_bound_then_dies() {
        assert_eq!(stall_verdict(true, 0, None), Stall::Holding);
        assert_eq!(stall_verdict(true, PARK_BOUND_MS - 1, None), Stall::Holding);
        assert_eq!(stall_verdict(true, PARK_BOUND_MS, None), Stall::Dead);
        assert_eq!(stall_verdict(true, u64::MAX, None), Stall::Dead);
        assert_eq!(stall_verdict(true, PARK_BOUND_MS, Some(0)), Stall::Dead, "a takeover never lengthens a stall");
    }
    #[test]
    fn a_takeover_wait_ends_at_the_bound_from_the_chord_however_young_the_stall() {
        // Vote 80: the second of two waits in series began late in the chord's limit.
        assert_eq!(stall_verdict(true, 0, Some(TAKEOVER_BOUND_MS - 1)), Stall::Holding);
        assert_eq!(stall_verdict(true, 0, Some(TAKEOVER_BOUND_MS)), Stall::Dead);
        assert_eq!(stall_verdict(true, PARK_BOUND_MS - 1, Some(TAKEOVER_BOUND_MS - 1)), Stall::Holding);
        assert_eq!(stall_verdict(true, 0, Some(u64::MAX)), Stall::Dead);
    }
    fn seat_h_ms(name: &str) -> u64 {
        let header = include_str!("../../../kernel/include/thylacine/seat.h");
        let prefix = alloc::format!("#define {name} ");
        let line = header.lines().find(|l| l.starts_with(prefix.as_str()))
            .unwrap_or_else(|| panic!("seat.h defines {name}"));
        let ns: u64 = line.split_whitespace().nth(2).unwrap().trim_end_matches("ull").parse().unwrap();
        ns / 1_000_000
    }
    #[test]
    fn the_takeover_bound_falls_inside_the_kernels_limit() {
        let limit_ms = seat_h_ms("SEAT_QUIESCE_LIMIT_NS");
        // Past the bound, a renewal a second old still covers one more pump before the
        // kernel's limit fails the chord.
        assert!(TAKEOVER_BOUND_MS + RENEW_EVERY_MS < limit_ms, "bound {TAKEOVER_BOUND_MS} ms, limit {limit_ms} ms");
    }
    #[test]
    fn a_renewal_a_second_late_still_lands_inside_the_kernels_deadline() {
        // The bound decides a chord only while the kernel's deadline is still ahead of it:
        // renewals at RENEW_EVERY_MS, a pass late, must keep landing inside SEAT_QUIESCE_NS.
        let quiesce_ms = seat_h_ms("SEAT_QUIESCE_NS");
        assert!(2 * RENEW_EVERY_MS < quiesce_ms, "renew every {RENEW_EVERY_MS} ms, deadline {quiesce_ms} ms");
    }
    #[test]
    fn a_hold_first_seen_renews_at_once_then_once_a_second() {
        assert!(renew_due(None, 7));
        assert!(renew_due(Some((6, 0)), 7), "a renewal of an earlier generation is not this one's");
        assert!(!renew_due(Some((7, 0)), 7));
        assert!(!renew_due(Some((7, RENEW_EVERY_MS - 1)), 7));
        assert!(renew_due(Some((7, RENEW_EVERY_MS)), 7));
    }
    #[test]
    fn a_failure_on_a_live_engine_with_no_stall_is_the_works_own() {
        for n in [0, 7, u32::MAX] {
            assert_eq!(failure_cause(n, n, Engine::Live), FailureCause::Answer, "stalls {n}");
        }
    }
    #[test]
    fn a_stall_begun_inside_the_span_leaves_the_outcome_unknown_even_once_revived() {
        assert_eq!(failure_cause(0, 1, Engine::Live), FailureCause::Stall);
        assert_eq!(failure_cause(u32::MAX, 0, Engine::Live), FailureCause::Stall, "the count wraps");
        assert_eq!(failure_cause(3, 4, Engine::Stalled), FailureCause::Stall);
    }
    #[test]
    fn a_step_refused_by_a_stall_begun_before_the_span_is_unknown_too() {
        assert_eq!(failure_cause(5, 5, Engine::Stalled), FailureCause::Stall);
    }
    #[test]
    fn a_dead_engine_outranks_every_stall() {
        for (before, after) in [(0, 0), (0, 1), (u32::MAX, 0)] {
            assert_eq!(failure_cause(before, after, Engine::Dead), FailureCause::EngineDead);
        }
    }
    #[test]
    fn seat_traffic_and_ordinary_work_run_whatever_the_hold() {
        for held in [Hold::Clear, Hold::Parks, Hold::Refused] {
            for r in ordinary() { assert!(admit(&r, 0, held, 0), "{r:?}"); }
            for phase in [0, 1, 2, 3, 4] {
                for r in seat() { assert!(admit(&r, phase, held, 0), "{r:?} in phase {phase}"); }
            }
        }
    }
}

#[cfg(test)]
mod park_tests {
    use super::*;
    use alloc::vec;
    const A: u32 = 3;
    const B: u32 = 4;
    const N: Released = Released(Vec::new());
    #[test]
    fn a_readback_parks_only_behind_its_own_contexts_device_work() {
        assert!(must_park(&[], A, true, true, &N), "its context has a chain on the device");
        assert!(!must_park(&[], A, true, false, &N), "nothing ahead of it: it publishes");
        assert!(!must_park(&[], A, false, true, &N), "a submit behind a submit is the device's order");
        assert!(!must_park(&[(B, true)], A, true, false, &N), "another context's parked readback");
    }
    #[test]
    fn everything_behind_a_parked_chain_of_its_context_parks() {
        assert!(must_park(&[(A, true)], A, false, false, &N), "a submit behind the readback");
        assert!(must_park(&[(A, true), (A, false)], A, true, false, &N), "a second readback");
        assert!(!must_park(&[(A, true)], B, false, true, &N), "another context's submit");
    }
    use Unpark::{Keep as K, Publish as P, Release as R};
    #[test]
    fn a_readback_goes_with_the_chains_behind_it_up_to_the_next_readback() {
        let parked = [(A, true), (A, false), (A, false), (A, true), (A, false)];
        assert_eq!(unpark_plan(&parked, &|_| false, &|_| false, &N), vec![P, P, P, K, K]);
        assert_eq!(unpark_plan(&parked, &|c| c == A, &|_| false, &N), vec![K; 5], "its prefix is on the device");
    }
    #[test]
    fn contexts_unpark_independently_and_keep_their_order() {
        let parked = [(A, true), (B, true), (A, false), (B, false)];
        assert_eq!(unpark_plan(&parked, &|c| c == A, &|_| false, &N), vec![K, P, K, P]);
        assert_eq!(unpark_plan(&parked, &|c| c == B, &|_| false, &N), vec![P, K, P, K]);
    }
    #[test]
    fn a_poisoned_contexts_parked_chains_are_released_and_no_one_elses() {
        let parked = [(A, true), (B, true), (A, false), (B, false)];
        // A's abandoned chain is still "on the device" too: release wins over waiting.
        assert_eq!(unpark_plan(&parked, &|c| c == A || c == B, &|c| c == A, &N), vec![R, K, R, K]);
        assert_eq!(unpark_plan(&parked, &|c| c == A, &|c| c == A, &N), vec![R, P, R, P]);
    }
    fn released_tag(ctx_pub: u32, comp: bool) -> FenceTag {
        FenceTag { fence_id: 7, ctx_pub, readback: true, comp, abandoned: false, ok: false,
            released: true, ring_idx: 0, parked_ms: 0, exec_ms: 0 }
    }
    const A_DEV: u32 = 1;
    #[test]
    fn nothing_of_a_context_runs_after_its_released_chain_while_its_device_context_lives() {
        // A's readback is released behind its abandoned chain; the seam has not
        // pumped yet, so it admits A's next submit from the older view.
        let mut rel = Released::default();
        assert_eq!(unpark_plan(&[(A, true)], &|c| c == A, &|c| c == A, &rel), vec![R]);
        let mut parked = released_tag(A, false);
        (parked.ok, parked.released) = (true, false);
        let tag = release_tag(parked, 5, A_DEV, &mut rel);
        assert!(tag.released && !tag.ok && !tag.abandoned && tag.parked_ms == 5);
        assert!(must_park(&[], A, false, false, &rel), "nothing of A parked or on the device: still parks");
        assert!(!must_park(&[], A, false, false, &N), "control: without the record it publishes");
        assert!(!must_park(&[], B, false, false, &rel), "another context publishes");
        // The pump releases it even once the abandoned chain retired late.
        assert_eq!(unpark_plan(&[(A, false), (B, false)], &|_| false, &|_| false, &rel), vec![R, P]);
        assert_eq!(unpark_plan(&[(A, false), (B, false)], &|_| false, &|_| false, &N), vec![P, P]);
        assert!(must_park(&[], A, true, false, &rel), "a readback after the vindication too");
        rel.forget(A_DEV);
        assert!(!must_park(&[], A, false, false, &rel), "a new device context is a new stream");
    }
    #[test]
    fn a_released_record_is_one_per_device_context_and_never_a_compositor_readbacks() {
        let mut rel = Released::default();
        rel.note(A_DEV, &released_tag(B, true));
        assert!(!rel.holds(B), "a compositor readback's release leaves the client's stream");
        rel.note(A_DEV, &released_tag(A, false));
        rel.note(A_DEV, &released_tag(B, false));
        assert!(rel.holds(B) && !rel.holds(A));
        assert_eq!(rel.0.len(), 1);
    }
    #[test]
    fn a_tag_carries_its_park_and_its_execution_across_the_seam() {
        let tag = FenceTag { fence_id: 9, ctx_pub: A, readback: true, comp: false, abandoned: false,
            ok: false, released: true, ring_idx: 2, parked_ms: 0x0102_0304, exec_ms: 0x0506_0708 };
        let mut bytes = Vec::new();
        tag.put(&mut bytes);
        assert_eq!(FenceTag::get(&mut Reader::new(&bytes).unwrap()).unwrap(), tag);
    }
}

#[cfg(test)]
mod cursor_wire_tests {
    use super::*;
    #[test]
    fn seat_quiescence_keeps_cursor_opcode_distinct() {
        let mut bytes=Vec::new();
        Request::SeatQuiesced{generation:0x0807060504030201}.put(&mut bytes);
        assert_eq!(bytes,[1,0,66,0,1,2,3,4,5,6,7,8]);
        let mut r=Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(),Request::SeatQuiesced{generation:0x0807060504030201}));
        r.finish().unwrap();
    }
    #[test]
    fn the_latch_lever_has_its_own_opcode() {
        let mut bytes = Vec::new();
        Request::TestArmLatch.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 67, 0]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::TestArmLatch));
        r.finish().unwrap();
    }
    #[test]
    fn the_chained_lever_has_its_own_opcode() {
        let mut bytes = Vec::new();
        Request::TestArmChained.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 69, 0]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::TestArmChained));
        r.finish().unwrap();
    }
    #[test]
    fn the_stall_lever_has_its_own_opcode() {
        let mut bytes = Vec::new();
        Request::TestStallNext.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 68, 0]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::TestStallNext));
        r.finish().unwrap();
    }
    #[test]
    fn stats_carry_the_engine_and_refuse_an_unknown_state() {
        let mut bytes = Vec::new();
        Stats { engine: Engine::Stalled, stalls: 0x0403_0201, ..Stats::default() }.put(&mut bytes);
        assert_eq!(&bytes[bytes.len() - 5..], [1, 1, 2, 3, 4]);
        let s = Stats::get(&mut Reader::new(&bytes).unwrap()).unwrap();
        assert_eq!((s.engine, s.stalls), (Engine::Stalled, 0x0403_0201));
        let at = bytes.len() - 5;
        bytes[at] = 3;
        assert!(Stats::get(&mut Reader::new(&bytes).unwrap()).is_err());
    }
    #[test]
    fn cursor_record_is_exact_and_rejects_truncation_and_bad_bool() {
        let mut bytes = Vec::new();
        Request::Cursor { shape: 4, scale: 175, x: 123, y: 456, visible: true }.put(&mut bytes);
        assert_eq!(bytes, [1,0,65,0,4,175,0,123,0,0,0,200,1,0,0,1]);
        for end in 0..bytes.len() {
            let mut r = Reader::new(&bytes[..end]).unwrap();
            assert!(Request::get(&mut r).is_err());
        }
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::Cursor {
            shape: 4, scale: 175, x: 123, y: 456, visible: true }));
        r.finish().unwrap();
        bytes.push(0);
        let mut r = Reader::new(&bytes).unwrap();
        Request::get(&mut r).unwrap();
        assert!(r.finish().is_err());
        bytes.pop(); *bytes.last_mut().unwrap() = 2;
        assert!(Request::get(&mut Reader::new(&bytes).unwrap()).is_err());
    }
}
