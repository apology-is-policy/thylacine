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
use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};
use smoltcp::phy::{ChecksumCapabilities, Device, Loopback, Medium};
use smoltcp::socket::{dns, icmp, tcp, udp};
use smoltcp::time::Instant as SmolInstant;
use smoltcp::wire::{
    DnsQueryType, EthernetAddress, HardwareAddress, Icmpv4Packet, Icmpv4Repr, IpAddress, IpCidr,
    IpEndpoint, IpListenEndpoint, Ipv4Address, Ipv4Cidr,
};

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

/// Per-connection smoltcp ICMP socket buffer sizes (net-3c). Like UDP, an ICMP
/// PacketBuffer stores whole packets (an echo request/reply) with per-packet
/// metadata (the peer IP). A ping is tiny, so 4 slots x 2 KiB is ample.
const ICMP_META_SLOTS: usize = 4;
const ICMP_RX_BUF: usize = 2048;
const ICMP_TX_BUF: usize = 2048;

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

/// net-4b live DNS resolver demo (an A-record query through the shared dns
/// socket). BEST-EFFORT, logged, never asserted -- slirp forwards DNS to the
/// host resolver, so a response is host-dependent (the net-3b lesson). The
/// /net/dns machinery's deterministic proof is joey's numeric + ndb probe.
const DNS_LIVE_POLLS: u32 = 40; // ~400ms worst case
const DNS_LIVE_POLL_MS: u64 = 10;
const DNS_LIVE_NAME: &[u8] = b"example.com";

/// net-3c ICMP ping round-trip demo (an echo request to slirp's gateway).
/// BEST-EFFORT, logged, never asserted -- whether QEMU slirp answers a guest
/// ICMP echo to the gateway internally (vs proxying it to a host ping socket,
/// which the host environment may or may not permit) is host-dependent, so a
/// round-trip is NOT a sound boot gate ([[feedback-no-host-load]]). The
/// deterministic proof is joey's /net/icmp machinery probe; the deterministic
/// in-guest ICMP round-trip E2E is owed to net-3d (the loopback interface, where
/// smoltcp auto-replies to an echo request addressed to its own IP).
const PING_PROBE_POLLS: u32 = 30; // ~300ms worst case
const PING_PROBE_POLL_MS: u64 = 10;
const PING_PROBE_GATEWAY: [u8; 4] = [10, 0, 2, 2]; // QEMU slirp's virtual gateway
const PING_PROBE_PAYLOAD: &[u8] = b"thylacine-net-3c";

/// The base Echo identifier netd's ICMP sockets rotate from. Each /net/icmp
/// connection binds a distinct ident so smoltcp routes EchoReplies back to the
/// connection that sent the matching EchoRequest.
const ICMP_IDENT_BASE: u16 = 0x7c00;

/// net-3d loopback E2E: a DETERMINISTIC in-guest round-trip self-test over an
/// ISOLATED loopback stack (127.0.0.1/8) -- no NIC, no host. Unlike the gateway/
/// DNS probes (host-coupled, best-effort), this is fully in-guest, so it is
/// ASSERTED (a PASS/FAIL line in the boot log). LO_LOOPBACK_PORT is the fixed TCP
/// listen port; LO_POLLS x LO_POLL_MS bounds each round-trip's poll loop (the
/// loopback device loops synchronously, but smoltcp's ms-clock must advance for
/// the TCP handshake timers, so a small per-poll sleep is used).
const LO_LOOPBACK_PORT: u16 = 7711;
const LO_POLLS: u32 = 200;
const LO_POLL_MS: u64 = 2;
const LO_SEED: u64 = 0x9e37_79b9_7f4a_7c15;
// The ISN seed for the RESIDENT loopback stack (net-8a). A fixed seed is sound
// for loopback (no off-path attacker on the wire), like LO_SEED for the E2Es; a
// distinct value keeps the two stacks' sequence spaces independent in a boot
// that runs both (the selftest + a live 127.x user).
const LO_RESIDENT_SEED: u64 = 0xd1b5_4a32_d192_ed03;

/// An IPv4 address in the loopback block 127.0.0.0/8 (RFC 1122) -- routed to the
/// resident lo stack (net-8a) rather than the NIC.
fn is_loopback_v4(ip: [u8; 4]) -> bool {
    ip[0] == 127
}

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
const P_ICMP_CLONE: u64 = 9; // /net/icmp/clone (net-3c)
const P_CS: u64 = 10; //        /net/cs (net-4a: the connection server)
const P_DNS: u64 = 11; //       /net/dns (net-4b: the resolver)
const P_IPIFC: u64 = 12; //     /net/ipifc (net-4c: the interface-config dir)
const P_NDB: u64 = 13; //       /net/ndb (net-4c: the live dynamic network database)
const P_IPIFC_0: u64 = 14; //   /net/ipifc/0 (the one interface dir; the NIC)
const P_IPIFC_0_CTL: u64 = 15; //    /net/ipifc/0/ctl (add/remove static config)
const P_IPIFC_0_STATUS: u64 = 16; // /net/ipifc/0/status (the live config view)
const P_IPIFC_0_LOCAL: u64 = 17; //  /net/ipifc/0/local (the interface address)
const P_SUMMARY: u64 = 18; //   /net/summary (net-7b: the observability rollup)

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
const PROTO_ICMP: u64 = 2; // net-3c

// Per-connection file kinds (the filekind low byte).
const FK_DIR: u64 = 0; //    /net/tcp/N/
const FK_CTL: u64 = 1; //    /net/tcp/N/ctl
const FK_DATA: u64 = 2; //   /net/tcp/N/data
const FK_LOCAL: u64 = 3; //  /net/tcp/N/local
const FK_REMOTE: u64 = 4; // /net/tcp/N/remote
const FK_STATUS: u64 = 5; // /net/tcp/N/status
const FK_ERR: u64 = 6; //    /net/tcp/N/err
const FK_LISTEN: u64 = 7; // /net/tcp/N/listen (the server accept file; net-3a)
const FK_READY: u64 = 8; //  /net/tcp/N/ready (the dev9p.poll readiness file; net-6b)

// The poll event bits the `ready` file speaks (net-6b). A read on
// /net/<proto>/N/ready carries the requested mask in its Tread OFFSET (POLLIN |
// POLLOUT -- the only requestable bits) and returns the satisfied `revents` (the
// requested readable/writable subset PLUS the always-reported POLLERR/POLLHUP) as
// a u32 LE, WITHOUT consuming socket data. The bit values mirror the kernel
// poll.h ABI -- dev9p.poll (NET-DESIGN 12.2) is the only client, passing its
// `events` as the read offset. A non-zero revents replies at once; a zero revents
// DEFERS (park a PendingReady; poll_ready delivers when the socket becomes ready
// per the mask), so a poll(POLLIN) on a writable-but-empty socket waits for data
// rather than busy-looping -- the offset names the SPECIFIC events to wait for.
const POLLIN: u16 = 0x001;
const POLLOUT: u16 = 0x004;
const POLLERR: u16 = 0x008;
const POLLHUP: u16 = 0x010;

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
        PROTO_ICMP => P_ICMP,
        _ => P_TCP,
    }
}

fn proto_name(proto: u64) -> &'static [u8] {
    match proto {
        PROTO_UDP => b"udp",
        PROTO_ICMP => b"icmp",
        _ => b"tcp",
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
    // The ICMP Echo identifier this slot's socket is bound to (net-3c): an echo
    // request carries it so the matching reply routes back to this slot.
    // Meaningless unless proto == PROTO_ICMP.
    icmp_ident: u16,
    // A monotonic mint generation (net-3d): every mint of this slot index stamps
    // a fresh value from `Net.mint_seq`. A deferred accept records its listener's
    // gen at registration; poll_accepts drops the pending if the slot's CURRENT
    // gen differs (the slot was freed + re-minted out from under the pending), so
    // a re-used index can never type-confuse the listener's `get::<tcp::Socket>`.
    // 0 == a free / never-minted slot (next_gen never returns 0).
    gen: u32,
    // Which smoltcp stack this slot's socket lives in (net-8a). false == the NIC
    // stack (`Net.sockets`/`Net.iface`, the default at clone); true == the
    // resident loopback stack (`Net.lo`), set by ensure_lo_stack when a 127.x
    // dial/announce migrates the fresh socket there. The set a slot's
    // SocketHandle is valid in -- `set_ref`/`set_mut` route every socket access
    // by it, since a handle is set-specific (a get on the wrong set panics). Only
    // ever true when `Net.lo` is Some, so the lo-branch unwrap is unreachable.
    lo: bool,
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
            icmp_ident: 0,
            gen: 0,
            lo: false,
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
    listening_gen: u32, // the listener slot's mint generation at registration;
                      // poll_accepts drops the pending if it no longer matches
                      // (the slot was freed + re-minted), so a re-used index
                      // cannot type-confuse the listener's get::<tcp::Socket>.
}

/// A blocking `data` read holding its Rread (net-6a). A `read` on `/net/tcp/N/
/// data` that finds the socket's rx buffer empty but the connection still open
/// DEFERS instead of returning 0: it parks a PendingRead and the serve loop's
/// poll_data sends the held Rread when bytes arrive (or 0 on EOF). This makes
/// `recv()` block (the net-5 audit F2 seam) over the existing dev9p client
/// (match-by-tag, no per-op deadline, death-interruptible) -- no kernel surface.
/// One per outstanding blocked read; multiple on one fid (a multi-thread Proc
/// reading a shared fd) are FIFO-delivered. Bounded by MAX_FIDS per connection.
#[derive(Copy, Clone)]
struct PendingRead {
    fid: u32,    // the fid the read is on (cancelled on its clunk)
    slot_n: u32, // the connection slot to recv from when ready
    tag: u16,    // the held Tread tag (cancelled by a Tflush on it)
    cap: usize,  // the negotiated max bytes to return
}

/// The connect state of a TCP slot's socket, for the deferred `data` open.
#[derive(Copy, Clone, PartialEq, Eq)]
enum ConnectState {
    Pending, // SynSent/SynReceived -- the handshake is in flight
    Ready,   // ESTABLISHED (or a non-TCP slot) -- `data` is usable
    Failed,  // Closed/aborted -- the connect was refused/reset/gave up
}

/// A deferred TCP `data` open held until the connection reaches ESTABLISHED
/// (the net-3a/net-6a deferred-reply pattern applied to the outbound connect).
/// A real outbound connection has RTT, so `data` is NOT usable the instant
/// `connect` is issued (the socket is SynSent); replying Rlopen immediately lets
/// the client write into a SynSent socket, where `send_slice` fails -- the
/// outbound-connect bug the loopback-only E2Es (0 RTT -> established within a
/// poll) masked. netd HOLDS the Rlopen until the handshake completes (deliver
/// the held Rlopen -> `data` is live), the connect fails (RST/reset -> Rlerror
/// ECONNREFUSED), or the deadline expires (abort -> Rlerror ETIMEDOUT, so a SYN
/// to an unreachable host cannot hang the open forever). Loopback establishes
/// within a poll, so it resolves on the first poll there (the E2Es are
/// behavior-identical). Cancelled by the fid's clunk / a Tflush on its tag /
/// teardown / Tversion, exactly like PendingRead. Bounded by MAX_FIDS (#65).
#[derive(Copy, Clone)]
struct PendingConnect {
    fid: u32,         // the data fid (cancelled on its clunk)
    slot_n: u32,      // the connection slot whose handshake we await
    tag: u16,         // the held Tlopen tag (cancelled by a Tflush on it)
    path: u64,        // the data file path (to compute the held Rlopen's qid)
    deadline_ms: u64, // abort + Rlerror ETIMEDOUT if still Pending past this
}

/// How long netd holds a deferred `data` open before giving up on an
/// unreachable peer (a SYN that never gets a SYN-ACK). A refusal (RST) fails
/// immediately via ConnectState::Failed; this bounds only the silent case.
const CONNECT_TIMEOUT_MS: u64 = 15_000;

/// A deferred readiness probe (net-6b): a `read(/net/<proto>/N/ready)` whose
/// requested poll mask (the Tread OFFSET) is not yet satisfied. netd HOLDS the
/// Rread until the socket becomes ready for the mask (or an always-reported
/// POLLERR/POLLHUP fires), then poll_ready sends the 4-byte revents bitmap. The
/// kernel dev9p.poll bridge (NET-DESIGN 12.2) is the sole client: it keeps one
/// outstanding readiness read per polled fd, so a poller parks until the socket
/// transitions -- the probe-then-observe of net_poll.tla, with no busy loop
/// (the mask names the specific events to wait for). Non-consuming (it reports
/// readiness, never dequeues). Cancelled by the fid's clunk / a Tflush on its
/// tag / teardown / Tversion, exactly like PendingRead. Bounded by MAX_FIDS per
/// connection (#65 floor).
#[derive(Copy, Clone)]
struct PendingReady {
    fid: u32,    // the fid the read is on (cancelled on its clunk)
    slot_n: u32, // the connection slot whose readiness to report
    tag: u16,    // the held Tread tag (cancelled by a Tflush on it)
    mask: u16,   // the requested poll events (POLLIN | POLLOUT) from the offset
}

/// The result of a non-blocking dequeue from a connection's rx (net-6a). The
/// blocking read path needs to distinguish "no data yet, the connection is open"
/// (DEFER) from "end of stream" (return 0) -- a distinction the older `data_recv`
/// collapsed to 0 (the net-5 F2 ambiguity). Mapped from smoltcp 0.12: TCP
/// Ok(k>0)=Data, Ok(0)/connecting=WouldBlock, Err(Finished)=Eof (peer FIN +
/// drained), Err(InvalidState)+!is_active=Eof (closed); UDP/ICMP Err(Exhausted)=
/// WouldBlock (connectionless -- never Eof; a blocked datagram/ping read waits
/// for a packet or its fid's clunk/flush).
enum RecvOutcome {
    Data(usize), // `n` bytes copied into the caller's buffer (n may be 0: a real
    // empty UDP datagram)
    WouldBlock, // no data yet, the connection is open -- the caller may defer
    Eof,        // end of stream (TCP FIN drained / socket gone) -- read returns 0
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

/// The outcome of the net-3c best-effort ICMP ping demo (an echo to the gateway).
/// The serve loop logs it; it is never a boot gate (whether slirp answers a
/// guest echo internally is host-dependent).
pub enum PingProbe {
    /// An echo reply came back (`reply_len` payload bytes).
    Ok { reply_len: usize },
    /// No reply within the bound (slirp proxied the echo to a host ping the host
    /// did not permit, most likely).
    NoResponse,
    /// Could not even mint the probe socket (the slot table was full).
    MintFailed,
}

/// The state of a started DNS query (net-4b). Resolved carries the first A
/// record; Failed covers SERVFAIL/NXDOMAIN/no-A-record/timeout (all map to the
/// empty answer at the cs/dns layer).
enum DnsPoll {
    Pending,
    Resolved([u8; 4]),
    Failed,
}

/// The outcome of the net-4b best-effort live DNS query demo. The serve loop
/// logs it; it is never a boot gate (the response is host-resolver-dependent).
pub enum DnsProbeResult {
    /// An A record came back.
    Ok([u8; 4]),
    /// No usable response within the bound (the host has no/blocked resolver).
    NoResponse,
    /// No resolver was learned from the lease (the dns socket is absent).
    NoServer,
}

/// The live interface configuration snapshot (net-4c, NET-DESIGN section 6).
/// Built from the DHCP lease at bring-up (the dynamic path) or overwritten by an
/// `ipconfig add` ctl write (the static path), and surfaced read-only through
/// `/net/ipifc/0` + `/net/ndb`. `mac`/`mtu` are fixed NIC facts; the address /
/// gateway / resolver are the live lease (or static) facts. The snapshot is kept
/// coherent with the smoltcp iface by `ifc_set_static`/`ifc_clear` (they mutate
/// both), so a reader never sees an address the iface is not actually using.
#[derive(Clone, Copy)]
pub struct IfConfig {
    pub mac: [u8; 6],
    pub mtu: usize,
    pub addr: [u8; 4],
    pub prefix: u8, // CIDR prefix length (24 == /24 == 255.255.255.0)
    pub gw: Option<[u8; 4]>,
    pub dns: Option<[u8; 4]>, // the primary resolver (the dynamic ndb `dns=`)
    pub up: bool,             // has an address (false before config / after remove)
    pub dynamic: bool,        // DHCP-leased (true) vs static `ipconfig add` (false)
}

impl IfConfig {
    /// An unconfigured interface (no address; used as the pre-lease seed and by
    /// the ipifc_e2e selftest's throwaway Net).
    pub fn empty() -> IfConfig {
        IfConfig {
            mac: [0; 6],
            mtu: 0,
            addr: [0; 4],
            prefix: 0,
            gw: None,
            dns: None,
            up: false,
            dynamic: false,
        }
    }
}

/// The resident loopback stack (net-8a): a second, isolated smoltcp interface on
/// a `Loopback` device, addressed `127.0.0.1/8`, so an in-guest client dialing
/// `127.x` reaches an in-guest server over the REAL `/net` 9P path (the owed
/// in-guest peer the NIC-only live stack could not give: slirp does not loop a
/// guest-to-own-IP packet deterministically). A loopback socket CANNOT share the
/// NIC iface+set -- the NIC default route steals `127.x` egress (net-3d) -- so it
/// gets its OWN iface + device + set, polled alongside the NIC each tick.
struct LoStack {
    iface: Interface,
    sockets: SocketSet<'static>,
    device: Loopback,
}

pub struct Net {
    iface: Interface,
    sockets: SocketSet<'static>,
    base: Instant,
    next_local_port: u16,
    slots: [Slot; MAX_SLOTS],
    tcp_active: u32,      // currently-live TCP connections (the `active` stat)
    tcp_opened: u32,      // total TCP connections ever minted
    udp_active: u32,      // currently-live UDP connections (net-3b)
    udp_opened: u32,      // total UDP connections ever minted
    icmp_active: u32,     // currently-live ICMP connections (net-3c)
    icmp_opened: u32,     // total ICMP connections ever minted
    next_icmp_ident: u16, // rotating Echo identifier for the next icmp_clone
    icmp_seq: u16,        // rotating echo sequence number across all icmp sends
    mint_seq: u32,        // monotonic slot mint generation (net-3d; stamped at
    // every clone/accept_swap; the deferred-accept guard)
    // Deferred accepts: held listen Rlopens awaiting an inbound call (net-3a).
    pending: Vec<PendingAccept>,
    // The shared DNS resolver socket (net-4b): one smoltcp dns::Socket seeded
    // from the DHCP-provided resolver, multiplexing every /net/dns + cs->dns
    // query across its growable query-slot table. None if the lease carried no
    // resolver (then a DNS query fails closed to an empty answer, never hangs).
    dns: Option<SocketHandle>,
    // The live interface-config snapshot (net-4c): the lease (or static) facts,
    // surfaced read-only through /net/ipifc/0 + /net/ndb. Kept coherent with the
    // iface by ifc_set_static / ifc_clear.
    ifc: IfConfig,
    // The resident loopback stack (net-8a). None until enable_loopback (the
    // resident netd opts in; the E2E selftests leave it None). A slot's socket
    // lives here iff its `lo` flag is set (a 127.x dial migrated it).
    lo: Option<LoStack>,
}

impl Net {
    /// Build the connection table around the already-configured smoltcp
    /// interface + socket set (moved in after DHCP bring-up). `base` is the
    /// monotonic anchor for smoltcp's millisecond clock. `ifc` is the live
    /// interface-config snapshot (net-4c): its `dns` field (the DHCP-learned
    /// primary resolver) installs the shared DNS socket -- None leaves `dns`
    /// None, so a DNS query fails closed to an empty answer (never hangs).
    pub fn new(
        iface: Interface,
        mut sockets: SocketSet<'static>,
        base: Instant,
        ifc: IfConfig,
    ) -> Net {
        // The DNS socket holds DNS_MAX_SERVER_COUNT (=1) server; the lease's
        // primary resolver is it. A growable (alloc) query-slot table backs the
        // per-fid queries, so start_query never fails for "no free slot".
        let dns = ifc.dns.map(|d| {
            let server = IpAddress::Ipv4(Ipv4Address::new(d[0], d[1], d[2], d[3]));
            sockets.add(dns::Socket::new(&[server], Vec::new()))
        });
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
            icmp_active: 0,
            icmp_opened: 0,
            next_icmp_ident: ICMP_IDENT_BASE,
            icmp_seq: 0,
            mint_seq: 0,
            pending: Vec::new(),
            dns,
            ifc,
            lo: None,
        }
    }

