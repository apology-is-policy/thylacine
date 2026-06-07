// dirname PATH -- strip the last component.
//
// Exercises libthyla-rs::fs::Path::parent(). Its result is mapped to the
// POSIX dirname answer (DOC-GAP G08): parent() returns Some("") for a bare
// relative name (POSIX dirname -> "."), and None for "/" and "" (POSIX
// dirname -> "/" and "." respectively).

#![no_std]
#![no_main]

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::fs::Path;

aux_rt::main!(run);

fn run(args: aux_rt::Args) -> i64 {
    let path = match args.get_str(1) {
        Some(p) => p,
        None => {
            aux_rt::eprintln!("dirname: missing operand");
            return 1;
        }
    };

    let out: &str = match Path::new(path).parent() {
        Some(p) if p.as_str().is_empty() => ".", // bare relative name -> "."
        Some(p) => p.as_str(),
        None => {
            // parent() == None for "/" (and all-slashes) and for "".
            if !path.is_empty() && path.trim_end_matches('/').is_empty() {
                "/"
            } else {
                "."
            }
        }
    };

    aux_rt::out(out.as_bytes());
    aux_rt::out(b"\n");
    0
}
