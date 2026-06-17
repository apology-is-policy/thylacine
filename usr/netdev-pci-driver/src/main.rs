// /netdev-pci-driver -- the Menagerie-bound virtio-net-PCI driver (MENAGERIE
// build-arc step 6b-3), the PCI sibling of /netdev-driver and the LIVE
// I-34-on-PCI proof.
//
// The kernel mediates PCIe topology at /hw/pci (devpci, 6b-1); the warden's
// in-process PciSource (6b-2) reads that view and identifies the NIC as
// `virtio-pci:1`. The warden binds THIS binary NARROWED -- conferring an
// allowance whose PCI axis is exactly that function's (bus,dev,fn), plus its
// wired INTID and a DMA pool, and NOTHING ELSE (I-34). There is no MMIO axis:
// a PCI function's registers live in its BARs, mapped through SYS_PCI_MAP_BAR
// off the claimed KObj_PCI handle, not through the MMIO allowance window.
//
// `probe` claims the net function (SYS_PCI_CLAIM by virtio device-id; the kernel
// resolves the id to a (bus,dev,fn) and gates it against the conferred PCI axis
// -- so a driver narrowed to a different function's bdf would be DENIED, the 6a
// `allowance.pci_claim_handler_gate` negative), maps its BARs, and runs the
// modern-PCI init from the grant. `serve` proves the live device with the net-1
// 24-ARP round-trip, then holds it long-lived until DeviceRemoved (like 5e-1).
//
// Diagnostics go to the console (`t_putstr`); a warden-spawned driver's stderr is
// /dev/null, and `libdriver::run` reports the lifecycle exit code the warden logs.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libdriver::driver::{run, Driver};
use libdriver::resource::BoundResources;
use libdriver::{DeviceId, Error};
use libthyla_rs::io::Write;
use netdev::{VirtioNetPci, MAX_FRAME};

/// Console-direct diagnostics (T_SYS_PUTS) -- visible regardless of fd wiring.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

/// The virtio device-id this driver drives: 1 = net (VIRTIO 1.2 section 5.1).
/// The warden binds us to `virtio-pci:1`; `probe` re-checks the conferred
/// identity is exactly that before claiming, so a manifest mis-bind (binding
/// this net driver to a non-net function) fails closed with a clear diagnostic
/// rather than an opaque kernel claim rejection.
const VIRTIO_ID_NET: u16 = 1;

const SLIRP_GUEST_IP: [u8; 4] = [10, 0, 2, 15];
const SLIRP_GATEWAY_IP: [u8; 4] = [10, 0, 2, 2];

const N_TARGET: u32 = 24;
const MAX_IRQ_ITERS: u32 = 256;
const MAX_DRAIN_PER_IRQ: u32 = 64;

const ETH_HDR: usize = 14;
const ARP_LEN: usize = 28;
const FRAME_LEN: usize = ETH_HDR + ARP_LEN; // 42

/// "who-has 10.0.2.2 tell 10.0.2.15", broadcast.
fn build_arp_request(src_mac: [u8; 6]) -> [u8; FRAME_LEN] {
    let mut f = [0u8; FRAME_LEN];
    for b in f.iter_mut().take(6) {
        *b = 0xff; // dst broadcast
    }
    f[6..12].copy_from_slice(&src_mac);
    f[12] = 0x08;
    f[13] = 0x06; // ARP ethertype
    let a = ETH_HDR;
    f[a + 1] = 0x01; // hw type Ethernet
    f[a + 2] = 0x08; // proto IPv4
    f[a + 4] = 6; // hlen
    f[a + 5] = 4; // plen
    f[a + 7] = 0x01; // op = request
    f[a + 8..a + 14].copy_from_slice(&src_mac); // sender mac
    f[a + 14..a + 18].copy_from_slice(&SLIRP_GUEST_IP); // sender ip
    // target mac = zeros (a+18..a+24)
    f[a + 24..a + 28].copy_from_slice(&SLIRP_GATEWAY_IP); // target ip
    f
}

fn is_arp_reply(frame: &[u8]) -> bool {
    if frame.len() < FRAME_LEN {
        return false;
    }
    if frame[12] != 0x08 || frame[13] != 0x06 {
        return false; // ethertype != ARP
    }
    let a = ETH_HDR;
    if frame[a + 6] != 0x00 || frame[a + 7] != 0x02 {
        return false; // op != reply
    }
    for i in 0..4 {
        if frame[a + 14 + i] != SLIRP_GATEWAY_IP[i] {
            return false; // sender ip != gateway
        }
        if frame[a + 24 + i] != SLIRP_GUEST_IP[i] {
            return false; // target ip != us
        }
    }
    true
}