    /// Stand up the resident loopback stack (net-8a): a second isolated smoltcp
    /// interface on a `Loopback` device, addressed `127.0.0.1/8`. Idempotent. The
    /// resident netd opts in (main.rs, after `new`); the E2E selftests do NOT (they
    /// pass a loopback-configured iface as the PRIMARY stack + leave `lo` None, so
    /// their 127.x dials stay on `self.sockets` and `set_*` is behavior-identical
    /// for them -- the audited E2E paths are untouched). A fixed seed is sound for
    /// loopback (no off-path attacker on the wire to predict an ISN), matching the
    /// E2E precedent.
    pub fn enable_loopback(&mut self) {
        if self.lo.is_some() {
            return;
        }
        let mut device = Loopback::new(Medium::Ethernet);
        let mut cfg = Config::new(HardwareAddress::Ethernet(EthernetAddress([
            0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
        ])));
        cfg.random_seed = LO_RESIDENT_SEED;
        let ts0 = self.now();
        let mut iface = Interface::new(cfg, &mut device, ts0);
        iface.update_ip_addrs(|a| {
            let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
        });
        self.lo = Some(LoStack {
            iface,
            sockets: SocketSet::new(Vec::new()),
            device,
        });
    }

    /// Migrate a fresh connection slot onto the loopback stack (net-8a) -- a
    /// dial/announce to `127.x`. The slot's socket is fresh (clone reserved it on
    /// the NIC set; only status/check_ready have read it, never connected it), and
    /// the smoltcp `Socket` enum is not re-`add`able (AnySocket is impl'd only for
    /// the concrete types), so we DROP the NIC-set socket and mint an equivalent
    /// one on the lo set -- a never-connected socket carries no state worth
    /// preserving. No-op (returns true) if already on lo. Returns TRUE with no
    /// migration when there is no resident lo stack: that is the single-stack E2E
    /// config, where the PRIMARY stack already IS a 127.0.0.1/8 loopback (so a 127.x
    /// dial stays on `self.sockets` and works) -- the resident netd always has a lo
    /// stack (enable_loopback), so a 127.x dial there migrates. Returns false only
    /// on a dead slot or an ICMP rebind failure (fail-closed). The per-protocol
    /// buffer shapes MUST mirror the clone sites.
    fn ensure_lo_stack(&mut self, n: u32) -> bool {
        let i = n as usize;
        if !self.slot_live(n) {
            return false;
        }
        if self.slots[i].lo {
            return true;
        }
        let lo = match self.lo.as_mut() {
            Some(l) => l,
            // No resident lo stack: the primary stack serves 127.x (the E2E config).
            None => return true,
        };
        let proto = self.slots[i].proto;
        // Drop the NIC-set socket (fresh -- nothing to preserve). Done before the
        // lo borrow so the two field borrows do not overlap.
        let nh = match proto {
            PROTO_UDP => {
                let rx = udp::PacketBuffer::new(
                    alloc::vec![udp::PacketMetadata::EMPTY; UDP_META_SLOTS],
                    alloc::vec![0u8; UDP_RX_BUF],
                );
                let tx = udp::PacketBuffer::new(
                    alloc::vec![udp::PacketMetadata::EMPTY; UDP_META_SLOTS],
                    alloc::vec![0u8; UDP_TX_BUF],
                );
                lo.sockets.add(udp::Socket::new(rx, tx))
            }
            PROTO_ICMP => {
                let rx = icmp::PacketBuffer::new(
                    alloc::vec![icmp::PacketMetadata::EMPTY; ICMP_META_SLOTS],
                    alloc::vec![0u8; ICMP_RX_BUF],
                );
                let tx = icmp::PacketBuffer::new(
                    alloc::vec![icmp::PacketMetadata::EMPTY; ICMP_META_SLOTS],
                    alloc::vec![0u8; ICMP_TX_BUF],
                );
                let mut sock = icmp::Socket::new(rx, tx);
                if sock
                    .bind(icmp::Endpoint::Ident(self.slots[i].icmp_ident))
                    .is_err()
                {
                    return false;
                }
                lo.sockets.add(sock)
            }
            _ => {
                let rx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_RX_BUF]);
                let tx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_TX_BUF]);
                lo.sockets.add(tcp::Socket::new(rx, tx))
            }
        };
        // Drop the old NIC-set socket now that the lo socket is in hand.
        if let Some(oldh) = self.slots[i].socket.take() {
            let _ = self.sockets.remove(oldh);
        }
        self.slots[i].socket = Some(nh);
        self.slots[i].lo = true;
        true
    }

    /// The socket set a live slot's socket lives in: the loopback stack iff the
    /// slot was migrated by a 127.x dial (net-8a), else the NIC stack. A handle is
    /// set-specific (a typed get on the wrong set panics), so EVERY socket access
    /// routes through `set_ref`/`set_mut`. `slot.lo` is set true only by
    /// ensure_lo_stack, which runs only when `self.lo` is Some, so the lo-branch
    /// access is unreachable when `slot.lo` is false.
    fn set_ref(&self, n: u32) -> &SocketSet<'static> {
        if (n as usize) < MAX_SLOTS && self.slots[n as usize].lo {
            &self.lo.as_ref().unwrap().sockets
        } else {
            &self.sockets
        }
    }

    /// Whether a live slot's socket lives on the loopback stack (net-8a) -- the
    /// resident_lo_selftest's migration assertion.
    fn slot_on_lo(&self, n: u32) -> bool {
        self.slot_live(n) && self.slots[n as usize].lo
    }

    fn set_mut(&mut self, n: u32) -> &mut SocketSet<'static> {
        if (n as usize) < MAX_SLOTS && self.slots[n as usize].lo {
            &mut self.lo.as_mut().unwrap().sockets
        } else {
            &mut self.sockets
        }
    }

    /// Stamp the next monotonic mint generation (net-3d). Never 0 (0 marks a free
    /// / never-minted slot), so a freed slot's gen can never match a pending's
    /// recorded generation by accident.
    fn next_gen(&mut self) -> u32 {
        self.mint_seq = self.mint_seq.wrapping_add(1);
        if self.mint_seq == 0 {
            self.mint_seq = 1;
        }
        self.mint_seq
    }

    /// smoltcp's monotonic timestamp (ms since `base`).
    pub fn now(&self) -> SmolInstant {
        SmolInstant::from_millis(self.base.elapsed().as_millis() as i64)
    }

    /// Monotonic ms since `base` (for deferred-connect deadlines).
    pub fn now_ms(&self) -> u64 {
        self.base.elapsed().as_millis() as u64
    }

    /// The connect state of a TCP slot, for the deferred `data` open: Pending
    /// while the handshake is in flight (SynSent/SynReceived), Ready once
    /// ESTABLISHED (`accept_ready` = is_active past the SYN exchange), Failed
    /// once Closed/aborted. A non-TCP slot (UDP/ICMP) has no handshake -> Ready.
    fn tcp_connect_state(&self, n: u32) -> ConnectState {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return ConnectState::Failed,
        };
        if self.slot_proto(n) != Some(PROTO_TCP) {
            return ConnectState::Ready;
        }
        let s = self.set_ref(n).get::<tcp::Socket>(h);
        if accept_ready(s) {
            ConnectState::Ready
        } else if s.is_active() {
            ConnectState::Pending
        } else {
            ConnectState::Failed
        }
    }

    /// Abort a TCP slot's in-flight connect (on a deadline expiry) so the socket
    /// leaves SynSent -> the next poll_connects sees Failed and replies Rlerror.
    fn tcp_abort_connect(&mut self, n: u32) {
        if self.slot_proto(n) != Some(PROTO_TCP) {
            return;
        }
        if let Some(h) = self.slot_socket(n) {
            self.set_mut(n).get_mut::<tcp::Socket>(h).abort();
        }
    }

    /// Service the stack once: TX flush + RX drain across every socket. Called
    /// at the top of each serve-loop iteration and again after dispatching a
    /// batch of 9P requests, so a just-enqueued connect/send egresses this tick
    /// rather than waiting for the next poll timeout. `device` stays owned by
    /// the serve loop (only `iface.poll` borrows it).
    pub fn poll<D: Device + ?Sized>(&mut self, device: &mut D) {
        let ts = self.now();
        self.iface.poll(ts, device, &mut self.sockets);
        // Service the resident loopback stack on its own device/iface/set
        // (net-8a). Disjoint fields of `lo`, so no aliasing with the NIC poll.
        if let Some(lo) = self.lo.as_mut() {
            lo.iface.poll(ts, &mut lo.device, &mut lo.sockets);
        }
    }

    /// smoltcp's poll-delay hint (ms): how long until the stack next needs
    /// servicing -- the SOONER of the NIC and loopback stacks (net-8a). None ==
    /// both idle (no deadline). The serve loop clamps it.
    pub fn poll_delay_ms(&mut self) -> Option<u64> {
        let ts = self.now();
        let nic = self
            .iface
            .poll_delay(ts, &self.sockets)
            .map(|d| d.total_millis());
        let lo = self
            .lo
            .as_mut()
            .and_then(|l| l.iface.poll_delay(ts, &l.sockets).map(|d| d.total_millis()));
        match (nic, lo) {
            (Some(a), Some(b)) => Some(a.min(b)),
            (Some(a), None) => Some(a),
            (None, b) => b,
        }
    }

    // -------------------------------------------------------------------------
    // The DNS resolver (net-4b). One shared dns::Socket multiplexes every query.
    //
    // The smoltcp query lifetime is the central correctness hazard:
    // get_query_result FREES the slot on a result (Resolved/Failed) and PANICS
    // on an already-free slot. So a started query handle MUST be polled at most
    // once-to-completion and never touched again after a result. The handle
    // lives in exactly one place -- the issuing fid's `Query` (server side) --
    // which nulls it the instant dns_poll returns Resolved/Failed, so it is
    // never double-polled; dns_cancel is called only on a still-pending handle
    // (an abandoned query), where the slot is occupied and cancel is safe.
    // -------------------------------------------------------------------------

    /// Start an A-record query for `name`. None if there is no resolver, `name`
    /// is not valid UTF-8, or smoltcp rejects it (bad/too-long name). The query
    /// is queued; the next iface.poll sends it. The caller owns the returned
    /// handle and must poll it to completion (dns_poll) or cancel it (dns_cancel).
    fn dns_query_start(&mut self, name: &[u8]) -> Option<dns::QueryHandle> {
        let h = self.dns?;
        let name = core::str::from_utf8(name).ok()?;
        // Disjoint-field borrows: the socket (self.sockets) and the iface
        // context (self.iface) are separate fields of self, like tcp_connect.
        self.sockets
            .get_mut::<dns::Socket>(h)
            .start_query(self.iface.context(), name, DnsQueryType::A)
            .ok()
    }

    /// Poll a started DNS query. On Resolved/Failed smoltcp frees the query
    /// slot, so the caller MUST null its handle and never poll it again (a
    /// re-poll of a freed slot panics). Pending leaves the slot intact (safe to
    /// re-poll). A resolve with no A record (e.g. an AAAA-only name) is Failed.
    fn dns_poll(&mut self, q: dns::QueryHandle) -> DnsPoll {
        let h = match self.dns {
            Some(h) => h,
            None => return DnsPoll::Failed,
        };
        match self.sockets.get_mut::<dns::Socket>(h).get_query_result(q) {
            Ok(addrs) => match addrs.iter().find_map(first_ipv4) {
                Some(ip) => DnsPoll::Resolved(ip),
                None => DnsPoll::Failed,
            },
            Err(dns::GetQueryResultError::Pending) => DnsPoll::Pending,
            Err(dns::GetQueryResultError::Failed) => DnsPoll::Failed,
        }
    }

    /// Cancel a STILL-PENDING DNS query, freeing its slot. Called only on a
    /// handle dns_poll has not yet completed (clunk / teardown / Tversion /
    /// Tflush / rewrite). Cancelling an already-collected slot would panic; the
    /// single-completion discipline (the handle is nulled on a result) prevents it.
    fn dns_cancel(&mut self, q: dns::QueryHandle) {
        if let Some(h) = self.dns {
            self.sockets.get_mut::<dns::Socket>(h).cancel_query(q);
        }
    }

    /// net-4b boot demo: a self-contained live A-record query through the shared
    /// resolver, bounded-polled in its own loop. BEST-EFFORT, logged, never a
    /// boot gate -- QEMU slirp FORWARDS DNS to the host's resolver, so a response
    /// is host-environment-dependent (the net-3b lesson). The deterministic proof
    /// of the /net/dns machinery is joey's probe (the numeric + ndb fast paths);
    /// the deterministic in-guest deferred-resolution E2E is owed to net-4d.
    pub fn dns_live_probe(&mut self, device: &mut NicDevice) -> DnsProbeResult {
        let q = match self.dns_query_start(DNS_LIVE_NAME) {
            Some(q) => q,
            None => return DnsProbeResult::NoServer,
        };
        for _ in 0..DNS_LIVE_POLLS {
            self.poll(device);
            match self.dns_poll(q) {
                DnsPoll::Pending => {}
                DnsPoll::Resolved(ip) => return DnsProbeResult::Ok(ip),
                // A result frees the slot; the handle is dead now -- return
                // without cancelling (cancel would panic on the freed slot).
                DnsPoll::Failed => return DnsProbeResult::NoResponse,
            }
            let _ = sleep(Duration::from_millis(DNS_LIVE_POLL_MS));
        }
        // Timed out still pending: cancel the live query to free its slot.
        self.dns_cancel(q);
        DnsProbeResult::NoResponse
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

    /// net-3c boot demo: a self-contained ICMP echo round-trip through the live
    /// /net/icmp data path (icmp_clone -> connect -> data_send -> poll ->
    /// data_recv), bounded-polled in its own loop. BEST-EFFORT: whether QEMU
    /// slirp answers a guest echo to the gateway internally (vs proxying it to a
    /// host ping socket) is host-dependent, so the caller logs the outcome and
    /// never gates the boot on it (the /net/icmp machinery is the deterministic
    /// proof, via joey; the in-guest ICMP round-trip E2E is owed to net-3d). The
    /// probe connection is minted + freed entirely within this call (no leak).
    pub fn icmp_ping_probe(&mut self, device: &mut NicDevice) -> PingProbe {
        let n = match self.icmp_clone() {
            Some(n) => n,
            None => return PingProbe::MintFailed,
        };
        if self.icmp_connect(n, PING_PROBE_GATEWAY).is_err() {
            self.free_orphan_mint(n);
            return PingProbe::NoResponse;
        }
        let _ = self.data_send(n, PING_PROBE_PAYLOAD);

        let mut buf = [0u8; 256];
        let mut result = PingProbe::NoResponse;
        for _ in 0..PING_PROBE_POLLS {
            self.poll(device);
            let k = self.data_recv(n, &mut buf);
            if k > 0 {
                result = PingProbe::Ok { reply_len: k };
                break;
            }
            let _ = sleep(Duration::from_millis(PING_PROBE_POLL_MS));
        }
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
        let gen = self.next_gen();
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
            icmp_ident: 0,
            gen,
            lo: false,
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
        let gen = self.next_gen();
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
            icmp_ident: 0,
            gen,
            lo: false,
        };
        self.udp_active += 1;
        self.udp_opened += 1;
        Some(n as u32)
    }

    /// Mint an ICMP connection (net-3c): an icmp::Socket bound to a unique Echo
    /// identifier, so EchoReplies for this connection's pings route back to it
    /// (smoltcp's accepts filter incoming ICMP by the bound ident). The Plan 9
    /// /net/icmp shape: `connect ip` records the target, a `data` write is an
    /// echo-request payload, a `data` read is the echo-reply payload.
    fn icmp_clone(&mut self) -> Option<u32> {
        let n = self.slots.iter().position(|s| !s.used)?;
        let rx = icmp::PacketBuffer::new(
            alloc::vec![icmp::PacketMetadata::EMPTY; ICMP_META_SLOTS],
            alloc::vec![0u8; ICMP_RX_BUF],
        );
        let tx = icmp::PacketBuffer::new(
            alloc::vec![icmp::PacketMetadata::EMPTY; ICMP_META_SLOTS],
            alloc::vec![0u8; ICMP_TX_BUF],
        );
        let ident = self.next_icmp_ident;
        let mut sock = icmp::Socket::new(rx, tx);
        // Bind BEFORE adding to the set, so a bind failure leaks nothing.
        if sock.bind(icmp::Endpoint::Ident(ident)).is_err() {
            return None;
        }
        self.next_icmp_ident = self.next_icmp_ident.wrapping_add(1);
        let gen = self.next_gen();
        let h = self.sockets.add(sock);
        self.slots[n] = Slot {
            used: true,
            refs: 0,
            proto: PROTO_ICMP,
            socket: Some(h),
            local: None,
            remote: None,
            err: None,
            listen_ep: None,
            icmp_ident: ident,
            gen,
            lo: false,
        };
        self.icmp_active += 1;
        self.icmp_opened += 1;
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
                    // Remove from the slot's stack (lo or NIC); set_mut still reads
                    // the slot's `lo` flag (only `socket` was taken). net-8a.
                    let _ = self.set_mut(n).remove(h);
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
            PROTO_ICMP => self.icmp_active = self.icmp_active.saturating_sub(1),
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
            Some(PROTO_ICMP) => self.icmp_opened = self.icmp_opened.saturating_sub(1),
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
            Some(PROTO_ICMP) => self.icmp_connect(n, ip),
            _ => Err(()),
        }
    }

    /// ICMP `connect` (net-3c): record the ping target. ICMP is portless and has
    /// no handshake (the socket was bound to its Echo ident at clone), so this
    /// only fixes the destination subsequent `data` echo-requests are sent to.
    fn icmp_connect(&mut self, n: u32, ip: [u8; 4]) -> Result<(), ()> {
        if self.slot_proto(n) != Some(PROTO_ICMP) {
            return Err(());
        }
        // A 127.x ping migrates this slot's socket onto the loopback stack (net-8a)
        // so the echo loops in-guest; the lo iface auto-answers an echo-request to
        // its own 127.0.0.1.
        if is_loopback_v4(ip) && !self.ensure_lo_stack(n) {
            return Err(());
        }
        let la = self.iface_ipv4_for(n).unwrap_or([0, 0, 0, 0]);
        self.slots[n as usize].local = Some((la, 0)); // ICMP has no local port
        self.slots[n as usize].remote = Some((ip, 0));
        self.slots[n as usize].err = None;
        Ok(())
    }

    /// The first IPv4 address bound on the stack a slot's socket lives on (net-8a):
    /// the loopback iface's `127.0.0.1` for a migrated slot, else the NIC iface's
    /// leased address (None pre-lease). UDP/ICMP report it as the connection's
    /// `local` address (the source the stack will select); TCP reads its selected
    /// source straight off the socket post-connect.
    fn iface_ipv4_for(&self, n: u32) -> Option<[u8; 4]> {
        let iface = if (n as usize) < MAX_SLOTS && self.slots[n as usize].lo {
            &self.lo.as_ref().unwrap().iface
        } else {
            &self.iface
        };
        iface.ip_addrs().iter().find_map(|c| match c.address() {
            IpAddress::Ipv4(v4) => Some(v4.octets()),
            #[allow(unreachable_patterns)]
            _ => None,
        })
    }

    // -------------------------------------------------------------------------
    // Interface configuration (net-4c, NET-DESIGN section 6). The /net/ipifc/0
    // ctl verbs apply static config onto BOTH the live smoltcp iface AND the
    // IfConfig snapshot, so the read-only /net/ipifc/0/status + /net/ndb views
    // always reflect what the iface is actually using.
    // -------------------------------------------------------------------------

    /// Apply one `/net/ipifc/0/ctl` command. Err on a malformed verb/operand
    /// (the handler maps it to Rlerror(EINVAL)); Ok consumes the whole write.
    ///   add <ip> <mask> [gw]  -- static address (mask = dotted-quad or a /N or
    ///                            bare prefix length); replaces any current addr.
    ///   remove | unbind       -- clear the address + default route.
    /// `bind ether <dev>` is a v1.x seam (the single NIC is bound at probe), so
    /// it (and any other verb) is rejected honestly rather than silently dropped.
    fn ipifc_ctl(&mut self, data: &[u8]) -> Result<(), ()> {
        let mut it = data
            .split(|&b| b == b' ' || b == b'\t' || b == b'\r' || b == b'\n')
            .filter(|t| !t.is_empty());
        let verb = it.next().ok_or(())?;
        match verb {
            b"add" => {
                let ip = parse_ipv4(it.next().ok_or(())?).ok_or(())?;
                let prefix = parse_mask(it.next().ok_or(())?).ok_or(())?;
                let gw = match it.next() {
                    Some(g) => Some(parse_ipv4(g).ok_or(())?),
                    None => None,
                };
                self.ifc_set_static(ip, prefix, gw);
                Ok(())
            }
            b"remove" | b"unbind" => {
                self.ifc_clear();
                Ok(())
            }
            _ => Err(()),
        }
    }

    /// Install a static address onto the iface + the snapshot. The resolver
    /// (ifc.dns) is retained -- a static address change does not by itself
    /// change the DNS server (a v1.x `/net/ndb` write would).
    fn ifc_set_static(&mut self, ip: [u8; 4], prefix: u8, gw: Option<[u8; 4]>) {
        let cidr = Ipv4Cidr::new(Ipv4Address::new(ip[0], ip[1], ip[2], ip[3]), prefix);
        self.iface.update_ip_addrs(|a| {
            a.clear();
            let _ = a.push(IpCidr::Ipv4(cidr));
        });
        let _ = self.iface.routes_mut().remove_default_ipv4_route();
        if let Some(g) = gw {
            let _ = self
                .iface
                .routes_mut()
                .add_default_ipv4_route(Ipv4Address::new(g[0], g[1], g[2], g[3]));
        }
        self.ifc.addr = ip;
        self.ifc.prefix = prefix;
        self.ifc.gw = gw;
        self.ifc.up = true;
        self.ifc.dynamic = false;
    }

    /// Clear the interface address + default route (the `remove`/`unbind` verb).
    fn ifc_clear(&mut self) {
        self.iface.update_ip_addrs(|a| a.clear());
        let _ = self.iface.routes_mut().remove_default_ipv4_route();
        self.ifc.up = false;
        self.ifc.gw = None;
        self.ifc.dynamic = false;
    }

    /// Render `/net/ipifc/0/status` (one `key value` line per fact -- easy to
    /// parse + assert; the Plan 9 ipifc status is terser columns).
    fn push_ifc_status(&self, c: &mut Content) {
        c.push(b"device ether0\n");
        c.push(b"mac ");
        c.push_mac(self.ifc.mac);
        c.push(b"\nmtu ");
        c.push_dec(self.ifc.mtu as u32);
        c.push(b"\n");
        if self.ifc.up {
            c.push(b"addr ");
            c.push_ip(self.ifc.addr);
            c.push(b"/");
            c.push_dec(self.ifc.prefix as u32);
            c.push(b"\n");
            if let Some(gw) = self.ifc.gw {
                c.push(b"gw ");
                c.push_ip(gw);
                c.push(b"\n");
            }
        } else {
            c.push(b"addr none\n");
        }
        if let Some(dns) = self.ifc.dns {
            c.push(b"dns ");
            c.push_ip(dns);
            c.push(b"\n");
        }
        c.push(if self.ifc.dynamic {
            b"mode dhcp\n".as_slice()
        } else if self.ifc.up {
            b"mode static\n".as_slice()
        } else {
            b"mode down\n".as_slice()
        });
    }

    /// Render `/net/ndb` -- the live dynamic network database in ndb(6) format
    /// (the DHCP-learned `ip`/`ipmask`/`ipgw`/`dns` facts, NET-DESIGN section 5's
    /// "dynamic half"). Empty when the interface has no address.
    fn push_ndb(&self, c: &mut Content) {
        if !self.ifc.up {
            return;
        }
        c.push(b"ip=");
        c.push_ip(self.ifc.addr);
        c.push(b" ipmask=");
        c.push_ip(mask_octets(self.ifc.prefix));
        if let Some(gw) = self.ifc.gw {
            c.push(b" ipgw=");
            c.push_ip(gw);
        }
        c.push(b"\n");
        if let Some(dns) = self.ifc.dns {
            c.push(b"\tdns=");
            c.push_ip(dns);
            c.push(b"\n");
        }
    }

    /// UDP `connect` (net-3b): bind a local ephemeral port (smoltcp requires a
    /// bound socket before send/recv) and record the remote for `data` sends. A
    /// datagram socket is connectionless, so this only fixes the default
    /// destination; a re-dial on an already-bound socket just updates the remote.
    fn udp_connect(&mut self, n: u32, ip: [u8; 4], port: u16) -> Result<(), ()> {
        // A 127.x destination migrates this slot's socket onto the loopback stack
        // (net-8a) BEFORE the bind, so the datagram loops in-guest.
        if is_loopback_v4(ip) && !self.ensure_lo_stack(n) {
            return Err(());
        }
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return Err(()),
        };
        let local_port = self.next_local_port;
        // Bind on the slot's stack (lo or NIC) if not already open. Block-scoped so
        // the set_mut borrow ends before we mutate self.slots / self.next_local_port.
        // Some(true)=newly bound, Some(false)=already open, None=bind rejected.
        let bound = {
            let ep = IpListenEndpoint {
                addr: None,
                port: local_port,
            };
            let set = self.set_mut(n);
            if set.get::<udp::Socket>(h).is_open() {
                Some(false)
            } else if set.get_mut::<udp::Socket>(h).bind(ep).is_err() {
                None
            } else {
                Some(true)
            }
        };
        match bound {
            None => {
                self.slots[n as usize].err = Some("udp bind rejected");
                return Err(());
            }
            Some(true) => {
                self.next_local_port = ephemeral_after(local_port);
                let la = self.iface_ipv4_for(n).unwrap_or([0, 0, 0, 0]);
                self.slots[n as usize].local = Some((la, local_port));
            }
            Some(false) => {}
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
        // A 127.x destination migrates this slot's socket onto the loopback stack
        // (net-8a) BEFORE the open, so the connect routes via the lo iface (whose
        // 127.0.0.1/8 is on-link) -- the NIC iface would route 127.x to the default
        // gateway with the wrong source address (net-3d).
        if is_loopback_v4(ip) && !self.ensure_lo_stack(n) {
            return Err(());
        }
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
        let lo = self.slots[n as usize].lo;
        // Connect + read back the tuple on the slot's stack (lo or NIC). Block-
        // scoped so the (set, iface) borrow ends before we mutate self.slots. The
        // socket + the iface context are disjoint fields within each stack.
        let endpoints = {
            let (set, iface): (&mut SocketSet<'static>, &mut Interface) = if lo {
                let l = self.lo.as_mut().unwrap();
                (&mut l.sockets, &mut l.iface)
            } else {
                (&mut self.sockets, &mut self.iface)
            };
            match set
                .get_mut::<tcp::Socket>(h)
                .connect(iface.context(), remote, local)
            {
                Ok(()) => {
                    // connect() set the tuple synchronously: remote is what we
                    // dialed; local is the selected source addr + our ephemeral port.
                    let le = set.get::<tcp::Socket>(h).local_endpoint();
                    let re = set.get::<tcp::Socket>(h).remote_endpoint();
                    Some((le, re))
                }
                Err(_) => None,
            }
        };
        match endpoints {
            Some((le, re)) => {
                self.next_local_port = ephemeral_after(local_port);
                self.slots[n as usize].local = le.map(endpoint_octets);
                self.slots[n as usize].remote = re.map(endpoint_octets);
                self.slots[n as usize].err = None;
                Ok(())
            }
            None => {
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
            Some(PROTO_UDP) => self.set_mut(n).get_mut::<udp::Socket>(h).close(),
            // ICMP is connectionless (no close/unbind on smoltcp's icmp socket):
            // the fid clunk frees the slot + removes the socket. Nothing to do.
            Some(PROTO_ICMP) => {}
            _ => self.set_mut(n).get_mut::<tcp::Socket>(h).close(),
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
        // An announce on an EXPLICIT loopback address migrates the listener onto
        // the lo stack (net-8a), so a 127.x client reaches it. A `*` announce
        // (addr None = any local) stays on the NIC -- a `*` listener does NOT span
        // loopback at v1.0 (a loopback server binds 127.0.0.1 explicitly; the
        // wildcard-spans-both refinement is a v1.x seam).
        if let Some(IpAddress::Ipv4(a)) = ep.addr {
            if is_loopback_v4(a.octets()) && !self.ensure_lo_stack(n) {
                self.slots[n as usize].err = Some("announce rejected");
                return Err(());
            }
        }
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return Err(()),
        };
        let listened = self.set_mut(n).get_mut::<tcp::Socket>(h).listen(ep).is_ok();
        if listened {
            self.slots[n as usize].listen_ep = Some(ep);
            self.slots[n as usize].err = None;
            Ok(())
        } else {
            // Already open/connected, or port 0: cannot listen.
            self.slots[n as usize].err = Some("announce rejected");
            Err(())
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
        // Record the listener's CURRENT mint generation: if the slot is freed +
        // re-minted before the call lands, poll_accepts will see the gen differ
        // and drop the pending rather than type-confuse the re-used index.
        // listening_n is always a live in-range slot here (it is conn_n of a
        // FK_LISTEN fid, parse_dec-bounded < MAX_SLOTS); the else is a fail-safe
        // for an impossible input -- gen 0 is never a live slot's gen, so a stray
        // pending recorded that way is dropped by poll_accepts, never a panic.
        let listening_gen = if (listening_n as usize) < MAX_SLOTS {
            self.slots[listening_n as usize].gen
        } else {
            0
        };
        self.pending.push(PendingAccept {
            conn_id,
            tag,
            fid,
            listening_n,
            listening_gen,
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

    /// Cancel any deferred accept held by `fid` on `conn_id` (the fid is being
    /// clunked). Without this, clunking the half-open listen fid would strand its
    /// PendingAccept -- the entry would survive the fid that owned it, then resolve
    /// against a re-minted slot (net-3d F1). A no-op if the fid holds no pending.
    fn cancel_accept_fid(&mut self, conn_id: i64, fid: u32) {
        self.pending
            .retain(|p| !(p.conn_id == conn_id && p.fid == fid));
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
        // N's stack (lo or NIC): the established + re-armed sockets all live here.
        // M inherits it -- a loopback listener accepts loopback calls (net-8a).
        let nlo = self.slots[n as usize].lo;
        let rx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_RX_BUF]);
        let tx = tcp::SocketBuffer::new(alloc::vec![0u8; TCP_TX_BUF]);
        // Mint the re-arm listener + read the established endpoints on N's set.
        // Block-scoped so the set borrow ends before we mutate self.slots. None ==
        // the re-arm listen failed (the fresh socket is dropped; N keeps the call).
        let swap = {
            let set = if nlo {
                &mut self.lo.as_mut().unwrap().sockets
            } else {
                &mut self.sockets
            };
            let fresh = set.add(tcp::Socket::new(rx, tx));
            if set.get_mut::<tcp::Socket>(fresh).listen(ep).is_err() {
                let _ = set.remove(fresh);
                None
            } else {
                let le = set.get::<tcp::Socket>(established).local_endpoint();
                let re = set.get::<tcp::Socket>(established).remote_endpoint();
                Some((fresh, le, re))
            }
        };
        // Re-arm failed: leave N holding the call so the accept retries next poll
        // (no socket leaked; N still active).
        let (fresh, le, re) = swap?;
        // N takes the fresh listener; M takes the established socket. Accept is
        // TCP-only (UDP never announces), so M is a TCP slot. N keeps its gen (it
        // is the same listener, re-armed); only M, a freshly-minted slot, stamps
        // a new one.
        let mgen = self.next_gen();
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
            icmp_ident: 0,
            gen: mgen,
            lo: nlo,
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
            // The listener must still be the SAME live TCP slot the pending
            // registered against (net-3d F1/F2): the proto must be TCP (else the
            // typed get::<tcp::Socket> below would PANIC on a re-minted UDP/ICMP
            // slot) AND the mint generation must match (else the slot was freed +
            // re-minted out from under a stranded pending). Either mismatch drops
            // the pending. The deferred-listen fid is held busy (opened +
            // clunk-cancel) AND pins N's refcount for the pending's whole
            // lifetime, so a strand cannot arise today -- the proto arm makes the
            // typed get::<tcp::Socket> below locally sound regardless, and the gen
            // arm is the belt against a FUTURE refcount-pin regression.
            if self.slot_proto(pa.listening_n) != Some(PROTO_TCP)
                || self.slots[pa.listening_n as usize].gen != pa.listening_gen
            {
                self.pending.remove(i);
                continue;
            }
            let ready = match self.slot_socket(pa.listening_n) {
                Some(h) => accept_ready(self.set_ref(pa.listening_n).get::<tcp::Socket>(h)),
                None => {
                    // Unreachable after the TCP+gen guard (a live TCP slot always
                    // has a socket); drop defensively so it cannot spin.
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
                let _ = self.set_mut(n).remove(h); // the slot's stack (net-8a)
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
                    .set_mut(n)
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
            Some(PROTO_ICMP) => {
                let (ip, _) = match self.slots[n as usize].remote {
                    Some(r) => r,
                    None => return 0, // no target: `connect` first
                };
                let ident = self.slots[n as usize].icmp_ident;
                let seq = self.icmp_seq;
                self.icmp_seq = self.icmp_seq.wrapping_add(1);
                // Build the echo-request packet from `data` (the payload). The
                // socket re-parses the queued bytes and the iface recomputes the
                // checksum on egress, so emit with default caps; the packet only
                // needs valid EchoRequest structure.
                let repr = Icmpv4Repr::EchoRequest {
                    ident,
                    seq_no: seq,
                    data,
                };
                let mut pkt_buf = alloc::vec![0u8; repr.buffer_len()];
                let mut pkt = Icmpv4Packet::new_unchecked(&mut pkt_buf[..]);
                repr.emit(&mut pkt, &ChecksumCapabilities::default());
                let dst = IpAddress::v4(ip[0], ip[1], ip[2], ip[3]);
                match self
                    .set_mut(n)
                    .get_mut::<icmp::Socket>(h)
                    .send_slice(&pkt_buf, dst)
                {
                    Ok(()) => data.len(),
                    Err(_) => {
                        self.slots[n as usize].err = Some("icmp send failed");
                        0
                    }
                }
            }
            _ => self
                .set_mut(n)
                .get_mut::<tcp::Socket>(h)
                .send_slice(data)
                .unwrap_or(0),
        }
    }

    /// Non-consuming readiness: are bytes ready to recv on connection `n`? (TCP
    /// can_recv / a queued UDP/ICMP packet.) Lets a caller wait for data without
    /// dequeuing it -- used by the net-6a recv-blocking selftest, and the
    /// readiness substrate the net-6b dev9p.poll bridge will query.
    fn slot_can_recv(&self, n: u32) -> bool {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return false,
        };
        let set = self.set_ref(n);
        match self.slot_proto(n) {
            Some(PROTO_UDP) => set.get::<udp::Socket>(h).can_recv(),
            Some(PROTO_ICMP) => set.get::<icmp::Socket>(h).can_recv(),
            _ => set.get::<tcp::Socket>(h).can_recv(),
        }
    }

    /// Non-consuming POLLIN readiness (net-6b): would a `read` on connection `n`
    /// return WITHOUT blocking -- data available OR end-of-stream? The poll twin
    /// of data_recv_outcome (net-6a), but NON-consuming via can_recv + the TCP
    /// state machine. (peek_slice cannot see EOF -- it returns Ok(0) for BOTH an
    /// empty-but-open socket and a drained-FIN one, unlike recv_slice's Finished;
    /// the state is the only non-consuming EOF signal.) A read returns Data when
    /// can_recv, and EOF (recv -> 0, no block) once the recv side is finished --
    /// the peer closed (CloseWait/Closing/LastAck/TimeWait) or the socket is
    /// Closed. While connecting (SynSent/SynReceived) or open with an empty rx
    /// (Established/FinWait, where the peer may still send), a recv WOULD block,
    /// so it is NOT readable. EOF counts as readable so a poller waiting on a peer
    /// disconnect wakes. UDP/ICMP readability is a queued datagram (can_recv).
    fn slot_poll_readable(&self, n: u32) -> bool {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return false,
        };
        let set = self.set_ref(n);
        match self.slot_proto(n) {
            Some(PROTO_UDP) => set.get::<udp::Socket>(h).can_recv(),
            Some(PROTO_ICMP) => set.get::<icmp::Socket>(h).can_recv(),
            _ => {
                let s = set.get::<tcp::Socket>(h);
                if s.can_recv() {
                    return true; // buffered data -> a read returns it (Data)
                }
                // No data buffered: readable iff a recv would return EOF (not
                // block). These terminal/closing states have a finished recv
                // side; Established/FinWait may still receive (block), and a
                // connecting socket's recv is InvalidState (block).
                matches!(
                    s.state(),
                    tcp::State::CloseWait
                        | tcp::State::Closing
                        | tcp::State::LastAck
                        | tcp::State::TimeWait
                        | tcp::State::Closed
                )
            }
        }
    }

    /// Non-consuming POLLOUT readiness (net-6b): can connection `n` accept bytes
    /// to send right now? TCP can_send (established with send-buffer room);
    /// UDP/ICMP are writable whenever the socket is open (datagram sends never
    /// block in smoltcp). A connecting/closed TCP socket is not writable.
    fn slot_can_send(&self, n: u32) -> bool {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return false,
        };
        let set = self.set_ref(n);
        match self.slot_proto(n) {
            Some(PROTO_UDP) => set.get::<udp::Socket>(h).is_open(),
            Some(PROTO_ICMP) => set.get::<icmp::Socket>(h).is_open(),
            _ => set.get::<tcp::Socket>(h).can_send(),
        }
    }

    /// Non-consuming POLLHUP readiness (net-6b; an always-reported output-only
    /// bit): the TCP connection is fully closed (the socket left the active set
    /// for good). A peer half-close (CloseWait) is NOT a HUP -- it reads EOF
    /// (POLLIN via slot_poll_readable) while the local side can still send; HUP
    /// fires once the socket is Closed. UDP/ICMP are connectionless -> never HUP.
    fn slot_is_hup(&self, n: u32) -> bool {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return false,
        };
        match self.slot_proto(n) {
            Some(PROTO_TCP) => !self.set_ref(n).get::<tcp::Socket>(h).is_open(),
            _ => false,
        }
    }

    /// The net-6b readiness computation: the satisfied poll `revents` for
    /// connection `n` against the requested `mask`. POLLIN/POLLOUT are gated by
    /// the request (so a poll(POLLIN) on a writable-but-empty socket reports
    /// nothing -> DEFER, never a busy loop); POLLERR/POLLHUP are always reported
    /// (the output-only bits). A non-zero result satisfies the read at once;
    /// zero means DEFER (park a PendingReady).
    fn check_ready(&self, n: u32, mask: u16) -> u16 {
        let mut revents: u16 = 0;
        if mask & POLLIN != 0 && self.slot_poll_readable(n) {
            revents |= POLLIN;
        }
        if mask & POLLOUT != 0 && self.slot_can_send(n) {
            revents |= POLLOUT;
        }
        if self.slot_err(n).is_some() {
            revents |= POLLERR;
        }
        if self.slot_is_hup(n) {
            revents |= POLLHUP;
        }
        revents
    }

    /// Dequeue up to `out.len()` bytes from the connection, distinguishing "no
    /// data yet" (WouldBlock -> the caller may defer = block) from end-of-stream
    /// (Eof -> the read returns 0). TCP dequeues from the byte stream; UDP/ICMP
    /// dequeue one whole datagram/packet (the sender endpoint is dropped -- the
    /// connected client knows its remote). The mapping is grounded in smoltcp
    /// 0.12 (recv_error_check, tcp.rs): may_recv-false + rx_fin_received =>
    /// Finished (EOF); may_recv-false + !fin => InvalidState (connecting if
    /// is_active, else closed). Non-blocking at the smoltcp layer; the BLOCKING
    /// semantics live in h_read/poll_data (net-6a), which park a PendingRead on
    /// WouldBlock.
    fn data_recv_outcome(&mut self, n: u32, out: &mut [u8]) -> RecvOutcome {
        let h = match self.slot_socket(n) {
            Some(h) => h,
            None => return RecvOutcome::Eof, // no socket: nothing will ever arrive
        };
        match self.slot_proto(n) {
            Some(PROTO_UDP) => match self.set_mut(n).get_mut::<udp::Socket>(h).recv_slice(out) {
                Ok((k, _meta)) => RecvOutcome::Data(k), // a datagram (k may be 0)
                Err(udp::RecvError::Exhausted) => RecvOutcome::WouldBlock, // none queued
                // Truncated: the datagram exceeded `out` (unreachable with the
                // DATA_CHUNK buffer); deliver an empty read (v1.x: large datagrams).
                Err(_) => RecvOutcome::Data(0),
            },
            Some(PROTO_ICMP) => {
                // recv_slice gives a whole ICMP packet (smoltcp already filtered
                // it to this socket's bound ident + verified the checksum). On an
                // EchoReply, copy the payload. A non-reply / parse error consumed
                // a non-matching packet -> WouldBlock (keep waiting for the reply;
                // ICMP is connectionless -- a blocked ping read never EOFs, it
                // waits for a reply or the fid's clunk/flush).
                let mut pkt_buf = [0u8; ICMP_RX_BUF];
                let k = match self
                    .set_mut(n)
                    .get_mut::<icmp::Socket>(h)
                    .recv_slice(&mut pkt_buf)
                {
                    Ok((k, _from)) => k,
                    Err(icmp::RecvError::Exhausted) => return RecvOutcome::WouldBlock,
                    Err(_) => return RecvOutcome::WouldBlock,
                };
                let pkt = match Icmpv4Packet::new_checked(&pkt_buf[..k]) {
                    Ok(p) => p,
                    Err(_) => return RecvOutcome::WouldBlock,
                };
                match Icmpv4Repr::parse(&pkt, &ChecksumCapabilities::ignored()) {
                    Ok(Icmpv4Repr::EchoReply { data, .. }) => {
                        let m = data.len().min(out.len());
                        out[..m].copy_from_slice(&data[..m]);
                        RecvOutcome::Data(m)
                    }
                    _ => RecvOutcome::WouldBlock,
                }
            }
            _ => {
                let sock = self.set_mut(n).get_mut::<tcp::Socket>(h);
                match sock.recv_slice(out) {
                    Ok(0) => RecvOutcome::WouldBlock, // established/half-open, rx empty
                    Ok(k) => RecvOutcome::Data(k),
                    Err(tcp::RecvError::Finished) => RecvOutcome::Eof, // peer FIN + drained
                    // Not established: connecting (is_active: SynSent/SynReceived)
                    // -> WouldBlock; otherwise closed/aborted -> Eof.
                    Err(tcp::RecvError::InvalidState) => {
                        if sock.is_active() {
                            RecvOutcome::WouldBlock
                        } else {
                            RecvOutcome::Eof
                        }
                    }
                }
            }
        }
    }

    /// Non-blocking dequeue: bytes available now, 0 if none / EOF. The
    /// best-effort net-3 demos + the loopback E2E use this; the blocking 9P
    /// `data` read path uses data_recv_outcome directly (net-6a).
    fn data_recv(&mut self, n: u32, out: &mut [u8]) -> usize {
        match self.data_recv_outcome(n, out) {
            RecvOutcome::Data(k) => k,
            RecvOutcome::WouldBlock | RecvOutcome::Eof => 0,
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
            return if self.set_ref(n).get::<udp::Socket>(h).is_open() {
                "Open"
            } else {
                "Closed"
            };
        }
        if self.slot_proto(n) == Some(PROTO_ICMP) {
            return if self.set_ref(n).get::<icmp::Socket>(h).is_open() {
                "Open"
            } else {
                "Closed"
            };
        }
        match self.set_ref(n).get::<tcp::Socket>(h).state() {
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
            matches!(path, P_ROOT | P_TCP | P_UDP | P_ICMP | P_IPIFC | P_IPIFC_0)
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
                P_TCP | P_UDP | P_ICMP | P_CS | P_DNS | P_IPIFC | P_NDB | P_SUMMARY => P_ROOT,
                P_TCP_CLONE | P_TCP_STATS => P_TCP,
                P_UDP_CLONE | P_UDP_STATS => P_UDP,
                P_ICMP_CLONE | P_ICMP_STATS => P_ICMP,
                P_IPIFC_0 => P_IPIFC,
                P_IPIFC_0_CTL | P_IPIFC_0_STATUS | P_IPIFC_0_LOCAL => P_IPIFC_0,
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
                b"ready" => FK_READY, // the dev9p.poll readiness file (net-6b)
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
                b"cs" => Some(P_CS),
                b"dns" => Some(P_DNS),
                b"ipifc" => Some(P_IPIFC),
                b"ndb" => Some(P_NDB),
                b"summary" => Some(P_SUMMARY),
                _ => None,
            },
            P_IPIFC => match name {
                b"0" => Some(P_IPIFC_0), // the single interface (the NIC)
                _ => None,
            },
            P_IPIFC_0 => match name {
                b"ctl" => Some(P_IPIFC_0_CTL),
                b"status" => Some(P_IPIFC_0_STATUS),
                b"local" => Some(P_IPIFC_0_LOCAL),
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
                b"clone" => Some(P_ICMP_CLONE),
                b"stats" => Some(P_ICMP_STATS),
                _ => parse_dec(name)
                    .filter(|&n| self.slot_proto(n) == Some(PROTO_ICMP))
                    .map(|n| make_conn(PROTO_ICMP, n, FK_DIR)),
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
            push(b"ready", make_conn(proto, n, FK_READY), false); // net-6b dev9p.poll
            return;
        }
        match dir {
            P_ROOT => {
                push(b"tcp", P_TCP, true);
                push(b"udp", P_UDP, true);
                push(b"icmp", P_ICMP, true);
                push(b"cs", P_CS, false);
                push(b"dns", P_DNS, false);
                push(b"ipifc", P_IPIFC, true);
                push(b"ndb", P_NDB, false);
                push(b"summary", P_SUMMARY, false); // net-7b: the observability rollup
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
            P_ICMP => {
                push(b"clone", P_ICMP_CLONE, false);
                push(b"stats", P_ICMP_STATS, false);
                self.push_conn_slots(PROTO_ICMP, &mut push);
            }
            P_IPIFC => {
                push(b"0", P_IPIFC_0, true);
            }
            P_IPIFC_0 => {
                push(b"ctl", P_IPIFC_0_CTL, false);
                push(b"status", P_IPIFC_0_STATUS, false);
                push(b"local", P_IPIFC_0_LOCAL, false);
            }
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
                        // ICMP is portless: report just the address.
                        if self.slot_proto(n) == Some(PROTO_ICMP) {
                            c.push_ip(ip);
                        } else {
                            c.push_endpoint(ip, port);
                        }
                    }
                }
                FK_REMOTE => {
                    if let Some((ip, port)) = self.slot_endpoint(n, false) {
                        if self.slot_proto(n) == Some(PROTO_ICMP) {
                            c.push_ip(ip);
                        } else {
                            c.push_endpoint(ip, port);
                        }
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
            P_ICMP_STATS => {
                c.push(b"icmp\n  active ");
                c.push_dec(self.icmp_active);
                c.push(b"\n  opened ");
                c.push_dec(self.icmp_opened);
                c.push(b"\n");
            }
            // net-4c: the interface-config + dynamic-database views.
            P_IPIFC_0_CTL => c.push(b"0\n"), // the interface number (Plan 9 idiom)
            P_IPIFC_0_STATUS => self.push_ifc_status(&mut c),
            P_IPIFC_0_LOCAL => {
                if self.ifc.up {
                    c.push_ip(self.ifc.addr);
                    c.push(b"/");
                    c.push_dec(self.ifc.prefix as u32);
                    c.push(b"\n");
                } else {
                    c.push(b"none\n");
                }
            }
            P_NDB => self.push_ndb(&mut c),
            // clone is never read as content (its open rebinds the fid to ctl).
            _ => {}
        }
        c
    }

    /// The /net/summary rollup (NET-DESIGN section 11): a read-only one-shot
    /// aggregate of the interface view + the per-protocol stats + the live
    /// connection table, rendered server-side (netd holds the connection table;
    /// the kernel holds no network state). Built fresh per read into a growable
    /// Vec, since a multi-connection table exceeds the fixed `Content` cap. It
    /// reports only THIS netd's /net -- per-territory by construction, visibility
    /// not authority (it mints nothing; every datum is already readable via the
    /// per-protocol `stats` + the per-connection `status`/`local`/`remote`).
    fn render_summary(&self) -> Vec<u8> {
        let mut out: Vec<u8> = Vec::new();
        out.extend_from_slice(b"ipifc\n");
        let mut c = Content::new();
        self.push_ifc_status(&mut c);
        out.extend_from_slice(c.as_slice());
        for path in [P_TCP_STATS, P_UDP_STATS, P_ICMP_STATS] {
            out.extend_from_slice(self.file_content(path).as_slice());
        }
        out.extend_from_slice(b"conn\n");
        for i in 0..MAX_SLOTS {
            if !self.slots[i].used {
                continue;
            }
            let n = i as u32;
            let proto = self.slots[i].proto;
            // One line per live connection, mirroring the FK_LOCAL/FK_REMOTE/
            // FK_STATUS renderers a per-connection read would return.
            let mut line = Content::new();
            line.push(proto_name(proto));
            line.push(b" ");
            line.push_dec(n);
            line.push(b" ");
            if let Some((ip, port)) = self.slot_endpoint(n, true) {
                if proto == PROTO_ICMP {
                    line.push_ip(ip);
                } else {
                    line.push_endpoint(ip, port);
                }
            } else {
                line.push(b"-");
            }
            line.push(b" ");
            if let Some((ip, port)) = self.slot_endpoint(n, false) {
                if proto == PROTO_ICMP {
                    line.push_ip(ip);
                } else {
                    line.push_endpoint(ip, port);
                }
            } else {
                line.push(b"-");
            }
            line.push(b" ");
            line.push(self.state_str(n).as_bytes());
            line.push(b"\n");
            out.extend_from_slice(line.as_slice());
        }
        out
    }
}

