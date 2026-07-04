// /bin/fsbench -- the native in-guest FS throughput bench (the netperf analog
// for storage). Measures END-TO-END filesystem throughput through the REAL
// Thylacine stack: SYS_READ/WRITE/FSYNC -> dev9p client -> SrvConn -> stratumd
// (the on-device FS server) -> bdev_thylacine -> virtio-blk -> the Stratum pool.
//
// WHY THIS EXISTS: the Stratum Stabilization arc measured the FS *core*
// host-side (the ctest benches: bench_concurrent_write / bench_commit /
// bench_create_many / bench_crypto). But the number a real workload (the go
// build) actually experiences is the IN-GUEST end-to-end throughput -- which
// additionally pays the syscall + 9P + msize-chunking + scheduler cost the host
// benches skip. That number had no instrument; this is it.
//
// THE MODES (each maps onto a Stabilization area, exercising its work in-guest):
//   seqwrite  -- write a large file sequentially + fsync   -> MiB/s  (Area A: coalesce/extent write)
//   seqread   -- read it back (cold)                        -> MiB/s  (Area B: multi-extent read + decrypt)
//   reread    -- read it AGAIN (warm)                       -> MiB/s + speedup
//                (Area E: the #343 decrypted-extent cache -- stratumd serves the
//                 2nd read from its dcache, decrypt -> memcpy, through the real stack)
//   rewrite   -- overwrite interior blocks (seek+write)     -> MiB/s  (Area A: the RMW / header-patch path)
//   create    -- create+write+close N small files           -> files/s + MiB/s (Area S: the Pleiades object-file storm)
//   fsync     -- write+fsync per small file (durable)        -> files/s (Area G: the per-file commit / bdev FLUSH cost)
//   sweep     -- seqwrite at several chunk sizes             -> MiB/s per size (Area D: record-size sensitivity)
//
// `fsbench`              -- the short boot/CI probe: seqwrite/seqread/reread +
//                          create + fsync, bounded; prints data, exits 0 (data is
//                          the value -- no flaky perf-threshold gate, per the
//                          no-host-load discipline).
//   `fsbench all`        -- the comprehensive run: every mode at solid params.
//   `fsbench <mode> [size_mib] [chunk_kib]` -- one mode, custom params, from the shell.
//
// Pure userspace -- a buggy bench corrupts only its own scratch files (the kernel
// + stratumd validate every op). Composes nothing privileged; adds no invariant.
// Cleans up its scratch files after every mode (the pool persists across boots).

#![no_std]
#![no_main]

extern crate alloc;

use alloc::format;
use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::fs::{self, File, OpenOptions};
use libthyla_rs::io::{Read, Seek, SeekFrom, Write};
use libthyla_rs::time::Instant;
use libthyla_rs::{t_exits, t_fsync, t_putstr};

// The pool-root writable scratch (joey mkdir's /tmp on the disk-backed root).
const DEFAULT_SCRATCH: &str = "/tmp";
const MIB: u64 = 1024 * 1024;

// ---------------------------------------------------------------------------
// Helpers.
// ---------------------------------------------------------------------------

/// MiB/s * 100 (integer math, u128 to avoid overflow on the bytes*1e9 product).
fn mibps_x100(bytes: u64, ns: u64) -> u64 {
    if ns == 0 {
        return 0;
    }
    ((bytes as u128) * 1_000_000_000u128 * 100 / (ns as u128) / (MIB as u128)) as u64
}

fn fmt_mibps(bytes: u64, ns: u64) -> String {
    let x = mibps_x100(bytes, ns);
    format!("{}.{:02} MiB/s", x / 100, x % 100)
}

fn per_sec(count: u64, ns: u64) -> u64 {
    if ns == 0 {
        return 0;
    }
    ((count as u128) * 1_000_000_000u128 / (ns as u128)) as u64
}

/// A reusable data buffer filled with varied (non-zero, non-dedupable) bytes.
/// HOT extents do not dedup, but varied content keeps the bench honest against
/// any all-zero special-casing.
fn make_buf(len: usize, seed: u64) -> Vec<u8> {
    let mut v = vec![0u8; len];
    let mut x = seed | 1;
    for b in v.iter_mut() {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        *b = (x & 0xff) as u8;
    }
    v
}

/// Ensure the scratch directory exists (joey is PRINCIPAL_SYSTEM -> owns the
/// pool root, so it can mkdir /tmp). Idempotent.
fn ensure_scratch(dir: &str) -> bool {
    if fs::exists(dir) {
        return true;
    }
    fs::create_dir(dir).is_ok()
}

