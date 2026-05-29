// /burrow-torture -- native kernel-burrow re-attach stress (EBADTAG DFS).
//
// Built to reproduce, in isolation (no musl, no Stratum, no mount), the
// content-sensitive corruption that surfaces in stratumd as STM_EBADTAG: a
// 128 KiB btree-node decrypt reads correct ciphertext (proven byte-perfect at
// op_read time) which is then corrupted BETWEEN read and decrypt. The pouch
// boundary maps malloc(128 KiB) -> SYS_BURROW_ATTACH (a 32-page burrow), and
// kernel/burrow.c:394 itself flags the suspect: "the trigger depends on buddy
// LIFO returning [a different PA] on re-attach" + stale PTE/TLB after
// SYS_BURROW_DETACH (the F1 fix's mmu_uninstall_user_range + TLBI is suspected
// incomplete for the multi-page case).
//
// This binary drives SYS_BURROW_ATTACH / SYS_BURROW_DETACH directly and checks
// the kernel's two contracts on every attach:
//   (a) a fresh attach is demand-ZERO (burrow_create_anon KP_ZERO);
//   (b) a write-then-read round-trips.
// The decisive detector is (a) on a RE-attached VA: if the VA was reused with a
// different PA but the prior detach's TLBI was incomplete, the stale TLB entry
// shadows the new (zeroed) PA and the read returns the PRIOR tenant's pattern
// (non-zero) -> corruption reproduced, kernel-side, no musl confound.
//
// Three phases per size, across the buddy order-5/order-6 boundary:
//   1. tight attach/read-zero/write/verify/detach (same VA+PA reuse; sanity).
//   2. K holders + churn (catches a new attach clobbering a LIVE burrow).
//   3. VA/PA decoupling pairs (the precise burrow.c:394 trigger): attach A,B;
//      write; detach A then B (LIFO: B's PA on top); re-attach C,D -> first-fit
//      reuses VA_A,VA_B while LIFO hands C=PA_B, D=PA_A. Both VAs come back with
//      a DIFFERENT PA; a fresh attach must read zero. Stale TLB -> non-zero.
//
// joey spawns this EARLY (before the slow stratumd mount). Prints one line per
// phase + "burrow-torture: ALL OK" on success (exit 0); on the first corruption
// prints a tagged FAIL with iter/page/va/got + exits 1.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use core::time::Duration;
use libthyla_rs::thread;
use libthyla_rs::{t_burrow_attach, t_burrow_detach, t_putstr};

const PAGE: u64 = 4096;
const K_MAX: usize = 8;

// Print "<label>=0x<16 hex>\n" with no allocator dependency.
fn put_hex(label: &str, v: u64) {
    let mut buf = [0u8; 18];
    buf[0] = b'0';
    buf[1] = b'x';
    let hexd = b"0123456789abcdef";
    let mut i = 0usize;
    while i < 16 {
        let nyb = ((v >> ((15 - i) * 4)) & 0xf) as usize;
        buf[2 + i] = hexd[nyb];
        i += 1;
    }
    t_putstr(label);
    t_putstr("=");
    if let Ok(s) = core::str::from_utf8(&buf) {
        t_putstr(s);
    }
    t_putstr("\n");
}

// Non-zero, recognizable, deterministic per (gen, page).
#[inline(always)]
fn pat(gen: u64, page: u64) -> u64 {
    0xC0DE_0000_0000_0000u64 | ((gen & 0xFFFFFF) << 24) | (page & 0xFFFF)
}

// Touch the first + last word of every page (forces a demand-fault on each
// page, so the detach must clear/flush every page's PTE/TLB).
unsafe fn touch_write(va: u64, npages: u64, gen: u64) {
    let mut p = 0u64;
    while p < npages {
        let base = va + p * PAGE;
        core::ptr::write_volatile(base as *mut u64, pat(gen, p));
        core::ptr::write_volatile((base + PAGE - 8) as *mut u64, pat(gen, p).rotate_left(32));
        p += 1;
    }
}

