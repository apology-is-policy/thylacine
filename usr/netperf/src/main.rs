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
use libthyla_rs::net::{Ipv4Addr, SocketAddrV4, TcpListener, TcpStream, WeftFlow};
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
// MW -- Weft zero-copy loopback throughput (the NET-THROUGHPUT section 8
// benchmark; #269 M6). The M2 twin: the SAME bulk transfer over the SAME
// resident `lo`, but the SEND side rides a WeftFlow (push/pop/wait over Loom ->
// Tweftio -> netd reads the ring IN PLACE) instead of TcpStream::write (byte-copy
// + the per-4-KiB-window POLLOUT cadence). The drain server stays a byte-copy
// TcpStream reader, so this measures the SEND-side zero-copy delta -- on loopback
// the receive copy + netd's lo delivery still dominate, so the win is MODEST
// here; the full zero-copy payoff is the NIC path (the copy is the bottleneck on
// a real link), the slirp-bounded M6. The HONEST in-guest number, logged head-to-
// head with M2's byte-copy KiB/s.
// =============================================================================

const MW_PORT: u16 = 7814;
const MW_BUF: usize = 16 * 1024; // the drain server's read scratch.
// The boot-probe transfer: a few weft rings' worth. Weft has no POLLOUT-per-
// window cadence (each push parks on the Loom CQE), so a larger byte count
// measures the rate stably without the M2 size-independence caveat.
const MW_BOOT_BYTES: u64 = 256 * 1024;

static MW_SRV_TID: AtomicU32 = AtomicU32::new(0);
static MW_READY: AtomicU32 = AtomicU32::new(0);
static MW_RESULT: AtomicU32 = AtomicU32::new(M2_PENDING);
static MW_GOT: AtomicU64 = AtomicU64::new(0);
static MW_EXPECT: AtomicU64 = AtomicU64::new(0);

/// The drain server Thread (a byte-copy TcpStream reader, exactly like M2's): the
/// receive side stays byte-copy so MW isolates the SEND-side zero-copy delta.
extern "C" fn mw_server_entry(_arg: u64) {
    let _ = thread::set_tid_address(&MW_SRV_TID);
    let code = mw_server_run();
    MW_RESULT.store(code, Ordering::SeqCst);
    thread::exit_self();
}

fn mw_server_run() -> u32 {
    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, MW_PORT)) {
        Ok(l) => l,
        Err(_) => return M2_BIND,
    };
    MW_READY.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake_all(&MW_READY);
    let (mut server, _peer) = match listener.accept() {
        Ok(s) => s,
        Err(_) => return M2_ACCEPT,
    };
    let expect = MW_EXPECT.load(Ordering::Acquire);
    let mut buf = vec![0u8; MW_BUF];
    let mut got: u64 = 0;
    loop {
        let n = match server.read(&mut buf) {
            Ok(n) => n,
            Err(_) => return M2_READ,
        };
        if n == 0 {
            break; // EOF: the client half-closed (FIN).
        }
        got += n as u64;
    }
    MW_GOT.store(got, Ordering::SeqCst);
    if got != expect {
        return M2_SHORT;
    }
    M2_OK
}

