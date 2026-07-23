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
- **Mesa**: pinned at CL-7 entry (a current 25.x-era release); the CL-0
  spike confirms gallium-OSMesa + ORC-gallivm state in the pinned release.
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
   *(§16.6: upstream removed the OSMesa frontend post-design — the
   frontend piece is a CL-7 entry decision; the delivery shape stands.)*
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
| **CL-1** | The process substrate: `posix_spawn` rewrite + `wait4` + `pouch-env` + `pouch-dirent`; make + ninja ports | `make -j` runs a toy multi-TU C build on-device (with the host-cross clang first) | boundary-line audit (the #68/#926 process-lifecycle lineage — prosecute the spawn/reap paths) | — (shared with the git port) |
| **CL-2** | The C++ runtime: libunwind + libc++abi + libc++ static into the sysroot; prover suite | a C++ prover (EH + RTTI + threads + TLS-dtors + filesystem) green on-device | focused round on the runtime/boundary seams | — |
| **CL-3** | The triple: `Triple::Thylacine` + clang ToolChain + lld default in `llvm-thylacine`; wrappers retired | host cross-builds via the real triple, byte-compatible artifacts | none (host-side) | — |
| **CL-4** | Support-layer port + the device toolchain: mmap detours, Program/Path/Process/Signals/DynamicLibrary; static multicall cross-built + baked to `/clade` | **`clang++ -O2` compiles, links (lld), and runs a real C++ program on-device** | focused round (the Support patches + the bake) | — |
| **CL-5** | Build storms: on-device parallel builds of real projects (zlib → sqlite → an LLVM subset); the F4 budget mechanism lands; perf measured (the CHASE toolkit) | `make -jN` of a nontrivial project completes; numbers recorded, no committed target | the F4 kernel change gets its own round | ThinLTO, sanitizers-on-device: out |
| **CL-6** | clangd + Nora C/C++ | diagnostics/hover/def in Nora on a C++ file | none (userspace client) | lldb-dap → post-arc |
| **CL-7k** | The JIT capability (kernel): code Burrow + `SYS_ICACHE_SYNC` + `CAP_JIT`; I-42 | the `libthyla_rs::jit` prover (emit→sync→call; ungated Proc **denied**) | **prosecute hard** (W^X-adjacent; own focused round; F8 spec posture) | — |
| **CL-7** | Mesa/llvmpipe + SDL-GL + GLQuake | **GLQuake renders via llvmpipe through Tapestry**; gears smoke in CI | focused round (the ORC mapper + the GL glue's weave lifetime) | lavapipe → stretch |
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

## 17. Revision history

| Date | Change |
|---|---|
| 2026-07-23 | Initial draft: research pass (tree + external) + the full arc design; forks §14 open. |
| 2026-07-23 | SIGNED OFF — all §14 leans adopted verbatim; JIT invariant renumbered I-41 → I-42 (I-41 reserved by ADVANCED-GO AG-2 between draft and signoff); moved to the main tree for the scripture commit. |
| 2026-07-23 | **CL-0 landed** (§16): syscall-gap census closed (zero new kernel syscalls for CL-1..CL-4; `renameat`+`getdents64` per-compile load-bearing), environ CLOSED (envp always empty), lld-in-multicall VERIFIED, Mesa OSMesa-removal correction (§16.6), F4 validated by measurement (worst TU 2.46 GiB). Instruments: disposable GCP ARM VM (torn down) + the fork clone @ 22.1.8. |
