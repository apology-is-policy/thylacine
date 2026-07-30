# LLVM-DESIGN.md — the on-device LLVM keystone arc (working name: "Clade")

**Status: BINDING DESIGN — SIGNED OFF 2026-07-23** (user adopted every §14
lean verbatim: *"I'll take your recommendation on all"*). Research pass
complete (tree ground-truth + external verification, §3); no code. The §15
adoption edits (ROADMAP / NOVEL / ARCH §28+§25.4+§6.6 / CLAUDE.md) land with
the scripture commit on the main track. Arc direction (2026-07-23): *"I think
we should do it, especially to unlock C++, Rust, and Mesa/graphics future."*
The JIT-capability invariant is **I-42** — I-41 was reserved by
`ADVANCED-GO-DESIGN.md` AG-2 (software-breakpoint isolation) between draft
and signoff.

**Working name: "Clade"** — a clade is a common ancestor plus *all* of its
descendants. LLVM is the common ancestor of the modern compiled-language
family (C++, Rust-release, Swift, Zig, Julia, Flang…) and of llvmpipe's shader
JIT; porting the ancestor admits the whole clade. The name sits squarely in
the project's taxonomic register (lineage, taxon, holotype). *Adopted
2026-07-23 (F1).*

---

## 1. Mission

Port the LLVM ecosystem to run **on** Thylacine, as one keystone investment
with four riders:

1. **The C++ runtime + ecosystem via Pouch** — libunwind/libc++abi/libc++ on
   the pouch sysroot. Today *no* C++ program is portable (pouch is musl/C
   only); this opens the category, and it is the hard prerequisite of
   everything below.
2. **The on-device optimizing C toolchain** — clang + lld (+ make/ninja), the
   already-committed ROADMAP D3 / #67 deliverable ("the single largest
   Phase-8 pole"), including its self-hosting / build-storm (W2) story.
3. **Software GL for the graphics arc** — Mesa's llvmpipe (LLVM shader JIT)
   via the gallium OSMesa frontend, realizing TAPESTRY.md's own pre-committed
   route ("port Mesa (swrast / llvmpipe via Pouch), not a hand-rolled GL; GL
   is v1.1+ and never blocks Halcyon") — and, as its enabler, **realizing
   JIT-ON-WX-DESIGN.md** (the dual-mapped code Burrow + `SYS_ICACHE_SYNC` +
   `CAP_JIT`), which finally has its first consumer.
4. **Release-grade Rust** — a cross `aarch64-thylacine` rustc target with
   std-over-pouch now; on-device rustc/cargo as a staged follow-on once the
   LLVM libs are resident.

Plus a near-free fifth: **clangd** extends the Nora IDE to C/C++ over the
same parley LSP client gopls already rides (lldb-dap is the staged debugger
sibling).

The framing discipline: LLVM is not "a compiler port," it is the **keystone
dependency** whose cost is already ~80% committed (clang/lld via Pouch is D3
scripture; clang is C++, so the C++ runtime port is forced either way) and
whose riders are near-free on that same investment.

---

## 2. Ground truth — what the tree provides, what it owes

Verified 2026-07-23 (greps + reference docs). The substrate is largely
*already built for this*:

| Already in place | Why it matters here |
|---|---|
| **REVENANT** (I-36) file-backed demand-paged exec + qid-keyed shared Image cache; no binary-size cap | clang-sized static binaries load; one Image's text is shared RO across every process running it — the build-storm RAM story |
| **Overcommit** (`SYS_BURROW_ATTACH_LAZY`/`DECOMMIT`) | ARCH §29 names "a future on-device LLVM/clang" as a motivating consumer verbatim; reserve-heavy allocators just work |
| **Pouch** (musl 1.2.5 + 26 boundary patches; triple `aarch64-thylacine`; sysroot `build/sysroot`; compiler-rt builtins present) | the ported-code tier the whole arc builds on |
| Spawn family (`SYS_SPAWN_FULL_ARGV` + fds), pthreads/torpor, `/env`, vDSO clocks, positioned I/O, Larder + POUNCE + CF-3 128 KiB I/O | process/thread/time/FS substrate, already go-build-hardened |
| **JIT-ON-WX-DESIGN.md** (adopted 2026-06-19, post-v1.0, "NO v1.0 consumer") | the JIT mechanism is fully designed and W1.5-precedented; llvmpipe is its first consumer — this arc pulls it forward |
| TAPESTRY.md §"GL" + `142-sdl-port.md` | the Mesa/llvmpipe route is *existing scripture*, not new direction; the SDL backend is the delivery vehicle |
| Stratum dedup + the mixed-encryption tooling-dataset design | the toolchain dataset (`/clade` or `/llvm`, the `/goroot` bake precedent) is the intended encryption=off candidate |

What the tree **owes** (the honest gap list — each becomes a workstream item):

- **No C++ runtime.** No libc++/libc++abi/libunwind anywhere; pouch is C-only.
- **No `posix_spawn`.** No pouch patch provides it, and musl's own
  implementation is `clone()`-based — structurally dead on Thylacine
  (`__NR_clone` = the `0xFFFF` ENOSYS sentinel). Must be *rewritten* onto
  `SYS_SPAWN_FULL_ARGV`, not merely enabled. (Shared prerequisite with the
  planned git port; also what `make`/`ninja` job-spawning converts onto.)
- **No dirent boundary.** musl `readdir` → `getdents64` → no patch maps it to
  `SYS_READDIR`. LLVM's `sys::fs::directory_iterator` and `std::filesystem`
  both need it. (Go sidesteps via its own runtime; C ports have simply never
  listed a directory yet.)
- **`environ` delivery unverified.** The exec frame carries an envp slot; who
  populates it for pouch programs is unconfirmed — Go reads `/env` directly.
  A `pouch-env` boundary line (populate `environ` from `/env` at crt startup)
  is the likely deliverable; verify at CL-0. (`make`/clang want `PATH`,
  `TMPDIR`.)
- **`wait4` stub.** The owed pouch `wait4 → SYS_WAIT_PID` wiring (recorded at
  U-7-pre) becomes load-bearing for build tools.
- **No file-backed `mmap` — by design, and it stays that way.** LLVM reads
  inputs via `MemoryBuffer` (mmap-preferring) and lld writes output via
  `FileOutputBuffer` (mmap-preferring). Both have / get non-mmap paths
  (§6.3); the conviction is not re-litigated.
- **I-32 vs `-O2`:** `PROC_PAGE_MAX` = 65536 pages = **256 MiB** per non-TCB
  Proc (`proc.h:110`). A real `clang -O2` TU can exceed it and a clang-sized
  *link* certainly does. Policy fork [F4], §7.
- **ARCH §6.6** still carries the older "pkey-shaped syscall" JIT note; the
  JIT-ON-WX dual-map design supersedes it. Reconciled in the adoption edits.

---

## 3. Research — heritage, SOTA, verified externals

**Heritage (Plan 9).** The heritage conviction this arc serves is
**self-hosting** — Plan 9's founding property was that the system rebuilt
itself in minutes, on itself. Plan 9 deliberately had *no C++*; Thylacine
diverges knowingly: native code stays C99 + no_std Rust (the native/APE
split holds — nothing native links C++), but the pouch *ports* tier exists
precisely to reuse well-written foreign software, and the modern
self-hosting vehicle is LLVM. The divergence is confined to the ports tier.

**SOTA precedents (verified 2026-07-23):**

- **SerenityOS** — the existence proof at hobby-OS scale: an LLVM patchset
  (target triple + clang driver + libc++ support) maintained since Aug 2021,
  since **upstreamed** (`clang/lib/Driver/ToolChains/Serenity.h` is in the
  upstream tree), tracking LLVM 19.x in-tree, with a live "SerenityOS on
  SerenityOS" self-host checklist. Their Zig port rides the LLVM port — the
  clade effect, observed in the wild.
- **Haiku** — upstream LLVM triple + driver; long-standing clang support.
- **Alpine Linux** — the whole LLVM/clang/rustc stack shipping on musl:
  proof that no glibc-ism is load-bearing anywhere in the stack.
- **Redox** — rustc running on a new, non-Linux OS via a libc shim (relibc):
  the rustc-on-new-OS precedent for CL-8.
- **Mesa llvmpipe** — gallivm gained an **ORC JIT** backend (MR 17801;
  MCJIT is deprecated upstream and closed to new architectures), and
  llvmpipe is a maintained GL 4.6 software rasterizer. **Gallium OSMesa**
  survives (classic OSMesa retired in Mesa 21.0; the gallium frontend —
  "renders using softpipe or llvmpipe and copies out at glFlush() time" —
  is exactly the delivery shape §9 needs: no DRI, no GBM, no dmabuf, no EGL).
- **llvm-driver multicall** — real (`LLVM_TOOL_LLVM_DRIVER_BUILD=On`; tools
  opt in via `GENERATE_DRIVER`; clang + the binutils-shaped tools confirmed
  members). **lld's membership: VERIFY at CL-0** (known limitations:
  multi-dispatch symlinks, `cl::opt` collisions). Fallback = two static
  binaries (the `llvm` multicall + `lld`) — the RAM story barely changes,
  the hot storm binary is clang.

**The novel surface.** Mostly this arc is deliberate *integration*, not
invention — the point is the keystone. Two genuinely novel realizations:

1. **JIT-as-a-capability, realized** (JIT-ON-WX promoted from a banked
   post-v1.0 candidate to a built mechanism with a real consumer): the first
   OS where a live GL shader JIT runs under system-wide strict W^X with the
   code-emission right an explicit, non-rfork-grantable capability.
2. **The ORC fit**: ORC's `MemoryMapper` abstraction separates *working*
   addresses (where the JIT writes) from *execution* addresses (where code
   runs) — designed for out-of-process JITs, but it is **exactly the
   dual-mapped code Burrow's shape** (`VA_w`/`VA_x` aliases of one physical
   region). A `DualMapMemoryMapper` over the code Burrow slots into ORC's
   own seam; no LLVM surgery, no `mprotect` emulation.

---

## 4. Version pin + vendoring

- **LLVM 22.x**, matching the host toolchain (clang 22 / lld 22 build the
  kernel today) — one-version discipline: the cross-compiler that builds the
  device toolchain IS the device toolchain, one major, one bug surface.
  Upgrade cadence follows the host pin. [FORK F2]
- **Mesa**: **pinned at `mesa-26.1.6`** (resolved at CL-7 entry, §16.19;
  the earlier "a current 25.x-era release" was written a year before the
  arc reached CL-7). The CL-0 spike's OSMesa premise was corrected in
  §16.6 and settled by measurement in §16.19: the gallium OSMesa frontend
  is resurrected in `mesa-thylacine` (6 files / 1,392 lines, zero C
  changes), and ORC-gallivm requires **both** `-Dllvm-orcjit=true` and an
  `LLVM_ENABLE_RTTI=ON` LLVM.
- **Vendoring: sibling fork repos**, the `go-thylacine` precedent —
  `~/projects/llvm-thylacine`, `~/projects/mesa-thylacine`,
  `~/projects/rust-thylacine`, each a pinned-SHA fork carrying a small,
  enumerable Thylacine delta. NOT in-tree vendoring (the musl model): the
  musl patch-series pattern is right for a ~100-file libc, wrong for a
  130+ MB monorepo. Upstreaming (the Serenity path: triple + driver are
  upstream-shaped by construction) is a post-v1.0 aspiration, same as Go's.

---

## 5. The toolchain shape (decisions)

1. **Static-only, one multicall binary.** Native no-dynamic-linking stands
   (REVENANT §7 conviction untouched). The device toolchain is the static
   `llvm` multicall (clang + tools; lld in-or-beside per the CL-0 check) +
   a static `clangd`. One multicall = ONE REVENANT Image whose text every
   concurrent compile in a `make -j` storm shares — the RAM answer. Disk:
   Stratum's content-defined-chunk dedup absorbs the static duplication
   across any binaries that don't fold in. [FORK F3]
2. **A real `Triple::Thylacine` + clang `ToolChain` driver + lld ELF
   default.** Retires the `pouch-clang`/`pouch-ld` wrapper pair by fixing
   the root cause they work around (clang's link driver mis-selecting the
   Darwin toolchain for an unknown OS). Upstream-shaped from day one
   (Serenity/Haiku pattern). The pouch sysroot layout is unchanged.
3. **`-fintegrated-cc1` (the default since clang 10)** — a compile spawns
   zero children; the driver spawns only the linker. Storm spawn-rate is
   `make`/`ninja`'s per-TU spawn + one lld per link, all via CL-1's
   `posix_spawn`.
4. **`LLVM_ENABLE_PLUGINS=OFF`, `DynamicLibrary` stubbed** — no `dlopen` on
   the native tier, and none needed.
5. **LLVM itself builds `-fno-exceptions -fno-rtti`** (its upstream
   default) — the first huge C++ binary does not depend on the unwinder;
   the *runtime stack* (CL-2) still ships full EH/RTTI for general C++
   ports.
6. **The C++ runtime** = libunwind + libc++abi + libc++, static, built via
   `LLVM_ENABLE_RUNTIMES` cross against the pouch sysroot (the Alpine-proven
   musl pairing). Provers: exceptions, RTTI, threads + TLS destructors
   (`__cxa_thread_atexit` — musl provides), iostreams, `std::filesystem`
   (drives the dirent boundary line).
7. **The toolchain lives in the pool** (`/clade` dataset, the `/goroot` bake
   precedent), host-baked by `tools/build.sh`; the intended first
   mixed-encryption (encryption=off) tooling dataset.

---

## 6. OS-boundary posture

### 6.1 Process creation (CL-1)

`posix_spawn`/`posix_spawnp` rewritten onto `SYS_SPAWN_FULL_ARGV` (+ the
file-actions subset onto the spawn fd-list: `adddup2`/`addopen`/`addclose`),
replacing musl's clone-based body. `wait4 → SYS_WAIT_PID` wired (the owed
stub). `make` (GNU make `job.c` fork→spawn conversion) and `ninja` (C++;
`subprocess-posix.cc` conversion — and the second C++ prover after the CL-2
smoke) ride it. Explicit synergy: this workstream is byte-for-byte the git
port's prerequisite too.

### 6.2 The environment + directories (CL-1/CL-2)

`pouch-env`: populate `environ` from `/env` at crt startup (verify the
current envp state at CL-0 first — /env stays the source of truth, environ
becomes the POSIX-shaped read-only snapshot, the Go `goenvs` analog).
`pouch-dirent`: `getdents64 → SYS_READDIR` translation.

### 6.3 No file-backed mmap — the design-true detour

The conviction stands (network transparency; ARCH §6.5). The toolchain
routes around it in three bounded places, all in the fork's Support layer:

- `MemoryBuffer` input reads: prefer/force the read path (the non-mmap
  branch exists upstream) — the Larder page cache + CF-3 128 KiB reads make
  this cheap, and build inputs are re-read hot.
- `FileOutputBuffer` (lld's output): force the in-memory buffer + a final
  `write()` — the output image is heap-resident during the link (§7 memory
  math accounts for it).
- No `mmap(PROT_EXEC)` anywhere in the toolchain path (the JIT is §8's
  separate, capability-gated story).

### 6.4 Signals, misc

The pouch signal subset (0007) suffices (`SIGINT`/`SIGTERM`/`SIGCHLD`
shapes); LLVM's crash-handler/`Signals.inc` is stubbed to the extinction-
adjacent minimal form (print + exit — the Halls do the real forensics).
`sysconf`/`getrandom`/TLS already have boundaries.

---

## 7. Memory: the numbers + the I-32 fork

Honest planning numbers (order-of-magnitude, verified against common
experience, re-measured at CL-0/CL-5): a typical `clang -O2` TU peaks
150–600 MiB RSS with template-heavy outliers >1 GiB; linking a clang-sized
static binary with lld runs ~2–4 GiB, plus the in-memory output image
(§6.3). The dev VM tiers (4/8/16 GiB) absorb this with `-j` clamped; the
overcommit model is the right substrate and already scripture-motivated by
this exact consumer.

The collision is **I-32's per-Proc floor**: `PROC_PAGE_MAX` = 256 MiB.
Options (F4 — resolved: **(b)**):

- **(a)** Raise the default (e.g. to 2 GiB): one constant, but weakens the
  fork-bomb floor ~8× for every Proc.
- **(b) — ADOPTED**: a **spawn-time page-budget** (a `SYS_SPAWN_*` budget arg
  or perm), defaulting to today's 256 MiB, raisable per-child up to a
  **global hard cap** (e.g. 4 GiB) that preserves the box-cliff protection.
  A user raising their own compiler's budget DoSes only their own budget;
  the per-user *aggregate* quota remains the recorded I-32 seam. Kernel
  change → audit-bearing, its own focused round; composes I-32 without
  renumbering.

### 7.1 As-built (CL-5) — measured, then built

**The estimates above were replaced by measurement before anything was
built.** The instrument is `Proc.page_peak` (the anon high-water, the Linux
`VmHWM` analog), surfaced as `peak:` in `/proc/<pid>/status` and read by the
clade gate. It is exact rather than sampled: it is stamped under the same
`vma_lock` that makes `page_count` exact, and a read of a ZOMBIE (which can no
longer charge) reports the final value — which is the only way to measure a
process that lives 200 ms.

On-device, via the CL-5 probe in the clade gate:

| invocation | peak anon |
|---|---|
| `clang++ -O2 hello.cpp -o hello` (driver; spawns cc1 + ld.lld) | 128 pages (0.5 MiB) |
| `clang++ -O2 -c hello.cpp` (**cc1 in-process**) | 11441 pages (44.7 MiB) |
| `clang++ ... hello.o -o hello2` (driver; spawns ld.lld) | 127 pages (0.5 MiB) |
| `clang++ -O2 -c stress.cpp` (**cc1 in-process**, template-heavy) | **64066 pages (250 MiB)** |

Two things follow, and the second is the one that mattered:

1. **Only the in-process (`-c`) figure measures a compiler.** The driver forks
   cc1 and ld.lld, so its own peak is half a megabyte — a fork-and-wait shell.
   Any future measurement of this path must use `-c` or reach the grandchild.
2. **A 1959-byte template-heavy TU costs 250 MiB — 97.8% of the default
   budget.** It fits, with 5.7 MiB to spare. A *real* project TU does not: on
   the host, `DAGCombiner.cpp` / `AArch64ISelLowering.cpp` / `SemaExpr.cpp`
   measure 735 / 798 / 867 MiB RSS, and the device's anon fraction (0.46 at
   hello, 0.70 at stress — it rises as heap overtakes text) puts them at
   roughly **500–650 MiB, 2–2.5× the default**. The 4 GiB hard cap proposed
   above survives contact with the numbers: it covers a real TU with room, and
   a clang-sized lld link at 2–4 GiB.

**Note the gate does not itself hit the cap**: joey is `PRINCIPAL_SYSTEM` and
`rfork` inherits the principal, so the gate's clang++ is resource-*exempt* and
never consults a budget. The measurement is what a real (non-exempt) user's
build would be charged. That exemption is exactly why this collision has been
invisible so far.

**The mechanism.** `Proc.page_budget` (pages) replaces the hardcoded constant
in `proc_page_charge`, seeded to `PROC_PAGE_MAX` so an untouched Proc is
byte-identical to pre-CL-5, and bounded by `PROC_PAGE_HARD_MAX` = 4 GiB.

It is **inherited** across rfork/spawn, and that is load-bearing rather than
incidental. The chain is `ut → make → clang → cc1`; `make` and `clang` are
*pouch ports* calling `posix_spawn`, with no notion of a Thylacine budget. A
spawn-time-only budget could therefore never reach cc1 — the process that
actually needs the memory — without patching every link. Inheritance means one
raise at the build root covers the whole tree. (This is why Linux rlimits are
inherited across fork/exec too.)

Authority is split by direction:

- **Lowering** a child's budget needs none — monotonic reduction, the I-2
  shape, and a free sandboxing primitive.
- **Raising** it above the spawner's own requires
  `SPAWN_PERM_MAY_RAISE_PAGE_BUDGET`, gated exactly like the audited
  `SPAWN_PERM_MAY_POST_SERVICE` one-hop delegation (console-attached, or an
  existing holder) so joey → login → shell → build-driver can carry it
  without any of them being console-attached.
- **Nothing** exceeds `PROC_PAGE_HARD_MAX`, authority or not. That is what
  preserves the box-cliff protection; graceful OOM
  (`proc_fault_terminate`) remains the real backstop.

An over-cap or unauthorized request **fails the spawn** rather than being
silently clamped — a clamp would hand back a budget the caller did not ask for
and hide the misconfiguration until it surfaced as an opaque OOM.

The ABI field claims the reserved `_pad_allow` slot in `sys_spawn_args`
(offset 92), so the struct does not grow and every existing caller — all of
which zero-fill — keeps the historical behaviour by construction: **0 means
inherit**.

v1.x seams: the per-user *aggregate* quota (the cgroups-equivalent, reading
the same counters) is unchanged as the recorded I-32 seam; and a shell-level
ergonomic for requesting a raise (a `ulimit`-shaped verb) is unbuilt — today
the raise is a spawn-time decision made by whatever launches the build.

### 7.2 The build storm (CL-5) — as-built

The charter's second half: *`make -jN` of a nontrivial project completes on
the device; numbers recorded, no committed target.*

**The project is GNU make 4.4.1 building itself.** Chosen over the charter's
sketched `zlib` for three reasons, each of which turns out to matter more than
the name on the tin:

1. It is **already Thylacine-configured** — `usr/ports/gnumake/config.h` is the
   hand-derived autoconf output. Thylacine has no POSIX shell, so a project
   whose build begins with `./configure` cannot be built on-device at all.
