# Thylacine OS — Architecture

**Status**: Phase 0. Committed decisions are signed off. Stubs claim the space; content
comes in dedicated design sessions with the implementation team.

## Section status legend

- **STUB** — structure only, open questions listed, no decisions yet.
- **DRAFT** — first-pass content, under discussion.
- **COMMITTED** — signed off. Changes require written rationale and explicit re-opening.

---

## 1. How to read this document

**STATUS**: COMMITTED

`ARCHITECTURE.md` translates the properties and philosophy in `VISION.md` into concrete
decisions. Every implementation document references this file. Decisions marked COMMITTED
are the contract between design and implementation — they do not change without reopening.

### 1.1 Document relationships

- `VISION.md` — what we're building and why.
- `ARCHITECTURE.md` (this document) — how we're building it.
- `ROADMAP.md` — in what order.

### 1.2 Change management

Once a section is COMMITTED:
- Changing it requires a written rationale in the PR description.
- The section is explicitly re-opened (status → DRAFT) before changes are made.
- The corresponding `ROADMAP.md` phases are revisited for impact.

---

## 2. System overview

**STATUS**: COMMITTED

### 2.1 The stack

```
  ┌─────────────────────────────────────────────────────────┐
  │  Halcyon (graphical shell, Rust)                        │
  ├─────────────────────────────────────────────────────────┤
  │  Userspace services (9P servers, Rust)                  │
  │    janus (key agent) │ drivers │ video │ network stack  │
  │    ← hardware via typed handles (MMIO / IRQ / DMA)    → │
  │    ← buffer sharing via VMO handles (zero-copy)       → │
  ├─────────────────────────────────────────────────────────┤
  │  Compatibility layer (POSIX/Linux syscall translation)  │
  ├─────────────────────────────────────────────────────────┤
  │  Thylacine kernel (C99, ARM64)                          │
  │    Namespace │ Scheduler │ VM │ 9P client (pipelined)   │
  │    Handle table │ VMO manager │ IRQ forwarding          │
  ├─────────────────────────────────────────────────────────┤
  │  Stratum (native filesystem, 9P server)                 │
  └─────────────────────────────────────────────────────────┘
```

### 2.2 Key abstractions

- **Process**: unit of isolation. Has a private namespace, address space, and credential set.
- **Namespace**: a process's view of the resource tree. Composed via `bind` and `mount`.
- **Chan**: the kernel object representing an open resource (file, device, synthetic node).
  Carries qid, dev pointer, offset, mode. The fundamental currency of the kernel.
- **Dev**: the vtable every kernel device implements:
  `attach`, `walk`, `stat`, `open`, `create`, `read`, `write`, `close`, `remove`, `wstat`.
- **9P server**: any userspace process exposing a resource tree via the 9P protocol. Drivers,
  services, and the filesystem are all 9P servers. Mounted into a process namespace via the
  kernel's 9P client.

### 2.3 Invariants each abstraction maintains

| Abstraction | Invariant | Enforcement |
|---|---|---|
| Process | Namespace operations do not affect other processes' namespaces | Kernel namespace isolation |
| Chan | `chan->dev` is valid for the chan's lifetime | Ref-counted dev attachment |
| Namespace mount | Mount points form a DAG, not a cycle | Kernel mount validation |
| Credential | Capabilities can only be reduced, never elevated, without kernel mediation | Syscall gate |
| 9P session | Fid table is per-connection; fids do not leak between connections | Per-session fid table |

---

## 3. Language and toolchain

**STATUS**: COMMITTED

| Layer | Language | Rationale |
|---|---|---|
| Kernel | C99 | Total control, no hidden runtime, all prior kernel art is C. Matches Stratum's heritage. |
| Halcyon shell | Rust | Complex behavior, rich type system pays off, no kernel constraints |
| Userspace services | Rust | Same as Halcyon; drivers-as-processes benefit from Rust's safety |
| TUI tooling | Rust | Already established by Stratum's TUI (Ratatui) |
| Build system | CMake (kernel), Cargo (Rust) | Matches Stratum's toolchain |

**Rationale for split**: The kernel is the wrong place for Rust's runtime model. `no_std`,
no unwinding, no global allocator — the friction is real and the kernel is small enough that
disciplined C99 is manageable. Rust is used exactly where it delivers: complex userspace
logic where the borrow checker catches real bugs.

**No C++ anywhere.** No exceptions, no vtables, no hidden allocations.

---

## 4. Target architecture

**STATUS**: COMMITTED

### 4.1 Primary target: ARM64 (AArch64)

- EL0: userspace. EL1: kernel. EL2: hypervisor (available, not used at v1.0).
- ARM64 exception model used directly: `SVC #0` for syscall entry, exception vectors
  for interrupts and faults.
- Pointer Authentication (PAC) enabled for kernel stacks where hardware supports it.
- No x86-64 support at v1.0. Architecture-specific code lives in `arch/arm64/`.

### 4.2 Development machine: QEMU `virt`, ARM64

QEMU `virt` machine under Hypervisor.framework on Apple Silicon. No instruction translation.

**QEMU `virt` device inventory** (what the kernel must support at v1.0):

| Device | Bus | Use |
|---|---|---|
| VirtIO block | VirtIO-MMIO / VirtIO-PCI | Storage |
| VirtIO net | VirtIO-MMIO / VirtIO-PCI | Network |
| VirtIO GPU (virgl) | VirtIO-PCI | Framebuffer + 3D for Halcyon |
| VirtIO input | VirtIO-MMIO | Keyboard, mouse |
| GIC v2/v3 | ARM system | Interrupt controller |
| PL011 | ARM system | UART (early boot, debug) |
| PSCI | ARM system | CPU on/off, reset, shutdown |

### 4.3 Bare metal Apple Silicon (post-v1.0)

Deferred. Depends on Asahi Linux's m1n1 bootloader, AIC interrupt controller support,
and AGX GPU driver. Not a v1.0 commitment. When pursued, `arch/arm64/apple/` is the home.

---

## 5. Boot sequence

**STATUS**: DRAFT

### 5.1 Boot chain

```
QEMU firmware (EDK2 / U-Boot)
  → kernel ELF loaded at EL1
  → arch/arm64/start.S: MMU off, identity map, jump to C
  → boot_main(): BSS clear, page allocator bootstrap, MMU on
  → memory_init(): full VM map
  → sched_init(), dev_init(), proc_init()
  → mount root 9P server (Stratum or ramfs)
  → exec("/boot/init")
```

