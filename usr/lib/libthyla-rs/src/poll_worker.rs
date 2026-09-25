//! Bounded one-shot readiness aggregation for a single owning event loop.
//!
//! No data reads, protocol parsing or authorization run on the worker. Owned
//! descriptors remain in retired slots until the worker has returned
//! from its previous poll, so a close/reuse cannot retarget a borrowed fd.
//! Every arm has a new ticket; stale poll results cannot acknowledge a re-arm.
//! The two private notification pipes each hold at most one byte, serialized
//! with their latch under the state mutex. Only these proven one-byte operations
//! run under that mutex. Poll, allocation, final close and join run outside it.

// All scheduling and fault hooks are absent from normal library builds.
#[cfg(feature = "poll-worker-test")]
pub mod qualification;

use crate::err::{Error, Result};
use crate::fs::File;
use crate::poll::AsFd;
use crate::sync::Mutex;
use crate::{thread, TPollFd, T_POLLERR, T_POLLHUP, T_POLLIN, T_POLLNVAL, T_POLLOUT};
use alloc_crate::alloc::{alloc, Layout};
use alloc_crate::boxed::Box;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use core::time::Duration;

/// One poll entry is reserved for configuration/shutdown notifications.
pub const MAX_WATCHES: usize = 63;
const STACK_BYTES: u64 = 64 * 1024;
const GUARD_BYTES: u64 = 4096;
/// Conservative user-address-space charge: stack+guard and bounded Shared box.
/// Kernel pipe buffers/handles and the kernel thread object are separate charges.
pub const MEMORY_RESERVE: u64 = STACK_BYTES + GUARD_BYTES + 4096;
static NEXT_OWNER: AtomicU64 = AtomicU64::new(0);
const EXCEPTIONS: i16 = T_POLLERR | T_POLLHUP | T_POLLNVAL;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct WatchId {
    owner: u64,
    slot: usize,
    generation: u64,
}

#[derive(Clone, Copy, Debug)]
pub struct Ready {
    pub watch: WatchId,
    pub events: i16,
}

pub struct ReadyBatch {
    events: [Option<Ready>; MAX_WATCHES],
}
impl ReadyBatch {
    pub fn iter(&self) -> impl Iterator<Item = &Ready> {
        self.events.iter().flatten()
    }
}

struct Slot {
    file: File,
    generation: u64,
    ticket: u64,
    interest: i16,
    ready: i16,
    retired: bool,
}

struct State {
    owner: u64,
    slots: [Option<Slot>; MAX_WATCHES],
    next: u64,
    started: bool,
    stopping: bool,
    failure: Option<Error>,
    command_pending: bool,
    notice_pending: bool,
}

impl State {
    fn ticket(&mut self) -> Result<u64> {
        let next = self.next.checked_add(1).ok_or(Error::NoSpace)?;
        self.next = next;
        Ok(next)
    }
    fn live(&self) -> Result<()> {
        if let Some(e) = self.failure {
            Err(e)
        } else if self.stopping {
            Err(Error::NotFound)
        } else {
            Ok(())
        }
    }
}

struct Shared {
    state: Mutex<State>,
    command_read: File,
    command_write: File,
    notice_read: File,
    notice_write: File,
    joined: AtomicU32,
    capacity: usize,
}

#[cfg(feature = "poll-worker-test")]
impl Drop for Shared {
    fn drop(&mut self) {
        qualification::context_dropped();
    }
}

// A latch is changed only while State is locked. A false latch means the pipe
// is empty: the consumer clears it only after reading its sole byte. Both pipe
// endpoints remain owned by Shared until kernel-confirmed thread exit.
fn signal(file: &File, pending: &mut bool) -> Result<()> {
    if !*pending {
        let byte = 1u8;
        let rc = unsafe { crate::t_write(file.as_raw_fd() as i64, &byte, 1) };
        if rc != 1 {
            return Err(if rc < 0 {
                Error::from_syscall_return(rc).unwrap_err()
            } else {
                Error::Io
            });
        }
        *pending = true;
    }
    Ok(())
}
fn consume(file: &File, pending: &mut bool) -> Result<()> {
    if *pending {
        let mut byte = 0u8;
        let rc = unsafe { crate::t_read(file.as_raw_fd() as i64, &mut byte, 1) };
        if rc != 1 {
            return Err(if rc < 0 {
                Error::from_syscall_return(rc).unwrap_err()
            } else {
                Error::Io
            });
        }
        *pending = false;
    }
    Ok(())
}

