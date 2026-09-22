//! Normal compositor GPU facade. Every device operation crosses the bounded
//! broker; this module cannot claim PCI functions, touch BARs or submit queues.
use alloc::vec::Vec;
use core::cell::RefCell;
use libdriver::Error;
use libthyla_rs::{t_open, t_close, t_weft_map, t_burrow_detach, t_weft_share,
    t_weft_unshare, T_WALK_OPEN_FROM_ROOT, T_OREAD};
use crate::{rpc_client::Client, gpu_api::{Info, Stats, Request, BufferRef, RingInfo}, wire::Wire};
pub use crate::gpu_api::{HostRing, FenceTag, FenceVindication, FencedErr};
pub const PAGE_SIZE: u64 = 4096;
pub const RING_DMA_SIZE: usize = 8192;
pub const FENCED_SLOTS: usize = 16;
pub const COMP_FSLOT: usize = FENCED_SLOTS - 1;
pub const FLANE_DMA_SIZE: usize = FENCED_SLOTS * 0x9000 + 4096;
pub const BLOB_FLAG_MAPPABLE: u32 = 1;
pub const BLOB_FLAG_SHAREABLE: u32 = 2;
pub const GPU_RESP_OK_NODATA: u32 = 0x1100;
pub const GPU_RESP_ERR_UNSPEC: u32 = 0x1200;
pub const GPU_RESP_ERR_INVALID_RESOURCE_ID: u32 = 0x1203;
pub const VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM: u32 = 1;
pub const VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM: u32 = 2;
pub fn dsb_sy() { unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)); } }
pub fn prewarm(va: u64, size: usize) {
    for off in (0..size).step_by(4096) { unsafe { core::ptr::write_volatile((va + off as u64) as *mut u8, 0); } }
}
pub struct Gpu {
    client: RefCell<Client>,
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
impl Gpu {
    pub fn probe(_bar: u64, _ring: u64, _flane: u64, _blob_probe: u64) -> Result<Self, Error> {
        let mut client = Client::connect()?;
        let (_, info): (_, Info) = client.call(Request::Info).map_err(|e| { let _ = libthyla_rs::t_putstr(&alloc::format!("lictor client: Info {:?}\n", e)); e })?;
        Ok(Self { client: RefCell::new(client),
            width: info.width,
            height: info.height,
            virgl: info.virgl,
            edid: info.edid,
            edid_mm: info.edid_mm,
            ctxinit: info.ctxinit,
            blob: info.blob,
            cmd_seq: info.cmd_seq,
            last_scanout_seq: info.last_scanout_seq,
            last_unref_seq: info.last_unref_seq,
            condemned_lost: info.condemned_lost,
            num_capsets: info.num_capsets,
            capset_blob: info.capset_blob,
            capset_id: info.capset_id,
            capset_ver: info.capset_ver,
            venus_capset_blob: info.venus_capset_blob,
        })
    }
    fn invoke<R: Wire>(&mut self, request: Request) -> Result<R, Error> {
        let (stats, value) = self.client.borrow_mut().call(request)?;
        self.adopt(stats); Ok(value)
    }
    fn query<R: Wire>(&self, request: Request) -> Result<R, Error> {
        self.client.borrow_mut().call(request).map(|(_, v)| v)
    }
    fn adopt(&mut self, s: Stats) {
        self.cmd_seq = s.cmd_seq; self.last_scanout_seq = s.last_scanout_seq;
        self.last_unref_seq = s.last_unref_seq; self.condemned_lost = s.condemned_lost;
    }
    pub fn sync_stream_max() -> usize { 0x500 - 32 }
    pub fn seat_state(&self) -> Result<(u64, u32), Error> { self.query(Request::SeatState) }
    /// Share a whole kernel object and select its logical subrange. No caller
    /// physical addresses are transmitted, including for scattered backings.
    pub fn attach_backing(&mut self, resource_id: u32, va: u64, size: u64,
                          offset: u64, length: u64) -> Result<(), Error> {
        let share = unsafe { t_weft_share(va, size) };
        if share <= 0 { return Err(Error::Hardware); }
        let result = self.invoke(Request::AttachBacking { resource_id,
            backing: BufferRef { share: share as u64, offset, length } });
        unsafe { t_weft_unshare(share as u64); }
        result?
    }
    pub fn create_ring_blob(&mut self, resource_id: u32, va: u64, len: u32) -> Result<(), Error> {
        let share = unsafe { t_weft_share(va, len as u64) };
        if share <= 0 { return Err(Error::Hardware); }
        let result = self.invoke(Request::CreateRingBlob { resource_id, len,
            backing: BufferRef { share: share as u64, offset: 0, length: len as u64 } });
        unsafe { t_weft_unshare(share as u64); }
        result?
    }
    pub fn mint_host3d_ring(&mut self, res_id: u32, ctx_id: u32, len: u32, blob_id: u64) -> Result<HostRing, Error> {
        let info: Result<RingInfo, Error> = self.invoke(Request::MintHost3dRing { res_id, ctx_id, len, blob_id })?;
        let info = info?;
        let path = alloc::format!("/srv/lictor");
        let root = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, path.as_ptr(), path.len(), T_OREAD) };
        if root < 0 {
            let _: Result<(), _> = self.invoke(Request::RetireHost3dRing { resource_id: res_id });
            return Err(Error::Hardware);
        }
        let path = alloc::format!("rings/{}", info.res_id);
        let fd = unsafe { t_open(root, path.as_ptr(), path.len(), T_OREAD) };
        unsafe { t_close(root); }
        if fd < 0 {
            let _: Result<(), _> = self.invoke(Request::RetireHost3dRing { resource_id: res_id });
            return Err(Error::Hardware);
        }
        let va = unsafe { t_weft_map(fd as u64, 0) };
        unsafe { t_close(fd); }
        if va <= 0 {
            let _: Result<(), _> = self.invoke(Request::RetireHost3dRing { resource_id: res_id });
            return Err(Error::Hardware);
        }
        Ok(HostRing { res_id, offset: 0, va: va as u64, size: info.size, cache: info.cache })
    }
    pub fn retire_host3d_ring(&mut self, ring: HostRing) {
        unsafe { t_burrow_detach(ring.va, ring.size); }
        let _: Result<(), _> = self.invoke(Request::RetireHost3dRing { resource_id: ring.res_id });
    }
    pub fn drop_host3d_ring(&mut self, ring: HostRing) {
        // The service's refcount-based park path also covers a refused local
        // unmap. Never ask it to force-free an allocation another mapping pins.
        self.retire_host3d_ring(ring);
    }
    #[cfg(feature = "test-mode")]
    pub fn pair_protocol_selftest(&mut self) -> () {
        self.invoke(Request::PairProtocolSelftest).unwrap_or(())
    }
    pub fn query_edid(&mut self) -> Result<Option<(u32, u32)>, Error> {
        self.invoke(Request::QueryEdid).unwrap_or(Err(Error::Hardware))
    }
    pub fn query_display_info(&mut self) -> Result<Option<(u32, u32)>, Error> {
        self.invoke(Request::QueryDisplayInfo).unwrap_or(Err(Error::Hardware))
    }
    pub fn resource_create_2d(&mut self, resource_id: u32, w: u32, h: u32) -> Result<(), Error> {
        self.invoke(Request::ResourceCreate2d { resource_id, w, h }).unwrap_or(Err(Error::Hardware))
    }
    pub fn detach_backing(&mut self, resource_id: u32) -> Result<(), Error> {
        self.invoke(Request::DetachBacking { resource_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn resource_unref(&mut self, resource_id: u32) -> Result<(), Error> {
        self.invoke(Request::ResourceUnref { resource_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn condemn(&mut self, res_id: u32) -> () {
        self.invoke(Request::Condemn { res_id }).unwrap_or(())
    }
    pub fn arm_scanout_disable_refusal(&mut self) -> () {
        self.invoke(Request::ArmScanoutDisableRefusal).unwrap_or(())
    }
    pub fn take_injected_refusal(&mut self) -> bool {
        self.invoke(Request::TakeInjectedRefusal).unwrap_or(false)
    }
    pub fn condemned_count(&self) -> usize {
        self.query(Request::CondemnedCount).unwrap_or(usize::MAX)
    }
    pub fn create_host3d_blob(&mut self, resource_id: u32, ctx_id: u32, blob_flags: u32, len: u32, blob_id: u64) -> Result<(), Error> {
        self.invoke(Request::CreateHost3dBlob { resource_id, ctx_id, blob_flags, len, blob_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn hostmem_park_count(&self) -> u64 {
        self.query(Request::HostmemParkCount).unwrap_or(u64::MAX)
    }
    pub fn hostmem_reap_count(&self) -> u64 {
        self.query(Request::HostmemReapCount).unwrap_or(u64::MAX)
    }
    pub fn ctx_create(&mut self, ctx_id: u32, debug_name: &[u8]) -> Result<(), Error> {
        self.invoke(Request::CtxCreate { ctx_id, debug_name: debug_name.to_vec() }).unwrap_or(Err(Error::Hardware))
    }
    pub fn ctx_create_venus(&mut self, ctx_id: u32) -> Result<(), Error> {
        self.invoke(Request::CtxCreateVenus { ctx_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn ctx_create_capset(&mut self, ctx_id: u32, capset_id: u32, debug_name: &[u8]) -> Result<(), Error> {
        self.invoke(Request::CtxCreateCapset { ctx_id, capset_id, debug_name: debug_name.to_vec() }).unwrap_or(Err(Error::Hardware))
    }
    pub fn ctx_destroy(&mut self, ctx_id: u32) -> Result<(), Error> {
        self.invoke(Request::CtxDestroy { ctx_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn ctx_attach_resource(&mut self, ctx_id: u32, resource_id: u32) -> Result<(), Error> {
        self.invoke(Request::CtxAttachResource { ctx_id, resource_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn ctx_detach_resource(&mut self, ctx_id: u32, resource_id: u32) -> Result<(), Error> {
        self.invoke(Request::CtxDetachResource { ctx_id, resource_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn resource_create_3d(&mut self, resource_id: u32, target: u32, format: u32, bind: u32, width: u32, height: u32, depth: u32, array_size: u32, last_level: u32, nr_samples: u32, flags: u32) -> Result<(), Error> {
        self.invoke(Request::ResourceCreate3d { resource_id, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags }).unwrap_or(Err(Error::Hardware))
    }
    pub fn set_scanout(&mut self, resource_id: u32, w: u32, h: u32) -> Result<(), Error> {
        self.invoke(Request::SetScanout { resource_id, w, h }).unwrap_or(Err(Error::Hardware))
    }
    pub fn set_scanout_blob_probe(&mut self, resource_id: u32, w: u32, h: u32, format: u32, stride: u32) -> Result<u32, ()> {
        self.invoke(Request::SetScanoutBlobProbe { resource_id, w, h, format, stride }).unwrap_or(Err(()))
    }
    pub fn set_scanout_blob(&mut self, resource_id: u32, w: u32, h: u32, format: u32, stride: u32) -> Result<(), Error> {
        self.invoke(Request::SetScanoutBlob { resource_id, w, h, format, stride }).unwrap_or(Err(Error::Hardware))
    }
    pub fn set_scanout_blob_then_flush(&mut self, resource_id: u32, w: u32, h: u32, format: u32, stride: u32) -> Result<(), Error> {
        self.invoke(Request::SetScanoutBlobThenFlush { resource_id, w, h, format, stride }).unwrap_or(Err(Error::Hardware))
    }
    pub fn create_presentable(&mut self, res_id: u32, ctx_id: u32, len: u32, blob_id: u64) -> Result<(), Error> {
        self.invoke(Request::CreatePresentable { res_id, ctx_id, len, blob_id }).unwrap_or(Err(Error::Hardware))
    }
    pub fn ctx_attach_resource_probe(&mut self, ctx_id: u32, resource_id: u32) -> Result<u32, ()> {
        self.invoke(Request::CtxAttachResourceProbe { ctx_id, resource_id }).unwrap_or(Err(()))
    }
    pub fn transfer(&mut self, resource_id: u32, offset: u64, x: u32, y: u32, w: u32, h: u32) -> Result<(), Error> {
        self.invoke(Request::Transfer { resource_id, offset, x, y, w, h }).unwrap_or(Err(Error::Hardware))
    }
    pub fn flush(&mut self, resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> Result<(), Error> {
        self.invoke(Request::Flush { resource_id, x, y, w, h }).unwrap_or(Err(Error::Hardware))
    }
    pub fn transfer_then_flush(&mut self, resource_id: u32, offset: u64, x: u32, y: u32, w: u32, h: u32) -> Result<(), Error> {
        self.invoke(Request::TransferThenFlush { resource_id, offset, x, y, w, h }).unwrap_or(Err(Error::Hardware))
    }
    pub fn submit_3d(&mut self, ctx_id: u32, ctx_pub: u32, stream: &[u8], ring_idx: u8) -> Result<u64, FencedErr> {
        self.invoke(Request::Submit3d { ctx_id, ctx_pub, stream: stream.to_vec(), ring_idx }).unwrap_or(Err(FencedErr::Dead))
    }
    pub fn submit_3d_sync(&mut self, ctx_id: u32, stream: &[u8]) -> Result<(), Error> {
        self.invoke(Request::Submit3dSync { ctx_id, stream: stream.to_vec() }).unwrap_or(Err(Error::Hardware))
    }
    pub fn transfer_to_3d_sync(&mut self, ctx_id: u32, res_id: u32, w: u32, h: u32, stride: u32) -> Result<(), Error> {
        self.invoke(Request::TransferTo3dSync { ctx_id, res_id, w, h, stride }).unwrap_or(Err(Error::Hardware))
    }
    pub fn transfer_to_3d_box_sync(&mut self, ctx_id: u32, res_id: u32, x: u32, y: u32, w: u32, h: u32, offset: u64, stride: u32) -> Result<(), Error> {
        self.invoke(Request::TransferTo3dBoxSync { ctx_id, res_id, x, y, w, h, offset, stride }).unwrap_or(Err(Error::Hardware))
    }
    pub fn transfer_3d(&mut self, to_host: bool, ctx_id: u32, ctx_pub: u32, res_id: u32, level: u32, x: u32, y: u32, z: u32, w: u32, h: u32, d: u32, offset: u64, stride: u32, layer_stride: u32) -> Result<u64, FencedErr> {
        self.invoke(Request::Transfer3d { to_host, ctx_id, ctx_pub, res_id, level, x, y, z, w, h, d, offset, stride, layer_stride }).unwrap_or(Err(FencedErr::Dead))
    }
    pub fn transfer_from_3d_comp(&mut self, ctx_id: u32, ctx_pub: u32, res_id: u32, w: u32, h: u32, stride: u32) -> Result<u64, FencedErr> {
        self.invoke(Request::TransferFrom3dComp { ctx_id, ctx_pub, res_id, w, h, stride }).unwrap_or(Err(FencedErr::Dead))
    }
    pub fn transfer_from_3d_sync(&mut self, ctx_id: u32, res_id: u32, w: u32, h: u32, stride: u32) -> Result<(), Error> {
        self.invoke(Request::TransferFrom3dSync { ctx_id, res_id, w, h, stride }).unwrap_or(Err(Error::Hardware))
    }
    pub fn transfer_from_3d_box_sync(&mut self, ctx_id: u32, res_id: u32, x: u32, y: u32, w: u32, h: u32, offset: u64, stride: u32) -> Result<(), Error> {
        self.invoke(Request::TransferFrom3dBoxSync { ctx_id, res_id, x, y, w, h, offset, stride }).unwrap_or(Err(Error::Hardware))
    }
    pub fn poll_completions(&mut self) -> () {
        self.invoke(Request::PollCompletions).unwrap_or(())
    }
    pub fn take_completions(&mut self) -> Vec<FenceTag> {
        self.invoke(Request::TakeCompletions).unwrap_or(Vec::new())
    }
    pub fn take_vindications(&mut self) -> Vec<FenceVindication> {
        self.invoke(Request::TakeVindications).unwrap_or(Vec::new())
    }
    #[cfg(feature = "test-mode")]
    pub fn test_hold_ctx(&mut self, ctx_pub: Option<u32>) -> () {
        self.invoke(Request::TestHoldCtx { ctx_pub }).unwrap_or(())
    }
    #[cfg(feature = "test-mode")]
    pub fn test_hold_ctx_current(&self) -> Option<u32> {
        self.query(Request::TestHoldCtxCurrent).unwrap_or(None)
    }
    #[cfg(feature = "test-mode")]
    pub fn test_abandon_ctx(&mut self, ctx_pub: u32) -> () {
        self.invoke(Request::TestAbandonCtx { ctx_pub }).unwrap_or(())
    }
    #[cfg(feature = "test-mode")]
    pub fn test_hold_ctx_died(&mut self, ctx_pub: u32) -> () {
        self.invoke(Request::TestHoldCtxDied { ctx_pub }).unwrap_or(())
    }
    #[cfg(feature = "test-mode")]
    pub fn test_abandoned_total(&self) -> u32 {
        self.query(Request::TestAbandonedTotal).unwrap_or(u32::MAX)
    }
    #[cfg(feature = "test-mode")]
    pub fn fenced_held(&self) -> Vec<(usize, u64, u32, u8, bool, bool, u64)> {
        self.query(Request::FencedHeld).unwrap_or(Vec::new())
    }
    #[cfg(feature = "test-mode")]
    pub fn test_fenced_free(&self) -> u32 {
        self.query(Request::TestFencedFree).unwrap_or(u32::MAX)
    }
    pub fn comp_slot_state(&self) -> u32 {
        self.query(Request::CompSlotState).unwrap_or(2)
    }
    pub fn ctx_fences_in_flight(&self, ctx_pub: u32) -> u32 {
        self.query(Request::CtxFencesInFlight { ctx_pub }).unwrap_or(u32::MAX)
    }
    pub fn ctx_has_poisoned_slot(&self, ctx_pub: u32) -> bool {
        self.query(Request::CtxHasPoisonedSlot { ctx_pub }).unwrap_or(true)
    }
    pub fn fenced_in_flight(&self) -> u32 {
        self.query(Request::FencedInFlight).unwrap_or(u32::MAX)
    }
    pub fn engine_dead(&self) -> bool {
        self.query(Request::EngineDead).unwrap_or(true)
    }
}
