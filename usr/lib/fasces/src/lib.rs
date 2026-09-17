// fasces -- the imperium scale indicator (IMPERIUM-DESIGN.md 4 + 11.6).
//
// A magistrate's imperium was legible at a glance by the fasces his lictors
// carried: a bundle of rods whose count named his rank, and an axe (securis)
// among them when he held the power of life and death. Thylacine's elevated
// shell shows the same: one rod per held elevation cap, the axe glyph when
// CAP_KILL is held, and `#` (the classic elevated-prompt marker) in place of
// the plain `$`/`⊢`.
//
// This module is PURE -- it parses the kernel's `/proc/<pid>/imperium` line
// (devproc.c format_imperium) and renders the fasces glyph string. No
// syscalls: the consumer (ut's prompt, the `abdicate` builtin, the `imperium`
// tool) does the getpid + open + read and hands the bytes here. Purity is what
// makes it host-testable, and a SINGLE parser for a kernel-defined ABI line is
// what keeps three consumers from drifting (the lifted-constant hazard).
//
// The /proc line format (format_imperium, one line):
//   scope N session N propagating 0|1 rods N axe 0|1 caps 0xHEX until NS
// A well-formed line for a Proc that is NOT a legate has `scope 0`; the parser
// returns None for that (and for any malformed line), so a consumer treats
// "not elevated" and "unreadable" alike -- fail closed, which is the correct
// posture for a convenience mirror the design says to distrust in favor of the
// SAK (IMPERIUM-DESIGN.md 4).
//
// `not(test)` is no_std so the native binaries link it; under `cargo test` the
// crate is std so the host harness + assert macros work (the corvus-crypto
// pattern). No external deps; the linking binary provides the allocator.
#![cfg_attr(not(test), no_std)]

extern crate alloc;

use alloc::string::String;

/// The kernel's account of a Proc's legate scope, as parsed from
/// `/proc/<pid>/imperium`. Only produced for an ELEVATED Proc (`scope != 0`);
/// `parse` returns `None` otherwise, so a live `Imperium` always means "this
/// Proc is in a legate scope."
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Imperium {
    /// The legate scope id (nonzero -- 0 means not a legate, which `parse`
    /// maps to `None`).
    pub scope: u64,
    /// The audit session id of the conferring grant.
    pub session: u64,
    /// Whether the scope is PROPAGATING (its caps flow to rfork/spawn
    /// descendants; the imperium level is always propagating).
    pub propagating: bool,
    /// popcount(legate_caps) -- the number of caps that FLOW to this Proc's
    /// children, i.e. the number of rods in the fasces.
    pub rods: u32,
    /// CAP_KILL held -- the executioner's axe among the rods.
    pub axe: bool,
    /// The Proc's elevation-only caps as HELD (a further redeem's extras show
    /// here and not in `rods`).
    pub caps: u64,
    /// legate_valid_until in kernel ns (0 = no deadline).
    pub until: u64,
}

/// One rod per held cap -- the DOUBLE VERTICAL LINE, U+2016 (3-byte UTF-8),
/// echoing a lictor's bundled rods. Capped at `RODS_MAX` glyphs so a
/// pathological /proc line cannot spam the prompt (the imperium level holds at
/// most 3 elevation caps; the cap is pure defense).
const ROD_GLYPH: &str = "\u{2016}";
/// The securis -- CROSSED SWORDS, U+2694 (3-byte UTF-8): the power of life and
/// death (CAP_KILL). A single BMP glyph so it renders in a plain monospace
/// terminal (an emoji axe would not on the serial console).
const AXE_GLYPH: &str = "\u{2694}";
/// The elevated-prompt marker -- `#` by the classic root-shell convention
/// (IMPERIUM-DESIGN.md 4: `#` vs `$`/`⊢`).
const ELEVATED_MARK: &str = "#";
/// Glyph cap: never render more rods than this, however large `rods` reads.
const RODS_MAX: u32 = 6;

/// Parse a `/proc/<pid>/imperium` line into an `Imperium`, or `None` if the
/// line is malformed OR describes a non-legate Proc (`scope 0`). The parse is
/// keyword-driven (find `scope`, take the next token as its value, and so on),
/// so a v1.x field addition after `until` does not break it. Every value parse
/// that fails leaves the field at its default; a missing-or-zero `scope` is the
/// one failure that returns `None`, because that is the "not elevated" signal
/// the whole module keys on.
pub fn parse_imperium(line: &str) -> Option<Imperium> {
    let mut scope: u64 = 0;
    let mut session: u64 = 0;
    let mut propagating = false;
    let mut rods: u32 = 0;
    let mut axe = false;
    let mut caps: u64 = 0;
    let mut until: u64 = 0;

    let mut it = line.split_ascii_whitespace();
    while let Some(key) = it.next() {
        match key {
            "scope" => scope = it.next().and_then(|v| v.parse().ok()).unwrap_or(0),
            "session" => session = it.next().and_then(|v| v.parse().ok()).unwrap_or(0),
            "propagating" => propagating = it.next() == Some("1"),
            "rods" => rods = it.next().and_then(|v| v.parse().ok()).unwrap_or(0),
            "axe" => axe = it.next() == Some("1"),
            "caps" => caps = it.next().and_then(parse_hex_u64).unwrap_or(0),
            "until" => until = it.next().and_then(|v| v.parse().ok()).unwrap_or(0),
            _ => {} // unknown key: skip; its value (if any) is consumed as the next key and skipped too
        }
    }

    if scope == 0 {
        return None;
    }
    Some(Imperium {
        scope,
        session,
        propagating,
        rods,
        axe,
        caps,
        until,
    })
}