fn rm(path: &str) {
    let _ = fs::remove_file(path);
}

/// Write `total` bytes to `path` in `buf`-sized chunks; optionally fsync at the
/// end (the durability barrier -- Area G's bdev FLUSH). Returns (bytes, ns) with
/// the timer spanning the create..(write+fsync), or None on any I/O error.
// Last write_file failure, for the FAIL prints (diagnostic; fsbench is
// single-threaded): which stage failed + the errno the kernel returned
// (Area-F propagates the real Stratum ecode, so this names
// ENOSPC-vs-EWEDGED-vs-EIO directly).
static mut G_WERR: i32 = 0;
static mut G_WSTAGE: &str = "-";

fn werr_note(stage: &'static str, e: i32) {
    unsafe {
        G_WERR = e;
        G_WSTAGE = stage;
    }
}

fn werr_str() -> String {
    unsafe { format!("stage={} errno={}", G_WSTAGE, G_WERR) }
}

fn write_file(path: &str, total: u64, buf: &[u8], do_fsync: bool) -> Option<(u64, u64)> {
    let t = Instant::now();
    let mut f = match File::create(path) {
        Ok(f) => f,
        Err(e) => { werr_note("create", e.as_errno()); return None; }
    };
    let mut written = 0u64;
    while written < total {
        let n = ((total - written) as usize).min(buf.len());
        if let Err(e) = f.write_all(&buf[..n]) {
            werr_note("write", e.as_errno());
            return None;
        }
        written += n as u64;
    }
    if do_fsync {
        // t_fsync on the underlying fd -- the real durability path (stratumd
        // Tsync -> commit -> bdev VIRTIO_BLK_T_FLUSH).
        let rc = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
        if rc < 0 {
            werr_note("fsync", -(rc as i32));
            return None;
        }
    }
    Some((written, t.elapsed().as_nanos() as u64))
}

/// Read the whole of `path` into `buf`-sized reads. Returns (bytes, ns).
fn read_file(path: &str, buf: &mut [u8]) -> Option<(u64, u64)> {
    let t = Instant::now();
    let mut f = File::open(path).ok()?;
    let mut total = 0u64;
    loop {
        match f.read(buf) {
            Ok(0) => break,
            Ok(k) => total += k as u64,
            Err(_) => return None,
        }
    }
    Some((total, t.elapsed().as_nanos() as u64))
}

/// State fingerprint after a write-path failure (#35 fsync cascade): one op
/// per FS path class, each with its errno, on a single line. Distinguishes a
/// wedged fs (everything fails) / a cap-pinned dirty buffer (buffered insert
/// ENOSPC, engine ops OK) / a dead bdev (direct extent write fails, engine +
/// reads OK). Path classes exercised on one fresh file:
///   create = Tlcreate (inode/dirent engine, RAM)
///   w32    = 32 B at 0   -> INLINE fast path (engine RAM, no buffer, no bdev)
///   w4k    = 4 KiB next  -> INLINE->EXTENT transition = DIRECT bdev write
///   w64    = 64 B next   -> buffered insert (dirty-buffer cap test)
///   fsync  = drain-all retry (the failing op itself)
///   readver= read of /thylacine-version (69 B inline -- no bdev; fails only
///            on a wedge/session death)
fn fingerprint(dir: &str) {
    let p = format!("{}/fsbench-fp.dat", dir);
    let mut line = String::from("fsbench: FP");
    match File::create(&p) {
        Ok(mut f) => {
            line.push_str(" create=0");
            match f.write_all(&[0xAAu8; 32]) {
                Ok(()) => line.push_str(" w32=0"),
                Err(e) => line.push_str(&format!(" w32={}", e.as_errno())),
            }
            match f.write_all(&[0xBBu8; 4096]) {
                Ok(()) => line.push_str(" w4k=0"),
                Err(e) => line.push_str(&format!(" w4k={}", e.as_errno())),
            }
            match f.write_all(&[0xCCu8; 64]) {
                Ok(()) => line.push_str(" w64=0"),
                Err(e) => line.push_str(&format!(" w64={}", e.as_errno())),
            }
            let rc = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
            line.push_str(&format!(
                " fsync={}",
                if rc < 0 { -(rc as i32) } else { 0 }
            ));
        }
        Err(e) => line.push_str(&format!(" create={}", e.as_errno())),
    }
    let mut rb = [0u8; 64];
    match File::open("/thylacine-version").and_then(|mut f| f.read(&mut rb)) {
        Ok(n) => line.push_str(&format!(" readver={}B", n)),
        Err(e) => line.push_str(&format!(" readver=E{}", e.as_errno())),
    }
    line.push('\n');
    t_putstr(&line);
    rm(&p);
}

