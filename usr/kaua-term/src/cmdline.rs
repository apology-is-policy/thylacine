//! The kaua-term argv contract -- what a host DECLARES to the terminal it
//! spawns, parsed as pure logic so both ends of the seam are host-testable.
//!
//! The process itself (`main.rs`) cannot be host-compiled, and a declaration
//! that never arrives is invisible: the tile simply wears the wrong theme, or
//! renders no Beacon frames, and nothing fails. So the parse lives here, where
//! halcyond's own tests can drive the args it BUILDS through the parser its
//! child actually RUNS -- the seam proven end to end, minus the exec.

use alloc::string::String;
use alloc::vec::Vec;
use vt::Palette;

/// The render tier a host advertises to the hosted app (KAUA-TERM.md R1).
/// Absent = `None`, fail-closed: an app whose host declared nothing must not
/// emit frames nobody will render.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Tier {
    None,
    Cells,
    Rich,
}

impl Tier {
    /// The `/env/BEACON` word, which is the tier's wire form both ways.
    pub fn as_str(self) -> &'static str {
        match self {
            Tier::None => "none",
            Tier::Cells => "cells",
            Tier::Rich => "rich",
        }
    }

    fn parse(a: &[u8]) -> Option<Tier> {
        match a {
            b"none" => Some(Tier::None),
            b"cells" => Some(Tier::Cells),
            b"rich" => Some(Tier::Rich),
            _ => None,
        }
    }
}

/// Why an argv was refused. Each variant is a thing the HOST got wrong, so the
/// message names the flag rather than the value -- the value came from code.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArgError {
    Beacon,
    Palette,
    Unknown,
    NonUtf8,
}

impl ArgError {
    pub fn message(self) -> &'static str {
        match self {
            ArgError::Beacon => "kaua-term: --beacon takes none|cells|rich\n",
            ArgError::Palette => "kaua-term: --palette takes 18 comma-separated RRGGBB\n",
            ArgError::Unknown => "kaua-term: unknown flag before the dimensions\n",
            ArgError::NonUtf8 => "kaua-term: non-utf8 argument\n",
        }
    }
}

/// A parsed command line: `[--beacon TIER] [--palette SPEC] <cols> <rows>
/// [prog [args...]]`.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Cmdline {
    pub tier: Tier,
    /// The palette cells are born in. `None` = no host declared one, so the
    /// caller uses the vt default: a kaua-term nobody themed is just a
    /// terminal, and MUST NOT guess at a compositor's colours.
    pub palette: Option<Palette>,
    pub cols: u16,
    pub rows: u16,
    /// The hosted command; never empty (defaults to the shell).
    pub argv: Vec<String>,
}

/// The dimensions a host that said nothing gets. Only reachable by hand-running
/// the binary; halcyond always sizes its tiles.
const DEFAULT_COLS: u16 = 80;
const DEFAULT_ROWS: u16 = 24;
pub const DEFAULT_PROG: &str = "/bin/ut";

fn parse_dim(a: Option<&[u8]>, fallback: u16) -> u16 {
    core::str::from_utf8(a.unwrap_or(b""))
        .ok()
        .and_then(|s| s.parse::<u16>().ok())
        .filter(|&d| d >= 1)
        .unwrap_or(fallback)
}

