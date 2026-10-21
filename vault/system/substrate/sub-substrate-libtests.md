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
  - usr/ports/fontconfig/patches/0001-tests-name-the-test-data-relative-to-the-build-root.patch
audit: none
guarded-by: []
validated-by: [prose, gate-interactive]
locks: []
abis: []
design: ["docs/BROWSER-DESIGN.md"]
created: 2026-10-15
updated: 2026-10-21
---
## Purpose

Run each browser library's compiled upstream tests on the device, with their
data, as the B-3 exit bar asks (`dec-2026-10-14-b3-test-bar`), without writing
a second copy of upstream's test list. A CMake library registers its tests
with CTest, and a meson one (FreeType, HarfBuzz, fontconfig) with meson; the build dumps
that registration, `tools/libtests-manifest.py` rewrites it for the device,
and `muster`, a native program, runs it there.
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
- `--meson <dump> --meson-build <dir> --meson-source <dir> [--skip <name>]...
  [--not-run <name>=<reason>]...`
  takes meson's list instead (`meson introspect --tests`, B-3f). `--not-run`
  leaves out a compiled test by an operator decision its reason names, and
  writes `# not run, compiled: <name> -- <reason>` into the manifest; it
  refuses an interpreter-driven test (that takes `--skip`), an empty reason,
  a name given twice, and, like `--skip`, a name the list does not hold. It also
  refuses a key the list holds that the translator does not know, a protocol
  other than `exitcode` and `tap`, `extra_paths`, a directory argument, an
  env path outside every map, a skipped compiled test, an unskipped Python
  one, a source tree that mentions `should_fail` or `MESON_TEST_ITERATION`,
  and an output of a target a test depends on that is not built or that
  nothing stages. The stage list gains `tree <host> <device>`: a directory a
  test's environment names, staged whole.
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
`lib`, `count`, `logs` once; then per test `test <name>`, `cwd <dir>`, any
`env <NAME>=<value>` lines, one or more steps, and `end`. A step is `step`,
then optionally `builtin <kind>` (`copy`, `cmp`, `sha256`, `mkdir`, `rmtree`,
`tap`), one `arg` per argv element or
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

