// libthyla-rs::sync -- thread synchronization primitives for native
// multi-threaded Procs.
//
// A `spawn_raw` thread (thread.rs) shares its parent's address space,
// handle table, and Territory; only the register file differs. So an
// intra-Proc lock is a plain futex: an `AtomicU32` the threads CAS,
// backed by `torpor::{wait,wake}` for the parked case. That is exactly
// what `Mutex<T>` here is -- the three-state futex mutex (Drepper's
// "Futexes Are Tricky", the same shape Rust std uses on Linux), plus a
// non-blocking `try_lock` for a realtime thread that must never sleep.
//
// WHY three states and not a spinlock: nocturned's cycle thread (the
// first consumer, docs/NOCTURNE.md D-1c) is the realtime audio clock and
// must not busy-wait on the control thread; the control thread must not
// busy-wait on the cycle. A parked waiter costs nothing, and `try_lock`
// lets the cycle decline the lock outright ("graph edit in progress, run
// last cycle's plan") instead of ever blocking on it.
//
// WHY not a std-style `Mutex` with poisoning: no_std, no unwinding; a
// panic in this OS terminates the Proc, so there is nothing to poison.

use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::ops::{Deref, DerefMut};
use core::sync::atomic::{AtomicU32, Ordering};

use crate::torpor;

// Lock word states. The ordering of CONTENDED above LOCKED matters only
// to the `swap`-to-CONTENDED step below; the values themselves are the
// conventional three.
const UNLOCKED: u32 = 0;
const LOCKED: u32 = 1; // held; no thread is known to be parked
const CONTENDED: u32 = 2; // held; at least one thread may be parked on the word

/// A mutual-exclusion lock over `T`, usable across the threads of one
/// Proc (threads share the address space, so `&Mutex<T>` is the shared
/// handle). Acquire it blockingly with [`Mutex::lock`], or without ever
/// sleeping with [`Mutex::try_lock`].
pub struct Mutex<T: ?Sized> {
    // The futex word. UNLOCKED / LOCKED / CONTENDED per the constants.
    state: AtomicU32,
    data: UnsafeCell<T>,
}

// A `&Mutex<T>` may be shared across threads iff `T` can itself move
// across a thread boundary: the guard hands out `&mut T`, i.e. exclusive
// access that migrates to whichever thread holds the lock. `T: Sync` is
// NOT required because access is serialized -- two threads never hold
// `&T` at once. This mirrors std's `Mutex` bounds.
unsafe impl<T: ?Sized + Send> Send for Mutex<T> {}
unsafe impl<T: ?Sized + Send> Sync for Mutex<T> {}

impl<T> Mutex<T> {
    /// A new, unlocked mutex owning `data`. `const` so a `Mutex` can be
    /// a `static` (the usual home for a cross-thread lock whose lifetime
    /// is the Proc).
    pub const fn new(data: T) -> Mutex<T> {
        Mutex {
            state: AtomicU32::new(UNLOCKED),
            data: UnsafeCell::new(data),
        }
    }

    /// Consume the mutex and return the owned data. No lock is taken --
    /// `self` by value proves there are no outstanding borrows.
    pub fn into_inner(self) -> T {
        self.data.into_inner()
    }
}

