// The virtio-gpu device half of tapestryd (Tapestry G-3; TAPESTRY.md
// section 18). A port of the audited G-1 gpud device machinery (itself the
// P4-L probe's command layer over the netdev VirtioNetPci PCI transport),
// generalized from the one-baked-framebuffer shape to the compositor's
// needs: per-surface 2D resources, whole-weave ATTACH_BACKING, per-present
// offset TRANSFER_TO_HOST_2D, and the retire pair
// (DETACH_BACKING + RESOURCE_UNREF) the one-shot lineage never needed.
//
// Every 2D/ctx command is synchronous: submit the 2-descriptor chain, kick
// the doorbell, wait the INTx IRQ, drain the used ring until OUR entry
// retires, verify the response type. That synchrony is load-bearing for the
// stage-0 I-40 present half: a present's TRANSFER window opens and closes
// INSIDE one server dispatch, so the 2D in-flight set is empty at every
// retire decision point (the tapestry_present.tla quiesce obligation holds
// by construction).
//
// Warp-2d adds the FENCED lane beside that synchrony: fence-bearing 3D
// chains (SUBMIT_3D, TRANSFER_*_3D) publish without waiting and retire by
// used-ENTRY attribution -- the response, withheld by the device until the
// fence signals, IS the fence completion. Presents remain wait-for-mine, so
// the I-40 argument above is untouched; the pipelined PRESENT path stays
// the G-6+ lift.

use libdriver::Error;
use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{
    mmio_read16, mmio_read32, mmio_read8, mmio_write16, mmio_write32, mmio_write64, mmio_write8,
    Dma, Irq, PciDev, PciRegion,
};
use libthyla_rs::time::Instant;
use libthyla_rs::virtio_rmb;
use libthyla_rs::{
    t_burrow_detach, t_hostmem_refcount, T_CACHE_CACHED, T_CACHE_UNCACHED, T_CACHE_WC, T_PROT_READ,
    T_PROT_WRITE,
};

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
// The rest of the LOW dword, named so a boot log can be read without a spec
// beside it. Warp-6 needs two of them: CONTEXT_INIT carries a context's capset
// id (a Venus context is unreachable without it -- `context_init` bits 0-7),
// and RESOURCE_BLOB is what Venus's ring is built from.
const VIRTIO_GPU_F_EDID_BIT_LO: u32 = 1 << 1;
const VIRTIO_GPU_F_RESOURCE_UUID_BIT_LO: u32 = 1 << 2;
const VIRTIO_GPU_F_RESOURCE_BLOB_BIT_LO: u32 = 1 << 3;
const VIRTIO_GPU_F_CONTEXT_INIT_BIT_LO: u32 = 1 << 4;

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
// Still the 2D command group, not the 3D one: GET_EDID (0x010a) and
// RESOURCE_ASSIGN_UUID (0x010b) sit between GET_CAPSET and this, both unused
// here, so the value is 0x010c and NOT contiguous with 0x0109 (VIRTIO 1.2
// section 5.7.6.7). Venus's command ring is a guest blob (GPU-DESIGN section
// 2.4), so this is Warp-6's real prerequisite -- the V-2 host3d/hostmem
// mapping path (MAP_BLOB, 0x0208) is a later, separate rung.
const VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB: u32 = 0x010c;

// blob_mem (section 5.7.6.7): GUEST = the blob's storage IS the guest
// `mem_entry` pages, no host allocation and no hostmem BAR. That is the only
// type V-1 creates; HOST3D (host-allocated, needs MAP_BLOB into the hostmem
// window) is the V-2 delta.
const VIRTIO_GPU_BLOB_MEM_GUEST: u32 = 0x0001;

// V-3b (Model B): HOST3D = host-allocated storage, no guest mem_entry
// (nr_entries=0). USE_MAPPABLE asks the host to place the blob in the hostmem
// BAR so RESOURCE_MAP_BLOB (a 3D-group command, 0x0208) can expose it at a
// chosen offset -- the substrate for Venus's host-consumed command ring.
const VIRTIO_GPU_BLOB_MEM_HOST3D: u32 = 0x0002;
const VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE: u32 = 0x0001;
// W-3c: USE_SHAREABLE marks a blob the host may hand to consumers OTHER than
// the creating context -- the display's scanout engine, and (at W-3c-2) the
// compositor context's blit. A presentable takes this WITHOUT
// USE_MAPPABLE: it is named, never guest-mapped (WARP-WSI-DESIGN 4.1).
const VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE: u32 = 0x0002;
/// Re-exported for the W-3c-1 self-test's flag-capability measurement (which
/// combination this host actually accepts for a HOST3D blob). Aliases, never
/// fresh literals -- the #230 mirror-by-meaning rule.
pub const BLOB_FLAG_MAPPABLE: u32 = VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE;
pub const BLOB_FLAG_SHAREABLE: u32 = VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE;

/// How many display-condemned resources can be parked at once. A refused
/// unbind is a device-level anomaly, not a steady state, so this is sized to
/// absorb a burst and no more; overflow is counted and leaks, never frees.
const GPU_CONDEMNED_MAX: usize = 16;

/// One display-condemned resource. `unref_requested` is what keeps the drain
/// from ACCELERATING a free: an id parked at a pre-quiesce eviction whose
/// owner has not yet asked to free it is simply un-parked by the drain, and
/// its real unref still happens later at the owner's own quiesce-safe moment
/// (round-3 F3).
#[derive(Clone, Copy)]
struct Condemned {
    res: u32,
    unref_requested: bool,
}
const VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB: u32 = 0x0208;
const VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB: u32 = 0x0209;

// W-3a (WARP-WSI-DESIGN section 4.2): the 2D-group command that binds a
// scanout to a BLOB resource, carrying the shape a blob does not have
// (format + width/height + strides/offsets -- VIRTIO 1.2 section 5.7.6.7,
// struct virtio_gpu_set_scanout_blob). Contiguous after CREATE_BLOB. Probe-
// only until the presentable object (W-3c) makes it a present-path command.
const VIRTIO_GPU_CMD_SET_SCANOUT_BLOB: u32 = 0x010d;

// virtio-gpu capset ids: 1 = VIRGL, 2 = VIRGL2, 4 = VENUS. Named so the V-0b
// probe reads without a magic 4 in three places.
const VIRTIO_GPU_CAPSET_VIRGL2: u32 = 2;
const VIRTIO_GPU_CAPSET_VENUS: u32 = 4;

// Warp-6 V-0b: the ctx ids the reachability probe creates and destroys inside
// init, BEFORE the server admits any client or creates COMPOSITOR_CTX, and are
// destroyed before Gpu::probe returns. That TIMING is the collision-freedom
// guarantee -- not the numeric window: no client (`dev_ctx = slot + 1`, server
// side) or compositor context is live while the probe runs. The static_assert
// below is belt-and-suspenders for the (today large) numeric margin, and ties
// these magic ids to the real seam constants so a future lift of MAX_WARP_CTXS
// past the probe ids fails the BUILD rather than silently narrowing the margin
// (main audit F1 / [[bug-230-lifted-constant-voids-proofs]] -- the audit round
// AND the author's self-audit both caught the old comment claiming a "128-slot
// seam" when MAX_WARP_CTXS is 8, a conflation with the per-ctx BO cap).
const PROBE_CTX_ID_VIRGL: u32 = 200;
const PROBE_CTX_ID_VENUS: u32 = 201;
const _: () = assert!(
    PROBE_CTX_ID_VIRGL > crate::server::MAX_WARP_CTXS as u32
        && PROBE_CTX_ID_VENUS < crate::server::COMPOSITOR_CTX
        && PROBE_CTX_ID_VIRGL != PROBE_CTX_ID_VENUS,
    "probe ctx ids must sit above the client seam and below COMPOSITOR_CTX, and be distinct"
);

// Warp-6 V-1: the resource id the guest-blob probe creates and unrefs inside
// init, BEFORE the Server exists (Gpu::probe returns the device; the Server is
// built on it afterward), so no client resource is live when it runs -- the
// same timing guarantee the ctx-capset probe rests on. The guard is the numeric
// belt-and-suspenders: the server mints resource ids from SCREEN_RES+1 UPWARD
// and never down (server.rs `next_res_id` pre-increments a counter seeded at
// SCREEN_RES), so any id <= SCREEN_RES is unmintable forever; tying it to the
// seam constant re-checks it if that seed ever changes.
const BLOB_PROBE_RES_ID: u32 = 0x2b;
const _: () = assert!(
    BLOB_PROBE_RES_ID < crate::server::SCREEN_RES,
    "blob probe resource id must sit below the server's first minted id"
);

// Warp-6 V-3b-1a: the HOST3D+MAP_BLOB reachability probe's ids, created and
// destroyed inside init alongside blob_probe (the same pre-Server timing
// guarantee -- no client resource or context is live). Two resource ids settle
// the open question of whether a HOST3D mappable blob needs a context: one is
// mapped under a real virgl context, one device-global (ctx_id 0). All below
// SCREEN_RES (unmintable by the server); the ctx above the client seam and
// below COMPOSITOR_CTX, distinct from the capset-probe ctxs.
const HOST3D_PROBE_RES_CTX: u32 = 0x2c;
const HOST3D_PROBE_RES_GLOBAL: u32 = 0x2d;
const HOST3D_PROBE_CTX_ID: u32 = 202;
const _: () = assert!(
    HOST3D_PROBE_RES_CTX < crate::server::SCREEN_RES
        && HOST3D_PROBE_RES_GLOBAL < crate::server::SCREEN_RES
        && HOST3D_PROBE_RES_CTX != HOST3D_PROBE_RES_GLOBAL
        && HOST3D_PROBE_RES_CTX != BLOB_PROBE_RES_ID
        && HOST3D_PROBE_RES_GLOBAL != BLOB_PROBE_RES_ID
        && HOST3D_PROBE_CTX_ID > crate::server::MAX_WARP_CTXS as u32
        && HOST3D_PROBE_CTX_ID < crate::server::COMPOSITOR_CTX
        && HOST3D_PROBE_CTX_ID != PROBE_CTX_ID_VIRGL
        && HOST3D_PROBE_CTX_ID != PROBE_CTX_ID_VENUS,
    "host3d probe ids must sit below SCREEN_RES / within the ctx window and be distinct"
);

// Warp-6 V-3b-1b: the hostmem guest-map probe's ids -- one HOST3D blob created
// under a venus ctx, mapped, then guest-mapped via SYS_BURROW_FROM_HOSTMEM and
// sentinel-round-tripped. Same pre-Server init timing as host3d_probe; distinct
// from every other fixed probe id.
const HOSTMEM_PROBE_RES: u32 = 0x2e;
// V-3b-1c: a second probe resource so hostmem_ring_probe can hold two live
// HOST3D rings at once and prove the allocator hands DISTINCT offsets.
const HOSTMEM_PROBE_RES_2: u32 = 0x2f;
const HOSTMEM_PROBE_CTX_ID: u32 = 203;
const _: () = assert!(
    HOSTMEM_PROBE_RES < crate::server::SCREEN_RES
        && HOSTMEM_PROBE_RES_2 < crate::server::SCREEN_RES
        && HOSTMEM_PROBE_RES != HOSTMEM_PROBE_RES_2
        && HOSTMEM_PROBE_RES != BLOB_PROBE_RES_ID
        && HOSTMEM_PROBE_RES_2 != BLOB_PROBE_RES_ID
        && HOSTMEM_PROBE_RES != HOST3D_PROBE_RES_CTX
        && HOSTMEM_PROBE_RES_2 != HOST3D_PROBE_RES_CTX
        && HOSTMEM_PROBE_RES != HOST3D_PROBE_RES_GLOBAL
        && HOSTMEM_PROBE_RES_2 != HOST3D_PROBE_RES_GLOBAL
        && HOSTMEM_PROBE_CTX_ID > crate::server::MAX_WARP_CTXS as u32
        && HOSTMEM_PROBE_CTX_ID < crate::server::COMPOSITOR_CTX
        && HOSTMEM_PROBE_CTX_ID != PROBE_CTX_ID_VIRGL
        && HOSTMEM_PROBE_CTX_ID != PROBE_CTX_ID_VENUS
        && HOSTMEM_PROBE_CTX_ID != HOST3D_PROBE_CTX_ID,
    "hostmem probe ids must sit below SCREEN_RES / within the ctx window and be distinct"
);

/// V-3b-1b: a page-aligned allocator over the hostmem BAR's shm region, handing
/// out non-overlapping byte offsets (relative to the region window base -- the
/// frame `map_blob` and `burrow_from_hostmem` both use) for HOST3D ring blobs.
/// V-3b-1c makes it persistent (a Gpu field) and adds a first-fit free-list so a
/// retired ring's offset is reclaimed: a persistent daemon mints and tears down
/// rings across client sessions, so bump-only would exhaust the region.
struct HostmemAllocator {
    next: u64,
    len: u64,
    /// Freed (offset, page-rounded size) extents, first-fit reuse. No coalescing
    /// at v1.0: ring blobs are uniform-ish (page-rounded, <= WARP_RING_MAX), so
    /// same-size frees exact-match without splitting and the list stays flat; a
    /// split extent's remainder is retained. A push that cannot grow the Vec
    /// leaks the extent (bump-only fallback) rather than aborting -- the offset
    /// is lost, never double-handed.
    free: alloc::vec::Vec<(u64, u64)>,
}

impl HostmemAllocator {
    fn new(region_len: u64) -> Self {
        Self { next: 0, len: region_len, free: alloc::vec::Vec::new() }
    }

    /// Reserve `size` bytes (rounded up to a page); returns the offset, or None
    /// when the region cannot fit it. Reuses a freed extent (first fit) before
    /// growing the region. Overflow-safe on every arithmetic step.
    fn alloc(&mut self, size: u64) -> Option<u64> {
        if size == 0 {
            // A zero-size reservation would not advance `next`, so it would alias
            // the next allocation -- refuse it rather than hand out a non-region.
            return None;
        }
        let size = size.checked_add(PAGE_SIZE - 1)? & !(PAGE_SIZE - 1);
        if let Some(i) = self.free.iter().position(|&(_, sz)| sz >= size) {
            let (off, sz) = self.free[i];
            if sz == size {
                self.free.swap_remove(i);
            } else {
                self.free[i] = (off + size, sz - size); // split: retain the remainder
            }
            return Some(off);
        }
        let off = self.next;
        let end = off.checked_add(size)?;
        if end > self.len {
            return None;
        }
        self.next = end;
        Some(off)
    }

    /// Return a page-aligned extent to the free-list. `size` MUST be the value
    /// passed to the `alloc` that returned `offset` (the caller carries it in the
    /// HostRing). Rejects (and logs) an extent that runs past the bump watermark
    /// or overlaps an already-freed extent: a double-free would else sit twice in
    /// the list and hand one offset to two live rings (holotype F1 defense-in-depth,
    /// behind the non-Copy handle). A legitimate free is always within `[0,next)`
    /// and disjoint from the list, so the guard never rejects one. A reserve
    /// failure leaks the extent rather than aborting.
    fn free(&mut self, offset: u64, size: u64) {
        let size = (size + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
        let end = offset.saturating_add(size);
        if end > self.next || self.free.iter().any(|&(o, s)| offset < o + s && o < end) {
            say!(
                "tapestryd: hostmem free rejected (offset={:#x} size={:#x}) -- oob/overlap, double-free?",
                offset, size
            );
            return;
        }
        if self.free.try_reserve(1).is_ok() {
            self.free.push((offset, size));
        }
    }
}

// V-3b-1b: virtio_gpu map_info cache types (VIRTIO_GPU_MAP_CACHE_*, section 5.7).
// map_blob returns one; the guest PTE must map at the MATCHING attribute
// (GPU-DESIGN 6.2 "honored exactly") or a mismatched host/guest alias loses
// coherency on ARM64. CACHED is the host-coherent pair on KVM.
const VIRTIO_GPU_MAP_CACHE_CACHED: u32 = 0x1;
const VIRTIO_GPU_MAP_CACHE_UNCACHED: u32 = 0x2;
const VIRTIO_GPU_MAP_CACHE_WC: u32 = 0x3;

/// Translate a `map_blob` `map_info` cache type into the `T_CACHE_*` the guest
/// PTE must use to MATCH the host's mapping. CACHED (0x1) and NONE/unknown map to
/// the host-coherent write-back default; never guess WC (the ARM64 mismatched-
/// alias hazard GPU-DESIGN 6.2 forbids). The `_CACHED` const names the 0x1 arm
/// the default already covers.
fn map_info_to_cache(map_info: u32) -> u64 {
    match map_info {
        VIRTIO_GPU_MAP_CACHE_CACHED => T_CACHE_CACHED,
        VIRTIO_GPU_MAP_CACHE_UNCACHED => T_CACHE_UNCACHED,
        VIRTIO_GPU_MAP_CACHE_WC => T_CACHE_WC,
        _ => T_CACHE_CACHED, // NONE / unknown -> host-coherent write-back
    }
}

/// A short name for a `T_CACHE_*` value, for the probe's log line.
fn cache_name(cache: u64) -> &'static str {
    match cache {
        T_CACHE_WC => "WC",
        T_CACHE_UNCACHED => "UNCACHED",
        _ => "CACHED",
    }
}

/// V-3b-1c: a minted HOST3D ring -- the engine handle the Model B ring path
/// (V-3b-1c-2) and the venus-stream forward (V-3b-2) will hold. Carries exactly
/// what `drop_host3d_ring` needs to retire it: the host resource, the hostmem
/// offset (to reclaim), tapestryd's guest VA + page-rounded size (to detach), and
/// the host-dictated cache attribute (logged; a guest/host mismatch is the
/// GPU-DESIGN 6.2 coherency hazard). Deliberately NOT `Copy` (holotype F1): a
/// ring handle is a single-use teardown token -- `drop_host3d_ring` consumes it
/// by value, so a double-drop (which would double-free the offset and alias two
/// live rings at one hostmem slot) and a use-after-drop are compile errors.
pub struct HostRing {
    pub res_id: u32,
    pub offset: u64,
    pub va: u64,
    pub size: u64,
    pub cache: u64,
}

/// Write an offset-derived sentinel through a guest-mapped hostmem VA and return
/// the written word. Offset-derived (holotype F2) so two rings carry DISTINCT
/// sentinels: if a host/kernel defect aliased their backings onto one PA, one
/// write clobbers the other, and the caller's re-read (after BOTH writes) then
/// mismatches -- witnessing PHYSICAL distinctness, not merely distinct allocator
/// offsets. The same-address, same-core write-then-read is barrier-free (ARM
/// coherency); a re-read mismatch means the VA does not hold what THIS ring wrote.
fn hostmem_sentinel(va: u64, off: u64) -> u32 {
    let sentinel = 0x5657_3342u32 ^ (off as u32); // "WV3B" ^ offset
    unsafe { w32(va, sentinel) };
    sentinel
}