### 5.2 Open questions

- **Bootloader**: U-Boot or EDK2 UEFI? UEFI is more portable; U-Boot is simpler for
  QEMU-only targets. Decision pending.
- **Device tree**: QEMU passes a DTB. Kernel parses it for memory regions and device
  addresses. Extent of DTB parsing needed at v1.0 TBD.
- **Initramfs**: needed before Stratum is mountable. Contents: `init`, `janus`,
  minimal Stratum binary. Format TBD (cpio or custom).

---

## 6. Memory management

**STATUS**: STUB

### 6.1 Goals

- Virtual memory for all processes (no real-mode or flat-model userspace).
- Demand paging with copy-on-write for `fork`-equivalent.
- Kernel heap allocator for kernel objects (slab-style).
- Physical frame allocator (buddy allocator).

### 6.2 ARM64 memory layout

**Decision**: 48-bit VA, 4 KiB pages, 4-level page tables.

```
0x0000_0000_0000_0000 – 0x0000_FFFF_FFFF_FFFF   User (TTBR0)
0xFFFF_0000_0000_0000 – 0xFFFF_FFFF_FFFF_FFFF   Kernel (TTBR1)
```

### 6.3 Open questions

- Physical allocator: buddy system (standard, well-understood) or something smaller?
- Slab allocator: custom or import from a known-good implementation?
- Swap: not at v1.0; development machines have enough RAM.
- Large pages (2 MiB blocks): detect and use for kernel mappings?

---

## 7. Process and thread model

**STATUS**: DRAFT

### 7.1 Process

A process owns:
- An address space (page table root).
- A namespace (mount table + per-bind overlays).
- A file descriptor table (open Chans).
- A credential set (uid, gid, capabilities).
- A set of threads.

Processes are created by `rfork` (Plan 9-style) with flags controlling which resources
are shared vs. cloned. This subsumes both `fork` and `clone` — the POSIX compat layer
translates `fork()`/`clone()` into `rfork()` calls.

`rfork` flags:
- `RFPROC` — new process (required).
- `RFMEM` — share address space (thread-like).
- `RFNAMEG` — share namespace (default for threads; clone for containers).
- `RFFDG` — share file descriptor table.
- `RFCREDG` — share credentials.

### 7.2 Thread

A thread owns:
- A register context (GPRs, PC, SP, PSTATE).
- A kernel stack.
- A signal mask.
- A per-thread `errstr` buffer (Plan 9-style error string).

Multiple threads share a process's address space, namespace, and fd table (unless
created with explicit `rfork` flags separating them).

### 7.3 Open questions

- **TID/PID distinction**: Plan 9 has no threads in the C sense; 9Front adds them.
  Thylacine uses threads natively. Mapping to POSIX `pthread_t` / `pid_t` in the compat
  layer needs design.
- **Thread-local storage**: ARM64 `TPIDR_EL0` register. Layout TBD.
- **Signal model**: Plan 9 uses notes (async text messages to a process). POSIX uses
  signals. The compat layer must translate. Internal model: notes. Signals are a compat
  surface.

---

## 8. Scheduler

**STATUS**: STUB

### 8.1 Goals

- Preemptive, tickless where possible.
- SMP-aware from the start (QEMU `virt` supports multiple vCPUs).
- Simple enough to reason about and audit.
- Priority-based with time slicing.

### 8.2 Committed decisions

- **Run queue per CPU.** Work stealing for load balancing.
- **Preemption at EL0→EL1 boundary** (syscall/interrupt entry). Kernel preemption deferred
  to v1.1.
- **No real-time guarantees at v1.0.** SCHED_OTHER equivalent only.

### 8.3 Open questions

- Scheduling algorithm: CFS-equivalent (virtual runtime based) or simpler
  multi-level feedback queue?
- Timer: ARM generic timer (`CNTV_CTL_EL0`). Tick rate TBD (likely 1000 Hz or tickless).
- vCPU count at v1.0: target 1–8 CPUs. Maximum TBD.

---

## 9. Namespace and device model

**STATUS**: COMMITTED

### 9.1 The namespace

Each process has a private namespace — a tree of mount points mapping names to
resource servers. The namespace is the fundamental unit of isolation and composition.

**Operations**:
- `bind(old, new, flags)` — attach a file or directory at another point in the tree.
  Flags: `MREPL` (replace), `MBEFORE` (union, checked first), `MAFTER` (union, checked last).
- `mount(fd, afd, old, flags, aname)` — attach a 9P server (reached via `fd`) at `old`.
- `unmount(name, old)` — remove a mount point.

These three operations, composed, express containers, overlay filesystems, per-process
views, chroot, and capability restriction — without additional kernel machinery.

### 9.2 The Dev vtable

Every kernel device implements:

```c
struct Dev {
    int   dc;           /* device character ('c' for cons, 'e' for ether, ...) */
    char *name;

    void   (*reset)(void);
    void   (*init)(void);
    void   (*shutdown)(void);
    Chan*  (*attach)(char *spec);
    Walkqid* (*walk)(Chan *c, Chan *nc, char **name, int nname);
    int    (*stat)(Chan *c, uint8_t *dp, int n);
    Chan*  (*open)(Chan *c, int omode);
    void   (*create)(Chan *c, char *name, int omode, uint32_t perm);
    void   (*close)(Chan *c);
    long   (*read)(Chan *c, void *buf, long n, int64_t off);
    Block* (*bread)(Chan *c, long n, int64_t off);
    long   (*write)(Chan *c, void *buf, long n, int64_t off);
    long   (*bwrite)(Chan *c, Block *bp, int64_t off);
    void   (*remove)(Chan *c);
    int    (*wstat)(Chan *c, uint8_t *dp, int n);
    Chan*  (*power)(Chan *c, int on);
};
```

All kernel devices — including synthetic ones like `/dev/cons`, `/dev/null`, `/proc` —
implement this interface. Userspace devices implement it remotely via 9P.

### 9.3 Userspace drivers as 9P servers

**Preferred driver model**: a userspace process owns device resources and exposes them
as a 9P server. The kernel mounts the server and routes Dev vtable calls to it via the
9P client.

Benefits:
- Driver crash does not crash the kernel.
- Drivers can be updated without reboot (unmount, restart, remount).
- Driver code is auditable without kernel build access.
- Security boundary between driver and kernel is the 9P session.

Kernel drivers (in-process) are used only where the performance cost of the 9P round-trip
is unacceptable (e.g., interrupt dispatch, core memory management). All other drivers
live in userspace.

