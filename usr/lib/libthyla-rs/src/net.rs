//! Native TCP networking over netd's `/net` 9P files (net-6a-3).
//!
//! Thylacine has no socket syscalls by commitment (ARCH 11.5; NOVEL #1): a
//! native program reaches the network exactly as a Plan 9 program does -- by
//! walking netd's `/net` connection-server tree (NET-DESIGN 3.2/4) with
//! `SYS_open`/`read`/`write`/`close`. This module is the native libthyla-rs
//! analog of the pouch AF_INET boundary-line (`0016-pouch-net-sockets.patch`);
//! both speak the SAME `/net` protocol netd serves. It is a *client* of `/net`
//! (a buggy client corrupts only its own state -- the kernel + netd validate
//! every operation), so it composes I-1/I-5/I-23/I-28 (the namespace firewall:
//! a Proc reaches only the `/net` its territory grants) and adds no invariant.
//!
//! The connection lifecycle (TCP):
//! - dial: open `/net/tcp/clone` (the opened fid IS the new connection's `ctl`;
//!   reading it yields the connection number N), write `connect a.b.c.d!port`
//!   to `ctl`, open `/net/tcp/N/data` (blocks to ESTABLISHED).
//! - listen: open `/net/tcp/clone`, write `announce *!port` (or
//!   `announce a.b.c.d!port`) to `ctl`.
//! - accept: open `/net/tcp/N/listen` (blocks until an inbound call; the opened
//!   fid is the new connection M's `ctl`), read M, open `/net/tcp/M/data`.
//! - data: read/write `data` (read blocks until bytes or EOF -- net-6a-1).
//! - close: drop the `TcpStream`/`TcpListener` (closes ctl + data).

use crate::err::{Error, Result};
use crate::fs::{File, OpenOptions};
use crate::io::{Read, Write};
use crate::loom::{self, BufReg, Cqe, Ring, Sqe};
use crate::poll::{PollEvents, PollSet, PollTimeout};
use crate::t_weft_map;
use crate::weft;
use alloc_crate::format;
use alloc_crate::string::String;
use alloc_crate::vec::Vec;
use core::fmt;
use core::time::Duration;

/// Plan 9 wire separator between host and port (`a.b.c.d!port`). The native API
/// presents the Rust `:` convention; we translate only at the `/net` boundary.
const WIRE_SEP: u8 = b'!';

/// An IPv4 address. The native API uses dotted-quad text on the surface and the
/// Plan 9 `!port` form only on the `/net` wire.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct Ipv4Addr {
    octets: [u8; 4],
}

impl Ipv4Addr {
    pub const LOCALHOST: Ipv4Addr = Ipv4Addr {
        octets: [127, 0, 0, 1],
    };
    pub const UNSPECIFIED: Ipv4Addr = Ipv4Addr {
        octets: [0, 0, 0, 0],
    };

    pub const fn new(a: u8, b: u8, c: u8, d: u8) -> Ipv4Addr {
        Ipv4Addr {
            octets: [a, b, c, d],
        }
    }

    pub const fn octets(&self) -> [u8; 4] {
        self.octets
    }

    pub const fn is_unspecified(&self) -> bool {
        let o = self.octets;
        o[0] == 0 && o[1] == 0 && o[2] == 0 && o[3] == 0
    }

    /// Parse a bare dotted-quad (`a.b.c.d`). Each octet must be 0..=255 with no
    /// leading-zero ambiguity beyond a lone `0`; a malformed string is rejected
    /// (`InvalidArgument`), never silently coerced.
    pub fn parse(s: &str) -> Result<Ipv4Addr> {
        let mut octets = [0u8; 4];
        let mut parts = 0usize;
        for field in s.split('.') {
            if parts >= 4 || field.is_empty() || field.len() > 3 {
                return Err(Error::InvalidArgument);
            }
            let mut v: u32 = 0;
            for b in field.bytes() {
                if !b.is_ascii_digit() {
                    return Err(Error::InvalidArgument);
                }
                v = v * 10 + (b - b'0') as u32;
            }
            if v > 255 {
                return Err(Error::InvalidArgument);
            }
            octets[parts] = v as u8;
            parts += 1;
        }
        if parts != 4 {
            return Err(Error::InvalidArgument);
        }
        Ok(Ipv4Addr { octets })
    }
}

impl fmt::Display for Ipv4Addr {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let o = self.octets;
        write!(f, "{}.{}.{}.{}", o[0], o[1], o[2], o[3])
    }
}

/// An `IPv4 address : port` endpoint. `Display`/`parse` use the Rust `:port`
/// convention; the `/net` wire form (`a.b.c.d!port`) is produced only by the
/// internal `wire()` helper at the netd boundary.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct SocketAddrV4 {
    ip: Ipv4Addr,
    port: u16,
}

impl SocketAddrV4 {
    pub const fn new(ip: Ipv4Addr, port: u16) -> SocketAddrV4 {
        SocketAddrV4 { ip, port }
    }

    pub const fn ip(&self) -> Ipv4Addr {
        self.ip
    }

