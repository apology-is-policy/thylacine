//! Private trusted pixels. Plain DMA is intentionally used here: unlike weave
//! and GPU-BO memory, it cannot be exported by SYS_WEFT_SHARE. No ordinary
//! context ever receives the reserved resource ID or these backing segments.
use alloc::vec::Vec;
use libdriver::Error;
use libthyla_rs::{handle::Rights, hardware::Dma, T_PROT_READ, T_PROT_WRITE};
use crate::{model::Model, render, skein::Seg};
use super::gpu::{Gpu, dsb_sy};
const RESOURCE: u32 = crate::objects::TRUSTED_ID_START + 1;
// The size taken where no other can be made is one the screen is drawn at.
const _: () = assert!(render::fits(super::gpu::DEFAULT_DISPLAY_W, super::gpu::DEFAULT_DISPLAY_H));
const CHUNK: usize = 1024 * 1024;
const BASE: u64 = 0x0400_0000;

/// CPU-only, non-exportable raster workspace, committed before broker admission.
/// A framebuffer is 35 MiB at the maximum supported geometry. Reserving it
/// separately keeps normal resource churn from fragmenting the allocation
/// needed to enter SAK.
struct Raster { base: u64, bytes: u64 }
impl Raster {
    fn new(bytes: usize) -> Result<Self, Error> {
        let base = unsafe { libthyla_rs::t_burrow_attach(bytes as u64) };
        if base <= 0 { return Err(Error::Hardware); }
        Ok(Self { base: base as u64, bytes: bytes as u64 })
    }
    fn pixels(&mut self, count: usize) -> &mut [u32] {
        assert!(count <= self.bytes as usize / 4);
        // Exclusively owned anonymous RW/XN mapping; never exported or aliased.
        unsafe { core::slice::from_raw_parts_mut(self.base as *mut u32, count) }
    }
    fn erase(&mut self) {
        for pixel in self.pixels(self.bytes as usize / 4) {
            unsafe { core::ptr::write_volatile(pixel, 0); }
        }
    }
}
impl Drop for Raster {
    fn drop(&mut self) {
        self.erase();
        unsafe { libthyla_rs::t_burrow_detach(self.base, self.bytes); }
    }
}