/// The outcome of the net-3d loopback E2E (each leg deterministic, in-guest).
pub struct LoopbackResult {
    pub icmp: bool,
    pub udp: bool,
    pub tcp: bool,
}

impl LoopbackResult {
    pub fn all_ok(&self) -> bool {
        self.icmp && self.udp && self.tcp
    }
}

/// net-3d: the DETERMINISTIC in-guest loopback E2E. Builds an ISOLATED loopback
/// stack -- a smoltcp `Loopback` device (TX queue -> own RX) + an `Interface`
/// (127.0.0.1/8) + its OWN `SocketSet` -- and runs three round-trips through the
/// REAL `Net` methods (the proto-dispatch, the data path, and the net-3a
/// deferred-accept `poll_accepts`/`accept_swap`). It is ISOLATED on purpose: a
/// loopback iface sharing the live NIC `SocketSet` mis-routes (the NIC's default
/// route steals the 127.0.0.1 egress -- proven from smoltcp source), so the
/// self-test runs over a dedicated `Net`. No NIC, no host -> fully deterministic,
/// so the caller ASSERTS it (a PASS/FAIL boot line), unlike the host-coupled
/// gateway/DNS probes. Runs once at bring-up, then the lo `Net` is dropped.
pub fn loopback_e2e(base: Instant) -> LoopbackResult {
    // Ethernet medium (netd builds with medium-ethernet, like the NIC): the
    // loopback ARPs for 127.0.0.1 (its own addr) and the request loops straight
    // back to a reply, so the handshake completes -- the smoltcp loopback example
    // shape. A locally-administered dummy MAC (02:..).
    let mut device = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x01,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let mut iface = Interface::new(config, &mut device, ts0);
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
    });
    let mut lo = Net::new(iface, SocketSet::new(Vec::new()), base, IfConfig::empty());
    LoopbackResult {
        icmp: lo_icmp_roundtrip(&mut lo, &mut device),
        udp: lo_udp_roundtrip(&mut lo, &mut device),
        tcp: lo_tcp_accept(&mut lo, &mut device),
    }
}

