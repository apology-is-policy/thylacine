// The /net 9P2000.L server (NET-DESIGN.md section 3).
//
// net-2c-1 grew the net-2b-2 static skeleton into the live fid state machine
// (section 3.4) for TCP: opening `/net/tcp/clone` MINTS a connection N, assigns
// it a `/net/tcp/N/` directory (`ctl`/`data`/`local`/`remote`/`status`/`err`),
// and rebinds the opened fid onto that connection's `ctl` (the Plan 9 clone
// idiom -- the kernel dev9p client accepts an Rlopen qid that differs from the
// walked qid). Reading `ctl` yields N. A connection is reference-counted by the
// fids that name its subtree; the LAST clunk frees N (the only free path, the
// I-10/I-11 invariant). Treaddir lists the live N directories.
//
// net-2c-2 makes the connection LIVE: clone reserves a real smoltcp TCP socket
// (the section-3.4 ALLOCATED state), and the `Net` table now OWNS the smoltcp
// interface + socket set (moved in after DHCP) so the 9P dispatch reaches them.
// Writing `ctl` "connect a.b.c.d!port" active-opens the socket (CONNECTING);
// `status`/`local`/`remote` report the live state + endpoints; `data` read/write
// IS recv/send on the byte stream. The last clunk frees N AND removes its
// smoltcp socket. `data` I/O is non-blocking at net-2c-2 (a 0-length read is
// ambiguous between "no data yet" and EOF); blocking/readiness is the dev9p.poll
// leg (net-6). `announce`/`listen` (the server side) is net-3.
//
// netd is single-threaded (one Proc, one serve() loop): every 9P frame across
// every session is processed sequentially, so the global connection table (the
// `Net` slot array) AND the smoltcp socket set need no lock -- the refcount and
// the stack are single-threaded-safe by construction (the section-3.4 "netd
// serializes its own connection table").
//
// netd posts /srv/net 9P-mode (perm=0; requires PROC_FLAG_MAY_POST_SERVICE,
// conferred warden->netd because netd's manifest is `lifecycle = persistent`).
// joey's open=connect mount (t_open(/srv/net, OREAD) -> dev9p root -> t_mount)
// drives ONE kernel dev9p-client 9P session over which every /net access from
// every Proc in the namespace is multiplexed. The codec is libthyla_rs::ninep.

use crate::NicDevice;
use alloc::vec::Vec;
use libthyla_rs::ninep as p9;
use libthyla_rs::time::{sleep, Duration, Instant};
use libthyla_rs::{
    t_close, t_open, t_walk_create, T_GID_SYSTEM, T_OPATH, T_OREAD, T_PRINCIPAL_SYSTEM,
    T_WALK_OPEN_FROM_ROOT,
};
use smoltcp::iface::{Interface, SocketHandle, SocketSet};
use smoltcp::socket::{tcp, udp};
use smoltcp::time::Instant as SmolInstant;
use smoltcp::wire::{IpAddress, IpEndpoint, IpListenEndpoint};

/// Max concurrent 9P connections the accept loop tracks. In practice the dev9p
/// mount drives ONE kernel-client session; the headroom covers a future direct
/// open=connect consumer (a native /net client that does not cross the mount).
pub const MAX_CONNS: usize = 8;

/// Per-connection fid-table size: one fid per open file/dir the client holds.
const MAX_FIDS: usize = 32;

/// Max live `/net/tcp/N/` connection slots. A bound, not headroom: an unbounded
/// connection table is a DoS vector (#65 resource floor), so clone-minting fails
/// (ENFILE) past this. Raise as the workload demands.
const MAX_SLOTS: usize = 16;

/// Max deferred accepts in flight (one held listen Rlopen per entry). Bounds the
/// pending-accept table (#65 resource floor); a listen open past this is rejected
/// (ENOMEM) rather than deferred.
const MAX_PENDING_ACCEPTS: usize = MAX_SLOTS;

/// Server-negotiated msize. Bounds every frame; the per-conn buffers are sized
/// to it. 8 KiB holds the largest reply (a stats read, an Rgetattr, or a full
/// Treaddir page of the shallow /net tree).
const SRV_MSIZE: u32 = 8192;
const SRV_MSIZE_USIZE: usize = SRV_MSIZE as usize;

/// Per-connection smoltcp TCP socket buffer sizes (rx/tx). 4 KiB each is a
/// modest TCP window -- enough for the net-2c-2 control path plus a small
/// stream; MAX_SLOTS * (rx + tx) bounds netd's socket memory (16 * 8 KiB).
const TCP_RX_BUF: usize = 4096;
const TCP_TX_BUF: usize = 4096;

/// Per-connection smoltcp UDP socket buffer sizes (net-3b). A UDP PacketBuffer
/// stores whole datagrams WITH per-packet metadata (sender endpoint), unlike
/// TCP's byte stream: UDP_META_SLOTS datagrams each up to UDP_*_BUF payload
/// bytes. 8 slots x 4 KiB holds a burst of small datagrams (DNS, NTP) plus a
/// full-MTU packet.
const UDP_META_SLOTS: usize = 8;
const UDP_RX_BUF: usize = 4096;
const UDP_TX_BUF: usize = 4096;

/// The largest `data` chunk a single Tread drains from a socket's rx buffer.
const DATA_CHUNK: usize = 4096;

/// The ephemeral local-port range floor (IANA 49152..=65535). smoltcp's
/// `connect` requires a non-zero local port (it auto-selects only the local
/// ADDRESS), so netd assigns one from this range, rotating per active open.
const EPHEMERAL_LO: u16 = 49152;

/// net-3b UDP round-trip demo (a DNS query to slirp's resolver). BEST-EFFORT,
/// logged, never asserted: slirp FORWARDS DNS to the host's resolver (unlike its
/// internal DHCP), so a response is host-environment-dependent. The bound is
/// tight -- on a resolver-equipped host the round-trip returns in a few polls;
/// otherwise it gives up fast and netd proceeds (the /net/udp machinery is the
/// deterministic proof, via joey; the UDP-via-9P round-trip E2E is owed net-3d).
const DNS_PROBE_ID: u16 = 0x7a51;
const DNS_PROBE_POLLS: u32 = 30; // ~300ms worst case
const DNS_PROBE_POLL_MS: u64 = 10;
const DNS_PROBE_SERVER: [u8; 4] = [10, 0, 2, 3]; // QEMU slirp's DNS forwarder
const DNS_PROBE_QNAME: &[u8] = b"example.com";

const P9_VERSION_9P2000_L: &[u8] = b"9P2000.L";

// Linux mode bits. The /net nodes are SYSTEM-owned but WORLD-accessible (dirs
// r-x; the rw control/data files rw; the read-only introspection files r): the
// kernel's A-3 dev9p enforcement runs a per-component X-search against the
// accessing principal, and the /net firewall is namespace reachability (section
// 8), NOT file mode -- so a Proc that can name /net can dial. World-rw on
// clone/ctl/data is the intended "anyone with /net dials" semantics.
const S_IFDIR: u32 = 0o040000;
const S_IFREG: u32 = 0o100000;
const DIR_MODE: u32 = S_IFDIR | 0o555;
const FILE_RW: u32 = S_IFREG | 0o666;
const FILE_RO: u32 = S_IFREG | 0o444;

/// Tgetattr request-mask: size. ninep exports MODE/NLINK/UID/GID; STATX_SIZE is
/// 0x200 (Linux v9fs), filled so a stat reports a file's length.
const P9_GETATTR_SIZE: u64 = 0x200;

// =============================================================================
// The /net tree. qid.path encodes the node identity in two disjoint ranges:
//
//   - Static skeleton nodes occupy the small fixed range [0, STATIC_MAX).
//   - A live connection N under protocol `proto` encodes as
//       CONN_FLAG | (proto << PROTO_SHIFT) | (n << FILE_BITS) | filekind
//     so a connection's directory and its files are each one stable qid.
//
// The ranges never collide (CONN_FLAG is bit 40; STATIC_MAX is small). A walk
// only resolves a connection node when its slot is LIVE, so a stale/forged
// connection qid is unreachable.
// =============================================================================

const P_ROOT: u64 = 0; //       /net
const P_TCP: u64 = 1; //        /net/tcp
const P_UDP: u64 = 2; //        /net/udp
const P_ICMP: u64 = 3; //       /net/icmp
const P_TCP_CLONE: u64 = 4; //  /net/tcp/clone
const P_TCP_STATS: u64 = 5; //  /net/tcp/stats
const P_UDP_STATS: u64 = 6; //  /net/udp/stats
const P_ICMP_STATS: u64 = 7; // /net/icmp/stats
const P_UDP_CLONE: u64 = 8; //  /net/udp/clone (net-3b)

const CONN_FLAG: u64 = 1 << 40;
const PROTO_SHIFT: u64 = 32;
const FILE_BITS: u64 = 8;
const FILE_MASK: u64 = 0xff;
const N_MASK: u64 = 0x00ff_ffff; // 24-bit connection number

// Protocols. The qid `proto` field selects the per-slot socket type (TCP stream
// vs UDP datagram), so the type-recovering `get::<tcp::Socket>` / `get::<udp::
// Socket>` is always matched to the slot (a mismatch panics in smoltcp).
const PROTO_TCP: u64 = 0;
const PROTO_UDP: u64 = 1; // net-3b

// Per-connection file kinds (the filekind low byte).
const FK_DIR: u64 = 0; //    /net/tcp/N/
const FK_CTL: u64 = 1; //    /net/tcp/N/ctl
const FK_DATA: u64 = 2; //   /net/tcp/N/data
const FK_LOCAL: u64 = 3; //  /net/tcp/N/local
const FK_REMOTE: u64 = 4; // /net/tcp/N/remote
const FK_STATUS: u64 = 5; // /net/tcp/N/status
const FK_ERR: u64 = 6; //    /net/tcp/N/err
const FK_LISTEN: u64 = 7; // /net/tcp/N/listen (the server accept file; net-3a)

