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

pub(super) struct Plane {
    notify: u64,
    pub(super) queue: crate::cursor_queue::Queue,
    backing: Option<Dma>,
    ready: bool,
    uploaded: bool,
    image: Option<(Shape, u16)>,
    visible: bool,
}
impl Plane {
    pub(super) fn new(notify: u64) -> Self {
        Self {
            notify,
            queue: Default::default(),
            backing: None,
            ready: false,
            uploaded: false,
            image: None,
            visible: false,
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
        if self.ctrl.dead || !self.cursor.queue.idle() {
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
        // Both queues share the device loop. A synchronous GPU readback can
        // hold that loop beyond the ordinary command bound; keep the larger
        // deadline even when the completion pump retires the readback first.
        let mut deadline_ms = if self.ctrl.readback_in_flight() {
            FENCE_ABANDON_MS
        } else {
            SUBMIT_DEADLINE_MS
        };
        loop {
            let used = self.ring_va + CURSOR_USED_OFF;
            let index = unsafe { r16(used + 2) };
            virtio_rmb();
            if index != self.cursor.queue.retired {
                let entry = used + 4 + u64::from(self.cursor.queue.retired % CURSORQ_SIZE) * 8;
                let (head, len, ok) = unsafe {
                    (
                        r32(entry),
                        r32(entry + 4),
                        r32(REPLY) == VIRTIO_GPU_RESP_OK_NODATA,
                    )
                };
                return self
                    .cursor
                    .queue
                    .complete(index, head, len, ok)
                    .map_err(|_| Error::Hardware);
            }
            if self.ctrl.readback_in_flight() {
                deadline_ms = FENCE_ABANDON_MS;
            }
            if since.elapsed().as_millis() >= u128::from(deadline_ms)
                || self.ctrl.service_completions(1_000_000).is_err()
            {
                self.cursor.queue.poison();
                say!("lictor: cursor queue failed; backing retained, trusted takeover unavailable");
                return Err(Error::Hardware);
            }
        }
    }

    fn cursor_prepare(&mut self) -> Result<(), Error> {
        if self.ctrl.dead || !self.cursor.queue.idle() {
            return Err(Error::Hardware);
        }
        self.cursor_storage()?;
        if !self.cursor.ready {
            // Store ownership before any hardware submission. On failure the
            // allocation survives; never retry a possibly-created resource ID.
            let pa = self.cursor.backing.as_ref().unwrap().paddr();
            if self.resource_create_2d(RESOURCE, 64, 64).is_err()
                || self
                    .attach_backing(
                        RESOURCE,
                        &[Seg {
                            pa,
                            len: IMAGE_BYTES as u64,
                        }],
                    )
                    .is_err()
            {
                self.cursor.queue.poison();
                return Err(Error::Hardware);
            }
            self.cursor.ready = true;
        }
        Ok(())
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
            if self.transfer(RESOURCE, 0, 0, 0, 64, 64).is_err() {
                self.cursor.queue.poison();
                return Err(Error::Hardware);
            }
            self.cursor.image = Some((shape, scale));
            self.cursor.uploaded = true;
        }
        self.cursor_command(0, RESOURCE, x, y, hot, changed || !self.cursor.visible)?;
        self.cursor.visible = true;
        Ok(())
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
            if self.transfer(RESOURCE, 0, 0, 0, 64, 64).is_err() {
                self.cursor.queue.poison();
                return Err(Error::Hardware);
            }
            self.cursor.image = None;
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
