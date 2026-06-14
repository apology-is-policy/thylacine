// kaua::query -- the launch-time terminal-size handshake (CPR), partly
// audit-bearing.
//
// The console has no winsize-query syscall (KAUA.md 3.4 / 12 listed it a seam);
// this realizes the query-at-launch half via the standard ANSI Cursor-Position
// Report round-trip: write `ESC[s ESC[9999;9999H ESC[6n` to fd 1 (save cursor,
// park it at a far corner -- the terminal clamps to the bottom-right -- then
// ask for its position), read the reply `ESC[<rows>;<cols>R` from fd 0, restore
// the cursor. The clamped position IS the screen size. A caller (nora) uses it
// to size the viewport, falling back to a fixed default on no reply.
//
// SPLIT: `parse_cpr` is pure (a byte-slice parser) and host-tested; only the
// fd-0/fd-1 I/O (`terminal_size`) needs libthyla-rs and is backend-gated.
//
// CAPABILITY DISCIPLINE (KAUA.md 3.5 / 5; I-27): like kaua::term + kaua::source
// this touches ONLY fd 0 (read) and fd 1 (write) -- never the line discipline
// (consctl), never console-attach. The probe is a bounded request/reply, not a
// steady-state input path: it is the launch handshake the binary runs ONCE,
// before the PollSource loop. Bytes are read one at a time and the read stops at
// the `R` terminator, so any input typed during the launch window stays in the
// kernel fd buffer for the steady-state PollSource (no lost keystroke). The read
// is deadline-bounded (a dumb terminal / the non-interactive harness never
// replies -> the first poll times out -> fallback) and the staging buffer is
// fixed-size (no unbounded growth on a garbage stream). The poll + read are #811
// death-interruptible, so a dying app unwinds during the handshake.

/// Largest reply we stage before giving up. A CPR reply is `ESC[` + <=5 rows
/// digits + `;` + <=5 cols digits + `R` = at most 14 bytes; 32 tolerates minor
/// leading noise (a stray sequence) and bounds the read.
const CPR_BUF_CAP: usize = 32;

/// Parse a Cursor-Position-Report `ESC[<rows>;<cols>R` out of `buf`, returning
/// `(cols, rows)` = `(width, height)`. Tolerates bytes before the `ESC` (scans
/// for the first well-formed report) and after the `R` (the caller stops
/// reading at `R`). Returns `None` if no well-formed, non-zero report is found.
pub fn parse_cpr(buf: &[u8]) -> Option<(u16, u16)> {
    let mut i = 0;
    while i + 1 < buf.len() {
        if buf[i] == 0x1b && buf[i + 1] == b'[' {
            if let Some(res) = parse_cpr_body(&buf[i + 2..]) {
                return Some(res);
            }
        }
        i += 1;
    }
    None
}

/// Parse `<rows>;<cols>R` (the CSI body after `ESC[`). `(cols, rows)` on
/// success.
fn parse_cpr_body(s: &[u8]) -> Option<(u16, u16)> {
    let mut idx = 0;
    let rows = parse_num(s, &mut idx)?;
    if s.get(idx) != Some(&b';') {
        return None;
    }
    idx += 1;
    let cols = parse_num(s, &mut idx)?;
    if s.get(idx) != Some(&b'R') {
        return None;
    }
    if rows == 0 || cols == 0 {
        return None;
    }
    Some((cols, rows))
}

/// Parse a run of ASCII digits at `*idx`, advancing it. `None` if no digit is
/// present. The value saturates at `u16::MAX` (the caller clamps to a sane
/// viewport bound) so a long digit run can never overflow.
fn parse_num(s: &[u8], idx: &mut usize) -> Option<u16> {
    let start = *idx;
    let mut n: u32 = 0;
    while let Some(&c) = s.get(*idx) {
        if c.is_ascii_digit() {
            n = n.saturating_mul(10).saturating_add((c - b'0') as u32);
            if n > u16::MAX as u32 {
                n = u16::MAX as u32;
            }
            *idx += 1;
        } else {
            break;
        }
    }
    if *idx == start {
        return None;
    }
    Some(n as u16)
}

