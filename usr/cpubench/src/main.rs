// /bin/cpubench -- the TI-4d multi-modal scheduler / CPU bench (the throughput
// + wakeup-latency regression net the TI arc lacked).
//
// WHY THIS EXISTS: the TI-3 tickless idle landing was gated on CORRECTNESS (the
// SMP gate = 0 corruption) and IDLE COST (the HVF re-measure), but NEVER on
// multi-core THROUGHPUT or wake LATENCY -- so a 2.4x boot regression sailed
// through. This bench is the missing gate: it mirrors real workload characters
// with distinct modes and reports the numbers that would have caught it.
//
// THE METHODOLOGY (SOTA-grounded -- LWN "A survey of scheduler benchmarks",
// schbench, will-it-scale, perf bench sched pipe):
//   * TAIL percentiles (p50/p99/p99.9/max), NEVER the mean. A "some wakes slow"
//     bug is a TAIL event the mean hides: a 100ms backstop park is a ~1e5 us
//     p99.9 spike while the p50 looks fine. schbench is purpose-built for this.
//   * Each mode brackets the kernel /ctl/sched work-conservation `wc-tickless`
//     delta, so the bench's own latency tail is correlated with kernel-observed
//     starvation (a core idle while work was queued = the steal/handoff gap).
//
// THE FIVE MODES (the user's five load characters):
//   single   -- single-threaded constant load            -> ops/sec baseline
//   scale    -- N long-running CPU threads (will-it-scale) -> efficiency T1/Tn
//   yield    -- long-running threads that periodically sleep -> sleeps/sec + fairness
//   storm    -- thread spawn/join churn (hackbench-shape)  -> threads/sec
//   pingpong -- 2-thread cross-CPU IPC wakeup (sched pipe) -> p50/p99/p99.9/max us
//
// `cpubench`           -- the short boot/CI probe: all five modes, bounded, with
//                         per-mode wc-delta; prints data, exits 0 (data is the
//                         value -- no flaky perf-threshold gate, per the no-host-
//                         load discipline).
//   `cpubench <mode> [N] [iters]` -- one mode, bigger params, from the shell.
//
// Pure userspace -- a buggy bench corrupts only its own state (the kernel
// validates every op). Composes nothing privileged; adds no invariant.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use core::time::Duration;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::fs::File;
use libthyla_rs::io::Read;
use libthyla_rs::time::{self, Instant};
use libthyla_rs::{t_burrow_attach, t_exits, t_putstr};

// ===========================================================================
// Worker substrate: a fixed pool of kernel Threads (libthyla_rs::thread is the
// raw spawn_raw + set_tid_address join protocol -- no closures in no_std). Each
// worker reads its index from x0 (arg), dispatches on G_KIND, runs, records its
// result + completion, and exits. Stacks are attached once and reused across
// modes (modes run sequentially, joined between, so reuse is race-free).
// ===========================================================================

const MAX_WORKERS: usize = 64;
const STACK_SIZE: u64 = 256 * 1024; // per worker; AAPCS 16-aligned region.
const TID_SENTINEL: u32 = 0xC0FFEE; // non-zero "spawned, not yet exited".

// Worker dispatch kind (set before spawning a batch).
const KIND_CPU: u32 = 0; // run G_INNER_ITERS of cpu_work, G_ROUNDS times.
const KIND_YIELD: u32 = 1; // G_ROUNDS x { cpu_work(G_INNER_ITERS); sleep(G_SLEEP_US) }.
const KIND_PINGPONG: u32 = 2; // the responder half of the ping-pong (B).

static G_KIND: AtomicU32 = AtomicU32::new(KIND_CPU);
static G_INNER_ITERS: AtomicU64 = AtomicU64::new(0);
static G_ROUNDS: AtomicU64 = AtomicU64::new(0);
static G_SLEEP_US: AtomicU64 = AtomicU64::new(0);

