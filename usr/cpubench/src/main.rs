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
// THE MODES (each a distinct scheduler axis mapping to a real Thylacine workload):
//   single    -- single-threaded constant load             -> ops/sec baseline
//   scale     -- N long CPU threads (will-it-scale)         -> efficiency T1/Tn
//   yield     -- long threads that periodically sleep        -> sleeps/sec + fairness
//   storm     -- thread spawn/join churn (hackbench-shape)  -> threads/sec
//   pingpong  -- 2-thread cross-CPU IPC wakeup (sched pipe) -> p50/p99/p99.9/max us
//   latency   -- schbench: 1 probe + N CPU hogs            -> probe wake-overshoot tail
//                THE work-conservation detector (tail-under-load; the wait-bound
//                boot's interactive-responsiveness analog).
//   wakestorm -- K short sleepers, no compute              -> wakes/sec + overshoot tail
//                the boot pattern (netd poll / Loom / the bring-up wait chain).
//   burst     -- imbalanced: hot subset + idle peers, then  -> cold vs loaded drain ratio
//                inject a burst; how fast idle peers absorb (the ILB detector).
//   pipeline  -- N-stage cross-CPU handoff chain           -> tokens/sec + end-to-end tail
//                the 9P request->dispatch->reply IPC shape.
//   contention-- N threads on one torpor lock              -> acquisitions/sec + fairness
//   idle      -- a quiet window, parks/sec via /ctl/sched  -> the #299 idle-cost axis
//                (high = tickful re-poll; low = tickless deep-park).
//   mixed     -- FORWARD SEAM (priority): INTERACTIVE probe + IDLE hogs. Today the
//                priorities are not enforced (no SYS_SCHED_SETATTR) -> the
//                uniform-priority BASELINE the future priority system must beat.
//   affinity  -- FORWARD SEAM (affinity): pinned cross-CPU ping-pong. Today pinning
//                is not enforced -> the natural-placement baseline.
//
// PRIORITY / AFFINITY PLUG POINT (user-directed 2026-06-22 -- "build with that in
// mind, easily pluggable later"): per-worker PRIO/AFFINITY are applied through ONE
// hook (`worker_apply_attr`) over `libthyla_rs::sched`, which returns
// NotImplemented today. When a `SYS_SCHED_SETATTR`-class syscall lands, only that
// lib stub changes and the mixed/affinity modes measure for real -- the scaffold,
// the call sites, and the consumers already exist.
//
// `cpubench`           -- the short boot/CI probe: the 5 core modes + idle,
//                         bounded; prints data, exits 0 (data is the value -- no
//                         flaky perf-threshold gate, per the no-host-load discipline).
//   `cpubench all`      -- the comprehensive deep run: EVERY mode at solid params,
//                         delimited block (the redesign's compass + baseline capture).
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
const KIND_HOG: u32 = 3; // cpu_work bursts until G_STOP -- background saturating load.
const KIND_WAKESTORM: u32 = 4; // G_ROUNDS x sleep(G_SLEEP_US), record wake overshoot.
const KIND_PIPELINE: u32 = 5; // stage worker: wait my turn, hand to next, until G_STOP.
const KIND_CONTEND: u32 = 6; // G_ROUNDS x { lock G_MUTEX; tiny; unlock }, count acquisitions.

static G_KIND: AtomicU32 = AtomicU32::new(KIND_CPU);
static G_INNER_ITERS: AtomicU64 = AtomicU64::new(0);
static G_ROUNDS: AtomicU64 = AtomicU64::new(0);
static G_SLEEP_US: AtomicU64 = AtomicU64::new(0);

// Run-until-stop flag for hog/pipeline workers (0 = run, 1 = stop + exit).
static G_STOP: AtomicU32 = AtomicU32::new(0);

// Pipeline stage handoff: STAGE[s] holds the token id currently AT stage s (0 =
// empty). Stage worker s waits STAGE[s] != 0, takes it, stores into STAGE[s+1],
// wakes the next. The injector (main) writes STAGE[0] and reads STAGE[G_STAGES].
const MAX_STAGES: usize = 16;
static STAGE: [AtomicU32; MAX_STAGES + 1] = {
    const Z: AtomicU32 = AtomicU32::new(0);
    [Z; MAX_STAGES + 1]
};
static G_STAGES: AtomicU32 = AtomicU32::new(0);

// Contention mutex: 0 = free, 1 = held (torpor-backed CAS lock).
static G_MUTEX: AtomicU32 = AtomicU32::new(0);

