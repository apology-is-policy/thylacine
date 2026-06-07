// basename PATH [SUFFIX] -- strip directory and (optional) suffix.
//
// Exercises libthyla-rs::fs::Path::file_name(). That method returns None for
// "/", ".", and ".." (DOC-GAP G08: it diverges from POSIX basename, which
// yields "/", ".", ".." respectively). We recover the POSIX answer for the
// None case here.

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
            aux_rt::eprintln!("basename: missing operand");
            return 1;
        }
    };

    // Path::file_name covers the common case; posix_base recovers "/", ".",
    // ".." which file_name reports as None.
    let base: &str = Path::new(path).file_name().unwrap_or_else(|| posix_base(path));

    let out = match args.get_str(2) {
        Some(suf) if !suf.is_empty() && base != suf && base.ends_with(suf) => {
            &base[..base.len() - suf.len()]
        }
        _ => base,
    };

    aux_rt::out(out.as_bytes());
    aux_rt::out(b"\n");
    0
}

fn posix_base(path: &str) -> &str {
    if path.is_empty() {
        return ""; // POSIX: basename '' -> empty
    }
    let trimmed = path.trim_end_matches('/');
    if trimmed.is_empty() {
        return "/"; // path was "/", "//", ...
    }
    match trimmed.rfind('/') {
        Some(i) => &trimmed[i + 1..],
        None => trimmed, // ".", "..", or a bare name
    }
}