fn is_conn(path: u64) -> bool {
    path & CONN_FLAG != 0
}

fn make_conn(proto: u64, n: u32, filekind: u64) -> u64 {
    CONN_FLAG | (proto << PROTO_SHIFT) | ((n as u64) << FILE_BITS) | filekind
}

fn conn_proto(path: u64) -> u64 {
    (path >> PROTO_SHIFT) & 0xff
}

fn conn_n(path: u64) -> u32 {
    ((path >> FILE_BITS) & N_MASK) as u32
}

fn conn_filekind(path: u64) -> u64 {
    path & FILE_MASK
}

/// The connection number a path belongs to (any node in its subtree), or None
/// for a static node. The refcount key: every fid bound here holds slot N live.
fn path_conn_n(path: u64) -> Option<u32> {
    if is_conn(path) {
        Some(conn_n(path))
    } else {
        None
    }
}

fn proto_dir(proto: u64) -> u64 {
    match proto {
        PROTO_UDP => P_UDP,
        _ => P_TCP,
    }
}

// =============================================================================
// Net -- the global connection table + live counters. Shared across all 9P
// connections (the /net namespace is one tree); passed &mut to Conn::service.
// =============================================================================

#[derive(Copy, Clone)]
struct Slot {
    used: bool,
    refs: u32,
    // Which smoltcp socket type backs this slot (TCP vs UDP): the type the
    // get::<T>() recovery must match. Meaningless when !used.
    proto: u64,
    socket: Option<SocketHandle>,
    // The endpoints recorded at `connect`/accept time (netd-side), so
    // `local`/`remote` report a stable tuple regardless of the live socket's
    // later transitions.
    local: Option<([u8; 4], u16)>,
    remote: Option<([u8; 4], u16)>,
    err: Option<&'static str>,
    // The announce endpoint (Some once `announce` made this connection's socket
    // listen): the listener N re-arms a fresh socket on this endpoint after each
    // accept, so N stays ANNOUNCED for the next call (NET-DESIGN 3.4). TCP only.
    listen_ep: Option<IpListenEndpoint>,
}

impl Slot {
    const fn empty() -> Slot {
        Slot {
            used: false,
            refs: 0,
            proto: PROTO_TCP,
            socket: None,
            local: None,
            remote: None,
            err: None,
            listen_ep: None,
        }
    }
}

/// A deferred accept: a client's blocking `open(/net/tcp/N/listen)` whose Rlopen
/// netd is HOLDING until an inbound call lands on listener N. The client's open()
/// stays blocked on the outstanding 9P tag; netd replies (rebinding the fid onto
/// the accepted connection's ctl) when `listening_n`'s socket establishes. This
/// is the committed-blocking realization of NET-DESIGN 3.4 over the existing
/// dev9p client (match-by-tag, no per-op deadline, death-interruptible) -- no
/// kernel surface; 12's readiness multiplexing (poll/Loom) is the separate net-6
/// leg.
#[derive(Copy, Clone)]
struct PendingAccept {
    conn_id: i64,     // the 9P connection (Conn handle) that issued the listen
    tag: u16,         // the held Tlopen tag to reply to
    fid: u32,         // the fid to rebind onto the accepted connection's ctl
    listening_n: u32, // the ANNOUNCED connection whose socket accepts the call
}

/// A completed accept ready to deliver: the serve loop rebinds `fid` onto the
/// new connection's ctl and sends the held Rlopen on `conn_id`. Opaque to the
/// serve loop (main.rs) -- it only routes the token by `conn_id()`.
#[derive(Copy, Clone)]
pub struct AcceptDone {
    conn_id: i64,
    tag: u16,
    fid: u32,
    new_n: u32,
    ctl_qid_path: u64,
}

impl AcceptDone {
    /// The 9P connection that issued the (now-completed) blocking listen.
    pub fn conn_id(&self) -> i64 {
        self.conn_id
    }
}

/// The outcome of the net-3b best-effort UDP round-trip demo (a DNS query). The
/// serve loop logs it; it is never a boot gate (the round-trip is host-resolver-
/// dependent).
pub enum DnsProbe {
    /// A DNS response came back (id matched, QR set); `ancount` answer records.
    Ok { resp_len: usize, ancount: u16 },
    /// No response within the bound (a host with no resolver, most likely).
    NoResponse,
    /// Could not even mint the probe socket (the slot table was full).
    MintFailed,
}

pub struct Net {
    iface: Interface,
    sockets: SocketSet<'static>,
    base: Instant,
    next_local_port: u16,
    slots: [Slot; MAX_SLOTS],
    tcp_active: u32, // currently-live TCP connections (the `active` stat)
    tcp_opened: u32, // total TCP connections ever minted
    udp_active: u32, // currently-live UDP connections (net-3b)
    udp_opened: u32, // total UDP connections ever minted
    // Deferred accepts: held listen Rlopens awaiting an inbound call (net-3a).
    pending: Vec<PendingAccept>,
}

impl Net {
    /// Build the connection table around the already-configured smoltcp
    /// interface + socket set (moved in after DHCP bring-up). `base` is the
    /// monotonic anchor for smoltcp's millisecond clock.
    pub fn new(iface: Interface, sockets: SocketSet<'static>, base: Instant) -> Net {
        Net {
            iface,
            sockets,
            base,
            next_local_port: EPHEMERAL_LO,
            slots: [Slot::empty(); MAX_SLOTS],
            tcp_active: 0,
            tcp_opened: 0,
            udp_active: 0,
            udp_opened: 0,
            pending: Vec::new(),
        }
    }

    /// smoltcp's monotonic timestamp (ms since `base`).
    pub fn now(&self) -> SmolInstant {
        SmolInstant::from_millis(self.base.elapsed().as_millis() as i64)
    }

    /// Service the stack once: TX flush + RX drain across every socket. Called
    /// at the top of each serve-loop iteration and again after dispatching a
    /// batch of 9P requests, so a just-enqueued connect/send egresses this tick
    /// rather than waiting for the next poll timeout. `device` stays owned by
    /// the serve loop (only `iface.poll` borrows it).
    pub fn poll(&mut self, device: &mut NicDevice) {
        let ts = self.now();
        self.iface.poll(ts, device, &mut self.sockets);
    }

    /// smoltcp's poll-delay hint (ms): how long until the stack next needs
    /// servicing. None == idle (no deadline). The serve loop clamps it.
    pub fn poll_delay_ms(&mut self) -> Option<u64> {
        let ts = self.now();
        self.iface
            .poll_delay(ts, &self.sockets)
            .map(|d| d.total_millis())
    }

    /// net-3b boot demo: a self-contained DNS round-trip through the live
    /// /net/udp data path (udp_clone -> connect -> data_send -> poll ->
    /// data_recv), bounded-polled like the DHCP bring-up so it is deterministic
    /// in its own loop (no host-timing race). BEST-EFFORT: slirp forwards DNS to
    /// the host resolver, so a response is environment-dependent; the caller logs
    /// the outcome and never gates the boot on it. The probe connection is minted
    /// + freed entirely within this call (no slot leak across it).
    pub fn udp_dns_probe(&mut self, device: &mut NicDevice) -> DnsProbe {
        let n = match self.udp_clone() {
            Some(n) => n,
            None => return DnsProbe::MintFailed,
        };
        if self.udp_connect(n, DNS_PROBE_SERVER, 53).is_err() {
            self.free_orphan_mint(n);
            return DnsProbe::NoResponse;
        }
        let query = build_dns_query(DNS_PROBE_ID, DNS_PROBE_QNAME);
        let _ = self.data_send(n, &query);

        let mut buf = [0u8; 512];
        let mut result = DnsProbe::NoResponse;
        for _ in 0..DNS_PROBE_POLLS {
            self.poll(device);
            let k = self.data_recv(n, &mut buf);
            // A DNS response: >= a 12-byte header, matching id, QR bit set.
            if k >= 12
                && buf[0] == (DNS_PROBE_ID >> 8) as u8
                && buf[1] == (DNS_PROBE_ID & 0xff) as u8
                && (buf[2] & 0x80) != 0
            {
                let ancount = u16::from_be_bytes([buf[6], buf[7]]);
                result = DnsProbe::Ok {
                    resp_len: k,
                    ancount,
                };
                break;
            }
            let _ = sleep(Duration::from_millis(DNS_PROBE_POLL_MS));
        }
        // The probe connection has refs==0 (never bound to a fid): free it.
        self.free_orphan_mint(n);
        result
    }

