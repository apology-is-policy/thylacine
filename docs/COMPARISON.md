# Thylacine vs the Field

**Status**: Phase 0 draft, 2026-05-04. Honest architectural and feature positioning of Thylacine OS against the systems it intends to peer with, lead, or deliberately not compete against.

## 1. Purpose

This document answers: **where does Thylacine match existing operating systems, where does it lead, and where does it deliberately not compete?**

A feature matrix is easy to cheat — anyone can write `✓` next to any feature if the definition is loose. This document tries to be precise about feature *quality* and *architectural shape*, not just presence. A `✓` in Linux's "container" column means the production-tested namespaces + cgroups stack with cgroup-v2, AppArmor, seccomp; a `✓` in Plan 9's "container" column means per-process namespaces composed via `bind`/`mount` with no separate machinery. Both get `✓` in lazy matrices; the architectural reality is that Plan 9's model is *primary* to its design and Linux's is bolted onto a Unix base.

## 2. Comparison subjects

Eight comparison subjects, grouped by relationship.

**Direct architectural ancestors** (Thylacine inherits substantially):
- **Plan 9 from Bell Labs** (Bell Labs, 1990s; last release Fourth Edition 2002). The single closest ancestor. Source of the namespace model, 9P, `Dev` vtable, `rfork`, notes, factotum, `/proc` synthetic FS.
- **9Front** (community, 2011-present). The most vital continuation of Plan 9. Better hardware support, USB, SMP, various refinements. Reference implementation for many decisions.

**Production peers** (Thylacine targets parity-or-lead):
- **Linux** (Linus Torvalds + community, 1991-present). The compatibility target; the system most users come from. Architectural shape is "Unix evolved over 35 years." Massive ecosystem, massive codebase.

**Capability-based / microkernel peers** (Thylacine borrows architectural elements):
- **Fuchsia / Zircon** (Google, 2016-present). Capability-based microkernel; typed handles; VMOs as first-class kernel objects. The single most direct source for Thylacine's handle and VMO designs.
- **seL4** (NICTA / DSTG / proofcraft, 2009-present). Formally verified capability microkernel. Source of the verification methodology.

**Adjacent / experimental peers** (Thylacine learns lessons from):
- **Redox** (Jeremy Soller + community, 2015-present). Microkernel + Rust + scheme-based-IPC (similar in spirit to 9P). Closest to Thylacine in development model and scope ambition.
- **MINIX 3** (Andrew Tanenbaum, 2005-present). Userspace drivers; reincarnation server. Source of cautionary lessons on IPC overhead in microkernels.

**Research influences** (Thylacine takes ideas, not code):
- **Helios** (CMU, 2009). Per-core kernel + RPC channels; satellite kernels. Multikernel direction, deferred to Thylacine v2.x.
- **Barrelfish** (ETH Zurich + Microsoft Research, 2009-2013). Multikernel architecture; per-core state; explicit cross-core message passing. Multikernel reference.

ext4, XFS, FAT, NFS are *filesystems*, not OSes; they're not in scope here. ChromeOS, Android, Windows, macOS are deeply non-comparable in design philosophy and are also out of scope.

---

## 3. Feature matrix

Cells use:
- **`✓`** mature, production-tested
- **`~`** supported with caveats (see notes below the table)
- **`○`** experimental / limited / opt-in
- **`✗`** not supported / not present
- **`★`** Thylacine v1.0 target lead position
- **`◇`** planned for post-v1.0

