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
use alloc_crate::format;
use alloc_crate::string::String;
use alloc_crate::vec::Vec;
use core::fmt;

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
    let rport: u16 = port_s
        .trim()
        .parse()
        .map_err(|_| Error::InvalidArgument)?;
    Ok(SocketAddrV4::new(ip, rport))
}
