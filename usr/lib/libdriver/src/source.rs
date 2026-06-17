// The discovery-source layer -- MENAGERIE.md sections 3 + 7.
//
// A `DiscoverySource` owns a discovery domain (the DTB, a virtio-mmio bank, a
// PCIe bus) and turns it into a list of typed `DeviceNode`s. The warden binds
// each node by its IDENTITY (a `DeviceId`), never by its transport, and never
// reads a device register itself: a bus whose device type is only knowable at
// runtime (a virtio-mmio slot's `DeviceID`, a PCIe function's class) is
// enumerated by ITS source, which claims the raw transport nodes and re-emits
// **typed** children the warden binds by id. (MENAGERIE section 3: "the warden
// binds on the identity, not the transport, and never reads a device register.")
//
// This module is PURE (no libthyla-rs): the `DeviceId` / `DeviceNode` types, the
// node-record codec (the source -> warden channel), and the bind matching are
// host-tested. The concrete `DtbSource` (which reads /hw) is feature-gated to the
// `driver` build at the bottom of the file.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::manifest::Manifest;
use crate::resource::{parse_irqs, parse_windows, push_hex, NodeResources};
use crate::Error;

/// The node-record wire-format version (the source -> warden channel). Bumped only
/// on an incompatible record change; the warden rejects an unknown version
/// (fail-closed on a source/warden build skew).
pub const NODE_RECORD_VERSION: u32 = 1;

/// Max match identities a node record may carry. A real device node has 1-4
/// (a short, most-specific-first compatible list); the cap bounds the
/// trust-boundary parser's allocation the same way the reg/intid caps do, so a
/// hostile source cannot make the warden build an unbounded id Vec.
pub const MAX_IDS: usize = 16;

/// A device's match identity -- the key the warden binds on. A node carries an
/// ordered list, most-specific first. Extensible: PCIe `vid:did` and USB
/// `vid:pid` join as those bus sources land (each a new variant + a `bus:`-prefix
/// string form).
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DeviceId {
    /// A DTB `compatible` string (e.g. `"arm,pl061"`) -- the static-fabric identity.
    Compatible(String),
    /// A virtio device-id (e.g. `1` = net) behind a virtio-MMIO transport, from the
    /// virtio-mmio bus source. String form `"virtio:<n>"`.
    Virtio(u16),
    /// A virtio device-id (e.g. `1` = net) behind a virtio-PCI function, from the
    /// PCIe source (devpci). String form `"virtio-pci:<n>"`. Deliberately DISTINCT
    /// from `Virtio`: the two transports have different claim paths (a virtio-PCI
    /// driver claims its function via `SYS_PCI_CLAIM` + maps BARs; a virtio-mmio
    /// driver mints an MMIO handle over its allowance window), so a manifest binds
    /// exactly one and never collides the two.
    VirtioPci(u16),
}

impl DeviceId {
    /// Parse a `binds` / record identity string into a typed id. `"virtio-pci:<n>"`
    /// -> `VirtioPci`; `"virtio:<n>"` (n a u16) -> `Virtio`; anything else -> a
    /// literal `Compatible`. An unrecognized `bus:`-style prefix therefore stays
    /// `Compatible`, so it simply never matches a typed node -- fail-closed
    /// forward-compat (an old libdriver does not choke on a new bus id, it just
    /// declines it). A DTB `compatible` never contains `:`, so the typed namespace
    /// cannot collide with one. (`"virtio-pci:"` is checked first, but the two
    /// prefixes are disjoint anyway -- `"virtio-pci:1"` does not start with
    /// `"virtio:"`.)
    pub fn parse(s: &str) -> DeviceId {
        if let Some(rest) = s.strip_prefix("virtio-pci:") {
            if let Ok(n) = rest.parse::<u16>() {
                return DeviceId::VirtioPci(n);
            }
        }
        if let Some(rest) = s.strip_prefix("virtio:") {
            if let Ok(n) = rest.parse::<u16>() {
                return DeviceId::Virtio(n);
            }
        }
        DeviceId::Compatible(s.to_string())
    }

    /// The canonical string form -- the inverse of `parse` for the typed variants;
    /// the manifest `binds` form, the `BoundResources.compatible` field, and the
    /// wire record all use it.
    pub fn as_string(&self) -> String {
        match self {
            DeviceId::Compatible(s) => s.clone(),
            DeviceId::Virtio(n) => {
                let mut s = String::from("virtio:");
                s.push_str(&n.to_string());
                s
            }
            DeviceId::VirtioPci(n) => {
                let mut s = String::from("virtio-pci:");
                s.push_str(&n.to_string());
                s
            }
        }
    }

    /// Does this node identity satisfy a manifest `binds` entry? The `binds` string
    /// is parsed to a typed id and compared, so `"virtio:1"` matches `Virtio(1)`
    /// and `"arm,pl061"` matches `Compatible("arm,pl061")`, but a raw
    /// `Compatible("virtio,mmio")` transport node never matches a `"virtio:1"`
    /// bind (the source re-emits the typed node the driver actually binds).
    pub fn matches_bind(&self, bind: &str) -> bool {
        DeviceId::parse(bind) == *self
    }
}

