// cat [-AbEnstTuv] [FILE...] -- concatenate files (or stdin) to stdout.
//
// With no operands (or "-"), copies stdin. With no flags the plain path
// streams bytes via io::copy (byte-clean -- cat is a pipe payload tool). Any
// of -n/-b (number), -E/-T/-v/-A (show ends/tabs/nonprinting), or -s (squeeze
// blank runs) switches to a line-oriented path: a user-requested transform,
// still plain text and pipe-safe. The line counter and the squeeze state are
// continuous across every operand. Absolute paths only (no cwd resolution).

#![no_std]
#![no_main]

extern crate alloc;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env::{self, Args};
use libthyla_rs::err::Result;
use libthyla_rs::fs::File;
use libthyla_rs::io::{self, BufRead, BufReader, Read, Write};
use libthyla_rs::eprintln;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: cat [-AbEnstTuv] [FILE...]
  Concatenate FILE(s) (or stdin) to stdout.
  -n      number all output lines
  -b      number nonblank output lines (overrides -n)
  -s      squeeze repeated blank output lines
  -E      show line ends as '$'
  -T      show tabs as '^I'
  -v      show nonprinting characters (^X and M- notation)
  -e      = -vE       -t      = -vT       -A      = -vET
  -u      ignored (always unbuffered)
  --help  show this help

Examples:
  cat file              # print a file
  cat -n file           # with line numbers
  cat a b > both        # concatenate two files
";

/// Which line transforms are active. `line_mode()` is true when any of them
/// is set (otherwise cat takes the raw byte-copy fast path).
#[derive(Default)]
struct Opts {
    number: bool,
    number_nonblank: bool,
    squeeze: bool,
    show_ends: bool,
    show_tabs: bool,
    show_nonprint: bool,
}

impl Opts {
    fn line_mode(&self) -> bool {
        self.number
            || self.number_nonblank
            || self.squeeze
            || self.show_ends
            || self.show_tabs
            || self.show_nonprint
    }
}

/// Output state carried across every operand (continuous numbering + squeeze).
#[derive(Default)]
struct State {
    lineno: u64,
    prev_blank: bool,
}

fn run(args: Args) -> i64 {
    if let Some(rc) = coreutils::usage::help_if_requested(args, USAGE) {
        return rc;
    }

    let mut idx = 1;
    let mut opts = Opts::default();
    while let Some(a) = args.get_str(idx) {
        if a == "--" {
            idx += 1;
            break;
        }
        if a.starts_with('-') && a.len() > 1 {
            for ch in a[1..].chars() {
                match ch {
                    'n' => opts.number = true,
                    'b' => opts.number_nonblank = true,
                    's' => opts.squeeze = true,
                    'E' => opts.show_ends = true,
                    'T' => opts.show_tabs = true,
                    'v' => opts.show_nonprint = true,
                    'e' => {
                        opts.show_nonprint = true;
                        opts.show_ends = true;
                    }
                    't' => {
                        opts.show_nonprint = true;
                        opts.show_tabs = true;
                    }
                    'A' => {
                        opts.show_nonprint = true;
                        opts.show_ends = true;
                        opts.show_tabs = true;
                    }
                    'u' => {} // unbuffered: already effectively so
                    _ => {
                        eprintln!("cat: invalid option -- '{}'", ch);
                        return 1;
                    }
                }
            }
            idx += 1;
        } else {
            break;
        }
    }

    let mut status = 0;
    let mut out = io::stdout();
    let mut had = false;
    let mut st = State::default();

    let mut i = idx;
    while let Some(op) = args.get(i) {
        i += 1;
        had = true;
        let path = match core::str::from_utf8(op) {
            Ok(p) => p,
            Err(_) => {
                eprintln!("cat: invalid UTF-8 in path operand");
                status = 1;
                continue;
            }
        };
        if path == "-" {
            if let Err(e) = cat_reader(io::stdin(), &mut out, &opts, &mut st) {
                eprintln!("cat: -: {}", e);
                status = 1;
            }
            continue;
        }
        match File::open(path) {
            Ok(f) => {
                if let Err(e) = cat_reader(f, &mut out, &opts, &mut st) {
                    eprintln!("cat: {}: {}", path, e);
                    status = 1;
                }
            }
            Err(e) => {
                eprintln!("cat: {}: {}", path, e);
                status = 1;
            }
        }
    }

    if !had {
        if let Err(e) = cat_reader(io::stdin(), &mut out, &opts, &mut st) {
            eprintln!("cat: stdin: {}", e);
            status = 1;
        }
    }
    status
}

