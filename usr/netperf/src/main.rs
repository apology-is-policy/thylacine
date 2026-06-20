// /netperf -- the NET-PERF NP-1 in-guest loopback micro-bench.
//
// A pure-measurement tool (docs/NET-PERF.md): it times three primitives over
// netd's RESIDENT loopback interface (net-8a -- a 2nd isolated 127.0.0.1/8
// smoltcp stack inside netd, NO NIC / slirp / QEMU device / real network), so
// the numbers isolate OUR stack: the 9P + netd + smoltcp per-op cost. Combined
// with the TCG-vs-HVF contrast (a fixed-time cost is equal on both; a CPU-bound
// cost is ~10-50x worse on TCG -- the free profiler, NET-PERF section 3), this
// splits CPU from IO with no in-guest instrumentation.
//
//   M1 -- per-9P-op RTT: a 1-byte ping-pong over an established loopback conn.
//         Isolates the 9P + netd + smoltcp per-op latency. Single-threaded
//         (this Proc is both ends; netd, a separate Proc, drives the loopback,
//         so each one-directional step makes progress while the other end
//         blocks -- the net-8b loopback_e2e pattern, in a timed loop).
//   M2 -- loopback bulk throughput: stream a fixed size and measure KiB/s.
//         Isolates the 4 KiB TCP window (server.rs) + the 4 KiB DATA_CHUNK + the
//         9P-RPC-per-buffer cost + the per-window POLLOUT-readiness cadence (the
//         write path is non-blocking, so a bulk sender polls POLLOUT per window).
//         Needs TWO Threads (a 4 KiB window deadlocks a single-threaded bulk
//         transfer): a drain server Thread + the main writer Thread interleave
//         through netd (the net-8c-2 two-thread pattern, here without TLS).
//   M3 -- TCP connect latency: time the active open to ESTABLISHED (the
//         PendingConnect deferred-reply path, #257) over loopback. On loopback
//         netd self-wakes on the ctl-write, so this measures our connect
//         processing -- NOT the 50 ms NIC RX-wake floor (NET-PERF section 2.1).
//
// All timing uses the LS-K CLOCK_MONOTONIC (libthyla_rs::time::Instant).
//
// Modes:
//   * default (no args) -- the joey boot probe: a SHORT fixed run (M1_ITERS /
//     M2_BOOT_BYTES / M3_CONNS), logged. The TCG boot log + an HVF boot log give
//     the TCG-vs-HVF contrast for free, deterministically.
//   * `netperf [iters] [mib] [conns]` -- bigger parameters, from the shell.
//
// joey spawns + reaps + asserts exit 0, so any failure (or a hang) gates the
// boot. Pure userspace; a buggy client corrupts only its own state (the kernel
// + netd validate every op); composes I-1/I-5/I-23/I-28, adds no invariant.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use core::time::Duration;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::net::{Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::thread;
use libthyla_rs::time::Instant;
use libthyla_rs::{t_burrow_attach, t_exits, t_putstr};

// Distinct ports per metric (netperf runs after net-echo has exited, so there
// is no live conflict, but distinct ports sidestep any TIME_WAIT lingering).
const M1_PORT: u16 = 7811;
const M2_PORT: u16 = 7812;
const M3_PORT: u16 = 7813;

// Boot-probe defaults: representative but bounded so the per-boot cost stays
// small (the real profiling runs pass bigger params from the shell). M2's size
// lives with the M2 constants (M2_BOOT_BYTES) since it is byte- not MiB-sized.
const M1_ITERS: u32 = 100;
const M3_CONNS: u32 = 50;

/// Format nanoseconds as "N.NNN us" (microseconds, 3 decimals). No float
/// formatting in no_std; integer split: us = ns/1000, frac = ns%1000.
fn us(ns: u64) -> String {
    format!("{}.{:03} us", ns / 1000, ns % 1000)
}

// =============================================================================
// M1 -- per-op RTT (single-threaded 1-byte ping-pong).
// =============================================================================