| Feature | Plan 9 | 9Front | Linux | Fuchsia | seL4 | Redox | MINIX 3 | **Thylacine v1.0** |
|---|---|---|---|---|---|---|---|---|
| **Namespace model** |
| Per-process namespace, primary | ✓ | ✓ | ○ (`unshare`) | ✗ | ✗ | ○ | ✗ | ✓ |
| `bind` / `mount` as composition | ✓ | ✓ | ~ (mount only) | ✗ | ✗ | ○ | ✗ | ✓ |
| Container = namespace, no separate runtime | ✓ | ✓ | ✗ (cgroups+ns) | ✗ | ✗ | ○ | ✗ | ✓ |
| Union mounts native | ✓ | ✓ | ○ (overlayfs) | ✗ | ✗ | ✗ | ✗ | ✓ |
| **Protocol / IPC** |
| 9P-native protocol | ✓ | ✓ | ○ (virtfs) | ✗ | ✗ | ~ (scheme model) | ✗ | ★ totalized |
| One IPC mechanism for everything | ✓ | ✓ | ✗ | ✗ | ✗ | ○ | ✗ | ✓ |
| Pipelined / async protocol | ~ | ~ | ✓ (io_uring) | ✓ | ✗ | ○ | ✗ | ★ |
| Out-of-order completion | ✗ | ✗ | ✓ (io_uring) | ✓ | ✗ | ○ | ✗ | ★ |
| **Driver model** |
| Userspace drivers, primary | ✗ | ✗ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Typed handles for hardware access | ✗ | ✗ | ✗ | ✓ | ✓ (caps) | ✓ | ~ | ✓ |
| Driver crash isolated from kernel | ✗ | ✗ | ✗ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Driver update without reboot | ✗ | ✗ | ~ (modules) | ✓ | ✓ | ✓ | ✓ | ✓ |
| Drivers expose 9P interface | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ★ |
| **Memory model** |
| First-class VMO kernel objects | ✗ | ✗ | ✗ | ✓ | ~ (untyped) | ✗ | ✗ | ✓ |
| Zero-copy buffer sharing | ✗ | ✗ | ○ (`splice`) | ✓ | ✓ | ○ | ✗ | ✓ |
| KASLR | ✗ | ✗ | ✓ | ✓ | ○ | ✗ | ✗ | ✓ |
| Userspace ASLR | ✗ | ✗ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ |
| W^X enforced as invariant | ✗ | ✗ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ |
| **CPU feature use** |
| ARM PAC (return-address signing) | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ |
| ARM MTE (memory tagging) | ✗ | ✗ | ○ | ○ | ✗ | ✗ | ✗ | ★ |
| ARM BTI (branch target ident) | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ |
| LSE atomics on ARM64 | ✗ | ✗ | ✓ | ✓ | ✓ | ✗ | ✗ | ✓ |
| **Compiler-level hardening** |
| Compiler CFI | ✗ | ✗ | ✓ (CFG) | ✓ | ✗ | ✗ | ✗ | ✓ |
| Stack canaries | ✗ | ✗ | ✓ | ✓ | ✓ | ✓ | ✗ | ✓ |
| **Concurrency** |
| Per-CPU kernel data | ✗ | ~ | ✓ | ✓ | ~ | ✓ | ✗ | ✓ |
| Lock-free read paths | ✗ | ~ | ~ (RCU) | ✓ | ✗ | ○ | ✗ | ✓ |
| Multikernel direction | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ◇ (designed v1, impl v2) |
| **Scheduler** |
| Modern fair-share (CFS / EEVDF) | ✗ | ✗ | ✓ (EEVDF) | ✓ | ~ | ○ | ✗ | ✓ (EEVDF) |
| Real-time class | ○ | ○ | ✓ (RT-PREEMPT) | ○ | ✓ | ✗ | ✗ | ✗ |
| **Process model** |
| `rfork` with explicit resource flags | ✓ | ✓ | ○ (`clone`) | ✗ | ✗ | ✗ | ✗ | ✓ |
| Threads as same primitive as procs | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| Plan 9 notes | ✓ | ✓ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ (POSIX signals via translation) |
| **Display / UI** |
| Compositor / window manager | ✗ (Rio/8½) | ✗ (Rio) | ✓ (Wayland/X) | ✓ | ✗ | ○ | ✗ | ✗ |
| Inline graphics in shell | ✗ | ✗ | ○ (term emul) | ✗ | ✗ | ✗ | ✗ | ★ (Halcyon) |
| Video player as 9P server | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ★ |
| **Storage / FS** |
| COW filesystem native | ✗ | ✗ | ✓ (btrfs/bcachefs) | ✗ (Minfs basic) | ✗ | ✓ (RedoxFS) | ✗ | ✓ (Stratum) |
| Encryption native | ✗ | ✗ | ✓ (fscrypt) | ✗ | ✗ | ✓ | ✗ | ✓ (PQ-hybrid via Stratum) |
| Tamper-evident metadata (Merkle) | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ✓ (via Stratum) |
| **Boot / discovery** |
| DTB-driven hardware discovery | ✗ | ~ | ✓ | ✓ | ✓ | ○ | ✗ | ✓ |
| Bootloader-agnostic kernel | ~ | ~ | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| **Compat surface** |
| POSIX surfaces as 9P servers | ✗ (some FSes) | ✗ | ✗ | ✗ | ✗ | ✗ | ✗ | ★ |
| Linux binary compat (static) | ✗ | ✗ | ✓ (native) | ○ | ✗ | ○ | ✗ | ✓ |
| OCI container compat | ✗ | ✗ | ✓ (native) | ○ | ✗ | ✗ | ✗ | ✓ |
| **Verification posture** |
| Formal kernel verification | ✗ | ✗ | ✗ | ○ | ✓ | ✗ | ✗ | ○ (TLA+ for invariants) |
| Adversarial audit cadence | ✗ | ✗ | ~ | ~ | ✓ | ✗ | ✗ | ✓ |
| Spec-first for invariant changes | ✗ | ✗ | ✗ | ○ | ✓ | ✗ | ✗ | ★ |
| **Out-of-band** |
| In-kernel filesystem driver | ✗ | ✗ | ✓ | ✗ | ✗ | ✓ | ✗ | ◇ (v2.0 hook) |
| Sound at v1.0 | ✓ | ✓ | ✓ | ✓ | ✗ | ○ | ✗ | ✗ (deferred) |
| Bluetooth / USB beyond HID | ~ | ✓ | ✓ | ✓ | ✗ | ○ | ✗ | ✗ (deferred) |
| Multiplatform (x86 + ARM) | ~ | ✓ | ✓ | ✓ | ✓ | ~ | ~ | ✗ (ARM only at v1.0) |

