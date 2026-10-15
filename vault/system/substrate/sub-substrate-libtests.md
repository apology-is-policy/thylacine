---
id: sub-substrate-libtests
type: sub
parent: moc-substrate
title: "The library tests — upstream's own test lists, translated for the device and run by muster"
code:
  - usr/muster/src/main.rs
  - usr/muster/src/tap.rs
  - tools/libtests-manifest.py
  - tools/libtests-webp.json
  - usr/ports/libpng/patches/0001-tests-link-the-static-library.patch
  - usr/ports/libxml2/patches/0001-tests-restore-the-cmake-test-scripts.patch
audit: none
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
abis: []
design: ["docs/BROWSER-DESIGN.md"]
created: 2026-10-15
updated: 2026-10-15
---
## Purpose

Run each browser library's compiled upstream tests on the device, with their
data, as the B-3 exit bar asks (`dec-2026-10-14-b3-test-bar`), without writing
a second copy of upstream's test list. A CMake library registers its tests
with CTest; the build dumps that registration, `tools/libtests-manifest.py`
rewrites it for the device, and `muster`, a native program, runs it there.
ICU is not here: its test programs carry their own drivers (B-3c, run
directly by `tools/interactive/ls-libs.exp`).

## Contract

- `tools/libtests-manifest.py --json <ctest dump> --lib <name> --map
  <host>=<device>... --logs <dir> --out <manifest> --stage <list>` writes the
  manifest and the list of files the device needs (`exe` for each program,
  stripped when staged; `file` for each input a test names that exists
  before any test runs). It FAILS the build rather than write a list that runs
  less than upstream's: a test with no command (its program is not built), a
  property other than `WORKING_DIRECTORY` and `DEPENDS` (and
  `PASS_REGULAR_EXPRESSION` in the digest shape alone), a `cmake` command of
  an unknown shape, a path outside every map, an argument holding a newline,
  or a relative argument that resolves elsewhere on the device than on the
  host.
- `muster <manifest>` runs the tests in file order and exits 0 iff every test
  passed (a test passes when each of its steps does, in order); its last
  line is `muster: <lib>: <n> of <n> passed`, or
  `muster: <lib>: <k> of <n> FAILED` with status 1. A failing test prints its
  name, the step that failed and why, and the last 1536 bytes of that step's
  stdout and stderr. All of it
  goes to descriptor 1, so a shell redirect captures the run; libthyla-rs's
  `t_putstr` would not do: it is `SYS_PUTS`, which writes the console.
- A manifest whose `count` line disagrees with the tests it holds is refused
  (exit 2), and ls-libs compares the verdict's count with the count the build
  recorded (`build/pouch/lib-tests/<lib>.count`), so a run of fewer tests
  than the build listed is red at both ends.

## Mechanism

**The manifest.** One keyword per line and the rest of the line its value:
`lib`, `count`, `logs` once; then per test `test <name>`, `cwd <dir>`, one
or more steps, and `end`. A step is `step`, then optionally `builtin <kind>`
(`copy`, `cmp`, `sha256`, `mkdir`, `rmtree`), one `arg` per argv element or
operand (for a program the first is the program), and for a program
optionally `stdin`, `stdout`, `stderr` (paths) and `status any`. A line
format, so the native parser is a split on the first space and no quoting
exists to get wrong; a builtin with the wrong number of operands, or with a
redirect, is refused at parse time.