/// net-8a: a DETERMINISTIC proof of the RESIDENT loopback interface -- the
/// dual-stack the live netd runs, NOT an isolated single-stack lo Net like
/// loopback_e2e. The primary "NIC" stack is addressed 10.0.0.1/24 (NOT 127), so a
/// 127.x dial CANNOT route there: it MUST migrate to the resident lo stack to
/// work. A successful announce-127 + connect-127 + accept + data round-trip
/// therefore PROVES ensure_lo_stack migration + the dual-poll (net.poll services
/// BOTH the primary device AND the owned lo device) + clean teardown (the
/// connection table returns to baseline -- no leak). The caller ASSERTS the
/// returned PASS/stage string (no NIC, no host -> fully deterministic).
pub fn resident_lo_selftest(base: Instant) -> &'static str {
    let mut nic = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let mut iface = Interface::new(config, &mut nic, ts0);
    // A non-loopback primary address: 127.x has no route here -> a 127.x dial only
    // succeeds via the resident lo stack (the whole point of the proof).
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::v4(10, 0, 0, 1), 24));
    });
    let mut net = Net::new(iface, SocketSet::new(Vec::new()), base, IfConfig::empty());
    net.enable_loopback();

    // announce 127.0.0.1!port -> the listener migrates to the lo stack.
    let ln = match net.tcp_clone() {
        Some(n) => n,
        None => return "clone-listen",
    };
    net.slot_ref(ln);
    let ep = IpListenEndpoint {
        addr: Some(IpAddress::v4(127, 0, 0, 1)),
        port: LO_LOOPBACK_PORT,
    };
    if net.ctl_announce(ln, ep).is_err() {
        return "announce";
    }
    if !net.slot_on_lo(ln) {
        return "listener-not-migrated";
    }

    // connect 127.0.0.1!port -> the client migrates to the lo stack.
    let cn = match net.tcp_clone() {
        Some(n) => n,
        None => return "clone-conn",
    };
    net.slot_ref(cn);
    if net
        .ctl_connect(cn, [127, 0, 0, 1], LO_LOOPBACK_PORT)
        .is_err()
    {
        return "connect";
    }
    if !net.slot_on_lo(cn) {
        return "client-not-migrated";
    }

    // Drive the deferred accept over the dual-poll, KEEPING the server side M so we
    // can recv on it (synthetic conn_id/tag/fid = 0, like lo_tcp_accept).
    net.register_accept(0, 0, 0, ln);
    let mut mslot: Option<u32> = None;
    let accepted = lo_drive(&mut net, &mut nic, |net| {
        let done = net.poll_accepts();
        if done.is_empty() {
            return false;
        }
        for d in done {
            mslot = Some(d.new_n);
            net.slot_ref(d.new_n); // own M (the serve loop's complete_accept would)
        }
        true
    });
    if !accepted {
        return "accept";
    }
    let m = match mslot {
        Some(m) => m,
        None => return "no-server-slot",
    };
    if !net.slot_on_lo(m) {
        return "server-not-migrated";
    }

    // client -> server data over the loopback (the live data path: data_send +
    // dual-poll + data_recv_outcome).
    let payload = b"net8a-lo";
    if net.data_send(cn, payload) != payload.len() {
        return "send";
    }
    let mut buf = [0u8; 32];
    let mut got = 0usize;
    let received = lo_drive(&mut net, &mut nic, |net| {
        match net.data_recv_outcome(m, &mut buf) {
            RecvOutcome::Data(k) if k > 0 => {
                got = k;
                true
            }
            _ => false,
        }
    });
    if !received {
        return "recv";
    }
    if &buf[..got] != payload {
        return "data-mismatch";
    }

    // Teardown -> the connection table returns to baseline (no fd/socket leak).
    net.slot_unref(cn);
    net.slot_unref(ln);
    net.slot_unref(m);
    if net.tcp_active != 0 {
        return "leak";
    }
    "PASS"
}

