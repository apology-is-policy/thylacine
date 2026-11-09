//! The input thread. It moves raw reports from every eventq into one bounded queue, so a
//! serve loop held in a synchronous GPU wait leaves no ring undrained (vote 72, GPU-DESIGN
//! 4.5.15). Seat logic stays on the serve thread: this thread never calls the seat
//! endpoint, never prints, and never decides what a key means.
//!
//! An eventq holds 64 reports, and a report the device cannot place is lost. The queue
//! holds 4096, and an entry that finds it full is lost too, but counted: the serve thread
//! learns which device lost one, at the point in the order where it was lost, and releases
//! that device's held keys there.
use super::input::{InputDev, Report};
use alloc::{boxed::Box, collections::VecDeque, vec::Vec};
use core::time::Duration;
use libthyla_rs::{sync::Mutex, t_burrow_attach, thread};

pub const CAPACITY: usize = 4096;
/// As many as the backend has input windows for (main.rs).
pub const MAX_DEVICES: usize = 3;
/// An eventq of 64 overruns only past 16,000 reports a second at this cadence.
const POLL_MS: u64 = 4;
const STACK: u64 = 64 * 1024;

pub enum Entry {
    Report(Report),
    /// The device may have dropped a report before this point: the eventq ran dry.
    Overran,
}

struct Queue {
    entries: VecDeque<(u8, Entry)>,
    /// The queue was full when this device had something to add. Set only while full and
    /// taken with the entries, so the loss always follows everything taken with it.
    lost: [bool; MAX_DEVICES],
}

static QUEUE: Mutex<Queue> = Mutex::new(Queue {
    entries: VecDeque::new(),
    lost: [false; MAX_DEVICES],
});

struct Reader {
    devices: Vec<InputDev>,
    batch: Vec<(u8, Entry)>,
}

/// Hand the devices to a new thread that drains them for the life of the process. Nothing
/// joins it: `t_exit_group` ends it with the process, and no device memory is released
/// before then.
pub fn start(devices: Vec<InputDev>) -> Result<(), libdriver::Error> {
    if devices.is_empty() || devices.len() > MAX_DEVICES {
        return Err(libdriver::Error::Hardware);
    }
    QUEUE.lock().entries.reserve_exact(CAPACITY);
    // SAFETY: a fresh anonymous region of STACK bytes, owned by the thread for its life.
    let stack = unsafe { t_burrow_attach(STACK) };
    if stack < 0 {
        return Err(libdriver::Error::Hardware);
    }
    // AAPCS64 wants the stack top 16-aligned: the region is page-aligned and STACK a
    // page multiple.
    let sp = stack as u64 + STACK;
    let reader = Reader {
        devices,
        batch: Vec::with_capacity(2 * crate::eventq::DEPTH_MAX as usize),
    };
    let arg = Box::into_raw(Box::new(reader)) as u64;
    // SAFETY: `read` is a C-ABI function of one u64 that never returns; `arg` is the
    // leaked Box, which only that thread reads from here on.
    match unsafe { thread::spawn_raw(read as *const () as u64, sp, arg, 0) } {
        Ok(_) => Ok(()),
        Err(_) => {
            // The thread never ran, so nothing else reaches the Box.
            drop(unsafe { Box::from_raw(arg as *mut Reader) });
            Err(libdriver::Error::Hardware)
        }
    }
}

/// Everything queued since the last call, in order, swapped into `into`, which must be
/// empty; and per device, whether a report was lost after all of it.
pub fn take(into: &mut VecDeque<(u8, Entry)>) -> [bool; MAX_DEVICES] {
    debug_assert!(into.is_empty());
    // The deque handed back is the one the thread fills next: give it room so the
    // thread does not allocate.
    if into.capacity() < CAPACITY {
        into.reserve_exact(CAPACITY);
    }
    let mut queue = QUEUE.lock();
    core::mem::swap(&mut queue.entries, into);
    core::mem::take(&mut queue.lost)
}

extern "C" fn read(arg: u64) {
    // SAFETY: `arg` is the Box leaked by `start` for this thread alone.
    let reader = unsafe { &mut *(arg as *mut Reader) };
    loop {
        #[cfg(feature = "test-mode")]
        if test::stopped() {
            let _ = libthyla_rs::time::sleep(Duration::from_millis(1));
            continue;
        }
        for (index, device) in reader.devices.iter_mut().enumerate() {
            let batch = &mut reader.batch;
            batch.clear();
            let overran = device.drain(|report| batch.push((index as u8, Entry::Report(report))));
            if overran {
                batch.push((index as u8, Entry::Overran));
            }
            // Stop before anything typed after the key leaves the ring.
            #[cfg(feature = "test-mode")]
            test::stop_at_key(batch);
            if !batch.is_empty() {
                push(batch);
            }
        }
        let _ = libthyla_rs::time::sleep(Duration::from_millis(POLL_MS));
    }
}

fn push(batch: &mut Vec<(u8, Entry)>) {
    let mut queue = QUEUE.lock();
    for (index, entry) in batch.drain(..) {
        if queue.entries.len() < CAPACITY {
            queue.entries.push_back((index, entry));
        } else if !matches!(
            entry,
            Entry::Report(Report::Low { .. } | Report::Malformed { .. })
        ) {
            // A lost note is a lost line of diagnostics; a lost event or overrun is a
            // release that may never come.
            queue.lost[index as usize] = true;
        }
    }
}

/// ls-graphical-keys-overrun's lever, input half. Under the armed latch, the blind key
/// stops this thread at the pass that read it, as a synchronous wait stopped the serve
/// loop when the serve loop drained the rings; the serve loop resumes it when its own
/// stop ends (`Device::test_blind_loop`).
#[cfg(feature = "test-mode")]
pub mod test {
    use super::{Entry, Report};
    use crate::backend::input::EV_KEY;
    use core::sync::atomic::{AtomicBool, Ordering};

    /// F8.
    pub const BLIND_KEY: u16 = 66;
    static ARMED: AtomicBool = AtomicBool::new(false);
    static STOPPED: AtomicBool = AtomicBool::new(false);

    pub fn arm() {
        ARMED.store(true, Ordering::Release);
    }
    pub fn resume() {
        STOPPED.store(false, Ordering::Release);
    }
    pub(super) fn stopped() -> bool {
        STOPPED.load(Ordering::Acquire)
    }
    /// Set before the batch is queued, so the serve loop, which resumes this thread only
    /// after it has read the key, can never resume it first.
    pub(super) fn stop_at_key(batch: &[(u8, Entry)]) {
        if !ARMED.load(Ordering::Acquire) {
            return;
        }
        let pressed = batch.iter().any(|(_, entry)| matches!(entry,
            Entry::Report(Report::Event(e)) if e.etype == EV_KEY && e.code == BLIND_KEY && e.value == 1));
        if pressed {
            STOPPED.store(true, Ordering::Release);
        }
    }
}
