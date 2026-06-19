// /bin/sntp -- a native libthyla_rs SNTP (Simple NTP, RFC 4330) client.
//
// net-7a-2 (NET-DESIGN section 10). Queries an NTP server over netd's
// `/net/udp` tree, computes the wall-clock offset against the LS-K monotonic
// clock, and steps `CLOCK_REALTIME` via the net-7a-1 `SYS_CLOCK_SETTIME`
// (CAP_HOSTOWNER-gated). A native tool over `/net` + the clock syscalls: it
// touches no hardware (netd owns the NIC, I-5) and reaches only the `/net` its
// territory grants (I-1/I-23/I-28) -- a buggy client corrupts only its own
// state.
//
// Modes:
//   * `sntp`  (no args) / `sntp --selftest` -- the DETERMINISTIC, peer-
//     independent boot probe (the gate): the NTPv4 build/parse/validate +
//     1900->Unix conversion + offset-arithmetic battery, plus the clock-step
//     GATE PROOF (as spawned unelevated by joey, `set_realtime` is denied with
//     EACCES -- a non-denial would be a privilege regression, so it FAILS the
//     boot). No I/O; runs on every boot.
//   * `sntp <a.b.c.d>` -- the LIVE query (best-effort, NOT a boot gate): dial
//     the server, send a request, bounded-poll for the reply, validate, compute
//     the offset, and step the clock (if elevated) or log the EACCES. Host-
//     dependent under slirp (no in-guest NTP peer) -- the deterministic in-guest
//     round-trip over a loopback responder is OWED to net-8.
//
// The selftest is an IN-GUEST runtime battery, not a `cargo test`: this is a
// no_std + aarch64 bin crate (the net-4d proto_selftest pattern).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::err::Error;
use libthyla_rs::net::{Ipv4Addr, SocketAddrV4, UdpSocket};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::t_putstr;
use libthyla_rs::time::{self, Duration, Instant, SystemTime};

/// Seconds from the NTP epoch (1900-01-01) to the Unix epoch (1970-01-01):
/// 70 years + 17 leap days = 2_208_988_800.
const NTP_UNIX_DELTA: u64 = 2_208_988_800;

/// The standard NTP service port.
const NTP_PORT: u16 = 123;

/// A 48-byte NTPv4 packet (RFC 4330). The only fields this client builds or
/// reads are byte 0 (LI/VN/Mode), byte 1 (Stratum), and the Originate (24..32)
/// + Transmit (40..48) timestamps; the rest stay zero in a request.
const NTP_PACKET_LEN: usize = 48;

/// A 64-bit NTP timestamp: 32-bit seconds since 1900 + 32-bit fraction, both
/// big-endian on the wire.
#[derive(Clone, Copy, PartialEq, Eq)]
struct NtpTimestamp {
    secs: u32,
    frac: u32,
}

impl NtpTimestamp {
    /// Read 8 big-endian bytes. The caller guarantees `b.len() >= 8`.
    fn from_bytes(b: &[u8]) -> NtpTimestamp {
        NtpTimestamp {
            secs: u32::from_be_bytes([b[0], b[1], b[2], b[3]]),
            frac: u32::from_be_bytes([b[4], b[5], b[6], b[7]]),
        }
    }

    /// Write 8 big-endian bytes. The caller guarantees `b.len() >= 8`.
    fn write_into(&self, b: &mut [u8]) {
        b[0..4].copy_from_slice(&self.secs.to_be_bytes());
        b[4..8].copy_from_slice(&self.frac.to_be_bytes());
    }

    fn is_zero(&self) -> bool {
        self.secs == 0 && self.frac == 0
    }

    /// Convert to a Unix-epoch `(secs, nanos)`. NTP era-0 (valid through 2036);
    /// a pre-1970 result (an era-2036+ timestamp whose 32-bit seconds wrapped
    /// below the delta, or a bogus tiny value) saturates to 0 rather than
    /// underflowing -- the documented v1.x era seam.
    fn to_unix(self) -> (u64, u32) {
        let secs = (self.secs as u64).saturating_sub(NTP_UNIX_DELTA);
        // frac is a /2^32 fraction of a second.
        let nanos = ((self.frac as u64 * 1_000_000_000u64) >> 32) as u32;
        (secs, nanos)
    }

