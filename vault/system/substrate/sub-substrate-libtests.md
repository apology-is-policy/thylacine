---
id: sub-substrate-libtests
type: sub
parent: moc-substrate
title: "The library tests — upstream's own test lists, translated for the device and run by muster"
code:
  - usr/muster/src/main.rs
  - tools/libtests-manifest.py
  - tools/libtests-webp.json
  - usr/ports/libpng/patches/0001-tests-link-the-static-library.patch
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
  property other than `WORKING_DIRECTORY` and `DEPENDS`, a `cmake` command of
  an unknown shape, a path outside every map, an argument holding a newline,
  or a relative argument that resolves elsewhere on the device than on the
  host.
- `muster <manifest>` runs the tests in file order and exits 0 iff every test
  exited 0; its last line is `muster: <lib>: <n> of <n> passed`, or
  `muster: <lib>: <k> of <n> FAILED` with status 1. A failing test prints its
  name, its status and the last 1536 bytes of each of its two logs.
- A manifest whose `count` line disagrees with the tests it holds is refused
  (exit 2), and ls-libs compares the verdict's count with the count the build
  recorded (`build/pouch/lib-tests/<lib>.count`), so a run of fewer tests
  than the build listed is red at both ends.

## Mechanism

**The manifest.** One keyword per line and the rest of the line its value:
`lib`, `count`, `logs` once; then per test `test <name>`, `cwd <dir>`, one
`arg` per argv element (the first is the program), optionally `builtin copy`
or `builtin cmp`, and `end`. A line format, so the native parser is a split
on the first space and no quoting exists to get wrong.

**The translation.** CTest's `--show-only=json-v1` gives each test's argv,
working directory and dependencies. Host paths are rewritten through the
maps, longest host prefix first; the build tree and the source tree map side
by side (`<lib>/build`, `<lib>/src`) so a path a test names relative to its
directory resolves the same on both. The tests are ordered so each follows
the tests it `DEPENDS` on, declaration order otherwise (CTest's own serial
order). Three `cmake` command shapes are understood and no other:
`-E copy_if_different A B` (libjpeg-turbo's tjbench setup) becomes the
runner's `builtin copy`; `-E compare_files A B` becomes `builtin cmp`; and
libpng's wrapper, `-DLIBPNG=.. -DTEST_COMMAND=<exe> -P tests/<name>.cmake`,
is unwrapped to `<exe>` with the script's `TEST_OPTIONS` and `TEST_FILES`.
The wrapper script is checked against libpng's `scripts/cmake/test.cmake.in`
with those two values removed, so a libpng that changes how a test runs is
refused rather than mistranslated.

**The runner.** `muster` is native (libthyla-rs): it is authored here and only
spawns the ported test programs. libthyla-rs's `Command` has no working
directory of its own, so muster sets its own (`set_current_dir`, which a
spawned child inherits) before each test, builtins included. Each test's
stdin is an empty file, so a test that reads it sees end-of-file instead of
the console; its stdout and stderr go to `<logs>/<test>.out` and `.err`.

**The libraries (B-3d).** libpng and libjpeg-turbo use their CTest lists.
libpng registers its tests only when it builds the shared library, which the
static-only Thylacine platform cannot (CMake 4 refuses `add_library(SHARED)`
there: CMP0164, which libpng's policy range sets NEW), so the port's patch
0001 links them against the static library instead. libwebp registers no
tests; `tools/libtests-webp.json` holds its checks in CTest's shape, so they
go through the same translation: upstream's documented verification (`dwebp
test.webp -ppm` equal byte for byte to `test_ref.ppm`, `doc/tools.md`), and
round trips of `test_ref.ppm` through the lossless encoder, through a PNG
written and read back, and through the lossy encoder.

## Data structures

The manifest (above) and the stage list: `<kind> <host path> <device path>`
per line, read by `build.sh`'s `lib_tests_codecs`.

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

Not measured yet.

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
- libwebp's lossy round trip checks that the bitstream is valid and decodes,
  not its quality: `get_disto` reports a distortion but judges none.

## Provenance

B-3d (2026-10-15), main session.
