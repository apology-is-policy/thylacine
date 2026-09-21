//! Private trusted pixels. Plain DMA is intentionally used here: unlike weave
//! and GPU-BO memory, it cannot be exported by SYS_WEFT_SHARE. No ordinary
//! context ever receives the reserved resource ID or these backing segments.
use alloc::vec::Vec;
use libdriver::Error;
use libthyla_rs::{handle::Rights, hardware::Dma, T_PROT_READ, T_PROT_WRITE};
use crate::{model::Model, render, skein::Seg};
use super::gpu::{Gpu, dsb_sy};
const RESOURCE: u32 = crate::objects::TRUSTED_ID_START + 1;
const CHUNK: usize = 1024 * 1024;
const BASE: u64 = 0x0400_0000;

/// CPU-only, non-exportable raster workspace, committed before broker admission.
/// A framebuffer can exceed the entire general-purpose heap (35 MiB at the
/// maximum supported geometry). Reserving it separately also prevents normal
/// resource churn from fragmenting the allocation needed to enter SAK.
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
    pixels: usize,
    active: bool,
    raster: Raster,
    // Once attached these pins survive every error. The owning service must
    // reset/drop Gpu before dropping Screen, including on an unwinding exit.
    _backing: Vec<Dma>,
}
impl Screen {
    pub fn new(gpu: &mut Gpu) -> Result<Self, Error> {
        let (width, height) = (gpu.width, gpu.height);
        let pixels = (width as usize).checked_mul(height as usize).ok_or(Error::Hardware)?;
        if width < render::MIN_WIDTH || height < render::MIN_HEIGHT || pixels > 4096 * 2160 {
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
        Ok(Self { width, height, pixels, active: false, raster, _backing: backing })
    }
    pub fn paint(&mut self, gpu: &mut Gpu, model: &Model, masked: usize,
                 backdrop: Option<&[u32]>) -> Result<(), Error> {
        // Rasterize away from scanout. Invalid content leaves the last complete
        // trusted image intact. CPU writes finish before the GPU upload begins.
        let next = self.raster.pixels(self.pixels);
        render::render(next, self.width, self.height, backdrop, model, masked)
            .map_err(|_| Error::BadField)?;
        unsafe { core::ptr::copy_nonoverlapping(next.as_ptr(), BASE as *mut u32, self.pixels); }
        dsb_sy();
        gpu.transfer(RESOURCE, 0, 0, 0, self.width, self.height)?;
        if self.active { gpu.flush(RESOURCE, 0, 0, self.width, self.height)?; }
        Ok(())
    }
    pub fn activate(&mut self, gpu: &mut Gpu) -> Result<(), Error> {
        gpu.exclude_all_outputs()?;
        gpu.set_scanout(RESOURCE, self.width, self.height)?;
        gpu.flush(RESOURCE, 0, 0, self.width, self.height)?;
        self.active = true;
        Ok(())
    }
    pub fn deactivated(&mut self) {
        self.active = false;
        // Restoration has completed on the device. Erase the private staging
        // pixels; a later episode must paint a fresh complete semantic frame.
        unsafe {
            for i in 0..self.pixels { core::ptr::write_volatile((BASE as *mut u32).add(i), 0); }
        }
        self.raster.erase();
        dsb_sy();
    }
    pub fn dimensions(&self) -> (u32, u32) { (self.width, self.height) }
}
