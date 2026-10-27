---
id: sub-kernel-declarations
type: sub
title: "Cross-file declarations -- one checked declaration per shared name, test reach marked, warnings fatal"
parent: moc-kernel-boot
code:
  - kernel/include/thylacine/visibility.h
  - kernel/test/test_registry.h
  - arch/arm64/sections.h
audit: light
guarded-by: []
validated-by: [prose]
locks: []
hazards: []
abis: []
design: [".claude/rules/kernel-code.md Cross-file declarations"]
created: 2026-10-27
updated: 2026-10-27
---
## Purpose

How every name the kernel shares between files is checked by the compiler:
one declaration, in the header its definer includes; test reach marked rather
than smuggled; warnings fatal; and a checker at every build that holds the
rest. The rules are operator votes 19 and 20 ([[dec-2026-10-27-kernel-declarations]])
and vote 23 (file-local functions), written down in
`.claude/rules/kernel-code.md` "Cross-file declarations".

## Contract

- **One declaration per cross-file name.** A function or object used outside
  its file is declared in exactly one header, and the defining `.c` includes
  that header, so the compiler compares the declaration with the definition in
  that TU. No `.c` declares a non-static name it does not define -- no local
  `extern`, at file or block scope.
- **Test reach** (vote 19, Linux KUnit's `VISIBLE_IF_KUNIT`). A production
  function or object that only the in-kernel tests use from another file is
  `VISIBLE_IF_KERNEL_TESTS` -- `thylacine/visibility.h`: empty under
  `KERNEL_TESTS`, `static` otherwise -- and its declaration sits in the module
  header under `#ifdef KERNEL_TESTS`. One with no production caller at all is
  compiled only under `KERNEL_TESTS`. A production caller above the definition
  gets an in-file forward declaration carrying the same marker.
- **File-local is static; uncalled is deleted** (vote 23).
- **The tests' own names.** `kernel/test/test_registry.h` declares every
  registered test once; `test.c`'s table and each test file see the same
  prototype.
- **Linker and asm symbols** that C reads (`_kernel_start`, the section
  bounds, the vector base) are declared in `arch/arm64/sections.h`.
- **Warnings are fatal** on both kernel builds (vote 20, `-Werror` in
  `cmake/Toolchain-aarch64-thylacine.cmake`), and so on the fault-test and
  sanitizer kernels built from the same flag list. A warning is fixed where it
  appears, never silenced for the file.
- **`tools/check-decls.py`** runs in every `tools/build.sh` kernel build, after
  the link, fatal, with no skip switch, and first runs its own `--self-test`
  (every case witnessed red before its rule was trusted). It reads the build
  directory's compile commands and the clang AST of every kernel TU --
  `kernel/`, `arch/`, `mm/`, `lib/` and the generated `halls_symtab.c`:
  - (a) one name, one type, across all TUs (`_Bool` and `bool` equal; an
    unsized array matches a sized one);
  - (b) no `.c` declares a non-static function or object it does not define,
    at file or block scope;
  - (c) one definition per struct tag;
  - (d) every non-static function or object a TU of the configuration can see
    is defined in one of its TUs, in an assembly source (`.global`/`.globl`)
    or in the linker script. On a production build this is what holds a test
    hook's prototype inside `#ifdef KERNEL_TESTS`: one left outside compiles
    for a production caller and fails only at the link.
- **Host fixtures take the tests build's shape.** A host checker that compiles
  kernel source (`tools/host-sanitizer-cc.py`) adds `-DKERNEL_TESTS
  -DVISIBLE_IF_KERNEL_TESTS=`: a fixture reaches what the in-kernel tests
  reach.

## Mechanism

C compares a declaration with its definition only inside one TU. A local
`extern` in another file is never checked against anything. That is how
`test_proc_identity.c` declared `sys_spawn_full_argv_identity_for_proc` with
17 parameters against the definition's 16 for three months (V-1b, 2026-07-30,
found 2026-10-26) -- undefined behaviour, harmless only because every call
passed 0. The census that found it also found 101 test hooks compiled into
production and 218 local `extern` declarations.

`check-decls.py` builds its view from the compiler, not from text: each TU's
clang JSON AST, with every declaration placed by its NAME's location (a
declaration whose range begins in a macro, such as `bool`, would otherwise be
attributed to the macro's header), implicit builtin declarations skipped, and
array bounds stripped before types are compared. Rule (b) compares a
declaration's file with the definition's. Rule (d) unions every TU's
definitions with the globals of the `.S` files `build.make` lists and the
symbols the `-T` linker script assigns. `tools/build.sh` runs it after
`regen-halls-symtab.sh`, so the generated symbol table is checked too.

## Data structures

None at run time. `VISIBLE_IF_KERNEL_TESTS` is the one sanctioned
storage-class macro.

## Concurrency

None: a build-time discipline.

## Invariants enforced

None of ARCHITECTURE section 28 directly. It keeps the cross-file calls that
carry them type-checked: a mismatched declaration is undefined behaviour at
every call through it.

## Error paths

`check-decls.py` exits 0 when every rule holds, non-zero on a violation or on a
build directory it cannot read; `tools/build.sh` stops the build either way,
and stops before checking anything if the self-test fails.

## Performance

About 7 s per production kernel build and 17 s per tests build (one AST dump per
TU, in parallel).

## Prosecution

Every rule was witnessed red on a seeded case before it was trusted, and the
seeds are the 21 `--self-test` cases. Rule (a) is red on a parameter mismatch,
an object type mismatch and a block-scope extern of the wrong type; rule (b) on
a local extern, a bare prototype, block-scope externs of a function and of an
object, and a declaration whose range begins in a macro (`bool`); rule (c) on a
second definition of a struct tag; rule (d) on a header prototype that no file
defines, alone and beside a used one. Rule (d) stays green when the same
name comes from the assembly or the linker script. The controls stay green where they must: a
typedef'd equal type and an unsized array against a sized one pass rule (a)
(rule (b) still refuses both declarations); a header declaration, a file's own
prototype, block-scope locals, compiler builtins and same-named statics in two
files pass every rule; a struct defined once in a header, or locally in a
function, passes rule (c).

## Seams

## Caveats

- **Redundant declarations are not machine-checked.** clang ignores
  `-Wredundant-decls` (a seeded same-file duplicate compiled silently), and no
  check-decls rule covers it; the 2177 redundant lines were removed by an AST
  census, once.
- **A census of uses must come from the compiler.** A name that appears only in
  a string is not a use (`vma_remove`'s one mention was its own extinction
  text); `-Wunused-function` on the production build is the measure.
- **The build variants see different code.** The fault-test and extinction
  variants, `VIV_TRACE`, `LOOM_PRIVATE_QUALIFICATION` and
  `THYLACINE_NO_TICKLESS` compile code the two main configurations do not; a
  census that ignores them would have moved 21 functions wrongly. A
  check-decls run covers only the configuration its build directory holds.
- **Nothing builds the production kernel routinely.** `tools/build.sh`
  builds the tests kernel unless asked for `--production`, and
  `tools/check-production.sh` is the gate that builds the other. Xa guarded
  `proc_test_console_renderer`'s definition with `KERNEL_TESTS` while
  `spawn_perm_grant_check` called it from production, and kept its prototype
  visible. Every syntax-only compile of both configurations passed. The
  first production link failed. 52 more hooks had the same visible prototype
  and no production caller, and `main.c` included the test registry
  unguarded (2124 declarations). Rule (d) on a production build names all of
  them.

## Provenance
(generated -- incoming `touched` backlinks, newest first; never hand-written)
