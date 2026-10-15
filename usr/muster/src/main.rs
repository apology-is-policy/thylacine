// /var/lib_tests/bin/muster -- runs one library's upstream test list on the
// device (Boosty B-3; dec-2026-10-14-b3-test-bar).
//
// The list is upstream's own: tools/libtests-manifest.py writes it from the
// library's CTest registration, so it cannot drift from what upstream runs.
// The manifest is line-oriented, one keyword and the rest of the line:
//   lib <name>       the library, first and once
//   count <n>        how many tests follow; a file holding another number of
//                    tests is refused, so a truncated or padded list is red
//   logs <dir>       each test's stdout and stderr land here, as
//                    <test>.out / <test>.err (created if absent)
//   test <name>      opens a test
//   cwd <dir>        its working directory (the child inherits it)
//   arg <word>       one argv element, in order; the first is the program
//   builtin copy     instead of a program: copy arg 1 to arg 2 (CMake's
//                    `-E copy_if_different`, which a test uses as setup)
//   builtin cmp      instead of a program: pass when arg 1 and arg 2 hold the
//                    same bytes (CMake's `-E compare_files`)
//   builtin sha256   instead of a program: pass when the file arg 2 has the
//                    SHA-256 digest arg 1, in lowercase hex (CMake's
//                    `-E sha256sum` under a PASS_REGULAR_EXPRESSION)
//   end              closes the test
// Tests run in file order, which the translator made DEPENDS-respecting.
// stdin is an empty file, so a test that reads it sees EOF, not the console.
//
// A test passes when it exits 0. A failing test prints its name, its status
// and the tails of its two logs; the last line is the verdict --
// "muster: <lib>: <n> of <n> passed", or "... <k> of <n> FAILED" with status 1.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::env;
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, File, OpenOptions};
use libthyla_rs::io::{Read, Write};
use libthyla_rs::process::{Command, Stdio};
use sha2::{Digest, Sha256};

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

// How much of each log a failing test prints: a failure summary, not the log.
const TAIL: usize = 1536;

// To fd 1, so a shell redirect captures the run (t_putstr is the console).
fn say(s: &str) {
    let _ = libthyla_rs::io::stdout().write_all(s.as_bytes());
}

#[derive(PartialEq)]
enum Builtin {
    None,
    Copy,
    Cmp,
    Sha256,
}

struct Test {
    name: String,
    cwd: String,
    argv: Vec<String>,
    builtin: Builtin,
}

struct Manifest {
    lib: String,
    logs: String,
    tests: Vec<Test>,
}

fn parse(text: &str) -> Result<Manifest, String> {
    let mut lib: Option<String> = None;
    let mut count: Option<usize> = None;
    let mut logs: Option<String> = None;
    let mut tests: Vec<Test> = Vec::new();
    let mut open: Option<Test> = None;
    for (n, line) in text.lines().enumerate() {
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let (key, rest) = line.split_once(' ').unwrap_or((line, ""));
        let at = || format!("line {}: ", n + 1);
        match (key, open.as_mut()) {
            ("lib", None) if lib.is_none() && tests.is_empty() => lib = Some(String::from(rest)),
            ("count", None) => {
                count = Some(rest.parse().map_err(|_| format!("{}count {:?} is not a number", at(), rest))?)
            }
            ("logs", None) => logs = Some(String::from(rest)),
            ("test", None) => {
                open = Some(Test { name: String::from(rest), cwd: String::new(), argv: Vec::new(), builtin: Builtin::None })
            }
            ("cwd", Some(t)) => t.cwd = String::from(rest),
            ("arg", Some(t)) => t.argv.push(String::from(rest)),
            ("builtin", Some(t)) if t.builtin == Builtin::None && rest == "copy" => t.builtin = Builtin::Copy,
            ("builtin", Some(t)) if t.builtin == Builtin::None && rest == "cmp" => t.builtin = Builtin::Cmp,
            ("builtin", Some(t)) if t.builtin == Builtin::None && rest == "sha256" => t.builtin = Builtin::Sha256,
            ("end", Some(_)) => {
                let t = open.take().unwrap();
                if t.name.is_empty() || t.cwd.is_empty() {
                    return Err(format!("{}test {:?} has no name or no cwd", at(), t.name));
                }
                if (t.builtin != Builtin::None && t.argv.len() != 2) || t.argv.is_empty() {
                    return Err(format!("{}test {:?} has a malformed argv", at(), t.name));
                }
                tests.push(t);
            }
            _ => return Err(format!("{}unexpected {:?}", at(), line)),
        }
    }
    if open.is_some() {
        return Err(String::from("the last test has no end"));
    }
    let lib = lib.ok_or_else(|| String::from("no lib line"))?;
    let logs = logs.ok_or_else(|| String::from("no logs line"))?;
    match count {
        Some(c) if c == tests.len() => Ok(Manifest { lib, logs, tests }),
        Some(c) => Err(format!("count says {} tests, the file holds {}", c, tests.len())),
        None => Err(String::from("no count line")),
    }
}

fn read_file(path: &str) -> Result<Vec<u8>, Error> {
    let mut f = File::open(path)?;
    let mut buf = Vec::new();
    f.read_to_end(&mut buf)?;
    Ok(buf)
}