// ---------------------------------------------------------------------------
// Modes. Each prints one summary line; FAIL lines are fail-soft (the boot
// probe never breaks the boot -- data is the value).
// ---------------------------------------------------------------------------

/// seqwrite -> seqread -> reread share one file (write once, read twice): the 2nd
/// read is the #343 dcache hit (stratumd serves it decrypt-free). Returns the
/// scratch file path so the caller can clean it up; None on a write failure.
fn mode_write_read_reread(dir: &str, total: u64, chunk: usize) -> Option<String> {
    let path = format!("{}/fsbench-seq.dat", dir);
    let buf = make_buf(chunk, 0x5eed_f00d);

    match write_file(&path, total, &buf, true) {
        Some((bytes, ns)) => {
            t_putstr(&format!(
                "seqwrite: {} MiB in {} ms -> {} (write+fsync, {} KiB chunks)\n",
                bytes / MIB,
                ns / 1_000_000,
                fmt_mibps(bytes, ns),
                chunk / 1024
            ));
        }
        None => {
            t_putstr(&format!("seqwrite: FAIL ({})\n", werr_str()));
            fingerprint(dir);
            rm(&path);
            return None;
        }
    }

    let mut rbuf = vec![0u8; chunk];
    let cold = match read_file(&path, &mut rbuf) {
        Some((bytes, ns)) => {
            t_putstr(&format!(
                "seqread:  {} MiB in {} ms -> {} (cold -- decrypt path)\n",
                bytes / MIB,
                ns / 1_000_000,
                fmt_mibps(bytes, ns)
            ));
            Some(ns)
        }
        None => {
            t_putstr("seqread:  FAIL (read error)\n");
            None
        }
    };
    match read_file(&path, &mut rbuf) {
        Some((bytes, ns)) => {
            let spd = match cold {
                Some(c) if ns > 0 => format!("{}.{:01}x vs cold", c * 10 / ns / 10, (c * 10 / ns) % 10),
                _ => String::from("n/a"),
            };
            t_putstr(&format!(
                "reread:   {} MiB in {} ms -> {} (warm -- #343 dcache; {})\n",
                bytes / MIB,
                ns / 1_000_000,
                fmt_mibps(bytes, ns),
                spd
            ));
        }
        None => {
            t_putstr("reread:   FAIL (read error)\n");
        }
    }
    Some(path)
}

/// rewrite -- overwrite `iters` interior blocks of an existing file (seek+write+
/// fsync). The Area-A RMW / ar-header-patch path: a sub-extent overwrite must
/// read-modify-write the covering extent.
fn mode_rewrite(dir: &str, file_mib: u64, block: usize, iters: u64) {
    let path = format!("{}/fsbench-rw.dat", dir);
    let total = file_mib * MIB;
    let buf = make_buf(block, 0xbeef_cafe);
    if write_file(&path, total, &buf, true).is_none() {
        t_putstr("rewrite:  FAIL (setup write)\n");
        rm(&path);
        return;
    }
    let span = total.saturating_sub(block as u64).max(1);
    let mut f = match File::open(&path) {
        Ok(f) => f,
        Err(_) => {
            t_putstr("rewrite:  FAIL (reopen)\n");
            rm(&path);
            return;
        }
    };
    let t = Instant::now();
    let mut moved = 0u64;
    let mut x = 0x1234_5678u64;
    for _ in 0..iters {
        x = x.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
        let off = (x >> 16) % span;
        let off = off & !(block as u64 - 1).min(off); // block-ish aligned
        if f.seek(SeekFrom::Start(off)).is_err() || f.write_all(&buf).is_err() {
            t_putstr("rewrite:  FAIL (seek/write)\n");
            rm(&path);
            return;
        }
        moved += block as u64;
    }
    let _ = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
    let ns = t.elapsed().as_nanos() as u64;
    t_putstr(&format!(
        "rewrite:  {} interior {}-KiB blocks ({} MiB) in {} ms -> {} (RMW + fsync)\n",
        iters,
        block / 1024,
        moved / MIB,
        ns / 1_000_000,
        fmt_mibps(moved, ns)
    ));
    rm(&path);
}

