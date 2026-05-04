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
  │    ← buffer sharing via VMO handles (zero-copy)       → │
  ├─────────────────────────────────────────────────────────┤
  │  Compatibility layer (POSIX/Linux syscall translation)  │
  │    musl port │ Linux ARM64 syscall shim │ container ns  │
  ├─────────────────────────────────────────────────────────┤
  │  Thylacine kernel (C99, ARM64)                          │
  │    Namespace │ EEVDF Scheduler │ VM │ 9P client (pipe.) │
  │    Handle table │ VMO manager │ IRQ forwarding │ Notes  │
  │    Dev vtable │ Chan │ Pipes │ rendezvous │ Pty infra   │
  ├─────────────────────────────────────────────────────────┤
  │  Stratum (native filesystem, 9P server, userspace)      │
  │  — Phase 4, 9P2000.L + Stratum extensions               │
  └─────────────────────────────────────────────────────────┘
```

### 2.2 Key abstractions

- **Process (`Proc`)**: unit of isolation. Has a private namespace, address space, credential set, handle table, and one or more threads.
- **Thread**: register context + kernel stack + signal mask + per-thread `errstr`. Multiple threads share a process's address space, namespace, fd table, and handle table unless explicitly cloned via `rfork`.
- **Namespace (`Pgrp`)**: a process's view of the resource tree. Composed via `bind` and `mount`.
- **Chan**: the kernel object representing an open resource (file, device, synthetic node). Carries `qid`, `dev` pointer, offset, mode. The fundamental currency of the kernel; `read` / `write` / `walk` / `clunk` operate on Chans.
- **Dev**: the vtable every kernel device implements (`attach`, `walk`, `stat`, `open`, `create`, `read`, `write`, `close`, `remove`, `wstat`, etc.). All kernel devices implement this interface; userspace devices implement it remotely via 9P.
- **9P server**: any userspace process exposing a resource tree via the 9P2000.L protocol. Drivers, services, and the filesystem are all 9P servers. Mounted into a process namespace via the kernel's 9P client.
- **Kernel object handle**: typed, unforgeable integer token scoped to a process. Eight types: `Process`, `Thread`, `VMO`, `MMIO`, `IRQ`, `DMA`, `Chan`, `Interrupt`. Carry rights bitmasks.
- **VMO** (Virtual Memory Object): kernel object representing a region of memory, independent of any process's address space. Reference-counted; pages live as long as any handle or mapping refers to them.
- **Note**: async text message delivered to a process. Plan 9's signal model. POSIX signals translate to/from notes in the compat layer.

### 2.3 Invariants each abstraction maintains

| Abstraction | Invariant | Enforcement | Spec |
|---|---|---|---|
| Process | Namespace operations don't affect other processes' namespaces | Kernel namespace isolation | `namespace.tla` |
| Process | Capability set monotonically reduces (`rfork` only reduces, never elevates) | Syscall gate | `handles.tla` |
| Chan | `chan->dev` valid for the chan's lifetime | Ref-counted dev attachment | runtime |
| Namespace mount | Mount points form a DAG, never a cycle | Kernel mount validation | `namespace.tla` |
| Handle | Rights monotonically reduce on transfer | Syscall-level check | `handles.tla` |
| Handle (hardware) | `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` cannot transfer | Syscall has no code path; static_assert | `handles.tla` |
| Handle (other) | Transferable handles (`Process`, `Thread`, `VMO`, `Chan`) transfer only via 9P | Kernel transfer syscall accepts only 9P-attached transfers | `handles.tla` |
| VMO | Pages live until last handle closed AND last mapping unmapped | Ref-counted; `vmo.tla` proves no UAF | `vmo.tla` |
| 9P session | Fid table is per-connection; fids don't leak between connections | Per-session fid table | `9p_client.tla` |
| 9P session | Per-session tag uniqueness | Per-session tag pool with monotonic generation | `9p_client.tla` |
| Scheduler | Every runnable thread eventually runs | EEVDF deadline computation | `scheduler.tla` |
| Wakeup | No wakeup lost between wait-condition check and sleep | Wait/wake state machine | `scheduler.tla`, `poll.tla`, `futex.tla` |
| Memory | W^X on every page | Page table entry mutual exclusion | runtime + `_Static_assert` on PTE bits |
| Memory | KASLR offset randomized at boot | Boot init randomizes TTBR1 base | runtime |

The full enumerated invariant list (I-1 through I-N) lives in §28 with file-level traceability.

### 2.4 Layer responsibilities

- **Kernel** owns: virtual memory, scheduling, namespace primitives, handle table, VMO manager, IRQ forwarding, 9P client, ramfs, procfs (synthetic Dev), tmpfs, ctlfs, console (cons / consctl). All in C99.
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
| Userspace drivers | **Rust** | Drivers handle hardware DMA descriptors, parse 9P, manage VMO ref-counts — exactly the domains where Rust's borrow checker eliminates classes of bugs. |
| Halcyon | **Rust** | Parses bash-subset syntax, decodes images, renders fonts, manages a stateful UI — historically high CVE density in C; Rust eliminates UAF + buffer overflow at compile time. |
| Userspace services (network stack, video player, etc.) | **Rust** | Same rationale as drivers. |
| POSIX-compat synthetic FS | **C99** | Small servers wrapping existing C libraries (libfuse-9p, etc.); Rust would add crate-build complexity for little gain. Rewrite candidates for v2.0. |
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

---

## 4. Target architecture

**STATUS**: COMMITTED

### 4.1 Primary target: ARM64 (AArch64)

- **EL0**: userspace. **EL1**: kernel. **EL2**: hypervisor (available, not used at v1.0). **EL3**: secure monitor (not used).
- ARM64 exception model used directly: `SVC #0` for syscall entry, exception vectors for interrupts and faults, `BRK` for kernel breakpoints.
- **Pointer Authentication (PAC)** enabled for kernel return addresses where hardware supports it (v8.3+). All kernel function returns are PAC-signed; tampering panics cleanly.
- **Memory Tagging Extension (MTE)** enabled by default where hardware supports (v8.5+; Apple Silicon, recent ARM cores, QEMU emulation). All kernel allocations and userspace heap allocations are MTE-tagged; UAF / overflow caught at hardware speed.
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

### 4.5 RISC-V (long horizon, v2.x)

Post-v1.0. Target: first RVA23-compliant SBC with PCIe and NVMe (Milk-V Jupiter 2 or equivalent). The kernel port from ARM64 to RISC-V is mechanical above `arch/`: swap GIC for PLIC, ARM CSRs for RISC-V CSRs, ARM generic timer for SBI timer. All namespace, scheduler, 9P, VMO, and handle code is architecture-independent and transfers without modification.

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
    ↓ mount_initramfs(): cpio archive at /; contains stratum, janus, init binaries
    ↓ create init process; exec("/sbin/init")
    ↓ idle loop: WFI on each CPU
