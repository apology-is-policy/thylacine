// /pipe-sink -- U-6d-a multi-element pipeline test fixture (sink side).
//
// Reads fd 0 (stdin) to EOF into a stack buffer and exits with a status
// that encodes what it saw. In a pipeline, fd 0 is the kernel-installed
// pipe read end (the upstream element's stdout end).
//
// Exit status is BINARY at v1.0:
//   0 -- read exactly EXPECT.
//   1 -- anything else (read error / EOF-empty / mismatch / overflow).
//
// The kernel's sys_exits_handler normalizes any non-zero rs_main return
// to exit_status = 1 (x0==0 -> "ok"/0; x0!=0 -> "fail"/1; richer u64
// status is a Phase 5+ deferral per kernel/syscall.c). So a sink that
// returned distinct codes (4/5/6) would still be reaped as 1 -- the
// distinction is invisible to the pipeline's wait. We return 1 for
// every failure to keep the fixture honest about what the waiter sees.
//
// Two pipeline probes consume this:
//   `pipe-src | pipe-sink`  -> 0  (data transfer + correct wiring)
//   `hello-rs | pipe-sink`  -> 1  (hello-rs writes via SYS_PUTS, never
//                                  fd 1, so the pipe carries no bytes;
//                                  sink sees EOF -> returns 1; pipefail
//                                  reports the rightmost non-zero = 1)

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

// Must match pipe-src's PAYLOAD. 13 bytes.
const EXPECT: &[u8] = b"PIPE-DATA-OK\n";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut buf = [0u8; 64];
    let mut total = 0usize;
    loop {
        if total >= buf.len() {
            return 1; // more than EXPECT.len() bytes -- mismatch
        }
        // SAFETY: t_read is the raw SYS_READ SVC wrapper; (ptr+total,
        // len-total) stays within buf.
        let n = unsafe {
            libthyla_rs::t_read(0, buf.as_mut_ptr().add(total), buf.len() - total)
        };
        if n < 0 {
            return 1; // read error
        }
        if n == 0 {
            break; // EOF
        }
        total += n as usize;
    }
    if total != EXPECT.len() {
        return 1; // EOF-empty or wrong length
    }
    let mut i = 0usize;
    while i < total {
        if buf[i] != EXPECT[i] {
            return 1;
        }
        i += 1;
    }
    0
}
