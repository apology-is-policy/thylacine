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
| `af293efc` | scripture: first-party `std` Rust on Pouch is the THIRD userspace substrate (ARCH 3.5) | docs only |
| `25df504f` | scripture + spec: the mount-table shed at pivot/chroot (#80, ARCH 9.6.10); `specs/territory_shed.tla` + 5 buggy cfgs | `specs/check-territory-shed.sh` |
| `b8b27f1d` | scripture + spec: poll re-registers every pass, owns death/stop, crosses a preemption point; `specs/poll_cpu.tla` NEW; ARCH 8.1's as-built note | `specs/check-poll.sh` 16/16 |
| `2a737959` | territory + stalk: the shed, the `..` floor, the dissolved-union degrade, the COPEN strip | suite + `symlink-probe` union-a/b/c 34/0 |
| `1f14b6c5` | poll: the re-arm, the loop's own death/stop, `sched_preempt_point`, the irqsave list ops, the two-endpoint SrvConn poll | suite; `sched.preempt_point_takes_a_pending_irq` |
| `92a9ae94` | pouch 0033-0042 + the provers + the census header + `check-patch-hunks.py` | `pouch-hello-*` legs |
| `b70e1bfd` | the WebKit port: JSCOnly JIT-off, the CMake platform/toolchain files, `CHUNK_WEBKIT`, ICU | `ls-jsc.exp` (6 legs) |
| `5c08c7fd` | journal: addenda 3-7 | docs only |
| `3df67cb0` | `vault/record/`: 43 notes -- 10 audit rounds, 27 findings, 5 changes, 1 decision, 1 arc | `quaestor lint` 0 fail |

## B-0 (built, audited over ten rounds, gated; landed as nine commits)

The working history is the branch `browser-b0` (46 commits, most of them labelled WIP and ungated as
they went). It was landed as nine coherent commits so that `main` stays bisectable -- the landed tree
is byte-identical to the branch's, checked with `git diff`. The per-step hashes cited in
`docs/JOURNAL.md` and in the `vault/record/` notes name working-branch commits.

The arc's record plane is `vault/record/`: one `chg` per change, one `adt` per audit round, one `fnd`
per finding, and `dec-2026-09-22-point-now-model-next` for the one decision that was the operator's.

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

**Three Pouch libc bugs found and fixed on this branch** (each a libc-only change, no kernel),
and a fourth found and deliberately NOT fixed here:
- `0033` -- `pthread_getattr_np()` told every program its main-thread stack is ONE PAGE
  (upstream probes with `mremap`, which the seam ENOSYSes). Measured `size=4096`; real is
  1 MiB. JSC threw a stack overflow on its first call and could not print it. Pinned by a
  two-sided check in `pouch-hello-threads`. **The aux Rust-`std` track would have hit this
  too** (std's main-thread guard).
- `0034` -- `sysconf(_SC_PHYS_PAGES)` returned UNINITIALISED STACK (upstream never checks its
  ENOSYSed `sysinfo`). JSC caps its heap at 2 x RAM, so every allocation over 8 KB failed
  ("Out of memory" at array element 1003). Now reads `/ctl/memory`; -1 on a miss (errno
  untouched; a figure that runs to the end of the read buffer is a miss too). Pinned by
  `pouch-hello-malloc`, which re-parses `/ctl/memory` itself and demands equality.
- `0035` -- found BY the 0034 pin, not by the engine: **`fscanf`/`scanf` have been broken on
  every real `FILE` since patch 0002**, and every `getc` was one syscall. 0002's read backend
  never refilled `f->buf`, so musl's "the byte just returned sits at `rpos[-1]`" invariant was
  gone and every scan pushback re-read `buf[1023]`. Raw `read`, `fgetc` and `fread` over the
  same file were all correct, which is what cleared the kernel. Restores upstream's own
  no-`readv` refill arm; `pouch-hello-fopen` gains a `scan` leg, measured RED on the unfixed
  libc first. Behaviour change: C stdio input now reads ahead, like every other libc.
- **OPEN, not fixed here: `getuid`/`geteuid`/`getgid`/`getegid`/`getppid` return the raw ENOSYS
  sentinel, `0xFFFFFFDA`.** The kernel has `SYS_GETUID`/`SYS_GETGID`; only `getpid` was ever
  wired. stratumd CONSUMES the value (its admin uid, the keyslot token gate, dataset-root
  ownership), so the fix changes the storage daemon's security behaviour and is its own chunk
  on the A-3 identity surface. Found by sweeping the class all four belong to: a syscall parked
  at the sentinel whose libc caller cannot report failure, so the program gets a wrong value.
  Every C library in B-3 and the Rust `std` crate tail will call these.

### The measured platform findings -- the list for the operator conversation

The operator, 2026-09-21: *"When you reach the need to design the kernel chunks -- mprotect,
dlopen etc., let's talk about it."* **DESIGNED 2026-09-23 -- see "The B-1
decisions" below.** The table is B-0's measurement, kept as the record: what the engine hit, in
order, and what the probe did instead.

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

### The B-1 decisions (2026-09-23; `dec-2026-09-23-memory-surface-and-loader`)

The conversation the operator asked for, held at max effort on Fable 5.1. Each vote was by blocking
question; the research behind them -- Plan 9, Fuchsia, Genode, seL4, OpenBSD, Mach, and WebKit's own
source read line by line -- is ARCH 6.5 "The permission ceiling", "Range detach", "Capacity" and
"Dynamic loading".

| # | Decision | Vote |
|---|---|---|
| 1 | the shape of the permission surface | **ceiling-bounded `burrow_protect`** (the Fuchsia / Mach shape): a mint-time `prot_max` per VMA; X never a target; a range within one mapping |
| 2 | a capability on the call, like `CAP_JIT`? | **no -- the gate is structural.** Attenuation and a bounded re-grant create no authority; a cap here would be ambient (every pthread guard needs it) or would break every threaded program |
| 3 | the one-way seal | **yes, same chunk** (`PROTECT_SEAL`; Mach `set_maximum`, not OpenBSD `mimmutable`) |
| 4 | F8 / F9 | **dlopen designed now** (rows 5-6); the stack to 8 MiB with its extent in auxv (my reading of "as well", stated to the operator and not contradicted) |
| 5 | the loader model | **L-A, dynamic Pouch**: `libc.so` is the loader; D-4's PT_INTERP rewrite lifted to native execs; `burrow_map_file` |
| 6 | what the sysroot ships | **static by default; `.so` only for runtime-loaded objects and `libc.so`** |
| 7 | the memory bar | **production-comparable, both substrates**: never refused while free memory exists; relinquished memory returns and the footprint shrinks |
| 8 | the I-32 default | **physical RAM minus a boot-sized TCB reserve**; the cap is the confinement mechanism |
| 9 | the native allocator | **`dlmalloc-rs`** over a Thylacine platform trait (alloc = lazy attach, free = detach, free_part = decommit) |
| 10 | the capacity chunk | **range detach + the charged sparse `filepages`**, both in B-1a' |
| 11 | the sequence | scripture -> **B-1a** permissions -> **B-1a'** capacity -> **B-1b** Pouch -> **B-1c** dlmalloc + witness -> **B-1d** dlopen, before B-3 |

Measured against the bar before the vote (the reason 7-10 exist): four refusals with free memory --
the fixed 4 MiB native heap (`alloc.rs:77`), the 256 MiB default budget against a 2 GiB VM
(`proc.h:114`), the 256 MiB / 1 GiB per-reservation caps anchored to the flat uncharged `filepages`
array (`syscall.h:3203/3217`), and eager attach's contiguity (rings only; not a bar issue) -- and three
paths that keep relinquished pages: `mallocng`'s `MADV_FREE` inside a retained group (`free.c:124`,
ENOSYS today), WebKit's own decommit (same), and the native allocator (never trims). Decommit itself
already frees and uncharges (`burrow.c:1246-1266`); whole-group `munmap` already works.

What the vote did NOT cover and stays open: F7 (a thread-directed async note + register capture --
its own notes design, needed by B-2 and by multi-threaded JS); the fixed 1024-handle table (#355,
flagged, not built); PIE for Pouch binaries (decided at B-1d); OOM victim selection (deliberately not
built -- the reserve is the production property that matters).

Also learned, not engine findings: **the host has 8 GiB of RAM and ~15 GiB of free disk** --
JSC builds locally at `-j5`; all of WebKit will not, and belongs on the GCP builder (the Clade
precedent). `ut` does not reset `$errstr` after a success, which cost this run an hour (see
the journal). A 60 MB binary fetched with the native `curl` into an encrypted home execs fine.

### Where B-0 stands (2026-09-22)

The working branch is `browser-b0`; the landed history is the nine commits in the table above.
`docs/JOURNAL.md` (2026-09-21 and its addenda 1-7) carries the story; `vault/record/` carries the
per-round evidence; this is the ledger.

- **Ten audit rounds.** Pouch/libc 1-4, the shed 1-3, the B-0 self round, poll 5-7. Every P0/P1/P2 is
  fixed or tracked with a queue item; nothing was closed silently. Rounds 4 onward ran on Opus 5 (the
  Fable fallback with the same-family preamble, per CLAUDE.md: a round that finishes is closed, and a
  round is never skipped for want of Fable). Round 7's three P2s were all failures of VERIFICATION
  rather than of the mechanism, which is the part worth carrying forward:
  a spec property entailed by the behaviour it was written to exclude (`fnd-b0poll-r7-f2`), tests that
  could not witness the window they were named for (`fnd-b0poll-r7-f3`), and a latency claim resting
  on an unbounded walk (`fnd-b0poll-r7-f1`, now `seam-poll-hooks-per-list`).
- **Every new test rode a RED-before** on a kernel with its own fix reverted
  (`work/b0/red3/kernel-sab2.py`). Three of those sabotages PASSED at some point and each was a real
  finding, not a formality: the deadline sabotage (the test asserted the result, not the wake), the
  c2s-drain sabotage (same shape), and `noisb` -- which is kept in the script as a labelled
  NON-discriminating control, because the `isb` widens the unmask window and cannot be witnessed here.

- **DECIDED 2026-09-23 (was owed as a conversation): the F3-F9 kernel design** -- eleven votes in
  "The B-1 decisions" above; the scripture is ARCH 6.5. Still owed as conversations: the
  small-integer socket fd redesign; unlink-while-open; and F7 (per-thread signals).
- **DECIDED 2026-09-22 ("point now, model next"), was owed:** syscalls run IRQ-masked end to end, so a
  noise-driven wait held its CPU's interrupts. poll now crosses a PREEMPTION POINT each re-loop
  (`sched_preempt_point`; ARCH 23.3, `specs/poll_cpu.tla` checks the CPU-level claim). Scheduled next,
  BEFORE the F3-F9 kernel work and it deletes the point: build ARCH 8.1 as written -- syscall bodies
  with interrupts ON, still non-preemptible. The Phase-0 deferral that was never executed (ROADMAP's
  "Kernel preemption" item never reached a status doc).
- **OPEN from poll round 7 (F1), tracked here because nothing else owns it:** nothing caps hooks on one
  `poll_waiter_list` (64 per call x `PROC_THREAD_MAX` x Procs), so a producer's `poll_waiter_list_wake`
  walk and the poller's per-pass unregister walks are O(attacker-scaled) and IRQ-masked -- and the
  producer's walk crosses no preemption point at all. The point bounds the NUMBER of masked spans, not
  the length of one. Fixes: the per-endpoint/event-keyed lists of round-4 F8, a per-walk wake cap with
  the remainder deferred, and an I-32 axis capping hooks per list.
- test262 / a real benchmark on `jsc`: not started.

## ARCH 8.1 -- the kernel chunk that precedes F3-F9 (BUILT 2026-09-22, branch `arch81`)

Not a browser chunk, but sequenced here because the operator put it before the
browser arc's kernel work and because B-0's audit is what surfaced the defect
it repairs. Seventeen commits off `main` @`ca1c7030`; tip `ce802b56`.

**Syscall bodies now run with interrupts ON and are still non-preemptible** --
ARCH 8.1's line as written, which Phase 0 deferred and P3-Ec accidentally built
as "interrupts off" instead. `Thread.in_syscall` gates `preempt_check_irq`;
`syscall_dispatch` is a wrapper that unmasks for the body and re-masks
unconditionally before the EL0-return tail, so the unmask cannot leak into the
KERNEL_EXIT eret window (#713).

What it means for the browser arc: **poll's preemption point is deleted**
(`b7132455`), and with it `specs/poll_cpu.tla`. `pipe_block_locked` and
`chan_role_acquire`, which had the same masked-loop shape and no point, are
covered without needing one. The B-0 residue item "any other masked loop"
(r5 F1's tail) is closed by construction rather than by a sweep.

Measured, and one number is a finding in its own right: the Linux-phenotype
syscall path was at **86% of the 16 KiB kernel stack** before this chunk,
because `viv_tier2`'s switch unioned getdents64's 4.6 KiB of staging into every
phenotype syscall's frame. Fixed (`e7c83ec6`); worst case is now 75.5% WITH the
new IRQ frame.

**Audit round 1 closed 2026-09-22: 0 P0 / 1 P1 / 1 P2 / 6 P3, all fixed**
(`dc77a4b0` + `ce802b56`). An **OPUS FALLBACK** round -- Fable was out of
credits and the rule is never to skip a round for want of it -- so the
family-diversity axis was forfeited and context independence was not; note the
tier when weighing it. The prime target (a lock-free read-modify-write on state
an IRQ handler also writes -- the class the chunk's lock sweep did NOT cover)
came back **clean**.

The P1 was a false claim in ARCH 8.12 itself: it said five latent single-CPU
hangs were fixed unlooked-for, and `loom_free`'s spin on a KTHREAD's exit flag
is not one of them -- `in_syscall` refuses the switch that would run the
kthread. Servicing an interrupt is not scheduling a thread. Scripture
corrected; the defect was left **OPEN, pre-existing, and tracked**, and is now
**CLOSED** (below). The P2 was an
unprivileged masked-window DoS in `/ctl/kstack`, an instrument this chunk had
added two commits earlier -- now `CAP_HOSTOWNER`-gated with a budgeted scan.

| bar | state |
|---|---|
| suite @tip `ce802b56` | **1616/1616 PASS** |
| poll spec gate | ALL CFGS AS CLAIMED (4 clean + 7 buggy) |
| `syscall_irqs` gate | 8 cfgs on their named verdicts, clean at 18 states; the tail-check sabotage now caught (it was NOT, before the close) |
| SMP gate @`dd0e9ce1` | **PASS -- 40/40 boots, 0 corruption** (default/ubsan x smp4/smp8) |
| SMP gate @close `40261a8c` | **PASS -- 40/40 boots, 0 corruption** (default/ubsan x smp4/smp8). This gate matters more than usual here: #713 was 3-13% of boots and NEVER at `-smp 1`, so the HVF suite is structurally blind to the hazard this chunk sits closest to. |
| `tools/test-fault.sh` | **8 PASS / 0 FAIL of 8** -- all three kernel-stack GUARD variants fire (`kstack_overflow`, `secondary_stack_guard`, `bootcpu_idle_guard`), plus `recursive_kernel_fault` and `el1_sync_runaway`, the nested-exception cases this chunk makes more reachable. Added to this chunk's bar MID-RUN: the chunk deepens the kernel stack and this is the only runtime witness that an overflow FAULTS into a no-access guard rather than corrupting its neighbour. A bar that omits the one gate aimed at the hazard the change creates is a bar that verifies around it. |
| kstack runtime witness | **peak=10448 of 16384 = 63.8%** on a default boot, now printed every boot |
| interactive fleet @`40261a8c` | **PASS -- 55/77, 0 FAIL** (22 SKIP = absent optional host artifacts + the documented ci-vs-halcyon image split, neither a guest result nor coverage). The ~15 scenarios that parse boot output all pass, which is the check that mattered: this chunk adds a `boot-kstack:` line before the banner. |

### The P1's own close: the loom join, and the gate row it bought (2026-09-22)

Fixed on the operator's ratified sequencing (the loom fix first, then identity).
`loom_free` now BLOCKS on a new `Loom.sqpoll_join` Rendez that the kthread's
terminal wakes after its `state=EXITING` + `sqpoll_exited` release stores, in
the same masked window -- so a joiner that observes the flag observes EXITING,
which is what `thread_free`'s not-RUNNING gate needs.

**It was not latent.** `usr/loom-smoke`, which joey spawns on every boot, has
carried an EL0 SQPOLL consumer since `15796866` (2026-09-03), so every `-smp 1`
boot for 19 days hung at its exit. One-variable control:

| | pre-fix `-smp 1` | post-fix `-smp 1` |
|---|---|---|
| last log line | `loom-smoke: PASS`, then silence | `joey: /loom-smoke reaped status=0` |
| boot banner | **never** (120 s) | present |
| suite | never completed | **1616/1616**, 5/5 boots |

**Nothing could see it**, and that bought the durable part: every boot gate ran
four or eight CPUs, so the one configuration where a thread spinning on another
THREAD's write cannot be rescued by a peer was the one configuration nothing
booted. `default-smp1` is now `ci-smp-gate.sh`'s first row. A peer CPU is a
rescue mechanism as well as extra concurrency, and a hazard a rescue mechanism
hides is one the matrix can no longer observe.

## Remaining work (in order; BROWSER-DESIGN section 9)

| Phase | What | Exit |
|---|---|---|
| **B-0** | `JSCOnly` cross-build, JIT off (asm LLInt + IPInt). Source lives OUTSIDE this repo (a `webkit-thylacine` fork beside `llvm-thylacine`); the repo carries the build wiring and patches. | `jsc` runs on the device; the measured list of P1 gaps |
| **B-1** | P1, the anonymous-memory surface. **Scripture LANDED 2026-09-23** (O-1 resolved; ARCH 6.5); five gated chunks follow in order: | |
| B-1a | **LANDED 2026-09-23 *(pending)*.** permissions: `burrow_reserve` (124) + `burrow_protect` (125) + `PROTECT_SEAL`; the phenotype `mprotect` row + exact `PROT_NONE`/`PROT_READ` mints; `cow.tla` extended FIRST behind `ALLOW_PROTECT` (bugs 4-6; additive by measurement, `cow_protect` 10636 states). Built beyond the letter: multi-mapping ranges all-or-nothing + a merge pass (ARCH 6.5 amended as built); the fork's per-Burrow clone dedupe; the eager-ANON fork share keyed on the ceiling; the lazy-piece detach uncharge. 1632/1632 kernel tests; `/protect-probe` + `/protect-guard-child` at boot. Audit-bearing. | probes with REDs; the `burrow_protect(X)` deny path; SMP gate; audit closed |
| B-1a' | capacity: range detach; the charged sparse `filepages`; the I-32 default = RAM minus a reserve; the >256 MiB detach refusal. Audit-bearing. | a 4 GiB reservation round-trips; the reserve holds under a memory bomb; SMP gate; audit closed |
| B-1b | Pouch: `mprotect` / `madvise` / partial `munmap` / real pthread guards; stack 8 MiB + auxv extent. | `pouch-hello-*` legs incl. a guard FAULT; the witness RED on the old libc |
| B-1c | native: `dlmalloc-rs` replaces the fixed 4 MiB heap; the two-substrate witness. | the page count rises past 4 MiB and FALLS after free, sabotaged once |
| B-1d | dlopen: PT_INTERP for native execs; `burrow_map_file`; driver `-shared` / PIE / `-dynamic-linker`; `libc.so`; ldso boundary-line. Before B-3. | a Pouch `.so` loaded on the device; the two deny paths |
| **B-2** | The JIT: JSC's separated WX heap on `SYS_JIT_CREATE` + `SYS_ICACHE_SYNC`; `CAP_JIT` clearance. Audit-bearing (I-42/I-12). | benchmark with JIT tiers; deny-path probe (no `CAP_JIT` -> interpreter, never RWX) |
| **B-3** | P3: ICU, FreeType, HarfBuzz, sqlite, libxml2, png/jpeg/webp, libpsl, curl, OpenSSL; fontconfig decision (O-4). | each library's tests under Pouch |
| **B-4** | P2: its own design document (O-3: generalise the Weft share gate vs Mycelium), the primitive, then WebKit's `Platform/IPC` + `SharedMemory` backend. Audit-bearing (I-4). | two-process message + shared-bitmap witness |
| **B-5** | WebCore + WebKit2 headless, `PORT=Thylacine` modelled on PlayStation; P5 (EGL) resolved here; O-2 (which WebKit line to track) measured here. | `WKPagePaint` renders a local page to a PNG on the device |
| **B-6** | The chrome on Tapestry; P6; the constructed namespaces with deny-path probes. | a TLS page in a tile; content Proc proven unable to open `/net` |
| **B-7** | Hardening, fuzz posture, the owed invariant ENFORCED, the Operator's Manual section. | arc close |
| **R** | **aux**: Rust `std` for Thylacine, the crate tail, then Servo. Brief: `docs/handoffs/041-rust-std-track-to-aux.md`. aux's design `docs/RUST-STD-DESIGN.md` was RATIFIED by the operator 2026-09-21 (on `aux-3` @`4cce758d`, not yet on `main`): target `aarch64-unknown-thylacine`, family unix over Pouch; and **O-5 decided: `std`-on-Pouch is a sanctioned THIRD substrate for new first-party programs, not ports only** -- an ARCHITECTURE 3.5 amendment that main owes. R-0 (the target JSON, the `libc` module, the `std` arms) is in progress; it consumes Pouch 0033/0034/0035 from `main`. | a `std` hello built by cargo and run on the device |

## Exit criteria status

- [ ] A page loads over TLS from the network and renders in a Halcyon tile.
- [ ] JavaScript runs with a JIT under strict W^X (no RWX page ever exists).
- [ ] A content Proc cannot name `/net`, `/srv`, `/proc` or `/dev` (deny-path probe).
- [ ] The owed section-28 invariant is allocated, then ENFORCED.
- [ ] The Operator's Manual has a browser section.

## Trip hazards

- **Effort is `xhigh`, by the operator's vote, for the whole arc.** Say so in
  every audit-bearing commit body; do not re-ask.
- **The permission surface is `burrow_protect` under a mint-time ceiling (ARCH
  6.5, ratified 2026-09-23; BUILT at B-1a as `SYS_BURROW_PROTECT` 125, with
  `SYS_BURROW_RESERVE` 124), not `mprotect`.** X is never a target of it and no
  capability gates it -- do not add either. The native detach is still
  exact-match per mapping: after a protect has cut a reservation into pieces,
  detach them piece by piece (B-1a' brings the range form).
- **`SYS_WEFT_SHARE` is gated to the driver tier on purpose** (Weft-7 F1,
  `kernel/syscall.c`). Read that audit before proposing to lift it.
- **Thylacine links statically BY DEFAULT; `dlopen` arrives with B-1d as an
  opt-in dynamic link against `libc.so`** (ARCH 6.5 "Dynamic loading"). WebKit
  is LGPLv2: keep the build reproducible from published source so LGPL
  section 6 is met.
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
