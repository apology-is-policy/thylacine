//! Whether recursion on input must stop.
//!
//! The parser and the evaluator each count their own levels, but those counts multiply and a
//! frame's size is the compiler's, so no count says how much stack a line uses. Every cycle of
//! their recursion also asks the stack.

/// Left free below a check: the deepest call chain that meets no further check, a builtin's
/// included. A drop of a tree does not run this low, as no statement runs beneath an
/// expression, and the largest drop measured would still fit here.
const RESERVE: usize = 1024 * 1024;

#[cfg(test)]
extern crate std;

#[cfg(test)]
std::thread_local! {
    static TEST_BASE: core::cell::Cell<usize> = const { core::cell::Cell::new(0) };
}

#[cfg(test)]
fn base() -> usize {
    TEST_BASE.with(|b| b.get())
}

#[cfg(all(not(test), feature = "backend"))]
fn base() -> usize {
    libthyla_rs::stack_base()
}

#[cfg(all(not(test), not(feature = "backend")))]
fn base() -> usize {
    0
}

fn here() -> usize {
    let probe = 0u8;
    core::hint::black_box(&probe) as *const u8 as usize
}

/// True within `RESERVE` of the initial stack's low end. Anywhere else is false: a peer
/// thread's stack, and every address when no stack is known, since then the base is 0.
#[inline(never)]
pub fn exhausted() -> bool {
    here().wrapping_sub(base()) < RESERVE
}

/// Runs `f` on a stack of `size` bytes that the check knows about.
#[cfg(test)]
pub(crate) fn on_stack_of<T: Send + 'static>(
    size: usize,
    f: impl FnOnce() -> T + Send + 'static,
) -> T {
    std::thread::Builder::new()
        .stack_size(size)
        .spawn(move || {
            allow_below_here(size - RESERVE);
            f()
        })
        .expect("a test thread")
        .join()
        .expect("the test thread finished")
}

/// Runs `f` where the check first fires, so with no more than the reserve below it.
#[cfg(test)]
#[inline(never)]
pub(crate) fn at_the_reserve(f: impl FnOnce()) {
    let pad = [0u8; 1024];
    core::hint::black_box(&pad);
    if exhausted() {
        f();
    } else {
        at_the_reserve(f);
    }
    core::hint::black_box(&pad);
}

/// From here on the check fires `bytes` below this frame.
#[cfg(test)]
pub(crate) fn allow_below_here(bytes: usize) {
    TEST_BASE.with(|b| b.set(here() - bytes - RESERVE));
}

/// From here on the check fires at once, in this frame and its callees.
#[cfg(test)]
pub(crate) fn stop_here() {
    TEST_BASE.with(|b| b.set(here() - RESERVE / 2));
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fires_only_near_the_low_end_of_the_known_stack() {
        assert!(!exhausted(), "no stack is known yet");
        allow_below_here(1024 * 1024);
        assert!(!exhausted());
        stop_here();
        assert!(exhausted());
        // Another stack altogether, on either side.
        TEST_BASE.with(|b| b.set(here() + 1024 * 1024));
        assert!(!exhausted());
        TEST_BASE.with(|b| b.set(here() - 4 * RESERVE));
        assert!(!exhausted());
    }
}
