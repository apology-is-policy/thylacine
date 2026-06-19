// /net-echo -- the native libthyla_rs TCP echo server + the net-6a-3 boot probe.
//
// Two modes:
//   * `net-echo serve <port> [n]` -- the real echo server (NET-DESIGN 4):
//     bind/announce <port>, accept + echo `n` connections (default 2). This is
//     the artifact net-8 drives with a live peer; it cannot run in-guest at
//     net-6a-3 because netd's live stack is NIC-only (no loopback route), so an
//     in-guest peer can't reach it -- the deterministic >=2-concurrent echo
//     proof is netd's isolated-loopback `echo_e2e` (server.rs), and the live
//     native-API round-trip is net-8's (it owns the in-guest peer mechanism).
//   * default (the joey boot probe) -- DETERMINISTIC + peer-independent:
//       0. net-8b: the live over-/net echo round-trip (bind+connect+accept+data)
//          on netd's resident loopback (net-8a). Peer-independent because this
//          one Proc is BOTH ends -- the blocking accept/recv defer inside netd,
//          so there is no self-deadlock (see `loopback_e2e`).
//       1. the native net parsing round-trips (Ipv4Addr / SocketAddrV4),
//       2. `TcpListener::bind` (the `announce` server-side passive open) +
//          `local_addr` over the live /net mount,
//       3. the Loom async witness: a Loom READ on a /net fid completes via the
//          kernel dev9p async client (the SAME deferred-reply path net-6a-1
//          audited), proving network I/O rides Loom with ZERO new Loom core
//          (NET-DESIGN 12.1).
//
// joey spawns + reaps + asserts exit 0 + "net-8b loopback E2E PASS" +
// "net-echo: PROBE OK", so any failure gates the boot.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::fs::File;
use libthyla_rs::loom::{BufReg, Ring, Sqe};
use libthyla_rs::net::{echo_serve, Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use libthyla_rs::{t_exits, t_putstr};

/// net-8b: the in-guest over-/net TCP echo round-trip on netd's resident loopback
/// interface (net-8a). This is the first LIVE exercise of the full Plan 9
/// dial+accept+data path over the kernel dev9p `/net` mount -- the deferred-accept
/// (net-3a held Rlopen) + the blocking data read (net-6a-1) + the resident lo
/// stack, end to end. (The net-3* echo_e2e proofs drive netd's methods DIRECTLY
/// on an isolated stack; this drives the real syscalls through the mount.)
///
/// Single-threaded: both ends live in this one Proc. That works because every
/// blocking step (accept, recv) DEFERS inside netd (a separate Proc whose serve
/// loop keeps polling both ends of the loopback) -- this Proc blocks in the
/// kernel while netd makes progress, so there is no self-deadlock. Closes #239
/// (the live `listen` open was denied by the A-3 dev9p perm_check because netd
/// served the listen file read-only; it is FILE_RW now).
fn loopback_e2e() -> Result<(), &'static str> {
    let addr = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 7788);
    let listener = TcpListener::bind(addr).map_err(|_| "bind")?;
    let mut client = TcpStream::connect(addr).map_err(|_| "connect")?;
    // accept blocks until netd's poll establishes the inbound call + swaps.
    let (mut server, _peer) = listener.accept().map_err(|_| "accept")?;

    // client -> server, echoed server -> client.
    let msg = b"thylacine-net-8b-loopback";
    client.write_all(msg).map_err(|_| "client write")?;
    let mut rbuf = [0u8; 64];
    let got = server.read(&mut rbuf).map_err(|_| "server read")?;
    if got == 0 {
        return Err("server EOF");
    }
    server.write_all(&rbuf[..got]).map_err(|_| "server echo")?;
    let mut ebuf = [0u8; 64];
    let back = client.read(&mut ebuf).map_err(|_| "client read")?;
    if &ebuf[..back] != &msg[..] {
        return Err("echo mismatch");
    }
    Ok(())
}

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // `serve <port> [n]` -> the real echo server (net-8, with a live peer).
    let args = env::args();
    if args.get_str(1) == Some("serve") {
        let port: u16 = args.get_str(2).and_then(|s| s.parse().ok()).unwrap_or(7777);
        let n: u32 = args.get_str(3).and_then(|s| s.parse().ok()).unwrap_or(2);
        let addr = SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, port);
        return match echo_serve(addr, n) {
            Ok(_served) => {
                t_putstr("net-echo: served connections, exiting\n");
                0
            }
            Err(_) => {
                t_putstr("net-echo: echo_serve FAILED\n");
                1
            }
        };
    }
    probe()
}

