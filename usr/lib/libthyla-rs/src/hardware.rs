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
// torn down at exit OR until SYS_BURROW_DETACH removes it (a hardware
// map is detachable wherever it was placed, ARCH 6.5). In practice every native
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
// MMIO accesses. `Mmio::read_u32` / `write_u32` funnel through the
// ISV-safe `mmio_read32` / `mmio_write32` primitives (single-instruction
// `ldr`/`str` via inline asm) so the compiler does not coalesce or
// reorder them AND the emitted instruction is hypervisor-decodable
// (ESR.ISV=1 -- see the primitives' note; #890). The kernel-installed
// PTE carries the MAIR_IDX_DEVICE (nGnRnE) attribute so the hardware
// ALSO does not reorder; all layers are needed.
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
use crate::{
    t_burrow_detach, t_burrow_from_hostmem, t_dma_create, t_dma_map, t_irq_create, t_irq_wait, t_irq_wait_timeout,
    t_mmio_create, t_mmio_map, t_pci_claim, t_pci_info, t_pci_map_window, t_pci_windows, TPciInfo, TPciWindow, T_PCI_WINDOW_MAX, T_PROT_READ,
    T_PROT_WRITE,
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
// ISV-safe device-MMIO primitives -- the single MMIO accessor in the tree.
// =============================================================================
//
// Every device-MMIO access -- the typed `Mmio` methods below AND every
// virtio-* driver's local register helpers -- funnels through these.
// Each emits a single general-register, non-writeback, base-only
// `ldr`/`str`; the register offset is pre-applied to `addr` in Rust, so
// the instruction carries no displacement.
//
// WHY the inline asm rather than `read_volatile`/`write_volatile`: a
// plain volatile access is functionally correct but lets LLVM choose the
// addressing mode. When the `#[inline(always)]` driver helpers fold into
// a caller that pokes several nearby registers off one base, LLVM emits
// a PRE-INDEXED WRITEBACK (`str w, [x, #imm]!`) and unscaled `stur`/
// `ldur`. On a stage-2 / MMIO abort the ARM ARM leaves ESR_EL1.ISS
// UNKNOWN for those forms -- ESR_EL1.ISV (Instruction Syndrome Valid) is
// 0 -- so a hypervisor cannot reconstruct the emulated access (size +
// target register + direction) and QEMU's HVF backend `assert(isv)`s
// (hvf.c). `read_volatile` constrains elision + ordering, NOT the
// addressing mode, so the volatile contract holds while the instruction
// is still undecodable. A bare `ldr/str {reg}, [{base}]` cannot become
// writeback / pre-post-index / unscaled / paired / SIMD, so ISV is
// always 1. (#890; PORTABILITY.md section 8.)
//
// Reads are deliberately NOT `readonly`/`pure`: a device read is
// side-effecting (it can pop a FIFO or clear a latch). The default
// memory-clobbering asm options preserve every volatile guarantee and
// add a compiler barrier -- strictly stronger than `read_volatile`. The
// `dsb`/`dmb` ordering barriers stay the caller's responsibility.

/// Single-instruction 32-bit MMIO load (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped, 4-byte-aligned device-MMIO address.
#[inline]
pub unsafe fn mmio_read32(addr: u64) -> u32 {
    let v: u32;
    core::arch::asm!("ldr {v:w}, [{a}]", v = out(reg) v, a = in(reg) addr,
                     options(nostack, preserves_flags));
    v
}

/// Single-instruction 32-bit MMIO store (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped, 4-byte-aligned device-MMIO address.
#[inline]
pub unsafe fn mmio_write32(addr: u64, value: u32) {
    core::arch::asm!("str {v:w}, [{a}]", v = in(reg) value, a = in(reg) addr,
                     options(nostack, preserves_flags));
}

/// Single-instruction 16-bit MMIO load (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped, 2-byte-aligned device-MMIO address.
#[inline]
pub unsafe fn mmio_read16(addr: u64) -> u16 {
    let v: u32;
    core::arch::asm!("ldrh {v:w}, [{a}]", v = out(reg) v, a = in(reg) addr,
                     options(nostack, preserves_flags));
    v as u16
}

/// Single-instruction 16-bit MMIO store (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped, 2-byte-aligned device-MMIO address.
#[inline]
pub unsafe fn mmio_write16(addr: u64, value: u16) {
    core::arch::asm!("strh {v:w}, [{a}]", v = in(reg) u32::from(value), a = in(reg) addr,
                     options(nostack, preserves_flags));
}

/// Single-instruction 64-bit MMIO load (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped, 8-byte-aligned device-MMIO address.
#[inline]
pub unsafe fn mmio_read64(addr: u64) -> u64 {
    let v: u64;
    core::arch::asm!("ldr {v:x}, [{a}]", v = out(reg) v, a = in(reg) addr,
                     options(nostack, preserves_flags));
    v
}

/// Single-instruction 64-bit MMIO store (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped, 8-byte-aligned device-MMIO address.
#[inline]
pub unsafe fn mmio_write64(addr: u64, value: u64) {
    core::arch::asm!("str {v:x}, [{a}]", v = in(reg) value, a = in(reg) addr,
                     options(nostack, preserves_flags));
}

/// Single-instruction 8-bit MMIO load (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped device-MMIO address.
#[inline]
pub unsafe fn mmio_read8(addr: u64) -> u8 {
    let v: u32;
    core::arch::asm!("ldrb {v:w}, [{a}]", v = out(reg) v, a = in(reg) addr,
                     options(nostack, preserves_flags));
    v as u8
}

/// Single-instruction 8-bit MMIO store (ISV=1). `addr` is the final VA.
///
/// # Safety
/// `addr` must be a mapped device-MMIO address.
#[inline]
pub unsafe fn mmio_write8(addr: u64, value: u8) {
    core::arch::asm!("strb {v:w}, [{a}]", v = in(reg) u32::from(value), a = in(reg) addr,
                     options(nostack, preserves_flags));
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
        unsafe { mmio_read32(self.base_va.add(offset) as u64) }
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
        unsafe { mmio_write32(self.base_va.add(offset) as u64, value) }
    }
}

