//! Shared `/net` plumbing for the native network tools (`nc`, `con`,
//! `tcpproxy`): dial-string resolution through `/net/cs` and the non-blocking
//! byte pumps. Backend-gated -- needs the libthyla-rs syscall surface, like
//! `meta`/`usage`.
//!
//! The load-bearing subtlety the pumps encode: netd's data write is NON-BLOCKING
//! -- a full send window returns a 0-count write, not a deferred reply
//! (`net.rs`). So a sender waits on POLLOUT (via the connection's `/net/<p>/N/
//! ready` sibling, net-6b) before retrying, and pumps half-duplex-buffered (one
//! in-flight chunk per direction). A naive `write_all` would spuriously fail the
//! instant the first window fills -- exactly the bulk-transfer case these tools
//! exist to drive.

use libthyla_rs::eprintln;
use libthyla_rs::err::{Error, Result};
use libthyla_rs::fs::OpenOptions;
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::net::{Ipv4Addr, SocketAddrV4, TcpStream, UdpSocket};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};

// Staging size. Larger than netd's 4 KiB TCP window (so a read rarely fragments
// a chunk) but small enough to keep stack frames modest.
const BUF: usize = 8 * 1024;
// fd 0 is stdin (the literal avoids importing the AsFd trait for a constant).
const STDIN_FD: i32 = 0;

/// Resolve a Plan 9 dial string (`proto!host!service`) through netd's connection
/// server `/net/cs` -- the one query that resolves BOTH the host (numeric ->
/// ndb-static -> DNS) and a named service (`http` -> 80). The reply line is
/// `<clone-file> <ip>!<port>`; the second field carries the resolved `ip!port`.
/// `NotFound` on an empty/unresolvable reply, `InvalidArgument` on a malformed
/// one.
pub fn cs_resolve(dialstr: &str) -> Result<SocketAddrV4> {
    let mut cs = OpenOptions::new().read(true).write(true).open("/net/cs")?;
    cs.write_all(dialstr.as_bytes())?;
    let mut buf = [0u8; 128];
    let k = cs.read(&mut buf)?;
    if k == 0 {
        return Err(Error::NotFound);
    }
    let line = core::str::from_utf8(&buf[..k]).map_err(|_| Error::InvalidArgument)?;
    let dial = line.split_whitespace().nth(1).ok_or(Error::NotFound)?;
    let (ip_s, port_s) = dial.rsplit_once('!').ok_or(Error::InvalidArgument)?;
    let ip = Ipv4Addr::parse(ip_s)?;
    let port: u16 = port_s.trim().parse().map_err(|_| Error::InvalidArgument)?;
    Ok(SocketAddrV4::new(ip, port))
}

/// Pump bytes between stdin/stdout and a TCP connection (the `nc`/`con` core).
/// Polls fd 0 and the connection's readiness sibling: stdin -> socket and socket
/// -> stdout, half-duplex-buffered, waiting on POLLOUT under window backpressure.
/// On stdin EOF the send side half-closes (a FIN) so a request/response peer sees
/// the request terminate; the socket then drains until the peer FINs. `prog`
/// prefixes diagnostics. Returns 0 when the peer FINs (the normal close), 1 on a
/// poll/write error.
pub fn stdio_pump(stream: &mut TcpStream, prog: &str) -> i64 {
    let ready = match stream.ready_fd() {
        Ok(r) => r,
        Err(_) => {
            eprintln!("{}: cannot open the readiness fd", prog);
            return 1;
        }
    };
    let ready_fd = ready.as_raw_fd();

    let mut rbuf = [0u8; BUF]; // socket -> stdout staging
    let mut obuf = [0u8; BUF]; // stdin  -> socket staging
    let mut o_len = 0usize;
    let mut o_off = 0usize;
    let mut stdin_open = true;
    let mut want_fin = false;

    loop {
        let pending = o_off < o_len;
        if want_fin && !pending {
            let _ = stream.shutdown();
            want_fin = false;
        }

        let mut ps = PollSet::new();
        if stdin_open && !pending {
            ps.add_raw(STDIN_FD, PollEvents::READ);
        }
        let sock_ev = if pending {
            PollEvents::READ | PollEvents::WRITE
        } else {
            PollEvents::READ
        };
        ps.add_raw(ready_fd, sock_ev);

        let results = match ps.poll(PollTimeout::Block) {
            Ok(r) => r,
            Err(_) => {
                eprintln!("{}: poll failed", prog);
                return 1;
            }
        };
        let mut sock_r = false;
        let mut sock_w = false;
        let mut in_r = false;
        for e in results {
            if e.fd == ready_fd {
                if e.is_readable() || e.is_hup() || e.is_err() {
                    sock_r = true;
                }
                if e.is_writable() {
                    sock_w = true;
                }
            } else if e.fd == STDIN_FD && (e.is_readable() || e.is_hup()) {
                in_r = true;
            }
        }

        // Inbound: socket -> stdout. A 0-read is the peer's FIN: done.
        if sock_r {
            match stream.read(&mut rbuf) {
                Ok(0) => return 0,
                Ok(n) => {
                    if io::stdout().write_all(&rbuf[..n]).is_err() {
                        eprintln!("{}: stdout write failed", prog);
                        return 1;
                    }
                }
                Err(_) => return 0,
            }
        }

        // Outbound: obuf -> socket. A 0-count means the send window is full.
        if pending && sock_w {
            match stream.write(&obuf[o_off..o_len]) {
                Ok(0) => {}
                Ok(n) => {
                    o_off += n;
                    if o_off >= o_len {
                        o_len = 0;
                        o_off = 0;
                    }
                }
                Err(_) => {
                    eprintln!("{}: connection write failed", prog);
                    return 1;
                }
            }
        }

        // Refill from stdin only once the prior chunk is fully sent.
        if stdin_open && !pending && in_r {
            match io::stdin().read(&mut obuf) {
                Ok(0) => {
                    stdin_open = false;
                    want_fin = true;
                }
                Ok(n) => {
                    o_len = n;
                    o_off = 0;
                }
                Err(_) => {
                    stdin_open = false;
                    want_fin = true;
                }
            }
        }
    }
}

