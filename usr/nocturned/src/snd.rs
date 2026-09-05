// The virtio-snd transport (VIRTIO 1.2 section 5.14, device id 25) over the
// modern-PCI capability regions: one playback stream, S16LE stereo at the graph
// rate, driven by TX-message completions. NOCTURNE.md section 5.10 records why
// the completion IRQ is the clock: QEMU's device (hw/audio/virtio-snd.c) returns
// a TX status only once the backend has consumed the whole buffer and has no
// eventq -- there is no period-elapsed event to wait on, so the driver keeps
// `PERIODS` messages in flight and treats every completion as one period.
//
// Device responses are UNTRUSTED input (the I-14 posture): every status word,
// latency figure, used-ring id and length is bounds-checked before it steers a
// slot or a counter. A bogus used id is dropped, never re-posted.
//
// The register constants mirror usr/lib/netdev/src/virtio_pci.rs (private
// there); hoisting a shared virtio-pci-modern module is a recorded seam.

use core::time::Duration;

use libdriver::driver::{alloc_dma, DriverVa};
use libdriver::resource::BoundResources;
use libdriver::Error;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, Irq, PciDev, PciRegion,
};
use libthyla_rs::virtio_rmb;


pub const VIRTIO_DEVICE_ID_SND: u32 = 25;

/// The BAR window: a free user-VA region of 6 x PCI_BAR_VA_STRIDE, clear of
/// libdriver's DriverVa bump region (0x0100_0000 + the DMA pool below).
const BAR_WINDOW_VA: u64 = 0x0200_0000;

const STATUS_ACKNOWLEDGE: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FEATURES_OK: u8 = 8;
const STATUS_FAILED: u8 = 128;
const VIRTIO_F_VERSION_1_BIT_HI: u32 = 1 << 0;
const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;

const CCFG_DEVICE_FEATURE_SELECT: u64 = 0x00;
const CCFG_DEVICE_FEATURE: u64 = 0x04;
const CCFG_DRIVER_FEATURE_SELECT: u64 = 0x08;
const CCFG_DRIVER_FEATURE: u64 = 0x0C;
const CCFG_CONFIG_MSIX_VECTOR: u64 = 0x10;
const CCFG_DEVICE_STATUS: u64 = 0x14;
const CCFG_QUEUE_SELECT: u64 = 0x16;
const CCFG_QUEUE_SIZE: u64 = 0x18;
const CCFG_QUEUE_MSIX_VECTOR: u64 = 0x1A;
const CCFG_QUEUE_ENABLE: u64 = 0x1C;
const CCFG_QUEUE_NOTIFY_OFF: u64 = 0x1E;
const CCFG_QUEUE_DESC: u64 = 0x20;
const CCFG_QUEUE_DRIVER: u64 = 0x28;
const CCFG_QUEUE_DEVICE: u64 = 0x30;
const CCFG_MIN_LEN: u32 = 0x38;

/// virtio_snd_config: jacks(4) streams(4) chmaps(4) [controls(4)].
const SND_CFG_MIN_LEN: u32 = 12;

const VIRTQ_DESC_F_NEXT: u16 = 1;
const VIRTQ_DESC_F_WRITE: u16 = 2;

const VQ_CONTROL: u16 = 0;
const VQ_TX: u16 = 2;

const R_PCM_INFO: u32 = 0x0100;
const R_PCM_SET_PARAMS: u32 = 0x0101;
const R_PCM_PREPARE: u32 = 0x0102;
const R_PCM_RELEASE: u32 = 0x0103;
const R_PCM_START: u32 = 0x0104;
const R_PCM_STOP: u32 = 0x0105;
const S_OK: u32 = 0x8000;

const D_OUTPUT: u8 = 0;
const FMT_S16: u8 = 5;
const RATE_48000: u8 = 7;

