// /u-redir-test -- U-6d-b post-pivot redirect round-trip (write side).
//
// u-test runs PRE-pivot (devramfs root, read-only) so its redirect flow
// can only cover the no-writable-FS forms (heredoc + the failure /
// NotImplemented paths). This binary is spawned by joey POST-pivot, when
// the territory root is the writable, SYSTEM-owned dev9p (Stratum) FS, and
// proves the `>` / `>>` / `<` round-trip end to end against a real file:
//
//   pipe-src > /u-redir-out     -- create + write the 13-byte payload
//   (read back, assert == payload)                    [`>` create+write]
//   pipe-src >> /u-redir-out    -- append (file grows to 26)
//   (read back, assert payload x2)                    [`>>` append]
//   pipe-src > /u-redir-out     -- truncate + rewrite (back to 13)
//   (read back, assert == payload)                    [`>` truncate]
//   pipe-sink < /u-redir-out    -- stdin from the file; exit 0 iff it
//                                  read exactly the payload     [`<`]
//
// pipe-src writes its fixed payload to fd 1; pipe-sink reads fd 0 to EOF
// and exits 0 iff it got exactly the payload. The spawn name lookup is
// devramfs-backed regardless of pivot (kernel/syscall.c devramfs_lookup),
// so pipe-src/pipe-sink resolve from the cpio without a pool bake; only
// the redirect TARGET (/u-redir-out) lives on the post-pivot dev9p root.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::fs::File;
use libthyla_rs::io::Read;
use libthyla_rs::t_putstr;
use libutopia::eval::{eval_source, Env, StatementFlow};

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

const PATH: &str = "/u-redir-out";
// pipe-src's fixed output. Keep in sync with usr/pipe-src + usr/pipe-sink.
const PAYLOAD: &[u8] = b"PIPE-DATA-OK\n";

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // 1. `>` create + write.
    if run("pipe-src > /u-redir-out") != 0 {
        return fail("`>` create status");
    }
    match read_file() {
        Some(b) if b.as_slice() == PAYLOAD => {}
        _ => return fail("`>` create content"),
    }

    // 2. `>>` append -- the file grows to two copies of the payload.
    if run("pipe-src >> /u-redir-out") != 0 {
        return fail("`>>` append status");
    }
    match read_file() {
        Some(b)
            if b.len() == PAYLOAD.len() * 2
                && &b[..PAYLOAD.len()] == PAYLOAD
                && &b[PAYLOAD.len()..] == PAYLOAD => {}
        _ => return fail("`>>` append content"),
    }

    // 3. `>` truncate -- re-`>` resets the file to one payload.
    if run("pipe-src > /u-redir-out") != 0 {
        return fail("`>` truncate status");
    }
    match read_file() {
        Some(b) if b.as_slice() == PAYLOAD => {}
        _ => return fail("`>` truncate content"),
    }

    // 4. `<` stdin-from-file -- pipe-sink reads fd 0 and exits 0 iff it
    //    got exactly the payload.
    if run("pipe-sink < /u-redir-out") != 0 {
        return fail("`<` read-redirect status");
    }

    t_putstr("u-redir-test: all OK\n");
    0
}

/// Evaluate one source line in a fresh interactive Env; return the
/// resulting $status, or -1 if the eval itself errored.
fn run(src: &str) -> i32 {
    let mut env = Env::new();
    env.interactive = true;
    match eval_source(&mut env, src) {
        Ok(StatementFlow::Normal) => env.status(),
        _ => -1,
    }
}

/// Read /u-redir-out fully into a heap buffer.
fn read_file() -> Option<Vec<u8>> {
    let mut f = File::open(PATH).ok()?;
    let mut buf = Vec::new();
    f.read_to_end(&mut buf).ok()?;
    Some(buf)
}

fn fail(tag: &str) -> i64 {
    t_putstr("u-redir-test: FAILED -- ");
    t_putstr(tag);
    t_putstr("\n");
    1
}