### 9.4 Device namespace layout (v1.0 target)

```
/
├── dev/
│   ├── cons          ← console (read: input, write: output)
│   ├── consctl       ← console mode control
│   ├── null          ← /dev/null
│   ├── zero          ← /dev/zero
│   ├── random        ← CSPRNG (ARM64 RNDR instruction)
│   ├── mem           ← physical memory (privileged)
│   ├── fb/           ← framebuffer server (9P, userspace driver)
│   │   ├── ctl
│   │   └── image
│   └── video/        ← video player server (9P, userspace driver)
│       ├── ctl
│       ├── frame
│       └── position
├── proc/             ← process namespace (synthetic, kernel Dev)
│   └── <pid>/
│       ├── ctl
│       ├── mem
│       ├── ns
│       ├── status
│       └── fd/
├── net/              ← network stack (9P, userspace driver)
│   ├── ipifc/
│   ├── tcp/
│   └── udp/
└── ctl/              ← kernel admin (synthetic, kernel Dev)
    ├── procs
    ├── memory
    └── devices
```

---

## 10. IPC

**STATUS**: COMMITTED

### 10.1 9P as the universal IPC

**There is no separate IPC mechanism.** All inter-process communication is mediated by
9P: one process mounts another's 9P server and reads/writes files. Pipes are 9P streams.
Shared memory is a 9P file backed by anonymous memory. Message queues are 9P files.

This is the Plan 9 model, adopted unchanged.

### 10.2 Pipes

Pipes are kernel-implemented 9P streams:
- `pipe(fd[2])` creates a pair of Chans backed by a kernel ring buffer.
- Reads block when empty; writes block when full.
- EOF on write end causes EOF on read end.

Pipes are the primary mechanism for connecting processes. Shell pipelines compose
processes via pipes, exactly as in Unix.

### 10.3 Notes (signals)

Plan 9's note mechanism is the internal signal model:
- A note is an async text message delivered to a process.
- The process can register a note handler or take the default action (kill).
- Standard notes: `interrupt`, `kill`, `alarm`, `hangup`.

The POSIX compat layer translates POSIX signals to notes and vice versa.

---

## 11. Syscall interface

**STATUS**: DRAFT

### 11.1 Design

The syscall interface is Plan 9-heritage, not POSIX. The syscall table is small by design.

**Core syscalls** (committed):

| Syscall | Description |
|---|---|
| `open(name, mode)` | Open a file in the namespace |
| `close(fd)` | Close a file descriptor |
| `read(fd, buf, n)` | Read from fd |
| `write(fd, buf, n)` | Write to fd |
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
| `mmap(addr, len, prot, flags, fd, off)` | Map memory (compat affordance) |
| `munmap(addr, len)` | Unmap memory |
| `pipe(fd[2])` | Create pipe |
| `dup(oldfd, newfd)` | Duplicate fd |
| `noted(v)` | Note handler return |
| `notify(fn)` | Register note handler |
| `postnote(pid, msg)` | Post a note to a process |
| `nsec()` | Nanosecond clock |
| `getpid()` | Process ID |

### 11.2 POSIX compat surface

POSIX syscalls that do not map cleanly to the above are implemented in the **compat
layer** — a userspace library or thin kernel shim that translates POSIX calls into
Thylacine primitives. The kernel is not required to implement POSIX directly.

**Compat layer strategy:**

1. **musl port**: musl libc is ported to the Thylacine syscall interface. Programs
   compiled against musl run natively. This is the primary compat path.
2. **Linux ARM64 binary compat**: a thin kernel shim intercepts `SVC #0` with Linux
   syscall numbers and routes them to Thylacine equivalents. Supports static binaries
   and musl-linked binaries. glibc-dynamic binaries: best-effort.

### 11.3 Open questions

- `epoll` / `kqueue` equivalent: Thylacine needs a multiplexed I/O wait mechanism.
  Plan 9 used `select`-on-files semantics. A `poll`-compatible surface is needed for
  the compat layer.
- Async I/O: the compat layer needs `io_uring` translation surface for Linux binaries
  that use it. Deferred to post-v1.0.

---

## 12. Interrupt and exception handling

**STATUS**: STUB

### 12.1 ARM64 exception model

ARM64 has four exception levels. Thylacine uses:
- **EL0**: userspace.
- **EL1**: kernel.
- EL2/EL3: not used at v1.0.

Exception types handled:
- **Synchronous**: syscall (SVC), page fault (Data Abort, Instruction Abort), illegal
  instruction, alignment fault.
- **IRQ**: hardware interrupts, routed via GIC.
- **FIQ**: fast interrupts (used for secure world only; not used at v1.0).
- **SError**: system errors (async, hardware-specific).

### 12.2 Interrupt controller

**QEMU `virt` target**: GIC v2 or v3 (QEMU selects based on configuration).

The kernel implements:
- GIC distributor init: enable, priority mask, routing.
- GIC CPU interface: acknowledge, EOI.
- IRQ → handler dispatch table.
- VirtIO IRQ routing.

### 12.3 Open questions

- GIC v2 vs v3: commit to one or support both?
- Timer IRQ: ARM generic timer, banked per CPU. IRQ number from DTB.
- IRQ threading model: handle in interrupt context or defer to a kernel thread?

---

## 13. Memory-mapped I/O and VirtIO

**STATUS**: DRAFT

### 13.1 VirtIO

VirtIO is the primary device interface for the QEMU `virt` target. All storage, network,
GPU, and input devices are VirtIO.

**VirtIO transport**: VirtIO-MMIO (primary for `virt`) and VirtIO-PCI (secondary; needed
for VirtIO-GPU which is PCI-only in QEMU).

**Committed**: a thin VirtIO core in the kernel providing:
- Virtqueue management (split virtqueue; packed virtqueue deferred).
- Descriptor chain allocation and completion.
- IRQ hookup.

Individual device drivers (block, net, GPU, input) are userspace 9P servers that use
the VirtIO core via a kernel-exported interface (`/dev/virtio/<n>/`).

Exception: **VirtIO block** may be in-kernel at v1.0 for performance during development.
This is revisited once the userspace driver model is proven.

### 13.2 Open questions

- PCIe enumeration: `virt` has a PCIe root complex for VirtIO-GPU. Minimal PCIe support
  needed. Extent TBD.
- DMA: ARM64 IOMMU (SMMU) not required for `virt`; deferred.
- VirtIO-net in userspace: feasible with tap/vhost interface from QEMU side?

---