/// net-6a-3: a DETERMINISTIC in-guest E2E of the >=2-concurrent TCP echo SERVER
/// LOGIC -- the accept + bidirectional data path netd serves, which the native
/// `net::TcpListener` echo server drives. Over an ISOLATED 127.0.0.1 loopback
/// stack (the net-3d pattern): announce ONE listener, connect TWO clients,
/// accept BOTH (the real `poll_accepts`/`accept_swap`), then each server
/// connection echoes its client's payload and each client verifies it gets its
/// OWN bytes back -- so the proof is independent of the accept order / the
/// client<->server pairing (each server connection is bound to exactly one
/// client and echoes that client's bytes). No NIC, no host -> the caller
/// ASSERTS it. The live cross-Proc native-API round-trip is net-8's (it owns the
/// in-guest peer mechanism netd's NIC-only live stack lacks).
pub fn echo_e2e(base: Instant) -> bool {
    let mut device = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x03,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let mut iface = Interface::new(config, &mut device, ts0);
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
    });
    let mut lo = Net::new(iface, SocketSet::new(Vec::new()), base, IfConfig::empty());
    echo_e2e_inner(&mut lo, &mut device)
}

fn echo_e2e_inner(lo: &mut Net, device: &mut Loopback) -> bool {
    let port = LO_LOOPBACK_PORT;
    let ln = match lo.tcp_clone() {
        Some(n) => n,
        None => return false,
    };
    let ep = IpListenEndpoint { addr: None, port };
    if lo.ctl_announce(ln, ep).is_err() {
        lo.free_orphan_mint(ln);
        return false;
    }
    // netd's listener backlog is 1 (net-3d): a 2nd SYN arriving before the first
    // is accepted + the listener re-armed is RST'd. So accept the two clients
    // SEQUENTIALLY -- connect, accept (the listener re-arms), then the next
    // connects. Both accepted connections then stay open simultaneously, which
    // is exactly what ">=2 concurrent connections" means (accept() inherently
    // returns one connection at a time). A backlog > 1 is a documented netd
    // seam (net-8).
    let (cn1, m1) = match lo_connect_accept(lo, device, ln, port) {
        Some(p) => p,
        None => {
            lo.free_orphan_mint(ln);
            return false;
        }
    };
    let (cn2, m2) = match lo_connect_accept(lo, device, ln, port) {
        Some(p) => p,
        None => {
            for n in [m1, cn1, ln] {
                lo.free_orphan_mint(n);
            }
            return false;
        }
    };

    // Both connections are now concurrently ESTABLISHED. Each client sends a
    // DISTINCT payload; each server echoes whatever it received; each client
    // must read back exactly its OWN payload (so the proof is independent of the
    // accept order / the client<->server pairing).
    let p1: &[u8] = b"echo-payload-alpha";
    let p2: &[u8] = b"echo-payload-bravo";
    let mut got1 = [0u8; 64];
    let mut got2 = [0u8; 64];
    let verified = lo.data_send(cn1, p1) == p1.len()
        && lo.data_send(cn2, p2) == p2.len()
        && lo_echo_one(lo, device, m1)
        && lo_echo_one(lo, device, m2)
        && lo_recv_n(lo, device, cn1, p1.len(), &mut got1) == p1.len()
        && lo_recv_n(lo, device, cn2, p2.len(), &mut got2) == p2.len()
        && &got1[..p1.len()] == p1
        && &got2[..p2.len()] == p2;

    for n in [m1, m2, cn1, cn2, ln] {
        lo.free_orphan_mint(n);
    }
    verified
}

/// Dial the loopback listener `ln` with a fresh client and drive until the
/// inbound call is accepted (the real `poll_accepts`/`accept_swap`). Returns the
/// `(client_n, accepted_server_n)` pair, or `None` (freeing the client) on
/// failure. The listener re-arms after the accept, so a subsequent call accepts
/// the next connection.
fn lo_connect_accept(
    lo: &mut Net,
    device: &mut Loopback,
    ln: u32,
    port: u16,
) -> Option<(u32, u32)> {
    let cn = lo.tcp_clone()?;
    if lo.tcp_connect(cn, [127, 0, 0, 1], port).is_err() {
        lo.free_orphan_mint(cn);
        return None;
    }
    lo.register_accept(0, 0, 0, ln);
    let mut m: Option<u32> = None;
    lo_drive(lo, device, |lo| {
        for d in lo.poll_accepts() {
            m = Some(d.new_n);
        }
        m.is_some()
    });
    match m {
        Some(mm) => Some((cn, mm)),
        None => {
            lo.free_orphan_mint(cn);
            None
        }
    }
}

/// Read whatever a just-accepted server connection received and write it
/// straight back (the echo). Bounded by the loopback drive; the small loopback
/// payload arrives in one segment.
fn lo_echo_one(lo: &mut Net, device: &mut Loopback, m: u32) -> bool {
    let mut buf = [0u8; 64];
    let mut got = 0usize;
    let ok = lo_drive(lo, device, |lo| {
        let k = lo.data_recv(m, &mut buf);
        if k > 0 {
            got = k;
            true
        } else {
            false
        }
    });
    ok && got > 0 && lo.data_send(m, &buf[..got]) == got
}

/// Drive until `want` bytes have accumulated from connection `n` into `out`
/// (across however many segments smoltcp delivers); returns the byte count.
fn lo_recv_n(lo: &mut Net, device: &mut Loopback, n: u32, want: usize, out: &mut [u8]) -> usize {
    let mut got = 0usize;
    lo_drive(lo, device, |lo| {
        if got < out.len() {
            got += lo.data_recv(n, &mut out[got..]);
        }
        got >= want
    });
    got
}

/// net-4c: a DETERMINISTIC in-guest self-test of the interface-config path
/// (the `/net/ipifc/0/ctl` add/remove verbs + the status/local/ndb renders),
/// run on a THROWAWAY Net so it never disturbs the live (DHCP-leased) config.
/// Exercises: a static `add` (the iface + IfConfig mutation), the status/ndb/
/// local content (the lease-into-view render in reverse -- a static fact in,
/// the rendered bytes out), `remove`, and the malformed-input rejects. No NIC,
/// no host -> fully deterministic, so the caller ASSERTS it (a PASS/FAIL line).
pub fn ipifc_e2e(base: Instant) -> bool {
    let mut device = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x02,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let iface = Interface::new(config, &mut device, ts0);
    let mut net = Net::new(iface, SocketSet::new(Vec::new()), base, IfConfig::empty());

    let has = |c: &Content, needle: &[u8]| {
        let s = c.as_slice();
        needle.len() <= s.len() && s.windows(needle.len()).any(|w| w == needle)
    };

    // add: static address + mask (dotted-quad) + gateway.
    if net
        .ipifc_ctl(b"add 192.168.7.5 255.255.255.0 192.168.7.1")
        .is_err()
    {
        return false;
    }
    let st = net.file_content(P_IPIFC_0_STATUS);
    if !has(&st, b"addr 192.168.7.5/24")
        || !has(&st, b"gw 192.168.7.1")
        || !has(&st, b"mode static")
    {
        return false;
    }
    let ndb = net.file_content(P_NDB);
    if !has(&ndb, b"ip=192.168.7.5")
        || !has(&ndb, b"ipmask=255.255.255.0")
        || !has(&ndb, b"ipgw=192.168.7.1")
    {
        return false;
    }
    let local = net.file_content(P_IPIFC_0_LOCAL);
    if local.as_slice() != b"192.168.7.5/24\n" {
        return false;
    }

    // A bare-prefix mask resolves identically (the /N + N forms).
    if net.ipifc_ctl(b"add 10.1.2.3 16").is_err() {
        return false;
    }
    let st2 = net.file_content(P_IPIFC_0_STATUS);
    if !has(&st2, b"addr 10.1.2.3/16") {
        return false;
    }
    // The prior gateway is cleared by the gateway-less add (no stale route).
    if has(&st2, b"gw ") {
        return false;
    }

    // remove: the address + route go away; the ndb dynamic half empties.
    if net.ipifc_ctl(b"remove").is_err() {
        return false;
    }
    let st3 = net.file_content(P_IPIFC_0_STATUS);
    if !has(&st3, b"addr none") || !has(&st3, b"mode down") {
        return false;
    }
    if net.file_content(P_IPIFC_0_LOCAL).as_slice() != b"none\n" {
        return false;
    }
    if !net.file_content(P_NDB).as_slice().is_empty() {
        return false;
    }

    // Malformed inputs fail closed (a bad octet, a non-contiguous mask, an
    // unknown verb, a missing operand) -- never silently accepted.
    if net.ipifc_ctl(b"add 999.1.1.1 255.255.255.0").is_ok()
        || net.ipifc_ctl(b"add 10.0.0.1 255.0.255.0").is_ok()
        || net.ipifc_ctl(b"add 10.0.0.1").is_ok()
        || net.ipifc_ctl(b"bogus").is_ok()
        || net.ipifc_ctl(b"").is_ok()
    {
        return false;
    }
    true
}

/// Build a minimal DNS A-record response for a received query (the net-4d
/// loopback responder): echo the transaction id + the single question, set the
/// response flags, and append one A answer (a compression-pointer name -> the
/// question + the given IP). Empty on a malformed/short query (the dns socket
/// then ignores it). Parses the qname explicitly so a trailing EDNS OPT in the
/// query's additional section is not echoed as part of the question.
fn build_dns_response(query: &[u8], ip: [u8; 4]) -> Vec<u8> {
    if query.len() < 12 {
        return Vec::new();
    }
    // Walk the qname labels to the 0x00 root, then +4 for qtype + qclass.
    let mut p = 12usize;
    loop {
        match query.get(p) {
            None => return Vec::new(),
            Some(0) => {
                p += 1;
                break;
            }
            Some(&len) if len >= 0xc0 => return Vec::new(), // a pointer in a question
            Some(&len) => p += 1 + len as usize,
        }
    }
    let qend = p + 4; // qtype(2) + qclass(2)
    if qend > query.len() {
        return Vec::new();
    }
    let mut r = Vec::new();
    r.extend_from_slice(&query[0..2]); // id (echo)
    r.extend_from_slice(&[0x81, 0x80]); // flags: QR=1 RD=1 RA=1 rcode=0
    r.extend_from_slice(&[0x00, 0x01]); // qdcount = 1
    r.extend_from_slice(&[0x00, 0x01]); // ancount = 1
    r.extend_from_slice(&[0x00, 0x00]); // nscount = 0
    r.extend_from_slice(&[0x00, 0x00]); // arcount = 0
    r.extend_from_slice(&query[12..qend]); // the question (echo)
    r.extend_from_slice(&[0xc0, 0x0c]); // answer name: pointer -> offset 12
    r.extend_from_slice(&[0x00, 0x01]); // type A
    r.extend_from_slice(&[0x00, 0x01]); // class IN
    r.extend_from_slice(&[0x00, 0x00, 0x00, 0x3c]); // ttl 60
    r.extend_from_slice(&[0x00, 0x04]); // rdlength 4
    r.extend_from_slice(&ip); // the A record
    r
}

/// net-4d: a DETERMINISTIC in-guest E2E of the net-4b DNS resolution path -- a
/// mock DNS responder on an ISOLATED 127.0.0.1 loopback stack answers a fixed A
/// query, so the shared dns::Socket resolves a name in-guest with NO host
/// coupling (the net-3d loopback analog; closes the net-4b OWED deferred-read
/// E2E). The resolver IS our own responder bound to 127.0.0.1:53; the round-trip
/// drives the REAL Net dns methods (dns_query_start -> poll -> dns_poll) -- the
/// same methods a /net/dns fid's query_read/poll_dns drive -- so the PASS proves
/// the resolution plumbing resolves deterministically.
pub fn dns_loopback_e2e(base: Instant) -> bool {
    let mut device = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x04,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let mut iface = Interface::new(config, &mut device, ts0);
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
    });
    // The resolver IS our loopback responder (127.0.0.1).
    let mut ifc = IfConfig::empty();
    ifc.dns = Some([127, 0, 0, 1]);
    let mut net = Net::new(iface, SocketSet::new(Vec::new()), base, ifc);

    // The mock resolver: a udp socket bound to 127.0.0.1:53 that answers any A
    // query with `answer`. On the same iface + SocketSet, so the loopback device
    // loops the dns query to it and its reply back (the lo_udp_roundtrip path).
    let rx = udp::PacketBuffer::new(
        alloc::vec![udp::PacketMetadata::EMPTY; 4],
        alloc::vec![0u8; 1024],
    );
    let tx = udp::PacketBuffer::new(
        alloc::vec![udp::PacketMetadata::EMPTY; 4],
        alloc::vec![0u8; 1024],
    );
    let mut responder = udp::Socket::new(rx, tx);
    if responder
        .bind(IpListenEndpoint {
            addr: None,
            port: 53,
        })
        .is_err()
    {
        return false;
    }
    let rh = net.sockets.add(responder);
    let answer = [203, 0, 113, 7]; // TEST-NET-3: a fixed, deterministic A record.

    let q = match net.dns_query_start(b"lo.thylacine") {
        Some(q) => q,
        None => return false,
    };

    for _ in 0..LO_POLLS {
        net.poll(&mut device);
        // The responder: answer any query that has landed on :53.
        let mut qbuf = [0u8; 512];
        let got = match net.sockets.get_mut::<udp::Socket>(rh).recv_slice(&mut qbuf) {
            Ok((n, meta)) => Some((n, meta.endpoint)),
            Err(_) => None,
        };
        if let Some((n, dst)) = got {
            let resp = build_dns_response(&qbuf[..n], answer);
            if !resp.is_empty() {
                let _ = net
                    .sockets
                    .get_mut::<udp::Socket>(rh)
                    .send_slice(&resp, dst);
            }
        }
        net.poll(&mut device); // flush the response back to the dns socket
        match net.dns_poll(q) {
            DnsPoll::Pending => {}
            DnsPoll::Resolved(ip) => return ip == answer, // slot freed; no cancel
            DnsPoll::Failed => return false,              // slot freed; no cancel
        }
        let _ = sleep(Duration::from_millis(LO_POLL_MS));
    }
    net.dns_cancel(q); // timed out still pending: free the slot
    false
}

/// net-4d: a DETERMINISTIC in-guest battery over the pure cs/dns/ndb/mask
/// parsers (the parser COVERAGE the OWED-since-net-2d host-test module would
/// give -- delivered in-guest, since netd is a no_std + aarch64 bin crate that
/// does not host-`cargo test`). Each malformed input must fail closed; each
/// valid input must resolve as documented.
pub fn proto_selftest() -> bool {
    // cs_parse: proto -> clone-file, service -> port (numeric or ndb), host left.
    if !matches!(
        cs_parse(b"tcp!1.2.3.4!80"),
        Some((
            QueryKind::Cs {
                clone_tcp: true,
                port: 80
            },
            b"1.2.3.4"
        ))
    ) || !matches!(
        cs_parse(b"net!example!http"), // net -> tcp; http -> ndb port 80
        Some((
            QueryKind::Cs {
                clone_tcp: true,
                port: 80
            },
            b"example"
        ))
    ) || !matches!(
        cs_parse(b"udp!h!53"),
        Some((
            QueryKind::Cs {
                clone_tcp: false,
                port: 53
            },
            b"h"
        ))
    ) {
        return false;
    }
    // cs_parse rejects: >3 fields, unknown proto, empty, missing host/service.
    if cs_parse(b"tcp!h!80!x").is_some()
        || cs_parse(b"sctp!h!80").is_some()
        || cs_parse(b"").is_some()
        || cs_parse(b"tcp").is_some()
        || cs_parse(b"tcp!h!nosuchsvc").is_some()
    {
        return false;
    }
    // dns_parse: bare name; A-type variants accepted (case-insensitive); else None.
    if !matches!(dns_parse(b"example.com"), Some(b"example.com"))
        || !matches!(dns_parse(b"host a"), Some(b"host"))
        || !matches!(dns_parse(b"host ipv4"), Some(b"host"))
        || !matches!(dns_parse(b"host IP"), Some(b"host"))
        || dns_parse(b"host aaaa").is_some()
        || dns_parse(b"").is_some()
    {
        return false;
    }
    // parse_mask: dotted (contiguous only) / /N / bare N; non-contiguous + >32 reject.
    if parse_mask(b"255.255.255.0") != Some(24)
        || parse_mask(b"/16") != Some(16)
        || parse_mask(b"8") != Some(8)
        || parse_mask(b"0.0.0.0") != Some(0)
        || parse_mask(b"255.255.255.255") != Some(32)
        || parse_mask(b"255.0.255.0").is_some() // a hole -> rejected
        || parse_mask(b"33").is_some()
        || parse_mask(b"/40").is_some()
    {
        return false;
    }
    if mask_octets(24) != [255, 255, 255, 0]
        || mask_octets(0) != [0, 0, 0, 0]
        || mask_octets(32) != [255, 255, 255, 255]
        || mask_octets(16) != [255, 255, 0, 0]
    {
        return false;
    }
    // ndb: a known host/service hits; an unknown misses.
    crate::ndb::lookup_host(b"localhost") == Some([127, 0, 0, 1])
        && crate::ndb::lookup_host(b"nonesuch.invalid").is_none()
        && crate::ndb::lookup_service(b"http") == Some(80)
        && crate::ndb::lookup_service(b"nosuchsvc").is_none()
}