/// The deterministic, peer-independent boot probe.
fn probe() -> i64 {
    // net-8b: the live over-/net echo round-trip (bind+connect+accept+data) on
    // the resident loopback. Deterministic + peer-independent (this Proc is both
    // ends). A failure gates the boot.
    match loopback_e2e() {
        Ok(()) => {
            t_putstr("net-echo: net-8b loopback E2E PASS (bind+connect+accept+echo over /net)\n")
        }
        Err(why) => {
            t_putstr(&alloc::format!(
                "net-echo: FAIL -- net-8b loopback E2E ({})\n",
                why
            ));
            unsafe { t_exits(1) }
        }
    };
    t_putstr("net-echo: starting (net-6a-3 native net + Loom async witness)\n");

    // 1. Pure parsing round-trips -- no I/O, fully deterministic.
    let a = match SocketAddrV4::parse("10.0.2.15:7777") {
        Ok(a) => a,
        Err(_) => fail("net-echo: FAIL -- SocketAddrV4::parse\n"),
    };
    if a.ip().octets() != [10, 0, 2, 15] || a.port() != 7777 {
        fail("net-echo: FAIL -- parsed endpoint mismatch\n");
    }
    if Ipv4Addr::parse("256.0.0.1").is_ok() || Ipv4Addr::parse("1.2.3").is_ok() {
        fail("net-echo: FAIL -- malformed addr accepted\n");
    }
    if SocketAddrV4::parse("1.2.3.4").is_ok() || SocketAddrV4::parse("1.2.3.4:99999").is_ok() {
        fail("net-echo: FAIL -- malformed sockaddr accepted\n");
    }

    // 2. The server-side passive open over the live /net mount (no peer needed).
    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, 7779)) {
        Ok(l) => l,
        Err(_) => fail("net-echo: FAIL -- TcpListener::bind\n"),
    };
    let n = listener.conn_n();
    // local_addr reads /net/tcp/N/local through 9P -- the announced endpoint.
    if listener.local_addr().is_err() {
        fail("net-echo: FAIL -- TcpListener::local_addr\n");
    }

    // 3. The Loom async witness: register a /net fid + a buffer, submit a Loom
    //    READ, and require it to COMPLETE via a CQE. This empirically proves a
    //    /net fid rides the kernel dev9p async path (network I/O on Loom, zero
    //    new Loom core -- NET-DESIGN 12.1) using the local-endpoint file, a
    //    non-blocking read (no peer, no hang). A completing CQE -- not the
    //    byte count -- is the witness.
    let local = match File::open(alloc::format!("/net/tcp/{}/local", n)) {
        Ok(f) => f,
        Err(_) => fail("net-echo: FAIL -- open /net/tcp/N/local\n"),
    };
    let ring = match Ring::setup(8, 0) {
        Ok(r) => r,
        Err(_) => fail("net-echo: FAIL -- Ring::setup\n"),
    };
    let mut buf = vec![0u8; 64];
    let breg = BufReg {
        va: buf.as_mut_ptr() as u64,
        len: buf.len() as u64,
    };
    if ring.register_buffers(&[breg]).is_err() {
        fail("net-echo: FAIL -- register_buffers\n");
    }
    if ring.register_handles(&[local.as_raw_fd()]).is_err() {
        fail("net-echo: FAIL -- register_handles(/net fid)\n");
    }
    // READ handle 0 (the /net local fid) -> buffer 0, up to 64 bytes.
    let cqe = match ring.submit_one_wait(&Sqe::read(0, 0, 64, 0, 0, 0x4E45)) {
        Ok(c) => c,
        Err(_) => fail("net-echo: FAIL -- Loom READ on /net fid did not complete\n"),
    };
    if cqe.user_data != 0x4E45 {
        fail("net-echo: FAIL -- Loom CQE user_data mismatch\n");
    }
    if cqe.result < 0 {
        // The dev9p async path reached netd but the read errored -- still proves
        // the path composes, but a /net/local read should succeed; treat a
        // negative result as a probe failure so a real regression is visible.
        fail("net-echo: FAIL -- Loom READ on /net fid returned an error CQE\n");
    }

    t_putstr(
        "net-echo: PROBE OK (native net bind/announce/local + Loom async /net read composes)\n",
    );
    0
}
