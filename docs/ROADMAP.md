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
            rfork, exec, namespace, EEVDF, handle table, VMO manager

Phase 3  ─ Device model + userspace drivers     [device layer]
            Dev vtable, VirtIO core, userspace virtio-blk/net/input/gpu

Phase 4  ─ 9P client + Stratum integration      [storage]
            Pipelined 9P client, Stratum mount, janus, ramfs → Stratum

Phase 5  ─ Syscall surface + musl + Utopia      [userspace milestone]
            Full syscall table, musl port, uutils-coreutils, rc, bash,
            poll, futex, pty, signals, /proc, /dev/pts/, Utopia ships

Phase 6  ─ Linux compat + network               [practical OS]
            Linux ARM64 binary shim, container runner, network stack,
            polished Utopia + binary shims = practical working OS

Phase 7  ─ Hardening + audit + v1.0-rc          [stable substrate]
            Fuzz, audit, benchmarks, 8-CPU stress, docs, v1.0-rc tag.
            If Halcyon (Phase 8) slips, this v1.0-rc ships as v1.0.

Phase 8  ─ Halcyon + v1.0 final                 [graphical layer]
            Framebuffer driver extensions, scroll-buffer shell, image,
            video, Halcyon-surface audit, v1.0 final release.

Post-v1.0 (v1.1, v1.2, v2.0+) — see §16
```

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
- **Stress tests**: long-running, with process creation/destruction, namespace manipulation, I/O concurrency.
- **Sanitizer matrices**: every commit on the default build + ASan + UBSan + (TSan from Phase 2 onward when SMP is enabled).

Pre-commit: full test suite on default build. Pre-merge for invariant-bearing changes: all matrices + all specs.

### 3.4 Stratum is the reference deployment environment from Phase 4 onward

Once Phase 4 lands, all development work happens inside a Thylacine QEMU VM with Stratum as the root filesystem. **Eating the dog food** from Phase 4 onward.

### 3.5 No in-kernel drivers (with bounded exceptions)

If a device can be driven from userspace via VirtIO + 9P, it is. Kernel drivers are the last resort. The boundary is: interrupt entry, DMA setup, and the VirtIO transport core are in-kernel; protocol logic and device state are in userspace.

There are *no* userspace-as-shortcut deferrals — the priming's "in-kernel virtio-blk for Phase 3 expedience, promote to userspace by Phase 6" is rejected. Phase 3 ships userspace drivers from day one.

### 3.6 The compat layer is built on top, not baked in

The kernel API is Thylacine-native throughout. POSIX/Linux compatibility is implemented as a userspace library (musl) and a thin syscall translation shim. The kernel is never modified to accommodate POSIX requirements.

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

**Goal**: the kernel can create, schedule, and destroy processes. `rfork`, `exec`, `exits`, `wait` work. Multiple processes run concurrently with EEVDF preemption. Per-CPU SMP scheduling on 4 vCPUs. Handle table + VMO manager production-ready.

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

**Namespace**:
- `kernel/namespace.c`: `Pgrp`, `bind`, `mount` (stub: only ramfs mountable at this phase), `unmount`. Namespace cloned on `rfork(RFPROC)`; shared on `rfork(RFPROC | RFNAMEG)`.

**Handle table + VMO** (full per `ARCHITECTURE.md §18-§19`, all 8 KObj types):
- `kernel/handle.c`: handle table per-process. Allocation, rights checking, transfer-via-9P (placeholder; 9P client comes Phase 4).
- `kernel/vmo.c`: VMO manager. `vmo_create()`, `vmo_create_physical()`, `mmap_handle()`, `irq_wait()` (placeholder for Phase 3 IRQ delivery).
- Handle types declared and enforced: `Process`, `Thread`, `VMO`, `MMIO`, `IRQ`, `DMA`, `Chan`, `Interrupt`. Hardware handles non-transferable by typed switch (per `ARCHITECTURE.md §18.3`).

**Memory**:
- `mm/vm.c` (extend): user address space management. `mmap` / `munmap` / `mprotect`. Page fault handler: allocate-on-demand for anonymous VMOs.
- `arch/arm64/fault.c`: page fault handler. Demand-pages anon mappings; COW on first write to shared page; VMO-backed faults route to VMO page lookup.

**Pipes and notes**:
- `kernel/pipe.c`: kernel pipe implementation. `pipe(fd[2])` syscall. Blocking read/write with a ring buffer.
- `kernel/notes.c`: note delivery, mask, handler invocation.

**Init**:
- `init/init-minimal.c`: the first userspace process at this phase. Starts a UART debug shell (busybox-equivalent, statically linked).

**Specs**:
- `specs/scheduler.tla`: EEVDF correctness, IPI ordering, wakeup atomicity, work-stealing fairness.
- `specs/namespace.tla`: bind/mount semantics, cycle-freedom, isolation.
- `specs/handles.tla`: rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability.

### 5.2 Exit criteria

- [ ] Two processes run concurrently on a single CPU; timer preemption works.
- [ ] Four processes run concurrently on 4 vCPUs; EEVDF latency bound holds (max latency ≤ slice_size × N).
- [ ] `rfork(RFPROC)` + `exits()` + `wait()` lifecycle works without leak (1000-iteration stress test).
- [ ] `exec()` loads and runs a static ELF from ramfs; rejects RWX segments.
- [ ] init starts a UART shell; `echo hello` works via pipe.
- [ ] Page fault handler allocates demand pages; stack growth works.
- [ ] **Handle table**: 10,000 handles open/close cycle without leak; rights reduction enforced; hardware-handle transfer attempt panics cleanly.
- [ ] **VMO**: create, map, write, read, unmap, close cycle correct; pages freed on last-handle-close + last-mapping-unmap.
- [ ] Stress: 1000 `rfork`/`exits`/`wait` cycles across 4 CPUs without leak or panic.
- [ ] Wakeup atomicity: 1000 producer/consumer pairs across 4 CPUs in tight loop; no missed wakeups (verified by counter).
- [ ] Work-stealing: 4-CPU test with imbalanced load; load redistributes within 5ms.
- [ ] **TSan clean** on the SMP test suite.
- [ ] `specs/scheduler.tla`, `specs/namespace.tla`, `specs/handles.tla` clean under TLC.
- [ ] `SPEC-TO-CODE.md` for all three specs maintained.
- [ ] No P0/P1 audit findings.

### 5.3 Specs landing this phase

- `specs/scheduler.tla` (mandatory)
- `specs/namespace.tla` (mandatory)
- `specs/handles.tla` (mandatory)

`SPEC-TO-CODE.md` mappings produced and CI-verified.

### 5.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| Scheduler | `kernel/sched.c`, `kernel/eevdf.c`, `kernel/run_tree.c`, `arch/arm64/context.c`, `arch/arm64/ipi.c` | EEVDF correctness, SMP, wakeup |
| Process | `kernel/proc.c`, `kernel/thread.c` | Lifecycle, rfork semantics |
| Namespace | `kernel/namespace.c` | Cycle-freedom, isolation |
| Handle table | `kernel/handle.c` | Rights, transfer, type discipline |
| VMO | `kernel/vmo.c`, `mm/vmo_pages.c` | Refcount, mapping lifecycle |
| Page fault | `arch/arm64/fault.c`, `mm/vm.c` | Lifetime, COW |
| ELF loader | `arch/arm64/elf.c` | RWX rejection |
| `mprotect` | `mm/vm.c` mprotect handler | W^X enforcement at runtime |

### 5.5 Risks

- **Scheduler correctness**: race conditions in `sched()` / `wakeup()` are subtle. Mitigation: TLA+ spec before coding; TSan from day one; single-CPU first, then 4-CPU.
- **EEVDF tuning**: `slice_size` parameter tuning may need empirical work. Mitigation: parameterize; sweep at Phase 8.
- **VMO lifecycle correctness**: handle ref + mapping ref interaction is subtle. Mitigation: `specs/vmo.tla` written here even though it's a Phase 3 deliverable (early scaffolding).
- **TLS corruption**: forgetting `TPIDR_EL0` save/restore breaks musl's TLS. Mitigation: tested early via a TLS-using userspace program.
- **ELF loader edge cases**: static ARM64 ELF with `PT_LOAD` segments. Keep simple at Phase 2 (no dynamic linker, no interpreter — Phase 5 work).

### 5.6 Dependencies

- Phase 1 (kernel skeleton, MMU, allocator, IRQ infrastructure).

### 5.7 Parallel opportunities

- Process / thread + scheduler can be built in parallel.
- Handle table + VMO can be built in parallel.
- Namespace work parallelizes with all of the above.
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

**Dev vtable + Chan**:
- `kernel/dev.c`: Dev vtable infrastructure. `devtab[]` registration. Chan lifecycle (`alloc`, `free`, `walk`, `clunk`).
- `kernel/chan.c`: Chan operations. Reference counting; per-Chan locking.

**Kernel-internal Devs**:
- `dev/cons.c`: console device (`/dev/cons`, `/dev/consctl`). Wires UART to Dev interface.
- `dev/null.c`, `dev/zero.c`: trivial.
- `dev/random.c`: CSPRNG. ARM64 `RNDR` instruction + chacha20 stir.
- `dev/proc.c`: `/proc/<pid>/` — synthetic Dev exposing process state (status, cmdline, fd list, mem).
- `dev/ctl.c`: `/ctl/` — kernel admin synthetic Dev. Exposes scheduler stats, IRQ counters, namespace dump.
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
- `specs/vmo.tla`: refcount + mapping lifecycle. Mostly written in Phase 2 (as scaffolding); finalized here.

### 6.2 Exit criteria

- [ ] `cat /dev/random` produces non-zero bytes.
- [ ] **Userspace virtio-blk**: read 1 GiB from VirtIO block device successfully; write 1 GiB and read it back, verify bit-exact.
- [ ] **Userspace virtio-net**: send and receive raw Ethernet frames via `/dev/ether0`; checksum verified.
- [ ] **Userspace virtio-input**: keyboard input from VirtIO console reaches user processes via `/dev/cons`.
- [ ] **Userspace virtio-gpu**: write pixels to framebuffer via VMO handle; visible on QEMU display.
- [ ] Chan lifecycle: 10,000 open/read/close cycles on `/dev/null` without leak.
- [ ] Dev vtable: all 11 ops dispatch correctly for cons, null, zero, random, proc, ctl, ramfs.
- [ ] **Driver crash recovery**: kill the virtio-blk driver process mid-I/O; supervisor restarts; subsequent I/O resumes.
- [ ] **Hardware handle non-transferability**: attempt to transfer `KObj_MMIO` panics with explicit "non-transferable type" message. Verified by deliberate test.
- [ ] IRQ-to-userspace handler latency p99 < 5µs (VISION §4.5 budget). Measured via dedicated benchmark.
- [ ] `specs/vmo.tla` clean under TLC. `SPEC-TO-CODE.md` maintained.
- [ ] No P0/P1 audit findings on driver model.

### 6.3 Specs landing this phase

- `specs/vmo.tla` (mandatory; scaffolded in Phase 2, finalized here).

### 6.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| VirtIO core | `kernel/virtio.c`, `kernel/virtio_pci.c` | Transport correctness |
| IRQ forwarding | `kernel/irqfwd.c` | Userspace IRQ delivery |
| VMO finalized | `kernel/vmo.c`, `mm/vmo_pages.c` | Refcount, mapping lifecycle |
| Userspace driver protocol | (driver Rust code, audit-target since v1.0) | Hardware access correctness |

### 6.5 Risks

- **PCIe enumeration**: `virt` has a PCIe root complex for VirtIO-GPU. Minimal PCIe support needed. Mitigation: keep enumeration minimal; one root complex; linear BAR allocation.
- **IRQ sharing**: VirtIO devices share IRQ lines in some QEMU configurations. Mitigation: GIC shared IRQ dispatch tested.
- **IRQ latency**: 5µs p99 to userspace is the architecture's claim; if missed, the project's driver model is in question. Mitigation: benchmark early in Phase 3; if missing, optimize the IRQ-to-handle wakeup path before Phase 4.
- **Driver process supervision**: getting clean crash recovery right requires careful ordering. Mitigation: write the supervision logic with a TLA+ sketch first.
- **VMO lifetime under driver crash**: when a driver crashes holding VMO handles, the kernel must clean up correctly without leaving stale mappings. Mitigation: `specs/vmo.tla` covers this.

### 6.6 Dependencies

- Phase 2 (handle table, VMO manager, IPI infrastructure, scheduler).

### 6.7 Parallel opportunities

- VirtIO core + drivers can be developed in parallel after the core's interface is stable.
- Each driver class (blk / net / input / gpu) is independent; multiple developers / sessions can work in parallel.
- `dev/proc.c` and `dev/ctl.c` are independent of VirtIO work.

### 6.8 Performance budget contribution

- IRQ to userspace handler: p99 < 5µs.
- VirtIO block I/O: ≥ 80% of QEMU-reported device throughput (target tightened at Phase 8).
- Network frame round-trip via `/dev/ether0`: < 50µs (rough; full network stack is Phase 7).

---

## 7. Phase 4: 9P client + Stratum integration

**Goal**: kernel speaks 9P2000.L + Stratum extensions. Stratum mounts as `/`. janus runs as userspace 9P server. The system boots from Stratum. Pipelined 9P client achieves throughput-budget targets.

### 7.1 Deliverables

**9P client (kernel)**:
- `kernel/9p_client.c`: 9P2000.L wire protocol implementation. Stratum extensions (`Tbind`, `Tunbind`, `Tpin`, `Tunpin`, `Tsync`, `Treflink`, `Tfallocate`).
- `kernel/9p_session.c`: per-session state (tag pool, outstanding-request table, send queue, receive loop). Pipelined per `ARCHITECTURE.md §21`.
- `kernel/9p_attach.c`: `mount` syscall integration. `Tattach` with subvolume name as `aname`.
- Per-process 9P connection management: connection established at `rfork`; closed at `exits`.

**Stratum daemon integration**:
- `init/init.c` (extend): after Phase 2's UART shell:
  1. Mount ramfs at `/`.
  2. Start `stratum` daemon (reads from VirtIO block; presents 9P server at `/var/run/stratum.sock`).
  3. Start `janus` daemon (key agent at `/dev/janus/`).
  4. Remount `/` from Stratum (kernel umounts ramfs from `/`; mounts Stratum 9P).
  5. Drop into UART debug shell (replaced by Halcyon at Phase 8 — the final phase of v1.0).
- `init/janus-client.c`: kernel-level janus client for cryptographic key unwrap during boot.

**Stratum coordination**:
- Phase 4 of Thylacine depends on Stratum Phase 9 (9P server + Stratum extensions). Stratum is on Phase 8 currently; Phase 9 is queued.
- Integration testing: mount a Stratum volume produced by Stratum's reference test suite; verify all standard FS operations.

**Specs**:
- `specs/9p_client.tla`: tag uniqueness, fid lifecycle, out-of-order completion, flow control.

### 7.2 Exit criteria

- [ ] `stratum` daemon starts from initramfs, mounts a Stratum volume on VirtIO block.
- [ ] Kernel mounts Stratum 9P server at `/`.
- [ ] `ls /`, `cat /etc/hostname`, `mkdir /tmp/test` work via Stratum.
- [ ] `janus` starts and successfully unwraps a passphrase-protected dataset key.
- [ ] **Reboot test**: data written before reboot is present after reboot (Stratum crash safety verified at OS boundary).
- [ ] 9P session: 10,000 open/read/write/close cycles on a Stratum file without protocol error or leak.
- [ ] **Pipelined throughput**: process issuing 32 concurrent reads on a Stratum file achieves throughput ≥ 90% of the session's bandwidth limit (vs naive serialized 9P).
- [ ] **9P round-trip latency** (loopback Stratum): p99 < 500µs (VISION §4.5).
- [ ] Per-process 9P connections: 100 processes each with their own connection to Stratum work without leaks.
- [ ] Stratum extensions verified: `Tbind` composes a subvolume into a connection's namespace; `Tpin` pins a snapshot; `Tsync` commits.
- [ ] `specs/9p_client.tla` clean under TLC.
- [ ] No P0/P1 audit findings on the 9P client.

### 7.3 Specs landing this phase

- `specs/9p_client.tla` (mandatory).

### 7.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| 9P client | `kernel/9p_client.c`, `kernel/9p_session.c`, `kernel/9p_attach.c` | Wire protocol correctness, fid lifecycle, tag uniqueness, pipelining |
| Boot transition | `init/init.c` | ramfs → Stratum handoff correctness |

### 7.5 Risks

- **9P client correctness**: fid management, walk chains, clunk on close — these are subtle. Stratum's server is strict (it's been audit-loop-hardened). Mitigation: spec first; integrate-with-Stratum testing from day one.
- **Stratum dependency**: Stratum Phase 9 is the integration target. If Stratum's Phase 9 timeline slips, Thylacine Phase 4 waits. Mitigation: coordinate timelines; communicate any Stratum-side issues immediately. Phases 1-3 of Thylacine can proceed in parallel with Stratum's Phase 8-9 work.
- **Stratum 9P extensions**: `Tbind` etc. are Stratum-specific; their semantics must be exactly aligned. Mitigation: exhaustive integration tests; spec the extensions in `9p_client.tla`.
- **janus integration**: janus is a userspace 9P server with its own protocol details. Mitigation: janus runs unchanged from its Linux/macOS deployment; the integration is just "mount and use."
- **Boot sequencing**: ramfs → Stratum transition is a one-shot operation that must be correct. Mitigation: documented sequence; failure means recovery boot (BusyBox fallback).

### 7.6 Dependencies

- Phase 3 (kernel virtio-blk userspace driver — Stratum reads from it).
- Phase 2 (handle table, VMO manager — for VMO transfer over 9P).
- **External**: Stratum Phase 9 (9P server + extensions).

### 7.7 Parallel opportunities

- 9P wire protocol implementation + session state can be developed in parallel.
- Stratum extension messages can be added incrementally; baseline 9P2000.L lands first.
- janus integration is independent of the rest of Phase 4; can land any time after Stratum mounts.

### 7.8 Performance budget contribution

- 9P round-trip latency (loopback Stratum): p99 < 500µs.
- 9P pipelined throughput: ≥ 90% of session bandwidth at 32 concurrent ops.
- Reboot time (Stratum mount + key unwrap + remount): < 5s (contributes to total boot-to-login budget).

---

## 8. Phase 5: Syscall surface + musl + Utopia

**Goal**: a complete textual POSIX environment. The Utopia milestone (`VISION.md §13`) — a developer using Thylacine via SSH or UART finds a working textual POSIX environment that "feels real, not broken." Halcyon is **not** required to ship at this phase or even at the next two; Halcyon is deliberately the final phase of v1.0 (Phase 8). Phase 5's exit is Utopia. Phase 6 adds Linux compat + network on top. Phase 7 hardens + audits the result and produces a v1.0-rc. Phase 8 lands Halcyon on top of the hardened substrate. The "practical working OS" the project commits to is achieved at Phase 7 exit; Halcyon is the additive graphical layer.

### 8.1 Deliverables

**Complete syscall table**:
- `kernel/syscall.c`: every syscall in `ARCHITECTURE.md §11.2` and `§11.3`. `errstr` exposed.
- `kernel/syscall-linux-shim.c`: top-50 Linux ARM64 syscall translation (per `§11.5`).

**Multiplexed I/O**:
- `kernel/poll.c`: `poll(fds, nfds, timeout)`. Wait list across N fds; first ready wakes thread. For 9P-backed fds, server signals readiness via 9P session notification; for kernel fds, readiness tracked in Chan structures.
- `select()` implemented atop `poll()`.

**Threading primitives**:
- `kernel/futex.c`: hash table keyed by physical address; per-bucket wait queue. `FUTEX_WAIT`, `FUTEX_WAKE`, `FUTEX_WAIT_BITSET`, `FUTEX_WAKE_BITSET`, `FUTEX_REQUEUE`, `FUTEX_CMP_REQUEUE`.
- `kernel/notes.c` (extend): full POSIX signal translation per `ARCHITECTURE.md §16.4`.

**PTY infrastructure**:
- Userspace 9P server `pty-server` exposing `/dev/ptmx` + `/dev/pts/<n>`.
- `dev/cons.c` (extend): `termios` via writes to `/dev/consctl` per `ARCHITECTURE.md §23.5`.

**musl port**:
- musl libc's `arch/aarch64/` adapted to emit Thylacine syscalls.
- pthread maps to `rfork(RFPROC | RFMEM | RFFDG | RFNAMEG | RFCRED | RFNOTEG)`.
- TLS via `TPIDR_EL0`.
- Dynamic linker (`ld-thylacine.so` — musl's ld.so, relinked).
- musl builds clean for Thylacine.

**Userland — Utopia**:
- **Tier 1 — Plan 9 userland**: ports of `rc`, `mk`, `awk`, `troff`, `tbl`, `eqn`, `9` launcher (from 9base / plan9port adapted for musl).
- **Tier 2 — uutils-coreutils**: complete coreutils suite (the Rust rewrite of GNU coreutils, full flag coverage). All commands in `ARCHITECTURE.md §23.2`.
- **Tier 3 — BusyBox in initramfs**: single-binary recovery shell.
- `bash` port: bash compiled against musl.
- `/etc/{passwd,group,hostname,resolv.conf,localtime,profile}` as Stratum files.

**POSIX-compat 9P servers**:
- `proc-linux/`: synthetic 9P server providing Linux-compat `/proc/<pid>/{status,cmdline,fd,maps,stat,statm,cwd,exe,root}` etc.
- (Native `/proc/` is in-kernel via `dev/proc.c`; Linux-compat layer adds Linux-specific names.)

**Specs**:
- `specs/poll.tla`: wait/wake state machine, missed-wakeup-freedom across N fds.
- `specs/futex.tla`: FUTEX_WAIT / FUTEX_WAKE atomicity.
- `specs/notes.tla`: note delivery ordering, signal mask correctness, async safety.
- `specs/pty.tla`: master/slave atomicity, termios state transitions.

### 8.2 Exit criteria — Utopia ships

The exit criteria are the Utopia bring-up integration test (per `ARCHITECTURE.md §23.9`):

- [ ] musl builds clean for Thylacine.
- [ ] uutils-coreutils all commands work with full Linux-compatible flags.
- [ ] **Compile and run a "hello world"** in C with `gcc` or `clang`; output reaches stdout.
- [ ] **Multi-stage shell pipeline**: `cat /etc/passwd | grep root | cut -d: -f1` produces correct output.
- [ ] **Job control**: launch `sleep 100`, `Ctrl-Z` to stop, `bg`, `fg` work; `Ctrl-C` interrupts.
- [ ] **`tmux` session**: create, split panes, run different commands per pane, attach/detach.
- [ ] **`vim`**: open a file, edit, save; syntax highlighting works.
- [ ] **`top` / `htop`**: shows correct stats, updates in real time.
- [ ] **`git clone https://...`** works (curl as fetcher; SSL via Stratum or system bundle).
- [ ] **`ssh` to a localhost service** works (PTY allocation).
- [ ] **`ps -ef | grep stratum`** shows the daemon running.
- [ ] **No kernel panics** during a 1-hour test session.
- [ ] **No driver crashes** during the same.
- [ ] Linux static ARM64 binary runs via the syscall shim (e.g., `echo hello` from Alpine Linux ARM64 container).
- [ ] `specs/poll.tla`, `specs/futex.tla`, `specs/notes.tla`, `specs/pty.tla` all clean under TLC.
- [ ] No P0/P1 audit findings.

