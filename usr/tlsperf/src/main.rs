// /tlsperf -- NET-PERF NP-2: TLS handshake timing (M4) + crypto micro-bench (M5).
//
// A pure-measurement tool (docs/NET-PERF.md). M4 times a full TLS 1.3 handshake
// over netd's resident loopback (the net-8c-2 two-Thread TlsStream <->
// TlsServerStream pattern, in a timed loop); the TCG-vs-HVF contrast splits the
// crypto-CPU cost from the round-trip-wait cost (a CPU-bound handshake is ~10-50x
// faster on HVF; a wait-bound one is ~equal). M5 micro-benches the EXACT
// RustCrypto primitives rustls-rustcrypto uses (aarch64 here has no AES/SHA
// intrinsics, so these are the software-crypto cost TLS pays -- a key NP-4
// lever): X25519 agree, AES-128-GCM seal/open, SHA-256, ECDSA-P256 verify.
//
// All timing uses the LS-K CLOCK_MONOTONIC (libthyla_rs::time::Instant).
//
// Modes:
//   * default (no args) -- the joey boot probe: a SHORT fixed run
//     (M4_HANDSHAKES + the M5 battery), logged. The TCG + HVF boot logs give the
//     TCG-vs-HVF contrast for free.
//   * `tlsperf [handshakes]` -- more M4 iterations, from the shell.
//
// Pure userspace; a buggy client corrupts only its own state (the kernel + netd
// + the cert validator check everything); composes I-1/I-5/I-23/I-28, adds no
// invariant.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, Ordering};
use core::time::Duration;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::net::{Ipv4Addr, SocketAddrV4, TcpListener, TcpStream};
use libthyla_rs::thread;
use libthyla_rs::time::Instant;
use libthyla_rs::{t_burrow_attach, t_exits, t_putstr};

use aes_gcm::aead::{Aead, KeyInit};
use aes_gcm::{Aes128Gcm, Key, Nonce};
use p256::ecdsa::signature::{Signer, Verifier};
use p256::ecdsa::{Signature, SigningKey, VerifyingKey};
use sha2::{Digest, Sha256};
use x25519_dalek::{PublicKey, StaticSecret};

/// Format nanoseconds as "N.NNN us" (microseconds, 3 decimals).
fn us(ns: u64) -> String {
    format!("{}.{:03} us", ns / 1000, ns % 1000)
}

// =============================================================================
// M4 -- TLS 1.3 handshake timing (two-Thread TlsStream <-> TlsServerStream).
// =============================================================================

const M4_PORT: u16 = 7821;
const M4_HOST: &str = "loopback.test";
const M4_CERT: &[u8] = include_bytes!("../testdata/loopback-cert.pem");
const M4_KEY: &[u8] = include_bytes!("../testdata/loopback-key.pem");
/// rustls's server handshake is moderately stack-hungry; 1 MiB is generous
/// (a too-small stack faults loudly past the mapped region, never silent).
const M4_SRV_STACK: u64 = 1024 * 1024;
const M4_SENTINEL: u32 = 1;
const M4_HANDSHAKES: u32 = 10; // boot-probe default.

// The server Thread's outcome codes.
const SR_PENDING: u32 = 0;
const SR_OK: u32 = 1;
const SR_CFG: u32 = 2;
const SR_BIND: u32 = 3;
const SR_ACCEPT: u32 = 4;
const SR_TLS: u32 = 5;

static M4_SRV_TID: AtomicU32 = AtomicU32::new(0);
static M4_READY: AtomicU32 = AtomicU32::new(0);
static M4_RESULT: AtomicU32 = AtomicU32::new(SR_PENDING);

/// The server Thread entry (arg = the handshake count): announce on the
/// loopback, then accept + run the SERVER side of `n` TLS handshakes, recording
/// its outcome for main to read after the join. Never prints (only main does).
extern "C" fn m4_server_entry(n: u64) {
    let _ = thread::set_tid_address(&M4_SRV_TID);
    let code = m4_server_run(n as u32);
    M4_RESULT.store(code, Ordering::SeqCst);
    thread::exit_self();
}

fn m4_server_run(n: u32) -> u32 {
    let cfg = match tls::server_config_single_cert(M4_CERT, M4_KEY) {
        Ok(c) => c,
        Err(_) => return SR_CFG,
    };
    let listener = match TcpListener::bind(SocketAddrV4::new(Ipv4Addr::LOCALHOST, M4_PORT)) {
        Ok(l) => l,
        Err(_) => return SR_BIND,
    };
    M4_READY.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake_all(&M4_READY);

    for _ in 0..n {
        let (sock, _peer) = match listener.accept() {
            Ok(s) => s,
            Err(_) => return SR_ACCEPT,
        };
        // accept() drives the SERVER handshake to completion (reads the client
        // Finished); a close sends close_notify. No app data -- M4 is the
        // handshake only.
        let mut tls = match tls::TlsServerStream::accept(sock, cfg.clone()) {
            Ok(t) => t,
            Err(_) => return SR_TLS,
        };
        tls.close();
    }
    SR_OK
}