// =============================================================================
// Irq -- typed IRQ handle.
// =============================================================================

/// A claimed IRQ line. Created by [`Irq::new`]; the kernel forwards
/// matching GIC dispatches to the handle's per-Proc pending counter.
///
/// Block on [`Irq::wait`] to consume one or more pending IRQs. An `Irq` is
/// deliberately NOT pollable: the kernel `poll(2)` has no KOBJ_IRQ readiness
/// arm (it returns POLLNVAL), so there is no `AsFd` impl and passing an `Irq`
/// to a `PollSet` is a compile error. A caller that wants to multiplex the IRQ
/// against fds runs `wait()` on a dedicated thread and signals its poll loop.
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
    /// Multiple GIC dispatches collapse into one wake; the returned count
    /// reflects dispatches consumed atomically under the kernel rendez lock.
    /// Trigger mode comes from the device's DTB description. For a level line,
    /// dispatch masks the interrupt and this call re-enables it: acknowledge
    /// the device (including completion of its MMIO access) BEFORE waiting
    /// again. Merely waking another thread to acknowledge later can cause an
    /// interrupt storm that starves that thread. Used-ring state, not the
    /// collapsed count, determines which device completions to consume.
    pub fn wait(&self) -> Result<u32> {
        let rc = unsafe { t_irq_wait(self.handle.raw() as i64) };
        if rc < 0 {
            return Err(hw_error(rc));
        }
        Ok(rc as u32)
    }

    /// Like [`wait`](Self::wait), bounded by `timeout_ns` relative nanoseconds
    /// (0 == wait forever). On a timeout the returned count is `0` -- the same
    /// value a death-interrupt returns, and treated the same way: no IRQ is
    /// pending, so the caller re-checks the device (e.g. the virtqueue used
    /// ring) and continues, or unwinds. F-A1 (C): a bounded wait turns a lost
    /// completion on a level line into a said, recoverable event instead of a
    /// silent forever-hang; a nonzero return is a genuine collapsed IRQ count.
    pub fn wait_timeout(&self, timeout_ns: u64) -> Result<u32> {
        let rc = unsafe { t_irq_wait_timeout(self.handle.raw() as i64, timeout_ns) };
        if rc < 0 {
            return Err(hw_error(rc));
        }
        Ok(rc as u32)
    }
}

