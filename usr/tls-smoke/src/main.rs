// /tls-smoke -- the net-7c-1 runtime TLS feasibility probe. Emits a real
// ClientHello in-guest: exercises the RustCrypto provider, the kernel CSPRNG
// (client random + ECDHE key share), and the TLS record layer on aarch64.
// joey spawns + reaps + asserts exit 0, so a regression gates the boot.
#![no_std]
#![no_main]
extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::{t_exits, t_putstr};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // 0x16 = TLS handshake ContentType; a real ClientHello is well over 16 bytes.
    match tls::client_hello("example.com") {
        Some(hello) if hello.len() > 16 && hello[0] == 0x16 => {
            t_putstr("tls-smoke: ClientHello OK\n");
            unsafe { t_exits(0) }
        }
        _ => {
            t_putstr("tls-smoke: ClientHello FAILED\n");
            unsafe { t_exits(1) }
        }
    }
}