2. It is **self-referential**: `/bin/make` (cross-built on the host) builds a
   new make from source, and the result is an *executable we can run*.
3. **Zero new vendoring**, and the object census is literally shared with
   `build_gnumake` (`GNUMAKE_SRC_OBJS`/`GNUMAKE_LIB_OBJS` in `tools/build.sh`),
   so the storm builds exactly what the host cross-build builds. A hand-copied
   object list would have rotted at the first census change.

`stage_storm` materializes `/storm` (sources + a **generated** Makefile) and
`stage_clade` gained a third multicall copy, a **C-mode `clang`** — a `clang++`
copy sets `CCCIsCXX` and would compile the storm's `.c` sources as C++.

Every recipe is **shell-free by construction**: GNU make's metachar set is
``#;\"*?[]&|<>(){}$`^`` (`src/job.c` `sh_chars_sh`), so the three
`-D LIBDIR='"..."'`-style flags the host build passes had to move into a
generated `-include storm-defs.h` — the double quote alone would have dragged
every recipe onto a `/bin/sh` that does not exist.

**The proof chain is four links, because only the last two are hard to fake:**
`make` exits 0 → the artifact is ~380 KiB → **it runs** (`--version`) → **it
drives a build of its own** (a recipe executes, output verified). A toolchain
that miscompiles can still satisfy the first two.

#### Measured (2026-07-29, `-smp 4`, 2 GiB, HVF)

| | |
|---|---|
| First `clang -c` of the boot (cold Image cache) | **1158 ms** |
| Per-TU once clang is resident | **~79 ms** (2759 ms / 35 TUs) |
| Peak anon for the largest TU (`main.c`, 121 KiB) | **2823 pages / 11 MiB** |
| `make -B -j1` (35 TUs + link) | **2759 ms** |
| `make -B -j4` (35 TUs + link) | **1033 ms** |
| Parallel speedup on 4 vCPUs | **2.67×** |
| Artifact | 380304 B (host cross-build: 380232 B) |

Both passes run `-B` (always-make): the pool persists across boots, so without
it the second boot would find every object current and exit 0 having compiled
nothing — a vacuous pass indistinguishable from a real one.

The `-j1` control is kept permanently, not just as a baseline: it is the
differential that **diagnosed #96** (below), and if the storm ever reddens
again it says immediately whether parallelism is the variable.

Note the first-invocation cost (1158 ms) versus the resident cost (~79 ms) —
a **~15×** spread. That is the REVENANT Image cache paying for the 95 MiB
multicall, and it is why the per-TU figure is only meaningful once warm.

#### Host-vs-device par (2026-07-29) — the device is within ~1.3× on warm serial compile

The storm Makefile is fully parameterized (`CC`/`S`/`L`/`O`/`CFLAGS`/`LDFLAGS`),
so the *same* 35 TUs can be driven host-side with the *same* fork clang at the
*same* target triple and flags. What differs is only the execution environment:
native macOS versus Thylacine under HVF, and APFS versus Stratum.

**Both sides on a clear host** (median of 3, `-B` every pass, warm pass
discarded):

| | host | device | ratio |
|---|---|---|---|
| `make -B -j1` | **2138 ms** | 2759 ms | **1.29×** |
| `make -B -j4` | 652 ms | 1033 ms | 1.58× |
| per-TU warm (total / 35) | **61.1 ms** | **78.8 ms** | **1.29×** |
| artifact | 380232 B | 380304 B | — |

