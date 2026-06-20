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
//       0b. net-8c-1: the soak / leak-baseline (NET-DESIGN 16) -- repeat the
//          round-trip 8x and require netd's live TCP `active` count to return to
//          its pre-soak baseline (no per-cycle slot leak).
//       0c. net-8c-2: a REAL TLS 1.3 handshake + app-data echo over the live
//          /net loopback (the `tls` crate's TlsStream client <-> TlsServerStream
//          server). Two Threads (server + client) interleave through netd, since
//          a handshake needs the server-side TLS state machine to pump
//          concurrently with the client's (see `tls_over_net_e2e`).
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
use core::sync::atomic::{AtomicU32, Ordering};
use core::time::Duration;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::fs::File;
use libthyla_rs::io::{Read, Write};
use libthyla_rs::loom::{BufReg, Ring, Sqe};
use libthyla_rs::net::{echo_serve, Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use libthyla_rs::thread;
use libthyla_rs::{t_burrow_attach, t_exits, t_putstr, t_weft_map};

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
    if ebuf[..back] != msg[..] {
        return Err("echo mismatch");
    }
    Ok(())
}

/// Weft-6b: the live SYS_WEFT_MAP round-trip over netd's resident loopback. A
/// flow's FIRST zero-copy use (SYS_WEFT_MAP on its data fd) issues Tweft(F) on the
/// shared `/net` dev9p client -> netd's h_weft allocates + registers a per-flow
/// ring + answers Rweft(share_id, geom) -> the kernel claims the share + maps the
/// ring into THIS guest (burrow_share_into). The proof of the grant-is-the-share
/// keystone, live: a real ring VA comes back; the geometry-mirror header netd
/// wrote is visible in the shared mapping (the magic + entry count); and a second
/// map on the same fd is idempotent (the kernel fast-path, no second Tweft).
fn weft_e2e() -> Result<(), &'static str> {
    let addr = SocketAddrV4::new(Ipv4Addr::LOCALHOST, 7790);
    let listener = TcpListener::bind(addr).map_err(|_| "bind")?;
    let client = TcpStream::connect(addr).map_err(|_| "connect")?;
    // accept establishes the inbound half over the loopback (this Proc is both
    // ends; the blocking accept defers inside netd, so no self-deadlock).
    let (_server, _peer) = listener.accept().map_err(|_| "accept")?;

    // SYS_WEFT_MAP on the client stream's data fd (as_raw_fd() IS the data fd).
    let data_fd = client.as_raw_fd();
    let va = unsafe { t_weft_map(data_fd as u64, 0) };
    if va <= 0 {
        return Err("map returned no ring");
    }
    // The geometry mirror netd wrote must be visible in the shared mapping.
    let base = va as u64 as *const u8;
    let magic = unsafe { core::ptr::read_volatile(base as *const u32) };
    if magic != libthyla_rs::weft::WEFT_MAGIC {
        return Err("ring magic mismatch");
    }
    // ring_entries lives at offset 4 in weft_ring_hdr; it must echo netd's choice.
    let entries = unsafe { core::ptr::read_volatile(base.add(4) as *const u32) };
    if entries != 64 {
        return Err("ring entries mismatch");
    }
    // Idempotent: a second map on the SAME fd returns the SAME VA (no 2nd Tweft).
    let va2 = unsafe { t_weft_map(data_fd as u64, 0) };
    if va2 != va {
        return Err("map not idempotent");
    }
    Ok(())
}

/// Read netd's live TCP connection count from `/net/tcp/stats` (the `active`
/// line). The soak's leak detector: every connection a cycle opens must be
/// freed by its last clunk (the section-16 no-leak exit criterion), so the count
/// returns to its pre-soak baseline.
fn read_tcp_active() -> Option<u32> {
    let mut f = File::open("/net/tcp/stats").ok()?;
    let mut buf = [0u8; 64];
    let n = f.read(&mut buf).ok()?;
    let s = core::str::from_utf8(&buf[..n]).ok()?;
    let at = s.find("active ")? + "active ".len();
    let rest = &s[at..];
    let end = rest
        .find(|c: char| !c.is_ascii_digit())
        .unwrap_or(rest.len());
    rest[..end].parse().ok()
}

/// net-8c-1: the soak / leak-baseline (NET-DESIGN section 16). Run the live
/// over-`/net` echo round-trip N times and require netd's live TCP connection
/// count to return to its pre-soak baseline -- a per-cycle leak of even one slot
/// (listener / connector / accepted M each open + free per cycle) would grow it.
fn soak_e2e(cycles: u32) -> Result<(), &'static str> {
    let before = read_tcp_active().ok_or("stats read (before)")?;
    for _ in 0..cycles {
        loopback_e2e()?;
    }
    let after = read_tcp_active().ok_or("stats read (after)")?;
    if after != before {
        return Err("leak: tcp active did not return to baseline");
    }
    Ok(())
}