struct Stack(u64);
impl Stack {
    fn new() -> Result<Self> {
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::Reserve)?;
        let base = Error::from_syscall_return(unsafe {
            crate::t_burrow_reserve(STACK_BYTES + GUARD_BYTES, crate::T_BURROW_PROT_NONE, 0)
        })? as u64;
        let stack = Self(base);
        #[cfg(feature = "poll-worker-test")]
        {
            qualification::stack_created(base);
            qualification::fail(qualification::Fault::Protect)?;
        }
        Error::from_syscall_return(unsafe {
            crate::t_burrow_protect(
                base + GUARD_BYTES,
                STACK_BYTES,
                crate::T_BURROW_PROT_READ | crate::T_BURROW_PROT_WRITE,
                0,
            )
        })?;
        Ok(stack)
    }
}
impl Drop for Stack {
    fn drop(&mut self) {
        unsafe {
            crate::t_burrow_detach(self.0, STACK_BYTES + GUARD_BYTES);
        }
    }
}
struct Owned {
    shared: Box<Shared>,
    _stack: Stack,
}

/// A fixed-capacity, single-owner aggregator. Ready slots stay disarmed until
/// rearm; removing a slot reclaims it asynchronously and wakes this owner.
/// Call shutdown explicitly to observe errors. Drop also joins; if joining is
/// ambiguous, it retains the context and stack for process teardown.
pub struct PollWorker {
    owned: Option<Owned>,
}