// Returns the offending page index, or -1 if every page holds its pattern.
unsafe fn verify(va: u64, npages: u64, gen: u64) -> i64 {
    let mut p = 0u64;
    while p < npages {
        let base = va + p * PAGE;
        if core::ptr::read_volatile(base as *const u64) != pat(gen, p) {
            return p as i64;
        }
        if core::ptr::read_volatile((base + PAGE - 8) as *const u64) != pat(gen, p).rotate_left(32) {
            return p as i64;
        }
        p += 1;
    }
    -1
}

// Returns the offending page index, or -1 if every page reads zero (the
// demand-zero contract). Reading each page also demand-faults it in, installing
// a PTE -- so a subsequent detach must clear all of them.
unsafe fn read_expect_zero(va: u64, npages: u64) -> i64 {
    let mut p = 0u64;
    while p < npages {
        let base = va + p * PAGE;
        if core::ptr::read_volatile(base as *const u64) != 0 {
            return p as i64;
        }
        if core::ptr::read_volatile((base + PAGE - 8) as *const u64) != 0 {
            return p as i64;
        }
        p += 1;
    }
    -1
}

unsafe fn report_stale(tag: &str, va: u64, page: u64) {
    t_putstr(tag);
    put_hex("  page", page);
    put_hex("  va", va);
    put_hex("  got", core::ptr::read_volatile((va + page * PAGE) as *const u64));
}