### 8.3 Specs landing this phase

- `specs/poll.tla` (mandatory).
- `specs/futex.tla` (mandatory).
- `specs/notes.tla` (mandatory).
- `specs/pty.tla` (mandatory).

### 8.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| Syscall surface | `kernel/syscall.c`, `kernel/syscall-linux-shim.c` | Privilege correctness |
| poll/select | `kernel/poll.c` | Wait/wake state machine |
| futex | `kernel/futex.c` | Wait/wake atomicity, hash collision |
| Notes / signals | `kernel/notes.c`, `compat/signals.c` | Delivery ordering, async safety |
| PTY | `pty-server/` (userspace), `dev/cons.c` (kernel) | termios state correctness |
| Linux syscall shim | `kernel/syscall-linux-shim.c` | Syscall number translation correctness |

### 8.5 Risks

- **musl port effort**: musl is designed for portability but has not been ported to Thylacine. The syscall interface is close enough to Plan 9 + POSIX that the port is tractable but non-trivial. Estimate: 2-4 weeks of focused work. Mitigation: Plan 9's libc as reference; musl's portability docs.
- **uutils-coreutils maturity**: uutils is reaching parity with GNU coreutils (Apr 2025 status: ~95% coverage); some commands may have functional gaps. Mitigation: test against the Linux Test Project's coreutils tests; fall back to GNU coreutils for any command with regressions.
- **Job control**: `tcsetpgrp` and process-group management are notoriously fiddly. Mitigation: spec'd; test against `bash` + `tmux` + `vim` as the integration test set.
- **futex hash performance**: hash collisions can degrade futex performance. Mitigation: large hash table; collision chains tracked; benchmark under contention.
- **`/proc-linux` coverage**: programs vary in what `/proc/` paths they read. Mitigation: implement what's needed for the integration test; expand as needed.
- **Utopia subjectivity**: "feels real, not broken" is a judgment call. Mitigation: the integration test is concrete; "feels real" is the test passing.

