// The virtio-gpu device half of tapestryd (Tapestry G-3; TAPESTRY.md
// section 18). A port of the audited G-1 gpud device machinery (itself the
// P4-L probe's command layer over the netdev VirtioNetPci PCI transport),
// generalized from the one-baked-framebuffer shape to the compositor's
// needs: per-surface 2D resources, whole-weave ATTACH_BACKING, per-present
// offset TRANSFER_TO_HOST_2D, and the retire pair
// (DETACH_BACKING + RESOURCE_UNREF) the one-shot lineage never needed.
//
// Every command is synchronous: submit the 2-descriptor chain, kick the
// doorbell, wait the INTx IRQ, verify used.idx + the response type. That
// synchrony is load-bearing for the stage-0 I-40 present half: a present's
// TRANSFER window opens and closes INSIDE one server dispatch, so the
// in-flight set is empty at every retire decision point (the
// tapestry_present.tla quiesce obligation holds by construction; the
// pipelined controlq is the G-6+ lift and must implement the real drain).

use libdriver::Error;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, Irq, PciDev, PciRegion,
};
use libthyla_rs::time::Instant;
use libthyla_rs::virtio_rmb;
use libthyla_rs::{T_PROT_READ, T_PROT_WRITE};

pub const PAGE_SIZE: u64 = 0x1000;

/// The VirtIO device id for a GPU (VIRTIO 1.2 section 5.7).
pub const VIRTIO_DEVICE_ID_GPU: u32 = 16;

// device_status bits (a u8 in common cfg).
const STATUS_ACKNOWLEDGE: u8 = 1;
const STATUS_DRIVER: u8 = 2;
const STATUS_DRIVER_OK: u8 = 4;
const STATUS_FEATURES_OK: u8 = 8;
const STATUS_FAILED: u8 = 128;

// VIRTIO_F_VERSION_1 is feature bit 32 -- bit 0 of the HIGH feature dword.
const VIRTIO_F_VERSION_1_BIT_HI: u32 = 1 << 0;

// VIRTIO_GPU_F_VIRGL is feature bit 0 -- bit 0 of the LOW feature dword.
// Offered only by the -gl device models. Acking it selects the host's virgl
// command path; every 2D command this driver issues remains valid under it
// (VIRTIO 1.2 section 5.7.6.8), so the compositor path is unchanged either way.
const VIRTIO_GPU_F_VIRGL_BIT_LO: u32 = 1 << 0;

// ISR status (section 4.1.4.5): bit 0 = a virtqueue raised the IRQ; reading
// the byte clears it.
const ISR_QUEUE: u8 = 1 << 0;

const VIRTIO_MSI_NO_VECTOR: u16 = 0xFFFF;

// virtio_pci_common_cfg field offsets (section 4.1.4.3).
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

// virtio-gpu protocol constants (VIRTIO 1.2 section 5.7).
const GPU_QUEUE_CONTROL: u16 = 0;
const GPU_QUEUE_CURSOR: u16 = 1;
pub const GPU_CTRL_HDR_LEN: u32 = 24;

const VIRTIO_GPU_CMD_GET_DISPLAY_INFO: u32 = 0x0100;
const VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: u32 = 0x0101;
const VIRTIO_GPU_CMD_RESOURCE_UNREF: u32 = 0x0102;
const VIRTIO_GPU_CMD_SET_SCANOUT: u32 = 0x0103;
const VIRTIO_GPU_CMD_RESOURCE_FLUSH: u32 = 0x0104;
const VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D: u32 = 0x0105;
const VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: u32 = 0x0106;
const VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: u32 = 0x0107;
const VIRTIO_GPU_CMD_GET_CAPSET_INFO: u32 = 0x0108;
const VIRTIO_GPU_CMD_GET_CAPSET: u32 = 0x0109;

// 3D commands (VIRTIO 1.2 section 5.7.6.9; virgl-negotiated only). The seam's
// context lifecycle + resource plumbing (Warp-2c); SUBMIT_3D + the transfers
// arrive with the fenced lane (Warp-2d).
const VIRTIO_GPU_CMD_CTX_CREATE: u32 = 0x0200;
const VIRTIO_GPU_CMD_CTX_DESTROY: u32 = 0x0201;
const VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE: u32 = 0x0202;
const VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE: u32 = 0x0203;
const VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: u32 = 0x0204;

const VIRTIO_GPU_RESP_OK_NODATA: u32 = 0x1100;
const VIRTIO_GPU_RESP_OK_DISPLAY_INFO: u32 = 0x1101;
const VIRTIO_GPU_RESP_OK_CAPSET_INFO: u32 = 0x1102;
const VIRTIO_GPU_RESP_OK_CAPSET: u32 = 0x1103;

// struct virtio_gpu_config field offsets in the Device region (section 5.7.4):
// events_read @0x00, events_clear @0x04, num_scanouts @0x08, num_capsets @0x0C.
const GPUCFG_NUM_SCANOUTS: u64 = 0x08;
const GPUCFG_NUM_CAPSETS: u64 = 0x0C;
const GPUCFG_MIN_LEN: u32 = 0x10;