// =============================================================================
// net-8c-2: a real TLS 1.3 handshake over a live /net TcpStream (SA-2).
// =============================================================================
//
// net-8b's loopback_e2e proves PLAINTEXT bytes flow over the live /net mount;
// this proves the `tls` crate's handshake + record layer compose over that SAME
// live byte transport -- the symmetric TlsStream (client) <-> TlsServerStream
// (server) pair, end to end, over netd's resident loopback.
//
// Unlike net-8b, this CANNOT be single-threaded: a TLS handshake is a multi-
// round-trip application protocol, so the server-side TLS state machine must
// PUMP concurrently with the client's (netd only moves TCP bytes; it does not
// run TLS). A dedicated server Thread runs the server side while the main
// Thread runs the client side; they interleave through netd over the loopback.
// (Both ends' blocking reads are death-interruptible + the kernel preempts, so
// it is sound on a single CPU too: when main blocks reading the ServerHello,
// the server Thread is scheduled, and vice versa.)

const TLS_PORT: u16 = 7799;
const TLS_HOST: &str = "loopback.test";
const LOOPBACK_CERT: &[u8] = include_bytes!("../testdata/loopback-cert.pem");
const LOOPBACK_KEY: &[u8] = include_bytes!("../testdata/loopback-key.pem");
/// The app data the client sends; the server echoes it back verbatim over TLS.
const TLS_MSG: &[u8] = b"thylacine-net-8c-2-tls-over-net";

/// The server Thread's stack. rustls's server handshake is moderately stack-
/// hungry; 1 MiB is generous (a too-small stack faults past the mapped region
/// -> the boot gate catches it loudly, never silent corruption).
const TLS_SRV_STACK: u64 = 1024 * 1024;
/// The non-zero tid sentinel the kernel clears on the server Thread's exit.
const SRV_SENTINEL: u32 = 1;

// The server Thread's outcome, surfaced to main after the join.
const SR_PENDING: u32 = 0;
const SR_OK: u32 = 1;
const SR_CFG: u32 = 2;
const SR_BIND: u32 = 3;
const SR_ACCEPT: u32 = 4;
const SR_TLS: u32 = 5;
const SR_READ: u32 = 6;
const SR_EOF: u32 = 7;
const SR_ECHO: u32 = 8;

/// The server Thread's join tid word, the announce-ready rendezvous, and its
/// result code. (Statics because the raw `spawn_raw` entry takes only a u64 arg
/// -- the server Thread builds its own listener + config and reports here.)
static SRV_TID: AtomicU32 = AtomicU32::new(0);
static SRV_READY: AtomicU32 = AtomicU32::new(0);
static SRV_RESULT: AtomicU32 = AtomicU32::new(SR_PENDING);

/// The server Thread entry: announce on the loopback, accept one connection, run
/// the SERVER side of the TLS handshake, echo the (decrypted) request, close.
/// Records its outcome in SRV_RESULT for main to read after the join. Never
/// prints (only main writes the console -- no concurrent console writes).
extern "C" fn tls_server_entry(_arg: u64) {
    let _ = thread::set_tid_address(&SRV_TID);
    let code = tls_server_run();
    SRV_RESULT.store(code, Ordering::SeqCst);
    thread::exit_self();
}

fn tls_server_run() -> u32 {
    // Build the server config (cert + key) BEFORE announcing, so a config error
    // never strands a client on a port that will not handshake.
    let server_cfg = match tls::server_config_single_cert(LOOPBACK_CERT, LOOPBACK_KEY) {
        Ok(c) => c,
        Err(_) => return SR_CFG,
    };
    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, TLS_PORT)) {
        Ok(l) => l,
        Err(_) => return SR_BIND,
    };
    // Announced now -- release the client to connect.
    SRV_READY.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake_all(&SRV_READY);

    let (stream, _peer) = match listener.accept() {
        Ok(s) => s,
        Err(_) => return SR_ACCEPT,
    };
    let mut tls = match tls::TlsServerStream::accept(stream, server_cfg) {
        Ok(t) => t,
        Err(_) => return SR_TLS,
    };
    // Read the client's (decrypted) request and echo it back (encrypted).
    let mut buf = [0u8; 256];
    let n = match tls.read(&mut buf) {
        Ok(n) => n,
        Err(_) => return SR_READ,
    };
    if n == 0 {
        return SR_EOF;
    }
    if tls.write_all(&buf[..n]).is_err() {
        return SR_ECHO;
    }
    tls.close();
    SR_OK
}