    /// As a `SystemTime` (Unix-epoch wall clock).
    fn to_systemtime(self) -> SystemTime {
        let (s, ns) = self.to_unix();
        SystemTime::from_unix(s, ns)
    }
}

/// Extract the Mode field (bits 0..3) from byte 0.
fn ntp_mode(b0: u8) -> u8 {
    b0 & 0x07
}

/// Build a 48-byte NTPv4 client request. Byte 0 = 0x23 (LI=0, VN=4, Mode=3 =
/// client). The Transmit timestamp carries `nonce`; a correct server echoes it
/// into the response's Originate field, so the client can reject an off-path
/// reply that doesn't match (the SNTP anti-spoof correlation).
fn build_request(nonce: NtpTimestamp) -> [u8; NTP_PACKET_LEN] {
    let mut p = [0u8; NTP_PACKET_LEN];
    p[0] = 0x23;
    nonce.write_into(&mut p[40..48]);
    p
}

/// Why a candidate NTP response was rejected.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum NtpReject {
    BadLen,
    NotServer,
    BadStratum,
    OriginateMismatch,
    ZeroTransmit,
}

/// Validate a candidate NTP response against the request `nonce` and return its
/// Transmit timestamp (the server's clock at send). Rejects: a short packet, a
/// non-server Mode, a Kiss-of-Death (stratum 0) / unsynchronized (stratum > 15)
/// server, an Originate that doesn't echo our nonce (spoof), or a zero Transmit.
fn validate_response(p: &[u8], nonce: NtpTimestamp) -> Result<NtpTimestamp, NtpReject> {
    if p.len() < NTP_PACKET_LEN {
        return Err(NtpReject::BadLen);
    }
    if ntp_mode(p[0]) != 4 {
        return Err(NtpReject::NotServer);
    }
    let stratum = p[1];
    if stratum == 0 || stratum > 15 {
        return Err(NtpReject::BadStratum);
    }
    if NtpTimestamp::from_bytes(&p[24..32]) != nonce {
        return Err(NtpReject::OriginateMismatch);
    }
    let transmit = NtpTimestamp::from_bytes(&p[40..48]);
    if transmit.is_zero() {
        return Err(NtpReject::ZeroTransmit);
    }
    Ok(transmit)
}

/// The corrected wall clock the server implies: its Transmit timestamp plus
/// half the measured round-trip (the time for the reply to travel back to us).
/// `rtt` comes from the LS-K MONOTONIC clock, so a concurrent wall-clock step
/// can't skew it.
fn corrected_time(transmit: NtpTimestamp, rtt: Duration) -> SystemTime {
    let d = transmit.to_systemtime().since_epoch() + rtt / 2;
    SystemTime::from_unix(d.as_secs(), d.subsec_nanos())
}

/// Format the signed offset of `target` from `now` as "+N.NNNs" / "-N.NNNs".
fn fmt_offset(target: SystemTime, now: SystemTime) -> alloc::string::String {
    let (sign, d) = match target.duration_since(now) {
        Ok(d) => ('+', d),
        Err(d) => ('-', d),
    };
    format!("{}{}.{:03}s", sign, d.as_secs(), d.subsec_millis())
}

fn fail(msg: &str) -> i64 {
    t_putstr(msg);
    1
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();
    match args.get_str(1) {
        None | Some("--selftest") => selftest(),
        Some(server) => live_query(server),
    }
}

