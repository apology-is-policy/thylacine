// /thread-torture -- a Proc retires and respawns threads far past PROC_THREAD_MAX
// (XT-3b; docs/X86-TRANSLATION-DESIGN.md 5.8 XT-K9; I-32).
//
// A thread that exits while a peer lives on RETIRES: it stops counting against
// the per-Proc thread cap, and a live peer frees it at its next spawn or exit.
// Before XT-3b an exited thread stayed linked until the whole Proc died, so the
// cap of 256 bounded a Proc's LIFETIME spawns: a non-exempt Proc's 256th spawn
// failed -EAGAIN, and an exempt one (joey's children) pinned every dead
// thread's kernel stack until exit.
//
// Two phases, then the check:
//   1. SEQUENTIAL: SEQ_SPAWNS workers, each spawned, run and joined in turn
//      (the clear-child-tid join: the kernel zeroes the word at exit and wakes
//      the joiner).
//   2. CONCURRENT: PAR_SPAWNERS peers each spawn and join PAR_PER workers at
//      the same time, so the kernel's reapers race each other and the exits
//      they reap.
//   3. /proc/<pid>/status `threads:` -- the live count -- must be back to 1.
//      Pre-XT-3b it read every thread the Proc had ever spawned.
//
// Run by joey at boot (an exempt Proc, so the cap itself is not in play there:
// the live count is the witness) and by tools/interactive/ls-ci.exp leg (f) as
// a logged-in user, where a lifetime cap would refuse spawn 256.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicU32, Ordering};
use core::time::Duration;

use libthyla_rs::fs::File;
use libthyla_rs::io::Read;
use libthyla_rs::t_write;
use libthyla_rs::thread::{exit_self, join_tid, set_tid_address, spawn_raw};

const SEQ_SPAWNS: u32 = 1100;    // more than four times PROC_THREAD_MAX (256)
const PAR_SPAWNERS: usize = 3;
const PAR_PER: u32 = 150;
const STACK_BYTES: usize = 16 * 1024;
const ARMED: u32 = 0xC71D_0B5E;  // a tid word before its thread exits
const JOIN_WAIT: Duration = Duration::from_secs(10);

// A thread's stack, owned by exactly one thread at a time: a joiner reuses a
// worker's stack only after the join, when the worker has made its exit SVC
// and the kernel's exit path no longer touches user memory.
#[repr(C, align(16))]
struct Stack(UnsafeCell<[u8; STACK_BYTES]>);
unsafe impl Sync for Stack {}
impl Stack {
    const fn new() -> Self { Stack(UnsafeCell::new([0; STACK_BYTES])) }
    fn top(&self) -> u64 { self.0.get() as u64 + STACK_BYTES as u64 }
}

struct Slot {
    tid_word: AtomicU32,   // the join word: ARMED, then 0 at the thread's exit
    ran: AtomicU32,        // workers that ran their body
}
impl Slot {
    const fn new() -> Self { Slot { tid_word: AtomicU32::new(0), ran: AtomicU32::new(0) } }
}

static SEQ_STACK: Stack = Stack::new();
static SEQ: Slot = Slot::new();

static SPAWNER_STACKS: [Stack; PAR_SPAWNERS] = [Stack::new(), Stack::new(), Stack::new()];
static WORKER_STACKS: [Stack; PAR_SPAWNERS] = [Stack::new(), Stack::new(), Stack::new()];
static SPAWNER: [Slot; PAR_SPAWNERS] = [Slot::new(), Slot::new(), Slot::new()];
static WORKER: [Slot; PAR_SPAWNERS] = [Slot::new(), Slot::new(), Slot::new()];
static PAR_FAILED: AtomicU32 = AtomicU32::new(0);

fn say(s: &str) {
    unsafe {
        let _ = t_write(1, s.as_ptr(), s.len());
    }
}

// The worker body: register the join word, count the run, exit.
extern "C" fn worker_main(arg: u64) -> ! {
    let slot = unsafe { &*(arg as *const Slot) };
    set_tid_address(&slot.tid_word);
    slot.ran.fetch_add(1, Ordering::AcqRel);
    exit_self()
}