// Capset ids (section 5.7.6.8 + virglrenderer): 1 = VIRGL, 2 = VIRGL2 (what
// Mesa's virgl driver prefers when offered); higher ids (venus, cross-domain,
// drm) are future consumers. Enumeration is bounded, not trusted.
const GPU_CAPSET_ENUM_MAX: u32 = 8;

pub const VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM: u32 = 1;

const GPU_MAX_SCANOUTS: u32 = 16;
const GPU_DISPLAY_ONE_LEN: u32 = 24;
const GPU_RESP_DISPLAY_INFO_LEN: u32 = GPU_CTRL_HDR_LEN + GPU_MAX_SCANOUTS * GPU_DISPLAY_ONE_LEN;

const VIRTQ_DESC_F_NEXT: u16 = 1;
const VIRTQ_DESC_F_WRITE: u16 = 2;

// Wall-clock bound on the STALE-WAKE regime before the submit is declared
// wedged (the G-5 F1 close). Anchored LAZILY at the first stale wake -- a
// device that never interrupts at all still blocks in irq.wait() forever
// (the pre-existing all-virtio-drivers posture) -- so the deadline trips
// only on EVENT-ful non-progress: >= this many ms of interrupt-ish events
// (a config-IRQ train from host window resizes, re-latched levels) with
// used.idx never retiring our command. A healthy device retires a 2D
// command in microseconds, so 500 ms is 5+ orders of margin; the first-cut
// bound here was a wake COUNT (16), which a resize config-storm during one
// slow-but-healthy present could exhaust -- a false dead-latch whose
// consequence is exactly the permanent console loss #31 exists to prevent.
const SUBMIT_DEADLINE_MS: u64 = 500;
// Bounded used.idx re-poll between stale wakes. If the wake WAS our
// completion's notification but the used.idx store is still propagating
// (the #31 live-display window), no further interrupt is coming -- the
// spin is the net for that world; the re-wait is the net for the
// stale-latched-edge world. ~tens of us of (barrier + load).
const USED_SPIN_PER_WAKE: u32 = 65536;

/// The QEMU-virt virtio-gpu default scanout geometry, used when
/// GET_DISPLAY_INFO reports no enabled scanout (never observed on QEMU;
/// fail-soft rather than fail-closed -- the display size is policy, not
/// soundness).
const DEFAULT_DISPLAY_W: u32 = 1024;
const DEFAULT_DISPLAY_H: u32 = 768;
/// A sanity clamp on the device-reported geometry (an absurd size would
/// size every fullscreen weave allocation).
const MAX_DISPLAY_DIM: u32 = 8192;

// Two-page DMA ring (the gpud/probe audited single-page layout, plus a
// dedicated RESP page): page 1 holds both queues + the request region
// unchanged; the response region moved to its own page because the
// GET_CAPSET blob (virgl_caps_v2, ~1.2 KiB and growing upstream) outgrew
// the 0x300-byte slice page 1 had left. The Warp-2/3 winsys get_caps slot
// reads the same blob, so the headroom is a direct next consumer, not slack.
const QUEUE_SIZE: u16 = 16;
pub const RING_DMA_SIZE: usize = 2 * PAGE_SIZE as usize;

const CTRL_DESC_OFF: u64 = 0x000;
const CTRL_AVAIL_OFF: u64 = 0x100;
const CTRL_USED_OFF: u64 = 0x200;
const CURSOR_DESC_OFF: u64 = 0x300;
const CURSOR_AVAIL_OFF: u64 = 0x400;
const CURSOR_USED_OFF: u64 = 0x500;
const REQ_OFF: u64 = 0x600;
const RESP_OFF: u64 = 0x1000;

const REQ_REGION_LEN: u32 = 0x100;
const RESP_REGION_LEN: u32 = 0x1000;

const _: () = {
    assert!(CTRL_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= CTRL_AVAIL_OFF);
    assert!(CTRL_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= CTRL_USED_OFF);
    assert!(CTRL_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= CURSOR_DESC_OFF);
    assert!(REQ_OFF + (REQ_REGION_LEN as u64) <= RESP_OFF);
    assert!(RESP_OFF + (RESP_REGION_LEN as u64) <= RING_DMA_SIZE as u64);
    assert!(GPU_RESP_DISPLAY_INFO_LEN <= RESP_REGION_LEN);
};

#[inline(always)]
unsafe fn r32(addr: u64) -> u32 {
    mmio_read32(addr)
}
#[inline(always)]
unsafe fn w32(addr: u64, v: u32) {
    mmio_write32(addr, v)
}
#[inline(always)]
unsafe fn r16(addr: u64) -> u16 {
    mmio_read16(addr)
}
#[inline(always)]
unsafe fn w16(addr: u64, v: u16) {
    mmio_write16(addr, v)
}
#[inline(always)]
unsafe fn w64(addr: u64, v: u64) {
    mmio_write64(addr, v)
}
#[inline(always)]
unsafe fn r8(addr: u64) -> u8 {
    mmio_read8(addr)
}
#[inline(always)]
unsafe fn w8(addr: u64, v: u8) {
    mmio_write8(addr, v)
}
#[inline(always)]
pub fn dsb_sy() {
    unsafe { core::arch::asm!("dsb sy", options(nostack, preserves_flags)) }
}