/// A function-bound PCI interrupt. Unlike `Irq`, WAIT never re-arms it.
/// No AsFd: endpoint poll registration has not been implemented.
pub struct PciIrq { handle: Handle, mode: PciIrqMode, vector: u16 }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PciIrqMode { Intx = 1, Msix = 2 }
impl PciIrq {
    pub fn new(pci: &PciDev, mode: PciIrqMode) -> Result<Self> {
        let raw = unsafe { crate::t_pci_irq_create(pci.handle.raw() as i64, mode as u32, 0) };
        if raw < 0 { return Err(hw_error(raw)); }
        let handle = unsafe { Handle::from_raw(raw as i32, Rights::READ | Rights::WRITE | Rights::SIGNAL) };
        let mut info = crate::TPciIrqInfo::default();
        Error::from_syscall_return(unsafe { crate::t_pci_irq_info(handle.raw() as i64, &mut info) })?;
        if info.mode != mode as u32 || (mode == PciIrqMode::Msix && info.table_index >= 0xffff) {
            return Err(Error::Io);
        }
        Ok(Self { handle, mode, vector: if mode == PciIrqMode::Msix { info.table_index as u16 } else { 0xffff } })
    }
    pub fn mode(&self) -> PciIrqMode { self.mode }
    pub fn vector(&self) -> u16 { self.vector }