impl PollWorker {
    pub fn new(capacity: usize) -> Result<Self> {
        if capacity == 0 || capacity > MAX_WATCHES {
            return Err(Error::InvalidArgument);
        }
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::CommandPipe)?;
        let (command_read, command_write) = crate::process::pipe()?;
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::NoticePipe)?;
        let (notice_read, notice_write) = crate::process::pipe()?;
        let owner = NEXT_OWNER
            .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1))
            .map_err(|_| Error::NoSpace)?
            + 1;
        let state = State {
            owner,
            slots: core::array::from_fn(|_| None),
            next: 0,
            started: false,
            stopping: false,
            failure: None,
            command_pending: false,
            notice_pending: false,
        };
        // Box::new aborts on allocation failure; constructors must unwind
        // already acquired pipes when the allocator can report refusal.
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::Context)?;
        let ptr = unsafe { alloc(Layout::new::<Shared>()) }.cast::<Shared>();
        if ptr.is_null() {
            return Err(Error::NoMemory);
        }
        let shared = unsafe {
            ptr.write(Shared {
                state: Mutex::new(state),
                command_read,
                command_write,
                notice_read,
                notice_write,
                joined: AtomicU32::new(1),
                capacity,
            });
            #[cfg(feature = "poll-worker-test")]
            qualification::context_created();
            Box::from_raw(ptr)
        };
        let stack = Stack::new()?;
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::Spawn)?;
        unsafe {
            thread::spawn_raw(
                worker_entry as *const () as u64,
                stack.0 + GUARD_BYTES + STACK_BYTES,
                (&*shared as *const Shared) as u64,
                0,
            )?;
        }
        // From successful spawn onward Drop must join before freeing either.
        let mut this = Self {
            owned: Some(Owned {
                shared,
                _stack: stack,
            }),
        };
        let mut fd = TPollFd {
            fd: this.as_raw_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        let rc = unsafe { crate::t_poll(&mut fd, 1, 5000) };
        if rc < 0 {
            return Err(Error::from_syscall_return(rc).unwrap_err());
        }
        if rc == 0 {
            return Err(Error::TimedOut);
        }
        this.take_ready()?;
        if !this.shared()?.state.lock().started {
            return Err(Error::Io);
        }
        Ok(this)
    }

    fn shared(&self) -> Result<&Shared> {
        self.owned
            .as_ref()
            .map(|o| &*o.shared)
            .ok_or(Error::NotFound)
    }

    /// Duplicate while file is borrowed, before publishing the owned fd. No
    /// raw descriptor is sent to the worker for a later, racy duplication.
    pub fn register(&mut self, file: &File, interest: i16) -> Result<WatchId> {
        valid_interest(interest)?;
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::Duplicate)?;
        self.register_owned(file.try_clone()?, interest)
    }

    /// Transfer the sole handle into the slot. Required for /srv endpoints and
    /// listeners, whose kernel ownership contract forbids dup. On refusal the
    /// consumed File is closed. UI I/O uses with_fd; removal retires the handle
    /// until the worker has returned from any poll that borrowed it.
    pub fn register_owned(&mut self, file: File, interest: i16) -> Result<WatchId> {
        valid_interest(interest)?;
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::Register)?;
        let shared = self.shared()?;
        let mut state = shared.state.lock();
        state.live()?;
        let slot = state.slots[..shared.capacity]
            .iter()
            .position(Option::is_none)
            .ok_or(Error::Busy)?;
        let generation = state.ticket()?;
        // Publish notification before the slot, under the same mutex; failure
        // leaves the table untouched; the consumed File drops after unlocking.
        signal(&shared.command_write, &mut state.command_pending)?;
        state.slots[slot] = Some(Slot {
            file,
            generation,
            ticket: generation,
            interest,
            ready: 0,
            retired: false,
        });
        Ok(WatchId {
            owner: state.owner,
            slot,
            generation,
        })
    }

    /// Borrow a live registration for one UI operation, with NO state lock
    /// held during the operation. The exclusive owner borrow prevents remove
    /// or shutdown. The worker only closes retired slots, never a live slot,
    /// including on worker failure. Thus the fd stays pinned until f returns.
    /// The closure must not close the fd or use a saved raw fd after returning;
    /// those operations require unsafe syscalls, just as for File::as_raw_fd.
    pub fn with_fd<R>(&mut self, id: WatchId, f: impl FnOnce(i32) -> R) -> Result<R> {
        let fd = {
            let shared = self.shared()?;
            let state = shared.state.lock();
            state.live()?;
            check_slot(&state, id)?;
            state.slots[id.slot].as_ref().unwrap().file.as_raw_fd()
        };
        Ok(f(fd))
    }

    /// Capacity includes retired slots until their old poll has returned.
    /// The sole owner uses this to suspend acceptance rather than accept and
    /// then discard a peer while descriptor reclamation is still pending.
    pub fn free_slots(&self) -> Result<usize> {
        let shared = self.shared()?;
        let state = shared.state.lock();
        state.live()?;
        Ok(state.slots[..shared.capacity]
            .iter()
            .filter(|s| s.is_none())
            .count())
    }

    /// The owner must have consumed any previously returned data itself. A new
    /// arm supersedes both latched readiness and in-flight old poll results.
    pub fn rearm(&mut self, id: WatchId, interest: i16) -> Result<()> {
        valid_interest(interest)?;
        let shared = self.shared()?;
        let mut state = shared.state.lock();
        state.live()?;
        check_slot(&state, id)?;
        let ticket = state.ticket()?;
        signal(&shared.command_write, &mut state.command_pending)?;
        let slot = state.slots[id.slot].as_mut().unwrap();
        slot.ticket = ticket;
        slot.interest = interest;
        slot.ready = 0;
        Ok(())
    }

    /// Retire immediately for result matching. The worker retains its fd until
    /// returning from poll; the next notice also reports reclaimed capacity.
    pub fn remove(&mut self, id: WatchId) -> Result<()> {
        let shared = self.shared()?;
        let mut state = shared.state.lock();
        state.live()?;
        check_slot(&state, id)?;
        signal(&shared.command_write, &mut state.command_pending)?;
        let slot = state.slots[id.slot].as_mut().unwrap();
        slot.retired = true;
        slot.ready = 0;
        slot.interest = 0;
        Ok(())
    }

    pub fn take_ready(&mut self) -> Result<ReadyBatch> {
        let shared = self.shared()?;
        let mut state = shared.state.lock();
        consume(&shared.notice_read, &mut state.notice_pending)?;
        state.live()?;
        let mut events = [None; MAX_WATCHES];
        let owner = state.owner;
        for (i, slot) in state.slots.iter_mut().enumerate() {
            if let Some(slot) = slot {
                if !slot.retired && slot.ready != 0 {
                    events[i] = Some(Ready {
                        watch: WatchId {
                            owner,
                            slot: i,
                            generation: slot.generation,
                        },
                        events: slot.ready,
                    });
                    slot.ready = 0;
                }
            }
        }
        Ok(ReadyBatch { events })
    }

    /// On error the object keeps all memory alive and can be shut down again.
    pub fn shutdown(&mut self) -> Result<()> {
        let Some(owned) = self.owned.as_ref() else {
            return Ok(());
        };
        let wake = {
            let shared = &owned.shared;
            let mut state = shared.state.lock();
            state.stopping = true;
            signal(&shared.command_write, &mut state.command_pending)
        };
        thread::join_tid(&owned.shared.joined, 1, Some(Duration::from_secs(5)))?;
        // A successful join makes all borrowed stack/context/fd references dead.
        self.owned.take();
        wake
    }
}
impl AsFd for PollWorker {
    fn as_raw_fd(&self) -> i32 {
        self.owned
            .as_ref()
            .map(|o| o.shared.notice_read.as_raw_fd())
            .unwrap_or(-1)
    }
}
impl Drop for PollWorker {
    fn drop(&mut self) {
        if self.shutdown().is_err() {
            if let Some(owned) = self.owned.take() {
                crate::t_putstr(
                    "poll-worker: join failed; retaining worker storage until process exit\n",
                );
                core::mem::forget(owned);
            }
        }
    }
}
fn valid_interest(interest: i16) -> Result<()> {
    if interest == 0 || interest & !(T_POLLIN | T_POLLOUT) != 0 {
        Err(Error::InvalidArgument)
    } else {
        Ok(())
    }
}
fn check_slot(state: &State, id: WatchId) -> Result<()> {
    if state.owner != id.owner {
        return Err(Error::NotFound);
    }
    match state.slots.get(id.slot).and_then(Option::as_ref) {
        Some(slot) if slot.generation == id.generation && !slot.retired => Ok(()),
        _ => Err(Error::NotFound),
    }
}