unsafe fn torture(sz: u64, n1: u64, k: usize, m: u64, pairs: u64) -> i64 {
    let npages = sz / PAGE;
    put_hex("burrow-torture: ==> size", sz);

    // Phase 1: tight cycle (mostly same VA+PA reuse; basic stale + writeback).
    let mut i = 0u64;
    while i < n1 {
        let va = t_burrow_attach(sz);
        if va < 0 {
            t_putstr("  FAIL attach (phase1)\n");
            put_hex("  iter", i);
            return 1;
        }
        let va = va as u64;
        let z = read_expect_zero(va, npages);
        if z >= 0 {
            t_putstr("  FAIL stale-nonzero on fresh attach (phase1)\n");
            put_hex("  iter", i);
            report_stale("  (incomplete TLBI/PTE-clear or demand-zero)\n", va, z as u64);
            return 1;
        }
        touch_write(va, npages, i);
        let b = verify(va, npages, i);
        if b >= 0 {
            t_putstr("  FAIL writeback mismatch (phase1)\n");
            put_hex("  iter", i);
            put_hex("  page", b as u64);
            return 1;
        }
        if t_burrow_detach(va, sz) != 0 {
            t_putstr("  FAIL detach (phase1)\n");
            put_hex("  iter", i);
            return 1;
        }
        i += 1;
    }
    t_putstr("  phase1 OK (tight attach/zero/write/verify/detach)\n");

    // Phase 2: K live holders + churn re-attach the middle one; cross-verify all
    // (catches a new attach clobbering a LIVE burrow).
    let mut hva = [0u64; K_MAX];
    let mut hgen = [0u64; K_MAX];
    let mut gen: u64 = 0x100000;
    let mut j = 0usize;
    while j < k {
        let va = t_burrow_attach(sz);
        if va < 0 {
            t_putstr("  FAIL attach (phase2 init)\n");
            return 1;
        }
        gen += 1;
        hgen[j] = gen;
        hva[j] = va as u64;
        touch_write(hva[j], npages, gen);
        j += 1;
    }
    let mut it = 0u64;
    while it < m {
        let jj = (it as usize) % k;
        if t_burrow_detach(hva[jj], sz) != 0 {
            t_putstr("  FAIL detach (phase2)\n");
            put_hex("  it", it);
            return 1;
        }
        let va = t_burrow_attach(sz);
        if va < 0 {
            t_putstr("  FAIL attach (phase2)\n");
            put_hex("  it", it);
            return 1;
        }
        let va = va as u64;
        let z = read_expect_zero(va, npages);
        if z >= 0 {
            t_putstr("  FAIL stale-nonzero on re-attach (phase2)\n");
            put_hex("  it", it);
            report_stale("  (stale TLB / different-PA re-attach)\n", va, z as u64);
            return 1;
        }
        gen += 1;
        hgen[jj] = gen;
        hva[jj] = va;
        touch_write(va, npages, gen);
        let mut c = 0usize;
        while c < k {
            let b = verify(hva[c], npages, hgen[c]);
            if b >= 0 {
                t_putstr("  FAIL cross-burrow corruption (phase2): a live holder was clobbered\n");
                put_hex("  it", it);
                put_hex("  clobbered_holder", c as u64);
                put_hex("  page", b as u64);
                put_hex("  holder_va", hva[c]);
                put_hex("  just_attached_va", va);
                return 1;
            }
            c += 1;
        }
        it += 1;
    }
    let mut t = 0usize;
    while t < k {
        t_burrow_detach(hva[t], sz);
        t += 1;
    }
    t_putstr("  phase2 OK (holders + churn, no stale, no cross-corruption)\n");

    // Phase 3: VA/PA decoupling -- the precise burrow.c:394 trigger.
    let mut q = 0u64;
    while q < pairs {
        let a = t_burrow_attach(sz);
        let b = t_burrow_attach(sz);
        if a < 0 || b < 0 {
            t_putstr("  FAIL attach (phase3 A/B)\n");
            put_hex("  q", q);
            return 1;
        }
        let (a, b) = (a as u64, b as u64);
        gen += 1;
        let ga = gen;
        touch_write(a, npages, ga);
        gen += 1;
        let gb = gen;
        touch_write(b, npages, gb);
        // Detach A then B: buddy LIFO now has B's PA on top, A's below.
        if t_burrow_detach(a, sz) != 0 || t_burrow_detach(b, sz) != 0 {
            t_putstr("  FAIL detach (phase3 A/B)\n");
            put_hex("  q", q);
            return 1;
        }
        // Re-attach C,D: first-fit reuses VA_A,VA_B; LIFO hands C=PA_B, D=PA_A.
        // Both VAs return with a DIFFERENT PA than their last tenant -> a fresh
        // (zeroed) attach. Stale TLB from the detach -> the read sees the PRIOR
        // tenant's pattern instead of zero.
        let c = t_burrow_attach(sz);
        if c < 0 {
            t_putstr("  FAIL attach (phase3 C)\n");
            put_hex("  q", q);
            return 1;
        }
        let c = c as u64;
        let zc = read_expect_zero(c, npages);
        if zc >= 0 {
            t_putstr("  FAIL stale-nonzero on decoupled re-attach (phase3 C) -- buddy-LIFO-different-PA + stale TLB\n");
            put_hex("  q", q);
            report_stale("  (VA reused, PA swapped, prior tenant visible)\n", c, zc as u64);
            return 1;
        }
        let d = t_burrow_attach(sz);
        if d < 0 {
            t_putstr("  FAIL attach (phase3 D)\n");
            put_hex("  q", q);
            return 1;
        }
        let d = d as u64;
        let zd = read_expect_zero(d, npages);
        if zd >= 0 {
            t_putstr("  FAIL stale-nonzero on decoupled re-attach (phase3 D)\n");
            put_hex("  q", q);
            report_stale("  (VA reused, PA swapped, prior tenant visible)\n", d, zd as u64);
            return 1;
        }
        gen += 1;
        let gc = gen;
        touch_write(c, npages, gc);
        gen += 1;
        let gd = gen;
        touch_write(d, npages, gd);
        let bc = verify(c, npages, gc);
        let bd = verify(d, npages, gd);
        if bc >= 0 || bd >= 0 {
            t_putstr("  FAIL writeback mismatch (phase3 C/D)\n");
            put_hex("  q", q);
            return 1;
        }
        t_burrow_detach(c, sz);
        t_burrow_detach(d, sz);
        q += 1;
    }
    t_putstr("  phase3 OK (VA/PA decoupling, no stale, no writeback mismatch)\n");
    0
}