### 8.6 Dependencies

- Phase 4 (Stratum mount; 9P client; per-process 9P connections).
- Phase 3 (Userspace drivers; framebuffer is fully exercised at Phase 8 with Halcyon, but the userspace virtio-gpu driver itself is Phase 3).
- Phase 2 (handles, scheduler, namespace, processes).

### 8.7 Parallel opportunities

- musl port + uutils-coreutils + bash port can proceed in parallel.
- POSIX syscall implementations + Linux shim parallelize.
- Plan 9 userland ports (`rc`, `mk`, `awk`, etc.) parallelize among themselves.
- Specs can be written before, during, alongside.

### 8.8 Performance budget contribution

- Syscall p99 (no contention): < 1µs (per VISION §4.5).
- Process creation p99: < 1ms.
- Pipe round-trip: < 5µs.
- futex wakeup: < 5µs.
- Halcyon dependency: nothing yet (Halcyon is Phase 8 — the final v1.0 phase).

### 8.9 Carry-overs

- Stratum extensions test coverage: any Stratum extension that emerged late in Phase 8 (the 9P server) gets tested in Phase 4 and re-tested here at Phase 5 with full POSIX integration.
- Recovery boot: BusyBox + Stratum-fsck path verified at Phase 5 entry.

---

## 9. Phase 6: Linux compat + network