## 14. Filesystem integration

**STATUS**: COMMITTED

### 14.1 Stratum as native filesystem

Stratum is the native filesystem. It runs as a userspace daemon, exposing a 9P server.
The kernel mounts it at `/` (or a specified mount point) via the standard `mount` syscall.

No in-kernel filesystem driver for Stratum at v1.0. The 9P client in the kernel is
sufficient. An in-kernel Stratum driver is a post-v1.0 optimization (mirroring Stratum's
own roadmap: FUSE first, kernel module later).

### 14.2 Early boot filesystem

Before Stratum is available, the kernel mounts a **ramfs** (simple in-memory filesystem,
kernel-native Dev implementation) to hold the initramfs contents:
- `stratum` daemon binary.
- `janus` key agent binary.
- `init` process binary.
- Minimal device configuration.

Once `init` starts Stratum and `janus`, the root namespace is remounted from ramfs
to Stratum.

### 14.3 Other filesystems

| Filesystem | Status | Notes |
|---|---|---|
| ramfs | In-kernel, v1.0 | Early boot only |
| tmpfs | In-kernel, v1.0 | Runtime temp storage |
| Stratum | Userspace 9P, v1.0 | Native persistent storage |
| 9P remote mount | Kernel 9P client, v1.0 | Mount host Stratum or any 9P server |
| procfs | In-kernel synthetic Dev, v1.0 | `/proc/<pid>/` subtree |
| FAT32 | Userspace 9P, post-v1.0 | USB and boot media compat |
| ext4 | Not planned | |

---

## 15. Security model

**STATUS**: DRAFT

### 15.1 Namespace isolation as the primary boundary

Each process's namespace is private. By default, a child process inherits its parent's
namespace at `rfork` time, but modifications to the child's namespace do not affect the
parent's. This is the primary isolation mechanism.

A "container" is simply a process whose namespace has been constructed to isolate it
from the broader system — a different root, a restricted `/dev`, a private network
server. No additional kernel mechanism is required.

### 15.2 Credentials

Each process has:
- **uid**: user identity (numeric).
- **gid**: primary group.
- **groups**: supplementary groups.

File access control: standard Unix DAC (owner/group/other + rwx bits). This matches
Plan 9's model and is sufficient for v1.0.

Capabilities: a minimal set of coarse-grained kernel capabilities (bind privileged ports,
load a kernel driver, access physical memory). Not the full Linux capability set.
Capabilities can only be reduced via `rfork`; elevation requires a privileged process.

### 15.3 Open questions

- **MAC (mandatory access control)**: not at v1.0. The namespace model provides much of
  what MAC provides in Linux without the policy complexity. Revisited post-v1.0.
- **Seccomp equivalent**: a process can restrict its own syscall surface. Implementation
  TBD; may be expressed as a namespace restriction.
- **setuid**: Plan 9 has no setuid. POSIX compat needs it. Mechanism TBD.

---

## 16. POSIX and Linux compatibility

**STATUS**: DRAFT

### 16.1 Compat strategy

Three tiers of compatibility:

| Tier | Target | Mechanism |
|---|---|---|
| **Native** | Programs compiled for Thylacine against musl | musl port to Thylacine syscalls |
| **Static binary compat** | Statically linked Linux ARM64 ELF binaries | Syscall translation shim |
| **Namespace compat** | Linux programs expecting `/proc`, `/sys`, `/dev/` layout | Synthetic 9P servers |

**Shell programs** work at Tier 1 almost immediately (they use pipes, files, and simple
syscalls). glibc-dynamically-linked binaries are Tier 2 with best-effort coverage.

### 16.2 musl port

musl libc is ported to emit Thylacine syscalls instead of Linux syscalls. This is the
primary path for running real software on Thylacine:
- Programs that build against musl on Linux can be recompiled for Thylacine.
- The musl port is a v1.0 deliverable.

### 16.3 Linux ARM64 binary compat

For pre-built Linux ARM64 ELF binaries:
- Kernel intercepts `SVC #0` with Linux syscall convention.
- Routing table maps Linux syscall numbers to Thylacine equivalents.
- `/proc`, `/sys`, `/dev` exposed via synthetic 9P servers that match Linux layout.

**Hard to support** (best-effort or post-v1.0):
- `epoll` (need a Thylacine equivalent first).
- `io_uring` (post-v1.0).
- `inotify` (post-v1.0).
- glibc dynamic linker paths.

### 16.4 Container-as-namespace

Linux container images (OCI format) can be run inside a Thylacine namespace:
- Mount the container's root filesystem (via Stratum or a 9P server) as the process root.
- Provide synthetic `/proc`, `/sys`, `/dev` servers matching the Linux layout.
- Run the container's init as a normal Thylacine process with a restricted namespace.

This is the "flatpak/Steam Deck" model from the vision: containers are namespaces,
not a separate subsystem. The kernel primitive (namespace) handles both.

---

## 17. Halcyon integration

**STATUS**: COMMITTED

### 17.1 Kernel surface Halcyon depends on

Halcyon requires from the kernel:
- **Framebuffer**: `/dev/fb/image` (raw pixel write) + `/dev/fb/ctl` (resolution, format).
- **Input**: `/dev/cons` for keyboard; `/dev/mouse` for pointer (if connected).
- **Processes**: `rfork`, `exec`, `pipe` for running commands.
- **9P mounts**: to mount video servers, image servers, etc.

Halcyon does not require a compositor, a display server, or any graphics API beyond
raw framebuffer write.

### 17.2 Framebuffer device

The framebuffer device is a userspace VirtIO-GPU driver exposing:

```
/dev/fb/
    ctl       ← write: "res <width> <height> <depth>", "flush"
    image     ← write: raw pixel data (ARGB32, row-major)
    info      ← read: current resolution and format
```

Halcyon writes pixels to `/dev/fb/image` and issues `flush` via `ctl`. The VirtIO-GPU
driver handles the DMA transfer to the host GPU.

For bare metal Apple Silicon (post-v1.0), the framebuffer device speaks to the AGX driver
(via Asahi) via the same 9P interface. Halcyon does not change.

---

## 18. Kernel object handles and capabilities

**STATUS**: COMMITTED

*Informed by: Zircon (Fuchsia), seL4.*

### 18.1 The problem with implicit privilege

The naive userspace driver model has a gap: how does the kernel know which process is
allowed to own which device? The answer cannot be "it is a privileged process" — that
recreates Unix's root problem. The answer must be explicit, unforgeable, typed grants.

