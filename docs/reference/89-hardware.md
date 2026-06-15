# 89 — t::hardware (libthyla-rs typed RAII over hardware handles)

**Status**: Mmio + Irq + Dma typed wrappers landed at U-2h-hardware (commit `*(pending)*`). Three of ten consumers migrated in the same chunk (mmio-probe, irq-probe, irq-bench); 7 virtio-* binaries continue to consume the bare SVC wrappers — migration is mechanical and lands at the discretion of subsequent chunks.

## Purpose

`libthyla_rs::hardware` provides typed RAII wrappers over the kernel's three hardware-handle surfaces:

- `Mmio` — wraps `KObj_MMIO`. Claim a PA range, install user-VA mappings, expose volatile register read / write at byte offsets.
- `Irq` — wraps `KObj_IRQ`. Claim a GIC SPI, block on pending interrupts via `wait()`, integrate with `t::poll` via `AsFd`.
- `Dma` — wraps `KObj_DMA`. Allocate a contiguous DMA buffer, install user-VA mappings, expose the PA for device-visible descriptor embedding, expose byte-level + 32-bit read/write at offsets.

The bare SVC wrappers (`t_mmio_create`, `t_mmio_map`, `t_irq_create`, `t_irq_wait`, `t_dma_create`, `t_dma_map`) remain exported. Drivers can choose either layer; the typed layer is more idiomatic Rust (RAII, type-safe rights, type-system-enforced non-transferability via I-5).

## Public API

```rust
// MMIO.

pub struct Mmio { /* opaque */ }

impl Mmio {
    pub unsafe fn new(
        pa: u64,
        size: usize,
        rights: Rights,
        vaddr: u64,
        prot: u32,
    ) -> Result<Self>;

    pub const fn base_va(&self) -> *mut u8;
    pub const fn len(&self) -> usize;
    pub const fn is_empty(&self) -> bool;

    pub fn read_u32(&self, offset: usize) -> u32;       // volatile; bounds + alignment asserted
    pub fn write_u32(&self, offset: usize, value: u32); // volatile; bounds + alignment asserted
}

// IRQ.

pub struct Irq { /* opaque */ }

impl Irq {
    pub fn new(intid: u32, rights: Rights) -> Result<Self>;
    pub const fn intid(&self) -> u32;
    pub fn wait(&self) -> Result<u32>;  // returns collapsed pending count
}

impl AsFd for Irq {
    fn as_raw_fd(&self) -> i32;
}

// DMA.

pub struct Dma { /* opaque */ }

impl Dma {
    pub unsafe fn new(
        size: usize,
        rights: Rights,
        vaddr: u64,
        prot: u32,
    ) -> Result<Self>;

    pub const fn base_va(&self) -> *mut u8;
    pub const fn len(&self) -> usize;
    pub const fn is_empty(&self) -> bool;
    pub const fn paddr(&self) -> u64;          // PA for device-visible descriptors

    pub fn as_slice(&self) -> &[u8];
    pub fn as_slice_mut(&mut self) -> &mut [u8];

    pub fn read_u32(&self, offset: usize) -> u32;          // normal memory; compiler_fence(Acquire) before
    pub fn write_u32(&mut self, offset: usize, value: u32); // normal memory; compiler_fence(Release) after
}

// PCI (pci-2 — the virtio-PCI transport; the #140 resolution).

pub enum PciRegion { Common = 0, Notify = 1, Isr = 2, Device = 3 } // cfg_type - 1
pub enum PciError  { Claim, Info, MapBar, BarTooLarge }
pub const PCI_BAR_VA_STRIDE: u64 = 0x10_0000;  // per-BAR user-VA window

pub struct PciDev { /* opaque */ }

impl PciDev {
    // Compose SYS_PCI_CLAIM + SYS_PCI_INFO + SYS_PCI_MAP_BAR: claim the first
    // virtio function matching `virtio_device_id` (1 = net, 4 = rng) and map
    // every present BAR `i` at `bar_window + i * PCI_BAR_VA_STRIDE`.
    pub unsafe fn claim(virtio_device_id: u32, bar_window: u64)
        -> core::result::Result<Self, PciError>;

    pub fn region(&self, kind: PciRegion) -> Option<(u64, u32)>; // (mapped VA, length)
    pub fn intid(&self) -> Option<u32>;                          // INTx GIC INTID
    pub fn notify_off_multiplier(&self) -> u32;
    pub fn virtio_device_id(&self) -> u16;
    pub fn bdf(&self) -> (u8, u8, u8);
}
```

