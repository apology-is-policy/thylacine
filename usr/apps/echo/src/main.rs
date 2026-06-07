// echo -- write arguments to stdout, space-separated, trailing newline.
//
// First native app to exercise the G03 argv workaround (aux-rt's naked
// rs_main) end to end: without argv, echo is meaningless. Supports the one
// flag every echo has, `-n` (suppress the trailing newline). Bytes are
// passed through verbatim (no backslash-escape interpretation -- that is
// `echo -e`, a bash extension out of scope here).
//
// Output goes to fd 1 (aux_rt::out). See DOC-GAP-REPORT G05: standalone,
// fd 1 may be unwired at v1.0, so the output is visible only in a pipeline /
// redirect / parent-inherited fd.

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use aux_rt::Args;

aux_rt::main!(run);

fn run(args: Args) -> i64 {
    let dash_n: &[u8] = b"-n";
    let mut idx = 1; // argv[0] is the program name
    let mut newline = true;

    if args.get(idx) == Some(dash_n) {
        newline = false;
        idx += 1;
    }

    let mut first = true;
    while let Some(a) = args.get(idx) {
        if !first {
            aux_rt::out(b" ");
        }
        aux_rt::out(a);
        first = false;
        idx += 1;
    }

    if newline {
        aux_rt::out(b"\n");
    }
    0
}
