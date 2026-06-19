// /tls-smoke -- the in-guest TLS test harness (the boot gate).
//
// net-7c-1: emits a real ClientHello -- proves the RustCrypto provider, the
// kernel CSPRNG (client random + ECDHE key share), and the record layer run on
// aarch64.
//
// net-7c-2: a full client<->server handshake + app-data round-trip entirely in
// memory (the `tls` loopback driver), using a baked self-signed test cert as
// both the server identity and the client's trust root. This exercises the REAL
// WebPki certificate verification (chain + validity window vs the LS-K wall
// clock + the SAN matching the server name) and the record layer end to end --
// the deterministic, peer-independent proof (the net-3d loopback pattern) that
// TLS works, not just that a ClientHello encodes.
//
// joey spawns + reaps + asserts exit 0 (WITH CAP_CSPRNG_READ), so a regression
// in either gates the boot.
#![no_std]
#![no_main]
extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::{t_exits, t_putstr};

/// The baked self-signed test leaf (ECDSA P-256, SAN=DNS:loopback.test, valid
/// 2020..2044). Used as BOTH the server's identity and the client's sole trust
/// anchor, so the handshake verifies a real (if self-issued) chain.
const LOOPBACK_CERT: &[u8] = include_bytes!("../testdata/loopback-cert.pem");
const LOOPBACK_KEY: &[u8] = include_bytes!("../testdata/loopback-key.pem");
const LOOPBACK_HOST: &str = "loopback.test";

/// The full in-memory handshake + round-trip. Returns true iff both sides saw
/// exactly the bytes the other sent (so the handshake completed, the cert
/// verified, and the record layer carried app data faithfully).
fn loopback_e2e() -> bool {
    let server_cfg = match tls::server_config_single_cert(LOOPBACK_CERT, LOOPBACK_KEY) {
        Ok(c) => c,
        Err(_) => {
            t_putstr("tls-smoke: loopback server config FAILED\n");
            return false;
        }
    };
    // The client trusts the self-signed leaf as its own root (a self-signed cert
    // is its own trust anchor -- the standard isolated-test pattern).
    let roots = match tls::load_roots_pem(LOOPBACK_CERT) {
        Ok(r) => r,
        Err(_) => {
            t_putstr("tls-smoke: loopback trust store FAILED\n");
            return false;
        }
    };
    let client_cfg = tls::client_config(roots);

    let request = b"GET / HTTP/1.0\r\nHost: loopback.test\r\n\r\n";
    let response = b"HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello";

    match tls::loopback_roundtrip(client_cfg, server_cfg, LOOPBACK_HOST, request, response) {
        Ok((server_saw, client_saw)) => {
            if server_saw == request && client_saw == response {
                t_putstr(
                    "tls-smoke: loopback E2E PASS (handshake + cert verify + app-data round-trip)\n",
                );
                true
            } else {
                t_putstr("tls-smoke: loopback E2E round-trip MISMATCH\n");
                false
            }
        }
        Err(_) => {
            t_putstr("tls-smoke: loopback E2E handshake FAILED\n");
            false
        }
    }
}

/// The negative half: a client that does NOT trust the server's cert (an empty
/// trust store) must REJECT the handshake. Proves the verifier actually
/// validates the chain -- it is NOT a permissive accept-all (the cert-validation
/// regression a real TLS client must never have). Without this, the positive
/// E2E alone could pass with a broken verifier.
fn loopback_rejects_untrusted() -> bool {
    let server_cfg = match tls::server_config_single_cert(LOOPBACK_CERT, LOOPBACK_KEY) {
        Ok(c) => c,
        Err(_) => {
            t_putstr("tls-smoke: untrusted-reject server config FAILED\n");
            return false;
        }
    };
    let client_cfg = tls::client_config_untrusting();
    let request = b"GET / HTTP/1.0\r\n\r\n";
    let response = b"HTTP/1.0 200 OK\r\n\r\n";
    match tls::loopback_roundtrip(client_cfg, server_cfg, LOOPBACK_HOST, request, response) {
        Err(_) => {
            t_putstr("tls-smoke: untrusted-cert REJECTED (verifier validates the chain)\n");
            true
        }
        Ok(_) => {
            // The handshake completed against an untrusted cert -> the verifier
            // accepted a chain it should have rejected. A real security hole.
            t_putstr("tls-smoke: SECURITY FAIL -- untrusted cert ACCEPTED (permissive verifier)\n");
            false
        }
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // 1. net-7c-1: the ClientHello prover. 0x16 = TLS handshake ContentType.
    match tls::client_hello("example.com") {
        Some(hello) if hello.len() > 16 && hello[0] == 0x16 => {
            t_putstr("tls-smoke: ClientHello OK\n");
        }
        _ => {
            t_putstr("tls-smoke: ClientHello FAILED\n");
            unsafe { t_exits(1) }
        }
    }

    // 2. net-7c-2: the deterministic full-handshake + cert-verify + round-trip.
    if !loopback_e2e() {
        unsafe { t_exits(1) }
    }

    // 3. net-7c-2: the verifier rejects an untrusted cert (not permissive).
    if !loopback_rejects_untrusted() {
        unsafe { t_exits(1) }
    }

    unsafe { t_exits(0) }
}