/// Stream `total` bytes through a WeftFlow over loopback while a byte-copy drain
/// server reads to EOF, timed from the first push to the join. Each push moves up
/// to `payload_capacity()` bytes zero-copy -- the kernel routes the Loom WRITE to
/// `Tweftio` and netd reads netd's ring slice IN PLACE; `wait` parks on the Loom
/// CQE (no POLLOUT-per-window cadence). The fill below is the bench's own write
/// into the shared ring (the only copy on the send side).
fn weft_throughput(total: u64) -> Result<(), &'static str> {
    MW_READY.store(0, Ordering::SeqCst);
    MW_RESULT.store(M2_PENDING, Ordering::SeqCst);
    MW_GOT.store(0, Ordering::SeqCst);
    MW_EXPECT.store(total, Ordering::SeqCst);
    MW_SRV_TID.store(M2_SENTINEL, Ordering::SeqCst);

    let stack = unsafe { t_burrow_attach(M2_STACK) };
    if stack < 0 {
        return Err("server stack attach");
    }
    let sp = (stack as u64) + M2_STACK;
    if unsafe { thread::spawn_raw(mw_server_entry as *const () as u64, sp, 0, 0) }.is_err() {
        return Err("server spawn");
    }
    let mut waited = 0;
    while MW_READY.load(Ordering::Acquire) == 0 && waited < 8 {
        let _ = libthyla_rs::torpor::wait(&MW_READY, 0, Some(Duration::from_secs(1)));
        waited += 1;
    }
    if MW_READY.load(Ordering::Acquire) == 0 {
        return Err("server announce timeout");
    }

    let mut client = TcpStream::connect(SocketAddrV4::new(Ipv4Addr::LOCALHOST, MW_PORT))
        .map_err(|_| "connect")?;
    // open() reads only the data fd (a Copy i32), holding no borrow of `client`,
    // so `client` stays usable for the shutdown below.
    let mut flow = WeftFlow::open(&client).map_err(|_| "weft open")?;
    let cap = flow.payload_capacity() as u64;
    if cap == 0 {
        return Err("zero payload capacity");
    }
    // A weft push moves at most the socket's FREE tx window (netd's data_send is
    // non-blocking, like the byte-copy write); a full 4 KiB window returns a
    // 0-count Rweftio -- back-pressure, NOT EOF (the connection is live). So a bulk
    // weft sender pays the SAME per-window POLLOUT cadence M2 does (the ring is 255
    // KiB but the socket tx buffer is 4 KiB -- the buffer, not the ring, paces
    // loopback throughput; the weft win is the per-op COPY savings, not the
    // cadence). Poll the QTPOLL `ready` sibling for POLLOUT on a 0 + retry.
    let ready = client.ready_fd().map_err(|_| "ready fd")?;
    let mut ps = PollSet::new();
    ps.add_raw(ready.as_raw_fd(), PollEvents::WRITE);

    let t = Instant::now();
    let mut sent: u64 = 0;
    let mut stalls = 0u32;
    // netd's data_send accepts at most the socket's free tx window (~4 KiB) per op,
    // so pushing the WHOLE 254 KiB ring would re-read ~254 KiB of ring per 4 KiB
    // sent (a bench artifact). Size each push to the window: start at a modest app
    // chunk + self-tune to the last accepted count, so the ring read tracks what
    // data_send will take. (A larger socket tx buffer is the v1.x lever that lets
    // one weft push absorb a big send -- the real zero-copy throughput win.)
    let mut hint = core::cmp::min(cap, M2_BUF as u64);
    while sent < total {
        let chunk = core::cmp::min(hint, total - sent) as usize;
        flow.tx_buf()[..chunk].fill(0xa5); // fill the shared ring in place.
        let tok = flow.push(chunk).map_err(|_| "push")?;
        let moved = flow.wait(tok).map_err(|_| "wait")?.bytes();
        if moved == 0 {
            // Full send window: wait for the drain server to read, then retry the
            // push (the server only EOFs after `total` bytes, so 0 here is always
            // back-pressure). Bounded so a never-draining peer fails loudly.
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
            continue;
        }
        stalls = 0;
        sent += moved as u64;
        // Self-tune: the next push fills only what the socket window took (>= 4 KiB
        // so we never under-fill the live window), so the ring read tracks the send.
        hint = core::cmp::max(moved as u64, 4096);
    }
    client.shutdown().map_err(|_| "shutdown")?; // FIN -> the server reads EOF.
    let joined = thread::join_tid(&MW_SRV_TID, M2_SENTINEL, Some(Duration::from_secs(30)));
    let dt = t.elapsed();
    drop(flow); // drop the Loom ring (releases the data-fd registration) first,
    drop(client); // then the connection fids.

    if joined.is_err() {
        return Err("server join timeout");
    }
    match MW_RESULT.load(Ordering::SeqCst) {
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
        "netperf MW weft-throughput: {} KiB over lo in {}.{:03} ms; {} KiB/s (zero-copy push, {} KiB ring; loopback is socket-tx-window-bound so this pays per-op Loom+Tweftio overhead -- the win is the NIC / large-buffer path, not lo)\n",
        total / 1024,
        dt.as_millis(),
        dt.as_micros() % 1000,
        kibps,
        cap / 1024
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

// =============================================================================
// M6 -- the NIC path (guest -> a HOST server over the virtio-net NIC + slirp).
// =============================================================================
//
// M1/M2/M3 are self-loop over the resident `lo` stack (this Proc is both ends).
// M6 is CLIENT-ONLY against a HOST server reached through a QEMU slirp `guestfwd`
// (`tools/np3-bench.sh` starts the host server + the launcher maps the magic
// `10.0.2.100:7820` -> the host's `127.0.0.1`). It isolates the QEMU virtio-net +
// slirp delta -- and DIRECTLY exposes the NET-PERF section-2.1 RX-wake floor: a
// host-driven reply (an echo, a SYN-ACK, the bulk ACKs) arrives on the NIC with
// NO 9P event to wake netd, so each completion waits up to netd's ~50 ms idle
// poll. Compare M6-rtt to M1, M6-connect to M3, M6-bw to M2.
//
// The host server speaks a 1-byte mode header per connection:
//   'r' -> echo every subsequent byte (RTT);  's' -> drain to EOF then send one
//   'A' ack (bulk sink);  no byte (the client connects + closes) -> connect-only.
//
// BEST-EFFORT: when no host server is reachable (the standard boot -- the
// guestfwd is inert / absent), the bounded gate `connect_timeout` resolves to a
// fast SKIP and the run still exits 0 (M6 is data, never a boot gate). The gate
// is bounded so an unreachable peer can NEVER hang the boot.

const M6_GATE_MS: u32 = 600; // the bounded reachability gate (skip-fast budget).
const M6_BW_BUF: usize = 16 * 1024;
const M6_POLL_MS: u32 = 2000;
const M6_MAX_STALLS: u32 = 15;
// The host-injected per-echo delay for the floor probe. Chosen well BELOW netd's
// IDLE_POLL_MIN_MS (50 ms) idle-poll floor: a reply that lands AFTER netd has
// deferred the read + parked in t_poll waits the floor, NOT the real RTT. So the
// floor probe's measured RTT >> this delay is the direct, isolated measurement of
// the NET-PERF section-2.1 RX-wake floor (the M6-rtt instant echo never parks).
const M6_FLOOR_DELAY_MS: u16 = 5;

/// One round of M6: gate the peer (bounded), then RTT + floor + connect + bulk.
/// Prints a SKIP line + returns Ok when the peer is unreachable (best-effort).
///
/// Each metric uses a DISTINCT dest port (`port`, `port+1`, `port+2`), where the
/// PORT selects the host server's behavior (echo / delayed-echo / sink) -- no
/// in-band mode byte. This is load-bearing: a slirp `guestfwd` maps each rule to
/// its OWN host connection, and sequential dials to the SAME dest:port coalesce
/// onto one host connection (observed empirically), which would conflate the
/// metrics. One metric per port keeps each measurement isolated.
fn run_m6(ip: Ipv4Addr, port: u16, iters: u32, kib: u64, conns: u32) {
    let rtt_addr = SocketAddrV4::new(ip, port); // echo
    let floor_addr = SocketAddrV4::new(ip, port.wrapping_add(1)); // delayed echo
    let bw_addr = SocketAddrV4::new(ip, port.wrapping_add(2)); // sink

    let mut gate = match TcpStream::connect_timeout(
        rtt_addr,
        Duration::from_millis(M6_GATE_MS as u64),
    ) {
        Ok(c) => c,
        Err(_) => {
            t_putstr(&format!(
                    "netperf M6 SKIP: no host server reachable at {} over the NIC (guestfwd inert; standard boot)\n",
                    rtt_addr
                ));
            return;
        }
    };

    // M6-rtt: reuse the established gate connection (the echo port). The reply is
    // near-instant, so it is usually already in the RX ring when netd next polls
    // -> the device + slirp per-op cost (the min); a reply that lands just after
    // netd parks pays the ~50 ms floor (the max) -- the bimodal device-vs-floor.
    if let Err(e) = m6_rtt(&mut gate, iters) {
        t_putstr(&format!("netperf M6 rtt: ERR ({})\n", e));
    }
    drop(gate);

    // M6-floor: a fresh dial to the DELAYED-echo port (the host sleeps
    // M6_FLOOR_DELAY_MS before echoing). The reply ALWAYS lands after netd has
    // deferred the read + parked, so the RTT is the ~50 ms RX-wake floor, NOT the
    // 5 ms host delay -- the clean isolation of the floor (the device-or-us core).
    if let Err(e) = m6_floor(floor_addr, iters) {
        t_putstr(&format!("netperf M6 floor: ERR ({})\n", e));
    }

    // M6-connect: blocking dials to the echo port; the connect-to-ESTABLISHED
    // latency. These DO coalesce among themselves (same dest:port), so they
    // measure netd's connect processing with a near-instant slirp SYN-ACK (no
    // SYN-ACK floor -- slirp answers at once), i.e. the NIC connect ~= M3 (lo).
    if let Err(e) = m6_connect(rtt_addr, conns) {
        t_putstr(&format!("netperf M6 connect: ERR ({})\n", e));
    }

    // M6-bw: a fresh connection to the SINK port, POLLOUT-paced bulk send.
    if let Err(e) = m6_throughput(bw_addr, kib * 1024) {
        t_putstr(&format!("netperf M6 throughput: ERR ({})\n", e));
    }
}

fn m6_rtt(client: &mut TcpStream, iters: u32) -> Result<(), &'static str> {
    let ping = [0x5au8; 1];
    let mut buf = [0u8; 1];
    let mut total_ns: u64 = 0;
    let mut min_ns: u64 = u64::MAX;
    let mut max_ns: u64 = 0;
    for _ in 0..iters {
        let t = Instant::now();
        client.write_all(&ping).map_err(|_| "write")?;
        let n = client.read(&mut buf).map_err(|_| "read")?;
        if n == 0 {
            return Err("peer EOF");
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
        "netperf M6 rtt: {} round-trips over NIC; mean {} / min {} / max {} (1B echo)\n",
        iters,
        us(mean),
        us(min_ns),
        us(max_ns)
    ));
    Ok(())
}