/// The deterministic boot gate: the NTP codec battery + the clock-step gate
/// proof. Exit 0 = all assertions held.
fn selftest() -> i64 {
    // 1. Request shape: byte 0 = 0x23, the nonce embedded in Transmit, the rest
    //    zero.
    let nonce = NtpTimestamp {
        secs: 0x1122_3344,
        frac: 0x5566_7788,
    };
    let req = build_request(nonce);
    if req[0] != 0x23 {
        return fail("sntp: FAIL -- request byte 0 != 0x23\n");
    }
    if NtpTimestamp::from_bytes(&req[40..48]) != nonce {
        return fail("sntp: FAIL -- request nonce not embedded\n");
    }
    if req[1..40].iter().any(|&b| b != 0) {
        return fail("sntp: FAIL -- request body not zero\n");
    }

    // 2. 1900->Unix conversion, a known vector: Unix 1_700_000_000 (2023-11-14)
    //    + a half-second fraction (0x8000_0000 = 2^31 = .5) -> (1_700_000_000,
    //    500_000_000).
    let known_unix: u64 = 1_700_000_000;
    let ts = NtpTimestamp {
        secs: (known_unix + NTP_UNIX_DELTA) as u32,
        frac: 0x8000_0000,
    };
    let (u, ns) = ts.to_unix();
    if u != known_unix || ns != 500_000_000 {
        return fail("sntp: FAIL -- 1900->Unix conversion wrong\n");
    }
    // saturating floor: a sub-delta timestamp clamps to 0 (no underflow panic).
    let sub_delta = NtpTimestamp { secs: 1, frac: 0 };
    if sub_delta.to_unix() != (0, 0) {
        return fail("sntp: FAIL -- sub-delta timestamp did not saturate\n");
    }

    // 3. Response validation: a well-formed server reply parses; each malformed
    //    variant is rejected with the right reason.
    let mut ok = [0u8; NTP_PACKET_LEN];
    ok[0] = 0x24; // VN=4, Mode=4 (server)
    ok[1] = 2; // stratum 2
    nonce.write_into(&mut ok[24..32]); // Originate echoes our nonce
    ts.write_into(&mut ok[40..48]); // Transmit
    match validate_response(&ok, nonce) {
        Ok(t) if t == ts => {}
        _ => return fail("sntp: FAIL -- valid response rejected\n"),
    }
    // mode-3 (not a server) -> NotServer.
    let mut bad = ok;
    bad[0] = 0x23;
    if validate_response(&bad, nonce) != Err(NtpReject::NotServer) {
        return fail("sntp: FAIL -- mode-3 not rejected\n");
    }
    // stratum 0 (Kiss-of-Death) -> BadStratum.
    let mut bad = ok;
    bad[1] = 0;
    if validate_response(&bad, nonce) != Err(NtpReject::BadStratum) {
        return fail("sntp: FAIL -- stratum-0 (KoD) not rejected\n");
    }
    // stratum 16 (unsynchronized) -> BadStratum.
    let mut bad = ok;
    bad[1] = 16;
    if validate_response(&bad, nonce) != Err(NtpReject::BadStratum) {
        return fail("sntp: FAIL -- stratum-16 not rejected\n");
    }
    // a spoofed Originate (doesn't echo our nonce) -> OriginateMismatch.
    let mut bad = ok;
    bad[24] ^= 0xFF;
    if validate_response(&bad, nonce) != Err(NtpReject::OriginateMismatch) {
        return fail("sntp: FAIL -- spoofed originate not rejected\n");
    }
    // a zero Transmit timestamp -> ZeroTransmit.
    let mut bad = ok;
    bad[40..48].fill(0);
    if validate_response(&bad, nonce) != Err(NtpReject::ZeroTransmit) {
        return fail("sntp: FAIL -- zero-transmit not rejected\n");
    }
    // a short packet -> BadLen.
    if validate_response(&ok[..40], nonce) != Err(NtpReject::BadLen) {
        return fail("sntp: FAIL -- short packet not rejected\n");
    }

    // 4. Offset arithmetic: transmit -> Unix 1_700_000_000.0, rtt 100ms ->
    //    corrected 1_700_000_000.050; offset vs a local 10s-slow clock = +10.050s.
    let transmit = NtpTimestamp {
        secs: (1_700_000_000u64 + NTP_UNIX_DELTA) as u32,
        frac: 0,
    };
    let corrected = corrected_time(transmit, Duration::from_millis(100));
    if corrected.since_epoch() != Duration::new(1_700_000_000, 50_000_000) {
        return fail("sntp: FAIL -- corrected time (transmit + rtt/2) wrong\n");
    }
    let local = SystemTime::from_unix(1_699_999_990, 0);
    match corrected.duration_since(local) {
        Ok(d) if d == Duration::new(10, 50_000_000) => {}
        _ => return fail("sntp: FAIL -- offset arithmetic wrong\n"),
    }
    if fmt_offset(corrected, local) != "+10.050s" {
        return fail("sntp: FAIL -- offset formatting wrong\n");
    }

    // 5. The clock-step GATE PROOF. As spawned by joey (unelevated), stepping
    //    CLOCK_REALTIME is denied (CAP_HOSTOWNER required). A non-denial here is
    //    a privilege regression, so it gates the boot. (A human running this
    //    elevated would see this line "fail" -- the selftest is the unelevated
    //    boot/CI probe; the live `sntp <server>` mode is the elevated path.)
    match time::set_realtime(SystemTime::from_unix(known_unix, 0)) {
        Err(Error::PermissionDenied) => {}
        Ok(()) => {
            return fail("sntp: FAIL -- set_realtime PERMITTED unelevated (gate regression)\n")
        }
        Err(_) => return fail("sntp: FAIL -- set_realtime returned a non-EACCES error\n"),
    }

    t_putstr(
        "sntp: SELFTEST OK (NTPv4 build/parse/validate + 1900->Unix + offset + EACCES gate)\n",
    );
    0
}

