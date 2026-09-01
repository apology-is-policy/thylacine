// grep [-cilnorvw] [--color[=WHEN]] [--beacon=WHEN] PATTERN [FILE...] --
// print matching lines.
//
// "simple" per the roadmap: PATTERN is a LITERAL substring, not a regex (no
// regex engine in libthyla-rs). -i case-insensitive, -v invert, -n line
// numbers, -c count-only, -w whole-word match, -o print only the matched part,
// -l print only names of files with a match, -r recurse into directories. No
// operand FILE reads stdin.
//
// Beacon (docs/BEACON.md): at the Rich tier the SAME plain bytes go out,
// bracketed by semantic frames -- `obj type=path` on the filename prefix
// (cleaned absolute ref) and `em class=strong` on each match span. SGR is
// off inside rich-structured output, so strip(rich) == the plain emission
// byte-exactly. Color defaults to auto (the H-1 unification): the console
// highlights, a pipe stays byte-clean.

#![no_std]
#![no_main]

extern crate alloc;
use alloc::string::String;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::fmt::Write as _;
use coreutils::color::{self, ColorMode};
use coreutils::{palette, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::fs::{self, File};
use libthyla_rs::{eprintln, io};

const USAGE: &str = "\
usage: grep [-cilnorvw] [--color[=WHEN]] [--beacon=WHEN] PATTERN [FILE...]
  Print lines matching PATTERN (a literal substring). No FILE reads stdin.
  -i  ignore case        -v  invert (non-matching lines)
  -n  line numbers       -c  count only
  -w  match whole words  -o  print only the matched part (one per line)
  -l  print only names of files that match
  -r  recurse into directories
  --color[=WHEN]  highlight matches (always | never | auto; default auto)
  --beacon=WHEN   semantic markup: auto (default) | always | never
  --help  show this help

Examples:
  grep foo file             # matching lines
  grep -rn foo .            # recurse, with file:line
  ls | grep .txt            # filter a pipe
";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

fn is_word_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'_'
}

/// A `[s,e)` match is word-bounded when neither neighbour is a word byte.
fn word_bounded(hay: &[u8], s: usize, e: usize) -> bool {
    (s == 0 || !is_word_byte(hay[s - 1])) && (e == hay.len() || !is_word_byte(hay[e]))
}

fn matches_at(hay: &[u8], i: usize, needle: &[u8], ci: bool) -> bool {
    if ci {
        hay[i..i + needle.len()]
            .iter()
            .zip(needle)
            .all(|(a, b)| a.eq_ignore_ascii_case(b))
    } else {
        &hay[i..i + needle.len()] == needle
    }
}

fn has_match(hay: &[u8], needle: &[u8], ci: bool, word: bool) -> bool {
    if needle.is_empty() {
        return true;
    }
    if needle.len() > hay.len() {
        return false;
    }
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        if matches_at(hay, i, needle, ci) && (!word || word_bounded(hay, i, i + needle.len())) {
            return true;
        }
        i += 1;
    }
    false
}

/// Byte spans of every non-overlapping occurrence of `needle` in `hay` (for the
/// highlight + -o), honoring -w word boundaries.
fn find_spans(hay: &[u8], needle: &[u8], ci: bool, word: bool) -> Vec<(usize, usize)> {
    let mut spans = Vec::new();
    if needle.is_empty() || needle.len() > hay.len() {
        return spans;
    }
    let mut i = 0;
    while i + needle.len() <= hay.len() {
        if matches_at(hay, i, needle, ci) && (!word || word_bounded(hay, i, i + needle.len())) {
            spans.push((i, i + needle.len()));
            i += needle.len(); // non-overlapping
        } else {
            i += 1;
        }
    }
    spans
}

/// Emit `line`, wrapping each matched span in bold ember (SGR) or, at Rich,
/// in an `em class=strong` frame around the SAME bytes (strip-clean). `spans`
/// empty -> the plain line.
fn emit_line(out: &mut io::OutSink, line: &[u8], spans: &[(usize, usize)], rich: bool) {
    if spans.is_empty() {
        out.put(line);
        return;
    }
    let mut last = 0;
    let mut f: Vec<u8> = Vec::new();
    for &(s, e) in spans {
        out.put(&line[last..s]);
        if rich {
            f.clear();
            beacon::wire::open(&mut f, beacon::wire::Op::Em, &[("class", "strong")]);
            out.put(&f);
            out.put(&line[s..e]);
            f.clear();
            beacon::wire::close(&mut f, beacon::wire::Op::Em);
            out.put(&f);
        } else {
            let _ = write!(out, "{}{}", palette::BOLD, palette::EMBER);
            out.put(&line[s..e]);
            let _ = write!(out, "{}", palette::RESET);
        }
        last = e;
    }
    out.put(&line[last..]);
}