    pub const fn port(&self) -> u16 {
        self.port
    }

    /// Parse `a.b.c.d:port`.
    pub fn parse(s: &str) -> Result<SocketAddrV4> {
        let (host, port) = s.rsplit_once(':').ok_or(Error::InvalidArgument)?;
        Ok(SocketAddrV4 {
            ip: Ipv4Addr::parse(host)?,
            port: parse_port(port)?,
        })
    }

    /// The Plan 9 `/net` wire form: `a.b.c.d!port`.
    fn wire(&self) -> String {
        format!("{}{}{}", self.ip, WIRE_SEP as char, self.port)
    }
}

impl fmt::Display for SocketAddrV4 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}:{}", self.ip, self.port)
    }
}

fn parse_port(s: &str) -> Result<u16> {
    if s.is_empty() || s.len() > 5 {
        return Err(Error::InvalidArgument);
    }
    let mut v: u32 = 0;
    for b in s.bytes() {
        if !b.is_ascii_digit() {
            return Err(Error::InvalidArgument);
        }
        v = v * 10 + (b - b'0') as u32;
    }
    if v > u16::MAX as u32 {
        return Err(Error::InvalidArgument);
    }
    Ok(v as u16)
}

/// Open `/net/<proto>/clone` for read+write; the opened fid is the new
/// connection's `ctl`. Read it to learn the connection number N. Returns
/// `(ctl, N)`. `proto` is `"tcp"` or `"udp"` (the `/net` connection-server
/// trees netd serves).
fn clone_conn(proto: &str) -> Result<(File, u32)> {
    let mut ctl = OpenOptions::new()
        .read(true)
        .write(true)
        .open(format!("/net/{}/clone", proto))?;
    let n = read_decimal(&mut ctl)?;
    Ok((ctl, n))
}

/// Read the leading ASCII-decimal number from a freshly-opened `ctl`/`listen`
/// fid (netd serves the connection number as decimal text). Bounded by a small
/// fixed buffer; a non-numeric or empty reply is a protocol error.
fn read_decimal(f: &mut File) -> Result<u32> {
    let mut buf = [0u8; 16];
    let n = f.read(&mut buf)?;
    let s = &buf[..n];
    let mut v: u32 = 0;
    let mut seen = false;
    for &b in s {
        if !b.is_ascii_digit() {
            break;
        }
        v = v
            .checked_mul(10)
            .and_then(|v| v.checked_add((b - b'0') as u32))
            .ok_or(Error::InvalidArgument)?;
        seen = true;
    }
    if !seen {
        return Err(Error::Io);
    }
    Ok(v)
}

/// Open `/net/<proto>/<n>/<file>` with the given access. `read`+`write` selects
/// ORDWR (Plan 9 OWRITE does not grant read).
fn open_conn_file(proto: &str, n: u32, file: &str, read: bool, write: bool) -> Result<File> {
    OpenOptions::new()
        .read(read)
        .write(write)
        .open(format!("/net/{}/{}/{}", proto, n, file))
}

/// Read and parse a `local`/`remote` endpoint file (`a.b.c.d!port`).
fn read_endpoint(proto: &str, n: u32, file: &str) -> Result<SocketAddrV4> {
    let mut f = open_conn_file(proto, n, file, true, false)?;
    let mut buf = [0u8; 48];
    let got = f.read(&mut buf)?;
    let line = core::str::from_utf8(&buf[..got]).map_err(|_| Error::Io)?;
    let line = line.trim();
    let (host, port) = line.rsplit_once(WIRE_SEP as char).ok_or(Error::Io)?;
    Ok(SocketAddrV4 {
        ip: Ipv4Addr::parse(host).map_err(|_| Error::Io)?,
        port: parse_port(port).map_err(|_| Error::Io)?,
    })
}

/// An established TCP connection. Holds the connection's `ctl` (for the lifetime
/// + `shutdown`) and `data` (the byte stream) fids; both close on drop.
pub struct TcpStream {
    ctl: File,
    data: File,
    n: u32,
}

impl TcpStream {
    /// Active open: dial `addr` and block until the connection is ESTABLISHED.
    pub fn connect(addr: SocketAddrV4) -> Result<TcpStream> {
        let (mut ctl, n) = clone_conn("tcp")?;
        ctl.write_all(format!("connect {}", addr.wire()).as_bytes())?;
        // Opening `data` blocks until ESTABLISHED (or fails on RST/refused).
        let data = open_conn_file("tcp", n, "data", true, true)?;
        Ok(TcpStream { ctl, data, n })
    }