// Per-worker join words + outputs. Index = the worker's spawn arg.
static TIDS: [AtomicU32; MAX_WORKERS] = {
    const Z: AtomicU32 = AtomicU32::new(0);
    [Z; MAX_WORKERS]
};
static WORK_NS: [AtomicU64; MAX_WORKERS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; MAX_WORKERS]
}; // per-worker elapsed from the start barrier to done (fairness).
static OPS_DONE: [AtomicU64; MAX_WORKERS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; MAX_WORKERS]
}; // per-worker op count (yield mode: sleeps completed).

// A start barrier so spawned workers begin CONCURRENTLY (else a small unit lets
// worker 0 finish before worker N is even spawned -> a serialized "scale").
static G_START: AtomicU32 = AtomicU32::new(0);

// Black-box sink: XOR every worker's accumulator here so the optimizer cannot
// elide the cpu_work loop (its result must be observably used).
static SINK: AtomicU64 = AtomicU64::new(0);

// Ping-pong shared turn flag (0 = A's turn / B has handed back; 1 = B's turn).
static PP_TURN: AtomicU32 = AtomicU32::new(0);
static PP_STOP: AtomicU32 = AtomicU32::new(0);

/// A fixed-cost integer kernel (xorshift64 mix). #[inline(never)] + the returned
/// accumulator (XOR'd into SINK by the caller) keep the loop from being elided.
#[inline(never)]
fn cpu_work(iters: u64, seed: u64) -> u64 {
    let mut x = seed | 1; // non-zero so xorshift never sticks at 0.
    let mut acc: u64 = 0;
    let mut i = 0u64;
    while i < iters {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        acc = acc.wrapping_add(x);
        i += 1;
    }
    acc
}

/// The worker entry (C ABI; x0 = worker index). Joins the start barrier, then
/// dispatches on G_KIND. The barrier wake is a one-time cost amortized by the
/// per-round work, and it guarantees all workers run concurrently.
extern "C" fn worker_entry(idx: u64) {
    let i = idx as usize;
    let _ = libthyla_rs::thread::set_tid_address(&TIDS[i]);

    // Start barrier: sleep until main releases (G_START == 1).
    while G_START.load(Ordering::Acquire) == 0 {
        let _ = libthyla_rs::torpor::wait(&G_START, 0, Some(Duration::from_secs(5)));
    }

    let t0 = Instant::now();
    match G_KIND.load(Ordering::Acquire) {
        KIND_YIELD => worker_yield(i),
        KIND_PINGPONG => worker_pingpong(),
        _ => worker_cpu(i),
    }
    WORK_NS[i].store(t0.elapsed().as_nanos() as u64, Ordering::SeqCst);

    libthyla_rs::thread::exit_self();
}

/// CPU worker: G_ROUNDS x cpu_work(G_INNER_ITERS). Records the op count
/// (rounds x inner) and folds its accumulator into the black-box sink.
fn worker_cpu(i: usize) {
    let inner = G_INNER_ITERS.load(Ordering::Acquire);
    let rounds = G_ROUNDS.load(Ordering::Acquire);
    let mut acc: u64 = 0;
    let mut r = 0u64;
    while r < rounds {
        acc ^= cpu_work(inner, acc ^ (i as u64).wrapping_mul(0x9e3779b9) ^ r);
        r += 1;
    }
    OPS_DONE[i].store(rounds.wrapping_mul(inner), Ordering::SeqCst);
    SINK.fetch_xor(acc, Ordering::Relaxed);
}

/// Yield worker: G_ROUNDS x { a small cpu_work burst; sleep(G_SLEEP_US) }. The
/// sleep forces a sleep -> reschedule -> tickless-deadline -> wake cycle, so this
/// mode stresses exactly the idle/wake path the TI arc touches. Records sleeps.
fn worker_yield(i: usize) {
    let inner = G_INNER_ITERS.load(Ordering::Acquire);
    let rounds = G_ROUNDS.load(Ordering::Acquire);
    let sleep_us = G_SLEEP_US.load(Ordering::Acquire);
    let dur = Duration::from_micros(sleep_us);
    let mut acc: u64 = 0;
    let mut r = 0u64;
    while r < rounds {
        acc ^= cpu_work(inner, acc ^ (i as u64) ^ r);
        let _ = time::sleep(dur);
        r += 1;
    }
    OPS_DONE[i].store(rounds, Ordering::SeqCst);
    SINK.fetch_xor(acc, Ordering::Relaxed);
}