**Goal**: a meaningful set of pre-built Linux ARM64 binaries runs on Thylacine without recompilation. Container-as-namespace works. Network stack (TCP/IP) up and running for both Thylacine-native programs and Linux containers. Combined with Phase 5's Utopia, this delivers a **practical working OS** — fully usable via SSH or UART before any graphical layer is added.

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
  2. Constructs a new process namespace with the container root + synthetic Linux servers (`/proc-linux`, `/sys-linux`, `/dev-linux`).
  3. Starts the container's init inside the namespace.
- This is the "flatpak / Steam Deck" model — containers are namespaces. No cgroups, no seccomp at v1.0; namespace isolation is the boundary.

**Network stack**:
- `net/` (userspace 9P server): TCP/IP stack via smoltcp Rust port.
- VirtIO-net driver in userspace (Phase 3 deliverable, validated here under load).
- Exposes `/net/tcp/`, `/net/udp/`, `/net/ipifc/<n>/` for compatibility with Plan 9-style network access.
- Linux-compat sockets API mapped via the syscall shim.

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
- [ ] No regressions in Utopia.
- [ ] No P0/P1 audit findings.

### 9.3 Specs landing this phase

None mandatory. Network protocol correctness is smoltcp's responsibility (it's been spec'd by smoltcp's authors).