    /// Active open with a bounded wait. Like `connect`, but instead of blocking
    /// the `data` open until ESTABLISHED (which has no deadline short of netd's
    /// 15 s connect timeout), it polls the QTPOLL `ready` sibling for POLLOUT --
    /// netd's `check_ready` gates POLLOUT on `tcp::can_send()`, true only once
    /// ESTABLISHED (a SynSent socket reports neither, net-6b) -- with `timeout`.
    /// Returns `TimedOut` if the handshake does not complete in time (the
    /// half-open `ctl` closes on drop). For a best-effort prober that must never
    /// block on an unreachable peer (NET-PERF M6, the slirp `guestfwd` path).
    pub fn connect_timeout(addr: SocketAddrV4, timeout: Duration) -> Result<TcpStream> {
        let (mut ctl, n) = clone_conn("tcp")?;
        ctl.write_all(format!("connect {}", addr.wire()).as_bytes())?;
        // The readiness sibling is openable while the socket is still SynSent;
        // POLLOUT fires only on ESTABLISHED (can_send). A RST leaves the socket
        // !can_send, so a refused dial also resolves to the timeout (bounded).
        let ready = open_conn_file("tcp", n, "ready", true, false)?;
        let mut ps = PollSet::new();
        ps.add_raw(ready.as_raw_fd(), PollEvents::WRITE);
        let ms = timeout.as_millis().min(u32::MAX as u128) as u32;
        let established = ps
            .poll(PollTimeout::Millis(ms))
            .map(|r| r.into_iter().any(|e| e.is_writable()))
            .map_err(|_| Error::Io)?;
        if !established {
            return Err(Error::TimedOut);
        }
        // ESTABLISHED: the `data` open returns at once (no further blocking).
        let data = open_conn_file("tcp", n, "data", true, true)?;
        Ok(TcpStream { ctl, data, n })
    }

    /// The connection number N (its `/net/tcp/N` directory).
    pub fn conn_n(&self) -> u32 {
        self.n
    }

    /// Read from the connection. Blocks until bytes arrive or the peer closes
    /// (net-6a-1); a `0` return is EOF (the peer sent FIN).
    pub fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        self.data.read(buf)
    }

    /// Write to the connection.
    pub fn write(&mut self, buf: &[u8]) -> Result<usize> {
        self.data.write(buf)
    }

    /// Write the whole buffer, retrying short writes.
    pub fn write_all(&mut self, buf: &[u8]) -> Result<()> {
        self.data.write_all(buf)
    }

    /// Half-close the send side (`shutdown(SHUT_WR)`): netd's `hangup` ctl verb
    /// is a smoltcp `tcp::close` = a send-side FIN, so the connection keeps
    /// receiving until the peer FINs (net-6a-2).
    pub fn shutdown(&mut self) -> Result<()> {
        self.ctl.write_all(b"hangup")?;
        Ok(())
    }

    /// The peer's address (`/net/tcp/N/remote`).
    pub fn peer_addr(&self) -> Result<SocketAddrV4> {
        read_endpoint("tcp", self.n, "remote")
    }

    /// This end's address (`/net/tcp/N/local`).
    pub fn local_addr(&self) -> Result<SocketAddrV4> {
        read_endpoint("tcp", self.n, "local")
    }

    /// The raw `data` fd (for `poll`/Loom registration).
    pub fn as_raw_fd(&self) -> i32 {
        self.data.as_raw_fd()
    }

    /// Open the QTPOLL `ready` sibling (`/net/tcp/N/ready`) for a bounded poll
    /// on readiness (net-6b dev9p.poll): POLLIN fires when bytes are queued,
    /// POLLOUT when the send window has room. A bulk sender polls POLLOUT before
    /// retrying a write that returned 0 -- netd's data write is non-blocking, so
    /// a full send buffer returns a 0-count `Rwrite`, not a deferred reply.
    pub fn ready_fd(&self) -> Result<File> {
        open_conn_file("tcp", self.n, "ready", true, false)
    }
}

impl Read for TcpStream {
    fn read(&mut self, buf: &mut [u8]) -> Result<usize> {
        self.data.read(buf)
    }
}

impl Write for TcpStream {
    fn write(&mut self, buf: &[u8]) -> Result<usize> {
        self.data.write(buf)
    }
    fn flush(&mut self) -> Result<()> {
        self.data.flush()
    }
}

/// A listening TCP socket. Holds the listener connection's `ctl`/N; `accept`
/// opens a fresh `listen` fid per call (the listener stays armed).
pub struct TcpListener {
    ctl: File,
    n: u32,
    local: SocketAddrV4,
}

impl TcpListener {
    /// Announce `addr` (the server-side passive open). A `0.0.0.0` host
    /// announces the wildcard (`announce *!port`); a concrete host pins the
    /// local address. The port must be non-zero (netd's `announce` needs a
    /// concrete port).
    pub fn bind(addr: SocketAddrV4) -> Result<TcpListener> {
        if addr.port() == 0 {
            return Err(Error::InvalidArgument);
        }
        let (mut ctl, n) = clone_conn("tcp")?;
        let verb = if addr.ip().is_unspecified() {
            format!("announce *!{}", addr.port())
        } else {
            format!("announce {}", addr.wire())
        };
        ctl.write_all(verb.as_bytes())?;
        Ok(TcpListener {
            ctl,
            n,
            local: addr,
        })
    }

