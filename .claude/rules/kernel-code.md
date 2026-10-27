---
paths:
  - "kernel/**"
  - "arch/**"
  - "mm/**"
---

# Kernel code patterns

> Moved from `CLAUDE.md` on 2026-09-23. Loads when kernel/arch/mm files are read.

### Idempotency on retry

Any function that writes durable state MUST short-circuit on clean state. If the function's contract is "on success, durable state X is recorded," then calling it twice with the same inputs and no intervening mutations must produce byte-identical durable state.

**Pattern**: carry a dirty flag. Mutations set dirty. Commits check dirty; if clean AND a durable result already exists, return cached result. If dirty, do the work + clear dirty.

### Compile-time invariants

Every on-disk, on-wire, or ABI-exposed format gets:
- `_Static_assert` (C/C++) on struct size, alignment, and discriminant ranges.
- Explicit version constants.
- Compat / ro-compat / incompat feature-flag tiers (where applicable).

Catches format drift at build time, not at runtime.

For Thylacine specifically:
- ELF loader: `_Static_assert` on ARM64 e_machine, ABI version.
- 9P wire format: `_Static_assert` on message header sizes, fid widths, tag widths.
- Handle table layout: `_Static_assert` on `struct Handle` size + alignment.
- Page table entry bit layout: `_Static_assert` on PTE bit positions (W^X invariant).
- DTB parse: `_Static_assert` on FDT magic, version expectations.

### Crash-injection + fault-injection testing

For torn-write-sensitive paths (Stratum mount transition, persistent state machines, multi-phase commits), wire fault-injection hooks at every durable write. Test that recovery from each injection point produces a valid state. Same pattern applies to interrupt injection in schedulers, fault injection in fault-tolerant networking, and partial-failure injection in distributed systems.

For Thylacine: kernel panic during ramfs → Stratum transition; driver process kill mid-IO; 9P session drop mid-walk.

### Cross-file declarations (operator votes 19 + 20, 2026-10-27)

C compares a declaration with its definition only inside one translation unit. A declaration written in some other `.c` file is never compared with the definition, and a call through one of the wrong type is undefined behaviour that no diagnostic sees: a test declared a 16-parameter syscall core with 17 parameters from 2026-07-30 to 2026-10-26, and every build passed. Plan 9 declares each cross-file kernel function once, in `port/portfns.h` or the architecture's `fns.h`; Thylacine does the same.

- **One header per cross-file name.** A function or object used outside its defining file is declared in exactly one header, and the defining file includes that header. No `.c` file declares a function or object it does not define, whether with `extern` or a bare prototype. The only exception is a symbol called solely by compiler-emitted code or assembly (`__stack_chk_fail`), which is declared beside its definition.
- **Test reach.** A function defined in production code and called from outside its file only by the in-kernel tests -- a `*_for_proc` syscall core, a test hook -- is defined with `VISIBLE_IF_KERNEL_TESTS`, which is `static` in a production build and external in a `KERNEL_TESTS` build (Linux KUnit's `VISIBLE_IF_KUNIT`). Its declaration sits in the module's header inside `#ifdef KERNEL_TESTS`, and a test reaches it by including that header. A function with no production caller at all is compiled only under `KERNEL_TESTS`. `VISIBLE_IF_KERNEL_TESTS` is the one sanctioned storage-class macro; the style rule against `#define` magic does not reach it.
- **Registered tests** are declared in the shared test header that `test.c`'s registry and every test file include, so each test's definition is checked against the declaration the registry calls through.
- **No warnings, enforced (vote 20).** Both kernel builds, with `KERNEL_TESTS` on and off, compile with `-Werror`. A warning brought in by a merge or a toolchain upgrade fails the build where it appears; fix it there, never by turning the warning off for the file. `tools/check-decls.py` is the backstop for what the compiler cannot see across translation units: one name, one type, across every kernel TU.

---