### 9.4 Audit-trigger surfaces introduced or modified

| Surface | Files | Why |
|---|---|---|
| Linux syscall shim (extended) | `kernel/syscall-linux-shim.c` | Larger surface; more translation correctness |
| Container runner | `thylacine-run/` | Namespace construction correctness, isolation |
| Network stack | `net/` | TCP/IP correctness; socket API translation |

### 9.5 Risks

- **`futex` correctness under heavy threading**: many threading libraries depend on it; subtle bugs can manifest at high contention. Mitigation: covered by `futex.tla`; stress-test under the Linux test programs.
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

### 11.1 Deliverables

**VirtIO-GPU userspace driver (extend Phase 3)**:
- `drivers/virtio-gpu/`: extend with VMO-handle-based zero-copy framebuffer; expose `/dev/fb/` per `ARCHITECTURE.md §17.2`.

**Halcyon (Rust)** (`halcyon/`):
- Scroll-buffer rendering (text + graphical regions in time order).
- Monospace font rendering via fontdue (Iosevka or equivalent at v1.0).
- Image display: `display image.png` decodes via `png` / `image` crate; renders inline.
- Video display: mounts `/dev/video/player/`; polls `frame` (VMO handle); blits to graphical region.
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

None mandatory. Halcyon is mostly UI; the underlying primitives (9P client, framebuffer, VMO transfer, signals, PTY) are spec'd at earlier phases.