/// create -- N small files, each create+write+close (NO per-file fsync; one
/// fsync of the last). The Pleiades object-file storm (Area S): per-file create
/// + inode-alloc + dirent throughput.
fn mode_create(dir: &str, n: u64, size: usize) {
    let buf = make_buf(size, 0xc0ffee);
    let t = Instant::now();
    let mut ok = 0u64;
    let mut bytes = 0u64;
    for i in 0..n {
        let path = format!("{}/fsbench-c{}.dat", dir, i);
        match File::create(&path) {
            Ok(mut f) => match f.write_all(&buf) {
                Ok(()) => {
                    ok += 1;
                    bytes += size as u64;
                }
                Err(e) => {
                    werr_note("cb-write", e.as_errno());
                    break;
                }
            },
            Err(e) => {
                werr_note("cb-create", e.as_errno());
                break;
            }
        }
    }
    let ns = t.elapsed().as_nanos() as u64;
    t_putstr(&format!(
        "create:   {} files x {} B in {} ms -> {} files/s, {} ({})\n",
        ok,
        size,
        ns / 1_000_000,
        per_sec(ok, ns),
        fmt_mibps(bytes, ns),
        if ok == n {
            String::from("all OK")
        } else {
            format!("stopped early: {}", werr_str())
        }
    ));
    for i in 0..n {
        rm(&format!("{}/fsbench-c{}.dat", dir, i));
    }
}

/// fsync -- N small files, each create+write+FSYNC+close (durable). The per-file
/// commit rate (Area G): every file is a real durability commit (bdev FLUSH).
/// The difference vs `create` is the cost of the commit barrier.
fn mode_fsync(dir: &str, n: u64, size: usize) {
    let buf = make_buf(size, 0xfaded);
    let t = Instant::now();
    let mut ok = 0u64;
    for i in 0..n {
        let path = format!("{}/fsbench-f{}.dat", dir, i);
        let done = (|| -> Option<()> {
            let mut f = match File::create(&path) {
                Ok(f) => f,
                Err(e) => {
                    werr_note("fb-create", e.as_errno());
                    return None;
                }
            };
            if let Err(e) = f.write_all(&buf) {
                werr_note("fb-write", e.as_errno());
                return None;
            }
            let rc = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
            if rc < 0 {
                werr_note("fb-fsync", -(rc as i32));
                return None;
            }
            Some(())
        })();
        if done.is_some() {
            ok += 1;
        } else {
            break;
        }
    }
    let ns = t.elapsed().as_nanos() as u64;
    let us_per = if ok > 0 { (ns / 1000) / ok } else { 0 };
    t_putstr(&format!(
        "fsync:    {} files x {} B (write+fsync each) in {} ms -> {} files/s ({} us/durable-file){}\n",
        ok,
        size,
        ns / 1_000_000,
        per_sec(ok, ns),
        us_per,
        if ok == n {
            String::new()
        } else {
            format!(" [stopped early: {}]", werr_str())
        }
    ));
    for i in 0..n {
        rm(&format!("{}/fsbench-f{}.dat", dir, i));
    }
}

/// sweep -- seqwrite at several chunk sizes (Area D: throughput scales with the
/// record/chunk size -- the fixed per-extent overhead amortizes over larger I/O).
fn mode_sweep(dir: &str, file_mib: u64) {
    let total = file_mib * MIB;
    let path = format!("{}/fsbench-sw.dat", dir);
    for &kib in &[4usize, 64, 256, 1024] {
        let buf = make_buf(kib * 1024, 0xa5a5 ^ kib as u64);
        match write_file(&path, total, &buf, true) {
            Some((bytes, ns)) => {
                t_putstr(&format!(
                    "sweep:    chunk {:>5} KiB -> {} ({} MiB in {} ms)\n",
                    kib,
                    fmt_mibps(bytes, ns),
                    bytes / MIB,
                    ns / 1_000_000
                ));
            }
            None => {
                t_putstr(&format!("sweep:    chunk {} KiB FAIL\n", kib));
                break;
            }
        }
        rm(&path);
    }
}