/// net-4d: a DETERMINISTIC in-guest regression for the F1 deferred-read guard. A
/// throwaway Net with a resolver but no device poll keeps a started query
/// Pending forever, so a read DEFERS. Asserts: (1) the first read defers (holds
/// tag 10), (2) a SECOND concurrent read on the same fid does NOT clobber the
/// held tag (it gets an empty reply, the first stays deferred), (3)
/// fid_has_deferred reports the in-flight read (so h_write rejects a racing
/// re-write). On the PRE-fix code the second read overwrote `deferred` to tag 11
/// -> assertion (2) fails. Closes I-9 for the cs/dns deferred-read path.
pub fn dns_defer_guard_selftest(base: Instant) -> bool {
    let mut device = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x05,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let mut iface = Interface::new(config, &mut device, ts0);
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
    });
    let mut ifc = IfConfig::empty();
    ifc.dns = Some([10, 0, 0, 1]); // a resolver (never polled -> the query stays Pending)
    let mut net = Net::new(iface, SocketSet::new(Vec::new()), base, ifc);
    let mut conn = Conn::new(0);

    // A DNS name resolution begins; the first read defers (Pending, no poll).
    conn.query_begin(&mut net, 1, P_DNS, b"defer.test");
    let _ = conn.query_read(&mut net, 1, 128, 10);
    let first_deferred = conn
        .queries
        .iter()
        .any(|q| q.fid == 1 && q.deferred == Some((10, 128)));
    let first_set_defer = conn.defer;
    conn.defer = false; // observe the second read's defer decision independently

    // F1 facet 1: the SECOND concurrent read must NOT clobber the held tag.
    let _ = conn.query_read(&mut net, 1, 128, 11);
    let held_intact = conn
        .queries
        .iter()
        .any(|q| q.fid == 1 && q.deferred == Some((10, 128)));
    let second_not_deferred = !conn.defer;

    // F1 facet 2 predicate: the fid reports an in-flight deferred read.
    let predicate = conn.fid_has_deferred(1);

    conn.query_clear_all(&mut net); // cancel the pending query (no smoltcp slot leak)

    first_deferred && first_set_defer && held_intact && second_not_deferred && predicate
}

/// Drive `lo` until `ready(lo)` or the poll budget is spent. The loopback device
/// loops synchronously within `poll`, but smoltcp's ms-clock must advance for the
/// TCP handshake timers, so a small per-poll sleep advances the real monotonic
/// clock `now()` reads.
fn lo_drive<F: FnMut(&mut Net) -> bool>(lo: &mut Net, device: &mut Loopback, mut ready: F) -> bool {
    for _ in 0..LO_POLLS {
        lo.poll(device);
        if ready(lo) {
            return true;
        }
        let _ = sleep(Duration::from_millis(LO_POLL_MS));
    }
    false
}

/// ICMP echo round-trip: a single icmp socket pings 127.0.0.1; the lo iface
/// auto-replies to an echo to its own address, and routes the reply back by ident.
fn lo_icmp_roundtrip(lo: &mut Net, device: &mut Loopback) -> bool {
    let n = match lo.icmp_clone() {
        Some(n) => n,
        None => return false,
    };
    let _ = lo.icmp_connect(n, [127, 0, 0, 1]);
    let _ = lo.data_send(n, b"thylacine-lo-icmp");
    let mut buf = [0u8; 64];
    let ok = lo_drive(lo, device, |lo| lo.data_recv(n, &mut buf) > 0);
    lo.free_orphan_mint(n);
    ok
}

/// UDP datagram round-trip: a udp socket bound to an ephemeral port re-points its
/// remote at its OWN bound port and sends -- a self-addressed datagram loops back
/// and is delivered to the same socket.
fn lo_udp_roundtrip(lo: &mut Net, device: &mut Loopback) -> bool {
    let n = match lo.udp_clone() {
        Some(n) => n,
        None => return false,
    };
    if lo.udp_connect(n, [127, 0, 0, 1], 9).is_err() {
        lo.free_orphan_mint(n);
        return false;
    }
    let pa = lo.slot_endpoint(n, true).map(|(_, p)| p).unwrap_or(0);
    if pa == 0 {
        lo.free_orphan_mint(n);
        return false;
    }
    let _ = lo.udp_connect(n, [127, 0, 0, 1], pa); // re-point remote -> self
    let _ = lo.data_send(n, b"thylacine-lo-udp");
    let mut buf = [0u8; 64];
    let ok = lo_drive(lo, device, |lo| lo.data_recv(n, &mut buf) > 0);
    lo.free_orphan_mint(n);
    ok
}

/// TCP inbound-accept round-trip: a listener (announce) + a client (connect to
/// 127.0.0.1:port). The SYN loops back, the listener establishes, and a synthetic
/// deferred accept resolves through the REAL net-3a path (poll_accepts ->
/// accept_swap mints M) -- the F1-fixed code, exercised in-guest. The Conn-level
/// fid rebind (complete_accept) is covered by the live /net mount; here we drive
/// the Net half (no Conn fids).
fn lo_tcp_accept(lo: &mut Net, device: &mut Loopback) -> bool {
    let ln = match lo.tcp_clone() {
        Some(n) => n,
        None => return false,
    };
    let ep = IpListenEndpoint {
        addr: None,
        port: LO_LOOPBACK_PORT,
    };
    if lo.ctl_announce(ln, ep).is_err() {
        lo.free_orphan_mint(ln);
        return false;
    }
    let cn = match lo.tcp_clone() {
        Some(n) => n,
        None => {
            lo.free_orphan_mint(ln);
            return false;
        }
    };
    if lo
        .tcp_connect(cn, [127, 0, 0, 1], LO_LOOPBACK_PORT)
        .is_err()
    {
        lo.free_orphan_mint(cn);
        lo.free_orphan_mint(ln);
        return false;
    }
    // Synthetic deferred accept (conn_id/tag/fid = 0): drives poll_accepts +
    // accept_swap without a Conn fid table.
    lo.register_accept(0, 0, 0, ln);
    let ok = lo_drive(lo, device, |lo| {
        let done = lo.poll_accepts();
        if done.is_empty() {
            return false;
        }
        // The inbound call was accepted (accept_swap minted M). Free the unowned
        // mints (no Conn to deliver to).
        for d in done {
            lo.discard_accept(d);
        }
        true
    });
    lo.free_orphan_mint(cn);
    lo.free_orphan_mint(ln);
    ok
}

/// net-6a: a DETERMINISTIC in-guest self-test of the blocking-read OUTCOME
/// distinction -- the heart of net-6a. `data_recv_outcome` must tell "no data
/// yet, the connection is open" (WouldBlock -> h_read PARKS, recv blocks) from
/// "end of stream" (Eof -> recv returns 0) -- the very distinction the older
/// `data_recv` collapsed (the net-5 F2 ambiguity). An isolated 127.0.0.1 stack
/// (the net-3d loopback pattern) establishes a TCP connection through the REAL
/// net-3a accept path, then drives the three states (empty -> sent -> peer-close)
/// through `data_recv_outcome` -- the method h_read/poll_data dispatch on. No
/// NIC, no host -> fully deterministic, asserted by the caller (a PASS/FAIL line).
pub fn recv_blocking_e2e(base: Instant) -> &'static str {
    let mut device = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x03,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let mut iface = Interface::new(config, &mut device, ts0);
    // The iface MUST carry 127.0.0.1/8 -- tcp_connect selects a source address
    // from it, so without an address the active-open fails (the loopback_e2e
    // does this; ipifc_e2e omits it because it never connects).
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
    });
    let mut lo = Net::new(iface, SocketSet::new(Vec::new()), base, IfConfig::empty());

    // Establish a connection (announce + connect + the deferred accept), KEEPING
    // the server side M (lo_tcp_accept discards it; here we recv on it).
    let ln = match lo.tcp_clone() {
        Some(n) => n,
        None => return "clone-ln",
    };
    let ep = IpListenEndpoint {
        addr: None,
        port: LO_LOOPBACK_PORT,
    };
    if lo.ctl_announce(ln, ep).is_err() {
        lo.free_orphan_mint(ln);
        return "announce";
    }
    let cn = match lo.tcp_clone() {
        Some(n) => n,
        None => {
            lo.free_orphan_mint(ln);
            return "clone-cn";
        }
    };
    if lo
        .tcp_connect(cn, [127, 0, 0, 1], LO_LOOPBACK_PORT)
        .is_err()
    {
        lo.free_orphan_mint(cn);
        lo.free_orphan_mint(ln);
        return "connect";
    }
    lo.register_accept(0, 0, 0, ln);
    let mut server: Option<u32> = None;
    let accepted = lo_drive(&mut lo, &mut device, |lo| {
        let done = lo.poll_accepts();
        if done.is_empty() {
            return false;
        }
        for d in done {
            server = Some(d.new_n); // the unowned synthetic mint -- the server side
        }
        true
    });
    let m = match (accepted, server) {
        (true, Some(m)) => m,
        _ => {
            lo.free_orphan_mint(cn);
            lo.free_orphan_mint(ln);
            return "accept";
        }
    };
    let mut buf = [0u8; 64];
    let stage = recv_blocking_legs(&mut lo, &mut device, m, cn, &mut buf);
    lo.free_orphan_mint(m);
    lo.free_orphan_mint(cn);
    lo.free_orphan_mint(ln);
    stage
}

/// The three blocking-read legs over an established loopback connection (server
/// side `m`, client side `cn`): empty -> WouldBlock; sent -> Data(exact bytes);
/// peer-close -> Eof. Returns "ok" on pass, else the failing stage name (so a
/// FAIL boot line pinpoints where the distinction broke).
fn recv_blocking_legs(
    lo: &mut Net,
    device: &mut Loopback,
    m: u32,
    cn: u32,
    buf: &mut [u8],
) -> &'static str {
    // (1) Server rx empty, connection open -> WouldBlock (h_read would PARK).
    if !matches!(lo.data_recv_outcome(m, buf), RecvOutcome::WouldBlock) {
        return "leg1-wouldblock";
    }
    // (2) Client sends; once the segment lands (non-consuming can_recv), the
    //     server's data_recv_outcome returns the exact bytes (Data).
    if lo.data_send(cn, b"hi") != 2 {
        return "leg2-send";
    }
    if !lo_drive(lo, device, |lo| lo.slot_can_recv(m)) {
        return "leg2-canrecv";
    }
    match lo.data_recv_outcome(m, buf) {
        RecvOutcome::Data(2) if &buf[..2] == b"hi" => {}
        _ => return "leg2-data",
    }
    // Drained again -> WouldBlock (the connection is still open).
    if !matches!(lo.data_recv_outcome(m, buf), RecvOutcome::WouldBlock) {
        return "leg2-redrain";
    }
    // (3) Client closes; once the FIN drains, the server's read sees EOF (recv
    //     returns 0), NOT a perpetual block.
    lo.ctl_hangup(cn);
    if lo_drive(lo, device, |lo| {
        matches!(lo.data_recv_outcome(m, &mut [0u8; 1]), RecvOutcome::Eof)
    }) {
        "ok"
    } else {
        "leg3-eof"
    }
}

/// net-6b: the readiness substrate (check_ready) over an established loopback
/// connection. Asserts the dev9p.poll bridge's netd half deterministically
/// in-guest -- a fresh established socket is WRITABLE (POLLOUT) but not READABLE
/// (POLLIN defers); after the peer sends it is READABLE; after the peer closes +
/// the FIN drains the recv side reports READABLE (a read returns EOF, so a poller
/// waiting on a disconnect wakes). The recv_blocking_e2e pattern; the loopback
/// isolation is load-bearing (net-3d -- a shared-NIC SocketSet mis-routes lo).
pub fn ready_e2e(base: Instant) -> &'static str {
    let mut device = Loopback::new(Medium::Ethernet);
    let mut config = Config::new(HardwareAddress::Ethernet(EthernetAddress([
        0x02, 0x00, 0x00, 0x00, 0x00, 0x07,
    ])));
    config.random_seed = LO_SEED;
    let ts0 = SmolInstant::from_millis(base.elapsed().as_millis() as i64);
    let mut iface = Interface::new(config, &mut device, ts0);
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::v4(127, 0, 0, 1), 8));
    });
    let mut lo = Net::new(iface, SocketSet::new(Vec::new()), base, IfConfig::empty());
    let (m, cn, ln) = match lo_establish_pair(&mut lo, &mut device) {
        Some(t) => t,
        None => return "establish",
    };
    let stage = ready_legs(&mut lo, &mut device, m, cn);
    lo.free_orphan_mint(m);
    lo.free_orphan_mint(cn);
    lo.free_orphan_mint(ln);
    stage
}

/// Establish a loopback TCP connection for the net-6 selftests: announce a
/// listener `ln`, connect a client `cn`, drive the accept to mint the server
/// side `m`. Returns (m, cn, ln) -- the caller frees all three via
/// free_orphan_mint -- or None (with the orphan mints freed) on any stage
/// failure. (recv_blocking_e2e inlines the same sequence; ready_e2e shares it.)
fn lo_establish_pair(lo: &mut Net, device: &mut Loopback) -> Option<(u32, u32, u32)> {
    let ln = lo.tcp_clone()?;
    let ep = IpListenEndpoint {
        addr: None,
        port: LO_LOOPBACK_PORT,
    };
    if lo.ctl_announce(ln, ep).is_err() {
        lo.free_orphan_mint(ln);
        return None;
    }
    let cn = match lo.tcp_clone() {
        Some(n) => n,
        None => {
            lo.free_orphan_mint(ln);
            return None;
        }
    };
    if lo
        .tcp_connect(cn, [127, 0, 0, 1], LO_LOOPBACK_PORT)
        .is_err()
    {
        lo.free_orphan_mint(cn);
        lo.free_orphan_mint(ln);
        return None;
    }
    lo.register_accept(0, 0, 0, ln);
    let mut server: Option<u32> = None;
    let accepted = lo_drive(lo, device, |lo| {
        let done = lo.poll_accepts();
        if done.is_empty() {
            return false;
        }
        for d in done {
            server = Some(d.new_n);
        }
        true
    });
    match (accepted, server) {
        (true, Some(m)) => Some((m, cn, ln)),
        _ => {
            lo.free_orphan_mint(cn);
            lo.free_orphan_mint(ln);
            None
        }
    }
}

