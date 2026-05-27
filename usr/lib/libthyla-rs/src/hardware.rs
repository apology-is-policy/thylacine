// t::hardware -- typed RAII wrappers over the KObj_MMIO / KObj_IRQ /
// KObj_DMA kernel surfaces. The substrate for every Thylacine-native
// device driver (the virtio-* family today; future native PCI / USB /
// SDIO drivers tomorrow).
//
// Lifted at U-2h-hardware. The bare SVC wrappers (`t_mmio_create`,
// `t_mmio_map`, `t_irq_create`, `t_irq_wait`, `t_dma_create`,
// `t_dma_map`) remain exported -- drivers that prefer the low-level
// shape can continue using them. The typed wrappers exist to give
// authored Thylacine drivers an idiomatic Rust API: RAII drop, type-
// system-enforced non-transferability (I-5), and safe-ish accessors
// over the mapped user VAs.
//
// DESIGN DECISIONS
//
// Combined create + map. Every existing consumer calls create
// immediately followed by map at a chosen user VA; no consumer holds
// an unmapped handle. `Mmio::new` / `Dma::new` collapse the two steps
// so the constructor returns a fully-usable typed object. If a future
// driver needs the split (e.g., to remap after a different policy
// chooses the VA), we add `create_unmapped` / `map` later; the
// combined form covers v1.0 needs.
//
// Drop closes the handle. SYS_CLOSE on a KObj_MMIO / KObj_IRQ /
// KObj_DMA handle releases the kernel's per-handle refcount. The
// Burrow wrapping the mapping holds an INDEPENDENT refcount on the
// underlying KObj (see `kernel/burrow.c::burrow_free_internal`); the
// user VA mapping survives SYS_CLOSE until the proc's pgtable is
// torn down at exit OR (post-handle-close) until a future
// SYS_BURROW_DETACH on the mapping window. In practice every native
// driver creates these handles once at startup and never closes, so
// the Drop only fires when the binary exits, where its effect is a
// no-op (proc_free is about to release everything anyway). The RAII
// shape exists for code clarity, not for runtime cleanup semantics.
//
// NON-TRANSFERABILITY (I-5) is preserved by NOT impl-ing any future
// `Transfer` trait on these types. The kernel statically rejects
// SYS_TRANSFER for KOBJ_MMIO/KOBJ_IRQ/KOBJ_DMA (per
// kernel/handle.c handle_acquire_obj); the type system makes the same
// invariant visible at the Rust level by not providing the operation.
//
// Volatile MMIO accesses. `Mmio::read_u32` / `write_u32` use
// `core::ptr::read_volatile` / `write_volatile` so the compiler does
// not coalesce or reorder them. The kernel-installed PTE carries the
// MAIR_IDX_DEVICE (nGnRnE) attribute so the hardware ALSO does not
// reorder; both layers are needed.
//
// Bounds + alignment checks. read_u32 / write_u32 assert offset + 4
// <= len AND offset % 4 == 0 at runtime; a misuse panics via
// libthyla-rs's panic_handler (which tail-calls t_exits(1)). The
// kernel-side ABI also rejects misaligned MMIO accesses
// (architecturally a fault); the runtime check catches the bug at
// the userspace boundary before the device sees a malformed cycle.
//
// DMA buffers are normal memory. `Dma::as_slice` / `as_slice_mut`
// return ordinary `&[u8]` / `&mut [u8]` views. The caller is
// responsible for memory barriers when interacting with a device
// (see the `virtio_rmb` helper exported from this crate for the
// load-acquire barrier).
//
// Send + Sync. Mmio/Irq/Dma are NOT Send + NOT Sync by default
// (raw-pointer fields make them !Send + !Sync automatically). A
// multi-thread driver that wants to share an Mmio across threads
// wraps it in `Arc<Mutex<_>>` -- but the VA mapping IS shared across
// threads in the same Proc (threads share pgtable_root + ASID), so
// `unsafe impl Send + Sync` could be safely added in a future
// version once we have a real multi-thread driver consumer.

