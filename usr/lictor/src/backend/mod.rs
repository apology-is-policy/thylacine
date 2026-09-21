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
pub mod screen;
pub mod import;
pub mod device;
pub mod server;

/// Ordinary console writes park during SAK. The hardware owner must never
/// block behind its own trusted episode merely to print a GPU diagnostic.
/// No key material is logged. Only the exclusive phase holds console writes.
/// This single-threaded owner is the only process able to acknowledge quiescence;
/// diagnostics in quiescing/restoring cannot race its own next acknowledgement.
pub fn diagnostic(text: &str) {
    let mut status = crate::endpoint::Message::default();
    if crate::endpoint::call(crate::endpoint::STATUS, &mut status).is_ok() && status.phase == 2 { return; }
    let _ = libthyla_rs::t_putstr(text);
}
pub mod seat;