    /// Mint a TCP connection: claim a free slot, refs=0 (the opener takes the
    /// first ref). Returns the connection number N, or None if the table is
    /// full. The slot is NOT yet freeable -- only slot_unref frees, and the
    /// caller refs it before any unref can occur.
    fn tcp_clone(&mut self) -> Option<u32> {
        let n = self.slots.iter().position(|s| !s.used)?;
        // Reserve the smoltcp socket now (the section-3.4 ALLOCATED state: one
        // socket bound to the connection for its whole open lifetime). It is
        // removed only when the last fid clunks (slot_unref -> refs 0).
        let rx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_RX_BUF]);
        let tx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_TX_BUF]);
        let h = self.sockets.add(tcp::Socket::new(rx, tx));
        self.slots[n] = Slot {
            used: true,
            refs: 0,
            proto: PROTO_TCP,
            socket: Some(h),
            local: None,
            remote: None,
            err: None,
            listen_ep: None,
        };
        self.tcp_active += 1;
        self.tcp_opened += 1;
        Some(n as u32)
    }

    /// Mint a UDP connection (net-3b): the datagram analog of tcp_clone. A UDP
    /// socket buffers whole datagrams with per-packet metadata (sender endpoint),
    /// so its PacketBuffer carries metadata slots alongside the payload bytes.
    fn udp_clone(&mut self) -> Option<u32> {
        let n = self.slots.iter().position(|s| !s.used)?;
        let rx = udp::PacketBuffer::new(
            alloc::vec![udp::PacketMetadata::EMPTY; UDP_META_SLOTS],
            alloc::vec![0u8; UDP_RX_BUF],
        );
        let tx = udp::PacketBuffer::new(
            alloc::vec![udp::PacketMetadata::EMPTY; UDP_META_SLOTS],
            alloc::vec![0u8; UDP_TX_BUF],
        );
        let h = self.sockets.add(udp::Socket::new(rx, tx));
        self.slots[n] = Slot {
            used: true,
            refs: 0,
            proto: PROTO_UDP,
            socket: Some(h),
            local: None,
            remote: None,
            err: None,
            listen_ep: None,
        };
        self.udp_active += 1;
        self.udp_opened += 1;
        Some(n as u32)
    }

    fn slot_live(&self, n: u32) -> bool {
        (n as usize) < MAX_SLOTS && self.slots[n as usize].used
    }

    /// The protocol backing a live slot (the get::<T>() type discriminator).
    fn slot_proto(&self, n: u32) -> Option<u64> {
        if self.slot_live(n) {
            Some(self.slots[n as usize].proto)
        } else {
            None
        }
    }

    fn slot_ref(&mut self, n: u32) {
        if self.slot_live(n) {
            self.slots[n as usize].refs += 1;
        }
    }

    /// Drop one reference to connection N. When the last reference goes, the
    /// connection is freed (the I-10/I-11 invariant: clunk is the only free
    /// path; N is not reusable until fully torn down). net-2c-2 removes the
    /// smoltcp socket here.
    fn slot_unref(&mut self, n: u32) {
        let i = n as usize;
        if i < MAX_SLOTS && self.slots[i].used && self.slots[i].refs > 0 {
            self.slots[i].refs -= 1;
            if self.slots[i].refs == 0 {
                // Last reference gone: free the connection AND its smoltcp
                // socket (the only free path -- I-10/I-11). remove() returns the
                // Socket, dropped here, releasing its rx/tx buffers.
                let proto = self.slots[i].proto;
                if let Some(h) = self.slots[i].socket.take() {
                    let _ = self.sockets.remove(h);
                }
                self.slots[i] = Slot::empty();
                self.dec_active(proto);
            }
        }
    }

    /// Decrement the per-protocol live-connection counter (the `active` stat).
    fn dec_active(&mut self, proto: u64) {
        match proto {
            PROTO_UDP => self.udp_active = self.udp_active.saturating_sub(1),
            _ => self.tcp_active = self.tcp_active.saturating_sub(1),
        }
    }

    /// Undo a just-minted clone whose Rlopen build failed: drop the opener's ref
    /// (frees the slot + removes its socket + decrements the active counter via
    /// slot_unref) AND uncount the mint, so a rolled-back clone does not inflate
    /// the `opened` stat. Reads the slot's protocol BEFORE slot_unref frees it.
    /// The ephemeral allocator is liveness-unchecked (49152..=65535 rotation); at
    /// MAX_SLOTS connections a post-wrap collision is astronomically unlikely (a
    /// liveness-checked allocator is a v1.x refinement).
    fn clone_rollback(&mut self, n: u32) {
        let proto = self.slot_proto(n);
        self.slot_unref(n);
        match proto {
            Some(PROTO_UDP) => self.udp_opened = self.udp_opened.saturating_sub(1),
            _ => self.tcp_opened = self.tcp_opened.saturating_sub(1),
        }
    }

    fn slot_socket(&self, n: u32) -> Option<SocketHandle> {
        if self.slot_live(n) {
            self.slots[n as usize].socket
        } else {
            None
        }
    }

    /// The `connect` ctl verb (section 3.3): dial `(ip, port)` on the connection.
    /// TCP active-opens (CONNECTING); UDP binds a local port + records the remote
    /// for subsequent `data` sends. Dispatched on the slot's protocol so the
    /// type-recovering socket access matches.
    fn ctl_connect(&mut self, n: u32, ip: [u8; 4], port: u16) -> Result<(), ()> {
        match self.slot_proto(n) {
            Some(PROTO_UDP) => self.udp_connect(n, ip, port),
            Some(PROTO_TCP) => self.tcp_connect(n, ip, port),
            _ => Err(()),
        }
    }

    /// The first IPv4 address bound on the interface, or None (pre-lease). UDP
    /// reports it as the connection's `local` address (the source the stack will
    /// select); TCP reads its selected source straight off the socket post-connect.
    fn iface_ipv4(&self) -> Option<[u8; 4]> {
        self.iface
            .ip_addrs()
            .iter()
            .find_map(|c| match c.address() {
                IpAddress::Ipv4(v4) => Some(v4.octets()),
                #[allow(unreachable_patterns)]
                _ => None,
            })
    }

    /// UDP `connect` (net-3b): bind a local ephemeral port (smoltcp requires a
    /// bound socket before send/recv) and record the remote for `data` sends. A
    /// datagram socket is connectionless, so this only fixes the default
    /// destination; a re-dial on an already-bound socket just updates the remote.
    fn udp_connect(&mut self, n: u32, ip: [u8; 4], port: u16) -> Result<(), ()> {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return Err(()),
        };
        if !self.sockets.get::<udp::Socket>(h).is_open() {
            let local_port = self.next_local_port;
            let ep = IpListenEndpoint {
                addr: None,
                port: local_port,
            };
            if self.sockets.get_mut::<udp::Socket>(h).bind(ep).is_err() {
                self.slots[n as usize].err = Some("udp bind rejected");
                return Err(());
            }
            self.next_local_port = ephemeral_after(local_port);
            let la = self.iface_ipv4().unwrap_or([0, 0, 0, 0]);
            self.slots[n as usize].local = Some((la, local_port));
        }
        self.slots[n as usize].remote = Some((ip, port));
        self.slots[n as usize].err = None;
        Ok(())
    }

    /// TCP `connect`: active-open the connection's socket to `(ip, port)`. Records
    /// the resolved local + remote endpoints in the slot, so `local`/`remote`
    /// report a stable tuple regardless of the live socket's later transitions.
    /// Err on a smoltcp connect rejection.
    fn tcp_connect(&mut self, n: u32, ip: [u8; 4], port: u16) -> Result<(), ()> {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return Err(()),
        };
        // Peek the next ephemeral port; commit the rotation only if connect
        // succeeds, so a rejected open (e.g. an already-open socket) does not
        // burn a port out of the 16k-wide pool.
        let local_port = self.next_local_port;
        let remote = IpEndpoint::new(IpAddress::v4(ip[0], ip[1], ip[2], ip[3]), port);
        let local = IpListenEndpoint {
            addr: None, // smoltcp selects the source address from the route
            port: local_port,
        };
        // Disjoint-field borrows: the socket (from self.sockets) and the iface
        // context (from self.iface) are separate fields of self.
        let r = self
            .sockets
            .get_mut::<tcp::Socket>(h)
            .connect(self.iface.context(), remote, local);
        match r {
            Ok(()) => {
                self.next_local_port = ephemeral_after(local_port);
                // connect() set the tuple synchronously: remote is what we
                // dialed; local is the selected source addr + our ephemeral port.
                let le = self.sockets.get::<tcp::Socket>(h).local_endpoint();
                let re = self.sockets.get::<tcp::Socket>(h).remote_endpoint();
                self.slots[n as usize].local = le.map(endpoint_octets);
                self.slots[n as usize].remote = re.map(endpoint_octets);
                self.slots[n as usize].err = None;
                Ok(())
            }
            Err(_) => {
                self.slots[n as usize].err = Some("connect rejected");
                Err(())
            }
        }
    }

    /// The `hangup` ctl verb: close the connection's socket. TCP drains + FINs;
    /// UDP just unbinds. The fid clunk later frees N and removes the socket.
    fn ctl_hangup(&mut self, n: u32) {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return,
        };
        match self.slot_proto(n) {
            Some(PROTO_UDP) => self.sockets.get_mut::<udp::Socket>(h).close(),
            _ => self.sockets.get_mut::<tcp::Socket>(h).close(),
        }
    }

    /// The `announce` ctl verb (NET-DESIGN 3.3): put the connection's socket into
    /// LISTEN on `ep`. The connection becomes ANNOUNCED, so `open(listen)` blocks
    /// for an inbound call. The endpoint is recorded so the listener re-arms a
    /// fresh socket after each accept (it stays ANNOUNCED for the next call).
    fn ctl_announce(&mut self, n: u32, ep: IpListenEndpoint) -> Result<(), ()> {
        // Only TCP accepts inbound connections; a UDP slot has no listen socket
        // (and get::<tcp::Socket> on a UDP handle would panic). UDP receive-from-
        // any (bind without a remote) is a net-3c+ refinement.
        if self.slot_proto(n) != Some(PROTO_TCP) {
            return Err(());
        }
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return Err(()),
        };
        match self.sockets.get_mut::<tcp::Socket>(h).listen(ep) {
            Ok(()) => {
                self.slots[n as usize].listen_ep = Some(ep);
                self.slots[n as usize].err = None;
                Ok(())
            }
            Err(_) => {
                // Already open/connected, or port 0: cannot listen.
                self.slots[n as usize].err = Some("announce rejected");
                Err(())
            }
        }
    }

    fn slot_listen_ep(&self, n: u32) -> Option<IpListenEndpoint> {
        if self.slot_live(n) {
            self.slots[n as usize].listen_ep
        } else {
            None
        }
    }

    pub fn has_pending_accepts(&self) -> bool {
        !self.pending.is_empty()
    }

    /// Register a deferred accept: the blocking `open(listen)` whose Rlopen is
    /// held until a call lands on `listening_n`. False if the table is full (the
    /// caller rejects the open with ENOMEM rather than deferring unboundedly).
    fn register_accept(&mut self, conn_id: i64, tag: u16, fid: u32, listening_n: u32) -> bool {
        if self.pending.len() >= MAX_PENDING_ACCEPTS {
            return false;
        }
        self.pending.push(PendingAccept {
            conn_id,
            tag,
            fid,
            listening_n,
        });
        true
    }

    /// Cancel a deferred accept abandoned by a Tflush (the client died on its
    /// blocked open): no late Rlopen, no connection minted for the dead op. The
    /// fid stays bound to /net/tcp/N/listen (refs N) until the session clunks it.
    fn cancel_accept_tag(&mut self, conn_id: i64, oldtag: u16) {
        self.pending
            .retain(|p| !(p.conn_id == conn_id && p.tag == oldtag));
    }

    /// Drop every deferred accept a closing connection held (its listen fids go
    /// away with it): called from Conn teardown + the Tversion session reset.
    fn cancel_accepts_for_conn(&mut self, conn_id: i64) {
        self.pending.retain(|p| p.conn_id != conn_id);
    }

    /// A call landed on listener N: mint a NEW connection M that takes N's now-
    /// established socket, and re-arm N with a fresh listening socket on its
    /// announced endpoint (N stays ANNOUNCED for the next call). M is born
    /// refs==0; the caller refs it immediately by rebinding the listen fid onto
    /// M's ctl. None if the slot table is full (the call stays buffered in N's
    /// socket) or the re-arm fails (N keeps the call; the accept retries).
    fn accept_swap(&mut self, n: u32) -> Option<u32> {
        let m = self.slots.iter().position(|s| !s.used)?;
        let established = self.slots[n as usize].socket?;
        let ep = self.slots[n as usize].listen_ep?;
        let rx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_RX_BUF]);
        let tx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_TX_BUF]);
        let fresh = self.sockets.add(tcp::Socket::new(rx, tx));
        if self
            .sockets
            .get_mut::<tcp::Socket>(fresh)
            .listen(ep)
            .is_err()
        {
            // Re-arm failed: drop the fresh socket, leave N holding the call so
            // the accept retries next poll (no socket leaked; N still active).
            let _ = self.sockets.remove(fresh);
            return None;
        }
        let le = self
            .sockets
            .get::<tcp::Socket>(established)
            .local_endpoint();
        let re = self
            .sockets
            .get::<tcp::Socket>(established)
            .remote_endpoint();
        // N takes the fresh listener; M takes the established socket. Accept is
        // TCP-only (UDP never announces), so M is a TCP slot.
        self.slots[n as usize].socket = Some(fresh);
        self.slots[m] = Slot {
            used: true,
            refs: 0,
            proto: PROTO_TCP,
            socket: Some(established),
            local: le.map(endpoint_octets),
            remote: re.map(endpoint_octets),
            err: None,
            listen_ep: None,
        };
        self.tcp_active += 1;
        self.tcp_opened += 1;
        Some(m as u32)
    }

    /// Scan the deferred accepts; for each whose listener has established an
    /// inbound call, mint the accepted connection (accept_swap) and emit an
    /// AcceptDone for the serve loop to deliver. Removes the completed entries.
    pub fn poll_accepts(&mut self) -> Vec<AcceptDone> {
        let mut done: Vec<AcceptDone> = Vec::new();
        let mut i = 0;
        while i < self.pending.len() {
            let pa = self.pending[i];
            let ready = match self.slot_socket(pa.listening_n) {
                Some(h) => accept_ready(self.sockets.get::<tcp::Socket>(h)),
                None => {
                    // The listener vanished (should not happen: the listen fid
                    // refs it). Drop defensively so it cannot spin.
                    self.pending.remove(i);
                    continue;
                }
            };
            if !ready {
                i += 1;
                continue;
            }
            match self.accept_swap(pa.listening_n) {
                Some(m) => {
                    done.push(AcceptDone {
                        conn_id: pa.conn_id,
                        tag: pa.tag,
                        fid: pa.fid,
                        new_n: m,
                        ctl_qid_path: make_conn(PROTO_TCP, m, FK_CTL),
                    });
                    self.pending.remove(i);
                    // do not advance i: remove() shifted the next entry into i.
                }
                None => i += 1, // slot table full: leave pending, retry next poll
            }
        }
        done
    }

    /// Free a just-minted accepted connection whose delivery could not complete
    /// (its 9P connection vanished, or the listen fid was clunked mid-accept).
    /// Only frees a slot still at refs==0 (an unowned mint) -- a slot the listen
    /// fid already rebound onto is owned and must not be freed here.
    fn free_orphan_mint(&mut self, n: u32) {
        let i = n as usize;
        if i < MAX_SLOTS && self.slots[i].used && self.slots[i].refs == 0 {
            let proto = self.slots[i].proto;
            if let Some(h) = self.slots[i].socket.take() {
                let _ = self.sockets.remove(h);
            }
            self.slots[i] = Slot::empty();
            self.dec_active(proto);
        }
    }

    /// Discard a completed accept the serve loop could not deliver (its 9P
    /// connection had already closed): free the unowned minted connection.
    pub fn discard_accept(&mut self, d: AcceptDone) {
        self.free_orphan_mint(d.new_n);
    }

    /// Write `data` to the connection. TCP enqueues onto the byte stream
    /// (returns bytes accepted). UDP sends one datagram to the recorded remote
    /// (all-or-nothing: returns data.len() on success, 0 if it could not send).
    /// Non-blocking.
    fn data_send(&mut self, n: u32, data: &[u8]) -> usize {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return 0,
        };
        match self.slot_proto(n) {
            Some(PROTO_UDP) => {
                let (ip, port) = match self.slots[n as usize].remote {
                    Some(r) => r,
                    None => return 0, // no destination: `connect` first
                };
                let remote = IpEndpoint::new(IpAddress::v4(ip[0], ip[1], ip[2], ip[3]), port);
                match self
                    .sockets
                    .get_mut::<udp::Socket>(h)
                    .send_slice(data, remote)
                {
                    Ok(()) => data.len(),
                    Err(_) => {
                        self.slots[n as usize].err = Some("udp send failed");
                        0
                    }
                }
            }
            _ => self
                .sockets
                .get_mut::<tcp::Socket>(h)
                .send_slice(data)
                .unwrap_or(0),
        }
    }

    /// Read up to `out.len()` bytes from the connection. TCP dequeues from the
    /// byte stream; UDP dequeues one whole datagram (the sender endpoint is
    /// dropped at net-3b -- the connected client knows its remote; receive-from-
    /// any with sender attribution is net-3c+). Returns bytes dequeued (0 if none
    /// available / closed). Non-blocking: a 0-return is ambiguous between "no
    /// data yet" and EOF -- proper readiness/blocking is the dev9p.poll leg (net-6).
    fn data_recv(&mut self, n: u32, out: &mut [u8]) -> usize {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return 0,
        };
        match self.slot_proto(n) {
            Some(PROTO_UDP) => match self.sockets.get_mut::<udp::Socket>(h).recv_slice(out) {
                Ok((k, _meta)) => k,
                Err(_) => 0,
            },
            _ => self
                .sockets
                .get_mut::<tcp::Socket>(h)
                .recv_slice(out)
                .unwrap_or(0),
        }
    }

    /// The live state of a connection (the `status` file). TCP reports its TCP
    /// state machine; UDP reports Open (bound) / Closed (a datagram socket has no
    /// connection state).
    fn state_str(&self, n: u32) -> &'static str {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return "Closed",
        };
        if self.slot_proto(n) == Some(PROTO_UDP) {
            return if self.sockets.get::<udp::Socket>(h).is_open() {
                "Open"
            } else {
                "Closed"
            };
        }
        match self.sockets.get::<tcp::Socket>(h).state() {
            tcp::State::Closed => "Closed",
            tcp::State::Listen => "Listen",
            tcp::State::SynSent => "Syn-Sent",
            tcp::State::SynReceived => "Syn-Received",
            tcp::State::Established => "Established",
            tcp::State::FinWait1 => "Fin-Wait-1",
            tcp::State::FinWait2 => "Fin-Wait-2",
            tcp::State::CloseWait => "Close-Wait",
            tcp::State::Closing => "Closing",
            tcp::State::LastAck => "Last-Ack",
            tcp::State::TimeWait => "Time-Wait",
        }
    }

    fn slot_endpoint(&self, n: u32, local: bool) -> Option<([u8; 4], u16)> {
        if !self.slot_live(n) {
            return None;
        }
        if local {
            self.slots[n as usize].local
        } else {
            self.slots[n as usize].remote
        }
    }

    fn slot_err(&self, n: u32) -> Option<&'static str> {
        if self.slot_live(n) {
            self.slots[n as usize].err
        } else {
            None
        }
    }

    fn is_dir(&self, path: u64) -> bool {
        if is_conn(path) {
            conn_filekind(path) == FK_DIR
        } else {
            matches!(path, P_ROOT | P_TCP | P_UDP | P_ICMP)
        }
    }

    fn parent_of(&self, path: u64) -> u64 {
        if is_conn(path) {
            if conn_filekind(path) == FK_DIR {
                proto_dir(conn_proto(path))
            } else {
                make_conn(conn_proto(path), conn_n(path), FK_DIR)
            }
        } else {
            match path {
                P_TCP | P_UDP | P_ICMP => P_ROOT,
                P_TCP_CLONE | P_TCP_STATS => P_TCP,
                P_UDP_CLONE | P_UDP_STATS => P_UDP,
                P_ICMP_STATS => P_ICMP,
                _ => P_ROOT, // P_ROOT is its own parent (the `..`-from-root fixpoint)
            }
        }
    }

    /// Resolve one walk component from `dir` (which must be a directory) to a
    /// child path. None == ENOENT. A numeric name under /net/tcp resolves to a
    /// connection directory only if that slot is live.
    fn walk_child(&self, dir: u64, name: &[u8]) -> Option<u64> {
        if !self.is_dir(dir) {
            return None;
        }
        if name == b"." {
            return Some(dir);
        }
        if name == b".." {
            return Some(self.parent_of(dir));
        }
        if is_conn(dir) {
            // Inside /net/<proto>/N/: the fixed file set. `listen` is TCP-only
            // (UDP has no accept), so it is unresolvable under a UDP conn dir.
            let (proto, n) = (conn_proto(dir), conn_n(dir));
            let fk = match name {
                b"ctl" => FK_CTL,
                b"data" => FK_DATA,
                b"local" => FK_LOCAL,
                b"remote" => FK_REMOTE,
                b"status" => FK_STATUS,
                b"err" => FK_ERR,
                b"listen" if proto == PROTO_TCP => FK_LISTEN,
                _ => return None,
            };
            return Some(make_conn(proto, n, fk));
        }
        match dir {
            P_ROOT => match name {
                b"tcp" => Some(P_TCP),
                b"udp" => Some(P_UDP),
                b"icmp" => Some(P_ICMP),
                _ => None,
            },
            P_TCP => match name {
                b"clone" => Some(P_TCP_CLONE),
                b"stats" => Some(P_TCP_STATS),
                // A numeric name resolves only if that slot is a live TCP conn.
                _ => parse_dec(name)
                    .filter(|&n| self.slot_proto(n) == Some(PROTO_TCP))
                    .map(|n| make_conn(PROTO_TCP, n, FK_DIR)),
            },
            P_UDP => match name {
                b"clone" => Some(P_UDP_CLONE),
                b"stats" => Some(P_UDP_STATS),
                _ => parse_dec(name)
                    .filter(|&n| self.slot_proto(n) == Some(PROTO_UDP))
                    .map(|n| make_conn(PROTO_UDP, n, FK_DIR)),
            },
            P_ICMP => match name {
                b"stats" => Some(P_ICMP_STATS),
                _ => None,
            },
            _ => None,
        }
    }

    /// Enumerate `dir`'s children, calling `push(name, qid_path, is_dir)` for
    /// each in a stable order. The order is fixed (the static names first, then
    /// live slots ascending), so the Treaddir resume cookie (an ordinal) is
    /// stable across a paginated read as long as no slot is freed mid-walk.
    fn for_each_child<F: FnMut(&[u8], u64, bool)>(&self, dir: u64, mut push: F) {
        if is_conn(dir) && conn_filekind(dir) == FK_DIR {
            let (proto, n) = (conn_proto(dir), conn_n(dir));
            push(b"ctl", make_conn(proto, n, FK_CTL), false);
            push(b"data", make_conn(proto, n, FK_DATA), false);
            // `listen` (the accept file) exists only on a TCP conn dir.
            if proto == PROTO_TCP {
                push(b"listen", make_conn(proto, n, FK_LISTEN), false);
            }
            push(b"local", make_conn(proto, n, FK_LOCAL), false);
            push(b"remote", make_conn(proto, n, FK_REMOTE), false);
            push(b"status", make_conn(proto, n, FK_STATUS), false);
            push(b"err", make_conn(proto, n, FK_ERR), false);
            return;
        }
        match dir {
            P_ROOT => {
                push(b"tcp", P_TCP, true);
                push(b"udp", P_UDP, true);
                push(b"icmp", P_ICMP, true);
            }
            P_TCP => {
                push(b"clone", P_TCP_CLONE, false);
                push(b"stats", P_TCP_STATS, false);
                self.push_conn_slots(PROTO_TCP, &mut push);
            }
            P_UDP => {
                push(b"clone", P_UDP_CLONE, false);
                push(b"stats", P_UDP_STATS, false);
                self.push_conn_slots(PROTO_UDP, &mut push);
            }
            P_ICMP => push(b"stats", P_ICMP_STATS, false),
            _ => {}
        }
    }

    /// Push the live connection-directory entries for one protocol (ascending
    /// slot index, a stable order for the readdir resume cookie).
    fn push_conn_slots<F: FnMut(&[u8], u64, bool)>(&self, proto: u64, push: &mut F) {
        for i in 0..MAX_SLOTS {
            if self.slots[i].used && self.slots[i].proto == proto {
                let mut name = DecName::new();
                name.set(i as u32);
                push(name.as_slice(), make_conn(proto, i as u32, FK_DIR), true);
            }
        }
    }

    /// The byte content of a readable file node. Connection files other than
    /// `ctl`/`status` are empty at net-2c-1 (the live socket lands at net-2c-2).
    fn file_content(&self, path: u64) -> Content {
        let mut c = Content::new();
        if is_conn(path) {
            let n = conn_n(path);
            match conn_filekind(path) {
                FK_CTL => c.push_dec(n),
                FK_STATUS => c.push(self.state_str(n).as_bytes()),
                FK_LOCAL => {
                    if let Some((ip, port)) = self.slot_endpoint(n, true) {
                        c.push_endpoint(ip, port);
                    }
                }
                FK_REMOTE => {
                    if let Some((ip, port)) = self.slot_endpoint(n, false) {
                        c.push_endpoint(ip, port);
                    }
                }
                FK_ERR => {
                    if let Some(e) = self.slot_err(n) {
                        c.push(e.as_bytes());
                    }
                }
                // FK_DATA is the live recv stream, served by h_read (not here).
                _ => {}
            }
            return c;
        }
        match path {
            P_TCP_STATS => {
                c.push(b"tcp\n  active ");
                c.push_dec(self.tcp_active);
                c.push(b"\n  opened ");
                c.push_dec(self.tcp_opened);
                c.push(b"\n");
            }
            P_UDP_STATS => {
                c.push(b"udp\n  active ");
                c.push_dec(self.udp_active);
                c.push(b"\n  opened ");
                c.push_dec(self.udp_opened);
                c.push(b"\n");
            }
            P_ICMP_STATS => c.push(b"icmp\n  echo 0\n"),
            // clone is never read as content (its open rebinds the fid to ctl).
            _ => {}
        }
        c
    }
}