    /// Initialize interrupt routing before allocating/publishing queues or DMA.
    /// This owns the reset and vector-selection transaction; the caller then
    /// starts ACKNOWLEDGE/DRIVER and must not reset away the selected vectors.
    /// PCI_IRQ_MODE=intx/msix forces a mode; unset/auto prefers MSI-X with full
    /// rollback to INTx. Both config and every listed queue are read back.
    pub fn for_virtio(pci: &PciDev, queues: &[u16]) -> Result<Self> {
        let (common, len) = pci.region(PciRegion::Common).ok_or(Error::InvalidArgument)?;
        if len < 56 || common & 7 != 0 { return Err(Error::InvalidArgument); }
        let preference = crate::env::var("PCI_IRQ_MODE");
        let force = match preference.as_deref() {
            None | Some("auto") => None,
            Some("intx") => Some(PciIrqMode::Intx),
            Some("msix") => Some(PciIrqMode::Msix),
            _ => return Err(Error::InvalidArgument),
        };
        Self::reset_virtio(common)?;
        if force != Some(PciIrqMode::Intx) {
            let attempt = Self::new(pci, PciIrqMode::Msix);
            match attempt {
                Ok(irq) => {
                    if irq.select_virtio_vectors(common, queues).is_ok() { return Ok(irq); }
                    // No queue/DMA has been published. Undo every selection
                    // before closing the masked endpoint and changing mode.
                    Self::reset_virtio(common)?;
                    drop(irq);
                    if force == Some(PciIrqMode::Msix) { return Err(Error::Io); }
                }
                Err(error) => {
                    if force == Some(PciIrqMode::Msix) { return Err(error); }
                    Self::reset_virtio(common)?;
                }
            }
        }
        let irq = Self::new(pci, PciIrqMode::Intx)?;
        irq.select_virtio_vectors(common, queues)?;
        Ok(irq)
    }
    fn reset_virtio(common: u64) -> Result<()> {
        unsafe { mmio_write8(common + 20, 0); core::arch::asm!("dsb sy", options(nostack)); }
        let deadline = crate::time::monotonic_ns().saturating_add(10_000_000);
        // The iteration cap also bounds a malfunctioning monotonic source.
        for _ in 0..1_000_000 {
            if unsafe { mmio_read8(common + 20) } == 0 { return Ok(()); }
            if crate::time::monotonic_ns() >= deadline { break; }
            core::hint::spin_loop();
        }
        Err(Error::TimedOut)
    }
    fn select_virtio_vectors(&self, common: u64, queues: &[u16]) -> Result<()> {
        unsafe {
            let count = mmio_read16(common + 18);
            if queues.iter().any(|&q| q >= count) { return Err(Error::InvalidArgument); }
            mmio_write16(common + 16, self.vector);
            if mmio_read16(common + 16) != self.vector { return Err(Error::Io); }
            for &queue in queues {
                mmio_write16(common + 22, queue);
                mmio_write16(common + 26, self.vector);
                if mmio_read16(common + 26) != self.vector { return Err(Error::Io); }
            }
            core::arch::asm!("dsb sy", options(nostack));
        }
        Ok(())
    }
    /// Initial enable only, after queue publication and initialization ack.
    pub fn arm(&self) -> Result<()> {
        Error::from_syscall_return(unsafe { crate::t_pci_irq_arm(self.handle.raw() as i64) }).map(|_| ())
    }
    /// Returns the outstanding ticket, or None on timeout. The same ticket is
    /// replayed until completion, so a copy failure cannot strand the source.
    pub fn wait_timeout(&self, timeout_ns: u64) -> Result<Option<crate::TPciIrqEvent>> {
        let mut event = crate::TPciIrqEvent::default();
        let rc = unsafe { crate::t_pci_irq_wait(self.handle.raw() as i64, timeout_ns, &mut event) };
        Error::from_syscall_return(rc)?;
        Ok(if rc == 0 { None } else { Some(event) })
    }
    pub fn wait(&self) -> Result<Option<crate::TPciIrqEvent>> { self.wait_timeout(0) }
    /// Call only after draining/acknowledging the device and a device barrier.
    /// WouldBlock leaves the source masked; WAIT supplies a timed retry of the
    /// same ticket, after which the driver drains and acknowledges again.
    pub fn complete(&self, event: crate::TPciIrqEvent) -> Result<()> {
        Error::from_syscall_return(unsafe { crate::t_pci_irq_complete(self.handle.raw() as i64,
            event.generation, event.sequence) }).map(|_| ())
    }
    pub fn disable(&self) -> Result<()> {
        Error::from_syscall_return(unsafe { crate::t_pci_irq_disable(self.handle.raw() as i64) }).map(|_| ())
    }
    pub fn info(&self) -> Result<crate::TPciIrqInfo> {
        let mut info = crate::TPciIrqInfo::default();
        Error::from_syscall_return(unsafe { crate::t_pci_irq_info(self.handle.raw() as i64, &mut info) })?;
        Ok(info)
    }
}
impl Drop for PciIrq {
    fn drop(&mut self) { let _ = self.disable(); }
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
    ///
    /// DEVICE-ALIASING CAVEAT: the DMA region is Normal-WB memory the device
    /// writes concurrently. A `&[u8]` asserts the bytes are immutable for the
    /// borrow, so do NOT hold the returned slice across a window in which the
    /// device may DMA-write -- the compiler may cache/hoist a load and return
    /// stale bytes. For device-concurrent access use `read_u32` (volatile) or
    /// raw `base_va()` with an explicit `virtio_rmb` / `dsb`.
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
    ///
    /// DEVICE-ALIASING CAVEAT: a `&mut [u8]` asserts EXCLUSIVE access; a device
    /// DMA-write while the borrow is live is UB (the compiler treats the slice
    /// as `noalias`). Take this view only while the device is quiesced for this
    /// region; for device-concurrent buffers use `write_u32` (volatile) + an
    /// explicit barrier before the device-visible notify.
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
    /// The `compiler_fence` here is COMPILER-ONLY -- it emits no instruction
    /// and provides ZERO CPU<->device ordering; the `Ordering::Acquire` arg
    /// does NOT make this an acquire-load against the device. A HARDWARE
    /// barrier (`dmb ishld` via `virtio_rmb` after the device's used-ring
    /// update, `dsb` before a device-visible notify) is MANDATORY for
    /// cross-device-CPU visibility and is the caller's responsibility.
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

// =============================================================================
// PciDev -- a claimed + BAR-mapped VirtIO-PCI function.
// =============================================================================
//
// The PCI sibling of `Mmio` (pci-2, the virtio-PCI transport). `PciDev::claim`
// composes the three pci-1c syscalls -- SYS_PCI_CLAIM (claim a function by its
// virtio_device_id), SYS_PCI_INFO (read its resolved BAR + capability-region +
// INTID topology), SYS_PCI_WINDOWS and SYS_PCI_MAP_WINDOW (map allowed BAR
// pages, retaining MSI-X routing pages in the kernel) -- so the returned object exposes the four virtio_pci capability regions
// (common / notify / isr / device config) as mapped VAs a driver pokes through
// the ISV-safe `mmio_*` primitives.
//
// WHY PCI rather than the virtio-mmio bank: on PCIe each function carries its
// own page-aligned BAR, so the existing page-exclusive KObj claim isolates two
// persistent userspace drivers (netd vs stratumd) at the MMU granule -- the #140
// resolution the virtio-mmio bank could not give (8 device slots / 4 KiB page).
//
// Lifetime + I-5: Drop detaches the mapped windows, then closes the PCI
// handle. Kernel BAR mappings and IRQ endpoints independently retain the
// function until their references are released. KObj_PCI joins KOBJ_KIND_HW_MASK, so
// the kernel rejects SYS_TRANSFER + handle_dup; this type adds no Transfer trait.

/// The four VirtIO-PCI capability-structure kinds (VIRTIO 1.2 section 4.1.4.1),
/// the index into the kernel-resolved `TPciInfo.regions` (`cfg_type - 1`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PciRegion {
    /// `virtio_pci_common_cfg` -- feature negotiation + per-queue config.
    Common = 0,
    /// Notify region -- the per-queue doorbell base.
    Notify = 1,
    /// ISR status byte (read-to-clear).
    Isr = 2,
    /// Device-specific config (the virtio-net MAC + link status).
    Device = 3,
}

