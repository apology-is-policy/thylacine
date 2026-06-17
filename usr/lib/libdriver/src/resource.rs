// The resource model -- the shared currency between the warden (which computes
// + encodes a grant) and the driver (which decodes it + mints handles).
//
// Two types and two operations:
//   - `NodeResources`  -- what a device node physically exposes (the warden
//     reads it from /hw, the devhw DTB tree).
//   - `BoundResources` -- the narrowed grant: the node's resources, selected by
//     the manifest's `needs`, never wider than the node. This is the auditable
//     I-34 grant.
//   - `resolve(manifest, node, instance)` -- the intersection that produces a
//     `BoundResources` from a matched manifest + node. The warden's grant
//     computation, in one host-tested function.
//   - `BoundResources::{to_descriptor, parse_descriptor}` -- the single-argv-slot
//     codec that carries the grant across the spawn boundary (the warden encodes
//     into `Command::arg`; the driver decodes in `bind`).
//
// PURE (no libthyla-rs): the grant logic + codec run on the host. The
// device-side `to_allowance` (which needs the kernel `TAllowanceDesc`) lives in
// `driver.rs`, but its values come straight from a `BoundResources` here, so the
// allowance the kernel enforces and the resources the driver creates handles for
// are one source of truth.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::manifest::{DmaNeed, IrqNeed, Manifest, MmioNeed, PciNeed};
use crate::source::DeviceNode;
use crate::Error;

/// The spawn-descriptor format version (the `v1` prefix). Bumped only on an
/// incompatible codec change; the driver refuses an unknown version (fail-closed
/// on a warden/driver build skew).
pub const DESCRIPTOR_VERSION: u32 = 1;

/// Max MMIO windows in a grant -- mirrors `T_ALLOWANCE_MMIO_MAX` (the kernel
/// allowance cap). `resolve` + the codec reject more.
pub const MAX_MMIO: usize = 8;

/// Max IRQs in a grant -- mirrors `T_ALLOWANCE_IRQ_MAX`.
pub const MAX_IRQ: usize = 8;

/// A device node's physical resources, as the warden reads them from /hw.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NodeResources {
    /// The node's `compatible` list, most-specific first (the DTB order).
    pub compatible: Vec<String>,
    /// The node's `reg` MMIO windows as `(base, size)` byte pairs.
    pub reg: Vec<(u64, u64)>,
    /// The node's wired GIC INTIDs (from `interrupts`, mapped to absolute INTID).
    pub interrupts: Vec<u32>,
    /// The node's PCI `(bus, dev, fn)`, for a virtio-PCI source node (from devpci).
    /// `None` for a DTB node (it has no bdf). The PCI allowance axis (I-34, 6a).
    pub pci: Option<(u8, u8, u8)>,
}

/// The narrowed resources conferred on a driver at spawn -- the auditable I-34
/// grant. `mmio`/`irq` are a subset of the bound node's resources; `dma_max` is
/// the manifest's declared budget.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BoundResources {
    /// The per-bind instance number (`%instance` in the served path).
    pub instance: u32,
    /// The matched `compatible` (which `binds` entry the node hit) -- informational
    /// so a multi-bind driver can branch on the actual device.
    pub compatible: String,
    /// The resolved service path (`%instance` expanded) the driver publishes.
    pub serves: String,
    /// The granted MMIO windows as `(base, size)` -- a subset of the node's `reg`.
    pub mmio: Vec<(u64, u64)>,
    /// The granted wired IRQ INTIDs -- a subset of the node's `interrupts`.
    pub irq: Vec<u32>,
    /// The per-buffer DMA byte ceiling (0 = no DMA).
    pub dma_max: u64,
    /// The granted PCI function `(bus, dev, fn)` the driver may `SYS_PCI_CLAIM`, or
    /// `None`. Equals the bound node's `pci` (never fabricated -- the I-34 grant
    /// property), or `None` when the manifest declines PCI or the node supplies none.
    pub pci: Option<(u8, u8, u8)>,
}