struct NetPciDriver {
    nic: VirtioNetPci,
}

impl Driver for NetPciDriver {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        // Verify the warden bound us to the device class we drive: the grant's
        // matched identity must be virtio-pci:1 (net). A grant carrying any other
        // identity is a manifest mis-bind -- fail closed before touching hardware.
        match DeviceId::parse(&res.compatible) {
            DeviceId::VirtioPci(VIRTIO_ID_NET) => {}
            other => {
                say!(
                    "netdev-pci-driver: bound to {} (expected virtio-pci:{}) -- refusing",
                    other.as_string(),
                    VIRTIO_ID_NET
                );
                return Err(Error::NoMatch);
            }
        }
        // The narrowing is the conferred allowance: PCI axis = this function's
        // (bus,dev,fn), IRQ axis = its INTID, DMA cap = the pool. open() claims
        // the net function (the kernel resolves the device-id -> bdf and gates it
        // against the PCI axis), maps its BARs, claims the INTID + DMA, and arms
        // the device -- every step admitted only because the grant permits it.
        say!(
            "netdev-pci-driver: grant compat={} pci={:?} irq={} dma={:#x}",
            res.compatible,
            res.pci,
            res.irq.len(),
            res.dma_max
        );
        let nic = VirtioNetPci::open().map_err(|e| {
            say!("netdev-pci-driver: open failed {:?}", e);
            Error::Hardware
        })?;
        Ok(NetPciDriver { nic })
    }

    fn serve(mut self, _res: &BoundResources) -> Result<(), Error> {
        // The net-1 ARP round-trip proof: N_TARGET (24) > QUEUE_SIZE (16), so the
        // RX descriptors recycle past one full ring and the TX descriptors wrap --
        // identical to the MMIO netdev-driver, over the BAR-mapped PCI transport.
        let mac = self.nic.mac();
        say!(
            "netdev-pci-driver: up mac={:02x?} link={} -- ARP round-trip x{}",
            mac,
            self.nic.link_up(),
            N_TARGET
        );
        let req = build_arp_request(mac);
        let mut rxbuf = [0u8; MAX_FRAME];
        let mut sent: u32 = 0;
        let mut validated: u32 = 0;

        // Prime: fill the TX ring (back-pressure stops at QUEUE_SIZE).
        while sent < N_TARGET && self.nic.send(&req) {
            sent += 1;
        }
        for _ in 0..MAX_IRQ_ITERS {
            if validated >= N_TARGET {
                break;
            }
            if !self.nic.wait_irq() {
                continue; // config-change-only wake; no ring progress
            }
            self.nic.drain_tx();
            let mut drained = 0u32;
            while drained < MAX_DRAIN_PER_IRQ {
                match self.nic.poll_rx(&mut rxbuf) {
                    Some(n) => {
                        drained += 1;
                        if is_arp_reply(&rxbuf[..n]) {
                            validated += 1;
                        }
                    }
                    None => break,
                }
            }
            // Reload the TX side now that drain_tx freed in-flight slots.
            while sent < N_TARGET && self.nic.send(&req) {
                sent += 1;
            }
        }

        if validated < N_TARGET {
            say!(
                "netdev-pci-driver: FAIL -- {}/{} ARP replies (sent={})",
                validated,
                N_TARGET,
                sent
            );
            return Err(Error::Hardware);
        }
        say!(
            "netdev-pci-driver: PASS -- {}/{} ARP replies via VirtioNetPci (grant-narrowed PCI claim)",
            validated,
            N_TARGET
        );

        // A driver is a long-lived service. Quiesce FIRST (the warden's teardown
        // is a forced group-terminate that skips Drop, so reset the device here --
        // while this driver still runs -- leaving no live queue to DMA into the
        // pages the reap then frees; the MENAGERIE section-10 surprise-removal
        // hazard, fully fenced by an IOMMU / cooperative quiesce at net-2 / real
        // hardware). Then announce on the console and signal readiness LAST -- the
        // READY line wakes the warden, so all console output precedes it.
        self.nic.quiesce();
        say!("netdev-pci-driver: serving (long-lived; awaiting DeviceRemoved)");
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");

        // Block until the warden's DeviceRemoved (a /proc/<pid>/ctl killgrp).
        // wait_irq is a death-interruptible sleep (#811): on the group-terminate
        // the Thread unwinds at its EL0-return checkpoint, so this never returns
        // to its caller -- the Proc dies here. The device is quiesced, so no IRQ
        // fires; the loop only re-blocks past any stale pending count.
        loop {
            let _ = self.nic.wait_irq();
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<NetPciDriver>()
}