pub struct Screen {
    width: u32,
    height: u32,
    /// Device pixels to a layout pixel (`render::fits_at`).
    scale: usize,
    pixels: usize,
    active: bool,
    raster: Raster,
    /// Stall the engine between disabling the outputs and binding the trusted
    /// image: a takeover that fails part-way (the test-latch-dead lever).
    #[cfg(feature = "test-mode")]
    pub test_stall_after_exclusion: bool,
    // Once attached these pins survive every error. The owning service must
    // reset/drop Gpu before dropping Screen, including on an unwinding exit.
    _backing: Vec<Dma>,
}
/// The platform's declared display scale, from the kernel's command line, as
/// the compositor reads it at its own start. None when there is none, when
/// it is not one of the scales there are, or when the line cannot be read.
pub fn declared_scale() -> Option<u16> {
    use libthyla_rs::{t_close, t_open, t_read, T_OREAD, T_WALK_OPEN_FROM_ROOT};
    // SAFETY: a path literal and a buffer of this frame.
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/hw/chosen/bootargs".as_ptr(), 19, T_OREAD) };
    if fd < 0 { return None; }
    let mut buf = [0u8; 1024];
    let n = unsafe { t_read(fd, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(fd); }
    // A line that fills the buffer may have been cut inside the token.
    if n <= 0 || n as usize >= buf.len() { return None; }
    match libhalcyon::scale::declared_scale(&buf[..n as usize]) {
        libhalcyon::scale::Declared::Scale(pct) => Some(pct),
        _ => None,
    }
}
impl Screen {
    /// The screen for the device's display, at the size and the scale
    /// `render::geometry_for` gives it: no side longer than the longest a
    /// display is taken to report, which is also the longest a client's
    /// scanout may be, and the driver's default size where nothing else can
    /// be made. The host's size is whatever its window happens to be, and
    /// this driver is the keyboard and the pointer too: it does not fail for
    /// a size. `declared` is the platform's `thylacine.scale`, if it has one
    /// (`declared_scale`).
    pub fn new(gpu: &mut Gpu, declared: Option<u16>) -> Result<Self, Error> {
        let host = (gpu.width, gpu.height);
        let ((width, height), scale) = render::geometry_for(host, declared, gpu.edid_mm,
            super::gpu::MAX_DISPLAY_DIM, (super::gpu::DEFAULT_DISPLAY_W, super::gpu::DEFAULT_DISPLAY_H));
        if (width, height) != host || scale != 1 {
            crate::backend::diagnostic(&alloc::format!(
                "lictor: trusted screen {}x{} at {}x for a display of {}x{}\n",
                width, height, scale, host.0, host.1));
        }
        let pixels = (width as usize).checked_mul(height as usize).ok_or(Error::Hardware)?;
        if !render::fits_at(width, height, scale) {
            return Err(Error::Hardware);
        }
        let bytes = pixels.checked_mul(4).ok_or(Error::Hardware)?;
        let rounded = (bytes + 4095) & !4095;
        let raster = Raster::new(rounded)?;
        let mut backing = Vec::new();
        let mut segments = Vec::new();
        let mut offset = 0;
        while offset < rounded {
            let size = (rounded - offset).min(CHUNK);
            let dma = unsafe { Dma::new(size, Rights::READ | Rights::WRITE | Rights::MAP,
                BASE + offset as u64, T_PROT_READ | T_PROT_WRITE) }.map_err(|_| Error::Hardware)?;
            segments.push(Seg { pa: dma.paddr(), len: size as u64 });
            backing.push(dma); offset += size;
        }
        unsafe { core::ptr::write_bytes(BASE as *mut u8, 0, rounded); }
        if gpu.resource_create_2d(RESOURCE, width, height).is_err()
            || gpu.attach_backing(RESOURCE, &segments).is_err() {
            // A timeout can mean the device accepted an attach but did not
            // report it. Never reclaim backing on an ambiguous completion.
            core::mem::forget(backing);
            return Err(Error::Hardware);
        }
        Ok(Self { width, height, scale, pixels, active: false, raster, _backing: backing,
            #[cfg(feature = "test-mode")] test_stall_after_exclusion: false })
    }
    pub fn paint(&mut self, gpu: &mut Gpu, model: &Model, masked: usize,
                 backdrop: Option<&[u32]>) -> Result<(), Error> {
        // Rasterize away from scanout. Invalid content leaves the last complete
        // trusted image intact. CPU writes finish before the GPU upload begins.
        let next = self.raster.pixels(self.pixels);
        render::render(next, self.width, self.height, self.scale, backdrop, model, masked)
            .map_err(|_| Error::BadField)?;
        unsafe { core::ptr::copy_nonoverlapping(next.as_ptr(), BASE as *mut u32, self.pixels); }
        dsb_sy();
        gpu.transfer(RESOURCE, 0, 0, 0, self.width, self.height)?;
        if self.active { gpu.flush(RESOURCE, 0, 0, self.width, self.height)?; }
        Ok(())
    }
    pub fn activate(&mut self, gpu: &mut Gpu) -> Result<(), Error> {
        #[cfg(feature = "test-mode")]
        let stall = core::mem::take(&mut self.test_stall_after_exclusion);
        gpu.exclude_all_outputs()?;
        #[cfg(feature = "test-mode")]
        if stall { gpu.test_latch_now(false); }
        gpu.set_scanout(RESOURCE, self.width, self.height)?;
        gpu.flush(RESOURCE, 0, 0, self.width, self.height)?;
        self.active = true;
        Ok(())
    }
    pub fn deactivated(&mut self) {
        self.active = false;
        // The device restored normal output, or its engine died and can show
        // nothing again. Erase the private staging pixels; a later episode
        // must paint a fresh complete semantic frame.
        unsafe {
            for i in 0..self.pixels { core::ptr::write_volatile((BASE as *mut u32).add(i), 0); }
        }
        self.raster.erase();
        dsb_sy();
    }
    pub fn dimensions(&self) -> (u32, u32) { (self.width, self.height) }
}