/// Compute the narrowed grant for a matched (manifest, node) pair.
///
/// The auditable property (I-34): for each axis the manifest *selects*, the node
/// *supplies* the values, so the grant never exceeds the device's physical
/// resources. A manifest cannot widen its reach; it can only decline an axis.
///
/// The node is matched on its typed identities (`node.ids`, most-specific first);
/// the matched id's string form is recorded in `BoundResources.compatible` (a DTB
/// compatible string, or a bus id like `"virtio:1"`).
///
/// - MMIO: `MmioNeed::Node` grants every `reg` window; `None` grants nothing.
/// - IRQ:  `IrqNeed::Node` grants every wired INTID; `Msi(_)` is resolved by the
///   PCIe source (not a DTB node) so it grants nothing here; `None` grants nothing.
/// - DMA:  the manifest's `Pool(bytes)` budget (DMA is allocated memory, not a
///   node resource), or 0.
///
/// Errors: `NoMatch` (no node id is in `binds`), `TooManyWindows` /
/// `TooManyIrqs` (the node exposes more than the allowance can carry).
pub fn resolve(
    manifest: &Manifest,
    node: &DeviceNode,
    instance: u32,
) -> Result<BoundResources, Error> {
    let matched = node
        .ids
        .iter()
        .find(|id| manifest.binds.iter().any(|b| id.matches_bind(b)))
        .ok_or(Error::NoMatch)?;

    let mmio = match manifest.needs.mmio {
        MmioNeed::None => Vec::new(),
        MmioNeed::Node => {
            if node.resources.reg.len() > MAX_MMIO {
                return Err(Error::TooManyWindows);
            }
            node.resources.reg.clone()
        }
    };

    let irq = match manifest.needs.irq {
        IrqNeed::None => Vec::new(),
        IrqNeed::Node => {
            if node.resources.interrupts.len() > MAX_IRQ {
                return Err(Error::TooManyIrqs);
            }
            node.resources.interrupts.clone()
        }
        // MSI vectors are provided by the PCIe source, not a DTB node. The grant
        // carries the count via the manifest; the source fills the INTIDs at the
        // real-hardware path. On a DTB node there is nothing to intersect.
        IrqNeed::Msi(_) => Vec::new(),
    };

    let dma_max = match manifest.needs.dma {
        DmaNeed::None => 0,
        DmaNeed::Pool(bytes) => bytes,
    };

    // PCI: the bound node's own bdf (never fabricated -- the I-34 grant property).
    // A manifest asking `PciNeed::Node` against a node with no bdf (a DTB node)
    // gets None -- fail-closed, the same lenience the MMIO/IRQ axes have when the
    // node supplies nothing.
    let pci = match manifest.needs.pci {
        PciNeed::None => None,
        PciNeed::Node => node.resources.pci,
    };

    Ok(BoundResources {
        instance,
        compatible: matched.as_string(),
        serves: expand_instance(&manifest.serves, instance),
        mmio,
        irq,
        dma_max,
        pci,
    })
}

/// Expand `%instance` in a served-path template to the instance number.
pub fn expand_instance(template: &str, instance: u32) -> String {
    template.replace("%instance", &instance.to_string())
}