/// Write the whole of `buf` to a TCP connection, waiting on POLLOUT under window
/// backpressure (netd's data write is non-blocking -- a full window returns a
/// 0-count write, not a deferred reply). `ready_fd` is the connection's readiness
/// sibling (`stream.ready_fd()`), opened once by the caller and reused across
/// chunks. Returns `true` on success, `false` on a write/poll error. The workhorse
/// for a streaming server (`httpd`) sending a response larger than one window.
pub fn send_all(stream: &mut TcpStream, mut buf: &[u8], ready_fd: i32) -> bool {
    while !buf.is_empty() {
        match stream.write(buf) {
            Ok(0) => {
                // Window full: block until the peer's ACKs reopen it.
                let mut ps = PollSet::new();
                ps.add_raw(ready_fd, PollEvents::WRITE);
                if ps.poll(PollTimeout::Block).is_err() {
                    return false;
                }
            }
            Ok(n) => buf = &buf[n..],
            Err(_) => return false,
        }
    }
    true
}

/// One direction of a socket-to-socket splice: a fixed staging buffer with a
/// fully-sent / pending cursor plus the half-close bookkeeping.
struct Leg {
    buf: [u8; BUF],
    len: usize,
    off: usize,
    src_eof: bool, // the source's read side closed
    finned: bool,  // the destination's send side already half-closed
}

impl Leg {
    fn new() -> Leg {
        Leg {
            buf: [0u8; BUF],
            len: 0,
            off: 0,
            src_eof: false,
            finned: false,
        }
    }
    fn pending(&self) -> bool {
        self.off < self.len
    }
    fn done(&self) -> bool {
        self.src_eof && !self.pending()
    }
}

/// Splice two TCP connections bidirectionally until both directions close (the
/// `tcpproxy` core). Each direction is an independent half-duplex `Leg` (read its
/// source, write its destination, waiting on POLLOUT under backpressure); when a
/// source FINs, its destination's send side is half-closed so the FIN
/// propagates. Returns 0 on a clean close of both directions, 1 on a poll/write
/// error.
pub fn splice(a: &mut TcpStream, b: &mut TcpStream, prog: &str) -> i64 {
    let a_ready = match a.ready_fd() {
        Ok(r) => r,
        Err(_) => {
            eprintln!("{}: cannot open the readiness fd", prog);
            return 1;
        }
    };
    let b_ready = match b.ready_fd() {
        Ok(r) => r,
        Err(_) => {
            eprintln!("{}: cannot open the readiness fd", prog);
            return 1;
        }
    };
    let af = a_ready.as_raw_fd();
    let bf = b_ready.as_raw_fd();

    let mut a2b = Leg::new(); // read a, write b
    let mut b2a = Leg::new(); // read b, write a

    loop {
        if a2b.done() && b2a.done() {
            return 0;
        }
        // Propagate each FIN once its carrying leg has drained.
        if a2b.src_eof && !a2b.pending() && !a2b.finned {
            let _ = b.shutdown();
            a2b.finned = true;
        }
        if b2a.src_eof && !b2a.pending() && !b2a.finned {
            let _ = a.shutdown();
            b2a.finned = true;
        }

        // a's readiness: POLLIN to read for a2b (when free), POLLOUT to write for
        // b2a (when it has data for a). b's is the mirror.
        let mut a_ev = PollEvents::NONE;
        if !a2b.src_eof && !a2b.pending() {
            a_ev |= PollEvents::READ;
        }
        if b2a.pending() {
            a_ev |= PollEvents::WRITE;
        }
        let mut b_ev = PollEvents::NONE;
        if !b2a.src_eof && !b2a.pending() {
            b_ev |= PollEvents::READ;
        }
        if a2b.pending() {
            b_ev |= PollEvents::WRITE;
        }

        let mut ps = PollSet::new();
        if !a_ev.is_empty() {
            ps.add_raw(af, a_ev);
        }
        if !b_ev.is_empty() {
            ps.add_raw(bf, b_ev);
        }
        if ps.is_empty() {
            // Nothing left to wait on but not "done": both FINned + drained.
            return 0;
        }

        let results = match ps.poll(PollTimeout::Block) {
            Ok(r) => r,
            Err(_) => {
                eprintln!("{}: poll failed", prog);
                return 1;
            }
        };
        let (mut a_r, mut a_w, mut b_r, mut b_w) = (false, false, false, false);
        for e in results {
            if e.fd == af {
                if e.is_readable() || e.is_hup() || e.is_err() {
                    a_r = true;
                }
                if e.is_writable() {
                    a_w = true;
                }
            } else if e.fd == bf {
                if e.is_readable() || e.is_hup() || e.is_err() {
                    b_r = true;
                }
                if e.is_writable() {
                    b_w = true;
                }
            }
        }

        if pump_leg(&mut a2b, a, b, a_r, b_w, prog).is_err() {
            return 1;
        }
        if pump_leg(&mut b2a, b, a, b_r, a_w, prog).is_err() {
            return 1;
        }
    }
}

