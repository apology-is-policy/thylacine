# Thylacine OS — Vision

**Status**: Phase 0 draft, 2026-05-04. This document is the root of all subsequent design decisions; every claim is contestable while it remains in draft, and binding once committed.
**Companion documents**: `COMPARISON.md`, `NOVEL.md`, `ARCHITECTURE.md`, `ROADMAP.md`.

---

## 1. Mission

Thylacine is a Plan 9-heritage operating system targeting ARM64, designed to be a real OS — not a toy, not a research prototype. It is built on three convictions, an unwavering commitment to state-of-the-art implementations from the first commit, and a refusal to ship anything primitive in the name of expedience.

The three convictions:

1. **Plan 9's ideas were correct.** The namespace model, the "everything is a file" philosophy taken seriously rather than superficially, and per-process namespaces as the primary isolation primitive — these are better ideas than what Unix evolved into. The industry walked away from them. Thylacine doesn't.
2. **The shell is sufficient as a UI.** A graphical windowing system is not a prerequisite for a capable, pleasant computing environment. A shell that renders images, plays video, and composes interfaces from synthetic filesystems can do everything a modern user needs, without the complexity tax of a compositor, a scene graph, or a display server.
3. **The filesystem is the OS.** Stratum — Thylacine's native filesystem — is not an afterthought. It is the storage substrate the OS is designed to run on: COW, formally verified, post-quantum encrypted, 9P-native. The OS and the filesystem share a design philosophy and a protocol.

The fourth, methodological conviction — the one that binds the project at every level — is that **complexity is permitted only where it is verified**. Maximum implementation rigor, formal specifications for every load-bearing invariant, adversarial audit before every invariant-bearing merge, and no shortcut implementations even when "we'll fix it later" would save weeks. Stubs and primitive implementations exist only when they make the overall roadmap cleaner — never when they merely defer work.