**Caveats (column notes)**:

- **Plan 9 / 9Front**: namespace + 9P + Dev + factotum are mature and production-tested for the workflows they cover. Hardware support is narrower than Linux. SMP works in 9Front but is not as polished. No native COW FS, no native encryption.
- **Linux**: massive feature set; per-process namespace is bolt-on (`unshare`, `clone(CLONE_NEWNS)`). Containers require cgroups + namespaces + AppArmor/SELinux + seccomp + a runtime. Drivers are kernel-mode primarily; userspace drivers via FUSE / DPDK / VFIO exist but are exceptions.
- **Fuchsia**: typed handles + VMOs are core; 9P is absent. Capabilities are the IPC primary. Display via Scenic + Flutter.
- **seL4**: formally verified, but ergonomics are hostile to application development. Capabilities are the primary primitive. Userland is bring-your-own.
- **Redox**: scheme-based IPC similar to 9P in spirit but not protocol-compatible. Rust kernel and userspace. Limited hardware coverage; not yet daily-driver-capable.
- **MINIX 3**: userspace drivers and reincarnation server are mature; IPC overhead is the well-known cost (~8% vs L4-class).

**Rough totals**:

- Plan 9: 16 ✓, 9 ~/○, 19 ✗ — pioneered the architecture but narrow hardware.
- 9Front: 17 ✓, 11 ~/○, 16 ✗ — most advanced descendant; still narrow.
- Linux: 31 ✓, 7 ~/○, 6 ✗ — feature-complete, but on Unix base.
- Fuchsia: 24 ✓, 5 ~/○, 15 ✗ — modern microkernel but no 9P.
- seL4: 14 ✓, 4 ~/○, 26 ✗ — verified but minimal.
- Redox: 14 ✓, 11 ~/○, 19 ✗ — modern, limited maturity.
- MINIX 3: 10 ✓, 3 ~/○, 31 ✗ — narrow scope.
- **Thylacine v1.0: 31 ✓ (peer parity), 6 ★ (lead positions), 4 ◇ (post-v1.0 designed-but-not-implemented), rest are deferred non-goals**.

The 6 ★s are where Thylacine has no peer at v1.0 release.

---

## 4. Category analysis

Numbers in the matrix don't tell the whole story. This section narrates the *why* behind each category.

### 4.1 Composition: namespaces and 9P

**Where Plan 9 / 9Front lead** (and Thylacine inherits): per-process namespaces composed via `bind` / `mount`. Containers *are* namespaces, not namespaces-plus-cgroups-plus-AppArmor. One mechanism, not three.

**Where Linux compromised**: namespaces were added piecemeal (`CLONE_NEWNS`, `CLONE_NEWNET`, `CLONE_NEWPID`, ... ) on top of a Unix base where the global namespace was the assumption. They work, but they're optional, opt-in, and a process accidentally not unsharing a namespace silently joins the global one. Security-critical isolation depends on container runtimes correctly orchestrating multiple primitives. The complexity is real and contributes to container escape vulnerabilities (CVE history shows ~10/year in this surface).

**Where Fuchsia / seL4 compromised**: capabilities replace namespaces. This is a fine choice for a microkernel, but it means there's no "compose two filesystems into a directory tree" primitive — you either get a capability or you don't. Hierarchical composition is reinvented at the application layer.