use core::sync::atomic::{compiler_fence, Ordering};

use crate::err::{Error, Result};
use crate::handle::{Handle, Rights};
use crate::poll::AsFd;
use crate::{
    t_dma_create, t_dma_map, t_irq_create, t_irq_wait, t_mmio_create, t_mmio_map,
};

// =============================================================================
// Error mapping helper.
// =============================================================================
//
// The HW-handle syscalls (SYS_MMIO_CREATE / SYS_MMIO_MAP /
// SYS_IRQ_CREATE / SYS_IRQ_WAIT / SYS_DMA_CREATE / SYS_DMA_MAP) return
// -1 as a flat error sentinel; the kernel does not discriminate among
// cap-missing / out-of-range / overlap / page-misaligned / IPS-bound /
// not-found. Map -1 to `InvalidArgument` as the closest catch-all
// (the request was structurally rejected). A future kernel ABI that
// returns proper -errno values routes through `from_syscall_return`
// for richer variants.
#[inline]
fn hw_error(rc: i64) -> Error {
    if rc == -1 {
        return Error::InvalidArgument;
    }
    // Defense in depth: if the kernel grows -errno returns later, the
    // typed wrapper picks up the right variant. Today the path below
    // is unreachable (every syscall returns either rc >= 0 or -1).
    Error::from_syscall_return(rc).err().unwrap_or(Error::Io)
}

// =============================================================================
// Mmio -- typed MMIO bank.
// =============================================================================

/// A claimed + mapped MMIO bank.
///
/// Created by [`Mmio::new`]; the constructor combines `SYS_MMIO_CREATE`
/// and `SYS_MMIO_MAP` so the returned object is fully usable for
/// register access via [`Mmio::read_u32`] / [`Mmio::write_u32`].
///
/// Lifetime: `Drop` closes the handle. The user VA mapping survives
/// the close (the kernel-side Burrow holds an independent ref on the
/// KObj_MMIO; the PTEs are not torn down until proc exit). In
/// practice every consumer holds an `Mmio` for the lifetime of the
/// process and lets proc_free do the cleanup.
///
/// Non-transferable per invariant I-5: this type has no `Transfer`
/// impl and the kernel rejects `SYS_TRANSFER` on `KOBJ_MMIO` at the
/// syscall layer.
pub struct Mmio {
    #[allow(dead_code)] // Drop fires on the handle.
    handle: Handle,
    base_va: *mut u8,
    len: usize,
}

impl Mmio {
    /// Claim the PA range `[pa, pa + size)` and install user-VA
    /// mappings at `vaddr`.
    ///
    /// `pa` + `size` must be page-aligned + within the IPS bound (40
    /// bits at v1.0); `vaddr` must be 4-KiB aligned; `prot` must be
    /// non-zero R / R+W (no EXEC on device memory, no W-without-R).
    /// The kernel enforces every constraint and returns -1 on any
    /// rejection.
    ///
    /// Required capability: `CAP_HW_CREATE`. Required rights:
    /// `Rights::READ` or `Rights::WRITE` (plus `Rights::MAP` -- the
    /// kernel `SYS_MMIO_MAP` handler checks for MAP on the handle).
    ///
    /// On error, returns `Err(Error::InvalidArgument)` -- the bare
    /// syscall reports -1 without errno discrimination.
    ///
    /// # Safety
    ///
    /// The caller MUST assert:
    /// 1. `pa..pa+size` is a real device range (not RAM owned by the
    ///    kernel or another driver). Misusing this can wedge the
    ///    system or hand the caller a pointer to RAM with device
    ///    memory attributes (uncached, non-gathering).
    /// 2. `vaddr` is a user-VA region the caller has chosen and is
    ///    not currently mapped.
    ///
    /// The kernel does the necessary checks for both layers (the PA
    /// claim table catches kernel-reserved + overlapping ranges; the
    /// pgtable installation rejects already-mapped VAs), but only
    /// AFTER trusting that the caller knows what device it's poking.
    pub unsafe fn new(
        pa: u64,
        size: usize,
        rights: Rights,
        vaddr: u64,
        prot: u32,
    ) -> Result<Self> {
        let rc_create = t_mmio_create(pa, size as u64, rights.bits());
        if rc_create < 0 {
            return Err(hw_error(rc_create));
        }
        let handle = Handle::from_raw(rc_create as i32, rights);

        let rc_map = t_mmio_map(rc_create, vaddr, prot);
        if rc_map < 0 {
            // Drop closes the handle; the kernel releases the PA
            // claim. base_va is never populated.
            drop(handle);
            return Err(hw_error(rc_map));
        }

        Ok(Self {
            handle,
            base_va: vaddr as *mut u8,
            len: size,
        })
    }