/// One discovered device: its match identities (most-specific first), its physical
/// resources, and a diagnostic label (provenance -- the /hw node name, a bus slot
/// id). The label is never matched on; it is for logging + `/proc`-style
/// introspection only.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeviceNode {
    /// Provenance label (diagnostic only).
    pub label: String,
    /// Match identities, most-specific first.
    pub ids: Vec<DeviceId>,
    /// The node's physical resources (reg windows + wired INTIDs). For a
    /// bus-source node `resources.compatible` is empty -- the identity is in `ids`.
    pub resources: NodeResources,
}

impl DeviceNode {
    /// Build a DTB-style node from a compatible list + resources (the `DtbSource`
    /// shape -- ids are the compatibles, most-specific first).
    pub fn from_compatible(label: &str, resources: NodeResources) -> DeviceNode {
        let ids = resources
            .compatible
            .iter()
            .map(|c| DeviceId::Compatible(c.clone()))
            .collect();
        DeviceNode {
            label: label.to_string(),
            ids,
            resources,
        }
    }

    /// Encode for the source -> warden channel (one line, newline-terminated by the
    /// caller). Form:
    ///   `v1;label=<l>;id=<id>,<id>;reg=<base>:<size>,...;intid=<hex>,...`
    /// (numbers bare lowercase hex; `reg`/`intid` lists may be empty). Rejects a
    /// `label` containing `;`, or an `id` containing `;`/`,` (a bus id never does;
    /// a DTB `Compatible` can carry a comma, but DTB nodes are enumerated
    /// IN-PROCESS by the warden's `DtbSource` and never cross this channel -- only
    /// bus-source nodes do). Fail-closed on a delimiter, mirroring the descriptor.
    ///
    /// The record does NOT carry a PCI `pci` bdf: the only PCI source (`PciSource`)
    /// is in-process and never pipes through this channel, so a node with a bdf has
    /// no business being recorded. Rather than SILENTLY drop the bdf (which would
    /// degrade to a no-PCI grant downstream), `to_record` rejects it LOUDLY -- a
    /// future out-of-process PCIe source must extend the record format first.
    pub fn to_record(&self) -> Result<String, Error> {
        if self.label.contains(';') {
            return Err(Error::BadField);
        }
        if self.ids.is_empty() {
            return Err(Error::BadField);
        }
        if self.resources.pci.is_some() {
            return Err(Error::BadField); // the record cannot represent a PCI bdf (forward seam)
        }
        let mut s = String::new();
        s.push('v');
        s.push_str(&NODE_RECORD_VERSION.to_string());
        s.push_str(";label=");
        s.push_str(&self.label);
        s.push_str(";id=");
        for (i, id) in self.ids.iter().enumerate() {
            if i != 0 {
                s.push(',');
            }
            let ids = id.as_string();
            if ids.contains(';') || ids.contains(',') {
                return Err(Error::BadField);
            }
            s.push_str(&ids);
        }
        s.push_str(";reg=");
        for (i, (base, size)) in self.resources.reg.iter().enumerate() {
            if i != 0 {
                s.push(',');
            }
            push_hex(&mut s, *base);
            s.push(':');
            push_hex(&mut s, *size);
        }
        s.push_str(";intid=");
        for (i, intid) in self.resources.interrupts.iter().enumerate() {
            if i != 0 {
                s.push(',');
            }
            push_hex(&mut s, *intid as u64);
        }
        Ok(s)
    }

    /// Decode a node record the warden read from a source's pipe. The inverse of
    /// `to_record`. Strict + bounded (the discovery-source trust boundary): an
    /// unknown/duplicate key, a bad number, an unknown version, an empty id list,
    /// or a `reg`/`intid` count over the allowance caps all reject -- a hostile or
    /// buggy source cannot make the warden allocate unboundedly or mis-grant.
    pub fn parse_record(line: &str) -> Result<DeviceNode, Error> {
        let mut fields = line.split(';');

        let v = fields.next().ok_or(Error::BadVersion)?;
        let vnum = v.strip_prefix('v').ok_or(Error::BadVersion)?;
        let vnum: u32 = vnum.parse().map_err(|_| Error::BadVersion)?;
        if vnum != NODE_RECORD_VERSION {
            return Err(Error::BadVersion);
        }

        let mut label: Option<String> = None;
        let mut ids: Option<Vec<DeviceId>> = None;
        let mut reg: Option<Vec<(u64, u64)>> = None;
        let mut intids: Option<Vec<u32>> = None;

        for field in fields {
            if field.is_empty() {
                continue; // tolerate a stray trailing ';'
            }
            let (key, val) = field.split_once('=').ok_or(Error::BadField)?;
            match key {
                "label" => {
                    if label.is_some() {
                        return Err(Error::BadField);
                    }
                    label = Some(val.to_string());
                }
                "id" => {
                    if ids.is_some() {
                        return Err(Error::BadField);
                    }
                    let mut v = Vec::new();
                    for part in val.split(',') {
                        if part.is_empty() {
                            continue;
                        }
                        v.push(DeviceId::parse(part));
                        if v.len() > MAX_IDS {
                            return Err(Error::TooManyResources);
                        }
                    }
                    if v.is_empty() {
                        return Err(Error::BadField);
                    }
                    ids = Some(v);
                }
                "reg" => {
                    if reg.is_some() {
                        return Err(Error::BadField);
                    }
                    reg = Some(parse_windows(val)?);
                }
                "intid" => {
                    if intids.is_some() {
                        return Err(Error::BadField);
                    }
                    intids = Some(parse_irqs(val)?);
                }
                _ => return Err(Error::BadField),
            }
        }

        Ok(DeviceNode {
            label: label.unwrap_or_default(),
            ids: ids.ok_or(Error::BadField)?,
            resources: NodeResources {
                compatible: Vec::new(),
                reg: reg.unwrap_or_default(),
                interrupts: intids.unwrap_or_default(),
                // The wire record does not carry a PCI bdf: the only PCI source
                // (devpci) is enumerated IN-PROCESS by the warden's `PciSource`
                // and never crosses this source->warden pipe. A v1.x out-of-process
                // PCIe source extends the record then; today it is a forward seam.
                pci: None,
            },
        })
    }
}

