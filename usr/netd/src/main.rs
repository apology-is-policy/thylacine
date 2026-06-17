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
// netd brings the link up by acquiring a DHCP lease from QEMU's slirp
// (10.0.2.15) -- exercising the whole lower stack (Ethernet TX/RX over the
// BAR-mapped virtqueues, ARP, UDP, and the DHCP client state machine) end-to-end
// through smoltcp (net-2a). net-2b-1 made it a PERSISTENT service (the warden
// leaves it running on READY, via `lifecycle = persistent`). net-2b-2 stood the
// /net 9P server up: after the lease, netd posts /srv/net (9P-mode), then runs a
// combined event loop that multiplexes the 9P server with the smoltcp stack;
// joey mounts /srv/net at /net so the namespace inherits it. net-2c-1 grew the
// static skeleton into the live TCP fid state machine (section 3.4):
// /net/tcp/clone mints a refcounted connection N, the /net/tcp/N/ directory +
// its files appear, Treaddir lists them, and the last clunk frees N. net-2c-2
// (this sub-chunk) makes it LIVE: clone reserves a real smoltcp TCP socket, the
// Net table owns the interface + socket set, writing `ctl` "connect a!p"
// active-opens, status/local/remote report the live socket, and data read/write
// is recv/send. The last clunk frees N AND its socket.
//
// Diagnostics go to the console (`t_putstr`): a warden-spawned driver's stderr
// is /dev/null. A long-lived service signals readiness by writing exactly one
// "READY" line to stdout (the warden's readiness pipe) AFTER all console output
// and AFTER posting its service -- so READY also means "/srv/net is up".

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
use libthyla_rs::{t_close, t_poll, t_srv_accept, TPollFd, T_POLLHUP, T_POLLIN};
use netdev::{VirtioNetPci, MAX_FRAME};

