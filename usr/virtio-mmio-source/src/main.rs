// /virtio-mmio-source -- the Menagerie virtio-mmio bus enumerator (MENAGERIE.md
// section 7), build-arc step 5d-2. The first concrete bus discovery source: a
// separate, capability-sandboxed Proc the warden spawns granted ONLY the
// virtio-mmio bank, so the warden itself never touches a device register.
//
// QEMU-virt exposes ~32 identical `virtio,mmio` transport slots; the DTB cannot
// say which device sits in which slot (all `compatible = "virtio,mmio"`) -- only
// each slot's runtime DeviceID register can. This source reads each slot's
// DeviceID, suppresses the raw transport nodes, and re-emits one TYPED
// `virtio:<device-id>` DeviceNode per POPULATED slot, carrying that slot's exact
// reg + INTID (from the DTB). The warden then binds by id (e.g. virtio:1 ->
// netdev), granting exactly that slot. The DeviceID-poke is isolated HERE, in a
// single-purpose enumerator -- the §3 "the warden never reads a device register"
// property realized.
//
// Channel: node records go to stdout (fd 1 = the warden's pipe), newline-
// delimited (`DeviceNode::to_record`). Human diagnostics go to the console
// (`t_putstr`), since a boot-probe child's stderr is /dev/null.
//
// Grant: the warden confers an allowance narrowed to the bank MMIO window +
// CAP_HW_CREATE to map it. The source maps the bank, reads MAGIC + DeviceID per
// slot, reports the populated ones, RELEASES the bank (so the warden can grant a
// slot to a driver), and exits. It claims NO IRQ -- it reads, it does not service.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

use libthyla_rs::handle::Rights;
use libthyla_rs::hardware::{mmio_read32, Mmio};
use libthyla_rs::io::Write;
use libthyla_rs::{T_PROT_READ, T_PROT_WRITE};

use libdriver::resource::NodeResources;
use libdriver::{bind, DeviceId, DeviceNode, DiscoverySource, DtbSource};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

/// Console-direct diagnostics (T_SYS_PUTS) -- visible regardless of fd wiring; a
/// boot-probe child loses fd-2 (eprintln) output. NOTE: distinct from the node
/// records, which go to fd 1 (the warden's pipe) via `io::stdout`.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

// virtio-mmio register offsets (VIRTIO 1.2 section 4.2.2).
const REG_MAGIC_VALUE: u64 = 0x000;
const REG_DEVICE_ID: u64 = 0x008;
const VIRTIO_MMIO_MAGIC: u32 = 0x7472_6976; // "virt"

/// The raw transport compatible the DtbSource emits for each slot.
const VIRTIO_MMIO_COMPATIBLE: &str = "virtio,mmio";

/// Where the source maps the granted bank in its own address space (a separate
/// Proc, so any valid user VA clear of its own image works -- matches netdev's
/// MMIO VA convention).
const BANK_VA: u64 = 0x0050_0000;

