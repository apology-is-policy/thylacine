//! Private normal cursor plane (HALCYON-INTERACTION §14). No normal resource
//! identifier or client pixels reach this lane. The GPU reset in `Gpu::drop`
//! precedes this allocation's destructor, including all ambiguous errors.
use super::*;
use libhalcyon::cursor::{self, Shape};

const BASE: u64 = 0x0700_0000; // after the maximum private trusted framebuffer
const RESOURCE: u32 = crate::objects::TRUSTED_ID_START + 2;
const IMAGE_BYTES: usize = 64 * 64 * 4;
const ALLOCATION: usize = IMAGE_BYTES + 4096;
const REPLY: u64 = BASE + IMAGE_BYTES as u64 + 64;
/// Long enough past an abandonment for the kernel to fail the takeover it held up
/// and for the seat to restore, so a pointer update meets the abandoned lane.
#[cfg(feature = "test-mode")]
const TEST_ABANDON_RELEASE_MS: u64 = 10_000;

pub(super) struct Plane {
    notify: u64,
    pub(super) queue: crate::cursor_queue::Queue,
    backing: Option<Dma>,
    ready: bool,
    created: bool,
    attached: bool,
    uploaded: bool,
    image: Option<(Shape, u16)>,
    visible: bool,
    /// The outstanding chain missed its deadline (GPU-DESIGN 4.5.15): the
    /// completion pump retires it late, or abandons the lane at
    /// FENCE_ABANDON_MS until it does.
    stalled: Option<Instant>,
    /// The next cursor command misses its deadline at once, and its
    /// retirement is held until released (the test-latch-dead lever).
    #[cfg(feature = "test-mode")]
    test_stall_next: bool,
    #[cfg(feature = "test-mode")]
    test_hold: bool,
    /// The hold outlasts the abandonment bound and the seat's restore.
    #[cfg(feature = "test-mode")]
    test_abandon: bool,
    /// When the lane was abandoned under that hold; the hold ends
    /// TEST_ABANDON_RELEASE_MS later.
    #[cfg(feature = "test-mode")]
    test_abandon_release: Option<Instant>,
}
impl Plane {
    pub(super) fn new(notify: u64) -> Self {
        Self {
            notify,
            queue: Default::default(),
            backing: None,
            ready: false,
            created: false,
            attached: false,
            uploaded: false,
            image: None,
            visible: false,
            stalled: None,
            #[cfg(feature = "test-mode")]
            test_stall_next: false,
            #[cfg(feature = "test-mode")]
            test_hold: false,
            #[cfg(feature = "test-mode")]
            test_abandon: false,
            #[cfg(feature = "test-mode")]
            test_abandon_release: None,
        }
    }
}
impl Gpu {
    fn cursor_storage(&mut self) -> Result<(), Error> {
        if self.cursor.backing.is_some() {
            return Ok(());
        }
        let dma = unsafe {
            Dma::new(
                ALLOCATION,
                Rights::READ | Rights::WRITE | Rights::MAP,
                BASE,
                T_PROT_READ | T_PROT_WRITE,
            )
        }
        .map_err(|_| Error::Hardware)?;
        unsafe {
            core::ptr::write_bytes(BASE as *mut u8, 0, ALLOCATION);
        }
        self.cursor.backing = Some(dma);
        Ok(())
    }