**The meson list (B-3f).** A meson test is its command, run in its
`workdir` or, naming none, in the build tree, where `mtest.py` runs the
harness; with its environment as `env` lines, except the
`LD_LIBRARY_PATH` and `DYLD_LIBRARY_PATH` meson adds for its own build tree,
which are inert for static libraries and refused if they name anything
outside it. A higher priority runs first, registration order otherwise, as
`mtest.py` orders them. A `tap` test is its command with stdout kept in the
log directory, then `builtin tap` over that file. A test's `depends` are the
targets `meson test` builds before it runs, read through the build's
`meson-info/intro-targets.json` as `mtest.py` reads them: each output must
exist and must be staged, by the test's command naming it or a staged tree
holding it. A target only a test depends on is outside `all`, so the build
builds `meson-test-prereq` first ([[sub-substrate-build]]). Without the check
an unbuilt input would pass as an argument like any other, since a relative
path that does not exist reads as an output or an option. Not carried:
timeouts (the gate's own bound covers the run), and the harness's
`MALLOC_PERTURB_` and sanitizer variables, which this libc does not read.
Interpreter-driven tests are skipped by name only: `--skip` takes a test whose
program is a Python script and nothing else, and a Python test not named is
refused, so one upstream adds cannot drop out unseen. A program is a Python
script when its name ends in `.py` or is `python*`, or when its `#!` line
names `python*`, directly or through `env` (fontconfig's `pytest`, an
installed command), past `env`'s own options and assignments (`env -S python3
-u`, `env -u NAME python3`, `env -i LC_ALL=C python3`). A `#!/bin/sh` wrapper
counts as compiled, so a skip of one is refused rather than allowed by guess.
`tools/test-libtests-manifest.py` holds the rule's cases in both directions
(28; the rule before `env`'s options were skipped fails 10 of them), and
`lib_tests_muster` runs it before it uses the translator. Measured on a fake meson list: a
`pytest` with `#!/usr/bin/env python3` and one with `#!/opt/x/bin/python3.14`
are skipped; unskipped, the first is refused as interpreter-driven; the
committed translator before this rule refused its skip as compiled.

**The runner.** `muster` is native (libthyla-rs): it is authored here and only
spawns the ported test programs. libthyla-rs's `Command` has no working
directory of its own, so muster sets its own (`set_current_dir`, which a
spawned child inherits) before each test, builtins included. A step's stdin
is an empty file unless it names one, so a test that reads it sees
end-of-file instead of the console; its stdout and stderr go to
`<logs>/<test>.<k>.out` and `.err` unless it redirects them, and a redirect's
parent directories are made first, as libxml2's wrapper makes them. The
first failing step ends the test, as a CMake script's first fatal error
does. A test's `env` lines are set through `/env/<NAME>`, which each child
inherits, after each variable's old value is recorded, and restored after
the test; a restore that fails fails the test, since the variable would
otherwise reach every later one. `builtin tap` (`usr/muster/src/tap.rs`) is
meson 1.12.1's TAP verdict (`mtest.py`'s `TAPParser` and
`TestRunTAP.parse`): a `not ok`, an `ok` under TODO, a `Bail out!` or any
parse error fails it, as in meson, and two refusals are muster's own, since
a run that reports nothing proved nothing: no plan, and no result but SKIPs
(meson calls that SKIP). A host harness compiled `tap.rs` unchanged and
judged 49 crafted TAP texts beside meson's own parser: no disagreement but
those two, and two sabotaged copies were each caught.

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

**FreeType and HarfBuzz (B-3f).** Both register their tests with meson
alone. FreeType 2.15.0 registers five: issue-1063, varc, outline-reverse,
issue-1469, and issue-1441, which only a build with HarfBuzz registers (the
autofitter classifies a shared GSUB lookup's output glyphs by script through
HarfBuzz), so FreeType builds twice ([[sub-substrate-build]]). varc and
issue-1063 read `FREETYPE_TESTS_DATA_DIR`, the source's `tests/data`, staged
as a tree; issue-1063's font is the one upstream's script downloads
(`[cache.ftfont]`). issue-1469's four fonts and issue-1441's two are made at
build time by Python scripts, issue-1441's by a target outside `all` (it has
no `build_by_default`), which the prerequisite build makes. HarfBuzz 14.6.0
registers 81 tests in a cross build. 76 are
compiled: 25 unit tests (with `hb_assert` live), 48 fuzzer runs over its
checked-in corpora (TAP; a missing input is an `assert(blob)` abort), the two
thread tests and the vector budget test. The other five are Python checks of
its source tree and release notes, skipped by name. HarfBuzz's GLib suites
(`test/api`, `test/shape`, `test/subset`) are skipped under the same vote
(`dec-2026-10-14-b3-test-bar`): `hb-shape` and `hb-subset` are built only with
GLib (`util/meson.build`), so `test/shape` cannot run here. The thread tests
read the fonts and texts their source
names in `perf/` and `test/subset/data/fonts`, which `build.sh` stages beside
the stage list. Built natively from the same pins in the same two passes
(Apple clang; Homebrew libpng 1.6.58 and ICU 78.3), both pass whole under
`meson test`, as they do on the device. With the same three inputs broken, the
native run and the device fail the same three tests with the same messages:
varc ("Could not open file", an emptied `varc-static-gvar.ttf`), issue-1441
("wrong Cyrillic, unreachable, or small-cap style": the mixed font copied over
the Latin one, which opens but classifies wrongly) and
`repacker-fuzzer-chunk-0` (an `assert(blob)` abort, one graph removed).

