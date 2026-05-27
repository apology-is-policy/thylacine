// /alloc-smoke — incremental runtime validation for libthyla-rs's
// typed Rust modules (U-2a onward).
//
// First native Thylacine binary that uses the alloc crate. Declares
// libthyla-rs::alloc::ThylaAlloc as its `#[global_allocator]` and
// then exercises each libthyla-rs uplift module's runtime surface in
// turn. Grows incrementally as U-2X sub-chunks land:
//
//   U-2b alloc:    Box / Vec / String / small-alloc loop
//   U-2c-path:     Path / PathBuf / parent / join / components
//
// Spawned by joey at boot; success prints a single "alloc-smoke: ...
// OK" line per module exercised + exits 0; any failed check prints a
// tagged FAIL message + exits 1.
//
// The failure modes this binary catches:
//   - SYS_BURROW_ATTACH return-value misinterpretation in
//     ensure_initialized.
//   - linked_list_allocator init pointer/size mistakes.
//   - alloc/dealloc protocol mismatches that would corrupt the free
//     list before another binary noticed.
//   - Path/PathBuf method off-by-one bugs that would surface as wrong
//     parent / file_name results once a real shell parses paths.
//
// Not a comprehensive stress test (no concurrency, no fragmentation
// scenarios, no large allocations near INITIAL_HEAP_SIZE). The
// libthyla-rs U-2-test sub-chunk exercises the integrated surface
// across all U-2X modules.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::fs::{Component, Path, PathBuf};
use libthyla_rs::t_putstr;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Box — single heap word + Drop.
    let b: Box<u32> = Box::new(0xDEADBEEF);
    if *b != 0xDEADBEEF {
        t_putstr("alloc-smoke: Box round-trip FAILED\n");
        return 1;
    }
    drop(b);

    // Vec — capacity growth path (forces multiple realloc cycles
    // starting from empty).
    let mut v: Vec<u32> = Vec::new();
    for i in 0..1024u32 {
        v.push(i);
    }
    if v.len() != 1024 || v[0] != 0 || v[1023] != 1023 {
        t_putstr("alloc-smoke: Vec growth FAILED\n");
        return 1;
    }
    // Triangular-number sanity (sum 0..1023 = 1023 * 1024 / 2 = 523776)
    // — catches off-by-one bugs in any future allocator that might
    // duplicate or drop entries.
    let sum: u64 = v.iter().map(|&x| x as u64).sum();
    if sum != 523_776 {
        t_putstr("alloc-smoke: Vec sum check FAILED\n");
        return 1;
    }
    drop(v);

    // String — capacity-doubling path (each push_str may realloc).
    let mut s = String::new();
    for _ in 0..64 {
        s.push_str("hello, world\n");
    }
    if s.len() != 64 * 13 {
        t_putstr("alloc-smoke: String length FAILED\n");
        return 1;
    }
    drop(s);

    // Many small allocs + frees in a tight loop — exercises the
    // free-list reuse path. If the allocator's bookkeeping is broken,
    // this loop typically panics or hangs within a few iterations.
    for _ in 0..256 {
        let small: Box<[u8; 32]> = Box::new([0xAAu8; 32]);
        if small[0] != 0xAA || small[31] != 0xAA {
            t_putstr("alloc-smoke: small Box pattern FAILED\n");
            return 1;
        }
        drop(small);
    }

    t_putstr("alloc-smoke: Box + Vec + String + small-alloc loop OK\n");

    // ====================================================================
    // U-2c-path: t::fs::Path + t::fs::PathBuf
    // ====================================================================

    // Path::new -- zero-cost view of &str as &Path.
    let p = Path::new("/etc/hosts");
    if p.as_str() != "/etc/hosts" || !p.is_absolute() {
        t_putstr("alloc-smoke: Path::new / is_absolute FAILED\n");
        return 1;
    }

    // parent / file_name / file_stem / extension on a typical path.
    if p.parent().map(|q| q.as_str()) != Some("/etc") {
        t_putstr("alloc-smoke: Path::parent FAILED\n");
        return 1;
    }
    if p.file_name() != Some("hosts") {
        t_putstr("alloc-smoke: Path::file_name FAILED\n");
        return 1;
    }

    let archive = Path::new("/tmp/foo.tar.gz");
    if archive.file_stem() != Some("foo.tar") || archive.extension() != Some("gz") {
        t_putstr("alloc-smoke: Path::file_stem / extension FAILED\n");
        return 1;
    }

    // Root and empty edge cases.
    if Path::new("/").parent().is_some() || Path::new("/").file_name().is_some() {
        t_putstr("alloc-smoke: Path root edge FAILED\n");
        return 1;
    }
    if Path::new("").parent().is_some() || Path::new("").file_name().is_some() {
        t_putstr("alloc-smoke: Path empty edge FAILED\n");
        return 1;
    }

    // Dotfile -- no extension (leading dot is part of the stem).
    let dotfile = Path::new(".bashrc");
    if dotfile.file_stem() != Some(".bashrc") || dotfile.extension().is_some() {
        t_putstr("alloc-smoke: Path dotfile stem/extension FAILED\n");
        return 1;
    }

    // PathBuf push (relative) inserts separator; absolute push replaces.
    let mut pb = PathBuf::from("/etc");
    pb.push("hosts");
    if pb.as_str() != "/etc/hosts" {
        t_putstr("alloc-smoke: PathBuf::push relative FAILED\n");
        return 1;
    }
    pb.push("/other");
    if pb.as_str() != "/other" {
        t_putstr("alloc-smoke: PathBuf::push absolute-replace FAILED\n");
        return 1;
    }

    // PathBuf pop walks up one component at a time.
    let mut chain = PathBuf::from("/a/b/c");
    if !chain.pop() || chain.as_str() != "/a/b" {
        t_putstr("alloc-smoke: PathBuf::pop step1 FAILED\n");
        return 1;
    }
    if !chain.pop() || chain.as_str() != "/a" {
        t_putstr("alloc-smoke: PathBuf::pop step2 FAILED\n");
        return 1;
    }
    if !chain.pop() || chain.as_str() != "/" {
        t_putstr("alloc-smoke: PathBuf::pop step3 (to root) FAILED\n");
        return 1;
    }
    if chain.pop() {
        t_putstr("alloc-smoke: PathBuf::pop past root unexpectedly succeeded\n");
        return 1;
    }

    // set_extension replaces, removes (when empty), or adds.
    let mut e = PathBuf::from("/x/y.txt");
    if !e.set_extension("md") || e.as_str() != "/x/y.md" {
        t_putstr("alloc-smoke: PathBuf::set_extension replace FAILED\n");
        return 1;
    }
    if !e.set_extension("") || e.as_str() != "/x/y" {
        t_putstr("alloc-smoke: PathBuf::set_extension remove FAILED\n");
        return 1;
    }
    if !e.set_extension("rs") || e.as_str() != "/x/y.rs" {
        t_putstr("alloc-smoke: PathBuf::set_extension add FAILED\n");
        return 1;
    }

    // Components walks RootDir + Normal segments; skips empty segments
    // from consecutive/trailing separators.
    let weird = Path::new("///foo//bar/");
    let mut it = weird.components();
    let want = [
        Component::RootDir,
        Component::Normal("foo"),
        Component::Normal("bar"),
    ];
    for (i, expected) in want.iter().enumerate() {
        let got = match it.next() {
            Some(c) => c,
            None => {
                t_putstr("alloc-smoke: Components ran short FAILED\n");
                return 1;
            }
        };
        if got != *expected {
            t_putstr("alloc-smoke: Components mismatch FAILED\n");
            let _ = i; // suppress unused if message is the same
            return 1;
        }
    }
    if it.next().is_some() {
        t_putstr("alloc-smoke: Components emitted spurious tail FAILED\n");
        return 1;
    }

    // starts_with / ends_with are component-aligned, not byte-aligned.
    let s = Path::new("/usr/bin/ut");
    if !s.starts_with("/usr") || !s.starts_with("/usr/bin") {
        t_putstr("alloc-smoke: Path::starts_with FAILED\n");
        return 1;
    }
    if s.starts_with("/usr/bi") {
        // "/usr/bi" is a byte-prefix but not a component-prefix; reject.
        t_putstr("alloc-smoke: Path::starts_with byte-prefix FAILED\n");
        return 1;
    }
    if !s.ends_with("ut") || !s.ends_with("bin/ut") {
        t_putstr("alloc-smoke: Path::ends_with FAILED\n");
        return 1;
    }

    t_putstr("alloc-smoke: Path + PathBuf + Components OK\n");
    0
}