### 11.4 Audit-trigger surfaces introduced

| Surface | Files | Why |
|---|---|---|
| Halcyon shell parser | `halcyon/src/parser/` | Bash-subset semantics correctness |
| Halcyon scroll buffer | `halcyon/src/buffer/` | State machine correctness |
| Video player | `drivers/video/src/` | 9P server + decode correctness |
| Framebuffer driver (extended) | `drivers/virtio-gpu/src/` | VMO sharing correctness |
| Image decode | `halcyon/src/image/` | Memory safety; format-fuzz coverage |

### 11.5 Risks

- **Font rendering quality**: fontdue's quality must match `vim` in `iTerm2` for the experience to feel real. Mitigation: validate early; have a fallback (RustyType, swash). Iosevka must work.
- **Scroll buffer model edge cases**: resize, reflowing text around images, history trimming need careful design. Mitigation: design pass before writing rendering code.
- **Video performance**: software H.264 decode may not keep up at 1080p. Mitigation: profile; reduce target to 720p if needed; HW decode is post-v1.0.
- **Bash compat coverage**: parser is a "subset"; some Bash features will not work. Mitigation: explicit subset documentation; any scripts that need full Bash run via `/bin/bash` directly.
- **VMO-based framebuffer race**: writer/reader race when Halcyon writes pixels and driver issues `flush`. Mitigation: simple double-buffering; documented protocol.
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
- `epoll`, `inotify`, `io_uring` syscalls (if not landed at Phase 6).
- HW video decode.
- Bare-metal Pi 5.
- Apple Silicon bare metal.
- Multi-pane Halcyon (within scroll buffer; experiment).