**Where Thylacine leads**:
- **9P-as-universal-composition, totalized**. Plan 9 had 9P everywhere except authentication, graphics, and some device interfaces — those escaped. Thylacine totalizes the model: Halcyon mounts 9P servers (no special graphics protocol); janus is a 9P server (auth via 9P); drivers are 9P servers (no driver protocol). The thesis is: if it can be a file, it is a file; if it can be a 9P server, it is a 9P server.
- **POSIX surfaces as 9P servers, native**. `/proc`, `/dev`, `/sys`, `/dev/pts`, `/run`, `/tmp` are all 9P servers. Linux programs see what they expect; underneath it's all 9P. Adding a new POSIX surface is adding a new 9P server, not modifying the kernel.
- **Per-process namespace = per-connection 9P namespace**. Stratum's per-connection namespace (its NOVEL angle #8) is the natural granularity; Thylacine's per-process model maps to it 1:1.

**Where Thylacine deliberately doesn't go**: capability-based replacement of namespaces. seL4's model is more granular but lacks the hierarchical composition that makes shell-driven workflows work. Thylacine is a Plan 9 revival, not an seL4 application platform.

### 4.2 Driver model

**Where Linux is the reference**: mature in-kernel drivers. Massive ecosystem (~25 million LOC of drivers). Performance excellent. But: a driver bug crashes the kernel, drivers run with full kernel privilege, updates require module reload (or full reboot for most drivers), the surface area is the security weakest link.

**Where Fuchsia / seL4 / MINIX 3 lead**: userspace drivers from day one. Capability-mediated hardware access. Crashes isolated. Updates trivial. The cost is IPC overhead for hot-path operations.

**Where Thylacine leads**:
- **Userspace drivers via typed handles + VMO zero-copy**. Drivers hold `KObj_MMIO` / `KObj_IRQ` / `KObj_DMA` handles for hardware access (hot path bypasses the kernel) and expose results via 9P with VMO-handle attachments for bulk data (zero-copy). The 9P interface is the public contract; the handles are the private mechanism.
- **Subordination invariant**: handles transfer between processes only via 9P sessions. Hardware handles are non-transferable by type — the transfer syscall has no code path for them. This is stronger than Fuchsia's runtime check; at the syscall site, the invariant is unfalsifiable.
- **Driver update without reboot is not just supported — it's the default**. Unmount the 9P server, restart the driver process, remount. Linux module reload is comparable for narrow cases; for most drivers (network, GPU) it is not actually possible.

**Where Thylacine pays the cost**: IPC overhead for non-hot-path operations is real. The 9P round-trip for "give me one byte" is more expensive than a kernel function call. We accept this trade because (a) hot paths use direct hardware access via handles, not 9P, and (b) the 9P round-trip latency budget (sub-500µs) is enforced as a regression-test target.

**Where Thylacine deliberately doesn't go**: in-kernel drivers as a fallback. Phase 3 lands userspace virtio-blk + virtio-net + virtio-input from day one. There is no in-kernel-because-faster shortcut. The priming had this as Phase 3 expedience; we reject the shortcut.

### 4.3 Concurrency and SMP

**Where Linux is the bar**: highly tuned per-CPU data, RCU, fine-grained locking, scales to hundreds of cores. The cost: kernel-level concurrency complexity is one of the hardest places to reason about correctness. Linux has had memory-ordering bugs that took 5+ years to find.

**Where seL4 / Redox compromise**: simpler concurrency models (often big-kernel-lock-equivalent or per-CPU kernel instance with no shared state) trade scalability for correctness. seL4's verification is harder under fine-grained concurrency.