// Per-worker wake-latency accumulators (wakestorm: overshoot = actual - intended).
static WAKE_SUM_NS: [AtomicU64; MAX_WORKERS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; MAX_WORKERS]
};
static WAKE_MAX_NS: [AtomicU64; MAX_WORKERS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; MAX_WORKERS]
};

// ---------------------------------------------------------------------------
// PRIORITY / AFFINITY FORWARD SEAM (user-directed: build pluggable). Per-worker
// scheduling attrs, applied through ONE hook over libthyla_rs::sched. The lib
// returns NotImplemented today (no SYS_SCHED_SETATTR), so this is a no-op now;
// when the syscall lands, only the lib stub changes and mixed/affinity measure
// for real. PRIO_UNSET / AFFINITY 0 = "leave default" (the common case).
// ---------------------------------------------------------------------------
const PRIO_UNSET: i32 = i32::MIN;
static PRIO: [AtomicU32; MAX_WORKERS] = {
    const Z: AtomicU32 = AtomicU32::new(PRIO_UNSET as u32);
    [Z; MAX_WORKERS]
};
static AFFINITY: [AtomicU64; MAX_WORKERS] = {
    const Z: AtomicU64 = AtomicU64::new(0);
    [Z; MAX_WORKERS]
};

/// The single pluggable point: apply worker `i`'s scheduling attrs to the
/// calling thread. Best-effort -- today `libthyla_rs::sched` returns
/// NotImplemented, so an unset/default attr is a no-op and a set attr fails
/// silently. When `SYS_SCHED_SETATTR` lands this takes effect with no change here.
fn worker_apply_attr(i: usize) {
    let p = PRIO[i].load(Ordering::Acquire) as i32;
    if p != PRIO_UNSET {
        let _ = libthyla_rs::sched::set_self_priority(p);
    }
    let a = AFFINITY[i].load(Ordering::Acquire);
    if a != 0 {
        let _ = libthyla_rs::sched::set_self_affinity(a);
    }
}

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
    worker_apply_attr(i); // priority/affinity forward seam (no-op today).

    // Start barrier: sleep until main releases (G_START == 1).
    while G_START.load(Ordering::Acquire) == 0 {
        let _ = libthyla_rs::torpor::wait(&G_START, 0, Some(Duration::from_secs(5)));
    }

    let t0 = Instant::now();
    match G_KIND.load(Ordering::Acquire) {
        KIND_YIELD => worker_yield(i),
        KIND_PINGPONG => worker_pingpong(),
        KIND_HOG => worker_hog(i),
        KIND_WAKESTORM => worker_wakestorm(i),
        KIND_PIPELINE => worker_pipeline(i),
        KIND_CONTEND => worker_contend(i),
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

/// Hog worker: cpu_work bursts until G_STOP. The saturating background load for
/// `latency` (a probe's wake-overshoot under load) and `burst` (the warm subset).
fn worker_hog(i: usize) {
    let inner = G_INNER_ITERS.load(Ordering::Acquire);
    let mut acc: u64 = 0;
    let mut r: u64 = 0;
    while G_STOP.load(Ordering::Acquire) == 0 {
        acc ^= cpu_work(inner, acc ^ (i as u64).wrapping_mul(0x9e3779b9) ^ r);
        r += 1;
    }
    OPS_DONE[i].store(r, Ordering::SeqCst);
    SINK.fetch_xor(acc, Ordering::Relaxed);
}

/// Wakestorm worker: G_ROUNDS short sleeps, recording the wake OVERSHOOT (actual
/// elapsed - intended) into per-worker sum/max. Many of these concurrently are
/// the boot's many-short-sleepers pattern; the overshoot tail is the wake latency.
fn worker_wakestorm(i: usize) {
    let rounds = G_ROUNDS.load(Ordering::Acquire);
    let sleep_us = G_SLEEP_US.load(Ordering::Acquire);
    let dur = Duration::from_micros(sleep_us);
    let target = sleep_us.wrapping_mul(1000); // intended sleep, ns.
    let mut sum: u64 = 0;
    let mut maxv: u64 = 0;
    let mut count: u64 = 0;
    let mut r: u64 = 0;
    while r < rounds {
        let t = Instant::now();
        let _ = time::sleep(dur);
        let over = (t.elapsed().as_nanos() as u64).saturating_sub(target);
        sum = sum.wrapping_add(over);
        if over > maxv {
            maxv = over;
        }
        count += 1;
        r += 1;
    }
    OPS_DONE[i].store(count, Ordering::SeqCst);
    WAKE_SUM_NS[i].store(sum, Ordering::SeqCst);
    WAKE_MAX_NS[i].store(maxv, Ordering::SeqCst);
}

/// Pipeline stage worker (stage index = spawn arg `i`). Waits its turn on
/// STAGE[i], does a tiny fixed unit, hands the token to STAGE[i+1] and wakes the
/// next stage. Loops until G_STOP. The N-stage cross-CPU handoff = the 9P
/// request->dispatch->reply chain + the boot's IPC shape.
fn worker_pipeline(i: usize) {
    loop {
        while STAGE[i].load(Ordering::Acquire) == 0 {
            if G_STOP.load(Ordering::Acquire) != 0 {
                return;
            }
            let _ = libthyla_rs::torpor::wait(&STAGE[i], 0, Some(Duration::from_millis(100)));
        }
        let tok = STAGE[i].swap(0, Ordering::AcqRel); // take the token.
        SINK.fetch_xor(cpu_work(64, tok as u64), Ordering::Relaxed); // a real (tiny) stage.
        STAGE[i + 1].store(tok, Ordering::Release);
        let _ = libthyla_rs::torpor::wake(&STAGE[i + 1], 1);
    }
}

/// Contention worker: G_ROUNDS x { acquire G_MUTEX (torpor-backed CAS lock); tiny
/// critical section; release + wake one waiter }. Records acquisitions. Stresses
/// the lock/wait-wake path (the allocator, shared server state).
fn worker_contend(i: usize) {
    let rounds = G_ROUNDS.load(Ordering::Acquire);
    let mut got: u64 = 0;
    let mut r: u64 = 0;
    while r < rounds {
        // Acquire.
        while G_MUTEX
            .compare_exchange(0, 1, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            let _ = libthyla_rs::torpor::wait(&G_MUTEX, 1, Some(Duration::from_millis(100)));
        }
        SINK.fetch_xor(cpu_work(32, r ^ (i as u64)), Ordering::Relaxed);
        // Release + wake exactly one waiter.
        G_MUTEX.store(0, Ordering::Release);
        let _ = libthyla_rs::torpor::wake_one(&G_MUTEX);
        got += 1;
        r += 1;
    }
    OPS_DONE[i].store(got, Ordering::SeqCst);
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
    clear_attrs(n); // the simple modes run at default priority/affinity.
    for i in 0..n {
        reset_slot(i);
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

/// Reset worker `i`'s output + join slots (NOT its attrs -- the caller manages
/// PRIO/AFFINITY for an attr-bearing spawn).
fn reset_slot(i: usize) {
    TIDS[i].store(TID_SENTINEL, Ordering::SeqCst);
    WORK_NS[i].store(0, Ordering::SeqCst);
    OPS_DONE[i].store(0, Ordering::SeqCst);
    WAKE_SUM_NS[i].store(0, Ordering::SeqCst);
    WAKE_MAX_NS[i].store(0, Ordering::SeqCst);
}

/// Clear scheduling attrs for slots [0, n) to default (unset). The attr-bearing
/// modes call this then set specific slots before spawning.
fn clear_attrs(n: usize) {
    for i in 0..n.min(MAX_WORKERS) {
        PRIO[i].store(PRIO_UNSET as u32, Ordering::SeqCst);
        AFFINITY[i].store(0, Ordering::SeqCst);
    }
}

/// Spawn a single worker into `slot` with spawn-arg `arg` (the worker reads it as
/// its index). Resets the output slots; leaves attrs as the caller set them. Used
/// where workers occupy disjoint slot ranges (burst, pipeline, mixed, affinity).
fn spawn_at(pool: &StackPool, slot: usize, arg: u64) -> bool {
    reset_slot(slot);
    unsafe {
        libthyla_rs::thread::spawn_raw(worker_entry as *const () as u64, pool.top(slot), arg, 0)
    }
    .is_ok()
}

/// Join workers in slot range [lo, hi). Returns false on any join timeout.
fn join_range(lo: usize, hi: usize) -> bool {
    for i in lo..hi {
        if libthyla_rs::thread::join_tid(&TIDS[i], TID_SENTINEL, Some(Duration::from_secs(30)))
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

/// All-parks counter (the `wc:` line -- BOTH tickful + tickless parks). The idle
/// mode's idle-cost proxy: parks/sec over a quiet window is the wake/vmexit rate
/// (a tickful idle CPU re-parks ~1000x/s on the periodic tick; tickless ~10).
fn wc_all_parks() -> Option<u64> {
    let mut buf = [0u8; 512];
    let n = read_ctl_sched(&mut buf)?;
    let s = core::str::from_utf8(&buf[..n]).ok()?;
    let tl = &s[s.find("wc: ")?..];
    field_u64(tl, "parks=")
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

/// latency -- schbench: N CPU hogs saturate the machine, ONE probe (this thread)
/// sleeps then measures its wake OVERSHOOT (actual - intended). The probe's tail
/// under saturation is THE work-conservation detector (a starved wake is a
/// p99.9/max spike). Maps onto interactive responsiveness while the box is busy.
fn mode_latency(pool: &StackPool, hogs: usize, probe_iters: u64, probe_us: u64) {
    G_KIND.store(KIND_HOG, Ordering::SeqCst);
    G_INNER_ITERS.store(50_000, Ordering::SeqCst);
    G_STOP.store(0, Ordering::SeqCst);
    if !spawn_batch(pool, hogs) {
        t_putstr("latency: FAIL (spawn hogs)\n");
        return;
    }
    release_batch();
    let _ = time::sleep(Duration::from_millis(20)); // ramp to saturation.
    let wc_before = wc_snapshot();
    let mut samples: Vec<u64> = Vec::with_capacity(probe_iters as usize);
    let target = probe_us.wrapping_mul(1000);
    for _ in 0..probe_iters {
        let t = Instant::now();
        let _ = time::sleep(Duration::from_micros(probe_us));
        samples.push((t.elapsed().as_nanos() as u64).saturating_sub(target));
    }
    let wc_after = wc_snapshot();
    G_STOP.store(1, Ordering::SeqCst);
    if !join_batch(hogs) {
        t_putstr("latency: WARN (hog join timeout)\n");
    }
    samples.sort_unstable();
    t_putstr(&format!(
        "latency: {} probes vs {} hogs (sleep {}us) -> wake-overshoot p50 {} p99 {} p99.9 {} max {} us\n",
        samples.len(),
        hogs,
        probe_us,
        fmt_us(pct(&samples, 500)),
        fmt_us(pct(&samples, 990)),
        fmt_us(pct(&samples, 999)),
        fmt_us(samples.last().copied().unwrap_or(0)),
    ));
    report_wc_delta(wc_before, wc_after);
}

/// wakestorm -- K short sleepers (no compute), the boot's many-short-sleepers
/// pattern. Aggregate wakes/sec + the per-wake overshoot mean/max. The wait-bound
/// boot, netd's poll loop, and Loom are this shape; the mode that would have
/// caught the TI-4 regression at once.
fn mode_wakestorm(pool: &StackPool, n: usize, rounds: u64, sleep_us: u64) {
    G_KIND.store(KIND_WAKESTORM, Ordering::SeqCst);
    G_ROUNDS.store(rounds, Ordering::SeqCst);
    G_SLEEP_US.store(sleep_us, Ordering::SeqCst);
    if !spawn_batch(pool, n) {
        t_putstr("wakestorm: FAIL (spawn)\n");
        return;
    }
    let wc_before = wc_snapshot();
    let t = Instant::now();
    release_batch();
    if !join_batch(n) {
        t_putstr("wakestorm: FAIL (join timeout)\n");
        return;
    }
    let ns = t.elapsed().as_nanos() as u64;
    let wc_after = wc_snapshot();
    let mut total: u64 = 0;
    let mut sum: u64 = 0;
    let mut maxv: u64 = 0;
    for i in 0..n {
        total += OPS_DONE[i].load(Ordering::SeqCst);
        sum = sum.wrapping_add(WAKE_SUM_NS[i].load(Ordering::SeqCst));
        maxv = maxv.max(WAKE_MAX_NS[i].load(Ordering::SeqCst));
    }
    let wps = if ns > 0 { total.saturating_mul(1_000_000_000) / ns } else { 0 };
    let mean = if total > 0 { sum / total } else { 0 };
    t_putstr(&format!(
        "wakestorm: {} wakes over {} threads in {} ms -> {} wakes/s; overshoot mean {} max {} us (sleep {}us)\n",
        total,
        n,
        ns / 1_000_000,
        wps,
        fmt_us(mean),
        fmt_us(maxv),
        sleep_us,
    ));
    report_wc_delta(wc_before, wc_after);
}

/// burst -- imbalanced-load absorption (the ILB detector). Drain a burst of
/// workers on an idle machine (cold), then drain the SAME burst while a hog
/// subset keeps some CPUs busy (loaded). The loaded/cold ratio is how well idle
/// peers absorb queued work -- a work-conserving scheduler keeps it near 1.0.
fn mode_burst(pool: &StackPool, burst: usize, hogs: usize) {
    clear_attrs(MAX_WORKERS);
    let work: u64 = 800_000; // each burst worker's unit (a few ms).

    // Phase 1: cold drain on an idle machine.
    G_KIND.store(KIND_CPU, Ordering::SeqCst);
    G_INNER_ITERS.store(work, Ordering::SeqCst);
    G_ROUNDS.store(1, Ordering::SeqCst);
    if !spawn_batch(pool, burst) {
        t_putstr("burst: FAIL (cold spawn)\n");
        return;
    }
    let t = Instant::now();
    release_batch();
    if !join_batch(burst) {
        t_putstr("burst: FAIL (cold join)\n");
        return;
    }
    let cold = t.elapsed().as_nanos() as u64;

    // Phase 2: warm a hog subset (slots [0, hogs)), then drain the burst (slots
    // [hogs, hogs+burst)) while the peers are busy.
    G_KIND.store(KIND_HOG, Ordering::SeqCst);
    G_INNER_ITERS.store(50_000, Ordering::SeqCst);
    G_STOP.store(0, Ordering::SeqCst);
    G_START.store(0, Ordering::SeqCst);
    for s in 0..hogs {
        if !spawn_at(pool, s, s as u64) {
            t_putstr("burst: FAIL (hog spawn)\n");
            G_STOP.store(1, Ordering::SeqCst);
            return;
        }
    }
    G_START.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake_all(&G_START);
    let _ = time::sleep(Duration::from_millis(20)); // hogs ramp.
    G_KIND.store(KIND_CPU, Ordering::SeqCst); // burst workers (spawned next) are CPU.
    G_INNER_ITERS.store(work, Ordering::SeqCst);
    let wc_before = wc_snapshot();
    let t2 = Instant::now();
    for s in hogs..hogs + burst {
        if !spawn_at(pool, s, s as u64) {
            t_putstr("burst: FAIL (loaded burst spawn)\n");
            G_STOP.store(1, Ordering::SeqCst);
            return;
        }
    }
    if !join_range(hogs, hogs + burst) {
        t_putstr("burst: FAIL (loaded join)\n");
        G_STOP.store(1, Ordering::SeqCst);
        return;
    }
    let loaded = t2.elapsed().as_nanos() as u64;
    let wc_after = wc_snapshot();
    G_STOP.store(1, Ordering::SeqCst);
    let _ = join_range(0, hogs);
    let ratio = if cold > 0 { loaded.saturating_mul(1000) / cold } else { 0 };
    t_putstr(&format!(
        "burst: drain {} workers -- cold {} ms, loaded ({} hogs) {} ms -> penalty {}.{:02}x (1.0 = idle peers absorbed instantly)\n",
        burst,
        cold / 1_000_000,
        hogs,
        loaded / 1_000_000,
        ratio / 1000,
        (ratio % 1000) / 10,
    ));
    report_wc_delta(wc_before, wc_after);
}

/// pipeline -- an N-stage cross-CPU handoff chain (the 9P request->dispatch->
/// reply IPC shape + the boot's producer-consumer pattern). Inject tokens through
/// all N stages, time the end-to-end latency tail + tokens/sec.
fn mode_pipeline(pool: &StackPool, stages_req: usize, tokens: u64) {
    clear_attrs(MAX_WORKERS);
    let stages = stages_req.clamp(1, MAX_STAGES);
    G_KIND.store(KIND_PIPELINE, Ordering::SeqCst);
    G_STAGES.store(stages as u32, Ordering::SeqCst);
    G_STOP.store(0, Ordering::SeqCst);
    for s in 0..=stages {
        STAGE[s].store(0, Ordering::SeqCst);
    }
    G_START.store(1, Ordering::SeqCst); // stage workers skip the barrier; they park on STAGE[i].
    for s in 0..stages {
        if !spawn_at(pool, s, s as u64) {
            t_putstr("pipeline: FAIL (spawn stage)\n");
            G_STOP.store(1, Ordering::SeqCst);
            for k in 0..stages {
                let _ = libthyla_rs::torpor::wake(&STAGE[k], 1);
            }
            return;
        }
    }
    let mut samples: Vec<u64> = Vec::with_capacity(tokens as usize);
    let wc_before = wc_snapshot();
    let wall = Instant::now();
    for k in 1..=tokens {
        let id = k as u32; // nonzero token id (k starts at 1).
        let t = Instant::now();
        STAGE[0].store(id, Ordering::Release);
        let _ = libthyla_rs::torpor::wake(&STAGE[0], 1);
        let mut spins = 0u32;
        while STAGE[stages].load(Ordering::Acquire) != id {
            let _ = libthyla_rs::torpor::wait(&STAGE[stages], 0, Some(Duration::from_millis(100)));
            spins += 1;
            if spins > 50 {
                break; // ~5s: a stage wedged; bail rather than hang.
            }
        }
        STAGE[stages].store(0, Ordering::Release);
        samples.push(t.elapsed().as_nanos() as u64);
    }
    let total_ns = wall.elapsed().as_nanos() as u64;
    let wc_after = wc_snapshot();
    G_STOP.store(1, Ordering::Release);
    for s in 0..stages {
        let _ = libthyla_rs::torpor::wake(&STAGE[s], 1);
    }
    let _ = join_range(0, stages);
    samples.sort_unstable();
    let mut sum: u64 = 0;
    for &x in &samples {
        sum += x;
    }
    let mean = if samples.is_empty() { 0 } else { sum / samples.len() as u64 };
    let tps = if total_ns > 0 {
        (samples.len() as u64).saturating_mul(1_000_000_000) / total_ns
    } else {
        0
    };
    t_putstr(&format!(
        "pipeline: {} tokens x {} stages -> {} tokens/s; end-to-end mean {} p50 {} p99 {} p99.9 {} max {} us\n",
        samples.len(),
        stages,
        tps,
        fmt_us(mean),
        fmt_us(pct(&samples, 500)),
        fmt_us(pct(&samples, 990)),
        fmt_us(pct(&samples, 999)),
        fmt_us(samples.last().copied().unwrap_or(0)),
    ));
    report_wc_delta(wc_before, wc_after);
}

/// contention -- N threads hammer one torpor-backed lock. Acquisitions/sec +
/// fairness (the min/max per-thread spread). Stresses the lock + wait/wake path
/// (the allocator, shared server state).
fn mode_contend(pool: &StackPool, n: usize, rounds: u64) {
    G_KIND.store(KIND_CONTEND, Ordering::SeqCst);
    G_ROUNDS.store(rounds, Ordering::SeqCst);
    G_MUTEX.store(0, Ordering::SeqCst);
    if !spawn_batch(pool, n) {
        t_putstr("contention: FAIL (spawn)\n");
        return;
    }
    let t = Instant::now();
    release_batch();
    if !join_batch(n) {
        t_putstr("contention: FAIL (join timeout)\n");
        return;
    }
    let ns = t.elapsed().as_nanos() as u64;
    let mut total: u64 = 0;
    let (mut lo, mut hi) = (u64::MAX, 0u64);
    for i in 0..n {
        let g = OPS_DONE[i].load(Ordering::SeqCst);
        total += g;
        lo = lo.min(g);
        hi = hi.max(g);
    }
    let aps = if ns > 0 { total.saturating_mul(1_000_000_000) / ns } else { 0 };
    let fairness = if hi > 0 { lo * 1000 / hi } else { 0 };
    t_putstr(&format!(
        "contention: {} acquisitions over {} threads in {} ms -> {} acq/s; fairness {}.{:01} (per-thread {}..{})\n",
        total,
        n,
        ns / 1_000_000,
        aps,
        fairness / 1000,
        (fairness % 1000) / 100,
        lo,
        hi,
    ));
}

/// idle -- the #299 idle-cost axis. A quiet window with no workers; the all-parks
/// delta over it is the wake/vmexit rate (tickful idle re-parks ~1000x/s/CPU on
/// the periodic tick; tickless deep-parks ~10x/s). The redesign must keep this
/// LOW while matching the tickful throughput/latency.
fn mode_idle(window_ms: u64) {
    let before = wc_all_parks();
    let _ = time::sleep(Duration::from_millis(window_ms));
    let after = wc_all_parks();
    match (before, after) {
        (Some(b), Some(a)) => {
            let parks = a.saturating_sub(b);
            let pps = parks.saturating_mul(1000) / window_ms.max(1);
            t_putstr(&format!(
                "idle: {} ms quiet window -> {} parks ({} parks/s; high = tickful re-poll, low = tickless deep-park = the #299 axis)\n",
                window_ms, parks, pps,
            ));
        }
        _ => {
            t_putstr("idle: n/a (/ctl/sched unreadable)\n");
        }
    }
}

/// mixed -- FORWARD SEAM (priority). An INTERACTIVE probe (this thread) vs N IDLE
/// CPU hogs. The per-worker priorities are applied via worker_apply_attr, which
/// no-ops today (no SYS_SCHED_SETATTR) -> this prints the UNIFORM-priority
/// baseline the future priority system must beat (the probe's tail should drop
/// below `latency`'s once IDLE hogs actually yield to the INTERACTIVE probe).
fn mode_mixed(pool: &StackPool, hogs: usize, probe_iters: u64, probe_us: u64) {
    clear_attrs(MAX_WORKERS);
    for s in 0..hogs.min(MAX_WORKERS) {
        PRIO[s].store(libthyla_rs::sched::prio::IDLE as u32, Ordering::SeqCst);
    }
    let _ = libthyla_rs::sched::set_self_priority(libthyla_rs::sched::prio::INTERACTIVE);
    G_KIND.store(KIND_HOG, Ordering::SeqCst);
    G_INNER_ITERS.store(50_000, Ordering::SeqCst);
    G_STOP.store(0, Ordering::SeqCst);
    G_START.store(0, Ordering::SeqCst);
    for s in 0..hogs {
        if !spawn_at(pool, s, s as u64) {
            t_putstr("mixed: FAIL (spawn hogs)\n");
            G_STOP.store(1, Ordering::SeqCst);
            return;
        }
    }
    G_START.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake_all(&G_START);
    let _ = time::sleep(Duration::from_millis(20));
    let mut samples: Vec<u64> = Vec::with_capacity(probe_iters as usize);
    let target = probe_us.wrapping_mul(1000);
    for _ in 0..probe_iters {
        let t = Instant::now();
        let _ = time::sleep(Duration::from_micros(probe_us));
        samples.push((t.elapsed().as_nanos() as u64).saturating_sub(target));
    }
    G_STOP.store(1, Ordering::SeqCst);
    let _ = join_range(0, hogs);
    let _ = libthyla_rs::sched::set_self_priority(libthyla_rs::sched::prio::NORMAL); // reset main.
    samples.sort_unstable();
    let enforced = libthyla_rs::sched::is_supported();
    t_putstr(&format!(
        "mixed: INTERACTIVE probe vs {} IDLE hogs -> overshoot p50 {} p99 {} p99.9 {} max {} us [priority {}: {}]\n",
        hogs,
        fmt_us(pct(&samples, 500)),
        fmt_us(pct(&samples, 990)),
        fmt_us(pct(&samples, 999)),
        fmt_us(samples.last().copied().unwrap_or(0)),
        if enforced { "ENFORCED" } else { "NOT enforced" },
        if enforced {
            "live"
        } else {
            "uniform-priority baseline; v1.x SYS_SCHED_SETATTR must beat it"
        },
    ));
}

/// affinity -- FORWARD SEAM (affinity). A cross-CPU ping-pong with the responder
/// pinned (slot 0 -> CPU 1, the timer -> CPU 0). Pinning is applied via
/// worker_apply_attr / set_self_affinity, which no-op today -> this is the
/// natural-placement RTT baseline; with real pinning, same-CPU vs cross-CPU RTT
/// becomes measurable.
fn mode_affinity(pool: &StackPool, iters: u64) {
    clear_attrs(MAX_WORKERS);
    AFFINITY[0].store(libthyla_rs::sched::affinity_for(1), Ordering::SeqCst);
    let _ = libthyla_rs::sched::set_self_affinity(libthyla_rs::sched::affinity_for(0));
    G_KIND.store(KIND_PINGPONG, Ordering::SeqCst);
    PP_TURN.store(0, Ordering::SeqCst);
    PP_STOP.store(0, Ordering::SeqCst);
    G_START.store(1, Ordering::SeqCst);
    if !spawn_at(pool, 0, 0) {
        t_putstr("affinity: FAIL (spawn responder)\n");
        return;
    }
    let mut samples: Vec<u64> = Vec::with_capacity(iters as usize);
    for _ in 0..iters {
        let t = Instant::now();
        PP_TURN.store(1, Ordering::Release);
        let _ = libthyla_rs::torpor::wake(&PP_TURN, 1);
        let mut spins = 0u32;
        while PP_TURN.load(Ordering::Acquire) != 0 {
            let _ = libthyla_rs::torpor::wait(&PP_TURN, 1, Some(Duration::from_millis(100)));
            spins += 1;
            if spins > 50 {
                break;
            }
        }
        samples.push(t.elapsed().as_nanos() as u64);
    }
    PP_STOP.store(1, Ordering::Release);
    PP_TURN.store(1, Ordering::Release);
    let _ = libthyla_rs::torpor::wake(&PP_TURN, 1);
    let _ = libthyla_rs::thread::join_tid(&TIDS[0], TID_SENTINEL, Some(Duration::from_secs(10)));
    let _ = libthyla_rs::sched::set_self_affinity(0); // reset main (0 = all CPUs).
    samples.sort_unstable();
    let enforced = libthyla_rs::sched::is_supported();
    t_putstr(&format!(
        "affinity: pinned cross-CPU ping-pong ({} rtt) -> p50 {} p99 {} p99.9 {} max {} us [pinning {}: {}]\n",
        samples.len(),
        fmt_us(pct(&samples, 500)),
        fmt_us(pct(&samples, 990)),
        fmt_us(pct(&samples, 999)),
        fmt_us(samples.last().copied().unwrap_or(0)),
        if enforced { "ENFORCED" } else { "NOT enforced" },
        if enforced { "live" } else { "natural-placement baseline; v1.x SYS_SCHED_SETATTR" },
    ));
}

/// The comprehensive deep run: every mode at solid params in a delimited block.
/// The redesign's compass + the tickful-baseline capture target.
fn run_all(pool: &mut StackPool, cpus: usize) {
    let need = (cpus * 2 + cpus / 2 + 2).min(MAX_WORKERS);
    if !pool.ensure(need) {
        t_putstr("cpubench all: FAIL (stack pool OOM)\n");
        return;
    }
    let burst = (cpus * 2).min(MAX_WORKERS - cpus.max(1));
    let hogsub = (cpus / 2).max(1);
    t_putstr(&format!(
        "cpubench: ==== TI-4e comprehensive bench (cpus={}) ====\n",
        cpus
    ));
    mode_idle(500); // first -- labels the build (high parks/s = tickful, low = tickless).
    mode_single(Duration::from_millis(200));
    mode_scale(pool, cpus, 30_000_000);
    mode_yield(pool, cpus, 100, 500);
    mode_storm(pool, cpus, 20);
    mode_pingpong(pool, 5_000);
    mode_latency(pool, cpus, 300, 1_000);
    mode_wakestorm(pool, (cpus * 2).min(MAX_WORKERS), 300, 200);
    mode_burst(pool, burst, hogsub);
    mode_pipeline(pool, cpus.clamp(2, 4), 3_000);
    mode_contend(pool, (cpus * 2).min(MAX_WORKERS), 5_000);
    mode_mixed(pool, cpus, 300, 1_000);
    mode_affinity(pool, 3_000);
    t_putstr("cpubench: ==== END comprehensive ====\n");
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
        Some("latency") => {
            let iters = p3.unwrap_or(500);
            if !pool.ensure(n) {
                fail("stack pool (latency)");
            }
            mode_latency(&pool, n, iters, 1_000);
        }
        Some("wakestorm") => {
            let rounds = p3.unwrap_or(500);
            if !pool.ensure(n) {
                fail("stack pool (wakestorm)");
            }
            mode_wakestorm(&pool, n, rounds, 200);
        }
        Some("burst") => {
            // arg2 = burst size (default cpus*2); hogs = cpus/2.
            let burst = args
                .get_str(2)
                .and_then(|s| s.parse::<usize>().ok())
                .map(|v| v.clamp(1, MAX_WORKERS - 1))
                .unwrap_or((cpus * 2).min(MAX_WORKERS - cpus.max(1)));
            let hogs = (cpus / 2).max(1);
            if !pool.ensure((burst + hogs).min(MAX_WORKERS)) {
                fail("stack pool (burst)");
            }
            mode_burst(&pool, burst, hogs);
        }
        Some("pipeline") => {
            let stages = n.clamp(2, MAX_STAGES);
            let tokens = p3.unwrap_or(5_000);
            if !pool.ensure(stages) {
                fail("stack pool (pipeline)");
            }
            mode_pipeline(&pool, stages, tokens);
        }
        Some("contention") | Some("contend") => {
            let rounds = p3.unwrap_or(10_000);
            if !pool.ensure(n) {
                fail("stack pool (contention)");
            }
            mode_contend(&pool, n, rounds);
        }
        Some("idle") => {
            mode_idle(p3.unwrap_or(1_000));
        }
        Some("mixed") => {
            let iters = p3.unwrap_or(500);
            if !pool.ensure(n) {
                fail("stack pool (mixed)");
            }
            mode_mixed(&pool, n, iters, 1_000);
        }
        Some("affinity") => {
            mode_affinity(&pool, p3.unwrap_or(5_000));
        }
        Some("all") => {
            run_all(&mut pool, cpus);
        }
        _ => {
            // Default: the short all-modes boot/CI probe. Bounded so it runs in
            // ~1-2s and never hangs; prints data (greppable), exits 0.
            t_putstr(&format!(
                "cpubench: TI-4e scheduler probe (cpus={}) -- single/scale/yield/storm/pingpong/idle ('cpubench all' = the full bench)\n",
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
            // the #299 idle-cost axis in every boot log (cheap; 200ms window).
            mode_idle(200);
            let wc1 = wc_snapshot();
            t_putstr("cpubench: full-run kernel work-conservation:\n");
            report_wc_delta(wc0, wc1);
            t_putstr("cpubench: PROBE OK\n");
        }
    }
    0
}