// Spawn one worker on `stack` reporting to `slot`, and join it.
fn spawn_and_join(slot: &Slot, stack: &Stack) -> Result<(), &'static str> {
    slot.tid_word.store(ARMED, Ordering::Release);
    let entry = worker_main as extern "C" fn(u64) -> ! as usize as u64;
    unsafe { spawn_raw(entry, stack.top(), slot as *const Slot as u64, 0) }
        .map_err(|_| "spawn refused")?;
    join_tid(&slot.tid_word, ARMED, Some(JOIN_WAIT)).map_err(|_| "join timed out")
}

extern "C" fn spawner_main(arg: u64) -> ! {
    let k = arg as usize;
    set_tid_address(&SPAWNER[k].tid_word);
    for _ in 0..PAR_PER {
        if spawn_and_join(&WORKER[k], &WORKER_STACKS[k]).is_err() {
            PAR_FAILED.store(1, Ordering::Release);
            break;
        }
    }
    exit_self()
}

// The `threads:` line of this Proc's /proc status, read in one call (a /proc
// leaf renders per read).
fn live_threads() -> Option<u32> {
    let path = alloc::format!("/proc/{}/status", libthyla_rs::identity::pid());
    let mut f = File::open(path.as_str()).ok()?;
    let mut buf = alloc::vec![0u8; 1024];
    let n = f.read(&mut buf).ok()?;
    let text = core::str::from_utf8(&buf[..n]).ok()?;
    let line = text.lines().find(|l| l.starts_with("threads:"))?;
    line["threads:".len()..].trim().parse().ok()
}

fn fail(what: &str, at: u32) -> i64 {
    say(&alloc::format!("thread-torture: FAIL {} at spawn {}\n", what, at));
    1
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    for i in 0..SEQ_SPAWNS {
        if let Err(e) = spawn_and_join(&SEQ, &SEQ_STACK) {
            return fail(e, i + 1);
        }
    }
    if SEQ.ran.load(Ordering::Acquire) != SEQ_SPAWNS {
        return fail("a sequential worker did not run", SEQ_SPAWNS);
    }
    say(&alloc::format!("thread-torture: {} sequential spawns joined\n", SEQ_SPAWNS));

    let entry = spawner_main as extern "C" fn(u64) -> ! as usize as u64;
    for k in 0..PAR_SPAWNERS {
        SPAWNER[k].tid_word.store(ARMED, Ordering::Release);
        if unsafe { spawn_raw(entry, SPAWNER_STACKS[k].top(), k as u64, 0) }.is_err() {
            return fail("spawner refused", k as u32 + 1);
        }
    }
    for k in 0..PAR_SPAWNERS {
        if join_tid(&SPAWNER[k].tid_word, ARMED, Some(JOIN_WAIT * PAR_PER)).is_err() {
            return fail("spawner join timed out", k as u32 + 1);
        }
    }
    if PAR_FAILED.load(Ordering::Acquire) != 0 {
        return fail("a concurrent spawn or join", 0);
    }
    for k in 0..PAR_SPAWNERS {
        if WORKER[k].ran.load(Ordering::Acquire) != PAR_PER {
            return fail("a concurrent worker did not run", k as u32 + 1);
        }
    }
    let total = SEQ_SPAWNS + PAR_SPAWNERS as u32 * (PAR_PER + 1);
    say(&alloc::format!("thread-torture: {} concurrent spawns joined\n",
                        PAR_SPAWNERS as u32 * (PAR_PER + 1)));

    // Every thread but this one has exited and retired, so the live count is
    // exactly 1 -- not `total + 1`, which is what a lifetime count would read.
    match live_threads() {
        Some(1) => {}
        Some(n) => {
            say(&alloc::format!("thread-torture: FAIL live threads {} after {} spawns (expected 1)\n",
                                n, total));
            return 1;
        }
        None => {
            say("thread-torture: FAIL /proc status unreadable\n");
            return 1;
        }
    }
    say(&alloc::format!("thread-torture: ok ({} spawns, live threads 1)\n", total));
    0
}
