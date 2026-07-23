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
| Wakeup | No wakeup lost between wait-condition check and sleep | Wait/wake state machine | `scheduler.tla`, `poll.tla`, `cons_poll.tla`, `tsleep.tla`, `death_wake.tla` (torpor leg prose-validated) |
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
| PL031 RTC | ARM system | Wall-clock epoch (read once at boot to anchor `CLOCK_REALTIME`; §22.6) |
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

### 6.2.1 ASID management: generation-rollover

**Decision** (RW-1 B-F1, user-voted 2026-06-10): user ASIDs are a recycled *cache* keyed by a global generation counter — the Linux arm64 rolling-ASID model — not a per-Proc permanent allocation. The prior design assigned one ASID per Proc for its lifetime and **extincted the kernel** on the 256th concurrent Proc (8-bit ASID space, no rollover) — an unprivileged whole-system DoS (RW-1 B-F1, graded P1). The fail-soft interim (`ef29456`) made exhaustion fail the spawn with `-errno`; this redesign removes exhaustion entirely, lifting the hard concurrent-process ceiling.

**Model.** A global `asid_generation` (the high bits) + the hardware ASID (the low `ASID_BITS`) form a per-Proc `context_id` (atomic `u64`; a Proc starts at generation 0 = "never assigned", which always misses). At each context switch the kernel resolves the running Proc's *current* ASID:

- **Fast path (lockless).** If the Proc's stored generation `==` the global `asid_generation`, its ASID is still valid: an `xchg` publishes it into this CPU's `active_asids[cpu]` (so a concurrent rollover on another CPU reserves it) and the switch proceeds. This is the register-then-observe discipline (I-9 family) specialized to the ASID surface.
- **Slow path (`new_context`, under `g_asid_lock`).** Generation stale: claim a free ASID from the bitmap. If none is free in the current generation, **roll over**: bump `asid_generation`, reset the bitmap, set `flush_pending` for every CPU, and preserve each CPU's currently-active ASID into `reserved_asids[cpu]` (so a running CPU is never yanked). The Proc gets a freshly-reserved ASID; its `context_id` is restamped.
- **flush_pending.** After a rollover each CPU does a *local* `tlbi vmalle1` on its `flush_pending` bit at its next switch — the broadcast `...is` flush covers only entries present at flush time, so the per-CPU local flush clears anything the CPU speculated in the rollover window.

**Hardware ASID width.** `TCR_EL1.AS` is set to 1 (16-bit ASIDs) when `ID_AA64MMFR0_EL1.ASIDBits == 0b0010`, else left 0 (8-bit). The rolling model is correct at either width; 16-bit makes rollovers rare (the performance win folded in from the B-F1 fork's "16-bit ASIDs" option — Linux uses the hardware width with the same rolling allocator).

**Safety invariant (I-31).** No two CPUs concurrently run user address spaces sharing an ASID in the same generation — else the TLB returns a wrong translation and one Proc reads/writes another's memory. The rollover race (a generation rollover concurrent with another CPU's context switch) is the classic, subtle hazard, so this surface is **model-first**: `specs/asid.tla` (with a `rollover_steals_active` buggy cfg) is written and TLC-green BEFORE the impl (spec-first re-enabled for this surface per the SMP-race precedent in ARCH §8.4).

**Thylacine wiring.** `struct Proc` carries `_Atomic u64 context_id` (replacing the permanent `u16 asid`); the context-switch path gains a C pre-hook `asid_check_and_switch(next, cpu)` that resolves the current ASID and refreshes `next->ctx.ttbr0` (ASID in bits [63:48]) before the asm `cpu_switch_context` runs. `proc_alloc` no longer allocates an ASID at creation and `proc_free` no longer frees one at reap (rollover reclaims; there is no per-Proc free). kproc keeps the global ASID-0 kernel TTBR0 (never rolls; the hook is gated on `pgtable_root != 0`). The all-ASID unmap/install TLBI paths (`mmu_install_user_pte` — `(void)asid` — and `burrow_unmap` via `tlbi vaae1is`, VA + *all*-ASID) are unaffected; the `asid` parameter there is vestigial. Working design note: `memory/project_asid_rollover_design.md`.

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
- Per-CPU "active slab" pointer; allocations come from this slab without lock. **DEFERRED (v1.x)** — as-built every `kmem_cache_alloc`/`free` takes the per-cache global `c->lock` (the per-CPU lockless fast path, SLUB's defining mechanism, is not yet built; HT11.R1-F7 + HT01.A-H/T. The page-allocator layer DOES have per-CPU magazines for orders 0/9).
- Free objects within a slab are tracked via a free-list embedded in the unused object memory (zero overhead per free object).
- When the active slab is full, fetch a new partially-full or empty slab from the per-class partial list.
- When a slab is fully free for some time, return its page(s) to the buddy allocator.
- Per-class debug mode: red zones, poison patterns, allocation/free trace. Compiled out in release builds; on in `slub_debug=1` boot cmdline. **DEFERRED (v1.x)** — no debug mode exists as-built (HT01.A-H/T).

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

**User address space management** (`kernel/vma.c` + `kernel/burrow.c` + `arch/arm64/mmu.c`):

Each `Proc` carries a page table tree (4-level ARM64). Mappings are tracked in a per-process sorted VMA list (`struct vma` describing each mapped region; the rb/maple-tree upgrade is a registered perf item, RW-1 C-F7).

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
- Demand-zero-pages anonymous mappings on first touch — **BUILT as the overcommit model (2026-06-23): a dedicated `SYS_BURROW_ATTACH_LAZY` syscall reserves the VA + a sparse `BURROW_TYPE_ANON_LAZY` Burrow and demand-zero-fills + charges each page on first touch; `SYS_BURROW_DECOMMIT` releases pages back. See "The overcommit model" below.** The eager `burrow_create_anon` backing stays for kernel-internal copy-target callers (exec data segments, DMA buffers, the Weft rings) that need pages present at create; the unconditional migration of every `SYS_BURROW_ATTACH` to demand-zero is a recorded follow-up.
- COW on first write to a shared page is a **designed seam, not yet built** — no caller exists (`rfork` `RFMEM` extincts, so no address space is shared at v1.0). Activates when a `fork`-style sharing caller lands. (IDENTITY-DESIGN.md §8.4 SEAM.)
- Translates BURROW-backed faults to BURROW page lookups.
- Panics on kernel faults outside the direct map / vmalloc / fixmap regions (a kernel page fault is a bug, not a recoverable condition).

**User memory growth: the two-tier native interface**

A Proc's address space is established at `exec_setup` (segments + stack) and grown at runtime through a deliberately small native interface. Plan 9 conflated "the heap" (`sbrk`) and "a memory region as a thing" (`segattach`) into segments; Thylacine separates them, because they are different kinds of object.

- **Tier 1 — anonymous regions.** `burrow_attach(length) → vaddr` attaches an anonymous, RW, demand-zero Burrow into the caller's address space at a kernel-chosen address; `burrow_detach(vaddr, length)` releases it (exact-match — no partial detach at v1.0). No handle: private heap memory is not a named, shared, or transferable thing — its owner is the VMA, its name is its address (as Plan 9's `sbrk` / `segattach` returned raw addresses). This is the substrate for both `libt`'s and pouch's `malloc`. It is `segattach` without `brk`'s single-cursor rigidity: many independent regions, individually releasable, kernel-placed (no `MAP_FIXED`). Lifecycle: the audited `burrow_create_anon → burrow_map → burrow_unref` sequence (§19; invariant I-7) — the VMA's `mapping_count` holds the Burrow alive, `handle_count` is 0; `burrow_detach` drops the last `mapping_count` and the pages are freed. VMA-list mutation is serialized under a per-Proc lock (uncontended at v1.0's single-threaded Procs; correct-by-construction when threads land).

- **Tier 2 — Burrow handles.** A Burrow created as a first-class object with a `KObj_Burrow` handle (§11.3) — for *shared*, *named*, or *transferable* memory: handed to a child for shared memory, or sent over a 9P / `/srv` connection. This is the part that goes beyond Plan 9, whose segments could not be passed around. Design intent; lands when a native workload needs it.

`brk` is **not provided** — a single process-global linear cursor is ASLR-hostile, thread-hostile, and cannot host multiple arenas; every modern allocator is arena-based (musl's own `mallocng` treats `brk` as a removable optimization). `mprotect` is **not provided** at v1.0 — Tier 1 is RW, and the only planned W↔X transition is the JIT-future `pkey`-shaped path (§6.6).

**The overcommit model (demand-zero anonymous memory).** A dedicated `SYS_BURROW_ATTACH_LAZY` syscall (the eager `SYS_BURROW_ATTACH` stays a separate 1-arg syscall, byte-identical — the Plan 9 small-syscall idiom that already keeps `ATTACH`/`DETACH` distinct; user-voted 2026-06-23 over a flags-on-`SYS_BURROW_ATTACH` arg, for blast-radius discipline and to keep the eager path byte-unchanged) reserves a VA range + a VMA + a sparse `BURROW_TYPE_ANON_LAZY` Burrow but commits **no physical pages**; each page is allocated, zero-filled, and installed RW/XN on its first touch (a `userland_demand_page` arm that mirrors the REVENANT `BURROW_TYPE_FILE` arm but is simpler — pure zero-fill, no backing read, so it runs entirely under `vma_lock` with no slow path; the two-CPUs-fault-one-slot race resolves install-once, loser frees). `SYS_BURROW_DECOMMIT(vaddr, length)` is the `madvise(MADV_DONTNEED)` analog: it clears the PTE (+ TLBI), frees the page, and NULLs the sparse slot, keeping the VMA — a later touch re-faults a fresh zero page. This is the **Linux overcommit contract** that the entire native-toolchain ecosystem assumes — reserve address space cheaply, commit on first touch, RSS = what was actually touched, release on `madvise` — and it is *required*, not optional, for the goals: pouch (Linux/musl binary compat) and a future on-device LLVM/clang (§29 #67) both depend on `mmap`-large-reserve / commit-sparse / `madvise`-release. The capability-microkernel precedent is **Fuchsia's VMOs** (lazy commit + `zx_vmo_op_range(DECOMMIT)`); seL4's explicit-frame, no-overcommit model is the deterministic-but-incompatible alternative, declined because Thylacine *runs arbitrary Linux binaries* (via pouch) where seL4 does not. The contract reaches every program through the two default malloc substrates — libthyla-rs's `sysAlloc` and the pouch boundary-line `mmap` — both calling `SYS_BURROW_ATTACH_LAZY`, so a reserve-heavy ported binary (jemalloc, a JVM, clang) gets overcommit with no internal change. The Go runtime is the worked example: `sysReserve` = `SYS_BURROW_ATTACH_LAZY`, `sysUnused` = `SYS_BURROW_DECOMMIT` (the GC finally shrinks RSS), and Go runs its **stock** `heapAddrBits=48` config — the proof that the overcommit model lets a toolchain use its native settings with no per-program tuning.

**Resource bound (I-32 fourth axis).** Lazy attach charges the I-32 `page_count` **at fault time, per page** (not at attach — the whole point is a free reservation), so `page_count` tracks true committed RSS; an over-cap fault on a non-TCB Proc fails the fault → `proc_fault_terminate` (the graceful-OOM backstop, never a box extinction). Decommit uncharges. But a *free* reservation reopens a DoS the eager path closed by construction (eager self-limits at ~`PROC_PAGE_MAX` VMAs since each charges ≥1 page): a hostile Proc could spam tiny lazy reservations to exhaust the VMA slab. So I-32 gains a fourth axis — a per-Proc **`PROC_VMA_MAX`** cap on live VMAs (the Linux `vm.max_map_count` analog), charged at `vma_insert` / uncharged at `vma_remove`, TCB-exempt like the others. It is a no-op for the eager path (already transitively bounded) and the real bound for lazy. This is an **audit-trigger surface** (§25.4): the fault path / W^X / charge-on-fault correctness + graceful OOM / the SMP install-once race / decommit lifetime (TLBI-before-free, no UAF) / the VMA-cap bound are prosecuted in a focused round; no new TLA+ spec (prose + audit, exactly as the near-identical REVENANT FILE arm was handled).

**Userspace file-backed `mmap` is refused — deliberately, permanently.** A userspace *writable* file mapping cannot be made 9P-network-transparent at acceptable cost: multi-writer coherence over a network FS is expensive (Mach's XMM was abandoned for exactly this), a network-backed mapping's failure surface is `SIGBUS`-on-a-load or an un-cancellable wedged fault rather than a checkable `-EIO`, and a raw file mapping is a capability trust hazard (the real Hurd "no read-only mappings" CVE, where one shared pager cache became a cross-client corruption channel). This is Plan 9's deliberate refusal, kept as a Thylacine conviction — "the filesystem is the OS" depends on userspace working with files via read and write, not a writable mapping. The substitute for a large file is read/write against the 9P-served FS. pouch surfaces file-backed `mmap(fd)` as a documented `ENOSYS` (POUCH-DESIGN.md §8.2).

**The refusal is precisely about the userspace *writable* mapping — NOT about kernel-internal read-only file paging.** When the kernel loads a binary it demand-pages the executable's read-only text from the FS, page by page, on fault (REVENANT / `docs/EXEC-LOAD-DESIGN.md`; invariant I-36) — exactly the benign case Plan 9 itself relies on (the qid-keyed `Image` cache). The Mach/Hurd external-pager evidence shows the "there is *no* coherent way to demand-page a network-served file" objection is an *absolute* that does not hold (Hurd's `nfs` translator demand-pages remote files today); its defensible residue (multi-writer cost, the `SIGBUS`/wedge failure surface, the raw-mapping trust hazard) is **dissolved** for the read-only, integrity-verified, **immutable-snapshot** case that Stratum's content-addressed FS provides. So: kernel-internal, read-only, capability-mediated, **death-interruptible** demand-paged exec from an immutable snapshot is sound and sanctioned (the reserved `BURROW_TYPE_FILE` Burrow type); a userspace *writable* file-mapping syscall is not, and never ships.

The `struct vma` sketch above predates the Burrow subsystem: the as-built VMA is the sorted, Burrow-backed list in `kernel/vma.c` (see `docs/reference/`); its `BURROW_TYPE_FILE` variant (read-only exec text, demand-paged + shared via the `Image` cache) is the REVENANT arc — post-net, pre-Imperium; design in `docs/EXEC-LOAD-DESIGN.md`, soundness in I-36.

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
- Memory cgroups-equivalent for territory-level / per-user memory accounting. Plan 9 doesn't have this; Linux does (`memory.max`). v1.0 ships the **per-Proc resource floor** (#65, invariant I-32): a non-TCB Proc's anon pages / threads / direct children are each capped (`PROC_PAGE_MAX` / `PROC_THREAD_MAX` / `PROC_CHILD_MAX`), with the live counters exposed in the per-Proc stat surface (`/proc/<pid>`, `/ctl`). The cgroups-equivalent **aggregate** quota (per-user / per-territory `memory.max`, summing those counters) is the recorded SEAM — it reads the per-Proc counters the floor already maintains. (Earlier drafts named a `/ctl/mm/` node for this; no such node exists — the real v1.0 surface is the per-Proc stat above. RW-12 W5-F8 reconciliation.)

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

**As-built (v1.0; the 2A-F6 / RW-11 SA-1b reconcile).** The shipped scheduler is
a *simplified* EEVDF: each thread carries a single monotonic `vd_t` minted from a
per-CPU yield counter (FIFO-within-band, reinserted at "current virtual time" on
wake -- RW-2 2A-F1), NOT the weighted `ve_t`/`vd_t` math above. The full weighted
form -- and with it the I-17 *quantitative* latency bound -- is a deferred design
target (the EEVDF lift). What v1.0 DOES guarantee:
- **Fixed-priority bands enforced on the wake path (RW-11 SA-1b).** `pick_next`
  already serves the highest-priority band first (8.3), but a *newly-runnable*
  higher-band thread used to wait up to a full slice for the next tick-driven
  preempt -- the empirically-pinned 6 ms "slice cliff". `sched_wake_preempts`
  closes it: a same-CPU wake sets `need_resched` when the wakee outranks the
  current thread (a CPU running its idle yields to any real wake; a strictly-
  higher band always preempts; same band stays EEVDF-fair). It is consumed at the
  next preempt point -- `preempt_check_irq` after every IRQ + the timer tick.
  (A *syscall-return-tail* preempt point shipped in RW-11 SA-1b but was REMOVED in
  #104: a `preempt_check_irq` from inside the C exception handler let a RUNNING
  thread be preempted -> RUNNABLE -> stolen + resumed mid-handler on a peer CPU,
  which reliably leaked a per-CPU run-queue lock under SMP -- `sched()` spun
  forever on QEMU TCG `-smp 4` through stratumd's mount IRQ-wait. The vector-level
  IRQ-return preempt -- a *clean* saved frame -- does the same yield without the
  hazard, so the wake-set `need_resched` is consumed there + at the tick, <=1 ms.
  A safe syscall-return preempt + the exact handoff-race root cause are owed.) The
  cross-CPU analog (`need_resched` on the target + a kick) is the #866 F1 half.
- **Cross-band starvation is NOT bounded** (see 8.3): bands are strict fixed
  priority with no cross-band aging at v1.0, so a CPU-bound INTERACTIVE thread
  starves NORMAL on its CPU. The realized INTERACTIVE set is deliberately narrow
  + mostly-blocked (8.3); the general CPU-DoS bound is the per-Proc quota (#65),
  and cross-band aging is the EEVDF-lift fairness extension.

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

**Realized at v1.0 (RW-11 SA-1b).** The INTERACTIVE band is no longer only
designed: a USER thread that blocks waiting for a device IRQ (`kobj_irq_wait`) or
for console input (`devcons_read`) is promoted to INTERACTIVE
(`sched_mark_interactive`), so its wake preempts NORMAL work (8.2 "as-built").
The promotion is sticky + one-way and gated to user threads (a kernel thread --
notably the in-kernel test runner that drives both wait paths synchronously --
stays NORMAL, so the boost never pollutes kernel scheduling). Each leg further
enforces a TRUST gate so the realized set stays narrow: the IRQ leg is implicitly
`CAP_HW_CREATE`-gated (reaching `kobj_irq_wait` requires an IRQ kobj), and the
console leg gates on the trusted console session -- the OWNER shell + the
console-ATTACHED login/corvus -- NOT an arbitrary foreground program that inherits
`/dev/cons` as stdin (which, since `/dev/cons` has no per-open capability gate and
PTY is unbuilt, would otherwise let unprivileged EL0 code self-promote above
NORMAL and starve it; the wake-preemption audit F1). The full "anything
from a controlling tty" rule + a dynamic boost-on-wake / demote-on-quantum
classifier (Plan 9) are the EEVDF-lift refinement; the controlling-tty inference
lands with the PTY work (Phase 8). The **no-cross-band-aging** caveat above is
now load-bearing: a CPU-bound INTERACTIVE thread starves NORMAL on its CPU
(bounded by the deliberately-narrow, mostly-blocked realized set + the #65 quota).

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
  from a peer (round-robin-from-rotating-start; steal frequency is bounded only
  by the `sched()` invocation rate — there is no explicit rate limit; the steal
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

### 8.6 Tickless idle (NO_HZ_IDLE)

**Status: DESIGNED + BUILDING (the TI arc; `docs/TICKLESS-IDLE.md`).** This section's original "idle CPUs disarm the timer" text was aspirational and never landed in the impl: the as-built kernel ran a 1 kHz periodic tick on *every* CPU and never stopped it at idle. The #299 measurement made the cost concrete — under HVF an idle CPU's never-stopped tick burns ~332% host CPU (per-tick VTIMER exit + emulated-GIC MMIO vmexits + a WFI that never parks at a 1 ms period ≈ the host wake latency); TCG idles at 21.5%, and cutting the tick 4× drops HVF idle 64× (→ 5.2%). The fix is what every production OS does (Linux NO_HZ_IDLE, FreeBSD dynticks; seL4/Zircon are deadline-tickless by design).

**NO_HZ_IDLE, not full tickless.** The periodic 1 kHz tick is kept unchanged for any *running* thread (slice accounting — so I-17, the §8.2 slice model, and the tick-coupled kernel tests are byte-identical). It is stopped ONLY on a CPU that has selected its idle thread (nothing runnable).

When a CPU goes idle it arms a **one-shot** instead of the periodic reload:
- target = `min(earliest g_timerwait deadline, now + TICKLESS_IDLE_BACKSTOP)`. The only timed wake an idle CPU owes is a deadlined `tsleep`/`torpor` sleeper (the global `g_timerwait` list); a running thread's slice is irrelevant to an idle CPU (it has none — so the original "band's slice_size" deadline term was wrong). `TICKLESS_IDLE_BACKSTOP` is **4 ms (TI-4e; retuned from 100 ms — see the TI-4 paragraph below)**: at TI-3 the 100 ms cap was pure defense-in-depth against the §8.7 dropped-IPI hazard (#868 made the IPI reliable, so it was belt-and-suspenders); TI-4e re-roles it as the **work-conservation re-poll** (~250 Hz) that the never-stopped 1 kHz tick used to provide, keeping idle vCPUs warm enough for a fast HVF dev boot.
- Work-arrival wakes ride `IPI_RESCHED` (`sched_notify_idle_peer`, §8.7), tick-independent and unchanged. The one-shot is only for *timed* wakes.
- On wake to a *running* thread the periodic tick resumes.

**As-built (TI-1/TI-2).** The body is the shared `sched_idle_park(bool tickless)` (`kernel/sched.c`), called by `bootcpu_idle_main` (always tickless) and `per_cpu_main` (`tickless=timer_armed` — false until a secondary's timer PPI is enabled, then the byte-identical pre-preempt WFI-on-IPI behavior). The arm uses `timer_arm_oneshot_cnt(tickless_target_cnt(now, timerwait_earliest_deadline(), backstop))` (TI-1 primitives). The **wake-to-running restore** (§3.3 of `docs/TICKLESS-IDLE.md`) is the load-bearing TI-2 detail: under the same IRQ-masked region, after WFI, the loop runs `timer_arm_this_cpu()` (re-arm periodic *before* any placed work runs — else an IPI-woken CPU runs the placed thread up to the backstop un-ticked, an I-17 window) **then** `timerwait_tick()` (service a passed deadline explicitly — the periodic re-arm *deasserts* the one-shot's pending timer IRQ, so without this the handler would never wake the deadlined sleeper, a busy-spin). `TICKLESS_IDLE_BACKSTOP_NS` = 4 ms (TI-4e; was 100 ms at TI-3). There is no `timer_disable_this_cpu` — the always-arm-the-backstop design makes a separate disable path dead code.

**TI-4 work-conservation (the kick + the backstop retune).** Stopping the idle tick (TI-3) also stopped the silent **work-steal re-poll** the 1 kHz tick provided — queued work stranded on a busy CPU until the backstop → a 2.4× boot slowdown. TI-4 restores it the Linux `NO_HZ_IDLE` way: **push-placement** (TI-4b — a wake prefers an idle peer + IPIs it) + the **busy-tick overload kick** (TI-4c — a still-ticking running CPU's `sched_tick`, if it holds surplus stealable work AND a peer is parked, kicks ONE peer to `try_steal`; `sched_rebalance.tla::Overload`, register-then-observe so `NoLostWake` holds) + a **`thread_may_run_on` affinity-ready seam** (always-true today; a future `SYS_SCHED_SETATTR` mask plugs into one predicate consulted at placement + steal, so the work-conserving machinery never foreclosed affinity). **TI-4e root-caused the residual tickless boot slowdown to wake LATENCY, not a guest bug:** the wake path is IPI-prompt (measured **99.85% of tickless parks woken by an IPI**, not the backstop), but resuming a deep-parked vCPU via SGI costs ~0.85 ms under HVF vs ~7 µs hot — an emulation artifact (HVF GICv2-MMIO vmexits + the host vCPU-thread resume; #299/#890), NOT a scheduler defect (a bare-metal SGI is ~ns, so deep-park gives fast boot + ~0% idle there). The **4 ms re-poll** keeps the dev-loop vCPUs warm → ~0.09 ms IPI resumes → HVF boot ~7 s (≈ tickful) at ~5% HVF idle; a v1.x adaptive/accel-gated backstop reclaims 0.3% HVF idle without the dev-boot cost (bare-metal confirmation owed at Lazarus/RPi). No new §28 invariant (composes I-8 + I-9). Spec: `specs/sched_rebalance.tla` (clean + `buggy_nokick` + `buggy_nolift`). As-built: `docs/reference/15-scheduler.md` "Work-conservation under tickless idle (TI-4)".

**I-9 across the arm (the new obligation):** the arm slots into the existing register-then-observe idle loop (§8.5; `idle_in_wfi=true` is set BEFORE the arm + WFI, all under the IRQ-masked idle region), so no work-arrival or deadline is lost between "nearest deadline = D" and WFI. Timekeeping is unaffected — `CLOCK_MONOTONIC`/`REALTIME` ride `CNTVCT`, not the tick. Spec-first (§8.10) is re-enabled for this surface: the focused sibling `specs/sched_tickless.tla` models the arm-race (register-then-observe), TLC-green model-first before the impl -- `scheduler.tla` already proves the tickless wake-correctness, so the sibling captures only the new arm-race (the `sched_oncpu` precedent). Full mechanism, the arm-race reasoning, and the sub-chunk plan: `docs/TICKLESS-IDLE.md`.

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

**Invariant.** This is I-9 generalized: *no death-wake is lost between a sleeper's condition check and its sleep*, for **every** rendez sleep — enforced by the register-then-observe under `wait_lock` exactly as `torpor` enforces it under `torpor_lock`. It also completes I-24's "no Thread executes at EL0 after its Proc's ZOMBIE transition" for the indefinite-sleeper class. **Modeled by `specs/death_wake.tla`** (HOLOTYPE RW-2 SA-1, spec-first re-enabled for this surface 2026-06-10): the clean cfg is TLC-green on the no-lost-death-wake (I-9) + exactly-once-ZOMBIE / no-EL0-after-ZOMBIE (I-24) safety + the `EventuallyReaps` liveness witness; `death_wake_buggy.cfg` (`BUGGY_OBSERVE_BEFORE_REGISTER`) is the executable counterexample of the #809-audit F1 lost-wake / non-reaping hang. The #811 audit + per-site regression tests remain the runtime rigor; the model is the design-level proof obligation + the pre-commit gate for any change to the wait_lock / rendez / cascade protocol.

##### 8.8.1.1 Frame-atomic exception — the elected 9P reader recv (#90)

**STATUS**: DESIGNED (this scripture commit; spec-first — the `specs/reader_frame.tla` model, the impl, and the focused audit follow). Task #90; user-signed-off + design-voted (block-through) 2026-07-19.

The universal `*_INTR` unwind above is *immediate*: the instant `thread_die_pending` is observed at a sleep site, the sleeper unwinds and dies at its tail. That is correct for every sleeper *except one* — the **elected 9P reader** (§21.10, the #841 `mountio` shared-stream reader), whose recv drains a byte stream **shared** across every Proc that mounts through the client. If a reader's Proc dies *mid-frame* — some bytes of a 9P frame already pulled from the transport ring (`got > 0`) — and the die-check unwinds it immediately, those consumed bytes are discarded; the **survivor** that takes over the reader role then reads the frame's TAIL as a new header → the shared byte stream **desyncs** (shared-session death / the task-#50 corruption class). This is the exact hazard the 8c-3 debug-*stop* block-through already closes for the stop path (`stop_no_park` + `stop_unwinds`, `kernel/9p_client.c::reader_recv_frame`); the **death** path — gated by the `thread_die_pending` die-check *below* the stop detour in `sleep()`/`tsleep()` — was left as a pre-existing #841/#811 latent (the 8c-3 close named it task #90).

**The refinement (block-through, mirroring 8c-3).** The die-check (`thread_die_pending`, in **both** `sleep()` and `tsleep()`, `kernel/sched.c`) becomes **frame-atomic** for a Thread inside the reader recv (`stop_no_park` set): it unwinds a dying reader **only at a frame boundary** (`stop_unwinds`, i.e. `got == 0` — no byte of the current frame consumed) and **blocks through mid-frame** — it falls to the normal register-then-observe + `sched()`, so the reader finishes the frame, then unwinds at the next boundary. Every *other* sleeper (`stop_no_park` clear) keeps the immediate unwind — the refinement touches the reader recv alone. The die-check **reuses the existing** `stop_no_park` (the "in a frame-atomic recv" gate) + `stop_unwinds` (the "at a boundary" latch): both are already maintained per-recv (`= (got == 0)`, `do_reader_recv_frame`) for the stop path and are *exactly* the predicates death needs, so #90 adds **no new `struct Thread` field** — it widens the die-check's guard from unconditional to `!stop_no_park || stop_unwinds` (fire the unwind unless we are mid-frame in the reader recv). Block-through strands no caller lock: the elected reader holds **no** lock across its recv (the #841 "lock never held across recv" discipline), so a blocked-through dying reader sleeps in exactly the state a normal recv would — nothing to unwind early for.

**Soundness + the liveness bound.** Block-through is **bounded by the trusted server's whole-frame delivery** (CF-3 B: a 9P server sends complete frames, and the SrvConn ring is ≥ 2× msize so a single frame fits) — a dying reader that blocks through mid-frame reaches the next boundary in bounded time, where it unwinds and dies. `DeathWinsOverStop` is preserved: the tail `el0_return_die_check` still precedes `el0_return_stop_check`, and with **both** death and stop now frame-atomic, both unwind at a boundary — a Proc that is simultaneously stopping and dying finishes the frame, then death wins at the tail. The **v1.x liveness seam**: an *untrusted* / hung 9P server that sends a partial frame then stalls would hang the dying reader mid-frame indefinitely — the same untrusted-server class the #845-F1 "server sends ≤ 1 reply per tag" trust assumption and Loom-4 F4 already carry; every v1.0 9P server is a trusted local Proc (stratumd / dev9p / netd / corvus).

**Invariant + model.** This **narrows** I-9's #811 generalization *for the reader recv only*: the death-interruptibility of that one sleep is **frame-atomic** (deferred to a boundary), not immediate — it does not weaken the no-lost-death-wake property for any other sleeper, and it never *loses* the death (the reader still dies, just at the next boundary; liveness holds under bounded delivery). **Spec-first RE-ENABLED for #90** (the genuinely-subtle async-unwind-vs-shared-stream class — the deep-smp-review point-(a) shape): **`specs/reader_frame.tla`** (model-first, landed before the impl) models a reader consuming a multi-chunk frame under an async unwind signal, proving **NoDesync** (an unwind fires only at `got == 0`, so no consumed partial frame is ever discarded → the survivor never reads a tail-as-header) + **EventuallyUnwinds** (a death-pending reader eventually unwinds — the liveness witness that block-through cannot hang under bounded frame delivery); `reader_frame_buggy.cfg` is the executable counterexample — the pre-#90 mid-frame unwind violating NoDesync. The focused #90 audit (death-path lineage) + the SMP gate are the runtime rigor.

#### 8.8.2 Note-interruptible sleep — the `interrupt` default disposition (LS-5)

**STATUS**: IMPLEMENTED (LS-5a P1 + LS-5b P2 + LS-5c P3-terminate, 2026-06;
the focused adversarial round + the LS-CI `ls-5` scenario ride LS-5-audit,
task #963). As-built per `docs/LIFE-SUPPORT.md` LS-5: the P3-terminate wake
predicate is the `PROC_FLAG_INTR_TERMINATE_PENDING` latch (proc.h), read
lock-free by `thread_die_pending` (notes.c) at every §8.8.1 sleep site; the
EL0-return tail remains the truth. Builds directly on §8.8.1.

`interrupt` (the cooked-Ctrl-C note) is, at v1.0, half-wired: the kernel posts it
but (a) to a NULL `g_console_owner` during a login session [dropped], (b) an
uncaught `interrupt` is left *queued*, not acted on (notes.c `notes_deliver_at_-
el0_return`), and (c) it cannot wake a child blocked in `sleep` / read. So
foreground Ctrl-C does nothing (ground-truthed: a `yes` runaway floods unfazed).
LS-5 makes `interrupt` a **real Plan 9 note / Unix SIGINT** in three properties:

- **P1 — delivery.** The session shell owns the console (`g_console_owner = ut`)
  via a new spawn perm **`SPAWN_PERM_CONSOLE_OWNER`**, conferred by trusted login.
  This is console-*owner* ("who receives Ctrl-C"), strictly DISTINCT from
  console-*attach* (I-27, the SAK / elevation gate) — a per-bit separation like
  `SPAWN_PERM_MAY_POST_SERVICE`; the owner bit NEVER confers attach, so I-27 is
  untouched. The shell forwards `interrupt` to its foreground child (the existing
  U-7c-b notes-fd poll). Real process groups + a controlling terminal = Phase-8
  job control; shell-forwards is the v1.0-correct interim.
- **P2 — default disposition (uncaught `interrupt` terminates).** Generalize the
  `snare:*` default-terminate (§7.6) to `interrupt`: at the EL0-return tail, an
  `interrupt` with no registered handler, not masked, on a NON-self-managing Proc
  terminates the Proc (SIGINT's "default = die, catchable"). The **self-managing
  gate** is the discriminator the fd-read note model requires: a Proc that has
  opened its notes fd (`devnotes`) has declared it consumes its own notes →
  exempt from auto-terminate (the shell qualifies via `open_notes`; a dumb
  coreutil never opens it → dies). A program catches Ctrl-C by registering an `on
  note interrupt` handler or masking it. This is an **I-19 extension** (a per-note
  default disposition) — it does NOT touch ordering, exactly-once, or N-4.
- **P3-terminate — a terminate-disposition `interrupt` wakes a blocked child,
  reusing §8.8.1.** §8.8.1's wake machinery (per-Thread `wait_lock` +
  `rendez_blocked_on`, every rendez site, unwind at the EL0-return tail) is
  widened: the wake predicate becomes "group-exit death **OR** a pending
  terminate-disposition `interrupt`." A single-threaded blocked child with no
  handler / no notes-fd / unmasked cannot change its disposition while blocked (it
  is not executing EL0 code to call `notify()`), so its disposition is decided at
  post time and it takes the §8.8.1 death path: wake + terminate at the tail. One
  more trigger on an already-audited mechanism — NOT a parallel kill path, and NOT
  the uncatchable-`kill` collapse (which would erase the catchable `interrupt` /
  non-catchable `kill` (N-4) distinction).

Composed (P1 + P2 + P3-terminate): **Ctrl-C terminates any foreground command —
CPU-bound, output-bound, or blocked in sleep / read — catchably.**

**LS-5 / LS-8 boundary.** Deferred to LS-8 (U-PTY: pollable cons + termios ISIG):
**P3-deliver** — promptly delivering a *caught* (handler-bearing / self-managing)
`interrupt` to a *blocked* program via an interrupted-syscall (`-EINTR`) return
WITHOUT terminating it. Its sole v1.0 consumer is idle-prompt-cancel (the shell's
own blocked cons-read returning so the editor's `Cancel` fires) + the reactive
mid-edit Ctrl-C — both inherently LS-8 (the editor never sees a `0x03` byte today;
the kernel cooks it to the note).

**Invariants.** I-19 gains the `interrupt` default disposition (terminate-if-
uncaught-and-not-self-managing); I-9 / §8.8.1's death-wake generalizes to
death-or-terminate-`interrupt`. No new TLA+ module (spec-to-code suspension,
2026-05-23); the focused LS-5 audit (the death-path + notes lineage) + the §8.8.1
per-site re-validation + the LS-CI `ls-5` scenario are the rigor.

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
- **Tickless idle (the TI arc, `docs/TICKLESS-IDLE.md`, §8.6):** the focused
  sibling `specs/sched_tickless.tla` (LANDED TI-spec) models the idle-enter
  arm-race: `registered` (the impl's `idle_in_wfi`) is set BEFORE the park, so a
  work-arrival in the window sees a registered CPU and IPIs it. `NoLostWake`
  (~(parked ∧ pending)) + `ParkedImpliesRegistered` + the `EventuallyRuns`
  liveness hold (clean cfg, TLC-green) vs `BUGGY_PARK` (park-before-register ⇒
  the peer reads `idle_in_wfi = FALSE`, sends no IPI ⇒ a parked CPU with runnable
  work — the lost-wake counterexample at depth 4). A SIBLING, not a
  `scheduler.tla` extension: `scheduler.tla` already proves the tickless
  wake-correctness (its `wfi`/IPI/`Wake` model never treats the periodic tick as
  a wake source, and `LatencyBound` holds without it), and the arm-race would
  entangle its 11 cfgs — the `sched_oncpu`/`sched_alpha` precedent.

### 8.11 Preemption discipline: plain spinlocks disable preemption (#359/#360)

A plain `spin_lock` hold makes the holding THREAD non-preemptible (the Linux
"spin_lock disables preemption" rule), realized as a per-thread hold count
(`Thread.preempt_count`, incremented before the acquire, decremented after the
release store). `preempt_check_irq` defers (does not consume) a pending
`need_resched` while the interrupted thread's count is nonzero -- the deferred
preempt fires at the first IRQ-return after the hold drops (<= 1 tick, the
granularity preemption already had). `sched()` extincts if entered with a
nonzero count: sleeping or yielding while holding a plain spinlock is
forbidden (the lock-across-sleep deadlock class), and an unlock at count zero
extincts as an unbalanced release.

Why load-bearing: syscalls and EL0 faults run IRQ-masked end-to-end, so their
lock spins are non-preemptible; a holder running IRQ-ENABLED (a kproc kthread,
or a spawn thunk on a fresh thread) could previously be preempted mid-hold and
starve behind a full complement of masked spinners -- a permanent whole-guest
deadlock (#359, exposed by the parallel on-device `go build` on the shared
dev9p client's `c->lock`; latent on every lock shared between a preemptible
context and syscall paths).

The count is per-THREAD, not per-CPU: it must travel with the thread across a
migration (a per-CPU slot has an unfixable mid-increment preempt+migrate tear
that poisons the old CPU's slot). The one cross-thread lock handoff --
sched()'s `cs->lock`, acquired by prev and released by the resuming thread --
uses dedicated RAW (uncounted) variants, sound because sched runs fully
IRQ-masked across the hold. See `docs/reference/15-scheduler.md` ("Preemption
discipline") for the full mechanism, the first-cut per-CPU bug, and the tests.

### 8.12 Summary

EEVDF on per-CPU run trees with work-stealing, three priority bands, tickless idle, IPI-driven cross-CPU operations, a per-thread spinlock preemption discipline, Plan 9 idiom layer on top, formally verified.

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
├── env/                 ← per-Proc environment (devenv kernel Dev; Plan 9 Egrp; G15, §9.7)
│   └── <NAME>           ← read: value; write/create: set; the dir lists the names
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

**As-built — the v1.0 boot namespace (#57).** The tree above is the target; what the boot namespace actually binds today: `/` (devramfs pre-pivot → the Stratum disk root post-pivot), `/bin` (the initrd binary tree, MREPL-bound post-pivot — §9.6.8 / #58), `/srv` (devsrv, the per-territory service registry — stalk-3), and **`/proc` (devproc) + `/ctl` (devctl)** (#57a). The introspection Devs are grafted onto synthetic devramfs mount-point dirs (0555, SYSTEM-owned) in the kproc boot namespace — inherited by every Proc via `territory_clone` — and re-grafted onto the pivoted root by the long-running init (the same pre-pivot-handle + post-pivot-MREPL idiom as `/srv` and `/bin`). `/proc` is world-walkable (Plan 9 all-pids-visible; `devproc.perm_enforced == false`), with per-pid `ctl` kill writes staying **I-26 two-axis-gated** (owner OR `CAP_HOSTOWNER`/`CAP_KILL`) independent of namespace reachability. `/ctl` is read-only introspection (`devctl_write == -1`), with the one exception that **`/ctl/kernel-base` (the live KASLR slide — an I-16 secret) is `CAP_HOSTOWNER`-gated** at the read site (the #57a focused-audit F1 fix; `CAP_HOSTOWNER` is elevation-only, so the slide is admin-only-via-elevation while the coarse procs/memory/devices/sched leaves stay world-readable). The mount thus widens *visibility*, never *authority* — but only after the focused audit closed three pre-existing devproc/devctl read-path latents the mount *activates* (EL0-reachable for the first time): the kernel-base KASLR leak (F1); a cross-Proc **UAF** where `devproc_read`/`devproc_stat_native` dereferenced a `Proc` after `proc_find_by_pid` released the proc-table lock (F2 — now find+format under `g_proc_table_lock`, the kill-path shape, so a concurrent reap cannot free the target mid-read); and the unbounded `/ctl/procs` IRQ-off proc-table walk (F3 — `format_procs_cb` now stops at buffer-full). (Making devproc/devctl reachable through `stalk` required fixing their `Dev.walk` to honor the **reuse-`nc` contract** — return the caller's pre-clone as `wq->spoor`, with a 0-element walk yielding `nqid == 0` — that `clone_walk_zero`'s mount-cross needs; they carried the pre-16b-gamma self-cloning shape because they had never been mounted, the same bug devramfs_walk fixed at 16b-gamma.) **`/dev`** is bound as-built (#57b) by **`devdev`** (a new aggregating directory Dev, dc='d', name="dev") mounted with the same /srv-idiom: it serves the kernel char devices `null`/`zero`/`full`/`random`/`urandom` (world-rw, ungated -- the same on every Unix) plus `cons`/`consctl` (the console). **`/dev/cons` does NOT create a second, ungated console front-door**: `devdev.open` enforces the same `proc_is_console_attached` gate as `SYS_CONSOLE_OPEN` for the `cons`/`consctl` qids (the I-27 gate-at-namespace-open; IDENTITY-DESIGN §9.8), so a non-attached caller resolves the name but `open` fails -1 -- only the console-attach holder (joey pre-relinquish / post-SAK corvus) can open it, exactly as via the syscall. The gate is at open (covering subsequent read AND write) and orthogonal to the 0666 leaf perms, mirroring `/ctl/kernel-base`'s `CAP_HOSTOWNER` read-gate (#57a). `consctl` is present + gated but carries no v1.0 modes (termios is LS-8 #952). **`/hw`** is bound by **`devhw`** (dc='H', the DTB hardware inventory as a walkable tree — the Menagerie discovery root, §22.7; read-only, `perm_enforced == false` → visibility not authority, the privilege boundary being the I-34 allowance, not the tree), and nested under it **`/hw/pci`** is bound by **`devpci`** (dc='P', a read-only directory Dev mounted via a synthetic `pci` mount-point child of `devhw`) serving the kernel's boot-enumerated PCI functions (`g_virtio_pci_devs[]`) as `<bus.dev.fn>/ctl` topology — vendor/device → derived `virtio` id + the INTx-routed INTID — **never raw ECAM, no config-space write surface** (the pci-3 "userspace never gets raw ECAM" I-5 property). `devpci` is the kernel-mediated PCIe **discovery source** the warden reads in-process (the `DtbSource` analog; MENAGERIE §7/§16-6b); at v1.0 only the pre-pivot warden reads it, so the post-pivot re-graft of the nested `/hw/pci` mount is a v1.x seam. The userspace-server entries (`ptmx`/`pts`, `fb`, `video`, `janus`, `proc-linux`, `net`) and the CAP-gated `mem`/`tty` aliases land with their servers (Phase 8 / the container runner #70 / LS-8). **`/env`** is bound (Go Stage 4a, G15) by **`devenv`** (dc='E', §9.7) — the per-Proc environment group, mounted by the same `/srv`-idiom and re-grafted across the pivot; the mount is global but every op resolves the calling Proc's own `p->env` (per-Proc content, I-1), so a Proc sees only its own environment.

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

#### 9.6.8 Exec from the namespace (#58)

Until #58, the `SYS_SPAWN_*` family resolved the binary through the flat boot-cpio
table (`devramfs_lookup`), bypassing the caller's Territory entirely. That left two
holes the container story cannot tolerate: a binary in a mounted FS (a container
root, the disk-backed Stratum FS) was **categorically unexecutable**, and a fully
confined Proc could still name + spawn **any** cpio binary -- a *reverse* visibility
leak, "what the container sees" did not bound "what it can run". #58 closes both by
routing every spawn through `stalk`, **realizing I-28 + I-1 for the exec path** (no
new invariant; the keystone of the container story, RW-12 W5-F1).

**Resolution.** Each of the five spawn bodies resolves the program name exactly as
`SYS_OPEN` does: an absolute path from `territory->root_spoor`, a relative name via
the LS-4 cwd-join (`territory_resolve_cwd`), through `stalk(p, start, path, len,
STALK_OPEN, OEXEC)`. Per-component X-search gates every directory hop; the final
hop's `perm_want_for_omode(OEXEC)` requires `PERM_X` on the file. The `OEXEC` open
yields a `RIGHT_READ` handle (A-3), and the ELF is **slurped** from the resolved
Spoor via `dev->read` into an 8-aligned kernel blob (bounded by `SYS_SPAWN_BLOB_MAX`),
then handed unchanged to `exec_setup` -- the same shape the cpio path already used
(it `memcpy`-d the whole binary into a kmalloc'd blob), so the audited ELF loader /
W^X reject / segment map stay byte-identical. Resolution + read run in the **parent's**
context (its Territory), like Unix `exec`. `devramfs_lookup` remains ONLY for the
kernel-internal init load (kproc loads `/joey` before any namespace exists) and kernel
tests -- neither is EL0-reachable, so the reverse-leak surface (userspace spawn) is
fully closed; a `stalk` miss returns `-1`, never a flat-table fallback.

**Boot bootstrap (option B, user-voted 2026-06-12): bind the initrd onto `/bin`.**
joey pivots to the disk root before it spawns the service chain (corvus, login), so
post-pivot those resolve in the disk-rooted namespace. Rather than duplicate the
binary corpus onto the data pool (the Unix pivot_root "installed system" model), joey
**binds the cpio binary tree onto `/bin`** in its post-pivot namespace -- the Plan 9
idiom (the boot medium is *bound into* the namespace; Fuchsia/Genode's
binary-from-namespace-mount agrees), reusing joey's existing pre-pivot-handle ->
post-pivot-MREPL pattern (the one it already uses to re-graft `/srv`). The binaries
live once, in the initrd; the disk pool stays for user data. Pre-pivot spawns keep
their bare names (cpio root, resolved via cwd=`/`); post-pivot spawns name
`/bin/<prog>`; the shell (`ut`) resolves a bare command through `$path` = `/bin` (a
command containing `/` is used as-is) -- the Plan 9/Unix split where the kernel
resolves a path and the shell does `$path`. A confined Proc that does NOT inherit the
`/bin` bind cannot name the system binaries (the leak stays closed); a container with
its own root + binaries can exec them (the capability opens). v1.x seams: a
disk-installed `/bin` (the "real installer", pulling the recorded host-bake
corpus-populate forward) and spawn-from-fd.

#### 9.6.9 Namespace name retention: Spoor.path (#66)

The Plan 9 4th-edition `Chan.path` model, adapted. Each `struct Spoor` carries a
refcounted, copy-on-walk `struct Path *path` — the cleaned namespace name by which
the Spoor was reached (`/srv/stratum`, `/bin/joey`, `/proc/3/ns`). This is the
substrate the introspection surface reads: `fd2path` (`SYS_FD2PATH`), `/proc/<pid>/fd`
names, and the full `/proc/<pid>/ns` mount/bind list the Plan 9 `ns` tool renders.
Until #66 a Spoor retained only its `(dc, devno, qid)` identity — enough to resolve
and mount, but a fd could not be turned back into the name that opened it.

**The Path is strictly non-load-bearing (invariant I-33).** The resolver is
*write-only* to `Path`: `stalk` / the single-hop walk / create *append* to it but
NEVER *read* it to make a decision. Resolution, the per-component X-search,
mount-crossing, and every permission check consult only `(dc, devno, qid.path)` +
`stat_native`, never `->path`. So a wrong, stale, truncated, or absent Path can never
change a resolution outcome, a privilege decision, or any syscall result other than
the cosmetic content of the three introspection readers. This is the design's central
soundness lever and the reason #66 — invasive as it is to the just-audited hot walk
path (RW-4) — adds no resolution-correctness risk: the worst a Path bug can do is
misreport a name.

**Plan 9 minus the `mtpt` history.** Plan 9's `Path` additionally carries a
`Chan **mtpt` mount-point stack so the device layer can walk `..` back across a mount
boundary. Thylacine's `stalk` resolves `..` *lexically* against its own in-call
`trail`, contained at `root_spoor` (I-28) — it never consults a Path to climb. So
Thylacine's `Path` is the pure name string with no mount history:
`struct Path { int ref; u32 len; char s[]; }` (one allocation, NUL-terminated, the
`ref` atomic). The string is **immutable once built** — `addelem` never mutates a
shared Path in place; it always allocates a fresh one — so the only
concurrently-mutated field is the atomic `ref`, and a Path string is safe to read
locklessly.

**Lifetime is subordinate to the Spoor's.** Every Spoor that references a Path holds
exactly one ref on it for the Spoor's whole life (taken when the Spoor is born —
`spoor_alloc` leaves it NULL, `spoor_clone` shares the parent's via incref — dropped
in `spoor_free_internal`). A Path is therefore freed exactly when its last referencing
Spoor frees. This piggybacks the Path on the already-audited Spoor refcount rather
than introducing a new lifetime axis: `path_lifetime ⊆ spoor_lifetime`, with shared
Paths (across clones) independently refcounted. No new UAF surface beyond "is the
Spoor refcount correct" — a settled question.

**Accumulation (copy-on-walk).** `spoor_clone` shares the parent's Path (O(1) incref —
the hot path, run on every walk hop including the ones that fail and unwind, copies no
string). A *successful* resolution step then replaces the clone's shared Path with an
extended private one:

- A walk step `name` → `child->path = addelem(parent->path, name)`: `/a` + `b` →
  `/a/b`; the root `/` + `b` → `/b`. `.` is a no-op (keep the parent's); `..` pops the
  last element (`/a/b` → `/a`, `/` → `/`). In `stalk`, `.`/`..` are handled by the
  resolver and never reach `addelem`; the single-hop `SYS_WALK_OPEN` (which passes a
  raw component to `Dev.walk`) can, so the helper handles all three.
- A mount **cross** transplants the *mount-point's* name onto the crossed clone:
  walking `/mnt` (a mount point, name `/mnt`) into the mounted tree keeps the namespace
  name `/mnt`, NOT the mount source's internal name. `stalk_cross_mounts` sets the
  final crossed Spoor's Path from the probe (mount-point) Spoor's Path. (A
  mount-over-mount chain keeps the original mount point's name — the user is "at /mnt"
  regardless of how many devices stack.)
- `..` across a cross needs no Path op: each `trail` entry carries its own correct
  Path, and popping the trail reveals the parent's shorter Path. The string tracks the
  trail exactly — the reason the `mtpt` history is unnecessary.

**The root seed.** Accumulation roots at a Spoor that already has a Path. The `/` is
seeded at the **root-filesystem Dev's attach** — `devramfs_attach` and
`dev9p_attach_client` stamp their fresh attach-root Spoor `/` at *birth*, before it is
published. This keeps the Path strictly **immutable after set** (the I-33
set-before-publish discipline — no lock, no atomics): a root Spoor is never re-stamped
after a peer thread could observe it, so `SYS_CHROOT`/`SYS_PIVOT_ROOT` do NOT mutate a
published Spoor's Path (an earlier sketch that stamped at chroot/pivot would have raced
a concurrent `fd2path` on the same fd — a multi-threaded Proc doing `SYS_CHROOT(fd)` +
`SYS_FD2PATH(fd)`). When a `/`-seeded root is instead used as a mount *source* (the
`/bin` bind's cpio root; a `/home/<user>` 9P mount), `stalk_cross_mounts` transplants
the mount-point's name onto the crossed clone, so the source's `/` is only ever
observed when the root *is* the namespace root (joey's pivot target). A NULL Path means
"name unknown" (a non-filesystem-root attach root, or a walk from an fd whose own Path
is unknown); the readers render it empty / `?`, never a wrong name. (A `SYS_CHROOT` to a
walked-to directory leaves that directory's accumulated name as the root name rather
than `/` — cosmetically imperfect but sound, and a documented v1.x refinement; the v1.0
boot roots are attach roots, so they are correctly `/`.)

**The chroot-name residue is a name *disclosure*, not just a cosmetic wart (audit F4).**
Because a chroot does not re-stamp the root, and an inherited / spawn-passed fd carries
the *spawner's* walk name, a confined Proc (corvus chrooted to its storage capability; a
future container) can `SYS_FD2PATH` a held fd and read the *outer* namespace layout it
was confined away from — e.g. a host-side `/var/lib/corvus/...` prefix or a
container-runner's host path. This has NO authority consequence (I-33 holds — the
resolver never reads the Path, `..` stays contained at `root_spoor` per I-28, and
`SYS_FD2PATH` grants nothing): it is a pure information-flow leak of namespace structure
across a confinement boundary, not an escape. The v1.x re-stamp-at-chroot (or
stamp-`/`-on-the-new-root) refinement should therefore be weighed as a *disclosure* fix;
until it lands, a Proc that must not learn its spawner's layout should be handed fds it
walked itself.

**Allocation failure / overflow is best-effort, never fatal.** `addelem` OOM, or a
path that would exceed `SYS_OPEN_PATH_MAX` (1024), leaves the child's Path NULL — the
walk *still succeeds* (the Path is decorative metadata, not load-bearing per I-33).
Introspection then reports "unknown" for that Spoor and its descendants. A path-alloc
failure can never fail a resolution.

**Hook sites** (the three Spoor-minting walk paths): `stalk` (append per successful
step + transplant per cross + the base seed via clone-share), `sys_walk_open_handler`
(the single-hop walk-from-fd: one `addelem`), `sys_walk_create_handler` (the created
child: one `addelem`). All are thread-local Spoor mutations before publication, so no
`Path`-field lock is needed (the field is set-before-publish / read-after, exactly like
`qid` / `dev`); only `path->ref` is concurrent (atomic).

**Consumers**: `SYS_FD2PATH(fd, buf, cap)` copies `handle→spoor→path→s` (#66a).
`/proc/<pid>/ns` (#66b, as-built) renders the real mount list: `PgrpMount` gained a
refcounted `mp_path` (the mount-POINT's namespace name — the table keys on the
`(dc, devno, qid.path)` identity, NEVER on `mp_path`, so the name is introspection-only
per I-33), captured by `path_ref`'ing the resolved mountpoint Spoor's `→path` at
`mount()` / MREPL and dropped at `unmount()` / MREPL-displace / `territory_unref` /
shared per entry at `territory_clone`. `territory_format_ns` (in `territory.c`, so the
`ns_lock` discipline stays encapsulated) renders one `mount <mountpoint> <source>` line
per entry — the source column is its Spoor's `→path` when it has a namespace name, else
`#<dc>` (the Plan 9 device spec) for a device root — plus a `binds: <N>` count.
`devproc.c::format_ns` calls it inside the `proc_for_each` callback, so the read runs
under `g_proc_table_lock` (the #57a F2 envelope keeps `p→territory` alive) AND, briefly,
`ns_lock` (the mount entries + their ref-held immutable `Path` strings are stable for the
copy); that `g_proc_table_lock → ns_lock` edge is acyclic (nothing held under `ns_lock`
takes `g_proc_table_lock`). The native `ns [pid]` tool (`usr/coreutils/src/bin/ns.rs`,
default pid 0 = kproc) reads `/proc/<pid>/ns`.

`/proc/<pid>/fd` is **deferred to #66c** (seam): a cross-Proc fd-list read of a LIVE
peer's handle table races the #926 at-exit handle-table free (which runs OUTSIDE
`g_proc_table_lock` with lockless slot-zeroing — the documented FOOTGUN at
`kernel/handle.c:301`). Closing it soundly needs the #926 table-lifetime restructure
(split `handle_table_free` into a lock-protected `close_all` + a reap-time struct-free,
so the table lives until reap like `territory` does) — a death-path-lineage change that
warrants its own focused chunk + audit, not a #66b fold-in.

**Split**: #66a (the substrate + `SYS_FD2PATH` + kernel tests) → #66b (`/proc/<pid>/ns`
via the refcounted `PgrpMount.mp_path` + the `ns` tool + docs) → #66c (`/proc/<pid>/fd`,
after the #926 restructure).

### 9.7 The per-Proc environment group (`/env`, G15)

Each Proc carries an **environment group** — a ref-counted table of name→value pairs, surfaced as a per-Proc `/env` directory. This is the Plan 9 `Egrp` / `devenv` idiom, and it is the v1.0 answer to **environment variables (G15)**: Thylacine's native ABI passes no Unix `envp` array at spawn, so the environment is a *namespace object*, not a syscall argument — written and read as files.

**Why a device, not a spawn-ABI slot.** Surfacing the environment as `/env` files (a) keeps `SYS_SPAWN_FULL_ARGV` unchanged (no `envp` ABI), (b) matches the Plan 9 model the Go runtime already speaks (`runtime/env_plan9.go::goenvs` reads `/env/*`), and (c) makes the environment composable and inspectable through the same namespace machinery as everything else. A program sets a variable by writing `/env/NAME`, reads it by reading `/env/NAME`, and lists the environment by reading the `/env` directory. (Rejected: wiring the reserved `envp` slot into `SYS_SPAWN_FULL_ARGV` — an ABI change, less Plan-9-idiomatic, and it would force a non-plan9 Go env path.)

**The Env group** (`kernel/env.{c,h}`):

```c
struct EnvEntry {
    u64  id;                    /* monotonic within this Env; never reused */
    u32  len;                   /* value length in bytes */
    char name[ENV_NAME_MAX];    /* NUL-terminated */
    char value[ENV_VALUE_MAX];  /* raw bytes */
};

struct Env {
    _Atomic int     ref;
    spinlock_t      lock;
    u64             next_id;
    int             count;
    struct EnvEntry entries[ENV_MAX_ENTRIES];
};
```

`struct Proc` gains an `env` pointer (allocated lazily — a Proc with no environment carries `env == NULL`, read as empty). All access — the Dev's walk/open/read/write/create/remove/readdir paths — takes `env->lock`; the table is shared across a Proc's threads exactly as the Territory and handle table are (peer threads see one `p->env`), so the lock is load-bearing for the multi-thread Proc (Go drives many M-threads through one Proc).

**Inheritance = deep copy on spawn** (`env_clone_into`, in `rfork_internal` right after `allowance_clone_into`). A spawned child inherits a *copy* of the spawner's environment — independent thereafter — exactly as it inherits a `territory_clone` copy of the namespace. This is the Plan 9 default-copy-on-`rfork`; the reserved `RFENVG` flag (share-vs-blank semantics) stays deferred at v1.0 (copy always). **No spawn-ABI change**: the child gets its environment through the kernel clone, never through a syscall argument — so a Go `go build` sub-process (`compile`/`asm`/`link`) inherits `$GOROOT`, `$GOCACHE` etc. from the `go` tool the same way a child inherits the namespace. (An *explicit* `Cmd.Env` override in `os/exec` — distinct from inheritance — is a userspace seam at v1.0; its resolution, if a workload needs it, is a Go-side `cmd/go` concern, never a kernel ABI change.) The group is freed at `proc_free` (after `allowance_free`).

**The `/env` Dev** (`devenv`, `dc='E'`, `name="env"`) is a synthetic directory Dev, mounted in the boot namespace by the same `/srv`-idiom as `/dev` / `/proc` / `/ctl` (`joey_mount_static_dev`, a synthetic devramfs mount-point dir, inherited by every Proc via `territory_clone` and re-grafted across the pivot). Like `/proc/self`, the mount is global but the *content is per-Proc*: every Dev op resolves `current_thread()->proc->env`, so a Proc sees only its own environment (**I-1**). Entries enumerate as standard 9P2000.L `Treaddir` dirents (the format every other Thylacine directory emits — `/env` does not introduce a legacy stat-format directory read; the Go runtime gets a thin `runtime/env_thylacine.go` that parses readdir, structurally the plan9 `goenvs`). A walked `/env/NAME` Spoor carries `qid.path =` the entry's **monotonic id** (assigned at create, never reused), and every op re-resolves id→entry under `env->lock`, so a variable removed between walk and read fails clean rather than resolving to a different variable (the net-3d slot-reuse discipline, applied preemptively).

**Bounds (DoS floor).** `ENV_MAX_ENTRIES` / `ENV_NAME_MAX` / `ENV_VALUE_MAX` cap a Proc's environment; storage is per-Proc and freed at exit. Composes with the per-Proc resource floor (**I-32**, §3.8).

**Invariant posture.** Composes **I-1** (per-Proc isolation — per-Proc content behind a shared mount); **no new §28 invariant**. A new EL0-facing Dev + a new per-Proc group + a multi-thread-shared structure → audit-bearing (§25.4), the `env->lock` discipline being the standard per-Proc-group pattern (Territory/allowance), so prose-validated per the 2026-05-23 spec-to-code broadening (no TLA+).

### 9.8 Open design questions

None at Gate 3.

### 9.9 Summary

Per-process territory, `bind` / `attach_9p` / `mount` / `unmount` as the only composition operations. `Dev` vtable for kernel devices; `dev9p` is a Dev whose vtable proxies to the kernel 9P client — userspace drivers and remote filesystems both present as Spoors with no semantic distinction. Standard `/dev/`, `/proc/`, `/ctl/` paths. Driver crash recovery via process supervision.

---

## 10. IPC

**STATUS**: COMMITTED

### 10.1 9P as the universal IPC

There is no separate IPC mechanism. All inter-process communication is mediated by 9P: one process mounts another's 9P server and reads/writes files. Pipes are 9P streams. Shared memory is a 9P file backed by anonymous memory (with BURROW handles for zero-copy). Message queues are 9P files.

This is the Plan 9 model, adopted unchanged.

**The network is 9P too** (the totalization completes; NOVEL.md Angle #1, added 2026-06-08). The stack is `/net`-as-9P (`/net/tcp`, `/net/cs`, dial by writing `/net/tcp/clone` — the Plan 9 model), realized as a userspace `/net` 9P server (`netd`, the stratumd-as-driver precedent — a `CAP_HW_CREATE` Proc owning virtio-net). A consequence falls out: **network I/O rides Loom with no socket opcodes** — a `LOOM_OP_READ`/`LOOM_OP_WRITE` on a `/net` connection's data fid *is* recv/send, and a multishot read on the listen file *is* an async accept loop; Loom's vocabulary stays pure 9P (no `LOOM_OP_SEND`/`ACCEPT` à la io_uring), and Loom is what makes the userspace stack fast enough (it amortizes the app↔`netd` hops). The native remote-access story is therefore `import`/`exportfs` over authenticated 9P (corvus as the auth agent, the Plan 9 `cpu(1)` model), not sshd — though sshd stays a portable Pouch option for ecosystem compatibility. The binding Phase-8 design is **`docs/NET-DESIGN.md`** (the #68 charter, 2026-06-15), which fills the eleven holes in this shape and binds three decisions: shared `netd` + namespace-narrowed views (one stack; per-Proc isolation via `/net` view-narrowing, not stack duplication), pouch-userspace BSD-socket compat (the socket calls are libc->`/net` translations, NOT kernel syscalls), and a namespace-restriction firewall (the explicit packet filter is a v1.x add).

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

**Network sockets are intentionally absent from the kernel syscall surface.** `socket`/`connect`/`bind`/`listen`/`accept`/`sendto`/`recvfrom` are NOT kernel syscalls (the network is 9P; NOVEL #1). Linux/pouch binaries reach them through a pouch boundary-line that translates each to `/net/tcp/clone` file operations (the Genode `socket_fs`-in-libc model); native programs use `/net` directly. See `NET-DESIGN.md` §7 (the #68 charter) -- this reconciles the long-standing ROADMAP §9.1 "socket shim" claim with §11.5's zero socket syscalls (HOLOTYPE W4-F2).

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
- VirtIO-MMIO: block, rng, input at v1.0.
- VirtIO-PCI: GPU (PCI-only in QEMU); **and virtio-net from the pci sub-arc** (`docs/VIRTIO-PCI-DESIGN.md`) — net moves to a per-device page-aligned BAR so a persistent `netd` and `stratumd` co-reside with real per-device isolation (dissolves the #140 sub-page-MMIO contention). blk → PCI is a recorded v1.x seam.

**Kernel-side VirtIO core** (`drivers/virtio-core/` — runs in-kernel):
- Virtqueue management (split virtqueue at v1.0; packed virtqueue post-v1.0).
- Descriptor chain allocation and completion tracking.
- IRQ hookup via GIC.
- Per-device control registers (`/dev/virtio/<n>/`).

**Userspace VirtIO device drivers**: receive `KObj_MMIO` handles for the device's BAR/MMIO range, plus `KObj_IRQ` for the device's interrupt line, plus `KObj_DMA` for DMA buffers. Implement the device-specific protocol on top of the kernel-exported VirtIO core.

There is **no in-kernel VirtIO device driver code** at v1.0. virtio-blk, virtio-net, virtio-input, virtio-gpu are all userspace from Phase 3 onward.

### 13.2 PCIe + the userspace virtio-PCI transport

`virt` exposes a PCIe root complex (ECAM, `pci-host-ecam-generic`). The kernel
**owns config space** (ECAM is bus-wide — it cannot be page-isolated per
function) and mediates per-function setup; a **userspace driver owns the
device's per-function, page-aligned BAR**. Full design + ground truth (the DTB
`interrupt-map` / BAR-window decode): `docs/VIRTIO-PCI-DESIGN.md`.

The kernel side (P4-H enumerator + the pci sub-arc):
- Bus-0 enumeration (Type 0 endpoints; bridges/multi-bus are a v1.x seam).
- **BAR assignment** — booting bare (no UEFI), the kernel is the PCI resource
  allocator: it sizes each BAR and assigns a region linearly from the
  host-bridge 32-bit MMIO window (no remapping).
- **INTx routing** — the device's GIC INTID is computed from the DTB
  `interrupt-map` swizzle (`SPI = 3 + ((slot + pin − 1) mod 4)`, lines 35–38 on
  `virt`), delivered to the driver via the existing `KObj_IRQ`. MSI-X needs a
  GIC ITS/v2m driver → v1.x seam.
- **Capability resolution** — the kernel walks `VIRTIO_PCI_CAP_*` and hands the
  driver the resolved common-cfg / notify / ISR / device-cfg region map.

The new per-device capability is **`KObj_PCI`** (exclusive per-function claim,
non-transferable — I-5): `SYS_PCI_CLAIM` → claim + assign/enable BARs +
cap-resolve; `SYS_PCI_MAP_BAR` → map a page-aligned BAR (kernel-controlled PA,
reached only through the owned handle); `SYS_PCI_INFO` → the region map + INTID.
The driver then drives virtio-pci-modern over the mapped BAR (`KObj_DMA` for the
queues). Because each function's BAR is its own page-aligned region, two
persistent userspace drivers (`netd`, a future PCI `stratumd`) get **real
per-device isolation** — dissolving the #140 sub-page-MMIO contention.

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

### 14.3 9P session model (as-built: shared-multiplexed)

**The original one-connection-per-Proc intent is superseded by the as-built shared client** (corrected 2026-06-11, HOLOTYPE RW-12 W1-F1). At v1.0 the kernel runs a *single shared, multiplexed* 9P client over the mounted Stratum connection (the #841 elected-reader design, §21.10): Procs that inherit the Stratum mount share the client via `territory_clone`, and it demultiplexes their concurrent RPCs by tag. The original design intent — one fresh connection per Proc (VISION §11) — is superseded; a per-Proc-connection model remains a possible v2.x direction where per-process Stratum-side territory isolation would justify the per-connection cost. Per-*user* sessions do get their own Stratum connection, via login's `--role client` stratumd (A-5b).

Each Stratum connection carries its own per-connection fid namespace / territory (`stratum/v2/docs/reference/20-9p.md`). Thylacine's per-process territory and Stratum's per-connection territory remain complementary layers (VISION §11): the Thylacine territory governs cross-server composition, the Stratum per-connection territory governs composition within one Stratum connection.

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

## 17. Halcyon integration (and Aurora, the textual sibling)

**STATUS**: COMMITTED (EVOLVED 2026-06-08 — the compositor / client architecture)

> The Phase-0 §17 held that "Halcyon does not require a compositor or display
> server, beyond raw framebuffer write." That is **superseded**: the graphics phase
> (`docs/TAPESTRY.md`, signed off 2026-06-07) introduced `tapestryd` as the display
> server, and the 2026-06-08 design session elevated it to the **compositor** and
> evolved Halcyon into an anti-window tiling environment. This section records the
> *foundational decisions* at ARCH altitude; the full model is **`docs/TAPESTRY.md`
> §13-18** + **`NOVEL.md` Angle #4**.
>
> **Two first-class environments (scope extension 2026-06-15; `docs/AURORA.md`,
> VISION §3.3).** Thylacine ships **Aurora** (textual) + **Halcyon** (graphical) and
> the user chooses. **Aurora** is the textual environment — a baked-font text grid
> blit to a real framebuffer (the screen-side of the VT protocol) + session
> multiplexing + a status surface, hosting Utopia + the Kaua TUI suite. It is *not* a
> Halcyon precursor: it is a complete, shippable environment, and the v1.0-rc.1
> textual fallback (§10/§11 of ROADMAP) *is* the Aurora environment — so the
> highest-risk Halcyon phase is not load-bearing for a beautiful v1.0. Both share the
> substrate: the **Tapestry** present-path, the framebuffer (`simplefb` / virtio-gpu),
> the **Menagerie** drivers (scanout + the trusted-tier USB keyboard), and the
> **trusted path** (`docs/TRUSTED-PATH.md` — a renderer is fully suspended during a
> SAK episode). The boundary between them is the **swappable `/dev/cons` backend**:
> UART-backed on serial/QEMU, Aurora-backed on a graphical board, so Utopia + the
> installer run on either *unchanged*. When Halcyon lands it hosts Aurora terminals as
> windows. Aurora adds **no new §28 invariant** (it inherits T-1 + the I-27 renderer
> obligation).

### 17.1 The compositor / client architecture

- **Tapestry (`tapestryd`) is the compositor / display server** — it owns the GPU (a `CAP_HW_CREATE` userspace driver, the stratumd-as-driver precedent), the surface set, layout, and input routing. **Halcyon is its first client**, as are ported apps (an SDL/Quake surface is just another client). Monolith-vs-server resolved to server+clients: the Plan 9 way (the window system is a file server, §17.2), the robust way (a client crash does not take the screen), and the way that makes placement-transparency fall out.
- **Surfaces are placement-transparent** (TAPESTRY.md D5): a client is handed a surface + input channel and cannot observe whether it is drawn inline, in a split, as a tab, or docked. The three display modes are one mechanism; a running surface re-parents live (D6 — the "live promotion" gift).
- **Two transports, never crossed: present-on-Loom, control-on-9P.** Pixels fly over the Loom ring (present is `LOOM_OP_WRITE`; TAPESTRY.md §5); structure + input are 9P (the `/dev/tapestry` layout tree; §17.2). This is the I-30-pinned, capability-safe path no raw shared-memory compositor can claim.

### 17.2 Kernel surface — still no graphics-specific API

The evolution *strengthens* the original claim: the kernel grows **no graphics-specific API**. Everything Halcyon and `tapestryd` need already exists:

- **Present**: the **Loom** ring (the io_uring-inverted 9P transport) — present is a generic `LOOM_OP_WRITE`; the framebuffer is a zero-copy shared BURROW (Angle #2).
- **Layout + control + introspection**: **9P** — `tapestryd` serves the `/dev/tapestry` tree (uniform containers; per-pane `ctl` / `mode` / `role` / `geometry` / `tag` / `surface` / `input`); `halcyon.rc` is a shell script writing those files (TAPESTRY.md §15).
- **Scanout**: a userspace **virtio-gpu** driver (the deferred scanout half: `CREATE_2D` / `ATTACH_BACKING` / `SET_SCANOUT` / `TRANSFER_TO_HOST_2D` / `RESOURCE_FLUSH`).
- **Input**: **virtio-input** (QEMU) / USB-HID (bare metal, a Lazarus arc) -> `tapestryd` -> the per-pane `input` 9P stream; a reserved Super/Hyper modifier is the compositor-control layer, intercepted above the focused surface's own input (TAPESTRY.md §14).
- **Processes**: `rfork` / `exec` / `pipe` / `wait` — Halcyon spawns commands into panes.
- **Notes**: resize (`winch`), `interrupt` (Ctrl-C).

### 17.3 Agentic enablement (the graphical agentic-loop ABI)

The graphics phase ships a perceive / act / assert API for the coding agent, designed in from the fbcon (TAPESTRY.md §16): structural perception is free (the agent is a 9P client of the `/dev/tapestry` tree, `cat` over the serial console it already drives); visual perception is QEMU `screendump` -> the agent reads the PNG visually (host-side, now) then an in-band per-pane snapshot (later); action is QMP `input-send-event` (+ an in-band inject file later); and the oracle/ground-truth pairing (9P structure vs. captured pixels) makes graphical testing rigorous. This keeps the post-Utopia agent-primary loop alive into the graphical phase, where it would otherwise go blind on pixels. It is a new agentic-loop ABI sibling to the boot-banner ABI (`TOOLING.md` §10, which gains the concrete contract at fbcon-time); the in-band capture / inject files are dev/test-build-only (the #880 strip-for-production class).

### 17.4 Sequencing (pointer)

fbcon = Tapestry stage 0 (the shell on a monitor; late Phase 9; the one graphics piece with a v1.0-rc claim) -> compositor API + SDL / software-Quake as the acceptance gate -> Halcyon (Phase 10) on the proven protocol. Halcyon is pure 2D; OpenGL (Mesa swrast via Pouch) is app-compat only, v1.1+, off Halcyon's critical path. The Tapestry API is QEMU-validatable (axis A); bare-metal output (RPi framebuffer) + input (USB-HID, the long pole) is Lazarus work (axis B) plugging in beneath it. Full detail: TAPESTRY.md §17 + ROADMAP Phase 9/10.

### 17.5 Open design questions

- **RESOLVED (2026-07-17 — the graphics-phase design pass, TAPESTRY.md §18).** The schema (`/dev/tapestry`, V4 — renamed from the provisional `/dev/halcyon`), the present/recycle + resize protocol (`tpresent`; the new-weave resize — never realloc-in-place), the event wire (`tevent`; the synthesized FRAME clock), and the determinism-mode wire (`test-mode` + `TPRESENT_HOLD`, dev/test-build-gated) are BOUND in TAPESTRY.md §18.1-§18.6. T-1 is spec-gated (`specs/tapestry_present.tla`, spec-first RE-ENABLED for this surface — the 6th instance of re-enable point (a)) and takes its §28 number at G-2/G-3; the remaining §28 edits stay reserved-then-enforced along the G-0..G-9 build arc (§18.9).

### 17.6 Summary

Halcyon is the first client of `tapestryd`, the compositor. It presents pixels over Loom and drives layout + input over the `/dev/tapestry` 9P tree; the kernel provides no graphics-specific API (Loom + 9P + virtio-gpu / virtio-input already suffice). The anti-window tiling model (uniform containers, placement-transparent surfaces, layout-as-9P, the Helix-modal transcript, pin = minimize = widget) lives in `docs/TAPESTRY.md` §13-17.

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

- **F3 [P3], reconciliation — the send side is ALL-OR-NOTHING, not block-on-room.** The "sender blocks death-interruptibly if `c2s` lacks room" phrasing above is the design sketch; the **as-built** `srvconn_client_send_frame` writes the whole frame or returns 0 (ring full) → the op fails → `client_run` marks the session dead. `SRVCONN_RING_CAP` (64 KiB since Weft-0; was 8 KiB) holds two `msize` frames, so the v1.0 corvus + joey single-in-flight workload never fills `c2s`; a *deep* burst that genuinely fills it would mark a healthy session dead. Bounded and dormant at v1.0; widened in v1.x by raising `SRVCONN_RING_CAP` or implementing the block-on-room sender. The all-or-nothing rule is load-bearing regardless: a partial frame on the wire desyncs the shared stream, which is worse than a clean session death.

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

### 22.4 First bare metal target: Raspberry Pi 4 / 5 (in v1.0 scope — the Menagerie arc)

**Scope shift (2026-06-15): RPi4/RPi5 bring-up is now in-v1.0 scope**, via the
Menagerie driver framework (§22.7, `docs/MENAGERIE.md`) — not the post-v1.0 sprint
this section originally scoped. The delta from QEMU `virt` (mailbox/clock chain,
the brcmstb PCIe host bridge + MSI, RP1 behind PCIe, GENET/SDHCI/USB, non-coherent
DMA, board identity) is absorbed by the discovery-source/warden model rather than
special-cased — adding a board is adding a BSP entry + drivers, not editing the
boot path. QEMU `virt` is a peer board in the universal image, so the model proves
out there first with zero real-hardware risk. See §22.7 + MENAGERIE.md.

### 22.5 Apple Silicon bare metal (v2.0+)

Per VISION §4.5. Depends on Asahi Linux's m1n1, AIC, AGX. Estimated: substantial effort; v2.0 candidate.

### 22.6 Wall clock and monotonic time (LS-K)

**STATUS**: COMMITTED (LS-K).

Thylacine exposes two clocks through a single POSIX-shaped `SYS_CLOCK_GETTIME(clk_id, timespec)` syscall — the Plan 9 / POSIX split:

- **`CLOCK_MONOTONIC`** — nanoseconds since boot, from the ARM virtual architectural counter (`CNTVCT_EL0`). This already exists kernel-side as `timer_now_ns()` (the tsleep / poll / futex timebase, §12.3); `clock_gettime` merely exposes it. It never goes backward, is unaffected by the wall clock, and is the correct base for measuring intervals. EL0 read access to `CNTVCT_EL0` is already enabled (`CNTKCTL_EL1.EL0VCTEN`, the future-vDSO hook in `timer_enable_el0_counter_access`), so a v1.x vDSO can serve `CLOCK_MONOTONIC` without a trap.

- **`CLOCK_REALTIME`** — nanoseconds since the Unix epoch (1970-01-01 UTC). Derived from a **boot-time RTC anchor**: the kernel reads the real-time clock **once** at boot to get the wall-clock epoch in seconds, snapshots the monotonic counter at the same instant, and thereafter computes `realtime(now) = epoch_anchor_ns + (monotonic_now_ns − monotonic_anchor_ns)`. The fast counter supplies the sub-second resolution and the elapsed delta; the slow RTC is touched exactly once. This is the standard RTC-anchor-plus-counter model and the Plan 9 idiom (`/dev/rtc` seeds the wall clock; `nsec()` advances it).

**The RTC device — PL031 (I-15).** On the QEMU `virt` target the real-time clock is an ARM PrimeCell PL031. It is discovered by the standard DTB path — `dtb_get_compat_reg("arm,pl031", …)` — with the documented QEMU-`virt` fallback base `0x09010000` (exactly the PL011 UART pattern, §22.2 / `arch/arm64/uart.c`). The kernel reads the 32-bit Data Register (`RTCDR`, offset `0x000`), which QEMU loads with the host's Unix time; that single read is the epoch anchor. The PL031 MMIO region is **reserved** (`reserve_compat("arm,pl031")` in `kernel/mmio_handle.c`) so a `CAP_HW_CREATE` userspace driver cannot claim the RTC slot (**I-5**) — though the kernel holds no live behaviour there after the boot read.

**Fail-soft.** If no PL031 is present in the DTB and the fallback read is implausible (`0`), the kernel anchors `epoch_anchor` to `0`, so `CLOCK_REALTIME` reads `1970-01-01 + uptime` — the honest "no wall-clock source" signal, never a fabricated plausible-but-wrong time. `CLOCK_MONOTONIC` is always correct. The boot path never extincts on a missing or odd RTC.

**No new invariant.** LS-K introduces no §28 invariant: it is a read-only introspection surface (no privilege, no mutation, no lifetime), covered by the existing **I-15** (the PL031 view derives entirely from the DTB) and **I-5** (the RTC MMIO slot is kernel-reserved). The implementation obligations are ordinary correctness — uaccess fault routing (`-EFAULT`), a bad `clk_id` (`-EINVAL`), and no overflow in the `epoch·1e9 + delta` arithmetic (an epoch of ~`1.7e18` ns stays well inside `i64`).

**The identity syscalls.** Alongside the clock, LS-K adds `SYS_GETPID` / `SYS_GETUID` / `SYS_GETGID` — trivial read-only returns of the calling Proc's `pid` / `principal_id` / `primary_gid` (all already durable Proc fields, A-1a). They carry no capability and mutate nothing.

**Setting the wall clock (net-7a).** The LS-K settability seam is closed by `SYS_CLOCK_SETTIME(clk_id, timespec)` (`= 79`), pulled forward into the network arc so SNTP can actually synchronize (NET-DESIGN §10). Only `CLOCK_REALTIME` is settable (`CLOCK_MONOTONIC` is the boot-counter timebase — `-EINVAL`). The "who may set the clock" capability is **`CAP_HOSTOWNER`** — a clock step is system-global, so it is the host owner's authority, never an identity's (I-22; `-EACCES` otherwise). It re-anchors the **single** wall-clock offset (`g_wallclock_offset_ns`, NOT a two-field anchor) at full-nanosecond granularity via one atomic `u64` store: `CLOCK_REALTIME = timer_now_ns() + offset`, so a runtime re-anchor races GETTIME readers only on a single aligned `u64` — each reads old-or-new, coherent, no seqlock (the LS-K single-`u64` design is what makes the setter SMP-safe). `CLOCK_MONOTONIC` is untouched. The handler validates `clk_id` + the cap before any buffer read, bounds `tv_sec` so `tv_sec·1e9 + tv_nsec` cannot overflow, and routes a bad VA to `-EFAULT`. A non-elevated tool gets `-EACCES`; the SNTP client (net-7a-2) is the consumer, and an admin steps the clock through it.

**v1.x seams** (recorded, not built):
- **Userspace timekeeper / continuous NTP discipline** — `SYS_CLOCK_SETTIME` (net-7a) gives the step primitive; a long-running timekeeper that *slews* (gradual `adjtime`-style correction) rather than steps, and the Fuchsia/Genode userspace-maintained-UTC-clock-object SOTA, are the v1.x refinement. v1.0 is step-on-demand via SNTP.
- **vDSO `clock_gettime`** — **now being built** (user-chosen 2026-06-25; binding design `docs/VDSO-DESIGN.md`, no longer a seam). Userspace reads `CNTVCT_EL0` directly (already EL0-enabled) for `CLOCK_MONOTONIC`, plus a shared read-only kernel `vvar`-style page (`struct vdso_clock` = `{magic, version, freq, wall_offset_ns}`, `thylacine/vdso.h`, discovered via the `AT_VDSO_CLOCK` auxv tag) for the `CLOCK_REALTIME` anchor, eliding the trap (the §4.5 per-syscall-cost backlog, #62). Driver: #343's measurement showed the on-device Go toolchain spends its wall time in `nanotime()` → ~740M `SYS_CLOCK_GETTIME` calls (Go's scheduler `findRunnable` / sysmon), which a vDSO removes. **No code page** (EL0 reads `CNTVCT` itself, so no `__kernel_clock_gettime` / ELF resolution — only the Linux `vvar` half) and **no seqlock** (freq is write-once; `wall_offset_ns` is the single atomic `u64` LS-K already relies on). A reader whose magic/version mismatches falls back to `SYS_CLOCK_GETTIME`.
- **64-bit RTC / Y2106** — the PL031 `RTCDR` is 32-bit (wraps in 2106). A 64-bit RTC, or NTP-maintained UTC, removes the limit.
- **Name resolution** — `whoami` / `id` render the numeric `principal_id` / `primary_gid` at v1.0; the uid→name map (a corvus `NAME_LOOKUP` verb or a kernel `principal_name` stamped at `CAP_SET_IDENTITY`) is the v1.x enhancement. `getgroups` (the supplementary-group set for a complete `id`) is the same class — the field exists (`supp_gids`); the syscall is the seam.
- **The pouch boundary-line** — a ported (musl) program's `getpid` / `getuid` / `clock_gettime` map onto these syscall numbers via `usr/lib/pouch/patches/*` when a port needs them (v1.x; LS-K's consumers are native libthyla-rs coreutils).

### 22.7 The driver framework (Menagerie)

Canonical: `docs/MENAGERIE.md`. The **binding layer** into which drivers drop, so
that supporting new hardware is *adding a driver*, not *editing the boot path*. It
realizes ROADMAP §3.5 (no in-kernel drivers, bounded exceptions) for real hardware
and unlocks the RPi4/RPi5 track (§22.4).

The model: pluggable **discovery sources** (DTB / PCIe / USB / SDIO-MMC /
overlay-EEPROM) reduce all hardware — static on-die fabric and self-enumerating
buses alike — to one stream of `{ DeviceAdded(node) | DeviceRemoved(node) }`. A
single trusted broker, the **warden** (a native `libthyla-rs` Proc in the TCB,
spawned by joey), matches each node's **identity** (the `compatible` strings for
DTB nodes, or a bus identity — e.g. a virtio device-id, a PCIe `vid:did` — for
bus-enumerated nodes; a bus source reads the runtime identity and re-emits **typed**
child nodes, so the warden binds by id and never reads a device register itself)
against a bind DB, computes a **narrowed hardware allowance** (the node's
resources ∩ the driver manifest's declared needs), and spawns the driver as an
isolated, capability-sandboxed userspace Proc that serves a file into the namespace. The model is recursive — a
bound bus driver *becomes* a discovery source — so the entire RPi5 bring-up (RP1
behind PCIe) falls out of "a driver can be a source." Deferred probe is
wait-on-a-file in the namespace; teardown (`DeviceRemoved` / crash) is the I-25
group-terminate; supervision restarts a crasher.

**The one kernel lift** is the **hardware allowance** (§4 of MENAGERIE.md;
**reserves I-34**, §28): `CAP_HW_CREATE` is coarse today (a flat gate at the three
`SYS_MMIO/IRQ/DMA_CREATE` handlers); the lift scopes it per-Proc to a bounded
resource set, checked at those gates, so a driver can mint a handle *only* within
its device's allowance. It preserves I-5 (handles stay created-in-Proc +
non-transferable — we pass down the bounded *authority to create*, not pre-minted
handles), generalizes pci-1b (a PCI device's allowance *is* its claimed BARs), and
makes every grant auditable (a small explicit set, not "anything unreserved"). The
kernel also gains `devhw` (the DTB published to userspace as a walkable tree — the
I-15 enforcement point) and a Loom **device-gone** terminal CQE for
surprise-removal. Audit-bearing — see §25.4.

**Ratified postures (2026-06-15):** third-party driver authorization is
**convenience** (try-bind, ask once on the trusted path, remember; paranoid
lockdown is an opt-in hostowner toggle); the **board self-identifies** from the DTB
root `compatible` (never a user prompt); a **universal image** carries multi-board
support and the DTB selects at boot (the pool is portable, the board is read fresh
each boot). Third-party drivers as capability-sandboxed Procs are a NOVEL position
(NOVEL.md) — a stable driver ABI becomes *desirable* precisely because a driver's
blast radius is its own device, which a monolithic kernel structurally cannot
offer.

### 22.8 Open design questions

None at Gate 3.

### 22.9 Summary

DTB-driven hardware discovery; the Menagerie driver framework (§22.7) binds
discovered devices to capability-sandboxed userspace driver Procs (the hardware
allowance, I-34); platform layers under `arch/arm64/<platform>/`; RPi4/RPi5 in v1.0
scope via Menagerie (§22.4). Wall clock + monotonic time via the PL031 RTC
boot-anchor (§22.6).

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

`futex` is an audit-trigger surface. The wait/wake atomicity invariant is subtle. As-built (`torpor`, Phase 6) it is prose-validated per the 2026-05-23 spec-to-code suspension — no `futex.tla` module was written; the reasoning lives in `kernel/torpor.c` + `torpor.h` + the focused audit, and `death_wake.tla` covers the death-wake interaction.

### 23.5 Pseudo-terminals (PTY)

Required for: Halcyon subprocess hosting, `ssh`, `tmux`, `vim`, any program that checks `isatty()` and changes behavior accordingly.

**Model**: a PTY is a 9P server managing a master/slave fd pair with `termios` semantics layered on a pipe-like channel. **The full design LANDED 2026-07-17 — `docs/PTY-DESIGN.md` is authoritative** (the Fable design pass; four votes: a USERSPACE 9P server `ptyfs` [the Plan 9 / Fuchsia-ptysvc / Genode / Hurd grain + ARCH's own sketch]; full POSIX sessions + process groups; Ctrl-Z generalizes the audited debug stopped-state [I-39]; spec-first RE-ENABLED for I-20, `specs/pty.tla` model-first). The security-sensitive signal routing (session/pgrp/controlling-terminal + note-to-pgrp) is kernel; the byte/cooking engine is the userspace server, whose sole signal authority is a pts-scoped `SYS_TTY_SIGNAL` (it can never name a pgrp — the I-1/I-22 bound).

**`/dev/ptmx`**: opening this allocates a new PTY master fd and creates a corresponding slave entry under `/dev/pts/<n>`.

**`/dev/pts/<n>`**: the slave end. Passed to the child process as its controlling terminal. Presents a full `termios` interface.

#### 23.5.1 The LS-8 v1.0 split (single-console line discipline; the master/slave pair is Phase 8)

Task #952 (LS-8) builds the **single physical console's** line discipline and pollability. The PTY master/slave *pair* above (`/dev/ptmx` + `/dev/pts/<n>`, per-fd `termios`, controlling terminals, process groups, Ctrl-Z) is **Phase-8** — that is I-20's surface, and `specs/pty.tla` lands with it. LS-8 splits in three:

**LS-8a — pollable cons (the deferred poll-wake).** `/dev/cons` gains a `.poll` hook so the shell can wait on the console *and* its notes fd at once (LS-8c). The RX interrupt (`cons_rx_input`) runs in IRQ context and can only call `wakeup()` on a `Rendez` (IRQ-safe, via `spin_lock_irqsave` on the global timer-wait lock); the poll-hook walk (`poll_waiter_list_wake`) takes a plain non-irqsave lock and nests a `wakeup` inside it, so it **cannot** run from IRQ context. So the IRQ sets a `poll_wake_pending` flag under `g_cons.lock` and wakes the existing `console_mgr` kthread, which drains the flag and walks the hook list in *process* context — exactly Linux's tty model (the hard IRQ buffers the byte and schedules `flush_to_ldisc` work; the line-discipline cooking + the read/poll wakeups run in that work item). This **deferred relay** is the one genuinely-new mechanism, and it is **spec-first** (re-enabled for this surface, 2026-06-12): `specs/cons_poll.tla` pins that the relay loses no wakeup — **I-9 generalized** across the IRQ→mgr→hook-list chain, with register-then-observe at *both* the mgr's sleep on `poll_wake_pending` (the `sleep(&mgr_rendez, cons_mgr_pending)` contract) and the poller's `.poll` hook install. `BUGGY_MGR_LOST_WAKE` is the executable counterexample: the mgr's go-to-sleep done as a hand-rolled check-then-sleep instead of register-then-observe drops a flag set in the window and strands a poller asleep on a ready console. Filed under **I-9**, distinct from I-20 (the Phase-8 master/slave atomicity, whose `pty.tla` is still unwritten).

**LS-8b — termios via `/dev/consctl` (fine-grained, POSIX-shaped).** The kernel-side line discipline (the cooking lives in the cons layer beside the kernel-owned console — required for the I-27 trusted path; not a userspace `consd`) is toggled by stty-style writes to `/dev/consctl`. **Five independent flags** (granularity B, user-voted 2026-06-12 over the Plan 9 coarse `rawon/rawoff`):

| Flag | When set (cooked default) | When clear |
|---|---|---|
| `ICANON` | line-buffer until Enter; backspace erases | char-at-a-time (raw input) |
| `ECHO` | typed chars echoed to console output | no echo (the enforced password mask) |
| `ISIG` | `Ctrl-C` → the `interrupt` note (§8.8.2, LS-5) | a literal `0x03` byte to the reader |
| `ICRNL` | CR→NL on input | CR passed through |
| `ONLCR` | NL→CRNL on output | NL passed through |

Independent bits make `cbreak` (`-icanon +isig`) representable and let the Phase-8 Pouch boundary-line map `tcsetattr(struct termios)` onto the flags 1:1. `ECHO`-off is a **hard kernel guarantee** — no input byte reaches the console output when `ECHO` is clear (no leak via the cooked-mode erase/redraw), the enforced mask that supersedes LS-6's hand-rolled interim. The termios state is **global to the one v1.0 console**; per-fd `termios` needs `/dev/pts` (Phase 8). The `/dev/consctl` open stays console-attach-gated (#57b). The icrnl/onlcr-independence and the cbreak path have no native v1.0 consumer (unit-tested, not driven, until a Pouch program exercises them) — the accepted speculative surface of granularity B, paid for the 1:1 Pouch mapping.

Mapping for the Phase-8 Pouch port: `tcgetattr()`/`tcsetattr()` → a structured read/write the boundary-line decomposes into the per-flag consctl writes; `tcgetattr` reads the current flags back. This covers `vim`, `less`, `bash` readline, `ssh` client when they arrive via Pouch.

**LS-8c — the shell multi-fd poll loop** (userspace; rides LS-8a): `ut` polls cons + the notes fd → async `[N]+ Done` job reaping while idle at the prompt + reactive Ctrl-C mid-edit.

`pty` (the I-20 master/slave) remains an audit-trigger surface for **Phase 8**; `specs/pty.tla` lands with that server. LS-8's line discipline is audit-bearing now (§25.4): the deferred poll-wake (`cons_poll.tla` + the focused audit) and the termios cooking (prose + unit-tested per the 2026-05-23 suspension).

**As-built (LS-8b kernel mechanism, landed):** the five-flag termios + the cooking in `cons_rx_input` (IRQ context — `uart_putc` is lock-free, so echo staged under `g_cons.lock` + emitted after release) + the `/dev/consctl` `+name`/`-name` parse/render (`cons_set_mode_cmd`/`cons_render_mode`, atomic multi-flag, symmetric read-back) + the ECHO-off hard guarantee, all under the boot default `CONS_ISIG`-only (== the pre-LS-8b behavior → breaks nothing). 9 unit tests (the cooking truth table + the ECHO-off no-output property + the consctl parse/render + the bounded line buffer). **Owed — the login-echo consumer (LS-6 fold-in) is deferred pending one decision:** login is the session leader that must set cooked/no-echo for its prompts, but it is **not** console-attached (it reads the console via an inherited `SYS_CONSOLE_OPEN` fd), so it cannot pass the `/dev/consctl` open gate. Resolution options, both sound: **(B)** relax the consctl I/O re-gate (the O_PATH bypass is already closed by `CWALKONLY`/#81, so the open-attach-gate + an inherited consctl handle from the getty suffices) — a surgical I-27-surface change; or **(C)** add a capability-keyed `SYS_CONSOLE_MODE(fd)` operating on the held console fd (capability-clean but deviates from the "consctl-file, not ioctl" decision above). `ut` (LS-8c) drives cooked mode for itself without the gate question (it is the console *owner* + sets mode for foreground children). The LS-8b kernel mechanism is complete + unit-tested independent of the consumer-gate decision.

#### 23.5.2 Console write atomicity + the P1-F buffered TX path (#75)

**The defect (#75 — pre-existing since P4-B, not merge-introduced).** `cons_output_write` loops byte-by-byte into `cons_emit` → `uart_putc` with **no lock held across the loop**: `g_uart_rx_lock` guards only the RX FIFO drain, and `uart_putc` is itself lock-free — a fact §23.5.1's LS-8b as-built note already records (it is *why* echo is staged under `g_cons.lock` and emitted after release), without ever drawing this consequence. Two CPUs inside `cons_output_write` therefore interleave at **byte** granularity, with `uart_putc`'s bounded TXFF spin (#67) as the natural interleave point. Observed: a `jc-probe` line and `ut`'s prompt shredded into each other 1–2 bytes at a time, tearing the 3-byte `⊢` (U+22A2, `e2 8a a2`) and an SGR colour escape, in 10 of 40 gate boots across *both* default+UBSan and *both* smp4+smp8. That orphaned continuation byte is the invalid UTF-8 that made BSD `grep` classify the gate log binary (#74). The history is unambiguous: `cons_output_write` is byte-identical at `2b7842ef` and at the gfx-2 merge, and none of its three commits (`6f417e93` P4-B / `71d52541` #57b / `966d9341` LS-8b) ever held a lock — **lockless since the console Dev was written in Phase 4**.

**The asymmetry that is the tell.** Console *input* is serialized (`g_cons.lock`, one shared line buffer). Console *output* is serialized nowhere.

**The blast radius is not the log.** `cons_drain_tap` (G-4) appends per byte under its own lock — each byte atomic, but in the *interleaved* order — so aurora's drain ring receives exactly the torn stream the serial log does. A torn SGR is a wrong colour; the same tearing on a cursor-positioning or alt-screen escape visibly corrupts a live TUI (`nora` / Kaua / `ut`). It is a user-visible failure mode, which is why the honest resolution is serialization rather than consumer tolerance.

**The contract (decided 2026-07-20, user vote).** A single `write()` to `/dev/cons` is **atomic with respect to other `/dev/cons` writers**: its bytes appear contiguously in the output stream. This closes a gap where ARCH honored POSIX write-atomicity for pipes (`PIPE_BUF`, §16) but said nothing for the console. POSIX itself guarantees atomicity only for `<= PIPE_BUF` pipe writes, *not* terminals — we take the stronger Linux tty position (`atomic_write_lock` wraps the whole write) because a torn escape corrupts terminal *state*, not merely log text.

**Why a lock alone cannot deliver it — and what does.** Linux can hold `atomic_write_lock` across a whole tty write because it is a **sleepable mutex** and the TX wait is a **sleep**. Thylacine's TX wait is a bounded **spin** (`uart_putc`: 20 ms / 2^24 iters, #67) and `cons_emit` is reachable from **IRQ context** (the echo path), so that shape is unavailable: a lock across the loop would be held across up to `SYS_RW_MAX` (128 KiB) of spinning, irqsave, with preemption disabled (#360) — pinning the CPU. The missing piece is the one `arch/arm64/uart.c` already names: *"P1-F (IRQ-driven TX with a buffer) was never built, so this IS the live TX path."*

So the design is **P1-F first, then serialize**:

1. **A bounded TX ring + the PL011 TX interrupt.** `cons_output_write` cooks (ONLCR) and pushes into a ring under an irqsave lock — an O(n) copy, no spin — then kicks `TXIM`. The UART IRQ (today `uart_rx_handler`; becoming a `MIS`-dispatching `uart_irq_handler`) drains ring → FIFO while `!TXFF`, clears `TXIM` when the ring empties, and wakes room-waiters. This removes the 20 ms spin from the console write path as a side effect.
2. **A writer role makes the multi-segment case call-atomic.** A write exceeding the ring's free space must sleep for room, dropping the lock — so the call holds a **writer role** across its whole duration, exactly `srvconn.c::chan_role_acquire` (#354 / CF-3 B): a second writer **parks** on a `poll_waiter_list` instead of interleaving, with register-then-observe under the ring lock (**I-9**; `poll.tla` NoStaleHook) and a `TSLEEP_INTR` unwind (#811). This is legal because `cons_output_write` runs in **process context** — `spoor_write_common` calls `dev->write` with no lock held and explicitly documents it as *blocking*. A **reuse of an audited pattern**, not a new mechanism.
3. **The #67 loss discipline is inherited, not weakened.** A stalled host consumer stops the TX IRQ, so the room-wait is a **deadlined** `tsleep`; on timeout the writer DROPS the remaining bytes and returns a short write. "A bounded-but-lossy console is strictly sounder than a wedged CPU" (#67) applies unchanged — the ring must never convert a stalled *consumer* into a wedged *writer*.
4. **Halls / extinction bypass the ring.** The crash dump runs IRQ-masked on a dying machine and cannot depend on an IRQ to drain, so `extinction` / `halls_*` keep the direct bounded `uart_putc` (HX-I discipline unchanged), preceded by a bounded synchronous ring flush so pre-crash output is not lost.
5. **Arming.** The ring path arms only once the UART IRQ is attached; every pre-GIC boot print takes the direct path. The `Thylacine boot OK` / `EXTINCTION:` tooling ABI (TOOLING.md §10) is byte-unchanged on both paths.

**The delivered guarantee, precisely (the #75-audit F1 correction — do not over-read it).** The contract is atomicity **against other `cons_output_write` writers** — the writer role serializes two program writes so their bytes never interleave, which is exactly the observed #75 bug (`jc-probe` vs the `ut` prompt SGR, #74's gate trigger). It is **not** a general "no byte ever lands mid-write" guarantee, because the impl pushes one byte per `g_cons_tx.lock` acquire/release. An earlier draft of this section claimed a ring-fitting write's "single lock hold excludes IRQ echo entirely"; the impl does not do that (there is no bulk push), so the claim was withdrawn. What actually holds:

- **Echo (IRQ context, one byte at a time via `cons_emit`) may interpose on a program write's byte stream** — in cooked `CONS_ECHO` mode, a typed-and-echoed keystroke can land between two bytes of a program's glyph on the wire. This is **unchanged from pre-#75** (both echo and program output already went byte-by-byte through the lock-free `uart_putc`), so #75 does **not regress** it; it simply does not close it. v1.0 reachability is narrow: `ut` runs raw (it echoes itself, `CONS_ECHO` off), a login passphrase is echo-off, so the common shells don't hit it. **Full echo-exclusion via a bulk-push fast path** (push a ring-fitting write's whole cooked run under one lock hold, so echo is excluded on the serial ring too) is a documented **v1.x enhancement (#79)** — it carries a two-ring (serial + the G-4 aurora drain) lock-ordering design that is not worth rushing onto the trusted-path surface for a non-regressed, narrow case.
- **`SYS_PUTS` (`t_puts`/`t_putstr`) is an EL0-reachable diagnostic path that bypasses the ring + role** (it writes `uart_putc` directly — the always-on diagnostic UART channel). A program mixing `SYS_PUTS` with `/dev/cons` writes, or two programs one on each path, can interleave at the FIFO. This is deliberate (the diagnostic channel must work independent of the console Dev) and pre-existing; it is documented here rather than closed.
- **A debug / job-control stop of a thread parked mid-`cons_output_write` holds the writer role for the stop's duration**, so other console writers park on `g_cons_tx.role_waiters` until it resumes (safety is clean — the role releases on resume or death; contenders park, never spin or extinct). This is a new liveness seam (the role did not exist before #75), the console analog of the #89/8c-3 elected-9P-reader freeze; it is bounded to one write call and accepted as a **v1.x** release-on-stop item, not merge-blocking.
- **Kernel prints (`uart_puts`) keep the direct path**, so a kernel print can still interleave with an EL0 write. They must work pre-GIC, in IRQ context, and inside extinction, where the ring is unavailable by construction. Post-boot kernel prints are diagnostic and rare; tightening this is a v1.x item, not a #75 requirement.

**Invariant status.** **No new §28 invariant** — the contract is a `Dev.write` semantic, and the two new park/wake legs (the writer role; the room-wait) are **I-9** obligations discharged by the same register-then-observe discipline the #354 role-park and `cons_poll.tla` already carry. **No new spec** per the 2026-05-23 broadening: the role-park is the audited #354 shape (prose + audit there too) and `cons_poll.tla`'s deferred-wake relay is untouched. Audit-bearing per §25.4 (the LS-8 row's P1-F addendum) — a new IRQ path on the console is the I-27 trusted-path surface.

#### 23.5.3 Console winsize + `tty:winch` — the #55 resize contract (user-voted 2026-07-22)

The console gains the pts winsize contract: §23.5.1's LS-8 line discipline carried the five termios flags but no geometry, while ptyfs has carried both since PTY-2c. One kernel-held size, one writer, one signal, two read paths.

**The state.** `cons.c` holds one console winsize (`cols`, `rows` — the Linux unsigned-short band; `0×0` = never set) under `g_cons.lock` beside the five mode flags. There is exactly one console at v1.0, so the state is global like the termios word.

**The writer is the renderer.** The console's geometry is PHYSICAL — it is aurora's cell grid (`w/cell_w × h/cell_h`), reported not negotiated. Aurora writes `winsize <cols> <rows>` to `/dev/consctl` (the ptyfs PTY-2c verb, byte-identical grammar) at first present and on every reweave (AURORA.md §4). The verb joins `cons_set_mode_cmd`'s grammar; `cons_render_mode` appends `winsize <cols> <rows>` to the mode line (parser parity: pouch 0021's `strstr(buf, "winsize ")` works on either ctl).

**The mint-gate widening (the I-27 argument).** consctl's `devdev_open` gate becomes attached-OR-renderer (`cons`, the DATA leaf, is UNCHANGED: attach-gated at open + per-I/O re-gated). Aurora self-serves by name exactly as it opens consdrain/consfeed. **The renderer-minted consctl is WINSIZE-VERB-ONLY** (the `CCONSWINSZONLY` Spoor tag; `cons_set_mode_cmd`'s `allow_flags=false` rejects a `+`/`-` flag token on it — the #55 audit F2 fix). The authority argument, corrected: the renderer already holds **consfeed** — arbitrary INPUT INJECTION (it can type anything the renderer feeds) — which dominates a *winsize report* (pure geometry, zero input-domain authority), but it does **not** dominate a termios flip on the **global** cooking word, which also governs the **serial** RX path (`cons_rx_input` reads the same `tio`) — so a compromised renderer with the full five-flag grammar could write `+echo` to unmask a concurrent serial-typed password into the drain it reads, defeating the LS-8b ECHO-off HARD guarantee. Restricting the renderer to the winsize verb closes that: winsize confers no input-domain authority the renderer's consfeed does not already have, so the widening is sound. The *attached* chain (login/ut's inherited consctl, #94-B) is UNMARKED → full grammar (it legitimately sets `-echo` for the password mask). consctl I/O stays ungated per #94-B (the inherited fd is the capability); an app cannot mint consctl, so a TIOCSWINSZ-shaped lie from an app is structurally blocked — see the pouch arm below.

**The signal.** On a CHANGED winsize (the Linux TIOCSWINSZ / ptyfs iff-changed semantics), the kernel posts `tty:winch` (`NOTE_NAME_TTY_WINCH` — the PTY-1b kernel-only-POST, catchable, informational class; I-19 untouched) to the console OWNER's PGRP via `notes_post_pgrp(owner->pgid)`. Why the pgrp and not the owner Proc: the LS-5 interrupt-to-owner routing works because `ut` FORWARDS `interrupt` to its children — but userspace cannot post `tty:*` (the PTY-1b F4 gate), so owner-only winch would strand every child; the pgrp is the minimal correct set (children share ut's rfork-inherited pgid — the console has no job control, ut never setpgids). The console does NOT participate in the PTY-1 controlling-terminal model (`ct_sid` is pts-registry state), so there is no console `fg_pgid` to prefer; when the console joins the ct model, the target refines to the fg pgrp (recorded seam). Lock discipline: the verb applies under `g_cons.lock`; the changed-flag is captured and the post runs AFTER the lock drops (no `g_cons.lock` → `g_proc_table_lock` edge). This is process context — a consctl write — so no `console_mgr` deferral is needed, unlike the ISIG cook which runs in RX IRQ context; the winsize path is NEVER reachable from `cons_rx_input`.

**Read path 1 — `/dev/winsize` (native + the pouch bridge).** A new UNGATED read-only devdev trivial leaf rendering `winsize <cols> <rows>\n` (the exact ctl fragment). Geometry is not sensitive (two u16s; no I-13/I-16 content), so it joins the null/zero/full class. Unset renders `winsize 0 0` — the serial posture: on a pure-serial boot no renderer exists, nobody writes, and apps fall back to the CPR probe (which the HOST terminal answers on serial). Never an error.

**Read path 2 — CPR (unchanged).** The in-band `ESC[6n` probe stays the universal fallback and the only path that works on serial. The deterministic client rule: read `/dev/winsize`; if `0 0`, CPR.

**The is-a-cons qid contract (a pulled-forward dependency).** Pouch 0021's tty dispatcher discriminates by fstat (S_IFCHR + qid decode), and cons fds are STATLESS today (fstat −1 → folded to ENOTTY → `isatty()` on the console is FALSE for pouch apps — a latent POSIX-compat defect: musl stdio runs fully-buffered on the console). #55's TIOCGWINSZ arm needs the discriminator regardless, so both land together: `devcons` + the devdev `cons` leaf gain `.stat_native` (one shared `cons_stat_native` — zero-fill per I-13, the S_IFCHR posture ptyfs already presents, SYSTEM-owned, `devno` per #100) reporting a CONS qid marker bit DISJOINT from ptyfs's `PTS_FLAG` under the shared S_IFCHR posture (the ptsname-qid documented-client-ABI precedent). 0021's cons arm: TIOCGWINSZ → open+read+parse `/dev/winsize` (stateless per-op, the pts-ctl idiom) → success even at `0×0` (isatty truth restored; musl stdio goes line-buffered on the console exactly as on a pts); TIOCSWINSZ → EPERM (geometry is renderer-owned; Linux-VT-resize semantics are NOT offered); tcgetattr/tcsetattr on a cons fd stay ENOTTY (the console termios is held by the consctl delegation chain — an honest documented seam).

**Consumers.** Kaua/native apps (nora, ut's Repl) poll their notes fd alongside stdin; on `tty:winch` → read `/dev/winsize` (CPR if `0 0`) → relayout. Pouch apps: SIGWINCH via the landed PTY-3 winch→NCONT mapping + TIOCGWINSZ per above — nothing new below the 0021 arm. Aurora's reweave (the G-6b client resize via libtapestry `handle_configure` — ack + generation swap + `Vt` resize) is AURORA.md §4.

**Invariant status.** NO new §28 invariant: composes **I-27** (the mint-gate widening per the dominance argument above), **I-19** (the `tty:*` class + kernel-only-POST unchanged; the poster is cons.c), **I-9** (no new park/wake — the post is a plain notes post in process context). NO new spec (`cons_poll.tla` untouched: winsize is state + a post, not a wait/wake protocol; the verb parse is a data transform like the five flags). Audit-bearing per §25.4 (the LS-8 row's #55 addendum).

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

> **Reconciled 2026-06-11 (HOLOTYPE RW-12 W6-F1/F2/F8 + the RW-13 re-plan).**
> This list predates the LS arc; three items below are NOT Phase-7 (Utopia)
> must-haves and have moved: **the editor is LS-7 (native `nora`)**, not the
> Helix-via-Pouch port (Helix-via-Pouch is an optional Phase-8 port); **the
> PTY master/slave server (`/dev/ptmx` + `/dev/pts/`) is Phase 8**, while LS-8
> delivers single-console termios via `/dev/consctl` (the Utopia-completion
> piece); **`/tmp`,`/run` tmpfs is Phase 8** (it pairs with the container
> runner; `/home` is the v1.0 workaround). Authoritative LS scope:
> `docs/LIFE-SUPPORT.md`; the forward re-plan: `docs/holotype/13-consolidation.md`.

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

### 23.12 Kaua — the native console TUI substrate (LS-7)

The console presentation layer: a native (`no_std` + `alloc`, libthyla-rs) immediate-mode + double-buffered cell-diff TUI library (`usr/lib/kaua`) over `/dev/cons` + `/dev/consctl`, themed with the *Bonfire* palette (`UTOPIA-VISUAL.md`); and its first consumer, the `nora` editor (`usr/nora`). The ratatui *model* brought native, with Plan-9 device discipline (cons/consctl, not termios) and the #94-B-b controlling-terminal capability handshake — the **ut raw-mode dance**: `ut`, which owns the consctl fd privately, sets raw before spawning a TUI child and restores cooked + the screen on its exit/death (the crash backstop). The editor is never console-attached and never holds consctl, so the SAK/elevation gate (I-27) is untouched. Canonical scripture: **`docs/KAUA.md`**. Audit-bearing surface (§25.4): the cons/consctl backend + the dance (I-27); the buffer/widget/layout layers are pure userspace. **No new §28 invariant** — Kaua consumes I-27 + I-9 (the LS-8a pollable cons via LS-8c) + the LS-8b consctl mechanism. It is the **text** member of the weave family (`Loom` the instrument → `Tapestry` the graphics weave, `docs/TAPESTRY.md` → Kaua the text presentation), standing *outside* the Loom-woven names because a v1.0 console has no verified need for Loom (not 9P-backed; human-speed; LS-8c poll suffices); the reserved name `Weft` is earned only when a console substrate genuinely weaves on Loom. Named for the Kauaʻi ʻōʻō, the last of family Mohoidae.

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
- Page tables (`arch/arm64/mmu.c`) + the VMA/Burrow layer (`kernel/vma.c`, `kernel/burrow.c`).
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

### 25.2 The spec inventory

The Phase-0 plan gate-tied nine specs. As-built (RW-10 reconcile 2026-06-11;
table re-reconciled +`allowance`/`net_poll`/`weft` 2026-06-20;
+`net_poll_teardown` @#294 2026-06-21; +`sched_tickless` @TI-spec 2026-06-21;
+`sched_rebalance` @TI-4a 2026-06-21; +`fs_cache` @L1b 2026-07-09 +
`debug_stop` @8a-1a 2026-07-14, both rows reconciled here 2026-07-15)
the committed inventory is **28 modules**; three of the planned nine
(`futex.tla`, `notes.tla`, `pty.tla`) were dropped per the 2026-05-23
spec-to-code suspension — their surfaces are prose-validated (torpor / notes)
or not yet built (the PTY *master/slave* pair, Phase 8) — and seventeen modules were
added beyond the plan. LS-8a's pollable-console *deferred* poll-wake DID get a
spec (`cons_poll.tla`, spec-first re-enabled for that surface, 2026-06-12),
filed under I-9 — distinct from the dropped `pty.tla`, which stays reserved for
the Phase-8 PTY master/slave atomicity (I-20).

**Reserved (spec-first re-enabled, owed at the surface's build):**
`specs/mandate.tla` — pins I-35 (mandate attenuation + revocation,
`docs/MANDATE-DESIGN.md`); the central hazard is the rotate-vs-install race
(a domain `key_gen` bump concurrent with a login-time redemption installing a
now-stale mandate — the ASID-rollover / death-wake class). Written + TLC-green
before the MA-1 impl (the post-net Imperium/Authority arc).

| Spec | Landed | Pins |
|---|---|---|
| `specs/scheduler.tla` | P2 | Wait/wake atomicity (I-9), IPI ordering (I-18), steal no-double-enqueue, eventual-progress liveness (NOT the quantitative I-17 bound) |
| `specs/territory.tla` | P2 | Bind cycle-freedom (I-3), isolation (I-1), mount-refcount consistency |
| `specs/handles.tla` | P2 | Rights ceiling (I-6), transfer-only-via-9P (I-4), hw non-transferability (I-5), caps ceiling (I-2) |
| `specs/burrow.tla` | P2/P3 | Dual refcount + mapping lifecycle (I-7) |
| `specs/9p_client.tla` | P4/P5 | Tag uniqueness (I-10), fid lifecycle (I-11), flow control |
| `specs/poll.tla` | P5 | Missed-wakeup-freedom across N fds (I-9) |
| `specs/pipe.tla` | P5 | Pipe two-direction wait/wake (I-9 specialized) |
| `specs/tsleep.tla` | P5 | Deadline-bounded Rendez sleep |
| `specs/corvus.tla` | P5 | Key-agent session/identity/elevation protocol |
| `specs/sched_ctxsw.tla` | P5 | Uniform-EL1h kernel (I-21) |
| `specs/sched_oncpu.tla` | deep-smp-review | Diagnostic — reproduces the #860 class |
| `specs/sched_alpha.tla` | deep-smp-review | The SMP redesign gating model (I-21 migration safety) |
| `specs/asid.tla` | RW-1 | ASID generation-rollover safety (I-31) |
| `specs/death_wake.tla` | RW-2 | Death-wake cascade (I-9 generalized + I-24) |
| `specs/loom.tla` | Loom-1 | Completion integrity (I-29), submit-time pin (I-30) |
| `specs/loom_multishot.tla` | Loom-5 | I-29 generalized to a CQE stream |
| `specs/loom_order.tla` | Loom-5 | LINK/DRAIN ordering + cancellation completeness |
| `specs/loom_devgone.tla` | Menagerie step 4 | I-29 device-gone terminal: a session-death CQE faithfully reflects its reason (device-gone vs transport), completes every in-flight op (no hang), exactly once |
| `specs/cons_poll.tla` | P7-LS-8 | I-9 across the IRQ->console_mgr->poll-hook *deferred* wake (LS-8a; the Linux-tty `flush_to_ldisc` relay) |
| `specs/allowance.tla` | Menagerie build-arc 2 | Hardware allowance / driver-authority bound (I-34): handles within the live allowance incl. the revoke-vs-create race; never widened; fully revoked on teardown |
| `specs/net_poll.tla` | net-6b | I-9 across the elicited (PROBE-then-observe) `dev9p.poll` readiness relay |
| `specs/net_poll_teardown.tla` | #294 | The `dev9p.poll` readiness-op cancel-at-close teardown lifetime (BELOW `net_poll.tla`'s abstraction): no UAF of the refcounted poll-state + the `ready`-fd Tclunk delivered exactly once at fd-close (the slot-leak fix; Fix=TRUE) vs the deferred-pin leak (Fix=FALSE counterexample) |
| `specs/weft.tla` | Weft-1 (spec; impl Weft-2 / 3 / 5 landed, Weft-6..7 OWED; Weft-4 = `weft_readiness.tla`) | Capability network dataplane (I-37): no per-op mediation + the F_NOTIF multi-holder buffer lifetime (Weft-5 `weft_notif`) + the descriptor-ring TOCTOU (Weft-3) + the shared-Burrow lifetime bounded by the flow |
| `specs/weft_readiness.tla` | Weft-4 | I-9 across the readiness ring: the single-cache-line poke's store-buffer register-then-observe (netd's edge vs the guest's park) loses no wake — the PUSH counterpart of `net_poll.tla`'s elicited PULL |
| `specs/sched_tickless.tla` | TI-spec (#299) | I-9 across the tickless idle one-shot arm (NO_HZ_IDLE, ARCH §8.6): register-then-observe (`idle_in_wfi` set BEFORE the arm + WFI) loses no work-arrival wake (`NoLostWake` + the `EventuallyRuns` liveness) vs `sched_tickless_buggy.cfg` (`BUGGY_PARK` = park-before-register ⇒ the IPI is never sent ⇒ parked-with-pending counterexample). A sibling, not a `scheduler.tla` extension (which already proves the tickless wake-correctness — the periodic tick was never a modeled wake) |
| `specs/sched_rebalance.tla` | TI-4a (#304) | Work-conservation under tickless idle — the push-on-overload rebalance leg (ARCH §8.6 / §8.10): a busy CPU pushes surplus queued work to an idle peer (`EventuallyParallelized` liveness) + the kick respects register-then-observe (`NoLostWake`) vs `sched_rebalance_buggy_nokick.cfg` (no kick ⇒ surplus strands = the TI-3 regression in model form) + `sched_rebalance_buggy_nolift.cfg` (kick forgets the park-lift ⇒ lost wake). A sibling — placement itself is already inside `sched_alpha.tla`'s arbitrary-target envelope; this models only the busy-side rebalance the periodic tick used to cover by pull-polling |
| `specs/fs_cache.tla` | L1b (+ the write-behind `StageWrite`/`FlushClose` legs @F1; + `EnableFlushPopulate` @G1) | Larder close-to-open coherence (I-38) on the content-token abstraction: `Open`-revalidate + `Read`-serve + `OwnWrite`-invalidate never serve a stale token (`NoWrongRead`), incl. the staged-write legs, vs 5 buggy cfgs (`stale_serve` / `no_invalidate` / `skip_staged` = the overlay-miss class / `lost_stage` = the lost-write class / `populate_unflushed` = the G1 install-before-land class) |
| `specs/debug_stop.tla` | 8a-1a (model-first, spec-first RE-ENABLED for this surface; 8a-2c added `StopImpliesOwned` + the 5th buggy cfg) | Debugger stop/continue on the death lineage (I-39 composition): `NoLostStop` + `DeathWinsOverStop` (a stop is observed ONLY at the EL0-return tail, ordered after `el0_return_die_check`) + `NoEL0AfterStopped` + `ExactlyOnceResume` + `StopImpliesOwned` (the per-Proc stop flag is set only while a debugger owns the slot -- the 8a-2 SA-1 fix: a hardware fire delivers via `proc_debug_fault_stop`, gated on `debug_owner` under `g_proc_table_lock`) + the `EventuallyResumed` NoStrand liveness (a debugger's detach/close/death always resumes the target) + `EventuallyLaunchedDies` (the EXITKILL refinement, designed 2026-07-23: a debugger-LAUNCHED `exitkill`-marked target dies with its debugger — die-with-launcher, `PTRACE_O_EXITKILL` — rather than being NoStrand-resumed and orphaned) vs 6 buggy cfgs (`park_before_die` / `lost_stop` / `double_wake` / `strand_on_debugger_death` / `fault_stop_ungated` / `exitkill_ignored`) |

Each module carries its clean cfg(s) plus buggy-cfg counterexamples (99
buggy cfgs total across the inventory).

### 25.3 Spec → code mapping

`SPEC-TO-CODE.md` per spec: each spec action maps to a source location, and
the canonical mapping lives there. **Honest status (RW-10):** the
"CI verifies the mapping is current" gate planned at Phase-2 close was never
built; `make specs` runs each module's *default clean cfg* and fails on any
TLC failure (RW-10 fix), but nothing runs the buggy-cfg counterexample set
automatically and nothing checks mapping currency — both remain manual
discipline (the tiered spec-gate runner is a tracked task).

### 25.4 Audit-trigger surfaces

Every change to a file or function listed below spawns an adversarial soundness audit before merge. Updated at every ARCH change. **The convergence-detour BUILD items (IDENTITY-DESIGN.md §8.4) — SMMU/DMA isolation, the new syscall surfaces (clock, FS-mutation, `CAP_KILL`, PAN), the resource floor, the orphan reaper — are upcoming audit-bearing surfaces, each appended here in the PR that introduces it.**

| Surface | Files | Why |
|---|---|---|
| Exception entry | `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S` | Every syscall, IRQ, fault path |
| Halls crash dump (HX-1) | `arch/arm64/halls.c`, `arch/arm64/halls.h`, `arch/arm64/exception.c` (entry wrappers), `kernel/extinction.c` | Tier-1 fatal-path dump; runs on a dying machine. HX-I1 re-entrancy guard (no loop), HX-I2 bounded sanity-gated fp walk, HX-I3 `EXTINCTION:` ABI line unchanged. See `docs/reference/101-halls.md`. |
| Page fault | `arch/arm64/fault.c`, `kernel/vma.c`, `kernel/burrow.c`, `arch/arm64/mmu.c` | Lifetime, demand-page, COW, W^X |
| REVENANT file-backed demand-paged exec (the Image cache + the `BURROW_TYPE_FILE` fault arm) | `kernel/exec.c` (`exec_setup_from_spoor` + `exec_read_header` [the OOB header-buffer guard] + `map_text_file_backed` + `map_eager_from_file` + the `PT_LOAD` dispatch gate; the legacy blob path `exec_setup`/`exec_setup_with_argv` kept test-only), `arch/arm64/fault.c` (the `BURROW_TYPE_FILE` demand-page arm: `demand_page_locked` [the `burrow_ref` pin] + `file_demand_page_slow` [the LOOPED death-interruptible `dev->read`] + `file_install_locked` [install-once re-validate]), `kernel/image.c` + `kernel/include/thylacine/image.h` (the qid-keyed Image cache + the eviction-safety proof), `kernel/burrow.c` (`burrow_create_file` + the FILE free arm + `burrow_acquire_mapping` FILE liveness), `arch/arm64/mmu.c` (`arch_icache_sync_range`), `kernel/syscall.c` (`exec_resolve_from_namespace` + the 5 spawn bodies + 3 thunks -- the pinned-Spoor ref ledger across `rfork`), `kernel/include/thylacine/{exec.h,page.h,syscall.h}` (`EXEC_FILE_MAX` / `EXEC_ELF_HEADER_MAX`); **cross-subsystem**: `kernel/torpor.c` (the pre-fault before `torpor_lock` -- R-5 F1) | REVENANT (`docs/REVENANT.md` = `docs/EXEC-LOAD-DESIGN.md`; ARCH §6.5 + §28 **I-36**). **Realizes I-36 (the 7 file-backed-exec soundness conditions); retires the 1-MiB whole-ELF slurp -- a binary of any size execs.** No new §28 invariant beyond I-36 (composes I-7 dual-refcount / I-9+#811 death-interruptible / I-12 W^X / I-14 ecode-bounding / I-30 pin-at-exec). Prosecute: the spoor/burrow ref ledgers across the 5 spawn paths + `image_lookup_or_create` + `map_text_file_backed` + the FILE free arm (no double-clunk / leak / UAF -- one `spoor_clunk` per adopted ref on every path); the demand-page install-once race (the `burrow_ref` pin closes UAF/ABA across the blocking read; two CPUs fault one page -> install-once, loser frees); the Image eviction-safety (only a `handle_count==1 && mapping_count==0` idle victim, stable under `g_image_lock`; lock order `g_image_lock -> v->lock`); the I-cache #317 (sync before the R+X PTE; the race-loser's page synced by its creator; `ic ivau` IS-broadcast); the OOB ELF header guard (`exec_read_header` bounds `elf_load`'s ehdr+phdr deref within the 16-KiB buffer; `elf_load` never derefs `e_shoff`/segment bytes; `elf_image` is all scalars -> kfree-before-use safe); the dispatch-gate tail (`round_up(filesz)==round_up(memsz)` routes whole-bss-page text to eager-KP_ZERO); W^X (text R+X); death-interruptible + `snare:bus` fail-close (never zero-fill text). **CLOSED CLEAN at R-5** -- two independent prosecutors (Opus-4.8-max `holotype-reviewer` + concurrent self-audit) caught DIFFERENT P1s: **F1** torpor `uaccess_load_u32`-under-`torpor_lock` newly sleeps on a file-backed text page -> system-wide futex stall (fixed: pre-fault before the lock) + **SA-F1** `file_demand_page_slow` single-read corrupts an interior dev9p text page on a partial Rread (fixed: loop the read; regression `demand_page.file_short_read_fills_page`); **F2** [P3, unreachable at v1.0] stale `freq->slot` reuse -> closed-with-justification. v1.x seams: anon-COW data, per-page I-32 charge, the blob-path unification, the Image pressure-reclaim pass (which must re-establish the torpor non-blocking-uaccess-under-lock property). **No new spec** per the 2026-05-23 broadening -- prose validation in `docs/REVENANT.md` + ARCH §6.5/§28 I-36 + `docs/reference/126-revenant.md` + this row + the focused audit (`memory/audit_revenant_closed_list.md`) + the kernel tests (`burrow.file_*` / `demand_page.file_*` incl. `file_short_read_fills_page` / `image.*` / `exec_ns.*`) + the boot proof (the unstripped >1.11-MiB `net-echo` file-backed exec + full TLS-over-/net E2E) + default 986/986 + the SMP gate. **Read-ahead cluster addendum (go-arc)**: `file_demand_page_slow` becomes a thin entry -> `file_demand_page_cluster` (ONE batched `dev->read` of up to `REVENANT_READAHEAD_PAGES`=64 pages via a KP_ZERO staging buffer -> per-slot order-0 pages, word-copied + I-cache-synced BEFORE any PTE can back them -> per-slot install-once under `vma_lock`+`v->lock` [`file_install_cluster_locked`: sibling-won slots keep the resident page, ours freed outside `v->lock`; only the faulting slot's PTE installs -- cluster-mates resident-hit on their own faults]) degrading to `file_demand_page_single` (the audited R-5 body, renamed) on any alloc shortfall / degenerate cluster -- read-ahead never fails a fault; byte-identical to N sequential single reads (same offsets, EOF tail stays zero); `filepages[]` semantics unchanged. Closes the 4-KiB-fault-vs-8-MiB-Stratum-extent amplification. **Companion torpor change (the same go-arc pass)**: after the R-5-F1 pre-fault load, `sys_torpor_wait_for_proc` returns `TORPOR_OK` on a `prefault != expected` mismatch WITHOUT taking `torpor_lock` (the Linux-futex compare-before-queue shape) -- sound because no waiter is registered on that path (I-9's window exists only between register and sleep, reached only by the equal path); the mismatch return provides only the plain aligned-u32 load's ordering, and every consumer (musl / Go runtime / libthyla-rs) re-checks with its own atomics per the universal futex contract. Both prosecuted by the focused read-ahead+torpor audit (`memory/audit_readahead_torpor_closed_list.md`) + the `demand_page.*` cluster tests; prose `docs/reference/126-revenant.md` + `docs/reference/80-torpor.md`. **Rodata file-backing addendum (#45, CHASE 2026-07-11)**: the dispatch gate generalizes `PF_X` -> `NOT PF_W` (`file_shareable`), so the R-only rodata `PT_LOAD` (roughly half a Go binary's bytes: compile 7.89 of 17.5 MiB) rides the SAME file-backed demand-paged Image-cached path as text; writable data stays eager-copied anon (I-36 condition 4 untouched); text byte-identical (`elf_load` rejects `W\|X`). NO fault-arm change (installs already at `vma->prot`; the #317 I-cache sync already `freq->exec`-gated -- sound because each FILE Burrow backs ONE segment at ONE prot, so no page migrates non-exec -> exec) and NO Image-cache change beyond `IMAGE_CACHE_MAX` 64->128 (2 entries/binary now). The per-Burrow-single-prot property is ENFORCED (not assumed) by the `exec` component of the Image key (`(dc,devno,qid.path,qid.vers,file_offset,size,exec)` -- #45 audit F1): a crafted ELF's aliased R+X + R-only PT_LOADs (identical file window, different X-ness) get DISTINCT Burrows, so no FILE Burrow is dual-prot and a non-exec fill never leaves an executable resident-hit unsynced (the #317 stale-I-cache hazard, closed); the gate also requires `PF_R` (#45 audit F2 -- a no-access flags==0 PT_LOAD stays eager). Prosecute on any change: the gate must keep routing `PF_W` eager (a file-backed WRITABLE mapping violates I-36-4 + the section-6.5 refusal); the exec component must stay in the Image key (both search passes + the install) so the single-exec-class property holds; the I-36 conditions' segment-generality (EXEC-LOAD-DESIGN section 4.6 soundness walk incl. the torpor pre-fault composition + the I-32 posture). Validated by the section-4.6 prose + `docs/reference/126-revenant.md` + the #45 focused audit (Fable-5-max prosecutor + concurrent self-audit CONVERGED on F1; 0 P0 / 1 P1 / 0 P2 / 2 P3, NOT dirty; `memory/audit_45_rodata_closed_list.md`) + the `exec.from_spoor_rodata_dispatch` + `exec.from_spoor_aliased_window_distinct` (the F1 regression) + `image.exec_discriminates_key` + `demand_page.file_rodata_prot` tests + boot OK (every boot binary's rodata now takes this path) + the SMP gate; measured (CHASE): the ~792 MiB of per-exec rodata re-reads in a cold `go build` collapse to first-touch demand paging. |
| Boot-time LSE alternatives-patcher (Lazarus W1.5) | `arch/arm64/alternatives.{c,h}` (the `ALTERNATIVE()` macro + `apply_alternatives()`), `arch/arm64/atomic_lse.h` (the LL/SC-default patchable RMW primitives), `arch/arm64/mmu.c::mmu_patch_text` + the static `patch_kva_to_pa` / `patch_map` / `patch_unmap` / `patch_sync_icache` helpers, `arch/arm64/kernel.ld` (the `.altinstructions` + `.altinstr_replacement` sections inside `.rodata`), `kernel/main.c` (the `apply_alternatives()` call, after `hw_features_detect` + the allocator, strictly before `smp_init`), the routed sites (`kernel/include/thylacine/spinlock.h` xchg, `kernel/sched.c` steal-rotate, `kernel/spoor.c` + `kernel/srvconn.c` int refcounts) | Lazarus W1.5 (PORTABILITY.md §4.5; user-voted 2026-06-06). **Self-modifies `.text` -- the central concern is I-12 (W^X).** Rewrites LL/SC atomic sites to single-instruction LSE on FEAT_LSE cores at boot. **I-12** holds because the write goes through a TRANSIENT RW-not-X alias (a scratch VA mapped `PTE_KERN_RW` = RW+PXN+UXN); the canonical `.text` stays RO+X and the direct-map alias RO+XN -- no page is ever writable AND executable at PTE granularity. Single-CPU, full-DAIF-masked, strictly before `smp_init` (no peer executes a site mid-patch; secondaries fetch patched bytes with cold I-caches). Prosecute: the W^X window (scratch torn down on every path incl. extinction), ARM-ARM-B2 I-cache maintenance completeness (`dc cvau` scratch / `ic ivau` canonical / `dsb ish` / `isb`; CTR_EL0 line stride; cross-page spans), the `at s1e1r`+PAR_EL1 VA->PA, the scratch-slot TLB discipline (no stale-entry write-to-wrong-PA on reuse), the NOP-pad fit (`alt_len <= orig_len`, bounded buffer), the reloc-free KASLR-independent PC-relative table, and per-op LL/SC<->LSE equivalence (operand/width/order; `fetch_sub == fetch_add(-v)` preserving the signed-PRE refcount UAF check). LL/SC is the always-correct fallback -- a patcher bug fails safe (slower, never wrong). **No new spec** per the 2026-05-23 broadening -- prose validation in PORTABILITY.md §4.5 + this row + the focused audit + the `alternatives.*` unit tests (`patch_applied`: `g_alt_applied == g_alt_total`; `atomics_correct`: the patched forms) + default 716/716 + the SMP gate on `-cpu max` (LSE active). |
| GICv2 driver + EL1 virtual timer (Lazarus W2) | `arch/arm64/gic.c` (the GICv2 path: `gic_init_v2` + `dist_init_v2` + `gic_cpu_config_v2` + `cpu_iface_init_v2` + the GICC MMIO ack/EOI with the per-CPU CPUID-preserving `g_v2_eoi_token` + `gic_send_ipi` GICD_SGIR + the banked `gic_enable_irq`/`gic_disable_irq` + the `v2_w32`/`v2_r32`/`v2_w8` dsb-after-each HVF accessors), `arch/arm64/gic.h`, `kernel/mmio_handle.c` (reserve the GICC region for both v2 compats -- I-5), `kernel/main.c`; AND the virtual-timer switch (#889): `arch/arm64/timer.{c,h}` (CNTV_*/CNTVCT + `TIMER_INTID_EL1_VIRT`=27 + EL0VCTEN) + `kernel/irqfwd.c`/`kernel/smp.c`/`kernel/main.c` (the INTID-27 consumers) + `usr/irq-bench` (EL0 CNTVCT) | I-18 (GICv2 SGI IPIs in send order), I-15 (DTB GIC discovery), I-5 (GICC reserved). The v2 MMIO CPU interface is the HVF-on-Apple enabler (sidesteps the GICv3-distributor `isv` assertion); the dsb-between-GIC-MMIO mitigation is empirically validated under HVF on M-series. GICv2 SGI EOI echoes the source CPUID (raw IAR saved per-CPU, no nesting). The virtual timer is required because the EL1 physical timer is hypervisor-reserved under HVF (CNTP_TVAL write -> EC=0 undef); CNTV_* works on TCG/HVF/bare-metal. PORTABILITY.md section 5. **No new spec** (2026-05-23 broadening) -- prose in PORTABILITY.md section 5 + `docs/reference/10-gic.md` + `docs/reference/11-timer.md` + the focused audit + default/GICv2 716/716 + SMP gate + the HVF empirical boot. Owed: #890 (userspace virtio-mmio `isv` under HVF -- a different layer). |
| Kernel CSPRNG (software-RNG, Lazarus W3) | `kernel/chacha20.{c,h}` (ChaCha20 keystream), `kernel/random.c` (the forward-secure CSPRNG + seed sources + the kernel virtio-rng driver `random_virtio_pull` + `kern_random_bytes`/`kern_random_seeded`/`random_seed_from_virtio`), `kernel/include/thylacine/random.h`, `kernel/main.c` (boot virtio reseed), `kernel/mmio_handle.c` (I-5 RNG-slot doc) | CSPRNG quality + seed sources + reseed cadence. Replaces the RNDR-only baseline with a ChaCha20 forward-secure CSPRNG (arc4random construction) so the same path runs on RNDR-less HVF/A72. Seed = DTB kaslr/rng-seed (domain-separated from KASLR) + CNTPCT + RNDR-stir + the kernel virtio-rng pull; `g_rng_seeded` gates on an UNOBSERVED source (RNDR/virtio). Prosecute: weak-seed/predictable-output (BSS-zero first rekey; the KASLR entropy-sharing the domain separation + deferred gate close), no rekey material served (`g_rng_buf[0..40)` zeroed), fail-closed `g_rng_seeded`, SMP races (`g_random_lock` chacha / `g_rng_dev_lock` device -- process-context-only, no nesting, lock-order vs buddy), the virtio driver memory safety (device reset BEFORE freeing ring/buffer pages; `used.len` clamped; the **wall-clock-bounded** poll [`RNG_VIRTIO_POLL_MS` CNTPCT deadline, NOT a fixed iteration count -- HVF's fast native vCPU spin outran QEMU's async BH -> #188 seed-miss -> fail-closed cascade; `RNG_VIRTIO_POLL_MAX`=1<<30 is the unconditional frozen/misconfigured-counter termination backstop (audit F1) + the CNTFRQ==0 budget] + the bounded `RNG_VIRTIO_PULL_TRIES` retry (boot=3, threshold=1); error paths reset+unlock+no-leak), the all-zero guard (non-coherent DMA fails SAFE), secret hygiene, I-5 (RNG slot kernel-driven but page-share -> inherits the v1.0 virtio-mmio kproc-trust posture, same residual virtio-blk carries). PORTABILITY.md section 6. **No new spec** (2026-05-23 broadening) -- prose in PORTABILITY.md section 6 + `docs/reference/106-random.md` + the focused audit (`audit_w3_closed_list.md`; 0 P0/2 P1/3 P2/3 P3, dirty close -> round 2; #188 wall-clock poll fix) + the chacha-RFC-vector + virtio-reseed + `virtio_deadline_ticks` tests + default 922/922 + the SMP gate (default+UBSan x smp4/smp8). |
| Wall clock + RTC (PL031) + time/identity syscalls (LS-K) | `arch/arm64/rtc.{c,h}` (PL031 discovery via `dtb_get_compat_reg("arm,pl031", ...)` + the QEMU-virt `0x09010000` fallback + the one-shot `RTCDR` read -> `rtc_read_epoch_seconds`), `arch/arm64/timer.{c,h}` (the boot-time wall-clock anchor `timer_set_wallclock_anchor` write-once-before-`smp_init` + `timer_realtime_ns`; `timer_now_ns` is the existing monotonic), `kernel/main.c` (the anchor after `dtb_init`+`timer_init`), `kernel/mmio_handle.c` (`reserve_compat("arm,pl031")` -- I-5), `kernel/syscall.c` (`sys_clock_gettime_handler` + `sys_getpid`/`getuid`/`getgid_handler` + dispatch), `kernel/include/thylacine/syscall.h` (`SYS_GETPID=72` / `SYS_GETUID=73` / `SYS_GETGID=74` / `SYS_CLOCK_GETTIME=75` + `struct t_timespec` {i64 sec; i64 nsec} + `T_CLOCK_REALTIME=0` / `T_CLOCK_MONOTONIC=1`), `usr/lib/libthyla-rs` (time `now`/`Instant`/`SystemTime` + the identity wrappers), `usr/coreutils/src/bin/{id,whoami,date}.rs` | LS-K (LIFE-SUPPORT.md + ARCH §22.6). **NO new §28 invariant** -- a read-only introspection surface covered by **I-15** (the PL031 view derives from the DTB; the documented QEMU-virt fallback is the argued exception) + **I-5** (the RTC MMIO slot kernel-reserved). Prosecute: the PL031 discovery + the one-shot MMIO read (mapped via `mmu_map_mmio`; fail-soft `epoch=0` on a missing/implausible RTC, never an extinction); the wall-clock anchor SMP-safety (write-once before `smp_init`, like `g_freq`; a plain read is a coherent snapshot); the `clock_gettime` uaccess (the `t_timespec` copy-out routes faults to `-EFAULT`; a bad `clk_id` -> `-EINVAL`); the `epoch*1e9 + delta` arithmetic (no i64 overflow -- epoch ~1.7e18 ns); the three identity syscalls carry no capability + read immutable Proc fields (`pid` / `principal_id` / `primary_gid`). The 32-bit `RTCDR` Y2106 wrap + settability + the vDSO + name-resolution + `getgroups` are recorded v1.x seams (ARCH §22.6). **No new spec** per the 2026-05-23 broadening -- prose validation in ARCH §22.6 + `docs/reference/11-timer.md` (the wall-clock section) + this row + the focused audit + the kernel unit tests (RTC anchor + clock monotonicity + clock_gettime EINVAL/EFAULT + getpid/getuid/getgid) + the `ls-k` LS-CI (`id`/`whoami`/`date`) + the SMP gate. **APPEND (net-7a-1, 2026-06-19): `SYS_CLOCK_SETTIME = 79`** -- the runtime wall-clock setter (closes the LS-K settability seam; NET-DESIGN section 10). `sys_clock_settime_handler` (REALTIME-only; **CAP_HOSTOWNER-gated** -- I-22; clk_id+cap before the buffer; `uaccess_load_u32` x4; range+overflow guards) + `timer_reset_wallclock_anchor_ns` (the SINGLE-`u64` `g_wallclock_offset_ns` re-anchor via one atomic store -- SMP-safe by construction, no seqlock; MONOTONIC untouched) + the libthyla-rs/libt wrappers. **New privilege surface -- net-7d CLOSED CLEAN** (Opus-4.8-max prosecutor + concurrent self-audit; 0 P0 / 0 P1 / 1 P2 / 4 P3; the load-bearing SMP gate PASS = 0 corruption across 40 boots [default+UBSan x smp4/smp8 N=10], the 18 timing-classified all ground-truthed to the healthy guest end-state): gate completeness+ordering, the single-`u64` re-anchor SMP-safety, MONOTONIC-untouched, + the overflow guard all traced sound; the 1 P2 = the shared net-7 TLS drain-clamp asymmetry, fixed. No new spec / no new section-28 invariant (composes I-22). Proven by `clock.settime_reanchors` + `clock.settime_cap_gate` + joey's elevated `net-7a SYS_CLOCK_SETTIME round-trip OK` (real dispatch + uaccess + a live cap). The SNTP consumer is net-7a-2. The full row is CLAUDE.md. `memory/audit_net7_closed_list.md`. |
| Monotonic-clock vDSO page (#343) | NEW `kernel/vdso.c` (`vdso_init` -- one kernel-owned `burrow_create_anon(PAGE_SIZE)` held forever + the `struct vdso_clock` populate; `vdso_publish_wall`; `vdso_clock_burrow`), NEW `kernel/include/thylacine/vdso.h` (`struct vdso_clock { magic, version, freq, wall_offset_ns, reserved[4] }` 64 B `_Static_assert`-pinned + `VDSO_CLOCK_MAGIC`/`VERSION`), `kernel/include/thylacine/elf.h` (`AT_VDSO_CLOCK = 0x5654` -- Thylacine-private, outside the Linux `AT_` range), `kernel/exec.c` (`exec_map_vdso` RO-maps the shared page at `EXEC_USER_VDSO_BASE` in both exec bodies; `exec_fill_auxv` both shapes route through), `kernel/include/thylacine/exec.h` (`EXEC_USER_VDSO_BASE = 0xC0000000` + `EXEC_INIT_AUXV_COUNT` 6->7), `arch/arm64/timer.c` (`vdso_publish_wall` from `wallclock_publish_ns`; `timer_wallclock_offset_ns_now` boot-seed getter), `kernel/main.c` (`vdso_init` after `burrow_init`); **sub-chunk 3/4**: the Go fork `os_thylacine.go` + `usr/lib/libthyla-rs` readers | vDSO (`docs/VDSO-DESIGN.md`; user-chosen + signed off 2026-06-25 -- the fix for #343's second issue, the Go toolchain's ~740M `SYS_CLOCK_GETTIME` `nanotime()` churn). **A new EL0-facing READ-ONLY kernel page + a new ABI -- audit-bearing.** **NO new §28 invariant** -- composes **I-12** (W^X: RO+XN; `mmu_install_user_pte` at `VMA_PROT_READ`), **I-13** (a kernel page mapped RO to EL0 holding ONLY `{magic,version,freq,wall_offset_ns}` + zeroed reserved -- no pointers/secrets/KASLR, a DEDICATED page, and a user WRITE faults [`fault.c` `is_write && !VMA_PROT_WRITE` -> snare:segv] so a hostile Proc can never corrupt the shared timebase), **I-15** (`freq` == CNTFRQ at `timer_init`). Prosecute: the **shared-page property** (the SAME physical page mapped RO into every Proc -- the Burrow is kernel-owned + held forever [`handle_count==1`], so it never frees even as Procs map/unmap; `mapping_count` rises at `burrow_map` + falls at `vma_drain`; no double-free at teardown -- the leaf is the Burrow's page, `pgtable_destroy` frees only sub-tables); the **no-seqlock atomicity** (`freq` write-once before `smp_init` + `wall_offset_ns` a single aligned `u64` from `SYS_CLOCK_SETTIME` -- old-or-new never torn, the LS-K/net-7a pattern; cross-Proc read-vs-settime coherent; MONOTONIC never backward); the **auxv builder** (`exec_fill_auxv` writes 6-or-7 entries into a frame ALWAYS reserving 7 [`EXEC_INIT_STACK_SIZE` 160] so the AT_RANDOM block + strings offsets stay STABLE; `AT_VDSO_CLOCK` is BEST-EFFORT -- `exec_map_vdso` returns 0 on OOM and the entry is omitted, the frame stays well-formed; both Shape A + B route through the one helper -- the literal-index fragility retired); the **fallback** (a magic/version mismatch falls back to `SYS_CLOCK_GETTIME` -- the syscall path retained, never a boot gate); the **boot ordering** (`vdso_init` after `timer_init` [freq] + the boot anchor [offset] + `burrow_init` [the page]; seeds from `timer_wallclock_offset_ns_now`, then `wallclock_publish_ns` mirrors). **No new spec** per the 2026-05-23 broadening (the atomicity argument is the single-`u64` LS-K load -- prose) -- prose validation in `docs/VDSO-DESIGN.md` + ARCH §22.6 (seam -> being-built) + this row + `docs/reference/11-timer.md` (the vDSO section) + the focused audit (sub-chunk 5) + the kernel tests (`vdso.page_populated` / `publish_updates` / `mono_matches_timer` / `maps_ro_read` [the shared-page PTE] / `maps_ro_write_faults` [I-13]) + boot OK + the re-measure (sub-chunk 3) + the SMP gate. The full prosecution copy is the CLAUDE.md row. |
| Kaua console-TUI substrate: the cons/consctl backend + the ut raw-mode dance (LS-7) | `usr/lib/kaua/src/term.rs` (the backend: the VT/ANSI input parser fd 0 -> KeyEvent; the damage -> escape emit to fd 1; the double-buffer cell-diff `Terminal`; the alt-screen/cursor/SGR screen-control escapes), `usr/nora/` (the editor consumer), `usr/utopia/libutopia/src/repl.rs` (`Repl::console_raw`/`console_cooked` on the #94-B-b `console_apply_default`), `usr/utopia/libutopia/src/eval/stmt.rs` (the `is_raw_command` spawn path: set raw -> stdin `Piped`->`Inherit` -> spawn -> on the child's exit/death restore cooked + re-emit the leave-alt-screen/show-cursor escapes -- the crash backstop) | Kaua (`docs/KAUA.md`, LS-7). **A new EL0-facing console presentation layer + the ut controlling-terminal dance -- the I-27 trusted-path surface; prosecute the dance + the backend, NOT the pure widget/buffer layers.** **No new ARCH §28 invariant** -- Kaua consumes **I-27** (the dance is the console capability handshake: the editor is NEVER console-attached and NEVER holds consctl, which stays private to `ut` per #94-B-b; the SAK/elevation gate keys on `PROC_FLAG_CONSOLE_ATTACHED`, untouched), **I-9** (the event loop polls the LS-8a pollable cons via LS-8c; no new wait/wake), and the **LS-8b consctl mechanism** (the raw/cooked mode strings are the audited `cons_set_mode_cmd` flags under `g_cons.lock`; no new kernel state -- the kernel is byte-identical). Prosecute: the I-27 properties (no consctl re-forward to the editor; no console-attach conferred; consctl can never read console INPUT -- only the 5 mode flags); the **restore-on-EVERY-exit-path backstop** (`ut` restores cooked + leave-alt-screen + show-cursor whether the editor exits cleanly, errors, or is killed mid-edit -- no wedged console; `no_std` `panic=abort` means the editor's `Drop` does NOT run on a crash, so `ut` is the authoritative restorer); the input-parser bounds (no OOB / no panic on a malformed or truncated escape sequence; a lone ESC is `KeyCode::Esc`); the escape-emit bounds (no OOB on the cell / SGR run); the fd 0/1 discipline (the backend touches ONLY stdin/stdout, never a capability fd). The buffer/widget/layout/event layers are PURE userspace (a bug corrupts only the app's own screen, never a privilege/safety boundary) -> unit-tested, NOT audit-gated. **No new spec** per the 2026-05-23 broadening -- prose validation in `docs/KAUA.md` + this row + the focused audit + the pure-layer unit tests (cell-diff, layout, the input-parser truth table, the editor state machine) + the `ls-7` LS-CI (open / edit / save / `cat` shows the edit) + boot OK. |
| Pollable console + termios/`consctl` line discipline (LS-8) | `kernel/cons.c` (LS-8a: a `poll_waiter_list` embedded in the cons layer + the IRQ-set `poll_wake_pending` flag under `g_cons.lock` + `console_mgr_main` draining it and calling `poll_waiter_list_wake` in *process* context + the `.poll` impl shared by `devcons` + `devdev`; LS-8b: the line discipline -- the five independent termios bits ICANON/ECHO/ISIG/ICRNL/ONLCR + the cooked-mode line buffer/echo/erase + the ISIG `Ctrl-C`->`interrupt`-note cooking + the `cons_set_mode_cmd`/`cons_render_mode` consctl parse/render + `cons_output_write` ONLCR), `kernel/cons.c::cons_rx_input` (the IRQ producer, fed by `arch/arm64/uart.c::uart_rx_handler`: the LS-8b cooking + ring-push + set `poll_wake_pending` + `wakeup(&g_cons_mgr_rendez)`; echo staged under `g_cons.lock` + emitted via `cons_emit` after release), `kernel/devdev.c` (the `/dev/consctl` read/write routed to `cons_render_mode`/`cons_set_mode_cmd`, replacing the v1.0 `read->0`/`write->-1` stub; the console-attach open gate unchanged), `kernel/poll.c` (the `.poll` vtable consumer; mechanism unchanged) | LS-8 (LIFE-SUPPORT.md LS-8 + ARCH §23.5). **The death-path/wait-wake lineage (#788/#806/#807/#808/#860/#809/#811/#926 -- the most bug-prone in the tree) AND a new EL0-facing line discipline + a userspace control ABI -- prosecute hard.** **Invariant I-9 generalized to the DEFERRED poll-wake**: the RX IRQ cannot walk the non-IRQ-safe `poll_waiter_list`, so it sets `poll_wake_pending` + wakes the `console_mgr` kthread, which walks the list in process context (the Linux-tty `flush_to_ldisc` model). **Spec-first RE-ENABLED for this surface**: `specs/cons_poll.tla` (clean + liveness + the `BUGGY_MGR_LOST_WAKE` counterexample = the mgr's go-to-sleep as a hand-rolled check-then-sleep instead of register-then-observe -> a flag set in the window is lost -> the poller strands asleep on a ready console) is the pre-commit gate. Prosecute: the deferred-wake no-lost-wakeup at BOTH the mgr relay (register-then-observe on `poll_wake_pending` under `g_cons.lock` -- the `sleep(&mgr_rendez, cons_mgr_pending)` contract) AND the poller (poll.tla register-then-observe -- `.poll` installs the hook + samples readiness under `g_cons.lock`); the **IRQ-safety boundary** (ONLY `wakeup()` on a Rendez from `cons_rx_input`; `poll_waiter_list_wake` ONLY from `console_mgr` process context -- a plain non-irqsave lock + a nested `wakeup`); the **multi-thread-per-Proc-shared-state hazard** on the new cons `poll_waiter_list` (the #57a-F2 / RW-2 lesson -- the hook list is reachable from every poller incl. peer Threads of one Proc; the list lock + the stack-allocated-hook lifetime hold under SMP; a returned poll unhooks completely -- poll.tla NoStaleHook); the termios cooking (LS-8b): the **ECHO-off hard guarantee** (no input byte reaches the console OUTPUT when ECHO is clear -- the enforced password mask, no leak via cooked-mode erase/redraw), the **ISIG cooking** (Ctrl-C is the `interrupt` note when ISIG set / a literal `0x03` byte when clear -- the LS-5 disposition path unchanged, no double-cook), the cooked line buffer (bounds + backspace + ICANON line-assembly), the consctl parse (bounds + malformed reject), the global-console termios state under SMP (one console at v1.0; a mode flip racing a concurrent cons read -- no torn read of the flag word), the console-attach gate on `/dev/consctl` (unchanged). The PTY master/slave pair (`/dev/ptmx`+`/dev/pts`) + per-fd termios + process groups + Ctrl-Z stay **Phase-8** (I-20; `pty.tla` lands then). **No new termios spec** per the 2026-05-23 broadening (the cooking is a data transform -- prose + unit-tested); the I-9 deferred-wake leg IS specced (`cons_poll.tla`, re-enabled). Prose validation in ARCH §23.5 + LIFE-SUPPORT.md LS-8 + `docs/reference/111-cons.md` (the line-discipline section, added with LS-8a) + this row + the focused audit + the kernel unit tests (the deferred-wake register-then-observe; the termios cooking truth table; the ECHO-off no-output property) + the `ls-8` LS-CI + the SMP gate. **P1-F ADDENDUM (console write atomicity + the buffered TX path; #75, design §23.5.2, user-voted 2026-07-20).** ADDS a bounded TX ring + the PL011 **TX interrupt** (`uart_rx_handler` → a `MIS`-dispatching `uart_irq_handler`; new `PL011_IMSC_TXIM`/`ICR_TXIC`/`CR_TXE`/`MIS`/`IFLS`), a **writer role** across `cons_output_write` (the audited `srvconn.c::chan_role_acquire` #354 shape), and a deadlined room-wait. **A NEW IRQ PATH ON THE CONSOLE — the I-27 trusted-path surface, and two new park/wake legs on the wait/wake lineage; prosecute hard.** Establishes the contract that a `/dev/cons` write is atomic w.r.t. other console writers (§23.5.2); **no new §28 invariant** (a `Dev.write` semantic; both new parks are **I-9** obligations) and **no new spec** (the #354 role-park is prose+audit-validated; `cons_poll.tla` is untouched and re-run as the gate). Prosecute: the **role park/release I-9** (register-then-observe under the ring lock; release-clears-then-wakes; a `TSLEEP_TIMEDOUT`/`TSLEEP_INTR` exit leaks neither the role nor a stale hook — `poll.tla` NoStaleHook; #811 death-unwind returns a short write, never a strand); the **room-wait I-9** (the IRQ drain's wake vs a writer about to sleep — the classic check-then-sleep window; the drain runs in IRQ so only `wakeup()` on a `Rendez` is legal, never `poll_waiter_list_wake` — the LS-8a IRQ-safety boundary, unchanged and now load-bearing for a second producer); **lock order** (the TX ring lock is a leaf, irqsave, and must never nest with `g_cons.lock` or `g_uart_rx_lock`; the IRQ drain takes only the ring lock); the **#67 loss discipline preserved** (a stalled host consumer must yield a bounded short write, NEVER a wedged writer or an unbounded park — the deadline is load-bearing, not ergonomic); the **TXIM enable/disable race** (a drain that clears `TXIM` concurrent with a push that sets it must not leave a non-empty ring with TX interrupts off — the silent-console wedge); **Halls/extinction bypass** (the dump keeps the direct bounded `uart_putc` under HX-I; the pre-dump flush must itself be bounded and must not take the ring lock if a dying CPU may already hold it); the **arming boundary** (pre-GIC prints direct; the tooling-ABI lines byte-unchanged on both paths — TOOLING.md §10); **`cons_output_write` may sleep** (`spoor_write_common` holds no lock across `dev->write`) but `cons_emit` from echo may NOT — the two callers of the ring must keep opposite blocking contracts. Regressions: a two-writer concurrent-write atomicity test (revert-probed — it must FAIL on the pre-P1-F lockless loop), a ring-full short-write-on-deadline test, a TXIM re-arm test, and the `⊢`-tearing gate signature at 0/40. **#55 WINSIZE ADDENDUM (console winsize + `tty:winch`; design §23.5.3, user-voted 2026-07-22).** ADDS the kernel-held console winsize (u16 cols/rows under `g_cons.lock`, `0×0` = unset/serial) + the ptyfs-grammar `winsize <cols> <rows>` consctl verb/render + the CHANGED-only `tty:winch` post to the console OWNER's PGRP (`notes_post_pgrp`; `ct_sid` is pts-only so no console fg pgrp exists — the fg refinement is the recorded seam) + the UNGATED read-only `/dev/winsize` trivial leaf (`winsize <cols> <rows>\n`) + the consctl mint-gate widening to attached-OR-renderer + `cons_stat_native` (devcons + the devdev cons leaf; the is-a-cons qid marker DISJOINT from `PTS_FLAG` — a documented client ABI like ptsname; fixes the statless-cons `isatty()==false` pouch latent). Prosecute: the winch post ordering (changed-flag captured under `g_cons.lock`, posted AFTER the drop — no `g_cons.lock` → `g_proc_table_lock` edge; process context only, NEVER reachable from `cons_rx_input`/IRQ); the iff-changed discipline (an unchanged rewrite must NOT post — a repeat-post storm is a notes-queue DoS on the pgrp); the pgrp resolve lifetime (the `notes_post_pgrp` precedent — membership read under `g_proc_table_lock`; a dying owner mid-post); the mint-gate widening STOPS at consctl (cons/consdrain/consfeed gates byte-unchanged) AND the renderer-minted consctl is WINSIZE-VERB-ONLY (`CCONSWINSZONLY`; the #55 audit F2 fix — the renderer-dominance argument holds ONLY for winsize [pure geometry], NOT for a termios flip on the global cooking word that also governs the serial RX path, so `+echo` from a compromised renderer must not unmask a serial-typed password into the drain; the attached login/ut chain stays unmarked = full grammar); the winsize verb bounds (u16 band, malformed reject, the five-flag grammar untouched); the leaf is read-only + truly trivial (write −1, no state beyond the two u16s, poll always-ready); `cons_stat_native` zero-fills (I-13) + the marker collides with nothing under S_IFCHR; the serial posture (`winsize 0 0` render, never an error, CPR unaffected). Regressions: `cons.winsize_roundtrip` (verb + render + leaf agree), `cons.winsize_winch_iff_changed` (post on change, silence on rewrite), `devdev.consctl_renderer_mint` (the renderer can mint consctl; a plain Proc still cannot), `devdev.winsize_leaf_ungated` (unattached read OK, write −1), `cons.stat_native_qid_contract` (S_IFCHR + marker + zero-fill), + the pouch prover leg (TIOCGWINSZ success + isatty true on the console + TIOCSWINSZ EPERM) + the ls-gfx resize E2E (an aurora reweave drives a live winch into a relayout). |
| Userspace virtio-PCI transport (`KObj_PCI`, pci sub-arc) | `kernel/virtio_pci.c` (the P4-H enumerator + the new per-function ops), the new `KObj_PCI` kobj + claim table (`kernel/handle.c` + `kernel/include/thylacine/handle.h` partition + acquire/release), `kernel/syscall.c` (`SYS_PCI_CLAIM` / `SYS_PCI_MAP_BAR` / `SYS_PCI_INFO` handlers + BAR assignment + the `VIRTIO_PCI_CAP_*` walk + the DTB `interrupt-map` INTx swizzle), `usr/lib/libthyla-rs/src/hardware.rs` (`PciDev` + the virtio-pci-modern register layer), `usr/lib/netdev` (the pci transport sibling), `tools/run-vm.sh` (`virtio-net-device` → `virtio-net-pci`) | Userspace virtio-PCI transport (`docs/VIRTIO-PCI-DESIGN.md`; the DFS fork off #140, user-voted 2026-06-15). **A new hardware-capability kobj + a privilege/MMIO surface — prosecute hard.** **No new §28 invariant** — composes **I-5** (extended: `KObj_PCI` non-transferable, static_assert + dup/transfer reject), the existing `HwResourceExclusive` (the exclusive per-function claim, one owner per `(bus,dev,fn)` — the PCI analog of the mmio overlap rejection), **I-12** (`SYS_PCI_MAP_BAR` maps RW-only), **I-2/I-6** (`CAP_HW_CREATE`-gated like `SYS_MMIO_CREATE`; no transferable rights). Prosecute: the `KObj_PCI` lifecycle (exclusive-claim correctness; no UAF/double-free/leak on every error path claim→assign→enable→cap-walk; close quiesces the device — `device_status=0` + command-reg decode/bus-master off — BEFORE the BAR map tears down, the RW-7 R3-F1 discipline); **config-space mediation** (userspace never gets raw ECAM; `SYS_PCI_INFO` returns only resolved, bounds-checked region descriptors; a hostile BAR/cap layout — cap-pointer loop, out-of-range bar index, oversized length — is rejected, never trusted into an OOB); **BAR assignment** (the linear allocator stays in the host-bridge window; no two functions' BARs overlap; size/align math cannot overflow; page-alignment guaranteed before map); **`SYS_PCI_MAP_BAR`** (maps only a page-aligned BAR owned by the caller's handle; W^X; VA bounds; rollback on map/copy fault); **INTx routing** (the swizzle is computed from the DTB `interrupt-map`, not hardcoded; the GIC stays kernel-reserved — the driver gets a forwarded INTID via `KObj_IRQ`, never the distributor). net-only scope; the blk→PCI migration (+ the shared-INTID multi-waiter question) is a recorded v1.x seam. **No new spec** per the 2026-05-23 broadening — prose validation in `docs/VIRTIO-PCI-DESIGN.md` + this row + the focused audit + the kernel unit tests (claim/exclusive/BAR-assign/cap-resolve/INTx-swizzle + the `pci.walk_caps_hostile` hostile-layout rejection + the I-5 non-transferability assert) + `netdev-test` PASS over PCI + boot OK + the SMP gate. **Audit CLOSED CLEAN (pci-3): 0 P0 / 0 P1 / 0 P2 / 4 P3** (Opus 4.8 max prosecutor + concurrent self-audit) — every prosecuted surface above verified sound; F2 notify-doorbell bound + F3 `TPciInfo` offset asserts + F4 `pci.walk_caps_hostile` test fixed, F1 partial-map leak tracked (no v1.0 detach path — proc-exit-bounded by design, single-BAR NIC never triggers it). `memory/audit_pci3_closed_list.md`. The `KObj_PCI` reference doc is `docs/reference/115-pci-claim.md`. **G-7c ADDENDUM (the (id, nth) instance selector; the dual virtio-input claim):** `SYS_PCI_CLAIM`'s one arg packs `virtio_device_id | nth<<32` -- the 0-based enumeration-order instance, reaching a SECOND same-id function (tapestryd's keyboard + tablet are both virtio-input id 18; pre-G-7c the second was structurally unreachable: resolve always returned the first match and the claim then double-claim-rejected). A bare id (every pre-G-7c caller; typed-u64 wrappers zero the high word) is nth 0 = the historical first match, byte-identical. Prosecute on any change: the **resolve/claim coherence** (`kobj_pci_resolve_bdf(id, nth)` and `kobj_pci_claim(id, nth)` MUST walk to the SAME function -- the I-34 allowance gate checks the resolved bdf before the claim, and the pair's stability rests on `g_virtio_pci_devs[]` being boot-built + immutable; a mutable device table would reopen the checked-A-claimed-B allowance bypass); the exclusivity slot still rejects a double-claim of one bdf (nth only SELECTS, never bypasses); an over-large nth resolves NULL (fail-closed, no wrap). Consumer: `PciDev::claim_nth` + tapestryd's order-independent dual-probe (both instances brought up identically, ROLES classified by the virtio-input `EV_BITS` config query -- `supports_abs()` = the tablet -- never by enumeration order). Regression `pci.claim_nth` (over-large-nth reject + nth-0 resolve==claim coupling + the two-instance distinct-bdf/dual-claim legs, self-skipping without a second input function). |
| Syscall byte-I/O staging + bulk uaccess + the per-service bulk ring (CF-3 A+B) | `kernel/include/thylacine/syscall.h` (`SYS_RW_MAX` 4096 -> 128 KiB + the new `SYS_RW_STACK` = 4096), `kernel/syscall.c` (the 4 byte-I/O handlers `sys_read/write/pread/pwrite_handler` -- the two-tier bounce: ops <= SYS_RW_STACK keep the 4 KiB stack scratch, larger take a transient kmalloc freed on every path, degrading to the stack tier on allocation failure; `SYS_READDIR` + `SYS_GETRANDOM` + `SYS_EXPLICIT_BZERO` PINNED at SYS_RW_STACK -- the scrub arm's oversize REJECT is ABI), `arch/arm64/uaccess.S` + `uaccess.h` (the bulk `uaccess_copy_out`/`uaccess_copy_in` -- byte head to 8-align the user VA / 8-byte body / byte tail; THREE fixup entries each sharing one fault label; NO writeback on faulting instructions so a demand-page resume re-executes exactly), the port mirrors (go `rwMax` -> 128 KiB + `readdirMax` PINNED at 4096 [os passes 8 KiB dirent buffers -- an unpinned mirror would hit the kernel readdir REJECT], libt/libthyla-rs doc caps) | CF-3 A (CONCURRENT-FS.md CF-3 as-measured; the CF3RT Tread-stream measurement 2026-07-07). **The EL0 byte-I/O staging boundary -- I-13 (kernel/user isolation) is THE invariant; prosecute hard.** The 4 KiB stack scratch capped every bulk read RPC at 4 KiB against a 32 KiB msize (67% of a go build's Treads were exactly-4096 chunks; 4096 B / ~145 us turnaround = the measured ~27 MiB/s read ceiling). NO new section-28 invariant -- composes I-13 (the fixup-table fault-close: every user-VA touch in the copy loops is a designated faulting instruction; a fault mid-copy returns -1, never extincts, never derefs a kernel VA -- `sys_validate_user_buf` + UACCESS_USER_VA_TOP still gate the range) + I-32 (the heap tier is user-drivable kernel memory whose "transient" hold is attacker-delayable across a blocked dev->read/write [held-open pipe / idle socket / hung server] -- the CF-3 audit F1 close: a per-Proc bounce budget [`Proc.bounce_bytes` <= PROC_BOUNCE_MAX = 512 KiB, CAS charge/uncharge balanced on every path, PRINCIPAL_SYSTEM exempt]; over-budget ops degrade to the stack tier; the fork-tree aggregate stays the I-32 per-Proc shape). Prosecute: the six fixup arms (head/body/tail x in/out) each resolve + fault-close (kproc negative tests pin all six); the no-writeback discipline on faulting instructions (a resume must see unchanged registers); the bounce free-on-every-path (fault, dev error, short op -- no leak); the degrade-to-stack correctness (kmalloc failure = a short op, never a failed one, never a stack overflow); the pinned bounds on readdir/getrandom/bzero (a silent widen of the scrub REJECT is an ABI change; the readdir array is stack-sized); Dev semantics under big n (a pipe/cons/srv read returns available bytes <= n -- no new blocking); the 9P CLIENT-SIDE payload clamp (`kernel/9p_client.c::client_max_read_count`/`client_max_write_payload`: a single op's count is clamped to the negotiated msize's payload [Twrite additionally to the out_buf frame-build bound] and returns SHORT -- the lift's first bench EXPOSED the missing write clamp as -P9_E_IO on every >payload Twrite [the frame build failed] -> EIO'd compiler object writes -> no cache puts -> the warm build ran cold; the clamp is the protocol's own contract and `9p_client.bulk_write_clamps_short` pins it). **No new spec** per the 2026-05-23 broadening -- prose in CONCURRENT-FS.md CF-3 + this row + `docs/reference/53-sys-rw.md` (the two-tier staging) + `40-uaccess.md` (the bulk primitives) + the focused audit + the `uaccess.copy_*` kernel tests (all six arms + the fixup-entry presence) + joey's always-run boot-fatal `cf3-bulk` probe (since CF-3 B: a 160 KiB round-trip crossing the exact clamp boundary of the NEGOTIATED session -- first write must be EXACTLY 131049 and first read 131061, the end-to-end proof the 128 KiB msize negotiated; all bytes verified + a 4 KiB pread stack-tier cross-check) + boot OK + the SMP gate. **CF-3 B ADDENDUM (the per-service bulk ring + the #354 role-park; option B2 user-voted 2026-07-08; CONCURRENT-FS.md CF-3 B as-built).** ADDS: `kernel/include/thylacine/srvconn.h` + `kernel/srvconn.c` (heap per-conn rings -- `srvconn_chan.buf/cap`, cap = 2x the conn's msize class, the inline 64 KiB array retired [which also stops the old ~129 KiB struct rounding EVERY conn to a 256 KiB kmalloc]; `SRVCONN_BULK_MSIZE` = 128 KiB + the two-point class validation in `srvconn_create(+msize)`; `cn->msize` + `srvconn_msize`; word-wise `chan_copy`; the **#354 role-park**: `chan_role_acquire`/`chan_role_release` + a per-chan `role_waiters` poll_waiter_list -- a 2nd concurrent blocking reader/writer PARKS instead of being refused -1, rendez/wrendez stay single-waiter [the audited #348/#349 machinery untouched], writes stay call-atomic [the role spans the whole delivery]; NEW `srvconn_client_send_blocking` [the THIRD producer of the #348/#349 family -- the byte-mode client write a proxy's write_full would EPIPE-close on a 0] + drain-wakes on the server recv paths), `kernel/include/thylacine/devsrv.h` + `kernel/devsrv.c` (`SrvService.ring_msize` set at reserve, IDENTITY on a tombstone rebind [the F2 mode discipline]; `devsrv_post_listener(+bulk)`; `devsrv_open_connect` captures the class under the registry lock; `devsrv_write`'s CSRVCLIENT arm goes blocking), `SYS_WALK_CREATE_DMSRVBULK` = 0x01000000 (bit 24; syscall.h/.c -- admitted ONLY on the devsrv-post branch, rejected on a regular create like DMSRVBYTE), `kernel/9p_attach.c` (the msize proposal + recv cap = `srvconn_msize(cn)` -- the negotiation can never exceed the rings), `kernel/9p_client.{h,c}` (the two-tier out_buf: inline 32 KiB default / heap msize-sized bulk, OOM-degrades to inline -- the CF-3 A write clamp min(msize, out_buf_cap) keeps both tiers correct), the pouch `0020-pouch-srv-bulk.patch` (a pre-bind AF_UNIX setsockopt(SO_SNDBUF/SO_RCVBUF >= 128 KiB) -> DMSRVBULK at bind; any value accepted 0 as advisory), and Stratum-side `stm_stratumd_listen_unix(+sockbuf)` (best-effort SO_*BUF before bind; the TWO FS listeners pass `STM_STRATUMD_FS_SOCKBUF` = 2x STM_9P_MSIZE_DEFAULT -- the system mount AND the per-user home proxy both negotiate 128 KiB; /ctl + slate + host-fs stay default). Prosecute: the ring lifetime (heap bufs freed ONLY at the last unref, after teardown unparks every blocked party; no copy in flight at free -- each blocking op holds a conn ref); the role-park I-9 (register-then-observe on the role list; release-clears-then-wakes; a TIMEDOUT/INTR role wait never leaks the role or a stale hook -- poll.tla NoStaleHook); the role/deadline composition (`srvconn_client_recv`'s role wait honors `client_deadline_ns` + sets `client_timed_out`); frame atomicity (the writing role spans the whole buffer; two writers' bytes never interleave -- `srvconn.role_park_second_writer` pins A-then-B); the class-capture coherence (ring_msize read under the registry lock atomically with LIVE + rebind-identity so a minted conn's geometry never disagrees with its poster); the msize-proposal bound (proposal == the conn class, so negotiated <= what the rings carry); the send_frame bound (`n > c2s.cap` reject; freeb from ch->cap -- the LANDED-THEN-CAUGHT wedge: a stale SRVCONN_RING_CAP freeb made the first bulk Twrite eternal-EAGAIN [fsbench boot hang], fixed + pinned by the in-test big-frame regression); the full-duplex both-rings-full park (POSIX AF_UNIX-equivalent application deadlock; teardown/#811 unwinds -- documented, not a kernel hang); the two-point class policy (an arbitrary ring_msize is rejected at BOTH srvconn_create and srv_reserve -- no arbitrary-size kernel-memory demand; worst-case user-drivable exposure SRV_MAX_CONNS x ~516 KiB ~= 32 MiB, documented). **No new spec** per the 2026-05-23 broadening -- prose in CONCURRENT-FS.md CF-3 B + this row + `docs/reference/71-srvconn.md` + `47-9p-client.md` + `78-pouch.md` (the SO_SNDBUF mapping) + the focused audit + the srvconn tests (`bulk_ring_class` [incl. the wedge regression] / `role_park_second_writer` / `role_park_second_reader` / `client_send_blocking_backpressure`) + the exact-value cf3-bulk probe + boot OK + the SMP gate. **CF-3 B audit CLOSED (Fable-5-max prosecutor, MODEL(end)==pinned, + concurrent self-audit): 0 P0 / 1 P1 / 0 P2 / 1 P3, NOT dirty, both fixed in-commit -- F1 [P1] the blocking client send's POLLIN wake deferred to end-of-delivery (a poll-then-read byte server + a >ring write = circular wait; now wakes per accepted chunk; `srvconn.client_send_blocking_poll_edge`); F2 [P3] the role-cond `|| eof` teardown busy-spin (conds now wait purely on role-free; the holder's guaranteed release carries liveness). `memory/audit_cf3b_closed_list.md`.** |
| Allocator | `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c` | Allocation correctness, lock-free invariants |
| Scheduler | `kernel/sched.c`, `arch/arm64/context.c`, `kernel/smp.c` (IPI logic), `kernel/include/thylacine/spinlock.h` + `Thread.preempt_count` (the #360 preemption discipline: plain locks disable preemption per-thread; the gate in `preempt_check_irq`; the sched() lock-across-sleep assert; the raw pending-release pair -- ARCH 8.11) | EEVDF correctness, SMP, wakeup atomicity |
| ASID generation-rollover (RW-1 B-F1) | `arch/arm64/asid.{c,h}` (the rolling allocator: `asid_generation` + the bitmap + per-CPU `active_asids`/`reserved_asids`/`flush_pending`; `asid_check_and_switch` / `new_context` / the rollover), `kernel/include/thylacine/proc.h` (`_Atomic u64 context_id` replacing `u16 asid`), `kernel/proc.c` (drop `asid_alloc`-at-create + `asid_free`-at-reap), `kernel/sched.c` + `arch/arm64/context.S` (the C pre-switch hook refreshing `next->ctx.ttbr0`), `kernel/thread.c` (ASID no longer baked into `ctx.ttbr0` at create), `arch/arm64/mmu.c` (TCR_EL1.AS=1 from ASIDBits) | **I-31** (ASID rollover safety). SMP-race-bearing: a generation rollover concurrent with another CPU's context switch must not reassign an `active`/`reserved` ASID (else two address spaces alias one ASID -> the TLB returns a wrong translation -> cross-Proc corruption). **Model-first**: `specs/asid.tla` (clean + the `rollover_steals_active` buggy cfg) TLC-green BEFORE the impl (spec-first re-enabled for this surface, per the SMP precedent in §8.4). Prosecute: rollover/active-reserved preservation, the fast-path lockless `xchg`-publish vs a concurrent rollover, the slow-path lock order (`g_asid_lock` near-leaf; no rq-lock cycle), the per-CPU `flush_pending` local-flush completeness, kproc's ASID-0 bypass, the 8-vs-16-bit width. Design: ARCH §6.2.1 + `memory/project_asid_rollover_design.md`. |
| Process wait/reap (`SYS_WAIT_PID` v2 / `wait_pid_for`, U-7-pre) | `kernel/proc.c` (`wait_pid_for` + the filtered `wait_pid_cond` + the `wait_pid` wrapper), `kernel/syscall.c` (`sys_wait_pid_handler` 3-arg + dispatch), `kernel/include/thylacine/proc.h` (`WAIT_WNOHANG`), the libt + libthyla-rs wrappers (`t_wait_pid_for` + `Child::try_wait`) | Death-path (#788/#809/#811/#926 lineage). ABI: `SYS_WAIT_PID` (status_out) -> (want_pid, flags, status_out); `pid=-1,flags=0` = legacy reap-any-blocking. Adds a pid filter (`want_pid>0`; closes the `joey.c:2451` wrong-pid extinction) + `WAIT_WNOHANG` (returns 0 = not-ready; pid 0 is never a child). Reap teardown copied verbatim; only selection + blocking + the cond filter change. I-8 (no starve), I-9 (no lost wake; filtered cond re-checks under r->lock), death-path lifetime (alloc-stable `c->pid` filter, stack-local `wait_cond_ctx`). Closes #856. **No new spec / no new invariant** (2026-05-23 broadening) -- prose in `docs/reference/14-process-model.md` + the focused audit + the 3 `proc.wait_pid_for_*` tests + the SMP gate. |
| Territory | `kernel/territory.c` | Cycle-freedom, isolation |
| Handle table | `kernel/handle.c` | Rights monotonicity, transfer rules, hardware-handle non-transferability |
| BURROW | `kernel/burrow.c`, `mm/burrow_pages.c` | Refcount, mapping lifecycle |
| Weft cross-Proc Burrow-share (the I-37 dataplane substrate) | `kernel/burrow.c` (`burrow_share_into` + the #847 dual-refcount extended across two Procs), `kernel/syscall.c` (the Weft-6 EL0 delivery: `SYS_WEFT_SHARE`/`SYS_WEFT_MAP` keyed on the `/net` data fid), `kernel/include/thylacine/syscall.h` (`SYS_WEFT_SHARE = 81` / `SYS_WEFT_MAP = 82` + ABI), the netd + `/net` data-fid grant path | **I-37** (capability network dataplane integrity; `specs/weft.tla`, spec-first re-enabled). The tree's FIRST cross-Proc shared page -- prosecute hard. Delivery = **grant-is-the-share** (user-voted 2026-06-20; kernel-mediated, no Burrow handle crosses Procs, the capability is the namespace-gated flow fid, I-1/I-28). Prosecute: the #847 dual-refcount SMP-safety across two Procs *concurrently* mapping/unmapping one Burrow (no UAF / double-free / leak; free iff both refs drop -- the spec's `ShareBoundedByFlow`); the share is bounded by the flow + fully dropped on teardown (no stale mapping past clunk -- `NoStaleShareAccess`); W^X on the shared mapping (RW-only); the per-flow keying (one ring per connection N, NOT per SrvConn -- the `/net` SrvConn is shared across flows); netd owns the NIC (I-5) + the share reaches only a Proc holding the flow fid (I-1). **Weft-2** lands the substrate (no EL0 ABI); the EL0 delivery lands at **Weft-6** -- **lazy fid-keyed (Option B, user-voted 2026-06-20; NET-THROUGHPUT 6.1, superseding the earlier eager "data-fid auto-map" sketch):** a new `Tweft(F)->Rweft(share_id)` 9P op (`P9_TWEFT=142`/`P9_RWEFT=143` since the #371 renumber -- born 134/135, latently colliding with Stratum Tfadvise/Tpin; registry `docs/9P-EXTENSIONS.md`; the #845 Tflush precedent) + `SYS_WEFT_SHARE`(81; netd allocs+pins the ring, mints the `share_id`) + `SYS_WEFT_MAP`(82; the guest resolves data_fd->(client,F), issues `Tweft` on first zero-copy use, joins the `share_id`->the pinned Burrow, `burrow_share_into`s it in). Only a zero-copy flow builds a ring (the section-4.8 hybrid threshold); the `share_id` is kernel-internal netd->kernel (never handed to the guest -> unforgeable); the data-fid clunk drops both #847 refs. The focused buffer-lifetime-UAF audit + the new `Tweft`/`share_id` correlation (consumed-once + no cross-flow mis-binding) at **Weft-7**. **Spec-first** (`specs/weft.tla`: clean + the 4 buggy cfgs as pre-commit gates). Prose: `docs/NET-THROUGHPUT.md` section 6 + the full CLAUDE.md row + this row + the Weft-2 substrate test + the Weft-7 audit + the SMP gate. **G-2 WEAVE ADDENDUM (TAPESTRY.md §18.1 + §18.12; ABI user-signed-off 2026-07-19; the I-40 T-1 KERNEL SHARE HALF — the §28 I-40 row is the authoritative invariant statement):** `SYS_DMA_CREATE_WEAVE` (99) mints the KERNEL-MINTED create-immutable `KObj_DMA.weave` subtype (64 MiB envelope; the same CAP_HW_CREATE + I-34 gates; a separate number, not a flags widening — existing two-arg callers leave x2 garbage, the #112 class, and a garbage flags word could accidentally set share-admissibility); `burrow_share_into` + the `SYS_WEFT_SHARE` register gate admit ANON or that subtype ONLY (a device-command DMA region stays as structurally unshareable as MMIO — admissibility minted at allocation, never creator-asserted); `weft_claimed_kind` derives the binding kind from the kernel-minted type + cross-checks the server declaration (mismatch fails closed); a WEAVE binding has no ring view and the kind-gated `weft_binding_validate_rw` closes all three Tweftio fast paths at one chokepoint; `SYS_WEFT_UNSHARE` (100) = the owner-gated disarm (removal-before-page-free + fail-closed late claim — the `tapestry_present.tla` Map-guard NoStaleMap kernel half; closes #289); the client pin is budget-bounded (`Proc.shared_map_pages` @348, PROC_SHARED_MAP_MAX_PAGES = 128 MiB, the I-32 fifth axis, charged in `burrow_share_into` / uncharged via the `VMA_FLAG_SHARED_IN` pairing) and dropped at the weave fid's clunk (pid-matched unmap in `dev9p_close`); the RING path is byte-unchanged. Prosecute on any change: the gates stay kernel-minted-subtype-only; the kind derivation stays type-authoritative; the unshare pin drop stays outside the registry leaf; every flagged-VMA teardown uncharges; the client map stays Normal-WB. Tests `weft.{share_rejects_plain_dma, weave_share_and_claim, unshare_disarm, shared_map_budget_cap, weave_clunk_unmap_guard}` (both probe families revert-probed); spec gate re-verified (tapestry 5413 + 4 buggy; weft + readiness GREEN); `specs/SPEC-TO-CODE.md::tapestry_present.tla`; as-built `docs/reference/125-weft.md` "The weave share (G-2)". **Audit CLOSED CLEAN (Fable-5-max, MODEL start==end, + self-audit): 0 P0 / 0 P1 / 0 P2 / 3 P3, NOT dirty** — F1 [P3, FIXED] the stale-guest_va clunk-unmap → the identity-guarded `weft_binding_clunk_unmap`; F2 [P3] the WEAVE-dispatch E2E G-3-owed; F3 [P3] bare-pid closed-as-documented; 1170/1170 + SMP 40/40 0-corruption + the F1-delta amplifier; `memory/audit_g2_closed_list.md`. |
| tapestryd: the compositor + the orphaned-weave reaper (Tapestry G-3; I-40 present half) | `usr/tapestryd/` (the warden gather-bound persistent compositor: `gpu.rs` the synchronous virtio-gpu-PCI command engine [per-surface resources + whole-weave backing + offset TRANSFER + the retire pair]; `input.rs` the poll-mode virtio-keyboard-PCI eventq; `server.rs` the /dev/tapestry 9P server -- the surface state machine, the present engine, the Tweft mint, the retire ordering, the F2 per-conn scoping, the F9 caps, the R2-F4 WEDGE; `keymap.rs`), `usr/lib/libtapestry` + `usr/tapestry-demo` (the client + the per-boot gate's scanout owner), `usr/lib/libdriver` + `usr/warden` (the `gather = all` manifest mode: `resolve_gathered` + `BoundResources.pci_extra` + the `pcix` codec key -- ONE grant/Proc over several matched PCI functions), `kernel/weft.c` + `kernel/include/thylacine/weft.h` (the R2-F3 reaper: `weft_reap_register/unregister/sweep` + the kproc kthread), `kernel/syscall.c` (the CAS-winner registration), `kernel/dev9p.c` (the close-path unregister), `kernel/devdev.c` (the tapestry mount stub), `kernel/main.c` (the reaper spawn) | **I-40's PRESENT half (the authoritative invariant statement is the section 28 row) -- ON the G-2 weave-share surface + a NEW cross-Proc kernel reclaim path; prosecute hard.** The stage-0 quiesce property is STRUCTURAL: presents complete synchronously inside one dispatch, so the in-flight set is empty at every retire -- **any move toward a pipelined/async controlq MUST land a real drain first** (the spec's `ServerRelease` guard; the SPEC-TO-CODE line is the obligation). Prosecute: the retire ordering (`t_weft_unshare` BEFORE any backing free -- the R2-F5 Map-guard discharge; a reordering reopens `MapStale`); the REAPER's lock order (`g_weft_reap_lock -> g_proc_table_lock(irqsave) -> vma_lock -> v->lock -> buddy`, acyclic -- register/unregister run lock-free; nothing under gptl/vma takes the reap lock) + the reaper-vs-close serialization (the close unregisters under `g_weft_reap_lock` BEFORE reading the binding; the reaper NULLs `wb->burrow` under the same lock -- a reordering is a UAF/double-free); the cross-Proc unmap's identity guard (the G-2-F1 re-check under the target's vma_lock -- gptl pins the target ALIVE, the devproc precedent; a non-ALIVE match is SKIPPED, its own teardown owns it); the borrowed session pointers' lifetime (valid while registered; the priv's att-ref/client outlive the registration by the unregister-before-release order); the register-vs-close structural order (the CAS winner registers while the #844 Spoor ref pins the priv); the F2 per-conn scoping (owner + generation gates at EVERY surface-qid consumer -- walk, readdir, open, read, write, Tweft; a missed gate is the cross-client screen-scrape); the present validation (slot + rect vs the geometry, overflow-safe -- the untrusted-client boundary; the tpresent version pin); the R2-F4 WEDGE (non-droppable overflow force-retires, never blocks, never drops a control event for a live client); the F9 caps (per-conn surface count + dims <= display -- the weave is tapestryd's DMA, the client's own page cap does not bound it); the gather grant (every conferred bdf/INTID is some MATCHED node's own -- `resolve_gathered` re-checks membership; the I-34 never-fabricated property per axis). **No new spec** (the model was landed model-first at the design pass; BOTH impl halves left it unperturbed -- clean 5413) -- prose in TAPESTRY.md section 18 + `specs/SPEC-TO-CODE.md::tapestry_present.tla` (the G-3 server map) + `docs/reference/139-tapestryd.md` + this row + the focused G-3 audit + the `weft.reap_*` regressions + the per-boot pattern gate (-v through the full path + the liveness double-dump) + boot OK + the SMP gate. **#31 addendum (the live-display controlq fix)**: `submit_and_wait`'s completion authority is the USED RING, never the ISR bit -- the wait re-reads `used.idx` behind `virtio_rmb` per wake + a bounded spin until the command retires (a wake is only a hint: irqfwd collapses INTx edges, and under a live display backend a mis-timed wake is routine -- the pre-#31 single-shot read-and-assert desynced `seq` from the device's avail consumption permanently); any submit failure latches `Controlq.dead` (fail-fast, never the zeroed-response cascade). Prosecute on any change: the wait must never break on ISR alone; the dead latch must stay one-way; the synchrony (single-in-flight, complete-inside-one-dispatch) is UNCHANGED so the stage-0 quiesce argument stands. Coverage: `tools/interactive/ls-gfx-live.exp` (the VNC live-display leg; the cocoa trigger has no headless repro -- three escalating attempts documented in `docs/reference/139-tapestryd.md`). **G-6 ADDENDUM (the compositor: pane tree + resize protocol + the interaction layer; G-6a `fd5a3646` / 6b `e2161294` / 6c `1e259a36`; PURE USERSPACE -- the kernel byte-unchanged across the arc, so the standing SMP records carry).** ADDS `usr/tapestryd/src/pane.rs` (the i3 container tree -- ONE structural primitive [split-h/v | tabbed | stacked], nestable; monotonic never-reused PUBLIC pane ids; zoom-by-id; the D6 directional move; the D7 glyph-free strips) + server.rs growth (weave GENERATIONS [the 6b resize protocol] + the Super chord plane + TEV_FOCUS + multi-rect presents + the section-18.6 determinism mode behind the default-on `test-mode` cargo feature) + `usr/lib/libtapestry` (present_rects/present_hold/release; client MAX_RECTS=63 under the server cap 64). Prosecute on any change: **the 6b GENERATION FENCE** (the resize ack's Rwrite completes only AFTER the new generation is fully allocated [reply-after-alloc, R2-F5] + the conn stream is FIFO => every post-fence present validates/blits against the NEW geometry; the displaced generation drains passively [never read again -- tearing-freedom holds] and retires at the first post-fence present or with the surface [<=2 gens; a second ack while draining is E_AGAIN]; `release_gen` keeps the R2-F5 unshare-BEFORE-backing-free order PER generation; a retired surface releases BOTH generations); **the multi-rect present validation** (rect_count <= TPRESENT_MAX_RECTS=64; EXACT payload length 32+16*(k-1); EVERY rect validated overflow-safe [u32+u32 in u64] against the CURRENT geometry BEFORE any pixel work -- no partial present); **HOLD vs tearing-freedom** (a held present does its pixel work INSIDE the dispatch [Direct: transfer without flush; Composed: blit without screen push] -- client weave bytes read ONLY inside the present dispatch, so the quiesce/tearing-freedom argument stands; only the DEVICE-VISIBLE push defers into `Held::Direct/Composed`; `release` is F2-owner-gated; a non-HOLD present / test-mode-off flushes implicitly [no stuck regions]; a scanout-mode change stales the hold [dropped]; a hold on a pending direct SWITCH is E_AGAIN); **the chord plane** (while Super is held EVERY non-modifier key is compositor input -- the whole plane reserved, so no client can ever depend on a Super combo; a swallowed press swallows its release/repeats via the 256-bit `chord_down` set even past Super-up [no half-pair reaches a client]; pre-Super presses keep flowing; modifiers always flow); **the slot-vs-id discipline** (pane PUBLIC ids never reused [a stale pane fid resolves to nothing]; zoom held by id [a stale target self-clears at recompute]; `last_focus` cleared at retire [a reused surface slot must not suppress/misdirect a FOCUS pair]; move_dir's insertion index stays valid because `dissolve_if_single` REPLACES in place at the same index); **TEV_FOCUS** (emitted ONLY from `focus_sync` at the reconcile tail -- the single emission point; `last_focus` set BEFORE the pushes so a wedge-retire's nested reconcile re-enters harmlessly); **the determinism mode** (`test-mode`/`tick`/`release`/HOLD only under the default-on `test-mode` cargo feature -- a production `--no-default-features` build strips them to E_OPNOTSUPP [the #880 class; BOTH variants must compile]; the frozen clock stops ONLY the wall tick [input drain + deferred deliveries + 9P dispatch keep running]; mode-off releases every hold). **The pane + layout tree is deliberately conn-GLOBAL** -- the WM-control model (control is 9P / layout-as-9P; the i3-IPC / tmux shape), NOT a per-surface ACL; F2 gates SURFACES (pixels/events/present/weave/release), never the shared layout tree. **G-6d holotype: 0 P0 / 0 P1 / 1 P2 / 4 P3, NOT dirty** (Fable-5-max, MODEL(end)=Fable 5). **F1 [P2]** = the sharpest consequence of the global control surface: a session peer could `close` another client's pane [for the console renderer aurora that queues TEV_CLOSE -> it exits -> the graphical console darkens] or focus-steal its input -- **the v1.0 trust boundary is the per-territory `/srv` (I-1/I-28): `/srv/tapestry` lives in the driver's own territory, unreachable from a user session's namespace, so only the trusted boot chain connects -- no untrusted tapestry client exists at v1.0**; the per-client layout-control CAPABILITY + renderer-role protection + the D5 read ACL are the Halcyon-era multi-untrusted-client fix (task #42; a partial owner-scope now would break the global-WM model -- the no-overfit trap). F2 [P3] focus-change plain-key half-pair leak (task #43; benign for aurora, a raw-evdev client stuck-key), F3 [P3] global `clock-rate` is same-session-trust (doc'd), F4 [P3] 24-bit pane-qid pin (commented; unreachable at 2^24 allocs), F5 [P3] single-point pixel asserts + the 64-MiB triple-buffered-weave cap on 4K displays (task #44; graceful E_NOMEM, config-dependent). Validated by the G-6d holotype + `ls-gfx-panes` (22 legs: exact strip-color pixels, the zoom corner, move pixels, the QMP Super+Left chord, frozen-tick, the hold bracket) + `ls-gfx`/`ls-gfx-live` byte-identical (the stage-0 direct path unregressed) + boot OK + the spec unperturbed (5413) + the standing SMP records (kernel byte-unchanged). Closed list: `memory/audit_g6_closed_list.md`. |
| 9P client (pipeline restoration, #841) | `kernel/9p_client.c`, `kernel/9p_session.c`, `kernel/9p_transport.c`, `kernel/9p_attach.c`, the SrvConn transport boundary (`kernel/srvconn.c` client send/recv + `kernel/9p_srvconn_transport.c`) | Tag uniqueness (I-10), fid lifecycle (I-11), no-lost-wakeup (I-9 specialized to the per-rpc rendez), flow control, out-of-order completion. Restores committed §21/§21.10 pipelining (elected-reader, Plan 9 `mountio`) from the R15-c F230 serial regression — the single spinlock held across the blocking `recv` + the 30 s per-op deadline that desynced the shared byte stream (the stalk-3c-d soundness bug). Elected-reader = lock NEVER held across `recv`; multi-in-flight tag-demux; no per-op timeout (block until reply or ring-EOF, death-interruptible via #811). **No new spec** per the 2026-05-23 broadening — but `specs/9p_client.tla` (clean + the 4 buggy cfgs) is the pre-commit gate; prose validation in ARCH §21.10 + this row + the audit + the multi-in-flight runtime tests + the #841 UBSan/forkstorm repro. **#349 send-side flow control**: a transiently-full c2s ring is back-pressure, not death — `P9_TRANSPORT_EAGAIN` (srvconn n==0 → do_send sent==0-only) → `client_send_flow` self-pumps/parks-and-retries instead of marking the shared session dead (the on-device `go build` `snare:bus`+`-EIO` cascade). Senders park MULTI-WAITER on a `poll_waiter_list` (each on its own stack `Rendez` via a `poll_waiter`) — a single `Rendez` would extinct on the 2nd concurrent parker (the R2-F1 audit finding, an SMP-reachable panic on the parallel-`go build` workload). Audit converged clean over 2 rounds (R1 1P1+1P2+1P3, R2 0/0/0/1); the full prosecution + close is the CLAUDE.md §"Audit-triggering changes" 9P-client row addendum; closed list `memory/audit_349_closed_list.md`. v1.x seam #350 (async `submit_async` EAGAIN-as-fatal, pre-existing). |
| poll | `kernel/poll.c` | Wait/wake across N fds, missed-wakeup-freedom (I-9), poll-hook list lifetime |
| Notes / signals | `kernel/notes.c`, `kernel/devnotes.c`, `kernel/include/thylacine/notes.h`, `kernel/proc.c` (synthetic `child_exit` post in `exits`), `kernel/pipe.c` (synthetic `pipe` post on write-to-closed), `arch/arm64/exception.c` (EL0-return-tail delivery) | Delivery ordering (I-19); async-safety (delivery only at zero-lock EL0-return tail); N-2 consumed-exactly-once across handler + fd-read paths; N-3 in_handler re-entrancy guard; N-4 `kill` non-catchable. **No `specs/notes.tla`** per the 2026-05-23 spec-to-code broadening — prose validation in `notes.h` + the audit + the runtime test suite are the rigor. |
| Capability checks | All syscall entry points | Privilege correctness |
| KASLR / ASLR | `arch/arm64/start.S`, `kernel/aslr.c` | Entropy quality, layout correctness |
| Crypto code | None in v1.0 kernel; janus in userspace | Side-channel, key handling |
| ELF loader | `kernel/elf.c` | RWX rejection, relocation correctness |
| `AT_HWCAP` exec-auxv CPU-feature word (the CF-4 A AEAD lever) | `arch/arm64/hwfeat.{c,h}` (`g_hw_features.linux_hwcap` — the Linux-uapi-numbered word derived from ID_AA64ISAR0_EL1 + ID_AA64PFR0_EL1 at `hw_features_detect`: FP/ASIMD [PFR0 inverted-sentinel fields] + AES/PMULL/SHA1/SHA2/SHA512/SHA3/CRC32/ATOMICS/ASIMDDP), `kernel/include/thylacine/elf.h` (`AT_HWCAP = 16` — the STANDARD SysV tag, unlike the deliberately-private `AT_VDSO_CLOCK`), `kernel/exec.c::exec_fill_auxv` (+1 unconditional entry; both frame shapes route through it), `kernel/include/thylacine/exec.h` (`EXEC_INIT_AUXV_COUNT` 7 -> 8; Shape-A frame 160 -> 176); consumers: pouch libsodium (`tools/build.sh` compiles the self-arming armcrypto TUs, runtime-gated on `getauxval(AT_HWCAP) & (1<<3)`), the Go fork (`os_thylacine.go` sysargs -> `internal/cpu.HWCap` + `cpu_arm64_thylacine.go`) | The CF-4 A AEAD lever (CONCURRENT-FS.md; measured: the go-build cold window was 20.0 s of 20.7 s inside SOFT AEGIS-256 decrypt while the CPU's AES extensions idled behind the missing feature word). **An exec-ABI addition — audit-noted, not privilege-bearing**: the word is read-only boot-derived info (I-15: derived from ID regs, like the banner), leaks no secret/layout (no KASLR interaction), and is APPEND-ONLY (a consumer treats unknown-clear as absent). Invariants: the bits must be TRUTHFUL (a set bit whose instructions extinct on this CPU = a SIGILL time bomb in every consumer; a clear bit = silent soft fallback — fail-safe by construction on crypto-less cores, RPi4's A72); **`hwcap_CPUID` (bit 11) must NEVER be set** while Thylacine lacks Linux's EL0 MIDR trap-and-emulate (Go's `hwcapInit` calls `getMIDR()` — an EL0 `mrs midr_el1` — when it sees CPUID; on Thylacine that is `snare:ill`); auxv consumers scan by tag to AT_NULL (verified: musl, libthyla-rs, the Go fork — no fixed-offset parser exists), so the +1 entry is layout-safe. RECORDED SEAM (audit F5): the word is BOOT-CPU-derived (before smp_init); a heterogeneous target must AND-in secondary ID regs at smp_init before the first exec (the Linux sanitized-view analog) — all current targets are homogeneous and the W1.5 LSE patcher carries the identical assumption. Proof: the `exec.setup_auxv` kernel tests (tag order + the FP\|ASIMD floor + the hwcap value) + the pouch-hello-sodium AGREEMENT probe (`sodium_runtime_has_armcrypto()` must equal the AT_HWCAP AES bit — proves the hardware path live on AES CPUs AND the fail-safe on AES-less ones, boot-fatal) + the bench re-measure. **No new §28 invariant; no new spec** per the 2026-05-23 broadening — prose in CONCURRENT-FS.md CF-4 A + this row + the focused audit + the SMP gate. |
| `burrow_attach` / `burrow_detach` | `kernel/syscall.c` handlers, `kernel/burrow.c`, `kernel/vma.c` | Anonymous-memory syscalls (§6.5 Tier 1) — VMA + Burrow refcount lifecycle, VA placement, per-Proc lock, W^X (RW-only) |
| Initial bringup | `kernel/main.c`, `init/joey.c` | Boot ordering correctness |
| Proc exit handle-close (#926) | `kernel/proc.c` (`proc_close_handles_at_exit` + the top-of-`exits()` gated call + the `thread_exit_self()` no-op note + the `proc_free` fallback), `kernel/handle.c` (`handle_table_free` lockless-safety comment) | #926 (U-6f prerequisite, user-voted 2026-06-08): a SINGLE-thread Proc closes its handle table at EXIT (top of `exits()`, while RUNNING + ALIVE + `thread_count==1`) rather than at reap, so inherited fds (pipe write ends) close at process termination and a peer draining its stdout sees EOF immediately (correct Unix/Plan 9 semantics; fixes the `$(cmd)` drain-to-EOF hang). The death path is the #788/#860/#809/#811 surface -- prosecute hard. Invariants: no sleep-in-EXITING (t is RUNNING at the close -- the close cascade may 9P-clunk-sleep); no reaper UAF (the reaper only frees ZOMBIE Procs; the close runs while ALIVE so `wait_pid` cannot reap mid-close); the `thread_count==1` lockless gate is stable (decrement is reap-only); I-7 (the #847 Burrow dual-refcount makes the handle-close-at-exit / vma-drain-at-reap ordering inversion safe). Multi-thread Procs keep close-at-reap (their EXITING mark is atomic-under-lock with the last-thread determination -- a v1.x EXITING-protocol restructure lifts it). **SMP-gated** (default+UBSan x smp4/smp8 N=10, 0 corruption) + focused audit (0 P0 / 0 P1 / 0 P2 / 3 P3 doc). **No new spec** -- prose in `docs/reference/14-process-model.md` + this row + the audit + the `u-test` #926 regression. |
| pouch lower half + kernel additions | `usr/lib/pouch/` (the syscall seam; socket / thread / signal translation), `kernel/` auxv population + the `torpor` wait-on-address syscall + the allocator-backend call | The POSIX→Thylacine boundary; invariants P-1..P-4 (POUCH-DESIGN.md §11); the `torpor` wait/wake (`futex.tla`). Phase 6 surface — rows enumerated per sub-chunk in POUCH-DESIGN.md §14. |
| FS-mutation syscalls (create / fsync / readdir) | `kernel/syscall.c` (`sys_walk_create_handler` / `sys_fsync_handler` / `sys_readdir_handler`), `kernel/dev9p.c` (real `dev9p_create` + new `dev9p_fsync` / `dev9p_readdir`), `kernel/devramfs.c` (create/fsync/readdir impls), `kernel/include/thylacine/dev.h` (new `.fsync` / `.readdir` vtable slots), `kernel/include/thylacine/syscall.h` (`SYS_WALK_CREATE = 54` / `SYS_FSYNC = 55` / `SYS_READDIR = 56` + ABI), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers | Convergence-detour FS foundation (IDENTITY-DESIGN.md §9.2), pulled ahead of A-1b corvus persistence. The create + write + fsync path is the AEGIS/mallocng-adjacent surface from Phase 6 — prosecute hard. Rights gates (`RIGHT_WRITE` on parent for create/fsync; `RIGHT_READ` for readdir); single-component name bounds + `/`-`\0` reject; `perm` reserved-`DM*`-bit reject; `DMDIR`-fold (mkdir vs lcreate); `readdir` buffer bounds + offset advance; durability contract; dev9p fid lifecycle on create/clunk (UAF on a failed create path). Per-file rwx enforcement is A-2d, NOT this surface (I-22 holds — no enforcement exists yet to bypass). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in IDENTITY-DESIGN.md §9.2 + this row + the audit + the runtime tests. **#99 addendum (the #102 create errno-loss fix):** `dev9p_create` collapsed every `Tlcreate`/`Tmkdir` `Rlerror` to `NULL` and `sys_walk_create_handler` returned a bare `-1` (== `-T_E_PERM`, the generic EPERM/EIO sentinel), so EL0 could not distinguish `-EEXIST` (a concurrent/duplicate create — POSIX `open(O_CREATE)` without `O_EXCL` opens the existing file) from a real failure (gopls's parallel filecache logged "operation not permitted"). Fix: a transient `dev9p_priv.create_errno` records the p9 client's real `-errno` on the two `Rlerror` paths; the handler returns `dev9p_create_errno(nc)` (self-gating: `-1` for non-dev9p / unrecorded; **clamped** to the `[-4095,-2]` passthrough range, defense-in-depth over I-14's ecode bounding) instead of `-1`. Prosecuted: no stale/shared `create_errno` (the create-target `nc` is a fresh clone-walk priv, kzalloc'd `create_errno=0`, read once before `spoor_clunk`); no behavior change for non-dev9p Devs; the errno reaches EL0 correctly so the go-fork's `os.OpenFile` retries `Open` on `EEXIST`. **F1 (the SMP-gate + holotype catch -- the load-bearing half):** the errno-propagation ALONE spuriously failed under a race (2/10 boots) -- a loser's `Open`->ENOENT installs a NEGATIVE `(parent,name)` dentry, so its retry-`Open` served the stale negative RPC-free and got ENOENT. Both `dev9p_create` failure arms now, on `rc == -T_E_EXIST`, call `larder_dentry_invalidate_name(parent_path, name)` before returning NULL (EEXIST proves existence -> the negative is stale; the call also bumps the Larder gen so a concurrent negative-install is skipped, closing the re-cache race) -- the loser's own EEXIST-invalidate runs before its retry-Open, which then sees the file (the *success* path already invalidated; the EEXIST arm returned NULL before reaching it). Verified **10/10** under the 8-way `go-fs` step-6b concurrent race. F2/F3: the mkdir arm latches `fid_suspect` (G2-symmetric) + the dir-create mid-sequence failures record their real errno. Regressions: `dev9p.create_errno_propagates_eexist` (asserts the `-17` return AND the dentry drop) + `go-fs` step 6b (race) + step 6c (deterministic O_EXCL-EEXIST). Focused holotype (Fable 5): 0 P0 / 1 P1 (F1, fixed) / 0 P2 / 4 P3 (F2/F3/F4 fixed, F5 documented); `memory/audit_99_closed_list.md`. See `docs/reference/96-fs-mutation.md`. |
| FS-mutation syscalls (rename / unlink) — FS-gamma | `kernel/syscall.c` (`sys_rename_handler` / `sys_unlink_handler`), `kernel/dev9p.c` (new `dev9p_rename` → `p9_client_renameat`, `dev9p_unlink` → `p9_client_unlinkat`), `kernel/include/thylacine/dev.h` (new `.rename` / `.unlink` vtable slots), `kernel/include/thylacine/syscall.h` (`SYS_RENAME = 57` / `SYS_UNLINK = 58` + `SYS_UNLINK_REMOVEDIR` + ABI), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers | Convergence-detour FS-gamma (IDENTITY-DESIGN.md §9.3), pulled ahead of A-1b to give corvus's identity-DB persistence the classic write-tmp + fsync + atomic rename-swap substrate. **First end-to-end exercise of `p9_client_renameat` / `unlinkat`** (implemented Phase 5, never driven by any syscall) — prosecute hard, same AEGIS/mallocng-adjacent write-path class as §9.2. Invariants: rights gates (`RIGHT_WRITE` on every directory fd mutated — both for rename); single-component name bounds + `/`/`\0`/`.`/`..` reject on every name; the **cross-Dev + same-session reject** (rename runs directly on the two looked-up dir Spoors — no clone-walk, since renameat doesn't transition the dirfid — and requires the same Dev, with `dev9p_rename` adding the same-`p9_client` check; rejected at the handler before any Dev op); `flags` validated against `{0, SYS_UNLINK_REMOVEDIR}`; the no-fid-leak property (these ops borrow the caller's dir fid and allocate no transient fid, so the §9.2 failed-create UAF class does not arise); rename's POSIX atomic-replace semantics; the rename-swap durability detail (post-rename `Tsync` on the parent dir as the metadata barrier — validated end-to-end by the A-1b cross-reboot persistence test). The pre-existing `void (*remove)` Plan 9 slot is left as-is (wrong shape; `SYS_UNLINK` uses the new `.unlink`). **No new spec** per the 2026-05-23 spec-to-code broadening — prose validation in IDENTITY-DESIGN.md §9.3 + this row + the audit + the runtime tests. |
| Capability-scoped service storage | `usr/joey/joey.c` (post-pivot: create `/var/lib/corvus` → `handle_dup`→`R|W` → endow as corvus's storage fd at spawn), `usr/corvus/src/main.rs` (use the handed fd as `storage_root` for all persistence), `usr/lib/libthyla-rs`; depends on **FS-delta** (the `SYS_WALK_OPEN` `T_OPATH` walk-without-open primitive, landed first; IDENTITY §9.4) | Convergence detour A-1.7 (ARCH §3.6; NOVEL §3.10 lead angle #10) — the storage-capability substrate, proven on corvus. Invariant **I-23** (FS authority bounded by the handed capability). Prosecuted: rights reduction (`R|W`, no `TRANSFER` -- least authority; monotonic I-6 through the spawn-fd endow); confinement (the chroot'd service cannot name a path outside its subtree); the shared-9P-session lifetime (corvus outlives joey -- SOUND via the `p9_attached_ref` chain). **Audit R1 CLEAN** (0 P0 / 1 P1 / 1 P2 / 3 P3): F1 (the "withholding `TRANSFER` blocks re-handing" claim was FALSE -> scripture-corrected to the monotonic bound), F2 (confinement is cooperative -> corvus now chroots FIRST), F5 (`T_OPATH` born `R|W`); F3/F4 P3 documented. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in ARCH §3.6 + this row + the audit + the corvus confinement smoke. |
| POUNCE: fused walk+getattr resolution (`Twalkgetattr` 140/141 + the stalk pounce + `SYS_STAT` = 88) | `kernel/9p_wire.{c,h}` (the 140/141 codec) + `kernel/9p_client.c` (`p9_client_walkgetattr`; NOFID query mode), `kernel/include/thylacine/dev.h` (the OPTIONAL `walk_attrs` vtable slot), `kernel/dev9p.c` + `kernel/devramfs.c` (slot impls), `kernel/stalk.c` (the pounce fast path: run-gather + the fail-ordering post-scan + the mount-mid-run split + `STALK_STAT`), `kernel/syscall.c` (`sys_stat_handler`), consumers (go port `os.Stat`/`os.Lstat`, `usr/lib/libthyla-rs` `fs::metadata`, pouch stat family); **Stratum-side**: `src/9p/server.c::h_walkgetattr` (the 3-phase fast path + per-component attrs + the NOFID no-bind arm) | POUNCE (`docs/POUNCE-DESIGN.md`; user-voted 2026-07-07 — phased, attr cache Phase-2-gated on the post-measure residual). **Path resolution is a privilege boundary (I-28) AND a new wire op — prosecute hard.** NO new §28 invariant (realizes I-28's per-component X-search at 1 RPC/run: attrs arrive FUSED with the walk — fresh every check, zero staleness; the earlier per-hop `spoor_stat_native` RPCs retire). Prosecute: the **fail-ordering invariant** (the post-scan consumes components strictly left-to-right; an X-denial at k MASKS everything past k INCLUDING a deeper walk-miss -> `T_E_ACCES` never `T_E_NOENT`, so no existence probe under a forbidden dir — check-after-batched-walk must preserve today's information surface exactly); **walk parity** (pounce == the per-component loop for every path shape: `.`/`..` runs, mount-mid-run split [take the resolution UP TO the mount point, re-walk the prefix, cross, resume], partial walk, over-long component, `STALK_MAX_DEPTH`); **fid lifecycle** (the batched fid clunks exactly once on every early-exit path — deny / mount-split / OOM; the NOFID query binds NOTHING on either end incl. the partial-walk server arm); the **Rwalkgetattr parser bounds** (nwqid vs body length; per-element truncation); **`SYS_STAT` uaccess** (path in / 80-byte `t_stat` out, the #3 errno clamp; X-search identical to the O_PATH+fstat emulation it replaces — POSIX stat authority = path-X only, #46). **No new spec** per the 2026-05-23 broadening (no new tag/fid semantics: one tag, at most one newfid, never installed for NOFID/partial) — `specs/9p_client.tla` buggy cfgs re-run as the pre-commit gate; prose validation in `docs/POUNCE-DESIGN.md` + this row + the focused audit (P-5) + the parity/masking kernel tests + the bench + RT-recount measure (P-4) + the SMP gate. **P-3 as-built additions to prosecute:** the **per-session capability latch** (`p9_client.wga_unsupported` + the `DEV_WALK_ATTRS_UNSUPPORTED` sentinel: Twalkgetattr is a Stratum extension and netd's `/net` answers it Rlerror ENOSYS -- dev9p latches the first ENOSYS per session and the resolver degrades to the per-component loop; prosecute that the sentinel NEVER reaches `walkqid_free` [a static object], that the fallback path is the byte-identical audited loop, and that a NULL return stays distinct [a real first-component miss]); the **carried-attrs optimization** (the previous run's fused leaf record seeds the next run's base X-check and the STALK_OPEN final-hop R/W check -- prosecute every invalidation edge: cross-on-descent, quarry-cross, '..' pop, old-path hop, split push); the **'..'-disables-pounce rule** (`path_has_dotdot` -- runs compress the trail, so a pop into a pounced run has no Spoor; any '..' takes the whole path down the per-component loop) + the **logical-depth axis** (runs compress `depth`, so the STALK_MAX_DEPTH surface is enforced on components-consumed while pouncing); the **strict walk_attrs shape contract** (full BIND walk MUST transition nc; partial/query MUST NOT -- a violating Dev is failed closed, never pushed). **P-4 CLOSE (2026-07-07)**: the fresh-twin gofmt baseline pair cold 21.8/22.1 s (-19-21% vs the pre-POUNCE fresh-pool CF-2f) / warm 8.2/8.3 s; the RT recount: the metadata trio 75k-class -> ~17k cold window, warm Twalk 23.3k-class -> 10, with the warm residual PER-OP server cost (#367, fresh-pool leaf-root scans) NOT op volume; the 1:1 Tgetattr:Twalkgetattr signature = the per-stalk base X-check, NOT fd-fstat -> **the section-8 Phase-2 attr cache is DEFERRED (gate not met)**; measure + decision in `docs/reference/131-pounce.md`. **P-5 CLOSED CLEAN (Fable-5-max prosecutor, MODEL start==end; + concurrent self-audit, 24 checks): 0 P0 / 0 P1 / 0 P2 / 1 P3, NOT dirty** -- the fail-ordering invariant SOUND under every constructed adversarial path; F1 [P3] = the latched-fallback double base-X Tgetattr (2 Tgetattr/component on a wga_unsupported session's per-component fallback -- idempotent, /net setup-only; TRACKED #372, reorder-the-base-X-after-the-sentinel deferred to avoid churning the just-audited deny/release ladder). SMP gate PASS 40/40 (default+UBSan x smp4/smp8 N=10, 0 corruption). `memory/audit_pounce_closed_list.md`. |
| The Larder: guest-side 9P FS cache (L1c substrate + attr sub-cache; L1d dentry sub-cache; L1e page sub-cache + cacheability gate; I-38) | NEW `kernel/include/thylacine/larder.h` + `kernel/larder.c` (`struct larder` on `p9_client` -- a dedicated near-leaf `lock` + a bounded `LARDER_ATTR_ENTRIES`=256 LRU array of `{qid_path, cvers, valid, lru, t_stat}` + a monotonic `gen`; `larder_init` / `larder_attr_serve` [Read] / `larder_gen_snapshot` / `larder_attr_install` [Open/Refetch, gen-guarded] / `larder_attr_invalidate` [OwnWrite]), `kernel/include/thylacine/9p_client.h` (embeds `struct larder larder`) + `kernel/9p_client.c` (`larder_init` in `p9_client_init`), `kernel/dev9p.c` (the hooks: serve+miss-populate at `dev9p_stat_native`; per-component free populate at `dev9p_walk_attrs`; invalidate at `dev9p_write` / `dev9p_wstat_native` [file] / `dev9p_create` [parent AND child] / `dev9p_rename` [both dirs] / `dev9p_unlink` [parent]) | The Larder (`docs/LARDER-DESIGN.md`; the measured top lever of the on-device `go build`; user-voted design 2026-07-09). **On the most-audited FS path (dev9p / 9p_client / stalk); a new SMP-shared cache on the base X-check + fstat serve -- prosecute hard.** Realizes **I-38** (cache coherence): a hit returns exactly what a fresh RPC would under close-to-open. **NO new invariant beyond I-38** (composes I-28 base-X-check TOCTOU, I-32 bound, I-10/I-11 untouched -- above the fid/tag layer). The attr serve is a `Read` (serve a cached attr with NO cvers re-check -- the RPC-elision win); coherence is (a) own-write invalidation + (b) `walk_attrs` revalidate-by-overwrite, so **NoWrongRead is ABSOLUTE single-writer**. Prosecute: the **serve/invalidate SMP atomicity** (serve COPIES the `t_stat` out under the leaf lock; invalidate drops under it -- no torn serve; the lock is a pure leaf, never held with `c->lock` or across an RPC, #360); the **populate GEN guard** (the spec's `Open` is an atomic read-and-install; the impl reads via RPC then installs, so `gen` captured pre-RPC + re-checked at install SKIPS a populate that raced an invalidate -- closes the populate-after-invalidate RESURRECTION, an unbounded-staleness/I-28 violation; `fs_cache.tla`'s atomic `Open` realized); the **create-reuse child-invalidate** (Stratum reuses freed inos -> a created qid.path may carry a stale prior-occupant attr, and the create path never runs `walk_attrs`, so `dev9p_create` MUST invalidate the child too -- the `stalk-2` delete+recreate+create-in-it failure, regression `dev9p.create_invalidates_reused_child`); the **`qid.path`=0 key** (root's qid.path is 0x0 -> an explicit `valid` bit, not a path==0 sentinel); the **Bounded LRU** (I-32; never exceeds capacity); the **never-populate-from-readdir** rule (L1a-2 audit-F1; readdir qid.version is a link-time si_gen snapshot, not si_cvers); the **base X-check bounded staleness** (a tightened dir perm via the guest's own chmod invalidates immediately; an out-of-band tighten is bounded by the next revalidation -- the accepted I-28 TOCTOU). v1.x seams (LARDER-DESIGN §11): writeback, persistent, cross-session, readdir-listing, per-key gen, the L1e page-cache /net content-versioned gate. **Spec-first RE-ENABLED (`specs/fs_cache.tla`, L1b)** is the pre-commit gate; **No new spec at L1c** (the gen guard is the impl realization of the modeled atomic Open -- prose). Prose validation in `docs/LARDER-DESIGN.md` + `docs/reference/132-larder.md` + this row + the L1f focused audit (the whole L1 arc: substrate + attr + dentry + page) + the `larder.*` + `dev9p.create_invalidates_reused_child` tests + boot OK (1055/1055) + the SMP gate. **L1d ADDENDUM (the dentry sub-cache -- `(parent-qid.path, name) -> {child-qid.path, type, negative}` incl. NEGATIVE entries; the same `struct larder` lock + `gen`; a bounded `LARDER_DENTRY_ENTRIES` LRU array; `larder_dentry_serve` / `larder_dentry_install` [pos + neg, gen-guarded] / `larder_dentry_invalidate_name` [the mutated name only, siblings preserved]).** Serve at `dev9p_walk_attrs` before the RPC: a FULL positive run in the **query** form (`SYS_STAT`'s final run, `nc==NULL`) skips the `Twalkgetattr` (the os.Stat win), and a NEGATIVE hop (in either form) serves the walk-miss verdict RPC-free (the LWN negative-lookup storm win); a bind-form full walk still RPCs (the server must bind the fid) but re-populates. Populate from the `walk_attrs` reply (positive per walked component + negative on the miss), NEVER from readdir. **Coherence = OWN-WRITE INVALIDATION ONLY, NOT a cvers gate (the L1d GROUND-TRUTH CORRECTION):** a name->child binding is a PARENT-dirent fact, and Stratum surfaces no directory-content version that tracks it -- verified in `src/fs/fs.c`, a child create/unlink touches only the separate dirent index and does NOT `stm_inode_set` the parent, so the parent `si_cvers` does NOT bump (only rename stamps it) -- so a parent-cvers compare would FALSELY MATCH a stale negative dentry after a create; the sole mechanism is dropping the mutated (parent, name) binding on the guest's own `dev9p_create` / `dev9p_rename` (both endpoints) / `dev9p_unlink` (only the named binding stales -- a create of foo cannot change whether a sibling bar exists -- so siblings are preserved and the drop is O(1) via the serve's (parent,name) hash; the whole-parent scan is retired). Maps onto `fs_cache.tla`'s `Read`+`OwnWrite` single-writer subset (NoWrongResolution absolute; no `Open` gate for dentries -- the attr sub-cache's exact discipline, keyed by `(parent,name)` instead of `qid.path`). Additionally prosecute: the **multi-hop serve atomicity** (the whole dentry chain + each hop's attr-cache lookup run under ONE Larder-lock hold -- an atomic snapshot, no torn multi-word read; a concurrent invalidate precedes or follows the whole serve); the **positive-serve attr coupling** (a positive hop's reply attr comes from the attr sub-cache -- a miss there bails the serve to the RPC, so a served qid always has a coherent attr); the **query-only full-serve** (a bind-form full walk MUST NOT be cache-served -- it needs a server-bound fid; only the query form + any-form negative skip the RPC); the **Territory-independence** (the dentry cache holds the raw dev9p-tree bindings -- `walk_attrs` walks the 9P server, not the per-Proc mount overlay -- so a mount point is a positive dentry the resolver's own mount scan crosses, and a negative dentry can never be a mount point since a mount point must exist in the underlying tree); the **name bound** (`LARDER_DENTRY_NAME_MAX` -- a longer component is simply not cached, fail-safe to the RPC); the **gen-guarded populate** (same as L1c -- a populate that raced an invalidate is skipped). **NO spec extension at L1d** (the dentry cache is the modeled `Read`+`OwnWrite` subset -- the parent-cvers gate the scripture originally sketched is REMOVED as unsound, so no new action; the single-writer cfg is already green). The Stratum create/unlink-doesn't-stamp-parent-mtime is a latent POSIX gap tracked separately (not a v1.0 requirement -- own-write invalidation makes L1d sound regardless). **L1e ADDENDUM (the page sub-cache + the LOAD-BEARING cacheability gate).** ADDS `larder_page_serve` [Read] / `larder_page_install` [Open/Refetch, gen-guarded] / `larder_page_invalidate` [OwnWrite] / `larder_destroy` on the same `struct larder` -- a bounded `LARDER_PAGE_ENTRIES` (512 at L1e landing -> 8192 [#25] -> 32768 [task #29 -- sized to hold the go-build read working set; measured compound warm win, `larder_page_invalidate` became O(pages-of-file) via the `page_qhash` qid_path secondary index; see `docs/reference/132-larder.md`]) LRU table keyed `(qid.path, page_index)` of `{cvers, valid_len, valid, lru/CLOCK, hnext, qnext, u8 *page}`; each slot's 4 KiB `page` buffer is HEAP (lazily kmalloc'd, reused across evictions, freed at `larder_destroy` from `p9_client_destroy` -- the attr/dentry entries are inline, only the page buffers are heap). Hooked at `dev9p_read` (serve the one page containing the offset, fresh + in range, RPC-free; populate each page the read covered FROM ITS ALIGNED START) + `dev9p_write` (own-write `larder_page_invalidate` alongside the attr invalidate). **The load-bearing gate: a per-`p9_client` `cacheable` flag (default false, `__atomic` relaxed) latched true by a successful `Twalkgetattr` in `dev9p_walk_attrs` -- the v1.0 proxy for a content-versioned, offset-stable FS; the whole Larder (attr serve/install in `dev9p_stat_native`, page serve/install in `dev9p_read`, dentry `walk_serve`) engages ONLY when it is set.** A stream/control server (netd `/net` -- consuming reads, `qid.version` always 0) answers `Twalkgetattr` Rlerror ENOSYS, never latches `cacheable`, and is never cached (a re-read of an offset would serve stale stream bytes; an attr the network mutated out of band would serve stale). Fail-safe (default false; a file is resolved via `walk_attrs` before it is read, so the gate is settled first); it also **closes the latent L1c gap** (attr caching had no server gate -- netd attrs could serve stale). netd is the only non-Stratum dev9p client at v1.0 (corvus is a byte-mode SrvConn -- no Larder; `/proc` `/ctl` `/env` `/dev` are native kernel Devs -- no dev9p, no Larder). Additionally prosecute: the **page-buffer lifetime SMP-safety** (a serve COPIES bytes out UNDER the leaf lock; evict reuses [never frees] a buffer; invalidate/destroy free UNDER the lock -- so a concurrent serve can never read a freed buffer; the `l->lock -> buddy zone->lock` order, kmalloc non-blocking under the leaf lock -- LARDER-DESIGN §7 anticipated this); the **cvers freshness gate on serve** (`page.cvers == c->qid.vers`, the fid's open-time version -- the close-to-open `Open` discipline; a cross-open external write [`qid.vers` bumped] misses + refetches; own-writes invalidate immediately so within a client the gate is strong); the **partial-page soundness** (a serve reads only `[0, valid_len)` and misses beyond, and populate caches only from a page's aligned start [no hole], so a partial small-file/EOF page is served soundly WITHOUT an EOF determination -- a short read from the RPC could be msize-clamping, not EOF, so an EOF-assuming cache would be unsound); the **gen-guarded populate** (same as attr/dentry -- a fill that raced an own-write is skipped, the resurrection close); the **whole-file over-invalidation** on write (sound; a per-range invalidate is a v1.x tuning); the **cacheability gate completeness + fail-safety** (all three serve paths + both populate paths gated; unproven -> not cached; the POUNCE-support proxy is not identical to the true property -- a future POUNCE-but-streaming server needs an explicit attach-time capability, a v1.x add). **NO spec extension at L1e** (a page is a content token like an attr -- the modeled `Read`+`Open`+`OwnWrite`; the cvers gate IS the `Open`, own-write IS `OwnWrite`; `fs_cache.cfg` unchanged). Tests: `larder.page_*` (10) + `dev9p.page_cache_serve_and_gate` (non-vacuous: 1076/1077 FAIL with the serve disabled -- the sentinel-offset "was a Tread sent?" probe). Prose in `docs/LARDER-DESIGN.md` §3.3/§4/§9/§11 + `docs/reference/132-larder.md` + this row; 1077/1077 + boot OK + reduced SMP gate 20/20 (default+ubsan-smp4, 0 corruption) -> the L1f focused audit + full SMP gate + gofmt re-measure closes the arc. v1.x page-cache seams (LARDER-DESIGN §11): precise per-range write-invalidate, multi-page serve, pin-and-copy-outside-lock, lazy-heap page metadata for non-cacheable clients. **D44 ADDENDUM (the CHASE read band, 2026-07-11): aligned wire reads + attr-served EOF at `dev9p_read`.** The D44 miss-class instrument proved 82% of warm wire-read misses were PERMANENT PARTIAL-PAGE HOLES: the msize payload (131049 B) is not a page multiple, so a sequential stream's chunks each end in a partial page; populate cannot fill a partial-FRONT page (aligned-starts-only), so the hole persisted and every re-read pv-missed at it and re-wired the whole tail -- the dominant victims being the go toolchain's multi-MB rodata+data segments, eagerly `dev->read` PER EXEC by `map_eager_from_file` (REVENANT demand-pages only text; `go` = 6.1 MB/exec, `compile` = 8.7 MB x 91 execs = 792 MB in one cold build). TWO guest-local fixes: **(a) the aligned wire read** -- a big (> one page) unaligned read on a CACHEABLE client is wired at the containing page's ALIGNED start and returned as a legal SHORT read (an in-buffer forward-copy shift; zero allocation; <= 4095 duplicate bytes per chunk ~3%), so each chunk fully REWRITES its predecessor's partial tail page -- holes heal in one pass and the second pass serves from pages (this RETIRES the partial-front-page-caching seam by construction); **(b) the attr-served EOF** -- a FRESH cached attr (`cvers == the fid's open-time qid.vers`, the page-serve freshness rule; PLAIN FILES only, so a dir's cached size cannot convert the server's read-on-directory error into a silent 0) answers the sequential reader's final read-returns-0 probe RPC-free (`larder_attr_fresh_size`; a NEW serve source whose EOF determination comes from the attr's authoritative open-time size, NOT from a short page -- the no-EOF-from-pages rule stands). Prosecute additionally: the shift's short-read legality at every consumer (all in-tree big readers loop; small reads <= one page keep the byte-exact path -- they never populate, so they cannot create holes); the **`got <= lead` arm's EOF/short split (the D44-audit F1 [P1] close)** -- a single Rread may legitimately short-return MID-FILE (the R-5 SA-F1 ground truth), so `0 < got <= lead` is NOT an EOF: the pre-fix `return 0` manufactured a false mid-file EOF that looping consumers (the REVENANT cluster fill -> zero-filled resident text pages, exec's eager segment read -> spurious exec failure, userspace streams -> truncation) treat as end-of-file; the fix returns 0 ONLY for `got == 0` (a Tread at wire_off returning nothing proves size <= wire_off <= offset) and otherwise RETRIES unshifted at the caller's offset, returning that verbatim (regression `dev9p.read_align_short_not_eof`, a one-shot mid-file short-return injection); the co_copy overlap direction (dst < src, forward-safe); non-cacheable clients byte-exact (netd stream offset semantics untouched); the lead pages populated under the SAME seq0 gen guard (an own-write racing the fetch skips the install). NO spec extension (the aligned read is wire-request shape, invisible to fs_cache.tla; the attr-EOF is a `Read` of the modeled attr cache). Regressions: `dev9p.read_align_heals_partial` (pass-2 wires only the EOF probe; fails pre-fix) + `dev9p.read_eof_attr_served` (fresh -> zero wire; absent/stale/post-write -> the probe wires); the loopback fixture gained a big-file pattern mode + msize-sized buffers (the 4096 buffers were under-msize, dormant until the first > 4 KiB reply). Measured (instrumented, N=3, clean sentinels, smp=4): warm gofmt 367 -> 249 ms med (the read band 157 -> 64 ms; wire reads 1568 -> 935; 64 MB -> 20 MB); S3 cold 5486 -> 4088 med; version-warm 18 -> 7 ms. The D44 focused audit additionally closed **F3 [P3, PRE-EXISTING]**: `dev9p_open` with OTRUNC truncates server-side but invalidated nothing -- the page/attr caches' soundness rested on Stratum bumping qid.version on truncate (an unverified cross-project guarantee); now the OTRUNC open drops the file's attr + pages like `dev9p_write` (own-write-through; regression `dev9p.open_trunc_invalidates`); and dispositioned **F2 [P3]**: the attr-served EOF, like the page serve, rests on the I-38 single-writer premise (an external writer's append is invisible until revalidation -- documented in 132-larder.md, within the stated premise). **F1 WRITE-BEHIND ADDENDUM (the loose writeback leg; Senate-voted 2026-07-11; LARDER-DESIGN.md section 12 is the design; the CHASE C-2 measured motivation: 97.9% of S3 Twrites <=4 KiB, the op-count band ~1.36 s):** on a LOOSE+cacheable client, `dev9p_write` STAGES pure-append runs per-open-file (`dev9p_priv.wb`: one contiguous {stage_off,stage_len,buf,cap<=DEV9P_WB_CAP=256KiB,base_size,err} under a per-priv spinlock; growth by doubling; staged buffers bounded by a GLOBAL DEV9P_WB_BUDGET [8 MiB, the DEV9P_CO_BUDGET shape -- a priv crosses Procs via dup/rfork/#926 close-at-exit, so a per-Proc I-32 charge has no sound uncharge site], over-budget -> write-through fallback; writes > DEV9P_WB_STAGE_MAX=32KiB flush-then-write-through [already wire-efficient; bounds the copy under the priv lock]; base_size KNOWN only for a create/OTRUNC-born priv [advances with completed flushes+write-throughs] -- an opened-existing file never stages) and FLUSHES as msize-max Twrites at close (BEFORE the async-clunk Tclunk -- the fid must be live) / fsync / cap / any non-append op on the priv (interior pwrite, wstat, weft-bind: flush-then-write-through -- fail-safe, zero new semantics off the append path). Same-priv reads OVERLAY the run (below stage_off = server/cache, complete by the append-anchor discipline; within = staged, short to the run end; at/past the run end = fall through to the normal path, which answers EOF honestly -- never a synthesized 0); fstat on the staging fd patches size = max(server, staged end); other-priv/path readers are close-to-open-legal until the close flush. Attr+page invalidates move per-write -> per-FLUSH. ERROR semantics (the voted NFS model, ABI-bounded): a flush failure LATCHES on the priv (subsequent writes/fsync return it; fsync is the reliable channel; `Dev.close` is void at v1.0 so close-flush is best-effort -- documented seam, v1.x grows the slot; the cap flushes bound the silent tail <256 KiB). STRICT mounts keep write-through byte-identical; non-cacheable/byte-mode/weft/cached-open privs untouched; no wire/server change. Prosecute: the LOST-STAGE class (every path out of a priv flushes or deliberately discards -- close/fsync/cap/non-append/death [#926 closes handles -> the close flush]); the OVERLAY completeness (a same-priv read can never see pre-staged bytes where the run covers -- the below/within/past split's boundary arithmetic); the VISIBLE-RUN flush SMP discipline (a flusher count freezes the run under the priv lock, wire I/O OUTSIDE it -- blocking 9P never under a spinlock; the frozen run pins the buffer [no growth realloc under a live flusher]; a concurrent APPEND goes write-through [order-free: disjoint, past the frozen end]; a concurrent read overlays the still-visible run; flushes are SINGLE-FLIGHT -- a second flush-needing party [fsync/non-append/wstat on a dup-shared fd] yield-waits at the flush entry until the in-flight flush retires, the on_cpu-spin wait class [bounded by the flusher's independent progress incl. its #811 death-unwind; no Rendez -> no park/wake surface, no single-waiter hazard, no new I-9 leg]; duplicate concurrent flushes are FORBIDDEN, not idempotent -- a completed duplicate lets an ordering-dependent through-write land while the FIRST flusher's stale residual chunks still fly, silently overwriting an interior write that overlaps the frozen run [the SA-F1 race]); the flush-before-async-clunk ordering (a Tclunk racing ahead of the flush Twrites writes to a dead fid); the global-budget charge/uncharge balance on every path (grow, fallback, error, close, death); the append-only gate completeness (every non-append reaches the flush-then-write-through arm -- a missed arm silently interleaves staged and wire bytes out of order); the fstat size patch (the Go buildid/truncate-gate reads it mid-open). **Spec-first (the L1b re-enabled surface): `fs_cache.tla` gains StageWrite + FlushClose on the content-token abstraction + 2 buggy cfgs (BUGGY_READ_SKIPS_STAGED = the overlay-miss class; BUGGY_LOST_STAGE = the lost-write class) -- model-first, BEFORE the impl; the byte-granular overlay split is beneath the abstraction -> kernel tests.** Tests: dev9p.wb_{coalesce_one_twrite,overlay_read,flush_at_close,fsync_flush_and_error,nonappend_writethrough,fstat_staged_size,cap_flush,budget_fallback} via the loopback fixture + the go-build E2E + the SMP gate. **G1 WRITE-POPULATE ADDENDUM (term-4; LARDER-DESIGN section 13):** the flush's per-flush page invalidate discarded the bytes the build reads back (the buildid ~100-byte in-place pwrite's write-through WHOLE-FILE invalidate nuked each freshly-populated archive). G1a: `wb_flush_locked`'s FULL-land arm (err==0 ONLY -- `fs_cache_buggy_populate_unflushed` pins the coupling) installs the frozen run's pages as OWN (`larder_page_install_own`; serves past the cvers gate + counts toward `pages_cover`; sound under the loose single-writer premise -- `EnableFlushPopulate` makes FlushClose -> Refetch, TLC-green; NO gen guard: the wb flush freeze excludes same-file mutators; page_off>0 EXTENDS only an own page ending exactly at the run start; a read-populate clears own). G1b: the write-through page invalidate is RANGE-SCOPED to [offset, offset+accepted) (`larder_page_invalidate_range`); OTRUNC + create-reuse + failed-flush keep whole-file. Prosecute: install coupled to the full-land arm; own-serve gated on loose+single-writer; the extend never mixes cvers/own or leaves a hole; the range invalidate never spares an IN-range page. Tests (revert-probed): dev9p.wb_populate_{readback,append_chain,failed_flush} + wb_writethrough_range. **G3+G4 ADDENDUM (term-4; LARDER-DESIGN section 14):** closes the T4-M-measured wga re-walk band (5.6x; am_mid=1104 mid-hop attr misses + 726-886 gen-guard-lost installs per S3 window) -- guest-only, no wire/server/spec change. **G3 perm-valid attr downgrade:** create/rename/unlink switch the PARENT-dir attr invalidate -> `larder_attr_downgrade` (`perm_only`): serves ONLY a `larder_walk_serve` INTERMEDIATE hop (the X-check reads mode/uid/gid + immutable qid fields -- untouchable by a child-mutation); `attr_serve`/`fresh_size`/the run's FINAL hop miss it (their consumers read the staled size/times/cvers); any full install upgrades; the downgrade LOGS an invalidation event (the resurrection close unchanged). wstat + the create-CHILD reused-ino defense keep FULL drops. **G4 qid-scoped gen guard:** every invalidation event logs its staled qid in the 128-slot `inval_qid` ring; the 3 install guards + the `pages_snapshot` B1-F1 gen witness skip IFF their OWN key appears in (seq0, gen] (dentry installs key on the PARENT; attr/page/snapshot on the file); window > ring -> fail-safe global skip. Soundness = the EVENT-LOGGING COMPLETENESS obligation: every mutation logs EVERY qid whose cached state it stales -- prosecute that property on any new mutation path. Tests (revert-probed, 3 probes -> 3 distinct failures): larder.attr_downgrade_perm_only + larder.downgrade_guards_raced_populate + larder.gen_scope_qid + dev9p.create_downgrades_parent_attr; the 3 pre-existing gen-guard tests re-pinned to the SAME-key contract. |
| FID-LIFECYCLE: async-clunk + cached-open (the fidless close-to-open open; I-38 / I-10 / I-11) | **async-clunk**: `kernel/9p_client.c` (`p9_client_clunk_async` -- fire-and-forget: `p9_session_send_clunk` + `mark_outstanding`, NO `client_run` block; the ownerless Rclunk drains via the #845 Tflush dispatch arm -> `clear_outstanding` frees the tag), `kernel/9p_session.c` (the ownerless-Rclunk dispatch), `kernel/dev9p.c::dev9p_close` (the normal-close clunk -> async). **cached-open**: `kernel/dev9p.c` (`dev9p_open_cached` -- the NULL-permitted `Dev.open_cached` slot stalk calls on the final run of a plain `STALK_OPEN`/`omode==0` resolution: an RPC-free hint [dentry chain + attr + page coverage] -> the FORCED-WIRE query `Twalkgetattr` [`newfid = P9_NOFID`, issued directly at the client, deliberately BYPASSING the L1d dentry serve -- the revalidation must be server-fresh or B2 silently degrades to B1] -> fresh-leaf checks [plain regular file; `DEV9P_CO_MAX_SIZE` 128 KiB cap] -> the global `DEV9P_CO_BUDGET` 8 MiB atomic charge -> **snapshot `[0,size)` into a private per-open buffer** under ONE Larder lock hold [atomic vs a concurrent invalidate] -> mint the opened fidless Spoor [`dev9p_priv.fid = P9_NOFID` + the snapshot + the open-time stat]; reads serve the snapshot; close frees + uncharges, wire-free; `SYS_WSTAT` on the fidless fd returns -1 [Tsetattr is fid-addressed and a fidless Spoor cannot late-bind; no v1.0 consumer fchmods an O_RDONLY dev9p fd -- the documented + tested seam; v1.x = the retain-the-walk-fid-unopened variant if a consumer appears]; every other wire op reachable from a fidless Spoor `P9_NOFID`-guarded), `kernel/stalk.c` (the cached-open arm in the pounce block: the Dev returns the Spoor + the FRESH per-component `sts`, and the RESOLVER runs the mandatory fail-ordering post-scan -- per-component X + mount scan [ANY mount hit, incl. the leaf, discards the cached open + falls back to the normal path's split/cross machinery] + the final-hop R/W perm_check on the fresh leaf record -- so the I-28/I-22 chokepoint stays in stalk, never fragmented into a Dev), `kernel/larder.c` (`larder_pages_cover` + `larder_pages_snapshot` -- PURE READERS; the Larder mutation surface stays byte-identical to its L1f-audited state). **Why snapshot-not-live-serve** (the 2026-07-10 scripture refinement): a fidless Spoor has NO read-time-miss recovery -- no "fid from qid" wire op exists, retained-path re-walks are rename-unsound -- so a mid-open eviction/invalidation under live `larder_page_serve` serving would force a false EOF at a non-EOF offset = silent data corruption; pinning pages in-cache instead couples into the L1f-audited eviction + own-write-invalidate paths (a pinned page surviving an own-write must go snapshot-only, or the writer's own read-after-write serves pre-write bytes at its matching open-time cvers). **prerequisite** (task #25, LANDED): the heap O(1) page cache cached-open snapshots from. **The metadata-cache re-size rides this arc** (the engagement fix): the Larder attr + dentry sub-caches go 256-inline-linear -> 4096-heap-hash-CLOCK (the task-#25 page pattern; `kernel/larder.c` `attr_*`/`dentry_*` index helpers; lazily allocated -> netd allocates neither; the coherence contract byte-identical) -- prosecuted with the L1 surface (hash link/unlink balance on install/evict/invalidate; the CLOCK victim's old-key unlink BEFORE re-key; the lazy-alloc OOM fail-safe). | FID-LIFECYCLE (`docs/FID-LIFECYCLE-DESIGN.md`; user-voted 2026-07-09; the measured 67%-of-the-warm-floor per-file fid overhead -- [[project-warm-floor-decomposition]]). **On the most-audited FS path (dev9p / 9p_client / 9p_session), and async-clunk touches the fid/tag layer (I-10 / I-11) directly -- prosecute hard.** NO new invariant. **async-clunk** composes **I-10** (the fire-and-forget Tclunk's tag stays reserved until its ownerless Rclunk drains, then freed -- the #845 discipline with no submitter to wake; a never-drained clunk permanently burns 1 of 64 `outstanding[]` slots -> tag-pool exhaustion is THE hazard) + **I-11** (the fid unbinds at SEND [`p9_session_send_clunk`:375, already reply-independent]; the monotonic fid NUMBER is never reused, so a clunk-pending fid is never re-referenced). Prosecute: the tag drains promptly (the reader dispatches Rclunk continuously; the pool never exhausts under the serial close/open pattern); the session-death path (clunk-pending tags abandoned with the session, no leak -- client-side already unbound); `any_outstanding_on_fid` still refuses a clunk while a live op targets the fid; the error-path clunks' sync-vs-async choice. **cached-open** refines **I-38** (the fidless open is close-to-open: the query-`Twalkgetattr` IS the `Open` revalidation; the snapshot is taken at the fresh `cvers` atomically under the Larder lock -- a concurrent own-write invalidate either precedes it, failing the coverage, or follows the whole copy; every read of the open serves the open-time content, exactly close-to-open; the fallback is byte-identical). Prosecute: the fidless-Spoor lifetime (no server fid to leak; close wire-free; the snapshot buffer + the global budget balance on EVERY path -- mint, resolver-denial-destroy, close, handle-transfer/rfork-inheritance [the budget is deliberately GLOBAL, not per-Proc: the fd crosses Proc boundaries, a per-Proc charge would unbalance at close-by-inheritor]; `attached_owner`/priv teardown correct with `fid == P9_NOFID`); the "fully page-cached" detection (a partial-cache open MUST fall back, never serve a hole -- incl. the msize-clamped mid-file partial page whose `valid_len` is short of the boundary); the `cvers` revalidation completeness (on a STRICT client every cached-open engagement is gated on the FRESH wire cvers -- the forced-wire query must bypass the dentry serve, else it silently becomes B1/loose and violates I-38; on a LOOSE client [the B1 per-attach opt-in, user-voted 2026-07-11 -- see the I-38 row] a FULL hint hit legitimately skips the wire query and snapshots at the CACHED cvers, so the prosecution item becomes the LOOSE-GATE EXACTNESS: the skip engages ONLY when `client->loose` is set AND the hint fully hit [chain + attr + coverage]; a strict client's path is byte-unchanged; a loose client's hint MISS still takes the forced-wire query; the flag is set ONCE at attach before the root publishes [no runtime flip -- a plain field ordered by the handle publication] and ONLY from the validated `SYS_ATTACH_9P_LOOSE` bit); the read-only gate (`omode == 0` exactly -- write / OTRUNC / OEXEC / O_PATH never take the fidless path); the resolver post-scan completeness (per-component X + mount scan + leaf R/W on the FRESH records -- the POUNCE fail-ordering invariant, I-28; a denial destroys the minted Spoor and fails with the identical error the normal path produces); the `P9_NOFID` guards on every wire op reachable from a fidless Spoor (walk / walk_attrs / create / readdir / rename / unlink / re-open); the `SYS_WSTAT` fidless seam (loud -1, tested); the fallback byte-identity; the SMP-safety (the snapshot is immutable post-mint; the Larder helpers are pure readers under the leaf lock). **Spec-first**: async-clunk extends `specs/9p_client.tla` (clean + `async_clunk_tag_leak` buggy cfg); cached-open maps onto the EXISTING `specs/fs_cache.tla` `Open`+`Read`+`OwnWrite` (no new action -- the query-getattr = `Open`, the page serve = `Read`), a prose SPEC-TO-CODE mapping (a focused `cached_open` cfg only if the audit surfaces subtlety). **No new §28 invariant** -- prose validation in `docs/FID-LIFECYCLE-DESIGN.md` + the I-38 row + this row + a focused audit per sub-chunk + the `9p_client.async_clunk_*` / `dev9p.cached_open_*` tests + the DIAG23 re-measure (clunk RTs off the critical path; open/read RTs eliminated for cached files) + boot OK + the SMP gate + the gofmt cold/warm re-measure (the arc payoff, ~4x). Sub-chunks: async-clunk (I-10/I-11, the quick win) -> page-cache sizing (task #25, the prerequisite) -> cached-open (refines I-38). **G2 DIR-FID CACHE ADDENDUM (term-4, 2026-07-13; FID-LIFECYCLE-DESIGN section 8):** a 64-entry `p9_dirfid_cache` on `p9_client` parks walk-fresh (never-opened) DIRECTORY fids by qid.path; dev9p owns all policy. CONSUME (`dev9p_walk_attrs` bind form): a fully-Larder-served chain whose dir leaf has a parked fid mints the walked Spoor from it -- ZERO wire ops (the take REMOVES the entry: one Spoor per live fid, I-11); a perm_only leaf serves THIS path only (the `leaf_perm_only` out-param -- the bind consumer reads mode/uid/gid + immutable qid fields, all fresh under a G3 downgrade). DONATE (`dev9p_close`): an unopened dir fid on a cacheable client parks instead of clunking (dedup/evict victims clunked outside the leaf lock) -- the by-name flow recycles one fid indefinitely. THE STALE-FID DEFENSE (three layers -- a fid tracks an INODE; prosecute all three on any change): (1) drop hooks -- create drops the returned qid's entry (ino reuse); unlink + rename-REPLACE resolve the victim qid from the (parent,name) dentry BEFORE the wire op (`larder_dentry_lookup`), drop+clunk the parked fid, AND invalidate the victim's own attr (the HARD event arming layer 2; strictly safer than the L1f-F3 leave-it posture); (2) the donate staleness gate -- `dev9p_priv.fid_gen` (the Larder gen at mint/take) + `larder_qid_staled_since` (HARD-only ring scan; the G4 ring is kind-tagged: attr_invalidate=HARD [identity death], downgrade/dentry/page=SOFT -- a by-name op downgrades its own parent on every use, so a soft-inclusive gate would block exactly the recycle; the install guard still scans ALL); (3) the suspect backstop -- a by-name op ERRORING through a priv latches `fid_suspect`, the close clunks, never re-parks (breaks the poison loop for the evicted-dentry residual whose staleness event is unknowable; cost = one honest server error, the same close-to-open race outcome as pre-G2). RT accounting: consuming ops (Tlcreate transitions the fid; Tlopen mode-binds) keep their 1 fused RT -- the 0-RT wins are the NON-consuming by-name ops + repeat parent resolutions. Tests (revert-probed, 2 probe builds -> 5 distinct targeted failures): dev9p.dirfid_{consume_and_recycle,perm_only_leaf_consume,create_reuse_drop,rmdir_drop_and_no_stale_repark,suspect_not_reparked}. 1120/1120 + boot OK. |
| Kernel rwx enforcement layer (A-2d) | `kernel/syscall.c` (`perm_check` insertions in `sys_walk_open_handler` [X on src + R/W on target], `sys_walk_create_handler` [W+X on parent], `sys_wstat_handler` [the chmod/chown/chgrp policy]), new `kernel/perm.c` `perm_check` + `proc_in_group`, `kernel/dev9p.c` (`dev9p_stat_native` gated on the `Rgetattr` valid mask -- closes A-2a F2) | Convergence-detour A-2d (IDENTITY-DESIGN.md §3.7.1 + §9.6; privilege model voted 2026-05-30). The first real exercise of **I-22**'s enforcement obligation -- the Linux-VFS model (kernel enforces rwx at the FS chokepoint; Stratum enforces dataset-scope ONLY, not file rwx, §3.7). Owner-first POSIX algorithm; group membership = `primary_gid` OR `supp_gids[0..count)`; enforcement at walk (X on the searched dir, per-component since walk is one-name-per-call), open (R/W per omode; `O_PATH` exempt from R/W but not from the X-search), create (W+X on parent), wstat (the policy); read/write not re-checked (open-time snapshot); the handle RIGHT (capability axis) AND the rwx check (identity axis) both required. **`CAP_HOSTOWNER` is the unified v1.0 fs-admin authority** (DAC-override + chmod/chown/chgrp-any) -- a capability (elevation-only, console-gated, never rfork-able), never an identity, so no `principal_id` -- not even `PRINCIPAL_SYSTEM` -- bypasses (**I-22 preserved**); owner keeps owner|self-group chmod + chgrp-to-own-group; no-give-away `chown` (`CAP_HOSTOWNER` only). Fail-closed on a NULL `stat_native` Dev. Honest scope: per-principal-real on devramfs (system-owned world-r/x -> boot chain owns everything, no brick; a `CAP_SET_IDENTITY`-spawned non-system child is denied write -- testable now, not gated on login A-5). **dev9p enforcement DEFERRED to A-3** (user-signed-off 2026-05-30): uniform dev9p enforcement bricks the boot (host-bake stamps pool entries host-uid-owned 0644/0755; the `PRINCIPAL_SYSTEM` boot chain as *other* cannot write the pool -> post-pivot creates denied). Gated by a new `Dev.perm_enforced` flag (devramfs=true, dev9p=false; A-3 = one-line flip); dev9p stays handle-RIGHT-gated only at v1.0. The wstat policy is also `perm_enforced`-gated (dormant + unit-tested at v1.0). A-2b create-check folds in; A-2c mount-cape stays a seam; A-4 splits a finer `fs-admin` clearance (`CAP_DAC_OVERRIDE`+`CAP_CHOWN`) additively. **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §3.7.1 + §9.6 + this row + the audit + the runtime tests. |
| O_PATH byte-I/O block (`CWALKONLY`, #81) | `kernel/include/thylacine/spoor.h` (the new `CWALKONLY` flag in the Spoor `flag` field, alongside `COPEN`/`CMSG`/`CSRVCLIENT`), `kernel/syscall.c` (set `CWALKONLY` on the installed Spoor at the TWO `T_OPATH` handle-creation sites -- `sys_walk_open_handler` ~1641 + `sys_open_handler` ~1814; REJECT it (-1) in `sys_read_for_proc` + `sys_write_for_proc` + `sys_readdir_handler` BEFORE `dev->read`/`write`/`readdir`; the F5 comment updated to note the enforcement), `usr/lib/libthyla-rs` + `usr/lib/libt` (no ABI change -- `T_OPATH` unchanged; the behavior change is read/write/readdir on an O_PATH fd now returns -1) | #81 (the #57b self-audit SA-2; user-voted minimal scope 2026-06-12). **Closes a perm_check bypass for reads -- privilege boundary; prosecute hard.** The §9.4 design says a `T_OPATH` handle "is NOT opened for byte I/O", but the impl never enforced it: `sys_read`/`write`/`readdir` gate only on the handle RIGHT (not COPEN/O_PATH), an O_PATH handle is born `R\|W`, and `T_OPATH` ALSO skips the A-2d `perm_check` -- so a non-owner who can X-search to a perm-restricted file could `SYS_WALK_OPEN(T_OPATH)` it (skipping perm_check) then `SYS_READ` it and read content the normal path denies (the live instance: the 0400 `/system.key` pool master key via the `/bin`=devramfs-root bind, readable by any logged-in Proc). **Invariants:** strengthens **I-22** (no ambient authority -- closes the read-bypass of the A-2d perm enforcement) + the §9.4 navigation-only intent; **I-23** preserved (the O_PATH handle stays born `R\|W` -- a valid A-1.7 storage-capability create/walk target; only byte I/O is blocked). Prosecute: the gate is COMPLETE (every content-I/O path -- read, write, readdir -- rejects `CWALKONLY`, the SA-1 lesson "cover every I/O site"); SAFE (the 5 O_PATH consumers -- chroot/walk_create/mount/pivot_root/the joey re-graft -- use the handle as a navigation base via `sys_lookup_spoor`, never read/write/readdir it, so none regress); the flag is set at BOTH creation sites (no O_PATH path escapes); a COPEN-required check was rejected (breaks `SYS_CONSOLE_OPEN`'s legitimate non-COPEN-readable handle), rights-removal rejected (A-1.7 needs O_PATH born `R\|W`); `fstat` stays allowed (Linux O_PATH allows it; navigation needs it), `lseek`/`wstat` unchanged (wstat re-runs perm_check -- not a bypass; full Linux-O_PATH alignment is a v1.x completeness item). **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.4 (the #81 addendum) + this row + the focused audit + the runtime tests (a non-owner O_PATH-read of a 0400 file -> denied; O_PATH chroot/create/mount still work; the `/system.key` leak closed) + boot OK + the SMP gate. |
| A-3: 9P identity presentation + dev9p enforcement activation | `usr/lib/pouch/patches/0006-pouch-sockets.patch` (SO_PEERCRED shim: `ucred.uid = info.principal_id`, `ucred.gid = info.primary_gid` -- was `0`/`0`), `kernel/dev9p.c` (`.perm_enforced = true` flip), `kernel/syscall.c` (`sys_walk_open_handler` F1: derive handle rights from omode; `sys_rename_handler`+`sys_unlink_handler` F2: `perm_check(parent, W\|X)`; `sys_attach_9p_handler`+`sys_attach_9p_srv_handler` M4: substitute `principal_id` for `n_uname`), `tools/build.sh::build_stratum_pool_fixture` (`--bake-owner-uid PRINCIPAL_SYSTEM`), Stratum `thylacine-pouch-arm` `src/cmd/stratumd/{run.c,serve.c}` (`--bake-owner-uid`/`--bake-owner-gid` override of `s->auth_uid`/`auth_gid`) | Convergence-detour A-3 (IDENTITY-DESIGN.md §9.7 + the §3.5 F-4 correction + §3.7.1 activation note; two user votes 2026-05-31: SO_PEERCRED channel + flip-now). **Activates dev9p rwx enforcement -- privilege boundary, AEGIS-adjacent write path; prosecute hard.** Corrects F-4: the load-bearing trusted-local identity channel is **`SO_PEERCRED`** (kernel-stamped via `SYS_srv_peer`, *unforgeable*), not `n_uname` (which Stratum ignores). Reconciliation (no-brick): host-bake stamps the pool `PRINCIPAL_SYSTEM`-owned (Stratum `--bake-owner-uid`; NOT an on-disk-format change -- `si_uid`/`si_gid` exist), pouch SO_PEERCRED carries `principal_id`, so the kernel-side `perm_check` is coherent and the boot chain (owner) is not denied. **Invariants:** I-22 preserved (the principal is kernel-stamped, not client-asserted -- no identity self-elevates; `CAP_HOSTOWNER` remains the only DAC-override); I-2/I-4/I-6 unaffected (no cap/transfer added; F1 *narrows* the handle envelope to omode-derived rights); A-1.7/I-23 preserved (F1's `T_OPATH` carve-out keeps the storage-capability base born `R\|W`, no TRANSFER); no-brick (boot OK + cross-reboot PASS are the gate). Closes A-2d F1 (handle-rights-from-omode) + F2 (perm_check on rename [both dirs] + unlink). `n_uname` forwarding kept but demoted to the v1.x foreign/authenticated path; the corvus trust-stamp gate is a v1.x SEAM (every v1.0 attach is local SO_PEERCRED-bearing -> no untrusted-assertion to gate). Per-user stratumd `--role client` (Stratum A2, verified merged) mechanism proven via a dataset-scope `EACCES`-at-Tattach probe; the per-login spawn is the A-5 consumer. **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.7 + this row + the audit + the runtime tests. |
| Group termination / cross-thread shootdown (`SYS_EXIT_GROUP`) | `kernel/proc.c` (`proc_group_terminate` + the single set-once `group_exit_msg` on `struct Proc`; the group-status variant of the last-Thread-out ZOMBIE reap; `exits`/`thread_exit_self` group-status path), `arch/arm64/exception.c` (`el0_return_die_check` at the sync-from-EL0 tail), `arch/arm64/vectors.S` (the **new IRQ-from-EL0 return-tail die-check** -- `#713`-safe), `kernel/torpor.c` (`torpor_wake_all_for_proc`), `kernel/smp.c` (`smp_resched_others` -- broadcast `IPI_RESCHED`), `kernel/syscall.c` (`SYS_EXIT_GROUP = 60` handler + the `kill`-cascade replacing the multi-thread refusal in `sys_postnote`), `kernel/include/thylacine/syscall.h` (`SYS_EXIT_GROUP = 60` ABI), `usr/lib/pouch/patches/0001-pouch-syscall-seam.patch` (`__NR_exit_group` 0 → 60), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers | SYS_EXIT_GROUP (#809, `89456e9`; pulls the documented v1.x lift forward -- #808 audit F3). **Invariant I-24** (group termination atomic + exactly-once + no-lost-wakeup). The privilege/lifetime boundary AND a wait/wake surface -- prosecute hard. As-built model = flag-and-self-terminate at the EL0-return checkpoint via a **single per-Proc set-once `group_exit_msg`** (NULL-sentinel CAS = die-flag + last-out status) + `torpor_wake_all_for_proc` + broadcast `smp_resched_others` (NOT the abandoned `die_requested`/per-Thread-`cpu`/`group_exiting`/`group_exit_status`/targeted-IPI design — F2 reconcile). Plan 9 / Linux / Zircon convergent (seL4 sync-stall rejected -- see §7.9.1). Invariants: **I-9** (the sleeper-wake reuses register-then-observe under the per-condition lock — `torpor_lock` for `torpor`, per-Thread `wait_lock` for all other rendez sleeps (§8.8.1, task #811); lock order `g_proc_table_lock → wait_lock → r->lock`); **#713** (the IRQ-from-EL0 die-check sits before the DAIF-masked eret window; the die path is noreturn); **#788** (`on_cpu`-spin before any peer `thread_free`); **I-8** (every flagged Thread eventually reaches its checkpoint -- broadcast IPI, timer-tick floor, wake for sleepers); status ok/fail collapse. Closes the `exits`-with-live-peers extinction (the `tools/test.sh` flake) + `kill → -EIO in multi-thread Proc` (13b R1-F9). **v1.0 residual → RESOLVED by task #811** (§7.9.1 [OPEN Q 7.9.A] = B, universal, user-voted 2026-05-31): the #809 audit (F1) showed the residual is a non-reaping HANG -- an indefinite `poll(-1)` / `pipe` / `devnotes_read` sleeper is un-woken; §8.8.1 makes the cascade wake total. The multi-thread **fault** path (`proc_fault_terminate`) is a tracked follow-up (#810), not this chunk. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in §7.9.1 + this row + the audit + the runtime tests. |
| Universal death-interruptible sleep (`*_INTR`) | `kernel/sched.c` (`sleep`/`tsleep` generalized register-then-observe of `group_exit_msg` + the `SLEEP_INTR`/`TSLEEP_INTR` return), `kernel/include/thylacine/thread.h` (per-Thread `wait_lock` protecting `rendez_blocked_on`; only the owner mutates it), `kernel/include/thylacine/rendez.h` (the `*_INTR` sentinel + contract), `kernel/proc.c` (`proc_group_terminate` walks `p->threads` + wakes each sleeper via `wait_lock`→`rendez_blocked_on`; `exits()` folds into the same universal wake), and **every** blocking site's "on `*_INTR` → cleanup → return" arm: `kernel/poll.c`, `kernel/pipe.c` (read + write), `kernel/devnotes.c`, `kernel/srvconn.c` (client + server recv), `kernel/devsrv.c` (accept), `kernel/irqfwd.c`, `kernel/proc.c` (`wait_pid`) | Task #811 -- the F1=B completion of `SYS_EXIT_GROUP` (#809-audit F1); §8.8.1. **The wait/wake primitive itself -- prosecute hard.** Invariant: **I-9** generalized (no death-wake lost between a sleeper's cond-check and its sleep, for **every** rendez sleep -- register-then-observe under the per-Thread `wait_lock`, the Plan 9 `p->rlock` analog); completes **I-24**'s "no Thread runs at EL0 after ZOMBIE" for the indefinite-sleeper class. Lock order: `wait_lock` is the **outermost** wait-lock (`wait_lock → g_timerwait.lock → r->lock`; waker `g_proc_table_lock → wait_lock → wakeup`); acyclic because only the owning Thread writes `rendez_blocked_on` and no sleeper holds `g_proc_table_lock` below `wait_lock` (`wait_pid` drops it first). Death unwinds at the EL0-return tail (`el0_return_die_check`), never inside `sleep()` (would strand caller locks). Re-validate each site's cleanup + I-9 in the audit (dirty-class follow-up per the #809 close). Closes #809-audit F4 (`exits()` fold-in). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in §8.8.1 + this row + the audit + per-site regression tests. |
| A-4 capability model + legate elevation (`rfork` I-2 strip; `cap` device clearance grant/redeem) | `kernel/include/thylacine/caps.h` (`CAP_GRANT_CLEARANCE`=1<<6 fork-grantable; `CAP_DAC_OVERRIDE`=1<<7 / `CAP_CHOWN`=1<<8 / `CAP_KILL`=1<<9 elevation-only; `CAP_ELEVATION_ONLY` expands to all four elevation-only bits), `kernel/proc.c` (`rfork_internal` ANDs `~CAP_ELEVATION_ONLY` -- the A-4-pre I-2 fix; `legate_scope_id`/`legate_session_id`/`legate_valid_until` on `struct Proc`; scope-teardown via `proc_group_terminate`), `kernel/devcap.c` (the `grant`/`use` files generalized from CAP_HOSTOWNER-only to arbitrary clearance cap-sets + `valid_until` -- the `grant` file is LENGTH-discriminated [16 = hostowner, 32 = clearance], the `use` file does ONE locked kind-branched redeem; the redeem rides the EXISTING `/cap/use` file -- NO new REDEEM syscall -- and CREATES a legate via `kernel/proc.c::proc_become_legate`; the clearance GRANT rides the new `SYS_CAP_GRANT_CLEARANCE` = 61 syscall [the grant-side bridge mirroring `SYS_CAP_GRANT`: corvus is chrooted to its storage cap and reaches the cap device by syscall, NOT a `/cap` file walk, exactly as the hostowner grant already does -- the 32-byte `/cap/grant` Dev write stays the conceptual path for un-chrooted writers + tests]), `kernel/perm.c` (honor `CAP_DAC_OVERRIDE` in `perm_check` + `CAP_CHOWN` in `perm_wstat_check`); A-4a-3 lands `SYS_CAP_GRANT_CLEARANCE` + the libthyla-rs `cap.rs` clearance grant/redeem wrappers + the corvus CLEARANCE verbs (14-17) + the E2E legate prover | A-4-pre + A-4a. **Invariants I-2 (the elevation-only strip -- the named P5-hostowner hole), I-25 (legate scope bounded + fully revoked), I-22 (no identity carries ambient authority).** Highest-stakes privilege surface -- prosecute hard. Prosecute: no elevation-only leak across `rfork`/`SYS_SPAWN_WITH_CAPS`; the `cap`-device grant lifecycle (no replay, no cross-stripes redeem, `valid_until` honored, `self_restriction` only reduces); scope teardown exactly-once + reuses #809/#811 correctly (no orphaned elevated Proc). **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| A-4b cross-process kill (`/proc/<pid>/ctl` + `CAP_KILL`) | `kernel/devproc.c` (the `ctl` write parses `kill`/`killgrp` -> `proc_group_terminate` uniformly under `g_proc_table_lock` via the `proc_for_each` resolve+authorize idiom -- the `#811` wake-total primitive; new `stat_native` reporting the target's `principal_id`/`primary_gid`/`0600`; **`perm_enforced = false`** -- the two-axis check (owner [same `principal_id` on the `0600` ctl] OR `CAP_HOSTOWNER` OR `CAP_KILL`; computed DIRECTLY, NOT via the `perm_check` DAC-override -> `CAP_DAC_OVERRIDE` is NOT a kill axis, keeping fs-admin orthogonal to kill) runs at the WRITE site in `devproc_write`, NOT at open: the SHARED open chokepoint hard-rejects pre-`devproc.open` so the gate-at-open sketch could not host the `CAP_KILL` axis -- reconciled 2026-06-01, user vote). USER-REACHABILITY of `/proc` is a Utopia namespace seam (needs the `namec` multi-component mount-crossing resolver + a boot-path mount); A-4b lands + kernel-unit-tests the mechanism + authority (the privilege logic is fully in-kernel testable). | A-4b. **Invariant I-26 (cross-process control two-axis; composes I-22 + I-1).** Privilege boundary -- prosecute hard. Prosecute: no identity bypass of the kill gate; namespace-visibility containment; the parent-of-target case still works; no UAF resolving `<pid>` -> Proc under the lock (resolve+authorize+kill all under `g_proc_table_lock`); multi-thread cascade correctness. **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| A-4c trusted path: kernel console RX + SAK | `arch/arm64/uart.c` (RX IRQ + IMSC.RXIM unmask + RX-FIFO drain + `DR.BE` BREAK detect; PL011 SPI 33 hardcoded as the QEMU-virt fallback -- DTB `interrupts`-parsing is a Lazarus seam), `kernel/main.c` (boot wiring: `gic_attach`+`gic_enable_irq` for the UART SPI, alongside the timer), `kernel/irqfwd.c` (reserve the UART SPI INTID, like the timer, so userspace `SYS_IRQ_CREATE` cannot claim it), `kernel/cons.c` (`devcons_read` real blocking read on a `Rendez` + single-reader busy-guard + a console input ring; the IRQ handler is wakeup-only -> the `console_mgr` kproc kthread does the deferred privileged work in process context [SAK = serial BREAK recognized pre-EL0; Ctrl-C -> `interrupt` note]), `kernel/proc.c` (`proc_revoke_console_attached` [atomic `proc_flags` clear] + the single `g_console_owner` pointer under `g_proc_table_lock` + `exits()` clear-on-owner-exit + re-grant to corvus via `g_console_trusted_proc`, FAIL-SAFE revoke-only if absent + notify), `kernel/devcap.c` (the redeem gate keys on `PROC_FLAG_CONSOLE_ATTACHED`) | A-4c-1 (console RX pull-forward, Phase-4-G work) + A-4c-2 (SAK + handoff). **Invariant I-27 (trusted path: unspoofable elevation prompt).** New EL0-bound input path + a privilege boundary -- prosecute hard. Prosecute: the RX ring (no overflow, no missed-wakeup on the `Rendez`); Ctrl-C delivery; the SAK recognizer cannot be starved/spoofed by crafted input (structural -- BREAK is a line condition, not data); the console-attach revoke/re-grant is atomic + leaves exactly one owner; only the console-attach holder can redeem elevation. On the kernel UART console Dev (`dc='c'`) -- the userspace VirtIO-input path is unaffected (ARCH §17.1). **Test note**: the harness cannot inject UART RX non-interactively (`-serial mon:stdio` + `< /dev/null`, one PL011, no QMP serial channel) without touching the boot-banner test ABI -> proven by in-kernel unit tests (synthetic RX-handler/recognizer/owner-transition drive) + boot survival + the interactive `Ctrl-A b` BREAK path. **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.8 + this row + the audit + tests. |
| `/dev` namespace front-door + the I-27 gate-at-namespace-open (#57b) | `kernel/devdev.c` (NEW: the aggregating directory Dev, dc='d', name="dev"; `.attach` -> a QTDIR root; the dual-mode reuse-`nc` `.walk` dispatching the leaves `null`/`zero`/`full`/`random`/`urandom`/`cons`/`consctl`; **`.open` -- the I-27 gate**: the `cons`/`consctl` qids require `proc_is_console_attached(current_thread()->proc)` else NULL [-> walk-open returns -1]; the trivial leaves pass through; `.read`/`.write` qid-dispatched, `cons` delegating to the exported `cons.c` API), `kernel/cons.c` (new public `cons_input_read`/`cons_output_write` shared by `devcons` [the syscall front-door] and `devdev` [the namespace front-door] -- ONE console implementation, ONE single-reader ring), `kernel/devramfs.c` (the `dev` synthetic mount-point dir in `g_ramfs_synth_dirs[]`, 0555 SYSTEM-owned), `kernel/joey.c` (`joey_mount_static_dev(kt, &devdev, "dev", 3)` in the kproc boot namespace), `usr/joey/joey.c` (the pre-pivot `t_open("/dev", O_PATH)` + post-pivot `mkdir` + `t_mount(MREPL)` re-graft, mirroring `/proc`+`/ctl`+`/srv`+`/bin`), `kernel/dev.c` (register `devdev` in the bestiary) | #57b (the container-keystone namespace layout; user-voted "Full /dev incl cons" 2026-06-12; scripture-first). **Invariant I-27 (trusted path) -- the namespace `open` of `/dev/cons` must NOT bypass the console-attach gate; prosecute hard.** A-4c gated only `SYS_CONSOLE_OPEN`; #57b adds the walkable `/dev/cons` path, so the gate MUST also cover the Dev-`open` site, or an ungated `open("/dev/cons")` lets any EL0 Proc become the single console reader and steal the getty's input (the A-5a-F2 break -- a login passphrase to a thief). Prosecute: the gate is enforced at `devdev.open` for BOTH `cons` and `consctl` (at OPEN -- covering all subsequent read AND write, so a non-attached Proc cannot even spoof console OUTPUT); the trivial leaves are correctly UNGATED (null/zero/full/random are world-rw on every Unix; no secret, no privilege); the **reuse-`nc` walk contract** (the #57a lesson -- a mounted Dev's `.walk` must return the caller's pre-clone as `wq->spoor`, 0-element -> `nqid==0`, or `clone_walk_zero` cannot cross the `/dev` mount); the **single-console-implementation sharing** (the `cons.c` ring / single-reader busy-guard / INTERACTIVE-promotion / death-interruptible read is REUSED, not forked -- no second reader path that races the first); no info-leak / OOB in the trivial leaves (random uses `kern_random_bytes`; zero/full bounded memsets); the gate is orthogonal to the 0666 leaf perms (`devdev.perm_enforced == false`, like devproc/devctl); the mount widens *visibility*, never *authority*. `consctl` is present + gated but v1.0-modeless (read EOF / write -1; termios is LS-8 #952). **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.8 + ARCH §9.4 + the §28 I-27 row + this row + `docs/reference/` (the devdev section) + the focused audit + the runtime tests (the gate deny/allow via console-attach; the trivial leaves; walk reuse-`nc`; mount+cross) + boot OK + the SMP gate. |
| Interactive Ctrl-C: `interrupt` as a real note (LS-5) | `kernel/include/thylacine/syscall.h` + `kernel/syscall.c` (the new `SPAWN_PERM_CONSOLE_OWNER` spawn-perm bit + its grant gate, like `SPAWN_PERM_MAY_POST_SERVICE`; sets `g_console_owner = child` at spawn), `kernel/proc.c` (the owner-set-at-spawn path; the joey relinquish unchanged), `usr/login/src/main.rs` (login spawns `ut` WITH the new perm), `kernel/notes.c` (`notes_deliver_at_el0_return` — the P2 `interrupt` default-terminate with the self-managing-fd gate) + `kernel/devnotes.c` / `kernel/include/thylacine/notes.h` (the "notes-fd-open => self-managing" mark), `kernel/sched.c` + `kernel/proc.c` + `kernel/torpor.c` + every #811 blocking site (the P3-terminate wake-predicate widening: death OR a pending terminate-disposition `interrupt`), `usr/utopia/libutopia/src/eval/stmt.rs` (`wait_pids_interruptible` already forwards `interrupt` — unchanged), `tools/interactive/ls-5.exp` | LS-5 (the Life Support arc; `docs/LIFE-SUPPORT.md`). **Makes `interrupt` a real catchable Plan 9 note / Unix SIGINT** — P1 delivery (console-owner) + P2 default-terminate-if-uncaught (the self-managing gate) + P3-terminate (the #811-reusing blocked-child wake). **Death-path + notes surface (the #788/#806/#807/#808/#860/#809/#811/#926 lineage — the most bug-prone in the tree) AND a privilege-adjacent ABI addition (a new SPAWN_PERM bit) -- prosecute hard.** Invariants: **I-19** (the `interrupt` per-note default disposition; ordering / exactly-once / N-4 untouched), **I-9 / §8.8.2** (the death-wake generalizes to death-or-terminate-`interrupt`, reusing the per-Thread `wait_lock` register-then-observe — re-validate every #811 site), **I-27** (console-*owner* is strictly distinct from console-*attach*; the owner bit never confers attach, so the SAK / elevation gate is untouched — the per-bit separation is the load-bearing check), **I-2** (the new perm is a spawn-time `perm_flags` decision, never `rfork`-propagated). Prosecute: the owner-grant gate (only a console-owner/trusted spawner may confer it; no ordinary Proc acquires it); the self-managing gate (the shell exempt, a dumb child not — and a child cannot register a handler while blocked to escape the terminate); the P3-terminate wake (no lost wake, no double-terminate, the single-thread disposition-fixed-at-post reasoning, lock order unchanged from #811); the LS-5/LS-8 boundary (P3-deliver / `-EINTR` is NOT built here). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in §8.8.2 + `docs/LIFE-SUPPORT.md` (LS-5) + this row + the focused audit + the #811 per-site re-validation + the `ls-5` LS-CI scenario + per-site regression tests. |
| A-5 login + session lifecycle + per-user encrypted home | NEW `usr/login/` (native `/sbin/login`, libthyla-rs: SAK-gated `/dev/cons` prompt -> corvus `AUTH` client -> `CAP_SET_IDENTITY` stamp via `SYS_SPAWN_FULL_ARGV`'s `SPAWN_IDENTITY_SET` -> per-user `--role client` stratumd + `/home/<user>` bind -> spawn `ut` as the session leader; logout = group-terminate + unmount + corvus `SESSION_CLOSE`), `usr/joey/joey.c` (spawn `/sbin/login` post-pivot + relinquish its boot console-attach at the bringup->session boundary), `kernel/proc.c` (the joey console-attach relinquish + the OPTIONAL `SPAWN_PERM_CONSOLE_OWNER` -> `g_console_owner`-without-attach, for Ctrl-C-to-shell), `kernel/include/thylacine/syscall.h` + `kernel/syscall.c` (`SPAWN_PERM_CONSOLE_OWNER` if added); **Stratum-side** (`thylacine-pouch-arm`) `src/sync/sync.c` (deferred-unwrap soft-skip of out-of-scope CURRENT slots, `sync_unwrap_cb`) + a runtime DEK install/evict consumer (reusing `stm_corvus_unwrap`) + the login token-forward | Convergence-detour A-5 (IDENTITY-DESIGN.md §9.9; 3 votes + a refining 4th, 2026-06-02). The capstone integration -- composes I-1 / I-22 / I-27 + the A-4 caps; adds NO new ARCH §28 invariant. **The DEK handoff is AEGIS/mallocng-adjacent -- prosecute hard.** Prosecute: **I-27** (login + the user shell are NEVER console-attached; the joey relinquish preserves "corvus is the sole console-attached Proc during a session"; no interposer between the SAK and the corvus prompt); the **identity stamp** (`CAP_SET_IDENTITY` gate; login stamps only the principal corvus authenticated; no forge); the **DEK handoff** (login never holds the raw DEK; the token-forward leaks no secret via argv/files; the coordinator install/evict has no UAF/leak; eviction actually zeroes); **user-vs-user isolation** (a 2nd user's session cannot unwrap or attach the first's dataset -- dataset-scope EACCES + the per-user-sealed DEK); the **session teardown** (no orphaned session Proc; the kill cascade is total per #811); the **Stratum deferred-unwrap** (a soft-skipped dataset is provably unreadable until its DEK is installed; the install validates the forwarded token). **A-5b DEK transport (RESOLVED 2026-06-02, user-voted; corrects the same-day "no corvus lift" note):** the coordinator PULLS the DEK with the login-forwarded token over its own corvus connection (§6.3), enabled by (a) the pouch `connect()` walking to corvus's `ctl` fid (`usr/lib/pouch/patches/0006-pouch-sockets.patch`, audit-bearing boundary-line; the kernel 2-arg `SYS_srv_connect` already drives the walk) and (b) the **corvus session-ownership lift** (`usr/corvus/src/main.rs` -- the AUTH-SESSION owning-connection tag + the `close_conn` clear-gate: clear the global AUTH session only on the OWNING connection's close or explicit SESSION_CLOSE, never on a non-owning bearer-token connection's close -- else the coordinator's transient connection wipes login's live session mid-session and breaks A-4 legate elevation). PROSECUTE both new surfaces: the corvus session-ownership change (no cross-session wipe; owning-connection tag unforgeable; the §4.2/§6.2 intent realized) + the pouch ctl-walk transport. corvus-PUSH rejected (role inversion; corvus lacks the storage layout). The security property (at-rest + session-scoped, login-never-holds-raw-DEK, evict-at-logout) is FIXED. Split A-5a (login core; Stratum-independent) / A-5b (encrypted home + the Stratum sub-chunk) / A-5c (RECOVER + hostowner-c) -- a focused round each. **No new spec** per the 2026-05-23 broadening -- prose in IDENTITY-DESIGN.md §9.9 + this row + the per-sub-chunk audits + the runtime + cross-reboot + login-E2E tests. |
| A-5c RECOVER recovery keyslot (corvus) | `usr/corvus/src/main.rs` (the new `VERB_RECOVER`=8 handler [`subject_kind` 0=system / 1=user]; the BIP-39 wordlist + phrase gen/decode/checksum; the per-user `recovery.corvus` mint at `handle_user_create` + the phrase returned in the OK response; the keypair re-wrap + rename-swap of `hybrid.corvus` / `system-wrap`; the fresh-phrase roll), `usr/login/` (the login recovery path + the create-time phrase surfacing); **A-5c-b host-bake (user-voted 2026-06-05)**: NEW `usr/lib/corvus-crypto/` (shared `no_std` crypto lib extracted from corvus -- the CRVS wrap layout + `argon2id_kek` + AEGIS wrap/unwrap + BIP-39 codec + keypair-gen, parameterized over an RNG), NEW `usr/corvus-mint/` (host-target minter, `OsRng`), `tools/build.sh` (build corvus-mint host-side + bake the `/var/lib/corvus` dir-chain + `system-wrap` / `system-recovery-wrap`, SYSTEM-owned), `usr/corvus/src/main.rs` (`system_identity_load` at boot + the real `handle_admin_elevate` argon2id+AEGIS unwrap of `system-wrap`, replacing the `b"thylacine"` byte-compare + RECOVER(`subject_kind=0`) console gate) | Convergence-detour A-5c (IDENTITY-DESIGN.md §9.9 design pass + CORVUS-DESIGN §5.5.1/§5.6/§6.4/§8/§9; user votes 2026-06-05: user-held-only [NO hostowner escrow] + mandatory enrollment + host-bake the system identity). **AEGIS/mallocng-adjacent recovery crypto -- prosecute hard.** A recovery keyslot = a 2nd wrap of the SAME hybrid keypair under a recovery-phrase KEK (`recovery.corvus` / `system-recovery-wrap`); it re-wraps the KEYPAIR not the DEK -> the Stratum DEK envelopes stay valid -> **NO Stratum / kernel surface**. Invariants: **C-28 no-escrow** (corvus stores no copy of a user's keypair/DEK recoverable by any authority other than that user's passphrase OR own phrase; the hostowner has NO user-data-recovery verb -> D3 mutually-encrypted-homes survives a malicious hostowner); **C-27** (per-user keyslot minted at USER_CREATE, displayed once, never persisted; same keypair as `hybrid.corvus`; domain-separated AD `"thylacine-corvus-recovery-v1"`); **C-20** generalized. Prosecute: the **gate** (RECOVER(user) = phrase + rate-limit ONLY, no session / no `CAP_HOSTOWNER`; RECOVER(system) additionally console-attached); **phrase brute-force** (the C-16 rate-limit is the online defense); **secret hygiene** (mlock + `explicit_bzero` of phrase, recovery_KEK, keypair, new_KEK on EVERY path incl. error); the **twin-wrap atomicity** (re-wrap `hybrid.corvus` AND roll `recovery.corvus`; a crash between must not strand the user un-recoverable nor leave the old phrase live; rename-swap, A-1.6); **no-DEK-rewrite correctness** (the recovered keypair must byte-equal the original so existing envelopes decapsulate); the **USER_CREATE OK-response growth** (callers parse the appended phrase; no overrun). **A-5c-b (host-bake) additionally**: the `corvus-crypto` extraction MUST preserve byte-identical wraps (the A-5c-a / A-1b / A-5b wrap + DEK-envelope paths behavior-unchanged); a host-minted `system-wrap` and the on-device unwrap must round-trip (same CRVS layout / AD / Argon2id -- no `corvus-mint` <-> corvus drift); the real `handle_admin_elevate` fails CLOSED on a missing / corrupt / wrong-passphrase `system-wrap`; RECOVER(`subject_kind=0`) requires console attachment; the bake lands the files SYSTEM-owned where corvus's chroot resolves them. **No new spec** per the 2026-05-23 broadening -- prose validation in IDENTITY-DESIGN.md §9.9 + CORVUS-DESIGN §5.5.1/§5.6 + this row + the audit + corvus unit tests + the boot E2E. |
| `SPAWN_PERM_MAY_POST_SERVICE` one-hop delegation (A-5b #827b) | `kernel/syscall.c` (the `SYS_SPAWN_*` perm-grant gate -- now per-bit: `CONSOLE_TRUSTED` requires console-attach; `MAY_POST_SERVICE` requires console-attach OR `proc_may_post_service(p)`), `kernel/include/thylacine/proc.h` (`proc_may_post_service`), `usr/joey/joey.c` (spawn login with `SPAWN_PERM_MAY_POST_SERVICE`), `usr/login/src/main.rs` + `usr/lib/libthyla-rs/src/process.rs` (`Command` gains a perm setter; login confers the bit on the per-user proxy) | A-5b #827b (user-voted 2026-06-04). **Privilege-gate change -- prosecute hard.** Lets a non-console-attached but trusted session authority (`/sbin/login`, spawned by the console-attached joey *with* the bit) confer `MAY_POST_SERVICE` on per-user `--role client` proxies. Prosecute: the per-bit separation (a `MAY_POST_SERVICE` holder can NEVER confer `SPAWN_PERM_CONSOLE_TRUSTED` -- the SAK anchor stays console-attach-only, I-27); the bit is never `rfork`-propagated (still a `perm_flags`-only decision, NOT a `cap_mask` bit -- I-2 unaffected); the delegation root is the console-attached joey (no path for an ordinary Proc to acquire the bit); soundness rests on *who holds the bit* (only joey's deliberately-conferred OS servers -- corvus, login, the proxy). **No new spec** -- prose validation in IDENTITY-DESIGN.md §9.9 + this row + the #828 audit + the grant-gate regression test. |
| Pathname resolution (`stalk`) + namespace-resident `/srv` | the new resolver (`stalk` + `cross_mounts`/`domount` + the in-call `trail`; folds in / supersedes `sys_walk_open_handler`); `kernel/include/thylacine/territory.h` + `kernel/territory.c` (`PgrpMount` re-keyed from `path_id_t` to mount-point Spoor identity `dc`+`qid.path`; `mount`/`unmount` path-keyed; the size-pinned `Territory` static_asserts re-bumped); `kernel/syscall.c` (`SYS_OPEN(path)` multi-component handler; `SYS_MOUNT`/`UNMOUNT` path-keyed; retire `SYS_SRV_CONNECT` + `SYS_POST_SERVICE`); `kernel/devsrv.c` (per-territory service table; `devsrv_open` = connect; `devsrv_create` = post); migrate every `/srv` client + `usr/lib/pouch/patches/0006-pouch-sockets.patch` | Convergence-detour A-5b-0 (`docs/STALK-DESIGN.md`; user-voted full Plan-9 spine 2026-06-02). **Path resolution is a privilege boundary -- prosecute hard.** Invariant **I-28** (path-resolution containment + per-component X-search). Prosecute: `..` escape above `root_spoor`; per-component X-search bypass (symlink / `..` / mount-cross tricks); Spoor lifetime across N hops (UAF / double-clunk / leak on the `trail`); per-territory `/srv` isolation (a 2nd user cannot NAME another's coordinator); mount-cross into a tree the caller lacks X on; the connection-handle reconciliation (`SO_PEERCRED`, the attach-9p unification, the KObj_Srv->KOBJ_SPOOR endpoint shift); the per-Spoor `SrvRegistry` ref lifecycle (drain-on-last-unref; no UAF / double-free); tokenizer bounds. **stalk-3 sub-design resolved (STALK-DESIGN §5, D5/D6/D7): two-step open=connect, `DMSRVBYTE` create=post, full per-territory refcounted registry, 9P-unification (embedded `srvconn_client_*` retires), `SRV_CONN_PER_PROC_MAX` removed; split 3a/3b/3c.** One focused round per sub-chunk (stalk-1/2/3a/3b done; stalk-3c audit CLEAN 0/0/0/3 -- all P3 doc-staleness; stalk-3 ARC COMPLETE). **#957 (single-hop crossing):** `cross_mounts` was made public as `stalk_cross_mounts` and the single-hop `sys_walk_open_handler` (which libthyla-rs `fs::` navigates parent dirs with, component-by-component) now crosses mounts at the SOURCE (before X-search/walk) AND the RESULT (before open), exactly like stalk -- so a logged-in user's create into their own `/home/<user>` mount reaches the mounted user-owned 0700 root, not the shadowed SYSTEM placeholder. Crossing into `/srv` also required making the devsrv registry ROOT openable + stat-able as a directory (`devsrv_open` root-as-dir branch + new `devsrv_stat_native` -> QTDIR 0555 SYSTEM; the open+stat prerequisite of #932 readdir). Prosecute the single-hop cross's `src`/`nc` lifetime (clunk-exactly-once on every exit) + the devsrv root navigability. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in `docs/STALK-DESIGN.md` + ARCH §9.6.7 + `docs/reference/104-stalk.md` (Single-hop walks cross too) + `docs/reference/70-devsrv.md` + this row + the #957 audit + the runtime tests (`devsrv.open_root_dir` / `devsrv.stat_native_root` + the u-builtin cd /srv boot path + the ls-3b.exp /home-write E2E). |
| Exec from the namespace (spawn binary resolution, #58) | `kernel/syscall.c` (`exec_load_from_namespace` -- the new helper resolving the program name via `stalk(STALK_OPEN, OEXEC)` against the caller's Territory [absolute from `root_spoor`; relative via `territory_resolve_cwd`] then slurping the ELF from the resolved Spoor via `dev->read` into an 8-aligned blob; the FIVE `*_for_proc` spawn bodies -- `sys_spawn_for_proc` / `_with_fds_for_proc` / `_with_caps_for_proc` / `_full_with_perms_for_proc` / `_full_argv_with_perms_for_proc` -- route through it INSTEAD of `devramfs_lookup`), `kernel/exec.c` (`exec_setup` UNCHANGED -- still takes the slurped blob; the ELF loader / W^X reject / segment map byte-identical), `kernel/devramfs.c` (`devramfs_lookup` RETAINED for the kproc init-load [`/joey`, no namespace yet] + kernel tests ONLY -- NOT EL0-reachable); the bootstrap (option B, user-voted 2026-06-12): `usr/joey/joey.c` (post-pivot bind the cpio root onto `/bin` via the pre-pivot-handle -> post-pivot-MREPL pattern it already uses for `/srv`; migrate the post-pivot corvus / legate-prover / login spawns to `/bin/<name>`; pre-pivot bare-name spawns unchanged), `usr/login/src/main.rs` (`SHELL` + the home-proxy `stratumd` -> `/bin/<name>`), `usr/utopia/libutopia/src/eval/stmt.rs` (the `$path`=`/bin` resolution at the 4 `Command::new` sites: a bare command -> `/bin/<name>`, a command with `/` used as-is) | #58 (RW-10 F9 / RW-12 W5-F1 [H2]; the container keystone; user-voted "build the tractable core" D2 2026-06-11 + the option-B `/bin` bind 2026-06-12). **Privilege boundary -- prosecute hard.** Realizes **I-28** (every spawn now resolves through `stalk`: `..`-contained at `root_spoor`, per-component X-search, mount-cross by full Spoor identity) + **I-1** (a confined Proc can exec ONLY what its namespace names -- closes the reverse-visibility leak where a confined Proc could spawn any cpio binary) for the exec path; NO new invariant. Prosecute: resolve-in-parent-context correctness (the parent's Territory, like Unix exec); the slurp bound (`SYS_SPAWN_BLOB_MAX`; short-read / truncated-file -> `elf_load` reject, no partial map); Spoor lifetime (the borrowed `start` released, the quarry clunked after the read; no UAF / leak on any error path); the blocking dev9p read is #811-death-interruptible; the reverse-leak closure (a `stalk` miss is `-1`, NEVER a `devramfs_lookup` fallback); X-search on the `/bin` bind (mount-cross into the devramfs tree the SYSTEM chain has owner-X on); the `OEXEC` -> `RIGHT_READ` handle envelope (the child cannot over-read). **No new spec** per the 2026-05-23 broadening -- prose validation in ARCH §9.6.8 + `docs/reference/14-process-model.md` + `docs/reference/104-stalk.md` + this row + the focused audit + the kernel tests (resolve-by-path + reverse-leak deny + X-search deny) + boot OK (the joey `/bin` bind + post-pivot corvus/login) + the SMP gate. |
| consctl-fd inheritance: the I-27 gate split for `/dev/consctl` (#94-B) | `kernel/devdev.c` (the gate split: `dev_kind_is_console` [open-mint gate, BOTH leaves] kept; NEW `dev_kind_is_cons_io` [cons only] gates `devdev_read`/`devdev_write`/`devdev_poll`, so **consctl I/O is ungated** while **cons stays fully re-gated**; the `devdev_console_gate_ok` soundness comment), `usr/joey/joey.c` (`session_getty_loop(cfd, consctl_fd)` builds a 4-fd + `"login\0--consctl-fd\03\0"` spawn; the (3b) `/dev/consctl` `T_ORDWR` open at the console transition, BEFORE `t_console_relinquish` while still console-attached; `do_login_e2e` opens consctl + 4-fd spawn = the deterministic CI proof), `usr/login/src/main.rs` (`parse_consctl_fd` ["--consctl-fd N"], `set_console_mode` [best-effort `cons_set_mode_cmd` write], the LS-6 dance: cooked+echo username / cooked-noecho passphrase / restore-default), `kernel/test/test_devdev.c` (`devdev.cons_gate` flips the consctl-I/O assertions deny->allow) | #94-B (the console-mode-access fork resolved **B = inherited consctl fd**, user-voted 2026-06-12, over C = `SYS_CONSOLE_MODE(fd)`; the LS-6 fold-in + the LS-7/#94-B-b enabler). **Invariant I-27 (trusted path) -- the consctl I/O ungating must NOT touch the console-ATTACH/SAK gate or the cons (input) gate; prosecute hard.** Drops the consctl-ONLY I/O re-gate so a non-console-attached but deliberately-delegated Proc (login, the session shell) sets the line discipline via an INHERITED consctl fd. Sound because: (a) the OPEN mint-gate (kept for both leaves) means only a console-attached Proc creates a consctl fd by name; (b) `CWALKONLY`/#81 rejects an O_PATH-walked handle at `sys_read`/`write`/`readdir` BEFORE `dev->read`/`write` (verified `kernel/syscall.c:760`/`788`); (c) a consctl fd reaches a non-attached Proc ONLY by deliberate spawn-inheritance from the trusted chain (joey, console-attached, opens it pre-relinquish + hands it down joey->login) -- the inherited fd IS the capability, and consctl is a control surface (5 mode flags) that can never read console INPUT, so an ungated consctl write cannot exfiltrate a keystroke. **cons (the data leaf) keeps its full open+read+write+poll re-gate** (console-input theft = the A-5a-F2 break; unchanged). Prosecute: the gate split is complete (cons still gated at every I/O site; consctl ungated for I/O only, open still gated); every path to a consctl fd is closed to an untrusted Proc; the SMP surface is the LS-8b-audited `cons_set_mode_cmd` under `g_cons.lock` (no new shared state); the plumbing (positional fd-4 install, argv encoding, login parse + the mode-restore on every exit path); the password-mask hard guarantee survives a mode-set failure (degrades to the pre-LS-6 raw prompt, password still masked, never echoed). The consctl-file scripture (§23.5.1) is **preserved** -- no scripture change. **No new spec** per the 2026-05-23 broadening -- prose validation in §23.5.1 + LIFE-SUPPORT.md LS-6/LS-8b + `docs/reference/111-cons.md` + this row + the focused audit + `devdev.cons_gate` (the flipped consctl-I/O regression) + the seeded `do_login_e2e` `login: consctl ok` boot-log witness + boot OK + the SMP gate (0 corruption). **#94-B-b (LANDED) -- the login->ut forward (PURE USERSPACE; the kernel binary is byte-identical -- the #94-B-a gate above is UNCHANGED):** a NEW `libthyla-rs Command::inherit_fd(fd)` generalizes the spawn fd_list from the 3 stdio slots to 3+N (extra inherited fds at child slots 3.., bounded `<= SYS_SPAWN_MAX_FDS`; the kernel `sys_bump_inherit_fds` already installs positionally + inherits the parent slot's rights, I-6). `/sbin/login` forwards its inherited consctl fd to the session shell `ut` (`.inherit_fd(consctl_fd)` + `--consctl-fd 3`), so **`ut` -- a USER-identity, NON-console-attached Proc -- owns the console line discipline** (the controlling-terminal termios), held on the `Repl`'s `Env.consctl_fd`. Sound: (a) the reach extends ONLY via the same deliberate trusted chain (joey console-attached -> login -> ut), the inherited fd IS the capability; (b) `ut` holds the consctl fd PRIVATELY (it never re-forwards it to a user child it spawns -- a user child's `Command` carries only the 3 stdio slots); (c) **I-27 untouched** -- holding/driving a consctl fd confers NO console-attach (the SAK/elevation gate keys on `PROC_FLAG_CONSOLE_ATTACHED`, which `ut` lacks), and consctl is a control surface (the 5 mode flags) that can never read console INPUT (qid 6 / `cons_input_read` is a SEPARATE handle `ut` is NOT given as consctl). `ut` establishes its prompt mode (writes `CONSOLE_MODE_DEFAULT` once via `Repl::console_apply_default`) -- the boot witness `ut: consctl ok`. The **raw/cooked dance** (set cooked/raw + switch a foreground child's stdin Piped->Inherit + restore after) is DEFERRED to **LS-7** (the editor arc, where the child's mode needs are known; `stmt.rs:84` already marks interactive-child stdin as LS-5/LS-8). Validation: `ut: consctl ok` from the seeded `do_login_e2e` boot log (the end-to-end `inherit_fd` regression) + boot OK + 0 EXTINCTION; no SMP gate (kernel byte-identical -- the U-6g pure-userspace precedent). |
| `/dev/cons` drain/feed renderer backend + `SPAWN_PERM_CONSOLE_RENDERER` (Tapestry G-4) | `kernel/cons.c` (the `cons_drain` mirror tap in `cons_emit` [program output + line-discipline echo -- the one chokepoint]; the bounded 8-KiB drop-OLDEST ring; `cons_drain_open`/`close`/`read`/`poll` + `cons_feed_write`; the LS-8a deferred poll-wake relay SECOND instance through `console_mgr`), `kernel/devdev.c` (the `consdrain`/`consfeed` leaves: open gate + per-I/O re-gate + POLLNVAL poll gate on `proc_is_console_renderer`; the COPEN-checked close-disarm), `kernel/proc.c` + `proc.h` (`PROC_FLAG_CONSOLE_RENDERER` + the single-holder `g_console_renderer` NULL-only claim under `g_proc_table_lock` + the zombie-chokepoint release), `kernel/syscall.c` + `syscall.h` (`SPAWN_PERM_CONSOLE_RENDERER` = 1u<<3: console-attach-only grant + the holder-exists refuse), `usr/lib/libt` (`T_SPAWN_PERM_CONSOLE_RENDERER`), the consumer `usr/aurora` + `usr/joey/joey.c` (the perm-granted spawn) | Tapestry G-4 (TAPESTRY.md section 18.7 / 18.11-F8 / 18.12-R2-F6; AURORA.md section 4; as-built `docs/reference/140-aurora.md`). **The I-27 THIRD console role -- a new privilege surface on the trusted-path device; prosecute hard.** The bound RENDERER (Aurora) holds the drain/feed pair: console output MIRRORS into the drain ring it reads; its decoded keyboard bytes enter the EXISTING LS-8 line discipline via the feed (cooking/ECHO/ISIG unchanged -- a graphical Ctrl-C rides ISIG to the console OWNER). NO new section-28 invariant (I-27 refines to three roles; composes I-9). Prosecute: **the role conveys exactly the pair** (no elevation authority -- not ATTACH; no interrupt-target authority -- not OWNER; the SAK/redeem gates key on ATTACH only, untouched); **the F8 leak gate** (open + EVERY I/O + poll re-gated on the live single holder -- a non-renderer can neither read the output stream nor inject input nor register a timing-leak poll hook; the O_PATH bypass closed by the re-gate + #81 CWALKONLY); **no SAK forgery** (`cons_feed_write` hardwires `is_break=false` -- the serial BREAK line condition stays the ONE unforgeable trigger; structural, by construction); **the narrow grant** (console-attach-only, never MAY_POST_SERVICE-delegable -- the CONSOLE_TRUSTED shape; single-holder: grant-check refuse + the claim-under-lock CAS whose loser is fail-closed at the open gate); **writers never block on the renderer** (the drop-oldest bounded ring -- a stalled/dead renderer cannot wedge console writes; the crash path [SYS_PUTS/extinction/Halls] is uart-direct and NEVER in the drain); **the I-9 deferred-wake second instance** (the drain's IRQ-context POLLIN edge -> `console_mgr`'s process-context hook walk -- the cons_poll.tla discipline; the drain lock is a leaf); **the tee posture** (on serial-bearing media the UART path is byte-identical -- the tooling ABI + the serial trusted path keep working; the exclusive board-era switch bound from the DTB medium fact [TRUSTED-PATH section 7] is the recorded seam and will gate `uart_putc`, composing with the tap); **teardown** (the opened drain Spoor's close -- incl. #926/#68 close-at-exit -- disarms via the COPEN hook; the parked reader wakes to EOF; the role releases at `proc_become_zombie_locked` on every death path). **No new spec** per the 2026-05-23 broadening (the wake protocol is the audited cons_poll.tla shape, second instance) -- prose in TAPESTRY.md section 18.7 as-built + `docs/reference/140-aurora.md` + this row + the focused G-4 audit + the 7 kernel tests (`cons.drain_*` x5 + `devdev.renderer_gate` + `sys_spawn_with_perms.renderer_gate`) + the evolved per-boot console gate (`screendump -c` signature + retry-compare liveness, every ci-smp-gate boot) + the `ls-gfx` interactive E2E (login + ls rendered; QMP-typed input through the FULL loop asserted on the serial tee) + boot OK + the SMP gate. |
| Namespace layout: /proc + /ctl mounts (#57a) | `kernel/devramfs.c` (the synthetic `/ctl` mount-point dir added to `g_ramfs_synth_dirs[]` alongside `/srv` + `/proc` -- 0555 SYSTEM-owned, world-searchable), `kernel/joey.c` (`joey_mount_static_dev` -- the generalized /srv-idiom helper -- + the boot-namespace mounts of `devproc` @ /proc + `devctl` @ /ctl in the kproc territory), `usr/joey/joey.c` (the pre-pivot `t_open(/proc,/ctl, O_PATH)` handle grabs + the post-pivot mkdir + MREPL re-grafts, mirroring /srv + /bin), `kernel/devctl.c` + `kernel/devproc.c` (the `Dev.walk` reuse-`nc` fix) | #57a (RW-10 F8 / HT10.F8 [H2]; the container keystone's layout half; ARCH §9.4 as-built; RW-13 D2 vote "bind /dev + /proc + /ctl"). **The default namespace = the ambient-authority surface -- prosecute hard.** Realizes ARCH §9.4 for the kernel introspection Devs; **NO new invariant**. Prosecute: the mount widens *visibility* not *authority* (`devctl_write == -1` read-only; `/proc/<pid>/ctl` kill writes stay **I-26** two-axis-gated -- owner OR CAP_HOSTOWNER/CAP_KILL -- independent of namespace reachability; `devproc`/`devctl` `perm_enforced == false` = Plan 9 all-pids-visible, the post-pivot disk mount points are 0755 so a logged-in user reaches them; the synth dirs are 0555 SYSTEM-owned so the SYSTEM boot chain searches them pre-pivot); the **reuse-`nc` walk fix** (devctl/devproc had never been mounted, so their `Dev.walk` kept the pre-16b-gamma self-cloning shape -- ignoring the caller's pre-clone `nc` and returning their own `spoor_clone`, which `stalk`/`clone_walk_zero` reject [`wq->spoor != nc`], so a mounted devctl/devproc was unreachable through `stalk` AND the boot's pre-pivot `t_open(/proc)` cross would fail; the fix is the dual mode devramfs_walk adopted at 16b-gamma: non-NULL `nc` -> reuse + mutate + return as `wq->spoor`, a 0-element walk -> `nqid == 0`; the `nc == NULL` legacy path is preserved for the kernel-internal direct-call tests; no UAF/leak -- the crossed `nc` shares the stateless Dev's aux, clunk-safe); the boot-mount + re-graft refcount discipline (the /srv idiom: `dev->attach` ref dropped after `mount` takes its own / after the failure path; the pre-pivot O_PATH handle closed after the post-pivot MREPL); `/proc` all-pids-visibility is the v1.0 behavior (per-namespace `/proc` filtering is the container runner #70). **No new spec** per the 2026-05-23 broadening -- prose validation in ARCH §9.4 (the as-built note) + `docs/reference/32-devproc.md` + `docs/reference/33-devctl.md` + `docs/reference/104-stalk.md` + this row + the focused audit + the kernel tests (`namespace_layout.proc_ctl_cross` mount + cross + the `devramfs` synth-`/ctl` trio) + boot OK (the boot-namespace mounts + post-pivot re-graft) + the SMP gate. **Focused audit (Fable round-1 + round-2 on the fixes, CONVERGED CLEAN): 0 P0 / 2 P1 / 1 P2 / 1 P3, all FIXED** -- the round-1 reviewer FALSIFIED the "visibility not authority" self-claim on the READ path: F1 `/ctl/kernel-base` leaked the live KASLR slide (I-16) world-readable -> `CAP_HOSTOWNER`-gated; F2 cross-Proc `devproc_read`/`stat` UAF (bare `Proc` deref after `proc_find_by_pid` released the lock) -> find+format under `g_proc_table_lock`; F3 unbounded `/ctl/procs` IRQ-off proc-table walk -> stop-at-buffer-full; F4 stale "lockless" comments. All pre-existing devproc/devctl read-path latents the mount ACTIVATES; closed in #57a (pull-forward). `memory/audit_57a_closed_list.md`. **`/dev` (the kernel char devices + the I-27 cons/consctl question) is #57b** -- a user scope vote, since HT10.F8 reserved the `/dev` scope for RW-13. |
| Per-Proc cwd (`SYS_CHDIR` / `SYS_GETCWD` + the `SYS_OPEN` relative->cwd join) | `kernel/include/thylacine/territory.h` + `kernel/territory.c` (`Territory.dot_path` -- the cleaned absolute cwd string, `NULL`==`"/"`, heap-allocated lazily, bounded by `SYS_OPEN_PATH_MAX`, size/offset `_Static_assert`s re-bumped; `territory_clone` deep-copies it [child inherits the parent snapshot, independent thereafter]; `territory_unref` final release frees it; a `territory_setdot`/`territory_getdot` helper pair under the territory lock), `kernel/syscall.c` (`sys_chdir_handler` [resolve `clean(join(dot_path,path))` from `root_spoor` via `stalk` -> require `QTDIR` + `perm_check` X -> swap `dot_path` under the territory lock -> clunk the verification Spoor]; `sys_getcwd_handler` [copy out `dot_path`, `-ERANGE`]; `sys_open_handler` [the relative->cwd join at the sentinel-start-fd + relative-path branch, BEFORE `stalk`]), `kernel/include/thylacine/syscall.h` (`SYS_CHDIR = 69` / `SYS_GETCWD = 70` + ABI), `usr/lib/libt` + `usr/lib/libthyla-rs` wrappers (the `fs::` relative mutations join via `getcwd`) | LS-4 (LIFE-SUPPORT.md; user-voted "kernel per-Proc cwd" 2026-06-09 over shell-side argv-expansion). **Path-resolution + a new syscall surface -- prosecute hard, but note the design DELIBERATELY adds NO new I-28 mechanism.** Invariant **I-28 PRESERVED** (the cwd-join resolves from `root_spoor` exactly like an absolute path; `stalk` clamps `..` at `root_spoor`, so a hostile un-cleaned join cannot escape -- the join is a convenience, not a containment authority); the chdir **X-search gate** is the per-Proc-identity check (a Proc may only `cd` into a directory it can search -- the open-path `perm_check`); threads share `dot_path` (POSIX per-process cwd) under the territory lock (no torn read/swap); `territory_clone` snapshot inheritance (no shared-mutable cwd across the fork boundary unless the Territory is `RFNAMEG`-ref-shared); `dot_path` lifetime (lazily allocated, freed exactly once at `territory_unref`, no leak/double-free/UAF; the join bounds-checks against `SYS_OPEN_PATH_MAX` -> `ENAMETOOLONG`). **Name-based (a string), NOT handle-based (a `dot` Spoor), for v1.0** -- because `stalk` contains `..` at its trail-floor `start` (a handle-`dot` start strands `cd ..` without a new device-`..`-walk mechanism on the I-28 surface) AND the symlink/`..` correctness argument is moot pre-symlinks (G11). Handle-based is the v1.x upgrade, landing with symlinks. **No new spec** per the 2026-05-23 broadening -- prose validation in LIFE-SUPPORT.md (LS-4) + STALK-DESIGN.md (the cwd-join note in 4.3) + `docs/reference/104-stalk.md` + this row + the focused audit + the kernel unit tests (`chdir`/`getcwd` round-trip; relative open resolves against dot; `..` clamps at root; X-search enforced) + the LS-CI relative-`cat` E2E. |
| Loom -- the io_uring-inverted 9P ring transport (KObj_Loom + SQ/CQ rings + registered handles + the pluggable-completion 9P-engine seam) | `kernel/include/thylacine/loom.h` (the ABI: `loom_sqe`/`loom_cqe`/`loom_ring_hdr`/`loom_params` + the `_Static_assert`s + `struct Loom`), `kernel/loom.c` (the KObj_Loom ring substrate -- `loom_create`/`loom_ref`/`loom_unref` + the registered-handle table), `kernel/syscall.c` (`sys_loom_setup_for_proc` / `sys_loom_register_for_proc` + the SVC handlers + dispatch; `SYS_LOOM_SETUP = 66` / `SYS_LOOM_REGISTER = 67` / [reserved `SYS_LOOM_ENTER = 68`]), `kernel/handle.c` + `kernel/include/thylacine/handle.h` (`KOBJ_LOOM` + the fourth partition mask + the acquire/release branches); **Loom-2b adds** `kernel/9p_client.c` (the pluggable-completion seam -- `p9_rpc.on_complete` `WAKE_RENDEZ` vs `POST_CQE` + `p9_client_submit_async` + `p9_client_reader_pump_once`) + `kernel/loom.c::loom_post_cqe` (the CQ writer, `CqNeverOverfull`); **Loom-3 adds** `kernel/loom.c` (`loom_enter` [SQ-index consume from the kernel-private `sq_head` + the TOCTOU SQE copy] + `loom_submit_one` [SQE dispatch: NOP inline + FSYNC via the submit-time pin; payload opcodes -> `-ENOSYS` until Loom-6] + `struct loom_async_op` [the production container, the I-30 `spoor_ref` pin] + `loom_async_complete` [CQE post + terminal under `l->lock`, no sleep] + `loom_reap_terminal` + the `loom_free` quiesce-before-free #898), `kernel/9p_client.c::p9_client_abandon_async` (#845 Tflush-on-abandon), `kernel/dev9p.c::dev9p_client_fid`, `SYS_LOOM_ENTER = 68` + `sys_loom_enter_for_proc` | Loom arc (`docs/LOOM.md`, signed off 2026-06-05). **Shared-memory async boundary -- prosecute hard.** Reserves **I-29** (completion integrity: no lost / double / stale; CQ back-pressure) + **I-30** (submit-time capability pin -- resolve + snapshot at submit, never re-resolve at completion; ring TOCTOU). Prosecute: ring TOCTOU (the kernel copies every SQE field before acting, never re-reads the shared ring after the check); the KObj_Loom + ring-Burrow lifecycle (the #847 dual-refcount keeps the pages alive while the kernel OR the user mapping holds a ref; no UAF / leak; teardown clunks every registered Spoor + `burrow_unref`s the ring); the registered-handle table (the held `spoor_ref` is the I-30 pin; the rights snapshot captured at register; non-transferable + non-dup-able KObj); the pluggable-completion refactor of the audited #841 engine (the seam must not break the synchronous path -- a LOOM op has no blocked submitter, so the reader handoff must skip it + a session death must complete it with an error CQE) [Loom-2b]; **the SQPOLL poll-thread + CQ wait-queue [Loom-4, LANDED]** -- `kernel/loom.c` (`loom_sqpoll_main` the `kproc()` kthread + `loom_start_sqpoll` + `loom_drain_sq` + the `loom_free` JOIN [stop + wake `sqpoll_park` + spin `sqpoll_exited` + `thread_free` before the #898 quiesce] + the IRQ-masked EXITING terminal [the kproc-kthread reap minus `thread_exit_self`'s zombie bookkeeping] + the deadline-capable register gate + the F2 admittability park cond + the F1 `loom_first_inflight_client` borrow-guard ref), `kernel/9p_client.{c,h}` + the transport `set_recv_deadline` backends (4a; spoor=NULL=rejected), the 4b `cq_waiters` wait-list, `sys_loom_setup_for_proc(LOOM_SETUP_SQPOLL)`. Prosecute: the join terminates (the EXITING handshake's preempt-loss-freedom + release/acquire + on_cpu spin); the deadline-gated interruptible recv; the lock-free park cond (race-free at async_inflight==0); I-9 on `cq_waiters`; the borrowed-client lifetime across the pump (F1). **Audit closed 0/1/1/3** (F1 UAF + F2 park busy-loop FIXED; DIRTY -> round-2; `memory/audit_loom_closed_list.md`). OWED: the live-FSYNC cross-Proc-death SMP harness (Loom-6 + #841/#907). multishot [Loom-5]. **Spec-first RE-ENABLED for this surface** (LOOM.md §7): the Loom spec SUITE (clean + liveness + the per-bug cfgs) is a pre-commit gate; a new mechanism extends the suite FIRST. Split Loom-2a (substrate) / 2b (engine seam) / 3 (dispatch) / 4 (SQPOLL) / 5 (multishot+LINK) / 6 (regbuf + native API + bench) -- one focused audit over the audit-bearing sub-chunks. **Loom-5 added TWO new focused spec modules** -- `specs/loom_multishot.tla` (the multiset-CQ stream lifecycle) + `specs/loom_order.tla` (LINK/DRAIN ordering + chain-cancel completeness) -- leaving the audited `loom.tla` UNTOUCHED (its single-CQE-per-op `cq` cannot represent a multiset CQ; the `sched_oncpu.tla` + `sched_alpha.tla` precedent). **Loom-5 IMPL adds** (5a `cbba808` multishot + 5b `3cf1852` LINK/DRAIN): `kernel/loom.c` (`loom_async_complete` MORE/terminal + `loom_rearm_pending`; `struct loom_chain_op` + `l->chain` + `loom_chain_drain_admits` [drain gate `async_inflight==0 && rearm_pending==0`] + `loom_admit_chain` [link cancel-cascade + drain barrier + cancel-retry] + `loom_reclaim_chain` + the `loom_drain_sq` chain routing + `loom_chain_done` under l->lock), `kernel/include/thylacine/errno.h` (`T_E_CANCELED = 125`). **Loom-5 audit closed 0/0/2/4** (F1 drain-early-vs-rearm-pending + F2 cancel-CQE-drop-vs-over-admit FIXED; F3 lock-free state write FIXED; F4/F5/F6 single-driver-contract + OOM-LINK + LINK-single-producer DOCUMENTED; the F2 dispatch-leg residual + exact-concurrent-admission OWED to Loom-6 with the SMP harness). **Loom-6a/6b adds** (registered buffers + the uniform `p9_client_*` payload surface; `738bd3e`/`2592560`/`e329126`): `kernel/loom.c` (`loom_register_buffers`/`loom_resolve_buf` [the I-30 BUFFER pin: a writable-anon-VMA `burrow_ref`, W^X RW-only, the BURROW_TYPE_ANON contiguity guarantee]; `loom_submit_payload` the opcode -> (builder, primary-right, secondary-right) dispatch for READ/WRITE [6a] + READDIR/READLINK/GETATTR/STATFS [6b-1] + SETATTR/MKDIR/MKNOD/SYMLINK/UNLINKAT/RENAMEAT/LINK [6b-2 mutation, names-from-buffer]; the THREE I-30 pins `op->pinned` + `op->pinned2` [the 2nd fid for RENAMEAT/LINK, SAME-session `cl1==cl2`] + `pinned_buf`, released via the goto-fail epilogue; the TWO submit-time memory-safety gates on the SQE snapshot [`0 < two-name split < len`; SETATTR `len >= 56`]), `loom.h` (the `loom_buf_reg` + `LOOM_SQE_BUF_OFF`/`LOOM_SQE_FID2` + the `p9_setattr`/`p9_attr`/`p9_statfs` size+OFFSET asserts). **Loom-6c adds** (the multi-in-flight harness + the focused audit close): `kernel/9p_transport_mq.{c,h}` (a byte-FIFO queueing test transport staging N in-flight replies -- the single-slot loopback cannot) + 2 deterministic multi-in-flight tests driving the multi-entry `inflight_ops` + the borrow-guard across a real pump (balance asserted by spoor-frees-once). **Audit closed 0/0/1/3** (one Opus prosecutor + self-audit): F1 [P2] >INT32_MAX count-vs-errno sign-alias FIXED (`loom_count_result` clamp); F2/F3/F4 [P3] two-name-`==count` gate + BURROW_TYPE_ANON contiguity anchor + offsetof asserts FIXED. NOT dirty. OWED to Loom-6d (native API + TSan): the CONCURRENT two-thread + cross-Proc-death SMP stress harness. **Loom-6d adds** (the native userspace API + harness/bench, the ARC CLOSE; 6d-1 `28967e7` + 6d-2 `caedb60` + 6d-3 `57ce416`): `usr/lib/libthyla-rs/src/loom.rs` (the native `Ring` over SYS_LOOM_SETUP/REGISTER/ENTER -- `repr(C)` byte-pinned to `loom.h` with `offset_of!` asserts; the SPSC release/acquire ring model paired with the kernel `__atomic_*`; Drop via #847) + the 3 svc wrappers (`T_SYS_LOOM_*=66/67/68`); `usr/loom-smoke` (pre-pivot gate); `usr/loom-stress` (the OWED CONCURRENT two-thread-same-`loom_fd` async-FSYNC stress + the cross-Proc-death #898 quiesce, joey-spawned post-pivot, every ci-smp-gate boot); `usr/loom-bench` (~7.7x NOP trap-amortization). Client code over the frozen ABI -- the kernel validates everything; not a privilege surface. **Audit closed 0/0/3, all FIXED** (one Opus prosecutor + self-audit, 2/3 cross-confirmed): F1 offset asserts; F2 bench reap-loop guards; F3 phase-3 in-flight-at-death best-effort doc. NOT dirty. The OWED concurrent SMP harness (since #841) is DELIVERED -- the SMP gate PASS 0 corruption; the exact borrow-guard free-race stays reasoned+round-2-audited + TSan unrestored (the gate is the witness). **THE LOOM ARC IS COMPLETE.** Prose validation in `docs/LOOM.md` + `docs/reference/107-loom.md` (the Loom-6 section) + this row + the audit + the kernel tests. |
| Resource/DoS floor -- per-Proc page/thread/child caps (#65) | `kernel/include/thylacine/proc.h` (`PROC_PAGE_MAX` / `PROC_THREAD_MAX` / `PROC_CHILD_MAX` + the new `page_count` / `child_count` counters on `struct Proc` + `proc_resource_exempt` decl), `kernel/proc.c` (`proc_resource_exempt` [`principal_id == PRINCIPAL_SYSTEM`] + the `rfork_internal` child-cap early reject + `child_count` ++/-- at `proc_link_child` / `proc_unlink_child` + the `proc_page_charge`/`proc_page_uncharge` helpers), `kernel/syscall.c` (`sys_burrow_attach_for_proc` page-cap check + charge / `sys_burrow_detach_for_proc` uncharge; `sys_thread_spawn_handler` thread-cap check), `kernel/devproc.c` + `kernel/devctl.c` (expose `page_count` / `child_count` in the per-Proc stat surface -- the SEAM counters + the W5-F8 `/ctl/mm/` reconciliation) | RW-12 #65 (HT12.W5-F2 [H2]; BUILD-committed at IDENTITY-DESIGN.md §3.8 / `IDENTITY-DESIGN:769`; D4 pre-rc). **Invariant I-32 (per-Proc resource floor / DoS bound).** A new privilege/DoS surface -- prosecute hard. Prosecute: **cap bypass** (a spawn-variant or a fault path that commits resources without passing its cap check -- rfork_internal is the SINGLE Proc-creation chokepoint so no SYS_SPAWN_* variant escapes; burrow_attach is the SINGLE eager-anon commit point); **exemption forge** (a post-login Proc must NOT reach `principal_id == PRINCIPAL_SYSTEM` -- `CAP_SET_IDENTITY` rejects SYSTEM, proc.h; the boot chain is the only exempt set); **counter race / underflow** (`page_count` written under `vma_lock`, `child_count` + `thread_count` under `g_proc_table_lock`; the uncharge must never underflow past 0; external stat reads are atomic snapshots); **the graceful-OOM backstop preserved** (every user creation path -- `proc_alloc` / `thread_create` / `territory_clone` / `burrow_create_anon` / the demand-page `mmu_install_user_pte` failure -> `proc_fault_terminate` -- returns an error / per-Proc-terminates, NEVER box-extincts; this is the property that bounds a *recursive* cross-Proc bomb at the physical cliff); **the TOCTOU overshoot is bounded** (<= ncpus-1 extra per axis; a floor, not an exact accountant); **the boot chain is not pinched** (the SYSTEM exemption keeps stratumd / joey / corvus / kproc unbounded -- the cap must not break the FS server or the orphan-adopter). Page scope = the repeatable user anon vectors: SYS_BURROW_ATTACH regions + the SYS_LOOM_SETUP ring (audit F1, sys_loom_setup_for_proc charges the ring); pgtable / kstack / exec-image are transitively or one-shot bounded. The global / per-user aggregate quota (cgroup-equivalent reading these counters) is the recorded SEAM. **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in IDENTITY-DESIGN.md §3.8 + `docs/reference/110-resource.md` + this row + the focused audit + the runtime tests (per-axis cap-hit + the unforgeable-exemption + graceful-OOM-no-extinction + the charge/uncharge balance) + the SMP gate. |
| Overcommit memory: lazy-anon demand-zero + decommit + the I-32 VMA-count axis | `kernel/syscall.c` (a NEW `sys_burrow_attach_lazy_for_proc` + handler — the eager `sys_burrow_attach_for_proc` is byte-unchanged; `sys_burrow_decommit_for_proc` + handler), `arch/arm64/fault.c` (the `BURROW_TYPE_ANON_LAZY` demand-zero fault arm: zero-fill + install-once + the **fault-time** `proc_page_charge` with graceful-OOM), `kernel/burrow.c` (`burrow_create_anon_lazy` + the sparse-page free arm mirroring the FILE arm; the decommit page release), `kernel/include/thylacine/burrow.h` (`BURROW_TYPE_ANON_LAZY` + the sparse per-slot page array), `kernel/include/thylacine/proc.h` (`PROC_VMA_MAX` + `vma_count`), `kernel/vma.c` + `kernel/proc.c` (`proc_vma_charge`/`uncharge` at `vma_insert`/`vma_remove`), `arch/arm64/mmu.c` (`mmu_uninstall_user_pte` decommit path), `kernel/include/thylacine/syscall.h` (`SYS_BURROW_ATTACH_LAZY = 83` + `SYS_BURROW_DECOMMIT = 84` + ABI; the eager `SYS_BURROW_ATTACH = 37` is byte-unchanged); **cross-subsystem**: `usr/lib/libthyla-rs` (`sysAlloc` passes `LAZY`) + `usr/lib/pouch/patches/*` (the `mmap` boundary-line passes `LAZY`) + the Go fork `runtime/mem_thylacine.go` (`sysReserve`=LAZY, `sysUnused`=DECOMMIT, stock `heapAddrBits=48`) | The overcommit memory model (§6.5 "The overcommit model"; realizes the §6.1/§6.5 demand-zero-anon BUILD seam; extends **I-32** with the VMA-count fourth axis — no new invariant number). **A new EL0 memory ABI + a fault-path arm + a charge-timing change — prosecute hard.** Prosecute: **W^X / I-12** (the lazy fault installs RW/XN, never executable — `make_user_pte_l3(VMA_PROT_RW)`); the **charge-on-fault correctness + graceful OOM** (an over-`PROC_PAGE_MAX` commit fails the fault → `proc_fault_terminate`, NEVER a box extinction; the charge is under `vma_lock`, no torn count; decommit + re-fault re-charges, so `page_count` tracks true RSS without leak/underflow); the **SMP install-once race** (two CPUs fault one sparse slot → install-once, loser frees — the REVENANT FILE-arm class, but no blocking read so it runs fully under `vma_lock`, no slow path / no pin); **decommit lifetime** (`mmu_uninstall_user_pte` TLBI **before** the page is freed to the buddy — the §"MMU user-PTE clear + TLBI" discipline; no stale-PTE-aliases-freed-page; idempotent on an unfaulted slot; valid only on `BURROW_TYPE_ANON_LAZY`, rejected/no-op on eager); the **VMA-count DoS bound** (`PROC_VMA_MAX` charged at `vma_insert` / uncharged at `vma_remove`; TCB-exempt; the free lazy reservation cannot exhaust the vma slab; the eager path is a no-op); **lock order** (`vma_lock → v->lock` for the sparse array, the established REVENANT order; `proc_page_charge` under `vma_lock`); the **eager path is byte-unchanged** (kernel-internal copy-target callers — exec data, DMA, Weft rings — keep `burrow_create_anon`); free/drain frees only the **present** sparse slots (mirrors the FILE `filepages` cleanup). **No new TLA+ spec** (prose + audit, exactly as the near-identical REVENANT `BURROW_TYPE_FILE` arm — the 2026-05-23 broadening) — prose validation in §6.5 + §28 I-32 + `docs/reference/NN-overcommit.md` (with the impl) + this row + the focused audit + the kernel tests (lazy-fault-zero-fills + charge-on-fault + cap-hit-graceful-OOM + decommit-releases + re-fault-fresh-zero + the SMP install-once race + the VMA-cap-hit) + the **Go stock-config proof** (stock `heapAddrBits=48` boots: a 512-MiB page-summary reserve commits nothing until touched) + default boot + the SMP gate. |
| Namespace name retention -- Spoor.path (#66) | NEW `kernel/include/thylacine/path.h` (`struct Path { int ref; u32 len; char s[]; }` + the API: `path_addelem` / `path_ref` / `path_unref` / `path_make_root`), NEW `kernel/path.c` (the impl), `kernel/include/thylacine/spoor.h` (`struct Path *path` field on `struct Spoor`), `kernel/spoor.c` (`spoor_clone` SHARES the parent Path via `path_ref`; `spoor_free_internal` `path_unref`s; the `spoor_path_extend` / `spoor_path_transplant` helpers), `kernel/stalk.c` (append per successful walk step + `stalk_cross_mounts` transplants the mount-point's Path onto the crossed clone + the base seed rides `spoor_clone`), `kernel/syscall.c` (`sys_walk_open_handler` single-hop `addelem`; `sys_walk_create_handler` created-child `addelem`; NEW `sys_fd2path_handler` + `SYS_FD2PATH = 71`), `kernel/devramfs.c` + `kernel/dev9p.c` (the root-filesystem attach roots seeded `/` via `path_make_root` at Spoor birth -> immutable, no chroot re-stamp), `usr/lib/libt` + `usr/lib/libthyla-rs` (`t_fd2path` wrappers -- libt for the joey C boot prober, libthyla-rs for the native `ns` tool) | #66 (RW-12 HT12.W5-F3 [H2]; the container keystone's introspection half; Q2 user vote 2026-06-12 -- the Plan 9 `Chan.path`, NOT the narrower mount-entry-name REC). **Invariant I-33 (namespace name retention is non-load-bearing).** INVASIVE to the just-RW-4-audited hot walk path -- prosecute the Path LIFETIME hard (a refcount per Spoor is new per-Spoor state; the #57a F2 SMP-lifetime lesson + the multi-thread-per-Proc-shared-state hazard are LIVE). Prosecute: the Path refcount balances on EVERY path (clone-share / walk-replace / cross-transplant / `..`-pop / failure-unwind / `spoor_free`) -- no leak / double-free / UAF; the resolver NEVER reads `->path` (I-33 -- a grep-complete check that no resolution / perm / cross decision consults it); the immutable-string property (`addelem` always allocates a fresh Path, never mutates a shared one) so the only concurrent field is the atomic `ref`; the set-before-publish / read-after discipline on the `c->path` field (no field lock needed, like `qid` / `dev`) is sound under the multi-thread-Proc handle-table sharing (`handle_dup` shares the SAME Spoor -> shares its Path ptr -> the atomic `ref` covers it); OOM / overflow leaves the Path NULL and the WALK STILL SUCCEEDS (a path-alloc failure can never fail a resolution -- the decorative-metadata property); the cross-transplant picks the mount-POINT name not the source's internal name (namespace path correctness); the 3 hook sites are COMPLETE (stalk + single-hop walk-open + walk-create -- a missed site = an incomplete-but-never-wrong name, the I-33 fail-soft). **No new spec** per the 2026-05-23 spec-to-code broadening -- prose validation in ARCH §9.6.9 + I-33 + `docs/reference/30-dev-spoor.md` (the Path section) + `docs/STALK-DESIGN.md` + this row + the focused audit + the runtime tests (accumulate-across-N-hops + cross-transplant + `..`-pop + OOM-leaves-NULL-walk-succeeds + clone-shares-then-COW + the alloc/free counter balance) + the SMP gate. Split #66a (substrate + `SYS_FD2PATH` + tests) / #66b (`/proc/ns` rewrite via a refcounted `PgrpMount.mp_path` + `/proc/fd` + the native `ns` tool + docs). |
| Hardware allowance / I-34 (Menagerie build-arc 2) | NEW `kernel/include/thylacine/allowance.h` (`struct Allowance` { `mmio[ALLOWANCE_MMIO_MAX]` PA windows + `irq[ALLOWANCE_IRQ_MAX]` INTIDs + `dma_max` per-buffer cap + the atomic `revoked` + `lock` } + the API), NEW `kernel/allowance.c` (`allowance_permits` [CreateBegin: lock-free read of the immutable windows + ACQUIRE `revoked`] / `allowance_handle_alloc` [CreateCommit: re-check `revoked` under `allowance->lock` then `handle_alloc`] / `proc_confer_allowance` [the warden's set-once-at-spawn grant] / `proc_revoke_allowance` [DeviceRemoved: set `revoked` RELEASE under the lock] / `allowance_clone_into` [rfork inherit] / `allowance_free`), `kernel/include/thylacine/proc.h` (`struct Allowance *allowance` field [offset 272, size 280; KP_ZERO -> NULL = broad] + the forward decl + the size/offset `_Static_assert`s), `kernel/proc.c` (`allowance_clone_into` in `rfork_internal` after the legate inherit + the rollback; `allowance_free` in `proc_free`), `kernel/syscall.c` (`sys_{mmio,irq,dma}_create_handler` -- the CreateBegin `allowance_permits` gate + the CreateCommit `allowance_handle_alloc` install, replacing the bare `handle_alloc`) | Menagerie build-arc 2 (docs/MENAGERIE.md §4 + §16; ARCH §28 I-34; **spec-first RE-ENABLED** user-voted 2026-06-15). **Invariant I-34 (driver authority bound) -- a new privilege/DoS surface scoping `CAP_HW_CREATE`; SMP-race-bearing; prosecute hard.** The allowance is an OPTIONAL narrowing: `allowance == NULL` = BROAD (the warden + the existing trusted servers, bounded only by the I-5 reservation -- the as-built v1.0 path, unchanged); non-NULL = NARROWED (the conferred set). Prosecute: an **allowance-bypass** on `SYS_*_CREATE` (a create over a resource not in the allowance -- the gate must be COMPLETE across all 3 handlers); the **revoke-vs-create SMP race** (the central hazard -- an in-flight create racing a `DeviceRemoved` revocation; closed by the two-step CreateBegin/CreateCommit with the `revoked` re-check under `allowance->lock`, which `proc_revoke_allowance` also takes; the buggy no-recheck variant is the spec's `BUGGY_COMMIT_NO_RECHECK`); the **intersection-superset** (the warden confers a superset of the node -- the warden's policy, prose-audited; the kernel copies what it is given); the **fully-revoked teardown** (revoke + `proc_group_terminate` drops the handles); the **never-widened** property (windows immutable post-confer; a forked child inherits an equally-narrow copy via `allowance_clone_into` -- the I-2 hardware-axis monotonicity; a broad parent's child stays NULL); the **lock order** (`allowance->lock` -> handle-table lock, both spinlock-only/non-sleeping, acyclic); the **lifetime** (freed at `proc_free` + on rfork rollback; no leak/UAF on any path); the **backward-compat** (the broad path must not regress the existing virtio drivers -- boot OK is the gate). The warden's confer-at-spawn syscall wiring is the build-arc step-5 consumer (the warden + `libdriver`); this chunk lands the kernel mechanism + the kernel-internal confer/revoke API + the 3 gate checks + tests. **Model-first**: `specs/allowance.tla` (clean cfg TLC-green + the 4 buggy cfgs `revoke_race` / `revoke_leak` / `confer_widen` / `self_widen`) is the pre-commit gate; prose validation in MENAGERIE.md §4 + ARCH §28 I-34 + `docs/reference/117-allowance.md` + this row + the focused audit + the `allowance.*` kernel tests (incl. `handle_alloc_revoked_aborts` = the race regression) + boot OK + the SMP gate. **Build-arc step 6a (#159) adds the FOURTH axis -- the per-`(bus,dev,fn)` PCI allowance** (`allowance.h` `pci[ALLOWANCE_PCI_MAX]` + `PCI_BDF_PACK` + `HW_RES_PCI`): `sys_pci_claim_handler` resolves `virtio_device_id -> (bus,dev,fn)` read-only (`kobj_pci_resolve_bdf`, the same first match the claim picks) then runs the SAME two-step gate (CreateBegin `allowance_permits(HW_RES_PCI)` -> `kobj_pci_claim` -> CreateCommit `allowance_handle_alloc` re-checking `revoked`), replacing the I-34-round fail-closed `allowance_is_narrowed` reject. The ABI `t_allowance_desc` grows 176->216 (appended `pci_count` + `pci[8]` + `_pad_pci`, every prior offset pinned). NO spec change (the abstract model covers a 4th opaque-resource kind; the 4 buggy cfgs re-run green); prosecute the same way + the new resolve-vs-claim consistency + the PCI_BDF_PACK injectivity + the ABI-append offset preservation. Test `allowance.pci_membership` (the per-bdf gate + the live rng-pci resolve leg). v1.x seams (documented in 117-allowance.md): the cumulative DMA-pool budget (composes with #65); the forked-child scope-teardown (composes with the I-25 legate scope); claim-by-specific-bdf for a multi-function device of one `virtio_device_id` (the 6a resolve picks the first match). |
| devpci: mediated PCI topology + the devhw synth child (Menagerie 6b-1) | NEW `kernel/devpci.c` (`dc='P'`, name="pci"; the devdev/devctl read-only dir-Dev: reuse-`nc` walk + qid-dispatch; root -> `<bus.dev.fn>` dir -> `ctl` file serving `g_virtio_pci_devs[]` topology), `kernel/devhw.c` (the synthetic `pci` mount-point child -- `HW_SYNTH_PCI` bit-62 qid special-cased in `walk_one`/`stat_native`/`readdir`), `kernel/joey.c` (`joey_mount_static_dev(&devpci, "hw/pci", 6)` AFTER the /hw mount), `kernel/dev.c` + `kernel/include/thylacine/dev.h` (register + extern), `kernel/include/thylacine/territory.h` (`PGRP_MAX_MOUNTS` 12->16 -- the new boot mount + the #80 orphan accumulation) | Menagerie 6b-1 (docs/MENAGERIE.md §7/§16-6b; ARCH §9.4 as-built; scripture `f0878ba`). **A new EL0-facing Dev exposing hardware topology -- audit-light; prosecute the I-5 boundary.** Realizes the mediated PCIe discovery source; NO new invariant (composes I-5 "userspace never gets raw ECAM" + I-15 hardware-view-from-kernel-enumeration). Prosecute: NO raw-ECAM / config-space-WRITE surface reachable (write/create/wstat/remove all -1; the only config-space READ is the INTx pin, kernel-side, and only the derived INTID crosses to userspace); the ctl-line builder leak-free + bounded (the `pf_*` 0-on-overflow degrade to a short-but-valid line; a stale ctl qid -> -1); the walk/read/readdir bounds (the qid index validated vs `virtio_pci_dev_count`; the readdir cookie strictly-increasing + never-0); the reuse-`nc` walk contract (the mount-cross prerequisite); the devhw synth-child soundness (the `HW_SYNTH_BIT` sentinel is NEVER decoded as an FDT offset; the surgery does not regress the audited devhw walk/readdir); SMP-safe by construction (read-only over the immutable boot-built `g_virtio_pci_devs[]`; no new lock / shared-mutable-state); the `PGRP_MAX_MOUNTS` bump (the static_assert auto-validates; the rfork-clone per-entry cost stays modest at 16). The PciSource consumer + the `netdev-pci-driver` bind (the live I-34-on-PCI proof) are 6b-2/6b-3; the focused audit + SMP gate are 6b-4. **No new spec** -- prose validation in `docs/reference/120-devpci.md` + ARCH §9.4 + this row + the `devpci.*` + `devhw.synth_pci_child` tests + boot OK (930/930 + /hw/pci mounted + 0 EXTINCTION) + the 6b-4 SMP gate. |
| netd: the network daemon -- smoltcp embedding + NIC ownership (NET-DESIGN.md the #68 charter) | NEW `usr/netd` (the warden-bound `virtio-pci:1` driver: `impl libdriver::Driver`; the smoltcp `phy::Device` `NicDevice`/`NicRxToken`/`NicTxToken` over `netdev::VirtioNetPci`; the net-2a DHCP-lease serve), `usr/netd/Cargo.toml` (the smoltcp 0.12 dep, `no_std`+`alloc`, minimal features), `usr/warden/src/main.rs` (the `netd` manifest, replacing the 6b-3 `netdev-pci-driver` ARP demo) | net-2a (NET-DESIGN.md §2/§13/§14/§17; ARCH §10.1). **The NIC-owning stack Proc -- the network arc's central audit-bearing surface (NET-DESIGN.md §15.2).** NO new section-28 invariant (the network composes I-1/I-5/I-9/I-10/I-11/I-23/I-28 per §15.1). At net-2a the landed surface is the **smoltcp embedding** + the **`phy::Device`** + the **NIC ownership** (I-5: the `KObj_PCI`/`IRQ`/`DMA` handles are non-transferable, so the claimer IS the stack -- a driver cannot leak its device); the virtio DMA/ring/IRQ memory-safety is inherited from the audited `VirtioNetPci` (net-1/pci-3). Prosecute (the focused audit is **net-2d**): the phy-token no-alias (`NicRxToken` OWNS its bytes, `NicTxToken` holds the single `&mut nic`); the TX `consume` length bound (`len.min(MAX_FRAME)`) + the back-pressure-drop soundness; the DHCP poll loop is self-bounding (no hang); the grant fail-closed (`probe` rejects a non-`virtio-pci:1` bind before touching hardware). **net-2b-2 LANDED** the 9P-server SKELETON (NEW `usr/netd/src/server.rs`: the static read-only `/net` tree `tcp/udp/icmp`+`stats`; `server::Conn` fid table; Tversion/attach/walk/lopen/read/getattr/clunk via `libthyla_rs::ninep`; the load-bearing `Tgetattr` security trio `0555`/`0444`-SYSTEM the A-3 dev9p X-search reads) + the combined `t_poll([listener]+conns, timeout=poll_delay)` accept/stack loop + the joey `/net` mount (open=connect `/srv/net` -> dev9p root -> `t_mount`, fail-soft). **NEW privilege surface = the MAY_POST_SERVICE conferral** (joey grants the warden `T_SPAWN_PERM_MAY_POST_SERVICE`; the warden re-confers it gated on `lifecycle == Persistent`; netd `devsrv_post_listener`s `/srv/net` -- the #827b one-hop delegation joey->warden->netd, no new ABI): **net-2d prosecutes** that a TRANSIENT driver can NEVER post (the gate is exactly the persistent service), the warden-holds-not-console-attached confer path is sound, and the one-hop adds no ambient authority. **net-2c-1 LANDED** the §3.4 fid state machine (`usr/netd/src/server.rs` reworked: the static `NODES` -> a qid-encoded dynamic `/net/tcp` tree -- a live connection `N` = `CONN_FLAG(1<<40)|proto<<32|N<<8|filekind`, resolvable ONLY while its slot is live; the clone-mints-`N` Plan 9 idiom rebinding the opened fid onto `ctl` -- the kernel dev9p client accepts the differing `Rlopen` qid, verified `9p_session.c`; the **refcounted** connection slots -- `fid_set` refs-new-before-unref-old, `fid_clunk`/`teardown`/Tversion unref, the *last* unref frees `N` [the ONLY free path, I-10/I-11], `MAX_SLOTS=16` the #65 DoS floor; a slot carries NO smoltcp socket yet ["N assigned"; net-2c-2]) + the `libthyla_rs::ninep` `Treaddir` codec (`P9_TREADDIR/RREADDIR` + `parse_treaddir`/`build_rreaddir`/`pack_dirent` + `DT_DIR/DT_REG`; the kernel dev9p readdir issues `Treaddir`, NOT a legacy `Tread` stream). **net-2d prosecutes** the fid-machine refcount (no slot leak/UAF/double-free; the only-free-path; N-reuse-only-after-clunk; the ref-before-unref no-transient-free; the partial-walk/clone-rollback/Tversion-reset/teardown paths take no stray ref), the `Treaddir` bounds (the cookie strictly-increasing + never-0; the budget cap vs `count`/msize; the scratch bound), the clone-rebind soundness, and the connection-table SMP-safety (netd is single-threaded -> the global `Net` needs no lock; verify no `thread_spawn`). **net-2c-2 LANDED** the live TCP data path: the `socket-tcp` feature; the `Net` table now OWNS the smoltcp `Interface`+`SocketSet` (moved in post-DHCP, reached as disjoint fields by the 9P dispatch; `device` stays a serve()-local `Net::poll(&mut)`); `clone` reserves a real `tcp::Socket` (rx/tx 4 KiB; freed at the last clunk via `SocketSet::remove` -- the sole free path, so the socket lifetime == the connection's); the `ctl` verb parser (`connect a.b.c.d!port` -> `socket.connect(iface.context(), remote, local)` with a rotating ephemeral local port [smoltcp requires a non-zero local port -- it auto-selects only the ADDRESS]; `hangup` -> `close()`; `announce`/`bind`/options -> honest `EOPNOTSUPP`, net-3+); the connection files (`status` = live `socket.state()`, `local`/`remote` = the endpoints recorded synchronously at connect, `err` = the recorded reason, `data` = `recv_slice`/`send_slice`, NON-BLOCKING -- a 0-read is ambiguous between no-data-yet and EOF, deferred to the dev9p.poll/net-6 leg). The NIC-IRQ poll fd is DEFERRED (a pollable IRQ fd is a kernel ABI surface -- `SYS_IRQ_WAIT` blocks; the `poll_delay`-clamped timeout poll is correct, <=50ms under load, with a post-dispatch `net.poll` TX-flush). **net-2d additionally prosecutes**: the socket reservation/free balance (added at clone, removed at the last unref -- no leak/double-remove/UAF; the `SocketHandle` lifetime == the refcount); the disjoint-field borrow soundness (`net.sockets.get_mut(h)` + `net.iface.context()` are separate fields of `Net`); the `ctl`-verb parser bounds (the dial-string octet/port range, no OOB, the `!r`/local suffix ignored; an unknown verb -> `EOPNOTSUPP` not silent-accept); `connect`/`data` on a freed/non-live/never-connected slot fails closed (returns 0 / Err, never a stale socket's bytes -- the I-10/I-11 "a half-open `data` read after CLOSED returns the err, never a reused socket"); the ephemeral-port rotation (a wrap-collision is v1.x-bounded at MAX_SLOTS=16). These JOIN this row. **net-2d CLOSED CLEAN (Opus-4.8-max prosecutor + concurrent self-audit; 0 P0 / 0 P1 / 1 P2 / 4 P3, NOT dirty)**: F1[P2] `h_readdir`'s budget omitted the 11-byte Rreaddir frame overhead (`P9_HDR_LEN+4`) that `h_read` reserves -> a populated dir read by a small-msize client overruns its negotiated msize -> FIXED via a `rreaddir_budget` parity helper; F2[P3] `h_attach`/`h_walk` accepted the `P9_NOFID` sentinel as a live fid -> FIXED fail-closed reject; F3[P3] a rejected re-`connect` burned an ephemeral port + a rolled-back clone over-counted `opened` -> FIXED peek-then-commit port + `tcp_clone_rollback` uncount; F4[P3] cross-call Treaddir coherency + F5[P3] ephemeral-port liveness + the self-audit SF4 cross-session liveness CLOSED-justified (by-design/documented; single-threaded + the qid re-validates `slot_live` so no stale resolution). The deterministic small-msize/NOFID/failed-connect regressions are architecturally unreachable in-VM (the ONLY /net client is the trusted large-msize kernel dev9p mount; /net is 9P-mode; netd has no host-test harness) -> OWED to a netd pure-protocol host-test module (net-3+, the netdev `cfg_attr(not(test),no_std)` pattern); the fixes rest on data-path parity (`h_read`) + the ninep `build_rreaddir` length guard + fail-closed correctness (`memory/audit_net2d_closed_list.md`). **net-3a LANDED** the TCP server side: `announce` (ctl verb -> `socket.listen`; the connection enters ANNOUNCED, `status` reads `Listen`; `listen_ep` recorded for the re-arm) + the blocking `listen`/accept via a **DEFERRED 9P REPLY** (the load-bearing mechanism: a single-threaded server CANNOT block in `h_lopen` -- it must keep polling the NIC to receive the SYN that unblocks it = self-deadlock -- so it registers a `PendingAccept{conn_id,tag,fid,listening_n}` + returns the `Disp::Deferred` sentinel [NO reply emitted; the client's open() stays blocked on the outstanding tag -- the dev9p client matches by tag, NO per-op deadline, #811-death-interruptible, verified `kernel/9p_client.c:296/400/454`]; the serve loop's `poll_accepts` detects the established listener [`accept_ready` = `is_active() && state != SynReceived`] + SWAPS [`accept_swap` mints `M` taking the established `SocketHandle` + re-arms `N` with a fresh listener -- `N` stays ANNOUNCED] + `complete_accept` rebinds the listen fid onto `M/ctl` [the refcount moves `N->M`; `N` stays alive via the announce fid] + sends the held `Rlopen`). NEW shared-crate surface = the `ninep` Tflush/Rflush codec (`P9_TFLUSH=108`/`P9_RFLUSH=109` + `parse_tflush`/`build_rflush`): `h_flush` cancels the pending accept under `oldtag` + replies `Rflush` -- the cancellation path a long-held listen needs (a client dying on its blocked open sends Tflush), ALSO closing a PRE-EXISTING net-2c-2 latent (an ignored Tflush left the kernel tag `awaiting_flush` -> a leak; narrow in net-2c-2, wide with a long-held listen). The COMMITTED-BLOCKING realization of §3.4; NO kernel surface (§12's readiness multiplexing via dev9p.poll/Loom is the SEPARATE net-6 leg, `net_poll.tla`). **net-3d prosecutes**: the deferred-reply lifetime (no leak/UAF if the Conn dies mid-accept -- teardown + Tversion-reset cancel pending; no double-reply; the no-reply sentinel never writes a stray frame; the tag echoed correctly), the socket-swap refcount balance (`M` ends refs=1 owned by the listen fid; `N` stays via the announce fid; the orphan-mint free path `free_orphan_mint`/`discard_accept`; no double-`SocketSet::remove`), the Tflush handler (cancels exactly the right pending accept; `Rflush` frees the kernel `awaiting_flush`; no cancel of a live op), the announce/listen gate (a non-announced `open(listen)` -> `E_INVAL` IMMEDIATELY, never deferred -> no hang; bounded `MAX_PENDING_ACCEPTS=16`), the backlog-of-1 (a 2nd SYN pre-swap -> RST, documented). Pure userspace -- kernel byte-unchanged; the full inbound-accept E2E is OWED to net-3d (a deterministic in-guest inbound path -- a netd loopback interface). These JOIN this row. **net-3b LANDED** the UDP datagram surface: the shared `Slot` gains a `proto` field (`PROTO_TCP`/`PROTO_UDP`) -- the discriminator for the type-recovering `sockets.get::<tcp::Socket>` vs `get::<udp::Socket>` (a mismatch PANICS in smoltcp, so EVERY socket touch is dispatched on `slot_proto(n)` -- memory-safety, NOT cosmetics); `/net/udp/clone` mints a `udp::Socket` (a `PacketBuffer` of whole datagrams + per-packet sender metadata, unlike TCP's byte stream); `ctl_connect` dispatches (UDP `bind`s a local ephemeral port + records the remote -- datagram setup, not a handshake); `data` = `send_slice(data, remote)`/`recv_slice -> (n, meta)`; `status` = `Open`/`Closed`; NO `listen` file on a UDP conn dir (`walk_child` rejects it -- datagrams have no accept); the shared slot pool means `walk_child`/`for_each_child` FILTER each protocol dir's numeric children to the matching `proto`. The `socket-udp` Cargo feature is the only new dep. **net-3d prosecutes**: the proto-dispatch COMPLETENESS (every `get::<T>` guarded -- no tcp-on-udp / udp-on-tcp panic; `accept_swap`/`poll_accepts` are tcp-only by construction since only tcp announces + registers accepts); the cross-proto refcount/free balance (`clone_rollback`/`slot_unref`/`free_orphan_mint` read the slot `proto` BEFORE freeing -> the right `*_active`/`*_opened` counter; the shared slot pool -- a tcp `N` + a udp `N` cannot coexist); the netd-internal DNS round-trip demo lifetime (the probe mints+frees its own slot via `free_orphan_mint`, no leak across it -- it is BEST-EFFORT + LOGGED + NEVER a boot gate, since slirp FORWARDS DNS to the host resolver so a response is environment-dependent). The deterministic UDP-via-9P data round-trip E2E is OWED to net-3d (the loopback iface, alongside the TCP accept). **net-3c LANDED** the ICMP ping surface: `PROTO_ICMP` joins the slot discriminator; `/net/icmp/clone` mints an `icmp::Socket` bound to a rotated Echo identifier (smoltcp routes EchoReplies back BY IDENT); `ctl_connect` dispatches to a PORTLESS `icmp_connect` (records the target only; the ctl `connect` verb accepts a bare IPv4 for an ICMP slot); `data` write wraps the payload into an `Icmpv4Repr::EchoRequest` (smoltcp's own encoder; the iface recomputes the checksum on egress), `data` read parses the queued packet (already filtered to the bound ident) + returns the `EchoReply` payload; `remote`/`local` are a bare dotted-quad (`Content::push_ip`, no `!port`); NO `listen` file; `hangup` is a no-op (connectionless -- the clunk frees the slot + removes the socket). The `socket-icmp` Cargo feature is the only new dep; ONE benign kernel constant bump (`JOEY_BLOB_MAX` 256->384 KiB -- the accumulating net boot probes outgrew the init-blob bound, surfaced as a `joey: /joey ELF exceeds JOEY_BLOB_MAX` extinction, root-caused to ground + fixed not waved off per [[feedback-no-host-load]]). **net-3d prosecutes**: the proto-dispatch COMPLETENESS extends to ICMP (every `get::<icmp::Socket>` reached ONLY via a `slot_proto == Some(PROTO_ICMP)` dispatch -- self-audit-verified across all 24 `get::<T>` sites, no unguarded type-recovery panic); the cross-proto refcount/free balance over the 3-proto shared pool (`dec_active`/`clone_rollback`/`free_orphan_mint` read `proto` BEFORE freeing -> the right `icmp_*` counter); the `icmp_clone` bind-BEFORE-add (a bind failure returns None leaking no socket); the netd-internal ping demo lifetime (mints+frees its own slot via `free_orphan_mint`). The best-effort gateway-ping demo is LOGGED, NEVER a boot gate -- whether slirp answers a guest echo INTERNALLY (vs proxying it to a host ping socket the host may not permit) is host-dependent (on the dev host it did NOT answer, VINDICATING the best-effort framing per [[feedback-no-host-load]] -- an asserted round-trip would have flaked); the deterministic in-guest ICMP round-trip E2E is OWED to net-3d (the loopback iface auto-replies to an echo to its own IP). **No new spec** (the I-9 `dev9p.poll` readiness leg reserves `net_poll.tla` at net-6) -- prose validation in `docs/reference/121-netd.md` + this row + the net-2d audit + boot OK (the DHCP-lease proof + the /net mount + the net-2c-2 `connect`/endpoint-readback/clunk-reuse probe + the net-3a `announce *!7777 -> Listen` + listen-file/readdir + the not-announced-gate probe + the net-3b udp clone/connect/remote-readback/no-listen probe + the best-effort DNS round-trip demo + the net-3c icmp clone/portless-connect/remote-readback/no-listen probe + the best-effort gateway-ping demo + the net-3d loopback E2E PASS) + the SMP gate. **net-3d CLOSED (Opus-4.8-max prosecutor R1 + concurrent self-audit, 0 P0 / 1 P1 / 1 P2 / 2 P3; DIRTY-CLOSE -> round-2 on the fix CLEAN 0/0/0/2 -- CONVERGED over 2 rounds)**: F1[P1] a half-open deferred-listen fid (h_lopen FK_LISTEN set defer+Ok(0) but NEVER opened=true) with a committed PendingAccept -- a native /net client CLUNKs it (fid_clunk does NOT gate opened) WITHOUT removing the pending; the slot frees + `clone` re-mints the index CROSS-PROTO -> the generation-less PendingAccept resolves into a wrong-proto `get::<tcp::Socket>` PANIC (SocketSet::get `.expect()`s on a type mismatch + add REUSES the freed index, both verified in smoltcp 0.12) -> netd (sole NIC owner, I-5) aborts -> a WHOLE-NETWORK DoS (+ walk-from/double-defer/hijack facets, same root). Latent in-VM (the trusted kernel dev9p client abandons ONLY via Tflush, handled) but reachable from the open=connect native client -> soundness-bar P1. FIXED by FOUR layers: a per-slot monotonic mint `gen` (Slot.gen + Net.mint_seq/next_gen, stamped at all 4 mints; the listener N KEEPS its gen on re-arm -- REQUIRED for the two-concurrent-pendings case) + PendingAccept.listening_gen + the poll_accepts proto+gen GUARD (drop unless the slot is still the same live TCP slot -- the panic + any strand become a harmless drop, locally sound) + `cancel_accept_fid` in fid_clunk (clunk cancels the pending) + the FK_LISTEN branch sets the fid opened=true (blocks walk-from + double-defer; complete_accept's fid_set ignores opened so the legit completion works). F2[P2] poll_accepts gated on liveness-not-proto -> FIXED by the same guard. F3/F4 + SA-2/SA-3 -> DOC caveats. The **loopback E2E** (server::loopback_e2e -- an ISOLATED 127.0.0.1/8 stack [a smoltcp Loopback device + Interface + its OWN SocketSet] driving the REAL Net methods; Net::poll made generic) delivers the THREE owed deterministic in-guest round-trips: TCP inbound-accept via the real poll_accepts/accept_swap (= the F1 fix's RUNTIME regression), UDP datagram, ICMP echo. The isolation is LOAD-BEARING -- a shared-NIC-SocketSet lo iface mis-routes (the NIC default route steals the 127.0.0.1 egress; tcp::Socket::dispatch has no iface-address-ownership gate -> route() falls to the default route, verified in the smoltcp source). Boot proof: `net-3d loopback E2E PASS` + 930/930 + boot OK + 0 EXT + the 3 joey probes UNREGRESSED; SMP gate PASS (default+UBSan x smp4/smp8 N=10, 0 corruption; every timing boot ground-truthed guest-clean at login per [[feedback-no-host-load]]). Pure userspace -- kernel byte-unchanged. (`memory/audit_net3_closed_list.md`.) **net-3 ARC COMPLETE.** **net-4a LANDED** `/net/cs` (the connection server) + the compiled-in ndb (NEW `usr/netd/src/ndb.rs` the ndb(6)-subset parser over `include_bytes!("../ndb/local")`; NEW `usr/netd/ndb/local` baked byte-identical to `/lib/ndb/local`; `usr/netd/src/server.rs` the `P_CS` static node + `cs_resolve` + the per-fid `CsSession` [the write = the dial query, the read drains via a per-fid cursor, dropped on clunk/teardown/Tversion]; `usr/joey/joey.c` the cs + `/lib/ndb/local`-reachability probe; `tools/build.sh` the `/lib/ndb/local` bake). cs resolves `proto!host!service` -> `<clone-file> <ip>!<port>` (numeric-then-ndb host + service, `net`->tcp, unresolvable/malformed -> empty). **The net-4 ndb-source DECISION (user-voted 2026-06-18; NET-DESIGN §5/§18 refined, scripture-first `dbc6523`)**: netd, a confined warden-bound leaf driver (I-34), CANNOT read `/lib` (devramfs Territory; widening fights I-28/I-34) -> compiled-in ndb (the capability-uK config-at-construction idiom) + the static `localhost`/services half + the DHCP-learned dynamic half read LIVE + the baked user-readable `/lib/ndb/local` (the v1.x cs/dns-daemon-split's live source). **net-4d prosecutes**: the cs dial-string parser bounds (>3-field reject; malformed -> empty); the `CsSession` lifetime (no leak/UAF; dropped on every clunk/teardown/Tversion; bounded by MAX_FIDS; an opened cs fid cannot be walked-from so no stale-session rebind); the ndb(6) parser bounds (malformed entry -> unresolved, no crash/OOB, no heap); the FILE_RW perm gate. **No new spec / no new invariant** -- prose in `docs/reference/121-netd.md` (the net-4a section) + this row + boot OK (`net-4a PROBE OK (cs ... -> /net/tcp/clone 127.0.0.1!80; unresolved -> empty; /lib/ndb/local readable)` + 930/930 + 0 EXT) + the SMP gate (0 corruption across 60 boots; every timing boot ground-truthed guest-clean at login per [[feedback-no-host-load]] -- the count fluctuated 11->8 on a re-gate, the harness post-marker exit artifact on the slow UBSan config). Pure userspace -- kernel byte-unchanged. The focused net-4 audit is **net-4d**. **net-4b LANDED** `/net/dns` (the resolver) + cs->dns delegation (a shared `dns::Socket` seeded from the DHCP resolver -- `Net::new(.., dns_servers)`, absent if the lease carried none -> queries fail closed fast; the net-4a `CsSession` generalizes into the deferred-capable per-fid `server::Query{fid,kind,query,resp,cursor,deferred}` + `query_begin`/`query_read`/`poll_dns`/`cancel_dns_flush`; the Net-level `dns_query_start`/`dns_poll`/`dns_cancel`; `Cargo.toml` the `socket-dns` feature). A name resolves numeric->ndb->DNS: numeric/ndb fill synchronously, a DNS name starts a query and the read DEFERS (the net-3a held-`Rread` mechanism -- `poll_dns` is the serve-loop analog of `poll_accepts`). cs->dns is automatic (the unresolved-host branch with `kind=Cs`). **The query-lifetime hazard**: smoltcp `get_query_result` FREES the slot on a result + PANICS on a free slot -> the handle lives in ONE place (`Query.query`) nulled the instant `dns_poll` returns a result, never double-polled (a netd panic = a whole-network DoS, the net-3d F1 class); `dns_cancel` runs only on a still-occupied slot. **net-4d prosecutes**: the query-lifetime single-completion (no double-poll-panic across `query_read` + `poll_dns`); the deferred-read lifetime (no leak/UAF on Conn death; `query_clear_all`/`cancel_dns_flush`/`query_drop` each cancel the in-flight query; no slot leak; the held `Rread` echoed once); the dns/cs parser bounds; the no-resolver fail-closed (empty, no 10s hang); the cs->dns format correctness. v1.0 is IPv4-A-record only. **No new spec / no new invariant** -- prose in `docs/reference/121-netd.md` (the net-4b section) + this row + boot OK (`net-4b PROBE OK (dns 10.0.2.2 -> 10.0.2.2; localhost ip -> 127.0.0.1; aaaa -> empty)` + the best-effort `net-4b DNS live query OK` [logged] + 930/930 + 0 EXT) + the SMP gate. The deterministic in-guest E2E of the 9P deferred-read plumbing is OWED to net-4d (a loopback DNS responder, the net-3d analog). **net-4c LANDED** `/net/ipifc/0` (the interface-config tree: `ctl`/`status`/`local`) + `/net/ndb` (the live dynamic network database, ndb(6) format) + the native `ipconfig` tool (NET-DESIGN §6; PURE userspace, kernel byte-unchanged, no new dep). NEW `server::IfConfig { mac,mtu,addr,prefix,gw,dns,up,dynamic }` on `Net`: the DHCP lease folds into it at bring-up (the "DHCP-lease-into-ipifc" + the "ndb dynamic half", surfaced READ-ONLY through status/local/ndb) and `Net::new` seeds the net-4b resolver socket from `ifc.dns` (ONE source of truth for cs->dns + the ndb `dns=` line). NEW `ipifc_ctl` (`/net/ipifc/0/ctl`): `add <ip> <mask> [gw]` (`ifc_set_static`; `parse_mask` = dotted-quad [contiguity-checked] / `/N` / bare-prefix) applies static config onto BOTH the live smoltcp iface AND the snapshot; `remove`/`unbind` (`ifc_clear`); `bind ether` a v1.x seam (rejected honestly). NEW `usr/coreutils/src/bin/ipconfig.rs` + baked. NEW `server::ipifc_e2e` (a throwaway-`Net` selftest: add/remove/status/ndb/local + malformed-rejects, deterministic, off the live config). **net-4d prosecutes**: the `ipifc_ctl` parser bounds (every malformed verb/octet/mask -> EINVAL, no mutation); the mask contiguity + prefix<=32; the renderers bound (Content[256], no truncation); the iface<->snapshot coherence (both mutated; a reader never sees an address the iface is not using; a gateway-less re-add leaves no stale route; `up==false` gates every renderer); the single-threaded no-SMP property (the `ifc` is mutated only in the serve loop); the 9P ctl-write path (h_write -> ipifc_ctl -> Rwrite/Rlerror, like the proven cs/dns write); the perm posture (ctl 0666 SYSTEM). v1.x seams: the DHCP-renewal re-application (snapshotted at bring-up; untestable with slirp's stable lease), the DHCP-vs-static arbitration, multi-interface + `bind ether`. **No new spec / no new invariant** -- prose in `docs/reference/121-netd.md` (the net-4c section) + NET-DESIGN §6 + this row + boot OK (`net-4c ipifc E2E PASS` + `net-4c PROBE OK (...)` + 930/930 + 0 EXT) + the SMP gate. **net-4d CLOSED (Opus-4.8-max prosecutor + concurrent self-audit, CONVERGED on the deferred-overwrite root; 0 P0 / 0 P1 / 1 P2 / 3 P3, NOT dirty; + a precautionary focused round-2 on the F1 deferred-reply fix, CLEAN)**: **F1[P2]** a held deferred cs/dns read tag could be LOST (I-9) -- `Query.deferred` is a single `(tag,cap)` slot, so a SECOND concurrent read on one fid (legal for a multi-thread Proc -- the kernel dev9p client multiplexes by TAG) OVERWROTE it, and a RE-WRITE while a read is deferred (`query_begin`->`query_drop`) dropped it; the first held `Rread` was never delivered (no crash, self-inflicted, death-recoverable -> P2 not P1). FIXED with two MINIMAL guards (NOT a wait/wake restructure): `query_read` returns an empty `Rread` for a 2nd read when one is already deferred (the first keeps its real answer); `h_write` rejects (`E_PROTO`) a re-write while `fid_has_deferred(fid)` (fid-specific; bounded by the ~10s DNS timeout, never a permanent wedge). The bare-clunk-while-deferred facet is mitigated on the trusted mount (Tflush->`cancel_dns_flush`) + a client protocol violation otherwise (per the net-3 listen-clunk close). **F2[P3]** `Content` buf 128->256 (the ~120 B status render was safe -- `push` min-clamps, no OOB -- but the net-4c "256" claim was wrong). **F3[P3]** documented the shared dns socket's `queries` Vec high-water (bounded, REUSED -- not a leak). **SA-3[P3]** clarified the DHCP-renewal comment (iface + ifc both pinned at bring-up -> coherent). The single-completion `get_query_result` discipline (verified against the REAL smoltcp 0.12 source -- panics on a freed slot -> a whole-network DoS), the no-query-slot-leak, the parser fail-closed bounds, the iface<->snapshot coherence, the single-threadedness, and isolation (I-1) all HELD. Three NEW deterministic in-guest proofs (the `loopback_e2e`/`ipifc_e2e` pattern): `proto_selftest` (the cs/dns/ndb/mask parser battery -- the OWED-since-net-2d host-test coverage delivered IN-GUEST, since netd is a no_std+aarch64 BIN crate that can't `cargo test` without a feature-gated refactor that would risk the green device build; carried as a NAMED seam), `dns_defer_guard_selftest` (the F1 regression -- fails pre-fix by construction), `dns_loopback_e2e` (the OWED net-4b deferred-read E2E -- a mock DNS responder bound to 127.0.0.1:53 on an ISOLATED stack answers a fixed A query, so the shared `dns::Socket` resolves a name in-guest via the real `dns_query_start`->poll->`dns_poll` methods; the net-3d loopback analog). **THE net-4 ARC IS COMPLETE.** prose in `docs/reference/121-netd.md` (the net-4d section) + NET-DESIGN §20 + this row + boot OK (`net-4d proto selftest PASS` + `net-4d dns defer-guard PASS` + `net-4d dns loopback E2E PASS` + 930/930 + 0 EXT) + the SMP gate (40/40 PASS, 0 corruption, 0 timing). `memory/audit_net4_closed_list.md`. |
| dev9p.poll readiness bridge + the 9P-client async reply-dispatch (net-6b-2b) | NEW `kernel/dev9p_poll.c` (the `dev9p_poll` Dev `.poll` slot + the global poll-pump kthread + the async readiness-op lifecycle + `dev9p_poll_complete` + the registry/borrow-guard/GC), `kernel/dev9p.c` (`.poll = dev9p_poll`; the `qid_type_p9_to_kernel` QTPOLL mapping; `dev9p_priv_of`; the `dev9p_poll_priv_release` close hook), `kernel/include/thylacine/dev9p.h` (`dev9p_priv.poll` + the API), `kernel/include/thylacine/9p_wire.h` + `kernel/include/thylacine/spoor.h` (the `P9_QTPOLL`/`QTPOLL` = 0x01 bit), `kernel/poll.c` (`poll_waiter_list_empty`), `kernel/main.c` (`dev9p_poll_init` + the boot kthread spawn), `usr/netd/src/server.rs::qid_of` + `usr/lib/libthyla-rs/src/ninep.rs` (netd marks the `ready` file `P9_QTPOLL`) | net-6b-2b (NET-DESIGN §12.2; scripture `eeab08e`; spec-first `specs/net_poll.tla`). **The one new kernel surface in the net arc, on the most-audited mechanism (the #841 9P elected reader) -- I-9 generalized to the elicited-readiness relay. Prosecute hard.** NO new §28 invariant (composes I-9). Prosecute: the **I-9 register-then-observe** (PollerRegister: the hook is installed + a non-terminal probe is outstanding BEFORE the not-ready sample is returned, so no readiness edge between the sample and the park is lost -- `net_poll.tla` NoMissedNetPoll); the **borrow-guard UAF** (the kthread borrows the netd client from a live op's `spoor_ref` pin, NEVER owning the client lifetime -- the Loom-4 F1 class); the **lock order** (`g_dev9p_poll_lock -> c->lock` at submit; `dev9p_poll_complete` is `c->lock` -> atomics-only -> no cycle; the kthread takes `c->lock` (pump) + the poll-list lock (GC) SEPARATELY; the GC empty-check is nested under g_lock for atomicity with the unlink vs a concurrent reuse that registers its hook before reusing the op); the **on_complete seam contract** (under `c->lock`: no sleep, no poll-state lock, no `p9_client_*` re-entry, atomics only); the **QTPOLL gate soundness** (a regular dev9p file -- Stratum/corvus -- is POSIX always-ready, NEVER probed; FAIL-SAFE: an unmarked file / a netd->dev9p plumbing slip degrades to always-ready, never an unsound probe; a native `SYS_POLL` on a regular dev9p file is reachable, so the gate is load-bearing); the **mask-union widening** (a broader poller abandons + resubmits the union via #845 Tflush so a `poll(POLLOUT)` cannot hang behind a concurrent `poll(POLLIN)`); the **stranded-op GC** (a timed-out poll on a never-ready socket abandons the op when its poll-list empties, else the pin leaks the Spoor); the **deadline-capability guard** (the kthread's frame-boundary recv deadline REQUIRES a deadline-capable transport -- netd's srvconn `/net` mount is; a non-deadline-capable QTPOLL server degrades to always-ready, fail-safe); the **multi-thread-Proc poll-state lifetime** (the lazily-allocated `dev9p_poll_state` is handle_dup-shared -- the poll_list has its own lock, cached_revents is atomic, op/wanted_mask are under g_lock; freed at `dev9p_close` only -- no op pins + no poller registered there by the ref discipline). **Spec-first RE-ENABLED**: `specs/net_poll.tla` (clean + `BUGGY_LOST_READY` + liveness) is the pre-commit gate. Prose validation in NET-DESIGN §12.2 (the as-built QTPOLL + kthread realization) + the `dev9p_poll.c` file header + `docs/reference/121-netd.md` (net-6b) + this row + the focused audit (net-6b-4) + the `dev9p.poll_regular_file_always_ready` kernel test + the joey `net-6b` in-guest E2E (POLLOUT-ready + POLLIN-times-out) + boot OK (931/931) + the SMP gate (net-6b-4). **net-6b-4 CLOSED CLEAN (2 Opus-4.8-max prosecutor rounds + a concurrent self-audit each; CONVERGED; `memory/audit_net6_closed_list.md`):** R1 0/1/0/4 -- DIRTY (the P1 fix restructured the kthread pump loop) -> R2 0/0/0/2 (P3 sharpenings). **F1 [P1]: the global poll-pump pumped only the HEAD op's client, starving a SECOND QTPOLL client's reply** (v1.0-SAFE [one netd -> one dev9p client]; A-5b per-user-netd-latent) -> FIXED `dev9p_poll_collect_clients` pumps every distinct client/cycle (bounded 16). F2 [P3] OOM unwakeable-park -> always-ready degrade; F5 [P3] `p->poll` `__atomic`. F3/F4/R2-F1/#220 = documented v1.x seams (#221/#222/#223/#220). |
| pouch AF_INET socket-compat boundary-line (net-5) | `usr/lib/pouch/patches/0016-pouch-net-sockets.patch` (the 11-file musl stacking patch on 0006) + `usr/pouch-hello/pouch-hello-net.c` (the prover) + the joey net-5 PROBE | net-5 (NET-DESIGN §7; closes W4-F2): the Linux/pouch AF_INET BSD calls (`socket`/`connect`/`bind`/`listen`/`accept`/`send`/`recv`/`getsockname`/`getpeername`/`setsockopt`/`getsockopt`) translated to netd's `/net` 9P file ops -- a per-slot `family` dispatch stacking on the AF_UNIX `0006` layer; **NO new kernel surface** (only `SYS_open`/`read`/`write`/`close`; the shim is libc-level, so ARCH §11.5's zero-socket-syscalls commitment holds). Composes I-1/I-23/I-28 (a Proc reaches only the `/net` its territory grants) + I-5 (netd owns the NIC); adds NO new §28 invariant. The net-6a/6b addenda complete the surface: net-6a-2 (`0017`) made `shutdown`/`sendto`/`recvfrom` tag-aware, and **net-6b-3 (`0018-pouch-net-poll.patch`) made `poll()`/`select()`/`pselect`/`ppoll` work over a `/net` socket** -- a new slot helper `pouch_sock_poll_fd` polls a FAM_INET socket's `QTPOLL`-marked `/net/<proto>/N/ready` sibling (the data fd is a regular dev9p file = always-ready), FAM_UNIX keeps polling its `/srv` `kernel_fd`; NO new kernel surface (only a `SYS_open`); composes I-9 (the kernel dev9p.poll register-then-observe). Remaining seam: `SOCK_NONBLOCK` -> EOPNOTSUPP (a non-blocking `read`/`write` path); the listener-poll gap (`check_ready` reports `can_recv()`, false for a TCP listener; #220, weighed at net-6b-4). **The authoritative prosecution + close record is the matching CLAUDE.md audit-trigger row** (net-5 CLOSE 0 P0 / 0 P1 / 1 P2 / 3 P3, NOT dirty, `memory/audit_net5_closed_list.md`; net-6a/6b addenda + the net-6b-4 audit). Prose: `docs/reference/78-pouch.md` (the AF_INET backend) + NET-DESIGN §7/§12.2/§20. Kernel byte-unchanged; 931/931 + boot OK + SMP gate (0 corruption). |
| Tickless idle (NO_HZ_IDLE; the TI arc) | `arch/arm64/timer.c` (`timer_arm_oneshot_cnt` + the pure `timer_oneshot_tval` -- the one-shot primitive, clamped to `[TIMER_MIN_RELOAD, TIMER_MAX_RELOAD]`; TI-1), `kernel/sched.c` (`timerwait_earliest_deadline` + the pure `tickless_target_cnt` + the shared `sched_idle_park(bool tickless)` body + `bootcpu_idle_main`), `kernel/smp.c` (`per_cpu_main` calls `sched_idle_park(timer_armed)`), `kernel/include/thylacine/sched.h` (`TICKLESS_IDLE_BACKSTOP_NS`). No `timer_disable_this_cpu` -- the always-arm-the-backstop design makes a disable path dead code. | The TI arc (`docs/TICKLESS-IDLE.md` + §8.6; #299). **Scheduler + timer + the idle/wake path -- the most bug-prone lineage in the tree (#788/#806/#860/#809/#811/#926); prosecute hard.** Stops the never-stopped 1 kHz tick on a genuinely-idle CPU (NO_HZ_IDLE -- the periodic tick is byte-unchanged for any RUNNING thread, so I-17 + the §8.2 slice model + the tick-coupled tests are untouched); arms a one-shot to `min(nearest g_timerwait deadline, now + 100 ms backstop)`. **NO new §28 invariant** -- composes **I-9** (no wakeup lost across the idle one-shot arm: register-then-observe, `idle_in_wfi=true` BEFORE the arm + WFI under the IRQ-masked idle region, §8.5) + **I-8** (a deadlined / work-arrival wake still reaches a tickless idle CPU; the `IPI_RESCHED` work-arrival path is tick-independent + unchanged). Prosecute: the I-9 arm-race (no deadline / work-arrival lost in the compute-nearest-deadline -> WFI window); the dropped-IPI backstop (the 100 ms cap self-heals a hypothetical dropped IPI -> a latency hiccup, never a hang -- it replaces the smp.c always-armed-timer backstop); lock order (`g_timerwait.lock` read-then-release before WFI, no new cycle); timekeeping unaffected (`CNTVCT`, not the tick; `g_ticks` freezes at cpu0-idle but is busy-wait/diagnostic-only + busy-wait runs in a running context); the redundant multi-idle-CPU deadline coverage (safe, bounded by ncpus); resume-periodic-on-wake-to-running. **Spec-first RE-ENABLED** (§8.10): the focused sibling `specs/sched_tickless.tla` (clean cfg TLC-green: `NoLostWake` + `ParkedImpliesRegistered` + `RunningNotParked` + the `EventuallyRuns` liveness; + `sched_tickless_buggy.cfg` = `BUGGY_PARK`, the park-before-register lost-wake counterexample at depth 4), LANDED model-first before TI-2 -- a sibling, not a `scheduler.tla` extension (which already proves the tickless wake-correctness; the arm-race would entangle its 11 cfgs -- the `sched_oncpu` precedent). Prose validation in `docs/TICKLESS-IDLE.md` + §8.6 / §8.10 + the matching CLAUDE.md row + the focused audit (TI-3) + the kernel tests (deadlined sleeper wakes on time under tickless; cross-CPU `ready()` wakes a tickless CPU; idle CPU tick-count stops advancing) + the HVF idle re-measure (~0%, the #299 method) + the SMP gate. |
| `/env` per-Proc environment device (Go Stage 4a, G15) | NEW `kernel/env.{c,h}` (`struct Env` { atomic `ref` + `lock` + `entries[ENV_MAX_ENTRIES]` {id,name,value,len} + `next_id` }; `env_alloc`/`env_clone_into`/`env_free`/`env_get`/`env_set`/`env_unset`/`env_list` under `env->lock`), `kernel/include/thylacine/proc.h` (`struct Proc.env` + the `_Static_assert`s; the reserved `RFENVG`=0x100), `kernel/proc.c` (`env_clone_into` in `rfork_internal` after `allowance_clone_into` + rollback; `env_free` in `proc_free` after `allowance_free`), NEW `kernel/devenv.c` (`dc='E'`, `name="env"`; attach/walk[reuse-`nc`]/open/read/write/create/readdir[9P2000.L dirent, monotonic-id cookie]/remove/close; `perm_enforced=false`), `kernel/devramfs.c` (the synthetic `env` mount-point dir), `kernel/dev.c` + `kernel/joey.c` (`dev_register(&devenv)` + `joey_mount_static_dev(&devenv,"env",3)`); the go-thylacine fork `runtime/env_thylacine.go` (`goenvs` reads `/env` via readdir) | ARCH §9.7 (G15). **A new EL0-facing Dev + a new per-Proc group + a multi-thread-shared structure -- prosecute hard.** Composes **I-1** (per-Proc isolation -- per-Proc content behind a shared mount, like `/proc/self`); **no new §28 invariant**. Prosecute: the `env->lock` multi-thread discipline (peer-thread walk/read/write/create/remove/readdir on one `p->env` -- the #844/#57a-F2 shared-state class; no torn read, no lost entry); the `Env` clone+free balance across `rfork_internal` rollback + `proc_free` (no UAF/leak/double-free; OOM-safe like `allowance_clone_into`); the monotonic-id qid (a var removed between walk and read fails clean, never resolves to a different var -- the net-3d slot-reuse discipline); the reuse-`nc` walk contract (the #57a lesson -- else a mounted `/env` is unreachable through `stalk`); the bounds (`ENV_MAX_ENTRIES`/`ENV_NAME_MAX`/`ENV_VALUE_MAX` DoS floor, composes I-32); per-Proc content (every op resolves `current_thread()->proc->env`, never a peer Proc's -- I-1); the readdir cookie (strictly-increasing, never-0, bounds-checked). **No new spec** per the 2026-05-23 broadening -- the `env->lock` is the standard per-Proc-group pattern (Territory/allowance); prose validation in ARCH §9.7 + `docs/reference/NN-devenv.md` + this row + the focused audit + the `env.*`/`devenv.*` kernel tests + boot OK (the `/env` mount + the go-env probe) + the SMP gate. |
| Kernel debug surface: `/proc/<pid>` debug-fs (stop/resume + regs/mem/kregs/kstack/wait; I-39; Go IDE Stage 8a-1) | `kernel/devproc.c` (the debug files: `ctl` verbs attach/detach/stop/start + `regs`/`fpregs`/`mem`/`kregs`/`kstack`/`wait`; `devproc_debug_authorized` = the I-39 two-axis gate; `devproc_target_fully_stopped` + `devproc_all_threads_parked` = the stopped-only predicate; `devproc_build_regs`/`devproc_apply_regs` [the SPSR privilege guard]; `.seekable = true` -- the E2E-caught F1: the debug files are POSITIONED I/O, and the #37 ESPIPE gate rejected `SYS_PREAD` BEFORE `devproc_read` ever ran), `kernel/proc.c` (`el0_return_stop_check(ctx)` at both EL0-return tails + `proc_debug_stop_deliver` [flag + `smp_resched_others`] / `proc_debug_resume` [clear-before-walk + wake] + the `Thread.debug_trapframe` record/clear -- the E2E-caught F2: the EL0 trapframe is NOT at a fixed kstack offset for a running thread [it is SP_EL1-at-entry - 288, lower than kstack_top - 288], so the stop check records the REAL vector `ctx` and the readers consume THAT, bounds-validated within the kstack), `arch/arm64/vectors.S` (the two `mov x0, sp` + `bl el0_return_stop_check` sites: the 0x480 IRQ tail + `.Lel0_sync_return`, both ordered AFTER `el0_return_die_check` -- death wins), `arch/arm64/mmu.c` (`mmu_cross_proc_read`/`write` -- the target-`pgtable_root`-confined VA->PA walk, RO-leaf write-refused, never fault-in), `arch/arm64/halls.c` (`halls_walk_kernel_frames` -- ADDITIVE: the dying-machine dump path stays byte-unchanged; explicit usable-kstack `[lo,hi)` + `fp+16<=hi` fault-safety), `kernel/include/thylacine/thread.h` (`debug_rendez` + `debug_stop_req` + `debug_trapframe`), `kernel/include/thylacine/caps.h` (`CAP_DEBUG = 1<<10`, elevation-only + clearance-grantable), `usr/debug-child` + `usr/debug-probe` (the in-guest E2E pair, joey boot-fatal) | Go IDE Stage 8a-1 (`docs/DEBUG-FS-DESIGN.md`; **invariant I-39**; **spec-first RE-ENABLED** -- the sixth instance of point (a)). **A privilege boundary ON the most bug-prone lineage in the tree (#788/#806/#860/#809/#811/#68 -- the death path) -- prosecute hard.** Prosecute: the **I-39 two-axis gate at EVERY debug file** (owner [same `principal_id` on the 0600 ctl] OR `CAP_HOSTOWNER`/`CAP_DEBUG`; `CAP_DAC_OVERRIDE` deliberately NOT an axis; kproc + `PROC_FLAG_NOTRACE` refused); **stopped-only reads AND writes** (`fully_stopped` = ALIVE + `debug_stop_req` + every non-EXITING thread parked on its OWN `debug_rendez` with `on_cpu==false` [the #788 discipline] + **NO pending `group_exit_msg`** -- the holotype HF1 [P2]: `all_threads_parked` SKIPS EXITING peers, so a group-terminating target read "fully stopped" and the reg/kstack readers raced the dying thread's final `sched()` ctx write -> the death gate + the `build_regs`/`format_kstack` EXITING-head backstops close it; both legs revert-probed in `devproc.debug_regs`); the **SPSR privilege guard** (a regs write applies x0..x30 + sp + pc, NEVER SPSR -- an arbitrary SPSR would eret the target to EL1); the **stop-vs-death ordering** (a stop parks ONLY at the EL0-return tail AFTER `el0_return_die_check`, NEVER inside `sleep()`/`tsleep()` -- the #811 death-unwind is preserved; a stopped target's kill wins over the park); the **trapframe-pointer lifetime** (recorded at the stop check, cleared on the resume-return + interrupt-bail + death legs [HF2]; readers bounds-validate it within `[kstack_base+GUARD, kstack_top-288]`); the **freeze window** (every debug-parked-thread waker holds `g_proc_table_lock`; every debug read holds it too via `proc_for_each` -> wake and read serialized, and the reader's hold pins the Thread against `thread_free` -- no UAF); the **cross-Proc confinement** (the walk resolves strictly through the TARGET's `pgtable_root` under the target `vma_lock`; a non-resident page reads 0, never demand-faults on the debugger's behalf; a `DEBUG_MEM_CHUNK` page clamp bounds the global-lock hold); the **NoStrand resume** (detach/close/**debugger death** resumes the target -- the handle close since #68/#926 fires the ctl close hook); `halls_walk_kernel_frames` stays ADDITIVE (the HX-1/HX-2 extinction dump is byte-unchanged). **Model-gated**: `specs/debug_stop.tla` (model-first @8a-1a; clean cfg + the `EventuallyResumed` NoStrand liveness + 4 buggy cfgs `park_before_die`/`lost_stop`/`double_wake`/`strand_on_debugger_death`) is the pre-commit gate for ANY change to the stop/park/resume/death protocol. **8a-1c CLOSE**: the in-guest `/debug-probe` E2E (stop a genuinely-parked EL0 thread -> regs/mem/kregs/kstack/wait -> cross-Proc mem-write-resume -> reap 0; boot-fatal) caught THREE defects the synthetic kernel tests were structurally blind to (F1 seekable, F2 trapframe location, the UBSan freestanding-memcpy) -- the tests call the read fns directly and hand-place the frame where the buggy code looked; the Fable-5-max holotype (MODEL start==end) closed **0 P0 / 0 P1 / 1 P2 / 2 P3** (HF1 fixed + regression, HF2 fixed, HF3 [P3] the no-realizable-path mmu-arith overflow TRACKED); default 1129/1129 + boot OK + 0 EXT + the SMP gate (default+UBSan x smp4/smp8 N=10, 0 corruption). As-built: `docs/reference/134-debug-fs.md`. **8a-2a ADDENDUM (the HW-debug delivery verify)**: NEW `arch/arm64/hwdebug.{c,h}` (`hwdebug_init_cpu` -- per-PE OS-Lock unlock [`OSLAR/OSDLR=0`, LOCKED at reset], called on the boot CPU [`kernel/main.c`] + every secondary [`kernel/smp.c::per_cpu_main`]; `hwdebug_enumerate` -- `ID_AA64DFR0_EL1` BRPs/WRPs into `g_hw_features.num_brps`/`num_wrps`, from `hw_features_detect`; the self-scoped verify `hwdebug_verify_arm`/`_on_ec`/`_result`/`_disarm`), `arch/arm64/exception.c` (the NEW `EC_BREAKPOINT_LOWER = 0x30` case: `hwdebug_verify_on_ec(ctx->elr)` swallows the verify bp [records + disarms] else falls to the pre-8a-2a SNARE_ILL default -- no 8a-2b routing yet), `kernel/devproc.c` (the `hwverify <hexva>`/`off` ctl verb -- SELF-ONLY [target pid == caller, satisfying I-39 by construction] + user-half VA [`< UACCESS_USER_VA_TOP`, 4-aligned] + the result read gated to the arming Proc [no cross-Proc leak]), `usr/hwbp-verify` (the boot EL0 probe, joey-orchestrated: exit 0 = delivered PASS, 3 = not-delivered [TCG-only, NOT a boot failure], other = malfunction [boot-fatal]). **Prosecute (the FULL HW-debug audit is 8a-2c, with the per-thread install + single-step + watchpoints)**: the `hwverify` self-only gate + user-half VA bound; the one-at-a-time arm (`g_hwbp.lock` a leaf; irqsave; no sleep); the EC-swallow correctness (only the verify arms bps today, so an armed `EC 0x30` IS the verify's -- record+disarm+resume, never kill; an unarmed `EC 0x30` = SNARE_ILL, the preserved default); the DFR0 enumeration (>= 2 bp/wp); the W^X/I-36 preservation (the bp is a debug register, never a `BRK` written into text). **KNOWN verify-only limitation** (unreachable in the boot context; closed by 8a-2b's per-thread install): a not-fired verify whose thread migrates off the arm CPU in the 1-instruction arm->trap window strands `DBGBCR0`/`MDE` on that CPU -- but the verify fires on attempt 1 on both HVF + TCG so the not-fired path is never taken, and it runs once at boot with no competing runnable thread to migrate to. **MEASURED 2026-07-15**: PASS on BOTH HVF (`-cpu host`, Apple M2, GICv2) AND TCG (`-cpu max`, GICv3), attempt 1 -- the research verdict (HVF works, QEMU >= 8.2) empirically confirmed, so 8a-2b builds on the HW path (not TCG-only). Tests: `hwdebug.dfr0_enumerate` + `hwdebug.arm_disarm_roundtrip` (EL1 half) + the in-guest `/hwbp-verify` (the delivery half). No new §28 invariant + no new spec (verify-only; the HW-debug protocol model + the focused Fable round are 8a-2b/8a-2c). **8a-2b ADDENDUM (the HW-breakpoint/single-step/watchpoint tier)**: NEW per-Proc `struct debug_hw` (`arch/arm64/hwdebug.h`; a `Proc.debug_hw` pointer, lazily `kzalloc`'d on the first `hwbreak`/`hwwatch`, freed ONLY at `proc_free` -- never at detach, so the ctx-switch reader never derefs freed memory; `bp_count`/`wp_count` are `__atomic` for a detach-while-running; the `bp_va[]` + `wp_va[]/wp_len[]/wp_flags[]` slots are mutated stopped-only) + `hwdebug_switch_in` (`kernel/sched.c`, after the ASID/ttbr0 install, IRQ-masked + CPU stable): loads the debugged Proc's bps/wps into THIS CPU's `DBGB*`/`DBGW*` + `MDSCR.MDE`/`SS` or clears them -- the **per-CPU-MDE isolation** (a bp/wp fires ONLY while the debugged Proc runs; `PMC=EL0` alone does not isolate two Procs sharing a user VA; per-CPU `g_cpu_debug_loaded` keeps the common non-debugged switch MSR-free). `hwdebug_init_cpu` clears EVERY implemented `DBGB*/DBGW*` ctrl at boot (a reset-garbage E-bit fires the first time MDE is set). The EC routes (`arch/arm64/exception.c`): **0x30** bp -> `hwdebug_breakpoint_from_el0` (match ELR vs the table -> the whole-Proc stop; no match but debugged -> benign STALE arm [detach cleared the table before this CPU reloaded] -> disable this CPU + resume; never debugged [`debug_hw == NULL`] -> `SNARE_ILL`), **0x32** step -> `hwdebug_singlestep_from_el0` (an armed step completed [one EL0 insn] -> disarm SS + re-stop; spurious -> benign clear), **0x34** wp -> `hwdebug_watchpoint_from_el0` (armed [`wp_count>0`] -> the whole-Proc stop; arm64 reports FAR imprecise-within-the-block so delivery is gated on `wp_count>0`, NOT an exact FAR match -- gating on FAR risks a MISSED stop; STALE `wp_count==0` -> benign). **Single-step is the only genuine protocol growth** (`specs/debug_step.tla`, the sibling of `debug_stop.tla`, model-first @8a-2b-alpha -- `StepExactlyOne` + `EventuallyAllDead` + `StepEventuallyReparks` vs `buggy_runs_free`/`buggy_death_lost`): the SS machine is **per-thread** (`Thread.debug_ss_armed` + `debug_stepover_va`) so `MDSCR.SS` follows a mid-step IRQ-preempt migration (the Linux per-task model); `el0_return_stop_check` arms `SPSR.SS` in the resume frame (IRQ-masked to the eret -> exactly one instruction); the **step-over-breakpoint dance** loads the stepped bp DISABLED (else the resume re-traps on it). **Watchpoints are the bp twin** (`hwdebug_wp_encode` -> `DBGWVR` [VA aligned to the 8-byte doubleword] + `DBGWCR` [E=1, PAC=0b10 EL0, LSC from rwx, BAS covering `[va,va+len)` within one doubleword; v1.0 `1..8B` with `(va&7)+len<=8`]); **watchpoints are DISABLED during a single-step** (`hwdebug_load_debug` loads `wpc = ss ? 0 : wp_count`) so the stepped instruction's own data access cannot trap a wp mid-step and derail `StepExactlyOne`. The ctl verbs `hwbreak`/`hwrmbreak`/`step`/`hwwatch`/`hwrmwatch` are gated identically (pid -> kproc-refuse -> I-39 `devproc_debug_authorized` -> slot-owner `debug_owner == ctl` -> stopped-only `devproc_target_fully_stopped` [so the ctx-switch reader is quiescent during table mutation]) + the lazy `debug_hw` pre-alloc discipline (the spare `kzalloc`'d OUTSIDE `g_proc_table_lock`, installed under the walk RELEASE, NULLed when consumed so no double-free). W^X/I-36 preserved (breakpoints are HARDWARE `DBGB*`, never a software `BRK` written into text). **8a-2c CLOSE (the arc audit)**: the consolidated Fable-5-max holotype over the whole 8a-2 surface + **SA-1** (self-found; fixed): a hardware fire delivered the stop by calling `proc_debug_stop_deliver` directly from the EC handler (no lock, no owner check) while a detach cleared `debug_owner` + the table + `proc_debug_resume` under `g_proc_table_lock` -- so a fire racing a detach could set `debug_stop_req` AFTER the detach's resume cleared it, parking the target with no debugger left (the strand; `debug_stop.tla` NoStrand). FIX: the 3 EC handlers now call `proc_debug_fault_stop(p)` (takes `g_proc_table_lock`, delivers ONLY while `p->debug_owner != NULL`; on a NULL owner returns false -> the caller falls back to the benign STALE-arm path / lets the stepped thread run free). The model gained `StopImpliesOwned == sflag => attached` + the `FaultStop` action + the `fault_stop_ungated` buggy cfg (all TLC-green; clean 2264 unperturbed). Regression `devproc.debug_stop_start_resume` SA-1 leg (revert-probed: ungated -> 1134/1135 FAIL). Prosecute (the authoritative 8a-2 list): the SA-1 gate completeness (`proc_debug_stop_deliver` has exactly 2 callers -- the ctl `stop` verb [owner-verified under GPTL] + `proc_debug_fault_stop` [owner-gated under GPTL]; no ungated setter remains) + the fix's lock-order (the EC handler holds no lock when it fires -> GPTL is the outermost, deadlock-free vs the detach's `GPTL -> wait_lock` walk); the per-CPU-MDE isolation (a bp/wp firing on the WRONG Proc); the `debug_hw` lifetime (lazy alloc / free-at-proc_free-only / no torn `(count, slots)` prefix under detach-while-running -- detach only zeroes `count`, add/remove are stopped-only); the ctx-switch install (MDSCR.SS survives a mid-step migration; no stranded MDE/DBGB*/DBGW*/SS after the Proc migrates off; the step-over skip); the EC handlers (never-debugged -> fatal; STALE benign -> no re-trap loop / strand; the wp FAR-imprecise delivery); `hwdebug_wp_encode` (BAS/LSC/doubleword-clamp math -- no wrong-byte / cross-doubleword / EL1-access trap); the 8a-2a verify (`g_hwbp` one-at-a-time) coexisting with the real install (`hwdebug_verify_on_ec` ELR-match); the I-39 gate completeness on the new verbs + user-VA bounds; W^X/I-36; HF3 (`mmu_cross_proc` overflow) stays inert (8a-2 adds no new `mmu_cross_proc` caller). **8a-2c CLOSE: 0 P0 / 1 P1 / 0 P2 / 2 P3, NOT dirty** (Fable-5-max holotype MODEL(end)=Fable 5 + a concurrent self-audit; both converged on the SA-1 fix + the verified-sound set). F1 [P1] a stale `debug_ss_armed` leaks a spurious single-step when a `step` is SUPERSEDED by a peer's whole-Proc stop (the always-multi-threaded Go trap; below `debug_step.tla`'s single-thread abstraction + invisible to the single-threaded E2E) -> FIXED: a whole-Proc stop cancels pending steps (`proc_debug_stop_deliver` clears `debug_ss_armed`+`debug_stepover_va` for every thread; regression `devproc.debug_step_cancel_on_stop`, revert-probed). F2 [P3] the GLOBAL `hwverify` slot (ELR-match swallow, not Proc-confined) could swallow another Proc's real bp + clear its MDE -> FIXED: `hwverify` is boot-only (`boot_is_complete()` gate; the vestigial verify has no post-boot user). F3 [P3] the detached-step leg cleared only SS not MDE+bps -> FIXED: symmetric `hwdebug_disable_this_cpu`. AS-BUILT: `docs/reference/134-debug-fs.md` (the 8a-2a/8a-2b-1/8a-2b-2/8a-2b-3/8a-2c sections). Gates: default 1136/1136 + boot OK + `/debug-probe`(bp/step/wp) + `/hwbp-verify` on HVF + `debug_stop.tla`(clean+5 buggy)/`debug_step.tla`(clean+2 buggy) TLC-green + the SMP gate (default+UBSan x smp4/smp8 N=10); closed list `memory/audit_8a2_closed_list.md`. **8b-1 ADDENDUM (the settled-thread kstack inspect; DEBUG-FS §5b; user-voted 2026-07-16 -- the Stage-8b cross-boundary unified stack)**: `kernel/devproc.c` (`devproc_kstack_walk_cb` DROPS the `fully_stopped` gate -> the SETTLED-thread inspect; `devproc_format_kstack` gates the head on `on_cpu==false`, a `<running>` marker otherwise) relaxes ONLY `/proc/<pid>/kstack` to the Linux `/proc/<pid>/stack` + Plan 9 model: I-39 authorization + kproc-refuse ONLY, NO debug-stop -- so a thread blocked DEEP in a syscall (`sleep -> the rendez`, which the EL0-return-tail-only debug-stop can never freeze) is inspectable. `mem`/`regs`/`kregs`/`wait` KEEP `fully_stopped` (user reads + ALL execution control stay stopped-only -- the vote relaxed ONLY the read-only KERNEL-stack inspect). **Prosecute (the 8b-1d focused holotype)**: the read is MEMORY-SAFE regardless of the target's concurrent execution -- the head walk is bounded to the thread's own usable kstack (`halls_walk_kernel_frames` `[lo,hi)` + `fp+16<=hi`), the Thread struct is pinned by the `g_proc_table_lock` hold (a live Proc's threads free only at the group reap), and a torn `th->ctx`/stack read (a wake+run mid-walk -- a wake takes the rendez/rq locks, NOT GPTL) is garbage-but-in-bounds (never a UAF/OOB); CONSISTENCY is best-effort (coherent for a sleeping thread -- the "why is it hung" diagnostic; garbage-but-bounded for a racing one); NO new race with the stop machinery (the read touches neither `debug_stop_req` nor the park/wake protocol nor the death path -> `debug_stop.tla` UNCHANGED + re-verified clean 2264 GREEN -- a gate condition, not an SM transition, the 8a `mem`/`regs`-read precedent); the I-39 gate is unchanged (owner OR `CAP_DEBUG`/`CAP_HOSTOWNER`; a non-owner inspect refused). **NO new §28 invariant** -- I-39 REFINED, not weakened (§28 I-39 + DEBUG-FS §5b.2). **Kernel DWARF DEFERRED to 8c** (user-signed-off -- the reader + the consumer arrive with `dlv`). Test `devproc.debug_kstack_settled` (settled-not-stopped -> walks [revert-probed: restoring the `fully_stopped` gate -> 1136/1137 FAIL]; running -> `<running>`; non-owner -> I-39 refused). **8b-1d CLOSE (Fable-5-max holotype MODEL(end)=Fable 5 + concurrent self-audit): 0 P0 / 1 P1 / 0 P2 / 2 P3, NOT dirty.** **F1 [P1] (holotype; the self-audit MISSED it -- the two-prosecutor discipline earning its keep): the relaxed kstack read DISCLOSES the KASLR slide (`koff = raw addr - unslid link`, both emitted per frame) to the UNPRIVILEGED OWNER axis** -- an I-16 secret `/ctl/kernel-base` gates behind `CAP_HOSTOWNER` (#57a), and in 8a the owner axis was structurally composed with a `fully_stopped` gate a Proc can never satisfy for its OWN threads (the reader isn't parked on `debug_rendez`), so the owner-axis kstack read was UNREACHABLE. 8b drops that gate -> a multi-thread Proc self-reads its `koff` off a settled head. PRE-EXISTING in 8a via the owner-`attach`-and-`stop`-another-owned-Proc path; 8b widened it to a no-attach self-read. **FIXED**: `devproc_format_kstack(.., bool raw)` emits the raw `addr`+`link` columns ONLY to a `CAP_DEBUG`/`CAP_HOSTOWNER` caller (the debugger tier -- reads `/ctl/kernel-base` anyway + needs raw addrs for the kernel DWARF at 8c); the owner axis gets the KASLR-INDEPENDENT symbolic form (`#N name+0xsoff` -- link-relative). Regression: `devproc.debug_kstack_settled` owner-axis-no-raw + CAP-tier-raw legs (revert-probed: always-raw -> 1136/1137 FAIL); the gamma-3 `debug_kregs_kstack_wait` frame-addr verify reads in the CAP tier. **F2 [P3] FIXED**: the §5b.1 lifetime bullet said "pins ALIVE" -- the real pin is `wait_pid` UNLINKS the ZOMBIE under GPTL before the lock-free `thread_free`, so reachable-under-GPTL (ALIVE or ZOMBIE-unreaped) => threads-not-freed (doc corrected). **F3 [P3] FIXED**: the test now exercises the EXITING-head -> empty-read leg (the relaxed gate no longer rejects a dying target; the format-time EXITING guard is load-bearing). The SOUND set (bounded walk / lifetime pin / on_cpu snapshot / no-stop-race / I-39 gate / probe reaps) traced + held by both prosecutors. Gates: default 1137/1137 + boot OK + 0 EXT + `/stack-probe` PASS (owner-axis SYMBOLIC chain `sched->sleep->tsleep->torpor->dispatch->exception-entry`, no `koff`) + `/debug-probe` PASS (subset) + `debug_stop.tla` clean 2264 GREEN (unchanged) + the SMP gate (default+UBSan x smp4/smp8 N=10). Closed list `memory/audit_8b_closed_list.md`. **8c-2 ADDENDUM (the stop-of-a-sleeper; DEBUG-FS §5c; user-voted 2026-07-14)**: the multi-thread-Go-target enabler -- a nested stop-detour INSIDE `sleep()`/`tsleep()` (`kernel/sched.c`, gated `r != &t->debug_rendez && t->proc`; the `#811` die-check fires FIRST on wake, so DEATH WINS -- a stop parks-and-reparks preserving the syscall while a death unwinds it) + `proc_debug_stop_sleeper_park` + `proc_debug_stop_deliver`'s sleeper-wake walk (`torpor_wake_all_for_proc` for the futex/torpor Ms + the per-peer `wait_lock`->`rendez_blocked_on`->`wakeup` cascade, `proc_group_terminate`-verbatim but arming the detour not the die). An idle futex-parked Go M never reaches the EL0-return tail on its own, so pre-8c-2 a multi-M Go target could never become fully-stopped and a blocking `stop` hung; the detour makes it stoppable. `debug_stop.tla` gained the `sleep` PC + `EnterSleep` + `StopWakesSleeper` + the `BUGGY_STOP_SKIPS_SLEEPER` cfg (clean TLC-green 2952; the buggy cfg violates `EventuallyStopSettles`). **#88 (fixed in the 8c-2 CLOSE)** -- the one regression the detour introduced: `devproc_build_regs` (`/proc/<pid>/regs`) reads the HEAD thread's `debug_trapframe`, which ONLY `el0_return_stop_check` (the EL0-return TAIL park) sets from its vector `ctx`. A DETOUR-parked thread never reaches that tail while stopped, so its `debug_trapframe` stayed NULL -> `build_regs` returns 0 -> `-1`/EPERM. On a multi-M Go target whose HEAD M was sleeping-in-a-syscall at stop-time (Go's sysmon in `nanosleep`->`tsleep`, or the elected 9P reader), `bt`/`print` intermittently EPERM'd while `goroutines` (a `/proc/mem` read, needs no frame) always worked -- the ground-truth signature that refuted the initial "settle-oscillation" theory (the hunt proved Ambush writes only `attach`+`stop`: no 2nd deliver, no resume; the un-park was a phantom). FIX: `arch/arm64/exception.c::exception_sync_lower_el_impl` records `current_thread()->debug_trapframe = ctx` at the single EL0-sync choke point, so a syscall/fault-blocked detour-parked thread reports the correct "ptrace at the syscall boundary" EL0 register state (the outermost EL0->EL1 frame, valid on the kstack for the whole syscall). Read-safe: `debug_trapframe` is READ only when fully-stopped (a parked thread always has a fresh frame -- tail-park overrides with its return ctx; detour is mid-syscall), so the between-syscalls stale value is never observed + a non-debugged Proc never reads its own; no new field. Prosecuted: all 3 readers (build_regs/apply_regs/the step-over PC) fully_stopped-gated; the entry frame within `[kstack_base+GUARD, kstack_top)`; the fault-path store BEFORE `arch_fault_handle`; F3 (the `proc_debug_stop_sleeper_park` corrupt-t guard -> `extinction`, dead-hardening since `t == current_thread()`). Corollary seam (pre-existing, unchanged): a `step` on a detour-parked head arms `SPSR.SS` in a frame the detour-resume never `eret`s -> the step is ineffective (the per-thread `/proc/<pid>/thread/<tid>/` step is its v1.x home). 8c-2 CLOSE gates: 16/16 focused smp8 verify + the full SMP gate (default+UBSan x smp4/smp8 N=10 = 40/40, 0 corruption) + `debug_stop.tla`/`debug_step.tla` TLC-green + the focused Fable holotype + the `/ambush-probe` E2E (goroutines + real `bt` frame + `print main.Sentinel`); closed list `memory/audit_8c2_closed_list.md`; as-built `docs/reference/134-debug-fs.md` + `docs/DEBUG-FS-DESIGN.md` §5c.6. **8c-3 ADDENDUM (the #89 reader-role release; DEBUG-FS §5c.6; on the 3-round-audited #841 elected-reader machinery -- prosecute hard)**: the elected-9P-reader role (#841 `reader_active`) was HELD across the detour-park, so a debugged Proc holding the SYSTEM Stratum client's reader role at stop-time froze system-FS (`/bin`,`/lib`) for every survivor Proc sharing that client for the stop duration (per-user home clients isolated via A-5b `--single-session`; the surface is the system client only). The fix mirrors the death-path role-release with THREE composed mechanisms, made FRAME-ATOMIC (F1) via a FOURTH flag: (0) **`Thread.stop_no_park`** (a 2nd per-Thread bool in the same padding) held by `reader_recv_frame` for the whole recv tenure so a MID-FRAME stop blocks through (above). (1) **`Thread.stop_unwinds`** (a per-Thread bool in the `debug_ss_armed` padding -- no struct-size change), which `reader_recv_frame` sets `= (got==0)` PER-CHUNK (frame-atomic -- a stop unwinds ONLY at a clean frame boundary; both flags cleared on the wrapper's exit); when it is set, the `sleep()`/`tsleep()` detour RETURNS `SLEEP_INTR`/`TSLEEP_INTR` -- reusing the death-interrupt propagation the transport recv already tolerates (leaves the transport reusable, no `ERROR` latch) -- INSTEAD of parking in place, so the recv unwinds to `client_wait`, which detects the stop (`client_stop_pending`: `debug_stop_req` set, not dying), clears `reader_active`, `client_handoff_reader_locked`s the role, then parks role-free (`client_debug_stop_park` -> `proc_debug_stop_sleeper_park`, `stop_unwinds` cleared so it PARKS) + re-elects on resume; every OTHER sleep (clear) parks in place (8c-2 default). (2) A **top-of-loop stop guard** in `client_wait` -- a debug-stopped thread reaching `client_wait` fresh parks PROMPTLY (never elects), re-handing-off first if it holds `be_reader` (the handed-then-stopped race). (3) The **handoff skips debug-stopped owners** (`p9_rpc.owner` = the submitting Proc; `client_handoff_reader_locked` skips `owner->debug_stop_req != 0`) -- the released role must land on a runnable SURVIVOR, not bounce to a stopped sibling (whose thread has parked, so its `rpc->rendez` waiter has moved to `debug_rendez` -> the `be_reader` wakeup is a no-op -> a lost role); no survivor pending -> the role is dropped (`reader_active` already false) -> a future survivor op self-elects. (4) **F2 -- the role-release covers ALL FOUR `reader_active` sites**, not just the `client_wait` election: centralizing the flags in `reader_recv_frame` gives the block-through to every caller, and each handles its stop-return without killing the shared session or spinning -- the `client_pump_or_park_locked` self-pump skips `client_mark_dead_locked` on a `client_stop_pending` unwind; `client_send_flow` (spilling `out_buf` FIRST, the #375 discipline) + `client_drain_until_free_tag` PARK a stopped sender at their loop top (else a stop-unwound self-pump drains nothing -> retry EAGAINs forever -> the Proc never fully-stops -> the debugger's stop HANGS); `p9_client_reader_pump_once` skips mark_dead on a stop; only `p9_client_reader_pump_once_deadline` has no guard, justified kproc-only (its sole callers -- `dev9p_poll.c` + `loom.c` SQPOLL -- have `t->proc == NULL`, detour-immune). Prosecute: the I-9 no-lost-stop-wake for the reader unwind (the recv's detour reads `debug_stop_req` under `wait_lock`, register-then-observe, identical to the 8c-2 park's I-9); the stream-desync -- **the load-bearing correctness line, CORRECTED at the frame-atomic re-fix (the 8c-3 holotype F1 [P1] REFUTED the original "unreachable" claim)**: delivery is CHUNKED (`srvconn` `chan_ring` short-read/write; `srvconn_client_recv` tsleeps on an EMPTY ring, reachable MID-FRAME under pipelining depth >= 2 + ring pressure -- a frame is split across the ring in time), so a SIMPLE stop-unwind (the abandoned first fix: set `stop_unwinds` for the whole recv) after a partial frame is consumed discards those bytes -> a survivor's fresh `reader_recv_frame` reads the frame TAIL as a header -> shared SYSTEM-Stratum session death / task-#50 silent misframe. The re-fix is **FRAME-ATOMIC**: `reader_recv_frame` sets `stop_unwinds = (got==0)` per-chunk (a stop unwinds ONLY at a clean frame boundary, no bytes consumed) and holds `stop_no_park` for the whole recv tenure so the `sleep`/`tsleep` detour BLOCKS a mid-frame stop THROUGH -- neither unwind (desync) nor park-in-place (holds `reader_active`) but fall through to the normal register+sched -- finishing the frame (bounded by the trusted server's delivery, CF-3 B), then unwinding at the next boundary. The IDENTICAL death-path mid-frame unwind (the `#811` die-check below the detour is UNCHANGED, so a death still unwinds mid-frame) is a PRE-EXISTING #841/#811 latent -- **owned + tracked as task #90** (it NARROWS #811 universal death-interruptible sleep for this one recv -> a section-28 refinement, ARCH section 8.8.1.1). **#90 DESIGN (block-through, mirror 8c-3; user-signed-off + design-voted 2026-07-19):** the die-check (`thread_die_pending` in sleep()/tsleep(), sched.c:1839/1998) becomes FRAME-ATOMIC for the elected reader -- it unwinds a dying reader ONLY at a frame boundary (`stop_no_park` set + `stop_unwinds`/got==0) and BLOCKS THROUGH mid-frame (falls to register+sched, finishing the frame bounded by the trusted server's whole-frame delivery per CF-3 B), exactly the shipped 8c-3 stop block-through -- so a death never discards consumed partial-frame bytes -> the survivor never reads a frame-tail-as-header (no shared-stream desync). REUSES the existing `stop_no_park` (frame-atomic-recv gate) + `stop_unwinds` (at-boundary latch) -- both already set `= (got==0)` in do_reader_recv_frame and exactly the predicates death needs -> NO new Thread field; the guard widens from unconditional to `!stop_no_park || stop_unwinds`. Block-through strands no caller lock (the reader holds none across its recv, #841). Spec-first RE-ENABLED: `specs/reader_frame.tla` model-first (NoDesync = an async unwind fires only at got==0 + EventuallyUnwinds + a mid-frame-unwind buggy cfg). v1.x LIVENESS SEAM: an UNTRUSTED / hung server sending a partial frame then stalling hangs the dying reader mid-frame (the #845-F1 / Loom-4-F4 untrusted-server class; all v1.0 servers are trusted local Procs). Prosecute at #90-audit (death-path lineage -- hard): the guard fires for every NON-reader sleeper (unconditional, as today); a mid-frame reader NEVER unwinds; BOTH die-check sites (sleep + tsleep); the survivor stream stays synced across a mid-frame reader death; liveness under bounded delivery. With #90 landed, DeathWinsOverStop at every branch (the dying check precedes the stop check in the recv loop, the top-of-loop, and the role-free park) -- now BOTH death and stop unwind at a frame boundary; the `owner` lifetime (alive while its rpc is inflight, as safe as `r->done`; the only two non-NULL `inflight[]` installs -- `client_run`, `submit_async` -- set it; the death path is unchanged: dying owners carry no `debug_stop_req`, so the F6 bounce still handles them); `stop_unwinds` scoping (set at the reader election, cleared before ANY park, owner-only access, never leaks past `client_wait`); the async path untouched (`submit_async` sets `owner=NULL` + `on_complete`, skipped first). NO new §28 invariant + NO spec change (the reader-role is below the `9p_client.tla` + `debug_stop.tla` models, both re-verified GREEN; the model is structurally blind to the reader-role, exactly as this row notes -- which is why it stays green while the fix lands). Validated by the focused 8c-3 Fable holotype + `9p_client.handoff_skips_debug_stopped_owner` (revert-probed: pre-fix 1137/1138 FAIL) + the SMP gate; the full multi-Proc-survivor-during-stop deterministic E2E is the SAME owed multi-in-flight cross-Proc SMP harness carried across #841/#845/#349/Loom. closed list `memory/audit_8c3_closed_list.md`; as-built `docs/reference/134-debug-fs.md` + `docs/DEBUG-FS-DESIGN.md` §5c.6. **8c-2 #95 ADDENDUM (the debug-fs FOCUS thread; multi-M breakpoint inspect):** the debug-fs read only the HEAD thread (`target->threads`), but a HW breakpoint on a multi-M Go target fires on whichever M runs the migrated goroutine -- NOT the head -- so `/proc/regs` reported the wrong M's PC, the debugger could not attribute the stop, and `continue` auto-resumed forever (measured 8199 fire/resume pairs, 0 step-over, on `ambush exec /ambush-child`). FIX (Option A, user-voted): `struct Proc.debug_focus_thread` (proc.h @328, size 328->336) -- `proc_debug_fault_stop` sets it to `current_thread()` (the ONE path every bp/step/wp EC fire routes through -- `hwdebug_{breakpoint,singlestep,watchpoint}_from_el0`), `proc_debug_resume` clears it, and `devproc_focus_thread` (validated-against-`p->threads` under `g_proc_table_lock`, else head) is read by `devproc_build_regs`/`apply_regs`/the step verb/the 8b kstack, so regs + kregs (`tpidr_el0`->Delve's G recovery) + kstack + step report the firing M coherently. Prosecute: the focus-pointer LIFETIME (no UAF -- the load-bearing reason is the `g_proc_table_lock` PIN, not a stop gate [#95-audit F1]: reap does proc_unlink_child under GPTL BEFORE the lock-free thread_free loop + proc_for_each walks the kproc-rooted children tree, so a still-reachable target is unlinked-none-freed, and devproc_focus_thread's in-list validation rejects a stale/foreign pointer [do NOT delete it -- the 8b settled kstack drops the fully-stopped gate, so pin+validation are its only net]; the fully-stopped gate is an ADDITIONAL property of mem/regs/kregs/step; a set focus is `current_thread()` of the debugged Proc, guarded at the store [`ct->proc == p`, F2]; any mismatch -> head); the set (`fault_stop` under lock) / clear (`resume` atomic) / read (the 4 walk_cbs under lock) ordering; the death-path interaction (DeathWinsOverStop -- the focus never points at a dying thread the read reaches); the 4-site consistency + the step re-focus on its own EC 0x32; the single-thread (8a-2b) + manual-stop (attach) regression (focus == head). NO new §28 invariant (I-39 holds -- it only refines WHICH stopped thread's frame crosses, gated identically); `debug_stop.tla`/`debug_step.tla` unchanged (frame-reporting-agnostic, re-verified GREEN). Validated by the focused 8c-2-#95 Fable holotype + the `/ambush-probe` stage-C E2E (the durable multi-M regression: it loops 8199x without the fix; break + continue + bt/print GREEN at a HW bp on a multi-M Go target) + 1138/1138 + boot OK + the SMP gate. closed list `memory/audit_8c2_95_closed_list.md`; as-built `docs/reference/134-debug-fs.md` (the "8c-2 #95" section). **EXITKILL addendum (I-39 die-with-launcher; designed 2026-07-23, user-voted; `specs/debug_stop.tla::EventuallyLaunchedDies`).** A per-Proc `Proc.debug_exitkill` (a pad-slot bool near `debug_stop_req`) marks a debugger-LAUNCHED target; the debugger sets it via the new ctl `exitkill` verb (owner-gated: `debug_owner == c`, under `g_proc_table_lock`) right after attaching a child it spawned. `devproc_debug_release_cb` (the ctl-fd-close release, reached on debugger DEATH via `devproc_close` + the #68 last-thread-out close) `proc_group_terminate`s an `exitkill`-marked ALIVE target -- the #811 death cascade wakes the debug-parked threads to die at the die-check, death wins over the stop, safe under `g_proc_table_lock` per the `devproc_kill_walk_cb` precedent -- instead of `proc_debug_resume`, closing the debugger-LAUNCHED-orphan leak (a Plan 9 NoStrand-resume orphans a launched debuggee to init to run forever -- the HVF-idle debuggee leak). Closes the leak for EVERY debugger-death mode (clean EOF, blocked-`Wait` timeout, the probe's own `amb.kill()`); the ambush userspace side already issues the graceful `kill` (both `/ambush-probe` stages C+D, stock Delve, traced end-to-end), so the death-path resume is the ONLY remaining gap. The mark is cleared on `attach` (fresh slot), on explicit `detach` (which still RESUMES -- the debugger's deliberate choice; it would send `kill` to terminate), and on release; only the IMPLICIT death-release honors it. Prosecute: the release-cb terminates ONLY an ALIVE `exitkill` target (a dying/ZOMBIE one falls to `proc_debug_resume`, itself magic-guarded + safe); the `exitkill` verb is owner-gated (a stranger cannot mark a target for death); the mark is set-once + cleared with the slot (no stale mark across re-attach; the exitkill verb + the release both run under `g_proc_table_lock` -- serialized); `StopImpliesOwned` unaffected (the terminate clears `debug_stop_req` via the group-terminate death path, not a stranded stop); an attached (unmarked) target still NoStrand-resumes (`EventuallyResumed`). NO new §28 invariant (refines I-39). Ambush half: `pkg/proc/native/proc_thylacine.go::Launch` sends `ctlWrite("exitkill")` after attach+stop (best-effort -- a failed mark loses only the death-safety-net, never breaks the launch; the graceful `kill` still fires); the `Attach` path does NOT send it. Regression `devproc.debug_exitkill_terminates_on_close` (revert-probed: a marked target dies on ctl-fd-close, an unmarked one resumes); the focused Fable-5-max holotype + the SMP gate close the arc. |
| PTY / job control: sessions + process groups + the pts registry + the tty signal seam + the job-control stop (I-20 stop leg; PTY-1 kernel arc) | **1a** `kernel/proc.c` (`proc_setsid`/`setpgid`/`getpgid`/`getsid` under `g_proc_table_lock`) + `kernel/include/thylacine/proc.h` (`sid`/`pgid` @328/@332, rfork-INHERITED unlike the KP_ZERO tail; a parentless Proc defaults `sid=pgid=pid`) + `kernel/syscall.c` (`SYS_SETSID`/`SETPGID`/`GETPGID`/`GETSID` = 89-92) + `kernel/include/thylacine/errno.h` (`T_E_SRCH = 3`). **1b** `kernel/notes.c` + `kernel/include/thylacine/notes.h` (the `tty:*` note CLASS -- kernel-only-POST [`notes_post` rejects the `tty:` prefix for `synthetic==false`, the F4 gate] AND CATCHABLE; `NOTE_BIT_TTY = 5`, mask `0x3f`; the terminate class `{interrupt, tty:quit, tty:hup}` via the SECOND latch `PROC_FLAG_TTY_TERMINATE_PENDING` [bit 8, paired 1:1 with `NOTE_BIT_TTY` in `thread_die_pending` so masking one family never spuriously unwinds for the other]; `notes_terminate_note_name_locked`; `notes_post_pgrp`/`notes_post_pid` -- one-`g_proc_table_lock`-hold fan-outs, pgid/pid 0 refused). **1c** NEW `kernel/pts.c` + `kernel/include/thylacine/pts.h` (the pts REGISTRY -- the Weft `share_id` shape, gen-stamped, NOT a handle-table kind; the `(SrvConn ptr, qid)` correlation key: bindings HOLD `srvconn_ref`s [no ABA], resolve POINTER-COMPARES only [never deref -> fail-closed]; MINT gated on a server-endpoint conn fd + `MAY_POST_SERVICE`; SLAVE/FREE on the minting server's never-reused pid; torn-conn lazy GC at mint-full; the gen `(gen<<16)|idx` never 0, bumped at free) + `kernel/9p_srvconn_transport.{h,c}` (`p9_srvconn_transport_conn` -- the magic-checked ctx downcast, IDENTITY-USE-ONLY) + `kernel/syscall.c` (`SYS_PTY_REGISTER = 93`). **1d** `kernel/pts.c` (`pts_tty_signal` -- server-scoped, routes pts -> ct_sid -> fg_pgid under the gen-valid lock then posts AFTER release [no `g_pts_lock`->`g_proc_table_lock` nesting]; HUP's F13 dual target; `pts_tty_acquire`/`set_fg`/`get_fg` -- session-leader + anti-steal F7 + one-ctty-per-session + the master-read carve-out) + `kernel/syscall.c` (`SYS_TTY_SIGNAL`/`ACQUIRE`/`SET_FG`/`GET_FG` = 94-97). **1e** `kernel/proc.c::wait_pid_for` (the `WAIT_UNTRACED`/`WAIT_CONTINUED` + `want_pid 0`/`< -1` pgrp selectors [caller-pgid read at each authoritative re-scan -- the mid-wait `setpgid`-leave]; report-IS-NOT-reap [pid + packed status, the latch consumed exactly-once under `g_proc_table_lock`, NO teardown]; the packed Linux `wait(2)` layout OPT-IN via the flags -- raw status preserved for every pre-PTY caller) + `kernel/include/thylacine/proc.h` (`stop_report_pending`/`cont_report_pending` @336/@337, KP_ZERO, never rfork-inherited). **1f** `kernel/include/thylacine/proc.h` (`job_stop_req` @316 -- the pad slot after `debug_stop_req`, same cache line, NO struct growth; `proc_stop_requested` = the `debug|job` disjunction) + `kernel/proc.c` (the shared `proc_stop_wake_cascade_locked`; `proc_job_stop_one_locked`/`proc_job_resume_one_locked`; the catchability gate `proc_tty_susp_would_stop_locked`; the orphan-rule helpers `pgrp_orphaned_locked`/`pgrp_has_job_stopped_locked`/`pgrp_orphan_hup_cont_locked`/`proc_orphan_rule_locked` [hooked into `proc_become_zombie_locked` BEFORE the reparent]; `proc_job_stop_pgrp`/`proc_job_cont_pgrp`; `stop_park_wake_cond`/`proc_stop_sleeper_park` -- renamed from the `debug_*` forms) + `kernel/sched.c` (both `sleep`/`tsleep` stop detours read `proc_stop_requested`) + `kernel/9p_client.c` (`client_stop_pending` + the elected-reader handoff skip read the disjunction -- the 8c-3 reader-role release generalizes to a JOB stop) + `kernel/devproc.c` (`devproc_target_fully_stopped` stays DEBUG-only -- documented do-not-generalize) + `kernel/pts.c` (`pts_tty_signal`'s TSTP arm -> `proc_job_stop_pgrp`; `pts_tty_cont` + the `pts_teardown_fan` staged off `pts_clear_locked` at FREE + the mint-GC arm) + `kernel/syscall.c` (`SYS_TTY_CONT = 98`; `TTY_SIG_TSTP` live) + libt/libthyla-rs (`t_tty_cont` + `T_SYS_TTY_CONT`) | The PTY-1 kernel arc (`docs/PTY-DESIGN.md`, the Fable design pass; **invariant I-20** [the stop leg]; **spec-first RE-ENABLED for the stop composition** -- `specs/pty_stop.tla` [the `debug_stop.tla` sibling], model-first). **On the death-path lineage (#788/#806/#860/#809/#811/#68/#89) AND a new privilege surface -- prosecute hard.** The security-sensitive routing (sessions/pgrps/controlling-terminal + note-to-pgrp + the stop) is kernel; the byte/cooking engine is the userspace `ptyfs` (PTY-2+), whose SOLE signal authority is the pts-scoped `SYS_TTY_SIGNAL` -- **it can NEVER name a pgrp** (the I-1/I-22 bound: pts -> ct_sid -> fg_pgid is kernel-resolved). Prosecute: **I-20 stopOwners** (park iff `debug_stop_req | job_stop_req`; each resume clears ONLY its own owner -- `proc_debug_resume` never touches job, `proc_job_resume_one_locked` never touches debug: `StopCompatI39`/`BUGGY_DOUBLE_STOP`; death wins over both -- every `group_exit_msg` check precedes, `DeathWinsOverJobStop`/`BUGGY_DEATH_BLOCKED`); **the park-predicate fan-out COMPLETENESS** (round-2 R2-F2 -- EVERY `debug_stop_req` read is the disjunction where a job stop must also park, and `devproc` fully-stopped stays debug-only where it must NOT; the sharpest is the 8c-3 elected-9P-reader release -- a Ctrl-Z'd fg child blocked as the SYSTEM-Stratum reader must release + re-hand the role or it freezes `/bin`,`/lib` for every survivor, the exact #89 bug via the job flag); **I-39 unchanged** (a job stop confers no debug readability; `notes_post` rejects userspace `tty:*` so no parent conts a debug-stopped child; the tty-seam authority is `server_pid` for SIGNAL, session membership for the fd ops); **I-9 generalized** (no stop-wake lost -- register-then-observe under `wait_lock` at the detours, the deliver's RELEASE-set-before-wake, the resume's clear-before-wake; no wait-report wake lost -- latches set under `g_proc_table_lock` + `child_waiters` woken under it, the same lock `wait_pid_for`'s re-scan takes -- the fresh WAIT_UNTRACED/WAIT_CONTINUED edge); **the catchability gate** (R2-F3 -- a caught susp [handler/self-managing/all-masked] is a note, NEVER a stop; the default stop CONSUMES the signal, no queued susp across the stop); **the POSIX orphan rule** (2.4.3 -- the TSTP-side discard on an orphaned group + the death-side hup+cont fan on a newly-orphaned stopped group; "newly orphaned" never re-signals an already-orphaned group [a spurious hup could kill]; a group the dying Proc never anchored is untouched); **lock order** (`g_pts_lock` a leaf -- the fans run AFTER it drops; every new `g_proc_table_lock` path composes with `q->lock`/`torpor_lock`/`wait_lock`->`r->lock`/the poll-waiter list as the notes/death precedents do); **the pts registry** (gen guard -- a stale `pts_id` never reaches a new occupant; the ref balance across mint/bind/free/GC/idempotent-re-bind; the correlation fail-closed). **Model-gated**: `specs/pty_stop.tla` (clean + `pty_stop_liveness` + `pty_stop_buggy_double_stop`[StopCompatI39] + `pty_stop_buggy_death_blocked`[DeathWinsOverJobStop]) is the pre-commit gate for any stop-ownership change; `specs/pty.tla` (the master/slave data path, reserved for PTY-2+) + `debug_stop.tla` (re-verified GREEN -- the reader-role + job owner are below its abstraction, exactly as 8c-3). **No new §28 invariant beyond I-20's stop leg** (composes I-1/I-9/I-19/I-22/I-24/I-39). **CLOSE at PTY-1g** (the focused Fable-5-max holotype over the whole 1a-1f surface + the SMP gate + the in-guest E2E; the byte/cooking + `ptmx`/`pts` tree + per-fd termios are PTY-2+, `pty.tla` lands then). As-built: `docs/reference/135-pty-kernel.md`; `docs/PTY-DESIGN.md` §13. Prose validation in PTY-DESIGN.md + this row + `docs/reference/135-pty-kernel.md` + the PTY-1g holotype + the pts/proc/9p_client tests + the spec gate + boot OK + the SMP gate. |
| ptyfs: the pseudoterminal server + /dev/pts (I-20 data path; the PTY-2 arc) | `usr/ptyfs/` (the native device-less /srv 9P server owning the pts pairs: `server.rs` -- the Conn/fid table + dispatch, the `Ptys` table [two byte rings m2s/s2m + `RecvOutcome` + the per-pts `tio`/line/winsize], the ptmx clone-mint + `SYS_PTY_REGISTER` MINT/SLAVE/FREE, the per-pts ldisc [`master_write` input cook ICRNL->ISIG->ICANON\|raw; `slave_write` ONLCR; the single `echo()` chokepoint], the `/dev/pts/<n>ctl` grammar [`ctl_apply`/`ctl_render`], `Conn::close_endpoint` [the HUP edge], the deferred multi-waiter read [`PendingRead` + `poll_reads`]; `main.rs` -- selftest -> post -> the serve loop), `kernel/devdev.c` (the `pts` mount-stub DIR child, `DEV_KIND_PTS` -- walk_one-only, empty, visibility-only), `usr/joey/joey.c` (the boot-fatal spawn/liveness/mount + the 2a-2 probe + the 2e E2E), `usr/pty-probe/` (the two-role openpty E2E) | The PTY-2 arc (`docs/PTY-DESIGN.md` section 5 as-built; **invariant I-20, the data path -- ENFORCED here**; `specs/pty.tla`'s data-path actions map to `server.rs` per `specs/SPEC-TO-CODE.md::pty.tla`). **The byte/cooking engine is USERSPACE -- a buggy ptyfs corrupts only its own pts state, never a kernel invariant; its SOLE signal authority is the pts-scoped `SYS_TTY_SIGNAL` (it can never name a pgrp -- the I-1/I-22 bound, prosecuted at PTY-1). Single-threaded by construction (one Proc, one serve loop -- every 9P frame across every session is sequential, so the pts table needs no lock and I-9 holds by `poll_reads`-before-park; a threading lift must revisit BOTH).** Prosecute: **I-20 byte conservation** (every consumed non-signal input byte is ring data [assembled-then-flushed or raw]; `SignalXorByte` -- an ISIG char raises + is consumed, never a byte, never echoed; ONLCR pair-atomic back-pressure [a torn CR-NL would double the CR on retry]); **the ring-full split** (cooked flush + echo DROP on full [tty input overrun; the cons posture]; raw input + output BACK-PRESSURE short); **drain-then-EOF** (`ring_drain` serves Data while non-empty regardless of closure; Eof only empty+peer-gone; the `slave_opened_once` latch -- a master read BEFORE any slave open PARKS, never a spurious EOF [the Linux master-blocks semantic; the 2e E2E's readiness read races the child's slave open without it]); **HupAtMostOnce BY CONSTRUCTION** (exactly one master fd per pts ever: mint-only + no walk resolves a master path + 9P forbids walking FROM an opened fid + a walk to an existing newfid is rejected -> the `n_master` 1->0 edge fires at most once; `close_endpoint` raises BEFORE the unref; the mint-rollback keeps the raw dec [not carrier loss]); **the fid/refcount discipline** (bound `refs` = the only free path [refs-new-before-unref-old on rebind]; opened `n_master`/`n_slave` = the EOF counts; a ctl fid holds a ref but is NOT an endpoint -- `is_endpoint_path` gates all FOUR open_dec sites [an opened-ctl clunk must never decrement the slave count]; FREE at last unref only; the registered-vs-selftest-local split [`pts_id 0` never signals]); **the ctl grammar bounds** (tcsetattr-atomic: ALL tokens validated before ANY applied; token/op caps; the winsize band <= 65535; WINCH iff CHANGED [Linux TIOCSWINSZ]; a flag apply resets the assembly [TCSAFLUSH]; the offset-served read never defers); **the cacheability posture -- LOAD-BEARING** (ptyfs answers `Twalkgetattr` E_NOSYS via the dispatch default -> the kernel latches the session non-cacheable -> the Larder NEVER caches pts bytes [the netd precedent; a cached tty read would replay stale bytes]); **the ptsname contract** (an endpoint qid = `PTS_FLAG(bit 40) \| N<<8 \| filekind`, surfaced verbatim via `t_stat.qid_path` -- a documented client ABI, the PTY-3 pouch `ptsname()`); **the devdev stub** (visibility-only; no gate change -- the I-27 cons/consctl gates untouched; walkable QTDIR as the mount-point identity only). **No new section-28 invariant** (realizes I-20's data path; composes I-1/I-9/I-22/I-27). Validated by `specs/pty.tla` (the as-built action map) + the in-server selftest (the rings + the full ldisc truth table + the ctl grammar + the teardown algebra) + the boot-fatal joey 2a-2 probe (the wire data path: cooked lines, echo, ptsname, ctl, drain-then-EOF) + the `/bin/pty-probe` openpty E2E (a LIVE controlling session: the parked deferred read + INT/WINCH/HUP observed on the fg session's notes fd) + the PTY-2e focused audit + boot OK + the SMP gate. As-built: `docs/reference/136-ptyfs.md`. |
| pouch pty boundary-line (PTY-3) | `usr/lib/pouch/patches/0021-pouch-pty.patch` (sixteen files -- the full per-file rationale is the patch preamble: `bits/syscall.h.in` [setsid/setpgid/getpgid/getsid -> 89-92 + the tty aliases 95-97], `src/misc/ioctl.c` FULL REWRITE [the tty ioctl dispatcher], `src/misc/pty.c` + `openpty.c` [the /dev/pts/ptmx mint redirect], `src/fcntl/openat.c` [resolution moved onto the stalk resolver -- SYS_open=65], the raw-SYS_ioctl reroutes [`__ptsname_r`/`isatty`/`tcdrain`/`tc[gs]etwinsize` + the `__fdopen`/`__stdout_write` line-buffering tty probes], the 0007 signal-layer growth [`_pouch_signal.{h,c}`/`sigaction.c`/`kill.c`/`raise.c` -- the tty family]), `usr/ptyfs/src/server.rs` (the S_IFCHR posture -- PTY-3a), `usr/pouch-hello/pouch-hello-pty.c` + `usr/joey/joey.c` (the boot-fatal probe + the (e2b) mode leg) | PTY-3 (PTY-DESIGN.md section 7 as-built; the pouch boundary-line row the section-8 design reserved). **The kernel is byte-unchanged** -- the surface is pouch + one ptyfs getattr posture. Composes I-1/I-9/I-20/I-22/I-27/I-28/I-39; adds NO new section-28 invariant. Prosecute: **the is-a-pts discrimination** (S_ISCHR + the PTS_FLAG qid decode + the POUCH_SOCK_TAG early reject -- a /net qid [bit-40 CONN_FLAG, S_IFREG] or any non-pts fd must NEVER pass; the ptyfs CHR posture is the load-bearing half, pinned E2E by the joey (e2b) leg); **the receive-only gate** (kill()/raise() of a tty signum -> EPERM BEFORE any kernel post -- the I-39 F4 gate's POSIX surfacing; a pouch program must have NO path to originate a tty:* event); **the SIG_DFL dispositions** (quit/hup NDFLT [whole-Proc terminate]; winch/cont NCONT; susp NCONT -- the DOCUMENTED seam: NDFLT would TERMINATE on ^Z [the kernel NDFLT arm exits, never stops] and the kernel pre-delivery stop gate reads pouch's always-registered handler_va as caught; the kernel NDFLT-stop arm is the signoff-needing fix, tracked in 83-pouch-signals.md]); **the openat stalk migration** (SYS_open FROM_ROOT + the real -errno decode -- every pre-existing pouch open() re-proven by the prover ladder + the stratumd boot; the old per-component loop opened intermediates with the final omode and could never open write-mode across a mount); **the dispatcher's ctl round-trips** (per-op open/read-or-write/close of `/dev/pts/<n>ctl`; the TCSETS decompose writes ALL FIVE flags atomically; the winsize col/row order inversion vs struct winsize); **the errno-preserved stdio probes** (the fdopen/first-write tty probe must not clobber a successful write's errno; one probe per stream lifetime); **the family-bit coarseness** (one NOTE_BIT_TTY for five signums -- blocking one blocks all; the kernel terminate-class latch fires a masked quit/hup on unmask). Honest seams: forkpty (no fork() in pouch -- fails at fork, asserted), kill(-pgrp) (no SYS_POSTNOTE pgrp arm -- ABI addition, signoff), TCFLSH/TIOCGSID/TIOCNOTTY (ENOTSUP), tcgetattr's fixed-cc report (the ldisc trio; VMIN=1/VTIME=0; other cc slots accepted-and-ignored). Validated by the `/pouch-hello-pty` boot-fatal probe (mint -> ptsname -> isatty trio -> cooked-default -> cfmakeraw round-trip -> winsize -> the session dance -> live ISIG ^C [SignalXorByte through the POSIX view] -> live WINCH/HUP -> EPERM -> forkpty honest-fail -> drain-then-EOF) + the unregressed 2a-2/2e native probes + every pouch prover + the stratumd boot + the focused audit + boot OK + the SMP gate. As-built: `docs/reference/78-pouch.md` "The pty boundary-line" + `83-pouch-signals.md` "The tty family". |

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
| I-2 | Fork-grantable capability set monotonically reduces (`rfork` only reduces; the elevation-only set `CAP_ELEVATION_ONLY` = HOSTOWNER + DAC_OVERRIDE + CHOWN + KILL is stripped at every fork — A-4-pre). Elevation-only capabilities are the sole sanctioned growth, conferred only via the `cap` device: the HOSTOWNER redeem is console-attach-gated (CORVUS-DESIGN.md §5.5.1 / C-21); the A-4a clearance redeem is gated corvus-side (AUTH before the grant is ever registered; `CAP_GRANT_CLEARANCE` is corvus-only) and deliberately NOT console-gated (`devcap.c` clearance leg) | Syscall gate (`rfork` strip); `cap` device redemption (per-kind gates above) | `handles.tla` |
| I-3 | Mount points form a DAG, never a cycle | Kernel mount validation | `territory.tla` |
| I-4 | Handles transfer between processes only via 9P sessions | Syscall surface (no direct-transfer syscall exists) | `handles.tla` |
| I-5 | `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA`, `KObj_Loom`, `KObj_PCI` cannot be transferred | Transfer syscall has no code path; static_assert | `handles.tla` |
| I-6 | Handle rights monotonically reduce on transfer | Syscall-level check | `handles.tla` |
| I-7 | BURROW pages live until last handle closed AND last mapping unmapped | Refcount; runtime check | `burrow.tla` |
| I-8 | Every runnable thread eventually runs | Monotonic-`vd_t` ordered dispatch + timer-driven preempt + WFI/IPI idle wake (as-built; the full weighted-EEVDF math is deferred — RW-2 2A-F6, RW-13 reconcile) | `scheduler.tla` (liveness cfgs) |
| I-9 | No wakeup is lost between wait-condition check and sleep — *including* the death-wake that group-termination delivers to every rendez sleeper (§8.8.1, task #811), **frame-atomic for the elected 9P reader recv** so a mid-frame death defers its `*_INTR` unwind to the next frame boundary (block-through, no shared-stream desync; §8.8.1.1, #90), and the terminate-disposition `interrupt` wake that generalizes that same machinery (§8.8.2, LS-5); and the LS-8a pollable-cons *deferred* poll-wake — the RX IRQ relays through `poll_wake_pending` + the `console_mgr` kthread to the poll-hook walk (§23.5); and the Weft-4 readiness-ring single-cache-line poke — netd's shared edge counter vs the guest's park, the store-buffer register-then-observe (the PUSH counterpart of `net_poll`'s elicited PULL) | Wait/wake protocol; register-then-observe under the per-condition lock (`torpor_lock` for `torpor`, the per-Thread `wait_lock` for `sleep` / `tsleep`, `g_cons.lock` for the cons poll relay) or — for the lock-free readiness poke — seq-cst on each side's store-then-load (the StoreLoad barrier; the Weft-6 wiring additionally serializes under the consumer's Rendez lock) | `scheduler.tla`, `poll.tla`, `cons_poll.tla` (the LS-8a deferred poll-wake, clean + liveness + buggy cfg), `net_poll.tla` (the elicited PULL relay), `weft_readiness.tla` (the Weft-4 readiness-ring poke, clean + liveness + `BUGGY_OBSERVE_BEFORE_ARM`), `tsleep.tla`, `death_wake.tla` (the death-wake generalization, clean + buggy cfg), `reader_frame.tla` (the #90 frame-atomic reader-recv refinement, model-first: NoDesync + EventuallyUnwinds + a mid-frame-unwind buggy cfg); the `torpor` wait-on-address leg is prose-validated per the 2026-05-23 spec-to-code suspension (no `futex.tla` was written) |
| I-10 | Per-9P-session tag uniqueness | Per-session tag pool: tag value == outstanding-table index, reserved while active; a tag is not reusable until its reply lands — or, post-abandon, until its `Rflush` (#845 `awaiting_flush`) | `9p_client.tla` |
| I-11 | Per-9P-session fid identity is stable for fid's open lifetime | Per-session fid table | `9p_client.tla` |
| I-12 | W^X: every page is writable XOR executable | PTE bit check (`pte_violates_wxe` + mint-site asserts) + ELF loader W+X-segment rejection + structurally: no prot-mutation syscall exists and every anonymous-memory mint is RW-only (stronger than an "mprotect rejection" — there is nothing to call); the W1.5 patcher writes `.text` only via a transient RW+XN alias | runtime + `_Static_assert` |
| I-13 | Kernel-userspace isolation: TTBR0 / TTBR1 split | Page table setup | runtime |
| I-14 | Storage integrity: every block from Stratum is integrity-verified | Stratum's responsibility (Merkle layer); OS observes via 9P | (Stratum-side spec) |
| I-15 | Hardware view derives entirely from DTB | No compile-time hardware constants outside `arch/arm64/` (the two documented QEMU-virt fallbacks — the PL011 base + its INTID 33 — are the argued Lazarus-seam exceptions, `uart.c`/`uart.h`) | code review + audit |
| I-16 | KASLR randomizes kernel image base at boot | Boot init randomizes TTBR1 base (never-zero slide) | runtime + the `/ctl/kernel-base` leaf (devctl) |
| I-17 | EEVDF latency bound: delay between runnable and running ≤ slice_size × N | DESIGN TARGET, not yet enforced as stated: the quantitative bound needs the full EEVDF deadline math, which is deferred (RW-2 2A-F6 → RW-13). As-built the eventual-progress form holds (ordered dispatch + timer preempt; `scheduler.tla` `LatencyBound` proves *eventually runs*, i.e. I-8's property, not the quantitative bound) | `scheduler.tla` (qualitative liveness only) |
| I-18 | IPIs from CPU A to CPU B are processed in send order | GIC SGI ordering | `scheduler.tla` |
| I-19 | Note delivery preserves causal order within a process; every posted non-`kill` note is consumed exactly once across the handler + fd-read paths; `kill` is non-catchable; an uncaught `interrupt` (no handler, not masked, on a non-self-managing Proc) default-terminates the Proc — the SIGINT-shaped per-note disposition (§8.8.2, LS-5) | Per-Proc `NoteQueue` under lock; EL0-return-tail dispatch; `devnotes` Dev; the self-managing gate = notes-fd-open. Sub-invariants N-1..N-5 enumerated in §7.6.7 | (`notes.tla` planned then dropped per CLAUDE.md spec-to-code suspension; prose + audit + tests) |
| I-20 | PTY master ↔ slave atomicity | **ENFORCED (PTY-1 + PTY-2 as-built, 2026-07-18).** The statement (PTY-DESIGN §6): (a) every byte written to one end appears at the other's read exactly once, in order, transformed by the ldisc — none lost/torn/duped; (b) a cooked signal-class control char raises exactly one signal to exactly the controlling terminal's fg pgrp, exactly once, and (ISIG set) is NOT also a byte; (c) no wake lost between a write and a blocked peer read; (d) teardown delivers every in-flight byte, then EOF, strands no reader; master-close raises HUP exactly once. **Realization**: the *stop leg* + the signal routing are KERNEL (PTY-1: sessions/pgrps + the pts registry + `SYS_TTY_SIGNAL` → ct_sid → fg_pgid + the `job_stop_req` stop owner — `pty_stop.tla`); the *data path* is the USERSPACE `ptyfs` (PTY-2: the rings + the per-pts ldisc + the ctl + teardown — `pty.tla`'s actions map to `usr/ptyfs/src/server.rs` per SPEC-TO-CODE; HupAtMostOnce holds BY CONSTRUCTION — one master fd per pts can ever exist; the ptyfs single-threadedness carries (c) via `poll_reads`-before-park). The 2e `pty-probe` openpty E2E proves (b)+(d) live end-to-end: Ctrl-C → "interrupt" on a real controlling session's notes fd, winsize → `tty:winch`, master-close → `tty:hup`, plus the parked deferred master read. (LS-8a's pollable-cons deferred poll-wake stays under I-9: `cons_poll.tla`, not this row.) | `specs/pty.tla` (clean + liveness + the buggy cfgs `signal_also_byte` / `lost_teardown_byte` / `double_stop` / `signal_wrong_pgrp`) + `specs/pty_stop.tla` (clean + liveness + `double_stop` / `death_blocked`) + the §25.4 PTY-1 + ptyfs rows + `docs/reference/135-pty-kernel.md` + `136-ptyfs.md` + the PTY-1g + PTY-2e focused audits + the selftest/probe/E2E + the SMP gate |
| I-21 | Kernel executes uniformly at EL1h (`SPSel=1`); `SP_EL0` is exclusively the userspace stack | Boot sets `SPSel=1` and never lowers it; per-thread kernel stack carries exception frames; `test_smp` asserts `SPSel==1` | `sched_ctxsw.tla` |
| I-22 | No identity carries ambient super-authority: there is no superuser identity; `hostowner` is an authority *source* (the system key), not an identity; elevated power is acquired only via the legate (scoped, audited, ephemeral) | Syscall surface grants no authority by identity alone; capabilities are explicit (grant or legate activation) via the `cap` device + clearance-level policy | `IDENTITY-DESIGN.md §3.3/§8.2` (prose + audit + tests per the spec-to-code suspension) |
| I-23 | A service's filesystem authority is bounded by the storage capability it is handed: it reaches only that subtree, at only the handle's rights R, with no ambient FS authority beyond it; authority is monotonic (any delegate is `<=` R + same subtree). `RIGHT_TRANSFER` is withheld at grant as least-authority hardening (reserved for the Phase-5+ 9P-transfer surface; does NOT gate spawn-endow at v1.0, so intra-grant delegation is possible + sound). Confinement at v1.0 is cooperative (the service chroots to the capability as its first action; spawner-set-root is the v1.x form). | Spawner endows a `handle_dup`-reduced (`R|W`) Spoor at spawn; service chroots to it + confines all FS ops to it; a composition of I-2/I-4/I-6 (handles) + I-1/I-3 (territory) | `ARCHITECTURE.md §3.6` + `docs/detour-status.md A-1.7` (prose + audit + tests per the spec-to-code suspension) |
| I-24 | Group termination is atomic + exactly-once: when a Proc group-terminates (`exit_group` / `kill` / [v1.x: fault]), every Thread of the Proc eventually reaches `THREAD_EXITING` and the Proc transitions to ZOMBIE **exactly once** with the group exit status; no Thread executes at EL0 after its Proc's ZOMBIE transition; the cross-thread shootdown loses no wakeup (the sleeper-wake reuses the I-9 register-then-observe discipline under the per-condition lock — `torpor_lock` for `torpor`, the per-Thread `wait_lock` for all other rendez sleeps (§8.8.1, task #811); lock order `g_proc_table_lock → wait_lock → r->lock`) | Single per-Proc set-once `group_exit_msg` (NULL-sentinel CAS — *is* both the die-flag and the last-out status); `proc_group_terminate` publishes it, then `torpor_wake_all_for_proc` + the universal death-interruptible-sleep wake (§8.8.1, task #811) + broadcast `smp_resched_others`; `el0_return_die_check` at every EL0-return tail (sync-from-EL0 + the IRQ-from-EL0 tail, `#713`-safe); last-Thread-out ZOMBIE reap; `wait_pid` multi-Thread reap (`on_cpu`-spin per #788) | `specs/death_wake.tla` (clean cfg: no-lost-death-wake + exactly-once-ZOMBIE / no-EL0-after-ZOMBIE + the `EventuallyReaps` liveness witness; + the `BUGGY_OBSERVE_BEFORE_REGISTER` buggy cfg) + `ARCHITECTURE.md §7.9.1` prose + audit + tests |
| I-25 | A legate's elevated authority is bounded to its scope subtree and fully revoked on scope exit: clearance caps stamp on the legate root, attenuate to children by I-2, and are revoked (the `legate_scope_id` subtree torn down via group-terminate) on the legate root's exit OR `valid_until` expiry; no elevated Proc outlives the scope; the durable `principal_id` is unchanged by elevation (the legate is the same human, more authority) | `legate_scope_id` (rfork-inherited subtree tag) + `legate_valid_until` + `legate_session_id` on `struct Proc`; the `cap` device clearance grant/redeem (corvus registers via `CAP_GRANT_CLEARANCE`, the Proc redeems its own grant, kernel stamps `caps |= clearance ∩ self_restriction`); scope teardown reuses `proc_group_terminate` (#809/#811) at the EL0-return tail | `IDENTITY-DESIGN.md §3.1/§9.8` (prose + audit + tests per the spec-to-code suspension) |
| I-26 | Cross-process control is explicit + two-axis: a `kill`/`killgrp` write to `/proc/<pid>/ctl` is authorized only by owner-rwx on the ctl file (identity axis) OR `CAP_HOSTOWNER` OR `CAP_KILL` (capability axis) -- no identity carries ambient kill authority (composes I-22); containment is namespace visibility (I-1) | `devproc.stat_native` reports the target's `principal_id`/`primary_gid`/`0600`; the two-axis check (owner -- same `principal_id` on the `0600` ctl -- OR `caller` holds `CAP_HOSTOWNER` OR `CAP_KILL`; computed DIRECTLY in `devproc_write`, NOT via the `perm_check` DAC-override, so `CAP_DAC_OVERRIDE` is deliberately NOT a kill axis -- the A-4 split keeps fs-admin orthogonal to process-kill) runs at the WRITE site (`perm_enforced = false`; the SHARED open chokepoint hard-rejects pre-`devproc.open`, so the gate-at-open sketch could not host the `CAP_KILL` axis -- reconciled 2026-06-01, user vote); kill dispatches via `proc_group_terminate` uniformly under `g_proc_table_lock` via the `proc_for_each` resolve+authorize idiom (the `#811` wake-total primitive) | `IDENTITY-DESIGN.md §9.8` + `ARCHITECTURE.md §7.6` (prose + audit + tests per the spec-to-code suspension) |
| I-27 | Trusted path -- the elevation prompt is unspoofable: after a kernel SAK, the console-attach bit is held only by corvus / the trusted login Proc, and only the console-attach holder may redeem `CAP_HOSTOWNER` / a high-stakes clearance, so an interposer that drew a fake prompt cannot complete an elevation; the SAK keystroke is recognized in the kernel UART RX path before EL0 delivery | SAK = serial BREAK (a line condition, not data -> unforgeable by EL0 bytes; stateless recognizer, so "cannot be starved/spoofed by crafted input" is structural); recognized in the RX IRQ handler pre-EL0; the IRQ handler is wakeup-only (`notes_post`/`poll_waiter_list_wake` are not IRQ-safe), deferring the privileged action to a `console_mgr` kproc kthread (process context). **Console-ATTACH and console-OWNER are distinct roles (RW-7 R2-F1, `@2608c88`)**: ATTACH (`PROC_FLAG_CONSOLE_ATTACHED`, revoked atomically via `proc_revoke_console_attached`) gates SAK/elevation redemption — the `cap` device redeem gate keys on it (devcap.c); OWNER (`g_console_owner` under `g_proc_table_lock`, cleared by `exits()` on owner-exit) is the `interrupt` (Ctrl-C) target, conferred at session spawn via `SPAWN_PERM_CONSOLE_OWNER`. The SAK transition revokes ATTACH from the live holder, sets `g_console_owner = NULL` (corvus is never a Ctrl-C target), and re-grants ATTACH-only to the trusted Proc (`g_console_trusted_proc`); FAIL-SAFE revoke-only if none is alive. **Namespace front-door (#57b)**: the console is also reachable at `/dev/cons` (the `devdev` aggregating Dev, §9.4); `devdev.open` enforces the SAME `proc_is_console_attached` gate as `SYS_CONSOLE_OPEN` for the `cons`/`consctl` qids, so binding `/dev/cons` as a walkable path adds NO ungated front door -- a non-attached caller resolves the name but `open` fails -1. For **cons** the gate-at-open is ALSO re-checked at every I/O site (read/write/poll), closing the O_PATH-skips-open bypass on the single-reader console input. For **consctl** (#94-B) the I/O re-gate is intentionally DROPPED: a non-console-attached but deliberately-delegated Proc (login / the session shell) sets the line discipline via an INHERITED consctl fd -- the open-mint gate + `CWALKONLY`/#81 keep a consctl fd inside the trusted chain (joey opens it console-attached, hands it down), the inherited fd is the capability, and consctl is a control surface (the 5 mode flags) that can never read console INPUT (no keystroke exfiltration). The console-ATTACH/SAK gate is untouched. **The RENDERER third role (Tapestry G-4, the section-18.12 R2-F6 split)**: `SPAWN_PERM_CONSOLE_RENDERER` designates the single bound console renderer (Aurora), which holds exactly the `/dev/consdrain` + `/dev/consfeed` pair -- console output mirrors to its drain (on serial-bearing media as a TEE; the serial trusted path is unaffected), and its keyboard bytes enter the LS-8 line discipline via the feed with `is_break` hardwired false (no SAK forgery, structural). The role confers NO elevation authority (not ATTACH -- the SAK/redeem gates are untouched) and NO interrupt-target authority (not OWNER); grant is console-attach-only + single-holder. **Medium-independence (`TRUSTED-PATH.md`, scripture 2026-06-15)**: the trusted path is *generalized off the serial substrate* -- the same SAK + ATTACH-to-corvus structure (revoke-attach / `owner = NULL` / attach-corvus, NEVER own-corvus) rides any renderer (serial / Aurora / Halcyon). On a framebuffer, corvus emits a medium-INDEPENDENT cell grid that a kernel **trusted sink** rasterizes (the sole painter; the renderer fully suspended, mapping no framebuffer; the indicator kernel-drawn + unforgeable), and the graphical SAK is a kernel-scanned key-combo delivered via the MENAGERIE trusted-tier (never third-party) keyboard. Production posture: framebuffer-only trusted path; serial interactive/trusted-path OFF by default (a per-image BSP flag), output crash-only. The framebuffer enforcement (the trusted sink shared with a graphical Halls dump; the key-combo scan; the renderer suspension; the posture gating) is *reserved-then-enforced* -- it lands at impl with the Aurora renderer + the MENAGERIE board input path; the serial path is live today (A-4c). No new §28 number -- I-27 generalizes | `IDENTITY-DESIGN.md §9.8` + `ARCHITECTURE.md §17.1` + `§9.4` + `docs/TRUSTED-PATH.md` (prose + audit + tests per the spec-to-code suspension) |
| I-28 | Pathname resolution is contained + identity-checked: `stalk` resolves a path component-by-component from the caller's Territory `root_spoor`; `..` can never resolve above that root (the chroot/pivot boundary); every directory hop is gated by a per-component X-search (`perm_check(p, &st, PERM_X)`) on a `perm_enforced` Dev (final hop = `perm_want_for_omode`); mount-crossing (`domount`) is keyed by the full mount-point Spoor identity `(dc, devno, qid.path)` and reads only the caller's per-Territory mount table (composes I-1); every intermediate Spoor on the resolution **trail** is released except the returned **quarry** (no UAF / no leak) | `stalk` + `cross_mounts` (the in-call `trail` for `..` containment + cleanup, bounded at `root_spoor`); `PgrpMount` re-keyed to the `(dc, devno, qid.path)` mount-point identity (the `devno` axis distinguishes concurrent dev9p sessions); per-component `perm_check` / `spoor_stat_native` (the single-hop walk-open generalized to N hops); one-component-per-`Dev.walk` at v1.0 | `ARCHITECTURE.md §9.6.7` + `docs/STALK-DESIGN.md` (prose + audit + tests per the spec-to-code suspension) |
| I-29 | Loom completion integrity: every SQE the kernel admits produces **exactly one** terminal CQE (no lost, no double); no CQE is posted whose `user_data` correlation is **stale** (a torn-down / abandoned op never surfaces as a live completion); the CQ is **never overfilled** (back-pressure holds the completion until userspace drains a slot -- the F3/F5 9P-client all-or-nothing discipline); and on a **session death** the terminal CQE faithfully reflects the **death reason** -- a backing-device-gone session (a clean peer-gone EOF, the server/driver endpoint vanished) completes its in-flight ops with the device-gone `-T_E_NODEV` terminal, distinct from a generic transport `-T_E_IO` (the MENAGERIE §10 extension: a vanished device must complete, never hang, and be *distinguishable*) | The SQ/CQ ring state machine: the demux posts at most one CQE per in-flight op + clears its `inflight[]` slot; teardown quiesces every in-flight op (the #811 death-interruptible unwind) before freeing the ring Burrow; `PostCqe` is gated on a free CQ slot; `client_mark_dead_locked(c, devgone)` threads the reason (a recv `0` = peer-gone -> device-gone; a recv error -> transport) to each in-flight async op's terminal | `loom.tla` (clean + the `double_post` / `lost_on_full` / `stale_after_teardown` buggy cfgs); GENERALIZED to a stream in `loom_multishot.tla` (`ExactlyOneTerminal` + `TerminalEndsStream` + per-shot `CqNeverOverfull` + `NoStaleAfterTeardown`) + to a cancellation-complete chain in `loom_order.tla` (`EveryDoneOpPosted` + `EverySubmittedPosts`) + to the device-gone terminal in `loom_devgone.tla` (`DeathResultFaithful` + `SessionDeathCompletes` + `NoDoubleTerminal`) |
| I-30 | Loom submit-time capability pin: the object + rights governing a Loom op are resolved + snapshotted at **submit** (object refcount held) and held for the op's lifetime; completion **never** re-resolves the handle (which races a `clunk` + slot-reuse -- the io_uring credential-vs-work CVE class). The kernel copies every SQE field to kernel memory before validating/acting (ring TOCTOU) -- never re-reads a shared-ring field after the check | The registered-handle table resolves each handle once (the #844 by-value snapshot) + the ring holds its own `spoor_ref`; the dispatch acts on the snapshot, never re-reads the registered slot at completion | `loom.tla` (clean + the `live_sqe_reread` / `recheck_at_completion` buggy cfgs) |
| I-31 | ASID rollover safety: no two CPUs concurrently run user address spaces that share an ASID within the same generation (else the TLB returns a wrong translation -> cross-Proc memory corruption); a generation rollover never reassigns an ASID `active` or `reserved` on any CPU (a running CPU is never yanked); every context switch installs a valid current-generation ASID before the TTBR0 write | Global `asid_generation` (atomic) + per-Proc `context_id` (generation+asid); fast path reuses on generation-match via an `xchg` into the per-CPU `active_asids[cpu]` (publishes liveness, lockless); slow path (`new_context` under `g_asid_lock`) claims a free ASID or rolls the generation (reset bitmap, set per-CPU `flush_pending`, preserve each CPU's active ASID into `reserved_asids[cpu]`); per-CPU local TLB flush on the `flush_pending` bit at the next switch (ARCH §6.2.1) | `specs/asid.tla` (clean + the 5 buggy cfgs: `rollover_steals_active` / `fast_no_regen` / `fast_no_flush_check` / `no_flush_pending` / `reserve_value_only`) + prose in ARCH §6.2.1 + the focused audit |
| I-32 | Per-Proc resource floor (DoS bound): a non-TCB Proc's live anonymous pages, live threads, live direct children, **and live VMAs** are each bounded by a fixed maximum (`PROC_PAGE_MAX` / `PROC_THREAD_MAX` / `PROC_CHILD_MAX` / **`PROC_VMA_MAX`**); on reaching a bound, creation fails with a clean errno (`-ENOMEM` / `-EAGAIN` / `-1`), NEVER a kernel extinction. **Overcommit (the LAZY anon model, §6.5): a `SYS_BURROW_ATTACH_LAZY` reservation charges `page_count` at FAULT time (per committed page), so the count tracks true RSS and an over-cap commit graceful-OOM-terminates at the fault; `PROC_VMA_MAX` (the Linux `vm.max_map_count` analog) bounds the reservation-bomb that a free (uncharged-at-attach) lazy reservation would otherwise open — it is a no-op for the eager path (already transitively VMA-bounded, each VMA charging ≥1 page) and the real bound for lazy. `SYS_BURROW_DECOMMIT` uncharges.** -- so a fork / thread / memory bomb is bounded, not box-killing. The TCB (`PRINCIPAL_SYSTEM` -- kproc + the boot/service chain) is exempt, and the exemption is **unforgeable** (no post-login Proc can acquire `PRINCIPAL_SYSTEM` -- `CAP_SET_IDENTITY` rejects it), so a capped user cannot escape. This is a resource axis, NOT a privilege axis: the exemption confers no capability and does not touch I-22. **Graceful-OOM on every user creation path is the backstop** that bounds a recursive (cross-Proc) bomb at the physical-memory cliff | Per-Proc counters: `page_count` (under `vma_lock`), `child_count` + `thread_count` (under `g_proc_table_lock`); cap checks at `sys_burrow_attach_for_proc` (page) / `sys_thread_spawn_handler` (thread) / `rfork_internal` (child); `proc_resource_exempt(p)` == `principal_id == PRINCIPAL_SYSTEM`; graceful-OOM verified across `proc_alloc` / `thread_create` / `territory_clone` / `burrow_create_anon` + the demand-page `mmu_install_user_pte` failure -> `proc_fault_terminate` path. Page scope = the repeatable user anon vectors -- SYS_BURROW_ATTACH regions + the SYS_LOOM_SETUP ring (audit F1); pgtable / kstack / exec-image are transitively or one-shot bounded. The global / per-user aggregate quota (cgroup-equivalent) is the recorded SEAM that reads these counters | prose in IDENTITY-DESIGN.md §3.8 + `docs/reference/110-resource.md` + the focused audit + runtime tests (cap-hit per axis + the unforgeable-exemption + graceful-OOM-no-extinction + counter charge/uncharge balance) |
| I-33 | Namespace name retention is non-load-bearing: every `struct Spoor` carries a refcounted copy-on-walk `Path` (the cleaned namespace name it was reached by — the Plan 9 `Chan.path`), but the resolver is **write-only** to it. `stalk` / the single-hop walk / create *append* to a Path; resolution, the per-component X-search, mount-crossing, and every `perm_check` consult only `(dc, devno, qid.path)` + `stat_native`, NEVER `->path`. So a wrong / stale / truncated / absent / failed-to-allocate Path can change ONLY the cosmetic content of the three introspection readers (`SYS_FD2PATH`, `/proc/<pid>/fd`, `/proc/<pid>/ns`), never a resolution outcome, a permission decision, or any other syscall result; a path-alloc failure never fails a walk (the walk succeeds, the Path is left NULL = "unknown"). Path storage lifetime is **subordinate** to its Spoor's: each referencing Spoor holds exactly one Path ref for its whole life (NULL at `spoor_alloc`, shared via incref at `spoor_clone`, dropped at `spoor_free_internal`), so a Path frees exactly with its last Spoor — no new lifetime axis beyond the audited Spoor refcount. The Path string is immutable once built (`addelem` always allocates a fresh Path, never mutates a shared one), so the only concurrently-mutated field is the atomic `path->ref` | `struct Path { int ref; u32 len; char s[]; }` (atomic `ref`, immutable `s`); `spoor.c` (`spoor_clone` share / `spoor_free_internal` drop / `path_addelem` COW / `path_ref` / `path_unref`); accumulation at the 3 hook sites (`stalk` step + `stalk_cross_mounts` transplant + base seed; `sys_walk_open_handler`; `sys_walk_create_handler`); `/` seeded at the root-filesystem Dev attach (`devramfs_attach` + `dev9p_attach_client`) at Spoor birth -> immutable after (so chroot/pivot never re-stamp a published Spoor); the field is set-before-publish / read-after (no Path-field lock, like `qid` / `dev`); bounded by `SYS_OPEN_PATH_MAX` | prose in ARCH §9.6.9 + `docs/reference/30-dev-spoor.md` (the Path section) + `docs/STALK-DESIGN.md` + the focused audit + runtime tests (accumulate-across-hops + cross-transplant + `..`-pop + OOM-leaves-NULL-walk-succeeds + clone-shares + the refcount balance) + the SMP gate |
| I-34 | Driver authority bound: a driver's hardware authority is exactly its warden-granted **allowance** — a per-Proc set of permitted MMIO PA windows / IRQ INTIDs / a DMA per-buffer cap / **PCI `(bus,dev,fn)` functions** (the fourth axis, build-arc step 6a). A **narrowed** allowance (`p->allowance != NULL`) bounds `SYS_MMIO/IRQ/DMA_CREATE` **and `SYS_PCI_CLAIM`** (gated on the resolved `(bus,dev,fn)`) to its conferred set; a **broad** Proc (`allowance == NULL` — the warden + the existing trusted servers) is bounded only by the I-5 kernel reservation (the as-built v1.0 behavior, unchanged). The allowance is never widened (the conferred windows are immutable after confer; a forked child inherits an equally-narrow copy — the hardware-axis analog of caps' monotonic reduction, I-2) and fully revoked on unbind/removal/crash (`proc_revoke_allowance` closes the gate + `proc_group_terminate` drops the handles). A narrowed driver also **cannot spawn a child Proc** (drivers are leaves — MENAGERIE §13.2 "sources, not spawners; one auditable chokepoint"; `rfork_internal` fail-closed denies a narrowed parent a child, 5e-4 F2), so no hw-capable grandchild can inherit a clone — or be conferred a subset — of the allowance and **survive** the per-Proc revoke + thread-group-scoped terminate. The central hazard — an in-flight `SYS_*_CREATE` racing a `DeviceRemoved` revocation — is closed by the **two-step create**: the gate check (`allowance_permits`, lock-free) then the install under an allowance-`revoked` re-check (`allowance_handle_alloc`) UNDER the lock `proc_revoke_allowance` takes, so the in-flight create aborts rather than slipping a handle through a being-revoked allowance. The I-25 (legate scope) analog for hardware; generalizes pci-1b (a PCI device's allowance IS its claimed BARs — the per-`(bus,dev,fn)` PCI axis enforced at `SYS_PCI_CLAIM` since build-arc step 6a, resolving the `virtio_device_id` to its `(bus,dev,fn)` read-only [`kobj_pci_resolve_bdf`, the same first match the claim picks] then gating `HW_RES_PCI`). The fourth leg — the grant being a subset of the bound node — is the warden's grant policy (node ∩ manifest; MENAGERIE §11), prose-audited | Per-Proc `struct Allowance *allowance` (NULL = broad); `allowance_permits` (CreateBegin: lock-free read of the immutable windows + the atomic `revoked`) at `sys_{mmio,irq,dma}_create_handler` + `sys_pci_claim_handler` (`kernel/syscall.c`); `allowance_handle_alloc` (CreateCommit: re-check `revoked` under `allowance->lock` then `handle_alloc`; lock order `allowance->lock` → handle-table lock, both spinlock-only/non-sleeping); `proc_confer_allowance` (the warden's set-once-at-spawn grant) / `proc_revoke_allowance` (DeviceRemoved) / `allowance_clone_into` (rfork inherit) / `allowance_free` (`kernel/allowance.c`) | `specs/allowance.tla` (clean cfg TLC-green: HandlesWithinAllowance + AllowanceWithinConferred + ConferredWithinNode + RevokedFullyCleared + the `EventuallyResolves` liveness witness; + the 4 buggy cfgs `revoke_race` [the headline SMP race] / `revoke_leak` / `confer_widen` / `self_widen`) + prose in MENAGERIE.md §4 + `docs/reference/117-allowance.md` + the focused audit + runtime tests (`allowance.*`, incl. `handle_alloc_revoked_aborts` = the race regression) + the SMP gate |
| I-35 | **Mandate attenuation + revocation** (persistent attenuated delegation — the *mandatum*, `docs/MANDATE-DESIGN.md`): a corvus-issued, censor-managed **standing** grant of a SUBSET of the issuer's authority to a user grants <= its issuer's held authority (the I-2 analog for *persistent* delegation — never widened, never a domain the issuer does not govern); is revoked by deletion OR a per-domain key-generation bump (a stale-`key_gen` mandate is NEVER installed at redemption — "invalid when the capability key changes"); revoke/extend requires `actor.clearance >= mandate.issuer_level` within the domain (system foundings sacrosanct to a normal censor); the subject's durable identity is unchanged (a mandate adds scoped authority, never a new principal); a network mandate composes I-1/I-28 (the `/net` view IS the firewall). The **floor IS a mandate**; imperium (self-elevation) is orthogonal. **v1 namespace-only (no kernel lift)**; cap-resource-scope is v1.x (a per-Proc scoped-cap table). | corvus **album** (`/var/lib/corvus/album/<subject>`, sole writer) + per-domain clearance keys/generations; the `censor` (= the user-admin clearance) issue/extend/revoke gate (I-2 + the issuer-clearance rule); login-time silent **redemption** (filter valid + install the narrowed `/net`/FS views) + the **netd** per-principal `/net` enforcement (the NET-DESIGN §8 `/net/filter` seam); manage-clearance key-rotation = mass revoke. No local crypto (gen-counter + corvus-owns-album + `stripes`). | `specs/mandate.tla` (spec-first, re-enabling point (a); clean + the buggy cfgs `stale_install` / `over_attenuate` / `revoke_above_clearance` — the rotate-vs-install race) + prose `docs/MANDATE-DESIGN.md` + the post-net **MA** arc focused audit + tests + the SMP gate. OWED at the MA build (Phase 8, after net). |
| I-36 | **File-backed demand-paged exec soundness** (REVENANT, `docs/EXEC-LOAD-DESIGN.md`): the kernel may demand-page an executable's read-only segments (text; + R-only rodata since #45, EXEC-LOAD-DESIGN §4.6) from the FS (the reserved `BURROW_TYPE_FILE`) — kernel-internal, never a userspace writable file mapping (§6.5) — and that path is sound iff seven conditions hold jointly: **(1)** the backing identity is an **immutable** content-addressed snapshot (a pinned `qid.version`), never a mutable path — so the file cannot change under a running binary (kills the torn-write/`SIGBUS` class); **(2)** every demand-paged page is **integrity-verified** (Merkle-checked) before install — a hostile/buggy FS server cannot inject corrupt or attacker-chosen text (closes the Hurd-CVE class); **(3)** the mapping is **read-only, rights-reduced, capability-mediated** (a Burrow over a kernel-pinned Spoor with monotonically-reduced rights, I-2/I-5/I-6), never a raw pager/server handle, and W^X holds (I-12: text is `R+X`, rodata `R`-only/XN, never writable); **(4)** writable `.data`/`.bss` terminates in **private anonymous** memory (eager-copy at v1.0; anon-COW at v1.x), never a file-backed writable mapping and never written back to the snapshot; **(5)** the page-in fault is **death-interruptible** (#811/I-9) — a dead/wedged FS server turns the in-flight fault into a per-Proc terminate (`proc_fault_terminate`), NEVER a wedged thread and NEVER a box extinction (the condition stock Mach lacked); **(6)** a page-in I/O error is **bounded + fail-closed** (`snare:bus`-class per-Proc terminate, attributable to page+Spoor; never a silent zero-fill of executable text; I-14 ecode-bounding extends); **(7)** file-backed pages are **resource-accounted** (charged vs I-32 like anon; shared read-only text charged once via the I-7 dual-refcount). Shared read-only text is deduped across Procs by the qid-keyed **Image cache** (file-identity-keyed — the Plan 9 `Image`; this is NOT content-scanning KSM, which is declined as ASLR-defeating). Conditions 1-2 are free from Stratum's content-addressed Merkle FS; 3-4-7 are existing mechanisms; 5-6 are the genuinely-new bits, and the hard primitive (5) already exists | `BURROW_TYPE_FILE` Burrow (kernel-pinned `Spoor` + base offset + sampled qid + the #847 dual-refcount); the `userland_demand_page` `BURROW_TYPE_FILE` arm (`dev->read` one page -> Image-cache lookup -> install at the VMA prot [`R+X` text / `R`-only rodata, #45]; death-interruptible via the per-Thread `wait_lock`; `snare:bus` fail-close); the qid-keyed reclaimable `Image` cache; `exec_load_from_namespace`/`exec_setup` reworked to parse the header + map text file-backed + eager-copy data into anon (the slurp + `SYS_SPAWN_BLOB_MAX` 1-MiB whole-ELF cap retired -> a header-sanity bound) | prose in `docs/EXEC-LOAD-DESIGN.md` + ARCH §6.5 + the REVENANT impl-arc focused audit (the three audit-trigger surfaces: exec / W^X / demand-page) + tests (spawn a >1 MiB binary; cross-Proc text sharing; a stalled-FS page-in is death-interruptible not wedged; a page-in I/O error fail-closes per-Proc; W^X on text) + the SMP gate. AS-BUILT (REVENANT R-1..R-5, CLOSED CLEAN at R-5 [two independent prosecutors, different P1s]; + the #45 rodata addendum + the read-ahead cluster — the §25.4 REVENANT row is the authoritative close record) |
| I-37 | **Capability network dataplane integrity** (Weft, `docs/NET-THROUGHPUT.md`; designed, OWED at the Weft build): a per-flow zero-copy network dataplane — a Burrow shared guest↔netd carrying a flow's Tread/Twrite PAYLOAD in place (the 9P control messages stay ordinary frames; small/control payloads keep the byte-copy ring — the virtio-9p hybrid threshold) — is sound iff: **(1)** the access bound is enforced ENTIRELY at setup/grant time (the flow capability = the `/net/<proto>/N/data` fid's I-30 submit-pin + the Burrow registration), NEVER per-packet, and netd is OUT of the per-packet path (no software toucher re-enters — the reviewer attack: each flow has its own ring + its own smoltcp socket, so there is no shared queue to police); **(2)** the shared payload page's lifetime is correct under the MULTI-HOLDER completion — a registered buffer is not reused/freed until the LAST of {netd stack done, NIC DMA done, peer ACK} releases it (the two-CQE `F_MORE`/`F_NOTIF` send contract; the I-30 pin released at notification-terminal, not op-terminal) → no in-flight-page UAF / cross-Proc corruption; **(3)** a confined Proc reaches only the flows its namespace grants and the ring is per-flow (I-1/I-28), and netd owns the NIC (I-5 — the ring is the only data path, never raw hardware); **(4)** the shared Burrow's lifetime is the #847 dual-refcount (I-7); **(5)** the descriptor ring is split-ring unidirectional (one writer per region) so SMP correctness holds without a per-op lock. GENERALIZES I-29/I-30 (Loom completion integrity + submit-time pin) to the cross-Proc shared buffer + the notification-terminal release | the per-flow shared Burrow (a cross-Proc Burrow-share surface over the #847 dual-refcount) + the virtio split-ring descriptor transport + the readiness ring (the Shenango single-cache-line poke, which also closes the RX-wake latency floor, NET-PERF N1) + the `F_NOTIF` two-CQE send completion; netd does the control-plane setup at grant, the bytes flow through the shared page with no per-op mediation (the Snap-transport + Arrakis-framing + RDMA-registration-is-the-capability fusion the literature confirms unoccupied) | `specs/weft.tla` (spec-first re-enabled, point (a); **LANDED Weft-1, TLC-green model-first**: clean [13 invariants, 1412 distinct, depth 22] + the `EventuallyReleased` liveness witness + the four buggy cfgs `premature_release` [the F_NOTIF UAF -> `PinHeldWhileInFlight`] / `recheck_per_op` [the reviewer-attack per-packet re-check, which FAILS the no-mediation invariant `NoPerOpMediation`] / `ring_toctou` [-> `DescPinnedToSnapshot`] / `share_outlives_flow` [-> `ShareBoundedByFlow`]) + prose in `docs/NET-THROUGHPUT.md` + `docs/NOVEL.md` (the Weft angle) + the Weft-arc focused audit (the buffer-lifetime UAF prosecution) + runtime tests + the SMP gate. OWED at the Weft build (post-net, before REVENANT). **Delivery model (grant-is-the-share, user-voted 2026-06-20):** opening the flow's `/net/<proto>/N/data` fid maps the per-flow shared Burrow into the guest -- NO Burrow handle crosses Procs (kernel-mediated; the #847 refs are kernel-internal; the capability is the namespace-gated flow fid, I-1/I-28 -- the Plan 9 mmap-the-server-file + Fuchsia IOBuffer RFC-0218 + seL4-coordinator convergent answer; *capability delegation* [a handle crosses] and an *explicit decoupled share-syscall pair* were rejected). The substrate (`burrow_share_into` + the #847 dual-refcount proven SMP-safe across two Procs) lands at **Weft-2** (no EL0 ABI -- the EL0 surface is flow-keyed because the `/net` SrvConn is shared across all flows); the EL0 surface (`SYS_WEFT_SHARE` = 81 / `SYS_WEFT_MAP` = 82, keyed on the data fid) lands at **Weft-6**, via the **lazy fid-keyed `Tweft(F)->Rweft(share_id)` correlation** (Option B, resolved 2026-06-20; NET-THROUGHPUT §6.1, superseding the earlier eager auto-map sketch): a flow issues `Tweft` only on first zero-copy use (small/control writes keep the byte-copy ring — the §4.8 hybrid threshold), the kernel-internal `share_id` (never handed to the guest → unforgeable) joins netd's `SYS_WEFT_SHARE` registration to the kernel's guest-side `burrow_share_into`, and the data-fid clunk drops both #847 refs (`ShareBoundedByFlow`). `weft.tla` stays unchanged (the `Tweft`/`share_id` setup is abstracted as `Init`; the consumed-once + no-cross-flow-mis-binding property is prose + Weft-7-audit validated — extend the spec first if 6a surfaces subtlety) |
| I-38 | **Larder cache coherence** (the guest-side FS cache, `docs/LARDER-DESIGN.md`; designed, OWED at the L1 build — the measured top lever of the on-device `go build`): a per-session `p9_client` cache of FS attrs / dentries (incl. **negative**) / data pages, keyed by `qid.path`, is sound iff a hit returns exactly what a fresh RPC would under **close-to-open**: an entry is served only while its **content-version** (`cvers` — the Stratum `si_cvers` surfaced as `qid.version`, bumped on every content/metadata mutation, DECOUPLED from the lifecycle `si_gen` so a write never `ESTALE`s a live fid) matches the file's current version (validated at open — POUNCE's `walk_attrs` carries the fresh `cvers`, so revalidation is often free), is invalidated on own-write (write-through), and is NEVER served past its revalidation point — so a stale entry can never yield a wrong read / stat / resolution / permission decision; the cache is a per-session resource bounded by a fixed capacity. **The `cvers`-revalidation gate applies to the attr + page sub-caches (each entry validated by its OWN file's content-version). A dentry — the `(parent,name)→child` binding, positive OR negative — is NOT cvers-gated (L1d ground truth): Stratum surfaces no directory-content version that tracks a dirent change (verified in `src/fs/fs.c`, a child create/unlink touches only the separate dirent index and does not `stm_inode_set` the parent, so the parent `si_cvers` does not bump; only rename stamps it), so a name-binding's coherence is OWN-WRITE INVALIDATION ONLY — a guest create/rename/unlink drops that directory's cached dentries. Under single-writer this is absolute; it maps onto `fs_cache.tla`'s `Read`+`OwnWrite` (the attr sub-cache's discipline), no `Open` gate. An external writer's dirent change is bounded by LRU eviction, not by a cvers window (a directory-content version in Stratum, at a create/unlink parent-COW cost, is the v1.x tightening).** Composes **I-28** (the base X-check perm-checks a memoized-but-`cvers`-validated attr — the same unsynchronized-snapshot TOCTOU the resolver already accepts, now with a *bounded* staleness window on a tightened dir perm), **I-32** (LRU-bounded per session — no unbounded kernel-memory growth from a hostile workload), **I-10/I-11** (untouched at L1 — the Larder sits ABOVE the fid/tag layer). v1.0 = write-through + in-memory + single-session + close-to-open (writeback / persistent-on-disk / cross-session / readdir caching are v1.x seams). **FID-LIFECYCLE extension (`docs/FID-LIFECYCLE-DESIGN.md`, user-voted 2026-07-09; the measured 67%-of-the-warm-floor per-file fid overhead):** the Larder gains a **cached-open** fast path — a read-only open whose content is fully page-cached + fresh is served open+read+close from the Larder with a single **query-form Twalkgetattr** revalidation (the I-38 `Open` gate, binding NO server fid) and a no-op close (no Tlopen, no Tclunk), falling back byte-identically to the bind-walk+lopen path on any miss. This is the **fidless close-to-open open** — it PRESERVES I-38 (the query-getattr IS the revalidation; the open SNAPSHOTS the covered pages at the fresh `cvers` into a budgeted private per-open buffer -- atomic under the Larder lock vs a concurrent own-write invalidate -- and every read of the open serves the open-time content, exactly close-to-open; a fidless Spoor has no read-time-miss recovery, so live serves from the evictable/invalidatable page cache would risk a false EOF = silent corruption) while eliminating the fid RTs; the fully-loose 0-RT variant (B1, no revalidation) was REJECTED as the v1.0 default (weakens I-38 globally). **B1 PER-ATTACH OPT-IN (user-voted option B, 2026-07-11 — the CHASE wga band; `docs/chase/B1-VOTE.md`):** an attach-time `SYS_ATTACH_9P_LOOSE` flag marks that ONE client loose — a cached-open whose RPC-free hint FULLY hits (positive dentry chain + leaf attr + full page coverage) skips the per-open wire revalidation and snapshots at the CACHED cvers; any hint miss still takes the forced-wire query (first touch unchanged; every non-cached-open path unchanged). I-38's DEFAULT stays strict; the exemption is confined to attaches where the mounter has checked the single-writer premise: the pool's block device is exclusively guest-owned (I-5), the concurrent 9P sessions reach disjoint datasets (`--datasets-allowed` + `ds:<user>` partition), own-writes invalidate through the same Larder, and the one cross-session-writable subtree under the system mount (corvus's `/var/lib/corvus`, written via corvus's own byte-mode connection) has no system-mount byte reader (corvus is its DB's sole reader; a stat through the system mount was ALREADY cache-served under L1c's attr discipline). joey opts the SYSTEM mount in; the per-user home proxy may follow with its own (cleaner: strictly session-private dataset) argument. RE-STRICT TRIGGERS (any of these revokes the opt-in at its attach site): a second guest / host-side concurrent mounter, a shared-dataset multi-session config, a cross-session byte reader of a foreign-written subtree. The companion **async-clunk** (fire-and-forget Tclunk, ownerless Rclunk drain — the #845 discipline) composes **I-10** (the tag reserved until its Rclunk drains) + **I-11** (fid unbound at send; the monotonic fid number is never reused) with no new invariant. Both are spec-first (cached-open maps onto the existing `fs_cache.tla` `Open`+`Read`; async-clunk extends `9p_client.tla` with a tag-leak buggy cfg). **Guest-side populate rule (the L1a-2 audit-F1 disposition, ground-truth-corrected):** the Larder is populated ONLY from a `getattr`/`walk_attrs` qid (which carries a true `si_cvers`), NEVER from a `readdir` qid — Rreaddir's `qid.version` is a link-time `si_gen` snapshot stored in the dirent record (no content-version), so a readdir-sourced version would read backwards against a getattr-sourced `si_cvers` for the same inode. A v1.x readdir-listing cache needs a per-child content-version mechanism (a per-child revalidation, NOT a dirent-format snapshot [format break + stale] nor a per-child stat [a readdir-path perf regression]) | the per-`p9_client` Larder (attr `qid.path→{attr,cvers}` + dentry `(parent-qid,name)→{child-qid,type,cvers}` incl. negative + page `(qid,page)→{bytes,cvers}`) under a dedicated cache lock (a near-leaf; never held across a blocking op, #360); serve/populate/invalidate hooks at `dev9p_stat_native` / `dev9p_walk_attrs` / `dev9p_read` / `dev9p_write` + create/rename/unlink; the Stratum `si_cvers` (`le32` carved from `si_reserved[44]` → `[40]`, NO on-disk format break, `server.c:774` surfaces it as `qid.version`) | `specs/fs_cache.tla` (**LANDED L1b, spec-first re-enabled point (a)**; TLC-green: `fs_cache.cfg` [single-writer — NoWrongRead ABSOLUTE + NoStalePastRevalidation + Bounded + CacheNeverAhead] + `fs_cache_external.cfg` [the open-revalidation gate vs an out-of-band writer — NoStalePastRevalidation holds; NoWrongRead deliberately not checked, the accepted within-episode window] + `fs_cache_liveness.cfg` [WriteEventuallyVisible]; + 2 buggy cfgs each a minimal 4-state counterexample on its named invariant: `stale_serve` [Open skips the `cvers` check → NoStalePastRevalidation] + `no_invalidate` [own-write skips the write-through drop → NoWrongRead]) + the Stratum `inode.tla` extension (LANDED L1a-1: `si_cvers` bump via `ContentMutate`; `si_gen` decoupled; `CversUniqueAllTime`; + 2 buggy cfgs — reset-on-reuse, gen-bump-on-write) + prose in `docs/LARDER-DESIGN.md` + the L1-arc focused audit (the most-audited FS path: dev9p / 9p_client / stalk) + tests (serve / invalidate / own-write / close-to-open-revalidate / negative-dentry / the bound) + the SMP gate. AS-BUILT (the L1 arc COMPLETE at L1f: attr + dentry [incl. negative] + page sub-caches + the cacheability gate, CLOSED 0 P0 / 1 P1 fixed / 0 P2 / 2 P3, 40/40 SMP gate; extended by the F1 write-behind + the term-4 G1-G4 addenda — the §25.4 Larder row is the authoritative close record) |

| I-39 | **Debug authority bounded** (designed 2026-07-14, user-voted; AS-BUILT at 8a-1, 2026-07-15 — `docs/DEBUG-FS-DESIGN.md` + `docs/reference/134-debug-fs.md`): a Proc may debug a target iff it can *name* `/proc/<pid>` in its namespace AND passes the two-axis gate (**owner** — same `principal_id` on the `0600` ctl — **OR** the capability axis `CAP_HOSTOWNER`/`CAP_DEBUG`, the I-26-analog: owner OR the host owner OR the domain cap, exactly as kill is owner OR `CAP_HOSTOWNER`/`CAP_KILL`; `CAP_HOSTOWNER` is a debug axis per the Plan-9-eve reading, user-voted 2026-07-15, since the host owner already kills any target and debug is less invasive; `CAP_DAC_OVERRIDE` is deliberately NOT a debug axis); the debug surface confers no authority beyond that gate. User register/memory reads AND writes, and all execution control (stop/step/write), are permitted only on a **stopped** target; a read-only inspection of a **settled** (`on_cpu==false`) thread's **kernel** stack (`/proc/<pid>/kstack` — the Linux `/proc/<pid>/stack` diagnostic tier, 8b) is I-39-authorized but does NOT require a debug-stop — memory-safe (bounded to the thread's own kstack via `halls_walk_kernel_frames` + the `g_proc_table_lock` lifetime pin) + best-effort-consistent (coherent for a sleeping thread, garbage-but-bounded for a racing one), controlling no execution. No debug op (a) writes executable text (I-12 W^X holds — breakpoints are hardware, never a software `BRK`), (b) corrupts the shared REVENANT Image cache (I-36 holds — same reason; this is why the `dlv` backend must use the HW path, not Delve's default software breakpoints), or (c) reads/writes outside the target's own address space (the cross-Proc VA->PA walk resolves strictly through the *target's* `pgtable_root` under the target `vma_lock`; a non-resident lazy-anon/FILE page reads as not-resident, never fault-in on the debugger's behalf). A stop is bounded by an explicit revocable ownership — the open debug ctl fd — so a debugger's `detach`/close/**death** (handles close at exit since #68/#926) provably resumes the target; no debugger strands its quarry. **EXITKILL refinement (designed 2026-07-23, user-voted; the die-with-launcher `PTRACE_O_EXITKILL` analog):** the resume-on-release is NoStrand-correct for an *attached* target (it pre-existed the debugger — leave it running) but WRONG for a debugger-*launched* one — the Plan 9 resume orphans it to init to run forever (the HVF-idle debuggee leak). So a debugger marks a launched target `exitkill` (the ctl `exitkill` verb, right after attaching a child it spawned; the kernel `Proc.debug_exitkill`), and the ctl-fd-close release-cb `proc_group_terminate`s a marked target — whose #811 death cascade wakes the debug-parked threads to die at the die-check (death wins) — instead of `proc_debug_resume`. Only the *implicit* death-release honors the mark; an explicit `detach` still resumes (the debugger's deliberate choice — it would send `kill` to terminate). `kproc` never debuggable; session/console-owner + `PROC_FLAG_NOTRACE` seams honored. The I-26 analog for the read/write/control axis; composes I-1/I-12/I-13/I-22/I-36 + the #811/#68 death path (a group-terminated target DIES rather than resuming — the stop-check is ordered after `el0_return_die_check` at the EL0-return tail; **8c-2** adds a nested stop-detour INSIDE `sleep()`/`tsleep()` for a syscall-blocked sleeper [a multi-thread Go target's idle futex-parked M never reaches the tail on its own], but the detour is die-check-FIRST too, so a stop still never unwinds a syscall — it parks-and-reparks, preserving it, while a death unwinds it: death always wins; **8c-3** lets the elected 9P reader's recv *primitive* unwind on a stop [`Thread.stop_unwinds`] so it can release the shared reader role before parking — but the *syscall* is still preserved [the reader re-elects + re-blocks on resume], and the released role is handed to a runnable survivor [the handoff skips debug-stopped owners], so a stop never freezes the shared FS client) — adds no bypass. `CAP_DEBUG` = `1<<10`, elevation-only + clearance-grantable (rfork-stripped; corvus-gated legate; 3 header edits, 0 devcap.c) | The two-axis gate at each debug read/write site (`devctl_kernel_base_readable` template) + stopped-only mem/reg + HW breakpoints (I-12/I-36) + the target-`pgtable_root`-confined walk + the handle-lifetime-tied slot; the stop parks at the EL0-return tail (zero-lock, `preempt_count==0`) after the die-check; targeted `gic_send_ipi` stop delivery; the DBGB*/DBGW* per-thread install (8a-2) | `specs/debug_stop.tla` (spec-first RE-ENABLED, model-first: NoLostStop + DeathWinsOverStop + NoEL0AfterStopped + ExactlyOnceResume + the NoStrand liveness witness + `EventuallyLaunchedDies` [the EXITKILL refinement — a launched/`exitkill` target dies with its debugger] + 6 buggy cfgs incl. `exitkill_ignored`) + prose in `docs/DEBUG-FS-DESIGN.md` + the focused 8a-1/8a-2 audits + the SMP gate. AS-BUILT at 8a-1 (2026-07-15): the software-checkpoint tier landed + audited — the §25.4 row + `debug_stop.tla` TLC-green + the 8a-1c Fable-5-max holotype (0 P0 / 0 P1 / 1 P2 fixed [HF1, the group-terminating-target death gate, revert-probed regression] / 2 P3) + the in-guest `/debug-probe` E2E (boot-fatal; it caught the F1 seekable + F2 trapframe defects the synthetic tests were blind to) + the SMP gate (40/40, 0 corruption) + `docs/reference/134-debug-fs.md`. AS-BUILT at 8a-2 (2026-07-15): the HW tier landed — per-Proc `DBGB*/DBGW*` install on ctx-switch (the per-CPU-MDE isolation) + `MDSCR.SS` single-step (`specs/debug_step.tla` model-first) + watchpoints (`DBGWCR/DBGWVR`, EC 0x30/0x32/0x34); `debug_stop.tla` gained `StopImpliesOwned` + the `fault_stop_ungated` cfg (the SA-1 fix: a hardware fire delivers the stop via `proc_debug_fault_stop`, gated on `debug_owner` under `g_proc_table_lock`); the 8a-2c consolidated Fable holotype + the SMP gate + the in-guest `/hwbp-verify` + `/debug-probe` (bp/step/wp phases) + `docs/reference/134-debug-fs.md` |

| I-40 | **T-1: no torn scanout / surface-share integrity** (TAPESTRY.md §18.8; the graphics arc; STAGED enumeration per the I-20 RESERVED→ENFORCED precedent): every page of a weave (a surface's framebuffer Burrow) stays backed, mapped-membership-immutable, from its first client map to its retire; a present op's lifetime brackets its `TRANSFER_TO_HOST_2D` window; a weave retires only after quiesce + scanout-composition release. **The KERNEL SHARE HALF is ENFORCED at G-2 (2026-07-19; the ABI user-signed-off)**: the share admits ONLY the KERNEL-MINTED device-passive DMA-weave subtype (`SYS_DMA_CREATE_WEAVE` → the create-immutable `KObj_DMA.weave` bit; a creator-asserted flag rejected per §18.12 R2-F1) alongside ANON rings — a device-command DMA region (virtqueue/descriptor, plain `SYS_DMA_CREATE`) is as structurally unshareable as MMIO; the claim derives the binding kind from the kernel-minted Burrow type and fails closed on a server-declaration mismatch (`weft_claimed_kind`); `SYS_WEFT_UNSHARE` realizes the spec's `Map` guard (registry-removal-before-free + the claim's live-registry lookup ⇒ a retire-raced claim fails closed — NoStaleMap's kernel half; also closes the #289 seam); the client's cross-Proc pin is budget-bounded (`Proc.shared_map_pages`, the I-32 FIFTH axis, §18.12 R2-F3) and dropped at the weave fid's clunk (`ClunkMap`); `MappedImpliesBacked`/`RefImpliesBacked` hold across the KObj_DMA SECOND refcount domain (the #847 dual count keeps the pixel chunk alive while EITHER Proc maps). **The PRESENT half is ENFORCED at G-3 (tapestryd stage 0 + the R2-F3 reaper)**: presents are handled SYNCHRONOUSLY (validate → `TRANSFER_TO_HOST_2D` → `RESOURCE_FLUSH` → the `Rwrite`-CQE recycle gate, all inside one server dispatch), so the in-flight present set is EMPTY at every retire decision point — the quiesce guard holds BY CONSTRUCTION at stage 0 (a pipelined controlq, G-6+, must implement a real drain before touching retire — the recorded obligation); the retire ordering is `t_weft_unshare`-BEFORE-any-backing-free (the R2-F5 Map-guard discharge) → scanout release → `DETACH_BACKING`+`RESOURCE_UNREF` → the server refs drop; the scanout switches only at first-present-COMPLETE (F16); and the `ServerDeath` leg is real — the R2-F3 KERNEL REAPER (`kernel/weft.c::weft_reap_sweep` + a kproc kthread) force-reclaims a dead compositor's orphaned client weave mappings after a bounded grace (cross-Proc unmap under `g_proc_table_lock`+`vma_lock` with the G-2-F1 identity guard re-checked; budget uncharged; the registration pin dropped so the pixel chunk frees at reclaim; serialized against `dev9p_close` by `g_weft_reap_lock`) | The `burrow_share_into` + `SYS_WEFT_SHARE` admission gates (kernel-minted-subtype-only) + `weft_claimed_kind` + `weft_share_unregister` + the `shared_map_pages` charge/uncharge pairing (the SHARED_IN VMA flag) + the dev9p_close weave clunk-unmap + tapestryd's retire discipline + the weft reaper | `specs/tapestry_present.tla` (model-first; clean GREEN 5413 distinct — unperturbed across BOTH impl halves — + liveness incl. `EventuallyRetired`-across-`ServerDeath` + 4 buggy cfgs each firing its named invariant) + `specs/SPEC-TO-CODE.md::tapestry_present.tla` (the G-2 kernel map + the G-3 server map) + the `weft.{share_rejects_plain_dma, weave_share_and_claim, unshare_disarm, shared_map_budget_cap, weave_clunk_unmap_guard, reap_orphan_reclaimed, reap_live_session_untouched, reap_close_unregisters}` regressions + the per-boot pattern-persists gate (the -v quadrants THROUGH the full compositor path + the liveness double-dump) + the G-2/G-3 focused audits + the SMP gate |

These are the project's promises. Every one has a spec or a runtime check or a compile-time assertion. None are policy-only.

**Corvus invariants (C-1..C-23)** are enumerated separately in `CORVUS-DESIGN.md §9` — they govern the key agent's runtime + audit guarantees (mlock'd pages, session ownership monotonicity, audit log encryption, the kernel-stamped per-connection `/srv` transport identity, etc.). Cross-referenced here so the global invariant surface is discoverable; the canonical text lives in CORVUS-DESIGN.

**Pouch invariants (P-1..P-4)** are enumerated separately in `POUCH-DESIGN.md §11` — they govern the Phase-6 POSIX environment's boundary guarantees: **P-1** no foreign syscall number ever enters the kernel (the structural form of `ROADMAP.md §3.6` — the kernel is never modified to accommodate POSIX); **P-2** pouch is the sole POSIX path to the kernel; **P-3** no silently-wrong POSIX surface (every surface either maps to a defined Thylacine behavior or returns a documented errno); **P-4** the pouch upper/lower boundary line holds (vendored musl above, Thylacine-native below). Cross-referenced here so the global invariant surface is discoverable; the canonical text lives in POUCH-DESIGN.

**Reserved invariants (graphics phase; land at impl, per the T-1 precedent in `docs/TAPESTRY.md` §6).** The compositor + agentic-enablement design (TAPESTRY.md §13-18 + ARCH §17, added 2026-06-08; concretized 2026-07-17 by the §18 design pass — **T-1 is now ENUMERATED as I-40** in the table above: the kernel share half ENFORCED at G-2 [2026-07-19], the present half ENFORCED at G-3 [tapestryd stage 0 + the R2-F3 reaper] — the row is COMPLETE) *reserves* — but does not yet enumerate as enforced — the remaining set that takes its §28 numbers when the graphics phase builds (the way Loom deferred I-29/I-30's §28 edit to its impl): **placement-transparency** (a surface's placement — inline / split / tab / pinned — is invisible to and unforgeable by its client; TAPESTRY.md D5); **layout-tree integrity** (the `/dev/tapestry` 9P tree is the single source of layout truth — control is 9P, pixels are Loom, the two transports never crossed); **agentic-capture gating** (the in-band screen-readback / input-injection files are dev/test-build-only — the #880 strip-for-production class). Reserving (not enumerating) keeps Phase-10 detail at vision-sketch altitude; the audit-trigger rows (`tapestryd`, `netd`, the agentic capture/inject surface) join §25.4 the same way, at impl.

**The network arc (`NET-DESIGN.md`, the #68 charter) adds NO new §28 invariant** — network soundness composes existing ones: **I-1** (a Proc's reach is bounded by its `/net` view = the firewall), **I-5** (netd's NIC handles non-transferable), **I-9** (the `dev9p.poll`->netd readiness wake loses no edge; reserved `net_poll.tla`), **I-10/I-11** (netd connection-`N` / fid identity), **I-23** (netd's authority bounded by its endowed NIC capability), **I-28** (`/net` path-containment + per-component X-search), **I-29/I-30** (network I/O riding Loom inherits its completion integrity + submit-time pin). Its audit-trigger surfaces (`netd`, `dev9p.poll`, the socket-compat pouch patch, the virtio-net RX+TX driver, `cs`/`dns`, `ipconfig`/DHCP, a settable-clock `SYS_CLOCK_SETTIME` if pulled forward) join §25.4 at the sub-chunk that lands each, per NET-DESIGN.md §15.2. **net-1 LANDED + audited the virtio-net RX+TX driver surface** (`usr/lib/netdev` — the reusable `VirtioNet` frame transport; the virtio DMA/IRQ/ring memory-safety class, Opus-4.8 focused audit 0 P0/0 P1/1 P2/4 P3 all fixed-or-closed); see `docs/reference/114-netdev.md` + the net-1 closed list.

**The Menagerie arc (`MENAGERIE.md`, the driver framework; scripture adopted 2026-06-15) adds one new invariant — I-34 — now ENUMERATED-as-enforced (the §28 table above) following the hardware-allowance landing (build-arc step 3, spec-first per the asid/death-wake precedent).** Everything else composes existing invariants: **I-1** (drivers are isolated Procs), **I-5** (their MMIO/IRQ/DMA handles non-transferable), **I-2/I-25** (the allowance grant + the `DeviceRemoved` scope-teardown lifecycle), **I-15** (hardware view from DTB — *enforced* by `devhw`), **I-29/I-30** (the driver data path rides Loom), and **pci-1b's exclusive claim** (the per-`(bus,dev,fn)` BAR-bounded claim is the first instance of I-34). **I-34 (driver authority bound, RESERVED)**: a driver's hardware authority is exactly its warden-granted allowance — a subset of its bound node's resources, declared in its manifest, granted only by the warden, never widened, and fully revoked on unbind/removal/crash; the I-25 analog for hardware. It is enforced at `SYS_MMIO/IRQ/DMA_CREATE` (the three coarse `CAP_HW_CREATE` gates today, `kernel/syscall.c:216/368/458`) by a per-Proc allowance check, and modeled by `specs/allowance.tla` (extends the I-25 scope model + generalizes pci-1b). Its audit-trigger surfaces (the allowance check, the warden's grant decision, `devhw`, the discovery-source trust boundary) join §25.4 at the sub-chunk that lands each, per MENAGERIE.md §15-16. **The Loom device-gone terminal CQE LANDED (build-arc step 4, 2026-06-15)** as an I-29 extension (not a new invariant): a session death threads its reason (a peer-gone EOF -> the device-gone `-T_E_NODEV`, a transport error -> `-T_E_IO`) to the terminal CQE, modeled by `specs/loom_devgone.tla`; see I-29 above + `docs/reference/107-loom.md` (Device-gone terminal).

**The Aurora + installer arc (`AURORA.md` + `INSTALLER.md`, scripture 2026-06-15) adds NO new §28 invariant.** **Aurora** (the textual environment, §17) composes **T-1** (no torn scanout, via Tapestry — reserved-then-enforced) + the **I-27** trusted-path renderer obligation (full suspension during a SAK episode, `TRUSTED-PATH.md`); its only security-load-bearing edge is that obligation. **The installer** ("the founding" — the host-bake promoted to a runtime program; `INSTALLER.md`) **mints the system root-of-trust**, the most security-load-bearing first-run, but composes existing guarantees: corvus **C-20/C-27/C-28** (the system identity + the no-escrow BIP-39 recovery; passphrase → Argon2id → system-identity → pool-DEK), the A-4 clearance model (**I-2/I-25**), Stratum integrity (**I-14**), and the driver-framework allowance (**I-34**, to reach the target disk). Its audit surfaces (the root-of-trust mint; the DEK seal; the crash-safety of the multi-phase founding write; the no-baked-secret property) and the Stratum-side pool-unlock change join §25.4 at the sub-chunk that lands each, per INSTALLER.md §11/§14.

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
