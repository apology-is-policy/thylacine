// The system-tier renderer config (AURORA-CONFIG.md section 3.2 "the writer
// defines the tier"; cfg-2a): /lib/aurora/config is the DEVICE's memory --
// aurora reads it at startup (the pre-login theme) and the F10 OSD writes
// through on every change, so a setting survives the boot exactly like a
// monitor's own OSD settings survive power-off. The per-user $home/lib/aurora
// is the SESSION's file (pushed in-band via OSC at login -- cfg-2b) and is
// never touched here: aurora is a pre-login SYSTEM process and the home is
// per-user-encrypted in the session's namespace.
//
// Fail-soft throughout: an absent/unreadable/malformed/oversized file leaves
// the compiled defaults; a failed save never disturbs the live settings.
// Config can never break the fbcon.

use crate::osd::{Mode, Settings};
use crate::vt::THEMES;
use alloc::format;
use alloc::string::String;
use libthyla_rs::fs::{self, File};
use libthyla_rs::io::{Read, Write};
use libthyla_rs::t_fsync;

pub const CONFIG_PATH: &str = "/lib/aurora/config";
const CONFIG_TMP: &str = "/lib/aurora/config.tmp";
const CONFIG_MAX: usize = 4096; // a handful of `key value` lines

/// Fold `key value` lines into `s`. Unknown keys and malformed lines are
/// IGNORED -- a future aurora with more keys reads an old file and vice
/// versa; the parse can only ever move settings, never fail.
pub fn parse(text: &str, s: &mut Settings) {
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (key, val) = match line.split_once(char::is_whitespace) {
            Some((k, v)) => (k, v.trim()),
            None => continue,
        };
        match key {
            "theme" => {
                if let Some(i) = THEMES.iter().position(|(n, _)| *n == val) {
                    s.theme = i;
                }
            }
            "cursor-blink" => match val {
                "on" => s.cursor_blink = true,
                "off" => s.cursor_blink = false,
                _ => {}
            },
            // cfg-3: the compositor tier. Reaches settings ONLY from the
            // config FILE tiers -- the OSC drain arm allowlists its keys
            // and never passes `mode` through here (a session-injected
            // mode reaching config::save would launder session authority
            // into the gated startup push). Structural parse only; the
            // server's gate + bounds are the authority.
            "mode" => {
                if val == "auto" {
                    s.mode = Mode::Auto;
                } else {
                    let mut it = val.split_ascii_whitespace();
                    if let (Some(w), Some(h), None) = (
                        it.next().and_then(|t| t.parse::<u32>().ok()),
                        it.next().and_then(|t| t.parse::<u32>().ok()),
                        it.next(),
                    ) {
                        s.mode = Mode::Fixed(w, h);
                    }
                }
            }
            // cfg-4: the compositor tier (chords + gaps). Like `mode`, these
            // reach settings ONLY from the config FILE (the OSC allowlist
            // never passes them -- authority verbs ride the gated ctl, never
            // OSC). Structurally parsed only for well-formedness (2 printable
            // tokens for chord; a bounded int for gaps); tapestryd's gated
            // verb is the real validator + authority.
            "chord" => {
                let mut it = val.split_ascii_whitespace();
                if let (Some(combo), Some(action), None) =
                    (it.next(), it.next(), it.next())
                {
                    if is_printable_token(combo) && is_printable_token(action)
                        && s.chords.len() < CHORDS_MAX
                    {
                        s.chords.push((String::from(combo), String::from(action)));
                    }
                }
            }
            "gaps" => {
                if let Ok(px) = val.parse::<u32>() {
                    if px <= GAPS_MAX {
                        s.gaps = Some(px);
                    }
                }
            }
            _ => {}
        }
    }
}

/// A config chord/action token must be a short run of printable, non-space,
/// non-control ASCII (the F1 discipline: never push an unvalidated token
/// into a ctl command, even from the trusted system file).
fn is_printable_token(t: &str) -> bool {
    !t.is_empty()
        && t.len() <= 32
        && t.bytes().all(|b| (0x21..0x7f).contains(&b))
}

/// The cfg-4 bounds mirror the compositor's (chords.rs GAPS_MAX; a bounded
/// chord count keeps the startup push cheap + the config small).
const GAPS_MAX: u32 = 32;
const CHORDS_MAX: usize = 32;

/// The canonical file content (the inverse of parse). The OSD edits only
/// theme/cursor/mode, but the cfg-4 chord/gaps lines are ROUND-TRIPPED so
/// an OSD save never wipes a hand-edited compositor config.
pub fn render(s: &Settings) -> String {
    let mode = match s.mode {
        Mode::Auto => String::from("auto"),
        Mode::Fixed(w, h) => format!("{} {}", w, h),
    };
    let mut out = format!(
        "# aurora renderer config (the system tier -- the F10 OSD writes this;\n\
         # AURORA-CONFIG.md section 3.2). key value; unknown keys are ignored.\n\
         theme {}\n\
         cursor-blink {}\n\
         mode {}\n",
        THEMES[s.theme % THEMES.len()].0,
        if s.cursor_blink { "on" } else { "off" },
        mode,
    );
    if let Some(g) = s.gaps {
        out.push_str(&format!("gaps {}\n", g));
    }
    for (combo, action) in &s.chords {
        out.push_str(&format!("chord {} {}\n", combo, action));
    }
    out
}