impl<T: ?Sized> Mutex<T> {
    /// Acquire without ever sleeping. `Some(guard)` if the lock was free
    /// and is now held by the caller; `None` if it was held by anyone
    /// (LOCKED or CONTENDED) -- the caller must not block, so it gets an
    /// immediate refusal. This is the realtime path.
    ///
    /// Only the UNLOCKED -> LOCKED edge is taken: a CONTENDED word is
    /// already held, so failing to CAS from UNLOCKED is exactly "held",
    /// and leaving a waiter's CONTENDED marker untouched is correct.
    pub fn try_lock(&self) -> Option<MutexGuard<'_, T>> {
        if self
            .state
            .compare_exchange(UNLOCKED, LOCKED, Ordering::Acquire, Ordering::Relaxed)
            .is_ok()
        {
            Some(MutexGuard::new(self))
        } else {
            None
        }
    }

    /// Acquire, parking on the futex until the lock is free. Returns a
    /// guard that releases on drop.
    pub fn lock(&self) -> MutexGuard<'_, T> {
        // Uncontended fast path: claim an UNLOCKED word as LOCKED (no
        // waiter marker, so the matching unlock skips the wake syscall).
        if self
            .state
            .compare_exchange(UNLOCKED, LOCKED, Ordering::Acquire, Ordering::Relaxed)
            .is_err()
        {
            self.lock_contended();
        }
        MutexGuard::new(self)
    }

    #[cold]
    fn lock_contended(&self) {
        loop {
            // If the word is free, take it AS CONTENDED: we reached the
            // slow path, so another thread may already be parked, and the
            // holder must wake it on release. A false CONTENDED (no one is
            // actually parked) costs at most one spurious wake later.
            let prev = self
                .state
                .compare_exchange(UNLOCKED, CONTENDED, Ordering::Acquire, Ordering::Relaxed)
                .unwrap_or_else(|e| e);
            if prev == UNLOCKED {
                return; // acquired
            }
            // Held. Ensure the word reads CONTENDED so whoever holds it
            // will wake us, then park while it stays CONTENDED. If the
            // holder released between our observation and the swap, `swap`
            // returns UNLOCKED and we have in fact acquired it.
            if self.state.swap(CONTENDED, Ordering::Acquire) == UNLOCKED {
                return; // acquired
            }
            // torpor::wait is register-then-observe: if the word no longer
            // reads CONTENDED at the syscall boundary it returns without
            // parking, so no wake is lost between the swap and here. Any
            // return (woken / value-changed / the kernel's own timeout)
            // just re-drives the loop.
            let _ = torpor::wait(&self.state, CONTENDED, None);
        }
    }

    /// Release. Called only by `MutexGuard::drop`.
    fn unlock(&self) {
        // Drop the lock and learn whether a waiter must be woken in one
        // step: only a CONTENDED word had (possibly) a parked thread.
        if self.state.swap(UNLOCKED, Ordering::Release) == CONTENDED {
            let _ = torpor::wake_one(&self.state);
        }
    }

    /// Borrow the data with no lock, proven safe by the `&mut self`
    /// exclusive borrow (no other reference can exist). For single-thread
    /// setup before any peer thread is spawned.
    pub fn get_mut(&mut self) -> &mut T {
        // SAFETY: `&mut self` is unique, so no guard or peer can alias.
        unsafe { &mut *self.data.get() }
    }
}

/// The RAII lock guard. Dereferences to the protected `T`; releasing the
/// lock happens on drop. Not `Send`/`Sync`: a guard is bound to the
/// thread that acquired it and must be dropped there (releasing on a
/// different thread would wake the wrong futex owner).
pub struct MutexGuard<'a, T: ?Sized> {
    lock: &'a Mutex<T>,
    // Removes the auto `Send`/`Sync` that `&Mutex<T>` would otherwise
    // grant: a raw pointer is neither.
    _not_send: PhantomData<*const ()>,
}

impl<'a, T: ?Sized> MutexGuard<'a, T> {
    fn new(lock: &'a Mutex<T>) -> MutexGuard<'a, T> {
        MutexGuard {
            lock,
            _not_send: PhantomData,
        }
    }
}

impl<T: ?Sized> Deref for MutexGuard<'_, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: holding the guard is holding the lock, so this is the
        // only live reference to the data.
        unsafe { &*self.lock.data.get() }
    }
}

impl<T: ?Sized> DerefMut for MutexGuard<'_, T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: as Deref; `&mut self` on the guard makes it exclusive.
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<T: ?Sized> Drop for MutexGuard<'_, T> {
    fn drop(&mut self) {
        self.lock.unlock();
    }
}
