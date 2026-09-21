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
struct Backing { resource: u32, _pin: Pin }
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
}
impl Device {
    pub fn new(mut gpu: Gpu, owner: u64) -> Result<Self, Error> {
        gpu.enable_retirement_journal();
        Ok(Self { gpu, objects: Objects::default(), backings: Vec::new(),
            retiring: Vec::new(), rings: Vec::new(), owner, presentation: Presentation::None })
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
    fn attach(&mut self, connection: i64, resource: u32, source: BufferRef) -> Result<(), Error> {
        self.check(resource, Kind::Resource)?;
        if self.backings.iter().any(|b| b.resource == resource) { return Err(Error::BadField); }
        let pin = Pin::claim(connection, source)?;
        let result = self.gpu.attach_backing(resource, pin.segments());
        // Even an error can be an ambiguous device completion. Pin first and
        // retain through a later proven detach/unref, never unwind the DMA.
        self.backings.push(Backing { resource, _pin: pin });
        result
    }
    pub fn reap(&mut self) {
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
            } else { i += 1; }
        }
    }
    /// Used only after kernel END. Ordinary admission remains closed while a
    /// complete previous normal frame is rebound and flushed. The compositor
    /// must also observe the changed seat epoch and repaint after it resumes.
    pub fn restore(&mut self) -> Result<(), Error> {
        if !self.gpu.all_work_retired() { return Err(Error::Hardware); }
        match self.presentation {
            Presentation::None => self.gpu.set_scanout(0, 0, 0),
            Presentation::Image { resource, width, height } => {
                self.check(resource, Kind::Resource)?;
                self.gpu.set_scanout(resource, width, height)?;
                self.gpu.flush(resource, 0, 0, width, height)
            }
            Presentation::Blob { resource, width, height, format, stride } => {
                self.check(resource, Kind::Resource)?;
                self.gpu.set_scanout_blob_then_flush(resource, width, height, format, stride)
            }
        }
    }
    /// The server parks retirement requests until all device work has really
    /// retired. Abandoned/poisoned chains do not qualify as completion.
    pub fn requires_quiescence(request: &Request) -> bool {
        matches!(request, Request::DetachBacking { .. } | Request::ResourceUnref { .. }
            | Request::CtxDestroy { .. } | Request::CtxDetachResource { .. }
            | Request::RetireHost3dRing { .. } | Request::DropHost3dRing { .. })
    }
    pub fn execute(&mut self, connection: i64, request: Request) -> Result<Vec<u8>, Error> {
        if Self::requires_quiescence(&request) && !self.gpu.all_work_retired() { return Err(Error::Hardware); }
        let mut out = Vec::new();
        match request {
            Request::Info => {
                Info::from(&self.gpu).put(&mut out);
            }
            Request::PairProtocolSelftest => {
                let result = self.gpu.pair_protocol_selftest();
                result.put(&mut out);
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
                self.reserve(resource_id, Kind::Resource)?;
                if w == 0 || h == 0 || w > 8192 || h > 8192 { return Err(Error::BadField); }
                let result = self.gpu.resource_create_2d(resource_id, w, h);
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
                self.reserve(resource_id, Kind::Resource)?;
                let pin = Pin::claim(connection, backing)?;
                if pin.count != 1 { return Err(Error::BadField); }
                let result = self.gpu.create_ring_blob(resource_id, pin.segments[0].pa, len);
                self.backings.push(Backing { resource: resource_id, _pin: pin });
                result.put(&mut out);
            }
            Request::CreateHost3dBlob { resource_id, ctx_id, blob_flags, len, blob_id } => {
                self.reserve(resource_id, Kind::Resource)?;
                self.check(ctx_id, Kind::Context)?;
                if len == 0 || len as u64 > 64 * 1024 * 1024 { return Err(Error::BadField); }
                let result = self.gpu.create_host3d_blob(resource_id, ctx_id, blob_flags, len, blob_id);
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
                self.reserve(res_id, Kind::Resource)?;
                if len == 0 || len > 1024 * 1024 { return Err(Error::BadField); }
                let result = self.gpu.mint_host3d_ring(res_id, ctx_id, len, blob_id);
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
                self.reserve(ctx_id, Kind::Context)?;
                if debug_name.len() > 64 { return Err(Error::BadField); }
                let result = self.gpu.ctx_create(ctx_id, &debug_name);
                result.put(&mut out);
            }
            Request::CtxCreateVenus { ctx_id } => {
                self.reserve(ctx_id, Kind::Context)?;
                let result = self.gpu.ctx_create_venus(ctx_id);
                result.put(&mut out);
            }
            Request::CtxCreateCapset { ctx_id, capset_id, debug_name } => {
                self.reserve(ctx_id, Kind::Context)?;
                if debug_name.len() > 64 { return Err(Error::BadField); }
                let result = self.gpu.ctx_create_capset(ctx_id, capset_id, &debug_name);
                result.put(&mut out);
            }
            Request::CtxDestroy { ctx_id } => {
                self.check(ctx_id, Kind::Context)?;
                let ticket = self.objects.retire(self.owner, ctx_id, Kind::Context).map_err(|_| Error::BadField)?;
                let result = self.gpu.ctx_destroy(ctx_id);
                if result.is_ok() {
                    self.objects.context_destroyed(self.owner, ctx_id).map_err(|_| Error::Hardware)?;
                    self.objects.retired(ticket).map_err(|_| Error::Hardware)?;
                }
                result.put(&mut out);
            }
            Request::CtxAttachResource { ctx_id, resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                self.check(ctx_id, Kind::Context)?;
                self.objects.attach(self.owner, ctx_id, resource_id).map_err(|_| Error::BadField)?;
                let result = self.gpu.ctx_attach_resource(ctx_id, resource_id);
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
                let result = self.gpu.resource_create_3d(resource_id, target, format, bind, width, height, depth, array_size, last_level, nr_samples, flags);
                result.put(&mut out);
            }
            Request::SetScanout { resource_id, w, h } => {
                if resource_id != 0 { self.ordinary_resource(resource_id)?; }
                let result = self.gpu.set_scanout(resource_id, w, h);
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
                self.reserve(res_id, Kind::Resource)?;
                self.check(ctx_id, Kind::Context)?;
                if len == 0 || len as u64 > 64 * 1024 * 1024 { return Err(Error::BadField); }
                let result = self.gpu.create_presentable(res_id, ctx_id, len, blob_id);
                result.put(&mut out);
            }
            Request::CtxAttachResourceProbe { ctx_id, resource_id } => {
                self.check(resource_id, Kind::Resource)?;
                self.check(ctx_id, Kind::Context)?;
                let result = self.gpu.ctx_attach_resource_probe(ctx_id, resource_id);
                if matches!(result, Ok(0x1100)) { self.objects.attach(self.owner, ctx_id, resource_id).map_err(|_| Error::BadField)?; }
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
                let result = self.gpu.fenced_in_flight();
                result.put(&mut out);
            }
            Request::EngineDead => {
                let result = self.gpu.engine_dead();
                result.put(&mut out);
            }
            Request::InputInfo { .. } => {
                return Err(Error::BadField);
            }
            Request::InputDrain { .. } => {
                return Err(Error::BadField);
            }
            Request::SeatState => {
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
