// sort [-bfnru] [-t SEP] [-k KEYDEF]... [FILE...] -- sort lines.
//
// Default: whole-line lexical. -n numeric, -f fold case, -b ignore leading
// blanks, -r reverse, -u drop adjacent-after-sort duplicates. -t SEP sets the
// field separator (default: runs of blanks, awk-style fields). -k F[.C][opts]
// [,F[.C][opts]] sorts on a key spanning field F (1-based) char C through end
// field/char, with per-key opts n/r/f/b; multiple -k are primary, secondary,
// ... with a whole-line last resort. Reads all inputs into memory.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::cmp::Ordering;
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::File;
use libthyla_rs::io;
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

#[derive(Clone, Copy, Default)]
struct Mods {
    numeric: bool,
    reverse: bool,
    fold: bool,
    blanks: bool,
}

/// A sort key: field F (1-based) char C (1-based) through an optional end.
struct Key {
    sf: usize,
    sc: usize,
    ef: Option<usize>,
    ec: Option<usize>,
    mods: Mods,
}

fn is_blank(b: u8) -> bool {
    b == b' ' || b == b'\t'
}

/// The slice past any leading blanks (when `blanks` is set), else the slice.
fn lead_strip(line: &[u8], blanks: bool) -> &[u8] {
    if !blanks {
        return line;
    }
    let mut s = 0;
    while s < line.len() && is_blank(line[s]) {
        s += 1;
    }
    &line[s..]
}

/// Leading numeric value of a slice (sign + digits after trimming blanks);
/// non-numeric -> 0. Integer only (-g float sort is deferred).
fn num_of(s: &[u8]) -> i64 {
    let t = core::str::from_utf8(s).unwrap_or("").trim_start();
    let b = t.as_bytes();
    let mut end = 0;
    if end < b.len() && (b[end] == b'-' || b[end] == b'+') {
        end += 1;
    }
    while end < b.len() && b[end].is_ascii_digit() {
        end += 1;
    }
    t[..end].parse::<i64>().unwrap_or(0)
}

/// Lexical compare, optionally ASCII-case-folded.
fn cmp_bytes(a: &[u8], b: &[u8], fold: bool) -> Ordering {
    if !fold {
        return a.cmp(b);
    }
    let mut ai = a.iter();
    let mut bi = b.iter();
    loop {
        match (ai.next(), bi.next()) {
            (Some(x), Some(y)) => {
                let (xl, yl) = (x.to_ascii_lowercase(), y.to_ascii_lowercase());
                if xl != yl {
                    return xl.cmp(&yl);
                }
            }
            (None, None) => return Ordering::Equal,
            (None, _) => return Ordering::Less,
            (_, None) => return Ordering::Greater,
        }
    }
}

/// Byte ranges of each field. With a separator each `sep` byte ends a field
/// (N separators -> N+1 fields, empties included); without, fields are maximal
/// runs of non-blank bytes.
fn field_bounds(line: &[u8], sep: Option<u8>) -> Vec<(usize, usize)> {
    let mut v = Vec::new();
    match sep {
        Some(s) => {
            let mut start = 0;
            for (i, &b) in line.iter().enumerate() {
                if b == s {
                    v.push((start, i));
                    start = i + 1;
                }
            }
            v.push((start, line.len()));
        }
        None => {
            let mut i = 0;
            while i < line.len() {
                while i < line.len() && is_blank(line[i]) {
                    i += 1;
                }
                if i >= line.len() {
                    break;
                }
                let start = i;
                while i < line.len() && !is_blank(line[i]) {
                    i += 1;
                }
                v.push((start, i));
            }
        }
    }
    v
}

/// Extract a key's byte slice from `line` per `key` and the field model.
fn extract<'a>(line: &'a [u8], key: &Key, sep: Option<u8>) -> &'a [u8] {
    let fields = field_bounds(line, sep);
    let (s0, s1) = match fields.get(key.sf - 1) {
        Some(&f) => f,
        None => return &[], // start field beyond the line -> empty key
    };
    let mut fstart = s0;
    if key.mods.blanks {
        while fstart < s1 && is_blank(line[fstart]) {
            fstart += 1;
        }
    }
    let start = (fstart + key.sc - 1).min(s1);
    let end = match key.ef {
        None => line.len(), // no end field -> to end of line
        Some(ef) => match fields.get(ef - 1) {
            Some(&(e0, e1)) => match key.ec {
                None | Some(0) => e1, // whole end field
                Some(c) => (e0 + c).min(e1),
            },
            None => line.len(),
        },
    };
    let start = start.min(end);
    &line[start..end]
}