**The translation.** CTest's `--show-only=json-v1` gives each test's argv,
working directory and dependencies. Host paths are rewritten through the
maps, longest host prefix first; the build tree and the source tree map side
by side (`<lib>/build`, `<lib>/src`) so a path a test names relative to its
directory resolves the same on both. The tests are ordered so each follows
the tests it `DEPENDS` on, declaration order otherwise (CTest's own serial
order). Six `cmake` command shapes are understood and no other:
`-E copy_if_different A B` (libjpeg-turbo's tjbench setup) becomes the
runner's `builtin copy`; `-E compare_files A B` becomes `builtin cmp`;
`-E sha256sum F` under a `PASS_REGULAR_EXPRESSION` of exactly
`^<64 lowercase hex>  ` (CMake prints `<digest>  <file>`) becomes
`builtin sha256 <digest> F`, muster hashing with RustCrypto's `sha2`; and
libpng's wrapper, `-DLIBPNG=.. -DTEST_COMMAND=<exe> -P tests/<name>.cmake`,
is unwrapped to `<exe>` with the script's `TEST_OPTIONS` and `TEST_FILES`.
The wrapper script is checked against libpng's `scripts/cmake/test.cmake.in`
with those two values removed, so a libpng that changes how a test runs is
refused rather than mistranslated. The last two are libxml2's scripts,
checked by their sha256 for the same reason, and translated into the steps
they perform: `run_and_diff.cmake` runs `PROG --shell XML` in `WORKDIR` with
its stdin from `STDIN` (both relative to `WORKDIR`, where CMake's
`execute_process` resolves them -- measured on the host build, whose test
directory holds no `test/`), its outputs to `STDOUT` and `STDERR` and its
status only reported, so the step is `status any`; then stdout against
`EXPECTED_STDOUT`, and stderr against `EXPECTED_STDERR` when that file
exists. `test_xmlcatalog_add_del.cmake` empties and makes its work directory
(script mode's `CMAKE_CURRENT_BINARY_DIR` is the directory cmake runs in),
then `xmlcatalog --create`, three `--add`, a compare, three `--del`, a
compare, each command required to succeed.

**The runner.** `muster` is native (libthyla-rs): it is authored here and only
spawns the ported test programs. libthyla-rs's `Command` has no working
directory of its own, so muster sets its own (`set_current_dir`, which a
spawned child inherits) before each test, builtins included. A step's stdin
is an empty file unless it names one, so a test that reads it sees
end-of-file instead of the console; its stdout and stderr go to
`<logs>/<test>.<k>.out` and `.err` unless it redirects them, and a redirect's
parent directories are made first, as libxml2's wrapper makes them. The
first failing step ends the test, as a CMake script's first fatal error
does.

**The libraries (B-3d).** libpng and libjpeg-turbo use their CTest lists.
libpng registers its tests only when it builds the shared library, which the
static-only Thylacine platform cannot (CMake 4 refuses `add_library(SHARED)`
there: CMP0164, which libpng's policy range sets NEW), so the port's patch
0001 links them against the static library instead. libwebp registers no
tests; `tools/libtests-webp.json` holds its 14 checks in CTest's shape, so
they go through the same translation. Upstream's documented verification
(`doc/tools.md`: `dwebp test.webp -ppm` equal byte for byte to
`test_ref.ppm`) has not held since 0.5.0 changed the YUV-to-RGB rounding
(its NEWS says so): 1.6.0's decode differs from `test_ref.ppm` by exactly 1
in 13265 of 49152 samples, on a native host build, through the C path and
NEON alike. So the lossy decode, the lossy encode at `-q 75` and its decode
each check a pinned SHA-256: libwebp 1.6.0's own output built natively on an
arm64 host, where `-noasm` (C) and NEON agree byte for byte. A new pin
recomputes the three the same way; a stale digest is red, never skipped. The
lossless round trip is exact against `test_ref.ppm`, and a PNG written and
read back through libpng (dwebp's default format) is exact against it too.

**libxml2 (B-3e).** 2.15.4's whole CTest list, 23 tests: the nine compiled
tests (runtest, runsuite, runxmlconf, testapi, testcatalog, testchar,
testdict, testparser, testrecurse -- runxmlconf registered because the W3C
XML conformance suite is unpacked as `xmlconf/`, as upstream's CI image
installs it), the seven xmllint shell scripts and six xmlcatalog scripts
under `run_and_diff.cmake`, and the catalog add/delete test. The release
tarball ships neither CMake script, so all fourteen script tests fail on any
host as packaged; the port's patch 0001 restores both from the tag. Before a
boot the translated list ran 23 of 23 on the host build against a pruned
copy holding only what the device stages: the stage list's 54 files and the
`test/`, `result/` and `xmlconf/` trees.

## Data structures

The manifest (above) and the stage list: `<kind> <host path> <device path>`
per line, read by `build.sh`'s `lib_tests_muster`.

## Concurrency

None: muster runs one test at a time, as CTest does by default.

## Invariants enforced

- The device runs upstream's list, not a copy: every test comes from the
  library's own CTest registration (or, for libwebp, the one JSON file),
  and a test the translator cannot carry stops the build.
- A pass is an exit status of 0 from every test, and the gate checks the
  count as well as the verdict.

## Error paths

A test that cannot be spawned, a working directory that cannot be entered,
a builtin's unreadable file: that test fails with the reason, and the run
continues. A malformed or miscounted manifest: exit 2 before any test runs.

## Performance

The `ls-libs` gate's five legs (ICU's two and the three codec lists) take
262 s with the boot, under HVF on the Mac (2026-10-15); B-3c's two ICU legs
alone took 118 s with the boot. The legs are not timed one by one.

## Prosecution

- Does a test that passes on the host but has no witness on the device slip
  through? Only by not being in CTest's list, which is upstream's choice.
- Is a relative input staged at the place the device resolves it? The
  translator checks each against the device working directory and refuses a
  mismatch.

## Seams

- libthyla-rs `Command` has no `cwd` or environment; a library whose tests
  set `ENVIRONMENT` is refused until the runner can carry it.

## Caveats

- A test program that reads a file it does not name on its command line
  gets only what `build.sh` stages beyond the translator's list.
- libwebp's lossy digests are the reference implementation's output, not an
  independent oracle: they prove the Pouch build decodes and encodes exactly
  as libwebp 1.6.0 does natively, and upstream's own reference image agrees
  with that decode to within 1 per sample.

## Provenance

B-3d (2026-10-15), main session.
