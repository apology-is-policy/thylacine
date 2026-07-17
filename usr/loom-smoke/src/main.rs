// /loom-smoke -- boot self-test for the native libthyla_rs::loom API (Loom-6d-1).
//
// Runs pre-pivot (joey's root is still devramfs), so it proves what is reachable
// without the disk 9P FS: the ring mechanism end-to-end + the registered
// handle/buffer pin substrate + graceful rejection of a non-9P handle.
//
//   1. NOP round-trip          -- setup + map + SQ produce + ENTER + dispatch +
//                                 CQE post + reap, with no fid (inline-completed).
//   2. register_buffers (heap) -- pin a ThylaAlloc-backed (anon VMA) buffer (the
//                                 I-30 buffer-pin path, observed from userspace).
//   3. register_handles (file) -- snapshot a /system.key fd into the fixed table
//                                 (the I-30 handle-pin path).
//   4. FSYNC on the non-9P handle -> clean error CQE -- Loom payload ops are
//                                 dev9p-only (the kernel 9P client drives them);
//                                 a devramfs Spoor has no client, so the op
//                                 error-completes rather than crashing. Proves
//                                 submit -> dispatch -> error CQE -> reap and the
//                                 safe rejection of a non-dev9p handle.
//
// The POSITIVE dev9p async round-trip (READ / WRITE / FSYNC that succeed) runs
// post-pivot in loom-stress (Loom-6d-2), where the disk 9P FS is live.
//
// joey spawns + reaps + asserts exit 0 + the "loom-smoke: PASS" marker, so any
// failure gates the boot (no "Thylacine boot OK").

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::fs::File;
use libthyla_rs::loom::{RegisteredBuffer, Ring, Sqe};
use libthyla_rs::{t_exits, t_putstr};

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("loom-smoke: starting (Loom-6d-1 native ring API)\n");

    // 1. NOP round-trip -- the whole ring mechanism with no fid.
    let ring = match Ring::setup(8, 0) {
        Ok(r) => r,
        Err(_) => fail("loom-smoke: FAIL -- Ring::setup\n"),
    };
    match ring.submit_one_wait(&Sqe::nop(0xA11CE)) {
        Ok(c) if c.user_data == 0xA11CE && c.result == 0 && !c.more() => {}
        Ok(_) => fail("loom-smoke: FAIL -- NOP CQE mismatch\n"),
        Err(_) => fail("loom-smoke: FAIL -- NOP submit_one_wait\n"),
    }
    t_putstr("loom-smoke: NOP round-trip ok\n");

    // 2. register an eager contiguous buffer (RegisteredBuffer -> SYS_BURROW_ATTACH,
    //    one alloc_pages chunk) -- the I-30 buffer pin. A registered Loom buffer needs
    //    contiguous physical backing, which the lazy general heap (ThylaAlloc ->
    //    SYS_BURROW_ATTACH_LAZY, demand-zero + scattered) does NOT provide; the kernel
    //    rejects it. Held to scope end so the kernel's pin always has a live VMA.
    let buf = match RegisteredBuffer::new(4096) {
        Ok(b) => b,
        Err(_) => fail("loom-smoke: FAIL -- RegisteredBuffer::new\n"),
    };
    if ring.register_buffers(&[buf.buf_reg()]).is_err() {
        fail("loom-smoke: FAIL -- register_buffers\n");
    }

    // 3. register a /system.key fd -- the I-30 handle pin. Hold `file` across the
    //    registration; the ring takes its own Spoor ref, decoupled from this fd.
    let file = match File::open("/system.key") {
        Ok(f) => f,
        Err(_) => fail("loom-smoke: FAIL -- File::open(/system.key)\n"),
    };
    if ring.register_handles(&[file.as_raw_fd()]).is_err() {
        fail("loom-smoke: FAIL -- register_handles\n");
    }
    t_putstr("loom-smoke: registered 1 buffer + 1 handle\n");

    // 4. A payload op on the non-9P (devramfs) handle must error-complete cleanly.
    let fsync_cqe = match ring.submit_one_wait(&Sqe::fsync(0, 0xF5)) {
        Ok(c) => c,
        Err(_) => fail("loom-smoke: FAIL -- FSYNC submit_one_wait\n"),
    };
    if fsync_cqe.user_data != 0xF5 || fsync_cqe.result >= 0 {
        fail("loom-smoke: FAIL -- payload op on non-9P handle was NOT rejected\n");
    }
    t_putstr("loom-smoke: payload op on non-9P handle cleanly rejected\n");

    t_putstr("loom-smoke: PASS\n");
    0
}
