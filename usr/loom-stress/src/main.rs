// /loom-stress -- the concurrent + cross-Proc-death SMP stress harness for the
// native libthyla_rs::loom ring API (Loom-6d-2). The OWED harness carried since
// #841 across all five Loom closed lists: it drives the kernel's concurrent
// async paths (the #841 elected-reader, the per-ring borrow-guard
// loom_first_inflight_client, the Loom-4b CQ wait-list, and the #898
// quiesce-on-Proc-death) from real userspace threads, under -smp 4/8 via the
// ci-smp-gate multi-boot -- turning what the Loom audits could only reason about
// into something exercised under real SMP scheduling.
//
// It runs POST-pivot (joey spawns it after the FS-mutation probe), so the disk
// 9P FS is live and Loom's payload ops -- which the kernel 9P client drives over
// dev9p -- actually dispatch. Three phases:
//
//   1. positive dev9p round-trip: write + fsync a temp, then READ it back over
//      the ring, byte-correct -- the real async op coverage loom-smoke (pre-pivot)
//      could not reach.
//   2. concurrent two-thread-same-loom_fd FSYNC stress: two sibling threads share
//      ONE ring; each serializes its SQ/CQ access under a spinlock but enters the
//      kernel WITHOUT it, so both are in loom_enter concurrently with async FSYNCs
//      in flight over the shared dev9p client -- the reader-election + CQ-wait-list
//      + borrow-guard witness. Every submitted op completes exactly once.
//   3. cross-Proc-death quiesce: submit K FSYNCs, dispatch them async (NONBLOCK),
//      then leak the ring + exit -- so the K ops are in flight when the Proc tears
//      down, exercising loom_free's #898 abandon-on-death. A clean reap by joey
//      (status 0) + 0 EXTINCTION across the SMP gate is the witness.
//
// joey spawns + reaps + asserts exit 0; the gate's 0-corruption multi-boot is the
// real assertion for the concurrency + death paths.

#![no_std]
#![no_main]

extern crate alloc;

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::loom::{RegisteredBuffer, Ring, Sqe, ENTER_GETEVENTS, ENTER_NONBLOCK};
use libthyla_rs::thread;
use libthyla_rs::{
    t_burrow_attach, t_exits, t_fsync, t_putstr, t_walk_create, t_walk_open, t_write,
    T_ORDWR, T_WALK_OPEN_FROM_ROOT,
};

const THREADS: usize = 2;
const ITERS: u32 = 32; // FSYNCs per thread
const INFLIGHT_AT_DEATH: u32 = 8;
const RING_ENTRIES: u32 = 32; // > 2*max-in-flight so the SQ never fills
const STACK_SZ: u64 = 128 * 1024;

const TMP_NAME: &[u8] = b"loom-stress-tmp";
const PROBE_DATA: &[u8] = b"LOOM-STRESS-6D2\n"; // 16 bytes

// Cross-thread state (statics; the burrow-torture precedent). The ring itself
// cannot be a static, so its address rides SHARED_PTR -- the main thread keeps
// the Share alive (it joins before returning) and the workers deref it.
static TID: [AtomicU32; THREADS] = [AtomicU32::new(0), AtomicU32::new(0)];
static SHARED_PTR: AtomicU64 = AtomicU64::new(0);
static LOCK: AtomicU32 = AtomicU32::new(0); // 0 = free, 1 = held
static SUBMITTED: AtomicU32 = AtomicU32::new(0);
static REAPED: AtomicU32 = AtomicU32::new(0);
static ERRORS: AtomicU32 = AtomicU32::new(0);

struct Share {
    ring: Ring,
}
// SAFETY: the workers serialize every SQ/CQ access (try_submit / reap) under
// LOCK, and Ring::enter touches no Ring memory (it only reads the immutable fd +
// makes the syscall, which the kernel serializes). So the Ring's interior is
// never raced even though it is reached from two threads. The Share outlives the
// workers (the main thread joins before it drops).
unsafe impl Sync for Share {}

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

#[inline]
fn spin_lock() {
    while LOCK
        .compare_exchange(0, 1, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
}

#[inline]
fn spin_unlock() {
    LOCK.store(0, Ordering::Release);
}

// Drain every available CQE under the lock, counting completions + errors.
fn drain_locked(r: &Ring) {
    spin_lock();
    while let Some(c) = r.reap() {
        REAPED.fetch_add(1, Ordering::Relaxed);
        if c.result < 0 {
            ERRORS.fetch_add(1, Ordering::Relaxed);
        }
    }
    spin_unlock();
}

fn share() -> &'static Share {
    unsafe { &*(SHARED_PTR.load(Ordering::Acquire) as *const Share) }
}

