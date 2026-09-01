//! The per-bin Beacon emission gate (BEACON.md 12.4). One call resolves the
//! effective tier from the three inputs the gate composes: the renderer's
//! advertised tier (the `BEACON` environment export, inherited from ut), the
//! Dev class of stdout (`SYS_FD_DEVCLASS` -- frames go only onto the
//! interactive console under Auto), and the tool's `--beacon=WHEN` flag.
//!
//! Discipline notes the emitters share:
//!   - At `Rich`, SGR is OFF inside beacon-structured output (the renderer
//!     stylesheet owns typography); the emitting bin forces its color gate
//!     off when the resolved tier is Rich.
//!   - An `obj type=path` ref is canonicalized via `crate::path::abs`; a ref
//!     that cannot be canonicalized emits NO frame (plain text only).

use beacon::{BeaconMode, Tier};
use libthyla_rs::env;
use libthyla_rs::io;

/// Resolve the effective tier for stdout. `flag` is the tool's `--beacon`
/// setting (default `Auto`).
pub fn resolve(flag: BeaconMode) -> Tier {
    let env_tier = env::var("BEACON")
        .and_then(|v| Tier::parse(&v))
        .unwrap_or(Tier::None);
    beacon::effective_tier(env_tier, libthyla_rs::fd_devclass(1), flag)
}

/// Adapter: a `beacon::sink::Out` over the bins' buffered stdout writer (the
/// orphan rule keeps the impl out of both crates).
pub struct SinkOut<'a>(pub &'a mut io::OutSink);

impl beacon::sink::Out for SinkOut<'_> {
    fn out(&mut self, bytes: &[u8]) {
        self.0.put(bytes);
    }
}