/// Parse a non-empty all-ASCII-decimal name into a connection number, bounded
/// to the slot range. None on any non-digit, empty, or out-of-range name.
fn parse_dec(name: &[u8]) -> Option<u32> {
    if name.is_empty() || name.len() > 8 {
        return None;
    }
    let mut v: u32 = 0;
    for &b in name {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((b - b'0') as u32)?;
    }
    if (v as usize) < MAX_SLOTS {
        Some(v)
    } else {
        None
    }
}

/// Extract (octets, port) from a smoltcp IPv4 endpoint. A non-v4 address ->
/// 0.0.0.0 (unreachable: the iface is proto-ipv4 only).
fn endpoint_octets(e: IpEndpoint) -> ([u8; 4], u16) {
    match e.addr {
        IpAddress::Ipv4(v4) => (v4.octets(), e.port),
        #[allow(unreachable_patterns)]
        _ => ([0, 0, 0, 0], e.port),
    }
}

/// Parse a Plan 9 dial string `a.b.c.d!port` (ignoring a trailing `!r` or a
/// pinned-local suffix) into (octets, port). None on any malformation.
fn parse_dial(arg: &[u8]) -> Option<([u8; 4], u16)> {
    let bang = arg.iter().position(|&b| b == b'!')?;
    let ip = parse_ipv4(&arg[..bang])?;
    let rest = &arg[bang + 1..];
    let pend = rest.iter().position(|&b| b == b'!').unwrap_or(rest.len());
    let port = parse_u16(&rest[..pend])?;
    if port == 0 {
        return None;
    }
    Some((ip, port))
}