impl BoundResources {
    /// Encode the grant into one argv element the warden passes to the driver.
    ///
    /// Form: `v1;inst=0;compat=...;serve=...;mmio=base:size,...;irq=id,...;dma=hex`
    /// (numbers hex except `inst`; `mmio`/`irq` omitted when empty; `dma` always).
    /// Rejects a `compat`/`serve` containing the field delimiter `;` (a DTB
    /// compatible / service path never does -- fail-closed against a malformed
    /// node), or a resource count over the allowance caps.
    pub fn to_descriptor(&self) -> Result<String, Error> {
        if self.mmio.len() > MAX_MMIO {
            return Err(Error::TooManyResources);
        }
        if self.irq.len() > MAX_IRQ {
            return Err(Error::TooManyResources);
        }
        if self.compatible.contains(';') || self.serves.contains(';') {
            return Err(Error::BadField);
        }
        let mut s = String::new();
        s.push('v');
        s.push_str(&DESCRIPTOR_VERSION.to_string());
        s.push_str(";inst=");
        s.push_str(&self.instance.to_string());
        s.push_str(";compat=");
        s.push_str(&self.compatible);
        s.push_str(";serve=");
        s.push_str(&self.serves);
        if !self.mmio.is_empty() {
            s.push_str(";mmio=");
            for (i, (base, size)) in self.mmio.iter().enumerate() {
                if i != 0 {
                    s.push(',');
                }
                push_hex(&mut s, *base);
                s.push(':');
                push_hex(&mut s, *size);
            }
        }
        if !self.irq.is_empty() {
            s.push_str(";irq=");
            for (i, intid) in self.irq.iter().enumerate() {
                if i != 0 {
                    s.push(',');
                }
                push_hex(&mut s, *intid as u64);
            }
        }
        s.push_str(";dma=");
        push_hex(&mut s, self.dma_max);
        if let Some((bus, dev, fun)) = self.pci {
            // bdf in the same bare-hex `<bus>.<dev>.<fn>` form devpci uses.
            s.push_str(";pci=");
            push_hex(&mut s, bus as u64);
            s.push('.');
            push_hex(&mut s, dev as u64);
            s.push('.');
            push_hex(&mut s, fun as u64);
        }
        Ok(s)
    }

    /// Decode a grant from the warden's argv element. The inverse of
    /// `to_descriptor`. Strict: an unknown/duplicate key, a bad number, a missing
    /// `base:size` colon, an unknown version, or a count over the caps all reject.
    pub fn parse_descriptor(desc: &str) -> Result<BoundResources, Error> {
        let mut fields = desc.split(';');

        // version prefix: "v<N>"
        let v = fields.next().ok_or(Error::BadVersion)?;
        let vnum = v.strip_prefix('v').ok_or(Error::BadVersion)?;
        let vnum: u32 = vnum.parse().map_err(|_| Error::BadVersion)?;
        if vnum != DESCRIPTOR_VERSION {
            return Err(Error::BadVersion);
        }

        let mut instance: Option<u32> = None;
        let mut compatible: Option<String> = None;
        let mut serves: Option<String> = None;
        let mut mmio: Option<Vec<(u64, u64)>> = None;
        let mut irq: Option<Vec<u32>> = None;
        let mut dma_max: Option<u64> = None;
        let mut pci: Option<(u8, u8, u8)> = None;
        let mut pci_seen = false; // distinguishes "absent" (-> None) from a dup

        for field in fields {
            if field.is_empty() {
                continue; // tolerate a stray trailing ';'
            }
            let (key, val) = field.split_once('=').ok_or(Error::BadField)?;
            match key {
                "inst" => {
                    if instance.is_some() {
                        return Err(Error::BadField);
                    }
                    instance = Some(val.parse().map_err(|_| Error::BadNumber)?);
                }
                "compat" => {
                    if compatible.is_some() {
                        return Err(Error::BadField);
                    }
                    compatible = Some(val.to_string());
                }
                "serve" => {
                    if serves.is_some() {
                        return Err(Error::BadField);
                    }
                    serves = Some(val.to_string());
                }
                "mmio" => {
                    if mmio.is_some() {
                        return Err(Error::BadField);
                    }
                    mmio = Some(parse_windows(val)?);
                }
                "irq" => {
                    if irq.is_some() {
                        return Err(Error::BadField);
                    }
                    irq = Some(parse_irqs(val)?);
                }
                "dma" => {
                    if dma_max.is_some() {
                        return Err(Error::BadField);
                    }
                    dma_max = Some(parse_hex(val)?);
                }
                "pci" => {
                    if pci_seen {
                        return Err(Error::BadField);
                    }
                    pci_seen = true;
                    pci = Some(parse_bdf(val)?);
                }
                _ => return Err(Error::BadField), // unknown key
            }
        }

        Ok(BoundResources {
            instance: instance.ok_or(Error::BadField)?,
            compatible: compatible.ok_or(Error::BadField)?,
            serves: serves.ok_or(Error::BadField)?,
            mmio: mmio.unwrap_or_default(),
            irq: irq.unwrap_or_default(),
            dma_max: dma_max.ok_or(Error::BadField)?,
            pci, // absent => None (the field is optional, like mmio/irq)
        })
    }
}