extern "C" fn worker_entry(arg: u64) -> ! {
    // Parent retains this Box and the stack until the kernel clears joined.
    let shared = unsafe { &*(arg as *const Shared) };
    thread::set_tid_address(&shared.joined);
    unsafe {
        crate::t_note_mask(1u64 << crate::T_NOTE_BIT_PIPE, core::ptr::null_mut());
    }
    let result = worker_loop(shared);
    {
        let mut state = shared.state.lock();
        if let Err(e) = result {
            state.failure = Some(e);
        }
        // Startup/exit share the ordinary notice latch. No silent worker exit.
        let _ = signal(&shared.notice_write, &mut state.notice_pending);
    }
    thread::exit_self()
}

fn worker_loop(shared: &Shared) -> Result<()> {
    #[cfg(feature = "poll-worker-test")]
    qualification::fail(qualification::Fault::Startup)?;
    {
        let mut state = shared.state.lock();
        state.started = true;
        signal(&shared.notice_write, &mut state.notice_pending)?;
    }
    loop {
        let mut fds = [TPollFd::default(); MAX_WATCHES + 1];
        let mut tickets = [(0usize, 0u64, 0u64); MAX_WATCHES];
        let mut retired: [Option<Slot>; MAX_WATCHES] = core::array::from_fn(|_| None);
        let mut count = 1;
        fds[0] = TPollFd {
            fd: shared.command_read.as_raw_fd(),
            events: T_POLLIN,
            revents: 0,
        };
        {
            let mut state = shared.state.lock();
            consume(&shared.command_read, &mut state.command_pending)?;
            if state.stopping {
                return Ok(());
            }
            let mut freed = false;
            for (i, entry) in state.slots.iter_mut().enumerate() {
                if entry.as_ref().is_some_and(|slot| slot.retired) {
                    retired[i] = entry.take();
                    freed = true;
                }
                if let Some(slot) = entry {
                    if slot.interest != 0 {
                        fds[count] = TPollFd {
                            fd: slot.file.as_raw_fd(),
                            events: slot.interest,
                            revents: 0,
                        };
                        tickets[count - 1] = (i, slot.generation, slot.ticket);
                        count += 1;
                    }
                }
            }
            if freed {
                signal(&shared.notice_write, &mut state.notice_pending)?;
            }
        }
        drop(retired); // close final duplicates outside the mutex and old poll
        #[cfg(feature = "poll-worker-test")]
        qualification::pause(qualification::Point::BeforePoll, &fds[1..count])?;
        let rc = unsafe { crate::t_poll(fds.as_mut_ptr(), count, -1) };
        #[cfg(feature = "poll-worker-test")]
        qualification::pause(qualification::Point::AfterPoll, &fds[1..count])?;
        #[cfg(feature = "poll-worker-test")]
        qualification::fail(qualification::Fault::Poll)?;
        Error::from_syscall_return(rc)?;
        if fds[0].revents & EXCEPTIONS != 0 {
            return Err(Error::Io);
        }
        let mut state = shared.state.lock();
        for n in 1..count {
            let (i, generation, ticket) = tickets[n - 1];
            if let Some(slot) = state.slots[i].as_mut() {
                if !slot.retired && slot.generation == generation && slot.ticket == ticket {
                    let events = fds[n].revents & (slot.interest | EXCEPTIONS);
                    if events != 0 {
                        slot.ready |= events;
                        slot.interest = 0;
                    }
                }
            }
        }
        if state.slots.iter().flatten().any(|s| s.ready != 0) {
            signal(&shared.notice_write, &mut state.notice_pending)?;
        }
    }
}

// Fixed allocation ledger: payload buffers remain with the caller. Poll arrays
// and retired slots live on the worker's 64 KiB stack, behind its 4 KiB guard.
// The UI returns at most one fixed batch; neither side grows a readiness queue.
const _: () = {
    assert!(core::mem::size_of::<Shared>() <= 4096);
    assert!(core::mem::size_of::<ReadyBatch>() <= 4096);
    assert!(core::mem::size_of::<PollWorker>() <= 32);
};