/// Spawn the server Thread, run `n` client-side TLS handshakes over the live
/// loopback, timing ONLY the `TlsStream::connect` (the TLS handshake) -- the TCP
/// connect is untimed (M3 owns it). The TCG-vs-HVF contrast then splits the
/// crypto-CPU from the round-trip-wait.
fn run_m4(n: u32) -> Result<(), &'static str> {
    M4_READY.store(0, Ordering::SeqCst);
    M4_RESULT.store(SR_PENDING, Ordering::SeqCst);
    M4_SRV_TID.store(M4_SENTINEL, Ordering::SeqCst);

    let stack = unsafe { t_burrow_attach(M4_SRV_STACK) };
    if stack < 0 {
        return Err("server stack attach");
    }
    let sp = (stack as u64) + M4_SRV_STACK;
    if unsafe { thread::spawn_raw(m4_server_entry as *const () as u64, sp, n as u64, 0) }.is_err() {
        return Err("server spawn");
    }

    let mut waited = 0;
    while M4_READY.load(Ordering::Acquire) == 0 && waited < 8 {
        let _ = libthyla_rs::torpor::wait(&M4_READY, 0, Some(Duration::from_secs(1)));
        waited += 1;
    }
    if M4_READY.load(Ordering::Acquire) == 0 {
        return Err("server announce timeout");
    }

    // The client trust store + config (built once): trust the self-signed leaf
    // as its own root (the isolated-test pattern, same as net-echo/tls-smoke).
    let roots = tls::load_roots_pem(M4_CERT).map_err(|_| "trust store")?;
    let cfg = tls::client_config(roots);
    let addr = SocketAddrV4::new(Ipv4Addr::LOCALHOST, M4_PORT);

    let mut total_ns: u64 = 0;
    let mut min_ns: u64 = u64::MAX;
    let mut max_ns: u64 = 0;
    for _ in 0..n {
        let sock = TcpStream::connect(addr).map_err(|_| "tcp connect")?;
        let t = Instant::now();
        let mut tls =
            tls::TlsStream::connect(sock, M4_HOST, cfg.clone()).map_err(|_| "tls handshake")?;
        let dt = t.elapsed().as_nanos() as u64;
        tls.close();
        total_ns += dt;
        if dt < min_ns {
            min_ns = dt;
        }
        if dt > max_ns {
            max_ns = dt;
        }
    }

    let joined = thread::join_tid(&M4_SRV_TID, M4_SENTINEL, Some(Duration::from_secs(60)));
    if joined.is_err() {
        return Err("server join timeout");
    }
    match M4_RESULT.load(Ordering::SeqCst) {
        SR_OK => {}
        SR_CFG => return Err("server tls config"),
        SR_BIND => return Err("server bind"),
        SR_ACCEPT => return Err("server accept"),
        SR_TLS => return Err("server tls accept"),
        _ => return Err("server pending"),
    }

    let mean = total_ns / n as u64;
    t_putstr(&format!(
        "tlsperf M4 handshake: {} TLS 1.3 handshakes over lo; mean {} / min {} / max {} (cert verify incl.)\n",
        n,
        us(mean),
        us(min_ns),
        us(max_ns)
    ));
    Ok(())
}

// =============================================================================
// M5 -- crypto micro-bench (the exact RustCrypto primitives TLS pays for).
// =============================================================================

// Boot-probe iteration counts: each op's cost is deterministic, so these are
// kept modest (the boot probe runs on EVERY boot) -- enough for a stable mean,
// not so many that the asymmetric ops (P256 verify ~6 ms/op on TCG) bloat the
// boot. ~1 s total on TCG; far less on HVF.
const AES_ITERS: u64 = 500; // seal + open each, of a 1 KiB record.
const SHA_ITERS: u64 = 1000; // a 1 KiB block.
const X25519_ITERS: u64 = 50; // a variable-base scalar mult (the ECDHE agree).
const P256_ITERS: u64 = 50; // an ECDSA-P256 verify (cert validation).
const RECORD: usize = 1024; // the bench record/block size.

fn rand32() -> [u8; 32] {
    let mut b = [0u8; 32];
    let _ = libthyla_rs::rand::getrandom(&mut b);
    b
}

/// ops/sec given iters over `ns` nanoseconds (integer; guards div-by-0).
fn ops_per_sec(iters: u64, ns: u64) -> u64 {
    if ns == 0 {
        0
    } else {
        iters.saturating_mul(1_000_000_000) / ns
    }
}

/// "tlsperf M5 <label>: N ops/s (X.XXX us/op, K iters)".
fn opsline(label: &str, iters: u64, ns: u64) -> String {
    let per_op = if iters > 0 { ns / iters } else { 0 };
    format!(
        "tlsperf M5 {}: {} ops/s ({}.{:03} us/op, {} iters)\n",
        label,
        ops_per_sec(iters, ns),
        per_op / 1000,
        per_op % 1000,
        iters
    )
}