The name **Thylacine** is intentional. The thylacine (*Thylacinus cynocephalus*) was a marsupial apex predator declared extinct in 1936 — beautiful, functional, lost not because it failed but because the world stopped making room for it. Plan 9 is the thylacine of operating systems: declared dead, occasionally glimpsed alive in the infrastructure of the systems that replaced it (9P in WSL, in container runtimes, in distributed systems, in QEMU's virtfs). Thylacine OS asks: what if it had lived?

---

## 2. The naming family

Names in this project share an emotional register: words that carry longing for something that cannot be again, returned to as if it could.

| Component | Name | Meaning | Pronunciation |
|---|---|---|---|
| Operating system | **Thylacine** | The lost animal; the unlived future | *thigh-luh-syne* |
| Filesystem | **Stratum** | A record of a different time, preserved in layers | *strah-tum* |
| Shell | **Halcyon** | The calm before; the impossible return | *hal-see-on* |
| Key agent | **janus** | Two-faced; the boundary between worlds (inherited from Stratum) | *jay-nus* |

Naming is load-bearing. A project that can articulate what it is and why in three words has a soul; a project that can't, drifts.

---

## 3. Design philosophy

### 3.1 9P is the universal composition mechanism

This is the core thesis. **9P is not a filesystem protocol. It is the unifying compositional primitive of Thylacine.** Every kernel subsystem that exposes state or accepts commands does so as a synthetic filesystem served over 9P. Device drivers, process state, network interfaces, the video player, the key agent, administrative controls — all are 9P trees that can be mounted, browsed, scripted, and composed.

This is not aesthetic. It means:

- **One permission model** for all resources (file mode bits + namespace position).
- **One IPC mechanism** (9P read/write/walk/clunk on a Chan).
- **One namespace operation** (`bind`, `mount`) for composition.
- **Drivers are userspace processes** with no special kernel privilege beyond typed hardware handles.
- **The entire system is observable and controllable from a shell** — there is no separate admin API, no separate driver-loading interface, no separate IPC framework.

Every architectural decision in this project is tested against the thesis. If a feature requires a separate composition mechanism alongside 9P, the design has failed and is reworked.

### 3.2 Namespaces are the isolation primitive

Each process has its own namespace — its own view of the filesystem tree. Containers, sandboxes, and compatibility environments are not special kernel features: they are processes with carefully constructed namespaces. This is what Plan 9 had from the start. Thylacine inherits it unchanged.

A "container" in Thylacine is a process with:
- A root namespace constructed from a chosen filesystem root (Stratum subvolume, OCI image extracted to a 9P server, etc.).
- A restricted `/dev/` showing only the device servers permitted.
- A private `/proc/` view (typically scoped via `rfork(RFNAMEG)` and namespace pruning).
- A private set of capabilities (reduced via `rfork`).

There is no separate container runtime, no cgroups, no namespaces-as-kernel-feature beyond the per-process namespace. Linux container images run via namespace construction, not via any new kernel mechanism. This is the Plan 9 / per-connection-9P-namespace model that Stratum already commits to (Stratum's NOVEL angle #8); the OS extends it to per-process.

### 3.3 The shell is the graphical environment

Halcyon is not a terminal emulator running inside a window manager. It is the graphical environment. The display surface is a scroll buffer where each entry is either a text region or a pixel-addressable graphical region. Images render inline. Video plays inline. Halcyon controls scroll, history, focus, and command launching.

There is no compositor. There is no scene graph. There is no window manager. There is Halcyon.

This is a deliberate scope choice with explicit consequences. Some applications cannot be built on this model — multi-pane IDEs, web browsers, side-by-side document editors. Those applications are out of scope (§9 non-goals). The scroll-buffer-with-inline-graphics model handles every workflow Thylacine targets: image viewing, video playback, code editing, REPL-driven development, file management, system administration, monitoring.

### 3.4 Correctness before compatibility

Compatibility with Linux/POSIX binaries is a useful property, not a design constraint. The kernel API is Plan 9-heritage first. Compatibility is provided by translation layers (musl port, syscall shim, synthetic Linux-shaped 9P servers), not by designing the kernel around a foreign ABI.

When a Linux behavior conflicts with a Plan 9 behavior, Plan 9 wins internally; the compat layer translates. Examples:
- Internal signal model: Plan 9 notes (text messages). POSIX signals: translation in musl.
- Internal process model: `rfork` with flags. POSIX `fork`, `clone`, `pthread_create`: translated to `rfork(...)`.
- Internal multiplexed I/O: 9P pipelining + `rendezvous`. POSIX `poll` / `select` / `epoll`: translated to 9P-based wait/wake.

### 3.5 Userspace drivers, from day one

Kernel drivers are the exception, not the rule. The preferred — and from Phase 3 onward, the only — driver model is a userspace process that owns device memory and IRQs (via typed kernel handles) and exposes the device as a 9P server. The kernel provides the scaffolding (handle table, IRQ forwarding, VirtIO core for transport); the driver logic lives outside it.

Crashes are isolated. Updates don't require reboots. Auditing is tractable. The 9P interface is the public contract; the handle is the private mechanism.

This commitment is non-negotiable. The priming Phase 0 plan included an in-kernel virtio-blk for "Phase 3 expedience, promote to userspace by Phase 6." That deferral is rejected; Phase 3 ships userspace drivers from day one. The cost is that Phase 2 must produce a complete handle table and VMO manager before Phase 3 begins. That cost is paid.

### 3.6 Maximum complexity, maximum verification

The kernel will not be small. By v1.0 it implements: namespace management, EEVDF scheduler, virtual memory with NUMA-shape support, a 9P client with pipelining, handle table, VMO manager, IRQ forwarding, `poll`, `futex`, signal delivery, PTY infrastructure, the entire compat shim. That is a real kernel — not minimalist for its own sake.

**Maximum complexity is permitted where it can be verified.** Every load-bearing invariant gets a TLA+ specification before the implementation is written. Fuzzers, sanitizers, and adversarial audit rounds are part of the development loop, not an afterthought. The audit pattern proven by Stratum (15 audit rounds, ~60 corruption-class fixes found, every round caught bugs the test suite missed) becomes Thylacine's permanent development cadence.

The trade is honest: Thylacine is not a minimalist OS. It is a **maximally rigorous** OS — the *interface* layer is simple (one mechanism: 9P), the *implementation* is no smaller than it has to be to do the job correctly, and the gap is closed by formal verification + adversarial audit + sanitizer-clean test matrices.

---

## 4. Target

### 4.1 Architecture

**Primary**: ARM64 (AArch64). No x86-64 at v1.0.

ARM64 is chosen because:
- Thylacine's primary development machine is Apple Silicon (M-series Mac).
- QEMU's `virt` machine on ARM64 runs under Hypervisor.framework with hardware virtualization — no instruction-level emulation, near-native performance during development.
- The ARM64 ISA is clean: no segmentation, no 40 years of compatibility barnacles, a coherent exception level model (EL0 user, EL1 kernel, EL2 hypervisor available, EL3 secure monitor available), and hardware security extensions (PAC, MTE, BTI) that matter for Thylacine's security posture.
- ARMv8.1+ Large System Extensions (LSE) atomics deliver lock-free primitives at hardware speed; LL/SC is the fallback for older hardware.
- x86's days as the primary platform are numbered. ARM64 dominates current development hardware and will dominate servers within Thylacine's v1.0 horizon.

x86-64 support is a v2.0 consideration. The abstractions built for ARM64 will be cleaner for having not been contaminated by x86 requirements, in the same sense that Stratum's design was cleaner for not having been ext4-compatible.

### 4.2 Development target

**Primary dev target**: QEMU `virt` machine, ARM64, on an Apple Silicon Mac, under Hypervisor.framework.

The `virt` machine gives:
- VirtIO block, network, GPU (virgl), input — clean, well-documented, designed for this purpose.
- GIC v2 or v3 interrupt controller (autodetected from DTB).
- PL011 UART for early-boot debug.
- ARM generic timer for tickless scheduling.
- PSCI for CPU on/off, reset, shutdown.
- VirtIO-GPU with virgl for hardware-accelerated rendering via the host GPU — Halcyon's framebuffer.

The gap between `virt` and Apple Silicon bare metal is real and deferred. Bare-metal Apple Silicon is a v2.0 goal (dependent on Asahi Linux's work for the AGX GPU and AIC interrupt controller).

### 4.3 First bare-metal target (post-v1.0)

**Raspberry Pi 5.** Chosen because:
- GIC-400, PL011 UART, ARM generic timer all transfer directly from QEMU `virt` — no driver rewrite.
- VideoCore VII + mailbox: framebuffer via mailbox, same `/dev/fb/` 9P interface as VirtIO-GPU.
- Pi 5 + M.2 NVMe HAT: Stratum on real NVMe hardware.
- Network boot via TFTP: fast iteration loop without SD-card swapping.
- Public datasheets, open-source Linux drivers as reference.

Delta from QEMU `virt` to Pi 5 bare metal: EL2→EL1 drop sequence, RP1 mailbox setup, RP1 Ethernet driver. Estimated: one focused sprint after v1.0 stabilizes. See `ROADMAP.md §12.1`.

### 4.4 Scale targets

| Tier | Cores | RAM | Disk | Use |
|---|---|---|---|---|
| Minimum | 1 vCPU | 512 MiB | 1 GiB | Tests, CI |
| Typical dev | 4 vCPUs | 4 GiB | 64 GiB | Daily development VM |
| Production dev | 8 vCPUs | 16 GiB | 1 TiB | Heavy workloads inside VM |
| Stretch | 16 vCPUs | 64 GiB | 8 TiB | Validation only at v1.0 |

Single-machine OS. No distributed computing, no cluster management. 9P is network-capable; Thylacine itself does not manage a cluster.

### 4.5 Tail-latency budget (committed at v1.0)

Throughput-focused budgets are insufficient: a 50 ms commit pause is invisible as throughput loss but visible to any user expecting interactive latency. The committed v1.0 budget:

| Operation | p50 | p99 | p99.9 |
|---|---|---|---|
| Boot to UART banner | — | — | < 500 ms |
| Boot to login prompt | — | — | < 3 s |
| Syscall, no contention (e.g. `getpid`) | < 200 ns | < 500 ns | < 1 µs |
| Syscall, kernel-handled (e.g. `read`/`write` on pipe) | < 1 µs | < 5 µs | < 20 µs |
| Process creation (`rfork(RFPROC) + exec`) | < 200 µs | < 1 ms | < 5 ms |
| 9P round-trip (loopback Stratum) | < 50 µs | < 500 µs | < 2 ms |
| IRQ to userspace driver handler | < 1 µs | < 5 µs | < 20 µs |
| Halcyon frame time (60 Hz floor) | < 10 ms | < 16 ms | < 33 ms |

These are aggressive — seL4 / Fuchsia / Helios territory — and achievable on QEMU + Hypervisor.framework on Apple Silicon. They are gated:
- Boot times: Phase 8 v1.0 release exit.
- Syscall latencies: measured continuously from Phase 2 onward.
- 9P round-trip: gated at Phase 4 exit.
- Halcyon frame time: gated at Phase 6 exit.
- IRQ-to-handler: gated at Phase 8 v1.0 release exit (drives the userspace-driver argument; if we miss this number, the driver model is broken).

Tail-latency regression is treated as a bug, not a performance variance.

---

## 5. Properties, ranked

In order of priority when they conflict. Each property has a brief rationale; the conflict-tiebreak rule is the single most important output of this section.

1. **Correctness.** The kernel must not corrupt data, violate namespace isolation, or permit privilege escalation. Correctness is non-negotiable. Every load-bearing invariant has a formal spec; every audit-trigger surface has an adversarial review.
2. **Security.** Namespace isolation as the primary boundary. Stratum's cryptographic integrity as the storage boundary. Userspace drivers as the fault-isolation boundary. Hardware extensions (PAC, MTE, BTI) used everywhere they apply. KASLR, ASLR, W^X, CFI as baseline.
3. **Auditability.** The interface layer is simple — one mechanism: 9P. The implementation is no smaller than it must be to do the job correctly, but every component has a bounded specification and a documented interface. A reviewer should be able to read one component without depending on knowledge of the rest. (Renamed from "Simplicity" — the priming was honest about the kernel's scope; auditability is the property worth optimizing.)
4. **Usability.** Halcyon should be a genuinely pleasant environment. The graphical shell is not a compromise; for the workflows it covers, it should be better than a terminal emulator running inside a windowing system.
5. **Compatibility.** Linux/POSIX binary compatibility is useful. It is not load-bearing. Degraded compatibility is acceptable; degraded correctness or security is not. musl-static and musl-dynamic ARM64 binaries run; glibc binaries run on best-effort.
6. **Performance.** Fast enough not to be the bottleneck — see the latency budget in §4.5. Throughput is permitted to land at ~70-80% of bare metal; tail latency is not permitted to spike. Performance is enforced by budget, not optimized at the expense of higher-priority properties.

**Conflict-tiebreak rule**: when properties N and M conflict, N wins iff N is ranked higher. This is not flexible. A design proposal that improves performance at the cost of correctness is rejected. A design proposal that improves usability at the cost of compatibility is accepted.

---

## 6. Comparable systems

A non-exhaustive list of systems this project should be measured against. Detailed comparisons are in `COMPARISON.md`.

| System | What they did right | Where they fell short | Relevance |
|---|---|---|---|
| **Plan 9 from Bell Labs** | Namespace model, 9P, "everything is a file" taken literally, factotum, Dev vtable | Did not survive the industry transition; graphics model (rio/8½) didn't fit the modern workflow; user-facing applications never reached critical mass | Direct ancestor; we inherit the model |
| **9Front** | Kept Plan 9 alive; better hardware support; SMP improvements; modern USB | Still Plan 9-shaped; no encryption story for storage; not aimed at being a "daily driver" | Reference implementation; many decisions cite 9Front |
| **Linux** | Hardware support, tooling, ecosystem, performance | Architectural sprawl; Unix semantics underneath all the modernization; capability model (capabilities, namespaces, cgroups) is bolt-on | Compat target; not an architectural model |
| **Fuchsia / Zircon** | Capability-based microkernel; typed handles; VMOs as first-class kernel objects; pipelined IPC | Closed development model; complex protocol surface; failed product strategy | Architectural source for typed handles + VMOs (subordinated to 9P) |
| **seL4** | Formally verified microkernel; capability-based; minimal TCB | Hostile ergonomics for application development; capability mediation everywhere is heavyweight | Verification model; the "specs come before code" methodology |
| **Redox** | Microkernel + Rust + 9P-ish scheme model | Limited driver coverage; small ecosystem; not yet daily-driver capable | Adjacent project; valuable lessons on Rust + microkernel trade-offs |
| **Helios (CMU)** | Per-core kernel instances + RPC channels (multikernel ancestor); influences pipelined 9P | Research-only; never aimed at a usable system | Architectural inspiration for v2.x multikernel direction |
| **Barrelfish** | Multikernel architecture; per-core state with explicit messages | Research-only; no production users | Multikernel design-direction reference |
| **MINIX 3** | Userspace drivers; reincarnation server | Slow (~8% L4 perf overhead); never reached daily-driver status | Userspace-driver reference; cautionary tale on IPC overhead |

The closest peer in spirit is **Plan 9 / 9Front**. The closest peer in technical substrate is **Fuchsia**. The closest peer in development methodology is **seL4** + **Stratum**. Thylacine combines all three.

---

## 7. Novel angles (preview)

Detailed in `NOVEL.md`. Each is a concrete, testable commitment.

| # | Angle | Risk | Sequence |
|---|---|---|---|
| 1 | 9P-as-universal-composition, total | Low | Foundational |
| 2 | Userspace drivers via typed handles + VMO zero-copy | Medium | Phase 2-3 |
| 3 | Pipelined 9P client (out-of-order, flow-controlled) | Low | Phase 4 |
| 4 | Halcyon graphical scroll-buffer shell, no windowing system | Medium | Phase 6 |
| 5 | POSIX surfaces as 9P servers (`/proc`, `/dev`, `/sys`, `/dev/pts`) | Low | Phase 5-7 |
| 6 | Stratum as native filesystem from Phase 4 | Low | Phase 4 |
| 7 | Per-process namespace inheriting Stratum's per-connection namespace | Low | Phase 4 |
| 8 | EEVDF scheduler with SOTA hardening (PAC, MTE, KASLR, ASLR, W^X, CFI, LSE atomics) from day one | Medium | Phase 1-2 |
| 9 | Formal-verification cadence: nine TLA+ specs gate-tied to phases | Low-medium | Continuous |
| 10 | Designed-not-implemented v2.0 hooks: factotum capability mediation, multikernel SMP, in-kernel Stratum driver | Low | Phase 0 contracts; v2.0 implementations |

Total novel-angle implementation: ~50-70 KLOC of C99 (kernel) + ~20-30 KLOC of Rust (userspace drivers, Halcyon) + ~3-5 KLOC of TLA+. Within the ~100-130 KLOC budget for a complete v1.0.

---

## 8. Load-bearing invariants (first pass)

These are the invariants that, if violated, constitute correctness or security failure. Every one of these gets a TLA+ spec or equivalent formal treatment; every one is enumerated in `ARCHITECTURE.md §N` (the "Invariants that must hold" section). These are the audit-trigger anchors.

**Process and isolation invariants**:
- I-1. **Namespace isolation**: namespace operations in process A do not affect process B's namespace, except via explicit shared-mount handoff at process creation.
- I-2. **Capability monotonicity**: a process's capability set can only be reduced (via `rfork`), never elevated, without kernel mediation.
- I-3. **Namespace acyclicity**: mount points form a DAG, never a cycle.

**Handle and capability invariants**:
- I-4. **Handle subordination**: handles transfer between processes only via 9P sessions. No syscall exists for direct handle transfer.
- I-5. **Hardware-handle non-transferability**: `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` handles cannot be transferred — the transfer syscall has no code path for them. Enforced at the syscall site.
- I-6. **Right monotonicity**: handle rights can only be reduced when transferring, never elevated.
- I-7. **VMO lifecycle**: a VMO's pages remain live until the last handle is closed *and* the last mapping is unmapped. No reader-after-free at the kernel level.

**Concurrency invariants**:
- I-8. **Scheduler progress**: every runnable thread eventually runs (no starvation under EEVDF's deadline computation).
- I-9. **Wakeup atomicity**: no wakeup is lost between a thread's wait-condition check and its sleep.
- I-10. **9P tag uniqueness**: per session, no two outstanding requests share a tag.
- I-11. **9P fid uniqueness**: per session, fid identity is stable for the duration of its open lifetime.

**Memory invariants**:
- I-12. **W^X**: every page is writable or executable, never both, never transitionable without explicit syscall mediation.
- I-13. **Kernel-userspace isolation**: TTBR0 (user) and TTBR1 (kernel) page tables enforce that userspace cannot read or write kernel memory; speculation-attack mitigations (KPTI-equivalent if needed on relevant hardware) gate side channels.

**Cryptographic invariants** (delegated to Stratum but observed at the OS boundary):
- I-14. **Storage integrity**: every block read from Stratum is integrity-verified against the Merkle root before being returned to userspace.

**Boot and recovery invariants**:
- I-15. **DTB-driven discovery**: the kernel's hardware view derives entirely from the DTB; no compile-time hardware constants exist outside `arch/arm64/<platform>/`.
- I-16. **KASLR**: the kernel image base address is randomized at boot.

This list is the first pass. `ARCHITECTURE.md §N` enumerates the full set with file-level and spec-level traceability. Every entry maps to one or more TLA+ specs or runtime-checked assertions.

---

## 9. Non-goals

Explicit, with rationale. Each non-goal is a deliberate scope choice; relaxing a non-goal requires re-opening this section.

**Out by design**:

- **Graphical windowing system.** No compositor, no window manager, no display server. Halcyon is the display surface. If you need a windowing system, Thylacine is not your OS. Rationale: design coherence — everything is 9P, including the display; a separate windowing system would be a second composition mechanism.
- **Web browser, multi-pane IDE, Office-suite-class applications.** These applications are designed around overlapping windows, side-by-side panes, or document-style canvases. Halcyon's scroll-buffer model does not host them. Rationale: the OS targets shell-driven workflows; targeting GUI applications would invalidate the shell-as-UI thesis.
- **Audio at v1.0.** No sound system. Rationale: a half-baked audio stack is the kind of stub the project explicitly rejects; ship it properly post-v1.0 or not at all.
- **Bluetooth, USB peripherals beyond keyboard+mouse, hardware sensors, hardware acceleration beyond VirtIO-GPU.** v1.0 hardware: storage (VirtIO block / NVMe), network (VirtIO net / Ethernet), display (VirtIO GPU / framebuffer), input (VirtIO input / USB HID). Rationale: dev VM target; real-hardware breadth comes post-v1.0.
- **x86-64 support at v1.0.** ARM64 only. Rationale: clean abstractions; the cost of carrying x86 from day one outweighs the userbase win at v1.0 scale.
- **setuid binaries.** Plan 9 didn't have setuid; Thylacine doesn't either. Rationale: setuid is a security disaster; the post-v1.0 capability-elevation mechanism (factotum-mediated) is designed in `ARCHITECTURE.md §15` but not implemented at v1.0. v1.0 programs that need elevated privileges run as elevated users from process creation.
- **Distributed or clustered OS.** Single machine. 9P can be used over a network, but Thylacine itself does not manage a cluster. Rationale: scope; clustering is Ceph / Kubernetes territory.
- **Windows binary compatibility.** Not in scope, ever.
- **NUMA optimization at v1.0.** Single-socket only. Rationale: dev VM target; allocator is NUMA-shaped so v2.0 multi-socket is configuration, not rewrite.

**Deliberately out at v1.0, contracted in `ARCHITECTURE.md` for v2.0 implementation** (the "designed-but-not-implemented" pattern):

- **Capability elevation via factotum.** Designed in `ARCHITECTURE.md §15.4`. v1.0 has no elevation mechanism.
- **Multikernel SMP** (Barrelfish-style per-core kernel instances). Designed sketch in `ARCHITECTURE.md §20.6`. v1.0 is a single-kernel-with-per-CPU-data-discipline.
- **In-kernel Stratum driver** (bypass 9P client overhead for root FS). Designed in `ARCHITECTURE.md §14.4`. v1.0 mounts Stratum via the kernel's 9P client.
- **`epoll`, `inotify`, `io_uring` syscall surfaces.** `poll` and `select` are v1.0; the others land post-v1.0. Linux binaries that require them are best-effort at v1.0.
- **HW video decode.** Software decode at v1.0; HW decode via VirtIO video extension post-v1.0.
- **In-kernel Rust components.** The kernel is C99 at v1.0; selected modules (9P client, ELF loader, handle table) are candidates for Rust port at v2.0. Code is written so the port is mechanical.

---

## 10. Relationship to Plan 9 and 9Front

Thylacine is not a Plan 9 fork. It does not derive from Plan 9 source code. It is a new OS that takes Plan 9's ideas seriously and implements them in 2026, on current hardware, with current tools, current security understanding, and current verification methodology.

**Specifically inherited from Plan 9**:
- The namespace model (per-process, composable via `bind`/`mount`).
- 9P (specifically 9P2000.L; see `ARCHITECTURE.md §11`) as the universal IPC and resource protocol.
- The `Dev` vtable pattern for kernel devices.
- `rfork` as the universal process/thread creation primitive.
- Notes as the internal asynchronous-message model (POSIX signals are a translation surface).
- The principle that authentication and key management are separate services (→ janus).
- The synthetic filesystem as the administration interface (→ `/ctl/`, `/proc/`).
- `Walkqid`, fid, qid, the open Chan as the fundamental kernel currency.

**Specifically not inherited**:
- Plan 9's graphics model (rio / 8½). Halcyon replaces it entirely.
- Plan 9's C dialect (with `auto`, function nesting, `channel` keywords, etc.). Thylacine uses standard C99 in the kernel.
- Plan 9's network stack. Thylacine uses a modern TCP/IP stack (smoltcp-Rust port or Plan 9 IP stack port; `ARCHITECTURE.md §13` decides).
- Plan 9's on-disk filesystem (cwfs, fossil, kfs). Stratum replaces it entirely.
- Plan 9's authentication protocol (p9any, p9sk1). Thylacine uses Stratum's PQ-hybrid wrap-key mechanism via janus; 9P-level authentication delegates to it.

**9Front is acknowledged as the most vital continuation of Plan 9 ideas**. Where 9Front has made good decisions (improved USB, better hardware support, SMP improvements, various protocol refinements), those decisions are studied and respected. Thylacine does not compete with 9Front — it asks a different question. 9Front asks "how does Plan 9 stay alive?". Thylacine asks "what would Plan 9 have become if the industry had not walked away?".

---

## 11. Relationship to Stratum

Stratum is Thylacine's native filesystem. This relationship is intentional and architectural:

- **Stratum is internally feature-complete as of 2026-05-04.** Phases 1-7 of Stratum's v2 roadmap (foundations, lock-free Bε-tree, persistence, integrity + crypto, multi-device, namespaces, cold tier + features) are landed. Phase 8 (POSIX surface — inodes, dirents, xattrs, ACLs, the full set of modern POSIX file/dir ops including every `O_TMPFILE` / `F_SEAL_*` / `FALLOC_FL_*` / `renameat2` / `name_to_handle_at` / `*at` flag and primitive) is in progress. Phase 9 (9P server + clients) is queued.
- **Thylacine integrates with Stratum's Phase 9 9P server.** Specifically the 9P2000.L dialect with Stratum extensions: `Tbind` / `Tunbind` (per-connection subvolume composition), `Tpin` / `Tunpin` (snapshot pinning), `Tsync` (commit), `Treflink` (`copy_file_range` with reflink), `Tfallocate` (every `FALLOC_FL_*` flag). Thylacine's kernel 9P client (Phase 4) must speak these extensions; the syscall layer translates Thylacine native calls into Stratum protocol messages.
- **Stratum's per-connection namespace model maps directly to Thylacine's per-process namespace model.** Each Thylacine process establishes its own 9P connection to Stratum at process creation; each connection gets its own Stratum-side namespace; the per-process Thylacine namespace and the per-connection Stratum namespace are complementary layers. The Thylacine namespace governs cross-server composition (e.g. mounting Stratum + a network FS + `/dev/`); the Stratum per-connection namespace governs composition *within* a single Stratum connection (e.g. multiple subvolumes overlaid via `Tbind`). A program that wants `home/` from one Stratum subvolume and `code/` from another can do it either way: two `mount()` syscalls (Thylacine-level), or one mount + two `Tbind` ops (Stratum-level). Both work; the first is simpler, the second more efficient.
- **Stratum's `janus` key agent is a userspace 9P server** — exactly the driver model Thylacine uses for everything. It runs unchanged on Thylacine. Authentication backends (passphrase, TPM 2.0, YubiKey, PKCS#11) all transfer; janus on Thylacine is the same binary as janus on Linux/macOS.
- **Stratum's formal verification posture** (TLA+ specs, adversarial audit loop, property-based tests, fuzz harness) is the model for Thylacine's own correctness work. The audit pattern, the spec naming convention, the buggy-config counterexample pattern — all transfer. Stratum has 8+ TLA+ specs (`sync.tla`, `nonce.tla`, `allocator.tla`, `tree_commit.tla`, `namespace.tla`, `inode.tla`, `dirent.tla`, plus extensions); Thylacine targets 9 (`scheduler.tla`, `namespace.tla`, `handles.tla`, `vmo.tla`, `9p_client.tla`, `poll.tla`, `futex.tla`, `notes.tla`, `pty.tla`).

Stratum was not designed for Thylacine. It will fit as if it was. The fit is evidence that both projects are built on the same proposition: 9P as the universal composition mechanism.

**Stratum/Thylacine coordination at Phase 4**:

Thylacine Phase 4 requires Stratum's Phase 9 9P server with the Stratum extensions. Stratum's Phase 9 is queued after Phase 8 (POSIX surface) completes. Thylacine Phases 1-3 (kernel skeleton, process model, device model) can proceed in parallel with Stratum's Phase 8-9 work; the dependency hits at Phase 4 entry. Practically: Stratum Phase 9 lands the 9P server + bindings + CLI; Thylacine Phase 4 consumes that 9P server inside the QEMU VM as the root filesystem.

The encryption / snapshot / MVCC / multi-device / dedup / cold-tier features of Stratum are Thylacine-transparent — the kernel sees only the 9P interface. None of those features gate Thylacine's progress. Only the 9P server and its Stratum extensions matter for integration.

---

## 12. Relationship to Linux and POSIX

Thylacine is not a Linux clone. It is an OS that *runs* a useful subset of Linux software via translation, where translation can be done without compromising the kernel's design.

**Tier 1 — native**: programs compiled for Thylacine against the musl port. Build clean, link clean, run with full functionality. Target: BusyBox coreutils, bash, rc, gcc/clang, vim, ssh, curl, Python, the entire Plan 9 userland (rc, troff, mk, etc.).

**Tier 2 — static Linux ARM64 binary**: pre-built static ARM64 ELF binaries that use a top-50 Linux syscall surface. Run via the kernel's syscall translation shim. Best-effort; works for most CLI tools.

**Tier 3 — Linux container**: OCI container images run inside a Thylacine namespace. The kernel primitive (namespace) handles container isolation; synthetic Linux-shaped 9P servers (`/proc`, `/sys`, `/dev` Linux layout) provide the expected files. This is the "flatpak / Steam Deck" model — containers as namespaces, not as a separate kernel subsystem.

**Out of scope at v1.0**: glibc-dynamic binaries (no `/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`); programs that require `epoll` / `inotify` / `io_uring` (those land post-v1.0); programs that require non-trivial `/sys` parsing.

The compat layer is *additive*: it does not modify the kernel API. The kernel API is Thylacine-native; compat is a userspace library + a thin syscall shim. Removing the compat layer leaves a working OS.

---

## 13. Halcyon — the graphical shell

Halcyon deserves its own section in the vision document because it is the most unusual design decision and the most exposed to "this can't work" objections.

The premise: a shell that is also the graphical environment. Not a terminal emulator inside a window manager — the shell itself renders images, plays video, and displays rich output, with the scroll buffer as the display surface.

### 13.1 The scroll-buffer model

The output stream is a sequence of entries. Each entry is either:
- A **text region**: rendered in the current font with standard terminal semantics (cursor position, color attributes, line wrapping).
- A **graphical region**: a fixed-dimension pixel buffer, rendered inline, scrolling naturally with the text stream.

When an image is displayed, it occupies N lines of the scroll buffer as a rendered bitmap. When output scrolls, the image scrolls with it. History is preserved. The model is simple enough to implement correctly, simple enough to formally specify the layout state machine, and rich enough to replace a windowing system for daily work.

A rough sketch of what a Halcyon session looks like:

```
$ ls
src/  docs/  Cargo.toml  README.md
$ display thylacine.png
[image: 1024×768 PNG, occupies 30 lines of scroll buffer]
$ play recording.mp4
[video: 720×480, plays inline, scrolls if more output appears]
$ cat src/main.rs | grep fn
fn main() { ... }
fn parse_args() { ... }
$
```

### 13.2 Video via synthetic filesystem

Video playback is not a Halcyon built-in. It is a 9P server:

```
/dev/video/player/
    ctl          ← write: "play <file>", "pause", "seek <seconds>", "stop"
    position     ← read: current position in seconds
    duration     ← read: total duration
    frame        ← read: current decoded frame (VMO handle, ARGB32)
    audio/ctl    ← write: "volume <0..100>", "mute", "unmute"
```

Halcyon mounts the video server, polls `frame` (which is a VMO handle for zero-copy access), and blits the result into a graphical region in the scroll buffer. Hardware video decoding (post-v1.0) is a driver that exposes this same interface. The shell does not need to know about codecs.

### 13.3 What Halcyon is not

- Not a Wayland compositor.
- Not an X11 server.
- Not Rio or 8½.
- Not a terminal emulator running inside anything else.
- Not capable of side-by-side editing, multi-pane layouts, or overlapping windows.

These are explicit non-features. If you need them, Thylacine is not your OS.

### 13.4 Implementation

Halcyon is written in Rust. It depends on:
- The Thylacine kernel's framebuffer device (a 9P server, userspace driver).
- A font rendering library (fontdue or equivalent — no FreeType dependency).
- A shell parser (custom; bash-subset compatible for interactive use; rc available as the scriptable alternative).
- The 9P client library for mounting servers.

Rust is chosen because Halcyon parses bash-subset syntax, manages a scroll buffer with mixed text/graphics regions, decodes images (PNG via the `png` crate), and renders fonts — these are exactly the domains where C's lack of bounds checking and string handling has historically produced UAF + buffer overflow CVEs (cf. bash CVEs over the years). The borrow checker eliminates a class of bugs that Halcyon's 9P-server boundary would otherwise need to defend against.

---

## 14. Open questions

Tracked here, resolved in `ARCHITECTURE.md` or carried into Phase 1+:

- **Q1**. EEVDF lookahead bound: what's the right service-time horizon? (Implementation decision in Phase 2; spec'd in `specs/scheduler.tla`.)
- **Q2**. Should Halcyon support transient pop-up regions for password prompts and confirmation dialogs? (Tentatively no — the password prompt is just a line of text reading from `janus`; confirmation is a `[Y/n]` prompt. If we ever need a pop-up, we've drifted into windowing-system territory.)
- **Q3**. v1.0 maximum core count. Tentatively 8; could push to 16 with extra Phase 8 testing. (Decision at Phase 8 entry.)
- **Q4**. Userspace TCP/IP: smoltcp Rust port vs. Plan 9 IP stack port to Rust. (Phase 7 decision; smoltcp is the default unless its memory profile or RFC-completeness is inadequate.)
- **Q5**. Video player codec coverage at v1.0. H.264 software decode is the baseline. AV1, HEVC, VP9 deferred to post-v1.0 unless one comes essentially for free via a FFI binding.
- **Q6**. Stratum extension propagation timing: when do new Stratum 9P extensions (post Phase 9) reach Thylacine's 9P client? Tentatively each Stratum extension that reaches Stratum's stable surface is added to Thylacine's client within a kernel patch release. The 9P client is designed to negotiate features at session establishment.

---

## 15. Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Initial draft (Phase 0). | Ground-up rewrite from `tlcprimer/VISION.md` after Phase 0 challenge round. C99 kernel confirmed; userspace drivers from Phase 3; SOTA hardening (KASLR/ASLR/W^X/CFI/PAC/MTE/LSE) committed; tail-latency budget tightened; per-process 9P-connection model committed; designed-not-implemented pattern adopted for v2.0 contracts. |

---

## 16. Summary

Thylacine OS is the operating system Plan 9 would have become if the industry had not walked away. It is built from first principles, in the right language for each layer (C99 kernel, Rust userspace), for the right architecture (ARM64 first), with the right filesystem underneath it (Stratum), with the right shell as the face of it (Halcyon), and with maximum verification rigor at every level — formal specs, adversarial audits, sanitizer matrices, no shortcut implementations.

It is ambitious. The ambition is bounded, sequenced, and verified.

The thylacine was real. So is this.