/// COHERENCE -- the go-build `link: _pkg_.a: not package main` diagnostic. The
/// linker reads the user package's compiled archive WHOLE (object included),
/// while every compile reading a dependency only touches the early `__.PKGDEF`
/// export data -- so a freshly-written-but-UNSYNCED file that reports a stale
/// size or a truncated TAIL to a FRESH reader fid would fail the link but pass
/// every compile (exactly the observed shape). Write `sz` bytes of a known
/// pattern, close the fd (mirrors the compiler process exiting), reopen a FRESH
/// fid, then fstat-size + full read + byte-compare (tail especially). The fsync
/// variant is the control -- it MUST pass. Prints one verdict line; fail-soft.
fn coherence_check(dir: &str, sz: usize, do_fsync: bool) {
    let label = if do_fsync { "fsync   " } else { "no-fsync" };
    let path = format!("{}/fsbench-coh.dat", dir);
    rm(&path);
    let buf = make_buf(sz, 0xC0_4E_5E_ED ^ sz as u64);
    // Write + close. The fd closes when `f` drops at the end of this block --
    // exactly the compiler closing _pkg_.a (and its process exiting) before the
    // linker opens it. NO fsync on the no-fsync path (what go does for $WORK).
    {
        let mut f = match File::create(&path) {
            Ok(f) => f,
            Err(_) => {
                t_putstr("fsbench: COHERENCE create FAIL\n");
                return;
            }
        };
        if f.write_all(&buf).is_err() {
            t_putstr("fsbench: COHERENCE write FAIL\n");
            rm(&path);
            return;
        }
        if do_fsync {
            let _ = unsafe { t_fsync(f.as_raw_fd() as i64, 0) };
        }
    }
    // Reopen a FRESH fid (the cross-fid read the linker does).
    let md_size = fs::metadata(&path).map(|m| m.len()).unwrap_or(u64::MAX);
    let mut rbuf = vec![0u8; sz];
    let mut got = 0usize;
    match File::open(&path) {
        Ok(mut f) => loop {
            match f.read(&mut rbuf[got..]) {
                Ok(0) => break,
                Ok(k) => {
                    got += k;
                    if got >= sz {
                        break;
                    }
                }
                Err(_) => break,
            }
        },
        Err(_) => {
            t_putstr("fsbench: COHERENCE reopen FAIL\n");
            rm(&path);
            return;
        }
    }
    let size_ok = md_size == sz as u64;
    let len_ok = got == sz;
    let first_bad = (0..got).find(|&i| rbuf[i] != buf[i]);
    let content_ok = len_ok && first_bad.is_none();
    let verdict = if size_ok && content_ok { "PASS" } else { "FAIL" };
    t_putstr(&format!(
        "fsbench: COHERENCE {} sz={} fstat_size={} read={} first_bad={:?} -> {}\n",
        label, sz, md_size, got, first_bad, verdict
    ));
    rm(&path);
}

/// ARPATCH -- the `ar` archive write pattern go uses for `_pkg_.a`. An archive
/// entry header carries the entry SIZE, known only after the entry data is
/// written, so `pack`/`compile -pack` writes a placeholder, writes the data,
/// then SEEKS BACK and overwrites the size field in the header. A fresh reader
/// (the linker) must then see the patched interior AND the intact TAIL (the
/// object bytes after the header). The `not package main` link failure is
/// consistent with a seek-back interior overwrite truncating/losing that tail --
/// a pattern the contiguous coherence check does NOT exercise. Write contiguous,
/// reopen+seek+overwrite an interior field, reopen fresh, verify size+patch+tail.
fn arpatch_check(dir: &str, sz: usize) {
    let path = format!("{}/fsbench-arp.dat", dir);
    rm(&path);
    let buf = make_buf(sz, 0x42AA_CE99);
    {
        let mut f = match File::create(&path) {
            Ok(f) => f,
            Err(_) => {
                t_putstr("fsbench: ARPATCH create FAIL\n");
                return;
            }
        };
        if f.write_all(&buf).is_err() {
            t_putstr("fsbench: ARPATCH setup-write FAIL\n");
            rm(&path);
            return;
        }
    }
    let patch_off: usize = 48; // the `ar` header size-field offset
    let patch: [u8; 10] = [b'9'; 10];
    {
        // R/W open (ORDWR, no truncate) -- exactly go's `os.OpenFile(target,
        // os.O_RDWR, 0)`. (File::open is OREAD-only since A-3b, so it cannot
        // write -- that was the prior false FAIL.)
        let mut f = match OpenOptions::new().read(true).write(true).open(&path) {
            Ok(f) => f,
            Err(_) => {
                t_putstr("fsbench: ARPATCH reopen-rw FAIL\n");
                rm(&path);
                return;
            }
        };
        if f.seek(SeekFrom::Start(patch_off as u64)).is_err() || f.write_all(&patch).is_err() {
            t_putstr("fsbench: ARPATCH seek/patch FAIL\n");
            rm(&path);
            return;
        }
    }
    let md_size = fs::metadata(&path).map(|m| m.len()).unwrap_or(u64::MAX);
    let mut rbuf = vec![0u8; sz];
    let mut got = 0usize;
    match File::open(&path) {
        Ok(mut f) => loop {
            match f.read(&mut rbuf[got..]) {
                Ok(0) => break,
                Ok(k) => {
                    got += k;
                    if got >= sz {
                        break;
                    }
                }
                Err(_) => break,
            }
        },
        Err(_) => {
            t_putstr("fsbench: ARPATCH verify-reopen FAIL\n");
            rm(&path);
            return;
        }
    }
    let size_ok = md_size == sz as u64;
    let len_ok = got == sz;
    let patch_ok = len_ok && rbuf[patch_off..patch_off + patch.len()] == patch;
    let tail_start = patch_off + patch.len();
    let tail_bad = if len_ok {
        (tail_start..sz).find(|&i| rbuf[i] != buf[i])
    } else {
        None
    };
    let verdict = if size_ok && len_ok && patch_ok && tail_bad.is_none() {
        "PASS"
    } else {
        "FAIL"
    };
    t_putstr(&format!(
        "fsbench: ARPATCH sz={} fstat={} read={} patch={} tail_bad={:?} -> {}\n",
        sz,
        md_size,
        got,
        if patch_ok { "ok" } else { "BAD" },
        tail_bad,
        verdict
    ));
    rm(&path);
}