/// AES-128-GCM seal + open of a `RECORD`-byte buffer. Returns (seal_ns, open_ns).
/// A fixed nonce is reused -- cryptographically wrong, but this is a SPEED bench,
/// not a secure channel; the cost is nonce-independent.
fn bench_aes() -> (u64, u64) {
    let r = rand32();
    let mut kb = [0u8; 16];
    kb.copy_from_slice(&r[..16]);
    let cipher = Aes128Gcm::new(Key::<Aes128Gcm>::from_slice(&kb));
    let nonce = Nonce::from_slice(&[0u8; 12]);
    let pt = vec![0xa5u8; RECORD];

    let t = Instant::now();
    let mut ct: Vec<u8> = Vec::new();
    for _ in 0..AES_ITERS {
        ct = cipher.encrypt(nonce, pt.as_ref()).expect("seal");
        core::hint::black_box(&ct);
    }
    let seal_ns = t.elapsed().as_nanos() as u64;

    let t = Instant::now();
    for _ in 0..AES_ITERS {
        let dec = cipher.decrypt(nonce, ct.as_ref()).expect("open");
        core::hint::black_box(&dec);
    }
    let open_ns = t.elapsed().as_nanos() as u64;
    (seal_ns, open_ns)
}

/// SHA-256 of a `RECORD`-byte block.
fn bench_sha256() -> u64 {
    let block = vec![0xa5u8; RECORD];
    let t = Instant::now();
    for _ in 0..SHA_ITERS {
        let h = Sha256::digest(&block);
        core::hint::black_box(&h);
    }
    t.elapsed().as_nanos() as u64
}

/// X25519 Diffie-Hellman agree (the ECDHE key share -- a variable-base scalar
/// mult, the dominant per-handshake asymmetric cost).
fn bench_x25519() -> u64 {
    let a = StaticSecret::from(rand32());
    let b = StaticSecret::from(rand32());
    let b_pub = PublicKey::from(&b);
    let t = Instant::now();
    for _ in 0..X25519_ITERS {
        let shared = a.diffie_hellman(&b_pub);
        core::hint::black_box(shared.as_bytes());
    }
    t.elapsed().as_nanos() as u64
}

/// ECDSA-P256 signature verify (the cert-chain + CertVerify cost a TLS client
/// pays per handshake). Signs once in setup (RFC6979 deterministic -- no RNG),
/// then times the verify. `None` only if the derived key is the rare invalid
/// scalar (degrade gracefully).
fn bench_p256_verify() -> Option<u64> {
    let seed: [u8; 32] = Sha256::digest(b"tlsperf-p256-bench-key").into();
    let sk = SigningKey::from_bytes((&seed).into()).ok()?;
    let vk = VerifyingKey::from(&sk);
    let msg = b"tlsperf p256 verify benchmark message";
    let sig: Signature = sk.sign(msg);
    let t = Instant::now();
    for _ in 0..P256_ITERS {
        let ok = vk.verify(msg, &sig).is_ok();
        core::hint::black_box(ok);
    }
    Some(t.elapsed().as_nanos() as u64)
}

fn run_m5() {
    t_putstr("tlsperf: M5 crypto micro-bench (RustCrypto software primitives)\n");
    let (seal_ns, open_ns) = bench_aes();
    t_putstr(&opsline("aes128gcm-seal-1KiB", AES_ITERS, seal_ns));
    t_putstr(&opsline("aes128gcm-open-1KiB", AES_ITERS, open_ns));
    t_putstr(&opsline("sha256-1KiB", SHA_ITERS, bench_sha256()));
    t_putstr(&opsline("x25519-agree", X25519_ITERS, bench_x25519()));
    match bench_p256_verify() {
        Some(ns) => t_putstr(&opsline("ecdsa-p256-verify", P256_ITERS, ns)),
        None => t_putstr("tlsperf M5 ecdsa-p256-verify: SKIP (key derivation)\n"),
    };
}

fn fail(metric: &str, why: &str) -> ! {
    t_putstr(&format!("tlsperf: FAIL -- {} ({})\n", metric, why));
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // `tlsperf [handshakes]` -- optional; default = the boot probe.
    let args = env::args();
    let handshakes: u32 = args
        .get_str(1)
        .and_then(|s| s.parse().ok())
        .unwrap_or(M4_HANDSHAKES);

    t_putstr("tlsperf: NET-PERF NP-2 -- TLS handshake (M4) + crypto micro-bench (M5)\n");

    if let Err(e) = run_m4(handshakes) {
        fail("M4", e);
    }
    run_m5();

    t_putstr("tlsperf: NP-2 OK (M4+M5 over the resident lo stack + RustCrypto)\n");
    0
}
