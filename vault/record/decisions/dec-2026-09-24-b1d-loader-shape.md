---
id: dec-2026-09-24-b1d-loader-shape
type: dec
title: "B-1d: PIE where needed, the file map's address, the native interpreter, and fdlopen"
date: 2026-09-24
status: standing
decided-by: user-vote
affects: [inv-i12, inv-i36, inv-i28, sub-kernel-exec, sub-kernel-elf, sub-kernel-syscall-dispatch, sub-pouch-mem]
created: 2026-09-24
---
## Fork

B-1d builds `dlopen` as the B-1 vote set it
([[dec-2026-09-23-memory-surface-and-loader]], rows 5 and 6): `libc.so` is the
loader, D-4's PT_INTERP rewrite serves native execs, and `burrow_map_file`
exposes D-3's file-map arms natively. Reading the ratified text against the
tree left four questions it did not settle. All four were put to the operator
by blocking question on 2026-09-24 (Opus 5.5, under the away grant's Opus
clause): how Pouch binaries use PIE, which ARCH 6.5 left "decided at B-1d";
how `burrow_map_file` carries the address its FIXED arm needs, since the
ratified row has none; which interpreter path native dynamic binaries carry;
and the shape of the handle form, designed now and built at B-6.

## Research

- **Direct mode places the program.** D-4 execs the interpreter with the
  program's path in argv, and musl's `map_library` maps the program itself
  (`third_party/musl/ldso/dynlink.c`). The whole span goes at `addr_min` as a
  hint, without MAP_FIXED (:809); an `ET_EXEC` that does not land exactly
  there fails with EBUSY (:816). The kernel ignores a hint (DISTRO D-3), so
  in direct mode only an `ET_DYN` program can load.
- **The FIXED arm needs an address.** Each later segment is mapped MAP_FIXED
  at `base + vaddr` (:842), and the bss tail anonymously at a fixed address
  (:848). The ratified `burrow_map_file(fd, offset, length, prot, flags)`
  has no address argument. The three phenotype cores it exposes all exist
  (`kernel/syscall.c:6232`, `:6484`, `:6579`). `-z separate-loadable-segments`
  gives our libraries four segments, so they are the first producer of the
  FIXED R / R+X overlay, which D-3b built and unit-tested without a gate.
- **PIE today.** The fork driver pushes `-static` and silently drops
  `-static-pie` (`Thylacine.cpp`; `Thylacine.h:79-83` isPIC/isPIE false;
  DISTRO 5). musl is configured `--disable-shared` (`tools/build.sh:2357`),
  so `libc.a` is non-PIC. Every ET_DYN loads at one fixed base,
  `ELF_PIE_LOAD_BIAS` 512 MiB (`kernel/include/thylacine/elf.h:143`), so PIE
  buys no ASLR yet. The aux's Rust target asks for static-PIE
  (`usr/ports/rust/aarch64-unknown-thylacine.json`) and gets ET_EXEC only
  because the driver drops the flag.
- **The v8.0 floor, measured after the vote.** The question called this
  measured; when it was put, it was reasoning. Measured since: musl's
  non-PIC (`.o`) and PIC (`.lo`) compiles of eight sources emit the same
  instruction set; on a C11 atomic, PIC adds only `ldr` (the GOT load), and
  the control, the same file at `-march=armv8.1-a`, adds `ldaddal`, so the
  comparison can see an added instruction. The shipped `libc.so` still
  passes through `tools/check-v80-floor.py` like every binary.
- **The Image cache.** lld refuses a text relocation in a PIE link (the
  measured `R_AARCH64_ABS64` failure, DISTRO 5), so PIE text is
  position-independent and the Image cache shares it at any base.
- **The device has no `/lib`.** The ramfs is flat and served as `/bin`; ARCH
  9.6's tree has no `/lib`, and `/` is the Stratum root after the pivot.
- **The handle form's peers.** FreeBSD's `fdlopen(int fd, int mode)` loads
  through a descriptor, for race-freedom and for Capsicum (`fdlopen(3)`,
  read 2026-09-24). Its rtld resolves dependencies under
  `LD_LIBRARY_PATH_FDS`, "a colon separated list of file descriptor numbers
  for library directories ... for use within capsicum(4) sandboxes"
  (`rtld(1)`, read 2026-09-24). Fuchsia's loader service hands libraries out
  as VMOs; Genode's ldso takes ROM dataspaces (ARCH 6.5).