/// Build a minimal ar(5) 60-byte entry header: name@0..16, size ASCII@48..58,
/// magic ('`','\n')@58..60. Mirrors go's `archive.FormatHeader`.
fn arw_hdr(name: &str, size: usize) -> [u8; 60] {
    let mut h = [b' '; 60];
    let nb = name.as_bytes();
    let nlen = core::cmp::min(nb.len(), 16);
    h[..nlen].copy_from_slice(&nb[..nlen]);
    let s = format!("{}", size);
    let slen = core::cmp::min(s.len(), 10);
    h[48..48 + slen].copy_from_slice(&s.as_bytes()[..slen]);
    h[58] = b'`';
    h[59] = b'\n';
    h
}

/// Write ONE archive entry exactly as go's cmd/compile `finishArchiveEntry`:
/// a placeholder 60-byte header at `hdr_off`, then the body (+ even pad), then
/// SEEK BACK to overwrite the header with the real size, then SEEK FORWARD past
/// the body so the next entry's write lands after it. Returns the offset where
/// the next entry's header begins, or u64::MAX on any I/O error.
fn arw_entry(f: &mut File, hdr_off: u64, name: &str, body: &[u8]) -> u64 {
    let body_off = hdr_off + 60;
    let pad = body.len() & 1;
    let next = body_off + body.len() as u64 + pad as u64;
    if f.seek(SeekFrom::Start(hdr_off)).is_err() {
        return u64::MAX;
    }
    if f.write_all(&[0u8; 60]).is_err() || f.write_all(body).is_err() {
        return u64::MAX;
    }
    if pad != 0 && f.write_all(&[0u8]).is_err() {
        return u64::MAX;
    }
    // seek BACK -> overwrite the placeholder header with the real size.
    if f.seek(SeekFrom::Start(hdr_off)).is_err()
        || f.write_all(&arw_hdr(name, body.len())).is_err()
    {
        return u64::MAX;
    }
    // seek FORWARD past body+pad (so the NEXT write does not clobber the body).
    if f.seek(SeekFrom::Start(next)).is_err() {
        return u64::MAX;
    }
    next
}