/// Establish one loopback connection (this Proc is both ends), then ping-pong a
/// 1-byte message `iters` times, timing each full round-trip with the monotonic
/// clock. Each round-trip is: client write -> server read -> server echo ->
/// client read = 4 ops through netd (2 each way). Reports mean/min/max.
fn m1_rtt(iters: u32) -> Result<(), &'static str> {
    let addr = SocketAddrV4::new(Ipv4Addr::LOCALHOST, M1_PORT);
    let listener = TcpListener::bind(addr).map_err(|_| "bind")?;
    let mut client = TcpStream::connect(addr).map_err(|_| "connect")?;
    // accept retrieves the connection netd established for our connect (the
    // backlog holds it); both ends are now ESTABLISHED.
    let (mut server, _peer) = listener.accept().map_err(|_| "accept")?;

    let ping = [0x5au8; 1];
    let mut buf = [0u8; 1];
    let mut total_ns: u64 = 0;
    let mut min_ns: u64 = u64::MAX;
    let mut max_ns: u64 = 0;
    for _ in 0..iters {
        let t = Instant::now();
        client.write_all(&ping).map_err(|_| "client write")?;
        let n = server.read(&mut buf).map_err(|_| "server read")?;
        if n == 0 {
            return Err("server EOF");
        }
        server.write_all(&buf[..n]).map_err(|_| "server echo")?;
        let m = client.read(&mut buf).map_err(|_| "client read")?;
        if m == 0 {
            return Err("client EOF");
        }
        let dt = t.elapsed().as_nanos() as u64;
        total_ns += dt;
        if dt < min_ns {
            min_ns = dt;
        }
        if dt > max_ns {
            max_ns = dt;
        }
    }
    let mean = total_ns / iters as u64;
    t_putstr(&format!(
        "netperf M1 rtt: {} round-trips over lo; mean {} / min {} / max {} (1B ping-pong, 4 ops/rt)\n",
        iters,
        us(mean),
        us(min_ns),
        us(max_ns)
    ));
    Ok(())
}

// =============================================================================
// M2 -- loopback bulk throughput (two-Thread: a drain server + the main writer).
// =============================================================================

const M2_STACK: u64 = 256 * 1024; // a plain drain server (no rustls): small.
const M2_SENTINEL: u32 = 1;
const M2_BUF: usize = 16 * 1024; // server read + client write scratch.
const M2_POLL_MS: u32 = 2000; // POLLOUT wait per stalled write (safety bound).
const M2_MAX_STALLS: u32 = 15; // consecutive POLLOUT timeouts before bailing.

// The boot-probe transfer. DELIBERATELY small: M2's throughput is bounded by the
// per-4-KiB-window POLLOUT-readiness cadence (netd's data write is non-blocking,
// so a bulk sender polls POLLOUT per window, and that readiness delivery is
// timer-paced -- the dev9p.poll idle-pump, #221/#220), so the rate is
// ~size-independent. 8 windows (32 KiB) measures it stably in ~1s; the shell
// (`netperf [iters] [mib] [conns]`) passes bigger MiB for a long baseline.
const M2_BOOT_BYTES: u64 = 32 * 1024;

// The server Thread's outcome codes.
const M2_PENDING: u32 = 0;
const M2_OK: u32 = 1;
const M2_BIND: u32 = 2;
const M2_ACCEPT: u32 = 3;
const M2_READ: u32 = 4;
const M2_SHORT: u32 = 5; // drained fewer bytes than sent (lost data).

static M2_SRV_TID: AtomicU32 = AtomicU32::new(0);
static M2_READY: AtomicU32 = AtomicU32::new(0);
static M2_RESULT: AtomicU32 = AtomicU32::new(M2_PENDING);
static M2_GOT: AtomicU64 = AtomicU64::new(0); // bytes the server drained.
static M2_EXPECT: AtomicU64 = AtomicU64::new(0); // bytes main will send.

/// The drain server Thread: bind, announce (release the client), accept, then
/// read until EOF counting bytes. Records its outcome for main to read after
/// the join. Never prints (only main writes the console).
extern "C" fn m2_server_entry(_arg: u64) {
    let _ = thread::set_tid_address(&M2_SRV_TID);
    let code = m2_server_run();
    M2_RESULT.store(code, Ordering::SeqCst);
    thread::exit_self();
}