    /// Block until an inbound connection arrives, then return the new
    /// `TcpStream` and the peer's address. The listener stays armed for the
    /// next `accept`.
    pub fn accept(&self) -> Result<(TcpStream, SocketAddrV4)> {
        // Opening `listen` blocks (net-3a deferred reply); the opened fid is
        // the new connection M's ctl, and reading it yields M.
        let mut mctl = open_conn_file("tcp", self.n, "listen", true, true)?;
        let m = read_decimal(&mut mctl)?;
        let data = open_conn_file("tcp", m, "data", true, true)?;
        let peer = read_endpoint("tcp", m, "remote")
            .unwrap_or(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 0));
        Ok((
            TcpStream {
                ctl: mctl,
                data,
                n: m,
            },
            peer,
        ))
    }

    /// The bound local address.
    pub fn local_addr(&self) -> Result<SocketAddrV4> {
        // Prefer netd's view; fall back to the announced address.
        read_endpoint("tcp", self.n, "local").or(Ok(self.local))
    }

    /// The listener connection number N.
    pub fn conn_n(&self) -> u32 {
        self.n
    }

    /// The raw `ctl` fd (for Loom registration of the listener).
    pub fn as_raw_fd(&self) -> i32 {
        self.ctl.as_raw_fd()
    }
}

/// A connected UDP socket over netd's `/net/udp` tree (net-7a). The Plan 9
/// datagram model is connection-oriented at the `/net` layer: `connect`
/// records the remote (and binds a local ephemeral port), `data` writes send a
/// datagram to that remote, `data` reads dequeue one received datagram. Holds
/// the `ctl` (lifetime) and `data` (the datagram channel) fids; both close on
/// drop.
///
/// `recv` BLOCKS until a datagram arrives (net-6a-1) -- a client wanting a
/// bounded wait polls `ready_fd()` (the QTPOLL readiness sibling, net-6b) with
/// a timeout first, then `recv` only when POLLIN fired. The SNTP client is the
/// first consumer.
pub struct UdpSocket {
    #[allow(dead_code)]
    ctl: File,
    data: File,
    n: u32,
}

impl UdpSocket {
    /// Dial `/net/udp` and `connect` to `peer` (binds a local ephemeral port,
    /// making the socket sendable). A subsequent `send` goes to `peer`.
    pub fn connect(peer: SocketAddrV4) -> Result<UdpSocket> {
        let (mut ctl, n) = clone_conn("udp")?;
        ctl.write_all(format!("connect {}", peer.wire()).as_bytes())?;
        let data = open_conn_file("udp", n, "data", true, true)?;
        Ok(UdpSocket { ctl, data, n })
    }

    /// Send a datagram to the connected remote. The whole buffer is one
    /// datagram (UDP has no coalescing); returns the bytes accepted.
    pub fn send(&mut self, buf: &[u8]) -> Result<usize> {
        self.data.write(buf)
    }

    /// Receive one datagram into `buf` (returns its length). BLOCKS until a
    /// datagram arrives (net-6a-1); poll `ready_fd()` first for a bounded wait.
    pub fn recv(&mut self, buf: &mut [u8]) -> Result<usize> {
        self.data.read(buf)
    }

    /// Open the QTPOLL `ready` sibling (`/net/udp/N/ready`) for a bounded poll
    /// on receive (net-6b dev9p.poll: POLLIN fires when a datagram is queued).
    pub fn ready_fd(&self) -> Result<File> {
        open_conn_file("udp", self.n, "ready", true, false)
    }

    /// The connected remote address (`/net/udp/N/remote`).
    pub fn peer_addr(&self) -> Result<SocketAddrV4> {
        read_endpoint("udp", self.n, "remote")
    }

    /// This end's bound address (`/net/udp/N/local`).
    pub fn local_addr(&self) -> Result<SocketAddrV4> {
        read_endpoint("udp", self.n, "local")
    }

    /// The connection number N (its `/net/udp/N` directory).
    pub fn conn_n(&self) -> u32 {
        self.n
    }

    /// The raw `data` fd (for `poll`/Loom registration).
    pub fn as_raw_fd(&self) -> i32 {
        self.data.as_raw_fd()
    }
}

/// A connected ICMP echo (ping) socket over netd's `/net/icmp` tree (net-3c).
/// ICMP is portless and connectionless: `connect` records the ping target (the
/// socket was bound to a rotating Echo identifier at clone), a `data` write
/// sends one EchoRequest carrying the payload, a `data` read returns the
/// matching EchoReply's payload (smoltcp filters replies to the socket's bound
/// ident). Holds the `ctl` (lifetime) + `data` (the echo channel) fids; both
/// close on drop. A 127.x target migrates the socket onto the in-guest loopback
/// stack (net-8a), which auto-answers an echo to its own address -- the
/// deterministic in-guest round-trip.
///
/// `recv` BLOCKS until a reply arrives. For a bounded wait -- the only sane mode
/// for `ping`, since a host that never answers must not hang -- poll `ready_fd()`
/// (the QTPOLL readiness sibling, net-6b) with a timeout first, then `recv` only
/// when POLLIN fired. `ping` is the first consumer.
pub struct IcmpSocket {
    #[allow(dead_code)]
    ctl: File,
    data: File,
    n: u32,
}