/// Query the terminal size via a CPR round-trip, waiting up to `timeout_ms` for
/// the reply. Returns `(cols, rows)` = `(width, height)`, or `None` on timeout /
/// EOF / no reply / a malformed reply -- the caller then uses a fixed default.
///
/// Must be called with the console already in raw mode (the caller's job -- for
/// nora, `ut` sets raw via consctl before the spawn) and before entering the
/// alternate screen; the cursor is saved/restored so the visible screen is
/// undisturbed.
#[cfg(feature = "backend")]
pub fn terminal_size(timeout_ms: u32) -> Option<(u16, u16)> {
    use crate::encode::{PARK_CURSOR_FAR, REQUEST_CURSOR_POS, RESTORE_CURSOR, SAVE_CURSOR};
    use libthyla_rs::io::{stdout, Write};

    let mut out = stdout();
    // Save the cursor, park it far (clamps to the bottom-right), request its
    // position. The restore runs after, on every path.
    let sent = out.write_all(SAVE_CURSOR).is_ok()
        && out.write_all(PARK_CURSOR_FAR).is_ok()
        && out.write_all(REQUEST_CURSOR_POS).is_ok()
        && out.flush().is_ok();
    let result = if sent { read_cpr(timeout_ms) } else { None };
    let _ = out.write_all(RESTORE_CURSOR);
    let _ = out.flush();
    result
}

/// Read the CPR reply from fd 0 within the deadline. Polls before each byte so
/// a read never blocks; stops at the `R` terminator (leaving any later bytes in
/// the kernel buffer for the steady-state PollSource); fixed-size staging.
#[cfg(feature = "backend")]
fn read_cpr(timeout_ms: u32) -> Option<(u16, u16)> {
    use libthyla_rs::io::{stdin, Read};
    use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};

    let mut poll = PollSet::new();
    let mut inp = stdin();
    poll.add(&inp, PollEvents::READ);

    let mut buf = [0u8; CPR_BUF_CAP];
    let mut len = 0usize;
    // At most CPR_BUF_CAP bytes; the loop bound == the buffer bound, so each
    // write `buf[len]` is in range and a reply with no `R` terminates as None.
    while len < CPR_BUF_CAP {
        let mut readable = false;
        match poll.poll(PollTimeout::Millis(timeout_ms)) {
            Ok(results) => {
                for ev in results {
                    if ev.fd == 0 {
                        if ev.is_readable() {
                            readable = true;
                        }
                        if ev.is_hup() || ev.is_err() {
                            return None;
                        }
                    }
                }
            }
            Err(_) => return None,
        }
        if !readable {
            // The deadline elapsed with no byte: a terminal that does not answer
            // CPR (dumb terminal / the non-interactive harness) -> fall back.
            return None;
        }
        let mut b = [0u8; 1];
        match inp.read(&mut b) {
            Ok(0) => return None, // EOF
            Ok(_) => {
                buf[len] = b[0];
                len += 1;
                if b[0] == b'R' {
                    return parse_cpr(&buf[..len]);
                }
            }
            Err(_) => return None,
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parses_a_well_formed_report() {
        // ESC[<rows>;<cols>R -> (cols, rows).
        assert_eq!(parse_cpr(b"\x1b[24;80R"), Some((80, 24)));
        assert_eq!(parse_cpr(b"\x1b[50;200R"), Some((200, 50)));
        assert_eq!(parse_cpr(b"\x1b[1;1R"), Some((1, 1)));
    }

    #[test]
    fn tolerates_leading_noise_and_trailing_bytes() {
        // A stray byte before the ESC; junk after the R is ignored (the caller
        // stops reading at R, but the parser must also not choke on it).
        assert_eq!(parse_cpr(b"x\x1b[40;132R"), Some((132, 40)));
        assert_eq!(parse_cpr(b"\x1b[24;80Rabc"), Some((80, 24)));
    }

    #[test]
    fn skips_a_non_cpr_csi_before_the_report() {
        // A DEC private-mode set then the real CPR: the first ESC[ body fails to
        // parse (no leading digit), the scan finds the second.
        assert_eq!(parse_cpr(b"\x1b[?1h\x1b[10;20R"), Some((20, 10)));
    }

    #[test]
    fn rejects_malformed_reports() {
        assert_eq!(parse_cpr(b""), None);
        assert_eq!(parse_cpr(b"\x1b["), None);
        assert_eq!(parse_cpr(b"\x1b[24;80"), None); // no terminator
        assert_eq!(parse_cpr(b"\x1b[2480R"), None); // no separator
        assert_eq!(parse_cpr(b"\x1b[;80R"), None); // missing rows
        assert_eq!(parse_cpr(b"\x1b[24;R"), None); // missing cols
        assert_eq!(parse_cpr(b"\x1b[0;0R"), None); // zero size
    }

    #[test]
    fn clamps_absurd_values_to_u16_max() {
        // A digit flood cannot overflow; it saturates (the caller bounds it
        // further to a sane viewport).
        assert_eq!(parse_cpr(b"\x1b[99999;99999R"), Some((65535, 65535)));
    }
}