/// Reproduce go `go tool compile -pack`'s archive shape byte-for-byte: an
/// `!<arch>\n` magic, then TWO seek-back-written entries (__.PKGDEF carrying the
/// `\nmain\n` line ldpkg scans for, then _go_.o). ARPATCH covers seek-back-
/// overwrite-then-close; THIS covers the seek-FORWARD-past-data-then-write-the-
/// next-entry sequence -- the one part of `finishArchiveEntry` ARPATCH does not.
/// If the on-device lseek/write emulation mis-handles it, the archive is corrupt
/// (wrong size field -> link reads the wrong length; or body0 clobbered -> the
/// `main` line vanishes) -- the exact `link: not package main` failure.
fn arwrite_check(dir: &str) {
    let path = format!("{}/fsbench-arw.a", dir);
    rm(&path);
    let body0 =
        b"go object thylacine arm64 go1.25.3\nbuild id \"deadbeefcafe\"\nmain\n\n$B\nEXPORTDATA-ENTRY-0"
            .to_vec();
    let body1: Vec<u8> = (0..1777u32).map(|i| (i as u8) ^ 0x5A).collect();
    let h0: u64 = 8; // after the 8-byte magic
    let pad0 = body0.len() & 1;
    let h1: u64 = h0 + 60 + body0.len() as u64 + pad0 as u64;
    {
        let mut f = match OpenOptions::new()
            .read(true)
            .write(true)
            .create(true)
            .truncate(true)
            .open(&path)
        {
            Ok(f) => f,
            Err(_) => {
                t_putstr("fsbench: ARWRITE create FAIL\n");
                return;
            }
        };
        if f.write_all(b"!<arch>\n").is_err() {
            t_putstr("fsbench: ARWRITE magic FAIL\n");
            rm(&path);
            return;
        }
        if arw_entry(&mut f, h0, "__.PKGDEF", &body0) == u64::MAX
            || arw_entry(&mut f, h1, "_go_.o", &body1) == u64::MAX
        {
            t_putstr("fsbench: ARWRITE write-seq FAIL\n");
            rm(&path);
            return;
        }
    } // close -> dirty-buffer drain to extents
    // Verify: reopen fresh, read whole, check magic + both size fields + bodies.
    let total = (h1 + 60 + body1.len() as u64 + (body1.len() & 1) as u64) as usize;
    let mut rbuf = vec![0u8; total];
    let mut got = 0usize;
    match File::open(&path) {
        Ok(mut f) => loop {
            match f.read(&mut rbuf[got..]) {
                Ok(0) => break,
                Ok(k) => {
                    got += k;
                    if got >= total {
                        break;
                    }
                }
                Err(_) => break,
            }
        },
        Err(_) => {
            t_putstr("fsbench: ARWRITE verify-reopen FAIL\n");
            rm(&path);
            return;
        }
    }
    let parse_size = |off: usize| -> Option<usize> {
        if off + 58 > got {
            return None;
        }
        core::str::from_utf8(&rbuf[off + 48..off + 58])
            .ok()
            .map(|s| s.trim())
            .and_then(|s| s.parse::<usize>().ok())
    };
    let magic_ok = got >= 8 && &rbuf[0..8] == b"!<arch>\n";
    let sz0 = parse_size(h0 as usize);
    let sz1 = parse_size(h1 as usize);
    let body0_off = (h0 + 60) as usize;
    let body1_off = (h1 + 60) as usize;
    let body0_ok = body0_off + body0.len() <= got
        && rbuf[body0_off..body0_off + body0.len()] == body0[..];
    let body1_ok = body1_off + body1.len() <= got
        && rbuf[body1_off..body1_off + body1.len()] == body1[..];
    // The exact ldpkg success condition: a header line == "main".
    let main_seen = rbuf[..got]
        .windows(6)
        .any(|w| w == b"\nmain\n");
    let verdict = if magic_ok
        && sz0 == Some(body0.len())
        && sz1 == Some(body1.len())
        && body0_ok
        && body1_ok
        && main_seen
    {
        "PASS"
    } else {
        "FAIL"
    };
    t_putstr(&format!(
        "fsbench: ARWRITE read={} magic={} sz0={:?}/{} sz1={:?}/{} body0={} body1={} main_line={} -> {}\n",
        got,
        magic_ok,
        sz0,
        body0.len(),
        sz1,
        body1.len(),
        if body0_ok { "ok" } else { "BAD" },
        if body1_ok { "ok" } else { "BAD" },
        main_seen,
        verdict
    ));
    rm(&path);
}

/// The comprehensive run: every mode at solid params, delimited.
fn run_all(dir: &str, size_mib: u64) {
    t_putstr(&format!("fsbench: ==== comprehensive FS bench (scratch={}, {} MiB files) ====\n", dir, size_mib));
    if let Some(p) = mode_write_read_reread(dir, size_mib * MIB, 256 * 1024) {
        rm(&p);
    }
    mode_rewrite(dir, size_mib, 64 * 1024, 512);
    mode_create(dir, 2000, 4096);
    mode_fsync(dir, 500, 4096);
    mode_sweep(dir, size_mib);
    t_putstr("fsbench: ==== END comprehensive ====\n");
}