impl IcmpSocket {
    /// Dial `/net/icmp` and `connect` to `target` (records the ping target; ICMP
    /// is portless so the ctl verb is a bare dotted-quad). A subsequent `send`
    /// emits an EchoRequest to `target`.
    pub fn connect(target: Ipv4Addr) -> Result<IcmpSocket> {
        let (mut ctl, n) = clone_conn("icmp")?;
        ctl.write_all(format!("connect {}", target).as_bytes())?;
        let data = open_conn_file("icmp", n, "data", true, true)?;
        Ok(IcmpSocket { ctl, data, n })
    }

    /// Send one EchoRequest carrying `payload`. netd wraps it (the bound ident +
    /// a rotating sequence) and the iface computes the checksum on egress.
    /// Returns the payload bytes accepted.
    pub fn send(&mut self, payload: &[u8]) -> Result<usize> {
        self.data.write(payload)
    }

    /// Receive the matching EchoReply's payload into `buf` (its length). BLOCKS
    /// until a reply arrives; poll `ready_fd()` first for a bounded wait.
    pub fn recv(&mut self, buf: &mut [u8]) -> Result<usize> {
        self.data.read(buf)
    }

    /// Open the QTPOLL `ready` sibling (`/net/icmp/N/ready`) for a bounded poll
    /// on receive (POLLIN fires when an EchoReply is queued).
    pub fn ready_fd(&self) -> Result<File> {
        open_conn_file("icmp", self.n, "ready", true, false)
    }

    /// The connection number N (its `/net/icmp/N` directory).
    pub fn conn_n(&self) -> u32 {
        self.n
    }

    /// The raw `data` fd (for `poll`/Loom registration).
    pub fn as_raw_fd(&self) -> i32 {
        self.data.as_raw_fd()
    }
}

/// The minimal single-threaded echo-server lifecycle (NET-DESIGN 4): bind, then
/// accept-and-echo each connection in turn. Two clients that each send-then-recv
/// are both served correctly (the connections are concurrently ESTABLISHED;
/// netd's listener backlog holds the second SYN while the first is serviced).
/// True I/O interleaving across many simultaneously-active streams wants a
/// thread per connection or the Loom-multishot async path (the net-8 server-path
/// refinement); this is the deterministic minimal demonstration.
pub fn echo_serve(addr: SocketAddrV4, max_conns: u32) -> Result<u32> {
    let listener = TcpListener::bind(addr)?;
    let mut served = 0u32;
    while served < max_conns {
        let (mut stream, _peer) = listener.accept()?;
        echo_one(&mut stream)?;
        served += 1;
    }
    Ok(served)
}

/// Echo a single connection until the peer half-closes (read returns 0).
fn echo_one(stream: &mut TcpStream) -> Result<()> {
    let mut buf = [0u8; 2048];
    loop {
        let n = stream.read(&mut buf)?;
        if n == 0 {
            return Ok(()); // EOF -- peer sent FIN.
        }
        stream.write_all(&buf[..n])?;
    }
}

/// Slurp up to `cap` bytes from a stream into a `Vec` (a small client helper).
pub fn read_to_vec(stream: &mut TcpStream, cap: usize) -> Result<Vec<u8>> {
    let mut out = Vec::new();
    let mut buf = [0u8; 2048];
    loop {
        let take = core::cmp::min(buf.len(), cap.saturating_sub(out.len()));
        if take == 0 {
            break;
        }
        let n = stream.read(&mut buf[..take])?;
        if n == 0 {
            break;
        }
        out.extend_from_slice(&buf[..n]);
    }
    Ok(out)
}

/// Resolve `host` to a `SocketAddrV4` on `port` via netd's connection server
/// `/net/cs` -- the Plan 9 name-resolution front door (numeric -> ndb-static ->
/// DNS; NET-DESIGN 5). A dotted-quad `host` resolves locally with no round-trip;
/// a name is dialed as `tcp!<host>!<port>` and the reply's `<clone> <ip>!<port>`
/// second field gives the address. `NotFound` on an empty / unresolvable reply,
/// `InvalidArgument` on a malformed one. The single resolution path every native
/// `/net` client (curl, wget, ping, https, nslookup) shares.
pub fn resolve(host: &str, port: u16) -> Result<SocketAddrV4> {
    if let Ok(ip) = Ipv4Addr::parse(host) {
        return Ok(SocketAddrV4::new(ip, port));
    }
    let mut cs = OpenOptions::new().read(true).write(true).open("/net/cs")?;
    cs.write_all(format!("tcp!{}!{}", host, port).as_bytes())?;
    let mut buf = [0u8; 128];
    let k = cs.read(&mut buf)?;
    if k == 0 {
        return Err(Error::NotFound);
    }
    let line = core::str::from_utf8(&buf[..k]).map_err(|_| Error::InvalidArgument)?;
    // Reply line: `<clone-file> <ip>!<port>`. The dial field carries ip!port.
    let dial = line.split_whitespace().nth(1).ok_or(Error::NotFound)?;
    let (ip_s, port_s) = dial
        .rsplit_once(WIRE_SEP as char)
        .ok_or(Error::InvalidArgument)?;
    let ip = Ipv4Addr::parse(ip_s)?;
    let rport: u16 = port_s.trim().parse().map_err(|_| Error::InvalidArgument)?;
    Ok(SocketAddrV4::new(ip, rport))
}