/// The floor probe: the host echoes each byte after sleeping M6_FLOOR_DELAY_MS,
/// so the reply arrives AFTER netd has deferred the read + parked in t_poll. The
/// measured RTT is then netd's idle-poll floor (~50 ms), not the 5 ms host delay
/// -- the direct measurement that the NIC slowness is OUR poll model, not the
/// QEMU device (M6-rtt, instant echo, never parks -> ~370 us).
fn m6_floor(addr: SocketAddrV4, iters: u32) -> Result<(), &'static str> {
    // The dest port (port+1) IS the delayed-echo behavior: the host sleeps
    // M6_FLOOR_DELAY_MS before each echo. No in-band mode byte (a same-port
    // reconnect would coalesce; the port selects the behavior).
    let mut client = TcpStream::connect(addr).map_err(|_| "connect")?;
    let ping = [0x5au8; 1];
    let mut buf = [0u8; 1];
    let mut total_ns: u64 = 0;
    let mut min_ns: u64 = u64::MAX;
    let mut max_ns: u64 = 0;
    for _ in 0..iters {
        let t = Instant::now();
        client.write_all(&ping).map_err(|_| "write")?;
        let n = client.read(&mut buf).map_err(|_| "read")?;
        if n == 0 {
            return Err("peer EOF");
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
        "netperf M6 floor: {} round-trips over NIC, host echo delayed {} ms; mean {} / min {} / max {} (excess over {} ms = netd RX-wake floor)\n",
        iters,
        M6_FLOOR_DELAY_MS,
        us(mean),
        us(min_ns),
        us(max_ns),
        M6_FLOOR_DELAY_MS
    ));
    Ok(())
}