/// The live query: dial `server`, send a request, bounded-poll for the reply,
/// validate, compute the offset, and step (if elevated). Best-effort -- a
/// timeout or a bad reply exits non-zero with a logged reason; it is NOT a boot
/// gate (joey runs the selftest).
fn live_query(server: &str) -> i64 {
    let ip = match Ipv4Addr::parse(server) {
        Ok(ip) => ip,
        Err(_) => {
            return fail("sntp: server must be a dotted-quad IPv4 (v1.0: no DNS in sntp)\n");
        }
    };
    let peer = SocketAddrV4::new(ip, NTP_PORT);

    let mut sock = match UdpSocket::connect(peer) {
        Ok(s) => s,
        Err(_) => return fail("sntp: /net/udp connect failed (netd down?)\n"),
    };
    // Open the QTPOLL readiness sibling for a bounded receive (net-6b).
    let ready = match sock.ready_fd() {
        Ok(f) => f,
        Err(_) => return fail("sntp: open /net/udp/N/ready failed\n"),
    };

    // A non-zero, per-request nonce (the server echoes it -- anti-spoof).
    let now_d = SystemTime::now().since_epoch();
    let mut nonce = NtpTimestamp {
        secs: now_d.as_secs() as u32,
        frac: now_d.subsec_nanos(),
    };
    if nonce.is_zero() {
        nonce.frac = 1;
    }

    let t_send = Instant::now();
    if sock.send(&build_request(nonce)).is_err() {
        return fail("sntp: send failed\n");
    }

    // Bounded wait: poll the readiness file for POLLIN (a queued datagram), then
    // recv only if it fired -- so a server that never answers can't hang us.
    let mut ps = PollSet::new();
    ps.add_raw(ready.as_raw_fd(), PollEvents::READ);
    let ready_in = match ps.poll(PollTimeout::Millis(3000)) {
        Ok(results) => results.into_iter().any(|e| e.is_readable()),
        Err(_) => false,
    };
    if !ready_in {
        return fail(&format!(
            "sntp: no response from {} within 3s (best-effort; host-dependent under slirp)\n",
            peer
        ));
    }
    let t_recv = Instant::now();

    let mut buf = [0u8; NTP_PACKET_LEN];
    let k = match sock.recv(&mut buf) {
        Ok(k) => k,
        Err(_) => return fail("sntp: recv failed\n"),
    };

    let transmit = match validate_response(&buf[..k], nonce) {
        Ok(t) => t,
        Err(e) => return fail(&format!("sntp: invalid NTP response ({:?})\n", e)),
    };

    let rtt = t_recv.duration_since(t_send);
    let corrected = corrected_time(transmit, rtt);
    let offset = fmt_offset(corrected, SystemTime::now());
    let (cs, _) = transmit.to_unix();
    t_putstr(&format!(
        "sntp: {} -> server time {} Unix, rtt {}ms, offset {}\n",
        peer,
        cs,
        rtt.as_millis(),
        offset
    ));

    match time::set_realtime(corrected) {
        Ok(()) => {
            t_putstr("sntp: CLOCK_REALTIME stepped\n");
            0
        }
        Err(Error::PermissionDenied) => {
            t_putstr("sntp: not stepped -- CAP_HOSTOWNER required (run elevated to step)\n");
            0
        }
        Err(_) => fail("sntp: clock step failed\n"),
    }
}