/// Exit codes (mirrors the libdriver lifecycle: bind failure vs probe failure).
const EXIT_BIND: i64 = 71;
const EXIT_PROBE: i64 = 72;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // The conferred grant: mmio[0] = the virtio-mmio bank window (the authority is
    // the kernel allowance; this descriptor is the matching information).
    let res = match bind() {
        Ok(r) => r,
        Err(e) => {
            say!("virtio-mmio-source: bind failed {:?}", e);
            return EXIT_BIND;
        }
    };
    let (bank_pa, bank_size) = match res.mmio.first().copied() {
        Some(w) => w,
        None => {
            say!("virtio-mmio-source: no bank window in grant");
            return EXIT_BIND;
        }
    };

    // The per-slot layout (reg + INTID) from /hw: the source reads its own DTB view
    // to correlate a populated slot's DeviceID with that slot's INTID, which it
    // emits in the typed node. (The warden granted the bank; the source enumerates
    // it.)
    let dtb = DtbSource::new().enumerate();
    let slots: Vec<&DeviceNode> = dtb
        .iter()
        .filter(|n| {
            n.ids
                .iter()
                .any(|id| matches!(id, DeviceId::Compatible(c) if c == VIRTIO_MMIO_COMPATIBLE))
        })
        .collect();

    // Map the granted bank (in-grant -> the I-34 gate admits it). RW map: the
    // accessors only read, but device MMIO is mapped RW (no EXEC, I-12).
    let rw_map = Rights::READ | Rights::WRITE | Rights::MAP;
    let bank = match unsafe {
        Mmio::new(
            bank_pa,
            bank_size as usize,
            rw_map,
            BANK_VA,
            T_PROT_READ | T_PROT_WRITE,
        )
    } {
        Ok(m) => m,
        Err(_) => {
            say!(
                "virtio-mmio-source: map bank {:#x}/{:#x} failed (allowance?)",
                bank_pa,
                bank_size
            );
            return EXIT_PROBE;
        }
    };

    let mut out = libthyla_rs::io::stdout();
    let mut reported = 0u32;
    for slot in &slots {
        let Some((slot_pa, _)) = slot.resources.reg.first().copied() else {
            continue;
        };
        // The slot's whole read extent (through REG_DEVICE_ID + 4) must lie within
        // the granted bank (defensive: a /hw node outside the granted window is
        // skipped, never read). Gating on the base alone would let a slot in the
        // last < 0xc bytes of the mapped region read past it -- not reachable with
        // the page-rounded bank + 0x200-aligned slots, but the gate bounds the
        // access, not just the base.
        let read_end = slot_pa.saturating_add(REG_DEVICE_ID + 4);
        if slot_pa < bank_pa || read_end > bank_pa.saturating_add(bank_size) {
            continue;
        }
        let slot_va = BANK_VA + (slot_pa - bank_pa);

        if unsafe { mmio_read32(slot_va + REG_MAGIC_VALUE) } != VIRTIO_MMIO_MAGIC {
            continue; // not a virtio-mmio transport (shouldn't happen on a real slot)
        }
        let device_id = unsafe { mmio_read32(slot_va + REG_DEVICE_ID) };
        if device_id == 0 {
            continue; // empty slot
        }
        // virtio device ids are small; a value that does not fit u16 is not a valid
        // virtio device (a garbage read) -- skip rather than truncate.
        let Ok(device_id) = u16::try_from(device_id) else {
            continue;
        };

        // Re-emit the TYPED node: id = virtio:<device-id>, resources = this slot's
        // reg + INTID (the raw "virtio,mmio" compatible is dropped -- a typed node
        // carries no transport compatible).
        let node = DeviceNode {
            label: slot.label.clone(),
            ids: alloc::vec![DeviceId::Virtio(device_id)],
            resources: NodeResources {
                compatible: Vec::new(),
                reg: slot.resources.reg.clone(),
                interrupts: slot.resources.interrupts.clone(),
                pci: None, // a virtio-mmio slot has no PCI bdf
            },
        };
        match node.to_record() {
            Ok(rec) => {
                // record -> the warden's pipe (fd 1), newline-delimited.
                let _ = out.write_all(rec.as_bytes());
                let _ = out.write_all(b"\n");
                reported += 1;
                say!(
                    "virtio-mmio-source: {} -> virtio:{} (reg={:#x} intid={:?})",
                    slot.label,
                    device_id,
                    slot_pa,
                    slot.resources.interrupts
                );
            }
            Err(e) => say!("virtio-mmio-source: encode {} failed {:?}", slot.label, e),
        }
    }

    // Release the bank BEFORE exit so the warden can grant an individual slot to a
    // driver (the exclusive MMIO claim must be free when netdev claims its slot).
    drop(bank);
    say!(
        "virtio-mmio-source: reported {} populated slot(s) of {} virtio-mmio slots",
        reported,
        slots.len()
    );
    0
}