// =============================================================================
// Weft -- the native zero-copy dataplane flow (NET-THROUGHPUT 6; I-37; Weft-6c-2).
// =============================================================================

/// An opaque handle to a submitted [`WeftFlow::push`] / [`WeftFlow::pop`],
/// redeemed by [`WeftFlow::wait`] (the Demikernel qtoken).
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub struct Ticket(u64);

/// The result of a completed [`WeftFlow`] push/pop: the byte count moved through
/// the shared ring (a send's accepted bytes, or a recv's delivered bytes -- 0 on a
/// recv means EOF). For a recv the bytes are in [`WeftFlow::rx_buf`].
#[derive(Copy, Clone, Debug)]
pub struct Completion {
    bytes: usize,
    recv: bool,
}

impl Completion {
    /// Bytes sent (push) or received into the ring (pop). 0 on a pop = EOF.
    #[inline]
    pub fn bytes(&self) -> usize {
        self.bytes
    }

    /// `true` if this completed a [`WeftFlow::pop`] (the bytes are in `rx_buf`).
    #[inline]
    pub fn is_recv(&self) -> bool {
        self.recv
    }
}

#[derive(Copy, Clone)]
struct Inflight {
    tok: u64,
    recv: bool,
}

/// A Weft zero-copy dataplane flow bound to a [`TcpStream`]'s `/net` data fid
/// (NET-THROUGHPUT 6; I-37). It maps the per-flow shared ring (`SYS_WEFT_MAP`),
/// sets up a Loom ring, and registers `(data fd, the whole shared ring)` as the
/// Loom `(handle, buffer)` so push/pop ride the kernel weft fast-path -- a
/// `Tweftio` descriptor, no 9P-body copy, the bytes moving through the shared
/// ring in place. This is the native, Demikernel-shaped async API the
/// NET-THROUGHPUT §6 design names: `push`/`pop` submit (non-blocking), `wait`
/// blocks for the completion. **It rides Loom and mints no new ABI** -- a weft
/// flow is just a Loom ring whose registered buffer is a `SYS_WEFT_MAP`'d region,
/// which the kernel's `loom_submit_payload` detects and routes to `Tweftio`.
///
/// The park is the Loom CQ wait (NET-THROUGHPUT §6: "the consumer's park *is* the
/// Loom CQ wait, not a separate weft Rendez"). The readiness ring is the
/// syscall-free busy-poll edge: [`WeftFlow::rx_ready_seq`] observes netd's
/// `ready_seq` bump (after it writes RX bytes into the ring) without a syscall.
///
/// **Buffer-lifetime contract (the Weft-7 audit surface).** The shared ring's
/// payload region is a SINGLE buffer, so exactly ONE op (push XOR pop) is in
/// flight at a time -- a second push/pop before its `wait` returns
/// [`Error::WouldBlock`]. The TX/RX region must not be mutated while an op is in
/// flight (between submit and the CQE); the borrow checker enforces it -- `tx_buf`
/// / `rx_buf` borrow `self`, `push` / `pop` / `wait` take `&mut self`, and no ring
/// slice outlives a submit. The kernel pins the ring Burrow (the #847 payload-
/// buffer ref) from submit to reap and validates the slice at submit against its
/// private `weft_ring_view` (I-30), so a buggy producer corrupts only its own
/// in-flight bytes -- never the kernel or netd. `WeftFlow` is auto-`!Send`+`!Sync`
/// (its `Ring` field holds raw `*mut` pointers), so the raw `ring_va` mapping can
/// never be shared across threads -- the single-in-flight contract is per-instance
/// and the `&mut self` discipline keeps even an aliased `&self` reader (`rx_buf`)
/// from overlapping a submit.
///
/// A weft flow is self-sufficient after `open`: the Loom handle registration takes
/// its own I-30 pin on the data fid, so push/pop keep working even if the
/// originating `TcpStream` is dropped (the kernel keeps the flow + ring alive until
/// the Loom registrations drop with the `WeftFlow`). Small/control transfers should
/// stay on `TcpStream::read`/`write` (the §4.8 hybrid threshold); weft is for bulk.
pub struct WeftFlow {
    ring: Ring,                 // the Loom SQ/CQ driving the async Tweftio ops
    ring_va: u64,               // the SYS_WEFT_MAP'd shared ring base (this Proc's VA)
    geom: weft::RingGeom,       // payload_off / payload_size / ready_off (the mirror)
    tok_seq: u64,               // monotonic user_data for push/pop tickets
    inflight: Option<Inflight>, // the single op in flight (one payload region)
}

impl WeftFlow {
    /// The fixed Loom registration indices: handle 0 = the data fd, buffer 0 = the
    /// whole shared ring. (One flow per Loom ring at v1.0.)
    const HANDLE: u32 = 0;
    const BUFIDX: u32 = 0;
    /// The Loom SQ depth. Single-in-flight, so a tiny ring is ample.
    const RING_ENTRIES: u32 = 8;

