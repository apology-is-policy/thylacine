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
//   U-2d:          process::pipe round-trip + EOF; Command::spawn
//                  hello-rs with piped stdio + wait (validates
//                  SYS_PIPE + SYS_SPAWN_FULL_ARGV + SYS_WAIT_PID)
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
use libthyla_rs::io::{Cursor, Read, Seek, SeekFrom, Write};
use libthyla_rs::notes::{self, NoteClass, NoteMask, NoteTarget, Notes};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};
use libthyla_rs::process::{self, Command, Stdio};
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

    // ====================================================================
    // U-2d: process::pipe + Command + Child + ExitStatus
    // ====================================================================

    // pipe() round-trip: write to writer, read from reader.
    let (mut rd, mut wr) = match process::pipe() {
        Ok(p) => p,
        Err(_) => {
            t_putstr("alloc-smoke: process::pipe() FAILED\n");
            return 1;
        }
    };
    let payload = b"thylacine";
    let n = match wr.write(payload) {
        Ok(n) => n,
        Err(_) => {
            t_putstr("alloc-smoke: pipe write FAILED\n");
            return 1;
        }
    };
    if n != payload.len() {
        t_putstr("alloc-smoke: pipe write short FAILED\n");
        return 1;
    }
    let mut got = [0u8; 16];
    let r = match rd.read(&mut got) {
        Ok(r) => r,
        Err(_) => {
            t_putstr("alloc-smoke: pipe read FAILED\n");
            return 1;
        }
    };
    if r != payload.len() || &got[..r] != payload {
        t_putstr("alloc-smoke: pipe round-trip content FAILED\n");
        return 1;
    }

    // pipe() EOF: drop writer; reader should see Ok(0).
    drop(wr);
    let eof = match rd.read(&mut got) {
        Ok(n) => n,
        Err(_) => {
            t_putstr("alloc-smoke: pipe read-after-drop FAILED\n");
            return 1;
        }
    };
    if eof != 0 {
        t_putstr("alloc-smoke: pipe EOF on drop FAILED\n");
        return 1;
    }
    drop(rd);

    // Command::spawn(hello-rs) with piped stdio. We must use Piped (not
    // Inherit) because alloc-smoke was spawned by joey via the no-fds
    // SYS_SPAWN path -- our fd table has no slots 0/1/2 to inherit
    // FROM. Piped creates fresh pipes whose fds we control.
    //
    // hello-rs prints via t_putstr -> SYS_PUTS -> UART, NOT via stdio
    // fds, so the piped stdout/stderr stays empty; the test confirms
    // only that spawn + wait + exit-status round-trip works.
    let mut child = match Command::new("hello-rs")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            t_putstr("alloc-smoke: Command::spawn(hello-rs) FAILED\n");
            return 1;
        }
    };
    if child.pid() <= 0 {
        t_putstr("alloc-smoke: Child::pid out of range FAILED\n");
        return 1;
    }
    // Close the parent's stdin write-end so hello-rs's notional stdin
    // sees EOF immediately (hello-rs doesn't read, but discipline).
    drop(child.stdin.take());
    drop(child.stdout.take());
    drop(child.stderr.take());

    let status = match child.wait() {
        Ok(s) => s,
        Err(_) => {
            t_putstr("alloc-smoke: Child::wait FAILED\n");
            return 1;
        }
    };
    if !status.success() {
        t_putstr("alloc-smoke: hello-rs exit non-zero FAILED\n");
        return 1;
    }

    t_putstr("alloc-smoke: pipe + Command + Child OK\n");

    // ====================================================================
    // U-2e: t::notes + t::poll
    // ====================================================================

    // Open the per-Proc notes fd. The kernel-side queue exists from
    // proc_alloc; multiple opens against the same Proc share one queue
    // (N-5 invariant).
    let n = match Notes::open_self() {
        Ok(n) => n,
        Err(_) => {
            t_putstr("alloc-smoke: Notes::open_self FAILED\n");
            return 1;
        }
    };

    // Drain any kernel-synthetic notes that piled up during prior
    // sub-chunks. The U-2d Command::spawn(hello-rs).wait() reaps a
    // child Proc; kernel `exits()` posts a `child_exit` to our queue
    // via notes_post_child_exit. We expect at least one drained
    // child_exit; we tolerate zero (alloc-smoke can be re-run).
    let mut saw_child_exit = false;
    let mut drained = 0u32;
    while let Some(note) = match n.try_read() {
        Ok(o) => o,
        Err(_) => {
            t_putstr("alloc-smoke: Notes::try_read drain FAILED\n");
            return 1;
        }
    } {
        if note.name == "child_exit" {
            saw_child_exit = true;
            // sender_pid is 0 for kernel-synthetic posters.
            if note.from.is_some() {
                t_putstr("alloc-smoke: child_exit from sender != None FAILED\n");
                return 1;
            }
        }
        drained += 1;
        if drained > 16 {
            t_putstr("alloc-smoke: notes drain overran NOTE_QUEUE_DEPTH FAILED\n");
            return 1;
        }
    }
    if !saw_child_exit {
        t_putstr("alloc-smoke: WARN no child_exit observed; continuing\n");
    }

    // Mask validation — set then restore. set_mask returns the prior
    // mask; setting NoteMask::just(Interrupt), then NONE, must
    // round-trip cleanly. Default mask is NONE.
    let prior = match notes::set_mask(NoteMask::just(NoteClass::Interrupt)) {
        Ok(m) => m,
        Err(_) => {
            t_putstr("alloc-smoke: notes::set_mask(interrupt) FAILED\n");
            return 1;
        }
    };
    if !prior.is_empty() {
        t_putstr("alloc-smoke: notes mask prior not NONE FAILED\n");
        return 1;
    }
    let after = match notes::set_mask(NoteMask::NONE) {
        Ok(m) => m,
        Err(_) => {
            t_putstr("alloc-smoke: notes::set_mask(NONE) FAILED\n");
            return 1;
        }
    };
    if !after.contains(NoteClass::Interrupt) {
        t_putstr("alloc-smoke: notes mask round-trip readback FAILED\n");
        return 1;
    }

    // with_mask RAII: enter scope with interrupt deferred, then drop
    // the guard — mask must return to NONE.
    {
        let _guard = match notes::with_mask(NoteMask::just(NoteClass::Interrupt)) {
            Ok(g) => g,
            Err(_) => {
                t_putstr("alloc-smoke: notes::with_mask FAILED\n");
                return 1;
            }
        };
        // Guard drops here.
    }
    // Drop already ran the restore. Confirm mask is back to NONE by
    // setting it to NONE and observing the prior (which should be the
    // value with_mask restored to — NONE — confirming restore ran).
    let post_guard = match notes::set_mask(NoteMask::NONE) {
        Ok(m) => m,
        Err(_) => {
            t_putstr("alloc-smoke: notes::set_mask post-guard FAILED\n");
            return 1;
        }
    };
    if !post_guard.is_empty() {
        t_putstr("alloc-smoke: with_mask did not restore prior mask FAILED\n");
        return 1;
    }

    // Send validation: empty name -> InvalidArgument.
    match notes::send(NoteTarget::SelfProc, "") {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("alloc-smoke: notes::send empty name unexpectedly accepted\n");
            return 1;
        }
    }
    // Send validation: too long (>= 16 bytes) -> InvalidArgument.
    match notes::send(NoteTarget::SelfProc, "0123456789abcdef") {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("alloc-smoke: notes::send oversize unexpectedly accepted\n");
            return 1;
        }
    }
    // Send validation: snare: prefix reserved -> InvalidArgument.
    match notes::send(NoteTarget::SelfProc, "snare:segv") {
        Err(Error::InvalidArgument) => {}
        _ => {
            t_putstr("alloc-smoke: notes::send snare: prefix unexpectedly accepted\n");
            return 1;
        }
    }

    // Send "interrupt" to self -- in the supported set, kernel accepts.
    // No handler is registered, so the EL0-return-tail dispatch is a
    // no-op; the note sits in the queue for the fd-read path.
    if notes::send(NoteTarget::SelfProc, "interrupt").is_err() {
        t_putstr("alloc-smoke: notes::send(SelfProc, interrupt) FAILED\n");
        return 1;
    }

    // Blocking read pulls the just-posted "interrupt" off the queue.
    let got = match n.read() {
        Ok(note) => note,
        Err(_) => {
            t_putstr("alloc-smoke: Notes::read interrupt FAILED\n");
            return 1;
        }
    };
    if got.name != "interrupt" {
        t_putstr("alloc-smoke: Notes::read name mismatch FAILED\n");
        return 1;
    }
    // The sender is "self" -- the kernel records the calling Proc's pid
    // (alloc-smoke's own pid). We don't know the value but it MUST be
    // Some(_) since synthetic posters use 0 and we're userspace.
    if got.from.is_none() {
        t_putstr("alloc-smoke: notes::send interrupt from None FAILED (expected Some)\n");
        return 1;
    }

    // After consuming, try_read returns None.
    match n.try_read() {
        Ok(None) => {}
        Ok(Some(_)) => {
            t_putstr("alloc-smoke: Notes::try_read after drain unexpectedly returned Some\n");
            return 1;
        }
        Err(_) => {
            t_putstr("alloc-smoke: Notes::try_read after drain FAILED\n");
            return 1;
        }
    }

    // ----- PollSet tests -----

    // Empty set: poll returns immediately with no readers.
    let mut empty = PollSet::new();
    match empty.poll(PollTimeout::Block) {
        Ok(mut it) => {
            if it.next().is_some() {
                t_putstr("alloc-smoke: PollSet empty produced result FAILED\n");
                return 1;
            }
        }
        Err(_) => {
            t_putstr("alloc-smoke: PollSet empty poll FAILED\n");
            return 1;
        }
    }

    // pipe(); poll the read end. Zero timeout: not yet ready.
    let (mut rd, mut wr) = match process::pipe() {
        Ok(p) => p,
        Err(_) => {
            t_putstr("alloc-smoke: U-2e pipe() FAILED\n");
            return 1;
        }
    };
    let mut ps = PollSet::new();
    ps.add(&rd, PollEvents::READ);
    let rd_fd = rd.as_raw_fd();
    match ps.poll(PollTimeout::Zero) {
        Ok(mut it) => {
            if it.next().is_some() {
                t_putstr("alloc-smoke: PollSet empty pipe Zero unexpectedly ready\n");
                return 1;
            }
        }
        Err(_) => {
            t_putstr("alloc-smoke: PollSet pipe Zero FAILED\n");
            return 1;
        }
    }
    // Write a byte; poll Block; expect rd ready with READ.
    if wr.write(b"x").is_err() {
        t_putstr("alloc-smoke: U-2e pipe write FAILED\n");
        return 1;
    }
    match ps.poll(PollTimeout::Block) {
        Ok(mut it) => {
            let ev = match it.next() {
                Some(e) => e,
                None => {
                    t_putstr("alloc-smoke: PollSet post-write produced no event FAILED\n");
                    return 1;
                }
            };
            if ev.fd != rd_fd {
                t_putstr("alloc-smoke: PollSet event fd mismatch FAILED\n");
                return 1;
            }
            if !ev.is_readable() {
                t_putstr("alloc-smoke: PollSet event not readable FAILED\n");
                return 1;
            }
            if it.next().is_some() {
                t_putstr("alloc-smoke: PollSet emitted spurious second event FAILED\n");
                return 1;
            }
        }
        Err(_) => {
            t_putstr("alloc-smoke: PollSet Block FAILED\n");
            return 1;
        }
    }
    // Consume the byte; remove rd from PollSet.
    let mut sink = [0u8; 1];
    if rd.read(&mut sink).is_err() {
        t_putstr("alloc-smoke: U-2e pipe drain FAILED\n");
        return 1;
    }
    ps.remove(&rd);
    if ps.len() != 0 {
        t_putstr("alloc-smoke: PollSet::remove did not shrink FAILED\n");
        return 1;
    }
    drop(rd);
    drop(wr);

    // Add the notes fd to a fresh PollSet; verify a self-posted note
    // makes the fd poll-ready. End-to-end smoke that notes integrate
    // with poll.
    let mut nps = PollSet::new();
    nps.add(&n, PollEvents::READ);
    // Should be empty.
    match nps.poll(PollTimeout::Zero) {
        Ok(mut it) => {
            if it.next().is_some() {
                t_putstr("alloc-smoke: notes-poll empty unexpectedly ready FAILED\n");
                return 1;
            }
        }
        Err(_) => {
            t_putstr("alloc-smoke: notes-poll empty FAILED\n");
            return 1;
        }
    }
    // Post and confirm poll wakes.
    if notes::send(NoteTarget::SelfProc, "interrupt").is_err() {
        t_putstr("alloc-smoke: notes::send for poll FAILED\n");
        return 1;
    }
    match nps.poll(PollTimeout::Block) {
        Ok(mut it) => match it.next() {
            Some(ev) if ev.is_readable() => {}
            _ => {
                t_putstr("alloc-smoke: notes-poll post-send not readable FAILED\n");
                return 1;
            }
        },
        Err(_) => {
            t_putstr("alloc-smoke: notes-poll Block FAILED\n");
            return 1;
        }
    }
    // Drain the pending note so we leave the queue clean.
    if n.read().is_err() {
        t_putstr("alloc-smoke: notes-poll drain FAILED\n");
        return 1;
    }
    drop(nps);
    drop(n);

    t_putstr("alloc-smoke: Notes + Mask + PollSet OK\n");
    0
}