// 3D commands (VIRTIO 1.2 section 5.7.6.9; virgl-negotiated only). The seam's
// context lifecycle + resource plumbing (Warp-2c); SUBMIT_3D + the transfers
// ride the fenced lane (Warp-2d).
const VIRTIO_GPU_CMD_CTX_CREATE: u32 = 0x0200;
const VIRTIO_GPU_CMD_CTX_DESTROY: u32 = 0x0201;
const VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE: u32 = 0x0202;
const VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE: u32 = 0x0203;
const VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: u32 = 0x0204;
const VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D: u32 = 0x0205;
const VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: u32 = 0x0206;
const VIRTIO_GPU_CMD_SUBMIT_3D: u32 = 0x0207;

// hdr.flags bit 0 (section 5.7.6.7): the device withholds this command's
// response until its fence signals, echoing flags + fence_id back -- so the
// virtqueue used-buffer notification IS the fence completion (GPU-DESIGN
// section 4.3: "we are labelling what we have", not building fence machinery).
const VIRTIO_GPU_FLAG_FENCE: u32 = 1 << 0;
// hdr.flags bit 1 (section 5.7.6.7): hdr.ring_idx (byte 20) is valid -- the
// fence rides that per-context timeline (virglrenderer: virgl_renderer_context_
// create_fence with ring_idx) instead of the device-global one. Venus binds a
// VkQueue to a timeline at vkGetDeviceQueue2 (.ringIdx), so a queue-attributed
// submission fences on the queue's own lane (the multi-queue F3 seam).
const VIRTIO_GPU_FLAG_INFO_RING_IDX: u32 = 1 << 1;

const VIRTIO_GPU_RESP_OK_NODATA: u32 = 0x1100;
const VIRTIO_GPU_RESP_OK_DISPLAY_INFO: u32 = 0x1101;
const VIRTIO_GPU_RESP_OK_CAPSET_INFO: u32 = 0x1102;
const VIRTIO_GPU_RESP_OK_CAPSET: u32 = 0x1103;
const VIRTIO_GPU_RESP_OK_MAP_INFO: u32 = 0x1106;
// The W-3a probe's discriminator set, pub so the server-side probe can read
// a raw resp_type verdict: INVALID_RESOURCE_ID on a bogus id proves the
// command was DISPATCHED and resolved (the vocabulary exists host-side);
// ERR_UNSPEC on the same leg is QEMU's unknown-command shape. Exported as
// GPU_RESP_* to keep the private VIRTIO_ set's visibility unchanged.
pub const GPU_RESP_OK_NODATA: u32 = VIRTIO_GPU_RESP_OK_NODATA;
pub const GPU_RESP_ERR_UNSPEC: u32 = 0x1200;
pub const GPU_RESP_ERR_INVALID_RESOURCE_ID: u32 = 0x1203;

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
/// The XRGB8 sibling (opaque alpha). W-3c-1: named because WARP-WSI-DESIGN
/// 4.1's stage-0 accept set is "BGRA8/XRGB8 -- the formats the console path
/// composes today", and the presentable registration must admit both.
pub const VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM: u32 = 2;

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

// Wall-clock bound on a FENCED chain before its slot is abandoned (audit
// F6). Not a deadline in the submit_and_wait sense -- it never declares
// the engine dead (a fence legitimately takes as long as its GL work, and
// a false dead-latch costs the console: #31). It bounds only OUR
// bookkeeping: 30 s is orders above any real GL job, so reaching it means
// the host renderer hung (the GPU-DESIGN section 9.2 accepted risk).
const FENCE_ABANDON_MS: u64 = 30_000;

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
// dedicated RESP page): page 1 holds both queues + the request region;
// the response region has its own page because the GET_CAPSET blob
// (virgl_caps_v2, ~1.2 KiB and growing upstream) outgrew the 0x300-byte
// slice page 1 had left. The Warp-2/3 winsys get_caps slot reads the
// same blob, so the headroom is a direct next consumer, not slack.
//
// The two queues have DIFFERENT sizes (#204): the controlq is 64 -- the
// QEMU device maximum, and the fenced lane's descriptor-pair budget
// (2 + 2*FENCED_SLOTS) is carved from it, so the controlq size is what
// ultimately bounds the submit pipeline depth. The cursorq stays 16
// because 16 IS its QEMU device maximum -- writing 64 there would fail
// negotiation, and nothing here submits to it anyway.
const QUEUE_SIZE: u16 = 64;
const CURSORQ_SIZE: u16 = 16;
pub const RING_DMA_SIZE: usize = 2 * PAGE_SIZE as usize;

const CTRL_DESC_OFF: u64 = 0x000;
const CTRL_AVAIL_OFF: u64 = 0x400;
const CTRL_USED_OFF: u64 = 0x500;
const CURSOR_DESC_OFF: u64 = 0x800;
const CURSOR_AVAIL_OFF: u64 = 0x900;
const CURSOR_USED_OFF: u64 = 0xA00;
const REQ_OFF: u64 = 0xB00;
const RESP_OFF: u64 = 0x1000;

/// The sync slot's request buffer: everything between the cursorq's used
/// ring and the response page. 0x100 -> 0x500 at Warp-C C-3: the compositor's
/// composition stream rides this slot (`submit_3d_sync`), and one present can
/// carry several VIRGL_CCMD_BLITs (22 dwords each) -- 0x100 admitted two. The
/// layout assert below is what makes the width safe: the buffer still ends
/// where the device-writable response region begins.
const REQ_REGION_LEN: u32 = 0x500;
const RESP_REGION_LEN: u32 = 0x1000;

const _: () = {
    assert!(CTRL_DESC_OFF + (QUEUE_SIZE as u64) * 16 <= CTRL_AVAIL_OFF);
    assert!(CTRL_AVAIL_OFF + 4 + (QUEUE_SIZE as u64) * 2 <= CTRL_USED_OFF);
    assert!(CTRL_USED_OFF + 4 + (QUEUE_SIZE as u64) * 8 <= CURSOR_DESC_OFF);
    assert!(CURSOR_DESC_OFF + (CURSORQ_SIZE as u64) * 16 <= CURSOR_AVAIL_OFF);
    assert!(CURSOR_AVAIL_OFF + 4 + (CURSORQ_SIZE as u64) * 2 <= CURSOR_USED_OFF);
    assert!(CURSOR_USED_OFF + 4 + (CURSORQ_SIZE as u64) * 8 <= REQ_OFF);
    assert!(REQ_OFF + (REQ_REGION_LEN as u64) <= RESP_OFF);
    assert!(RESP_OFF + (RESP_REGION_LEN as u64) <= RING_DMA_SIZE as u64);
    assert!(GPU_RESP_DISPLAY_INFO_LEN <= RESP_REGION_LEN);
};

// The fenced lane (Warp-2d): a SECOND DMA region for fence-bearing 3D chains
// (SUBMIT_3D + TRANSFER_*_3D), so the audited two-page sync ring above stays
// byte-identical. Slot i owns the fixed descriptor pair (2+2i, 3+2i), a
// FREQ_LEN request buffer (SUBMIT_3D carries its command stream inline;
// 64 KiB covers Mesa's VIRGL_MAX_CMDBUF, though the per-Twrite bound --
// msize, ~32 KiB -- is the effective ceiling until the Loom-carried path),
// and a GPU_CTRL_HDR_LEN response header in the tail page. A full lane
// refuses with Again (the client retries); it never blocks the serve loop
// (#31/#125: the compositor IS the console).
//
// 4 -> 16 at #204: the per-ctx share (FENCED_SLOTS / 2) was 2, and that
// depth-2 throttle -- faithfully mirrored client-side -- serialized every
// GL frame against full guest->host->retire round trips (the #215 ~300x
// per-draw collapse). 16 is the ceiling this layout admits: exactly one
// response page at FRESP_STRIDE (16 * 0x100 = PAGE_SIZE), and 2 + 2*16
// descriptor pairs fit the 64-deep controlq with headroom.
//
// FREQ_LEN shrank 64 KiB -> 36 KiB in the same change: the whole lane is
// ONE plain SYS_DMA_CREATE, whose kernel per-buffer cap is 1 MiB
// (KOBJ_DMA_MAX_SIZE), and 16 x 64 KiB + the response page overshot it by
// one page -- the allocation failed and the warden restart-looped a
// console-less tapestryd (caught by the GL-host capset gate; a 2D boot
// never allocates the lane, so the default suite is structurally blind to
// this). 36 KiB still swallows anything the byte seam can DELIVER -- one
// Twrite is one submission and msize bounds its payload at ~32 KiB
// (fenced_begin refuses larger cleanly); Mesa's 256 KiB VIRGL_MAX_CMDBUF
// only matters to the Loom bulk path (GPU-DESIGN 4.1), which will carry
// its own sizing.
pub const FENCED_SLOTS: usize = 16;
/// The one slot reserved for compositor-owned chains (Warp-C C-6, GPU-DESIGN
/// 4.5.13): the composed-GL present's readback rides it, so it never
/// competes with -- and can never be starved by -- the client pool, which is
/// the other FENCED_SLOTS - 1. Clients see one fewer slot; the per-ctx share
/// is unchanged. A compositor readback abandoned here poisons only this
/// slot, never a client's.
pub const COMP_FSLOT: usize = FENCED_SLOTS - 1;
const FREQ_LEN: u64 = 0x9000;
const FRESP_STRIDE: u64 = 0x100;
const FRESP_OFF: u64 = (FENCED_SLOTS as u64) * FREQ_LEN;
pub const FLANE_DMA_SIZE: usize = (FRESP_OFF + PAGE_SIZE) as usize;