fn m2_server_run() -> u32 {
    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, M2_PORT)) {
        Ok(l) => l,
        Err(_) => return M2_BIND,
    };
    M2_READY.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake_all(&M2_READY);

    let (mut server, _peer) = match listener.accept() {
        Ok(s) => s,
        Err(_) => return M2_ACCEPT,
    };
    let expect = M2_EXPECT.load(Ordering::Acquire);
    let mut buf = vec![0u8; M2_BUF];
    let mut got: u64 = 0;
    loop {
        let n = match server.read(&mut buf) {
            Ok(n) => n,
            Err(_) => return M2_READ,
        };
        if n == 0 {
            break; // EOF: the client half-closed the send side (FIN).
        }
        got += n as u64;
    }
    M2_GOT.store(got, Ordering::SeqCst);
    if got != expect {
        return M2_SHORT;
    }
    M2_OK
}

/// Spawn the drain server, connect, stream `total` bytes while the server
/// drains concurrently (the 4 KiB window paces the writer -- a full window
/// returns a 0-count Rwrite, so the writer waits for POLLOUT, the server drains,
/// the window reopens), then half-close and join. Times from the first byte
/// until the server has drained the whole stream + exited (the join), so the
/// measure captures the full transfer (the connect is untimed -- M3 owns connect
/// latency).
fn m2_throughput(total: u64) -> Result<(), &'static str> {
    // Reset the rendezvous + result statics before spawning.
    M2_READY.store(0, Ordering::SeqCst);
    M2_RESULT.store(M2_PENDING, Ordering::SeqCst);
    M2_GOT.store(0, Ordering::SeqCst);
    M2_EXPECT.store(total, Ordering::SeqCst);
    M2_SRV_TID.store(M2_SENTINEL, Ordering::SeqCst); // sentinel BEFORE spawn.

    let stack = unsafe { t_burrow_attach(M2_STACK) };
    if stack < 0 {
        return Err("server stack attach");
    }
    let sp = (stack as u64) + M2_STACK;
    if unsafe { thread::spawn_raw(m2_server_entry as *const () as u64, sp, 0, 0) }.is_err() {
        return Err("server spawn");
    }

    // Wait for the server to announce (bounded; torpor::wait closes the
    // check-then-wait race exactly as net-echo's TLS E2E does).
    let mut waited = 0;
    while M2_READY.load(Ordering::Acquire) == 0 && waited < 8 {
        let _ = libthyla_rs::torpor::wait(&M2_READY, 0, Some(Duration::from_secs(1)));
        waited += 1;
    }
    if M2_READY.load(Ordering::Acquire) == 0 {
        return Err("server announce timeout");
    }

    let mut client = TcpStream::connect(SocketAddrV4::new(Ipv4Addr::LOCALHOST, M2_PORT))
        .map_err(|_| "connect")?;
    // netd's data write is non-blocking: a full 4 KiB send window returns a
    // 0-count Rwrite (NOT a deferred reply), so a bulk sender must wait for
    // POLLOUT on the QTPOLL `ready` sibling (net-6b) before retrying. This is
    // the realistic streaming path -- the poll-per-window cost IS part of the
    // throughput M2 measures.
    let ready = client.ready_fd().map_err(|_| "ready fd")?;
    let ready_raw = ready.as_raw_fd();
    let mut ps = PollSet::new();
    ps.add_raw(ready_raw, PollEvents::WRITE);

    let scratch = vec![0xa5u8; M2_BUF];
    let t = Instant::now();
    let mut sent: u64 = 0;
    while sent < total {
        let take = core::cmp::min(scratch.len() as u64, total - sent) as usize;
        let mut off = 0usize;
        let mut stalls = 0u32;
        while off < take {
            match client.write(&scratch[off..take]) {
                Ok(0) => {
                    // Window full: block on POLLOUT until the server drains.
                    let woke = ps
                        .poll(PollTimeout::Millis(M2_POLL_MS))
                        .map(|r| r.into_iter().any(|e| e.is_writable()))
                        .map_err(|_| "poll")?;
                    if woke {
                        stalls = 0;
                    } else {
                        stalls += 1;
                        if stalls > M2_MAX_STALLS {
                            return Err("stalled (server not draining)");
                        }
                    }
                }
                Ok(k) => {
                    off += k;
                    sent += k as u64;
                    stalls = 0;
                }
                Err(_) => return Err("write"),
            }
        }
    }
    client.shutdown().map_err(|_| "shutdown")?; // FIN -> the server reads EOF.
    let joined = thread::join_tid(&M2_SRV_TID, M2_SENTINEL, Some(Duration::from_secs(30)));
    let dt = t.elapsed();
    drop(client);

    if joined.is_err() {
        return Err("server join timeout");
    }
    match M2_RESULT.load(Ordering::SeqCst) {
        M2_OK => {}
        M2_BIND => return Err("server bind"),
        M2_ACCEPT => return Err("server accept"),
        M2_READ => return Err("server read"),
        M2_SHORT => return Err("server short (byte count mismatch)"),
        _ => return Err("server pending"),
    }

    let us_total = dt.as_micros() as u64;
    let kibps = if us_total > 0 {
        total * 1_000_000 / 1024 / us_total
    } else {
        0
    };
    t_putstr(&format!(
        "netperf M2 throughput: {} KiB over lo in {}.{:03} ms; {} KiB/s (4 KiB window, POLLOUT-paced)\n",
        total / 1024,
        dt.as_millis(),
        dt.as_micros() % 1000,
        kibps
    ));
    Ok(())
}