// ---- Phase 4: multi-threaded SMP burrow stress -------------------------------
// stratumd is multi-threaded; the single-threaded burrow path tested clean, so
// the residual suspect is the SMP path -- concurrent attach/detach across vCPUs
// stressing the buddy zone lock, the per-Proc vma_lock, and the inner-shareable
// TLBI broadcast (a detach on CPU-A must invalidate CPU-B's TLB before CPU-B
// reuses the freed VA/PA). Each thread tortures its OWN burrows (no shared
// region -> no app-level data race); a thread reading another thread's pattern
// means the buddy handed the same PA to two threads (lock race), and a stale
// non-zero on a fresh attach means a cross-CPU TLBI gap. Threads record to
// shared atomics only (no concurrent console writes); main joins + reports.

const MT_THREADS: usize = 3;
const MT_ITERS: u64 = 96;
const MT_SZ: u64 = 128 * 1024;
const MT_STACK_SZ: u64 = 128 * 1024;

static MT_TID: [AtomicU32; MT_THREADS] =
    [AtomicU32::new(0), AtomicU32::new(0), AtomicU32::new(0)];
static MT_FAIL: AtomicU32 = AtomicU32::new(0);
static MT_FAIL_IDX: AtomicU32 = AtomicU32::new(0xFFFF_FFFF);
static MT_FAIL_KIND: AtomicU32 = AtomicU32::new(0); // 1 stale, 2 verify, 3 attach, 4 detach
static MT_FAIL_PAGE: AtomicU64 = AtomicU64::new(0);
static MT_FAIL_VA: AtomicU64 = AtomicU64::new(0);
static MT_FAIL_GOT: AtomicU64 = AtomicU64::new(0);

// First failer wins; record detail for main to print after the join.
fn mt_record(idx: usize, kind: u32, page: u64, va: u64, got: u64) {
    if MT_FAIL
        .compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst)
        .is_ok()
    {
        MT_FAIL_IDX.store(idx as u32, Ordering::SeqCst);
        MT_FAIL_KIND.store(kind, Ordering::SeqCst);
        MT_FAIL_PAGE.store(page, Ordering::SeqCst);
        MT_FAIL_VA.store(va, Ordering::SeqCst);
        MT_FAIL_GOT.store(got, Ordering::SeqCst);
    }
}

// Per-thread entry. arg = thread index. gen encodes idx in the low 24 bits so
// pat()'s mask preserves it -> a cross-thread PA leak shows up as a wrong idx
// field in the verify mismatch.
extern "C" fn mt_entry(arg: u64) {
    let idx = arg as usize;
    if idx < MT_THREADS {
        let _ = thread::set_tid_address(&MT_TID[idx]);
    }
    let npages = MT_SZ / PAGE;
    let mut i = 0u64;
    while i < MT_ITERS {
        if MT_FAIL.load(Ordering::Relaxed) != 0 {
            break; // another thread already tripped; stop early
        }
        let gen = ((idx as u64) << 20) | (i & 0xF_FFFF);
        let va = unsafe { t_burrow_attach(MT_SZ) };
        if va < 0 {
            mt_record(idx, 3, 0, 0, 0);
            break;
        }
        let va = va as u64;
        let z = unsafe { read_expect_zero(va, npages) };
        if z >= 0 {
            let got = unsafe { core::ptr::read_volatile((va + (z as u64) * PAGE) as *const u64) };
            mt_record(idx, 1, z as u64, va, got);
            unsafe { t_burrow_detach(va, MT_SZ) };
            break;
        }
        unsafe { touch_write(va, npages, gen) };
        let b = unsafe { verify(va, npages, gen) };
        if b >= 0 {
            let got = unsafe { core::ptr::read_volatile((va + (b as u64) * PAGE) as *const u64) };
            mt_record(idx, 2, b as u64, va, got);
            unsafe { t_burrow_detach(va, MT_SZ) };
            break;
        }
        if unsafe { t_burrow_detach(va, MT_SZ) } != 0 {
            mt_record(idx, 4, 0, va, 0);
            break;
        }
        i += 1;
    }
    thread::exit_self();
}