fn create(path: &str) -> Result<File, Error> {
    OpenOptions::new().write(true).create(true).truncate(true).open(path)
}

fn copy(from: &str, to: &str) -> Result<(), Error> {
    let bytes = read_file(from)?;
    create(to)?.write_all(&bytes)
}

// Prints the end of a log, starting at a line boundary when one is in reach.
fn tail(label: &str, path: &str) {
    let Ok(bytes) = read_file(path) else { return };
    if bytes.is_empty() {
        return;
    }
    let mut from = bytes.len().saturating_sub(TAIL);
    if from > 0 {
        if let Some(nl) = bytes[from..].iter().position(|&b| b == b'\n') {
            from += nl + 1;
        }
    }
    say(&format!("  --- {} (last {} bytes) ---\n", label, bytes.len() - from));
    say(&String::from_utf8_lossy(&bytes[from..]));
    if bytes.last() != Some(&b'\n') {
        say("\n");
    }
}

// Answers the test's exit status, or why it could not be run.
fn run(t: &Test, out: &str, err: &str, empty: &str) -> Result<i32, String> {
    // Before a builtin too: its paths may be relative to the test's directory.
    env::set_current_dir(&t.cwd).map_err(|e| format!("chdir {}: {}", t.cwd, e))?;
    match t.builtin {
        Builtin::Copy => {
            return copy(&t.argv[0], &t.argv[1])
                .map(|()| 0)
                .map_err(|e| format!("copy {} -> {}: {}", t.argv[0], t.argv[1], e));
        }
        Builtin::Cmp => {
            let a = read_file(&t.argv[0]).map_err(|e| format!("read {}: {}", t.argv[0], e))?;
            let b = read_file(&t.argv[1]).map_err(|e| format!("read {}: {}", t.argv[1], e))?;
            return if a == b {
                Ok(0)
            } else {
                Err(format!("{} ({} bytes) and {} ({} bytes) differ", t.argv[0], a.len(), t.argv[1], b.len()))
            };
        }
        Builtin::Sha256 => {
            let bytes = read_file(&t.argv[1]).map_err(|e| format!("read {}: {}", t.argv[1], e))?;
            let got: String = Sha256::digest(&bytes).iter().map(|b| format!("{:02x}", b)).collect();
            return if got == t.argv[0] {
                Ok(0)
            } else {
                Err(format!("{} ({} bytes) has SHA-256 {}, not {}", t.argv[1], bytes.len(), got, t.argv[0]))
            };
        }
        Builtin::None => {}
    }
    let so = create(out).map_err(|e| format!("create {}: {}", out, e))?;
    let se = create(err).map_err(|e| format!("create {}: {}", err, e))?;
    let si = File::open(empty).map_err(|e| format!("open {}: {}", empty, e))?;
    let mut child = Command::new(t.argv[0].as_str())
        .args(t.argv[1..].iter().map(String::as_str))
        .stdin(Stdio::File(si))
        .stdout(Stdio::File(so))
        .stderr(Stdio::File(se))
        .spawn()
        .map_err(|e| format!("spawn {}: {}", t.argv[0], e))?;
    let status = child.wait().map_err(|e| format!("wait: {}", e))?;
    Ok(status.raw())
}

fn log_name(test: &str) -> String {
    test.chars().map(|c| if c == '/' || c == ' ' { '_' } else { c }).collect()
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();
    let path = match args.get_str(1) {
        Some(p) if args.len() == 2 => p,
        _ => {
            say("usage: muster <manifest>\n");
            return 2;
        }
    };
    let text = match read_file(path) {
        Ok(b) => b,
        Err(e) => {
            say(&format!("muster: {}: {}\n", path, e));
            return 2;
        }
    };
    let m = match core::str::from_utf8(&text).map_err(|_| String::from("not UTF-8")).and_then(parse) {
        Ok(m) => m,
        Err(e) => {
            say(&format!("muster: {}: {}\n", path, e));
            return 2;
        }
    };
    match fs::create_dir(m.logs.as_str()) {
        Ok(()) | Err(Error::Exists) => {}
        Err(e) => {
            say(&format!("muster: logs {}: {}\n", m.logs, e));
            return 2;
        }
    }
    let empty = format!("{}/stdin.empty", m.logs);
    if let Err(e) = create(&empty) {
        say(&format!("muster: {}: {}\n", empty, e));
        return 2;
    }

    let mut failed = 0usize;
    for t in &m.tests {
        let base = format!("{}/{}", m.logs, log_name(&t.name));
        let (out, err) = (format!("{}.out", base), format!("{}.err", base));
        match run(t, &out, &err, &empty) {
            Ok(0) => {}
            Ok(status) => {
                failed += 1;
                say(&format!("muster: FAIL {}: exit status {}\n", t.name, status));
                tail("stdout", &out);
                tail("stderr", &err);
            }
            Err(why) => {
                failed += 1;
                say(&format!("muster: FAIL {}: {}\n", t.name, why));
            }
        }
    }
    let n = m.tests.len();
    if failed == 0 {
        say(&format!("muster: {}: {} of {} passed\n", m.lib, n, n));
        0
    } else {
        say(&format!("muster: {}: {} of {} FAILED\n", m.lib, failed, n));
        1
    }
}