/// Ping-pong responder (B): wait until it is B's turn (PP_TURN == 1), hand back
/// (PP_TURN = 0 + wake A), repeat until A sets PP_STOP. A (the main thread)
/// times each full round-trip. The torpor wait/wake IS the cross-CPU wakeup the
/// 9P round-trip + boot IPC depend on.
fn worker_pingpong() {
    loop {
        // Sleep while it is NOT B's turn.
        while PP_TURN.load(Ordering::Acquire) != 1 {
            if PP_STOP.load(Ordering::Acquire) != 0 {
                return;
            }
            let _ = libthyla_rs::torpor::wait(&PP_TURN, 0, Some(Duration::from_millis(100)));
        }
        // Hand back to A.
        PP_TURN.store(0, Ordering::Release);
        let _ = libthyla_rs::torpor::wake(&PP_TURN, 1);
    }
}

// ===========================================================================
// Stack pool: attach (lazily) up to MAX_WORKERS user stacks and hand out the
// 16-aligned TOP for each worker. Reused across modes.
// ===========================================================================

struct StackPool {
    bases: [u64; MAX_WORKERS],
    attached: usize,
}

impl StackPool {
    const fn new() -> Self {
        StackPool { bases: [0; MAX_WORKERS], attached: 0 }
    }

    /// Ensure at least `n` stacks are attached. Returns false on OOM.
    fn ensure(&mut self, n: usize) -> bool {
        while self.attached < n {
            let base = unsafe { t_burrow_attach(STACK_SIZE) };
            if base < 0 {
                return false;
            }
            self.bases[self.attached] = base as u64;
            self.attached += 1;
        }
        true
    }

    /// The 16-aligned stack top for worker `i` (must be < self.attached).
    fn top(&self, i: usize) -> u64 {
        (self.bases[i] + STACK_SIZE) & !0xfu64
    }
}

/// Spawn `n` CPU/yield workers (KIND set by the caller), all parked on the start
/// barrier. Returns false if a spawn fails. The caller releases the barrier.
fn spawn_batch(pool: &StackPool, n: usize) -> bool {
    G_START.store(0, Ordering::SeqCst);
    for i in 0..n {
        TIDS[i].store(TID_SENTINEL, Ordering::SeqCst);
        WORK_NS[i].store(0, Ordering::SeqCst);
        OPS_DONE[i].store(0, Ordering::SeqCst);
    }
    for i in 0..n {
        let sp = pool.top(i);
        if unsafe { libthyla_rs::thread::spawn_raw(worker_entry as *const () as u64, sp, i as u64, 0) }
            .is_err()
        {
            return false;
        }
    }
    true
}

/// Release the start barrier (all parked workers wake and begin together).
fn release_batch() {
    G_START.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake_all(&G_START);
}

/// Join `n` workers (bounded). Returns false on a join timeout (a hung worker).
fn join_batch(n: usize) -> bool {
    for i in 0..n {
        if libthyla_rs::thread::join_tid(&TIDS[i], TID_SENTINEL, Some(Duration::from_secs(30)))
            .is_err()
        {
            return false;
        }
    }
    true
}

// ===========================================================================
// /ctl/sched work-conservation reader. Snapshot the kernel `wc-tickless` line
// (parks / starved / starved_ns / max_starved_ns) + `cpus:` so a mode can
// report the kernel-observed starvation DELTA across its run -- the clean
// production-phase measurement (tickless active, no test-phase noise).
// ===========================================================================

#[derive(Clone, Copy, Default)]
struct WcSnap {
    parks: u64,
    starved: u64,
    starved_ns: u64,
    max_ns: u64,
}

/// Parse the unsigned integer immediately following `key` in `s` (e.g. "parks=").
fn field_u64(s: &str, key: &str) -> Option<u64> {
    let pos = s.find(key)?;
    let rest = &s[pos + key.len()..];
    let end = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    if end == 0 {
        return None;
    }
    rest[..end].parse().ok()
}