/// `PciDev::claim` failure causes (the bare PCI syscalls return -1 without
/// errno discrimination, so claim/info/map collapse to one variant each).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PciError {
    /// `SYS_PCI_CLAIM` failed: no matching function, already claimed, BAR
    /// assignment failed, or the caller lacks `CAP_HW_CREATE`.
    Claim,
    /// `SYS_PCI_INFO` failed (bad handle -- should not happen post-claim).
    Info,
    /// A BAR/hostmem mapping syscall failed: `SYS_PCI_MAP_WINDOW` (overlap / bad VA
    /// / prot) or `SYS_BURROW_FROM_HOSTMEM` (bad shmid, out-of-bounds or
    /// non-page-aligned subrange, missing `RIGHT_MAP`, or an unknown cache policy).
    MapBar,
}

/// Per-BAR user-VA window stride: `PciDev::claim` maps BAR `i` at
/// `bar_window + i * PCI_BAR_VA_STRIDE`. 1 MiB dwarfs every capability-region
/// BAR (common+notify+isr+device pack into a single <= 16 KiB BAR) while
/// keeping the six-BAR window (6 MiB) a trivial slice of the 48-bit user AS.
/// A BAR larger than the stride -- the virtio-gpu hostmem shared-memory class
/// (#166; GPU-DESIGN.md section 6.2) -- is claimed but NOT eagerly mapped:
/// no capability region ever lives in one (the spec routes those through
/// cfg_type 1..4 into KiB-scale BARs; `region()` fails closed regardless),
/// and mapping a multi-hundred-MiB heap wholesale is exactly what the
/// deferred per-allocation Venus path exists to avoid.
pub const PCI_BAR_VA_STRIDE: u64 = 0x10_0000;

/// A claimed VirtIO-PCI function with its memory BARs mapped into user VA.
///
/// Created by [`PciDev::claim`]. Register access goes through [`PciDev::region`]
/// (which yields the mapped VA + length of a capability region) + the crate's
/// `mmio_*` ISV-safe primitives. Non-transferable per invariant I-5.
pub struct PciDev {
    #[allow(dead_code)] // Drop fires on the handle.
    handle: Handle,
    info: TPciInfo,
    windows: [TPciWindow; T_PCI_WINDOW_MAX],
    window_va: [Option<u64>; T_PCI_WINDOW_MAX],
}

impl PciDev {
    /// Claim the first VirtIO-PCI function whose `virtio_device_id` matches
    /// (1 = net, 4 = rng, ...), read its topology, and map every present memory
    /// BAR that fits the per-BAR stride into user VA. BAR `i` lands at
    /// `bar_window + i * PCI_BAR_VA_STRIDE`; a larger BAR (the hostmem
    /// shared-memory class) is claimed but left unmapped — see
    /// [`PCI_BAR_VA_STRIDE`] and [`PciDev::shm_region`].
    ///
    /// Required capability: `CAP_HW_CREATE`. The minted handle carries the
    /// kernel-fixed `R | W | MAP` rights (no `TRANSFER`).
    ///
    /// # Safety
    ///
    /// `bar_window` must name a free user-VA region of at least
    /// `6 * PCI_BAR_VA_STRIDE` bytes; the kernel rejects overlaps, but only
    /// after trusting the caller chose an unmapped window.
    pub unsafe fn claim(virtio_device_id: u32, bar_window: u64) -> core::result::Result<Self, PciError> {
        Self::claim_nth(virtio_device_id, 0, bar_window)
    }