/// Parse one position spec `F[.C][opts]` (e.g. `2`, `2.3`, `3nr`).
fn parse_pos(s: &str) -> Option<(usize, Option<usize>, Mods)> {
    let b = s.as_bytes();
    let mut i = 0;
    let mut field = 0usize;
    while i < b.len() && b[i].is_ascii_digit() {
        field = field.checked_mul(10)?.checked_add((b[i] - b'0') as usize)?;
        i += 1;
    }
    if field == 0 {
        return None; // fields are 1-based
    }
    let mut ch = None;
    if i < b.len() && b[i] == b'.' {
        i += 1;
        let mut c = 0usize;
        while i < b.len() && b[i].is_ascii_digit() {
            c = c.checked_mul(10)?.checked_add((b[i] - b'0') as usize)?;
            i += 1;
        }
        ch = Some(c);
    }
    let mut m = Mods::default();
    while i < b.len() {
        match b[i] {
            b'n' => m.numeric = true,
            b'r' => m.reverse = true,
            b'f' => m.fold = true,
            b'b' => m.blanks = true,
            _ => return None,
        }
        i += 1;
    }
    Some((field, ch, m))
}

/// Parse a full KEYDEF `start[,end]` against the global ordering options.
fn parse_key(s: &str, global: Mods) -> Option<Key> {
    let mut it = s.splitn(2, ',');
    let (sf, sc, sm) = parse_pos(it.next().unwrap_or(""))?;
    let (ef, ec) = match it.next() {
        Some(e) => {
            let (f, c, _) = parse_pos(e)?;
            (Some(f), c)
        }
        None => (None, None),
    };
    // Per-key options override; the global option applies to any a key lacks
    // (GNU). Since the only override is "set", OR-ing global into the key is
    // exactly that rule.
    let mods = Mods {
        numeric: global.numeric || sm.numeric,
        reverse: global.reverse || sm.reverse,
        fold: global.fold || sm.fold,
        blanks: global.blanks || sm.blanks,
    };
    Some(Key {
        sf,
        sc: sc.unwrap_or(1).max(1),
        ef,
        ec,
        mods,
    })
}

const USAGE: &str = "\
usage: sort [-bfnru] [-t SEP] [-k KEYDEF]... [FILE...]
  Sort lines of text.
  -n      compare by leading numeric value
  -f      fold case (case-insensitive)
  -b      ignore leading blanks in keys
  -r      reverse the result
  -u      output only the first of an equal run
  -t SEP  field separator (default: runs of blanks)
  -k KEY  sort key F[.C][opts][,F[.C][opts]], opts in {n,r,f,b}; repeatable
  --help  show this help

Examples:
  sort file                 # lexical
  sort -rn nums             # numeric, descending
  sort -t: -k3 -n /etc/passwd  # by the 3rd colon-field, numeric
