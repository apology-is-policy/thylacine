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
    /// The venus TIMELINE this fence rides (multi-queue F3): 0 = the
    /// ctx-global lane (every pre-multi-queue submission, transfers, the
    /// compositor readback); 1..=3 = a VkQueue's timeline (the submit
    /// carried INFO_RING_IDX). The seam's per-timeline `timeline_signaled`
    /// retires by this value -- server-minted, never client bytes, so it is
    /// always < WARP_TIMELINES by construction.
    pub ring_idx: u8,
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
    abandoned: bool, ok: bool, ring_idx: u8 });
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
            _ => return Err(Malformed),
        })
    }
}

#[derive(Default)]
pub struct Stats {
    pub cmd_seq: u64, pub last_scanout_seq: u64, pub last_unref_seq: u64,
    pub condemned_lost: u32,
}
record!(Stats { cmd_seq: u64, last_scanout_seq: u64, last_unref_seq: u64, condemned_lost: u32 });
#[cfg(feature = "backend")]
impl From<&crate::backend::gpu::Gpu> for Stats {
    fn from(g: &crate::backend::gpu::Gpu) -> Self {
        Self { cmd_seq: g.cmd_seq, last_scanout_seq: g.last_scanout_seq,
            last_unref_seq: g.last_unref_seq, condemned_lost: g.condemned_lost }
    }
}
/// A host allocation's description. The mapping arrives through authenticated
/// 9P Rweft on its ring fid, never as a device-private address in a ctl reply.
pub struct RingInfo { pub res_id: u32, pub size: u64, pub cache: u64 }
record!(RingInfo { res_id: u32, size: u64, cache: u64 });