/// `--color=auto`: stdout is the interactive console iff its Dev class is
/// `'c'` (`SYS_FD_DEVCLASS`; H-1 closed the long-parked `true` stub).
fn stdout_is_console() -> bool {
    libthyla_rs::stdout_is_terminal()
}

struct Flags {
    ci: bool,
    invert: bool,
    number: bool,
    count: bool,
    word: bool,
    only: bool,
    list: bool,
    recursive: bool,
}

/// The filename (slate) + `:` and, with -n, the line number (moss) + `:` that
/// prefix a matching line/match. Byte-clean when color is off. At Rich the
/// filename carries an `obj type=path` frame (cleaned absolute ref; no frame
/// when the ref cannot be canonicalized) around the SAME shown text.
fn emit_prefix(out: &mut io::OutSink, prefix: Option<&str>, n: usize, number: bool, on: bool, rich: bool) {
    if let Some(p) = prefix {
        if rich {
            {
                let mut sout = coreutils::beacon_gate::SinkOut(out);
                let mut s = beacon::sink::Sink::new(&mut sout, beacon::Tier::Rich);
                match coreutils::path::abs(p) {
                    Some(r) => s.obj(beacon::sink::ObjType::Path, &r, p),
                    None => s.text(p),
                }
            }
            out.put(b":");
        } else {
            let _ = write!(
                out,
                "{}{}{}{}:{}",
                color::col(palette::SLATE, on),
                p,
                color::reset(on),
                color::col(palette::DIM, on),
                color::reset(on)
            );
        }
    }
    if number {
        let _ = write!(
            out,
            "{}{}{}{}:{}",
            color::col(palette::GREEN, on),
            n + 1,
            color::reset(on),
            color::col(palette::DIM, on),
            color::reset(on)
        );
    }
}

/// Grep one in-memory buffer. Returns the match count. With -o, emits each
/// matched span on its own line; with -l, stops at the first match (the caller
/// prints the filename). -c output is the caller's job.
fn grep_data(out: &mut io::OutSink, data: &[u8], pat: &[u8], f: &Flags, prefix: Option<&str>, on: bool, rich: bool) -> usize {
    let mut lines: Vec<&[u8]> = data.split(|&b| b == b'\n').collect();
    if data.last() == Some(&b'\n') {
        lines.pop();
    }
    let mut matches = 0usize;
    for (n, line) in lines.iter().enumerate() {
        if has_match(line, pat, f.ci, f.word) == f.invert {
            continue;
        }
        matches += 1;
        if f.list {
            break; // one match suffices; the caller prints the name
        }
        if f.count {
            continue; // the caller prints the total
        }
        if f.only {
            // Only the matched substrings (an inverted line has none -> nothing).
            for &(s, e) in &find_spans(line, pat, f.ci, f.word) {
                emit_prefix(out, prefix, n, f.number, on, rich);
                let sub = &line[s..e];
                if on || rich {
                    // The whole emitted text IS the match span (styled).
                    emit_line(out, sub, &[(0, sub.len())], rich);
                } else {
                    out.put(sub);
                }
                out.put(b"\n");
            }
        } else {
            emit_prefix(out, prefix, n, f.number, on, rich);
            // Spans are found when a styled realization wants them: SGR
            // highlight (color on) or the Rich em frames. Inverted lines
            // carry no matched span by definition.
            let spans = if (on || rich) && !f.invert {
                find_spans(line, pat, f.ci, f.word)
            } else {
                Vec::new()
            };
            emit_line(out, line, &spans, rich);
            out.put(b"\n");
        }
    }
    matches
}

/// Grep a path: a regular file is slurped + searched; a directory recurses
/// under -r (else an error). Returns `(any_match, had_error)`.
fn grep_path(out: &mut io::OutSink, path: &str, pat: &[u8], f: &Flags, on: bool, rich: bool, show_prefix: bool) -> (bool, bool) {
    match fs::metadata(path) {
        Ok(m) if m.is_dir() => {
            if !f.recursive {
                eprintln!("grep: {}: Is a directory", path);
                return (false, true);
            }
            let entries = match fs::read_dir(path) {
                Ok(e) => e,
                Err(e) => {
                    eprintln!("grep: {}: {}", path, e);
                    return (false, true);
                }
            };
            let (mut any, mut err) = (false, false);
            for ent in entries {
                let ent = match ent {
                    Ok(e) => e,
                    Err(e) => {
                        eprintln!("grep: {}: {}", path, e);
                        err = true;
                        continue;
                    }
                };
                let name = ent.file_name();
                if name == "." || name == ".." {
                    continue;
                }
                let (a, e) = grep_path(out, &join(path, name), pat, f, on, rich, true);
                any |= a;
                err |= e;
            }
            (any, err)
        }
        Ok(_) => match File::open(path).and_then(|mut fh| io::slurp(&mut fh)) {
            Ok(data) => {
                let prefix = if show_prefix { Some(path) } else { None };
                let m = grep_data(out, &data, pat, f, prefix, on, rich);
                if f.list {
                    if m > 0 {
                        if rich {
                            {
                                let mut sout = coreutils::beacon_gate::SinkOut(out);
                                let mut s = beacon::sink::Sink::new(&mut sout, beacon::Tier::Rich);
                                match coreutils::path::abs(path) {
                                    Some(r) => s.obj(beacon::sink::ObjType::Path, &r, path),
                                    None => s.text(path),
                                }
                            }
                            out.put(b"\n");
                        } else {
                            let _ = writeln!(out, "{}{}{}", color::col(palette::SLATE, on), path, color::reset(on));
                        }
                    }
                } else if f.count {
                    if show_prefix {
                        let _ = writeln!(out, "{}:{}", path, m);
                    } else {
                        let _ = writeln!(out, "{}", m);
                    }
                }
                (m > 0, false)
            }
            Err(e) => {
                eprintln!("grep: {}: {}", path, e);
                (false, true)
            }
        },
        Err(e) => {
            eprintln!("grep: {}: {}", path, e);
            (false, true)
        }
    }
}