`-j1` is the number to quote: it is pure serial compile throughput with no
scheduler or core-count confound. `-j4` compares 4 jobs on 8 host cores against
4 jobs on 4 vCPUs, so the host has headroom the device does not (host
self-speedup 3.28× versus the device's 2.67×) — it measures the *configuration*,
not the platform.

**A correction to this section's first version.** It reported 1.18× / 1.87×
from a run taken while a sibling tree's VM held ~2.7 of 8 cores, and justified
the `-j1` row by claiming "both `-j1` figures moved <3% from their clear-host
values." That was measured for the *device* (2830 vs 2759 = 2.6%) and **assumed
for the host**. The clear-host run falsifies it: the host `-j1` moved **12.3%**
(2400 → 2138), so the contended ratio was flattering the device by ~0.1× and
the honest serial figure is 1.29×, not 1.18×. The pairing discipline was right
— an unpaired ratio would have been worse — but a *symmetry* asserted across
the pair without measuring both halves is the same unverified-claim failure it
was meant to prevent. `-j4` moved as predicted (1.87 → 1.58).

Decomposing the host side explains the near-parity rather than leaving it
surprising:

| host measurement | |
|---|---|
| `clang --version` (pure process startup) | 18.9 ms |
| empty TU `-c` | 22.6 ms |
| `main.c` (largest TU) `-c` | 168.6 ms |

So ~19 ms of every host invocation is startup — ~31% of the 61.1 ms average
TU. The device pays the same shape but amortizes its 95 MiB multicall through
the REVENANT Image cache after the first invocation. (These three were taken
under contention and are not re-measured; they are used only for the *share*
of a TU that is startup, which contention moves both terms of.)

**The interesting comparison is against the go build's 45–53×** (task #34).
Same device, same era, two orders of magnitude apart in ratio — because the
gap was never CPU. Under HVF the guest executes on the same M2 cores; once
clang is resident this workload is compute-bound and lands at near-native. The
go build's gap is FS-metadata-bound (~75k walk/getattr/clunk round trips
versus 35 TUs' handful of opens). This measurement isolates that from an
independent direction and corroborates task #60's conclusion — the levers are
Image-cache retention and metadata round-trip reduction, not raw compute.

One honest limit remains. **HVF is virtualization on the same silicon**, so
this is a virtualization-overhead number, not a different-hardware number — the
bare-metal comparison is Lazarus's to make.

#### What the storm found: #96 (fstat on a pipe)

The storm's first run failed in a way no existing gate could have caught, and
the shape is worth recording.

`make -j4` had **job 1 succeed and every concurrent sibling exit 1 with no
diagnostic at all**, in under 180 ms. `-j1` over the identical cold tree
completed and produced a working binary. That one differential localized it to
parallelism, and from there the mechanism is a three-link chain:

1. GNU make gives the real stdin to only **one** job; every other concurrent
   job gets `get_bad_stdin()` (`src/posixos.c`) — the read end of a broken
   pipe — dup2'd onto fd 0.
2. clang's `FixupStandardFileDescriptors` fstats fds 0/1/2 at startup and
   treats a **non-EBADF failure as fatal**. This is CL-4 masking layer 3,
   already fixed twice: once for the console, once for `/dev`.
3. `devpipe` had **no `.stat_native`**, and `spoor_stat_native` returns -1 when
   the slot is NULL. So fstat on a pipe failed, and clang died before its
   diagnostic engine could say why.

**The pipe was the third door, and nothing had ever opened it** — no pouch
program had had a pipe on a standard fd at startup until now. POSIX *requires*
`fstat(2)` on a pipe to succeed and report `S_IFIFO`, so this was a real gap
well beyond the storm: any program in a shell pipeline is on that path.

Pointedly, **the CL-1c-2 `make -j3` gate could not have caught it**: its
children are `/bin/cp`, which produce no output and are verified by file
content — a silently-broken concurrent child looks exactly like a working one.
It took a child that *talks* to make the failure visible.

Fixed by `devpipe_stat_native` (`T_S_IFIFO | 0600`, size 0), plus a monotonic
`qid.path` stamped into **both** ends so fstat reports one inode per pipe and
distinguishes two pipes — a same-inode-for-every-pipe report is the kind of
latent wrong answer that surfaces much later inside someone's file-identity
comparison. Regression `pipe.fstat_reports_fifo`, revert-probed.

The completeness sweep (the CL-4 F2 lesson — that fix covered only one door)
found **8 Devs lack `.stat_native`**, of which exactly two are EL0-reachable on
a live fd: `pipe` (fixed) and `devnotes` (task #97 — fail-closed, no consumer,
and its `st_mode` type is a real ABI choice not worth guessing at mid-storm).
`null`/`zero`/`full`/`random` are reachable only through `/dev`, whose `devdev`
owns the Spoor and has `stat_native`; `devcap` and `devnone` are mounted
nowhere.

---

## 8. The JIT capability (realizing JIT-ON-WX; proposed I-42)

Pulled forward, depth-first, because llvmpipe is the first consumer:

- **Kernel half** (CL-7k): the dual-mappable **code Burrow** (one BURROW
  attached RW at `VA_w` and RX at `VA_x` in the same Proc — W^X-clean at PTE
  granularity, the W1.5 self-patcher's own discipline turned outward) +
  **`SYS_ICACHE_SYNC(range)`** (the `dc cvau / ic ivau / dsb ish / isb`
  dance, lifted to a syscall) + **`CAP_JIT`** (elevation-only,
  non-rfork-grantable, the `CAP_HW_CREATE` class). Exactly
  JIT-ON-WX-DESIGN.md; no design drift.
- **Userspace half**: `libthyla_rs::jit` (create → `(writer_ptr, exec_ptr)`
  → emit → `icache_sync` → call) and, for LLVM, a **`DualMapMemoryMapper`**
  implementing ORC's MemoryMapper over the code Burrow — the
  working/execution address split is ORC's native shape (§3).
- **Obligations**: JITed code must be BTI/PAC-well-formed on hardened
  silicon (gallivm modules get the branch-protection attrs); `CAP_JIT`
  controls who-may-emit, the *namespace* controls what emitted code can
  reach (the JIT-ON-WX caveat #1, restated as policy: llvmpipe runs inside
  the app's own Proc — no new authority).
- **Proposed invariant I-42** (minted at the scripture commit; **I-41 is
  reserved** by `ADVANCED-GO-DESIGN.md` AG-2's software-breakpoint-isolation
  invariant — a sibling on the same W^X/I-36-adjacent ground: AG-2 governs
  the *kernel's* one sanctioned text mutation [debug COW-break], I-42 governs
  *userspace's* one sanctioned code-emission path [the capability-gated
  dual-map]): *executable
  JIT memory exists only as a code-Burrow RX alias whose content arrived via
  the paired RW alias + an explicit `SYS_ICACHE_SYNC`; no PTE is ever W∧X;
  creation is `CAP_JIT`-gated and non-heritable.* Audit-trigger row added
  (W^X-adjacent — prosecute hard). **Spec posture** [FORK F8]: lean
  prose+audit (the W1.5 precedent — same mechanism, already trusted
  in-kernel), escalating to a focused spec only if the CL-7 design pass
  surfaces genuine SMP subtlety (the icache/publish protocol is the
  candidate).
- ARCH §6.6's "pkey-shaped syscall" note is superseded → reconciled at
  adoption.

---

## 9. Mesa / llvmpipe delivery (CL-7)

The path of least invention, per the OSMesa verification:

1. **Mesa port via Pouch** (the C/C++ mix now buildable): gallium llvmpipe
   + the **gallium OSMesa frontend** — off-screen render into client
   memory, no DRI/GBM/dmabuf/EGL, ORC-gallivm over the §8 mapper.
   *(§16.6: upstream removed the OSMesa frontend post-design. RESOLVED at
   CL-7 entry — §16.19: the frontend is resurrected in `mesa-thylacine`
   at pin `mesa-26.1.6` and compiles with zero C changes; EGL surfaceless
   was rejected as structurally incompatible, not merely larger, because
   the DRI loader dlopens its driver and Thylacine is static-only. The
   delivery shape stands unchanged.)*
2. **SDL-GL glue**: `SDL_thylacine` grows a GL context path — OSMesa
   context rendering into (or blitted into) the surface's **weave**, then
   the existing tear-free `tpresent`. Stock SDL-GL programs recompile.
3. **Acceptance gate: GLQuake** (`tyr-glquake`) — the poetic echo of G-7:
   software Quake proved the 2D present path; GLQuake proves the GL stack,
   through llvmpipe, through the JIT capability, onto the same compositor
   scanout. Plus a gears-class smoke for CI.
4. **Stretch, cuttable**: lavapipe (Vulkan-conformant software Vulkan) —
   same libs, new frontend; explicitly NOT a gate.

Perf posture: llvmpipe on an M-series guest under HVF is comfortably
adequate for the acceptance class; no budget is committed beyond "GLQuake
is playable," measured honestly at CL-7 (the CHASE method — measure first,
no bolted-on chasing).

---

## 10. Rust staging (CL-8)

- **CL-8a (in-arc): the cross target.** `aarch64-thylacine` in
  `rust-thylacine` + **std over pouch** — rustc is *foreign* code, so its
  std belongs to the ports tier (the native tier stays no_std libthyla-rs;
  the Plan 9 split is not blurred) [FORK F6]. Key de-riskers: proc-macros
  and build scripts are HOST artifacts under cross-compilation — the
  no-dylib conviction is untouched by CL-8a; Redox/relibc is the precedent.
  Deliverable: a std Rust program cross-built on the host runs on-device.
- **CL-8b (staged follow-on arc): on-device rustc + cargo.** Needs the
  resident LLVM libs (this arc), lld (present), sparse-registry cargo over
  `/net` (the Go-modules precedent). The **proc-macro fork** (the sandboxed
  compat-dylib tier vs `watt`-style wasm — the capability-clean option and
  a NOVEL candidate) is surfaced at CL-8b's own design pass, not decided
  here. Cranelift remains an optional fast-debug backend, never a gate.

---

## 11. The IDE riders (CL-6)

- **clangd**: static (or in-multicall), spoken to over the existing parley
  LSP client — Nora gains C/C++ intelligence with near-zero client work
  (`compile_commands.json` from make/ninja or a thin generator).
- **lldb-dap** (stretch, cuttable to post-arc): the C/C++/Rust debugger over
  the same DAP client + kernel debug-fs, requiring an LLDB Process plugin
  (the `proc_thylacine` analog of Ambush's backend). Ambush already owns
  the Go story; this generalizes it when it lands.

---

## 12. Workstreams

| # | Scope | Gate / deliverable | Audit posture | Cut line |
|---|---|---|---|---|
| **CL-0** | Spikes + verify: Tier-2 static-musl clang run (syscall-gap census); lld-in-multicall; gallium-OSMesa + ORC state in pinned Mesa; environ/dirent ground truth; memory re-measure | a one-page findings addendum to this doc — **LANDED, §16** | none (read-only) | — |
| **CL-1** | The process substrate: `posix_spawn` rewrite + `wait4` + `pouch-env` + `pouch-dirent`; make + ninja ports. **CL-1a LANDED** (the FS/process wires: `0024`; §16.9). **CL-1b-0 LANDED** (pouch-env: `0025`; §16.10). **CL-1b core LANDED** (posix_spawn/wait4/dup2/pipe2: `0026`; §16.11). **CL-1c-1 LANDED** (the GNU make 4.4.1 port: `third_party/gnumake` + `usr/ports/gnumake` + `build_gnumake()`; `USE_POSIX_SPAWN` drives CL-1b, `MAKE_JOBSERVER` off; `/make --version` runs on-device; §16.12). **CL-1c-2 LANDED — THE CL-1c ARC IS COMPLETE** (the on-device `make -j3` gate: make drives CL-1b's posix_spawn/wait4 under parallelism; audit CLOSED 0/1/0/4 NOT dirty; §16.13). | **DONE:** `make -j3` runs a toy multi-TU build on-device (shell-free `/bin/cp` recipes) | **DONE:** boundary-line audit (the #68/#926 process-lifecycle lineage) — CLOSED 0 P0/1 P1/0 P2/4 P3, the P1 a surfaced pre-existing getcwd bug (tracked) | — (shared with the git port) |
| **CL-2** | The C++ runtime: libunwind + libc++abi + libc++ static into the sysroot; prover suite. **LANDED** (§16.14; `build_libcxx` via `LLVM_ENABLE_RUNTIMES` against the pouch sysroot from `$LLVMFORK`; `0027-pouch-remove` fixed the surfaced `remove(3)` gap). | **DONE:** `pouch-hello-cxx: ALL C++ WIRES PASS` (EH + RTTI + threads + TLS-dtors + iostreams + std::filesystem) on-device; boot OK, 0 EXT, suite 1196/1196 | **CLOSED 0 P0 / 1 P1 / 0 P2 / 4 P3, NOT dirty** (Opus-4.8-max + self-audit; F1 dead-`remove_all` masking-diagnostic FIXED, `-D__linux__` ODR resolved SOUND against the real llvm-thylacine source; F2/F3/F4 folded, F2b/F5 tracked); `memory/audit_cl2_closed_list.md` | — |
| **CL-3** | The triple: `Triple::Thylacine` + clang ToolChain + lld default in `llvm-thylacine`; wrappers retired. **CL-3a LANDED** (the driver — 8-file fork change-map + `ThylacineTargetInfo` + a Fuchsia-templated `Thylacine` ToolChain; fork commit `df919c8dd`; §16.15a). **CL-3b LANDED — THE CL-3 ARC IS COMPLETE** (the wrapper retirement: `pouch-clang`/`pouch-ld`/`build_libcxx` onto the fork driver, fork-less fallback kept; F2b closed at the root — the fork `__cxa_thread_atexit` guard gains `__thylacine__`, so `-D__linux__` retires and the int32/int64 split is ELIMINATED; §16.15b). | **DONE:** the real triple cross-builds byte-compatible artifacts; fork-driver-linked `pouch-hello-*` + a fork-clang-built, `clang++`-driver-linked `pouch-hello-cxx` boot + `ALL C++ WIRES PASS`; boot OK, 0 EXT, suite 1196/1196, SMP 40/40 (kernel byte-unchanged) | none (host-side) | — |
| **CL-4** | Support-layer port + the device toolchain: mmap detours, Program/Path/Process/Signals/DynamicLibrary; static multicall cross-built + baked to `/clade`. **LANDED — THE CL-4 ARC IS COMPLETE** (§16.16): a five-layer masking stack (ELF OSABI / raw 6-arg mmap / console fstat / empty InstalledDir / the cc1-argv prepend); fork commits `ce5a1c519` (CL-4b) + `e7d6be5f8` (CL-4c); durable patches `usr/ports/llvm/patches/0001..0006`. | **DONE:** `clang++ -O2` compiles, links (ld.lld), and runs a real C++ program on-device — STL + a live throw/catch — via spawned cc1, in-process cc1 (`-c`), and link-only; `CLADE-HELLO sum=285 eh=1`, suite 1197/1197, boot OK, 0 EXT | focused round CLOSED 0 P0 / 2 P1 / 1 P2 / 3 P3, NOT dirty (`memory/audit_cl4_closed_list.md`) | — |
| **CL-5** | Build storms + the F4 budget mechanism. **LANDED** (§7.1 mechanism, §7.2 storm): the spawn-time per-Proc page budget (measured first — a 1959-byte template-heavy TU costs 250 MiB, 97.8% of the default), and the on-device storm — **GNU make 4.4.1 builds itself under `make -jN`** (35 TUs; `-j1` 2759 ms / `-j4` 1033 ms = **2.67×** on 4 vCPUs; the artifact runs AND drives a build of its own). The storm found **#96** (fstat on a pipe returned -1 → every concurrent `-j4` job died silently) and, via the same sweep, **#97** (the notes-fd twin). **Host-vs-device par**: the same 35 TUs driven host-side with the same fork clang put the device within **~1.29×** on warm serial compile (61.1 vs 78.8 ms/TU, both clear-host) — versus the go build's 45–53×, because that gap is FS-metadata-bound, not CPU. | **DONE:** `make -jN` of a real project completes on-device; numbers recorded, no committed target | **DONE:** F4 mechanism round CLOSED 0/0/0/3 (`memory/audit_cl5_closed_list.md`); the storm is pure build-system + a 1-slot Dev addition | ThinLTO, sanitizers-on-device: out. **zlib/sqlite/LLVM-subset** not built — GNU make was chosen instead (already Thylacine-configured; no `./configure` is runnable without a POSIX shell) |
| **CL-6** | clangd + Nora C/C++ | diagnostics/hover/def in Nora on a C++ file | none (userspace client) | lldb-dap → post-arc |
| **CL-7k** | The JIT capability (kernel): code Burrow + `SYS_ICACHE_SYNC` + `CAP_JIT`; I-42 | the `libthyla_rs::jit` prover (emit→sync→call; ungated Proc **denied**) | **prosecute hard** (W^X-adjacent; own focused round; F8 spec posture) | — |
| **CL-7** | Mesa/llvmpipe + SDL-GL + GLQuake. **Entry decision LANDED (§16.19)**: pin `mesa-26.1.6`; frontend fork resolved to option (i) — resurrect gallium OSMesa in `mesa-thylacine` (6 files / 1,392 lines, **zero C changes**, compiles against LLVM 22.1.8); EGL surfaceless rejected as structurally incompatible (the DRI loader `dlopen`s its driver; static-only Thylacine can't host it; GLES-only, no `libGL`). Three build requirements found + recorded: `-Dllvm-orcjit=true` MANDATORY (aarch64 is in Mesa's `llvm_has_mcjit` list, so ORC is NOT auto-selected — a forgetful build silently gets MCJIT and bypasses the CL-7k mapper), `LLVM_ENABLE_RTTI=ON` REQUIRED on the clade LLVM (the ORC backend's `dynamic_cast` cannot build without it — so CL-7a starts with a clade rebuild), and MCJIT must be linkable + the LLVM library set complete (`--shared-mode` enumerates everything). Owed to CL-7a: a shim cross `llvm-config` (`NATIVE/bin/llvm-config` reports its own 4-archive libdir, not the target's 207) — **DONE at CL-7a-1 (§16.20)**; the full llvmpipe link — **DONE at CL-7a-2 (§16.21): `osmesa-prove` links, a 142 MB static aarch64 `ET_EXEC`, 1300 GL entry points, no `PT_DYNAMIC`, no RWX segment**; the on-device run — **CL-7b-1 (§16.22): llvmpipe RUNS on the device and reaches the I-42 capability gate** (the `DualMapMemoryMapper` CL-7k named is written; `USE_JITLINK` had to gain aarch64 or ORC silently stayed on RTDyld, which cannot work here). Two blockers remain before a triangle: `CAP_JIT` (elevation-only — CL-7b-2 adds the corvus clearance) and a static-musl `dlopen` on the OSMesa init path (**#115**). | **GLQuake renders via llvmpipe through Tapestry**; gears smoke in CI | focused round (the ORC mapper + the GL glue's weave lifetime) | lavapipe → stretch |
| **CL-8a** | Rust cross target + std-over-pouch | a std Rust program runs on-device | boundary audit (std's OS seam) | CL-8b → follow-on arc |
| **CL-9** | Arc close: the D3 self-host story — device clang rebuilds clang (stage-2) | stage-2 completes on the 8–16 GiB config | consolidated close + SMP gates | stage-3 byte-compare → stretch |

Sequencing (F7, resolved): the Phase-8 pole per ROADMAP D3, slotted after
the current gfx-track milestones. CL-1/CL-2/CL-3 are parallelizable with
late gfx work (disjoint surfaces).

---

## 13. Risks

- **Memory ceiling** (the honest #1): `-O2` + in-memory lld output on a
  4 GiB VM. Mitigations: overcommit + DECOMMIT already built; `-j` clamps;
  the F4 budget; the 8–16 GiB configs for self-host. Re-measured at CL-0.
- **Support-layer long tail**: the Unix/*.inc surface hides small POSIX
  assumptions; the CL-0 Tier-2 census exists to flush them early.
- **Maintenance drag**: three fork repos on a 6-month LLVM cadence.
  Bounded by the pin-to-host-major rule + upstream-shaped deltas
  (Serenity's demonstrated path).
- **Scope creep**: the cut lines are in the table; lavapipe, lldb-dap,
  CL-8b, ThinLTO, on-device sanitizers are all explicitly severable.
- **llvmpipe/JIT perf under HVF**: unmeasured; the gate is deliberately
  "GLQuake playable," not a number.

---

## 14. Forks — RESOLVED 2026-07-23 (user adopted every lean verbatim)

| # | Fork | Resolution (adopted) |
|---|---|---|
| F1 | Arc name "Clade" | adopted |
| F2 | Pin LLVM 22.x (host parity) | adopted |
| F3 | Static-only + llvm-driver multicall | adopted (lld membership verified at CL-0) |
| F4 | I-32 vs toolchain RSS | **(b)** spawn-time page-budget under a global hard cap; default unchanged |
| F5 | Mesa delivery = gallium OSMesa → weave → tpresent | adopted (re-confirm frontend at CL-0) |
| F6 | Rust std tier = over pouch (ports tier) | adopted |
| F7 | Sequencing slot | Phase-8 pole, post-gfx-milestones |
| F8 | I-42 spec posture | prose+audit (W1.5 precedent); escalate only on CL-7 SMP subtlety |

---

## 15. Adoption edits (on signoff)

ROADMAP (#67/D3 → the Clade arc + workstream table) · NOVEL.md (the JIT
angle promoted from post-v1.0 capture to scheduled-with-consumer; a keystone
note on #67) · ARCH §28 (mint I-42) + §25.4 (the code-Burrow row; the F4
budget row) + §6.6 (supersede the pkey note with the dual-map design) ·
CLAUDE.md (trigger-table mirror rows) · memory (`project_llvm_arc_design.md`).

## 16. CL-0 findings (2026-07-23) — the spike/census addendum

Instruments: (i) the tree census (greps; the §2 confirmations); (ii) a
disposable GCP ARM VM (t2a-standard-16 spot, Alpine containers, torn down
after; <$1 total) — the syscall census of **stock Alpine clang 22.1.3**
(`aarch64-alpine-linux-musl` — version-exact against the 22.x pin; the
demand side) via `strace -f -c` over four workloads (C compile,
template-heavy C++ `-O2` compile, static `-fuse-ld=lld` link, `llvm-ar`,
`make -j2` toy build), plus the pinned **llvmorg-22.1.8** static
AArch64-only clang+lld multicall build (the CL-4 infra dry-run + the RSS
instrument); (iii) source reads of the pinned LLVM tree + the Mesa GitHub
mirror. The fork base is cloned: `~/projects/llvm-thylacine` @
`llvmorg-22.1.8` (shallow, single-branch; host brew is 22.1.4 — same
major, point-skew acceptable under F2).

### 16.1 The syscall-gap census (Tier-2 demand vs the pouch seam)

46 distinct syscalls demanded across the workloads. Disposition against
the pouch boundary (the seam table + the source-level patches):

- **Already served — table** (~10): `close fstat lseek mmap munmap
  pread64 read write set_tid_address` + the exit family.
- **Already served — source-rerouted** (~12): `openat` (0009
  legacy-name), `newfstatat` (0019 → `SYS_STAT`/POUNCE), `futex` (0004 →
  torpor), `clone`-for-threads (0004 → `SYS_THREAD_SPAWN`),
  `writev`/`readv` (0002 stdio-no-iovec), `rt_sig*` (0007), `mmap`
  family (0003), `ioctl`+`fcntl` (partial: 0006/0010/0021).
- **GAP → CL-1 boundary lines, ALL onto existing kernel syscalls** (~13):
  `getdents64→SYS_READDIR(56)` · `wait4→SYS_WAIT_PID(22)` ·
  `renameat→SYS_RENAME(57)` · `unlinkat→SYS_UNLINK(58)` ·
  `pipe2→SYS_PIPE(8)` · `dup3→SYS_DUP(12)` · `chdir→SYS_CHDIR(69)` ·
  `getcwd→SYS_GETCWD(70)` · `getpid→SYS_GETPID(72)` ·
  `faccessat[2]`→stat+perm probe · `ftruncate`/`fchmodat`→`SYS_WSTAT`
  (size/mode) · `pselect6`→the 0005 poll shim · `execve`+`clone`-for-
  process → the CL-1 `posix_spawn` rewrite (structural, §6.1).
  **Headline: ZERO new kernel syscalls are needed for CL-1..CL-4** — the
  gap census closes entirely onto surface the kernel already ships. The
  kernel changes in the whole arc remain exactly the CL-5 F4 budget and
  the CL-7k JIT trio, as scoped.
- **Stub-OK / ENOSYS-tolerated** (~11): `brk` (mallocng mmap-fallback),
  `getrusage` (zeros), `membarrier` (fallback fences), `mknodat` (make
  output-sync degrade), `mprotect` (thread-stack guards bypassed by
  0004; residual: verify at CL-1), `mremap` (musl realloc falls back;
  perf note), `prlimit64` (RLIM_INFINITY), `sigaltstack` (§6.4
  Signals.inc stub), `umask` (libc-local emulation),
  `sched_getaffinity` (stub now; wiring a real ncpus source is a CL-1
  nicety — it feeds lld's thread count and `make -j` defaults).

New load-bearing findings the §2 owes-list did NOT have:

1. **`renameat` is per-compile load-bearing** — clang writes every `.o`
   via temp + atomic rename (1 rename/compile observed). Unmapped today;
   wires to `SYS_RENAME`. Without it every compile fails at output-write.
2. **`getdents64` is per-compile load-bearing** — 8 calls in an ordinary
   `clang++ -O2 -c` (header-search dir scanning), not just
   `std::filesystem`. Raises `pouch-dirent` from "directory tools need
   it" to "every compile needs it."
3. **`fsync` is genuinely unmapped in pouch** (no port has ever needed
   it; `SYS_FSYNC` = 55 exists kernel-side). Not needed by
   clang/lld/make — but the git port (CL-1's sibling consumer) will;
   note for that arc.

### 16.2 The environ ground truth (§2's open question — CLOSED)

`kernel/include/thylacine/exec.h:158`: the exec frame always writes
`envp[0] = NULL` (*"no envp at v1.0"*) — the kernel never populates
envp, for any program; `environ` is empty in every pouch program today
and `/env` (kernel-cloned per-Proc) is the sole environment channel.
The `pouch-env` crt boundary line (populate `environ` from `/env` at
startup; `/env` stays the source of truth) is **confirmed required** at
CL-1, as designed (§6.2).

### 16.3 The special-path census (the §13 Support-layer long tail)

From the full compile trace: **`/proc/self/exe`** (LLVM
`getMainExecutable`) and **`/proc/self/fd[/N]`** are the two `/proc`
dependencies — Thylacine's devproc has neither (no `self`, no `fd` at
v1.0; `/proc/fd` is the deferred #66c). Both become CL-4 Support
patches (argv[0]-based resolution for the former; the latter's caller
is bounded). `readlinkat` runs ~169×/compile (config + InstalledDir
resolution) — the Support patch must resolve cheaply, not
per-call-fail slowly. `/dev/urandom` is touched (exists on Thylacine —
covered). `/etc/clang22/*.cfg` probes are Alpine-config artifacts, not
intrinsic.

### 16.4 The link shape (§6.1 confirmed by trace)

The clang driver spawns **one** child (`ld.lld`) and `wait4`s it; lld
itself is heavily threaded (futex-hot: ~1600 calls on a trivial link —
torpor's audited ground). `ftruncate` appears exactly once (lld's
`FileOutputBuffer` sizing the output) — removed by the §6.3 in-memory
detour or wired via `SYS_WSTAT`; either suffices. `make -j2` adds
`pipe2` (jobserver) + `wait4` + fork-per-job — all CL-1 `posix_spawn`
territory.

### 16.5 lld-in-multicall (F3 — VERIFIED, source + build)

`lld/tools/lld/CMakeLists.txt:10` carries `GENERATE_DRIVER` at 22.1.8;
members: clang, lld, clang-scan-deps, clang-installapi + the
binutils-shaped tools. The pinned static build produced `bin/llvm` with
the full dispatch set (`clang`, `clang++`, `clang-cl`,
`clang-installapi`, `lld`, `ld.lld`, `ld64.lld`, `lld-link` → `llvm`);
`clang --version` and `ld.lld --version` both answer 22.1.8 through the
multicall. The F3 fallback (two binaries) is not needed. The first
smoke exposed a useful preview: the fresh clang, config-less, could not
find the census environment's GCC crt/libgcc pieces — exactly the
driver knowledge CL-3's `Triple::Thylacine` ToolChain encodes for the
pouch sysroot (the `--config`/`--gcc-toolchain` retries kept failing on the Alpine GCC-triple layout vs the build’s default triple — closed as a census-environment artifact: the stock same-version clang proved the musl E2E in the strace phase, and the driver-config lesson is precisely CL-3’s deliverable). `clangd` has NO `GENERATE_DRIVER` — it ships
as its own static binary at CL-6 (resolves §5's parenthetical).

### 16.6 Mesa: gallium OSMesa is GONE upstream — a CL-7 frontend fork

The §3/§9 premise ("gallium OSMesa survives") is stale: upstream
commit `027ccd96` (2025-03-02, MR 33836) **removed the OSMesa
frontend** — *"redundant with EGL surfaceless."* Last release carrying
it: **25.0.x**; gone from 25.1 on. gallivm-ORC is healthy: at 25.2
`lp_bld_init_orc.cpp` is live and meson auto-selects ORC whenever the
LLVM build lacks MCJIT (`llvm-orcjit` option; LLVM ≥ 15) — the LLVM-22
pairing is fine on the JIT axis. The collision: a Mesa old enough to
have OSMesa (25.0) is too old for LLVM-22 gallivm; a current Mesa has
no OSMesa. Options for the CL-7 entry decision (the pin was always
deferred there):

- **(i) — the CL-0 lean**: pin a current LLVM-22-compatible Mesa and
  **resurrect the OSMesa frontend in `mesa-thylacine`** — it is ONE
  file (`osmesa.c`, ~1.1 kLOC + a 444-byte meson.build; also preserved
  on the amber branch), squarely inside the §4 "small, enumerable
  delta" vendoring policy.
- (ii) EGL surfaceless — upstream's named replacement;
  headless-capable (no DRI/GBM/display) but pulls the EGL loader
  surface into the port.
- (iii) a Thylacine-native thin gallium embedding rendering straight
  into the weave (zero copy-out) — the ambitious variant of (i).

In all three the §9 delivery (off-screen render → weave → `tpresent`)
stands; only the frontend piece moves. Decide at CL-7 entry with a
configure smoke against the candidate pin.

**RESOLVED at CL-7 entry (2026-07-30) → option (i). See §16.19 for the
smoke that decided it, and for three CL-7 build requirements it found
that this section got wrong.**

### 16.19 CL-7 entry: the frontend smoke (§16.6 RESOLVED → option (i))

The §16.6 fork is closed on measurement, per its own instruction. The
pin is **`mesa-26.1.6`** — §4's "a current 25.x-era release" is a year
stale; 25.0.x is four minor releases back, and OSMesa is confirmed
absent at the pin (`src/gallium/frontends/` holds d3d10umd, dri, glx,
hgl, lavapipe, mediafoundation, rusticl, teflon, va, wgl — and 0 files
anywhere in the tree mention osmesa).

**Option (i) is adopted, and it COMPILES.** The 25.0.7 frontend was
grafted onto 26.1.6 and built against the fork's LLVM 22.1.8: a
385,464-byte AArch64 object defining all nine public entry points
(`OSMesaCreateContext` / `…Ext` / `…Attribs`, `MakeCurrent`,
`DestroyContext`, `GetColorBuffer`, `GetProcAddress`, `PixelStore`,
`GetIntegerv`). **Zero C source changes.** The entire drift is three
build-graph fixes:

1. `inc_mapi` no longer exists → drop it from the frontend's include list.
2. `glapi/glapi.h` moved to `src/mesa/glapi/glapi/` → put `src/mesa/glapi`
   on the include path. (Note `inc_glapi` is defined as
   `src/mesa/glapi/glapi` — one level too deep to resolve
   `#include <glapi/glapi.h>`; the parent is what is wanted.)
3. `with_shared_glapi` / `libglapi_static` are gone — referenced only by
   the outer `targets/osmesa` boilerplate, which Thylacine rewrites
   **static** anyway (see below).

**Correction to §16.6's "ONE file".** The measured delta is **6 files /
1,392 insertions** for the frontend half — `osmesa.c` (1,029),
`include/GL/osmesa.h` (332), two `meson.build`, and 4 hook lines across
`meson.build` / `meson.options` / `src/gallium/meson.build`. Upstream
also ships `src/gallium/targets/osmesa/` (`osmesa_target.c`,
`osmesa.sym`, `osmesa-symbols.txt`, `osmesa.def.in`, `meson.build`) for
the outer library; Thylacine replaces that with a `static_library()`,
so it is ~40 lines of our own build file rather than carried upstream
code. Still comfortably inside §4's "small, enumerable delta" — but it
is eight files, not one.

**Why (ii) is not merely bigger — it is structurally incompatible.**
Three measured facts, any one of which is disqualifying:

- **It cannot be static.** With `-Ddefault_library=static` the EGL
  configuration *still* emits four shared libraries: `libEGL.so`,
  `libGLESv1_CM.so`, `libGLESv2.so`, and `libgallium-26.1.6.so`. That is
  not a build-flag oversight — `src/loader/loader.c:883` does
  `dlopen(path, RTLD_NOW | RTLD_LOCAL)` to load the gallium driver. The
  DRI loader model *is* dynamic loading, and Thylacine is static-only
  with no dynamic loader (F3). By contrast the OSMesa *frontend* is
  already a `static_library()` upstream; only its outer wrapper is
  shared, and that wrapper is ours.
- **It yields GLES, not desktop GL.** With `glx=disabled` and no glvnd,
  no configuration produces `libGL` — the EGL path gives GLES 1/2 only.
  §9's acceptance gate is **GLQuake**, which is desktop GL.
- **It is ~36× the surface**: `src/egl` 24,895 lines + the DRI frontend
  10,525 + `src/loader` 2,057 ≈ 37,500 lines across 74+ files, versus
  1,392.

Option (iii) is unchanged as the follow-on refinement of (i): once the
frontend is ours, replacing its copy-out-at-`glFlush` with a direct
weave write is a local change inside a file we already carry. So (i) is
also the staging for (iii), not an alternative to it.

#### Three build requirements the smoke found

All three are the same shape this project keeps meeting — **a wrong
default that configures and builds cleanly**:

**(a) `-Dllvm-orcjit=true` is MANDATORY, and §16.6's reasoning above is
wrong.** This section claims meson "auto-selects ORC whenever the LLVM
build lacks MCJIT". It does not probe the LLVM build at all:

```
llvm_has_mcjit = host_machine.cpu_family() in ['aarch64','arm','ppc','ppc64','s390x','x86','x86_64']
llvm_with_orcjit = get_option('llvm-orcjit') or not llvm_has_mcjit
```

It is a **CPU-family list, and aarch64 is in it** — so on our target ORC
is *not* auto-selected. Measured both ways: with the option,
`GALLIVM_USE_ORCJIT=1`; without it, `=0`. A build that forgets the flag
gets the **MCJIT** path, which does not go through ORC's `MemoryMapper`
— the exact seam CL-7k's `DualMapMemoryMapper` plugs into (§8, §16.18).
It would then try to allocate RWX and fail under I-42's strict W^X,
having configured and compiled without a murmur. Treat the flag as
load-bearing and assert `GALLIVM_USE_ORCJIT=1` in the build.

**(b) the clade LLVM must be built `-DLLVM_ENABLE_RTTI=ON`.** Neither
`tools/clade-stage1.sh` nor `build.sh::build_clade` set it, so both
inherit upstream LLVM's RTTI-**off** default. Mesa then refuses to
configure ("LLVM was built without RTTI, so Mesa must also disable
RTTI"), and `-Dcpp_rtti=false` — Mesa's own suggested escape — makes
`lp_bld_init_orc.cpp:246` fail outright:

```
error: 'dynamic_cast' not permitted with '-fno-rtti'
      auto &sc = dynamic_cast<llvm::orc::SimpleCompiler &>(irc);
```

So **the ORC path cannot be built against an RTTI-less LLVM at all** —
the two requirements are in direct contradiction and RTTI is the only
resolution. Proven: stage1 rebuilt with `LLVM_ENABLE_RTTI=ON` (3,237
edges), `llvm-config --has-rtti` → `YES`, after which the ORC backend
(6,522,552-byte object) *and* the resurrected OSMesa frontend both
compile with 0 errors and `GALLIVM_USE_ORCJIT=1`. This is also the
distro-standard setting — Debian/Fedora enable RTTI in LLVM precisely
so Mesa can link it. Consequence for sequencing: CL-7a begins with a
clade-LLVM rebuild, since the `/clade` toolchain shipped at CL-4/CL-5
was built RTTI-off.

**(c) Mesa needs MCJIT *linkable*, and needs the library set
COMPLETE.** Two separate traps, both now closed in
`tools/clade-keep-build.sh`:

- `llvm_modules` (meson.build:1877) names `'mcjit'` **unconditionally**,
  even for an ORC build — so `libLLVMMCJIT.a` and `libLLVMInterpreter.a`
  must exist. Stage 3 had MCJIT in its *optional* list, which would have
  printed "note: optional absent — skipping", passed, and failed only
  later at Mesa configure. Now required.
- Meson probes `llvm-config --shared-mode`, a query taking **no module
  list**, so llvm-config enumerates every component it knows and errors
  on the first absent archive. Meson treats any error there as
  "dependency not found" — so a partially-built LLVM is rejected
  *wholesale* even when every requested module resolves. 19 unbuilt
  libraries were doing exactly that. Stage 3b now completes the set from
  llvm-config's own complaint and asserts `--shared-mode` passes.

#### The remaining CL-7a plumbing seam: a cross `llvm-config`

Mesa's LLVM detection is `config-tool` only on non-Windows
(`method : host_machine.system() == 'windows' ? 'auto' : 'config-tool'`),
so a cross build needs a **host-runnable llvm-config reporting TARGET
paths**. LLVM's cross build does produce `NATIVE/bin/llvm-config`, but it
reports its own subbuild: `--libdir` = `…/NATIVE/lib`, which holds **4**
archives against the target tree's **207**. So it cannot drive the cross
configure as-is. CL-7a supplies a shim `llvm-config` in the meson cross
file's `[binaries]` — small, and the standard technique for cross-Mesa.

Not yet measured (CL-7a's own work): a full llvmpipe *link*, and the
on-device run. The smoke proves configure + the two load-bearing
compiles, which is what the frontend decision needed.

### 16.20 CL-7a: the cross plumbing (and what a cross configure found)

CL-7a's first act is deliberately **not** the RTTI rebuild. The rebuild is
the expensive step (a full cross LLVM), and §16.19 had already been wrong
twice about what Mesa needs — so the order is: build the cross plumbing,
run a cross *configure* against the existing RTTI-off tree, harvest the
complete requirement set, and only then pay for one rebuild. That order
found three things a rebuild-first sequence would have hit an hour later.

#### The two tools

`tools/clade-llvm-config-cross.sh` — a cross `llvm-config`. It splits the
incoming questions by **authority**: the component *graph* ("what does
`orcjit` pull in?") is a property of the LLVM version and is delegated to
the fork's host `llvm-config`, which is the real implementation and cannot
drift from LLVM's own dependency tables; every *path* and every *target
fact* (version, triples, targets-built, RTTI, build mode) is read out of
the cross tree's own generated headers and `CMakeCache.txt`. Three
deliberate behaviours:

- every archive it names in a `--libs` answer is checked to exist, and a
  miss is a loud FATAL listing all of them, because meson reports any
  llvm-config error as the flat "dependency LLVM found: NO";
- `--shared-mode` answers `static` **without** enumerating all ~130
  components (the §16.19c trap), which is true by construction here and
  moves the completeness check to `--libs`, where it is load-bearing;
- an unknown query is delegated, path-rewritten, and logged as UNHANDLED
  rather than confidently answered.

`tools/clade-mesa-cross.sh` — emits the meson cross file. **Every path in
it is read out of the cross LLVM's own `CMakeCache.txt`** (compiler,
binutils, sysroot). That is not convenience: Mesa's objects get linked
against that LLVM, so a cross file naming a *different* compiler or a
different sysroot describes a toolchain the LLVM was never built with, and
the mismatch surfaces as an inscrutable link or runtime failure rather
than an error.

#### Finding 1: toolchain flags cannot live in `c_args`

The first cross configure died at `meson.build:1111: ERROR: Could not get
define 'ETIME'`, with `'errno.h' file not found`. The probe command line
carried `-nostdlibinc` **but not the `-isystem` that makes it survivable**:
meson's compiler-*check* path drops the `-isystem <dir>` pair out of
cross-file `c_args` while keeping every other flag (`-march`,
`-moutline-atomics`, `-fno-pic`, `-D_GNU_SOURCE` all present). The sanity
check, by contrast, gets the full list — so the two paths disagree. The
obvious suspect at the assembly site, `remove_linkerlike_args`, filters
none of these (read: its sets are `-Wl,`, `-L`, `-framework`,
`-headerpad_max_install_names`), so the drop is elsewhere in meson's
`CompilerArgs` machinery and is **not recorded here as a mechanism we
proved.**

The fix is structural rather than archaeological: toolchain flags belong in
the compiler **exelist** (`[binaries] c = [...]`), which nothing filters —
`--target=` and `--sysroot=` survived every path. `-moutline-atomics` moves
there for the same reason and a stronger one: it is a flag whose absence
compiles perfectly and faults on an A72 (#71/#91), so it must not sit
anywhere a build system is free to drop it.

#### Finding 2: `llvm_modules` never contains `orcjit`

With `-Dllvm-orcjit=true`, `src/gallium/auxiliary/meson.build:391` compiles
`gallivm/lp_bld_init_orc.cpp` (22 `orc::` references) — but the LLVM link
line comes from `llvm_modules`, which is:

```
llvm_modules = ['bitwriter','engine','mcdisassembler','mcjit','core',
                'executionengine','scalaropts','transformutils','instcombine']
llvm_optional_modules = ['coroutines']          # + 'lto' when draw_with_llvm
```

**No `orcjit`, and `engine` does not pull it** — measured: `llvm-config
--link-static --libs engine mcjit` yields ExecutionEngine, MCJIT,
OrcShared, OrcTargetProcess, RuntimeDyld and *not* OrcJIT or JITLink. So a
**static** ORC build has no `libLLVMOrcJIT.a` on its link line and fails on
undefined `llvm::orc::*` symbols. Upstream never meets this because
distros link a *shared* libLLVM, where every symbol is present regardless
of the module list, and because `llvm_has_mcjit` makes ORC non-default on
every common CPU family — **static + ORC is an untested upstream
combination**, and it is precisely the combination Thylacine requires
(no dynamic loader, I-42 W^X via ORC's `MemoryMapper`).

Fix: `llvm_modules += 'orcjit'` under `llvm_with_orcjit`, in the Mesa
fork — **required, not optional**, on the same reasoning that moved MCJIT
out of stage 3's optional list (§16.19c): a missing ORC library must fail
at configure, not at link. Measured delta: `orcjit` adds exactly four
archives — `LLVMOrcJIT`, `LLVMJITLink`, `LLVMOption`, `LLVMWindowsDriver`
— **all four already present** in the cross tree, so the rebuild's target
list is unchanged.

#### Finding 3: the cross `llvm-config` does not fail on the builder -- it SPINS

§16.19 said LLVM's cross build "does produce `NATIVE/bin/llvm-config`" reporting
its own subbuild. That is wrong for this configuration: with
`LLVM_USE_HOST_TOOLS=ON` and the fork's host tblgens there is no NATIVE subbuild,
so `<cross tree>/bin/llvm-config` is an **aarch64-thylacine static binary**. And
running it on the Linux builder does not error out -- it **spins**. Two instances
were caught in state `R` having burned 1343 s and 348 s of CPU, with the stage
reporting "running" the whole time:

```
27623  1343  R  llvm-config  .../clade/llvm-build/bin/llvm-config --shared-mode
28886   348  R  llvm-config  .../clade/llvm-build/bin/llvm-config --version
```

That is a **hang, not a crash**, which is the worse failure mode: it is
indistinguishable from a slow build. It also means the shim's justification is
stronger than "it reports the wrong paths" -- the cross binary cannot be run at
all.

The stage that ran it was `clade-keep-build.sh`'s own stage 3b, added at #110 and
recorded there as working. **Its load-bearing path had never executed**: every
prior run found no `bin/llvm-config` in the cross tree and took the early-return,
so the "verified" claim rested on a branch that was never taken. (The 207-archive
tree it was credited with producing came from the #110 empty-`ninja` bug building
everything.) Stage 3b now uses the shim instead, and asks a tighter question --
which archives are missing for the modules Mesa actually links, rather than for
all ~130 components -- so it builds what is needed, cannot hang, and no longer
needs `--shared-mode` at all. It answers in under a second: **73 archives, none
missing, `--has-rtti: YES`.**

#### Finding 4: a build-tree LLVM splits its headers, and the shim must not merge them

Measured, on both trees:

| dir | `llvm-c/Core.h` | `llvm/Config/llvm-config.h` |
|---|---|---|
| `<llvm source>/include` | present | absent |
| `<objroot>/include` | absent | present |

So a consumer needs **both** `-I` paths, and that is exactly what `--cppflags`
emits. The source half is *shared* between the host and cross builds (one fork
tree, two object dirs), so it must pass through **unrewritten**. The first version
of the shim rewrote the host `--includedir` onto the cross tree, which collapsed
the pair into a single path holding the generated config and none of the real
headers -- and gallivm died on `'llvm-c/Core.h' file not found`, again nowhere
near the cause. The shim now answers `--includedir` from the cross tree's
`LLVM_SOURCE_DIR` and rewrites only the object paths. With that fixed the gallivm
ORC backend **cross-compiles clean**: rc=0, 0 errors, an 875,792-byte
aarch64-thylacine object.

#### The harvest: the cross tree is already complete

With the plumbing working, meson resolved LLVM (`llvm-config found: YES
… 22.1.8`, `Run-time dependency LLVM … found: YES`) and asked for twelve
modules: `bitwriter core engine executionengine instcombine mcdisassembler
mcjit native scalaropts transformutils coroutines lto`. That closure is
**69 archives, none missing** from the cross tree; with `orcjit`, **73,
none missing**. So the RTTI rebuild changes exactly one thing — the flag —
and needs no new ninja targets.

And the configure then stopped **naming RTTI**, in Mesa's own words:

```
ERROR: LLVM was built without RTTI, so Mesa must also disable RTTI.
       Use an LLVM built with LLVM_ENABLE_RTTI or add cpp_rtti=false.
```

That diagnosis exists only because the shim reports the cross tree's *real*
`LLVM_ENABLE_RTTI` from its cache instead of a hardcoded answer — the
honesty is what makes the error land in the right place. §16.19b is now
confirmed independently, from the cross direction.

Everything else that came back `NO` (zlib, libzstd, expat, libdrm,
libudev, libdisplay-info) is non-fatal: the DRI/GLX/EGL platform deps are
disabled by the option set, and none blocked configure.

#### Where the Mesa delta lives

The Mesa fork stays **on the builder**, and its delta is carried in this
repo as a numbered patch series — the `usr/lib/pouch/patches/` model, not
the `llvm-thylacine` local-fork model. The two are different for a reason
that matters: the LLVM fork also builds the *host* toolchain the dev
machine itself uses, so it must exist locally; **nothing on the dev machine
needs a Mesa tree** (~500 MB checkout, and the build only ever runs on the
32-core builder). Patches-in-repo keeps the delta reviewable and lets any
builder reconstruct the tree from `mesa-26.1.6` + the series.

#### Two defects in my own tooling, and a probe that lied twice

Recorded because the shapes recur. The shim (a) logged its argv *after* its
precondition checks, so a precondition failure logged only a FATAL and no
argv — and (b) passed the component list to every delegated query, which
real `llvm-config` rejects with `components given, but unused`, turning a
working configure into a FATAL on `--ldflags`. Both are now covered by a
27-check self-test, including an A/B that deletes an archive to prove the
missing-archive assert can actually fire.

A third, in the *fix* for the stage-3b hang: the new completeness check piped its
filter into `grep .`, which exits 1 when nothing matches -- so under `set -e` with
`pipefail`, "nothing is missing" (the SUCCESS case) aborted the stage. This is the
trailing-filter trap the project has now met five times, and it was written into
the very patch that removed a hang. Reproduced and fixed with a two-second local
A/B rather than another builder round-trip.

The probes lied twice in the same session, both times by **showing nothing
and being read as nothing-happened**: a `grep '^ARGV:'` over the shim log
reported "empty" when the log held the FATAL that explained everything, and
an earlier module-list check ran under zsh — which does not word-split an
unquoted parameter — so `llvm-config` received one giant component name and
an rc-only assertion "passed" for entirely the wrong reason. Both are the
[[feedback-assertion-satisfiable-by-broken]] shape: before trusting a check
that returns nothing, prove its pattern can match something.

#### A benign recorded drift

The builder's *host* stage-1 tree currently carries `LLVM_ENABLE_RTTI=ON`
from the §16.19b proof, while `tools/clade-stage1.sh` does not set it.
Nothing needs it — only the **cross** LLVM's RTTI matters, because that is
what Mesa links — so the recipe stays as-is and a future stage-1 re-run
simply reconverges to it. Noted rather than "fixed" so the next reader does
not mistake the difference for a missing flag.

### 16.21 CL-7a-2 as-built: the OS-port layer, and the link that closed

CL-7a-1 left a precisely-scoped remainder: the llvmpipe build failed in three
translation units on "Unsupported OS" and a missing `util_get_process_name`.
That count was **incomplete, and knowably so** — it came from a `ninja` run
without `-k 0`, which stops at the first failure, so it was never the whole
set. This pass built with `-k 0` from the start: 979 objects compiled, and the
real remainder was four TUs, not three.

#### The tier decision

Mesa detects the OS in `src/util/detect_os.h`, and before this every
`DETECT_OS_*` was 0 for us — Mesa was compiling as if for a freestanding
target with no operating system at all. The question was which tier to join,
and the answer was already in the header: **`DETECT_OS_POSIX_LITE`**, the tier
Fuchsia introduced and the only one that sets a POSIX flag *without* claiming
full POSIX.

That is the honest description. Thylacine has pthreads, mmap, poll,
`clock_gettime`, nanosleep, `sched_yield` and BSD sockets; it does not have
fork, a dynamic loader, or `/proc/self/exe`. Fuchsia — a capability
microkernel with a partial POSIX libc — is the same shape, which is why the
tier exists.

#### Four arms, and what ground truth changed about each

Each arm was added the way Managarm was added to the same lists — a new OS
joins an existing condition rather than getting a bespoke branch.

**`os_time.c`** takes *both* `clock_nanosleep` arms, including the
`TIMER_ABSTIME` one. The seam table's syscall numbers say
`__NR_nanosleep 0xFFFF` and `__NR_clock_nanosleep 0xFFFF`, which reads as "no
sleep on this platform" — and that reading is **wrong**.
`usr/lib/pouch/patches/0022-pouch-nanosleep.patch` (landed for the SDL seam)
*rewrote the caller*: `__clock_nanosleep` — which `nanosleep()`, `usleep()`
and `clock_nanosleep()` all route through — is built on `SYS_TORPOR_WAIT` and
supports `CLOCK_MONOTONIC` and `TIMER_ABSTIME` both. The syscall number is
dead code. So Thylacine takes Mesa's *preferred* sleep path, not a fallback.
(The patch stack is a stack: a later patch can invalidate an earlier one's
number without touching it. Reading only the seam file gets this backwards in
either direction.)

**`os_misc.c`** needs only the `<unistd.h>` arm.
`os_get_total_physical_memory` and `os_get_page_size` need no arm at all:
both are gated on `HAVE_SYSCONF`, which the cross configure detects from musl
(confirmed in the emitted compile line).

**`log.c`** is a genuine upstream latent bug. It calls
`util_get_process_name()` under `#if !DETECT_OS_WINDOWS` but includes the
declaring header under `#if DETECT_OS_POSIX` — so *any* POSIX_LITE-only
platform compiles the call with no declaration in scope. Fuchsia has this too.
Fixing it *there* would mean widening the gate to `POSIX_LITE`, which would
also newly compile `<syslog.h>` on Fuchsia; that is not testable from here, so
Thylacine joins the arm rather than shipping an unverifiable change to someone
else's platform. The latency is recorded, not fixed.

**`include/drm-uapi/drm.h`** was the fourth TU and the one CL-7a-1 never saw.
llvmpipe's `lp_texture.c` includes `drm_fourcc.h` under a bare
`#ifndef _WIN32`, and uses `DRM_FORMAT_MOD_LINEAR` under that *same* gate
rather than under `HAVE_LIBDRM` — so the include cannot simply be skipped
without also editing driver logic. `drm.h` then forks on platform, and the
tempting fix is the wrong one: its `__linux__` arm wants `<linux/types.h>` and
`<asm/ioctl.h>`, and **neither is in the pouch sysroot**, so claiming
`__linux__` would have failed too (measured before choosing). Its `#else` arm
is the portable one — it typedefs `__u8..__u64` from `<stdint.h>` itself and
needs only the ioctl-encoding macros — and its single non-portable line,
`<sys/ioccom.h>`, already carries a `__GNU__` (Hurd) escape to
`<sys/ioctl.h>`. Thylacine is exactly the Hurd shape: not Linux, not BSD, musl
headers with `_IO`/`_IOR`/`_IOW`/`_IOWR` in `bits/ioctl.h`. One line.

#### The archive lies; only the executable tells the truth

With those four arms, all 982 objects compiled and `libOSMesa.a` built — 210
MB of it. It was also missing **every GL entry point**, and building it again
with half of them supplied succeeded just as quietly. An archive is a bag of
objects; no symbol resolution happens while making one, so it cannot fail this
way. The `osmesa-prove` executable that CL-7a-1 added specifically to answer
"what proves what" is the only thing that ever said so.

What it found: **glapi is a pair at 26.1.6, and neither half is in libmesa** —
which is what this target's first cut assumed. `libglapi`
(`glapi/shared-glapi/core.c`) carries the `_mesa_glapi_*` dispatch and the
noop table; `libglapi_bridge` (`glapi/glapi/libgl_public.c`) carries the 1300
public `gl*` entry points, and its *only* undefined symbol is
`_mesa_glapi_tls_Dispatch` from its partner. On Linux the split is invisible
because `libGL.so` links the bridge and resolves the dispatch dynamically from
`libglapi.so`; a static target has to name both halves explicitly.

Worth knowing if you touch glapi: aarch64 takes the **generic C** entry path.
`_GLAPI_ENTRY_ARCH_TLS_H` is defined only for x86, x86-64 and ppc64le, so the
hand-written TLS assembly stubs — and the `#error "Unsupported architecture"`
sitting next to them — are not in play here at all.

#### The result, and why the binary is loadable

`osmesa-prove` links: a **142 MB statically-linked aarch64 `ET_EXEC`**, 1300 GL
entry points, 13 OSMesa entry points, 3282 ORC/JIT symbols. Checked against
what `kernel/elf.c` actually validates, because a binary that cannot be
`exec`'d would make CL-7b a surprise:

- **No `PT_DYNAMIC`** — the loader rejects it explicitly (`elf.c:185`).
- **`ET_EXEC` + `EM_AARCH64` + ELF64 + LSB** — all four gates pass.
- **`OS/ABI: UNIX - GNU`** is accepted (`elf.c:77`), and deliberately so: the
  comment there names Clade-produced binaries as the reason.
- Segments are `R` / `R E` / `RW` / `RW` — **no RWX**, so I-12 is untroubled
  before the JIT even starts.
- Undefined symbols: `_DYNAMIC` (local; the usual static-binary artifact) plus
  two **weak** optional hooks (`__cxa_thread_atexit_impl`, a glapi TLS init).
  **Zero strong undefined.**

#### The delta, and where it lives

Three patches, 14 files, in `usr/ports/mesa/patches/` — the same
patches-are-the-durable-form policy as `usr/ports/llvm`, and more load-bearing
here because the Mesa fork lives on a GCP disk that exists to be thrown away.
The series is **round-trip verified**, not merely written: applied with
`git am` to a pristine `mesa-26.1.6` worktree it reproduces the fork's tree
hash exactly (`bb4a37cc…`).

#### One defect in my own tooling

`tools/clade-mesa-cross.sh` wrote `` `cc.get_define('ETIME')` `` — markdown
backticks — inside an **unquoted** heredoc, so bash tried to execute it as a
command substitution on every emit. It printed a syntax error to stderr,
returned 0 anyway, and silently truncated that line of the emitted cross
file's comment. Fixed, and the rest of `tools/` swept for the same shape
(clean).

### 16.22 CL-7b-1 as-built: the mapper, and what the device said

`6717f57f` (Thylacine) + `ca850c6e` (llvm-thylacine). llvmpipe now reaches the
I-42 JIT capability on the device. It does not yet rasterise, and the two
things standing between here and a triangle are now *measured* rather than
assumed.

#### The mapper CL-7k named but did not have

§16.18 closed with "the ORC `DualMapMemoryMapper` over `CodeRegion` is its
first consumer" — and that mapper did not exist. The fork's whole Thylacine
delta was Triple, driver and Support-layer.

Writing it confirmed §8's shape argument rather than straining it.
`InProcessMemoryMapper` cannot work here for a reason worth stating precisely:
Thylacine has **no mprotect at all**, and pouch's `mmap` accepts a `PROT`
argument only to ignore it (`0003-pouch-mman.patch` upgrades every anonymous
mapping to RW). So upstream's reserve-RW → write → raise-to-RX sequence fails
at its last step, and fails **late** — the reserve succeeds, the content lands,
and only the protect call reports that nothing here will ever be executable.

I-42 answers with two fixed-permission aliases instead of one mutable mapping,
and **ORC already draws that same line**: `MemoryMapper::prepare()` returns
*working memory* that need not be the address code runs at, which is precisely
why `SharedMemoryMapper` exists (a JIT writing into another process writes at
its own mapping of shared pages and links against the executor's). The writer
alias is that split with both halves in one address space. The mapper is
therefore `SharedMemoryMapper`'s addressing over `InProcessMemoryMapper`'s
bookkeeping, and the delta is four methods — `reserve` is `SYS_JIT_CREATE`,
`prepare` adds the writer/exec displacement, `initialize` zero-fills through
the writer and publishes with `SYS_ICACHE_SYNC` where upstream calls mprotect,
`release` is `SYS_JIT_DESTROY` (which names the **writer** alias — the kernel
remembers the pairing, so half a region cannot be released).

Two things for whoever touches it next. Upstream's `initialize()` memsets the
zero-fill tail *at the executor address*; here that is the read-only alias, and
it is the one place a faithful copy would fault rather than misbehave. And a
segment asking for `Write` gets the exec alias's RX like every other, so JIT'd
code writing to its own data section **at runtime** would fault — link-time
writes are unaffected because JITLink routes all content through `prepare()`,
which covers the GOT and stubs, the only writable groups llvmpipe is known to
emit. Recorded rather than rejected: failing `initialize()` on a Write group
would refuse allocations that work.

#### The finding: a third wrong default, and the quietest

The first build **succeeded** — `ninja` rc=0, zero `FAILED`, `osmesa-prove`
linked — while the gallivm object referenced **neither** memory manager.

`USE_JITLINK` selects the object **linking layer**, and is a *different axis*
from `GALLIVM_USE_ORCJIT`, which selects ORC vs MCJIT. Reading
`lp_bld_init_orc.cpp` and concluding "Mesa uses `ObjectLinkingLayer`" conflated
them: the file contains both paths, and which one is live is a `#ifdef`.
aarch64 is **not** in Mesa's `USE_JITLINK` list (RISCV, LoongArch and Win32
only), so the ORC path still ran on `RTDyldObjectLinkingLayer`, whose
`SectionMemoryManager` allocates RW and then mprotects `PROT_EXEC` on — and
`MemoryMapper` is a **JITLink-only** seam, so the dual-map mapper was never
even consulted.

Third wrong-default-that-builds-clean in this arc after `llvm_has_mcjit`
(§16.19) and `LLVM_ENABLE_RTTI`, and the quietest of the three: nothing fails
until runtime. It was caught only because the check asked *"is the symbol in
the object"* rather than *"did the build succeed"* — the same
archive-cannot-fail discipline §16.21 arrived at, applied one layer down.

#### What the device said

Boot OK, 1232/1232, 0 `EXTINCTION`, `boot-ms: 27495`:

    osmesa-prove: I-42 probe EACCES -- no CAP_JIT, llvmpipe cannot JIT
    Dynamic loading not supported
    joey: clade CL-7b osmesa-prove rc=1 peak=76 pages

**Line 1 is the CL-7b-1 result.** A 68 MB static aarch64 binary `exec`s, musl
comes up, and the kernel refuses `SYS_JIT_CREATE` exactly where it should:
`CAP_JIT` is elevation-only, so a joey-spawned child cannot hold it. The
refusal *is* the capability model working. The prover calls the syscall
directly before any of Mesa runs precisely so this is legible — reached through
gallivm it would present as "OSMesaCreateContextExt returned NULL", which is
also what a broken mapper and an unrelated gallivm fault look like.

**Line 2 is a second blocker, independent of the first, and it fires first.**
That string is musl's static-build `dlopen` stub verbatim
(`src/ldso/dlopen.c:6`). Something on the OSMesa init path `dlopen`s and treats
the failure as fatal, so the process exits 1 without reaching the prover's own
OSMesa check. Tracked as **#115**; prime suspect is LLVM's
`DynamicLibrary::DLOpen(NULL)` behind ORC's process-symbol search generator —
how JIT'd code resolves libc — but **not confirmed**, and confirming it is
CL-7b-2's first job. It does not overturn §16.19: that rejected
EGL-surfaceless because `loader.c` `dlopen`s the gallium *driver*, and this is
a different `dlopen`, so the reasoning stands but was incomplete.

#### The page budget was never the 142 MB number

Eager anon at exec is **~1.36 MB** (536 KB data + 821 KB bss). The other 67 MB
is text and rodata, which REVENANT demand-pages and does not charge per page at
v1.0. Measured peak across the run: **76 pages**.

#### Gating

joey's `gl_probe()` **reports rather than gates** at CL-7b-1, deliberately: the
prover cannot pass until it holds `CAP_JIT`, and gating on a known-impossible
result would be theatre. It is proven inert in the default configuration — a
non-clade boot emits **zero** `CL-7b` lines — rather than argued inert from the
`t_open` guard. It becomes a gate at CL-7b-2, which adds the corvus clearance
and answers #115.

### 16.7 The memory re-measure (§7 / F4)

From the pinned 22.1.8 static build (AArch64-only, Release, `-j16`,
`LLVM_PARALLEL_LINK_JOBS=2`, t2a-standard-16; host toolchain = the
census environment's default GCC 15 — the TU numbers are gcc's, same
order as clang's own):

- Wall: **33m 26s** (7h 40m user, 16 vcpu).
- Worst single compile RSS: **2.46 GiB** (`cc1plus`, an LLVM TU at
  `-O2`) — ~10× the 256 MiB `PROC_PAGE_MAX` default and beyond §7's
  ">1 GiB outlier" band.
- Static link of the 158 MiB multicall: **0.84 GiB** (GNU ld,
  isolated relink, 7 s); lld re-drive of the same link: not measured — three re-drive attempts fought the census env’s chained ninja command plumbing, each re-measuring GNU ld (~0.83–0.84 GiB); the lld-specific number defers to CL-4’s device build, where it falls out for free (same order expected).
- Whole-storm peak at `-j16`: cgroup `memory.peak` **23.5 GiB**
  (includes page cache; the anon component is bounded by it).
- Artifacts: `bin/llvm` **158 MiB** unstripped / **134 MiB** stripped
  (static, AArch64-only, clang+lld+tools); build tree 2.9 GiB +
  source 2.6 GiB.

**F4 verdict**: the data confirms option (b) as adopted — a
per-child raisable budget is *necessary* (the worst TU alone is ~10×
the default floor) and the 4 GiB global hard cap *suffices* (worst
observed single process 2.46 GiB, with headroom). On-device storms
are RAM-bounded by Σ(active TUs): the 4–16 GiB VM tiers need `-j`
clamps exactly as §7 planned. CL-9's stage-2 self-host on the 8–16
GiB configs is consistent with these numbers.

### 16.8 The static-binary syscall superset

Disassembly census of the static multicall (`mov w8/x8,#NR` + `svc`
pairing over the whole binary — the superset of any runtime demand):
**79 distinct NRs**, fully name-mapped (one unknown). The additions over the strace set are all cold-path musl families: AIO (`io_setup`/`io_submit`/`io_destroy`), `symlinkat`/`linkat` (honest-ENOSYS at v1.0 — no symlinks/hardlinks), `mkdirat`/`fchownat`/`fchmod[at]`/`fchown` (→ `SYS_WALK_CREATE`/`SYS_WSTAT`), `statfs`, `ppoll` (0005), `setitimer`/`sched_setscheduler`/`set_robust_list`/rlimit/`uname`/`sysinfo`/`gettimeofday` (stubs), `kill`/`tkill` (0007), `socket` (0006), `madvise` (ENOSYS-tolerated). Nothing in the superset demands new kernel surface. Cross-checks 16.1's strace set; entries outside the
dispositions are cold-path musl.

### 16.9 CL-1a as-built (the FS/process wires)

The first CL-1 sub-chunk landed: `usr/lib/pouch/patches/0024-pouch-fs-
process-wires.patch` (20 files: 2 new + 18 rewritten lower-half `.c`) wires
the per-compile/per-link FS+process calls from 16.1 onto existing kernel
syscalls, plus the `open(O_CREAT)` -> `SYS_WALK_CREATE` arm (16.1 missed
that clang's output-write goes through it -- traced while writing the
prover). Shared `__pouch_open_parent` splits a path into (parent-dir, leaf)
for the parent-fd kernel primitives; `readdir` translates the 9P Treaddir
stream into `struct dirent`. Proven end-to-end in-guest by
`/pouch-hello-fs` (spawned post-pivot against the writable Stratum FS).
Full as-built: `docs/reference/78-pouch.md` "The FS/process wires". Two
CL-0 predictions refined by ground truth: `dup2`/`dup3` (need dup-onto-N,
not `SYS_DUP`) and `pipe2` (need a 2-register `svc` shim) are NOT clean 1:1
wires -> deferred to CL-1b (their real home is the spawn fd-list).

### 16.10 CL-1b-0 as-built (the environ populate)

`0025-pouch-env.patch`: a crt boundary line (`src/env/_pouch_env.c` +
`__libc_start_main` hook) that populates `__environ` from the `/env` device
at startup, closing the 16.2 finding (envp is always empty; `/env` is the
sole environment channel). It `readdir`s `/env`, opens+reads each value, and
builds a malloc'd `"NAME=value"` vector so `getenv()` + `environ` iteration
both work; fail-soft (a missing `/env` leaves the empty envp). Proven
in-guest by `/pouch-hello-env` (joey sets two vars, the child inherits a
copy via the rfork clone, reads both back + confirms an absent var is NULL).
Full as-built: `docs/reference/78-pouch.md` "The environ populate". The
`posix_spawn` `envp` argument stays inherited-via-`/env` (the
`SYS_SPAWN_FULL_ARGV` `_pad_envp` slot reserves the kernel-side per-child
override); `setenv` mutates only the in-process copy.

### 16.11 CL-1b core as-built (posix_spawn / wait4 / pipe2 / dup2)

`0026-pouch-process.patch` (10 files: 2 new + 8 rewritten) wires the process
substrate the toolchain drives -- the clang driver `posix_spawn`s `cc1`/`lld`
and `wait4`s them -- each onto an existing kernel syscall (ZERO new kernel
surface). Since Thylacine has no fork/execve, `posix_spawn` is rewritten to
resolve its file_actions STATICALLY into the positional `SYS_SPAWN_FULL_ARGV`
fd_list (the dominant open/dup2-onto-0/1/2/close pattern resolves to
`{0,1,2}`); `wait4` translates the flag word (kernel `WAIT_CONTINUED`=4 vs
musl `WCONTINUED`=8) and repacks the plain-wait raw status `(raw&0xff)<<8` so
musl's `W*` macros decode it; `pipe` uses a 2-register `svc` shim. Proven
in-guest by `/pouch-hello-spawn` (self-respawn via `pipe2`+`posix_spawn`+a
stdout-redirect file_action + `waitpid` decode; `WEXITSTATUS ok=0 fail=1`;
argv pass-through). dup2/dup3 onto-target = a documented ENOSYS seam (no
kernel primitive; posix_spawn never needs a runtime dup2). Ground-truth
bring-up fixed three issues before the audit: `handle_dup` rejects a rights
superset (dup2 probe uses 0 rights → rejected → probe with WRITE/READ),
`argv[0]` is NULL under `SYS_SPAWN_WITH_FDS` (hardcode the self-name), and the
`{0,1,2}` seed over-specifies for a parent lacking a std fd (the existence
probe). Self-audit caught + fixed a P1 (opened[] stack overflow on >64
FDOP_OPEN). **Focused audit CLOSED CLEAN (Opus-4.8-max holotype + self-audit;
0 P0 / 0 P1 / 0 P2 / 6 P3, NOT dirty)** -- the ABI mirror, resolver, fd
lifecycle, and wait/pipe translations all traced sound; the 2 substantive P3s
fixed (F4 argv defensive bound; F1 comment naming the real runtime
onto-target callers). **CL SEAM (F1)**: dup2/dup3 onto a target fd is ENOSYS
(no kernel primitive), which leaves `freopen(filename,…)`/`login_tty`/
`daemon`/`wordexp` non-functional (each fails LOUD); the durable fix is a
kernel dup-onto-target syscall (an ABI addition -> escalate when a ported
workload needs it). Full as-built: `docs/reference/78-pouch.md` "The process
lifecycle"; closed list `memory/audit_cl1b_closed_list.md`. Boot OK, 0
EXTINCTION, suite 1196/1196 (kernel byte-unchanged). Next = CL-1c (GNU make +
on-device `make -j`).

### 16.12 CL-1c-1 as-built (the GNU make port — build + load-and-run)

The first REAL toolchain program runs on Thylacine: **GNU make 4.4.1**,
cross-built by `tools/build.sh::build_gnumake()` for `aarch64-thylacine` and
baked into the ramfs as `/make`. It is a **vendored port** (the SDL2/tyrquake
idiom), not a musl boundary-line: pristine source at `third_party/gnumake/`
(pruned-pristine, sha256 `dd16fb1d…`; see its PRUNE-MANIFEST.md), the Thylacine
delta at `usr/ports/gnumake/` (a hand-derived `config.h` + the two committed
generated gnulib headers `fnmatch.h`/`glob.h`; `patches/` is EMPTY — the port
needs zero source edits).

**The census (§16 questions 1-10) picked the clean config**: `USE_POSIX_SPAWN=1`
routes make's `child_execute_job` through `posix_spawn` (compiling out the
vfork/execve paths) so make natively drives CL-1b; `MAKE_JOBSERVER` left
UNDEFINED makes a top-level `make -jN` use the pure `job_slots` counter +
blocking `waitpid` reap — **no pipe/fifo/pselect/SIGCHLD/fcntl-O_NONBLOCK at
all**, a perfect fit for the Thylacine process substrate (pipe-blocking-only,
`fcntl` unwired). No fcntl boundary-line was needed: with the jobserver off,
`fcntl`→ENOSYS survives only in make's ENOSYS-tolerant startup checks +
harmless `fd_noinherit` no-ops (posix_spawn's fd_list is explicit, so CLOEXEC
is moot).

**config.h** is derived from an autoconf reference config.h (a real `./configure`
run — so the surface is autoconf-detected, not hand-guessed) with the census
deltas: `MAKE_HOST="aarch64-thylacine"`, `ST_MTIM_NSEC st_mtim.tv_nsec` (musl
POSIX, not darwin's `st_mtimespec`), and UNSET
`HAVE_FORK/VFORK/WORKING_*`/`MKFIFO`/`PSELECT`/`WAIT3`/`MAKE_JOBSERVER`/`MAKE_LOAD`/`HAVE_DECL_SYS_SIGLIST`.
The compile list is 30 src + 5 lib gnulib objects (concat-filename, findprog-in,
fnmatch, glob, getloadavg — musl provides `alloca`, so `lib/alloca.c` is not
built). All 35 compile + link cleanly against the pouch sysroot (zero undefined
symbols — pouch musl provides posix_spawn/glob/fnmatch/getloadavg/realpath/…),
a 371 KB static ET_EXEC.

Proven in-guest by the joey probe `/make --version` (`GNU Make 4.4.1` +
`Built for aarch64-thylacine` — the latter proves the derived config.h's
MAKE_HOST reached the binary), boot OK, 0 EXTINCTION, the CL-1a/1b siblings
unregressed. `--version` prints and exits before reading any Makefile or
spawning a child, so this is the load-and-run milestone; **the parallel-spawn
gate** (`make -j` driving CL-1b's posix_spawn over a real toy build) + the
boundary-line audit on the #68/#926 process-lifecycle lineage are **CL-1c-2**.

**Flagged seams** (from the census, neither needed for the toy gate):
*execvp self-re-exec* (`main.c:2817`, only hit by a self-remaking makefile — a
static toy Makefile never triggers it; a targeted `execvp→posix_spawn` patch is
owed at CL-4/CL-5 when real autotools projects build) and *adddup2-onto-0/1/2*
(handled — CL-1b resolves file-actions statically into the positional fd_list,
so arbitrary child←parent fd mappings work).

### 16.13 CL-1c-2 as-built (the on-device `make -j3` gate — the arc-2 close)

The audit-bearing proof that GNU make actually **drives** CL-1b's
posix_spawn/wait4 under `-j` parallelism. A joey post-pivot boot probe (search
"CL-1c-2" in `usr/joey/joey.c`) writes a self-contained toy project to the
writable `/tmp/mkt` — three INDEPENDENT "compile" recipes (each a shell-free
`/bin/cp` of a `.c`→`.o`) + a "link" recipe that DEPENDS on all three — and runs
`make -f /tmp/mkt/Makefile -j3`. Under `-j3` make starts the 3 compiles in
parallel (the `job_slots` counter + blocking `waitpid` reap; `MAKE_JOBSERVER`
off), reaps them, then runs the dependent link. **Everything is ABSOLUTE**
(`-f`, `/bin/cp`, absolute target/prereq paths) so the gate has zero cwd / PATH /
`-C`-chdir dependence. The gate verifies all four output files by exact content:
`a.o`/`b.o`/`c.o` prove the 3 parallel compiles ran; `prog` (== `a.o`'s content)
proves the link ran AFTER its prerequisites. Boot-fatal + non-vacuous (it unlinks
stale outputs + rewrites fresh inputs each boot, so a PASS requires make to
actually run the recipes). Verified in-guest: `/make -j3 PASS`, `status=0`, boot
OK, 0 EXTINCTION, suite 1196/1196 (kernel byte-unchanged).

**Shell-free is mandatory** (not just a gate convenience): Thylacine has no
`/bin/sh` and `ut` has no `-c` mode, so a recipe with a shell metacharacter would
make make spawn `/bin/sh -c '...'` → posix_spawn ENOENT → make exit 2. make's
`construct_command_argv` fast path spawns a metacharacter-free `/bin/cp` recipe
DIRECTLY (no shell), which is what drives CL-1b. **Two bringup fixes**
(ground-truthed, not guessed): `/tmp` must be created before the probe (the
Go-4c block that also creates it runs later), and the 4-arg spawn argv blob needs
a **trailing NUL** — the kernel `SYS_SPAWN_FULL_ARGV` parser requires the last
byte be NUL AND the NUL-count == argc (`kernel/syscall.c:6059/6064`); CL-1c-1's
2-arg blob didn't need it because the last string ended exactly at the buffer
bound.

**The focused boundary-line audit CLOSED 0 P0 / 1 P1 / 0 P2 / 4 P3, NOT dirty**
(Opus 4.8 max — the authorized Fable fallback, Fable being depleted; MODEL
start==end — plus a concurrent self-audit that independently root-caused the
P1). The audit confirmed by `#if`-guard trace that fork/vfork/execve/clone are
genuinely compiled OUT (only the `#else /* USE_POSIX_SPAWN */` branch of the
reachable `child_execute_job` is live), the jobserver + its SIGCHLD/pselect
machinery are not compiled (`MAKE_JOBSERVER` off), the SIGCHLD handler block is
skipped (`HAVE_WAIT_NOHANG` via `HAVE_WAITPID`) so make reaps purely via
`waitpid(-1,WNOHANG)` + blocking `wait`, the reap-any path composes with the
existing audited `SYS_WAIT_PID(-1)` (make is single-threaded; the kernel
serializes zombie-create/reap under `g_proc_table_lock`), the bad-stdin
adddup2-onto-0 and the wait-status translation round-trip, and the gate is
non-vacuous + boot-fatal. Dispositions:

- **F1 [P1] — the getcwd oversized-buffer bug, SURFACED not introduced.** The
  make oracle exposed a **pre-existing kernel defect** (LS-4, not CL-1c):
  `sys_getcwd_handler` (`kernel/syscall.c`) rejects any buffer
  `> SYS_OPEN_PATH_MAX+1` (1025), but make (like clang/git/every POSIX program)
  passes `getcwd(buf, PATH_MAX=4096)` → EIO → `make: getcwd: I/O error` at
  startup. **Benign for this gate** (make degrades gracefully — it only affects
  `$(CURDIR)`, not chdir / relative resolution — and the absolute-path gate is
  unaffected; `status != 0` is not tripped). Does NOT block the CL-1c close, but
  a **probable CL-2 blocker** (C++ `current_path()`) and broadly reachable. The
  fix is a one-line drop of the oversized reject (the `sys_validate_user_buf` +
  the `len+1 > buf_len_raw` fit-check are the correct + sufficient gates); tracked
  in `memory/bug_getcwd_oversized_buffer.md` and fixed as a separate kernel chunk.
- **F2 [P3] FIXED**: `mkt_file_eq` did a single `t_read` → a benign short read
  could false-FAIL a correct build (never a false pass). Now uses `read_exact`
  (a loop) + an EOF probe for exact length.
- **F3 [P3] FIXED**: two darwin-only CoreFoundation config macros
  (`HAVE_CFLOCALECOPYCURRENT`/`HAVE_CFPREFERENCESCOPYAPPVALUE`) carried from the
  autoconf reference were set `1` — inert (their gnulib consumers aren't in the
  compile list) but a landmine if it grows; now `#undef`.
- **F4 [P3] SEAM (CL-4)**: no `/bin/sh` → make can only run shell-free recipes.
  Real Makefiles (autoconf, kernel builds) lean on shell recipes (`;`/`&&`/`|`/
  `$(...)`/globs). A Thylacine `/bin/sh` OR make's one-shell mode over `ut` is the
  CL-4 lift.
- **F5 [P3] SEAM (CL-4)**: the stat wire leaves `st_mtim.tv_nsec == 0` →
  second-granularity mtime → the classic "make within one second" incremental
  race. The gate sidesteps it (unlink-then-rebuild), so mtime is never
  load-bearing here; a CL-4 incremental-build concern.

**THE CL-1c ARC IS COMPLETE** (the GNU make port builds, runs, and drives the
process substrate under parallelism). Closed list: `memory/audit_cl1c_closed_list.md`.

### 16.14 CL-2 as-built (the C++ runtime + the prover)

The C++ runtime stack -- **libunwind + libc++abi + libc++, static** -- cross-built
for aarch64-thylacine against the pouch musl sysroot via `LLVM_ENABLE_RUNTIMES`
(`tools/build.sh build_libcxx`), installed into `build/sysroot` (the three
archives + the `include/c++/v1` header tree), plus a C++ prover
(`/bin/pouch-hello-cxx`) that drives the whole stack END TO END. The runtime
SOURCES live in the LLVM fork (`$LLVMFORK`, `~/projects/llvm-thylacine` @
`llvmorg-22.1.8`) -- not vendored, like the Go arc's `$GOFORK`; absent fork ->
skip cleanly.

**The config, each decision ground-truthed (not guessed):**

- **`--target=aarch64-thylacine`** (the pouch convention), NOT `aarch64-linux-musl`.
  Under the unknown OS, libc++'s `atomic`-wait uses the GENERIC pthread fallback
  (pouch routes pthread), whereas `__linux__` selects the direct-futex path
  (`<linux/futex.h>` + raw `syscall(SYS_futex)`), which is BROKEN on Thylacine
  (pouch sets `__NR_futex` to the `0xFFFF`/ENOSYS sentinel; the futex is
  torpor-routed only for musl's OWN `__futexwait`, not a raw syscall). So the
  unknown OS is STRICTLY better for the runtime.
- **`CMAKE_SYSTEM_NAME=Linux`** -- a CMAKE-TOOLING knob ONLY (it makes CMake use
  `llvm-ar` instead of the Apple `libtool` that rejects aarch64 ELF objects on a
  macOS host). The compiled code's OS is the `--target` (thylacine), so `__linux__`
  stays undefined in the emitted code. Without it the `libc++abi.a` archive step
  dies on `cxa_personality.cpp.o is not an object file`.
- **`LIBCXX_HAS_PTHREAD_API=ON`** -- the unknown OS can't auto-detect pthread, so
  `__config` errors `"No thread API"`; this forces the pthread thread-API selection.
- **`LIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF`** -- musl has no
  `__cxa_thread_atexit_impl` (verified by `nm` on `libc.a`), so libc++abi uses its
  pthread-key fallback. (The CMake probe FALSE-POSITIVES it as present, because
  `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY` -- needed to break the runtimes
  chicken-and-egg -- turns the `check_library_exists` LINK probe into compile-only.)
- **`LIBCXXABI_ADDITIONAL_COMPILE_FLAGS=-D__linux__`** -- SURGICAL, libc++abi ONLY.
  libc++abi guards the whole `__cxa_thread_atexit` definition `#if defined(__linux__)
  || defined(__Fuchsia__)` (`cxa_thread_atexit.cpp:109`); the unknown OS matches
  neither, so the TLS-dtor ABI entry would be undefined at link. Verified this is
  the SOLE `__linux__` user in `libcxxabi/src` (cxa_guard keys on `SYS_gettid`;
  atomic.cpp is libc++, not libc++abi), so nothing else is perturbed. Re-points at
  CL-3's `Triple::Thylacine` + a `__thylacine__` guard.
- **`LIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF`** -- Thylacine ships no IANA tzdb
  (`tzdb.cpp`'s path is `#if defined(__linux__)`-only; `CMAKE_SYSTEM_NAME=Linux`
  defaulted it ON).
- **`_GNU_SOURCE`** on every compile -- exposes musl's POSIX/GNU surface
  (`nanosleep`/`uselocale`/`locale_t`/`wcsnrtombs`) that libc++ headers reference.

The prover compiles with `clang++ --target=aarch64-thylacine -std=c++20 -nostdlibinc
-isystem $sysroot/include/c++/v1 -isystem $sysroot/include -D_GNU_SOURCE`, links via
`pouch-ld` with `--eh-frame-hdr` (so libunwind finds `.eh_frame` via
`PT_GNU_EH_FRAME` + musl's static `dl_iterate_phdr`) + `-lc++ -lc++abi -lunwind`.

**Proven in-guest** (`/bin/pouch-hello-cxx`, a joey post-pivot boot probe with the
`pouch_smoke_one` pipe/reap/marker check; skips cleanly if the fork was absent at
build): exceptions (throw across frames, unwinder runs local dtors, catch-by-base +
`what()`), RTTI (`dynamic_cast` up/down + `typeid`), STL (`vector`/`map`/`string`/
`sort`), `std::thread` + join, **thread_local destructors** (the
`__cxa_thread_atexit` pthread-key path -- 4 workers each run their TLS dtor),
iostreams (`std::cout`), and `std::filesystem` (`current_path`/`create_directory`/
`directory_iterator`/`rename`/`file_size`/`remove`). `pouch-hello-cxx: ALL C++ WIRES
PASS`, boot OK, 0 EXTINCTION, suite 1196/1196 (kernel byte-unchanged).

**One latent CL-1a gap this surfaced + FIXED (`0027-pouch-remove.patch`):**
`std::filesystem::remove` -> `::remove(3)`, and musl's `stdio/remove.c` (a) issues a
RAW `__syscall(SYS_unlinkat)` (bypassing the pouch-overridden `unlinkat()` FUNCTION)
-> the `0xFFFF`/ENOSYS sentinel, AND (b) relies on the kernel returning `-EISDIR`
to fall through from `unlink` to `rmdir` for a directory. 0024 wired
`unlink()`/`rmdir()`/`unlinkat()` (the functions) but missed the stdio `remove(3)`
shim; the CL-1a prover used `unlink()`/`rmdir()` (which worked), never `remove(3)`.
The 0027 fix is an **lstat-dispatch** `remove()` (a directory -> `rmdir()`, anything
else -> `unlink()`, both the pouch-wired functions) -- it avoids BOTH the raw
syscall AND the EISDIR reliance, because Thylacine's `SYS_UNLINK` collapses every
failure to a generic `-1` (no distinct `EISDIR`), a **#102-class kernel errno-loss
gap on the unlink path** (`memory/bug_unlink_errno_loss.md`; a future kernel fix
returning the real errno would let `remove()` use the simpler classic form).

**Two SEAMS (documented, not blockers):**

- **`__cxa_guard` concurrent-static-init (correctness, tracked).** libc++abi's
  thread-safe guard for FUNCTION-LOCAL STATICS (Meyers singletons) runs a recursion
  check keyed on `syscall(SYS_gettid)` -- which pouch routes to ENOSYS -> every
  thread gets the same bogus id -> two threads racing the SAME static's first init
  can FALSE-ABORT with "recursive initialization". The prover's threads test would
  ride straight into this (a concurrent FIRST init of libc++abi's `__cxa_thread_
  atexit` `manager` static across the 4 workers -- the CL-2 audit F1), so it
  PRE-INITS that machinery UNCONTENDED on the main thread before spawning workers;
  the workers then exercise only concurrent TLS-dtor REGISTRATION (which works),
  not the static's first init. Concurrent FIRST init of a shared function-local
  static remains broken. Root fix needs a real `gettid` in pouch/kernel
  (a design fork -- a new `SYS_GETTID` kernel syscall vs a pouch shim over the
  pthread struct's `self->tid`); ESCALATE. Tracked: `memory/bug_cxa_guard_gettid.md`.
- **dirfd-relative `openat`/`unlinkat` (CL-4).** `std::filesystem::remove_all` +
  `recursive_directory_iterator` walk via `openat(fd,name)`/`unlinkat(fd,name)` with
  a REAL dirfd, but the pouch wire is AT_FDCWD-only at v1.0 (a real dirfd ->
  ENOTSUP). The prover uses `fs::remove(abspath)` per-file instead. Widening the
  wire to accept a real dirfd (the kernel SYS_WALK_CREATE/SYS_UNLINK already take a
  parent_fd) is a CL-4 lift.

**The focused audit (CLOSED 0 P0 / 1 P1 / 0 P2 / 4 P3, NOT dirty).** Opus-4.8-max
(Fable depleted; MODEL start==end, no fallback) + a concurrent self-audit over
the boundary-line surface (`0027`, `build_libcxx`, the prover, the joey spawn).
The sharpest question -- the **`-D__linux__` split-personality ODR/ABI hazard**
(libc++abi built with `__linux__`, libc++ + the consumer without) -- was
prosecuted against the ACTUAL `~/projects/llvm-thylacine` @ `22.1.8` source and
**resolved SOUND**: every type that crosses the archive boundary
(`type_info`/`__cxa_exception`/the EH personality/`_Unwind_*`) is
`__linux__`-independent; the sole divergence (`__cxx_contention_t` -- `int32` vs
`int64`) is provably never referenced by libc++abi (its only `<atomic>` user is
`private_typeinfo.cpp`'s plain `.fetch_add`, which emits no contention symbol),
so the split materializes into no boundary symbol -- corroborated by the prover's
cross-archive throw + `dynamic_cast` passing. Findings:
- **F1 [P1, FIXED]** -- the prover's `fs::remove_all(dir)` pre-clean was DEAD (it
  recurses through dirfd-relative `unlinkat`, the CL-4 AT_FDCWD-only ENOTSUP
  seam), so on a PRESERVE=1 pool a prior partial-failure left the probe dir
  populated -> `create_directory` on the existing dir returns false-with-cleared-ec
  -> a boot-fatal `create_directory FAIL` that MASKS the true prior cause (the
  anti-masking-diagnostic class). Fixed to the AT_FDCWD-safe clean the cleanup
  loop already uses (`directory_iterator` + `fs::remove(abspath)`).
- **F2/F3/F4 [P3, FOLDED IN]** -- a post-build **nm-guard** pins the `-D__linux__`
  inert-property (fail LOUD if a fork bump makes `libc++abi.a` reference an
  atomic-wait/contention symbol); the reuse gate keys freshness on all three
  runtime trees (not just `libcxx/CMakeLists.txt`); the header stage `rm -rf`s
  the destination first (no ghost headers across a fork bump).
- **F2b + F5 [P3, TRACKED]** -- at CL-3, `Triple::Thylacine` must auto-define
  `__thylacine__` for the runtime AND every consumer (once `__thylacine__` header
  guards replace `-D__linux__`, a consumer left undefined re-introduces the split
  on a boundary type); the 0027 `remove()` lstat-dispatch TOCTOU window (forced by
  the #102 errno-loss gap, single-threaded-FS-benign) reverts to the classic
  atomic form once the #102 kernel errno restoration lands.

The pw_wake test-race that the CL-2 SMP gate surfaced (`cons.drain_poll_deferred_wake`)
was a PRE-EXISTING kernel-test-hygiene race whose fix (#58, `cons_test_mgr_hold` +
the error-string restructure) existed on the gfx track but had never merged into
this line -- cherry-picked (`8383ccad` -> `7df809c9`) as a separate prior commit;
the full SMP gate (default+UBSan x smp4/smp8 N=10 = 40/40) then passed 0
corruption (`memory/bug_pw_wake_drain_poll_test_leak.md`). Closed list:
`memory/audit_cl2_closed_list.md`.

### 16.15 CL-3 as-built (the real triple + the wrapper retirement)

CL-3 makes the fork clang THE pouch toolchain: `--target=aarch64-thylacine` now
resolves a real `Triple::Thylacine` (a `ThylacineTargetInfo` + a `Thylacine`
clang `ToolChain`) instead of an unknown OS, so the driver -- not a hand-rolled
`ld.lld` line -- drives the link. Landed in two sub-chunks.

**16.15a -- the driver (CL-3a; fork commit `df919c8dd`, branch `thylacine`, NOT
pushed -- the fork's origin is read-only upstream `llvm/llvm-project`).** Eight
files in `~/projects/llvm-thylacine` @ 22.1.8:
- `llvm/.../Triple.h` + `Triple.cpp`: the `Triple::Thylacine` enum value +
  `getOSTypeName`(`"thylacine"`) + `parseOS`(`.StartsWith("thylacine", ...)`) +
  `isOSThylacine()`. `LastOSType` advanced.
- `clang/.../Driver.cpp` + `CMakeLists.txt`: `#include "ToolChains/Thylacine.h"`
  + the `getToolChain` dispatch case + the new source in the build.
- `clang/.../ToolChains/Thylacine.{h,cpp}`: a Fuchsia-templated `ToolChain`
  subclass -- `RLT_CompilerRT`, `CST_Libcxx`, non-PIC/non-PIE, `ld.lld` default,
  and a `Linker::ConstructJob` that reproduces `tools/pouch-ld` verbatim (static
  / `-z max-page-size=4096` / `-z separate-loadable-segments` / `-z noexecstack`
  / `--build-id=none` / `--eh-frame-hdr` + crt1/crti + `-L<sysroot>/lib` +
  `--start-group -lc libclang_rt.builtins.a --end-group` + crtn; the C++ group
  `-lc++ -lc++abi -lunwind` added by `AddCXXStdlibLibArgs` when `CCCIsCXX`).
- `clang/.../Basic/Targets/OSTargets.h` + `Targets.cpp`: a `ThylacineTargetInfo`
  (`getOSDefines` -> `__thylacine__` + `__unix__` + `_GNU_SOURCE`-for-C++) +
  the aarch64 `AllocateTarget` case.

Verified host-side (a Release/AArch64 clang built in `~/projects/llvm-thylacine/build`):
`--target=aarch64-thylacine -dumpmachine` -> `aarch64-unknown-thylacine`; the C
`-###` link line is byte-for-byte `pouch-ld`'s (`ld.lld`, no Darwin `ld64` /
`-arch` / `platform_version`); C++ adds the `-lc++ -lc++abi -lunwind` group; real
C + C++ links produce a valid static `ET_EXEC` with 0 `PT_DYNAMIC` (the
`kernel/elf.c` acceptance shape), modulo a benign `PT_GNU_EH_FRAME` the loader
skips. **CL-3's gate (byte-compatible cross-build via the real triple) is MET.**
Host-build gotcha (re-needed on any reconfigure): LLVM's CMake adds
`-isystem /opt/homebrew/include`, whose Linux-style `uuid/uuid.h` shadows the
macOS SDK's -> `LockFileManager.cpp: unknown type name 'uuid_string_t'`; fix by
configuring `-DLLVM_ENABLE_{ZLIB,ZSTD,LIBXML2,TERMINFO,LIBEDIT,CURL,HTTPLIB}=OFF`.

**16.15b -- the wrapper retirement + F2b (CL-3b).** The pouch toolchain retires
onto the driver, and the CL-2 split-personality flags drop:
- `tools/pouch-clang` prefers `$POUCH_CC` (the fork `build/bin/clang`); a
  fork-less checkout falls back to homebrew clang (unknown-OS, compile-only).
- `tools/pouch-ld` -- when the fork clang is present -- is a thin shim over the
  driver (`clang --target=aarch64-thylacine --sysroot=$SR "$@"`), which supplies
  the CRT + libc + builtins itself. The hand-rolled `ld.lld` block remains only
  as the fork-less fallback, so a fresh checkout still links.
- `build_libcxx` builds the C++ runtime with the fork clang/clang++
  (`--target=aarch64-thylacine`), so `__thylacine__` is auto-defined and the
  surgical `-D__thylacine__=1` drops. The C++ prover links through the fork
  `clang++` *driver* (the ToolChain emits the `--start-group -lc++ -lc++abi
  -lunwind --end-group` + `--eh-frame-hdr` itself -- no hand-rolled group).
- **F2b closed at the root.** The `-D__linux__` that CL-2 used to unlock
  libc++abi's `__cxa_thread_atexit` (guard `#if __linux__ || __Fuchsia__`) is
  retired: the fork's `libcxxabi/src/cxa_thread_atexit.cpp` guard now reads
  `#if defined(__linux__) || defined(__Fuchsia__) || defined(__thylacine__)`
  (a 1-line fork patch that `build_libcxx` recompiles -- no clang rebuild). So
  libc++abi is built WITHOUT `__linux__`: its `__cxx_contention_t` is `int64`,
  identical to libc++/consumers -- the CL-2-audit int32/int64 ODR split is not
  merely inert now, it is ELIMINATED. The old atomic-wait-symbol tripwire that
  pinned the inertness retires with it.
- **The cxa_guard/gettid seam FIXED (the SMP gate caught the pre-existing bug).**
  The CL-3b SMP gate's first pass hit `bug_cxa_guard_gettid.md` 1/40 (ubsan-smp4):
  libc++abi's `__cxa_guard` recursion check (`cxa_guard_impl.h` `PlatformThreadID`)
  used `syscall(SYS_gettid)`, which on pouch is the ENOSYS sentinel, so every
  thread read back the same bogus id and a concurrent first-init of a
  function-local static false-aborted "recursive initialization". PRE-EXISTING
  (cxa_guard is byte-identical CL-2<->CL-3b; the gettid path keys on
  `defined(SYS_gettid)`, not `__linux__`; CL-2 passed 40/40 on luck). The seam was
  flagged ESCALATE only because the anticipated fix was a kernel-ABI gettid; but
  the `__APPLE__` branch already uses `pthread_self()`, so a matching
  `#elif defined(__thylacine__)` branch returning `pthread_self()` (a distinct
  per-thread value -- the id is only the recursion heuristic, the atomic init byte
  is the real synchronization) fixes it as a 1-branch fork patch, no ABI change.
  A deterministic regression (`pouch-hello-cxx` wire 7: NRACE threads barrier-sync
  then race one static's first-init) reproduced the abort reliably pre-fix and
  passes post-fix; it now runs on every boot, so the SMP gate exercises the
  concurrent cxa_guard path every time.

Proven in-guest: the fork-driver-linked `pouch-hello-*` + the fork-clang-built,
`clang++`-driver-linked `pouch-hello-cxx` all boot and pass -- `pouch-hello-cxx:
ALL C++ WIRES PASS` (EH/RTTI/threads/TLS-dtors/iostreams/std::filesystem + the
wire-7 concurrent cxa_guard race, with `-D__linux__` gone), boot OK, 0 EXTINCTION,
suite 1196/1196, SMP gate 40/40 (default+UBSan x smp4/smp8 N=10) 0 corruption.
Kernel byte-unchanged. Seam carried forward: unlink-path errno-loss
(`memory/bug_unlink_errno_loss.md`). CL-3b did NOT retire the wrappers for the
`sdl2`/`gnumake`/`tyrquake` *compile* (they keep homebrew clang; only their link
routes through the driver via `pouch-ld`).

### 16.16 CL-4 as-built (the device toolchain — the arc close)

`clang++ -O2` compiles, links via `ld.lld`, and runs a real C++ program **on the
device**. Reached by fixing a five-layer masking stack; the opening theory ("the
122 MB multicall dies pre-main") was **refuted** by a syscall trace showing ZERO
EL0 syscalls — it never ran at all.

| # | Layer | Root cause | Fix | VM? |
|---|---|---|---|---|
| 1 | exec | `elf_load` rejected `EI_OSABI != ELFOSABI_NONE`; lld stamps `ELFOSABI_GNU` | accept GNU(3) alongside NONE | no |
| 2 | musl TLS | `__init_tls` issues a RAW 6-arg Linux mmap for large TLS (clang++'s is 1232 B vs the ~128 B builtin), bypassing the patched 1-arg `__mmap` | dispatch accepts both ABIs, anonymous-shape-gated | no |
| 3 | console | clang's `FixupStandardFileDescriptors` fstats fds 0/1/2 and treats a non-EBADF failure as fatal; the console had no `.stat_native`, so `clang_main` returned 1 before emitting anything | `devcons_stat_native` + `devdev_stat_native` | no |
| 4 | driver | no `/proc/self/exe` and `realpath` fails, so `getMainExecutable` returned `""` -> empty `InstalledDir` -> clang found neither its resource dir nor `ld.lld` | fork CL-4b: return an absolute argv0 | **yes** |
| 5 | driver | the cc1 self-spawn argv carried the program name TWICE -> `-cc1` at argv[2] -> the child re-entered as a *driver* and rejected `-cc1` | fork CL-4c | **yes** |

**Layer 5 in full**, since it is the subtle one and the fix is upstream-shaped.
`clang/tools/driver/driver.cpp` sets `Driver::PrependArg` whenever
`ToolContext.NeedsPrependArg || CanonicalPrefixes` — and CanonicalPrefixes
defaults on. The comment justifies the disjunct with "PrependArg will be null so
setPrependArg will be a no-op", true only for **non-multicall** builds, whose
`main` passes a null PrependArg. In a multicall build `llvm-driver.cpp`'s
`MakeDriverArgs` *always* supplies one: on a direct invocation
(`ToolName == Argv0`) it returns `{Argv0, sys::path::filename(Argv0), false}` —
so `PrependArg == "clang++"` with `NeedsPrependArg == false`. `Command::Execute`
then emits `[Executable, PrependArg, ...Arguments]`.

That is harmless when the install uses **symlinks**: `getMainExecutable`
canonicalises `bin/clang++` back to `bin/llvm`, so the child really does need to
be told which tool to be. It is wrong for a plain **copy**, or on a target where
canonicalisation cannot happen and `Path` is just argv0 — the exec path already
names the tool, the child re-dispatches on argv[0], and the prepend only shifts
`-cc1` out of the position `clang_main` checks. Thylacine is both cases at once:
no symlinks (so `/clade/bin/clang++` is a copy) and no `/proc/self/exe`. The fix
prepends only when `filename(Path) != filename(PrependArg)`; symlink installs and
`llvm clang++ ...` are untouched, and **copy-based multicall installs on Linux —
equally broken upstream — are fixed too**.

Diagnosis order mattered for cost: `-no-canonical-prefixes` suppresses the
prepend, so it served as a free on-device confirmation AND unmasked the rest of
the driver surface, proving there was no layer 6 before spending on a rebuild.
All three shapes were then exercised — spawned cc1 (multi-job), **in-process cc1**
(`-c`, a single job; `Driver.cpp:5349` disables integrated-cc1 only above one
job, and the same stale `PrependArg` breaks it via `ExecuteCC1Tool`'s own
`ArgV[1] == "-cc1"`), and link-only — so one VM run sufficed.

**The gate** (`joey.c::clade_gate`, boot-fatal, every clade boot, **no** special
driver flags — nothing else would pass them): `clang++ --version` -> compile+link
`/tmp/hello.cpp` -> run it -> `-c` then link-only -> run that. The program is
deliberately a real one: `<vector>`/`<string>` exercise the libc++ headers and
template instantiation, and a `throw`/`catch` round trip exercises the CL-2 C++
runtime end to end in a freshly on-device-compiled binary — libc++abi's
personality routine plus libunwind walking `.eh_frame` emitted by that very
clang. A build that links but cannot unwind would sail through a printf-only
gate. Marker: `CLADE-HELLO sum=285 eh=1`.

**Focused audit** (Fable 5 max + concurrent self-audit): **0 P0 / 2 P1 / 1 P2 /
3 P3, NOT dirty**, all addressed in-commit — `memory/audit_cl4_closed_list.md`.
The headline F1: accepting the Linux 6-arg shape turned a **fail-closed** refusal
into **silent wrong data** — a direct `syscall(SYS_mmap, NULL, len, prot,
MAP_PRIVATE, fd, off)` used to land as `-1`/`MAP_FAILED` (ARCH §6.3's "no
file-backed mmap", loudly) and instead returned anonymous zero pages where the
caller asked for file bytes. Closed by gating the 6-arg reading on the exact
anonymous-private shape, extracted as the unit-testable
`burrow_lazy_len_from_args` (revert-probed: the pre-fix expression gives
1196/1197 FAIL). F2: the fstat fix covered only the `SYS_CONSOLE_OPEN` door — the
`#57b` `/dev` door mints **devdev** Spoors, so `clang++ < /dev/null` reproduced
the very bug being fixed.

Proof: suite **1197/1197**, `clade CL-4 gate: PASS` on both invocations, boot OK,
0 EXTINCTION.

### 16.17 CL-6 as-built (clangd + the Nora C/C++ client)

**The "near-zero client work" estimate in §11 was half right, and the half it
elided is the half that mattered.** Recorded here because the correction is
more useful than the original claim.

TRUE: `parley` — the whole 3919-line protocol layer — needed **zero** changes.
Every `go`/`gopls` mention in it is a comment or a test fixture. LSP really is
language-agnostic, and the client really did generalize without touching a line
of protocol code.

INCOMPLETE: `nora/src/lsp_host.rs`, the binary-side glue, was gopls-**bound** in
four load-bearing places — the binary path, the `didOpen` `languageId` (a
literal `"go"`), the workspace-root marker (`go.mod`), and the extension gate —
and, structurally, **there was no language dispatch at all**: `Lsp::start(path)`
took a path and decided nothing. A second server needed a concept the code did
not have. Small, but an abstraction rather than a substitution: ~150 lines.

The shape now is a `ServerSpec` table (suffix→`languageId`, binary, root
markers, status-message name) with `lang_for()` as the dispatch, and `Lsp`
holding its own spec for the session. Adding a language is a row, not a code
path. Two details worth keeping:

- The type is `ServerSpec`, not `Lang`, because `nora::Lang` already exists —
  the syntax highlighter's language enum, re-exported from `lib.rs`.
- An earlier draft of the table carried a comment claiming the suffix order was
  load-bearing ("`.h` would shadow `.hpp`"). **That was wrong**, and checking it
  took one command: `"x.hpp".ends_with(".h")` is `false` — a suffix match is not
  a prefix match. Order within a spec's list is cosmetic; order *across* `LANGS`
  is not.

**Packaging.** clangd is its own static binary at `/clade/bin/clangd`, not a
multicall dispatch name (§16.5: no `GENERATE_DRIVER`). It rides the one CL-4
cross-config with `clang-tools-extra` added, trimmed by `CLANGD_TIDY_CHECKS=OFF`
(drops `ALL_CLANG_TIDY_CHECKS`; clangd still links `clangTidy`/`clangTidyUtils`,
which its CMakeLists does unconditionally), plus DEXP/REMOTE/XPC/MALLOC_TRIM off.
`stage_clade` copies it when present and says so when absent — a pre-CL-6 tree
still stages a working C++ toolchain, and nora treats a missing binary as "no
language server", so the image degrades instead of breaking.

**The builder.** `tools/clade-gcp-build.sh` cross-builds this on a disposable
`t2a-standard-16` spot VM, because `build_clade` sizes its jobs as
(host RAM GiB / 3) against 2.46 GiB worst-case TUs — an 8 GiB dev box gets
**-j2**. The script deliberately duplicates **no** configuration: the VM clones
this repo and runs the real `tools/build.sh` (`sysroot`, `libcxx`, `clade`) with
the local working copy of `build.sh` overlaid, so the cmake args cannot drift
from the recipe they mirror. Two things it must get right, both learned the hard
way on the first run:

1. The detached launch needs `< /dev/null`. Redirecting only stdout and stderr
   leaves the child holding the ssh channel's **stdin**, so the launcher hangs
   for the build's entire duration while the build itself runs fine.
2. `LLVM_PREFIX`/`LLD_PREFIX` point at the **fork's own** stage-1 build, not the
   distro's llvm. `build_sysroot` uses a *stock* clang (the triple is driven
   explicitly — "clang treats thylacine as an unknown OS"), and on the dev box
   that stock clang is 22.1.4 while Ubuntu ships 18. Four majors of skew on the
   sysroot everything else links against is not worth accepting when the fork
   toolchain is already being built: the v8.0 userspace floor (#71) rides
   `-moutline-atomics`, and task #91 exists because an LSE regression there is
   **silent**.

**The permanent builder (CL-7, #108/#110).** Once the wanted artifact stops
being the multicall binary and becomes the *build tree* — CL-7 links Mesa against
the cross-LLVM's 207 static libraries — a disposable VM is the wrong shape, since
each iteration re-creates ~45 GiB of LLVM. `tools/clade-keep-build.sh` drives a
machine that is **stopped, never deleted** (`thyla-keep`; the name sits outside
`clade-gcp-build.sh`'s `clade-builder-*` prefix precisely so that tool's teardown
cannot reach it), whose `/build` disk persists, turning a 24-minute cold build
into an incremental `ninja`. Its stage 3 adds the ORC/ExecutionEngine slice that
clang and lld never needed — the reason #108 looked like "there is no linkable
LLVM" when 127 libraries were already sitting in the tree.

Stage 1 is now `tools/clade-stage1.sh`, called by **both** drivers. It is the one
part of the build `build.sh` cannot express (every `build.sh` target *consumes* a
fork clang that already knows the triple), so the recipe has to live somewhere —
and it must live in exactly one place, or the copy passes its own checks while
silently disagreeing with the original, which is this tree's `struct t_stat`
failure mode (#100). Three bugs surfaced only once the VM-only copies came under
review, all of the same family — **a failure that reports success**: an empty
`ninja $WANT` silently meant *build everything* while its verify loop iterated
zero times; that empty list masked a SIGPIPE-under-`pipefail` race in the verify
loop itself (`readelf | grep -m1` on a multi-member archive, which loses the race
to a file and wins it to a tty); and bash does not inherit an `ERR` trap into
functions, so a dead stage left its status file reading `running` forever. The
recipe's own drift test is cheap and worth keeping: re-run stage 1 against an
existing tree and require `ninja: no work to do.`

**The gate: `lsp-probe`, generalized rather than forked.** The owed in-guest
round-trip is the CL-6 twin of the gopls E2E (#76), and it is the SAME probe.
Host unit tests were never the vehicle — `lsp_host.rs` lives in the binary
crate, which `lib.rs` documents as not host-testable ("the bin needs the
backend"), and moving a server table into the pure editor engine to win an
`ends_with` test is the wrong trade.

The probe's chain (spawn → `initialize` → `initialized` → `didOpen` → wait for
`publishDiagnostics` carrying a planted identifier at a planted line) is
*protocol*, not language: only the binary, workspace, `languageId`, and error
position differ. So the differences moved into a `PROBES` table and the loop
runs one session per present server. Forking a second probe would have
duplicated ~300 lines of session loop whose two copies could then drift — and a
drifted copy still passes its own gate, which is the failure mode that produced
#100's sibling, the `struct t_stat` mirrors.

**Three sessions, and the third is the one that earned its keep.** They are
deliberately separate claims, so their failures are distinguishable in the boot
log:

| session | claims | result |
|---|---|---|
| `gopls` | the generalized client did not regress the working path | 83 ms, peak 7 MiB |
| `clangd` | the protocol round-trip works for C++ | 185 ms, peak 5 MiB |
| `clangd+headers` | clangd is actually *configured* — `#include` resolves | 1281 ms, peak **145 MiB** |

CL-6's done-definition is "diagnostics/**hover/def** on a C++ file", so the
first and third sessions assert all three. Hover and definition are pure
protocol — the same `parley` code for every language — which makes it tempting
to infer them from the Go side. That inference is precisely what "clangd will
find its headers" was, so they are asked for instead:

```
[gopls]          hover OK -- "func Probe() int"
                 definition OK -> file:///tmp/lspp/main.go:2
[clangd+headers] hover OK -- "class vector<int, std::allocator<int>>"
                 definition OK -> file:///clade/sysroot/include/c%2B%2B/v1/__vector/vector.h:86
```

(`c%2B%2B` is `c++` — LSP locations are URIs, so the `+`s are percent-encoded
on the wire. Quoted verbatim rather than prettified, so a future reader greps
the boot log for what is actually in it.)

The definition target is the load-bearing one: landing *inside libc++* proves
clangd **indexed** the header, which no diagnostic can show — a file can be
found and never indexed. (It also confirms hover/def on the Go side, which
8e-2c had only unit coverage for.)

The include-free spec cannot prove the third thing: a clangd that resolves **no
header at all** still reports the planted undefined identifier, so it would pass
while `#include` — most of what makes an editor useful for C++ — is completely
broken. That is why `ProbeSpec` carries `forbid`, a list of messages that must
be ABSENT, checked *before* the planted match so a forbidden diagnostic cannot
be masked by a successful one.

It caught exactly that on first run: **`'vector' file not found`**. The cause
was a wrong claim in this codebase's own comment, mine: I had written that
`--sysroot` alone lets the driver derive `c++/v1`, generalizing from the /storm
Makefile (**C-only**, so silent on the question) and the libc++ CMake flags
(which build libc++ *itself*, headers supplied by CMake). The one recipe that
compiles a C++ TU **against** installed libc++ — build.sh's `/pouch-hello-cxx`
consumer line, ~100 lines below the one I read — says the opposite explicitly,
and its own comment names it: "the pouch C++ consumer flags: … **explicit
-isystem c++/v1**". `-nostdlibinc` suppresses the standard *library* include
dirs, C++ ones included. The probe's compile command now mirrors that recipe
byte-for-byte, with a comment saying the flag is load-bearing and that this spec
is what fails if someone "simplifies" it.

**#100, answered with a number rather than re-guessed.** The probe reads the
server's peak anon commit from `/proc/<pid>/status` before the reap (the
kernel's `peak` is monotonic, so a live read cannot under-report; reading after
the reap is impossible — the reap frees the Proc and the counter with it) and
prints `peak=N MiB/BUDGET MiB` on every PASS line.

The axis is the right one: `page_count`/`page_peak` count anon pages, exactly
what `PROC_PAGE_MAX` bounds, and clangd's 44 MB of text is file-backed via
REVENANT so it is *not* on this axis — 146 MiB is genuinely heap. Cross-checked
against CL-5's cc1 measurement (250 MiB, 97.8% of budget, on a 1959-byte
template-heavy TU): same axis, same frontend, directly comparable.

So **one `#include <vector>` costs 57% of the default budget**, and a real
project header set will exceed it. The risk is confirmed, not hypothetical —
and the trivial-file number alone (5 MiB) would have been reassuring and wrong.
The fix is not built here: the kernel half already exists (`sys_spawn_args`
`page_budget` @92 + the `SPAWN_PERM_MAY_RAISE_PAGE_BUDGET` gate, CL-5), but
`Command` has no setter, and whether nora may confer a raised budget on its
clangd child is a privilege question, not a plumbing one. Its own chunk.

**Still owed:** a `compile_commands.json` generator, so a real project gets
these flags without hand-writing the database.

### 16.18 CL-7k as-built (the JIT capability, I-42)

`1f0e66c0` kernel + `5633d056` userspace. Full as-built reference:
`docs/reference/145-jit.md`.

**The estimate held.** §8 predicted "the kernel pieces are small" and they were
— but the reason is worth recording: **nothing needed relaxing**. The VMA layer
already rejects W|X per mapping, `make_user_pte_l3` already encodes AP/UXN from
prot, and `vma_find_gap` already hands out disjoint ranges. Two aliases of one
Burrow were therefore *already* expressible; what was missing was a syscall to
ask for them and a rule about who may. The W^X design paid for itself here: a
system that enforced W^X with a mutable per-page bit would have had to grow a
new enforcement path for this, and that path is where the bugs would live.

Three shape decisions, each with a live alternative:

- **One syscall installs both aliases.** Splitting create from map would admit
  an RX-alias-with-no-writer state and push the half-installed rollback onto the
  caller. Both go in under one `vma_lock` hold. Also literally §8's own
  userspace shape: *create → (writer_ptr, exec_ptr)*.
- **`SYS_ICACHE_SYNC` syncs the kernel direct map, never the user VA.** `dc
  cvau`/`ic ivau` can take translation faults, and a user VA is exactly what a
  caller can arrange to be unmapped. Architecturally exact on ARMv8 (PIPT data
  caches; `IC IVAU` invalidates all aliases of the PA) — the same reason Linux's
  `flush_icache_range` works on linear-map addresses for module text.
- **DESTROY and SYNC are not CAP_JIT-gated.** Authority to *create* is scarce;
  releasing or publishing what you already own is not. Gating them would turn a
  legate-scope expiry into a leak.

**CAP_JIT is elevation-only** (`CAP_ELEVATION_ONLY` + `CAP_GRANTABLE_CLEARANCE`,
like `CAP_DEBUG`), which is an invariant obligation rather than taste: I-42's
text requires non-heritability. Consequence: a corvus clearance is the *only*
path to the capability. A `jit` clearance level lands in `usr/corvus` — the
level table's own comment had anticipated exactly this, noting that a `hw-dev`
level was impossible because `CAP_HW_CREATE` is fork-grantable, while `CAP_JIT`
is not.

**A scripture-wording defect, recorded not silently resolved.** All three
documents (JIT-ON-WX, this file §8, ARCH §28 I-42) say *"elevation-only,
non-rfork-grantable, the `CAP_HW_CREATE` class"*. Read literally the trailing
phrase is **wrong** — `CAP_HW_CREATE` is fork-grantable, which contradicts both
the words beside it and I-42's non-heritable clause. Implemented per the
explicit properties; flagged in `caps.h` so a later reader does not "fix" the
bit toward `CAP_ALL`.

**Three defects, all found by measuring:**

1. The cap denial returned `-T_E_PERM`, which `errno.h` **forbids** (value 1
   collides with the flat `-1` generic sentinel, so libthyla-rs decodes it as
   `Io`). A capless caller would have been told "I/O error". Now `-T_E_ACCES`.
   The kernel test asserted the wrong constant and passed — caught by reading
   `err.rs`'s TRAPS note while writing the client.
2. `uaccess_copy_out` returns 0/-1, **not a byte count**; the check against
   `sizeof(reg)` made every *successful* create report EFAULT and tear itself
   down. Structurally invisible to the kernel tests, which drive the mechanism
   *below* the copy-out. The in-guest prover caught it on its first real run.
3. **Self-audit:** `SYS_BURROW_DETACH` accepted a code alias. One region carries
   ONE I-32 charge but has TWO aliases, so detaching both refunded it twice —
   a CAP_JIT holder could loop create-then-detach-both to drive `page_count` to
   zero while real usage never moved, then allocate a fresh `PROC_PAGE_MAX`. A
   bound a capability holder can zero is not a bound. Now refused outright: the
   JIT syscalls own that lifetime, and the same condition closes the orphaned-
   alias leak (detach one, and `SYS_JIT_DESTROY` refuses the survivor for having
   no peer).

**Proof.** The invariant test asserts on **real L3 page-table entries**, not VMA
prots — the prot is intent, the PTE is what the MMU consults — and on both
aliases resolving to the **same physical page**, without which "dual map" is
unproven. Revert-probed: mapping the exec alias RW fails the suite on exactly
*"exec PTE is AP_RO (not writable)"*. The detach gate is revert-probed too.

In-guest (`/jit-prover`, boot-fatal), one process across the capability
boundary so `CAP_JIT` is the only variable:

    ungated create REFUSED (no CAP_JIT) -- correct
    CAP_JIT acquired via the jit clearance
    JITed fn(35,100) = 142 -- emitted, published, EXECUTED
    re-emitted fn returns 99 -- icache invalidate is live

The re-emit leg matters: a missing icache invalidate shows up there as "the old
function keeps running" — no fault, no crash, just a stale answer.

Also: `JOEY_BLOB_MAX` 512 → 768 KiB. Measured before bumping — joey was at
523,656 of 524,288 bytes (**99.88% full**) *before* this chunk. Any next
addition would have tipped it; the 36→65→128→256→384→512 history is a record of
bumping to just-past-what-tipped-it.

**NEXT: CL-7** (Mesa/llvmpipe + GLQuake) — the ORC `DualMapMemoryMapper` over
`CodeRegion` is its first consumer.

---

## 17. Revision history

| Date | Change |
|---|---|
| 2026-07-23 | Initial draft: research pass (tree + external) + the full arc design; forks §14 open. |
| 2026-07-23 | SIGNED OFF — all §14 leans adopted verbatim; JIT invariant renumbered I-41 → I-42 (I-41 reserved by ADVANCED-GO AG-2 between draft and signoff); moved to the main tree for the scripture commit. |
| 2026-07-23 | **CL-0 landed** (§16): syscall-gap census closed (zero new kernel syscalls for CL-1..CL-4; `renameat`+`getdents64` per-compile load-bearing), environ CLOSED (envp always empty), lld-in-multicall VERIFIED, Mesa OSMesa-removal correction (§16.6), F4 validated by measurement (worst TU 2.46 GiB). Instruments: disposable GCP ARM VM (torn down) + the fork clone @ 22.1.8. |
| 2026-07-23 | **CL-1a landed** (§16.9): the pouch FS/process wires (`0024`, 20 files) -- getpid/chdir/getcwd/mkdir/open(O_CREAT)/rename/unlink/readdir/ftruncate/fchmod/access, each onto an existing kernel syscall (ZERO new kernel surface); the `__pouch_open_parent` path-split helper; openat's O_CREAT arm + relative-path lift. Proven in-guest by `/pouch-hello-fs` (ALL WIRES PASS, boot OK, 0 EXTINCTION). dup2/dup3/pipe2 deferred to CL-1b (not clean 1:1). Surfaced + enqueued an ftruncate shrink-after-sparse-extend EIO below the wire (Stratum `stm_fs_truncate`; `memory/bug_ftruncate_shrink_after_extend.md`). |
| 2026-07-23 | **CL-1b-0 landed** (§16.10): the pouch-env crt boundary line (`0025`, `_pouch_env.c` + `__libc_start_main` hook) -- populate `__environ` from the `/env` device at startup so `getenv()`/`environ` work (kernel writes envp[0]=NULL). Fail-soft. Proven in-guest by `/pouch-hello-env` (PGENV1/PGENVNUM inherited via the rfork clone; boot OK, 0 EXTINCTION, suite 1196/1196). Pure userspace (kernel byte-unchanged). NEXT = CL-1b core (posix_spawn/wait4/dup2/pipe2). |
| 2026-07-23 | **CL-1b core landed** (§16.11): the process lifecycle (`0026`, 10 files) -- posix_spawn (STATIC file_actions resolve -> positional SYS_SPAWN_FULL_ARGV fd_list), posix_spawnp (PATH search), wait4/waitpid (SYS_WAIT_PID + flag/status translation), pipe/pipe2 (2-reg svc shim), dup2/dup3 (old==new; onto-target ENOSYS). Proven in-guest by `/pouch-hello-spawn` (pipe2+posix_spawn+waitpid; WEXITSTATUS ok=0 fail=1; argv pass-through). Self-audit fixed a P1 (opened[] overflow); ground-truth bring-up fixed the dup2-rights/argv0-NULL/std-fd-seed issues. Boot OK, 0 EXTINCTION, suite 1196/1196 (kernel byte-unchanged). Focused audit CLOSED CLEAN (Opus-4.8-max + self-audit; 0 P0/0 P1/0 P2/6 P3, NOT dirty; F4 argv-bound + F1 dup-onto-target comment fixed; the freopen onto-target ENOSYS is a tracked kernel-primitive seam). `memory/audit_cl1b_closed_list.md`. NEXT = CL-1c (make). |
| 2026-07-24 | **CL-1c-1 landed** (§16.12): the GNU make 4.4.1 **port** (vendored, not a musl patch) -- `third_party/gnumake/` pruned-pristine (sha256 `dd16fb1d…`) + `usr/ports/gnumake/{config.h,generated/}` (the Thylacine delta; `patches/` EMPTY -- zero source edits) + `build_gnumake()`. The census (§16 Q1-10) picked the clean config: `USE_POSIX_SPAWN=1` (make natively drives CL-1b's posix_spawn/wait4) + `MAKE_JOBSERVER` UNDEFINED (top-level `make -jN` = pure job_slots counter + blocking waitpid, no pipe/fifo/pselect/SIGCHLD/fcntl-O_NONBLOCK); no fcntl boundary-line needed. 35 objects (30 src + 5 lib gnulib) compile+link clean against the pouch sysroot -> 371 KB static ET_EXEC. Proven in-guest by `/make --version` (`GNU Make 4.4.1` + `Built for aarch64-thylacine`), boot OK, 0 EXTINCTION, CL-1a/1b siblings unregressed (kernel byte-unchanged). `--version` doesn't spawn, so this is the load-and-run milestone; the parallel-spawn `make -j` gate + the boundary-line audit are CL-1c-2. Flagged seams (neither needed for the gate): execvp self-re-exec (self-remaking makefiles only; owed at CL-4/CL-5) + adddup2-onto-0/1/2 (handled by CL-1b's static fd-list resolve). |
| 2026-07-24 | **CL-1c-2 landed + THE CL-1c ARC IS COMPLETE** (§16.13): the on-device `make -j3` gate -- a joey probe writes a toy project to `/tmp/mkt` (3 independent shell-free `/bin/cp` compiles + a dependent link, ALL absolute paths) + runs `make -f /tmp/mkt/Makefile -j3`, verifying all 4 outputs by content -- proving make DRIVES CL-1b's posix_spawn/wait4 under `-j` parallelism (the job_slots counter + reap-any `waitpid(-1)`). `/make -j3 PASS`, boot OK, 0 EXT, suite 1196/1196 (kernel byte-unchanged). Bringup: `/tmp` created before the probe + a trailing-NUL on the 4-arg argv blob (the kernel argv parser requires it). **Boundary-line audit CLOSED 0 P0 / 1 P1 / 0 P2 / 4 P3, NOT dirty** (Opus-4.8-max fallback [Fable depleted] + self-audit, CONVERGED on the P1): F1 [P1] = a PRE-EXISTING (LS-4) kernel getcwd bug the make oracle SURFACED (`sys_getcwd_handler` rejects buf > SYS_OPEN_PATH_MAX+1=1025; make passes PATH_MAX=4096 -> EIO) -- benign here (make degrades gracefully; abs-path gate unaffected), does NOT block the close, a probable CL-2 blocker, tracked `memory/bug_getcwd_oversized_buffer.md` + fixed as a separate kernel chunk; F2 [P3, FIXED] mkt_file_eq short-read -> read_exact loop; F3 [P3, FIXED] 2 inert darwin CF config macros -> undef; F4/F5 [P3, SEAMS] no-/bin/sh shell recipes + sub-second mtime -> CL-4. `memory/audit_cl1c_closed_list.md`. NEXT: the getcwd kernel fix, then CL-2 (C++ runtime) / CL-3 (Triple::Thylacine), parallelizable. |
| 2026-07-24 | **CL-2 landed** (§16.14): the C++ runtime -- libunwind + libc++abi + libc++, static, cross-built via `LLVM_ENABLE_RUNTIMES` against the pouch musl sysroot (`build_libcxx`; sources from the `$LLVMFORK` @ 22.1.8, absent-fork-safe) -- installed into `build/sysroot` + a C++ prover `/bin/pouch-hello-cxx`. Config ground-truthed: `--target=aarch64-thylacine` (unknown OS -> the correct GENERIC atomic-wait fallback, NOT the broken raw-`syscall(SYS_futex)` Linux path) + `CMAKE_SYSTEM_NAME=Linux` (archiver-only, uses llvm-ar not Apple libtool) + `LIBCXX_HAS_PTHREAD_API=ON` + `LIBCXXABI_HAS_CXA_THREAD_ATEXIT_IMPL=OFF` + a SURGICAL `LIBCXXABI_ADDITIONAL_COMPILE_FLAGS=-D__linux__` (libc++abi-only, to unlock `__cxa_thread_atexit`'s `__linux__`-guarded definition) + `LIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF` + `_GNU_SOURCE`. **Proven in-guest**: `pouch-hello-cxx: ALL C++ WIRES PASS` -- exceptions/RTTI/threads/thread_local-dtors/iostreams/std::filesystem all live; boot OK, 0 EXTINCTION, suite 1196/1196 (kernel byte-unchanged). Surfaced + FIXED a latent CL-1a gap (`0027-pouch-remove.patch`: musl's `stdio/remove.c` used a raw `__syscall(SYS_unlinkat)` -> ENOSYS + relied on EISDIR; now lstat-dispatch through the pouch-wired `unlink()`/`rmdir()`). Two documented SEAMS: the `__cxa_guard`/`gettid` concurrent-static-init false-abort (`memory/bug_cxa_guard_gettid.md`, ESCALATE) + dirfd-relative `openat`/`unlinkat` for `remove_all`/`recursive_directory_iterator` (CL-4). Also tracked: a #102-class unlink-path errno-loss kernel gap (`memory/bug_unlink_errno_loss.md`). **Focused audit CLOSED 0 P0 / 1 P1 / 0 P2 / 4 P3, NOT dirty** (Opus-4.8-max [Fable depleted; MODEL start==end] + self-audit): the `-D__linux__` split-personality ODR/ABI question resolved SOUND against the real `~/projects/llvm-thylacine` @ 22.1.8 (every boundary-crossing type is `__linux__`-independent; the sole divergence `__cxx_contention_t` is never referenced by libc++abi); F1 [P1] the prover's dead `fs::remove_all` pre-clean (dirfd-relative -> the CL-4 ENOTSUP seam) -> a `create_directory` masking-diagnostic false-failure under PRESERVE=1 pool reuse -> FIXED (AT_FDCWD-safe clean); F2/F3/F4 [P3] FOLDED (contention-symbol nm-guard + 3-tree reuse freshness + header-dest `rm -rf`); F2b (CL-3 `__thylacine__` auto-define) + F5 (0027 TOCTOU -> #102 errno restoration) TRACKED. The CL-2 SMP gate surfaced a PRE-EXISTING pw_wake kernel-test race whose #58 fix (`cons_test_mgr_hold`) existed on the gfx track but never merged -- cherry-picked (`8383ccad` -> `7df809c9`); full SMP gate 40/40 (default+UBSan x smp4/smp8 N=10) 0 corruption. `memory/audit_cl2_closed_list.md`. NEXT: CL-3 (Triple::Thylacine). |
| 2026-07-24 | **CL-3a landed** (§16.15a): the real driver in the fork (`~/projects/llvm-thylacine` @ 22.1.8, branch `thylacine`, commit `df919c8dd` — NOT pushed; the fork origin is read-only upstream). Eight files: `Triple::Thylacine` (enum/name/parse/`isOSThylacine`) + the `getToolChain` dispatch + a Fuchsia-templated `Thylacine` `ToolChain` whose `Linker::ConstructJob` reproduces `tools/pouch-ld` verbatim + a `ThylacineTargetInfo` (auto-defines `__thylacine__`/`__unix__`/`_GNU_SOURCE`-for-C++). Verified host-side: `-dumpmachine` → `aarch64-unknown-thylacine`; the C/C++ `-###` link lines == `pouch-ld`/the C++ group (`ld.lld`, no Darwin `ld64`); real C+C++ links → valid static `ET_EXEC`, 0 `PT_DYNAMIC`. **CL-3's gate MET.** Host-build gotcha fixed (Homebrew `uuid.h` shadow → `-DLLVM_ENABLE_{ZLIB,ZSTD,LIBXML2,TERMINFO,LIBEDIT,CURL,HTTPLIB}=OFF`). Thylacine tree unchanged (fork-only). |
| 2026-07-24 | **CL-3b landed — THE CL-3 ARC IS COMPLETE** (§16.15b): the wrapper retirement + F2b closed. `tools/pouch-clang` prefers the fork clang; `tools/pouch-ld` becomes a thin shim over the fork driver (which supplies CRT+libc+builtins), the hand-rolled `ld.lld` kept only as the fork-less fallback; `build_libcxx` builds the C++ runtime with the fork clang/clang++ and links the prover through the `clang++` driver. **F2b closed at the root**: a 1-line fork guard patch (`libcxxabi/.../cxa_thread_atexit.cpp`: `#if __linux__ || __Fuchsia__ || __thylacine__`, recompiled by `build_libcxx` — no clang rebuild) retires the surgical `-D__linux__`, so libc++abi's `__cxx_contention_t` is `int64` like everyone else — the CL-2 int32/int64 ODR split is ELIMINATED, not merely inert; the old tripwire retires. Also dropped the redundant `-D__thylacine__=1` (now auto-defined). Proven in-guest: fork-driver-linked `pouch-hello-*` + fork-clang-built `pouch-hello-cxx` boot; `ALL C++ WIRES PASS` (with `-D__linux__` gone), boot OK, 0 EXTINCTION, suite 1196/1196, SMP 40/40 (default+UBSan × smp4/smp8 N=10) 0 corruption. Kernel byte-unchanged (host toolchain only). **The SMP gate caught + closed the pre-existing cxa_guard/gettid seam** (`bug_cxa_guard_gettid.md`): 1/40 (ubsan-smp4) false-aborted a concurrent static-init because libc++abi's `__cxa_guard` used `syscall(SYS_gettid)`=ENOSYS (shared bogus id). Fixed by a fork `cxa_guard_impl.h` `PlatformThreadID` `__thylacine__` branch using `pthread_self()` (a real per-thread id; no ABI change — dissolves the ESCALATE) + a deterministic `pouch-hello-cxx` wire-7 regression (reliable abort pre-fix, passes post-fix, runs every boot); re-gate 40/40 clean. A cleanup-collateral detour: the disk prune had removed `~/.rustup/toolchains/` — restored (stable 1.97.1 + `aarch64-unknown-none`). Seam carried: unlink-path errno-loss. NEXT: CL-4 (the device toolchain + Support-layer port). |
| 2026-07-27 | **CL-4 landed — THE CL-4 ARC IS COMPLETE** (section 16.16): `clang++ -O2` compiles, links via `ld.lld`, and runs a real C++ program ON THE DEVICE. Five masking layers, found in order: (1) `elf_load` rejected `ELFOSABI_GNU`, which lld stamps for `SHF_GNU_RETAIN` on `.bss` -- a link-time flag with no runtime meaning (ground-truthed on the UNSTRIPPED 122 MB binary: 340,474 symbols, zero `STT_GNU_IFUNC`, zero `R_AARCH64_IRELATIVE`, no `PT_DYNAMIC`); (2) musl's `__init_tls` issues a RAW 6-arg Linux mmap for clang++'s 1232-byte TLS, bypassing the patched 1-arg `__mmap`; (3) clang's `FixupStandardFileDescriptors` fstats fds 0/1/2 and treats a non-EBADF failure as fatal, but the console had no `.stat_native`; (4) fork CL-4b `ce5a1c519` -- no `/proc/self/exe`, so `getMainExecutable` returned "" and `InstalledDir` was empty; (5) fork CL-4c `e7d6be5f8` -- the multicall's `PrependArg` is set whenever `NeedsPrependArg || CanonicalPrefixes`, so a directly-invoked COPY got the tool name prepended anyway, shifting `-cc1` to argv[2] and making the cc1 child re-enter as a driver (upstream-shaped: copy-based multicall installs on Linux are broken identically). The opening theory 'dies pre-main' was REFUTED by a syscall trace showing zero EL0 syscalls. `-no-canonical-prefixes` served as a free on-device confirmation that also unmasked the rest of the driver surface, proving no layer 6 before spending on the cross-build (one spot VM, torn down). Durable fork delta now `usr/ports/llvm/patches/0001..0006` (this also captured CL-4b, which had never reached the durable set). Gate is boot-fatal with NO special driver flags and covers spawned cc1, in-process cc1 (`-c`), and link-only, against a program with `<vector>`/`<string>` and a live throw/catch -- so libc++abi + libunwind are proven on a freshly on-device-compiled binary, not just printf. **Focused audit CLOSED 0 P0 / 2 P1 / 1 P2 / 3 P3, NOT dirty** (Fable 5 max + self-audit): F1 [P1] the 6-arg acceptance converted a fail-closed refusal into SILENT WRONG DATA (a direct file-backed `syscall(SYS_mmap, ...)` got anonymous zeros instead of `MAP_FAILED`) -> gated on the exact anonymous-private shape via the unit-testable `burrow_lazy_len_from_args`, revert-probed 1196/1197 FAIL; F2 [P1] the fstat fix covered only the `SYS_CONSOLE_OPEN` door, so `clang++ < /dev/null` reproduced the same bug through `/dev` -> `devdev_stat_native`; F3 [P2] `x0 ? x0 : x1` voided libthyla-rs's documented `length == 0 -> -1` nondeterministically -> `in("x1") 0` pinned. Suite 1197/1197, `clade CL-4 gate: PASS`, boot OK, 0 EXTINCTION. `memory/audit_cl4_closed_list.md`. NEXT: CL-5. |
| 2026-07-30 | **CL-7 entry decision LANDED (section 16.19): the section-16.6 frontend fork RESOLVED to option (i) by measurement, per its own instruction.** Pin = **`mesa-26.1.6`** (section 4's "current 25.x-era release" was a year stale; OSMesa confirmed absent, 0 files in the tree mention it). **Option (i) COMPILES**: the 25.0.7 gallium OSMesa frontend grafted onto 26.1.6 and built against fork LLVM 22.1.8 -> a 385,464-byte AArch64 object defining all nine public `OSMesa*` entry points, with **ZERO C source changes**; the whole drift is 3 build-graph fixes (`inc_mapi` gone; `glapi/glapi.h` moved to `src/mesa/glapi/glapi/`; `with_shared_glapi`/`libglapi_static` gone, referenced only by the outer target boilerplate Thylacine rewrites static). Corrected section 16.6's "ONE file": the measured delta is **6 files / 1,392 insertions** (+ ~40 lines of our own static target build file). **Option (ii) rejected as STRUCTURALLY incompatible, not merely larger**: `-Ddefault_library=static` still emits 4 shared libs because `src/loader/loader.c:883` `dlopen`s the gallium driver (the DRI loader model IS dynamic loading; Thylacine is static-only), it yields GLES-only with no `libGL` (the gate is GLQuake, desktop GL), and it is ~36x the surface (~37,500 lines vs 1,392). Option (iii) stays the follow-on refinement OF (i). **Three build requirements found, all of the wrong-default-that-builds-clean family**: (a) **`-Dllvm-orcjit=true` is MANDATORY** and section 16.6's stated mechanism was WRONG -- `llvm_has_mcjit` is a CPU-FAMILY list that INCLUDES aarch64, so ORC is not auto-selected; measured both ways (`GALLIVM_USE_ORCJIT=1` with, `=0` without), and the `=0` path bypasses ORC's `MemoryMapper` -- the exact seam CL-7k's `DualMapMemoryMapper` plugs into -- so a forgetful build would silently defeat I-42 and still compile. (b) **`LLVM_ENABLE_RTTI=ON` is REQUIRED** on the clade LLVM (neither `clade-stage1.sh` nor `build_clade` set it, so both inherit upstream's RTTI-off default): Mesa then demands `-Dcpp_rtti=false`, which makes `lp_bld_init_orc.cpp:246`'s `dynamic_cast<orc::SimpleCompiler&>` fail outright -- the ORC path CANNOT build against an RTTI-less LLVM, so RTTI is the only resolution (and the distro-standard setting). PROVEN: stage1 rebuilt RTTI=ON (3,237 edges) -> `--has-rtti YES` -> the ORC backend (6,522,552-byte object) AND the resurrected frontend both compile, 0 errors, `USE_ORCJIT=1`. CL-7a therefore starts with a clade-LLVM rebuild. (c) Mesa names `'mcjit'` in `llvm_modules` UNCONDITIONALLY even for ORC, and `llvm-config --shared-mode` takes NO module list so it enumerates every component and rejects a partial tree WHOLESALE (19 unbuilt libs were doing exactly that) -- both closed in `tools/clade-keep-build.sh` (MCJIT+Interpreter moved to required; new stage 3b completes the set from llvm-config's own complaint and asserts `--shared-mode` passes). Owed to CL-7a: a shim cross `llvm-config` (`NATIVE/bin/llvm-config` reports its own 4-archive libdir, not the target's 207), the full llvmpipe link, the on-device run. Instrument: `thyla-keep` (stopped after). Thylacine tree: docs + `tools/` only. |
| 2026-07-30 | **CL-7a-2 landed (section 16.21): the Mesa OS-port layer, and the llvmpipe link CLOSES.** Thylacine joins Mesa's `detect_os.h` at the **`DETECT_OS_POSIX_LITE`** tier -- the tier Fuchsia introduced, and the honest one (pthreads/mmap/poll/clock_gettime/nanosleep/sched_yield/sockets yes; fork, dynamic loader, `/proc/self/exe` no). Before this every `DETECT_OS_*` was 0 and Mesa compiled as if for a freestanding target. CL-7a-1's "exactly 3 failing TUs" was **incomplete and knowably so** -- it came from a ninja run without `-k 0`, which stops at the first failure; building with `-k 0` from the start gave 979 objects and a fourth TU. Four arms, each added the way Managarm was added to the same lists: `os_time.c` takes **both** clock_nanosleep arms incl. `TIMER_ABSTIME` (the seam numbers say `__NR_nanosleep 0xFFFF`, which reads as "no sleep here" and is WRONG -- `0022-pouch-nanosleep.patch` rewrote the *caller* onto `SYS_TORPOR_WAIT`, so the number is dead code and Thylacine takes Mesa's PREFERRED path, not a fallback); `os_misc.c` the `<unistd.h>` arm (the memory/page-size functions need none -- gated on `HAVE_SYSCONF`, which the cross configure detects from musl); `log.c` the u_process.h include (a genuine **upstream latent bug**: it calls `util_get_process_name()` under `!DETECT_OS_WINDOWS` but includes the header under `DETECT_OS_POSIX`, so any POSIX_LITE-only platform -- Fuchsia included -- compiles the call undeclared; fixing it *there* would newly compile `<syslog.h>` on Fuchsia, untestable from here, so we join the arm rather than ship an unverifiable change to someone else's platform); and `drm-uapi/drm.h`, the fourth TU CL-7a-1 never saw -- `lp_texture.c` includes `drm_fourcc.h` under a bare `#ifndef _WIN32` and uses `DRM_FORMAT_MOD_LINEAR` under that SAME gate, so the include cannot be skipped, and the tempting fix is wrong (drm.h's `__linux__` arm wants `<linux/types.h>`+`<asm/ioctl.h>`, **neither in the pouch sysroot** -- measured before choosing), so Thylacine takes the existing `__GNU__` (Hurd) escape to `<sys/ioctl.h>`: one line. **Then the archive lied and the executable told the truth.** All 982 objects compiled and the 210 MB `libOSMesa.a` built while missing EVERY GL entry point -- and built again just as quietly with only half supplied. An archive resolves no symbols, so it cannot fail that way; the `osmesa-prove` executable CL-7a-1 added to answer "what proves what" is the only thing that said so. It found that **glapi is a PAIR at 26.1.6 and neither half is in libmesa** (this target's first cut assumed otherwise): `libglapi` (shared-glapi/core.c) has the `_mesa_glapi_*` dispatch, `libglapi_bridge` (glapi/libgl_public.c) has the 1300 public `gl*` entry points whose only undefined symbol is its partner's `_mesa_glapi_tls_Dispatch`. On Linux the split is invisible because libGL.so links the bridge and resolves the dispatch dynamically; a static target must name both. (Aarch64 takes glapi's generic **C** entry path -- `_GLAPI_ENTRY_ARCH_TLS_H` is x86/x86-64/ppc64le only, so the TLS asm stubs and their `#error "Unsupported architecture"` are not in play.) **Result: `osmesa-prove` links** -- a 142 MB statically-linked aarch64 `ET_EXEC`, 1300 GL + 13 OSMesa entry points, 3282 ORC/JIT symbols; checked against what `kernel/elf.c` actually validates so CL-7b is not a surprise: no `PT_DYNAMIC` (rejected at elf.c:185), `ET_EXEC`/`EM_AARCH64`/ELF64/LSB all pass, `OS/ABI: UNIX - GNU` accepted at elf.c:77 (deliberately -- the comment names Clade binaries), segments R / R+E / RW with **no RWX**, and zero STRONG undefined symbols (`_DYNAMIC` local + two weak optional hooks). Delta = 3 patches / 14 files in `usr/ports/mesa/patches/`, **round-trip verified**: `git am` onto a pristine `mesa-26.1.6` worktree reproduces the fork tree hash exactly (`bb4a37cc`). One defect in my own tooling: `clade-mesa-cross.sh` had markdown backticks inside an UNQUOTED heredoc, so bash tried to execute `cc.get_define('ETIME')` on every emit -- stderr noise, rc=0 anyway, and a silently truncated comment in the generated cross file; fixed, and `tools/` swept for the same shape (clean). Kernel byte-unchanged. NEXT: CL-7b (the on-device run). |
| 2026-07-30 | **CL-7b-1 landed (section 16.22): the ORC dual-map mapper is WRITTEN, and llvmpipe RUNS on the device.** CL-7k closed naming the `DualMapMemoryMapper` as its "first consumer" and that mapper did not exist -- the fork's Thylacine delta was Triple/driver/Support only. It exists now (llvm-thylacine `ca850c6e`). `InProcessMemoryMapper` cannot work here: there is NO mprotect at all and pouch's mmap accepts PROT only to ignore it, so the upstream reserve-RW-then-raise-to-RX shape fails at its LAST step and fails LATE. ORC turns out to already draw I-42's line -- `MemoryMapper::prepare()` returns WORKING memory that need not be where code runs (why `SharedMemoryMapper` exists) -- so the mapper is SharedMemoryMapper's addressing over InProcessMemoryMapper's bookkeeping, four methods, and `initialize()` publishes with `SYS_ICACHE_SYNC` where upstream mprotects. **THE FINDING, invisible until a symbol was checked**: the first build SUCCEEDED (ninja rc=0, 0 FAILED, osmesa-prove linked) while the gallivm object referenced NEITHER memory manager. `USE_JITLINK` selects the object LINKING LAYER and is a DIFFERENT AXIS from `GALLIVM_USE_ORCJIT` (ORC vs MCJIT); reading the source and concluding "Mesa uses JITLink" conflated them -- the file contains both paths and a `#ifdef` picks. aarch64 is NOT in Mesa's USE_JITLINK list (RISCV/LoongArch/Win32), so ORC still ran on RTDyld, whose SectionMemoryManager mprotects, and `MemoryMapper` is a JITLink-ONLY seam -- the dual-map mapper was never consulted. **Third wrong-default-that-builds-clean after `llvm_has_mcjit` and `LLVM_ENABLE_RTTI`, and the quietest: nothing fails until runtime.** Caught only because the check asked "is the symbol in the object", not "did the build succeed" -- the archive-cannot-fail discipline one layer down. **MEASURED IN-GUEST** (boot OK, 1232/1232, 0 EXTINCTION, boot-ms 27495): `osmesa-prove: I-42 probe EACCES -- no CAP_JIT, llvmpipe cannot JIT` / `Dynamic loading not supported` / `rc=1 peak=76 pages`. Line 1 IS the CL-7b-1 result -- a 68 MB static aarch64 binary execs, musl comes up, and the kernel refuses SYS_JIT_CREATE exactly where it should (CAP_JIT is elevation-only, so a joey-spawned child cannot hold it; the refusal is the capability model WORKING). The prover calls the syscall directly before any of Mesa runs so this is legible -- via gallivm it would present as "OSMesaCreateContextExt returned NULL", which is also what a broken mapper and an unrelated gallivm fault look like. Line 2 is a SECOND blocker, independent and firing FIRST: musl's static-build dlopen stub verbatim (`src/ldso/dlopen.c:6`) -- something on the OSMesa init path dlopens and treats failure as fatal, exiting 1 before the prover's own OSMesa check. Tracked **#115** (suspect: LLVM `DynamicLibrary::DLOpen(NULL)` behind ORC's process-symbol generator -- NOT confirmed). Does not overturn section 16.19: that rejected EGL-surfaceless over `loader.c` dlopening the gallium DRIVER; this is a different dlopen, so the reasoning stands but was incomplete. Also measured: eager anon at exec is **~1.36 MB** (536 KB data + 821 KB bss) -- the other 67 MB is text+rodata that REVENANT demand-pages and does not charge per page at v1.0, so 142 MB was never the page-budget number. Kernel byte-unchanged. joey's `gl_probe()` REPORTS rather than gates (the prover cannot pass without CAP_JIT; gating on a known-impossible result would be theatre) and is proven inert -- a non-clade boot emits ZERO CL-7b lines. NEXT: CL-7b-2 (the corvus clearance + #115). |
