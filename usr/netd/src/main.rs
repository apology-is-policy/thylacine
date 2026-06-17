// /netd -- the Thylacine network daemon (NET-DESIGN.md, the #68 charter).
//
// netd owns the NIC and runs the TCP/IP stack. The Menagerie warden binds it
// NARROWED to the virtio-pci:1 function's allowance (PCI bdf + INTID + a DMA
// pool, nothing else -- I-34); the NIC handles are non-transferable (I-5), so
// the Proc that CLAIMS the device is the Proc that RUNS the stack. This is the
// reconciliation of the charter's "joey spawns netd" with the Menagerie
// warden-binds-narrowed model: netd is just the netdev-pci-driver evolved from
// the ARP-proof demo into the real daemon, conferred exactly the NIC.
//
// net-2a (THIS sub-chunk) proves smoltcp lives on the PCI NIC: it brings the
// link up by acquiring a DHCP lease from QEMU's slirp (10.0.2.15) -- exercising
// the whole lower stack (Ethernet TX/RX over the BAR-mapped virtqueues, ARP,
// UDP, and the DHCP client state machine) end-to-end through smoltcp. It is a
// ONE-SHOT proof: on a lease it prints PASS and exits 0, so the warden reaps it
// as Up with NO teardown logic involved. net-2b makes netd a persistent 9P
// server for /net.
//
// Diagnostics go to the console (`t_putstr`): a warden-spawned driver's stderr
// is /dev/null, and a one-shot signals completion by EXITING (the warden's
// `try_wait`), never by a "READY" line (which is the long-lived-service contract).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libdriver::driver::{run, Driver};
use libdriver::resource::BoundResources;
use libdriver::{DeviceId, Error};
use libthyla_rs::io::Write;
use libthyla_rs::time::{sleep, Duration, Instant};
use netdev::{VirtioNetPci, MAX_FRAME};

use smoltcp::iface::{Config, Interface, SocketSet};
use smoltcp::phy::{self, Device, DeviceCapabilities, Medium};
use smoltcp::socket::dhcpv4;
use smoltcp::time::Instant as SmolInstant;
use smoltcp::wire::{EthernetAddress, HardwareAddress, IpCidr};

/// Console-direct diagnostics (T_SYS_PUTS) -- visible regardless of fd wiring.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        let _ = libthyla_rs::t_putstr(&s);
    }};
}

/// The virtio device-id this driver drives: 1 = net (VIRTIO 1.2 section 5.1).
const VIRTIO_ID_NET: u16 = 1;

/// The Ethernet L2 header length (smoltcp builds the full frame incl. header).
const ETHERNET_HEADER: usize = 14;

/// DHCP poll cadence + bound. A sleep-poll loop (not an IRQ-blocking wait): it
/// cannot hang and is self-bounding, the right shape for a one-shot boot proof.
/// slirp answers DISCOVER/REQUEST immediately and losslessly, so the exchange
/// converges in a few polls; the bound (~5s) is the fail-closed backstop.
const POLL_MS: u64 = 10;
const MAX_POLLS: u32 = 500;

/// The resident loop's idle poll bounds (ms). netd honors smoltcp's poll-delay
/// hint clamped to [MIN, MAX]: the MIN floor forecloses a 0ms spin if the stack
/// ever asks for an immediate re-poll (no busy-spin, #108); the MAX ceiling keeps
/// netd responsive and bounds the idle-wakeup rate. Post-lease with no active
/// sockets the hint is the DHCP renew deadline (minutes), so idle netd wakes
/// ~once/sec. net-2b-2 replaces this loop with the IRQ + 9P-accept event loop.
const IDLE_POLL_MIN_MS: u64 = 50;
const IDLE_POLL_MAX_MS: u64 = 1000;

// =============================================================================
// smoltcp phy::Device over the virtio-net-pci NIC.
//
// The classic no-alias pattern: the RxToken OWNS its received bytes (a Vec, no
// device borrow), and the TxToken holds the single `&mut nic` borrow. `receive`
// thus hands back both from one `&mut self` without aliasing -- the RX poll
// completes (filling an owned buffer) before the `&mut nic` for the TxToken.
// =============================================================================

struct NicDevice {
    nic: VirtioNetPci,
}

struct NicRxToken {
    frame: Vec<u8>,
}

struct NicTxToken<'a> {
    nic: &'a mut VirtioNetPci,
}

impl Device for NicDevice {
    type RxToken<'a>
        = NicRxToken
    where
        Self: 'a;
    type TxToken<'a>
        = NicTxToken<'a>
    where
        Self: 'a;

    fn receive(&mut self, _t: SmolInstant) -> Option<(Self::RxToken<'_>, Self::TxToken<'_>)> {
        let mut buf = [0u8; MAX_FRAME];
        match self.nic.poll_rx(&mut buf) {
            Some(n) => Some((
                NicRxToken {
                    frame: buf[..n].to_vec(),
                },
                NicTxToken { nic: &mut self.nic },
            )),
            None => None,
        }
    }

    fn transmit(&mut self, _t: SmolInstant) -> Option<Self::TxToken<'_>> {
        Some(NicTxToken { nic: &mut self.nic })
    }

    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.medium = Medium::Ethernet;
        // The maximum L2 frame `nic.send` accepts -- so smoltcp never builds a
        // frame the NIC would drop, and the derived IP MTU (MAX_FRAME - 14) is
        // exact. The device does its own checksums in software (smoltcp default).
        c.max_transmission_unit = MAX_FRAME;
        c
    }
}

impl phy::RxToken for NicRxToken {
    fn consume<R, F>(self, f: F) -> R
    where
        F: FnOnce(&[u8]) -> R,
    {
        f(&self.frame)
    }
}

