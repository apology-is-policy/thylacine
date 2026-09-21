---
id: sub-webkit
type: sub
title: "WebKit (Boosty) — JavaScriptCore on Pouch, JIT off, and what the engine asked the platform for"
parent: moc-userspace
code:
  - usr/ports/webkit/README.md
  - usr/ports/webkit/patches/0001-thylacine-jsconly-b0.patch
  - cmake/Toolchain-aarch64-pouch-cxx.cmake
  - cmake/Platform/Thylacine.cmake
  - tools/interactive/ls-jsc.exp
audit: light
guarded-by: [inv-i12]
validated-by: [prose, gate-interactive]
locks: []
hazards: []
abis: []
design: [docs/BROWSER-DESIGN.md]
created: 2026-09-21
updated: 2026-09-21
---
## Purpose

The WebKit port for Boosty, the Thylacine web browser
([[dec-2026-09-21-browser-engine-order]]: WebKit first, then Servo). At B-0
it is ONE thing: JavaScriptCore alone (`PORT=JSCOnly`), static, with every
JIT tier off, built against Pouch + libc++ + a cross-built ICU, proven to
run on the device. It exists to measure what a real engine asks of the
platform before any kernel work is designed — the findings list in
`docs/browser-status.md` is its product as much as the binary is.

The source is NOT vendored. WebKit is about 13 GB; it lives in a sparse,
partial clone beside the other forks (`~/projects/webkit-thylacine`, branch
`thylacine`), pinned to `webkitgtk-2.54.0` =
`5220e80b97a253c60ed899361654142ab5021998`. This directory carries the patch
series; `tools/forage.sh webkit` gathers the source and the ICU tarball, and
`CHUNK_WEBKIT=y` (default off) builds and bakes it -- the wiring is described in
[[sub-substrate-build]].

## Contract

- A 60 MB static `ET_EXEC`, segments R / R+E / RW / RW, no `PT_DYNAMIC`, no
  segment both writable and executable ([[inv-i12]] by construction — with
  the JIT off the engine emits no code at all).
- Interpreters only: the offlineasm-generated LLInt for JavaScript
  (`ENABLE_C_LOOP=OFF`, so it is the assembly interpreter, not the C loop)
  and IPInt for WebAssembly, which JSC keeps when the JIT is off.
- `USE_SYSTEM_MALLOC=ON`: bmalloc/libpas is out, mallocng is in. Gigacage
  is therefore off.
- One JavaScript thread. `Thread::suspend` is fatal on Thylacine rather
  than silently wrong (see Mechanism).

## Mechanism

**CMake needs a real platform module, not `Generic`.** `CMAKE_SYSTEM_NAME
Generic` loads `Platform/Generic.cmake`, which leaves `UNIX` unset after
the toolchain file runs — so a `set(UNIX 1)` in the toolchain is reset and
WebKit stops at "Unknown OS 'Generic'". `cmake/Platform/Thylacine.cmake`
sets `UNIX`, declares no shared-library support and the `.a` suffixes;
`cmake/Toolchain-aarch64-pouch-cxx.cmake` names the system `Thylacine`,
puts that directory on `CMAKE_MODULE_PATH`, selects the fork clang
(`~/projects/llvm-thylacine/build`, the driver that defines
`__thylacine__`) with the host LLVM's `ar`/`ranlib`/`nm`, points at
`build/sysroot`, takes extra roots (`THYLACINE_EXTRA_ROOTS`, how the ICU
stage is found), links `-static`, and sets
`CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` so configure probes never
need a runnable link. Both files are port-neutral: any C++ CMake port can
use them.

**ICU is cross-built in two passes** (host tools first, then
`--with-cross-build`), static data packaging, `--disable-dyload`. The
target flags go in `CC` itself: with `--target` only in `CFLAGS`, ICU's
dependency-generation steps compile for macOS.

**All of WTF compiled against Pouch unpatched except five files, and all of
JavaScriptCore except two.** The whole delta is one patch, ten files,
+60/−3:

- `PlatformOS.h` — `OS(THYLACINE)` from the compiler's `__thylacine__`.
- `ARM64Assembler.h` `cacheFlush` — `SYS_ICACHE_SYNC` (103) by raw `svc`,
  `RELEASE_ASSERT` on failure. Unreached with the JIT off; present so the
  file compiles and so B-2 inherits a fail-loud publish.
- `InlineASM.h` + offlineasm `arm64.rb` — Thylacine takes the ELF branches
  (`HIDE_SYMBOL`, `.L` local labels, the `globaladdr` sequence).