/// A source of typed device nodes. The warden runs each source's `enumerate` and
/// binds the union. Best-effort: a source logs + skips a malformed node and
/// returns what it found (a fatal source error yields an empty `Vec`), so one bad
/// node never sinks the whole discovery pass.
pub trait DiscoverySource {
    fn enumerate(&mut self) -> Vec<DeviceNode>;
}

/// Find the bind-database manifest that binds this node at its most-specific
/// identity (the earliest id in the node's most-specific-first list). Returns the
/// manifest index, or `None` if no manifest binds the node. (The warden's match
/// step -- generalized off `compatible`-strings to typed `DeviceId`s.)
pub fn best_match(db: &[Manifest], node: &DeviceNode) -> Option<usize> {
    let mut best: Option<(usize, usize)> = None; // (db index, node-id position)
    for (i, m) in db.iter().enumerate() {
        for (pos, id) in node.ids.iter().enumerate() {
            if m.binds.iter().any(|b| id.matches_bind(b)) {
                let better = match best {
                    None => true,
                    Some((_, bp)) => pos < bp,
                };
                if better {
                    best = Some((i, pos));
                }
                break; // the node's first hit is this manifest's most-specific match
            }
        }
    }
    best.map(|(i, _)| i)
}

/// Constrain a source-reported node to the warden's trusted device view -- the
/// discovery-source trust boundary, ENFORCED rather than trusted. A bus source is
/// non-TCB (MENAGERIE section 3): the warden trusts it to *identify* a slot (read
/// its DeviceID -> a typed id), never to *describe* one. This matches the reported
/// node to a trusted slot by its reg base and rebuilds the node's resources (reg +
/// INTID) from that trusted slot, so a source can only mis-identify a real slot
/// (caught downstream by the driver's own device re-validation) -- it can never
/// fabricate a reg/INTID outside the domain the warden granted it, nor inflate a
/// peer driver's conferred allowance. Returns the reconciled node (the reported
/// identity + the trusted resources), or `None` if the reported node names no
/// trusted slot (a fabricated address). Generalizes to the recursive PCIe/USB
/// sources: the parent's granted slot table is always the containment bound.
pub fn reconcile_reported_node(reported: &DeviceNode, trusted: &[DeviceNode]) -> Option<DeviceNode> {
    let base = reported.resources.reg.first().map(|(b, _)| *b)?;
    let slot = trusted
        .iter()
        .find(|s| s.resources.reg.first().map(|(b, _)| *b) == Some(base))?;
    Some(DeviceNode {
        label: reported.label.clone(),
        ids: reported.ids.clone(),
        resources: NodeResources {
            compatible: Vec::new(),
            reg: slot.resources.reg.clone(),
            interrupts: slot.resources.interrupts.clone(),
            // Take the trusted slot's bdf, never the reported one -- same discipline
            // as reg/INTID. (reconcile keys on the reg base, so it is the virtio-mmio
            // path; a PCI source is the in-process `PciSource`, which is itself the
            // kernel-trusted view and bypasses reconcile.)
            pci: slot.resources.pci,
        },
    })
}