## Implementation

`usr/lib/libthyla-rs/src/hardware.rs` (~560 LOC).

### Combined create + map

Every existing consumer calls `t_mmio_create` immediately followed by `t_mmio_map` at a chosen user VA; no consumer holds an unmapped handle. `Mmio::new` / `Dma::new` collapse the two steps so the returned object is fully usable for register access. If a future driver needs the split (e.g., to remap after a different policy chooses the VA), `create_unmapped` / `map` can be added later; the combined form covers v1.0 needs.

### Drop closes the handle

`SYS_CLOSE` on a `KObj_MMIO` / `KObj_IRQ` / `KObj_DMA` handle releases the kernel's per-handle refcount. The Burrow wrapping the user-VA mapping (`kernel/burrow.c::burrow_free_internal`) holds an INDEPENDENT refcount on the underlying KObj — the mapping survives `SYS_CLOSE` until the proc's pgtable is torn down at exit OR (post-handle-close) until a future `SYS_BURROW_DETACH` on the mapping window.

In practice every native driver creates these handles once at startup and never closes; the `Drop` only fires when the binary exits, where its effect is a no-op (proc_free is about to release everything anyway). The RAII shape exists for code clarity, not for runtime cleanup semantics.

### Non-transferability (I-5)

The kernel statically rejects `SYS_TRANSFER` for `KOBJ_MMIO` / `KOBJ_IRQ` / `KOBJ_DMA` at the syscall layer (per `kernel/handle.c::handle_acquire_obj`). The Rust type system makes the same invariant visible by NOT implementing any future `Transfer` trait on `Mmio` / `Irq` / `Dma`.

### ISV-safe MMIO accesses

`Mmio::read_u32` / `write_u32` funnel through the module-level `mmio_read32` / `mmio_write32` primitives -- a single-instruction `ldr`/`str` via inline asm, with the register offset pre-applied to the address in Rust so the instruction is base-only (`[xN]`, no displacement). This guarantees two things at once: the compiler does not coalesce or reorder the access (the default memory-clobbering asm options are strictly stronger than `read_volatile`), AND the emitted instruction sets `ESR_EL1.ISV=1` (Instruction Syndrome Valid) on an MMIO abort, so a hypervisor (HVF) can decode the emulated access. A plain `read_volatile`/`write_volatile` is functionally correct but lets LLVM pick the addressing mode; once the `#[inline(always)]` driver helpers fold into a register-dense caller it emits pre-indexed writeback (`str w, [x, #imm]!`) and unscaled `stur`/`ldur` -- both ISV=0, which trips QEMU's HVF backend `assert(isv)` (this was #890; the kernel's out-of-line accessors stayed ISV=1, which is why kernel virtio worked under HVF while userspace tripped). The kernel-installed PTE carries the MAIR_IDX_DEVICE (nGnRnE) attribute so the hardware also does not reorder; all layers are needed.

The primitives ship for all four widths -- `mmio_read8`/`16`/`32`/`64` + `mmio_write8`/`16`/`32`/`64` -- and are the single ISV-safe MMIO accessor in the tree: every native virtio-* driver's local register helper delegates to them (see PORTABILITY.md section 8).

### Bounds + alignment checks

`read_u32` / `write_u32` assert `offset + 4 <= len` AND `offset % 4 == 0` at runtime; a misuse panics via libthyla-rs's panic_handler (which tail-calls `t_exits(1)`). The kernel-side ABI also rejects misaligned MMIO accesses (architecturally a fault); the runtime check catches the bug at the userspace boundary before the device sees a malformed cycle.