    /// Claim the nth (0-based, enumeration-order) matching function -- the
    /// G-7c selector for a SECOND same-id function (two virtio-input
    /// functions: keyboard + tablet). nth rides the high 32 bits of the
    /// SYS_PCI_CLAIM arg; nth 0 == [`PciDev::claim`]. Same safety contract.
    pub unsafe fn claim_nth(virtio_device_id: u32, nth: u32, bar_window: u64) -> core::result::Result<Self, PciError> {
        let rc = t_pci_claim(u64::from(virtio_device_id) | (u64::from(nth) << 32));
        if rc < 0 {
            return Err(PciError::Claim);
        }
        let handle = Handle::from_raw(rc as i32, Rights::READ | Rights::WRITE | Rights::MAP);

        let mut info = TPciInfo::zeroed();
        if t_pci_info(i64::from(handle.raw()), &mut info) < 0 {
            // Drop closes the handle; the kernel releases the claim + BARs.
            return Err(PciError::Info);
        }

        let mut windows = [TPciWindow::default(); T_PCI_WINDOW_MAX];
        let n = t_pci_windows(i64::from(handle.raw()), windows.as_mut_ptr(), T_PCI_WINDOW_MAX as u64);
        if n < 0 || n as usize > T_PCI_WINDOW_MAX { return Err(PciError::Info); }
        // Validate every record before creating the first mapping, so even
        // malformed topology cannot bypass the rollback path later.
        for w in &windows[..n as usize] {
            let bar = info.bars.get(w.bar as usize).ok_or(PciError::Info)?;
            let size = bar.size.checked_add(4095).ok_or(PciError::Info)? & !4095;
            if bar.present == 0 || w.reserved != 0 || w.length == 0 ||
                (w.offset | w.length) & 4095 != 0 ||
                w.offset > size || w.length > size - w.offset {
                return Err(PciError::Info);
            }
        }
        let mut window_va = [None; T_PCI_WINDOW_MAX];
        for i in 0..n as usize {
            let w = windows[i];
            let bar = &info.bars[w.bar as usize];
            // Large shared-memory BARs are mapped per allocation, explicitly.
            if bar.size > PCI_BAR_VA_STRIDE { continue; }
            let va = bar_window.checked_add(u64::from(w.bar) * PCI_BAR_VA_STRIDE)
                .and_then(|v| v.checked_add(w.offset));
            let rc = match va {
                Some(v) => t_pci_map_window(i64::from(handle.raw()), v, u64::from(w.bar),
                    T_PROT_READ | T_PROT_WRITE, w.offset, w.length),
                None => -1,
            };
            if rc < 0 {
                // A mapping owns a separate claim reference. Dropping only
                // the handle would strand earlier windows until process exit.
                for j in 0..i {
                    if let Some(v) = window_va[j] { let _ = t_burrow_detach(v, windows[j].length); }
                }
                return Err(PciError::MapBar);
            }
            window_va[i] = va;
        }
        Ok(Self { handle, info, windows, window_va })
    }

    /// The mapped VA + byte length of a resolved VirtIO-PCI capability region,
    /// or `None` if the region (or its backing BAR) is absent. The returned VA
    /// is bounded to `length` bytes; every register access a driver makes off it
    /// stays inside the region.
    #[must_use]
    pub fn region(&self, kind: PciRegion) -> Option<(u64, u32)> {
        let r = self.info.regions[kind as usize];
        if r.present == 0 {
            return None;
        }
        let bar = self.info.bars.get(r.bar as usize)?;
        let off = u64::from(r.offset);
        let len = u64::from(r.length);
        if off.checked_add(len)? > bar.size { return None; }
        for (w, va) in self.windows.iter().zip(self.window_va.iter()) {
            if w.bar != u32::from(r.bar) || off < w.offset { continue; }
            let delta = off - w.offset;
            if delta <= w.length && len <= w.length - delta {
                if let Some(base) = va { return Some((base.checked_add(delta)?, r.length)); }
            }
        }
        None
    }

