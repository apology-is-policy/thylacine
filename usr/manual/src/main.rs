// manual -- the Operator's Manual reader (docs/MANUAL-DESIGN.md section 5).
// Lists the sections installed in /manual, shows one by name or a Markdown file
// by path, or checks files against the section format. The format, the
// rendering and the lookup live in the library; this body supplies files, the
// Beacon tier, and the console width.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec::Vec;

// A section is at most 1 MiB; its parsed form and rendering are a small
// multiple of that.
#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAllocN<{ 16 * 1024 * 1024 }> =
    libthyla_rs::alloc::ThylaAllocN;

use beacon::{BeaconMode, Tier};
use libthyla_rs::eprintln;
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, File};
use libthyla_rs::{env, io};

use manual::catalog::{self, Lookup};
use manual::render::{self, Listed};
use manual::{format as section, SECTION_MAX};

const MANUAL_DIR: &str = "/manual";
const WRITE_CHUNK: usize = 64 * 1024;
const USAGE: &str =
    "usage: manual [--beacon=auto|always|never] [name | file]\n       manual --check file...\n";

enum Mode {
    Contents,
    Show(String),
    Check(Vec<String>),
}

/// The parsed command line, or the exit status to return at once.
fn parse_args() -> Result<(Mode, BeaconMode), i64> {
    let mut flag = BeaconMode::Auto;
    let mut check = false;
    let mut operands: Vec<String> = Vec::new();
    let mut options_done = false;
    for raw in env::args().operands() {
        let Ok(arg) = core::str::from_utf8(raw) else {
            eprintln!("manual: an argument is not valid UTF-8");
            return Err(2);
        };
        if !options_done && arg.starts_with('-') && arg != "-" {
            if arg == "--" {
                options_done = true;
            } else if arg == "--check" {
                check = true;
            } else if arg == "-h" || arg == "--help" {
                io::out(USAGE.as_bytes());
                return Err(0);
            } else if let Some(when) = arg.strip_prefix("--beacon=") {
                match BeaconMode::parse_when(when) {
                    Some(m) => flag = m,
                    None => {
                        eprintln!("manual: --beacon takes auto, always, or never");
                        io::err(USAGE.as_bytes());
                        return Err(2);
                    }
                }
            } else {
                eprintln!("manual: unknown option '{}'", arg);
                io::err(USAGE.as_bytes());
                return Err(2);
            }
            continue;
        }
        operands.push(String::from(arg));
    }
    let mode = if check {
        if operands.is_empty() {
            io::err(USAGE.as_bytes());
            return Err(2);
        }
        Mode::Check(operands)
    } else {
        match operands.len() {
            0 => Mode::Contents,
            1 => Mode::Show(operands.remove(0)),
            _ => {
                io::err(USAGE.as_bytes());
                return Err(2);
            }
        }
    };
    Ok((mode, flag))
}

/// The effective tier, resolved as the coreutils resolve it (4.1).
fn resolve_tier(flag: BeaconMode) -> Tier {
    let env_tier = env::var("BEACON")
        .and_then(|v| Tier::parse(&v))
        .unwrap_or(Tier::None);
    beacon::effective_tier(env_tier, libthyla_rs::fd_devclass(1), flag)
}

/// The wrap width (4.3): only at a plain tier, only on the console, and only
/// when `/dev/winsize` reports one.
fn plain_width(tier: Tier) -> Option<usize> {
    if tier == Tier::Rich || libthyla_rs::fd_devclass(1) != Some(beacon::DC_CONSOLE) {
        return None;
    }
    let mut f = File::open("/dev/winsize").ok()?;
    let bytes = io::slurp_capped(&mut f, 256).ok()?;
    manual::console_width(&bytes)
}

/// Read a section file, or describe why it cannot be read.
fn read_section(path: &str) -> Result<String, String> {
    let mut f = File::open(path).map_err(|e| format!("{}: {}", path, e))?;
    let bytes = io::slurp_capped(&mut f, SECTION_MAX).map_err(|e| match e {
        Error::NoMemory => format!("{}: larger than 1 MiB", path),
        e => format!("{}: {}", path, e),
    })?;
    String::from_utf8(bytes).map_err(|_| format!("{}: not valid UTF-8", path))
}

