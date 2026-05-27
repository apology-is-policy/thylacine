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
//   U-2c-io:       Cursor (in-mem Read/Seek), File::open + Read over
//                  /system.key in devramfs (validates SYS_WALK_OPEN
//                  + SYS_READ + SYS_LSEEK round-trip via t::io traits)
//   U-2c-fs:       File::metadata, free fs::{metadata, exists, is_file,
//                  is_dir}, OpenOptions builder (validates SYS_FSTAT
//                  + open-mode composition)
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
use libthyla_rs::err::Error;
use libthyla_rs::fs::{self, Component, File, OpenOptions, Path, PathBuf};
use libthyla_rs::io::{Cursor, Read, Seek, SeekFrom};
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

    // ====================================================================
    // U-2c-io: t::io::{Read, Seek, Cursor} + t::fs::File
    // ====================================================================

    // Cursor over a static byte slice -- exercises the in-memory Read +
    // Seek paths without involving any syscall.
    let data: &[u8] = b"the thylacine yips at dusk";
    let mut cursor = Cursor::new(data);

    // Read first 3 bytes.
    let mut head = [0u8; 3];
    match cursor.read(&mut head) {
        Ok(3) => {}
        _ => {
            t_putstr("alloc-smoke: Cursor::read first 3 FAILED\n");
            return 1;
        }
    }
    if &head != b"the" {
        t_putstr("alloc-smoke: Cursor::read content FAILED\n");
        return 1;
    }

    // Seek to start of "yips at dusk" (12 bytes from the end -> the 'y').
    let pos = match cursor.seek(SeekFrom::End(-12)) {
        Ok(p) => p,
        Err(_) => {
            t_putstr("alloc-smoke: Cursor::seek End(-12) FAILED\n");
            return 1;
        }
    };
    if pos != (data.len() - 12) as u64 {
        t_putstr("alloc-smoke: Cursor::seek End(-12) position FAILED\n");
        return 1;
    }

    // Read the 12-byte tail.
    let mut tail = [0u8; 12];
    if let Err(_) = cursor.read(&mut tail) {
        t_putstr("alloc-smoke: Cursor::read tail FAILED\n");
        return 1;
    }
    if &tail != b"yips at dusk" {
        t_putstr("alloc-smoke: Cursor::read tail content FAILED\n");
        return 1;
    }

    // stream_position via SeekFrom::Current(0). After reading the
    // 12-byte tail from data.len() - 12, we should be at data.len().
    let cur = match cursor.stream_position() {
        Ok(p) => p,
        Err(_) => {
            t_putstr("alloc-smoke: Cursor::stream_position FAILED\n");
            return 1;
        }
    };
    if cur != data.len() as u64 {
        t_putstr("alloc-smoke: Cursor::stream_position value FAILED\n");
        return 1;
    }

    // EOF: rewind, exhaust the cursor, expect Ok(0) at end.
    cursor.set_position(0);
    let mut sink = [0u8; 64];
    let total = match cursor.read(&mut sink) {
        Ok(n) => n,
        Err(_) => {
            t_putstr("alloc-smoke: Cursor::read full FAILED\n");
            return 1;
        }
    };
    if total != data.len() {
        t_putstr("alloc-smoke: Cursor::read total length FAILED\n");
        return 1;
    }
    match cursor.read(&mut sink) {
        Ok(0) => {}
        _ => {
            t_putstr("alloc-smoke: Cursor::read post-EOF FAILED\n");
            return 1;
        }
    }

    // File::open of /system.key (devramfs, 3656 bytes, read-only).
    // Validates: multi-step walk-or-single-step (this is single
    // component "system.key"), SYS_WALK_OPEN return decoding, Read
    // over SYS_READ, Seek over SYS_LSEEK, Drop via SYS_CLOSE.
    let mut sk = match File::open("/system.key") {
        Ok(f) => f,
        Err(_) => {
            t_putstr("alloc-smoke: File::open(/system.key) FAILED\n");
            return 1;
        }
    };

    // Read first 16 bytes -- libsodium key material; we only check
    // we got 16 distinct bytes back (not "is it valid AEGIS key").
    let mut sk_head = [0u8; 16];
    match sk.read(&mut sk_head) {
        Ok(16) => {}
        Ok(n) => {
            let _ = n;
            t_putstr("alloc-smoke: File::read short read FAILED\n");
            return 1;
        }
        Err(_) => {
            t_putstr("alloc-smoke: File::read FAILED\n");
            return 1;
        }
    }

    // SeekFrom::End(0) -> total length = 3656 (the kernel-tested size).
    let total_len = match sk.seek(SeekFrom::End(0)) {
        Ok(p) => p,
        Err(_) => {
            t_putstr("alloc-smoke: File::seek(End(0)) FAILED\n");
            return 1;
        }
    };
    if total_len != 3656 {
        t_putstr("alloc-smoke: File::seek size mismatch FAILED\n");
        return 1;
    }

    // SeekFrom::Start(0) -> 0, then verify by re-reading first 16
    // bytes match what we saw above.
    if let Err(_) = sk.seek(SeekFrom::Start(0)) {
        t_putstr("alloc-smoke: File::seek(Start(0)) FAILED\n");
        return 1;
    }
    let mut sk_head2 = [0u8; 16];
    if let Err(_) = sk.read(&mut sk_head2) {
        t_putstr("alloc-smoke: File::read after rewind FAILED\n");
        return 1;
    }
    if sk_head != sk_head2 {
        t_putstr("alloc-smoke: File::read after rewind content mismatch FAILED\n");
        return 1;
    }

    // sk drops here -- handle closes via SYS_CLOSE in Handle::drop.

    // File::open on a definitely-missing path -> NotFound (or whatever
    // the devramfs reports).
    if File::open("/no-such-file-thylacine").is_ok() {
        t_putstr("alloc-smoke: File::open(missing) unexpectedly succeeded\n");
        return 1;
    }

    // File::open of a relative path -> InvalidArgument (v1 only
    // supports absolute paths).
    if File::open("relative").is_ok() {
        t_putstr("alloc-smoke: File::open(relative) unexpectedly succeeded\n");
        return 1;
    }

    t_putstr("alloc-smoke: Cursor + File + Read + Seek OK\n");

    // ====================================================================
    // U-2c-fs: Metadata, free functions, OpenOptions
    // ====================================================================

    // File::metadata on an open File.
    let sk = match File::open("/system.key") {
        Ok(f) => f,
        Err(_) => {
            t_putstr("alloc-smoke: File::open(/system.key) for metadata FAILED\n");
            return 1;
        }
    };
    let md = match sk.metadata() {
        Ok(m) => m,
        Err(_) => {
            t_putstr("alloc-smoke: File::metadata FAILED\n");
            return 1;
        }
    };
    if md.len() != 3656 {
        t_putstr("alloc-smoke: Metadata::len mismatch FAILED\n");
        return 1;
    }
    if !md.is_file() || md.is_dir() {
        t_putstr("alloc-smoke: Metadata::is_file/is_dir FAILED\n");
        return 1;
    }
    // mode should be 0o100644 per the joey boot probe. permissions()
    // masks off the type bits, leaving 0o644.
    if md.permissions() != 0o644 {
        t_putstr("alloc-smoke: Metadata::permissions FAILED\n");
        return 1;
    }
    drop(sk);

    // Free-function metadata: same checks, single line.
    let md2 = match fs::metadata("/system.key") {
        Ok(m) => m,
        Err(_) => {
            t_putstr("alloc-smoke: fs::metadata FAILED\n");
            return 1;
        }
    };
    if md2.len() != 3656 || !md2.is_file() {
        t_putstr("alloc-smoke: fs::metadata fields FAILED\n");
        return 1;
    }

    // exists / is_file / is_dir.
    if !fs::exists("/system.key") {
        t_putstr("alloc-smoke: fs::exists(/system.key) FAILED\n");
        return 1;
    }
    if fs::exists("/no-such-file-thylacine") {
        t_putstr("alloc-smoke: fs::exists(missing) unexpectedly true\n");
        return 1;
    }
    if !fs::is_file("/system.key") {
        t_putstr("alloc-smoke: fs::is_file(/system.key) FAILED\n");
        return 1;
    }
    if fs::is_dir("/system.key") {
        t_putstr("alloc-smoke: fs::is_dir(/system.key) unexpectedly true\n");
        return 1;
    }

    // OpenOptions: explicit-read open.
    let mut sk3 = match OpenOptions::new().read(true).open("/system.key") {
        Ok(f) => f,
        Err(_) => {
            t_putstr("alloc-smoke: OpenOptions::new().read(true).open FAILED\n");
            return 1;
        }
    };
    let mut probe = [0u8; 8];
    if sk3.read(&mut probe).is_err() {
        t_putstr("alloc-smoke: OpenOptions-opened File read FAILED\n");
        return 1;
    }
    drop(sk3);

    // OpenOptions: no read or write -> InvalidArgument.
    match OpenOptions::new().open("/system.key") {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("alloc-smoke: OpenOptions::new()-no-mode unexpectedly succeeded\n");
            return 1;
        }
    }

    // OpenOptions: truncate without write -> InvalidArgument.
    match OpenOptions::new().read(true).truncate(true).open("/system.key") {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("alloc-smoke: OpenOptions::truncate-without-write unexpectedly succeeded\n");
            return 1;
        }
    }

    // OpenOptions: create requested -> NotImplemented (v1 lacks kernel surface).
    match OpenOptions::new().write(true).create(true).open("/no-such") {
        Err(Error::NotImplemented) => {}
        _ => {
            t_putstr("alloc-smoke: OpenOptions::create did not return NotImplemented\n");
            return 1;
        }
    }

    // OpenOptions: append requested -> NotImplemented.
    match OpenOptions::new().write(true).append(true).open("/system.key") {
        Err(Error::NotImplemented) => {}
        _ => {
            t_putstr("alloc-smoke: OpenOptions::append did not return NotImplemented\n");
            return 1;
        }
    }

    t_putstr("alloc-smoke: Metadata + OpenOptions + free fns OK\n");
    0
}
