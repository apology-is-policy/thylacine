// /var/lib_tests/bin/muster -- runs one library's upstream test list on the
// device (Boosty B-3; dec-2026-10-14-b3-test-bar).
//
// The list is upstream's own: tools/libtests-manifest.py writes it from the
// library's CTest registration, so it cannot drift from what upstream runs.
// The manifest is line-oriented, one keyword and the rest of the line:
//   lib <name>       the library, first and once
//   count <n>        how many tests follow; a file holding another number of
//                    tests is refused, so a truncated or padded list is red
//   logs <dir>       each step's stdout and stderr land here, as
//                    <test>.<k>.out / .err, unless the step redirects them
//                    (created if absent)
//   test <name>      opens a test
//   cwd <dir>        its working directory (each child inherits it)
//   step             opens one of the test's steps, which run in order; the
//                    first that fails fails the test
//   arg <word>       one argv element, in order; the first is the program
//   stdin <path>     the step's stdin (default: an empty file, so a test that
//                    reads it sees EOF, not the console)
//   stdout <path>    where its stdout goes, parent directories made
//   stderr <path>    likewise for stderr
//   status any       its exit status is not checked (a step whose outputs
//                    later steps compare)
//   builtin copy     instead of a program: copy arg 1 to arg 2 (CMake's
//                    `-E copy_if_different`)
//   builtin cmp      pass when arg 1 and arg 2 hold the same bytes (CMake's
//                    `-E compare_files`)
//   builtin sha256   pass when the file arg 2 has the SHA-256 digest arg 1,
//                    in lowercase hex (CMake's `-E sha256sum` under a
//                    PASS_REGULAR_EXPRESSION)
//   builtin mkdir    make the directory arg 1 and its parents
//   builtin rmtree   remove arg 1 and everything under it; absent is fine
//   end              closes the test
// Tests run in file order, which the translator made DEPENDS-respecting.
//
// A test passes when every step does. A failing test prints its name, the
// step and why, and the tails of that step's output; the last line is the
// verdict -- "muster: <lib>: <n> of <n> passed", or "... <k> of <n> FAILED"
// with status 1.

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

// How much of each output a failing step prints: a failure summary, not the log.
const TAIL: usize = 1536;

// To fd 1, so a shell redirect captures the run (t_putstr is the console).
fn say(s: &str) {
    let _ = libthyla_rs::io::stdout().write_all(s.as_bytes());
}

#[derive(Clone, Copy, PartialEq)]
enum Builtin {
    None,
    Copy,
    Cmp,
    Sha256,
    Mkdir,
    Rmtree,
}

impl Builtin {
    fn parse(s: &str) -> Option<Builtin> {
        match s {
            "copy" => Some(Builtin::Copy),
            "cmp" => Some(Builtin::Cmp),
            "sha256" => Some(Builtin::Sha256),
            "mkdir" => Some(Builtin::Mkdir),
            "rmtree" => Some(Builtin::Rmtree),
            _ => None,
        }
    }

    fn operands(self) -> usize {
        match self {
            Builtin::None => 0,
            Builtin::Copy | Builtin::Cmp | Builtin::Sha256 => 2,
            Builtin::Mkdir | Builtin::Rmtree => 1,
        }
    }
}

struct Step {
    argv: Vec<String>,
    builtin: Builtin,
    stdin: Option<String>,
    stdout: Option<String>,
    stderr: Option<String>,
    any_status: bool,
}

struct Test {
    name: String,
    cwd: String,
    steps: Vec<Step>,
}

struct Manifest {
    lib: String,
    logs: String,
    tests: Vec<Test>,
}