/// The file names in `/manual`; an absent directory has none.
fn manual_files() -> Result<Vec<String>, String> {
    let dir = match fs::read_dir(MANUAL_DIR) {
        Ok(d) => d,
        Err(Error::NotFound) => return Ok(Vec::new()),
        Err(e) => return Err(format!("{}: {}", MANUAL_DIR, e)),
    };
    let mut names = Vec::new();
    for ent in dir {
        match ent {
            Ok(e) => names.push(String::from(e.file_name())),
            Err(e) => return Err(format!("{}: {}", MANUAL_DIR, e)),
        }
    }
    Ok(names)
}

fn emit(bytes: &[u8]) -> i64 {
    let mut out = io::OutSink::new();
    for chunk in bytes.chunks(WRITE_CHUNK) {
        out.put(chunk);
    }
    if out.failed() {
        eprintln!("manual: write error");
        return 1;
    }
    0
}

fn report(path: &str, diags: &[section::Diagnostic]) {
    for d in diags {
        eprintln!("manual: {}:{}: {}", path, d.line, d.message);
    }
}

fn file_name(path: &str) -> Option<&str> {
    path.rsplit('/').next()
}

fn contents(tier: Tier) -> i64 {
    let files = match manual_files() {
        Ok(f) => f,
        Err(m) => {
            eprintln!("manual: {}", m);
            return 1;
        }
    };
    let entries = catalog::entries(files.iter().map(|f| f.as_str()));
    let listed: Vec<Listed> = entries
        .iter()
        .map(|e| {
            let path = format!("{}/{}", MANUAL_DIR, e.file);
            let title = read_section(&path)
                .ok()
                .and_then(|src| section::title_text(&src))
                .unwrap_or_else(|| String::from("?"));
            Listed {
                name: e.name.clone(),
                title,
            }
        })
        .collect();
    emit(&render::render_contents(&listed, tier))
}

fn show(operand: &str, tier: Tier) -> i64 {
    let path = if catalog::is_path_operand(operand) {
        String::from(operand)
    } else {
        let files = match manual_files() {
            Ok(f) => f,
            Err(m) => {
                eprintln!("manual: {}", m);
                return 1;
            }
        };
        let entries = catalog::entries(files.iter().map(|f| f.as_str()));
        match catalog::lookup(&entries, operand) {
            Lookup::Found(e) => format!("{}/{}", MANUAL_DIR, e.file),
            Lookup::Missing => {
                eprintln!("manual: no section named '{}'", operand);
                return 1;
            }
            Lookup::Ambiguous(v) => {
                let names: Vec<&str> = v.iter().map(|e| e.name.as_str()).collect();
                eprintln!(
                    "manual: '{}' matches several sections: {}",
                    operand,
                    names.join(", ")
                );
                return 1;
            }
        }
    };
    let src = match read_section(&path) {
        Ok(s) => s,
        Err(m) => {
            eprintln!("manual: {}", m);
            return 1;
        }
    };
    match section::check_section(file_name(&path), &src) {
        Ok(doc) => emit(&render::render(&doc, tier, plain_width(tier))),
        Err(diags) => {
            report(&path, &diags);
            1
        }
    }
}

fn check(paths: &[String]) -> i64 {
    let mut status = 0;
    for path in paths {
        match read_section(path) {
            Ok(src) => {
                if let Err(diags) = section::check_section(file_name(path), &src) {
                    report(path, &diags);
                    status = 1;
                }
            }
            Err(m) => {
                eprintln!("manual: {}", m);
                status = 1;
            }
        }
    }
    status
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let (mode, flag) = match parse_args() {
        Ok(p) => p,
        Err(status) => return status,
    };
    match mode {
        Mode::Contents => contents(resolve_tier(flag)),
        Mode::Show(operand) => show(&operand, resolve_tier(flag)),
        Mode::Check(paths) => check(&paths),
    }
}