### DMA accesses are normal memory

`Dma::as_slice` / `as_slice_mut` return ordinary `&[u8]` / `&mut [u8]` views. The buffer's underlying pages have Normal-WB cache attributes (not Device); the device sees writes after the appropriate memory barrier (`virtio_wmb` / `dmb ishst`). `Dma::read_u32` / `write_u32` insert a `compiler_fence(Acquire)` / `compiler_fence(Release)` for compiler-side ordering, but the HARDWARE barrier (`dmb ishld` via the `virtio_rmb` helper exported from this crate) is still the caller's responsibility because placement depends on the device's ordering semantics.

### Error mapping

The HW-handle syscalls (`SYS_MMIO_CREATE` / `SYS_MMIO_MAP` / `SYS_IRQ_CREATE` / `SYS_IRQ_WAIT` / `SYS_DMA_CREATE` / `SYS_DMA_MAP`) return `-1` as a flat error sentinel; the kernel does not discriminate among cap-missing / out-of-range / overlap / page-misaligned / IPS-bound / not-found. The typed wrappers map `-1` to `Error::InvalidArgument` as the closest catch-all. A future kernel ABI that returns proper `-errno` values would route through `from_syscall_return` for richer variants (the `hw_error` helper in `hardware.rs` handles both cases).

## Data structures

```rust
// Mmio: 16 bytes on 64-bit.
//   handle: Handle              (RAII; closes on Drop)
//   base_va: *mut u8            (user VA where bank is mapped)
//   len: usize                  (size in bytes)
//
// Irq: 8 bytes.
//   handle: Handle
//   intid: u32
//
// Dma: 24 bytes.
//   handle: Handle
//   base_va: *mut u8
//   len: usize
//   paddr: u64                  (kernel-allocated PA; embed in descriptors)
```

No `#[repr(C)]` — these are Rust types, not ABI mirrors. The underlying syscall ABI is pinned by the SVC wrappers in `lib.rs`.

## Send + Sync

`Mmio`, `Irq`, and `Dma` are NOT `Send` and NOT `Sync` by default (the raw-pointer fields make them !Send + !Sync automatically). A multi-thread driver that wants to share an `Mmio` across threads wraps it in `Arc<Mutex<_>>`. The user-VA mapping IS shared across threads in the same Proc (threads share `pgtable_root` + ASID), so `unsafe impl Send + Sync` could be safely added in a future version once we have a real multi-thread driver consumer — held until the consumer surfaces.

## Spec cross-reference

No formal `specs/*.tla` module for hardware. Per the spec-to-code FULLY suspended broadening (CLAUDE.md, 2026-05-23 direction), the invariants are validated by prose reasoning + the audit + the runtime test suite. The relevant invariants:

- **I-5 (non-transferability)**: `KObj_MMIO` / `KObj_IRQ` / `KObj_DMA` cannot be transferred via 9P. Enforced at the kernel syscall layer (`kernel/handle.c::handle_acquire_obj` returns `-EINVAL`); reflected in the Rust type system by NOT impl-ing any transfer operation.
- **Capability gating**: `CAP_HW_CREATE` is required for the create syscalls; absent the cap, the kernel returns -1. The typed wrappers surface this as `Err(Error::InvalidArgument)`.
- **Rights gating**: each syscall checks the requested rights (e.g., `SYS_IRQ_WAIT` requires `RIGHT_SIGNAL`); the wrappers pass the `Rights` bitmask through to the kernel which enforces.

## Tests