// ---------------------------------------------------------------------------
// Entry.
// ---------------------------------------------------------------------------

fn fail(why: &str) -> ! {
    t_putstr(&format!("fsbench: FAIL -- {}\n", why));
    unsafe { t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let args = env::args();
    let mode = args.get_str(1);
    let dir = DEFAULT_SCRATCH;

    if !ensure_scratch(dir) {
        fail("scratch dir unavailable (cannot mkdir /tmp)");
    }

    // arg2 = file size MiB (seq/rewrite/sweep) OR file count (create/fsync);
    // arg3 = chunk KiB (seq).
    let p2 = args.get_str(2).and_then(|s| s.parse::<u64>().ok());
    let p3 = args.get_str(3).and_then(|s| s.parse::<u64>().ok());

    match mode {
        Some("seqwrite") | Some("seqread") | Some("reread") | Some("seq") => {
            let mib = p2.unwrap_or(64).clamp(1, 1024);
            let chunk = (p3.unwrap_or(256).clamp(4, 8192) * 1024) as usize;
            if let Some(p) = mode_write_read_reread(dir, mib * MIB, chunk) {
                rm(&p);
            }
        }
        Some("rewrite") => {
            let mib = p2.unwrap_or(16).clamp(1, 256);
            let iters = p3.unwrap_or(2000);
            mode_rewrite(dir, mib, 64 * 1024, iters);
        }
        Some("create") => {
            let n = p2.unwrap_or(5000).clamp(1, 200_000);
            let size = (p3.unwrap_or(4).clamp(1, 1024) * 1024) as usize;
            mode_create(dir, n, size);
        }
        Some("fsync") => {
            let n = p2.unwrap_or(1000).clamp(1, 100_000);
            let size = (p3.unwrap_or(4).clamp(1, 1024) * 1024) as usize;
            mode_fsync(dir, n, size);
        }
        Some("sweep") => {
            mode_sweep(dir, p2.unwrap_or(64).clamp(1, 512));
        }
        Some("coherence") | Some("coh") => {
            // write-no-fsync -> close -> reopen-fresh -> fstat-size + read-verify.
            // The go-build `link: not package main` hypothesis (a truncated tail
            // on an unsynced file seen by a fresh reader).
            let sz = p2.map(|m| (m as usize).clamp(1, 8 * 1024 * 1024)).unwrap_or(70_000);
            coherence_check(dir, sz, false);
            coherence_check(dir, sz, true);
        }
        Some("arwrite") | Some("arw") => {
            // The go `cmd/compile finishArchiveEntry` seek-back-then-seek-forward
            // archive-write pattern -- the `link: not package main` hypothesis.
            arwrite_check(dir);
        }
        Some("all") => {
            run_all(dir, p2.unwrap_or(64).clamp(1, 256));
        }
        _ => {
            // The short boot/CI probe: bounded (fits a modest pool, ~seconds),
            // prints data, exits 0. Smaller files than `all` so it never ENOSPCs
            // a tight pool or stalls the boot.
            t_putstr("fsbench: FS throughput probe -- seqwrite/seqread/reread + create + fsync ('fsbench all' = the full bench)\n");
            if let Some(p) = mode_write_read_reread(dir, 8 * MIB, 256 * 1024) {
                rm(&p);
            }
            mode_create(dir, 200, 4096);
            mode_fsync(dir, 50, 4096);
            // The write-close-reopen coherence diagnostic (the go-build link
            // hypothesis). Small (partial-tail) + large (multi-RPC, dirty-buffer-
            // cap-crossing) no-fsync cases + one fsync control.
            coherence_check(dir, 3000, false);
            coherence_check(dir, 70_000, false);
            coherence_check(dir, 70_000, true);
            // The go-archive seek-back-patch pattern (the `not package main`
            // hypothesis): small (single-extent) + large (multi-extent) so an
            // interior overwrite that drops the tail surfaces.
            arpatch_check(dir, 12_000);
            arpatch_check(dir, 200_000);
            // The go `cmd/compile finishArchiveEntry` two-entry seek-back +
            // seek-FORWARD-then-write-more pattern (the one part ARPATCH omits)
            // -- the leading `link: not package main` root hypothesis.
            arwrite_check(dir);
            t_putstr("fsbench: PROBE OK\n");
        }
    }
    0
}