/// Parse a `0x`-prefixed (or bare) hex string into a u64. Returns `None` on any
/// non-hex byte or overflow, so a corrupt `caps` field leaves the default 0.
fn parse_hex_u64(s: &str) -> Option<u64> {
    let body = s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")).unwrap_or(s);
    if body.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for c in body.bytes() {
        let d = match c {
            b'0'..=b'9' => c - b'0',
            b'a'..=b'f' => c - b'a' + 10,
            b'A'..=b'F' => c - b'A' + 10,
            _ => return None,
        };
        v = v.checked_mul(16)?.checked_add(d as u64)?;
    }
    Some(v)
}

/// Render the fasces glyph string for an elevated scope: one rod per held cap
/// (bounded by `RODS_MAX`), then the axe glyph iff CAP_KILL is held, then the
/// `#` elevated marker. NO color -- the prompt applies the palette role (a
/// warning hue when the axe is present, ember otherwise) as a single
/// self-resetting SGR, so this stays pure. Always ends in `#`, so even a
/// zero-rod scope (should not happen -- a scope holds >= 1 cap) reads as
/// elevated rather than as a plain prompt.
pub fn render_fasces(im: &Imperium) -> String {
    let n = if im.rods > RODS_MAX { RODS_MAX } else { im.rods };
    let mut s = String::with_capacity(n as usize * ROD_GLYPH.len() + AXE_GLYPH.len() + 1);
    for _ in 0..n {
        s.push_str(ROD_GLYPH);
    }
    if im.axe {
        s.push_str(AXE_GLYPH);
    }
    s.push_str(ELEVATED_MARK);
    s
}

#[cfg(test)]
mod tests {
    use super::*;

    const ROD: &str = "\u{2016}";
    const AXE: &str = "\u{2694}";

    #[test]
    fn parses_a_propagating_three_cap_scope_with_the_axe() {
        let line = "scope 7 session 3 propagating 1 rods 3 axe 1 caps 0x380 until 12345\n";
        let im = parse_imperium(line).expect("elevated line parses");
        assert_eq!(im.scope, 7);
        assert_eq!(im.session, 3);
        assert!(im.propagating);
        assert_eq!(im.rods, 3);
        assert!(im.axe);
        assert_eq!(im.caps, 0x380);
        assert_eq!(im.until, 12345);
    }

    #[test]
    fn scope_zero_is_not_elevated() {
        // A well-formed line for a plain (non-legate) Proc: the whole module's
        // "not elevated" signal. Must be None, not a zero-rod Imperium.
        let line = "scope 0 session 0 propagating 0 rods 0 axe 0 caps 0x0 until 0\n";
        assert_eq!(parse_imperium(line), None);
    }

    #[test]
    fn a_malformed_line_fails_closed() {
        assert_eq!(parse_imperium(""), None);
        assert_eq!(parse_imperium("garbage without a scope key"), None);
        // scope present but its value is junk -> defaults to 0 -> None (fail closed).
        assert_eq!(parse_imperium("scope xyz rods 2 axe 1"), None);
    }

    #[test]
    fn a_future_field_after_until_does_not_break_the_parse() {
        // v1.x may append fields; the keyword-driven parse must still read the
        // ones it knows and ignore the rest (the additive-ABI contract).
        let line = "scope 4 session 1 propagating 1 rods 2 axe 0 caps 0x180 until 99 pomerium 1 dictator 0";
        let im = parse_imperium(line).expect("parses despite trailing fields");
        assert_eq!(im.scope, 4);
        assert_eq!(im.rods, 2);
        assert!(!im.axe);
        assert_eq!(im.caps, 0x180);
    }

    #[test]
    fn fasces_renders_rods_then_axe_then_hash() {
        let two_no_axe = Imperium { scope: 1, session: 1, propagating: true, rods: 2, axe: false, caps: 0x180, until: 0 };
        assert_eq!(render_fasces(&two_no_axe), alloc::format!("{ROD}{ROD}#"));

        let three_axe = Imperium { scope: 1, session: 1, propagating: true, rods: 3, axe: true, caps: 0x380, until: 0 };
        assert_eq!(render_fasces(&three_axe), alloc::format!("{ROD}{ROD}{ROD}{AXE}#"));
    }

    #[test]
    fn fasces_caps_the_rod_count() {
        // A pathological rods value cannot spam the prompt: bounded at RODS_MAX.
        let many = Imperium { scope: 1, session: 1, propagating: true, rods: 99, axe: false, caps: 0, until: 0 };
        let rendered = render_fasces(&many);
        // RODS_MAX rods + '#', no axe.
        let rods_only = rendered.trim_end_matches('#');
        assert_eq!(rods_only.chars().count(), RODS_MAX as usize);
    }

    #[test]
    fn hex_parse_handles_prefix_and_rejects_junk() {
        assert_eq!(parse_hex_u64("0x380"), Some(0x380));
        assert_eq!(parse_hex_u64("380"), Some(0x380));
        assert_eq!(parse_hex_u64("0X1F"), Some(0x1f));
        assert_eq!(parse_hex_u64("0xzz"), None);
        assert_eq!(parse_hex_u64(""), None);
    }
}