/// Parse an argv WITHOUT argv[0]. Declarations come first, in any order, then
/// the dimensions, then the hosted command.
///
/// A malformed declaration is an ERROR, never a default -- including a flag
/// this build does not know. The alternative is what an order-sensitive parser
/// does with an unexpected flag: read it as a dimension, fall back to 80x24,
/// and run the next argument as the program. That is a QUIET misparse, and a
/// quiet misparse of a theme declaration is exactly the failure the
/// declaration exists to prevent. A flag that is merely ABSENT is not
/// malformed -- see `Cmdline::palette`.
///
/// Flags stop at the first non-flag token, so a hosted program whose own name
/// begins with `--` is still a program: it sits after the dimensions.
pub fn parse(args: &[&[u8]]) -> Result<Cmdline, ArgError> {
    let mut i = 0usize;
    let mut tier = Tier::None;
    let mut palette = None;
    while let Some(flag) = args.get(i) {
        match *flag {
            b"--beacon" => {
                tier =
                    Tier::parse(args.get(i + 1).copied().unwrap_or(b"")).ok_or(ArgError::Beacon)?;
            }
            b"--palette" => {
                let spec = args.get(i + 1).copied().unwrap_or(b"");
                let spec = core::str::from_utf8(spec).map_err(|_| ArgError::Palette)?;
                palette = Some(vt::palette_from_spec(spec).ok_or(ArgError::Palette)?);
            }
            f if f.starts_with(b"--") => return Err(ArgError::Unknown),
            _ => break,
        }
        i += 2;
    }
    let cols = parse_dim(args.get(i).copied(), DEFAULT_COLS);
    let rows = parse_dim(args.get(i + 1).copied(), DEFAULT_ROWS);
    i = (i + 2).min(args.len());
    let mut argv: Vec<String> = Vec::new();
    for a in &args[i..] {
        match core::str::from_utf8(a) {
            Ok(s) => argv.push(String::from(s)),
            Err(_) => return Err(ArgError::NonUtf8),
        }
    }
    if argv.is_empty() {
        argv.push(String::from(DEFAULT_PROG));
    }
    Ok(Cmdline {
        tier,
        palette,
        cols,
        rows,
        argv,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn parse_str(args: &[&str]) -> Result<Cmdline, ArgError> {
        let owned: Vec<&[u8]> = args.iter().map(|s| s.as_bytes()).collect();
        parse(&owned)
    }

    #[test]
    fn a_full_command_line_carries_every_declaration() {
        let spec = vt::palette_to_spec(&vt::PARCHMENT);
        let c = parse_str(&[
            "--beacon",
            "rich",
            "--palette",
            &spec,
            "100",
            "40",
            "/bin/hx",
            "f",
        ])
        .unwrap();
        assert_eq!(c.tier, Tier::Rich);
        assert_eq!(c.palette, Some(vt::PARCHMENT));
        assert_eq!((c.cols, c.rows), (100, 40));
        assert_eq!(c.argv, vec![String::from("/bin/hx"), String::from("f")]);
    }

    // The defaults are what a hand-run kaua-term gets. `palette: None` is the
    // load-bearing one: it must be DISTINGUISHABLE from a declared palette, so
    // the caller can tell "nobody themed me" from "themed me like this".
    #[test]
    fn an_undeclared_command_line_defaults_and_says_so() {
        let c = parse_str(&[]).unwrap();
        assert_eq!(c.tier, Tier::None, "fail-closed: no frames");
        assert_eq!(c.palette, None, "no host theme, not a guessed one");
        assert_eq!((c.cols, c.rows), (80, 24));
        assert_eq!(c.argv, vec![String::from(DEFAULT_PROG)]);
    }

    // A malformed flag is refused, not defaulted around. The POSITIVE control
    // one variable away proves the refusal is the value's and not the shape's.
    #[test]
    fn a_malformed_declaration_is_refused_not_defaulted() {
        let good = vt::palette_to_spec(&vt::BONFIRE);
        assert!(
            parse_str(&["--palette", &good, "80", "24"]).is_ok(),
            "the control must pass"
        );
        assert_eq!(
            parse_str(&["--palette", "nonsense", "80", "24"]),
            Err(ArgError::Palette)
        );
        assert_eq!(
            parse_str(&["--palette"]),
            Err(ArgError::Palette),
            "no value"
        );
        assert_eq!(
            parse_str(&["--beacon", "loud", "80", "24"]),
            Err(ArgError::Beacon)
        );
        assert_eq!(parse_str(&["--beacon"]), Err(ArgError::Beacon), "no value");
    }

    // Order-independence, and the reason for it: an order-sensitive parser
    // reads the SECOND flag as a dimension, silently falls back to 80x24, and
    // runs the next argument as the program -- a quiet misparse of a theme
    // declaration, which is the whole failure class this arc exists to close.
    #[test]
    fn declarations_are_read_in_either_order() {
        let spec = vt::palette_to_spec(&vt::PARCHMENT);
        let a = parse_str(&["--beacon", "rich", "--palette", &spec, "100", "40"]).unwrap();
        let b = parse_str(&["--palette", &spec, "--beacon", "rich", "100", "40"]).unwrap();
        assert_eq!(a, b, "flag order must not change the meaning");
        assert_eq!((b.cols, b.rows), (100, 40), "not the 80x24 fallback");
        assert_eq!(b.argv, vec![String::from(DEFAULT_PROG)], "no stray program");
    }

    // A flag this build does not know is refused rather than read as a
    // dimension. The control proves the refusal is the UNKNOWN flag's.
    #[test]
    fn an_unknown_flag_is_refused_not_read_as_a_dimension() {
        assert!(parse_str(&["--beacon", "rich", "8", "2"]).is_ok());
        assert_eq!(
            parse_str(&["--beacon", "rich", "--future", "x", "8", "2"]),
            Err(ArgError::Unknown)
        );
        assert_eq!(parse_str(&["--future", "8", "2"]), Err(ArgError::Unknown));
    }

    // Either flag alone, and neither consuming the other's slot -- the bug a
    // positional parser actually has.
    #[test]
    fn each_flag_stands_alone_without_eating_the_dimensions() {
        let c = parse_str(&["--beacon", "cells", "12", "7", "/bin/sh"]).unwrap();
        assert_eq!(
            (c.tier, c.palette, c.cols, c.rows),
            (Tier::Cells, None, 12, 7)
        );
        let spec = vt::palette_to_spec(&vt::BONFIRE);
        let c = parse_str(&["--palette", &spec, "12", "7"]).unwrap();
        assert_eq!(c.tier, Tier::None);
        assert_eq!(c.palette, Some(vt::BONFIRE));
        assert_eq!((c.cols, c.rows), (12, 7));
    }

    // A zero or garbage dimension falls back rather than reaching the parser,
    // which assumes cols >= 1 && rows >= 1.
    #[test]
    fn a_degenerate_dimension_falls_back() {
        let c = parse_str(&["0", "0"]).unwrap();
        assert_eq!((c.cols, c.rows), (80, 24));
        let c = parse_str(&["wide", "tall"]).unwrap();
        assert_eq!((c.cols, c.rows), (80, 24));
    }

    // The tier word round-trips: it is written to /env/BEACON verbatim, so a
    // parse that lost the distinction would advertise the wrong tier.
    #[test]
    fn the_tier_word_round_trips_through_env() {
        for t in [Tier::None, Tier::Cells, Tier::Rich] {
            assert_eq!(Tier::parse(t.as_str().as_bytes()), Some(t));
        }
    }
}