/// Stream one source to `out`: byte-for-byte when no transform is active, or
/// line-oriented (numbering / squeeze / show-ends-tabs-nonprinting) when one
/// is, continuing the cross-operand `st`.
fn cat_reader<R: Read>(r: R, out: &mut impl Write, opts: &Opts, st: &mut State) -> Result<()> {
    if !opts.line_mode() {
        let mut r = r;
        return io::copy(&mut r, out).map(|_| ());
    }
    let mut br = BufReader::new(r);
    let mut line: Vec<u8> = Vec::new();
    loop {
        line.clear();
        if br.read_until(b'\n', &mut line)? == 0 {
            break; // EOF
        }
        let is_blank = line.as_slice() == b"\n";
        // -s: collapse a run of blank lines to a single one.
        if opts.squeeze && is_blank && st.prev_blank {
            continue;
        }
        st.prev_blank = is_blank;
        // Numbering: -b numbers nonblank only (and subsumes -n); -n numbers all.
        let number_this = if opts.number_nonblank {
            !is_blank
        } else {
            opts.number
        };
        if number_this {
            st.lineno += 1;
            write!(out, "{:6}\t", st.lineno)?;
        }
        emit_content(out, &line, opts)?;
    }
    Ok(())
}

/// Emit one line's content with -E/-T/-v transforms. `line` includes its
/// trailing '\n' (if any); the newline is emitted last, preceded by '$' under
/// -E. A final line without a newline gets neither.
fn emit_content(out: &mut impl Write, line: &[u8], opts: &Opts) -> Result<()> {
    let has_nl = line.last() == Some(&b'\n');
    let content = if has_nl { &line[..line.len() - 1] } else { line };
    if opts.show_tabs || opts.show_nonprint {
        for &b in content {
            match b {
                b'\t' if opts.show_tabs => out.write_all(b"^I")?,
                // -v leaves TAB literal (only -T renders it); printable ASCII
                // passes through; other controls/high-bit render under -v.
                b'\t' | 0x20..=0x7e => out.write_all(&[b])?,
                _ if opts.show_nonprint => write_nonprint(out, b)?,
                _ => out.write_all(&[b])?, // -T only: non-tab controls stay literal
            }
        }
    } else {
        out.write_all(content)?;
    }
    if opts.show_ends && has_nl {
        out.write_all(b"$")?;
    }
    if has_nl {
        out.write_all(b"\n")?;
    }
    Ok(())
}

/// Render one nonprinting byte in GNU `cat -v` notation: a high bit becomes a
/// `M-` prefix over the low 7 bits; a control char becomes `^X` (X = c+0x40);
/// DEL (0x7f) becomes `^?`.
fn write_nonprint(out: &mut impl Write, b: u8) -> Result<()> {
    let mut buf = [0u8; 4];
    let mut n = 0;
    let mut c = b;
    if c >= 0x80 {
        buf[n] = b'M';
        buf[n + 1] = b'-';
        n += 2;
        c &= 0x7f;
    }
    if c < 0x20 {
        buf[n] = b'^';
        buf[n + 1] = c + 0x40;
        n += 2;
    } else if c == 0x7f {
        buf[n] = b'^';
        buf[n + 1] = b'?';
        n += 2;
    } else {
        buf[n] = c;
        n += 1;
    }
    out.write_all(&buf[..n])
}
