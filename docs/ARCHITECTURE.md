# Thylacine OS — Architecture

**Status**: Phase 0 draft, 2026-05-04. This document translates the properties and philosophy in `VISION.md` into concrete, binding decisions. Every implementation document references this file. Decisions marked **COMMITTED** are the contract between design and implementation — they do not change without reopening.

**Companion documents**: `VISION.md`, `COMPARISON.md`, `NOVEL.md`, `ROADMAP.md`.

## Section status legend

- **STUB** — structure only, open questions listed, no decisions yet. There are no STUBs at Phase 0 exit; Phase 0's task is to eliminate them.
- **DRAFT** — first-pass content, under discussion within Phase 0.
- **COMMITTED** — signed off. Changes require written rationale and explicit re-opening.

---

## 1. How to read this document

**STATUS**: COMMITTED

`ARCHITECTURE.md` translates the properties and philosophy in `VISION.md` into concrete decisions. Every implementation document references this file. Decisions marked COMMITTED are the contract between design and implementation — they do not change without reopening.

### 1.1 Document relationships

- `VISION.md` — what we're building and why.
- `COMPARISON.md` — where we sit vs comparable systems.
- `NOVEL.md` — the lead positions and per-angle scope.
- `ARCHITECTURE.md` (this document) — how we're building it.
- `ROADMAP.md` — in what order.

### 1.2 Change management

Once a section is COMMITTED:
- Changing it requires a written rationale in the PR description.
- The section is explicitly re-opened (status → DRAFT) before changes are made.
- The corresponding `ROADMAP.md` phases are revisited for impact.
- The corresponding TLA+ spec (if any) is updated FIRST; then the architecture; then the implementation. If the three disagree, the spec wins.

### 1.3 The SOTA-from-start tenet