/// Best-effort startup load (bounded read; no alloc proportional to the
/// file -- /lib is SYSTEM-owned, but a bounded read costs nothing).
pub fn load(s: &mut Settings) {
    let mut f = match File::open(CONFIG_PATH) {
        Ok(f) => f,
        Err(_) => return,
    };
    let mut buf = [0u8; CONFIG_MAX];
    let mut n = 0usize;
    loop {
        match f.read(&mut buf[n..]) {
            Ok(0) => break,
            Ok(k) => {
                n += k;
                if n == CONFIG_MAX {
                    break; // oversized: parse the bounded prefix (fail-soft)
                }
            }
            Err(_) => return,
        }
    }
    if let Ok(text) = core::str::from_utf8(&buf[..n]) {
        parse(text, s);
    }
}

/// Write-through: write-tmp + fsync + rename (the A-1.6 swap discipline --
/// a crash mid-save leaves the OLD config intact, never a torn one; dev9p
/// renameat replaces atomically). The tmp fid is dropped (clunked) before
/// the rename. Returns false on any failure.
pub fn save(s: &Settings) -> bool {
    let text = render(s);
    let mut f = match File::create(CONFIG_TMP) {
        Ok(f) => f,
        Err(_) => return false,
    };
    if f.write_all(text.as_bytes()).is_err() {
        return false;
    }
    // The content barrier before the swap (best-effort): the installed name
    // never points at undurable bytes -- a kill between the rename and the
    // post-rename barrier rolls back to the OLD config, never a torn one.
    let _ = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
    if fs::rename(CONFIG_TMP, CONFIG_PATH).is_err() {
        return false;
    }
    // The A-1.6 METADATA barrier, POST-rename, on the SAME fd -- and STRICT.
    // Without it a hard kill (a power cut; the E2E's Ctrl-A x) rolls the
    // swap back (the persist E2E caught it: a later fsync commits the
    // transaction group GLOBALLY -- stratumd h_fsync == stm_fs_commit,
    // whole-pool -- so the PREVIOUS save's rename survived while this one's
    // died uncommitted). The fd is the one we WROTE: SYS_FSYNC gates on
    // RIGHT_WRITE, and an omode-derived OREAD handle (A-3 F1) fails that
    // gate with -1 before any 9P is sent -- which is exactly how the first
    // two barrier attempts (an OREAD parent-dir fd, then an OREAD re-open
    // of the renamed file) silently and loudly failed. A 9P fid follows the
    // FILE across a rename, so the OWRITE create fd is still valid here --
    // one open, the right rights, the proven fsync-what-you-wrote shape
    // (the corvus persist_keypair_wrap discipline).
    let ok = unsafe { t_fsync(f.as_raw_fd() as i64, 0) == 0 };
    drop(f);
    ok
}

// DORMANT host-harness tests (the G-4f named seam, like the sibling
// modules): parse/render are pure -- the IO wrappers are proven by the
// ls-gfx-osd-persist in-guest E2E (write-through + cross-reboot read).
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_render_round_trip() {
        let mut s = Settings::new();
        s.theme = 1;
        s.cursor_blink = false;
        s.mode = Mode::Fixed(1600, 900);
        let text = render(&s);
        let mut back = Settings::new();
        parse(&text, &mut back);
        assert_eq!(back.theme, 1);
        assert!(!back.cursor_blink);
        assert!(back.mode == Mode::Fixed(1600, 900));
        // The auto form round-trips too (the non-pushing default).
        s.mode = Mode::Auto;
        let mut back2 = Settings::new();
        back2.mode = Mode::Fixed(1, 1);
        parse(&render(&s), &mut back2);
        assert!(back2.mode == Mode::Auto);
    }

    #[test]
    fn parse_is_fail_soft() {
        let mut s = Settings::new();
        parse(
            "# comment\n\
             \n\
             theme spinifex\n\
             theme not-a-theme\n\
             cursor-blink sideways\n\
             mode garbage here\n\
             mode 12\n\
             mode 1280 800 7\n\
             unknown-key whatever\n\
             justakeywithnovalue\n",
            &mut s,
        );
        // The valid theme line applied; the invalid one was ignored (did not
        // reset); the bad blink value left the default; every malformed mode
        // form (non-numeric, one field, three fields) left Auto; unknowns
        // ignored.
        assert_eq!(s.theme, 2, "spinifex applied, not-a-theme ignored");
        assert!(s.cursor_blink, "bad blink value leaves the default");
        assert!(s.mode == Mode::Auto, "malformed mode lines leave the default");
    }

    #[test]
    fn chords_and_gaps_round_trip_and_fail_soft() {
        // Round-trip: the OSD does not edit chords/gaps, so render() must
        // preserve hand-edited lines (else an OSD theme save wipes them).
        let mut s = Settings::new();
        s.gaps = Some(8);
        s.chords.push((String::from("super+g"), String::from("zoom")));
        s.chords.push((String::from("super+f"), String::from("none")));
        let mut back = Settings::new();
        parse(&render(&s), &mut back);
        assert_eq!(back.gaps, Some(8));
        assert_eq!(back.chords.len(), 2);
        assert_eq!(back.chords[0], (String::from("super+g"), String::from("zoom")));
        assert_eq!(back.chords[1], (String::from("super+f"), String::from("none")));

        // Fail-soft: an over-cap gap, a control-byte or over-long token, or
        // a wrong-arity chord line are all IGNORED (tapestryd's gated verb
        // is the real validator; the parser only rejects the unpushable).
        let mut b = Settings::new();
        parse(
            "gaps 999\n\
             gaps notanum\n\
             chord super+g\n\
             chord super+g zoom extra\n\
             chord super+g zo\x01om\n\
             chord super+g zoom\n",
            &mut b,
        );
        assert_eq!(b.gaps, None, "over-cap + non-numeric gaps ignored");
        assert_eq!(b.chords.len(), 1, "only the well-formed 2-token chord kept");
        assert_eq!(b.chords[0].1, "zoom");
    }
}