    /// Bind a Weft flow to an established [`TcpStream`]'s `/net` data fid: map the
    /// per-flow ring (the first map issues `Tweft` -> netd allocates + registers +
    /// answers `Rweft`, idempotent thereafter), read its geometry mirror, set up a
    /// Loom ring, and register `(data fd, whole ring)`. The stream must already be
    /// connected (its `data` fid open).
    pub fn open(stream: &TcpStream) -> Result<WeftFlow> {
        Self::open_fd(stream.as_raw_fd())
    }

    /// `open` over a raw `/net` data fd (the internal path; `open` is the public
    /// entry over a `TcpStream`). The fd must be an open `/net/<proto>/N/data` fid.
    fn open_fd(data_fd: i32) -> Result<WeftFlow> {
        // Map the per-flow ring. SYS_WEFT_MAP returns ring_va (> 0) or -1; the first
        // call issues Tweft(F) on the shared /net dev9p client, netd allocates +
        // registers the ring and answers Rweft(share_id, geom), the kernel claims
        // the share + maps it into this Proc. Idempotent on the same fd.
        let rc = unsafe { t_weft_map(data_fd as u64, 0) };
        if rc <= 0 {
            return Err(Error::Io);
        }
        let ring_va = rc as u64;
        // The geometry mirror netd wrote at grant (magic-validated). ring_size =
        // payload_off + payload_size == netd's whole-ring Burrow size, which the
        // kernel's weft-bound detection requires the registered buffer length to
        // equal exactly (buf == the flow's ring Burrow && buf_reg_len == ring_size).
        let geom = unsafe { weft::read_ring_geom(ring_va as *const u8) }.ok_or(Error::Io)?;
        // A Loom ring to drive the async Tweftio ops, and the two registrations the
        // kernel routing keys on: the data fid (the I-30 fid pin) as handle 0, the
        // WHOLE shared ring as buffer 0.
        let ring = Ring::setup(Self::RING_ENTRIES, 0)?;
        ring.register_handles(&[data_fd])?;
        ring.register_buffers(&[BufReg {
            va: ring_va,
            len: geom.ring_size as u64,
        }])?;
        Ok(WeftFlow {
            ring,
            ring_va,
            geom,
            tok_seq: 1,
            inflight: None,
        })
    }

    /// The ring's payload region as a writable slice -- fill it directly for a
    /// true zero-copy [`push`](WeftFlow::push) (no app->ring copy). Borrows `self`,
    /// so it cannot be held across a `push` / `wait`.
    pub fn tx_buf(&mut self) -> &mut [u8] {
        let base = (self.ring_va + self.geom.payload_off as u64) as *mut u8;
        // SAFETY: [ring_va+payload_off, +payload_size) is the payload region of this
        // Proc's own SYS_WEFT_MAP'd ring mapping (a live RW anon page span). The &mut
        // borrow is tied to &mut self, so no concurrent alias and no overlap with an
        // in-flight op (push/pop take &mut self).
        unsafe { core::slice::from_raw_parts_mut(base, self.geom.payload_size as usize) }
    }

    /// The ring's payload region as a read-only slice -- read the bytes a
    /// [`pop`](WeftFlow::pop) delivered (the first [`Completion::bytes`] are
    /// valid). Zero-copy: those bytes were written by netd into this shared
    /// mapping, never copied through a 9P body.
    pub fn rx_buf(&self) -> &[u8] {
        let base = (self.ring_va + self.geom.payload_off as u64) as *const u8;
        // SAFETY: as tx_buf, but a shared read-only borrow tied to &self.
        unsafe { core::slice::from_raw_parts(base, self.geom.payload_size as usize) }
    }

    /// The ring's payload-region capacity (the max single push/pop length).
    #[inline]
    pub fn payload_capacity(&self) -> usize {
        self.geom.payload_size as usize
    }

    /// Submit a zero-copy send of the first `len` bytes already in [`tx_buf`] (the
    /// Demikernel `push`: non-blocking; redeem with [`wait`](WeftFlow::wait)). The
    /// Loom WRITE is staged in the SQ now and submitted by `wait` (the io_uring
    /// submit-and-wait batch); the kernel routes it to the weft fast-path
    /// (`Tweftio` WRITE -- netd reads the slice in place + sends). Returns
    /// [`Error::WouldBlock`] if an op is already in flight (single payload region),
    /// [`Error::InvalidArgument`] if `len` is 0 or exceeds the payload capacity.
    pub fn push(&mut self, len: usize) -> Result<Ticket> {
        if self.inflight.is_some() {
            return Err(Error::WouldBlock);
        }
        if len == 0 || len > self.geom.payload_size as usize {
            return Err(Error::InvalidArgument);
        }
        let tok = self.next_tok();
        let sqe = Sqe::write(
            Self::HANDLE,
            0, // file offset -- ignored on a weft op (the kernel uses the ring offset)
            len as u32,
            Self::BUFIDX,
            self.geom.payload_off as u64, // buf_off = the ring payload-region start
            tok,
        );
        self.ring.try_submit(&sqe)?; // stage in the SQ (no kernel entry until wait)
        self.inflight = Some(Inflight { tok, recv: false });
        Ok(Ticket(tok))
    }