- **No identity-keyed authority.** Exec grants nothing by which image it
  loads; capabilities come at spawn. So direct mode's re-open of the program
  by name is not a confused deputy.

## Options

PIE: only where needed (static stays ET_EXEC; `-pie` a dynamic PIE;
`-shared` a `.so`; `-static-pie` refused) / everywhere (static-PIE default,
PIC `libc.a`, libc++ and builtins, every Pouch binary recompiled) / nowhere
(`burrow_map_file` honours an ET_EXEC's address hint). The address: a sixth
argument, read only under the FIXED flag / two calls, `burrow_map_file`
placing a span and `burrow_overlay` replacing part of one (the ATTACH /
ATTACH_LAZY precedent). The interpreter: `/lib/libc.so` / musl's canonical
`/lib/ld-musl-aarch64.so.1`, the Linux phenotype's string. The handle form:
`fdlopen` plus endowed directory handles / plus a loader service / with the
caller loading dependencies in order.

## The call

The operator, by blocking question, 2026-09-24 -- four votes, each on the
recommended option:

1. **PIE only where needed.** A static program stays a non-PIE `ET_EXEC`,
   byte-identical. `-pie` links a dynamic PIE against `libc.so` (`Scrt1.o`,
   PT_INTERP) and `-shared` a `.so`. `-static-pie` becomes an error; the Rust
   target turns static-PIE off. Userspace ASLR stays its own work.
2. **`burrow_map_file(fd, offset, length, prot, flags, addr) -> vaddr`.**
   `addr` is read only under `BURROW_MAP_FIXED`; a nonzero `addr` without it
   is `EINVAL`, never a hint silently ignored. `fd` -1 under the flag is the
   anonymous tail. One entry over D-3's three cores.
3. **The native interpreter is `/lib/libc.so`.** The native namespace gains
   `/lib`, bound from the initrd as `/bin` is. The question's text said
   `dlopen` searches `/lib` "then LD_LIBRARY_PATH", which was wrong twice.
   musl's order is `LD_LIBRARY_PATH`, then the needing object's run path,
   then the system list, whose first entry is `/lib` (`dynlink.c:1119-1163`).
   And the loader ignores `LD_LIBRARY_PATH` and `LD_PRELOAD` whenever it
   judges the process secure, which it judges every Thylacine process to be:
   the kernel's auxv carries none of `AT_UID` / `AT_EUID` / `AT_GID` /
   `AT_EGID` (`dynlink.c:1819-1826`; `kernel/exec.c:596-612`). So a bare name
   is found in the run path or in `/lib`. That was already the tree's
   behaviour, for the Linux phenotype too; nobody chose it, and the decision
   it needs is enqueued (OPEN-BUGS). It fails safe: an injected `LD_PRELOAD`
   cannot ride into a Proc that later elevates.
4. **The handle form is `fdlopen(fd, mode)` plus directory handles endowed
   at spawn**, FreeBSD's pair. A library loaded by handle resolves its
   `DT_NEEDED` names only under those directories. Built at B-6.

## Rationale

Each vote takes the smallest change that serves the ratified design. PIE is
required exactly where the loader places code -- a `.so` and a program run
through the interpreter -- and nowhere else yet, because the thing PIE buys
everywhere, ASLR, does not exist until exec randomises the base; making every
static binary PIE now would recompile the whole Pouch world for a property the
kernel cannot yet deliver. The address argument completes the ratified row
rather than adding a call: the FIXED arm was voted, and it cannot be expressed
without one. A distinct interpreter string keeps the phenotype decision where
VIVARIUM put it, in the program's location, instead of letting a namespace's
`/lib` pick a loader of the wrong ABI. And the handle form follows the one
peer that already solved dependency resolution for a sandboxed process with
descriptors alone; directory handles are Spoors, so the endowment is the
namespace idiom carried by handle. The scripture: ARCH 6.5 "Dynamic loading"
and its syscall row; DISTRO D-3, D-4 and 5; POUCH-DESIGN 2.2 and 9;
LLVM-DESIGN F3; browser-status "The B-1 decisions" row 14.
