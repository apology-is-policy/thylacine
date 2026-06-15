# Thylacine OS — Roadmap

**Status**: Phase 0 draft, 2026-05-04. This document translates `ARCHITECTURE.md` committed decisions into a staged implementation plan. Time estimates are guidance, not commitments.

**Companion documents**: `VISION.md`, `COMPARISON.md`, `NOVEL.md`, `ARCHITECTURE.md`.

---

## 1. Purpose

`ROADMAP.md` answers:
- What do we build, in what order, and why?
- What are each phase's exit criteria?
- Where are the risk hot spots?
- How does Stratum integrate at each phase?
- What formal specs land in which phase?
- What audit-trigger surfaces are introduced or modified per phase?

The roadmap is a commitment to an *ordering*, not to specific dates. Phases can overlap where dependencies allow. The Stratum lesson: month-based estimates are fiction; phase ordering is not.

---

## 2. Phase structure at a glance

```
Phase 0  ─ Design (DONE — VISION.md, COMPARISON.md, NOVEL.md, ARCHITECTURE.md, ROADMAP.md)

Phase 1  ─ Kernel skeleton                      [foundation]
            Boot, memory, exception vectors, UART, hardening defaults

Phase 2  ─ Process model + scheduler + handles  [core kernel]
            rfork, exec, territory, EEVDF, handle table

Phase 3  ─ Address spaces + page faults + exec  [userspace ramp]
            Per-Proc TTBR0, fault handler, BURROW mapping, /init in EL0

Phase 4  ─ Device model + userspace drivers     [device layer]
            Dev vtable, VirtIO core, userspace virtio-blk/net/input/gpu,
            hw-handle SVC surface (MMIO + IRQ + DMA + BURROW + rfork-w-caps)

Phase 5  ─ 9P client + Stratum integration      [storage] ★ THIS DOC'S §7
            Pipelined 9P client (specs/9p_client.tla first); kernel mounts
            stratumd's Unix socket; .key sidecar unwrap; corvus key agent;
            /ctl/ admin synthetic FS consumed; ramfs → Stratum pivot.
            SUSPENDED, substantially complete — the real-stratumd tail
            resumes after Phase 6 (pouch) provides the libc.

Phase 6  ─ Pouch: POSIX libc + cross-compilation [practicality] ★ §7A
            The pouch libc (musl-derived); aarch64-thylacine cross-
            toolchain + sysroot; POSIX runtime layer (sockets→/srv,
            poll→t_poll, signals→notes, pthreads→Thylacine threads +
            the torpor wait-on-address primitive); real stratumd built
            + booted. Binding design: POUCH-DESIGN.md.

Phase 7  ─ Syscall surface + Utopia              [userspace milestone]
            Full syscall table, uutils-coreutils, rc, bash, the rich
            Unix namespace (/proc, /dev/pts/), Utopia ships — on pouch

Phase 8  ─ Linux compat + network               [practical OS]
            Linux ARM64 binary shim, container runner, network stack,
            polished Utopia + binary shims = practical working OS

Phase 9  ─ Hardening + audit + v1.0-rc           [stable substrate]
            Fuzz, audit, benchmarks, 8-CPU stress, docs, v1.0-rc tag.
            If Halcyon (Phase 10) slips, this v1.0-rc ships as v1.0.

Phase 10 ─ Halcyon + v1.0 final                  [graphical layer]
            Framebuffer driver extensions, scroll-buffer shell, image,
            video, Halcyon-surface audit, v1.0 final release.

Post-v1.0 (v1.1, v1.2, v2.0+) — see §16
```

### 2.1 Section ↔ phase mapping

The execution numbering reflects what status docs use (`docs/phaseN-status.md`). This document's section headers in §4-§11 are stale relative to execution numbering — they predate **two** phase insertions: Phase 3 (address spaces + exec + /init) and Phase 6 (Pouch, inserted 2026-05-22 — see `POUCH-DESIGN.md`). A full header + in-body phase-number renumber across ROADMAP / ARCHITECTURE / VISION / CLAUDE.md is a tracked deferred doc-hygiene pass. **This table is the authoritative phase registry**; until the renumber lands:

| Execution phase | This doc's section | Header label (stale) |
|---|---|---|
| Phase 1 — Kernel skeleton | §4 | "Phase 1: Kernel skeleton" |
| Phase 2 — Process model + scheduler + handles | §5 | "Phase 2: Process model + scheduler + handles" |
| Phase 3 — Address spaces + page faults + exec | (no section — see `docs/phase3-status.md`) | — |
| Phase 4 — Device model + userspace drivers | §6 | "Phase 3: Device model + userspace drivers" |
| Phase 5 — 9P client + Stratum integration *(suspended; substantially complete)* | §7 | "Phase 4: 9P client + Stratum integration" |
| **Phase 6 — Pouch: POSIX libc + cross-compilation** | **§7A** | **"Phase 6: Pouch"** — binding design `POUCH-DESIGN.md` |
| Phase 7 — Syscall surface + Utopia | §8 | "Phase 5: Syscall surface + musl + Utopia" *(musl moved to Phase 6)* |
| Phase 8 — Linux compat + network | §9 | "Phase 6: Linux compat + network" |
| Phase 9 — Hardening + audit + v1.0-rc | §10 | "Phase 7: Hardening + audit + v1.0-rc" |
| Phase 10 — Halcyon + v1.0 final | §11 | "Phase 8: Halcyon + v1.0 final" |

When in doubt, the **execution phase** in this table is authoritative — it is what `docs/phaseN-status.md`, `memory/project_active.md`, and commit messages use. Document section headers + in-body "Phase N" references elsewhere lag pending the renumber pass.

### 2.2 The HOLOTYPE RW-13 re-plan (2026-06-11)

The thirteen-part whole-system deep review (`docs/HOLOTYPE.md`, RW-0..RW-13) closed 2026-06-11. It found the specimen **sound** (0 P0 across the tree; ~15 P1 + ~40 P2 all fixed in-arc; the three cross-cuts returned 0 soundness) and re-planned the road to a maximal-coherent v1.0. The full triage + the four voted scope decisions live in `docs/holotype/13-consolidation.md`; this subsection is the roadmap-facing summary.