/// Parse one devpci `ctl` line into a typed virtio-PCI device node. The line is
/// the kernel-mediated topology channel (`docs/reference/120-devpci.md`):
///
///   `v1 bus=<hex> dev=<hex> fn=<hex> vendor=<hex> device=<hex> virtio=<dec> intid=<hex|none>`
///
/// `label` is the source's provenance tag (the `<bdf>` directory name). Numbers
/// are bare lowercase hex EXCEPT `virtio`, which is DECIMAL (matching the
/// `virtio-pci:<n>` id convention). `intid=none` (or an absent intid) yields no
/// wired IRQ -- the function declares no INTx pin, and MSI is a v1.x path.
///
/// Returns `None` on any malformed line (wrong/absent version token, a missing
/// required field, a bad number) OR a `virtio=0` (no virtio identity): the source
/// logs + skips, best-effort like every source, and devpci lists only virtio
/// functions today. devpci is the KERNEL (the TCB), so this parser is lenient --
/// it tolerates an unknown appended field (the forward-compat the `v1` contract
/// promises) rather than the hostile-source-hardened strictness of `parse_record`
/// (which is the non-TCB bus-source boundary).
///
/// CONTRACT: apply this ONLY to a kernel-mediated devpci read (the in-process
/// `PciSource`). Its leniency is sound exactly because the reporter is the TCB; a
/// future OUT-OF-PROCESS PCIe source carrying untrusted pipe bytes MUST instead go
/// through `parse_record`'s strict, count-bounded codec (and `to_record` already
/// LOUD-rejects a PCI node for the same reason -- a bdf cannot ride the pipe until
/// the record format is extended). It is memory-safe on any input regardless (only
/// `?`/`.ok()?`, no `unsafe`), so a misuse degrades to `None`, never to a hole.
pub fn parse_pci_ctl(label: &str, line: &str) -> Option<DeviceNode> {
    let mut it = line.split_whitespace();
    if it.next()? != "v1" {
        return None; // version token absent or not v1
    }

    let mut bus: Option<u8> = None;
    let mut dev: Option<u8> = None;
    let mut func: Option<u8> = None;
    let mut virtio: Option<u16> = None;
    let mut intid: Option<u32> = None; // Some only when a real INTID is present

    for field in it {
        let (key, val) = field.split_once('=')?;
        match key {
            "bus" => bus = Some(u8::from_str_radix(val, 16).ok()?),
            "dev" => dev = Some(u8::from_str_radix(val, 16).ok()?),
            "fn" => func = Some(u8::from_str_radix(val, 16).ok()?),
            // vendor/device are informational; validate them as a light integrity
            // gate (they are genuinely u16) but do not carry them into the node.
            "vendor" | "device" => {
                u16::from_str_radix(val, 16).ok()?;
            }
            "virtio" => virtio = Some(val.parse::<u16>().ok()?), // DECIMAL
            "intid" => {
                if val != "none" {
                    intid = Some(u32::from_str_radix(val, 16).ok()?);
                }
            }
            _ => {} // tolerate an unknown appended field (the v1 forward-compat)
        }
    }

    let bus = bus?;
    let dev = dev?;
    let func = func?;
    let virtio = virtio?;
    if virtio == 0 {
        return None; // not a virtio function -> not bound by a virtio-PCI manifest
    }

    let interrupts = match intid {
        Some(id) => alloc::vec![id],
        None => Vec::new(),
    };
    Some(DeviceNode {
        label: label.to_string(),
        ids: alloc::vec![DeviceId::VirtioPci(virtio)],
        resources: NodeResources {
            compatible: Vec::new(),
            reg: Vec::new(), // a virtio-PCI function exposes BARs, not DTB reg windows
            interrupts,
            pci: Some((bus, dev, func)),
        },
    })
}

// =============================================================================
// DtbSource -- the concrete /hw source (feature `driver`; needs libthyla-rs).
// =============================================================================

#[cfg(feature = "driver")]
mod dtb_source {
    use alloc::string::{String, ToString};
    use alloc::vec::Vec;

    use libthyla_rs::fs;
    use libthyla_rs::io::Read;

    use super::{DeviceNode, DiscoverySource};
    use crate::dtb::{ARM64_ADDR_CELLS, ARM64_SIZE_CELLS, GIC_INTERRUPT_CELLS};
    use crate::resource::NodeResources;

    /// The DTB discovery source (MENAGERIE section 7): enumerates `/hw` (the devhw
    /// DTB tree) and emits one `DeviceNode` per node that exposes a `compatible`,
    /// identified by its `Compatible` ids. The static-fabric source + the
    /// bootstrap root every other source layers on. Top-level only -- every
    /// bindable device on the v1.0 targets (QEMU-virt, RPi4/5) is a direct child
    /// of the FDT root; descending nested buses is a v1.x refinement.
    pub struct DtbSource {
        root: String,
    }

    impl DtbSource {
        /// A source rooted at the conventional `/hw` mount.
        pub fn new() -> Self {
            DtbSource {
                root: String::from("/hw"),
            }
        }

        /// A source rooted at an arbitrary path (for an alternate devhw mount).
        pub fn at(root: &str) -> Self {
            DtbSource {
                root: root.to_string(),
            }
        }
    }

    impl Default for DtbSource {
        fn default() -> Self {
            Self::new()
        }
    }

    impl DiscoverySource for DtbSource {
        fn enumerate(&mut self) -> Vec<DeviceNode> {
            let mut out = Vec::new();
            let rd = match fs::read_dir(&self.root) {
                Ok(r) => r,
                Err(_) => return out, // no /hw -> no DTB nodes (best-effort)
            };
            for ent in rd {
                let ent = match ent {
                    Ok(e) => e,
                    Err(_) => continue,
                };
                if !ent.is_dir() {
                    continue; // skip the root-level property files
                }
                let name = ent.file_name();
                let res = read_node(&self.root, name);
                if res.compatible.is_empty() {
                    continue; // a node with no `compatible` is not a bindable device
                }
                out.push(DeviceNode::from_compatible(name, res));
            }
            out
        }
    }

