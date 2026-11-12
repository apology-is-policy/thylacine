//! Private trusted pixels. Plain DMA is intentionally used here: unlike weave
//! and GPU-BO memory, it cannot be exported by SYS_WEFT_SHARE. No ordinary
//! context ever receives a reserved resource ID or these backing segments.
use alloc::vec::Vec;
use libdriver::Error;
use libhalcyon::display::{Follow, Step};
use libthyla_rs::{handle::Rights, hardware::Dma, T_PROT_READ, T_PROT_WRITE};
use crate::{gpu_api::Engine, model::Model, render::{self, Refit}, skein::Seg};
use super::gpu::{Gpu, dsb_sy};
/// The image is in one of these, and a display that changes has the next one
/// made in the other.
const RESOURCES: [u32; 2] = crate::objects::TRUSTED_IMAGES;
// The size taken where no other can be made is one the screen is drawn at.
const _: () = assert!(render::fits(super::gpu::DEFAULT_DISPLAY_W, super::gpu::DEFAULT_DISPLAY_H));
const CHUNK: usize = 1024 * 1024;
const BASE: u64 = 0x0400_0000;
/// The most chunks the largest screen's image is backed by, and the address
/// past the last of them: nothing else of the service's is mapped below it.
const MOST: usize = (render::MAX_PIXELS * 4).div_ceil(CHUNK);
pub(crate) const END: u64 = BASE + (MOST * CHUNK) as u64;

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
    /// The platform's `thylacine.scale`, read once: a command line does not
    /// change.
    declared: Option<u16>,
    /// Which of RESOURCES holds the image.
    image: usize,
    /// The other one may be the device's: a making of it that was not
    /// finished, or a release of it that was not answered for. It is let go
    /// of, and that answered for, before it is made again, and `follow`
    /// asks for that until it is.
    spare_owed: bool,
    /// When the host's display is asked about again.
    follow: Follow,
    /// Stall the engine between disabling the outputs and binding the trusted
    /// image: a takeover that fails part-way (the test-latch-dead lever).
    #[cfg(feature = "test-mode")]
    pub test_stall_after_exclusion: bool,
    /// The backing's chunks in the device's terms, in their order.
    segments: Vec<Seg>,
    // Once attached these pins survive every error. The owning service must
    // reset/drop Gpu before dropping Screen, including on an unwinding exit.
    // They are never given back while the service runs: a display that grows
    // again finds them.
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
fn said(width: u32, height: u32, scale: usize, host: (u32, u32)) {
    crate::backend::diagnostic(&alloc::format!(
        "lictor: trusted screen {}x{} at {}x for a display of {}x{}\n",
        width, height, scale, host.0, host.1));
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
        if (width, height) != host || scale != 1 { said(width, height, scale, host); }
        let pixels = (width as usize).checked_mul(height as usize).ok_or(Error::Hardware)?;
        if !render::fits_at(width, height, scale) {
            return Err(Error::Hardware);
        }
        let bytes = pixels.checked_mul(4).ok_or(Error::Hardware)?;
        let raster = Raster::new((bytes + 4095) & !4095)?;
        let mut screen = Self { width, height, scale, pixels, active: false, raster, declared,
            image: 0, spare_owed: false, follow: Follow::new(), segments: Vec::new(), _backing: Vec::new(),
            #[cfg(feature = "test-mode")] test_stall_after_exclusion: false };
        screen.back(bytes)?;
        if gpu.resource_create_2d(RESOURCES[0], width, height).is_err()
            || gpu.attach_backing(RESOURCES[0], &screen.segments[..bytes.div_ceil(CHUNK)]).is_err() {
            // A timeout can mean the device accepted an attach but did not
            // report it. Never reclaim backing on an ambiguous completion.
            core::mem::forget(core::mem::take(&mut screen._backing));
            return Err(Error::Hardware);
        }
        Ok(screen)
    }
    /// Backing for an image of `bytes`: whole chunks, each mapped after the
    /// last, so that the image is one run of memory whatever its size.
    fn back(&mut self, bytes: usize) -> Result<(), Error> {
        if bytes.div_ceil(CHUNK) > MOST { return Err(Error::Hardware); }
        while self._backing.len() < bytes.div_ceil(CHUNK) {
            let at = BASE + (self._backing.len() * CHUNK) as u64;
            let dma = unsafe { Dma::new(CHUNK, Rights::READ | Rights::WRITE | Rights::MAP,
                at, T_PROT_READ | T_PROT_WRITE) }.map_err(|_| Error::Hardware)?;
            unsafe { core::ptr::write_bytes(at as *mut u8, 0, CHUNK); }
            self.segments.push(Seg { pa: dma.paddr(), len: CHUNK as u64 });
            self._backing.push(dma);
        }
        Ok(())
    }
    /// A pass of the service while the seat is normal. A display that has
    /// changed and stood is asked about, and the trusted screen made the one
    /// for it. Nothing here is needed to enter SAK, and nothing is done while
    /// the screen is up: what cannot be made leaves the screen that stands.
    pub fn follow(&mut self, gpu: &mut Gpu) {
        if self.active { return; }
        let now_ms = libthyla_rs::time::monotonic_ns() / 1_000_000;
        if self.follow.step(gpu.display_events(), now_ms) == Step::Wait { return; }
        match gpu.engine() {
            Engine::Dead => return,
            // The driver ends a stall itself; nothing is asked meanwhile.
            Engine::Stalled => { self.follow.hold(now_ms); return; }
            Engine::Live => {}
        }
        // A stall a client armed is for that client's next command.
        #[cfg(feature = "test-mode")]
        let shot = gpu.test_stall_set_aside();
        let made = self.refit(gpu);
        // The image before this one, where its making could not let go of
        // it: held, it is the device's memory until the display next changes.
        let kept = made.is_ok() && self.spare_owed && !self.release_spare(gpu);
        #[cfg(feature = "test-mode")]
        gpu.test_stall_put_back(shot);
        if made.is_ok() && !kept { return; }
        let again = !gpu.engine_dead()
            && self.follow.retry(libthyla_rs::time::monotonic_ns() / 1_000_000);
        let tail = if again { ", to be tried again" } else { "" };
        crate::backend::diagnostic(&match made {
            Err(why) => alloc::format!("lictor: trusted screen stays {}x{} at {}x: {}{}\n",
                self.width, self.height, self.scale, why, tail),
            Ok(()) => alloc::format!("lictor: trusted screen {}x{} at {}x: the image before it was not let go of{}\n",
                self.width, self.height, self.scale, tail),
        });
    }
    /// The reserved resource that is not the image, let go of. A refusal is
    /// an answer: the device has no such resource, which is what is wanted.
    /// Not answered, nothing is known of it, and it stays owed.
    fn release_spare(&mut self, gpu: &mut Gpu) -> bool {
        let spare = RESOURCES[1 - self.image];
        let stalls = gpu.stalls();
        let _ = gpu.detach_backing(spare);
        let _ = gpu.unref_own(spare);
        let answered = gpu.stalls() == stalls && matches!(gpu.engine(), Engine::Live);
        if answered { self.spare_owed = false; }
        answered
    }
    fn refit(&mut self, gpu: &mut Gpu) -> Result<(), &'static str> {
        // A host that shows no display has nothing to be followed to.
        let Some(host) = gpu.query_display_info().map_err(|_| "its display was not told")? else {
            return Ok(());
        };
        let mm = gpu.query_edid().map_err(|_| "its display's EDID was not answered")?;
        match render::refit(((self.width, self.height), self.scale), host, self.declared, mm,
            super::gpu::MAX_DISPLAY_DIM, (super::gpu::DEFAULT_DISPLAY_W, super::gpu::DEFAULT_DISPLAY_H)) {
            Refit::Stands => return Ok(()),
            Refit::Scale(scale) => self.scale = scale,
            Refit::Size(size, scale) => self.remake(gpu, size, scale)?,
        }
        said(self.width, self.height, self.scale, host);
        Ok(())
    }
    /// The image at another size. The one that stands is the image until the
    /// device has answered for the new one whole, so a making that fails
    /// anywhere leaves a screen SAK can enter.
    fn remake(&mut self, gpu: &mut Gpu, (width, height): (u32, u32), scale: usize) -> Result<(), &'static str> {
        if !render::fits_at(width, height, scale) { return Err("the size for its display is not one it is drawn at"); }
        let pixels = width as usize * height as usize;
        let bytes = pixels * 4;
        if bytes as u64 > self.raster.bytes {
            self.raster = Raster::new((bytes + 4095) & !4095).map_err(|_| "no memory for its workspace")?;
        }
        self.back(bytes).map_err(|_| "no memory for its pixels")?;
        let spare = RESOURCES[1 - self.image];
        if self.spare_owed && !self.release_spare(gpu) {
            return Err("the device did not answer for the image before the last");
        }
        // From its first command the spare may be the device's, whatever is
        // answered.
        self.spare_owed = true;
        if gpu.resource_create_2d(spare, width, height).is_err()
            || gpu.attach_backing(spare, &self.segments[..bytes.div_ceil(CHUNK)]).is_err() {
            return Err("the device did not make its image");
        }
        self.image = 1 - self.image;
        (self.width, self.height, self.scale, self.pixels) = (width, height, scale, pixels);
        // The image that was is the spare from here. Not answered for, it
        // stays owed, and `follow` asks again.
        let _ = self.release_spare(gpu);
        Ok(())
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
        let image = RESOURCES[self.image];
        gpu.transfer(image, 0, 0, 0, self.width, self.height)?;
        if self.active { gpu.flush(image, 0, 0, self.width, self.height)?; }
        Ok(())
    }
    pub fn activate(&mut self, gpu: &mut Gpu) -> Result<(), Error> {
        #[cfg(feature = "test-mode")]
        let stall = core::mem::take(&mut self.test_stall_after_exclusion);
        gpu.exclude_all_outputs()?;
        #[cfg(feature = "test-mode")]
        if stall { gpu.test_latch_now(false); }
        let image = RESOURCES[self.image];
        gpu.set_scanout(image, self.width, self.height)?;
        gpu.flush(image, 0, 0, self.width, self.height)?;
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
