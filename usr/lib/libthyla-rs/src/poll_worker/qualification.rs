//! Opt-in native qualification hooks. No production ABI or default feature.
//!
//! The single test owner can suspend one matching worker poll outside State's
//! mutex. Real pipes, descriptors, polls, memory and joins remain in use.
//! A gate owns the pause and releases it on Drop, including test failure. Both
//! sides have bounded waits so a broken schedule produces a named failure.
use crate::err::{Error, Result};
use crate::TPollFd;
use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};

#[repr(u32)]
#[derive(Clone, Copy)]
pub enum Point {
    BeforePoll = 1,
    AfterPoll = 2,
}
#[repr(u32)]
#[derive(Clone, Copy)]
pub enum Fault {
    CommandPipe = 1,
    NoticePipe,
    Context,
    Reserve,
    Protect,
    Spawn,
    Startup,
    Duplicate,
    Register,
    Poll,
}
static FAULT: AtomicU32 = AtomicU32::new(0);
static CONTEXTS: AtomicU32 = AtomicU32::new(0);
static STACK: AtomicU64 = AtomicU64::new(0);
static STATE: AtomicU32 = AtomicU32::new(0); // idle, armed, arrived, released
static POINT: AtomicU32 = AtomicU32::new(0);
static FD: AtomicI32 = AtomicI32::new(-1);

pub fn fail_next(fault: Fault) {
    FAULT.store(fault as u32, Ordering::Release);
}
pub fn fault_consumed() -> bool {
    FAULT.load(Ordering::Acquire) == 0
}
pub(super) fn fail(fault: Fault) -> Result<()> {
    if FAULT
        .compare_exchange(fault as u32, 0, Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
    {
        Err(Error::NoMemory)
    } else {
        Ok(())
    }
}
pub fn live_contexts() -> u32 {
    CONTEXTS.load(Ordering::Acquire)
}
pub(super) fn context_created() {
    CONTEXTS.fetch_add(1, Ordering::AcqRel);
}
pub(super) fn context_dropped() {
    CONTEXTS.fetch_sub(1, Ordering::AcqRel);
}
pub fn last_stack() -> u64 {
    STACK.load(Ordering::Acquire)
}
pub(super) fn stack_created(base: u64) {
    STACK.store(base, Ordering::Release);
}
fn deadline() -> u64 {
    crate::time::monotonic_ns().saturating_add(3_000_000_000)
}
fn wait_until(mut done: impl FnMut() -> bool) -> Result<()> {
    let end = deadline();
    while !done() {
        if crate::time::monotonic_ns() >= end {
            return Err(Error::TimedOut);
        }
        crate::t_yield();
    }
    Ok(())
}

/// Only one test owner/gate at a time. No production caller can name this type.
pub struct Gate;
impl Gate {
    pub fn arm(point: Point, fd: i32) -> Result<Self> {
        wait_until(|| STATE.load(Ordering::Acquire) == 0)?;
        POINT.store(point as u32, Ordering::Relaxed);
        FD.store(fd, Ordering::Relaxed);
        STATE.store(1, Ordering::Release);
        Ok(Self)
    }
    pub fn arrived(&self) -> Result<()> {
        wait_until(|| STATE.load(Ordering::Acquire) == 2)
    }
    pub fn release(self) -> Result<()> {
        drop(self);
        wait_until(|| STATE.load(Ordering::Acquire) == 0)
    }
}
impl Drop for Gate {
    fn drop(&mut self) {
        // Cancel an unclaimed gate, or release the worker that claimed it.
        if STATE
            .compare_exchange(1, 0, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            let _ = STATE.compare_exchange(2, 3, Ordering::AcqRel, Ordering::Acquire);
        }
    }
}
pub(super) fn pause(point: Point, fds: &[TPollFd]) -> Result<()> {
    if STATE.load(Ordering::Acquire) != 1
        || POINT.load(Ordering::Relaxed) != point as u32
        || !fds.iter().any(|p| p.fd == FD.load(Ordering::Relaxed))
        || STATE
            .compare_exchange(1, 2, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
    {
        return Ok(());
    }
    let result = wait_until(|| STATE.load(Ordering::Acquire) != 2);
    STATE.store(0, Ordering::Release);
    result
}
