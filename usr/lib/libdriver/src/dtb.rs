// The DTB discovery-source decode -- how the warden turns the raw devhw property
// bytes into a `NodeResources` (MENAGERIE.md section 5; the DTB is the v1.0
// discovery source). The warden reads `/hw/<node>/{compatible,reg,interrupts}`
// (the devhw tree publishes each FDT property verbatim, big-endian on-wire) and
// these decoders produce the typed resources `resolve` intersects.
//
// PURE (no libthyla-rs) so the endianness + cell-width + SPI-mapping logic is
// host-tested against the real QEMU-virt bytes -- the fiddly part a runtime test
// can only exercise on whatever hardware it happens to boot on.
//
// Cell widths: the FDT addresses every `reg` window in `#address-cells` /
// `#size-cells` (the parent's, 2/2 on every ARM64 platform Menagerie targets at
// v1.0) and every interrupt in the GIC's `#interrupt-cells` (3). v1.0 passes the
// constants below; reading them from the DTB root + the interrupt-parent is a
// v1.x refinement (the auditable grant -- never wider than the node -- holds
// regardless of how the cell counts are obtained).

use alloc::string::{String, ToString};
use alloc::vec::Vec;

use crate::resource::NodeResources;

/// `#address-cells` on the FDT root, every ARM64 platform Menagerie targets.
pub const ARM64_ADDR_CELLS: u32 = 2;
/// `#size-cells` on the FDT root.
pub const ARM64_SIZE_CELLS: u32 = 2;
/// `#interrupt-cells` on the GIC (the `<type number flags>` form).
pub const GIC_INTERRUPT_CELLS: u32 = 3;

/// First interrupt cell == 0: a Shared Peripheral Interrupt. INTID = 32 + number.
const GIC_TYPE_SPI: u32 = 0;
/// First interrupt cell == 1: a Private Peripheral Interrupt. INTID = 16 + number.
const GIC_TYPE_PPI: u32 = 1;
/// GIC INTID base for an SPI (matches the kernel `lib/dtb.c` convention + I-5).
const GIC_SPI_BASE: u32 = 32;
/// GIC INTID base for a PPI.
const GIC_PPI_BASE: u32 = 16;

fn be_u32(b: &[u8]) -> u32 {
    u32::from_be_bytes([b[0], b[1], b[2], b[3]])
}

/// Decode a `compatible` property -- the FDT stores it as a sequence of
/// NUL-terminated strings, most-specific first. Empty segments (the trailing NUL,
/// or a malformed double-NUL) and non-UTF-8 segments are skipped.
pub fn parse_compatible(bytes: &[u8]) -> Vec<String> {
    let mut out = Vec::new();
    for seg in bytes.split(|&b| b == 0) {
        if seg.is_empty() {
            continue;
        }
        if let Ok(s) = core::str::from_utf8(seg) {
            out.push(s.to_string());
        }
    }
    out
}

/// Decode a `reg` property into `(base, size)` byte windows. Each entry is
/// `addr_cells` address cells followed by `size_cells` size cells, every cell a
/// big-endian u32; cells fold big-endian into the low 64 bits (so a >2-cell
/// address keeps its low 64 bits rather than panicking). A trailing partial entry
/// (a malformed property) is ignored -- decode is best-effort, never a panic.
pub fn parse_reg(bytes: &[u8], addr_cells: u32, size_cells: u32) -> Vec<(u64, u64)> {
    let ac = addr_cells as usize;
    let sc = size_cells as usize;
    let cells = ac + sc;
    if cells == 0 {
        return Vec::new();
    }
    let stride = cells * 4;
    let mut out = Vec::new();
    let mut off = 0usize;
    while off + stride <= bytes.len() {
        let mut base = 0u64;
        for i in 0..ac {
            base = (base << 32) | be_u32(&bytes[off + i * 4..]) as u64;
        }
        let mut size = 0u64;
        for i in 0..sc {
            size = (size << 32) | be_u32(&bytes[off + (ac + i) * 4..]) as u64;
        }
        out.push((base, size));
        off += stride;
    }
    out
}

/// Decode an `interrupts` property into absolute GIC INTIDs. Each interrupt is
/// `interrupt_cells` big-endian u32 cells; for the GIC 3-cell form the first cell
/// is the type (0 = SPI, 1 = PPI) and the second is the number, giving INTID =
/// number + 32 (SPI) / number + 16 (PPI). An unknown type is skipped (e.g. the
/// PMU/timer rows with extra affinity flags -- those nodes have no `reg` and so
/// never match a bindable manifest anyway). The base add saturates so a malformed
/// number cannot panic; a resulting out-of-range INTID is rejected downstream by
/// the kernel `SYS_IRQ_CREATE` gate, never here.
pub fn parse_interrupts(bytes: &[u8], interrupt_cells: u32) -> Vec<u32> {
    let ic = interrupt_cells as usize;
    if ic < 2 {
        return Vec::new(); // need at least <type number>
    }
    let stride = ic * 4;
    let mut out = Vec::new();
    let mut off = 0usize;
    while off + stride <= bytes.len() {
        let itype = be_u32(&bytes[off..]);
        let number = be_u32(&bytes[off + 4..]);
        match itype {
            GIC_TYPE_SPI => out.push(number.saturating_add(GIC_SPI_BASE)),
            GIC_TYPE_PPI => out.push(number.saturating_add(GIC_PPI_BASE)),
            _ => {}
        }
        off += stride;
    }
    out
}