/// Read /ctl/sched into `buf` and return it as a &str slice (or None if /ctl is
/// not in this namespace -- the bench then degrades to no wc reporting).
fn read_ctl_sched(buf: &mut [u8]) -> Option<usize> {
    let mut f = File::open("/ctl/sched").ok()?;
    let mut total = 0usize;
    loop {
        if total >= buf.len() {
            break;
        }
        match f.read(&mut buf[total..]) {
            Ok(0) => break,
            Ok(k) => total += k,
            Err(_) => return None,
        }
    }
    Some(total)
}

/// Snapshot the tickless work-conservation counters. None if /ctl/sched is
/// unreadable (graceful: the bench still runs, just without the wc delta).
fn wc_snapshot() -> Option<WcSnap> {
    let mut buf = [0u8; 512];
    let n = read_ctl_sched(&mut buf)?;
    let s = core::str::from_utf8(&buf[..n]).ok()?;
    let tl = &s[s.find("wc-tickless:")?..];
    Some(WcSnap {
        parks: field_u64(tl, "parks=").unwrap_or(0),
        starved: field_u64(tl, "starved=").unwrap_or(0),
        starved_ns: field_u64(tl, "starved_ns=").unwrap_or(0),
        max_ns: field_u64(tl, "max_starved_ns=").unwrap_or(0),
    })
}

/// The online CPU count from /ctl/sched `cpus:`; falls back to 1 if unreadable.
fn ncpus() -> usize {
    let mut buf = [0u8; 512];
    if let Some(n) = read_ctl_sched(&mut buf) {
        if let Ok(s) = core::str::from_utf8(&buf[..n]) {
            if let Some(c) = field_u64(s, "cpus: ") {
                if c >= 1 {
                    return (c as usize).min(MAX_WORKERS);
                }
            }
        }
    }
    1
}

/// Print the kernel wc delta across a mode (starved parks + the max single
/// starved park during the run -- a backstop-length max here = the steal gap).
fn report_wc_delta(before: Option<WcSnap>, after: Option<WcSnap>) {
    match (before, after) {
        (Some(b), Some(a)) => {
            let parks = a.parks.saturating_sub(b.parks);
            let starved = a.starved.saturating_sub(b.starved);
            let starved_us = a.starved_ns.saturating_sub(b.starved_ns) / 1000;
            // max_ns is a high-water mark, not a delta -- report the after value
            // (the longest single starved park observed up to now).
            t_putstr(&format!(
                "  kernel wc-delta: tickless parks={} starved={} starved_us={} max_starved_us={}\n",
                parks,
                starved,
                starved_us,
                a.max_ns / 1000
            ));
        }
        _ => {
            t_putstr("  kernel wc-delta: n/a (/ctl/sched unreadable)\n");
        }
    }
}

// ===========================================================================
// Percentiles. TAIL not mean: a starved-park spike lives at p99/p99.9, not the
// average. `sorted` must be ascending.
// ===========================================================================

fn pct(sorted: &[u64], permille: u64) -> u64 {
    if sorted.is_empty() {
        return 0;
    }
    let idx = ((sorted.len() as u64 - 1) * permille / 1000) as usize;
    sorted[idx]
}

fn fmt_us(ns: u64) -> String {
    format!("{}.{:03}", ns / 1000, ns % 1000)
}

// ===========================================================================
// The five modes.
// ===========================================================================

/// single -- single-threaded constant load. Runs cpu_work in a wall-clock budget
/// and reports ops/sec. The no-contention baseline (scale's denominator) and a
/// per-op-overhead regression detector. Runs in THIS thread (no spawn).
fn mode_single(budget: Duration) -> u64 {
    const UNIT: u64 = 200_000; // cpu_work iters per timed chunk.
    let t = Instant::now();
    let mut ops: u64 = 0;
    let mut acc: u64 = 0;
    while t.elapsed() < budget {
        acc ^= cpu_work(UNIT, acc);
        ops += UNIT;
    }
    let ns = t.elapsed().as_nanos() as u64;
    SINK.fetch_xor(acc, Ordering::Relaxed);
    let ops_per_s = if ns > 0 { ops.saturating_mul(1_000_000_000) / ns } else { 0 };
    t_putstr(&format!(
        "single: {} Mops in {} ms -> {} Mops/s (1 thread, no contention)\n",
        ops / 1_000_000,
        ns / 1_000_000,
        ops_per_s / 1_000_000
    ));
    ops_per_s
}

