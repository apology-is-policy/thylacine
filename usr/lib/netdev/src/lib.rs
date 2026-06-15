//! netdev -- native Thylacine NIC drivers: the net-1 frame transport.
//!
//! The reusable Ethernet frame driver layer net-2's `netd` owns and smoltcp
//! wraps (the charter, docs/NET-DESIGN.md section 13/17). net-1 generalizes the
//! proven `virtio-net-loop` probe (RX descriptor recycling + TX reuse, with the
//! virtio audit hardenings -- desc_id bounds, `virtio_rmb` placement, u16-wrap,
//! back-pressure) into a `send(&[u8])` / `poll_rx(&mut [u8])` API over arbitrary
//! Ethernet frames, plus the device-death QUIESCE the single-shot probes skip
//! (QUEUE_READY=0 + device reset before the DMA pages release; RW-7 R3-F1).
//!
//! Layering (the kaua pattern):
//!   - `ring` -- the pure split-virtqueue index arithmetic (avail/used counters,
//!     wrap, back-pressure). Terminal-free + HOST-TESTABLE; the audit-bearing
//!     core in isolation.
//!   - `virtio` (feature `driver`, on by default) -- `VirtioNet`, the device
//!     glue over the libthyla-rs MMIO/IRQ/DMA substrate. The only libthyla-rs +
//!     audit-bearing layer; built for aarch64-thylacine, not the host.
//!   - `virtio_pci` (feature `driver`) -- `VirtioNetPci`, the PCI sibling of
//!     `virtio` over the libthyla-rs `PciDev` substrate (the #140 transport).
//!     Reuses `ring` verbatim; the same RX/TX API + audit hardenings.
//!
//! `#![cfg_attr(not(test), no_std)]`: no_std for the userspace target; builds
//! against std under `cargo test` so the pure `ring` layer runs on the host.

#![cfg_attr(not(test), no_std)]

pub mod ring;

#[cfg(feature = "driver")]
pub mod virtio;

#[cfg(feature = "driver")]
pub mod virtio_pci;

#[cfg(feature = "driver")]
pub use virtio::{OpenError, VirtioNet, MAX_FRAME, MTU, VIRTIO_NET_HDR_LEN};

#[cfg(feature = "driver")]
pub use virtio_pci::{PciOpenError, VirtioNetPci};
