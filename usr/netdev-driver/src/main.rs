// /netdev-driver -- the Menagerie-bound virtio-net driver (MENAGERIE build-arc
// step 5d-3), the first USEFUL driver bound through the discovery-source layer.
//
// The warden's virtio-mmio bus source (5d-2) identifies the net slot as
// `virtio:1` and the warden binds THIS binary NARROWED -- conferring an allowance
// scoped to exactly that slot's MMIO window + INTID + a DMA pool, and nothing
// else (I-34). `probe` brings the device up entirely from that grant (no hardcoded
// base -- 5d retired `virtio.rs:51`); `serve` proves it with the net-1 ARP
// round-trip (send + poll_rx + recycle past the ring + quiesce-on-drop), then
// returns. At 5d-3 this is a one-shot proof, reaped by the warden; 5e makes it
// serve `/dev/net/0` until `DeviceRemoved`.
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
use libdriver::Error;
use netdev::{VirtioNet, MAX_FRAME};

/// Console-direct diagnostics (T_SYS_PUTS) -- visible regardless of fd wiring.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

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

struct NetDriver {
    nic: VirtioNet,
}

impl Driver for NetDriver {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        // The grant: mmio[0] = the net slot's window, irq[0] = its INTID. The warden
        // (via its bus source) supplied both from the device node, so there is no
        // bank probe -- the driver is told its exact slot.
        let (slot_pa, _) = res.mmio.first().copied().ok_or(Error::NoSuchResource)?;
        let intid = res.irq.first().copied().ok_or(Error::NoSuchResource)?;
        say!(
            "netdev-driver: grant compat={} slot={:#x} intid={} dma={:#x}",
            res.compatible,
            slot_pa,
            intid,
            res.dma_max
        );
        let nic = VirtioNet::open_slot(slot_pa, intid).map_err(|e| {
            say!("netdev-driver: open_slot failed {:?}", e);
            Error::Hardware
        })?;
        Ok(NetDriver { nic })
    }

    fn serve(mut self, _res: &BoundResources) -> Result<(), Error> {
        // The net-1 ARP round-trip proof: N_TARGET (24) > QUEUE_SIZE (16), so the RX
        // descriptors recycle past one full ring and the TX descriptors wrap. A
        // one-shot at 5d-3; 5e makes this serve /dev/net/0 until DeviceRemoved.
        let mac = self.nic.mac();
        say!(
            "netdev-driver: up mac={:02x?} link={} -- ARP round-trip x{}",
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
                "netdev-driver: FAIL -- {}/{} ARP replies (sent={})",
                validated,
                N_TARGET,
                sent
            );
            return Err(Error::Hardware);
        }
        say!(
            "netdev-driver: PASS -- {}/{} ARP replies via VirtioNet (grant-driven open_slot)",
            validated,
            N_TARGET
        );
        Ok(())
        // self.nic drops here -> quiesce-on-drop resets the device + releases the
        // slot's MMIO/IRQ/DMA claims, so a later claimant (5e re-bind) finds it free.
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<NetDriver>()
}