fn parse_ipv4(s: &[u8]) -> Option<[u8; 4]> {
    let mut octs = [0u8; 4];
    let mut idx = 0usize;
    for part in s.split(|&b| b == b'.') {
        if idx >= 4 {
            return None;
        }
        octs[idx] = parse_u8(part)?;
        idx += 1;
    }
    if idx == 4 {
        Some(octs)
    } else {
        None
    }
}

fn parse_u8(s: &[u8]) -> Option<u8> {
    if s.is_empty() || s.len() > 3 {
        return None;
    }
    let mut v: u32 = 0;
    for &b in s {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as u32;
    }
    u8::try_from(v).ok()
}

fn parse_u16(s: &[u8]) -> Option<u16> {
    if s.is_empty() || s.len() > 5 {
        return None;
    }
    let mut v: u32 = 0;
    for &b in s {
        if !b.is_ascii_digit() {
            return None;
        }
        v = v * 10 + (b - b'0') as u32;
    }
    u16::try_from(v).ok()
}

/// The max dirent-stream byte budget for an Rreaddir reply: bounded by the
/// client's `count` AND by `msize` minus the Rreaddir frame overhead
/// (`P9_HDR_LEN` + the 4-byte `count` field), so the built reply NEVER exceeds
/// the negotiated msize. Parity with `h_read`'s data cap (the data path already
/// reserves this overhead via the same `saturating_sub`); the directory path
/// must too -- otherwise a populated directory read by a small-msize client
/// yields an Rreaddir frame larger than the client negotiated.
fn rreaddir_budget(count: u32, msize: u32) -> usize {
    (count as usize).min((msize as usize).saturating_sub(p9::P9_HDR_LEN + 4))
}