    /// One request/response pair, disjoint from the controlq scratch. QEMU
    /// returns zero bytes after applying the cursor; other implementations may
    /// return OK_NODATA. No descriptors/backing are recycled on uncertainty.
    fn cursor_command(
        &mut self,
        output: u32,
        resource: u32,
        x: u32,
        y: u32,
        hot: (u32, u32),
        update: bool,
    ) -> Result<(), Error> {
        if self.ctrl.refusing() || !self.cursor.queue.idle() {
            return Err(Error::Hardware);
        }
        self.cursor_storage()?;
        // Catch unsolicited completions before reusing even an idle lane.
        virtio_rmb();
        if unsafe { r16(self.ring_va + CURSOR_USED_OFF + 2) } != self.cursor.queue.retired {
            self.cursor.queue.poison();
            return Err(Error::Hardware);
        }
        let req = BASE + IMAGE_BYTES as u64;
        let pa = self.cursor.backing.as_ref().unwrap().paddr() + IMAGE_BYTES as u64;
        let desc = self.ring_va + CURSOR_DESC_OFF;
        unsafe {
            write_ctrl_hdr(req, if update { 0x0300 } else { 0x0301 });
            w32(req + 24, output);
            w32(req + 28, x);
            w32(req + 32, y);
            w32(req + 36, 0);
            w32(req + 40, resource);
            w32(req + 44, hot.0);
            w32(req + 48, hot.1);
            w32(req + 52, 0);
            core::ptr::write_bytes(REPLY as *mut u8, 0, 24);
            w64(desc, pa);
            w32(desc + 8, 56);
            w16(desc + 12, VIRTQ_DESC_F_NEXT);
            w16(desc + 14, 1);
            w64(desc + 16, pa + 64);
            w32(desc + 24, 24);
            w16(desc + 28, VIRTQ_DESC_F_WRITE);
            w16(desc + 30, 0);
        }
        let slot = self.cursor.queue.begin().map_err(|_| Error::Hardware)?;
        let avail = self.ring_va + CURSOR_AVAIL_OFF;
        unsafe {
            w16(avail, 0);
            w16(avail + 4 + u64::from(slot % CURSORQ_SIZE) * 2, 0);
        }
        dsb_sy();
        unsafe {
            w16(avail + 2, self.cursor.queue.published);
        }
        dsb_sy();
        unsafe {
            w16(self.cursor.notify, GPU_QUEUE_CURSOR);
        }
        let since = Instant::now();
        // Both queues share the device loop, so a client's readback can hold this
        // chain for as long as that client queued ahead of it. A stall ends by
        // itself; waiting here would stop the serve loop for all of it.
        let deadline_ms = if self.ctrl.serving { SUBMIT_DEADLINE_MS } else { FENCE_ABANDON_MS };
        #[cfg(feature = "test-mode")]
        if core::mem::take(&mut self.cursor.test_stall_next) && deadline_ms < FENCE_ABANDON_MS {
            self.cursor.test_hold = true;
            self.cursor.stalled = Some(Instant::now());
            say!("lictor: cursor chain missed its deadline (INJECTED by test-latch-dead, retirement held) -- cursor queue stalled until it retires");
            return Err(Error::Hardware);
        }
        loop {
            if let Some(result) = self.cursor_retire() {
                return result.map_err(|_| Error::Hardware);
            }
            let missed = since.elapsed().as_millis() >= u128::from(deadline_ms);
            if missed && deadline_ms < FENCE_ABANDON_MS {
                self.cursor.stalled = Some(Instant::now());
                say!(
                    "lictor: cursor chain missed its {} ms deadline -- cursor queue stalled until it retires",
                    deadline_ms
                );
                return Err(Error::Hardware);
            }
            if missed {
                self.cursor.queue.abandon();
                say!("lictor: cursor chain never retired in {} ms -- cursor queue abandoned until it retires", deadline_ms);
                return Err(Error::Hardware);
            }
            if self.ctrl.service_completions(1_000_000).is_err() {
                self.cursor.queue.poison();
                say!("lictor: cursor queue failed; backing retained, trusted takeover unavailable");
                return Err(Error::Hardware);
            }
        }
    }

    /// Retire the outstanding chain if its used entry has arrived: None while
    /// it has not; a malformed entry poisons the lane (`Queue::complete`).
    fn cursor_retire(&mut self) -> Option<Result<(), ()>> {
        #[cfg(feature = "test-mode")]
        if self.cursor.test_hold {
            return None;
        }
        let used = self.ring_va + CURSOR_USED_OFF;
        let index = unsafe { r16(used + 2) };
        virtio_rmb();
        if index == self.cursor.queue.retired {
            return None;
        }
        let entry = used + 4 + u64::from(self.cursor.queue.retired % CURSORQ_SIZE) * 8;
        let (head, len, ok) = unsafe {
            (
                r32(entry),
                r32(entry + 4),
                r32(REPLY) == VIRTIO_GPU_RESP_OK_NODATA,
            )
        };
        Some(self.cursor.queue.complete(index, head, len, ok))
    }