- `usr/alloc-smoke/src/main.rs` hardware section (~70 LOC, added U-2h-hardware): exercises the negative paths — `Mmio::new`, `Irq::new`, `Dma::new` without `CAP_HW_CREATE` all return `Err(Error::InvalidArgument)`. Runs on every boot via joey-spawned `/alloc-smoke`. Pass shows `alloc-smoke: Mmio + Irq + Dma cap-missing reject OK`.
- `usr/mmio-probe/src/main.rs` (migrated at U-2h-hardware): exercises the POSITIVE Mmio path — claims the PL031 RTC PA, maps it at user VA, reads PeriphID0 + RTCDR. Pass shows `mmio-probe: Mmio::new ok (create + map combined)` and `mmio-probe: PASS`.
- `usr/irq-probe/src/main.rs` (migrated at U-2h-hardware): exercises the POSITIVE Irq path — claims SPI 96, waits for a kernel-pre-pended IRQ, validates count == 1.
- `usr/irq-bench/src/main.rs` (migrated at U-2h-hardware): exercises Irq::wait in a loop (128 iterations) measuring user-VA-to-IRQ-return latency.

## Error paths

Every typed constructor returns:
- `Err(Error::InvalidArgument)` on the kernel's -1 generic-rejection. This is the catch-all for cap-missing / overlap / out-of-range / page-misaligned / kernel-reserved / IPS-bound. The kernel does not distinguish further at v1.0.

Every accessor (`read_u32`, `write_u32`) panics on bounds-overflow or misalignment. Panics route through libthyla-rs's `panic_handler` which prints the panic message and tail-calls `t_exits(1)`.

## Performance characteristics

- `Mmio::read_u32` / `write_u32`: one volatile load/store; ~10s of cycles at the device (PCIe / MMIO bus is the bottleneck). Bounds + alignment asserts add 2-3 cycles in debug builds; LLVM tends to elide them in release when offset is constant.
- `Irq::wait`: blocks via `tsleep` on the rendez; wake latency dominated by GIC + scheduler context-switch (~10s of microseconds on QEMU TCG; ~hundreds of nanoseconds on bare metal).
- `Dma::as_slice` / `as_slice_mut`: zero overhead (cast). `read_u32` / `write_u32` add a compiler fence (no runtime overhead; barrier emission only).

## Status

- **U-2h-hardware LANDED**: `t::hardware::{Mmio, Irq, Dma}` available; the bare SVC wrappers remain.
- **pci-2 LANDED**: `t::hardware::PciDev` over the pci-1c syscalls (`t_pci_claim` / `t_pci_info` / `t_pci_map_bar`). First consumer: `netdev::VirtioNetPci`. Non-transferable (I-5) like the other three.
- **pci-3 audit CLEAN** (0 P0 / 0 P1 / 0 P2 / 4 P3; Opus 4.8 max + self-audit): F1 partial-map leak tracked (no v1.0 detach path — see caveats), F2 notify-doorbell bound fixed, F3 `TPciInfo` `offset_of!` asserts completed, F4 `pci.walk_caps_hostile` kernel test added. The sub-arc is COMPLETE.
- **Consumers migrated**: mmio-probe, irq-probe, irq-bench (the 3 simple probes). The migrations validate the positive paths on real hardware (PL031 RTC) and real GIC (SPI 96).
- **Consumers NOT YET migrated**: virtio-blk-probe, virtio-blk-rw, virtio-gpu, virtio-input, virtio-net-arp, virtio-net-loop, virtio-net-probe (7 binaries). They continue to use the bare SVC wrappers; their migration is mechanical and can land incrementally as their authors prefer. The bare wrappers remain exported — there is no "must migrate before X" deadline.

## Naming rationale

`hardware` rather than `hw` (matching the verbose Rust ecosystem convention — `t::process` not `t::proc`, `t::ninep` not `t::9p`). The submodule names `Mmio` / `Irq` / `Dma` match the kernel's `KObj_*` lineage exactly so grepping across layers stays direct.

## Known caveats / footguns