// One worker: ITERS rounds of {submit FSYNC under lock; enter WITHOUT lock;
// drain under lock}. The fsync targets registered handle 0 (the writable temp).
extern "C" fn worker(arg: u64) {
    let idx = arg as usize;
    if idx < THREADS {
        let _ = thread::set_tid_address(&TID[idx]);
    }
    let r = &share().ring;
    let tag_base = 0xF000_0000u64 | ((idx as u64) << 16);

    let mut i = 0u32;
    while i < ITERS {
        let sqe = Sqe::fsync(0, tag_base | i as u64);
        // Submit (single-producer SQ under the lock). With RING_ENTRIES >> the
        // in-flight depth this succeeds first try; the retry-with-drain only
        // matters if a peer transiently filled the SQ.
        let mut submitted = false;
        let mut tries = 0;
        while tries < 64 {
            spin_lock();
            let ok = r.try_submit(&sqe).is_ok();
            spin_unlock();
            if ok {
                submitted = true;
                break;
            }
            let _ = r.enter(0, 1, ENTER_GETEVENTS);
            drain_locked(r);
            tries += 1;
        }
        if !submitted {
            ERRORS.fetch_add(1, Ordering::Relaxed);
            break;
        }
        SUBMITTED.fetch_add(1, Ordering::Relaxed);

        // Enter WITHOUT the lock: both workers can be inside loom_enter at once,
        // with async FSYNCs in flight over the shared dev9p client.
        let _ = r.enter(1, 1, ENTER_GETEVENTS);
        drain_locked(r);
        i += 1;
    }
    thread::exit_self();
}