pub fn prewarm(va: u64, size: usize) {
    let mut off = 0u64;
    while (off as usize) < size {
        unsafe { w8(va + off, 0) };
        off += PAGE_SIZE;
    }
}

fn setup_queue(common: u64, queue: u16, desc_pa: u64, avail_pa: u64, used_pa: u64) -> Option<u16> {
    unsafe {
        w16(common + CCFG_QUEUE_SELECT, queue);
        if r16(common + CCFG_QUEUE_SIZE) < QUEUE_SIZE {
            return None;
        }
        w16(common + CCFG_QUEUE_SIZE, QUEUE_SIZE);
        w64(common + CCFG_QUEUE_DESC, desc_pa);
        w64(common + CCFG_QUEUE_DRIVER, avail_pa);
        w64(common + CCFG_QUEUE_DEVICE, used_pa);
        w16(common + CCFG_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
        let notify_off = r16(common + CCFG_QUEUE_NOTIFY_OFF);
        w16(common + CCFG_QUEUE_ENABLE, 1);
        Some(notify_off)
    }
}

fn init_device(
    common: u64,
    notify_base: u64,
    notify_mul: u64,
    notify_len: u64,
    ring_pa: u64,
) -> Result<(u64, bool), Error> {
    unsafe {
        w8(common + CCFG_DEVICE_STATUS, 0);
        w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE);
        w8(common + CCFG_DEVICE_STATUS, STATUS_ACKNOWLEDGE | STATUS_DRIVER);
        w16(common + CCFG_CONFIG_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);

        w32(common + CCFG_DEVICE_FEATURE_SELECT, 0);
        let dev_feat_lo = r32(common + CCFG_DEVICE_FEATURE);
        w32(common + CCFG_DEVICE_FEATURE_SELECT, 1);
        let dev_feat_hi = r32(common + CCFG_DEVICE_FEATURE);
        if dev_feat_hi & VIRTIO_F_VERSION_1_BIT_HI == 0 {
            say!("tapestryd: gpu lacks VIRTIO_F_VERSION_1");
            w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
            return Err(Error::Hardware);
        }

        let virgl = dev_feat_lo & VIRTIO_GPU_F_VIRGL_BIT_LO != 0;
        w32(common + CCFG_DRIVER_FEATURE_SELECT, 0);
        w32(
            common + CCFG_DRIVER_FEATURE,
            if virgl { VIRTIO_GPU_F_VIRGL_BIT_LO } else { 0 },
        );
        w32(common + CCFG_DRIVER_FEATURE_SELECT, 1);
        w32(common + CCFG_DRIVER_FEATURE, VIRTIO_F_VERSION_1_BIT_HI);

        w8(
            common + CCFG_DEVICE_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK,
        );
        if r8(common + CCFG_DEVICE_STATUS) & STATUS_FEATURES_OK == 0 {
            say!("tapestryd: gpu FEATURES_OK rejected");
            w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
            return Err(Error::Hardware);
        }

        let ctrl_off = match setup_queue(
            common,
            GPU_QUEUE_CONTROL,
            ring_pa + CTRL_DESC_OFF,
            ring_pa + CTRL_AVAIL_OFF,
            ring_pa + CTRL_USED_OFF,
        ) {
            Some(o) => o,
            None => {
                say!("tapestryd: gpu controlq size below QUEUE_SIZE");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        };
        let cursor_off = match setup_queue(
            common,
            GPU_QUEUE_CURSOR,
            ring_pa + CURSOR_DESC_OFF,
            ring_pa + CURSOR_AVAIL_OFF,
            ring_pa + CURSOR_USED_OFF,
        ) {
            Some(o) => o,
            None => {
                say!("tapestryd: gpu cursorq size below QUEUE_SIZE");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        };

        // Bound each device-supplied doorbell offset within the notify region
        // (the pci-3 F2 guard).
        for off in [ctrl_off, cursor_off] {
            if u64::from(off) * notify_mul + 2 > notify_len {
                say!("tapestryd: gpu notify doorbell past the notify region");
                w8(common + CCFG_DEVICE_STATUS, STATUS_FAILED);
                return Err(Error::Hardware);
            }
        }

        w8(
            common + CCFG_DEVICE_STATUS,
            STATUS_ACKNOWLEDGE | STATUS_DRIVER | STATUS_FEATURES_OK | STATUS_DRIVER_OK,
        );
        Ok((notify_base + u64::from(ctrl_off) * notify_mul, virgl))
    }
}

struct Controlq {
    ring_va: u64,
    ring_pa: u64,
    notify_va: u64,
    isr_va: u64,
    irq: Irq,
    seq: u16,
    /// Latched on any submit failure: the engine's seq tracker can no
    /// longer be trusted against the device's avail consumption, so every
    /// later submit would read a freshly-zeroed response buffer as
    /// resp_type=0x0 (the #31 cascade). Fail fast + honestly instead.
    dead: bool,
}

impl Controlq {
    fn submit_and_wait(&mut self, req_len: u32, resp_len: u32) -> Result<u32, ()> {
        if self.dead {
            return Err(());
        }
        let next_seq = self.seq.wrapping_add(1);

        for i in 0..(GPU_CTRL_HDR_LEN as u64) {
            unsafe { w8(self.ring_va + RESP_OFF + i, 0) };
        }

        let desc_va = self.ring_va + CTRL_DESC_OFF;
        unsafe {
            w64(desc_va + 0, self.ring_pa + REQ_OFF);
            w32(desc_va + 8, req_len);
            w16(desc_va + 12, VIRTQ_DESC_F_NEXT);
            w16(desc_va + 14, 1);

            w64(desc_va + 16, self.ring_pa + RESP_OFF);
            w32(desc_va + 24, resp_len);
            w16(desc_va + 28, VIRTQ_DESC_F_WRITE);
            w16(desc_va + 30, 0);
        };

        let avail_va = self.ring_va + CTRL_AVAIL_OFF;
        let ring_slot = (self.seq % QUEUE_SIZE) as u64;
        unsafe {
            w16(avail_va + 0, 0);
            w16(avail_va + 4 + ring_slot * 2, 0);
        };

        // VIRTIO 1.2 2.7.13.1: descriptor + ring writes visible before the
        // idx bump.
        dsb_sy();
        unsafe { w16(avail_va + 2, next_seq) };
        dsb_sy();

        unsafe { w16(self.notify_va, GPU_QUEUE_CONTROL) };

        // The completion authority is the USED RING, never the ISR bit.
        // VIRTIO 1.2 2.7.13.2 orders the device's used.idx write before its
        // notification, but a wake proves only that SOME notification-ish
        // event reached us: irqfwd collapses INTx edges, a level re-fire or
        // a config event can latch a stale pending event, and under a live
        // display backend (#31, -display cocoa) such a mis-timed wake is
        // routine. The pre-fix shape -- break on the first ISR_QUEUE wake,
        // read used.idx ONCE, hard-fail if behind -- turned that benign
        // timing into a permanent engine desync (seq diverges from the
        // device's avail consumption; every later command re-publishes a
        // consumed avail idx and reads its own zeroed response buffer as
        // resp_type=0x0). Wait until used.idx retires OUR command, bounded.
        let used_va = self.ring_va + CTRL_USED_OFF;
        let mut wakes = 0u32;
        let mut stale_since: Option<Instant> = None;
        'wait: loop {
            if self.irq.wait().is_err() {
                say!("tapestryd: gpu SYS_IRQ_WAIT returned error");
                self.dead = true;
                return Err(());
            }
            // Read-to-clear on every wake: consumes + deasserts the INTx
            // source (level hygiene). Deliberately NOT the break condition.
            let _ = unsafe { r8(self.isr_va) };
            virtio_rmb();
            let used_idx = unsafe { r16(used_va + 2) };
            if used_idx == next_seq {
                break 'wait;
            }
            if used_idx != self.seq {
                // Neither not-yet (seq) nor done (next_seq): single-in-flight
                // submission makes any other value ring corruption, not timing.
                say!(
                    "tapestryd: gpu used.idx {} outside {}..{} (ring corrupt)",
                    used_idx, self.seq, next_seq
                );
                self.dead = true;
                return Err(());
            }
            // Not ours yet. If this wake WAS our completion's notification
            // with the used.idx store still propagating, no further
            // interrupt is coming -- spin-poll briefly before re-waiting.
            for _ in 0..USED_SPIN_PER_WAKE {
                virtio_rmb();
                if unsafe { r16(used_va + 2) } == next_seq {
                    break 'wait;
                }
            }
            wakes += 1;
            let t0 = *stale_since.get_or_insert_with(Instant::now);
            if t0.elapsed().as_millis() as u64 >= SUBMIT_DEADLINE_MS {
                say!(
                    "tapestryd: gpu command never retired ({} stale wakes over {} ms)",
                    wakes, SUBMIT_DEADLINE_MS
                );
                self.dead = true;
                return Err(());
            }
        }

        // The spin-break path can exit with the COMPLETION's own INTx
        // assertion unconsumed (the wake-path read above cleared only the
        // PRIOR level; the device re-asserts when it bumps used.idx during
        // the spin). Read-to-clear once more so a retired command's own
        // assertion cannot surface as the next submit's stale wake (G-5 F2).
        let _ = unsafe { r8(self.isr_va) };

        // VIRTIO 1.2 2.7.13.2: order the used.idx observation before the
        // response-buffer read.
        virtio_rmb();
        let resp_type = unsafe { r32(self.ring_va + RESP_OFF) };
        self.seq = next_seq;
        Ok(resp_type)
    }

    fn step(&mut self, label: &str, req_len: u32, resp_len: u32, expected: u32) -> Result<(), Error> {
        match self.submit_and_wait(req_len, resp_len) {
            Ok(t) if t == expected => Ok(()),
            Ok(t) => {
                say!("tapestryd: gpu {} resp_type={:#x} (expected {:#x})", label, t, expected);
                Err(Error::Hardware)
            }
            Err(()) => {
                say!("tapestryd: gpu {} submit error", label);
                Err(Error::Hardware)
            }
        }
    }
}

// Request builders (VIRTIO 1.2 section 5.7.6 layouts; probe-derived).

unsafe fn write_ctrl_hdr(req_va: u64, cmd_type: u32) {
    w32(req_va + 0, cmd_type);
    w32(req_va + 4, 0);
    w64(req_va + 8, 0);
    w32(req_va + 16, 0);
    w8(req_va + 20, 0);
    w8(req_va + 21, 0);
    w8(req_va + 22, 0);
    w8(req_va + 23, 0);
}

// The ctx-scoped header (ctx_id at offset 16): CTX_* + SUBMIT_3D address a
// specific rendering context; everything else leaves it 0.
unsafe fn write_ctrl_hdr_ctx(req_va: u64, cmd_type: u32, ctx_id: u32) {
    write_ctrl_hdr(req_va, cmd_type);
    w32(req_va + 16, ctx_id);
}

unsafe fn write_rect(va: u64, x: u32, y: u32, w: u32, h: u32) {
    w32(va + 0, x);
    w32(va + 4, y);
    w32(va + 8, w);
    w32(va + 12, h);
}

/// The GPU device: the claimed PCI function, the command ring, and the
/// display geometry. Owns the RAII handles for the Proc's lifetime
/// (persistent driver; the RW-7 quiesce at reap is the teardown).
pub struct Gpu {
    ctrl: Controlq,
    ring_va: u64,
    pub width: u32,
    pub height: u32,
    /// VIRTIO_GPU_F_VIRGL negotiated (the -gl device models). Warp-1 keys
    /// the capset probe on it; Warp-2 keys the 3D context path.
    pub virgl: bool,
    pub num_capsets: u32,
    /// The fetched preferred capset (Warp-2c: served verbatim by the
    /// /dev/warp caps file; empty when no capset was fetchable). The id +
    /// version ride the warp ctl line so a client can decode the blob.
    pub capset_blob: alloc::vec::Vec<u8>,
    pub capset_id: u32,
    pub capset_ver: u32,
    pci: PciDev,
    _ring: Dma,
}

impl Gpu {
    /// Claim + bring up the virtio-gpu PCI function: transport handshake,
    /// both queues, then GET_DISPLAY_INFO for the scanout geometry.
    pub fn probe(bar_window_va: u64, ring_va: u64) -> Result<Gpu, Error> {
        let pci = unsafe { PciDev::claim(VIRTIO_DEVICE_ID_GPU, bar_window_va) }.map_err(|e| {
            say!("tapestryd: gpu PCI claim/map failed {:?}", e);
            Error::Hardware
        })?;

        let (common_va, common_len) = pci.region(PciRegion::Common).ok_or_else(|| {
            say!("tapestryd: gpu no common-cfg region");
            Error::Hardware
        })?;
        if common_len < CCFG_MIN_LEN {
            say!("tapestryd: gpu common-cfg region too small ({})", common_len);
            return Err(Error::Hardware);
        }
        let (notify_base, notify_len) = pci.region(PciRegion::Notify).ok_or_else(|| {
            say!("tapestryd: gpu no notify region");
            Error::Hardware
        })?;
        let isr_va = pci
            .region(PciRegion::Isr)
            .ok_or_else(|| {
                say!("tapestryd: gpu no ISR region");
                Error::Hardware
            })?
            .0;
        let notify_mul = u64::from(pci.notify_off_multiplier());
        let intid = pci.intid().ok_or_else(|| {
            say!("tapestryd: gpu no INTx INTID resolved");
            Error::Hardware
        })?;

        let irq = Irq::new(intid, Rights::SIGNAL).map_err(|_| {
            say!("tapestryd: SYS_IRQ_CREATE failed for gpu intid {}", intid);
            Error::Hardware
        })?;

        let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;
        let ring = unsafe { Dma::new(RING_DMA_SIZE, rw_map, ring_va, prot) }.map_err(|_| {
            say!("tapestryd: SYS_DMA_CREATE(gpu ring) failed");
            Error::Hardware
        })?;
        prewarm(ring.base_va() as u64, RING_DMA_SIZE);
        let ring_pa = ring.paddr();

        let (notify_va, virgl) =
            init_device(common_va, notify_base, notify_mul, u64::from(notify_len), ring_pa)?;

        let mut gpu = Gpu {
            ctrl: Controlq {
                ring_va,
                ring_pa,
                notify_va,
                isr_va,
                irq,
                seq: 0,
                dead: false,
            },
            ring_va,
            width: DEFAULT_DISPLAY_W,
            height: DEFAULT_DISPLAY_H,
            virgl,
            num_capsets: 0,
            capset_blob: alloc::vec::Vec::new(),
            capset_id: 0,
            capset_ver: 0,
            pci,
            _ring: ring,
        };

        gpu.read_display_info()?;
        if gpu.virgl {
            gpu.probe_capsets()?;
        }
        say!(
            "tapestryd: gpu up -- {}x{}, pci intid={}, virgl={} capsets={}",
            gpu.width,
            gpu.height,
            intid,
            gpu.virgl as u32,
            gpu.num_capsets
        );
        Ok(gpu)
    }

    /// The Warp-1 capset probe (GPU-DESIGN.md section 12): enumerate the
    /// virgl capability sets and fetch the preferred blob, as boot-log
    /// evidence that the 3D command path round-trips. Only `num_capsets`
    /// is kept -- the object model lands at Warp-2. Runs only when VIRGL
    /// was negotiated, so a 2D-device boot never executes it (its two
    /// residual deltas -- the RESP page PA and this log line's format --
    /// are behavior-equivalent, not byte-identical).
    ///
    /// Failure disposition (audit W1 F1): a missing/short device-cfg
    /// region OR a capset command the device merely REFUSES (resp-type
    /// mismatch -- `submit_and_wait` returned Ok, so the engine is
    /// healthy and `Controlq.dead` is UNSET) degrades to 2D with a log
    /// line: the probe is evidence, and evidence must never cost the
    /// console (a probe-fatal tapestryd would warden-restart-loop with
    /// no renderer on a device whose 2D path works). Only a real engine
    /// death (`dead` latched: wait error / deadline / ring corruption)
    /// propagates -- there every later 2D command would fail anyway, and
    /// honest-fast beats a doomed limp.
    fn probe_capsets(&mut self) -> Result<(), Error> {
        let (dev_va, dev_len) = match self.pci.region(PciRegion::Device) {
            Some(r) => r,
            None => {
                say!("tapestryd: gpu virgl but no device-cfg region; capsets unknown");
                return Ok(());
            }
        };
        if dev_len < GPUCFG_MIN_LEN {
            say!("tapestryd: gpu device-cfg region too small ({}); capsets unknown", dev_len);
            return Ok(());
        }
        let scanouts = unsafe { r32(dev_va + GPUCFG_NUM_SCANOUTS) };
        self.num_capsets = unsafe { r32(dev_va + GPUCFG_NUM_CAPSETS) };
        say!(
            "tapestryd: gpu virgl -- num_scanouts={} num_capsets={}",
            scanouts,
            self.num_capsets
        );

        // Rank: VIRGL2 (2) > VIRGL (1) > anything else; fetch one blob.
        let rank = |i: u32| -> u32 {
            match i {
                2 => 3,
                1 => 2,
                _ => 1,
            }
        };
        let mut best: Option<(u32, u32, u32)> = None;
        for idx in 0..self.num_capsets.min(GPU_CAPSET_ENUM_MAX) {
            let (id, ver, size) = match self.get_capset_info(idx) {
                Ok(t) => t,
                Err(e) => {
                    if self.ctrl.dead {
                        return Err(e);
                    }
                    say!("tapestryd: gpu capset[{}] refused (engine healthy); 2D only", idx);
                    return Ok(());
                }
            };
            say!(
                "tapestryd: gpu capset[{}] id={} max_version={} max_size={}",
                idx,
                id,
                ver,
                size
            );
            // An enumerated-but-empty capset is not fetchable (audit W1
            // F1: a VMM may list an id its renderer lacks with size 0;
            // rank-by-id alone would prefer it over a populated one).
            if size == 0 {
                continue;
            }
            if best.map_or(true, |(bid, _, _)| rank(id) > rank(bid)) {
                best = Some((id, ver, size));
            }
        }
        if let Some((id, ver, size)) = best {
            if let Err(e) = self.get_capset(id, ver, size) {
                if self.ctrl.dead {
                    return Err(e);
                }
                say!("tapestryd: gpu GET_CAPSET id={} refused (engine healthy); 2D only", id);
            }
        }
        Ok(())
    }

    /// GET_CAPSET_INFO for one index -> (capset_id, max_version, max_size).
    fn get_capset_info(&mut self, index: u32) -> Result<(u32, u32, u32), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_GET_CAPSET_INFO);
            w32(req_va + 24, index);
            w32(req_va + 28, 0);
            // Residue guard (audit W1 F2 -- the get_capset <16-byte rule,
            // applied here): the engine zeroes only the response HEADER,
            // and these three words are read with no used-len check, so a
            // short-writing device would hand back the PRIOR response's
            // bytes. Zeroed, a short write reads as size=0 -> skipped.
            let resp = self.ring_va + RESP_OFF;
            w32(resp + 24, 0);
            w32(resp + 28, 0);
            w32(resp + 32, 0);
        };
        self.ctrl.step(
            "GET_CAPSET_INFO",
            GPU_CTRL_HDR_LEN + 8,
            GPU_CTRL_HDR_LEN + 16,
            VIRTIO_GPU_RESP_OK_CAPSET_INFO,
        )?;
        let r = self.ring_va + RESP_OFF + GPU_CTRL_HDR_LEN as u64;
        Ok((unsafe { r32(r) }, unsafe { r32(r + 4) }, unsafe { r32(r + 8) }))
    }

    /// GET_CAPSET: fetch the capset blob into the RESP region and log its
    /// head -- the Warp-1 in-guest gate evidence. `size` (the device's own
    /// max_size) is clamped to the RESP region; the virgl v1/v2 blobs
    /// (308 / ~1.2K) fit whole, and a clamped fetch says so in the log.
    fn get_capset(&mut self, id: u32, version: u32, size: u32) -> Result<(), Error> {
        let take = size.min(RESP_REGION_LEN - GPU_CTRL_HDR_LEN);
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_GET_CAPSET);
            w32(req_va + 24, id);
            w32(req_va + 28, version);
        };
        self.ctrl.step(
            "GET_CAPSET",
            GPU_CTRL_HDR_LEN + 8,
            GPU_CTRL_HDR_LEN + take,
            VIRTIO_GPU_RESP_OK_CAPSET,
        )?;
        // The log is the Warp-1 gate evidence, so it must only quote bytes
        // the device wrote THIS round: submit_and_wait zeroes just the
        // response header, and a blob shorter than 16 bytes would leave the
        // word reads on a prior response's residue.
        if take < 16 {
            say!(
                "tapestryd: gpu GET_CAPSET id={} ver={} -> {} bytes (short; head not quoted)",
                id,
                version,
                take
            );
            return Ok(());
        }
        let blob = self.ring_va + RESP_OFF + GPU_CTRL_HDR_LEN as u64;
        say!(
            "tapestryd: gpu GET_CAPSET id={} ver={} -> {} bytes{}; caps[0..16] = {:#010x} {:#010x} {:#010x} {:#010x}",
            id,
            version,
            take,
            if take < size { " (clamped)" } else { "" },
            unsafe { r32(blob) },
            unsafe { r32(blob + 4) },
            unsafe { r32(blob + 8) },
            unsafe { r32(blob + 12) }
        );
        // Warp-2c: retain the blob for the /dev/warp caps file. Byte copy
        // out of the RESP region NOW -- the next command overwrites it.
        let mut kept = alloc::vec![0u8; take as usize];
        for (i, b) in kept.iter_mut().enumerate() {
            *b = unsafe { r8(blob + i as u64) };
        }
        self.capset_blob = kept;
        self.capset_id = id;
        self.capset_ver = version;
        Ok(())
    }

    /// GET_DISPLAY_INFO: adopt scanout 0's enabled rect as the display
    /// geometry (fail-soft to the default when absent or absurd).
    fn read_display_info(&mut self) -> Result<(), Error> {
        match self.query_display_info()? {
            Some((w, h)) => {
                self.width = w;
                self.height = h;
            }
            None => {
                say!(
                    "tapestryd: scanout0 absent/absurd; default {}x{}",
                    self.width,
                    self.height
                );
            }
        }
        Ok(())
    }

    /// cfg-3: probe GET_DISPLAY_INFO WITHOUT adopting -- the `mode auto`
    /// re-probe (the boot path adopts via read_display_info above).
    /// Ok(None) = scanout 0 absent or absurd (the caller fails soft).
    pub fn query_display_info(&mut self) -> Result<Option<(u32, u32)>, Error> {
        unsafe { write_ctrl_hdr(self.ring_va + REQ_OFF, VIRTIO_GPU_CMD_GET_DISPLAY_INFO) };
        self.ctrl.step(
            "GET_DISPLAY_INFO",
            GPU_CTRL_HDR_LEN,
            GPU_RESP_DISPLAY_INFO_LEN,
            VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
        )?;
        // struct virtio_gpu_display_one { rect r; u32 enabled; u32 flags; }
        // starting right after the response header; scanout 0 first.
        let d0 = self.ring_va + RESP_OFF + GPU_CTRL_HDR_LEN as u64;
        let w = unsafe { r32(d0 + 8) };
        let h = unsafe { r32(d0 + 12) };
        let enabled = unsafe { r32(d0 + 16) };
        if enabled != 0 && (1..=MAX_DISPLAY_DIM).contains(&w) && (1..=MAX_DISPLAY_DIM).contains(&h) {
            Ok(Some((w, h)))
        } else {
            Ok(None)
        }
    }

    pub fn resource_create_2d(&mut self, resource_id: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_CREATE_2D);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM);
            w32(req_va + 32, w);
            w32(req_va + 36, h);
        };
        self.ctrl
            .step("RESOURCE_CREATE_2D", GPU_CTRL_HDR_LEN + 16, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// Attach the whole weave (one physically-contiguous KObj_DMA chunk) as
    /// the resource's guest backing. Per-present slot selection rides the
    /// TRANSFER offset, so one attach serves all slots.
    pub fn attach_backing(&mut self, resource_id: u32, pa: u64, len: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 1); // nr_entries
            w64(req_va + 32, pa);
            w32(req_va + 40, len);
            w32(req_va + 44, 0);
        };
        self.ctrl
            .step("ATTACH_BACKING", GPU_CTRL_HDR_LEN + 8 + 16, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn detach_backing(&mut self, resource_id: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl
            .step("DETACH_BACKING", GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn resource_unref(&mut self, resource_id: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_UNREF);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl
            .step("RESOURCE_UNREF", GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// CTX_CREATE: mint rendering context `ctx_id` host-side. context_init
    /// stays 0 (VIRTIO_GPU_F_CONTEXT_INIT is not negotiated), so the host
    /// creates the default virgl context -- which serves both VIRGL and
    /// VIRGL2 capset consumers; the seam records the client's declared
    /// capset for the day the feature is negotiated. Virgl-gated by the
    /// caller (the warp tree answers E_OPNOTSUPP on a 2D device).
    pub fn ctx_create(&mut self, ctx_id: u32, debug_name: &[u8]) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        let nlen = debug_name.len().min(63);
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_CTX_CREATE, ctx_id);
            w32(req_va + 24, nlen as u32);
            w32(req_va + 28, 0); // context_init (F_CONTEXT_INIT not negotiated)
            for i in 0..64u64 {
                w8(req_va + 32 + i, 0);
            }
            for (i, b) in debug_name.iter().take(nlen).enumerate() {
                w8(req_va + 32 + i as u64, *b);
            }
        };
        self.ctrl
            .step("CTX_CREATE", GPU_CTRL_HDR_LEN + 72, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn ctx_destroy(&mut self, ctx_id: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe { write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_CTX_DESTROY, ctx_id) };
        self.ctrl
            .step("CTX_DESTROY", GPU_CTRL_HDR_LEN, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// Attach a device-global resource to a context: virgl scopes command
    /// streams to the resources attached to the submitting ctx -- the
    /// host-side half of the I-45 exposure bound.
    pub fn ctx_attach_resource(&mut self, ctx_id: u32, resource_id: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl
            .step("CTX_ATTACH_RESOURCE", GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn ctx_detach_resource(&mut self, ctx_id: u32, resource_id: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl
            .step("CTX_DETACH_RESOURCE", GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// RESOURCE_CREATE_3D: mint a device-global 3D resource. The parameter
    /// tuple is the client's (the Mesa winsys drives these directly); the
    /// server validates only what IT must stay sound against (the backing
    /// size at ATTACH time) -- the host renderer owns 3D-parameter validity,
    /// per the no-command-validation posture (GPU-DESIGN section 2.1).
    #[allow(clippy::too_many_arguments)]
    pub fn resource_create_3d(
        &mut self,
        resource_id: u32,
        target: u32,
        format: u32,
        bind: u32,
        width: u32,
        height: u32,
        depth: u32,
        array_size: u32,
        last_level: u32,
        nr_samples: u32,
        flags: u32,
    ) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_CREATE_3D);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, target);
            w32(req_va + 32, format);
            w32(req_va + 36, bind);
            w32(req_va + 40, width);
            w32(req_va + 44, height);
            w32(req_va + 48, depth);
            w32(req_va + 52, array_size);
            w32(req_va + 56, last_level);
            w32(req_va + 60, nr_samples);
            w32(req_va + 64, flags);
            w32(req_va + 68, 0); // padding
        };
        self.ctrl
            .step("RESOURCE_CREATE_3D", GPU_CTRL_HDR_LEN + 48, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// Bind scanout 0 to a resource (resource_id 0 = disable the scanout).
    pub fn set_scanout(&mut self, resource_id: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_SET_SCANOUT);
            write_rect(req_va + 24, 0, 0, w, h);
            w32(req_va + 40, 0); // scanout_id
            w32(req_va + 44, resource_id);
        };
        self.ctrl
            .step("SET_SCANOUT", GPU_CTRL_HDR_LEN + 24, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// TRANSFER_TO_HOST_2D: host-DMA-read the backing at `offset` into the
    /// resource rect. Rows advance by the RESOURCE stride (w*4), so `offset`
    /// = slot_base + (y*res_w + x)*4 selects both the slot and the rect
    /// origin within it.
    pub fn transfer(&mut self, resource_id: u32, offset: u64, x: u32, y: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D);
            write_rect(req_va + 24, x, y, w, h);
            w64(req_va + 40, offset);
            w32(req_va + 48, resource_id);
            w32(req_va + 52, 0);
        };
        self.ctrl
            .step("TRANSFER", GPU_CTRL_HDR_LEN + 32, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    pub fn flush(&mut self, resource_id: u32, x: u32, y: u32, w: u32, h: u32) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_FLUSH);
            write_rect(req_va + 24, x, y, w, h);
            w32(req_va + 40, resource_id);
            w32(req_va + 44, 0);
        };
        self.ctrl
            .step("FLUSH", GPU_CTRL_HDR_LEN + 24, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }
}