    /// Submit a recv of up to `max` bytes into the ring (the Demikernel `pop`:
    /// non-blocking; redeem with [`wait`](WeftFlow::wait), then read [`rx_buf`]).
    /// The Loom READ rides the weft read fast-path (`Tweftio` READ -- netd recvs in
    /// place into the ring). Returns [`Error::WouldBlock`] if an op is in flight.
    pub fn pop(&mut self, max: usize) -> Result<Ticket> {
        if self.inflight.is_some() {
            return Err(Error::WouldBlock);
        }
        let len = core::cmp::min(max, self.geom.payload_size as usize);
        if len == 0 {
            return Err(Error::InvalidArgument);
        }
        let tok = self.next_tok();
        let sqe = Sqe::read(
            Self::HANDLE,
            0,
            len as u32,
            Self::BUFIDX,
            self.geom.payload_off as u64,
            tok,
        );
        self.ring.try_submit(&sqe)?;
        self.inflight = Some(Inflight { tok, recv: true });
        Ok(Ticket(tok))
    }

    /// Block until `t`'s push/pop completes; return its [`Completion`]. Submits the
    /// staged SQE and parks on the Loom CQ wait (death-interruptible, no per-op
    /// deadline). A negative CQE result maps to the kernel errno. The op is no
    /// longer in flight on return (success or error), so a new push/pop may follow.
    pub fn wait(&mut self, t: Ticket) -> Result<Completion> {
        let inflight = match self.inflight {
            Some(i) if i.tok == t.0 => i,
            _ => return Err(Error::InvalidArgument), // not the outstanding op
        };
        // io_uring submit-and-wait: submit the single staged SQE + block for >= 1 CQE.
        self.ring.enter(1, 1, loom::ENTER_GETEVENTS)?;
        let cqe = self.reap_token(t.0)?;
        // Clear inflight BEFORE decoding the result: the op is done (even on an error
        // CQE), so a new push/pop may follow regardless of the outcome.
        self.inflight = None;
        let bytes = cqe.ok()? as usize;
        Ok(Completion {
            bytes,
            recv: inflight.recv,
        })
    }

    /// A blocking convenience: copy `data` into the ring + push + wait. Returns the
    /// bytes sent. `data.len()` must be `<= payload_capacity()`.
    pub fn send(&mut self, data: &[u8]) -> Result<usize> {
        let cap = self.geom.payload_size as usize;
        if data.len() > cap {
            return Err(Error::InvalidArgument);
        }
        self.tx_buf()[..data.len()].copy_from_slice(data);
        let t = self.push(data.len())?;
        Ok(self.wait(t)?.bytes())
    }

    /// A blocking convenience: pop + wait + copy the recv'd bytes into `buf`.
    /// Returns the bytes received (0 = EOF). For zero-copy, use `pop` + `wait` +
    /// [`rx_buf`] directly (this copy-out is the ergonomic path).
    pub fn recv(&mut self, buf: &mut [u8]) -> Result<usize> {
        let t = self.pop(buf.len())?;
        let n = self.wait(t)?.bytes();
        let n = core::cmp::min(n, buf.len());
        buf[..n].copy_from_slice(&self.rx_buf()[..n]);
        Ok(n)
    }

    /// The syscall-free readiness observe (NET-THROUGHPUT §6 busy-poll edge): netd
    /// bumps `ready_seq` after writing RX bytes into the ring, so a change since a
    /// prior snapshot means RX data landed -- letting a client skip a parking READ.
    /// Returns the current sequence; the caller compares it to a prior snapshot.
    pub fn rx_ready_seq(&self) -> u32 {
        let ready_base = (self.ring_va + self.geom.ready_off as u64) as *const u8;
        // SAFETY: ready_off is within this Proc's live ring mapping; observe reads
        // only the producer-owned words.
        unsafe { weft::ready_observe(ready_base) }.0
    }

    /// The raw Loom fd (`KObj_Loom`; in-Proc only -- non-transferable).
    #[inline]
    pub fn ring_fd(&self) -> i32 {
        self.ring.raw_fd()
    }

    #[inline]
    fn next_tok(&mut self) -> u64 {
        let t = self.tok_seq;
        self.tok_seq = self.tok_seq.wrapping_add(1);
        t
    }

    /// Drain CQEs until the one whose `user_data == tok` (single op in flight -> the
    /// first CQE after a blocking enter is it; loop defensively for a spurious
    /// wakeup or a stray CQE).
    fn reap_token(&self, tok: u64) -> Result<Cqe> {
        loop {
            match self.ring.reap() {
                Some(c) if c.user_data == tok => return Ok(c),
                Some(_) => continue, // foreign CQE (not expected single-in-flight); drain
                None => {
                    self.ring.enter(0, 1, loom::ENTER_GETEVENTS)?;
                }
            }
        }
    }
}