// Open (or create) a read+write temp on the disk FS for the registered fid.
// Idempotent across reboots (the Stratum pool is persistent).
fn open_rw_temp() -> i64 {
    let probe = unsafe {
        t_walk_open(
            T_WALK_OPEN_FROM_ROOT,
            TMP_NAME.as_ptr(),
            TMP_NAME.len(),
            T_ORDWR,
        )
    };
    if probe >= 0 {
        return probe;
    }
    unsafe {
        t_walk_create(
            T_WALK_OPEN_FROM_ROOT,
            TMP_NAME.as_ptr(),
            TMP_NAME.len(),
            T_ORDWR,
            0o644,
        )
    }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("loom-stress: starting (Loom-6d-2 concurrent + cross-Proc-death)\n");

    let rw_fd = open_rw_temp();
    if rw_fd < 0 {
        fail("loom-stress: FAIL -- open/create rw temp\n");
    }
    // Seed known bytes at offset 0 (a fresh ORDWR fd starts at 0) + a durability
    // barrier, so the phase-1 READ has a deterministic prefix to compare.
    if unsafe { t_write(rw_fd, PROBE_DATA.as_ptr(), PROBE_DATA.len()) } != PROBE_DATA.len() as i64 {
        fail("loom-stress: FAIL -- seed write\n");
    }
    if unsafe { t_fsync(rw_fd, 0) } != 0 {
        fail("loom-stress: FAIL -- seed fsync\n");
    }

    let ring = match Ring::setup(RING_ENTRIES, 0) {
        Ok(r) => r,
        Err(_) => fail("loom-stress: FAIL -- Ring::setup\n"),
    };
    if ring.register_handles(&[rw_fd as i32]).is_err() {
        fail("loom-stress: FAIL -- register_handles\n");
    }
    // The registered buffer is eager + contiguous (RegisteredBuffer ->
    // SYS_BURROW_ATTACH); the lazy general heap is non-contiguous and the
    // kernel rejects it for registration. See libthyla_rs::loom::RegisteredBuffer.
    let mut buf = match RegisteredBuffer::new(4096) {
        Ok(b) => b,
        Err(_) => fail("loom-stress: FAIL -- RegisteredBuffer::new\n"),
    };
    if ring.register_buffers(&[buf.buf_reg()]).is_err() {
        fail("loom-stress: FAIL -- register_buffers\n");
    }

    // --- Phase 1: positive dev9p round-trip (READ over the ring, byte-correct).
    let n = PROBE_DATA.len() as u32;
    let rc = match ring.submit_one_wait(&Sqe::read(0, 0, n, 0, 0, 0x1)) {
        Ok(c) => c,
        Err(_) => fail("loom-stress: FAIL -- phase1 READ submit\n"),
    };
    match rc.ok() {
        Ok(got) if got as usize >= PROBE_DATA.len()
            && buf.as_mut_slice()[..PROBE_DATA.len()] == *PROBE_DATA => {}
        _ => fail("loom-stress: FAIL -- phase1 READ bytes wrong\n"),
    }
    // A positive FSYNC on the writable handle too (the op the stress hammers).
    match ring.submit_one_wait(&Sqe::fsync(0, 0x2)) {
        Ok(c) if c.result == 0 => {}
        _ => fail("loom-stress: FAIL -- phase1 FSYNC not 0\n"),
    }
    t_putstr("loom-stress: phase1 dev9p READ/WRITE/FSYNC round-trip ok\n");

    // --- Phase 2: concurrent two-thread-same-loom_fd FSYNC stress.
    let shared = Share { ring };
    SHARED_PTR.store(&shared as *const Share as u64, Ordering::Release);

    let mut j = 0usize;
    while j < THREADS {
        TID[j].store((j as u32) + 1, Ordering::SeqCst); // sentinel BEFORE spawn
        let base = unsafe { t_burrow_attach(STACK_SZ) };
        if base < 0 {
            fail("loom-stress: FAIL -- thread stack attach\n");
        }
        let sp = (base as u64) + STACK_SZ; // 16-aligned top (page base + page mult)
        if unsafe { thread::spawn_raw(worker as *const () as u64, sp, j as u64, 0) }.is_err() {
            fail("loom-stress: FAIL -- spawn_raw\n");
        }
        j += 1;
    }
    let mut k = 0usize;
    while k < THREADS {
        let _ = thread::join_tid(&TID[k], (k as u32) + 1, None);
        k += 1;
    }

    // Drain any stragglers (an op submitted late may still be in flight / posted-
    // but-undrained at join). Single-threaded now, so no lock contention.
    let mut guard = 0u32;
    while REAPED.load(Ordering::Relaxed) < SUBMITTED.load(Ordering::Relaxed) && guard < 100_000 {
        let _ = shared.ring.enter(0, 1, ENTER_GETEVENTS);
        drain_locked(&shared.ring);
        guard += 1;
    }

    let sub = SUBMITTED.load(Ordering::Relaxed);
    let reap = REAPED.load(Ordering::Relaxed);
    let err = ERRORS.load(Ordering::Relaxed);
    if sub != (THREADS as u32) * ITERS || reap != sub || err != 0 {
        t_putstr("loom-stress: FAIL -- concurrent stress accounting (sub/reap/err mismatch)\n");
        unsafe { t_exits(1) }
    }
    t_putstr("loom-stress: phase2 concurrent two-thread FSYNC stress ok (every op completed once)\n");

    // --- Phase 3: cross-Proc-death quiesce. Submit K FSYNCs + dispatch them async
    // (NONBLOCK, no reap), then LEAK the ring (mem::forget) + exit. The K ops are
    // in flight when the Proc tears down; the teardown closes the leaked loom
    // handle -> loom_free quiesces them (#898 abandon-on-death). joey reaping
    // loom-stress status 0 + 0 EXTINCTION across the SMP gate is the witness.
    //
    // BEST-EFFORT coverage (Loom-6d audit F3): nothing *enforces* an op is still
    // outstanding at exit -- it relies on the dev9p FSYNC round-trip (measured
    // ~1.8 ms by loom-bench) being orders of magnitude larger than the few-us
    // exit path, so the K ops are reliably in flight at teardown (a ~1000x
    // margin). The deterministic single-reader multi-in-flight coverage of #898
    // is the 6c kernel harness (9p_transport_mq); this is the cross-PROC-death
    // exercise on a live client.
    let mut s = 0u32;
    while s < INFLIGHT_AT_DEATH {
        if shared.ring.try_submit(&Sqe::fsync(0, 0xDEAD_0000 | s as u64)).is_err() {
            break;
        }
        s += 1;
    }
    let _ = shared.ring.enter(s, 0, ENTER_NONBLOCK); // dispatch async; do NOT reap

    t_putstr("loom-stress: PASS\n");
    // Leak the ring so its Drop (explicit close) does NOT run -- the in-flight
    // ops must be quiesced by the Proc-death teardown path, not a clean close.
    core::mem::forget(shared);
    unsafe { t_exits(0) }
}