    /// User VA where the bank is mapped. The pointer is valid for
    /// `len()` bytes; bounded by `&self`.
    #[inline]
    pub const fn base_va(&self) -> *mut u8 {
        self.base_va
    }

    /// Bytes in the mapped bank.
    #[inline]
    pub const fn len(&self) -> usize {
        self.len
    }

    /// True iff the bank is zero-sized. Unreachable in practice
    /// (`SYS_MMIO_CREATE` rejects `size == 0`); present for the
    /// clippy::len_without_is_empty lint.
    #[inline]
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Volatile 32-bit register read at `offset`.
    ///
    /// Panics if `offset + 4 > len()` or `offset % 4 != 0`. Both
    /// conditions indicate a driver bug (the device-register table is
    /// fixed); a panic here surfaces the bug at userspace before the
    /// device sees a misaligned cycle (architectural fault).
    ///
    /// The access is volatile so the compiler does not coalesce or
    /// reorder it; the kernel-installed PTE carries MAIR_IDX_DEVICE
    /// so the hardware also does not reorder.
    #[inline]
    #[must_use]
    pub fn read_u32(&self, offset: usize) -> u32 {
        assert!(offset + 4 <= self.len, "Mmio::read_u32 OOB");
        assert!(offset % 4 == 0, "Mmio::read_u32 misaligned");
        // SAFETY: bounds + alignment asserted above; base_va is a
        // valid mapped user VA per the constructor's invariant.
        unsafe { core::ptr::read_volatile(self.base_va.add(offset) as *const u32) }
    }

    /// Volatile 32-bit register write at `offset`.
    ///
    /// Same bounds + alignment requirements as [`read_u32`]; same
    /// panic semantics on misuse.
    #[inline]
    pub fn write_u32(&self, offset: usize, value: u32) {
        assert!(offset + 4 <= self.len, "Mmio::write_u32 OOB");
        assert!(offset % 4 == 0, "Mmio::write_u32 misaligned");
        // SAFETY: bounds + alignment asserted above; base_va is a
        // valid mapped user VA per the constructor's invariant.
        unsafe { core::ptr::write_volatile(self.base_va.add(offset) as *mut u32, value) }
    }
}

// =============================================================================
// Irq -- typed IRQ handle.
// =============================================================================

/// A claimed IRQ line. Created by [`Irq::new`]; the kernel forwards
/// matching GIC dispatches to the handle's per-Proc pending counter.
///
/// Block on [`Irq::wait`] to consume one or more pending IRQs.
///
/// Composes with `t::poll::PollSet` via the [`AsFd`] impl.
///
/// Non-transferable per invariant I-5.
pub struct Irq {
    handle: Handle,
    intid: u32,
}