**Subordination invariant**: handles are not a general-purpose IPC mechanism. The
only channel through which a handle may be transferred between processes is a 9P
session (as out-of-band metadata on a 9P message). No syscall exists for direct
handle transfer between processes. This invariant ensures 9P remains the sole
composition primitive in Thylacine. Any design that requires handle transfer outside
a 9P session is a signal that a 9P interface is missing, not that this invariant
should be relaxed.

### 18.2 Kernel objects and handles

Every resource the kernel manages is a **kernel object**. Kernel objects are accessed
exclusively via **handles** — unforgeable integer tokens scoped to a process. A process
cannot fabricate a handle; it can only receive one from the kernel or from another
process that holds a transferable handle.

**Kernel object types** (committed set):

| Type | Represents |
|---|---|
| `KObj_Process` | A process |
| `KObj_Thread` | A thread within a process |
| `KObj_VMO` | A virtual memory object (see §19) |
| `KObj_MMIO` | A mapped MMIO region at a specific physical address range |
| `KObj_IRQ` | The right to receive a specific interrupt |
| `KObj_DMA` | A DMA-capable physically contiguous buffer |
| `KObj_Chan` | An open 9P channel (wraps the existing Chan concept) |
| `KObj_Interrupt` | An eventfd-like fd that fires on IRQ delivery |

Handles carry **rights** — a bitmask of what the holder can do with the object:

| Right | Meaning |
|---|---|
| `RIGHT_READ` | Read the object's state |
| `RIGHT_WRITE` | Modify the object's state |
| `RIGHT_MAP` | Map the object into an address space (VMO, MMIO) |
| `RIGHT_TRANSFER` | Pass the handle to another process |
| `RIGHT_DMA` | Program DMA from this object |
| `RIGHT_SIGNAL` | Deliver/receive signals on this object |

Rights can only be reduced when transferring a handle, never elevated. A process
that holds `KObj_MMIO` with `RIGHT_MAP | RIGHT_WRITE` but not `RIGHT_TRANSFER`
can program the device but cannot grant that capability to any other process.

### 18.3 Driver startup flow

When the kernel starts a driver process for a device:

1. Kernel creates `KObj_MMIO` for the device's BAR/MMIO range.
2. Kernel creates `KObj_IRQ` for the device's interrupt line.
3. Kernel creates `KObj_DMA` for the device's CMA allocation.
4. Kernel passes these handles to the driver process at startup (via a dedicated
   startup channel, not inherited from a parent).
5. Driver maps the MMIO region into its address space via `mmap(KObj_MMIO handle)`.
6. Driver maps the DMA buffer similarly.
7. Driver registers for interrupt delivery by `read()`-blocking on the IRQ handle.

After step 4, the kernel has no further involvement in the hot path. The driver owns
the hardware directly via the mapped MMIO and DMA regions.

### 18.4 Regular processes

Regular processes receive no hardware handles. They hold only:
- `KObj_Chan` handles to open 9P connections (the normal file descriptor model).
- `KObj_VMO` handles for shared memory passed to them explicitly.
- `KObj_Process` and `KObj_Thread` handles for process management.

This is the hard boundary: regular programs cannot reach hardware regardless of what
they do. The capability is simply not present in their handle table.

### 18.5 Handle transfer over 9P

Handles of types `KObj_VMO` (with `RIGHT_TRANSFER`) can be passed between processes
over a 9P connection as out-of-band metadata on a message. This is the mechanism
for zero-copy buffer passing: a driver creates a VMO, fills it, and passes the VMO
handle to Halcyon (or any consumer) via the 9P session. The consumer maps the VMO
and reads directly. No copy, kernel not in the data path.

### 18.6 Syscall additions

New syscalls to support the handle model:

| Syscall | Description |
|---|---|
| `handle_close(h)` | Release a handle |
| `handle_rights(h)` | Query rights on a handle |
| `handle_reduce(h, rights)` | Return a new handle with reduced rights |
| `mmap_handle(h, addr, len, prot)` | Map a VMO or MMIO handle into address space |
| `irq_wait(h)` | Block until the IRQ handle fires; returns count |

---

## 19. Virtual memory objects (VMOs)

**STATUS**: COMMITTED

*Informed by: Zircon (Fuchsia).*

### 19.1 What a VMO is

A **Virtual Memory Object** is a kernel object representing a region of memory,
independent of any process's address space. It is the unit of memory sharing in
Thylacine.

A VMO has:
- A size (page-aligned).
- A backing type: anonymous (zero-filled on demand), physical (pinned physical pages
  for DMA), or file-backed (Stratum page cache, post-v1.0).
- A reference count. Pages are freed when the last handle is closed and all mappings
  are unmapped.

### 19.2 Why VMOs over anonymous mmap

Unix `mmap(MAP_ANONYMOUS | MAP_SHARED)` shared memory has implicit aliasing — two
processes share memory by convention (both knowing the fd number or the shm name),
not by explicit kernel-tracked grant. VMOs make sharing explicit:

- One process creates the VMO and holds the handle.
- It maps the VMO into its address space.
- It passes the VMO handle (with `RIGHT_MAP | RIGHT_READ`) to another process.
- The second process maps the VMO independently.

The kernel tracks both mappings. When the producing process unmaps, the pages remain
live until all handles are closed and all mappings are gone. No use-after-free at the
kernel level.

### 19.3 Primary use cases in Thylacine

**Zero-copy framebuffer**: The VirtIO-GPU driver (or rpi5-fb driver) creates a
physical VMO for the framebuffer. Halcyon receives the VMO handle and maps it
read-only. Halcyon writes pixels directly into the mapped region. The driver issues
`flush` when Halcyon signals readiness. No copy at any point in the path.

**Zero-copy video decode**: The video decoder driver creates a physical VMO per
decoded frame. It passes the VMO handle to the video player 9P server. The player
passes it to Halcyon. Halcyon blits from the mapped VMO into the framebuffer VMO.
One blit; no intermediate copies; no kernel involvement after handle transfer.

**DMA buffer lifecycle**: NVMe/VirtIO-blk driver creates a physical VMO for its
DMA descriptor rings and data buffers. The VMO handle is never transferred — the
driver holds it exclusively. The kernel's IOMMU mapping (where present) is derived
from the VMO's physical page list. Clean, auditable DMA ownership.

**Inter-driver shared memory**: Two cooperating driver processes (e.g. network driver
and network stack) share a ring buffer via a VMO. No kernel copy in the packet path.