init (userspace, /sbin/init)
    ↓ start stratum (mounts VirtIO block; presents 9P server)
    ↓ start janus (mounts /dev/janus/; key agent)
    ↓ remount / from Stratum (kernel umounts ramfs from /)
    ↓ exec /sbin/init-stage2 if present, else /bin/sh
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
- `/sbin/init` (Phase 4+) or `/sbin/init-minimal` (Phase 1-3 — UART shell only).
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
- Demand paging with copy-on-write for `rfork(RFPROC)` (when `RFMEM` is not set).
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
- `chan_cache` (size of `struct Chan`)
- `vmo_cache` (size of `struct Vmo`)
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
            struct Vmo *vmo;    /* VMO-backed mapping */
            uintptr_t offset;
        } vmo;
        struct {
            struct Chan *chan;  /* file-backed (post-v1.0) */
            uintptr_t offset;
        } file;
    };
    struct rb_node rb;
};
```

**Page fault handler** (`arch/arm64/fault.c`):
- Demand-pages anonymous mappings on first touch (zero-filled).
- COW-copies on first write to a `RFPROC`-shared page.
- Translates VMO-backed faults to VMO page lookups.
- Panics on kernel faults outside the direct map / vmalloc / fixmap regions (a kernel page fault is a bug, not a recoverable condition).

**`mmap` and `mprotect`**:
- `mmap(addr, len, prot, flags, fd, off)` creates a vma. Anonymous if `fd == -1`; file-backed if `fd` references a Chan; VMO-backed if `fd` is a VMO handle wrapped in a Chan.
- `mprotect(addr, len, prot)` updates the vma's flags. **Rejects W^X violations**: a transition from `R+W` to `R+X` (or `R+W+X`) returns `-EPERM`. Once a page is writable, it cannot become executable without a separate kernel-mediated path (none at v1.0; future JIT support will define one).

### 6.6 W^X enforcement

W^X is enumerated invariant I-12. Enforcement layered:

- **PTE bit layer**: ARM64 page table entry has separate `AP[2:1]` (access permissions) and `XN` (execute-never) bits. The kernel page table writer rejects any PTE that has both `AP[2:1] != 0b11` (writable) AND `XN == 0` (executable). Compile-time `_Static_assert` on a sanity bit pattern.
- **`mmap` / `mprotect` layer**: as above.
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
- Memory cgroups-equivalent for namespace-level memory accounting. Plan 9 doesn't have this; Linux does (`memory.max`); for the development-VM target, system-wide accounting via `/ctl/mm/` is sufficient at v1.0.

### 6.9 Summary

Buddy allocator + per-CPU magazines for physical frames; SLUB for kernel objects; per-process VMAs in a red-black tree; W^X enforced at PTE bit layer + syscall layer + ELF loader. KASLR randomizes kernel base. NUMA-shaped from v1.0 (single-node).

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
- A namespace (`Pgrp` — mount table + per-bind overlays).
- A file descriptor table (open `Chan`s).
- A handle table (typed kernel object handles).
- A credential set (uid, gid, supplementary groups, capabilities).
- A set of threads (one or more).
- A note queue (pending notes; per-process, delivered to whichever thread has notes unmasked).
- Parent / child relationships (for `wait`).

```c
struct Proc {
    int            pid;
    struct Pgrp   *namespace;
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
| `RFNAMEG` | Share namespace (default for threads; clone for processes). |
| `RFFDG` | Share file descriptor table (default for threads). |
| `RFCRED` | Share credentials. |
| `RFNOTEG` | Share note queue (default for threads). |
| `RFNOWAIT` | Don't add to parent's children list (orphan from creation). |
| `RFREND` | Share rendezvous space. |
| `RFENVG` | Share environment. |

Examples:
- POSIX `fork()`: `rfork(RFPROC | RFFDG | RFENVG)` — new Proc, copy-on-write address space, clone fd table.
- POSIX `pthread_create()`: `rfork(RFPROC | RFMEM | RFNAMEG | RFFDG | RFCRED | RFNOTEG)` — new thread within existing Proc.
- Plan 9 container: `rfork(RFPROC | RFNAMEG)` — new Proc, fresh namespace, copy-on-write address space.
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

Spec: `notes.tla` proves note delivery ordering, mask correctness, and async-safety properties (handlers don't fire while the kernel is in a critical section).

### 7.7 Capability set

Each `Proc` has a coarse-grained capability bitmask:

| Capability | Allows |
|---|---|
| `CAP_BIND_PRIV` | Bind to privileged ports (< 1024) |
| `CAP_HW_HANDLE` | Receive hardware handles at process creation (driver bit) |
| `CAP_RAW_MEM` | Open `/dev/mem` for physical memory access |
| `CAP_PTRACE` | Debug another process |
| `CAP_NS_ADMIN` | Modify namespace beyond own (e.g., post-mount /proc) |
| `CAP_NET_ADMIN` | Configure network interfaces |
| `CAP_SYS_ADMIN` | Mount filesystems other than into own namespace |

Capabilities can only be reduced via `rfork`, never elevated, except via the v2.0 factotum-mediated capability elevation (§15.4 — designed-not-implemented at v1.0).

Default at process creation: empty set. Drivers receive `CAP_HW_HANDLE` from kernel at startup; they immediately drop after retrieving their handles.

### 7.8 `errstr` and error reporting

Every thread has a per-thread `errstr` buffer (Plan 9 idiom). System calls returning failure also set this buffer with a textual description:

```c
errstr("namespace cycle: cannot bind /a/b onto /a (would create loop)");
return -1;
```

The user retrieves it via `errstr(buf, sz)` syscall. POSIX errno is provided by musl's translation: each Plan 9 errstr maps to an errno (`-EEXIST`, `-EINVAL`, etc.).

This is richer than POSIX errno alone and is preserved as a Plan 9 idiom worth keeping. errstr text is also logged at `/ctl/log/errstr/<pid>` for debugging.

### 7.9 Process exit and wait

`exits(msg)` terminates the calling process. `msg` is a text status string ("ok" for clean exit, anything else for failure). Goes into `Proc->exit_status`; parent retrieves via `wait()`.

`wait(*WaitMsg)` blocks until a child exits; returns child's PID + exit_status. If multiple children exit, they're queued; `wait` returns oldest first. POSIX `waitpid` translates straightforwardly.

When a process exits:
1. All threads stopped.
2. All open Chans clunked.
3. All handles closed.
4. Address space torn down.
5. Namespace dropped.
6. Children re-parented to PID 1 (init).
7. Notes drained.
8. Parent notified (synthetic note-equivalent if waiting).
9. `Proc` structure freed via slab cache.

Spec: `notes.tla` covers the exit-during-note-delivery race (note arrives just as process is exiting; handler is no longer valid).

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
- Energy-aware scheduling (big.LITTLE). v1.0 treats all cores equal.
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

### 8.4 Per-CPU run trees + work-stealing

Each CPU has a `cpu_t` with:
- Three EEVDF run trees (one per band) — implemented as red-black trees keyed on `vd_t`.
- A current running thread.
- An idle thread.
- Scheduler stats (`/ctl/sched/cpu<N>/`).

When a thread becomes runnable on CPU N (e.g., wakeup from sleep), it's enqueued in CPU N's run tree. If the wakeup is from a different CPU M, an IPI is sent to N to update its tree.

When a CPU goes idle (no runnable threads in any band), it tries work-stealing:
1. Pick a peer CPU (round-robin from random start).
2. If peer's tree is non-empty, lock peer's tree, dequeue a thread from the lowest band with runnable threads, transfer to local tree.
3. Wake locally.

Steal frequency is bounded — an idle CPU steals at most every N microseconds to avoid thrashing.

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
int  wakeup(struct Rendez *r);     /* wake one waiter */
void rendezvous(void *tag, void *value);  /* cross-thread synchronous handoff */
```

These are familiar to anyone who's read Plan 9 kernel source. Underneath, they map to EEVDF + wait queue + IPI machinery.

### 8.9 Open design questions

- **EEVDF lookahead bound** (the `slice_size` parameter). Default 6 ms (matching Linux EEVDF). Tune-able at `/ctl/sched/slice-size`. Decision deferred to Phase 2 implementation; spec sweep over a range.
- **Cross-band aging**: should an idle-band thread eventually promote to normal-band? At v1.0, no — the bands are clean-cut. v2.x could add age-based promotion if needed.

### 8.10 Spec

`specs/scheduler.tla` proves:
- **Progress**: every runnable thread eventually runs (no starvation, even under adversarial wakeup patterns).
- **Latency bound**: the delay between a thread becoming runnable and running is at most `slice_size × N_runnable_in_band`.
- **Wakeup atomicity**: no wakeup is lost across the wait/wake race.
- **IPI ordering**: IPIs from one CPU to another are processed in send order.
- **Work-stealing fairness**: stealing doesn't preferentially favor or disadvantage any band.

### 8.11 Summary

EEVDF on per-CPU run trees with work-stealing, three priority bands, tickless idle, IPI-driven cross-CPU operations, Plan 9 idiom layer on top, formally verified.

---

## 9. Namespace and device model

**STATUS**: COMMITTED

### 9.1 The namespace

Each process has a private namespace — a tree of mount points mapping names to resource servers. The namespace is the fundamental unit of isolation and composition.

**Operations**:
- `bind(old, new, flags)` — attach a file or directory at another point in the tree. Flags: `MREPL` (replace), `MBEFORE` (union, checked first), `MAFTER` (union, checked last), `MCREATE` (allow creates in the union).
- `mount(fd, afd, old, flags, aname)` — attach a 9P server (reached via `fd`) at `old`. `afd` is the auth fd (or -1); `aname` is the attach name (passed to `Tattach`). Flags: `MREPL`, `MBEFORE`, `MAFTER`.
- `unmount(name, old)` — remove a mount point. `name` is the mounted source (or NULL for any); `old` is the mount point.

These three operations, composed, express containers, overlay filesystems, per-process views, chroot, and capability restriction — without additional kernel machinery.

**Namespace inheritance at `rfork`**: by default (no `RFNAMEG`), the child gets a private copy of the parent's namespace. Modifications to the child don't affect parent. With `RFNAMEG`, parent and child share the namespace (typical for thread creation).

**Namespace data structure** (`kernel/namespace.c`):

```c
struct Mount {
    struct Chan *to;           /* what's mounted */
    struct Chan *from;          /* mount point */
    int flags;
    struct Mount *next, *prev;
    int order;                  /* union ordering */
};

struct MountHead {
    struct Mount *mount;        /* list of mounts at this point */
    struct rb_node rb;
};

struct Pgrp {
    struct MountTable {
        struct rb_tree mounts;  /* keyed by mount point qid */
        rwlock_t lock;
    } mt;
    int ref;
};
```

`bind` / `mount` operations take `Pgrp->mt.lock` write-locked; lookups take it read-locked. Lock contention is rare (mount changes happen at process startup or container creation, not on hot paths).

**Spec**: `namespace.tla` proves:
- Cycle-freedom: no `bind` operation can create a cycle.
- Isolation: namespace operations in process A don't affect process B's namespace (unless they share via `RFNAMEG`).
- Walk determinism: a path lookup from a fixed namespace state always produces the same Chan.

### 9.2 The Dev vtable

Every kernel device implements:

```c
struct Dev {
    int   dc;                    /* device character ('c' for cons, 'e' for ether, etc.) */
    char *name;

    void   (*reset)(void);
    void   (*init)(void);
    void   (*shutdown)(void);
    struct Chan*  (*attach)(char *spec);
    struct Walkqid* (*walk)(struct Chan *c, struct Chan *nc, char **name, int nname);
    int    (*stat)(struct Chan *c, uint8_t *dp, int n);
    struct Chan*  (*open)(struct Chan *c, int omode);
    void   (*create)(struct Chan *c, char *name, int omode, uint32_t perm);
    void   (*close)(struct Chan *c);
    long   (*read)(struct Chan *c, void *buf, long n, int64_t off);
    struct Block* (*bread)(struct Chan *c, long n, int64_t off);
    long   (*write)(struct Chan *c, void *buf, long n, int64_t off);
    long   (*bwrite)(struct Chan *c, struct Block *bp, int64_t off);
    void   (*remove)(struct Chan *c);
    int    (*wstat)(struct Chan *c, uint8_t *dp, int n);
    struct Chan*  (*power)(struct Chan *c, int on);
};
```

All kernel devices — including synthetic ones like `/dev/cons`, `/dev/null`, `/proc` — implement this interface. Userspace devices implement it remotely via 9P.

The interface is preserved verbatim from Plan 9 (with C99 typing) for two reasons: (a) it's right; (b) it makes porting from 9Front straightforward when we want to.

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
2. All 9P sessions to the driver are clunked; affected `Chan`s in clients are marked invalid (return `-EIO` on subsequent ops).
3. Handles held by the driver are released (the `KObj_*` resources go back to the kernel free pool).
4. `init` (or a designated supervisor) detects the crash and restarts the driver. New driver process gets fresh handles for the same hardware.
5. Clients reconnect; resume.

This is the same shape as MINIX 3's reincarnation server. Implemented as a userspace supervisor (Phase 6+); kernel exposes the necessary signals (`/ctl/proc-events/exit`).

### 9.4 Device namespace layout (v1.0 target)

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
│   │   ├── image        ← VMO handle to pixel buffer
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
├── proc/                ← process namespace (kernel Dev)
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

### 9.5 Interactions with the per-process namespace

A process's namespace is a tree. The mount table at any path can have multiple Chans (union mount):

```
Process A's namespace:
/home/alice (bound from /var/stratum/users/alice)
/code       (bound from /var/stratum/code)
/dev        (kernel Dev tree)
/etc        (Stratum / etc subtree)
```

The Plan 9 walk algorithm: walk from `/` to each path component; at each component, check the mount table for that path. If multiple mounts (union), check each in declared order until one succeeds. If a path component creates (in `MCREATE`-flagged union), create in the first writable mount.

### 9.6 Open design questions

None at Gate 3.

### 9.7 Summary

Per-process namespace, `bind` / `mount` / `unmount` as the only composition operations. `Dev` vtable for kernel devices; userspace drivers as 9P servers. Standard `/dev/`, `/proc/`, `/ctl/` paths. Driver crash recovery via process supervision.

---

## 10. IPC

**STATUS**: COMMITTED

### 10.1 9P as the universal IPC

There is no separate IPC mechanism. All inter-process communication is mediated by 9P: one process mounts another's 9P server and reads/writes files. Pipes are 9P streams. Shared memory is a 9P file backed by anonymous memory (with VMO handles for zero-copy). Message queues are 9P files.

This is the Plan 9 model, adopted unchanged.

### 10.2 9P dialect: 9P2000.L + Stratum extensions

**Dialect**: 9P2000.L (the Linux-extended dialect). Includes the L-extension messages: `Tgetattr`, `Tsetattr`, `Treaddir`, `Tlock`, `Tlink`, `Tsymlink`, `Tmknod`, `Trename`, `Tflush`, `Trenameat`, `Tunlinkat`, `Txattrwalk`, `Txattrcreate`, `Tfsync`, `Tlcreate`, `Tmkdir`. Also covers POSIX file modes properly (vs vanilla 9P2000's restricted set).

**Stratum extensions** (committed at Stratum's Phase 9):
- `Tbind` / `Rbind` — per-connection subvolume composition (within the Stratum connection's namespace).
- `Tunbind` / `Runbind` — undo the above.
- `Tpin` / `Rpin` — pin a snapshot for the connection (prevents reclamation).
- `Tunpin` / `Runpin` — release the pin.
- `Tsync` / `Rsync` — explicit fsync.
- `Treflink` / `Rreflink` — `copy_file_range` with reflink semantics.
- `Tfallocate` / `Rfallocate` — `fallocate` with all FALLOC_FL_* flags.

These are negotiated at session establishment; clients that don't support them fall back to the L-baseline.

### 10.3 Pipes

Pipes are kernel-implemented 9P streams:
- `pipe(fd[2])` creates a pair of `Chan`s backed by a kernel ring buffer.
- Read end has read-only mode; write end has write-only.
- Reads block when empty; writes block when full.
- EOF on write end causes EOF on read end.
- Buffer size: `PIPE_BUF = 4 KiB` (matching POSIX guarantee for atomic writes).

Pipes are the primary mechanism for connecting processes. Shell pipelines compose processes via pipes, exactly as in Unix.

For very large inter-process data transfer (GiB-class), VMO-based shared-memory regions transferred via 9P sessions (§19) are the zero-copy alternative.

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
| `open(name, mode)` | Open a file in the namespace |
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
| `bind(old, new, flags)` | Bind in namespace |
| `mount(fd, afd, old, flags, aname)` | Mount 9P server |
| `unmount(name, old)` | Unmount |
| `rfork(flags)` | Create process/thread |
| `exec(name, argv)` | Replace process image |
| `exits(msg)` | Terminate process |
| `wait(waitmsg)` | Wait for child |
| `sleep(ms)` | Sleep milliseconds |
| `brk(addr)` | Adjust data segment |
| `mmap(addr, len, prot, flags, fd, off)` | Map memory |
| `mprotect(addr, len, prot)` | Change page protection (rejects W^X violations) |
| `munmap(addr, len)` | Unmap memory |
| `pipe(fd[2])` | Create pipe |
| `dup(oldfd, newfd)` | Duplicate fd |
| `noted(v)` | Note handler return |
| `notify(fn)` | Register note handler |
| `postnote(pid, msg)` | Post a note to a process |
| `rendezvous(tag, val)` | Synchronous handoff |
| `nsec()` | Nanosecond clock |
| `getpid()` | Process ID |
| `gettid()` | Thread ID |
| `errstr(buf, n)` | Read per-thread errstr |

### 11.3 Handle syscalls

Per §18:

| Syscall | Description |
|---|---|
| `handle_close(h)` | Release a handle |
| `handle_rights(h)` | Query rights on a handle |
| `handle_reduce(h, rights)` | Return new handle with reduced rights |
| `mmap_handle(h, addr, len, prot)` | Map a VMO or MMIO handle |
| `irq_wait(h)` | Block until IRQ handle fires; returns count |
| `vmo_create(size, flags)` | Create anonymous VMO |
| `vmo_create_physical(paddr, size)` | Create physical VMO (privileged) |
| `vmo_get_size(h)` | Query VMO size |
| `vmo_read(h, buf, off, len)` | Read from VMO without mapping |
| `vmo_write(h, buf, off, len)` | Write to VMO without mapping |

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

The kernel uses a monotonic syscall number namespace; Linux syscall numbers don't overlap (kernel has its own numbering). The Linux shim has a separate entry path that decodes Linux numbers and dispatches.

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

Stratum is the native filesystem. It runs as a userspace daemon, exposing a 9P server (Stratum's Phase 9 deliverable). The kernel mounts it at `/` (or specified mount point) via the standard `mount` syscall.

Stratum is internally feature-complete (Phases 1-7 done; Phase 8 POSIX surface in progress; Phase 9 is the integration target). See `VISION.md §11` for the coordination story.

### 14.2 9P client (kernel)

The kernel implements a 9P2000.L client with Stratum extensions (`Tbind`, `Tunbind`, `Tpin`, `Tunpin`, `Tsync`, `Treflink`, `Tfallocate`). Pipelined from day one (§21).

Implementation (`kernel/9p_client.c`): wire protocol encode/decode, fid management, tag pool, outstanding-request table, dispatch loop. Spec: `9p_client.tla`.

### 14.3 Per-process 9P session

At process creation, if the process inherits a Stratum mount, a fresh 9P connection is established. (Per VISION §11, one connection per Proc at v1.0; multiplexing is a v2.x optimization.) The connection is kept alive for the process's lifetime; closed at exit.

Stratum sees N connections, one per Thylacine process, and gives each its own per-connection namespace (Stratum's NOVEL angle #8). Thylacine's per-process namespace and Stratum's per-connection namespace are complementary layers (VISION §11).

### 14.4 In-kernel Stratum driver — DESIGNED-NOT-IMPLEMENTED for v2.0

**Status**: design committed at Phase 0; implementation is post-v1.0.

**Motivation**: 9P-client mount adds one round-trip per FS operation. For root FS operations (where Stratum is the bottleneck), the round-trip is measurable. An in-kernel Stratum driver bypasses the 9P client by linking part of `libstratum.a` into the kernel.

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

### 15.1 Namespace isolation as the primary boundary

Each process's namespace is private. By default, a child process inherits its parent's namespace at `rfork` time, but modifications to the child's namespace do not affect the parent's. **This is the primary isolation mechanism in Thylacine.**

A "container" is simply a process whose namespace has been constructed to isolate it: a different root, a restricted `/dev/`, a private network server. No additional kernel mechanism is required (no cgroups, no AppArmor, no seccomp at v1.0).

Spec: `namespace.tla` proves isolation.

### 15.2 Credentials

Each process has:
- **uid**: user identity (numeric).
- **gid**: primary group.
- **groups**: supplementary groups (up to 32).
- **capabilities**: bitmask of coarse-grained kernel capabilities (§7.7).

File access control: standard Unix DAC (owner/group/other × rwx bits). This matches Plan 9's model and is sufficient for v1.0.

Capabilities can only be reduced via `rfork`. Elevation requires the v2.0 factotum mechanism (§15.4).

### 15.3 Capability set (v1.0)

Per §7.7. Coarse-grained.

### 15.4 Capability elevation via factotum — DESIGNED-NOT-IMPLEMENTED for v2.0

**Status**: design committed at Phase 0; implementation is post-v1.0.

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

Detailed in §24. Summary: KASLR, ASLR, W^X (enforced as invariant), CFI, stack canaries, ARM PAC, ARM MTE, ARM BTI — all enabled by default at v1.0.

### 15.6 Open design questions

- **Mandatory access control (MAC)**: not at v1.0. Namespace isolation provides much of what MAC provides without policy complexity. Revisited post-v1.0 if specific compliance / multi-tenant scenarios require it.
- **seccomp-equivalent syscall filtering**: a process can restrict its own syscall surface. v1.0 does this implicitly via namespace pruning (without `/dev/foo`, the syscall surface that takes a path doesn't reach foo). Explicit per-syscall filtering is post-v1.0.

### 15.7 Summary

Namespace isolation primary; standard Unix DAC + coarse capabilities; full SOTA hardening; factotum-mediated capability elevation designed for v2.0.

---

## 16. POSIX and Linux compatibility

**STATUS**: COMMITTED

### 16.1 Compat strategy (three tiers)

Per `VISION.md §12`:

| Tier | Target | Mechanism |
|---|---|---|
| 1 — native | Programs compiled against musl port | musl libc port to Thylacine syscalls |
| 2 — Linux static | Pre-built static Linux ARM64 binaries | Kernel syscall translation shim |
| 3 — container | OCI / Docker images | Process namespace + Linux-shaped 9P servers |

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

`sigaction`, `sigprocmask`, `sigsuspend`, `sigwaitinfo` implemented in musl + thin kernel shim against note delivery. POSIX programs receive signals normally; the note mechanism is invisible to them.

Hard cases handled:
- `SIGCHLD` with `SA_NOCLDWAIT` and `waitpid(WNOHANG | WUNTRACED | WCONTINUED)`.
- Signal masks across `rfork()` (inherited).
- `SA_RESTART` for syscall restart after signal delivery (kernel-level support).

Spec: `notes.tla` covers note delivery and signal-translation atomicity.

### 16.5 Container as namespace

Linux container images (OCI format) run inside a Thylacine namespace:
1. Mount the container's root filesystem (extracted to a Stratum subvolume or a 9P server) as the process root.
2. Provide synthetic `/proc-linux`, `/sys-linux`, `/dev-linux` 9P servers matching the Linux layout.
3. Run the container's init as a normal Thylacine process with the constructed namespace.

This is the "flatpak / Steam Deck" model from the vision: containers are namespaces, not a separate subsystem. The kernel primitive (namespace) handles both.

`thylacine-run` (a userspace tool, Phase 6) takes an OCI image and produces the namespace + init process. No cgroups, no seccomp at v1.0; namespace isolation is the boundary.

### 16.6 Open design questions

- **`epoll` design** (post-v1.0). The shape is: `epoll_create` returns a Chan; `epoll_ctl` adds/removes fds; `epoll_wait` blocks on multiple fds simultaneously. Cleanest implementation: a kernel `Dev` that wraps the existing `poll` infrastructure with a stable fd set. v1.1 candidate.

### 16.7 Summary

musl port (Tier 1 native); Linux syscall shim (Tier 2 static); container-as-namespace (Tier 3). Signals → notes via musl translation. epoll/inotify/io_uring deferred.

---

## 17. Halcyon integration

**STATUS**: COMMITTED

### 17.1 Kernel surface Halcyon depends on

Halcyon requires from the kernel:
- **Framebuffer**: `/dev/fb/image` (VMO handle), `/dev/fb/ctl` (resolution, format, flush).
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
    image     ← VMO handle to ARGB32 pixel buffer (zero-copy)
    info      ← read: current resolution and format
```

Halcyon receives the VMO handle on first open of `/dev/fb/image`; maps it; writes pixels directly to the mapped region; issues `flush` via `/dev/fb/ctl`. The VirtIO-GPU driver handles the DMA transfer to the host GPU.

For bare metal Apple Silicon (post-v2.0), the framebuffer device speaks to the AGX driver (via Asahi) via the same 9P interface. Halcyon does not change.

### 17.3 Input

Keyboard input via `/dev/cons` (line- or character-mode based on `consctl` settings). Halcyon reads keystrokes; no kernel-level translation beyond raw character delivery.

Mouse via `/dev/mouse` (if VirtIO input device is present). Halcyon uses mouse for selection and scrolling; not as a primary input model.

### 17.4 Process management

Halcyon is the user's primary launcher. When the user types `cmd args`, Halcyon:
1. Parses the command line.
2. `rfork(RFPROC | RFFDG | RFNAMEG)` — new process, share fd table briefly to set up redirections, share namespace (so mounted servers are visible).
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
| `KObj_VMO` | A virtual memory object | Yes |
| `KObj_Chan` | An open 9P channel (wraps Chan) | Yes |
| `KObj_MMIO` | A mapped MMIO region at a specific phys range | **No (typed)** |
| `KObj_IRQ` | The right to receive a specific interrupt | **No (typed)** |
| `KObj_DMA` | A DMA-capable physically contiguous buffer | **No (typed)** |
| `KObj_Interrupt` | An eventfd-like fd that fires on IRQ delivery | **No (typed)** |

Handles carry **rights** — a bitmask of what the holder can do:

| Right | Meaning |
|---|---|
| `RIGHT_READ` | Read the object's state |
| `RIGHT_WRITE` | Modify the object's state |
| `RIGHT_MAP` | Map the object into an address space (VMO, MMIO) |
| `RIGHT_TRANSFER` | Pass the handle to another process (only meaningful for transferable types) |
| `RIGHT_DMA` | Program DMA from this object |
| `RIGHT_SIGNAL` | Deliver/receive signals on this object |

Rights monotonically reduce when transferring; never elevate.

### 18.3 Typed transferability

The transfer syscall is typed — not just runtime-checked. The transfer-via-9P mechanism's switch covers only transferable types:

```c
long handle_transfer_via_9p(struct Chan *9p_chan, int handle_idx) {
    struct Handle *h = handle_get(handle_idx);
    if (!h) return -EBADF;
    switch (h->type) {
    case KObj_Process:
    case KObj_Thread:
    case KObj_VMO:
    case KObj_Chan:
        if (!(h->rights & RIGHT_TRANSFER)) return -EPERM;
        return do_transfer_via_9p(9p_chan, h);
    /* No code path for KObj_MMIO, KObj_IRQ, KObj_DMA, KObj_Interrupt. */
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
- `KObj_Chan` handles to open 9P connections (the normal file descriptor model).
- `KObj_VMO` handles for shared memory passed to them explicitly (e.g., framebuffer).
- `KObj_Process` and `KObj_Thread` handles for process management.

This is the hard boundary: regular programs cannot reach hardware regardless of what they do. The capability is simply not present in their handle table.

### 18.6 Handle transfer over 9P

`KObj_VMO` (with `RIGHT_TRANSFER`) and `KObj_Chan` are passed between processes over a 9P connection as out-of-band metadata on a message. This is the mechanism for zero-copy buffer passing: a driver creates a VMO, fills it, and passes the VMO handle to Halcyon (or any consumer) via the 9P session. The consumer maps the VMO and reads directly. No copy, kernel not in the data path.

The 9P out-of-band attachment uses an extension to 9P2000.L (negotiated at session establishment):

```
Twrite tag fid offset count data <handle: vmo_handle>
```

The receiver (or kernel-mediated 9P receive path) extracts the handle from the message metadata, places it in the receiving process's handle table with the requested rights, and continues normal 9P processing.

### 18.7 Open design questions

None at Gate 3.

### 18.8 Summary

Eight kernel object types, four transferable, four non-transferable. Subordination invariant: transfer only via 9P; hardware handles never. Typed enforcement at the syscall site.

---

## 19. Virtual memory objects (VMOs)

**STATUS**: COMMITTED

*Informed by: Zircon (Fuchsia).*

### 19.1 What a VMO is

A **Virtual Memory Object** is a kernel object representing a region of memory, independent of any process's address space. It is the unit of memory sharing in Thylacine.

A VMO has:
- A size (page-aligned).
- A backing type: anonymous (zero-filled on demand), physical (pinned for DMA), or file-backed (Stratum page cache, post-v1.0).
- A reference count. Pages are freed when the last handle is closed and all mappings are unmapped.

```c
struct Vmo {
    size_t size;
    enum {VMO_ANON, VMO_PHYS, VMO_FILE} type;
    union {
        struct page **pages;          /* anon: lazy-allocated */
        struct {
            paddr_t base;
            int locked;
        } phys;
        struct {                      /* post-v1.0 */
            struct Chan *chan;
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

- One process creates the VMO and holds the handle.
- It maps the VMO into its address space.
- It passes the VMO handle (with `RIGHT_MAP | RIGHT_READ`) to another process via 9P.
- The second process maps the VMO independently.

The kernel tracks both mappings. When the producing process unmaps, the pages remain live until all handles are closed and all mappings are gone. No use-after-free at the kernel level.

### 19.3 Primary use cases

**Zero-copy framebuffer**: VirtIO-GPU driver creates a physical VMO for the framebuffer. Halcyon receives the VMO handle and maps it. Halcyon writes pixels directly into the mapped region. Driver issues `flush` when Halcyon signals readiness. No copy at any point.

**Zero-copy video decode**: video decoder creates a physical VMO per decoded frame. Passes VMO handle to the video player 9P server. Player passes it to Halcyon. Halcyon blits from the mapped VMO into the framebuffer VMO. One blit; no intermediate copies; no kernel involvement after handle transfer.

**DMA buffer lifecycle**: NVMe / VirtIO-blk driver creates a physical VMO for its DMA descriptor rings and data buffers. The VMO handle is held exclusively by the driver. The kernel's IOMMU mapping (post-v1.0) is derived from the VMO's physical page list. Clean, auditable DMA ownership.

**Inter-driver shared memory**: two cooperating driver processes (e.g., network driver and network stack) share a ring buffer via a VMO. No kernel copy in the packet path.

### 19.4 VMO syscalls

| Syscall | Description |
|---|---|
| `vmo_create(size, flags)` | Create anonymous VMO, return handle |
| `vmo_create_physical(paddr, size)` | Create VMO over physical range (privileged) |
| `vmo_get_size(h)` | Query VMO size |
| `vmo_read(h, buf, off, len)` | Read from VMO without mapping |
| `vmo_write(h, buf, off, len)` | Write to VMO without mapping |

Mapping is done via `mmap_handle()` from §18. Unmapping via standard `munmap`.

### 19.5 Spec

`specs/vmo.tla` proves:
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
| VMO reference counts | Atomic operations (no lock) |
| Handle table (per-process) | Per-process lock, not global |
| IRQ routing table | Written at boot/driver-start only, read locklessly after |
| 9P session table | Per-session lock |
| Namespace mount table | Per-process rwlock |

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

(Real code is more careful with memory ordering and error paths; see implementation.)

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

### 23.1 Design principle: POSIX surfaces are 9P servers

Every POSIX surface in Thylacine is a 9P server that speaks a POSIX-shaped interface. There is no separate compat kernel layer. The mechanism is always: a 9P server mounts at a conventional path and serves the expected file tree. Thylacine-native programs use the underlying 9P interface directly; POSIX programs see what they expect; both are served by the same infrastructure.

This is the Plan 9 principle applied consistently: if it can be a file, it is a file. If it can be a 9P server, it is a 9P server.

### 23.2 Utopia minimum viable POSIX (Phase 5 entry requirement)

Per `VISION.md §13`. The set of surfaces that must exist for Utopia to feel real rather than broken.

**Shells**:
- `rc` — Plan 9 native, scriptable.
- `bash` — POSIX compat.
- Both require: `rfork`/`exec`/`wait`, `pipe`/`dup3`, file redirection, note→signal translation for Ctrl-C, `$PATH`-equivalent via union directories, job control (`SIGTSTP`, `SIGCONT`).
- Job control is required at Phase 5; feels broken without it.

**Coreutils** — uutils-coreutils (default; Rust rewrite of GNU coreutils, complete flag coverage):
```
ls, cat, echo, cp, mv, rm, mkdir, rmdir, chmod, chown, chgrp, ln, touch
stat, du, df, tee, dd, seq, yes, true, false, test, [, expr, printf
printenv, env, pwd, which, whoami, id, groups, hostname, uname, date
tty, tput, clear, reset, sleep, timeout, nice, nohup, time
kill, pkill, pgrep, ps, top, htop, free, vmstat, lsof
tar, gzip, bzip2, xz, zstd, cpio
find, xargs, sort, uniq, wc, head, tail, cut, paste, join, split, csplit, comm
diff, patch, cmp, md5sum, sha256sum, b3sum, od, hexdump, xxd, strings, file
grep, sed, awk, tr, fmt, fold, pr, nl, column, expand, unexpand
iconv, base64, base32, b2sum, realpath, readlink, dirname, basename, mktemp
install, getopt, getopts, mountpoint, chroot, runuser, sudo (v1.0 stub)
```

Plus Plan 9 userland (Tier 1): `rc`, `mk`, `awk`, `troff`, `tbl`, `eqn`, `9` launcher.

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
    root        ← symlink to root of process namespace
    ns/         ← namespace dump
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

**Status**: `poll` and `select` are must-have for Phase 5 (Utopia). Without `poll`, interactive bash, curl, Python asyncio, and essentially all non-trivial programs are broken.

**`poll(fds, nfds, timeout)`**: park the calling thread with a wait list across N fds. The first fd to become ready wakes the thread. For 9P-backed fds, the server signals readiness via a synthetic notification on the 9P session. For pipes and kernel fds, readiness is tracked in the kernel's Chan structures.

**`select()`**: implemented on top of `poll()`. Trivial.

**`epoll`**: Linux-specific scalable multiplexing. **Deferred to v1.1**. Designed as an extension of `poll` semantics, not a separate subsystem. Most programs degrade gracefully to `poll` when `epoll` is absent; those that don't are in the Linux binary compat tier anyway.

`poll` is an audit-trigger surface: the wakeup race between a thread parking on multiple fds simultaneously is a classic source of missed wakeups and spurious returns. `specs/poll.tla` covers it; spec is mandated before merge.

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

### 23.8 Phase 5 POSIX priority order

```
Must have (Utopia does not work without these — Phase 5 exit gate):
  musl port
  uutils-coreutils
  rc + bash
  poll / select
  futex
  /proc synthetic server (kernel + Linux-compat userspace)
  /dev basics (null, zero, random, tty)
  termios on /dev/cons
  PTY server (/dev/ptmx, /dev/pts/)
  Signal translation (SIGINT, SIGTERM, SIGCHLD, SIGHUP, SIGPIPE, SIGWINCH, SIGUSR1, SIGUSR2)
  TPIDR_EL0 save/restore (TLS)
  /tmp, /run as tmpfs
  /etc minimum files on Stratum
  Dynamic linker (musl ld.so)
  Job control (SIGTSTP, SIGCONT, tcsetpgrp)
  Plan 9 userland: rc, mk, awk, troff, tbl, eqn, 9 launcher

Should have before Phase 8 (Halcyon):
  PTY-related niceties (winch propagation across nested ttys)
  More complete bash subset
  More uutils coverage if any commands are buggy

Defer to Phase 6 (Linux binary compat):
  /sys stub
  setuid/setgid mechanics (deferred-not-implemented per §15.4)
  Extended attributes (xattr) at the namespace level
  POSIX ACLs

Defer to v1.1+:
  epoll
  inotify (most programs degrade gracefully)
  io_uring
```

### 23.9 The Utopia bring-up integration test

At Phase 5 exit, the integration test for Utopia (run in CI):
1. Boot a fresh Thylacine VM.
2. SSH in (or attach via UART).
3. Run a script that exercises every major surface:
   - Compile a "hello world" with gcc/clang.
   - Run a multi-stage shell pipeline.
   - Start tmux; create panes; run different commands.
   - Open vim; edit a file; save.
   - Run `top`; verify it shows correct stats.
   - Run `git clone https://...`; verify SSL works (curl-as-fetcher).
   - Use `ssh` to connect to a localhost service.
   - `ps -ef | grep stratum` shows the daemon running.
4. Assert no kernel panics, no driver crashes, no zombie processes.

If this passes, Utopia ships at Phase 5 exit.

### 23.10 Open design questions

None at Gate 3.

### 23.11 Summary

POSIX as 9P. Utopia at Phase 5 exit: rc + bash + uutils-coreutils + Plan 9 userland + musl + futex + poll + pty + signals + tmpfs. Test: "feels real, not broken."

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

Runtime detection at boot via `ID_AA64ISAR0_EL1.Atomic`. The kernel's atomic primitive header (`include/atomic.h`) does runtime patching: on first call, branches to either LSE or LL/SC implementation based on detected support.

```c
/* arch/arm64/atomic.S */
ENTRY(atomic_add)
    HWCAP_ALTERNATIVE  // patched at boot to one of:
    /* LSE path */
    ldaddal x1, x0, [x2]
    ret
    /* LL/SC fallback */
1:  ldaxr x0, [x2]
    add   x3, x0, x1
    stlxr w4, x3, [x2]
    cbnz  w4, 1b
    ret
ENDPROC
```

Hardware patching at boot (`apply_alternatives()`).

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
| 2 | `specs/namespace.tla` | 2 | bind/mount semantics, cycle-freedom, isolation between processes |
| 3 | `specs/handles.tla` | 2 | Rights monotonicity, transfer-via-9P invariant, hardware-handle non-transferability |
| 4 | `specs/vmo.tla` | 3 | Refcount + mapping lifecycle, no-use-after-free |
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
- `IPIRecv` ↔ `arch/arm64/ipi.c:ipi_handle()` lines 78-103
- ... etc

### 25.4 Audit-trigger surfaces

Every change to a file or function listed below spawns an adversarial soundness audit before merge. Updated at every ARCH change.

| Surface | Files | Why |
|---|---|---|
| Exception entry | `arch/arm64/start.S`, `arch/arm64/exception.c`, `arch/arm64/vectors.S` | Every syscall, IRQ, fault path |
| Page fault | `arch/arm64/fault.c`, `mm/vm.c` | Lifetime, demand-page, COW, W^X |
| Allocator | `mm/buddy.c`, `mm/slub.c`, `mm/magazines.c` | Allocation correctness, lock-free invariants |
| Scheduler | `kernel/sched.c`, `arch/arm64/context.c`, `arch/arm64/ipi.c` | EEVDF correctness, SMP, wakeup atomicity |
| Namespace | `kernel/namespace.c` | Cycle-freedom, isolation |
| Handle table | `kernel/handle.c` | Rights monotonicity, transfer rules, hardware-handle non-transferability |
| VMO | `kernel/vmo.c`, `mm/vmo_pages.c` | Refcount, mapping lifecycle |
| 9P client | `kernel/9p_client.c`, `kernel/9p_session.c` | Tag uniqueness, fid lifecycle, pipelining |
| Notes / signals | `kernel/notes.c`, `compat/signals.c` | Delivery ordering, async safety |
| Capability checks | All syscall entry points | Privilege correctness |
| KASLR / ASLR | `arch/arm64/start.S`, `kernel/aslr.c` | Entropy quality, layout correctness |
| Crypto code | None in v1.0 kernel; janus in userspace | Side-channel, key handling |
| ELF loader | `kernel/elf.c` | RWX rejection, relocation correctness |
| `mprotect` / `mmap` | `mm/vm.c` syscall handlers | W^X enforcement |
| Initial bringup | `kernel/main.c`, `init/init.c` | Boot ordering correctness |

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
| §9 Namespace and device model | COMMITTED | 1 |
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
| I-1 | Namespace operations in process A don't affect process B | Kernel namespace isolation | `namespace.tla` |
| I-2 | Capability set monotonically reduces (`rfork` only reduces) | Syscall gate | `handles.tla` |
| I-3 | Mount points form a DAG, never a cycle | Kernel mount validation | `namespace.tla` |
| I-4 | Handles transfer between processes only via 9P sessions | Syscall surface (no direct-transfer syscall exists) | `handles.tla` |
| I-5 | `KObj_MMIO`, `KObj_IRQ`, `KObj_DMA` cannot be transferred | Transfer syscall has no code path; static_assert | `handles.tla` |
| I-6 | Handle rights monotonically reduce on transfer | Syscall-level check | `handles.tla` |
| I-7 | VMO pages live until last handle closed AND last mapping unmapped | Refcount; runtime check | `vmo.tla` |
| I-8 | Every runnable thread eventually runs | EEVDF deadline computation | `scheduler.tla` |
| I-9 | No wakeup is lost between wait-condition check and sleep | Wait/wake protocol | `scheduler.tla`, `poll.tla`, `futex.tla` |
| I-10 | Per-9P-session tag uniqueness | Per-session tag pool with monotonic generation | `9p_client.tla` |
| I-11 | Per-9P-session fid identity is stable for fid's open lifetime | Per-session fid table | `9p_client.tla` |
| I-12 | W^X: every page is writable XOR executable | PTE bit check + mprotect rejection + ELF loader rejection | runtime + `_Static_assert` |
| I-13 | Kernel-userspace isolation: TTBR0 / TTBR1 split | Page table setup | runtime |
| I-14 | Storage integrity: every block from Stratum is integrity-verified | Stratum's responsibility (Merkle layer); OS observes via 9P | (Stratum-side spec) |
| I-15 | Hardware view derives entirely from DTB | No compile-time hardware constants outside `arch/arm64/<platform>/` | code review + audit |
| I-16 | KASLR randomizes kernel image base at boot | Boot init randomizes TTBR1 base | runtime + `/ctl/kernel/base` audit |
| I-17 | EEVDF latency bound: delay between runnable and running ≤ slice_size × N | EEVDF deadline math | `scheduler.tla` |
| I-18 | IPIs from CPU A to CPU B are processed in send order | GIC SGI ordering | `scheduler.tla` |
| I-19 | Note delivery preserves causal order within a process | Note queue per Proc | `notes.tla` |
| I-20 | PTY master ↔ slave atomicity | PTY data path locked | `pty.tla` |

These are the project's promises. Every one has a spec or a runtime check or a compile-time assertion. None are policy-only.

---

## 29. Glossary

| Term | Definition |
|---|---|
| 9P | Plan 9 protocol — universal file-server protocol; Thylacine speaks 9P2000.L |
| ASLR | Address Space Layout Randomization (userspace) |
| BTI | Branch Target Identification (ARMv8.5+ hardware feature) |
| Chan | Kernel object representing an open resource |
| CFI | Control Flow Integrity (compiler-level mitigation) |
| `Dev` | Kernel device vtable |
| DTB | Device Tree Blob — describes hardware to the kernel |
| EEVDF | Earliest Eligible Virtual Deadline First scheduler |
| EBR | Epoch-Based Reclamation (used in `vmo.tla`) |
| EL0/EL1/EL2/EL3 | ARM64 Exception Levels |
| `errstr` | Plan 9's per-thread error string |
| GIC | Generic Interrupt Controller (ARM) |
| Halcyon | Thylacine's graphical scroll-buffer shell |
| Handle | Typed unforgeable token referencing a kernel object |
| IPI | Inter-Processor Interrupt |
| janus | Stratum's key agent (also runs on Thylacine) |
| KASLR | Kernel ASLR |
| KObj | Kernel Object (e.g., `KObj_MMIO`, `KObj_VMO`) |
| LSE | Large System Extensions (ARMv8.1+ atomic instructions) |
| MTE | Memory Tagging Extension (ARMv8.5+) |
| Namespace | Per-process tree of mount points |
| Note | Plan 9's asynchronous text message (signal-equivalent) |
| PAC | Pointer Authentication Code (ARMv8.3+) |
| `Pgrp` | Process Group / namespace |
| Proc | Process |
| `rfork` | Plan 9 process/thread creation primitive |
| SLUB | Linux's modern slab allocator (replaces classical slab) |
| Stratum | Thylacine's native filesystem (independent project; feature-complete) |
| TLA+ | Temporal Logic of Actions, Lamport's specification language |
| Utopia | The Phase 5 milestone — textual POSIX environment that "feels real" |
| VMO | Virtual Memory Object (kernel object for shared memory regions) |
| W^X | Write XOR Execute (page protection invariant) |

---

## 30. Revision history

| Date | Change | Reason |
|---|---|---|
| 2026-05-04 | Initial draft (Phase 0). | Ground-up rewrite from `tlcprimer/ARCHITECTURE.md` after Phase 0 challenge round. STUBs/DRAFTs promoted to COMMITTED. C21-C27 commitments added (LSE, MTE, BTI, CFI, KASLR, ASLR, W^X, hardened malloc). Userspace drivers from Phase 3 (no in-kernel virtio-blk shortcut). Typed handle subordination. EEVDF scheduler. Three v2.0 design contracts (factotum capability mediation §15.4, multikernel §20.6, in-kernel Stratum driver §14.4). Nine TLA+ specs gate-tied. Audit-trigger surface table. Utopia milestone in §23. Twenty load-bearing invariants enumerated in §28. |
| 2026-05-04 | Halcyon-as-last-phase reorder. | User direction: practical working OS comes first; Halcyon at the end. ROADMAP phases 6/7/8 reordered: Phase 6 → Linux compat + network (was 7); Phase 7 → Hardening + v1.0-rc (was 8); Phase 8 → Halcyon + v1.0 final (was 6). ARCH §17 layer-cake updated. ARCH §8.1 SMP stress moved from Phase 8 to Phase 7 (the v1.0-rc release point). ARCH §16.5 container runner moved from Phase 7 to Phase 6. ARCH §23 POSIX surfaces references updated (Linux compat at Phase 6, Halcyon at Phase 8). |