impl Irq {
    /// Claim the GIC SPI `intid` for this Proc.
    ///
    /// `intid` MUST be in the SPI range (32..1019); the kernel
    /// rejects SGI / PPI (already-claimed by the kernel itself).
    ///
    /// Required capability: `CAP_HW_CREATE`. Required rights:
    /// `Rights::SIGNAL` (the kernel `SYS_IRQ_WAIT` handler checks for
    /// SIGNAL on the handle).
    pub fn new(intid: u32, rights: Rights) -> Result<Self> {
        let rc = unsafe { t_irq_create(intid, rights.bits()) };
        if rc < 0 {
            return Err(hw_error(rc));
        }
        Ok(Self {
            handle: Handle::from_raw(rc as i32, rights),
            intid,
        })
    }

    /// The GIC INTID claimed at construction.
    #[inline]
    #[must_use]
    pub const fn intid(&self) -> u32 {
        self.intid
    }

    /// Block until at least one IRQ is pending; return the collapsed
    /// pending-count consumed.
    ///
    /// Edge-triggered semantics: multiple GIC dispatches while the
    /// waiter is blocked collapse into one wake, but the returned
    /// count reflects the count seen at wake time. The kernel
    /// atomically reads-and-clears the counter under the rendez lock,
    /// so no IRQ is dropped between consume and the next dispatch.
    pub fn wait(&self) -> Result<u32> {
        let rc = unsafe { t_irq_wait(self.handle.raw() as i64) };
        if rc < 0 {
            return Err(hw_error(rc));
        }
        Ok(rc as u32)
    }
}

impl AsFd for Irq {
    /// Returns the kernel handle index. Suitable for direct use with
    /// `t::poll::PollSet` -- when the IRQ has a pending count >= 1,
    /// the poll surface reports the fd as readable.
    #[inline]
    fn as_raw_fd(&self) -> i32 {
        self.handle.raw()
    }
}

// =============================================================================
// Dma -- typed contiguous DMA buffer.
// =============================================================================

/// A kernel-allocated contiguous DMA buffer mapped into user VA at
/// `base_va` with PA `paddr`. The PA is what the caller embeds in
/// device-visible descriptors (VirtIO virtqueue rings, etc.).
///
/// Created by [`Dma::new`]; the constructor combines `SYS_DMA_CREATE`
/// (kernel allocates + pins the underlying buddy chunk) and
/// `SYS_DMA_MAP` (installs user-VA mappings and returns the PA).
///
/// Backed by normal memory (NOT volatile, NOT device-attribute). Use
/// [`Dma::as_slice`] / [`Dma::as_slice_mut`] for byte-level access,
/// or [`Dma::read_u32`] / [`Dma::write_u32`] for aligned 32-bit
/// access. Memory barriers (`virtio_rmb` / `virtio_wmb`) are the
/// caller's responsibility because their placement depends on the
/// device's ordering semantics.
///
/// Non-transferable per invariant I-5.
pub struct Dma {
    #[allow(dead_code)] // Drop fires on the handle.
    handle: Handle,
    base_va: *mut u8,
    len: usize,
    paddr: u64,
}

impl Dma {
    /// Allocate a contiguous DMA buffer of `size` bytes (rounded up to
    /// the next 4-KiB boundary; max 1 MiB at v1.0) and install user-VA
    /// mappings at `vaddr`.
    ///
    /// `vaddr` must be 4-KiB aligned; `prot` must be non-zero R / R+W
    /// (no EXEC, no W-without-R).
    ///
    /// Required capability: `CAP_HW_CREATE`. Required rights:
    /// `Rights::READ` + `Rights::WRITE` + `Rights::MAP` (the kernel
    /// `SYS_DMA_MAP` handler checks for MAP on the handle).
    ///
    /// # Safety
    ///
    /// The caller MUST assert that `vaddr` is a user-VA region not
    /// currently mapped. The kernel rejects collisions, but only
    /// after trusting the caller chose a reasonable target.
    pub unsafe fn new(
        size: usize,
        rights: Rights,
        vaddr: u64,
        prot: u32,
    ) -> Result<Self> {
        let rc_create = t_dma_create(size as u64, rights.bits());
        if rc_create < 0 {
            return Err(hw_error(rc_create));
        }
        let handle = Handle::from_raw(rc_create as i32, rights);

        let rc_map = t_dma_map(rc_create, vaddr, prot);
        if rc_map < 0 {
            drop(handle);
            return Err(hw_error(rc_map));
        }
        // rc_map is the PA on success (always non-negative; PAs fit
        // in 40 bits at v1.0). i64 -> u64 is safe because rc_map >= 0.
        let paddr = rc_map as u64;

        Ok(Self {
            handle,
            base_va: vaddr as *mut u8,
            len: size,
            paddr,
        })
    }

