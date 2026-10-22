// env [-i] [NAME=VALUE]... [COMMAND [ARG]...] -- print or modify the environment.
//
// The environment is the per-Proc /env device (G15): each variable is the file
// /env/NAME, a spawned child receives a copy of its parent's, and the copy
// survives exec. So env changes ITS OWN /env and then execs COMMAND in place,
// as POSIX env does: the command keeps env's pid and descriptors, its exit
// status is env's, and the caller's environment is untouched. A redirect such
// as `echo 1 > /env/NAME` from ut cannot do this: ut opens the file in its own
// environment and echo writes into echo's copy (OPEN-BUGS, bind-at-open).
//
// With no COMMAND, env prints the environment a program it started would
// receive: the kernel's NAME=VALUE rendering of /env (/proc/<pid>/environ, the
// projection exec puts in a new image's envp), one variable per line.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use coreutils::envcmd::{self, ParseError};
use libthyla_rs::env::{self, Args};
use libthyla_rs::io::{self, Read, Write};
use libthyla_rs::{eprintln, fs};

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    run(env::args())
}

const USAGE: &str = "\
usage: env [-i] [NAME=VALUE]... [COMMAND [ARG]...]
  Set each NAME to VALUE and run COMMAND with the result, or print the
  environment when no COMMAND is given. A bare COMMAND is searched on $PATH
  (on /bin when PATH is unset). The caller's environment is not changed.
  -i      start from an empty environment
  --help  show this help
Exit status: COMMAND's own; 125 if env itself failed; 126 if COMMAND was
found but could not be run; 127 if it was not found.
";

fn run(args: Args) -> i64 {
    let ops: Vec<&[u8]> = args.operands().collect();
    let inv = match envcmd::parse(&ops) {
        Ok(inv) => inv,
        Err(ParseError::Help) => {
            io::out(USAGE.as_bytes());
            return 0;
        }
        Err(ParseError::BadOption(o)) => {
            eprintln!("env: invalid option '{}'", String::from_utf8_lossy(o));
            coreutils::usage::hint("env");
            return 125;
        }
        Err(ParseError::BadName(o)) => {
            eprintln!("env: cannot set '{}': invalid variable name", String::from_utf8_lossy(o));
            return 125;
        }
    };
    if inv.clear {
        if let Err(rc) = clear() {
            return rc;
        }
    }
    for &(name, value) in &inv.assigns {
        if let Err(rc) = set(name, value) {
            return rc;
        }
    }
    if inv.command.is_empty() {
        return print_environ();
    }
    exec(inv.command)
}

fn env_path(name: &str) -> String {
    let mut p = String::from("/env/");
    p.push_str(name);
    p
}

fn clear() -> Result<(), i64> {
    let dir = fs::read_dir("/env").map_err(|e| {
        eprintln!("env: cannot read /env: {}", e);
        125
    })?;
    let mut names: Vec<String> = Vec::new();
    for ent in dir {
        match ent {
            Ok(ent) => names.push(String::from(ent.file_name())),
            Err(e) => {
                eprintln!("env: cannot read /env: {}", e);
                return Err(125);
            }
        }
    }
    for n in names {
        if let Err(e) = fs::remove_file(env_path(&n)) {
            eprintln!("env: cannot unset '{}': {}", n, e);
            return Err(125);
        }
    }
    Ok(())
}

// A name is a path component under /env, so it must be UTF-8; a value is any bytes.
fn set(name: &[u8], value: &[u8]) -> Result<(), i64> {
    let Ok(n) = core::str::from_utf8(name) else {
        eprintln!("env: cannot set '{}': invalid variable name", String::from_utf8_lossy(name));
        return Err(125);
    };
    let r = fs::File::create(env_path(n)).and_then(|mut f| f.write_all(value));
    r.map_err(|e| {
        eprintln!("env: cannot set '{}': {}", n, e);
        125
    })
}

fn print_environ() -> i64 {
    let mut path = String::from("/proc/");
    path.push_str(&alloc::format!("{}", libthyla_rs::identity::pid()));
    path.push_str("/environ");
    let mut buf: Vec<u8> = Vec::new();
    let r = fs::File::open(&path).and_then(|mut f| {
        let mut chunk = [0u8; 4096];
        loop {
            let n = f.read(&mut chunk)?;
            if n == 0 {
                return Ok(());
            }
            buf.extend_from_slice(&chunk[..n]);
        }
    });
    if let Err(e) = r {
        eprintln!("env: cannot read {}: {}", path, e);
        return 125;
    }
    for rec in buf.split(|&c| c == 0).filter(|r| !r.is_empty()) {
        io::out(rec);
        io::out(b"\n");
    }
    0
}

// The first existing PATH entry holding the name, else the name under /bin so
// the exec reports the miss.
fn resolve(name: &[u8]) -> Vec<u8> {
    if name.contains(&b'/') {
        return Vec::from(name);
    }
    let path = env::var("PATH").unwrap_or_else(|| String::from("/bin"));
    for dir in path.split(':').filter(|d| !d.is_empty()) {
        let mut cand = Vec::from(dir.as_bytes());
        cand.push(b'/');
        cand.extend_from_slice(name);
        if let Ok(s) = core::str::from_utf8(&cand) {
            if fs::exists(s) {
                return cand;
            }
        }
    }
    let mut cand = Vec::from(&b"/bin/"[..]);
    cand.extend_from_slice(name);
    cand
}

fn exec(command: &[&[u8]]) -> i64 {
    let prog = resolve(command[0]);
    let mut argv: Vec<u8> = Vec::new();
    for a in command {
        argv.extend_from_slice(a);
        argv.push(0);
    }
    // Returns only on failure; on success the command runs in this Proc.
    let rc = unsafe { libthyla_rs::t_execve(&prog, &argv, command.len() as u64) };
    let err = libthyla_rs::err::Error::from(-(rc as i32));
    eprintln!("env: '{}': {}", String::from_utf8_lossy(command[0]), err);
    if err == libthyla_rs::err::Error::NotFound {
        127
    } else {
        126
    }
}