    /// The completion pump's half of a cursor stall: a late retirement revives
    /// the lane, one outstanding FENCE_ABANDON_MS abandons it, and a retirement
    /// after that still revives it.
    pub(super) fn cursor_settle(&mut self) {
        if self.cursor.queue.abandoned() {
            #[cfg(feature = "test-mode")]
            if self.cursor.test_abandon_release
                .is_some_and(|at| at.elapsed().as_millis() as u64 >= TEST_ABANDON_RELEASE_MS)
            {
                self.cursor.test_abandon_release = None;
                self.cursor.test_hold = false;
                say!("lictor: cursor chain retirement released (test-latch-dead)");
            }
            match self.cursor_retire() {
                Some(Ok(())) => say!("lictor: abandoned cursor chain retired -- cursor queue revived"),
                Some(Err(())) => say!("lictor: cursor queue failed; backing retained, trusted takeover unavailable"),
                None => {}
            }
            return;
        }
        let Some(t0) = self.cursor.stalled else { return };
        let ms = t0.elapsed().as_millis() as u64;
        match self.cursor_retire() {
            Some(Ok(())) => {
                self.cursor.stalled = None;
                say!("lictor: cursor chain retired {} ms after its deadline -- cursor queue revived", ms);
            }
            Some(Err(())) => {
                self.cursor.stalled = None;
                say!("lictor: cursor queue failed; backing retained, trusted takeover unavailable");
            }
            None if crate::gpu_api::stall_verdict(true, ms) == crate::gpu_api::Stall::Dead => {
                self.cursor.stalled = None;
                self.cursor.queue.abandon();
                say!("lictor: cursor chain never retired in {} ms -- cursor queue abandoned until it retires", ms);
                #[cfg(feature = "test-mode")]
                if core::mem::take(&mut self.cursor.test_abandon) {
                    self.cursor.test_abandon_release = Some(Instant::now());
                }
            }
            None => {}
        }
    }

    fn cursor_prepare(&mut self) -> Result<(), Error> {
        if self.ctrl.refusing() || !self.cursor.queue.idle() {
            return Err(Error::Hardware);
        }
        self.cursor_storage()?;
        if !self.cursor.ready {
            // Store ownership before any hardware submission. On failure the
            // allocation survives; never retry a possibly-created resource ID.
            // A step that stalled is taken as done: the device retires it
            // before the engine revives, and repeating it would collide with
            // the id or the backing it made. A wrong guess fails a later step.
            let pa = self.cursor.backing.as_ref().unwrap().paddr();
            if !self.cursor.created {
                let stalls = self.ctrl.stalls;
                let result = self.resource_create_2d(RESOURCE, 64, 64);
                self.cursor.created = result.is_ok() || self.ctrl.stalls != stalls;
                if result.is_err() {
                    self.cursor_step_failed(stalls);
                    return Err(Error::Hardware);
                }
            }
            if !self.cursor.attached {
                let stalls = self.ctrl.stalls;
                let result = self.attach_backing(RESOURCE, &[Seg { pa, len: IMAGE_BYTES as u64 }]);
                self.cursor.attached = result.is_ok() || self.ctrl.stalls != stalls;
                if result.is_err() {
                    self.cursor_step_failed(stalls);
                    return Err(Error::Hardware);
                }
            }
            self.cursor.ready = true;
        }
        Ok(())
    }

    /// A controlq step on the plane that stalled is not the lane's failure:
    /// the stall holds quiescence until the device retires it (GPU-DESIGN
    /// 4.5.15). Any other failure poisons the lane.
    fn cursor_step_failed(&mut self, stalls: u32) {
        if self.ctrl.stalls == stalls {
            self.cursor.queue.poison();
        }
    }