These are explicitly *not in v1.0*; they're tracked for post-v1.0 work.

---

## 12. Post-v1.0 roadmap

### 12.1 v1.1 candidates (3-6 months post-v1.0)

- **Bare-metal Raspberry Pi 5**: EL2→EL1 drop, mailbox framebuffer driver (`arch/arm64/rpi5/`), RP1 Ethernet for network boot. GIC-400 and PL011 transfer from QEMU unchanged. Estimated: one focused sprint.
- **`epoll` syscall surface**: full Linux-compat epoll implementation as a kernel `Dev` wrapping `poll`. ~2-3 KLOC.
- **`inotify` syscall surface**: filesystem change notifications. Stratum already supports this internally; surface it.
- **HW video decode**: VirtIO video decoder as a separate driver class. ~3-5 KLOC Rust.
- **VirtIO-GPU virgl 3D**: 3D acceleration for Halcyon if needed. ~5-8 KLOC.
- **Container runtime refinement**: cgroup-equivalent resource limits via a Thylacine-native mechanism (per-process resource controls in `/ctl/proc-limits/<pid>/`).
- **Stratum extension propagation**: any new Stratum extensions added post-Phase 9 (e.g., new Tcommand) reach Thylacine's 9P client.
- **Recovery shell improvements**: better debugging tools in initramfs.