impl phy::TxToken for NicTxToken<'_> {
    fn consume<R, F>(self, len: usize, f: F) -> R
    where
        F: FnOnce(&mut [u8]) -> R,
    {
        let n = len.min(MAX_FRAME);
        let mut buf = [0u8; MAX_FRAME];
        let r = f(&mut buf[..n]);
        // Back-pressure-tolerant: `send` self-drains the TX ring and drops the
        // frame only if still full -- acceptable, since smoltcp retransmits.
        let _ = self.nic.send(&buf[..n]);
        r
    }
}

// =============================================================================
// The driver.
// =============================================================================

struct NetD {
    nic: VirtioNetPci,
}

impl Driver for NetD {
    fn probe(res: &BoundResources) -> Result<Self, Error> {
        // The warden must have bound us to the device class we drive: a grant
        // carrying any other identity is a manifest mis-bind -- fail closed
        // before touching hardware (the netdev-pci-driver discipline).
        match DeviceId::parse(&res.compatible) {
            DeviceId::VirtioPci(VIRTIO_ID_NET) => {}
            other => {
                say!(
                    "netd: bound to {} (expected virtio-pci:{}) -- refusing",
                    other.as_string(),
                    VIRTIO_ID_NET
                );
                return Err(Error::NoMatch);
            }
        }
        say!(
            "netd: grant compat={} pci={:?} irq={} dma={:#x}",
            res.compatible,
            res.pci,
            res.irq.len(),
            res.dma_max
        );
        let nic = VirtioNetPci::open().map_err(|e| {
            say!("netd: NIC open failed {:?}", e);
            Error::Hardware
        })?;
        Ok(NetD { nic })
    }

    fn serve(self, _res: &BoundResources) -> Result<(), Error> {
        let mac = self.nic.mac();
        say!(
            "netd: up mac={:02x?} link={} mtu={} -- bringing the link up (DHCP)",
            mac,
            self.nic.link_up(),
            self.nic.mtu()
        );

        let mut device = NicDevice { nic: self.nic };

        // The interface: Ethernet medium, the NIC's MAC, a random transaction
        // seed (the DHCP xid + TCP ISN source; from the kernel CSPRNG).
        let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress(mac)));
        let mut seed = [0u8; 8];
        let _ = libthyla_rs::rand::fill_bytes(&mut seed);
        config.random_seed = u64::from_le_bytes(seed);

        // smoltcp's clock: a monotonic millisecond count off a base Instant.
        let base = Instant::now();
        let now = |base: &Instant| SmolInstant::from_millis(base.elapsed().as_millis() as i64);

        let mut iface = Interface::new(config, &mut device, now(&base));
        let mut sockets = SocketSet::new(Vec::new());
        let dhcp = dhcpv4::Socket::new();
        let dhcp_handle = sockets.add(dhcp);

        // Bring-up: poll the stack until the DHCP client leases an address. A
        // bounded sleep-poll loop (not an IRQ wait) -- it cannot hang, the right
        // shape for a boot bring-up. slirp answers immediately, so it converges in
        // a few polls; the bound (~5s) is the fail-closed backstop.
        let mut leased = false;
        for _ in 0..MAX_POLLS {
            let ts = now(&base);
            iface.poll(ts, &mut device, &mut sockets);

            if let Some(dhcpv4::Event::Configured(cfg)) =
                sockets.get_mut::<dhcpv4::Socket>(dhcp_handle).poll()
            {
                iface.update_ip_addrs(|addrs| {
                    addrs.clear();
                    let _ = addrs.push(IpCidr::Ipv4(cfg.address));
                });
                if let Some(router) = cfg.router {
                    let _ = iface.routes_mut().add_default_ipv4_route(router);
                }
                // The IP MTU is the L2 frame minus the Ethernet header.
                say!(
                    "netd: DHCP lease addr={} router={:?} dns={} ip-mtu={}",
                    cfg.address,
                    cfg.router,
                    cfg.dns_servers.len(),
                    MAX_FRAME - ETHERNET_HEADER
                );
                leased = true;
                break;
            }

            let _ = sleep(Duration::from_millis(POLL_MS));
        }

        if !leased {
            say!(
                "netd: FAIL -- no DHCP lease after {} polls (~{}s)",
                MAX_POLLS,
                MAX_POLLS as u64 * POLL_MS / 1000
            );
            return Err(Error::Hardware);
        }

        // A persistent service, not a one-shot. Announce success on the console,
        // then signal readiness LAST: the READY line is what the warden's
        // readiness pipe waits on, so all console output precedes it (the
        // netdev-driver ordering discipline). The manifest's `lifecycle =
        // persistent` then has the warden LEAVE netd running. net-2b-2 stands the
        // /net 9P server up here, in the same loop.
        say!("netd: PASS -- smoltcp brought the link up via the PCI NIC (DHCP)");
        say!("netd: serving (persistent; /net 9P server lands at net-2b-2)");
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");

        // The resident loop: keep servicing the stack (DHCP renew, ARP) until the
        // Proc dies. Honoring smoltcp's poll-delay hint (clamped) keeps an idle
        // netd from busy-spinning (#108); net-2b-2 replaces it with the IRQ +
        // 9P-accept multiplexed event loop.
        loop {
            let ts = now(&base);
            iface.poll(ts, &mut device, &mut sockets);
            let ms = iface
                .poll_delay(ts, &sockets)
                .map(|d| d.total_millis())
                .unwrap_or(IDLE_POLL_MAX_MS)
                .clamp(IDLE_POLL_MIN_MS, IDLE_POLL_MAX_MS);
            let _ = sleep(Duration::from_millis(ms));
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<NetD>()
}