/// Service one splice leg: flush pending bytes to `dst` if `dst_w`, then refill
/// from `src` if `src_r`. A `src` read of 0 sets `src_eof`. `Err(())` on a fatal
/// write error.
fn pump_leg(
    leg: &mut Leg,
    src: &mut TcpStream,
    dst: &mut TcpStream,
    src_r: bool,
    dst_w: bool,
    prog: &str,
) -> core::result::Result<(), ()> {
    if leg.pending() && dst_w {
        match dst.write(&leg.buf[leg.off..leg.len]) {
            Ok(0) => {}
            Ok(n) => {
                leg.off += n;
                if leg.off >= leg.len {
                    leg.len = 0;
                    leg.off = 0;
                }
            }
            Err(_) => {
                eprintln!("{}: connection write failed", prog);
                return Err(());
            }
        }
    }
    if !leg.src_eof && !leg.pending() && src_r {
        match src.read(&mut leg.buf) {
            Ok(0) => leg.src_eof = true,
            Ok(n) => {
                leg.len = n;
                leg.off = 0;
            }
            Err(_) => leg.src_eof = true,
        }
    }
    Ok(())
}

// After stdin EOF, drain replies for this long before exiting -- UDP has no FIN,
// so a request/response peer's reply still arrives, while a pure source/sink run
// finds nothing and exits promptly.
const UDP_DRAIN_MS: u32 = 2000;

/// Pump datagrams between stdin/stdout and a connected UDP socket (the `nc -u`
/// core). Each stdin read becomes one datagram to the connected peer; each
/// received datagram is written to stdout. UDP has no connection close, so on
/// stdin EOF this drains replies for `UDP_DRAIN_MS` (extended by each arrival)
/// then exits. Sends are best-effort (UDP semantics -- a full netd buffer drops
/// the datagram rather than blocking). Returns 0 on a clean finish, 1 on a
/// poll/stdout error.
pub fn udp_pump(sock: &mut UdpSocket, prog: &str) -> i64 {
    let ready = match sock.ready_fd() {
        Ok(r) => r,
        Err(_) => {
            eprintln!("{}: cannot open the readiness fd", prog);
            return 1;
        }
    };
    let rfd = ready.as_raw_fd();

    let mut rbuf = [0u8; BUF];
    let mut obuf = [0u8; BUF];
    let mut stdin_open = true;

    loop {
        let mut ps = PollSet::new();
        if stdin_open {
            ps.add_raw(STDIN_FD, PollEvents::READ);
        }
        ps.add_raw(rfd, PollEvents::READ);
        let timeout = if stdin_open {
            PollTimeout::Block
        } else {
            PollTimeout::Millis(UDP_DRAIN_MS)
        };

        let results = match ps.poll(timeout) {
            Ok(r) => r,
            Err(_) => {
                eprintln!("{}: poll failed", prog);
                return 1;
            }
        };
        let mut sock_r = false;
        let mut in_r = false;
        let mut any = false;
        for e in results {
            any = true;
            if e.fd == rfd {
                if e.is_readable() || e.is_hup() || e.is_err() {
                    sock_r = true;
                }
            } else if e.fd == STDIN_FD && (e.is_readable() || e.is_hup()) {
                in_r = true;
            }
        }

        // Inbound: one datagram -> stdout.
        if sock_r {
            match sock.recv(&mut rbuf) {
                Ok(0) => {}
                Ok(n) => {
                    if io::stdout().write_all(&rbuf[..n]).is_err() {
                        eprintln!("{}: stdout write failed", prog);
                        return 1;
                    }
                }
                Err(_) => {}
            }
        }

        // Outbound: one stdin chunk -> one datagram (best-effort).
        if stdin_open && in_r {
            match io::stdin().read(&mut obuf) {
                Ok(0) => stdin_open = false, // EOF: enter the drain window
                Ok(n) => {
                    let _ = sock.send(&obuf[..n]);
                }
                Err(_) => stdin_open = false,
            }
        } else if !stdin_open && !any {
            // The drain window elapsed with no datagram: the transfer is done.
            return 0;
        }
    }
}
