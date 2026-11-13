//! Hardware backends. Normal compositor code will use the broker, not these.
macro_rules! say {
    ($($a:tt)*) => {{
        let mut s = alloc::format!($($a)*);
        s.push('\n');
        crate::backend::diagnostic(&s);
    }};
}
pub mod gpu;
pub mod input;
pub mod intake;
pub mod screen;
pub mod import;
pub mod device;
pub mod server;

/// Ordinary console writes park during SAK. The hardware owner must never
/// block behind its own trusted episode merely to print a GPU diagnostic.
/// No key material is logged. The exclusive phase holds console writes, and so
/// does a serial episode, which freezes every writer but its own.
/// Only the serve thread prints (the input thread never does), and it alone can
/// acknowledge quiescence, so diagnostics in quiescing/restoring cannot race its own
/// next acknowledgement.
pub fn diagnostic(text: &str) {
    let mut status = crate::endpoint::Message::default();
    if crate::endpoint::call(crate::endpoint::STATUS, &mut status).is_ok() && status.phase == 2 { return; }
    let _ = libthyla_rs::t_putstr(text);
}
/// The kernel's command line, read to its end. A line that fills the buffer may
/// have been cut inside a word, so it is read as none, and so is one that cannot
/// be read.
pub fn bootargs(buf: &mut [u8; 1024]) -> &[u8] {
    use libthyla_rs::{t_close, t_open, t_read, T_OREAD, T_WALK_OPEN_FROM_ROOT};
    // SAFETY: a path literal and the caller's buffer.
    let fd = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, b"/hw/chosen/bootargs".as_ptr(), 19, T_OREAD) };
    if fd < 0 { return &[]; }
    let mut got = 0usize;
    // A read may return less than is there, so only the end of the file ends it.
    let whole = loop {
        // SAFETY: `got < buf.len()` here, so the tail lies inside the buffer.
        let n = unsafe { t_read(fd, buf.as_mut_ptr().add(got), buf.len() - got) };
        if n < 0 { break false; }
        if n == 0 { break true; }
        got += n as usize;
        if got >= buf.len() { break false; }
    };
    unsafe { t_close(fd); }
    if !whole { return &[]; }
    &buf[..got]
}
pub mod seat;
