---
id: dec-2026-10-27-kernel-declarations
type: dec
title: "Every cross-file kernel name is declared once, in a header the compiler checks; the kernel builds with -Werror"
date: 2026-10-27
status: standing
decided-by: user-vote
affects: [sub-kernel-syscall-dispatch, sub-kernel-proc, sub-kernel-notes, sub-kernel-cons, sub-kernel-vivarium]
created: 2026-10-27
---
## Fork

How are kernel functions that the in-kernel tests reach from another file
declared? The code's convention, stated in comments and in no scripture, was
"absent from the header, extern-declared by the harness": a test file wrote
its own `extern` for a syscall core or a test hook. C never compares such a
declaration with the definition. `test_proc_identity.c` declared
`sys_spawn_full_argv_identity_for_proc` with 17 parameters against its
16-parameter definition from V-1b (3434d6c0a, 2026-07-30) until a signature
census found it on 2026-10-26: undefined behaviour, harmless only because
every call passed 0 in the extra argument. A second question rode with it:
once the build is clean, do warnings fail it?

## Research

- Plan 9: every cross-file kernel function is declared once, in
  `port/portfns.h` or the architecture's `fns.h`, which every kernel file
  includes. No local externs.
- Linux: `-Wmissing-prototypes` turned on for every build (commit
  0fcb70851, aimed at v6.8) because it "catches real bugs when there are
  mismatched prototypes"; checkpatch warns on externs in .c files; KUnit's
  `VISIBLE_IF_KUNIT` (include/kunit/visibility.h) is `static` when KUnit is
  off, so a function opened to tests stays file-local in production;
  `CONFIG_WERROR` makes warnings fatal.
- The tree at 9c6b860c9, measured over all 236 kernel translation units with
  the build's own flags: 464 warnings in the tests build (443 missing
  prototypes) and 96 in production; 218 local `extern` function declarations
  across 53 .c files naming 122 functions; 101 test hooks compiled into the
  production kernel; `test.c` forward-declares every registered test in a
  file that defines none of them. One signature mismatch (above); no object
  mismatch.

## Options

- **A. Headers, static in production.** A function called from another file
  only by the tests is defined with `VISIBLE_IF_KERNEL_TESTS` (`static`
  without `KERNEL_TESTS`) and declared in its module's header under
  `#ifdef KERNEL_TESTS`; tests include the header. Every call is
  compiler-checked; production loses the global symbols nothing outside
  their file calls.
- **B. Headers, always visible.** The same declarations, unconditional; the
  functions stay global. Simpler, but the `*_for_proc` cores, which skip the
  caller's current-thread lookup, become callable from any kernel file.
- **C. Keep the convention; `tools/check-decls.py` as a gate leg.** A
  mismatch is caught at gate time rather than compile time, and production
  keeps its missing-prototype warnings.

For warnings: -Werror on both kernel builds; a gate-time ratchet only; or
advisory.

## The call

A (operator vote 19, 2026-10-27), and -Werror on both kernel builds,
`KERNEL_TESTS` on and off (vote 20, same batch). The rule is written into
`.claude/rules/kernel-code.md`, "Cross-file declarations".

## Rationale

A declaration the compiler checks cannot drift; one a tool checks at gate time
drifts until the gate runs, and one nothing checks drifted for three months.
Keeping test-opened functions `static` in production keeps the kernel's
cross-file surface what production actually uses. -Werror makes the zero
hold: a warning brought in by a merge or a toolchain upgrade stops the build
where it appears. `tools/check-decls.py` stays as the backstop for the one
thing a compiler cannot see, a name declared with two types in two
translation units.