fn m6_connect(addr: SocketAddrV4, conns: u32) -> Result<(), &'static str> {
    let mut total_ns: u64 = 0;
    let mut min_ns: u64 = u64::MAX;
    let mut max_ns: u64 = 0;
    for _ in 0..conns {
        let t = Instant::now();
        let c = TcpStream::connect(addr).map_err(|_| "connect")?;
        let dt = t.elapsed().as_nanos() as u64;
        drop(c); // FIN -> the host accept's recv(1) sees EOF + closes.
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
        "netperf M6 connect: {} dials over NIC; mean {} / min {} / max {}\n",
        conns,
        us(mean),
        us(min_ns),
        us(max_ns)
    ));
    Ok(())
}

fn m6_throughput(addr: SocketAddrV4, total: u64) -> Result<(), &'static str> {
    let mut client = TcpStream::connect(addr).map_err(|_| "connect")?;
    let ready = client.ready_fd().map_err(|_| "ready fd")?;
    let mut ps = PollSet::new();
    ps.add_raw(ready.as_raw_fd(), PollEvents::WRITE);

    // The dest port (port+2) IS the sink behavior: the host drains to EOF then
    // sends one ack byte. No in-band mode byte.
    let scratch = vec![0xa5u8; M6_BW_BUF];
    let t = Instant::now();
    let mut sent: u64 = 0;
    while sent < total {
        let take = core::cmp::min(scratch.len() as u64, total - sent) as usize;
        let mut off = 0usize;
        let mut stalls = 0u32;
        while off < take {
            match client.write(&scratch[off..take]) {
                Ok(0) => {
                    let woke = ps
                        .poll(PollTimeout::Millis(M6_POLL_MS))
                        .map(|r| r.into_iter().any(|e| e.is_writable()))
                        .map_err(|_| "poll")?;
                    if woke {
                        stalls = 0;
                    } else {
                        stalls += 1;
                        if stalls > M6_MAX_STALLS {
                            return Err("stalled");
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
    // Time = send-completion (all bytes accepted, POLLOUT-paced by the host's
    // ACKs reopening the window). We deliberately do NOT half-close + read an ack
    // here: a `shutdown()` (hangup) followed by a `read` of the host's post-FIN
    // reply HANGS -- netd does not deliver post-hangup RX to a blocking read
    // (tracked, a netd read-after-SHUT_WR seam). Dropping `client` (below) sends
    // the FIN; the host drains the tail + closes. The last window's transit is
    // not counted, a < 1/N_windows overcount, fine for a throughput proxy.
    let dt = t.elapsed();
    drop(client);
    let us_total = dt.as_micros() as u64;
    let kibps = if us_total > 0 {
        total * 1_000_000 / 1024 / us_total
    } else {
        0
    };
    t_putstr(&format!(
        "netperf M6 throughput: {} KiB over NIC in {}.{:03} ms; {} KiB/s (send-completion to sink)\n",
        total / 1024,
        dt.as_millis(),
        dt.as_micros() % 1000,
        kibps
    ));
    Ok(())
}

fn fail(metric: &str, why: &str) -> ! {
    t_putstr(&format!("netperf: FAIL -- {} ({})\n", metric, why));
    unsafe { t_exits(1) }
}

// M6 boot-probe defaults (small + bounded: each NIC op pays the ~50 ms RX-wake
// floor, so 20 RTTs + 10 dials + 32 KiB measure it in ~2-3 s).
const M6_ITERS: u32 = 20;
const M6_CONNS: u32 = 10;
const M6_KIB: u64 = 32;
const M6_DEFAULT_IP: Ipv4Addr = Ipv4Addr::new(10, 0, 2, 100); // the slirp guestfwd magic.
const M6_DEFAULT_PORT: u16 = 7820;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();

    // `netperf nic [ip] [port] [iters] [kib] [conns]` -- the NET-PERF M6 NIC
    // path (NP-3). Best-effort: a fast bounded SKIP when no host server is
    // reachable (the standard boot), real numbers when np3-bench.sh runs a
    // host server behind the slirp guestfwd. Always exits 0 (M6 is data).
    if args.get_str(1) == Some("nic") {
        let ip = args
            .get_str(2)
            .and_then(|s| Ipv4Addr::parse(s).ok())
            .unwrap_or(M6_DEFAULT_IP);
        let port: u16 = args
            .get_str(3)
            .and_then(|s| s.parse().ok())
            .unwrap_or(M6_DEFAULT_PORT);
        let iters: u32 = args
            .get_str(4)
            .and_then(|s| s.parse().ok())
            .unwrap_or(M6_ITERS);
        let kib: u64 = args
            .get_str(5)
            .and_then(|s| s.parse().ok())
            .unwrap_or(M6_KIB);
        let conns: u32 = args
            .get_str(6)
            .and_then(|s| s.parse().ok())
            .unwrap_or(M6_CONNS);
        t_putstr("netperf: NET-PERF NP-3 -- NIC path (M6 rtt / connect / throughput)\n");
        run_m6(ip, port, iters, kib, conns);
        t_putstr("netperf: NP-3 OK (M6 over the NIC, or a best-effort SKIP)\n");
        return 0;
    }

    // `netperf [iters] [mib] [conns]` -- all optional; default = the boot probe.
    // The M2 size is in MiB from the shell, but defaults to M2_BOOT_BYTES (a
    // small fixed byte count) so the boot probe stays ~1s -- the per-window-
    // paced rate is size-independent (see M2_BOOT_BYTES).
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
    // MW -- the Weft zero-copy throughput twin of M2 (NET-THROUGHPUT s8; #269 M6).
    if let Err(e) = weft_throughput(MW_BOOT_BYTES) {
        fail("MW", e);
    }

    t_putstr("netperf: NP-1 OK (M1+M2+M3+MW[weft] over the resident lo stack)\n");
    0
}