/// The stream geometry: S16LE stereo at 48 kHz, 512-frame periods, four in
/// flight -- byte-identical to QEMU's device defaults (period 2048 / buffer
/// 8192), so the first cut asks the device for exactly what it already assumes.
pub const RATE_HZ: u32 = 48_000;
pub const CHANNELS: u32 = 2;
pub const FRAME_BYTES: usize = 4;
pub const PERIOD_BYTES: usize = 2048;
pub const PERIODS: usize = 4;
pub const BUFFER_BYTES: usize = PERIOD_BYTES * PERIODS;

/// Queue geometry: 64 entries (QEMU's size), but only PERIODS x 3 descriptors
/// of the TX queue and 2 of the control queue are ever used.
const QUEUE_SIZE: u16 = 64;
const QUEUE_SIZE_USZ: usize = QUEUE_SIZE as usize;

const PAGE: usize = 4096;
// DMA pool layout (page offsets). Each queue: desc page, avail page, used page.
const CTRLQ_DESC_OFF: usize = 0 * PAGE;
const CTRLQ_AVAIL_OFF: usize = 1 * PAGE;
const CTRLQ_USED_OFF: usize = 2 * PAGE;
const TXQ_DESC_OFF: usize = 3 * PAGE;
const TXQ_AVAIL_OFF: usize = 4 * PAGE;
const TXQ_USED_OFF: usize = 5 * PAGE;
const CTRL_REQ_OFF: usize = 6 * PAGE; // request at +0, response at +2048
const CTRL_RESP_OFF: usize = CTRL_REQ_OFF + 2048;
const TX_META_OFF: usize = 7 * PAGE; // per slot 64 B: xfer hdr at +0, status at +32
const TX_PAYLOAD_OFF: usize = 8 * PAGE; // per slot PERIOD_BYTES
const DMA_POOL_SIZE: usize = TX_PAYLOAD_OFF + PERIODS * PERIOD_BYTES;

const CTRL_WAIT_STEPS: u32 = 2000; // x 1 ms = a 2 s bound on a control round-trip

#[inline(always)]
unsafe fn r8(a: u64) -> u8 {
    mmio_read8(a)
}
#[inline(always)]
unsafe fn r16(a: u64) -> u16 {
    mmio_read16(a)
}
#[inline(always)]
unsafe fn r32(a: u64) -> u32 {
    mmio_read32(a)
}
#[inline(always)]
unsafe fn w8(a: u64, v: u8) {
    mmio_write8(a, v)
}
#[inline(always)]
unsafe fn w16(a: u64, v: u16) {
    mmio_write16(a, v)
}
#[inline(always)]
unsafe fn w32(a: u64, v: u32) {
    mmio_write32(a, v)
}
#[inline(always)]
unsafe fn w64(a: u64, v: u64) {
    mmio_write64(a, v)
}
#[inline(always)]
fn dsb_sy() {
    unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) }
}

/// Playback counters, all device-derived figures bounded at the parse site.
#[derive(Default, Clone, Copy)]
pub struct Stats {
    pub periods_played: u64,
    pub silence_periods: u64,
    pub tx_errors: u64,
    pub bad_used: u64,
    pub last_latency_bytes: u32,
}

pub struct VirtioSnd {
    _pci: PciDev,
    irq: Irq,
    pool: Dma,
    common_va: u64,
    isr_va: u64,
    ctrl_notify_va: u64,
    tx_notify_va: u64,
    /// The TX avail index we last published (the driver's copy; the ring's is a mirror).
    tx_avail_idx: u16,
    /// The TX used index we last consumed.
    tx_used_idx: u16,
    ctrl_avail_idx: u16,
    ctrl_used_idx: u16,
    /// Slot s is in flight iff bit s is set.
    tx_inflight: u32,
    pub stats: Stats,
    started: bool,
}