    pub fn cursor(
        &mut self,
        code: u8,
        scale: u16,
        x: u32,
        y: u32,
        visible: bool,
    ) -> Result<(), Error> {
        let shape = Shape::from_code(code).ok_or(Error::BadField)?;
        let hot = shape.hotspot(scale).ok_or(Error::BadField)?;
        // Bounds match the broker's display extent limit. Do not validate
        // against boot EDID: mode changes update Tapestry's logical geometry
        // before the replacement frame is bound. Hardware clips the interim
        // pointer to the currently bound output; it is never a memory offset.
        if x >= 8192 || y >= 8192 {
            return Err(Error::BadField);
        }
        if !visible {
            return self.cursor_hide_output(0);
        }
        self.cursor_prepare()?;
        let changed = self.cursor.image != Some((shape, scale));
        if changed {
            let pixels = unsafe { core::slice::from_raw_parts_mut(BASE as *mut u32, 64 * 64) };
            pixels.fill(0);
            if !cursor::raster(shape, scale, pixels, 64) {
                return Err(Error::BadField);
            }
            dsb_sy();
            // transfer() verifies the echoed controlq fence before returning.
            let stalls = self.ctrl.stalls;
            if self.transfer(RESOURCE, 0, 0, 0, 64, 64).is_err() {
                // A stalled upload can still land, so nothing says what the host holds.
                self.cursor.uploaded = false;
                self.cursor_step_failed(stalls);
                return Err(Error::Hardware);
            }
            self.cursor.image = Some((shape, scale));
            self.cursor.uploaded = true;
        }
        self.cursor_command(0, RESOURCE, x, y, hot, changed || !self.cursor.visible)?;
        self.cursor.visible = true;
        Ok(())
    }

    /// Arm (or disarm) the lever's cursor stall for the next cursor command.
    #[cfg(feature = "test-mode")]
    pub fn test_cursor_abandon_next(&mut self) {
        self.cursor.test_abandon = true;
    }

    #[cfg(feature = "test-mode")]
    pub fn test_cursor_stall_next(&mut self, on: bool) {
        self.cursor.test_stall_next = on;
    }

    /// Let a held cursor retirement through: the completion pump then
    /// retires the chain late, the way a slow device does.
    #[cfg(feature = "test-mode")]
    pub fn test_cursor_release(&mut self) {
        if self.cursor.test_abandon || self.cursor.test_abandon_release.is_some() {
            return;
        }
        if core::mem::take(&mut self.cursor.test_hold) {
            say!("lictor: cursor chain retirement released (test-latch-dead)");
        }
    }

    /// The lane waits on a chain past its deadline. The chain need not be a device
    /// fault: it shares the host loop a client's readback holds.
    pub(super) fn cursor_stalled(&self) -> bool {
        self.cursor.stalled.is_some()
    }

    pub(super) fn cursor_hide_output(&mut self, output: u32) -> Result<(), Error> {
        self.cursor_prepare()?;
        // Some display listeners (notably QEMU VNC) ignore mouse visibility
        // updates. Replace their cached cursor pixels with transparent pixels
        // BEFORE hiding the plane. A visibility flag alone is insufficient.
        if self.cursor.image.is_some() || !self.cursor.uploaded {
            unsafe {
                core::ptr::write_bytes(BASE as *mut u8, 0, IMAGE_BYTES);
            }
            dsb_sy();
            // A stalled upload can still land, so the host's copy is unknown from here
            // until one is answered.
            self.cursor.image = None;
            self.cursor.uploaded = false;
            let stalls = self.ctrl.stalls;
            if self.transfer(RESOURCE, 0, 0, 0, 64, 64).is_err() {
                self.cursor_step_failed(stalls);
                return Err(Error::Hardware);
            }
            self.cursor.uploaded = true;
        }
        self.cursor_command(output, RESOURCE, 0, 0, (0, 0), true)?;
        self.cursor_command(output, 0, 0, 0, (0, 0), true)?;
        if output == 0 {
            self.cursor.visible = false;
        }
        Ok(())
    }
}
