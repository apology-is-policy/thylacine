# Boosty -- browser arc status (the B-arc; `docs/BROWSER-DESIGN.md`)

The authoritative pickup guide for the web-browser arc. The design document is
the plan and the research; this tracks what has LANDED and what is next. The
arc sits outside ROADMAP's eight phases and must never put the v1.0 release
candidate at risk (ROADMAP section 11; BROWSER-DESIGN section 10, risk 7).

## TL;DR

**Ratified 2026-09-21. The browser is named Boosty (the operator's cat). B-0 is in progress on branch `browser-b0`: JavaScriptCore already RUNS on the device (JIT off).** The operator voted: **WebKit
first, then Servo; no stage 0 (no NetSurf, no `webfs`); the Rust `std` port
runs in parallel, owned by the aux track; effort `xhigh` throughout.** The
first implementation chunk is **B-0: JavaScriptCore alone** (`JSCOnly`, no
JIT) cross-built for `aarch64-thylacine` -- WebKit's own first step for a new
OS, and the cheap way to *measure* the anonymous-memory gaps (P1) and answer
the JIT question (B-2) instead of guessing.

## Landed chunks

| Commit | What | Witness |
|---|---|---|
| `c09141da` | `docs/BROWSER-DESIGN.md` PROPOSED: six research lanes, the tree's measured starting line, the platform tranche, the JIT mapping, the capability-graph design | docs only |
| `8cd50a2d` | RATIFIED: the vote recorded (`dec-2026-09-21-browser-engine-order`), this status doc, the NOVEL.md candidate, the track-R brief for aux (`docs/handoffs/041`) | docs only |

## B-0 in progress (branch `browser-b0`, WIP -- NOT gated, NOT on `main`)

**JavaScriptCore runs on Thylacine** (2026-09-21, HVF, JIT off = asm LLInt + IPInt).
WebKit `webkitgtk-2.54.0`, `PORT=JSCOnly`, static, against ICU 78.3; a 60 MB `ET_EXEC`
with no `PT_DYNAMIC` and no W+X segment. Measured on the device:

| Check | Result |
|---|---|
| `print("hello-" + 6*7)` | `hello-42` |
| `new Intl.NumberFormat("de-DE").format(1234567.891)` | `1.234.567,891` (ICU + its data) |
| runaway recursion in `try/catch` | `RangeError`, graceful |
| `a.push()` x 400,000 (ints, then objects); `new Array(300000)` filled | all reach the end |
| `new Uint8Array(N << 20)`, N = 1..256 | all succeed, last byte written |
| `typeof WebAssembly`, `WebAssembly.validate(header)` | `object`, `true` |
| `fib(30)` on the interpreter | 832040 in 71-72 ms |
| ICU's own `genrb` tool (same toolchain) | runs, prints its ICU 78.3 usage |

**All of WTF compiled against Pouch unpatched except five files**; all of JavaScriptCore
except two. The whole WebKit-side delta is `usr/ports/webkit/patches/0001` (10 files, +60/-3):
an `OS(THYLACINE)` guard; `cacheFlush` -> `SYS_ICACHE_SYNC`, fatal on failure; the ELF
branches of `InlineASM.h` and offlineasm's `globaladdr`; an upstream bit-rot fix in
`MemoryFootprintGeneric.cpp`; the Wasm fault handler compiled out where no machine context
exists (upstream never builds Wasm-on + no-machine-context); `OptionsJSCOnly` API tests off;
and three tolerances listed as findings F3-F5 below.

**Two Pouch libc bugs found and fixed on this branch** (each a libc-only change, no kernel):
- `0033` -- `pthread_getattr_np()` told every program its main-thread stack is ONE PAGE
  (upstream probes with `mremap`, which the seam ENOSYSes). Measured `size=4096`; real is
  1 MiB. JSC threw a stack overflow on its first call and could not print it. Pinned by a
  two-sided check in `pouch-hello-threads`. **The aux Rust-`std` track would have hit this
  too** (std's main-thread guard).
- `0034` -- `sysconf(_SC_PHYS_PAGES)` returned UNINITIALISED STACK (upstream never checks its
  ENOSYSed `sysinfo`). JSC caps its heap at 2 x RAM, so every allocation over 8 KB failed
  ("Out of memory" at array element 1003). Now reads `/ctl/memory`; -1 on a miss.

### The measured platform findings -- the list for the operator conversation

The operator, 2026-09-21: *"When you reach the need to design the kernel chunks -- mprotect,
dlopen etc., let's talk about it."* Nothing below has been designed. This is what B-0
measured, in the order the engine hit it, with what the probe did instead.

| # | What JavaScriptCore needs | What Pouch/Thylacine does today | What the probe did | Kind |
|---|---|---|---|---|
| F1 | main-thread stack bounds | reported one page | fixed (`0033`) | libc, done |
| F2 | machine RAM size | returned stack garbage | fixed (`0034`) | libc, done |
| F3 | **aligned address-space reservation** (`tryReserveUncommittedAligned`: map size+align, then `munmap` the two slack ends) | partial `munmap` refused (whole-mapping detach only) -> `CRASH()` | keeps the slack mapped; harmless because lazy reservations commit nothing, but the VA is never returned | **kernel/Pouch design** (an alignment request on attach, or partial detach) |
| F4 | **decommit** (`madvise(MADV_DONTNEED/FREE)` on the GC's freed blocks) | `madvise` = ENOSYS; WTF ignores the error, so memory is simply never returned | nothing (RSS only grows) | Pouch wiring onto the existing `SYS_BURROW_DECOMMIT` -- no kernel change, but it is the P1 conversation |
| F5 | **guard pages** (`mmap(MAP_FIXED, PROT_NONE)` over the ends of a reservation) | `MAP_FIXED` refused; WTF ignores the result | silently none | **kernel design** (reservation holes vs an I-12 wording change -- BROWSER-DESIGN O-1) |
| F6 | **reservations over 256 MiB** (structure heap asks 4 GiB, halves until it fits) | `BURROW_ATTACH_MAX` = 256 MiB per mapping | JSC settled at 128 MiB by itself | policy; fine for JSC, will bind Gigacage / Wasm fast memory |
| F7 | **per-thread async signal + machine context** (`SIGUSR1` for GC thread suspend, signal-based VM traps, the sampling profiler, Wasm fast-memory faults) | `sigaction` admits SIGINT/TERM/PIPE/CHLD only; no `pthread_kill`; no `ucontext` registers | polling VM traps; suspend made fatal; fault handler off. Costs nothing with one JS thread and no JIT; **matters for B-2 (JIT) and for multi-threaded JS** | kernel/Pouch design (notes are per-Proc, fd-first) |
| F8 | a main-thread stack of several MiB (JSC asks 5) | fixed 1 MiB reservation; SPARSE since LINEAGE L-4a (`exec_map_user_stack` is `burrow_create_anon_lazy`) -- I first wrote "eager" here from `exec.h`'s comment, which was stale and is corrected in the same change | JSC clamps to what it gets | raising it costs nothing at exec: a constant in `exec.h`, its mirror in pouch 0033, the prover's pin, and a higher I-32 ceiling for runaway recursion. Better still: pass the extent in auxv so libc DERIVES it instead of mirroring |
| F9 | `dlopen` | none (static only) | not needed by JSC | the operator named it; WebKit proper does not need it either (no plugins) |

Also learned, not engine findings: **the host has 8 GiB of RAM and ~15 GiB of free disk** --
JSC builds locally at `-j5`; all of WebKit will not, and belongs on the GCP builder (the Clade
precedent). `ut` does not reset `$errstr` after a success, which cost this run an hour (see
the journal). A 60 MB binary fetched with the native `curl` into an encrypted home execs fine.

### What is NOT done in B-0 yet

- `build_icu` / `build_jsc` in `tools/build.sh`, the manifest entries (`fork.webkit`,
  `cache.icu4c`), pool staging (`/webkit`), a gate (`ls-jsc.exp`). The recipe is in
  `usr/ports/webkit/README.md`; the scratch build is `build/pouch/{icu,jsc}`.
- The Pouch patches are UNGATED: a sysroot rebuild (which wipes and rebuilds libc++, zlib,
  SDL2) + the kernel suite + the ci fleet are owed before they reach `main`. They were
  verified by linking the patched objects into `jsc` and `stackprobe`, which is a real test
  of the code and NOT a test of the series applying in a from-scratch sysroot build.
- The audit round for the two Pouch patches (pouch pthread + the sysconf surface).
- test262 / a real benchmark; the v8.0 floor check on `jsc`.

## Remaining work (in order; BROWSER-DESIGN section 9)

| Phase | What | Exit |
|---|---|---|
| **B-0** | `JSCOnly` cross-build, JIT off (asm LLInt + IPInt). Source lives OUTSIDE this repo (a `webkit-thylacine` fork beside `llvm-thylacine`); the repo carries the build wiring and patches. | `jsc` runs on the device; the measured list of P1 gaps |
| **B-1** | P1, the anonymous-memory surface, scoped by B-0's measurements. **Scripture first** -- O-1 (reservation holes vs an I-12 wording amendment) needs the operator's signature. Audit-bearing. | allocators run; SMP gate; audit closed |
| **B-2** | The JIT: JSC's separated WX heap on `SYS_JIT_CREATE` + `SYS_ICACHE_SYNC`; `CAP_JIT` clearance. Audit-bearing (I-42/I-12). | benchmark with JIT tiers; deny-path probe (no `CAP_JIT` -> interpreter, never RWX) |
| **B-3** | P3: ICU, FreeType, HarfBuzz, sqlite, libxml2, png/jpeg/webp, libpsl, curl, OpenSSL; fontconfig decision (O-4). | each library's tests under Pouch |
| **B-4** | P2: its own design document (O-3: generalise the Weft share gate vs Mycelium), the primitive, then WebKit's `Platform/IPC` + `SharedMemory` backend. Audit-bearing (I-4). | two-process message + shared-bitmap witness |
| **B-5** | WebCore + WebKit2 headless, `PORT=Thylacine` modelled on PlayStation; P5 (EGL) resolved here; O-2 (which WebKit line to track) measured here. | `WKPagePaint` renders a local page to a PNG on the device |
| **B-6** | The chrome on Tapestry; P6; the constructed namespaces with deny-path probes. | a TLS page in a tile; content Proc proven unable to open `/net` |
| **B-7** | Hardening, fuzz posture, the owed invariant ENFORCED, the Operator's Manual section. | arc close |
| **R** | **aux**: Rust `std` for Thylacine, the crate tail, then Servo. Brief: `docs/handoffs/041-rust-std-track-to-aux.md`. | a `std` hello built by cargo and run on the device |

## Exit criteria status

- [ ] A page loads over TLS from the network and renders in a Halcyon tile.
- [ ] JavaScript runs with a JIT under strict W^X (no RWX page ever exists).
- [ ] A content Proc cannot name `/net`, `/srv`, `/proc` or `/dev` (deny-path probe).
- [ ] The owed section-28 invariant is allocated, then ENFORCED.
- [ ] The Operator's Manual has a browser section.

## Trip hazards

- **Effort is `xhigh`, by the operator's vote, for the whole arc.** Say so in
  every audit-bearing commit body; do not re-ask.
- **No permission-mutation syscall exists (I-12).** Do not add `mprotect` to
  make an allocator happy. B-1 is a scripture commit with a signature first.
- **`SYS_WEFT_SHARE` is gated to the driver tier on purpose** (Weft-7 F1,
  `kernel/syscall.c`). Read that audit before proposing to lift it.
- **Thylacine links statically and has no `dlopen`.** WebKit is LGPLv2: keep
  the build reproducible from published source so LGPL section 6 is met.
- **Swift is entering WebKit** (`ENABLE_BACK_FORWARD_LIST_SWIFT`; OFF when
  cross-compiling). Pin it OFF explicitly and watch for it becoming mandatory.
- **WebKit cannot be built on thyla-pi** (1.5-2 GB per unified job). Host only.
- **Never claim a research fact from memory.** The design's section 13 marks
  what was verified in source; five items are flagged unverified, and the
  first of them (every JSC writer uses `performJITMemcpy`) is B-2's first job.

## References

`docs/BROWSER-DESIGN.md`; `vault/record/decisions/dec-2026-09-21-browser-engine-order.md`;
`docs/NOVEL.md` ("The browser as a capability graph", "Mycelium", "JIT-as-a-capability");
`docs/JIT-ON-WX-DESIGN.md`; `docs/LLVM-DESIGN.md` (the C++ runtime, CL-2/CL-3);
`docs/POUCH-DESIGN.md` section 8 (memory); `usr/lib/thylajit/thyla_jit.h`.
