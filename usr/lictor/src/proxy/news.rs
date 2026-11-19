//! The compositor's half of the broker's `news` file (news.rs). A thread of its
//! own parks a read of it and wakes a local pipe, so a serve loop polls a local
//! descriptor and calls the broker only for what changed. A readiness file would
//! not do: the kernel samples a remote file with a round trip on every pass of
//! every poll, so the loop would pay one per pass instead of one per change.
use alloc::boxed::Box;
use core::sync::atomic::{AtomicBool, AtomicU32, Ordering};
use libdriver::Error;
use libthyla_rs::{t_burrow_attach, t_close, t_pipe, t_poll, t_pread, t_read, t_set_nonblock, t_thread_exit, t_write,
    thread, TPollFd, T_OREAD, T_POLLIN};
pub use crate::news::{ALL, GPU, INPUT, SEAT};
const STACK: u64 = 16 * 1024;
/// POSIX EINTR (ERRORS.md).
const E_INTR: i64 = 4;
struct Shared { file: i64, wake: i64, interest: u32, bits: AtomicU32, failed: AtomicBool }
/// Nothing joins the thread: it reads for the life of the process.
pub struct Watch { wake: i64, shared: &'static Shared }
impl Watch {
    /// Watch `interest`. All of it is pending at first, so the first take reads
    /// every answer once; news from before the broker accepted the connection is
    /// covered by that.
    pub fn start(interest: u32) -> Result<Self, Error> {
        if crate::news::interest(interest as u64).is_none() { return Err(Error::BadField); }
        let file = crate::rpc_client::open(b"news", T_OREAD)?;
        // SAFETY: descriptor calls on a pair this function owns. Both ends are
        // nonblocking: a full pipe already wakes the loop, and a take must not park.
        let (wake_r, wake_w) = unsafe { t_pipe() };
        let ready = wake_r >= 0 && unsafe { t_set_nonblock(wake_r, true) } == 0
            && unsafe { t_set_nonblock(wake_w, true) } == 0;
        // SAFETY: a fresh anonymous region of STACK bytes, owned by the thread for its life.
        let stack = if ready { unsafe { t_burrow_attach(STACK) } } else { -1 };
        if stack < 0 {
            unsafe { t_close(file); if wake_r >= 0 { t_close(wake_r); t_close(wake_w); } }
            return Err(Error::Hardware);
        }
        let shared = Box::into_raw(Box::new(Shared {
            file, wake: wake_w, interest, bits: AtomicU32::new(interest), failed: AtomicBool::new(false),
        }));
        // SAFETY: `watch` is a C-ABI function of one u64 that never returns; its
        // argument is the Shared, leaked for the life of the process once the thread
        // runs. The stack top is 16-aligned: the region is page-aligned and STACK a
        // page multiple.
        if unsafe { thread::spawn_raw(watch as *const () as u64, stack as u64 + STACK, shared as u64, 0) }.is_err() {
            // The thread never ran, so nothing else reaches the Shared or the
            // descriptors; the stack region goes with the process.
            drop(unsafe { Box::from_raw(shared) });
            unsafe { t_close(file); t_close(wake_r); t_close(wake_w); }
            return Err(Error::Hardware);
        }
        // SAFETY: from here the Shared is never freed.
        Ok(Self { wake: wake_r, shared: unsafe { &*shared } })
    }
    /// The descriptor to poll: readable once there is news to take.
    pub fn fd(&self) -> i32 { self.wake as i32 }
    /// Everything heard since the last take. The pipe is emptied first, so a byte
    /// left in it always stands for bits a later take will find.
    pub fn take(&self) -> u32 {
        let mut bytes = [0u8; 64];
        // SAFETY: a nonblocking read into a local buffer; it stops at EAGAIN or EOF.
        while unsafe { t_read(self.wake, bytes.as_mut_ptr(), bytes.len()) } > 0 {}
        self.shared.bits.swap(0, Ordering::AcqRel)
    }
    /// The read failed: no more news will come. Every class was made pending once.
    pub fn failed(&self) -> bool { self.shared.failed.load(Ordering::Acquire) }
    /// Park until there is news to take.
    pub fn wait(&self) {
        let mut fds = [TPollFd { fd: self.fd(), events: T_POLLIN, revents: 0 }];
        // SAFETY: one pollfd on the stack.
        unsafe { t_poll(fds.as_mut_ptr(), fds.len(), -1); }
    }
}
extern "C" fn watch(arg: u64) {
    // SAFETY: `arg` is the Shared leaked by `start`, alive for the process.
    let s = unsafe { &*(arg as *const Shared) };
    loop {
        let mut reply = [0u8; 4];
        // SAFETY: a read into a local buffer; the offset names the interest.
        let n = unsafe { t_pread(s.file, reply.as_mut_ptr(), reply.len(), s.interest as i64) };
        // A note can interrupt the read after the broker answered it, and the
        // answer is gone with it: everything is pending again, and reading goes on.
        let interrupted = n == -E_INTR;
        let failed = n != 4 && !interrupted;
        let bits = if n == 4 { u32::from_le_bytes(reply) & s.interest } else { s.interest };
        if failed { s.failed.store(true, Ordering::Release); }
        s.bits.fetch_or(bits, Ordering::Release);
        // After the bits, so the woken take finds them. Refused only while the pipe
        // is full, which means a byte already waits.
        // SAFETY: one byte to the nonblocking write end.
        unsafe { t_write(s.wake, [1u8].as_ptr(), 1); }
        if failed { unsafe { t_thread_exit() } }
    }
}