/// scale -- N long-running CPU threads, the will-it-scale efficiency. Time one
/// worker doing W (T1), then N workers EACH doing W concurrently (Tn from the
/// barrier release to the last join). Efficiency E = T1/Tn (permille): perfect
/// parallelism -> 1000; fully serialized -> 1000/N. This is THE scheduler-health
/// number whose absence let the regression slip.
fn mode_scale(pool: &StackPool, n: usize, work: u64) -> u64 {
    G_KIND.store(KIND_CPU, Ordering::SeqCst);
    G_INNER_ITERS.store(work, Ordering::SeqCst);
    G_ROUNDS.store(1, Ordering::SeqCst);

    // T1: a single worker doing W (spawned + barrier-released, so the spawn/wake
    // path is in BOTH measurements -- the ratio cancels it).
    if !spawn_batch(pool, 1) {
        t_putstr("scale: FAIL (spawn T1)\n");
        return 0;
    }
    let t1_start = Instant::now();
    release_batch();
    if !join_batch(1) {
        t_putstr("scale: FAIL (join T1 timeout)\n");
        return 0;
    }
    let t1 = t1_start.elapsed().as_nanos() as u64;

    // Tn: N workers each doing W, concurrently.
    if !spawn_batch(pool, n) {
        t_putstr("scale: FAIL (spawn Tn)\n");
        return 0;
    }
    let tn_start = Instant::now();
    release_batch();
    if !join_batch(n) {
        t_putstr("scale: FAIL (join Tn timeout)\n");
        return 0;
    }
    let tn = tn_start.elapsed().as_nanos() as u64;

    let eff = if tn > 0 { t1 * 1000 / tn } else { 0 };
    // Fairness: the spread of per-worker completion (max/min) -- a fair scheduler
    // finishes all N close together.
    let (mut lo, mut hi) = (u64::MAX, 0u64);
    for i in 0..n {
        let w = WORK_NS[i].load(Ordering::SeqCst);
        lo = lo.min(w);
        hi = hi.max(w);
    }
    t_putstr(&format!(
        "scale: T1={} ms Tn={} ms (n={}) -> efficiency {}.{:01}x of ideal; per-worker {}..{} ms\n",
        t1 / 1_000_000,
        tn / 1_000_000,
        n,
        eff / 1000,
        (eff % 1000) / 100,
        lo / 1_000_000,
        hi / 1_000_000
    ));
    eff
}

/// yield -- N long-running threads that periodically sleep (the sleep forces a
/// reschedule -> tickless deadline -> wake). Reports aggregate sleeps/sec +
/// fairness. Stresses exactly the idle/wake path the TI arc touches.
fn mode_yield(pool: &StackPool, n: usize, rounds: u64, sleep_us: u64) {
    G_KIND.store(KIND_YIELD, Ordering::SeqCst);
    G_INNER_ITERS.store(20_000, Ordering::SeqCst); // a small burst between sleeps.
    G_ROUNDS.store(rounds, Ordering::SeqCst);
    G_SLEEP_US.store(sleep_us, Ordering::SeqCst);

    if !spawn_batch(pool, n) {
        t_putstr("yield: FAIL (spawn)\n");
        return;
    }
    let t = Instant::now();
    release_batch();
    if !join_batch(n) {
        t_putstr("yield: FAIL (join timeout)\n");
        return;
    }
    let ns = t.elapsed().as_nanos() as u64;

    let mut total_sleeps: u64 = 0;
    let (mut lo, mut hi) = (u64::MAX, 0u64);
    for i in 0..n {
        total_sleeps += OPS_DONE[i].load(Ordering::SeqCst);
        let w = WORK_NS[i].load(Ordering::SeqCst);
        lo = lo.min(w);
        hi = hi.max(w);
    }
    let sleeps_per_s = if ns > 0 { total_sleeps.saturating_mul(1_000_000_000) / ns } else { 0 };
    // Fairness: 1000 = every thread finished at the same instant.
    let fairness = if hi > 0 { lo * 1000 / hi } else { 0 };
    t_putstr(&format!(
        "yield: {} sleeps over {} threads in {} ms -> {} sleeps/s; fairness {}.{:01} (1.0=ideal, sleep={}us)\n",
        total_sleeps,
        n,
        ns / 1_000_000,
        sleeps_per_s,
        fairness / 1000,
        (fairness % 1000) / 100,
        sleep_us
    ));
}