    /// Read + decode a node's `compatible`/`reg`/`interrupts` property files.
    fn read_node(root: &str, name: &str) -> NodeResources {
        NodeResources::from_dtb(
            &read_prop(root, name, "compatible"),
            &read_prop(root, name, "reg"),
            &read_prop(root, name, "interrupts"),
            ARM64_ADDR_CELLS,
            ARM64_SIZE_CELLS,
            GIC_INTERRUPT_CELLS,
        )
    }

    /// Read one `/hw/<node>/<prop>` property file into bytes. A missing property
    /// yields an empty buffer -- the decoder treats that as an absent axis.
    fn read_prop(root: &str, node: &str, prop: &str) -> Vec<u8> {
        let path = alloc::format!("{}/{}/{}", root, node, prop);
        let mut f = match fs::File::open(&path) {
            Ok(f) => f,
            Err(_) => return Vec::new(),
        };
        let mut buf = Vec::new();
        if f.read_to_end(&mut buf).is_err() {
            return Vec::new();
        }
        buf
    }
}

#[cfg(feature = "driver")]
pub use dtb_source::DtbSource;

// =============================================================================
// PciSource -- the in-process /hw/pci source (feature `driver`; needs libthyla-rs).
// =============================================================================

#[cfg(feature = "driver")]
mod pci_source {
    use alloc::string::{String, ToString};
    use alloc::vec::Vec;

    use libthyla_rs::fs;
    use libthyla_rs::io::Read;

    use super::{parse_pci_ctl, DeviceNode, DiscoverySource};

    /// The PCIe discovery source (MENAGERIE section 7, the mediated realization):
    /// enumerates `/hw/pci` (the kernel-mediated [devpci](../../../docs/reference/120-devpci.md)
    /// topology) and emits one `VirtioPci` `DeviceNode` per function. The DtbSource
    /// sibling for the PCI bus: on QEMU-virt the kernel owns ECAM and re-publishes
    /// the enumerated functions at `/hw/pci/<bdf>/ctl`, so this source is itself the
    /// kernel-TRUSTED view -- it reads the mediated result IN-PROCESS, never poking
    /// config space, never spawning a poking Proc, and (unlike the non-TCB
    /// virtio-mmio bus source) needs no `reconcile_reported_node` step. Best-effort:
    /// a malformed `ctl` line is logged + skipped; a missing `/hw/pci` yields no
    /// nodes. Top-level only (every QEMU-virt virtio-PCI function is a direct child
    /// of the root bridge); nested-bridge descent is a v1.x refinement.
    pub struct PciSource {
        root: String,
    }

    impl PciSource {
        /// A source rooted at the conventional `/hw/pci` mount.
        pub fn new() -> Self {
            PciSource {
                root: String::from("/hw/pci"),
            }
        }

        /// A source rooted at an arbitrary path (for an alternate devpci mount).
        pub fn at(root: &str) -> Self {
            PciSource {
                root: root.to_string(),
            }
        }
    }

    impl Default for PciSource {
        fn default() -> Self {
            Self::new()
        }
    }

    impl DiscoverySource for PciSource {
        fn enumerate(&mut self) -> Vec<DeviceNode> {
            let mut out = Vec::new();
            let rd = match fs::read_dir(&self.root) {
                Ok(r) => r,
                Err(_) => return out, // no /hw/pci -> no PCI nodes (best-effort)
            };
            for ent in rd {
                let ent = match ent {
                    Ok(e) => e,
                    Err(_) => continue,
                };
                if !ent.is_dir() {
                    continue; // the root holds only <bdf> directories
                }
                let bdf = ent.file_name();
                if let Some(node) = read_function(&self.root, bdf) {
                    out.push(node);
                }
            }
            out
        }
    }

    /// Read + parse one function's `<bdf>/ctl` line into a typed node. The provenance
    /// label is the `<bdf>` directory name (e.g. `"0.1.0"`).
    fn read_function(root: &str, bdf: &str) -> Option<DeviceNode> {
        let path = alloc::format!("{}/{}/ctl", root, bdf);
        let mut f = fs::File::open(&path).ok()?;
        let mut buf = Vec::new();
        f.read_to_end(&mut buf).ok()?;
        let line = core::str::from_utf8(&buf).ok()?;
        parse_pci_ctl(bdf, line)
    }
}