/// The readiness legs over an established loopback connection (server `m`, client
/// `cn`). Established yields POLLOUT set and POLLIN clear; after the peer sends,
/// POLLIN sets; after the peer closes and the FIN drains, POLLIN stays set (EOF
/// reads as readable). Returns "ok" or the failing stage (so a FAIL boot line
/// pinpoints where readiness broke).
fn ready_legs(lo: &mut Net, device: &mut Loopback, m: u32, cn: u32) -> &'static str {
    // (1) Established, rx empty: POLLOUT satisfied (writable); POLLIN NOT (no
    //     data) -> a poll(POLLIN) would DEFER, a poll(POLLOUT) returns at once.
    if lo.check_ready(m, POLLOUT) & POLLOUT == 0 {
        return "leg1-writable";
    }
    if lo.check_ready(m, POLLIN) & POLLIN != 0 {
        return "leg1-not-readable";
    }
    // (2) Client sends; once the segment lands, the server is READABLE.
    if lo.data_send(cn, b"hi") != 2 {
        return "leg2-send";
    }
    if !lo_drive(lo, device, |lo| lo.check_ready(m, POLLIN) & POLLIN != 0) {
        return "leg2-readable";
    }
    // (3) Drain the buffered "hi", then the client closes. Once the FIN drains
    //     and the rx is empty, the server's recv side is at EOF -- still POLLIN
    //     (a read returns 0), so a poller waiting on a peer disconnect wakes.
    let mut buf = [0u8; 8];
    let _ = lo.data_recv_outcome(m, &mut buf);
    lo.ctl_hangup(cn);
    if lo_drive(lo, device, |lo| {
        matches!(lo.data_recv_outcome(m, &mut [0u8; 1]), RecvOutcome::Eof)
            && lo.check_ready(m, POLLIN) & POLLIN != 0
    }) {
        "ok"
    } else {
        "leg3-eof-readable"
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

/// The IPv4 octets of a resolved address (net-4b), or None for a non-IPv4
/// result (smoltcp only yields IPv4 with proto-ipv4 alone, but match defensively).
fn first_ipv4(addr: &IpAddress) -> Option<[u8; 4]> {
    match addr {
        IpAddress::Ipv4(v4) => Some(v4.octets()),
        #[allow(unreachable_patterns)]
        _ => None,
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

/// Parse an ipifc `add` mask operand into a CIDR prefix length (net-4c). Accepts
/// a dotted-quad netmask (`255.255.255.0` -> 24, contiguous masks only), a
/// `/N`-form, or a bare prefix length `N`. None on a malformed / >32 / non-
/// contiguous mask.
fn parse_mask(s: &[u8]) -> Option<u8> {
    let body = if !s.is_empty() && s[0] == b'/' {
        &s[1..]
    } else {
        s
    };
    // A dotted-quad netmask -> count the leading ones (and verify contiguity).
    if body.contains(&b'.') {
        let m = parse_ipv4(body)?;
        let bits = (u32::from(m[0]) << 24)
            | (u32::from(m[1]) << 16)
            | (u32::from(m[2]) << 8)
            | u32::from(m[3]);
        let ones = bits.leading_ones();
        // Contiguous iff the mask is exactly `ones` leading 1s (no holes).
        let expect = if ones == 0 { 0 } else { !0u32 << (32 - ones) };
        if bits != expect {
            return None;
        }
        return Some(ones as u8);
    }
    // A bare prefix length.
    let p = parse_u8(body)?;
    if p <= 32 {
        Some(p)
    } else {
        None
    }
}

/// Render a CIDR prefix length as a dotted-quad netmask (net-4c ndb `ipmask=`).
fn mask_octets(prefix: u8) -> [u8; 4] {
    let p = prefix.min(32);
    let bits: u32 = if p == 0 { 0 } else { !0u32 << (32 - p as u32) };
    bits.to_be_bytes()
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

/// Trim leading/trailing ASCII whitespace (a cs write often carries a trailing
/// newline; the dial string itself has no internal spaces).
fn trim_ws(s: &[u8]) -> &[u8] {
    let mut a = 0;
    let mut b = s.len();
    while a < b && s[a].is_ascii_whitespace() {
        a += 1;
    }
    while b > a && s[b - 1].is_ascii_whitespace() {
        b -= 1;
    }
    &s[a..b]
}

/// What a resolved address becomes in a /net/cs or /net/dns response (net-4b).
#[derive(Copy, Clone)]
enum QueryKind {
    /// /net/cs: format `<clone-file> <ip>!<port>\n` (NET-DESIGN s5). The dial's
    /// proto picks the clone-file; the service is pre-resolved to `port` here.
    Cs { clone_tcp: bool, port: u16 },
    /// /net/dns: format `<ip>\n` (the bare resolved address; the v1.0 minimal
    /// form -- a v1.x refinement returns the full ndb-style attribute line).
    Dns,
}

/// Parse + partially resolve a cs dial string (`proto!host!service`, the Plan 9
/// dial notation). Resolves proto -> clone-file and service -> port
/// SYNCHRONOUSLY (neither needs DNS), returning (kind, host) and leaving only
/// the host for resolution (numeric -> ndb -> DNS, by the caller). `net!...`
/// maps to tcp (the v1.0 default; a tcp+udp fan-out is a v1.x refinement). None
/// on any malformation or an unknown proto/service -- cs's "no reachable path"
/// signal (the caller fills the empty answer; the reader sees 0 bytes).
fn cs_parse(dial: &[u8]) -> Option<(QueryKind, &[u8])> {
    let mut it = dial.split(|&b| b == b'!');
    let proto = it.next()?;
    let host = it.next()?; // a dial needs at least proto!host
    let service = it.next().unwrap_or(b"");
    if it.next().is_some() {
        return None; // more than three '!'-separated fields
    }
    let clone_tcp = match proto {
        b"tcp" | b"net" => true,
        b"udp" => false,
        _ => return None, // unknown protocol
    };
    let port = match parse_u16(service) {
        Some(p) if p != 0 => p,
        _ => crate::ndb::lookup_service(service)?, // unknown service / zero port
    };
    Some((QueryKind::Cs { clone_tcp, port }, host))
}

/// Parse a /net/dns request line (`name [type]`, net-4b). v1.0 resolves only A
/// (IPv4) records: an explicit `ip`/`a`/`ipv4` type is accepted; any other type
/// (e.g. `aaaa`) yields None (no IPv6 at v1.0 -> the empty answer). Returns the
/// name to resolve (numeric -> ndb -> DNS, by the caller).
fn dns_parse(line: &[u8]) -> Option<&[u8]> {
    let mut it = line
        .split(|&b| b == b' ' || b == b'\t')
        .filter(|t| !t.is_empty());
    let name = it.next()?;
    if let Some(ty) = it.next() {
        if !eq_ascii_lower(ty, b"ip") && !eq_ascii_lower(ty, b"a") && !eq_ascii_lower(ty, b"ipv4") {
            return None;
        }
    }
    Some(name)
}

/// Format a resolved address per the request's kind (net-4b).
fn format_resolved(kind: QueryKind, ip: [u8; 4]) -> Vec<u8> {
    let mut c = Content::new();
    match kind {
        QueryKind::Cs { clone_tcp, port } => {
            let clone: &[u8] = if clone_tcp {
                b"/net/tcp/clone"
            } else {
                b"/net/udp/clone"
            };
            c.push(clone);
            c.push(b" ");
            c.push_endpoint(ip, port);
            c.push(b"\n");
        }
        QueryKind::Dns => {
            c.push_ip(ip);
            c.push(b"\n");
        }
    }
    let mut out = Vec::new();
    out.extend_from_slice(c.as_slice());
    out
}

/// ASCII case-insensitive equality (for the dns type token).
fn eq_ascii_lower(a: &[u8], b: &[u8]) -> bool {
    a.len() == b.len() && a.iter().zip(b).all(|(x, y)| x.to_ascii_lowercase() == *y)
}

// A small fixed-cap content buffer: file bodies are short (a number, a few-line
// stats block, a status word), so reads never allocate. 256 holds the widest
// render (the ipifc/0 status block ~120 B) with comfortable headroom; `push`
// min-clamps, so an over-long body truncates (never an OOB), but no current
// render approaches the cap.
struct Content {
    buf: [u8; 256],
    len: usize,
}

impl Content {
    fn new() -> Content {
        Content {
            buf: [0; 256],
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
        self.push_ip(ip);
        self.push(b"!");
        self.push_dec(port as u32);
    }
    /// Format a bare dotted-quad address (the portless ICMP `local`/`remote`).
    fn push_ip(&mut self, ip: [u8; 4]) {
        self.push_dec(ip[0] as u32);
        self.push(b".");
        self.push_dec(ip[1] as u32);
        self.push(b".");
        self.push_dec(ip[2] as u32);
        self.push(b".");
        self.push_dec(ip[3] as u32);
    }
    /// Format a colon-separated lowercase-hex MAC (net-4c ipifc status).
    fn push_mac(&mut self, mac: [u8; 6]) {
        for (i, &b) in mac.iter().enumerate() {
            if i != 0 {
                self.push(b":");
            }
            let hi = b >> 4;
            let lo = b & 0xf;
            let nib = |n: u8| if n < 10 { b'0' + n } else { b'a' + (n - 10) };
            self.push(&[nib(hi), nib(lo)]);
        }
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

/// A per-fid /net/cs or /net/dns request/response session (net-4a; deferred-
/// capable at net-4b). The write begins the resolution: a numeric/ndb host fills
/// `resp` synchronously (query None); a name that needs DNS starts a query
/// (query Some, resp None). The read drains `resp` via `cursor` (offset-agnostic,
/// like FK_DATA); if a query is still in flight it DEFERS (the held Rread is sent
/// by poll_dns when the query resolves). One per open cs/dns fid, dropped on
/// clunk / teardown / Tversion (cancelling any in-flight query) -- bounded by the
/// connection's fid table (a re-write replaces the same fid's session; MAX_FIDS
/// bounds the rest).
struct Query {
    fid: u32,
    kind: QueryKind,
    // Some while a DNS query is in flight (resp is None until it resolves). The
    // smoltcp query slot is freed the instant dns_poll returns a result, so this
    // is nulled then and never polled again (the single-completion discipline).
    query: Option<dns::QueryHandle>,
    // The formatted answer, ready to drain. None iff a DNS query is in flight.
    resp: Option<Vec<u8>>,
    cursor: usize,
    // Set when a read deferred on this query: the held Tread (tag, cap) to answer
    // when the query resolves. None = no read awaiting a deferred completion.
    deferred: Option<(u16, usize)>,
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
    // Set by a handler that holds its reply (a blocking listen open or a deferred
    // cs/dns read); read + cleared by dispatch to return Disp::Deferred.
    defer: bool,
    // Per-fid /net/cs + /net/dns request/response sessions (net-4a/4b).
    queries: Vec<Query>,
    // Blocking `data` reads holding their Rread (net-6a): each parked on an empty
    // rx, completed by poll_data when bytes arrive (or 0 on EOF).
    pending_reads: Vec<PendingRead>,
    // Deferred `ready` readiness probes holding their Rread (net-6b): each parked
    // on a not-yet-satisfied poll mask, completed by poll_ready when the socket
    // becomes ready per the mask. The dev9p.poll bridge's netd half.
    pending_ready: Vec<PendingReady>,
    // Deferred TCP `data` opens holding their Rlopen (#257): each parked on a
    // still-connecting socket, completed by poll_connects when the handshake
    // reaches ESTABLISHED (or fails / times out). Makes the data open block to
    // ESTABLISHED, as TcpStream::connect documents -- the outbound NIC path.
    pending_connects: Vec<PendingConnect>,
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
            queries: Vec::new(),
            pending_reads: Vec::new(),
            pending_ready: Vec::new(),
            pending_connects: Vec::new(),
        }
    }

    /// Begin a /net/cs or /net/dns resolution for `fid` (the write IS the query).
    /// Replaces any prior session on the same fid (cancelling its in-flight query
    /// first, so no smoltcp query slot leaks). `file` is P_CS or P_DNS. A
    /// numeric/ndb host fills the answer synchronously; a name that needs DNS
    /// starts a query (the subsequent read defers). A malformed request or a
    /// resolver-less DNS name fills the empty answer (cs's "no path" signal).
    fn query_begin(&mut self, net: &mut Net, fid: u32, file: u64, data: &[u8]) {
        // Drop any prior session for this fid (cancels its in-flight query +
        // any deferred read), so a re-write never leaks a smoltcp query slot.
        self.query_drop(net, fid);
        let dial = trim_ws(data);
        let (kind, host) = if file == P_CS {
            match cs_parse(dial) {
                Some(kh) => kh,
                None => return self.query_fill(fid, QueryKind::Dns, Vec::new()),
            }
        } else {
            match dns_parse(dial) {
                Some(name) => (QueryKind::Dns, name),
                None => return self.query_fill(fid, QueryKind::Dns, Vec::new()),
            }
        };
        // Resolve the host: numeric -> static ndb -> DNS.
        if let Some(ip) = parse_ipv4(host) {
            return self.query_fill(fid, kind, format_resolved(kind, ip));
        }
        if let Some(ip) = crate::ndb::lookup_host(host) {
            return self.query_fill(fid, kind, format_resolved(kind, ip));
        }
        // Delegate to DNS. A start failure (no resolver, bad name) fills empty
        // -- fail closed fast, never a hung read on an unanswerable query.
        match net.dns_query_start(host) {
            Some(q) => self.queries.push(Query {
                fid,
                kind,
                query: Some(q),
                resp: None,
                cursor: 0,
                deferred: None,
            }),
            None => self.query_fill(fid, kind, Vec::new()),
        }
    }

    /// Install a synchronously-resolved answer for `fid` (query None).
    fn query_fill(&mut self, fid: u32, kind: QueryKind, resp: Vec<u8>) {
        self.queries.push(Query {
            fid,
            kind,
            query: None,
            resp: Some(resp),
            cursor: 0,
            deferred: None,
        });
    }

    /// Read up to `cap` bytes of the /net/cs|dns answer for `fid`. If the answer
    /// is ready, drain from the cursor. If a DNS query is still in flight, poll
    /// it: a result formats + drains; Pending DEFERS (holds the Rread, sent by
    /// poll_dns on resolution). A read before any write (no session) yields 0.
    fn query_read(&mut self, net: &mut Net, fid: u32, cap: usize, tag: u16) -> Result<usize, ()> {
        let idx = match self.queries.iter().position(|q| q.fid == fid) {
            Some(i) => i,
            None => return p9::build_rread(&mut self.out_buf, tag, &[]),
        };
        // F1 guard (net-4d): a read is ALREADY deferred on this fid (a second
        // concurrent read on a shared fd -- legal for a multi-thread Proc, which
        // the kernel client multiplexes by tag). Do NOT clobber the held tag --
        // answer the new read with an empty Rread; the first read stays deferred
        // and is delivered by poll_dns when the query lands. (Preserves I-9: no
        // held Rread is silently lost; the older read keeps its real answer.)
        if self.queries[idx].deferred.is_some() {
            return p9::build_rread(&mut self.out_buf, tag, &[]);
        }
        // If a query is in flight, check whether it has resolved since the write.
        if let Some(h) = self.queries[idx].query {
            match net.dns_poll(h) {
                DnsPoll::Pending => {
                    // Hold the Rread; poll_dns completes it when the query lands.
                    self.queries[idx].deferred = Some((tag, cap));
                    self.defer = true;
                    return Ok(0); // ignored (defer set)
                }
                DnsPoll::Resolved(ip) => {
                    let kind = self.queries[idx].kind;
                    self.queries[idx].query = None; // slot freed by dns_poll
                    self.queries[idx].resp = Some(format_resolved(kind, ip));
                }
                DnsPoll::Failed => {
                    self.queries[idx].query = None; // slot freed by dns_poll
                    self.queries[idx].resp = Some(Vec::new()); // unresolved -> empty
                }
            }
        }
        let chunk = self.query_drain(idx, cap);
        p9::build_rread(&mut self.out_buf, tag, &chunk)
    }

    /// Drain up to `cap` bytes of query `idx`'s ready answer from its cursor.
    fn query_drain(&mut self, idx: usize, cap: usize) -> Vec<u8> {
        let q = &mut self.queries[idx];
        match &q.resp {
            Some(resp) => {
                let end = (q.cursor + cap).min(resp.len());
                let out = resp[q.cursor..end].to_vec();
                q.cursor = end;
                out
            }
            None => Vec::new(),
        }
    }

    /// Complete any deferred cs/dns reads whose DNS query has resolved (net-4b):
    /// the serve-loop analog of poll_accepts. For each session awaiting a result,
    /// poll the query; on a result, format the answer + send the held Rread.
    /// Returns false if a held-Rread write failed (the session is dead -> the
    /// serve loop tears this connection down).
    pub fn poll_dns(&mut self, net: &mut Net) -> bool {
        for idx in 0..self.queries.len() {
            let (tag, cap) = match self.queries[idx].deferred {
                Some(d) => d,
                None => continue,
            };
            let h = match self.queries[idx].query {
                Some(h) => h,
                None => {
                    // A resolved/empty answer with a stuck deferred marker
                    // (defensive): deliver it now.
                    self.queries[idx].deferred = None;
                    if !self.deliver_deferred(idx, tag, cap) {
                        return false;
                    }
                    continue;
                }
            };
            match net.dns_poll(h) {
                DnsPoll::Pending => {} // still waiting; check again next tick
                DnsPoll::Resolved(ip) => {
                    let kind = self.queries[idx].kind;
                    self.queries[idx].query = None;
                    self.queries[idx].resp = Some(format_resolved(kind, ip));
                    self.queries[idx].deferred = None;
                    if !self.deliver_deferred(idx, tag, cap) {
                        return false;
                    }
                }
                DnsPoll::Failed => {
                    self.queries[idx].query = None;
                    self.queries[idx].resp = Some(Vec::new());
                    self.queries[idx].deferred = None;
                    if !self.deliver_deferred(idx, tag, cap) {
                        return false;
                    }
                }
            }
        }
        true
    }

    /// Build + send the held Rread for a now-ready deferred query. False on a
    /// write failure (tear the connection down).
    fn deliver_deferred(&mut self, idx: usize, tag: u16, cap: usize) -> bool {
        let chunk = self.query_drain(idx, cap);
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rread(&mut self.out_buf, tag, &chunk) {
            Ok(rlen) => self.send_all(rlen),
            Err(()) => false,
        }
    }

    /// Any session holding a deferred read (the serve loop clamps its poll delay
    /// to the floor so an in-flight DNS query completes promptly).
    pub fn has_pending_dns(&self) -> bool {
        self.queries.iter().any(|q| q.deferred.is_some())
    }

    /// Complete any blocking `data` reads whose socket now has bytes (or EOF):
    /// the serve-loop analog of poll_dns/poll_accepts, called per-Conn after
    /// net.poll() (so RX delivered this tick is visible). For each parked read,
    /// re-attempt the dequeue: Data/Eof sends the held Rread (removing the
    /// pending); WouldBlock keeps waiting. FIFO over the slot, so two reads on one
    /// fd drain in order. Returns false if a held-Rread write failed (the session
    /// is dead -> the serve loop tears this connection down). I-9: the
    /// single-threaded loop runs net.poll() BEFORE this, so a data edge cannot be
    /// lost between h_read's empty dequeue and the park (the poll_dns reasoning).
    pub fn poll_data(&mut self, net: &mut Net) -> bool {
        let mut i = 0;
        while i < self.pending_reads.len() {
            let pr = self.pending_reads[i];
            let mut scratch = [0u8; DATA_CHUNK];
            let cap = pr.cap.min(DATA_CHUNK);
            match net.data_recv_outcome(pr.slot_n, &mut scratch[..cap]) {
                RecvOutcome::WouldBlock => i += 1, // still empty; check next tick
                RecvOutcome::Data(k) => {
                    self.pending_reads.remove(i);
                    if !self.deliver_read(pr.tag, &scratch[..k]) {
                        return false;
                    }
                }
                RecvOutcome::Eof => {
                    self.pending_reads.remove(i);
                    if !self.deliver_read(pr.tag, &[]) {
                        return false;
                    }
                }
            }
        }
        true
    }

    /// Build + send the held Rread for a now-ready blocking data read. False on a
    /// write failure (tear the connection down). Mirrors deliver_deferred.
    fn deliver_read(&mut self, tag: u16, data: &[u8]) -> bool {
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rread(&mut self.out_buf, tag, data) {
            Ok(rlen) => self.send_all(rlen),
            Err(()) => false,
        }
    }

    /// Any blocking data read is parked (the serve loop clamps its poll delay to
    /// the floor so RX-driven completion stays prompt -- RX arrives on the NIC,
    /// not a pollable 9P fd, so only a timeout-driven net.poll catches it).
    pub fn has_pending_reads(&self) -> bool {
        !self.pending_reads.is_empty()
    }

    /// Complete any deferred TCP `data` opens whose handshake has resolved (#257;
    /// the serve-loop pass, called per-Conn after net.poll so a transition this
    /// tick is visible). ESTABLISHED -> send the held Rlopen (the data stream is
    /// now live); Failed (RST/reset/timeout-abort) -> send Rlerror; still Pending
    /// past the deadline -> abort the socket + Rlerror ETIMEDOUT (so a SYN to an
    /// unreachable host cannot hold the open forever). False on a write failure
    /// (tear the connection down). Mirrors poll_data.
    pub fn poll_connects(&mut self, net: &mut Net) -> bool {
        let mut i = 0;
        while i < self.pending_connects.len() {
            let pc = self.pending_connects[i];
            match net.tcp_connect_state(pc.slot_n) {
                ConnectState::Pending => {
                    if net.now_ms() >= pc.deadline_ms {
                        net.tcp_abort_connect(pc.slot_n);
                        self.pending_connects.remove(i);
                        if !self.deliver_connect_err(pc.tag, p9::E_TIMEDOUT) {
                            return false;
                        }
                    } else {
                        i += 1; // still handshaking; check next tick
                    }
                }
                ConnectState::Ready => {
                    self.pending_connects.remove(i);
                    let q = self.qid_of(net, pc.path);
                    if !self.deliver_connect(pc.tag, &q) {
                        return false;
                    }
                }
                ConnectState::Failed => {
                    self.pending_connects.remove(i);
                    if !self.deliver_connect_err(pc.tag, p9::E_CONNREFUSED) {
                        return false;
                    }
                }
            }
        }
        true
    }

    /// Build + send the held Rlopen for a now-ESTABLISHED deferred data open.
    fn deliver_connect(&mut self, tag: u16, qid: &p9::Qid) -> bool {
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rlopen(&mut self.out_buf, tag, qid, 0) {
            Ok(rlen) => self.send_all(rlen),
            Err(()) => false,
        }
    }

    /// Build + send the held Rlerror for a failed/timed-out deferred data open.
    fn deliver_connect_err(&mut self, tag: u16, code: u32) -> bool {
        self.out_buf.clear();
        self.out_buf.resize(SRV_MSIZE_USIZE, 0);
        match p9::build_rlerror(&mut self.out_buf, tag, code) {
            Ok(rlen) => self.send_all(rlen),
            Err(()) => false,
        }
    }

    pub fn has_pending_connects(&self) -> bool {
        !self.pending_connects.is_empty()
    }

    /// Complete any deferred readiness probes whose socket now satisfies the mask
    /// (the net-6b analog of poll_data): the serve-loop pass, called per-Conn
    /// after net.poll() (so a transition this tick is visible). For each parked
    /// probe, re-run check_ready(mask): a non-zero revents sends the held Rread
    /// (the 4-byte bitmap) and removes the pending; a still-zero revents keeps
    /// waiting. Returns false if a held-Rread write failed (the session is dead
    /// -> the serve loop tears this connection down). I-9: the single-threaded
    /// loop runs net.poll() BEFORE this, so a readiness edge cannot be lost
    /// between h_read's check and the park (the poll_data/poll_dns reasoning).
    pub fn poll_ready(&mut self, net: &mut Net) -> bool {
        let mut i = 0;
        while i < self.pending_ready.len() {
            let pr = self.pending_ready[i];
            let revents = net.check_ready(pr.slot_n, pr.mask);
            if revents != 0 {
                self.pending_ready.remove(i);
                let bytes = (revents as u32).to_le_bytes();
                if !self.deliver_read(pr.tag, &bytes) {
                    return false;
                }
            } else {
                i += 1; // still not ready for the mask; check next tick
            }
        }
        true
    }

    /// Any deferred readiness probe is parked (the serve loop clamps its poll
    /// delay to the floor so a socket transition driven by NIC RX -- not a
    /// pollable 9P fd -- completes the held readiness read promptly).
    pub fn has_pending_ready(&self) -> bool {
        !self.pending_ready.is_empty()
    }

    /// Whether the cs/dns session for `fid` currently holds a deferred (in-flight)
    /// read. h_write consults this to REJECT a re-write that would otherwise drop
    /// the held tag (the F1 re-write facet, net-4d): a held read completes
    /// normally instead of being silently superseded.
    fn fid_has_deferred(&self, fid: u32) -> bool {
        self.queries
            .iter()
            .any(|q| q.fid == fid && q.deferred.is_some())
    }

    /// Drop the cs/dns session for `fid` (on clunk / rewrite): cancels any
    /// in-flight DNS query (freeing its smoltcp slot) before removing the entry.
    fn query_drop(&mut self, net: &mut Net, fid: u32) {
        if let Some(i) = self.queries.iter().position(|q| q.fid == fid) {
            if let Some(h) = self.queries[i].query {
                net.dns_cancel(h); // still-pending: cancel is safe (slot occupied)
            }
            self.queries.remove(i);
        }
    }

    /// Drop EVERY cs/dns session (on teardown / Tversion), cancelling each
    /// in-flight DNS query so no smoltcp query slot leaks.
    fn query_clear_all(&mut self, net: &mut Net) {
        for q in self.queries.drain(..) {
            if let Some(h) = q.query {
                net.dns_cancel(h);
            }
        }
    }

    /// Cancel a deferred cs/dns read abandoned by a Tflush (the client died on a
    /// blocked dns read): the session whose deferred tag == oldtag has its query
    /// cancelled + its deferred marker cleared, so poll_dns never sends a late
    /// Rread for the flushed tag. A no-op if no session holds that tag.
    fn cancel_dns_flush(&mut self, net: &mut Net, oldtag: u16) {
        if let Some(i) = self
            .queries
            .iter()
            .position(|q| q.deferred.map(|(t, _)| t) == Some(oldtag))
        {
            if let Some(h) = self.queries[i].query {
                net.dns_cancel(h);
            }
            self.queries[i].query = None;
            self.queries[i].deferred = None;
            self.queries[i].resp = Some(Vec::new());
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
        self.query_clear_all(net); // free cs/dns sessions + cancel queries (net-4b)
        self.pending_reads.clear(); // the held Rreads die with the connection (net-6a)
        self.pending_ready.clear(); // ... and the held readiness Rreads (net-6b)
        self.pending_connects.clear(); // ... and the held connect Rlopens (#257)
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
            // Cancel any deferred accept this fid held: clunking the half-open
            // listen fid must not strand its PendingAccept (net-3d F1).
            net.cancel_accept_fid(self.handle, fid);
            self.query_drop(net, fid); // free cs/dns session + cancel query (net-4b)
            self.pending_reads.retain(|pr| pr.fid != fid); // drop blocked reads (net-6a)
            self.pending_ready.retain(|pr| pr.fid != fid); // drop readiness probes (net-6b)
            self.pending_connects.retain(|pc| pc.fid != fid); // drop deferred connects (#257)
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
        // net-6b-2b: mark the per-connection `ready` file QTPOLL so the kernel
        // dev9p.poll probes it (a regular file is POSIX always-ready; only a
        // readiness file is probed). This is the central qid builder -- h_walk
        // (Rwalk qids), h_lopen (non-clone Rlopen qid), and h_getattr all route
        // through it, so the kernel's CACHED qid (set on walk + open) carries the
        // bit. P9_QTPOLL == 0x01 (additive; unmarked files unaffected).
        let kind = if net.is_dir(path) {
            p9::P9_QTDIR
        } else if is_conn(path) && conn_filekind(path) == FK_READY {
            p9::P9_QTFILE | p9::P9_QTPOLL
        } else {
            p9::P9_QTFILE
        };
        p9::Qid {
            kind,
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
        self.query_clear_all(net); // a Tversion also resets cs/dns sessions (net-4b)
        self.pending_reads.clear(); // ... and drops any blocked data reads (net-6a)
        self.pending_ready.clear(); // ... and any deferred readiness probes (net-6b)
        self.pending_connects.clear(); // ... and any deferred connect opens (#257)
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
            // Mark the fid busy (net-3d F1): a committed-pending listen fid must
            // not be walked-from or re-opened (h_walk/h_lopen reject opened fids),
            // which would otherwise strand the PendingAccept. A clunk is handled
            // separately by fid_clunk -> cancel_accept_fid. complete_accept's
            // fid_set rebinds this fid onto the accepted ctl (fid_set ignores
            // opened), so the busy mark does not block the legitimate completion.
            if let Some(slot) = self.fids[i].as_mut() {
                slot.opened = true;
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
            P_ICMP_CLONE => Some(PROTO_ICMP),
            _ => None,
        };
        if let Some(proto) = clone_proto {
            let minted = match proto {
                PROTO_UDP => net.udp_clone(),
                PROTO_ICMP => net.icmp_clone(),
                _ => net.tcp_clone(),
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
            // Opening a TCP `data` file on a still-connecting socket DEFERS the
            // Rlopen until the handshake reaches ESTABLISHED (#257): the data
            // stream is unusable while the socket is SynSent, and a real outbound
            // connection has RTT, so an immediate Rlopen lets the client write
            // into a SynSent socket (send fails). poll_connects sends the held
            // Rlopen on ESTABLISHED (or Rlerror on failure/timeout). Loopback
            // establishes within a poll -> resolves on the first poll. UDP/ICMP
            // have no handshake (ConnectState::Ready), and an accepted TCP M is
            // already ESTABLISHED -> both reply immediately below.
            if is_conn(f.path)
                && conn_filekind(f.path) == FK_DATA
                && net.tcp_connect_state(conn_n(f.path)) == ConnectState::Pending
            {
                if self.pending_connects.len() >= MAX_FIDS {
                    return self.err(tag, p9::E_NOMEM);
                }
                let mut nf = f;
                nf.opened = true; // bind the data fid now; the Rlopen is held
                self.fids[i] = Some(nf);
                self.pending_connects.push(PendingConnect {
                    fid: a.fid,
                    slot_n: conn_n(f.path),
                    tag,
                    path: f.path,
                    deadline_ms: net.now_ms() + CONNECT_TIMEOUT_MS,
                });
                self.defer = true; // dispatch emits no reply for this frame
                return Ok(0); // the 0 is ignored (defer is set)
            }
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
        // from the socket's rx buffer. BLOCKING (net-6a): bytes return at once;
        // an empty-but-open socket DEFERS (park a PendingRead, poll_data sends
        // the held Rread when data arrives); EOF returns 0. A stream has no
        // seekable offset, so the Tread offset is ignored.
        if is_conn(f.path) && conn_filekind(f.path) == FK_DATA {
            let n = conn_n(f.path);
            let cap = (self.msize as usize)
                .saturating_sub(p9::P9_HDR_LEN + 4)
                .min(a.count as usize)
                .min(DATA_CHUNK);
            // A 0-count read returns 0 at once (POSIX); never park it -- a
            // 0-length dequeue is Ok(0) even on a readable socket, which would
            // otherwise look like WouldBlock and block forever.
            if cap == 0 {
                return p9::build_rread(&mut self.out_buf, tag, &[]);
            }
            let mut scratch = [0u8; DATA_CHUNK];
            match net.data_recv_outcome(n, &mut scratch[..cap]) {
                // Data now: the fast path -- a recv that finds bytes never blocks.
                RecvOutcome::Data(k) => {
                    return p9::build_rread(&mut self.out_buf, tag, &scratch[..k])
                }
                // Genuine end of stream: recv() returns 0.
                RecvOutcome::Eof => return p9::build_rread(&mut self.out_buf, tag, &[]),
                // Open but empty: BLOCK. Park the read; poll_data delivers the
                // held Rread when bytes arrive (or 0 on EOF). Bounded per
                // connection so a client cannot pin unbounded reads (#65 floor).
                RecvOutcome::WouldBlock => {
                    if self.pending_reads.len() >= MAX_FIDS {
                        return self.err(tag, p9::E_PROTO);
                    }
                    self.pending_reads.push(PendingRead {
                        fid: a.fid,
                        slot_n: n,
                        tag,
                        cap,
                    });
                    self.defer = true;
                    return Ok(0); // ignored: dispatch returns Disp::Deferred
                }
            }
        }
        // The `ready` file is the net-6b dev9p.poll readiness probe: report the
        // satisfied poll revents for the mask carried in the Tread OFFSET, WITHOUT
        // consuming socket data. A non-zero revents replies at once (the socket is
        // ready for the requested events); a zero revents DEFERS -- park a
        // PendingReady, poll_ready sends the held Rread when the socket becomes
        // ready per the mask (so a poll(POLLIN) on a writable-but-empty socket
        // waits for data, never busy-loops). The reply is the revents as a u32 LE
        // (the kernel reads 4 bytes). Bounded per connection (#65 floor).
        if is_conn(f.path) && conn_filekind(f.path) == FK_READY {
            let n = conn_n(f.path);
            let mask = a.offset as u16; // the requested poll events (POLLIN|POLLOUT)
            let revents = net.check_ready(n, mask);
            if revents != 0 {
                let bytes = (revents as u32).to_le_bytes();
                let want = (self.msize as usize)
                    .saturating_sub(p9::P9_HDR_LEN + 4)
                    .min(a.count as usize)
                    .min(4);
                return p9::build_rread(&mut self.out_buf, tag, &bytes[..want]);
            }
            if self.pending_ready.len() >= MAX_FIDS {
                return self.err(tag, p9::E_PROTO);
            }
            self.pending_ready.push(PendingReady {
                fid: a.fid,
                slot_n: n,
                tag,
                mask,
            });
            self.defer = true;
            return Ok(0); // ignored: dispatch returns Disp::Deferred
        }
        if f.path == P_CS || f.path == P_DNS {
            // cs/dns response (net-4a/4b): drain the per-fid answer via a per-fid
            // cursor (the Tread offset is ignored, like the FK_DATA stream). If a
            // DNS query is still in flight the read DEFERS (held until resolved).
            // A read before any write yields 0 bytes.
            let cap = (self.msize as usize)
                .saturating_sub(p9::P9_HDR_LEN + 4)
                .min(a.count as usize);
            return self.query_read(net, a.fid, cap, tag);
        }
        if f.path == P_SUMMARY {
            // net-7b: the /net/summary rollup. A multi-connection table can
            // exceed the fixed Content cap, so render into a Vec and offset-slice
            // it exactly like the static file_content path below. Re-rendered per
            // Tread; a `cat` fits the whole rollup in one msize frame (coherent),
            // and a paginated read sees a stable byte stream as long as no slot
            // is freed mid-read -- the same property the per-protocol stats carry.
            let body = net.render_summary();
            let off = a.offset as usize;
            let avail: &[u8] = if off >= body.len() { &[] } else { &body[off..] };
            let cap = (self.msize as usize).saturating_sub(p9::P9_HDR_LEN + 4);
            let want = (a.count as usize).min(cap).min(avail.len());
            return p9::build_rread(&mut self.out_buf, tag, &avail[..want]);
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
                P_TCP_CLONE | P_UDP_CLONE | P_ICMP_CLONE | P_CS | P_DNS | P_IPIFC_0_CTL => FILE_RW,
                _ if is_conn(f.path) => match conn_filekind(f.path) {
                    // FK_LISTEN is opened ORDWR by the accept (net-3a): its fid is
                    // REBOUND onto the accepted connection's ctl (rw -- `shutdown`
                    // writes it), so it MUST be writable, else the kernel's A-3
                    // dev9p perm_check denies the W and the open fails before the
                    // Tlopen ever reaches h_lopen (the #239 live-accept break).
                    FK_CTL | FK_DATA | FK_LISTEN => FILE_RW,
                    _ => FILE_RO,
                },
                _ => FILE_RO, // stats + summary
            };
            // P_SUMMARY's bytes come from render_summary (a Vec), not the fixed
            // Content of file_content -- report its true length so a stat is
            // accurate (a reader that loops to EOF is unaffected either way).
            let size = if f.path == P_SUMMARY {
                net.render_summary().len() as u64
            } else {
                net.file_content(f.path).as_slice().len() as u64
            };
            (m, 1u64, size)
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
    /// interrupt on a thread blocked in a `listen` open or a deferred dns read).
    /// Cancel any deferred accept OR deferred dns read held under `oldtag` -- no
    /// late Rlopen/Rread, no connection minted, no dns slot leaked -- then Rflush.
    /// Per 9P the client reuses `oldtag` only after this Rflush, so the held tag
    /// is reclaimed cleanly (no tag leak in the kernel's outstanding-table).
    fn h_flush(&mut self, net: &mut Net, tmsg: &[u8], tag: u16) -> Result<usize, ()> {
        let a = match p9::parse_tflush(tmsg) {
            Ok(a) => a,
            Err(_) => return self.err(tag, p9::E_PROTO),
        };
        net.cancel_accept_tag(self.handle, a.oldtag);
        self.cancel_dns_flush(net, a.oldtag); // net-4b: drop a deferred dns read
        self.pending_reads.retain(|pr| pr.tag != a.oldtag); // net-6a: drop a blocked read
        self.pending_ready.retain(|pr| pr.tag != a.oldtag); // net-6b: drop a readiness probe
        self.pending_connects.retain(|pc| pc.tag != a.oldtag); // #257: drop a deferred connect
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
        if f.path == P_CS || f.path == P_DNS {
            // F1 guard (net-4d): a re-write while a read is deferred on this fid
            // would drop the held tag (query_begin -> query_drop discards the
            // Query incl. its deferred marker). Reject it instead, so the
            // deferred read completes normally (I-9: no held Rread is lost). The
            // single-threaded normal flow (write -> read -> the read completes ->
            // re-write) never trips this; only a concurrent thread2-write while
            // thread1's read blocks does.
            if self.fid_has_deferred(a.fid) {
                return self.err(tag, p9::E_PROTO);
            }
            // cs/dns are request/response files (net-4a/4b): the write IS the
            // query. Begin the resolution now (numeric/ndb fill the answer
            // synchronously; a name needing DNS starts a query the read defers
            // on). The whole write is consumed even when unresolvable -- an
            // unresolved name yields an empty answer, the Plan 9 "no reachable
            // path" signal (the reader sees 0 bytes), never an error frame.
            self.query_begin(net, a.fid, f.path, a.data);
            return p9::build_rwrite(&mut self.out_buf, tag, a.count);
        }
        if f.path == P_IPIFC_0_CTL {
            // net-4c: the interface-config ctl (add/remove static config). A
            // malformed verb/operand is a hard EINVAL (not silently accepted).
            return match net.ipifc_ctl(a.data) {
                Ok(()) => p9::build_rwrite(&mut self.out_buf, tag, a.count),
                Err(()) => self.err(tag, p9::E_INVAL),
            };
        }
        if !is_conn(f.path) {
            return self.err(tag, p9::E_INVAL); // other static nodes (stats/clone) are read-only
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
                // ICMP is portless: dial a bare IPv4 (any `!suffix` is ignored).
                // TCP/UDP dial the full `ip!port`.
                let r = if net.slot_proto(n) == Some(PROTO_ICMP) {
                    let ipbytes = arg.split(|&b| b == b'!').next().unwrap_or(arg);
                    match parse_ipv4(ipbytes) {
                        Some(ip) => net.ctl_connect(n, ip, 0),
                        None => return self.err(tag, p9::E_INVAL),
                    }
                } else {
                    match parse_dial(arg) {
                        Some((ip, port)) => net.ctl_connect(n, ip, port),
                        None => return self.err(tag, p9::E_INVAL),
                    }
                };
                match r {
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