/// storm -- thread spawn/join churn (hackbench-shape). R rounds of {spawn N
/// trivial workers, release, join all}. Reports threads/sec -- the boot's
/// spawn-heavy character that exposed the regression.
fn mode_storm(pool: &StackPool, n: usize, rounds: u64) {
    G_KIND.store(KIND_CPU, Ordering::SeqCst);
    G_INNER_ITERS.store(2_000, Ordering::SeqCst); // trivial work; the cost is spawn/join.
    G_ROUNDS.store(1, Ordering::SeqCst);

    let t = Instant::now();
    let mut spawned: u64 = 0;
    let mut r = 0u64;
    while r < rounds {
        if !spawn_batch(pool, n) {
            t_putstr("storm: FAIL (spawn)\n");
            return;
        }
        release_batch();
        if !join_batch(n) {
            t_putstr("storm: FAIL (join timeout)\n");
            return;
        }
        spawned += n as u64;
        r += 1;
    }
    let ns = t.elapsed().as_nanos() as u64;
    let per_thread_us = if spawned > 0 { (ns / 1000) / spawned } else { 0 };
    let threads_per_s = if ns > 0 { spawned.saturating_mul(1_000_000_000) / ns } else { 0 };
    t_putstr(&format!(
        "storm: {} thread spawn+join in {} ms -> {} threads/s ({} us/thread, n={} x {} rounds)\n",
        spawned,
        ns / 1_000_000,
        threads_per_s,
        per_thread_us,
        n,
        rounds
    ));
}

/// pingpong -- 2-thread cross-CPU IPC wakeup latency (perf bench sched pipe +
/// schbench tail). THE regression detector: a starved/backstop wake shows as a
/// p99.9 / max spike while p50 stays flat. Reports the full tail histogram + the
/// kernel wc delta. Maps directly onto the 9P round-trip + boot IPC pattern.
fn mode_pingpong(pool: &StackPool, iters: u64) {
    G_KIND.store(KIND_PINGPONG, Ordering::SeqCst);
    PP_TURN.store(0, Ordering::SeqCst);
    PP_STOP.store(0, Ordering::SeqCst);

    // Spawn the responder (B) on worker slot 0, released immediately (no
    // barrier needed -- it parks on PP_TURN).
    TIDS[0].store(TID_SENTINEL, Ordering::SeqCst);
    G_START.store(1, Ordering::SeqCst); // B skips the barrier.
    if unsafe {
        libthyla_rs::thread::spawn_raw(worker_entry as *const () as u64, pool.top(0), 0, 0)
    }
    .is_err()
    {
        t_putstr("pingpong: FAIL (spawn responder)\n");
        return;
    }

    let mut samples: Vec<u64> = Vec::with_capacity(iters as usize);
    let wc_before = wc_snapshot();
    for _ in 0..iters {
        let t = Instant::now();
        // Hand to B and wake it.
        PP_TURN.store(1, Ordering::Release);
        let _ = libthyla_rs::torpor::wake(&PP_TURN, 1);
        // Sleep until B hands back (PP_TURN == 0). The kernel sleeps THIS thread
        // and B's wake re-dispatches it -- the cross-CPU round-trip we measure.
        let mut spins = 0u32;
        while PP_TURN.load(Ordering::Acquire) != 0 {
            let _ = libthyla_rs::torpor::wait(&PP_TURN, 1, Some(Duration::from_millis(100)));
            spins += 1;
            if spins > 50 {
                break; // ~5s: B is wedged; bail rather than hang.
            }
        }
        samples.push(t.elapsed().as_nanos() as u64);
    }
    let wc_after = wc_snapshot();

    // Stop B + join.
    PP_STOP.store(1, Ordering::Release);
    PP_TURN.store(1, Ordering::Release); // nudge B out of its wait.
    let _ = libthyla_rs::torpor::wake(&PP_TURN, 1);
    let _ = libthyla_rs::thread::join_tid(&TIDS[0], TID_SENTINEL, Some(Duration::from_secs(10)));

    samples.sort_unstable();
    let mut sum: u64 = 0;
    for &s in &samples {
        sum += s;
    }
    let mean = if samples.is_empty() { 0 } else { sum / samples.len() as u64 };
    t_putstr(&format!(
        "pingpong: {} round-trips; mean {} p50 {} p99 {} p99.9 {} max {} us (cross-CPU futex wake)\n",
        samples.len(),
        fmt_us(mean),
        fmt_us(pct(&samples, 500)),
        fmt_us(pct(&samples, 990)),
        fmt_us(pct(&samples, 999)),
        fmt_us(samples.last().copied().unwrap_or(0))
    ));
    report_wc_delta(wc_before, wc_after);
}