**Where Thylacine targets**:
- **EEVDF scheduler from v1.0**: Linux 6.6+'s default since 2023. Provable latency bounds, bounded service time, fair under all weights. Skips the CFS step entirely.
- **Per-CPU run queues + work-stealing on idle**: standard modern pattern.
- **Per-CPU kernel data discipline from v1.0**: scheduler queues, allocator slabs, IRQ counters all per-CPU. Cross-CPU communication is explicit (IPI), not implicit (lock).
- **Spec-first for the IPI / wakeup state machine**: `specs/scheduler.tla` proves that IPI-driven wakeups don't lose events; that work-stealing doesn't violate fairness.
- **Multikernel direction designed for v2.x**: per-core kernel instances communicating via 9P (the natural extension of Thylacine's model). The design contract lands in `ARCHITECTURE.md §20.6` at Phase 0.

**Where Thylacine deliberately doesn't go**: real-time guarantees at v1.0. EEVDF gives latency bounds for fair-share work; hard real-time would need a separate scheduling class. Out at v1.0; v2.x candidate.

### 4.4 Memory protection and CPU-feature hardening

**Where Linux is the reference**: KASLR, ASLR, W^X (page protection), stack canaries, Compiler CFI (recently), PAC on ARM64, BTI on ARM64, MTE on ARM64 (in progress). Production-tested across millions of devices.

**Where Fuchsia matches Linux**: same hardening features. Both adopted them as table-stakes.

**Where Plan 9 / 9Front / MINIX 3 fall short**: minimal hardening. Plan 9 predates much of this; 9Front hasn't prioritized it; MINIX 3 is research-focused.

**Where Thylacine matches the bar**: KASLR, ASLR, W^X, stack canaries, CFI, PAC, BTI, LSE atomics — all enabled by default at v1.0. No "hardening as opt-in"; the default build is the hardened build.

**Where Thylacine leads (modestly)**:
- **MTE enabled by default where hardware supports** (Apple Silicon does; QEMU emulates). Linux has MTE in opt-in mode for some kernels; Thylacine enables it by default. Catches use-after-free / heap overflow at hardware speed.
- **W^X as an enumerated invariant** (I-12 in VISION §8), not just a default-on policy. The kernel has *no* code path that can flip a page from W to X. JIT compilers, when they appear post-v1.0, get an explicit syscall for this.

### 4.5 Display and UI

**Where Linux leads**: Wayland and X11 are mature. GUI applications work. Browsers, IDEs, Office suites — the entire desktop ecosystem is on Linux.

**Where Fuchsia targets**: Scenic + Flutter. Modern compositor, declarative UI framework. Production in Google Nest Hub.

**Where Plan 9 has its identity**: Rio (window manager) + 8½ (terminal). Tile-based, mouse-heavy, not aimed at Office-class applications. Beautiful for Plan 9 workflows; not a desktop replacement.

**Where Thylacine differs from all of them**:
- **No compositor, no scene graph, no display server**. Halcyon is the display surface.
- **Inline graphics in scroll buffer** is the model: text and pixel-addressable regions in one stream. Images render inline; videos play inline; the scroll buffer is the history.
- **Video player as 9P server** (`/dev/video/player/`) — Halcyon mounts it, polls `frame`, blits. The shell does not know about codecs.

This is **not a display-server-shaped UI**. It is a deliberately different shape that handles a smaller set of workflows. The trade is honest: no web browser, no multi-pane IDE, no Office-suite-class apps. The OS is for shell-driven workflows.

This is the most exposed design decision in Thylacine. The risk is that "shell-driven workflows" is too narrow a target. The mitigation is that it's exactly the target the designers want — and a generation of developers who do everything in `tmux` + `vim` + a browser tab for documentation is the user base. Halcyon makes the browser tab a non-starter, replacing it with man pages and inline-rendered Markdown. That trade is acceptable to the project.

### 4.6 Storage

**Where Linux leads**: ext4 (workhorse), btrfs (mature feature-rich), bcachefs (modern), XFS (high-performance), ZFS (best-in-class but license-restricted on Linux).

**Where Plan 9 / 9Front have**: cwfs, fossil, kfs. None of them are competitive with modern Linux filesystems. Plan 9 was always FS-light.

**Where Fuchsia / seL4 / MINIX 3 have**: minimal filesystems (Minfs, etc.). Storage was never the focus.

**Where Thylacine leads** (via Stratum):
- **Stratum as native filesystem**: COW, formally verified sync, PQ-hybrid AEAD-SIV encryption, Merkle-rooted metadata integrity, content-defined chunking, lock-free metadata path with MVCC readers, succinct in-RAM state (~1 MiB allocator RAM per TiB), Reed-Solomon + Locally Repairable Codes for redundancy, io_uring-native I/O, per-connection 9P namespaces, synthetic `/ctl/` administration, factotum-style key agent.
- **The OS and the FS share a protocol**. Stratum is a 9P server; Thylacine mounts it like any other 9P server. No translation layer. Stratum's per-connection namespace maps 1:1 to Thylacine's per-process namespace.

This is the strongest point of leverage in the project. Thylacine on Stratum is a fundamentally different storage story than any other OS: tamper-evident, post-quantum-encrypted, formally verified, with the OS integration being literally just "mount the 9P server."

**Where Thylacine deliberately doesn't go at v1.0**: in-kernel Stratum driver. The design contract lands in ARCH §14.4 — bypass the 9P client overhead for root FS operations — but the implementation is post-v1.0. The 9P-client path is fast enough for v1.0 development workflows.

### 4.7 Compatibility

**Where Linux is dominant**: native target for ~all modern software. Massive ecosystem.

**Where Fuchsia / seL4 / Redox / MINIX 3 fall short**: limited POSIX compat. Bring-your-own-runtime.

**Where Plan 9 / 9Front fall short**: minimal POSIX (APE — ANSI/POSIX Environment — works for some programs, not most). No Linux binary compat.

**Where Thylacine targets** (three tiers, per VISION §12):
1. **Tier 1 — native**: programs compiled against the musl port. Build clean, link clean, run with full functionality. The full Linux developer surface — `rc` + `bash` + uutils-coreutils (complete flag coverage), `vim`, `less`, `top`, `htop`, `tmux`, `ssh`, `git`, `make`, `mk`, `gcc`/`clang`, `python3`, `curl`. BusyBox in initramfs as recovery. This tier delivers **Utopia** (VISION §13) at Phase 5 exit — the textual POSIX environment that proves the kernel + 9P + Stratum + compat layer compose into a usable system before Halcyon arrives.
2. **Tier 2 — static Linux ARM64 binaries**: pre-built static ARM64 ELF binaries via the Linux syscall translation shim. Best-effort; covers most CLI tools.
3. **Tier 3 — OCI container as namespace**: Linux container images run inside a Thylacine namespace. The kernel primitive (namespace) handles container isolation; synthetic Linux-shaped 9P servers (`/proc`, `/sys`, `/dev` Linux layout) provide the expected files.

**Out at v1.0**: glibc-dynamic binaries, programs requiring `epoll` / `inotify` / `io_uring` (post-v1.0 surface), Windows binaries.

This puts Thylacine at "useful Linux compat" territory — narrower than Linux's native, broader than Plan 9's APE. The compat is *additive*: removing it leaves a working OS.

### 4.8 Verification posture

**Where seL4 leads decisively**: full functional correctness proof in Isabelle/HOL. The reference for what kernel verification can be.

**Where Linux / Fuchsia have**: extensive testing, sanitizers, fuzzers, but no formal proofs. CFI catches a class of CFG attacks at runtime.

**Where Thylacine targets**: formal specs for load-bearing invariants, not full functional correctness. Nine TLA+ specs (`scheduler`, `namespace`, `handles`, `vmo`, `9p_client`, `poll`, `futex`, `notes`, `pty`) gate-tied to phases. Adversarial audit cadence (modeled on Stratum's 15-round audit history). Sanitizer matrices (ASan, UBSan, TSan) on every commit. Property-based tests + fuzzers as standard. The bar is *practical formal verification* — every load-bearing invariant has a spec; the spec is the source of truth; the implementation references the spec; CI runs TLC on every PR touching specified subsystems.

This is the same bar Stratum holds itself to. It's higher than Linux and Fuchsia; lower than seL4's full-correctness proof. The trade: seL4-class verification for a Plan 9-shaped OS would take a decade and require ~10x the team. Practical TLA+ verification is achievable in the project's actual scope.

### 4.9 Implementation size and auditability

Targets for Thylacine v1.0 (from VISION §3.6 + ARCHITECTURE plans):

- **Kernel** (C99): ~30-40 KLOC at v1.0. Comparable to ext4 (30 KLOC). Smaller than XFS (100 KLOC), much smaller than Linux (~25 MLOC) or Fuchsia (~5 MLOC).
- **Userspace drivers** (Rust): ~10-15 KLOC across virtio-blk, virtio-net, virtio-gpu, virtio-input, network stack.
- **Halcyon** (Rust): ~8-12 KLOC.
- **Compat layer** (musl port + syscall shim, C99): ~5-8 KLOC of new code on top of musl's existing ~30 KLOC.
- **TLA+ specs**: ~3-5 KLOC across nine specs.
- **Total**: ~60-80 KLOC of new code at v1.0.

For comparison:
- seL4 kernel: ~10 KLOC C + ~200 KLOC Isabelle proofs.
- MINIX 3 kernel: ~5 KLOC; full system ~600 KLOC.
- Redox kernel: ~30 KLOC Rust; full system ~200 KLOC.
- Plan 9 / 9Front kernel: ~50-80 KLOC C.
- Fuchsia / Zircon: ~150 KLOC C++ kernel; ~5 MLOC system.

Thylacine is in Plan 9 / 9Front territory for kernel size, with stronger verification than them and weaker than seL4. The interface layer is simpler (one mechanism: 9P) than any peer; the implementation is no smaller than it must be.

### 4.10 Performance posture

Honest expectations at v1.0:

- **Boot time**: < 500ms to UART banner, < 3s to login prompt (Halcyon up). Faster than Linux (typical ~5-15s on QEMU); comparable to Plan 9 / 9Front (sub-second to login on QEMU); slower than seL4 (~100ms; bare metal optimized).
- **Syscall p99**: < 1µs no-contention. Microkernel-class — comparable to seL4 on ARM64; faster than Linux's typical 1-5µs; slower than uniprocessor Plan 9 on idle hardware.
- **Process creation p99**: < 1ms. Comparable to Linux; faster than MINIX 3 (~5-10ms historically).
- **9P round-trip p99 (loopback)**: < 500µs. Faster than Plan 9's typical 1-2ms (Plan 9's network stack overhead); slower than Linux pipe latency (~100ns) — but pipes are not the comparison here.
- **IRQ to userspace handler p99**: < 5µs. Modern microkernel territory — Fuchsia ~1-3µs; seL4 ~1µs. Linux kernel IRQ handler ~500ns; userspace IRQ via uio ~10-50µs.

Thylacine pays a measurable performance cost vs Linux for the userspace-driver model. We accept this. Target workloads (development inside a VM) are not throughput-bound; they are latency-bound for interactive ops, and the latency budget delivers.

We do *not* lead on raw throughput. We trade throughput for the architectural integrity of the userspace-driver model and the formal correctness of the kernel.

---

## 5. Positioning summary

### 5.1 Where Thylacine matches the bar

- **Plan 9 / 9Front on namespaces, 9P, `rfork`, `Dev`, notes, factotum**. Architecturally: Plan 9 with 25 years of hindsight applied.
- **Fuchsia on typed handles, VMOs, userspace drivers**. Subordinated to 9P (the public interface) as the private mechanism.
- **Linux on KASLR, ASLR, W^X, CFI, stack canaries, compiler hardening**. Standard modern security baseline.
- **seL4 on the spec-first methodology** for invariant-bearing changes. Practical TLA+ rather than full Isabelle proofs.
- **Stratum on storage**. Native filesystem with PQ-hybrid AEAD-SIV encryption, Merkle-rooted metadata, lock-free metadata path, formal verification.

### 5.2 Where Thylacine leads

- **9P-as-universal-composition, totalized**. Plan 9 had 9P everywhere except graphics, auth, some devices. Thylacine totalizes the model.
- **Halcyon: shell as the graphical environment**. Inline graphics in scroll buffer, video as 9P server, no compositor, no windowing system. Unique design point.
- **Userspace drivers via 9P + handles, totalized**. Hardware handles are non-transferable by type; transfer-via-9P is enforced at the syscall site as an invariant.
- **POSIX surfaces as 9P servers, native**. `/proc`, `/dev`, `/sys`, `/dev/pts`, `/run`, `/tmp` are all 9P servers. No special compat machinery.
- **Per-process namespace = per-connection Stratum namespace**. Cleanest possible OS-FS coupling.
- **MTE enabled by default where hardware supports**. Catches UAF / heap overflow at hardware speed.
- **W^X as enumerated invariant** (not just default-on policy).
- **Spec-first for invariant-bearing changes**, with nine TLA+ specs gate-tied to phases.
- **Designed-not-implemented v2.0 contracts**: capability elevation, multikernel SMP, in-kernel Stratum driver. Architectural shape committed at Phase 0; implementations land later.

### 5.3 Where Thylacine deliberately doesn't compete

- **Wayland / X11 / Scenic compositor.** No graphical windowing system. Halcyon is the UI surface. If you need overlapping windows, Thylacine is not your OS.
- **Web browser support.** Browsers assume a windowing system; Halcyon doesn't have one.
- **Multi-pane IDEs, Office-suite-class applications.** Out by design.
- **x86-64 at v1.0.** ARM64 only. x86 is v2.x.
- **Distributed / clustered OS.** Single machine. 9P can be used across a network; Thylacine itself doesn't manage a cluster.
- **Real-time scheduling at v1.0.** EEVDF gives soft latency bounds; hard RT is v2.x.
- **Sound at v1.0, Bluetooth, USB beyond HID, hardware sensors.** Deferred.
- **In-kernel filesystem driver at v1.0.** 9P-client mount is the path; in-kernel driver is a v2.0 hook.
- **setuid binaries.** No capability-elevation mechanism at v1.0; designed for v2.0 via factotum.
- **glibc-dynamic Linux binary compat.** Best-effort; musl is the target.

### 5.4 Where Thylacine pays an honest cost

- **Performance vs Linux.** Userspace-driver IPC overhead is measurable. Mitigated by direct hardware access via handles (hot path bypasses kernel) and zero-copy via VMOs (bulk data path). But the 9P round-trip for control operations is more expensive than a kernel function call.
- **Hardware coverage at v1.0.** ARM64 + QEMU `virt` only; bare-metal Pi 5 post-v1.0; bare-metal Apple Silicon v2.0+. Linux runs on everything; Thylacine runs on a curated, growing list.
- **Ecosystem at v1.0.** Plan 9 / 9Front have 30 years of accumulated software (rc scripts, Plan 9 ports of games, etc.) but most of it doesn't translate. Linux has the largest ecosystem ever assembled; Thylacine starts with whatever musl + BusyBox + a curated list of programs we get running.
- **Maturity.** Linux has 35 years of production. Plan 9 has 30 years of niche use. Thylacine v1.0 will have the audit and verification rigor of a much older system, but actual time-in-production is short. No substitute for years of users finding bugs.

### 5.5 Honest weaknesses at v1.0

- **No bare-metal hardware**. v1.0 ships in QEMU. Real-hardware is post-v1.0 (Pi 5 first; Apple Silicon later).
- **No GPU 3D acceleration**. Halcyon uses VirtIO-GPU virgl for 2D framebuffer; 3D / OpenGL ES is post-v1.0.
- **No sound, no Bluetooth, no USB beyond keyboard/mouse**. Deferred to post-v1.0.
- **Limited Linux binary coverage**. Static binaries work; glibc-dynamic + io_uring + epoll-dependent programs don't at v1.0.
- **Halcyon is a deliberately narrow UI**. Browsers, multi-pane IDEs, and Office apps don't run. By design — but it's a real cost for users.
- **In-kernel Stratum driver isn't there yet**. 9P-client mount is fast enough for development, but root FS operations have one extra round-trip vs an in-kernel driver. Post-v1.0 closes this.

---

## 6. Brief: Plan 9 → Thylacine, the architectural delta

| Subsystem | Plan 9 | Thylacine |
|---|---|---|
| Namespace | Per-process, `bind`/`mount` | Same, with hardened isolation invariants spec'd |
| Process | `rfork` | Same, plus `pthread`/`futex`/`poll` translation in musl |
| 9P dialect | 9P2000 (vanilla) | 9P2000.L + Stratum extensions (Tbind, Tunbind, Tpin, Tunpin, Tsync, Treflink, Tfallocate) |
| Pipelining | Optional in protocol; usually not exploited | Mandatory; up to 32 concurrent ops/session |
| Drivers | Mostly in-kernel | Userspace from Phase 3, no exceptions |
| Driver hardware access | Privileged kernel context | Typed handles (`KObj_MMIO`/`IRQ`/`DMA`); subordinated to 9P |
| Memory sharing | Anonymous shm (convention-based) | First-class VMO kernel objects (Zircon-derived, transferable via 9P) |
| Notes / signals | Notes | Notes internally; POSIX signals via translation |
| Authentication | factotum + p9any/p9sk1 | janus + Stratum's PQ-hybrid wrap-key |
| Filesystem | cwfs / fossil / kfs | Stratum (PQ-encrypted, Merkle-integrity, formally verified) |
| Graphics | Rio + 8½ | Halcyon (no windowing system) |
| Network | Plan 9 IP stack | Modern stack (smoltcp Rust port or Plan 9 IP port to Rust) |
| Hardware ABI | Plan 9 device drivers | DTB-driven, ARM64 first |
| Verification | Manual review | Spec-first for invariants + adversarial audit + sanitizer matrix |
| Hardening | Minimal | KASLR, ASLR, W^X, CFI, PAC, MTE, BTI, LSE atomics |

The pattern: every Plan 9 idea preserved; every Plan 9 limitation revisited with 25 years of tooling and verification methodology. The OS is recognizably Plan 9 in shape and recognizably 2026+ in implementation rigor.

---

## 7. The positioning claim

Thylacine OS is **the operating system Plan 9 would have become if the industry had not walked away** — built from first principles in 2026, with the namespace + 9P architecture taken to its conclusion, the typed-handle / VMO performance model adopted from Fuchsia (subordinated to 9P), the verification methodology adopted from seL4 + Stratum, the security hardening adopted from Linux + Fuchsia, the storage substrate provided by Stratum's already-feature-complete-and-verified filesystem.

It is not a research OS — it is meant to be the thing you actually use to develop software. But it uses the research, and it uses Plan 9's complete architectural lineage rather than picking pieces.

The consolidated value proposition: **Plan 9's thread, picked back up in 2026 and carried forward with the same rigor that produced Stratum, Fuchsia, and seL4, deliberately scoped for shell-driven development workflows on ARM64 hardware, and held to the standard that any complexity must be verified before it enters the binary.**

It matches Plan 9 / 9Front on architecture, leads them on verification + hardening + filesystem + graphics. It matches Fuchsia on typed handles + VMOs, leads it on namespace composition + 9P-as-universal-protocol. It matches seL4 on spec-first methodology, leads it on ergonomics for Plan 9-shape applications. It matches Linux on hardening defaults + container compat, deliberately doesn't compete on hardware breadth or windowing-system applications.

**Not a research OS. Not a toy. The OS Plan 9 would have become.**
