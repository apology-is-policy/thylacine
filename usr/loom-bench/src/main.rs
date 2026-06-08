// /loom-bench -- trap-amortization + present-path latency bench for the native
// loom ring API (Loom-6d-3). Demonstrates the Loom value proposition
// (docs/LOOM.md section 5): the current path is one trap per 9P op; the ring
// amortizes the trap (and pipelines the round-trips) so N ops cost ~one enter.
// This is exactly where it hurts today -- a server fanning out across many
// connections, a bulk copy = many read/write pairs, a shell globbing = many
// walks/stats.
//
// Runs POST-pivot (joey spawns it; the disk 9P FS is live so the dev9p ops
// dispatch). Mirrors irq-bench's CNTVCT_EL0 timing (the virtual counter, shared
// with the kernel timebase across TCG/HVF/bare-metal). It reports three numbers:
//
//   1. NOP batch throughput -- N NOPs via ONE enter, per-op cost. NOPs complete
//      inline (no 9P round-trip), so this isolates the RING's per-op overhead:
//      the SQE write + the dispatch. It is tiny, which is the whole point --
//      amortizing N real ops onto one trap is nearly free on the ring side.
//   2. FSYNC: N serial t_fsync syscalls (N traps, serial round-trips) vs N
//      Loom-batched FSYNCs (one enter, the #841 client pipelines the in-flight
//      Tfsyncs) -- per-op each + the speedup. The real "many ops" workload.
//   3. present-proxy -- a single LOOM_OP_WRITE of a small rect descriptor
//      (TAPESTRY.md D3: present = a generic WRITE to a present-fid), the
//      Tapestry present round-trip latency proxy.
//
// INFORMATIONAL: joey gates only on correctness (the Loom batches reap N CQEs,
// all ok); the absolute numbers are noisy under QEMU + host load and are not a
// budget gate (the VISION.md budgets are for real hardware; the full win lands
// with SQPOLL's zero-syscall hot path + a pipelining server).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::loom::{BufReg, Ring, Sqe, ENTER_GETEVENTS};
use libthyla_rs::{
    t_exits, t_fsync, t_putstr, t_puts, t_walk_create, t_walk_open, t_write, T_ORDWR,
    T_WALK_OPEN_FROM_ROOT,
};

const N_NOP: u32 = 64;
const N_FSYNC: u32 = 8;
const ITERS: usize = 3; // batches per measurement (median)
const RING_ENTRIES: u32 = 128; // > N_NOP so a whole NOP batch fits the SQ/CQ
const WARMUP: u32 = 4;

const TMP_NAME: &[u8] = b"loom-bench-tmp";

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

#[inline(always)]
fn read_cntvct() -> u64 {
    let v: u64;
    unsafe {
        core::arch::asm!("isb", "mrs {0}, cntvct_el0", out(reg) v,
                         options(nomem, nostack, preserves_flags));
    }
    v
}

#[inline(always)]
fn read_cntfrq() -> u64 {
    let v: u64;
    unsafe {
        core::arch::asm!("mrs {0}, cntfrq_el0", out(reg) v,
                         options(nomem, nostack, preserves_flags));
    }
    v
}

#[inline]
fn ticks_to_ns(ticks: u64, freq: u64) -> u64 {
    if freq == 0 {
        0
    } else {
        ticks.saturating_mul(1_000_000_000) / freq
    }
}

// Print a u64 decimal (no alloc; a fixed stack buffer).
fn put_u64(v: u64) {
    let mut buf = [0u8; 20];
    let mut i = 20usize;
    let mut n = v;
    if n == 0 {
        i -= 1;
        buf[i] = b'0';
    }
    while n > 0 {
        i -= 1;
        buf[i] = b'0' + (n % 10) as u8;
        n /= 10;
    }
    unsafe {
        let _ = t_puts(buf[i..].as_ptr(), 20 - i);
    }
}

fn median3(mut a: [u64; ITERS]) -> u64 {
    // Tiny insertion sort over ITERS samples; return the middle.
    let n = a.len();
    let mut i = 1;
    while i < n {
        let mut j = i;
        while j > 0 && a[j - 1] > a[j] {
            a.swap(j - 1, j);
            j -= 1;
        }
        i += 1;
    }
    a[n / 2]
}

