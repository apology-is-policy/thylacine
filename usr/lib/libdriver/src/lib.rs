// libdriver -- the native Menagerie driver framework (MENAGERIE.md section 6).
//
// A Thylacine driver is a userspace Proc holding only its device's narrowed,
// non-transferable hardware allowance (I-34). This crate is the scaffold that
// makes one droppable: the manifest schema the warden's bind database consumes,
// the node-INTERSECT-needs intersection that computes the auditable grant, the
// spawn-descriptor codec that carries the bound resources from warden to driver,
// and the `probe`/`bind`/`serve` runtime a driver is written against.
//
// The split, mirroring kaua/netdev:
//   - `manifest` + `resource` are PURE (no libthyla-rs): the schema, the
//     `resolve` intersection (the I-34 grant property in one host-tested place),
//     and the descriptor encode/decode. The warden calls `resolve` + encodes; a
//     driver decodes. Both halves are exercised on the host.
//   - `driver` (feature `driver`, default-on) is the only libthyla-rs layer: the
//     `Driver` trait, the `run` entry, and the allowance + handle-mint helpers.
//
// `#![cfg_attr(not(test), no_std)]`: no_std for aarch64-unknown-none; std under
// `cargo test` so the pure layers run on the host.

#![cfg_attr(not(test), no_std)]

extern crate alloc;

pub mod dtb;
pub mod manifest;
pub mod readyline;
pub mod resource;
pub mod source;
pub mod supervise;

#[cfg(feature = "driver")]
pub mod driver;

pub use manifest::{
    DmaNeed, IrqNeed, Lifecycle, Manifest, MmioNeed, Needs, PciNeed, Restart, MANIFEST_ABI,
};
pub use readyline::{feed_ready_line, ReadyLine, READY_LINE_MAX};
pub use resource::{resolve, BoundResources, NodeResources, DESCRIPTOR_VERSION, MAX_IRQ, MAX_MMIO};
pub use source::{
    best_match, parse_pci_ctl, reconcile_reported_node, DeviceId, DeviceNode, DiscoverySource,
    MAX_IDS, NODE_RECORD_VERSION,
};
pub use supervise::{
    backoff_ms, next_step, Disposition, RunOutcome, SuperviseStep, BACKOFF_MAX_MS, RESTART_LIMIT,
};

#[cfg(feature = "driver")]
pub use driver::{
    alloc_dma, bind, claim_irq, map_mmio, run, to_allowance, Driver, DriverVa, EXIT_BIND, EXIT_OK,
    EXIT_PROBE, EXIT_SERVE,
};
#[cfg(feature = "driver")]
pub use source::{DtbSource, PciSource};

/// Every fallible operation in the framework reports one of these. Flat + `Copy`
/// so it prints under `{:?}` (the `run` scaffold logs it) and the pure layers
/// stay host-testable without a libthyla-rs error type.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    /// A manifest could not be parsed (malformed brace block, bad value, bad
    /// size suffix, a duplicate or missing required key).
    Parse,
    /// No node `compatible` matched any manifest `binds` entry. (`resolve`.)
    NoMatch,
    /// A node exposes more `reg` windows than the allowance can carry
    /// (`MAX_MMIO`). (`resolve`.)
    TooManyWindows,
    /// A node exposes more wired `interrupts` than the allowance can carry
    /// (`MAX_IRQ`). (`resolve`.)
    TooManyIrqs,
    /// The spawn descriptor carried an unknown format version. (codec.)
    BadVersion,
    /// The spawn descriptor had a malformed field (unknown key, missing `=`,
    /// duplicate key, or a value the field does not accept). (codec.)
    BadField,
    /// A hex/decimal number in the descriptor could not be parsed, or a window
    /// lacked its `base:size` colon. (codec.)
    BadNumber,
    /// The descriptor named more MMIO windows or IRQs than the allowance caps
    /// allow (`MAX_MMIO` / `MAX_IRQ`). (codec.)
    TooManyResources,
    /// `bind` found no descriptor at argv[1] (the warden did not pass one).
    NoDescriptor,
    /// A handle-mint helper was asked for a resource index the bound set does
    /// not contain (a driver bug -- it asked for window/IRQ N with fewer than N
    /// conferred). (`driver`.)
    NoSuchResource,
    /// A `libthyla_rs::hardware` create/map failed -- typically the request fell
    /// outside the conferred allowance (the kernel I-34 gate), or the chosen VA
    /// collided. (`driver`.)
    Hardware,
}