    /// User VA where the buffer is mapped.
    #[inline]
    pub const fn base_va(&self) -> *mut u8 {
        self.base_va
    }

    /// Bytes in the mapped buffer.
    #[inline]
    pub const fn len(&self) -> usize {
        self.len
    }

    /// True iff the buffer is zero-sized (unreachable -- the kernel
    /// rejects `size == 0` at create time).
    #[inline]
    pub const fn is_empty(&self) -> bool {
        self.len == 0
    }

    /// Physical address of the buffer. Embed in device-visible
    /// descriptors (e.g., a VirtIO virtqueue desc.addr field).
    /// Stable for the lifetime of `self`.
    #[inline]
    pub const fn paddr(&self) -> u64 {
        self.paddr
    }

    /// Byte-level read view.
    #[inline]
    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        // SAFETY: base_va is a valid mapped user VA for `len` bytes
        // (constructor invariant); lifetime bound to `&self`.
        unsafe { core::slice::from_raw_parts(self.base_va, self.len) }
    }

    /// Byte-level mutable view.
    ///
    /// Multiple `&mut Dma` cannot exist concurrently (Rust borrow
    /// rules); the resulting `&mut [u8]` is exclusive for the
    /// borrow's lifetime.
    #[inline]
    #[must_use]
    pub fn as_slice_mut(&mut self) -> &mut [u8] {
        // SAFETY: as_slice rationale; `&mut self` ensures exclusive
        // access to the buffer for the borrow's lifetime.
        unsafe { core::slice::from_raw_parts_mut(self.base_va, self.len) }
    }

    /// 32-bit read at `offset` (NON-volatile -- DMA buffers are
    /// normal memory; the device sees writes once the appropriate
    /// memory barrier fires).
    ///
    /// Panics on `offset + 4 > len()` or `offset % 4 != 0`.
    ///
    /// Inserts a compiler fence so the read isn't reordered around
    /// neighbouring barrier-bearing code. The HARDWARE barrier
    /// (`dmb ishld` via `virtio_rmb`) is still the caller's
    /// responsibility for cross-device-CPU visibility.
    #[inline]
    #[must_use]
    pub fn read_u32(&self, offset: usize) -> u32 {
        assert!(offset + 4 <= self.len, "Dma::read_u32 OOB");
        assert!(offset % 4 == 0, "Dma::read_u32 misaligned");
        compiler_fence(Ordering::Acquire);
        // SAFETY: bounds + alignment asserted; base_va valid.
        unsafe { core::ptr::read_volatile(self.base_va.add(offset) as *const u32) }
    }

    /// 32-bit write at `offset` (NON-volatile from the device's
    /// perspective; caller emits the appropriate memory barrier
    /// before the device-visible notify).
    ///
    /// Panics on `offset + 4 > len()` or `offset % 4 != 0`.
    #[inline]
    pub fn write_u32(&mut self, offset: usize, value: u32) {
        assert!(offset + 4 <= self.len, "Dma::write_u32 OOB");
        assert!(offset % 4 == 0, "Dma::write_u32 misaligned");
        // SAFETY: bounds + alignment asserted; base_va valid;
        // `&mut self` ensures exclusive access.
        unsafe { core::ptr::write_volatile(self.base_va.add(offset) as *mut u32, value) };
        compiler_fence(Ordering::Release);
    }
}