// Submit `n` FSYNCs on handle 0, drive them, and reap exactly `n` terminal CQEs.
// Returns false if any op errored or the count is wrong.
fn loom_fsync_batch(ring: &Ring, n: u32) -> bool {
    let mut submitted = 0u32;
    while submitted < n {
        if ring.try_submit(&Sqe::fsync(0, submitted as u64)).is_err() {
            // SQ full (shouldn't happen with RING_ENTRIES > n): drain + retry.
            let _ = ring.enter(0, 1, ENTER_GETEVENTS);
            while ring.reap().is_some() {}
            continue;
        }
        submitted += 1;
    }
    let _ = ring.enter(n, n, ENTER_GETEVENTS);
    let mut reaped = 0u32;
    let mut guard = 0u32;
    while reaped < n && guard < 100_000 {
        if let Some(c) = ring.reap() {
            if c.result < 0 {
                return false;
            }
            reaped += 1;
        } else {
            let _ = ring.enter(0, 1, ENTER_GETEVENTS);
            guard += 1;
        }
    }
    reaped == n
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("loom-bench: starting (Loom-6d-3 trap-amortization)\n");
    let freq = read_cntfrq();

    let temp = {
        let p = unsafe { t_walk_open(T_WALK_OPEN_FROM_ROOT, TMP_NAME.as_ptr(), TMP_NAME.len(), T_ORDWR) };
        if p >= 0 {
            p
        } else {
            unsafe { t_walk_create(T_WALK_OPEN_FROM_ROOT, TMP_NAME.as_ptr(), TMP_NAME.len(), T_ORDWR, 0o644) }
        }
    };
    if temp < 0 {
        fail("loom-bench: FAIL -- open/create temp\n");
    }
    let seed = b"loom-bench-rect\n";
    if unsafe { t_write(temp, seed.as_ptr(), seed.len()) } != seed.len() as i64 {
        fail("loom-bench: FAIL -- seed write\n");
    }

    let ring = match Ring::setup(RING_ENTRIES, 0) {
        Ok(r) => r,
        Err(_) => fail("loom-bench: FAIL -- Ring::setup\n"),
    };
    if ring.register_handles(&[temp as i32]).is_err() {
        fail("loom-bench: FAIL -- register_handles\n");
    }
    let mut rect = vec![0u8; 64];
    rect[..seed.len()].copy_from_slice(seed);
    if ring.register_buffers(&[BufReg { va: rect.as_mut_ptr() as u64, len: rect.len() as u64 }]).is_err() {
        fail("loom-bench: FAIL -- register_buffers\n");
    }

    // Warm up the dev9p client + caches.
    for _ in 0..WARMUP {
        if !loom_fsync_batch(&ring, 2) {
            fail("loom-bench: FAIL -- warmup\n");
        }
        unsafe {
            let _ = t_fsync(temp, 0);
        }
    }

    // 1. NOP trap-amortization (I/O-free: NOPs complete inline in the kernel, so
    //    the ONLY difference between the paths is the trap COUNT -- this isolates
    //    the amortization win from any 9P round-trip cost).
    //    Path A: one enter per NOP (N traps).  Path B: one enter for N NOPs.
    let mut nop_indiv = [0u64; ITERS];
    for s in nop_indiv.iter_mut() {
        let t0 = read_cntvct();
        let mut i = 0u32;
        while i < N_NOP {
            let _ = ring.try_submit(&Sqe::nop(i as u64));
            if ring.enter(1, 1, ENTER_GETEVENTS).is_err() {
                fail("loom-bench: FAIL -- NOP indiv enter\n");
            }
            let mut g = 0u32;
            while ring.reap().is_none() {
                // Guard + propagate enter errors so a kernel regression FAILS the
                // bench rather than hanging the boot (the loom_fsync_batch policy).
                if g >= 100_000 || ring.enter(0, 1, ENTER_GETEVENTS).is_err() {
                    fail("loom-bench: FAIL -- NOP indiv reap stuck\n");
                }
                g += 1;
            }
            i += 1;
        }
        *s = read_cntvct().wrapping_sub(t0) / N_NOP as u64;
    }
    let nop_indiv_per_op = ticks_to_ns(median3(nop_indiv), freq);

    let mut nop_batch = [0u64; ITERS];
    for s in nop_batch.iter_mut() {
        let t0 = read_cntvct();
        let mut i = 0u32;
        while i < N_NOP {
            if ring.try_submit(&Sqe::nop(i as u64)).is_err() {
                let _ = ring.enter(0, 1, ENTER_GETEVENTS);
                while ring.reap().is_some() {}
                continue;
            }
            i += 1;
        }
        if ring.enter(N_NOP, N_NOP, ENTER_GETEVENTS).is_err() {
            fail("loom-bench: FAIL -- NOP batch enter\n");
        }
        let mut reaped = 0u32;
        let mut g = 0u32;
        while reaped < N_NOP {
            if ring.reap().is_some() {
                reaped += 1;
            } else if g >= 100_000 || ring.enter(0, 1, ENTER_GETEVENTS).is_err() {
                fail("loom-bench: FAIL -- NOP batch reap stuck\n");
            } else {
                g += 1;
            }
        }
        *s = read_cntvct().wrapping_sub(t0) / N_NOP as u64;
    }
    let nop_batch_per_op = ticks_to_ns(median3(nop_batch), freq);
    let nop_speedup_x10 = if nop_batch_per_op == 0 {
        0
    } else {
        nop_indiv_per_op * 10 / nop_batch_per_op
    };

    // 2a. FSYNC serial (syscall-per-op: N_FSYNC traps + serial round-trips).
    let mut serial_samples = [0u64; ITERS];
    for s in serial_samples.iter_mut() {
        let t0 = read_cntvct();
        let mut i = 0u32;
        while i < N_FSYNC {
            if unsafe { t_fsync(temp, 0) } != 0 {
                fail("loom-bench: FAIL -- serial t_fsync\n");
            }
            i += 1;
        }
        *s = read_cntvct().wrapping_sub(t0) / N_FSYNC as u64;
    }
    let serial_per_op = ticks_to_ns(median3(serial_samples), freq);

    // 2b. FSYNC Loom-batched (one enter; the #841 client pipelines the Tfsyncs).
    let mut batch_samples = [0u64; ITERS];
    for s in batch_samples.iter_mut() {
        let t0 = read_cntvct();
        if !loom_fsync_batch(&ring, N_FSYNC) {
            fail("loom-bench: FAIL -- loom fsync batch\n");
        }
        *s = read_cntvct().wrapping_sub(t0) / N_FSYNC as u64;
    }
    let batch_per_op = ticks_to_ns(median3(batch_samples), freq);

    // 3. present-proxy: one LOOM_OP_WRITE of the rect descriptor (present = WRITE).
    let t0 = read_cntvct();
    let present = match ring.submit_one_wait(&Sqe::write(0, 0, seed.len() as u32, 0, 0, 0x9)) {
        Ok(c) if c.result >= 0 => c,
        _ => fail("loom-bench: FAIL -- present-proxy WRITE\n"),
    };
    let present_ns = ticks_to_ns(read_cntvct().wrapping_sub(t0), freq);
    let _ = present;

    // Report. speedup x10 (one decimal) of serial vs batched per-op.
    let speedup_x10 = if batch_per_op == 0 { 0 } else { serial_per_op * 10 / batch_per_op };

    t_putstr("loom-bench: NOP trap-amortization (n=");
    put_u64(N_NOP as u64);
    t_putstr(") syscall-per-op=");
    put_u64(nop_indiv_per_op);
    t_putstr(" ns, loom-batched=");
    put_u64(nop_batch_per_op);
    t_putstr(" ns (speedup x10=");
    put_u64(nop_speedup_x10);
    t_putstr(")\n");

    t_putstr("loom-bench: FSYNC per-op syscall-per-op=");
    put_u64(serial_per_op);
    t_putstr(" ns, loom-batched=");
    put_u64(batch_per_op);
    t_putstr(" ns (n=");
    put_u64(N_FSYNC as u64);
    t_putstr(", speedup x10=");
    put_u64(speedup_x10);
    t_putstr(")\n");

    t_putstr("loom-bench: present-proxy WRITE round-trip = ");
    put_u64(present_ns);
    t_putstr(" ns\n");

    t_putstr("loom-bench: PASS\n");
    0
}