/// The next rotating ephemeral local port after `p` (49152..=65535 wrap).
fn ephemeral_after(p: u16) -> u16 {
    if p == u16::MAX {
        EPHEMERAL_LO
    } else {
        p + 1
    }
}

/// "An inbound call has landed and the handshake completed": the listening
/// socket left LISTEN for an active connection past the SYN exchange (so its
/// local/remote endpoints are set and `data` is immediately usable). SynSent/
/// SynReceived are excluded; is_active() already excludes Closed/TimeWait/Listen.
fn accept_ready(s: &tcp::Socket) -> bool {
    s.is_active() && !matches!(s.state(), tcp::State::SynSent | tcp::State::SynReceived)
}

/// Parse a Plan 9 announce string `addr!port`, where `addr` is `*` (any local
/// address) or a dotted IPv4, into a smoltcp listen endpoint. None on any
/// malformation or a zero port (a listener needs a concrete port).
fn parse_announce(arg: &[u8]) -> Option<IpListenEndpoint> {
    let bang = arg.iter().position(|&b| b == b'!')?;
    let addr_s = &arg[..bang];
    let rest = &arg[bang + 1..];
    let pend = rest.iter().position(|&b| b == b'!').unwrap_or(rest.len());
    let port = parse_u16(&rest[..pend])?;
    if port == 0 {
        return None;
    }
    let addr = if addr_s == b"*" {
        None
    } else {
        let ip = parse_ipv4(addr_s)?;
        Some(IpAddress::v4(ip[0], ip[1], ip[2], ip[3]))
    };
    Some(IpListenEndpoint { addr, port })
}

/// Build a minimal DNS A-record query (RFC 1035) for `qname` (dotted, e.g.
/// b"example.com"): a 12-byte header (id + RD flag + qdcount=1) followed by the
/// question (length-prefixed labels + qtype=A + qclass=IN). Used only by the
/// net-3b boot demo.
fn build_dns_query(id: u16, qname: &[u8]) -> Vec<u8> {
    let mut q = Vec::new();
    q.extend_from_slice(&id.to_be_bytes());
    q.extend_from_slice(&0x0100u16.to_be_bytes()); // flags: RD (recursion desired)
    q.extend_from_slice(&1u16.to_be_bytes()); // qdcount
    q.extend_from_slice(&0u16.to_be_bytes()); // ancount
    q.extend_from_slice(&0u16.to_be_bytes()); // nscount
    q.extend_from_slice(&0u16.to_be_bytes()); // arcount
    for label in qname.split(|&b| b == b'.') {
        q.push(label.len() as u8);
        q.extend_from_slice(label);
    }
    q.push(0); // the root label terminates the name
    q.extend_from_slice(&1u16.to_be_bytes()); // qtype A
    q.extend_from_slice(&1u16.to_be_bytes()); // qclass IN
    q
}

// A small fixed-cap content buffer: file bodies are short (a number, a few-line
// stats block, a status word), so reads never allocate.
struct Content {
    buf: [u8; 128],
    len: usize,
}

impl Content {
    fn new() -> Content {
        Content {
            buf: [0; 128],
            len: 0,
        }
    }
    fn as_slice(&self) -> &[u8] {
        &self.buf[..self.len]
    }
    fn push(&mut self, b: &[u8]) {
        let n = b.len().min(self.buf.len() - self.len);
        self.buf[self.len..self.len + n].copy_from_slice(&b[..n]);
        self.len += n;
    }
    fn push_dec(&mut self, v: u32) {
        let mut d = DecName::new();
        d.set(v);
        self.push(d.as_slice());
    }
    /// Format `ip!port` (the Plan 9 dial notation the `local`/`remote` files use).
    fn push_endpoint(&mut self, ip: [u8; 4], port: u16) {
        self.push_dec(ip[0] as u32);
        self.push(b".");
        self.push_dec(ip[1] as u32);
        self.push(b".");
        self.push_dec(ip[2] as u32);
        self.push(b".");
        self.push_dec(ip[3] as u32);
        self.push(b"!");
        self.push_dec(port as u32);
    }
}

// Decimal formatter for a u32 into a stack buffer (a connection number / slot
// index), no allocation.
struct DecName {
    buf: [u8; 10],
    start: usize,
}

impl DecName {
    fn new() -> DecName {
        DecName {
            buf: [0; 10],
            start: 10,
        }
    }
    fn set(&mut self, mut v: u32) {
        self.start = self.buf.len();
        loop {
            self.start -= 1;
            self.buf[self.start] = b'0' + (v % 10) as u8;
            v /= 10;
            if v == 0 {
                break;
            }
        }
    }
    fn as_slice(&self) -> &[u8] {
        &self.buf[self.start..]
    }
}

/// Post the /net 9P service (9P-mode) into the boot namespace's /srv. Returns
/// the listener handle. The boot /srv is the immortal registry joey re-grafts
/// across the pivot, so a service posted here is reachable by joey for the
/// mount. perm=0 = 9P-mode (vs DMSRVBYTE byte-mode). Err(()) on a post failure
/// (most likely a missing MAY_POST_SERVICE).
pub fn post_srv_net() -> Result<i64, ()> {
    // O_PATH = a navigation base (9P forbids create from an opened fid).
    let srv = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/srv".as_ptr(), 4, T_OPATH) };
    if srv < 0 {
        return Err(());
    }
    let listener = unsafe { t_walk_create(srv, b"net".as_ptr(), 3, T_OREAD, 0) };
    let _ = unsafe { t_close(srv) };
    if listener < 0 {
        return Err(());
    }
    Ok(listener)
}

#[derive(Copy, Clone)]
struct Fid {
    fid: u32,
    path: u64,
    opened: bool,
}

/// The outcome of dispatching one request frame.
enum Disp {
    /// A complete reply of `len` bytes is in out_buf; send it.
    Reply(usize),
    /// The reply is HELD (a blocking listen open): emit nothing now; the Rlopen
    /// is sent later by complete_accept when the inbound call lands.
    Deferred,
    /// Unrecoverable build failure: tear the connection down.
    Fatal,
}

/// One accepted 9P connection. Owns its fid table + framing buffers. The fid
/// lifecycle is refcounted against the shared `Net`: binding a fid into a
/// connection's subtree refs the slot, clunking/rebinding-away unrefs it.
pub struct Conn {
    handle: i64,
    version_done: bool,
    msize: u32,
    fids: [Option<Fid>; MAX_FIDS],
    in_buf: Vec<u8>,
    out_buf: Vec<u8>,
    // Set by a handler that holds its reply (a blocking listen open); read +
    // cleared by dispatch to return Disp::Deferred.
    defer: bool,
}

impl Conn {
    pub fn new(handle: i64) -> Conn {
        Conn {
            handle,
            version_done: false,
            msize: SRV_MSIZE,
            fids: [None; MAX_FIDS],
            in_buf: Vec::new(),
            out_buf: Vec::new(),
            defer: false,
        }
    }

    pub fn handle(&self) -> i64 {
        self.handle
    }

    /// Drop all this connection's references before the connection is closed
    /// (the serve() loop calls this before removing a dead Conn), so a session
    /// teardown frees any connections only this session held open.
    pub fn teardown(&mut self, net: &mut Net) {
        // Drop any deferred accepts this session held (their held Rlopens die
        // with the connection) before unref'ing its fids.
        net.cancel_accepts_for_conn(self.handle);
        for slot in self.fids.iter_mut() {
            if let Some(f) = slot.take() {
                if let Some(n) = path_conn_n(f.path) {
                    net.slot_unref(n);
                }
            }
        }
    }

    fn fid_find(&self, fid: u32) -> Option<usize> {
        self.fids
            .iter()
            .position(|f| matches!(f, Some(e) if e.fid == fid))
    }

    /// Bind `fid` -> `path`. Adjusts connection refs: refs the NEW connection
    /// first, then unrefs the OLD (so a within-connection rebind never transits
    /// refs==0 and frees the slot out from under the fid). Returns false only if
    /// the fid table is full (a fresh bind with no free slot).
    fn fid_set(&mut self, net: &mut Net, fid: u32, path: u64, opened: bool) -> bool {
        if let Some(i) = self.fid_find(fid) {
            let old = self.fids[i].unwrap().path;
            if let Some(n) = path_conn_n(path) {
                net.slot_ref(n);
            }
            self.fids[i] = Some(Fid { fid, path, opened });
            if let Some(n) = path_conn_n(old) {
                net.slot_unref(n);
            }
            return true;
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            if let Some(n) = path_conn_n(path) {
                net.slot_ref(n);
            }
            self.fids[i] = Some(Fid { fid, path, opened });
            return true;
        }
        false
    }

    fn fid_clunk(&mut self, net: &mut Net, fid: u32) -> bool {
        if let Some(i) = self.fid_find(fid) {
            let f = self.fids[i].take().unwrap();
            if let Some(n) = path_conn_n(f.path) {
                net.slot_unref(n);
            }
            return true;
        }
        false
    }