- **`base_va` outlives the wrapper**: extracting `mmio.base_va()` and then dropping the `Mmio` leaves a `*mut u8` that still points to a valid mapping (the Burrow keeps the mapping alive until proc exit) — accessing the pointer after Drop is technically defined but conceptually wrong. The borrow checker doesn't catch this because raw pointers have no lifetime. Drivers should hold the typed wrapper for as long as they need the mapping.
- **No `Mmio::read_u8 / u16 / u64` methods**: the typed `Mmio` struct exposes only the u32 accessor methods at v1.0 (most VirtIO and ARM MMIO surfaces use 32-bit registers). Callers needing other widths use the module-level `mmio_read8`/`16`/`64` + `mmio_write8`/`16`/`64` primitives on `base_va() + offset` -- NOT `read_volatile` on a raw pointer, which can compile to an ISV=0 form and trip HVF (see "ISV-safe MMIO accesses" above). The native virtio-* drivers do exactly this through their local helper wrappers.
- **No `Mmio::write_u32` exclusivity**: the method takes `&self`, not `&mut self`. MMIO write is a side-effect at the device; multiple `&Mmio` references can concurrently write (Rust borrow checker allows it because there's no aliasable in-memory state). If a future driver wants to enforce "only one writer at a time" at the type level, it can wrap `Mmio` in a `Mutex` (single-threaded today). The kernel-side rights gating already prevents cross-Proc concurrent access (the handle is non-transferable).
- **Compiler fence vs hardware barrier**: `Dma::read_u32` / `write_u32` insert compiler fences but NOT hardware barriers. Drivers interacting with VirtIO devices MUST use `virtio_rmb()` (from `lib.rs`) for cross-CPU + cross-device-visibility ordering. The compiler fence prevents compiler reorderings but is not sufficient on its own.
- **`PciDev::claim` partial-map on error (pci-3 F1, tracked)**: if `claim` maps BAR 0 then a later BAR's map fails (or exceeds `PCI_BAR_VA_STRIDE`), the handle Drop closes the KObj_PCI but BAR 0's user-VA mapping persists via its independent Burrow ref until proc exit — the same "Drop does not unmap" `Mmio` carries. This is NOT explicitly unwound because there is **no v1.0 detach path**: `SYS_BURROW_DETACH` is confined to the burrow-attach window (`EXEC_USER_BURROW_BASE` = 4 GiB+, the security bound protecting ELF/stack/guard VMAs), and the driver-VA windows (BAR + DMA, mirroring the byte-identical mmio driver) live below it by design — so `t_burrow_detach` on a BAR VA returns -1 and unwinds nothing, and the proc-exit-bounded posture is shared by every virtio driver mapping (MMIO / DMA / BAR). Every `claim` error is fatal to the driver, so the Proc exits and `proc_free` reclaims the window; a single-BAR virtio-net-pci never maps a second BAR, so the path is unreachable in practice. A v1.x `create_unmapped`/`map` split (with the driver-VA windows moved above 4 GiB so detach works) would unwind it — deferred with the `Mmio` one.
- **`PciDev::region` bounds the region within its BAR, NOT the driver's field offsets within the region**: `region()` guarantees `[VA, VA+length)` lies inside the mapped (page-rounded) BAR, so no access escapes the mapping. But a driver that reads a fixed register offset (e.g. `common_cfg + 0x30`) trusts the resolved region is large enough; `VirtioNetPci` adds explicit `CCFG_MIN_LEN`/`DEVICE_CFG_MIN_LEN` guards for that, and (pci-3 F2) bounds the device-supplied `queue_notify_off * notify_off_multiplier` doorbell within the notify region (`NotifyRegionTooSmall`). A hostile device can therefore only fault within its own driver Proc, never escalate (the kernel keeps ECAM kernel-only + validates every config field it copies out).

## References

- Kernel-side implementation: `kernel/mmio_handle.c`, `kernel/irqfwd.c`, `kernel/dma_handle.c`
- Kernel handle dispatch: `kernel/handle.c::handle_release_obj`
- Burrow wrapping (mapping lifetime): `kernel/burrow.c::burrow_free_internal`
- VirtIO 1.2 specification (device-side ordering semantics): https://docs.oasis-open.org/virtio/virtio/v1.2/
- VirtIO RX barrier discipline (P4-Z F217 close): `docs/reference/39-hw-handles.md` caveat #11
- ARM ARMv8-A architecture reference manual (MMIO ordering): MAIR_IDX_DEVICE attribute semantics