- `MemoryFootprintGeneric.cpp` — upstream bit-rot: it calls `memoryStatus()`
  on platforms that do not declare it. Returns 0 elsewhere.
- `WasmFaultSignalHandler.cpp` + `Options.cpp` — the Wasm fault handler is
  compiled out where there is no machine context, and
  `useWasmFaultSignalHandler` is forced off there; upstream never builds
  Wasm-on with no machine context. `usePollingTraps` is on for Thylacine.
- `OptionsJSCOnly.cmake` — API tests off (the sparse clone has no gtest).
- Three PROBE-ONLY tolerances, each a recorded finding rather than a fix:
  `ThreadingPOSIX.cpp` skips the `SIGUSR1` suspend handler (Pouch's
  `sigaction` admits SIGINT/TERM/PIPE/CHLD only) and makes
  `Thread::suspend` fatal (F7); `OSAllocatorPOSIX.cpp` does not trim the
  slack of an aligned reservation and tolerates the `munmap` failure in
  `releaseDecommitted` (F3). Guard pages and decommit are silently absent
  because WTF ignores those results itself (F4, F5).

**The device gate.** `tools/interactive/ls-jsc.exp` runs six legs as the logged-in user, each of
which is the measured symptom of something: `hello` (exec + the LLInt),
`icu` (`1.234.567,891` through the static ICU data), `recurse` (a graceful
`RangeError` -- needs true main-thread stack bounds, pouch 0033), `heap`
(400,000 object pushes -- needs a true RAM figure, pouch 0034), `typed` (a
64 MiB typed array written at its last byte), `wasm` (the object exists and
validates a header, IPInt). Every marker is printed in lower case by the
guest and asserted in UPPER case through `tr`, and no expected string occurs
in the typed line (inputs are spelled `6*7`, `4e5`, `64<<20`): the terminal
echoes what is typed, and an expect the echo can satisfy proves nothing.
`$errstr` is never consulted; `ut` does not clear it after a success.

## Data structures

None of its own.

## Concurrency

One JS thread at B-0. JSC's GC and compiler threads exist as pthreads; the
collector's stop-the-world uses `Thread::suspend`, which is made fatal
here, and no B-0 workload reached it. That is a statement about the
workloads run, not a proof about the collector: a multi-threaded JS
workload (Workers, shared-memory Wasm) is expected to hit it, and F7 is the
design item that answers it.

## Invariants enforced

- **[[inv-i12]]** — trivially: no code is generated. The patch already
  routes the one publish point through `SYS_ICACHE_SYNC`, so the B-2 JIT
  work (JSC's separated-WX-heap mode onto `SYS_JIT_CREATE`) starts from a
  fail-loud seam rather than from `__builtin___clear_cache`.

## Error paths

A WebKit `CRASH()`/`RELEASE_ASSERT` is `abort()`, which on Pouch is a
SILENT `_Exit(127)` — the same number `ut` reports for "command not found",
and `ut` does not clear `$errstr` after a success, so a stale "spawn
failed" can sit next to it. B-0 lost an hour to exactly that pairing. The
diagnostic that ended it is twelve lines: an `abort()` override linked
ahead of `libc.a` that prints return addresses via `_Unwind_Backtrace`,
symbolized with `llvm-nm -n --demangle` (return address − 4).

## Performance

`fib(30)` = 832040 in 71–72 ms on the LLInt (HVF, M2). No JIT figure exists
yet.

## Prosecution

- The three tolerances are probe posture. None may survive into a shipped
  browser: each either leaks VA (F3), never returns memory (F4), or removes
  a safety net (F5 guard pages, F7 suspend).
- The pin is a tag AND a hash; a codified build must check the hash.
- When the JIT lands, the `cacheFlush` arm stops being dead code — it is
  then a W^X-adjacent surface and this dossier's `audit:` becomes `hard`.

## Seams

The platform findings F3–F9 in `docs/browser-status.md` are this port's
seams, and by the operator's direction (2026-09-21) their kernel design is
a conversation first.

## Caveats

- `build_sysroot` wipes `build/pouch/`, which holds the ICU and JSC objects
  ON PURPOSE (a static binary linked against the old `libc.a` must not
  survive a libc change) -- so with the chunk on, every Pouch patch costs a
  ~40 minute rebuild. That is the price of static linking, paid knowingly.
- The host has 8 GiB of RAM: `ninja -j5` for JSC; all of WebKit belongs on
  the remote builder.
- B-0 found three Pouch libc defects by running (0033, 0034, 0035); they
  are described where they live, in [[sub-pouch-thread]] and
  [[sub-pouch-seam]].

## Provenance