// ===========================================================================
// Entry.
// ===========================================================================

fn fail(why: &str) -> ! {
    t_putstr(&format!("cpubench: FAIL -- {}\n", why));
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();
    let mode = args.get_str(1);
    let cpus = ncpus();

    let mut pool = StackPool::new();
    if !pool.ensure(cpus.max(2)) {
        fail("stack pool attach (OOM)");
    }

    // arg2 = worker count override (default ncpus); arg3 = iters/rounds override.
    let n = args
        .get_str(2)
        .and_then(|s| s.parse::<usize>().ok())
        .map(|v| v.clamp(1, MAX_WORKERS))
        .unwrap_or(cpus);
    let p3 = args.get_str(3).and_then(|s| s.parse::<u64>().ok());

    match mode {
        Some("single") => {
            let budget = p3.unwrap_or(200);
            mode_single(Duration::from_millis(budget));
        }
        Some("scale") => {
            // work sized so a single worker runs ~tens of ms (spawn noise small).
            let work = p3.unwrap_or(40_000_000);
            if !pool.ensure(n) {
                fail("stack pool (scale)");
            }
            mode_scale(&pool, n, work);
        }
        Some("yield") => {
            let rounds = p3.unwrap_or(200);
            if !pool.ensure(n) {
                fail("stack pool (yield)");
            }
            mode_yield(&pool, n, rounds, 500);
        }
        Some("storm") => {
            let rounds = p3.unwrap_or(20);
            if !pool.ensure(n) {
                fail("stack pool (storm)");
            }
            mode_storm(&pool, n, rounds);
        }
        Some("pingpong") => {
            let iters = p3.unwrap_or(5_000);
            mode_pingpong(&pool, iters);
        }
        _ => {
            // Default: the short all-modes boot/CI probe. Bounded so it runs in
            // ~1-2s and never hangs; prints data (greppable), exits 0.
            t_putstr(&format!(
                "cpubench: TI-4d scheduler probe (cpus={}) -- single/scale/yield/storm/pingpong\n",
                cpus
            ));
            let wc0 = wc_snapshot();
            mode_single(Duration::from_millis(120));
            // scale work sized so a single worker runs ~a few hundred ms (well
            // above spawn noise, but the probe stays ~1s total).
            mode_scale(&pool, cpus, 20_000_000);
            mode_yield(&pool, cpus, 40, 500);
            mode_storm(&pool, cpus, 8);
            // 3000 round-trips so p99.9 has >= 1000 samples behind it.
            mode_pingpong(&pool, 3_000);
            let wc1 = wc_snapshot();
            t_putstr("cpubench: full-run kernel work-conservation:\n");
            report_wc_delta(wc0, wc1);
            t_putstr("cpubench: PROBE OK\n");
        }
    }
    0
}
