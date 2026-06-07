// pwd -- print the working directory.
//
// DEGENERATE at v1.0 (DOC-GAP G07): Thylacine has no per-Proc
// current-directory concept. libthyla-rs::fs only accepts ABSOLUTE paths
// (file.rs: "The current-directory concept isn't part of v1"), and
// territory mount/bind take absolute mount points -- there is no
// getcwd/chdir. The only well-defined working-directory anchor is the
// Proc's territory root, which is always "/". So pwd prints "/".
//
// When a real cwd lands (a per-Proc dot, Plan 9-style), pwd reads it here.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

aux_rt::main!(run);

fn run(_args: aux_rt::Args) -> i64 {
    aux_rt::out(b"/\n");
    0
}