### 12.2 v1.2 candidates (6-12 months post-v1.0)

- **`io_uring` syscall surface**: ~5-8 KLOC. The existing 9P pipelining gives most of the win for 9P-mediated I/O; `io_uring` is for direct kernel I/O paths (rare under Thylacine architecture).
- **Bluetooth + USB beyond keyboard/mouse**: USB stack, basic Bluetooth.
- **Multi-pane Halcyon (within scroll buffer)**: experiment with scroll-buffer-with-side-by-side regions. Carefully scoped to not become a windowing system.
- **`thylacine-run` improvements**: cgroups-equivalent (without cgroup machinery; using namespace-level resource accounting).

### 12.3 v2.0 candidates (12-24+ months)

- **Capability elevation via factotum** (per `ARCHITECTURE.md §15.4` design). Implementation per the design contract.
- **Multikernel SMP** (per `ARCHITECTURE.md §20.6` design). Post-v1.0 research direction; design contract scaffolds the v1.0 work.
- **In-kernel Stratum driver** (per `ARCHITECTURE.md §14.4` design). Bypasses 9P-client for root FS hot path.
- **Apple Silicon bare metal**: m1n1 + AIC + AGX (via Asahi).
- **RISC-V port**: target first RVA23-compliant SBC. Mechanical above `arch/`.
- **x86-64 port**: target QEMU `q35`; later bare metal.
- **Rust kernel components**: selected modules (9P client, ELF loader, handle table) ported from C99 to Rust.
- **Full POSIX ACLs / xattrs at the namespace level**: complement Stratum's existing xattr/ACL with namespace-level semantics.
- **MAC (mandatory access control)**: SELinux-equivalent for multi-tenant deployments. Post-v2.0; namespace isolation suffices for v1.0/v1.x.
- **Audio stack**: VirtIO sound device + userspace audio server.

### 12.4 v3.0 horizon

- **Multikernel + 16+ cores**: per-core kernel instances; cross-core via 9P; NUMA-aware. The full Barrelfish-style architecture.
- **Network filesystem as native namespace**: 9P-over-TLS; Thylacine processes' namespaces transparently span machines.
- **Distributed namespaces**: per-process namespaces that span multiple Thylacine nodes via 9P forwarding.

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
| Namespace primitives | 2 | §9.1 |
| Handle table + VMO manager | 2 | §18, §19 |
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
| 2026-05-04 | Initial draft (Phase 0). | Ground-up rewrite from `tlcprimer/ROADMAP.md`. Userspace drivers from Phase 3 (no in-kernel virtio-blk shortcut). Utopia milestone at Phase 5 exit. uutils-coreutils as default coreutils; Plan 9 userland Tier 1; BusyBox in initramfs. Stratum coordination story aligned with Stratum's actual state (feature-complete; Phase 9 9P server is the integration target). 9 TLA+ specs gate-tied to phases (`scheduler` / `namespace` / `handles` at Phase 2; `vmo` at Phase 3; `9p_client` at Phase 4; `poll` / `futex` / `notes` / `pty` at Phase 5). 8-CPU stress at Phase 8. SOTA hardening from Phase 1. Performance budgets per phase. Per-phase deliverable mapping cross-reference. Risk register expanded. v2.0 contracts (factotum / multikernel / in-kernel Stratum) tracked under post-v1.0. |
| 2026-05-04 | Halcyon-as-last-phase reorder. | User direction: "place Halcyon as the last phase — practical working OS with compat, binary shims, and polished Utopia will suffice." Phase 6 → Linux compat + network (was Phase 7). Phase 7 → Hardening + audit + 8-CPU stress + **v1.0-rc.1 tag** (was Phase 8 hardening). Phase 8 → Halcyon + Halcyon-surface audit + **v1.0 final** (was Phase 6). Risk register restructured: Halcyon's medium-high risk now isolated to the last phase; Phase 7 produces a shippable v1.0-rc as deliberate insurance. If Halcyon hits a wall, v1.0-rc ships as v1.0 and Halcyon becomes v1.1. |
