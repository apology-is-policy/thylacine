//! Beacon -- the semantic output markup (docs/BEACON.md is the binding spec).
//!
//! Three layers, one crate:
//!   - `wire`: the OSC 1936 frame grammar -- emit, parse, strip. The strip
//!     property (BEACON.md 12.8 P1) is the crate's soul: stripping every
//!     frame from a rich stream yields byte-exactly the `none` emission,
//!     because the plain text is always the payload IN the stream and frames
//!     only bracket it.
//!   - `sink`: the per-tier realization API programs emit through (Sink for
//!     runs/zones, Table for listings).
//!   - `verbs`: the presentation verb table (BEACON.md 7) -- the rules
//!     engine a renderer's context menu offers per obj type (H-3c).
//!   - `boxd` / `color` / `palette`: the cells tier -- the Bonfire visual
//!     language, relocated verbatim from the coreutils crate (2026-09-01).
//!     The color discipline (COREUTILS-THYLACINE-DESIGN.md) is inherited
//!     wholesale: presentation and diagnostics may be styled; data payloads
//!     a pipe consumes stay byte-clean at EVERY tier.
//!
//! The emission gate (BEACON.md 12.4) is `effective_tier`: a pure function of
//! the advertised tier, the fd's Dev class, and the per-tool flag -- the
//! caller supplies the syscall-derived inputs, so the crate stays host-
//! testable with no libthyla-rs dependency.

#![no_std]

extern crate alloc;

pub mod boxd;
pub mod color;
pub mod palette;
pub mod sink;
pub mod verbs;
pub mod wire;

/// The renderer-advertised capability tier (BEACON.md 12.3; ARCH 23.5.4).
/// Transported renderer -> consctl `beacon <tier>` -> the shell's `BEACON`
/// environment export -> children. Absent anywhere along that chain = None.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Tier {
    /// No render capability advertised (a pipe, a serial host terminal).
    None,
    /// Aurora: the box+SGR cells language, no frames.
    Cells,
    /// Halcyon: full Beacon frames; no SGR inside Beacon-structured output.
    Rich,
}

impl Tier {
    /// Parse the `BEACON` environment value / the consctl tier word.
    pub fn parse(s: &str) -> Option<Tier> {
        match s {
            "none" => Some(Tier::None),
            "cells" => Some(Tier::Cells),
            "rich" => Some(Tier::Rich),
            _ => None,
        }
    }

    pub fn as_str(self) -> &'static str {
        match self {
            Tier::None => "none",
            Tier::Cells => "cells",
            Tier::Rich => "rich",
        }
    }
}

/// The per-tool override, mirroring `--color=WHEN` (`--beacon=WHEN`).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BeaconMode {
    /// The default: rich/cells only onto an interactive console sink.
    Auto,
    /// Emit at the advertised tier regardless of the fd class (floor Cells:
    /// an explicit "always" still never frames into a `none` advertisement --
    /// there is no renderer to read them).
    Always,
    /// Plain bytes, period (the clean escape hatch, like `--color=never`).
    Never,
}

impl BeaconMode {
    pub fn parse_when(s: &str) -> Option<BeaconMode> {
        match s {
            "" | "always" | "yes" | "force" => Some(BeaconMode::Always),
            "never" | "no" | "none" => Some(BeaconMode::Never),
            "auto" | "tty" | "if-tty" => Some(BeaconMode::Auto),
            _ => None,
        }
    }
}

/// The Dev class char of the console (`SYS_FD_DEVCLASS` answers `'c'` for a
/// `SYS_CONSOLE_OPEN` fd and a walked `/dev/cons` fd -- the normalization the
/// kernel guarantees, docs/SYS-FD-DEVCLASS-SPEC.md AS-BUILT).
pub const DC_CONSOLE: u8 = b'c';

/// The Dev class char of a pts SLAVE (`SYS_FD_DEVCLASS` answers `'t'` for a
/// fd the kernel's pts registry knows as a slave, H-4d): a terminal its pts
/// host renders -- a Halcyon tile's shell prints onto one. The host declares
/// the tier it renders in the hosted program's `BEACON` at spawn
/// (KAUA-TERM.md R1: `kaua-term --beacon`), so for a pts the advertisement
/// is the host's word exactly as the console's is the renderer's.
pub const DC_PTS: u8 = b't';

/// The two-condition emission gate (BEACON.md 12.4): emit above None iff the
/// sink is a terminal something renders -- the interactive console, or a pts
/// slave whose host declared its tier -- AND the advertised tier says it
/// renders. `dc_of_stdout` is `t_fd_devclass(1)` (None when the syscall errs
/// -- a closed or pre-H-1 fd reads as not-a-terminal, never as one).
pub fn effective_tier(env_tier: Tier, dc_of_stdout: Option<u8>, flag: BeaconMode) -> Tier {
    match flag {
        BeaconMode::Never => Tier::None,
        BeaconMode::Always => env_tier,
        BeaconMode::Auto => {
            if dc_of_stdout == Some(DC_CONSOLE) || dc_of_stdout == Some(DC_PTS) {
                env_tier
            } else {
                Tier::None
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tier_parse_roundtrip() {
        for t in [Tier::None, Tier::Cells, Tier::Rich] {
            assert_eq!(Tier::parse(t.as_str()), Some(t));
        }
        assert_eq!(Tier::parse("loud"), None);
        assert_eq!(Tier::parse(""), None);
    }

    #[test]
    fn gate_auto_needs_console_and_tier() {
        // The load-bearing pair: a pipe never gets frames, whatever the env.
        assert_eq!(
            effective_tier(Tier::Rich, Some(b'|'), BeaconMode::Auto),
            Tier::None
        );
        assert_eq!(
            effective_tier(Tier::Rich, Some(DC_CONSOLE), BeaconMode::Auto),
            Tier::Rich
        );
        // No advertisement -> nothing, even on the console.
        assert_eq!(
            effective_tier(Tier::None, Some(DC_CONSOLE), BeaconMode::Auto),
            Tier::None
        );
        // A failed probe reads as not-a-terminal, never as one.
        assert_eq!(
            effective_tier(Tier::Rich, None, BeaconMode::Auto),
            Tier::None
        );
        // H-4d: a pts slave is a terminal its host renders -- the host's
        // advertisement decides, in both directions.
        assert_eq!(
            effective_tier(Tier::Rich, Some(DC_PTS), BeaconMode::Auto),
            Tier::Rich
        );
        assert_eq!(
            effective_tier(Tier::None, Some(DC_PTS), BeaconMode::Auto),
            Tier::None
        );
        // A plain 9P file -- the class a pts MASTER also answers -- never
        // gets frames: printing onto a master is typing into the terminal.
        assert_eq!(
            effective_tier(Tier::Rich, Some(b'9'), BeaconMode::Auto),
            Tier::None
        );
    }

    #[test]
    fn gate_overrides() {
        assert_eq!(
            effective_tier(Tier::Rich, Some(b'|'), BeaconMode::Never),
            Tier::None
        );
        // Always trusts the advertisement, not the fd -- but an absent
        // advertisement still yields None (no renderer reads the frames).
        assert_eq!(
            effective_tier(Tier::Cells, Some(b'|'), BeaconMode::Always),
            Tier::Cells
        );
        assert_eq!(
            effective_tier(Tier::None, Some(b'|'), BeaconMode::Always),
            Tier::None
        );
    }
}