// =============================================================================
// codec helpers
// =============================================================================

pub(crate) fn push_hex(s: &mut String, v: u64) {
    // bare lowercase hex, no "0x" prefix (the decoder strips an optional prefix).
    if v == 0 {
        s.push('0');
        return;
    }
    let mut buf = [0u8; 16];
    let mut n = v;
    let mut i = buf.len();
    while n != 0 {
        i -= 1;
        let d = (n & 0xf) as u8;
        buf[i] = if d < 10 { b'0' + d } else { b'a' + (d - 10) };
        n >>= 4;
    }
    // SAFETY: buf[i..] is ASCII hex digits.
    s.push_str(core::str::from_utf8(&buf[i..]).unwrap());
}

fn parse_hex(s: &str) -> Result<u64, Error> {
    let s = s
        .strip_prefix("0x")
        .or_else(|| s.strip_prefix("0X"))
        .unwrap_or(s);
    if s.is_empty() {
        return Err(Error::BadNumber);
    }
    u64::from_str_radix(s, 16).map_err(|_| Error::BadNumber)
}

pub(crate) fn parse_windows(val: &str) -> Result<Vec<(u64, u64)>, Error> {
    let mut out = Vec::new();
    for win in val.split(',') {
        if win.is_empty() {
            continue;
        }
        let (base, size) = win.split_once(':').ok_or(Error::BadNumber)?;
        out.push((parse_hex(base)?, parse_hex(size)?));
        if out.len() > MAX_MMIO {
            return Err(Error::TooManyResources);
        }
    }
    Ok(out)
}

/// Parse a `<bus>.<dev>.<fn>` bdf (bare hex, each component a u8) -- the descriptor
/// + devpci form. Exactly three dot-separated components; any malformed shape (too
/// few/many, a non-hex or out-of-u8 component) rejects.
fn parse_bdf(val: &str) -> Result<(u8, u8, u8), Error> {
    let mut it = val.split('.');
    let bus = it.next().ok_or(Error::BadNumber)?;
    let dev = it.next().ok_or(Error::BadNumber)?;
    let fun = it.next().ok_or(Error::BadNumber)?;
    if it.next().is_some() {
        return Err(Error::BadNumber); // more than three components
    }
    Ok((parse_hex_u8(bus)?, parse_hex_u8(dev)?, parse_hex_u8(fun)?))
}

fn parse_hex_u8(s: &str) -> Result<u8, Error> {
    u8::try_from(parse_hex(s)?).map_err(|_| Error::BadNumber)
}

pub(crate) fn parse_irqs(val: &str) -> Result<Vec<u32>, Error> {
    let mut out = Vec::new();
    for id in val.split(',') {
        if id.is_empty() {
            continue;
        }
        let v = parse_hex(id)?;
        let v = u32::try_from(v).map_err(|_| Error::BadNumber)?;
        out.push(v);
        if out.len() > MAX_IRQ {
            return Err(Error::TooManyResources);
        }
    }
    Ok(out)
}

