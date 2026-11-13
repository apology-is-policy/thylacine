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
/// A chain that can reach an owed free (`owed`, `OwedReach::reaches`) parks
/// until the free is paid.
pub fn must_park(parked: &[(u32, bool)], ctx: u32, readback: bool, on_device: bool, owed: bool, released: &Released) -> bool {
    if owed || released.holds(ctx) {
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
/// Held by pub: a seam context's chains ride two device contexts (GL and
/// venus), and the seam reissues no pub while an entry may still name it.
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
/// released chain, for as long as its device context lives. A chain that can
/// reach an owed free (`owed`, by index) stays, and its context's later
/// chains with it, until the free is paid.
pub fn unpark_plan(
    parked: &[(u32, bool)],
    on_device: &dyn Fn(u32) -> bool,
    poisoned: &dyn Fn(u32) -> bool,
    owed: &dyn Fn(usize) -> bool,
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
        if owed(i) || (readback && (sent.contains(&ctx) || on_device(ctx))) {
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
    /// Test lever: the next trusted episode's restore has its rebind of the
    /// compositor's frame refused, so the seat returns to a blank output.
    TestArmRestore,
    /// Test lever: one stall shot outside the latch witness's budget, for the
    /// compositor to aim at a command of its own; the caller bounds how often.
    TestStallShot,
    /// Test lever: for a bounded time the owed-free pass passes over the
    /// frees a stream on `ctx_id` can reach (`on`), or no longer (`!on`, its
    /// own only); the broker bounds how often.
    TestOwedDefer { ctx_id: u32, on: bool },
    /// Test lever: one stall shot on the owed-free pass's next command for
    /// `resource_id`'s free, for a bounded time; the broker bounds how often.
    TestOwedStall { resource_id: u32 },
    FencedHeld,
    TestFencedFree,
    CompSlotState,
    CtxFencesInFlight { ctx_pub: u32 },
    CtxHasPoisonedSlot { ctx_pub: u32 },
    FencedInFlight,
    EngineDead,
    /// The frees the broker owes the device and repeats (`Frees`).
    FreesOwed,
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
/// The owed frees a chain may not go to the device past (GPU-DESIGN 4.5.15):
/// their ids, and the device contexts whose streams can name one (`holds`).
/// A free's repeat waits for every chain on the device that can reach it, so
/// a context that kept a chain there would hold the free off for as long as
/// it kept rendering; a chain that can reach one parks instead, and the free
/// waits only on chains already on the device, which end. Only frees no
/// poisoned chain reaches are listed: one a poisoned chain reaches waits for
/// the vindication whatever else runs, and a chain parked behind it would
/// wait as long with no clock of its own.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct OwedReach {
    res: Vec<u32>,
    ctxs: Vec<u32>,
}
impl OwedReach {
    fn new(mut res: Vec<u32>, mut ctxs: Vec<u32>) -> Self {
        res.sort_unstable();
        res.dedup();
        ctxs.sort_unstable();
        ctxs.dedup();
        OwedReach { res, ctxs }
    }
    /// The frees in `owed` no chain in `poisoned` can reach, and the contexts
    /// that can reach one of them, less every context that can reach a free
    /// in `standing_down`: a free's stand-down lets its holders' streams go
    /// whatever else they hold, or a context holding two would never have a
    /// window. A transfer naming a free in `owed` still parks. `held_by(ctx)`
    /// is what a stream on `ctx` can name, `holders(set)` the contexts that
    /// can name one of a set.
    pub fn of(
        mut owed: Vec<u32>,
        standing_down: &[u32],
        poisoned: &[ChainReach],
        held_by: impl Fn(u32) -> Vec<u32>,
        holders: impl Fn(&dyn Fn(u32) -> bool) -> Vec<u32>,
    ) -> Self {
        if owed.is_empty() {
            return OwedReach::default();
        }
        for p in poisoned {
            if p.res != 0 {
                owed.retain(|&r| r != p.res);
            } else {
                let mut held = held_by(p.ctx);
                held.sort_unstable();
                owed.retain(|r| held.binary_search(r).is_err());
            }
        }
        if owed.is_empty() {
            return OwedReach::default();
        }
        owed.sort_unstable();
        owed.dedup();
        let mut ctxs = holders(&|r| owed.binary_search(&r).is_ok());
        if !standing_down.is_empty() {
            let released = holders(&|r| standing_down.contains(&r));
            ctxs.retain(|c| !released.contains(c));
        }
        OwedReach::new(owed, ctxs)
    }
    /// Whether `chain` can reach one of the frees, as `ChainReach::reaches`
    /// asks of each.
    pub fn reaches(&self, chain: ChainReach) -> bool {
        if chain.res != 0 {
            self.res.binary_search(&chain.res).is_ok()
        } else {
            self.ctxs.binary_search(&chain.ctx).is_ok()
        }
    }
    /// Whether the free `res` holds chains back: a transfer naming it parks.
    pub fn bars(&self, res: u32) -> bool {
        self.res.binary_search(&res).is_ok()
    }
}
/// The longest the owed-free pass stands down after a revival: a synchronous
/// deadline's worth (backend gpu.rs asserts the two equal).
pub const OWED_QUIET_MS: u64 = 500;
/// Whether a stand-down holds: from a stall an owed-free pass command met
/// (`stood_down`, that stall's number; `stalls` is the engine's count) until
/// another stall begins or the engine has run `OWED_QUIET_MS` since that
/// stall revived. The pass stands down after a stall any of its commands met,
/// so that revival's window goes to the requests; a free's barrier stands
/// down only after a stall its own command met (`owed_barrier`). Any other
/// stall leaves the pass and every barrier up, and the pass takes its turn
/// ahead of the requests at that revival, after whatever the revival's pump
/// published (a readback's run, never a chain a barrier holds).
pub fn owed_stands_down(stood_down: Option<u32>, stalls: u32, refusing: bool, since_revival_ms: Option<u64>) -> bool {
    stood_down == Some(stalls) && (refusing || since_revival_ms.is_some_and(|ms| ms < OWED_QUIET_MS))
}
/// The owed frees a new chain may not go to the device past, and apart from
/// them the one standing down because its own command met the stall now
/// counted (`OwedReach::of` lets its holders go). Another free's stall lowers
/// none of the rest, so the pass's turn at one free never lets the chains
/// held back for another go: a free is tried only while no chain on the
/// device can reach it, so its holders' work cannot have caused its stall.
pub fn owed_barrier<'a>(owed: impl Iterator<Item = &'a OwedFree>, stalls: u32, refusing: bool, since_revival_ms: Option<u64>) -> (Vec<u32>, Vec<u32>) {
    let (mut holding, mut standing_down) = (Vec::new(), Vec::new());
    for o in owed {
        if owed_stands_down(o.stood_down, stalls, refusing, since_revival_ms) {
            standing_down.push(o.res);
        } else {
            holding.push(o.res);
        }
    }
    (holding, standing_down)
}
/// One owed-free pass's order. It first tries, once, a free a parked chain
/// waits on, taking the contexts with parked chains in turn. Under a load
/// that stalls every repeat a pass tries one free a window, and a stand-down
/// lets only that free's holders' chains go, so in plain rotation a context
/// would wait for its own free's turn once per debt, which another client
/// can make long; taken in turn, it waits once per context with a parked
/// chain, unless more than `cap` of the frees its chains wait on are waiting
/// in turn for chains on the device. Then the rest in rotation. A turn looks
/// at `cap` frees at most, only frees that hold a parked chain back, and the
/// rotation as many again, each free still queued once, never the turn's
/// pick: so a turn never spends the rotation, and a context whose turn the
/// contexts before it cut short starts the next turn with every look. A free
/// the rotation looks at and finds not clear goes to the back.
pub struct OwedPass {
    /// Each context with a parked chain, and the frees its chains wait on.
    waits: Vec<(u32, Vec<u32>)>,
    turned: bool,
    cap: usize,
    left: usize,
}
impl OwedPass {
    /// `parked`: what each parked chain can reach (a transfer its resource,
    /// a stream all `held_by` its context), of which only the frees that hold
    /// chains back count (`bars`, as `OwedReach::bars`): one a poisoned chain
    /// reaches holds none back, and stays unclear for as long as the poison.
    pub fn new(
        parked: impl Iterator<Item = ChainReach>,
        held_by: impl Fn(u32) -> Vec<u32>,
        bars: impl Fn(u32) -> bool,
        cap: usize,
    ) -> Self {
        let mut waits: Vec<(u32, Vec<u32>, bool)> = Vec::new();
        for p in parked {
            let i = match waits.iter().position(|w| w.0 == p.ctx) {
                Some(i) => i,
                None => {
                    waits.push((p.ctx, Vec::new(), false));
                    waits.len() - 1
                }
            };
            let w = &mut waits[i];
            if p.res != 0 {
                if bars(p.res) {
                    w.1.push(p.res);
                }
            } else if !w.2 {
                w.2 = true;
                w.1.extend(held_by(p.ctx).into_iter().filter(|&r| bars(r)));
            }
        }
        let mut waits: Vec<(u32, Vec<u32>)> = waits
            .into_iter()
            .map(|(ctx, mut res, _)| {
                res.sort_unstable();
                res.dedup();
                (ctx, res)
            })
            .collect();
        waits.sort_unstable_by_key(|w| w.0);
        OwedPass { waits, turned: false, cap, left: 0 }
    }
    /// The next free to try, taken out of `owed`, and whether it was a parked
    /// context's turn; None once the pass has looked at its share. `turn` is
    /// the context the next turn starts after: the one last served, one whose
    /// turn had every look and found no free clear, or the one before a
    /// context a turn was cut short in. `clear(res)`: the free may be tried
    /// now.
    pub fn next(
        &mut self,
        owed: &mut alloc::collections::VecDeque<OwedFree>,
        turn: &mut Option<u32>,
        mut clear: impl FnMut(u32) -> bool,
    ) -> Option<(OwedFree, bool)> {
        if !core::mem::replace(&mut self.turned, true) {
            let picked = self.turn(owed, turn, &mut clear);
            // Set once the pick is out: put back behind the rest, it is not
            // looked at again this pass.
            self.left = owed.len().min(self.cap);
            if let Some(o) = picked {
                return Some((o, true));
            }
        }
        while self.left > 0 {
            self.left -= 1;
            let o = owed.pop_front()?;
            if clear(o.res) {
                return Some((o, false));
            }
            owed.push_back(o);
        }
        None
    }
    /// The contexts with parked chains in turn from the one after `turn`, each
    /// asked for a clear free its chains wait on. Out of looks inside one that
    /// had them all, the next turn starts after it; inside one the contexts
    /// before it cut short, at it.
    fn turn(
        &self,
        owed: &mut alloc::collections::VecDeque<OwedFree>,
        turn: &mut Option<u32>,
        clear: &mut impl FnMut(u32) -> bool,
    ) -> Option<OwedFree> {
        let n = self.waits.len();
        let from = self.waits.iter().position(|w| match *turn {
            Some(t) => w.0 > t,
            None => true,
        });
        let from = from.unwrap_or(0);
        let mut looks = self.cap;
        for k in 0..n {
            let w = &self.waits[(from + k) % n];
            let had = looks;
            let mut i = 0;
            while i < owed.len() && looks > 0 {
                if w.1.binary_search(&owed[i].res).is_ok() {
                    if clear(owed[i].res) {
                        *turn = Some(w.0);
                        return owed.remove(i);
                    }
                    looks -= 1;
                    if looks == 0 {
                        // Cut short (k > 0): the next turn starts at it.
                        *turn = Some(if had == self.cap { w.0 } else { self.waits[(from + k - 1) % n].0 });
                    }
                }
                i += 1;
            }
        }
        None
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
/// The word on the kernel's command line that declares a boot one whose test
/// levers may be armed. The platform writes that line; no client can.
pub const TEST_LEVERS_WORD: &[u8] = b"thylacine.test-latch-dead";
/// The same for the chained seat shots, which are armed by a request of their own.
pub const TEST_CHAINED_WORD: &[u8] = b"thylacine.test-latch-chained";
/// The same for the restore shot, which is armed by a request of its own.
pub const TEST_RESTORE_WORD: &[u8] = b"thylacine.test-latch-restore";
/// Whether a kernel command line holds `word`, whole: ended by white space,
/// a NUL (the FDT property carries one) or the line's end. The empty word is
/// on no line: a split leaves empty pieces, and they declare nothing.
pub fn bootarg_declared(bootargs: &[u8], word: &[u8]) -> bool {
    !word.is_empty() && bootargs.split(|b| b.is_ascii_whitespace() || *b == 0).any(|w| w == word)
}
/// What `InputDrain` answers: the seat's generation and phase, and the events
/// the device gathered since its last drain. One reply from the one thread
/// that gathers them, so the events are of the seat it tells.
pub type Drained = (u64, u32, Vec<(u16, u16, u32)>);
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
/// A free the broker owes the device (GPU-DESIGN 4.5.15): one it could not
/// finish, or one it took while a chain could still reach the resource.
/// `extent` is the hostmem range a mapped HOST3D blob returns, only once both
/// its unmap (`unmapped`) and its free are acknowledged.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct OwedFree {
    pub res: u32,
    pub extent: Option<(u64, u64)>,
    pub unmapped: bool,
    /// Answered refusals so far; a stall or a death is no answer.
    pub answered: u8,
    /// The last stall one of its own commands met (`owed_barrier`).
    pub stood_down: Option<u32>,
}
/// The device command an owed free needs next.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FreeStep {
    /// CTX_DETACH_RESOURCE of a context link the ledger still records: a
    /// retired id names no link, and a refused detach left one.
    Detach,
    Unmap,
    Unref,
}
/// How the device took one command of an owed free.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Answer {
    Done,
    /// Refused on a live engine with no stall across it: nothing changed.
    Refused,
    /// A stall began across it, or the engine died: it may have run or not.
    Unknown,
}
impl Answer {
    pub fn of(acknowledged: bool, stalled: bool, dead: bool) -> Self {
        if acknowledged {
            Self::Done
        } else if stalled || dead {
            Self::Unknown
        } else {
            Self::Refused
        }
    }
}
/// What one command made of an owed free.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Owed {
    /// Acknowledged, and another command is due.
    Next,
    /// Still owed, for a later pass.
    Kept,
    /// Off the device: the id may retire and `extent` be reused.
    Paid(Option<(u64, u64)>),
    /// Refused `OWED_FREE_ANSWERS` times: a leak, its id, pins and extent kept.
    GivenUp,
}
/// Answered refusals an owed free may meet before it is given up. QEMU
/// refuses a free of an existing resource only when the host unmap fails,
/// and that clears the mapping, so the next repeat runs (QEMU v10.0
/// virgl_cmd_resource_unref, virtio_gpu_virgl_unmap_resource_blob).
pub const OWED_FREE_ANSWERS: u8 = 3;
impl OwedFree {
    pub fn new(res: u32, extent: Option<(u64, u64)>, unmapped: bool) -> Self {
        Self { res, extent, unmapped, answered: 0, stood_down: None }
    }
    /// Links first, then the unmap a mapped blob needs, then the free.
    /// `linked`: the ledger still links the resource to a context.
    pub fn next(&self, linked: bool) -> FreeStep {
        if linked {
            FreeStep::Detach
        } else if self.unmapped {
            FreeStep::Unref
        } else {
            FreeStep::Unmap
        }
    }
    pub fn after(&mut self, step: FreeStep, answer: Answer) -> Owed {
        match (answer, step) {
            (Answer::Done, FreeStep::Unref) => Owed::Paid(self.extent),
            (Answer::Done, FreeStep::Unmap) => {
                self.unmapped = true;
                Owed::Next
            }
            (Answer::Done, FreeStep::Detach) => Owed::Next,
            (Answer::Unknown, _) => Owed::Kept,
            (Answer::Refused, _) => {
                self.answered = self.answered.saturating_add(1);
                if self.answered >= OWED_FREE_ANSWERS { Owed::GivenUp } else { Owed::Kept }
            }
        }
    }
}
/// virtio-gpu response types a RESOURCE_UNREF is judged by.
pub const RESP_OK_NODATA: u32 = 0x1100;
pub const RESP_ERR_INVALID_RESOURCE_ID: u32 = 0x1203;
/// Whether a RESOURCE_UNREF's answer leaves the resource off the device: OK,
/// or no such resource, which a repeat of a free that ran gets, and so does
/// a free whose create a stall met and never ran (QEMU v10.0
/// virgl_cmd_resource_unref). Any other answer leaves the resource and the
/// backing it reads in place.
pub fn unref_frees(resp_type: u32) -> bool {
    resp_type == RESP_OK_NODATA || resp_type == RESP_ERR_INVALID_RESOURCE_ID
}
/// What a stalled engine or cursor lane becomes at a completion pump
/// (GPU-DESIGN 4.5.15): a chain that has retired revives it however late. One
/// still outstanding at the abandonment bound, or at the bound of a takeover
/// waiting on it, kills an engine, and abandons a cursor lane until that chain
/// retires.
#[derive(Debug, PartialEq, Eq)]
pub enum Stall { Holding, Revived, Dead }
/// What a failed command of the broker's own met at the device (GPU-DESIGN 4.5.15).
/// Only `Refused` is the device's answer. A stall is other work ahead of the
/// command, and nothing may be concluded from one.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Met { Refused, Stalled, Dead }
/// Asked of a command that failed after an engine that took commands staged it.
/// `stalls` counts the stalls begun since boot, read before the command and
/// after: one begun across it leaves the command, or one behind it, unanswered.
pub fn met(dead: bool, stalls_before: u32, stalls_after: u32) -> Met {
    if dead { Met::Dead } else if stalls_before != stalls_after { Met::Stalled } else { Met::Refused }
}
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
            Self::TestArmRestore => { 74u16.put(out); },
            Self::TestStallShot => { 70u16.put(out); },
            Self::TestOwedDefer { ctx_id, on } => { 72u16.put(out); ctx_id.put(out); on.put(out); },
            Self::TestOwedStall { resource_id } => { 73u16.put(out); resource_id.put(out); },
            Self::FencedHeld => { 55u16.put(out); },
            Self::TestFencedFree => { 56u16.put(out); },
            Self::CompSlotState => { 57u16.put(out); },
            Self::CtxFencesInFlight { ctx_pub } => { 58u16.put(out); ctx_pub.put(out); },
            Self::CtxHasPoisonedSlot { ctx_pub } => { 59u16.put(out); ctx_pub.put(out); },
            Self::FencedInFlight => { 60u16.put(out); },
            Self::EngineDead => { 61u16.put(out); },
            Self::FreesOwed => { 71u16.put(out); },
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
            74 => Self::TestArmRestore,
            70 => Self::TestStallShot,
            72 => Self::TestOwedDefer { ctx_id: <u32>::get(r)?, on: <bool>::get(r)? },
            73 => Self::TestOwedStall { resource_id: <u32>::get(r)? },
            55 => Self::FencedHeld,
            56 => Self::TestFencedFree,
            57 => Self::CompSlotState,
            58 => Self::CtxFencesInFlight { ctx_pub: <u32>::get(r)? },
            59 => Self::CtxHasPoisonedSlot { ctx_pub: <u32>::get(r)? },
            60 => Self::FencedInFlight,
            61 => Self::EngineDead,
            71 => Self::FreesOwed,
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
/// `stalls_begun` is the count as the request began, so a one-request span
/// reads `failure_cause(stalls_begun, stalls, engine)`, which no stall the
/// broker met between two requests can move.
#[derive(Default, Clone, Copy)]
pub struct Stats {
    pub cmd_seq: u64, pub last_scanout_seq: u64, pub last_unref_seq: u64,
    pub condemned_lost: u32,
    /// The device's display events so far, wrapping. A change since the last
    /// reply: the host's preferred rect or EDID may differ; ask again.
    pub display_events: u32,
    pub engine: Engine, pub stalls: u32, pub stalls_begun: u32,
    /// The restores that ended on a blank output, wrapping: the client's frame
    /// given up (`Device::restore`) or none presented. A change since the last
    /// reply: scanout 0 is not what the client took it to be.
    pub restores_blank: u32,
}
record!(Stats { cmd_seq: u64, last_scanout_seq: u64, last_unref_seq: u64, condemned_lost: u32,
    display_events: u32, engine: Engine, stalls: u32, stalls_begun: u32, restores_blank: u32 });
