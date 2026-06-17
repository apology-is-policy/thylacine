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
use libthyla_rs::time::Instant;
use libthyla_rs::{
    t_close, t_open, t_walk_create, T_GID_SYSTEM, T_OPATH, T_OREAD, T_PRINCIPAL_SYSTEM,
    T_WALK_OPEN_FROM_ROOT,
};
use smoltcp::iface::{Interface, SocketHandle, SocketSet};
use smoltcp::socket::tcp;
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

/// The largest `data` chunk a single Tread drains from a socket's rx buffer.
const DATA_CHUNK: usize = 4096;

/// The ephemeral local-port range floor (IANA 49152..=65535). smoltcp's
/// `connect` requires a non-zero local port (it auto-selects only the local
/// ADDRESS), so netd assigns one from this range, rotating per active open.
const EPHEMERAL_LO: u16 = 49152;

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

const CONN_FLAG: u64 = 1 << 40;
const PROTO_SHIFT: u64 = 32;
const FILE_BITS: u64 = 8;
const FILE_MASK: u64 = 0xff;
const N_MASK: u64 = 0x00ff_ffff; // 24-bit connection number

// Protocols (only TCP mints connections at net-2c-1; the field is kept so udp/
// icmp connections slot in without a qid re-encode).
const PROTO_TCP: u64 = 0;

// Per-connection file kinds (the filekind low byte). FK_LISTEN (the server
// accept file) lands at net-2c-2 with the announce/accept path.
const FK_DIR: u64 = 0; //    /net/tcp/N/
const FK_CTL: u64 = 1; //    /net/tcp/N/ctl
const FK_DATA: u64 = 2; //   /net/tcp/N/data
const FK_LOCAL: u64 = 3; //  /net/tcp/N/local
const FK_REMOTE: u64 = 4; // /net/tcp/N/remote
const FK_STATUS: u64 = 5; // /net/tcp/N/status
const FK_ERR: u64 = 6; //    /net/tcp/N/err

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
        PROTO_TCP => P_TCP,
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
    socket: Option<SocketHandle>,
    // The endpoints recorded at `connect` time (netd-side), so `local`/`remote`
    // report a stable tuple regardless of the live socket's later transitions.
    local: Option<([u8; 4], u16)>,
    remote: Option<([u8; 4], u16)>,
    err: Option<&'static str>,
}

impl Slot {
    const fn empty() -> Slot {
        Slot {
            used: false,
            refs: 0,
            socket: None,
            local: None,
            remote: None,
            err: None,
        }
    }
}