### 19.4 VMO syscalls

| Syscall | Description |
|---|---|
| `vmo_create(size, flags)` | Create anonymous VMO, return handle |
| `vmo_create_physical(paddr, size)` | Create VMO over physical range (privileged) |
| `vmo_get_size(h)` | Query VMO size |
| `vmo_read(h, buf, offset, len)` | Read from VMO without mapping |
| `vmo_write(h, buf, offset, len)` | Write to VMO without mapping |

Mapping is done via `mmap_handle()` from §18.6. Unmapping via standard `munmap`.

---

## 20. Per-core discipline (SMP)

**STATUS**: DRAFT

*Informed by: Barrelfish (ETH Zurich / Microsoft Research).*

### 20.1 The shared-state SMP trap

The conventional multicore kernel approach — shared kernel data structures protected
by locks — has a fundamental cost that grows with core count: cache coherency traffic.
Every lock acquisition on a shared structure sends invalidation messages across the
interconnect. At 8+ cores this becomes the bottleneck, not the computation.

Thylacine does not go full multikernel (per-core kernel instances with no shared
state — that is a post-v1.0 research direction). But the *discipline* of treating
cross-core communication as explicit shapes the design correctly from the start.

### 20.2 Committed per-core discipline

**Per-CPU data is the default**: scheduler run queues, interrupt handler state, kernel
allocator slabs, and interrupt counters are per-CPU. No lock needed to access them
from the owning CPU. Cross-CPU access is the exception, not the rule.

**Cross-core communication is explicit**: when one CPU must affect another's state
(e.g. TLB shootdown, process migration, IPI-driven wakeup), the operation is an
explicit message — an IPI with a defined payload — not a lock on a shared structure.
The recipient CPU processes the message in its own interrupt handler.

**No global kernel lock**: there is no equivalent of the Linux BKL or a single global
spinlock protecting kernel state. Every shared structure that genuinely must be shared
has its own fine-grained lock with documented contention analysis.

**Interrupt affinity**: device interrupts are affine to a specific CPU (configured in
the GIC). The driver process that owns the device receives IRQ delivery on the CPU
its IRQ handle is bound to. This eliminates cross-CPU IRQ dispatch in the common
case.

### 20.3 Data structures with global state

Some structures genuinely require cross-CPU visibility:

| Structure | Sharing mechanism |
|---|---|
| Process table | RCU-style: readers lockless, writers take a narrow lock |
| VMO reference counts | Atomic operations (no lock) |
| Handle table (per-process) | Per-process lock, not global |
| IRQ routing table | Written at boot/driver-start only, read locklessly after |
| 9P session table | Per-session lock |

### 20.4 TLA+ specification requirement

The per-core scheduler and IPI protocol are audit-trigger surfaces. Before SMP is
enabled (likely Phase 2 exit criterion for single-CPU, Phase post-v1.0 for full SMP),
a TLA+ spec of the cross-CPU state machine — process migration, TLB shootdown
sequencing, wakeup races — is required. The spec precedes the implementation.

### 20.5 Open questions

- **Core count at v1.0**: 1–4 vCPUs in QEMU. Full SMP (8+) is post-v1.0.
- **NUMA**: single-socket only. No NUMA topology at v1.0.
- **CPU hotplug**: not at v1.0. CPUs are online at boot and stay online.

---

## 21. Async 9P — pipelined requests

**STATUS**: DRAFT

*Informed by: io_uring (Linux), Helios (CMU), 9P2000.L.*

### 21.1 The synchronous 9P bottleneck

The basic 9P model is synchronous: send a Tmessage, wait for the Rmessage, send the
next. Under high I/O load this serializes unnecessarily — the client is idle while
the server processes each request, and network/IPC round-trip latency dominates
throughput.

9P2000 already solves this at the protocol level: the `tag` field in every message is
a request identifier. The client can have multiple outstanding requests with different
tags simultaneously. The server responds to each independently and out of order.
This is **pipelining** and it is part of the core protocol spec, not an extension.

Stratum's server already supports this (io_uring-native write path). Thylacine's
client must exploit it.

### 21.2 Committed client design

The kernel's 9P client maintains a **request pipeline** per session:

- Maximum outstanding requests: configurable per session (default: 32).
- Each request is assigned a unique tag from a per-session tag pool.
- Requests are submitted to the session's send queue without waiting for prior
  requests to complete.
- Completions are matched to waiting kernel threads by tag.
- Out-of-order completions are handled correctly — tag N's completion wakes tag N's
  waiter regardless of submission order.

This means a process doing 32 concurrent file reads into a Stratum mount gets 32
requests in flight simultaneously. The throughput approaches the session's bandwidth
limit rather than being divided by round-trip latency.

### 21.3 Userspace 9P servers benefit automatically