impl NodeResources {
    /// Build a `NodeResources` from a node's three raw devhw property buffers.
    /// Any property may be empty (a node without it): an absent `compatible`
    /// yields no match, an absent `reg`/`interrupts` yields an empty axis.
    pub fn from_dtb(
        compatible: &[u8],
        reg: &[u8],
        interrupts: &[u8],
        addr_cells: u32,
        size_cells: u32,
        interrupt_cells: u32,
    ) -> NodeResources {
        NodeResources {
            compatible: parse_compatible(compatible),
            reg: parse_reg(reg, addr_cells, size_cells),
            interrupts: parse_interrupts(interrupts, interrupt_cells),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The real QEMU-virt pl061 GPIO bytes (dtc -I dtb): the warden's proof node.
    //   compatible = "arm,pl061", "arm,primecell"
    //   reg        = <0x00 0x9030000 0x00 0x1000>
    //   interrupts = <0x00 0x07 0x04>   (SPI 7 -> INTID 39)
    const PL061_COMPAT: &[u8] = b"arm,pl061\0arm,primecell\0";
    const PL061_REG: &[u8] = &[
        0, 0, 0, 0, 0x09, 0x03, 0, 0, // base 0x09030000
        0, 0, 0, 0, 0, 0, 0x10, 0, // size 0x00001000
    ];
    const PL061_INTR: &[u8] = &[
        0, 0, 0, 0, // type 0 (SPI)
        0, 0, 0, 0x07, // number 7
        0, 0, 0, 0x04, // flags
    ];

    #[test]
    fn compatible_nul_separated_most_specific_first() {
        assert_eq!(
            parse_compatible(PL061_COMPAT),
            ["arm,pl061", "arm,primecell"]
        );
    }

    #[test]
    fn compatible_tolerates_empty_and_unterminated() {
        assert_eq!(parse_compatible(b""), Vec::<String>::new());
        assert_eq!(parse_compatible(b"virtio,mmio"), ["virtio,mmio"]); // no trailing NUL
        assert_eq!(parse_compatible(b"a\0\0b\0"), ["a", "b"]); // double NUL skipped
    }

    #[test]
    fn reg_decodes_addr2_size2() {
        assert_eq!(
            parse_reg(PL061_REG, ARM64_ADDR_CELLS, ARM64_SIZE_CELLS),
            [(0x0903_0000u64, 0x1000u64)]
        );
        // a virtio-mmio slot: <0 0xa000000 0 0x200>
        let virtio_reg: &[u8] = &[0, 0, 0, 0, 0x0a, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0];
        assert_eq!(parse_reg(virtio_reg, 2, 2), [(0x0a00_0000u64, 0x200u64)]);
    }

    #[test]
    fn reg_addr1_size1_and_multi_entry() {
        // <0x1000 0x10  0x2000 0x20> in 1/1 cells
        let bytes: &[u8] = &[
            0, 0, 0x10, 0, 0, 0, 0, 0x10, // (0x1000, 0x10)
            0, 0, 0x20, 0, 0, 0, 0, 0x20, // (0x2000, 0x20)
        ];
        assert_eq!(parse_reg(bytes, 1, 1), [(0x1000, 0x10), (0x2000, 0x20)]);
    }

    #[test]
    fn reg_ignores_trailing_partial() {
        // one full 16-byte entry + 5 stray bytes
        let mut bytes = PL061_REG.to_vec();
        bytes.extend_from_slice(&[1, 2, 3, 4, 5]);
        assert_eq!(parse_reg(&bytes, 2, 2), [(0x0903_0000u64, 0x1000u64)]);
    }

    #[test]
    fn interrupts_spi_maps_plus_32() {
        assert_eq!(parse_interrupts(PL061_INTR, GIC_INTERRUPT_CELLS), [39]); // 7 + 32
                                                                             // virtio-mmio slot 0: <0 0x10 1> -> SPI 16 -> INTID 48
        let virtio_intr: &[u8] = &[0, 0, 0, 0, 0, 0, 0, 0x10, 0, 0, 0, 1];
        assert_eq!(parse_interrupts(virtio_intr, 3), [48]);
    }

    #[test]
    fn interrupts_ppi_maps_plus_16_and_unknown_skipped() {
        // <1 13 flags> (PPI 13 -> INTID 29), then <2 0 0> (unknown type, skipped)
        let bytes: &[u8] = &[
            0, 0, 0, 1, 0, 0, 0, 0x0d, 0, 0, 0, 0, // PPI 13
            0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, // type 2: skip
        ];
        assert_eq!(parse_interrupts(bytes, 3), [29]);
    }

    #[test]
    fn from_dtb_builds_the_full_node() {
        let n = NodeResources::from_dtb(
            PL061_COMPAT,
            PL061_REG,
            PL061_INTR,
            ARM64_ADDR_CELLS,
            ARM64_SIZE_CELLS,
            GIC_INTERRUPT_CELLS,
        );
        assert_eq!(n.compatible, ["arm,pl061", "arm,primecell"]);
        assert_eq!(n.reg, [(0x0903_0000u64, 0x1000u64)]);
        assert_eq!(n.interrupts, [39]);
    }
}
