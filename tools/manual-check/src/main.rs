//! manual-check -- the Operator's Manual checker for the build host
//! (MANUAL-DESIGN.md section 6).
//!
//!     manual-check <dir>
//!
//! Checks every file in `<dir>` (docs/manual) with the reader's checker. Every
//! file other than `.gitkeep` must be a regular file named `NN-<name>.md`, be
//! valid UTF-8, and pass the check. On success, prints each section's file name
//! in book order, one per line, to standard output: the set the bake installs.
//! Every problem goes to standard error. Exits 1 when any file fails, 2 on a
//! usage error.

use std::process::ExitCode;

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let [dir] = args.as_slice() else {
        eprintln!("usage: manual-check <dir>");
        return ExitCode::from(2);
    };
    let entries = match std::fs::read_dir(dir) {
        Ok(e) => e,
        Err(e) => {
            eprintln!("manual-check: {}: {}", dir, e);
            return ExitCode::from(1);
        }
    };
    let mut names = Vec::new();
    for ent in entries {
        match ent {
            Ok(e) => names.push(e.file_name()),
            Err(e) => {
                eprintln!("manual-check: {}: {}", dir, e);
                return ExitCode::from(1);
            }
        }
    }
    names.sort();
    let mut failed = false;
    let mut sections = Vec::new();
    for name in &names {
        let Some(name) = name.to_str() else {
            eprintln!("manual-check: {}: a file name that is not UTF-8", dir);
            failed = true;
            continue;
        };
        if name == ".gitkeep" {
            continue;
        }
        let path = format!("{}/{}", dir, name);
        if manual::catalog::parse_file_name(name).is_none() {
            eprintln!(
                "manual-check: {}: not named NN-<name>.md; a draft belongs in docs/manual-drafts",
                path
            );
            failed = true;
            continue;
        }
        match std::fs::symlink_metadata(&path) {
            Ok(m) if m.file_type().is_file() => {}
            Ok(_) => {
                eprintln!("manual-check: {}: not a regular file", path);
                failed = true;
                continue;
            }
            Err(e) => {
                eprintln!("manual-check: {}: {}", path, e);
                failed = true;
                continue;
            }
        }
        let src = match std::fs::read(&path).map(String::from_utf8) {
            Ok(Ok(s)) => s,
            Ok(Err(_)) => {
                eprintln!("manual-check: {}: not valid UTF-8", path);
                failed = true;
                continue;
            }
            Err(e) => {
                eprintln!("manual-check: {}: {}", path, e);
                failed = true;
                continue;
            }
        };
        let problems = manual::format::check(Some(name), &src, &mut |line, p| {
            eprintln!("manual-check: {}:{}: {}", path, line, p);
        });
        if problems > 0 {
            failed = true;
        } else {
            sections.push(name);
        }
    }
    if failed {
        return ExitCode::from(1);
    }
    for e in manual::catalog::entries(sections.iter().copied()) {
        println!("{}", e.file);
    }
    ExitCode::SUCCESS
}
