// aurora-push -- push the per-user renderer config to aurora, in-band
// (AURORA-CONFIG.md section 3.2, cfg-2b). Reads $HOME/lib/aurora (or the
// operand path) and emits each `key value` line as the private settings OSC
// (`ESC ] 7770;aurora;<key>;<value> BEL`) on stdout -- the console byte
// stream aurora drains -- ALWAYS preceded by a `reset system` verb, so every
// push is deterministic: system defaults + the user's overrides (a stale
// prior-session push dies at the next login's push). SESSION-SCOPED by
// scripture: aurora applies without persisting; only the F10 OSD writes the
// system file. login runs this at session start (post /env seed, pre shell);
// running it by hand re-applies anytime. On a serial console the host
// terminal swallows the unknown OSC -- harmless everywhere.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;

use alloc::format;
use alloc::string::String;
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io::Read;
use libthyla_rs::t_write;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: aurora-push [FILE]
  Push the per-user renderer config (default $HOME/lib/aurora) to the
  Aurora console renderer, in-band. Always resets to the system defaults
  first, then applies the file's `key value` lines (theme, cursor-blink).
  Session-scoped: nothing is persisted; the F10 OSD owns the system file.
  --help  show this help

Examples:
  aurora-push                       # push $HOME/lib/aurora
  aurora-push /tmp/preview-config   # try a config without installing it
";

const READ_MAX: usize = 4096; // the same bound aurora's own loader uses

fn emit(out: &mut String, k: &str, v: &str) {
    // Defensive: a key/value carrying the OSC's own structure or a control
    // byte is skipped (no valid setting contains one; aurora would reject
    // it anyway -- fail-soft on both ends).
    if k.is_empty()
        || k.contains(';')
        || v.contains(';')
        || k.bytes().chain(v.bytes()).any(|b| b < 0x20)
    {
        return;
    }
    out.push_str(&format!("\x1b]7770;aurora;{};{}\x07", k, v));
}

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let path: String = match args.get_str(1) {
        Some(p) => String::from(p),
        None => match env::var("HOME") {
            Some(h) => format!("{}/lib/aurora", h),
            None => return 0, // no home in /env: nothing to push (fail-soft)
        },
    };

    // The reset ALWAYS goes out -- even with no per-user file, the session
    // must start from the system defaults (the deterministic-session-start
    // half of the contract).
    let mut out = String::new();
    emit(&mut out, "reset", "system");

    if let Ok(mut f) = File::open(&path) {
        let mut buf = [0u8; READ_MAX];
        let mut n = 0usize;
        loop {
            match f.read(&mut buf[n..]) {
                Ok(0) => break,
                Ok(k) => {
                    n += k;
                    if n == READ_MAX {
                        break; // oversize: push the bounded prefix (fail-soft)
                    }
                }
                Err(_) => break,
            }
        }
        if let Ok(text) = core::str::from_utf8(&buf[..n]) {
            for line in text.lines() {
                let line = line.trim();
                if line.is_empty() || line.starts_with('#') {
                    continue;
                }
                if let Some((k, v)) = line.split_once(char::is_whitespace) {
                    emit(&mut out, k, v.trim());
                }
            }
        }
    }
    // An absent/unreadable file still pushed the reset above -- the normal
    // no-per-user-config case.

    let _ = unsafe { t_write(1, out.as_ptr(), out.len()) };
    0
}