    /// Read available bytes and dispatch every COMPLETE 9P frame. Returns false
    /// to close the connection (EOF, framing violation, or write failure). One
    /// `t_read` per call (the caller re-enters via the poll loop), so a partial
    /// frame waits for the next readable event rather than blocking mid-frame.
    pub fn service(&mut self, net: &mut Net) -> bool {
        let cur = self.in_buf.len();
        if cur >= SRV_MSIZE_USIZE {
            // A full msize buffered with no complete frame -> oversized/malformed.
            return false;
        }
        let want = SRV_MSIZE_USIZE - cur;
        self.in_buf.resize(cur + want, 0);
        let n =
            unsafe { libthyla_rs::t_read(self.handle, self.in_buf.as_mut_ptr().add(cur), want) };
        if n <= 0 {
            self.in_buf.truncate(cur);
            return false; // EOF (0) or error (<0): tear the connection down.
        }
        self.in_buf.truncate(cur + n as usize);

        loop {
            if self.in_buf.len() < p9::P9_HDR_LEN {
                return true; // need more header bytes
            }
            let hdr = match p9::peek_header(&self.in_buf) {
                Ok(h) => h,
                Err(_) => return false,
            };
            let size = hdr.size as usize;
            if !(p9::P9_HDR_LEN..=SRV_MSIZE_USIZE).contains(&size) {
                return false; // framing violation
            }
            if self.in_buf.len() < size {
                return true; // incomplete frame; wait for more
            }
            let frame: Vec<u8> = self.in_buf[..size].to_vec();
            match self.dispatch(net, &frame, hdr) {
                Disp::Fatal => return false, // unrecoverable build failure
                Disp::Deferred => {}         // reply held (a blocking listen open)
                Disp::Reply(rlen) => {
                    if !self.send_all(rlen) {
                        return false;
                    }
                }
            }
            self.in_buf.drain(..size);
        }
    }

    fn dispatch(&mut self, net: &mut Net, tmsg: &[u8], hdr: p9::Header) -> Disp {
        let tag = hdr.tag;
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        let r = match hdr.mtype {
            p9::P9_TVERSION => self.h_version(net, tmsg, tag),
            p9::P9_TATTACH => self.h_attach(tmsg, tag),
            p9::P9_TWALK => self.h_walk(net, tmsg, tag),
            p9::P9_TLOPEN => self.h_lopen(net, tmsg, tag),
            p9::P9_TREAD => self.h_read(net, tmsg, tag),
            p9::P9_TWRITE => self.h_write(net, tmsg, tag),
            p9::P9_TREADDIR => self.h_readdir(net, tmsg, tag),
            p9::P9_TGETATTR => self.h_getattr(net, tmsg, tag),
            p9::P9_TCLUNK => self.h_clunk(net, tmsg, tag),
            p9::P9_TFLUSH => self.h_flush(net, tmsg, tag),
            // Tauth/Tsetattr/... are unused on the /net tree (read-only metadata).
            _ => self.err(tag, p9::E_NOSYS),
        };
        // A blocking listen open held its reply (NET-DESIGN 3.4): emit nothing
        // now; the Rlopen is sent later when the inbound call lands.
        if self.defer {
            self.defer = false;
            return Disp::Deferred;
        }
        let len = r.unwrap_or_else(|_| {
            // A build/parse error mid-reply: re-clear (a partial build may have
            // written into out_buf) and emit Rlerror(EPROTO).
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        });
        if len == 0 {
            Disp::Fatal
        } else {
            Disp::Reply(len)
        }
    }

    /// Write `rlen` bytes from out_buf to the 9P connection. False on a write
    /// failure (the session is dead -> tear the connection down). Used by both
    /// the inline reply path and the deferred-accept Rlopen.
    fn send_all(&mut self, rlen: usize) -> bool {
        let mut sent = 0usize;
        while sent < rlen {
            let w = unsafe {
                libthyla_rs::t_write(self.handle, self.out_buf.as_ptr().add(sent), rlen - sent)
            };
            if w <= 0 {
                return false;
            }
            sent += w as usize;
        }
        true
    }

    /// Deliver a completed accept (called from the serve loop): rebind the
    /// blocked listen fid onto the accepted connection's ctl -- the refcount
    /// moves from the listener to the new connection -- and send the held
    /// Rlopen, unblocking the client's open(). False on a write failure (tear
    /// the connection down).
    pub fn complete_accept(&mut self, net: &mut Net, d: AcceptDone) -> bool {
        // The listen fid must still exist: it is bound to /net/tcp/N/listen and
        // refs the listener, so a clunk cannot remove it while the accept is
        // pending, and a Tversion reset would have cancelled the pending entry.
        // If it somehow vanished, the mint is unowned -> free it.
        if self.fid_find(d.fid).is_none() {
            net.free_orphan_mint(d.new_n);
            return true;
        }
        // Rebind onto M's ctl (refs M, unrefs the listener). The fid exists, so
        // fid_set takes the existing-slot path and cannot fail.
        let _ = self.fid_set(net, d.fid, d.ctl_qid_path, true);
        let q = p9::Qid {
            kind: p9::P9_QTFILE,
            version: 0,
            path: d.ctl_qid_path,
        };
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rlopen(&mut self.out_buf, d.tag, &q, 0) {
            Ok(rlen) => self.send_all(rlen),
            Err(()) => false,
        }
    }

    fn err(&mut self, tag: u16, code: u32) -> Result<usize, ()> {
        p9::build_rlerror(&mut self.out_buf, tag, code)
    }

    fn qid_of(&self, net: &Net, path: u64) -> p9::Qid {
        p9::Qid {
            kind: if net.is_dir(path) {
                p9::P9_QTDIR
            } else {
                p9::P9_QTFILE
            },
            version: 0,
            path,
        }
    }