/// Whether what the device scans is unknown after a scanout command: an
/// accepted one is the last word; a refusal with a stall inside its own
/// request (`Stats::stalls_begun` to `stalls`) may still be executed; one
/// with none was answered, or refused before staging, and leaves `before`.
pub fn scanout_unsettled_after(before: bool, accepted: bool, stalls_begun: u32, stalls: u32) -> bool {
    if accepted { false } else if stalls != stalls_begun { true } else { before }
}
/// Whether what the device scans is unknown after a reply: a restore that
/// ended on a blank output since the last one (`Stats::restores_blank` moved)
/// left scanout 0 without the client's frame, whatever the client took it to scan.
pub fn scanout_unsettled_by_restore(before: bool, restores_seen: u32, restores_blank: u32) -> bool {
    before || restores_seen != restores_blank
}
#[cfg(test)]
mod scanout_verdict_tests {
    use super::*;
    #[test]
    fn a_restore_that_ended_blank_unsettles_the_scanout() {
        assert!(!scanout_unsettled_by_restore(false, 3, 3), "no restore ended blank");
        assert!(scanout_unsettled_by_restore(false, 3, 4), "one did");
        assert!(scanout_unsettled_by_restore(true, 3, 3), "what was unknown stays so");
        assert!(scanout_unsettled_by_restore(false, u32::MAX, 0), "the count wraps");
    }
    #[test]
    fn a_scanout_is_unsettled_only_by_a_stall_inside_its_own_request() {
        assert!(!scanout_unsettled_after(true, true, 4, 5), "accepted is the last word");
        assert!(scanout_unsettled_after(false, false, 4, 5), "a stall inside the request");
        assert!(!scanout_unsettled_after(false, false, 5, 5), "a stall before it began staged none of it");
        assert!(scanout_unsettled_after(true, false, 5, 5), "an answer leaves what was not known");
        assert!(scanout_unsettled_after(false, false, u32::MAX, 0), "the count wraps");
    }
}
/// The broker's owed frees: owed now; incurred, paid and given up, monotone;
/// the resources whose retirement has not finished (`retiring`), each owed a
/// free or still linked to a context; the pass's stand-downs, monotone; and,
/// monotone, the frees it tried first as a parked context's turn.
#[derive(Default, Clone, Copy, Debug, PartialEq, Eq)]
pub struct Frees { pub owed: u32, pub incurred: u32, pub paid: u32, pub leaked: u32, pub retiring: u32, pub stood_down: u32, pub parked_first: u32 }
record!(Frees { owed: u32, incurred: u32, paid: u32, leaked: u32, retiring: u32, stood_down: u32, parked_first: u32 });
#[cfg(feature = "backend")]
impl From<&crate::backend::gpu::Gpu> for Stats {
    fn from(g: &crate::backend::gpu::Gpu) -> Self {
        Self { cmd_seq: g.cmd_seq, last_scanout_seq: g.last_scanout_seq,
            last_unref_seq: g.last_unref_seq, condemned_lost: g.condemned_lost,
            display_events: g.display_events(), engine: g.engine(), stalls: g.stalls(), stalls_begun: g.stalls(),
            restores_blank: g.restores_blank }
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
            Request::TestArmLatch, Request::TestStallNext, Request::TestArmChained, Request::TestArmRestore, Request::TestStallShot,
            Request::TestOwedDefer { ctx_id: 1, on: true }, Request::TestOwedStall { resource_id: 1 },
            Request::FreesOwed]
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
    fn a_lever_is_declared_by_a_whole_word_of_the_command_line() {
        let w = TEST_LEVERS_WORD;
        for line in [&b"thylacine.test-latch-dead"[..], b"console=ttyAMA0 thylacine.test-latch-dead thylacine.scale=200\0",
            b"a\tthylacine.test-latch-dead\n", b"thylacine.test-latch-dead\0"] {
            assert!(bootarg_declared(line, w), "{line:?}");
        }
        for line in [&b""[..], b"thylacine.test-latch-deadx", b"xthylacine.test-latch-dead", b"thylacine.test-latch-dead=1",
            b"thylacine.test-latch", b"thylacine.test-latch -dead", b"thylacine.scale=200"] {
            assert!(!bootarg_declared(line, w), "{line:?}");
        }
    }
    #[test]
    fn each_lever_has_its_own_word() {
        // Neither word is a prefix of the other as a whole word, so a boot that
        // declares one arms nothing of the other.
        let both = b"thylacine.test-latch-dead thylacine.test-latch-chained\0";
        assert!(bootarg_declared(both, TEST_LEVERS_WORD) && bootarg_declared(both, TEST_CHAINED_WORD));
        assert!(!bootarg_declared(b"thylacine.test-latch-dead\0", TEST_CHAINED_WORD));
        assert!(!bootarg_declared(b"thylacine.test-latch-chained\0", TEST_LEVERS_WORD));
        for line in [&b""[..], b"\0", b"a  b\0", b"thylacine.test-latch-dead\0"] {
            assert!(!bootarg_declared(line, b""), "the empty word, on {line:?}");
        }
    }
    #[test]
    fn only_the_devices_answer_is_a_refusal() {
        assert_eq!(met(false, 3, 3), Met::Refused);
        // A stall begun across the command leaves it unanswered, at any count.
        assert_eq!(met(false, 3, 4), Met::Stalled);
        assert_eq!(met(false, u32::MAX, 0), Met::Stalled);
        // A dead engine answers nothing, stall or no stall.
        assert_eq!(met(true, 3, 3), Met::Dead);
        assert_eq!(met(true, 3, 4), Met::Dead);
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
        assert!(must_park(&[], A, true, true, false, &N), "its context has a chain on the device");
        assert!(!must_park(&[], A, true, false, false, &N), "nothing ahead of it: it publishes");
        assert!(!must_park(&[], A, false, true, false, &N), "a submit behind a submit is the device's order");
        assert!(!must_park(&[(B, true)], A, true, false, false, &N), "another context's parked readback");
    }
    #[test]
    fn everything_behind_a_parked_chain_of_its_context_parks() {
        assert!(must_park(&[(A, true)], A, false, false, false, &N), "a submit behind the readback");
        assert!(must_park(&[(A, true), (A, false)], A, true, false, false, &N), "a second readback");
        assert!(!must_park(&[(A, true)], B, false, true, false, &N), "another context's submit");
    }
    use Unpark::{Keep as K, Publish as P, Release as R};
    #[test]
    fn a_readback_goes_with_the_chains_behind_it_up_to_the_next_readback() {
        let parked = [(A, true), (A, false), (A, false), (A, true), (A, false)];
        assert_eq!(unpark_plan(&parked, &|_| false, &|_| false, &|_| false, &N), vec![P, P, P, K, K]);
        assert_eq!(unpark_plan(&parked, &|c| c == A, &|_| false, &|_| false, &N), vec![K; 5], "its prefix is on the device");
    }
    #[test]
    fn contexts_unpark_independently_and_keep_their_order() {
        let parked = [(A, true), (B, true), (A, false), (B, false)];
        assert_eq!(unpark_plan(&parked, &|c| c == A, &|_| false, &|_| false, &N), vec![K, P, K, P]);
        assert_eq!(unpark_plan(&parked, &|c| c == B, &|_| false, &|_| false, &N), vec![P, K, P, K]);
    }
    #[test]
    fn a_poisoned_contexts_parked_chains_are_released_and_no_one_elses() {
        let parked = [(A, true), (B, true), (A, false), (B, false)];
        // A's abandoned chain is still "on the device" too: release wins over waiting.
        assert_eq!(unpark_plan(&parked, &|c| c == A || c == B, &|c| c == A, &|_| false, &N), vec![R, K, R, K]);
        assert_eq!(unpark_plan(&parked, &|c| c == A, &|c| c == A, &|_| false, &N), vec![R, P, R, P]);
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
        assert_eq!(unpark_plan(&[(A, true)], &|c| c == A, &|c| c == A, &|_| false, &rel), vec![R]);
        let mut parked = released_tag(A, false);
        (parked.ok, parked.released) = (true, false);
        let tag = release_tag(parked, 5, A_DEV, &mut rel);
        assert!(tag.released && !tag.ok && !tag.abandoned && tag.parked_ms == 5);
        assert!(must_park(&[], A, false, false, false, &rel), "nothing of A parked or on the device: still parks");
        assert!(!must_park(&[], A, false, false, false, &N), "control: without the record it publishes");
        assert!(!must_park(&[], B, false, false, false, &rel), "another context publishes");
        // The pump releases it even once the abandoned chain retired late.
        assert_eq!(unpark_plan(&[(A, false), (B, false)], &|_| false, &|_| false, &|_| false, &rel), vec![R, P]);
        assert_eq!(unpark_plan(&[(A, false), (B, false)], &|_| false, &|_| false, &|_| false, &N), vec![P, P]);
        assert!(must_park(&[], A, true, false, false, &rel), "a readback after the vindication too");
        rel.forget(A_DEV);
        assert!(!must_park(&[], A, false, false, false, &rel), "a new device context is a new stream");
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
    #[test]
    fn a_chain_that_can_reach_an_owed_free_parks_and_holds_its_context_back() {
        assert!(must_park(&[], A, false, false, true, &N), "nothing ahead of it, but it reaches an owed free");
        assert!(!must_park(&[], A, false, false, false, &N), "control: one variable away it publishes");
        let none = |_: usize| false;
        let parked = [(A, false), (A, false), (B, false)];
        assert_eq!(unpark_plan(&parked, &|_| false, &|_| false, &|i| i == 0, &N), vec![K, K, P],
            "A's later chain keeps A's order; B goes");
        assert_eq!(unpark_plan(&parked, &|_| false, &|_| false, &none, &N), vec![P, P, P], "control");
        assert_eq!(unpark_plan(&parked, &|_| false, &|_| false, &|i| i == 1, &N), vec![P, K, P],
            "the chain ahead of it is not held");
        assert_eq!(unpark_plan(&[(A, false), (A, true)], &|_| false, &|_| false, &|i| i == 0, &N), vec![K, K],
            "nor is a readback behind it sent past it");
        // A poisoned or released context's chains are released, never kept.
        assert_eq!(unpark_plan(&parked, &|_| false, &|c| c == A, &|_| true, &N), vec![R, R, K]);
        let mut rel = Released::default();
        rel.note(A_DEV, &released_tag(A, false));
        assert_eq!(unpark_plan(&parked, &|_| false, &|_| false, &|_| true, &rel), vec![R, R, K]);
    }
    #[test]
    fn owed_reach_asks_what_each_owed_free_asks_of_a_chain() {
        // Context 1 links resource 10, context 2 links 11, resource 12 was born in 3.
        let holds = |ctx: u32, res: u32| ctx != 0 && matches!((ctx, res), (1, 10) | (2, 11) | (3, 12));
        let owed = [12, 10];
        let ctxs: Vec<u32> = (0..6).filter(|&c| owed.iter().any(|&r| holds(c, r))).collect();
        let reach = OwedReach::new(vec![12, 10, 10], ctxs);
        assert!(!reach.res.is_empty() && !reach.ctxs.is_empty());
        let mut seen = (0, 0);
        for ctx in 0..6 {
            for res in [0, 10, 11, 12, 13] {
                let chain = ChainReach { ctx, res };
                let want = owed.iter().any(|&r| chain.reaches(Retired::Resource(r), &holds));
                assert_eq!(reach.reaches(chain), want, "{chain:?}");
                if want { seen.0 += 1 } else { seen.1 += 1 }
            }
        }
        assert!(seen.0 > 0 && seen.1 > 0, "both answers were asked: {seen:?}");
        assert!(!OwedReach::default().reaches(ChainReach { ctx: 1, res: 0 }), "nothing owed reaches nothing");
    }
    #[test]
    fn the_pass_stands_down_only_after_its_own_stall_and_only_for_one_window() {
        // A stall the pass did not meet leaves it, and its barrier, up.
        assert!(!owed_stands_down(None, 3, true, None), "another's stall, standing");
        assert!(!owed_stands_down(None, 3, false, Some(0)), "another's stall, just revived");
        // Its own stall: down while it stands and for a deadline after it revives.
        assert!(owed_stands_down(Some(3), 3, true, None));
        assert!(owed_stands_down(Some(3), 3, true, Some(OWED_QUIET_MS)), "standing, whenever the last revival was");
        assert!(owed_stands_down(Some(3), 3, false, Some(0)), "just revived: the requests go first");
        assert!(owed_stands_down(Some(3), 3, false, Some(OWED_QUIET_MS - 1)));
        assert!(!owed_stands_down(Some(3), 3, false, Some(OWED_QUIET_MS)), "the window has run");
        assert!(!owed_stands_down(Some(3), 3, false, None), "no revival to count from: never down for good");
        // The next stall, whoever's, ends it: the requests have had their turn.
        assert!(!owed_stands_down(Some(3), 4, true, None));
        assert!(!owed_stands_down(Some(3), 4, false, Some(0)));
        assert!(!owed_stands_down(Some(u32::MAX), 0, true, None), "the count wraps");
    }
    #[test]
    fn a_free_stands_its_own_barrier_down_and_no_other() {
        let free = |res, stood_down| OwedFree { stood_down, ..OwedFree::new(res, None, true) };
        // 10's own repeat met stall 3, the one now counted; 11's never met
        // one; 12's met stall 2, long revived.
        let owed = [free(10, Some(3)), free(11, None), free(12, Some(2))];
        let (down, all) = ((vec![11, 12], vec![10]), (vec![10, 11, 12], vec![]));
        assert_eq!(owed_barrier(owed.iter(), 3, true, None), down, "while 10's stall stands, only 10 lets chains past");
        assert_eq!(owed_barrier(owed.iter(), 3, false, Some(0)), down, "and for the window after it revives");
        assert_eq!(owed_barrier(owed.iter(), 3, false, Some(OWED_QUIET_MS - 1)), down);
        assert_eq!(owed_barrier(owed.iter(), 3, false, Some(OWED_QUIET_MS)), all, "the window has run: every free holds");
        assert_eq!(owed_barrier(owed.iter(), 4, true, None), all, "the next stall, whoever's, ends it");
        assert_eq!(owed_barrier(owed.iter(), 4, false, Some(0)), all);
        // A stall none of their own commands met lowers no barrier.
        let none = [free(10, None), free(11, None)];
        assert_eq!(owed_barrier(none.iter(), 3, true, None), (vec![10, 11], vec![]));
        assert_eq!(owed_barrier(none.iter(), 3, false, Some(0)), (vec![10, 11], vec![]));
        assert_eq!(owed_barrier(core::iter::empty(), 3, true, None), (vec![], vec![]));
    }
    #[test]
    fn a_free_standing_down_lets_every_context_that_holds_it_go() {
        // Context 1 links 10 and 11, context 2 links 11 only, context 3 links 12.
        let holds = |ctx: u32, res: u32| matches!((ctx, res), (1, 10) | (1, 11) | (2, 11) | (3, 12));
        let held_by = |ctx: u32| (1..20).filter(|&r| holds(ctx, r)).collect::<Vec<u32>>();
        let holders = |set: &dyn Fn(u32) -> bool| (1..6).filter(|&c| (1..20).any(|r| set(r) && holds(c, r))).collect::<Vec<u32>>();
        // 10 stands down; 11 and 12 hold.
        let reach = OwedReach::of(vec![11, 12], &[10], &[], held_by, holders);
        assert!(!reach.reaches(ChainReach { ctx: 1, res: 0 }), "1 holds 11 too, but 10's window is its window");
        assert!(reach.reaches(ChainReach { ctx: 2, res: 0 }), "2 holds only 11: still parked");
        assert!(reach.reaches(ChainReach { ctx: 3, res: 0 }));
        assert!(reach.reaches(ChainReach { ctx: 1, res: 11 }), "a transfer naming a free that holds still parks");
        assert!(!reach.reaches(ChainReach { ctx: 1, res: 10 }), "one naming the free standing down goes");
        // The control, one variable away: nothing standing down, 1 parks.
        let held = OwedReach::of(vec![11, 12], &[], &[], held_by, holders);
        assert!(held.reaches(ChainReach { ctx: 1, res: 0 }));
        assert_eq!(held.ctxs, [1, 2, 3]);
        assert_eq!(reach.ctxs, [2, 3]);
    }
    fn owed_queue(ids: &[u32]) -> alloc::collections::VecDeque<OwedFree> {
        ids.iter().map(|&r| OwedFree::new(r, None, true)).collect()
    }
    #[test]
    fn a_parked_context_gets_its_turn_ahead_of_any_debt() {
        // Context 9 has a stream parked and holds 50; a hundred frees nothing
        // waits on are owed ahead of 50.
        let held_by = |ctx: u32| if ctx == 9 { vec![50, 51] } else { Vec::new() };
        let ids: Vec<u32> = (100..200).chain([50]).collect();
        let mut owed = owed_queue(&ids);
        let mut turn = None;
        let mut pass = OwedPass::new([ChainReach { ctx: 9, res: 0 }].into_iter(), held_by, |_| true, 16);
        let (o, turned) = pass.next(&mut owed, &mut turn, |_| true).unwrap();
        assert_eq!((o.res, turned, turn), (50, true, Some(9)), "the parked context's free first");
        let (o, turned) = pass.next(&mut owed, &mut turn, |_| true).unwrap();
        assert_eq!((o.res, turned), (100, false), "then the rotation");
        // The control, one variable away: nothing parked, the rotation alone.
        let mut owed = owed_queue(&ids);
        let mut pass = OwedPass::new(core::iter::empty(), held_by, |_| true, 16);
        assert_eq!(pass.next(&mut owed, &mut None, |_| true).map(|(o, t)| (o.res, t)), Some((100, false)));
    }
    #[test]
    fn parked_contexts_take_their_turns_in_order() {
        // Streams parked on 7 (twice) and 3; a transfer parked on 5 names 55.
        let held_by = |ctx: u32| match ctx {
            3 => vec![30],
            7 => vec![70, 71],
            _ => Vec::new(),
        };
        let parked = [
            ChainReach { ctx: 7, res: 0 },
            ChainReach { ctx: 3, res: 0 },
            ChainReach { ctx: 5, res: 55 },
            ChainReach { ctx: 7, res: 0 },
        ];
        let mut owed = owed_queue(&[71, 1, 55, 2, 30, 70]);
        let mut turn = None;
        let mut served = Vec::new();
        for _ in 0..4 {
            let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 16);
            let (o, turned) = pass.next(&mut owed, &mut turn, |_| true).unwrap();
            assert!(turned);
            served.push((turn.unwrap(), o.res));
            // Kept, as a repeat a stall met is: to the back.
            owed.push_back(o);
        }
        assert_eq!(served, [(3, 30), (5, 55), (7, 71), (3, 30)], "each context in turn, its first free in the queue's order");
    }
    #[test]
    fn a_turn_passes_over_a_free_not_clear_and_the_rotation_keeps_to_its_share() {
        let held_by = |ctx: u32| if ctx == 9 { vec![50, 51] } else { Vec::new() };
        let parked = [ChainReach { ctx: 9, res: 0 }];
        let mut owed = owed_queue(&[1, 50, 2, 51, 3]);
        let mut turn = None;
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 3);
        // A chain on the device holds 50 and 2; 51 is clear.
        let clear = |r: u32| r != 50 && r != 2;
        let mut next = |owed: &mut alloc::collections::VecDeque<OwedFree>| pass.next(owed, &mut turn, clear).map(|(o, t)| (o.res, t));
        assert_eq!(next(&mut owed), Some((51, true)));
        // The rotation's three looks: 1, then 50 to the back, then 2.
        assert_eq!(next(&mut owed), Some((1, false)));
        assert_eq!(next(&mut owed), None, "the rotation's share is spent");
        assert_eq!(owed.iter().map(|o| o.res).collect::<Vec<_>>(), [3, 50, 2]);
        // No clear free a parked chain waits on: no turn is taken.
        let mut owed = owed_queue(&[50, 1]);
        let mut turn = None;
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 16);
        assert_eq!(pass.next(&mut owed, &mut turn, |r| r != 50).map(|(o, t)| (o.res, t)), Some((1, false)));
        assert_eq!(turn, None);
    }
    #[test]
    fn one_context_spends_neither_the_rotation_nor_the_next_turn() {
        // 3 waits on twenty frees, every one held by a chain still on the
        // device; 7 waits on 70, clear; 1 is clear and nothing waits on it.
        let held_by = |ctx: u32| match ctx {
            3 => (100..120).collect(),
            7 => vec![70],
            _ => Vec::new(),
        };
        let parked = [ChainReach { ctx: 3, res: 0 }, ChainReach { ctx: 7, res: 0 }];
        let ids: Vec<u32> = [1].into_iter().chain(100..120).chain([70]).collect();
        let mut owed = owed_queue(&ids);
        let clear = |r: u32| !(100..120).contains(&r);
        let mut turn = None;
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 16);
        assert_eq!(pass.next(&mut owed, &mut turn, clear).map(|(o, t)| (o.res, t)), Some((1, false)),
            "3's turn ran out of looks; the rotation still has its own");
        assert_eq!(turn, Some(3), "and the next pass starts after 3");
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 16);
        assert_eq!(pass.next(&mut owed, &mut turn, clear).map(|(o, t)| (o.res, t)), Some((70, true)));
        assert_eq!(turn, Some(7));
    }
    #[test]
    fn a_turn_s_pick_is_not_offered_again_in_its_pass() {
        let held_by = |ctx: u32| if ctx == 9 { vec![50] } else { Vec::new() };
        let mut owed = owed_queue(&[50, 1]);
        let mut turn = None;
        let mut pass = OwedPass::new([ChainReach { ctx: 9, res: 0 }].into_iter(), held_by, |_| true, 16);
        let (o, turned) = pass.next(&mut owed, &mut turn, |_| true).unwrap();
        assert_eq!((o.res, turned), (50, true));
        // Kept, as a refused repeat is: back behind the rest.
        owed.push_back(o);
        assert_eq!(pass.next(&mut owed, &mut turn, |_| true).map(|(o, t)| (o.res, t)), Some((1, false)));
        assert_eq!(pass.next(&mut owed, &mut turn, |_| true).map(|(o, t)| (o.res, t)), None, "50 is not refused twice in one pass");
    }
    #[test]
    fn a_context_cut_short_by_the_ones_before_it_starts_the_next_turn() {
        // 3 waits on eleven frees, 7 on seven and 70; only 70 is clear, and a
        // hundred frees nothing waits on are owed ahead, none clear.
        let held_by = |ctx: u32| match ctx {
            3 => (100..=110).collect(),
            7 => (200..=206).chain([70]).collect(),
            _ => Vec::new(),
        };
        let parked = [ChainReach { ctx: 3, res: 0 }, ChainReach { ctx: 7, res: 0 }];
        let ids: Vec<u32> = (1000..=1100).chain(100..=110).chain(200..=206).chain([70]).collect();
        let mut owed = owed_queue(&ids);
        let clear = |r: u32| r == 70;
        let mut turn = None;
        // 3 spends eleven looks, 7 the last five before it reaches 70.
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 16);
        assert_eq!(pass.next(&mut owed, &mut turn, clear).map(|(o, t)| (o.res, t)), None);
        assert_eq!(turn, Some(3), "7 was cut short: the next turn starts at it");
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 16);
        assert_eq!(pass.next(&mut owed, &mut turn, clear).map(|(o, t)| (o.res, t)), Some((70, true)));
        assert_eq!(turn, Some(7));
    }
    #[test]
    fn a_turn_looks_only_at_frees_that_hold_its_chains_back() {
        // 3 holds sixteen frees a poisoned chain reaches, which hold nothing
        // back, ahead of 70, which parks 3's stream and is clear.
        let held_by = |ctx: u32| if ctx == 3 { (100..116).chain([70]).collect() } else { Vec::new() };
        let parked = [ChainReach { ctx: 3, res: 0 }];
        let ids: Vec<u32> = (100..116).chain([70]).collect();
        let clear = |r: u32| r == 70;
        let mut owed = owed_queue(&ids);
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |r| r == 70, 16);
        assert_eq!(pass.next(&mut owed, &mut None, clear).map(|(o, t)| (o.res, t)), Some((70, true)));
        // The control, one variable away: every free counted, the turn and
        // the rotation each spend their sixteen looks on the poisoned ones.
        let mut owed = owed_queue(&ids);
        let mut pass = OwedPass::new(parked.iter().copied(), held_by, |_| true, 16);
        assert_eq!(pass.next(&mut owed, &mut None, clear).map(|(o, t)| (o.res, t)), None);
    }
    #[test]
    fn a_free_a_poisoned_chain_reaches_holds_no_chain_back() {
        // Context 1 links 10 and 11, context 2 links 11, 12 was born in 3, 13 in 4.
        let holds = |ctx: u32, res: u32| matches!((ctx, res), (1, 10) | (1, 11) | (2, 11) | (3, 12) | (4, 13));
        let held_by = |ctx: u32| (1..20).filter(|&r| holds(ctx, r)).collect::<Vec<u32>>();
        let holders = |set: &dyn Fn(u32) -> bool| (1..6).filter(|&c| (1..20).any(|r| set(r) && holds(c, r))).collect::<Vec<u32>>();
        let owed = vec![13, 10, 11, 12];
        // A poisoned stream on 2 reaches 11; a poisoned transfer names 12.
        let poisoned = [ChainReach { ctx: 2, res: 0 }, ChainReach { ctx: 5, res: 12 }];
        let reach = OwedReach::of(owed.clone(), &[], &poisoned, held_by, holders);
        let payable: Vec<u32> = owed.iter().copied()
            .filter(|&r| !poisoned.iter().any(|p| p.reaches(Retired::Resource(r), &holds))).collect();
        assert_eq!(payable, [13, 10], "the control: two frees left, two excluded");
        for ctx in 0..6 {
            for res in [0, 10, 11, 12, 13, 14] {
                let chain = ChainReach { ctx, res };
                let want = payable.iter().any(|&r| chain.reaches(Retired::Resource(r), &holds));
                assert_eq!(reach.reaches(chain), want, "{chain:?}");
            }
        }
        assert_eq!(reach.ctxs, [1, 4], "2 holds only the excluded 11, 3 only the excluded 12");
        let none = OwedReach::of(vec![11], &[], &poisoned, held_by, holders);
        assert_eq!(none, OwedReach::default(), "every free excluded: nothing parks");
        let all = OwedReach::of(owed.clone(), &[], &[], held_by, holders);
        assert_eq!(all.ctxs, [1, 2, 3, 4], "nothing poisoned: every holder");
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
    fn the_restore_lever_has_its_own_opcode_and_word() {
        let mut bytes = Vec::new();
        Request::TestArmRestore.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 74, 0]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::TestArmRestore));
        r.finish().unwrap();
        let all = b"thylacine.test-latch-dead thylacine.test-latch-chained thylacine.test-latch-restore\0";
        assert!(bootarg_declared(all, TEST_RESTORE_WORD));
        for other in [&b"thylacine.test-latch-dead\0"[..], b"thylacine.test-latch-chained\0"] {
            assert!(!bootarg_declared(other, TEST_RESTORE_WORD));
        }
        let alone = b"thylacine.test-latch-restore\0";
        assert!(!bootarg_declared(alone, TEST_LEVERS_WORD) && !bootarg_declared(alone, TEST_CHAINED_WORD));
    }
    #[test]
    fn the_owed_frees_query_has_its_own_opcode() {
        let mut bytes = Vec::new();
        Request::FreesOwed.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 71, 0]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::FreesOwed));
        r.finish().unwrap();
    }
    #[test]
    fn the_frees_census_is_seven_counts_in_order() {
        let mut bytes = Vec::new();
        let f = Frees { owed: 1, incurred: 2, paid: 3, leaked: 4, retiring: 5, stood_down: 6, parked_first: 7 };
        f.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 0, 0, 2, 0, 0, 0, 3, 0, 0, 0, 4, 0, 0, 0, 5, 0, 0, 0, 6, 0, 0, 0, 7, 0, 0, 0]);
        let mut r = Reader::new(&bytes).unwrap();
        assert_eq!(Frees::get(&mut r).unwrap(), f);
        r.finish().unwrap();
        assert!(Frees::get(&mut Reader::new(&bytes[..24]).unwrap()).is_err());
    }
    #[test]
    fn the_owed_defer_lever_has_its_own_opcode() {
        let mut bytes = Vec::new();
        Request::TestOwedDefer { ctx_id: 0x0102_0304, on: true }.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 72, 0, 4, 3, 2, 1, 1]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::TestOwedDefer { ctx_id: 0x0102_0304, on: true }));
        r.finish().unwrap();
    }
    #[test]
    fn the_owed_stall_lever_has_its_own_opcode() {
        let mut bytes = Vec::new();
        Request::TestOwedStall { resource_id: 0x0102_0304 }.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 73, 0, 4, 3, 2, 1]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::TestOwedStall { resource_id: 0x0102_0304 }));
        r.finish().unwrap();
    }
    #[test]
    fn the_stall_shot_has_its_own_opcode() {
        let mut bytes = Vec::new();
        Request::TestStallShot.put(&mut bytes);
        assert_eq!(bytes, [1, 0, 70, 0]);
        let mut r = Reader::new(&bytes).unwrap();
        assert!(matches!(Request::get(&mut r).unwrap(), Request::TestStallShot));
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
        Stats { display_events: 0x0c0b_0a09, engine: Engine::Stalled, stalls: 0x0403_0201,
            stalls_begun: 0x0807_0605, restores_blank: 0x100f_0e0d, ..Stats::default() }.put(&mut bytes);
        assert_eq!(&bytes[bytes.len() - 17..], [9, 10, 11, 12, 1, 1, 2, 3, 4, 5, 6, 7, 8, 13, 14, 15, 16]);
        let s = Stats::get(&mut Reader::new(&bytes).unwrap()).unwrap();
        assert_eq!((s.display_events, s.engine, s.stalls, s.stalls_begun, s.restores_blank),
            (0x0c0b_0a09, Engine::Stalled, 0x0403_0201, 0x0807_0605, 0x100f_0e0d));
        let at = bytes.len() - 13;
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

#[cfg(test)]
mod owed_free_tests {
    use super::*;
    const EXTENT: Option<(u64, u64)> = Some((0x4000, 0x2000));
    #[test]
    fn a_guest_backed_free_is_paid_by_one_unref() {
        let mut o = OwedFree::new(7, None, true);
        assert_eq!(o.next(false), FreeStep::Unref);
        assert_eq!(o.after(FreeStep::Unref, Answer::Done), Owed::Paid(None));
    }
    #[test]
    fn a_mapped_blob_returns_its_extent_only_once_both_are_acknowledged() {
        let mut o = OwedFree::new(7, EXTENT, false);
        assert_eq!(o.next(false), FreeStep::Unmap);
        assert_eq!(o.after(FreeStep::Unmap, Answer::Refused), Owed::Kept);
        assert_eq!(o.next(false), FreeStep::Unmap);
        assert_eq!(o.after(FreeStep::Unmap, Answer::Done), Owed::Next);
        assert_eq!(o.next(false), FreeStep::Unref);
        assert_eq!(o.after(FreeStep::Unref, Answer::Unknown), Owed::Kept);
        // The unmap that ran is not asked again.
        assert_eq!(o.next(false), FreeStep::Unref);
        assert_eq!(o.after(FreeStep::Unref, Answer::Done), Owed::Paid(EXTENT));
    }
    #[test]
    fn a_stall_or_a_death_is_never_an_answer() {
        let mut o = OwedFree::new(7, EXTENT, true);
        for _ in 0..20 {
            assert_eq!(o.after(FreeStep::Unref, Answer::of(false, true, false)), Owed::Kept);
            assert_eq!(o.after(FreeStep::Unref, Answer::of(false, false, true)), Owed::Kept);
        }
        assert_eq!(o.answered, 0);
    }
    #[test]
    fn answers_are_judged_by_acknowledgement_then_stall_then_death() {
        assert_eq!(Answer::of(true, false, false), Answer::Done);
        assert_eq!(Answer::of(false, false, false), Answer::Refused);
        assert_eq!(Answer::of(false, true, false), Answer::Unknown);
        assert_eq!(Answer::of(false, false, true), Answer::Unknown);
        assert_eq!(Answer::of(false, true, true), Answer::Unknown);
    }
    #[test]
    fn answered_refusals_give_a_free_up_without_its_extent() {
        let mut o = OwedFree::new(7, EXTENT, true);
        for _ in 1..OWED_FREE_ANSWERS {
            assert_eq!(o.after(FreeStep::Unref, Answer::Refused), Owed::Kept);
        }
        // Given up carries no extent: it stays reserved, never reused.
        assert_eq!(o.after(FreeStep::Unref, Answer::Refused), Owed::GivenUp);
    }
    #[test]
    fn context_links_go_before_the_unmap_and_the_free() {
        let mut o = OwedFree::new(7, EXTENT, false);
        assert_eq!(o.next(true), FreeStep::Detach);
        assert_eq!(o.after(FreeStep::Detach, Answer::Done), Owed::Next);
        assert!(!o.unmapped);
        assert_eq!(o.next(true), FreeStep::Detach);
        assert_eq!(o.next(false), FreeStep::Unmap);
        let mut g = OwedFree::new(8, None, true);
        assert_eq!(g.next(true), FreeStep::Detach);
        assert_eq!(g.after(FreeStep::Detach, Answer::Unknown), Owed::Kept);
    }
    #[test]
    fn only_ok_and_no_such_resource_read_as_gone() {
        assert!(unref_frees(0x1100));
        assert!(unref_frees(0x1203));
        // UNSPEC is QEMU's answer to a failed host unmap: the resource and
        // the iovs it reads stay, so the pins must too.
        for t in [0x1200, 0x1201, 0x1202, 0x1204, 0x1205, 0x1101, 0x0, 0xffff_ffff] {
            assert!(!unref_frees(t), "resp {:#x}", t);
        }
    }
}