#[cfg(feature = "driver")]
pub use pci_source::PciSource;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::Manifest;

    fn vnode(label: &str, virtio_id: u16, reg: (u64, u64), intid: u32) -> DeviceNode {
        DeviceNode {
            label: label.to_string(),
            ids: alloc::vec![DeviceId::Virtio(virtio_id)],
            resources: NodeResources {
                compatible: Vec::new(),
                reg: alloc::vec![reg],
                interrupts: alloc::vec![intid],
                pci: None,
            },
        }
    }

    fn manifest(binds: &str, name: &str) -> Manifest {
        let src = alloc::format!(
            "driver \"{name}\" {{ abi = 1 binds = [{binds}] \
             needs {{ mmio = \"node:reg\" irq = \"node:interrupts\" }} \
             serves = \"/dev/{name}/%instance\" }}"
        );
        Manifest::parse(&src).unwrap()
    }

    #[test]
    fn device_id_parse_and_string() {
        assert_eq!(DeviceId::parse("virtio:1"), DeviceId::Virtio(1));
        assert_eq!(DeviceId::parse("virtio:65535"), DeviceId::Virtio(65535));
        // out-of-range / non-numeric virtio suffix falls back to a literal compatible
        assert_eq!(
            DeviceId::parse("virtio:99999"),
            DeviceId::Compatible("virtio:99999".to_string())
        );
        assert_eq!(
            DeviceId::parse("arm,pl061"),
            DeviceId::Compatible("arm,pl061".to_string())
        );
        assert_eq!(DeviceId::Virtio(1).as_string(), "virtio:1");
        assert_eq!(DeviceId::Compatible("arm,pl061".to_string()).as_string(), "arm,pl061");
    }

    #[test]
    fn matches_bind_typed_vs_literal() {
        assert!(DeviceId::Virtio(1).matches_bind("virtio:1"));
        assert!(!DeviceId::Virtio(1).matches_bind("virtio:2"));
        assert!(DeviceId::Compatible("arm,pl061".to_string()).matches_bind("arm,pl061"));
        // a raw transport-node compatible never matches a typed bus bind
        assert!(!DeviceId::Compatible("virtio,mmio".to_string()).matches_bind("virtio:1"));
        // and a typed id never matches the raw transport compatible
        assert!(!DeviceId::Virtio(1).matches_bind("virtio,mmio"));
    }

    #[test]
    fn node_record_round_trip_virtio() {
        let n = vnode("virtio-slot-31", 1, (0x0a00_3e00, 0x200), 0x2f);
        let rec = n.to_record().unwrap();
        assert_eq!(
            rec,
            "v1;label=virtio-slot-31;id=virtio:1;reg=a003e00:200;intid=2f"
        );
        let n2 = DeviceNode::parse_record(&rec).unwrap();
        assert_eq!(n, n2);
    }

    #[test]
    fn node_record_empty_resource_axes() {
        let n = DeviceNode {
            label: "x".to_string(),
            ids: alloc::vec![DeviceId::Virtio(4)],
            resources: NodeResources::default(),
        };
        let rec = n.to_record().unwrap();
        let n2 = DeviceNode::parse_record(&rec).unwrap();
        assert_eq!(n, n2);
        assert!(n2.resources.reg.is_empty());
        assert!(n2.resources.interrupts.is_empty());
    }

    #[test]
    fn node_record_rejects_bad_input() {
        assert_eq!(
            DeviceNode::parse_record("v2;id=virtio:1"),
            Err(Error::BadVersion)
        );
        assert_eq!(
            DeviceNode::parse_record("id=virtio:1"),
            Err(Error::BadVersion) // no v-prefix
        );
        assert_eq!(
            DeviceNode::parse_record("v1;label=x;reg=1000:1000"),
            Err(Error::BadField) // no id
        );
        assert_eq!(
            DeviceNode::parse_record("v1;id=virtio:1;id=virtio:2"),
            Err(Error::BadField) // duplicate id key
        );
        assert_eq!(
            DeviceNode::parse_record("v1;id=virtio:1;reg=deadbeef"),
            Err(Error::BadNumber) // window without :size
        );
        assert_eq!(
            DeviceNode::parse_record("v1;id=virtio:1;bogus=1"),
            Err(Error::BadField) // unknown key
        );
    }

    #[test]
    fn node_record_bounds_resource_counts() {
        // a hostile source over-reporting windows is rejected at parse (the
        // discovery-source trust boundary), not silently truncated.
        let mut rec = String::from("v1;id=virtio:1;reg=");
        for i in 0..(crate::resource::MAX_MMIO + 1) {
            if i != 0 {
                rec.push(',');
            }
            rec.push_str("1000:1000");
        }
        assert_eq!(
            DeviceNode::parse_record(&rec),
            Err(Error::TooManyResources)
        );
    }

    #[test]
    fn to_record_rejects_delimiter_in_label() {
        let mut n = vnode("ev;il", 1, (0x1000, 0x100), 5);
        assert_eq!(n.to_record(), Err(Error::BadField));
        n.label = "ok".to_string();
        assert!(n.to_record().is_ok());
    }

    #[test]
    fn best_match_picks_most_specific_and_typed() {
        let db = alloc::vec![manifest("\"virtio:1\"", "netdev"), manifest("\"virtio:2\"", "blkdev")];
        // a virtio:1 node binds netdev (index 0)
        let net = vnode("slot", 1, (0x0a00_3e00, 0x200), 0x2f);
        assert_eq!(best_match(&db, &net), Some(0));
        // a virtio:2 node binds blkdev (index 1)
        let blk = vnode("slot", 2, (0x0a00_3c00, 0x200), 0x2e);
        assert_eq!(best_match(&db, &blk), Some(1));
        // an unbound virtio id matches nothing
        let rng = vnode("slot", 4, (0x0a00_3a00, 0x200), 0x2d);
        assert_eq!(best_match(&db, &rng), None);
    }

    #[test]
    fn best_match_most_specific_id_wins() {
        // a manifest binding the LESS specific id still binds, but a node whose
        // most-specific id is bound by an earlier-listed manifest prefers that.
        let db = alloc::vec![manifest("\"specific\"", "a"), manifest("\"generic\"", "b")];
        let node = DeviceNode {
            label: "n".to_string(),
            ids: alloc::vec![
                DeviceId::Compatible("specific".to_string()),
                DeviceId::Compatible("generic".to_string()),
            ],
            resources: NodeResources::default(),
        };
        // "specific" is the node's most-specific id (position 0) -> manifest a.
        assert_eq!(best_match(&db, &node), Some(0));
    }

    fn trusted_slot(base: u64, intid: u32) -> DeviceNode {
        // a raw virtio,mmio DTB slot as the warden holds it (the trusted view).
        DeviceNode {
            label: "virtio_mmio".to_string(),
            ids: alloc::vec![DeviceId::Compatible("virtio,mmio".to_string())],
            resources: NodeResources {
                compatible: alloc::vec!["virtio,mmio".to_string()],
                reg: alloc::vec![(base, 0x200)],
                interrupts: alloc::vec![intid],
                pci: None,
            },
        }
    }

    #[test]
    fn reconcile_honest_node_keeps_identity_takes_trusted_resources() {
        let trusted = alloc::vec![trusted_slot(0x0a00_3a00, 0x2f), trusted_slot(0x0a00_3c00, 0x2e)];
        // an honest source reports the real slot's reg + intid
        let reported = vnode("net", 1, (0x0a00_3a00, 0x200), 0x2f);
        let r = reconcile_reported_node(&reported, &trusted).unwrap();
        assert_eq!(r.ids, alloc::vec![DeviceId::Virtio(1)]); // identity preserved
        assert_eq!(r.resources.reg, alloc::vec![(0x0a00_3a00u64, 0x200u64)]);
        assert_eq!(r.resources.interrupts, alloc::vec![0x2f]);
    }

    #[test]
    fn reconcile_rejects_fabricated_address() {
        let trusted = alloc::vec![trusted_slot(0x0a00_3a00, 0x2f)];
        // a hostile source reports a typed node at a PA outside its granted domain
        let forged = vnode("evil", 1, (0xdead_0000, 0x1000), 0x2f);
        assert_eq!(reconcile_reported_node(&forged, &trusted), None);
    }

    #[test]
    fn reconcile_discards_fabricated_resources_for_a_real_slot() {
        // the security-proving case: a source reports a REAL slot base but inflates
        // the window + lies about the intid. The reconciled node MUST carry the
        // trusted slot's reg/intid, never the source's fabrication -- so the warden
        // cannot be made to confer a wider allowance than the slot.
        let trusted = alloc::vec![trusted_slot(0x0a00_3a00, 0x2f)];
        let inflated = vnode("greedy", 1, (0x0a00_3a00, 0x10_0000), 0x07);
        let r = reconcile_reported_node(&inflated, &trusted).unwrap();
        assert_eq!(r.resources.reg, alloc::vec![(0x0a00_3a00u64, 0x200u64)]); // trusted size, not 0x100000
        assert_eq!(r.resources.interrupts, alloc::vec![0x2f]); // trusted intid, not 0x07
    }

    #[test]
    fn parse_record_bounds_id_count() {
        // a hostile source over-reporting ids is rejected at parse (the id list got
        // the same count-bound the reg/intid lists have).
        let mut rec = String::from("v1;id=");
        for i in 0..(MAX_IDS + 1) {
            if i != 0 {
                rec.push(',');
            }
            rec.push_str("virtio:1");
        }
        assert_eq!(DeviceNode::parse_record(&rec), Err(Error::TooManyResources));
    }

    // -------------------------------------------------------------------------
    // virtio-PCI identity + the devpci ctl-line parser (6b-2)
    // -------------------------------------------------------------------------

    #[test]
    fn device_id_virtio_pci_parse_and_string() {
        assert_eq!(DeviceId::parse("virtio-pci:1"), DeviceId::VirtioPci(1));
        assert_eq!(DeviceId::parse("virtio-pci:65535"), DeviceId::VirtioPci(65535));
        // out-of-range suffix -> a literal compatible (fail-closed forward-compat)
        assert_eq!(
            DeviceId::parse("virtio-pci:99999"),
            DeviceId::Compatible("virtio-pci:99999".to_string())
        );
        assert_eq!(DeviceId::VirtioPci(1).as_string(), "virtio-pci:1");
        // round-trip
        assert_eq!(DeviceId::parse(&DeviceId::VirtioPci(2).as_string()), DeviceId::VirtioPci(2));
    }

    #[test]
    fn virtio_pci_distinct_from_virtio_transport() {
        // the load-bearing separation: the MMIO and PCI virtio transports never
        // collide, so a manifest binds exactly one.
        assert_ne!(DeviceId::VirtioPci(1), DeviceId::Virtio(1));
        assert!(DeviceId::VirtioPci(1).matches_bind("virtio-pci:1"));
        assert!(!DeviceId::VirtioPci(1).matches_bind("virtio:1"));
        assert!(!DeviceId::Virtio(1).matches_bind("virtio-pci:1"));
        // "virtio:1" still parses to the MMIO id even now that the longer prefix exists
        assert_eq!(DeviceId::parse("virtio:1"), DeviceId::Virtio(1));
    }

    #[test]
    fn parse_pci_ctl_happy_path() {
        // a full devpci line for a virtio-net (id 1) function at bdf 0.1.0, INTID 0x23.
        let line = "v1 bus=0 dev=1 fn=0 vendor=1af4 device=1041 virtio=1 intid=23";
        let n = parse_pci_ctl("0.1.0", line).expect("parse");
        assert_eq!(n.label, "0.1.0");
        assert_eq!(n.ids, alloc::vec![DeviceId::VirtioPci(1)]);
        assert_eq!(n.resources.pci, Some((0u8, 1u8, 0u8)));
        assert_eq!(n.resources.interrupts, alloc::vec![0x23u32]);
        assert!(n.resources.reg.is_empty()); // PCI BARs are not DTB reg windows
    }

    #[test]
    fn parse_pci_ctl_intid_none_yields_no_irq() {
        let line = "v1 bus=2 dev=a fn=1 vendor=1af4 device=1041 virtio=1 intid=none";
        let n = parse_pci_ctl("2.a.1", line).expect("parse");
        assert_eq!(n.resources.pci, Some((2u8, 0x0au8, 1u8)));
        assert!(n.resources.interrupts.is_empty());
    }

    #[test]
    fn parse_pci_ctl_tolerates_unknown_appended_field() {
        // the v1 forward-compat contract: an unknown appended key is ignored, not
        // a rejection (devpci is the kernel/TCB; the parser is lenient).
        let line = "v1 bus=0 dev=1 fn=0 vendor=1af4 device=1041 virtio=1 intid=23 future=xyz";
        let n = parse_pci_ctl("0.1.0", line).expect("parse");
        assert_eq!(n.ids, alloc::vec![DeviceId::VirtioPci(1)]);
    }

    #[test]
    fn parse_pci_ctl_rejects_malformed() {
        // wrong version token
        assert!(parse_pci_ctl("0.1.0", "v2 bus=0 dev=1 fn=0 virtio=1").is_none());
        // absent version token
        assert!(parse_pci_ctl("0.1.0", "bus=0 dev=1 fn=0 virtio=1").is_none());
        // missing a required bdf component
        assert!(parse_pci_ctl("0.1.0", "v1 bus=0 fn=0 virtio=1").is_none());
        // missing virtio
        assert!(parse_pci_ctl("0.1.0", "v1 bus=0 dev=1 fn=0").is_none());
        // garbled hex
        assert!(parse_pci_ctl("0.1.0", "v1 bus=zz dev=1 fn=0 virtio=1").is_none());
        // a field with no '='
        assert!(parse_pci_ctl("0.1.0", "v1 bus=0 dev 1 fn=0 virtio=1").is_none());
        // virtio must be decimal -- "1a" hex is not a decimal u16
        assert!(parse_pci_ctl("0.1.0", "v1 bus=0 dev=1 fn=0 virtio=1a").is_none());
        // empty line
        assert!(parse_pci_ctl("0.1.0", "").is_none());
    }

    #[test]
    fn parse_pci_ctl_skips_non_virtio_function() {
        // a virtio=0 function (no virtio identity) is not bound by a virtio-PCI
        // manifest -> skipped (devpci lists only virtio functions today anyway).
        let line = "v1 bus=0 dev=2 fn=0 vendor=1234 device=5678 virtio=0 intid=none";
        assert!(parse_pci_ctl("0.2.0", line).is_none());
    }

    #[test]
    fn best_match_binds_virtio_pci_node() {
        let db = alloc::vec![manifest("\"virtio-pci:1\"", "netpci"), manifest("\"virtio:1\"", "netmmio")];
        let line = "v1 bus=0 dev=1 fn=0 vendor=1af4 device=1041 virtio=1 intid=23";
        let node = parse_pci_ctl("0.1.0", line).unwrap();
        // binds the PCI manifest (index 0), NOT the MMIO one
        assert_eq!(best_match(&db, &node), Some(0));
    }

    #[test]
    fn to_record_rejects_a_pci_node() {
        // the record (the source->warden PIPE) cannot represent a PCI bdf; encoding
        // a VirtioPci node must fail LOUDLY rather than silently drop the bdf (the
        // in-process PciSource never pipes; a future out-of-process source extends
        // the record first). The non-regression witness that the PCI gate did NOT
        // break the live virtio-mmio producer is `node_record_round_trip_virtio`
        // (a non-PCI node still encodes + round-trips after this gate landed).
        let node = parse_pci_ctl(
            "0.1.0",
            "v1 bus=0 dev=1 fn=0 vendor=1af4 device=1041 virtio=1 intid=23",
        )
        .unwrap();
        assert!(node.resources.pci.is_some());
        assert_eq!(node.to_record(), Err(Error::BadField));
    }
}
