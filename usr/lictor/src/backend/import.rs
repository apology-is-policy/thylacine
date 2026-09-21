//! Kernel-authenticated normal backing import. Handles, not caller-supplied
//! addresses, keep DMA alive. The broker retains a Pin across ambiguous device
//! completion, even when the normal owner closes its connection or exits.
use libdriver::Error;
use libthyla_rs::{t_close, t_dma_segments, TDmaSeg};
use crate::{gpu_api::BufferRef, skein::{self, Seg}};
pub struct Pin { fd: i64, pub segments: [Seg; 32], pub count: usize }
impl Pin {
    pub fn claim(connection: i64, source: BufferRef) -> Result<Self, Error> {
        if source.share == 0 || source.length == 0 || source.offset.checked_add(source.length).is_none() {
            return Err(Error::BadField);
        }
        let mut fd = connection;
        unsafe { core::arch::asm!("svc #0", inlateout("x0") fd, in("x1") source.share,
            in("x8") libthyla_rs::T_SYS_SEAT_IMPORT, options(nostack)); }
        if fd < 0 { return Err(Error::Hardware); }
        let mut pin = Self { fd, segments: [Seg::default(); 32], count: 0 };
        let mut raw = [TDmaSeg::default(); 32];
        let n = unsafe { t_dma_segments(fd, &mut raw) };
        if n <= 0 || n as usize > raw.len() { return Err(Error::Hardware); }
        let mut whole = [Seg::default(); 32];
        for i in 0..n as usize { whole[i] = Seg { pa: raw[i].pa, len: raw[i].len }; }
        pin.count = skein::subrange(&whole[..n as usize], source.offset, source.length,
            &mut pin.segments).map_err(|_| Error::BadField)?;
        Ok(pin)
    }
    pub fn segments(&self) -> &[Seg] { &self.segments[..self.count] }
}
impl Drop for Pin { fn drop(&mut self) { unsafe { t_close(self.fd); } } }