unsafe fn phase4_mt() -> i64 {
    t_putstr("burrow-torture: ==> phase4 multi-threaded SMP stress\n");
    put_hex("  threads", MT_THREADS as u64);
    put_hex("  iters_each", MT_ITERS);
    put_hex("  size", MT_SZ);
    let mut j = 0usize;
    while j < MT_THREADS {
        MT_TID[j].store((j as u32) + 1, Ordering::SeqCst); // sentinel BEFORE spawn
        let base = t_burrow_attach(MT_STACK_SZ);
        if base < 0 {
            t_putstr("  FAIL stack attach (phase4)\n");
            return 1;
        }
        let sp = (base as u64) + MT_STACK_SZ; // 16-aligned top (page-aligned base + page-mult)
        if thread::spawn_raw(mt_entry as usize as u64, sp, j as u64, 0).is_err() {
            t_putstr("  FAIL spawn_raw (phase4)\n");
            put_hex("  thread", j as u64);
            return 1;
        }
        j += 1;
    }
    // Join all (clear-child-tid + torpor). Generous timeout: bounded torture
    // under TCG is slow but finite; a real timeout would itself signal a hang.
    let mut k = 0usize;
    while k < MT_THREADS {
        let _ = thread::join_tid(&MT_TID[k], (k as u32) + 1, Some(Duration::from_secs(180)));
        k += 1;
    }
    if MT_FAIL.load(Ordering::SeqCst) != 0 {
        t_putstr("  FAIL multi-threaded burrow corruption (phase4): buddy PA double-hand / vma_lock race / cross-CPU TLBI gap\n");
        put_hex("  thread", MT_FAIL_IDX.load(Ordering::SeqCst) as u64);
        put_hex("  kind(1=stale 2=verify 3=attach 4=detach)", MT_FAIL_KIND.load(Ordering::SeqCst) as u64);
        put_hex("  page", MT_FAIL_PAGE.load(Ordering::SeqCst));
        put_hex("  va", MT_FAIL_VA.load(Ordering::SeqCst));
        put_hex("  got", MT_FAIL_GOT.load(Ordering::SeqCst));
        return 1;
    }
    t_putstr("  phase4 OK (multi-threaded, no SMP burrow corruption)\n");
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("burrow-torture: start (kernel burrow attach/detach/re-attach stress; EBADTAG DFS)\n");
    // Sizes span the suspect band + the buddy order boundary:
    //   128 KiB = 32 pages = buddy order-5 (the documented STM_BTNODE_SIZE).
    //   132 KiB = 33 pages -> rounds to buddy order-6 (64-page block, half used).
    //   256 KiB = 64 pages = buddy order-6 (full).
    let sizes: [u64; 3] = [128 * 1024, 132 * 1024, 256 * 1024];
    let mut s = 0usize;
    while s < sizes.len() {
        // (n1 tight, k holders, m churn, pairs decoupling) -- modest counts to
        // bound boot time under QEMU-TCG while churning VA/PA reuse thoroughly.
        let rc = unsafe { torture(sizes[s], 48, 4, 64, 64) };
        if rc != 0 {
            t_putstr("burrow-torture: REPRODUCED corruption (kernel burrow re-attach bug) -- see above\n");
            return 1;
        }
        s += 1;
    }
    // Phase 4: multi-threaded SMP stress (concurrent attach/detach across vCPUs).
    if unsafe { phase4_mt() } != 0 {
        t_putstr("burrow-torture: REPRODUCED corruption (multi-threaded SMP burrow) -- see above\n");
        return 1;
    }
    t_putstr("burrow-torture: ALL OK (single-threaded x3 sizes + multi-threaded SMP)\n");
    0
}