const _: () = {
    assert!(2 + 2 * FENCED_SLOTS <= QUEUE_SIZE as usize);
    assert!((FENCED_SLOTS as u64) * FRESP_STRIDE <= PAGE_SIZE);
    assert!(FRESP_STRIDE >= GPU_CTRL_HDR_LEN as u64);
    // The reserved slot must leave a client pool (C-6): a lane of one would
    // give clients nothing, and the share below is derived from the WHOLE
    // lane, so it must still fit the client pool with room for a second
    // client (the round-5 F4 property).
    assert!(COMP_FSLOT >= 2 && COMP_FSLOT < FENCED_SLOTS);
    assert!(FENCED_SLOTS / 2 < COMP_FSLOT);
    // The lane is one PLAIN dma_create: stay under the kernel's 1 MiB
    // per-buffer cap (KOBJ_DMA_MAX_SIZE -- a literal here because the
    // kernel header is not visible to this crate; the runtime witness is
    // the GL-host boot, which fails loudly when this drifts).
    assert!(FLANE_DMA_SIZE <= 1024 * 1024);
    // A max-msize Twrite (one submission) must fit a slot with its
    // header: submit_3d stages GPU_CTRL_HDR_LEN + 8 + payload, and the
    // payload maxes at one Twrite = SRV_MSIZE - 23 (the Twrite envelope).
    // Derived from the seam constant, not a literal mirror of it (#204
    // audit F2): an SRV_MSIZE lift moves this floor with it instead of
    // sleeping through the drift.
    assert!(FREQ_LEN >= (crate::server::SRV_MSIZE as u64 - 23) + 8 + GPU_CTRL_HDR_LEN as u64);
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

fn setup_queue(
    common: u64,
    queue: u16,
    size: u16,
    desc_pa: u64,
    avail_pa: u64,
    used_pa: u64,
) -> Option<u16> {
    unsafe {
        w16(common + CCFG_QUEUE_SELECT, queue);
        if r16(common + CCFG_QUEUE_SIZE) < size {
            return None;
        }
        w16(common + CCFG_QUEUE_SIZE, size);
        w64(common + CCFG_QUEUE_DESC, desc_pa);
        w64(common + CCFG_QUEUE_DRIVER, avail_pa);
        w64(common + CCFG_QUEUE_DEVICE, used_pa);
        w16(common + CCFG_QUEUE_MSIX_VECTOR, VIRTIO_MSI_NO_VECTOR);
        let notify_off = r16(common + CCFG_QUEUE_NOTIFY_OFF);
        w16(common + CCFG_QUEUE_ENABLE, 1);
        Some(notify_off)
    }
}

/// What `init_device` negotiated, named rather than positional: a growing
/// tuple of bools is the shape that let V-0b's `ctxinit` go unreturned until
/// a build error caught it, and V-2 adds more feature bits here.
struct DevInit {
    /// The controlq doorbell VA (`notify_base + ctrl_off * notify_mul`).
    notify_va: u64,
    /// VIRTIO_GPU_F_VIRGL: the 3D command path exists.
    virgl: bool,
    /// VIRTIO_GPU_F_CONTEXT_INIT: `context_init` selects a ctx capset.
    ctxinit: bool,
    /// VIRTIO_GPU_F_RESOURCE_BLOB: RESOURCE_CREATE_BLOB is legal. Only then
    /// may a blob command go on the wire (the spec forbids it otherwise), so
    /// this both records the offer and gates the V-1 probe.
    blob: bool,
}

fn init_device(
    common: u64,
    notify_base: u64,
    notify_mul: u64,
    notify_len: u64,
    ring_pa: u64,
) -> Result<DevInit, Error> {
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

        // The device's offer, reported rather than discarded: it was read here
        // and dropped, so "does this host offer CONTEXT_INIT?" -- the question
        // that decides whether a Venus context is reachable at all -- had no
        // answer short of a new build. One line makes it a per-boot fact.
        say!(
            "tapestryd: gpu features lo=0x{:08x} (virgl={} edid={} uuid={} blob={} ctxinit={}) hi=0x{:08x}",
            dev_feat_lo,
            u32::from(dev_feat_lo & VIRTIO_GPU_F_VIRGL_BIT_LO != 0),
            u32::from(dev_feat_lo & VIRTIO_GPU_F_EDID_BIT_LO != 0),
            u32::from(dev_feat_lo & VIRTIO_GPU_F_RESOURCE_UUID_BIT_LO != 0),
            u32::from(dev_feat_lo & VIRTIO_GPU_F_RESOURCE_BLOB_BIT_LO != 0),
            u32::from(dev_feat_lo & VIRTIO_GPU_F_CONTEXT_INIT_BIT_LO != 0),
            dev_feat_hi
        );

        let virgl = dev_feat_lo & VIRTIO_GPU_F_VIRGL_BIT_LO != 0;
        // Accepted on the same accept-if-offered footing as virgl, because it
        // is the only way a context's capset can be selected -- the device
        // ignores `context_init` entirely without it, so a capset written
        // there would be silently discarded and the context would come back
        // implicitly-virgl with an OK response.
        //
        // Deliberately NOT a second gate on "is 3D available": that stays
        // `virgl` alone. The two are orthogonal, and a host could in principle
        // offer either without the other.
        let ctxinit = dev_feat_lo & VIRTIO_GPU_F_CONTEXT_INIT_BIT_LO != 0;
        // Accepted on the same accept-if-offered footing: a blob command is
        // illegal on the wire unless the feature is negotiated, so V-1's
        // guest-blob create both needs this and self-skips without it. It is
        // orthogonal to virgl and ctxinit -- a host may offer any subset.
        let blob = dev_feat_lo & VIRTIO_GPU_F_RESOURCE_BLOB_BIT_LO != 0;
        let mut want_lo = 0u32;
        if virgl {
            want_lo |= VIRTIO_GPU_F_VIRGL_BIT_LO;
        }
        if ctxinit {
            want_lo |= VIRTIO_GPU_F_CONTEXT_INIT_BIT_LO;
        }
        if blob {
            want_lo |= VIRTIO_GPU_F_RESOURCE_BLOB_BIT_LO;
        }
        w32(common + CCFG_DRIVER_FEATURE_SELECT, 0);
        w32(common + CCFG_DRIVER_FEATURE, want_lo);
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
            QUEUE_SIZE,
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
            CURSORQ_SIZE,
            ring_pa + CURSOR_DESC_OFF,
            ring_pa + CURSOR_AVAIL_OFF,
            ring_pa + CURSOR_USED_OFF,
        ) {
            Some(o) => o,
            None => {
                say!("tapestryd: gpu cursorq size below CURSORQ_SIZE");
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
        Ok(DevInit {
            notify_va: notify_base + u64::from(ctrl_off) * notify_mul,
            virgl,
            ctxinit,
            blob,
        })
    }
}

/// The owning ctx of an abandoned slot, kept so a LATE retire can tell the
/// seam its context is healthy again (round-3 F2: un-poisoning only the
/// descriptor pair left the ctx -- and its slot -- condemned forever).
#[derive(Clone, Copy)]
pub struct FenceVindication {
    pub ctx_pub: u32,
    /// ROUND F3 [P1] / main#242: TRUE when the late-retiring chain was the
    /// COMPOSITOR's own readback on the reserved slot. The completion arm
    /// already guards its dense `fence_signaled` bump on `!tag.comp`, but a
    /// vindication is produced AFTER the tag was taken by abandonment, so
    /// without this bit the seam credited the CLIENT with a fence it never
    /// issued -- and the tag carries the client's ctx (AS-BUILT 1), so the
    /// credit lands squarely on it. The winsys computes `issued - signaled`
    /// on unsigned counters and `warp_fence_wait` returns on
    /// `signaled >= seq`: one ahead, permanently, means every wait returns
    /// ONE FENCE EARLY for the ctx's life -- the client may reuse a buffer
    /// the GPU is still writing. Sourced structurally from the slot index,
    /// which is the only thing that survives the abandonment.
    pub comp: bool,
    /// The venus timeline the abandoned chain rode (multi-queue F3). Same
    /// production problem as `comp`: the vindication is minted AFTER the tag
    /// was taken by abandonment, so the lane must be RETAINED per-slot
    /// (`fslot_poison_ring`, exactly like the ctx id) or a vindicated fence
    /// bumps the ctx total but never its timeline -- `timeline_signaled[t]`
    /// one short forever, the per-timeline replay of the #210 silent
    /// post-recovery park.
    pub ring_idx: u8,
}

/// A retired fenced chain: the fence id + the owning seam context (pub id),
/// delivered to the server's fence pump. Attribution came from the used
/// ENTRY (the head descriptor names the slot), so no cross-context
/// signal-order assumption is load-bearing driver-side.
#[derive(Clone, Copy)]
pub struct FenceTag {
    pub fence_id: u64,
    /// The seam ctx whose RESOURCES this chain touches -- always the
    /// client's, even for a compositor-owned readback (`comp`): the
    /// abandonment bookkeeping (`fslot_poison_ctx`, `ctx_has_poisoned_slot`,
    /// the vindication) keys on this id, and a device write into a client
    /// BO that never retired must poison THAT ctx and hold up THAT ctx's
    /// vindication (round-4 F1: one late retire proves nothing about the
    /// rest). 0 is `warp_ctx_vindicate`'s no-slot sentinel and is never
    /// minted, so it can never be a marker here.
    pub ctx_pub: u32,
    /// TRANSFER_FROM_HOST_3D: the device READS the resource synchronously at
    /// processing time (Warp-C C-6, GPU-DESIGN 4.5.13), so while one is in
    /// flight the sync slot's stale-wake deadline is the fence bound, not
    /// the dead-device one -- a device stalled behind a legitimate readback
    /// is busy, and a false `dead` latch is the #31 loss.
    pub readback: bool,
    /// Compositor-owned (the composed-GL present's readback arm, C-6): rode
    /// the reserved slot; the fence pump routes it to
    /// `comp_readback_retired`, it is counted in the ctx's `fences_in_flight`
    /// (retire safety) but subtracted from admission, and its retire never
    /// bumps the client's `fence_signaled` (#210: the client counts fences it
    /// ISSUED).
    pub comp: bool,
    /// The device never retired this chain within FENCE_ABANDON_MS: the
    /// slot's bookkeeping was reclaimed so the engine stops counting it,
    /// but the chain may still be live device-side. NOT a completion --
    /// the owning ctx is poisoned (every later BO retire leaks rather
    /// than frees, since the device may still DMA the backing).
    pub abandoned: bool,
    /// ROUND F2 [P1]: the device's verdict on this chain. The pre-C-6b
    /// composed readback was SYNCHRONOUS and gated its compose on
    /// `transfer_from_3d_sync(...).is_ok()`; moving to the fenced lane
    /// dropped that gate on the floor, because `drain` logged a non-OK
    /// response type and pushed the tag anyway and the tag carried no
    /// status. `comp_readback_retired` then composed on an ERROR retire --
    /// painting whatever the backing held (zeros on a fresh BO: the pane
    /// blanks) and counting it `rb_landed`. False for an abandoned tag too:
    /// nothing was verified about a chain that never retired.
    pub ok: bool,
    /// The venus TIMELINE this fence rides (multi-queue F3): 0 = the
    /// ctx-global lane (every pre-multi-queue submission, transfers, the
    /// compositor readback); 1..=3 = a VkQueue's timeline (the submit
    /// carried INFO_RING_IDX). The seam's per-timeline `timeline_signaled`
    /// retires by this value -- server-minted, never client bytes, so it is
    /// always < WARP_TIMELINES by construction.
    pub ring_idx: u8,
}

/// Why a fenced submission was refused (mapped to a 9P errno at the seam).
pub enum FencedErr {
    /// Every fenced slot is in flight -- the client retries (E_AGAIN).
    /// Refuse-not-block: the serve loop must stay live (#31/#125).
    Again,
    /// The request exceeds the slot buffer (E_INVAL).
    TooBig,
    /// The engine is dead (latched) or the lane absent (E_IO).
    Dead,
}

struct Controlq {
    ring_va: u64,
    ring_pa: u64,
    notify_va: u64,
    isr_va: u64,
    irq: Irq,
    /// Count of avail entries published. Up to 1 + FENCED_SLOTS chains can
    /// be outstanding (Warp-2d), so completion is attributed by used-ENTRY
    /// id -- never inferred from this cursor (the single-in-flight `seq`
    /// check died with the fenced lane).
    avail_idx: u16,
    /// Count of used entries consumed.
    used_seen: u16,
    /// Latched on any submit/ring failure: after one, no ring cursor can
    /// be trusted against the device's consumption, so every later submit
    /// would read a freshly-zeroed response buffer as resp_type=0x0 (the
    /// #31 cascade). Fail fast + honestly instead.
    dead: bool,
    /// The fenced lane (virgl only; 0 = absent): slot i owns descriptor
    /// pair (2+2i, 3+2i), its request buffer at flane_va + i*FREQ_LEN and
    /// its response header at flane_va + FRESP_OFF + i*FRESP_STRIDE.
    flane_va: u64,
    flane_pa: u64,
    fslots: [Option<FenceTag>; FENCED_SLOTS],
    /// When each occupied slot was published -- the abandonment clock
    /// (audit F6). Without it a fence that never signals (a hung host GL
    /// job, the GPU-DESIGN section 9.2 accepted risk) pins the slot
    /// forever, and with it the serve loop's 1 ms poll clamp: an
    /// unbounded ~1 kHz spin in the console renderer.
    fslot_since: [Option<Instant>; FENCED_SLOTS],
    /// An abandoned slot's descriptors may still be written by the device,
    /// so the pair is never re-used -- retired from the pool, not freed.
    fslot_poisoned: [bool; FENCED_SLOTS],
    /// The abandoned chain's venus timeline, retained per-slot like the ctx
    /// id below: a late retire mints its `FenceVindication` after the tag is
    /// gone, and the per-timeline count needs the lane (see
    /// `FenceVindication.ring_idx`).
    fslot_poison_ring: [u8; FENCED_SLOTS],
    /// Which ctx each poisoned slot belonged to, so its late retire can
    /// vindicate that ctx (round-3 F2).
    fslot_poison_ctx: [u32; FENCED_SLOTS],
    /// Contexts whose abandoned chain the device later retired: proof the
    /// host is finished with them, so the seam may un-poison.
    vindicated: alloc::vec::Vec<FenceVindication>,
    /// A slot-0 (sync) chain is published and unretired. Only
    /// submit_and_wait sets it, and it never returns success with it set
    /// -- drain() treats an id-0 entry outside that window as corruption.
    sync_pending: bool,
    /// Fence completions drained but not yet taken by the server pump.
    completed: alloc::vec::Vec<FenceTag>,
    /// #175: hold the completion drain so a submitted fence STAYS in
    /// flight. Without it a prover that submits and then abandons races
    /// the serve loop -- the completion normally lands first, so nothing
    /// is in flight, the abandon is a no-op, and every assertion after it
    /// runs against a HEALTHY ctx while the harness reports PASS. Held
    /// completions are not dropped: clearing the hold drains them and the
    /// real vindication follows, which is what makes that leg testable.
    ///
    /// #177: gating `poll_completions` alone was NOT enough, and the first
    /// red run proved it. `submit_and_wait` calls `drain()` directly, so
    /// every synchronous controlq command (and `create3d` issues four) is
    /// its own un-gated drain. The abandoned chain's late retire therefore
    /// landed on the churn loop's FIRST command, vindicated the ctx, and
    /// the assertions ran against a healed ctx -- a lever that held the
    /// window before the trigger and none of the window after it.
    ///
    /// #178: and it is scoped to ONE ctx, not global. `default =
    /// ["test-mode"]` with no build passing --no-default-features means
    /// this ships, and `/srv/warp` `ctl` is mode 0666 -- so a global hold
    /// let any client stop every other client's fences forever. Identity
    /// cannot separate the prover from an attacker (the in-guest battery
    /// is an ordinary uid-1000 client BY DESIGN -- the same reason SA-1
    /// leaves the determinism surface ungated), so the fix is to make the
    /// POWER proportionate rather than to gate the caller: a client may
    /// hold only its own ctx's fences, which it could already stall by
    /// simply not reading them.
    #[cfg(feature = "test-mode")]
    hold_ctx: Option<u32>,
    /// #177/#178: slots whose used-ring retire was swallowed while their
    /// ctx was held. DEFERRED, never dropped -- the healing leg asserts
    /// the vindication ARRIVES on release, so discarding here would trade
    /// a vacuous wedge for a vindication that could never come. One list
    /// covers both orders: at release the slot either still holds a live
    /// tag (retire seen before the abandon) or is poisoned with the tag
    /// taken (retire seen after it), which is the same two-way branch
    /// `drain` itself makes.
    #[cfg(feature = "test-mode")]
    held_retires: alloc::vec::Vec<usize>,
    /// #175: slots actually abandoned, so the prover can assert its
    /// trigger BIT before trusting anything downstream of it.
    #[cfg(feature = "test-mode")]
    abandoned_total: u32,
}

impl Controlq {
    /// W-4 stall observability: every HELD fenced slot as
    /// (slot, fence_id, ctx_pub, ring_idx, readback, comp, age_ms) -- the
    /// ctl prints one row per entry so a post-mortem read NAMES the op
    /// class a wedged chain belongs to (run 6's whole evidence was one
    /// anonymous missing count: fenced-free 15).
    fn fenced_held(&self) -> alloc::vec::Vec<(usize, u64, u32, u8, bool, bool, u64)> {
        self.fslots
            .iter()
            .enumerate()
            .filter_map(|(i, t)| {
                t.as_ref().map(|t| {
                    let age = self.fslot_since[i]
                        .map(|s| s.elapsed().as_millis() as u64)
                        .unwrap_or(0);
                    (i, t.fence_id, t.ctx_pub, t.ring_idx, t.readback, t.comp, age)
                })
            })
            .collect()
    }

    fn fenced_in_flight(&self) -> u32 {
        self.fslots.iter().flatten().count() as u32
    }

    /// A CLIENT chain's slot: first-fit over the client pool, which is every
    /// slot but the reserved one (Warp-C C-6). Two clients at their share
    /// (`WARP_CTX_FENCE_MAX` = FENCED_SLOTS / 2) fill the whole lane, so the
    /// compositor's own readback must not compete here; the shares are
    /// unchanged and the second client's last chain simply refuses Again.
    fn alloc_fenced_slot(&self) -> Option<usize> {
        (0..COMP_FSLOT).find(|&i| self.fslots[i].is_none() && !self.fslot_poisoned[i])
    }

    /// The reserved slot for compositor-owned chains (C-6): free and not
    /// poisoned, or None. A poisoned reserved slot comes back through the
    /// same late-retire vindication as any other; until then the readback
    /// arm parks its surfaces on stale frames rather than borrowing a
    /// client slot (a compositor chain abandoned in the client pool would
    /// shrink the lane for every client).
    fn alloc_comp_slot(&self) -> Option<usize> {
        if self.fslots[COMP_FSLOT].is_none() && !self.fslot_poisoned[COMP_FSLOT] {
            Some(COMP_FSLOT)
        } else {
            None
        }
    }

    /// Every CLIENT slot poisoned and none in flight: nothing will ever free
    /// one, so a client must be told to STOP retrying (round-2 F6). Over the
    /// client pool only -- a healthy reserved slot is not one a client can
    /// ever be handed, so counting it would keep clients retrying forever
    /// against a pool that is in fact exhausted.
    fn lane_exhausted(&self) -> bool {
        (0..COMP_FSLOT).all(|i| self.fslot_poisoned[i] && self.fslots[i].is_none())
    }

    /// Any in-flight fenced chain that is a READBACK (a client's
    /// `transfer_3d(to_host = false)` or the compositor's own): the device
    /// executes it synchronously at processing time and every sync step
    /// queued behind it inherits that wait (C-6, consequence 2).
    fn readback_in_flight(&self) -> bool {
        self.fslots.iter().flatten().any(|t| t.readback)
    }

    /// Reclaim slots whose chains never retired (audit F6). Bounds the
    /// spin + the slot pool WITHOUT latching `dead` (a slow device is not
    /// a dead one -- the #31 rule) and WITHOUT freeing anything the device
    /// may still touch: the descriptor pair is poisoned, and the owning
    /// ctx is told, so its backings leak rather than free.
    fn reap_abandoned(&mut self) {
        for i in 0..FENCED_SLOTS {
            let stale = match (self.fslots[i], self.fslot_since[i]) {
                (Some(_), Some(t0)) => t0.elapsed().as_millis() as u64 >= FENCE_ABANDON_MS,
                _ => false,
            };
            if !stale {
                continue;
            }
            let mut tag = self.fslots[i].take().unwrap();
            self.fslot_since[i] = None;
            self.fslot_poisoned[i] = true;
            self.fslot_poison_ctx[i] = tag.ctx_pub;
            self.fslot_poison_ring[i] = tag.ring_idx;
            tag.abandoned = true;
            say!(
                "tapestryd: gpu fence {} never retired in {} ms -- slot {} retired, ctx {} poisoned",
                tag.fence_id, FENCE_ABANDON_MS, i, tag.ctx_pub
            );
            self.completed.push(tag);
        }
    }

    /// Publish one descriptor chain: avail entry + idx bump + doorbell
    /// (VIRTIO 1.2 2.7.13.1: descriptor + ring writes visible before the
    /// idx bump).
    fn publish(&mut self, head: u16) {
        let avail_va = self.ring_va + CTRL_AVAIL_OFF;
        let slot = (self.avail_idx % QUEUE_SIZE) as u64;
        unsafe {
            w16(avail_va + 0, 0);
            w16(avail_va + 4 + slot * 2, head);
        };
        dsb_sy();
        let next = self.avail_idx.wrapping_add(1);
        unsafe { w16(avail_va + 2, next) };
        dsb_sy();
        self.avail_idx = next;
        unsafe { w16(self.notify_va, GPU_QUEUE_CONTROL) };
    }

    /// Consume every used entry the device has published, attributing each
    /// by its ENTRY id -- the head descriptor of the retired chain (the
    /// Warp-2d replacement for the single-in-flight used.idx==seq
    /// inference). Fence completions accumulate on `completed` (the server
    /// pump takes them); returns true if the pending slot-0 sync chain
    /// retired. An entry naming nothing in flight latches `dead`: the
    /// device retired a chain we never published, so no cursor is
    /// trustworthy.
    fn drain(&mut self) -> bool {
        let used_va = self.ring_va + CTRL_USED_OFF;
        let mut sync_retired = false;
        loop {
            virtio_rmb();
            let used_idx = unsafe { r16(used_va + 2) };
            if used_idx == self.used_seen {
                return sync_retired;
            }
            let outstanding = self.avail_idx.wrapping_sub(self.used_seen);
            if used_idx.wrapping_sub(self.used_seen) > outstanding {
                say!(
                    "tapestryd: gpu used.idx {} past published {} (ring corrupt)",
                    used_idx, self.avail_idx
                );
                self.dead = true;
                return sync_retired;
            }
            // VIRTIO 1.2 2.7.13.2: order the used.idx observation before
            // the entry read.
            virtio_rmb();
            let entry_va = used_va + 4 + ((self.used_seen % QUEUE_SIZE) as u64) * 8;
            let id = unsafe { r32(entry_va) };
            self.used_seen = self.used_seen.wrapping_add(1);
            if id == 0 {
                if !self.sync_pending {
                    say!("tapestryd: gpu used entry id 0 with no sync chain (ring corrupt)");
                    self.dead = true;
                    return sync_retired;
                }
                self.sync_pending = false;
                sync_retired = true;
                continue;
            }
            let slot = if id >= 2 && id % 2 == 0 {
                ((id - 2) / 2) as usize
            } else {
                FENCED_SLOTS // odd / resp-desc id: never a chain head
            };
            // #177/#178: while THIS slot's ctx is held, swallow the retire
            // -- that is exactly what models "the device has not proved it
            // finished". The used entry is still consumed above, so the
            // sync chain behind it retires normally and other clients are
            // untouched; the tag is deliberately NOT taken, so the fence
            // stays in flight and `abandon` can still find it.
            // ROUND F4 [P1]: the reserved compositor slot is EXEMPT from the
            // harness hold. The lever matches on the tag's ctx, and our
            // readback's tag carries the CLIENT's ctx (AS-BUILT 1) -- so a
            // client holding its OWN ctx used to pin `COMP_FSLOT` too, which
            // freezes readback composition for EVERY client, pins
            // `readback_in_flight()` (disabling the 500 ms sync deadline
            // process-wide) and pins `warp_fences_pending()` (a ~1 kHz spin in
            // the console). #178's bound -- "the worst a client can do is wedge
            // its own ctx" -- was written when "your ctx's fences" meant only
            // your own; C-6b made that false one resource over.
            #[cfg(feature = "test-mode")]
            if slot < FENCED_SLOTS && slot != COMP_FSLOT && self.hold_ctx.is_some() {
                let held = self.fslots[slot]
                    .as_ref()
                    .map(|t| t.ctx_pub)
                    .unwrap_or(self.fslot_poison_ctx[slot]);
                if self.hold_ctx == Some(held) {
                    self.held_retires.push(slot);
                    continue;
                }
            }
            // ROUND F4, the second half: clear the staleness anchor only AFTER
            // the hold branch. It used to be cleared one line above it, so a
            // held slot had `fslot_since == None` and `reap_abandoned`'s
            // `(Some(_), Some(t0))` test could never fire on it -- the pin was
            // INDEFINITE rather than bounded by the 30 s deadline. A swallowed
            // retire must leave the slot exactly as it was.
            if slot < FENCED_SLOTS {
                self.fslot_since[slot] = None;
            }
            let mut tag = match self.fslots.get_mut(slot).and_then(|s| s.take()) {
                Some(t) => t,
                None => {
                    // A poisoned slot retiring LATE is expected, not
                    // corruption: we abandoned its bookkeeping, the device
                    // finished anyway. Consume it silently -- the slot
                    // stays out of the pool (its response buffer is no
                    // longer trusted to belong to any live chain).
                    if slot < FENCED_SLOTS && self.fslot_poisoned[slot] {
                        // The device just PROVED it is finished with this
                        // descriptor pair and response buffer, so the slot
                        // is safe to reuse -- return it to the pool
                        // (round-2 F6: leaving it poisoned meant four
                        // slow fences permanently killed the lane for
                        // every client, reported as a retryable E_AGAIN).
                        self.fslot_poisoned[slot] = false;
                        // Tell the seam too (round-3 F2): the device has
                        // proved it is done with this chain, so the ctx's
                        // backings are no longer at risk and its slot need
                        // not stay condemned.
                        self.vindicated.push(FenceVindication {
                            ctx_pub: self.fslot_poison_ctx[slot],
                            comp: slot == COMP_FSLOT,
                            ring_idx: self.fslot_poison_ring[slot],
                        });
                        continue;
                    }
                    say!(
                        "tapestryd: gpu used entry id {} names no in-flight chain (ring corrupt)",
                        id
                    );
                    self.dead = true;
                    return sync_retired;
                }
            };
            // A FENCE-flagged response is withheld until the fence
            // signals, so retirement IS the completion. An ERROR response
            // still retires the fence -- nothing further will run, and a
            // client waiting on a fence that can never signal is the
            // wedge this avoids. Logged, not withheld.
            let resp_va = self.flane_va + FRESP_OFF + (slot as u64) * FRESP_STRIDE;
            let rt = unsafe { r32(resp_va) };
            // ROUND F2 [P1]: record the verdict, do not merely narrate it.
            // A compositor readback that the device REFUSED must not be
            // composed from (see `FenceTag.ok`); a client's own chain still
            // retires either way -- withholding the completion would wedge a
            // client waiting on a fence that can never signal.
            tag.ok = rt == VIRTIO_GPU_RESP_OK_NODATA;
            if !tag.ok {
                say!(
                    "tapestryd: gpu fenced cmd (fence {}) resp_type={:#x}",
                    tag.fence_id, rt
                );
            }
            self.completed.push(tag);
        }
    }

    /// The serve loop's non-blocking completion pump. Nothing to do unless
    /// fenced work is in flight (a sync chain never outlives its own
    /// dispatch). The ISR read is level hygiene: nobody irq.waits between
    /// dispatches, so the assert would otherwise sit latched.
    fn poll_completions(&mut self) {
        // #178: NO global early return for the harness hold. It used to
        // bail here, which stopped EVERY client's drain -- an unprivileged
        // box-wide DoS on a mode-0666 ctl. The hold is enforced per-slot
        // in `drain` instead, so a held ctx's fences stay in flight while
        // everyone else's retire normally.
        //
        // Poisoned-but-idle still needs draining (round-3 F3): abandonment
        // TAKES the tag, so `fenced_in_flight()` is 0 the moment the last
        // slot is abandoned -- and the un-poison lives in drain()'s
        // late-retire arm, which this early return then made unreachable.
        // Poison is state that OUTLIVES in-flight-ness, so it cannot be
        // the guard's only question.
        let poisoned_any = self.fslot_poisoned.iter().any(|&p| p);
        if self.fenced_in_flight() == 0 && !poisoned_any {
            return;
        }
        if self.dead {
            // A dead engine will never retire anything, so the slots it
            // still holds must be released as ABANDONED right now
            // (round-2 F5): otherwise every ctx that owned one keeps a
            // nonzero fence count forever and the deferred-retire pump
            // strands its handles, mappings and pages for the process
            // lifetime. Abandoned (not completed) is the honest tag --
            // the device may still be mid-DMA, so the backings leak.
            self.abandon_all("engine dead");
            return;
        }
        let _ = unsafe { r8(self.isr_va) };
        let _ = self.drain();
        self.reap_abandoned();
    }

    /// Release every occupied fenced slot as abandoned (poisoning both the
    /// slot and, via the tag, its owning ctx).
    fn abandon_all(&mut self, why: &str) {
        self.abandon_matching(why, None)
    }

    /// The same release, optionally restricted to ONE ctx (#178). The
    /// deadline and dead-engine callers pass `None` -- a wedge that real
    /// is genuinely global. The harness verb passes its own ctx, so a
    /// client can only abandon chains it owns.
    fn abandon_matching(&mut self, why: &str, only_ctx: Option<u32>) {
        for i in 0..FENCED_SLOTS {
            // ROUND F4 [P1]: a SCOPED abandon is a client-driven lever, and our
            // readback's tag carries the client's ctx -- so without this the
            // caller could poison the shared reserved slot on demand. The
            // UNSCOPED callers (the deadline, a dead engine) still reach it:
            // a wedge that real is genuinely global.
            if only_ctx.is_some() && i == COMP_FSLOT {
                continue;
            }
            if let Some(want) = only_ctx {
                if self.fslots[i].as_ref().map(|t| t.ctx_pub) != Some(want) {
                    continue;
                }
            }
            let mut tag = match self.fslots[i].take() {
                Some(t) => t,
                None => continue,
            };
            self.fslot_since[i] = None;
            self.fslot_poisoned[i] = true;
            self.fslot_poison_ctx[i] = tag.ctx_pub;
            self.fslot_poison_ring[i] = tag.ring_idx;
            tag.abandoned = true;
            #[cfg(feature = "test-mode")]
            {
                self.abandoned_total = self.abandoned_total.saturating_add(1);
            }
            say!(
                "tapestryd: gpu fence {} released ({}) -- slot {} retired, ctx {} poisoned",
                tag.fence_id, why, i, tag.ctx_pub
            );
            self.completed.push(tag);
        }
    }

    /// Publish a fenced chain on slot `slot` (request already staged in
    /// the slot's flane buffer) and return WITHOUT waiting: its retirement
    /// arrives via drain()'s entry attribution.
    fn submit_fenced(&mut self, slot: usize, req_len: u32, tag: FenceTag) -> Result<(), ()> {
        if self.dead || self.flane_va == 0 || self.fslots[slot].is_some() {
            return Err(());
        }
        let resp_off = FRESP_OFF + (slot as u64) * FRESP_STRIDE;
        for i in 0..(GPU_CTRL_HDR_LEN as u64) {
            unsafe { w8(self.flane_va + resp_off + i, 0) };
        }
        let d = (2 + 2 * slot) as u64;
        let desc_va = self.ring_va + CTRL_DESC_OFF + d * 16;
        unsafe {
            w64(desc_va + 0, self.flane_pa + (slot as u64) * FREQ_LEN);
            w32(desc_va + 8, req_len);
            w16(desc_va + 12, VIRTQ_DESC_F_NEXT);
            w16(desc_va + 14, (d + 1) as u16);

            w64(desc_va + 16, self.flane_pa + resp_off);
            w32(desc_va + 24, GPU_CTRL_HDR_LEN);
            w16(desc_va + 28, VIRTQ_DESC_F_WRITE);
            w16(desc_va + 30, 0);
        };
        self.fslots[slot] = Some(tag);
        self.fslot_since[slot] = Some(Instant::now());
        self.publish(d as u16);
        Ok(())
    }

    /// Submit the slot-0 (sync) chain and wait for ITS retirement, draining
    /// any fenced completions that arrive first. The G-5 wait discipline is
    /// unchanged -- the used ring is the only completion authority (a wake
    /// proves only that SOME notification-ish event reached us: irqfwd
    /// collapses INTx edges, a level re-fire or a config event can latch a
    /// stale pending event, and under a live display backend (#31) such a
    /// mis-timed wake is routine), the ISR read is level hygiene per wake,
    /// the spin-poll nets the used.idx store-propagation window, and the
    /// wall-clock deadline bounds EVENT-ful non-progress. The deadline
    /// applies to THIS chain only -- fenced chains have none (a fence
    /// legitimately takes as long as its GL work) -- and stays honest under
    /// a fence backlog of SUBMITs: the device writes a non-fenced response
    /// at PROCESSING time, and controlq processing is in-order, so pending
    /// fenced submits ahead of this chain cannot delay its retirement --
    /// their processing is a decode.
    ///
    /// A pending fenced READBACK is the exception, and the claim above is
    /// false for it (Warp-C C-6, GPU-DESIGN 4.5.13; F2b): QEMU processes the
    /// controlq inline on its main loop and virglrenderer executes a
    /// TRANSFER_FROM_HOST_3D synchronously at decode (`glMapBufferRange` /
    /// `glReadPixels` on the resource's context -- a GL wait for every job
    /// that writes the resource), so a sync step queued behind a readback of
    /// a BUSY resource inherits that wait, for as long as the resource's
    /// owner has queued ahead of it. That device is busy, not dead, and the
    /// 500 ms deadline latching `dead` on it is exactly the #31 loss it
    /// exists to prevent. So while ANY readback is in flight -- a client's
    /// `transfer_3d(to_host = false)` or the compositor's own -- the deadline
    /// is FENCE_ABANDON_MS, the bound every fenced chain already carries.
    /// STICKY for this wait once observed: the readback that caused a stall
    /// retires in the drain that finally moves used.idx, and this chain's own
    /// entry follows it in the same drain or the next -- re-narrowing the
    /// bound the instant the readback is gone would fail the very wait it
    /// widened.
    fn submit_and_wait(&mut self, req_len: u32, resp_len: u32) -> Result<u32, ()> {
        if self.dead {
            return Err(());
        }
        let mut deadline_ms = if self.readback_in_flight() {
            FENCE_ABANDON_MS
        } else {
            SUBMIT_DEADLINE_MS
        };
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
        self.sync_pending = true;
        self.publish(0);

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
            if self.drain() {
                break 'wait;
            }
            if self.dead {
                return Err(());
            }
            // Not ours yet. If this wake WAS our completion's notification
            // with the used.idx store still propagating, no further
            // interrupt is coming -- spin-poll briefly before re-waiting.
            // A fence retiring during the spin is progress but not ours:
            // drain it (bounded by the slot count) and keep spinning.
            let mut spins = 0u32;
            while spins < USED_SPIN_PER_WAKE {
                virtio_rmb();
                if unsafe { r16(used_va + 2) } != self.used_seen {
                    if self.drain() {
                        break 'wait;
                    }
                    if self.dead {
                        return Err(());
                    }
                    continue;
                }
                spins += 1;
            }
            wakes += 1;
            if self.readback_in_flight() {
                deadline_ms = FENCE_ABANDON_MS;
            }
            let t0 = *stale_since.get_or_insert_with(Instant::now);
            if t0.elapsed().as_millis() as u64 >= deadline_ms {
                say!(
                    "tapestryd: gpu command never retired ({} stale wakes over {} ms)",
                    wakes, deadline_ms
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

        // VIRTIO 1.2 2.7.13.2: order the used-entry consumption before the
        // response-buffer read.
        virtio_rmb();
        let resp_type = unsafe { r32(self.ring_va + RESP_OFF) };
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

// The fenced ctx header (Warp-2d): FLAG_FENCE + the fence id the device
// echoes in the withheld response.
unsafe fn write_ctrl_hdr_fenced(req_va: u64, cmd_type: u32, ctx_id: u32, fence_id: u64) {
    write_ctrl_hdr_ctx(req_va, cmd_type, ctx_id);
    w32(req_va + 4, VIRTIO_GPU_FLAG_FENCE);
    w64(req_va + 8, fence_id);
}

/// Byte-copy a client stream into a flane request buffer (volatile stores;
/// 8-byte bulk + byte tail; both sides little-endian, so the unaligned
/// native read preserves byte order).
fn copy_stream(dst_va: u64, src: &[u8]) {
    let mut i = 0usize;
    while i + 8 <= src.len() {
        let v = unsafe { core::ptr::read_unaligned(src.as_ptr().add(i) as *const u64) };
        unsafe { w64(dst_va + i as u64, v) };
        i += 8;
    }
    while i < src.len() {
        unsafe { w8(dst_va + i as u64, src[i]) };
        i += 1;
    }
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
    /// FEATURES_OK'd `VIRTIO_GPU_F_CONTEXT_INIT`. Gates capset-selected
    /// context creation: without it the device ignores `context_init`, so
    /// attempting a capset there yields a success response over a context
    /// that is not the requested kind.
    pub ctxinit: bool,
    /// FEATURES_OK'd `VIRTIO_GPU_F_RESOURCE_BLOB`. Gates the V-1 guest-blob
    /// path -- a blob command is illegal on the wire without it.
    pub blob: bool,
    /// W-3c-1: a monotonic tick stamped by the two commands whose RELATIVE
    /// ORDER is the display-safe teardown's whole invariant -- `set_scanout`
    /// (the unbind) and `resource_unref`. It exists because the self-test's
    /// "ordering witness" could not, in fact, witness an ordering (audit F5):
    /// checking `bound_res == 0` after the destroy catches the unbind being
    /// OMITTED, but an INVERTED teardown (unref, then unbind) clears
    /// `bound_res` just the same and read as a pass -- and inversion is the
    /// display-fatal arrangement the ordering exists to forbid. A per-command
    /// tick makes the order itself observable, so the arm can assert what its
    /// name claims. Counts commands ISSUED (before the response), which is
    /// the order the device sees them in.
    ///
    /// SCOPE, because the sentence above invites over-reading (round-2 F15):
    /// only `set_scanout` and `resource_unref` tick this. It is NOT a global
    /// command counter -- notably `set_scanout_blob`, the OTHER command that
    /// changes what the display names, does not participate. Anyone building
    /// a new ordering witness must add the tick to the commands it means to
    /// order rather than assume this already counts them. And a tick proves
    /// ISSUE, never ACCEPTANCE: a refused command ticks exactly like an
    /// accepted one, which is precisely how round-2 F1 got its `unbind=ok`.
    pub cmd_seq: u64,
    /// The tick at which the last `set_scanout` / `resource_unref` was issued.
    pub last_scanout_seq: u64,
    pub last_unref_seq: u64,
    /// Resources the DEVICE still scans because it REFUSED to unbind them
    /// (W-3c-1 round-2 F3). `resource_unref` DEFERS on these rather than
    /// freeing memory the display is actively reading -- the I-40
    /// `punbind_skipped` state. The next ACCEPTED scanout means the display
    /// no longer names any of them, and drains the list for real.
    ///
    /// Centralised here, not at the call sites, deliberately: round-1 F8
    /// exposed the unbind verdict and round 2 found two of three callers
    /// still dropping it. A guard every unref path passes through cannot be
    /// forgotten by the next caller added.
    condemned: [Condemned; GPU_CONDEMNED_MAX],
    condemned_n: usize,
    /// Sticky: the park list overflowed, so EVERY unref defers until the next
    /// drain. Over-deferral is the safe direction (round-3 F2).
    condemned_overflowed: bool,
    /// Self-test lever: fail the next display disable without issuing it.
    fail_next_scanout_disable: bool,
    /// Set when the lever (not the device) produced the last refusal, so the
    /// report can SAY it was injected. The code path stays identical -- that
    /// is what makes the drill valid -- but the LOG must distinguish, because
    /// the gate keys on a real refusal being absent and the self-test now
    /// deliberately produces one every boot.
    injected_refusal: bool,
    /// Condemnations dropped because the list was full. These leak for the
    /// process's life -- the safe direction (a leaked resource id costs
    /// memory; a freed one the display still scans is a UAF).
    pub condemned_lost: u32,
    pub num_capsets: u32,
    /// The fetched preferred capset (Warp-2c: served verbatim by the
    /// /dev/warp caps file; empty when no capset was fetchable). The id +
    /// version ride the warp ctl line so a client can decode the blob.
    pub capset_blob: alloc::vec::Vec<u8>,
    pub capset_id: u32,
    pub capset_ver: u32,
    /// V-3b-3 (Model B): the VENUS capset (id=4), fetched + retained
    /// SEPARATELY from the ranked virgl capset above -- the Mesa vn_renderer
    /// backend's instance gate reads the venus wire-format / vk-xml versions
    /// from it, and a virgl capset's bytes in those fields stub the instance
    /// (device-less). Served by the `caps-venus` file; empty on a non-venus
    /// host. The id is implicitly VIRTIO_GPU_CAPSET_VENUS; the version rides
    /// the blob itself (the backend reads it from the wire), so it is not
    /// separately retained -- V-3c adds a ver/id record if the authority half
    /// needs one.
    pub venus_capset_blob: alloc::vec::Vec<u8>,
    /// Global monotone fence ids (Warp-2d). Global, not per-ctx: QEMU's
    /// virgl fence walk retires every queued fence with id <= the signaled
    /// value, so independent per-ctx sequences would cross-release each
    /// other's fences.
    fence_next: u64,
    pci: PciDev,
    /// V-3b-1c: the persistent hostmem-BAR ring-offset allocator (None on a
    /// device with no hostmem shm region). Outlives any single ring so the
    /// Model B ring path can mint + retire across a client's session.
    hostmem: Option<HostmemAllocator>,
    /// V-3b-1c-2b F2: HOST3D rings retired while a client still references the
    /// GPA. The host bytes (the QEMU subregion) live OUTSIDE the kernel #847
    /// count, so reclaiming the offset here while a client's weft map -- or a
    /// claimed-but-not-yet-mapped client's transferred pin -- is live would
    /// re-hand it under the client's PTEs (a cross-client alias). A ring whose
    /// total ref count is still > 1 (any client ref beyond tapestryd's own map)
    /// is parked here -- tapestryd KEEPS its own VA mapped so `t_hostmem_refcount`
    /// can re-query it -- and reaped by `reap_hostmem_parked` once the count drops
    /// to 1. Bounded: a client that never unmaps pins only its own I-32 budget.
    hostmem_parked: alloc::vec::Vec<HostRing>,
    /// V-3b-2 xproc-E2E: the host3d-ring reap ledger. `park` counts retires that
    /// PARKED (a client ref beyond tapestryd's own map kept the offset live);
    /// `reap` counts parked rings later RECLAIMED (the client released, so the
    /// total ref count dropped to 1). The cross-Proc lifecycle E2E reads both via
    /// the warp ctl to witness park-on-mapped-retire -> reclaim-on-release with a
    /// REAL client refcount. A real leak-shape ledger like `warp_probe_parked`
    /// (readable on production, not test-mode): a park that never reaps is the
    /// shape of a client that never released, bounded by its I-32 budget.
    hostmem_park_count: u64,
    hostmem_reap_count: u64,
    _ring: Dma,
    _flane: Option<Dma>,
}

impl Gpu {
    /// Claim + bring up the virtio-gpu PCI function: transport handshake,
    /// both queues, then GET_DISPLAY_INFO for the scanout geometry.
    /// `flane_va` is the fenced lane's mapping slot -- allocated only when
    /// VIRGL negotiates (a 2D device never sees a fence-bearing command;
    /// the seam answers E_OPNOTSUPP), so plain boots pay nothing (Warp-2d).
    pub fn probe(
        bar_window_va: u64,
        ring_va: u64,
        flane_va: u64,
        blob_probe_va: u64,
    ) -> Result<Gpu, Error> {
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

        let DevInit {
            notify_va,
            virgl,
            ctxinit,
            blob,
        } = init_device(common_va, notify_base, notify_mul, u64::from(notify_len), ring_pa)?;

        let flane = if virgl {
            let f = unsafe { Dma::new(FLANE_DMA_SIZE, rw_map, flane_va, prot) }.map_err(|_| {
                say!("tapestryd: SYS_DMA_CREATE(gpu fenced lane) failed");
                Error::Hardware
            })?;
            prewarm(f.base_va() as u64, FLANE_DMA_SIZE);
            Some(f)
        } else {
            None
        };
        let flane_pa = flane.as_ref().map_or(0, |f| f.paddr());
        // V-3b-1c: the persistent hostmem ring-offset allocator, sized to the
        // hostmem BAR's shm region (shm id 1). None on a device without it.
        let hostmem = pci.shm_region(1).map(|(_, len)| HostmemAllocator::new(len));

        let mut gpu = Gpu {
            ctrl: Controlq {
                ring_va,
                ring_pa,
                notify_va,
                isr_va,
                irq,
                avail_idx: 0,
                used_seen: 0,
                dead: false,
                flane_va: if flane.is_some() { flane_va } else { 0 },
                flane_pa,
                fslots: [None; FENCED_SLOTS],
                fslot_since: [None; FENCED_SLOTS],
                fslot_poisoned: [false; FENCED_SLOTS],
                fslot_poison_ring: [0; FENCED_SLOTS],
                fslot_poison_ctx: [0; FENCED_SLOTS],
                vindicated: alloc::vec::Vec::new(),
                sync_pending: false,
                completed: alloc::vec::Vec::new(),
                #[cfg(feature = "test-mode")]
                hold_ctx: None,
                #[cfg(feature = "test-mode")]
                held_retires: alloc::vec::Vec::new(),
                #[cfg(feature = "test-mode")]
                abandoned_total: 0,
            },
            ring_va,
            width: DEFAULT_DISPLAY_W,
            height: DEFAULT_DISPLAY_H,
            virgl,
            ctxinit,
            blob,
            cmd_seq: 0,
            last_scanout_seq: 0,
            last_unref_seq: 0,
            condemned: [Condemned { res: 0, unref_requested: false }; GPU_CONDEMNED_MAX],
            condemned_n: 0,
            condemned_overflowed: false,
            fail_next_scanout_disable: false,
            injected_refusal: false,
            condemned_lost: 0,
            num_capsets: 0,
            capset_blob: alloc::vec::Vec::new(),
            capset_id: 0,
            capset_ver: 0,
            venus_capset_blob: alloc::vec::Vec::new(),
            fence_next: 0,
            pci,
            hostmem,
            hostmem_parked: alloc::vec::Vec::new(),
            hostmem_park_count: 0,
            hostmem_reap_count: 0,
            _ring: ring,
            _flane: flane,
        };

        gpu.read_display_info()?;
        if gpu.virgl {
            gpu.probe_capsets()?;
            gpu.blob_probe(blob_probe_va)?;
            gpu.host3d_probe()?;
            gpu.hostmem_map_probe()?;
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
        let mut saw_venus = false;
        // (ver, size) of the enumerated venus capset, for the separate
        // caps-venus fetch below (Model B).
        let mut venus_info: Option<(u32, u32)> = None;
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
            if id == VIRTIO_GPU_CAPSET_VENUS {
                saw_venus = true;
                venus_info = Some((ver, size));
            }
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
            match self.get_capset(id, ver, size) {
                Ok(blob) => {
                    self.capset_blob = blob;
                    self.capset_id = id;
                    self.capset_ver = ver;
                }
                Err(e) => {
                    if self.ctrl.dead {
                        return Err(e);
                    }
                    say!("tapestryd: gpu GET_CAPSET id={} refused (engine healthy); 2D only", id);
                }
            }
        }
        // V-3b-3 (Model B): retain the VENUS capset separately (served by the
        // caps-venus file) for the vn_renderer backend's instance gate -- the
        // ranked virgl blob above is the wrong shape for it. Fetch only a
        // populated enumerated venus capset (size 0 = a listed-but-absent id).
        if let Some((vver, vsize)) = venus_info {
            if vsize > 0 {
                match self.get_capset(VIRTIO_GPU_CAPSET_VENUS, vver, vsize) {
                    Ok(blob) => {
                        self.venus_capset_blob = blob;
                    }
                    Err(e) => {
                        if self.ctrl.dead {
                            return Err(e);
                        }
                        say!("tapestryd: gpu GET_CAPSET id=4 (venus) refused (engine healthy)");
                    }
                }
            }
        }

        // Warp-6 V-0b: is a capset-SELECTED context actually creatable, or is
        // the selection ignored? Those two questions share a success response,
        // so this is only meaningful with the controls the venus gate asserts
        // around it (a capset-2 positive control on both legs; a capset-4
        // create that must FAIL on a no-venus boot). Same failure disposition
        // as the rest of this function (audit W1 F1): a refusal with a healthy
        // engine is a log line, only a real engine death propagates.
        if !self.ctxinit {
            // Not a failure -- the host does not offer the feature. Said out
            // loud: silence here would read as "the probe passed", and a
            // create attempted anyway returns OK over an implicitly-virgl
            // context, the false pass this rung exists to avoid.
            say!("tapestryd: gpu ctx-capset skipped (F_CONTEXT_INIT not offered)");
        } else {
            // The POSITIVE control first and deliberately first: if a plain
            // virgl capset cannot be created either, a later id=4 refusal says
            // nothing about Venus and everything about the engine. Distinct ctx
            // ids per capset so a failed destroy cannot make the next create
            // collide on a duplicate id and read as a Venus refusal.
            for (want, label, ctx) in [
                (VIRTIO_GPU_CAPSET_VIRGL2, "id=2", PROBE_CTX_ID_VIRGL),
                (VIRTIO_GPU_CAPSET_VENUS, "id=4", PROBE_CTX_ID_VENUS),
            ] {
                if want == VIRTIO_GPU_CAPSET_VENUS && !saw_venus {
                    say!("tapestryd: gpu ctx-capset id=4 skipped (capset not enumerated)");
                    continue;
                }
                match self.ctx_create_capset(ctx, want, b"warp-v0b-probe") {
                    Ok(()) => {
                        say!("tapestryd: gpu ctx-capset {} CREATED", label);
                        // The probe is evidence, not a resource: destroy at
                        // once. A destroy refusal is logged and otherwise
                        // ignored -- the id is never reused, so a leak costs one
                        // host context and must not cost the console.
                        if self.ctx_destroy(ctx).is_err() && !self.ctrl.dead {
                            say!("tapestryd: gpu ctx-capset {} destroy refused", label);
                        }
                    }
                    Err(e) => {
                        if self.ctrl.dead {
                            return Err(e);
                        }
                        say!("tapestryd: gpu ctx-capset {} REFUSED (engine healthy)", label);
                    }
                }
            }
        }
        Ok(())
    }

    /// Warp-6 V-1: is a GUEST blob creatable on this host? The rung after
    /// V-0b, gated one feature further along. Like the ctx-capset probe it is
    /// meaningful ONLY with the controls the venus gate asserts -- the same
    /// discrimination shape: a blob CREATED on the venus (blob=on) leg and
    /// `skipped` on the no-blob control, since a blob command is illegal on
    /// the wire without F_RESOURCE_BLOB negotiated. Venus's command ring is a
    /// guest blob (GPU-DESIGN section 2.4), so this is Warp-6's real
    /// prerequisite -- but the object model, mapping and coherency are V-3.
    ///
    /// A dedicated one-page DMA backs the blob rather than borrowing the ring
    /// or the fenced lane: the probe never transfers to it, but its own buffer
    /// removes the question of whether creating a resource over a live
    /// transport region leaves host-side residue, and it prefigures V-3's real
    /// ring buffer. The buffer Drops (handle close + unmap) when this returns.
    ///
    /// Failure disposition matches probe_capsets exactly (audit W1 F1): a
    /// backing alloc that fails, or a create the device merely REFUSES with a
    /// healthy engine, is a log line; only a latched engine death propagates.
    fn blob_probe(&mut self, backing_va: u64) -> Result<(), Error> {
        if !self.blob {
            // Not a failure -- the host does not offer the feature. Said out
            // loud so the control leg's silence is a positive "skipped", not
            // an absent line a broken fixture could equally produce.
            say!("tapestryd: gpu blob-create skipped (F_RESOURCE_BLOB not offered)");
            return Ok(());
        }
        let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
        let prot = T_PROT_READ | T_PROT_WRITE;
        let backing = match unsafe { Dma::new(PAGE_SIZE as usize, rw_map, backing_va, prot) } {
            Ok(d) => d,
            Err(_) => {
                say!("tapestryd: gpu blob-create backing alloc failed; skipped");
                return Ok(());
            }
        };
        // A device-global guest blob over the single backing page. blob_flags
        // 0: the bare object, no USE_MAPPABLE -- V-1 proves creation, the ring
        // (V-3) exercises mapping.
        match self.resource_create_blob(
            BLOB_PROBE_RES_ID,
            VIRTIO_GPU_BLOB_MEM_GUEST,
            0,
            backing.paddr(),
            PAGE_SIZE as u32,
        ) {
            Ok(()) => {
                say!("tapestryd: gpu blob-create guest CREATED");
                // Evidence, not a resource: unref at once. On a CONFIRMED
                // unref the host has released the backing, so `backing` may
                // Drop (unmap + free) safely at scope end. On a FAILED unref
                // (engine alive) the host may still reference these pages, so
                // LEAK the buffer rather than unmap under a live reference -- a
                // theoretical UAF (the probe issues no transfer, so the host
                // never DMAs a bare blob), but one page leaked once at init is
                // the correct trade against unmapping referenced memory. The
                // id is never reused, so the host-side resource leak is bounded
                // to one context's worth.
                if self.resource_unref(BLOB_PROBE_RES_ID).is_err() {
                    if !self.ctrl.dead {
                        say!("tapestryd: gpu blob-create unref refused; backing leaked");
                    }
                    core::mem::forget(backing);
                }
            }
            Err(e) => {
                if self.ctrl.dead {
                    // The engine died with the create PUBLISHED but unretired
                    // (submit_and_wait rings the doorbell before it waits): the
                    // device MAY already have recorded the guest mem_entry PA
                    // into a live host resource. So leak the backing rather than
                    // unmap under a possibly-live reference -- SF1's principle,
                    // its create-Err sibling. Leaving the two branches to
                    // disagree is the trap that reuses the wrong disposition at
                    // V-3, where transfers exist (round F1).
                    core::mem::forget(backing);
                    return Err(e);
                }
                // A HEALTHY refusal means the device processed-and-rejected the
                // create, holding no backing reference, so `backing` Drops
                // (unmap) safely below.
                say!("tapestryd: gpu blob-create guest REFUSED (engine healthy)");
            }
        }
        Ok(())
    }

    /// Warp-6 V-3b-1a: prove the HOST3D + MAP_BLOB path against the real host.
    /// Model B's first risk retired -- does QEMU's virtio-gpu-gl answer
    /// RESOURCE_MAP_BLOB for a HOST3D blob, and does it need a rendering
    /// context? A HOST3D blob_id=0 mappable blob is the venus renderer's shm
    /// path (vkr: blob_id==0 && flags==USE_MAPPABLE), reached only via a
    /// capset-4 (venus) context -- a virgl ctx or device-global is refused
    /// RESP_ERR_UNSPEC. Arm A (venus ctx) is the POSITIVE (the Model B ring
    /// substrate); Arm B (device-global) is the NEGATIVE control whose refusal
    /// confirms the venus-ctx requirement. Each says its verdict + the map_info cache
    /// word out loud, so an absent line is a broken fixture, never a pass; a
    /// device without hostmem (plain virgl, or the 2D dev device) is a positive
    /// "skipped", not silence. Init-time, pre-Server, like blob_probe: each arm
    /// unmaps + unrefs its evidence before any client exists.
    ///
    /// Deliberate deviation from probe_capsets/blob_probe: those propagate a
    /// latched engine death (`self.ctrl.dead`) up to fail bringup fast; this
    /// probe swallows every outcome (always returns Ok). It runs LAST and
    /// venus/HOST3D is an optional path, so a wedge here must NOT fail the whole
    /// compositor init -- tapestryd is the console, and a propagate would turn a
    /// venus-only fault into a warden restart loop with no console at all. A
    /// wedge instead surfaces as the absent MAPPED line, which the venus gate
    /// flags loud. Do not "fix" this into a propagate.
    fn host3d_probe(&mut self) -> Result<(), Error> {
        if !self.blob {
            say!("tapestryd: gpu host3d-map skipped (F_RESOURCE_BLOB not offered)");
            return Ok(());
        }
        if self.pci.shm_region(1).is_none() {
            say!("tapestryd: gpu host3d-map skipped (no hostmem shm region)");
            return Ok(());
        }
        if !self.ctxinit {
            say!("tapestryd: gpu host3d-map skipped (no CONTEXT_INIT -- venus ctx unavailable)");
            return Ok(());
        }
        // Arm A (POSITIVE): a HOST3D blob_id=0 mappable blob under a VENUS
        // (capset-4) context -- the vkr shm path, the Model B ring substrate.
        if self
            .ctx_create_capset(HOST3D_PROBE_CTX_ID, VIRTIO_GPU_CAPSET_VENUS, b"host3d-probe")
            .is_err()
        {
            say!("tapestryd: gpu host3d-map venus ctx create failed; skipped");
            return Ok(());
        }
        self.host3d_probe_arm("venus-ctx", HOST3D_PROBE_RES_CTX, HOST3D_PROBE_CTX_ID, 0);
        let _ = self.ctx_destroy(HOST3D_PROBE_CTX_ID);

        // Arm B (NEGATIVE control): device-global (ctx_id 0). A HOST3D blob_id=0
        // create with no vkr context is EXPECTED to be refused; its refusal
        // confirms the venus-ctx requirement is real, not incidental to Arm A.
        // A distinct hostmem offset so it never aliases Arm A's subregion.
        self.host3d_probe_arm("global", HOST3D_PROBE_RES_GLOBAL, 0, PAGE_SIZE as u64);
        Ok(())
    }

    /// One arm of host3d_probe: create a HOST3D mappable blob under `ctx_id`,
    /// MAP_BLOB it at `offset`, say the verdict, then unmap + unref. A failed
    /// unref leaks the (never-reused) host resource id, per blob_probe.
    fn host3d_probe_arm(&mut self, tag: &str, res_id: u32, ctx_id: u32, offset: u64) {
        match self.create_host3d_blob(
            res_id,
            ctx_id,
            VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE,
            PAGE_SIZE as u32,
            0, // blob_id 0: a probe blob binds no VkDeviceMemory
        ) {
            Ok(()) => match self.map_blob(res_id, offset) {
                Ok(mi) => {
                    say!("tapestryd: gpu host3d-map {} MAPPED (map_info={:#x})", tag, mi);
                    let _ = self.unmap_blob(res_id);
                }
                Err(_) => say!("tapestryd: gpu host3d-map {} create OK but MAP refused", tag),
            },
            Err(_) => say!("tapestryd: gpu host3d-map {} create refused", tag),
        }
        let _ = self.resource_unref(res_id);
    }

    /// Warp-6 V-3b-1c: prove the persistent hostmem RING ENGINE. V-3b-1b proved a
    /// single guest-mapped HOST3D blob; this exercises the reusable
    /// mint_host3d_ring / drop_host3d_ring lifecycle the Model B ring path
    /// (V-3b-1c-2) and the venus-stream forward (V-3b-2) build on. Init-time,
    /// pre-Server, like host3d_probe, and swallows every outcome (Ok) for the same
    /// last-probe / optional-venus reason -- a wedge here never fails the console.
    fn hostmem_map_probe(&mut self) -> Result<(), Error> {
        if !self.blob {
            say!("tapestryd: gpu hostmem-ring skipped (F_RESOURCE_BLOB not offered)");
            return Ok(());
        }
        if self.hostmem.is_none() {
            say!("tapestryd: gpu hostmem-ring skipped (no hostmem shm region)");
            return Ok(());
        }
        if !self.ctxinit {
            say!("tapestryd: gpu hostmem-ring skipped (no CONTEXT_INIT -- venus ctx unavailable)");
            return Ok(());
        }
        if self
            .ctx_create_capset(HOSTMEM_PROBE_CTX_ID, VIRTIO_GPU_CAPSET_VENUS, b"hostmem-probe")
            .is_err()
        {
            say!("tapestryd: gpu hostmem-ring venus ctx create failed; skipped");
            return Ok(());
        }
        self.hostmem_ring_probe();
        let _ = self.ctx_destroy(HOSTMEM_PROBE_CTX_ID);
        Ok(())
    }

    /// The V-3b-1c engine proof: mint TWO HOST3D rings under one venus ctx (the
    /// allocator must hand DISTINCT offsets), round-trip a sentinel through each
    /// guest VA, tear both down, then RE-MINT (the free-list must reclaim a freed
    /// offset -- else a persistent daemon would exhaust the region). Every ring
    /// goes through the same mint_host3d_ring / drop_host3d_ring the Model B ring
    /// path uses. One summary line carries the verdict so a coherency or lifecycle
    /// regression is diagnosable from the boot log.
    fn hostmem_ring_probe(&mut self) {
        let len = PAGE_SIZE as u32;
        let a = match self.mint_host3d_ring(HOSTMEM_PROBE_RES, HOSTMEM_PROBE_CTX_ID, len, 0) {
            Ok(r) => r,
            Err(_) => {
                say!("tapestryd: gpu hostmem-ring mint A refused");
                return;
            }
        };
        let b = match self.mint_host3d_ring(HOSTMEM_PROBE_RES_2, HOSTMEM_PROBE_CTX_ID, len, 0) {
            Ok(r) => r,
            Err(_) => {
                say!("tapestryd: gpu hostmem-ring mint B refused");
                self.drop_host3d_ring(a);
                return;
            }
        };
        // Write BOTH offset-derived sentinels, THEN re-read each: if the two rings
        // aliased one PA, B's write clobbers A's word and A's re-read mismatches --
        // so a_ok/b_ok witness PHYSICAL distinctness, not just distinct allocator
        // offsets (holotype F2). `distinct` still checks the handed offsets.
        let a_want = hostmem_sentinel(a.va, a.offset);
        let b_want = hostmem_sentinel(b.va, b.offset);
        let a_ok = unsafe { r32(a.va) } == a_want;
        let b_ok = unsafe { r32(b.va) } == b_want;
        let distinct = b.offset != a.offset;
        // Save what the summary + reuse check need BEFORE the by-value drops.
        let (a_off, b_off, a_cache) = (a.offset, b.offset, a.cache);
        // Retire both; each drop reclaims its offset into the free-list.
        self.drop_host3d_ring(a);
        self.drop_host3d_ring(b);
        // Re-mint: the allocator must hand back a FREED offset (A's or B's), not a
        // fresh bump past both -- proof the free-list reclaimed on teardown.
        let reuse = match self.mint_host3d_ring(HOSTMEM_PROBE_RES, HOSTMEM_PROBE_CTX_ID, len, 0) {
            Ok(c) => {
                let ok = c.offset == a_off || c.offset == b_off;
                self.drop_host3d_ring(c);
                ok
            }
            Err(_) => false,
        };
        if a_ok && b_ok && distinct && reuse {
            say!(
                "tapestryd: gpu hostmem-ring MAPPED+ROUNDTRIP x2 (off_a={:#x} off_b={:#x} cache={}) teardown+remint-reuse OK",
                a_off, b_off, cache_name(a_cache)
            );
        } else {
            say!(
                "tapestryd: gpu hostmem-ring FAIL (a_ok={} b_ok={} distinct={} reuse={})",
                a_ok, b_ok, distinct, reuse
            );
        }
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
    fn get_capset(
        &mut self,
        id: u32,
        version: u32,
        size: u32,
    ) -> Result<alloc::vec::Vec<u8>, Error> {
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
            return Ok(alloc::vec::Vec::new());
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
        // Byte copy out of the RESP region NOW -- the next command overwrites
        // it. The caller retains it (the ranked virgl capset -> capset_blob,
        // the venus capset -> venus_capset_blob).
        let mut kept = alloc::vec![0u8; take as usize];
        for (i, b) in kept.iter_mut().enumerate() {
            *b = unsafe { r8(blob + i as u64) };
        }
        Ok(kept)
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

    /// Free a resource -- UNLESS the display still names it because a
    /// previous unbind was refused, in which case the free is DEFERRED to
    /// `drain_condemned` and this reports success. Callers must not treat
    /// `Ok(())` as "the host object is gone"; they may drop their record
    /// either way, which is exactly what makes the deferral safe.
    pub fn resource_unref(&mut self, resource_id: u32) -> Result<(), Error> {
        // Membership FIRST (round-4 F3): a PARKED entry's request must be
        // recorded even during an overflow window, or the drain un-parks it
        // without a free and its owner -- told Ok -- never retries. The
        // round-3 F2 shape (overflow-first) silently DROPPED such requests
        // while its say line claimed "defer until the next drain".
        if let Some(e) = self.condemned[..self.condemned_n]
            .iter_mut()
            .find(|e| e.res == resource_id)
        {
            // Record that a free was ASKED FOR. The drain issues only these
            // (round-3 F3): an id parked but never unref-requested belongs to
            // an object whose owner will free it at its own quiesce-safe
            // moment, and the drain must not accelerate that.
            e.unref_requested = true;
            return Ok(());
        }
        // OVERFLOWED is sticky (round-3 F2 [P1]): an overflowed `condemn`
        // records no id, so nothing knows WHICH ids the display still names
        // -- an unrecorded unref here is LEAKED, permanently and honestly
        // (nothing re-issues it later). Over-deferral is the safe direction.
        // Structurally unreachable while condemned_n <= 1; kept for the day
        // the bound changes.
        if self.condemned_overflowed {
            return Ok(());
        }
        self.resource_unref_raw(resource_id)
    }

    /// Park `res_id` as still-scanned-by-the-device. Idempotent.
    ///
    /// THE ID-REUSE WINDOW, since a reviewer will ask: a parked id is matched
    /// by VALUE, so a `res_seq` wrap that reissued the same id before the
    /// drain would act on the NEW resource. How long the window can be:
    /// **unbounded on a persistently-refusing device** (round-3 F7 corrected
    /// an earlier claim of "a handful of commands" -- on the refused path
    /// `reconcile` frequently issues no scanout at all, and if none is ever
    /// accepted no drain runs). What actually bounds this is the OTHER end:
    /// `res_seq` is monotonic and skips zero, and a create that reissued a
    /// still-live device id FAILS AT THE DEVICE (the mint returns E_IO), so
    /// the wrap is fail-closed rather than silently aliasing.
    pub fn condemn(&mut self, res_id: u32) {
        if res_id == 0 || self.condemned[..self.condemned_n].iter().any(|e| e.res == res_id) {
            return;
        }
        if self.condemned_n == GPU_CONDEMNED_MAX {
            // Cannot park it -- so switch the WHOLE unref path to deferring
            // (round-3 F2). Leaking every pending free until the next drain is
            // wasteful and safe; freeing one the display is scanning is a
            // host-side UAF. Say it rather than only counting it: a counter
            // whose flip is nobody's read is indistinguishable from one that
            // never flips.
            self.condemned_lost = self.condemned_lost.saturating_add(1);
            self.condemned_overflowed = true;
            say!(
                "tapestryd: gpu condemned list FULL ({} entries) -- res {} could not \
                 be parked, so PARKED entries still record their unref request; \
                 UNRECORDED unrefs in this window are LEAKED, fail-safe, until drain \
                 ({} lost); the device has refused that many unbinds without \
                 accepting one",
                GPU_CONDEMNED_MAX, res_id, self.condemned_lost
            );
            return;
        }
        self.condemned[self.condemned_n] = Condemned { res: res_id, unref_requested: false };
        self.condemned_n += 1;
    }

    /// Test lever, self-test only: make the next display DISABLE
    /// (`set_scanout(0, ..)`) report a device refusal without issuing it, so
    /// the condemn/defer/drain chain has a real driver. Deliberately NOT a
    /// client verb -- `warp_img_selftest` runs pre-READY, before any
    /// connection exists, so this needs no external surface at all, and giving it one
    /// "for symmetry" would be the #178 box-wide-kill-switch anti-pattern its
    /// `ring-inject` sibling is already bounded against.
    pub fn arm_scanout_disable_refusal(&mut self) {
        self.fail_next_scanout_disable = true;
    }

    /// Consume the "that refusal was mine" marker. One-shot: a SECOND refusal
    /// in the same boot is by definition not the injected one and reports as
    /// a real device refusal, which is what the gate must catch.
    pub fn take_injected_refusal(&mut self) -> bool {
        core::mem::replace(&mut self.injected_refusal, false)
    }

    pub fn condemned_count(&self) -> usize {
        self.condemned_n
    }

    /// An accepted scanout means the display no longer names anything parked
    /// BEFORE it -- with one exception: the resource that scanout just BOUND
    /// (`keep`), which the display now names precisely because of it. Draining
    /// that one would free what was just put on screen, i.e. the mechanism
    /// producing the exact fault it exists to prevent.
    ///
    /// Only entries whose free was actually REQUESTED are unref'd (round-3
    /// F3). The rest are merely un-parked: their owner will free them at its
    /// own quiesce-safe moment, and issuing the unref here would be the first
    /// unref-before-quiesce path in tapestryd.
    ///
    /// Calls the RAW unref -- routing through the checking one would re-read a
    /// list this is in the middle of emptying.
    fn drain_condemned(&mut self, keep: u32) {
        let n = core::mem::replace(&mut self.condemned_n, 0);
        self.condemned_overflowed = false;
        let mut kept = 0usize;
        for i in 0..n {
            let e = self.condemned[i];
            if e.res != 0 && e.res == keep {
                self.condemned[kept] = e; // still on screen: stays parked
                kept += 1;
                continue;
            }
            if e.res != 0 && e.unref_requested {
                let _ = self.resource_unref_raw(e.res);
            }
        }
        self.condemned_n = kept;
    }

    fn resource_unref_raw(&mut self, resource_id: u32) -> Result<(), Error> {
        self.cmd_seq = self.cmd_seq.wrapping_add(1);
        self.last_unref_seq = self.cmd_seq;
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_UNREF);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl
            .step("RESOURCE_UNREF", GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA)
    }

    /// RESOURCE_CREATE_BLOB for a GUEST-memory blob (Warp-6 V-1): the blob's
    /// storage IS the single guest `mem_entry` at `pa`/`len` -- no host
    /// allocation, no `blob_id`, no hostmem BAR. This is the substrate Venus's
    /// command ring is built from (GPU-DESIGN section 2.4). The host3d path
    /// (host-allocated storage mapped through the hostmem window via MAP_BLOB)
    /// is the V-2 delta and is deliberately NOT here. `blob_flags` is the
    /// caller's (0 for the bare V-1 probe; the ring's USE_MAPPABLE arrives with
    /// the ring at V-3). device-global (ctx_id 0): the transport ring is not
    /// context-scoped.
    ///
    /// A blob command is illegal on the wire without `F_RESOURCE_BLOB`
    /// negotiated, and this REFUSES it at runtime rather than trusting the
    /// caller -- the V-0b F2 lesson (a caller-side-only guard is a no-op the
    /// moment a future caller forgets it; V-3 will reach here with a
    /// client-influenced request). `blob_probe` also checks `self.blob`, so the
    /// guard is redundant for the V-1 probe and load-bearing for V-3.
    pub fn resource_create_blob(
        &mut self,
        resource_id: u32,
        blob_mem: u32,
        blob_flags: u32,
        pa: u64,
        len: u32,
    ) -> Result<(), Error> {
        if !self.blob {
            return Err(Error::Hardware);
        }
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, blob_mem);
            w32(req_va + 32, blob_flags);
            w32(req_va + 36, 1); // nr_entries
            w64(req_va + 40, 0); // blob_id: 0 for a guest blob (host mints none)
            w64(req_va + 48, u64::from(len)); // size
            // mem_entry[0]: the single guest-page backing.
            w64(req_va + 56, pa);
            w32(req_va + 64, len);
            w32(req_va + 68, 0); // padding
        };
        // 24 (hdr) + 32 (fixed fields) + 16 (one mem_entry) = 72 = HDR + 48.
        self.ctrl.step(
            "RESOURCE_CREATE_BLOB",
            GPU_CTRL_HDR_LEN + 48,
            GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA,
        )
    }

    /// V-3a: register a coherent guest-blob ring backing (blob_mem=GUEST) as a
    /// single contiguous mem_entry. A thin wrapper so the server mints ring
    /// blobs without importing the wire constant. Err(Hardware) on a device
    /// without the blob feature -- the caller treats that as an unregistered
    /// pure-shmem ring (the ring transport does not depend on the device
    /// knowing the blob at V-3a; that is Venus's, V-3b).
    pub fn create_ring_blob(&mut self, resource_id: u32, pa: u64, len: u32) -> Result<(), Error> {
        self.resource_create_blob(resource_id, VIRTIO_GPU_BLOB_MEM_GUEST, 0, pa, len)
    }

    /// V-3b (Model B): create a HOST3D-backed mappable blob. Unlike a GUEST
    /// blob (`resource_create_blob`), the host allocates the storage: there is
    /// no guest `mem_entry` (nr_entries=0), so the request is HDR+32 with no
    /// trailing entry. The ctx_id rides the header -- QEMU passes
    /// `cblob.hdr.ctx_id` straight to virglrenderer, which scopes a HOST3D blob
    /// to a rendering context. `blob_flags` carries USE_MAPPABLE for a blob the
    /// caller will MAP_BLOB. Refuses on a device without F_RESOURCE_BLOB (the
    /// V-0b F2 runtime-guard rule, not a caller-side-only check).
    pub fn create_host3d_blob(
        &mut self,
        resource_id: u32,
        ctx_id: u32,
        blob_flags: u32,
        len: u32,
        blob_id: u64,
    ) -> Result<(), Error> {
        if !self.blob {
            return Err(Error::Hardware);
        }
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB, ctx_id);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, VIRTIO_GPU_BLOB_MEM_HOST3D);
            w32(req_va + 32, blob_flags);
            w32(req_va + 36, 0); // nr_entries: 0 -- host-allocated, no guest backing
            // blob_id names the host resource the blob binds. A command ring
            // uses 0 (Venus requires supports_blob_id_0, matching vtest's
            // shmem); a HOST_VISIBLE VkDeviceMemory bo passes the Venus
            // mem_id, so virglrenderer maps THAT allocation's host pages
            // (V-3b-3c-2). The kernel/QEMU treat it as an opaque u64.
            w64(req_va + 40, blob_id);
            w64(req_va + 48, u64::from(len)); // size
        };
        // 24 (hdr) + 32 (fixed fields) = 56 = HDR + 32. No mem_entry for HOST3D.
        self.ctrl.step(
            "RESOURCE_CREATE_BLOB(HOST3D)",
            GPU_CTRL_HDR_LEN + 32,
            GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA,
        )
    }

    /// V-3b (Model B): RESOURCE_MAP_BLOB -- ask the host to place a HOST3D blob
    /// at `offset` within the hostmem BAR, and read back its `map_info` (the
    /// VIRTIO_GPU_MAP_CACHE_* cache type). On the host this is
    /// `memory_region_add_subregion(&hostmem, offset, mr)`, so the blob's bytes
    /// become visible at `hostmem_base + offset` -- the PA the guest then maps
    /// via SYS_BURROW_FROM_HOSTMEM (V-2). Returns the map_info cache word.
    pub fn map_blob(&mut self, resource_id: u32, offset: u64) -> Result<u32, Error> {
        if !self.blob {
            return Err(Error::Hardware);
        }
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0); // padding
            w64(req_va + 32, offset);
            // Residue guard (the get_capset_info rule): submit_and_wait zeroes
            // only the response HEADER, and map_info is read with no used-len
            // check, so pre-zero it -- a short-writing device reads as cache=0.
            let resp = self.ring_va + RESP_OFF;
            w32(resp + 24, 0);
        };
        // 24 (hdr) + 4 (resource_id) + 4 (padding) + 8 (offset) = 40 = HDR + 16.
        self.ctrl.step(
            "RESOURCE_MAP_BLOB",
            GPU_CTRL_HDR_LEN + 16,
            GPU_CTRL_HDR_LEN + 8,
            VIRTIO_GPU_RESP_OK_MAP_INFO,
        )?;
        let r = self.ring_va + RESP_OFF + GPU_CTRL_HDR_LEN as u64;
        Ok(unsafe { r32(r) })
    }

    /// V-3b (Model B): RESOURCE_UNMAP_BLOB -- release a HOST3D blob's hostmem
    /// subregion (`memory_region_del_subregion` on the host). The inverse of
    /// map_blob; a teardown unmaps before unref so no dangling subregion is
    /// left in the hostmem BAR.
    pub fn unmap_blob(&mut self, resource_id: u32) -> Result<(), Error> {
        if !self.blob {
            return Err(Error::Hardware);
        }
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0); // padding
        };
        // 24 (hdr) + 8 (resource_id + padding) = 32 = HDR + 8.
        self.ctrl.step(
            "RESOURCE_UNMAP_BLOB",
            GPU_CTRL_HDR_LEN + 8,
            GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA,
        )
    }

    /// V-3b-1c: mint a HOST3D ring -- the composed Model B ring lifecycle over
    /// create_host3d_blob + map_blob + burrow_from_hostmem. Reserves a page-
    /// aligned hostmem offset from the persistent allocator, creates the blob
    /// under `ctx_id` (which MUST be a venus ctx -- V-3b-1a proved a non-venus
    /// create is refused), maps it into the hostmem BAR, and guest-maps the
    /// subrange at the HOST-DICTATED cache attribute (map_info; never a guess --
    /// GPU-DESIGN 6.2). Returns the ring handle. Every early-error path unwinds
    /// exactly what it acquired (offset -> resource -> subregion) so no half-minted
    /// ring is left behind. `res_id` is the caller's, unique among LIVE rings (an
    /// id may be reused once its ring is retired -- the probe's re-mint does).
    /// Fails Hardware without a hostmem region, when the region is full, or when
    /// `len` page-rounds past a u32 (holotype F3 -- the engine carries its own
    /// size bound rather than inheriting the region size as an accidental guard).
    pub fn mint_host3d_ring(
        &mut self,
        res_id: u32,
        ctx_id: u32,
        len: u32,
        blob_id: u64,
    ) -> Result<HostRing, Error> {
        let size = (u64::from(len) + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
        // The blob size crosses the wire as a u32; a len that page-rounds to 1<<32
        // would truncate to a 0-size create over a full-size reservation. Guard
        // BEFORE the alloc so nothing is unwound (holotype F3). Create at the page-
        // rounded size so the host allocation covers the whole mapped page.
        let size32 = u32::try_from(size).map_err(|_| Error::Hardware)?;
        // V-3b-1c-2b F2: reclaim-before-alloc. A parked ring (retired while a
        // client still referenced it) is freed here once its client has released
        // it, so offset pressure drives reclaim exactly when a new mint needs the
        // space. Issuing the parked rings' controlq teardown here is safe -- mint
        // runs in the serve-loop context, where controlq commands are normal. Loop
        // reap+alloc while the (per-pass-capped) reap makes progress (round-2 F2):
        // a single capped pass may not free enough, but each pass that reclaims >0
        // may free the offset the alloc needs; fail only when a pass frees nothing
        // and the region is still full.
        let off = loop {
            let reclaimed = self.reap_hostmem_parked();
            match self.hostmem.as_mut().and_then(|a| a.alloc(size)) {
                Some(o) => break o,
                None if reclaimed > 0 => continue,
                None => return Err(Error::Hardware),
            }
        };
        if let Err(e) =
            self.create_host3d_blob(res_id, ctx_id, VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE, size32, blob_id)
        {
            self.hostmem_free(off, size);
            return Err(e);
        }
        let map_info = match self.map_blob(res_id, off) {
            Ok(mi) => mi,
            Err(e) => {
                let _ = self.resource_unref(res_id);
                self.hostmem_free(off, size);
                return Err(e);
            }
        };
        let cache = map_info_to_cache(map_info);
        let va = match self.pci.burrow_from_hostmem(1, off, size, cache) {
            Ok(v) => v,
            Err(_) => {
                let _ = self.unmap_blob(res_id);
                let _ = self.resource_unref(res_id);
                self.hostmem_free(off, size);
                return Err(Error::Hardware);
            }
        };
        Ok(HostRing { res_id, offset: off, va, size, cache })
    }

    /// V-3b-1c-2b F2: retire a HOST3D ring -- reap now if safe, else PARK. The
    /// CALLER disarms any weft share BEFORE this (so no NEW client can claim the
    /// backing). Then this reads the ring's hostmem burrow's TOTAL ref count
    /// (handle_count + mapping_count) via tapestryd's still-live VA: at count == 1
    /// the ONLY reference is tapestryd's own map -- no client map AND no client
    /// that has CLAIMED but not yet mapped -- so the offset is safe to reclaim NOW
    /// (`drop_host3d_ring`). At count > 1 (a client kept its map past its conn, OR
    /// a client claimed the share and holds the transferred registration pin
    /// pending its map) or on an unexpected query error, tapestryd KEEPS its VA
    /// mapped and parks the token for `reap_hostmem_parked`; freeing the QEMU
    /// subregion now would re-hand the offset under the client's live-or-pending
    /// PTEs (a cross-client alias).
    ///
    /// Why the TOTAL ref count, not mapping_count (audit F1): weft_share_claim
    /// consumes the share and TRANSFERS the registration pin (a handle_count ref)
    /// to the claiming client BEFORE burrow_share_into bumps mapping_count later
    /// in the same SYS_WEFT_MAP -- so a mapping_count==1 read would miss a client
    /// irrevocably about to map, and reclaim under it. The transferred pin makes
    /// the SUM >= 2, closing that window (the image.c handle_count half). At
    /// sum==1 the reference set is empty of clients AND cannot grow: the share is
    /// disarmed (no claim), and there is no client ref (map OR pin) to fork.
    /// Consumes the token by value (no double-drop).
    pub fn retire_host3d_ring(&mut self, ring: HostRing) {
        let refs = unsafe { t_hostmem_refcount(ring.va, ring.size) };
        if refs == 1 {
            self.drop_host3d_ring(ring);
        } else {
            if refs < 1 {
                // Impossible while tapestryd still maps it (its own map is >= 1):
                // a negative errno means the VA no longer resolves to this hostmem
                // burrow. Park conservatively -- never reclaim on an uncertain
                // count -- and log; the reaper frees it if/when the count reads 1,
                // else it is bounded by the client's own I-32 budget.
                say!(
                    "tapestryd: hostmem ring res {} unexpected refcount {} at retire -- parking",
                    ring.res_id, refs
                );
            }
            self.hostmem_parked.push(ring);
            self.hostmem_park_count = self.hostmem_park_count.saturating_add(1);
        }
    }

    /// V-3b-1c-2b F2: re-check parked HOST3D rings and reclaim those whose only
    /// remaining reference is tapestryd's own map (total ref count back to 1 --
    /// the claiming client released BOTH its mapping and its transferred pin).
    /// Runs at MINT-time (reclaim-before-alloc), NOT the completion pump: reclaim
    /// issues controlq teardown (`drop_host3d_ring`), which is only established
    /// safe from the serve-loop request context mint runs in; moving it into the
    /// pump would need a re-entrancy re-examination (audit F2). Mint is also where
    /// offset pressure arises, so the reclaim happens exactly when the space is
    /// needed. A parked ring whose client never unmaps stays parked, bounded by
    /// that client's own I-32 budget; Proc death drops the mapping (address-space
    /// teardown) so a crashed client's ring is reclaimed at the next mint. Bounded
    /// per PASS (`HOSTMEM_REAP_PER_PASS`) so no single pass issues an unbounded
    /// burst of controlq teardowns; a mint under offset pressure may run several
    /// passes (the reap+alloc loop in mint_host3d_ring), draining up to the whole
    /// parked list -- bounded by live clients' ring budgets, so a latency spike,
    /// never unbounded.
    /// Returns the number reclaimed THIS pass (0 = none eligible in the scanned
    /// prefix) so a caller under offset pressure (mint) can loop while progress is
    /// made rather than fail an alloc that a second pass would satisfy (round-2 F2).
    fn reap_hostmem_parked(&mut self) -> u32 {
        if self.hostmem_parked.is_empty() {
            return 0;
        }
        const HOSTMEM_REAP_PER_PASS: u32 = 8;
        let mut reclaimed: u32 = 0;
        let mut i = 0;
        while i < self.hostmem_parked.len() && reclaimed < HOSTMEM_REAP_PER_PASS {
            let (va, size) = (self.hostmem_parked[i].va, self.hostmem_parked[i].size);
            if unsafe { t_hostmem_refcount(va, size) } == 1 {
                let ring = self.hostmem_parked.swap_remove(i);
                self.drop_host3d_ring(ring);
                reclaimed += 1;
                self.hostmem_reap_count = self.hostmem_reap_count.saturating_add(1);
                // swap_remove moved a not-yet-checked element to i; do not advance.
            } else {
                i += 1;
            }
        }
        reclaimed
    }

    /// V-3b-2 xproc-E2E: the host3d-ring reap ledger, surfaced to the warp ctl
    /// so the cross-Proc lifecycle E2E can witness park-on-retire and
    /// reclaim-on-release with a real client refcount.
    pub fn hostmem_park_count(&self) -> u64 {
        self.hostmem_park_count
    }
    pub fn hostmem_reap_count(&self) -> u64 {
        self.hostmem_reap_count
    }

    /// V-3b-1c: the unconditional inverse of mint_host3d_ring (detach tapestryd's
    /// VA, release the hostmem subregion, drop the host resource, reclaim the
    /// offset). Callers reach it through `retire_host3d_ring` (reap-if-safe) or
    /// `reap_hostmem_parked` (a parked ring now unmapped) -- NEVER directly on a
    /// client-shared ring, since it reclaims the offset without checking the
    /// mapping_count. Takes the handle BY VALUE (holotype F1): consuming it makes a
    /// double-drop / use-after-drop a compile error. Best-effort on each device
    /// step (a teardown never fails the caller), but a device refusal is LOGGED
    /// (holotype F4) -- a swallowed unref/unmap would else surface as a bogus
    /// `reuse=false` at the next re-mint, indicting the free-list for a teardown
    /// fault.
    pub fn drop_host3d_ring(&mut self, ring: HostRing) {
        let _ = unsafe { t_burrow_detach(ring.va, ring.size) };
        if self.unmap_blob(ring.res_id).is_err() {
            say!("tapestryd: hostmem ring res {} unmap refused at teardown", ring.res_id);
        }
        if self.resource_unref(ring.res_id).is_err() {
            say!("tapestryd: hostmem ring res {} unref refused at teardown", ring.res_id);
        }
        self.hostmem_free(ring.offset, ring.size);
    }

    /// Return a hostmem extent to the persistent allocator (no-op when there is no
    /// allocator -- only when the device had no hostmem region to allocate from).
    fn hostmem_free(&mut self, offset: u64, size: u64) {
        if let Some(a) = self.hostmem.as_mut() {
            a.free(offset, size);
        }
    }

    /// CTX_CREATE: mint rendering context `ctx_id` host-side. context_init
    /// stays 0 (VIRTIO_GPU_F_CONTEXT_INIT is not negotiated), so the host
    /// creates the default virgl context -- which serves both VIRGL and
    /// VIRGL2 capset consumers; the seam records the client's declared
    /// capset for the day the feature is negotiated. Virgl-gated by the
    /// caller (the warp tree answers E_OPNOTSUPP on a 2D device).
    pub fn ctx_create(&mut self, ctx_id: u32, debug_name: &[u8]) -> Result<(), Error> {
        // capset 0 == the device's default, i.e. exactly what this call sent
        // before the capset parameter existed. Every existing caller keeps
        // byte-identical behaviour.
        self.ctx_create_capset(ctx_id, 0, debug_name)
    }

    /// Create a VENUS (capset-4) device context -- the Model B HOST3D-ring
    /// owner (V-3b-1c-2). A thin wrapper so the venus capset id stays
    /// gpu-internal; refuses (Error::Hardware) on a device that did not
    /// negotiate context-init / the venus capset, exactly as `ctx_create_capset`.
    pub fn ctx_create_venus(&mut self, ctx_id: u32) -> Result<(), Error> {
        self.ctx_create_capset(ctx_id, VIRTIO_GPU_CAPSET_VENUS, b"warp-venus")
    }

    /// `CTX_CREATE` selecting a capset via `context_init` (bits 0-7).
    ///
    /// The field is honoured ONLY when `VIRTIO_GPU_F_CONTEXT_INIT` was
    /// negotiated. A non-zero capset without `self.ctxinit` is REFUSED at
    /// runtime, not merely debug-asserted (main audit F2): the device would
    /// otherwise discard the selection and return OK over a wrong-kind context
    /// -- the exact false pass this whole path exists to prevent -- and a
    /// `debug_assert` is a no-op in the shipping release build, so it is not
    /// the guard the false-pass argument needs. This closes it in release for
    /// V-3, where a client-influenced `capset_id` will reach here.
    pub fn ctx_create_capset(
        &mut self,
        ctx_id: u32,
        capset_id: u32,
        debug_name: &[u8],
    ) -> Result<(), Error> {
        if capset_id != 0 && !self.ctxinit {
            return Err(Error::Hardware);
        }
        let req_va = self.ring_va + REQ_OFF;
        let nlen = debug_name.len().min(63);
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_CTX_CREATE, ctx_id);
            w32(req_va + 24, nlen as u32);
            w32(req_va + 28, capset_id & 0xff);
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
        self.cmd_seq = self.cmd_seq.wrapping_add(1);
        self.last_scanout_seq = self.cmd_seq;
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_SET_SCANOUT);
            write_rect(req_va + 24, 0, 0, w, h);
            w32(req_va + 40, 0); // scanout_id
            w32(req_va + 44, resource_id);
        };
        // The self-test lever, checked for the DISABLE only. It returns the
        // same Err the wire path returns, WITHOUT issuing the command, so
        // everything downstream is driven by a refusal indistinguishable from
        // a real one -- a lever that took a different path would prove
        // nothing about the path that matters.
        if resource_id == 0 && self.fail_next_scanout_disable {
            self.fail_next_scanout_disable = false;
            self.injected_refusal = true;
            return Err(Error::Hardware);
        }
        let r = self
            .ctrl
            .step("SET_SCANOUT", GPU_CTRL_HDR_LEN + 24, GPU_CTRL_HDR_LEN, VIRTIO_GPU_RESP_OK_NODATA);
        if r.is_ok() {
            self.drain_condemned(resource_id);
        }
        r
    }

    /// SET_SCANOUT_BLOB, returning the RAW resp_type rather than stepping
    /// against an expectation. Layout per VIRTIO 1.2 section 5.7.6.7 struct
    /// virtio_gpu_set_scanout_blob: hdr(24) + rect(16) + scanout_id +
    /// resource_id + width + height + format + padding + strides[4] +
    /// offsets[4] = 96 bytes. Err(()) = the submission itself died (device
    /// wedged), distinct from any refusal code.
    ///
    /// ONE wire implementation for both callers (the #230 mirror-by-meaning
    /// rule): the W-3a capability probe needs the raw code (reading WHICH
    /// refusal this host gives IS its measurement), while the W-3c Direct
    /// bind needs a verdict -- so the verdict arm is a thin wrapper below,
    /// never a second copy of the 96-byte layout.
    pub fn set_scanout_blob_probe(
        &mut self,
        resource_id: u32,
        w: u32,
        h: u32,
        format: u32,
        stride: u32,
    ) -> Result<u32, ()> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr(req_va, VIRTIO_GPU_CMD_SET_SCANOUT_BLOB);
            write_rect(req_va + 24, 0, 0, w, h);
            w32(req_va + 40, 0); // scanout_id
            w32(req_va + 44, resource_id);
            w32(req_va + 48, w);
            w32(req_va + 52, h);
            w32(req_va + 56, format);
            w32(req_va + 60, 0); // padding
            w32(req_va + 64, stride); // strides[0]
            w32(req_va + 68, 0);
            w32(req_va + 72, 0);
            w32(req_va + 76, 0);
            w32(req_va + 80, 0); // offsets[0..4]
            w32(req_va + 84, 0);
            w32(req_va + 88, 0);
            w32(req_va + 92, 0);
        };
        self.ctrl.submit_and_wait(GPU_CTRL_HDR_LEN + 72, GPU_CTRL_HDR_LEN)
    }

    /// W-3c: the Direct arm's bind -- scanout 0 shows the presentable's blob
    /// resource at its DECLARED shape (WARP-WSI-DESIGN 4.2). The verdict form
    /// of the probe above; `OK_NODATA` is the only acceptance (the W-3a
    /// measurement: this host answers `INVALID_RESOURCE_ID` for a resource it
    /// cannot fetch, so a refusal is legible). Acceptance is NOT a pixel
    /// claim -- what it establishes is that the display now REFERENCES this
    /// resource, which is precisely what makes the unbind-before-unref
    /// ordering load-bearing.
    pub fn set_scanout_blob(
        &mut self,
        resource_id: u32,
        w: u32,
        h: u32,
        format: u32,
        stride: u32,
    ) -> Result<(), Error> {
        match self.set_scanout_blob_probe(resource_id, w, h, format, stride) {
            Ok(VIRTIO_GPU_RESP_OK_NODATA) => {
                // Drain here too (round-3 F2). A blob bind changes what the
                // display names exactly as a plain bind does, and this was
                // the ONLY accepted bind that did not drain -- which is what
                // let the park list grow past one and made the overflow arm
                // reachable at W-3c-2, where `present-to <surface> img <n>`
                // makes blob binds client-driven and repeatable. Draining on
                // both keeps `condemned_n <= 1` structural.
                self.drain_condemned(resource_id);
                Ok(())
            }
            Ok(_) => Err(Error::Hardware),
            Err(()) => Err(Error::Hardware),
        }
    }

    /// W-3c: mint a PRESENTABLE -- a HOST3D blob bound by `blob_id` to a venus
    /// allocation, for a swapchain image that exists to be NAMED (scanned out,
    /// composed from) and never guest-mapped.
    ///
    /// THE FLAG IS NOT THE PROPERTY (WARP-WSI-DESIGN 4.1, AMENDED at W-3c-1
    /// after measurement; operator-ratified). The design originally specified
    /// `USE_SHAREABLE` *without* `USE_MAPPABLE`, on the reasoning that omitting
    /// the flag is what keeps the image out of the guest. The W-3c-1 self-test
    /// measured that on the real chain and this host REFUSES it -- cleanly
    /// isolated, same size, same ctx, blob_id 0, only the flag varying:
    /// `USE_SHAREABLE` alone refused, `USE_MAPPABLE|USE_SHAREABLE` refused,
    /// `USE_MAPPABLE` alone accepted. So virglrenderer 1.1.0 refuses
    /// USE_SHAREABLE on a HOST3D blob outright.
    ///
    /// The correction: `USE_MAPPABLE` only declares that the host MAY place the
    /// blob in the hostmem BAR. What actually exposes bytes to the guest is
    /// `RESOURCE_MAP_BLOB` + `SYS_BURROW_FROM_HOSTMEM` -- so guest-invisibility
    /// is secured by NEVER MAPPING IT, which is a property of this call's
    /// CALLERS, not of its flags. Hence: mint with the flag the host accepts,
    /// and let the presentable path call neither `map_blob` nor
    /// `burrow_from_hostmem`. Every consequence the original design claimed
    /// still holds -- no hostmem offset, no guest VA, no weft share, no reclaim
    /// park, no #847 dual count -- because none of them followed from the flag.
    ///
    /// That is the ONE difference from `mint_host3d_ring`, which takes the same
    /// flag and then goes on to map: this function stops here. Teardown is the
    /// lone `resource_unref`, ordered AFTER the display unbind by the caller
    /// (the `gl_evict_res` discipline).
    ///
    /// Class-scoped, per the W-3a rule: the measurement above used blob_id 0
    /// (virglrenderer's own plain-memory arm). Whether USE_SHAREABLE is
    /// accepted for a REAL venus allocation is unmeasured and needs a client
    /// (W-3d) -- but the mapped-never property does not depend on the answer.
    pub fn create_presentable(
        &mut self,
        res_id: u32,
        ctx_id: u32,
        len: u32,
        blob_id: u64,
    ) -> Result<(), Error> {
        self.create_host3d_blob(res_id, ctx_id, VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE, len, blob_id)
    }

    /// W-3a probe: CTX_ATTACH_RESOURCE with the raw resp_type returned -- the
    /// cross-ctx import acceptance leg (can the compositor's virgl ctx attach
    /// a resource created under a DIFFERENT device ctx?). Same Err(()) split
    /// as set_scanout_blob_probe.
    pub fn ctx_attach_resource_probe(&mut self, ctx_id: u32, resource_id: u32) -> Result<u32, ()> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id);
            w32(req_va + 24, resource_id);
            w32(req_va + 28, 0);
        };
        self.ctrl.submit_and_wait(GPU_CTRL_HDR_LEN + 8, GPU_CTRL_HDR_LEN)
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

    // --- the fenced lane (Warp-2d) ---------------------------------------

    /// Common CLIENT fenced-lane admission: lane present + engine alive, the
    /// request (header + `extra` payload bytes) fits a slot, a client-pool
    /// slot free (the reserved compositor slot is not in this pool, C-6).
    fn fenced_begin(&mut self, extra: u64) -> Result<usize, FencedErr> {
        if self.ctrl.flane_va == 0 || self.ctrl.dead {
            return Err(FencedErr::Dead);
        }
        if (GPU_CTRL_HDR_LEN as u64) + extra > FREQ_LEN {
            return Err(FencedErr::TooBig);
        }
        if let Some(s) = self.ctrl.alloc_fenced_slot() {
            return Ok(s);
        }
        // Full-but-recoverable retries; permanently exhausted does not.
        if self.ctrl.lane_exhausted() {
            Err(FencedErr::Dead)
        } else {
            Err(FencedErr::Again)
        }
    }

    /// The compositor-owned admission (Warp-C C-6): lane present + engine
    /// alive, the reserved slot free. `Again` here means the reserved slot is
    /// occupied or poisoned -- the caller keeps the request and retries at
    /// the next completion / vindication -- never that the client pool is
    /// full, which this path does not touch.
    fn fenced_begin_comp(&mut self) -> Result<usize, FencedErr> {
        if self.ctrl.flane_va == 0 || self.ctrl.dead {
            return Err(FencedErr::Dead);
        }
        self.ctrl.alloc_comp_slot().ok_or(FencedErr::Again)
    }

    #[allow(clippy::too_many_arguments)]
    fn fenced_commit(
        &mut self,
        slot: usize,
        req_len: u32,
        fence_id: u64,
        ctx_pub: u32,
        readback: bool,
        comp: bool,
        ring_idx: u8,
    ) -> Result<u64, FencedErr> {
        self.ctrl
            .submit_fenced(
                slot,
                req_len,
                FenceTag { fence_id, ctx_pub, readback, comp, abandoned: false, ok: false, ring_idx },
            )
            .map_err(|_| FencedErr::Dead)?;
        self.fence_next = fence_id;
        Ok(fence_id)
    }

    /// SUBMIT_3D: queue `stream` (an opaque VIRGL_CCMD buffer -- the server
    /// does not parse it, GPU-DESIGN section 2.1) on the fenced lane for
    /// `ctx_id`, returning the fence id its completion will carry on the
    /// owning seam ctx (`ctx_pub`). A nonzero `ring_idx` fences on that
    /// venus TIMELINE (hdr byte 20 + INFO_RING_IDX -- the host signals it
    /// when the bound VkQueue's work completes, not when the global lane
    /// drains); 0 keeps today's device-global fence exactly (the flag is
    /// not set, so the header is byte-identical to the pre-multi-queue one).
    pub fn submit_3d(
        &mut self,
        ctx_id: u32,
        ctx_pub: u32,
        stream: &[u8],
        ring_idx: u8,
    ) -> Result<u64, FencedErr> {
        let slot = self.fenced_begin(8 + stream.len() as u64)?;
        let req = self.ctrl.flane_va + (slot as u64) * FREQ_LEN;
        let fence_id = self.fence_next.wrapping_add(1);
        unsafe {
            write_ctrl_hdr_fenced(req, VIRTIO_GPU_CMD_SUBMIT_3D, ctx_id, fence_id);
            if ring_idx > 0 {
                w32(req + 4, VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX);
                w8(req + 20, ring_idx);
            }
            w32(req + 24, stream.len() as u32);
            w32(req + 28, 0); // padding
        };
        copy_stream(req + 32, stream);
        self.fenced_commit(
            slot,
            GPU_CTRL_HDR_LEN + 8 + stream.len() as u32,
            fence_id,
            ctx_pub,
            false,
            false,
            ring_idx,
        )
    }

    /// SUBMIT_3D on the SYNCHRONOUS slot, under the server's own authority
    /// (#240 / GPU-DESIGN 4.5.4b). The health probe is an ordered
    /// upload-copy-readback triple, and riding the sync slot is what makes
    /// "ordered" a property of the code rather than an argument about
    /// controlq FIFO across two different publication paths. Never used for
    /// client streams -- those keep the fenced lane and its admission.
    pub fn submit_3d_sync(&mut self, ctx_id: u32, stream: &[u8]) -> Result<(), Error> {
        // Audit F6: the fenced twin bounds its payload against the slot
        // (`fenced_begin`); this lane had no bound at all. It was
        // unreachable when the sole caller passed a fixed 56 bytes; the
        // compositor's C-3 blit stream is the next caller (it chunks at
        // `sync_stream_max`), and the overrun is silent: past
        // REQ_REGION_LEN the copy walks into the DEVICE-WRITABLE response
        // region, so the failure would surface as a corrupted response
        // rather than as this request being too big.
        // `Hardware` because that is the single code this whole lane
        // returns and every caller treats any Err identically (the probe
        // folds it to UNKNOWN); a distinct variant would be a libdriver ABI
        // change buying nothing at the one call site that can see it.
        if (GPU_CTRL_HDR_LEN as usize) + 8 + stream.len() > REQ_REGION_LEN as usize {
            return Err(Error::Hardware);
        }
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_SUBMIT_3D, ctx_id);
            w32(req_va + 24, stream.len() as u32);
            w32(req_va + 28, 0); // padding
        };
        copy_stream(req_va + 32, stream);
        self.ctrl.step(
            "SUBMIT_3D_SYNC",
            GPU_CTRL_HDR_LEN + 8 + stream.len() as u32,
            GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA,
        )
    }

    /// The upload twin of `transfer_from_3d_sync`. A virtio-gpu command,
    /// NOT a command-buffer one, so it keeps working on a context whose
    /// command stream vrend has latched off (measured, GPU-DESIGN 4.5.4a)
    /// -- which is precisely what lets the health probe seed a token the
    /// dropped copy will fail to overwrite.
    pub fn transfer_to_3d_sync(
        &mut self,
        ctx_id: u32,
        res_id: u32,
        w: u32,
        h: u32,
        stride: u32,
    ) -> Result<(), Error> {
        self.transfer_to_3d_box_sync(ctx_id, res_id, 0, 0, w, h, 0, stride)
    }

    /// The RECT form (Warp-C C-3): box (x, y, w, h) at level 0, read from
    /// the attached backing at `offset` with row `stride` -- the 3D twin of
    /// `transfer` (TRANSFER_TO_HOST_2D), which is how the composed screen's
    /// CPU-painted chrome and its CPU-composed surfaces reach a 3D screen
    /// without re-uploading the whole frame. Same sync slot, same ordering.
    #[allow(clippy::too_many_arguments)]
    pub fn transfer_to_3d_box_sync(
        &mut self,
        ctx_id: u32,
        res_id: u32,
        x: u32,
        y: u32,
        w: u32,
        h: u32,
        offset: u64,
        stride: u32,
    ) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, ctx_id);
            w32(req_va + 24, x); // box.x
            w32(req_va + 28, y); // box.y
            w32(req_va + 32, 0); // box.z
            w32(req_va + 36, w);
            w32(req_va + 40, h);
            w32(req_va + 44, 1); // box.d
            w64(req_va + 48, offset); // offset into the backing
            w32(req_va + 56, res_id);
            w32(req_va + 60, 0); // level
            w32(req_va + 64, stride);
            w32(req_va + 68, 0); // layer_stride
        }
        self.ctrl.step(
            "TRANSFER_TO_3D",
            GPU_CTRL_HDR_LEN + 48,
            GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA,
        )
    }

    /// TRANSFER_TO/FROM_HOST_3D: fence-bearing by design -- a readback's
    /// completion (the data landed in the BO backing) is exactly what the
    /// client waits the fence for. Box + level + strides are the client's
    /// (section 2.1); `offset` is into the attached backing.
    #[allow(clippy::too_many_arguments)]
    pub fn transfer_3d(
        &mut self,
        to_host: bool,
        ctx_id: u32,
        ctx_pub: u32,
        res_id: u32,
        level: u32,
        x: u32,
        y: u32,
        z: u32,
        w: u32,
        h: u32,
        d: u32,
        offset: u64,
        stride: u32,
        layer_stride: u32,
    ) -> Result<u64, FencedErr> {
        let slot = self.fenced_begin(48)?;
        self.stage_transfer_3d(
            slot, to_host, ctx_id, res_id, level, x, y, z, w, h, d, offset, stride, layer_stride,
        );
        let fence_id = self.fence_next.wrapping_add(1);
        // A readback marks the lane (the sync-slot deadline reads it, C-6):
        // the device executes it synchronously at processing time.
        self.fenced_commit(slot, GPU_CTRL_HDR_LEN + 48, fence_id, ctx_pub, !to_host, false, 0)
    }

    /// The COMPOSITOR-OWNED fenced readback (Warp-C C-6, GPU-DESIGN 4.5.13):
    /// a full-frame TRANSFER_FROM_HOST_3D of a client's BO into its own
    /// backing, issued under the CLIENT's device ctx (the resource is
    /// attached there), on the reserved slot, tagged `comp` + `readback`
    /// against the client's seam ctx (`ctx_pub`, for the poison/vindication
    /// bookkeeping -- see `FenceTag`). The caller never waits: the fence
    /// pump routes the retire to `comp_readback_retired`. Level 0, origin
    /// box, depth 1, tight stride -- the exact request the synchronous
    /// `transfer_from_3d_sync` used to make on the console's dispatch, moved
    /// to a lane the console does not wait on.
    pub fn transfer_from_3d_comp(
        &mut self,
        ctx_id: u32,
        ctx_pub: u32,
        res_id: u32,
        w: u32,
        h: u32,
        stride: u32,
    ) -> Result<u64, FencedErr> {
        let slot = self.fenced_begin_comp()?;
        self.stage_transfer_3d(slot, false, ctx_id, res_id, 0, 0, 0, 0, w, h, 1, 0, stride, 0);
        let fence_id = self.fence_next.wrapping_add(1);
        self.fenced_commit(slot, GPU_CTRL_HDR_LEN + 48, fence_id, ctx_pub, true, true, 0)
    }

    /// Stage a TRANSFER_TO/FROM_HOST_3D request in fenced slot `slot`'s
    /// buffer (the fence id is written by `fenced_commit`'s caller order:
    /// the header carries `fence_next + 1`, which is what `fenced_commit`
    /// then records).
    #[allow(clippy::too_many_arguments)]
    fn stage_transfer_3d(
        &mut self,
        slot: usize,
        to_host: bool,
        ctx_id: u32,
        res_id: u32,
        level: u32,
        x: u32,
        y: u32,
        z: u32,
        w: u32,
        h: u32,
        d: u32,
        offset: u64,
        stride: u32,
        layer_stride: u32,
    ) {
        let req = self.ctrl.flane_va + (slot as u64) * FREQ_LEN;
        let fence_id = self.fence_next.wrapping_add(1);
        let cmd = if to_host {
            VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D
        } else {
            VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D
        };
        unsafe {
            write_ctrl_hdr_fenced(req, cmd, ctx_id, fence_id);
            w32(req + 24, x);
            w32(req + 28, y);
            w32(req + 32, z);
            w32(req + 36, w);
            w32(req + 40, h);
            w32(req + 44, d);
            w64(req + 48, offset);
            w32(req + 56, res_id);
            w32(req + 60, level);
            w32(req + 64, stride);
            w32(req + 68, layer_stride);
        };
    }

    /// Synchronous full-frame TRANSFER_FROM_HOST_3D under the COMPOSITOR's
    /// own authority (the Warp-4 composed-GL readback). Unlike the
    /// client-facing fenced `transfer_3d`, this rides the sync `.step`
    /// slot: the present dispatch that calls it must stay one synchronous
    /// unit (the I-40 premise), and the response IS the completion --
    /// virglrenderer copies the texture into the attached backing before
    /// answering. Level 0, origin box, depth 1, tight stride.
    pub fn transfer_from_3d_sync(
        &mut self,
        ctx_id: u32,
        res_id: u32,
        w: u32,
        h: u32,
        stride: u32,
    ) -> Result<(), Error> {
        self.transfer_from_3d_box_sync(ctx_id, res_id, 0, 0, w, h, 0, stride)
    }

    /// The RECT readback (Warp-C C-3): box (x, y, w, h) at level 0 into the
    /// attached backing at `offset` with row `stride`. The composed screen's
    /// pixel oracle (`probe-screen`) reads ONE texel this way, and the
    /// bring-up convention probe reads its few rows -- never a frame.
    #[allow(clippy::too_many_arguments)]
    pub fn transfer_from_3d_box_sync(
        &mut self,
        ctx_id: u32,
        res_id: u32,
        x: u32,
        y: u32,
        w: u32,
        h: u32,
        offset: u64,
        stride: u32,
    ) -> Result<(), Error> {
        let req_va = self.ring_va + REQ_OFF;
        unsafe {
            write_ctrl_hdr_ctx(req_va, VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, ctx_id);
            w32(req_va + 24, x); // box.x
            w32(req_va + 28, y); // box.y
            w32(req_va + 32, 0); // box.z
            w32(req_va + 36, w);
            w32(req_va + 40, h);
            w32(req_va + 44, 1); // box.d
            w64(req_va + 48, offset); // offset into the backing
            w32(req_va + 56, res_id);
            w32(req_va + 60, 0); // level
            w32(req_va + 64, stride);
            w32(req_va + 68, 0); // layer_stride
        }
        self.ctrl.step(
            "TRANSFER_FROM_3D",
            GPU_CTRL_HDR_LEN + 48,
            GPU_CTRL_HDR_LEN,
            VIRTIO_GPU_RESP_OK_NODATA,
        )
    }

    /// The largest command stream `submit_3d_sync` admits (Warp-C C-3): the
    /// compositor chunks a present's blit stream at this bound.
    pub fn sync_stream_max() -> usize {
        REQ_REGION_LEN as usize - GPU_CTRL_HDR_LEN as usize - 8
    }

    /// The serve loop's non-blocking completion pump (Warp-2d).
    pub fn poll_completions(&mut self) {
        self.ctrl.poll_completions();
    }

    /// Take the drained fence completions (the server posts each on its
    /// owning seam ctx).
    pub fn take_completions(&mut self) -> alloc::vec::Vec<FenceTag> {
        core::mem::take(&mut self.ctrl.completed)
    }

    /// Take the contexts a late retire vindicated (round-3 F2).
    pub fn take_vindications(&mut self) -> alloc::vec::Vec<FenceVindication> {
        core::mem::take(&mut self.ctrl.vindicated)
    }

    /// #175, the harness levers -- both scoped to ONE ctx (#178), because
    /// they ship (`default = ["test-mode"]`) on a mode-0666 ctl and
    /// identity cannot separate the prover from an attacker: the in-guest
    /// battery is an ordinary uid-1000 client by design. Scoped, the worst
    /// a client can do is wedge its own ctx, which it could already do.
    ///
    /// `hold` keeps THIS ctx's submitted fences in flight so abandonment is
    /// reachable without racing the drain; `abandon` drives the production
    /// `abandon_matching` -- the same poison path the 30 s deadline drives,
    /// forced rather than shortened, so the test never races a real clock.
    /// `abandoned_total` lets a prover prove its own trigger fired instead
    /// of asserting against an untouched, healthy ctx.
    #[cfg(feature = "test-mode")]
    pub fn test_hold_ctx(&mut self, ctx_pub: Option<u32>) {
        self.ctrl.hold_ctx = ctx_pub;
        if ctx_pub.is_some() {
            return;
        }
        // Release replays every swallowed retire through the SAME branch
        // `drain` would have taken, so the healing leg exercises the real
        // recovery rather than a test-only shortcut. Which branch depends
        // on whether an abandon intervened: a live tag completes normally,
        // a taken one leaves the slot poisoned and owes a vindication.
        for slot in core::mem::take(&mut self.ctrl.held_retires) {
            match self.ctrl.fslots[slot].take() {
                Some(mut tag) => {
                    let resp_va =
                        self.ctrl.flane_va + FRESP_OFF + (slot as u64) * FRESP_STRIDE;
                    let rt = unsafe { r32(resp_va) };
                    // Mirror `drain`'s arm EXACTLY -- including the round-F2
                    // verdict. A replay that composed where the real path
                    // would not is a test lever that proves the wrong thing.
                    tag.ok = rt == VIRTIO_GPU_RESP_OK_NODATA;
                    if !tag.ok {
                        say!(
                            "tapestryd: gpu fenced cmd (fence {}) resp_type={:#x}",
                            tag.fence_id, rt
                        );
                    }
                    self.ctrl.completed.push(tag);
                }
                None if self.ctrl.fslot_poisoned[slot] => {
                    self.ctrl.fslot_poisoned[slot] = false;
                    self.ctrl.vindicated.push(FenceVindication {
                        ctx_pub: self.ctrl.fslot_poison_ctx[slot],
                        comp: slot == COMP_FSLOT,
                        ring_idx: self.ctrl.fslot_poison_ring[slot],
                    });
                }
                None => {}
            }
        }
    }

    /// Which ctx currently holds, if any (round-8 F1). `hold_ctx` is ONE
    /// slot, so "scoped to the caller" bounded the hold's EFFECT without
    /// bounding its STORAGE: a second client's arm silently displaced the
    /// first's and orphaned its deferred retires. The caller uses this to
    /// refuse a displacing arm outright, which keeps at most one ctx held
    /// and makes the departing-holder release always sufficient.
    #[cfg(feature = "test-mode")]
    pub fn test_hold_ctx_current(&self) -> Option<u32> {
        self.ctrl.hold_ctx
    }

    #[cfg(feature = "test-mode")]
    pub fn test_abandon_ctx(&mut self, ctx_pub: u32) {
        self.ctrl
            .abandon_matching("test-mode abandon", Some(ctx_pub));
    }

    /// Drop a hold whose ctx just died (#178). Without this the scope
    /// survives its subject: `hold_ctx` would keep naming a dead pub_id
    /// and, after a 2^32 `warp_ctx_seq` wrap, could re-attach to an
    /// unrelated ctx. Releasing here also replays whatever that ctx had
    /// deferred, so nothing is stranded.
    #[cfg(feature = "test-mode")]
    pub fn test_hold_ctx_died(&mut self, ctx_pub: u32) {
        if self.ctrl.hold_ctx == Some(ctx_pub) {
            self.test_hold_ctx(None);
        }
    }

    #[cfg(feature = "test-mode")]
    pub fn test_abandoned_total(&self) -> u32 {
        self.ctrl.abandoned_total
    }

    /// Fenced slots currently allocatable. Read-only, and the ONLY thing that
    /// can tell a departed client's slots came back from a client that
    /// silently stranded them: `ctxs` deliberately excludes `retiring`
    /// contexts, so a ctx wedged forever and a ctx that finished cleanly read
    /// identically there -- round-5 F5's trap, one level down.
    #[cfg(feature = "test-mode")]
    /// W-4 stall observability (delegate; the slots live on Controlq).
    pub fn fenced_held(&self) -> alloc::vec::Vec<(usize, u64, u32, u8, bool, bool, u64)> {
        self.ctrl.fenced_held()
    }

    pub fn test_fenced_free(&self) -> u32 {
        // The CLIENT pool (C-6): the reserved slot is never a client's, so a
        // compositor readback in flight must not read as a client's slot
        // stranded -- the relative comparisons this feeds would go red on an
        // unrelated surface's present.
        (0..COMP_FSLOT)
            .filter(|&i| self.ctrl.fslots[i].is_none() && !self.ctrl.fslot_poisoned[i])
            .count() as u32
    }

    /// The reserved compositor slot's state (C-6 census): 0 free, 1 busy (a
    /// compositor readback in flight), 2 poisoned (one was abandoned and the
    /// device has not yet proved it finished).
    pub fn comp_slot_state(&self) -> u32 {
        if self.ctrl.fslots[COMP_FSLOT].is_some() {
            1
        } else if self.ctrl.fslot_poisoned[COMP_FSLOT] {
            2
        } else {
            0
        }
    }

    /// Fenced chains this ctx still has in flight. Exposed because the only
    /// other way to watch a fence land is to READ the fence fd, and that read
    /// PARKS -- so a regression in the per-ctx scoping would hang the prover
    /// and time the boot out, rather than failing with a message. A bounded
    /// poll over this is the same shape as the existing vindication wait.
    /// Un-gated at Warp-3 alongside the ctl promotion: the winsys throttles
    /// on it, so it is production surface now.
    pub fn ctx_fences_in_flight(&self, ctx_pub: u32) -> u32 {
        self.ctrl
            .fslots
            .iter()
            .flatten()
            .filter(|t| t.ctx_pub == ctx_pub)
            .count() as u32
    }

    /// Does this ctx still own a POISONED slot? A vindication is only
    /// honest once the answer is no (round-4 F1: one ctx may abandon
    /// several chains, and one late retire proves nothing about the rest).
    pub fn ctx_has_poisoned_slot(&self, ctx_pub: u32) -> bool {
        (0..FENCED_SLOTS)
            .any(|i| self.ctrl.fslot_poisoned[i] && self.ctrl.fslot_poison_ctx[i] == ctx_pub)
    }

    pub fn fenced_in_flight(&self) -> u32 {
        self.ctrl.fenced_in_flight()
    }

    pub fn engine_dead(&self) -> bool {
        self.ctrl.dead
    }
}