// =============================================================================
// M3 -- TCP connect latency (single-threaded: connect + drain the backlog-of-1).
// =============================================================================

/// Bind one listener, then repeatedly time the active open to ESTABLISHED
/// (TcpStream::connect blocks until the PendingConnect deferred reply lands).
/// Each iteration accepts (drains the backlog-of-1 so the next connect can
/// establish) and drops both ends. Reports mean/min/max connect time.
fn m3_connect(conns: u32) -> Result<(), &'static str> {
    let addr = SocketAddrV4::new(Ipv4Addr::LOCALHOST, M3_PORT);
    let listener = TcpListener::bind(addr).map_err(|_| "bind")?;
    let mut total_ns: u64 = 0;
    let mut min_ns: u64 = u64::MAX;
    let mut max_ns: u64 = 0;
    for _ in 0..conns {
        let t = Instant::now();
        let client = TcpStream::connect(addr).map_err(|_| "connect")?;
        let dt = t.elapsed().as_nanos() as u64;
        // Drain the established call (netd's backlog is 1) so the next connect
        // can land; drop both ends (closes the fids -> netd frees the slots).
        let (server, _peer) = listener.accept().map_err(|_| "accept")?;
        drop(client);
        drop(server);
        total_ns += dt;
        if dt < min_ns {
            min_ns = dt;
        }
        if dt > max_ns {
            max_ns = dt;
        }
    }
    let mean = total_ns / conns as u64;
    t_putstr(&format!(
        "netperf M3 connect: {} dials over lo; mean {} / min {} / max {}\n",
        conns,
        us(mean),
        us(min_ns),
        us(max_ns)
    ));
    Ok(())
}

fn fail(metric: &str, why: &str) -> ! {
    t_putstr(&format!("netperf: FAIL -- {} ({})\n", metric, why));
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // `netperf [iters] [mib] [conns]` -- all optional; default = the boot probe.
    // The M2 size is in MiB from the shell, but defaults to M2_BOOT_BYTES (a
    // small fixed byte count) so the boot probe stays ~1s -- the per-window-
    // paced rate is size-independent (see M2_BOOT_BYTES).
    let args = env::args();
    let iters: u32 = args
        .get_str(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(M1_ITERS);
    let m2_bytes: u64 = match args.get_str(2).and_then(|s| s.parse::<u64>().ok()) {
        Some(mib) => mib << 20,
        None => M2_BOOT_BYTES,
    };
    let conns: u32 = args
        .get_str(3)
        .and_then(|s| s.parse().ok())
        .unwrap_or(M3_CONNS);

    t_putstr(
        "netperf: NET-PERF NP-1 -- loopback micro-bench (M1 rtt / M2 throughput / M3 connect)\n",
    );

    if let Err(e) = m1_rtt(iters) {
        fail("M1", e);
    }
    if let Err(e) = m2_throughput(m2_bytes) {
        fail("M2", e);
    }
    if let Err(e) = m3_connect(conns) {
        fail("M3", e);
    }

    t_putstr("netperf: NP-1 OK (M1+M2+M3 over the resident lo stack)\n");
    0
}
