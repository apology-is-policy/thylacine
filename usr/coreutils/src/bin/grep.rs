// grep [-cilnorvw] [--color[=WHEN]] [--beacon=WHEN] PATTERN [FILE...] --
// print matching lines.
//
// "simple" per the roadmap: PATTERN is a LITERAL substring, not a regex (no
// regex engine in libthyla-rs). -i case-insensitive, -v invert, -n line
// numbers, -c count-only, -w whole-word match, -o print only the matched part,
// -l print only names of files with a match, -r recurse into directories. No
// operand FILE reads stdin. Input is read a line at a time (coreutils::stream),
// so a file of any length is searched; only its longest line must fit.
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
use coreutils::{find, palette, stream, usage};
use libthyla_rs::env::{self, Args};
use libthyla_rs::err;
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

/// Emit one match's bytes as the flags say: at Rich, an `em class=strong`
/// frame around the SAME bytes (strip-clean); with colour on, bold ember
/// (SGR); otherwise plain. `frame` is scratch for the frame bytes.
fn emit_match(out: &mut io::OutSink, m: &[u8], f: &Flags, frame: &mut Vec<u8>) {
    if f.rich {
        frame.clear();
        beacon::wire::open(frame, beacon::wire::Op::Em, &[("class", "strong")]);
        out.put(frame);
        out.put(m);
        frame.clear();
        beacon::wire::close(frame, beacon::wire::Op::Em);
        out.put(frame);
    } else if f.on {
        let _ = write!(out, "{}{}", palette::BOLD, palette::EMBER);
        out.put(m);
        let _ = write!(out, "{}", palette::RESET);
    } else {
        out.put(m);
    }
}

/// Emit `line` with each match of `pat` styled as it is found (a line can
/// hold as many matches as bytes, so they are never collected).
fn emit_line(out: &mut io::OutSink, line: &[u8], pat: &[u8], f: &Flags) {
    let mut frame = Vec::new();
    let mut last = 0;
    find::each_match(line, pat, f.ci, f.word, |s, e| {
        out.put(&line[last..s]);
        emit_match(out, &line[s..e], f, &mut frame);
        last = e;
        !out.failed()
    });
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
    // SGR highlight, and the Rich tier's frames (the second turns the first off).
    on: bool,
    rich: bool,
}

/// The filename (slate) + `:` and, with -n, the line number (moss) + `:` that
/// prefix a matching line/match. Byte-clean when color is off. At Rich the
/// filename carries an `obj type=path` frame (cleaned absolute ref; no frame
/// when the ref cannot be canonicalized) around the SAME shown text.
fn emit_prefix(out: &mut io::OutSink, prefix: Option<&str>, n: usize, f: &Flags) {
    let (on, rich) = (f.on, f.rich);
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
    if f.number {
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

/// Grep one input a line at a time (coreutils::stream), holding only the line
/// being searched. Returns the match count. With -o, emits each matched span on
/// its own line; with -l, stops reading at the first match (the caller prints
/// the filename). -c output is the caller's job.
fn grep_input<R: io::Read + ?Sized>(out: &mut io::OutSink, input: &mut R, pat: &[u8], f: &Flags, prefix: Option<&str>) -> Result<usize, stream::Error<err::Error>> {
    let mut matches = 0usize;
    stream::lines(
        |buf| input.read(buf),
        |line, n| {
            if find::has_match(line, pat, f.ci, f.word) == f.invert {
                return true;
            }
            matches += 1;
            // One match suffices: -l prints only the name, and once the reader
            // is gone only the verdict is left to find.
            if f.list || out.reader_gone() {
                return false;
            }
            if f.count {
                return true; // the caller prints the total
            }
            if f.only {
                // Only the matched substrings (an inverted line has none -> nothing).
                let mut frame = Vec::new();
                find::each_match(line, pat, f.ci, f.word, |s, e| {
                    emit_prefix(out, prefix, n, f);
                    emit_match(out, &line[s..e], f, &mut frame);
                    out.put(b"\n");
                    !out.failed()
                });
            } else {
                emit_prefix(out, prefix, n, f);
                // A styled realization marks the matches: SGR highlight (color
                // on) or the Rich em frames. A plain line needs no search for
                // them, and an inverted line has none.
                if (f.on || f.rich) && !f.invert {
                    emit_line(out, line, pat, f);
                } else {
                    out.put(line);
                }
                out.put(b"\n");
            }
            // Nothing more reaches stdout once a write has failed.
            !out.failed()
        },
    )?;
    Ok(matches)
}

/// How a path reached grep: named on the command line (and whether its lines
/// carry the name), or found under a directory by -r.
#[derive(Clone, Copy)]
enum Operand {
    Named { prefix: bool },
    Found,
}

/// Grep a path: a file is searched; a directory recurses under -r (else an
/// error). `found` is whether a line was selected before it. Returns
/// `(any_match, had_error)`.
fn grep_path(out: &mut io::OutSink, path: &str, pat: &[u8], f: &Flags, how: Operand, found: bool) -> (bool, bool) {
    let show_prefix = matches!(how, Operand::Found | Operand::Named { prefix: true });
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
                if stopped(out, found || any) {
                    break;
                }
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
                let (a, e) = grep_path(out, &join(path, name), pat, f, Operand::Found, found || any);
                any |= a;
                err |= e;
            }
            (any, err)
        }
        // A device -r finds is skipped, as GNU grep skips it: /dev/zero has no
        // end and no lines. Anything else is read, including a file whose
        // server reports no type.
        Ok(m) if matches!(how, Operand::Found) && m.is_char_device() => (false, false),
        Ok(_) => match File::open(path)
            .map_err(stream::Error::Read)
            .and_then(|mut fh| grep_input(out, &mut fh, pat, f, show_prefix.then_some(path)))
        {
            Ok(m) => {
                if f.list {
                    if m > 0 {
                        if f.rich {
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
                            let _ = writeln!(out, "{}{}{}", color::col(palette::SLATE, f.on), path, color::reset(f.on));
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

/// Whether the search is over once stdout has failed. A reader that went away
/// leaves the verdict still to find if nothing was selected yet (`-c` writes a
/// file's count of none): the rest is searched without output to a first
/// match, as GNU's `-q` searches. With no match that reads all the command
/// would have read for a reader; after one, no later operand (nor its error) is
/// reached.
fn stopped(out: &io::OutSink, found: bool) -> bool {
    out.failed() && (found || !out.reader_gone())
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
        on: false,
        rich: false,
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
    f.rich = coreutils::beacon_gate::resolve(bmode) == beacon::Tier::Rich;
    f.on = !f.rich && mode.resolve(stdout_is_console);

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
        match grep_input(&mut out, &mut io::stdin(), pat, &f, None) {
            Ok(m) => {
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
            if stopped(&out, any_match) {
                break;
            }
            let (a, e) = grep_path(&mut out, path, pat, &f, Operand::Named { prefix: show_prefix }, any_match);
            any_match |= a;
            status_err |= e;
        }
    }

    // A reader that went away had what it wanted; any other failed write is an
    // error.
    if out.failed() && !out.reader_gone() {
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