/// net-8c-2: spawn the server Thread, run the client side here, join, and assert
/// both halves of a real TLS handshake + app-data echo over the live /net
/// loopback succeeded. Runs once (not in the soak loop).
fn tls_over_net_e2e() -> Result<(), &'static str> {
    // Reset the rendezvous before spawning.
    SRV_READY.store(0, Ordering::SeqCst);
    SRV_RESULT.store(SR_PENDING, Ordering::SeqCst);
    SRV_TID.store(SRV_SENTINEL, Ordering::SeqCst); // sentinel BEFORE spawn

    // The server Thread's stack (a fresh anon burrow; its top is 16-aligned
    // since the base is page-aligned and the size is a page multiple).
    let stack = unsafe { t_burrow_attach(TLS_SRV_STACK) };
    if stack < 0 {
        return Err("server stack attach");
    }
    let sp = (stack as u64) + TLS_SRV_STACK;
    if unsafe { thread::spawn_raw(tls_server_entry as *const () as u64, sp, 0, 0) }.is_err() {
        return Err("server spawn");
    }

    // Wait for the server to announce (bounded). torpor::wait closes the
    // check-then-wait race: if READY is already 1 the load exits the loop; if
    // the server stores+wakes between our load and the syscall, the kernel
    // returns ValueMismatch without sleeping.
    let mut waited = 0;
    while SRV_READY.load(Ordering::Acquire) == 0 && waited < 8 {
        let _ = libthyla_rs::torpor::wait(&SRV_READY, 0, Some(Duration::from_secs(1)));
        waited += 1;
    }
    if SRV_READY.load(Ordering::Acquire) == 0 {
        return Err("server announce timeout");
    }

    // Run the client side, then join the server Thread (the kernel wakes us when
    // it clears SRV_TID on the server's exit). Join BEFORE inspecting the client
    // result so the server Thread is reaped on the happy path.
    let client_res = tls_client_roundtrip();
    let joined = thread::join_tid(&SRV_TID, SRV_SENTINEL, Some(Duration::from_secs(30)));

    client_res?;
    if joined.is_err() {
        return Err("server join timeout");
    }
    match SRV_RESULT.load(Ordering::SeqCst) {
        SR_OK => Ok(()),
        SR_CFG => Err("server tls config"),
        SR_BIND => Err("server bind"),
        SR_ACCEPT => Err("server accept"),
        SR_TLS => Err("server tls accept"),
        SR_READ => Err("server tls read"),
        SR_EOF => Err("server saw EOF"),
        SR_ECHO => Err("server tls echo"),
        _ => Err("server pending"),
    }
}

/// The client half: TCP-connect over /net, complete the TLS handshake
/// (validating the self-signed server cert against the trust anchor + the wall
/// clock), send the request, read + verify the echo.
fn tls_client_roundtrip() -> Result<(), &'static str> {
    // Trust the self-signed leaf as its own root (the standard isolated-test
    // pattern -- the same trust setup tls-smoke's loopback E2E uses).
    let roots = tls::load_roots_pem(LOOPBACK_CERT).map_err(|_| "client trust store")?;
    let client_cfg = tls::client_config(roots);
    let sock = TcpStream::connect(SocketAddrV4::new(Ipv4Addr::LOCALHOST, TLS_PORT))
        .map_err(|_| "client connect")?;
    let mut tls =
        tls::TlsStream::connect(sock, TLS_HOST, client_cfg).map_err(|_| "client tls handshake")?;
    tls.write_all(TLS_MSG).map_err(|_| "client tls write")?;
    // Read the echo back. TLS records may fragment app data, so loop until we
    // have the whole message (or a clean EOF cuts it short).
    let mut got = [0u8; 256];
    let mut have = 0usize;
    while have < TLS_MSG.len() {
        let n = tls.read(&mut got[have..]).map_err(|_| "client tls read")?;
        if n == 0 {
            break; // clean TLS EOF before the full echo
        }
        have += n;
    }
    tls.close();
    if &got[..have] != TLS_MSG {
        return Err("client tls echo mismatch");
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
    // Weft-6b: the live SYS_WEFT_MAP round-trip (Tweft -> netd h_weft -> share ->
    // burrow_share_into) -- the grant-is-the-share keystone, live. Runs BEFORE the
    // soak so its connection is freed (the ring detached) before the leak baseline.
    match weft_e2e() {
        Ok(()) => t_putstr(
            "net-echo: weft-6b MAP E2E PASS (Tweft -> netd ring register -> mapped ring visible)\n",
        ),
        Err(why) => {
            t_putstr(&alloc::format!("net-echo: FAIL -- weft-6b MAP E2E ({})\n", why));
            unsafe { t_exits(1) }
        }
    };
    // net-8c-1: the soak / leak-baseline (section 16) -- repeat the round-trip and
    // require netd's live TCP connection count to return to baseline (no leak).
    match soak_e2e(8) {
        Ok(()) => t_putstr(
            "net-echo: net-8c-1 soak PASS (8x echo over /net; tcp active returns to baseline)\n",
        ),
        Err(why) => {
            t_putstr(&alloc::format!(
                "net-echo: FAIL -- net-8c-1 soak ({})\n",
                why
            ));
            unsafe { t_exits(1) }
        }
    };
    // net-8c-2: a real TLS 1.3 handshake + app-data echo over the live /net
    // loopback (the client TlsStream <-> the server TlsServerStream, two Threads
    // interleaving through netd). SA-2.
    match tls_over_net_e2e() {
        Ok(()) => t_putstr(
            "net-echo: net-8c-2 TLS-over-/net E2E PASS (handshake + cert verify + echo over the live mount)\n",
        ),
        Err(why) => {
            t_putstr(&alloc::format!("net-echo: FAIL -- net-8c-2 TLS-over-/net ({})\n", why));
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