fn join(dir: &str, name: &str) -> String {
    let mut s = String::from(dir.trim_end_matches('/'));
    s.push('/');
    s.push_str(name);
    s
}

fn run(args: Args) -> i64 {
    if let Some(rc) = usage::help_if_requested(args, USAGE) {
        return rc;
    }
    let mut idx = 1;
    let mut f = Flags {
        ci: false,
        invert: false,
        number: false,
        count: false,
        word: false,
        only: false,
        list: false,
        recursive: false,
    };
    // Both gates default Auto (the H-1 unification): a pipe is byte-clean
    // by construction (dc != 'c'), the console highlights + may frame.
    let mut mode = ColorMode::Auto; // color iff console (the H-1 SYS_FD_DEVCLASS unification)
    let mut bmode = beacon::BeaconMode::Auto;
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a == "--color" {
            mode = ColorMode::Always;
            idx += 1;
            continue;
        }
        if let Some(when) = a.strip_prefix("--color=") {
            match ColorMode::parse_when(when) {
                Some(m) => mode = m,
                None => {
                    eprintln!("grep: invalid --color value -- '{}'", when);
                    usage::hint("grep");
                    return 2;
                }
            }
            idx += 1;
            continue;
        }
        if a == "--beacon" {
            bmode = beacon::BeaconMode::Always;
            idx += 1;
            continue;
        }
        if let Some(when) = a.strip_prefix("--beacon=") {
            match beacon::BeaconMode::parse_when(when) {
                Some(m) => bmode = m,
                None => {
                    eprintln!("grep: invalid --beacon value -- '{}'", when);
                    usage::hint("grep");
                    return 2;
                }
            }
            idx += 1;
            continue;
        }
        if a.starts_with('-') && a != "-" && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'i' => f.ci = true,
                    'v' => f.invert = true,
                    'n' => f.number = true,
                    'c' => f.count = true,
                    'w' => f.word = true,
                    'o' => f.only = true,
                    'l' => f.list = true,
                    'r' | 'R' => f.recursive = true,
                    _ => {
                        eprintln!("grep: invalid option -- '{}'", ch);
                        usage::hint("grep");
                        return 2;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }
    // The emission gate (BEACON.md 12.4); SGR is off inside rich output.
    let rich = coreutils::beacon_gate::resolve(bmode) == beacon::Tier::Rich;
    let on = !rich && mode.resolve(stdout_is_console);

    let pat = match args.get(idx) {
        Some(p) => p,
        None => {
            eprintln!("grep: missing pattern");
            return 2;
        }
    };
    idx += 1;

    let mut files: Vec<&str> = Vec::new();
    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        match core::str::from_utf8(op) {
            Ok(p) => files.push(p),
            Err(_) => {
                eprintln!("grep: invalid UTF-8 path");
                return 2;
            }
        }
    }

    // Prefix filenames when there is more than one source or we recurse.
    let show_prefix = files.len() > 1 || f.recursive;
    let mut any_match = false;
    let mut status_err = false;
    let mut out = io::OutSink::new();

    if files.is_empty() {
        match io::slurp(&mut io::stdin()) {
            Ok(data) => {
                let m = grep_data(&mut out, &data, pat, &f, None, on, rich);
                if f.list {
                    if m > 0 {
                        let _ = writeln!(out, "(standard input)");
                    }
                } else if f.count {
                    let _ = writeln!(out, "{}", m);
                }
                any_match |= m > 0;
            }
            Err(e) => {
                eprintln!("grep: stdin: {}", e);
                status_err = true;
            }
        }
    } else {
        for path in files {
            let (a, e) = grep_path(&mut out, path, pat, &f, on, rich, show_prefix);
            any_match |= a;
            status_err |= e;
        }
    }

    if out.failed() {
        eprintln!("grep: write error");
        status_err = true;
    }

    if status_err {
        2
    } else if any_match {
        0
    } else {
        1
    }
}
