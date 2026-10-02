//! One persistent service owner with a guarded stack and kernel-confirmed join.
//! The parent owns context/stack; the child only borrows them until clear-tid.
//! No protocol or authority lives here. Callers supply bounded, nonblocking
//! service loops that poll Control::stop_fd and never borrow graphics state.
use crate::{
    err::{Error, Result},
    fs::File,
    thread,
};
use alloc_crate::{
    alloc::{alloc, Layout},
    boxed::Box,
};
use core::{
    sync::atomic::{AtomicBool, AtomicI32, AtomicU32, Ordering},
    time::Duration,
};
pub const STACK_BYTES: u64 = 128 * 1024;
pub const GUARD_BYTES: u64 = 4096;
struct Stack(u64);
impl Stack {
    fn new() -> Result<Self> {
        let va = Error::from_syscall_return(unsafe {
            crate::t_burrow_reserve(STACK_BYTES + GUARD_BYTES, crate::T_BURROW_PROT_NONE, 0)
        })? as u64;
        let s = Self(va);
        Error::from_syscall_return(unsafe {
            crate::t_burrow_protect(
                va + GUARD_BYTES,
                STACK_BYTES,
                crate::T_BURROW_PROT_READ | crate::T_BURROW_PROT_WRITE,
                0,
            )
        })?;
        Ok(s)
    }
}
impl Drop for Stack {
    fn drop(&mut self) {
        unsafe {
            crate::t_burrow_detach(self.0, STACK_BYTES + GUARD_BYTES);
        }
    }
}
pub struct Control {
    stop_read: File,
    stop_write: File,
    notice_read: File,
    notice_write: File,
    stopping: AtomicBool,
    status: AtomicI32,
    joined: AtomicU32,
}
impl Control {
    /// Notify setup completion only after all service resources are installed.
    pub fn ready(&self) -> Result<()> {
        self.status
            .compare_exchange(0, 1, Ordering::Release, Ordering::Relaxed)
            .map_err(|_| Error::InvalidArgument)?;
        self.notify()
    }
    pub fn notify(&self) -> Result<()> {
        let b = 1;
        let rc = unsafe { crate::t_write(self.notice_write.as_raw_fd() as i64, &b, 1) };
        if rc == 1 || rc == -11 {
            Ok(())
        } else {
            Err(Error::Io)
        }
    }
    /// Wake hints are not commands: state is retained in the bounded mailbox.
    pub fn drain_wake(&self) -> Result<()> {
        #[cfg(feature = "poll-worker-test")]
        if qualification::take(qualification::Fault::Service) {
            return Err(Error::NoMemory);
        }
        let mut b = [0u8; 64];
        let n =
            unsafe { crate::t_read(self.stop_read.as_raw_fd() as i64, b.as_mut_ptr(), b.len()) };
        if n >= 0 || n == -11 {
            Ok(())
        } else {
            Err(Error::Io)
        }
    }
    pub fn stopping(&self) -> bool {
        self.stopping.load(Ordering::Acquire)
    }
    pub fn stop_fd(&self) -> i32 {
        self.stop_read.as_raw_fd()
    }
}
struct Shared<S> {
    state: S,
    control: Control,
    run: fn(&S, &Control) -> Result<()>,
}
struct Owned<S> {
    shared: Box<Shared<S>>,
    _stack: Stack,
}
pub struct ServiceWorker<S: Send + Sync + 'static> {
    owned: Option<Owned<S>>,
}
impl<S: Send + Sync + 'static> ServiceWorker<S> {
    /// Setup waits at most five seconds. The service's normal operation does
    /// not make the graphics owner wait for protocol replies.
    pub fn new(state: S, run: fn(&S, &Control) -> Result<()>) -> Result<Self> {
        #[cfg(feature = "poll-worker-test")]
        if qualification::take(qualification::Fault::Startup) {
            return Err(Error::NoMemory);
        }
        let (stop_read, stop_write) = crate::process::pipe()?;
        let (notice_read, notice_write) = crate::process::pipe()?;
        // Coalesced wake/notice bytes carry no data. Durable caller state is
        // checked after draining; full pipes already supply a wake.
        for f in [&stop_read, &stop_write, &notice_write, &notice_read] {
            Error::from_syscall_return(unsafe {
                crate::t_set_nonblock(f.as_raw_fd() as i64, true)
            })?;
        }
        let ptr = unsafe { alloc(Layout::new::<Shared<S>>()) }.cast::<Shared<S>>();
        if ptr.is_null() {
            return Err(Error::NoMemory);
        }
        let shared = unsafe {
            ptr.write(Shared {
                state,
                control: Control {
                    stop_read,
                    stop_write,
                    notice_read,
                    notice_write,
                    stopping: AtomicBool::new(false),
                    status: AtomicI32::new(0),
                    joined: AtomicU32::new(1),
                },
                run,
            });
            Box::from_raw(ptr)
        };
        let stack = Stack::new()?;
        unsafe {
            thread::spawn_raw(
                entry::<S> as *const () as u64,
                stack.0 + GUARD_BYTES + STACK_BYTES,
                (&*shared as *const Shared<S>) as u64,
                0,
            )?;
        }
        let this = Self {
            owned: Some(Owned {
                shared,
                _stack: stack,
            }),
        };
        let mut p = crate::TPollFd {
            fd: this.notice_fd(),
            events: crate::T_POLLIN,
            revents: 0,
        };
        let n = Error::from_syscall_return(unsafe { crate::t_poll(&mut p, 1, 5000) })?;
        if n == 0 {
            return Err(Error::TimedOut);
        }
        this.check()?;
        Ok(this)
    }
    pub fn state(&self) -> Result<&S> {
        self.owned
            .as_ref()
            .map(|o| &o.shared.state)
            .ok_or(Error::NotFound)
    }
    pub fn notice_fd(&self) -> i32 {
        self.owned
            .as_ref()
            .map(|o| o.shared.control.notice_read.as_raw_fd())
            .unwrap_or(-1)
    }
    /// Drain a bounded batch of wake hints and observe current service state.
    pub fn check(&self) -> Result<()> {
        let o = self.owned.as_ref().ok_or(Error::NotFound)?;
        let mut notices = [0u8; 64];
        let n = unsafe {
            crate::t_read(
                o.shared.control.notice_read.as_raw_fd() as i64,
                notices.as_mut_ptr(),
                notices.len(),
            )
        };
        if n < 0 && n != -11 {
            return Err(Error::from_syscall_return(n).unwrap_err());
        }
        if n == 0 {
            return Err(Error::Io);
        }
        match o.shared.control.status.load(Ordering::Acquire) {
            1 => Ok(()),
            0 => Err(Error::WouldBlock),
            n if n < 0 => Err(Error::from_syscall_return(n as i64).unwrap_err()),
            _ => Err(Error::Io),
        }
    }
    pub fn wake(&self) -> Result<()> {
        let o = self.owned.as_ref().ok_or(Error::NotFound)?;
        let b = 1u8;
        let n = unsafe { crate::t_write(o.shared.control.stop_write.as_raw_fd() as i64, &b, 1) };
        if n == 1 || n == -11 {
            Ok(())
        } else {
            Err(Error::Io)
        }
    }
    pub fn shutdown(&mut self) -> Result<()> {
        let Some(o) = self.owned.as_ref() else {
            return Ok(());
        };
        let mut wake = Ok(());
        if !o.shared.control.stopping.swap(true, Ordering::AcqRel) {
            let b = 1;
            let n =
                unsafe { crate::t_write(o.shared.control.stop_write.as_raw_fd() as i64, &b, 1) };
            if n != 1 && n != -11 {
                wake = Err(Error::Io);
            }
        }
        thread::join_tid(&o.shared.control.joined, 1, Some(Duration::from_secs(5)))?;
        self.owned.take();
        wake
    }
}
impl<S: Send + Sync + 'static> Drop for ServiceWorker<S> {
    fn drop(&mut self) {
        if self.shutdown().is_err() {
            if let Some(o) = self.owned.take() {
                // No console logging here: console writes park during SAK.
                // Explicit shutdown returns the join error to the owner.
                core::mem::forget(o);
            }
        }
    }
}
extern "C" fn entry<S: Send + Sync + 'static>(arg: u64) -> ! {
    // The parent retains Shared and stack until the kernel clears joined.
    let s = unsafe { &*(arg as *const Shared<S>) };
    thread::set_tid_address(&s.control.joined);
    unsafe {
        crate::t_note_mask(1u64 << crate::T_NOTE_BIT_PIPE, core::ptr::null_mut());
    }
    let status = match (s.run)(&s.state, &s.control) {
        Ok(()) => 2,
        Err(e) => -e.as_errno(),
    };
    s.control.status.store(status, Ordering::Release);
    let _ = s.control.notify();
    thread::exit_self()
}

/// Explicit qualification images only; never armed in production.
#[cfg(feature = "poll-worker-test")]
pub mod qualification {
    use core::sync::atomic::{AtomicU32, Ordering};
    #[repr(u32)]
    #[derive(Clone, Copy)]
    pub enum Fault {
        Startup = 1,
        Published = 2,
        Service = 3,
    }
    static FAULT: AtomicU32 = AtomicU32::new(0);
    pub fn fail_next(f: Fault) {
        FAULT.store(f as u32, Ordering::Release);
    }
    pub fn take(f: Fault) -> bool {
        FAULT
            .compare_exchange(f as u32, 0, Ordering::AcqRel, Ordering::Acquire)
            .is_ok()
    }
}
/// Called at the posting service's publication boundary, not a generic worker.
pub fn qualify_published() -> Result<()> {
    #[cfg(feature = "poll-worker-test")]
    if qualification::take(qualification::Fault::Published) {
        return Err(Error::NoMemory);
    }
    Ok(())
}