impl VirtioSnd {
    /// Claim the function, run the modern-PCI handshake, set up the control +
    /// TX queues, and negotiate the playback stream (PCM_INFO -> SET_PARAMS ->
    /// PREPARE). `START` is deferred to `start()` so the stream begins with real
    /// data queued.
    pub fn open(res: &BoundResources, va: &mut DriverVa) -> Result<Self, Error> {
        let pci = unsafe { PciDev::claim(VIRTIO_DEVICE_ID_SND, BAR_WINDOW_VA) }.map_err(|e| {
            say!("nocturned: virtio-snd claim failed: {:?}", e);
            Error::Hardware
        })?;
        let (common_va, common_len) = pci.region(PciRegion::Common).ok_or(Error::Hardware)?;
        if common_len < CCFG_MIN_LEN {
            say!("nocturned: common-cfg region too small ({})", common_len);
            return Err(Error::Hardware);
        }
        let (notify_base, notify_len) = pci.region(PciRegion::Notify).ok_or(Error::Hardware)?;
        let isr_va = pci.region(PciRegion::Isr).ok_or(Error::Hardware)?.0;
        let (dev_va, dev_len) = pci.region(PciRegion::Device).ok_or(Error::Hardware)?;
        if dev_len < SND_CFG_MIN_LEN {
            say!("nocturned: device-cfg region too small ({})", dev_len);
            return Err(Error::Hardware);
        }
        let notify_mul = u64::from(pci.notify_off_multiplier());
        let intid = pci.intid().ok_or_else(|| {
            say!("nocturned: no INTx line for the sound function");
            Error::Hardware
        })?;
        // The allowance the warden conferred names this INTID (I-34); the kernel
        // gate rejects any other.
        let irq = Irq::new(intid, Rights::SIGNAL).map_err(|_| {
            say!("nocturned: IRQ {} claim failed (line shared with another driver?)", intid);
            Error::Hardware
        })?;
        let pool = alloc_dma(res, DMA_POOL_SIZE, va)?;
        let pool_va = pool.base_va() as u64;
        let pool_pa = pool.paddr();
        // Touch every page once so the kernel backs them before the device DMAs.
        let mut off = 0usize;
        while off < DMA_POOL_SIZE {
            unsafe { w8(pool_va + off as u64, 0) };
            off += PAGE;
        }

        unsafe {
            w8(common_va + CCFG_DEVICE_STATUS, 0);
            w8(common_va + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
            w8(common_va + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
            w16(common_va + CCFG_CONFIG_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

            w32(common_va + CCFG_DEVICE_FEATURE_SELECT, 0);
            let feat_lo = r32(common_va + CCFG_DEVICE_FEATURE);
            w32(common_va + CCFG_DEVICE_FEATURE_SELECT, 1);
            let feat_hi = r32(common_va + CCFG_DEVICE_FEATURE);
            if feat_hi & VIRTIO_F_VERSION_1_BIT_HI == 0 {
                say!("nocturned: device lacks VIRTIO_F_VERSION_1");
                w8(common_va + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
            // Accept nothing device-specific (VIRTIO_SND_F_CTLS is the 1.3 control
            // element family; unused here). VERSION_1 only.
            w32(common_va + CCFG_DRIVER_FEATURE_SELECT, 0);
            w32(common_va + CCFG_DRIVER_FEATURE, 0);
            w32(common_va + CCFG_DRIVER_FEATURE_SELECT, 1);
            w32(common_va + CCFG_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT_HI);
            w8(
                common_va + CCFG_DEVICE_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK,
            );
            if r8(common_va + CCFG_DEVICE_STATUS) & STATUS_FEATURES_OK == 0 {
                say!("nocturned: FEATURES_OK rejected (lo=0x{:08x} hi=0x{:08x})", feat_lo, feat_hi);
                w8(common_va + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
            let jacks = r32(dev_va);
            let streams = r32(dev_va + 4);
            let chmaps = r32(dev_va + 8);
            say!(
                "nocturned: virtio-snd features lo=0x{:08x} hi=0x{:08x} jacks={} streams={} chmaps={} intid={}",
                feat_lo, feat_hi, jacks, streams, chmaps, intid
            );
            if streams == 0 {
                say!("nocturned: device advertises no PCM streams");
                w8(common_va + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        }

        let ctrl_off = setup_queue(
            common_va,
            VQ_CONTROL,
            pool_pa + CTRLQ_DESC_OFF as u64,
            pool_pa + CTRLQ_AVAIL_OFF as u64,
            pool_pa + CTRLQ_USED_OFF as u64,
        )
        .ok_or_else(|| {
            say!("nocturned: controlq setup failed");
            Error::Hardware
        })?;
        let tx_off = setup_queue(
            common_va,
            VQ_TX,
            pool_pa + TXQ_DESC_OFF as u64,
            pool_pa + TXQ_AVAIL_OFF as u64,
            pool_pa + TXQ_USED_OFF as u64,
        )
        .ok_or_else(|| {
            say!("nocturned: txq setup failed");
            Error::Hardware
        })?;
        // Doorbells: notify_base + queue_notify_off * multiplier, each inside the
        // notify region (a malformed offset is refused, never dereferenced).
        let door = |off: u16| -> Result<u64, Error> {
            let o = u64::from(off) * notify_mul;
            if o + 2 > u64::from(notify_len) {
                say!("nocturned: notify offset {} outside the notify region", off);
                return Err(Error::Hardware);
            }
            Ok(notify_base + o)
        };
        let ctrl_notify_va = door(ctrl_off)?;
        let tx_notify_va = door(tx_off)?;

        unsafe {
            w8(
                common_va + CCFG_DEVICE_STATUS,
                STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK,
            );
        }

        let mut snd = VirtioSnd {
            _pci: pci,
            irq,
            pool,
            common_va,
            isr_va,
            ctrl_notify_va,
            tx_notify_va,
            tx_avail_idx: 0,
            tx_used_idx: 0,
            ctrl_avail_idx: 0,
            ctrl_used_idx: 0,
            tx_inflight: 0,
            stats: Stats::default(),
            started: false,
        };
        snd.negotiate_stream()?;
        Ok(snd)
    }

    /// The GIC INTID this driver waits on (the poll-set fd is `irq_fd`).
    pub fn irq_fd(&self) -> i32 {
        use libthyla_rs::poll::AsFd;
        self.irq.as_raw_fd()
    }

    fn pool_va(&self) -> u64 {
        self.pool.base_va() as u64
    }

    fn pool_pa(&self) -> u64 {
        self.pool.paddr()
    }

    // ---- control queue ------------------------------------------------------

    /// One control round-trip: `req` bytes in, the response into the response
    /// page; returns the response bytes (bounded by `resp_max`) once the device
    /// has used the chain, or Err after CTRL_WAIT_STEPS ms.
    fn ctrl_rpc(&mut self, req: &[u8], resp_max: usize) -> Result<usize, Error> {
        if req.is_empty() || req.len() > 2048 || resp_max == 0 || resp_max > 2048 {
            return Err(Error::Hardware);
        }
        let pv = self.pool_va();
        let pp = self.pool_pa();
        unsafe {
            for (i, &b) in req.iter().enumerate() {
                w8(pv + CTRL_REQ_OFF as u64 + i as u64, b);
            }
            for i in 0..resp_max as u64 {
                w8(pv + CTRL_RESP_OFF as u64 + i, 0);
            }
            // desc 0: request (RO, NEXT -> 1); desc 1: response (WRITE).
            let d0 = pv + CTRLQ_DESC_OFF as u64;
            let d1 = d0 + 16;
            w64(d0, pp + CTRL_REQ_OFF as u64);
            w32(d0 + 8, req.len() as u32);
            w16(d0 + 12, VIRTQ_DESC_F_NEXT);
            w16(d0 + 14, 1);
            w64(d1, pp + CTRL_RESP_OFF as u64);
            w32(d1 + 8, resp_max as u32);
            w16(d1 + 12, VIRTQ_DESC_F_WRITE);
            w16(d1 + 14, 0);
            let avail = pv + CTRLQ_AVAIL_OFF as u64;
            let slot = (self.ctrl_avail_idx % QUEUE_SIZE) as u64;
            w16(avail + 4 + slot * 2, 0);
            dsb_sy();
            self.ctrl_avail_idx = self.ctrl_avail_idx.wrapping_add(1);
            w16(avail + 2, self.ctrl_avail_idx);
            dsb_sy();
            w16(self.ctrl_notify_va, VQ_CONTROL);
        }
        let used = pv + CTRLQ_USED_OFF as u64;
        let mut steps = 0u32;
        loop {
            let cur = unsafe { r16(used + 2) };
            virtio_rmb();
            if cur != self.ctrl_used_idx {
                let slot = (self.ctrl_used_idx % QUEUE_SIZE) as u64;
                let entry = used + 4 + slot * 8;
                let id = unsafe { r32(entry) };
                let len = unsafe { r32(entry + 4) } as usize;
                self.ctrl_used_idx = self.ctrl_used_idx.wrapping_add(1);
                // Level hygiene: the control completion raised INTx too.
                let _ = unsafe { r8(self.isr_va) };
                if id != 0 {
                    self.stats.bad_used = self.stats.bad_used.saturating_add(1);
                    return Err(Error::Hardware);
                }
                return Ok(len.min(resp_max));
            }
            steps += 1;
            if steps > CTRL_WAIT_STEPS {
                say!("nocturned: control request timed out (code 0x{:x})", u32::from_le_bytes([req[0], req[1], req[2], req[3]]));
                return Err(Error::Hardware);
            }
            let _ = libthyla_rs::time::sleep(Duration::from_millis(1));
        }
    }

    fn resp_u32(&self, off: usize) -> u32 {
        unsafe { r32(self.pool_va() + CTRL_RESP_OFF as u64 + off as u64) }
    }

    fn resp_u64(&self, off: usize) -> u64 {
        let lo = self.resp_u32(off) as u64;
        let hi = self.resp_u32(off + 4) as u64;
        lo | (hi << 32)
    }

    fn resp_u8(&self, off: usize) -> u8 {
        unsafe { r8(self.pool_va() + CTRL_RESP_OFF as u64 + off as u64) }
    }

    /// A stream verb with no payload beyond the pcm_hdr; checks S_OK.
    fn pcm_verb(&mut self, code: u32, what: &str) -> Result<(), Error> {
        let mut req = [0u8; 8];
        req[0..4].copy_from_slice(&code.to_le_bytes());
        req[4..8].copy_from_slice(&0u32.to_le_bytes()); // stream_id 0
        let n = self.ctrl_rpc(&req, 4)?;
        let status = if n >= 4 { self.resp_u32(0) } else { 0 };
        if status != S_OK {
            say!("nocturned: {} failed: status 0x{:x}", what, status);
            return Err(Error::Hardware);
        }
        Ok(())
    }

    fn negotiate_stream(&mut self) -> Result<(), Error> {
        // PCM_INFO for stream 0: query_info { hdr, start_id 0, count 1, size 32 }.
        let mut q = [0u8; 16];
        q[0..4].copy_from_slice(&R_PCM_INFO.to_le_bytes());
        q[4..8].copy_from_slice(&0u32.to_le_bytes());
        q[8..12].copy_from_slice(&1u32.to_le_bytes());
        q[12..16].copy_from_slice(&32u32.to_le_bytes());
        let n = self.ctrl_rpc(&q, 4 + 32)?;
        if n < 4 + 32 || self.resp_u32(0) != S_OK {
            say!("nocturned: PCM_INFO failed (len {} status 0x{:x})", n, self.resp_u32(0));
            return Err(Error::Hardware);
        }
        // virtio_snd_pcm_info after the 4-byte status: hda_fn_nid(4) features(4)
        // formats(8) rates(8) direction(1) ch_min(1) ch_max(1) pad(5).
        let features = self.resp_u32(4 + 4);
        let formats = self.resp_u64(4 + 8);
        let rates = self.resp_u64(4 + 16);
        let direction = self.resp_u8(4 + 24);
        let ch_min = self.resp_u8(4 + 25);
        let ch_max = self.resp_u8(4 + 26);
        say!(
            "nocturned: stream 0: dir={} ch={}..{} features=0x{:x} formats=0x{:x} rates=0x{:x}",
            direction, ch_min, ch_max, features, formats, rates
        );
        if direction != D_OUTPUT {
            say!("nocturned: stream 0 is not playback");
            return Err(Error::Hardware);
        }
        if formats & (1u64 << FMT_S16) == 0 || rates & (1u64 << RATE_48000) == 0 {
            say!("nocturned: stream 0 lacks S16 @ 48 kHz");
            return Err(Error::Hardware);
        }
        if !(u32::from(ch_min) <= CHANNELS && CHANNELS <= u32::from(ch_max)) {
            say!("nocturned: stream 0 cannot do {} channels", CHANNELS);
            return Err(Error::Hardware);
        }
        // SET_PARAMS { pcm_hdr(8) buffer_bytes period_bytes features channels format rate pad }.
        let mut p = [0u8; 24];
        p[0..4].copy_from_slice(&R_PCM_SET_PARAMS.to_le_bytes());
        p[4..8].copy_from_slice(&0u32.to_le_bytes());
        p[8..12].copy_from_slice(&(BUFFER_BYTES as u32).to_le_bytes());
        p[12..16].copy_from_slice(&(PERIOD_BYTES as u32).to_le_bytes());
        p[16..20].copy_from_slice(&0u32.to_le_bytes());
        p[20] = CHANNELS as u8;
        p[21] = FMT_S16;
        p[22] = RATE_48000;
        p[23] = 0;
        let n = self.ctrl_rpc(&p, 4)?;
        if n < 4 || self.resp_u32(0) != S_OK {
            say!("nocturned: SET_PARAMS failed: status 0x{:x}", self.resp_u32(0));
            return Err(Error::Hardware);
        }
        self.pcm_verb(R_PCM_PREPARE, "PCM_PREPARE")?;
        Ok(())
    }

    // ---- TX ------------------------------------------------------------------

    /// Fill slot `s` with `period` (exactly PERIOD_BYTES) and post it. The chain
    /// is fixed per slot: descs 3s (xfer hdr) -> 3s+1 (payload) -> 3s+2 (status).
    fn post_tx(&mut self, s: usize, period: &[u8]) {
        debug_assert!(s < PERIODS && period.len() == PERIOD_BYTES);
        let pv = self.pool_va();
        let pp = self.pool_pa();
        let meta_off = (TX_META_OFF + s * 64) as u64;
        let payload_off = (TX_PAYLOAD_OFF + s * PERIOD_BYTES) as u64;
        unsafe {
            // xfer header: stream_id 0.
            w32(pv + meta_off, 0);
            // status word cleared (the device writes it).
            w32(pv + meta_off + 32, 0);
            w32(pv + meta_off + 36, 0);
            // payload
            let dst = pv + payload_off;
            let mut i = 0usize;
            while i + 4 <= PERIOD_BYTES {
                let v = u32::from_le_bytes([period[i], period[i + 1], period[i + 2], period[i + 3]]);
                w32(dst + i as u64, v);
                i += 4;
            }
            let d = pv + TXQ_DESC_OFF as u64 + (3 * s as u64) * 16;
            w64(d, pp + meta_off);
            w32(d + 8, 4);
            w16(d + 12, VIRTQ_DESC_F_NEXT);
            w16(d + 14, (3 * s + 1) as u16);
            w64(d + 16, pp + payload_off);
            w32(d + 24, PERIOD_BYTES as u32);
            w16(d + 28, VIRTQ_DESC_F_NEXT);
            w16(d + 30, (3 * s + 2) as u16);
            w64(d + 32, pp + meta_off + 32);
            w32(d + 40, 8);
            w16(d + 44, VIRTQ_DESC_F_WRITE);
            w16(d + 46, 0);
            let avail = pv + TXQ_AVAIL_OFF as u64;
            let slot = (self.tx_avail_idx % QUEUE_SIZE) as u64;
            w16(avail + 4 + slot * 2, (3 * s) as u16);
            dsb_sy();
            self.tx_avail_idx = self.tx_avail_idx.wrapping_add(1);
            w16(avail + 2, self.tx_avail_idx);
            dsb_sy();
            w16(self.tx_notify_va, VQ_TX);
        }
        self.tx_inflight |= 1 << s;
    }

    /// Prime every slot from `next_period` and START the stream. `next_period`
    /// fills the slice (silence when it has nothing) and reports whether real
    /// data went in.
    pub fn start<F: FnMut(&mut [u8]) -> bool>(&mut self, mut next_period: F) -> Result<(), Error> {
        if self.started {
            return Ok(());
        }
        let mut buf = [0u8; PERIOD_BYTES];
        for s in 0..PERIODS {
            let real = next_period(&mut buf);
            if !real {
                self.stats.silence_periods = self.stats.silence_periods.saturating_add(1);
            }
            self.post_tx(s, &buf);
        }
        self.pcm_verb(R_PCM_START, "PCM_START")?;
        self.started = true;
        Ok(())
    }

    pub fn started(&self) -> bool {
        self.started
    }

    /// Consume the IRQ's pending count. Only called when the poll set reported
    /// the IRQ fd readable, so this never blocks the serve loop.
    pub fn irq_wait(&self) -> Result<u32, ()> {
        self.irq.wait().map_err(|_| ())
    }

    /// STOP + RELEASE the stream after an idle stretch and reap the flushed
    /// completions WITHOUT re-posting, then PREPARE again so the next `start`
    /// can prime + START. Best-effort on a device that stops answering.
    pub fn stop(&mut self) {
        if !self.started {
            return;
        }
        let _ = self.pcm_verb(R_PCM_STOP, "PCM_STOP");
        let _ = self.pcm_verb(R_PCM_RELEASE, "PCM_RELEASE");
        self.started = false;
        self.reap_without_repost();
        if self.pcm_verb(R_PCM_PREPARE, "PCM_PREPARE").is_err() {
            say!("nocturned: re-PREPARE after idle stop failed; the stream stays down");
        }
    }

    /// Drain the TX used ring, freeing slots, posting nothing.
    fn reap_without_repost(&mut self) {
        let _ = unsafe { r8(self.isr_va) };
        let pv = self.pool_va();
        let used = pv + TXQ_USED_OFF as u64;
        loop {
            let cur = unsafe { r16(used + 2) };
            virtio_rmb();
            if cur == self.tx_used_idx {
                break;
            }
            let slot = (self.tx_used_idx % QUEUE_SIZE) as u64;
            let id = unsafe { r32(used + 4 + slot * 8) } as usize;
            self.tx_used_idx = self.tx_used_idx.wrapping_add(1);
            if id % 3 == 0 && id / 3 < PERIODS {
                self.tx_inflight &= !(1 << (id / 3));
            } else {
                self.stats.bad_used = self.stats.bad_used.saturating_add(1);
            }
        }
        // A device that has not returned every message yet leaves stale bits;
        // the next prime overwrites those slots anyway (RELEASE completed them).
        self.tx_inflight = 0;
    }

    /// Reap completed TX messages (the period clock) and re-post each slot with
    /// the next period from `next_period`. Returns the number of periods reaped.
    /// Called on every IRQ wake AND opportunistically (a late used entry costs
    /// nothing to look for).
    pub fn pump<F: FnMut(&mut [u8]) -> bool>(&mut self, mut next_period: F) -> usize {
        // Level hygiene: read-to-clear the ISR byte.
        let _ = unsafe { r8(self.isr_va) };
        let pv = self.pool_va();
        let used = pv + TXQ_USED_OFF as u64;
        let mut reaped = 0usize;
        let mut buf = [0u8; PERIOD_BYTES];
        loop {
            let cur = unsafe { r16(used + 2) };
            virtio_rmb();
            if cur == self.tx_used_idx {
                break;
            }
            let slot = (self.tx_used_idx % QUEUE_SIZE) as u64;
            let entry = used + 4 + slot * 8;
            let id = unsafe { r32(entry) } as usize;
            let _len = unsafe { r32(entry + 4) };
            self.tx_used_idx = self.tx_used_idx.wrapping_add(1);
            // The used id is DEVICE-controlled: it must name a chain head we posted
            // (3s for an in-flight s). Anything else is dropped without re-posting.
            if id % 3 != 0 || id / 3 >= PERIODS || self.tx_inflight & (1 << (id / 3)) == 0 {
                self.stats.bad_used = self.stats.bad_used.saturating_add(1);
                continue;
            }
            let s = id / 3;
            self.tx_inflight &= !(1 << s);
            let meta = pv + (TX_META_OFF + s * 64) as u64;
            let status = unsafe { r32(meta + 32) };
            let latency = unsafe { r32(meta + 36) };
            if status != S_OK {
                self.stats.tx_errors = self.stats.tx_errors.saturating_add(1);
            }
            // latency_bytes is bounded to the buffer we declared (a device cannot
            // report more queued than it was ever given).
            self.stats.last_latency_bytes = latency.min(BUFFER_BYTES as u32);
            self.stats.periods_played = self.stats.periods_played.saturating_add(1);
            reaped += 1;
            let real = next_period(&mut buf);
            if !real {
                self.stats.silence_periods = self.stats.silence_periods.saturating_add(1);
            }
            self.post_tx(s, &buf);
        }
        reaped
    }

    /// Stop + release the stream and reset the device (`device_status = 0`), so
    /// no further DMA reaches the pool. Best-effort: a device that stopped
    /// answering is reset regardless. UNREACHED at N-1: the warden's
    /// DeviceRemoved is a forced group-terminate that skips Drop (the netdev
    /// precedent), so a cooperative quiesce-on-remove is the MENAGERIE section
    /// 10 seam this driver inherits rather than closes.
    #[allow(dead_code)]
    pub fn quiesce(&mut self) {
        if self.started {
            let _ = self.pcm_verb(R_PCM_STOP, "PCM_STOP");
            self.started = false;
        }
        let _ = self.pcm_verb(R_PCM_RELEASE, "PCM_RELEASE");
        unsafe {
            w8(self.common_va + CCFG_DEVICE_STATUS, 0);
            dsb_sy();
        }
    }
}

fn setup_queue(common: u64, queue: u16, desc_pa: u64, avail_pa: u64, used_pa: u64) -> Option<u16> {
    unsafe {
        w16(common + CCFG_QUEUE_SELECT, queue);
        let max = r16(common + CCFG_QUEUE_SIZE);
        if max < QUEUE_SIZE {
            return None;
        }
        w16(common + CCFG_QUEUE_SIZE, QUEUE_SIZE);
        w64(common + CCFG_QUEUE_DESC, desc_pa);
        w64(common + CCFG_QUEUE_DRIVER, avail_pa);
        w64(common + CCFG_QUEUE_DEVICE, used_pa);
        w16(common + CCFG_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
        let off = r16(common + CCFG_QUEUE_NOTIFY_OFF);
        w16(common + CCFG_QUEUE_ENABLE, 1);
        Some(off)
    }
}

/// The descriptor-table entry count the pool layout assumes (a build-time pin).
const _: () = assert!(QUEUE_SIZE_USZ * 16 <= PAGE);
const _: () = assert!(6 + 2 * QUEUE_SIZE_USZ + 2 <= PAGE);
const _: () = assert!(6 + 8 * QUEUE_SIZE_USZ + 2 <= PAGE);
const _: () = assert!(PERIODS * 64 <= PAGE);
const _: () = assert!(3 * PERIODS <= QUEUE_SIZE_USZ);
const _: () = assert!(PERIOD_BYTES % FRAME_BYTES == 0);