";

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut global = Mods::default();
    let mut uniq = false;
    let mut sep: Option<u8> = None;
    let mut key_defs: Vec<&str> = Vec::new();
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if !(a.starts_with('-') && a != "-" && a.len() > 1) {
            break;
        }
        let bytes = a.as_bytes();
        let mut j = 1;
        while j < bytes.len() {
            match bytes[j] {
                b'r' => global.reverse = true,
                b'n' => global.numeric = true,
                b'f' => global.fold = true,
                b'b' => global.blanks = true,
                b'u' => uniq = true,
                b't' => {
                    let arg = if j + 1 < a.len() {
                        &a[j + 1..]
                    } else {
                        idx += 1;
                        match args.get_str(idx) {
                            Some(s) => s,
                            None => {
                                eprintln!("sort: option requires an argument -- 't'");
                                return 1;
                            }
                        }
                    };
                    if arg.len() != 1 {
                        eprintln!("sort: -t requires a single-character separator");
                        return 1;
                    }
                    sep = Some(arg.as_bytes()[0]);
                    break; // rest of this arg was the separator
                }
                b'k' => {
                    let arg = if j + 1 < a.len() {
                        &a[j + 1..]
                    } else {
                        idx += 1;
                        match args.get_str(idx) {
                            Some(s) => s,
                            None => {
                                eprintln!("sort: option requires an argument -- 'k'");
                                return 1;
                            }
                        }
                    };
                    key_defs.push(arg);
                    break; // rest of this arg was the key definition
                }
                other => {
                    eprintln!("sort: invalid option -- '{}'", other as char);
                    return 1;
                }
            }
            j += 1;
        }
        idx += 1;
    }

    // Parse key definitions now that the global options are known.
    let mut keys: Vec<Key> = Vec::with_capacity(key_defs.len());
    for d in &key_defs {
        match parse_key(d, global) {
            Some(k) => keys.push(k),
            None => {
                eprintln!("sort: invalid key definition -- '{}'", d);
                return 1;
            }
        }
    }

    let mut data: Vec<u8> = Vec::new();
    let mut had = false;
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("sort: invalid UTF-8 path");
                return 1;
            }
        };
        match File::open(path).and_then(|mut f| io::slurp(&mut f)) {
            Ok(d) => data.extend_from_slice(&d),
            Err(e) => {
                eprintln!("sort: {}: {}", path, e);
                return 1;
            }
        }
    }
    if !had {
        match io::slurp(&mut io::stdin()) {
            Ok(d) => data = d,
            Err(e) => {
                eprintln!("sort: stdin: {}", e);
                return 1;
            }
        }
    }

    let mut lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    if data.last() == Some(&b'\n') {
        lines.pop();
    }

    if keys.is_empty() {
        // Whole-line compare (the global ordering), then a reverse pass --
        // preserves the historical -n/-r/-u behavior.
        lines.sort_unstable_by(|a, b| {
            let (ka, kb) = (lead_strip(a, global.blanks), lead_strip(b, global.blanks));
            if global.numeric {
                num_of(ka)
                    .cmp(&num_of(kb))
                    .then_with(|| cmp_bytes(ka, kb, global.fold))
            } else {
                cmp_bytes(ka, kb, global.fold)
            }
        });
        if global.reverse {
            lines.reverse();
        }
    } else {
        // Multi-key compare; reverse is baked per-key, with a forward
        // whole-line last resort (only distinguishes all-keys-equal lines).
        lines.sort_unstable_by(|a, b| {
            for k in &keys {
                let (ka, kb) = (extract(a, k, sep), extract(b, k, sep));
                let mut ord = if k.mods.numeric {
                    num_of(ka).cmp(&num_of(kb))
                } else {
                    cmp_bytes(ka, kb, k.mods.fold)
                };
                if k.mods.reverse {
                    ord = ord.reverse();
                }
                if ord != Ordering::Equal {
                    return ord;
                }
            }
            a.cmp(b)
        });
    }

    let mut out = io::OutSink::new();
    let mut prev: Option<&[u8]> = None;
    for line in lines {
        if uniq {
            // -u drops adjacent-equal lines; equality respects the active
            // ordering (a key-equal line is a duplicate for -u).
            let dup = match prev {
                Some(p) => lines_equal(p, line, &keys, sep, global),
                None => false,
            };
            if dup {
                continue;
            }
            prev = Some(line);
        }
        out.put(line);
        out.put(b"\n");
    }
    if out.failed() {
        eprintln!("sort: write error");
        return 1;
    }
    0
}

/// Whether two adjacent output lines are "equal" for -u: all keys compare
/// equal (or, with no keys, the whole line under the global options).
fn lines_equal(a: &[u8], b: &[u8], keys: &[Key], sep: Option<u8>, global: Mods) -> bool {
    if keys.is_empty() {
        return if global.numeric {
            num_of(a) == num_of(b) && cmp_bytes(a, b, global.fold) == Ordering::Equal
        } else {
            cmp_bytes(a, b, global.fold) == Ordering::Equal
        };
    }
    for k in keys {
        let (ka, kb) = (extract(a, k, sep), extract(b, k, sep));
        let eq = if k.mods.numeric {
            num_of(ka) == num_of(kb)
        } else {
            cmp_bytes(ka, kb, k.mods.fold) == Ordering::Equal
        };
        if !eq {
            return false;
        }
    }
    true
}