Thylacine commits to state-of-the-art implementations from the first commit. Stubs and primitive implementations are accepted only when they make the overall roadmap cleaner — not when they merely defer work that we know will need redoing. Examples of accepted simplifications: ramfs-during-bring-up before Stratum mounts (because the ramfs is throwaway by design); software video decode at v1.0 (because hardware decode requires a driver class we don't yet need). Examples of rejected shortcuts: in-kernel virtio-blk for "Phase 3 expedience" (because the userspace driver model must be proven; the priming had this and we reject it); MLFQ-3 instead of EEVDF (because EEVDF is the modern fair-share answer; skipping ahead is correct under SOTA).

### 1.4 The audit-trigger discipline

Section §25.4 enumerates the audit-trigger surfaces. Every change to a file or function listed there spawns a focused adversarial soundness audit before merge. This discipline is the project's permanent development cadence; it is not optional and it is not relaxed under schedule pressure.

---

## 2. System overview

**STATUS**: COMMITTED

### 2.1 The stack

```
  ┌─────────────────────────────────────────────────────────┐
  │  Halcyon (graphical scroll-buffer shell, Rust)           │
  │  — Phase 8 (final v1.0 phase), layered on top of the     │
  │    practical working OS achieved at Phases 5-7           │
  ├─────────────────────────────────────────────────────────┤
  │  Userspace services (9P servers, mostly Rust, some C)   │
  │    janus (key agent) │ drivers │ video │ network stack  │
  │    POSIX-compat 9P servers ( /proc, /sys, /dev/pts )    │
  │    ← hardware via typed handles (MMIO / IRQ / DMA)    → │
  │    ← buffer sharing via BURROW handles (zero-copy)       → │
  ├─────────────────────────────────────────────────────────┤
  │  Compatibility layer (POSIX/Linux syscall translation)  │
  │    musl port │ Linux ARM64 syscall shim │ container ns  │
  ├─────────────────────────────────────────────────────────┤
  │  Thylacine kernel (C99, ARM64)                          │
  │    Territory │ EEVDF Scheduler │ VM │ 9P client (pipe.) │
  │    Handle table │ BURROW manager │ IRQ forwarding │ Notes  │
  │    Dev vtable │ Spoor │ Pipes │ rendezvous │ Pty infra   │
  ├─────────────────────────────────────────────────────────┤
  │  Stratum (native filesystem, 9P server, userspace)      │
  │  — Phase 4, 9P2000.L + Stratum extensions               │
  └─────────────────────────────────────────────────────────┘
```

### 2.2 Key abstractions

- **Process (`Proc`)**: unit of isolation. Has a private territory, address space, credential set, handle table, and one or more threads.
- **Thread**: register context + kernel stack + signal mask + per-thread `errstr`. Multiple threads share a process's address space, territory, fd table, and handle table unless explicitly cloned via `rfork`.
- **Territory (`Territory`)**: a process's view of the resource tree. Composed via `bind` and `mount`.
- **Spoor**: the kernel object representing an open resource (file, device, synthetic node). Carries `qid`, `dev` pointer, offset, mode. The fundamental currency of the kernel; `read` / `write` / `walk` / `clunk` operate on Spoors.
- **Dev**: the vtable every kernel device implements (`attach`, `walk`, `stat`, `open`, `create`, `read`, `write`, `close`, `remove`, `wstat`, etc.). All kernel devices implement this interface; userspace devices implement it remotely via 9P.
- **9P server**: any userspace process exposing a resource tree via the 9P2000.L protocol. Drivers, services, and the filesystem are all 9P servers. Mounted into a process territory via the kernel's 9P client.
- **Kernel object handle**: typed, unforgeable integer token scoped to a process. Eight types: `Process`, `Thread`, `BURROW`, `MMIO`, `IRQ`, `DMA`, `Spoor`, `Interrupt`. Carry rights bitmasks.
- **BURROW** (Virtual Memory Object): kernel object representing a region of memory, independent of any process's address space. Reference-counted; pages live as long as any handle or mapping refers to them.
- **Note**: async text message delivered to a process. Plan 9's signal model. POSIX signals translate to/from notes in the compat layer.

### 2.3 Invariants each abstraction maintains

| Abstraction | Invariant | Enforcement | Spec |
|---|---|---|---|
| Process | Territory operations don't affect other processes' territories | Kernel territory isolation | `territory.tla` |
| Process | Capability set monotonically reduces (`rfork` only reduces, never elevates) | Syscall gate | `handles.tla` |
| Spoor | `spoor->dev` valid for the spoor's lifetime | Ref-counted dev attachment | runtime |
| Territory mount | Mount points form a DAG, never a cycle | Kernel mount validation | `territory.tla` |
| Handle | Rights monotonically reduce on transfer | Syscall-level check | `handles.tla` |
| Handle (hardware) | `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` cannot transfer | Syscall has no code path; static_assert | `handles.tla` |
| Handle (other) | Transferable handles (`Process`, `Thread`, `BURROW`, `Spoor`) transfer only via 9P | Kernel transfer syscall accepts only 9P-attached transfers | `handles.tla` |
| BURROW | Pages live until last handle closed AND last mapping unmapped | Ref-counted; `burrow.tla` proves no UAF | `burrow.tla` |
| 9P session | Fid table is per-connection; fids don't leak between connections | Per-session fid table | `9p_client.tla` |
| 9P session | Per-session tag uniqueness | Per-session tag pool with monotonic generation | `9p_client.tla` |
| Scheduler | Every runnable thread eventually runs | EEVDF deadline computation | `scheduler.tla` |
| Wakeup | No wakeup lost between wait-condition check and sleep | Wait/wake state machine | `scheduler.tla`, `poll.tla`, `futex.tla` |
| Memory | W^X on every page | Page table entry mutual exclusion | runtime + `_Static_assert` on PTE bits |
| Memory | KASLR offset randomized at boot | Boot init randomizes TTBR1 base | runtime |

The full enumerated invariant list (I-1 through I-N) lives in §28 with file-level traceability.

### 2.4 Layer responsibilities

- **Kernel** owns: virtual memory, scheduling, territory primitives, handle table, BURROW manager, IRQ forwarding, 9P client, ramfs, procfs (synthetic Dev), tmpfs, ctlfs, console (cons / consctl). All in C99.
- **Userspace driver processes** own: VirtIO devices (block, net, GPU, input). All in Rust. They hold `KObj_MMIO` / `KObj_IRQ` / `KObj_DMA` handles for hardware access and expose results as 9P servers.
- **Userspace services** own: network stack (TCP/IP), key agent (janus), POSIX-compat synthetic FS (`/proc-linux`, `/sys-linux`, `/dev/pts/`), video player. All in Rust except where C-FFI bindings to existing libraries (Stratum's `libstratum.a`, libsodium, etc.) make C the right call.
- **Halcyon** owns: the graphical scroll-buffer shell. Rust.
- **Compat layer** owns: musl libc port, Linux ARM64 syscall translation shim. Mostly C99 (musl is C); the shim is C99.
- **Stratum** owns: storage. Runs as a userspace 9P server. Already-feature-complete C99 + 9P2000.L + Stratum extensions.

---

## 3. Language and toolchain

**STATUS**: COMMITTED

| Layer | Language | Rationale |
|---|---|---|
| Kernel | **C99** | Total control, no hidden runtime, all prior kernel art is C. Matches Stratum's heritage. SOTA C99 with sanitizers + formal specs is sound (seL4 precedent). |
| Userspace drivers | **Rust** | Drivers handle hardware DMA descriptors, parse 9P, manage BURROW ref-counts — exactly the domains where Rust's borrow checker eliminates classes of bugs. |
| Halcyon | **Rust** | Parses bash-subset syntax, decodes images, renders fonts, manages a stateful UI — historically high CVE density in C; Rust eliminates UAF + buffer overflow at compile time. |
| Userspace services (network stack, video player, etc.) | **Rust** | Same rationale as drivers. |
| Utopia native programs (`ut` shell, coreutils, `libutopia`) | **Rust on `libthyla-rs`** (no_std, no Pouch) | Authored within the project; speaks Thylacine syscalls directly. The native side of the Plan 9 split (§3.5). |
| POSIX-compat synthetic FS | **C99** | Small servers wrapping existing C libraries (libfuse-9p, etc.); Rust would add crate-build complexity for little gain. Rewrite candidates for v2.0. |
| Ported foreign programs (Helix, stratumd, libsodium, future ssh/git/python) | **C or Rust on Pouch** | Adapted to Thylacine via pouch's boundary-line patches. The ported side of the Plan 9 split (§3.5). |
| musl libc port | **C99** | musl is C; we adapt its arch layer, not rewrite. |
| Compat syscall shim | **C99** | Lives in the kernel; matches kernel toolchain. |
| TUI tooling, build helpers | **Rust** | Already established by Stratum's TUI (Ratatui). |
| Build system | **CMake** (kernel) + **Cargo** (Rust) | Matches Stratum's toolchain; well-understood; reproducible builds. |
| Bootloader | **EDK2 UEFI** (QEMU) / **U-Boot** (bare metal Pi 5+) | DTB-driven; bootloader is interchangeable; the DTB contract is fixed. |
| Initramfs format | **cpio** | Linux convention; well-tooled; portable. |

**Rationale for the C99/Rust split**: the kernel's `no_std` + no global allocator + no unwinding constraints make Rust friction non-trivial; a disciplined C99 kernel with sanitizers + spec-first verification + adversarial audit is sound (seL4 is C). Userspace doesn't have those constraints; Rust's safety pays directly.

**Rust-port-friendly C99 discipline** (the v2.0 escape hatch): kernel C99 code is written so a v2.0 module-by-module Rust port is mechanical:
- Clear interface boundaries — every subsystem has a single header that defines its public surface.
- No `goto fail`-style multi-target gotos that resist Rust's control flow.
- No `#define`-based macro magic that resists Rust syntax.
- Separation of policy (algorithm) from mechanism (data layout).
- Idiomatic data structures (struct-with-functions, not OO with `.next` chained virtuals).

This discipline is enforced by the audit rounds; deviations get flagged.

**No C++ anywhere.** No exceptions, no vtables, no hidden allocations. C++'s ABI fragility is incompatible with the kernel/userspace boundary.

**Kernel uses Clang** (not GCC) for: CFI support (`-fsanitize=cfi`), better ARMv8.5 support (BTI, MTE), better static analysis. Clang version pinned per release; `clangd` for development tooling.

### 3.5 The Plan 9 split: native vs ported userspace

**STATUS**: COMMITTED — scripture under U-1 (the Utopia scripture commit).

Every Thylacine userspace program is in one of two camps; the boundary determines the runtime substrate it builds against.

**Native code** — programs authored within Thylacine — builds against `libthyla-rs` (the existing no_std Rust crate at `usr/lib/libthyla-rs/`). Uses Thylacine syscalls directly. Speaks Thylacine concepts natively (Spoor, KObj_*, notes, capabilities, Territories). No musl. No POSIX shim. No Pouch boundary-line patches.

Native programs include: `ut` (the shell), `libutopia` (the shared Rust library), the coreutils (cat / ls / echo / grep / sed / awk / etc.), corvus, the userspace virtio-* drivers, hello-rs / mmio-probe / irq-probe / virtio-blk-* / virtio-net-* / virtio-input / virtio-gpu, and future Thylacine-shaped daemons and tools the project itself authors.

**Ported code** — foreign code adapted to Thylacine — builds via Pouch (the cross-compilation environment from execution Phase 6; binding design `docs/POUCH-DESIGN.md`). Uses musl + the `usr/lib/pouch/patches/*` boundary-line patches that bridge musl syscalls to Thylacine syscalls and adapt POSIX semantics (notes-as-signals, SrvConn-as-AF_UNIX, etc.).

Ported programs include: stratumd, libsodium, the pouch-hello-* probing binaries, **Helix** (the default `$EDITOR`, ported via Pouch under U-Helix), and future ports of foreign programs (ssh, git, python, etc.).

**The decision rule.** When a new program is added: ask whether the program is **authored within Thylacine** (native libthyla-rs, no Pouch) or **ported from elsewhere** (Pouch). Code review enforces; `tools/build.sh` has separate build paths for the two camps. The rationale mirrors Plan 9's `libc.h` (native) / APE (POSIX-compat ported) split: native programs benefit from being Thylacine-shaped — smaller binaries, faster startup, no impedance mismatch, fewer patches to maintain — while ported programs get POSIX-shape via the pouch boundary-line, which is the right place to do the translation work once per surface rather than at every program's syscall site.

Full scripture: `docs/UTOPIA-SHELL-DESIGN.md §3`.

### 3.6 Capability-scoped service storage

A system service reaches persistent storage through a **storage-root capability** rather than an ambient path. The spawner hands the service a `KObj_Spoor` for its storage subtree — endowed at spawn like fd 0/1/2 but for state — and the service performs *all* persistence relative to that handle. The handle's rights and the subtree it names bound what the service can do and where; there is no path the service can name that escapes its granted storage. This is the storage counterpart to the native-vs-ported program model (§3.5): both give a program exactly the shape — and here, exactly the authority — it should have, by mechanism rather than policy.

**Invariant I-23**: a service's filesystem authority is bounded by the storage capability it is handed. A service granted a storage-root Spoor with rights R reaches only that Spoor's subtree, at only rights R; it holds no ambient FS authority beyond it. Authority is **monotonic** — any delegate (e.g. a child the service spawns) is `<=` R and confined to the same subtree. `RIGHT_TRANSFER` is withheld at grant as least-authority hardening, reserved for the Phase-5+ cross-Proc 9P-transfer surface; at v1.0 it does **not** gate spawn-fd endow or `handle_dup`, so a grantee may still delegate *within its grant* to its own spawned children — which is sound (the delegate stays `<=` R, same subtree). (A-1.7 audit F1: the earlier claim that withholding `RIGHT_TRANSFER` "blocks re-handing" was false; the security-bearing property is the monotonic bound, which holds.)

**Mechanism.** The substrate composes existing primitives plus one small FS primitive (**FS-delta**, the `T_OPATH` walk-without-open mode; `IDENTITY-DESIGN.md §9.4`):
- the spawner builds the storage directory (`mkdir -p` via `SYS_WALK_CREATE`) and obtains a **non-opened, walkable** handle to it via `SYS_WALK_OPEN` with `T_OPATH`. FS-delta is load-bearing: a normally-opened handle is NOT a valid walk/create base (9P forbids `Twalk` from an opened fid), so without it the capability could not create files;
- it reduces the handle to the grant rights with `handle_dup(h, R|W)` — the subset check in `kernel/handle.c` is the I-6 enforcement point (a delegate can never exceed R); dropping `RIGHT_TRANSFER` is least-authority hardening for the Phase-5+ 9P-transfer surface — note it does **not** block spawn-fd endow at v1.0, so the grantee may still delegate within its grant to its own children (sound: monotonic, same subtree). FS-delta now also returns `T_OPATH` handles born `R|W` (no `TRANSFER`);
- it endows the reduced handle as a child fd at spawn (the existing `t_spawn_with_perms` / `t_spawn_with_fds` fd array; the child's `handle_alloc` preserves I-6 monotonic reduction);
- the service **`SYS_CHROOT`s to the handed (non-opened) handle** as its FIRST action, so its filesystem world *is* the capability. Confinement is established by that first chroot (it displaces the broad root the child inherits via `territory_clone`, before any FS access); post-chroot the service's root IS the capability, `walk_open` rejects `..`, and it holds no Spoor outside the cap. (A-1.7 audit F2: confinement at v1.0 is thus *cooperative* — the service chroots itself first — not spawner-set; a spawner-set-root variant, the child born with root = the capability and no ambient window at all, is the v1.x mechanism-enforced form.) It then creates/reads its state at `FROM_ROOT` (now the capability) and may `SYS_WALK_CREATE` children (the chrooted root is a non-opened walkable base);
- `SYS_MOUNT`-into-Territory at a named mount point (e.g. `/state`) is the alternative composition for a service that must keep a broader root (a v1.x ergonomic); chroot-to-capability is the v1.0 default for a single-storage daemon like corvus.

**Lineage + position.** This is the fusion of Thylacine's two heritages. Plan 9 gives the per-process namespace (Territory) but still *names* its mounts; the capability microkernels — Fuchsia (routed directory capabilities) and Genode (per-component `File_system` sessions) — give the handed capability but have no namespace to compose it into. Thylacine does both, over 9P + Spoor: the storage capability can be backed by *any* 9P server (Stratum, a synthetic fs, another process's served tree), is endowed at spawn, and is mountable into a Plan 9 Territory. NOVEL.md §3.10 (lead angle #10) records the position; the convergence-detour sub-chunk that builds it is A-1.7 (`docs/detour-status.md`); corvus is the first consumer (its identity DB + key wraps live in its handed storage capability).

**v1.0 scope.** Build the mechanism + pin the convention; prove on corvus. A general service-storage abstraction waits for a second consumer.

---

## 4. Target architecture

**STATUS**: COMMITTED

### 4.1 Primary target: ARM64 (AArch64)

- **EL0**: userspace. **EL1**: kernel. **EL2**: hypervisor (available, not used at v1.0). **EL3**: secure monitor (not used).
- ARM64 exception model used directly: `SVC #0` for syscall entry, exception vectors for interrupts and faults, `BRK` for kernel breakpoints.
- **Pointer Authentication (PAC)** enabled for kernel return addresses where hardware supports it (v8.3+). All kernel function returns are PAC-signed; tampering panics cleanly.
- **Memory Tagging Extension (MTE)** — **deferred to Phase 8** (reconciled 2026-05-28 per IDENTITY-DESIGN.md §8.4; NOT enabled at v1.0). The hardware feature is *detected* (`hwfeat.c`) but not programmed; enabling needs SLUB tag-aware integration. When on (v8.5+): kernel + userspace heap allocations MTE-tagged, UAF / overflow caught at hardware speed.
- **Branch Target Identification (BTI)** enabled for kernel and userspace (v8.5+). Indirect branches must land on a `BTI` instruction; deviations panic.
- **Large System Extensions (LSE)** atomics used where hardware supports (v8.1+; standard on Apple Silicon). Runtime-detected via `HWCAP_ATOMICS`; LL/SC fallback for older hardware.
- No x86-64 support at v1.0. Architecture-specific code lives in `arch/arm64/`.

### 4.2 Development machine: QEMU `virt`, ARM64

QEMU `virt` machine under Hypervisor.framework on Apple Silicon. No instruction translation; near-native performance.

**QEMU `virt` device inventory** (what the kernel must support at v1.0):

| Device | Bus | Use |
|---|---|---|
| VirtIO block | VirtIO-MMIO / VirtIO-PCI | Storage |
| VirtIO net | VirtIO-MMIO / VirtIO-PCI | Network |
| VirtIO GPU (virgl) | VirtIO-PCI | Framebuffer + 2D for Halcyon |
| VirtIO input | VirtIO-MMIO | Keyboard, mouse |
| GIC v2/v3 | ARM system | Interrupt controller (autodetect via DTB) |
| PL011 | ARM system | UART (early boot, debug) |
| ARM generic timer | ARM system | Tickless scheduling |
| PSCI | ARM system | CPU on/off, reset, shutdown |

### 4.3 Bare metal Apple Silicon (post-v2.0)

Deferred. Depends on:
- Asahi Linux's m1n1 bootloader.
- AIC interrupt controller support (Asahi's reverse-engineered driver as reference).
- AGX GPU driver (Asahi's, via LinuxKPI shim or full reimplementation).

Not a v1.0 commitment. When pursued, `arch/arm64/apple/` is the home.

### 4.4 First bare metal target (post-v1.0): Raspberry Pi 5

Post-Phase 8. Pi 5 chosen because:
- GIC-400: identical to QEMU `virt` — driver transfers directly.
- PL011 UART: identical to QEMU `virt` — driver transfers directly.
- ARM generic timer: identical — driver transfers directly.
- VideoCore VII + mailbox: framebuffer via mailbox, same `/dev/fb/` 9P interface.
- RP1 datasheet published; Linux driver open source.
- Network boot via TFTP: fast iteration loop without SD card swapping.
- Pi 5 + M.2 NVMe HAT: Stratum on real NVMe hardware.

The delta from QEMU `virt` to Pi 5 bare metal: EL2→EL1 drop sequence, mailbox framebuffer driver, RP1 Ethernet driver for network boot. Estimated: one focused sprint after Phase 8 stabilizes. See `ROADMAP.md §12.1`.

> **Reconciled 2026-05-30 by the Lazarus portability arc (`docs/PORTABILITY.md`).**
> Two corrections from the HW-accel assessment: (1) "GIC-400 identical to QEMU
> `virt`" is **wrong** — QEMU `virt` is **GICv3**; GIC-400 (both Pi 4/400 and
> Pi 5) is **GICv2** (a distinct CPU-interface: MMIO `GICC_*`, not `ICC_*`
> sysregs). The kernel's GICv3-only driver currently extincts on v2; a GICv2 path
> is owed (Lazarus W2). (2) The **first board is reconciled to Raspberry Pi 400**
> (Cortex-A72, ARMv8.0-A) and the **compile target to the v8.0 floor** — the
> strict common subset of A72 / A76 (Pi 5) / Apple M-series, so one binary runs
> on QEMU-TCG, QEMU-HVF-on-Apple, and bare metal. Pi 5 (A76/v8.2) is covered by
> the same v8.0 base as a secondary target. The hardening posture (PAC/BTI/LSE)
> is **runtime-conditional** (best-effort on capable hardware) — see
> `PORTABILITY.md §4` + the §15.5 / §24.4 reconciliation landed at Lazarus W1.
> **Sequencing: after the identity detour (A-2..A-5).**

### 4.5 RISC-V (long horizon, v2.x)

Post-v1.0. Target: first RVA23-compliant SBC with PCIe and NVMe (Milk-V Jupiter 2 or equivalent). The kernel port from ARM64 to RISC-V is mechanical above `arch/`: swap GIC for PLIC, ARM CSRs for RISC-V CSRs, ARM generic timer for SBI timer. All territory, scheduler, 9P, BURROW, and handle code is architecture-independent and transfers without modification.

x86-64 is also v2.x. Both ports get clean abstractions because v1.0's ARM64-only design isn't contaminated by the older architecture.

---

## 5. Boot sequence

**STATUS**: COMMITTED

### 5.1 Boot chain

```
EDK2 UEFI (QEMU) or U-Boot (bare metal)
    ↓ loads kernel ELF, sets up flat identity mapping for low region,
    ↓ passes DTB pointer in x0
arch/arm64/start.S
    ↓ at EL1: read DTB pointer, initialize early stack
    ↓ enable MMU with TTBR0 (identity) + TTBR1 (kernel high half)
    ↓ apply KASLR offset to kernel virtual base
    ↓ jump to C: boot_main()
boot_main()
    ↓ BSS clear, early kmalloc bootstrap
    ↓ DTB parse: extract memory regions, GIC base, UART base, timer IRQ
    ↓ phys_init(): buddy allocator over discovered RAM
    ↓ vm_init(): full VM map; switch off identity mapping for low half
    ↓ trap_init(): exception vectors, IRQ dispatch
    ↓ arch_init(): PAC, MTE, BTI, LSE detection + enablement
    ↓ sched_init(): per-CPU run queues, idle threads
    ↓ dev_init(): kernel-internal Devs (cons, null, zero, random, proc, ctl, ramfs)
    ↓ proc_init(): bootstrap proc (PID 0); idle thread
    ↓ irq_init(): GIC distributor + CPU interface; attach timer IRQ
    ↓ mount_initramfs(): cpio archive at /; contains stratumd-system, joey, system.key
    ↓ create init process; exec("/sbin/joey")
    ↓ idle loop: WFI on each CPU
init (userspace, /sbin/joey) — full sequence in CORVUS-DESIGN.md §3 D4
    ↓ fork stratumd-system (file-key from ESP; verifies pool serial per CORVUS-DESIGN.md C-14)
    ↓ kernel-side 9P mount /sysroot via stratumd-system's Spoor
    ↓ pivot root to /sysroot; explicit_bzero the in-memory system.key
    ↓ start /sbin/corvus (Thylacine's key agent; CORVUS-DESIGN.md)
    ↓ start /sbin/login (or Halcyon login at Phase 8)
    ↓ (per-user stratumd processes spawned on login; CORVUS-DESIGN.md §5.1)
```

### 5.2 Bootloader contract

The kernel sees a DTB regardless of bootloader. EDK2 UEFI on QEMU is the path of least friction (it's QEMU's default for ARM64); U-Boot is the path of least friction for bare-metal Pi 5+. **Bootloader is interchangeable; the DTB contract is fixed.**

The kernel never hardcodes peripheral addresses, IRQ numbers, or memory regions. All hardware discovery is via the Device Tree Blob passed by the bootloader. Compatible strings drive driver selection:

```c
/* arch/arm64/dtb.c */
static const struct dtb_match gic_match[] = {
    { "arm,gic-400",  &gic_v2_ops },
    { "arm,cortex-a15-gic", &gic_v2_ops },
    { "arm,gic-v3",   &gic_v3_ops },
    { NULL, NULL }
};
```

This is a hard rule: no `#ifdef QEMU`, no hardcoded MMIO base addresses, no compile-time peripheral configuration. The DTB parser is the single source of hardware truth.

### 5.3 KASLR

Kernel image base is randomized at boot. The bootloader hands control to the kernel at a fixed entry point; `start.S` reads the random seed from the DTB's `/chosen/kaslr-seed` property (UEFI fills this), derives an offset, applies it to the kernel virtual base via TTBR1 setup. The dynamic relocator processes kernel ELF relocations against the offset.

The KASLR seed is passed by the bootloader; if absent, kernel falls back to a low-entropy boot-counter source and logs a warning. Production boot configurations always provide a high-entropy seed.

`/ctl/kernel/base` exposes the runtime base address (privileged read; useful for debugging).

### 5.4 Initramfs

cpio format, passed by the bootloader as a "ramdisk" image (bootloader-specific mechanism: UEFI passes via a config table; U-Boot passes via DTB `/chosen/linux,initrd-start` etc.).

Contents:
- `/sbin/joey` (Phase 4+) or `/sbin/joey-minimal` (Phase 1-3 — UART shell only).
- `/sbin/stratum` — Stratum daemon binary (Phase 4+).
- `/sbin/janus` — key agent binary (Phase 4+).
- `/sbin/busybox` — recovery shell (Phase 5+).
- `/etc/initramfs/{passwd,group,resolv.conf}` — minimal config to boot.
- DTB-fragment overrides if any (rare; mostly platform-specific).

The ramfs is mounted at `/` initially. After Stratum is running, init issues a `unmount("/", "ramfs")` and mounts Stratum at `/`. The ramfs is unreferenced and freed.

### 5.5 Recovery boot

If Stratum mount fails (corrupted volume, missing key, hardware fault), init falls back to BusyBox at `/sbin/busybox` and presents a recovery shell. The user can:
- Examine `/proc/`, `/dev/`, kernel logs in `/ctl/log/`.
- Manually mount Stratum with debug flags, or mount an alternative server.
- Run `stratum-fsck` (Stratum's repair tool, statically linked into the recovery initramfs).

Recovery boot is a designed path, not a degraded mode. It always produces a usable shell.

### 5.6 Open design questions

None at Gate 3. KASLR seed entropy source on bare metal Pi 5+ is a Phase post-v1.0 detail.

---

## 6. Memory management

**STATUS**: COMMITTED

### 6.1 Goals and non-goals

**Goals**:
- 48-bit virtual addressing with split TTBR0 (user) / TTBR1 (kernel).
- 4 KiB pages with 2 MiB block mappings for kernel large-page mappings.
- Demand-zero paging for anonymous mappings (BUILD item per IDENTITY-DESIGN.md §8.4). Copy-on-write for `rfork(RFPROC)` is a **designed seam** — no address-space-sharing caller exists at v1.0 (`RFMEM` extincts); it activates when a `fork`-style caller lands. *(Reconciled 2026-05-28.)*
- SOTA physical frame allocator: buddy + per-CPU magazines (illumos kmem-style) for hot-path lock-freedom.
- SOTA kernel object allocator: SLUB-style (Linux's modern default), better cache locality and lower overhead than slab.
- W^X enforced as page-table invariant.
- KASLR offset applied to TTBR1 region.
- NUMA-shaped from v1.0 (single-socket only at v1.0; multi-socket configuration extension at v2.x).

**Non-goals at v1.0**:
- Swap. Development VMs have enough RAM; swap is post-v1.0 if demand justifies.
- Multi-socket NUMA topology. v1.0 is single-socket.
- THP (Transparent Huge Pages). Large pages used for kernel mappings only.
- Memory hot-add / hot-remove. v1.0 boots with fixed RAM.

### 6.2 ARM64 memory layout

**Decision**: 48-bit VA, 4 KiB granule, 4-level page tables (L0 → L1 → L2 → L3).

```
0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF   User (TTBR0)
                                                ↑ 256 TiB user VA
0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF   Kernel (TTBR1)
                                                ↑ 256 TiB kernel VA

Kernel VA breakdown:
  0xFFFF_0000_0000_0000 – 0xFFFF_7FFF_FFFF_FFFF   Direct map (linear phys)
  0xFFFF_8000_0000_0000 – 0xFFFF_9FFF_FFFF_FFFF   Vmalloc / kmap
  0xFFFF_A000_0000_0000 – 0xFFFF_BFFF_FFFF_FFFF   Kernel modules + KASLR
  0xFFFF_C000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF   Per-CPU + percpu data
```

KASLR randomizes kernel image placement within `0xFFFF_A000_*` region. Direct map and vmalloc regions are fixed (the direct map's virtual base = its physical base + a known offset, simplifying physical address computation).

The kernel image itself is mapped page-grained (4 KiB PTEs, for per-section W^X) across a **4 MiB region** built from two contiguous L3 tables, shared between the TTBR0 identity and TTBR1 high-half walks. The KASLR slide is **4 MiB-aligned** so that 4 MiB region never straddles a 1 GiB (L2-table) boundary — it always maps to two consecutive entries of one L2 table. The slide spans a 16 GiB window in 4 MiB steps: **12 bits** of kernel-base entropy (invariant **I-16**). The region was 2 MiB (a single L3 table) through Phase 4; P5-kernel-l3-4mib widened it to 4 MiB when the UBSan-instrumented kernel image outgrew the 1.5 MiB usable ceiling.

Page table walks via standard ARMv8 PTE format. `nG` bit set in user mappings (no global TLB entry); kernel mappings global with ASID 0.

### 6.3 Physical frame allocator: buddy + per-CPU magazines

**Decision**: classic buddy allocator (Knuth, 1973) for the underlying free-list, with per-CPU magazines (Bonwick & Adams 2001, illumos kmem) layered on top for hot-path lock-freedom.

**Buddy layer** (`mm/buddy.c`):
- Power-of-two free lists from order 0 (4 KiB) to order 18 (1 GiB).
- One buddy zone per NUMA node (single zone at v1.0; multi-zone is configuration).
- Standard buddy split + merge on alloc / free.
- Lock per zone (rarely contested with magazines in front).

**Per-CPU magazine layer** (`mm/magazines.c`):
- Each CPU has a "magazine" — a small stack (default 16 entries) of pre-allocated single-page descriptors at common orders (0, 9 — i.e. 4 KiB and 2 MiB).
- Allocate: pop from local magazine; on empty, refill from buddy (one buddy lock acquisition per refill).
- Free: push to local magazine; on full, drain to buddy.
- Cross-CPU stealing on load imbalance: an idle CPU can steal a magazine from a peer if its own is empty and buddy is contended.

This layering achieves: typical allocation is a stack pop (no lock); typical deallocation is a stack push; buddy lock contention is rare and only for refill/drain.

**Allocator API**:
```c
struct page *alloc_pages(unsigned order, unsigned flags);
void free_pages(struct page *p, unsigned order);
struct page *alloc_pages_node(int node, unsigned order, unsigned flags);

/* convenience for single page */
void *kpage_alloc(unsigned flags);  /* returns kernel direct-mapped VA */
void  kpage_free(void *p);
```

Flags: `KP_ZERO` (zero on alloc), `KP_DMA` (low-32-bit physical), `KP_NOWAIT` (return NULL instead of wait), `KP_COMPLETE` (don't return until refilled if local magazine is empty).

### 6.4 Kernel object allocator: SLUB-style

**Decision**: SLUB-style slab allocator (Linux's modern default since 2008) for kernel object caches. Replaces classical slab (Bonwick 1994); provides better cache behavior and lower per-object overhead.

**Mechanism** (`mm/slub.c`):
- Per-object-class slab pages, each containing N objects of identical size.
- Per-CPU "active slab" pointer; allocations come from this slab without lock.
- Free objects within a slab are tracked via a free-list embedded in the unused object memory (zero overhead per free object).
- When the active slab is full, fetch a new partially-full or empty slab from the per-class partial list.
- When a slab is fully free for some time, return its page(s) to the buddy allocator.
- Per-class debug mode: red zones, poison patterns, allocation/free trace. Compiled out in release builds; on in `slub_debug=1` boot cmdline.

**Standard caches**:
- `proc_cache` (size of `struct Proc`)
- `thread_cache` (size of `struct Thread`)
- `spoor_cache` (size of `struct Spoor`)
- `burrow_cache` (size of `struct Burrow`)
- `handle_cache` (size of `struct Handle`)
- 16 power-of-two general-purpose caches: `kmalloc-8`, `kmalloc-16`, ..., `kmalloc-262144`. `kmalloc(N)` finds the next-larger power of two.

**API**:
```c
void *kmalloc(size_t n, unsigned flags);
void  kfree(void *p);
void *kcalloc(size_t n, size_t size, unsigned flags);
void *kzalloc(size_t n, unsigned flags);  /* zero-initialized */

struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, unsigned flags);
void *kmem_cache_alloc(struct kmem_cache *c, unsigned flags);
void  kmem_cache_free(struct kmem_cache *c, void *obj);
```

### 6.5 Virtual memory and page faults

**User address space management** (`mm/vm.c`):

Each `Proc` carries a page table tree (4-level ARM64). Mappings are tracked in a per-process `struct vma_tree` (red-black tree of `struct vma` describing each mapped region).

```c
struct vma {
    uintptr_t start, end;       /* virtual address range */
    uintptr_t flags;            /* VM_READ | VM_WRITE | VM_EXEC | VM_SHARED */
    union {
        struct {
            void *page_handle;  /* anonymous page state */
        } anon;
        struct {
            struct Burrow *burrow;    /* BURROW-backed mapping */
            uintptr_t offset;
        } burrow;
        struct {
            struct Spoor *spoor;  /* file-backed (post-v1.0) */
            uintptr_t offset;
        } file;
    };
    struct rb_node rb;
};
```

**Page fault handler** (`arch/arm64/fault.c`):
- Demand-zero-pages anonymous mappings on first touch (BUILD item per IDENTITY-DESIGN.md §8.4). *As-built baseline (2026-05-28): `burrow_create_anon` eagerly backs the region and lazily installs PTEs; demand-zero replaces the eager backing.*
- COW on first write to a shared page is a **designed seam, not yet built** — no caller exists (`rfork` `RFMEM` extincts, so no address space is shared at v1.0). Activates when a `fork`-style sharing caller lands. (IDENTITY-DESIGN.md §8.4 SEAM.)
- Translates BURROW-backed faults to BURROW page lookups.
- Panics on kernel faults outside the direct map / vmalloc / fixmap regions (a kernel page fault is a bug, not a recoverable condition).

**User memory growth: the two-tier native interface**

A Proc's address space is established at `exec_setup` (segments + stack) and grown at runtime through a deliberately small native interface. Plan 9 conflated "the heap" (`sbrk`) and "a memory region as a thing" (`segattach`) into segments; Thylacine separates them, because they are different kinds of object.

- **Tier 1 — anonymous regions.** `burrow_attach(length) → vaddr` attaches an anonymous, RW, demand-zero Burrow into the caller's address space at a kernel-chosen address; `burrow_detach(vaddr, length)` releases it (exact-match — no partial detach at v1.0). No handle: private heap memory is not a named, shared, or transferable thing — its owner is the VMA, its name is its address (as Plan 9's `sbrk` / `segattach` returned raw addresses). This is the substrate for both `libt`'s and pouch's `malloc`. It is `segattach` without `brk`'s single-cursor rigidity: many independent regions, individually releasable, kernel-placed (no `MAP_FIXED`). Lifecycle: the audited `burrow_create_anon → burrow_map → burrow_unref` sequence (§19; invariant I-7) — the VMA's `mapping_count` holds the Burrow alive, `handle_count` is 0; `burrow_detach` drops the last `mapping_count` and the pages are freed. VMA-list mutation is serialized under a per-Proc lock (uncontended at v1.0's single-threaded Procs; correct-by-construction when threads land).

- **Tier 2 — Burrow handles.** A Burrow created as a first-class object with a `KObj_Burrow` handle (§11.3) — for *shared*, *named*, or *transferable* memory: handed to a child for shared memory, or sent over a 9P / `/srv` connection. This is the part that goes beyond Plan 9, whose segments could not be passed around. Design intent; lands when a native workload needs it.

`brk` is **not provided** — a single process-global linear cursor is ASLR-hostile, thread-hostile, and cannot host multiple arenas; every modern allocator is arena-based (musl's own `mallocng` treats `brk` as a removable optimization). `mprotect` is **not provided** at v1.0 — Tier 1 is RW, and the only planned W↔X transition is the JIT-future `pkey`-shaped path (§6.6).

**File-backed `mmap` is refused — deliberately, permanently.** A mapped file cannot be 9P-network-transparent: there is no coherent way to demand-page a file served over the network, and a mapping aliases the read/write model 9P composes with. This is Plan 9's deliberate refusal, kept as a Thylacine conviction — "the filesystem is the OS" depends on files being read and written, not mapped. The substitute for working with a large file is read/write against a 9P-served page cache (Stratum's). pouch surfaces file-backed `mmap` as a documented `ENOSYS` (POUCH-DESIGN.md §8.2).

The `struct vma` sketch above predates the Burrow subsystem: the as-built VMA is the sorted, Burrow-backed list in `kernel/vma.c` (see `docs/reference/`), and carries no file-backed variant.

### 6.6 W^X enforcement

W^X is enumerated invariant I-12. Enforcement layered:

- **PTE bit layer**: ARM64 page table entry has separate `AP[2:1]` (access permissions) and `XN` (execute-never) bits. The kernel page table writer rejects any PTE that has both `AP[2:1] != 0b11` (writable) AND `XN == 0` (executable). Compile-time `_Static_assert` on a sanity bit pattern.
- **Memory-syscall layer**: `burrow_attach` (§6.5) produces only RW (never executable) mappings, and v1.0 exposes no `mprotect` — no userspace runtime path can request a W+X page at all. W^X holds by construction on the memory-growth surface.
- **ELF loader layer**: rejects ELF segments with `PF_W | PF_X` flags set together. `.text` segments are RX, `.rodata` is R, `.data` is RW.
- **Audit layer**: `/ctl/security/wx-violations` counter exposed; non-zero implies a kernel bug.

Future JIT compilers (none at v1.0) will get an explicit `pkey_alloc` + `pkey_mprotect`-shaped syscall that enables W↔X transitions under explicit kernel mediation. This is post-v1.0.

### 6.7 NUMA shape (v1.0 single-socket; v2.x multi-socket)

Even at v1.0 single-socket, allocator structures are NUMA-shaped:
- `struct numa_node` exists and contains zone information.
- Buddy allocator queries `current_cpu_to_node()` to pick the local zone.
- Per-CPU magazines are NUMA-local.

At v1.0 there's exactly one node. At v2.x multi-socket, the structure is filled in; allocator preferences apply (local-first, remote-fallback). No allocator code rewrite needed.

### 6.8 Open design questions

- THP / 2 MiB user mappings for hugepage workloads. Post-v1.0; v1.0 user mappings are 4 KiB only. Kernel mappings use 2 MiB blocks where contiguous.
- Memory cgroups-equivalent for territory-level memory accounting. Plan 9 doesn't have this; Linux does (`memory.max`); for the development-VM target, system-wide accounting via `/ctl/mm/` is sufficient at v1.0.

### 6.9 Summary

Buddy allocator + per-CPU magazines for physical frames; SLUB for kernel objects; per-process VMAs in a red-black tree; W^X enforced at PTE bit layer + syscall layer + ELF loader. KASLR randomizes kernel base. NUMA-shaped from v1.0 (single-node).

### 6.10 Capability-based kernel addressing (v2.x direction)

**STATUS**: COMMITTED (v2.x design contract; not v1.0).

The v1.0 kernel direct map (§6.2) is the **pragmatic compromise**, not the principled SOTA. This subsection records the v2.x architectural goal — capability-based kernel addressing — so the v1.0 direct map is explicitly the trade-off rather than implicitly the only choice. Per `NOVEL.md §3.9` Contract D.

**The direct map's "hack-ish" properties** (acknowledged at v1.0):

- Full kernel exposure: every byte of RAM is reachable via constant-offset arithmetic from any kernel code path. A speculative-load gadget anywhere in the kernel can, in principle, read any kernel data — filesystem cache, process credentials, crypto keys, all in one undifferentiated R/W blob.
- Implicit aliasing: the same physical page lives at multiple VAs (direct map at `0xFFFF_0000_*` + kernel image at `0xFFFF_A000_* + KASLR` + user mapping in TTBR0 if applicable). Cache-attribute coherence is the kernel's responsibility.
- No subsystem differentiation: filesystem cache, process credentials, and crypto keys all sit in one R/W mapping.

**SOTA alternatives** (research / production-deployed):

- **seL4** — no kernel direct map. Every kernel data structure is allocated via Untyped → typed capabilities and mapped explicitly. The kernel's address space is minimal; every access is capability-mediated. Formally verified.
- **CHERI** — capability hardware. Pointers carry bounds + type + permission tags enforced by the CPU. Even within a flat address space, kernel code can't fabricate or escalate capabilities. Production-deployed by Arm Morello; Cambridge / SRI's ongoing work.

**Why v2.x, not v1.0**:

- C's type system can't express provably-safe capabilities. `kobj_t` in Thylacine today is `void *` precisely because C bottoms out there. seL4-discipline in C is possible but the safety is by-convention, not enforced.
- A capability-based kernel allocator requires re-architecting buddy + SLUB + every kernel subsystem to consume/produce capabilities. Multi-year effort.
- Rust kernel port is the natural carrier (see §3 Toolchain — "Rust-port-friendly discipline" is already a v1.0 commitment) but the port itself is post-v1.0.
- CHERI / Morello is hardware-conditional; ARM's Morello board is research-grade, not v1.0-shippable.

**What v1.0 commits that survives the v2.x migration**:

- Every direct-map PTE is **R/W + XN unconditionally** — kernel direct map is data, never code. W^X invariant I-12 holds at the alias level: the same physical page mapped R/X via kernel image VA is mapped R/W + XN via direct map; never both R/W and X.
- The kernel allocator's API surface (`kmem_cache_alloc`, `alloc_pages`, `kmalloc`) is structured around cache + size + flags arguments, NOT raw arithmetic on PAs. v2.x can swap the implementation behind it from "raw void * pointers into direct map" to "typed kernel capabilities with explicit bounds + permissions" without rewriting callers.
- Every PA↔KVA conversion is funneled through `pa_to_kva()` / `kva_to_pa()` (added at P3-Bb). v2.x replaces these with capability-derive + capability-extract operations.
- KASLR-randomization of the direct-map base is a v1.x hardening pass. The offset is currently fixed at `0xFFFF_0000_0000_0000`; randomizing it gives speculation-attack mitigation independent of the v2.x capability path.
- MMIO is mapped at vmalloc range `0xFFFF_8000_*` with Device-nGnRnE attributes, NOT via the direct map. This separation already implements one form of "subsystem differentiation."

**What v2.x changes**:

- `kmem_cache_alloc` returns `cap_t obj_cap` instead of `void *obj` (or returns `void *` AS a capability if CHERI hardware is in use; the source-level API stays similar).
- Buddy yields capabilities to physical regions, not pointers to direct-map VAs.
- Kernel subsystems hold typed capabilities to their data structures; access requires explicit unwrap.
- Speculative-load attacks lose their target: even if a Spectre-style gadget exists in the kernel, the direct map doesn't expose all RAM as data.

**Phasing** (post-v1.0):

- v1.x: KASLR-randomize the direct-map base + audit every kernel call site for compliance with the cache+size+flags API surface (no raw PA arithmetic outside `pa_to_kva` / `kva_to_pa`).
- v2.0: Rust kernel port begins. Capability-typed allocator API as the primary surface; `void *` is a thin shim during the port window.
- v2.1+: CHERI / Morello hardware target. Capabilities become CPU-enforced; the C-shim layer is removable.
- v3.x: full capability discipline — kernel-internal addressing parallels handles.tla's user-facing capability monotonicity.

This direction is consistent with Thylacine's existing commitments: handles.tla discipline applied INWARD; SOTA security; verification-driven design. The compromise at v1.0 is honest and recoverable; the principled answer waits for the right tool.

---

## 7. Process and thread model

**STATUS**: COMMITTED

### 7.1 Goals and non-goals

**Goals**:
- Plan 9-style `rfork` as the universal process/thread creation primitive.
- Threads are first-class siblings within a Proc (Plan 9 9Front model). PID = Proc identity; TID = thread within Proc.
- POSIX `pthread_t`, `pid_t` semantics provided by musl translation, mapping cleanly to Thylacine internal.
- Plan 9 notes as the internal asynchronous-message model. POSIX signals provided by translation (musl).
- Per-thread `errstr` buffer (Plan 9 idiom).
- Thread-local storage via ARM64 `TPIDR_EL0`.

**Non-goals at v1.0**:
- Process migration across CPUs at run time as a syscall (kernel-internal load-balancing migrates; user-visible CPU pinning is `/ctl/proc/<pid>/cpu-affinity` post-v1.0).
- Real-time scheduling class. v1.0 has soft latency bounds via EEVDF; hard RT is v2.x.

### 7.2 Process

A `Proc` owns:
- An address space (page table root, `vma_tree`).
- A territory (`Territory` — mount table + per-bind overlays).
- A file descriptor table (open `Spoor`s).
- A handle table (typed kernel object handles).
- A credential set (uid, gid, supplementary groups, capabilities).
- A set of threads (one or more).
- A note queue (pending notes; per-process, delivered to whichever thread has notes unmasked).
- Parent / child relationships (for `wait`).

```c
struct Proc {
    int            pid;
    struct Territory   *territory;
    struct AddrSpace *addr_space;
    struct FdTable *fds;
    struct HandleTable *handles;
    struct Cred    cred;
    struct Thread *threads;          /* doubly-linked list */
    int            thread_count;
    struct NoteQueue notes;
    struct Proc   *parent;
    struct Proc   *children, *siblings; /* for wait */
    int            exit_status;
    /* ... */
};
```

### 7.3 Thread

A `Thread` owns:
- A register context (GPRs, PC, SP, PSTATE — saved/restored on context switch).
- A kernel stack (`THREAD_STACK_SIZE = 16 KiB` default, with a guard page).
- A signal mask (note mask, more precisely — Plan 9 idiom).
- A per-thread `errstr` buffer (`struct errstr { char buf[ERRMAX]; }`).
- The `TPIDR_EL0` register value (TLS pointer; set on context switch).
- Run state: `RUNNING`, `RUNNABLE`, `SLEEPING`, `STOPPED`, `EXITING`.
- Parent `Proc *` pointer.

```c
struct Thread {
    int            tid;
    struct Proc   *proc;
    struct Context ctx;       /* arch-specific saved registers */
    void          *kstack;
    unsigned       state;
    struct EventQueue events;  /* wait/wake for sleep / wakeup */
    struct ErrStr  errstr;
    uintptr_t      tpidr_el0;
    /* EEVDF data */
    int64_t        virtual_eligible;
    int64_t        virtual_deadline;
    int            band;       /* INTERACTIVE | NORMAL | IDLE */
    int            cpu;        /* current or last CPU */
    /* ... */
};
```

### 7.4 `rfork` — process/thread creation

`rfork(flags)` is the universal primitive. Flags control which resources are shared vs. cloned:

| Flag | Meaning |
|---|---|
| `RFPROC` | Create a new Proc (always required for "create"). |
| `RFMEM` | Share address space (creates a thread within current Proc, not a new Proc). |
| `RFNAMEG` | Share territory (default for threads; clone for processes). |
| `RFFDG` | Share file descriptor table (default for threads). |
| `RFCRED` | Share credentials. |
| `RFNOTEG` | Share note queue (default for threads). |
| `RFNOWAIT` | Don't add to parent's children list (orphan from creation). |
| `RFREND` | Share rendezvous space. |
| `RFENVG` | Share environment. |

Examples:
- POSIX `fork()`: `rfork(RFPROC | RFFDG | RFENVG)` — new Proc, copy-on-write address space, clone fd table.
- POSIX `pthread_create()`: `rfork(RFPROC | RFMEM | RFNAMEG | RFFDG | RFCRED | RFNOTEG)` — new thread within existing Proc.
- Plan 9 container: `rfork(RFPROC | RFNAMEG)` — new Proc, fresh territory, copy-on-write address space.
- Capability reduction: `rfork(0)` (no resource flags) — no creation; just reduces capabilities of the calling process. Used for sandboxing.

The musl port's `clone()` syscall translates Linux `CLONE_*` flags into `rfork()` flags; the translation is straightforward and well-defined.

### 7.5 Thread-local storage

ARM64 `TPIDR_EL0` register holds the per-thread TLS base. Saved/restored on context switch (~5 instructions). musl's TLS layout follows the standard ARM64 ABI; the kernel doesn't need to know the layout, only to save/restore the register.

This is a two-line addition to `arch/arm64/context.c` that's easy to forget and breaks all of musl's TLS if missed. Audit-trigger surface; `scheduler.tla` and Phase 2's audit cover it.

### 7.6 Notes — Plan 9's signal model

A note is an asynchronous text message delivered to a process. Standard notes:

| Note | Generated by | Meaning |
|---|---|---|
| `interrupt` | Ctrl-C on cons | Interrupt the foreground process |
| `kill` | `postnote(pid, "kill")` | Terminate (non-catchable) |
| `hangup` | Terminal close | Process should exit |
| `alarm` | `alarm()` syscall | Timer fired |
| `usr1`, `usr2` | User-defined | Generic notification |
| `stop`, `cont` | Job control | Stop / continue |

A process registers a note handler via `notify(handler_func)`. When a note arrives, if a handler is registered and the note is not masked, the kernel:
1. Saves the current thread's user context.
2. Switches to the note handler with the note text in `argv`.
3. The handler returns via `noted(NCONT)` (continue) or `noted(NDFLT)` (default action — usually terminate).

If no handler is registered, the default action is taken (`kill` for most notes; `interrupt` is ignored if no handler).

POSIX signals are translated to/from notes by musl + a thin kernel shim. `kill(pid, SIGINT)` posts the `interrupt` note; the musl signal handler is invoked by the note dispatcher with a fake `siginfo_t`.

Spec: `notes.tla` was the planned formalization. Per CLAUDE.md "Spec-to-code FULLY suspended" (broadened 2026-05-23), the invariants the spec was to prove — delivery ordering (I-19), mask correctness, async-safety (handlers don't fire while the kernel is in a critical section), consumed-exactly-once across handler and fd-read paths — are pinned by the discipline in §7.6.5 + the audit round + the runtime tests.

### 7.6.1 The fd-shaped path  *(novel angle — see NOVEL.md §3.1)*

Notes are **fd-shaped first; async handlers are an explicit opt-in**. Every Proc carries a kernel-owned note Spoor whose read end is mintable into the Proc's handle table via `SYS_NOTE_OPEN()` (idempotent — same fd on every call until the Proc closes it; a subsequent call mints a fresh fd against the same queue). The Spoor is backed by a tiny new Dev (`devnotes`) over the Proc's `struct NoteQueue` — so the queue is the truth; the fd is a view.

The pattern a modern Thylacine daemon writes:

```c
int notes = note_open();                    /* pouch wrapper over SYS_NOTE_OPEN */
struct pollfd pfd[2] = {
    { notes,       POLLIN, 0 },
    { client_fd,   POLLIN, 0 },
};
poll(pfd, 2, -1);
if (pfd[0].revents & POLLIN) {
    struct note_record rec;
    read(notes, &rec, sizeof rec);
    dispatch(rec.name, rec.arg);            /* synchronous; no async-cancel-safety hell */
}
```

Reads from `devnotes` yield fixed-size `struct note_record` (32 bytes — `name[16] + arg + sender_pid + timestamp_ns`). Strongly typed; architecture-independent; no `siginfo_t` ABI nightmare. `read()` returns one record at a time at v1.0 (a multi-record vectored read is a v1.x extension). `poll()` reports `POLLIN` iff the queue has at least one mask-permitted readable note.

This is a deliberate inversion of priority versus Plan 9 (where `/proc/<pid>/note` reads exist but async handlers are the documented default) and Linux (where `signalfd(2)` is an afterthought bolted onto the signal model). Async signal handlers are the **single nastiest part of POSIX signal programming** — only ~30 functions are safe to call in a handler; modern code that genuinely wants robust async event handling either uses `signalfd` or a self-pipe trick. Thylacine commits the fd-shaped path as the documented default; `SYS_NOTIFY`/`SYS_NOTED` exist for compatibility with libcs that insist on the handler model (musl).

### 7.6.2 Kernel data structures

```c
/* kernel/include/thylacine/notes.h */

#define NOTE_NAME_MAX    16u    /* 15 bytes + NUL; bounds the supported set */
#define NOTE_QUEUE_DEPTH 16u    /* per-Proc; overflow → -EAGAIN at SYS_POSTNOTE */

#define NOTE_BIT_INTERRUPT   0u
#define NOTE_BIT_KILL        1u
#define NOTE_BIT_PIPE        2u
#define NOTE_BIT_CHILD_EXIT  3u
#define NOTE_MASK_SUPPORTED  0x0fu   /* grows as supported notes grow */

struct Note {
    char name[NOTE_NAME_MAX];   /* NUL-terminated note name */
    u32  arg;                   /* small int slot (child_exit packs pid+status) */
    u32  sender_pid;            /* posting Proc's pid; 0 for kernel-synthetic */
    u64  timestamp_ns;          /* monotonic kernel time at post */
};

struct note_record {            /* SYS_NOTE_OPEN read-side wire format */
    char name[NOTE_NAME_MAX];
    u32  arg;
    u32  sender_pid;
    u64  timestamp_ns;
};
_Static_assert(sizeof(struct note_record) == 32, "ABI-pinned");

struct NoteQueue {
    spinlock_t       lock;
    u32              head, tail, count;
    struct Note      ring[NOTE_QUEUE_DEPTH];
    struct Rendez    waiters;   /* devnotes_read parks here */
};
```

In `struct Proc`:
- `struct NoteQueue notes;`
- `u64 handler_va;` — registered async handler (per-Proc; 0 = no handler; inherited across `rfork(RFPROC)`).

In `struct Thread`:
- `u64 note_mask;` — bit-per-supported-note; bit set = deferred to this Thread.
- `bool in_handler;` — re-entrancy guard.
- `u64 note_saved_pc, note_saved_sp;` — handler-restore cross-check (the full `ureg` is on the user stack).

The `RFNOTEG` rfork bit (already reserved in `proc.h`) selects share-vs-copy of the queue. v1.0 always copies (each Proc gets its own); the share semantics land when rfork is properly used (Phase 7).

### 7.6.3 Syscall surface

| # | Syscall | Args | Returns |
|---|---|---|---|
| `SYS_NOTE_OPEN` | (none) | fd of the calling Proc's note Spoor read end |
| `SYS_NOTIFY` | `handler_va` | 0 / -EINVAL — register the async handler (0 clears) |
| `SYS_NOTED` | `arg` (0 = NCONT, 1 = NDFLT) | NEVER RETURNS NORMALLY (or -EINVAL if outside a handler) |
| `SYS_POSTNOTE` | `pid`, `name_va`, `name_len` | 0 / -EINVAL / -EPERM / -ESRCH / -EAGAIN |
| `SYS_NOTE_MASK` | `new_mask`, `old_mask_out_va` | 0 / -EINVAL |

**`SYS_POSTNOTE` permission gate at v1.0**: caller must be the target's parent OR `pid == self_pid`. **A-4b (IDENTITY-DESIGN §9.8) generalizes cross-process kill** via `write "kill"`/`"killgrp"` to `/proc/<pid>/ctl`, two-axis-authorized (owner-rwx on the ctl file OR `CAP_HOSTOWNER` OR the new elevation-only `CAP_KILL`); `SYS_POSTNOTE` stays the parent/self fast path. The general `/proc/<pid>/note`-file form (posting *arbitrary* notes by namespace write-rights on the Spoor) remains the later composition (§7.6.8 [OPEN Q 7.6.A]).

### 7.6.4 Delivery discipline

Two delivery paths consume the same queue; **every posted non-`kill` note is consumed exactly once by either the async-handler path or the fd-read path.** Mutual exclusion is enforced by the queue lock: a `peek + pop` under `notes.lock` is the consume.

**Fd-read path** (`devnotes_read`):
1. Acquire `notes.lock`.
2. Walk the queue head-to-tail, picking the first note whose `NOTE_BIT_*` is **not** set in the calling Thread's `note_mask`.
3. If found: copy to a `struct note_record`; remove from the queue; release lock; copy out to user buffer (uaccess with fault fixup).
4. If empty (or all masked): release lock; `tsleep` on `notes.waiters` (with optional non-blocking via `O_NONBLOCK` on the fd — deferred to v1.x; v1.0 always blocks).

**Async-handler path** (at EL0-return tail in `arch/arm64/exception.c::exception_sync_lower_el`):
1. Acquire `notes.lock`. If `t->in_handler` is true, release and continue (no nested delivery).
2. Walk the queue; pick the first non-masked note. If none, release and continue.
3. Pop the note. If `name == "kill"`: release lock; call `exits("killed")` (non-catchable; bypasses handler regardless of registration).
4. If `p->handler_va == 0`: this note has no handler; release lock; apply default action — for the v1.0 set, `exits(name)`.
5. Otherwise: cache `t->note_saved_pc = ctx->elr; t->note_saved_sp = ctx->sp_el0; t->in_handler = true;` — release lock.
6. Push a `struct ureg` + the note name onto the user stack (sp aligned to 16; sp -= sizeof(ureg) + NOTE_NAME_MAX, with red-zone allowance).
7. Mutate the exception context: `elr = handler_va; regs[0] = ureg_va; regs[1] = note_name_va; sp_el0 = adjusted_sp`.
8. `eret` normally; the handler runs in the same Thread's user context.

**`SYS_NOTED(arg)`**:
- arg == 0 (NCONT): validate `t->in_handler == true`; restore `ctx->elr` and `ctx->sp_el0` from the user-stack `ureg` (via uaccess) — the cached `note_saved_pc/sp` provide cross-checks against tampering; clear `in_handler`; eret to the restored user state.
- arg == 1 (NDFLT): take the note's default action (for the v1.0 set, `exits(name)`).
- Anything else → `-EINVAL`.

**Async-safety**: delivery happens only at the EL0-return tail, after `regs[0]` is written and before `eret`. At that point the kernel holds no locks (a syscall acquires-then-releases all locks within its body; an IRQ-return is similar). Handlers therefore can never observe a partial-kernel state. The lock discipline in steps 1-5 above ensures the queue-mutation is atomic w.r.t. concurrent fd-reads from another Thread.

**Multi-thread Procs (post sub-chunk 9a)**: notes are Proc-directed. The EL0-return-tail check fires on each Thread's return; first-eligible Thread wins the delivery (its mask is the gate). If every Thread has the note masked, it stays queued; delivery happens when any Thread clears its mask (the `SYS_NOTE_MASK` syscall re-pumps before returning).

**`kill` is non-catchable**: bypasses mask, handler, and fd-read. Posting `kill` to a Proc terminates it immediately at the next return-to-EL0 (mirrors SIGKILL).

**Queue overflow**: `SYS_POSTNOTE` returns `-EAGAIN` if the queue is full. Kernel-synthetic posters (`exits` for child_exit, pipe-write for `pipe`) coalesce same-name notes when over a soft threshold (e.g., queued `child_exit` count > 4) to keep synthetic delivery infallible. Coalescing preserves the most recent `arg` (latest child).

### 7.6.5 Standard notes (v1.0 supported set)

| Note | NOTE_BIT | Source | Catchable? | Default action |
|---|---|---|---|---|
| `interrupt` | 0 | `SYS_POSTNOTE`; future cons ^C | yes | `exits("interrupt")` |
| `kill` | 1 | `SYS_POSTNOTE` | **no** | `exits("killed")` — handler/mask bypassed |
| `pipe` | 2 | kernel-synthetic on write to closed pipe peer | yes | `exits("pipe")` (musl can `SIG_IGN` via mask) |
| `child_exit` | 3 | kernel-synthetic on child `exits()` | yes | `exits("child_exit")` (most Procs handle it) |

`alarm`, `hangup`, `usr1`, `usr2`, `stop`, `cont` — DEFERRED at v1.0; each needs an additional kernel hook (`SYS_ALARM`, cons close path, scheduler stop/cont). `sigaction` on a deferred signal returns `EINVAL`. The supported set can grow per chunk without ABI break.

### 7.6.6 Synthetic posting hooks (v1.0)

- `kernel/proc.c::exits()` — posts `child_exit` to `p->parent->notes` with `arg = (child_pid << 16) | (exit_status & 0xffff)`. Tolerant of queue-full via coalesce.
- `kernel/pipe.c` write-path — when writing to a pipe whose read end is closed, posts `pipe` to the writing Thread's Proc; the write itself still returns `-EPIPE`. Tolerant of queue-full via coalesce.

### 7.6.7 Invariants

- **N-1 (queue ordering)**: notes are consumed in post order per source. I-19 (causal order within a process) refines to this: each posting source (a specific `SYS_POSTNOTE` caller, the synthetic-`child_exit` hook on `exits`, the synthetic-`pipe` hook) sees its posts consumed in order; ordering between distinct sources is `tsc`-stamped (timestamp_ns is the tiebreaker).
- **N-2 (consumed exactly once)**: every posted note enters the queue once and is consumed by exactly one path — either the async-handler EL0-return-tail consume, or the `devnotes_read` consume. `kill` is the exception (immediate exits regardless of consume path).
- **N-3 (handler re-entrancy)**: while `t->in_handler == true`, no further note is delivered to that Thread. Queue state stays consistent across handler nest depth = 1.
- **N-4 (`kill` non-catchable)**: a `kill` note terminates the target Proc at the next EL0-return, regardless of mask, handler registration, or in_handler state. For a multi-thread Proc, `kill` invokes `proc_group_terminate` (§7.9.1) so *every* Thread reaches its EL0-return death-checkpoint — not just the one that consumes the note (SYS_EXIT_GROUP, task #809; closes the prior `kill → -EIO in multi-thread Proc` refusal, 13b R1-F9).
- **N-5 (fd lifecycle)**: a closed note Spoor fd does not affect future `SYS_NOTE_OPEN` calls or queue state. The queue lives with the Proc.

### 7.6.8 Open design questions

- **Cross-Proc posting via namespace**: **A-4b (IDENTITY-DESIGN §9.8) lands the `/proc/<pid>/ctl` `kill`/`killgrp` form** (authority = owner-rwx on the ctl file -- the two-axis gate). The remaining open part is a general `/proc/<pid>/note` file for posting *arbitrary* notes by namespace write-rights (vs the `SYS_POSTNOTE` syscall shortcut). Tracked in §7.6 [OPEN Q 7.6.A].
- **`CAP_KILL` -- RESOLVED (A-4b, 2026-06-01; IDENTITY-DESIGN §9.8)**: an *elevation-only* `CAP_KILL` is the cross-identity override on the `/proc/<pid>/ctl` kill gate (the third axis beside owner-rwx + `CAP_HOSTOWNER`), acquired via clearance-activation through the `cap` device, rfork-stripped so it never leaks to a legate's children. **[OPEN Q 7.6.B -- CLOSED]**.
- **Vectored read on devnotes**: a single `read(notes, &arr, sizeof(arr))` returning multiple records would let event loops drain efficiently. v1.x. **[OPEN Q 7.6.C]**.
- **`stop`/`cont`**: needs scheduler integration (the `stop` note suspends run-tree eligibility). Phase 7 work. **[OPEN Q 7.6.D]**.

### 7.7 Capability set

Each `Proc` has a coarse-grained capability bitmask:

| Capability | Allows |
|---|---|
| `CAP_BIND_PRIV` | Bind to privileged ports (< 1024) |
| `CAP_HW_HANDLE` | Receive hardware handles at process creation (driver bit) |
| `CAP_RAW_MEM` | Open `/dev/mem` for physical memory access |
| `CAP_PTRACE` | Debug another process |
| `CAP_NS_ADMIN` | Modify territory beyond own (e.g., post-mount /proc) |
| `CAP_NET_ADMIN` | Configure network interfaces |
| `CAP_SYS_ADMIN` | Mount filesystems other than into own territory |

Capabilities can only be reduced via `rfork`, never elevated — with two sanctioned exceptions: the v1.0 `cap` device, which confers the elevation-only `CAP_HOSTOWNER` on a console session (§15.3; CORVUS-DESIGN.md §5.5.1), and the v2.0 factotum-mediated per-syscall capability elevation (§15.4 — designed-not-implemented at v1.0).

Default at process creation: empty set. Drivers receive `CAP_HW_HANDLE` from kernel at startup; they immediately drop after retrieving their handles.

### 7.8 `errstr` and error reporting

Every thread has a per-thread `errstr` buffer (Plan 9 idiom). System calls returning failure also set this buffer with a textual description:

```c
errstr("territory cycle: cannot bind /a/b onto /a (would create loop)");
return -1;
```

The user retrieves it via `errstr(buf, sz)` syscall. POSIX errno is provided by musl's translation: each Plan 9 errstr maps to an errno (`-EEXIST`, `-EINVAL`, etc.).

This is richer than POSIX errno alone and is preserved as a Plan 9 idiom worth keeping. errstr text is also logged at `/ctl/log/errstr/<pid>` for debugging.

### 7.9 Process exit and wait

`exits(msg)` terminates the calling process. `msg` is a text status string ("ok" for clean exit, anything else for failure). Goes into `Proc->exit_status`; parent retrieves via `wait()`.

`wait(*WaitMsg)` blocks until a child exits; returns child's PID + exit_status. If multiple children exit, they're queued; `wait` returns oldest first. POSIX `waitpid` translates straightforwardly.

When a process exits:
1. All threads stopped.
2. All open Spoors clunked.
3. All handles closed.
4. Address space torn down.
5. Territory dropped.
6. Children re-parented to PID 1 (init).
7. Notes drained.
8. Parent notified (synthetic note-equivalent if waiting).
9. `Proc` structure freed via slab cache.

Spec: `notes.tla` covers the exit-during-note-delivery race (note arrives just as process is exiting; handler is no longer valid).

### 7.9.1 Group termination — `exit_group`, `kill`, and cross-thread shootdown

**STATUS**: IMPLEMENTED — `SYS_EXIT_GROUP`, task #809, `89456e9`. The universal death-interruptible-sleep completion (which resolves the v1.0 residual at the end of this section) is DESIGNED in this commit and implemented as task #811.

A multi-thread Proc (peer Threads via `SYS_THREAD_SPAWN`, §7.4 / P6-pouch-threads §9) needs a way to terminate the *whole* Proc when one Thread declares program-wide exit (`exit_group(2)`), or when an external `kill` targets it. At P6-pouch-threads v1.0 neither existed: `exits()` **extincts the kernel** if the Proc still has live peer Threads ("exits with live peer threads"), and `SYS_POSTNOTE("kill")` **refuses** a multi-thread target (the documented `kill → -EIO`, 13b R1-F9). `_Exit` / `abort` / a mallocng assert in a multi-thread pouch Proc (stratumd at shutdown) routes through `__NR_exit_group → SYS_EXITS` and trips the extinction intermittently — the `tools/test.sh` flake surfaced as #808 audit F3.

**The model — flag-and-self-terminate at the EL0-return checkpoint.** Prior art converges: Plan 9 (`Proc_exitme` set via `postnote`, noticed at the next trap), Linux (`zap_other_threads` + `kick_process`; `do_group_exit` flags siblings then self-`do_exit`s; the last Thread out — `atomic_dec_and_test(&sig->live)` — does the group cleanup), and Zircon (`THREAD_SIGNAL_KILL` + IPI-to-checkpoint) all **flag each peer and let it kill itself** at its next return-to-EL0; the IPI is a latency accelerant, never a synchronous stop. seL4's synchronous cross-core barrier-stall is the outlier — affordable only with a big kernel lock and no in-kernel process; it does not fit Thylacine's fine-grained-locked monolith. Thylacine adopts the convergent model (its Plan 9 lineage *is* this model).

**Mechanism (as built, `89456e9`).** `proc_group_terminate(p, msg)` — the unified primitive — publishes a single per-Proc word and wakes/kicks every peer toward its EL0-return checkpoint. The implementation deliberately collapsed the design's per-Thread/per-status bookkeeping to one set-once flag:
1. **CAS-publishes `group_exit_msg`** (NULL-sentinel, set once, `__ATOMIC_RELEASE`). This single word *is* both the "group is exiting" flag AND the last-Thread-out exit status (`"ok"` → 0, anything else → 1) — there is no separate `group_exiting` bool, no per-Thread `die_requested` flag, no `group_exit_status` field. A Thread is "flagged to die" iff its Proc's `group_exit_msg` is non-NULL; each peer's checkpoint reads the Proc word directly. The CAS makes a concurrent second `exit_group` / `kill` a no-op (first writer wins → exactly-once).
2. **Wakes** each sleeping peer so it returns toward its EL0-return tail: `torpor_wait` sleepers via `torpor_wake_all_for_proc(p)`; **and (task #811) every other rendez sleeper** — `poll`, `pipe`, `devnotes_read`, 9P RPC, `wait_pid`, any `sleep` / `tsleep` — via the universal death-interruptible-sleep wake (§8.8.1), which makes the wake *total* rather than class-by-class.
3. **Broadcasts `smp_resched_others()`** — one `IPI_RESCHED` to every other CPU, not a per-Thread targeted IPI. Broadcast is a correct, simpler superset: any peer RUNNING on another CPU traps and reaches its checkpoint; a CPU with no flagged Thread simply re-runs the scheduler and continues. No per-Thread `cpu` field is needed.

Each flagged Thread, on its next **EL0-return tail**, observes its Proc's non-NULL `group_exit_msg` (via `el0_return_die_check`) and runs the group-exit Thread-exit path on *itself* (it `sched()`s away — noreturn; it never ERETs to EL0). The **last Thread out** (the one that drives the live-peer count to zero) performs the Proc → ZOMBIE transition with the recorded group status; `wait_pid` already reaps a multi-Thread zombie (walks `p->threads`, `on_cpu`-spins each, `thread_free`s each). The `exit_group` caller is itself flagged and exits the same way, so it need not be the last out.

**Die-check sites.** The check runs at *every* return-to-EL0:
- the **sync-from-EL0 tail** (`exception_sync_lower_el_impl`: SVC return + page-fault-handled return) — already the note-delivery site; the die-check folds in. A woken `torpor_wait` / blocking syscall returns through this tail and dies before userspace resumes.
- a **new IRQ-from-lower-EL return tail** — required by the IPI-kick: the reschedule IPI traps via the *IRQ* vector, which today has **no** return-to-EL0 hook (the handler bumps a counter and ERETs straight back to EL0). The die-check added here also gives the periodic timer tick a death-checkpoint for free (a pure-userspace-spinning peer dies within one tick even absent the IPI). **`#713`-sensitive**: the check sits *before* the DAIF-masked `ELR_EL1`-set..`eret` window, and the die path is noreturn (`sched()` away), so it never reaches the `eret`.

**Consumers (this chunk).** `SYS_EXIT_GROUP = 60(status)` calls `proc_group_terminate(self→proc, status, msg)`; pouch rewires `__NR_exit_group` from `SYS_EXITS`(0) to 60. A cross-Proc `kill` of a multi-thread target calls `proc_group_terminate(target, 1, "killed")` instead of refusing (closes `kill → -EIO`, 13b R1-F9). The multi-thread **fault** path (`proc_fault_terminate` extincting when `thread_count > 1`, §7.9) is the third natural consumer — a **tracked follow-up**, not this chunk (the fault context adds audit surface).

**v1.0 residual → RESOLVED by task #811 (universal death-interruptible sleep).** The #809 chunk (`89456e9`) wakes `torpor` sleepers and broadcast-kicks running peers, and *documented* a residual: a peer blocked **indefinitely** in a non-`torpor` rendez sleep (`poll(-1)`, `pipe`, `devnotes_read`) is not woken by the cascade. The #809 audit (F1) sharpened it — the residual is not "dies at call completion" but a **non-reaping HANG**: such a peer never reaches its checkpoint, so the group-exiting Proc never drives its live-peer count to zero and never transitions to ZOMBIE (the kernel survives — a contained Proc-leak — but the Proc hangs). **Resolution ([OPEN Q 7.9.A] = B, *universal*; user-voted 2026-05-31):** §8.8.1 generalizes `torpor`'s death-interruptibility to **every** rendez sleep, so step 2's wake above is *total* and every flagged peer reaches its EL0-return checkpoint. Implemented as task #811; the design is settled in the same scripture commit that records this resolution. The original framing of this residual as "dies at call completion" was wrong (it was a hang) — corrected here so the record is honest.

Exit status is the existing ok/fail collapse (`exit_status` ∈ {0,1}); the structured 64-bit status that distinguishes the literal `exit_group(N)` code remains the Phase-5+ deferral (`docs/ERRORS.md` "Exit-status semantics"; U-6d-a note).

### 7.10 Open design questions

None at Gate 3.

### 7.11 Summary

Plan 9 `rfork` semantics; threads as siblings in a Proc; notes as the internal signal model; coarse capabilities; per-thread `errstr`. Linux semantics provided by musl + thin kernel shim; no kernel design contortion to accommodate POSIX.

---

## 8. Scheduler

**STATUS**: COMMITTED

### 8.1 Goals and non-goals

**Goals**:
- **EEVDF** (Earliest Eligible Virtual Deadline First) algorithm — Linux 6.6+'s default since 2023, replacing CFS.
- Preemptive at EL0→EL1 boundary (syscall + IRQ entry). Kernel preemption deferred to Phase 7 hardening for safety.
- Per-CPU run trees (work-stealing on idle).
- Three priority bands: `INTERACTIVE`, `NORMAL`, `IDLE` — separate run trees per band per CPU.
- Tickless idle.
- Provable: bounded latency proportional to weight; no starvation; wakeup atomicity.
- SMP-aware from v1.0; up to 8 cores in 72-hour stress test at Phase 7 (hardening / v1.0-rc).

**Non-goals at v1.0**:
- Hard real-time (RT) scheduling class. EEVDF gives soft latency bounds; hard RT is v2.x.
- **Full Energy-Aware Scheduling (EAS)** — per-task PELT utilization + an energy model + DVFS/schedutil coupling + asymmetric misfit migration. v1.0 builds the **HMP-ready foundation** (per-CPU capacity from DTB, a `select_target_cpu` placement hook, util fields, a push-capable `balance()` abstraction) + a basic capacity-aware placement policy — **all logic-verified against a synthetic asymmetric DTB** — but the *empirical* EAS tuning is deferred to real heterogeneous hardware (unverifiable on QEMU/HVF; see §8.4.4). The scheduler is **HMP-ready by design; v1.0 ships homogeneous-treatment** on the uniform-topology targets (QEMU virt, RPi). (deep-smp-review, 2026-06-05.)
- CPU isolation / dedicated cores (cpuset-equivalent). v1.0 schedules everywhere.

### 8.2 EEVDF — the algorithm

(Stoica et al 1995; refined in Linux EEVDF patches 2022-2023.)

Each runnable thread has:
- A **virtual eligible time** `ve_t`: the earliest virtual time at which the thread is eligible to run.
- A **virtual deadline** `vd_t`: the latest virtual time by which the thread should run.

The scheduler picks the eligible thread with the earliest virtual deadline.

Virtual time advances with execution: a thread that runs `Δt` real time advances its virtual time by `Δt × W_total / w_self`, where `W_total` is the sum of weights of all runnable threads and `w_self` is the thread's weight.

When a thread blocks (sleeps), it's removed from the run tree. When unblocked, it's reinserted with its `ve_t` set to the current virtual time (it doesn't get "credit" for sleep time but doesn't get penalized either).

**Deadline computation**:
- `vd_t = ve_t + slice_size × W_total / w_self`
- where `slice_size` is the algorithm's lookahead bound (typically 6 ms, configurable).

This produces:
- Bounded latency: the delay between becoming runnable and running is at most `slice_size × N` for N runnable threads at equal weight.
- Fair share: weight `w` gets `w / W_total` of CPU time.
- No starvation: every runnable thread eventually has the earliest deadline.

### 8.3 Three priority bands

Threads belong to one of three bands:
- **INTERACTIVE**: short deadlines, high weight. Default for Halcyon, terminal apps, anything started from a controlling tty.
- **NORMAL**: standard deadline + weight. Default for everything else.
- **IDLE**: long deadlines, low weight. Background tasks (scrub, indexing, log compaction).

Bands are separate run trees. The scheduler always serves the highest-priority band with runnable threads:
```
schedule(cpu) {
    for band in [INTERACTIVE, NORMAL, IDLE]:
        thread = pick_eevdf(cpu->run_trees[band])
        if thread: return thread
    return cpu->idle_thread
}
```

Within a band, EEVDF picks. Across bands, fixed priority (no aging across bands at v1.0; would be a fairness extension).

### 8.4 Per-CPU run trees + the SMP mechanism

**STATUS**: REDESIGNED (deep-smp-review, 2026-06-05; model-first via
`specs/sched_oncpu.tla` + `specs/sched_alpha.tla`). The original §8.4 described
work-stealing at the level the original `scheduler.tla` modeled it — as a *single
atomic transfer*, with **no `on_cpu` variable**. The deep SMP review found that
the load-bearing mechanism (the `on_cpu` protocol + the boot-CPU idle dispatch)
was never modeled and had accreted point-patches as bugs surfaced one at a time
(#788, #806, #860 — the masking-bug stack). This section now pins the
as-redesigned mechanism. Full evidence: `docs/SMP-REVIEW-FINDINGS.md`.

Each CPU owns a `struct CpuSched`:
- Three per-band run trees keyed on `vd_t` (v1.0 impl: sorted intrusive lists;
  the API is tree-shaped — red-black trees are a deferred optimization).
- A current running thread — **never NULL** (each CPU always runs at least its
  own idle; §8.4.2).
- A **pinned per-CPU idle thread** (§8.4.2).
- The `on_cpu` handoff slots + the per-CPU run-tree lock (§8.4.1).
- A per-CPU **capacity** class + per-CPU scheduler stats (§8.4.4).

#### 8.4.1 The `on_cpu` protocol (the load-bearing SMP correctness mechanism)

`on_cpu[t]` is the cross-CPU signal "this thread's ctx/kstack is physically in use
or mid-switch." It is set under the per-CPU lock (or under the *peer* lock for a
steal-claim) **before** `cpu_switch_context`, and cleared with a RELEASE store by
the *resuming* thread's `finish_task_switch` **after** the switch completes.
`thread_free`, `wait_pid`'s reap, and `wakeup` all spin on `on_cpu` before
reclaiming / waking a thread. The per-CPU run-tree lock is **held across the entire
multi-step switch** (from `sched()` entry until the resuming thread releases it) —
this is what makes a peer's `spin_trylock` skip a mid-switch CPU, and is the
property that keeps a runqueue free of `on_cpu==true` threads (so a steal can never
load a half-saved ctx). **Invariant** (I-21 corollary): a thread runs on ≤1 CPU,
and a ctx/kstack is never written by two CPUs concurrently. Modeled by
`sched_oncpu.tla` / `sched_alpha.tla`; the kernel additionally ASSERTs
`state==RUNNABLE && !on_cpu` on every steal/pick victim (§8.4.5).

#### 8.4.2 Per-CPU pinned in-tree idle (retires the `g_bootcpu_idle` special case)

Every CPU — **including the boot CPU** — has its own idle thread that is
- **pinned to its CPU** (`cpu_pinned`; `try_steal` never migrates a pinned thread
  — this generalizes the secondaries' `kstack_base==NULL` skip and *retires* the
  `g_bootcpu_idle` special case), and
- **in-tree** (it lives in `run_tree[IDLE]` and is dispatched by the ordinary
  `pick_next`, exactly like any other thread).

The boot CPU's idle gets a **dedicated BSS idle stack** (symmetric with the
secondaries' `g_secondary_boot_stacks`) rather than a real-kstack idle — so cpu0
is fully like the secondaries, with no real-kstack/stealability hazard. (The boot
CPU's old idle role was split between `kthread` and a separate real-kstack
`g_bootcpu_idle` dispatched only via a "deadlock path"; that thread owned a real
kstack, which made it *stealable* if it ever entered a tree — the #860 root cause.)

Consequences, all **by construction**: NO deadlock-path dispatch, NO off-tree
RUNNABLE state, NO racy deadlock guard. Each CPU's idle is *always* reachable on
its own CPU (`sched_alpha.tla` `IdleAvailable`), so the old `"sched: deadlock —
current is blocking, no runnable peer"` condition is structurally impossible; and
a pinned idle never migrates (`IdleStaysHome`). This closes #860 + the entire
off-tree class. Alternatives weighed (a `cpu_pinned` flag on a real-kstack cpu0
idle vs. the dedicated BSS stack): the dedicated stack was chosen for full symmetry
with the secondaries (no special case anywhere). Rationale in SMP-REVIEW-FINDINGS §7.

#### 8.4.3 The placement seam: `select_target_cpu` + `balance()`

Placement **policy** is separated from the enqueue **mechanism** (the original
design baked placement into `ready()` = "the CPU that woke you"; that is a policy,
not a law, and made HMP a cross-cutting retrofit).

- **`select_target_cpu(task, prev_cpu)`** — chooses the target CPU's run tree when
  a thread becomes runnable. v1.0 homogeneous body returns the current/prev CPU
  (the existing behavior, preserved). HMP bodies bias by capacity (§8.4.4).
- **`balance()`** — the load-rebalancing abstraction. v1.0 implements it as
  **pull-only work-stealing**: an idle CPU pulls one *non-pinned* runnable thread
  from a peer (round-robin-from-rotating-start, bounded steal frequency, the steal
  claims `on_cpu` under the peer lock per §8.4.1). The abstraction is deliberately
  **future-ready**: it is shaped to host a capacity-aware **push** path — *misfit
  migration*, pushing a heavy task off a low-capacity core to a high-capacity one
  even when the latter is not idle — without re-architecting the scheduler. Misfit
  push is the one HMP mechanism a pull-only stealer structurally cannot express;
  building `balance()` push-capable now is the design-for-the-future decision
  (user-directed) that keeps the eventual EAS layer additive rather than a rewrite.

The placement hook + `balance()` are the HMP **seams**. The `on_cpu` / migration
protocol is **placement-agnostic** — `sched_alpha.tla` proves the safety invariants
hold under *arbitrary* placement — so any `select_target_cpu` / `balance()` policy
(homogeneous or capacity-aware) composes with correctness by construction.

#### 8.4.4 HMP-ready foundation (capacity, util) + the verification boundary

- **Per-CPU capacity** class parsed from the DTB (`cpu-map` clusters +
  `capacity-dmips-mhz`); composes with **I-15** (the hardware view derives entirely
  from the DTB). Uniform on QEMU virt / RPi → homogeneous behavior.
- **Per-task utilization** (PELT-style) fields + accounting hooks on the
  enqueue/dequeue/tick transitions — inert on uniform-topology targets.
- A **basic capacity-aware placement policy** (bias high-util tasks to
  high-capacity CPUs), gated on *declared* heterogeneity.

**The verification boundary** (the load-bearing methodological commitment, per
"complexity is permitted only where it is verified"):

- **Verifiable NOW → built now.** The placement *logic* is unit-tested against a
  hand-crafted **synthetic asymmetric DTB** (declared `capacity-dmips-mhz`
  asymmetry; deterministic; needs no real perf asymmetry): "does `select_target_cpu`
  route the heavy task to the high-capacity CPU." Plus the safety composition
  (`sched_alpha.tla`, arbitrary placement). So v1.0 ships the seams + capacity +
  util-hooks + a logic-verified basic placement policy.
- **NOT verifiable until real heterogeneous hardware → deferred.** The *empirical*
  EAS tuning — PELT decay constants, the energy model, schedutil/DVFS coupling,
  misfit thresholds — cannot be validated on the dev target: QEMU virt declares a
  **homogeneous DTB**, and HVF (the native dev loop, `THYLACINE_ACCEL=hvf`) runs
  guest vCPUs on real P/E cores but the **host floats them** (macOS has no P-core
  pin), so the guest sees no *stable, declared* capacity asymmetry to place against
  and measure. **HVF closes the speed gap, not the heterogeneity gap.** The first
  true EAS verification surface is a real big.LITTLE SoC bare-metal (the Lazarus
  RPi boards are homogeneous A72/A76; bare Apple Silicon is v2 per PORTABILITY.md)
  or a deliberate Linux+KVM+cpuset-pin+custom-DTB harness — both post-v1.0. The EAS
  layer is therefore additive + pre-modeled, landing when it becomes verifiable.

#### 8.4.5 `idle_in_wfi` + steal-invariant hardening

- **`idle_in_wfi`** means "about to WFI," cleared when `sched()` switches the idle
  thread away to real work. This fixes the F7 stale-flag bug both review
  prosecutors found independently: a CPU that left the idle-loop body but is now
  *running stolen work* looked idle to `sched_notify_idle_peer`, so peers
  mis-routed wake-IPIs to it and skipped a genuinely-idle peer (an I-8 / I-17
  latency/fairness leak).
- **The steal/pick invariant is loud.** `try_steal` / `pick_next` ASSERT
  `state==RUNNABLE && !on_cpu` on every victim, making the load-bearing "a run tree
  only ever holds `on_cpu==false` threads" invariant fail-stop instead of
  silently-corrupting if a future change ever shortens the lock-hold (F3).

#### 8.4.6 The multi-boot soundness gate (the verification process)

The deep review's load-bearing process finding: **a single boot is not a soundness
gate.** The #788 / #806 / #860 context-corruption races are layout-/timing-sensitive
and pass a single boot most of the time, so a one-shot `tools/test.sh` *masked* #860
for weeks — it is the verification gap, not just a bug. The redesign therefore ships
its own gate as scripture, per "complexity is permitted only where it is verified":

- **`tools/ci-smp-gate.sh`** (`make smp-gate`) multi-boots the matrix — `default-smp4`,
  `default-smp8`, `ubsan-smp4` (the #860 amplifier), `ubsan-smp8` — at **N≥10** each,
  composing `tools/smp-multiboot.sh`'s classifier (a ctx/stack-corruption signature
  FAILS; benign host-timing fragility is reported, not failed). `tools/test.sh` is the
  primitive the gate multi-boots, never itself the gate.
- **Host-timing budgets soft-warn, never extinct.** The `test_irq_latency_bench` QEMU
  p99 budget uses the `TEST_SOFT_WARN` harness primitive (log + count, do not fail the
  suite) rather than `TEST_ASSERT`. A hard assert there turned host throttling into a
  kernel "crash" and — because `boot_main` extincts on any suite failure — masked any
  real fault later in the same boot (#860 lived in the post-test production bringup). A
  true pathological regression is still caught (`BOOT_TIMEOUT` for a hang; the hard
  sample-validity assert for counter-math). The soft-warn is for host-timing budgets
  ONLY; correctness asserts (the cons/torpor quiescence checks) stay hard.

### 8.5 Wakeup atomicity

The classic OS bug: thread A is about to sleep on condition X; thread B sets X just before A sleeps; A sleeps with X set; A misses the wakeup and never runs.

Thylacine prevents this with the standard wait/wake protocol:
1. A acquires a per-condition lock.
2. A checks X under lock; if true, returns (no sleep needed).
3. A enqueues itself into the condition's wait queue under lock.
4. A releases the lock and immediately blocks (atomically — the kernel-internal sleep takes the wait queue lock as part of the block step).
5. B sets X under the lock.
6. B walks the wait queue under lock and wakes any thread.

Spec: `scheduler.tla` proves that this protocol delivers no missed wakeups, even with arbitrary interleavings.

### 8.6 Tickless idle

ARM generic timer is per-CPU and programmable. Idle CPUs:
- Disarm the periodic timer.
- Compute next deadline (earliest of: any sleeping thread's wakeup, any band's earliest deadline + band's slice_size).
- Program the timer to fire at that deadline.
- Execute `WFI` (wait for interrupt).

The CPU sleeps until the timer fires or an IPI/IRQ wakes it. Power efficiency benefit on bare metal; latency benefit in QEMU (no spurious wakeups).

### 8.7 IPI infrastructure

Cross-CPU operations are IPIs (Inter-Processor Interrupts), not shared-memory polling. Standard IPI types:
- `IPI_RESCHED`: peer should rerun the scheduler (a thread became runnable on it from another CPU).
- `IPI_TLB_FLUSH`: peer should flush a specific TLB range (after page table change).
- `IPI_HALT`: peer should halt (used at shutdown).
- `IPI_GENERIC`: peer should run a callback function (used for cross-CPU data structure operations).

GIC v2/v3 SGI (Software-Generated Interrupt) mechanism delivers IPIs. Spec: `scheduler.tla` proves IPI ordering — an IPI sent before another IPI to the same CPU is processed first.

### 8.8 Plan 9 idiom layer

The kernel exposes the Plan 9 idiom on top of EEVDF:

```c
void sched(void);                  /* yield to scheduler */
void ready(struct Thread *t);      /* mark thread runnable */
void sleep(struct Rendez *r, int (*cond)(void *), void *arg);  /* wait */
int  tsleep(struct Rendez *r, int (*cond)(void *), void *arg, uint64_t deadline_ns);  /* wait, deadline-bounded */
int  wakeup(struct Rendez *r);     /* wake one waiter */
void rendezvous(void *tag, void *value);  /* cross-thread synchronous handoff */
```

These are familiar to anyone who's read Plan 9 kernel source. Underneath, they map to EEVDF + wait queue + IPI machinery.

`tsleep` is `sleep` with a deadline: the waiter wakes on `wakeup`, on its condition becoming true, or when the deadline passes — the return value distinguishes a timeout from a condition wake. It is the primitive behind every bounded kernel wait: a `/srv` client blocked on a possibly-hung 9P server (CORVUS-DESIGN.md §6.2), `poll` / `select` timeouts, `futex` with a timeout. Plain `sleep` is `tsleep` with no deadline. Lands at P5-tsleep.

#### 8.8.1 Death-interruptible sleep (universal Eintr) — task #811

**STATUS**: DESIGNED (scripture-first; implementation follows — task #811, the F1=B completion of `SYS_EXIT_GROUP`, §7.9.1).

`sleep` / `tsleep` block on a `Rendez` until their condition (or, for `tsleep`, a deadline) fires. Group termination (§7.9.1) — and, at v1.x, the multi-thread fault / signal path (§7.9, task #810) — needs a *third* wake source: when a sleeping peer's Proc is group-terminating, that peer must be woken so it returns to its EL0-return tail and self-terminates at `el0_return_die_check`. `torpor_wait` already has this — a register-then-observe re-check of `group_exit_msg` under `torpor_lock`, plus `torpor_wake_all_for_proc`. This subsection generalizes that one death-interruptible sleep to **every** rendez sleep, closing the §7.9.1 v1.0 residual (a peer blocked indefinitely in `poll(-1)` / `pipe` / `devnotes_read` would otherwise never reach its checkpoint and its group-exiting Proc would never reap — the #809-audit F1 hang).

**The discovery problem.** `torpor`'s wake walks a central waiter table. A general rendez sleep has no such registry — the only record that "Thread T sleeps on Rendez R" is the reverse pointer `Thread.rendez_blocked_on`. The cascade waker holds the *Proc* (it walks `p->threads`); to wake a peer it must read that peer's `rendez_blocked_on` and `wakeup()` it. That read races the sleeper's write, and acquire/release on `group_exit_msg` **alone does not** establish the happens-before in the miss case — a lost wakeup. A lock is required.

**Mechanism — a per-Thread `wait_lock` (the Plan 9 `p->rlock` analog).**
- A new per-Thread spinlock `wait_lock` protects the thread's wait registration (`rendez_blocked_on` + its `THREAD_SLEEPING` transition). Plan 9's `postnote` takes the sleeping proc's `rlock` for exactly this reason — to read `p->r` and `ready` it; Thylacine adopts the same shape.
- **Generalized register-then-observe** (the I-9 close): `sleep` / `tsleep`, after registering the waiter (set `rendez_blocked_on`, enqueue on the Rendez / timer-wait list, transition `THREAD_SLEEPING`), re-check `current->proc->group_exit_msg` (acquire-load) *before* dropping the locks and calling `sched()`. If it is set, unwind the registration and return a new `*_INTR` sentinel instead of sleeping. This is exactly `torpor`'s pattern hoisted into the two core primitives.
- **The cascade waker** (`proc_group_terminate`; at v1.x also the fault / signal poster) walks `p->threads`; for each peer in `THREAD_SLEEPING` it takes that peer's `wait_lock`, reads `rendez_blocked_on`, and `wakeup()`s the Rendez. The woken peer's `sleep` / `tsleep` loop re-checks `group_exit_msg` on resume and returns `*_INTR`.
- **Only the owning Thread mutates `rendez_blocked_on`** — set at register, cleared on resume, both under its own `wait_lock`. Wakers at most *read* it (the cascade, under `wait_lock`). The normal `wakeup(r)` hot path still finds its waiter via `r->waiter` and is unchanged — it adds no per-Thread lock. This is what keeps the lock graph acyclic (below). A cascade that reads a stale `rendez_blocked_on` for a peer already woken on its normal path is harmless: `wakeup(r)` on a Rendez whose `waiter != t` is a no-op, and the peer re-checks `group_exit_msg` on resume regardless.

**Die at the EL0-return tail, never inside `sleep`.** An interrupted sleep returns `*_INTR`; it does **not** self-terminate inside `sleep()` — that would strand whatever kernel locks / transient state the *caller* holds. The blocking syscall handler treats `*_INTR` as "abort this call": release locks, free transient resources, return. The Thread then unwinds to its EL0-return tail, where `el0_return_die_check` (§7.9.1) terminates it. The value returned to userspace is immaterial — a group-flagged Thread never reaches EL0. Each of the nine non-`torpor` blocking sites (`poll`, `pipe` ×2, `devnotes_read`, `srvconn` ×2, `devsrv`-accept, `irqfwd`, `wait_pid`) gets a uniform "on `*_INTR` → cleanup → return" arm; this per-site sweep is the bulk of the implementation, and each site's cleanup + I-9 correctness is re-validated in the #811 audit.

**Lock order (amended).** `wait_lock` is the **outermost wait-lock**:

```
sleeper:  wait_lock → g_timerwait.lock → Rendez.lock          (registration)
waker:    g_proc_table_lock → wait_lock → (wakeup: g_timerwait.lock → Rendez.lock)
```

Acyclic because (a) only the owning Thread *writes* `rendez_blocked_on`, so the waker→sleeper edge is read-only under `wait_lock`; and (b) no sleeper holds `g_proc_table_lock` *below* `wait_lock` — `wait_pid`, the one sleeper that touches the proc table, drops `g_proc_table_lock` before it sleeps (verified, `kernel/proc.c`). The existing `g_timerwait.lock → Rendez.lock → CpuSched.lock` order (`kernel/sched.c`) is preserved beneath `wait_lock`.

**Scope — `exits()` folds in (closes #809-audit F4).** Whole-Proc `exits()` / `SYS_EXITS` has the same "sleeping-peer never reaped" exposure as `exit_group`: it already publishes `group_exit_msg`, but at v1.0 only `torpor` peers are woken. #811 builds the exact mechanism that fixes it, so `exits()` on a multi-thread Proc routes through the same universal wake — neutralizing the #809-audit F4 `__NR_exit`-with-live-peers trap rather than leaving it a separate documented hazard. (The `kproc` guard in `proc_group_terminate` — `#809`-audit self-found P3a — stays.)

**Invariant.** This is I-9 generalized: *no death-wake is lost between a sleeper's condition check and its sleep*, for **every** rendez sleep — enforced by the register-then-observe under `wait_lock` exactly as `torpor` enforces it under `torpor_lock`. It also completes I-24's "no Thread executes at EL0 after its Proc's ZOMBIE transition" for the indefinite-sleeper class. No new TLA+ module (spec-to-code suspension, 2026-05-23); the prose here + the #811 audit + per-site regression tests are the rigor.

### 8.9 Open design questions

- **EEVDF lookahead bound** (the `slice_size` parameter). Default 6 ms (matching Linux EEVDF). Tune-able at `/ctl/sched/slice-size`. Decision deferred to Phase 2 implementation; spec sweep over a range.
- **Cross-band aging**: should an idle-band thread eventually promote to normal-band? At v1.0, no — the bands are clean-cut. v2.x could add age-based promotion if needed.

### 8.10 Spec

Spec-first is **re-enabled for the SMP scheduler/thread-lifecycle mechanism** (the
natural re-enabling point flagged by the spec-to-code suspension: an
invariant-bearing feature that genuinely benefits from machine-checked
exploration). Three modules gate this surface:

- **`specs/scheduler.tla`** (the high-level state machine) proves: progress
  (every runnable thread eventually runs), latency bound, wakeup atomicity, IPI
  ordering, work-stealing fairness. **Caveat (deep-smp-review):** it models
  `Steal` as a *single atomic transfer* with **no `on_cpu` variable**, so it is
  structurally blind to the multi-step claim/load/clear window and the boot-CPU
  idle dispatch where #788/#806/#860 actually live. It remains valid for what it
  models (the state machine under atomic steal); the two modules below cover the
  mechanism it abstracts away.
- **`specs/sched_oncpu.tla`** (the diagnostic) re-introduces `on_cpu` + the
  multi-step switch + the per-CPU lock held across the switch + the boot-CPU
  deadlock-dispatch special case. It **reproduces #860** (two CPUs running one
  thread) and produces the A/B matrix that justified the fix.
- **`specs/sched_alpha.tla`** (the **gating model** for the redesigned scheduler)
  proves the target architecture (§8.4.1–§8.4.4) holds the safety invariants —
  `NoSimultaneousRun`, `OwnerUnique`, `RunqOnCpuSafe`, and the redesign-specific
  `IdleStaysHome` (pinned idles never migrate), `IdleAvailable` (the deadlock-path
  special case is unnecessary), `AlwaysRunning` — **under arbitrary placement**
  (so any `select_target_cpu`/`balance()` policy composes with correctness). The
  HMP placement *logic* is additionally LOGIC-verified by a kernel unit test
  against a synthetic asymmetric DTB (§8.4.4); the empirical EAS tuning is deferred
  to real heterogeneous hardware.

### 8.11 Summary

EEVDF on per-CPU run trees with work-stealing, three priority bands, tickless idle, IPI-driven cross-CPU operations, Plan 9 idiom layer on top, formally verified.

---

## 9. Territory and device model

**STATUS**: COMMITTED

### 9.1 The territory

Each process has a private territory — a tree of mount points mapping names to resource servers. The territory is the fundamental unit of isolation and composition.

**Operations**:
- `bind(old, new, flags)` — attach a file or directory at another point in the tree. Flags: `MREPL` (replace), `MBEFORE` (union, checked first), `MAFTER` (union, checked last), `MCREATE` (allow creates in the union).
- `mount(fd, afd, old, flags, aname)` — attach a 9P server (reached via `fd`) at `old`. `afd` is the auth fd (or -1); `aname` is the attach name (passed to `Tattach`). Flags: `MREPL`, `MBEFORE`, `MAFTER`.
- `unmount(name, old)` — remove a mount point. `name` is the mounted source (or NULL for any); `old` is the mount point.

These three operations, composed, express containers, overlay filesystems, per-process views, chroot, and capability restriction — without additional kernel machinery.

**Territory inheritance at `rfork`**: by default (no `RFNAMEG`), the child gets a private copy of the parent's territory. Modifications to the child don't affect parent. With `RFNAMEG`, parent and child share the territory (typical for thread creation).

**Territory data structure** (`kernel/territory.c`):

```c
struct Mount {
    struct Spoor *to;           /* what's mounted */
    struct Spoor *from;          /* mount point */
    int flags;
    struct Mount *next, *prev;
    int order;                  /* union ordering */
};

struct MountHead {
    struct Mount *mount;        /* list of mounts at this point */
    struct rb_node rb;
};

struct Territory {
    struct MountTable {
        struct rb_tree mounts;  /* keyed by mount point qid */
        rwlock_t lock;
    } mt;
    int ref;
};
```

`bind` / `mount` operations take `Territory->mt.lock` write-locked; lookups take it read-locked. Lock contention is rare (mount changes happen at process startup or container creation, not on hot paths).

**Spec**: `territory.tla` proves:
- Cycle-freedom: no `bind` operation can create a cycle.
- Isolation: territory operations in process A don't affect process B's territory (unless they share via `RFNAMEG`).
- Walk determinism: a path lookup from a fixed territory state always produces the same Spoor.

### 9.2 The Dev vtable

Every kernel device implements:

```c
struct Dev {
    int   dc;                    /* device character ('c' for cons, 'e' for ether, etc.) */
    char *name;

    void   (*reset)(void);
    void   (*init)(void);
    void   (*shutdown)(void);
    struct Spoor*  (*attach)(char *spec);
    struct Walkqid* (*walk)(struct Spoor *c, struct Spoor *nc, char **name, int nname);
    int    (*stat)(struct Spoor *c, uint8_t *dp, int n);
    struct Spoor*  (*open)(struct Spoor *c, int omode);
    void   (*create)(struct Spoor *c, char *name, int omode, uint32_t perm);
    void   (*close)(struct Spoor *c);
    long   (*read)(struct Spoor *c, void *buf, long n, int64_t off);
    struct Block* (*bread)(struct Spoor *c, long n, int64_t off);
    long   (*write)(struct Spoor *c, void *buf, long n, int64_t off);
    long   (*bwrite)(struct Spoor *c, struct Block *bp, int64_t off);
    short  (*poll)(struct Spoor *c, short events, struct poll_waiter *pw);
    void   (*remove)(struct Spoor *c);
    int    (*wstat)(struct Spoor *c, uint8_t *dp, int n);
    struct Spoor*  (*power)(struct Spoor *c, int on);
};
```

All kernel devices — including synthetic ones like `/dev/cons`, `/dev/null`, `/proc` — implement this interface. Userspace devices implement it remotely via 9P.

The interface is Plan 9's `Dev` vtable (with C99 typing), preserved for two reasons: (a) it's right; (b) it makes porting from 9Front straightforward when we want to. The one Thylacine addition is `poll` — the readiness query backing `SYS_POLL` (§23.3); a device with no readiness state leaves the slot NULL, and `poll` then treats the fd as always ready (POSIX-correct for a regular file).

### 9.3 Userspace drivers as 9P servers

**Driver model**: a userspace process owns device resources via typed handles and exposes them as a 9P server. The kernel mounts the server and routes Dev vtable calls to it via the 9P client.

**Driver startup flow** (handled by `init` based on DTB inspection):
1. Kernel parses DTB, discovers a device (e.g., a VirtIO block device).
2. Kernel creates `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` handles for the device.
3. Kernel exec's the driver process with these handles attached (via a startup channel — not inherited from a parent).
4. Driver process uses the handles to map MMIO, register for IRQs, set up DMA buffers.
5. Driver process exposes its 9P server at `/dev/<name>/` (where `<name>` is the driver-class name + device index).
6. Kernel mounts the driver's 9P server.

After step 5, the kernel has no further involvement in the hot path. The driver owns the hardware directly via the mapped MMIO and DMA regions.

**At v1.0**: the drivers in this model are virtio-blk, virtio-net, virtio-input, virtio-gpu (all VirtIO; running on QEMU). Each runs as a separate process. There is no in-kernel VirtIO device driver code at v1.0.

**Driver crash recovery**: if a driver process crashes (segfault, assertion, OOM):
1. Kernel observes the process exit.
2. All 9P sessions to the driver are clunked; affected `Spoor`s in clients are marked invalid (return `-EIO` on subsequent ops).
3. Handles held by the driver are released (the `KObj_*` resources go back to the kernel free pool).
4. `init` (or a designated supervisor) detects the crash and restarts the driver. New driver process gets fresh handles for the same hardware.
5. Clients reconnect; resume.

This is the same shape as MINIX 3's reincarnation server. Implemented as a userspace supervisor (Phase 6+); kernel exposes the necessary signals (`/ctl/proc-events/exit`).

### 9.4 Device territory layout (v1.0 target)

```
/
├── dev/
│   ├── cons             ← console (kernel Dev: read input, write output)
│   ├── consctl          ← console mode control (kernel Dev)
│   ├── null             ← /dev/null (kernel Dev)
│   ├── zero             ← /dev/zero (kernel Dev)
│   ├── random           ← CSPRNG (kernel Dev; ARM RNDR + chacha20-stir)
│   ├── urandom          ← same as random; kept for POSIX compat
│   ├── mem              ← physical memory (kernel Dev; CAP_RAW_MEM gated)
│   ├── tty              ← controlling tty alias
│   ├── ptmx             ← PTY allocation (userspace 9P server)
│   ├── pts/             ← PTY slaves (userspace 9P server)
│   ├── fb/              ← framebuffer (userspace driver, virtio-gpu)
│   │   ├── ctl          ← write: "res 1920 1080", "flush"
│   │   ├── image        ← BURROW handle to pixel buffer
│   │   └── info         ← read: current resolution, format
│   ├── video/           ← video player (userspace 9P server)
│   │   └── player/
│   │       ├── ctl
│   │       ├── frame
│   │       ├── position
│   │       └── duration
│   ├── janus/           ← key agent (userspace 9P server)
│   │   ├── keys/
│   │   ├── audit
│   │   └── ctl
│   └── virtio-blk0     ← virtio block device interface (rare; usually via /dev/sda)
├── proc/                ← process territory (kernel Dev)
│   └── <pid>/
│       ├── ctl
│       ├── mem
│       ├── ns
│       ├── status
│       ├── cmdline
│       └── fd/
├── proc-linux/          ← Linux-compat /proc (userspace 9P server)
│   └── (same shape as Linux's /proc)
├── sys/                 ← Linux-compat /sys (userspace 9P server, post-v1.0)
├── net/                 ← network stack (userspace 9P server)
│   ├── ipifc/
│   ├── tcp/
│   └── udp/
├── srv/                 ← service registry (devsrv kernel Dev; Plan 9 #s)
│   └── corvus/          ← corvus key agent's 9P server (CORVUS-DESIGN.md §6)
├── ctl/                 ← kernel admin (kernel Dev)
│   ├── procs            ← list of processes
│   ├── memory           ← memory stats
│   ├── devices          ← attached devices
│   ├── kernel/          ← kernel state
│   │   └── base         ← KASLR base
│   ├── sched/           ← scheduler stats
│   ├── 9p/              ← 9P session info
│   ├── irq/             ← IRQ delivery stats
│   ├── mm/              ← memory management stats
│   ├── security/        ← hardening counters
│   └── log/             ← kernel logs
├── tmp/                 ← tmpfs
├── run/                 ← tmpfs (fast volatile state)
└── (root from Stratum)
```

`/srv` is served by **`devsrv`**, a kernel `Dev` distinct from `dev9p`: a userspace 9P server registers a name with `SYS_POST_SERVICE` (§11.2c) and the kernel mediates per-connection client access, stamping each connection with the peer Proc's kernel identity. A `/srv/<name>` connection Spoor is a `KObj_Srv` handle (§18.2), non-transferable — so a connection's peer identity cannot be forged by transferring the handle. `corvus` (CORVUS-DESIGN.md §6) is the v1.0 consumer; the separate Dev keeps `KObj_Srv` structural.

**stalk-3 (STALK-DESIGN.md §5; design signed off 2026-06-02, lands per §5.4):** `/srv` becomes **namespace-resident** — `devsrv` mounted per-territory over a **heap-allocated, refcounted `SrvRegistry`** reached through the mounted root Spoor (D7); `open(/srv/<name>)` becomes the connect (9p-mode → a dev9p root via `p9_srvconn_transport`+`p9_attached`; byte-mode → a byte-stream Spoor), so the connection endpoint becomes a **`KOBJ_SPOOR`** and the `KObj_Srv` kind narrows to the **listener** side; posting becomes `create(/srv/<name>)` with a `DMSRVBYTE` `perm` bit selecting byte-vs-9P (D6); `SYS_SRV_CONNECT` / `SYS_POST_SERVICE` / `_BYTE` retire (D3). The per-territory registry is the A-5b isolation foundation (a second user's coordinator is unnameable).

### 9.5 Interactions with the per-process territory

A process's territory is a tree. The mount table at any path can have multiple Spoors (union mount):

```
Process A's territory:
/home/alice (bound from /var/stratum/users/alice)
/code       (bound from /var/stratum/code)
/dev        (kernel Dev tree)
/etc        (Stratum / etc subtree)
```

The Plan 9 walk algorithm: walk from `/` to each path component; at each component, check the mount table for that path. If multiple mounts (union), check each in declared order until one succeeds. If a path component creates (in `MCREATE`-flagged union), create in the first writable mount.

### 9.6 Mount: the filesystem-as-Spoor principle

**The unifying observation**: every filesystem entity in Thylacine is a Spoor. There is no separate notion of "filesystem" distinct from "the Spoor abstraction." This is true both for kernel-internal filesystems (devcons, devnull, devramfs, /proc, /ctl) and for remote 9P filesystems (Stratum, future userspace 9P servers, network 9P).

Two kinds of Spoor backing exist, distinguished only by which Dev vtable they invoke:

- **Kernel-Dev-backed Spoors**: vtable operations dispatch directly to a C implementation. The Dev struct (§9.2) implements walk/open/read/write/clunk in kernel code. Used by all the v1.0 in-kernel synthetic filesystems.
- **`dev9p`-backed Spoors**: vtable operations route through a `kernel/9p_client.c` instance to a remote 9P server. Each Spoor carries a (`p9_client *`, `u32 fid`) pair as private state; walk → `p9_client_walk_one`, read → `p9_client_read`, write → `p9_client_write`, clunk → `p9_client_clunk`, etc. The `dev9p` Dev vtable IS the proxy.

Above this layer the kernel sees only Spoors. The walk algorithm (§9.5) doesn't know which backing a Spoor uses; it just dispatches the vtable.

#### 9.6.1 Mount decomposed

The Plan 9 `mount(fd, afid, mountpoint, flags, spec)` syscall conflates two operations:
1. **Establishing a 9P session** over a byte-pipe fd (handshake + attach).
2. **Grafting** the resulting filesystem tree onto a path in the namespace.

Thylacine decomposes these into two small syscalls:

- **`attach_9p(transport_spoor_fd, aname, n_uname) → new_spoor_fd`**: wraps a byte-pipe Spoor (Unix socket, virtio-vsock, pipe, etc.) in a kernel-internal `p9_client`, drives the Tversion + Tattach handshake, and returns a new Spoor backed by `dev9p` whose root fid is the bound attach. The returned Spoor IS the 9P tree's root. Walk it, read it, or mount it — all work identically.
- **`mount(source_spoor_fd, target_path, flags) → 0`**: grafts the source Spoor's tree at `target_path` in the caller's Territory. The source Spoor can be ANY Spoor — a `dev9p`-backed one from attach_9p, a kernel-internal Dev's root (e.g., the devramfs root), or even a sub-tree of an existing mount. `flags` mirror Plan 9: `MREPL`, `MBEFORE`, `MAFTER`, `MCREATE`.

The existing `bind(source_path, target_path, flags)` syscall (§9.1) handles path-to-path symbolic mappings — that's the Plan 9 `bind`, unchanged.

#### 9.6.2 Why decomposed

| Property | Combined Plan 9 `mount` | Linux NFS `mount` | Decomposed `attach_9p + mount` |
|---|---|---|---|
| Transport-setup in kernel | No | Yes | No |
| Connection management in kernel | No | Yes | No |
| One syscall per "mount a server" action | Yes | Yes | No (two) |
| Composable (mount a sub-tree of a mount) | Harder | No | Yes |
| Linux-compat shim location | userspace (translates to attach+mount) | kernel | userspace |
| Adding new transport types | No kernel change | Per-type kernel code | No kernel change |
| Spoor representing an open 9P session is a first-class kernel object | No (mount is the only way to use 9P) | No | Yes |

The decomposition trades one syscall for two and gains: kernel stays out of connection management, the attach_9p result is usable without a mount (ad-hoc 9P RPCs, future agent-style daemons), composition works across mounts, Linux-vs-Plan9 lives purely in userspace as a libc shim.

Modern Linux arrived at the same conclusion (independently): `fsopen` + `fsconfig` + `fsmount` + `move_mount` decompose `mount(2)` into 4 syscalls. Thylacine's 2-syscall decomposition is the same idea with less surface area, because attach_9p is the only transport-setup the kernel knows about.

#### 9.6.3 Plan 9 native programs

A Plan 9 program calling `mount(fd, afid, mountpoint, flags, spec)`:

```c
// musl/Plan9-libc shim, on Thylacine:
int mount(int fd, int afid, char *mountpoint, int flags, char *spec) {
    int t = attach_9p(fd, spec, /*n_uname=*/0);   // afid handling: see §9.6.5
    if (t < 0) return t;
    int rc = mount_syscall(t, mountpoint, flags);
    close(t);                                      // mount holds its own ref
    return rc;
}
```

The shim is 5 lines; the kernel sees only the decomposed primitives.

#### 9.6.4 Linux compat

A Linux program calling `mount(source, target, "9p", flags, "trans=fd,rfdno=N,...")`:

```c
// musl shim on Thylacine (Phase 6+):
int mount(const char *source, const char *target, const char *type,
          unsigned long flags, const void *data) {
    if (strcmp(type, "9p") == 0) {
        int rfdno = parse_rfdno(data);              // from option string
        int aname_len; const char *aname = parse_aname(data);
        int t = attach_9p(rfdno, aname, aname_len, /*n_uname=*/0);
        if (t < 0) { errno = -t; return -1; }
        int rc = mount_syscall(t, target, flags_to_mflags(flags));
        close(t);
        return rc;
    }
    // Other fstypes: ENODEV at v1.0; FUSE-style proxy at v1.x.
    errno = ENODEV;
    return -1;
}
```

The kernel has **zero** Linux-specific code. v9fs option parsing happens in the shim; the shim composes attach_9p + mount.

#### 9.6.5 Authentication (afid)

Plan 9's `mount` takes a separate auth fd. At v1.0 Thylacine ships no-auth (matches the Stratum-internal-trust model — Stratum's threat model is offline-attack-resistance via the `.key` sidecar; live-protocol auth is out of v1.0 scope). The `n_uname` arg is preserved so Linux v9fs `uname=`/`access=` options propagate.

v1.1+ adds `attach_9p_auth(transport_spoor_fd, auth_spoor_fd, aname, n_uname) → new_spoor_fd` as a second flavor when authentication backends land. No kernel ABI break — additive syscall.

#### 9.6.6 Lifecycle

The mount lifecycle composes naturally with existing refcounting:

- **`attach_9p`** allocates a `p9_client` + a `dev9p`-backed Spoor referencing it. The Spoor holds the only ref to the client. Caller gets an fd referencing the Spoor.
- **`mount`** adds a `(target_path, Spoor_ref)` entry to the Territory's mount table; this bumps the Spoor's refcount.
- **Caller can close the attach_9p fd** after `mount` — the mount table holds the ref. Caller can also keep the fd to do ad-hoc 9P walks on the same session without going through the namespace.
- **`unmount(target_path)`** removes the mount-table entry; drops the Spoor ref.
- **When the Spoor's refcount hits 0**, its dev9p vtable's close path destroys the `p9_client` (which closes the transport).
- **`rfork(RFNAMEG=0)`** shares the Territory → shares the mount table → all Spoors keep their existing refs.
- **`rfork(RFNAMEG=1)`** clones the Territory → clones the mount table → bumps each mount Spoor's refcount. Procs in the new Territory see the same backing clients but can mount/unmount independently.
- **Territory destruction** releases each mount entry, decrementing Spoor refs.

Mount-lifecycle invariants extend `specs/namespace.tla` (the existing bind / cycle-freedom / isolation spec) with:
- A mount table is a finite set of `(path, Spoor)` pairs.
- Every Spoor in the table has refcount ≥ 1 contributed by the table.
- `mount` is idempotent under (path, Spoor) equality.
- `unmount` removes one entry; the Spoor's refcount drops by 1.
- Territory destroy releases all entries.

#### 9.6.7 Pathname resolution: stalk

`stalk` is the per-Proc multi-component pathname resolver — Plan 9's `namec`
(*name-to-channel*), renamed for the Thylacine bestiary (the predator stalks its
quarry along a path to the target Spoor). It is the **first consumer of the mount
table in the walk path**: until it lands, `SYS_WALK_OPEN` resolves a single
component and never crosses a mount (the abstract `path_id_t` mount keying has no
string→path layer). `stalk` makes absolute paths (`/srv/corvus/ctl`,
`/sbin/login`, `/home/<user>`) resolve from the Territory's `root_spoor`.

The full design is binding scripture in **`docs/STALK-DESIGN.md`** (signed off
2026-06-02; user-voted the full Plan-9 spine over an fd-relative shim). The
load-bearing properties:

- **Per-component X-search** — every directory hop runs `perm_check(p, &st,
  PERM_X)` (the A-3 enforcement, generalized from the single-hop walk-open to N
  hops); the final hop uses `perm_want_for_omode`. This is the privilege boundary.
- **Mount crossing = Plan 9 `domount`** — the mount table re-keys from the
  abstract `path_id_t` to the **full Plan 9 mount-point identity** `(type, dev,
  qid)` = `(dc, devno, qid.path)`; after resolving each component, `stalk` checks
  whether the Spoor is a mount point and crosses (cross-on-descent). The `devno`
  axis (a per-attach `Spoor.devno`, Plan 9 `Chan.dev`, minted by
  `spoor_next_devno()`) is load-bearing: every dev9p session shares `dc='9'` +
  root `qid.path 0`, so `(dc, qid.path)` alone collides two concurrent 9P
  sessions (the A-5b corvus + per-user-stratum-fs case). `SYS_MOUNT`/`UNMOUNT`
  are path-keyed and resolve the mount point with a fourth amode `STALK_MOUNT`
  (final element NOT crossed, so MREPL re-keys the same underlying point). Mount
  points must exist as walkable dirs (D4 = M1; devramfs gains `/srv`, `/proc`).
  This supersedes the §9.6.1 forward-looking `mount(source_spoor_fd, target_path,
  flags)` sketch with the as-built Spoor-identity-keyed mechanism.
- **`..` containment** — `..` pops an in-call **trail** (the stack of resolved
  Spoors) and can never resolve above `root_spoor` (the chroot/pivot boundary, I-1).
- **Lifetime** — each resolved Spoor is a clone on the **trail**; at return,
  `unwind` `spoor_clunk`s every trail entry except the returned **quarry**.
- **No batching in v1.0** — one component per `Dev.walk` with a kernel X-check
  each hop (correctness over the dev9p deep-path round-trip cost; component
  batching into one `Twalk` is a v1.x perf optimization).

First consumer: **namespace-resident `/srv`** — `devsrv` mounted per-territory
over a refcounted `SrvRegistry` (D7); `open(/srv/<name>)` connects (9p-mode → a
dev9p root; byte-mode → a stream Spoor); for corvus the path is **two-step**
(`open(/srv/corvus)` → root, then walk `ctl` relative to it — `stalk` does no
blocking connect mid-resolution; D5); a **per-session service table** makes a
second user's coordinator *unnameable* (the A-5b isolation foundation — the global
flat registry cannot host per-user same-named coordinators). Posting moves to
`create(/srv/<name>)` with a `DMSRVBYTE` `perm` bit (D2/D6); `SYS_SRV_CONNECT` /
`SYS_POST_SERVICE` / `_BYTE` retire (D3, subsumed by `SYS_OPEN` /
`SYS_WALK_CREATE`). Symbolic bind-in-walk is deferred to v1.x (D1, no v1.0
consumer). Sub-chunks: stalk-1 (resolver core + `SYS_OPEN`) → stalk-2 (mount
re-key + crossing) → stalk-3 (devsrv per-territory + `/srv` + retire; split
3a/3b/3c per STALK-DESIGN §5.4), each its own audit. The new invariant is **I-28**.

### 9.7 Open design questions

None at Gate 3.

### 9.8 Summary

Per-process territory, `bind` / `attach_9p` / `mount` / `unmount` as the only composition operations. `Dev` vtable for kernel devices; `dev9p` is a Dev whose vtable proxies to the kernel 9P client — userspace drivers and remote filesystems both present as Spoors with no semantic distinction. Standard `/dev/`, `/proc/`, `/ctl/` paths. Driver crash recovery via process supervision.

---

## 10. IPC

**STATUS**: COMMITTED

### 10.1 9P as the universal IPC

There is no separate IPC mechanism. All inter-process communication is mediated by 9P: one process mounts another's 9P server and reads/writes files. Pipes are 9P streams. Shared memory is a 9P file backed by anonymous memory (with BURROW handles for zero-copy). Message queues are 9P files.

This is the Plan 9 model, adopted unchanged.

### 10.2 9P dialect: 9P2000.L + Stratum extensions

**Dialect**: 9P2000.L (the Linux-extended dialect; the `.L` distinguishes it from Plan 9's 9P2000). Includes the L-extension messages: `Tlopen`, `Tlcreate`, `Tsymlink`, `Tmknod`, `Trename`, `Treaddir`, `Tlock`, `Tgetlock`, `Tlink`, `Tmkdir`, `Trenameat`, `Tunlinkat`, `Tgetattr`, `Tsetattr`, `Treadlink`, `Tstatfs`, `Tfsync`, `Tflush`, `Txattrwalk`, `Txattrcreate`. Covers POSIX file modes properly (vs vanilla 9P2000's restricted set); the `.L` is what Linux's v9fs speaks to userspace 9P servers and what Stratum's `stratumd` exposes per `stratum/v2/docs/reference/20-9p.md`.

**Stratum extensions** (committed in Stratum v2; stable per `stratum/v2/docs/OS-INTEGRATION.md §13` ABI envelope):
- `Tsync` / `Rsync` — explicit sync barrier on a fid. Drains the dirty buffer through the three-phase commit; returns when durability is established. Maps to Linux's `fsync(2)` for files and `syncfs(2)` for the connection.
- `Treflink` / `Rreflink` — `copy_file_range` with reflink semantics. **Single-dataset only at v2.x**; cross-dataset is gated on Stratum's per-dataset rekeying primitive (Stratum-upstream roadmap, returns `STM_EXDEV` until then).
- `Tbind` / `Rbind` — per-connection subvolume composition (within the Stratum connection's territory). Composes a Stratum subvolume into the connection's view; the Thylacine territory layer composes ACROSS Stratum connections (e.g., Stratum + a network FS).
- `Tunbind` / `Runbind` — undo the above.
- `Txattrwalk`, `Txattrcreate`, `Tgetxattr`, `Tsetxattr`, `Tlistxattr`, `Tremovexattr` — POSIX xattr surface end-to-end. Sufficient to carry SELinux contexts (`security.selinux`), IMA signatures (`security.ima`), and arbitrary user xattrs.

Extensions Thylacine does NOT speak at v1.0 (deferred per Stratum upstream or out-of-scope):
- `Tpin` / `Tunpin` — snapshot pin/release for the connection. Pinning is currently handled via `/ctl/datasets/<id>/{hold,release}-snapshot` admin verbs on the `/ctl/` socket, which Thylacine consumes through `/srv/stratum-ctl/`. The protocol-level `T(un)pin` may land in a later Stratum release.
- `Tfallocate` — `fallocate` with `FALLOC_FL_*` flags. Stratum exposes the full set (PUNCH/COLLAPSE/INSERT/ZERO/UNSHARE) via POSIX `fallocate(2)` on the v9fs layer; the protocol-level `Tfallocate` is not currently a distinct message in Stratum v2's wire surface — POSIX `fallocate` is plumbed through the standard 9P2000.L attribute paths. If a future Stratum release introduces a dedicated `Tfallocate` for richer semantics, Thylacine adds it then.

These are negotiated at session establishment via the standard 9P2000.L `Tversion` capability-flag mechanism; clients that don't support them fall back to the L-baseline. Thylacine's kernel 9P client negotiates the full Stratum extension set at every session attach.

### 10.3 Pipes

Pipes are kernel-implemented 9P streams:
- `pipe(fd[2])` creates a pair of `Spoor`s backed by a kernel ring buffer.
- Read end has read-only mode; write end has write-only.
- Reads block when empty; writes block when full.
- EOF on write end causes EOF on read end.
- Buffer size: `PIPE_BUF = 4 KiB` (matching POSIX guarantee for atomic writes).

Pipes are the primary mechanism for connecting processes. Shell pipelines compose processes via pipes, exactly as in Unix.

For very large inter-process data transfer (GiB-class), BURROW-based shared-memory regions transferred via 9P sessions (§19) are the zero-copy alternative.

### 10.4 Notes (signals)

Plan 9's note mechanism is the internal signal model (covered in §7.6).

The POSIX compat layer translates POSIX signals to notes and vice versa (§16.4).

### 10.5 Rendezvous

Plan 9's `rendezvous(tag, val)` is a synchronous handoff — two threads on the same `tag` exchange `val`s. The first to call blocks; the second wakes the first and proceeds.

Used for: tightly-coupled producer/consumer (e.g., a kernel thread waiting for a userspace driver's reply); synchronization without shared memory; the underlying primitive for `futex` translation (§16).

### 10.6 Open design questions

None at Gate 3.

### 10.7 Summary

9P everywhere. Pipes, notes, rendezvous as kernel-internal IPC implemented as 9P-shaped operations. No separate IPC API.

---

## 11. Syscall interface

**STATUS**: COMMITTED

### 11.1 Design

The syscall interface is Plan 9-heritage, not POSIX. The syscall table is small by design.

### 11.2 Core syscalls

| Syscall | Description |
|---|---|
| `open(name, mode)` | Open a file in the territory |
| `close(fd)` | Close a file descriptor |
| `read(fd, buf, n)` | Read from fd |
| `write(fd, buf, n)` | Write to fd |
| `pread(fd, buf, n, off)` | Read at offset |
| `pwrite(fd, buf, n, off)` | Write at offset |
| `seek(fd, offset, type)` | Reposition fd |
| `stat(name, buf, n)` | Stat a file |
| `wstat(name, buf, n)` | Write file metadata |
| `fstat(fd, buf, n)` | Stat an open fd |
| `create(name, mode, perm)` | Create a file |
| `remove(name)` | Remove a file |
| `bind(old, new, flags)` | Bind path-to-path in territory (Plan 9 symbolic bind) |
| `attach_9p(transport_fd, aname, n_uname) → spoor_fd` | Wrap a byte-pipe Spoor in a kernel 9P client; return a Spoor representing the 9P tree's root (§9.6) |
| `mount(source_spoor_fd, target_path, flags) → 0` | Graft a Spoor's tree at `target_path`. Source can be any Spoor — `dev9p`-backed from `attach_9p`, kernel-Dev-backed, or a sub-tree (§9.6) |
| `unmount(target_path)` | Remove a mount entry; release the Spoor ref |
| `rfork(flags)` | Create process/thread |
| `exec(name, argv)` | Replace process image |
| `exits(msg)` | Terminate process |
| `wait(waitmsg)` | Wait for child |
| `sleep(ms)` | Sleep milliseconds |
| `burrow_attach(length) → vaddr` | Attach an anonymous Burrow of `length` bytes (page-rounded) into the caller's address space at a kernel-chosen address; RW, demand-zero. The v1.0 native memory primitive — the substrate for `libt`'s and pouch's `malloc` (§6.5, Tier 1). `brk` and Linux-shaped / file-backed `mmap` are not provided. |
| `burrow_detach(vaddr, length) → 0` | Detach a region returned by `burrow_attach`. Exact-match — no partial detach at v1.0. |
| `pipe(fd[2])` | Create pipe |
| `dup(oldfd, newfd)` | Duplicate fd |
| `poll(fds, nfds, timeout) → ready count` | Wait until ≥1 of `nfds` descriptors is ready, or `timeout` (ms) elapses; the multi-fd wait primitive (§23.3) |
| `noted(v)` | Note handler return |
| `notify(fn)` | Register note handler |
| `postnote(pid, msg)` | Post a note to a process |
| `rendezvous(tag, val)` | Synchronous handoff |
| `nsec()` | Nanosecond clock |
| `getpid()` | Process ID |
| `gettid()` | Thread ID |
| `errstr(buf, n)` | Read per-thread errstr |

### 11.2b Corvus hardening syscalls (v1.0, per CORVUS-DESIGN.md §4.1.1)

Required by `corvus` and per-user `stratumd` at startup; land at P5-corvus-syscalls.

| Syscall | Description |
|---|---|
| `mlockall(flags)` | Pin all current + future-mapped pages. Caller must hold `CAP_LOCK_PAGES`. |
| `set_dumpable(0)` | Disable core-dump for this Proc. Idempotent; no-op if already 0. |
| `set_traceable(0)` | Refuse any future debug-Spoor attach to this Proc. One-way (no re-enable). |
| `explicit_bzero(ptr, len)` | Compiler-barrier'd memset that the optimizer can't elide. Userspace can call its own; this syscall is the libc-portable equivalent for non-musl callers. |
| `getrandom(buf, len, flags)` | Read from kernel CSPRNG. Blocks until seeded; non-blocking via `GRND_NONBLOCK` flag. Caller must hold `CAP_CSPRNG_READ`. |

### 11.2c Service-registry syscalls (v1.0, per CORVUS-DESIGN.md §6)

The `/srv` transport (`devsrv`, §9.4) by which a userspace 9P server — `corvus` at v1.0 — is reached per-connection with a kernel-stamped peer identity. Land at P5-corvus-srv-impl-a.

| Syscall | Description |
|---|---|
| `post_service(name) → service_handle` | Register the caller as the 9P server for `/srv/<name>`. The kernel creates and owns all transport. Posting/rebinding a name is gated on a one-way joey-stamped `proc_flags` bit (`PROC_FLAG_MAY_POST_SERVICE`); on server death the name is tombstoned, not freed. |
| `srv_accept(service_handle) → connection_handle` | Block until a client opens `/srv/<name>`; receive the server end of one fresh kernel-minted per-client connection. Bounded accept queue. |
| `srv_peer(connection_handle) → {stripes, console, caps}` | Read the connection's kernel-stamped peer identity: the immutable `stripes` tag + console-attachment bit (captured by value at bind), and the live `caps` (read under the process-table lock, fail-closed for an exited peer). Gated to the service's poster. |
| `srv_connect(name, path) → connection_handle` | Client-side open of `/srv/<name>/<path>`. Composes the kernel-side connection mint + the synchronous `Tversion + Tattach + optional Twalk + Tlopen` handshake. Returns a non-transferable `KObj_Srv` handle whose `client_fid` is ready for `SYS_READ` / `SYS_WRITE`. Per-Proc cap = 1. |
| `spawn_with_perms(name, fds, fd_count, cap_mask, perm_flags)` | Spawn variant that atomically stamps `SPAWN_PERM_*` bits on the child Proc inside the spawn thunk (before `exec_setup`). The v1.0 bit is `SPAWN_PERM_MAY_POST_SERVICE`, which stamps `PROC_FLAG_MAY_POST_SERVICE`. Granting `SPAWN_PERM_CONSOLE_TRUSTED` requires the caller be console-attached (the local-console / SAK trust anchor — joey); `SPAWN_PERM_MAY_POST_SERVICE` may be granted by a console-attached caller OR by a current holder (the one-hop delegation, A-5b #827b — see the paragraph below). This is joey's race-free path to confer post-service authority on `/sbin/corvus`, and login's path to stand up per-user `--role client` proxies. |
| `walk_open(spoor_fd, name, omode) → opened_fd` | Walk one path component from a `KOBJ_SPOOR` and open the result, returning a new opened `KOBJ_SPOOR` fd. The v1.0 minimum walk-through-mount primitive — dev-agnostic (dispatches through `dev->walk` + `dev->open`), single-component only (no '/' splitting, no '.' / '..'). Composes with `attach_9p` to let userspace read files served by a 9P server. Multi-component path resolution + traversal land with the production `open(name, mode)` namec walker. The `spoor_fd == -1` sentinel (`SYS_WALK_OPEN_FROM_ROOT`) walks from the caller's pivoted Territory `root_spoor` instead of a handle (P5-stratumd-stub-bringup-e2). Lands at P5-stratumd-stub-bringup-e1. |
| `chroot(spoor_fd) → 0` | Stamp the calling Proc's Territory `root_spoor` to the given `KOBJ_SPOOR`. The pivoted root is the Spoor at which name resolution starts when `walk_open` is called with the `-1` sentinel. The Territory takes its own refcount on the source Spoor (the caller may close `spoor_fd` afterward; the Territory holds the Spoor alive until either a subsequent `chroot` replaces it or Territory destruction releases it). Idempotent: re-chroot to the same Spoor is a no-op success. Per `CORVUS-DESIGN.md §10.1` — v1.0 chroot mechanism (full pivot at v1.x). Spec: `specs/territory.tla::Chroot`. Audit-trigger surface: `kernel/territory.c`. Lands at P5-stratumd-stub-bringup-e2. |
| `set_tid_address(tidptr) → tid` | Return the calling thread's tid — the calling Proc's pid at v1.0 (single-threaded Procs; the Linux thread-group-leader convention that the leader's tid equals the pid). A C runtime (pouch — the Thylacine POSIX libc) calls this once at thread startup. `tidptr` (the clear-child-tid address) is accepted but neither stored nor acted on at v1.0 — its exit-time semantics (clearing `*tidptr` + a futex wake) are observable only with multiple threads (POUCH-DESIGN.md §12.4); the pouch-threads sub-chunk wires it. Never fails for a userspace caller. Lands at P6-pouch-kernel-auxv. |

`stripes` is a `u64` per-Proc identity tag — a monotonic counter assigned fresh at `proc_alloc`, immutable for the Proc's life, `0` reserved as a fail-closed sentinel. A `/srv` connection Spoor is a `KObj_Srv` handle (§18.2), non-transferable.

`PROC_FLAG_MAY_POST_SERVICE` is structurally NOT a cap: it is **kernel-stamped only at spawn time** (the `spawn_with_perms` thunk above) so the bit cannot be propagated by `rfork` — the same discipline as `PROC_FLAG_CONSOLE_ATTACHED` (§5.5 of CORVUS-DESIGN). The pre-spawn stamp is what avoids the race window a post-spawn `mark()` syscall would open between the parent's mark and the child's first `SYS_POST_SERVICE` call.

**One-hop delegation (A-5b #827b, user-voted 2026-06-04).** The spawn-time grant gate is *per-bit*: a Proc that *already holds* `PROC_FLAG_MAY_POST_SERVICE` may confer it on a child (in addition to a console-attached granter). This lets a non-console-attached but trusted session authority — `/sbin/login`, which the console-attached joey spawns *with* the bit — stand up per-user `--role client` proxies that post `/srv/home-<user>` into the session's private (per-territory, stalk-3 / I-1) `/srv`. The grant is still never `rfork`-propagated (each hop is an explicit `perm_flags` decision, not a `cap_mask` bit) and is rooted at the console-attached joey; the delegation is exactly one hop in intent (login → proxy) though the kernel does not cap the hop count — soundness rests on *who holds the bit*, and the only holders are joey's deliberately-conferred OS servers. `SPAWN_PERM_CONSOLE_TRUSTED` (the SAK trust anchor) is **NOT** delegable: it stays console-attached-only, so a service-poster can never confer the console-trust used for elevation (I-27 unchanged).

**Init is the persistent grant-root (A-5b #827b-β).** joey relinquishes its console-attach at the bringup→session boundary (I-27), but the getty then spawns a *fresh* `/sbin/login` per session that must each receive `MAY_POST_SERVICE` — and a non-console-attached granter can confer it only by holding it (the per-bit gate above). So `joey_thunk` stamps `PROC_FLAG_MAY_POST_SERVICE` on init (alongside its console-attach + console-owner stamps): init is the persistent service-posting grant-root, exactly as it is the console-trust and capability root. Init holding the bit grants nothing to its children automatically (it is never `rfork`-propagated; each spawn decides per `perm_flags`), and init never confers `CONSOLE_TRUSTED` — so I-2 and I-27 are both untouched. This is the post-relinquish completion of the one-hop delegation: console-attached-joey-during-boot **and** holder-joey-after-relinquish are both valid grant-roots for `MAY_POST_SERVICE`.

### 11.2d Filesystem-mutation syscalls (the FS foundation; IDENTITY-DESIGN.md §9.2 + §9.3)

The create / durability / enumeration trio (FS-alpha+beta, §9.2) plus the rename + unlink pair (FS-gamma, §9.3). Pulled forward ahead of the corvus identity-DB persistence (A-1b) per the convergence-detour sequencing: real persistence needs them, and the A-2 coreutils + the shell need them shortly after. The kernel 9P client already implements the wire half for all five; these are the syscall wrappers + the real `dev9p_create` / `dev9p_rename` / `dev9p_unlink` + four new `Dev` vtable slots (`.fsync`, `.readdir`, `.rename`, `.unlink`). Numbers continue from `SYS_PIVOT_ROOT = 53`. The rename + unlink pair is the substrate for A-1b's atomic-swap DB persistence (`write tmp → fsync → rename-swap → dir-fsync`); `p9_client_renameat` / `unlinkat` were implemented in Phase 5 but never exercised, so FS-gamma's audit is their first end-to-end prosecution.

| Syscall | Description |
|---|---|
| `walk_create(parent_fd, name, name_len, omode, perm) → opened_fd` | Create-then-open the single component `name` in directory `parent_fd` (`KOBJ_SPOOR`, `RIGHT_WRITE`; or the `-1` FROM_ROOT sentinel), returning a new opened `KOBJ_SPOOR` fd (`R\|W\|TRANSFER`). The create-sibling of `walk_open`. `perm`'s low 9 bits are the rwxrwxrwx mode; the `DMDIR` bit (`0x80000000`) selects a directory (`Tmkdir`) instead of a file (`Tlcreate`), other `DM*` bits reserved. Dispatches `dev->create` (real `dev9p_create` → `p9_client_lcreate`/`mkdir`, carrying the caller's `primary_gid` into the 9P `gid` field). Ownership-on-create attribution and per-file rwx enforcement are A-2 (this is the create MECHANISM). `SYS_WALK_CREATE = 54`. |
| `fsync(fd, datasync) → 0` | Durability barrier on `fd` (`KOBJ_SPOOR`, `RIGHT_WRITE`). `datasync` 0 = full, 1 = data-only. New `Dev.fsync` slot → `dev9p_fsync` → `p9_client_fsync` (Stratum `Tsync`); in-memory Devs no-op success. The "write-then-fsync = durable" contract on the integrity FS. `SYS_FSYNC = 55`. |
| `readdir(fd, buf, buf_len) → bytes` | Read the next run of directory entries from `fd` (`KOBJ_SPOOR` on a directory, `RIGHT_READ`) into `buf` (≤ `SYS_RW_MAX`), advancing the Spoor's offset; 0 bytes = end-of-directory. Buffer is the raw 9P2000.L `Treaddir` dirent stream (`qid + offset + type + name_len + name` per entry); the caller parses it (a native `struct t_dirent` is a v1.x seam). New `Dev.readdir` slot → `dev9p_readdir` → `p9_client_readdir`. `SYS_READDIR = 56`. |
| `rename(olddir_fd, oldname, oldname_len, newdir_fd, newname, newname_len) → 0` | Atomically rename/move a single component from `olddir_fd` to `newdir_fd` (both `KOBJ_SPOOR` directories, `RIGHT_WRITE`; or the `-1` FROM_ROOT sentinel). POSIX `rename(2)` / 9P2000.L `Trenameat` — an existing destination is **atomically replaced** (the property A-1b's DB-swap relies on). Both dir fds are used directly (no clone-walk — renameat operates on the dirfid by name without transitioning it, unlike create); they must be on the same Dev/session (cross-Dev → -1). New `Dev.rename` slot → `dev9p_rename` → `p9_client_renameat`. `SYS_RENAME = 57`. |
| `unlink(parent_fd, name, name_len, flags) → 0` | Remove a single component (non-directory, or empty directory) from `parent_fd` (`KOBJ_SPOOR` directory, `RIGHT_WRITE`; or FROM_ROOT). `flags` 0 = unlink a non-directory; `SYS_UNLINK_REMOVEDIR` (`0x200`, mirrors `P9_UNLINK_AT_REMOVEDIR`) = rmdir an empty directory; other bits reserved → reject. New `Dev.unlink` slot → `dev9p_unlink` → `p9_client_unlinkat`. `SYS_UNLINK = 58`. |

All five are audit-trigger surfaces (§25.4): the create + write + fsync path is the AEGIS/mallocng-adjacent surface flagged for a focused round, and rename + unlink are the first end-to-end exercise of `p9_client_renameat` / `unlinkat` (FS-gamma). The per-file rwx-permission enforcement (no id bypass, I-22) is A-2d, not this foundation.

### 11.3 Handle syscalls

Per §18:

| Syscall | Description |
|---|---|
| `handle_close(h)` | Release a handle |
| `handle_rights(h)` | Query rights on a handle |
| `handle_reduce(h, rights)` | Return new handle with reduced rights |
| `mmap_handle(h, addr, len, prot)` | Map a BURROW or MMIO handle |
| `irq_wait(h)` | Block until IRQ handle fires; returns count |
| `burrow_create(size, flags)` | Create anonymous BURROW |
| `burrow_create_physical(paddr, size)` | Create physical BURROW (privileged) |
| `burrow_get_size(h)` | Query BURROW size |
| `burrow_read(h, buf, off, len)` | Read from BURROW without mapping |
| `burrow_write(h, buf, off, len)` | Write to BURROW without mapping |

The `burrow_*` rows above are the **Tier 2** memory surface (§6.5): a Burrow held as a first-class `KObj_Burrow` handle, for *shared*, *named*, or *transferable* memory — distinct from the handle-free Tier 1 `burrow_attach` / `burrow_detach` (§11.2). Tier 2 is design intent; it lands when a native workload needs shared memory, not at v1.0.

### 11.4 POSIX compat surface

POSIX syscalls that don't map cleanly to the above are implemented in the **compat layer** — the musl port + a thin kernel shim that translates POSIX calls into Thylacine primitives. The kernel is not required to implement POSIX directly.

**Compat layer strategy** (per `VISION.md §12`):

1. **musl port**: musl libc is ported to the Thylacine syscall interface. Programs compiled against musl run natively. This is the primary compat path.
2. **Linux ARM64 binary compat**: a thin kernel shim intercepts `SVC #0` with Linux syscall numbers and routes them to Thylacine equivalents. Supports static binaries and musl-linked binaries. glibc-dynamic binaries: best-effort.

### 11.5 Linux syscall coverage at v1.0

The translation shim covers the top ~50 Linux ARM64 syscalls by frequency of use in typical programs (covered in §16.3). High-priority surface:

- File I/O: `openat`, `close`, `read`, `write`, `pread`, `pwrite`, `lseek`, `dup3`, `pipe2`.
- Filesystem: `newfstatat`, `fstat`, `getdents64`, `mkdirat`, `unlinkat`, `renameat2`, `symlinkat`, `linkat`, `readlinkat`, `fallocate`, `copy_file_range`.
- Process/thread: `clone`, `execve`, `wait4`, `exit_group`, `getpid`, `gettid`, `set_tid_address`.
- Memory: `mmap`, `munmap`, `brk`, `mprotect`.
- Timing: `clock_gettime`, `clock_nanosleep`, `nanosleep`.
- Signals: `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn`, `kill`, `tgkill`.
- Threading: `futex`.
- I/O multiplexing: `poll`, `pselect6`.
- POSIX modern: `name_to_handle_at`, `open_by_handle_at`, `flock`, `fcntl` (`F_SETLK`, `F_GETLK`, `F_OFD_SETLK`).
- Misc: `prlimit64`, `getrlimit`, `setrlimit`, `getrandom`, `getcwd`, `chdir`.

Out at v1.0: `epoll_*`, `inotify_*`, `io_uring_*`, `bpf`, `perf_event_open`, `ptrace`. These either need their own design (`epoll`) or are excluded entirely (`bpf`, `perf_event_open`).

### 11.6 Syscall ABI

ARM64: x0-x7 carry up to 8 args; x8 carries the syscall number; `SVC #0` traps. Return value in x0; second return (e.g., for `pipe`) in x1. Errors signaled by negative x0 with errno in `errstr` (Plan 9 style); musl translates to negative-errno return.

The kernel uses a monotonic syscall number territory; Linux syscall numbers don't overlap (kernel has its own numbering). The Linux shim has a separate entry path that decodes Linux numbers and dispatches.

### 11.7 Open design questions

None at Gate 3.

### 11.8 Summary

Plan 9-heritage syscall surface, ~30 native syscalls. Linux compat via musl + kernel shim covering ~50 Linux syscalls. POSIX compat via musl translation. epoll / inotify / io_uring deferred post-v1.0.

---

## 12. Interrupt and exception handling

**STATUS**: COMMITTED

### 12.1 ARM64 exception model

ARM64 has four exception levels. Thylacine uses:
- **EL0**: userspace.
- **EL1**: kernel.
- EL2/EL3: not used at v1.0.

Exception types handled:
- **Synchronous**: syscall (`SVC #0`), page fault (Data Abort, Instruction Abort), illegal instruction, alignment fault, BTI mismatch, PAC mismatch.
- **IRQ**: hardware interrupts, routed via GIC.
- **FIQ**: fast interrupts (used for secure world only; not used at v1.0).
- **SError**: system errors (async, hardware-specific). v1.0 logs and panics.

**Stack-pointer discipline (SPSel) — the kernel runs uniformly at EL1h.** The
kernel executes with `PSTATE.SPSel = 1` at all times; the active stack pointer
is always `SP_EL1`. Each thread's `SP_EL1` is its own kernel stack — an
exception taken while in the kernel builds its register frame on that same
per-thread stack, and an exception taken from EL0 hardware-switches to the
(per-thread) kernel `SP_EL1`. `SP_EL0` is therefore *exclusively the userspace
stack*; the kernel never executes with `SP_EL0` selected. This is invariant
**I-21** (§28).

This is a deliberate single-mode discipline. The alternative — running normal
kernel code at **EL1t** (`SPSel=0`, `sp = SP_EL0`) and entering EL1h only
transiently inside exception handlers — was implemented during early SMP
bring-up (P2-Cc) and proved unsound. With two modes, a thread's execution mode
becomes a function of scheduling history rather than a fixed property:
`cpu_switch_context` saves/restores `SP` but not `SPSel`, so a thread resumes
in whatever mode the *outgoing* thread left the CPU in. Under SMP work-stealing
this let a thread resume an exception-return path (`KERNEL_EXIT`) in the wrong
mode, where `MSR SP_EL0` against the *currently-selected* stack pointer is
CONSTRAINED UNPREDICTABLE — observed as an `EC=0` Undefined-instruction trap on
the QEMU target, silently killing a secondary CPU on every boot. A per-CPU
exception stack is also fundamentally incompatible with migrating a thread that
is mid-exception (its frame cannot follow it across CPUs). Running uniformly at
EL1h removes the mode entirely: one stack bank, exception frames travel with
the thread (migration-safe), and `MSR SP_EL0` is always a write to the
non-current bank (architecturally well-defined). The P2-Cc EL1t implementation
was corrected to this model in Phase 5 (`P5-el1h-kernel`); §12.2's vector table
already reflected the EL1h-kernel intent.

### 12.2 Exception vector table

Standard ARM64 exception vector layout (`arch/arm64/vectors.S`):

```
.align 11               ; 2 KiB-aligned
vectors:
  ; current EL with SP_EL0
  .org 0x000  ; Synchronous EL1t
    b sync_invalid
  .org 0x080  ; IRQ EL1t
    b irq_invalid
  .org 0x100  ; FIQ EL1t
    b fiq_invalid
  .org 0x180  ; SError EL1t
    b serror_handler

  ; current EL with SP_ELx
  .org 0x200  ; Synchronous EL1h
    b sync_kernel
  .org 0x280  ; IRQ EL1h
    b irq_kernel
  .org 0x300  ; FIQ EL1h
    b fiq_kernel
  .org 0x380  ; SError EL1h
    b serror_handler

  ; lower EL using AArch64 (EL0)
  .org 0x400  ; Synchronous EL0
    b sync_user
  .org 0x480  ; IRQ EL0
    b irq_user
  .org 0x500  ; FIQ EL0
    b fiq_user
  .org 0x580  ; SError EL0
    b serror_handler
```

Because the kernel runs uniformly at EL1h (§12.1), the **current EL with
SP_EL0** group (`0x000`–`0x180`, the "EL1t" slots) is never the entry path for
a kernel exception — those slots route to an `unexpected`-vector diagnostic
that extincts (a kernel exception arriving on the EL1t group means `SPSel` was
somehow cleared, which is a soundness violation). Kernel exceptions always
enter via the **current EL with SP_ELx** group (`0x200`–`0x380`); userspace
exceptions via the **lower EL** group (`0x400`–`0x580`).

### 12.3 Interrupt controller

**QEMU `virt` target**: GIC v2 or v3 (autodetected via DTB compatible string).

The kernel implements (`arch/arm64/gic.c`):
- GIC distributor init: enable, priority mask, routing per-CPU.
- GIC CPU interface: ack (read `IAR` to get IRQ ID), EOI (write `EOIR`).
- IRQ → handler dispatch table (`irq_attach(int irqno, void (*handler)(int, void *), void *arg)`).
- Per-IRQ enable / disable / mask / affinity.
- Per-CPU GIC initialization on secondary CPU bring-up.

GIC v2 vs v3: v2 has 8 CPU interfaces max; v3 supports up to 256 with the redistributor model. v1.0 supports both via separate ops tables.

### 12.4 IRQ to userspace forwarding

Userspace drivers register for IRQs via the `KObj_IRQ` handle (§18). The kernel's IRQ infrastructure:

```c
void kernel_irq_handler(int irq) {
    struct irq_desc *desc = irq_descs[irq];
    if (desc->kobj) {
        // Wake any process blocked on irq_wait
        atomic_inc(&desc->kobj->fire_count);
        wakeup_irq_kobj(desc->kobj);
    } else if (desc->kernel_handler) {
        desc->kernel_handler(irq, desc->arg);
    } else {
        panic("unhandled IRQ %d", irq);
    }
    gic_eoi(irq);
}
```

Userspace driver:
```c
fire_count = irq_wait(my_irq_handle);  // blocks; returns count of fires
// process the device's IRQ register
```

Latency budget (VISION §4.5): IRQ to userspace handler p99 < 5µs. ARM64 exception handling is clean enough to achieve this on Apple Silicon and QEMU.

### 12.5 Open design questions

None at Gate 3.

### 12.6 Summary

ARM64 EL0/EL1 model; GIC v2/v3 autodetected; standard vector table; IRQ-to-userspace via `KObj_IRQ` handles; sub-5µs handler latency target.

---

## 13. Memory-mapped I/O and VirtIO

**STATUS**: COMMITTED

### 13.1 VirtIO

VirtIO is the primary device interface for the QEMU `virt` target. All storage, network, GPU, and input devices are VirtIO.

**Transport**:
- VirtIO-MMIO: primary for `virt` (block, net, input).
- VirtIO-PCI: needed for VirtIO-GPU (PCI-only in QEMU).

**Kernel-side VirtIO core** (`drivers/virtio-core/` — runs in-kernel):
- Virtqueue management (split virtqueue at v1.0; packed virtqueue post-v1.0).
- Descriptor chain allocation and completion tracking.
- IRQ hookup via GIC.
- Per-device control registers (`/dev/virtio/<n>/`).

**Userspace VirtIO device drivers**: receive `KObj_MMIO` handles for the device's BAR/MMIO range, plus `KObj_IRQ` for the device's interrupt line, plus `KObj_DMA` for DMA buffers. Implement the device-specific protocol on top of the kernel-exported VirtIO core.

There is **no in-kernel VirtIO device driver code** at v1.0. virtio-blk, virtio-net, virtio-input, virtio-gpu are all userspace from Phase 3 onward.

### 13.2 PCIe enumeration (minimal)

`virt` has a PCIe root complex for VirtIO-GPU. Minimal PCIe enumeration in the kernel:
- Type 0 (endpoint) and Type 1 (bridge) header parsing.
- BAR allocation (linear, no remapping).
- IRQ assignment (read INT line; map via `intc` DTB property).
- Capability list parsing for VirtIO-PCI.

No PCIe hot-plug at v1.0. No SR-IOV, no PASID, no advanced extensions.

### 13.3 DMA

ARM64 IOMMU (SMMU) is **not used at v1.0**; v2.0 candidate for security (driver-process isolation from kernel).

CMA (Contiguous Memory Allocator) reserves a pool of physically contiguous low-address pages at boot for DMA. `KObj_DMA` handles wrap CMA allocations.

### 13.4 Open design questions

None at Gate 3.

### 13.5 Summary

In-kernel VirtIO transport core; userspace VirtIO device drivers from day one. PCIe minimal; no IOMMU at v1.0.

---

## 14. Filesystem integration

**STATUS**: COMMITTED

### 14.1 Stratum as native filesystem

Stratum is the native filesystem. It runs as a userspace daemon (`stratumd`, one process per pool) bound to a Unix socket (within the Thylacine guest VM the socket is a Spoor that the kernel 9P client routes through). The kernel mounts it at `/` (or specified mount point) via the standard `mount` syscall, which translates to a 9P `Tattach` over the FS socket.

**Stratum v2 is feature-complete and shipping** (2026 Q2). The integration consumes Stratum's stable ABIs documented in `stratum/v2/docs/OS-INTEGRATION.md`:

| Stratum ABI | Thylacine consumer |
|---|---|
| 9P2000.L wire + Stratum extensions (Unix socket) | Kernel 9P client at `kernel/9p_*` (the primary integration; recommended path per OS-INTEGRATION.md §3) |
| `libstratum-9p` C ABI | Userspace tools that need direct 9P access (typically reach the same data via the kernel's mount; the library is optional for Thylacine) |
| `libstm_fs` in-process C ABI | NOT consumed (UNSTABLE per Stratum's ABI envelope; the in-process bypass is reserved for the case where the process is provably the sole writer — not applicable to Thylacine's mount model) |

See `VISION.md §11` for the coordination story.

### 14.2 9P client (kernel)

The kernel implements a 9P2000.L client with the Stratum extensions enumerated in §10.2 above. Pipelined per §21 — the elected-reader pipeline (§21.10 records the P5 serial regression and the #841 restoration; the P5-client shipped a single-in-flight serial stand-in that §21.10 replaces).

Implementation (`kernel/9p_*` — split across `9p_wire.c`, `9p_session.c`, `9p_transport.c`, `9p_attach.c`): wire protocol encode/decode, fid management, tag pool, outstanding-request table, dispatch loop, Unix-socket transport. Spec: `specs/9p_client.tla`.

### 14.3 Per-process 9P session

At process creation, if the process inherits a Stratum mount, a fresh 9P connection is established. (Per VISION §11, one connection per Proc at v1.0; multiplexing is a v2.x optimization.) The connection is kept alive for the process's lifetime; closed at exit.

Stratum sees N connections, one per Thylacine process, and gives each its own per-connection territory (Stratum's per-connection fid namespace per `stratum/v2/docs/reference/20-9p.md`). Thylacine's per-process territory and Stratum's per-connection territory are complementary layers (VISION §11).

### 14.3a Boot lifecycle (Stratum-mounted root)

**v1.0 commitment (per CORVUS-DESIGN.md §3 D2)**: the **system pool** is integrity-only (Stratum Merkle, no AEAD); the `.key` sidecar uses the file backend, **cryptographically bound to the pool device's unique serial** (C-14). User datasets are AEAD-encrypted with corvus-managed per-user wrap chains, mounted post-pivot by **per-user stratumd processes** (one process per logged-in user; CORVUS-DESIGN §3 D4). The text below describes the OS-INTEGRATION.md-style sequence; CORVUS-DESIGN §3 D4 is the authoritative version for v1.0 Thylacine.

Canonical boot sequence per `stratum/v2/docs/OS-INTEGRATION.md §4`:

```
1. Boot firmware → kernel + initramfs.
2. initramfs:
   a. Find the pool device. e.g. /dev/vblk0 (virtio-blk) or the bare-metal NVMe.
   b. Locate the wrapped .key sidecar. ramfs-shipped at v1.0;
      tpm-sealed or hardware-token-stored at v1.x; Argon2id-from-typed-input
      via janus.
   c. Unwrap the master key via janus (passphrase + Argon2id default).
   d. fork stratumd:
        stratumd --pool /dev/<…> \
                 --key /run/stratum.key \
                 --fs-listen  /run/stratum/fs.sock \
                 --ctl-listen /run/stratum/ctl.sock
   e. Wait until /run/stratum/fs.sock binds (the readiness signal — do
      NOT read the socket before it binds).
   f. mount -t 9p -o trans=unix,version=9p2000.L,uname=root,access=user,msize=8388608 \
        /run/stratum/fs.sock /sysroot
   g. Pivot root into /sysroot.
   h. Shred the in-memory unwrapped-key copy (explicit_bzero).
3. Post-pivot: stratumd survives, reparented to PID 1 inside the new namespace.
   - Optionally mount /run/stratum/ctl.sock at /srv/stratum-ctl/.
   - Optionally start slate as system or per-user daemon (see §14.5).
4. Long-running operations:
   - Periodic snapshot via /srv/stratum-ctl/datasets/<id>/create-snapshot.
   - Periodic scrub via /srv/stratum-ctl/pools/<uuid>/scrub-trigger.
```

**Failure-mode discipline** (the initramfs must surface these clearly):
- `STM_ECORRUPT` (Merkle mismatch) → refuse to boot, recovery shell.
- `STM_EBADTAG` (AEAD MAC failure) → refuse to boot.
- `STM_EBADKEY` (wrong .key) → prompt for re-unlock.
- `STM_EWEDGED` (fs marked wedged at prior unmount) → refuse to boot, run `stratum fs verify`.

**`.key` sidecar lifetime**: between unwrap and stratumd-consume, the key is `mlock`'d + `MADV_DONTDUMP`'d (hibernation-leak defense). After `stratumd` acknowledges, the initramfs shreds its copy.

### 14.3b Stratum `/ctl/` admin synthetic FS

Stratum exposes a second 9P surface (admin / observability) per `stratum/v2/docs/reference/22-ctl.md`. Thylacine consumes it as just another 9P tree, typically mounted at `/srv/stratum-ctl/`:

```
/srv/stratum-ctl/
├── version                            — daemon build info
├── state                              — fs counter dump
├── events                             — append-only audit log
├── pools/<pool-uuid>/
│   ├── status                         — Healthy / Degraded / Faulted
│   ├── scrub                          — scrub state + counters
│   ├── scrub-trigger                  — start / pause / resume / abort
│   ├── metrics/prometheus             — Prometheus exposition
│   └── devices/<id>/status            — per-device state
├── datasets/<dataset-id>/
│   ├── properties                     — effective properties
│   ├── set-property                   — set a property
│   ├── snapshots/<snap-id>            — per-snapshot info
│   ├── create-snapshot                — admin verb
│   ├── delete-snapshot                — admin verb
│   ├── hold-snapshot                  — admin verb
│   ├── release-snapshot               — admin verb
│   └── rollback-snapshot              — admin verb
├── debug/allocator-state/<device-id>  — diagnostic dump
└── admin/
    ├── peer                           — caller credentials (SO_PEERCRED)
    └── clear-events                   — clear audit log
```

Auth is via `SO_PEERCRED` on the Unix socket (stratumd reads the connecting process's UID and gates admin verbs server-side). Thylacine's kernel preserves this credential through the 9P attach path so the trust boundary is intact.

Coordination with Thylacine's `/ctl` (kernel admin Dev introduced at Phase 4 P4-D): the two surfaces are distinct namespaces. Thylacine's `/ctl` is OS-level (procs, memory, devices, kernel-base, sched); Stratum's `/ctl/` is storage-level (pools, datasets, snapshots, scrub). Userspace tools dial both as needed.

### 14.4 In-kernel Stratum driver — DESIGNED-NOT-IMPLEMENTED for v2.0

**Status**: design committed at Phase 0; implementation is post-v1.0.

**Motivation**: 9P-client mount adds one round-trip per FS operation. For root FS operations (where Stratum is the bottleneck), the round-trip is measurable. An in-kernel Stratum driver bypasses the 9P client by linking part of `libstratum.a` into the kernel — corresponding to Stratum's UNSTABLE `libstm_fs` C ABI per `stratum/v2/docs/OS-INTEGRATION.md §2`. The same caveat applies: this ABI is `STM_UB_VERSION`-bound; bypassing 9P trades portability + ABI stability for performance.

**Design**: Stratum's `libstratum.a` exposes a `stm_fs_*` C API. An in-kernel Stratum driver wraps this API as a kernel `Dev` registration:

```c
/* drivers/stratum-kernel/stratum.c (post-v1.0) */
struct Dev stratum_dev = {
    .dc = 's',
    .name = "stratum",
    .attach = stm_attach,        // calls stm_fs_mount
    .walk   = stm_walk,           // calls stm_fs_lookup
    .open   = stm_open,           // calls stm_fs_create_file or stm_fs_lookup
    .read   = stm_read,           // calls stm_fs_read_extent
    .write  = stm_write,          // calls stm_fs_write_extent
    /* ... */
};
```

The kernel mounts Stratum directly:
```c
mount("stratum:tank/root", "/", "stratum", 0, NULL);
```

Operations bypass the 9P round-trip; they're function calls into the linked-in `libstratum.a`.

**Constraints (v1.0 scaffolding for v2.0)**:
- The kernel's mount-syscall infrastructure has no 9P-specific assumptions baked in — any Dev registration can be the root.
- The 9P-client mount and the in-kernel Stratum mount expose the same Dev interface; userspace does not know the difference.
- Switching between them is a runtime choice via `/etc/fstab` or the `mount` syscall's third argument.

**Implementation effort estimate**: ~10-20 KLOC of new kernel code (mostly `Dev` ↔ `stm_fs_*` glue) + ~3-5 KLOC of build system work. Post-v1.0.

**Open at v2.0 entry**:
- Whether the in-kernel Stratum runs in kernel context (full speed) or in a "kernel container" (a separate kernel VA region with restricted privileges).
- Whether VMOs and direct mapping pass through unchanged or get a Stratum-specific fast path.

### 14.4a Slate integration (Halcyon-side consideration; v1.0 evaluation)

Stratum ships **slate** — a Plan-9-shaped TUI daemon that is itself a synthetic 9P filesystem (`stratum/v2/src/slate/`; design at `stratum/v2/docs/SLATE-DESIGN.md`). Clients interact with slate by reading/writing files in a virtual tree; the bundled `stratum tui` renderer is one consumer, but the substrate is intentionally programmable for AI agents and alternative renderers.

What slate provides:
- A FAR-Commander dual-pane file manager.
- Per-pane editor (RW at `/editor/`; save / quit / revert verbs).
- Snapshot graph view, integrity dashboard, volume map.
- A programmable surface: any client that can read/write files can drive slate. This is the substrate for AI-driven system administration.

**Open question for Halcyon (Thylacine Phase 9)**: adopt slate directly, OR build a Thylacine-native equivalent? Adoption gets a Plan-9-aesthetic system file manager with zero additional code; the slate schema contract is `stratum/v2/docs/SLATE-DESIGN.md`. Building a native equivalent gives more control over the visual contract but duplicates effort. The Halcyon design pass at Phase 9 entry should weigh the two — both options are scaffolded by the 9P-as-universal-composition thesis. **Deferral note**: this decision is Phase 9 work; the v1.0-rc.1 fallback (per `ROADMAP.md §10` / §11) does not depend on either path.

### 14.5 ramfs (early boot)

Before Stratum mounts, the kernel uses a simple in-memory filesystem (`fs/ramfs.c`):
- Per-inode list of pages.
- B-tree-keyed name → inode map per directory.
- No persistence; entirely in RAM.
- Mounted from cpio at boot.

Unmounted and freed once Stratum is running.

### 14.6 tmpfs

Same implementation as ramfs but persists for the system's lifetime. Mounted at `/tmp` and `/run`.

`/tmp` is typical user-tmp (lots of churn; cleared at boot — a fresh tmpfs is mounted).
`/run` is system runtime state (sockets, pid files, daemon state); cleared at boot.

### 14.7 Other filesystems

| Filesystem | Status | Notes |
|---|---|---|
| ramfs | In-kernel, v1.0 | Early boot only |
| tmpfs | In-kernel, v1.0 | Runtime temp storage |
| Stratum | Userspace 9P, v1.0 | Native persistent storage |
| 9P remote mount | Kernel 9P client, v1.0 | Mount any 9P server (incl. Stratum on another machine) |
| procfs | In-kernel synthetic Dev, v1.0 | `/proc/<pid>/` subtree |
| ctlfs | In-kernel synthetic Dev, v1.0 | `/ctl/` subtree |
| FAT32 | Userspace 9P, post-v1.0 | USB / boot-media compat |
| ext4 | Not planned | |
| In-kernel Stratum | Designed v1.0, impl v2.0 | §14.4 |

### 14.8 Open design questions

None at Gate 3.

### 14.9 Summary

Stratum (userspace 9P) for persistent storage; ramfs (kernel) for early boot; tmpfs for `/tmp` and `/run`; in-kernel Stratum driver designed for v2.0.

---

## 15. Security model

**STATUS**: COMMITTED

### 15.1 Territory isolation as the primary boundary

Each process's territory is private. By default, a child process inherits its parent's territory at `rfork` time, but modifications to the child's territory do not affect the parent's. **This is the primary isolation mechanism in Thylacine.**

A "container" is simply a process whose territory has been constructed to isolate it: a different root, a restricted `/dev/`, a private network server. No additional kernel mechanism is required (no cgroups, no AppArmor, no seccomp at v1.0).

Spec: `territory.tla` proves isolation.

### 15.2 Credentials

Each process has:
- **uid**: user identity (numeric).
- **gid**: primary group.
- **groups**: supplementary groups (up to 32).
- **capabilities**: bitmask of coarse-grained kernel capabilities (§7.7).

File access control: standard Unix DAC (owner/group/other × rwx bits). This matches Plan 9's model and is sufficient for v1.0.

Capabilities can only be reduced via `rfork`. Elevation requires the v2.0 factotum mechanism (§15.4).

**No UID 0 in v1.0** (per CORVUS-DESIGN.md §3 D5). Admin authority ("hostowner") is held by whoever holds the system passphrase + is console-attached. Privileged operations gate on `CAP_HOSTOWNER` — granted to a Proc only after corvus verifies the system passphrase from a console-attached session. No process runs as UID 0 by default; numeric UIDs are still allocated per user but no UID has implicit elevated privilege.

### 15.3 Capability set (v1.0)

Per §7.7. Coarse-grained, on two axes (per CORVUS-DESIGN.md §3 D5, §5.5.1):

**Fork-grantable capabilities** — members of `CAP_ALL`; conferred at Proc creation and monotonically reduced thereafter (I-2). kproc starts with `CAP_ALL`; `rfork_with_caps` confers a subset.
- `CAP_HW_CREATE` — required to create hardware handles (MMIO / IRQ / DMA). Phase 4 surface.
- `CAP_LOCK_PAGES` — required for `sys_mlockall`. Granted to corvus + per-user stratumd processes.
- `CAP_CSPRNG_READ` — required for `sys_getrandom`. Granted broadly (most userspace processes have legitimate use for randomness).
- `CAP_GRANT_HOSTOWNER` — authorizes writing the `cap` device's `grant` file. Held by corvus alone (joey confers it at corvus's spawn).

**Elevation-only capabilities** — deliberately excluded from `CAP_ALL`; no Proc holds one at creation, and `rfork` strips every elevation-only bit from the child, so one can never be conferred by fork.
- `CAP_HOSTOWNER` — admin authority; required for the corvus admin verbs (user-create, etc.). It enters a Proc's capability set only by redeeming a grant through the kernel `cap` device's `use` file, and only for a console-attached Proc.

**The `cap` device** — the kernel device through which the userspace key agent (corvus) confers `CAP_HOSTOWNER` on a console session, after corvus verifies the system passphrase. corvus writes a pending grant to `grant` (kernel-gated on `CAP_GRANT_HOSTOWNER`); the target Proc redeems it through `use` (kernel-gated on `PROC_FLAG_CONSOLE_ATTACHED`). Two-phase and file-mediated, modelled on Plan 9's `cap(3)`. The console-attachment check is kernel-enforced at redemption, so a compromised corvus still cannot elevate a non-console process. Canonical detail: CORVUS-DESIGN.md §5.5.1.

**Console attachment** — `PROC_FLAG_CONSOLE_ATTACHED`, a Proc flag set by joey for console-login chains, never propagated across `rfork`. The kernel trust anchor for hostowner elevation.

### 15.4 Capability elevation via factotum — DESIGNED-NOT-IMPLEMENTED for v2.0

**Status**: design committed at Phase 0; implementation is post-v1.0.

**Relationship to corvus (v1.0)**: corvus handles **key management** (passphrases, DEKs, per-user authentication) per CORVUS-DESIGN.md. The factotum-style **capability elevation** described below is a separate v2.0 concern. The v1.0 hostowner model (CORVUS-DESIGN §3 D5) covers the elevated-admin case via console + system passphrase; factotum extends to fine-grained per-syscall capability grants.

**Motivation**: setuid is a security disaster — Plan 9 didn't have it; modern security thinking (capability-based, polkit-style) all agrees. But POSIX programs sometimes need elevation (e.g., `sudo`, `mount`, `ping` historically). v1.0 has no elevation; v2.0 adds factotum-mediated elevation.

**Design**:
- A privileged 9P server, `factotum` (named after Plan 9's auth agent), runs as a system service.
- factotum holds a policy: which users can request which capabilities, with what audit trail, with what time bound.
- A program needing elevation opens `/dev/factotum/elevate`, writes a request: `{capability: CAP_BIND_PRIV, ttl: 60s, justification: "binding port 80 for HTTP server"}`.
- factotum validates against policy, optionally prompts the user (via Halcyon → confirmation prompt), grants a time-bounded capability handle.
- The program receives a `KObj_Capability` handle with the granted capability + expiry.
- The kernel's syscall gates check for an explicit capability handle when a syscall requires one not in the process's base capability set.
- Audit log: every elevation request + grant + use is recorded in `/dev/factotum/audit`.

**Threat model**:
- factotum is the trust root for capability decisions. A factotum compromise is a system compromise.
- factotum is small, single-purpose, audited heavily, and has no dependencies beyond core kernel + 9P + janus (for cryptographic key management).
- factotum runs as uid 0 (the only privileged user); it never grants its own privilege.

**v1.0 scaffolding for v2.0**:
- The kernel's syscall gates already check for a "capability handle attached to syscall" path; it's a no-op at v1.0 (no handle attached → standard capability check).
- The credential structure has space for a `held_capability_handles` field; empty at v1.0.
- These don't add user-visible behavior at v1.0 but ensure v2.0 implementation is a fill-in.

**Implementation effort estimate**: ~5-8 KLOC of factotum code (Rust) + ~1-2 KLOC of kernel handle plumbing. Post-v1.0.

### 15.5 Hardening (CPU + compiler features)

Detailed in §24. **Unconditional at v1.0 on every target** (matches the `kernel/main.c` boot banner): MMU, W^X (enforced as invariant, via per-PTE PXN/UXN), KASLR, exception vectors, IRQ hardening, stack canaries, extinction. **Runtime-conditional** (Lazarus W1, `PORTABILITY.md §4`): ARM PAC + ARM BTI (HINT-space markers — active only where the CPU implements FEAT_PAuth / FEAT_BTI *and* `start.S` enabled them; a NOP on the A72 / v8.0 floor) and LSE atomics (the kernel compiles the LL/SC floor; W1.5 patches single-instruction LSE in at boot where FEAT_LSE is present). The honest delta: on bare v8.0 silicon (A72 / Pi 400) the PAC/BTI exploit mitigations are unavailable — a property of that hardware, not a regression on capable targets (M2 / QEMU). **Deferred — NOT on at v1.0** (reconciled 2026-05-28, IDENTITY-DESIGN.md §8.4): CFI (`-fsanitize=cfi`; post-v1.0) and ARM MTE (Phase 8). PAC key entropy is derived off `CNTPCT_EL0` (§8.4 BUILD). Userspace ASLR lands with the loader-randomization work.

### 15.6 Open design questions

- **Mandatory access control (MAC)**: not at v1.0. Territory isolation provides much of what MAC provides without policy complexity. Revisited post-v1.0 if specific compliance / multi-tenant scenarios require it.
- **seccomp-equivalent syscall filtering**: a process can restrict its own syscall surface. v1.0 does this implicitly via territory pruning (without `/dev/foo`, the syscall surface that takes a path doesn't reach foo). Explicit per-syscall filtering is post-v1.0.

### 15.7 Summary

Territory isolation primary; standard Unix DAC + coarse capabilities; full SOTA hardening; factotum-mediated capability elevation designed for v2.0.

---

## 16. POSIX and Linux compatibility

**STATUS**: COMMITTED

> **Phase 6 (Pouch) realizes this section.** `POUCH-DESIGN.md` is the binding design for the Tier-1 native compat layer — the **pouch** libc (a musl derivative) + the `aarch64-thylacine` cross-toolchain. §16.1's Tier-1 "musl libc port" *is* pouch. Where this section and POUCH-DESIGN.md describe the same surface, POUCH-DESIGN.md is authoritative for the as-designed Phase-6 detail; this section is the architectural overview.

### 16.1 Compat strategy (three tiers)

Per `VISION.md §12`:

| Tier | Target | Mechanism |
|---|---|---|
| 1 — native | Programs compiled against musl port | musl libc port to Thylacine syscalls |
| 2 — Linux static | Pre-built static Linux ARM64 binaries | Kernel syscall translation shim |
| 3 — container | OCI / Docker images | Process territory + Linux-shaped 9P servers |

### 16.2 musl port

musl libc's `arch/aarch64/` is adapted to emit Thylacine syscalls. Most of musl is unchanged (it's portable C). The platform layer is replaced.

Specific changes:
- `arch/aarch64/syscall_arch.h` — Thylacine syscall numbers.
- `arch/aarch64/pthread_arch.h` — TPIDR_EL0 TLS.
- `src/thread/aarch64/` — pthread_create maps to `rfork(RFPROC | RFMEM | ...)`.
- `src/signal/` — POSIX signal API maps to notes (§16.4).
- `src/process/` — fork/clone map to rfork.

The musl port is a v1.0 deliverable (Phase 5).

### 16.3 Linux ARM64 binary compat

The kernel intercepts `SVC #0` with Linux syscall numbers and routes them to Thylacine equivalents. The shim (`compat/linux-syscall.c`) is a switch table:

```c
long handle_linux_syscall(int nr, ...) {
    switch (nr) {
    case __NR_openat:     return linux_openat(args);
    case __NR_close:      return linux_close(args);
    case __NR_read:       return linux_read(args);
    case __NR_write:      return linux_write(args);
    case __NR_futex:      return linux_futex(args);
    /* ... */
    default:
        return -ENOSYS;
    }
}
```

Each `linux_*` function translates Linux semantics to Thylacine native:
- `linux_openat(dirfd, path, flags)` → resolves dirfd-relative path → `open(resolved_path, translate_flags(flags))`.
- `linux_clone(flags, ...)` → translates `CLONE_*` to `RF*` and calls `rfork`.
- `linux_futex(uaddr, op, val, ...)` → maps to Thylacine internal wait/wake on a virtual address (covered in `futex.tla`).

**Coverage**: top ~50 Linux ARM64 syscalls (per §11.5). Programs that use only this subset run unchanged. glibc-dynamic binaries are best-effort (the dynamic linker path `/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` would need to exist; we provide a best-effort shim).

### 16.4 Signal translation

POSIX signals map to Plan 9 notes:

| POSIX | Plan 9 note | Notes |
|---|---|---|
| `SIGINT` | `interrupt` | Ctrl-C on cons → note |
| `SIGTERM` | `kill` | `postnote(pid, "kill")` |
| `SIGKILL` | `kill` (non-catchable) | Kernel enforces non-catchable |
| `SIGCHLD` | synthetic | Generated on child `exits()` |
| `SIGHUP` | `hangup` | Terminal close |
| `SIGPIPE` | synthetic | Write to closed pipe |
| `SIGALRM` | `alarm` | `alarm()` syscall → timer note |
| `SIGUSR1`/2 | `usr1`/`usr2` | User-defined |
| `SIGSTOP` | `stop` | Job control; non-catchable |
| `SIGCONT` | `cont` | Job control resume |
| `SIGWINCH` | synthetic | Terminal resize |

`sigaction`, `sigprocmask`, `sigsuspend`, `sigwaitinfo` implemented in musl + thin kernel shim against note delivery (§7.6.3 syscall surface). POSIX programs receive signals normally; the note mechanism is invisible to them.

**v1.0 supported subset** (POUCH-DESIGN.md §6.4; the proving binary `/pouch-hello-signals` exercises it):

| POSIX | Plan 9 note | NOTE_BIT | Status at v1.0 |
|---|---|---|---|
| `SIGINT` | `interrupt` | 0 | supported (handler + fd-read paths) |
| `SIGTERM` | `kill` | 1 | supported (non-catchable per §7.6.5) |
| `SIGPIPE` | `pipe` | 2 | supported; pouch default-masks per [RESOLVED 6.4] (write returns EPIPE) |
| `SIGCHLD` | `child_exit` | 3 | supported (synthetic on child exits()) |
| `SIGHUP`/`SIGALRM`/`SIGUSR1`/`SIGUSR2`/`SIGSTOP`/`SIGCONT` | (deferred) | n/a | `sigaction` returns EINVAL |

**Modern fd-first path** — daemons that don't want async handlers (libsodium, stratumd) read notes from the SYS_NOTE_OPEN fd in their event loop alongside other fds via `poll`. See ARCH §7.6.1.

Hard cases handled by the supported set:
- `SIGCHLD`: pouch's `waitpid(WNOHANG)` reads pending `child_exit` notes off the fd; `WUNTRACED`/`WCONTINUED` deferred with `stop`/`cont`.
- Signal masks across `rfork()`: inherited per `RFNOTEG` (queue + handler; mask is per-Thread, not inherited).
- `SA_RESTART`: v1.0 does NOT interrupt blocked syscalls (the note stays queued; delivery happens at the syscall's return). Documented limitation; mirrors what most daemons assume anyway.

Spec: `notes.tla` was the planned formalization (per CLAUDE.md "Spec-to-code suspended" — no formal module; prose-reasoning + audit + tests).

### 16.5 Container as territory

Linux container images (OCI format) run inside a Thylacine territory:
1. Mount the container's root filesystem (extracted to a Stratum subvolume or a 9P server) as the process root.
2. Provide synthetic `/proc-linux`, `/sys-linux`, `/dev-linux` 9P servers matching the Linux layout.
3. Run the container's init as a normal Thylacine process with the constructed territory.

This is the "flatpak / Steam Deck" model from the vision: containers are territories, not a separate subsystem. The kernel primitive (territory) handles both.

`thylacine-run` (a userspace tool, Phase 6) takes an OCI image and produces the territory + init process. No cgroups, no seccomp at v1.0; territory isolation is the boundary.

### 16.6 Open design questions

- **`epoll` design** (post-v1.0). The shape is: `epoll_create` returns a Spoor; `epoll_ctl` adds/removes fds; `epoll_wait` blocks on multiple fds simultaneously. Cleanest implementation: a kernel `Dev` that wraps the existing `poll` infrastructure with a stable fd set. v1.1 candidate.

### 16.7 Summary

musl port (Tier 1 native); Linux syscall shim (Tier 2 static); container-as-territory (Tier 3). Signals → notes via musl translation. epoll/inotify/io_uring deferred.

---

## 17. Halcyon integration

**STATUS**: COMMITTED

### 17.1 Kernel surface Halcyon depends on

Halcyon requires from the kernel:
- **Framebuffer**: `/dev/fb/image` (BURROW handle), `/dev/fb/ctl` (resolution, format, flush).
- **Input**: `/dev/cons` (keyboard via UART or VirtIO input); `/dev/mouse` (if mouse connected).
- **Processes**: `rfork`, `exec`, `pipe`, `wait` for command execution.
- **9P mounts**: standard `mount` syscall to bring up video servers, image servers, etc.
- **Notes**: terminal-resize note (`winch`); `interrupt` for Ctrl-C.

Halcyon does not require a compositor, a display server, or any graphics API beyond raw framebuffer write.

### 17.2 Framebuffer device

The framebuffer device is a userspace VirtIO-GPU driver exposing:

```
/dev/fb/
    ctl       ← write: "res <width> <height> <depth>", "flush"
    image     ← BURROW handle to ARGB32 pixel buffer (zero-copy)
    info      ← read: current resolution and format
```

Halcyon receives the BURROW handle on first open of `/dev/fb/image`; maps it; writes pixels directly to the mapped region; issues `flush` via `/dev/fb/ctl`. The VirtIO-GPU driver handles the DMA transfer to the host GPU.

For bare metal Apple Silicon (post-v2.0), the framebuffer device speaks to the AGX driver (via Asahi) via the same 9P interface. Halcyon does not change.

### 17.3 Input

Keyboard input via `/dev/cons` (line- or character-mode based on `consctl` settings). Halcyon reads keystrokes; no kernel-level translation beyond raw character delivery.

Mouse via `/dev/mouse` (if VirtIO input device is present). Halcyon uses mouse for selection and scrolling; not as a primary input model.

### 17.4 Process management

Halcyon is the user's primary launcher. When the user types `cmd args`, Halcyon:
1. Parses the command line.
2. `rfork(RFPROC | RFFDG | RFNAMEG)` — new process, share fd table briefly to set up redirections, share territory (so mounted servers are visible).
3. In the child: set up file redirections via `dup`; `exec(cmd, args)`.
4. In the parent: `wait` for the child; collect exit status.

Pipelines (`cmd1 | cmd2`) handled by chaining pipes between the children.

Job control (`Ctrl-Z`, `bg`, `fg`) handled via process groups + `stop`/`cont` notes.

### 17.5 Open design questions

None at Gate 3.

### 17.6 Summary

Halcyon is a 9P client that mounts framebuffer + input + video + Stratum and renders the scroll-buffer UI. Kernel provides standard syscalls and the `/dev/fb/`, `/dev/cons`, `/dev/mouse`, `/dev/video/` device interfaces. No graphics-specific kernel API.

---

## 18. Kernel object handles and capabilities

**STATUS**: COMMITTED

*Informed by: Zircon (Fuchsia), seL4. Subordinated to 9P (the public composition mechanism).*

### 18.1 The problem with implicit privilege

The naive userspace driver model has a gap: how does the kernel know which process is allowed to own which device? The answer cannot be "it is a privileged process" — that recreates Unix's root problem. The answer must be **explicit, unforgeable, typed grants**.

**Subordination invariant**: handles are not a general-purpose IPC mechanism. The only channel through which a handle may be transferred between processes is a 9P session (as out-of-band metadata on a 9P message). No syscall exists for direct handle transfer between processes. This invariant ensures 9P remains the sole composition primitive in Thylacine. Any design that requires handle transfer outside a 9P session is a signal that a 9P interface is missing, not that this invariant should be relaxed.

### 18.2 Kernel objects and handles

Every resource the kernel manages is a **kernel object**. Kernel objects are accessed exclusively via **handles** — unforgeable integer tokens scoped to a process. A process cannot fabricate a handle; it can only receive one from the kernel or via 9P transfer.

**Kernel object types** (committed set):

| Type | Represents | Transferable? |
|---|---|---|
| `KObj_Process` | A process | Yes (within 9P) |
| `KObj_Thread` | A thread within a process | Yes |
| `KObj_Burrow` | A virtual memory object | Yes |
| `KObj_Spoor` | An open 9P channel (wraps Spoor) | Yes |
| `KObj_MMIO` | A mapped MMIO region at a specific phys range | **No (typed)** |
| `KObj_IRQ` | The right to receive a specific interrupt | **No (typed)** |
| `KObj_DMA` | A DMA-capable physically contiguous buffer | **No (typed)** |
| `KObj_Interrupt` | An eventfd-like fd that fires on IRQ delivery | **No (typed)** |
| `KObj_Srv` | A `/srv/<name>` service registration (held by the server) or per-connection channel (held by a client) to a userspace 9P server | **No (typed)** |

Handles carry **rights** — a bitmask of what the holder can do:

| Right | Meaning |
|---|---|
| `RIGHT_READ` | Read the object's state |
| `RIGHT_WRITE` | Modify the object's state |
| `RIGHT_MAP` | Map the object into an address space (BURROW, MMIO) |
| `RIGHT_TRANSFER` | Pass the handle to another process (only meaningful for transferable types) |
| `RIGHT_DMA` | Program DMA from this object |
| `RIGHT_SIGNAL` | Deliver/receive signals on this object |

Rights monotonically reduce when transferring; never elevate.

### 18.3 Typed transferability

The transfer syscall is typed — not just runtime-checked. The transfer-via-9P mechanism's switch covers only transferable types:

```c
long handle_transfer_via_9p(struct Spoor *9p_chan, int handle_idx) {
    struct Handle *h = handle_get(handle_idx);
    if (!h) return -EBADF;
    switch (h->type) {
    case KObj_Process:
    case KObj_Thread:
    case KObj_Burrow:
    case KObj_Spoor:
        if (!(h->rights & RIGHT_TRANSFER)) return -EPERM;
        return do_transfer_via_9p(9p_chan, h);
    /* No code path for KObj_MMIO, KObj_IRQ, KObj_DMA, KObj_Interrupt, KObj_Srv. */
    default:
        panic("transfer of non-transferable handle type %d", h->type);
    }
}
```

Hardware handles cannot be transferred — the transfer syscall has no code path for them. Compile-time `_Static_assert` on the type enum ensures every type is accounted for; a future addition that's transferable must be added to the `case` list explicitly.

This is stronger than "policy that says don't transfer hardware handles" — at the syscall site, the invariant is unfalsifiable. A bug that would have transferred a hardware handle instead panics, immediately revealing the violation.

Spec: `handles.tla` proves:
- Rights monotonically reduce.
- Transferable types use 9P transfer; non-transferable have no transfer codepath.
- Handle transferred over 9P arrives intact at the receiver with rights ≤ sender's rights.
- Transfer is not lossy (no double-counting, no loss).

### 18.4 Driver startup flow

When the kernel starts a driver process for a device:

1. Kernel inspects DTB, identifies the device.
2. Kernel allocates `KObj_MMIO` for the device's BAR/MMIO range.
3. Kernel allocates `KObj_IRQ` for the device's interrupt line.
4. Kernel allocates `KObj_DMA` for the device's CMA allocation (if the device does DMA).
5. Kernel exec's the driver process with these handles attached (via a dedicated startup channel — `/dev/driver-startup/<n>` — that the driver reads to receive its handles; or via auxv-shaped delivery).
6. Driver maps MMIO via `mmap_handle(KObj_MMIO_handle)`.
7. Driver maps DMA buffer similarly.
8. Driver registers for interrupt delivery by `read()`-blocking on the IRQ handle.
9. Driver implements its 9P server, exposes at `/dev/<name>/`.

After step 4, the kernel has no further involvement in the hot path. The driver owns the hardware directly.

### 18.5 Regular processes

Regular processes receive no hardware handles. They hold only:
- `KObj_Spoor` handles to open 9P connections (the normal file descriptor model).
- `KObj_Burrow` handles for shared memory passed to them explicitly (e.g., framebuffer).
- `KObj_Process` and `KObj_Thread` handles for process management.

This is the hard boundary: regular programs cannot reach hardware regardless of what they do. The capability is simply not present in their handle table.

### 18.6 Handle transfer over 9P

`KObj_Burrow` (with `RIGHT_TRANSFER`) and `KObj_Spoor` are passed between processes over a 9P connection as out-of-band metadata on a message. This is the mechanism for zero-copy buffer passing: a driver creates a BURROW, fills it, and passes the BURROW handle to Halcyon (or any consumer) via the 9P session. The consumer maps the BURROW and reads directly. No copy, kernel not in the data path.

The 9P out-of-band attachment uses an extension to 9P2000.L (negotiated at session establishment):

```
Twrite tag fid offset count data <handle: burrow_handle>
```

The receiver (or kernel-mediated 9P receive path) extracts the handle from the message metadata, places it in the receiving process's handle table with the requested rights, and continues normal 9P processing.

### 18.7 Open design questions

None at Gate 3.

### 18.8 Summary

Nine kernel object types, four transferable, five non-transferable. Subordination invariant: transfer only via 9P; hardware handles and `/srv` connection handles never. Typed enforcement at the syscall site.

---

## 19. Virtual memory objects (VMOs)

**STATUS**: COMMITTED

*Informed by: Zircon (Fuchsia).*

### 19.1 What a BURROW is

A **Virtual Memory Object** is a kernel object representing a region of memory, independent of any process's address space. It is the unit of memory sharing in Thylacine.

A BURROW has:
- A size (page-aligned).
- A backing type: anonymous (zero-filled on demand), physical (pinned for DMA), or file-backed (Stratum page cache, post-v1.0).
- A reference count. Pages are freed when the last handle is closed and all mappings are unmapped.

```c
struct Burrow {
    size_t size;
    enum {VMO_ANON, VMO_PHYS, VMO_FILE} type;
    union {
        struct page **pages;          /* anon: lazy-allocated */
        struct {
            paddr_t base;
            int locked;
        } phys;
        struct {                      /* post-v1.0 */
            struct Spoor *spoor;
            uint64_t offset;
        } file;
    };
    atomic_t handle_refs;             /* count of open handles */
    atomic_t mapping_refs;             /* count of mappings */
    /* ... */
};
```

### 19.2 Why VMOs over anonymous mmap

Unix `mmap(MAP_ANONYMOUS | MAP_SHARED)` shared memory has implicit aliasing — two processes share memory by convention (both knowing the fd number or the shm name), not by explicit kernel-tracked grant. VMOs make sharing explicit:

- One process creates the BURROW and holds the handle.
- It maps the BURROW into its address space.
- It passes the BURROW handle (with `RIGHT_MAP | RIGHT_READ`) to another process via 9P.
- The second process maps the BURROW independently.

The kernel tracks both mappings. When the producing process unmaps, the pages remain live until all handles are closed and all mappings are gone. No use-after-free at the kernel level.

### 19.3 Primary use cases

**Zero-copy framebuffer**: VirtIO-GPU driver creates a physical BURROW for the framebuffer. Halcyon receives the BURROW handle and maps it. Halcyon writes pixels directly into the mapped region. Driver issues `flush` when Halcyon signals readiness. No copy at any point.

**Zero-copy video decode**: video decoder creates a physical BURROW per decoded frame. Passes BURROW handle to the video player 9P server. Player passes it to Halcyon. Halcyon blits from the mapped BURROW into the framebuffer BURROW. One blit; no intermediate copies; no kernel involvement after handle transfer.

**DMA buffer lifecycle**: NVMe / VirtIO-blk driver creates a physical BURROW for its DMA descriptor rings and data buffers. The BURROW handle is held exclusively by the driver. The kernel's IOMMU mapping (post-v1.0) is derived from the BURROW's physical page list. Clean, auditable DMA ownership.

**Inter-driver shared memory**: two cooperating driver processes (e.g., network driver and network stack) share a ring buffer via a BURROW. No kernel copy in the packet path.

### 19.4 BURROW syscalls

| Syscall | Description |
|---|---|
| `burrow_create(size, flags)` | Create anonymous BURROW, return handle |
| `burrow_create_physical(paddr, size)` | Create BURROW over physical range (privileged) |
| `burrow_get_size(h)` | Query BURROW size |
| `burrow_read(h, buf, off, len)` | Read from BURROW without mapping |
| `burrow_write(h, buf, off, len)` | Write to BURROW without mapping |

Mapping is done via `mmap_handle()` from §18. Unmapping via standard `munmap`.

### 19.5 Spec

`specs/burrow.tla` proves:
- Pages live until last handle closed AND last mapping unmapped (no use-after-free).
- Mapping/unmapping is atomic with respect to handle close.
- Reference counts are accurate under concurrent open/close/map/unmap.

### 19.6 Open design questions

- File-backed VMOs (Stratum page cache integration). Post-v1.0; v1.0 has anonymous + physical only.

### 19.7 Summary

Kernel objects representing memory regions; first-class transferable handles; explicit lifecycle (handle refs + mapping refs); zero-copy bulk-data passing via 9P. Spec'd.

---

## 20. Per-core discipline (SMP)

**STATUS**: COMMITTED

*Informed by: Barrelfish (ETH Zurich / Microsoft Research). Multikernel direction designed for v2.x.*

### 20.1 The shared-state SMP trap

The conventional multicore kernel approach — shared kernel data structures protected by locks — has a fundamental cost that grows with core count: cache coherency traffic. Every lock acquisition on a shared structure sends invalidation messages across the interconnect. At 8+ cores this becomes the bottleneck, not the computation.

Thylacine v1.0 does not go full multikernel (per-core kernel instances with no shared state — that is the v2.x research direction). But the **discipline** of treating cross-core communication as explicit shapes the design correctly from the start.

### 20.2 Committed per-core discipline

**Per-CPU data is the default**: scheduler run trees, interrupt handler state, kernel allocator slabs, IRQ counters, RCU state, are per-CPU. No lock needed to access them from the owning CPU. Cross-CPU access is the exception, not the rule.

**Cross-core communication is explicit**: when one CPU must affect another's state (e.g., TLB shootdown, process migration, IPI-driven wakeup), the operation is an explicit IPI with a defined payload — not a lock on a shared structure. The recipient CPU processes the message in its own interrupt handler.

**No global kernel lock**: there is no equivalent of the Linux BKL or a single global spinlock protecting kernel state. Every shared structure that genuinely must be shared has its own fine-grained lock with documented contention analysis.

**Interrupt affinity**: device interrupts are affine to a specific CPU (configured in the GIC). The driver process that owns the device receives IRQ delivery on the CPU its IRQ handle is bound to. Cross-CPU IRQ dispatch is eliminated in the common case.

### 20.3 Data structures with shared state

Some structures genuinely require cross-CPU visibility:

| Structure | Sharing mechanism |
|---|---|
| Process table | RCU-style: readers lockless, writers take a narrow lock |
| BURROW reference counts | Atomic operations (no lock) |
| Handle table (per-process) | Per-process lock, not global |
| IRQ routing table | Written at boot/driver-start only, read locklessly after |
| 9P session table | Per-session lock |
| Territory mount table | Per-process rwlock |

### 20.4 IPI types and payloads

Standard IPI types (§8.7):

| IPI | Payload | Purpose |
|---|---|---|
| `IPI_RESCHED` | None | Peer should rerun the scheduler (a thread became runnable on it) |
| `IPI_TLB_FLUSH` | `{vaddr_start, vaddr_end, asid}` | Peer should flush a TLB range |
| `IPI_HALT` | None | Peer should halt (shutdown) |
| `IPI_GENERIC` | `(callback_fn, arg)` | Peer should invoke `callback_fn(arg)` |

Sender writes the payload to a per-recipient ring buffer; sends an SGI; recipient processes.

### 20.5 TLA+ specification requirement

The per-core scheduler and IPI protocol are audit-trigger surfaces. Before SMP is enabled (Phase 2 exit criterion: 4-CPU SMP with stress test; Phase 7 hardening: 8-CPU 72-hour stress test), `specs/scheduler.tla` proves the cross-CPU state machine.

### 20.6 Multikernel direction — DESIGNED-NOT-IMPLEMENTED for v2.x

**Status**: design committed at Phase 0; implementation is post-v1.0.

**Motivation**: the per-CPU discipline at v1.0 already treats cross-core operations as explicit messages. The v2.x extension: each CPU runs its own kernel instance; cross-core IPC is 9P (the natural extension of Thylacine's model — cross-process IPC is 9P, so cross-core IPC is the same). The result is true cache-coherence-free SMP at scale (Barrelfish's claim, 16+ cores, with linear scaling).

**Design sketch**:
- Each CPU has its own kernel instance with its own address space and its own subset of kernel objects.
- Process migration across CPUs becomes explicit: the source kernel hands off the process state to the destination kernel via a 9P message.
- Shared memory is not actually shared — it's a ring buffer in cache-line-sized chunks, with explicit transfers.
- Synchronization is 9P-mediated: locks become "ask the responsible kernel for permission."
- Coherent global views (e.g., `ps` listing all processes) are queries to all kernels via parallel 9P calls, then aggregated.

**v1.0 scaffolding**:
- Per-CPU data is *not* shared by default. The discipline is in place from v1.0.
- Cross-CPU operations use IPIs, not shared memory polling. The IPI infrastructure is what becomes 9P-based cross-kernel messaging in v2.x.
- The scheduler is per-CPU; work-stealing is the "ask peer for work" pattern that becomes the v2.x model.

**Implementation effort estimate**: substantial. ~30-50 KLOC of new kernel code at v2.x. NUMA-aware. Memory model carefully designed.

**Open at v2.x entry**:
- Single global address space vs per-kernel address spaces. The latter is more Barrelfish-like but harder for shared-memory programs.
- Whether the user-visible model changes at all (probably yes — explicit cross-NUMA-node placement becomes a thing).

### 20.7 Open design questions at v1.0

- **Core count at v1.0**: 1-8 vCPUs in QEMU. Phase 8 exit: 8-core stress.
- **NUMA**: single-socket only at v1.0.
- **CPU hotplug**: not at v1.0. CPUs online at boot.

### 20.8 Summary

Per-CPU data discipline; explicit IPI cross-CPU communication; no global kernel lock; SMP up to 8 cores at v1.0; multikernel designed for v2.x.

---

## 21. Async 9P — pipelined requests

**STATUS**: COMMITTED

*Informed by: io_uring (Linux), Helios (CMU), 9P2000.L native pipelining.*

### 21.1 The synchronous 9P bottleneck

The basic 9P model is synchronous: send a Tmessage, wait for the Rmessage, send the next. Under high I/O load this serializes unnecessarily — the client is idle while the server processes each request, and network/IPC round-trip latency dominates throughput.

**9P2000 already solves this at the protocol level**: the `tag` field in every message is a request identifier. The client can have multiple outstanding requests with different tags simultaneously. The server responds to each independently and out of order. This is **pipelining** and it is part of the core protocol spec, not an extension.

Stratum's server already supports this (io_uring-native write path). Thylacine's client must exploit it.

### 21.2 Committed client design

The kernel's 9P client maintains a **request pipeline** per session:

- Maximum outstanding requests: 32 default, configurable per session via `/ctl/9p/<session>/max-outstanding`.
- Each request is assigned a unique tag from a per-session tag pool (16-bit tag space, tag 0xFFFF reserved for Tversion).
- Requests are submitted to the session's send queue without waiting for prior requests to complete.
- Completions are matched to waiting kernel threads by tag.
- Out-of-order completions are handled correctly — tag N's completion wakes tag N's waiter regardless of submission order.

This means a process doing 32 concurrent file reads into a Stratum mount gets 32 requests in flight simultaneously. The throughput approaches the session's bandwidth limit rather than being divided by round-trip latency.

### 21.3 Implementation

`kernel/9p_client.c`:

```c
struct Session {
    struct Tag tags[32];                 /* per-session tag pool */
    bitmap_t tag_in_use;                 /* free / in-use */
    struct Request outstanding[32];       /* indexed by tag */
    spinlock_t lock;                      /* protects above */
    struct queue send_q;                  /* pending sends */
    struct kernel_thread *receive_loop;   /* dispatches Rmessages */
    /* ... */
};

long send_request(struct Session *s, struct Message *msg, struct Reply **reply) {
    spin_lock(&s->lock);
    int tag = bitmap_alloc(&s->tag_in_use);
    if (tag < 0) { spin_unlock(&s->lock); return -EBUSY; }  // or block
    msg->tag = tag;
    s->outstanding[tag] = (struct Request){ .waiter = current(), ... };
    queue_push(&s->send_q, msg);
    spin_unlock(&s->lock);

    /* Block waiting for tag's completion */
    sleep_on_tag(&s->outstanding[tag]);

    *reply = s->outstanding[tag].reply;
    spin_lock(&s->lock);
    bitmap_free(&s->tag_in_use, tag);
    spin_unlock(&s->lock);
    return 0;
}

void receive_loop(struct Session *s) {
    while (s->alive) {
        struct Reply *r = receive_one(s);
        spin_lock(&s->lock);
        struct Request *req = &s->outstanding[r->tag];
        req->reply = r;
        wakeup_tag(req);
        spin_unlock(&s->lock);
    }
}
```

(Real code is more careful with memory ordering and error paths; see implementation.) The as-built v1.0 client uses an **elected-reader** model rather than the dedicated `receive_loop` kthread sketched here — see §21.10 for the as-built design and the #841 restoration from the P5 serial regression.

### 21.4 Userspace 9P servers benefit automatically

Because the kernel's 9P client pipelines, every userspace 9P server — drivers, janus, the video player, Stratum — gets pipelined access from any client that issues concurrent requests. No server-side changes needed. The server already handles multiple tags (Stratum and any decent 9P server library do).

### 21.5 Flow control

The pipeline is bounded to prevent unbounded queue growth:
- If the outstanding request count reaches the session maximum, new requests block until a slot frees.
- Per-session credit-based flow control (from 9P2000.L): the server can advertise how many requests it can handle; the client respects this.

### 21.6 Halcyon and async I/O

Halcyon issues concurrent reads against multiple 9P servers simultaneously — keyboard input, polling the video frame fd, reading command output from a pipe — via the pipelining client. This eliminates the need for a dedicated async I/O syscall in the common case. Multiple blocking reads in flight across different fds, each on a different 9P tag, compose naturally.

### 21.7 Spec

`specs/9p_client.tla` proves:
- Per-session tag uniqueness.
- Out-of-order completion correctness — tag N's reply wakes tag N's waiter regardless of arrival order.
- No missed wakeups across the wait/wake race when a reply arrives before the requester sleeps.
- Flow control — bounded outstanding requests.
- Fid lifecycle — clunk eventually frees the fid; no fid reuse before clunk completes.

### 21.8 Open design questions

None at Gate 3.

### 21.9 Summary

Pipelined 9P client from day one. Per-session 32 outstanding requests. Out-of-order completion. Halcyon and POSIX programs benefit automatically.

### 21.10 v1.0 as-built: the serial regression and its restoration (#841)

**What shipped at P5-client (R15-c F230) was NOT the §21.3 pipeline.** To get an SMP-safe client quickly, the P5 client serialized every operation under a single per-client spinlock (`c->lock`) held across the *entire* build → transport-exchange → dispatch window — i.e. across the blocking receive. `struct p9_transport`'s `exchange` is strictly synchronous (send one frame, then read exactly the next frame off the wire and dispatch it); there is no tag demux at the transport layer, and at most one request is ever in flight. The simplification was documented in the layer headers ("thread-safe at the public surface … per-client `spin_lock_t` held across every send/dispatch sequence"). It is a **deviation from this committed section** — the pipeline, the tag-matched completion, and the out-of-order dispatch that §21.2–§21.7 commit were never built.

That deviation is the **#841 soundness bug**. Two independent defects compound:

1. **Spinlock held across a blocking sleep.** Every `p9_client_*` op holds `c->lock` (a busy-wait spinlock) across `p9_transport_exchange`, whose receive blocks (`srvconn_client_recv` → `tsleep`). A second CPU calling any client op **busy-spins** on `c->lock` while the holder sleeps — burning a core the server needs to make progress. When a single kernel 9P client is shared across Procs (corvus and joey share the Stratum-root client, because `territory_clone` shares `root_spoor`), one Proc's blocked op stalls the whole client *and* spins every other CPU that touches it.

2. **Per-op timeout desyncs the shared byte stream.** The receive is bounded by `SRVCONN_OP_DEADLINE_NS` (30 s). On timeout the op abandons its in-flight request: the late reply is never consumed, so the next op reads a frame-misaligned stream; and `do_recv` latches the transport to `P9_TRANS_ERROR`, wedging the session permanently. A single slow reply is unrecoverable.

Observed in stalk-3c-d: corvus, spawned post-pivot with a Stratum-FS root, issues an A-3 X-search `Tgetattr(Stratum root)` for its `/srv` open over the client it shares with joey; under SMP host load (UBSan build) the busy-spin starves `stratumd`, the 30 s deadline trips, the stream desyncs, corvus's open fails persistently, corvus exits, and kproc's `wait_pid` reaps the orphaned corvus → wrong-pid extinction. ~30% under bursty host load; invisible at low load (which is why the serial client passed for so long).

**The restoration (#841)** builds the committed §21 pipeline, with one deliberate departure from the §21.3 *sketch* and one simplification of the timeout policy:

- **Elected-reader, not a reader kthread.** §21.3's `receive_loop` kthread is illustrative ("Real code is more careful … see implementation"). A per-session kthread that blocks indefinitely in `recv` is precisely the free-while-blocked lifecycle hazard that produced #788 and #713 (a thread asleep in the kernel while its owning structure is torn down), and it cannot drive the *synchronous* loopback test transport. Thylacine instead uses the **Plan 9 `devmnt`/`mountio` elected-reader** model: a submitter that has sent its request and finds no reply yet becomes *the* reader (one at a time, guarded), receives the next frame **with no lock held**, demuxes it by tag to its owning request, wakes that owner, and loops until its own reply arrives; if another thread is already the reader, the submitter sleeps on its own request. This (a) never holds a lock across `recv`, (b) has no long-lived kthread to spawn, join, or tear down, (c) preserves the SrvConn's "single blocking consumer of `s2c`" invariant via the election, and (d) drives both the blocking srvconn transport and the synchronous loopback unchanged. It delivers every committed *property* of §21.2–§21.7 (max-outstanding, tag-matched completion, out-of-order dispatch, no-missed-wakeup, flow control); only the sketch's reader-model detail differs. It is exactly the heritage the #841 fix direction named ("Plan 9 mntrpc").

- **No per-op timeout.** §21 commits no per-request deadline. The elected reader blocks until a frame arrives or the transport reaches EOF (server death → all in-flight requests woken with `-EIO`, session marked dead); a blocked op is **death-interruptible** via #811 (universal `*_INTR` sleep), so a group-terminate unblocks it. The 30 s `SRVCONN_OP_DEADLINE_NS` is removed from the kernel-client path. This removes the desync class structurally — there is no "abandon one in-flight request and keep using the stream" path. A wedged-but-alive server now blocks the caller (as Plan 9 and local Linux-9p do) rather than corrupting the session; the caller is still killable.

**Per-request shape.** Each op allocates a `p9_rpc` (the §21.3 "Request"): tag, op-kind, a wait rendez, a done flag + error slot, a result struct, and a per-request reply buffer (≤ msize). Submit: under `c->lock`, allocate a tag (block at `P9_SESSION_MAX_OUTSTANDING` = flow control), build the Tmsg, register the rpc in a tag-indexed `inflight[]` table, send the framed Tmsg (frame-atomic; a sender blocks death-interruptibly if `c2s` lacks room for a whole frame, **without** holding `c->lock` across the block), release `c->lock`, then register-then-observe-sleep on the rpc's rendez (the I-9 no-lost-wakeup discipline `specs/9p_client.tla` pins). The elected reader copies each received frame into the owning rpc's reply buffer, marks it done, and wakes it; the woken submitter dispatches its own buffer through `p9_session_dispatch_rmsg` (which clears `session.outstanding[tag]` and applies fid state, under `c->lock`), extracts its result, frees the tag + rpc, and returns. The per-rpc reply buffer + the one frame copy are the v1.0 cost; a buffer pool / read-into-owner-buffer is a v1.x optimization.

**Wait/wake mechanism (single-waiter Rendez → per-rpc rendez + reader election).** `struct Rendez` is **single-waiter** — it extincts on a second concurrent sleeper — so the elected reader cannot use one shared wait-rendez for all blocked submitters. Each `p9_rpc` therefore carries **its own** Rendez (single waiter = the submitting thread) and three rpc-local flags — `done`, `dead`, `be_reader` — each mutated only under `c->lock` and each mutation immediately followed by `wakeup(&rpc->rendez)` (the I-9 register-then-observe discipline `specs/9p_client.tla` pins; the waiter's `sleep` cond reads only these rpc-local flags, never cross-lock client state). The reader role is a single `c->reader_active` flag, taken and released under `c->lock`. The op loop (holding `c->lock`): while my rpc is neither `done` nor `dead` — if no reader is active, take the role, drop `c->lock`, `recv` one frame (`srvconn_client_recv` with `deadline_ns = 0` blocks until data / EOF / death), retake `c->lock`, demux by tag to the owning rpc (copy frame → its reply buffer, set its `done`, `wakeup` it if it is not me), and repeat until my own rpc is `done`; **on departing the reader role, hand it off** by selecting one still-pending (`active && !done && !dead`) rpc, setting its `be_reader`, and waking it (so a surviving submitter becomes the next reader); if a reader is already active, sleep on my own rpc's rendez. Transport EOF/error marks the session dead and wakes **every** in-flight rpc with `-EIO`. **Death-interrupt unwind**: a `recv` that returns the #811 INTR signal (the reader's Proc is group-terminating) or a `sleep` returning `SLEEP_INTR` unwinds — the dying thread removes its rpc from `inflight[]`, hands off the reader role if it held it, releases `c->lock`, and returns (it dies at its EL0-return die-check). Because the client is shared across Procs (corvus + joey on the Stratum root), the **hand-off-on-death is load-bearing**: one Proc dying must not strand the survivors' in-flight ops. The send side is frame-atomic — `chan_produce` into `c2s` under the ring lock; a sender blocks death-interruptibly if `c2s` lacks room for a whole frame, **without** holding `c->lock` across the block (the common case — corvus + joey one getattr each — never fills the ring; deep bursts are the only back-pressure path, and `SRVCONN_RING_CAP` may be raised to widen it).

**Invariants.** I-10 (per-session tag uniqueness — the allocator refuses an active tag), I-11 (fid lifecycle — unchanged; `dispatch_rmsg` is still the codec authority), I-9 (no wakeup lost between the rpc-done check and the sleep — register-then-observe under `c->lock`, generalized to the per-rpc rendez), and flow control (bounded outstanding) all hold. The public `p9_client_*` API surface is **unchanged** — `dev9p` and every consumer are untouched; the change is internal to `9p_client.c` + the `9p_transport.c`/srvconn boundary.

**Audit posture.** Audit-bearing (the 9P-client trigger surface; §25.4). Validated against `specs/9p_client.tla` (clean cfg + the four buggy cfgs — `tag_collision`, `ooo_match`, `fid_after_clunk`, `unbounded` — re-run as pre-commit gates per the spec-to-code-suspension carve-out), a focused 9P-client soundness audit, the kernel test suite (extended for multi-in-flight + out-of-order + reader-election + EOF-wakes-all), and the #841 runtime repro (`build/3cd-flake/forkstorm.sh` + `capbare.sh` under the UBSan build) GREEN across N boots under host load.

**Focused-audit close (#841): findings + reconciliations.** The audit returned 1 P1 + 1 P2 + 3 P3; the P1 is fixed, the rest disposed below. These also reconcile the as-built impl with the prose above where they diverged.

- **F1 [P1], fixed — reply-buffer lifetime.** `p9_session_dispatch_rmsg`'s read / readdir / readlink results **zero-copy alias** into the dispatched frame buffer (`out->read_data` etc. point inside it), and the public op copies those out *after* `client_run` returns. So `client_run` must NOT free the per-op reply buffer on the DONE path — it stashes it in `c->done_reply_buf` (freeing the prior occupant), and the buffer is freed at the **next** completion or at `p9_client_destroy`, both under `c->lock` (by which point the prior caller has copied out and dropped the lock). Holds at most one buffer; SMP-safe. (The old serial client aliased the long-lived transport `recv_buf`, never a per-op buffer, so this hazard did not exist before the pipeline.)

- **F3 [P3], reconciliation — the send side is ALL-OR-NOTHING, not block-on-room.** The "sender blocks death-interruptibly if `c2s` lacks room" phrasing above is the design sketch; the **as-built** `srvconn_client_send_frame` writes the whole frame or returns 0 (ring full) → the op fails → `client_run` marks the session dead. `SRVCONN_RING_CAP` (8 KiB) holds two `msize` frames, so the v1.0 corvus + joey single-in-flight workload never fills `c2s`; a *deep* burst that genuinely fills it would mark a healthy session dead. Bounded and dormant at v1.0; widened in v1.x by raising `SRVCONN_RING_CAP` or implementing the block-on-room sender. The all-or-nothing rule is load-bearing regardless: a partial frame on the wire desyncs the shared stream, which is worse than a clean session death.

- **F5 [P3], reconciliation — death triggers include demux-level malformation.** The session is marked dead not only on transport EOF/error but also on a demux-level protocol violation (malformed header, out-of-range tag, oversize frame) — a malformed frame is an unrecoverable desync, so fail-closed is correct.

- **F2 [P2], deferred (documented) — DIED-leaked outstanding slot.** A Proc that dies mid-op leaves `session.outstanding[tag]` active (so the tag is not reused before the now-ownerless late reply drains it via `demux_frame_locked`'s discard branch — the I-10 protection against a reused tag matching a stale reply). If that reply *never* arrives (a pathological never-replying-but-not-EOF'd server), the slot leaks permanently; bounded at `P9_SESSION_MAX_OUTSTANDING` = 64. A well-behaved server (stratumd always answers) reclaims the slot when the late reply lands, so the practical leak is near-zero. The clean fix is the per-tag **monotonic generation** I-10 already names (a stale-generation reply is detected and discarded without reserving the slot) — a deliberate v1.x change, not bolted onto this fix.

- **F4 [P3], accepted — byte-granular reader recv.** `reader_recv_frame` tolerates 1-byte short reads (it loops `ops.recv` to a full frame); bounded by `msize` (≤ 4096 iterations/frame), cannot spin or hang, and mirrors the existing `do_recv` pattern. No change.

**Sound (do not re-prosecute):** the devsrv server-close-EOF fix (server endpoint always tears down; only the kernel-attached CLIENT endpoint skips — the #841 boot-hang root cause), I-9 no-lost-wakeup (per-rpc rendez + register-then-observe), reader election (single `reader_active` under `c->lock`), stack-rpc lifetime under the lock (the DIED unwind clears `inflight[tag]` under `c->lock` before the dying thread leaves the client), tag uniqueness, and the handshake's `HANDSHAKE_DEADLINE`-then-0 deadline split.

**Round-2 close (dirty-close follow-up, the restructure changed a wait/wake protocol).** A second prosecutor round, scoped to the death/hand-off SMP path that round-1 reasoned-about but could not reproduce (no KASAN; single-threaded harness), found 1 P1 + 1 P2 + 1 P3 — all in the reader-election hand-off, all now fixed. They are latent at v1.0 (the boot workload is single-in-flight) but load-bearing for the multi-in-flight A-5b future the elected reader exists to support.

- **F6 [P1], fixed — reader-role loss strands a survivor.** A departing reader hands the role to a pending rpc (sets `be_reader` + wakes it). If that target's Proc dies *before* it assumes the role, it returns `CLIENT_WAIT_DIED` at the top of `client_wait` — before the `!reader_active` election — and `client_run`'s DIED handler did not re-hand-off. With `reader_active` left false and no active reader, a *surviving* Proc's pending sleeper would never be elected a reader nor woken — a permanent hang (the exact survivor-stranding the "hand-off is load-bearing" clause forbids, realized via the hand-off *target* dying rather than the reader). Fix: on the DIED return, if `rpc->be_reader` (the dying thread was the designated-next reader, never assumed/cleared), clear it and `client_handoff_reader_locked` before unwinding. The `be_reader` gate is the discriminator: an active-reader-dies (already handed off at role-exit; `be_reader` cleared on assuming) and a plain-sleeper-dies (an active reader still exists) both have `be_reader == false` → no spurious double-hand-off.
- **F7 [P2], fixed — busy-spin when a hand-off target loses the election race.** If a handed-off rpc (`be_reader` set + woken) re-checks and finds `reader_active` already taken by a thread that won the race, it slept in the else branch with `be_reader` still set — and `rpc_wait_cond` reads `be_reader`, so `sleep` returned immediately, busy-spinning (re-lock, re-check, re-sleep) until that reader departed, burning a CPU + contending `c->lock` (the very pathology the elected reader removes). Self-resolving + bounded, but reintroduced in miniature. Fix: clear `be_reader` at the top of the else branch before sleeping; the active reader's departure hand-off re-wakes it (no lost wakeup — a re-set + wake after the clear is observed by `sleep`'s register-then-observe).
- **F8 [P3], fixed — destroy-time free now matches the documented invariant.** `p9_client_destroy` freed `done_reply_buf` without `c->lock` (safe by the attached-refcount no-in-flight guarantee, but the header + F1 close claim "both under `c->lock`"). Now taken under `c->lock` (uncontended on the teardown path; defense-in-depth against a future destroy-of-a-shared-client).

Round-2 confirmed SOUND (do not re-prosecute): the F1 `done_reply_buf` deferred-free (SMP-safe; no double-free/leak — the DEAD/DIED paths never stash; init'd before the first DONE), the elected reader's cross-lock barrier chain (the reader's `done`/buffer stores release via `wakeup`'s `rendez.lock`, the sleeper acquires the same lock — full visibility cross-CPU), the devsrv server-close refcount/teardown interplay (idempotent teardown; the adapter's own srvconn_ref keeps the SrvConn alive until `p9_attached_destroy`), and the lock order (`c->lock` is dropped before every `sleep`/`recv`; acyclic with `rendez.lock`/`wait_lock`/the ring locks).

**Coverage gap (owed follow-up):** there is no deterministic multi-in-flight / reader-election / cross-Proc-death unit test — F1/F6/F7 live in the SMP window the synchronous harness cannot produce (the same reason rounds 1–2 reasoned-not-reproduced them). A loopback-fake-server harness driving ≥3 concurrent in-flight ops with scripted hand-off-target death (F6) and election-race-loss (F7) is owed; it is naturally exercised — and should land — with the A-5b multi-user workload that first makes the client genuinely multi-in-flight. Until then these fixes are validated by prose + the prosecutor rounds + the boot/UBSan/smp8/capbare matrix.

**Round-3 close (dirty-close recursion — a P1 returned in round-2).** A third prosecutor round, scoped to the round-2 fixes themselves (F6/F7/F8) + the hand-off state-machine completeness, returned **CLEAN (0 P0 / 0 P1 / 0 P2 / 0 P3)**. It hand-traced every interleaving against the real lock discipline and confirmed the load-bearing invariants: `be_reader` is a pure *advisory wake-hint* (the actual election is gated solely by `c->reader_active` under `c->lock`, so two threads can never both hold the reader role no matter how many carry `be_reader`); the F7 clear has no lost wakeup (the sole `be_reader` setter — `client_handoff_reader_locked` — always pairs the set with `wakeup` on the same rendez, and `wakeup` takes/releases that rendez lock even on the no-waiter path, so the sleeper's `sleep`-time cond re-check observes it); the F6 re-hand-off chain is bounded by `P9_SESSION_MAX_OUTSTANDING` and terminates (each dying thread clears `inflight[tag]` under the same held `c->lock` before any other thread runs its scan); F8 is sound (no read-magic-then-lock-expecting-validity path). **The close converged: 3 rounds (R1 1P1+4 disposed, R2 1P1+1P2+1P3 fixed, R3 clean). A clean close reached via N>1 rounds is still clean.**

**Tflush-on-abandon (#845 — closing the #841-audit F2 DIED-leaked slot).** The elected-reader's death-unwind originally left `session.outstanding[tag]` active when a Proc died mid-RPC, dropping `inflight[tag]` and relying on the server's eventual *late reply* to drain the tag (the ownerless-reply path in `demux_frame_locked`). For a conformant server this reclaims the tag in bounded time; but a server that stays alive, accepts the request, and *never* replies and never EOFs leaks the tag permanently — after `P9_SESSION_MAX_OUTSTANDING` (64) such abandons the pool exhausts and every new op fails `-EIO` (fail-safe, but the client is wedged). #841-audit F2 named the fix and deferred it; #845 implements it as **9P `Tflush`** — the protocol's designed-in request-cancellation, exactly as Plan 9's `devmnt` and Linux's v9fs use it.

As-built: on the `CLIENT_WAIT_DIED` arm, after NULLing `inflight[tag]` and freeing its reply buffer, the dying thread (still holding `c->lock`) calls `p9_session_send_flush(oldtag=T)` + `p9_transport_send` (a non-blocking ring write, reusing `out_buf` whose prior frame was already pushed). `send_flush` allocates a fresh tag `F` (`kind = P9_TFLUSH`, remembering `flush_oldtag = T`) and **reserves** the abandoned tag by setting `outstanding[T].awaiting_flush = true` — T stays *active* (so `alloc_tag` skips it) and is freed **only** by the flush's `Rflush`, **never** by a late original reply. That reservation is the load-bearing **I-10 reuse-race guard**: 9P forbids reusing `oldtag` until the `Rflush` arrives, so a stray/duplicate late reply for T can never be mis-attributed to a tag the client has reused — the exact reuse-race a naive "free T on its late reply" fix would open. `dispatch_rmsg` therefore (a) *consumes-without-clearing* any reply that arrives on an `awaiting_flush` tag (the late original — discarded, T stays reserved), and (b) on the `Rflush` (routed through tag F, ownerless via the existing `demux_frame_locked` ownerless branch) frees **both** T (in the `P9_TFLUSH` branch) and F (in the common tail). The flush is never registered in `inflight[]` (no waiter — the dying thread does not block on it); a *survivor's* elected reader drains the `Rflush` as a side effect of reading for its own op. If the flush cannot be built or sent (tag pool full, session not OPEN, or a broken transport), the client falls back to the pre-#845 reclaim path (no regression), and a failed transport write latches the session dead.

`stratumd` already answers `Tflush` (both the lp9 server it runs and the p9 server reply `Rflush`), so #845 is Thylacine-only — no Stratum change. The one residual (Opus audit F1 [P2], closed-with-justification) is a *non-conformant* server that sends a **duplicate** `Rflush` after the flush tag F was freed and reused for a new flush: indistinguishable on the wire (9P carries no per-tag generation), it would free the new flush's reserved `oldtag`. This is the generic "server sends exactly one reply per tag" assumption the entire client already rests on (a duplicate same-type reply mis-attributes for *any* op kind); it does not arise with the v1.0 trusted servers, and closing it for an untrusted/remote 9P server needs wire-level tag generations — a v1.x ABI lift, the same seam as the `n_uname` trust-stamp. No new spec per the 2026-05-23 spec-to-code broadening (Tflush is not modelled; `specs/9p_client.tla` clean + the 4 buggy cfgs remain the pre-commit gate, re-run GREEN). The deterministic multi-in-flight harness exercising the *live* `DIED → Tflush → survivor-reader` path is OWED with the A-5b multi-user workload (the same gap as the #841 elected-reader). Closed list: `memory/audit_845_closed_list.md`.

---

## 22. Hardware platform model

**STATUS**: COMMITTED

### 22.1 Primary development target

QEMU `virt`, ARM64. All Phase 1-8 work targets this machine. No QEMU-specific assumptions in the kernel above `arch/arm64/`.

### 22.2 DTB as the hardware abstraction

The kernel never hardcodes peripheral addresses, IRQ numbers, or memory regions. All hardware discovery is via the Device Tree Blob.

This is a hard rule: no `#ifdef QEMU`, no hardcoded MMIO base addresses, no compile-time peripheral configuration. The DTB parser is the single source of hardware truth.

Compatible strings drive driver selection. Same kernel binary boots QEMU `virt`, Pi 5, RK3588 (when supported) by reading their DTBs.

### 22.3 Platform-specific layers

```
arch/arm64/
  common/       ← shared: exception vectors, MMU, GIC, generic timer, PAC/MTE/BTI
  qemu-virt/    ← QEMU-specific: nearly empty if DTB-driven correctly
  rpi5/         ← Pi 5 (post-v1.0): EL2→EL1 drop, RP1 mailbox setup
  rk3588/       ← RK3588 (post-v1.0)
  apple/        ← bare metal Apple Silicon (v2.0+)
```

`arch/arm64/common/` is the investment. It runs unmodified on all platforms.

### 22.4 First bare metal target: Raspberry Pi 5 (post-v1.0)

Per VISION §4.4. Delta from QEMU `virt`: EL2→EL1 drop, mailbox framebuffer, RP1 Ethernet. Estimated: one focused sprint after v1.0 release.

### 22.5 Apple Silicon bare metal (v2.0+)

Per VISION §4.5. Depends on Asahi Linux's m1n1, AIC, AGX. Estimated: substantial effort; v2.0 candidate.

### 22.6 Open design questions

None at Gate 3.

### 22.7 Summary

DTB-driven hardware discovery; platform layers under `arch/arm64/<platform>/`; first bare metal target Pi 5 post-v1.0.

---

## 23. POSIX surfaces and the Utopia milestone

**STATUS**: COMMITTED

> **Phase 6 (Pouch) + Phase 7 (Utopia) realize this section.** `POUCH-DESIGN.md` is the binding design for the pouch POSIX environment; its §6.6 refines §23.1's "POSIX surfaces are 9P servers" principle into the synthetic-filesystem-as-translation model (the synthetic FS *is* the translation layer). The Utopia userland milestone is execution **Phase 7**; **`docs/UTOPIA.md`** + **`docs/UTOPIA-SHELL-DESIGN.md`** + **`docs/UTOPIA-VISUAL.md`** are the binding designs for the textual layer (the `ut` shell, the native Rust coreutils, the `hx` editor port, the Pale Fire visual identity). The shell + coreutils enumeration below predates the U-1 scripture commit; per `docs/UTOPIA-SHELL-DESIGN.md` the native libthyla-rs runtime + the `ut` shell supersede the original "rc + bash + uutils" deliverable. The kernel-side POSIX surface enumerations in §23.2–§23.7 (`/proc`, `/dev`, `/etc`, `poll`, `pty`, signals, threading, futex) remain binding. The phase-number references in this section predate the Phase-6 insertion; per `ROADMAP.md §2.1` the execution-phase registry there is authoritative.

### 23.1 Design principle: POSIX surfaces are 9P servers

Every POSIX surface in Thylacine is a 9P server that speaks a POSIX-shaped interface. There is no separate compat kernel layer. The mechanism is always: a 9P server mounts at a conventional path and serves the expected file tree. Thylacine-native programs use the underlying 9P interface directly; POSIX programs see what they expect; both are served by the same infrastructure.

This is the Plan 9 principle applied consistently: if it can be a file, it is a file. If it can be a 9P server, it is a 9P server.

### 23.2 Utopia minimum viable POSIX (Phase 7 entry requirement)

Per `VISION.md §13`. The set of surfaces that must exist for Utopia to feel real rather than broken.

**Shell**:
- `ut` — Thylacine's native Rust shell. Plan 9-rc-shaped with refinements (fd-notes job control, `?`+try/catch error model, double-quote interpolation, `case` block, Thylacine-extension builtins like `bind`/`mount`/`cap`/`note`). Built on `libthyla-rs`, no Pouch. Binding design: `docs/UTOPIA-SHELL-DESIGN.md`.
- The shell consumes: `rfork`/`exec`/`wait`, `pipe`/`dup3`, file redirection, note delivery for Ctrl-C, `$path`-equivalent search, job control via fd-notes + poll().
- Bash via Pouch is deferred — not part of Phase 7 Utopia v1. Users who need bash install it themselves via the Pouch path before Phase 8 (Linux compat) lands it as a ROADMAP §9 deliverable.

**Coreutils** — native Rust on libthyla-rs, sub-chunked under U-9..N (binding design: `docs/UTOPIA-SHELL-DESIGN.md §19`). 9base-shaped feature scope, uutils-coreutils-shaped Linux flag compatibility where helpful without bloating. Initial set sized to pass the Utopia bring-up integration test (`docs/UTOPIA-SHELL-DESIGN.md §18`):

```
cat, ls, echo, cp, mv, rm, mkdir, rmdir, ln, touch, chmod
pwd, env, printenv, sleep, true, false, test, [, printf
find, xargs, sort, uniq, wc, head, tail, cut, grep, sed, awk
tr, basename, dirname, realpath, readlink, mktemp
which, type, whence (the last two via shell builtins)
ps (depends on /proc), kill (shell builtin)
```

Plus Plan 9 userland: `mk`, `9` launcher (post-Helix arc).

**Editor**: `hx` — Helix, ported via Pouch under U-Helix. Default `$EDITOR`. Modal Kakoune-style; tree-sitter; LSP. Bundled Pale Fire theme.

**`/proc` synthetic 9P server**:
```
/proc/<pid>/
    status      ← Linux-compat field names (Name, Pid, PPid, State, VmRSS, ...)
    cmdline     ← null-separated argv
    fd/         ← symlinks to open file descriptions
    maps        ← virtual memory map (for ldd, gdb)
    mem         ← raw process memory (privileged, KObj_Process handle required)
    stat        ← Linux-compat stat fields
    statm       ← memory stats
    cwd         ← symlink to current working directory
    exe         ← symlink to executable path
    root        ← symlink to root of process territory
    ns/         ← territory dump
/proc/self/     ← symlink to /proc/<current-pid>/
```

`procfs` is an in-kernel synthetic Dev (faster path); `proc-linux` is a userspace 9P server providing Linux-compat names where they differ.

**`/dev` minimum**:
```
/dev/null, /dev/zero, /dev/random, /dev/urandom
/dev/tty        ← current controlling terminal
/dev/stdin      → /proc/self/fd/0
/dev/stdout     → /proc/self/fd/1
/dev/stderr     → /proc/self/fd/2
/dev/pts/       ← PTY slave ends (§23.5)
/dev/ptmx       ← PTY master allocator
/dev/cons       ← console
/dev/consctl    ← console mode control
```

**`/etc` minimum** (ordinary Stratum files):
```
/etc/passwd, /etc/group, /etc/hostname
/etc/resolv.conf (populated when network is up)
/etc/localtime
/etc/profile, /etc/profile.d/
/etc/ssh/   (post-Phase 7)
```

**`/tmp`, `/run`** — backed by tmpfs (in-kernel Dev). `/var/run` → symlink to `/run`.

### 23.3 `poll` / `select` / `epoll`

**Status**: COMMITTED. `poll` is must-have for Phase 5 (Utopia) — without it interactive bash, curl, Python asyncio, and essentially every non-trivial program are broken — and it is the wait primitive a single-threaded userspace 9P server (`corvus`, CORVUS-DESIGN.md §6.2) uses to serve N connections. It lands as the **P5-poll** chunk (sub-chunks spec / -a / -b), a prerequisite of P5-corvus-srv-impl-b.

**ABI.** `SYS_POLL` (syscall 29): `poll(fds, nfds, timeout_ms) → ready-fd count` (`≥0`; `0` on timeout; `-1` on error). `fds` is a user-VA array of `nfds` (`1..64`, the per-Proc handle-table bound) `struct pollfd`:

```c
struct pollfd {       /* 8 bytes; Linux-shaped — the future musl shim is a no-op */
    int32_t fd;       /* a handle index (hidx_t) — Thylacine has no separate fd layer */
    int16_t events;   /* requested:  POLLIN | POLLOUT | ... */
    int16_t revents;  /* returned:   the subset currently ready */
};
```

Event bits take the Linux values: `POLLIN 0x001`, `POLLOUT 0x004`, `POLLERR 0x008`, `POLLHUP 0x010`, `POLLNVAL 0x020`. `timeout_ms`: `-1` blocks indefinitely, `0` returns immediately (a non-blocking scan), `>0` bounds the wait. `_Static_assert` pins `sizeof(struct pollfd) == 8` and the field offsets.

**Mechanism.** A Thylacine thread waits on exactly one `Rendez` (single-waiter; `rendez.h` extincts on a second). `poll` does **not** make `Rendez` multi-waiter. The poller sleeps on its **own private `Rendez`** via `tsleep` (§8.8 — `timeout_ms` is the deadline), and registers a lightweight `struct poll_waiter` hook on each polled object's **poll-hook list**. The `Dev` vtable gains one op (§9.2):

```c
short (*poll)(struct Spoor *c, short events, struct poll_waiter *pw);
```

`dev->poll` returns the fd's currently-ready `revents` and, if `pw` is non-NULL, registers `pw` on the object's hook list — atomically, under the object's own lock. A NULL `.poll` slot means *always ready* for the requested events: the POSIX-correct answer for a regular file, so only objects with genuine readiness state (`devpipe`, `devsrv`) implement a real `.poll`. When an object becomes ready, its existing wakeup site also walks its poll-hook list, sets each registered `poll_waiter`'s flag, and signals that poller's private `Rendez`.

The load-bearing discipline is **register-then-observe**: `dev->poll` installs the hook and samples readiness in one locked step, so no readiness event between the sample and the sleep is lost. The poller's `tsleep` commit re-checks "any of my `poll_waiter` flags set" under its `Rendez` lock; a producer that readied an fd took that lock to signal — the happens-before that closes the missed-wakeup race across N fds. `poll` unregisters every hook before it returns.

**`select()`**: implemented on top of `poll()`; lands with the broader Phase-5 syscall surface, not in P5-poll.

**`epoll`**: Linux-specific scalable multiplexing. **Deferred to v1.1** — an extension of `poll` semantics, not a separate subsystem. Most programs degrade gracefully to `poll` when `epoll` is absent; those that don't are in the Linux binary compat tier anyway.

**Scope at v1.0.** P5-poll covers readiness for **kernel-fd Spoor kinds** — `devpipe` and the `devsrv` connection + listener (the `corvus` consumer). A 9P-*mounted* file polls always-ready (a regular file is); polling a 9P-served FIFO/device via a synthetic 9P-session readiness notification is a separable follow-on, deferred — no v1.0 consumer needs it.

`poll` is an audit-trigger surface (§25.4): the wakeup race between a thread parking on multiple fds and the poll-hook list lifetime are classic missed-wakeup / use-after-free sources. `specs/poll.tla` proves missed-wakeup-freedom across N fds (I-9) and no-stale-hook; the spec is mandated before merge.

### 23.4 Threading — `pthread` and `futex`

**`pthread`**: musl's pthread maps onto `rfork(RFPROC | RFMEM | RFFDG | RFNAMEG | RFCRED | RFNOTEG)`. Thread-local storage uses `TPIDR_EL0` — saved/restored on context switch.

**`futex`**: load-bearing primitive under all of musl's mutexes, condition variables, and semaphores. Without it, threading is broken under contention.

`futex` syscall semantics supported at Phase 5:
- `FUTEX_WAIT`: sleep if `*addr == val`.
- `FUTEX_WAKE`: wake N waiters on `addr`.
- `FUTEX_WAIT_BITSET` / `FUTEX_WAKE_BITSET`: needed by musl's condvar.
- `FUTEX_REQUEUE`: condvar broadcast efficiency.
- `FUTEX_CMP_REQUEUE`: same with extra check.

Implementation: a kernel hash table keyed by physical address (so futexes work across `mmap`-shared regions). Each bucket has a wait queue. `FUTEX_WAIT` checks `*addr == val` then atomically sleeps on the bucket. `FUTEX_WAKE` wakes N waiters from the bucket.

`futex` is an audit-trigger surface. The wait/wake atomicity invariant is subtle. `specs/futex.tla` covers it.

### 23.5 Pseudo-terminals (PTY)

Required for: Halcyon subprocess hosting, `ssh`, `tmux`, `vim`, any program that checks `isatty()` and changes behavior accordingly.

**Model**: a PTY is a 9P server managing a master/slave fd pair with `termios` semantics layered on a pipe-like channel.

**`/dev/ptmx`**: opening this allocates a new PTY master fd and creates a corresponding slave entry under `/dev/pts/<n>`.

**`/dev/pts/<n>`**: the slave end. Passed to the child process as its controlling terminal. Presents a full `termios` interface.

**`termios` on `/dev/cons`**: the main console device implements `termios` via writes to `/dev/consctl`. Mapping:
- `tcgetattr()` / `tcsetattr()` → structured read/write on `/dev/consctl`.
- Raw mode (`TCSAFLUSH | ~ICANON`) → `"rawon"` written to `/dev/consctl`.
- Cooked mode → `"rawoff"`.
- Echo control → `"echo"` / `"noecho"`.

This covers `vim`, `less`, `bash` readline, `ssh` client.

`pty` is an audit-trigger surface. `specs/pty.tla` covers master/slave atomicity and `termios` state transitions.

### 23.6 Signal translation

Per §16.4. Spec: `notes.tla`.

### 23.7 `/sys` (minimal stub)

Linux-specific. Needed for: dynamic linker path probing, `ldd`, some hardware enumeration tools.

A minimal synthetic `/sys-linux` 9P server sufficient for:
```
/sys/class/net/<iface>/   ← network interface info (for ip, ifconfig)
/sys/block/<dev>/         ← block device info (for lsblk)
/sys/devices/             ← minimal device tree (for udev-dependent programs)
```

Full `/sys` is not a goal. The stub satisfies the dynamic linker and basic admin tools. Programs that parse `/sys` heavily are Linux-admin tools Thylacine doesn't need to run.

Deferred to Phase 6 (Linux binary compat phase).

### 23.8 Phase 7 Utopia priority order

```
Must have (Utopia does not work without these — Phase 7 exit gate):
  libthyla-rs heap allocator + File I/O + Path + Poll + Notes + Command/Child  (U-2)
  ut shell skeleton + libutopia palette + tools/build.sh wiring  (U-3)
  libutopia line editor (raw-mode, emacs keybindings, multi-line, history)  (U-4)
  ut parser + AST (rc-shape with refinements)  (U-5)
  ut evaluator core + main loop + builtins + pipes + redirects + error model  (U-6)
  ut fd-notes job control (Ctrl-C, Ctrl-Z, &, jobs/fg/bg, on note, mask note)  (U-7)
  Thylacine builtins (bind, mount, unmount, pivot_root, rfork, cap, note)  (U-8)
  Native Rust coreutils — initial set (cat, ls, echo, grep, sed, awk, cp, mv, rm, mkdir, find, wc)  (U-9..N)
  Helix port via Pouch  (U-Helix, parallel)
  PTY server (/dev/ptmx, /dev/pts/) + termios via /dev/consctl
  /proc synthetic server (in-kernel) for ps + jobs + on-note's "running" check
  /dev basics (null, zero, random, tty)
  /tmp, /run as tmpfs
  /etc minimum files on Stratum
  Pale Fire visual identity discipline

Should have before Phase 8 (Linux compat + network):
  PTY-related niceties (winch propagation across nested ttys)
  Bash via Pouch (deferred to Phase 8)
  Linux-compat /proc names where they differ from native /proc

Defer to Phase 8 (Linux binary compat + network):
  /sys stub
  setuid/setgid mechanics (deferred-not-implemented per §15.4)
  Extended attributes (xattr) at the territory level
  POSIX ACLs
  uutils-coreutils for Linux-compat flag coverage where native ones diverge
  Stratum-native history (cross-host sync, structured query)

Defer to v1.1+:
  epoll
  inotify (most programs degrade gracefully)
  io_uring
  Sixel and Kitty graphics emission from Thylacine programs
  Syntax highlighting at the prompt
  Plugin model beyond sourceable scripts
```

### 23.9 The Utopia bring-up integration test

At Phase 7 exit, the integration test (run in CI). Authoritative spec: `docs/UTOPIA-SHELL-DESIGN.md §18`. Headline checks:

1. Boot a fresh Thylacine VM.
2. Attach via UART; reach the Pale Fire `ut` prompt at `pwd == $home`.
3. Multi-stage shell pipeline: `cat /etc/passwd | grep root | cut -d: -f1` produces correct output.
4. Job control: `sleep 100 &` shows in `jobs`; Ctrl-Z foregrounds; `fg` resumes; Ctrl-C terminates.
5. Function with `?` propagation: `cmd1?; cmd2?; cmd3?` short-circuits on `cmd2` failure without running `cmd3`.
6. Namespace builtin: `bind /srv/stratum-ctl /n/stratum; ls /n/stratum` shows the Stratum admin surface.
7. Notes via builtin: `note send $$ snare:user1` triggers a registered `on note 'snare:user1' { ... }` handler.
8. `hx /etc/hosts` opens Helix; edit + save observable in the file.
9. rc-shape script: `for (f in *.md) { wc -l $f }` runs.
10. Pale Fire prompt renders with the correct three-segment colour scheme.
11. Assert no kernel extinctions, no driver crashes, no zombie processes.

Network-dependent tests (curl, ssh, git clone) are deferred to Phase 8 (Linux compat + network) along with the network stack itself.

If this passes, Utopia ships at Phase 7 exit.

### 23.10 Open design questions

None at Gate 3.

### 23.11 Summary

POSIX as 9P. Utopia at Phase 7 exit: `ut` (native Rust shell on libthyla-rs) + native Rust coreutils + `hx` (Helix via Pouch) + Pale Fire + Plan 9 namespace builtins + futex + poll + pty + notes + tmpfs. Test: "feels real, not broken." Binding designs: `docs/UTOPIA.md`, `docs/UTOPIA-SHELL-DESIGN.md`, `docs/UTOPIA-VISUAL.md`.

---

## 24. Hardening

**STATUS**: COMMITTED

### 24.1 Goals

Modern security hardening on every binary (kernel + userspace) at v1.0. No "hardening as opt-in" — the default build is the hardened build. SOTA tenet applies.

### 24.2 Compile-time hardening

**Compiler**: Clang (not GCC) for CFI and ARMv8.5 feature support.

**Kernel + userspace flags**:
```cmake
add_compile_options(
    -fstack-protector-strong          # stack canaries
    -fsanitize=cfi                     # control flow integrity
    -fsanitize-cfi-cross-dso           # CFI across shared libs
    -mbranch-protection=standard       # PAC + BTI
    -fPIE                              # position-independent
    -fstack-clash-protection            # stack clash mitigation
    -D_FORTIFY_SOURCE=2                # libc bounds-checking macros
    -Wformat -Wformat-security
    -Wstrict-prototypes -Wmissing-prototypes
)

add_link_options(
    -pie
    -Wl,-z,now                         # full RELRO
    -Wl,-z,relro
    -Wl,-z,noexecstack                 # NX stack
)
```

`-fsanitize=cfi` requires LTO (`-flto=thin`); kernel + userspace builds use ThinLTO.

### 24.3 Runtime hardening

**ARM PAC** (Pointer Authentication, ARMv8.3+):
- Kernel return addresses are PAC-signed at function entry, verified at return.
- PAC keys derived from a kernel boot-time random seed (not exposed to userspace).
- Runtime detection via `ID_AA64ISAR1_EL1.{APA,API}`. If hardware doesn't support, fall back to plain returns.
- Userspace ld.so similarly signs return addresses.

**ARM MTE** (Memory Tagging Extension, ARMv8.5+):
- Default-on where supported (Apple Silicon, recent ARM cores, QEMU emulation).
- Each kernel allocation (via SLUB) gets a hardware tag; loads/stores against tagged memory check the tag matches.
- Userspace heap (musl's mallocator) similarly tagged.
- UAF / overflow caught at hardware speed (no software ASAN overhead).
- Performance overhead: ~5-15% on tagged allocations. Measurement at Phase 8.

**ARM BTI** (Branch Target Identification, ARMv8.5+):
- Indirect branches must land on a `BTI` instruction.
- Compiler-generated; runtime check on the CPU.
- Catches indirect-call hijacking even if the attacker can corrupt a function pointer.

**KASLR**: per §5.3.

**Userspace ASLR**: musl ld.so randomizes load base; kernel randomizes mmap region.

**W^X**: per §6.6. Enumerated invariant I-12.

**Stack canaries**: per-function entry inserts a random canary before the saved frame pointer; verified on return; mismatch = abort.

**Hardened malloc** (musl + scudo or similar): zero-initialized allocations on free; randomized allocation order; separate quarantine for recently-freed memory.

### 24.4 LSE atomics

ARMv8.1+ Large System Extensions provide hardware atomic instructions (`LDADD`, `STADD`, `CAS`, etc.) that don't require LL/SC retry loops. Performance benefit is significant on multi-core (LL/SC retries scale poorly).

**Lazarus W1 + W1.5 (`PORTABILITY.md` §4 + §4.5).** The kernel compiles to the ARMv8.0-A floor (`-march=armv8-a`, no `+lse`), so every C11 `__atomic_*` op inlines LL/SC (`ldaxr`/`stlxr`) — correct on every v8.0 core, the A72 / Pi 400 included. **LSE is restored with zero steady-state runtime cost** by a boot-time alternatives-patcher (W1.5): the read-modify-write atomic primitives are authored LL/SC-default with the single-instruction LSE form recorded in an `.altinstructions`-style table, and a one-shot pass in `boot_main` — after `hw_features_detect()` (which reads `ID_AA64ISAR0_EL1.Atomic`), before `smp_init()` — rewrites each site to LSE iff `g_hw_features.atomic`. This is *not* a first-call branch and *not* outline-atomics (the userspace call+branch form, which no kernel uses in-kernel); it is the Linux ARM64 model — after boot the code is exactly as if compiled for that CPU.

```asm
/* arch/arm64/atomic_lse.h: each RMW primitive, LL/SC default, with an
   .altinstructions entry recording the single-instruction LSE replacement.
   alternatives.c's boot pass copies the LSE form over the LL/SC site (NOP-
   padding the tail to the LL/SC length) iff FEAT_LSE is present. */
    /* default: LL/SC (runs everywhere) */
1:  ldaxr   x0, [x2]
    add     x3, x0, x1
    stlxr   w4, x3, [x2]
    cbnz    w4, 1b
    /* boot-patched, where FEAT_LSE present, to: */
    ldaddal x1, x0, [x2]   /* + NOP pad to the LL/SC length */
```

The patcher writes through a transient RW-not-X alias of the target `.text` page (the canonical mapping stays RO+X), so W^X (I-12) is never violated; ARM-ARM-B2 cache maintenance (`dc cvau` / `ic ivau` / `dsb` / `isb`) follows. LL/SC is the always-correct fallback — a patcher early-out, or any v8.0 core, simply keeps LL/SC. Audit-bearing; full design in `PORTABILITY.md` §4.5.

### 24.5 KASAN-equivalent in userspace

A development-build KASAN-like instrumentation for userspace: every memory access is shadow-mapped; freed memory tagged; accesses to invalid memory abort.

This is `-fsanitize=address` at compile time. Significant performance overhead; only used in development. Production binaries don't have it; MTE provides hardware-equivalent protection where supported.

### 24.6 Audit-trigger surface integration

Every change to:
- Page tables (`mm/vm.c`, `arch/arm64/mmu.c`).
- Capability checks.
- Handle table.
- 9P client / server.
- Crypto code (in janus or anywhere else).

triggers an audit round per §25.4.

### 24.7 Hardening counters

`/ctl/security/` exposes runtime stats:
- `wx-violations` (should always be 0).
- `pac-mismatches` (kernel return-address tampering attempts).
- `mte-mismatches` (UAF / overflow catches).
- `bti-mismatches` (indirect-branch-to-non-BTI-instruction).
- `cfi-violations` (indirect call to wrong function signature).
- `stack-canary-mismatches`.

Each counter logs with caller details for forensic analysis.

### 24.8 Open design questions

None at Gate 3.

### 24.9 Summary

Full SOTA hardening: KASLR, ASLR, W^X (invariant), CFI, stack canaries, PAC, MTE, BTI, LSE atomics. All on by default. ARMv8.5-class hardware (Apple Silicon) gets full coverage; older hardware gets graceful fallback.

---

## 25. Verification cadence and audit triggers

**STATUS**: COMMITTED

### 25.1 The cadence

Per `NOVEL.md` Angle #8. Practical TLA+ verification of every load-bearing OS invariant. Spec-first: the TLA+ model is written before the implementation; TLC catches design errors at the spec level where they cost minutes, not at runtime where they cost commits.

### 25.2 The nine specs

| # | Spec | Phase | Invariants |
|---|---|---|---|
| 1 | `specs/scheduler.tla` | 2 | EEVDF correctness, IPI ordering, wakeup atomicity, work-stealing fairness |
| 2 | `specs/territory.tla` | 2 | bind/mount semantics, cycle-freedom, isolation between processes |
| 3 | `specs/handles.tla` | 2 | Rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability |
| 4 | `specs/burrow.tla` | 3 | Refcount + mapping lifecycle, no-use-after-free |
| 5 | `specs/9p_client.tla` | 4 | Tag uniqueness per session, fid lifecycle, out-of-order completion correctness, flow control |
| 6 | `specs/poll.tla` | 5 | Wait/wake state machine, missed-wakeup-freedom across N fds |
| 7 | `specs/futex.tla` | 5 | FUTEX_WAIT / FUTEX_WAKE atomicity (no wakeup lost between value check and sleep) |
| 8 | `specs/notes.tla` | 5 | Note delivery ordering, signal mask correctness, async safety |
| 9 | `specs/pty.tla` | 5 | Master/slave atomicity, termios state transitions |

### 25.3 Spec → code mapping

`SPEC-TO-CODE.md` per spec. Each spec action maps to a source location. CI verifies the mapping is current — file must exist, function must exist, line range must match.

Example for `scheduler.tla`:
- `EpochEnter` ↔ `kernel/sched.c:sched_enter()` lines 145-189
- `EpochExit` ↔ `kernel/sched.c:sched_exit()` lines 192-220
- `IPIRecv` ↔ `kernel/smp.c:ipi_resched_handler()` (~line 100)
- ... etc

### 25.4 Audit-trigger surfaces

Every change to a file or function listed below spawns an adversarial soundness audit before merge. Updated at every ARCH change. **The convergence-detour BUILD items (IDENTITY-DESIGN.md §8.4) — SMMU/DMA isolation, the new syscall surfaces (clock, FS-mutation, `CAP_KILL`, PAN), the resource floor, the orphan reaper — are upcoming audit-bearing surfaces, each appended here in the PR that introduces it.**

| Surface | Files | Why |
|---|---|---|
| Exception entry | `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S` | Every syscall, IRQ, fault path |
| Halls crash dump (HX-1) | `arch/arm64/halls.c`, `arch/arm64/halls.h`, `arch/arm64/exception.c` (entry wrappers), `kernel/extinction.c` | Tier-1 fatal-path dump; runs on a dying machine. HX-I1 re-entrancy guard (no loop), HX-I2 bounded sanity-gated fp walk, HX-I3 `EXTINCTION:` ABI line unchanged. See `docs/reference/101-halls.md`. |
| Page fault | `arch/arm64/fault.c`, `mm/vm.c` | Lifetime, demand-page, COW, W^X |
| Allocator | `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c` | Allocation correctness, lock-free invariants |
| Scheduler | `kernel/sched.c`, `arch/arm64/context.c`, `kernel/smp.c` (IPI logic) | EEVDF correctness, SMP, wakeup atomicity |
| Territory | `kernel/territory.c` | Cycle-freedom, isolation |
| Handle table | `kernel/handle.c` | Rights monotonicity, transfer rules, hardware-handle non-transferability |
| BURROW | `kernel/burrow.c`, `mm/burrow_pages.c` | Refcount, mapping lifecycle |
| 9P client (pipeline restoration, #841) | `kernel/9p_client.c`, `kernel/9p_session.c`, `kernel/9p_transport.c`, `kernel/9p_attach.c`, the SrvConn transport boundary (`kernel/srvconn.c` client send/recv + `kernel/9p_srvconn_transport.c`) | Tag uniqueness (I-10), fid lifecycle (I-11), no-lost-wakeup (I-9 specialized to the per-rpc rendez), flow control, out-of-order completion. Restores committed §21/§21.10 pipelining (elected-reader, Plan 9 `mountio`) from the R15-c F230 serial regression — the single spinlock held across the blocking `recv` + the 30 s per-op deadline that desynced the shared byte stream (the stalk-3c-d soundness bug). Elected-reader = lock NEVER held across `recv`; multi-in-flight tag-demux; no per-op timeout (block until reply or ring-EOF, death-interruptible via #811). **No new spec** per the 2026-05-23 broadening — but `specs/9p_client.tla` (clean + the 4 buggy cfgs) is the pre-commit gate; prose validation in ARCH §21.10 + this row + the audit + the multi-in-flight runtime tests + the #841 UBSan/forkstorm repro. |
| poll | `kernel/poll.c` | Wait/wake across N fds, missed-wakeup-freedom (I-9), poll-hook list lifetime |
| Notes / signals | `kernel/notes.c`, `kernel/devnotes.c`, `kernel/include/thylacine/notes.h`, `kernel/proc.c` (synthetic `child_exit` post in `exits`), `kernel/pipe.c` (synthetic `pipe` post on write-to-closed), `arch/arm64/exception.c` (EL0-return-tail delivery) | Delivery ordering (I-19); async-safety (delivery only at zero-lock EL0-return tail); N-2 consumed-exactly-once across handler + fd-read paths; N-3 in_handler re-entrancy guard; N-4 `kill` non-catchable. **No `specs/notes.tla`** per the 2026-05-23 spec-to-code broadening — prose validation in `notes.h` + the audit + the runtime test suite are the rigor. |
| Capability checks | All syscall entry points | Privilege correctness |
| KASLR / ASLR | `arch/arm64/start.S`, `kernel/aslr.c` | Entropy quality, layout correctness |
| Crypto code | None in v1.0 kernel; janus in userspace | Side-channel, key handling |
| ELF loader | `kernel/elf.c` | RWX rejection, relocation correctness |
| `burrow_attach` / `burrow_detach` | `kernel/syscall.c` handlers, `kernel/burrow.c`, `kernel/vma.c` | Anonymous-memory syscalls (§6.5 Tier 1) — VMA + Burrow refcount lifecycle, VA placement, per-Proc lock, W^X (RW-only) |
| Initial bringup | `kernel/main.c`, `init/joey.c` | Boot ordering correctness |
| pouch lower half + kernel additions | `usr/lib/pouch/` (the syscall seam; socket / thread / signal translation), `kernel/` auxv population + the `torpor` wait-on-address syscall + the allocator-backend call | The POSIX→Thylacine boundary; invariants P-1..P-4 (POUCH-DESIGN.md §11); the `torpor` wait/wake (`futex.tla`). Phase 6 surface — rows enumerated per sub-chunk in POUCH-DESIGN.md §14. |
| FS-mutation syscalls (create / fsync / readdir) | `kernel/syscall.c` (`sys_walk_create_handler` / `sys_fsync_handler` / `sys_readdir_handler`), `kernel/dev9p.c` (real `dev9p_create` + new `dev9p_fsync` / `dev9p_readdir`), `kernel/devramfs.c` (create/fsync/readdir impls), `kernel/include/thylacine/dev.h` (new `.fsync` / `.readdir` vtable slots), `kernel/include/thylacine/syscall.h` (`SYS_WALK_CREATE = 54` / `SYS_FSYNC = 55` / `SYS_READDIR = 56` + ABI), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers | Convergence-detour FS foundation (IDENTITY-DESIGN.md §9.2), pulled ahead of A-1b corvus persistence. The create + write + fsync path is the AEGIS/mallocng-adjacent surface from Phase 6 — prosecute hard. Rights gates (`RIGHT_WRITE` on parent for create/fsync; `RIGHT_READ` for readdir); single-component name bounds + `/`-`\0` reject; `perm` reserved-`DM*`-bit reject; `DMDIR`-fold (mkdir vs lcreate); `readdir` buffer bounds + offset advance; durability contract; dev9p fid lifecycle on create/clunk (UAF on a failed create path). Per-file rwx enforcement is A-2d, NOT this surface (I-22 holds — no enforcement exists yet to bypass). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in IDENTITY-DESIGN.md §9.2 + this row + the audit + the runtime tests. |
| FS-mutation syscalls (rename / unlink) — FS-gamma | `kernel/syscall.c` (`sys_rename_handler` / `sys_unlink_handler`), `kernel/dev9p.c` (new `dev9p_rename` → `p9_client_renameat`, `dev9p_unlink` → `p9_client_unlinkat`), `kernel/include/thylacine/dev.h` (new `.rename` / `.unlink` vtable slots), `kernel/include/thylacine/syscall.h` (`SYS_RENAME = 57` / `SYS_UNLINK = 58` + `SYS_UNLINK_REMOVEDIR` + ABI), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers | Convergence-detour FS-gamma (IDENTITY-DESIGN.md §9.3), pulled ahead of A-1b to give corvus's identity-DB persistence the classic write-tmp + fsync + atomic rename-swap substrate. **First end-to-end exercise of `p9_client_renameat` / `unlinkat`** (implemented Phase 5, never driven by any syscall) — prosecute hard, same AEGIS/mallocng-adjacent write-path class as §9.2. Invariants: rights gates (`RIGHT_WRITE` on every directory fd mutated — both for rename); single-component name bounds + `/`/`\0`/`.`/`..` reject on every name; the **cross-Dev + same-session reject** (rename runs directly on the two looked-up dir Spoors — no clone-walk, since renameat doesn't transition the dirfid — and requires the same Dev, with `dev9p_rename` adding the same-`p9_client` check; rejected at the handler before any Dev op); `flags` validated against `{0, SYS_UNLINK_REMOVEDIR}`; the no-fid-leak property (these ops borrow the caller's dir fid and allocate no transient fid, so the §9.2 failed-create UAF class does not arise); rename's POSIX atomic-replace semantics; the rename-swap durability detail (post-rename `Tsync` on the parent dir as the metadata barrier — validated end-to-end by the A-1b cross-reboot persistence test). The pre-existing `void (*remove)` Plan 9 slot is left as-is (wrong shape; `SYS_UNLINK` uses the new `.unlink`). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in IDENTITY-DESIGN.md §9.3 + this row + the audit + the runtime tests. |
| Capability-scoped service storage | `usr/joey/joey.c` (post-pivot: create `/var/lib/corvus` → `handle_dup`→`R|W` → endow as corvus's storage fd at spawn), `usr/corvus/src/main.rs` (use the handed fd as `storage_root` for all persistence), `usr/lib/libthyla-rs`; depends on **FS-delta** (the `SYS_WALK_OPEN` `T_OPATH` walk-without-open primitive, landed first; IDENTITY §9.4) | Convergence detour A-1.7 (ARCH §3.6; NOVEL §3.10 lead angle #10) — the storage-capability substrate, proven on corvus. Invariant **I-23** (FS authority bounded by the handed capability). Prosecuted: rights reduction (`R|W`, no `TRANSFER` -- least authority; monotonic I-6 through the spawn-fd endow); confinement (the chroot'd service cannot name a path outside its subtree); the shared-9P-session lifetime (corvus outlives joey -- SOUND via the `p9_attached_ref` chain). **Audit R1 CLEAN** (0 P0 / 1 P1 / 1 P2 / 3 P3): F1 (the "withholding `TRANSFER` blocks re-handing" claim was FALSE -> scripture-corrected to the monotonic bound), F2 (confinement is cooperative -> corvus now chroots FIRST), F5 (`T_OPATH` born `R|W`); F3/F4 P3 documented. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in ARCH §3.6 + this row + the audit + the corvus confinement smoke. |
| Kernel rwx enforcement layer (A-2d) | `kernel/syscall.c` (`perm_check` insertions in `sys_walk_open_handler` [X on src + R/W on target], `sys_walk_create_handler` [W+X on parent], `sys_wstat_handler` [the chmod/chown/chgrp policy]), new `kernel/perm.c` `perm_check` + `proc_in_group`, `kernel/dev9p.c` (`dev9p_stat_native` gated on the `Rgetattr` valid mask -- closes A-2a F2) | Convergence-detour A-2d (IDENTITY-DESIGN.md §3.7.1 + §9.6; privilege model voted 2026-05-30). The first real exercise of **I-22**'s enforcement obligation -- the Linux-VFS model (kernel enforces rwx at the FS chokepoint; Stratum enforces dataset-scope ONLY, not file rwx, §3.7). Owner-first POSIX algorithm; group membership = `primary_gid` OR `supp_gids[0..count)`; enforcement at walk (X on the searched dir, per-component since walk is one-name-per-call), open (R/W per omode; `O_PATH` exempt from R/W but not from the X-search), create (W+X on parent), wstat (the policy); read/write not re-checked (open-time snapshot); the handle RIGHT (capability axis) AND the rwx check (identity axis) both required. **`CAP_HOSTOWNER` is the unified v1.0 fs-admin authority** (DAC-override + chmod/chown/chgrp-any) -- a capability (elevation-only, console-gated, never rfork-able), never an identity, so no `principal_id` -- not even `PRINCIPAL_SYSTEM` -- bypasses (**I-22 preserved**); owner keeps owner|self-group chmod + chgrp-to-own-group; no-give-away `chown` (`CAP_HOSTOWNER` only). Fail-closed on a NULL `stat_native` Dev. Honest scope: per-principal-real on devramfs (system-owned world-r/x -> boot chain owns everything, no brick; a `CAP_SET_IDENTITY`-spawned non-system child is denied write -- testable now, not gated on login A-5). **dev9p enforcement DEFERRED to A-3** (user-signed-off 2026-05-30): uniform dev9p enforcement bricks the boot (host-bake stamps pool entries host-uid-owned 0644/0755; the `PRINCIPAL_SYSTEM` boot chain as *other* cannot write the pool -> post-pivot creates denied). Gated by a new `Dev.perm_enforced` flag (devramfs=true, dev9p=false; A-3 = one-line flip); dev9p stays handle-RIGHT-gated only at v1.0. The wstat policy is also `perm_enforced`-gated (dormant + unit-tested at v1.0). A-2b create-check folds in; A-2c mount-cape stays a seam; A-4 splits a finer `fs-admin` clearance (`CAP_DAC_OVERRIDE`+`CAP_CHOWN`) additively. **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §3.7.1 + §9.6 + this row + the audit + the runtime tests. |
| A-3: 9P identity presentation + dev9p enforcement activation | `usr/lib/pouch/patches/0006-pouch-sockets.patch` (SO_PEERCRED shim: `ucred.uid = info.principal_id`, `ucred.gid = info.primary_gid` -- was `0`/`0`), `kernel/dev9p.c` (`.perm_enforced = true` flip), `kernel/syscall.c` (`sys_walk_open_handler` F1: derive handle rights from omode; `sys_rename_handler`+`sys_unlink_handler` F2: `perm_check(parent, W\|X)`; `sys_attach_9p_handler`+`sys_attach_9p_srv_handler` M4: substitute `principal_id` for `n_uname`), `tools/build.sh::build_stratum_pool_fixture` (`--bake-owner-uid PRINCIPAL_SYSTEM`), Stratum `thylacine-pouch-arm` `src/cmd/stratumd/{run.c,serve.c}` (`--bake-owner-uid`/`--bake-owner-gid` override of `s->auth_uid`/`auth_gid`) | Convergence-detour A-3 (IDENTITY-DESIGN.md §9.7 + the §3.5 F-4 correction + §3.7.1 activation note; two user votes 2026-05-31: SO_PEERCRED channel + flip-now). **Activates dev9p rwx enforcement -- privilege boundary, AEGIS-adjacent write path; prosecute hard.** Corrects F-4: the load-bearing trusted-local identity channel is **`SO_PEERCRED`** (kernel-stamped via `SYS_srv_peer`, *unforgeable*), not `n_uname` (which Stratum ignores). Reconciliation (no-brick): host-bake stamps the pool `PRINCIPAL_SYSTEM`-owned (Stratum `--bake-owner-uid`; NOT an on-disk-format change -- `si_uid`/`si_gid` exist), pouch SO_PEERCRED carries `principal_id`, so the kernel-side `perm_check` is coherent and the boot chain (owner) is not denied. **Invariants:** I-22 preserved (the principal is kernel-stamped, not client-asserted -- no identity self-elevates; `CAP_HOSTOWNER` remains the only DAC-override); I-2/I-4/I-6 unaffected (no cap/transfer added; F1 *narrows* the handle envelope to omode-derived rights); A-1.7/I-23 preserved (F1's `T_OPATH` carve-out keeps the storage-capability base born `R\|W`, no TRANSFER); no-brick (boot OK + cross-reboot PASS are the gate). Closes A-2d F1 (handle-rights-from-omode) + F2 (perm_check on rename [both dirs] + unlink). `n_uname` forwarding kept but demoted to the v1.x foreign/authenticated path; the corvus trust-stamp gate is a v1.x SEAM (every v1.0 attach is local SO_PEERCRED-bearing -> no untrusted-assertion to gate). Per-user stratumd `--role client` (Stratum A2, verified merged) mechanism proven via a dataset-scope `EACCES`-at-Tattach probe; the per-login spawn is the A-5 consumer. **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.7 + this row + the audit + the runtime tests. |
| Group termination / cross-thread shootdown (`SYS_EXIT_GROUP`) | `kernel/proc.c` (`proc_group_terminate` + the single set-once `group_exit_msg` on `struct Proc`; the group-status variant of the last-Thread-out ZOMBIE reap; `exits`/`thread_exit_self` group-status path), `arch/arm64/exception.c` (`el0_return_die_check` at the sync-from-EL0 tail), `arch/arm64/vectors.S` (the **new IRQ-from-EL0 return-tail die-check** -- `#713`-safe), `kernel/torpor.c` (`torpor_wake_all_for_proc`), `kernel/smp.c` (`smp_resched_others` -- broadcast `IPI_RESCHED`), `kernel/syscall.c` (`SYS_EXIT_GROUP = 60` handler + the `kill`-cascade replacing the multi-thread refusal in `sys_postnote`), `kernel/include/thylacine/syscall.h` (`SYS_EXIT_GROUP = 60` ABI), `usr/lib/pouch/patches/0001-pouch-syscall-seam.patch` (`__NR_exit_group` 0 → 60), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers | SYS_EXIT_GROUP (#809, `89456e9`; pulls the documented v1.x lift forward -- #808 audit F3). **Invariant I-24** (group termination atomic + exactly-once + no-lost-wakeup). The privilege/lifetime boundary AND a wait/wake surface -- prosecute hard. As-built model = flag-and-self-terminate at the EL0-return checkpoint via a **single per-Proc set-once `group_exit_msg`** (NULL-sentinel CAS = die-flag + last-out status) + `torpor_wake_all_for_proc` + broadcast `smp_resched_others` (NOT the abandoned `die_requested`/per-Thread-`cpu`/`group_exiting`/`group_exit_status`/targeted-IPI design — F2 reconcile). Plan 9 / Linux / Zircon convergent (seL4 sync-stall rejected -- see §7.9.1). Invariants: **I-9** (the sleeper-wake reuses register-then-observe under the per-condition lock — `torpor_lock` for `torpor`, per-Thread `wait_lock` for all other rendez sleeps (§8.8.1, task #811); lock order `g_proc_table_lock → wait_lock → r->lock`); **#713** (the IRQ-from-EL0 die-check sits before the DAIF-masked eret window; the die path is noreturn); **#788** (`on_cpu`-spin before any peer `thread_free`); **I-8** (every flagged Thread eventually reaches its checkpoint -- broadcast IPI, timer-tick floor, wake for sleepers); status ok/fail collapse. Closes the `exits`-with-live-peers extinction (the `tools/test.sh` flake) + `kill → -EIO in multi-thread Proc` (13b R1-F9). **v1.0 residual → RESOLVED by task #811** (§7.9.1 [OPEN Q 7.9.A] = B, universal, user-voted 2026-05-31): the #809 audit (F1) showed the residual is a non-reaping HANG -- an indefinite `poll(-1)` / `pipe` / `devnotes_read` sleeper is un-woken; §8.8.1 makes the cascade wake total. The multi-thread **fault** path (`proc_fault_terminate`) is a tracked follow-up (#810), not this chunk. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in §7.9.1 + this row + the audit + the runtime tests. |
| Universal death-interruptible sleep (`*_INTR`) | `kernel/sched.c` (`sleep`/`tsleep` generalized register-then-observe of `group_exit_msg` + the `SLEEP_INTR`/`TSLEEP_INTR` return), `kernel/include/thylacine/thread.h` (per-Thread `wait_lock` protecting `rendez_blocked_on`; only the owner mutates it), `kernel/include/thylacine/rendez.h` (the `*_INTR` sentinel + contract), `kernel/proc.c` (`proc_group_terminate` walks `p->threads` + wakes each sleeper via `wait_lock`→`rendez_blocked_on`; `exits()` folds into the same universal wake), and **every** blocking site's "on `*_INTR` → cleanup → return" arm: `kernel/poll.c`, `kernel/pipe.c` (read + write), `kernel/devnotes.c`, `kernel/srvconn.c` (client + server recv), `kernel/devsrv.c` (accept), `kernel/irqfwd.c`, `kernel/proc.c` (`wait_pid`) | Task #811 -- the F1=B completion of `SYS_EXIT_GROUP` (#809-audit F1); §8.8.1. **The wait/wake primitive itself -- prosecute hard.** Invariant: **I-9** generalized (no death-wake lost between a sleeper's cond-check and its sleep, for **every** rendez sleep -- register-then-observe under the per-Thread `wait_lock`, the Plan 9 `p->rlock` analog); completes **I-24**'s "no Thread runs at EL0 after ZOMBIE" for the indefinite-sleeper class. Lock order: `wait_lock` is the **outermost** wait-lock (`wait_lock → g_timerwait.lock → r->lock`; waker `g_proc_table_lock → wait_lock → wakeup`); acyclic because only the owning Thread writes `rendez_blocked_on` and no sleeper holds `g_proc_table_lock` below `wait_lock` (`wait_pid` drops it first). Death unwinds at the EL0-return tail (`el0_return_die_check`), never inside `sleep()` (would strand caller locks). Re-validate each site's cleanup + I-9 in the audit (dirty-class follow-up per the #809 close). Closes #809-audit F4 (`exits()` fold-in). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in §8.8.1 + this row + the audit + per-site regression tests. |
| A-4 capability model + legate elevation (`rfork` I-2 strip; `cap` device clearance grant/redeem) | `kernel/include/thylacine/caps.h` (`CAP_GRANT_CLEARANCE`=1<<6 fork-grantable; `CAP_DAC_OVERRIDE`=1<<7 / `CAP_CHOWN`=1<<8 / `CAP_KILL`=1<<9 elevation-only; `CAP_ELEVATION_ONLY` expands to all four elevation-only bits), `kernel/proc.c` (`rfork_internal` ANDs `~CAP_ELEVATION_ONLY` -- the A-4-pre I-2 fix; `legate_scope_id`/`legate_session_id`/`legate_valid_until` on `struct Proc`; scope-teardown via `proc_group_terminate`), `kernel/devcap.c` (the `grant`/`use` files generalized from CAP_HOSTOWNER-only to arbitrary clearance cap-sets + `valid_until` -- the `grant` file is LENGTH-discriminated [16 = hostowner, 32 = clearance], the `use` file does ONE locked kind-branched redeem; the redeem rides the EXISTING `/cap/use` file -- NO new REDEEM syscall -- and CREATES a legate via `kernel/proc.c::proc_become_legate`; the clearance GRANT rides the new `SYS_CAP_GRANT_CLEARANCE` = 61 syscall [the grant-side bridge mirroring `SYS_CAP_GRANT`: corvus is chrooted to its storage cap and reaches the cap device by syscall, NOT a `/cap` file walk, exactly as the hostowner grant already does -- the 32-byte `/cap/grant` Dev write stays the conceptual path for un-chrooted writers + tests]), `kernel/perm.c` (honor `CAP_DAC_OVERRIDE` in `perm_check` + `CAP_CHOWN` in `perm_wstat_check`); A-4a-3 lands `SYS_CAP_GRANT_CLEARANCE` + the libthyla-rs `cap.rs` clearance grant/redeem wrappers + the corvus CLEARANCE verbs (14-17) + the E2E legate prover | A-4-pre + A-4a. **Invariants I-2 (the elevation-only strip -- the named P5-hostowner hole), I-25 (legate scope bounded + fully revoked), I-22 (no identity carries ambient authority).** Highest-stakes privilege surface -- prosecute hard. Prosecute: no elevation-only leak across `rfork`/`SYS_SPAWN_WITH_CAPS`; the `cap`-device grant lifecycle (no replay, no cross-stripes redeem, `valid_until` honored, `self_restriction` only reduces); scope teardown exactly-once + reuses #809/#811 correctly (no orphaned elevated Proc). **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| A-4b cross-process kill (`/proc/<pid>/ctl` + `CAP_KILL`) | `kernel/devproc.c` (the `ctl` write parses `kill`/`killgrp` -> `proc_group_terminate` uniformly under `g_proc_table_lock` via the `proc_for_each` resolve+authorize idiom -- the `#811` wake-total primitive; new `stat_native` reporting the target's `principal_id`/`primary_gid`/`0600`; **`perm_enforced = false`** -- the two-axis check (owner [same `principal_id` on the `0600` ctl] OR `CAP_HOSTOWNER` OR `CAP_KILL`; computed DIRECTLY, NOT via the `perm_check` DAC-override -> `CAP_DAC_OVERRIDE` is NOT a kill axis, keeping fs-admin orthogonal to kill) runs at the WRITE site in `devproc_write`, NOT at open: the SHARED open chokepoint hard-rejects pre-`devproc.open` so the gate-at-open sketch could not host the `CAP_KILL` axis -- reconciled 2026-06-01, user vote). USER-REACHABILITY of `/proc` is a Utopia namespace seam (needs the `namec` multi-component mount-crossing resolver + a boot-path mount); A-4b lands + kernel-unit-tests the mechanism + authority (the privilege logic is fully in-kernel testable). | A-4b. **Invariant I-26 (cross-process control two-axis; composes I-22 + I-1).** Privilege boundary -- prosecute hard. Prosecute: no identity bypass of the kill gate; namespace-visibility containment; the parent-of-target case still works; no UAF resolving `<pid>` -> Proc under the lock (resolve+authorize+kill all under `g_proc_table_lock`); multi-thread cascade correctness. **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| A-4c trusted path: kernel console RX + SAK | `arch/arm64/uart.c` (RX IRQ + IMSC.RXIM unmask + RX-FIFO drain + `DR.BE` BREAK detect; PL011 SPI 33 hardcoded as the QEMU-virt fallback -- DTB `interrupts`-parsing is a Lazarus seam), `kernel/main.c` (boot wiring: `gic_attach`+`gic_enable_irq` for the UART SPI, alongside the timer), `kernel/irqfwd.c` (reserve the UART SPI INTID, like the timer, so userspace `SYS_IRQ_CREATE` cannot claim it), `kernel/cons.c` (`devcons_read` real blocking read on a `Rendez` + single-reader busy-guard + a console input ring; the IRQ handler is wakeup-only -> the `console_mgr` kproc kthread does the deferred privileged work in process context [SAK = serial BREAK recognized pre-EL0; Ctrl-C -> `interrupt` note]), `kernel/proc.c` (`proc_revoke_console_attached` [atomic `proc_flags` clear] + the single `g_console_owner` pointer under `g_proc_table_lock` + `exits()` clear-on-owner-exit + re-grant to corvus via `g_console_trusted_proc`, FAIL-SAFE revoke-only if absent + notify), `kernel/devcap.c` (the redeem gate keys on `PROC_FLAG_CONSOLE_ATTACHED`) | A-4c-1 (console RX pull-forward, Phase-4-G work) + A-4c-2 (SAK + handoff). **Invariant I-27 (trusted path: unspoofable elevation prompt).** New EL0-bound input path + a privilege boundary -- prosecute hard. Prosecute: the RX ring (no overflow, no missed-wakeup on the `Rendez`); Ctrl-C delivery; the SAK recognizer cannot be starved/spoofed by crafted input (structural -- BREAK is a line condition, not data); the console-attach revoke/re-grant is atomic + leaves exactly one owner; only the console-attach holder can redeem elevation. On the kernel UART console Dev (`dc='c'`) -- the userspace VirtIO-input path is unaffected (ARCH §17.1). **Test note**: the harness cannot inject UART RX non-interactively (`-serial mon:stdio` + `< /dev/null`, one PL011, no QMP serial channel) without touching the boot-banner test ABI -> proven by in-kernel unit tests (synthetic RX-handler/recognizer/owner-transition drive) + boot survival + the interactive `Ctrl-A b` BREAK path. **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| A-5 login + session lifecycle + per-user encrypted home | NEW `usr/login/` (native `/sbin/login`, libthyla-rs: SAK-gated `/dev/cons` prompt -> corvus `AUTH` client -> `CAP_SET_IDENTITY` stamp via `SYS_SPAWN_FULL_ARGV`'s `SPAWN_IDENTITY_SET` -> per-user `--role client` stratumd + `/home/<user>` bind -> spawn `ut` as the session leader; logout = group-terminate + unmount + corvus `SESSION_CLOSE`), `usr/joey/joey.c` (spawn `/sbin/login` post-pivot + relinquish its boot console-attach at the bringup->session boundary), `kernel/proc.c` (the joey console-attach relinquish + the OPTIONAL `SPAWN_PERM_CONSOLE_OWNER` -> `g_console_owner`-without-attach, for Ctrl-C-to-shell), `kernel/include/thylacine/syscall.h` + `kernel/syscall.c` (`SPAWN_PERM_CONSOLE_OWNER` if added); **Stratum-side** (`thylacine-pouch-arm`) `src/sync/sync.c` (deferred-unwrap soft-skip of out-of-scope CURRENT slots, `sync_unwrap_cb`) + a runtime DEK install/evict consumer (reusing `stm_corvus_unwrap`) + the login token-forward | Convergence-detour A-5 (IDENTITY-DESIGN.md §9.9; 3 votes + a refining 4th, 2026-06-02). The capstone integration -- composes I-1 / I-22 / I-27 + the A-4 caps; adds NO new ARCH §28 invariant. **The DEK handoff is AEGIS/mallocng-adjacent -- prosecute hard.** Prosecute: **I-27** (login + the user shell are NEVER console-attached; the joey relinquish preserves "corvus is the sole console-attached Proc during a session"; no interposer between the SAK and the corvus prompt); the **identity stamp** (`CAP_SET_IDENTITY` gate; login stamps only the principal corvus authenticated; no forge); the **DEK handoff** (login never holds the raw DEK; the token-forward leaks no secret via argv/files; the coordinator install/evict has no UAF/leak; eviction actually zeroes); **user-vs-user isolation** (a 2nd user's session cannot unwrap or attach the first's dataset -- dataset-scope EACCES + the per-user-sealed DEK); the **session teardown** (no orphaned session Proc; the kill cascade is total per #811); the **Stratum deferred-unwrap** (a soft-skipped dataset is provably unreadable until its DEK is installed; the install validates the forwarded token). **A-5b DEK transport (RESOLVED 2026-06-02, user-voted; corrects the same-day "no corvus lift" note):** the coordinator PULLS the DEK with the login-forwarded token over its own corvus connection (§6.3), enabled by (a) the pouch `connect()` walking to corvus's `ctl` fid (`usr/lib/pouch/patches/0006-pouch-sockets.patch`, audit-bearing boundary-line; the kernel 2-arg `SYS_srv_connect` already drives the walk) and (b) the **corvus session-ownership lift** (`usr/corvus/src/main.rs` -- the AUTH-SESSION owning-connection tag + the `close_conn` clear-gate: clear the global AUTH session only on the OWNING connection's close or explicit SESSION_CLOSE, never on a non-owning bearer-token connection's close -- else the coordinator's transient connection wipes login's live session mid-session and breaks A-4 legate elevation). PROSECUTE both new surfaces: the corvus session-ownership change (no cross-session wipe; owning-connection tag unforgeable; the §4.2/§6.2 intent realized) + the pouch ctl-walk transport. corvus-PUSH rejected (role inversion; corvus lacks the storage layout). The security property (at-rest + session-scoped, login-never-holds-raw-DEK, evict-at-logout) is FIXED. Split A-5a (login core; Stratum-independent) / A-5b (encrypted home + the Stratum sub-chunk) / A-5c (RECOVER + hostowner-c) -- a focused round each. **No new spec** per the 2026-05-23 broadening -- prose in IDENTITY-DESIGN.md §9.9 + this row + the per-sub-chunk audits + the runtime + cross-reboot + login-E2E tests. |
| A-5c RECOVER recovery keyslot (corvus) | `usr/corvus/src/main.rs` (the new `VERB_RECOVER`=8 handler [`subject_kind` 0=system / 1=user]; the BIP-39 wordlist + phrase gen/decode/checksum; the per-user `recovery.corvus` mint at `handle_user_create` + the phrase returned in the OK response; the keypair re-wrap + rename-swap of `hybrid.corvus` / `system-wrap`; the fresh-phrase roll), `usr/login/` (the login recovery path + the create-time phrase surfacing); **A-5c-b host-bake (user-voted 2026-06-05)**: NEW `usr/lib/corvus-crypto/` (shared `no_std` crypto lib extracted from corvus -- the CRVS wrap layout + `argon2id_kek` + AEGIS wrap/unwrap + BIP-39 codec + keypair-gen, parameterized over an RNG), NEW `usr/corvus-mint/` (host-target minter, `OsRng`), `tools/build.sh` (build corvus-mint host-side + bake the `/var/lib/corvus` dir-chain + `system-wrap` / `system-recovery-wrap`, SYSTEM-owned), `usr/corvus/src/main.rs` (`system_identity_load` at boot + the real `handle_admin_elevate` argon2id+AEGIS unwrap of `system-wrap`, replacing the `b"thylacine"` byte-compare + RECOVER(`subject_kind=0`) console gate) | Convergence-detour A-5c (IDENTITY-DESIGN.md §9.9 design pass + CORVUS-DESIGN §5.5.1/§5.6/§6.4/§8/§9; user votes 2026-06-05: user-held-only [NO hostowner escrow] + mandatory enrollment + host-bake the system identity). **AEGIS/mallocng-adjacent recovery crypto -- prosecute hard.** A recovery keyslot = a 2nd wrap of the SAME hybrid keypair under a recovery-phrase KEK (`recovery.corvus` / `system-recovery-wrap`); it re-wraps the KEYPAIR not the DEK -> the Stratum DEK envelopes stay valid -> **NO Stratum / kernel surface**. Invariants: **C-28 no-escrow** (corvus stores no copy of a user's keypair/DEK recoverable by any authority other than that user's passphrase OR own phrase; the hostowner has NO user-data-recovery verb -> D3 mutually-encrypted-homes survives a malicious hostowner); **C-27** (per-user keyslot minted at USER_CREATE, displayed once, never persisted; same keypair as `hybrid.corvus`; domain-separated AD `"thylacine-corvus-recovery-v1"`); **C-20** generalized. Prosecute: the **gate** (RECOVER(user) = phrase + rate-limit ONLY, no session / no `CAP_HOSTOWNER`; RECOVER(system) additionally console-attached); **phrase brute-force** (the C-16 rate-limit is the online defense); **secret hygiene** (mlock + `explicit_bzero` of phrase, recovery_KEK, keypair, new_KEK on EVERY path incl. error); the **twin-wrap atomicity** (re-wrap `hybrid.corvus` AND roll `recovery.corvus`; a crash between must not strand the user un-recoverable nor leave the old phrase live; rename-swap, A-1.6); **no-DEK-rewrite correctness** (the recovered keypair must byte-equal the original so existing envelopes decapsulate); the **USER_CREATE OK-response growth** (callers parse the appended phrase; no overrun). **A-5c-b (host-bake) additionally**: the `corvus-crypto` extraction MUST preserve byte-identical wraps (the A-5c-a / A-1b / A-5b wrap + DEK-envelope paths behavior-unchanged); a host-minted `system-wrap` and the on-device unwrap must round-trip (same CRVS layout / AD / Argon2id -- no `corvus-mint` <-> corvus drift); the real `handle_admin_elevate` fails CLOSED on a missing / corrupt / wrong-passphrase `system-wrap`; RECOVER(`subject_kind=0`) requires console attachment; the bake lands the files SYSTEM-owned where corvus's chroot resolves them. **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.9 + CORVUS-DESIGN §5.5.1/§5.6 + this row + the audit + corvus unit tests + the boot E2E. |
| `SPAWN_PERM_MAY_POST_SERVICE` one-hop delegation (A-5b #827b) | `kernel/syscall.c` (the `SYS_SPAWN_*` perm-grant gate -- now per-bit: `CONSOLE_TRUSTED` requires console-attach; `MAY_POST_SERVICE` requires console-attach OR `proc_may_post_service(p)`), `kernel/include/thylacine/proc.h` (`proc_may_post_service`), `usr/joey/joey.c` (spawn login with `SPAWN_PERM_MAY_POST_SERVICE`), `usr/login/src/main.rs` + `usr/lib/libthyla-rs/src/process.rs` (`Command` gains a perm setter; login confers the bit on the per-user proxy) | A-5b #827b (user-voted 2026-06-04). **Privilege-gate change -- prosecute hard.** Lets a non-console-attached but trusted session authority (`/sbin/login`, spawned by the console-attached joey *with* the bit) confer `MAY_POST_SERVICE` on per-user `--role client` proxies. Prosecute: the per-bit separation (a `MAY_POST_SERVICE` holder can NEVER confer `SPAWN_PERM_CONSOLE_TRUSTED` -- the SAK anchor stays console-attach-only, I-27); the bit is never `rfork`-propagated (still a `perm_flags`-only decision, NOT a `cap_mask` bit -- I-2 unaffected); the delegation root is the console-attached joey (no path for an ordinary Proc to acquire the bit); soundness rests on *who holds the bit* (only joey's deliberately-conferred OS servers -- corvus, login, the proxy). **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.9 + this row + the #828 audit + the grant-gate regression test. |
| Pathname resolution (`stalk`) + namespace-resident `/srv` | the new resolver (`stalk` + `cross_mounts`/`domount` + the in-call `trail`; folds in / supersedes `sys_walk_open_handler`); `kernel/include/thylacine/territory.h` + `kernel/territory.c` (`PgrpMount` re-keyed from `path_id_t` to mount-point Spoor identity `dc`+`qid.path`; `mount`/`unmount` path-keyed; the size-pinned `Territory` static_asserts re-bumped); `kernel/syscall.c` (`SYS_OPEN(path)` multi-component handler; `SYS_MOUNT`/`UNMOUNT` path-keyed; retire `SYS_SRV_CONNECT` + `SYS_POST_SERVICE`); `kernel/devsrv.c` (per-territory service table; `devsrv_open` = connect; `devsrv_create` = post); migrate every `/srv` client + `usr/lib/pouch/patches/0006-pouch-sockets.patch` | Convergence-detour A-5b-0 (`docs/STALK-DESIGN.md`; user-voted full Plan-9 spine 2026-06-02). **Path resolution is a privilege boundary -- prosecute hard.** Invariant **I-28** (path-resolution containment + per-component X-search). Prosecute: `..` escape above `root_spoor`; per-component X-search bypass (symlink / `..` / mount-cross tricks); Spoor lifetime across N hops (UAF / double-clunk / leak on the `trail`); per-territory `/srv` isolation (a 2nd user cannot NAME another's coordinator); mount-cross into a tree the caller lacks X on; the connection-handle reconciliation (`SO_PEERCRED`, the attach-9p unification, the KObj_Srv->KOBJ_SPOOR endpoint shift); the per-Spoor `SrvRegistry` ref lifecycle (drain-on-last-unref; no UAF / double-free); tokenizer bounds. **stalk-3 sub-design resolved (STALK-DESIGN §5, D5/D6/D7): two-step open=connect, `DMSRVBYTE` create=post, full per-territory refcounted registry, 9P-unification (embedded `srvconn_client_*` retires), `SRV_CONN_PER_PROC_MAX` removed; split 3a/3b/3c.** One focused round per sub-chunk (stalk-1/2/3a/3b done; stalk-3c audit CLEAN 0/0/0/3 -- all P3 doc-staleness; stalk-3 ARC COMPLETE). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in `docs/STALK-DESIGN.md` + ARCH §9.6.7 + this row + the audit + the runtime tests. |

### 25.5 The audit round

Per CLAUDE.md (Phase 4 deliverable). Standard procedure:

1. Spawn a soundness-prosecutor agent (general-purpose subagent, `run_in_background: true`).
2. In the prompt, include `memory/audit_rN_closed_list.md` contents as the "already fixed — do not re-report" preamble.
3. Scope the prompt to the surface changed.
4. Tell the agent explicitly to prosecute, not defend.
5. Wait for the completion notification.
6. Trust but verify: validate quoted file:line references.
7. Fix every P0/P1/P2 finding before merge. P3 findings get tracked or closed with explicit justification.

### 25.6 Audit trigger refresh policy

When a new file is added that touches a load-bearing invariant, the audit-trigger table is updated in the same PR. The refresh is part of the chunk close, not deferred.

### 25.7 Open design questions

None at Gate 3.

### 25.8 Summary

Nine TLA+ specs gate-tied to phases. Spec-first methodology. Audit-trigger surface table updated continuously. Sanitizer matrix (ASan, UBSan, TSan) on every commit.

---

## 26. Open design questions (cross-cutting)

**STATUS**: COMMITTED (no open questions at Gate 3)

All section-local open questions are resolved at Gate 3. Cross-cutting open questions are tracked here as they emerge. Currently empty.

---

## 27. Status summary

| Section | Status | Priority |
|---|---|---|
| §1 How to read | COMMITTED | — |
| §2 System overview | COMMITTED | — |
| §3 Language | COMMITTED | — |
| §4 Target architecture | COMMITTED | 1 |
| §5 Boot sequence | COMMITTED | 2 |
| §6 Memory management | COMMITTED | 1 |
| §7 Process and thread model | COMMITTED | 2 |
| §8 Scheduler | COMMITTED | 1 |
| §9 Territory and device model | COMMITTED | 1 |
| §10 IPC | COMMITTED | 1 |
| §11 Syscall interface | COMMITTED | 2 |
| §12 Interrupt and exception handling | COMMITTED | 2 |
| §13 VirtIO | COMMITTED | 2 |
| §14 Filesystem integration | COMMITTED | 2 |
| §15 Security model | COMMITTED | 1 |
| §16 POSIX compat | COMMITTED | 3 |
| §17 Halcyon integration | COMMITTED | 3 |
| §18 Kernel object handles | COMMITTED | 1 |
| §19 VMOs | COMMITTED | 2 |
| §20 Per-core SMP | COMMITTED | 2 |
| §21 Async 9P | COMMITTED | 2 |
| §22 Hardware platform | COMMITTED | 1 |
| §23 POSIX surfaces + Utopia | COMMITTED | 2 |
| §24 Hardening | COMMITTED | 1 |
| §25 Verification cadence | COMMITTED | 1 |
| §26 Open design questions | COMMITTED | — |
| §28 Invariants enumerated | COMMITTED | 1 |

---

## 28. Invariants enumerated

**STATUS**: COMMITTED

The complete list of load-bearing invariants. Source for `VISION.md §8`. Each maps to enforcement: TLA+ spec, runtime check, compile-time assertion, or architectural discipline.

| # | Invariant | Enforcement | Spec |
|---|---|---|---|
| I-1 | Territory operations in process A don't affect process B | Kernel territory isolation | `territory.tla` |
| I-2 | Fork-grantable capability set monotonically reduces (`rfork` only reduces). Elevation-only capabilities (`CAP_HOSTOWNER`) are the sole sanctioned growth — conferred only via the `cap` device for a console-attached Proc, never by `rfork` (CORVUS-DESIGN.md §5.5.1 / C-21) | Syscall gate; `cap` device redemption | `handles.tla` |
| I-3 | Mount points form a DAG, never a cycle | Kernel mount validation | `territory.tla` |
| I-4 | Handles transfer between processes only via 9P sessions | Syscall surface (no direct-transfer syscall exists) | `handles.tla` |
| I-5 | `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` cannot be transferred | Transfer syscall has no code path; static_assert | `handles.tla` |
| I-6 | Handle rights monotonically reduce on transfer | Syscall-level check | `handles.tla` |
| I-7 | BURROW pages live until last handle closed AND last mapping unmapped | Refcount; runtime check | `burrow.tla` |
| I-8 | Every runnable thread eventually runs | EEVDF deadline computation | `scheduler.tla` |
| I-9 | No wakeup is lost between wait-condition check and sleep — *including* the death-wake that group-termination delivers to every rendez sleeper (§8.8.1, task #811) | Wait/wake protocol; register-then-observe under the per-condition lock (`torpor_lock` for `torpor`, the per-Thread `wait_lock` for `sleep` / `tsleep`) | `scheduler.tla`, `poll.tla`, `futex.tla` (+ prose/audit for the death-wake generalization per the spec-to-code suspension) |
| I-10 | Per-9P-session tag uniqueness | Per-session tag pool with monotonic generation | `9p_client.tla` |
| I-11 | Per-9P-session fid identity is stable for fid's open lifetime | Per-session fid table | `9p_client.tla` |
| I-12 | W^X: every page is writable XOR executable | PTE bit check + mprotect rejection + ELF loader rejection | runtime + `_Static_assert` |
| I-13 | Kernel-userspace isolation: TTBR0 / TTBR1 split | Page table setup | runtime |
| I-14 | Storage integrity: every block from Stratum is integrity-verified | Stratum's responsibility (Merkle layer); OS observes via 9P | (Stratum-side spec) |
| I-15 | Hardware view derives entirely from DTB | No compile-time hardware constants outside `arch/arm64/<platform>/` | code review + audit |
| I-16 | KASLR randomizes kernel image base at boot | Boot init randomizes TTBR1 base | runtime + `/ctl/kernel/base` audit |
| I-17 | EEVDF latency bound: delay between runnable and running ≤ slice_size × N | EEVDF deadline math | `scheduler.tla` |
| I-18 | IPIs from CPU A to CPU B are processed in send order | GIC SGI ordering | `scheduler.tla` |
| I-19 | Note delivery preserves causal order within a process; every posted non-`kill` note is consumed exactly once across the handler + fd-read paths; `kill` is non-catchable | Per-Proc `NoteQueue` under lock; EL0-return-tail dispatch; `devnotes` Dev. Sub-invariants N-1..N-5 enumerated in §7.6.7 | (`notes.tla` planned then dropped per CLAUDE.md spec-to-code suspension; prose + audit + tests) |
| I-20 | PTY master ↔ slave atomicity | PTY data path locked | `pty.tla` |
| I-21 | Kernel executes uniformly at EL1h (`SPSel=1`); `SP_EL0` is exclusively the userspace stack | Boot sets `SPSel=1` and never lowers it; per-thread kernel stack carries exception frames; `test_smp` asserts `SPSel==1` | `sched_ctxsw.tla` |
| I-22 | No identity carries ambient super-authority: there is no superuser identity; `hostowner` is an authority *source* (the system key), not an identity; elevated power is acquired only via the legate (scoped, audited, ephemeral) | Syscall surface grants no authority by identity alone; capabilities are explicit (grant or legate activation) via the `cap` device + clearance-level policy | `IDENTITY-DESIGN.md §3.3/§8.2` (prose + audit + tests per the spec-to-code suspension) |
| I-23 | A service's filesystem authority is bounded by the storage capability it is handed: it reaches only that subtree, at only the handle's rights R, with no ambient FS authority beyond it; authority is monotonic (any delegate is `<=` R + same subtree). `RIGHT_TRANSFER` is withheld at grant as least-authority hardening (reserved for the Phase-5+ 9P-transfer surface; does NOT gate spawn-endow at v1.0, so intra-grant delegation is possible + sound). Confinement at v1.0 is cooperative (the service chroots to the capability as its first action; spawner-set-root is the v1.x form). | Spawner endows a `handle_dup`-reduced (`R|W`) Spoor at spawn; service chroots to it + confines all FS ops to it; a composition of I-2/I-4/I-6 (handles) + I-1/I-3 (territory) | `ARCHITECTURE.md §3.6` + `docs/detour-status.md A-1.7` (prose + audit + tests per the spec-to-code suspension) |
| I-24 | Group termination is atomic + exactly-once: when a Proc group-terminates (`exit_group` / `kill` / [v1.x: fault]), every Thread of the Proc eventually reaches `THREAD_EXITING` and the Proc transitions to ZOMBIE **exactly once** with the group exit status; no Thread executes at EL0 after its Proc's ZOMBIE transition; the cross-thread shootdown loses no wakeup (the sleeper-wake reuses the I-9 register-then-observe discipline under the per-condition lock — `torpor_lock` for `torpor`, the per-Thread `wait_lock` for all other rendez sleeps (§8.8.1, task #811); lock order `g_proc_table_lock → wait_lock → r->lock`) | Single per-Proc set-once `group_exit_msg` (NULL-sentinel CAS — *is* both the die-flag and the last-out status); `proc_group_terminate` publishes it, then `torpor_wake_all_for_proc` + the universal death-interruptible-sleep wake (§8.8.1, task #811) + broadcast `smp_resched_others`; `el0_return_die_check` at every EL0-return tail (sync-from-EL0 + the IRQ-from-EL0 tail, `#713`-safe); last-Thread-out ZOMBIE reap; `wait_pid` multi-Thread reap (`on_cpu`-spin per #788) | `ARCHITECTURE.md §7.9.1` (prose + audit + tests per the spec-to-code suspension) |
| I-25 | A legate's elevated authority is bounded to its scope subtree and fully revoked on scope exit: clearance caps stamp on the legate root, attenuate to children by I-2, and are revoked (the `legate_scope_id` subtree torn down via group-terminate) on the legate root's exit OR `valid_until` expiry; no elevated Proc outlives the scope; the durable `principal_id` is unchanged by elevation (the legate is the same human, more authority) | `legate_scope_id` (rfork-inherited subtree tag) + `legate_valid_until` + `legate_session_id` on `struct Proc`; the `cap` device clearance grant/redeem (corvus registers via `CAP_GRANT_CLEARANCE`, the Proc redeems its own grant, kernel stamps `caps |= clearance ∩ self_restriction`); scope teardown reuses `proc_group_terminate` (#809/#811) at the EL0-return tail | `IDENTITY-DESIGN.md §3.1/§9.8` (prose + audit + tests per the spec-to-code suspension) |
| I-26 | Cross-process control is explicit + two-axis: a `kill`/`killgrp` write to `/proc/<pid>/ctl` is authorized only by owner-rwx on the ctl file (identity axis) OR `CAP_HOSTOWNER` OR `CAP_KILL` (capability axis) -- no identity carries ambient kill authority (composes I-22); containment is namespace visibility (I-1) | `devproc.stat_native` reports the target's `principal_id`/`primary_gid`/`0600`; the two-axis check (owner -- same `principal_id` on the `0600` ctl -- OR `caller` holds `CAP_HOSTOWNER` OR `CAP_KILL`; computed DIRECTLY in `devproc_write`, NOT via the `perm_check` DAC-override, so `CAP_DAC_OVERRIDE` is deliberately NOT a kill axis -- the A-4 split keeps fs-admin orthogonal to process-kill) runs at the WRITE site (`perm_enforced = false`; the SHARED open chokepoint hard-rejects pre-`devproc.open`, so the gate-at-open sketch could not host the `CAP_KILL` axis -- reconciled 2026-06-01, user vote); kill dispatches via `proc_group_terminate` uniformly under `g_proc_table_lock` via the `proc_for_each` resolve+authorize idiom (the `#811` wake-total primitive) | `IDENTITY-DESIGN.md §9.8` + `ARCHITECTURE.md §7.6` (prose + audit + tests per the spec-to-code suspension) |
| I-27 | Trusted path -- the elevation prompt is unspoofable: after a kernel SAK, the console-attach bit is held only by corvus / the trusted login Proc, and only the console-attach holder may redeem `CAP_HOSTOWNER` / a high-stakes clearance, so an interposer that drew a fake prompt cannot complete an elevation; the SAK keystroke is recognized in the kernel UART RX path before EL0 delivery | SAK = serial BREAK (a line condition, not data -> unforgeable by EL0 bytes; stateless recognizer, so "cannot be starved/spoofed by crafted input" is structural); recognized in the RX IRQ handler pre-EL0; the IRQ handler is wakeup-only (`notes_post`/`poll_waiter_list_wake` are not IRQ-safe), deferring the privileged action to a `console_mgr` kproc kthread (process context); `g_console_owner` (`struct Proc *`) under `g_proc_table_lock`, init to joey, cleared by `exits()` on owner-exit; `proc_revoke_console_attached` clears `PROC_FLAG_CONSOLE_ATTACHED` atomically; SAK re-grants to corvus via `g_console_trusted_proc`, FAIL-SAFE revoke-only (`g_console_owner=NULL`) if no trusted proc is alive; the `cap` device redeem gate keys on `PROC_FLAG_CONSOLE_ATTACHED` (devcap.c) | `IDENTITY-DESIGN.md §9.8` + `ARCHITECTURE.md §17.1` (prose + audit + tests per the spec-to-code suspension) |
| I-28 | Pathname resolution is contained + identity-checked: `stalk` resolves a path component-by-component from the caller's Territory `root_spoor`; `..` can never resolve above that root (the chroot/pivot boundary); every directory hop is gated by a per-component X-search (`perm_check(p, &st, PERM_X)`) on a `perm_enforced` Dev (final hop = `perm_want_for_omode`); mount-crossing (`domount`) is keyed by the full mount-point Spoor identity `(dc, devno, qid.path)` and reads only the caller's per-Territory mount table (composes I-1); every intermediate Spoor on the resolution **trail** is released except the returned **quarry** (no UAF / no leak) | `stalk` + `cross_mounts` (the in-call `trail` for `..` containment + cleanup, bounded at `root_spoor`); `PgrpMount` re-keyed to the `(dc, devno, qid.path)` mount-point identity (the `devno` axis distinguishes concurrent dev9p sessions); per-component `perm_check` / `spoor_stat_native` (the single-hop walk-open generalized to N hops); one-component-per-`Dev.walk` at v1.0 | `ARCHITECTURE.md §9.6.7` + `docs/STALK-DESIGN.md` (prose + audit + tests per the spec-to-code suspension) |

These are the project's promises. Every one has a spec or a runtime check or a compile-time assertion. None are policy-only.

**Corvus invariants (C-1..C-23)** are enumerated separately in `CORVUS-DESIGN.md §9` — they govern the key agent's runtime + audit guarantees (mlock'd pages, session ownership monotonicity, audit log encryption, the kernel-stamped per-connection `/srv` transport identity, etc.). Cross-referenced here so the global invariant surface is discoverable; the canonical text lives in CORVUS-DESIGN.

**Pouch invariants (P-1..P-4)** are enumerated separately in `POUCH-DESIGN.md §11` — they govern the Phase-6 POSIX environment's boundary guarantees: **P-1** no foreign syscall number ever enters the kernel (the structural form of `ROADMAP.md §3.6` — the kernel is never modified to accommodate POSIX); **P-2** pouch is the sole POSIX path to the kernel; **P-3** no silently-wrong POSIX surface (every surface either maps to a defined Thylacine behavior or returns a documented errno); **P-4** the pouch upper/lower boundary line holds (vendored musl above, Thylacine-native below). Cross-referenced here so the global invariant surface is discoverable; the canonical text lives in POUCH-DESIGN.

---

## 29. Glossary

| Term | Definition |
|---|---|
| 9P | Plan 9 protocol — universal file-server protocol; Thylacine speaks 9P2000.L |
| ASLR | Address Space Layout Randomization (userspace) |
| BTI | Branch Target Identification (ARMv8.5+ hardware feature) |
| Spoor | Kernel object representing an open resource |
| CFI | Control Flow Integrity (compiler-level mitigation) |
| `Dev` | Kernel device vtable |
| DTB | Device Tree Blob — describes hardware to the kernel |
| EEVDF | Earliest Eligible Virtual Deadline First scheduler |
| EBR | Epoch-Based Reclamation (used in `burrow.tla`) |
| EL0/EL1/EL2/EL3 | ARM64 Exception Levels |
| `errstr` | Plan 9's per-thread error string |
| GIC | Generic Interrupt Controller (ARM) |
| Halcyon | Thylacine's graphical scroll-buffer shell |
| Handle | Typed unforgeable token referencing a kernel object |
| Hostowner | Thylacine's admin-authority model (no UID 0). See CORVUS-DESIGN.md §3 D5. |
| IPI | Inter-Processor Interrupt |
| janus | Stratum's key agent; design influence on corvus but not ported. See CORVUS-DESIGN.md §2. |
| corvus | Thylacine's key agent (v1.0); userspace daemon serving `/srv/corvus/`. See CORVUS-DESIGN.md. |
| KASLR | Kernel ASLR |
| KObj | Kernel Object (e.g., `KObj_MMIO`, `KObj_Burrow`) |
| LSE | Large System Extensions (ARMv8.1+ atomic instructions) |
| MTE | Memory Tagging Extension (ARMv8.5+) |
| Territory | Per-process tree of mount points |
| Note | Plan 9's asynchronous text message (signal-equivalent) |
| PAC | Pointer Authentication Code (ARMv8.3+) |
| `Territory` | Process Group / territory |
| Proc | Process |
| `rfork` | Plan 9 process/thread creation primitive |
| SLUB | Linux's modern slab allocator (replaces classical slab) |
| Stratum | Thylacine's native filesystem (independent project; feature-complete) |
| TLA+ | Temporal Logic of Actions, Lamport's specification language |
| Utopia | The Phase 5 milestone — textual POSIX environment that "feels real" |
| BURROW | Virtual Memory Object (kernel object for shared memory regions) |
| W^X | Write XOR Execute (page protection invariant) |

---

## 30. Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Initial draft (Phase 0). | Ground-up rewrite from `tlcprimer/ARCHITECTURE.md` after Phase 0 challenge round. STUBs/DRAFTs promoted to COMMITTED. C21-C27 commitments added (LSE, MTE, BTI, CFI, KASLR, ASLR, W^X, hardened malloc). Userspace drivers from Phase 3 (no in-kernel virtio-blk shortcut). Typed handle subordination. EEVDF scheduler. Three v2.0 design contracts (factotum capability mediation §15.4, multikernel §20.6, in-kernel Stratum driver §14.4). Nine TLA+ specs gate-tied. Audit-trigger surface table. Utopia milestone in §23. Twenty load-bearing invariants enumerated in §28. |
| 2026-05-04 | Halcyon-as-last-phase reorder. | User direction: practical working OS comes first; Halcyon at the end. ROADMAP phases 6/7/8 reordered: Phase 6 → Linux compat + network (was 7); Phase 7 → Hardening + v1.0-rc (was 8); Phase 8 → Halcyon + v1.0 final (was 6). ARCH §17 layer-cake updated. ARCH §8.1 SMP stress moved from Phase 8 to Phase 7 (the v1.0-rc release point). ARCH §16.5 container runner moved from Phase 7 to Phase 6. ARCH §23 POSIX surfaces references updated (Linux compat at Phase 6, Halcyon at Phase 8). |