Because the kernel's 9P client pipelines, every userspace 9P server — drivers, janus,
the video player — gets pipelined access from any client that issues concurrent
requests. No server-side changes needed. The server already handles multiple tags
(Plan 9's `lib9p` and Stratum both do this correctly).

### 21.4 Flow control

The pipeline is bounded to prevent unbounded queue growth:
- If the outstanding request count reaches the session maximum, new requests block
  until a slot is available.
- Per-session credit-based flow control (from 9P2000.L) is supported: the server
  advertises how many requests it can handle; the client respects this.

### 21.5 Halcyon and async I/O

Halcyon issues concurrent reads against multiple 9P servers simultaneously — reading
keyboard input, polling the video frame fd, reading command output from a pipe — via
the pipelining client. This eliminates the need for a dedicated async I/O syscall in
the common case. Multiple blocking reads in flight across different fds, each on a
different 9P tag, compose naturally.

### 21.6 Open questions

- **Tag space size**: 9P2000 uses a 16-bit tag (65535 slots, tag 0xFFFF reserved for
  Tversion). Per-session tag pool allocation strategy TBD.
- **Kernel thread per blocked request vs. async completion**: should a blocking
  `read()` syscall park the calling thread or use an internal async mechanism?
  Current decision: park the thread (simple, correct). Revisit if thread count becomes
  a bottleneck.

---

## 22. Hardware platform model

**STATUS**: COMMITTED

*This section formalizes the bare-metal-open / QEMU-primary principle discussed
in design sessions.*

### 22.1 Primary development target

**QEMU `virt`, ARM64.** All Phase 1–6 work targets this machine. No QEMU-specific
assumptions in the kernel above `arch/arm64/`.

### 22.2 DTB as the hardware abstraction

The kernel never hardcodes peripheral addresses, IRQ numbers, or memory regions.
All hardware discovery is via the Device Tree Blob passed by the bootloader (QEMU
generates this automatically; U-Boot passes it on real hardware).

This is a hard rule: no `#ifdef QEMU`, no hardcoded MMIO base addresses, no
compile-time peripheral configuration. The DTB parser is the single source of
hardware truth.

Compatible strings in the DTB drive driver selection:

```c
static const char *gic_compat[] = {
    "arm,gic-400",
    "arm,gic-v3",
    NULL
};
```

The same kernel binary boots QEMU `virt`, Pi 5, and RK3588 boards by reading
their respective DTBs.

### 22.3 Platform-specific layers

Architecture-specific code is isolated under `arch/arm64/<platform>/`:

```
arch/arm64/
  common/       ← shared: exception vectors, MMU, GIC, ARM generic timer
  qemu-virt/    ← QEMU-specific: empty if DTB-driven correctly
  rpi5/         ← Pi 5: EL2→EL1 drop, mailbox interface, RP1 init
  rk3588/       ← future
```

`arch/arm64/common/` is the investment. It runs unmodified on all platforms.
`arch/arm64/rpi5/` is thin — primarily the EL2→EL1 drop sequence and early
mailbox setup before the DTB is available.

### 22.4 First bare metal target: Raspberry Pi 5

Post-Phase 6. Pi 5 is chosen because:
- GIC-400: identical to QEMU `virt` — GIC driver transfers directly.
- PL011 UART: identical to QEMU `virt` — UART driver transfers directly.
- ARM generic timer: identical — timer driver transfers directly.
- VideoCore VII + mailbox: framebuffer via mailbox, same `/dev/fb/` 9P interface.
- RP1 datasheet published, Linux driver open source.
- Network boot via TFTP: fast iteration loop without SD card swapping.
- Pi 5 + M.2 NVMe HAT: Stratum on real NVMe hardware.

The delta from QEMU `virt` to Pi 5 bare metal is: EL2→EL1 drop, mailbox framebuffer
driver, RP1 Ethernet driver for network boot. Estimated: one focused sprint after
Phase 6 stabilizes.

### 22.5 RISC-V (long horizon)

Post-v1.0. Target: first RVA23-compliant SBC with PCIe and NVMe (Milk-V Jupiter 2
or equivalent). The kernel port from ARM64 to RISC-V is mechanical above `arch/`:
swap GIC for PLIC, ARM CSRs for RISC-V CSRs, ARM generic timer for SBI timer.
All namespace, scheduler, 9P, VMO, and handle code is architecture-independent and
transfers without modification.

---

---

## 23. POSIX surfaces

**STATUS**: DRAFT

### 23.1 Design principle: POSIX surfaces are 9P servers

Every POSIX surface in Thylacine is a 9P server that speaks a POSIX-shaped
interface. There is no separate compat kernel layer. The mechanism is always:
a 9P server mounts at a conventional path and serves the expected file tree.
Thylacine-native programs use the underlying 9P interface directly; POSIX programs
see what they expect; both are served by the same infrastructure.

This is the Plan 9 principle applied consistently: if it can be a file, it is a file.
If it can be a 9P server, it is a 9P server.

### 23.2 Utopia minimum viable POSIX (Phase 5 entry requirement)

The set of surfaces that must exist for Utopia to feel real rather than broken.

**Shell**:
- `rc` as the native shell. `bash` as the POSIX compat shell.
- Both require: `rfork`/`exec`/`wait`, `pipe()`/`dup2()`, file redirection,
  note→signal translation for `Ctrl-C`, `$PATH`-equivalent via union directories.
- Job control (`SIGTSTP`, `SIGCONT`) required at Phase 5; feels broken without it.

**Coreutils** — BusyBox compiled against musl:
```
ls, cat, echo, cp, mv, rm, mkdir, rmdir, chmod, chown
grep, sed, awk, find, sort, uniq, wc, head, tail, cut
ps, kill, env, pwd, which, tar, gzip
```

**`/proc` synthetic 9P server**:
```
/proc/<pid>/
    status      ← Linux-compat field names (Name, Pid, PPid, State, VmRSS)
    cmdline     ← null-separated argv
    fd/         ← symlinks to open file descriptions
    maps        ← virtual memory map (for ldd, gdb)
    mem         ← raw process memory (privileged, KObj_Process handle required)
/proc/self/     ← symlink to /proc/<current-pid>/
```

**`/dev` minimum**:
```
/dev/null, /dev/zero, /dev/random, /dev/urandom
/dev/tty        ← current controlling terminal
/dev/stdin      → /proc/self/fd/0
/dev/stdout     → /proc/self/fd/1
/dev/stderr     → /proc/self/fd/2
/dev/pts/       ← PTY slave ends (see §23.5)
```

**`/etc` minimum** (ordinary Stratum files):
```
/etc/passwd, /etc/group, /etc/hostname
/etc/resolv.conf (populated when network is up)
/etc/localtime
```

**`/tmp`, `/run`** — backed by tmpfs (in-kernel Dev). `/var/run` → symlink to `/run`.

### 23.3 `poll` / `select` / `epoll`

**Status**: must-have for Phase 5. Without `poll`, interactive bash, curl, Python
asyncio, and essentially all non-trivial programs are broken.

**`poll(fds, nfds, timeout)`**: park the calling thread with a wait list across N
fds. The first fd to become ready wakes the thread. For 9P-backed fds, the server
signals readiness via a synthetic notification on the 9P session. For pipes and
kernel fds, readiness is tracked in the kernel's Chan structures.

**`select()`**: implemented on top of `poll()`. Trivial.

**`epoll`**: Linux-specific scalable multiplexing. Deferred to Phase 7. Designed as
an extension of `poll` semantics, not a separate subsystem. Most programs degrade
gracefully to `poll` when `epoll` is absent; those that don't are in the Linux
binary compat tier anyway.

`poll` is an audit-trigger surface: the wakeup race between a thread parking on
multiple fds simultaneously is a classic source of missed wakeups and spurious
returns. A TLA+ spec of the poll wait/wake state machine is required before merge.

### 23.4 Threading — `pthread` and `futex`

**`pthread`**: musl's pthread implementation maps onto `rfork(RFPROC|RFMEM|RFFDG)`.
Thread-local storage uses ARM64 `TPIDR_EL0` — saved/restored on context switch.
This is a two-line addition to `arch/arm64/context.c` that is easy to forget and
breaks all of musl's TLS if missed.

**`futex`**: the load-bearing primitive under all of musl's mutexes, condition
variables, and semaphores. Without it, threading is broken under contention.

`futex` is implemented as a compatibility syscall that maps to Thylacine's internal
wait/wake mechanism. It is not a native Thylacine primitive — the native primitive
is `rendezvous()`. The musl port uses `futex`; Thylacine-native code uses
`rendezvous()`.

`futex` syscall semantics supported at Phase 5:
- `FUTEX_WAIT`: sleep if `*addr == val`
- `FUTEX_WAKE`: wake N waiters on `addr`
- `FUTEX_WAIT_BITSET` / `FUTEX_WAKE_BITSET`: needed by musl's condvar implementation

`futex` is an audit-trigger surface. The wait/wake atomicity invariant (no wakeup
lost between the value check and the sleep) is subtle and must be proven correct.
TLA+ spec required before merge.

### 23.5 Pseudo-terminals (PTY)

Required for: Halcyon subprocess hosting, `ssh`, `tmux`, `vim`, any program that
checks `isatty()` and changes behavior accordingly.

**Model**: a PTY is a 9P server managing a master/slave fd pair with `termios`
semantics layered on a pipe-like channel.

**`/dev/ptmx`**: opening this allocates a new PTY master fd and creates a
corresponding slave entry under `/dev/pts/<n>`.

**`/dev/pts/<n>`**: the slave end. Passed to the child process as its controlling
terminal. The slave presents a full `termios` interface.

**`termios` on `/dev/cons`**: the main console device implements `termios` via
writes to `/dev/consctl`. Mapping:
- `tcgetattr()`/`tcsetattr()` → structured read/write on `/dev/consctl`
- Raw mode (`TCSAFLUSH` + `~ICANON`) → `"rawon"` written to `/dev/consctl`
- Cooked mode → `"rawoff"`
- Echo control → `"echo"` / `"noecho"`

This covers `vim`, `less`, `bash` readline, `ssh` client.

### 23.6 Signal translation

Thylacine's internal signal model is Plan 9 notes — async text messages. POSIX
signals are a translation surface implemented in musl and a thin kernel shim.

| POSIX signal | Thylacine note | Notes |
|---|---|---|
| `SIGINT` | `"interrupt"` | Ctrl-C on cons → note |
| `SIGTERM` | `"kill"` | `postnote(pid, "kill")` |
| `SIGKILL` | `"kill"` (non-catchable) | Kernel enforces non-catchable |
| `SIGCHLD` | synthetic | Generated on child `exits()` |
| `SIGHUP` | `"hangup"` | Terminal close |
| `SIGPIPE` | synthetic | Write to closed pipe |
| `SIGALRM` | `"alarm"` | `alarm()` syscall → timer note |
| `SIGUSR1/2` | `"usr1"`, `"usr2"` | User-defined |
| `SIGSTOP` | `"stop"` | Job control; non-catchable |
| `SIGCONT` | `"cont"` | Job control resume |
| `SIGWINCH` | synthetic | Terminal resize → note |

`sigaction()`, `sigprocmask()`, `sigsuspend()`, `sigwaitinfo()` are implemented
in the musl port against note delivery. POSIX programs receive signals normally;
the note mechanism is invisible to them.

Hard cases requiring careful implementation:
- `SIGCHLD` with `SA_NOCLDWAIT` and `waitpid(WNOHANG|WUNTRACED|WCONTINUED)`
- Signal masks across `rfork()` — inherited vs. reset
- `SA_RESTART` for syscall restart after signal delivery

### 23.7 `/sys` (minimal stub)

Linux-specific. Needed for: dynamic linker path probing, `ldd`, some hardware
enumeration tools.

A minimal synthetic `/sys` 9P server sufficient for:
```
/sys/class/net/<iface>/    ← network interface info (for ip, ifconfig)
/sys/block/<dev>/          ← block device info (for lsblk)
/sys/devices/              ← minimal device tree (for udev-dependent programs)
```

Full `/sys` is not a goal. The stub satisfies the dynamic linker and basic admin
tools. Programs that parse `/sys` heavily are Linux-admin tools Thylacine does not
need to run.

Deferred to Phase 7 (Linux binary compat phase).

### 23.8 Phase 5 POSIX priority order

```
Must have (Utopia does not work without these):
  musl port
  BusyBox coreutils
  poll / select
  futex
  /proc synthetic server
  /dev basics (null, zero, random, tty)
  termios on /dev/cons
  PTY server (/dev/ptmx, /dev/pts/)
  Signal translation (SIGINT, SIGTERM, SIGCHLD, SIGHUP, SIGPIPE, SIGWINCH)
  TPIDR_EL0 save/restore (TLS)

Should have before Phase 6:
  /tmp, /run as tmpfs
  /etc minimum files on Stratum
  Dynamic linker (musl ld.so)
  bash port
  Job control (SIGTSTP, SIGCONT, tcsetpgrp)

Defer to Phase 7:
  epoll
  inotify (most programs degrade gracefully)
  /sys stub
  setuid/setgid mechanics
  Extended attributes (xattr)
  POSIX ACLs
```

---

## 24. Status summary

| Section | Status | Priority |
|---|---|---|
| §1 How to read | COMMITTED | — |
| §2 System overview | COMMITTED | — |
| §3 Language | COMMITTED | — |
| §4 Target architecture | COMMITTED | 1 |
| §5 Boot sequence | DRAFT | 2 |
| §6 Memory management | STUB | 3 |
| §7 Process and thread model | DRAFT | 4 |
| §8 Scheduler | STUB | 5 |
| §9 Namespace and device model | COMMITTED | 1 |
| §10 IPC | COMMITTED | 1 |
| §11 Syscall interface | DRAFT | 2 |
| §12 Interrupt handling | STUB | 3 |
| §13 VirtIO | DRAFT | 3 |
| §14 Filesystem integration | COMMITTED | 2 |
| §15 Security model | DRAFT | 4 |
| §16 POSIX compat | DRAFT | 5 |
| §17 Halcyon integration | COMMITTED | 2 |
| §18 Kernel object handles and capabilities | COMMITTED | 2 |
| §19 Virtual memory objects (VMOs) | COMMITTED | 3 |
| §20 Per-core discipline (SMP) | DRAFT | 5 |
| §21 Async 9P — pipelined requests | DRAFT | 3 |
| §22 Hardware platform model | COMMITTED | 1 |
| §23 POSIX surfaces | DRAFT | 3 |