pub struct Net {
    iface: Interface,
    sockets: SocketSet<'static>,
    base: Instant,
    next_local_port: u16,
    slots: [Slot; MAX_SLOTS],
    tcp_active: u32, // currently-live TCP connections (the `active` stat)
    tcp_opened: u32, // total TCP connections ever minted
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
            socket: Some(h),
            local: None,
            remote: None,
            err: None,
        };
        self.tcp_active += 1;
        self.tcp_opened += 1;
        Some(n as u32)
    }

    fn slot_live(&self, n: u32) -> bool {
        (n as usize) < MAX_SLOTS && self.slots[n as usize].used
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
                if let Some(h) = self.slots[i].socket.take() {
                    let _ = self.sockets.remove(h);
                }
                self.slots[i] = Slot::empty();
                self.tcp_active = self.tcp_active.saturating_sub(1);
            }
        }
    }

    /// Allocate the next rotating ephemeral local port (49152..=65535). smoltcp
    /// requires a non-zero local port on connect; at MAX_SLOTS connections a
    /// collision after wrap is astronomically unlikely (a liveness-checked
    /// allocator is a v1.x refinement).
    fn next_ephemeral(&mut self) -> u16 {
        let p = self.next_local_port;
        self.next_local_port = if p == u16::MAX { EPHEMERAL_LO } else { p + 1 };
        p
    }

    fn slot_socket(&self, n: u32) -> Option<SocketHandle> {
        if self.slot_live(n) {
            self.slots[n as usize].socket
        } else {
            None
        }
    }

    /// The `connect` ctl verb (section 3.3): active-open the connection's socket
    /// to `(ip, port)`. Records the resolved local + remote endpoints in the
    /// slot, so `local`/`remote` report a stable tuple regardless of the live
    /// socket's later transitions. Err on a smoltcp connect rejection.
    fn ctl_connect(&mut self, n: u32, ip: [u8; 4], port: u16) -> Result<(), ()> {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return Err(()),
        };
        let local_port = self.next_ephemeral();
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

    /// The `hangup` ctl verb: active-close the connection's socket (it drains +
    /// FINs; the fid clunk later frees N and the socket).
    fn ctl_hangup(&mut self, n: u32) {
        if let Some(h) = self.slot_socket(n) {
            self.sockets.get_mut::<tcp::Socket>(h).close();
        }
    }

    /// Write `data` to the connection's send stream. Returns bytes accepted (0
    /// if the socket cannot send: not established / tx-buffer full). Non-blocking.
    fn data_send(&mut self, n: u32, data: &[u8]) -> usize {
        match self.slot_socket(n) {
            Some(h) => self
                .sockets
                .get_mut::<tcp::Socket>(h)
                .send_slice(data)
                .unwrap_or(0),
            None => 0,
        }
    }

    /// Read up to `out.len()` bytes from the connection's recv stream. Returns
    /// bytes dequeued (0 if none available or the stream is closed). Non-
    /// blocking: a 0-return is ambiguous between "no data yet" and EOF at
    /// net-2c-2 -- proper readiness/blocking is the dev9p.poll leg (net-6).
    fn data_recv(&mut self, n: u32, out: &mut [u8]) -> usize {
        match self.slot_socket(n) {
            Some(h) => self
                .sockets
                .get_mut::<tcp::Socket>(h)
                .recv_slice(out)
                .unwrap_or(0),
            None => 0,
        }
    }

    /// The live TCP state of a connection (the `status` file).
    fn state_str(&self, n: u32) -> &'static str {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return "Closed",
        };
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
                P_UDP_STATS => P_UDP,
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
            // Inside /net/tcp/N/: the fixed file set.
            let (proto, n) = (conn_proto(dir), conn_n(dir));
            let fk = match name {
                b"ctl" => FK_CTL,
                b"data" => FK_DATA,
                b"local" => FK_LOCAL,
                b"remote" => FK_REMOTE,
                b"status" => FK_STATUS,
                b"err" => FK_ERR,
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
                _ => parse_dec(name)
                    .filter(|&n| self.slot_live(n))
                    .map(|n| make_conn(PROTO_TCP, n, FK_DIR)),
            },
            P_UDP => match name {
                b"stats" => Some(P_UDP_STATS),
                _ => None,
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
                for i in 0..MAX_SLOTS {
                    if self.slots[i].used {
                        let mut name = DecName::new();
                        name.set(i as u32);
                        push(
                            name.as_slice(),
                            make_conn(PROTO_TCP, i as u32, FK_DIR),
                            true,
                        );
                    }
                }
            }
            P_UDP => push(b"stats", P_UDP_STATS, false),
            P_ICMP => push(b"stats", P_ICMP_STATS, false),
            _ => {}
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
            P_UDP_STATS => c.push(b"udp\n  ports 0\n"),
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
        }
    }

    pub fn handle(&self) -> i64 {
        self.handle
    }

    /// Drop all this connection's references before the connection is closed
    /// (the serve() loop calls this before removing a dead Conn), so a session
    /// teardown frees any connections only this session held open.
    pub fn teardown(&mut self, net: &mut Net) {
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
            let rlen = self.dispatch(net, &frame, hdr);
            if rlen == 0 {
                return false; // unrecoverable build failure
            }
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
            self.in_buf.drain(..size);
        }
    }

    fn dispatch(&mut self, net: &mut Net, tmsg: &[u8], hdr: p9::Header) -> usize {
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
            // Tauth/Tsetattr/... are unused on the /net tree (read-only metadata).
            _ => self.err(tag, p9::E_NOSYS),
        };
        r.unwrap_or_else(|_| {
            // A build/parse error mid-reply: re-clear (a partial build may have
            // written into out_buf) and emit Rlerror(EPROTO).
            self.out_buf.clear();
            self.out_buf.resize(SRV_MSIZE_USIZE, 0);
            p9::build_rlerror(&mut self.out_buf, tag, p9::E_PROTO).unwrap_or(0)
        })
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
        let _ = a.flags; // open is non-blocking; ctl/data drive the connection

        // The Plan 9 clone idiom: opening `clone` MINTS a connection and rebinds
        // THIS fid onto the new connection's `ctl` (the kernel dev9p client
        // accepts the differing Rlopen qid). Ref-before-build so a build failure
        // rolls the mint back cleanly.
        if f.path == P_TCP_CLONE {
            let n = match net.tcp_clone() {
                Some(n) => n,
                None => return self.err(tag, p9::E_NOMEM), // table full (ENFILE-class)
            };
            net.slot_ref(n); // refs 0 -> 1 (this fid owns the connection)
            let ctl = make_conn(PROTO_TCP, n, FK_CTL);
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
                    net.slot_unref(n); // refs 1 -> 0 -> freed; fid stays at clone
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
        let budget = (a.count as usize)
            .min(self.msize as usize)
            .min(SRV_MSIZE_USIZE);
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
                P_TCP_CLONE => FILE_RW,
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
    /// is consumed (Rwrite count). connect/hangup are live at net-2c-2;
    /// announce/bind/keepalive/ttl/tos are the server-side + options surface
    /// (net-3+), rejected honestly (EOPNOTSUPP) rather than silently accepted.
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
            _ => self.err(tag, p9::E_OPNOTSUPP),
        }
    }
}