mod server;

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

        say!("netd: PASS -- smoltcp brought the link up via the PCI NIC (DHCP)");

        // Post the /net 9P service (9P-mode) into the boot namespace's /srv
        // BEFORE signaling READY: the warden's READY -> "left running" then also
        // means "/srv/net is posted", so joey (after its warden wait) is
        // guaranteed to find it for the mount. A post failure (most likely a
        // missing MAY_POST_SERVICE -- conferred warden->netd via the persistent
        // lifecycle) is fail-closed: netd does NOT signal READY, the warden logs
        // it as gave-up, and the box still boots (just no /net).
        let listener = match server::post_srv_net() {
            Ok(l) => l,
            Err(()) => {
                say!("netd: FAIL -- could not post /srv/net (MAY_POST_SERVICE?)");
                return Err(Error::Hardware);
            }
        };

        // A persistent service, not a one-shot. Signal readiness LAST: the READY
        // line is what the warden's readiness pipe waits on, so all console
        // output precedes it (the netdev-driver ordering discipline). The
        // manifest's `lifecycle = persistent` then has the warden LEAVE netd
        // running.
        say!("netd: serving /net (9P over /srv/net)");
        let mut out = libthyla_rs::io::stdout();
        let _ = out.write_all(b"READY\n");

        // The resident event loop multiplexes the stack with the /net 9P server.
        // `t_poll` over [listener] + connections wakes on a 9P request; its
        // timeout is smoltcp's poll-delay hint (clamped), so on an idle tick the
        // stack is still serviced (DHCP renew, ARP) at least that often. With an
        // active TCP socket smoltcp's hint is short, so the clamp floor governs
        // (<=50ms RX latency under load). A pollable NIC-IRQ fd for RX-driven
        // wakeups would need a kernel ABI surface (SYS_IRQ_WAIT blocks; it is not
        // pollable) -- deferred; the timeout poll is correct, just not minimal.
        let mut conns: Vec<server::Conn> = Vec::new();
        // The global /net connection table (the section-3.4 fid state machine),
        // shared across every 9P session netd accepts. It now OWNS the smoltcp
        // interface + socket set (moved in here, post-DHCP) so the 9P data path
        // reaches them; `device` stays local (only `net.poll` borrows it).
        let mut net = server::Net::new(iface, sockets, base);
        let mut pollfds = [TPollFd::default(); 1 + server::MAX_CONNS];
        loop {
            net.poll(&mut device);

            // Deliver any deferred accepts whose inbound call has landed: rebind
            // the blocked listen fid onto the accepted connection's ctl and send
            // the held Rlopen (NET-DESIGN 3.4 server side). A write failure means
            // the issuing session died -> tear that connection down.
            for d in net.poll_accepts() {
                match conns.iter().position(|c| c.handle() == d.conn_id()) {
                    Some(idx) => {
                        if !conns[idx].complete_accept(&mut net, d) {
                            conns[idx].teardown(&mut net);
                            let _ = unsafe { t_close(conns[idx].handle()) };
                            conns.remove(idx);
                        }
                    }
                    // The issuing connection already closed: free the minted
                    // connection (it has no owner).
                    None => net.discard_accept(d),
                }
            }

            // With a pending accept, the inbound SYN arrives on the NIC (not a
            // pollable fd), so only a timeout-driven net.poll catches it: clamp to
            // the floor to keep accept latency <= IDLE_POLL_MIN_MS.
            let delay = if net.has_pending_accepts() {
                IDLE_POLL_MIN_MS
            } else {
                net.poll_delay_ms()
                    .unwrap_or(IDLE_POLL_MAX_MS)
                    .clamp(IDLE_POLL_MIN_MS, IDLE_POLL_MAX_MS)
            };

            pollfds[0] = TPollFd {
                fd: listener as i32,
                events: T_POLLIN,
                revents: 0,
            };
            let nc = conns.len().min(server::MAX_CONNS);
            for i in 0..nc {
                pollfds[1 + i] = TPollFd {
                    fd: conns[i].handle() as i32,
                    events: T_POLLIN,
                    revents: 0,
                };
            }
            let nfds = 1 + nc;
            let rc = unsafe { t_poll(pollfds.as_mut_ptr(), nfds, delay as i32) };
            if rc < 0 {
                // Unexpected poll error on otherwise-valid fds: back off so a
                // persistent error cannot become a busy-spin (#108 discipline).
                let _ = sleep(Duration::from_millis(IDLE_POLL_MAX_MS));
                continue;
            }
            if rc == 0 {
                continue; // timeout: re-service the stack
            }

            // Accept a new connection (one per tick is enough; the listener
            // re-fires next iteration if more are pending).
            if pollfds[0].revents & T_POLLIN != 0 && conns.len() < server::MAX_CONNS {
                let h = unsafe { t_srv_accept(listener) };
                if h >= 0 {
                    conns.push(server::Conn::new(h));
                }
            }

            // Service ready connections backward, so a remove() of a closed
            // connection does not shift an unvisited lower index (the accept
            // above only appended, leaving [0, nc) stable for this pass).
            let mut i = nc;
            while i > 0 {
                i -= 1;
                let pf = pollfds[1 + i];
                let mut close = false;
                if pf.revents & T_POLLIN != 0 && !conns[i].service(&mut net) {
                    close = true;
                }
                if pf.revents & T_POLLHUP != 0 {
                    close = true;
                }
                if close {
                    // Drop this session's connection refs before the Conn dies,
                    // so any /net/tcp/N/ it alone held open is freed (the only
                    // free path besides an explicit clunk).
                    conns[i].teardown(&mut net);
                    let _ = unsafe { t_close(conns[i].handle()) };
                    conns.remove(i);
                }
            }

            // Flush any connect/send the dispatch just enqueued before sleeping,
            // so a SYN/data egresses this tick rather than waiting for the next
            // poll timeout.
            net.poll(&mut device);
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run::<NetD>()
}
