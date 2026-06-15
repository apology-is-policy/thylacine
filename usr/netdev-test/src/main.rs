// /netdev-test -- net-1 end-to-end proof of the reusable netdev::VirtioNet
// frame transport. Drives the same ARP round-trip the virtio-net-loop probe
// proved (P4-Jc), but THROUGH the driver's send(&[u8]) / poll_rx(&mut [u8])
// API + the recycle path + the quiesce-on-drop -- so it exercises the
// generalized, audit-bearing surface, not the hand-rolled inline mechanics.
//
// Sends N broadcast ARP requests "who-has 10.0.2.2 tell 10.0.2.15"; QEMU's
// slirp gateway synthesizes a reply per request. N_TARGET = 24 > QUEUE_SIZE
// (16), so the RX descriptors recycle past one full ring and the TX
// descriptors wrap -- the steady-state discipline a real driver needs.
//
// Spawned in joey's THYLA_BOOT_PROBES ladder BEFORE stratumd, so the net
// device's MMIO page (shared with virtio-blk) is free; the probe claims it,
// round-trips, and exits (freeing the claim) before stratumd needs it.
//
// PASS / SKIP (no virtio-net device) / FAIL (exit 1).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::{t_exits, t_puts, t_putstr};
use netdev::{OpenError, VirtioNet, MAX_FRAME};

const SLIRP_GUEST_IP: [u8; 4] = [10, 0, 2, 15];
const SLIRP_GATEWAY_IP: [u8; 4] = [10, 0, 2, 2];

const N_TARGET: u32 = 24;
const MAX_IRQ_ITERS: u32 = 256;
const MAX_DRAIN_PER_IRQ: u32 = 64;

const ETH_HDR: usize = 14;
const ARP_LEN: usize = 28;
const FRAME_LEN: usize = ETH_HDR + ARP_LEN; // 42

fn log(s: &str) {
    t_putstr(s);
}

fn log_dec(val: u32) {
    let mut buf = [0u8; 11];
    let mut n = val;
    let mut i = 11;
    if n == 0 {
        i -= 1;
        buf[i] = b'0';
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    unsafe { t_puts(buf.as_ptr().add(i), 11 - i) };
}

fn log_mac(mac: [u8; 6]) {
    let mut buf = [0u8; 17];
    let h = |x: u8| -> u8 {
        if x < 10 {
            b'0' + x
        } else {
            b'a' + (x - 10)
        }
    };
    for (i, b) in mac.iter().enumerate() {
        buf[i * 3] = h(*b >> 4);
        buf[i * 3 + 1] = h(*b & 0xf);
        if i < 5 {
            buf[i * 3 + 2] = b':';
        }
    }
    unsafe { t_puts(buf.as_ptr(), 17) };
}

// "who-has 10.0.2.2 tell 10.0.2.15", broadcast.
fn build_arp_request(src_mac: [u8; 6]) -> [u8; FRAME_LEN] {
    let mut f = [0u8; FRAME_LEN];
    for b in f.iter_mut().take(6) {
        *b = 0xff; // dst broadcast
    }
    f[6..12].copy_from_slice(&src_mac);
    f[12] = 0x08;
    f[13] = 0x06; // ARP ethertype
    let a = ETH_HDR;
    f[a] = 0x00;
    f[a + 1] = 0x01; // hw type Ethernet
    f[a + 2] = 0x08;
    f[a + 3] = 0x00; // proto IPv4
    f[a + 4] = 6; // hlen
    f[a + 5] = 4; // plen
    f[a + 6] = 0x00;
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

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut nic = match VirtioNet::open() {
        Ok(n) => n,
        Err(OpenError::NoNetDevice) => {
            log("netdev-test: SKIP -- no virtio-net device (DeviceID=1)\n");
            return 0;
        }
        Err(e) => {
            log("netdev-test: FAIL -- VirtioNet::open error ");
            log_dec(e as u32);
            log("\n");
            unsafe { t_exits(1) };
        }
    };

    let mac = nic.mac();
    log("netdev-test: mac=");
    log_mac(mac);
    log(" link=");
    log_dec(nic.link_up() as u32);
    log(" target=");
    log_dec(N_TARGET);
    log("\n");

    let req = build_arp_request(mac);
    let mut rxbuf = [0u8; MAX_FRAME];
    let mut sent: u32 = 0;
    let mut validated: u32 = 0;

    // Prime: fill the TX ring (back-pressure stops us at QUEUE_SIZE).
    while sent < N_TARGET && nic.send(&req) {
        sent += 1;
    }

    for _ in 0..MAX_IRQ_ITERS {
        if validated >= N_TARGET {
            break;
        }
        if !nic.wait_irq() {
            continue; // config-change-only wake; no ring progress
        }
        nic.drain_tx();

        let mut drained = 0u32;
        while drained < MAX_DRAIN_PER_IRQ {
            match nic.poll_rx(&mut rxbuf) {
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
        while sent < N_TARGET && nic.send(&req) {
            sent += 1;
        }
    }

    if validated < N_TARGET {
        log("netdev-test: FAIL -- ");
        log_dec(validated);
        log("/");
        log_dec(N_TARGET);
        log(" ARP replies (sent=");
        log_dec(sent);
        log(")\n");
        unsafe { t_exits(1) };
    }

    log("netdev-test: PASS -- ");
    log_dec(validated);
    log("/");
    log_dec(N_TARGET);
    log(" ARP replies via VirtioNet (send + poll_rx + recycle past ring + quiesce-on-drop)\n");
    0
}