**fontconfig (B-3g).** fontconfig 2.18.3 registers seventeen compiled tests in
a static cross build (`test_d1f48f11` and `test-issue110` only when static),
plus `pytest` when the build host has pytest; this host has none, and the
build adds `--skip pytest` only when the list holds it. `test_d1f48f11`
is listed as not run (`dec-2026-10-21-d1f48f11-not-run`): its fixture is a
`symlink()`, which Thylacine does not provide. Two tests compile in their
data's path (`test-bz89617`'s `SRCDIR`, `test-bz106632`'s `FONTFILE`); the
port's patch names it relative to the build root
(`fs.relative_to(meson.current_source_dir(), meson.project_build_root())`,
`../src/fontconfig-2.18.3/test` with the pinned meson), so it resolves in
the copied trees on the device as on the host, and `build.sh` stages
`test/4x6.pcf`, which the list cannot name. `test-bz106632` copies the font
with `system("sleep 1; cp -a ...")`, so it needs `/bin/sh`. Several tests
make their fixtures in `mkdtemp` directories under `/tmp`; the ones that load
the default config (`test-bz106618`, `test-crbug1004254`, `test-mt-fccfg`)
read the device's own `/etc/fonts` and `/lib/font`
([[sub-substrate-build]]) and cache into the user's home.
`test-crbug1004254` creates up to 3000 threads, half matching and half
reinitializing, stops at the first refused create, and exits 0 whatever
happens; its errors are only printed.

## Data structures

The manifest (above) and the stage list: `<kind> <host path> <device path>`
per line (`exe`, `file` or `tree`), read by `build.sh`'s `lib_tests_muster`.

## Concurrency

None: muster runs one test at a time, as CTest does by default.

## Invariants enforced

- The device runs upstream's list, not a copy: every test comes from the
  library's own CTest or meson registration (or, for libwebp, the one JSON
  file), and a test the translator cannot carry stops the build.
- A pass is an exit status of 0 from every test, and the gate checks the
  count as well as the verdict.

## Error paths

A test that cannot be spawned, a working directory that cannot be entered,
a builtin's unreadable file: that test fails with the reason, and the run
continues. A malformed or miscounted manifest: exit 2 before any test runs.

## Performance

The `ls-libs` gate's eight legs (ICU's two and the six muster lists) take 336
to 348 s with the boot, under HVF on the Mac (three runs, 2026-10-19/20); with
B-3d's five legs it took 262 s, and B-3c's two ICU legs alone 118 s. The legs
are not timed one by one.

## Prosecution

- Does a test that passes on the host but has no witness on the device slip
  through? Only by not being in CTest's list, which is upstream's choice.
- Is a relative input staged at the place the device resolves it? The
  translator checks each against the device working directory and refuses a
  mismatch.
- Is an input the build makes for a test there on the device? For a meson
  test its `depends` name it: the build builds `meson-test-prereq`, and the
  translator refuses an output that is not built or not staged
  (issue-1441's two fonts are such outputs). CTest builds nothing before it
  runs a
  test, so a CTest library's inputs are what its whole build made, or what an
  earlier test writes.

## Seams

- libthyla-rs `Command` has no `cwd` or environment of its own: muster sets
  its own working directory, and a meson test's variables through `/env`,
  which each child inherits. CTest's `ENVIRONMENT` property is still refused;
  no library here sets it.

## Caveats

- A test program that reads a file it does not name on its command line
  gets only what `build.sh` stages beyond the translator's list (libxml2's
  three trees, HarfBuzz's font and text trees), except a meson target it
  depends on, which is checked.
- issue-1063's font is not freely licensed: its copyright record reads "All
  Rights Reserved" and grants nothing, and the sites that list it say
  personal use only. It is fetched as upstream's script fetches it, so an
  image baked with these tests is for the builder's own machines and is never
  published (`tools/build-manifest.toml`, `[cache.ftfont]`).
- libwebp's lossy digests are the reference implementation's output, not an
  independent oracle: they prove the Pouch build decodes and encodes exactly
  as libwebp 1.6.0 does natively, and upstream's own reference image agrees
  with that decode to within 1 per sample.

## Provenance

B-3d (2026-10-15), main session; libxml2 B-3e; meson, `env` and TAP B-3f
(2026-10-15; the `depends` check 2026-10-19, witnessed 2026-10-20).