    fn h_version(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tversion(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let negotiated = a.msize.min(SRV_MSIZE);
        // Tversion resets all session state (the 9P "clunk every fid"
        // semantics) -- drop every fid's connection ref first.
        self.drop_all_fids(net);
        self.msize = negotiated;
        let ver: &[u8] = if a.version == P9_VERSION_9P2000_L {
            self.version_done = true;
            P9_VERSION_9P2000_L
        } else {
            self.version_done = false;
            b"unknown"
        };
        p9::build_rversion(&mut self.out_buf, tag, negotiated, ver)
    }

    fn drop_all_fids(&mut self, net: &mut Net) {
        // A Tversion resets all session state -- abandon any deferred accepts
        // this connection held (their held Rlopens are dropped) too.
        net.cancel_accepts_for_conn(self.handle);
        for slot in self.fids.iter_mut() {
            if let Some(f) = slot.take() {
                if let Some(n) = path_conn_n(f.path) {
                    net.slot_unref(n);
                }
            }
        }
    }

    fn h_attach(&mut self, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        if !self.version_done {
            return self.err(tag, p9::E_PROTO);
        }
        let a = match p9::parse_tattach(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if a.afid != p9::P9_NOFID {
            return self.err(tag, p9::E_OPNOTSUPP); // no auth fid (trusted local transport)
        }
        if a.fid == p9::P9_NOFID {
            // NOFID is the 9P "no fid" sentinel, never a live fid -- binding it
            // would let a later op address it as a real fid.
            return self.err(tag, p9::E_INVAL);
        }
        // The root is a static node (no connection ref); bind directly.
        if self.fid_find(a.fid).is_some() {
            return self.err(tag, p9::E_INVAL);
        }
        if let Some(i) = self.fids.iter().position(|f| f.is_none()) {
            self.fids[i] = Some(Fid {
                fid: a.fid,
                path: P_ROOT,
                opened: false,
            });
        } else {
            return self.err(tag, p9::E_NOMEM);
        }
        let q = p9::Qid {
            kind: p9::P9_QTDIR,
            version: 0,
            path: P_ROOT,
        };
        p9::build_rattach(&mut self.out_buf, tag, &q)
    }

    fn h_walk(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twalk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let src = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let src_fid = self.fids[src].unwrap();
        if src_fid.opened {
            return self.err(tag, p9::E_PROTO); // 9P forbids walking from an opened fid
        }
        if a.newfid == p9::P9_NOFID {
            return self.err(tag, p9::E_INVAL); // newfid must be a real fid to bind
        }
        if a.newfid != a.fid && self.fid_find(a.newfid).is_some() {
            return self.err(tag, p9::E_INVAL); // newfid already in use
        }

        let mut cur = src_fid.path;
        let mut qids: [p9::Qid; p9::P9_MAX_WALK] = [p9::Qid::default(); p9::P9_MAX_WALK];
        let mut nwalked = 0usize;
        for i in 0..(a.nwname as usize) {
            match net.walk_child(cur, a.names[i]) {
                Some(next) => {
                    cur = next;
                    qids[nwalked] = self.qid_of(net, next);
                    nwalked += 1;
                }
                None => break,
            }
        }
        // First component failed -> ENOENT (no partial fid established).
        if a.nwname > 0 && nwalked == 0 {
            return self.err(tag, p9::E_NOENT);
        }
        // Per 9P: newfid binds to the last walked element ONLY on a full walk
        // (nwqid == nwname). A partial walk leaves newfid untouched. nwname==0
        // is a clone (newfid -> the same node as fid). The refcount moves with
        // the bind (fid_set refs the new connection / unrefs the old).
        if nwalked == a.nwname as usize && !self.fid_set(net, a.newfid, cur, false) {
            return self.err(tag, p9::E_NOMEM);
        }
        p9::build_rwalk(&mut self.out_buf, tag, &qids[..nwalked])
    }

    fn h_lopen(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tlopen(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        let _ = a.flags; // open is non-blocking EXCEPT listen (a deferred reply)

        // The server accept file: opening it is a BLOCKING accept. Register a
        // deferred 9P reply (NET-DESIGN 3.4) -- hold the Rlopen until an inbound
        // call lands on this connection's listening socket. The client's open()
        // stays blocked on the outstanding tag; complete_accept rebinds this fid
        // onto the accepted connection's ctl and sends the held Rlopen. The
        // connection must be ANNOUNCED (its socket put into LISTEN by `announce`).
        if is_conn(f.path) && conn_filekind(f.path) == FK_LISTEN {
            let n = conn_n(f.path);
            if net.slot_listen_ep(n).is_none() {
                return self.err(tag, p9::E_INVAL); // not announced -> cannot listen
            }
            if !net.register_accept(self.handle, tag, a.fid, n) {
                return self.err(tag, p9::E_NOMEM); // too many deferred accepts
            }
            self.defer = true; // dispatch emits no reply for this frame
            return Ok(0); // the 0 is ignored (defer is set)
        }

        // The Plan 9 clone idiom: opening `clone` (TCP or UDP) MINTS a connection
        // and rebinds THIS fid onto the new connection's `ctl` (the kernel dev9p
        // client accepts the differing Rlopen qid). Ref-before-build so a build
        // failure rolls the mint back cleanly.
        let clone_proto = match f.path {
            P_TCP_CLONE => Some(PROTO_TCP),
            P_UDP_CLONE => Some(PROTO_UDP),
            _ => None,
        };
        if let Some(proto) = clone_proto {
            let minted = if proto == PROTO_UDP {
                net.udp_clone()
            } else {
                net.tcp_clone()
            };
            let n = match minted {
                Some(n) => n,
                None => return self.err(tag, p9::E_NOMEM), // table full (ENFILE-class)
            };
            net.slot_ref(n); // refs 0 -> 1 (this fid owns the connection)
            let ctl = make_conn(proto, n, FK_CTL);
            let q = self.qid_of(net, ctl);
            match p9::build_rlopen(&mut self.out_buf, tag, &q, 0) {
                Ok(len) => {
                    self.fids[i] = Some(Fid {
                        fid: a.fid,
                        path: ctl,
                        opened: true,
                    });
                    Ok(len)
                }
                Err(()) => {
                    net.clone_rollback(n); // refs 1 -> 0 -> freed; fid stays at clone
                    Err(())
                }
            }
        } else {
            let mut nf = f;
            nf.opened = true;
            self.fids[i] = Some(nf);
            let q = self.qid_of(net, f.path);
            p9::build_rlopen(&mut self.out_buf, tag, &q, 0)
        }
    }

    fn h_read(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tread(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        if net.is_dir(f.path) {
            // Directory listing is Treaddir. A Tread on a dir is EISDIR.
            return self.err(tag, p9::E_ISDIR);
        }
        // The `data` file is the live recv stream, not static content: dequeue
        // from the socket's rx buffer (non-blocking; 0 = no data / closed). A
        // stream has no seekable offset, so the Tread offset is ignored.
        if is_conn(f.path) && conn_filekind(f.path) == FK_DATA {
            let n = conn_n(f.path);
            let cap = (self.msize as usize)
                .saturating_sub(p9::P9_HDR_LEN + 4)
                .min(a.count as usize)
                .min(DATA_CHUNK);
            let mut scratch = [0u8; DATA_CHUNK];
            let k = net.data_recv(n, &mut scratch[..cap]);
            return p9::build_rread(&mut self.out_buf, tag, &scratch[..k]);
        }
        let content = net.file_content(f.path);
        let body = content.as_slice();
        let off = a.offset as usize;
        let avail: &[u8] = if off >= body.len() { &[] } else { &body[off..] };
        let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
        let want = (a.count as usize).min(cap).min(avail.len());
        p9::build_rread(&mut self.out_buf, tag, &avail[..want])
    }

    fn h_readdir(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_treaddir(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        if !net.is_dir(f.path) {
            return self.err(tag, p9::E_NOTDIR);
        }
        // Assemble the dirent stream for entries AFTER `offset` (an ordinal
        // resume cookie: entry K carries next_offset K+1, never 0), bounded by
        // the Treaddir count and msize. A directory's child order is stable, so
        // the ordinal is a valid resume point across a paginated read.
        let budget = rreaddir_budget(a.count, self.msize);
        let mut data: Vec<u8> = Vec::new();
        let mut ord: u64 = 0;
        let mut full = false;
        net.for_each_child(f.path, |name, child, is_dir| {
            ord += 1;
            if full || ord <= a.offset {
                return; // already delivered in a prior page (or past the budget)
            }
            let entry_len = p9::dirent_len(name.len());
            if data.len() + entry_len > budget {
                full = true;
                return;
            }
            let mut scratch = [0u8; 64 + p9::P9_QID_LEN + 8 + 1 + 2];
            let q = p9::Qid {
                kind: if is_dir { p9::P9_QTDIR } else { p9::P9_QTFILE },
                version: 0,
                path: child,
            };
            let dtype = if is_dir { p9::DT_DIR } else { p9::DT_REG };
            if let Ok(used) = p9::pack_dirent(&mut scratch, 0, &q, ord, dtype, name) {
                data.extend_from_slice(&scratch[..used]);
            } else {
                full = true;
            }
        });
        p9::build_rreaddir(&mut self.out_buf, tag, &data)
    }

    fn h_getattr(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let fid = match p9::parse_tgetattr(tmsg) {
            Ok(f) => f,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        let (mode, nlink, size) = if net.is_dir(f.path) {
            (DIR_MODE, 2u64, 0u64)
        } else {
            let m = match f.path {
                P_TCP_CLONE | P_UDP_CLONE => FILE_RW,
                _ if is_conn(f.path) => match conn_filekind(f.path) {
                    FK_CTL | FK_DATA => FILE_RW,
                    _ => FILE_RO,
                },
                _ => FILE_RO, // stats
            };
            (m, 1u64, net.file_content(f.path).as_slice().len() as u64)
        };
        // The security trio (mode/uid/gid) MUST be filled: the kernel's A-3
        // dev9p per-component X-search reads them, and an unfilled trio fails
        // closed -> the /net walk is DENIED (ninep build_rgetattr doc).
        let valid = p9::P9_GETATTR_MODE
            | p9::P9_GETATTR_NLINK
            | p9::P9_GETATTR_UID
            | p9::P9_GETATTR_GID
            | P9_GETATTR_SIZE;
        let q = self.qid_of(net, f.path);
        p9::build_rgetattr(
            &mut self.out_buf,
            tag,
            valid,
            &q,
            mode,
            T_PRINCIPAL_SYSTEM,
            T_GID_SYSTEM,
            nlink,
            size,
        )
    }

    fn h_clunk(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tclunk(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        if !self.fid_clunk(net, a.fid) {
            return self.err(tag, p9::E_BADF);
        }
        p9::build_rclunk(&mut self.out_buf, tag)
    }

    /// Tflush: the kernel client abandoned an in-flight request (a death-
    /// interrupt on a thread blocked in a `listen` open). Cancel any deferred
    /// accept held under `oldtag` -- no late Rlopen, no connection minted for
    /// the dead op -- then Rflush. Per 9P the client reuses `oldtag` only after
    /// this Rflush, so the held listen tag is reclaimed cleanly (no tag leak in
    /// the kernel's outstanding-table, the latent net-2c-2 closes here).
    fn h_flush(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tflush(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        net.cancel_accept_tag(self.handle, a.oldtag);
        p9::build_rflush(&mut self.out_buf, tag)
    }

    fn h_write(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_twrite(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        let i = match self.fid_find(a.fid) {
            Some(i) => i,
            None => return self.err(tag, p9::E_BADF),
        };
        let f = self.fids[i].unwrap();
        if !f.opened {
            return self.err(tag, p9::E_PROTO);
        }
        if !is_conn(f.path) {
            return self.err(tag, p9::E_INVAL); // static nodes (stats/clone) are read-only
        }
        let n = conn_n(f.path);
        match conn_filekind(f.path) {
            FK_CTL => self.ctl_write(net, n, a.data, a.count, tag),
            FK_DATA => {
                let k = net.data_send(n, a.data);
                p9::build_rwrite(&mut self.out_buf, tag, k as u32)
            }
            // local/remote/status/err are read-only.
            _ => self.err(tag, p9::E_INVAL),
        }
    }

    /// Parse + apply one `ctl` command (section 3.3). On success the whole write
    /// is consumed (Rwrite count). connect/hangup (net-2c-2) + announce (net-3a)
    /// are live; bind/keepalive/ttl/tos are the options surface (net-4+), rejected
    /// honestly (EOPNOTSUPP) rather than silently accepted.
    fn ctl_write(
        &mut self,
        net: &mut Net,
        n: u32,
        data: &[u8],
        count: u32,
        tag: u16,
    ) -> Result<usize, ()> {
        let mut it = data
            .split(|&b| b == b' ' || b == b'\t' || b == b'\r' || b == b'\n')
            .filter(|t| !t.is_empty());
        let verb = it.next().unwrap_or(b"");
        match verb {
            b"connect" => {
                let arg = match it.next() {
                    Some(a) => a,
                    None => return self.err(tag, p9::E_INVAL),
                };
                let (ip, port) = match parse_dial(arg) {
                    Some(v) => v,
                    None => return self.err(tag, p9::E_INVAL),
                };
                match net.ctl_connect(n, ip, port) {
                    Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, count),
                    Err(()) => self.err(tag, p9::E_INVAL),
                }
            }
            b"hangup" => {
                net.ctl_hangup(n);
                p9::build_rwrite(&mut self.out_buf, tag, count)
            }
            b"announce" => {
                let arg = match it.next() {
                    Some(a) => a,
                    None => return self.err(tag, p9::E_INVAL),
                };
                let ep = match parse_announce(arg) {
                    Some(e) => e,
                    None => return self.err(tag, p9::E_INVAL),
                };
                match net.ctl_announce(n, ep) {
                    Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, count),
                    Err(()) => self.err(tag, p9::E_INVAL),
                }
            }
            _ => self.err(tag, p9::E_OPNOTSUPP),
        }
    }
}
