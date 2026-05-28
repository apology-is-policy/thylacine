// /pipe-src -- U-6d-a multi-element pipeline test fixture (source side).
//
// Writes a fixed payload to fd 1 (stdout) then exits 0. In a pipeline
// (`pipe-src | pipe-sink`), libutopia::eval::exec_pipeline installs the
// pipe write end at this process's fd 1, so the payload flows through
// the kernel pipe to the downstream element's fd 0. pipe-sink reads it
// back and validates -- the pair proves the evaluator wires pipe ends
// to the correct slots (write end -> upstream stdout, read end ->
// downstream stdin), i.e. the data direction isn't reversed.
//
// Unlike hello-rs (which writes via SYS_PUTS direct to the UART, never
// touching fd 1), pipe-src writes through the real fd 1 so the pipe
// actually carries bytes.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

// Must match pipe-sink's EXPECT. 13 bytes.
const PAYLOAD: &[u8] = b"PIPE-DATA-OK\n";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let mut off = 0usize;
    while off < PAYLOAD.len() {
        // SAFETY: t_write is the raw SYS_WRITE SVC wrapper; PAYLOAD is a
        // valid static slice and (ptr+off, len-off) stays within it.
        let n = unsafe {
            libthyla_rs::t_write(1, PAYLOAD.as_ptr().add(off), PAYLOAD.len() - off)
        };
        if n <= 0 {
            return 2; // write error / EPIPE -- downstream closed early
        }
        off += n as usize;
    }
    0
}