**The four v1.0 scope decisions (voted 2026-06-11):**
- **D1 — Networking is IN v1.0** (Phase 8). The Plan 9 `/net`-via-`netd` shape stands; the net design pass is **`docs/NET-DESIGN.md`** (the #68 charter, **LANDED 2026-06-15**) — it fills the eleven holes in section 9.1 (schema + fid state machine, DNS/`cs`, `/ctl/net` observability, poll-over-9P readiness, IP config, packet filter, TLS, NTP, server-side acceptance, the spec-waiver re-scope, the virtio-net/`/dev/ether0` reconcile) and binds three decisions: shared `netd` + namespace-narrowed views; pouch-userspace BSD-socket compat (no kernel socket syscalls); namespace-restriction firewall (explicit packet filter -> v1.x).
- **D2 — Containers: build the tractable core** for v1.0. exec-from-namespace (route the 5 spawn variants through stalk + per-component X-search, retiring the flat `devramfs_lookup`) + the namespace-introspection substrate (Spoor name-retention so the Plan 9 `ns` tool renders) build now; **union mounts move to v1.x** (COMPARISON `✓` -> `○`, fail-loud the no-op `bind_before/after`).
- **D3 — The on-system toolchain is IN v1.0** (NEW Phase-8 scope). clang/lld + make + git ported via Pouch; the self-hosting / build-storm (W2) story is part of v1.0. This makes VISION section 397 "no subset" + the Phase-9 "parallel make" criterion coherent — and is the single largest Phase-8 pole.
- **D4 — Pre-rc hardening lands FIRST** (scope-independent): the 6 ms wake-preemption slice cliff fix (the latency-budget gate cannot pass without it), a production `KERNEL_TESTS=OFF` boot configuration + gated joey ladder, and the Resource/DoS floor (per-Proc page/thread/child caps — a fork/thread/memory bomb currently extincts the box).

**The forward sequence** (the arc emerges here; the system resumes building):
RW-13 reconciliation pass (the long-deferred section-2.1 renumber propagation + the present-tense honesty edits — pure scripture) -> the D4 pre-rc sub-arc -> the container keystone (exec-from-namespace + ns substrate) -> the **LS line, Phase-7 completion** (LS-K id/whoami/date + the wall clock -> LS-8 termios/PTY -> LS-7 the nora editor -> LS-6 login echo) -> **Phase 8** (net arc -> container runner -> on-system toolchain -> Linux binary shim) -> **Phase 9** (hardening -> v1.0-rc.1) -> **Phase 10** (Halcyon -> v1.0 final). The section 10/11 fallback stands: if Halcyon slips, the Phase-9 rc ships as v1.0.

---

## 3. Principles throughout

### 3.1 Audit loop from Phase 1

Every change to an audit-trigger surface (per `ARCHITECTURE.md §25.4`) spawns a focused soundness audit before merge. Modeled on Stratum's 15-round audit history. Findings at P0/P1 severity block phase exit. P2 findings get tracked or closed before phase exit. P3 findings get tracked indefinitely with explicit justification.

### 3.2 Spec-first for invariant-bearing changes

Every TLA+ spec (per `ARCHITECTURE.md §25.2`) is written *before* its implementation phase begins. The spec is the source of truth; the implementation references it. CI runs TLC on every PR touching specified subsystems.

If TLC fails, the merge fails — the spec is fixed first, then the implementation. This is non-negotiable.

### 3.3 Tests are tiered

- **Unit tests**: per-function, per-syscall.
- **Integration tests**: boot a QEMU instance, run a workload, check result.
- **Stress tests**: long-running, with process creation/destruction, territory manipulation, I/O concurrency.
- **Sanitizer matrices**: every commit on the default build + ASan + UBSan + (TSan from Phase 2 onward when SMP is enabled).

Pre-commit: full test suite on default build. Pre-merge for invariant-bearing changes: all matrices + all specs.

### 3.4 Stratum is the reference deployment environment from Phase 5 onward

Once Phase 5 (= §7 by header label) lands, all development work happens inside a Thylacine QEMU VM with Stratum as the root filesystem. **Eating the dog food** from Phase 5 onward. Stratum v2 is shipping today (2026 Q2) — the integration binds to its stable 9P2000.L wire surface + Unix-socket-bound `stratumd` daemon, exactly as documented in `stratum/v2/docs/OS-INTEGRATION.md`.

### 3.5 No in-kernel drivers (with bounded exceptions)

If a device can be driven from userspace via VirtIO + 9P, it is. Kernel drivers are the last resort. The boundary is: interrupt entry, DMA setup, and the VirtIO transport core are in-kernel; protocol logic and device state are in userspace.

There are *no* userspace-as-shortcut deferrals — the priming's "in-kernel virtio-blk for Phase 3 expedience, promote to userspace by Phase 6" is rejected. Phase 3 ships userspace drivers from day one.

### 3.6 The compat layer is built on top, not baked in

The kernel API is Thylacine-native throughout. POSIX/Linux compatibility is implemented as a userspace library — **pouch**, the Thylacine-native POSIX libc (a musl derivative; binding design `POUCH-DESIGN.md`, execution Phase 6) — and a thin syscall translation shim. The kernel is never modified to accommodate POSIX requirements; this is invariant **P-1** (no foreign syscall number ever enters the kernel — POUCH-DESIGN.md §11).

### 3.7 Performance is measured continuously

Each phase has its own latency budget contribution; the cumulative budget at Phase 8 v1.0 release is `VISION.md §4.5`. Measurements run in CI against a reference QEMU configuration.

### 3.8 Documentation updates are part of every PR

`ARCHITECTURE.md` changes update first; `ROADMAP.md` (this document) updates next; status docs under `docs/phaseN-status.md` update per chunk; `docs/REFERENCE.md` updates per landed module. Code that lacks doc-update is reverted.

### 3.9 Hardening is on from Phase 1, not added later

Per `NOVEL.md` Angle #7. KASLR / ASLR / W^X / CFI / stack canaries / PAC / MTE / BTI / LSE — all enabled by default in v1.0 builds, from Phase 1 forward. No "hardening as opt-in" mode; the default build is the hardened build.

### 3.10 SOTA tenet: no shortcut implementations

Per `VISION.md §1` and `ARCHITECTURE.md §1.3`. Stubs and primitive implementations are accepted only when they make the overall roadmap cleaner — not when they merely defer work. Examples accepted: ramfs during bring-up (it's throwaway). Examples rejected: in-kernel virtio-blk for "Phase 3 expedience" (it would need to be redone).

---

## 4. Phase 1: Kernel skeleton

**Goal**: the kernel boots in QEMU `virt`, initializes hardware, and drops to a debug shell via UART. No processes, no virtual memory beyond identity + kernel mapping, no devices beyond UART.

### 4.1 Deliverables

**Boot path**:
- `arch/arm64/start.S`: exception vector table, EL1 entry, BSS clear, early stack, MMU enable with KASLR offset.
- `arch/arm64/kaslr.c`: KASLR seed extraction from DTB `/chosen/kaslr-seed`; offset application; relocation processing.
- `arch/arm64/uart.c`: PL011 UART driver (polled, no IRQ yet). Kernel `print()`.
- `lib/dtb.c`: minimal DTB parser. Extracts memory regions, GIC base, UART base, timer IRQ number, KASLR seed. No full libfdt.
- `kernel/main.c`: `boot_main()`. Calls subsystem inits in order. Prints boot banner. Hangs in a loop with a debug menu over UART.

**Memory subsystem**:
- `arch/arm64/mmu.c`: identity map for low (boot transition only), kernel mapping in TTBR1 with KASLR offset, MMU enable + invalidate.
- `mm/buddy.c`: physical frame allocator (buddy + per-CPU magazines per `ARCHITECTURE.md §6.3`). Single zone at v1.0; NUMA-shaped struct.
- `mm/slub.c`: kernel object allocator (SLUB-style per `ARCHITECTURE.md §6.4`). Standard caches: `kmalloc-N` for N in {8, 16, 32, ..., 262144}.
- `mm/vm.c`: kernel virtual memory map. Direct map + vmalloc region. `kmalloc` / `kfree` public surface.
- `mm/wxe.c`: W^X enforcement at the page-table layer; `_Static_assert` on PTE bit pattern; runtime rejection of forbidden combinations.

**Interrupt subsystem**:
- `arch/arm64/gic.c`: GIC v2/v3 detection from DTB, distributor init, CPU interface init, IRQ → handler dispatch table. Spurious IRQ handling. Timer IRQ registered (no scheduler yet — just counts ticks for boot debug).
- `arch/arm64/exception.c`: synchronous exception handlers (sync_kernel, sync_user). User SVC = panic at this phase (no syscalls yet).

**Hardening defaults from Phase 1**:
- All compile flags enabled per `ARCHITECTURE.md §24.2`.
- KASLR per `§5.3`.
- LSE atomic detection at boot per `§24.4`.
- PAC enable for kernel return addresses per `§24.3`.
- BTI enable per `§24.3`.
- MTE enable where supported per `§24.3`.

**Observability**:
- `/ctl/log/early` ring buffer for boot messages (in-kernel; flushed to UART).
- Boot timing instrumentation (rdtsc-equivalent: `CNTVCT_EL0`).
- `/ctl/kernel/base` exposing KASLR base for debugging.

### 4.2 Exit criteria

- [ ] QEMU `virt` ARM64 boots to a UART banner without crashing.
- [ ] Boot to UART banner: < 500ms (VISION §4.5 budget).
- [ ] `kmalloc`/`kfree` round-trip 10,000 allocations without leak (manual check via `/ctl/mm/leaks`).
- [ ] GIC initialized; timer IRQ fires at 1000 Hz (verified via UART counter).
- [ ] MMU on; kernel VA map correct (read/write kernel data, no fault).
- [ ] KASLR: kernel base address differs across boots (verified across 10 boots).
- [ ] LSE atomic ops verified via runtime detection (Apple Silicon QEMU); fall back to LL/SC works (older CPUs in test).
- [ ] PAC return-address signing verified (forge a return address; expect kernel panic with PAC-mismatch info).
- [ ] BTI enabled (deliberate indirect branch to non-BTI target panics cleanly).
- [ ] MTE enabled where supported (deliberate UAF detected by MTE).
- [ ] Sanitizer build (KASAN-equivalent for `kmalloc`) runs without false positives on boot path.
- [ ] No P0/P1 audit findings on the boot path.

### 4.3 Specs landing this phase

None mandatory. Optional: a sketch of `mmu.tla` for page table validity, but not gating.

### 4.4 Audit-trigger surfaces introduced

| Surface | Files | Why |
|---|---|---|
| Exception entry | `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S` | Every fault path |
| Allocator | `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c` | Allocation correctness |
| Page tables | `arch/arm64/mmu.c`, `mm/wxe.c` | W^X invariant |
| KASLR | `arch/arm64/kaslr.c` | Entropy + relocation correctness |
| LSE detection | `arch/arm64/atomic.S` | Runtime patching correctness |

### 4.5 Risks

- **DTB parsing**: QEMU's DTB is well-formed; real hardware is not. Risk is low for `virt` target. Mitigation: minimal hand-rolled parser; do not import libfdt at v1.0.
- **GIC v2 vs v3**: QEMU `virt` defaults to GICv2 in older versions, GICv3 in newer. Mitigation: support both; autodetect from DTB compatible string.
- **Boot timing budget**: 500ms is tight on QEMU. Mitigation: instrument every boot subsystem; profile and optimize hot paths if needed. Budget headroom assumes Hypervisor.framework.
- **Hardening interactions**: compiler flags occasionally interact in unexpected ways (e.g., `-fsanitize=cfi` + ThinLTO + custom linker scripts). Mitigation: Phase 1 is the place to catch these; subsequent phases inherit a working build.

### 4.6 Dependencies

None. This is the foundation phase.

### 4.7 Parallel opportunities

- DTB parser + UART driver can be built in parallel.
- Allocator + MMU can be built in parallel after early boot lands.
- Hardening flags can be enabled incrementally if they cause problems; each is independently toggle-able.

### 4.8 Performance budget contribution

- Boot to UART banner: < 500ms (full budget — no later phase reduces this).
- `kmalloc(small)` p99: < 50ns (uncontested).
- Allocator scaling: linear with core count for refill operations (per-CPU magazines mean uncontested fast path).

---

## 5. Phase 2: Process model + scheduler + handles

**Goal**: the kernel can create, schedule, and destroy processes. `rfork`, `exec`, `exits`, `wait` work. Multiple processes run concurrently with EEVDF preemption. Per-CPU SMP scheduling on 4 vCPUs. Handle table + BURROW manager production-ready.

### 5.1 Deliverables

**Process and thread**:
- `kernel/proc.c`: `Proc` struct. `rfork(flags)` creates new processes/threads per `ARCHITECTURE.md §7.4`. `exits()` terminates. `wait()` reaps children.
- `kernel/thread.c`: `Thread` struct, per-thread kernel stack, `errstr` buffer, `TPIDR_EL0` save/restore.
- `arch/arm64/context.c`: `savectx()`, `restorectx()`, `swtch()`. Full ARM64 GPR + PC + SP + PSTATE + TPIDR_EL0 save/restore.
- `arch/arm64/elf.c`: ELF loader. Validates ARM64 + EL0 + PT_LOAD; rejects RWX segments per W^X invariant.

**Scheduler (EEVDF)**:
- `kernel/sched.c`: per-CPU run queues (one per band per CPU). `sched()`, `ready()`, `sleep()`, `wakeup()`, `rendezvous()` Plan 9 idiom layer.
- `kernel/eevdf.c`: virtual eligible time + virtual deadline computation; band-aware insertion; pick-earliest-deadline.
- `kernel/run_tree.c`: red-black tree implementation for run trees.
- `kernel/work_steal.c`: cross-CPU work-stealing logic.
- `arch/arm64/timer.c`: ARM generic timer driver; tickless idle.
- `arch/arm64/ipi.c`: GIC SGI-based IPI infrastructure; IPI types (`IPI_RESCHED`, `IPI_TLB_FLUSH`, `IPI_HALT`, `IPI_GENERIC`).

**Territory**:
- `kernel/territory.c`: `Territory`, `bind`, `mount` (stub: only ramfs mountable at this phase), `unmount`. Territory cloned on `rfork(RFPROC)`; shared on `rfork(RFPROC | RFNAMEG)`.

**Handle table + BURROW** (full per `ARCHITECTURE.md §18-§19`, all 8 KObj types):
- `kernel/handle.c`: handle table per-process. Allocation, rights checking, transfer-via-9P (placeholder; 9P client comes Phase 4).
- `kernel/burrow.c`: BURROW manager. `burrow_create()`, `burrow_create_physical()`, `mmap_handle()`, `irq_wait()` (placeholder for Phase 3 IRQ delivery).
- Handle types declared and enforced: `Process`, `Thread`, `BURROW`, `MMIO`, `IRQ`, `DMA`, `Spoor`, `Interrupt`. Hardware handles non-transferable by typed switch (per `ARCHITECTURE.md §18.3`).

**Memory**:
- `mm/vm.c` (extend): user address space management. `mmap` / `munmap` / `mprotect`. Page fault handler: allocate-on-demand for anonymous VMOs.
- `arch/arm64/fault.c`: page fault handler. Demand-pages anon mappings; COW on first write to shared page; BURROW-backed faults route to BURROW page lookup.

**Pipes and notes**:
- `kernel/pipe.c`: kernel pipe implementation. `pipe(fd[2])` syscall. Blocking read/write with a ring buffer.
- `kernel/notes.c`: note delivery, mask, handler invocation.

**Init**:
- `init/joey-minimal.c`: the first userspace process at this phase. Starts a UART debug shell (busybox-equivalent, statically linked).

**Specs**:
- `specs/scheduler.tla`: EEVDF correctness, IPI ordering, wakeup atomicity, work-stealing fairness.
- `specs/territory.tla`: bind/mount semantics, cycle-freedom, isolation.
- `specs/handles.tla`: rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability.

### 5.2 Exit criteria

- [ ] Two processes run concurrently on a single CPU; timer preemption works.
- [ ] Four processes run concurrently on 4 vCPUs; EEVDF latency bound holds (max latency ≤ slice_size × N).
- [ ] `rfork(RFPROC)` + `exits()` + `wait()` lifecycle works without leak (1000-iteration stress test).
- [ ] `exec()` loads and runs a static ELF from ramfs; rejects RWX segments.
- [ ] init starts a UART shell; `echo hello` works via pipe.
- [ ] Page fault handler lazily installs PTEs for anonymous mappings. *(Reconciled 2026-05-28: anon memory is eagerly backed at v1.0; demand-zero paging is a convergence-detour BUILD item and user-stack auto-grow a SEAM — IDENTITY-DESIGN.md §8.4.)*
- [ ] **Handle table**: 10,000 handles open/close cycle without leak; rights reduction enforced; hardware-handle transfer attempt panics cleanly.
- [ ] **BURROW**: create, map, write, read, unmap, close cycle correct; pages freed on last-handle-close + last-mapping-unmap.
- [ ] Stress: 1000 `rfork`/`exits`/`wait` cycles across 4 CPUs without leak or panic.
- [ ] Wakeup atomicity: 1000 producer/consumer pairs across 4 CPUs in tight loop; no missed wakeups (verified by counter).
- [ ] Work-stealing: 4-CPU test with imbalanced load; load redistributes within 5ms.
- [ ] **TSan clean** on the SMP test suite.
- [ ] `specs/scheduler.tla`, `specs/territory.tla`, `specs/handles.tla` clean under TLC.
- [ ] `SPEC-TO-CODE.md` for all three specs maintained.
- [ ] No P0/P1 audit findings.

### 5.3 Specs landing this phase

- `specs/scheduler.tla` (mandatory)
- `specs/territory.tla` (mandatory)
- `specs/handles.tla` (mandatory)

`SPEC-TO-CODE.md` mappings produced and CI-verified.

### 5.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| Scheduler | `kernel/sched.c`, `kernel/eevdf.c`, `kernel/run_tree.c`, `arch/arm64/context.c`, `arch/arm64/ipi.c` | EEVDF correctness, SMP, wakeup |
| Process | `kernel/proc.c`, `kernel/thread.c` | Lifecycle, rfork semantics |
| Territory | `kernel/territory.c` | Cycle-freedom, isolation |
| Handle table | `kernel/handle.c` | Rights, transfer, type discipline |
| BURROW | `kernel/burrow.c`, `mm/burrow_pages.c` | Refcount, mapping lifecycle |
| Page fault | `arch/arm64/fault.c`, `mm/vm.c` | Lifetime, COW |
| ELF loader | `arch/arm64/elf.c` | RWX rejection |
| `mprotect` | `mm/vm.c` mprotect handler | W^X enforcement at runtime |

### 5.5 Risks

- **Scheduler correctness**: race conditions in `sched()` / `wakeup()` are subtle. Mitigation: TLA+ spec before coding; TSan from day one; single-CPU first, then 4-CPU.
- **EEVDF tuning**: `slice_size` parameter tuning may need empirical work. Mitigation: parameterize; sweep at Phase 8.
- **BURROW lifecycle correctness**: handle ref + mapping ref interaction is subtle. Mitigation: `specs/burrow.tla` written here even though it's a Phase 3 deliverable (early scaffolding).
- **TLS corruption**: forgetting `TPIDR_EL0` save/restore breaks musl's TLS. Mitigation: tested early via a TLS-using userspace program.
- **ELF loader edge cases**: static ARM64 ELF with `PT_LOAD` segments. Keep simple at Phase 2 (no dynamic linker, no interpreter — Phase 5 work).

### 5.6 Dependencies

- Phase 1 (kernel skeleton, MMU, allocator, IRQ infrastructure).

### 5.7 Parallel opportunities

- Process / thread + scheduler can be built in parallel.
- Handle table + BURROW can be built in parallel.
- Territory work parallelizes with all of the above.
- Specs can be written before, during, or alongside implementation; they always land first chronologically.

### 5.8 Performance budget contribution

- Syscall, no-contention (e.g., `getpid`): p99 < 1µs (VISION §4.5 budget).
- Process creation (`rfork(RFPROC) + exec`): p99 < 1ms.
- IPC via pipe: ~5 µs round-trip (rough; tighten in Phase 8).
- Wakeup latency on a single CPU: < 1µs uncontested.

---

## 6. Phase 3: Device model + userspace drivers

**Goal**: the Dev vtable is implemented. VirtIO transport core in kernel. **Userspace virtio-blk, virtio-net, virtio-input, virtio-gpu drivers all working** — no in-kernel VirtIO device drivers. Disk I/O, network frame send/receive, keyboard/mouse, framebuffer all functional via userspace.

### 6.1 Deliverables

**Dev vtable + Spoor**:
- `kernel/dev.c`: Dev vtable infrastructure. `bestiary[]` registration. Spoor lifecycle (`alloc`, `free`, `walk`, `clunk`).
- `kernel/spoor.c`: Spoor operations. Reference counting; per-Spoor locking.

**Kernel-internal Devs**:
- `dev/cons.c`: console device (`/dev/cons`, `/dev/consctl`). Wires UART to Dev interface.
- `dev/null.c`, `dev/zero.c`: trivial.
- `dev/random.c` (as-built `kernel/random.c`): ARM64 `RNDR` baseline. *(Reconciled 2026-05-28: the chacha20 stir + reseed is a convergence-detour SEAM over the RNDR baseline; also harden the no-`FEAT_RNG` fallback — IDENTITY-DESIGN.md §8.4.)*
- `dev/proc.c`: `/proc/<pid>/` — synthetic Dev exposing process state (status, cmdline, fd list, mem).
- `dev/ctl.c`: `/ctl/` — kernel admin synthetic Dev. Exposes scheduler stats, IRQ counters, territory dump.
- `dev/ramfs.c`: in-memory filesystem. cpio-loaded at boot; freed once persistent FS is mounted.

**VirtIO core (in-kernel transport)**:
- `kernel/virtio.c`: VirtIO core. Split virtqueue. MMIO transport. PCI transport (minimal, for VirtIO-GPU).
- `kernel/virtio_pci.c`: minimal PCIe enumeration for VirtIO-PCI devices.
- `kernel/irqfwd.c`: IRQ forwarding. Hardware IRQ → `KObj_IRQ` handle's blocker wakeup.

**Userspace drivers** (Rust):
- `drivers/virtio-blk/`: VirtIO block driver. Holds `KObj_MMIO` for the BAR, `KObj_IRQ` for the interrupt, `KObj_DMA` for descriptor + data buffers. Exposes 9P at `/dev/virtio-blk0/`.
- `drivers/virtio-net/`: VirtIO net driver. Exposes raw Ethernet frames at `/dev/ether0`. (Network stack is Phase 7.)
- `drivers/virtio-input/`: VirtIO input driver. Keyboard + mouse events. Exposes 9P at `/dev/virtio-input/`.
- `drivers/virtio-gpu/`: VirtIO-GPU 2D framebuffer driver. Exposes 9P at `/dev/fb/` (`ctl`, `image`, `info`). (3D / virgl is post-v1.0.)

**Driver supervision**:
- `init/driver-supervisor.c`: runs as init child; watches for driver process exit; restarts on crash. Hooks into `/ctl/proc-events/exit` notifications.

**Specs**:
- `specs/burrow.tla`: refcount + mapping lifecycle. Mostly written in Phase 2 (as scaffolding); finalized here.

### 6.2 Exit criteria

- [ ] `cat /dev/random` produces non-zero bytes.
- [ ] **Userspace virtio-blk**: read 1 GiB from VirtIO block device successfully; write 1 GiB and read it back, verify bit-exact.
- [ ] **Userspace virtio-net**: send and receive raw Ethernet frames via `/dev/ether0`; checksum verified.
- [ ] **Userspace virtio-input**: keyboard input from VirtIO console reaches user processes via `/dev/cons`.
- [ ] **Userspace virtio-gpu**: write pixels to framebuffer via BURROW handle; visible on QEMU display.
- [ ] Spoor lifecycle: 10,000 open/read/close cycles on `/dev/null` without leak.
- [ ] Dev vtable: all 11 ops dispatch correctly for cons, null, zero, random, proc, ctl, ramfs.
- [x] **Driver crash recovery**: a driver process terminates (clean or non-zero); the kernel releases its handles via `proc_exit` → `kobj_*_unref`; a subsequent driver re-claims the same hardware. *(P4-M: `userspace.driver_crash_recovery` test verifies A→B sequential claim over the SAME virtio-blk hardware; release-path discipline pre-audited at R9/R12-DMA/R13-burrow. Structural-equivalence: at v1.0 single-CPU with no kill-syscall, "kill mid-I/O" ≡ "exit non-zero" — both call `proc_exit`. Auto-restart supervision policy is Phase 5+.)*
- [ ] **Hardware handle non-transferability**: attempt to transfer `KObj_MMIO` panics with explicit "non-transferable type" message. Verified by deliberate test.
- [ ] IRQ-to-userspace handler latency p99 < 5µs (VISION §4.5 budget). Measured via dedicated benchmark.
- [ ] `specs/burrow.tla` clean under TLC. `SPEC-TO-CODE.md` maintained.
- [x] No P0/P1 audit findings on driver model. *(P4-Z: R14 cumulative driver-model audit across 4 composed userspace drivers (virtio-blk-rw, virtio-net-loop, virtio-input, virtio-gpu) + libthyla-rs surfaced 0 P0 + 1 P1 + 3 P2 + 6 P3; F217 (P1) — missing VIRTIO 1.2 §2.7.13.2 LoadLoad barrier — closed via `libthyla_rs::virtio_rmb()` (`dmb ishld`) uniformly applied; F218-F220 (P2) + 6 P3 closed in the same audit-close commit; 2 cosmetic P3 deferred. Closed list at `memory/audit_r14_p4z_closed_list.md`; phase4-status.md row carries the full narrative.)*

### 6.3 Specs landing this phase

- `specs/burrow.tla` (mandatory; scaffolded in Phase 2, finalized here).

### 6.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| VirtIO core | `kernel/virtio.c`, `kernel/virtio_pci.c` | Transport correctness |
| IRQ forwarding | `kernel/irqfwd.c` | Userspace IRQ delivery |
| BURROW finalized | `kernel/burrow.c`, `mm/burrow_pages.c` | Refcount, mapping lifecycle |
| Userspace driver protocol | (driver Rust code, audit-target since v1.0) | Hardware access correctness |

### 6.5 Risks

- **PCIe enumeration**: `virt` has a PCIe root complex for VirtIO-GPU. Minimal PCIe support needed. Mitigation: keep enumeration minimal; one root complex; linear BAR allocation.
- **IRQ sharing**: VirtIO devices share IRQ lines in some QEMU configurations. Mitigation: GIC shared IRQ dispatch tested.
- **IRQ latency**: 5µs p99 to userspace is the architecture's claim; if missed, the project's driver model is in question. Mitigation: benchmark early in Phase 3; if missing, optimize the IRQ-to-handle wakeup path before Phase 4.
- **Driver process supervision**: getting clean crash recovery right requires careful ordering. Mitigation: write the supervision logic with a TLA+ sketch first.
- **BURROW lifetime under driver crash**: when a driver crashes holding BURROW handles, the kernel must clean up correctly without leaving stale mappings. Mitigation: `specs/burrow.tla` covers this.

### 6.6 Dependencies

- Phase 2 (handle table, BURROW manager, IPI infrastructure, scheduler).

### 6.7 Parallel opportunities

- VirtIO core + drivers can be developed in parallel after the core's interface is stable.
- Each driver class (blk / net / input / gpu) is independent; multiple developers / sessions can work in parallel.
- `dev/proc.c` and `dev/ctl.c` are independent of VirtIO work.

### 6.8 Performance budget contribution

- IRQ to userspace handler: p99 < 5µs.
- VirtIO block I/O: ≥ 80% of QEMU-reported device throughput (target tightened at Phase 8).
- Network frame round-trip via `/dev/ether0`: < 50µs (rough; full network stack is Phase 7).

---

## 7. Phase 5 (= header label "Phase 4"): 9P client + Stratum integration

> **Phase-numbering note**: this section's header label is stale by one — see §2.1. The chunk-tracking name and the status-doc filename are **Phase 5** (`docs/phase5-status.md`). This section's content is the binding plan; the renumber is a future doc-only churn pass.

**Goal**: Thylacine's kernel speaks 9P2000.L + Stratum extensions; mounts Stratum at `/`; `stratumd` runs as a userspace driver-as-9P-server within Thylacine's process model; the `.key` sidecar is unwrapped via janus during boot; Stratum's `/ctl/` admin synthetic FS is consumed by Thylacine userspace; the system boots from a real Stratum pool. Pipelined 9P client achieves the latency + throughput budgets per VISION §4.5.

**Stratum v2 status**: feature-complete and shipping (2026 Q2). The 9P2000.L wire surface + Stratum extensions + libstratum-9p ABIs are stable per `stratum/v2/docs/OS-INTEGRATION.md`. **No external delivery dependency** — Phase 5 entry consumes a known-stable integration target.

### 7.1 The integration model

Thylacine binds to Stratum the same way Linux v9fs does, with adjustments for Thylacine's userspace-driver-everything posture:

- **`stratumd` is a userspace daemon under Thylacine's process model**. One process per Stratum pool. Bound to a Unix socket (Thylacine's equivalent: a 9P endpoint reachable inside the territory). Started early in the boot sequence as one of the first non-kernel procs; survives root pivot.
- **Two sockets per stratumd**: the FS socket (Thylacine's kernel 9P client mounts this at `/`) and the `/ctl/` socket (admin / observability synthetic FS; mounted at `/srv/stratum-ctl/` by userspace tools).
- **`.key` sidecar is a separate-factor security boundary**. Stored separately from the pool block device — initramfs unwraps it before `stratumd` consumes it. Key-delivery options per `stratum/v2/docs/OS-INTEGRATION.md §5`: passphrase + Argon2id, TPM-sealed, hardware token, network keyserver, Shamir split. Thylacine v1.0 ships passphrase + Argon2id as the default; janus handles the unwrap.
- **`stratum` unified CLI** lives in `/sbin/stratum` (carried in the ramfs initially, lives on Stratum-mounted `/sbin/` post-pivot).

### 7.2 Deliverables

**9P client stack (kernel)** — `kernel/9p_*`:

The Phase 5 9P stack lands as a sequence of bounded sub-chunks, each tightly scoped + testable + revertable. The decomposition mirrors the architectural layering described in ARCH §9.6 and §10.

Codec + session + transport + client (the byte-pipe layer):
- `kernel/9p_wire.c` (**P5-wire / -io / -meta / -mutation**): 9P2000.L wire codec — message framing, marshalling, demarshalling per `stratum/v2/docs/reference/20-9p.md`. Covers the 9P2000.L standard surface (handshake + walk + clunk + IO + metadata + mutation). Stratum extensions (`Tsync`, `Treflink`, `Tbind`, `Tunbind`, `Txattrwalk`, `Txattrcreate`, `Tgetxattr`, `Tsetxattr`, `Tlistxattr`, `Tremovexattr`) land in `-stratum-ext` and `-xattr` sub-chunks; lock family in `-lock`. **All landed except `-lock` / `-xattr` / `-stratum-ext`** (deferred low-priority).
- `kernel/9p_session.c` (**P5-session**): per-session state — tag pool (`I-10` per-session tag uniqueness), fid table (`I-11` per-session fid identity stable across open lifetime), outstanding-request table, send/receive dispatch. Pipelined per ARCH §10 (out-of-order completion + flow control). **Landed.**
- `kernel/9p_transport.c` + `kernel/9p_transport_loopback.c` (**P5-transport**): frame-aware byte pipe + vtable-based backend abstraction (initially loopback for testing; future Spoor-over-Unix-socket implements the same vtable). State machine INIT → OPEN → CLOSED + ERROR sink. Partial-read aggregation. Session composition helper (`p9_transport_exchange`). **Landed.**
- `kernel/9p_client.c` (**P5-client**): high-level wrapper consolidating codec + session + transport into 25 op functions (lifecycle / handshake / walk / clunk / IO / metadata / mutation). Signed-errno return convention (0 / -EINVAL / -EBUSY / -EIO / -ecode). **Landed.**

Attach + mount integration (per ARCH §9.6 "filesystem-as-Spoor" design):

The Plan 9 `mount(fd, afid, mountpoint, flags, spec)` syscall is decomposed in Thylacine into two small primitives that compose. The decomposition keeps the kernel out of transport setup, makes `dev9p`-backed Spoors first-class kernel objects, and lets the Linux-compat layer live purely in userspace as a libc shim. See ARCH §9.6 for the full rationale.

- `kernel/dev9p.c` (**P5-attach-dev**): the `dev9p` Dev vtable — every kernel Dev op (walk / open / read / write / clunk / stat / etc.) routes through a `p9_client` instance. Spoor private state carries `(p9_client *, fid)`. Tested kernel-internal against the loopback transport: spawn a client, wrap it in a dev9p Spoor, exercise the full Dev surface, verify the calls land on the right p9_client functions.
- `kernel/9p_attach.c` (**P5-attach-create**): the kernel-internal body of the future `attach_9p` syscall. `p9_attached_create(transport_ops, recv_cap, root_fid, msize, uname, aname, n_uname) → struct p9_attached *` heap-allocates the `p9_client` (~12 KiB) + recv_buf, drives `Tversion + Tattach`, returns a wrapper struct owning the lifecycle. `p9_attached_root_spoor(a)` produces a `dev9p`-backed Spoor at the bound root. Used by tests (loopback transport) + the future P5-stratumd boot path (kernel-internal mount before fd syscalls exist).
- `kernel/sys_attach_9p.c` (**P5-attach-syscall**, deferred): the user-visible `attach_9p(transport_fd, aname, n_uname) → spoor_fd` SVC handler. Requires fd-syscall infrastructure (open/close/read/write/dup + KOBJ_SPOOR populated from userspace) — deferred to a P5-fd-syscalls or P6 chunk that builds that surface. The kernel-internal `p9_attached_*` machinery (above) is what the handler will call once the fd surface exists; the design + scripture commitment in ARCH §11.2 is forward-looking until then.
- `kernel/sys_mount.c` (**P5-attach-mount**): the `mount(source_spoor_fd, target_path, flags) → 0` syscall + Territory mount table + `specs/namespace.tla` extension for mount-lifecycle invariants. Source Spoor can be any Spoor — `dev9p`-backed from attach_9p, kernel-Dev-backed (e.g., devramfs root), or a sub-tree. Mount table refcounting + rfork sharing + Territory destruction release per ARCH §9.6.6.

Stratum + janus + init lifecycle (the boot path that uses the above):

- `kernel/9p_attach.c` (LEGACY NAME; deprecated by §9.6 decomposition. The boot-side integration that drives initramfs → attach_9p → mount is owned by `init/joey.c` + `init/stratumd-launch.c`.)
- Per-process / per-Territory connection management: a Territory holds the mount table; mounts hold Spoor refs to attach_9p results; refcounting handles cleanup.

**`stratumd` lifecycle** (`init/joey.c` + new helpers):
1. ramfs holds `stratumd`, `janus`, `init` binaries + the wrapped `.key` sidecar.
2. init prompts for passphrase (UART at Phase 5; Halcyon prompt at Phase 9).
3. init invokes `janus unwrap < /run/stratum.key.wrapped > /run/stratum.key` (Argon2id-derived; `mlock`'d).
4. init forks `stratumd --pool /dev/vblk0 --key /run/stratum.key --fs-listen /run/stratum/fs.sock --ctl-listen /run/stratum/ctl.sock`.
5. init waits until `/run/stratum/fs.sock` binds (readiness signal per OS-INTEGRATION.md §4).
6. init mounts the FS socket at `/sysroot` over 9P2000.L (`mount -t 9p -o trans=unix,version=9p2000.L,uname=root,access=user,msize=8388608 /run/stratum/fs.sock /sysroot`).
7. init pivots root into `/sysroot`. Ramfs detached, freed.
8. Inside the new namespace, `/run/stratum/{fs,ctl}.sock` are accessible (the post-pivot `/run/` is the same backing store; the socket fds are preserved). `stratumd` survives the pivot.
9. Post-pivot init mounts `/srv/stratum-ctl/` from the `/ctl/` socket so userspace tools can read pool/dataset/snapshot state.

**janus key agent** (Stratum's binary, runs unchanged):
- Runs as another userspace 9P server. Spoor at `/srv/janus/`.
- Wraps the typed passphrase through Argon2id (or whatever backend is configured).
- Authentication backends portable across Linux/macOS/Thylacine: passphrase, TPM 2.0, YubiKey, PKCS#11.

**Boot-failure paths** (must surface clearly):
- `STM_ECORRUPT` (Merkle mismatch) → refuse to boot, drop to UART recovery shell.
- `STM_EBADTAG` (AEAD MAC failure on superblock read) → refuse to boot.
- `STM_EBADKEY` (wrong `.key`) → prompt for re-unlock.
- `STM_EWEDGED` (pool marked wedged at prior unmount) → refuse to boot, drop to UART recovery shell with `stratum fs verify` available.

**Specs**:
- `specs/9p_client.tla`: tag uniqueness (`I-10`), fid lifecycle (`I-11`), out-of-order completion, flow control. **Spec-first per CLAUDE.md** — written before `kernel/9p_*.c` lands.

### 7.2a Corvus arc (key agent, hostowner, login — per `docs/CORVUS-DESIGN.md`)

CORVUS-DESIGN.md (landed at P5-corvus-design) is the binding scripture for the post-pivot stack: corvus key agent, per-user stratumd processes, login manager, hostowner model, kernel-update flow. The implementation chunks from CORVUS-DESIGN §10:

- **P5-corvus-design** (this doc + ARCH/ROADMAP cross-refs; no code). **Landed.**
- **P5-stratumd-multi** (Stratum-side; Michal-owned): multi-stratumd-per-pool support — multiple stratumd processes share one pool's block device, each restricted to specific datasets. Per CORVUS-DESIGN §13.1. Required before P5-login can spawn per-user stratumd processes.
- **P5-corvus-syscalls** (Thylacine-side): land `sys_mlockall`, `sys_set_dumpable`, `sys_set_traceable`, `sys_explicit_bzero`, `sys_getrandom` in the kernel + libt stubs + tests. Per ARCH §11.2b + CORVUS-DESIGN §4.1.1. **Critical-path foundation** — corvus + per-user stratumd both depend on these.
- **P5-stratumd-bringup**: joey forks stratumd-system against the system pool with file-backed key + pool-serial verification (C-14); kernel-side 9P mount of /sysroot; pivot root; explicit_bzero the in-memory key.
- **P5-corvus-bringup**: implement `/sbin/corvus` from scratch (NOT a port; Thylacine-native using Stratum's libstratum-crypto primitives). State file format with magic `CRVS`. Binary frame wire codec. All v1.0 verbs. Encrypted audit log. Per CORVUS-DESIGN §4 + §6.
- **P5-corvus-srv** (macro-chunk; sub-chunks design / tsleep / impl-a / impl-b): the `/srv/corvus/` transport (CORVUS-DESIGN §6). corvus becomes a full 9P2000.L server reached **per-connection**, every connection carrying a kernel-stamped, unforgeable peer identity (`stripes` 64-bit tag, console bit, caps) read via `SYS_SRV_PEER`. The r5 design converged across four adversarial design-audit rounds. Decomposition:
  - **P5-corvus-srv-design** *(scripture + specs; no code)*: r5 into CORVUS-DESIGN §6 + invariants C-22/C-23, and the two TLA+ specs (`corvus.tla` connection layer, `handles.tla` `KObj_Srv` partition), TLC-verified clean + buggy. Not audit-bearing.
  - **P5-tsleep**: the deadline-bounded `Rendez` sleep primitive (`tsleep`, ARCH §8.8) — a small kernel prerequisite so a hung corvus cannot wedge its clients; also owed for the Phase-5 `poll` / `futex` work.
  - **P5-corvus-srv-impl-a**: the kernel side — the `devsrv` Dev + `/srv`, the service registry, `SYS_POST_SERVICE` / `SYS_SRV_ACCEPT` / `SYS_SRV_PEER`, per-connection setup, the `KObj_Srv` kobj kind, `stripes`. Audit-bearing.
  - **P5-poll** *(general Phase-5 primitive — not a P5-corvus-srv sub-chunk; lands spec / -a / -b)*: the multi-fd wait syscall (`SYS_POLL`; ARCH §23.3). Owed for Utopia broadly (bash, musl), and the prerequisite for impl-b — corvus is single-threaded and `poll`s its listener plus its accepted connections. Spec-first (`poll.tla`, gate-tied spec #6); audit-bearing.
  - **P5-corvus-srv-impl-b**: the corvus side — corvus becomes a full 9P2000.L server over `/srv/corvus`, retiring the joey pipe harness. Depends on P5-poll. Audit-bearing.
  Prerequisite for P5-login and P5-hostowner — corvus-bringup left corvus on a joey-driven pipe harness; login and hostowner both gate on the peer Proc.
- **P5-login**: `/sbin/login` — console-based login that drives corvus + spawns per-user stratumd processes + binds user dataset Spoor into user's territory.
- **P5-hostowner** (sub-chunks a/b/c): -a *(landed)* — `CAP_HOSTOWNER` (elevation-only) + the console-attachment kernel bit; -b — the kernel `cap` device + `CAP_GRANT_HOSTOWNER` + the `rfork` elevation-only strip + ADMIN_ELEVATE + admin-verb gating; -c — the RECOVER verb (paper recovery phrase). Per CORVUS-DESIGN §3 D5 + §5.5/§5.5.1 + §5.6.
- **P5-kernel-update**: `thyla-pkg kernel-update` script + 5-min ok-marker discipline + two-kernel rotation on ESP. Per CORVUS-DESIGN §7.

The previously-listed `kernel/sys_attach_9p.c` (P5-attach-syscall) **landed at `ae0746e`** (handled SVC dispatch + dev9p_priv extension); CORVUS-DESIGN.md supersedes the speculative parts of the earlier §7.2 about janus running unchanged (corvus replaces janus for v1.0 Thylacine; design influence retained).

### 7.3 Exit criteria

Substrate + integration:
- [ ] `specs/9p_client.tla` clean under TLC. At least 4 buggy `.cfg` variants exist demonstrating I-10 / I-11 / out-of-order / flow-control violations under buggy assumptions.
- [ ] `stratumd` boots from initramfs against a real Stratum pool on virtio-blk.
- [ ] Kernel mounts the FS socket at `/`. `ls /`, `cat /etc/hostname`, `mkdir /tmp/test`, `echo hello > /tmp/test/h && cat /tmp/test/h` all work via Stratum end-to-end.
- [ ] janus successfully unwraps a passphrase-protected pool key.
- [ ] **Reboot test**: data written before reboot is present after reboot. Stratum's three-phase sync + Merkle-rooted metadata verifies at-boot. No data loss across abrupt power cycle.
- [ ] **Pull-the-plug test** (per OS-INTEGRATION.md §19): kill `stratumd` with SIGKILL mid-write; verify next mount succeeds and state is consistent post-recovery.
- [ ] 9P session: 10,000 open/read/write/close cycles on a Stratum file without protocol error or leak.
- [ ] **Pipelined throughput**: a process issuing 32 concurrent reads on a Stratum file achieves throughput ≥ 90% of the session's bandwidth limit (vs the ~3% naive serialized 9P would deliver at typical RTT).
- [ ] **9P round-trip latency** (loopback Stratum): p99 < 500µs (VISION §4.5).
- [ ] Per-process 9P connections: 100 processes each with their own connection to Stratum work without leaks; total fid count + kernel memory tracked.
- [ ] Stratum extensions verified end-to-end: `Tbind` composes a subvolume into a connection's territory; `Tsync` returns after the dirty buffer drains; `Treflink` produces a shared-block clone with `STAT_NLINK` accounting correct; xattr family round-trips a `security.selinux` value bit-exact.
- [ ] No P0/P1 audit findings on the 9P client (R-series prosecutor pass scoped to `kernel/9p_*.c`).

Admin + observability:
- [ ] `/srv/stratum-ctl/` mounted; `cat /srv/stratum-ctl/version` reports the running stratumd build.
- [ ] `echo start > /srv/stratum-ctl/pools/<uuid>/scrub-trigger` initiates a scrub; `cat /srv/stratum-ctl/pools/<uuid>/scrub` reports progress.
- [ ] **Atomic-snapshot upgrade smoke test** (per OS-INTEGRATION.md §8): take a snapshot via `/srv/stratum-ctl/datasets/<root-id>/create-snapshot`; intentionally write garbage to a tracked file; rollback via `/srv/stratum-ctl/datasets/<root-id>/rollback-snapshot`; verify the file is restored bit-exact.
- [ ] Prometheus exposition reachable at `/srv/stratum-ctl/pools/<uuid>/metrics/prometheus`; scraped values include `stratum_pool_total_bytes`, `stratum_pool_used_bytes`, `stratum_scrub_state`.
- [ ] `/srv/stratum-ctl/events` append-only log surfaces every admin verb fired during the boot.

POSIX coverage on Stratum:
- [ ] All Stratum v2 live POSIX features (per OS-INTEGRATION.md §6: inodes / dirents / xattrs / file seals / advisory locks / statx / name_to_handle_at / copy_file_range / single-dataset reflink / rename family / fallocate family / symlinks / hard links / O_TMPFILE / posix_fadvise / inline-data optimization / snapshots) exercise correctly through Thylacine's 9P client + syscall layer. The bound is "Stratum's v2 POSIX test surface that runs against the Linux v9fs client passes equivalently against Thylacine's kernel 9P client."

### 7.4 Specs landing this phase

- `specs/9p_client.tla` (mandatory; spec-first; **landed at P5-spec** — `ce4fa31`/`f1aadfc`).
- `specs/namespace.tla` extended with mount-lifecycle invariants at **P5-attach-mount** per ARCH §9.6.6: mount table is a finite set of (path, Spoor) pairs; refcount accounting; rfork sharing semantics; Territory destruction release.

### 7.5 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| 9P wire codec | `kernel/9p_wire.c` | Wire-protocol correctness; malformed-message defense; integer overflow on length fields |
| 9P session state | `kernel/9p_session.c` | I-10 tag uniqueness, I-11 fid lifecycle, out-of-order completion, flow control |
| 9P transport | `kernel/9p_transport.c` + backend impls | Frame-aware byte-pipe state machine; partial-read aggregation; backend vtable |
| 9P client wrapper | `kernel/9p_client.c` | High-level API surface; error mapping; struct-copy discipline |
| dev9p Dev vtable | `kernel/dev9p.c` | Proxy from kernel Dev surface to `p9_client` ops; Spoor private state lifecycle |
| Kernel-internal attach machinery | `kernel/9p_attach.c` | Heap-resident `p9_client` + recv_buf lifecycle; handshake; root Spoor production; orderly teardown. The body of the future `attach_9p` syscall. |
| `attach_9p` SVC handler (deferred) | `kernel/sys_attach_9p.c` | User-visible syscall + KOBJ_SPOOR fd-table integration; requires fd syscalls (open/close/read/write) to exist; deferred to a later chunk that builds that surface. |
| `mount` syscall + Territory mount table | `kernel/sys_mount.c` | Mount table representation; refcounting; rfork sharing; namespace integration; spec extension |
| Boot transition | `init/joey.c`, `init/stratumd-launch.c` | Initramfs → pivot → post-pivot ordering; `.key` lifetime; stratumd readiness probe |
| Key unwrap | `init/janus-client.c` or libthyla-rs janus wrapper | `.key` sidecar handling, `mlock` + `MADV_DONTDUMP`, shred-after-consume |

### 7.6 Risks

- **9P client correctness**: fid management, walk chains, clunk on close — these are subtle. Stratum's server is strict (it's been audit-loop-hardened through Stratum's R0..R136+ series). Mitigation: spec first; integrate-with-Stratum testing from day one — `tools/test.sh` boots Thylacine against a real Stratum pool produced by `stratum/v2/tools/stratum/`.
- **Stratum extensions semantics**: `Tsync` / `Treflink` / `Tbind` / `Txattrwalk` are Stratum-specific; their semantics must be exactly aligned with `stratum/v2/docs/reference/20-9p.md`. Mitigation: exhaustive bidirectional integration tests; spec the extensions in `9p_client.tla`.
- **Boot sequencing**: ramfs → Stratum transition is a one-shot operation that must be correct. Mitigation: documented sequence (this section + OS-INTEGRATION.md §4); failure path drops to UART recovery shell with `stratum fs verify` available; the recovery shell is also where `STM_ECORRUPT` / `STM_EBADTAG` / `STM_EBADKEY` / `STM_EWEDGED` get surfaced and resolved.
- **Key lifetime**: the unwrapped key sits in RAM between janus-unwrap and stratumd-consume. Mitigation: `mlock` + `MADV_DONTDUMP` per OS-INTEGRATION.md §5; `explicit_bzero` the ramfs copy after `stratumd` acknowledges consumption.
- **Pipelining bug surface**: out-of-order completion is the load-bearing throughput claim; bugs here are subtle (e.g., a fid clunked on session A is reused on session B before the server confirms; a stale Rread arrives after the fid is recycled). Mitigation: `9p_client.tla` covers it; integration tests stress it.
- **Per-process connection scaling**: 100 procs × 1 connection = 100 sockets + ~100 KB session state. Mitigation: bounded; tracked; v2.x will multiplex if it becomes a constraint (deferred per VISION §11).

### 7.7 Dependencies

- Phase 2 (handle table, BURROW manager — for BURROW transfer over 9P).
- Phase 4 (kernel virtio-blk userspace driver — stratumd reads through it).
- **External (READY)**: Stratum v2 stable ABIs + binaries (`stratumd`, `stratum`, `janus`). All shipping today.

### 7.8 Parallel opportunities

- 9P wire codec implementation + session state can be developed in parallel.
- Stratum extension messages can be added incrementally; the 9P2000.L baseline lands first.
- janus integration is independent of the rest of Phase 5; can land any time after Stratum mounts.
- `specs/9p_client.tla` proceeds in parallel with early `kernel/9p_*.c` skeleton.

### 7.9 Performance budget contribution

- 9P round-trip latency (loopback Stratum, Unix-socket transport): p99 < 500µs at Phase 5 exit; bare-metal target tightened to p99 < 50µs in Phase 8 hardening per VISION §4.5.
- 9P pipelined throughput: ≥ 90% of session bandwidth at 32 concurrent ops.
- Reboot time (Stratum mount + key unwrap + remount): < 5s (contributes to total boot-to-login budget; bare-metal target is < 1s).

### 7.10 What Phase 5 deliberately does NOT do

- **In-kernel Stratum driver bypassing 9P**: designed in `ARCHITECTURE.md §14.4`; implemented post-v1.0. v1.0 mounts via the kernel's 9P client; the discipline is "Thylacine talks to Stratum exactly as any 9P client would."
- **Multi-pool composition**: today, one pool per kernel mount. Multiple pools = multiple `stratumd` instances + multiple mounts at distinct paths (e.g., `/var/data` from one pool, `/home/legacy` from another).
- **Cross-dataset reflink**: gated on Stratum's rekeying primitive (Stratum-upstream roadmap).
- **inotify/fanotify**: Stratum-upstream roadmap; until then, polling is the fallback.
- **OTLP exposition**: Prometheus is the v2.x baseline; OTLP is a sidecar translator if needed.
- **Cross-phase pulls**: P4-Id (virtio-net `/dev/ether0` 9P surface) and virtio-input `/dev/cons` 9P surface were the remaining Phase 4 §6.2 boxes; they're naturally absorbed by Phase 5 once the kernel learns to host a userspace 9P server (the symmetric mechanism to mounting one). Both close as Phase 5 side effects.

---

## 7A. Phase 6: Pouch — POSIX libc + cross-compilation

**Execution Phase 6.** Binding design: **`docs/POUCH-DESIGN.md`** (converged 2026-05-22). This section is the ROADMAP-level summary; POUCH-DESIGN.md is authoritative for scope, architecture, invariants, and sub-chunk detail.

### 7A.1 Why this phase, why here

Thylacine's practicality — its claim to be a real OS, not a toy — depends on the reuse of POSIX/Linux/BSD software. That capability was a single bullet ("musl port") inside the old Phase 6; it is in fact phase-sized and load-bearing for the whole project, so it was extracted, expanded, and made its own phase. It **preempts the Phase 5 close**: Phase 5 (9P client + Stratum integration) is suspended, substantially complete — its remaining tail (real stratumd swap-in, P5-hostowner-c, P5-login) is pouch-dependent and resumes once pouch exists. stratumd is the proving binary; the durable deliverable is the cross-compilation path itself. (Stratum is a Thylacine-native-primary project — written mainly for Thylacine, with Linux/macOS as secondary targets — so making Thylacine a first-class build + run target for it is core, not accommodation.)

### 7A.2 Deliverables

- **pouch** — the Thylacine-native POSIX libc: musl's portable upper half + a Thylacine-native lower half, split at musl's syscall seam. POSIX lives entirely in pouch's userspace; the kernel gains no foreign syscall number (invariant P-1).
- The **`aarch64-thylacine` cross-toolchain** + **sysroot** — `tools/build.sh sysroot` becomes real; usable from CMake and plain Makefiles.
- The **POSIX runtime layer** — `AF_UNIX`→`/srv`, `poll(2)`→`t_poll`, `sigaction`→notes, pthreads→Thylacine threads with the **`torpor`** wait-on-address primitive.
- The **synthetic-filesystem namespace** — the minimal Unix `/dev` set + the convention by which POSIX path-based APIs resolve to Thylacine Devs (POUCH-DESIGN.md §6.6).
- **Proving artifacts** — a static C "hello", libsodium cross-compiled, **real stratumd built + booted**, the Phase-5 stub retired.

### 7A.3 Sub-chunk decomposition

16 sub-chunks — see POUCH-DESIGN.md §14. Critical path: the `pouch-wait-addr` sub-chunk (the `torpor` primitive, spec-first against `futex.tla`) and `pouch-threads`. `pouch-compiler-rt` (#6 — the compiler runtime + the `pouch-ld` link wrapper) was inserted after `pouch-hello-smoke` surfaced the gap.

### 7A.4 Exit criteria

Full list in POUCH-DESIGN.md §13. Headline: the cross-toolchain builds; a static + a printf hello run in Thylacine; a multithreaded test passes under TSan; an `AF_UNIX` echo pair round-trips over `/srv`; libsodium's self-test passes; stratumd boots, serves 9P, the stub is retired; joey completes the ramfs→Stratum pivot. The stratumd criteria **re-open and satisfy the bulk of Phase 5's own exit criteria** — this phase is what unblocks the Phase 5 close.

### 7A.5 Specs

- `futex.tla` (gate-tied spec #7) — was planned for `torpor`; landed without a spec module per the 2026-05-23 spec-to-code broadening (prose + audit + tests).
- `notes.tla` (#8) — was planned for the kernel notes substrate (sub-chunk 13a); also landed without a spec module per the same broadening. Design + invariants in ARCH §7.6.1-§7.6.8 + NOVEL.md §3.1 (the fd-first inversion is the project's novel contribution vs. Plan 9 + Linux).
- `poll.tla` (#6), `pty.tla` (#9) — pouch's poll / (later) PTY layers are consumers; those kernel mechanisms are pre-existing or pre-planned.

### 7A.6 Audit-trigger surfaces

pouch's lower half (the syscall seam, the socket/thread/signal translation) + the kernel additions (auxv population, the `torpor` syscall, the allocator-backend call). Enumerated in POUCH-DESIGN.md §14 (the audit-bearing column); mirrored in `ARCHITECTURE.md §25`.

### 7A.7 Dependencies

- **Inbound**: Phase 5's 9P client + handle table + `/srv` + `SYS_POLL` + Thylacine threads — all landed; pouch rests on them.
- **Outbound**: the Phase 5 tail (real stratumd, P5-hostowner-c, P5-login) resumes after pouch. The renamed Phase 7 (Utopia) builds its userland on pouch.
- **Stratum-side**: chunks 13–14 need Stratum changes (the Thylacine `peer_creds` arm). User-owned, coordinated work.

### 7A.8 Naming

`pouch` — the libc (the marsupium; shelters foreign code until it can run native). `torpor` — the wait-on-address primitive (a thread enters torpor on an address until roused). Both signed off 2026-05-22. See POUCH-DESIGN.md §16.

---

## 8. Phase 7: Utopia (textual milestone)

**Execution Phase 7.** Section numbered §8 in ROADMAP per pre-Pouch convention; the project-execution registry per `§2.1` is authoritative. Pouch (execution Phase 6, ROADMAP §7A) closed `218feb0` and delivered the musl port + cross-compilation that this phase originally bundled — those deliverables are now Pouch's, NOT Phase 7's.

**Binding designs**: `docs/UTOPIA.md` (the experience), `docs/UTOPIA-SHELL-DESIGN.md` (the shell + coreutils design), `docs/UTOPIA-VISUAL.md` (Pale Fire palette + glyph + prompt). All three landed under U-1 (the scripture commit). This section is the ROADMAP-level summary; the binding designs are authoritative for scope, architecture, and sub-chunk detail.

**Goal**: a complete textual environment. The Utopia milestone (`VISION.md §13`) — a developer using Thylacine via UART or SSH finds a working textual environment that "feels real, not broken." Halcyon (graphical layer; Phase 10 execution / ROADMAP §11) is **not** required at this phase; Halcyon is deliberately the final phase of v1.0. Phase 7's exit is Utopia. Phase 8 (execution; ROADMAP §9) adds Linux compat + network on top. Phase 9 (execution; ROADMAP §10) hardens + audits + produces v1.0-rc. Phase 10 (execution; ROADMAP §11) lands Halcyon. The "practical working OS" the project commits to is achieved at Phase 9 exit; Halcyon is the additive graphical layer.

### 8.0 The convergence detour (foundation completion; CLOSED 2026-06-05)

The Utopia shell arc is **paused at U-6d-a**. Building the shell's next features
(redirects, coreutils, the identity-aware prompt) surfaced that they sit on an
identity / permission / privilege substrate that did not yet exist; a deliberate
kernel completeness sweep then found a cluster of fundamental arch/roadmap
*misses* (no IOMMU/DMA isolation; no create/clock/FS-mutation syscalls; no resource
floor; no orphan reaper; PAN off; scripture over-claims). Rather than build the
shell on an incomplete foundation, one comprehensive detour brings the system to a
clean boundary line where **it really is what it says it is**.

**Binding design + the full ordered work list + every finding's disposition
(BUILD / SEAM / DOC): `docs/IDENTITY-DESIGN.md §8`** (promoted to binding scripture
2026-05-28). Governing bar: *build to the extent of the foreseeable future + design
every seam so extension is additive*; the detour closes when every
VISION/ARCH/ROADMAP claim is true-because-built or honestly-scoped-with-its-reason
(NOT "build everything maximally" — that would add unverified complexity).

The one ordered arc — not split, not deferred to v1.x:
- **A — identity / access / privilege.** The hybrid identity + groups; the
  legate/clearance scoped-elevation model (no superuser identity, **I-22**);
  `SYS_WALK_CREATE` + ownership-on-create; the rwx surface; `CAP_KILL`; the trusted
  path; the uniform mount-cape. Absorbs the suspended Phase 5 tail (P5-login,
  per-user stratumd wiring [Stratum A2 delivered], P5-hostowner-c, the corvus Q11
  4-byte seam).
- **B — DMA isolation (ARM SMMUv3), in v1.0.** Makes the userspace-driver trust
  model safe (kernel currently fully trusts every driver); the DMA API hands out
  device-IOVAs, not raw PAs. The F-7 dev-mode flag is dropped (subsumed).
- **C — kernel completeness-sweep dispositions** (2026-05-28 sweep; kernel CORE
  verified sound). BUILD: syscall-surface completion (clock + FS-mutation +
  `fsync` durability + readdir), real exit-status, demand-zero paging, PAN, PAC
  real entropy, the resource/DoS floor, the orphan reaper, cheap speculation
  detection. SEAM: COW, ACLs, resource quotas, HW-cap allowlist, hardware-backed
  keys, verifiable-credential identity, KPTI, stack auto-grow, RNG stir. DOC: the
  scripture over-claim reconciliations (MTE/CFI, demand-paging/COW, RNG stir, the
  `ipi.c` drift) — landed 2026-05-28.

**2nd-order detour (2026-05-29): capability-scoped service storage (A-1.7),
pulled before A-1b.** While pinning A-1b (corvus identity-DB persistence), the
question "where does a service's persistent storage live, and with what
authority?" surfaced a foundational substrate worth building once, before its
first consumer: a service is *handed* a storage-root capability (a `KObj_Spoor`)
at spawn rather than naming an ambient path, and its FS authority is bounded by
that capability (**I-23**; ARCH §3.6; NOVEL.md §3.10 lead angle #10). corvus is
the canonical first subject (a secret-holding daemon that should be confined).
Per the depth-first discipline, A-1b is preempted at a clean boundary and
**resumes immediately after A-1.7 closes**, building corvus's identity DB + key
wraps inside the handed storage capability. The substrate needs zero new kernel
surface (verified: `handle_dup` rights reduction, spawn fd-endowment,
`walk_*`/`fsync`-from-a-base-Spoor, `SYS_MOUNT` all exist). Full work-list +
resume pointer: `docs/detour-status.md` A-1.7.

**Namespace spine pulled forward (2026-06-02): stalk + namespace-resident `/srv`
(A-5b-0).** A-5b (per-user encrypted home) needs per-user storage coordinators a
second user cannot even *name* — and the global flat `/srv` registry (8 slots,
one poster per name) cannot host two per-user `stratum-fs` coordinators. The
system-right answer (user-voted; system over cost/scope) is to build the
multi-component pathname resolver **stalk** (Plan 9 `namec`) that Thylacine
deferred since Phase 5 — `SYS_WALK_OPEN` is single-component and never crosses a
mount — with namespace-resident `/srv` as its first consumer. This unlocks all
absolute-path resolution (`/srv`, `/proc`, `/net`, `/home/<user>`), not just the
A-5b driver. Design: `docs/STALK-DESIGN.md` (signed off); invariant **I-28**;
sub-chunks stalk-1/2/3, each audited. The A-5b body resumes on top. Resume
pointer: `docs/detour-status.md` A-5b-0.

The convergence detour **closed 2026-06-05** (A-5c complete; the identity /
access / privilege arc is done — `docs/detour-status.md`). Before Phase 7
resumes, **two scheduled arcs** land (user-decided 2026-06-05; see §8.0a).

### 8.0a Two pre-Utopia arcs: Lazarus M1 + Loom (scheduled 2026-06-05)

With the foundation sound, two arcs are scheduled **before** Phase 7 resumes —
both designed scripture-first, no code until each is signed off; **impl order
Lazarus M1 then Loom**. User rationale: Utopia brings userspace apps, and Loom's
fast IO should exist before the apps that consume it.

- **Lazarus M1 — bare-metal portability foundation + QEMU HVF acceleration.**
  Binding design `docs/PORTABILITY.md` (signed off + sequenced 2026-06-05). The
  *same* kernel binary runs QEMU-TCG (CI baseline, unchanged), QEMU-HVF on Apple
  Silicon (the fast dev loop — TCG boots take 16-26 s, HVF is near-native), and
  is ARMv8.0-A-ISA-ready for bare metal. **M1 = W1** (v8.0 ISA floor: inline
  LL/SC atomics — *not* outline-atomics, which no kernel uses in-kernel;
  PAC/BTI/LSE → runtime-conditional, the rest of the hardening unconditional on
  every target) **+ W1.5** (boot-time LSE alternatives-patching — the Linux-style
  patcher restores single-instruction LSE atomics on FEAT_LSE cores at zero
  runtime cost, self-modifying `.text` through a transient RW-not-X alias so
  W^X / I-12 is never violated; **audit-bearing**; amended 2026-06-06,
  PORTABILITY §4.5) **+ W2** (the real GICv2 driver — MMIO CPU interface, no
  `ICC_*` sysregs; the HVF-on-Apple enabler *and* what the Pi's GIC-400 needs;
  **audit-bearing**, scheduler/IPI surface, I-18) **+ W3** (a chacha20
  software-RNG seeded by a kernel virtio-rng driver + DTB `rng-seed` + `cntpct`,
  RNDR stirs when present — the "software-backed RNDR"; **audit-bearing**, CSPRNG
  quality). Fully QEMU-testable (TCG + HVF). **W4** (the actual RPi-400 board
  bring-up: EL2→EL1 drop, BCM mailbox, a new SD/EMMC block backend) stays
  post-v1.0 (§12.1).
- **Loom — the io_uring-inverted Burrow-backed 9P ring transport.** Binding
  design `docs/LOOM.md` (signed off 2026-06-05). Scheduled as the second
  pre-Utopia arc, **pulled forward from its documented post-v1.0 slot** (§12.2)
  because Utopia's native userspace apps consume it. Expose the #841 pipelined 9P
  client to userspace via a shared-memory SQ/CQ ring (the 9P op-set *is* the SQE
  vocabulary — io_uring's "what opcodes?" problem dissolves). Resolved design:
  **native op-descriptor SQE** (dispatched straight to the structured
  `p9_client_*` API) + a *reserved, not-built* wire-passthrough seam; **SQPOLL +
  multishot built up front** (the maximal arc, each its own spec'd + audited
  sub-chunk); the core kernel work is a **pluggable-completion refactor** of the
  #841 client (post-CQE vs wake-rendez — one engine, two front-ends);
  **submit-time capability pin** (the #844 by-value snapshot, never re-checked at
  completion — io_uring's CVE class). **Spec-first re-enabled** — `specs/loom.tla`
  (no-lost / no-double / no-stale completion + ring TOCTOU) gates each impl
  sub-chunk; reserves invariants **I-29** (completion integrity) + **I-30**
  (submit-time pin); audit-bearing. Sub-chunks Loom-0 (this scripture) →
  Loom-1 (model) → Loom-2..6 (impl). The liburing-compat shim stays out of core
  (§12.2). **Loom-5/6 are shaped by their first concrete consumer — Tapestry, the
  graphics fast-path** (`docs/TAPESTRY.md`, signed off 2026-06-07): present =
  `LOOM_OP_WRITE`, input/vsync = a multishot `LOOM_OP_READ`, the framebuffer a
  zero-copy shared Burrow — zero new Loom core. Graphics is the Loom-5/6 benchmark
  workload (a present+input+vsync loop). The graphics *half* (virtio-gpu scanout +
  the `tapestryd` display server + an SDL backend) is a **post-Loom graphics
  phase**, NOVEL #2/#4 territory feeding Halcyon — additive over the textual OS,
  not gating the v1.0-rc.

Phase 7 then resumes at **U-6d-b** (redirects), on a sound + accelerated
foundation.

### 8.1 Deliverables

Per `docs/UTOPIA-SHELL-DESIGN.md §19`. The U-* arc:

**The native runtime substrate — the `libthyla-rs` uplift to the library Thylacine deserves**. Authoritative scripture: `docs/UTOPIA-SHELL-DESIGN.md §15`. The current libthyla-rs (857 lines as of `218feb0`) is materially behind Thylacine's kernel surface; the U-2..U-2-test sub-arc brings it forward as a complete, idiomatic Rust API. Modules:
- `t::err` — `Error` enum + `Result<T>` alias (POSIX-aligned per `docs/ERRORS.md`).
- `t::handle` — `Handle` RAII newtype + `Rights` bitflags.
- `t::alloc` — `#[global_allocator]` backed by `burrow_attach`; enables `alloc::*`.
- `t::fs` + `t::io` — `File`, `Path`, `PathBuf`, `OpenOptions`, `Metadata`, `ReadDir`, `Read`/`Write`/`Seek` traits.
- `t::process` + `t::process::pipe` — `Command`, `Child`, `Stdio`, `pipe()`.
- `t::notes` + `t::poll` — `Notes`/`Note`/`MaskGuard`, `PollSet`/`PollEvents`.
- `t::territory` + `t::cap` — `bind`/`mount`/`pivot_root`/`rfork`, `Caps`.
- `t::thread`, `t::torpor`, `t::time`, `t::rand`, `t::tty` — focused smaller modules.
- `t::ninep` — 9P client (lifted from corvus).
- `t::hardware` — `Mmio`/`Irq`/`Dma` (consolidated from virtio-* drivers).

Every module is idiomatic Rust (RAII, typed errors, builders, lifetimes). Every U-3+ chunk and every future native Thylacine Rust program (Phase 8 daemons, Phase 10 Halcyon, v1.x daemons) builds on this library — lead-by-example.

**The shell — `ut`**:
- Native Rust on `libthyla-rs`. Plan 9-rc-shaped with refinements. NO Pouch, NO musl.
- Twelve resolved design axes: plain text streams; rc-shaped + refinements; implicit-fail + try/catch + `?` + pipefail; rc + double-quote interpolation + `$(cmd)`; `case`/match-block + word operators; modern-middle builtins + Thylacine extensions (bind/mount/cap/note); fd-notes + poll() main loop + `on note`/`mask note`; `fn prompt` + hand-rolled line editor + emacs default; flat-file history; binary name `ut`; workspace under `usr/utopia/` (Helix vendored separately); native libthyla-rs runtime.

**The line editor — `libutopia::line_editor`**:
- Hand-rolled (~1500-2500 LOC). Raw-mode + emacs keybindings + multi-line + history + tab completion + Ctrl-R fuzzy search.
- NOT reedline (reedline assumes std and Pouch; we're native).

**The shared library — `libutopia`**:
- `palette` (Pale Fire constants + role-aware ANSI helpers).
- `ansi` (escape emission, prompt-length tracking).
- `line_editor`, `notes`, `ninep`, `tty`, `prompt`, `path`, `errors` modules.

**The coreutils** (native Rust on libthyla-rs):
- Initial set sized to pass the Utopia bring-up integration test: `cat`, `ls`, `echo`, `grep`, `sed`, `awk`, `cp`, `mv`, `rm`, `mkdir`, `find`, `wc`, plus the shell-builtin tier.
- 9base-shaped feature scope; uutils-coreutils-shaped Linux flag compatibility where helpful.
- One Rust crate per command under `usr/utopia/coreutils/`.

**The default editor — `hx` (Helix)**:
- Ported via Pouch (NOT native libthyla-rs; Helix is full-std Rust + tokio + ratatui + tree-sitter; impractical to no_std-port).
- Vendored at `usr/helix/`.
- Pale Fire theme bundled.

**The kernel-side prerequisites** (most already delivered; remaining ones land in U-* chunks as needed):
- `SYS_POLL` (existing).
- Notes substrate (existing).
- `/proc` synthetic Dev (existing in kernel; minor extensions for ps/jobs).
- PTY infrastructure — `/dev/ptmx` + `/dev/pts/<n>` (Phase 7 work; deliverable here for the editor-via-PTY path).
- `/dev/consctl` termios writes (existing or near-existing).

**The Pale Fire visual identity**:
- Binding scripture in `docs/UTOPIA-VISUAL.md`.
- Implemented in `libutopia::palette` + the `palette` shell builtin.
- Discipline applies to Utopia's own programs; third-party tools colour freely.
- Terminal-config files shipped at `share/terminal-configs/` (Ghostty / Kitty / Alacritty / etc.).

### 8.2 Exit criteria — Utopia ships

Authoritative: `docs/UTOPIA-SHELL-DESIGN.md §18`. Summary:

- [ ] Boot a fresh Thylacine VM; reach the Pale Fire `ut` prompt via UART.
- [ ] Multi-stage shell pipeline: `cat /etc/passwd | grep root | cut -d: -f1` produces correct output.
- [ ] Job control via fd-notes: `sleep 100 &` appears in `jobs`; Ctrl-Z + `fg` resume; Ctrl-C terminates.
- [ ] Error model: function with `cmd1?; cmd2?; cmd3?` short-circuits on `cmd2` failure.
- [ ] Namespace builtin: `bind /srv/stratum-ctl /n/stratum`; `ls /n/stratum` shows the Stratum admin surface.
- [ ] Notes builtin: `note send $$ snare:user1` triggers a registered `on note` handler.
- [ ] `hx /etc/hosts` opens Helix; edit + save observable.
- [ ] rc-shape scripting: `for (f in *.md) { wc -l $f }` runs.
- [ ] Pale Fire prompt renders with the canonical three-segment colour scheme.
- [ ] No kernel extinctions, no driver crashes, no zombie processes.
- [ ] No P0/P1 audit findings on the Utopia surface (the shell + line editor + libthyla-rs extensions + native coreutils).

Linux-compat (curl/git/ssh) and network-stack tests are NOT in Phase 7 exit; they land in Phase 8 (ROADMAP §9).

### 8.3 Specs landing this phase

No new formal specs. Per the 2026-05-23 spec-to-code suspension (`CLAUDE.md`), the U-1 design is validated by prose reasoning + adversarial audit + runtime test suite. The originally-gated `specs/poll.tla` / `specs/futex.tla` / `specs/notes.tla` / `specs/pty.tla` are deferred — `poll` + `futex` (`torpor`) + `notes` shipped under Pouch (Phase 6) without spec modules; PTY follows the same pattern under U-* chunks.

### 8.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| libthyla-rs extensions | `usr/lib/libthyla-rs/src/lib.rs` + new modules | New userspace runtime surface — file I/O wrappers, allocator, poll set, notes API, process spawn |
| ut shell parser + evaluator | `usr/utopia/shell/src/*` | Quoting, expansion, command lookup, builtin dispatch — input-parsing correctness on user input |
| ut fd-notes job control | `usr/utopia/shell/src/jobs.rs` (approx) | Note delivery + poll loop interaction — race-condition surface |
| Thylacine-extension builtins | `usr/utopia/shell/src/builtins/{bind,mount,cap,note,rfork,pivot_root}.rs` | Privilege-affecting builtins; capability + namespace semantics must be correct |
| libutopia line editor | `usr/utopia/libutopia/src/line_editor.rs` | Raw mode + termios state; signal-clean exit on Ctrl-C; cursor-accounting correctness for ANSI escapes |
| PTY server | `usr/pty-server/` (Phase 7 deliverable) | termios state correctness; master/slave atomicity |
| Helix port | `usr/helix/` + pouch-patch growth | Ported foreign code via Pouch; same audit shape as stratumd (boundary-line patches) |

### 8.5 Risks

- **The line editor is hand-rolled.** Reedline parity is not free — ~2000 LOC of state-machine code that has to be right. Mitigation: scoped to v1 essentials; iterate in v1.x; tested in U-Z integration.
- **Helix port has dependency-graph surface.** Helix vendors ~150 Rust crates and tree-sitter grammars. Each may surface a new pouch-patch need. Mitigation: bound to Helix's audited crate set; pouch-patch growth audited per-extension.
- **PTY infrastructure is new.** termios + master/slave atomicity has historically been an audit surface. Mitigation: the PTY chunk gets its own audit round.
- **Native libthyla-rs is unfamiliar surface for new contributors.** Most Rust devs reach for std reflexively. Mitigation: U-1 scripture makes the decision rule explicit; code-review enforces.
- **"Feels real, not broken" is a judgment call.** Mitigation: the integration test is concrete (the 11 headline checks of `docs/UTOPIA-SHELL-DESIGN.md §18`); "feels real" is the test passing.

### 8.6 Dependencies

- **Inbound (already landed)**:
  - Phase 5 (ROADMAP §7): 9P client + Stratum 9P integration.
  - Phase 5 corvus: precedent for native libthyla-rs Rust daemon.
  - Phase 6 (Pouch; ROADMAP §7A): the cross-compilation environment for Helix.
  - Phase 4 / 3 / 2 / 1 (kernel, scheduler, handles, drivers): all foundational.
- **Outbound**:
  - Phase 8 (Linux compat + network; ROADMAP §9): extends the Utopia surface; bash port; container runner.
  - Phase 9 (hardening + v1.0-rc; ROADMAP §10): audits the Utopia surface end-to-end.

### 8.7 Parallel opportunities

- The libthyla-rs uplift (U-2..U-2-test, ~9-12 sessions) gates everything else; within the uplift the foundation modules (err, handle, alloc) are strictly sequential but later modules (fs, process, notes/poll, territory/cap, thread/time/rand/tty, ninep/hardware) can be developed in parallel batches.
- After U-3 (workspace skeleton) lands, U-4 (line editor) + U-5 (parser) can proceed in parallel.
- U-Helix runs in parallel with U-6..N (independent dependency graph).
- U-9..N coreutils are mutually independent and can be batched/parallelized.

### 8.8 Performance budget contribution

- Syscall p99 (no contention): < 1µs (per `VISION.md §4.5`).
- Process creation p99: < 1ms.
- Pipe round-trip: < 5µs.
- Shell startup (`ut -c 'echo hi'`): < 50ms.
- Prompt redraw latency: < 16ms (one display frame at 60Hz on a typical terminal).
- Halcyon dependency: nothing yet (Halcyon is Phase 10 — the final v1.0 phase).

### 8.9 Carry-overs

- Linux-compat shell flags + bash port: deferred to Phase 8 (ROADMAP §9). Bash via Pouch becomes Linux-compat's deliverable.
- uutils-coreutils for full GNU-flag-coverage: deferred to Phase 8. Native Rust coreutils ship 9base-shape at v1; uutils brings GNU-coverage where users need it.
- Stratum-native history (cross-host sync, structured query): deferred to v1.x. Flat-file history ships at v1.
- Sixel / Kitty graphics emission from Thylacine programs: deferred to v1.x.
- Syntax highlighting at the prompt: deferred to v1.x.

---

## 9. Phase 6: Linux compat + network

**Goal**: a meaningful set of pre-built Linux ARM64 binaries runs on Thylacine without recompilation. Container-as-territory works. Network stack (TCP/IP) up and running for both Thylacine-native programs and Linux containers. Combined with Phase 5's Utopia, this delivers a **practical working OS** — fully usable via SSH or UART before any graphical layer is added.

### 9.1 Deliverables

**Linux syscall shim (extend Phase 5 shim)**:
- Cover the top 50 Linux ARM64 syscalls by frequency (per `ARCHITECTURE.md §11.5`).
- Add: `epoll_*` (post-v1.0; v1.1 candidate but if low-effort lands here), `inotify_*` (degrade gracefully), `bpf` (return ENOSYS), `perf_event_open` (return ENOSYS).

**Synthetic Linux filesystem servers**:
- `proc-linux/` (Phase 5 deliverable): polished here for full Linux-tool coverage (`ps`, `top`, `lsof`).
- `sys-linux/`: minimal `/sys/` server for `ldd`, dynamic linker, basic admin tools.
- `dev-linux/`: Linux-shaped `/dev/` (`tty`, `urandom`, `null`, `zero`, `fd/`).

**Container runner (`thylacine-run`)**:
- Userspace tool that:
  1. Takes an OCI container image (or directory root).
  2. Constructs a new process territory with the container root + synthetic Linux servers (`/proc-linux`, `/sys-linux`, `/dev-linux`).
  3. Starts the container's init inside the territory.
- This is the "flatpak / Steam Deck" model — containers are territories. No cgroups, no seccomp at v1.0; territory isolation is the boundary.
- **Container exec prerequisite** (HOLOTYPE #58, pulled forward to the pre-rc sub-arc): spawn must resolve binaries through the territory/stalk namespace (per-component X-search), retiring the flat `devramfs_lookup` — without it a container's binaries are categorically unexecutable.

**On-system toolchain (v1.0 scope per HOLOTYPE RW-13 / D3, 2026-06-11)**:
- clang/lld + make + git ported via Pouch — the self-hosting / build-storm (W2) story. Cross-compile-from-host remains the primary build path; the on-system toolchain is the additive v1.0 deliverable that makes VISION §11's full-developer-expectation userland coherent (`gcc`/`make`/`git` had no phase home before this vote — HOLOTYPE RW-12 W2-F1).
- The single largest Phase-8 pole; sequenced after the net arc + the container runner.

**Network stack** (binding design: `docs/NET-DESIGN.md` — the #68 charter, 2026-06-15; the bullets below are the index, refined there):
- `net/` (userspace 9P server): TCP/IP stack via smoltcp Rust port.
- VirtIO-net driver in userspace (Phase 3 deliverable, validated here under load).
- Exposes `/net/tcp/`, `/net/udp/`, `/net/ipifc/<n>/` for compatibility with Plan 9-style network access.
- Linux-compat sockets API mapped via the syscall shim.
- **Network I/O rides Loom** (added 2026-06-08; `docs/LOOM.md` + `NOVEL.md` Angle #1 + `ARCHITECTURE.md` §10.1). Because `/net` is 9P, a `LOOM_OP_READ`/`LOOM_OP_WRITE` on a connection's data fid *is* recv/send and a multishot read on the listen file *is* an async accept loop -- so **no socket opcodes are added to Loom** (the vocabulary stays pure 9P). The userspace `net/` server is exactly `netd` (the stratumd-as-driver precedent), and Loom is what makes a userspace network stack fast enough -- it amortizes the app<->netd hops the way it does for stratumd. The native remote-access story is `import`/`exportfs` over authenticated 9P (corvus auth, the Plan 9 `cpu(1)` model); sshd stays a portable Pouch option for ecosystem compatibility.

**Integration tests**:
- A pre-built Linux ARM64 static binary (e.g., curl, wget, redis-cli) runs and performs network operations.
- `python3` static-musl binary runs a non-trivial script.
- An Alpine Linux container starts and a shell runs inside it.
- `wget https://example.com` works from Utopia (curl-as-fetcher).

### 9.2 Exit criteria

- [ ] `curl` (pre-built Linux ARM64 static binary) runs and fetches a URL.
- [ ] `python3` (pre-built musl-static Linux ARM64 binary) starts a REPL and runs a non-trivial script.
- [ ] **Container runner**: an Alpine Linux container image starts and a shell runs inside it; commands within the container behave correctly.
- [ ] **Network**: `ping 1.1.1.1` works from inside Thylacine (ICMP via VirtIO-net).
- [ ] `wget https://example.com` works from Utopia (TLS via system root cert bundle).
- [ ] `ssh` from Thylacine to an external host works (assuming bridged QEMU networking).
- [ ] **Server (W4-F8)**: a native `net::TcpListener` echo server accepts >=2 concurrent connections + echoes; the Loom-multishot accept loop accepts a stream; a ported `listen`/`accept` server (socket-compat) accepts a connection.
- [ ] **Soak (W4-F8)**: N connections x M seconds bidirectional traffic — no fd/connection/Burrow leak (the table returns to baseline), no corruption, under the SMP gate.
- [ ] No regressions in Utopia.
- [ ] No P0/P1 audit findings.

### 9.3 Specs landing this phase

One reserved net spec: **`net_poll.tla`** (clean + `BUGGY_LOST_READY`) — the I-9 register-then-observe over the `dev9p.poll`->netd readiness wake, written before the net-6 `dev9p.poll` impl per the per-surface spec-first re-enablement (NET-DESIGN.md §12.2). Otherwise none mandatory: network protocol correctness is smoltcp's responsibility (spec'd by smoltcp's authors), and the netd fid state machine + the rest are prose-validated per the 2026-05-23 broadening (W4-F7: the §9.3 waiver is now correctly scoped — smoltcp != the netd fid machine, which is designed in NET-DESIGN.md §3.4).

### 9.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| Linux syscall shim (extended) | `kernel/syscall-linux-shim.c` | Larger surface; more translation correctness |
| Container runner | `thylacine-run/` | Territory construction correctness, isolation |
| Network stack | `net/` | TCP/IP correctness; socket API translation |

### 9.5 Risks

- **`futex` correctness under heavy threading**: many threading libraries depend on it; subtle bugs can manifest at high contention. Mitigation: the torpor wait/wake atomicity is prose-validated + audited (no `futex.tla` per the 2026-05-23 suspension; `death_wake.tla` covers the death-wake interaction); stress-test under the Linux test programs.
- **`epoll`**: Linux-specific; many programs assume it. Mitigation: implement at v1.0 if low-effort; otherwise programs that assume it fall to "best effort" tier; v1.1 brings the full thing.
- **glibc dynamic linker**: glibc assumes `/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`. Mitigation: musl-static and musl-dynamic are the primary compat target; glibc-dynamic is best-effort.
- **Network stack maturity**: smoltcp is capable but not a full Linux network stack. Programs that assume specific socket options or kernel network behavior may fail. Mitigation: fall back to a Plan 9 IP stack port if smoltcp can't cover; coordinate at Phase 6 entry.
- **OCI compat**: container images vary; some assume specific cgroup features Thylacine doesn't have. Mitigation: ship documentation of what's supported; support `oci-runtime-spec` minimum.

### 9.6 Dependencies

- Phase 5 (musl, syscall shim — extended; pty + signals + futex for any compat program).
- Phase 4 (Stratum, for container root extraction).
- Phase 3 (userspace virtio-net driver — validated here under load).

### 9.7 Parallel opportunities

- Linux shim extension + synthetic FS servers parallelize.
- Network stack work is independent of container runner work.
- Both work-streams parallelize with each other.

### 9.8 Performance budget contribution

- Network round-trip latency (loopback): < 100µs.
- Network round-trip (single-hop): < 1ms (real-world bottleneck dominated by physical network).
- Container startup time: < 1s for a typical Alpine image.

### 9.9 Carry-overs

- Stratum 9P extensions used by container runner (e.g., `Tbind` for subvolume composition) tested in Phase 4 and re-validated here.
- Late-Utopia issues found during Linux compat testing get fixed here before Phase 7 entry.

---

## 10. Phase 7: Hardening + audit + 8-CPU stress + v1.0-rc

**Goal**: the textual + compat OS achieved at Phases 5-6 is hardened, audited end-to-end, stress-tested on 8 CPUs, fuzz-tested, and tagged as **v1.0-rc**. This is the **shippable v1.0-rc**: a complete, hardened, audited, Linux-binary-compatible textual OS. If Halcyon (Phase 8) hits problems and slips, this v1.0-rc ships as v1.0 and Halcyon becomes v1.1.

### 10.1 Deliverables

**Comprehensive audit pass**:
- Every audit-trigger surface (per `ARCHITECTURE.md §25.4`) — except those not yet introduced (Halcyon's surfaces come Phase 8) — gets a dedicated soundness audit.
- Findings tracked per round; closed before phase exit.
- Cumulative audit closed-list maintained at `memory/audit_rN_closed_list.md`.

**Fuzzer integration**:
- AFL++ or LibFuzzer for:
  - The 9P client (malformed server responses).
  - The Linux syscall shim (malformed syscall arguments).
  - The ELF loader (malformed binaries).
  - The DTB parser (malformed device trees).
  - VirtIO core (malformed virtqueue descriptors).
  - musl libc (allocator, parsers).
- 1000+ CPU-hours per surface.

**Benchmark suite**:
- Boot time, context switch latency, VirtIO block throughput, 9P round-trip latency, IRQ-to-userspace latency, syscall p99 — against `VISION.md §4.5` budget. Halcyon frame rate is deferred to Phase 8.
- Comparison runs against Plan 9 / 9Front (where possible) and Linux on the same QEMU configuration.

**SMP stress**:
- 8-CPU stress test for 72 hours. Multi-process / multi-thread workloads. No panic, no deadlock, no measurable latency regression.

**Documentation (Phase 7 portion)**:
- `docs/HACKING.md`: how to build and run.
- `docs/SYSCALLS.md`: full syscall reference.
- `docs/DRIVERS.md`: how to write a userspace driver.
- `docs/CONTAINERS.md`: container runner usage.
- `docs/NETWORK.md`: network stack architecture and admin.
- (`docs/HALCYON.md` lands Phase 8 with Halcyon.)

**Kernel preemption**:
- Kernel preemption enabled (the Phase 2 deferral). Audit-gated.

**Framebuffer console (fbcon) -- Tapestry stage 0** (added 2026-06-08; `docs/TAPESTRY.md` §17):
- The shell on a real monitor: the compositor with exactly one fullscreen surface (a shell pane), over virtio-gpu scanout + virtio-input. The forcing function for the bottom of the graphics stack (scanout + raster + bitmap text + input) and the first real consumer that hardens the green virtio-gpu / virtio-input drivers. **The one graphics piece with a v1.0-rc claim** ("a real OS on a monitor, not just a serial console") -- but **optional**: if it slips it becomes v1.1, like Halcyon, and never blocks the rc.
- The **agentic-enablement capture/inject loop** (TAPESTRY.md §16) wires in HERE, so graphics is never developed blind: QEMU `screendump` -> the agent reads the PNG; QMP `input-send-event`; the 9P structural view (`cat /dev/halcyon/layout`); dev/test-build-gated in-band variants. This keeps the agent-primary loop alive into the graphical phase (TOOLING.md §10 gains the concrete ABI here).

**v1.0-rc release**:
- Git tag `v1.0-rc.1`.
- Release notes describing the textual + compat OS.
- QEMU disk image (ready-to-boot Stratum volume + initramfs + kernel).
- Reproducible build: same source produces same kernel binary.
- **Halcyon NOT included in v1.0-rc.** v1.0-rc is the shippable fallback if Halcyon slips.

### 10.2 Exit criteria — v1.0-rc

- [ ] **72-hour QEMU run** without kernel panic (automated, via CI). Multi-process workload simulating typical development use.
- [ ] **8-CPU 72-hour stress** with `parallel make`-style workload; no measurable latency regression.
- [ ] All audit-trigger surfaces (except Halcyon-introduced; Halcyon is Phase 8) have a completed audit round with **zero P0/P1/P2 findings**.
- [ ] **Boot to Utopia login prompt** in < 3s (QEMU `virt`, 4 vCPUs, 4 GiB RAM). VISION §4.5 budget.
- [ ] **VirtIO block throughput** ≥ 80% of QEMU-reported device limit.
- [ ] **9P round-trip latency** p99 < 500µs (loopback Stratum mount).
- [ ] **Syscall p99 (no contention)**: < 1µs.
- [ ] **Process creation p99**: < 1ms.
- [ ] **IRQ to userspace handler p99**: < 5µs.
- [ ] **Network round-trip latency (loopback)**: < 100µs.
- [ ] **MTE catches a deliberate UAF in a userspace test program**; same for kernel test.
- [ ] **PAC catches a deliberate kernel return-address forge**.
- [ ] **W^X invariant**: `/ctl/security/wx-violations` is 0 across the test suite.
- [ ] Fuzzers run for 1000+ CPU-hours per surface without finding new correctness bugs.
- [ ] Zero known kernel panics in the issue tracker.
- [ ] `docs/REFERENCE.md` snapshot reflects v1.0-rc state (Halcyon section pending).
- [ ] All TLA+ specs clean under TLC (9 specs total).
- [ ] `SPEC-TO-CODE.md` mappings current for all 9 specs.
- [ ] Reproducible build verified.
- [ ] **v1.0-rc.1 tag created** — this is the shippable fallback.

### 10.3 Specs landing this phase

None new. All 9 specs are landed by Phase 5; Phase 7 is the verification + tightening phase for them.

### 10.4 Audit-trigger surfaces

All — comprehensive pass. The list in `ARCHITECTURE.md §25.4` is the audit map. Halcyon-introduced surfaces (which don't exist yet) are deferred to Phase 8's audit pass.

### 10.5 Risks

- **Performance regression**: late-found regressions in any phase manifest here. Mitigation: continuous performance measurement throughout earlier phases.
- **Deadlock under SMP stress**: subtle deadlocks may not appear in shorter test runs. Mitigation: deadlock detector (lock dependency graph) instrumented in debug builds.
- **Audit findings**: P0/P1 findings late in Phase 7 push v1.0-rc. Mitigation: audit cadence throughout earlier phases means most issues are caught before Phase 7.
- **Documentation completeness**: docs are not "code" but they're a v1.0-rc release deliverable. Mitigation: doc-update-per-PR rule throughout.
- **Kernel preemption interactions**: enabling kernel preemption (deferred from Phase 2) can surface latent races. Mitigation: TSan + audit on the kernel preemption codepaths; staged enablement.

### 10.6 Dependencies

- All prior phases (1-6) complete.

### 10.7 Carry-overs / what comes next

If Phase 7 exits cleanly with v1.0-rc.1 tagged, **Phase 8 (Halcyon) begins**. The v1.0-rc tag is the shippable insurance: if Halcyon's medium-high risk materializes, the project can ship v1.0-rc as v1.0 and treat Halcyon as v1.1.

The "designed-not-implemented" v2.0 contracts (`NOVEL.md` Angle #9) remain explicitly out of v1.0:
- Capability elevation via factotum.
- Multikernel SMP.
- In-kernel Stratum driver.

Other v1.1+ candidates:
- `epoll`, `inotify`, `io_uring` syscalls.
- HW video decode.
- Bare-metal Pi 5.
- Apple Silicon bare metal.

---

## 11. Phase 8: Halcyon + v1.0 final release

**Goal**: Halcyon is the primary user interface. Images render inline. Video plays inline. The development workflow moves from UART debug shell to Halcyon. The hardened substrate from Phase 7 + Halcyon + a focused Halcyon-surface audit pass + final release tasks = **v1.0 final**.

This is the last phase of v1.0 and the highest-risk angle (`NOVEL.md` Angle #4 — medium-high risk). Halcyon is held to the end deliberately so its risk does not endanger the rest of the OS. If Halcyon hits a wall, Phase 7's v1.0-rc.1 becomes v1.0 final and Halcyon becomes v1.1.

> **EVOLVED 2026-06-08 — the compositor / client architecture.** The §11.1
> deliverables below are the Phase-0 model (a monolithic scroll-buffer Halcyon
> writing raw `/dev/fb`, with a bash-subset parser). They are **reframed** by the
> graphics-phase design (`docs/TAPESTRY.md` §13-17 + `NOVEL.md` Angle #4 +
> `ARCHITECTURE.md` §17): Halcyon is the first **client** of `tapestryd` (the
> compositor), presents pixels over **Loom**, drives layout + input over the
> `/dev/halcyon` **9P** tree, runs the **Utopia `ut`** shell (not a bash-subset
> parser), and renders an **anti-window tiling** UI (uniform containers;
> placement-transparent surfaces; a Helix-modal transcript) rather than a
> scroll-buffer-only primitive. New Phase-10 sequencing:
>
> - **On-ramp, BEFORE Halcyon: the compositor API + an SDL backend + software-Quake
>   as the API acceptance gate.** Prove the protocol under a demanding non-Halcyon
>   client first (original Quake's *software* rasterizer needs no GL). fbcon
>   (Tapestry stage 0) lands earlier, in Phase 9 (§10.1).
> - **Halcyon is pure 2D.** OpenGL is app-compat only (Mesa swrast via Pouch),
>   **v1.1+**, off Halcyon's critical path -- it never blocks v1.0.
> - **Two axes:** the Tapestry API is QEMU-validatable (virtio-gpu + virtio-input);
>   bare-metal output (RPi framebuffer) + input (USB-HID, the long pole) is Lazarus
>   work (`PORTABILITY.md`) plugging in beneath the finished API.
> - **Agentic enablement** (TAPESTRY.md §16) keeps the agent-primary loop alive
>   through this phase (it wires in at fbcon, §10.1).
>
> The risk profile (highest-risk, last-phase, fallback to the Phase-9 v1.0-rc) is
> unchanged and re-affirmed. The §11.1 bullets firm up at the graphics-phase design
> pass; **`docs/TAPESTRY.md` §13-17 is authoritative**.

### 11.1 Deliverables

**VirtIO-GPU userspace driver (extend Phase 3)**:
- `drivers/virtio-gpu/`: extend with BURROW-handle-based zero-copy framebuffer; expose `/dev/fb/` per `ARCHITECTURE.md §17.2`.

**Halcyon (Rust)** (`halcyon/`):
- Scroll-buffer rendering (text + graphical regions in time order).
- Monospace font rendering via fontdue (Iosevka or equivalent at v1.0).
- Image display: `display image.png` decodes via `png` / `image` crate; renders inline.
- Video display: mounts `/dev/video/player/`; polls `frame` (BURROW handle); blits to graphical region.
- Bash-subset interactive parser; job control (`Ctrl-Z`, `bg`, `fg`); pipes; redirection.
- 9P mount commands.

**Video player** (`drivers/video/`):
- Rust 9P server.
- Software H.264 decode at v1.0 (via FFI to a software decoder library or pure-Rust decoder).
- Exposes `/dev/video/player/` per `ARCHITECTURE.md §13`.

**Halcyon as init**:
- After Phase 8, `init` exec's Halcyon as the primary shell. UART debug shell becomes recovery only.

**Halcyon-surface audit pass**:
- Halcyon-introduced audit-trigger surfaces (scroll-buffer state machine, image decode, video player 9P client, framebuffer driver extensions, bash-subset parser) get their own focused audit round.
- Findings closed before v1.0 final.

**Documentation (Phase 8 portion)**:
- `docs/HALCYON.md`: Halcyon usage, customization, scroll buffer model.

**v1.0 final release**:
- Git tag `v1.0`.
- Release notes describing the complete v1.0 (Phase 7 v1.0-rc + Halcyon).
- QEMU disk image (ready-to-boot Stratum volume + initramfs + kernel + Halcyon).
- Reproducible build verified.

### 11.2 Exit criteria — v1.0 final

- [ ] Halcyon starts on boot; replaces UART debug shell as primary interface.
- [ ] Text output renders correctly (Iosevka, correct metrics, scrollback works).
- [ ] `display thylacine.png` renders a PNG inline in the scroll buffer.
- [ ] Scroll buffer: image scrolls away naturally as new output is added.
- [ ] `ls`, `cat`, `grep`, pipes — all work inside Halcyon.
- [ ] **Frame time p99**: < 16ms (60Hz floor; VISION §4.5).
- [ ] Halcyon resident memory: < 64 MiB with 100k-line scrollback.
- [ ] **Video**: `play video.mp4` plays a short H.264 video in the scroll buffer; controlled via `/dev/video/player/ctl`.
- [ ] User runs `vim` inside Halcyon; edits a Rust source file with syntax highlighting.
- [ ] User runs `tmux` inside Halcyon; multi-pane workflows work via tmux's own model.
- [ ] **Boot to Halcyon login** in < 3s (extends VISION §4.5 boot-to-login budget to include framebuffer init).
- [ ] No P0/P1 audit findings on Halcyon-introduced surfaces.
- [ ] No regressions in Phase 7 v1.0-rc behavior.
- [ ] `docs/REFERENCE.md` snapshot reflects v1.0 final state (including Halcyon).
- [ ] `docs/HALCYON.md` complete.
- [ ] **v1.0 final tag created**.

### 11.3 Specs landing this phase

None mandatory. Halcyon is mostly UI; the underlying primitives (9P client, framebuffer, BURROW transfer, signals) are spec'd or prose-validated at earlier phases. PTY is the exception — not yet built (LS-8, task #952); its validation lands with it.

### 11.4 Audit-trigger surfaces introduced

| Surface | Files | Why |
|---|---|---|
| Halcyon shell parser | `halcyon/src/parser/` | Bash-subset semantics correctness |
| Halcyon scroll buffer | `halcyon/src/buffer/` | State machine correctness |
| Video player | `drivers/video/src/` | 9P server + decode correctness |
| Framebuffer driver (extended) | `drivers/virtio-gpu/src/` | BURROW sharing correctness |
| Image decode | `halcyon/src/image/` | Memory safety; format-fuzz coverage |

### 11.5 Risks

- **Font rendering quality**: fontdue's quality must match `vim` in `iTerm2` for the experience to feel real. Mitigation: validate early; have a fallback (RustyType, swash). Iosevka must work.
- **Scroll buffer model edge cases**: resize, reflowing text around images, history trimming need careful design. Mitigation: design pass before writing rendering code.
- **Video performance**: software H.264 decode may not keep up at 1080p. Mitigation: profile; reduce target to 720p if needed; HW decode is post-v1.0.
- **Bash compat coverage**: parser is a "subset"; some Bash features will not work. Mitigation: explicit subset documentation; any scripts that need full Bash run via `/bin/bash` directly.
- **BURROW-based framebuffer race**: writer/reader race when Halcyon writes pixels and driver issues `flush`. Mitigation: simple double-buffering; documented protocol.
- **Halcyon-as-final-phase risk**: if Halcyon hits any of the above, Phase 8 slips. Mitigation: **Phase 7 produced v1.0-rc.1 as the shippable fallback**. If Halcyon takes meaningfully longer than estimated, the project ships v1.0-rc as v1.0 and treats Halcyon as v1.1. This is deliberate insurance, not aspirational — the v1.0-rc tag exists at Phase 7 exit precisely so Halcyon can fail without dragging the rest of the OS.

### 11.6 Dependencies

- Phase 7 complete (hardened, audited, v1.0-rc tagged substrate).
- Phase 5 (musl, futex, poll, pty, signals — all Halcyon's runtime depends on).
- Phase 4 (Stratum for image / video file loads).
- Phase 3 (userspace virtio-gpu driver — extended here).

### 11.7 Parallel opportunities

- Halcyon's scroll buffer + parser + image display can be built in parallel.
- Video player + Halcyon's video integration parallelize.
- VirtIO-GPU driver extensions parallelize with Halcyon work.
- Halcyon-surface audit and documentation work parallelize with implementation.

### 11.8 Performance budget contribution

- Halcyon frame time p99: < 16ms (60Hz floor).
- Halcyon resident memory: < 64 MiB at typical workload.
- Image decode + display p99: < 100ms for a 1024×768 PNG.
- Boot to Halcyon login: < 3s (extends Phase 7's < 3s boot-to-Utopia-login budget).

### 11.9 Carry-overs / what doesn't ship at v1.0

The "designed-not-implemented" v2.0 contracts (`NOVEL.md` Angle #9):
- Capability elevation via factotum.
- Multikernel SMP.
- In-kernel Stratum driver.

Other v1.1+ candidates:
- `epoll`, `inotify` syscalls (if not landed at Phase 6). (The io_uring concept is **Loom**, now a pre-Utopia arc §8.0a; the literal io_uring ABI is the v1.2 liburing-shim-over-Loom, §12.2.)
- HW video decode.
- Bare-metal Pi 5.
- Apple Silicon bare metal.
- Multi-pane Halcyon (within scroll buffer; experiment).

These are explicitly *not in v1.0*; they're tracked for post-v1.0 work.

---

## 12. Post-v1.0 roadmap

### 12.1 v1.1 candidates (3-6 months post-v1.0)

- **Bare-metal Raspberry Pi (Lazarus W4 / M2)**: the actual board bring-up — EL2→EL1 drop, BCM mailbox, a new SD/EMMC block backend (the load-bearing gap: Thylacine has only a virtio-blk path today), USB input, mailbox framebuffer (`arch/arm64/rpi400/`). **Pi 400 first** (the v8.0 ISA floor → widest compatibility; Pi 5 secondary, covered by the v8.0 base). Lazarus **M1** (W1 v8.0 floor + W2 GICv2 + W3 software-RNG) lands **pre-Utopia** (§8.0a) and is the QEMU-validated groundwork; **W4 is this post-v1.0 board port**. GIC-400 (now real via W2's GICv2 driver) and PL011 transfer from QEMU. Binding design `docs/PORTABILITY.md` §7; storage-backend fork open (PORTABILITY §9). Estimated: one focused sprint.
- **`epoll` syscall surface**: full Linux-compat epoll implementation as a kernel `Dev` wrapping `poll`. ~2-3 KLOC.
- **`inotify` syscall surface**: filesystem change notifications. Stratum already supports this internally; surface it.
- **HW video decode**: VirtIO video decoder as a separate driver class. ~3-5 KLOC Rust.
- **VirtIO-GPU virgl 3D**: 3D acceleration for Halcyon if needed. ~5-8 KLOC.
- **Container runtime refinement**: cgroup-equivalent resource limits via a Thylacine-native mechanism (per-process resource controls in `/ctl/proc-limits/<pid>/`).
- **Stratum extension propagation**: any new Stratum extensions added post-Phase 9 (e.g., new Tcommand) reach Thylacine's 9P client.
- **Recovery shell improvements**: better debugging tools in initramfs.

### 12.2 v1.2 candidates (6-12 months post-v1.0)

- **liburing-compat shim over Loom** (the literal Linux io_uring ABI): a thin, optional layer translating Linux `io_uring_setup`/`io_uring_enter` + the SQE/CQE format onto Loom (`docs/LOOM.md`). The *native* ring transport (Loom) itself moved **pre-Utopia** (§8.0a); this entry is now only the Linux-binary-compat shim, which stays out of Loom's core — a near-empty consumer set at v1.0 (the Pouch targets use zero io_uring; relevant only if a future perf-critical io_uring-native server with no fallback is ported).
- **Bluetooth + USB beyond keyboard/mouse**: USB stack, basic Bluetooth.
- **Multi-pane Halcyon (within scroll buffer)**: experiment with scroll-buffer-with-side-by-side regions. Carefully scoped to not become a windowing system.
- **`thylacine-run` improvements**: cgroups-equivalent (without cgroup machinery; using territory-level resource accounting).

### 12.3 v2.0 candidates (12-24+ months)

- **Capability elevation via factotum** (per `ARCHITECTURE.md §15.4` design). Implementation per the design contract.
- **Multikernel SMP** (per `ARCHITECTURE.md §20.6` design). Post-v1.0 research direction; design contract scaffolds the v1.0 work.
- **In-kernel Stratum driver** (per `ARCHITECTURE.md §14.4` design). Bypasses 9P-client for root FS hot path.
- **Apple Silicon bare metal**: m1n1 + AIC + AGX (via Asahi).
- **RISC-V port**: target first RVA23-compliant SBC. Mechanical above `arch/`.
- **x86-64 port**: target QEMU `q35`; later bare metal.
- **Rust kernel components**: selected modules (9P client, ELF loader, handle table) ported from C99 to Rust.
- **Full POSIX ACLs / xattrs at the territory level**: complement Stratum's existing xattr/ACL with territory-level semantics.
- **MAC (mandatory access control)**: SELinux-equivalent for multi-tenant deployments. Post-v2.0; territory isolation suffices for v1.0/v1.x.
- **Audio stack**: VirtIO sound device + userspace audio server.

### 12.4 v3.0 horizon

- **Multikernel + 16+ cores**: per-core kernel instances; cross-core via 9P; NUMA-aware. The full Barrelfish-style architecture.
- **Network filesystem as native territory**: 9P-over-TLS; Thylacine processes' territories transparently span machines.
- **Distributed territories**: per-process territories that span multiple Thylacine nodes via 9P forwarding.

### 12.5 Ruled out

- **Graphical windowing system** (compositor, window manager, display server). Ever.
- **Binary-perfect glibc compatibility**. Best-effort always.
- **Distributed / clustered OS**. 9P over network is supported; the OS doesn't manage clusters.
- **Windows binary compatibility**. Not in scope, ever.
- **Backward compatibility with Linux kernel modules**. Not in scope.
- **Sound / Bluetooth / hardware sensors at v1.0**. Deferred to v1.x.
- **Real-time scheduling at v1.0**. EEVDF gives soft latency bounds; hard RT is v2.x.

---

## 13. Risk register

| # | Risk | Phase | Severity | Mitigation |
|---|---|---|---|---|
| 1 | Scheduler race conditions (SMP) | 2 | HIGH | TLA+ spec before coding; TSan from day one; single-CPU first |
| 2 | Userspace driver IRQ latency budget miss | 3 | HIGH | Benchmark early; if missed, optimize before Phase 4 |
| 3 | 9P client correctness | 4 | HIGH | Dedicated audit round; strict server (Stratum) catches bugs early |
| 4 | Stratum Phase 9 timeline coordination | 4 | MEDIUM | Communicate; Phases 1-3 proceed in parallel; coordinate at Phase 4 entry |
| 5 | musl port effort | 5 | MEDIUM | Plan 9's libc as reference; musl designed for portability |
| 6 | uutils-coreutils gaps | 5 | MEDIUM | Fall back to GNU coreutils for any command with regression |
| 7 | `futex` translation correctness | 6 | MEDIUM | Spec'd; stress-test under Linux test programs |
| 8 | Network stack maturity (smoltcp) | 6 | MEDIUM | Plan 9 IP stack as fallback if smoltcp gaps surface |
| 9 | glibc compat | 6 | LOW | Explicitly best-effort; musl is the compat target |
| 10 | Container compat across OCI variants | 6 | LOW | Documented support; minimum oci-runtime-spec coverage |
| 11 | Audit late-finding | 7 | MEDIUM | Audit cadence throughout earlier phases means most issues are caught before Phase 7 |
| 12 | Performance regression undetected | 7 | MEDIUM | Continuous performance measurement throughout earlier phases |
| 13 | 8-CPU SMP correctness under stress | 7 | MEDIUM | Spec'd; TSan-clean; staged enablement (1 → 4 → 8 cores) |
| 14 | Kernel preemption races (deferred from Phase 2) | 7 | MEDIUM | Spec'd; staged enablement; TSan-clean |
| 15 | Halcyon font rendering quality | 8 | MEDIUM-HIGH | Validate fontdue early; fallback to swash / RustyType. v1.0-rc.1 from Phase 7 is the shippable fallback if Halcyon slips |
| 16 | Halcyon scroll-buffer design edge cases | 8 | MEDIUM-HIGH | Design pass before rendering; iterate. v1.0-rc fallback as above |
| 17 | Halcyon-as-final-phase risk | 8 | HIGH (mitigated) | **Insurance: Phase 7 produces v1.0-rc.1 textual+compat OS. If Halcyon slips, v1.0-rc ships as v1.0 and Halcyon becomes v1.1.** |
| 18 | GIC v2/v3 autodetection | 1 | LOW | DTB compatible string; test both QEMU configurations |
| 19 | ELF loader edge cases | 2 | LOW | Static binaries only initially; fuzz the loader in Phase 7 |
| 20 | KASLR / hardening interactions with toolchain | 1 | LOW | Phase 1 catches these; subsequent phases inherit working build |
| 21 | MTE performance overhead | 1, 7 | LOW | Measure; if > 15% on critical paths, restrict to allocations only |

---

## 14. Cross-phase concerns

### 14.1 Git workflow

- **Branches**: feature branches per chunk; merged to `main` after audit + tests + spec verification.
- **Commits**: per-chunk commits with detailed messages. First line under ~70 chars; body explains WHY and what alternative was rejected.
- **`Co-Authored-By` footer** on AI-assisted commits.
- **Never force-push to `main`**.
- **Never skip pre-commit hooks** unless explicitly authorized.
- **Pre-commit**: full default-build test suite passes.
- **Pre-merge** for invariant-bearing changes: full sanitizer matrix + all specs.

### 14.2 Versioning during development

- v0.1 — Phase 1 complete (kernel skeleton boots).
- v0.2 — Phase 2 complete (processes + scheduler + handles).
- v0.3 — Phase 3 complete (userspace drivers).
- v0.4 — Phase 4 complete (Stratum mounted).
- v0.5 — **Utopia** — Phase 5 complete. The first user-visible release.
- v0.6 — Phase 6 complete (Linux compat + network; practical working OS).
- **v1.0-rc.1** — Phase 7 complete (hardening + audit + 8-CPU stress + v1.0-rc tag). The shippable textual + compat OS. If Halcyon slips, this ships as v1.0.
- **v1.0** — Phase 8 complete (Halcyon + final audit + final release tag). The graphical v1.0.

### 14.3 Documentation cadence

- ARCHITECTURE updates first (when an architectural decision changes).
- ROADMAP updates next (when phase scope changes).
- Phase status doc (`docs/phaseN-status.md`) updates per chunk.
- `docs/REFERENCE.md` updates per landed module (the as-built reference, distinct from ARCHITECTURE which is design intent).
- CLAUDE.md audit-trigger table refreshes when new audit-trigger surfaces are added.

### 14.4 Memory + session continuity

Per CLAUDE.md (Phase 4 deliverable). At every session boundary:
1. Update `project_active.md` with current state.
2. Update `project_next_session.md` with pickup pointer.
3. Update affected phase status doc.
4. Commit memory + status updates.

### 14.5 Performance measurement

A reference QEMU configuration (4 vCPUs, 4 GiB RAM, virtio-blk on a tmpfs-backed image, virtio-net bridged) runs the benchmark suite per CI build. Regressions exceeding 5% from previous build trigger a flag.

`docs/BENCHMARKS.md` captures historical numbers; the v1.0 release version is the authoritative comparison point.

### 14.6 Compat-tier release plan

- **Tier 1 — native** (musl + uutils + Plan 9 userland): Phase 5 ships (Utopia).
- **Tier 2 — static Linux ARM64**: Phase 6 ships.
- **Tier 3 — OCI containers**: Phase 6 ships.

### 14.7 The audit-round closed list

Per Stratum's pattern. Each audit round produces a `memory/audit_rN_closed_list.md` listing fixed findings. Subsequent rounds use this as the "do-not-re-report" preamble. Cumulative.

### 14.8 Stratum dependency liaison

Stratum is on Phase 8 (POSIX surface) currently, on track for Phase 9 (9P server + clients) to land before Thylacine Phase 4 begins. Coordination:

- Stratum Phase 8 and Thylacine Phases 1-3 proceed in parallel (no dependency).
- Stratum Phase 9 entry triggers Thylacine Phase 4 entry-readiness check.
- Any Stratum 9P extension that emerges late in Phase 9 is added to Thylacine's 9P client at Phase 4 (or at v1.1 if late enough).
- Stratum's audit-trigger surfaces are *Stratum's* responsibility; Thylacine's audit covers the OS-side integration.

---

## 15. Per-phase deliverable mapping (cross-reference)

| Deliverable | Phase | ARCH section |
|---|---|---|
| Boot path (start.S, MMU, KASLR) | 1 | §5 |
| Buddy + magazines + SLUB allocator | 1 | §6 |
| Hardening defaults (CFI, PAC, MTE, BTI, LSE) | 1 | §24 |
| GIC + IRQ infrastructure | 1 | §12 |
| Process + thread + rfork | 2 | §7 |
| EEVDF scheduler + per-CPU + IPI | 2 | §8 |
| Territory primitives | 2 | §9.1 |
| Handle table + BURROW manager | 2 | §18, §19 |
| Page fault + COW + W^X enforcement | 2 | §6 |
| Pipes + notes | 2 | §10 |
| Dev vtable | 3 | §9.2 |
| Kernel-internal Devs (cons, null, zero, etc) | 3 | §9.4 |
| VirtIO core | 3 | §13 |
| Userspace virtio-blk/net/input/gpu | 3 | §13 |
| Driver process supervisor | 3 | §9.3 |
| 9P2000.L client + Stratum extensions | 4 | §10.2, §21 |
| Stratum mount as / | 4 | §14.1 |
| janus integration | 4 | §15 |
| Per-process 9P connection | 4 | §14.3 |
| Full syscall table | 5 | §11 |
| musl port | 5 | §16.2 |
| poll / select / futex / pty / signal | 5 | §23 |
| uutils-coreutils + Plan 9 userland + bash | 5 | §23.2 |
| /proc-linux + /sys-linux + /dev-linux | 5, 6 | §23 |
| **Utopia** ships | **5 exit** | §13 (VISION) |
| Linux syscall shim (extended) | 6 | §16.3 |
| Container runner | 6 | §16.5 |
| Network stack (smoltcp) | 6 | §16 (TBD subsection) |
| **Practical working OS** (Utopia + compat + network) | **6 exit** | — |
| Comprehensive audit pass (excl. Halcyon-introduced) | 7 | §25 |
| 8-CPU 72-hour stress | 7 | §20 |
| Fuzzers (1000+ CPU-hours per surface) | 7 | §25 |
| Performance budget compliance (excl. Halcyon frame) | 7 | §4.5 (VISION) |
| Kernel preemption (Phase 2 deferral) | 7 | §8 |
| **v1.0-rc.1 tag** (shippable fallback) | **7 exit** | — |
| VirtIO-GPU userspace driver (extended for Halcyon) | 8 | §13, §17 |
| Halcyon shell | 8 | §17 |
| Video player 9P server | 8 | §13 |
| Halcyon-surface audit pass | 8 | §25 |
| Halcyon frame time budget | 8 | §4.5 (VISION) |
| **v1.0 final release** | **8 exit** | — |

---

## 16. Summary

Thylacine OS is an 8-phase journey from kernel skeleton to v1.0 release:

1. **Phase 0 (DONE)**: VISION, COMPARISON, NOVEL, ARCHITECTURE, ROADMAP, CLAUDE.md.
2. **Phases 1-8**: kernel → processes/scheduler/handles → userspace drivers → Stratum integration → Utopia → Linux compat + network → hardening + v1.0-rc → Halcyon + v1.0 final.

Key commitments:
- ARM64 / QEMU `virt` throughout.
- C99 kernel; Rust userspace.
- 9P2000.L + Stratum extensions as the universal protocol.
- Userspace drivers from Phase 3 — no in-kernel virtio shortcut.
- Stratum integration at Phase 4 (Stratum is feature-complete; Phase 9 is the integration target).
- **Utopia at Phase 5 exit** — the textual POSIX milestone.
- **Linux compat + network at Phase 6** — the practical working OS.
- **Hardening + v1.0-rc at Phase 7** — the shippable fallback. If Halcyon slips, v1.0-rc.1 ships as v1.0.
- **Halcyon at Phase 8** — the final phase; graphical layer on top of the hardened, audited, network-capable substrate. Highest-risk angle; held to last so its risk doesn't endanger the rest of v1.0.
- Spec-first for invariant-bearing changes (9 TLA+ specs gate-tied).
- Adversarial audit cadence from Phase 1 onward (comprehensive at Phase 7; Halcyon-surface at Phase 8).
- SOTA hardening from day one.
- Latency budget enforced continuously.
- Designed-not-implemented contracts for v2.0 (factotum, multikernel, in-kernel Stratum).

The ordering is correct. The ambition is bounded. The audit discipline is enforced. **Halcyon-as-last-phase is deliberate insurance against the riskiest novel angle.**

The thylacine runs again.

---

## 17. Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Initial draft (Phase 0). | Ground-up rewrite from `tlcprimer/ROADMAP.md`. Userspace drivers from Phase 3 (no in-kernel virtio-blk shortcut). Utopia milestone at Phase 5 exit. uutils-coreutils as default coreutils; Plan 9 userland Tier 1; BusyBox in initramfs. Stratum coordination story aligned with Stratum's actual state (feature-complete; Phase 9 9P server is the integration target). 9 TLA+ specs gate-tied to phases (`scheduler` / `territory` / `handles` at Phase 2; `burrow` at Phase 3; `9p_client` at Phase 4; `poll` / `futex` / `notes` / `pty` at Phase 5). 8-CPU stress at Phase 8. SOTA hardening from Phase 1. Performance budgets per phase. Per-phase deliverable mapping cross-reference. Risk register expanded. v2.0 contracts (factotum / multikernel / in-kernel Stratum) tracked under post-v1.0. |
| 2026-05-04 | Halcyon-as-last-phase reorder. | User direction: "place Halcyon as the last phase — practical working OS with compat, binary shims, and polished Utopia will suffice." Phase 6 → Linux compat + network (was Phase 7). Phase 7 → Hardening + audit + 8-CPU stress + **v1.0-rc.1 tag** (was Phase 8 hardening). Phase 8 → Halcyon + Halcyon-surface audit + **v1.0 final** (was Phase 6). Risk register restructured: Halcyon's medium-high risk now isolated to the last phase; Phase 7 produces a shippable v1.0-rc as deliberate insurance. If Halcyon hits a wall, v1.0-rc ships as v1.0 and Halcyon becomes v1.1. |