    /// The function's swizzled GIC INTID (the INTx line), or `None` if the DTB
    /// interrupt-map did not resolve one.
    #[inline]
    #[must_use]
    pub fn intid(&self) -> Option<u32> {
        if self.info.intid_valid != 0 {
            Some(self.info.intid)
        } else {
            None
        }
    }

    /// The NOTIFY_CFG capability's `notify_off_multiplier` (the per-queue
    /// doorbell stride: a queue's notify address is
    /// `notify_base + queue_notify_off * multiplier`).
    #[inline]
    #[must_use]
    pub fn notify_off_multiplier(&self) -> u32 {
        self.info.notify_off_multiplier
    }

    /// The discovered VIRTIO shared-memory region carrying `shmid` (cfg_type 8
    /// -- virtio-gpu hostmem is shmid 1), as `(bar_pa + offset, length)`: the
    /// region's device PA and byte length. Discovery only -- the backing BAR is
    /// deliberately unmapped at claim (see [`PCI_BAR_VA_STRIDE`]); mapping a
    /// subrange is the Venus-arc kernel delta. `None` when the device carries
    /// no such region or the kernel rejected its layout.
    #[must_use]
    pub fn shm_region(&self, shmid: u8) -> Option<(u64, u64)> {
        for s in self.info.shm.iter() {
            if s.present == 0 || s.shmid != shmid {
                continue;
            }
            let bar = self.info.bars.get(s.bar as usize)?;
            if bar.present == 0 {
                return None;
            }
            return Some((bar.pa + s.offset, s.length));
        }
        None
    }

    /// Warp-6 V-3b: map a subrange of the hostmem BAR into this Proc's VA and
    /// return the guest VA. `shmid` selects the shared-memory region (1 = gpu
    /// hostmem), `offset` is relative to that region's window base -- the SAME
    /// base a HOST3D `map_blob(res, offset)` places its blob at, so a blob mapped
    /// host-side at offset O is reached in the guest at this `offset` O. `cache`
    /// is the `T_CACHE_*` the host DICTATED for the region (from `map_blob`'s
    /// `map_info`) -- pass the MATCHING attribute, GPU-DESIGN 6.2 "honored
    /// exactly"; a mismatched host/guest alias loses coherency on ARM64. `offset`
    /// and `length` must be page-aligned (the kernel refuses otherwise). The
    /// KObj_PCI claim this `PciDev` holds is the authority (I-5-non-transferable,
    /// RIGHT_MAP-gated). Errs `MapBar` on a bad/unaligned subrange / missing right
    /// / unknown cache policy (the bare syscall gives no errno).
    pub fn burrow_from_hostmem(
        &self,
        shmid: u8,
        offset: u64,
        length: u64,
        cache: u64,
    ) -> core::result::Result<u64, PciError> {
        let rc = unsafe {
            t_burrow_from_hostmem(
                i64::from(self.handle.raw()),
                u64::from(shmid),
                offset,
                length,
                cache,
            )
        };
        if rc < 0 {
            Err(PciError::MapBar)
        } else {
            Ok(rc as u64)
        }
    }

    /// The VirtIO device id (1 = net, 4 = rng, ...).
    #[inline]
    #[must_use]
    pub fn virtio_device_id(&self) -> u16 {
        self.info.virtio_device_id
    }

    /// The function's PCI bus / device / function numbers.
    #[inline]
    #[must_use]
    pub fn bdf(&self) -> (u8, u8, u8) {
        (self.info.bus, self.info.dev, self.info.fn_)
    }
}

impl Drop for PciDev {
    fn drop(&mut self) {
        for (w, va) in self.windows.iter().zip(self.window_va.iter()) {
            if let Some(va) = va { unsafe { let _ = t_burrow_detach(*va, w.length); } }
        }
        // Handle drops after this body. An endpoint/hostmem alias may retain
        // the function, so drivers must quiesce before releasing DMA buffers.
    }
}