fn check(t: &Test) -> Result<(), String> {
    if t.name.is_empty() || t.cwd.is_empty() || t.steps.is_empty() {
        return Err(format!("test {:?} has no name, no cwd or no step", t.name));
    }
    for (k, s) in t.steps.iter().enumerate() {
        let ok = if s.builtin == Builtin::None {
            !s.argv.is_empty()
        } else {
            s.argv.len() == s.builtin.operands()
                && s.stdin.is_none()
                && s.stdout.is_none()
                && s.stderr.is_none()
                && !s.any_status
        };
        if !ok {
            return Err(format!("test {:?}: step {} is malformed", t.name, k + 1));
        }
    }
    Ok(())
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
        let bad = || format!("{}unexpected {:?}", at(), line);
        let Some(t) = open.as_mut() else {
            match key {
                "lib" if lib.is_none() && tests.is_empty() => lib = Some(String::from(rest)),
                "count" => {
                    count = Some(rest.parse().map_err(|_| format!("{}count {:?} is not a number", at(), rest))?)
                }
                "logs" => logs = Some(String::from(rest)),
                "test" => open = Some(Test { name: String::from(rest), cwd: String::new(), steps: Vec::new() }),
                _ => return Err(bad()),
            }
            continue;
        };
        match key {
            "cwd" if t.steps.is_empty() => t.cwd = String::from(rest),
            "step" if rest.is_empty() => t.steps.push(Step {
                argv: Vec::new(),
                builtin: Builtin::None,
                stdin: None,
                stdout: None,
                stderr: None,
                any_status: false,
            }),
            "end" => {
                let t = open.take().unwrap();
                check(&t).map_err(|e| format!("{}{}", at(), e))?;
                tests.push(t);
            }
            _ => {
                let Some(s) = t.steps.last_mut() else { return Err(bad()) };
                match key {
                    "arg" => s.argv.push(String::from(rest)),
                    "builtin" if s.builtin == Builtin::None && s.argv.is_empty() => {
                        s.builtin = Builtin::parse(rest).ok_or_else(bad)?
                    }
                    "stdin" if s.stdin.is_none() => s.stdin = Some(String::from(rest)),
                    "stdout" if s.stdout.is_none() => s.stdout = Some(String::from(rest)),
                    "stderr" if s.stderr.is_none() => s.stderr = Some(String::from(rest)),
                    "status" if rest == "any" => s.any_status = true,
                    _ => return Err(bad()),
                }
            }
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

// An existing component is skipped, not created: a mkdir of an existing name
// in a directory the caller cannot write (`/tmp` under `/`) answers
// PermissionDenied here, not Exists.
fn mkdir_p(path: &str) -> Result<(), Error> {
    let mut at = String::from(if path.starts_with('/') { "/" } else { "" });
    for part in path.split('/').filter(|p| !p.is_empty()) {
        if !at.is_empty() && !at.ends_with('/') {
            at.push('/');
        }
        at.push_str(part);
        if fs::is_dir(at.as_str()) {
            continue;
        }
        match fs::create_dir(at.as_str()) {
            Ok(()) | Err(Error::Exists) => {}
            Err(e) => return Err(e),
        }
    }
    Ok(())
}

fn rmtree(path: &str) -> Result<(), Error> {
    if !fs::exists(path) {
        return Ok(());
    }
    if !fs::is_dir(path) {
        return fs::remove_file(path);
    }
    let mut names = Vec::new();
    for entry in fs::read_dir(path)? {
        let name = entry?.into_file_name();
        if name != "." && name != ".." {
            names.push(name);
        }
    }
    for name in names {
        rmtree(&format!("{}/{}", path, name))?;
    }
    fs::remove_dir(path)
}

// Makes an output path's parent directories, as libxml2's test wrapper does.
fn parent_made(path: &str) -> Result<(), String> {
    match path.rfind('/') {
        Some(0) | None => Ok(()),
        Some(i) => mkdir_p(&path[..i]).map_err(|e| format!("mkdir {}: {}", &path[..i], e)),
    }
}

// Prints the end of an output, starting at a line boundary when one is in reach.
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

fn builtin(s: &Step) -> Result<(), String> {
    let a = &s.argv;
    match s.builtin {
        Builtin::Copy => read_file(&a[0])
            .and_then(|bytes| create(&a[1])?.write_all(&bytes))
            .map_err(|e| format!("copy {} -> {}: {}", a[0], a[1], e)),
        Builtin::Cmp => {
            let x = read_file(&a[0]).map_err(|e| format!("read {}: {}", a[0], e))?;
            let y = read_file(&a[1]).map_err(|e| format!("read {}: {}", a[1], e))?;
            if x == y {
                Ok(())
            } else {
                Err(format!("{} ({} bytes) and {} ({} bytes) differ", a[0], x.len(), a[1], y.len()))
            }
        }
        Builtin::Sha256 => {
            let bytes = read_file(&a[1]).map_err(|e| format!("read {}: {}", a[1], e))?;
            let got: String = Sha256::digest(&bytes).iter().map(|b| format!("{:02x}", b)).collect();
            if got == a[0] {
                Ok(())
            } else {
                Err(format!("{} ({} bytes) has SHA-256 {}, not {}", a[1], bytes.len(), got, a[0]))
            }
        }
        Builtin::Mkdir => mkdir_p(&a[0]).map_err(|e| format!("mkdir {}: {}", a[0], e)),
        Builtin::Rmtree => rmtree(&a[0]).map_err(|e| format!("rmtree {}: {}", a[0], e)),
        Builtin::None => Err(String::from("not a builtin")),
    }
}

fn program(s: &Step, out: &str, err: &str, empty: &str) -> Result<(), String> {
    parent_made(out)?;
    parent_made(err)?;
    let so = create(out).map_err(|e| format!("create {}: {}", out, e))?;
    let se = create(err).map_err(|e| format!("create {}: {}", err, e))?;
    let input = s.stdin.as_deref().unwrap_or(empty);
    let si = File::open(input).map_err(|e| format!("open {}: {}", input, e))?;
    let mut child = Command::new(s.argv[0].as_str())
        .args(s.argv[1..].iter().map(String::as_str))
        .stdin(Stdio::File(si))
        .stdout(Stdio::File(so))
        .stderr(Stdio::File(se))
        .spawn()
        .map_err(|e| format!("spawn {}: {}", s.argv[0], e))?;
    let status = child.wait().map_err(|e| format!("wait: {}", e))?.raw();
    if status != 0 && !s.any_status {
        return Err(format!("exit status {}", status));
    }
    Ok(())
}

// Runs the test's steps in order and reports the first failure; true if none.
fn run(t: &Test, base: &str, empty: &str) -> bool {
    // Before a builtin too: its paths may be relative to the test's directory.
    if let Err(e) = env::set_current_dir(&t.cwd) {
        say(&format!("muster: FAIL {}: chdir {}: {}\n", t.name, t.cwd, e));
        return false;
    }
    let n = t.steps.len();
    for (k, s) in t.steps.iter().enumerate() {
        let (result, outputs) = if s.builtin != Builtin::None {
            (builtin(s), None)
        } else {
            let out = s.stdout.clone().unwrap_or_else(|| format!("{}.{}.out", base, k + 1));
            let err = s.stderr.clone().unwrap_or_else(|| format!("{}.{}.err", base, k + 1));
            (program(s, &out, &err, empty), Some((out, err)))
        };
        if let Err(why) = result {
            let which = if n > 1 { format!("step {} of {}: ", k + 1, n) } else { String::new() };
            say(&format!("muster: FAIL {}: {}{}\n", t.name, which, why));
            if let Some((out, err)) = outputs {
                tail("stdout", &out);
                tail("stderr", &err);
            }
            return false;
        }
    }
    true
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
    if let Err(e) = mkdir_p(&m.logs) {
        say(&format!("muster: logs {}: {}\n", m.logs, e));
        return 2;
    }
    let empty = format!("{}/stdin.empty", m.logs);
    if let Err(e) = create(&empty) {
        say(&format!("muster: {}: {}\n", empty, e));
        return 2;
    }

    let mut failed = 0usize;
    for t in &m.tests {
        if !run(t, &format!("{}/{}", m.logs, log_name(&t.name)), &empty) {
            failed += 1;
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