/// Round an MMIO window out to 4 KiB page boundaries. MMIO is mapped page-granular,
/// so an allowance window (the authority the kernel enforces at `SYS_MMIO_CREATE`,
/// where the driver's *page*-sized map request is checked) must be page-granular
/// too -- a sub-page device register (a virtio-mmio slot is 0x200) is only
/// reachable by mapping its whole page. Returns `(page_base, page_len)` covering
/// `[base, base+size)`; a zero-size window yields a zero-length window at the page
/// base. Used by `to_allowance` (the device-side allowance mirror); public so the
/// warden / tests can page-round a window directly.
pub fn page_round(base: u64, size: u64) -> (u64, u64) {
    const PAGE: u64 = 0x1000;
    let lo = base & !(PAGE - 1);
    if size == 0 {
        return (lo, 0);
    }
    let end = base.saturating_add(size);
    let hi = end.saturating_add(PAGE - 1) & !(PAGE - 1);
    (lo, hi.saturating_sub(lo))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::manifest::Manifest;
    use crate::source::{DeviceId, DeviceNode};

    /// A DTB-style node (ids = the compatibles, most-specific first) for the
    /// resolve tests -- the `DtbSource` shape.
    fn dtb_node(compatible: &[&str], reg: Vec<(u64, u64)>, interrupts: Vec<u32>) -> DeviceNode {
        let compat: Vec<String> = compatible.iter().map(|s| s.to_string()).collect();
        let ids = compat.iter().map(|c| DeviceId::Compatible(c.clone())).collect();
        DeviceNode {
            label: "test".to_string(),
            ids,
            resources: NodeResources {
                compatible: compat,
                reg,
                interrupts,
                pci: None,
            },
        }
    }

    /// A virtio-PCI node (the `PciSource` shape): id = `VirtioPci(virtio_id)`,
    /// resources = a bdf + an optional wired INTID, no reg windows.
    fn pci_node(label: &str, virtio_id: u16, bdf: (u8, u8, u8), intid: Option<u32>) -> DeviceNode {
        DeviceNode {
            label: label.to_string(),
            ids: alloc::vec![DeviceId::VirtioPci(virtio_id)],
            resources: NodeResources {
                compatible: Vec::new(),
                reg: Vec::new(),
                interrupts: intid.into_iter().collect(),
                pci: Some(bdf),
            },
        }
    }

    fn example_manifest() -> Manifest {
        Manifest::parse(
            r#"driver "rp1-eth" {
                abi = 1
                binds = ["raspberrypi,rp1-eth", "brcm,genet-v5"]
                needs { mmio = "node:reg" irq = "msi:1" dma = "pool: 2 MiB" }
                serves = "/dev/net/%instance"
                restart = on-crash
            }"#,
        )
        .unwrap()
    }

    fn wired_manifest() -> Manifest {
        Manifest::parse(
            r#"driver "virtio-net" {
                abi = 1
                binds = ["virtio,mmio"]
                needs { mmio = "node:reg" irq = "node:interrupts" dma = "pool: 256 KiB" }
                serves = "/dev/net/%instance"
                restart = on-crash
            }"#,
        )
        .unwrap()
    }

    #[test]
    fn resolve_msi_manifest_grants_node_mmio_no_dtb_irq() {
        let m = example_manifest();
        let node = dtb_node(
            &["raspberrypi,rp1-eth", "brcm,genet-v5"],
            vec![(0x1f00_100000, 0x10000)],
            vec![],
        );
        let b = resolve(&m, &node, 3).unwrap();
        assert_eq!(b.instance, 3);
        assert_eq!(b.compatible, "raspberrypi,rp1-eth"); // most-specific match
        assert_eq!(b.serves, "/dev/net/3"); // %instance expanded
        assert_eq!(b.mmio, [(0x1f00_100000u64, 0x10000u64)]); // node supplied
        assert_eq!(b.irq, Vec::<u32>::new()); // Msi not DTB-resolvable
        assert_eq!(b.dma_max, 2 * 1024 * 1024);
    }

    #[test]
    fn resolve_wired_manifest_grants_node_interrupts() {
        let m = wired_manifest();
        let node = dtb_node(&["virtio,mmio"], vec![(0x0a00_3000, 0x1000)], vec![0x2a]);
        let b = resolve(&m, &node, 0).unwrap();
        assert_eq!(b.mmio, [(0x0a00_3000u64, 0x1000u64)]);
        assert_eq!(b.irq, [0x2a]);
        assert_eq!(b.dma_max, 256 * 1024);
    }

    #[test]
    fn resolve_grant_never_exceeds_node() {
        // the auditable I-34 property: every granted window is a node window.
        let m = wired_manifest();
        let node = dtb_node(
            &["virtio,mmio"],
            vec![(0x1000, 0x100), (0x2000, 0x200)],
            vec![5, 6],
        );
        let b = resolve(&m, &node, 0).unwrap();
        for w in &b.mmio {
            assert!(node.resources.reg.contains(w), "granted window {w:?} not in node");
        }
        for i in &b.irq {
            assert!(
                node.resources.interrupts.contains(i),
                "granted irq {i} not in node"
            );
        }
    }

    #[test]
    fn resolve_no_match() {
        let m = wired_manifest();
        let node = dtb_node(&["acme,widget"], vec![(0x1000, 0x100)], vec![5]);
        assert_eq!(resolve(&m, &node, 0), Err(Error::NoMatch));
    }

    #[test]
    fn resolve_too_many_windows() {
        let m = wired_manifest();
        let reg: Vec<(u64, u64)> = (0..(MAX_MMIO as u64 + 1))
            .map(|i| (i * 0x1000, 0x1000))
            .collect();
        let node = dtb_node(&["virtio,mmio"], reg, vec![]);
        assert_eq!(resolve(&m, &node, 0), Err(Error::TooManyWindows));
    }

    #[test]
    fn descriptor_round_trip() {
        let b = BoundResources {
            instance: 2,
            compatible: "raspberrypi,rp1-eth".to_string(),
            serves: "/dev/net/2".to_string(),
            mmio: vec![(0x0a00_3000, 0x1000), (0x0a00_4000, 0x200)],
            irq: vec![0x2a, 0x2b],
            dma_max: 0x20_0000,
            pci: None,
        };
        let desc = b.to_descriptor().unwrap();
        let b2 = BoundResources::parse_descriptor(&desc).unwrap();
        assert_eq!(b, b2);
    }

    #[test]
    fn descriptor_round_trip_empty_axes() {
        let b = BoundResources {
            instance: 0,
            compatible: "virtio,mmio".to_string(),
            serves: "/dev/x/0".to_string(),
            mmio: vec![],
            irq: vec![],
            dma_max: 0,
            pci: None,
        };
        let desc = b.to_descriptor().unwrap();
        // empty mmio/irq/pci omitted; dma=0 present
        assert!(!desc.contains("mmio="));
        assert!(!desc.contains("irq="));
        assert!(!desc.contains("pci="));
        assert!(desc.contains("dma=0"));
        assert_eq!(BoundResources::parse_descriptor(&desc).unwrap(), b);
    }

    #[test]
    fn descriptor_shape() {
        let b = BoundResources {
            instance: 1,
            compatible: "virtio,mmio".to_string(),
            serves: "/dev/net/1".to_string(),
            mmio: vec![(0xa003000, 0x1000)],
            irq: vec![0x2a],
            dma_max: 0x100000,
            pci: None,
        };
        assert_eq!(
            b.to_descriptor().unwrap(),
            "v1;inst=1;compat=virtio,mmio;serve=/dev/net/1;mmio=a003000:1000;irq=2a;dma=100000"
        );
    }

    #[test]
    fn descriptor_rejects_bad_input() {
        assert_eq!(
            BoundResources::parse_descriptor("v2;inst=0;compat=x;serve=/x;dma=0"),
            Err(Error::BadVersion)
        );
        assert_eq!(
            BoundResources::parse_descriptor("inst=0;compat=x;serve=/x;dma=0"),
            Err(Error::BadVersion) // no v-prefix
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;compat=x;serve=/x;dma=0"),
            Err(Error::BadField) // missing inst
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;serve=/x;dma=0"),
            Err(Error::BadField) // missing compat
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;dma=0"),
            Err(Error::BadField) // missing serve
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x"),
            Err(Error::BadField) // missing dma
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;inst=1;compat=x;serve=/x;dma=0"),
            Err(Error::BadField) // duplicate
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x;dma=0;bogus=1"),
            Err(Error::BadField) // unknown key
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x;mmio=a003000;dma=0"),
            Err(Error::BadNumber) // window missing :size
        );
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=z;compat=x;serve=/x;dma=0"),
            Err(Error::BadNumber) // bad instance
        );
    }

    #[test]
    fn descriptor_rejects_overlong_axes() {
        let mut mmio = String::from("v1;inst=0;compat=x;serve=/x;mmio=");
        for i in 0..(MAX_MMIO + 1) {
            if i != 0 {
                mmio.push(',');
            }
            mmio.push_str("1000:1000");
        }
        mmio.push_str(";dma=0");
        assert_eq!(
            BoundResources::parse_descriptor(&mmio),
            Err(Error::TooManyResources)
        );
    }

    #[test]
    fn encode_rejects_delimiter_in_value() {
        let b = BoundResources {
            instance: 0,
            compatible: "ev;il".to_string(), // a ';' would corrupt the descriptor
            serves: "/x".to_string(),
            mmio: vec![],
            irq: vec![],
            dma_max: 0,
            pci: None,
        };
        assert_eq!(b.to_descriptor(), Err(Error::BadField));
    }

    #[test]
    fn end_to_end_resolve_then_codec() {
        // the live path: warden resolves -> encodes -> driver decodes -> same grant.
        let m = wired_manifest();
        let node = dtb_node(&["virtio,mmio"], vec![(0x0a00_3000, 0x1000)], vec![0x2a]);
        let warden_grant = resolve(&m, &node, 7).unwrap();
        let desc = warden_grant.to_descriptor().unwrap();
        let driver_grant = BoundResources::parse_descriptor(&desc).unwrap();
        assert_eq!(warden_grant, driver_grant);
    }

    // -------------------------------------------------------------------------
    // PCI axis (6b-2): the resolve grant + the descriptor codec
    // -------------------------------------------------------------------------

    /// A virtio-PCI driver manifest: claims its function (`pci = "node"`), takes
    /// its INTID + a DMA pool, and -- crucially -- NO mmio (the BARs are
    /// handle-gated via SYS_PCI_MAP_BAR, not allowance windows; pci-3).
    fn pci_manifest() -> Manifest {
        Manifest::parse(
            r#"driver "netdev-pci" {
                abi = 1
                binds = ["virtio-pci:1"]
                needs { irq = "node:interrupts" dma = "pool: 64 KiB" pci = "node" }
                serves = "/dev/net/%instance"
                restart = on-crash
            }"#,
        )
        .unwrap()
    }

    #[test]
    fn resolve_pci_node_grants_bdf_and_intid_no_mmio() {
        let m = pci_manifest();
        let node = pci_node("0.1.0", 1, (0, 1, 0), Some(0x23));
        let b = resolve(&m, &node, 4).unwrap();
        assert_eq!(b.compatible, "virtio-pci:1");
        assert_eq!(b.serves, "/dev/net/4");
        assert_eq!(b.pci, Some((0, 1, 0))); // node bdf conferred
        assert_eq!(b.irq, [0x23]); // node INTID conferred
        assert!(b.mmio.is_empty()); // BARs are NOT allowance MMIO
        assert_eq!(b.dma_max, 64 * 1024);
    }

    #[test]
    fn resolve_pci_grant_equals_node_bdf() {
        // the auditable I-34 property on the PCI axis: the granted bdf is exactly
        // the node's, never fabricated.
        let m = pci_manifest();
        let node = pci_node("3.5.1", 1, (3, 5, 1), None);
        let b = resolve(&m, &node, 0).unwrap();
        assert_eq!(b.pci, node.resources.pci);
        assert!(b.irq.is_empty()); // no INTID on the node -> none granted
    }

    #[test]
    fn resolve_pci_none_when_node_lacks_bdf() {
        // defensive fail-closed: a (degenerate) virtio-PCI node carrying no bdf
        // yields no PCI grant rather than a fabricated one. devpci never emits such
        // a node, but resolve must not invent authority.
        let m = pci_manifest();
        let node = DeviceNode {
            label: "degenerate".to_string(),
            ids: alloc::vec![DeviceId::VirtioPci(1)],
            resources: NodeResources {
                compatible: Vec::new(),
                reg: Vec::new(),
                interrupts: Vec::new(),
                pci: None,
            },
        };
        let b = resolve(&m, &node, 0).unwrap();
        assert_eq!(b.pci, None);
    }

    #[test]
    fn resolve_no_pci_need_ignores_node_bdf() {
        // a manifest that does not ask for PCI gets no bdf even from a PCI node.
        // (wired_manifest binds "virtio,mmio", so give it a matching id but a bdf;
        // it declines the PCI axis -> no grant.)
        let m = wired_manifest();
        let node = DeviceNode {
            label: "x".to_string(),
            ids: alloc::vec![DeviceId::Compatible("virtio,mmio".to_string())],
            resources: NodeResources {
                compatible: alloc::vec!["virtio,mmio".to_string()],
                reg: alloc::vec![(0x0a00_3000, 0x1000)],
                interrupts: alloc::vec![0x2a],
                pci: Some((0, 1, 0)),
            },
        };
        let b = resolve(&m, &node, 0).unwrap();
        assert_eq!(b.pci, None); // manifest declined pci -> not conferred
    }

    #[test]
    fn descriptor_round_trip_with_pci() {
        let b = BoundResources {
            instance: 4,
            compatible: "virtio-pci:1".to_string(),
            serves: "/dev/net/4".to_string(),
            mmio: vec![],
            irq: vec![0x23],
            dma_max: 0x1_0000,
            pci: Some((0, 1, 0)),
        };
        let desc = b.to_descriptor().unwrap();
        assert_eq!(BoundResources::parse_descriptor(&desc).unwrap(), b);
    }

    #[test]
    fn descriptor_shape_pci() {
        let b = BoundResources {
            instance: 0,
            compatible: "virtio-pci:1".to_string(),
            serves: "/dev/net/0".to_string(),
            mmio: vec![],
            irq: vec![0x23],
            dma_max: 0x1_0000,
            pci: Some((0, 0xa, 1)),
        };
        assert_eq!(
            b.to_descriptor().unwrap(),
            "v1;inst=0;compat=virtio-pci:1;serve=/dev/net/0;irq=23;dma=10000;pci=0.a.1"
        );
    }

    #[test]
    fn descriptor_rejects_bad_pci() {
        // too few components
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x;dma=0;pci=0.1"),
            Err(Error::BadNumber)
        );
        // too many components
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x;dma=0;pci=0.1.0.0"),
            Err(Error::BadNumber)
        );
        // non-hex component
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x;dma=0;pci=g.0.0"),
            Err(Error::BadNumber)
        );
        // a component out of u8 range
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x;dma=0;pci=100.0.0"),
            Err(Error::BadNumber)
        );
        // duplicate pci field
        assert_eq!(
            BoundResources::parse_descriptor("v1;inst=0;compat=x;serve=/x;dma=0;pci=0.1.0;pci=0.1.0"),
            Err(Error::BadField)
        );
    }

    #[test]
    fn end_to_end_resolve_then_codec_pci() {
        // the live PCI path: warden resolves a devpci node -> encodes -> driver
        // decodes -> identical grant (the bdf the driver SYS_PCI_CLAIMs).
        let m = pci_manifest();
        let node = pci_node("0.1.0", 1, (0, 1, 0), Some(0x23));
        let warden_grant = resolve(&m, &node, 7).unwrap();
        let desc = warden_grant.to_descriptor().unwrap();
        let driver_grant = BoundResources::parse_descriptor(&desc).unwrap();
        assert_eq!(warden_grant, driver_grant);
        assert_eq!(driver_grant.pci, Some((0, 1, 0)));
    }

    #[test]
    fn page_round_covers_sub_page() {
        // a virtio-mmio slot (0x200, non-page-aligned) -> its whole 4 KiB page
        assert_eq!(page_round(0x0a00_3a00, 0x200), (0x0a00_3000, 0x1000));
        // a window straddling a page boundary -> both pages
        assert_eq!(page_round(0x0a00_3f00, 0x200), (0x0a00_3000, 0x2000));
        // an already page-aligned/sized window is unchanged (the pl061 case)
        assert_eq!(page_round(0x0903_0000, 0x1000), (0x0903_0000, 0x1000));
        // zero size -> zero length at the page base
        assert_eq!(page_round(0x0a00_3a00, 0), (0x0a00_3000, 0));
    }
}
